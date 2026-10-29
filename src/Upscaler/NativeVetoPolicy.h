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
// That last promise needs the two halves to agree on where the veto is read. The hook
// latches only on the session this gate honours it for (HDRRoundTrip::NativeVetoHonoured:
// the NGX product path at ratio 1). Anywhere else - the FFX product path, a diagnostic
// continuous gate - nothing skips, so the reconstruction keeps its bias as it did before.
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

	// The pre-world half: whether this frame's world raster is drawn for the veto, without
	// the native mip bias. Only a main frame, and only where the gate above will skip it.
	[[nodiscard]] constexpr bool LatchNativeVeto(
		bool a_mainFrame,
		bool a_vetoActive,
		bool a_honoured) noexcept
	{
		return a_mainFrame && a_vetoActive && a_honoured;
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
	static_assert(!LatchNativeVeto(true, true, false), "no latch where the gate does not skip");
	static_assert(!LatchNativeVeto(false, true, true), "a scope or unknown frame never latches");
	static_assert(LatchNativeVeto(true, true, true), "a main frame under the veto latches");
}
