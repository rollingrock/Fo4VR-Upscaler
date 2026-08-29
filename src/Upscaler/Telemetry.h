#pragma once

// Per-stage GPU and CPU timing, and the evaluate counters behind the dev tooling's perf
// answer.
// Blocked on: a D3D11 device to create queries on, and stages worth timing.

#include <cstdint>
#include <string>
#include <string_view>

namespace Upscaler::Telemetry
{
	// A ring of timestamp-disjoint query pairs several frames deep, collected without
	// flushing, discarding any frame whose disjoint result says the clock was disturbed,
	// and paired with QueryPerformanceCounter marks so a CPU-bound stage can be told
	// from a GPU-bound one. The unbalanced Begin a fault leaves behind is handled rather
	// than assumed away. None of this is borrowed: the dev tooling host has no GPU stage
	// timers on this platform.
	void Begin();
	void Mark(std::uint32_t a_slot, std::string_view a_label);
	void End();

	// Non-blocking. Whole-frame percentiles stay the host's job; do not duplicate them.
	void Collect();

	// The JSON body the perf action serves.
	[[nodiscard]] std::string Report();
}
