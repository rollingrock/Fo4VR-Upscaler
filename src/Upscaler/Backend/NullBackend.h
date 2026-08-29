#pragma once

#include "Upscaler/Backend/IBackend.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

namespace Upscaler::Backend
{
	// Passthrough. Always available, loads nothing, reconstructs nothing, and copies
	// colour to output so the frame is intact.
	//
	// It has two jobs. It is the fail-soft target when no vendor runtime is present,
	// and it is the A/B control arm: it takes the same hooks, the same staging copies
	// and the same per-eye submission as a real backend, so a measurement against it
	// isolates our own overhead from the cost of reconstruction. That is also why it
	// enforces the call order - Initialize, CreateViews, then Evaluate - even though it
	// needs none of that state itself. The layer above should be tested against the
	// same contract a vendor backend will impose on it, and it should be tested before
	// any vendor SDK exists.
	class NullBackend final : public IBackend
	{
	public:
		[[nodiscard]] std::string_view Name() const noexcept override;
		[[nodiscard]] Caps Probe(ID3D11Device* a_device) const noexcept override;

		[[nodiscard]] bool Initialize(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_immediate,
			const std::filesystem::path& a_runtimeDir) noexcept override;

		[[nodiscard]] bool CreateViews(
			const Config& a_config,
			std::uint32_t a_viewCount) noexcept override;

		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> QueryRenderSize(
			std::uint32_t a_outputWidth,
			std::uint32_t a_outputHeight,
			QualityMode a_quality) const noexcept override;

		[[nodiscard]] float RecommendedMipBias(
			std::uint32_t a_renderWidth,
			std::uint32_t a_outputWidth) const noexcept override;

		[[nodiscard]] std::uint32_t JitterPhaseCount(
			std::uint32_t a_renderWidth,
			std::uint32_t a_outputWidth) const noexcept override;

		void JitterOffset(
			std::uint32_t a_frameIndex,
			std::uint32_t a_phaseCount,
			float& a_outX,
			float& a_outY) const noexcept override;

		[[nodiscard]] bool Evaluate(
			ID3D11DeviceContext* a_immediate,
			const EyeView& a_view) noexcept override;

		void DestroyViews() noexcept override;
		void Shutdown() noexcept override;

	private:
		// Recorded, not consumed. A copy needs none of it; keeping it means the control
		// arm holds the same state a vendor backend holds, so a diagnostic that reads it
		// works against either.
		Config _config{};

		std::uint32_t _viewCount = 0;
		std::uint32_t _declineLogs = 0;
		bool _initialized = false;
		bool _loggedFirstEvaluate = false;
	};
}
