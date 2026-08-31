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
#include "Upscaler/Backend/IBackend.h"
#include "Upscaler/Backend/NgxD3D11.h"
#include "Upscaler/Backend/NullBackend.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/Intermediates.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/TAAPassthroughShader.h"

#include <Windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
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

		CHECK(RectFor(Eye::kLeft, Order::kLeftRight, kWideWidth - 1, kEyeHeight).w == 0);
		CHECK(RectFor(Eye::kLeft, Order::kLeftRight, 0, kEyeHeight).w == 0);
		CHECK(RectFor(Eye::kLeft, Order::kLeftRight, kWideWidth, 0).w == 0);
		CHECK(RectFor(static_cast<Eye>(2), Order::kLeftRight, kWideWidth, kEyeHeight).w == 0);
		CHECK(RectFor(Eye::kLeft, static_cast<Order>(2), kWideWidth, kEyeHeight).w == 0);
	}

	void NativeJitterConversionUsesPerEyeExtentAndBackendSigns()
	{
		constexpr float kTolerance = 0.000001f;
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
			const float jitterX = Upscaler::Jitter::ProjectionToCameraPixelsX(projectionX, kEyeWidth);
			const float jitterY = Upscaler::Jitter::ProjectionToCameraPixelsY(projectionY, kEyeHeight);
			CHECK(std::abs(jitterX - (0.5f - halton2[phase])) < kTolerance);
			CHECK(std::abs(jitterY - (halton3[phase] - 0.5f)) < kTolerance);
			CHECK(std::abs(jitterX) <= 0.5f && std::abs(jitterY) <= 0.5f);
		}

		// Supplying the double-wide width would double X and breach the canonical range
		// on phase 7. This negative control makes the divisor proof executable.
		const float phase7ProjectionX = (2.0f * halton2[7] - 1.0f) / static_cast<float>(kEyeWidth);
		CHECK(std::abs(Upscaler::Jitter::ProjectionToCameraPixelsX(phase7ProjectionX, kWideWidth)) > 0.5f);
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
				REQUIRE(staged.color && staged.motionVectors && staged.output);
				REQUIRE(depth.resource == wideDepth.Get());
				CHECK(depth.rect.x == rect.x && depth.rect.y == rect.y &&
					depth.rect.w == rect.w && depth.rect.h == rect.h);
				REQUIRE(ExtractColor(immediate.Get(), wideSource.Get(), rect, staged.color));
				REQUIRE(ExtractMotion(immediate.Get(), wideMotion.Get(), rect, staged.motionVectors));

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
		CHECK_FALSE(ExtractMotion(immediate.Get(), wideSource.Get(), firstHalf, left.motionVectors));
		CHECK_FALSE(ExtractMotion(immediate.Get(), wideMotion.Get(), Rect{ 1, 0, kSmallEyeW * 2, kSmallEyeH }, left.motionVectors));
		CHECK_FALSE(ReassembleOutput(immediate.Get(), left.output, wideOutput.Get(), Rect{ kSmallWideW, 0, kSmallEyeW, kSmallEyeH }));

		backend.DestroyViews();
		backend.Shutdown();
		Upscaler::Intermediates::Release();
		CHECK_FALSE(Upscaler::Intermediates::Snapshot().ready);
		CHECK(Get(Eye::kLeft).color == nullptr && Get(Eye::kLeft).motionVectors == nullptr &&
			Get(Eye::kRight).output == nullptr);
	}

	void MotionSanitizerPreservesFiniteAndZerosNonFiniteBothOrders()
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
		};
		auto finitePair = [](std::uint32_t a_raw) {
			const auto x = static_cast<std::uint16_t>(a_raw & 0xFFFFu);
			const auto y = static_cast<std::uint16_t>(a_raw >> 16u);
			return (x & 0x7C00u) != 0x7C00u && (y & 0x7C00u) != 0x7C00u;
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
				REQUIRE(ExtractMotion(immediate.Get(), wideMotion.Get(), rect, staged.motionVectors));

				const auto actual = Readback(device.Get(), immediate.Get(), staged.motionVectors);
				std::vector<std::uint32_t> expected(static_cast<std::size_t>(kSmallEyeW) * kSmallEyeH);
				for (std::uint32_t y = 0; y < kSmallEyeH; ++y) {
					for (std::uint32_t x = 0; x < kSmallEyeW; ++x) {
						const auto raw = sourcePixels[static_cast<std::size_t>(y) * kSmallWideW + rect.x + x];
						expected[static_cast<std::size_t>(y) * kSmallEyeW + x] = finitePair(raw) ? raw : 0u;
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

		backend.DestroyViews();
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

		backend.DestroyViews();
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

		backend.DestroyViews();
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
		const auto before = backend.Probe(device.Get());
		CHECK_FALSE(before.available);
		CHECK_FALSE(Text(before.unavailableReason).empty());

		REQUIRE(backend.Initialize(device.Get(), immediate.Get(), NgxRuntimeDir()));
		const auto after = backend.Probe(device.Get());
		CHECK(after.available);
		CHECK_FALSE(after.supportsSubrects);
		CHECK(after.supportsExposure);
		CHECK_FALSE(after.needsDeviceCreation);

		const auto native = backend.QueryRenderSize(640, 360, QualityMode::kNativeAA);
		CHECK(native.first == 640 && native.second == 360);
		CHECK(backend.JitterPhaseCount(640, 640) == 8);
		CHECK(backend.RecommendedMipBias(640, 640) == -1.0f);

		backend.Shutdown();
		backend.Shutdown();
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

		backend.DestroyViews();
		backend.DestroyViews();
		backend.Shutdown();
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

		backend.DestroyViews();
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
		backend.DestroyViews();
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

	struct Case
	{
		const char* name;
		void (*fn)();
	};

	constexpr Case kCases[] = {
		{ "a hardware D3D11 device comes up with no window and no game", &DeviceComesUp },
		{ "the measured double-wide target allocates at full size", &DoubleWideAllocatesAtFullSize },
		{ "the shipping stereo geometry is exact and invalid shapes fail closed", &ShippingStereoGeometryIsExactAndFailClosed },
		{ "native jitter converts to backend pixels with per-eye extent and verified signs", &NativeJitterConversionUsesPerEyeExtentAndBackendSigns },
		{ "shipping intermediates extract, evaluate and reassemble both eye orders", &ShippingIntermediatesExtractEvaluateAndReassembleBothOrders },
		{ "private motion sanitization preserves finite values and zeros NaN/Inf in both eye orders", &MotionSanitizerPreservesFiniteAndZerosNonFiniteBothOrders },
		{ "D3D11.1 context-state swaps isolate host and persistent private state", &ContextStateSwapIsolatesHostAndPrivateState },
		{ "the exact TAA passthrough shader converts color and writes both MRTs", &TaaPassthroughConvertsAndWritesBothMRTs },
		{ "writing the right eye leaves the left eye byte-for-byte untouched", &RightEyeWriteLeavesLeftEyeUntouched },
		{ "both eyes land in their own half and neither overwrites the other", &BothEyesLandInTheirOwnHalf },
		{ "the backend enforces its call order", &BackendEnforcesCallOrder },
		{ "the control arm reports ratio 1.0 and no divide-by-zero", &ControlArmReportsRatioOne },
		{ "backend auto-exposure is explicit and rejects texture or scalar drift", &BackendAutoExposureContractIsExplicit },
#ifndef UPSCALER_NO_NGX
		{ "the runtime initialises outside a game process", &NgxInitialisesOutsideAGame },
		{ "two feature handles share one device with independent histories", &NgxTwoHandlesOneDevice },
		{ "two NGX views evaluate with the whole stereo D24S8 depth resource", &NgxEvaluatesTwoEyesWithWholeStereoDepth },
		{ "an NGX evaluate into the right half writes only that half", &NgxSubrectWritesOnlyItsHalf },
		{ "shipping state isolation restores the host after two NGX evaluates", &NgxIsolationRestoresHostContextState },
		{ "a non-zero sub-rect base without sub-rects enabled fails as documented", &NgxRejectsSubrectBaseWhenNotEnabled },
#endif
	};
}

int main()
{
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
