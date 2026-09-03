#pragma once

// Descriptor-keyed per-eye zero-origin staging textures, and the exact copies in
// and out of them. With Config::enableSubrects the colour, output and raw-motion
// textures are not built at all: colour and output are addressed in the double-wide
// targets by the backend, and the motion sanitizer reads the engine's double-wide
// motion in place. Completed motion and private depth stay per-eye and zero-origin
// on both paths. The one copy the sub-rect path still owns is on request only: a
// retained per-eye colour texture a capture can read as the evaluate-time input,
// allocated the first time it is asked for and released with the set. This module
// is engine-free and is linked into both the plugin and the standalone hardware
// harness; the tested helpers are therefore the ones that ship.

#include "Upscaler/Backend/BackendTypes.h"
#include "Upscaler/MotionReprojection.h"
#include "Upscaler/StereoLayout.h"

#include <cstdint>

#include <d3d11.h>

namespace Upscaler::Intermediates
{
	struct Staged
	{
		ID3D11Texture2D* color = nullptr;          // borrowed from this module; null on the sub-rect path
		ID3D11Texture2D* depth = nullptr;          // borrowed, zero-origin R32_FLOAT
		ID3D11Texture2D* motionVectors = nullptr;  // borrowed from this module
		ID3D11Texture2D* output = nullptr;         // borrowed from this module; null on the sub-rect path
	};

	struct DepthReference
	{
		ID3D11Texture2D* resource = nullptr;  // borrowed from the renderer
		Backend::Rect rect{};
	};

	struct State
	{
		bool ready = false;
		bool subrects = false;  // the set was built for Config::enableSubrects; colour/output/raw-motion pointers are 0
		std::uint64_t resourceBuilds = 0;
		std::uint64_t motionSanitizerBuilds = 0;
		std::uint64_t motionSanitizerDraws = 0;
		std::uint64_t motionCompletionDraws = 0;
		std::uint64_t motionFallbackDraws = 0;
		std::int32_t lastError = 0;
		std::uint32_t renderWidth = 0;
		std::uint32_t renderHeight = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		std::uint32_t format = 0;
		std::uint32_t motionFormat = 0;
		std::uintptr_t leftColor = 0;
		std::uintptr_t rightColor = 0;
		std::uintptr_t leftMotionVectors = 0;
		std::uintptr_t rightMotionVectors = 0;
		std::uintptr_t leftOutput = 0;
		std::uintptr_t rightOutput = 0;
		// Capture-only retention on the sub-rect path: the pointers are 0 until
		// RetainColor has run for that eye since the set was built.
		std::uint64_t colorRetentions = 0;
		std::uintptr_t leftRetainedColor = 0;
		std::uintptr_t rightRetainedColor = 0;
	};

	// Creates two independent camera-completed motion and shader-extracted depth
	// textures at zero origin, plus - unless a_config.enableSubrects - two colour,
	// two raw-motion and two output textures for the copy path. The cache key includes
	// the device, both complete copy-source descriptors, the four per-eye dimensions
	// and the sub-rect flag. Any change builds a complete replacement set before
	// releasing the previous set.
	//
	// The current gate deliberately accepts only single-subresource, single-sample
	// DEFAULT textures with no CPU or miscellaneous flags. Colour follows verified
	// logical 1 and motion is the measured R16G16_FLOAT logical 35 shape. Expanding this
	// contract belongs to the input that needs it, not to a speculative generic fallback.
	[[nodiscard]] bool EnsureFor(
		ID3D11Device* a_device,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config) noexcept;

	[[nodiscard]] Staged Get(StereoLayout::Eye a_eye) noexcept;

	// D3D11 depth-stencil resources cannot be rectangle-copied. Keep logical DS1 whole
	// and describe one eye with a source rectangle instead. At reduced resolution the
	// two active eyes are packed at x=0 and x=renderWidth inside the full allocation;
	// at native resolution that naturally becomes the ordinary half-width split. This
	// validates the exact shader-readable R24G8 typeless source contract and returns no
	// owning reference.
	[[nodiscard]] DepthReference ReferenceDepth(
		ID3D11Texture2D* a_doubleWide,
		const D3D11_TEXTURE2D_DESC& a_desc,
		const Backend::Rect& a_sourceRect) noexcept;

	// Copy one rectangle from a double-wide texture into a descriptor-compatible
	// zero-origin texture. The helper validates all geometry before issuing the command.
	[[nodiscard]] bool ExtractColor(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWide,
		const Backend::Rect& a_sourceRect,
		ID3D11Texture2D* a_zeroOrigin) noexcept;

	// FO4VR stores current-to-previous reprojection offsets in normalized per-eye UV
	// units in logical target 35, but its no-clear/no-write regions contain zero or
	// non-finite or implausibly large half values. On the copy path the requested
	// rectangle is copied into a private raw texture first; on the sub-rect path the
	// sanitizer samples a_doubleWide itself at the rectangle's base through a cached
	// shader resource view, which requires D3D11_BIND_SHADER_RESOURCE on that texture
	// and declines the frame without it. Either way it preserves finite nonzero pairs
	// inside four per-eye UV extents and fills holes from standard-Z DS1 plus the
	// guarded current-to-previous camera transform, with the same loads and constants,
	// so the two paths are bit-identical. The same pass extracts the requested depth
	// rectangle to private R32_FLOAT, because D3D11 cannot rectangle-copy a
	// depth-stencil resource and some reconstruction APIs expose no input base. Invalid
	// reprojection data leaves motion holes at finite zero. Vendor-specific unit conversion
	// belongs at the backend seam. The caller owns D3D11 context-state isolation around
	// this GPU pass.
	[[nodiscard]] bool ExtractMotion(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWide,
		ID3D11Texture2D* a_doubleWideDepth,
		const Backend::Rect& a_sourceRect,
		ID3D11Texture2D* a_zeroOrigin,
		const MotionReprojection::Data& a_reprojection) noexcept;

	// Copy one complete zero-origin texture into its named rectangle in a double-wide
	// destination. This is the exact inverse geometry used for handback.
	[[nodiscard]] bool ReassembleOutput(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_zeroOrigin,
		ID3D11Texture2D* a_doubleWide,
		const Backend::Rect& a_destinationRect) noexcept;

	// Sub-rect path diagnostics. Copies one eye's packed rectangle of the double-wide
	// colour into a retained zero-origin texture of the set's colour descriptor at the
	// render extent, so a capture can read the evaluate-time input on a path that
	// otherwise keeps no copy of it. The texture is allocated on the first call after
	// EnsureFor built the set and released with it; nothing is allocated or copied
	// until a caller asks. A failed retention releases that eye's texture rather than
	// leaving an older frame behind for the capture to mistake for this one. The
	// caller owns D3D11 context-state isolation around the copy.
	[[nodiscard]] bool RetainColor(
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWide,
		const Backend::Rect& a_sourceRect,
		StereoLayout::Eye a_eye) noexcept;

	// The retained colour for one eye, or null when nothing has been retained since
	// the set was built. Borrowed from this module.
	[[nodiscard]] ID3D11Texture2D* RetainedColor(StereoLayout::Eye a_eye) noexcept;

	[[nodiscard]] State Snapshot() noexcept;

	void Release() noexcept;
}
