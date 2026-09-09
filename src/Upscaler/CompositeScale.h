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
// view position the full raster would have.
//
// The rows are corrected IN the engine's own buffer, at the uploader's exit, and nothing
// is rebound. That follows from what the composite turned out to be: MEASURED 2026-09-09,
// its technique setup runs about 360 times in a 1,240-frame launch and its render pass
// exactly three times, both at ratios 1.0 x 1.0 and both within milliseconds of world
// entry, so the pass is recorded once and replayed. A recorded pass carries its own
// bindings, so slot 12 in it is the engine's buffer OBJECT, fixed; what the composite
// reads each frame is that object's CONTENT when the replay runs. Correcting the content
// once per upload therefore reaches every replay, and binding a copy could never have.
//
// Nothing else in the frame reads rows 20-27: the deferred light pass reconstructs from
// its own constants, which BSDFLightShader's setup fills from the live ratios itself, and
// the motion-vector permutations use rows 51/52/54 and 63/64/66. That is what makes a
// whole-frame correction of these eight rows safe, and it is INFERRED from the shipped
// bytecode survey rather than proved exhaustively.
//
// The write is one Map with WRITE_NO_OVERWRITE of 128 bytes at buffer offset 0x140,
// which needs the D3D11.1 feature MapNoOverwriteOnDynamicConstantBuffer; without it the
// pass declines with a reason rather than falling back to something unsound. No restore
// is needed and none is made: the next upload rewrites the whole buffer.
//
// Whichever thread the uploader runs on; the module's lock is what makes that safe.
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
		kPartialUpdate = 4,     // the device cannot map a dynamic constant buffer NO_OVERWRITE
		kCopyFailed = 5,        // the map, the write or the unmap failed or raised an SEH
	};

	struct State
	{
		std::uint64_t attempts = 0;
		std::uint64_t applied = 0;
		std::uint64_t declines = 0;
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

	// From the global camera uploader's entry hook, AFTER the engine's own upload has
	// returned and unmapped, so the eight rows this rewrites are the ones it just wrote.
	// The flag is read once in the hook and passed by value.
	void OnCameraConstantsUploaded(bool a_enabled) noexcept;

	// Drops the feature latch and re-arms the once-per-flip logs, so whatever comes next
	// probes fresh. This module owns no D3D11 object, so there is nothing to free; the
	// call exists because a key flip has to re-arm the logs and the decline state. Takes
	// the lock; any thread.
	void Release() noexcept;

	// Any thread. Asks for a release at the next upload. A live mode change is what this
	// exists for.
	void RequestRelease() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
