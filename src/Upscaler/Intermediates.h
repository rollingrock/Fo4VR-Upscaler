#pragma once

// Descriptor-keyed per-eye zero-origin staging textures, and the exact copies in
// and out of them. This module is engine-free and is linked into both the plugin and
// the standalone hardware harness; the tested helpers are therefore the ones that
// ship.

#include "Upscaler/Backend/BackendTypes.h"
#include "Upscaler/StereoLayout.h"

#include <cstdint>

#include <d3d11.h>

namespace Upscaler::Intermediates
{
	struct Staged
	{
		ID3D11Texture2D* color = nullptr;          // borrowed from this module
		ID3D11Texture2D* depth = nullptr;          // semantically staged; null today
		ID3D11Texture2D* motionVectors = nullptr;  // semantically staged; null today
		ID3D11Texture2D* output = nullptr;         // borrowed from this module
	};

	struct State
	{
		bool ready = false;
		std::uint64_t resourceBuilds = 0;
		std::int32_t lastError = 0;
		std::uint32_t renderWidth = 0;
		std::uint32_t renderHeight = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		std::uint32_t format = 0;
		std::uintptr_t leftColor = 0;
		std::uintptr_t rightColor = 0;
		std::uintptr_t leftOutput = 0;
		std::uintptr_t rightOutput = 0;
	};

	// This is the default path, not a fallback. It creates two independent input and
	// two independent output textures, all at zero origin. The cache key includes the
	// device, the complete source descriptor and the four per-eye dimensions. Any
	// change builds a complete replacement set before releasing the previous set.
	//
	// The current colour-only gate deliberately accepts only a single-subresource,
	// single-sample DEFAULT texture with no CPU or miscellaneous flags. That is the
	// verified live logical-1 shape. Expanding this contract belongs to the input that
	// needs it, not to a speculative generic fallback.
	[[nodiscard]] bool EnsureFor(
		ID3D11Device* a_device,
		const D3D11_TEXTURE2D_DESC& a_sourceDesc,
		const Backend::Config& a_config) noexcept;

	[[nodiscard]] Staged Get(StereoLayout::Eye a_eye) noexcept;

	// Copy one rectangle from a double-wide texture into a descriptor-compatible
	// zero-origin texture. The helper validates all geometry before issuing the command.
	[[nodiscard]] bool ExtractColor(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWide,
		const Backend::Rect& a_sourceRect,
		ID3D11Texture2D* a_zeroOrigin) noexcept;

	// Copy one complete zero-origin texture into its named rectangle in a double-wide
	// destination. This is the exact inverse geometry used for handback.
	[[nodiscard]] bool ReassembleOutput(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_zeroOrigin,
		ID3D11Texture2D* a_doubleWide,
		const Backend::Rect& a_destinationRect) noexcept;

	[[nodiscard]] State Snapshot() noexcept;

	void Release() noexcept;
}
