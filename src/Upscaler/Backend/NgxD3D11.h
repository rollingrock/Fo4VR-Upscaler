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
	// D3D11 colour, motion and output sub-rects work when Config::enableSubrects asked
	// for them at creation: tools/subrect-harness evaluates the double-wide stereo
	// target in place at a (W, 0) base, sees only that half written, and gets the same
	// bits as the zero-origin evaluate; without the flag the runtime answers
	// InvalidParameter to any non-zero base, and Evaluate declines such a rect before
	// the call. Caps::supportsSubrects still says false, because taking the per-eye
	// copies out is a stage-layer change with its own acceptance, and nothing above
	// the seam has to move when it flips.
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
	// The implementation file is the only place in the plugin where a vendor SDK symbol
	// appears; the one other user is the raw negative control in tools/subrect-harness.
	// No SDK header or library is committed; the optional build stages the official
	// archive under the ignored cache described by THIRD_PARTY.md.
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

		[[nodiscard]] bool DestroyViews() noexcept override;
		bool Shutdown() noexcept override;

	private:
		// The SDK types stay opaque outside NgxD3D11.cpp.
		void* _state = nullptr;
	};
}
