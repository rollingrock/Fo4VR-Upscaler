#pragma once

// Per-eye zero-origin staging textures, and the copies in and out of them.
// Blocked on: the sub-rect harness result, plus resolved stereo geometry.

#include "Upscaler/Backend/BackendTypes.h"
#include "Upscaler/StereoLayout.h"

struct ID3D11Texture2D;

namespace Upscaler::Intermediates
{
	struct Staged
	{
		ID3D11Texture2D* color = nullptr;
		ID3D11Texture2D* depth = nullptr;
		ID3D11Texture2D* motionVectors = nullptr;
		ID3D11Texture2D* output = nullptr;
	};

	// This is the default path, not a fallback: one runtime is documented rejecting
	// non-zero sub-rect bases on this graphics API, and the other carries no sub-rect
	// bases at any offset. A backend reporting supportsSubrects makes the copies an
	// optimisation to drop, not a rewrite.
	//
	// Every cached texture carries the width, height and format it was created for as
	// its cache key, and is torn down and rebuilt when any of them change. For an
	// upscaler that is the normal case rather than the exception.
	[[nodiscard]] bool EnsureFor(const Backend::Config& a_config);

	[[nodiscard]] Staged Get(StereoLayout::Eye a_eye) noexcept;

	void Release() noexcept;
}
