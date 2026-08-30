#include "Upscaler/Intermediates.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>

#include <wrl/client.h>

namespace Upscaler::Intermediates
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

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

		ComPtr<ID3D11Texture2D> g_color[2];
		ComPtr<ID3D11Texture2D> g_motionVectors[2];
		ComPtr<ID3D11Texture2D> g_output[2];
		CacheKey g_key{};

		std::atomic<bool> g_ready{ false };
		std::atomic<std::uint64_t> g_resourceBuilds{ 0 };
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
		D3D11_TEXTURE2D_DESC outputDesc = a_colorDesc;
		outputDesc.Width = a_config.outputWidth;
		outputDesc.Height = a_config.outputHeight;

		ID3D11Texture2D* nextColor[2] = {};
		ID3D11Texture2D* nextMotionVectors[2] = {};
		ID3D11Texture2D* nextOutput[2] = {};
		HRESULT hr = S_OK;
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			hr = a_device->CreateTexture2D(&colorDesc, nullptr, &nextColor[eye]);
			if (FAILED(hr) || !nextColor[eye]) {
				break;
			}
			hr = a_device->CreateTexture2D(&motionDesc, nullptr, &nextMotionVectors[eye]);
			if (FAILED(hr) || !nextMotionVectors[eye]) {
				break;
			}
			hr = a_device->CreateTexture2D(&outputDesc, nullptr, &nextOutput[eye]);
			if (FAILED(hr) || !nextOutput[eye]) {
				break;
			}
		}

		if (FAILED(hr) || !nextColor[0] || !nextColor[1] ||
			!nextMotionVectors[0] || !nextMotionVectors[1] ||
			!nextOutput[0] || !nextOutput[1]) {
			for (std::uint32_t eye = 0; eye < 2; ++eye) {
				if (nextColor[eye]) {
					nextColor[eye]->Release();
				}
				if (nextOutput[eye]) {
					nextOutput[eye]->Release();
				}
				if (nextMotionVectors[eye]) {
					nextMotionVectors[eye]->Release();
				}
			}
			g_lastError.store(static_cast<std::int32_t>(FAILED(hr) ? hr : E_FAIL), std::memory_order_relaxed);
			return false;
		}

		g_ready.store(false, std::memory_order_release);
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			g_color[eye].Attach(nextColor[eye]);
			g_motionVectors[eye].Attach(nextMotionVectors[eye]);
			g_output[eye].Attach(nextOutput[eye]);
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

		spdlog::info("intermediates: built two colour, two motion-vector and two output textures at {}x{} per eye, colour dxgi {}, motion dxgi {}",
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
		return { g_color[index].Get(), nullptr, g_motionVectors[index].Get(), g_output[index].Get() };
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
		const Backend::Rect& a_sourceRect,
		ID3D11Texture2D* a_zeroOrigin) noexcept
	{
		return ExtractRectangle(a_immediate, a_doubleWide, a_sourceRect, a_zeroOrigin, DXGI_FORMAT_R16G16_FLOAT);
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
		for (auto& texture : g_motionVectors) {
			texture.Reset();
		}
		for (auto& texture : g_output) {
			texture.Reset();
		}
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
