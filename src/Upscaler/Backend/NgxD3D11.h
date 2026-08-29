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
	// Raw NGX on D3D11. Stub on this build: it compiles unconditionally, reports itself
	// unavailable, and names what is missing.
	//
	// The open question that unblocks it: do NGX D3D11 output subrects work at all.
	// Nothing in the permitted research corpus has ever observed a non-zero subrect base
	// succeeding on this graphics API - one readable reference reaches the same effect
	// through a viewport abstraction, and another passes the caller's parameter block
	// through verbatim without originating such a call. tools/subrect-harness exists to
	// answer this with no game, no extender and no headset; until it does,
	// Caps::supportsSubrects stays false and the stage layer copies each eye into its
	// own zero-origin intermediate. If the harness says yes, that saves two
	// full-resolution copies per eye per frame and nothing above the seam changes.
	//
	// Two structural facts any real implementation must honour:
	//
	// - Two feature handles with two independent histories, one per eye. Feeding one
	//   handle a double-wide image puts a hard disparity discontinuity down the middle
	//   of its history.
	// - The evaluate takes the IMMEDIATE context. REPORTED (vendor documentation, not
	//   read on this project): the runtime preserves that context's state on D3D11
	//   specifically. That is the single fact making mid-frame injection into a cached
	//   engine state machine safe at all, so the harness verifies it rather than
	//   trusting it.
	//
	// When the real body lands it is the only file in the tree where an NVSDK_NGX_
	// symbol appears - grepping that prefix must return one file. No SDK headers are
	// committed. REPORTED (not read on this project): the NGX headers are proprietary
	// and their licence restricts use that would subject the SDK to an open-source
	// licence. See THIRD_PARTY.md, where that row's status is also REPORTED.
	class NgxD3D11Backend final : public IBackend
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
