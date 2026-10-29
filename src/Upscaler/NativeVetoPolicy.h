#pragma once

// The operator A/B veto at the DLAA product gate, as one pure decision, so the
// engine-free harness in tools/world-admission-tests runs the same function the HDR
// dispatch does.
//
// Reduced modes read the veto through world admission (ResetPolicy), which DLAA never
// runs, so DLAA reads it at its own product gate instead. A vetoed frame runs no DLSS
// and publishes no candidate, so the TemporalAA seam finds no handback and the engine's
// own TemporalAA draws: vanilla, with no DLSS and no sharpening. The pre-world hook
// latches the veto for the frame's world raster and withholds the native mip bias while
// it is latched (Hooks.cpp); this gate skips on the latch or on the live veto. A veto
// that flips mid-frame therefore costs one extra native frame at most, and DLSS never
// runs on a raster drawn without the bias it was configured for.
//
// History follows the closed jitter-gate episode in HDRRoundTrip.cpp. The first vetoed
// frame opens an episode and, if a backend and its views are standing, asks for one
// paired reset. That reset stays pending until the first successful stereo evaluation
// after the release, and the same success closes the episode.

namespace Upscaler::HDRRoundTrip::Detail
{
	struct NativeVetoStep
	{
		// Record a skip and return before any DLAA work this frame.
		bool skip = false;
		// The first vetoed frame of an episode.
		bool openEpisode = false;
		// The episode's one paired history reset. Only when histories exist to reset.
		bool requestReset = false;
		// A frame past the release with an episode still open: if it evaluates
		// successfully, it closes the episode and counts the resume.
		bool closeEpisode = false;
	};

	[[nodiscard]] constexpr NativeVetoStep DecideNativeVeto(
		bool a_latched,
		bool a_live,
		bool a_episodeOpen,
		bool a_prepared) noexcept
	{
		NativeVetoStep step{};
		step.skip = a_latched || a_live;
		step.openEpisode = step.skip && !a_episodeOpen;
		step.requestReset = step.openEpisode && a_prepared;
		step.closeEpisode = !step.skip && a_episodeOpen;
		return step;
	}

	// The rules the plugin build itself enforces; the harness walks the whole table.
	static_assert(DecideNativeVeto(true, false, false, false).skip, "a latched raster alone skips");
	static_assert(DecideNativeVeto(false, true, false, false).skip, "a live veto alone skips");
	static_assert(!DecideNativeVeto(false, false, true, true).skip, "a released frame evaluates");
	static_assert(DecideNativeVeto(false, true, false, true).requestReset, "the first prepared frame resets");
	static_assert(!DecideNativeVeto(false, true, true, true).requestReset, "an open episode never resets again");
	static_assert(!DecideNativeVeto(false, true, false, false).requestReset, "no histories, no reset");
	static_assert(DecideNativeVeto(false, false, true, false).closeEpisode, "a released frame closes the episode");
	static_assert(!DecideNativeVeto(true, false, true, true).closeEpisode, "a latched frame keeps it open");
}
