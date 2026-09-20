#define NOMINMAX
#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "Upscaler/DepthResolveShader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr std::uint32_t kFullEyeWidth = 64;
	constexpr std::uint32_t kFullHeight = 48;
	constexpr std::uint32_t kCustomEyeWidth = 36;   // floor(64 * .57)
	constexpr std::uint32_t kCustomHeight = 27;     // floor(48 * .57)
	// Backend::SnapRenderScale rounds output / 1.5 for vendor Quality.
	constexpr std::uint32_t kDefaultEyeWidth = 43;  // round(64 / 1.5)
	constexpr std::uint32_t kDefaultHeight = 32;    // round(48 / 1.5)
	constexpr float kLeftDepth = 0.75f;
	constexpr float kRightDepth = 0.25f;
	constexpr float kGuardDepth = 0.9375f;
	constexpr std::uint8_t kInitialStencil = 0xA5;
	constexpr std::uint32_t kDepthMask = 0x00FFFFFFu;

	struct Program
	{
		ComPtr<ID3D11VertexShader> vertex;
		ComPtr<ID3D11PixelShader> pixel;
		ComPtr<ID3D11DepthStencilState> depthStencil;
		ComPtr<ID3D11RasterizerState> rasterizer;
		ComPtr<ID3D11Buffer> constants;
	};

	struct Readback
	{
		std::vector<std::uint32_t> pixels;
		std::uint32_t rowPitch = 0;
	};

	[[nodiscard]] std::uint32_t PackDepth(float a_depth)
	{
		return static_cast<std::uint32_t>(std::lround(a_depth * static_cast<float>(kDepthMask))) & kDepthMask;
	}

	[[nodiscard]] bool CheckHr(HRESULT a_hr, const char* a_operation)
	{
		if (SUCCEEDED(a_hr)) {
			return true;
		}
		std::printf("FAIL: %s (HRESULT 0x%08X)\n", a_operation, static_cast<unsigned>(a_hr));
		return false;
	}

	[[nodiscard]] bool Compile(const char* a_entry, const char* a_target, ComPtr<ID3DBlob>& a_blob)
	{
		ComPtr<ID3DBlob> errors;
		const auto hr = D3DCompile(
			Upscaler::DepthResolveShader::kSource,
			sizeof(Upscaler::DepthResolveShader::kSource) - 1,
			"DepthResolveShader.h",
			nullptr,
			nullptr,
			a_entry,
			a_target,
			D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
			0,
			a_blob.ReleaseAndGetAddressOf(),
			errors.GetAddressOf());
		if (FAILED(hr)) {
			std::printf(
				"FAIL: %s/%s compilation (HRESULT 0x%08X): %s\n",
				a_entry,
				a_target,
				static_cast<unsigned>(hr),
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler output");
			return false;
		}
		return true;
	}

	[[nodiscard]] bool BuildProgram(ID3D11Device* a_device, Program& a_program)
	{
		ComPtr<ID3DBlob> vs;
		ComPtr<ID3DBlob> ps;
		if (!Compile("VSMain", "vs_5_0", vs) || !Compile("PSMain", "ps_5_0", ps)) {
			return false;
		}
		if (!CheckHr(a_device->CreateVertexShader(
				vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, a_program.vertex.GetAddressOf()),
			"CreateVertexShader") ||
			!CheckHr(a_device->CreatePixelShader(
				ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, a_program.pixel.GetAddressOf()),
				"CreatePixelShader")) {
			return false;
		}

		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthEnable = TRUE;
		depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
		depth.StencilEnable = TRUE;
		depth.StencilReadMask = 0xFF;
		depth.StencilWriteMask = 0xFF;
		const D3D11_DEPTH_STENCILOP_DESC face{
			D3D11_STENCIL_OP_KEEP,
			D3D11_STENCIL_OP_KEEP,
			D3D11_STENCIL_OP_REPLACE,
			D3D11_COMPARISON_ALWAYS
		};
		depth.FrontFace = face;
		depth.BackFace = face;
		if (!CheckHr(a_device->CreateDepthStencilState(&depth, a_program.depthStencil.GetAddressOf()),
			"CreateDepthStencilState")) {
			return false;
		}

		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = FALSE;
		if (!CheckHr(a_device->CreateRasterizerState(&raster, a_program.rasterizer.GetAddressOf()),
			"CreateRasterizerState")) {
			return false;
		}

		D3D11_BUFFER_DESC buffer{};
		buffer.ByteWidth = sizeof(Upscaler::DepthResolveShader::Constants);
		buffer.Usage = D3D11_USAGE_DEFAULT;
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		return CheckHr(a_device->CreateBuffer(&buffer, nullptr, a_program.constants.GetAddressOf()),
			"CreateBuffer(constants)");
	}

	[[nodiscard]] std::wstring AdapterName(ID3D11Device* a_device)
	{
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC desc{};
		if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()))) ||
			FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) || FAILED(adapter->GetDesc(&desc))) {
			return L"unknown";
		}
		return desc.Description;
	}

	[[nodiscard]] bool CreateDevice(
		D3D_DRIVER_TYPE a_type,
		ComPtr<ID3D11Device>& a_device,
		ComPtr<ID3D11DeviceContext>& a_context)
	{
		constexpr std::array<D3D_FEATURE_LEVEL, 1> levels{ D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL actual{};
		return CheckHr(D3D11CreateDevice(
			nullptr,
			a_type,
			nullptr,
			0,
			levels.data(),
			static_cast<UINT>(levels.size()),
			D3D11_SDK_VERSION,
			a_device.GetAddressOf(),
			&actual,
			a_context.GetAddressOf()),
			a_type == D3D_DRIVER_TYPE_HARDWARE ? "D3D11CreateDevice(HARDWARE)" : "D3D11CreateDevice(WARP)");
	}

	[[nodiscard]] std::vector<std::uint32_t> PackedInput(
		std::uint32_t a_reducedWidth,
		std::uint32_t a_reducedHeight)
	{
		const auto wide = kFullEyeWidth * 2u;
		std::vector<std::uint32_t> pixels(static_cast<std::size_t>(wide) * kFullHeight, PackDepth(kGuardDepth));
		for (std::uint32_t y = 0; y < a_reducedHeight; ++y) {
			for (std::uint32_t x = 0; x < a_reducedWidth; ++x) {
				pixels[static_cast<std::size_t>(y) * wide + x] = PackDepth(kLeftDepth);
				pixels[static_cast<std::size_t>(y) * wide + a_reducedWidth + x] = PackDepth(kRightDepth);
			}
		}
		return pixels;
	}

	[[nodiscard]] bool CreateSource(
		ID3D11Device* a_device,
		const std::vector<std::uint32_t>& a_pixels,
		ComPtr<ID3D11Texture2D>& a_texture,
		ComPtr<ID3D11ShaderResourceView>& a_srv)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kFullEyeWidth * 2u;
		desc.Height = kFullHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = a_pixels.data();
		initial.SysMemPitch = desc.Width * sizeof(std::uint32_t);
		if (!CheckHr(a_device->CreateTexture2D(&desc, &initial, a_texture.GetAddressOf()),
			"CreateTexture2D(source)")) {
			return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		return CheckHr(a_device->CreateShaderResourceView(a_texture.Get(), &srv, a_srv.GetAddressOf()),
			"CreateShaderResourceView(source)");
	}

	[[nodiscard]] bool CreateDestination(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		ComPtr<ID3D11Texture2D>& a_texture,
		ComPtr<ID3D11DepthStencilView>& a_dsv)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kFullEyeWidth * 2u;
		desc.Height = kFullHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (!CheckHr(a_device->CreateTexture2D(&desc, nullptr, a_texture.GetAddressOf()),
			"CreateTexture2D(destination)")) {
			return false;
		}
		D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
		dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		if (!CheckHr(a_device->CreateDepthStencilView(a_texture.Get(), &dsv, a_dsv.GetAddressOf()),
			"CreateDepthStencilView(destination)")) {
			return false;
		}
		a_context->ClearDepthStencilView(
			a_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, kInitialStencil);
		return true;
	}

	[[nodiscard]] bool ReadDestination(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		ID3D11Texture2D* a_source,
		Readback& a_out)
	{
		D3D11_TEXTURE2D_DESC desc{};
		a_source->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> staging;
		if (!CheckHr(a_device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()),
			"CreateTexture2D(staging)")) {
			return false;
		}
		a_context->CopyResource(staging.Get(), a_source);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (!CheckHr(a_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(staging)")) {
			return false;
		}
		a_out.rowPitch = mapped.RowPitch / sizeof(std::uint32_t);
		a_out.pixels.resize(static_cast<std::size_t>(a_out.rowPitch) * desc.Height);
		for (std::uint32_t y = 0; y < desc.Height; ++y) {
			auto* sourceRow = static_cast<const std::uint32_t*>(mapped.pData) +
				static_cast<std::size_t>(y) * a_out.rowPitch;
			auto* destinationRow = a_out.pixels.data() + static_cast<std::size_t>(y) * a_out.rowPitch;
			std::copy_n(sourceRow, a_out.rowPitch, destinationRow);
		}
		a_context->Unmap(staging.Get(), 0);
		return true;
	}

	[[nodiscard]] bool Draw(
		ID3D11DeviceContext* a_context,
		const Program& a_program,
		ID3D11ShaderResourceView* a_source,
		ID3D11DepthStencilView* a_destination,
		const Upscaler::DepthResolveShader::Constants& a_constants)
	{
		a_context->UpdateSubresource(a_program.constants.Get(), 0, nullptr, &a_constants, 0, 0);
		a_context->IASetInputLayout(nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_context->VSSetShader(a_program.vertex.Get(), nullptr, 0);
		a_context->PSSetShader(a_program.pixel.Get(), nullptr, 0);
		ID3D11Buffer* constants[] = { a_program.constants.Get() };
		a_context->PSSetConstantBuffers(0, 1, constants);
		ID3D11ShaderResourceView* source[] = { a_source };
		a_context->PSSetShaderResources(0, 1, source);
		a_context->OMSetRenderTargets(0, nullptr, a_destination);
		a_context->OMSetDepthStencilState(a_program.depthStencil.Get(), 0);
		a_context->RSSetState(a_program.rasterizer.Get());
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(kFullEyeWidth * 2u);
		viewport.Height = static_cast<float>(kFullHeight);
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		a_context->RSSetViewports(1, &viewport);
		a_context->Draw(3, 0);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		a_context->PSSetShaderResources(0, 1, &nullSrv);
		a_context->OMSetRenderTargets(0, nullptr, nullptr);
		return true;
	}

	struct Counts
	{
		std::uint32_t leftMismatch = 0;
		std::uint32_t rightMismatch = 0;
		std::uint32_t nonzeroStencil = 0;
	};

	[[nodiscard]] Counts Inspect(const Readback& a_readback)
	{
		Counts counts{};
		const auto left = PackDepth(kLeftDepth);
		const auto right = PackDepth(kRightDepth);
		for (std::uint32_t y = 0; y < kFullHeight; ++y) {
			for (std::uint32_t x = 0; x < kFullEyeWidth * 2u; ++x) {
				const auto value = a_readback.pixels[static_cast<std::size_t>(y) * a_readback.rowPitch + x];
				const auto expected = x < kFullEyeWidth ? left : right;
				const auto actual = value & kDepthMask;
				const auto delta = actual > expected ? actual - expected : expected - actual;
				if (delta > 1u) {
					if (x < kFullEyeWidth) {
						++counts.leftMismatch;
					} else {
						++counts.rightMismatch;
					}
				}
				if ((value >> 24u) != 0u) {
					++counts.nonzeroStencil;
				}
			}
		}
		return counts;
	}

	[[nodiscard]] bool RunCase(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_context,
		const Program& a_program,
		const char* a_name,
		std::uint32_t a_packedWidth,
		std::uint32_t a_packedHeight,
		std::uint32_t a_constantWidth,
		std::uint32_t a_constantHeight,
		bool a_expectExact)
	{
		const auto input = PackedInput(a_packedWidth, a_packedHeight);
		ComPtr<ID3D11Texture2D> source;
		ComPtr<ID3D11ShaderResourceView> srv;
		ComPtr<ID3D11Texture2D> destination;
		ComPtr<ID3D11DepthStencilView> dsv;
		if (!CreateSource(a_device, input, source, srv) ||
			!CreateDestination(a_device, a_context, destination, dsv)) {
			return false;
		}

		const Upscaler::DepthResolveShader::Constants constants{
			kFullEyeWidth, kFullHeight, a_constantWidth, a_constantHeight
		};
		if (!Draw(a_context, a_program, srv.Get(), dsv.Get(), constants)) {
			return false;
		}
		Readback readback;
		if (!ReadDestination(a_device, a_context, destination.Get(), readback)) {
			return false;
		}
		const auto counts = Inspect(readback);
		const bool exact = counts.leftMismatch == 0 && counts.rightMismatch == 0;
		const bool stencilCleared = counts.nonzeroStencil == 0;
		const bool passed = (a_expectExact ? exact : !exact) && stencilCleared;
		std::printf(
			"%s: %s packed=%ux%u constants=%ux%u left_mismatch=%u right_mismatch=%u nonzero_stencil=%u\n",
			passed ? "PASS" : "FAIL",
			a_name,
			a_packedWidth,
			a_packedHeight,
			a_constantWidth,
			a_constantHeight,
			counts.leftMismatch,
			counts.rightMismatch,
			counts.nonzeroStencil);
		return passed;
	}

	[[nodiscard]] bool RunDevice(D3D_DRIVER_TYPE a_type)
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		if (!CreateDevice(a_type, device, context)) {
			return false;
		}
		std::wprintf(
			L"DEVICE: %ls (%ls)\n",
			a_type == D3D_DRIVER_TYPE_HARDWARE ? L"hardware" : L"warp",
			AdapterName(device.Get()).c_str());
		Program program;
		if (!BuildProgram(device.Get(), program)) {
			return false;
		}

		bool passed = true;
		passed &= RunCase(device.Get(), context.Get(), program, "custom_0.57_exact",
			kCustomEyeWidth, kCustomHeight, kCustomEyeWidth, kCustomHeight, true);
		passed &= RunCase(device.Get(), context.Get(), program, "default_quality_snap_exact",
			kDefaultEyeWidth, kDefaultHeight, kDefaultEyeWidth, kDefaultHeight, true);
		passed &= RunCase(device.Get(), context.Get(), program, "custom_0.57_with_stale_default_negative_control",
			kCustomEyeWidth, kCustomHeight, kDefaultEyeWidth, kDefaultHeight, false);
		return passed;
	}
}

int main()
{
	std::printf("DepthResolveShader exact-source D3D11 regression\n");
	std::printf("geometry: full=%ux%u custom=.57=>%ux%u default=round(output/1.5)=>%ux%u\n",
		kFullEyeWidth,
		kFullHeight,
		kCustomEyeWidth,
		kCustomHeight,
		kDefaultEyeWidth,
		kDefaultHeight);
	const bool hardware = RunDevice(D3D_DRIVER_TYPE_HARDWARE);
	const bool warp = RunDevice(D3D_DRIVER_TYPE_WARP);
	const bool passed = hardware && warp;
	std::printf("%s: hardware and WARP suites\n", passed ? "PASS" : "FAIL");
	return passed ? 0 : 1;
}
