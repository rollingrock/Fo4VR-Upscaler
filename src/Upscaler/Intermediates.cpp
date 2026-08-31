#include "Upscaler/Intermediates.h"
#include "Upscaler/MotionSanitizerShader.h"

#include <spdlog/spdlog.h>

#include <Windows.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <wrl/client.h>

namespace Upscaler::Intermediates
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
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

		struct CacheKey
		{
			ID3D11Device* device = nullptr;
			D3D11_TEXTURE2D_DESC color{};
			D3D11_TEXTURE2D_DESC motion{};
			std::uint32_t renderWidth = 0;
			std::uint32_t renderHeight = 0;
			std::uint32_t outputWidth = 0;
			std::uint32_t outputHeight = 0;
		};

		struct SanitizerObjects
		{
			ComPtr<ID3D11Device> device;
			ComPtr<ID3D11VertexShader> vertexShader;
			ComPtr<ID3D11PixelShader> pixelShader;
			ComPtr<ID3D11Buffer> constantBuffer;
			ComPtr<ID3D11BlendState> blendState;
			ComPtr<ID3D11RasterizerState> rasterizerState;
			ComPtr<ID3D11DepthStencilState> depthStencilState;

			[[nodiscard]] bool Ready() const noexcept
			{
				return device && vertexShader && pixelShader && constantBuffer && blendState && rasterizerState && depthStencilState;
			}
		};

		struct CompletionConstants
		{
			float currentClipToPreviousClip[16]{};
			float inverseDimensions[2]{};
			std::uint32_t depthBase[2]{};
			std::uint32_t completionEnabled = 0;
			std::uint32_t depthAvailable = 0;
			float padding[2]{};
		};
		static_assert(sizeof(CompletionConstants) == 96);

		ComPtr<ID3D11Texture2D> g_color[2];
		ComPtr<ID3D11Texture2D> g_rawMotion[2];
		ComPtr<ID3D11ShaderResourceView> g_rawMotionSrv[2];
		ComPtr<ID3D11Texture2D> g_motionVectors[2];
		ComPtr<ID3D11RenderTargetView> g_motionRtv[2];
		ComPtr<ID3D11Texture2D> g_privateDepth[2];
		ComPtr<ID3D11RenderTargetView> g_privateDepthRtv[2];
		ID3D11Texture2D* g_depthSource = nullptr;
		ComPtr<ID3D11ShaderResourceView> g_depthSrv;
		ComPtr<ID3D11Texture2D> g_output[2];
		SanitizerObjects g_sanitizer;
		CacheKey g_key{};
		HMODULE g_compilerModule = nullptr;
		D3DCompileFn g_compile = nullptr;

		std::atomic<bool> g_ready{ false };
		std::atomic<std::uint64_t> g_resourceBuilds{ 0 };
		std::atomic<std::uint64_t> g_motionSanitizerBuilds{ 0 };
		std::atomic<std::uint64_t> g_motionSanitizerDraws{ 0 };
		std::atomic<std::uint64_t> g_motionCompletionDraws{ 0 };
		std::atomic<std::uint64_t> g_motionFallbackDraws{ 0 };
		std::atomic<std::int32_t> g_lastError{ S_OK };
		std::atomic<std::uint32_t> g_renderWidth{ 0 };
		std::atomic<std::uint32_t> g_renderHeight{ 0 };
		std::atomic<std::uint32_t> g_outputWidth{ 0 };
		std::atomic<std::uint32_t> g_outputHeight{ 0 };
		std::atomic<std::uint32_t> g_format{ 0 };
		std::atomic<std::uint32_t> g_motionFormat{ 0 };
		std::atomic<std::uintptr_t> g_leftColor{ 0 };
		std::atomic<std::uintptr_t> g_rightColor{ 0 };
		std::atomic<std::uintptr_t> g_leftMotionVectors{ 0 };
		std::atomic<std::uintptr_t> g_rightMotionVectors{ 0 };
		std::atomic<std::uintptr_t> g_leftOutput{ 0 };
		std::atomic<std::uintptr_t> g_rightOutput{ 0 };

		[[nodiscard]] bool SameDesc(const D3D11_TEXTURE2D_DESC& a_lhs, const D3D11_TEXTURE2D_DESC& a_rhs) noexcept
		{
			return a_lhs.Width == a_rhs.Width &&
			       a_lhs.Height == a_rhs.Height &&
			       a_lhs.MipLevels == a_rhs.MipLevels &&
			       a_lhs.ArraySize == a_rhs.ArraySize &&
			       a_lhs.Format == a_rhs.Format &&
			       a_lhs.SampleDesc.Count == a_rhs.SampleDesc.Count &&
			       a_lhs.SampleDesc.Quality == a_rhs.SampleDesc.Quality &&
			       a_lhs.Usage == a_rhs.Usage &&
			       a_lhs.BindFlags == a_rhs.BindFlags &&
			       a_lhs.CPUAccessFlags == a_rhs.CPUAccessFlags &&
			       a_lhs.MiscFlags == a_rhs.MiscFlags;
		}

		[[nodiscard]] bool SameKey(const CacheKey& a_lhs, const CacheKey& a_rhs) noexcept
		{
			return a_lhs.device == a_rhs.device &&
			       SameDesc(a_lhs.color, a_rhs.color) &&
			       SameDesc(a_lhs.motion, a_rhs.motion) &&
			       a_lhs.renderWidth == a_rhs.renderWidth &&
			       a_lhs.renderHeight == a_rhs.renderHeight &&
			       a_lhs.outputWidth == a_rhs.outputWidth &&
			       a_lhs.outputHeight == a_rhs.outputHeight;
		}

		[[nodiscard]] bool ValidSourceDesc(const D3D11_TEXTURE2D_DESC& a_desc) noexcept
		{
			return a_desc.Width >= 2 && (a_desc.Width & 1u) == 0 && a_desc.Height != 0 &&
			       a_desc.MipLevels == 1 && a_desc.ArraySize == 1 &&
			       a_desc.Format != DXGI_FORMAT_UNKNOWN && a_desc.SampleDesc.Count == 1 &&
			       a_desc.Usage == D3D11_USAGE_DEFAULT && a_desc.CPUAccessFlags == 0 &&
			       a_desc.MiscFlags == 0;
		}

		[[nodiscard]] bool ValidMotionDesc(
			const D3D11_TEXTURE2D_DESC& a_desc,
			const Backend::Config& a_config) noexcept
		{
			return ValidSourceDesc(a_desc) &&
			       a_desc.Width == a_config.renderWidth * 2u &&
			       a_desc.Height == a_config.renderHeight &&
			       a_desc.Format == DXGI_FORMAT_R16G16_FLOAT;
		}

		[[nodiscard]] bool ValidDepthDesc(const D3D11_TEXTURE2D_DESC& a_desc) noexcept
		{
			return ValidSourceDesc(a_desc) &&
			       a_desc.Format == DXGI_FORMAT_R24G8_TYPELESS &&
			       (a_desc.BindFlags & (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) ==
			           (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
		}

		[[nodiscard]] bool CompatibleForCopy(
			const D3D11_TEXTURE2D_DESC& a_zero,
			const D3D11_TEXTURE2D_DESC& a_wide,
			const Backend::Rect& a_rect) noexcept
		{
			return a_rect.w != 0 && a_rect.h != 0 &&
			       a_rect.x <= a_wide.Width && a_rect.y <= a_wide.Height &&
			       a_rect.w <= a_wide.Width - a_rect.x &&
			       a_rect.h <= a_wide.Height - a_rect.y &&
			       a_zero.Width == a_rect.w && a_zero.Height == a_rect.h &&
			       a_zero.MipLevels == 1 && a_wide.MipLevels == 1 &&
			       a_zero.ArraySize == 1 && a_wide.ArraySize == 1 &&
			       a_zero.Format == a_wide.Format &&
			       a_zero.SampleDesc.Count == 1 && a_wide.SampleDesc.Count == 1;
		}

		[[nodiscard]] bool ExtractRectangle(
			ID3D11DeviceContext* a_immediate,
			ID3D11Texture2D* a_doubleWide,
			const Backend::Rect& a_sourceRect,
			ID3D11Texture2D* a_zeroOrigin,
			DXGI_FORMAT a_requiredFormat) noexcept
		{
			if (!a_immediate || !a_doubleWide || !a_zeroOrigin || a_doubleWide == a_zeroOrigin) {
				return false;
			}
			D3D11_TEXTURE2D_DESC wideDesc{};
			D3D11_TEXTURE2D_DESC zeroDesc{};
			a_doubleWide->GetDesc(&wideDesc);
			a_zeroOrigin->GetDesc(&zeroDesc);
			if ((a_requiredFormat != DXGI_FORMAT_UNKNOWN && wideDesc.Format != a_requiredFormat) ||
				!CompatibleForCopy(zeroDesc, wideDesc, a_sourceRect)) {
				return false;
			}

			const D3D11_BOX box{
				a_sourceRect.x,
				a_sourceRect.y,
				0,
				a_sourceRect.x + a_sourceRect.w,
				a_sourceRect.y + a_sourceRect.h,
				1,
			};
			a_immediate->CopySubresourceRegion(a_zeroOrigin, 0, 0, 0, 0, a_doubleWide, 0, &box);
			return true;
		}

		[[nodiscard]] bool CompileSanitizerShader(
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
				spdlog::error("intermediates: d3dcompiler_47.dll / D3DCompile unavailable for motion sanitization");
				return false;
			}

			ComPtr<ID3DBlob> errors;
			const HRESULT hr = g_compile(
				MotionSanitizerShader::kSource,
				sizeof(MotionSanitizerShader::kSource) - 1,
				"Fo4VRMotionSanitizer",
				nullptr,
				nullptr,
				a_entry,
				a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
				0,
				a_out,
				errors.GetAddressOf());
			g_lastError.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr)) {
				spdlog::error(
					"intermediates: motion sanitizer {} compile failed, HRESULT {:08X}: {}",
					a_entry,
					static_cast<std::uint32_t>(hr),
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler message");
				return false;
			}
			return true;
		}

		[[nodiscard]] bool BuildSanitizerObjects(
			ID3D11Device* a_device,
			SanitizerObjects& a_out) noexcept
		{
			if (!a_device) {
				return false;
			}

			ComPtr<ID3DBlob> vertexCode;
			ComPtr<ID3DBlob> pixelCode;
			if (!CompileSanitizerShader("VSMain", "vs_5_0", vertexCode.GetAddressOf()) ||
				!CompileSanitizerShader("PSMain", "ps_5_0", pixelCode.GetAddressOf())) {
				return false;
			}

			HRESULT hr = a_device->CreateVertexShader(
				vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
				a_out.vertexShader.GetAddressOf());
			if (SUCCEEDED(hr)) {
				hr = a_device->CreatePixelShader(
					pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr,
					a_out.pixelShader.GetAddressOf());
			}

			D3D11_BUFFER_DESC constants{};
			constants.ByteWidth = sizeof(CompletionConstants);
			constants.Usage = D3D11_USAGE_DEFAULT;
			constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateBuffer(&constants, nullptr, a_out.constantBuffer.GetAddressOf());
			}

			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			blend.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateBlendState(&blend, a_out.blendState.GetAddressOf());
			}

			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateRasterizerState(&raster, a_out.rasterizerState.GetAddressOf());
			}

			D3D11_DEPTH_STENCIL_DESC depth{};
			depth.DepthEnable = FALSE;
			depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateDepthStencilState(&depth, a_out.depthStencilState.GetAddressOf());
			}

			g_lastError.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr) || !a_out.vertexShader || !a_out.pixelShader || !a_out.constantBuffer || !a_out.blendState ||
				!a_out.rasterizerState || !a_out.depthStencilState) {
				spdlog::error(
					"intermediates: motion-sanitizer shader/state creation failed, HRESULT {:08X}",
					static_cast<std::uint32_t>(hr));
				return false;
			}
			a_out.device = a_device;
			return true;
		}

		[[nodiscard]] bool EnsureDepthSrv(ID3D11Texture2D* a_depth) noexcept
		{
			if (!a_depth || !g_sanitizer.device) {
				return false;
			}
			if (g_depthSource == a_depth && g_depthSrv) {
				return true;
			}

			D3D11_TEXTURE2D_DESC desc{};
			a_depth->GetDesc(&desc);
			if (!ValidDepthDesc(desc)) {
				return false;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			view.Texture2D.MostDetailedMip = 0;
			view.Texture2D.MipLevels = 1;
			ComPtr<ID3D11ShaderResourceView> next;
			const HRESULT hr = g_sanitizer.device->CreateShaderResourceView(
				a_depth, &view, next.GetAddressOf());
			g_lastError.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr) || !next) {
				return false;
			}

			g_depthSource = a_depth;
			g_depthSrv = std::move(next);
			return true;
		}

		void PublishPointers() noexcept
		{
			g_leftColor.store(reinterpret_cast<std::uintptr_t>(g_color[0].Get()), std::memory_order_relaxed);
			g_rightColor.store(reinterpret_cast<std::uintptr_t>(g_color[1].Get()), std::memory_order_relaxed);
			g_leftMotionVectors.store(reinterpret_cast<std::uintptr_t>(g_motionVectors[0].Get()), std::memory_order_relaxed);
			g_rightMotionVectors.store(reinterpret_cast<std::uintptr_t>(g_motionVectors[1].Get()), std::memory_order_relaxed);
			g_leftOutput.store(reinterpret_cast<std::uintptr_t>(g_output[0].Get()), std::memory_order_relaxed);
			g_rightOutput.store(reinterpret_cast<std::uintptr_t>(g_output[1].Get()), std::memory_order_relaxed);
		}
	}

	bool EnsureFor(
		ID3D11Device* a_device,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config) noexcept
	{
		if (!a_device || !ValidSourceDesc(a_colorDesc) ||
			a_config.renderWidth == 0 || a_config.renderHeight == 0 ||
			a_config.outputWidth == 0 || a_config.outputHeight == 0 ||
			a_config.renderWidth > UINT32_MAX / 2u ||
			!ValidMotionDesc(a_motionDesc, a_config) ||
			a_config.renderWidth != a_colorDesc.Width / 2u ||
			a_config.renderHeight != a_colorDesc.Height ||
			a_config.outputWidth != a_colorDesc.Width / 2u ||
			a_config.outputHeight != a_colorDesc.Height) {
			g_lastError.store(E_INVALIDARG, std::memory_order_relaxed);
			return false;
		}

		const CacheKey requested{
			a_device,
			a_colorDesc,
			a_motionDesc,
			a_config.renderWidth,
			a_config.renderHeight,
			a_config.outputWidth,
			a_config.outputHeight,
		};
		if (g_ready.load(std::memory_order_acquire) && SameKey(g_key, requested)) {
			g_lastError.store(S_OK, std::memory_order_relaxed);
			return true;
		}

		D3D11_TEXTURE2D_DESC colorDesc = a_colorDesc;
		colorDesc.Width = a_config.renderWidth;
		colorDesc.Height = a_config.renderHeight;
		D3D11_TEXTURE2D_DESC motionDesc = a_motionDesc;
		motionDesc.Width = a_config.renderWidth;
		motionDesc.Height = a_config.renderHeight;
		motionDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_TEXTURE2D_DESC rawMotionDesc = motionDesc;
		rawMotionDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_TEXTURE2D_DESC depthDesc{};
		depthDesc.Width = a_config.renderWidth;
		depthDesc.Height = a_config.renderHeight;
		depthDesc.MipLevels = 1;
		depthDesc.ArraySize = 1;
		depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthDesc.SampleDesc.Count = 1;
		depthDesc.Usage = D3D11_USAGE_DEFAULT;
		depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_TEXTURE2D_DESC outputDesc = a_colorDesc;
		outputDesc.Width = a_config.outputWidth;
		outputDesc.Height = a_config.outputHeight;
		// Reconstruction writes through a UAV even when the engine-owned source is
		// only an RTV/SRV. The output is our resource, so declare the extra bind
		// contract here instead of relying on flags inherited from the source.
		outputDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		SanitizerObjects nextSanitizer;
		const bool replaceSanitizer = !g_sanitizer.Ready() || g_sanitizer.device.Get() != a_device;
		if (replaceSanitizer && !BuildSanitizerObjects(a_device, nextSanitizer)) {
			return false;
		}

		ComPtr<ID3D11Texture2D> nextColor[2];
		ComPtr<ID3D11Texture2D> nextRawMotion[2];
		ComPtr<ID3D11ShaderResourceView> nextRawMotionSrv[2];
		ComPtr<ID3D11Texture2D> nextMotionVectors[2];
		ComPtr<ID3D11RenderTargetView> nextMotionRtv[2];
		ComPtr<ID3D11Texture2D> nextDepth[2];
		ComPtr<ID3D11RenderTargetView> nextDepthRtv[2];
		ComPtr<ID3D11Texture2D> nextOutput[2];
		HRESULT hr = S_OK;
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			hr = a_device->CreateTexture2D(&colorDesc, nullptr, nextColor[eye].GetAddressOf());
			if (FAILED(hr) || !nextColor[eye]) {
				break;
			}
			hr = a_device->CreateTexture2D(&rawMotionDesc, nullptr, nextRawMotion[eye].GetAddressOf());
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateShaderResourceView(
					nextRawMotion[eye].Get(), nullptr, nextRawMotionSrv[eye].GetAddressOf());
			}
			if (FAILED(hr) || !nextRawMotion[eye] || !nextRawMotionSrv[eye]) {
				break;
			}
			hr = a_device->CreateTexture2D(&motionDesc, nullptr, nextMotionVectors[eye].GetAddressOf());
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateRenderTargetView(
					nextMotionVectors[eye].Get(), nullptr, nextMotionRtv[eye].GetAddressOf());
			}
			if (FAILED(hr) || !nextMotionVectors[eye] || !nextMotionRtv[eye]) {
				break;
			}
			hr = a_device->CreateTexture2D(&depthDesc, nullptr, nextDepth[eye].GetAddressOf());
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateRenderTargetView(
					nextDepth[eye].Get(), nullptr, nextDepthRtv[eye].GetAddressOf());
			}
			if (FAILED(hr) || !nextDepth[eye] || !nextDepthRtv[eye]) {
				break;
			}
			hr = a_device->CreateTexture2D(&outputDesc, nullptr, nextOutput[eye].GetAddressOf());
			if (FAILED(hr) || !nextOutput[eye]) {
				break;
			}
		}

		if (FAILED(hr) || !nextColor[0] || !nextColor[1] ||
			!nextRawMotion[0] || !nextRawMotion[1] ||
			!nextRawMotionSrv[0] || !nextRawMotionSrv[1] ||
			!nextMotionVectors[0] || !nextMotionVectors[1] ||
			!nextMotionRtv[0] || !nextMotionRtv[1] ||
			!nextDepth[0] || !nextDepth[1] ||
			!nextDepthRtv[0] || !nextDepthRtv[1] ||
			!nextOutput[0] || !nextOutput[1]) {
			g_lastError.store(static_cast<std::int32_t>(FAILED(hr) ? hr : E_FAIL), std::memory_order_relaxed);
			return false;
		}

		g_ready.store(false, std::memory_order_release);
		if (replaceSanitizer) {
			g_sanitizer = std::move(nextSanitizer);
			g_motionSanitizerBuilds.fetch_add(1, std::memory_order_relaxed);
		}
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			g_color[eye] = std::move(nextColor[eye]);
			g_rawMotion[eye] = std::move(nextRawMotion[eye]);
			g_rawMotionSrv[eye] = std::move(nextRawMotionSrv[eye]);
			g_motionVectors[eye] = std::move(nextMotionVectors[eye]);
			g_motionRtv[eye] = std::move(nextMotionRtv[eye]);
			g_privateDepth[eye] = std::move(nextDepth[eye]);
			g_privateDepthRtv[eye] = std::move(nextDepthRtv[eye]);
			g_output[eye] = std::move(nextOutput[eye]);
		}
		g_key = requested;
		g_renderWidth.store(a_config.renderWidth, std::memory_order_relaxed);
		g_renderHeight.store(a_config.renderHeight, std::memory_order_relaxed);
		g_outputWidth.store(a_config.outputWidth, std::memory_order_relaxed);
		g_outputHeight.store(a_config.outputHeight, std::memory_order_relaxed);
		g_format.store(static_cast<std::uint32_t>(a_colorDesc.Format), std::memory_order_relaxed);
		g_motionFormat.store(static_cast<std::uint32_t>(a_motionDesc.Format), std::memory_order_relaxed);
		PublishPointers();
		g_resourceBuilds.fetch_add(1, std::memory_order_relaxed);
		g_lastError.store(S_OK, std::memory_order_relaxed);
		g_ready.store(true, std::memory_order_release);

		spdlog::info("intermediates: built two colour, raw-motion, camera-completed-motion, private-depth and output textures at {}x{} per eye, colour dxgi {}, motion dxgi {}",
			a_config.renderWidth,
			a_config.renderHeight,
			static_cast<std::uint32_t>(a_colorDesc.Format),
			static_cast<std::uint32_t>(a_motionDesc.Format));
		return true;
	}

	Staged Get(StereoLayout::Eye a_eye) noexcept
	{
		if (!g_ready.load(std::memory_order_acquire)) {
			return {};
		}
		const auto index = static_cast<std::uint32_t>(a_eye);
		if (index >= 2) {
			return {};
		}
		return { g_color[index].Get(), g_privateDepth[index].Get(), g_motionVectors[index].Get(), g_output[index].Get() };
	}

	DepthReference ReferenceDepth(
		ID3D11Texture2D* a_doubleWide,
		const D3D11_TEXTURE2D_DESC& a_desc,
		const Backend::Rect& a_sourceRect) noexcept
	{
		if (!a_doubleWide || !ValidDepthDesc(a_desc)) {
			return {};
		}

		D3D11_TEXTURE2D_DESC actual{};
		a_doubleWide->GetDesc(&actual);
		const std::uint32_t eyeWidth = a_desc.Width / 2u;
		if (!SameDesc(actual, a_desc) ||
			a_sourceRect.y != 0 || a_sourceRect.w != eyeWidth || a_sourceRect.h != a_desc.Height ||
			(a_sourceRect.x != 0 && a_sourceRect.x != eyeWidth)) {
			return {};
		}

		return { a_doubleWide, a_sourceRect };
	}

	bool ExtractColor(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWide,
		const Backend::Rect& a_sourceRect,
		ID3D11Texture2D* a_zeroOrigin) noexcept
	{
		return ExtractRectangle(a_immediate, a_doubleWide, a_sourceRect, a_zeroOrigin, DXGI_FORMAT_UNKNOWN);
	}

	bool ExtractMotion(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWide,
		ID3D11Texture2D* a_doubleWideDepth,
		const Backend::Rect& a_sourceRect,
		ID3D11Texture2D* a_zeroOrigin,
		const MotionReprojection::Data& a_reprojection) noexcept
	{
		if (!g_ready.load(std::memory_order_acquire) || !g_sanitizer.Ready()) {
			g_lastError.store(E_FAIL, std::memory_order_relaxed);
			return false;
		}
		std::uint32_t index = 2;
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			if (a_zeroOrigin == g_motionVectors[eye].Get()) {
				index = eye;
				break;
			}
		}
		if (index >= 2 || !g_rawMotion[index] || !g_rawMotionSrv[index] || !g_motionRtv[index] ||
			!ExtractRectangle(a_immediate, a_doubleWide, a_sourceRect, g_rawMotion[index].Get(), DXGI_FORMAT_R16G16_FLOAT)) {
			g_lastError.store(E_INVALIDARG, std::memory_order_relaxed);
			return false;
		}
		const bool depthAvailable = a_doubleWideDepth != nullptr;
		if (depthAvailable && !EnsureDepthSrv(a_doubleWideDepth)) {
			g_lastError.store(E_INVALIDARG, std::memory_order_relaxed);
			return false;
		}

		D3D11_TEXTURE2D_DESC outputDesc{};
		a_zeroOrigin->GetDesc(&outputDesc);
		CompletionConstants constants{};
		for (std::size_t i = 0; i < 16; ++i) {
			constants.currentClipToPreviousClip[i] = a_reprojection.currentClipToPreviousClip[i];
		}
		constants.inverseDimensions[0] = 1.0f / static_cast<float>(outputDesc.Width);
		constants.inverseDimensions[1] = 1.0f / static_cast<float>(outputDesc.Height);
		constants.depthBase[0] = a_sourceRect.x;
		constants.depthBase[1] = a_sourceRect.y;
		constants.completionEnabled = (a_reprojection.valid && depthAvailable) ? 1u : 0u;
		constants.depthAvailable = depthAvailable ? 1u : 0u;
		a_immediate->UpdateSubresource(g_sanitizer.constantBuffer.Get(), 0, nullptr, &constants, 0, 0);

		ID3D11Buffer* nullVertexBuffer = nullptr;
		constexpr UINT zero = 0;
		a_immediate->IASetInputLayout(nullptr);
		a_immediate->IASetVertexBuffers(0, 1, &nullVertexBuffer, &zero, &zero);
		a_immediate->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		a_immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_immediate->VSSetShader(g_sanitizer.vertexShader.Get(), nullptr, 0);
		a_immediate->HSSetShader(nullptr, nullptr, 0);
		a_immediate->DSSetShader(nullptr, nullptr, 0);
		a_immediate->GSSetShader(nullptr, nullptr, 0);
		a_immediate->PSSetShader(g_sanitizer.pixelShader.Get(), nullptr, 0);
		ID3D11ShaderResourceView* sources[2] = {
			g_rawMotionSrv[index].Get(),
			depthAvailable ? g_depthSrv.Get() : nullptr,
		};
		a_immediate->PSSetShaderResources(0, 2, sources);
		ID3D11Buffer* constantsBuffer = g_sanitizer.constantBuffer.Get();
		a_immediate->PSSetConstantBuffers(0, 1, &constantsBuffer);
		ID3D11RenderTargetView* destinations[2] = {
			g_motionRtv[index].Get(),
			g_privateDepthRtv[index].Get(),
		};
		a_immediate->OMSetRenderTargets(2, destinations, nullptr);
		constexpr float blendFactor[4]{};
		a_immediate->OMSetBlendState(g_sanitizer.blendState.Get(), blendFactor, 0xFFFFFFFFu);
		a_immediate->OMSetDepthStencilState(g_sanitizer.depthStencilState.Get(), 0);
		a_immediate->RSSetState(g_sanitizer.rasterizerState.Get());
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(outputDesc.Width);
		viewport.Height = static_cast<float>(outputDesc.Height);
		viewport.MaxDepth = 1.0f;
		a_immediate->RSSetViewports(1, &viewport);
		a_immediate->Draw(3, 0);

		ID3D11ShaderResourceView* nullSources[2]{};
		a_immediate->PSSetShaderResources(0, 2, nullSources);
		a_immediate->OMSetRenderTargets(0, nullptr, nullptr);
		const HRESULT removed = g_sanitizer.device->GetDeviceRemovedReason();
		g_lastError.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
		if (FAILED(removed)) {
			return false;
		}
		g_motionSanitizerDraws.fetch_add(1, std::memory_order_relaxed);
		if (a_reprojection.valid && depthAvailable) {
			g_motionCompletionDraws.fetch_add(1, std::memory_order_relaxed);
		} else {
			g_motionFallbackDraws.fetch_add(1, std::memory_order_relaxed);
		}
		return true;
	}

	bool ReassembleOutput(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_zeroOrigin,
		ID3D11Texture2D* a_doubleWide,
		const Backend::Rect& a_destinationRect) noexcept
	{
		if (!a_immediate || !a_zeroOrigin || !a_doubleWide || a_zeroOrigin == a_doubleWide) {
			return false;
		}
		D3D11_TEXTURE2D_DESC zeroDesc{};
		D3D11_TEXTURE2D_DESC wideDesc{};
		a_zeroOrigin->GetDesc(&zeroDesc);
		a_doubleWide->GetDesc(&wideDesc);
		if (!CompatibleForCopy(zeroDesc, wideDesc, a_destinationRect)) {
			return false;
		}

		const D3D11_BOX box{ 0, 0, 0, zeroDesc.Width, zeroDesc.Height, 1 };
		a_immediate->CopySubresourceRegion(
			a_doubleWide,
			0,
			a_destinationRect.x,
			a_destinationRect.y,
			0,
			a_zeroOrigin,
			0,
			&box);
		return true;
	}

	State Snapshot() noexcept
	{
		return {
			g_ready.load(std::memory_order_acquire),
			g_resourceBuilds.load(std::memory_order_relaxed),
			g_motionSanitizerBuilds.load(std::memory_order_relaxed),
			g_motionSanitizerDraws.load(std::memory_order_relaxed),
			g_motionCompletionDraws.load(std::memory_order_relaxed),
			g_motionFallbackDraws.load(std::memory_order_relaxed),
			g_lastError.load(std::memory_order_relaxed),
			g_renderWidth.load(std::memory_order_relaxed),
			g_renderHeight.load(std::memory_order_relaxed),
			g_outputWidth.load(std::memory_order_relaxed),
			g_outputHeight.load(std::memory_order_relaxed),
			g_format.load(std::memory_order_relaxed),
			g_motionFormat.load(std::memory_order_relaxed),
			g_leftColor.load(std::memory_order_relaxed),
			g_rightColor.load(std::memory_order_relaxed),
			g_leftMotionVectors.load(std::memory_order_relaxed),
			g_rightMotionVectors.load(std::memory_order_relaxed),
			g_leftOutput.load(std::memory_order_relaxed),
			g_rightOutput.load(std::memory_order_relaxed),
		};
	}

	void Release() noexcept
	{
		g_ready.store(false, std::memory_order_release);
		for (auto& texture : g_color) {
			texture.Reset();
		}
		for (auto& texture : g_rawMotion) {
			texture.Reset();
		}
		for (auto& view : g_rawMotionSrv) {
			view.Reset();
		}
		for (auto& texture : g_motionVectors) {
			texture.Reset();
		}
		for (auto& view : g_motionRtv) {
			view.Reset();
		}
		for (auto& texture : g_privateDepth) {
			texture.Reset();
		}
		for (auto& view : g_privateDepthRtv) {
			view.Reset();
		}
		g_depthSource = nullptr;
		g_depthSrv.Reset();
		for (auto& texture : g_output) {
			texture.Reset();
		}
		g_sanitizer = {};
		g_key = {};
		g_renderWidth.store(0, std::memory_order_relaxed);
		g_renderHeight.store(0, std::memory_order_relaxed);
		g_outputWidth.store(0, std::memory_order_relaxed);
		g_outputHeight.store(0, std::memory_order_relaxed);
		g_format.store(0, std::memory_order_relaxed);
		g_motionFormat.store(0, std::memory_order_relaxed);
		PublishPointers();
		g_lastError.store(S_OK, std::memory_order_relaxed);
	}
}
