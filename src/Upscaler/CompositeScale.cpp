#include "Upscaler/CompositeScale.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/CompositeScaleMath.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <atomic>
#include <cmath>
#include <mutex>

namespace Upscaler::CompositeScale
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
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

		// MEASURED 2026-09-09, and the reason this module is mutex-guarded rather than
		// render-thread-owned like every other D3D11 module here: the composite's technique
		// setup does NOT run on the thread ClearColor runs on. In one headless Quality
		// launch 331 of 333 world composite passes ran on another thread and only the
		// main-menu ones ran on ClearColor's. It is safe to drive the context from there
		// anyway, because the engine does: the same function Maps and Unmaps the immediate
		// context at 142919e9f / 14291a3b4 and binds slot 2 through it at 14291a410 and
		// 14291a447. What is NOT safe is two threads walking the objects below, so the lock
		// covers every touch of them and is held across the binding itself.
		std::mutex g_lock;
		ComPtr<ID3D11Device> g_device;
		ComPtr<ID3D11Buffer> g_copy;
		ComPtr<ID3D11Buffer> g_staging;
		std::uint32_t g_builtBytes = 0;
		// Borrowed and only ever compared, never dereferenced through and never released:
		// the engine owns the camera buffer and outlives us, and a reallocation at the same
		// ByteWidth still has to rebuild the copy.
		ID3D11Buffer* g_builtSource = nullptr;
		bool g_creationFailed = false;
		bool g_creationLogged = false;
		bool g_rowsLogged = false;
		bool g_declineLogged = false;
		// The first ten attempts after a key flip log the two live ratios the site actually
		// read, decline or not. That is what says the site is inside the reduced window,
		// and it is bounded by a count rather than by time so a ten-pass reading is exactly
		// ten lines.
		std::uint32_t g_ratiosLogged = 0;
		constexpr std::uint32_t kRatioLogLimit = 10;

		// Set by the setup, consumed by the restore. Thread-local, because the pair runs on
		// whichever thread the composite pass is on and two passes on two threads must not
		// hand each other a binding; cleared before anything can fault, so a faulted setup
		// cannot leave the restore rebinding a pointer it never bound.
		thread_local bool g_bound = false;
		thread_local ID3D11Buffer* g_boundEngineBuffer = nullptr;

		std::atomic<bool> g_enabled{ false };
		std::atomic<bool> g_releaseRequested{ false };
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_applied{ 0 };
		std::atomic<std::uint64_t> g_declines{ 0 };
		std::atomic<std::uint64_t> g_restores{ 0 };
		std::atomic<std::uint64_t> g_restoreFoundOurs{ 0 };
		std::atomic<std::uint64_t> g_restoreFoundOther{ 0 };
		std::atomic<std::uint32_t> g_lastDecline{ 0 };
		std::atomic<std::uint32_t> g_exceptionCode{ 0 };
		std::atomic<std::int32_t> g_lastHr{ 0 };
		std::atomic<float> g_lastRatioWidth{ 0.0f };
		std::atomic<float> g_lastRatioHeight{ 0.0f };
		std::atomic<std::uint32_t> g_bufferBytes{ 0 };
		std::atomic<bool> g_ready{ false };

		// The lock is already held at every internal call site; Release below is the
		// public one and takes it.
		void ReleaseLocked() noexcept
		{
			g_creationFailed = false;
			g_creationLogged = false;
			g_rowsLogged = false;
			g_declineLogged = false;
			g_ratiosLogged = 0;
			if (!g_device) {
				return;
			}
			g_ready.store(false, std::memory_order_relaxed);
			g_staging.Reset();
			g_copy.Reset();
			g_device.Reset();
			g_builtBytes = 0;
			g_builtSource = nullptr;
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
		// reads eye-0's rows for both eyes - a flat-derived pass that VR never corrected -
		// so reproducing the uploader's operand exactly is what keeps the copy a scaled
		// version of what the shader would otherwise have read, and nothing else.
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

		// The copy is the engine buffer's own size so CopyResource stays a whole-resource
		// copy; the staging buffer is the eight rows and nothing else, updated whole and
		// then copied into the copy's own byte range.
		[[nodiscard]] bool EnsureObjects(
			ID3D11Device* a_device,
			ID3D11Buffer* a_source,
			std::uint32_t a_bytes,
			Decline& a_reason) noexcept
		{
			if (g_device) {
				if (g_device.Get() != a_device || g_builtBytes != a_bytes || g_builtSource != a_source) {
					ReleaseLocked();
				}
			}
			if (g_ready.load(std::memory_order_relaxed)) {
				return true;
			}
			if (g_creationFailed) {
				a_reason = Decline::kResourceCreation;
				return false;
			}

			D3D11_BUFFER_DESC copyDesc{};
			copyDesc.ByteWidth = a_bytes;
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			copyDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			ComPtr<ID3D11Buffer> copy;
			HRESULT hr = a_device->CreateBuffer(&copyDesc, nullptr, copy.GetAddressOf());

			ComPtr<ID3D11Buffer> staging;
			if (SUCCEEDED(hr)) {
				const Rows initial{};
				D3D11_BUFFER_DESC stagingDesc{};
				stagingDesc.ByteWidth = sizeof(Rows);
				stagingDesc.Usage = D3D11_USAGE_DEFAULT;
				stagingDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				D3D11_SUBRESOURCE_DATA initialData{};
				initialData.pSysMem = &initial;
				hr = a_device->CreateBuffer(&stagingDesc, &initialData, staging.GetAddressOf());
			}

			g_lastHr.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr) || !copy || !staging) {
				g_creationFailed = true;
				if (!g_creationLogged) {
					g_creationLogged = true;
					logger::error(
						FMT_STRING("composite reconstruction scale: could not build a {}-byte copy of the global camera buffer, HRESULT {:08X}; the pass does nothing until the key is flipped"),
						a_bytes,
						static_cast<std::uint32_t>(hr));
				}
				a_reason = Decline::kResourceCreation;
				return false;
			}

			g_device = a_device;
			g_copy = std::move(copy);
			g_staging = std::move(staging);
			g_builtBytes = a_bytes;
			g_builtSource = a_source;
			g_ready.store(true, std::memory_order_relaxed);
			g_bufferBytes.store(a_bytes, std::memory_order_relaxed);
			logger::info(
				FMT_STRING("composite reconstruction scale: ready, {} bytes of global camera constants copied per composite pass, rows written at byte {:#x}"),
				a_bytes,
				Addr::kGlobalCameraFarReconstructionOffset);
			return true;
		}

		// Keep this boundary free of destructible C++ locals: a driver SEH must not unwind
		// through a frame the engine's own context state depends on.
		struct BindResult
		{
			std::uint32_t exceptionCode = 0;
			bool bound = false;
		};

		[[nodiscard]] BindResult BindIsolated(
			ID3D11DeviceContext* a_context,
			ID3D11Buffer* a_source,
			ID3D11Buffer* a_copy,
			ID3D11Buffer* a_staging,
			const Rows* a_rows) noexcept
		{
			BindResult result{};
			__try {
				// Whole-resource first, then the eight rows over the top of it, so every row
				// the composite reads that is NOT one of the eight is the engine's own.
				a_context->CopyResource(a_copy, a_source);
				a_context->UpdateSubresource(a_staging, 0, nullptr, a_rows, 0, 0);
				D3D11_BOX box{};
				box.left = 0;
				box.right = static_cast<UINT>(sizeof(Rows));
				box.top = 0;
				box.bottom = 1;
				box.front = 0;
				box.back = 1;
				a_context->CopySubresourceRegion(
					a_copy,
					0,
					static_cast<UINT>(Addr::kGlobalCameraFarReconstructionOffset),
					0,
					0,
					a_staging,
					0,
					&box);
				ID3D11Buffer* buffers[] = { a_copy };
				a_context->PSSetConstantBuffers(Addr::kCameraConstantBufferSlot, 1, buffers);
				a_context->VSSetConstantBuffers(Addr::kCameraConstantBufferSlot, 1, buffers);
				result.bound = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			return result;
		}

		[[nodiscard]] std::uint32_t RestoreIsolated(
			ID3D11DeviceContext* a_context,
			ID3D11Buffer* a_engine,
			ID3D11Buffer* a_copy) noexcept
		{
			std::uint32_t exceptionCode = 0;
			__try {
				// What slot 12 holds at the restore is the whole binding self-check: the
				// engine caches its own constant-buffer bindings and only rebinds on a
				// change, so finding our copy here is the pass having survived to the
				// composite's draws, and finding anything else says something rebound cb12
				// in between and the copy never reached one.
				ID3D11Buffer* observed = nullptr;
				a_context->PSGetConstantBuffers(Addr::kCameraConstantBufferSlot, 1, &observed);
				if (observed == a_copy) {
					g_restoreFoundOurs.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_restoreFoundOther.fetch_add(1, std::memory_order_relaxed);
				}
				if (observed) {
					observed->Release();
				}
				ID3D11Buffer* buffers[] = { a_engine };
				a_context->PSSetConstantBuffers(Addr::kCameraConstantBufferSlot, 1, buffers);
				a_context->VSSetConstantBuffers(Addr::kCameraConstantBufferSlot, 1, buffers);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			return exceptionCode;
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
		// The build latch clears first inside, so a key flipped off and on again is a
		// fresh attempt.
		const std::lock_guard<std::mutex> guard{ g_lock };
		ReleaseLocked();
	}

	void RequestRelease() noexcept
	{
		g_releaseRequested.store(true, std::memory_order_release);
	}

	void OnCompositeSetup(bool a_enabled) noexcept
	{
		// The release point. This is the render thread and the composite runs on every
		// world frame whatever the mode, so a live switch to DLAA or native drops the copy
		// here rather than holding it for the session. One relaxed exchange when nothing
		// was asked.
		const std::lock_guard<std::mutex> guard{ g_lock };
		if (g_releaseRequested.exchange(false, std::memory_order_acq_rel)) {
			ReleaseLocked();
		}
		if (!a_enabled) {
			// Not an attempt: the key off is the default and would otherwise report a
			// decline on every composite pass of every frame.
			ReleaseLocked();
			return;
		}

		g_attempts.fetch_add(1, std::memory_order_relaxed);
		g_bound = false;
		g_boundEngineBuffer = nullptr;

		// The gate is the two LIVE ratios and nothing else, which is the engine's own
		// ImageSpaceEffectDynamicResolution::IsActive predicate: either differing from one
		// is precisely the condition the composite's reconstruction gets wrong, and it is
		// true exactly while the raster is packed. The mod's own reduced-world bracket is
		// deliberately NOT consulted - MEASURED 2026-09-09, it is closed at every composite
		// pass of a Quality launch, because this pass runs on the world render thread and
		// the bracket is opened and closed on the thread that drives the ImageSpace chain,
		// so a bracket test here declined 355 of 355 attempts and did nothing at all.
		const auto dynamicResolution = DynamicResolution::Snapshot();
		const float ratioWidth = dynamicResolution.currentWidth;
		const float ratioHeight = dynamicResolution.currentHeight;
		// Recorded before the gate, not after, so a declining arm still says what the
		// ratios were at the composite's own instant rather than leaving a reader to guess.
		g_lastRatioWidth.store(ratioWidth, std::memory_order_relaxed);
		g_lastRatioHeight.store(ratioHeight, std::memory_order_relaxed);
		if (g_ratiosLogged < kRatioLogLimit) {
			++g_ratiosLogged;
			logger::info(
				FMT_STRING("composite reconstruction scale: pass {} read live ratios {:.6f} x {:.6f}, readable {}"),
				g_ratiosLogged,
				ratioWidth,
				ratioHeight,
				dynamicResolution.currentValuesValid ? "yes"sv : "no"sv);
		}
		if (!dynamicResolution.currentValuesValid) {
			RecordDecline(Decline::kNoState);
			return;
		}
		if (!std::isfinite(ratioWidth) || !std::isfinite(ratioHeight) ||
			ratioWidth <= 0.0f || ratioHeight <= 0.0f ||
			ratioWidth > 1.0f || ratioHeight > 1.0f) {
			RecordDecline(Decline::kNotReduced);
			return;
		}
		if (ratioWidth == 1.0f && ratioHeight == 1.0f) {
			// T is the identity and the copy would be the engine's buffer with extra steps.
			RecordDecline(Decline::kRatioOne);
			return;
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
			return;
		}

		D3D11_BUFFER_DESC sourceDesc{};
		engineBuffer->GetDesc(&sourceDesc);
		if (sourceDesc.ByteWidth <
			Addr::kGlobalCameraNearReconstructionOffset + sizeof(XMFLOAT4X4)) {
			RecordDecline(Decline::kNoState);
			return;
		}

		auto reason = Decline::kResourceCreation;
		if (!EnsureObjects(device, engineBuffer, sourceDesc.ByteWidth, reason)) {
			RecordDecline(reason);
			return;
		}

		// The engine writes transpose(XMMatrixInverse(M)) - the inverse's four rows run
		// through the standard four-SHUFPS transpose before the 0x40-byte copy into the
		// buffer - so each stored row is the row the shader's dp4 applies to
		// (clip.x, clip.y, depth, 1), and post-multiplying by T is exactly
		// row' . p = row . (T p). CompositeScaleMath.h carries both and its own check.
		const XMMATRIX farEngine = EngineRows(farViewData);
		const XMMATRIX nearEngine = EngineRows(nearViewData);
		Rows rows{};
		XMStoreFloat4x4(&rows.farRows, ScaleRows(farEngine, ratioWidth, ratioHeight));
		XMStoreFloat4x4(&rows.nearRows, ScaleRows(nearEngine, ratioWidth, ratioHeight));

		if (!g_rowsLogged) {
			g_rowsLogged = true;
			XMFLOAT4X4 farBefore{};
			XMFLOAT4X4 nearBefore{};
			XMStoreFloat4x4(&farBefore, farEngine);
			XMStoreFloat4x4(&nearBefore, nearEngine);
			logger::info(
				FMT_STRING("composite reconstruction scale: ratios {:.6f} x {:.6f}, eight rows before and after the transform"),
				ratioWidth,
				ratioHeight);
			LogRows("far before", farBefore);
			LogRows("far after", rows.farRows);
			LogRows("near before", nearBefore);
			LogRows("near after", rows.nearRows);
		}

		const auto result = BindIsolated(context, engineBuffer, g_copy.Get(), g_staging.Get(), &rows);
		if (result.exceptionCode != 0) {
			g_exceptionCode.store(result.exceptionCode, std::memory_order_relaxed);
		}
		if (result.exceptionCode != 0 || !result.bound) {
			RecordDecline(Decline::kCopyFailed);
			// The context cannot be proved after a fault inside it; nothing this module
			// owns may be bound again until the key is flipped.
			ReleaseLocked();
			return;
		}

		g_bound = true;
		g_boundEngineBuffer = engineBuffer;
		g_applied.fetch_add(1, std::memory_order_relaxed);
	}

	void OnCompositeRestore() noexcept
	{
		if (!g_bound || !g_boundEngineBuffer) {
			return;
		}
		auto* const engineBuffer = g_boundEngineBuffer;
		g_bound = false;
		g_boundEngineBuffer = nullptr;
		const std::lock_guard<std::mutex> guard{ g_lock };
		auto* const copy = g_copy.Get();
		auto* const context = Engine::ImmediateContext();
		if (!context || !copy) {
			return;
		}
		const auto exceptionCode = RestoreIsolated(context, engineBuffer, copy);
		if (exceptionCode != 0) {
			g_exceptionCode.store(exceptionCode, std::memory_order_relaxed);
			ReleaseLocked();
			return;
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
		state.restoreFoundOurs = g_restoreFoundOurs.load(std::memory_order_relaxed);
		state.restoreFoundOther = g_restoreFoundOther.load(std::memory_order_relaxed);
		state.lastDecline = g_lastDecline.load(std::memory_order_relaxed);
		state.exceptionCode = g_exceptionCode.load(std::memory_order_relaxed);
		state.lastHr = g_lastHr.load(std::memory_order_relaxed);
		state.lastRatioWidth = g_lastRatioWidth.load(std::memory_order_relaxed);
		state.lastRatioHeight = g_lastRatioHeight.load(std::memory_order_relaxed);
		state.bufferBytes = g_bufferBytes.load(std::memory_order_relaxed);
		state.ready = g_ready.load(std::memory_order_relaxed);
		return state;
	}
}
