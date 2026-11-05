#pragma once

// A development watchdog for a stalled game. Diagnostics.renderWatchdogSeconds above zero
// starts one background thread that watches a heartbeat the render thread bumps once a
// frame. When the heartbeat stops for that many seconds after it had started, the thread
// suspends the render thread for as long as it takes to read its registers, unwinds its
// stack from the unwind tables alone (no allocation, no lock, no symbol load while the
// thread is suspended), resumes it, and writes the return addresses - module and offset -
// to Fo4VRUpscaler-watchdog.log beside the plugin log with plain file calls, so a stall
// inside the logger cannot hide its own stack. The game's main thread is written the same
// way; the addresses are symbolized offline against the build's PDB. One report per
// stall; a heartbeat that resumes re-arms it and says how long it was out.
//
// A load is not a hang: the heartbeat stops for a cell load as surely as for a hung
// thread, and a load resumes. So a stall is reported only when the heartbeat is still
// stopped a second after the threshold, and never while the loading probe (the game's
// LoadingMenu or FaderMenu open, from the menu events) says a load is up; the threshold
// restarts when the load ends.
//
// Off, Beat is one relaxed load and nothing else exists: no thread, no file.

#include <cstdint>

namespace Upscaler::Watchdog
{
	// Plugin load, on the game's main thread: remembers it for the report.
	void RememberMainThread() noexcept;

	// Any thread. 0 turns it off; the thread, once started, stays parked while off.
	void Configure(double a_seconds) noexcept;

	// Render thread, once per frame, at the frame's first plugin seam.
	void Beat() noexcept;

	// Reports written this session; for /state and the tests.
	[[nodiscard]] std::uint64_t Reports() noexcept;

	// Any thread, once at plugin load: true while a load or a fade is up. None means none.
	void SetLoadingProbe(bool (*a_probe)() noexcept) noexcept;

	// Stalls a load held back, and stalls that resumed within the second look.
	[[nodiscard]] std::uint64_t SuppressedStalls() noexcept;

	// Tests only: write reports to this file instead of beside the plugin log.
	void RedirectReportsForTest(const wchar_t* a_file) noexcept;
}
