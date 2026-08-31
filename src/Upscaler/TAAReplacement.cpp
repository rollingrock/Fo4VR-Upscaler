#include "Upscaler/TAAReplacement.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/Engine.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/TAAPassthroughShader.h"

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

		constexpr std::uint64_t kTargetSuccesses = 512;
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
		std::atomic<std::uint64_t> g_parityReadAttempts{ 0 };
		std::atomic<std::uint64_t> g_parityReadFailures{ 0 };
		std::atomic<std::uint64_t> g_parityInvalidValues{ 0 };
		std::atomic<std::uint64_t> g_currentRenderTargetMismatches{ 0 };
		std::atomic<std::uint64_t> g_lastTemporalAACall{ 0 };
		std::atomic<std::uint64_t> g_handbackBaseline{ 0 };
		std::atomic<std::uint64_t> g_lastHandbackSerial{ 0 };
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

		// Configure may run on the devbench listener while the render thread is already
		// inside a replacement. The render thread never waits: it either joins one stable
		// even configuration epoch or declines that frame. Configure closes the epoch and
		// waits for admitted entries to drain before replacing the source/baseline. The
		// sequentially-consistent ordering also closes the load-before-increment race: an
		// entry delayed across a complete close/reopen observes a different epoch and exits.
		class RenderEntryGuard
		{
		public:
			explicit RenderEntryGuard(const std::atomic<bool>& a_modeEnabled) noexcept
			{
				const auto epoch = g_configEpoch.load(std::memory_order_seq_cst);
				if ((epoch & 1u) != 0 || !a_modeEnabled.load(std::memory_order_acquire)) {
					return;
				}

				g_activeRenderEntries.fetch_add(1, std::memory_order_seq_cst);
				if (g_configEpoch.load(std::memory_order_seq_cst) != epoch ||
					!a_modeEnabled.load(std::memory_order_acquire)) {
					g_activeRenderEntries.fetch_sub(1, std::memory_order_seq_cst);
					return;
				}
				entered_ = true;
			}

			~RenderEntryGuard()
			{
				if (entered_) {
					g_activeRenderEntries.fetch_sub(1, std::memory_order_seq_cst);
				}
			}

			RenderEntryGuard(const RenderEntryGuard&) = delete;
			RenderEntryGuard& operator=(const RenderEntryGuard&) = delete;

			[[nodiscard]] explicit operator bool() const noexcept { return entered_; }

		private:
			bool entered_ = false;
		};

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

		[[nodiscard]] bool ReadHistoryParity(std::uint32_t& a_out) noexcept
		{
			g_parityReadAttempts.fetch_add(1, std::memory_order_relaxed);
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
				g_parityReadFailures.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (a_out > 1) {
				g_parityReadFailures.fetch_add(1, std::memory_order_relaxed);
				g_parityInvalidValues.fetch_add(1, std::memory_order_relaxed);
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
			drawnButFailed
		};

		[[nodiscard]] DrawOutcome DrawBindings(
			const Bindings& a_bindings,
			std::string_view a_mode) noexcept
		{
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

			D3D11StateIsolation::Scope scope{ g_isolation };
			if (!scope) {
				g_lastError.store(static_cast<std::uint32_t>(g_isolation.LastError()), std::memory_order_relaxed);
				Disable("private D3D11.1 context-state activation failed"sv, false, true);
				return DrawOutcome::notDrawn;
			}

			ID3D11Buffer* nullVertexBuffer = nullptr;
			constexpr UINT zero = 0;
			context->IASetInputLayout(nullptr);
			context->IASetVertexBuffers(0, 1, &nullVertexBuffer, &zero, &zero);
			context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
			ID3D11ShaderResourceView* source[] = { a_bindings.source.Get() };
			context->PSSetShaderResources(0, 1, source);
			ID3D11RenderTargetView* outputs[] = {
				a_bindings.history.Get(), a_bindings.submitted.Get()
			};
			context->OMSetRenderTargets(2, outputs, nullptr);
			constexpr float blendFactor[4]{};
			context->OMSetBlendState(g_blendState.Get(), blendFactor, 0xFFFFFFFFu);
			context->OMSetDepthStencilState(g_depthStencilState.Get(), 0);
			context->RSSetState(g_rasterizerState.Get());
			D3D11_VIEWPORT viewport{};
			viewport.Width = static_cast<float>(a_bindings.sourceDesc.Width);
			viewport.Height = static_cast<float>(a_bindings.sourceDesc.Height);
			viewport.MaxDepth = 1.0f;
			context->RSSetViewports(1, &viewport);
			context->Draw(3, 0);

			ID3D11ShaderResourceView* nullSource = nullptr;
			context->PSSetShaderResources(0, 1, &nullSource);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			if (!scope.Close()) {
				g_lastError.store(static_cast<std::uint32_t>(g_isolation.LastError()), std::memory_order_relaxed);
				Disable(fmt::format(FMT_STRING("exact host D3D11 state handback failed after the {} draw"), a_mode), false, true);
				return DrawOutcome::drawnButFailed;
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

		void RecordSuccess(const Bindings& a_bindings, bool a_bypass) noexcept
		{
			const auto successes = g_successes.fetch_add(1, std::memory_order_relaxed) + 1;
			if (a_bindings.historyLogical == Addr::kRT_Taa) {
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
				successes == 256 || successes == kTargetSuccesses) {
				logger::info(FMT_STRING("native-TAA replacement: accepted format-converting {} {}/{} at {}x{}; outer vanilla cleanup continues"),
					a_bypass ? "prepared-draw bypass"sv : "post-draw MRT overwrite"sv,
					successes,
					kTargetSuccesses,
					a_bindings.sourceDesc.Width,
					a_bindings.sourceDesc.Height);
			}
			if (successes >= kTargetSuccesses) {
				g_completed.store(true, std::memory_order_release);
				logger::info(FMT_STRING("native-TAA replacement: bounded 512-{} gate completed and is inert; fully native TemporalAA resumes on the next frame"),
					a_bypass ? "bypass"sv : "overwrite"sv);
			}
		}
	}

	void Configure(
		bool a_postDrawEnabled,
		bool a_bypassEnabled,
		bool a_requireNgxHandback,
		bool a_requireFfxHandback) noexcept
	{
		const std::scoped_lock configLock(g_configLock);
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
		if (newEpisode) {
			const auto baseline = HDRRoundTrip::AcceptedContinuousHandbackSerial(source);
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
	}

	bool TryBypass(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		const RenderEntryGuard renderEntry(g_bypassEnabled);
		if (!renderEntry || !BeginAttempt(a_temporalAACall, a_handback)) {
			return false;
		}

		std::uint32_t parity = 0;
		if (!ReadHistoryParity(parity)) {
			Disable("the byte-verified TAA parity global did not resolve to 0 or 1"sv, true, false);
			return false;
		}

		Bindings bindings;
		const bool validBindings = CaptureDirectAndValidate(parity, bindings);
		PublishBindings(bindings);
		if (!validBindings) {
			ReportValidationFailure(bindings, "bypass"sv);
			return false;
		}

		const auto outcome = DrawBindings(bindings, "prepared-draw bypass"sv);
		if (outcome == DrawOutcome::succeeded) {
			RecordSuccess(bindings, true);
		}
		return outcome != DrawOutcome::notDrawn;
	}

	bool TryOverwrite(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		const RenderEntryGuard renderEntry(g_postDrawEnabled);
		if (!renderEntry || !BeginAttempt(a_temporalAACall, a_handback)) {
			return false;
		}

		auto* const context = Engine::ImmediateContext();
		g_contextAddress.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
		if (!context) {
			Disable("the engine immediate context is null"sv, true, false);
			return false;
		}

		Bindings bindings;
		const bool validBindings = CapturePostDrawAndValidate(context, bindings);
		PublishBindings(bindings);
		if (!validBindings) {
			ReportValidationFailure(bindings, "post-draw replacement"sv);
			return false;
		}

		const auto outcome = DrawBindings(bindings, "post-draw overwrite"sv);
		if (outcome == DrawOutcome::succeeded) {
			RecordSuccess(bindings, false);
		}
		return outcome != DrawOutcome::notDrawn;
	}

	State Snapshot() noexcept
	{
		const auto currentHandback = HDRRoundTrip::AcceptedContinuousHandbackStamp();
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
		state.parityReadAttempts = g_parityReadAttempts.load(std::memory_order_relaxed);
		state.parityReadFailures = g_parityReadFailures.load(std::memory_order_relaxed);
		state.parityInvalidValues = g_parityInvalidValues.load(std::memory_order_relaxed);
		state.currentRenderTargetMismatches = g_currentRenderTargetMismatches.load(std::memory_order_relaxed);
		state.targetSuccesses = kTargetSuccesses;
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
