#include "Upscaler/Backend/NgxD3D11.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

// UPSCALER_NO_NGX is defined by CMakeLists.txt whenever FindNgxSdk located no staged
// SDK, which includes the case where UPSCALER_ENABLE_NGX is OFF (the default). The
// stub below is what makes the tree build green on a machine with no vendor SDK, which
// is not a convenience: a build only the author can perform is not open source.
#ifdef UPSCALER_NO_NGX

namespace Upscaler::Backend
{
	namespace
	{
		constexpr std::string_view kUnavailable = "no NGX SDK staged - configure with -DUPSCALER_ENABLE_NGX=ON after scripts/fetch-sdks.ps1 has staged one under .sdk-cache/ngx";
	}

	std::string_view NgxD3D11Backend::Name() const noexcept
	{
		return "ngx";
	}

	Caps NgxD3D11Backend::Probe(ID3D11Device*) const noexcept
	{
		Caps caps{};
		caps.available = false;

		// Stays false until tools/subrect-harness proves otherwise. See the header.
		caps.supportsSubrects = false;

		caps.needsDeviceCreation = false;
		SetText(caps.unavailableReason, kUnavailable);
		return caps;
	}

	bool NgxD3D11Backend::Initialize(ID3D11Device*, ID3D11DeviceContext*, const std::filesystem::path&) noexcept
	{
		spdlog::warn("ngx backend: {}", kUnavailable);
		return false;
	}

	bool NgxD3D11Backend::CreateViews(const Config&, std::uint32_t) noexcept
	{
		return false;
	}

	std::pair<std::uint32_t, std::uint32_t> NgxD3D11Backend::QueryRenderSize(
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight,
		QualityMode) const noexcept
	{
		// Neutral, not zero. The backend is unavailable so nothing reaches an evaluate,
		// and a zero here would be a divide-by-zero waiting for the one caller that
		// forgets to check availability first.
		return { a_outputWidth, a_outputHeight };
	}

	float NgxD3D11Backend::RecommendedMipBias(std::uint32_t, std::uint32_t) const noexcept
	{
		return 0.0f;
	}

	std::uint32_t NgxD3D11Backend::JitterPhaseCount(std::uint32_t, std::uint32_t) const noexcept
	{
		return 1;
	}

	void NgxD3D11Backend::JitterOffset(std::uint32_t, std::uint32_t, float& a_outX, float& a_outY) const noexcept
	{
		a_outX = 0.0f;
		a_outY = 0.0f;
	}

	bool NgxD3D11Backend::Evaluate(ID3D11DeviceContext*, const EyeView&) noexcept
	{
		return false;
	}

	void NgxD3D11Backend::DestroyViews() noexcept
	{}

	void NgxD3D11Backend::Shutdown() noexcept
	{}
}

#else

#	error "The NGX backend has no implementation yet - an SDK is staged but nothing consumes it. Answer the output-subrect question with tools/subrect-harness first (see NgxD3D11.h), then write the real body here. Until then configure with -DUPSCALER_ENABLE_NGX=OFF."

#endif
