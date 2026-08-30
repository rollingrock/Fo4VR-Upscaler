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
	// Raw NGX on D3D11. It compiles as a fail-soft stub when the optional SDK is absent.
	//
	// The remaining optimisation question is whether D3D11 output subrects work at all.
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
	// - The evaluate takes the immediate context. The caller still wraps it in the
	//   plugin's D3D11.1 context-state isolation; a vendor state-preservation promise is
	//   not used as a substitute for exact host handback.
	//
	// The implementation file is the only place in the tree where a vendor SDK symbol
	// appears. No SDK header or library is committed; the optional build stages the
	// official archive under the ignored cache described by THIRD_PARTY.md.
	class NgxD3D11Backend final : public IBackend
	{
	public:
		~NgxD3D11Backend() override;

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
		// The SDK types stay opaque outside NgxD3D11.cpp.
		void* _state = nullptr;
	};
}
