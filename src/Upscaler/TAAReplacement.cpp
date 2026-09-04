#include "Upscaler/TAAReplacement.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/Engine.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/SharpenShader.h"
#include "Upscaler/TAAPassthroughShader.h"
#include "Upscaler/Telemetry.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace Upscaler::TAAReplacement
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		constexpr std::uint64_t kDefaultTargetSuccesses = 512;
		using D3DCompileFn = HRESULT(WINAPI*)(
			LPCVOID,
			SIZE_T,
			LPCSTR,
			const D3D_SHADER_MACRO*,
			ID3DInclude*,
			LPCSTR,
			LPCSTR,
			UINT,
			UINT,
			ID3DBlob**,
			ID3DBlob**);

		std::atomic<bool> g_enabled{ false };
		std::atomic<bool> g_postDrawEnabled{ false };
		std::atomic<bool> g_bypassEnabled{ false };
		std::atomic<bool> g_requireNgxHandback{ false };
		std::atomic<bool> g_requireFfxHandback{ false };
		std::atomic<std::uint32_t> g_handbackSource{
			static_cast<std::uint32_t>(HDRRoundTrip::HandbackSource::kNone)
		};
		std::atomic<std::uint64_t> g_configEpoch{ 0 };
		std::atomic<std::uint32_t> g_activeRenderEntries{ 0 };
		std::mutex g_configLock;
		std::atomic<bool> g_ready{ false };
		std::atomic<bool> g_completed{ false };
		std::atomic<bool> g_sessionDisabled{ false };
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_successes{ 0 };
		std::atomic<std::uint64_t> g_noHandbackSkips{ 0 };
		std::atomic<std::uint64_t> g_handbackSourceMismatches{ 0 };
		std::atomic<std::uint64_t> g_handbackSequenceGaps{ 0 };
		std::atomic<std::uint64_t> g_validationFailures{ 0 };
		std::atomic<std::uint64_t> g_isolationFailures{ 0 };
		std::atomic<std::uint32_t> g_validationMask{ 0 };
		std::atomic<std::uint64_t> g_shaderBuilds{ 0 };
		std::atomic<std::uint64_t> g_history61Writes{ 0 };
		std::atomic<std::uint64_t> g_history62Writes{ 0 };
		std::atomic<std::uint64_t> g_postDrawOverwrites{ 0 };
		std::atomic<std::uint64_t> g_nativeDrawBypasses{ 0 };
		std::atomic<std::uint64_t> g_qualityPublishDraws{ 0 };
		std::atomic<std::uint64_t> g_qualityHistoryRepairArms{ 0 };
		std::atomic<std::uint64_t> g_qualityHistoryRepairAttempts{ 0 };
		std::atomic<std::uint64_t> g_qualityHistoryRepairCopies{ 0 };
		std::atomic<std::uint64_t> g_qualityHistoryRepairSuccesses{ 0 };
		std::atomic<std::uint64_t> g_qualityHistoryRepairFailures{ 0 };
		std::atomic<std::uint64_t> g_qualityNativeFallbackReleases{ 0 };
		std::atomic<std::uint64_t> g_qualityNativeFallbackDraws{ 0 };
		std::atomic<std::uint64_t> g_qualityNativeFallbackHistory61Writes{ 0 };
		std::atomic<std::uint64_t> g_qualityNativeFallbackHistory62Writes{ 0 };
		std::atomic<std::uint64_t> g_qualityNativeFallbackHistoryMismatches{ 0 };
		std::atomic<std::uint64_t> g_parityReadAttempts{ 0 };
		std::atomic<std::uint64_t> g_parityReadFailures{ 0 };
		std::atomic<std::uint64_t> g_parityInvalidValues{ 0 };
		std::atomic<std::uint64_t> g_currentRenderTargetMismatches{ 0 };
		std::atomic<std::uint64_t> g_lastTemporalAACall{ 0 };
		std::atomic<std::uint64_t> g_handbackBaseline{ 0 };
		std::atomic<std::uint64_t> g_lastHandbackSerial{ 0 };
		std::atomic<std::uint64_t> g_targetSuccesses{ kDefaultTargetSuccesses };
		std::atomic<std::uint32_t> g_width{ 0 };
		std::atomic<std::uint32_t> g_height{ 0 };
		std::atomic<std::uint32_t> g_sourceFormat{ 0 };
		std::atomic<std::uint32_t> g_historyFormat{ 0 };
		std::atomic<std::uint32_t> g_outputFormat{ 0 };
		std::atomic<std::uint32_t> g_lastParity{ 0xFFFFFFFFu };
		std::atomic<std::int32_t> g_expectedHistoryLogical{ -1 };
		std::atomic<std::int32_t> g_engineHistoryLogical{ -1 };
		std::atomic<std::int32_t> g_deviceRemovedReason{ S_OK };
		std::atomic<std::uint32_t> g_lastError{ S_OK };
		std::atomic<std::uintptr_t> g_contextAddress{ 0 };
		std::atomic<std::uintptr_t> g_sourceSrvAddress{ 0 };
		std::atomic<std::uintptr_t> g_historyRtvAddress{ 0 };
		std::atomic<std::uintptr_t> g_outputRtvAddress{ 0 };
		std::atomic<std::uintptr_t> g_parityAddress{ 0 };
		// Render thread only: which caller's isolated draw last consumed
		// seamDrawCloseFailure - true for the TemporalAA-seam publish (the interesting
		// one, where HDRRoundTrip has already written RT1), false for the plain
		// prepared-draw bypass or the post-draw overwrite. Distinguished by whether
		// DrawBindingsIsolated received a world-admission ticket, which only the
		// seam-publish caller passes.
		std::atomic<bool> g_seamDrawCloseFailureAtPublish{ false };

		// The reason for the disable that first tripped g_sessionDisabled this session, or
		// "none" before any. First-failure-wins, same as Hooks' dynamic-geometry reason -
		// but Disable's callers include fmt::format(...) temporaries (the handback-
		// sequence-gap and SEH-escape reasons below), so a bare pointer would dangle the
		// instant the caller's temporary is destroyed. This copies instead: 192 bytes holds
		// the longest reason in this file with every {} substitution filled to its
		// realistic worst case (108 characters - the handback-sequence-gap format with two
		// full-width uint64 serials) plus headroom, matching Backend/BackendTypes.h's
		// unavailableReason[192] precedent for a reason buffer. A reason longer than that is
		// truncated, not overrun.
		//
		// Written by whichever thread wins the first-failure exchange, and that is not
		// always the render thread: Disable's own callers run there, but AbortConfiguration
		// reaches the same latch from the settings thread. The exchange picks one winner
		// either way, so the buffer still has exactly one writer.
		constexpr std::size_t kSessionDisabledReasonBufferSize = 192;
		char g_sessionDisabledReasonBuffer[kSessionDisabledReasonBufferSize] = {};
		// Published only after the buffer above is fully written - see
		// RecordSessionDisabledReason. The listener thread reads it through
		// SessionDisabledReason() below, which is the only reader.
		std::atomic<bool> g_sessionDisabledReasonReady{ false };

		D3D11StateIsolation::Context g_isolation;
		ComPtr<ID3D11Device> g_device;
		ComPtr<ID3D11VertexShader> g_vertexShader;
		ComPtr<ID3D11PixelShader> g_pixelShader;
		ComPtr<ID3D11BlendState> g_blendState;
		ComPtr<ID3D11RasterizerState> g_rasterizerState;
		ComPtr<ID3D11DepthStencilState> g_depthStencilState;
		HMODULE g_compilerModule = nullptr;
		D3DCompileFn g_compile = nullptr;

		// The seam sharpen. The settings thread publishes the value and bumps the
		// generation under the closed configuration epoch; the render thread compares the
		// generation against the one it last uploaded (render-thread only, no atomic
		// needed) and re-writes the constant buffer only then. The shader objects are
		// built once with the passthrough's in EnsureObjects and are render-thread only.
		std::atomic<float> g_sharpnessRequested{ 0.0f };
		std::atomic<std::uint32_t> g_sharpenGeneration{ 0 };
		std::uint32_t g_sharpenUploadedGeneration = 0;
		std::atomic<std::uint64_t> g_sharpenCompileFailures{ 0 };
		std::atomic<std::uint32_t> g_sharpenLastHr{ S_OK };
		std::atomic<std::uint32_t> g_sharpenShaderBound{ static_cast<std::uint32_t>(SeamShader::kNone) };
		ComPtr<ID3D11PixelShader> g_sharpenPixelShader;
		ComPtr<ID3D11Buffer> g_sharpenConstants;

		struct Bindings
		{
			ComPtr<ID3D11ShaderResourceView> source;
			ComPtr<ID3D11RenderTargetView> history;
			ComPtr<ID3D11RenderTargetView> submitted;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			D3D11_TEXTURE2D_DESC historyDesc{};
			D3D11_TEXTURE2D_DESC submittedDesc{};
			std::int32_t historyLogical = 0;
			std::uint32_t validationMask = 0;
		};

		// One reduced-NGX episode from PrepareQualityBypass to the late-range commit.
		// published means the seam draw wrote the history and RT4 recorded here; until
		// then an abort must repair the selected history from RT4.
		struct PendingQualitySuccess
		{
			bool active = false;
			bool historyRepairRequired = false;
			bool repairBothHistories = false;
			bool published = false;
			bool bypass = false;
			std::uint32_t parity = 0xFFFFFFFFu;
			std::int32_t historyLogical = 0;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::uint64_t configEpoch = 0;
			HDRRoundTrip::HandbackStamp handback{};
			ComPtr<ID3D11Texture2D> historyTexture;
			ComPtr<ID3D11Texture2D> alternateHistoryTexture;
			ComPtr<ID3D11Texture2D> submittedTexture;
			ComPtr<ID3D11RenderTargetView> historyRtv;
			ComPtr<ID3D11RenderTargetView> alternateHistoryRtv;
			ComPtr<ID3D11RenderTargetView> submittedRtv;
			D3D11_TEXTURE2D_DESC historyDesc{};
			D3D11_TEXTURE2D_DESC alternateHistoryDesc{};
			D3D11_TEXTURE2D_DESC submittedDesc{};
		};
		thread_local PendingQualitySuccess g_pendingQualitySuccess{};

		// Configure may run on the devbench listener while the render thread is already
		// inside a replacement. The render thread never waits: it either joins one stable
		// even configuration epoch or declines that frame. Configure closes the epoch and
		// waits for admitted entries to drain before replacing the source/baseline. The
		// sequentially-consistent ordering also closes the load-before-increment race: an
		// entry delayed across a complete close/reopen observes a different epoch and exits.
		struct RenderEntryToken
		{
			HDRRoundTrip::RuntimeConfigurationEntryToken runtime{};
			bool entered = false;
			std::uint64_t epoch = 0;
		};

		[[nodiscard]] RenderEntryToken EnterRenderEntry(
			const std::atomic<bool>& a_modeEnabled) noexcept
		{
			RenderEntryToken token{};
			token.runtime = HDRRoundTrip::EnterRuntimeConfiguration();
			if (!token.runtime.entered) {
				return token;
			}
			const auto epoch = g_configEpoch.load(std::memory_order_seq_cst);
			if ((epoch & 1u) != 0 || !a_modeEnabled.load(std::memory_order_acquire)) {
				HDRRoundTrip::LeaveRuntimeConfiguration(token.runtime);
				return token;
			}

			g_activeRenderEntries.fetch_add(1, std::memory_order_seq_cst);
			if (g_configEpoch.load(std::memory_order_seq_cst) != epoch ||
				!a_modeEnabled.load(std::memory_order_acquire)) {
				g_activeRenderEntries.fetch_sub(1, std::memory_order_seq_cst);
				HDRRoundTrip::LeaveRuntimeConfiguration(token.runtime);
				return token;
			}
			token.entered = true;
			token.epoch = epoch;
			return token;
		}

		void LeaveRenderEntry(RenderEntryToken& a_token) noexcept
		{
			if (a_token.entered) {
				g_activeRenderEntries.fetch_sub(1, std::memory_order_seq_cst);
				a_token.entered = false;
				a_token.epoch = 0;
			}
			HDRRoundTrip::LeaveRuntimeConfiguration(a_token.runtime);
		}

		[[nodiscard]] bool Describe(
			ID3D11View* a_view,
			D3D11_TEXTURE2D_DESC& a_desc) noexcept
		{
			if (!a_view) {
				return false;
			}
			ComPtr<ID3D11Resource> resource;
			a_view->GetResource(resource.GetAddressOf());
			ComPtr<ID3D11Texture2D> texture;
			if (!resource || FAILED(resource.As(&texture)) || !texture) {
				return false;
			}
			texture->GetDesc(&a_desc);
			return true;
		}

		[[nodiscard]] bool ResolveTexture(
			ID3D11View* a_view,
			ComPtr<ID3D11Texture2D>& a_out) noexcept
		{
			if (!a_view) {
				return false;
			}
			ComPtr<ID3D11Resource> resource;
			a_view->GetResource(resource.GetAddressOf());
			return resource && SUCCEEDED(resource.As(&a_out)) && a_out;
		}

		[[nodiscard]] bool SameTextureContract(
			const D3D11_TEXTURE2D_DESC& a_lhs,
			const D3D11_TEXTURE2D_DESC& a_rhs) noexcept
		{
			return a_lhs.Width == a_rhs.Width && a_lhs.Height == a_rhs.Height &&
			       a_lhs.MipLevels == a_rhs.MipLevels && a_lhs.ArraySize == a_rhs.ArraySize &&
			       a_lhs.Format == a_rhs.Format &&
			       a_lhs.SampleDesc.Count == a_rhs.SampleDesc.Count &&
			       a_lhs.SampleDesc.Quality == a_rhs.SampleDesc.Quality;
		}

		[[nodiscard]] bool CapturePostDrawAndValidate(
			ID3D11DeviceContext* a_context,
			Bindings& a_out) noexcept
		{
			ID3D11ShaderResourceView* source = nullptr;
			a_context->PSGetShaderResources(0, 1, &source);
			a_out.source.Attach(source);

			ID3D11RenderTargetView* outputs[2]{};
			a_context->OMGetRenderTargets(2, outputs, nullptr);
			a_out.history.Attach(outputs[0]);
			a_out.submitted.Attach(outputs[1]);

			const auto color = RenderTargets::Lookup(Addr::kRT_PostTonemapColor);
			const auto historyA = RenderTargets::Lookup(Addr::kRT_Taa);
			const auto historyB = RenderTargets::Lookup(Addr::kRT_TaaAlt);
			const auto submitted = RenderTargets::Lookup(Addr::kRT_Submitted);
			constexpr std::uint32_t kLookup = 1u << 0;
			constexpr std::uint32_t kSourceIdentity = 1u << 1;
			constexpr std::uint32_t kHistoryIdentity = 1u << 2;
			constexpr std::uint32_t kSubmittedIdentity = 1u << 3;
			constexpr std::uint32_t kDistinctOutputs = 1u << 4;
			constexpr std::uint32_t kSourceDescription = 1u << 5;
			constexpr std::uint32_t kHistoryDescription = 1u << 6;
			constexpr std::uint32_t kSubmittedDescription = 1u << 7;
			constexpr std::uint32_t kDimensions = 1u << 8;
			constexpr std::uint32_t kFormats = 1u << 9;
			constexpr std::uint32_t kMipLevels = 1u << 10;
			constexpr std::uint32_t kArraySizes = 1u << 11;
			constexpr std::uint32_t kSampleCounts = 1u << 12;

			if (!color.valid || !historyA.valid || !historyB.valid || !submitted.valid) {
				a_out.validationMask |= kLookup;
			}
			if (a_out.source.Get() != color.srv) {
				a_out.validationMask |= kSourceIdentity;
			}
			if (a_out.history.Get() != historyA.rtv && a_out.history.Get() != historyB.rtv) {
				a_out.validationMask |= kHistoryIdentity;
			}
			if (a_out.submitted.Get() != submitted.rtv) {
				a_out.validationMask |= kSubmittedIdentity;
			}
			if (!a_out.history || a_out.history.Get() == a_out.submitted.Get()) {
				a_out.validationMask |= kDistinctOutputs;
			}
			if (!Describe(a_out.source.Get(), a_out.sourceDesc)) {
				a_out.validationMask |= kSourceDescription;
			}
			if (!Describe(a_out.history.Get(), a_out.historyDesc)) {
				a_out.validationMask |= kHistoryDescription;
			}
			if (!Describe(a_out.submitted.Get(), a_out.submittedDesc)) {
				a_out.validationMask |= kSubmittedDescription;
			}
			a_out.historyLogical = a_out.history.Get() == historyA.rtv ?
				Addr::kRT_Taa : Addr::kRT_TaaAlt;

			if (a_out.sourceDesc.Width == 0 || a_out.sourceDesc.Height == 0 ||
				a_out.sourceDesc.Width != a_out.historyDesc.Width ||
				a_out.sourceDesc.Height != a_out.historyDesc.Height ||
				a_out.sourceDesc.Width != a_out.submittedDesc.Width ||
				a_out.sourceDesc.Height != a_out.submittedDesc.Height) {
				a_out.validationMask |= kDimensions;
			}
			if (a_out.sourceDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
				a_out.historyDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
				a_out.submittedDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
				a_out.validationMask |= kFormats;
			}
			if (a_out.sourceDesc.MipLevels != 1 || a_out.historyDesc.MipLevels != 1 ||
				a_out.submittedDesc.MipLevels != 1) {
				a_out.validationMask |= kMipLevels;
			}
			if (a_out.sourceDesc.ArraySize != 1 || a_out.historyDesc.ArraySize != 1 ||
				a_out.submittedDesc.ArraySize != 1) {
				a_out.validationMask |= kArraySizes;
			}
			if (a_out.sourceDesc.SampleDesc.Count != 1 ||
				a_out.historyDesc.SampleDesc.Count != 1 ||
				a_out.submittedDesc.SampleDesc.Count != 1) {
				a_out.validationMask |= kSampleCounts;
			}
			return a_out.validationMask == 0;
		}

		[[nodiscard]] bool ReadHistoryParity(
			std::uint32_t& a_out,
			bool a_recordAttempt = true) noexcept
		{
			if (a_recordAttempt) {
				g_parityReadAttempts.fetch_add(1, std::memory_order_relaxed);
			}
			auto address = g_parityAddress.load(std::memory_order_acquire);
			if (address == 0) {
				address = Engine::RipResolve(
					Addr::kTaaHistoryParityAnchor,
					{ 0x8B, 0x35 },
					2,
					6,
					"native TAA history parity"sv);
				if (address != 0) {
					g_parityAddress.store(address, std::memory_order_release);
					logger::info(FMT_STRING("native-TAA bypass: resolved verified history-parity anchor to {:016X}"), address);
				}
			}
			if (address == 0 || !Engine::ReadU32(address, a_out)) {
				if (a_recordAttempt) {
					g_parityReadFailures.fetch_add(1, std::memory_order_relaxed);
				}
				return false;
			}
			if (a_out > 1) {
				if (a_recordAttempt) {
					g_parityReadFailures.fetch_add(1, std::memory_order_relaxed);
					g_parityInvalidValues.fetch_add(1, std::memory_order_relaxed);
				}
				g_lastParity.store(a_out, std::memory_order_relaxed);
				return false;
			}
			g_lastParity.store(a_out, std::memory_order_relaxed);
			return true;
		}

		[[nodiscard]] bool CaptureDirectAndValidate(
			std::uint32_t a_parity,
			Bindings& a_out) noexcept
		{
			const auto color = RenderTargets::Lookup(Addr::kRT_PostTonemapColor);
			const auto historyA = RenderTargets::Lookup(Addr::kRT_Taa);
			const auto historyB = RenderTargets::Lookup(Addr::kRT_TaaAlt);
			const auto submitted = RenderTargets::Lookup(Addr::kRT_Submitted);
			const auto& expectedHistory = a_parity == 0 ? historyB : historyA;
			a_out.historyLogical = a_parity == 0 ? Addr::kRT_TaaAlt : Addr::kRT_Taa;
			a_out.source = color.srv;
			a_out.history = expectedHistory.rtv;
			a_out.submitted = submitted.rtv;

			constexpr std::uint32_t kLookup = 1u << 0;
			constexpr std::uint32_t kSourceIdentity = 1u << 1;
			constexpr std::uint32_t kHistoryIdentity = 1u << 2;
			constexpr std::uint32_t kSubmittedIdentity = 1u << 3;
			constexpr std::uint32_t kDistinctOutputs = 1u << 4;
			constexpr std::uint32_t kSourceDescription = 1u << 5;
			constexpr std::uint32_t kHistoryDescription = 1u << 6;
			constexpr std::uint32_t kSubmittedDescription = 1u << 7;
			constexpr std::uint32_t kDimensions = 1u << 8;
			constexpr std::uint32_t kFormats = 1u << 9;
			constexpr std::uint32_t kMipLevels = 1u << 10;
			constexpr std::uint32_t kArraySizes = 1u << 11;
			constexpr std::uint32_t kSampleCounts = 1u << 12;
			constexpr std::uint32_t kCurrentRenderTarget = 1u << 13;

			if (!color.valid || !historyA.valid || !historyB.valid || !submitted.valid) {
				a_out.validationMask |= kLookup;
			}
			if (!a_out.source || a_out.source.Get() != color.srv) {
				a_out.validationMask |= kSourceIdentity;
			}
			if (!a_out.history || a_out.history.Get() != expectedHistory.rtv) {
				a_out.validationMask |= kHistoryIdentity;
			}
			if (!a_out.submitted || a_out.submitted.Get() != submitted.rtv) {
				a_out.validationMask |= kSubmittedIdentity;
			}
			if (a_out.history.Get() == a_out.submitted.Get()) {
				a_out.validationMask |= kDistinctOutputs;
			}
			if (!Describe(a_out.source.Get(), a_out.sourceDesc)) {
				a_out.validationMask |= kSourceDescription;
			}
			if (!Describe(a_out.history.Get(), a_out.historyDesc)) {
				a_out.validationMask |= kHistoryDescription;
			}
			if (!Describe(a_out.submitted.Get(), a_out.submittedDesc)) {
				a_out.validationMask |= kSubmittedDescription;
			}

			const auto engineHistory = Engine::CurrentRenderTargetIndex();
			g_expectedHistoryLogical.store(a_out.historyLogical, std::memory_order_relaxed);
			g_engineHistoryLogical.store(engineHistory, std::memory_order_relaxed);
			if (engineHistory != a_out.historyLogical) {
				a_out.validationMask |= kCurrentRenderTarget;
				g_currentRenderTargetMismatches.fetch_add(1, std::memory_order_relaxed);
			}
			if (a_out.sourceDesc.Width == 0 || a_out.sourceDesc.Height == 0 ||
				a_out.sourceDesc.Width != a_out.historyDesc.Width ||
				a_out.sourceDesc.Height != a_out.historyDesc.Height ||
				a_out.sourceDesc.Width != a_out.submittedDesc.Width ||
				a_out.sourceDesc.Height != a_out.submittedDesc.Height) {
				a_out.validationMask |= kDimensions;
			}
			if (a_out.sourceDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
				a_out.historyDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
				a_out.submittedDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
				a_out.validationMask |= kFormats;
			}
			if (a_out.sourceDesc.MipLevels != 1 || a_out.historyDesc.MipLevels != 1 ||
				a_out.submittedDesc.MipLevels != 1) {
				a_out.validationMask |= kMipLevels;
			}
			if (a_out.sourceDesc.ArraySize != 1 || a_out.historyDesc.ArraySize != 1 ||
				a_out.submittedDesc.ArraySize != 1) {
				a_out.validationMask |= kArraySizes;
			}
			if (a_out.sourceDesc.SampleDesc.Count != 1 ||
				a_out.historyDesc.SampleDesc.Count != 1 ||
				a_out.submittedDesc.SampleDesc.Count != 1) {
				a_out.validationMask |= kSampleCounts;
			}
			return a_out.validationMask == 0;
		}

		// Returns the compiler's HRESULT and leaves g_lastError alone: the passthrough
		// caller records a failure there and the sharpen caller in g_sharpenLastHr, so a
		// sharpen failure never overwrites the status of a healthy passthrough seam.
		[[nodiscard]] HRESULT CompileShader(
			const char* a_source,
			std::size_t a_sourceSize,
			const char* a_name,
			const char* a_entry,
			const char* a_target,
			ID3DBlob** a_out) noexcept
		{
			if (!g_compile) {
				g_compilerModule = ::LoadLibraryW(L"d3dcompiler_47.dll");
				if (g_compilerModule) {
					g_compile = reinterpret_cast<D3DCompileFn>(
						::GetProcAddress(g_compilerModule, "D3DCompile"));
				}
			}
			if (!g_compile) {
				logger::error("native-TAA replacement: d3dcompiler_47.dll / D3DCompile unavailable"sv);
				return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
			}

			ComPtr<ID3DBlob> errors;
			const HRESULT hr = g_compile(
				a_source,
				a_sourceSize,
				a_name,
				nullptr,
				nullptr,
				a_entry,
				a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
				0,
				a_out,
				errors.GetAddressOf());
			if (FAILED(hr)) {
				logger::error(
					FMT_STRING("native-TAA replacement: {} {} compile failed, HRESULT {:08X}: {}"),
					a_name,
					a_entry,
					static_cast<std::uint32_t>(hr),
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler message");
			}
			return hr;
		}

		[[nodiscard]] bool EnsureObjects(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context) noexcept
		{
			if (g_ready.load(std::memory_order_acquire)) {
				return g_device.Get() == a_device && g_isolation.Initialize(a_device, a_context);
			}
			if (!a_device || !a_context || !g_isolation.Initialize(a_device, a_context)) {
				g_lastError.store(
					static_cast<std::uint32_t>(g_isolation.LastError()),
					std::memory_order_relaxed);
				return false;
			}

			ComPtr<ID3DBlob> vertexCode;
			ComPtr<ID3DBlob> pixelCode;
			HRESULT hr = CompileShader(TAAPassthroughShader::kSource, sizeof(TAAPassthroughShader::kSource) - 1, "Fo4VRTaaReplacement", "VSMain", "vs_5_0", vertexCode.GetAddressOf());
			if (SUCCEEDED(hr)) {
				hr = CompileShader(TAAPassthroughShader::kSource, sizeof(TAAPassthroughShader::kSource) - 1, "Fo4VRTaaReplacement", "PSMain", "ps_5_0", pixelCode.GetAddressOf());
			}
			if (FAILED(hr)) {
				g_lastError.store(static_cast<std::uint32_t>(hr), std::memory_order_relaxed);
				return false;
			}

			ComPtr<ID3D11VertexShader> vertexShader;
			ComPtr<ID3D11PixelShader> pixelShader;
			ComPtr<ID3D11BlendState> blendState;
			ComPtr<ID3D11RasterizerState> rasterizerState;
			ComPtr<ID3D11DepthStencilState> depthStencilState;
			hr = a_device->CreateVertexShader(
				vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
				vertexShader.GetAddressOf());
			if (SUCCEEDED(hr)) {
				hr = a_device->CreatePixelShader(
					pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr,
					pixelShader.GetAddressOf());
			}

			D3D11_BLEND_DESC blend{};
			blend.IndependentBlendEnable = TRUE;
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			blend.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateBlendState(&blend, blendState.GetAddressOf());
			}

			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateRasterizerState(&raster, rasterizerState.GetAddressOf());
			}

			D3D11_DEPTH_STENCIL_DESC depth{};
			depth.DepthEnable = FALSE;
			depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depth.StencilEnable = FALSE;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateDepthStencilState(&depth, depthStencilState.GetAddressOf());
			}
			g_lastError.store(static_cast<std::uint32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr) || !vertexShader || !pixelShader || !blendState ||
				!rasterizerState || !depthStencilState) {
				logger::error(FMT_STRING("native-TAA replacement: shader/state creation failed, HRESULT {:08X}"),
					static_cast<std::uint32_t>(hr));
				return false;
			}

			// The sharpen shader is a quality add-on, not a correctness requirement: a
			// compile or buffer failure is counted, reported in g_sharpenLastHr, and the
			// seam keeps the passthrough with its g_lastError untouched - research tooling
			// reads that field as the seam's health. It shares the vertex shader - both
			// sources carry the same VSMain. The constant is created with the current
			// setting; the generation is read before the value so a change racing this
			// build is re-uploaded later.
			ComPtr<ID3D11PixelShader> sharpenShader;
			ComPtr<ID3D11Buffer> sharpenConstants;
			const auto sharpenGeneration = g_sharpenGeneration.load(std::memory_order_acquire);
			{
				ComPtr<ID3DBlob> sharpenCode;
				HRESULT sharpenHr = CompileShader(SharpenShader::kSource, sizeof(SharpenShader::kSource) - 1, "Fo4VRTaaSharpen", "PSMain", "ps_5_0", sharpenCode.GetAddressOf());
				const bool compiled = SUCCEEDED(sharpenHr);
				if (compiled) {
					sharpenHr = a_device->CreatePixelShader(
						sharpenCode->GetBufferPointer(), sharpenCode->GetBufferSize(), nullptr,
						sharpenShader.GetAddressOf());
				}
				if (compiled && SUCCEEDED(sharpenHr)) {
					const SharpenShader::Constants initial{
						SharpenShader::LinearScale(g_sharpnessRequested.load(std::memory_order_acquire))
					};
					D3D11_BUFFER_DESC constantsDesc{};
					constantsDesc.ByteWidth = sizeof(SharpenShader::Constants);
					constantsDesc.Usage = D3D11_USAGE_DEFAULT;
					constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
					D3D11_SUBRESOURCE_DATA initialData{};
					initialData.pSysMem = &initial;
					sharpenHr = a_device->CreateBuffer(&constantsDesc, &initialData, sharpenConstants.GetAddressOf());
				}
				if (FAILED(sharpenHr) || !sharpenShader || !sharpenConstants) {
					sharpenShader.Reset();
					sharpenConstants.Reset();
					g_sharpenCompileFailures.fetch_add(1, std::memory_order_relaxed);
					g_sharpenLastHr.store(static_cast<std::uint32_t>(sharpenHr), std::memory_order_relaxed);
					// A compile failure has already been logged with the compiler's message;
					// this line names its consequence rather than repeating it.
					logger::error(FMT_STRING("native-TAA replacement: RCAS sharpen shader {}, HRESULT {:08X}; the seam keeps the unsharpened passthrough and sharpness has no effect this session"),
						compiled ? "object creation failed"sv : "unavailable after the compile failure above"sv,
						static_cast<std::uint32_t>(sharpenHr));
				}
			}

			g_device = a_device;
			g_vertexShader = std::move(vertexShader);
			g_pixelShader = std::move(pixelShader);
			g_sharpenPixelShader = std::move(sharpenShader);
			g_sharpenConstants = std::move(sharpenConstants);
			g_sharpenUploadedGeneration = sharpenGeneration;
			g_blendState = std::move(blendState);
			g_rasterizerState = std::move(rasterizerState);
			g_depthStencilState = std::move(depthStencilState);
			g_shaderBuilds.fetch_add(1, std::memory_order_relaxed);
			g_ready.store(true, std::memory_order_release);
			logger::info(FMT_STRING("native-TAA replacement: private format-converting MRT shader and exact D3D11.1 state isolation are ready; RCAS sharpen shader {}"),
				g_sharpenPixelShader ? "compiled"sv : "unavailable"sv);
			return true;
		}

		[[nodiscard]] bool ValidateQualityPreflightTargets(
			D3D11_TEXTURE2D_DESC& a_submittedDesc) noexcept
		{
			const auto color = RenderTargets::Lookup(Addr::kRT_PostTonemapColor);
			const auto historyA = RenderTargets::Lookup(Addr::kRT_Taa);
			const auto historyB = RenderTargets::Lookup(Addr::kRT_TaaAlt);
			const auto submitted = RenderTargets::Lookup(Addr::kRT_Submitted);
			if (!color.valid || !color.srv || !historyA.valid || !historyA.rtv ||
				!historyB.valid || !historyB.rtv || !submitted.valid || !submitted.rtv ||
				!Describe(submitted.rtv, a_submittedDesc)) {
				return false;
			}
			D3D11_TEXTURE2D_DESC colorDesc{};
			D3D11_TEXTURE2D_DESC historyADesc{};
			D3D11_TEXTURE2D_DESC historyBDesc{};
			return Describe(color.srv, colorDesc) && Describe(historyA.rtv, historyADesc) &&
			       Describe(historyB.rtv, historyBDesc) &&
			       colorDesc.Width == a_submittedDesc.Width && colorDesc.Height == a_submittedDesc.Height &&
			       colorDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT &&
			       SameTextureContract(historyADesc, a_submittedDesc) &&
			       SameTextureContract(historyBDesc, a_submittedDesc);
		}

		[[nodiscard]] bool ArmQualityHistoryRepair(
			std::uint32_t a_parity,
			std::uint64_t a_configEpoch,
			const D3D11_TEXTURE2D_DESC& a_submittedDesc) noexcept
		{
			if (g_pendingQualitySuccess.active || a_parity > 1 || a_configEpoch == 0 ||
				(a_configEpoch & 1u) != 0) {
				return false;
			}
			const auto historyA = RenderTargets::Lookup(Addr::kRT_Taa);
			const auto historyB = RenderTargets::Lookup(Addr::kRT_TaaAlt);
			const auto& history = a_parity == 0 ? historyB : historyA;
			const auto& alternateHistory = a_parity == 0 ? historyA : historyB;
			const auto submitted = RenderTargets::Lookup(Addr::kRT_Submitted);
			ComPtr<ID3D11Texture2D> historyTexture;
			ComPtr<ID3D11Texture2D> alternateHistoryTexture;
			ComPtr<ID3D11Texture2D> submittedTexture;
			D3D11_TEXTURE2D_DESC historyDesc{};
			D3D11_TEXTURE2D_DESC alternateHistoryDesc{};
			if (!history.valid || !history.tex || !history.rtv ||
				!alternateHistory.valid || !alternateHistory.tex || !alternateHistory.rtv ||
				!submitted.valid || !submitted.tex || !submitted.rtv ||
				history.tex == alternateHistory.tex || history.rtv == alternateHistory.rtv ||
				history.tex == submitted.tex || history.rtv == submitted.rtv ||
				alternateHistory.tex == submitted.tex || alternateHistory.rtv == submitted.rtv ||
				!ResolveTexture(history.rtv, historyTexture) ||
				!ResolveTexture(alternateHistory.rtv, alternateHistoryTexture) ||
				!ResolveTexture(submitted.rtv, submittedTexture) ||
				historyTexture.Get() != history.tex ||
				alternateHistoryTexture.Get() != alternateHistory.tex ||
				submittedTexture.Get() != submitted.tex ||
				!Describe(history.rtv, historyDesc) ||
				!Describe(alternateHistory.rtv, alternateHistoryDesc) ||
				!SameTextureContract(historyDesc, a_submittedDesc) ||
				!SameTextureContract(alternateHistoryDesc, a_submittedDesc)) {
				return false;
			}

			PendingQualitySuccess pending{};
			pending.active = true;
			pending.historyRepairRequired = true;
			pending.parity = a_parity;
			pending.historyLogical = a_parity == 0 ? Addr::kRT_TaaAlt : Addr::kRT_Taa;
			pending.width = a_submittedDesc.Width;
			pending.height = a_submittedDesc.Height;
			pending.configEpoch = a_configEpoch;
			pending.historyTexture = std::move(historyTexture);
			pending.alternateHistoryTexture = std::move(alternateHistoryTexture);
			pending.submittedTexture = std::move(submittedTexture);
			pending.historyRtv = history.rtv;
			pending.alternateHistoryRtv = alternateHistory.rtv;
			pending.submittedRtv = submitted.rtv;
			pending.historyDesc = historyDesc;
			pending.alternateHistoryDesc = alternateHistoryDesc;
			pending.submittedDesc = a_submittedDesc;
			g_pendingQualitySuccess = std::move(pending);
			g_qualityHistoryRepairArms.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// Copies a_reason into the fixed reason buffer, truncated to fit, then publishes
		// it. Must run at most once per session, from the branch that just won the
		// g_sessionDisabled first-failure race - callers below pass fmt::format(...)
		// temporaries, so storing a_reason.data() instead of its bytes would leave a
		// dangling read the instant the caller's temporary is destroyed. Readers
		// (SessionDisabledReason) synchronise with the release store to
		// g_sessionDisabledReasonReady below: an acquire load that observes it true is
		// guaranteed to see this copy complete, never a partial write.
		void RecordSessionDisabledReason(std::string_view a_reason) noexcept
		{
			// Not std::min: Windows.h's min() macro (no NOMINMAX in this file) would
			// swallow it.
			const auto maxLen = kSessionDisabledReasonBufferSize - 1;
			const auto len = a_reason.size() < maxLen ? a_reason.size() : maxLen;
			std::memcpy(g_sessionDisabledReasonBuffer, a_reason.data(), len);
			g_sessionDisabledReasonBuffer[len] = '\0';
			g_sessionDisabledReasonReady.store(true, std::memory_order_release);
		}

		// The one place g_sessionDisabled is ever set. Returns true only to the caller
		// that won the first-failure race, so a logging caller (Disable, below) fires its
		// log exactly once and a silent caller (the direct-store sites) still records the
		// reason exactly once.
		[[nodiscard]] bool DisableSessionOnce(std::string_view a_reason) noexcept
		{
			if (g_sessionDisabled.exchange(true, std::memory_order_acq_rel)) {
				return false;
			}
			RecordSessionDisabledReason(a_reason);
			return true;
		}

		void Disable(
			std::string_view a_reason,
			bool a_validationFailure,
			bool a_isolationFailure) noexcept
		{
			if (a_validationFailure) {
				g_validationFailures.fetch_add(1, std::memory_order_relaxed);
			}
			if (a_isolationFailure) {
				g_isolationFailures.fetch_add(1, std::memory_order_relaxed);
			}
			if (DisableSessionOnce(a_reason)) {
				logger::error(FMT_STRING("native-TAA replacement disabled for this session: {}; vanilla TemporalAA continues on subsequent frames"), a_reason);
			}
		}

		[[nodiscard]] bool BeginAttempt(
			std::uint64_t a_temporalAACall,
			HDRRoundTrip::HandbackStamp a_handback) noexcept
		{
			if (g_completed.load(std::memory_order_acquire) ||
				g_sessionDisabled.load(std::memory_order_acquire)) {
				return false;
			}

			const auto requiredSource = static_cast<HDRRoundTrip::HandbackSource>(
				g_handbackSource.load(std::memory_order_acquire));
			if (requiredSource != HDRRoundTrip::HandbackSource::kNone) {
				if (a_handback.source != requiredSource) {
					g_noHandbackSkips.fetch_add(1, std::memory_order_relaxed);
					if (a_handback.source != HDRRoundTrip::HandbackSource::kNone) {
						const auto mismatches = g_handbackSourceMismatches.fetch_add(1, std::memory_order_relaxed) + 1;
						if (mismatches == 1 || mismatches == 16 || mismatches == 128) {
							logger::warn(FMT_STRING("native-TAA replacement: skipped handback source mismatch {} - required {}, observed {} serial {}"),
								mismatches,
								HDRRoundTrip::HandbackSourceName(requiredSource),
								HDRRoundTrip::HandbackSourceName(a_handback.source),
								a_handback.serial);
						}
					}
					return false;
				}

				const auto previous = g_lastHandbackSerial.load(std::memory_order_relaxed);
				const auto expected = previous + 1;
				if (a_handback.serial < expected) {
					g_noHandbackSkips.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				if (a_handback.serial > expected) {
					g_noHandbackSkips.fetch_add(1, std::memory_order_relaxed);
					g_handbackSequenceGaps.fetch_add(1, std::memory_order_relaxed);
					Disable(
						fmt::format(
							FMT_STRING("accepted {} handback sequence skipped required serial {} and reached {}"),
							HDRRoundTrip::HandbackSourceName(requiredSource),
							expected,
							a_handback.serial),
						false,
						false);
					return false;
				}
				g_lastHandbackSerial.store(a_handback.serial, std::memory_order_relaxed);
			}
			g_attempts.fetch_add(1, std::memory_order_relaxed);
			g_lastTemporalAACall.store(a_temporalAACall, std::memory_order_relaxed);
			return true;
		}

		void PublishBindings(const Bindings& a_bindings) noexcept
		{
			g_validationMask.store(a_bindings.validationMask, std::memory_order_relaxed);
			g_width.store(a_bindings.sourceDesc.Width, std::memory_order_relaxed);
			g_height.store(a_bindings.sourceDesc.Height, std::memory_order_relaxed);
			g_sourceFormat.store(static_cast<std::uint32_t>(a_bindings.sourceDesc.Format), std::memory_order_relaxed);
			g_historyFormat.store(static_cast<std::uint32_t>(a_bindings.historyDesc.Format), std::memory_order_relaxed);
			g_outputFormat.store(static_cast<std::uint32_t>(a_bindings.submittedDesc.Format), std::memory_order_relaxed);
			g_sourceSrvAddress.store(reinterpret_cast<std::uintptr_t>(a_bindings.source.Get()), std::memory_order_relaxed);
			g_historyRtvAddress.store(reinterpret_cast<std::uintptr_t>(a_bindings.history.Get()), std::memory_order_relaxed);
			g_outputRtvAddress.store(reinterpret_cast<std::uintptr_t>(a_bindings.submitted.Get()), std::memory_order_relaxed);
		}

		void ReportValidationFailure(const Bindings& a_bindings, std::string_view a_mode) noexcept
		{
			const auto failures = g_validationFailures.fetch_add(1, std::memory_order_relaxed) + 1;
			if (failures == 1 || failures == 16 || failures == 128) {
				logger::warn(FMT_STRING("native-TAA {}: skipped binding/descriptor mismatch {} mask {:08X}; parity {}, engine/expected history {}/{}, descriptors {}x{}:{} / {}x{}:{} / {}x{}:{} - vanilla TemporalAA continues"),
					a_mode,
					failures,
					a_bindings.validationMask,
					g_lastParity.load(std::memory_order_relaxed),
					g_engineHistoryLogical.load(std::memory_order_relaxed),
					g_expectedHistoryLogical.load(std::memory_order_relaxed),
					a_bindings.sourceDesc.Width,
					a_bindings.sourceDesc.Height,
					static_cast<std::uint32_t>(a_bindings.sourceDesc.Format),
					a_bindings.historyDesc.Width,
					a_bindings.historyDesc.Height,
					static_cast<std::uint32_t>(a_bindings.historyDesc.Format),
					a_bindings.submittedDesc.Width,
					a_bindings.submittedDesc.Height,
					static_cast<std::uint32_t>(a_bindings.submittedDesc.Format));
			}
		}

		enum class DrawOutcome
		{
			notDrawn,
			succeeded,
			drawnButFailed,
			stateUncertain,
			worldAdmissionVeto,
			faultInjected,
		};

		struct IsolatedDrawResult
		{
			bool began = false;
			bool drawIssued = false;
			bool ended = false;
			bool stateUncertain = false;
			bool worldAdmissionVeto = false;
			bool faultInjected = false;
			std::uint32_t exceptionCode = 0;
		};

		// The pixel shader a seam draw binds, with its constant buffer when it has one.
		// Raw pointers only: it crosses into the SEH-bracketed draw.
		struct SeamProgram
		{
			ID3D11PixelShader* pixelShader = nullptr;
			ID3D11Buffer* constants = nullptr;
			SeamShader shader = SeamShader::kPassthrough;
		};

		// Render thread, outside the isolated scope. The sharpen constant is re-written
		// only when Configure has changed the setting since the last upload - a resource
		// write, not context state, so it needs no isolation and no per-frame cost - and
		// the pixel shader is selected from the same value. Zero, or a sharpen shader
		// that never compiled, selects the passthrough, so the off path binds exactly what
		// it bound before the sharpen existed.
		[[nodiscard]] SeamProgram SelectSeamProgram(ID3D11DeviceContext* a_context) noexcept
		{
			SeamProgram program{};
			program.pixelShader = g_pixelShader.Get();
			const auto generation = g_sharpenGeneration.load(std::memory_order_acquire);
			const float sharpness = SharpenShader::Sanitize(g_sharpnessRequested.load(std::memory_order_acquire));
			const bool available = g_sharpenPixelShader && g_sharpenConstants;
			if (available && generation != g_sharpenUploadedGeneration) {
				const SharpenShader::Constants constants{ SharpenShader::LinearScale(sharpness) };
				a_context->UpdateSubresource(g_sharpenConstants.Get(), 0, nullptr, &constants, 0, 0);
				g_sharpenUploadedGeneration = generation;
			}
			if (available && SharpenShader::UsesRcas(sharpness)) {
				program.pixelShader = g_sharpenPixelShader.Get();
				program.constants = g_sharpenConstants.Get();
				program.shader = SeamShader::kRcas;
			}
			g_sharpenShaderBound.store(static_cast<std::uint32_t>(program.shader), std::memory_order_relaxed);
			return program;
		}

		// Keep this boundary free of destructible C++ locals. A D3D driver SEH must
		// still run End so the exact host context state is restored. drawIssued is set
		// before Draw: an exception from or after that call means real MRT pixels may
		// have changed and publication can no longer be rolled back as a private abort.
		[[nodiscard]] IsolatedDrawResult DrawBindingsIsolated(
			D3D11StateIsolation::Context& a_isolation,
			ID3D11DeviceContext* a_context,
			ID3D11ShaderResourceView* a_source,
			ID3D11RenderTargetView* a_history,
			ID3D11RenderTargetView* a_submitted,
			std::uint32_t a_width,
			std::uint32_t a_height,
			const SeamProgram& a_program,
			const ResetPolicy::WorldAdmissionTicket* a_worldAdmission,
			Telemetry::IsolationMarks a_marks) noexcept
		{
			IsolatedDrawResult result{};
			__try {
				Telemetry::Mark(a_marks.beforeBegin);
				result.began = a_isolation.Begin();
				if (result.began) {
					__try {
						Telemetry::Mark(a_marks.afterBegin);
						ID3D11Buffer* nullVertexBuffer = nullptr;
						constexpr UINT zero = 0;
						a_context->IASetInputLayout(nullptr);
						a_context->IASetVertexBuffers(0, 1, &nullVertexBuffer, &zero, &zero);
						a_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
						a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
						a_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
						a_context->HSSetShader(nullptr, nullptr, 0);
						a_context->DSSetShader(nullptr, nullptr, 0);
						a_context->GSSetShader(nullptr, nullptr, 0);
						a_context->PSSetShader(a_program.pixelShader, nullptr, 0);
						if (a_program.constants) {
							ID3D11Buffer* constants[] = { a_program.constants };
							a_context->PSSetConstantBuffers(0, 1, constants);
						}
						ID3D11ShaderResourceView* source[] = { a_source };
						a_context->PSSetShaderResources(0, 1, source);
						ID3D11RenderTargetView* outputs[] = { a_history, a_submitted };
						a_context->OMSetRenderTargets(2, outputs, nullptr);
						constexpr float blendFactor[4]{};
						a_context->OMSetBlendState(g_blendState.Get(), blendFactor, 0xFFFFFFFFu);
						a_context->OMSetDepthStencilState(g_depthStencilState.Get(), 0);
						a_context->RSSetState(g_rasterizerState.Get());
						D3D11_VIEWPORT viewport{};
						viewport.Width = static_cast<float>(a_width);
						viewport.Height = static_cast<float>(a_height);
						viewport.MaxDepth = 1.0f;
						a_context->RSSetViewports(1, &viewport);
						// All public bindings and private context state are prepared. Keep the
						// admitted world epoch live through this last CPU check; the next D3D
						// operation is the only irreversible history/RT4 write. The prePublish
						// fault is consumed here, after that check and before the draw, so it
						// stands in for a seam refused at exactly this point; the CAS neither
						// logs nor allocates inside the isolated scope.
						if (a_worldAdmission &&
							!ResetPolicy::RevalidateQualityWorldAdmissionForPublish(*a_worldAdmission)) {
							result.worldAdmissionVeto = true;
						} else if (a_worldAdmission &&
								   HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kPrePublish)) {
							result.faultInjected = true;
						} else {
							result.drawIssued = true;
							a_context->Draw(3, 0);
						}

						ID3D11ShaderResourceView* nullSource = nullptr;
						a_context->PSSetShaderResources(0, 1, &nullSource);
						a_context->OMSetRenderTargets(0, nullptr, nullptr);
						Telemetry::Mark(a_marks.beforeEnd);
					} __finally {
						result.ended = a_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_isolation.StateUncertain()) {
				static_cast<void>(a_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			// The real Draw(3,0) and End() above already ran; this only overwrites the
			// flag DrawBindings reads, after the fact, so history and RT4 are genuinely
			// written underneath a reported close failure. Gated on drawIssued: a seam
			// that fired on a frame where the draw was vetoed or never attempted would
			// prove nothing and would be indistinguishable from postTaaStage/prePublish.
			// a_worldAdmission is non-null only for the TemporalAA-seam publish caller
			// (TryBypassImpl's a_publishAtSeam branch) - the plain prepared-draw bypass
			// and the post-draw overwrite both pass none - so its presence is recorded
			// to tell the interesting caller apart from the other two.
			if (result.drawIssued &&
				HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kSeamDrawCloseFailure)) {
				result.ended = false;
				g_seamDrawCloseFailureAtPublish.store(a_worldAdmission != nullptr, std::memory_order_relaxed);
			}
			return result;
		}

		[[nodiscard]] DrawOutcome DrawBindings(
			const Bindings& a_bindings,
			std::string_view a_mode,
			bool* a_publicDrawIssued = nullptr,
			bool* a_publicStateUncertain = nullptr,
			const ResetPolicy::WorldAdmissionTicket* a_worldAdmission = nullptr,
			Telemetry::IsolationMarks a_marks = {}) noexcept
		{
			if (D3D11StateIsolation::HostStateUncertain()) {
				if (a_publicStateUncertain) {
					*a_publicStateUncertain = true;
				}
				Disable("the shared immediate-context state was already unprovable before the private draw"sv, false, true);
				return DrawOutcome::stateUncertain;
			}
			auto* const device = Engine::Device();
			auto* const context = Engine::ImmediateContext();
			g_contextAddress.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
			if (!device || !context) {
				Disable("the engine device or immediate context is null"sv, true, false);
				return DrawOutcome::notDrawn;
			}
			if (!EnsureObjects(device, context)) {
				Disable("private shader/state preparation or context-state isolation initialization failed"sv, false, true);
				return DrawOutcome::notDrawn;
			}

			const auto program = SelectSeamProgram(context);
			const auto draw = DrawBindingsIsolated(
				g_isolation,
				context,
				a_bindings.source.Get(),
				a_bindings.history.Get(),
				a_bindings.submitted.Get(),
				a_bindings.sourceDesc.Width,
				a_bindings.sourceDesc.Height,
				program,
				a_worldAdmission,
				a_marks);
			if (draw.drawIssued && a_publicDrawIssued) {
				// Publish this fact before any post-draw COM health query or logging.
				// The outer transaction must never mistake an issued MRT draw for a
				// private, rollback-safe abort if one of those later operations SEHs.
				*a_publicDrawIssued = true;
			}
			if (draw.stateUncertain) {
				if (a_publicStateUncertain) {
					*a_publicStateUncertain = true;
				}
				g_lastError.store(static_cast<std::uint32_t>(g_isolation.LastError()), std::memory_order_relaxed);
				Disable("exact shared immediate-context state could not be recovered after context-state activation or handback"sv, false, true);
				return DrawOutcome::stateUncertain;
			}
			if (!draw.began) {
				g_lastError.store(static_cast<std::uint32_t>(g_isolation.LastError()), std::memory_order_relaxed);
				Disable("private D3D11.1 context-state activation failed"sv, false, true);
				return DrawOutcome::notDrawn;
			}
			if (draw.exceptionCode != 0) {
				g_lastError.store(draw.exceptionCode, std::memory_order_relaxed);
				Disable(
					fmt::format(
						FMT_STRING("SEH 0x{:08X} escaped the {} draw boundary {} publication"),
						draw.exceptionCode,
						a_mode,
						draw.drawIssued ? "after possible"sv : "before"sv),
					false,
					true);
				return draw.drawIssued ? DrawOutcome::drawnButFailed : DrawOutcome::notDrawn;
			}
			if (!draw.ended) {
				g_lastError.store(static_cast<std::uint32_t>(g_isolation.LastError()), std::memory_order_relaxed);
				Disable(fmt::format(FMT_STRING("exact host D3D11 state handback failed after the {} draw"), a_mode), false, true);
				return draw.drawIssued ? DrawOutcome::drawnButFailed : DrawOutcome::notDrawn;
			}
			if (draw.worldAdmissionVeto) {
				return DrawOutcome::worldAdmissionVeto;
			}
			if (draw.faultInjected) {
				return DrawOutcome::faultInjected;
			}
			if (!draw.drawIssued) {
				return DrawOutcome::notDrawn;
			}

			const HRESULT removed = device->GetDeviceRemovedReason();
			g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
			if (FAILED(removed)) {
				g_lastError.store(static_cast<std::uint32_t>(removed), std::memory_order_relaxed);
				Disable(fmt::format(FMT_STRING("the D3D11 device reported removal after the {} draw"), a_mode), false, false);
				return DrawOutcome::drawnButFailed;
			}
			return DrawOutcome::succeeded;
		}

		void RecordSuccess(
			std::int32_t a_historyLogical,
			std::uint32_t a_width,
			std::uint32_t a_height,
			bool a_bypass) noexcept
		{
			const auto successes = g_successes.fetch_add(1, std::memory_order_relaxed) + 1;
			const auto targetSuccesses = g_targetSuccesses.load(std::memory_order_acquire);
			if (a_historyLogical == Addr::kRT_Taa) {
				g_history61Writes.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_history62Writes.fetch_add(1, std::memory_order_relaxed);
			}
			if (a_bypass) {
				g_nativeDrawBypasses.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_postDrawOverwrites.fetch_add(1, std::memory_order_relaxed);
			}
			if (successes == 1 || successes == 16 || successes == 128 ||
				successes == 256 || successes == targetSuccesses) {
				logger::info(FMT_STRING("native-TAA replacement: accepted format-converting {} {}/{} at {}x{}; outer vanilla cleanup continues"),
					a_bypass ? "prepared-draw bypass"sv : "post-draw MRT overwrite"sv,
					successes,
					targetSuccesses,
					a_width,
					a_height);
			}
			if (successes >= targetSuccesses) {
				g_completed.store(true, std::memory_order_release);
				logger::info(FMT_STRING("native-TAA replacement: bounded {}-frame {} gate completed and is inert; fully native TemporalAA resumes on the next frame"),
					targetSuccesses,
					a_bypass ? "bypass"sv : "overwrite"sv);
			}
		}
	}

	void AbortConfiguration() noexcept
	{
		g_postDrawEnabled.store(false, std::memory_order_release);
		g_bypassEnabled.store(false, std::memory_order_release);
		g_enabled.store(false, std::memory_order_release);
		g_requireNgxHandback.store(false, std::memory_order_release);
		g_requireFfxHandback.store(false, std::memory_order_release);
		g_handbackSource.store(
			static_cast<std::uint32_t>(HDRRoundTrip::HandbackSource::kNone),
			std::memory_order_release);
		static_cast<void>(DisableSessionOnce("an interrupted settings transaction aborted configuration before it completed"sv));

		auto epoch = g_configEpoch.load(std::memory_order_seq_cst);
		while ((epoch & 1u) != 0 &&
		       !g_configEpoch.compare_exchange_weak(
			       epoch, epoch + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
		}
	}

	[[nodiscard]] bool ConfigureImpl(
		bool a_postDrawEnabled,
		bool a_bypassEnabled,
		bool a_requireNgxHandback,
		bool a_requireFfxHandback,
		std::uint64_t a_targetSuccesses,
		float a_sharpness) noexcept
	{
		bool postDrawEnabled = a_postDrawEnabled;
		bool bypassEnabled = a_bypassEnabled;
		bool enabled = postDrawEnabled || bypassEnabled;
		if (enabled && a_requireNgxHandback && a_requireFfxHandback) {
			logger::error("native-TAA replacement: NGX and FFX handback sources were both requested; disabling the diagnostic instead of admitting an ambiguous frame"sv);
			postDrawEnabled = false;
			bypassEnabled = false;
			enabled = false;
		}

		const auto source = !enabled ? HDRRoundTrip::HandbackSource::kNone :
		                    a_requireNgxHandback ? HDRRoundTrip::HandbackSource::kNgx :
		                    a_requireFfxHandback ? HDRRoundTrip::HandbackSource::kFfx :
		                                              HDRRoundTrip::HandbackSource::kNone;
		const bool wasEnabled = g_enabled.load(std::memory_order_acquire);
		const bool wasPostDrawEnabled = g_postDrawEnabled.load(std::memory_order_acquire);
		const bool wasBypassEnabled = g_bypassEnabled.load(std::memory_order_acquire);
		const auto previousSource = static_cast<HDRRoundTrip::HandbackSource>(
			g_handbackSource.load(std::memory_order_acquire));
		const bool newEpisode = enabled &&
		                        (!wasEnabled || wasPostDrawEnabled != postDrawEnabled ||
								 wasBypassEnabled != bypassEnabled || previousSource != source);

		// Close both render-thread entry points, then wait only on this configuration
		// thread until any operation admitted under the old epoch has returned. Render
		// callbacks arriving during the odd epoch immediately preserve vanilla TAA.
		g_configEpoch.fetch_add(1, std::memory_order_seq_cst);
		g_postDrawEnabled.store(false, std::memory_order_release);
		g_bypassEnabled.store(false, std::memory_order_release);
		g_enabled.store(false, std::memory_order_release);
		while (g_activeRenderEntries.load(std::memory_order_seq_cst) != 0) {
			std::this_thread::yield();
		}
		// Not part of the episode: the sharpen only changes which pixel shader the seam
		// binds and one constant, uploaded by the render thread when it next draws. The
		// generation starts at zero so the first configuration always publishes one.
		{
			const float sharpness = SharpenShader::Sanitize(a_sharpness);
			const float previous = g_sharpnessRequested.exchange(sharpness, std::memory_order_acq_rel);
			if (previous != sharpness || g_sharpenGeneration.load(std::memory_order_acquire) == 0) {
				g_sharpenGeneration.fetch_add(1, std::memory_order_acq_rel);
				if (SharpenShader::UsesRcas(sharpness)) {
					logger::info(FMT_STRING("native-TAA replacement: sharpen: rcas, sharpness {:.3f}, {:.1f} stops"),
						sharpness,
						SharpenShader::Stops(sharpness));
				} else {
					logger::info("native-TAA replacement: sharpen: off"sv);
				}
			}
		}
		const auto requestedTarget = a_targetSuccesses == 0 ? kDefaultTargetSuccesses : a_targetSuccesses;
		if (g_attempts.load(std::memory_order_acquire) == 0) {
			g_targetSuccesses.store(requestedTarget, std::memory_order_release);
		} else {
			const auto latchedTarget = g_targetSuccesses.load(std::memory_order_acquire);
			if (latchedTarget != requestedTarget) {
				logger::info(FMT_STRING("native-TAA replacement: target remains latched at {}; requested live change to {} is ignored after the episode began"),
					latchedTarget,
					requestedTarget);
			}
		}
		if (newEpisode) {
			const auto baseline = HDRRoundTrip::HandbackCandidateSerial(source);
			g_handbackBaseline.store(baseline, std::memory_order_relaxed);
			g_lastHandbackSerial.store(baseline, std::memory_order_relaxed);
			if (source != HDRRoundTrip::HandbackSource::kNone) {
				logger::info(FMT_STRING("native-TAA replacement: armed {} handback episode at accepted baseline {}; first admissible serial {}"),
					HDRRoundTrip::HandbackSourceName(source),
					baseline,
					baseline + 1);
			}
		} else if (!enabled) {
			g_handbackBaseline.store(0, std::memory_order_relaxed);
			g_lastHandbackSerial.store(0, std::memory_order_relaxed);
		}
		g_requireNgxHandback.store(source == HDRRoundTrip::HandbackSource::kNgx, std::memory_order_relaxed);
		g_requireFfxHandback.store(source == HDRRoundTrip::HandbackSource::kFfx, std::memory_order_relaxed);
		g_handbackSource.store(static_cast<std::uint32_t>(source), std::memory_order_relaxed);
		g_postDrawEnabled.store(postDrawEnabled, std::memory_order_release);
		g_bypassEnabled.store(bypassEnabled, std::memory_order_release);
		g_enabled.store(enabled, std::memory_order_release);
		g_configEpoch.fetch_add(1, std::memory_order_seq_cst);
		return true;
	}

	[[nodiscard]] bool ConfigureImplNoexcept(
		bool a_postDrawEnabled,
		bool a_bypassEnabled,
		bool a_requireNgxHandback,
		bool a_requireFfxHandback,
		std::uint64_t a_targetSuccesses,
		float a_sharpness) noexcept
	{
		try {
			return ConfigureImpl(
				a_postDrawEnabled,
				a_bypassEnabled,
				a_requireNgxHandback,
				a_requireFfxHandback,
				a_targetSuccesses,
				a_sharpness);
		} catch (...) {
			return false;
		}
	}

	[[nodiscard]] bool AcquireConfigurationLock() noexcept
	{
		try {
			g_configLock.lock();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool Configure(
		bool a_postDrawEnabled,
		bool a_bypassEnabled,
		bool a_requireNgxHandback,
		bool a_requireFfxHandback,
		std::uint64_t a_targetSuccesses,
		float a_sharpness) noexcept
	{
		// The terminal check precedes lock acquisition so a previous interrupted
		// build can never wait on suspect state.  The POD finally below makes the
		// current lock acquisition safe against driver/hardware SEH as well.
		if (g_sessionDisabled.load(std::memory_order_acquire)) {
			AbortConfiguration();
			return false;
		}

		struct ConfigureOperation
		{
			bool locked = false;
			bool returned = false;
			bool result = false;
		};
		ConfigureOperation operation{};
		__try {
			__try {
				if (AcquireConfigurationLock()) {
					operation.locked = true;
					operation.result = !g_sessionDisabled.load(std::memory_order_acquire) &&
					                   ConfigureImplNoexcept(
						                   a_postDrawEnabled,
						                   a_bypassEnabled,
						                   a_requireNgxHandback,
						                   a_requireFfxHandback,
						                   a_targetSuccesses,
						                   a_sharpness);
					operation.returned = true;
				}
			} __finally {
				if (operation.locked) {
					g_configLock.unlock();
					operation.locked = false;
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			AbortConfiguration();
		}
		if (!operation.returned || !operation.result) {
			AbortConfiguration();
			return false;
		}
		return true;
	}

	std::uint64_t PrepareQualityBypassImpl(std::uint64_t a_configEpoch) noexcept
	{
		if (g_sessionDisabled.load(std::memory_order_acquire) ||
			!g_enabled.load(std::memory_order_acquire) ||
			!g_requireNgxHandback.load(std::memory_order_acquire) ||
			g_requireFfxHandback.load(std::memory_order_acquire) ||
			g_postDrawEnabled.load(std::memory_order_acquire) ||
			static_cast<HDRRoundTrip::HandbackSource>(g_handbackSource.load(std::memory_order_acquire)) !=
				HDRRoundTrip::HandbackSource::kNgx ||
			g_pendingQualitySuccess.active ||
			ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
			return 0;
		}

		auto* const device = Engine::Device();
		auto* const context = Engine::ImmediateContext();
		g_contextAddress.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
		if (!device || !context || !EnsureObjects(device, context)) {
			return 0;
		}
		D3D11_TEXTURE2D_DESC submittedDesc{};
		if (!ValidateQualityPreflightTargets(submittedDesc)) {
			return 0;
		}
		std::uint32_t parity = 0;
		if (!ReadHistoryParity(parity, false)) {
			return 0;
		}
		const HRESULT removed = device->GetDeviceRemovedReason();
		g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
		if (FAILED(removed) || !ArmQualityHistoryRepair(parity, a_configEpoch, submittedDesc)) {
			return 0;
		}
		return a_configEpoch;
	}

	std::uint64_t PrepareQualityBypass() noexcept
	{
		auto renderEntry = EnterRenderEntry(g_bypassEnabled);
		if (!renderEntry.entered) {
			return 0;
		}
		std::uint64_t result = 0;
		__try {
			result = PrepareQualityBypassImpl(renderEntry.epoch);
		} __finally {
			LeaveRenderEntry(renderEntry);
		}
		return result;
	}

	DrawResult TryBypassImpl(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback,
		bool a_publishAtSeam,
		std::uint64_t a_qualityConfigEpoch,
		ResetPolicy::WorldAdmissionTicket a_worldAdmission,
		std::uint64_t a_renderConfigEpoch) noexcept
	{
		if (ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
			return {};
		}
		if (a_publishAtSeam &&
			(a_qualityConfigEpoch == 0 || a_renderConfigEpoch != a_qualityConfigEpoch ||
			 !g_enabled.load(std::memory_order_acquire) ||
			 !g_requireNgxHandback.load(std::memory_order_acquire) ||
			 g_requireFfxHandback.load(std::memory_order_acquire) ||
			 g_postDrawEnabled.load(std::memory_order_acquire) ||
				 static_cast<HDRRoundTrip::HandbackSource>(g_handbackSource.load(std::memory_order_acquire)) !=
					 HDRRoundTrip::HandbackSource::kNgx)) {
			if (g_pendingQualitySuccess.active) {
				g_pendingQualitySuccess.repairBothHistories = true;
			}
			return {};
		}
		if (a_publishAtSeam &&
			HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kPostTaaStage)) {
			// The DevBench stand-in for a seam path lost before the draw is attempted: no
			// attempt is counted, no serial consumed, and the caller runs the native draw.
			DrawResult injected{};
			injected.faultInjected = true;
			injected.injectedSeam = HDRRoundTrip::QualityFaultSeam::kPostTaaStage;
			return injected;
		}
		if (!BeginAttempt(a_temporalAACall, a_handback)) {
			if (a_publishAtSeam && g_pendingQualitySuccess.active) {
				g_pendingQualitySuccess.repairBothHistories = true;
			}
			return {};
		}

		std::uint32_t parity = 0;
		if (!ReadHistoryParity(parity)) {
			if (a_publishAtSeam && g_pendingQualitySuccess.active) {
				g_pendingQualitySuccess.repairBothHistories = true;
			}
			Disable("the byte-verified TAA parity global did not resolve to 0 or 1"sv, true, false);
			return {};
		}
		bool parityDrifted = false;
		if (a_publishAtSeam && g_pendingQualitySuccess.active) {
			parityDrifted = g_pendingQualitySuccess.parity != parity;
			// DevBench-only: the byte-verified parity above was just read for real and
			// genuinely matched the admitted candidate; only the comparison result is
			// forced to report drift, so the genuine Disable and repair-both-histories
			// branch below can be reached without an actual mismatch. Nothing is mutated -
			// this simulates a REPORT of a validation failure, not a close failure like the
			// copyClose/seamDrawClose/captureNotify seams: no engine call runs differently
			// either way. Consumed only here, where the draw was genuinely about to be
			// attempted (a_publishAtSeam && pending.active), so a fired seam always proves
			// this branch, not an incidental one.
			if (!parityDrifted &&
				HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kParityDriftFailure)) {
				parityDrifted = true;
			}
		}
		if (parityDrifted) {
			g_pendingQualitySuccess.repairBothHistories = true;
			Disable("the native-TAA history parity changed between pre-HDR admission and the owned prepared draw"sv, true, false);
			return {};
		}

		Bindings bindings;
		const bool validBindings = CaptureDirectAndValidate(parity, bindings);
		PublishBindings(bindings);
		if (!validBindings) {
			if (a_publishAtSeam && g_pendingQualitySuccess.active) {
				g_pendingQualitySuccess.repairBothHistories = true;
			}
			ReportValidationFailure(bindings, "bypass"sv);
			return {};
		}

		if (!a_publishAtSeam) {
			const auto outcome = DrawBindings(bindings, "prepared-draw bypass"sv);
			if (outcome == DrawOutcome::succeeded) {
				RecordSuccess(bindings.historyLogical, bindings.sourceDesc.Width, bindings.sourceDesc.Height, true);
			}
			DrawResult result{};
			result.drew = outcome != DrawOutcome::notDrawn;
			result.succeeded = outcome == DrawOutcome::succeeded;
			return result;
		}

		const auto configEpoch = a_renderConfigEpoch;
		const auto& pending = g_pendingQualitySuccess;
		if (!pending.active || !pending.historyRepairRequired || pending.published ||
			(configEpoch & 1u) != 0 || pending.configEpoch != configEpoch ||
			pending.parity != parity || pending.historyLogical != bindings.historyLogical ||
			a_handback.source != HDRRoundTrip::HandbackSource::kNgx || a_handback.serial == 0 ||
			bindings.history.Get() != pending.historyRtv.Get() ||
			bindings.submitted.Get() != pending.submittedRtv.Get() ||
			!SameTextureContract(bindings.historyDesc, pending.historyDesc) ||
			!SameTextureContract(bindings.submittedDesc, pending.submittedDesc)) {
			g_pendingQualitySuccess.repairBothHistories = true;
			return {};
		}
		// Full UI/menu/cell validation runs before entering private D3D state; the
		// isolated draw repeats a lock-free generation check at the exact issue boundary,
		// so no engine lock is held while the immediate context is live.
		if (!a_worldAdmission.admitted ||
			!ResetPolicy::RevalidateQualityWorldAdmission(a_worldAdmission)) {
			DrawResult veto{};
			veto.worldAdmissionVeto = true;
			return veto;
		}
		constexpr Telemetry::IsolationMarks seamMarks{
			Telemetry::MarkPoint::kBeforeSeamPublishIsolationBegin,
			Telemetry::MarkPoint::kAfterSeamPublishIsolationBegin,
			Telemetry::MarkPoint::kBeforeSeamPublishIsolationEnd,
		};
		bool issued = false;
		bool stateUncertain = false;
		const auto outcome = DrawBindings(
			bindings,
			"TemporalAA-seam publish"sv,
			&issued,
			&stateUncertain,
			&a_worldAdmission,
			seamMarks);
		if (issued) {
			g_qualityPublishDraws.fetch_add(1, std::memory_order_relaxed);
		}
		DrawResult result{};
		result.drew = issued;
		if (outcome == DrawOutcome::worldAdmissionVeto) {
			result.worldAdmissionVeto = true;
			return result;
		}
		if (outcome == DrawOutcome::faultInjected) {
			result.faultInjected = true;
			result.injectedSeam = HDRRoundTrip::QualityFaultSeam::kPrePublish;
			return result;
		}
		if (outcome == DrawOutcome::notDrawn) {
			return result;
		}
		if (outcome != DrawOutcome::succeeded) {
			// Public pixels may have changed, or the host state after them cannot be
			// proved. Retain the COM references for process teardown and make every
			// logical route to the episode inert; the caller latches the session.
			g_pendingQualitySuccess.active = false;
			g_pendingQualitySuccess.historyRepairRequired = false;
			g_pendingQualitySuccess.published = false;
			result.issuedUnrecoverable = true;
			return result;
		}
		g_pendingQualitySuccess.published = true;
		g_pendingQualitySuccess.bypass = true;
		g_pendingQualitySuccess.handback = a_handback;
		Telemetry::Mark(Telemetry::MarkPoint::kAfterSeamPublish);
		if (HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kPostPublish)) {
			g_pendingQualitySuccess.active = false;
			g_pendingQualitySuccess.historyRepairRequired = false;
			g_pendingQualitySuccess.published = false;
			result.faultInjected = true;
			result.injectedSeam = HDRRoundTrip::QualityFaultSeam::kPostPublish;
			result.issuedUnrecoverable = true;
			return result;
		}
		result.succeeded = true;
		return result;
	}

	DrawResult TryBypass(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback,
		bool a_publishAtSeam,
		std::uint64_t a_qualityConfigEpoch,
		ResetPolicy::WorldAdmissionTicket a_worldAdmission) noexcept
	{
		auto renderEntry = EnterRenderEntry(g_bypassEnabled);
		if (!renderEntry.entered) {
			return {};
		}
		DrawResult result{};
		__try {
			result = TryBypassImpl(
				a_temporalAACall,
				a_handback,
				a_publishAtSeam,
				a_qualityConfigEpoch,
				a_worldAdmission,
				renderEntry.epoch);
		} __finally {
			LeaveRenderEntry(renderEntry);
		}
		return result;
	}

	DrawResult TryOverwriteImpl(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		if (ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain ||
			!BeginAttempt(a_temporalAACall, a_handback)) {
			return {};
		}

		auto* const context = Engine::ImmediateContext();
		g_contextAddress.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
		if (!context) {
			Disable("the engine immediate context is null"sv, true, false);
			return {};
		}

		Bindings bindings;
		const bool validBindings = CapturePostDrawAndValidate(context, bindings);
		PublishBindings(bindings);
		if (!validBindings) {
			ReportValidationFailure(bindings, "post-draw replacement"sv);
			return {};
		}

		const auto outcome = DrawBindings(bindings, "post-draw overwrite"sv);
		if (outcome == DrawOutcome::succeeded) {
			RecordSuccess(bindings.historyLogical, bindings.sourceDesc.Width, bindings.sourceDesc.Height, false);
		}
		return {
			outcome != DrawOutcome::notDrawn,
			outcome == DrawOutcome::succeeded,
		};
	}

	DrawResult TryOverwrite(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		auto renderEntry = EnterRenderEntry(g_postDrawEnabled);
		if (!renderEntry.entered) {
			return {};
		}
		DrawResult result{};
		__try {
			result = TryOverwriteImpl(a_temporalAACall, a_handback);
		} __finally {
			LeaveRenderEntry(renderEntry);
		}
		return result;
	}

	bool CommitSeamPublish() noexcept
	{
		if (!g_pendingQualitySuccess.active || !g_pendingQualitySuccess.published) {
			return false;
		}
		const auto committed = g_pendingQualitySuccess;
		g_pendingQualitySuccess = {};
		RecordSuccess(
			committed.historyLogical,
			committed.width,
			committed.height,
			committed.bypass);
		return true;
	}

	struct HistoryRepairResult
	{
		bool began = false;
		bool descriptorsValid = false;
		bool selectedCopyIssued = false;
		bool alternateCopyIssued = false;
		bool ended = false;
		bool stateUncertain = false;
		std::uint32_t exceptionCode = 0;
	};

	struct AbortRepairOperation
	{
		HistoryRepairResult repair{};
		AbortResult result = AbortResult::kUnrecoverable;
		bool repairRequired = false;
		bool attemptsCounted = false;
		bool copiesCounted = false;
		bool successCounted = false;
		bool failureCounted = false;
	};

	// POD-only transactional repair for an engine TAA helper that our owned frame
	// suppressed but never published. The parity has already advanced, so the selected
	// output history must receive the retained submitted frame before native TAA can
	// consume it next frame. RT4 itself is intentionally never written here.
	[[nodiscard]] HistoryRepairResult RepairSelectedHistoryIsolated(
		ID3D11DeviceContext* a_context,
		ID3D11Texture2D* a_history,
		ID3D11Texture2D* a_alternateHistory,
		ID3D11Texture2D* a_submitted,
		const D3D11_TEXTURE2D_DESC& a_historyDesc,
		const D3D11_TEXTURE2D_DESC& a_alternateHistoryDesc,
		const D3D11_TEXTURE2D_DESC& a_submittedDesc,
		bool a_repairBothHistories) noexcept
	{
		HistoryRepairResult result{};
		if (!a_context || !a_history || !a_submitted || a_history == a_submitted ||
			(a_repairBothHistories &&
				(!a_alternateHistory || a_alternateHistory == a_history ||
				 a_alternateHistory == a_submitted))) {
			return result;
		}
		__try {
			result.began = g_isolation.Begin();
			if (result.began) {
				__try {
					D3D11_TEXTURE2D_DESC liveHistory{};
					D3D11_TEXTURE2D_DESC liveAlternateHistory{};
					D3D11_TEXTURE2D_DESC liveSubmitted{};
					a_history->GetDesc(&liveHistory);
					if (a_repairBothHistories) {
						a_alternateHistory->GetDesc(&liveAlternateHistory);
					}
					a_submitted->GetDesc(&liveSubmitted);
					result.descriptorsValid =
						SameTextureContract(liveHistory, a_historyDesc) &&
						SameTextureContract(liveSubmitted, a_submittedDesc) &&
						SameTextureContract(liveHistory, liveSubmitted) &&
						liveHistory.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
						(!a_repairBothHistories ||
							(SameTextureContract(liveAlternateHistory, a_alternateHistoryDesc) &&
							 SameTextureContract(liveAlternateHistory, liveSubmitted)));
					if (result.descriptorsValid) {
						a_context->ClearState();
						result.selectedCopyIssued = true;
						a_context->CopyResource(a_history, a_submitted);
						if (a_repairBothHistories) {
							result.alternateCopyIssued = true;
							a_context->CopyResource(a_alternateHistory, a_submitted);
						}
					}
				} __finally {
					result.ended = g_isolation.End();
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
		}
		if (g_isolation.StateUncertain()) {
			static_cast<void>(g_isolation.RecoverUncertainState());
		}
		result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
		return result;
	}

	AbortResult AbortPendingQualityEpisodeImpl(AbortRepairOperation* a_operation) noexcept
	{
		a_operation->repairRequired = g_pendingQualitySuccess.active &&
		                              g_pendingQualitySuccess.historyRepairRequired &&
		                              !g_pendingQualitySuccess.published;
		if (!a_operation->repairRequired) {
			g_pendingQualitySuccess = {};
			a_operation->result = AbortResult::kNoRepairNeeded;
			return a_operation->result;
		}

		a_operation->attemptsCounted = true;
		g_qualityHistoryRepairAttempts.fetch_add(1, std::memory_order_relaxed);
		const auto history = RenderTargets::Lookup(g_pendingQualitySuccess.historyLogical);
		const auto alternateHistory = RenderTargets::Lookup(
			g_pendingQualitySuccess.parity == 0 ? Addr::kRT_Taa : Addr::kRT_TaaAlt);
		const auto submitted = RenderTargets::Lookup(Addr::kRT_Submitted);
		auto* const device = Engine::Device();
		auto* const context = Engine::ImmediateContext();
		const bool alternateReady = !g_pendingQualitySuccess.repairBothHistories ||
			(alternateHistory.valid &&
			 alternateHistory.tex == g_pendingQualitySuccess.alternateHistoryTexture.Get() &&
			 alternateHistory.rtv == g_pendingQualitySuccess.alternateHistoryRtv.Get());
		if (history.valid && submitted.valid && alternateReady &&
			history.tex == g_pendingQualitySuccess.historyTexture.Get() &&
			history.rtv == g_pendingQualitySuccess.historyRtv.Get() &&
			submitted.tex == g_pendingQualitySuccess.submittedTexture.Get() &&
			submitted.rtv == g_pendingQualitySuccess.submittedRtv.Get() &&
			device && context && SUCCEEDED(device->GetDeviceRemovedReason())) {
			a_operation->repair = RepairSelectedHistoryIsolated(
				context,
				g_pendingQualitySuccess.historyTexture.Get(),
				g_pendingQualitySuccess.alternateHistoryTexture.Get(),
				g_pendingQualitySuccess.submittedTexture.Get(),
				g_pendingQualitySuccess.historyDesc,
				g_pendingQualitySuccess.alternateHistoryDesc,
				g_pendingQualitySuccess.submittedDesc,
				g_pendingQualitySuccess.repairBothHistories);
		}
		a_operation->copiesCounted = true;
		g_qualityHistoryRepairCopies.fetch_add(
			static_cast<std::uint64_t>(a_operation->repair.selectedCopyIssued) +
				static_cast<std::uint64_t>(a_operation->repair.alternateCopyIssued),
			std::memory_order_relaxed);
		const bool repaired = a_operation->repair.began && a_operation->repair.descriptorsValid &&
		                      a_operation->repair.selectedCopyIssued &&
		                      (!g_pendingQualitySuccess.repairBothHistories || a_operation->repair.alternateCopyIssued) &&
		                      a_operation->repair.ended && !a_operation->repair.stateUncertain &&
		                      a_operation->repair.exceptionCode == 0 && device &&
		                      SUCCEEDED(device->GetDeviceRemovedReason());
		if (a_operation->repair.stateUncertain) {
			// COM reference release is another unbounded driver call while the shared
			// immediate context is unproved. Remove logical ownership and retain the refs
			// for orderly process teardown.
			g_pendingQualitySuccess.active = false;
			g_pendingQualitySuccess.historyRepairRequired = false;
			g_pendingQualitySuccess.published = false;
		} else {
			g_pendingQualitySuccess = {};
		}
		if (repaired) {
			a_operation->successCounted = true;
			g_qualityHistoryRepairSuccesses.fetch_add(1, std::memory_order_relaxed);
			a_operation->result = AbortResult::kHistoryRepaired;
			return a_operation->result;
		}
		a_operation->failureCounted = true;
		g_qualityHistoryRepairFailures.fetch_add(1, std::memory_order_relaxed);
		g_lastError.store(
			a_operation->repair.exceptionCode != 0 ? a_operation->repair.exceptionCode :
				static_cast<std::uint32_t>(g_isolation.LastError()),
			std::memory_order_relaxed);
		Disable("the suppressed native-TAA frame could not repair its required history target(s) from retained RT4"sv, false, true);
		a_operation->result = AbortResult::kUnrecoverable;
		return a_operation->result;
	}

	AbortResult AbortPendingQualityEpisode() noexcept
	{
		if (D3D11StateIsolation::HostStateUncertain()) {
			const bool repairRequired = g_pendingQualitySuccess.active &&
			                            g_pendingQualitySuccess.historyRepairRequired &&
			                            !g_pendingQualitySuccess.published;
			if (repairRequired) {
				g_qualityHistoryRepairAttempts.fetch_add(1, std::memory_order_relaxed);
				g_qualityHistoryRepairFailures.fetch_add(1, std::memory_order_relaxed);
			}
			g_pendingQualitySuccess.active = false;
			g_pendingQualitySuccess.historyRepairRequired = false;
			g_pendingQualitySuccess.published = false;
			g_isolationFailures.fetch_add(1, std::memory_order_relaxed);
			static_cast<void>(DisableSessionOnce("the shared immediate-context state was already unprovable before the pending quality episode could abort"sv));
			return AbortResult::kUnrecoverable;
		}
		AbortRepairOperation operation{};
		__try {
			operation.result = AbortPendingQualityEpisodeImpl(&operation);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			const auto exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			g_lastError.store(exceptionCode, std::memory_order_relaxed);
			if (operation.repairRequired) {
				if (!operation.attemptsCounted) {
					g_qualityHistoryRepairAttempts.fetch_add(1, std::memory_order_relaxed);
				}
				if (!operation.copiesCounted) {
					g_qualityHistoryRepairCopies.fetch_add(
						static_cast<std::uint64_t>(operation.repair.selectedCopyIssued) +
							static_cast<std::uint64_t>(operation.repair.alternateCopyIssued),
						std::memory_order_relaxed);
				}
				if (!operation.successCounted && !operation.failureCounted) {
					g_qualityHistoryRepairFailures.fetch_add(1, std::memory_order_relaxed);
				}
			}
			// COM references may be the source of the fault. Clear only their logical
			// ownership here and retain the references until orderly process teardown.
			g_pendingQualitySuccess.active = false;
			g_pendingQualitySuccess.historyRepairRequired = false;
			g_pendingQualitySuccess.published = false;
			g_isolationFailures.fetch_add(1, std::memory_order_relaxed);
			static_cast<void>(DisableSessionOnce("SEH escaped while aborting the pending quality episode's history repair"sv));
			// AbortQualityPixels owns the paired reset for every unrecoverable result.
			operation.result = AbortResult::kUnrecoverable;
		}
		return operation.result;
	}

	bool ReleaseStagedQualityForNativeFallback() noexcept
	{
		const bool pending = g_pendingQualitySuccess.active;
		g_pendingQualitySuccess = {};
		if (pending) {
			g_qualityNativeFallbackReleases.fetch_add(1, std::memory_order_relaxed);
		}
		return pending;
	}

	bool ReleasePublishedQualityEpisode() noexcept
	{
		const bool published = g_pendingQualitySuccess.active && g_pendingQualitySuccess.published;
		if (published) {
			g_pendingQualitySuccess = {};
		}
		return published;
	}

	void RecordNativeFallbackDraw(std::uint64_t a_temporalAACall) noexcept
	{
		g_qualityNativeFallbackDraws.fetch_add(1, std::memory_order_relaxed);
		g_lastTemporalAACall.store(a_temporalAACall, std::memory_order_relaxed);
		std::uint32_t parity = 0;
		if (!ReadHistoryParity(parity)) {
			g_qualityNativeFallbackHistoryMismatches.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		// The same parity-to-history mapping the bypass validates: parity 0 writes 62,
		// parity 1 writes 61, and the engine's current target index names the one it
		// actually bound for this draw.
		const std::int32_t expectedHistory = parity == 0 ? Addr::kRT_TaaAlt : Addr::kRT_Taa;
		const auto engineHistory = Engine::CurrentRenderTargetIndex();
		g_expectedHistoryLogical.store(expectedHistory, std::memory_order_relaxed);
		g_engineHistoryLogical.store(engineHistory, std::memory_order_relaxed);
		if (engineHistory != expectedHistory) {
			g_currentRenderTargetMismatches.fetch_add(1, std::memory_order_relaxed);
			g_qualityNativeFallbackHistoryMismatches.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (expectedHistory == Addr::kRT_Taa) {
			g_qualityNativeFallbackHistory61Writes.fetch_add(1, std::memory_order_relaxed);
		} else {
			g_qualityNativeFallbackHistory62Writes.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void DiscardHandbackCandidate(HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		const auto requiredSource = static_cast<HDRRoundTrip::HandbackSource>(
			g_handbackSource.load(std::memory_order_acquire));
		if (a_handback.source != requiredSource || a_handback.serial == 0) {
			return;
		}
		auto previous = g_lastHandbackSerial.load(std::memory_order_relaxed);
		while (previous < a_handback.serial &&
			!g_lastHandbackSerial.compare_exchange_weak(
				previous,
				a_handback.serial,
				std::memory_order_relaxed,
				std::memory_order_relaxed)) {
		}
	}

	bool SessionDisabled() noexcept
	{
		return g_sessionDisabled.load(std::memory_order_acquire);
	}

	const char* SessionDisabledReason() noexcept
	{
		if (!g_sessionDisabledReasonReady.load(std::memory_order_acquire)) {
			return "none";
		}
		return g_sessionDisabledReasonBuffer;
	}

	State Snapshot() noexcept
	{
		const auto currentHandback = HDRRoundTrip::HandbackCandidateStamp();
		State state{};
		state.enabled = g_enabled.load(std::memory_order_acquire);
		state.postDrawEnabled = g_postDrawEnabled.load(std::memory_order_acquire);
		state.bypassEnabled = g_bypassEnabled.load(std::memory_order_acquire);
		state.requireNgxHandback = g_requireNgxHandback.load(std::memory_order_acquire);
		state.requireFfxHandback = g_requireFfxHandback.load(std::memory_order_acquire);
		state.ready = g_ready.load(std::memory_order_acquire);
		state.completed = g_completed.load(std::memory_order_acquire);
		state.sessionDisabled = g_sessionDisabled.load(std::memory_order_acquire);
		state.handbackSource = static_cast<HDRRoundTrip::HandbackSource>(
			g_handbackSource.load(std::memory_order_acquire));
		state.currentHandbackSource = currentHandback.source;
		state.attempts = g_attempts.load(std::memory_order_relaxed);
		state.successes = g_successes.load(std::memory_order_relaxed);
		state.noHandbackSkips = g_noHandbackSkips.load(std::memory_order_relaxed);
		state.handbackSourceMismatches = g_handbackSourceMismatches.load(std::memory_order_relaxed);
		state.handbackSequenceGaps = g_handbackSequenceGaps.load(std::memory_order_relaxed);
		state.validationFailures = g_validationFailures.load(std::memory_order_relaxed);
		state.isolationFailures = g_isolationFailures.load(std::memory_order_relaxed);
		state.validationMask = g_validationMask.load(std::memory_order_relaxed);
		state.shaderBuilds = g_shaderBuilds.load(std::memory_order_relaxed);
		state.history61Writes = g_history61Writes.load(std::memory_order_relaxed);
		state.history62Writes = g_history62Writes.load(std::memory_order_relaxed);
		state.postDrawOverwrites = g_postDrawOverwrites.load(std::memory_order_relaxed);
		state.nativeDrawBypasses = g_nativeDrawBypasses.load(std::memory_order_relaxed);
		state.qualityPublishDraws = g_qualityPublishDraws.load(std::memory_order_relaxed);
		state.qualityHistoryRepairArms = g_qualityHistoryRepairArms.load(std::memory_order_relaxed);
		state.qualityHistoryRepairAttempts = g_qualityHistoryRepairAttempts.load(std::memory_order_relaxed);
		state.qualityHistoryRepairCopies = g_qualityHistoryRepairCopies.load(std::memory_order_relaxed);
		state.qualityHistoryRepairSuccesses = g_qualityHistoryRepairSuccesses.load(std::memory_order_relaxed);
		state.qualityHistoryRepairFailures = g_qualityHistoryRepairFailures.load(std::memory_order_relaxed);
		state.qualityNativeFallbackReleases = g_qualityNativeFallbackReleases.load(std::memory_order_relaxed);
		state.qualityNativeFallbackDraws = g_qualityNativeFallbackDraws.load(std::memory_order_relaxed);
		state.qualityNativeFallbackHistory61Writes = g_qualityNativeFallbackHistory61Writes.load(std::memory_order_relaxed);
		state.qualityNativeFallbackHistory62Writes = g_qualityNativeFallbackHistory62Writes.load(std::memory_order_relaxed);
		state.qualityNativeFallbackHistoryMismatches = g_qualityNativeFallbackHistoryMismatches.load(std::memory_order_relaxed);
		state.parityReadAttempts = g_parityReadAttempts.load(std::memory_order_relaxed);
		state.parityReadFailures = g_parityReadFailures.load(std::memory_order_relaxed);
		state.parityInvalidValues = g_parityInvalidValues.load(std::memory_order_relaxed);
		state.currentRenderTargetMismatches = g_currentRenderTargetMismatches.load(std::memory_order_relaxed);
		state.targetSuccesses = g_targetSuccesses.load(std::memory_order_relaxed);
		state.lastTemporalAACall = g_lastTemporalAACall.load(std::memory_order_relaxed);
		state.handbackBaseline = g_handbackBaseline.load(std::memory_order_relaxed);
		state.lastHandbackSerial = g_lastHandbackSerial.load(std::memory_order_relaxed);
		state.currentHandbackSerial = currentHandback.serial;
		state.width = g_width.load(std::memory_order_relaxed);
		state.height = g_height.load(std::memory_order_relaxed);
		state.sourceFormat = g_sourceFormat.load(std::memory_order_relaxed);
		state.historyFormat = g_historyFormat.load(std::memory_order_relaxed);
		state.outputFormat = g_outputFormat.load(std::memory_order_relaxed);
		state.lastParity = g_lastParity.load(std::memory_order_relaxed);
		state.expectedHistoryLogical = g_expectedHistoryLogical.load(std::memory_order_relaxed);
		state.engineHistoryLogical = g_engineHistoryLogical.load(std::memory_order_relaxed);
		state.deviceRemovedReason = g_deviceRemovedReason.load(std::memory_order_relaxed);
		state.lastError = g_lastError.load(std::memory_order_relaxed);
		state.context = g_contextAddress.load(std::memory_order_relaxed);
		state.sourceSrv = g_sourceSrvAddress.load(std::memory_order_relaxed);
		state.historyRtv = g_historyRtvAddress.load(std::memory_order_relaxed);
		state.outputRtv = g_outputRtvAddress.load(std::memory_order_relaxed);
		state.parityAddress = g_parityAddress.load(std::memory_order_relaxed);
		state.qualitySeamDrawCloseFailureAtPublish =
			g_seamDrawCloseFailureAtPublish.load(std::memory_order_relaxed);
		state.sharpnessRequested = g_sharpnessRequested.load(std::memory_order_acquire);
		state.sharpenCompileFailures = g_sharpenCompileFailures.load(std::memory_order_relaxed);
		// The sharpen objects are written once, before g_ready's release store, and never
		// reset, so the acquire load into state.ready above orders this read on any thread.
		// Effective is what the seam can draw with: zero until the objects exist.
		const bool sharpenAvailable = state.ready && g_sharpenPixelShader && g_sharpenConstants;
		state.sharpnessEffective = sharpenAvailable && state.sharpenCompileFailures == 0 ? state.sharpnessRequested : 0.0f;
		state.sharpenStops = SharpenShader::UsesRcas(state.sharpnessEffective) ? SharpenShader::Stops(state.sharpnessEffective) : 0.0f;
		state.sharpenShader = static_cast<SeamShader>(g_sharpenShaderBound.load(std::memory_order_relaxed));
		state.sharpenLastHr = g_sharpenLastHr.load(std::memory_order_relaxed);
		return state;
	}
}
