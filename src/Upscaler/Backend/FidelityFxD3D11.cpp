#include "Upscaler/Backend/FidelityFxD3D11.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

// UPSCALER_ENABLE_FFX is defined by CMakeLists.txt from the option of the same name,
// which is OFF by default. The file compiles either way so the stub keeps type-checking
// against IBackend and cannot rot.
#ifndef UPSCALER_ENABLE_FFX

namespace Upscaler::Backend
{
	namespace
	{
		constexpr std::string_view kUnavailable = "no FidelityFX DX11 source is pinned - the upstream SDK has no D3D11 backend and no community fork has been chosen or licence-checked yet";
	}

	std::string_view FidelityFxD3D11Backend::Name() const noexcept
	{
		return "ffx";
	}

	Caps FidelityFxD3D11Backend::Probe(ID3D11Device*) const noexcept
	{
		Caps caps{};
		caps.available = false;

		// Permanently false for this backend, not pending a measurement. See the header.
		caps.supportsSubrects = false;

		caps.needsDeviceCreation = false;
		SetText(caps.unavailableReason, kUnavailable);
		return caps;
	}

	bool FidelityFxD3D11Backend::Initialize(ID3D11Device*, ID3D11DeviceContext*, const std::filesystem::path&) noexcept
	{
		spdlog::warn("ffx backend: {}", kUnavailable);
		return false;
	}

	bool FidelityFxD3D11Backend::CreateViews(const Config&, std::uint32_t) noexcept
	{
		return false;
	}

	std::pair<std::uint32_t, std::uint32_t> FidelityFxD3D11Backend::QueryRenderSize(
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight,
		QualityMode) const noexcept
	{
		// Neutral, not zero - a zero here would be a divide-by-zero waiting for the one
		// caller that forgets to check availability first.
		return { a_outputWidth, a_outputHeight };
	}

	float FidelityFxD3D11Backend::RecommendedMipBias(std::uint32_t, std::uint32_t) const noexcept
	{
		return 0.0f;
	}

	std::uint32_t FidelityFxD3D11Backend::JitterPhaseCount(std::uint32_t, std::uint32_t) const noexcept
	{
		return 1;
	}

	void FidelityFxD3D11Backend::JitterOffset(std::uint32_t, std::uint32_t, float& a_outX, float& a_outY) const noexcept
	{
		a_outX = 0.0f;
		a_outY = 0.0f;
	}

	bool FidelityFxD3D11Backend::Evaluate(ID3D11DeviceContext*, const EyeView&) noexcept
	{
		return false;
	}

	void FidelityFxD3D11Backend::DestroyViews() noexcept
	{}

	void FidelityFxD3D11Backend::Shutdown() noexcept
	{}
}

#else

#	error "The FidelityFX backend has no implementation yet. Pin a DX11 fork and record its licence row in THIRD_PARTY.md first (see FidelityFxD3D11.h), and give this translation unit a target built with /EHa. Until then configure with -DUPSCALER_ENABLE_FFX=OFF."

#endif
