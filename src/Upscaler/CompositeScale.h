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
//
// The correction is affine in clip: c' = (c + 1)/r - 1, so the eight rows post-multiplied
// by T = [[1/rw,0,0,1/rw-1],[0,1/rh,0,1/rh-1],[0,0,1,0],[0,0,0,1]] reconstruct the same
// view position the full raster would have.
//
// WHY THE SCOPE IS ONE DRAW AND NOT ONE FRAME. Rows 20-27 are SHARED. The deferred light
// shader reconstructs through the same rows but forms its clip position from its own
// ratio-scaled screen constant - that is what BSDFLightShader's setup writes 1/(W*r) and
// 1/(H*r) for - and the water shader's depth reconstruction does the same through its
// screen constant's .zw. Both would be DOUBLE-scaled by a patched row: lights would land
// in the wrong place and water refraction would read the wrong depth. Only the composite
// reads these rows with an unscaled clip position, so only the composite may see them
// patched. A whole-frame patch at the uploader was measured on 2026-09-09 and is what that
// damage looks like.
//
// The site is BSDFCompositeShader's vtable slot 3, its per-geometry material setup, found
// by counting all eight members of the class: it is the ONLY one that ever runs while the
// ratios are reduced (298 of 587 calls at 0.666982 x 0.666950 in one Quality launch, where
// slots 4, 5, 9 and 10 ran 362 times each and never saw a reduced ratio, slot 2 three
// times, and slots 0 and 11 never). It writes the composite's per-geometry constants and
// ends by calling the base class's material restore; it issues no draw itself, so the
// composite's draws follow it and the rows have to be patched ACROSS them.
//
// There is therefore no paired restore-side member, and the bracket closes at the first of
// three points instead, all idempotent and all counted separately so the record says which
// one carried it: the next call of that same member, the water shader's own constant setup
// (the pass whose depth reconstruction the stale rows would corrupt), and the TemporalAA
// seam exit as the backstop that cannot be skipped.
//
// The write is one Map with WRITE_NO_OVERWRITE of 128 bytes at buffer offset 0x140 on the
// engine's own camera buffer at renderer state +0x1EA8, which needs the D3D11.1 feature
// MapNoOverwriteOnDynamicConstantBuffer; without it the pass declines with a reason rather
// than falling back to something unsound. Both the patched and the original rows are
// recomputed from eye-0 ViewData, the same operands the uploader uses, so the restore puts
// back exactly what the engine wrote.
//
// Whichever thread the sites run on - slot 3 never runs on ClearColor's - the module's lock
// is what makes that safe. A frame that is not rasterizing reduced declines and does no
// work, which is what keeps DLAA, native and menu frames untouched.

#include <cstdint>

namespace Upscaler::CompositeScale
{
	// The last reason a call declined. One value, not a set: a decline repeats every frame
	// while its cause stands, so the last one is the live one.
	enum class Decline : std::uint32_t
	{
		kNone = 0,
		kNotReduced = 1,     // a live ratio is not finite or is outside (0, 1]
		kRatioOne = 2,       // both live ratios are one: T is the identity
		kNoState = 3,        // ratios, graphics state, ViewData, camera buffer, device or context
		kPartialUpdate = 4,  // the device cannot map a dynamic constant buffer NO_OVERWRITE
		kCopyFailed = 5,     // the map, the write or the unmap failed or raised an SEH
		kRestoreFailed = 6,  // the rows were patched and could not be put back
	};

	struct State
	{
		std::uint64_t attempts = 0;
		std::uint64_t applied = 0;
		std::uint64_t declines = 0;
		std::uint64_t restores = 0;
		std::uint32_t lastDecline = 0;  // a Decline value
		std::uint32_t exceptionCode = 0;
		std::int32_t lastHr = 0;
		float lastRatioWidth = 0.0f;
		float lastRatioHeight = 0.0f;
		std::uint32_t bufferBytes = 0;  // the engine camera buffer's own ByteWidth
		bool ready = false;
		// Which closing point carried each restore. A restore that is mostly the seam
		// backstop would say the two tight points are not firing.
		std::uint64_t restoredAtNextSetup = 0;
		std::uint64_t restoredAtWater = 0;
		std::uint64_t restoredAtSeamExit = 0;
	};

	// Any thread. The live key, published by Configuration::Apply.
	void SetEnabled(bool a_enabled) noexcept;
	[[nodiscard]] bool Enabled() noexcept;

	// Where the bracket was closed, so a reading can tell a tight restore from a late one.
	enum class RestoreSite : std::uint32_t
	{
		kNextSetup = 0,  // the next call of the composite's own geometry setup
		kWater = 1,      // the water shader's constant setup, the pass most at risk
		kSeamExit = 2,   // the TemporalAA seam's exit, the backstop
	};

	// From the composite's per-geometry material setup, AFTER the engine's own setup has
	// returned, so the rows are patched across the draws that follow it. The flag is read
	// once in the hook and passed by value.
	void OnCompositeGeometrySetup(bool a_enabled) noexcept;

	// Puts the engine's own rows back if they are patched, and does nothing if they are
	// not. Idempotent, callable from any of the three closing points and from any thread.
	void RestoreRows(RestoreSite a_site) noexcept;

	// Drops the feature latch, the adopted list and the once-per-flip logs. This module
	// owns no D3D11 object, so there is nothing to free. Takes the lock; any thread.
	void Release() noexcept;

	// Any thread. Asks for a release at the next execute.
	void RequestRelease() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
