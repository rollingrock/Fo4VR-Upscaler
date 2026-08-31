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
		ID3D11Texture2D* motionVectors = nullptr;  // borrowed from this module
		ID3D11Texture2D* output = nullptr;         // borrowed from this module
	};

	struct DepthReference
	{
		ID3D11Texture2D* resource = nullptr;  // borrowed from the renderer
		Backend::Rect rect{};
	};

	struct State
	{
		bool ready = false;
		std::uint64_t resourceBuilds = 0;
		std::uint64_t motionSanitizerBuilds = 0;
		std::uint64_t motionSanitizerDraws = 0;
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
	};

	// This is the default path, not a fallback. It creates two independent colour,
	// raw-motion, sanitized-motion and output textures, all at zero origin.
	// The cache key includes the device, both complete copy-source descriptors and the
	// four per-eye dimensions. Any
	// change builds a complete replacement set before releasing the previous set.
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
	// and describe one eye with a source rectangle instead. This validates the exact
	// shader-readable R24G8 typeless source contract and returns no owning reference.
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
	// units in logical target 35, but its no-clear regions contain non-finite half values.
	// Copy the requested rectangle into a private raw texture, then preserve every finite
	// R16G16_FLOAT value through a private shader while replacing non-finite pairs with
	// zero. Vendor-specific unit conversion belongs at the backend seam. The caller owns
	// D3D11 context-state isolation around this GPU pass.
	[[nodiscard]] bool ExtractMotion(
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
