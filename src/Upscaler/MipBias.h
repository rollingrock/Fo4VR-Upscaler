#pragma once

// Rewrites the engine's sampler states with a new mip LOD bias and puts them back.
// Blocked on: where the sampler-state table lives, and a resolved ratio to derive a
// bias from.

namespace Upscaler::MipBias
{
	// The bias comes from the selected backend's own formula - they differ between
	// vendors, which is why RecommendedMipBias sits on the backend seam rather than in a
	// shared helper - and a TOML override wins over both, because vendor guidance warns
	// the recommended bias can produce flicker and moire.
	[[nodiscard]] bool Apply(float a_bias);

	// Every original sampler state is cached before the first rewrite. Restore runs on
	// shutdown and on a decline, and getting it right matters as much as Apply does: a
	// mip bias left applied after the pass is off is a permanently blurry game.
	void Restore();
}
