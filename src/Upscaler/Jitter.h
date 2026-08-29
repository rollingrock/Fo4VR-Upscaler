#pragma once

// Reads and writes the engine's sub-pixel offset, in exactly one place.
// Blocked on: whether the divisor that turns the engine's stored floats into
// render-resolution pixels is the per-eye width or the double-wide width.

#include <cstdint>

namespace Upscaler::Jitter
{
	// Reading the engine's own offset back out is correct for a native-resolution pass
	// with vanilla jitter, and is safe.
	[[nodiscard]] bool ReadEngine(float& a_outX, float& a_outY) noexcept;

	// Always false on this build, and the asymmetry with ReadEngine is the point: jitter
	// written at half magnitude produces a picture that looks right and samples wrong.
	[[nodiscard]] bool WriteEngine(float a_x, float a_y) noexcept;

	// The sequence and its length come from the backend, which was trained on the
	// sequence it names. A sequence tuned for one-to-one temporal anti-aliasing is far
	// too short for reconstruction, and its symptom is shimmer rather than an error.
	[[nodiscard]] std::uint32_t PhaseCount() noexcept;
}
