#pragma once

// Render-target dumping and input visualisation - the instruments for answering "is the
// motion-vector input actually right", which no look at the final image can answer.
// Blocked on: render-target resolution, and a device to copy through.

#include <cstdint>
#include <string_view>

namespace Upscaler::Debug
{
	// Dumps to DDS, never to BMP: a target in an uncommon two-channel format silently
	// defeated a BMP decoder for a whole investigation, and the motion-vector target is
	// the next candidate for the same trap.
	//
	// Live hazard: capturing a render target while a per-frame pass writes it has
	// deadlocked this game before, and an upscaler writes its output target every frame.
	[[nodiscard]] bool DumpTarget(std::int32_t a_logical, std::string_view a_label);

	// Instrumentation has to represent failure states. Non-finite pixels are reported as
	// such and painted a colour that cannot be mistaken for data, never silently
	// coerced: a non-finite value carries the largest possible exponent, so an
	// exponent-threshold darkness test calls it bright.
	void SetVisualise(std::string_view a_mode);
}
