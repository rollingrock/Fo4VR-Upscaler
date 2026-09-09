#pragma once

// The seam-exit depth-stencil resolve behind [Diagnostics] resolveDepthAfterSeam.
//
// The reduced path leaves depth-stencil 1 holding the reduced raster's own packed
// rectangle at the origin of the double-wide, and every pass after the seam reads that
// surface at the full extent. Step 9c measured the STENCIL half of that residue and
// cleared it, and the headset answered: the compass came back in both eyes and the left
// eye's ribbon was still cut by geometry that moves with the scene. The depth half is
// what is left, and this module rewrites the whole plane at the full extent from a copy
// of the packed one, zeroing the stencil in the same draw.
//
// Everything here is render-thread only and runs at exactly one site: the TemporalAA
// seam's exit, on a frame that published there, after that draw's isolation bracket has
// closed - the same gate as the stencil clear beside it, and after it when both are on.
// A native, DLAA or native-fallback frame never publishes at the seam and never reaches
// this module.

#include <cstdint>

namespace Upscaler::DepthResolve
{
	// The last reason a call declined. One value, not a set: a decline repeats every
	// frame while its cause stands, so the last one is the live one.
	enum class Decline : std::uint32_t
	{
		kNone = 0,
		kNoDepthStencil = 1,   // DS1, its texture, its DSV, the device or the context
		kExtentMismatch = 2,   // the packed geometry is not known, or does not fit DS1
		kResourceCreation = 3, // a shader, view, state object or buffer would not build
		kIsolation = 4,        // the context-state bracket could not open or close
		kIdentityExtent = 5,   // the reduced raster is the full extent: nothing to map
	};

	struct State
	{
		std::uint64_t attempts = 0;
		std::uint64_t resolves = 0;
		std::uint64_t declines = 0;
		std::uint32_t lastDecline = 0;  // a Decline value
		std::uint32_t exceptionCode = 0;
		std::int32_t lastHr = 0;
		// The geometry the last accepted call resolved with, so a reader can check the
		// packed rectangle the pass believed in against the one the stencil readback saw.
		std::uint32_t fullWidth = 0;
		std::uint32_t fullHeight = 0;
		std::uint32_t reducedEyeWidth = 0;
		std::uint32_t reducedHeight = 0;
		bool ready = false;  // every object is built for the current extent
	};

	// Render thread, at the seam's exit, only with the key on. Lazily builds everything
	// it needs, rebuilds it on an extent change, and issues one CopyResource and one
	// full-screen triangle inside an exact context-state bracket.
	void Run() noexcept;

	// Render thread. Drops every object this module owns, the one-attempt-per-flip build
	// latch included, so whatever comes next builds fresh. Called when the key is seen off
	// at a seam-published frame, when the depth-stencil changed underneath the built
	// resources, and out of ReleaseIfRequested below.
	void Release() noexcept;

	// Any thread. Asks the render thread to release at its next visit to the seam. A live
	// mode change is what this exists for: DLAA, native and off never publish at the seam,
	// so nothing would otherwise reach the release above and the scratch copy of
	// depth-stencil 1 - 115 MiB at a headset extent - would hold video memory for the rest
	// of the session. The freeing itself stays on the render thread, which is the only
	// thread that ever binds what it frees.
	void RequestRelease() noexcept;

	// Render thread, at the seam, on every frame the hook sees whether or not it published.
	// One relaxed exchange when nothing was requested.
	void ReleaseIfRequested() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
