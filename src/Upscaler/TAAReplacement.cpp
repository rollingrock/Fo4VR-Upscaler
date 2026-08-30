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
		std::atomic<bool> g_requireNgxHandback{ false };
		std::atomic<bool> g_ready{ false };
		std::atomic<bool> g_completed{ false };
		std::atomic<bool> g_sessionDisabled{ false };
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_successes{ 0 };
		std::atomic<std::uint64_t> g_noHandbackSkips{ 0 };
		std::atomic<std::uint64_t> g_validationFailures{ 0 };
		std::atomic<std::uint64_t> g_isolationFailures{ 0 };
		std::atomic<std::uint32_t> g_validationMask{ 0 };
		std::atomic<std::uint64_t> g_shaderBuilds{ 0 };
		std::atomic<std::uint64_t> g_history61Writes{ 0 };
		std::atomic<std::uint64_t> g_history62Writes{ 0 };
		std::atomic<std::uint64_t> g_lastTemporalAACall{ 0 };
		std::atomic<std::uint64_t> g_lastHandbackSerial{ 0 };
		std::atomic<std::uint32_t> g_width{ 0 };
		std::atomic<std::uint32_t> g_height{ 0 };
		std::atomic<std::uint32_t> g_sourceFormat{ 0 };
		std::atomic<std::uint32_t> g_historyFormat{ 0 };
		std::atomic<std::uint32_t> g_outputFormat{ 0 };
		std::atomic<std::int32_t> g_deviceRemovedReason{ S_OK };
		std::atomic<std::uint32_t> g_lastError{ S_OK };
		std::atomic<std::uintptr_t> g_contextAddress{ 0 };
		std::atomic<std::uintptr_t> g_sourceSrvAddress{ 0 };
		std::atomic<std::uintptr_t> g_historyRtvAddress{ 0 };
		std::atomic<std::uintptr_t> g_outputRtvAddress{ 0 };

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

		[[nodiscard]] bool CaptureAndValidate(
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
				logger::error(FMT_STRING("native-TAA replacement disabled for this session: {}; vanilla TemporalAA continues"), a_reason);
			}
		}
	}

	void Configure(bool a_enabled, bool a_requireNgxHandback) noexcept
	{
		g_enabled.store(a_enabled, std::memory_order_release);
		g_requireNgxHandback.store(a_requireNgxHandback, std::memory_order_release);
	}

	bool TryOverwrite(
		std::uint64_t a_temporalAACall,
		std::uint64_t a_ngxHandbackSerial) noexcept
	{
		if (!g_enabled.load(std::memory_order_acquire) ||
			g_completed.load(std::memory_order_acquire) ||
			g_sessionDisabled.load(std::memory_order_acquire)) {
			return false;
		}

		if (g_requireNgxHandback.load(std::memory_order_acquire)) {
			const auto previous = g_lastHandbackSerial.load(std::memory_order_relaxed);
			if (a_ngxHandbackSerial == 0 || a_ngxHandbackSerial == previous) {
				g_noHandbackSkips.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			g_lastHandbackSerial.store(a_ngxHandbackSerial, std::memory_order_relaxed);
		}

		g_attempts.fetch_add(1, std::memory_order_relaxed);
		g_lastTemporalAACall.store(a_temporalAACall, std::memory_order_relaxed);
		auto* const device = Engine::Device();
		auto* const context = Engine::ImmediateContext();
		g_contextAddress.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
		if (!device || !context) {
			Disable("the engine device or immediate context is null"sv, true, false);
			return false;
		}

		Bindings bindings;
		const bool validBindings = CaptureAndValidate(context, bindings);
		g_validationMask.store(bindings.validationMask, std::memory_order_relaxed);
		g_width.store(bindings.sourceDesc.Width, std::memory_order_relaxed);
		g_height.store(bindings.sourceDesc.Height, std::memory_order_relaxed);
		g_sourceFormat.store(static_cast<std::uint32_t>(bindings.sourceDesc.Format), std::memory_order_relaxed);
		g_historyFormat.store(static_cast<std::uint32_t>(bindings.historyDesc.Format), std::memory_order_relaxed);
		g_outputFormat.store(static_cast<std::uint32_t>(bindings.submittedDesc.Format), std::memory_order_relaxed);
		g_sourceSrvAddress.store(reinterpret_cast<std::uintptr_t>(bindings.source.Get()), std::memory_order_relaxed);
		g_historyRtvAddress.store(reinterpret_cast<std::uintptr_t>(bindings.history.Get()), std::memory_order_relaxed);
		g_outputRtvAddress.store(reinterpret_cast<std::uintptr_t>(bindings.submitted.Get()), std::memory_order_relaxed);
		if (!validBindings) {
			const auto failures = g_validationFailures.fetch_add(1, std::memory_order_relaxed) + 1;
			if (failures == 1 || failures == 16 || failures == 128) {
				logger::warn(FMT_STRING("native-TAA replacement: skipped binding/descriptor mismatch {} mask {:04X}; descriptors {}x{}:{} / {}x{}:{} / {}x{}:{} - vanilla TemporalAA continues"),
					failures,
					bindings.validationMask,
					bindings.sourceDesc.Width,
					bindings.sourceDesc.Height,
					static_cast<std::uint32_t>(bindings.sourceDesc.Format),
					bindings.historyDesc.Width,
					bindings.historyDesc.Height,
					static_cast<std::uint32_t>(bindings.historyDesc.Format),
					bindings.submittedDesc.Width,
					bindings.submittedDesc.Height,
					static_cast<std::uint32_t>(bindings.submittedDesc.Format));
			}
			return false;
		}

		if (!EnsureObjects(device, context)) {
			Disable("private shader/state preparation or context-state isolation initialization failed"sv, false, true);
			return false;
		}

		D3D11StateIsolation::Scope scope{ g_isolation };
		if (!scope) {
			g_lastError.store(static_cast<std::uint32_t>(g_isolation.LastError()), std::memory_order_relaxed);
			Disable("private D3D11.1 context-state activation failed"sv, false, true);
			return false;
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
		ID3D11ShaderResourceView* source[] = { bindings.source.Get() };
		context->PSSetShaderResources(0, 1, source);
		ID3D11RenderTargetView* outputs[] = {
			bindings.history.Get(), bindings.submitted.Get()
		};
		context->OMSetRenderTargets(2, outputs, nullptr);
		constexpr float blendFactor[4]{};
		context->OMSetBlendState(g_blendState.Get(), blendFactor, 0xFFFFFFFFu);
		context->OMSetDepthStencilState(g_depthStencilState.Get(), 0);
		context->RSSetState(g_rasterizerState.Get());
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(bindings.sourceDesc.Width);
		viewport.Height = static_cast<float>(bindings.sourceDesc.Height);
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);
		context->Draw(3, 0);

		ID3D11ShaderResourceView* nullSource = nullptr;
		context->PSSetShaderResources(0, 1, &nullSource);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		if (!scope.Close()) {
			g_lastError.store(static_cast<std::uint32_t>(g_isolation.LastError()), std::memory_order_relaxed);
			Disable("exact host D3D11 state handback failed after the post-draw overwrite"sv, false, true);
			return true;
		}

		const HRESULT removed = device->GetDeviceRemovedReason();
		g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
		if (FAILED(removed)) {
			g_lastError.store(static_cast<std::uint32_t>(removed), std::memory_order_relaxed);
			Disable("the D3D11 device reported removal after the post-draw overwrite"sv, false, false);
			return true;
		}

		const auto successes = g_successes.fetch_add(1, std::memory_order_relaxed) + 1;
		if (bindings.historyLogical == Addr::kRT_Taa) {
			g_history61Writes.fetch_add(1, std::memory_order_relaxed);
		} else {
			g_history62Writes.fetch_add(1, std::memory_order_relaxed);
		}
		if (successes == 1 || successes == 16 || successes == 128 ||
			successes == 256 || successes == kTargetSuccesses) {
			logger::info(FMT_STRING("native-TAA replacement: accepted format-converting post-draw MRT overwrite {}/{} at {}x{}; vanilla pass cleanup continues"),
				successes,
				kTargetSuccesses,
				bindings.sourceDesc.Width,
				bindings.sourceDesc.Height);
		}
		if (successes >= kTargetSuccesses) {
			g_completed.store(true, std::memory_order_release);
			logger::info("native-TAA replacement: bounded 512-overwrite gate completed and is inert; native TemporalAA output resumes unobstructed on the next frame"sv);
		}
		return true;
	}

	State Snapshot() noexcept
	{
		return {
			g_enabled.load(std::memory_order_acquire),
			g_requireNgxHandback.load(std::memory_order_acquire),
			g_ready.load(std::memory_order_acquire),
			g_completed.load(std::memory_order_acquire),
			g_sessionDisabled.load(std::memory_order_acquire),
			g_attempts.load(std::memory_order_relaxed),
			g_successes.load(std::memory_order_relaxed),
			g_noHandbackSkips.load(std::memory_order_relaxed),
			g_validationFailures.load(std::memory_order_relaxed),
			g_isolationFailures.load(std::memory_order_relaxed),
			g_validationMask.load(std::memory_order_relaxed),
			g_shaderBuilds.load(std::memory_order_relaxed),
			g_history61Writes.load(std::memory_order_relaxed),
			g_history62Writes.load(std::memory_order_relaxed),
			kTargetSuccesses,
			g_lastTemporalAACall.load(std::memory_order_relaxed),
			g_lastHandbackSerial.load(std::memory_order_relaxed),
			g_width.load(std::memory_order_relaxed),
			g_height.load(std::memory_order_relaxed),
			g_sourceFormat.load(std::memory_order_relaxed),
			g_historyFormat.load(std::memory_order_relaxed),
			g_outputFormat.load(std::memory_order_relaxed),
			g_deviceRemovedReason.load(std::memory_order_relaxed),
			g_lastError.load(std::memory_order_relaxed),
			g_contextAddress.load(std::memory_order_relaxed),
			g_sourceSrvAddress.load(std::memory_order_relaxed),
			g_historyRtvAddress.load(std::memory_order_relaxed),
			g_outputRtvAddress.load(std::memory_order_relaxed),
		};
	}
}
