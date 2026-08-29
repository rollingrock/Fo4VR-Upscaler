#include "Upscaler/Backend/Streamline.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

// UPSCALER_ENABLE_STREAMLINE is defined by CMakeLists.txt from the option of the same
// name, which is OFF by default. The file compiles either way so the stub keeps
// type-checking against IBackend and cannot rot.
#ifndef UPSCALER_ENABLE_STREAMLINE

namespace Upscaler::Backend
{
	namespace
	{
		constexpr std::string_view kUnavailable = "Streamline is not yet verified on D3D11 from an F4SEVR plugin - see docs/MODULE_MAP.md for the measurement that settles it";
	}

	std::string_view StreamlineBackend::Name() const noexcept
	{
		return "streamline";
	}

	Caps StreamlineBackend::Probe(ID3D11Device*) const noexcept
	{
		Caps caps{};
		caps.available = false;

		// Declared now so that enabling this backend later is a registry decision, not
		// a change to the plugin entry point. See the header.
		caps.needsDeviceCreation = true;

		caps.supportsSubrects = false;
		SetText(caps.unavailableReason, kUnavailable);
		return caps;
	}

	bool StreamlineBackend::Initialize(ID3D11Device*, ID3D11DeviceContext*, const std::filesystem::path&) noexcept
	{
		spdlog::warn("streamline backend: {}", kUnavailable);
		return false;
	}

	bool StreamlineBackend::CreateViews(const Config&, std::uint32_t) noexcept
	{
		return false;
	}

	std::pair<std::uint32_t, std::uint32_t> StreamlineBackend::QueryRenderSize(
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight,
		QualityMode) const noexcept
	{
		// Neutral, not zero - a zero here would be a divide-by-zero waiting for the one
		// caller that forgets to check availability first.
		return { a_outputWidth, a_outputHeight };
	}

	float StreamlineBackend::RecommendedMipBias(std::uint32_t, std::uint32_t) const noexcept
	{
		return 0.0f;
	}

	std::uint32_t StreamlineBackend::JitterPhaseCount(std::uint32_t, std::uint32_t) const noexcept
	{
		return 1;
	}

	void StreamlineBackend::JitterOffset(std::uint32_t, std::uint32_t, float& a_outX, float& a_outY) const noexcept
	{
		a_outX = 0.0f;
		a_outY = 0.0f;
	}

	bool StreamlineBackend::Evaluate(ID3D11DeviceContext*, const EyeView&) noexcept
	{
		return false;
	}

	void StreamlineBackend::DestroyViews() noexcept
	{}

	void StreamlineBackend::Shutdown() noexcept
	{}
}

#else

#	error "The Streamline backend has no implementation yet. Settle the manual-hooking question first (see Streamline.h), then write the real body here. Until then configure with -DUPSCALER_ENABLE_STREAMLINE=OFF."

#endif
