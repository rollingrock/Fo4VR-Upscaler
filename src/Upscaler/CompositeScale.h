#pragma once

// The deferred composite's clip-to-view reconstruction, corrected for the reduced raster:
// [Diagnostics] compositeReconstructionScale, default off, live.
//
// The shipped composite pixel shaders form every environment-cube reflection per screen
// pixel. They read uv = SV_Position * cb2[0].xy, build clip = uv*2-1 with no ratio term,
// and reconstruct a view position through the global camera buffer's rows 20-23 (far) or
// 24-27 (near, device depth <= 0.01). Under our reduced raster the pixels sit in a packed
// rectangle at the origin, so clip spans [-1, 2r-1) instead of [-1, 1) and every cube
// reflection is zoomed by 1/r about the eye's top-left corner. The GBuffer reads are
// correct because the GBuffers are packed the same way; only the reconstruction is wrong.
// VERIFIED from the shipped bytecode and from the engine - the composite class never reads
// the live ratios, while BSDFLightShader's own setup does, which is why the lighting is
// right and the reflections are not.
//
// The correction is affine in clip: c' = (c + 1)/r - 1, so the eight rows post-multiplied
// by T = [[1/rw,0,0,1/rw-1],[0,1/rh,0,1/rh-1],[0,0,1,0],[0,0,0,1]] reconstruct the same
// view position the full raster would have. cb12 is shared by every shader in the frame
// and the deferred light pass reconstructs correctly on its own, so the rows are NOT
// changed in place: a copy of the engine's buffer carries them, bound at slot 12 between
// the composite's technique setup and its restore, and the engine's own buffer goes back
// at the restore.
//
// The two sites are the composite's own vtable slots 9 and 10. Neither is the ClearColor
// thread: MEASURED 2026-09-09 over three headless launches, 333 to 357 of about 360
// composite passes ran on a thread that was not ClearColor's and on more than one thread
// within a launch, so this module is mutex-guarded rather than render-thread-owned, and
// the setup/restore handshake is thread-local. Driving the context from there is what the
// engine itself does in the same function.
//
// STATUS, and it is why the key ships false: MEASURED 2026-09-09, the pass declines every
// attempt. At every composite technique setup of a Quality launch the engine's live
// dynamic-resolution ratios read 1.0 x 1.0, so this hook site is outside the window in
// which the raster is reduced - 365 of 365 attempts declined with kRatioOne - and the
// composite's setup is called about 360 times in a 1,240-frame launch rather than once a
// frame, so it is a technique-change site and not a per-frame one. The arithmetic below
// and its self-test stand; the site does not. What unblocks it is one measurement: which
// site issues the composite's draws, and what the live ratios read there.
//
// A frame that is not rasterizing reduced declines and does no work, which is what keeps
// DLAA, native and menu frames untouched.

#include <cstdint>

namespace Upscaler::CompositeScale
{
	// The last reason a call declined. One value, not a set: a decline repeats every frame
	// while its cause stands, so the last one is the live one.
	enum class Decline : std::uint32_t
	{
		kNone = 0,
		kNotReduced = 1,        // a live ratio is not finite or is outside (0, 1]
		kRatioOne = 2,          // both live ratios are one: T is the identity
		kNoState = 3,           // ratios, graphics state, ViewData, camera buffer, device or context
		kResourceCreation = 4,  // the copy or the staging buffer would not build
		kCopyFailed = 5,        // the copy, the row write or the bind raised an SEH
	};

	struct State
	{
		std::uint64_t attempts = 0;
		std::uint64_t applied = 0;
		std::uint64_t declines = 0;
		std::uint64_t restores = 0;
		// What slot 12 held when the restore ran. Ours is the pass having survived to the
		// composite's own draws; the engine's would mean something rebound cb12 in between
		// and the copy never reached a draw.
		std::uint64_t restoreFoundOurs = 0;
		std::uint64_t restoreFoundOther = 0;
		std::uint32_t lastDecline = 0;  // a Decline value
		std::uint32_t exceptionCode = 0;
		std::int32_t lastHr = 0;
		float lastRatioWidth = 0.0f;
		float lastRatioHeight = 0.0f;
		std::uint32_t bufferBytes = 0;  // the engine camera buffer's own ByteWidth
		bool ready = false;
	};

	// Any thread. The live key, published by Configuration::Apply.
	void SetEnabled(bool a_enabled) noexcept;
	[[nodiscard]] bool Enabled() noexcept;

	// From the composite's technique setup AFTER the engine's own setup has returned, so
	// the binding is the last one before the composite's draws. Whichever thread that site
	// runs on; the module's lock is what makes that safe. The flag is read once in the hook
	// and passed by value.
	void OnCompositeSetup(bool a_enabled) noexcept;

	// From the composite's technique restore BEFORE the engine's, on the same thread the
	// setup ran on, and only meaningful when that setup bound. Puts the engine's own buffer
	// back at slot 12.
	void OnCompositeRestore() noexcept;

	// Drops every object this module owns and re-arms the once-per-flip row log, so
	// whatever comes next builds fresh. Takes the lock; any thread.
	void Release() noexcept;

	// Any thread. Asks for a release at the next composite setup, which is where the
	// freeing happens - never from the settings thread, which could free a buffer a
	// composite pass is binding. A live mode change is what this exists for.
	void RequestRelease() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
