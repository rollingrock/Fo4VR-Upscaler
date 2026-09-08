#include "Upscaler/DepthResolve.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/DepthResolveShader.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/RenderTargets.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>

namespace Upscaler::DepthResolve
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

		// Render-thread-owned. Every object below is built on the render thread, used on
		// it and dropped on it; nothing here is reachable from a settings or listener
		// thread, which is why none of it is atomic and the counters that ARE read from
		// another thread are the only atomics in the file.
		HMODULE g_compilerModule = nullptr;
		D3DCompileFn g_compile = nullptr;

		D3D11StateIsolation::Context g_isolation;
		ComPtr<ID3D11Device> g_device;
		ComPtr<ID3D11VertexShader> g_vertexShader;
		ComPtr<ID3D11PixelShader> g_pixelShader;
		ComPtr<ID3D11Texture2D> g_scratch;
		ComPtr<ID3D11ShaderResourceView> g_scratchDepthSrv;
		ComPtr<ID3D11DepthStencilView> g_ownedDsv;
		ComPtr<ID3D11DepthStencilState> g_depthStencilState;
		ComPtr<ID3D11RasterizerState> g_rasterizerState;
		ComPtr<ID3D11Buffer> g_constants;
		DepthResolveShader::Constants g_uploaded{};
		std::uint32_t g_builtWidth = 0;
		std::uint32_t g_builtHeight = 0;
		std::uint32_t g_builtFormat = 0;
		// Latched on the first failed build and cleared only by Release, so a shader the
		// compiler refuses is one compile attempt per key flip and not one per frame.
		bool g_creationFailed = false;
		bool g_creationFailureLogged = false;
		bool g_declineLogged = false;

		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_resolves{ 0 };
		std::atomic<std::uint64_t> g_declines{ 0 };
		std::atomic<std::uint32_t> g_lastDecline{ 0 };
		std::atomic<std::uint32_t> g_exceptionCode{ 0 };
		std::atomic<std::int32_t> g_lastHr{ 0 };
		std::atomic<std::uint32_t> g_fullWidth{ 0 };
		std::atomic<std::uint32_t> g_fullHeight{ 0 };
		std::atomic<std::uint32_t> g_reducedEyeWidth{ 0 };
		std::atomic<std::uint32_t> g_reducedHeight{ 0 };
		std::atomic<bool> g_ready{ false };

		void RecordDecline(Decline a_reason) noexcept
		{
			g_declines.fetch_add(1, std::memory_order_relaxed);
			g_lastDecline.store(static_cast<std::uint32_t>(a_reason), std::memory_order_relaxed);
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
				}
			}
			if (!g_compile) {
				logger::error("seam-exit depth resolve: d3dcompiler_47.dll / D3DCompile unavailable"sv);
				return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
			}
			ComPtr<ID3DBlob> errors;
			const HRESULT hr = g_compile(
				DepthResolveShader::kSource,
				sizeof(DepthResolveShader::kSource) - 1,
				"Fo4VRDepthResolve",
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
					FMT_STRING("seam-exit depth resolve: {} compile failed, HRESULT {:08X}: {}"),
					a_entry,
					static_cast<std::uint32_t>(hr),
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler message");
			}
			return hr;
		}

		// Everything the pass owns, built against DS1's own description so a build that
		// moved the depth-stencil's format or extent rebuilds rather than reinterpreting.
		// The engine's writable DSV is used when the render-target record carries one;
		// only when it does not is a DSV of our own created, and then at
		// D24_UNORM_S8_UINT, which is the typed view of the R24G8_TYPELESS resource.
		[[nodiscard]] bool EnsureObjects(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			ID3D11Texture2D* a_depthTexture,
			const D3D11_TEXTURE2D_DESC& a_depthDesc) noexcept
		{
			if (g_device.Get() != a_device) {
				Release();
			}
			if (g_builtWidth != a_depthDesc.Width || g_builtHeight != a_depthDesc.Height ||
				g_builtFormat != static_cast<std::uint32_t>(a_depthDesc.Format)) {
				// An extent or format change invalidates the scratch, its view and the
				// DSV built on the old resource together; nothing here is salvageable
				// piecemeal, so the whole set goes and is rebuilt below.
				Release();
			}
			if (!g_isolation.Initialize(a_device, a_context)) {
				g_lastHr.store(static_cast<std::int32_t>(g_isolation.LastError()), std::memory_order_relaxed);
				return false;
			}
			if (g_ready.load(std::memory_order_relaxed)) {
				return true;
			}
			if (g_creationFailed) {
				return false;
			}

			HRESULT hr = S_OK;
			ComPtr<ID3DBlob> vertexCode;
			ComPtr<ID3DBlob> pixelCode;
			ComPtr<ID3D11VertexShader> vertexShader;
			ComPtr<ID3D11PixelShader> pixelShader;
			hr = CompileShader("VSMain", "vs_5_0", vertexCode.GetAddressOf());
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

			// The scratch copy carries DS1's own bind flags, not a reduced set: the copy
			// below is a whole-resource CopyResource between two depth-stencil textures,
			// and matching the description exactly is what keeps it one.
			ComPtr<ID3D11Texture2D> scratch;
			if (SUCCEEDED(hr)) {
				auto scratchDesc = a_depthDesc;
				scratchDesc.MipLevels = 1;
				scratchDesc.ArraySize = 1;
				scratchDesc.SampleDesc.Count = 1;
				scratchDesc.SampleDesc.Quality = 0;
				scratchDesc.Usage = D3D11_USAGE_DEFAULT;
				scratchDesc.CPUAccessFlags = 0;
				scratchDesc.MiscFlags = 0;
				scratchDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
				hr = a_device->CreateTexture2D(&scratchDesc, nullptr, scratch.GetAddressOf());
			}

			ComPtr<ID3D11ShaderResourceView> scratchSrv;
			if (SUCCEEDED(hr)) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MostDetailedMip = 0;
				srvDesc.Texture2D.MipLevels = 1;
				hr = a_device->CreateShaderResourceView(scratch.Get(), &srvDesc, scratchSrv.GetAddressOf());
			}

			// Only when the engine's record has no writable DSV of its own. It has one on
			// this build - the seam-exit stencil clear uses it every published frame - so
			// this is the branch that keeps a record shaped differently from working.
			ComPtr<ID3D11DepthStencilView> ownedDsv;
			const auto record = RenderTargets::LookupDepth(Addr::kDS_MainScene);
			if (SUCCEEDED(hr) && !record.dsv) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
				dsvDesc.Texture2D.MipSlice = 0;
				hr = a_device->CreateDepthStencilView(a_depthTexture, &dsvDesc, ownedDsv.GetAddressOf());
			}

			// DepthFunc ALWAYS with write-all, so every full-extent texel takes the
			// shader's SV_Depth whatever the packed value under it was. StencilFunc ALWAYS
			// with PassOp REPLACE and reference 0 (supplied at the bind, not here) is what
			// zeroes the stencil plane in the same draw - the whole of the step 9c clear,
			// paid for by a pass that was going to run anyway.
			ComPtr<ID3D11DepthStencilState> depthStencilState;
			if (SUCCEEDED(hr)) {
				D3D11_DEPTH_STENCIL_DESC depth{};
				depth.DepthEnable = TRUE;
				depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
				depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
				depth.StencilEnable = TRUE;
				depth.StencilReadMask = 0xFF;
				depth.StencilWriteMask = 0xFF;
				const D3D11_DEPTH_STENCILOP_DESC face{
					D3D11_STENCIL_OP_KEEP,     // fail: unreachable under ALWAYS
					D3D11_STENCIL_OP_KEEP,     // depth fail: unreachable under ALWAYS
					D3D11_STENCIL_OP_REPLACE,  // pass: the reference, which is 0
					D3D11_COMPARISON_ALWAYS
				};
				depth.FrontFace = face;
				depth.BackFace = face;
				hr = a_device->CreateDepthStencilState(&depth, depthStencilState.GetAddressOf());
			}

			ComPtr<ID3D11RasterizerState> rasterizerState;
			if (SUCCEEDED(hr)) {
				D3D11_RASTERIZER_DESC raster{};
				raster.FillMode = D3D11_FILL_SOLID;
				raster.CullMode = D3D11_CULL_NONE;
				raster.FrontCounterClockwise = FALSE;
				// Off, both of them, and for the same reason: the triangle covers the
				// whole surface and the shader's SV_Depth is the value being written, so
				// neither a scissor left over from a state object nor a clip against the
				// vertex depth may reach it.
				raster.DepthClipEnable = FALSE;
				raster.ScissorEnable = FALSE;
				hr = a_device->CreateRasterizerState(&raster, rasterizerState.GetAddressOf());
			}

			ComPtr<ID3D11Buffer> constants;
			if (SUCCEEDED(hr)) {
				const DepthResolveShader::Constants initial{};
				D3D11_BUFFER_DESC constantsDesc{};
				constantsDesc.ByteWidth = sizeof(DepthResolveShader::Constants);
				constantsDesc.Usage = D3D11_USAGE_DEFAULT;
				constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				D3D11_SUBRESOURCE_DATA initialData{};
				initialData.pSysMem = &initial;
				hr = a_device->CreateBuffer(&constantsDesc, &initialData, constants.GetAddressOf());
			}

			g_lastHr.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);
			if (FAILED(hr) || !vertexShader || !pixelShader || !scratch || !scratchSrv ||
				!depthStencilState || !rasterizerState || !constants) {
				g_creationFailed = true;
				if (!g_creationFailureLogged) {
					g_creationFailureLogged = true;
					logger::error(
						FMT_STRING("seam-exit depth resolve: object creation failed for a {}x{} depth-stencil, HRESULT {:08X}; the key does nothing this session"),
						a_depthDesc.Width, a_depthDesc.Height, static_cast<std::uint32_t>(hr));
				}
				return false;
			}

			g_device = a_device;
			g_vertexShader = std::move(vertexShader);
			g_pixelShader = std::move(pixelShader);
			g_scratch = std::move(scratch);
			g_scratchDepthSrv = std::move(scratchSrv);
			g_ownedDsv = std::move(ownedDsv);
			g_depthStencilState = std::move(depthStencilState);
			g_rasterizerState = std::move(rasterizerState);
			g_constants = std::move(constants);
			g_uploaded = DepthResolveShader::Constants{};
			g_builtWidth = a_depthDesc.Width;
			g_builtHeight = a_depthDesc.Height;
			g_builtFormat = static_cast<std::uint32_t>(a_depthDesc.Format);
			g_ready.store(true, std::memory_order_relaxed);
			logger::info(
				FMT_STRING("seam-exit depth resolve: ready for a {}x{} depth-stencil, own depth-stencil view {}"),
				g_builtWidth, g_builtHeight, g_ownedDsv ? "created"sv : "not needed"sv);
			return true;
		}

		// Keep this boundary free of destructible C++ locals: a driver SEH must still run
		// End, because the host's context state is what the rest of the frame draws under.
		struct IsolatedResult
		{
			std::uint32_t exceptionCode = 0;
			bool began = false;
			bool ended = false;
			bool drew = false;
			bool stateUncertain = false;
		};

		[[nodiscard]] IsolatedResult ResolveIsolated(
			ID3D11DeviceContext* a_context,
			ID3D11Texture2D* a_depthTexture,
			ID3D11DepthStencilView* a_dsv,
			std::uint32_t a_width,
			std::uint32_t a_height) noexcept
		{
			IsolatedResult result{};
			__try {
				result.began = g_isolation.Begin();
				if (result.began) {
					__try {
						// Inside the bracket, where nothing of the engine's is bound: a
						// whole-resource copy with DS1 still bound as a depth-stencil view
						// would be a debug-layer hazard and a driver's licence to skip it.
						a_context->CopyResource(g_scratch.Get(), a_depthTexture);

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
						ID3D11ShaderResourceView* source[] = { g_scratchDepthSrv.Get() };
						a_context->PSSetShaderResources(0, 1, source);
						// No colour target: the pass writes SV_Depth and the stencil the
						// state replaces, and nothing else.
						a_context->OMSetRenderTargets(0, nullptr, a_dsv);
						a_context->OMSetDepthStencilState(g_depthStencilState.Get(), 0);
						a_context->RSSetState(g_rasterizerState.Get());
						D3D11_VIEWPORT viewport{};
						viewport.Width = static_cast<float>(a_width);
						viewport.Height = static_cast<float>(a_height);
						viewport.MinDepth = 0.0f;
						viewport.MaxDepth = 1.0f;
						a_context->RSSetViewports(1, &viewport);

						result.drew = true;
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

	void Release() noexcept
	{
		// The build latch first: a key flipped off and on again is a fresh attempt, which
		// is the only way a session recovers from a transient creation failure.
		g_creationFailed = false;
		g_creationFailureLogged = false;
		// Called once per published frame while the key is off, so the nothing-to-do case
		// costs one branch rather than nine ComPtr stores.
		if (!g_device) {
			return;
		}
		g_ready.store(false, std::memory_order_relaxed);
		g_constants.Reset();
		g_rasterizerState.Reset();
		g_depthStencilState.Reset();
		g_ownedDsv.Reset();
		g_scratchDepthSrv.Reset();
		g_scratch.Reset();
		g_pixelShader.Reset();
		g_vertexShader.Reset();
		g_device.Reset();
		g_uploaded = DepthResolveShader::Constants{};
		g_builtWidth = 0;
		g_builtHeight = 0;
		g_builtFormat = 0;
	}

	void Run() noexcept
	{
		g_attempts.fetch_add(1, std::memory_order_relaxed);

		const auto depth = RenderTargets::LookupDepth(Addr::kDS_MainScene);
		auto* const device = Engine::Device();
		auto* const context = Engine::ImmediateContext();
		if (!depth.valid || !depth.tex || !device || !context ||
			D3D11StateIsolation::HostStateUncertain()) {
			RecordDecline(Decline::kNoDepthStencil);
			if (!g_declineLogged) {
				g_declineLogged = true;
				logger::warn(
					FMT_STRING("seam-exit depth resolve declined: depth {} valid, texture {}, device {}, context {}"),
					depth.valid ? "is"sv : "is not"sv,
					depth.tex ? "present"sv : "absent"sv,
					device ? "present"sv : "absent"sv,
					context ? "present"sv : "absent"sv);
			}
			return;
		}

		D3D11_TEXTURE2D_DESC depthDesc{};
		depth.tex->GetDesc(&depthDesc);

		// The packed rectangle, from the transaction's own snapped extents rather than
		// from arithmetic on the configured ratio: the snap is what the engine was
		// actually given, and a ratio that was never snapped is a geometry this pass
		// cannot state, so it declines instead of guessing one.
		const auto dynamicResolution = DynamicResolution::Snapshot();
		const DepthResolveShader::Constants geometry{
			dynamicResolution.snappedOutputWidth,
			dynamicResolution.snappedOutputHeight,
			dynamicResolution.snappedRenderWidth,
			dynamicResolution.snappedRenderHeight
		};
		if (!dynamicResolution.snapped || geometry.fullEyeWidth == 0 || geometry.fullHeight == 0 ||
			geometry.reducedEyeWidth == 0 || geometry.reducedHeight == 0 ||
			geometry.reducedEyeWidth > geometry.fullEyeWidth ||
			geometry.reducedHeight > geometry.fullHeight ||
			depthDesc.Width != geometry.fullEyeWidth * 2u || depthDesc.Height != geometry.fullHeight ||
			depthDesc.Format != DXGI_FORMAT_R24G8_TYPELESS || depthDesc.MipLevels != 1 ||
			depthDesc.ArraySize != 1 || depthDesc.SampleDesc.Count != 1) {
			RecordDecline(Decline::kExtentMismatch);
			return;
		}

		if (!EnsureObjects(device, context, depth.tex, depthDesc)) {
			RecordDecline(Decline::kResourceCreation);
			return;
		}

		auto* const dsv = depth.dsv ? depth.dsv : g_ownedDsv.Get();
		if (!dsv) {
			RecordDecline(Decline::kNoDepthStencil);
			return;
		}

		if (geometry != g_uploaded) {
			context->UpdateSubresource(g_constants.Get(), 0, nullptr, &geometry, 0, 0);
			g_uploaded = geometry;
		}

		const auto result = ResolveIsolated(context, depth.tex, dsv, depthDesc.Width, depthDesc.Height);
		if (result.exceptionCode != 0) {
			g_exceptionCode.store(result.exceptionCode, std::memory_order_relaxed);
		}
		if (!result.began || !result.ended || !result.drew || result.stateUncertain) {
			RecordDecline(Decline::kIsolation);
			// The host state is the shared immediate context's: once it cannot be proved,
			// nothing this module owns may be bound again this session.
			if (result.stateUncertain) {
				Release();
			}
			return;
		}

		g_fullWidth.store(depthDesc.Width, std::memory_order_relaxed);
		g_fullHeight.store(depthDesc.Height, std::memory_order_relaxed);
		g_reducedEyeWidth.store(geometry.reducedEyeWidth, std::memory_order_relaxed);
		g_reducedHeight.store(geometry.reducedHeight, std::memory_order_relaxed);
		g_resolves.fetch_add(1, std::memory_order_relaxed);
	}

	State Snapshot() noexcept
	{
		State state{};
		state.attempts = g_attempts.load(std::memory_order_relaxed);
		state.resolves = g_resolves.load(std::memory_order_relaxed);
		state.declines = g_declines.load(std::memory_order_relaxed);
		state.lastDecline = g_lastDecline.load(std::memory_order_relaxed);
		state.exceptionCode = g_exceptionCode.load(std::memory_order_relaxed);
		state.lastHr = g_lastHr.load(std::memory_order_relaxed);
		state.fullWidth = g_fullWidth.load(std::memory_order_relaxed);
		state.fullHeight = g_fullHeight.load(std::memory_order_relaxed);
		state.reducedEyeWidth = g_reducedEyeWidth.load(std::memory_order_relaxed);
		state.reducedHeight = g_reducedHeight.load(std::memory_order_relaxed);
		state.ready = g_ready.load(std::memory_order_relaxed);
		return state;
	}
}
