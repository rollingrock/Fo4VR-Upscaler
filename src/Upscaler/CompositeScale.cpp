#include "Upscaler/CompositeScale.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/CompositeScaleMath.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"

#include <Windows.h>
#include <d3d11.h>

#include <DirectXMath.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Upscaler::CompositeScale
{
	namespace
	{
		using namespace DirectX;

		// The eight rows, in the order and at the spacing the engine's own uploader writes
		// them: the far pair at buffer byte 0x140, the near pair at 0x180, contiguous.
		struct Rows
		{
			XMFLOAT4X4 farRows{};
			XMFLOAT4X4 nearRows{};
		};
		static_assert(sizeof(Rows) == 128);
		static_assert(Addr::kGlobalCameraNearReconstructionOffset ==
					  Addr::kGlobalCameraFarReconstructionOffset + sizeof(XMFLOAT4X4));

		// None of the sites runs on the thread ClearColor runs on, and the composite's own
		// members run on several, so everything below is behind one lock rather than owned
		// by the render thread. Driving the context from those threads is what the engine
		// does in the same calls.
		std::mutex g_lock;

		// Borrowed, never released: the engine and the runtime own these and outlive us.
		ID3D11Device* g_probedDevice = nullptr;
		ID3D11Buffer* g_probedBuffer = nullptr;
		bool g_probed = false;
		bool g_mapNoOverwriteSupported = false;
		bool g_featureLogged = false;
		bool g_rowsLogged = false;
		bool g_declineLogged = false;
		bool g_mapFailureLogged = false;
		// The rows the engine wrote, kept between the patch and the restore so the restore
		// puts back what was there rather than an approximation of it.
		Rows g_originalRows{};
		bool g_patched = false;
		// The first ten patched executes log the two live ratios the site read. Bounded by
		// a count rather than by time, so a ten-frame reading is exactly ten lines.
		std::uint32_t g_ratiosLogged = 0;
		constexpr std::uint32_t kRatioLogLimit = 10;

		std::atomic<bool> g_enabled{ false };
		std::atomic<bool> g_releaseRequested{ false };
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_applied{ 0 };
		std::atomic<std::uint64_t> g_declines{ 0 };
		std::atomic<std::uint64_t> g_restores{ 0 };
		std::atomic<std::uint32_t> g_lastDecline{ 0 };
		std::atomic<std::uint32_t> g_exceptionCode{ 0 };
		std::atomic<std::int32_t> g_lastHr{ 0 };
		std::atomic<float> g_lastRatioWidth{ 0.0f };
		std::atomic<float> g_lastRatioHeight{ 0.0f };
		std::atomic<std::uint32_t> g_bufferBytes{ 0 };
		std::atomic<bool> g_ready{ false };
		std::atomic<std::uint64_t> g_restoredAtDraw{ 0 };
		std::atomic<std::uint64_t> g_drawsExamined{ 0 };
		std::atomic<std::uint64_t> g_compositeDraws{ 0 };
		std::atomic<std::uint32_t> g_compositeShaders{ 0 };
		std::atomic<std::uint64_t> g_restoredAtWater{ 0 };
		std::atomic<std::uint64_t> g_restoredAtSeamExit{ 0 };

		void ReleaseLocked() noexcept
		{
			g_probedDevice = nullptr;
			g_probedBuffer = nullptr;
			g_probed = false;
			g_mapNoOverwriteSupported = false;
			g_featureLogged = false;
			g_rowsLogged = false;
			g_declineLogged = false;
			g_mapFailureLogged = false;
			g_ratiosLogged = 0;
			g_ready.store(false, std::memory_order_relaxed);
		}

		void RecordDecline(Decline a_reason) noexcept
		{
			g_declines.fetch_add(1, std::memory_order_relaxed);
			g_lastDecline.store(static_cast<std::uint32_t>(a_reason), std::memory_order_relaxed);
		}

		// The engine's global camera constant buffer, the object the composite reads as
		// cb12. Two guarded reads: the state pointer's own slot, then the buffer pointer at
		// the verified field offset.
		[[nodiscard]] ID3D11Buffer* EngineCameraBuffer() noexcept
		{
			const auto state = Engine::GraphicsState();
			if (state == 0) {
				return nullptr;
			}
			void* buffer = nullptr;
			if (!Engine::ReadPtr(state + Addr::kStateGlobalCameraConstantBuffer, buffer)) {
				return nullptr;
			}
			return static_cast<ID3D11Buffer*>(buffer);
		}

		// Eye 0's ViewData, which is what the uploader reads for rows 20-27. The composite
		// reads eye-0's rows for both eyes - a flat-derived pass VR never corrected - so
		// reproducing the uploader's operand exactly is what makes the restore put back
		// what the engine wrote rather than an approximation of it.
		[[nodiscard]] bool ReadEyeZeroMatrix(std::uintptr_t a_fieldOffset, XMFLOAT4X4& a_out) noexcept
		{
			const auto state = Engine::GraphicsState();
			if (state == 0) {
				return false;
			}
			void* viewData = nullptr;
			if (!Engine::ReadPtr(state + Addr::kStateCameraViewData, viewData) || viewData == nullptr) {
				return false;
			}
			return FaultGuard::SafeReadBytes(
				reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(viewData) + a_fieldOffset),
				&a_out,
				sizeof(a_out));
		}

		void LogRows(const char* a_what, const XMFLOAT4X4& a_rows) noexcept
		{
			for (int row = 0; row < 4; ++row) {
				logger::info(
					FMT_STRING("composite reconstruction scale: {} row {} = {:.6f} {:.6f} {:.6f} {:.6f}"),
					a_what,
					row,
					a_rows.m[row][0],
					a_rows.m[row][1],
					a_rows.m[row][2],
					a_rows.m[row][3]);
			}
		}

		// One probe per key flip, per device and per camera buffer. There is nothing to
		// create: the write goes into the engine's own buffer, so all this establishes is
		// whether the runtime will let a dynamic constant buffer be mapped without
		// discarding it, and how big that buffer is.
		[[nodiscard]] bool EnsureProbed(
			ID3D11Device* a_device,
			ID3D11Buffer* a_buffer,
			Decline& a_reason) noexcept
		{
			if (g_probed && (g_probedDevice != a_device || g_probedBuffer != a_buffer)) {
				ReleaseLocked();
			}
			if (!g_probed) {
				g_probed = true;
				g_probedDevice = a_device;
				g_probedBuffer = a_buffer;
				D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
				const HRESULT hr = a_device->CheckFeatureSupport(
					D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
				g_lastHr.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);
				g_mapNoOverwriteSupported =
					SUCCEEDED(hr) && options.MapNoOverwriteOnDynamicConstantBuffer != FALSE;
				D3D11_BUFFER_DESC desc{};
				a_buffer->GetDesc(&desc);
				g_bufferBytes.store(desc.ByteWidth, std::memory_order_relaxed);
				if (!g_featureLogged) {
					g_featureLogged = true;
					logger::info(
						FMT_STRING("composite reconstruction scale: global camera buffer is {} bytes, usage {}, map-no-overwrite on a dynamic constant buffer {}"),
						desc.ByteWidth,
						static_cast<std::uint32_t>(desc.Usage),
						g_mapNoOverwriteSupported ? "supported"sv : "not supported"sv);
				}
				g_ready.store(g_mapNoOverwriteSupported, std::memory_order_relaxed);
			}
			if (!g_mapNoOverwriteSupported) {
				a_reason = Decline::kPartialUpdate;
				return false;
			}
			return true;
		}

		// Keep this boundary free of destructible C++ locals: a driver SEH must not unwind
		// through a frame the engine's buffer state depends on, and an Unmap the unwind
		// skipped would leave the engine's next upload mapping an already-mapped buffer.
		struct WriteResult
		{
			std::uint32_t exceptionCode = 0;
			std::int32_t hr = 0;
			bool written = false;
		};

		[[nodiscard]] WriteResult WriteRowsIsolated(
			ID3D11DeviceContext* a_context,
			ID3D11Buffer* a_buffer,
			const Rows* a_rows) noexcept
		{
			WriteResult result{};
			__try {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				// NO_OVERWRITE rather than DISCARD: a discard would throw away the rows the
				// engine has written, which is every row but these eight.
				const HRESULT hr = a_context->Map(
					a_buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
				result.hr = static_cast<std::int32_t>(hr);
				if (SUCCEEDED(hr) && mapped.pData) {
					std::memcpy(
						static_cast<std::uint8_t*>(mapped.pData) +
							Addr::kGlobalCameraFarReconstructionOffset,
						a_rows,
						sizeof(Rows));
					result.written = true;
					a_context->Unmap(a_buffer, 0);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			return result;
		}
	}

	void SetEnabled(bool a_enabled) noexcept
	{
		g_enabled.store(a_enabled, std::memory_order_release);
	}

	bool Enabled() noexcept
	{
		return g_enabled.load(std::memory_order_acquire);
	}

	void Release() noexcept
	{
		const std::lock_guard<std::mutex> guard{ g_lock };
		ReleaseLocked();
	}

	void RequestRelease() noexcept
	{
		g_releaseRequested.store(true, std::memory_order_release);
	}

	void NoteDrawExamined() noexcept
	{
		g_drawsExamined.fetch_add(1, std::memory_order_relaxed);
	}

	void NoteCompositeDraw(std::uint32_t a_distinctShaders) noexcept
	{
		g_compositeDraws.fetch_add(1, std::memory_order_relaxed);
		g_compositeShaders.store(a_distinctShaders, std::memory_order_relaxed);
	}

	bool PatchForDraw(bool a_enabled) noexcept
	{
		const std::lock_guard<std::mutex> guard{ g_lock };
		if (g_releaseRequested.exchange(false, std::memory_order_acq_rel)) {
			ReleaseLocked();
		}
		if (!a_enabled) {
			// Not an attempt: the key off is the default and would otherwise report a
			// decline on every composite draw of every frame.
			ReleaseLocked();
			return false;
		}

		g_attempts.fetch_add(1, std::memory_order_relaxed);

		// The gate is the two LIVE ratios and nothing else, which is the engine's own
		// ImageSpaceEffectDynamicResolution::IsActive predicate. The light accessor, not
		// Snapshot: this is a per-draw site.
		float ratioWidth = 0.0f;
		float ratioHeight = 0.0f;
		const bool readable = DynamicResolution::CurrentRatios(ratioWidth, ratioHeight);
		// Recorded before the gate, not after, so a declining arm still says what the
		// ratios were at the draw's own instant.
		g_lastRatioWidth.store(ratioWidth, std::memory_order_relaxed);
		g_lastRatioHeight.store(ratioHeight, std::memory_order_relaxed);
		if (g_ratiosLogged < kRatioLogLimit) {
			++g_ratiosLogged;
			logger::info(
				FMT_STRING("composite reconstruction scale: composite draw {} read live ratios {:.6f} x {:.6f}, readable {}, thread {}"),
				g_ratiosLogged,
				ratioWidth,
				ratioHeight,
				readable ? "yes"sv : "no"sv,
				static_cast<std::uint32_t>(::GetCurrentThreadId()));
		}
		if (!readable) {
			RecordDecline(Decline::kNoState);
			return false;
		}
		if (!std::isfinite(ratioWidth) || !std::isfinite(ratioHeight) ||
			ratioWidth <= 0.0f || ratioHeight <= 0.0f ||
			ratioWidth > 1.0f || ratioHeight > 1.0f) {
			RecordDecline(Decline::kNotReduced);
			return false;
		}
		if (ratioWidth == 1.0f && ratioHeight == 1.0f) {
			// T is the identity and the write would put back what is already there.
			RecordDecline(Decline::kRatioOne);
			return false;
		}

		auto* const device = Engine::Device();
		auto* const context = Engine::ImmediateContext();
		auto* const engineBuffer = EngineCameraBuffer();
		XMFLOAT4X4 farViewData{};
		XMFLOAT4X4 nearViewData{};
		if (!device || !context || !engineBuffer ||
			!ReadEyeZeroMatrix(Addr::kViewDataFarViewProjection, farViewData) ||
			!ReadEyeZeroMatrix(Addr::kViewDataNearViewProjection, nearViewData)) {
			RecordDecline(Decline::kNoState);
			if (!g_declineLogged) {
				g_declineLogged = true;
				logger::warn(
					FMT_STRING("composite reconstruction scale declined: device {}, context {}, camera buffer {}"),
					device ? "present"sv : "absent"sv,
					context ? "present"sv : "absent"sv,
					engineBuffer ? "present"sv : "absent"sv);
			}
			return false;
		}

		auto reason = Decline::kPartialUpdate;
		if (!EnsureProbed(device, engineBuffer, reason)) {
			RecordDecline(reason);
			return false;
		}
		if (g_bufferBytes.load(std::memory_order_relaxed) <
			Addr::kGlobalCameraNearReconstructionOffset + sizeof(XMFLOAT4X4)) {
			RecordDecline(Decline::kNoState);
			return false;
		}

		// The engine writes transpose(XMMatrixInverse(field)) - the inverse's four rows run
		// through the standard four-SHUFPS transpose before the 0x40-byte copy into the
		// buffer - so each stored row is the row the shader's dp4 applies to
		// (clip.x, clip.y, depth, 1), and post-multiplying by T is exactly
		// row' . p = row . (T p). A dynamic buffer cannot be read back, so the originals
		// are recomputed from the same two operands the uploader used rather than saved.
		const XMMATRIX farEngine = EngineRows(farViewData);
		const XMMATRIX nearEngine = EngineRows(nearViewData);
		XMStoreFloat4x4(&g_originalRows.farRows, farEngine);
		XMStoreFloat4x4(&g_originalRows.nearRows, nearEngine);
		Rows patched{};
		XMStoreFloat4x4(&patched.farRows, ScaleRows(farEngine, ratioWidth, ratioHeight));
		XMStoreFloat4x4(&patched.nearRows, ScaleRows(nearEngine, ratioWidth, ratioHeight));

		if (!g_rowsLogged) {
			g_rowsLogged = true;
			logger::info(
				FMT_STRING("composite reconstruction scale: ratios {:.6f} x {:.6f}, eight rows before and after the transform"),
				ratioWidth,
				ratioHeight);
			LogRows("far before", g_originalRows.farRows);
			LogRows("far after", patched.farRows);
			LogRows("near before", g_originalRows.nearRows);
			LogRows("near after", patched.nearRows);
		}

		const auto result = WriteRowsIsolated(context, engineBuffer, &patched);
		g_lastHr.store(result.hr, std::memory_order_relaxed);
		if (result.exceptionCode != 0) {
			g_exceptionCode.store(result.exceptionCode, std::memory_order_relaxed);
		}
		if (result.exceptionCode != 0 || !result.written) {
			RecordDecline(Decline::kCopyFailed);
			if (!g_mapFailureLogged) {
				g_mapFailureLogged = true;
				logger::error(
					FMT_STRING("composite reconstruction scale: the camera buffer could not be mapped without discarding it, HRESULT {:08X}, exception {:08X}"),
					static_cast<std::uint32_t>(result.hr),
					result.exceptionCode);
			}
			if (result.exceptionCode != 0) {
				// The buffer's map state cannot be proved after a fault inside Map; nothing
				// here may touch it again until the key is flipped.
				ReleaseLocked();
			}
			return false;
		}

		g_patched = true;
		g_applied.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void RestoreRows(RestoreSite a_site) noexcept
	{
		const std::lock_guard<std::mutex> guard{ g_lock };
		if (!g_patched) {
			return;
		}
		g_patched = false;
		auto* const context = Engine::ImmediateContext();
		auto* const engineBuffer = EngineCameraBuffer();
		if (!context || !engineBuffer) {
			RecordDecline(Decline::kRestoreFailed);
			return;
		}
		const auto result = WriteRowsIsolated(context, engineBuffer, &g_originalRows);
		if (result.exceptionCode != 0) {
			g_exceptionCode.store(result.exceptionCode, std::memory_order_relaxed);
		}
		if (result.exceptionCode != 0 || !result.written) {
			// A patch that could not be undone is the one failure that damages the rest of
			// the frame, so it latches the module off rather than trying again next frame.
			g_lastHr.store(result.hr, std::memory_order_relaxed);
			RecordDecline(Decline::kRestoreFailed);
			logger::error(
				FMT_STRING("composite reconstruction scale: the eight rows could not be put back, HRESULT {:08X}; the correction is off for this session"),
				static_cast<std::uint32_t>(result.hr));
			ReleaseLocked();
			return;
		}
		switch (a_site) {
		case RestoreSite::kDraw:
			g_restoredAtDraw.fetch_add(1, std::memory_order_relaxed);
			break;
		case RestoreSite::kWater:
			g_restoredAtWater.fetch_add(1, std::memory_order_relaxed);
			break;
		case RestoreSite::kSeamExit:
			g_restoredAtSeamExit.fetch_add(1, std::memory_order_relaxed);
			break;
		}
		g_restores.fetch_add(1, std::memory_order_relaxed);
	}

	State Snapshot() noexcept
	{
		State state{};
		state.attempts = g_attempts.load(std::memory_order_relaxed);
		state.applied = g_applied.load(std::memory_order_relaxed);
		state.declines = g_declines.load(std::memory_order_relaxed);
		state.restores = g_restores.load(std::memory_order_relaxed);
		state.lastDecline = g_lastDecline.load(std::memory_order_relaxed);
		state.exceptionCode = g_exceptionCode.load(std::memory_order_relaxed);
		state.lastHr = g_lastHr.load(std::memory_order_relaxed);
		state.lastRatioWidth = g_lastRatioWidth.load(std::memory_order_relaxed);
		state.lastRatioHeight = g_lastRatioHeight.load(std::memory_order_relaxed);
		state.bufferBytes = g_bufferBytes.load(std::memory_order_relaxed);
		state.ready = g_ready.load(std::memory_order_relaxed);
		state.restoredAtDraw = g_restoredAtDraw.load(std::memory_order_relaxed);
		state.drawsExamined = g_drawsExamined.load(std::memory_order_relaxed);
		state.compositeDraws = g_compositeDraws.load(std::memory_order_relaxed);
		state.compositeShaders = g_compositeShaders.load(std::memory_order_relaxed);
		state.restoredAtWater = g_restoredAtWater.load(std::memory_order_relaxed);
		state.restoredAtSeamExit = g_restoredAtSeamExit.load(std::memory_order_relaxed);
		return state;
	}
}
