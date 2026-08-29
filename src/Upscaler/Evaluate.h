#pragma once

// The per-frame orchestrator, and the only file besides StereoLayout that knows there
// are two eyes.
// Blocked on: an installed render-path hook to be called from, and every module below
// it.

namespace Upscaler::Evaluate
{
	// The sequence: gate, stage the inputs, evaluate eye 0, evaluate eye 1, resolve both
	// back, optional sharpen. All stereo knowledge stays here and in StereoLayout, above
	// the backend seam, so a backend cannot get stereo wrong because it cannot see it.
	//
	// Two rules for when this gets a body. Anything that binds D3D11 state saves every
	// stage it touches with the matching Get calls, draws, restores, and releases every
	// reference those Get calls handed back - a leaked view per frame is invisible in a
	// five-minute test and fatal in an hour of play. And the engine caches what it
	// believes is bound: after any pass of ours, its cached-state block is invalidated
	// and its constant buffers rebound, or the artefacts appear several draw calls later
	// in code we did not write.
	[[nodiscard]] bool Frame();

	[[nodiscard]] bool Active() noexcept;
}
