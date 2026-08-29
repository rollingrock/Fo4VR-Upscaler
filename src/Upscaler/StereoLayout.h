#pragma once

// Owns all double-wide geometry: the only place a per-eye rectangle is computed.
// Blocked on: whether the pre-tonemap targets share the submitted frame's layout, which
// is a different question from how the submitted frame is laid out.

#include <cstdint>

#include "Upscaler/Backend/BackendTypes.h"

namespace Upscaler::StereoLayout
{
	// The eye is always named. Never a bare int, never a bool isRight.
	enum class Eye
	{
		kLeft = 0,
		kRight = 1
	};

	[[nodiscard]] bool Resolve();

	[[nodiscard]] bool Resolved() noexcept;

	// Zeroed while unresolved; it never guesses. Both eyes come through here indexed,
	// never as two hand-written blocks - divergent eye paths are how one eye ends up a
	// frame stale, and a stale eye in VR is a nausea bug rather than a rendering bug.
	[[nodiscard]] Backend::Rect EyeRect(Eye a_eye, std::uint32_t a_width, std::uint32_t a_height) noexcept;
}
