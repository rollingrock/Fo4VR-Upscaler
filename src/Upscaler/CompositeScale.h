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
// WHY THE SCOPE IS ONE EXECUTE AND NOT ONE FRAME. Rows 20-27 are SHARED. The deferred
// light shader reconstructs through the same rows but forms its clip position from its own
// ratio-scaled screen constant - that is what BSDFLightShader's setup writes 1/(W*r) and
// 1/(H*r) for - and the water shader's depth reconstruction does the same through its
// screen constant's .zw. Both would be DOUBLE-scaled by a patched row: lights would land
// in the wrong place and water refraction would read the wrong depth. Only the composite
// reads these rows with an unscaled clip position, so only the composite may see them
// patched. A whole-frame patch at the uploader was measured on 2026-09-09 and moved far
// more of the image than the composite owns; that breadth was the damage, not the fix.
//
// So the patch is bracketed around the one execute that replays the composite's recorded
// command list: patch, ExecuteCommandList, restore, all on the immediate context, so the
// ordering against the light pass before it and the water after it holds by construction.
// The composite's list is identified rather than guessed - it is the list FinishCommandList
// returns while the composite's own render pass is on the stack - and if no such list is
// ever seen, nothing is ever patched.
//
// The write is one Map with WRITE_NO_OVERWRITE of 128 bytes at buffer offset 0x140 on the
// engine's own camera buffer at renderer state +0x1EA8, which needs the D3D11.1 feature
// MapNoOverwriteOnDynamicConstantBuffer; without it the pass declines with a reason rather
// than falling back to something unsound. Both the patched and the original rows are
// recomputed from eye-0 ViewData at the execute, the same operands the uploader uses, so
// the restore puts back exactly what the engine wrote.
//
// Whichever thread the sites run on; the module's lock is what makes that safe.
// A frame that is not rasterizing reduced declines and does no work, which is what keeps
// DLAA, native and menu frames untouched.

#include <cstdint>

struct ID3D11CommandList;

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
		kNotComposite = 7,   // an execute of a list that is not the composite's
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
		// The composite's recorded list, once it has been seen. Reported as an integer so a
		// reading can tell "never identified" from "identified and never executed".
		std::uint64_t compositeList = 0;
	};

	// Any thread. The live key, published by Configuration::Apply.
	void SetEnabled(bool a_enabled) noexcept;
	[[nodiscard]] bool Enabled() noexcept;

	// From the composite's own render pass, around the engine's call. Nothing is bound and
	// nothing is patched here: this only marks the stack so the list the pass finishes can
	// be told from every other list in the frame.
	void BeginCompositeRecording() noexcept;
	void EndCompositeRecording() noexcept;

	// From the immediate context's FinishCommandList, after the runtime has produced the
	// list. Adopts it as the composite's when a recording is marked on this thread.
	void NoteFinishedCommandList(ID3D11CommandList* a_list) noexcept;

	// From the immediate context's ExecuteCommandList, around the engine's call, and the
	// only place the engine's buffer is written. Patch returns true when the rows were
	// changed and the caller must call Restore after the execute; the flag is read once in
	// the hook and passed by value.
	[[nodiscard]] bool PatchForExecute(ID3D11CommandList* a_list, bool a_enabled) noexcept;
	void RestoreAfterExecute() noexcept;

	// Drops the feature latch, the adopted list and the once-per-flip logs. This module
	// owns no D3D11 object, so there is nothing to free. Takes the lock; any thread.
	void Release() noexcept;

	// Any thread. Asks for a release at the next execute.
	void RequestRelease() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
