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
	// FidelityFX on D3D11. Stub on this build, and gated off at configure time: it
	// compiles unconditionally, reports itself unavailable, and names what is missing.
	//
	// This is the vendor-neutral arm. It is also the only backend that could produce a
	// self-contained release archive - MIT source, nothing binary to redistribute, no
	// licence question in the package - which is why it is worth carrying even though
	// it is third in the selection order.
	//
	// The open question that unblocks it: which fork, and under which licence.
	// REPORTED, from the research phase and not re-checked here: the official vendor SDK
	// has no D3D11 backend at all. DX11 exists only as community forks, their health
	// differs sharply, and the fork the obvious reference integration pins is roughly
	// twenty months stale. Pinning one is a THIRD_PARTY.md row and a licence read, not a
	// build default, so no fork is pinned and no URL is in scripts/sdk-manifest.psd1.
	//
	// Two facts that shape the implementation whenever it arrives:
	//
	// - Caps::supportsSubrects is permanently false for this backend. Its dispatch
	//   descriptions carry no subrect bases at any offset, so per-eye copies into
	//   zero-origin intermediates are unconditional here regardless of what the NGX
	//   harness concludes.
	// - The dispatch uses structured exception handling and needs /EHa, which is
	//   incompatible with the /EHsc that arrives from CMAKE_CXX_FLAGS in the preset.
	//   That flag cannot be removed per-source - appending /EHa only produces the
	//   override warning, and this tree builds /WX - so turning this backend on means
	//   giving this translation unit its own target with /EHsc stripped from that
	//   target's flags. Decide it in CMakeLists.txt before the first integration bug,
	//   not during one.
	//
	// When the real body lands it is the only file in the tree where an Ffx symbol
	// appears.
	class FidelityFxD3D11Backend final : public IBackend
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
	};
}
