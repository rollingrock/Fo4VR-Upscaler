#include "Upscaler/TAAReplacement.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/Engine.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/TAAPassthroughShader.h"
#include "Upscaler/Telemetry.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
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
		std::atomic<std::uint64_t> g_qualityStageDraws{ 0 };
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

		D3D11StateIsolation::Context g_isolation;
		ComPtr<ID3D11Device> g_device;
		ComPtr<ID3D11VertexShader> g_vertexShader;
		ComPtr<ID3D11PixelShader> g_pixelShader;
		ComPtr<ID3D11BlendState> g_blendState;
		ComPtr<ID3D11RasterizerState> g_rasterizerState;
		ComPtr<ID3D11DepthStencilState> g_depthStencilState;
		ComPtr<ID3D11Texture2D> g_qualityStage;
		ComPtr<ID3D11RenderTargetView> g_qualityStageRtv;
		ComPtr<ID3D11ShaderResourceView> g_qualityStageSrv;
		D3D11_TEXTURE2D_DESC g_qualityStageDesc{};
		HMODULE g_compilerModule = nullptr;
		D3DCompileFn g_compile = nullptr;

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

		struct PendingQualitySuccess
		{
			bool active = false;
			bool historyRepairRequired = false;
			bool repairBothHistories = false;
			bool stageSucceeded = false;
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

		[[nodiscard]] bool CompileShader(
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
				g_lastError.store(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND), std::memory_order_relaxed);
				logger::error("native-TAA replacement: d3dcompiler_47.dll / D3DCompile unavailable"sv);
				return false;
			}

			ComPtr<ID3DBlob> errors;
			const HRESULT hr = g_compile(
				TAAPassthroughShader::kSource,
				sizeof(TAAPassthroughShader::kSource) - 1,
				"Fo4VRTaaReplacement",
				nullptr,
				nullptr,
				a_entry,
				a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
				0,
				a_out,
				errors.GetAddressOf());
			g_lastError.store(static_cast<std::uint32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr)) {
				logger::error(
					FMT_STRING("native-TAA replacement: {} compile failed, HRESULT {:08X}: {}"),
					a_entry,
					static_cast<std::uint32_t>(hr),
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler message");
				return false;
			}
			return true;
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
			if (!CompileShader("VSMain", "vs_5_0", vertexCode.GetAddressOf()) ||
				!CompileShader("PSMain", "ps_5_0", pixelCode.GetAddressOf())) {
				return false;
			}

			ComPtr<ID3D11VertexShader> vertexShader;
			ComPtr<ID3D11PixelShader> pixelShader;
			ComPtr<ID3D11BlendState> blendState;
			ComPtr<ID3D11RasterizerState> rasterizerState;
			ComPtr<ID3D11DepthStencilState> depthStencilState;
			HRESULT hr = a_device->CreateVertexShader(
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

			g_device = a_device;
			g_vertexShader = std::move(vertexShader);
			g_pixelShader = std::move(pixelShader);
			g_blendState = std::move(blendState);
			g_rasterizerState = std::move(rasterizerState);
			g_depthStencilState = std::move(depthStencilState);
			g_shaderBuilds.fetch_add(1, std::memory_order_relaxed);
			g_ready.store(true, std::memory_order_release);
			logger::info("native-TAA replacement: private format-converting MRT shader and exact D3D11.1 state isolation are ready"sv);
			return true;
		}

		[[nodiscard]] bool EnsureQualityStage(
			ID3D11Device* a_device,
			const D3D11_TEXTURE2D_DESC& a_submittedDesc) noexcept
		{
			if (!a_device || a_submittedDesc.Width == 0 || a_submittedDesc.Height == 0 ||
				a_submittedDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
				a_submittedDesc.MipLevels != 1 || a_submittedDesc.ArraySize != 1 ||
				a_submittedDesc.SampleDesc.Count != 1) {
				return false;
			}

			if (g_qualityStage && g_qualityStageRtv && g_qualityStageSrv &&
				SameTextureContract(g_qualityStageDesc, a_submittedDesc)) {
				return true;
			}

			D3D11_TEXTURE2D_DESC stageDesc = a_submittedDesc;
			stageDesc.Usage = D3D11_USAGE_DEFAULT;
			stageDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			stageDesc.CPUAccessFlags = 0;
			stageDesc.MiscFlags = 0;
			ComPtr<ID3D11Texture2D> stage;
			ComPtr<ID3D11RenderTargetView> rtv;
			ComPtr<ID3D11ShaderResourceView> srv;
			HRESULT hr = a_device->CreateTexture2D(&stageDesc, nullptr, stage.GetAddressOf());
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateRenderTargetView(stage.Get(), nullptr, rtv.GetAddressOf());
			}
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateShaderResourceView(stage.Get(), nullptr, srv.GetAddressOf());
			}
			g_lastError.store(static_cast<std::uint32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr) || !stage || !rtv || !srv) {
				return false;
			}

			g_qualityStage = std::move(stage);
			g_qualityStageRtv = std::move(rtv);
			g_qualityStageSrv = std::move(srv);
			g_qualityStageDesc = stageDesc;
			logger::info(FMT_STRING("native-TAA replacement: private transactional RGBA8 stage ready at {}x{}"),
				stageDesc.Width,
				stageDesc.Height);
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
				!SameTextureContract(alternateHistoryDesc, a_submittedDesc) ||
				g_qualityStage.Get() == history.tex ||
				g_qualityStage.Get() == alternateHistory.tex ||
				g_qualityStage.Get() == submitted.tex) {
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
			if (!g_sessionDisabled.exchange(true, std::memory_order_acq_rel)) {
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
		};

		struct IsolatedDrawResult
		{
			bool began = false;
			bool drawIssued = false;
			bool ended = false;
			bool stateUncertain = false;
			bool worldAdmissionVeto = false;
			std::uint32_t exceptionCode = 0;
		};

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
			ID3D11RenderTargetView* a_privateStage,
			std::uint32_t a_width,
			std::uint32_t a_height,
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
						a_context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
						ID3D11ShaderResourceView* source[] = { a_source };
						a_context->PSSetShaderResources(0, 1, source);
						ID3D11RenderTargetView* outputs[] = { a_history, a_submitted };
						if (a_privateStage) {
							a_context->OMSetRenderTargets(1, &a_privateStage, nullptr);
						} else {
							a_context->OMSetRenderTargets(2, outputs, nullptr);
						}
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
						// operation is the only irreversible history/RT4 write.
						if (a_worldAdmission &&
							!ResetPolicy::RevalidateQualityWorldAdmissionForPublish(*a_worldAdmission)) {
							result.worldAdmissionVeto = true;
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
			return result;
		}

		[[nodiscard]] DrawOutcome DrawBindings(
			const Bindings& a_bindings,
			std::string_view a_mode,
			ID3D11RenderTargetView* a_privateStage = nullptr,
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

			const auto draw = DrawBindingsIsolated(
				g_isolation,
				context,
				a_bindings.source.Get(),
				a_bindings.history.Get(),
				a_bindings.submitted.Get(),
				a_privateStage,
				a_bindings.sourceDesc.Width,
				a_bindings.sourceDesc.Height,
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
		g_sessionDisabled.store(true, std::memory_order_release);

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
		std::uint64_t a_targetSuccesses) noexcept
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
		std::uint64_t a_targetSuccesses) noexcept
	{
		try {
			return ConfigureImpl(
				a_postDrawEnabled,
				a_bypassEnabled,
				a_requireNgxHandback,
				a_requireFfxHandback,
				a_targetSuccesses);
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
		std::uint64_t a_targetSuccesses) noexcept
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
						                   a_targetSuccesses);
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
		if (!ValidateQualityPreflightTargets(submittedDesc) ||
			!EnsureQualityStage(device, submittedDesc)) {
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
		bool a_deferSuccess,
		std::uint64_t a_qualityConfigEpoch,
		std::uint64_t a_renderConfigEpoch) noexcept
	{
		if (ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
			return {};
		}
		if (a_deferSuccess &&
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
		if (!BeginAttempt(a_temporalAACall, a_handback)) {
			if (a_deferSuccess && g_pendingQualitySuccess.active) {
				g_pendingQualitySuccess.repairBothHistories = true;
			}
			return {};
		}

		std::uint32_t parity = 0;
		if (!ReadHistoryParity(parity)) {
			if (a_deferSuccess && g_pendingQualitySuccess.active) {
				g_pendingQualitySuccess.repairBothHistories = true;
			}
			Disable("the byte-verified TAA parity global did not resolve to 0 or 1"sv, true, false);
			return {};
		}
		if (a_deferSuccess && g_pendingQualitySuccess.active &&
			g_pendingQualitySuccess.parity != parity) {
			g_pendingQualitySuccess.repairBothHistories = true;
			Disable("the native-TAA history parity changed between pre-HDR admission and the owned prepared draw"sv, true, false);
			return {};
		}

		Bindings bindings;
		const bool validBindings = CaptureDirectAndValidate(parity, bindings);
		PublishBindings(bindings);
		if (!validBindings) {
			if (a_deferSuccess && g_pendingQualitySuccess.active) {
				g_pendingQualitySuccess.repairBothHistories = true;
			}
			ReportValidationFailure(bindings, "bypass"sv);
			return {};
		}

		DrawOutcome outcome = DrawOutcome::notDrawn;
		const auto configEpoch = a_renderConfigEpoch;
		if (a_deferSuccess) {
			const auto& pending = g_pendingQualitySuccess;
			if (!pending.active || !pending.historyRepairRequired || pending.stageSucceeded ||
				(configEpoch & 1u) != 0 || pending.configEpoch != configEpoch ||
				pending.parity != parity || pending.historyLogical != bindings.historyLogical ||
				a_handback.source != HDRRoundTrip::HandbackSource::kNgx || a_handback.serial == 0 ||
				!EnsureQualityStage(Engine::Device(), bindings.submittedDesc) ||
				bindings.history.Get() != pending.historyRtv.Get() ||
				bindings.submitted.Get() != pending.submittedRtv.Get() ||
				!SameTextureContract(bindings.historyDesc, pending.historyDesc) ||
				!SameTextureContract(bindings.submittedDesc, pending.submittedDesc) ||
				g_qualityStage.Get() == pending.historyTexture.Get() ||
				g_qualityStage.Get() == pending.submittedTexture.Get()) {
				g_pendingQualitySuccess.repairBothHistories = true;
				return {};
			}
			constexpr Telemetry::IsolationMarks stagedMarks{
				Telemetry::MarkPoint::kBeforeStagedTaaIsolationBegin,
				Telemetry::MarkPoint::kAfterStagedTaaIsolationBegin,
				Telemetry::MarkPoint::kBeforeStagedTaaIsolationEnd,
			};
			outcome = DrawBindings(
				bindings,
				"transactional prepared-draw stage"sv,
				g_qualityStageRtv.Get(),
				nullptr,
				nullptr,
				nullptr,
				stagedMarks);
			if (outcome == DrawOutcome::succeeded || outcome == DrawOutcome::drawnButFailed) {
				g_qualityStageDraws.fetch_add(1, std::memory_order_relaxed);
			}
		} else {
			outcome = DrawBindings(bindings, "prepared-draw bypass"sv);
		}
		if (outcome == DrawOutcome::succeeded) {
			if (a_deferSuccess) {
				g_pendingQualitySuccess.stageSucceeded = true;
				g_pendingQualitySuccess.bypass = true;
				g_pendingQualitySuccess.handback = a_handback;
				Telemetry::Mark(Telemetry::MarkPoint::kAfterStagedTaa);
				if (HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kPostTaaStage)) {
					return { true, false, true };
				}
			} else {
				RecordSuccess(bindings.historyLogical, bindings.sourceDesc.Width, bindings.sourceDesc.Height, true);
			}
		}
		return {
			outcome != DrawOutcome::notDrawn,
			outcome == DrawOutcome::succeeded,
		};
	}

	DrawResult TryBypass(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback,
		bool a_deferSuccess,
		std::uint64_t a_qualityConfigEpoch) noexcept
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
				a_deferSuccess,
				a_qualityConfigEpoch,
				renderEntry.epoch);
		} __finally {
			LeaveRenderEntry(renderEntry);
		}
		return result;
	}

	DrawResult TryOverwriteImpl(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback,
		bool a_deferSuccess) noexcept
	{
		// Once vanilla TemporalAA has drawn, its real history/submitted outputs are already
		// dirty. Reduced NGX therefore admits only the pre-draw staged bypass path.
		if (a_deferSuccess) {
			return {};
		}
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
		HDRRoundTrip::HandbackStamp a_handback,
		bool a_deferSuccess) noexcept
	{
		// Preserve the old fast rejection before admission when this path is
		// categorically unavailable to the reduced-NGX transaction.
		if (a_deferSuccess) {
			return {};
		}
		auto renderEntry = EnterRenderEntry(g_postDrawEnabled);
		if (!renderEntry.entered) {
			return {};
		}
		DrawResult result{};
		__try {
			result = TryOverwriteImpl(a_temporalAACall, a_handback, false);
		} __finally {
			LeaveRenderEntry(renderEntry);
		}
		return result;
	}

	PublishResult PublishStagedQualitySuccessImpl(
		HDRRoundTrip::HandbackStamp a_handback,
		ResetPolicy::WorldAdmissionTicket a_worldAdmission,
		std::uint64_t a_configEpoch,
		bool* a_publicationIssued,
		bool* a_publicationStateUncertain,
		bool* a_publicationCounted) noexcept
	{
		if (!g_enabled.load(std::memory_order_acquire) ||
			!g_requireNgxHandback.load(std::memory_order_acquire) ||
			g_requireFfxHandback.load(std::memory_order_acquire) ||
			g_postDrawEnabled.load(std::memory_order_acquire) ||
			static_cast<HDRRoundTrip::HandbackSource>(g_handbackSource.load(std::memory_order_acquire)) !=
				HDRRoundTrip::HandbackSource::kNgx) {
			return PublishResult::kNotIssued;
		}
		const auto& pending = g_pendingQualitySuccess;
		if (!pending.active || !pending.historyRepairRequired || !pending.stageSucceeded) {
			return PublishResult::kNotIssued;
		}
		const auto history = RenderTargets::Lookup(pending.historyLogical);
		const auto submitted = RenderTargets::Lookup(Addr::kRT_Submitted);
		D3D11_TEXTURE2D_DESC liveHistoryDesc{};
		D3D11_TEXTURE2D_DESC liveSubmittedDesc{};
		if (a_handback.source != pending.handback.source ||
			a_handback.serial != pending.handback.serial || a_handback.serial == 0 ||
			(a_configEpoch & 1u) != 0 || a_configEpoch != pending.configEpoch ||
			!g_qualityStage || !g_qualityStageRtv || !g_qualityStageSrv ||
			g_qualityStage.Get() == pending.historyTexture.Get() ||
			g_qualityStage.Get() == pending.submittedTexture.Get() ||
			pending.historyTexture.Get() == pending.submittedTexture.Get() ||
			!history.valid || history.tex != pending.historyTexture.Get() ||
			history.rtv != pending.historyRtv.Get() ||
			!submitted.valid || submitted.tex != pending.submittedTexture.Get() ||
			submitted.rtv != pending.submittedRtv.Get() ||
			!Describe(history.rtv, liveHistoryDesc) ||
			!Describe(submitted.rtv, liveSubmittedDesc) ||
			!SameTextureContract(liveHistoryDesc, pending.historyDesc) ||
			!SameTextureContract(liveSubmittedDesc, pending.submittedDesc) ||
			!SameTextureContract(g_qualityStageDesc, pending.submittedDesc)) {
			return PublishResult::kNotIssued;
		}

		auto* const device = Engine::Device();
		if (!device || FAILED(device->GetDeviceRemovedReason())) {
			return PublishResult::kNotIssued;
		}
		Bindings publish;
		publish.source = g_qualityStageSrv;
		publish.history = pending.historyRtv;
		publish.submitted = pending.submittedRtv;
		publish.sourceDesc = g_qualityStageDesc;
		publish.historyDesc = pending.historyDesc;
		publish.submittedDesc = pending.submittedDesc;
		publish.historyLogical = pending.historyLogical;
		// Full UI/menu/cell validation runs before entering private D3D state. The
		// isolated draw performs a second lock-free generation check at the exact
		// issue boundary, so no engine lock is held while the immediate context is live.
		if (!ResetPolicy::RevalidateQualityWorldAdmission(a_worldAdmission)) {
			return PublishResult::kWorldAdmissionVeto;
		}
		// kBeforePublish is already the point before this validation, so no beforeBegin.
		constexpr Telemetry::IsolationMarks publishMarks{
			Telemetry::MarkPoint::kNone,
			Telemetry::MarkPoint::kAfterPublishIsolationBegin,
			Telemetry::MarkPoint::kBeforePublishIsolationEnd,
		};
		const auto outcome = DrawBindings(
			publish,
			"late transactional MRT publish"sv,
			nullptr,
			a_publicationIssued,
			a_publicationStateUncertain,
			&a_worldAdmission,
			publishMarks);
		if (outcome == DrawOutcome::worldAdmissionVeto) {
			return PublishResult::kWorldAdmissionVeto;
		}
		if (outcome == DrawOutcome::stateUncertain) {
			if (*a_publicationIssued) {
				g_qualityPublishDraws.fetch_add(1, std::memory_order_relaxed);
				*a_publicationCounted = true;
			}
			// Publication crossed an uncertain driver boundary. Retain the COM references
			// for process teardown, but make every logical route to the staged episode inert.
			g_pendingQualitySuccess.active = false;
			g_pendingQualitySuccess.historyRepairRequired = false;
			g_pendingQualitySuccess.stageSucceeded = false;
			return PublishResult::kIssuedUnrecoverable;
		}
		if (outcome == DrawOutcome::notDrawn) {
			return PublishResult::kNotIssued;
		}
		g_qualityPublishDraws.fetch_add(1, std::memory_order_relaxed);
		*a_publicationCounted = true;

		const auto committed = g_pendingQualitySuccess;
		g_pendingQualitySuccess = {};
		const bool injectedPostPublish = outcome == DrawOutcome::succeeded &&
			HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kPostPublish);
		if (outcome != DrawOutcome::succeeded || injectedPostPublish) {
			return PublishResult::kIssuedUnrecoverable;
		}
		RecordSuccess(
			committed.historyLogical,
			committed.width,
			committed.height,
			committed.bypass);
		return PublishResult::kSucceeded;
	}

	PublishResult PublishStagedQualitySuccess(
		HDRRoundTrip::HandbackStamp a_handback,
		ResetPolicy::WorldAdmissionTicket a_worldAdmission) noexcept
	{
		// Keep Configure from replacing the episode across validation, the public
		// MRT draw, and terminal bookkeeping. The explicit finally is required for
		// driver SEH; relying on a C++ guard destructor could strand reload forever.
		auto renderEntry = EnterRenderEntry(g_bypassEnabled);
		if (!renderEntry.entered) {
			return PublishResult::kNotIssued;
		}
		PublishResult result = PublishResult::kNotIssued;
		bool publicationIssued = false;
		bool publicationStateUncertain = false;
		bool publicationCounted = false;
		__try {
			__try {
				result = PublishStagedQualitySuccessImpl(
					a_handback,
					a_worldAdmission,
					renderEntry.epoch,
					&publicationIssued,
					&publicationStateUncertain,
					&publicationCounted);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				g_lastError.store(static_cast<std::uint32_t>(::GetExceptionCode()), std::memory_order_relaxed);
				if (publicationIssued || publicationStateUncertain) {
					if (publicationIssued && !publicationCounted) {
						g_qualityPublishDraws.fetch_add(1, std::memory_order_relaxed);
					}
					// Do not release potentially faulting COM references in the handler.
					// The session is terminal; make the TLS intent inert and preserve the
					// references until orderly process teardown.
					g_pendingQualitySuccess.active = false;
					g_pendingQualitySuccess.historyRepairRequired = false;
					g_pendingQualitySuccess.stageSucceeded = false;
					g_sessionDisabled.store(true, std::memory_order_release);
					result = PublishResult::kIssuedUnrecoverable;
				} else {
					result = PublishResult::kNotIssued;
				}
			}
		} __finally {
			// Keep both the TAA-local and central admission alive until the SEH
			// handler has classified publication and made ownership terminal.
			LeaveRenderEntry(renderEntry);
		}
		return result;
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

	AbortResult AbortStagedQualitySuccessImpl(AbortRepairOperation* a_operation) noexcept
	{
		a_operation->repairRequired = g_pendingQualitySuccess.active &&
		                              g_pendingQualitySuccess.historyRepairRequired;
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
			g_pendingQualitySuccess.stageSucceeded = false;
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

	AbortResult AbortStagedQualitySuccess() noexcept
	{
		if (D3D11StateIsolation::HostStateUncertain()) {
			const bool repairRequired = g_pendingQualitySuccess.active &&
			                            g_pendingQualitySuccess.historyRepairRequired;
			if (repairRequired) {
				g_qualityHistoryRepairAttempts.fetch_add(1, std::memory_order_relaxed);
				g_qualityHistoryRepairFailures.fetch_add(1, std::memory_order_relaxed);
			}
			g_pendingQualitySuccess.active = false;
			g_pendingQualitySuccess.historyRepairRequired = false;
			g_pendingQualitySuccess.stageSucceeded = false;
			g_isolationFailures.fetch_add(1, std::memory_order_relaxed);
			g_sessionDisabled.store(true, std::memory_order_release);
			return AbortResult::kUnrecoverable;
		}
		AbortRepairOperation operation{};
		__try {
			operation.result = AbortStagedQualitySuccessImpl(&operation);
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
			g_pendingQualitySuccess.stageSucceeded = false;
			g_isolationFailures.fetch_add(1, std::memory_order_relaxed);
			g_sessionDisabled.store(true, std::memory_order_release);
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

	ID3D11Texture2D* StagedQualityOutput() noexcept
	{
		return g_pendingQualitySuccess.active && g_pendingQualitySuccess.stageSucceeded ?
		           g_qualityStage.Get() :
		           nullptr;
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
		state.qualityStageDraws = g_qualityStageDraws.load(std::memory_order_relaxed);
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
		return state;
	}
}
