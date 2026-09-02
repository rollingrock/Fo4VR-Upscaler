// Standalone sub-rect harness. It creates its own D3D11 device and drives the shipping
// backend directly - no game, no script extender, no VR runtime, no headset.
//
// Two questions, and only one of them needs a vendor SDK.
//
// The stereo plumbing - the rect arithmetic, the per-eye submission, and whether writing
// one eye can disturb the other - is ours, and it is testable today against NullBackend on
// a real device. Those cases run in every configuration and are the reason this target
// exists before any SDK does. They drive the same backend library the plugin links, so they
// are a proof about what ships rather than about a lookalike.
//
// The vendor question - whether a reconstruction runtime accepts a non-zero output sub-rect
// base, which decides whether every frame pays two full-resolution copies per eye - needs
// the SDK and is gated at the bottom. Nothing in the research corpus has been observed
// doing a non-zero colour/output sub-rect NGX D3D11 evaluate, so the shipping path assumes
// those copies and that backend's Caps::supportsSubrects stays false until this says otherwise.
// Depth is separate: the whole DS1 resource plus a per-eye extent is the verified contract.
//
// The double-wide dimensions here are the real ones, read from a running Fallout 4 VR:
// 3704 x 2056 total, 1852 x 2056 per eye.

#include "Upscaler/Backend/BackendTypes.h"
#include "Upscaler/Backend/FidelityFxD3D11.h"
#include "Upscaler/Backend/IBackend.h"
#include "Upscaler/Backend/NgxD3D11.h"
#include "Upscaler/Backend/NullBackend.h"
#include "Upscaler/CameraProjection.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/Intermediates.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/SamplerBias.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/TAAPassthroughShader.h"

#include <Windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <thread>
#include <utility>
#include <vector>
#include <wrl/client.h>

// A self-contained runner rather than a test framework. This target's claim is that it
// needs nothing but a GPU, and linking one cost a link failure against the separately
// compiled STL that the plugin itself never hits - a poor trade for assertion macros.
// REQUIRE abandons the case, CHECK records and carries on.
namespace harness
{
	struct Failure : std::exception
	{
	};

	inline int g_checks = 0;
	inline int g_failures = 0;
	inline int g_skips = 0;

	inline bool Check(bool a_ok, const char* a_expr, int a_line)
	{
		++g_checks;
		if (!a_ok) {
			++g_failures;
			std::printf("    FAIL  line %d:  %s\n", a_line, a_expr);
		}
		return a_ok;
	}

	inline void Skip(const char* a_why)
	{
		++g_skips;
		std::printf("    SKIP  %s\n", a_why);
	}
}

#define CHECK(expr) (void)harness::Check((expr), #expr, __LINE__)
#define CHECK_FALSE(expr) (void)harness::Check(!(expr), "!(" #expr ")", __LINE__)
#define REQUIRE(expr)                                   \
	do {                                                \
		if (!harness::Check((expr), #expr, __LINE__)) { \
			throw harness::Failure{};                   \
		}                                               \
	} while (false)
#define SKIP(why) harness::Skip(why)

namespace
{
	using Microsoft::WRL::ComPtr;
	using namespace Upscaler::Backend;

	// The measured FO4VR stereo layout. Per-eye is half the width and the full height;
	// the engine renders both eyes into one double-wide target.
	constexpr std::uint32_t kEyeWidth = 1852;
	constexpr std::uint32_t kEyeHeight = 2056;
	constexpr std::uint32_t kWideWidth = kEyeWidth * 2;

	static_assert(kEyeWidth * 2 == kWideWidth, "per-eye width must halve the double-wide");

	// A small stand-in for the real thing. The arithmetic under test is identical and a
	// 3704 x 2056 readback per assertion turns a fast harness into a slow one; the full
	// size is allocated once, in its own case, to prove it can be.
	constexpr std::uint32_t kSmallEyeW = 64;
	constexpr std::uint32_t kSmallEyeH = 32;
	constexpr std::uint32_t kSmallWideW = kSmallEyeW * 2;

	constexpr auto kVerifiedLeftRect = Upscaler::StereoLayout::RectFor(
		Upscaler::StereoLayout::Eye::kLeft,
		Upscaler::StereoLayout::Order::kLeftRight,
		kWideWidth,
		kEyeHeight);
	constexpr auto kVerifiedRightRect = Upscaler::StereoLayout::RectFor(
		Upscaler::StereoLayout::Eye::kRight,
		Upscaler::StereoLayout::Order::kLeftRight,
		kWideWidth,
		kEyeHeight);
	static_assert(kVerifiedLeftRect.x == 0 && kVerifiedLeftRect.w == kEyeWidth);
	static_assert(kVerifiedRightRect.x == kEyeWidth && kVerifiedRightRect.w == kEyeWidth);

	constexpr std::uint32_t kLeftSentinel = 0xFF0000FFu;
	constexpr std::uint32_t kRightSentinel = 0xFF00FF00u;
	constexpr std::uint32_t kSource = 0xFFFF0000u;
	constexpr std::uint32_t kLeftMotion = 0xB8003C00u;   // half2(1.0, -0.5)
	constexpr std::uint32_t kRightMotion = 0x3400BC00u;  // half2(-1.0, 0.25)
	constexpr auto kVerifiedMotionScale = NormalizedUvMotionScale(kEyeWidth, kEyeHeight);
	static_assert(kVerifiedMotionScale.x == 1852.0f && kVerifiedMotionScale.y == 2056.0f);

	// Which driver the cases below actually ran on, reported once at the end so a green
	// result is never mistaken for a green result on real hardware.
	bool g_usedWarp = false;

	// No swapchain and no window: every result is read back rather than presented, which
	// is also what lets this run unattended.
	//
	// Falls back to WARP because a CI runner has no GPU and the assertions here are about
	// copy semantics, which the reference rasteriser implements exactly. The vendor cases
	// must NOT accept that fallback - a reconstruction runtime needs a real adapter, and a
	// WARP pass there would be meaningless.
	bool MakeDevice(ComPtr<ID3D11Device>& a_device, ComPtr<ID3D11DeviceContext>& a_immediate)
	{
		UINT flags = 0;
#ifdef UPSCALER_HARNESS_D3D_DEBUG
		flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };

		for (const auto driver : drivers) {
			D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
			a_device.Reset();
			a_immediate.Reset();

			const HRESULT hr = D3D11CreateDevice(
				nullptr,
				driver,
				nullptr,
				flags,
				wanted,
				static_cast<UINT>(std::size(wanted)),
				D3D11_SDK_VERSION,
				a_device.GetAddressOf(),
				&obtained,
				a_immediate.GetAddressOf());

			if (SUCCEEDED(hr) && a_device && a_immediate) {
				g_usedWarp = (driver == D3D_DRIVER_TYPE_WARP);
				return true;
			}
		}

		return false;
	}

	ComPtr<ID3D11Texture2D> MakeTexture(
		ID3D11Device* a_device,
		std::uint32_t a_width,
		std::uint32_t a_height,
		DXGI_FORMAT a_format = DXGI_FORMAT_R8G8B8A8_UNORM,
		std::uint32_t a_bindFlags = D3D11_BIND_SHADER_RESOURCE)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = a_width;
		d.Height = a_height;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = a_format;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = a_bindFlags;

		ComPtr<ID3D11Texture2D> tex;
		return SUCCEEDED(a_device->CreateTexture2D(&d, nullptr, tex.GetAddressOf())) ? tex : nullptr;
	}

	// Fills every four-byte pixel with one raw value by uploading an immutable copy.
	void Fill(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_tex, std::uint32_t a_value)
	{
		D3D11_TEXTURE2D_DESC d{};
		a_tex->GetDesc(&d);

		const std::vector<std::uint32_t> pixels(static_cast<std::size_t>(d.Width) * d.Height, a_value);

		D3D11_TEXTURE2D_DESC up = d;
		up.Usage = D3D11_USAGE_IMMUTABLE;
		up.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		up.CPUAccessFlags = 0;

		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = pixels.data();
		init.SysMemPitch = d.Width * 4u;

		ComPtr<ID3D11Texture2D> staged;
		REQUIRE(SUCCEEDED(a_device->CreateTexture2D(&up, &init, staged.GetAddressOf())));
		a_ctx->CopyResource(a_tex, staged.Get());
	}

	void UploadRaw(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_ctx,
		ID3D11Texture2D* a_tex,
		const std::vector<std::uint32_t>& a_pixels)
	{
		D3D11_TEXTURE2D_DESC d{};
		a_tex->GetDesc(&d);
		REQUIRE(a_pixels.size() == static_cast<std::size_t>(d.Width) * d.Height);

		D3D11_TEXTURE2D_DESC up = d;
		up.Usage = D3D11_USAGE_IMMUTABLE;
		up.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		up.CPUAccessFlags = 0;

		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = a_pixels.data();
		init.SysMemPitch = d.Width * 4u;

		ComPtr<ID3D11Texture2D> staged;
		REQUIRE(SUCCEEDED(a_device->CreateTexture2D(&up, &init, staged.GetAddressOf())));
		a_ctx->CopyResource(a_tex, staged.Get());
	}

	// Reads a whole texture back to system memory, one uint32 per pixel.
	std::vector<std::uint32_t> Readback(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_tex)
	{
		D3D11_TEXTURE2D_DESC d{};
		a_tex->GetDesc(&d);

		D3D11_TEXTURE2D_DESC s = d;
		s.Usage = D3D11_USAGE_STAGING;
		s.BindFlags = 0;
		s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		s.MiscFlags = 0;

		ComPtr<ID3D11Texture2D> staging;
		REQUIRE(SUCCEEDED(a_device->CreateTexture2D(&s, nullptr, staging.GetAddressOf())));
		a_ctx->CopyResource(staging.Get(), a_tex);

		D3D11_MAPPED_SUBRESOURCE m{};
		REQUIRE(SUCCEEDED(a_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)));

		std::vector<std::uint32_t> out(static_cast<std::size_t>(d.Width) * d.Height, 0u);
		const auto* src = static_cast<const std::uint8_t*>(m.pData);
		for (std::uint32_t y = 0; y < d.Height; ++y) {
			const auto* row = reinterpret_cast<const std::uint32_t*>(src + static_cast<std::size_t>(y) * m.RowPitch);
			for (std::uint32_t x = 0; x < d.Width; ++x) {
				out[static_cast<std::size_t>(y) * d.Width + x] = row[x];
			}
		}

		a_ctx->Unmap(staging.Get(), 0);
		return out;
	}

	void TaaPassthroughConvertsAndWritesBothMRTs()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
			const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
			ID3DBlob**, ID3DBlob**);
		const HMODULE compilerModule = ::LoadLibraryW(L"d3dcompiler_47.dll");
		REQUIRE(compilerModule != nullptr);
		const auto compile = reinterpret_cast<D3DCompileFn>(
			::GetProcAddress(compilerModule, "D3DCompile"));
		REQUIRE(compile != nullptr);

		ComPtr<ID3DBlob> vertexCode;
		ComPtr<ID3DBlob> pixelCode;
		ComPtr<ID3DBlob> errors;
		REQUIRE(SUCCEEDED(compile(
			Upscaler::TAAPassthroughShader::kSource,
			sizeof(Upscaler::TAAPassthroughShader::kSource) - 1,
			"Fo4VRTaaReplacementHarness", nullptr, nullptr, "VSMain", "vs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
			vertexCode.GetAddressOf(), errors.GetAddressOf())));
		errors.Reset();
		REQUIRE(SUCCEEDED(compile(
			Upscaler::TAAPassthroughShader::kSource,
			sizeof(Upscaler::TAAPassthroughShader::kSource) - 1,
			"Fo4VRTaaReplacementHarness", nullptr, nullptr, "PSMain", "ps_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
			pixelCode.GetAddressOf(), errors.GetAddressOf())));

		ComPtr<ID3D11VertexShader> vertexShader;
		ComPtr<ID3D11PixelShader> pixelShader;
		REQUIRE(SUCCEEDED(device->CreateVertexShader(
			vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
			vertexShader.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreatePixelShader(
			pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr,
			pixelShader.GetAddressOf())));

		constexpr std::uint32_t width = 16;
		constexpr std::uint32_t height = 8;
		auto source = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R11G11B10_FLOAT,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
		auto history = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_RENDER_TARGET);
		auto submitted = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_RENDER_TARGET);
		REQUIRE(source && history && submitted);

		ComPtr<ID3D11ShaderResourceView> sourceSrv;
		ComPtr<ID3D11RenderTargetView> sourceRtv;
		ComPtr<ID3D11RenderTargetView> historyRtv;
		ComPtr<ID3D11RenderTargetView> submittedRtv;
		REQUIRE(SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, sourceSrv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(source.Get(), nullptr, sourceRtv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(history.Get(), nullptr, historyRtv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(submitted.Get(), nullptr, submittedRtv.GetAddressOf())));
		constexpr float sourceValue[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
		immediate->ClearRenderTargetView(sourceRtv.Get(), sourceValue);

		ComPtr<ID3D11BlendState> blendState;
		D3D11_BLEND_DESC blend{};
		blend.IndependentBlendEnable = TRUE;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		blend.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		REQUIRE(SUCCEEDED(device->CreateBlendState(&blend, blendState.GetAddressOf())));

		ComPtr<ID3D11RasterizerState> rasterizerState;
		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = TRUE;
		REQUIRE(SUCCEEDED(device->CreateRasterizerState(&raster, rasterizerState.GetAddressOf())));

		ComPtr<ID3D11DepthStencilState> depthStencilState;
		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthEnable = FALSE;
		depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		REQUIRE(SUCCEEDED(device->CreateDepthStencilState(&depth, depthStencilState.GetAddressOf())));

		immediate->IASetInputLayout(nullptr);
		immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		immediate->VSSetShader(vertexShader.Get(), nullptr, 0);
		immediate->PSSetShader(pixelShader.Get(), nullptr, 0);
		ID3D11ShaderResourceView* sourceViews[] = { sourceSrv.Get() };
		immediate->PSSetShaderResources(0, 1, sourceViews);
		ID3D11RenderTargetView* outputs[] = { historyRtv.Get(), submittedRtv.Get() };
		immediate->OMSetRenderTargets(2, outputs, nullptr);
		constexpr float blendFactor[4]{};
		immediate->OMSetBlendState(blendState.Get(), blendFactor, 0xFFFFFFFFu);
		immediate->OMSetDepthStencilState(depthStencilState.Get(), 0);
		immediate->RSSetState(rasterizerState.Get());
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(width);
		viewport.Height = static_cast<float>(height);
		viewport.MaxDepth = 1.0f;
		immediate->RSSetViewports(1, &viewport);
		immediate->Draw(3, 0);
		ID3D11ShaderResourceView* nullSource = nullptr;
		immediate->PSSetShaderResources(0, 1, &nullSource);
		immediate->OMSetRenderTargets(0, nullptr, nullptr);

		const auto historyPixels = Readback(device.Get(), immediate.Get(), history.Get());
		const auto submittedPixels = Readback(device.Get(), immediate.Get(), submitted.Get());
		CHECK(historyPixels == submittedPixels);
		REQUIRE(!historyPixels.empty());
		const auto closeByte = [](std::uint32_t a_pixel, unsigned a_shift, int a_expected) {
			return std::abs(static_cast<int>((a_pixel >> a_shift) & 0xFFu) - a_expected) <= 1;
		};
		CHECK(std::all_of(historyPixels.begin(), historyPixels.end(), [&](std::uint32_t a_pixel) {
			return closeByte(a_pixel, 0, 64) && closeByte(a_pixel, 8, 128) &&
			       closeByte(a_pixel, 16, 191) && ((a_pixel >> 24) & 0xFFu) == 255u;
		}));
		CHECK(device->GetDeviceRemovedReason() == S_OK);
		// The returned ID3DBlob implementations live in d3dcompiler_47.dll. Keep the
		// module loaded until process exit instead of unloading it before their ComPtr
		// destructors call Release.
	}

	// Initialize plus CreateViews for two eyes, which is the contract Evaluate enforces.
	void BringUp(NullBackend& a_backend, ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, std::uint32_t a_eyeW, std::uint32_t a_eyeH)
	{
		REQUIRE(a_backend.Initialize(a_device, a_ctx, {}));

		Config cfg{};
		cfg.renderWidth = a_eyeW;
		cfg.renderHeight = a_eyeH;
		cfg.outputWidth = a_eyeW;
		cfg.outputHeight = a_eyeH;
		cfg.quality = QualityMode::kNativeAA;

		REQUIRE(a_backend.CreateViews(cfg, 2));
	}

	void DeviceComesUp()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		REQUIRE(device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0);
	}

	void DoubleWideAllocatesAtFullSize()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		// The two formats seen carrying a double-wide buffer in a running game.
		REQUIRE(MakeTexture(device.Get(), kWideWidth, kEyeHeight, DXGI_FORMAT_R8G8B8A8_UNORM));
		REQUIRE(MakeTexture(device.Get(), kWideWidth, kEyeHeight, DXGI_FORMAT_R11G11B10_FLOAT));
		REQUIRE(MakeTexture(device.Get(), kWideWidth, kEyeHeight, DXGI_FORMAT_R16G16_FLOAT));
		REQUIRE(MakeTexture(device.Get(), kWideWidth, kEyeHeight, DXGI_FORMAT_R24G8_TYPELESS,
			D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE));
	}

	void ShippingStereoGeometryIsExactAndFailClosed()
	{
		using Upscaler::StereoLayout::Eye;
		using Upscaler::StereoLayout::Order;
		using Upscaler::StereoLayout::RectFor;

		const auto left = RectFor(Eye::kLeft, Order::kLeftRight, kWideWidth, kEyeHeight);
		const auto right = RectFor(Eye::kRight, Order::kLeftRight, kWideWidth, kEyeHeight);
		CHECK(left.x == 0 && left.y == 0 && left.w == kEyeWidth && left.h == kEyeHeight);
		CHECK(right.x == kEyeWidth && right.y == 0 && right.w == kEyeWidth && right.h == kEyeHeight);

		const auto reversedLeft = RectFor(Eye::kLeft, Order::kRightLeft, kWideWidth, kEyeHeight);
		const auto reversedRight = RectFor(Eye::kRight, Order::kRightLeft, kWideWidth, kEyeHeight);
		CHECK(reversedLeft.x == kEyeWidth && reversedLeft.w == kEyeWidth);
		CHECK(reversedRight.x == 0 && reversedRight.w == kEyeWidth);

		// The active dynamic-resolution viewport is itself double-wide. Its eye split
		// therefore moves with the render width rather than retaining the allocation's
		// native eye stride.
		constexpr std::uint32_t renderWidth = 1240;
		constexpr std::uint32_t renderHeight = 1377;
		const auto reducedLeft = RectFor(Eye::kLeft, Order::kLeftRight, renderWidth * 2u, renderHeight);
		const auto reducedRight = RectFor(Eye::kRight, Order::kLeftRight, renderWidth * 2u, renderHeight);
		CHECK(reducedLeft.x == 0 && reducedLeft.w == renderWidth && reducedLeft.h == renderHeight);
		CHECK(reducedRight.x == renderWidth && reducedRight.w == renderWidth && reducedRight.h == renderHeight);

		CHECK(RectFor(Eye::kLeft, Order::kLeftRight, kWideWidth - 1, kEyeHeight).w == 0);
		CHECK(RectFor(Eye::kLeft, Order::kLeftRight, 0, kEyeHeight).w == 0);
		CHECK(RectFor(Eye::kLeft, Order::kLeftRight, kWideWidth, 0).w == 0);
		CHECK(RectFor(static_cast<Eye>(2), Order::kLeftRight, kWideWidth, kEyeHeight).w == 0);
		CHECK(RectFor(Eye::kLeft, static_cast<Order>(2), kWideWidth, kEyeHeight).w == 0);
	}

	void BackendOwnedJitterSequenceAndProjectionRoundTrip()
	{
		using Upscaler::Jitter::CameraPixelsToProjectionX;
		using Upscaler::Jitter::CameraPixelsToProjectionY;
		using Upscaler::Jitter::ProjectionToCameraPixelsX;
		using Upscaler::Jitter::ProjectionToCameraPixelsY;

		constexpr float kTolerance = 0.000001f;
		// The engine's own eight samples, from the generator's Halton(2,n+1)/Halton(3,n+1).
		constexpr float halton2[] = { 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f };
		constexpr float halton3[] = {
			1.0f / 3.0f,
			2.0f / 3.0f,
			1.0f / 9.0f,
			4.0f / 9.0f,
			7.0f / 9.0f,
			2.0f / 9.0f,
			5.0f / 9.0f,
			8.0f / 9.0f,
		};

		for (std::uint32_t phase = 0; phase < 8; ++phase) {
			const float projectionX = (2.0f * halton2[phase] - 1.0f) / static_cast<float>(kEyeWidth);
			const float projectionY = (2.0f * halton3[phase] - 1.0f) / static_cast<float>(kEyeHeight);
			const float jitterX = ProjectionToCameraPixelsX(projectionX, kEyeWidth);
			const float jitterY = ProjectionToCameraPixelsY(projectionY, kEyeHeight);
			CHECK(std::abs(jitterX - (0.5f - halton2[phase])) < kTolerance);
			CHECK(std::abs(jitterY - (halton3[phase] - 0.5f)) < kTolerance);
			CHECK(std::abs(jitterX) <= 0.5f && std::abs(jitterY) <= 0.5f);

			// The writer's conversion is the exact inverse of the reader's, so a committed
			// sample survives a round trip through the engine's two floats unchanged.
			CHECK(std::abs(ProjectionToCameraPixelsX(CameraPixelsToProjectionX(jitterX, kEyeWidth), kEyeWidth) - jitterX) < kTolerance);
			CHECK(std::abs(ProjectionToCameraPixelsY(CameraPixelsToProjectionY(jitterY, kEyeHeight), kEyeHeight) - jitterY) < kTolerance);
			CHECK(std::abs(CameraPixelsToProjectionX(jitterX, kEyeWidth) - projectionX) < kTolerance);
			CHECK(std::abs(CameraPixelsToProjectionY(jitterY, kEyeHeight) - projectionY) < kTolerance);

			// The ratio-cancellation identity, and the one case that catches a
			// double-compensating writer. BuildStereoCameraViewData applies
			// stored * W / floor(W*r) as the projection's fourth row, and the applied
			// offset is that row times renderWidth/2. Dividing by the FULL W makes the
			// ratio cancel exactly; dividing by renderWidth or by W*r would not.
			for (const float ratio : { 1.0f, 0.77f, 0.67f, 0.5f }) {
				const auto renderWidth = static_cast<std::uint32_t>(std::floor(static_cast<float>(kEyeWidth) * ratio));
				const auto renderHeight = static_cast<std::uint32_t>(std::floor(static_cast<float>(kEyeHeight) * ratio));
				const float rowX = CameraPixelsToProjectionX(jitterX, kEyeWidth) *
				                   static_cast<float>(kEyeWidth) / static_cast<float>(renderWidth);
				const float rowY = CameraPixelsToProjectionY(jitterY, kEyeHeight) *
				                   static_cast<float>(kEyeHeight) / static_cast<float>(renderHeight);
				const float appliedX = -rowX * static_cast<float>(renderWidth) * 0.5f;
				const float appliedY = rowY * static_cast<float>(renderHeight) * 0.5f;
				CHECK(std::abs(appliedX - jitterX) < kTolerance);
				CHECK(std::abs(appliedY - jitterY) < kTolerance);
				CHECK(std::abs(appliedX) <= 0.5f && std::abs(appliedY) <= 0.5f);
			}
		}

		// Negative control one: supplying the double-wide width doubles X and breaches the
		// canonical range on phase 7. This makes the divisor proof executable.
		const float phase7ProjectionX = (2.0f * halton2[7] - 1.0f) / static_cast<float>(kEyeWidth);
		CHECK(std::abs(ProjectionToCameraPixelsX(phase7ProjectionX, kWideWidth)) > 0.5f);

		// Negative control two: a writer that pre-divides by the ratio double-compensates.
		// At 0.67 the engine then applies +-0.5/0.67 = +-0.746 px, which the bounds check
		// in CommitFrame and AcquireFrameSample both reject.
		constexpr float kRatio = 0.67f;
		const auto reducedWidth = static_cast<std::uint32_t>(std::floor(static_cast<float>(kEyeWidth) * kRatio));
		const float doubleCompensatedRow = CameraPixelsToProjectionX(0.5f, reducedWidth) *
		                                   static_cast<float>(kEyeWidth) / static_cast<float>(reducedWidth);
		const float doubleCompensatedApplied = -doubleCompensatedRow * static_cast<float>(reducedWidth) * 0.5f;
		CHECK(std::abs(doubleCompensatedApplied) > 0.5f + 0.2f);

		// Negative control three: a writer that divides by the double-wide extent halves
		// the offset the engine then applies, so the picture looks correct at a glance and
		// samples half the pixel it should.
		CHECK(std::abs(ProjectionToCameraPixelsX(CameraPixelsToProjectionX(0.5f, kWideWidth), kEyeWidth) - 0.25f) < kTolerance);

#ifndef UPSCALER_NO_NGX
		// The vendor's own phase table. DLSS Programming Guide rev 310.6.0 section
		// 3.7.1.1: ceil(8*(output/render)^2), minimum 8/18/24/32/72 for
		// DLAA/Quality/Balanced/Performance/UltraPerformance at the ratios in that row.
		NgxD3D11Backend backend;
		CHECK(backend.JitterPhaseCount(1852, 1852) == 8);
		CHECK(backend.JitterPhaseCount(1240, 1852) == 18);
		CHECK(backend.JitterPhaseCount(1074, 1852) == 24);
		CHECK(backend.JitterPhaseCount(926, 1852) == 32);
		CHECK(backend.JitterPhaseCount(618, 1852) == 72);

		for (const std::uint32_t phaseCount : { 8u, 18u, 32u, 72u }) {
			std::vector<std::pair<float, float>> samples;
			samples.reserve(phaseCount);
			for (std::uint32_t n = 0; n < phaseCount; ++n) {
				float x = 0.0f;
				float y = 0.0f;
				backend.JitterOffset(n, phaseCount, x, y);
				CHECK(std::isfinite(x) && std::isfinite(y));
				CHECK(std::abs(x) <= 0.5f && std::abs(y) <= 0.5f);
				CHECK(std::abs(ProjectionToCameraPixelsX(CameraPixelsToProjectionX(x, kEyeWidth), kEyeWidth) - x) < kTolerance);
				CHECK(std::abs(ProjectionToCameraPixelsY(CameraPixelsToProjectionY(y, kEyeHeight), kEyeHeight) - y) < kTolerance);
				samples.emplace_back(x, y);
			}
			// A repeated phase inside one period is a shorter sequence than the count
			// claims, which is the shimmer this whole change exists to remove.
			bool distinct = true;
			for (std::size_t i = 0; i < samples.size(); ++i) {
				for (std::size_t j = i + 1; j < samples.size(); ++j) {
					if (samples[i] == samples[j]) {
						distinct = false;
					}
				}
			}
			CHECK(distinct);
		}

		// At DLAA the armed sequence must be the native one, bit for bit within tolerance.
		// A regression here means the writer changed DLAA, which it must not.
		for (std::uint32_t n = 0; n < 8; ++n) {
			float x = 0.0f;
			float y = 0.0f;
			backend.JitterOffset(n, 8, x, y);
			CHECK(std::abs(x - (0.5f - halton2[n])) < kTolerance);
			CHECK(std::abs(y - (halton3[n] - 0.5f)) < kTolerance);
		}
#endif
	}

	void ShippingIntermediatesExtractEvaluateAndReassembleBothOrders()
	{
		using Upscaler::Intermediates::ExtractColor;
		using Upscaler::Intermediates::ExtractMotion;
		using Upscaler::Intermediates::Get;
		using Upscaler::Intermediates::ReferenceDepth;
		using Upscaler::Intermediates::ReassembleOutput;
		using Upscaler::StereoLayout::Eye;
		using Upscaler::StereoLayout::Order;
		using Upscaler::StereoLayout::RectFor;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		auto wideSource = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH);
		auto wideDepth = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH, DXGI_FORMAT_R24G8_TYPELESS,
			D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
		auto wideMotion = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH, DXGI_FORMAT_R16G16_FLOAT);
		auto wideOutput = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH);
		auto leftSeed = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH);
		auto rightSeed = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH);
		auto leftMotionSeed = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH, DXGI_FORMAT_R16G16_FLOAT);
		auto rightMotionSeed = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH, DXGI_FORMAT_R16G16_FLOAT);
		REQUIRE(wideSource && wideDepth && wideMotion && wideOutput && leftSeed && rightSeed &&
			leftMotionSeed && rightMotionSeed);
		Fill(device.Get(), immediate.Get(), wideSource.Get(), 0u);
		Fill(device.Get(), immediate.Get(), wideMotion.Get(), 0u);
		Fill(device.Get(), immediate.Get(), leftSeed.Get(), kLeftSentinel);
		Fill(device.Get(), immediate.Get(), rightSeed.Get(), kRightSentinel);
		Fill(device.Get(), immediate.Get(), leftMotionSeed.Get(), kLeftMotion);
		Fill(device.Get(), immediate.Get(), rightMotionSeed.Get(), kRightMotion);

		const auto firstHalf = RectFor(Eye::kLeft, Order::kLeftRight, kSmallWideW, kSmallEyeH);
		const auto secondHalf = RectFor(Eye::kRight, Order::kLeftRight, kSmallWideW, kSmallEyeH);
		D3D11_BOX wholeEye{ 0, 0, 0, kSmallEyeW, kSmallEyeH, 1 };
		immediate->CopySubresourceRegion(wideSource.Get(), 0, firstHalf.x, 0, 0, leftSeed.Get(), 0, &wholeEye);
		immediate->CopySubresourceRegion(wideSource.Get(), 0, secondHalf.x, 0, 0, rightSeed.Get(), 0, &wholeEye);
		immediate->CopySubresourceRegion(wideMotion.Get(), 0, firstHalf.x, 0, 0, leftMotionSeed.Get(), 0, &wholeEye);
		immediate->CopySubresourceRegion(wideMotion.Get(), 0, secondHalf.x, 0, 0, rightMotionSeed.Get(), 0, &wholeEye);

		D3D11_TEXTURE2D_DESC sourceDesc{};
		D3D11_TEXTURE2D_DESC depthDesc{};
		D3D11_TEXTURE2D_DESC motionDesc{};
		wideSource->GetDesc(&sourceDesc);
		wideDepth->GetDesc(&depthDesc);
		wideMotion->GetDesc(&motionDesc);
		constexpr std::uint32_t reducedEyeWidth = kSmallEyeW - 8u;
		constexpr std::uint32_t reducedHeight = kSmallEyeH - 4u;
		const auto reducedLeft = RectFor(Eye::kLeft, Order::kLeftRight, reducedEyeWidth * 2u, reducedHeight);
		const auto reducedRight = RectFor(Eye::kRight, Order::kLeftRight, reducedEyeWidth * 2u, reducedHeight);
		CHECK(ReferenceDepth(wideDepth.Get(), depthDesc, reducedLeft).resource == wideDepth.Get());
		CHECK(ReferenceDepth(wideDepth.Get(), depthDesc, reducedRight).resource == wideDepth.Get());
		CHECK_FALSE(ReferenceDepth(
			wideDepth.Get(), depthDesc, Rect{ reducedEyeWidth + 1u, 0, reducedEyeWidth, reducedHeight }).resource);
		Config cfg{};
		cfg.renderWidth = kSmallEyeW;
		cfg.renderHeight = kSmallEyeH;
		cfg.outputWidth = kSmallEyeW;
		cfg.outputHeight = kSmallEyeH;
		cfg.quality = QualityMode::kNativeAA;

		Upscaler::Intermediates::Release();
		const auto buildsBefore = Upscaler::Intermediates::Snapshot().resourceBuilds;
		REQUIRE(Upscaler::Intermediates::EnsureFor(device.Get(), sourceDesc, motionDesc, cfg));
		const auto allocated = Upscaler::Intermediates::Snapshot();
		CHECK(allocated.ready);
		CHECK(allocated.resourceBuilds == buildsBefore + 1);
		CHECK(allocated.renderWidth == kSmallEyeW && allocated.renderHeight == kSmallEyeH);
		CHECK(allocated.outputWidth == kSmallEyeW && allocated.outputHeight == kSmallEyeH);
		CHECK(allocated.format == static_cast<std::uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM));
		CHECK(allocated.motionFormat == static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT));
		CHECK(allocated.leftMotionVectors != 0 && allocated.rightMotionVectors != 0);

		// An identical request is a cache hit: all six addresses and the build count stay.
		REQUIRE(Upscaler::Intermediates::EnsureFor(device.Get(), sourceDesc, motionDesc, cfg));
		const auto cached = Upscaler::Intermediates::Snapshot();
		CHECK(cached.resourceBuilds == allocated.resourceBuilds);
		CHECK(cached.leftColor == allocated.leftColor && cached.rightColor == allocated.rightColor);
		CHECK(cached.leftMotionVectors == allocated.leftMotionVectors && cached.rightMotionVectors == allocated.rightMotionVectors);
		CHECK(cached.leftOutput == allocated.leftOutput && cached.rightOutput == allocated.rightOutput);

		NullBackend backend;
		BringUp(backend, device.Get(), immediate.Get(), kSmallEyeW, kSmallEyeH);

		for (const auto order : { Order::kLeftRight, Order::kRightLeft }) {
			Fill(device.Get(), immediate.Get(), wideOutput.Get(), kSource);
			for (std::uint32_t index = 0; index < 2; ++index) {
				const Eye eye = index == 0 ? Eye::kLeft : Eye::kRight;
				const auto rect = RectFor(eye, order, kSmallWideW, kSmallEyeH);
				const auto staged = Get(eye);
				const auto depth = ReferenceDepth(wideDepth.Get(), depthDesc, rect);
				REQUIRE(staged.color && staged.depth && staged.motionVectors && staged.output);
				REQUIRE(depth.resource == wideDepth.Get());
				CHECK(depth.rect.x == rect.x && depth.rect.y == rect.y &&
					depth.rect.w == rect.w && depth.rect.h == rect.h);
				REQUIRE(ExtractColor(immediate.Get(), wideSource.Get(), rect, staged.color));
				REQUIRE(ExtractMotion(immediate.Get(), wideMotion.Get(), nullptr, rect, staged.motionVectors, {}));

				EyeView view{};
				view.index = index;
				view.color = staged.color;
				view.depth = depth.resource;
				view.motionVectors = staged.motionVectors;
				view.output = staged.output;
				view.depthRect = depth.rect;
				const auto mvScale = NormalizedUvMotionScale(cfg.renderWidth, cfg.renderHeight);
				view.mvScaleX = mvScale.x;
				view.mvScaleY = mvScale.y;
				REQUIRE(backend.Evaluate(immediate.Get(), view));
				REQUIRE(ReassembleOutput(immediate.Get(), staged.output, wideOutput.Get(), rect));
			}

			const auto leftStaged = Readback(device.Get(), immediate.Get(), Get(Eye::kLeft).color);
			const auto rightStaged = Readback(device.Get(), immediate.Get(), Get(Eye::kRight).color);
			const auto leftMotionStaged = Readback(device.Get(), immediate.Get(), Get(Eye::kLeft).motionVectors);
			const auto rightMotionStaged = Readback(device.Get(), immediate.Get(), Get(Eye::kRight).motionVectors);
			const auto expectedLeft = order == Order::kLeftRight ? kLeftSentinel : kRightSentinel;
			const auto expectedRight = order == Order::kLeftRight ? kRightSentinel : kLeftSentinel;
			const auto expectedLeftMotion = order == Order::kLeftRight ? kLeftMotion : kRightMotion;
			const auto expectedRightMotion = order == Order::kLeftRight ? kRightMotion : kLeftMotion;
			CHECK(!leftStaged.empty() && leftStaged.front() == expectedLeft && leftStaged.back() == expectedLeft);
			CHECK(!rightStaged.empty() && rightStaged.front() == expectedRight && rightStaged.back() == expectedRight);
			CHECK(!leftMotionStaged.empty() && leftMotionStaged.front() == expectedLeftMotion && leftMotionStaged.back() == expectedLeftMotion);
			CHECK(!rightMotionStaged.empty() && rightMotionStaged.front() == expectedRightMotion && rightMotionStaged.back() == expectedRightMotion);

			const auto pixels = Readback(device.Get(), immediate.Get(), wideOutput.Get());
			CHECK(pixels == Readback(device.Get(), immediate.Get(), wideSource.Get()));
		}

		const auto left = Get(Eye::kLeft);
		CHECK_FALSE(ExtractColor(immediate.Get(), wideSource.Get(), Rect{ 1, 0, kSmallEyeW * 2, kSmallEyeH }, left.color));
		CHECK_FALSE(ReferenceDepth(wideSource.Get(), sourceDesc, firstHalf).resource);
		CHECK_FALSE(ReferenceDepth(wideDepth.Get(), depthDesc, Rect{ 1, 0, kSmallEyeW, kSmallEyeH }).resource);
		CHECK_FALSE(ReferenceDepth(wideDepth.Get(), depthDesc, Rect{ 0, 0, kSmallWideW, kSmallEyeH }).resource);
		CHECK_FALSE(ExtractMotion(immediate.Get(), wideSource.Get(), nullptr, firstHalf, left.motionVectors, {}));
		CHECK_FALSE(ExtractMotion(immediate.Get(), wideMotion.Get(), nullptr, Rect{ 1, 0, kSmallEyeW * 2, kSmallEyeH }, left.motionVectors, {}));
		CHECK_FALSE(ReassembleOutput(immediate.Get(), left.output, wideOutput.Get(), Rect{ kSmallWideW, 0, kSmallEyeW, kSmallEyeH }));

		static_cast<void>(backend.DestroyViews());
		backend.Shutdown();
		Upscaler::Intermediates::Release();
		CHECK_FALSE(Upscaler::Intermediates::Snapshot().ready);
		CHECK(Get(Eye::kLeft).color == nullptr && Get(Eye::kLeft).motionVectors == nullptr &&
			Get(Eye::kRight).output == nullptr);
	}

	void MotionSanitizerPreservesBoundedFiniteAndRejectsInvalidBothOrders()
	{
		using Upscaler::Intermediates::ExtractMotion;
		using Upscaler::Intermediates::Get;
		using Upscaler::StereoLayout::Eye;
		using Upscaler::StereoLayout::Order;
		using Upscaler::StereoLayout::RectFor;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		auto wideColor = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH);
		auto wideMotion = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH, DXGI_FORMAT_R16G16_FLOAT);
		REQUIRE(wideColor && wideMotion);

		constexpr std::uint32_t inputPattern[] = {
			0xB8003800u,  // half2(+0.5, -0.5)
			0x00007E00u,  // half2(NaN, 0)
			0x7C000000u,  // half2(0, +Inf)
			0x0000FC00u,  // half2(-Inf, 0)
			0x3C00BC00u,  // half2(-1, +1)
			0xB4003400u,  // half2(+0.25, -0.25)
			0xC8004800u,  // half2(+8, -8)
		};
		auto boundedFinitePair = [](std::uint32_t a_raw) {
			const auto x = static_cast<std::uint16_t>(a_raw & 0xFFFFu);
			const auto y = static_cast<std::uint16_t>(a_raw >> 16u);
			return (x & 0x7C00u) != 0x7C00u && (y & 0x7C00u) != 0x7C00u &&
			       a_raw != 0xC8004800u;
		};

		std::vector<std::uint32_t> sourcePixels(static_cast<std::size_t>(kSmallWideW) * kSmallEyeH);
		for (std::uint32_t y = 0; y < kSmallEyeH; ++y) {
			for (std::uint32_t x = 0; x < kSmallWideW; ++x) {
				const std::uint32_t phase = x < kSmallEyeW ? x : x - kSmallEyeW + 3u;
				sourcePixels[static_cast<std::size_t>(y) * kSmallWideW + x] =
					inputPattern[phase % std::size(inputPattern)];
			}
		}
		UploadRaw(device.Get(), immediate.Get(), wideMotion.Get(), sourcePixels);

		D3D11_TEXTURE2D_DESC colorDesc{};
		D3D11_TEXTURE2D_DESC motionDesc{};
		wideColor->GetDesc(&colorDesc);
		wideMotion->GetDesc(&motionDesc);
		Config cfg{};
		cfg.renderWidth = kSmallEyeW;
		cfg.renderHeight = kSmallEyeH;
		cfg.outputWidth = kSmallEyeW;
		cfg.outputHeight = kSmallEyeH;
		cfg.quality = QualityMode::kNativeAA;

		Upscaler::Intermediates::Release();
		REQUIRE(Upscaler::Intermediates::EnsureFor(device.Get(), colorDesc, motionDesc, cfg));
		const auto before = Upscaler::Intermediates::Snapshot();

		Upscaler::D3D11StateIsolation::Context isolation;
		REQUIRE(isolation.Initialize(device.Get(), immediate.Get()));
		for (const auto order : { Order::kLeftRight, Order::kRightLeft }) {
			Upscaler::D3D11StateIsolation::Scope scope{ isolation };
			REQUIRE(static_cast<bool>(scope));
			for (std::uint32_t index = 0; index < 2; ++index) {
				const Eye eye = index == 0 ? Eye::kLeft : Eye::kRight;
				const auto rect = RectFor(eye, order, kSmallWideW, kSmallEyeH);
				const auto staged = Get(eye);
				REQUIRE(staged.motionVectors);
				REQUIRE(ExtractMotion(immediate.Get(), wideMotion.Get(), nullptr, rect, staged.motionVectors, {}));

				const auto actual = Readback(device.Get(), immediate.Get(), staged.motionVectors);
				std::vector<std::uint32_t> expected(static_cast<std::size_t>(kSmallEyeW) * kSmallEyeH);
				for (std::uint32_t y = 0; y < kSmallEyeH; ++y) {
					for (std::uint32_t x = 0; x < kSmallEyeW; ++x) {
						const auto raw = sourcePixels[static_cast<std::size_t>(y) * kSmallWideW + rect.x + x];
						expected[static_cast<std::size_t>(y) * kSmallEyeW + x] = boundedFinitePair(raw) ? raw : 0u;
					}
				}
				CHECK(actual == expected);
			}
			REQUIRE(scope.Close());
		}

		const auto after = Upscaler::Intermediates::Snapshot();
		CHECK(after.motionSanitizerBuilds == before.motionSanitizerBuilds);
		CHECK(after.motionSanitizerDraws == before.motionSanitizerDraws + 4u);
		CHECK(Readback(device.Get(), immediate.Get(), wideMotion.Get()) == sourcePixels);
		CHECK(device->GetDeviceRemovedReason() == S_OK);

		isolation.Shutdown();
		Upscaler::Intermediates::Release();
	}

	void MotionCompletionUsesDepthAndPreservesFiniteInputsBothOrders()
	{
		using Upscaler::Intermediates::ExtractMotion;
		using Upscaler::Intermediates::Get;
		using Upscaler::StereoLayout::Eye;
		using Upscaler::StereoLayout::Order;
		using Upscaler::StereoLayout::RectFor;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		auto wideColor = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH);
		auto wideMotion = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH, DXGI_FORMAT_R16G16_FLOAT);
		auto wideDepth = MakeTexture(
			device.Get(),
			kSmallWideW,
			kSmallEyeH,
			DXGI_FORMAT_R24G8_TYPELESS,
			D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
		REQUIRE(wideColor && wideMotion && wideDepth);

		std::vector<std::uint32_t> motionPixels(static_cast<std::size_t>(kSmallWideW) * kSmallEyeH, 0u);
		std::vector<std::uint32_t> depthPixels(static_cast<std::size_t>(kSmallWideW) * kSmallEyeH, 0u);
		for (std::uint32_t y = 0; y < kSmallEyeH; ++y) {
			const auto row = static_cast<std::size_t>(y) * kSmallWideW;
			motionPixels[row] = kLeftMotion;
			motionPixels[row + 1] = 0x00007E00u;  // half2(NaN, 0): completed to zero at depth 0
			motionPixels[row + kSmallEyeW] = kRightMotion;
			motionPixels[row + kSmallEyeW + 1] = 0x00007E00u;  // completed at depth 1
			for (std::uint32_t x = kSmallEyeW; x < kSmallWideW; ++x) {
				depthPixels[row + x] = 0x00FFFFFFu;  // R24_UNORM depth 1
			}
		}
		UploadRaw(device.Get(), immediate.Get(), wideMotion.Get(), motionPixels);
		UploadRaw(device.Get(), immediate.Get(), wideDepth.Get(), depthPixels);

		float current[16]{};
		float previous[16]{};
		for (std::uint32_t i = 0; i < 4; ++i) {
			current[i * 5] = 1.0f;
			previous[i * 5] = 1.0f;
		}
		// Row-vector convention: previousClip.x = currentClip.x + 0.2 * depth,
		// therefore previousUV.x-currentUV.x = 0.1 * depth.
		previous[8] = 0.2f;
		Upscaler::MotionReprojection::Data reprojection{};
		REQUIRE(Upscaler::MotionReprojection::Build(current, previous, reprojection));
		CHECK(reprojection.valid);

		float singular[16]{};
		Upscaler::MotionReprojection::Data rejected{};
		CHECK_FALSE(Upscaler::MotionReprojection::Build(singular, previous, rejected));
		CHECK_FALSE(rejected.valid);

		D3D11_TEXTURE2D_DESC colorDesc{};
		D3D11_TEXTURE2D_DESC motionDesc{};
		wideColor->GetDesc(&colorDesc);
		wideMotion->GetDesc(&motionDesc);
		Config cfg{};
		cfg.renderWidth = kSmallEyeW;
		cfg.renderHeight = kSmallEyeH;
		cfg.outputWidth = kSmallEyeW;
		cfg.outputHeight = kSmallEyeH;
		cfg.quality = QualityMode::kNativeAA;

		Upscaler::Intermediates::Release();
		REQUIRE(Upscaler::Intermediates::EnsureFor(device.Get(), colorDesc, motionDesc, cfg));
		const auto before = Upscaler::Intermediates::Snapshot();

		Upscaler::D3D11StateIsolation::Context isolation;
		REQUIRE(isolation.Initialize(device.Get(), immediate.Get()));
		constexpr std::uint32_t completedAtDepthOne = 0x00002E66u;  // half2(0.1, 0)
		for (const auto order : { Order::kLeftRight, Order::kRightLeft }) {
			Upscaler::D3D11StateIsolation::Scope scope{ isolation };
			REQUIRE(static_cast<bool>(scope));
			for (std::uint32_t index = 0; index < 2; ++index) {
				const Eye eye = index == 0 ? Eye::kLeft : Eye::kRight;
				const auto rect = RectFor(eye, order, kSmallWideW, kSmallEyeH);
				const auto staged = Get(eye);
				REQUIRE(staged.motionVectors);
				REQUIRE(ExtractMotion(
					immediate.Get(), wideMotion.Get(), wideDepth.Get(), rect, staged.motionVectors, reprojection));

				const auto actual = Readback(device.Get(), immediate.Get(), staged.motionVectors);
				const auto actualDepth = Readback(device.Get(), immediate.Get(), staged.depth);
				REQUIRE(actual.size() == static_cast<std::size_t>(kSmallEyeW) * kSmallEyeH);
				REQUIRE(actualDepth.size() == actual.size());
				const bool depthOne = rect.x == kSmallEyeW;
				const std::uint32_t preserved = depthOne ? kRightMotion : kLeftMotion;
				const std::uint32_t completed = depthOne ? completedAtDepthOne : 0u;
				for (std::uint32_t y = 0; y < kSmallEyeH; ++y) {
					const auto row = static_cast<std::size_t>(y) * kSmallEyeW;
					CHECK(actual[row] == preserved);
					CHECK(actual[row + 1] == completed);
					CHECK(actual[row + kSmallEyeW - 1] == completed);
					CHECK(actualDepth[row] == (depthOne ? 0x3F800000u : 0u));
					CHECK(actualDepth[row + kSmallEyeW - 1] == (depthOne ? 0x3F800000u : 0u));
				}
			}
			REQUIRE(scope.Close());
		}

		const auto after = Upscaler::Intermediates::Snapshot();
		CHECK(after.motionSanitizerDraws == before.motionSanitizerDraws + 4u);
		CHECK(after.motionCompletionDraws == before.motionCompletionDraws + 4u);
		CHECK(after.motionFallbackDraws == before.motionFallbackDraws);
		CHECK(Readback(device.Get(), immediate.Get(), wideMotion.Get()) == motionPixels);
		CHECK(Readback(device.Get(), immediate.Get(), wideDepth.Get()) == depthPixels);
		CHECK(device->GetDeviceRemovedReason() == S_OK);

		isolation.Shutdown();
		Upscaler::Intermediates::Release();
	}

	void CameraProjectionExtractsFsrScalarsAndRejectsWrongShapes()
	{
		constexpr float nearPlane = 5.0f;
		constexpr float farPlane = 80000.0f;
		constexpr float yScale = 1.0f;
		float standard[16]{};
		standard[0] = yScale;
		standard[5] = yScale;
		standard[10] = farPlane / (farPlane - nearPlane);
		standard[11] = 1.0f;
		standard[14] = -nearPlane * farPlane / (farPlane - nearPlane);

		Upscaler::CameraProjection::Data camera{};
		REQUIRE(Upscaler::CameraProjection::Build(standard, false, camera));
		CHECK(camera.valid);
		CHECK(std::abs(camera.nearPlane - nearPlane) < 0.001f);
		CHECK(std::abs(camera.farPlane - farPlane) < 128.0f);
		CHECK(std::abs(camera.verticalFovRadians - 1.57079632679f) < 0.0001f);

		float asymmetric[16]{};
		std::copy(std::begin(standard), std::end(standard), std::begin(asymmetric));
		asymmetric[9] = 0.2f;
		Upscaler::CameraProjection::Data asymmetricCamera{};
		REQUIRE(Upscaler::CameraProjection::Build(asymmetric, false, asymmetricCamera));
		const float expectedFov = std::atan(0.8f) + std::atan(1.2f);
		CHECK(std::abs(asymmetricCamera.verticalFovRadians - expectedFov) < 0.0001f);

		float inverted[16]{};
		inverted[0] = yScale;
		inverted[5] = yScale;
		inverted[10] = nearPlane / (nearPlane - farPlane);
		inverted[11] = 1.0f;
		inverted[14] = -farPlane * nearPlane / (nearPlane - farPlane);
		Upscaler::CameraProjection::Data invertedCamera{};
		REQUIRE(Upscaler::CameraProjection::Build(inverted, true, invertedCamera));
		CHECK(std::abs(invertedCamera.nearPlane - nearPlane) < 0.001f);
		CHECK(std::abs(invertedCamera.farPlane - farPlane) < 128.0f);

		float wrongShape[16]{};
		Upscaler::CameraProjection::Data rejected{};
		CHECK_FALSE(Upscaler::CameraProjection::Build(wrongShape, false, rejected));
		CHECK_FALSE(rejected.valid);

		float infiniteFar[16]{};
		std::copy(std::begin(standard), std::end(standard), std::begin(infiniteFar));
		infiniteFar[10] = 1.0f;
		infiniteFar[14] = -nearPlane;
		CHECK_FALSE(Upscaler::CameraProjection::Build(infiniteFar, false, rejected));
	}

	struct HostState
	{
		ComPtr<ID3D11Buffer> constantBuffer;
		ComPtr<ID3D11Buffer> vertexBuffer;
		ComPtr<ID3D11Buffer> indexBuffer;
		ComPtr<ID3D11Buffer> streamOutputBuffer;
		ComPtr<ID3D11ShaderResourceView> shaderResource;
		ComPtr<ID3D11UnorderedAccessView> unorderedAccess;
		ComPtr<ID3D11RenderTargetView> renderTarget;
		ComPtr<ID3D11SamplerState> sampler;
		ComPtr<ID3D11RasterizerState> rasterizer;
		ComPtr<ID3D11BlendState> blend;
		ComPtr<ID3D11DepthStencilState> depthStencil;
		ComPtr<ID3D11Predicate> predicate;
	};

	ComPtr<ID3D11Buffer> MakeBuffer(ID3D11Device* a_device, UINT a_bytes, UINT a_bind)
	{
		D3D11_BUFFER_DESC d{};
		d.ByteWidth = a_bytes;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = a_bind;
		ComPtr<ID3D11Buffer> out;
		return SUCCEEDED(a_device->CreateBuffer(&d, nullptr, out.GetAddressOf())) ? out : nullptr;
	}

	ComPtr<ID3D11Texture2D> MakeViewTexture(ID3D11Device* a_device, UINT a_bind)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = 16;
		d.Height = 16;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = a_bind;
		ComPtr<ID3D11Texture2D> out;
		return SUCCEEDED(a_device->CreateTexture2D(&d, nullptr, out.GetAddressOf())) ? out : nullptr;
	}

	HostState MakeHostState(ID3D11Device* a_device)
	{
		HostState s;
		s.constantBuffer = MakeBuffer(a_device, 64, D3D11_BIND_CONSTANT_BUFFER);
		s.vertexBuffer = MakeBuffer(a_device, 128, D3D11_BIND_VERTEX_BUFFER);
		s.indexBuffer = MakeBuffer(a_device, 128, D3D11_BIND_INDEX_BUFFER);
		s.streamOutputBuffer = MakeBuffer(a_device, 128, D3D11_BIND_STREAM_OUTPUT);

		const auto srvTexture = MakeViewTexture(a_device, D3D11_BIND_SHADER_RESOURCE);
		const auto uavTexture = MakeViewTexture(a_device, D3D11_BIND_UNORDERED_ACCESS);
		const auto rtvTexture = MakeViewTexture(a_device, D3D11_BIND_RENDER_TARGET);
		if (srvTexture) {
			static_cast<void>(a_device->CreateShaderResourceView(srvTexture.Get(), nullptr, s.shaderResource.GetAddressOf()));
		}
		if (uavTexture) {
			static_cast<void>(a_device->CreateUnorderedAccessView(uavTexture.Get(), nullptr, s.unorderedAccess.GetAddressOf()));
		}
		if (rtvTexture) {
			static_cast<void>(a_device->CreateRenderTargetView(rtvTexture.Get(), nullptr, s.renderTarget.GetAddressOf()));
		}

		D3D11_SAMPLER_DESC sampler{};
		sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		static_cast<void>(a_device->CreateSamplerState(&sampler, s.sampler.GetAddressOf()));

		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_WIREFRAME;
		raster.CullMode = D3D11_CULL_FRONT;
		raster.ScissorEnable = TRUE;
		raster.DepthClipEnable = TRUE;
		static_cast<void>(a_device->CreateRasterizerState(&raster, s.rasterizer.GetAddressOf()));

		D3D11_BLEND_DESC blend{};
		blend.AlphaToCoverageEnable = TRUE;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_GREEN;
		static_cast<void>(a_device->CreateBlendState(&blend, s.blend.GetAddressOf()));

		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthEnable = FALSE;
		depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
		depth.StencilEnable = TRUE;
		depth.StencilReadMask = 0x3c;
		depth.StencilWriteMask = 0xc3;
		depth.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		depth.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		depth.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		depth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		depth.BackFace = depth.FrontFace;
		static_cast<void>(a_device->CreateDepthStencilState(&depth, s.depthStencil.GetAddressOf()));

		D3D11_QUERY_DESC predicate{};
		predicate.Query = D3D11_QUERY_OCCLUSION_PREDICATE;
		static_cast<void>(a_device->CreatePredicate(&predicate, s.predicate.GetAddressOf()));
		return s;
	}

	void ApplyHostState(ID3D11DeviceContext* a_ctx, const HostState& a_s)
	{
		ID3D11Buffer* cb = a_s.constantBuffer.Get();
		a_ctx->VSSetConstantBuffers(4, 1, &cb);
		a_ctx->PSSetConstantBuffers(7, 1, &cb);
		a_ctx->CSSetConstantBuffers(3, 1, &cb);

		ID3D11ShaderResourceView* srv = a_s.shaderResource.Get();
		a_ctx->PSSetShaderResources(11, 1, &srv);
		ID3D11UnorderedAccessView* uav = a_s.unorderedAccess.Get();
		const UINT initialCount = 0;
		a_ctx->CSSetUnorderedAccessViews(2, 1, &uav, &initialCount);
		ID3D11SamplerState* sampler = a_s.sampler.Get();
		a_ctx->PSSetSamplers(5, 1, &sampler);

		ID3D11Buffer* vb = a_s.vertexBuffer.Get();
		const UINT stride = 24;
		const UINT vbOffset = 8;
		a_ctx->IASetVertexBuffers(3, 1, &vb, &stride, &vbOffset);
		a_ctx->IASetIndexBuffer(a_s.indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 6);
		a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

		ID3D11Buffer* so = a_s.streamOutputBuffer.Get();
		const UINT soOffset = 12;
		a_ctx->SOSetTargets(1, &so, &soOffset);

		ID3D11RenderTargetView* rtv = a_s.renderTarget.Get();
		a_ctx->OMSetRenderTargets(1, &rtv, nullptr);
		const float blendFactor[4] = { 0.125f, 0.25f, 0.5f, 1.0f };
		a_ctx->OMSetBlendState(a_s.blend.Get(), blendFactor, 0x13579bdfu);
		a_ctx->OMSetDepthStencilState(a_s.depthStencil.Get(), 0x5au);

		a_ctx->RSSetState(a_s.rasterizer.Get());
		const D3D11_VIEWPORT viewports[2] = {
			{ 3.0f, 5.0f, 101.0f, 103.0f, 0.125f, 0.875f },
			{ 107.0f, 109.0f, 211.0f, 223.0f, 0.25f, 0.75f }
		};
		const D3D11_RECT scissors[2] = { { 7, 11, 113, 127 }, { 131, 137, 227, 229 } };
		a_ctx->RSSetViewports(2, viewports);
		a_ctx->RSSetScissorRects(2, scissors);
		a_ctx->SetPredication(a_s.predicate.Get(), TRUE);
	}

	bool HostStateMatches(ID3D11DeviceContext* a_ctx, const HostState& a_s)
	{
		ComPtr<ID3D11Buffer> vsCB;
		ComPtr<ID3D11Buffer> psCB;
		ComPtr<ID3D11Buffer> csCB;
		a_ctx->VSGetConstantBuffers(4, 1, vsCB.GetAddressOf());
		a_ctx->PSGetConstantBuffers(7, 1, psCB.GetAddressOf());
		a_ctx->CSGetConstantBuffers(3, 1, csCB.GetAddressOf());

		ComPtr<ID3D11ShaderResourceView> srv;
		ComPtr<ID3D11UnorderedAccessView> uav;
		ComPtr<ID3D11SamplerState> sampler;
		a_ctx->PSGetShaderResources(11, 1, srv.GetAddressOf());
		a_ctx->CSGetUnorderedAccessViews(2, 1, uav.GetAddressOf());
		a_ctx->PSGetSamplers(5, 1, sampler.GetAddressOf());

		ComPtr<ID3D11Buffer> vb;
		UINT stride = 0;
		UINT vbOffset = 0;
		a_ctx->IAGetVertexBuffers(3, 1, vb.GetAddressOf(), &stride, &vbOffset);
		ComPtr<ID3D11Buffer> ib;
		DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
		UINT indexOffset = 0;
		a_ctx->IAGetIndexBuffer(ib.GetAddressOf(), &indexFormat, &indexOffset);
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		a_ctx->IAGetPrimitiveTopology(&topology);

		ComPtr<ID3D11Buffer> so;
		a_ctx->SOGetTargets(1, so.GetAddressOf());
		ComPtr<ID3D11RenderTargetView> rtv;
		a_ctx->OMGetRenderTargets(1, rtv.GetAddressOf(), nullptr);

		ComPtr<ID3D11BlendState> blend;
		float blendFactor[4] = {};
		UINT sampleMask = 0;
		a_ctx->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
		ComPtr<ID3D11DepthStencilState> depth;
		UINT stencilRef = 0;
		a_ctx->OMGetDepthStencilState(depth.GetAddressOf(), &stencilRef);
		ComPtr<ID3D11RasterizerState> raster;
		a_ctx->RSGetState(raster.GetAddressOf());

		D3D11_VIEWPORT viewports[2] = {};
		UINT viewportCount = 2;
		a_ctx->RSGetViewports(&viewportCount, viewports);
		D3D11_RECT scissors[2] = {};
		UINT scissorCount = 2;
		a_ctx->RSGetScissorRects(&scissorCount, scissors);
		ComPtr<ID3D11Predicate> predicate;
		BOOL predicateValue = FALSE;
		a_ctx->GetPredication(predicate.GetAddressOf(), &predicateValue);

		return vsCB.Get() == a_s.constantBuffer.Get() &&
		       psCB.Get() == a_s.constantBuffer.Get() &&
		       csCB.Get() == a_s.constantBuffer.Get() &&
		       srv.Get() == a_s.shaderResource.Get() &&
		       uav.Get() == a_s.unorderedAccess.Get() &&
		       sampler.Get() == a_s.sampler.Get() &&
		       vb.Get() == a_s.vertexBuffer.Get() && stride == 24 && vbOffset == 8 &&
		       ib.Get() == a_s.indexBuffer.Get() && indexFormat == DXGI_FORMAT_R16_UINT && indexOffset == 6 &&
		       topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST &&
		       so.Get() == a_s.streamOutputBuffer.Get() && rtv.Get() == a_s.renderTarget.Get() &&
		       blend.Get() == a_s.blend.Get() && blendFactor[0] == 0.125f && blendFactor[1] == 0.25f &&
		       blendFactor[2] == 0.5f && blendFactor[3] == 1.0f && sampleMask == 0x13579bdfu &&
		       depth.Get() == a_s.depthStencil.Get() && stencilRef == 0x5au &&
		       raster.Get() == a_s.rasterizer.Get() &&
		       viewportCount == 2 && viewports[0].TopLeftX == 3.0f && viewports[1].Width == 211.0f &&
		       scissorCount == 2 && scissors[0].left == 7 && scissors[1].right == 227 &&
		       predicate.Get() == a_s.predicate.Get() && predicateValue == TRUE;
	}

	void ContextStateSwapIsolatesHostAndPrivateState()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		const HostState host = MakeHostState(device.Get());
		REQUIRE(host.constantBuffer && host.vertexBuffer && host.indexBuffer && host.streamOutputBuffer);
		REQUIRE(host.shaderResource && host.unorderedAccess && host.renderTarget);
		REQUIRE(host.sampler && host.rasterizer && host.blend && host.depthStencil && host.predicate);
		ApplyHostState(immediate.Get(), host);
		REQUIRE(HostStateMatches(immediate.Get(), host));

		Upscaler::D3D11StateIsolation::Context isolation;
		REQUIRE(isolation.Initialize(device.Get(), immediate.Get()));
		CHECK(isolation.Initialize(device.Get(), immediate.Get()));
		CHECK(isolation.Ready());
		CHECK(isolation.ChosenFeatureLevel() == device->GetFeatureLevel());
		CHECK_FALSE(isolation.End());

		REQUIRE(isolation.Begin());
		CHECK(isolation.Active());
		CHECK_FALSE(isolation.Begin());
		CHECK_FALSE(HostStateMatches(immediate.Get(), host));

		immediate->ClearState();
		const auto privateCB = MakeBuffer(device.Get(), 64, D3D11_BIND_CONSTANT_BUFFER);
		REQUIRE(privateCB);
		ID3D11Buffer* privateCBRaw = privateCB.Get();
		immediate->VSSetConstantBuffers(4, 1, &privateCBRaw);
		immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
		const D3D11_VIEWPORT privateViewport{ 17.0f, 19.0f, 23.0f, 29.0f, 0.0f, 1.0f };
		immediate->RSSetViewports(1, &privateViewport);

		REQUIRE(isolation.End());
		CHECK_FALSE(isolation.Active());
		CHECK(HostStateMatches(immediate.Get(), host));

		REQUIRE(isolation.Begin());
		ComPtr<ID3D11Buffer> persistedCB;
		immediate->VSGetConstantBuffers(4, 1, persistedCB.GetAddressOf());
		D3D11_PRIMITIVE_TOPOLOGY persistedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		immediate->IAGetPrimitiveTopology(&persistedTopology);
		D3D11_VIEWPORT persistedViewport{};
		UINT persistedViewportCount = 1;
		immediate->RSGetViewports(&persistedViewportCount, &persistedViewport);
		CHECK(persistedCB.Get() == privateCB.Get());
		CHECK(persistedTopology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
		CHECK(persistedViewportCount == 1 && persistedViewport.TopLeftX == 17.0f && persistedViewport.Height == 29.0f);
		REQUIRE(isolation.End());
		CHECK(HostStateMatches(immediate.Get(), host));

		isolation.Shutdown();
		CHECK_FALSE(isolation.Ready());
	}

	constexpr std::uint32_t kAtomicMissingView = 1u << 0;
	constexpr std::uint32_t kAtomicStageDescription = 1u << 1;
	constexpr std::uint32_t kAtomicHistoryDescription = 1u << 2;
	constexpr std::uint32_t kAtomicSubmittedDescription = 1u << 3;
	constexpr std::uint32_t kAtomicDimensions = 1u << 4;
	constexpr std::uint32_t kAtomicFormats = 1u << 5;
	constexpr std::uint32_t kAtomicShape = 1u << 6;
	constexpr std::uint32_t kAtomicBindFlags = 1u << 7;
	constexpr std::uint32_t kAtomicStageOutputAlias = 1u << 8;
	constexpr std::uint32_t kAtomicOutputAlias = 1u << 9;

	inline constexpr char kAtomicPublishShaderSource[] = R"(
Texture2D<float4> stagedColor : register(t0);

struct PixelOutputs
{
    float4 history : SV_Target0;
    float4 submitted : SV_Target1;
};

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}

PixelOutputs PSMain(float4 position : SV_Position)
{
    float4 value = stagedColor.Load(int3(uint2(position.xy), 0));
    PixelOutputs output;
    output.history = value;
    output.submitted = value;
    return output;
}
)";

	struct AtomicPublishProgram
	{
		ComPtr<ID3D11VertexShader> vertexShader;
		ComPtr<ID3D11PixelShader> pixelShader;
		ComPtr<ID3D11BlendState> blendState;
		ComPtr<ID3D11RasterizerState> rasterizerState;
		ComPtr<ID3D11DepthStencilState> depthStencilState;
	};

	[[nodiscard]] bool MakeWarpDevice(
		ComPtr<ID3D11Device>& a_device,
		ComPtr<ID3D11DeviceContext>& a_immediate)
	{
		UINT flags = 0;
#ifdef UPSCALER_HARNESS_D3D_DEBUG
		flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
		return SUCCEEDED(D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_WARP,
			nullptr,
			flags,
			wanted,
			static_cast<UINT>(std::size(wanted)),
			D3D11_SDK_VERSION,
			a_device.GetAddressOf(),
			&obtained,
			a_immediate.GetAddressOf())) &&
		       a_device && a_immediate;
	}

	[[nodiscard]] bool MakeAtomicPublishProgram(ID3D11Device* a_device, AtomicPublishProgram& a_out)
	{
		using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
			const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
			ID3DBlob**, ID3DBlob**);
		static const HMODULE s_compilerModule = ::LoadLibraryW(L"d3dcompiler_47.dll");
		if (!s_compilerModule) {
			return false;
		}
		const auto compile = reinterpret_cast<D3DCompileFn>(
			::GetProcAddress(s_compilerModule, "D3DCompile"));
		if (!compile) {
			return false;
		}

		ComPtr<ID3DBlob> vertexCode;
		ComPtr<ID3DBlob> pixelCode;
		ComPtr<ID3DBlob> errors;
		if (FAILED(compile(
				kAtomicPublishShaderSource,
				sizeof(kAtomicPublishShaderSource) - 1,
				"Fo4VRAtomicPublishHarness", nullptr, nullptr, "VSMain", "vs_5_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
				vertexCode.GetAddressOf(), errors.GetAddressOf()))) {
			return false;
		}
		errors.Reset();
		if (FAILED(compile(
				kAtomicPublishShaderSource,
				sizeof(kAtomicPublishShaderSource) - 1,
				"Fo4VRAtomicPublishHarness", nullptr, nullptr, "PSMain", "ps_5_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
				pixelCode.GetAddressOf(), errors.GetAddressOf()))) {
			return false;
		}
		if (FAILED(a_device->CreateVertexShader(
				vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
				a_out.vertexShader.GetAddressOf())) ||
			FAILED(a_device->CreatePixelShader(
				pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr,
				a_out.pixelShader.GetAddressOf()))) {
			return false;
		}

		D3D11_BLEND_DESC blend{};
		blend.IndependentBlendEnable = TRUE;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		blend.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(a_device->CreateBlendState(&blend, a_out.blendState.GetAddressOf()))) {
			return false;
		}

		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = TRUE;
		if (FAILED(a_device->CreateRasterizerState(&raster, a_out.rasterizerState.GetAddressOf()))) {
			return false;
		}

		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthEnable = FALSE;
		depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		return SUCCEEDED(a_device->CreateDepthStencilState(&depth, a_out.depthStencilState.GetAddressOf()));
	}

	void PreparedImageSpaceDrawIgnoresStencilOnlyInsideExactOmBracket()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeWarpDevice(device, immediate));

		constexpr std::uint32_t width = 16;
		constexpr std::uint32_t height = 8;
		auto source = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_SHADER_RESOURCE);
		auto history = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_RENDER_TARGET);
		auto submitted = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_RENDER_TARGET);
		auto uavTexture = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_UNORDERED_ACCESS);
		REQUIRE(source && history && submitted && uavTexture);
		Fill(device.Get(), immediate.Get(), source.Get(), kSource);

		ComPtr<ID3D11ShaderResourceView> sourceSrv;
		ComPtr<ID3D11RenderTargetView> historyRtv;
		ComPtr<ID3D11RenderTargetView> submittedRtv;
		ComPtr<ID3D11UnorderedAccessView> preservedUav;
		REQUIRE(SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, sourceSrv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(history.Get(), nullptr, historyRtv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(submitted.Get(), nullptr, submittedRtv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateUnorderedAccessView(uavTexture.Get(), nullptr, preservedUav.GetAddressOf())));

		D3D11_TEXTURE2D_DESC depthDesc{};
		depthDesc.Width = width;
		depthDesc.Height = height;
		depthDesc.MipLevels = 1;
		depthDesc.ArraySize = 1;
		depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDesc.SampleDesc.Count = 1;
		depthDesc.Usage = D3D11_USAGE_DEFAULT;
		depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		ComPtr<ID3D11Texture2D> depthTexture;
		ComPtr<ID3D11DepthStencilView> depthView;
		REQUIRE(SUCCEEDED(device->CreateTexture2D(&depthDesc, nullptr, depthTexture.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateDepthStencilView(depthTexture.Get(), nullptr, depthView.GetAddressOf())));

		D3D11_DEPTH_STENCIL_DESC rejectStencil{};
		rejectStencil.DepthEnable = FALSE;
		rejectStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		rejectStencil.StencilEnable = TRUE;
		rejectStencil.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
		rejectStencil.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
		rejectStencil.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		rejectStencil.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		rejectStencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		rejectStencil.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
		rejectStencil.BackFace = rejectStencil.FrontFace;
		ComPtr<ID3D11DepthStencilState> rejectStencilState;
		REQUIRE(SUCCEEDED(device->CreateDepthStencilState(
			&rejectStencil, rejectStencilState.GetAddressOf())));

		AtomicPublishProgram program;
		REQUIRE(MakeAtomicPublishProgram(device.Get(), program));
		immediate->ClearState();
		ID3D11Buffer* nullVertexBuffer = nullptr;
		constexpr UINT zero = 0;
		immediate->IASetInputLayout(nullptr);
		immediate->IASetVertexBuffers(0, 1, &nullVertexBuffer, &zero, &zero);
		immediate->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		immediate->VSSetShader(program.vertexShader.Get(), nullptr, 0);
		immediate->PSSetShader(program.pixelShader.Get(), nullptr, 0);
		ID3D11ShaderResourceView* sourceView = sourceSrv.Get();
		immediate->PSSetShaderResources(0, 1, &sourceView);
		constexpr float blendFactor[4]{};
		immediate->OMSetBlendState(program.blendState.Get(), blendFactor, 0xFFFFFFFFu);
		immediate->OMSetDepthStencilState(rejectStencilState.Get(), 1);
		immediate->RSSetState(program.rasterizerState.Get());
		const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
		immediate->RSSetViewports(1, &viewport);
		immediate->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);

		ID3D11RenderTargetView* outputs[] = { historyRtv.Get(), submittedRtv.Get() };
		ID3D11UnorderedAccessView* outputUav = preservedUav.Get();
		auto bindPreparedOutputs = [&]() {
			immediate->OMSetRenderTargetsAndUnorderedAccessViews(
				2, outputs, depthView.Get(), 2, 1, &outputUav, nullptr);
		};
		auto allPixelsAre = [&](ID3D11Texture2D* a_texture, std::uint32_t a_value) {
			const auto pixels = Readback(device.Get(), immediate.Get(), a_texture);
			return std::all_of(pixels.begin(), pixels.end(), [=](std::uint32_t a_pixel) {
				return a_pixel == a_value;
			});
		};

		constexpr float black[4]{};
		immediate->ClearRenderTargetView(historyRtv.Get(), black);
		immediate->ClearRenderTargetView(submittedRtv.Get(), black);
		bindPreparedOutputs();
		immediate->Draw(3, 0);
		immediate->OMSetRenderTargets(0, nullptr, nullptr);
		CHECK(allPixelsAre(history.Get(), 0));
		CHECK(allPixelsAre(submitted.Get(), 0));

		immediate->ClearRenderTargetView(historyRtv.Get(), black);
		immediate->ClearRenderTargetView(submittedRtv.Get(), black);
		bindPreparedOutputs();
		Upscaler::D3D11StateIsolation::DepthStencilOverrideToken token{};
		REQUIRE(Upscaler::D3D11StateIsolation::BeginDepthStencilOverride(immediate.Get(), token));
		CHECK(token.active && !token.alreadyNull && token.renderTargetCount == 2);
		immediate->Draw(3, 0);
		REQUIRE(Upscaler::D3D11StateIsolation::EndDepthStencilOverride(immediate.Get(), token));
		CHECK(token.restored && !token.restoreMismatch && !token.stateUncertain);

		ID3D11RenderTargetView* observedOutputs[2]{};
		ID3D11DepthStencilView* observedDepth = nullptr;
		ID3D11UnorderedAccessView* observedUav = nullptr;
		immediate->OMGetRenderTargetsAndUnorderedAccessViews(
			2, observedOutputs, &observedDepth, 2, 1, &observedUav);
		CHECK(observedOutputs[0] == historyRtv.Get() && observedOutputs[1] == submittedRtv.Get());
		CHECK(observedDepth == depthView.Get());
		CHECK(observedUav == preservedUav.Get());
		if (observedOutputs[0]) {
			observedOutputs[0]->Release();
		}
		if (observedOutputs[1]) {
			observedOutputs[1]->Release();
		}
		if (observedDepth) {
			observedDepth->Release();
		}
		if (observedUav) {
			observedUav->Release();
		}
		ComPtr<ID3D11DepthStencilState> observedDepthState;
		UINT observedStencilRef = 0;
		immediate->OMGetDepthStencilState(observedDepthState.GetAddressOf(), &observedStencilRef);
		CHECK(observedDepthState.Get() == rejectStencilState.Get() && observedStencilRef == 1);
		immediate->ClearState();
		CHECK(allPixelsAre(history.Get(), kSource));
		CHECK(allPixelsAre(submitted.Get(), kSource));
		CHECK_FALSE(Upscaler::D3D11StateIsolation::HostStateUncertain());
	}

	[[nodiscard]] bool DescribeAtomicView(
		ID3D11View* a_view,
		ComPtr<ID3D11Resource>& a_resource,
		D3D11_TEXTURE2D_DESC& a_desc)
	{
		if (!a_view) {
			return false;
		}
		a_view->GetResource(a_resource.GetAddressOf());
		ComPtr<ID3D11Texture2D> texture;
		if (!a_resource || FAILED(a_resource.As(&texture)) || !texture) {
			return false;
		}
		texture->GetDesc(&a_desc);
		return true;
	}

	[[nodiscard]] std::uint32_t AtomicPublishValidationMask(
		ID3D11ShaderResourceView* a_stage,
		ID3D11RenderTargetView* a_history,
		ID3D11RenderTargetView* a_submitted)
	{
		std::uint32_t mask = 0;
		if (!a_stage || !a_history || !a_submitted) {
			mask |= kAtomicMissingView;
		}

		ComPtr<ID3D11Resource> stageResource;
		ComPtr<ID3D11Resource> historyResource;
		ComPtr<ID3D11Resource> submittedResource;
		D3D11_TEXTURE2D_DESC stageDesc{};
		D3D11_TEXTURE2D_DESC historyDesc{};
		D3D11_TEXTURE2D_DESC submittedDesc{};
		const bool stageDescribed = DescribeAtomicView(a_stage, stageResource, stageDesc);
		const bool historyDescribed = DescribeAtomicView(a_history, historyResource, historyDesc);
		const bool submittedDescribed = DescribeAtomicView(a_submitted, submittedResource, submittedDesc);
		if (!stageDescribed) {
			mask |= kAtomicStageDescription;
		}
		if (!historyDescribed) {
			mask |= kAtomicHistoryDescription;
		}
		if (!submittedDescribed) {
			mask |= kAtomicSubmittedDescription;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC stageViewDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC historyViewDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC submittedViewDesc{};
		if (a_stage) {
			a_stage->GetDesc(&stageViewDesc);
		}
		if (a_history) {
			a_history->GetDesc(&historyViewDesc);
		}
		if (a_submitted) {
			a_submitted->GetDesc(&submittedViewDesc);
		}

		if (!stageDescribed || !historyDescribed || !submittedDescribed ||
			stageDesc.Width == 0 || stageDesc.Height == 0 ||
			stageDesc.Width != historyDesc.Width || stageDesc.Height != historyDesc.Height ||
			stageDesc.Width != submittedDesc.Width || stageDesc.Height != submittedDesc.Height) {
			mask |= kAtomicDimensions;
		}
		if (!stageDescribed || !historyDescribed || !submittedDescribed ||
			stageDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
			historyDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
			submittedDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
			stageViewDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
			historyViewDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
			submittedViewDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
			mask |= kAtomicFormats;
		}
		if (!stageDescribed || !historyDescribed || !submittedDescribed ||
			stageDesc.MipLevels != 1 || historyDesc.MipLevels != 1 || submittedDesc.MipLevels != 1 ||
			stageDesc.ArraySize != 1 || historyDesc.ArraySize != 1 || submittedDesc.ArraySize != 1 ||
			stageDesc.SampleDesc.Count != 1 || historyDesc.SampleDesc.Count != 1 || submittedDesc.SampleDesc.Count != 1 ||
			stageDesc.SampleDesc.Quality != 0 || historyDesc.SampleDesc.Quality != 0 || submittedDesc.SampleDesc.Quality != 0 ||
			stageViewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
			stageViewDesc.Texture2D.MostDetailedMip != 0 || stageViewDesc.Texture2D.MipLevels != 1 ||
			historyViewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || historyViewDesc.Texture2D.MipSlice != 0 ||
			submittedViewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || submittedViewDesc.Texture2D.MipSlice != 0) {
			mask |= kAtomicShape;
		}
		if (!stageDescribed || !historyDescribed || !submittedDescribed ||
			(stageDesc.BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)) !=
				(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
			(historyDesc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 ||
			(submittedDesc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
			mask |= kAtomicBindFlags;
		}
		if (stageResource &&
			(stageResource.Get() == historyResource.Get() || stageResource.Get() == submittedResource.Get())) {
			mask |= kAtomicStageOutputAlias;
		}
		if (historyResource && historyResource.Get() == submittedResource.Get()) {
			mask |= kAtomicOutputAlias;
		}
		return mask;
	}

	[[nodiscard]] bool TryAtomicPublish(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		Upscaler::D3D11StateIsolation::Context& a_isolation,
		const AtomicPublishProgram& a_program,
		ID3D11ShaderResourceView* a_stage,
		ID3D11RenderTargetView* a_history,
		ID3D11RenderTargetView* a_submitted,
		std::uint32_t& a_validationMask)
	{
		a_validationMask = AtomicPublishValidationMask(a_stage, a_history, a_submitted);
		if (a_validationMask != 0) {
			return false;
		}

		D3D11_TEXTURE2D_DESC desc{};
		ComPtr<ID3D11Resource> stageResource;
		if (!DescribeAtomicView(a_stage, stageResource, desc)) {
			return false;
		}

		Upscaler::D3D11StateIsolation::Scope scope{ a_isolation };
		if (!scope) {
			return false;
		}
		a_context->ClearState();
		ID3D11Buffer* nullVertexBuffer = nullptr;
		constexpr UINT zero = 0;
		a_context->IASetInputLayout(nullptr);
		a_context->IASetVertexBuffers(0, 1, &nullVertexBuffer, &zero, &zero);
		a_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_context->VSSetShader(a_program.vertexShader.Get(), nullptr, 0);
		a_context->PSSetShader(a_program.pixelShader.Get(), nullptr, 0);
		ID3D11ShaderResourceView* source[] = { a_stage };
		a_context->PSSetShaderResources(0, 1, source);
		ID3D11RenderTargetView* outputs[] = { a_history, a_submitted };
		a_context->OMSetRenderTargets(2, outputs, nullptr);
		constexpr float blendFactor[4]{};
		a_context->OMSetBlendState(a_program.blendState.Get(), blendFactor, 0xFFFFFFFFu);
		a_context->OMSetDepthStencilState(a_program.depthStencilState.Get(), 0);
		a_context->RSSetState(a_program.rasterizerState.Get());
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(desc.Width);
		viewport.Height = static_cast<float>(desc.Height);
		viewport.MaxDepth = 1.0f;
		a_context->RSSetViewports(1, &viewport);
		a_context->Draw(3, 0);
		ID3D11ShaderResourceView* nullSource = nullptr;
		a_context->PSSetShaderResources(0, 1, &nullSource);
		a_context->OMSetRenderTargets(0, nullptr, nullptr);
		return scope.Close() && a_device->GetDeviceRemovedReason() == S_OK;
	}

	[[nodiscard]] bool AtomicPublishHostStateMatches(ID3D11DeviceContext* a_context, const HostState& a_host)
	{
		if (!HostStateMatches(a_context, a_host)) {
			return false;
		}

		ComPtr<ID3D11VertexShader> vertexShader;
		ComPtr<ID3D11PixelShader> pixelShader;
		ComPtr<ID3D11ShaderResourceView> source;
		ID3D11RenderTargetView* outputs[2]{};
		a_context->VSGetShader(vertexShader.GetAddressOf(), nullptr, nullptr);
		a_context->PSGetShader(pixelShader.GetAddressOf(), nullptr, nullptr);
		a_context->PSGetShaderResources(0, 1, source.GetAddressOf());
		a_context->OMGetRenderTargets(2, outputs, nullptr);
		ComPtr<ID3D11RenderTargetView> output0;
		ComPtr<ID3D11RenderTargetView> output1;
		output0.Attach(outputs[0]);
		output1.Attach(outputs[1]);
		return !vertexShader && !pixelShader && !source &&
		       output0.Get() == a_host.renderTarget.Get() && !output1;
	}

	void AtomicRgba8StagePublishesBothMRTsOrDeclinesBeforeDrawing()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeWarpDevice(device, immediate));

		constexpr std::uint32_t width = 256;
		constexpr std::uint32_t height = 1;
		auto stage = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
		auto history = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_RENDER_TARGET);
		auto submitted = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_RENDER_TARGET);
		REQUIRE(stage && history && submitted);

		ComPtr<ID3D11ShaderResourceView> stageSrv;
		ComPtr<ID3D11RenderTargetView> stageRtv;
		ComPtr<ID3D11RenderTargetView> historyRtv;
		ComPtr<ID3D11RenderTargetView> submittedRtv;
		REQUIRE(SUCCEEDED(device->CreateShaderResourceView(stage.Get(), nullptr, stageSrv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(stage.Get(), nullptr, stageRtv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(history.Get(), nullptr, historyRtv.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(submitted.Get(), nullptr, submittedRtv.GetAddressOf())));

		std::vector<std::uint32_t> exhaustive(width);
		for (std::uint32_t value = 0; value < width; ++value) {
			const auto red = value;
			const auto green = 255u - value;
			const auto blue = value ^ 0xA5u;
			const auto alpha = (value * 73u) & 0xFFu;
			exhaustive[value] = red | (green << 8) | (blue << 16) | (alpha << 24);
		}
		UploadRaw(device.Get(), immediate.Get(), stage.Get(), exhaustive);
		Fill(device.Get(), immediate.Get(), history.Get(), 0x11223344u);
		Fill(device.Get(), immediate.Get(), submitted.Get(), 0x55667788u);

		AtomicPublishProgram program;
		REQUIRE(MakeAtomicPublishProgram(device.Get(), program));
		Upscaler::D3D11StateIsolation::Context isolation;
		REQUIRE(isolation.Initialize(device.Get(), immediate.Get()));
		const HostState host = MakeHostState(device.Get());
		REQUIRE(host.constantBuffer && host.vertexBuffer && host.indexBuffer && host.streamOutputBuffer);
		REQUIRE(host.shaderResource && host.unorderedAccess && host.renderTarget);
		REQUIRE(host.sampler && host.rasterizer && host.blend && host.depthStencil && host.predicate);

		auto armHost = [&]() {
			immediate->ClearState();
			ApplyHostState(immediate.Get(), host);
			REQUIRE(AtomicPublishHostStateMatches(immediate.Get(), host));
		};
		armHost();
		std::uint32_t validationMask = ~0u;
		REQUIRE(TryAtomicPublish(
			device.Get(), immediate.Get(), isolation, program,
			stageSrv.Get(), historyRtv.Get(), submittedRtv.Get(), validationMask));
		CHECK(validationMask == 0);
		CHECK(AtomicPublishHostStateMatches(immediate.Get(), host));
		immediate->ClearState();
		CHECK(Readback(device.Get(), immediate.Get(), stage.Get()) == exhaustive);
		const auto historyPixels = Readback(device.Get(), immediate.Get(), history.Get());
		const auto submittedPixels = Readback(device.Get(), immediate.Get(), submitted.Get());
		CHECK(historyPixels == exhaustive);
		CHECK(submittedPixels == exhaustive);
		CHECK(historyPixels == submittedPixels);

		auto expectRejected = [&](ID3D11ShaderResourceView* a_stage,
								  ID3D11RenderTargetView* a_history,
								  ID3D11RenderTargetView* a_submitted,
								  std::uint32_t a_requiredMask,
								  ID3D11Texture2D* a_watch0,
								  std::uint32_t a_sentinel0,
								  ID3D11Texture2D* a_watch1,
								  std::uint32_t a_sentinel1) {
			immediate->ClearState();
			if (a_watch0) {
				Fill(device.Get(), immediate.Get(), a_watch0, a_sentinel0);
			}
			if (a_watch1 && a_watch1 != a_watch0) {
				Fill(device.Get(), immediate.Get(), a_watch1, a_sentinel1);
			}
			armHost();
			std::uint32_t rejectedMask = 0;
			CHECK_FALSE(TryAtomicPublish(
				device.Get(), immediate.Get(), isolation, program,
				a_stage, a_history, a_submitted, rejectedMask));
			CHECK((rejectedMask & a_requiredMask) != 0);
			CHECK(AtomicPublishHostStateMatches(immediate.Get(), host));
			immediate->ClearState();
			if (a_watch0) {
				const auto pixels = Readback(device.Get(), immediate.Get(), a_watch0);
				CHECK(std::all_of(pixels.begin(), pixels.end(), [&](std::uint32_t a_pixel) {
					return a_pixel == a_sentinel0;
				}));
			}
			if (a_watch1 && a_watch1 != a_watch0) {
				const auto pixels = Readback(device.Get(), immediate.Get(), a_watch1);
				CHECK(std::all_of(pixels.begin(), pixels.end(), [&](std::uint32_t a_pixel) {
					return a_pixel == a_sentinel1;
				}));
			}
		};

		auto wrongFormatStage = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UINT,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
		ComPtr<ID3D11ShaderResourceView> wrongFormatStageSrv;
		REQUIRE(wrongFormatStage);
		REQUIRE(SUCCEEDED(device->CreateShaderResourceView(wrongFormatStage.Get(), nullptr, wrongFormatStageSrv.GetAddressOf())));
		expectRejected(
			wrongFormatStageSrv.Get(), historyRtv.Get(), submittedRtv.Get(), kAtomicFormats,
			history.Get(), 0x10203040u, submitted.Get(), 0x50607080u);

		auto shortSubmitted = MakeTexture(device.Get(), width - 1, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_RENDER_TARGET);
		ComPtr<ID3D11RenderTargetView> shortSubmittedRtv;
		REQUIRE(shortSubmitted);
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(shortSubmitted.Get(), nullptr, shortSubmittedRtv.GetAddressOf())));
		expectRejected(
			stageSrv.Get(), historyRtv.Get(), shortSubmittedRtv.Get(), kAtomicDimensions,
			history.Get(), 0x21324354u, shortSubmitted.Get(), 0x65768798u);

		auto noRtvStage = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D11_BIND_SHADER_RESOURCE);
		ComPtr<ID3D11ShaderResourceView> noRtvStageSrv;
		REQUIRE(noRtvStage);
		REQUIRE(SUCCEEDED(device->CreateShaderResourceView(noRtvStage.Get(), nullptr, noRtvStageSrv.GetAddressOf())));
		expectRejected(
			noRtvStageSrv.Get(), historyRtv.Get(), submittedRtv.Get(), kAtomicBindFlags,
			history.Get(), 0x31425364u, submitted.Get(), 0x758697A8u);

		D3D11_TEXTURE2D_DESC mippedDesc{};
		mippedDesc.Width = width;
		mippedDesc.Height = height;
		mippedDesc.MipLevels = 2;
		mippedDesc.ArraySize = 1;
		mippedDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		mippedDesc.SampleDesc.Count = 1;
		mippedDesc.Usage = D3D11_USAGE_DEFAULT;
		mippedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		ComPtr<ID3D11Texture2D> mippedStage;
		ComPtr<ID3D11ShaderResourceView> mippedStageSrv;
		REQUIRE(SUCCEEDED(device->CreateTexture2D(&mippedDesc, nullptr, mippedStage.GetAddressOf())));
		REQUIRE(SUCCEEDED(device->CreateShaderResourceView(mippedStage.Get(), nullptr, mippedStageSrv.GetAddressOf())));
		expectRejected(
			mippedStageSrv.Get(), historyRtv.Get(), submittedRtv.Get(), kAtomicShape,
			history.Get(), 0x41526374u, submitted.Get(), 0x8596A7B8u);

		ComPtr<ID3D11RenderTargetView> historyAliasRtv;
		REQUIRE(SUCCEEDED(device->CreateRenderTargetView(history.Get(), nullptr, historyAliasRtv.GetAddressOf())));
		expectRejected(
			stageSrv.Get(), historyRtv.Get(), historyAliasRtv.Get(), kAtomicOutputAlias,
			history.Get(), 0x51627384u, nullptr, 0u);

		expectRejected(
			stageSrv.Get(), stageRtv.Get(), submittedRtv.Get(), kAtomicStageOutputAlias,
			stage.Get(), 0x61728394u, submitted.Get(), 0xA5B6C7D8u);
		expectRejected(
			stageSrv.Get(), historyRtv.Get(), stageRtv.Get(), kAtomicStageOutputAlias,
			history.Get(), 0x718293A4u, stage.Get(), 0xB5C6D7E8u);

		CHECK(device->GetDeviceRemovedReason() == S_OK);
		std::printf("    atomic publish arm ran on WARP with no vendor SDK.\n");
	}

	// The load-bearing case for our own plumbing: if a per-eye sub-rect write bleeds, the
	// stereo path is wrong no matter which reconstruction runtime sits underneath it.
	void RightEyeWriteLeavesLeftEyeUntouched()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		auto wide = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH);
		auto source = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH);
		REQUIRE(wide);
		REQUIRE(source);

		Fill(device.Get(), immediate.Get(), wide.Get(), kLeftSentinel);
		Fill(device.Get(), immediate.Get(), source.Get(), kSource);

		NullBackend backend;
		BringUp(backend, device.Get(), immediate.Get(), kSmallEyeW, kSmallEyeH);

		EyeView right{};
		right.index = 1;
		right.color = source.Get();
		right.output = wide.Get();
		right.colorRect = Rect{ 0, 0, kSmallEyeW, kSmallEyeH };
		right.outputRect = Upscaler::StereoLayout::RectFor(
			Upscaler::StereoLayout::Eye::kRight,
			Upscaler::StereoLayout::Order::kLeftRight,
			kSmallWideW,
			kSmallEyeH);

		REQUIRE(backend.Evaluate(immediate.Get(), right));

		const auto pixels = Readback(device.Get(), immediate.Get(), wide.Get());
		REQUIRE(pixels.size() == static_cast<std::size_t>(kSmallWideW) * kSmallEyeH);

		std::size_t leftWrong = 0;
		std::size_t rightWrong = 0;
		for (std::uint32_t y = 0; y < kSmallEyeH; ++y) {
			for (std::uint32_t x = 0; x < kSmallWideW; ++x) {
				const auto p = pixels[static_cast<std::size_t>(y) * kSmallWideW + x];
				if (x < kSmallEyeW) {
					leftWrong += (p != kLeftSentinel) ? 1u : 0u;
				} else {
					rightWrong += (p != kSource) ? 1u : 0u;
				}
			}
		}

		CHECK(leftWrong == 0);
		CHECK(rightWrong == 0);

		static_cast<void>(backend.DestroyViews());
		backend.Shutdown();
	}

	void BothEyesLandInTheirOwnHalf()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		auto wide = MakeTexture(device.Get(), kSmallWideW, kSmallEyeH);
		auto left = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH);
		auto rightSrc = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH);
		REQUIRE(wide);
		REQUIRE(left);
		REQUIRE(rightSrc);

		Fill(device.Get(), immediate.Get(), wide.Get(), 0u);
		Fill(device.Get(), immediate.Get(), left.Get(), kLeftSentinel);
		Fill(device.Get(), immediate.Get(), rightSrc.Get(), kRightSentinel);

		NullBackend backend;
		BringUp(backend, device.Get(), immediate.Get(), kSmallEyeW, kSmallEyeH);

		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			EyeView v{};
			v.index = eye;
			v.color = (eye == 0) ? left.Get() : rightSrc.Get();
			v.output = wide.Get();
			v.colorRect = Rect{ 0, 0, kSmallEyeW, kSmallEyeH };
			v.outputRect = Upscaler::StereoLayout::RectFor(
				eye == 0 ? Upscaler::StereoLayout::Eye::kLeft : Upscaler::StereoLayout::Eye::kRight,
				Upscaler::StereoLayout::Order::kLeftRight,
				kSmallWideW,
				kSmallEyeH);
			REQUIRE(backend.Evaluate(immediate.Get(), v));
		}

		const auto pixels = Readback(device.Get(), immediate.Get(), wide.Get());

		std::size_t wrong = 0;
		for (std::uint32_t y = 0; y < kSmallEyeH; ++y) {
			for (std::uint32_t x = 0; x < kSmallWideW; ++x) {
				const auto expected = (x < kSmallEyeW) ? kLeftSentinel : kRightSentinel;
				wrong += (pixels[static_cast<std::size_t>(y) * kSmallWideW + x] != expected) ? 1u : 0u;
			}
		}
		CHECK(wrong == 0);

		static_cast<void>(backend.DestroyViews());
		backend.Shutdown();
	}

	// The control arm imposes the same contract a vendor backend will, so the stage layer
	// is tested against it before any SDK exists.
	void BackendEnforcesCallOrder()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		auto tex = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH);
		auto out = MakeTexture(device.Get(), kSmallEyeW, kSmallEyeH);
		REQUIRE(tex);
		REQUIRE(out);

		NullBackend backend;

		EyeView v{};
		v.index = 0;
		v.color = tex.Get();
		v.output = out.Get();

		CHECK_FALSE(backend.Evaluate(immediate.Get(), v));  // before Initialize
		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), {}));
		CHECK_FALSE(backend.Evaluate(immediate.Get(), v));  // before CreateViews

		Config cfg{};
		cfg.renderWidth = kSmallEyeW;
		cfg.renderHeight = kSmallEyeH;
		cfg.outputWidth = kSmallEyeW;
		cfg.outputHeight = kSmallEyeH;
		REQUIRE(backend.CreateViews(cfg, 2));

		CHECK(backend.Evaluate(immediate.Get(), v));

		v.index = 7;  // out of range for two views
		CHECK_FALSE(backend.Evaluate(immediate.Get(), v));

		static_cast<void>(backend.DestroyViews());
		backend.Shutdown();
	}

	void ControlArmReportsRatioOne()
	{
		NullBackend backend;

		const auto [w, h] = backend.QueryRenderSize(kEyeWidth, kEyeHeight, QualityMode::kPerformance);
		CHECK(w == kEyeWidth);  // passthrough never reconstructs, whatever mode is asked for
		CHECK(h == kEyeHeight);

		// A caller does frameIndex % phaseCount, so zero here is a crash rather than a
		// wrong picture. Every backend must answer at least one, including an unavailable
		// one.
		CHECK(backend.JitterPhaseCount(kEyeWidth, kEyeWidth) >= 1u);
	}

	void BackendAutoExposureContractIsExplicit()
	{
		Config config{};
		CHECK(IsBackendAutoExposureContract(config, nullptr));

		config.autoExposure = false;
		CHECK_FALSE(IsBackendAutoExposureContract(config, nullptr));
		config.autoExposure = true;

		config.preExposure = 0.5f;
		CHECK_FALSE(IsBackendAutoExposureContract(config, nullptr));
		config.preExposure = 1.0f;
		config.exposureScale = 2.0f;
		CHECK_FALSE(IsBackendAutoExposureContract(config, nullptr));
		config.exposureScale = 1.0f;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		auto exposure = MakeTexture(device.Get(), 1, 1, DXGI_FORMAT_R11G11B10_FLOAT);
		REQUIRE(exposure);
		CHECK_FALSE(IsBackendAutoExposureContract(config, exposure.Get()));
	}

#ifdef UPSCALER_ENABLE_FFX

	[[nodiscard]] Config FfxNativeConfig()
	{
		Config config{};
		config.renderWidth = 640;
		config.renderHeight = 360;
		config.outputWidth = 640;
		config.outputHeight = 360;
		config.quality = QualityMode::kNativeAA;
		config.preset = "auto";
		config.hdr = true;
		return config;
	}

	void FfxInitialisesAndReportsVendorMath()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		if (g_usedWarp) {
			SKIP("the FFX compute path needs a feature-level-11 hardware adapter; this machine supplied only WARP.");
			return;
		}

		FidelityFxD3D11Backend backend;
		const auto asynchronousBefore = backend.Probe(nullptr);
		CHECK_FALSE(asynchronousBefore.available);
		CHECK_FALSE(asynchronousBefore.needsDeviceCreation);
		CHECK_FALSE(Text(asynchronousBefore.unavailableReason).empty());
		CHECK(Text(asynchronousBefore.runtimeVersion) == "FSR 3.1.5 DX11 source e65b253");
		const auto before = backend.Probe(device.Get());
		CHECK_FALSE(before.available);
		CHECK_FALSE(Text(before.unavailableReason).empty());

		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), {}));
		const auto after = backend.Probe(device.Get());
		CHECK(after.available);
		CHECK_FALSE(after.supportsSubrects);
		CHECK_FALSE(after.supportsExposure);
		CHECK_FALSE(after.supportsReactiveMask);
		CHECK(after.ownsSharpening);
		CHECK_FALSE(after.needsDeviceCreation);
		CHECK(Text(after.runtimeVersion) == "FSR 3.1.5 DX11 source e65b253");
		const auto asynchronousLive = backend.Probe(nullptr);
		CHECK_FALSE(asynchronousLive.available);
		CHECK_FALSE(asynchronousLive.needsDeviceCreation);
		CHECK_FALSE(Text(asynchronousLive.unavailableReason).empty());

		const auto native = backend.QueryRenderSize(640, 360, QualityMode::kNativeAA);
		const auto performance = backend.QueryRenderSize(640, 360, QualityMode::kPerformance);
		CHECK(native.first == 640 && native.second == 360);
		CHECK(performance.first > 0 && performance.first < native.first);
		CHECK(performance.second > 0 && performance.second < native.second);
		const auto phases = backend.JitterPhaseCount(native.first, native.first);
		CHECK(phases >= 1u);
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		backend.JitterOffset(0, phases, jitterX, jitterY);
		CHECK(std::isfinite(jitterX) && std::isfinite(jitterY));
		CHECK(jitterX != 0.0f || jitterY != 0.0f);
		CHECK(backend.RecommendedMipBias(native.first, native.first) == -1.0f);

		REQUIRE(backend.CreateViews(FfxNativeConfig(), 2));
		REQUIRE(backend.CreateViews(FfxNativeConfig(), 2));
		static_cast<void>(backend.DestroyViews());
		static_cast<void>(backend.DestroyViews());
		std::atomic<bool> stopReader{ false };
		std::atomic<bool> invalidDescriptor{ false };
		std::atomic<std::uint32_t> readerPasses{ 0 };
		std::thread reader([&]() {
			while (!stopReader.load(std::memory_order_acquire)) {
				const auto caps = backend.Probe(nullptr);
				if (caps.available || caps.needsDeviceCreation || caps.supportsSubrects ||
					!caps.ownsSharpening || Text(caps.runtimeVersion) != "FSR 3.1.5 DX11 source e65b253" ||
					Text(caps.unavailableReason).empty()) {
					invalidDescriptor.store(true, std::memory_order_relaxed);
				}
				readerPasses.fetch_add(1, std::memory_order_relaxed);
			}
		});
		while (readerPasses.load(std::memory_order_relaxed) < 1000) {
			std::this_thread::yield();
		}
		const bool shutdownSucceeded = backend.Shutdown();
		stopReader.store(true, std::memory_order_release);
		reader.join();
		CHECK(shutdownSucceeded);
		CHECK_FALSE(invalidDescriptor.load(std::memory_order_relaxed));
		CHECK(readerPasses.load(std::memory_order_relaxed) >= 1000);
		backend.Shutdown();
		const auto asynchronousAfter = backend.Probe(nullptr);
		CHECK_FALSE(asynchronousAfter.available);
		CHECK_FALSE(asynchronousAfter.needsDeviceCreation);
		CHECK(Text(asynchronousAfter.runtimeVersion) == "FSR 3.1.5 DX11 source e65b253");
	}

	void FfxEvaluatesTwoIndependentEyes()
	{
		constexpr std::uint32_t eyeWidth = 640;
		constexpr std::uint32_t eyeHeight = 360;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		if (g_usedWarp) {
			SKIP("the FFX compute path needs a feature-level-11 hardware adapter; this machine supplied only WARP.");
			return;
		}

		ComPtr<ID3D11Texture2D> colors[2];
		ComPtr<ID3D11Texture2D> depths[2];
		ComPtr<ID3D11Texture2D> motionVectors[2];
		ComPtr<ID3D11Texture2D> outputs[2];
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			colors[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R11G11B10_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			depths[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R32_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			motionVectors[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R16G16_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			outputs[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R11G11B10_FLOAT,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			REQUIRE(colors[eye] && depths[eye] && motionVectors[eye] && outputs[eye]);
			Fill(device.Get(), immediate.Get(), colors[eye].Get(), 0u);
			Fill(device.Get(), immediate.Get(), depths[eye].Get(), 0x3F800000u);  // float 1.0, standard-Z far
			Fill(device.Get(), immediate.Get(), motionVectors[eye].Get(), 0u);
			Fill(device.Get(), immediate.Get(), outputs[eye].Get(), 0u);
		}

		FidelityFxD3D11Backend backend;
		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), {}));
		REQUIRE(backend.CreateViews(FfxNativeConfig(), 2));

		EyeView views[2]{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			EyeView& view = views[eye];
			view.index = eye;
			view.color = colors[eye].Get();
			view.depth = depths[eye].Get();
			view.motionVectors = motionVectors[eye].Get();
			view.output = outputs[eye].Get();
			view.colorRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.depthRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.mvRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.outputRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.mvScaleX = static_cast<float>(eyeWidth);
			view.mvScaleY = static_cast<float>(eyeHeight);
			view.frameTimeDeltaMs = 1000.0f / 90.0f;
			view.cameraNear = 5.0f;
			view.cameraFar = 80000.0f;
			view.cameraFovAngleVertical = 1.5f;
			view.viewSpaceToMetersFactor = 0.3048f / 22.0f;
			view.resetHistory = true;
		}

		EyeView incomplete = views[0];
		incomplete.cameraFovAngleVertical = 0.0f;
		CHECK_FALSE(backend.Evaluate(immediate.Get(), incomplete));

		for (const EyeView& view : views) {
			REQUIRE(backend.Evaluate(immediate.Get(), view));
		}
		for (EyeView& view : views) {
			view.resetHistory = false;
			REQUIRE(backend.Evaluate(immediate.Get(), view));
		}

		const auto left = Readback(device.Get(), immediate.Get(), outputs[0].Get());
		const auto right = Readback(device.Get(), immediate.Get(), outputs[1].Get());
		CHECK(left.size() == static_cast<std::size_t>(eyeWidth) * eyeHeight);
		CHECK(right.size() == static_cast<std::size_t>(eyeWidth) * eyeHeight);
		CHECK(device->GetDeviceRemovedReason() == S_OK);

		static_cast<void>(backend.DestroyViews());
		backend.Shutdown();
	}

	void FfxPreservesStereoHistoryThroughBoundedSoak()
	{
		constexpr std::uint32_t eyeWidth = 640;
		constexpr std::uint32_t eyeHeight = 360;
		// Mirror the diagnostic gate's bounded lifetime without paying full headset resolution:
		// 16 private-output warm-up frames, then 512 engine-handback-equivalent frames.
		constexpr std::uint32_t warmupFrames = 16;
		constexpr std::uint32_t steadyFrames = 512;
		constexpr std::uint32_t totalFrames = warmupFrames + steadyFrames;
		constexpr std::uint32_t midWindowResetFrame = warmupFrames + steadyFrames / 2;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		if (g_usedWarp) {
			SKIP("the FFX compute path needs a feature-level-11 hardware adapter; this machine supplied only WARP.");
			return;
		}

		ComPtr<ID3D11Texture2D> colors[2];
		ComPtr<ID3D11Texture2D> depths[2];
		ComPtr<ID3D11Texture2D> motionVectors[2];
		ComPtr<ID3D11Texture2D> outputs[2];
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			colors[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R11G11B10_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			depths[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R32_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			motionVectors[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R16G16_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			outputs[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R11G11B10_FLOAT,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			REQUIRE(colors[eye] && depths[eye] && motionVectors[eye] && outputs[eye]);
			Fill(device.Get(), immediate.Get(), colors[eye].Get(), eye == 0 ? 0x000003C0u : 0x001E0000u);
			Fill(device.Get(), immediate.Get(), depths[eye].Get(), 0x3F800000u);  // float 1.0, standard-Z far
			Fill(device.Get(), immediate.Get(), motionVectors[eye].Get(), 0u);
			Fill(device.Get(), immediate.Get(), outputs[eye].Get(), 0u);
		}

		FidelityFxD3D11Backend backend;
		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), {}));
		REQUIRE(backend.CreateViews(FfxNativeConfig(), 2));

		EyeView views[2]{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			EyeView& view = views[eye];
			view.index = eye;
			view.color = colors[eye].Get();
			view.depth = depths[eye].Get();
			view.motionVectors = motionVectors[eye].Get();
			view.output = outputs[eye].Get();
			view.colorRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.depthRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.mvRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.outputRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.mvScaleX = static_cast<float>(eyeWidth);
			view.mvScaleY = static_cast<float>(eyeHeight);
			view.frameTimeDeltaMs = 1000.0f / 90.0f;
			view.cameraNear = 5.0f;
			view.cameraFar = 80000.0f;
			view.cameraFovAngleVertical = 1.5f;
			view.viewSpaceToMetersFactor = 0.3048f / 22.0f;
		}

		const std::uint32_t phaseCount = backend.JitterPhaseCount(eyeWidth, eyeWidth);
		REQUIRE(phaseCount >= 1u);
		std::uint32_t pairedResetFrames = 0;
		for (std::uint32_t frame = 0; frame < totalFrames; ++frame) {
			float jitterX = 0.0f;
			float jitterY = 0.0f;
			backend.JitterOffset(frame % phaseCount, phaseCount, jitterX, jitterY);
			const bool reset = frame == 0 || frame == midWindowResetFrame;
			pairedResetFrames += reset ? 1u : 0u;

			for (EyeView& view : views) {
				view.jitterX = jitterX;
				view.jitterY = jitterY;
				view.resetHistory = reset;
				REQUIRE(backend.Evaluate(immediate.Get(), view));
			}
		}
		CHECK(pairedResetFrames == 2u);

		const auto left = Readback(device.Get(), immediate.Get(), outputs[0].Get());
		const auto right = Readback(device.Get(), immediate.Get(), outputs[1].Get());
		CHECK(left.size() == static_cast<std::size_t>(eyeWidth) * eyeHeight);
		CHECK(right.size() == static_cast<std::size_t>(eyeWidth) * eyeHeight);
		CHECK(std::any_of(left.begin(), left.end(), [](std::uint32_t a_pixel) { return a_pixel != 0u; }));
		CHECK(std::any_of(right.begin(), right.end(), [](std::uint32_t a_pixel) { return a_pixel != 0u; }));
		CHECK(left != right);
		CHECK(device->GetDeviceRemovedReason() == S_OK);

		REQUIRE(backend.DestroyViews());
		REQUIRE(backend.Shutdown());
		Fill(device.Get(), immediate.Get(), outputs[0].Get(), 0u);
		Fill(device.Get(), immediate.Get(), outputs[1].Get(), 0u);
		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), {}));
		REQUIRE(backend.CreateViews(FfxNativeConfig(), 2));
		float restartJitterX = 0.0f;
		float restartJitterY = 0.0f;
		backend.JitterOffset(0, phaseCount, restartJitterX, restartJitterY);
		for (EyeView& view : views) {
			view.resetHistory = true;
			view.jitterX = restartJitterX;
			view.jitterY = restartJitterY;
			REQUIRE(backend.Evaluate(immediate.Get(), view));
		}
		const auto recreatedLeft = Readback(device.Get(), immediate.Get(), outputs[0].Get());
		const auto recreatedRight = Readback(device.Get(), immediate.Get(), outputs[1].Get());
		CHECK(std::any_of(recreatedLeft.begin(), recreatedLeft.end(), [](std::uint32_t a_pixel) { return a_pixel != 0u; }));
		CHECK(std::any_of(recreatedRight.begin(), recreatedRight.end(), [](std::uint32_t a_pixel) { return a_pixel != 0u; }));
		CHECK(recreatedLeft != recreatedRight);
		CHECK(device->GetDeviceRemovedReason() == S_OK);
		REQUIRE(backend.DestroyViews());
		REQUIRE(backend.Shutdown());
	}

#endif

#ifndef UPSCALER_NO_NGX

	// The vendor half. These need a staged SDK. The cases above prove our own arithmetic;
	// these prove the runtime's contract, and the sub-rect one is deliberately the same
	// shape as the NullBackend sub-rect case so the only variable is the backend.
	[[nodiscard]] std::filesystem::path NgxRuntimeDir()
	{
		return std::filesystem::path{ UPSCALER_NGX_RUNTIME_DIR };
	}

	[[nodiscard]] Config NgxNativeConfig()
	{
		Config config{};
		config.renderWidth = 640;
		config.renderHeight = 360;
		config.outputWidth = 640;
		config.outputHeight = 360;
		config.quality = QualityMode::kNativeAA;
		config.preset = "auto";
		config.hdr = true;
		return config;
	}

	void NgxInitialisesOutsideAGame()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		if (g_usedWarp) {
			SKIP("NGX requires an NVIDIA hardware adapter; this machine supplied only WARP.");
			return;
		}

		NgxD3D11Backend backend;
		const auto asynchronousBefore = backend.Probe(nullptr);
		CHECK_FALSE(asynchronousBefore.available);
		CHECK_FALSE(asynchronousBefore.needsDeviceCreation);
		CHECK(asynchronousBefore.supportsExposure);
		CHECK_FALSE(Text(asynchronousBefore.unavailableReason).empty());
		const auto before = backend.Probe(device.Get());
		CHECK_FALSE(before.available);
		CHECK_FALSE(Text(before.unavailableReason).empty());

		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), NgxRuntimeDir()));
		const auto after = backend.Probe(device.Get());
		CHECK(after.available);
		CHECK_FALSE(after.supportsSubrects);
		CHECK(after.supportsExposure);
		CHECK_FALSE(after.needsDeviceCreation);
		const auto asynchronousLive = backend.Probe(nullptr);
		CHECK_FALSE(asynchronousLive.available);
		CHECK_FALSE(asynchronousLive.needsDeviceCreation);
		CHECK(asynchronousLive.supportsExposure);
		CHECK_FALSE(Text(asynchronousLive.unavailableReason).empty());

		const auto native = backend.QueryRenderSize(640, 360, QualityMode::kNativeAA);
		CHECK(native.first == 640 && native.second == 360);
		CHECK(backend.JitterPhaseCount(640, 640) == 8);
		CHECK(backend.RecommendedMipBias(640, 640) == -1.0f);

		std::atomic<bool> stopReader{ false };
		std::atomic<bool> invalidDescriptor{ false };
		std::atomic<std::uint32_t> readerPasses{ 0 };
		std::thread reader([&]() {
			while (!stopReader.load(std::memory_order_acquire)) {
				const auto caps = backend.Probe(nullptr);
				if (caps.available || caps.needsDeviceCreation || caps.supportsSubrects ||
					!caps.supportsExposure || Text(caps.unavailableReason).empty()) {
					invalidDescriptor.store(true, std::memory_order_relaxed);
				}
				readerPasses.fetch_add(1, std::memory_order_relaxed);
			}
		});
		while (readerPasses.load(std::memory_order_relaxed) < 1000) {
			std::this_thread::yield();
		}
		const bool shutdownSucceeded = backend.Shutdown();
		stopReader.store(true, std::memory_order_release);
		reader.join();
		CHECK(shutdownSucceeded);
		CHECK_FALSE(invalidDescriptor.load(std::memory_order_relaxed));
		CHECK(readerPasses.load(std::memory_order_relaxed) >= 1000);
		backend.Shutdown();
		const auto asynchronousAfter = backend.Probe(nullptr);
		CHECK_FALSE(asynchronousAfter.available);
		CHECK_FALSE(asynchronousAfter.needsDeviceCreation);
		CHECK(asynchronousAfter.supportsExposure);
	}

	void NgxTwoHandlesOneDevice()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		if (g_usedWarp) {
			SKIP("NGX requires an NVIDIA hardware adapter; this machine supplied only WARP.");
			return;
		}

		NgxD3D11Backend backend;
		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), NgxRuntimeDir()));
		const Config config = NgxNativeConfig();
		REQUIRE(backend.CreateViews(config, 2));
		REQUIRE(backend.CreateViews(config, 2));

		const auto caps = backend.Probe(device.Get());
		CHECK(caps.available);
		CHECK_FALSE(Text(caps.runtimePath).empty());
		CHECK_FALSE(Text(caps.runtimeVersion).empty());

		static_cast<void>(backend.DestroyViews());
		static_cast<void>(backend.DestroyViews());
		backend.Shutdown();
	}

	void NgxPresetHintIsExplicitAndRecreatable()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		if (g_usedWarp) {
			SKIP("NGX requires an NVIDIA hardware adapter; this machine supplied only WARP.");
			return;
		}

		NgxD3D11Backend backend;
		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), NgxRuntimeDir()));
		auto config = NgxNativeConfig();
		config.preset = "K";
		REQUIRE(backend.CreateViews(config, 2));
		CHECK(backend.CreateViews(config, 2));

		config.preset = "auto";
		REQUIRE(backend.CreateViews(config, 2));
		config.preset = "not-a-preset";
		CHECK_FALSE(backend.CreateViews(config, 2));
		CHECK(device->GetDeviceRemovedReason() == S_OK);
		REQUIRE(backend.DestroyViews());
		REQUIRE(backend.Shutdown());
	}

	void NgxEvaluatesTwoEyesWithWholeStereoDepth()
	{
		constexpr std::uint32_t eyeWidth = 640;
		constexpr std::uint32_t eyeHeight = 360;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		if (g_usedWarp) {
			SKIP("NGX requires an NVIDIA hardware adapter; this machine supplied only WARP.");
			return;
		}

		ComPtr<ID3D11Texture2D> colors[2];
		ComPtr<ID3D11Texture2D> motionVectors[2];
		ComPtr<ID3D11Texture2D> outputs[2];
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			colors[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R11G11B10_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			motionVectors[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R16G16_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			outputs[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R11G11B10_FLOAT,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			REQUIRE(colors[eye] && motionVectors[eye] && outputs[eye]);
			Fill(device.Get(), immediate.Get(), colors[eye].Get(), 0u);
			Fill(device.Get(), immediate.Get(), motionVectors[eye].Get(), 0u);
			Fill(device.Get(), immediate.Get(), outputs[eye].Get(), 0u);
		}

		auto wholeDepth = MakeTexture(device.Get(), eyeWidth * 2u, eyeHeight, DXGI_FORMAT_R24G8_TYPELESS,
			D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
		REQUIRE(wholeDepth);
		// Packed D24S8: standard-Z far plane and stencil zero. This is the exact
		// whole-resource shape handed over by the FO4VR intermediates path.
		Fill(device.Get(), immediate.Get(), wholeDepth.Get(), 0x00FFFFFFu);

		NgxD3D11Backend backend;
		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), NgxRuntimeDir()));
		const Config config = NgxNativeConfig();
		REQUIRE(backend.CreateViews(config, 2));

		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			EyeView view{};
			view.index = eye;
			view.color = colors[eye].Get();
			view.depth = wholeDepth.Get();
			view.motionVectors = motionVectors[eye].Get();
			view.output = outputs[eye].Get();
			view.colorRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.depthRect = Rect{ eye * eyeWidth, 0, eyeWidth, eyeHeight };
			view.mvRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.outputRect = Rect{ 0, 0, eyeWidth, eyeHeight };
			view.mvScaleX = static_cast<float>(eyeWidth);
			view.mvScaleY = static_cast<float>(eyeHeight);
			view.frameTimeDeltaMs = 1000.0f / 90.0f;
			view.resetHistory = true;
			REQUIRE(backend.Evaluate(immediate.Get(), view));
		}

		// Mapping the result forces both dispatches to complete. Black is a valid
		// image here; the contract under test is successful consumption of the exact
		// D3D11 formats and the distinct left/right depth extents.
		const auto left = Readback(device.Get(), immediate.Get(), outputs[0].Get());
		const auto right = Readback(device.Get(), immediate.Get(), outputs[1].Get());
		CHECK(left.size() == static_cast<std::size_t>(eyeWidth) * eyeHeight);
		CHECK(right.size() == static_cast<std::size_t>(eyeWidth) * eyeHeight);
		CHECK(device->GetDeviceRemovedReason() == S_OK);

		static_cast<void>(backend.DestroyViews());
		backend.Shutdown();
	}

	void NgxSubrectWritesOnlyItsHalf()
	{
		SKIP("Deliberately deferred: production advertises supportsSubrects=false and uses per-eye intermediates, so this copy-removal optimization is not on the activation critical path.");
	}

	void NgxIsolationRestoresHostContextState()
	{
		constexpr std::uint32_t eyeWidth = 640;
		constexpr std::uint32_t eyeHeight = 360;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));
		if (g_usedWarp) {
			SKIP("NGX requires an NVIDIA hardware adapter; this machine supplied only WARP.");
			return;
		}

		ComPtr<ID3D11Texture2D> colors[2];
		ComPtr<ID3D11Texture2D> motionVectors[2];
		ComPtr<ID3D11Texture2D> outputs[2];
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			colors[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R11G11B10_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			motionVectors[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R16G16_FLOAT,
				D3D11_BIND_SHADER_RESOURCE);
			outputs[eye] = MakeTexture(device.Get(), eyeWidth, eyeHeight, DXGI_FORMAT_R11G11B10_FLOAT,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			REQUIRE(colors[eye] && motionVectors[eye] && outputs[eye]);
			Fill(device.Get(), immediate.Get(), colors[eye].Get(), 0u);
			Fill(device.Get(), immediate.Get(), motionVectors[eye].Get(), 0u);
			Fill(device.Get(), immediate.Get(), outputs[eye].Get(), 0u);
		}
		auto wholeDepth = MakeTexture(device.Get(), eyeWidth * 2u, eyeHeight, DXGI_FORMAT_R24G8_TYPELESS,
			D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
		REQUIRE(wholeDepth);
		Fill(device.Get(), immediate.Get(), wholeDepth.Get(), 0x00FFFFFFu);

		NgxD3D11Backend backend;
		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), NgxRuntimeDir()));
		REQUIRE(backend.CreateViews(NgxNativeConfig(), 2));

		const HostState host = MakeHostState(device.Get());
		REQUIRE(host.constantBuffer && host.vertexBuffer && host.indexBuffer && host.streamOutputBuffer);
		REQUIRE(host.shaderResource && host.unorderedAccess && host.renderTarget);
		REQUIRE(host.sampler && host.rasterizer && host.blend && host.depthStencil && host.predicate);
		ApplyHostState(immediate.Get(), host);
		REQUIRE(HostStateMatches(immediate.Get(), host));

		Upscaler::D3D11StateIsolation::Context isolation;
		REQUIRE(isolation.Initialize(device.Get(), immediate.Get()));
		{
			Upscaler::D3D11StateIsolation::Scope scope{ isolation };
			REQUIRE(static_cast<bool>(scope));
			for (std::uint32_t eye = 0; eye < 2; ++eye) {
				EyeView view{};
				view.index = eye;
				view.color = colors[eye].Get();
				view.depth = wholeDepth.Get();
				view.motionVectors = motionVectors[eye].Get();
				view.output = outputs[eye].Get();
				view.colorRect = Rect{ 0, 0, eyeWidth, eyeHeight };
				view.depthRect = Rect{ eye * eyeWidth, 0, eyeWidth, eyeHeight };
				view.mvRect = Rect{ 0, 0, eyeWidth, eyeHeight };
				view.outputRect = Rect{ 0, 0, eyeWidth, eyeHeight };
				view.mvScaleX = static_cast<float>(eyeWidth);
				view.mvScaleY = static_cast<float>(eyeHeight);
				view.frameTimeDeltaMs = 1000.0f / 90.0f;
				view.resetHistory = true;
				REQUIRE(backend.Evaluate(immediate.Get(), view));
			}
			REQUIRE(scope.Close());
		}
		CHECK(HostStateMatches(immediate.Get(), host));
		CHECK(device->GetDeviceRemovedReason() == S_OK);

		isolation.Shutdown();
		static_cast<void>(backend.DestroyViews());
		backend.Shutdown();
	}

	void NgxRejectsSubrectBaseWhenNotEnabled()
	{
		// The negative control. If this succeeds instead, the documentation is stale, the
		// per-eye intermediate copies become an optimisation rather than a requirement,
		// and the saving is two full-resolution copies per eye per frame.
		SKIP("Not written yet. Same evaluate with output sub-rects not enabled at creation; expect the documented failure.");
	}

#endif

	// A stand-in for a reconstruction backend, so the derived bias under test is the
	// vendor formula rather than whichever backend happens to be compiled in. Both the
	// passthrough control arm and the SDK-free NGX stub recommend no bias at all, which
	// would make this case pass while measuring nothing.
	class BiasReferenceBackend final : public IBackend
	{
	public:
		[[nodiscard]] std::string_view Name() const noexcept override { return std::string_view{ "bias-reference" }; }
		[[nodiscard]] Caps Probe(ID3D11Device*) const noexcept override { return {}; }

		[[nodiscard]] bool Initialize(ID3D11Device*, ID3D11DeviceContext*, const std::filesystem::path&) noexcept override { return true; }
		[[nodiscard]] bool CreateViews(const Config&, std::uint32_t) noexcept override { return true; }

		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> QueryRenderSize(
			std::uint32_t a_outputWidth,
			std::uint32_t a_outputHeight,
			QualityMode) const noexcept override
		{
			return { a_outputWidth, a_outputHeight };
		}

		[[nodiscard]] float RecommendedMipBias(
			std::uint32_t a_renderWidth,
			std::uint32_t a_outputWidth) const noexcept override
		{
			return static_cast<float>(std::log2(static_cast<double>(a_renderWidth) / a_outputWidth) - 1.0);
		}

		[[nodiscard]] std::uint32_t JitterPhaseCount(std::uint32_t, std::uint32_t) const noexcept override { return 1; }
		void JitterOffset(std::uint32_t, std::uint32_t, float& a_outX, float& a_outY) const noexcept override
		{
			a_outX = 0.0f;
			a_outY = 0.0f;
		}

		[[nodiscard]] bool Evaluate(ID3D11DeviceContext*, const EyeView&) noexcept override { return false; }
		[[nodiscard]] bool DestroyViews() noexcept override { return true; }
		bool Shutdown() noexcept override { return true; }
	};

	// AddRef then Release reports the count without changing it, which is the only way to
	// prove from outside that a displaced sampler was swapped rather than dropped.
	[[nodiscard]] ULONG RefCount(IUnknown* a_object)
	{
		a_object->AddRef();
		return a_object->Release();
	}

	void SamplerMipBiasTableRewrite()
	{
		namespace SamplerBias = Upscaler::SamplerBias;

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediate;
		REQUIRE(MakeDevice(device, immediate));

		// The engine's own descriptor pattern, filter index by filter index: five filter
		// classes, four address modes, sixteen mip indices, MinLOD the mip index, MaxLOD
		// unbounded on the three classes that get it and the mip index on the two that do
		// not, MaxAnisotropy raised only for the anisotropic and comparison classes, and
		// MipLODBias zero everywhere.
		constexpr D3D11_FILTER kFilters[SamplerBias::kFilterCount] = {
			D3D11_FILTER_MIN_MAG_MIP_POINT,                    // 0x00
			D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,             // 0x14
			D3D11_FILTER_MIN_MAG_MIP_LINEAR,                   // 0x15
			D3D11_FILTER_ANISOTROPIC,                          // 0x55
			D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,  // 0x94
		};
		static_assert(static_cast<std::uint32_t>(kFilters[3]) == 0x55, "the anisotropic class is filter index 3");
		static_assert(static_cast<std::uint32_t>(kFilters[4]) == 0x94, "the comparison class is filter index 4");

		ComPtr<ID3D11SamplerState> owned[SamplerBias::kEntryCount];
		D3D11_SAMPLER_DESC originalDescs[SamplerBias::kEntryCount] = {};
		ULONG originalRefs[SamplerBias::kEntryCount] = {};
		void* table[SamplerBias::kEntryCount] = {};

		for (std::size_t addressMode = 0; addressMode < SamplerBias::kAddressModeCount; ++addressMode) {
			for (std::size_t filter = 0; filter < SamplerBias::kFilterCount; ++filter) {
				for (std::size_t mip = 0; mip < SamplerBias::kMipCount; ++mip) {
					// The engine's own index arithmetic. Getting it wrong here would move
					// the runs under test and the case would still pass.
					const std::size_t index =
						(addressMode * SamplerBias::kFilterCount + filter) * SamplerBias::kMipCount + mip;
					const auto mode = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(addressMode + 1);
					D3D11_SAMPLER_DESC desc{};
					desc.Filter = kFilters[filter];
					desc.AddressU = mode;
					desc.AddressV = mode;
					desc.AddressW = mode;
					desc.MipLODBias = 0.0f;
					desc.MaxAnisotropy = (filter == 3 || filter == 4) ? 16u : 1u;
					desc.ComparisonFunc = filter == 4 ? D3D11_COMPARISON_LESS : D3D11_COMPARISON_NEVER;
					desc.MinLOD = static_cast<float>(mip);
					desc.MaxLOD = filter >= 2 ? (std::numeric_limits<float>::max)() : static_cast<float>(mip);
					REQUIRE(SUCCEEDED(device->CreateSamplerState(&desc, owned[index].GetAddressOf())));
					owned[index]->GetDesc(&originalDescs[index]);
					originalRefs[index] = RefCount(owned[index].Get());
					table[index] = owned[index].Get();
				}
			}
		}

		void* pristine[SamplerBias::kEntryCount] = {};
		std::memcpy(pristine, table, sizeof(table));

		REQUIRE(SamplerBias::Initialize(reinterpret_cast<std::uintptr_t>(&table[0]), device.Get()));

		BiasReferenceBackend backend;
		constexpr std::uint32_t kRenderWidth = 1240;
		constexpr std::uint32_t kOutputWidth = 1852;
		const float derived = SamplerBias::Derive(backend, kRenderWidth, kOutputWidth);
		const float vendor = backend.RecommendedMipBias(kRenderWidth, kOutputWidth);
		CHECK(std::abs(derived - (vendor + SamplerBias::kNativeBias + SamplerBias::kEpsilon)) < 0.000001f);
		CHECK(SamplerBias::InGuideBand(derived, kRenderWidth, kOutputWidth));
		CHECK_FALSE(SamplerBias::InGuideBand(-6.0f, kRenderWidth, kOutputWidth));

		REQUIRE(SamplerBias::Build(&backend, kRenderWidth, kOutputWidth, SamplerBias::kOverrideSentinel, false, false));
		auto state = SamplerBias::Snapshot();
		CHECK(state.built);
		CHECK(state.originalsCached);
		CHECK_FALSE(state.overrideActive);
		bool histogramExact = state.filterOther == 0;
		for (const auto count : state.filterHistogram) {
			histogramExact = histogramExact && count == 64;
		}
		CHECK(histogramExact);
		CHECK(state.entriesTotal == 320);
		CHECK(state.entriesBiased == 64);
		CHECK(state.samplersCreated == 64);
		CHECK(state.builtBias == derived);
		CHECK(state.bandWarnings == 0);
		CHECK(state.builds == 1);

		// An unchanged tuple builds nothing: no counter moves and no second set of
		// duplicates is created.
		REQUIRE(SamplerBias::Build(&backend, kRenderWidth, kOutputWidth, SamplerBias::kOverrideSentinel, false, false));
		state = SamplerBias::Snapshot();
		CHECK(state.builds == 1);
		CHECK(state.samplersCreated == 64);

		SamplerBias::Apply();
		CHECK(SamplerBias::Applied());
		int rewritten = 0;
		bool unmarkedIdentical = true;
		bool markedReplaced = true;
		bool biasApplied = true;
		bool otherFieldsUnchanged = true;
		for (std::size_t index = 0; index < SamplerBias::kEntryCount; ++index) {
			if (originalDescs[index].Filter != D3D11_FILTER_ANISOTROPIC) {
				unmarkedIdentical = unmarkedIdentical && table[index] == pristine[index];
				continue;
			}
			++rewritten;
			markedReplaced = markedReplaced && table[index] != pristine[index] && table[index] != nullptr;
			D3D11_SAMPLER_DESC applied{};
			static_cast<ID3D11SamplerState*>(table[index])->GetDesc(&applied);
			const auto& original = originalDescs[index];
			biasApplied = biasApplied && applied.MipLODBias == derived;
			otherFieldsUnchanged = otherFieldsUnchanged &&
			                       applied.Filter == original.Filter &&
			                       applied.AddressU == original.AddressU &&
			                       applied.AddressV == original.AddressV &&
			                       applied.AddressW == original.AddressW &&
			                       applied.MaxAnisotropy == original.MaxAnisotropy &&
			                       applied.ComparisonFunc == original.ComparisonFunc &&
			                       applied.MinLOD == original.MinLOD &&
			                       applied.MaxLOD == original.MaxLOD;
		}
		CHECK(rewritten == 64);
		CHECK(unmarkedIdentical);
		CHECK(markedReplaced);
		CHECK(biasApplied);
		CHECK(otherFieldsUnchanged);

		SamplerBias::Restore();
		CHECK_FALSE(SamplerBias::Applied());
		CHECK(std::memcmp(table, pristine, sizeof(table)) == 0);

		// The invariant that keeps an engine-side by-value copy from dangling: nothing
		// this module displaces is ever released, so every original's reference count
		// comes back exactly as it went in.
		bool referencesStable = true;
		for (std::size_t index = 0; index < SamplerBias::kEntryCount; ++index) {
			referencesStable = referencesStable && RefCount(owned[index].Get()) == originalRefs[index];
		}
		CHECK(referencesStable);

		// A table something else has rewritten declines instead of writing blind, and
		// declines whole: not one of the other slots moves.
		std::size_t mutated = SamplerBias::kEntryCount;
		for (std::size_t index = 0; index < SamplerBias::kEntryCount; ++index) {
			if (originalDescs[index].Filter == D3D11_FILTER_ANISOTROPIC) {
				mutated = index;
				break;
			}
		}
		REQUIRE(mutated < SamplerBias::kEntryCount);
		const auto declinesBefore = SamplerBias::Snapshot().applyDeclines;
		table[mutated] = owned[0].Get();
		SamplerBias::Apply();
		CHECK_FALSE(SamplerBias::Applied());
		CHECK(SamplerBias::Snapshot().applyDeclines == declinesBefore + 1);
		bool othersUntouched = true;
		for (std::size_t index = 0; index < SamplerBias::kEntryCount; ++index) {
			othersUntouched = othersUntouched && (index == mutated || table[index] == pristine[index]);
		}
		CHECK(othersUntouched);
		table[mutated] = pristine[mutated];

		// An override outside the vendor's own band is honoured and counted, never
		// clamped, and the superseded duplicates are retained rather than released.
		const auto warningsBefore = SamplerBias::Snapshot().bandWarnings;
		REQUIRE(SamplerBias::Build(&backend, kRenderWidth, kOutputWidth, -6.0f, false, false));
		state = SamplerBias::Snapshot();
		CHECK(state.bandWarnings == warningsBefore + 1);
		CHECK(state.overrideActive);
		CHECK(state.builtBias == -6.0f);
		CHECK(state.samplersCreated == 128);
		CHECK(state.retainedSamplers == 64);

		// The trilinear opt-in doubles the marked set and nothing else.
		REQUIRE(SamplerBias::Build(&backend, kRenderWidth, kOutputWidth, SamplerBias::kOverrideSentinel, true, false));
		state = SamplerBias::Snapshot();
		CHECK(state.includeTrilinear);
		CHECK(state.entriesBiased == 128);
		CHECK(state.retainedSamplers == 128);

		// The MinLOD opt-in is what actually removes the engine's own clamp, and it moves
		// only MinLOD.
		REQUIRE(SamplerBias::Build(&backend, kRenderWidth, kOutputWidth, -1.0f, false, true));
		SamplerBias::Apply();
		bool minLodLowered = true;
		for (std::size_t index = 0; index < SamplerBias::kEntryCount; ++index) {
			if (originalDescs[index].Filter != D3D11_FILTER_ANISOTROPIC) {
				continue;
			}
			D3D11_SAMPLER_DESC applied{};
			static_cast<ID3D11SamplerState*>(table[index])->GetDesc(&applied);
			minLodLowered = minLodLowered &&
			                applied.MinLOD == (std::max)(0.0f, originalDescs[index].MinLOD - 1.0f) &&
			                applied.MaxLOD == originalDescs[index].MaxLOD;
		}
		CHECK(minLodLowered);

		SamplerBias::Restore();
		CHECK(std::memcmp(table, pristine, sizeof(table)) == 0);
		bool referencesStillStable = true;
		for (std::size_t index = 0; index < SamplerBias::kEntryCount; ++index) {
			referencesStillStable = referencesStillStable && RefCount(owned[index].Get()) == originalRefs[index];
		}
		CHECK(referencesStillStable);
		CHECK(device->GetDeviceRemovedReason() == S_OK);

		// The module keeps every duplicate for the process, which is the invariant, and it
		// still points at this stack table. Nothing after this case applies anything, and
		// re-binding a different base forgets the build without releasing a sampler.
	}

	void VendorRenderSizeSnapAgreesWithVendorAndConfig()
	{
		// The engine's viewport extent is a raw float, not a rounded integer:
		// TS_BSGraphicsState_BuildViewportFromCamDataRect @ 0x141d8d480 multiplies the
		// full per-eye extent by the live ratio ([0x1438ad674] / [0x1438ad678]) and stores
		// the result with MOVSS, with no CVTTSS2SI and no ROUNDSS anywhere in the
		// function. So there is no engine truncation to land on exactly. What has to
		// agree is the pair of integers that name the same rectangle to the vendor -
		// BuildNgxConfig's floor(double(extent) * ratio) and the runtime's
		// round(extent / scalingRatio) - plus the same floor taken in float, which is what
		// the engine's own jitter compensation divides by at 0x141daa588.
		//
		// The float product is then checked for clearance rather than for equality: it has
		// to stay away from an integer, or the raw-float viewport width, and the
		// double-wide eye seam at Width/2 with it, sits exactly on a pixel centre.
		constexpr float kRatios[] = { 1.5f, 1.724f, 2.0f, 3.0f };
		constexpr std::uint32_t kExtents[] = { 1852, 3760, 2560, 1440, 2056, 3996 };

		for (const float ratio : kRatios) {
			for (const std::uint32_t extent : kExtents) {
				const auto snapped = SnapRenderScale(extent, ratio);
				CHECK(snapped.valid);
				if (!snapped.valid) {
					continue;
				}
				CHECK(snapped.ratio > 0.0f && snapped.ratio <= 1.0f);
				// Round to nearest is what NGX_DLSS_GET_OPTIMAL_SETTINGS reports.
				const auto expected = static_cast<std::uint32_t>(
					static_cast<float>(extent) / ratio + 0.5f);
				CHECK(snapped.renderExtent == expected);
				const float product = static_cast<float>(extent) * snapped.ratio;
				const auto floatFloor = static_cast<std::uint32_t>(std::floor(product));
				CHECK(floatFloor == snapped.renderExtent);
				// And in the double arithmetic the NGX configuration path actually uses.
				// A disagreement here means the vendor is handed a rectangle that is not
				// the one the extents were chosen for.
				const auto configFloor = static_cast<std::uint32_t>(
					std::floor(static_cast<double>(extent) * static_cast<double>(snapped.ratio)));
				CHECK(configFloor == snapped.renderExtent);
				// Clearance from an integer is what any aim in (0, 1) buys, so it is a
				// weak guard on its own. The +0.25 aim specifically keeps the raw float
				// viewport extent, and the double-wide eye seam at Width/2, off a pixel
				// centre: at +0.5 the product is exactly x.5 for 20 of these 24 pairs.
				// Assert both, so reverting the aim to +0.5 fails here.
				const float fraction = product - std::floor(product);
				const float distanceToInteger = (std::min)(fraction, 1.0f - fraction);
				CHECK(distanceToInteger >= 0.2f);
				CHECK(std::abs(fraction - 0.5f) >= 0.2f);
			}
		}

		// The two eyes this project has measured. Quality on the shipping 1852x2056 eye
		// is the pair the runtime reported while the unsnapped 0.6667f ratio floored to
		// 1234x1370 in BuildNgxConfig.
		const auto shippingX = SnapRenderScale(kEyeWidth, VendorScalingRatio(QualityMode::kQuality));
		const auto shippingY = SnapRenderScale(kEyeHeight, VendorScalingRatio(QualityMode::kQuality));
		CHECK(shippingX.renderExtent == 1235);
		CHECK(shippingY.renderExtent == 1371);
		const auto largeX = SnapRenderScale(3760, VendorScalingRatio(QualityMode::kQuality));
		const auto largeY = SnapRenderScale(3996, VendorScalingRatio(QualityMode::kQuality));
		CHECK(largeX.renderExtent == 2507);
		CHECK(largeY.renderExtent == 2664);

		// Native is ratio one exactly. The midpoint form would give a ratio above one,
		// which the dynamic-resolution controller refuses outright.
		const auto native = SnapRenderScale(kEyeWidth, VendorScalingRatio(QualityMode::kNativeAA));
		CHECK(native.valid);
		CHECK(native.renderExtent == kEyeWidth);
		CHECK(native.ratio == 1.0f);

		// Fail closed on the shapes a caller can hand it before the renderer is up.
		CHECK_FALSE(SnapRenderScale(0, 1.5f).valid);
		CHECK_FALSE(SnapRenderScale(kEyeWidth, 0.0f).valid);
		CHECK_FALSE(SnapRenderScale(kEyeWidth, 0.5f).valid);
		CHECK_FALSE(SnapRenderScale(kEyeWidth, 64.0f).valid);
	}

	struct Case
	{
		const char* name;
		void (*fn)();
	};

	constexpr Case kCases[] = {
		{ "a hardware D3D11 device comes up with no window and no game", &DeviceComesUp },
		{ "the measured double-wide target allocates at full size", &DoubleWideAllocatesAtFullSize },
		{ "the shipping stereo geometry is exact and invalid shapes fail closed", &ShippingStereoGeometryIsExactAndFailClosed },
		{ "backend-owned jitter sequence and projection round trip", &BackendOwnedJitterSequenceAndProjectionRoundTrip },
		{ "sampler mip bias table rewrite", &SamplerMipBiasTableRewrite },
		{ "shipping intermediates extract, evaluate and reassemble both eye orders", &ShippingIntermediatesExtractEvaluateAndReassembleBothOrders },
		{ "private motion sanitization preserves bounded finite values and rejects invalid inputs in both eye orders", &MotionSanitizerPreservesBoundedFiniteAndRejectsInvalidBothOrders },
		{ "camera motion completion uses per-eye depth and preserves finite inputs in both eye orders", &MotionCompletionUsesDepthAndPreservesFiniteInputsBothOrders },
		{ "camera projection exposes finite near far and asymmetric vertical FOV scalars", &CameraProjectionExtractsFsrScalarsAndRejectsWrongShapes },
		{ "D3D11.1 context-state swaps isolate host and persistent private state", &ContextStateSwapIsolatesHostAndPrivateState },
		{ "prepared ImageSpace draws ignore stencil only inside an exact OM bracket", &PreparedImageSpaceDrawIgnoresStencilOnlyInsideExactOmBracket },
		{ "one private RGBA8 stage publishes byte-exactly to both MRTs or fails closed", &AtomicRgba8StagePublishesBothMRTsOrDeclinesBeforeDrawing },
		{ "the exact TAA passthrough shader converts color and writes both MRTs", &TaaPassthroughConvertsAndWritesBothMRTs },
		{ "writing the right eye leaves the left eye byte-for-byte untouched", &RightEyeWriteLeavesLeftEyeUntouched },
		{ "both eyes land in their own half and neither overwrites the other", &BothEyesLandInTheirOwnHalf },
		{ "the backend enforces its call order", &BackendEnforcesCallOrder },
		{ "the control arm reports ratio 1.0 and no divide-by-zero", &ControlArmReportsRatioOne },
		{ "the vendor render-size snap agrees with the vendor query and the NGX config floor", &VendorRenderSizeSnapAgreesWithVendorAndConfig },
		{ "backend auto-exposure is explicit and rejects texture or scalar drift", &BackendAutoExposureContractIsExplicit },
#ifdef UPSCALER_ENABLE_FFX
		{ "the pinned FFX DX11 source initialises and reports vendor reconstruction math", &FfxInitialisesAndReportsVendorMath },
		{ "two independent FSR 3.1 histories dispatch both eyes for two frames", &FfxEvaluatesTwoIndependentEyes },
		{ "two independent FSR 3.1 histories survive the bounded continuous soak and recreate",
			&FfxPreservesStereoHistoryThroughBoundedSoak },
#endif
#ifndef UPSCALER_NO_NGX
		{ "the runtime initialises outside a game process", &NgxInitialisesOutsideAGame },
		{ "two feature handles share one device with independent histories", &NgxTwoHandlesOneDevice },
		{ "NGX preset hints are explicit, idempotent and safely recreatable", &NgxPresetHintIsExplicitAndRecreatable },
		{ "two NGX views evaluate with the whole stereo D24S8 depth resource", &NgxEvaluatesTwoEyesWithWholeStereoDepth },
		{ "an NGX evaluate into the right half writes only that half", &NgxSubrectWritesOnlyItsHalf },
		{ "shipping state isolation restores the host after two NGX evaluates", &NgxIsolationRestoresHostContextState },
		{ "a non-zero sub-rect base without sub-rects enabled fails as documented", &NgxRejectsSubrectBaseWhenNotEnabled },
#endif
	};
}

int main()
{
#ifdef UPSCALER_ENABLE_FFX
	std::printf("sub-rect harness: pinned FFX DX11 source present, FSR 3.1 hardware cases included.\n");
#else
	std::printf("sub-rect harness: FFX source absent, FSR hardware cases are not compiled.\n");
#endif
#ifdef UPSCALER_NO_NGX
	std::printf("sub-rect harness: no NGX SDK staged, so the vendor cases are absent.\n");
	std::printf("the stereo and contract cases below need no SDK and are the point of running this now.\n\n");
#else
	std::printf("sub-rect harness: NGX SDK present, vendor cases included.\n\n");
#endif

	int failedCases = 0;
	for (const auto& c : kCases) {
		std::printf("[ run  ] %s\n", c.name);
		const int before = harness::g_failures;
		const int skipsBefore = harness::g_skips;
		try {
			c.fn();
		} catch (const harness::Failure&) {
			// Already counted and printed by the REQUIRE that threw.
		} catch (const std::exception& e) {
			++harness::g_failures;
			std::printf("    FAIL  unexpected exception: %s\n", e.what());
		}
		const bool ok = harness::g_failures == before;
		failedCases += ok ? 0 : 1;
		const bool skipped = ok && harness::g_skips != skipsBefore;
		std::printf("[ %s ] %s\n", skipped ? "SKIP " : (ok ? " ok  " : "FAILED"), c.name);
	}

	std::printf("\n%d case(s), %d failed, %d skipped; %d check(s), %d failed.\n",
		static_cast<int>(std::size(kCases)),
		failedCases,
		harness::g_skips,
		harness::g_checks,
		harness::g_failures);
	std::printf("ran on %s.\n", g_usedWarp ? "WARP, the software rasteriser - copy semantics only, this says nothing about a real adapter" : "a hardware adapter");

	return failedCases == 0 ? 0 : 1;
}
