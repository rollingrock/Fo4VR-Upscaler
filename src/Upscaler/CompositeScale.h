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
// WHY THE SITE IS THE DRAW ITSELF, and it cost five rounds to establish. The composite is
// RECORDED on a worker thread and REPLAYED on the render thread. Its per-geometry material
// setup, vtable slot 3, is the only member that ever runs while the ratios are reduced, but
// the census that found it also recorded onRenderThread = 0 for every one of those calls:
// it is recording time. Writing the rows there reaches nothing, because the engine's own
// uploader maps the camera buffer with WRITE_DISCARD, which renames the allocation and
// throws away anything written into the previous one. That was confirmed in the headset on
// 2026-09-09 at 7520x3996: 20,722 applied, zero declines, every patch restored, and the
// puddle and a lamp post's reflection unchanged either way.
//
// So the rows are patched around the DRAW. Every draw on the immediate context is examined
// on the render thread, a composite draw is recognised by its bound pixel shader binding a
// TextureCubeArray at t8 - the declaration the whole composite family shares, and the
// verdict is cached per shader object so the probe is one lookup per draw - and the rows
// are patched, the draw issued, and the rows put back. Nothing else in the frame ever sees
// them changed.
//
// WHY THE SCOPE MATTERS. Rows 20-27 are SHARED. The deferred light shader reconstructs
// through the same rows but forms its clip position from its own ratio-scaled screen
// constant - that is what BSDFLightShader's setup writes 1/(W*r) and 1/(H*r) for - and the
// water shader's depth reconstruction does the same through its screen constant's .zw. Both
// would be DOUBLE-scaled by a patched row: lights would land in the wrong place and water
// refraction would read the wrong depth. Only the composite reads these rows with an
// unscaled clip position. A whole-frame patch at the uploader was measured on 2026-09-09
// and is what that damage looks like.
//
// The correction is affine in clip: c' = (c + 1)/r - 1, so the eight rows post-multiplied
// by T = [[1/rw,0,0,1/rw-1],[0,1/rh,0,1/rh-1],[0,0,1,0],[0,0,0,1]] reconstruct the same
// view position the full raster would have.
//
// The write is one Map with WRITE_NO_OVERWRITE of 128 bytes at buffer offset 0x140 on the
// engine's own camera buffer at renderer state +0x1EA8, which needs the D3D11.1 feature
// MapNoOverwriteOnDynamicConstantBuffer; without it the pass declines with a reason rather
// than falling back to something unsound. Both the patched and the original rows are
// recomputed from eye-0 ViewData, the same operands the uploader uses - a dynamic buffer
// cannot be read back - so the restore puts back exactly what the engine wrote.
//
// Render thread only, and the module is still mutex-guarded because the settings thread
// reaches SetEnabled and RequestRelease. A frame that is not rasterizing reduced declines
// and does no work, which keeps DLAA, native and menu frames untouched.

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
		// Which closing point carried each restore. Anything but kDraw is a net that
		// fired, and a net firing at all is a defect worth seeing.
		std::uint64_t restoredAtDraw = 0;
		std::uint64_t restoredAtWater = 0;
		std::uint64_t restoredAtSeamExit = 0;
		// What the draw hook saw: every draw it examined, the ones it recognised as the
		// composite's, and how many distinct composite pixel shaders were identified.
		std::uint64_t drawsExamined = 0;
		std::uint64_t compositeDraws = 0;
		std::uint32_t compositeShaders = 0;
	};

	// Any thread. The live key, published by Configuration::Apply.
	void SetEnabled(bool a_enabled) noexcept;
	[[nodiscard]] bool Enabled() noexcept;

	// Where the bracket was closed, so a reading can tell a tight restore from a net.
	enum class RestoreSite : std::uint32_t
	{
		kDraw = 0,      // immediately after the composite's own draw, where it belongs
		kWater = 1,     // the water shader's constant setup, a net that should never fire
		kSeamExit = 2,  // the TemporalAA seam's exit, the outermost net
	};

	// Render thread, immediately before the engine's own draw call, and only when that draw
	// has been recognised as the composite's. True when the rows were changed and the
	// caller must call RestoreRows after issuing the draw. The flag is read once in the
	// hook and passed by value.
	[[nodiscard]] bool PatchForDraw(bool a_enabled) noexcept;

	// Puts the engine's own rows back if they are patched, and does nothing if they are
	// not. Idempotent, callable from any closing point and from any thread.
	void RestoreRows(RestoreSite a_site) noexcept;

	// Render thread. Counts a draw the hook examined, and one it recognised as the
	// composite's, so a reading can tell "no composite draw was seen" from "seen and
	// declined". Also records how many distinct composite pixel shaders were identified.
	void NoteDrawExamined() noexcept;
	void NoteCompositeDraw(std::uint32_t a_distinctShaders) noexcept;

	// Drops the feature latch, the adopted list and the once-per-flip logs. This module
	// owns no D3D11 object, so there is nothing to free. Takes the lock; any thread.
	void Release() noexcept;

	// Any thread. Asks for a release at the next execute.
	void RequestRelease() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
