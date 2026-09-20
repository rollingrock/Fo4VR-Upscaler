#include "Upscaler/WorkshopMask.h"

#include "Upscaler/D3D11StateIsolation.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <utility>

namespace Upscaler::WorkshopMask
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		static_assert(sizeof(WorkshopMaskShader::Constants) == 32);
		static_assert(sizeof(WorkshopMaskShader::Constants) % 16 == 0);

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

		// Render-thread-owned. Cross-thread readers see only the atomic snapshot below.
		HMODULE g_compilerModule = nullptr;
		D3DCompileFn g_compile = nullptr;
		D3D11StateIsolation::Context g_isolation;
		ComPtr<ID3D11Device> g_device;
		ComPtr<ID3D11DeviceContext> g_context;
		ComPtr<ID3D11VertexShader> g_vertexShader;
		ComPtr<ID3D11PixelShader> g_pixelShader;
		ComPtr<ID3D11BlendState> g_blendState;
		ComPtr<ID3D11RasterizerState> g_rasterizerState;
		ComPtr<ID3D11DepthStencilState> g_depthStencilState;
		ComPtr<ID3D11Buffer> g_constants;
		ComPtr<ID3D11Texture2D> g_outputTexture;
		ComPtr<ID3D11RenderTargetView> g_outputRtv;
		ComPtr<ID3D11ShaderResourceView> g_outputSrv;
		WorkshopMaskShader::Constants g_uploaded{};
		bool g_uploadedValid = false;
		std::uint32_t g_builtWidth = 0;
		std::uint32_t g_builtHeight = 0;
		DXGI_FORMAT g_builtFormat = DXGI_FORMAT_UNKNOWN;
		bool g_privatePassFailed = false;
		std::atomic<std::uint32_t> g_renderThread{ 0 };

		// A failed build gets one attempt for one exact key. A changed device, extent or
		// typed format gets one fresh attempt; the same key never compiles once per frame.
		ComPtr<ID3D11Device> g_failedDevice;
		ComPtr<ID3D11DeviceContext> g_failedContext;
		std::uint32_t g_failedWidth = 0;
		std::uint32_t g_failedHeight = 0;
		DXGI_FORMAT g_failedFormat = DXGI_FORMAT_UNKNOWN;
		bool g_creationFailed = false;

		std::atomic_bool g_releaseRequested{ false };
		std::atomic_bool g_ready{ false };
		std::atomic_bool g_outputValid{ false };
		std::atomic_bool g_failureLatched{ false };
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_prepared{ 0 };
		std::atomic<std::uint64_t> g_declines{ 0 };
		std::atomic<std::uint64_t> g_resourceBuilds{ 0 };
		std::atomic<std::uint64_t> g_resourceFailures{ 0 };
		std::atomic<std::uint64_t> g_draws{ 0 };
		std::atomic<std::uint64_t> g_drawFailures{ 0 };
		std::atomic<std::uint64_t> g_isolationFailures{ 0 };
		std::atomic<std::uint64_t> g_hostStateUncertain{ 0 };
		std::atomic<std::uint64_t> g_releases{ 0 };
		std::atomic<std::uint32_t> g_lastDecline{ 0 };
		std::atomic<std::uint32_t> g_exceptionCode{ 0 };
		std::atomic<std::int32_t> g_lastHr{ S_OK };
		std::atomic<std::uint32_t> g_outputWidth{ 0 };
		std::atomic<std::uint32_t> g_outputHeight{ 0 };
		std::atomic<std::uint32_t> g_format{ 0 };

		void RecordDecline(Decline a_reason) noexcept
		{
			g_outputValid.store(false, std::memory_order_release);
			g_declines.fetch_add(1, std::memory_order_relaxed);
			g_lastDecline.store(static_cast<std::uint32_t>(a_reason), std::memory_order_relaxed);
		}

		[[nodiscard]] bool IsOwnerThread() noexcept
		{
			const auto owner = g_renderThread.load(std::memory_order_acquire);
			return owner == 0 || owner == ::GetCurrentThreadId();
		}

		[[nodiscard]] PrepareResult RecordHostStateUncertain() noexcept
		{
			g_outputValid.store(false, std::memory_order_release);
			g_ready.store(false, std::memory_order_release);
			g_hostStateUncertain.fetch_add(1, std::memory_order_relaxed);
			return PrepareResult::hostStateUncertain;
		}

		[[nodiscard]] bool IsTypedRgba8(DXGI_FORMAT a_format) noexcept
		{
			return a_format == DXGI_FORMAT_R8G8B8A8_UNORM ||
			       a_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		}

		[[nodiscard]] bool IsCompatibleResourceFormat(
			DXGI_FORMAT a_resource,
			DXGI_FORMAT a_view) noexcept
		{
			return a_resource == a_view || a_resource == DXGI_FORMAT_R8G8B8A8_TYPELESS;
		}

		[[nodiscard]] Decline ValidateInput(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_immediate,
			ID3D11Texture2D* a_sourceTexture,
			ID3D11ShaderResourceView* a_sourceSrv,
			const WorkshopMaskShader::Constants& a_geometry,
			DXGI_FORMAT& a_format) noexcept
		{
			if (!a_device || !a_immediate || !a_sourceTexture || !a_sourceSrv) {
				return Decline::invalidArguments;
			}
			if (a_immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
				return Decline::notImmediateContext;
			}

			ComPtr<ID3D11Device> contextDevice;
			ComPtr<ID3D11Device> textureDevice;
			a_immediate->GetDevice(contextDevice.GetAddressOf());
			a_sourceTexture->GetDevice(textureDevice.GetAddressOf());
			if (contextDevice.Get() != a_device || textureDevice.Get() != a_device) {
				return Decline::deviceMismatch;
			}

			ComPtr<ID3D11Resource> viewResource;
			a_sourceSrv->GetResource(viewResource.GetAddressOf());
			ComPtr<ID3D11Texture2D> viewTexture;
			ComPtr<IUnknown> sourceIdentity;
			ComPtr<IUnknown> viewIdentity;
			if (!viewResource || FAILED(viewResource.As(&viewTexture)) ||
				FAILED(a_sourceTexture->QueryInterface(IID_PPV_ARGS(sourceIdentity.GetAddressOf()))) ||
				FAILED(viewTexture.As(&viewIdentity)) || sourceIdentity.Get() != viewIdentity.Get() ||
				a_sourceTexture == g_outputTexture.Get()) {
				return Decline::sourceIdentity;
			}

			D3D11_TEXTURE2D_DESC textureDesc{};
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			a_sourceTexture->GetDesc(&textureDesc);
			a_sourceSrv->GetDesc(&viewDesc);
			if (textureDesc.Width == 0 || textureDesc.Height == 0 || textureDesc.MipLevels != 1 ||
				textureDesc.ArraySize != 1 || textureDesc.SampleDesc.Count != 1 ||
				textureDesc.SampleDesc.Quality != 0 ||
				(textureDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
				return Decline::sourceDescriptor;
			}
			if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
				viewDesc.Texture2D.MostDetailedMip != 0 || viewDesc.Texture2D.MipLevels != 1 ||
				!IsTypedRgba8(viewDesc.Format) ||
				!IsCompatibleResourceFormat(textureDesc.Format, viewDesc.Format)) {
				return Decline::sourceView;
			}
			if (!WorkshopMaskShader::Validate(a_geometry)) {
				return Decline::geometry;
			}

			const std::uint64_t outputWidth =
				static_cast<std::uint64_t>(a_geometry.outputEyeWidth) * 2u;
			if (textureDesc.Width != outputWidth || textureDesc.Height != a_geometry.outputHeight ||
				textureDesc.Width != a_geometry.sourceAllocationWidth ||
				textureDesc.Height != a_geometry.sourceAllocationHeight) {
				return Decline::geometry;
			}
			if (a_geometry.sourceOriginX == 0 && a_geometry.sourceOriginY == 0 &&
				a_geometry.sourceEyeWidth == a_geometry.outputEyeWidth &&
				a_geometry.sourceHeight == a_geometry.outputHeight) {
				return Decline::identityGeometry;
			}

			a_format = viewDesc.Format;
			return Decline::none;
		}

		[[nodiscard]] HRESULT CompileShader(
			const char* a_entry,
			const char* a_target,
			ID3DBlob** a_out) noexcept
		{
			if (!g_compile) {
				g_compilerModule = ::LoadLibraryW(L"d3dcompiler_47.dll");
				if (g_compilerModule) {
					g_compile = reinterpret_cast<D3DCompileFn>(
						::GetProcAddress(g_compilerModule, "D3DCompile"));
					if (!g_compile) {
						static_cast<void>(::FreeLibrary(g_compilerModule));
						g_compilerModule = nullptr;
					}
				}
			}
			if (!g_compile) {
				return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
			}

			ComPtr<ID3DBlob> errors;
			return g_compile(
				WorkshopMaskShader::kSource,
				sizeof(WorkshopMaskShader::kSource) - 1,
				"Fo4VRWorkshopMask",
				nullptr,
				nullptr,
				a_entry,
				a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
				0,
				a_out,
				errors.GetAddressOf());
		}

		void ClearFailureLatch() noexcept
		{
			g_failedDevice.Reset();
			g_failedContext.Reset();
			g_failedWidth = 0;
			g_failedHeight = 0;
			g_failedFormat = DXGI_FORMAT_UNKNOWN;
			g_creationFailed = false;
			g_failureLatched.store(false, std::memory_order_relaxed);
		}

		void ReleaseObjects() noexcept
		{
			g_outputValid.store(false, std::memory_order_release);
			g_ready.store(false, std::memory_order_release);
			g_isolation.Shutdown();
			g_outputSrv.Reset();
			g_outputRtv.Reset();
			g_outputTexture.Reset();
			g_constants.Reset();
			g_depthStencilState.Reset();
			g_rasterizerState.Reset();
			g_blendState.Reset();
			g_pixelShader.Reset();
			g_vertexShader.Reset();
			g_device.Reset();
			g_context.Reset();
			g_uploaded = {};
			g_uploadedValid = false;
			g_builtWidth = 0;
			g_builtHeight = 0;
			g_builtFormat = DXGI_FORMAT_UNKNOWN;
			g_privatePassFailed = false;
			g_outputWidth.store(0, std::memory_order_relaxed);
			g_outputHeight.store(0, std::memory_order_relaxed);
			g_format.store(0, std::memory_order_relaxed);
		}

		[[nodiscard]] bool FailureKeyMatches(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			std::uint32_t a_width,
			std::uint32_t a_height,
			DXGI_FORMAT a_format) noexcept
		{
			return g_creationFailed && g_failedDevice.Get() == a_device &&
			       g_failedContext.Get() == a_context &&
			       g_failedWidth == a_width && g_failedHeight == a_height &&
			       g_failedFormat == a_format;
		}

		void LatchCreationFailure(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			std::uint32_t a_width,
			std::uint32_t a_height,
			DXGI_FORMAT a_format,
			HRESULT a_hr) noexcept
		{
			g_failedDevice = a_device;
			g_failedContext = a_context;
			g_failedWidth = a_width;
			g_failedHeight = a_height;
			g_failedFormat = a_format;
			g_creationFailed = true;
			g_failureLatched.store(true, std::memory_order_relaxed);
			g_resourceFailures.fetch_add(1, std::memory_order_relaxed);
			g_lastHr.store(static_cast<std::int32_t>(a_hr), std::memory_order_relaxed);
		}

		[[nodiscard]] bool EnsureObjects(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			std::uint32_t a_width,
			std::uint32_t a_height,
			DXGI_FORMAT a_format,
			Decline& a_reason) noexcept
		{
			if (g_ready.load(std::memory_order_acquire) && g_device.Get() == a_device &&
				g_context.Get() == a_context &&
				g_builtWidth == a_width && g_builtHeight == a_height &&
				g_builtFormat == a_format) {
				return true;
			}

			if (g_device || g_outputTexture ||
				(g_creationFailed && !FailureKeyMatches(a_device, a_context, a_width, a_height, a_format))) {
				ReleaseObjects();
			}
			if (FailureKeyMatches(a_device, a_context, a_width, a_height, a_format)) {
				a_reason = Decline::failureLatched;
				return false;
			}
			ClearFailureLatch();

			if (!g_isolation.Initialize(a_device, a_context)) {
				const HRESULT hr = g_isolation.LastError();
				LatchCreationFailure(a_device, a_context, a_width, a_height, a_format, hr);
				a_reason = Decline::resourceCreation;
				return false;
			}

			ComPtr<ID3DBlob> vertexCode;
			ComPtr<ID3DBlob> pixelCode;
			ComPtr<ID3D11VertexShader> vertexShader;
			ComPtr<ID3D11PixelShader> pixelShader;
			ComPtr<ID3D11BlendState> blendState;
			ComPtr<ID3D11RasterizerState> rasterizerState;
			ComPtr<ID3D11DepthStencilState> depthStencilState;
			ComPtr<ID3D11Buffer> constants;
			ComPtr<ID3D11Texture2D> outputTexture;
			ComPtr<ID3D11RenderTargetView> outputRtv;
			ComPtr<ID3D11ShaderResourceView> outputSrv;

			HRESULT hr = CompileShader("VSMain", "vs_5_0", vertexCode.GetAddressOf());
			if (SUCCEEDED(hr)) {
				hr = CompileShader("PSMain", "ps_5_0", pixelCode.GetAddressOf());
			}
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateVertexShader(
					vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
					vertexShader.GetAddressOf());
			}
			if (SUCCEEDED(hr)) {
				hr = a_device->CreatePixelShader(
					pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr,
					pixelShader.GetAddressOf());
			}

			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
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

			const WorkshopMaskShader::Constants initial{};
			D3D11_BUFFER_DESC constantsDesc{};
			constantsDesc.ByteWidth = sizeof(initial);
			constantsDesc.Usage = D3D11_USAGE_DEFAULT;
			constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			D3D11_SUBRESOURCE_DATA initialData{};
			initialData.pSysMem = &initial;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateBuffer(&constantsDesc, &initialData, constants.GetAddressOf());
			}

			D3D11_TEXTURE2D_DESC outputDesc{};
			outputDesc.Width = a_width;
			outputDesc.Height = a_height;
			outputDesc.MipLevels = 1;
			outputDesc.ArraySize = 1;
			outputDesc.Format = a_format;
			outputDesc.SampleDesc.Count = 1;
			outputDesc.Usage = D3D11_USAGE_DEFAULT;
			outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateTexture2D(&outputDesc, nullptr, outputTexture.GetAddressOf());
			}

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = a_format;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rtvDesc.Texture2D.MipSlice = 0;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateRenderTargetView(
					outputTexture.Get(), &rtvDesc, outputRtv.GetAddressOf());
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = a_format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			if (SUCCEEDED(hr)) {
				hr = a_device->CreateShaderResourceView(
					outputTexture.Get(), &srvDesc, outputSrv.GetAddressOf());
			}

			g_lastHr.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr) || !vertexShader || !pixelShader || !blendState ||
				!rasterizerState || !depthStencilState || !constants || !outputTexture ||
				!outputRtv || !outputSrv) {
				LatchCreationFailure(
					a_device, a_context, a_width, a_height, a_format, FAILED(hr) ? hr : E_FAIL);
				a_reason = Decline::resourceCreation;
				return false;
			}

			g_device = a_device;
			g_context = a_context;
			g_vertexShader = std::move(vertexShader);
			g_pixelShader = std::move(pixelShader);
			g_blendState = std::move(blendState);
			g_rasterizerState = std::move(rasterizerState);
			g_depthStencilState = std::move(depthStencilState);
			g_constants = std::move(constants);
			g_outputTexture = std::move(outputTexture);
			g_outputRtv = std::move(outputRtv);
			g_outputSrv = std::move(outputSrv);
			g_uploaded = {};
			g_uploadedValid = false;
			g_builtWidth = a_width;
			g_builtHeight = a_height;
			g_builtFormat = a_format;
			g_privatePassFailed = false;
			g_outputWidth.store(a_width, std::memory_order_relaxed);
			g_outputHeight.store(a_height, std::memory_order_relaxed);
			g_format.store(static_cast<std::uint32_t>(a_format), std::memory_order_relaxed);
			g_resourceBuilds.fetch_add(1, std::memory_order_relaxed);
			g_ready.store(true, std::memory_order_release);
			return true;
		}

		struct IsolatedResult
		{
			std::uint32_t exceptionCode = 0;
			bool began = false;
			bool constantsUploaded = false;
			bool drawIssued = false;
			bool ended = false;
			bool stateUncertain = false;
		};

		// No destructible C++ locals: __finally is the exact host-state handback even if
		// a driver raises SEH after changing the immediate context.
		[[nodiscard]] IsolatedResult DrawIsolated(
			ID3D11DeviceContext* a_context,
			ID3D11ShaderResourceView* a_sourceSrv,
			const WorkshopMaskShader::Constants& a_geometry,
			bool a_uploadConstants) noexcept
		{
			IsolatedResult result{};
			__try {
				result.began = g_isolation.Begin();
				if (result.began) {
					__try {
						if (a_uploadConstants) {
							a_context->UpdateSubresource(
								g_constants.Get(), 0, nullptr, &a_geometry, 0, 0);
							result.constantsUploaded = true;
						}

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
						ID3D11Buffer* constants[] = { g_constants.Get() };
						a_context->PSSetConstantBuffers(0, 1, constants);
						ID3D11ShaderResourceView* sources[] = { a_sourceSrv };
						a_context->PSSetShaderResources(0, 1, sources);
						ID3D11RenderTargetView* outputs[] = { g_outputRtv.Get() };
						a_context->OMSetRenderTargets(1, outputs, nullptr);
						constexpr float blendFactor[4]{};
						a_context->OMSetBlendState(g_blendState.Get(), blendFactor, 0xFFFFFFFFu);
						a_context->OMSetDepthStencilState(g_depthStencilState.Get(), 0);
						a_context->RSSetState(g_rasterizerState.Get());
						D3D11_VIEWPORT viewport{};
						viewport.Width = static_cast<float>(g_builtWidth);
						viewport.Height = static_cast<float>(g_builtHeight);
						viewport.MinDepth = 0.0f;
						viewport.MaxDepth = 1.0f;
						a_context->RSSetViewports(1, &viewport);

						result.drawIssued = true;
						a_context->Draw(3, 0);

						ID3D11ShaderResourceView* nullSource = nullptr;
						a_context->PSSetShaderResources(0, 1, &nullSource);
						a_context->OMSetRenderTargets(0, nullptr, nullptr);
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
	}

	PrepareResult Prepare(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_sourceTexture,
		ID3D11ShaderResourceView* a_sourceSrv,
		const WorkshopMaskShader::Constants& a_geometry) noexcept
	{
		g_attempts.fetch_add(1, std::memory_order_relaxed);
		g_outputValid.store(false, std::memory_order_release);
		if (!IsOwnerThread()) {
			RecordDecline(Decline::wrongThread);
			return PrepareResult::declined;
		}
		ReleaseIfRequested();
		if (D3D11StateIsolation::HostStateUncertain()) {
			return RecordHostStateUncertain();
		}

		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		const Decline validation = ValidateInput(
			a_device, a_immediate, a_sourceTexture, a_sourceSrv, a_geometry, format);
		if (validation != Decline::none) {
			RecordDecline(validation);
			return PrepareResult::declined;
		}
		std::uint32_t expectedThread = 0;
		const auto currentThread = ::GetCurrentThreadId();
		if (!g_renderThread.compare_exchange_strong(
				expectedThread, currentThread, std::memory_order_acq_rel, std::memory_order_acquire) &&
			expectedThread != currentThread) {
			RecordDecline(Decline::wrongThread);
			return PrepareResult::declined;
		}

		const std::uint32_t outputWidth = a_geometry.outputEyeWidth * 2u;
		auto reason = Decline::resourceCreation;
		if (!EnsureObjects(
				a_device, a_immediate, outputWidth, a_geometry.outputHeight, format, reason)) {
			if (D3D11StateIsolation::HostStateUncertain()) {
				return RecordHostStateUncertain();
			}
			RecordDecline(reason);
			return PrepareResult::declined;
		}
		if (g_privatePassFailed) {
			RecordDecline(Decline::failureLatched);
			return PrepareResult::declined;
		}

		const bool uploadConstants = !g_uploadedValid ||
			std::memcmp(&g_uploaded, &a_geometry, sizeof(g_uploaded)) != 0;
		const auto result = DrawIsolated(
			a_immediate, a_sourceSrv, a_geometry, uploadConstants);
		if (result.constantsUploaded) {
			g_uploaded = a_geometry;
			g_uploadedValid = true;
		}
		if (result.exceptionCode != 0) {
			g_exceptionCode.store(result.exceptionCode, std::memory_order_relaxed);
		}
		if (result.stateUncertain) {
			g_isolationFailures.fetch_add(1, std::memory_order_relaxed);
			return RecordHostStateUncertain();
		}
		if (!result.began || !result.ended) {
			g_isolationFailures.fetch_add(1, std::memory_order_relaxed);
			RecordDecline(Decline::isolation);
			return PrepareResult::declined;
		}
		if (result.exceptionCode != 0 || !result.drawIssued) {
			g_privatePassFailed = true;
			g_failureLatched.store(true, std::memory_order_relaxed);
			g_drawFailures.fetch_add(1, std::memory_order_relaxed);
			RecordDecline(Decline::privateDraw);
			return PrepareResult::declined;
		}

		g_draws.fetch_add(1, std::memory_order_relaxed);
		g_prepared.fetch_add(1, std::memory_order_relaxed);
		g_lastDecline.store(static_cast<std::uint32_t>(Decline::none), std::memory_order_relaxed);
		g_lastHr.store(S_OK, std::memory_order_relaxed);
		g_outputValid.store(true, std::memory_order_release);
		return PrepareResult::prepared;
	}

	ID3D11ShaderResourceView* Output() noexcept
	{
		return g_outputValid.load(std::memory_order_acquire) ? g_outputSrv.Get() : nullptr;
	}

	void Release() noexcept
	{
		if (!IsOwnerThread()) {
			g_outputValid.store(false, std::memory_order_release);
			g_releaseRequested.store(true, std::memory_order_release);
			return;
		}
		// Clear only requests already visible at entry. A settings-thread request racing
		// the release stays set for the next render-thread visit.
		static_cast<void>(g_releaseRequested.exchange(false, std::memory_order_acq_rel));
		if (D3D11StateIsolation::HostStateUncertain()) {
			g_outputValid.store(false, std::memory_order_release);
			g_releaseRequested.store(true, std::memory_order_release);
			return;
		}
		const bool hadState = g_device || g_outputTexture || g_creationFailed || g_privatePassFailed;
		ReleaseObjects();
		ClearFailureLatch();
		g_renderThread.store(0, std::memory_order_release);
		if (hadState) {
			g_releases.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void RequestRelease() noexcept
	{
		g_releaseRequested.store(true, std::memory_order_release);
	}

	void ReleaseIfRequested() noexcept
	{
		if (g_releaseRequested.load(std::memory_order_acquire)) {
			Release();
		}
	}

	Snapshot GetSnapshot() noexcept
	{
		return {
			g_attempts.load(std::memory_order_relaxed),
			g_prepared.load(std::memory_order_relaxed),
			g_declines.load(std::memory_order_relaxed),
			g_resourceBuilds.load(std::memory_order_relaxed),
			g_resourceFailures.load(std::memory_order_relaxed),
			g_draws.load(std::memory_order_relaxed),
			g_drawFailures.load(std::memory_order_relaxed),
			g_isolationFailures.load(std::memory_order_relaxed),
			g_hostStateUncertain.load(std::memory_order_relaxed),
			g_releases.load(std::memory_order_relaxed),
			g_lastDecline.load(std::memory_order_relaxed),
			g_exceptionCode.load(std::memory_order_relaxed),
			g_lastHr.load(std::memory_order_relaxed),
			g_outputWidth.load(std::memory_order_relaxed),
			g_outputHeight.load(std::memory_order_relaxed),
			g_format.load(std::memory_order_relaxed),
			g_ready.load(std::memory_order_acquire),
			g_outputValid.load(std::memory_order_acquire),
			g_failureLatched.load(std::memory_order_relaxed),
		};
	}
}
