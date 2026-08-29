#pragma once

// Decides when accumulated reconstruction history is invalid.
// Blocked on: nothing structural - the triggers need engine edges to fire from, which
// arrive with the first hooks.

#include <string_view>

namespace Upscaler::ResetPolicy
{
	// Both eyes are always reset together. Resetting one and not the other is a
	// stereo-rivalry artifact, which is worse than the ghost it fixed.
	//
	// Candidate triggers: menu open and close, save load, cell change, fast travel,
	// resolution change, scope raise. Each becomes a TOML toggle and each reset is
	// logged with its reason, because a wrong reset policy is a documented flicker
	// source and a hardcoded guess is one nobody can audit.
	void RequestReset(std::string_view a_reason);

	[[nodiscard]] bool ConsumeReset() noexcept;
}
