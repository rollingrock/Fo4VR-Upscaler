#pragma once

// Resolves a logical render-target index to live D3D11 objects: a remap table in the
// render-target manager gives a physical slot, and that slot indexes a stride-based
// table hanging off the renderer.
// Blocked on: the remap-table offset, the slot-table base and stride, and the VR array
// lengths.

#include <cstdint>

struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace Upscaler::RenderTargets
{
	struct PhysRT
	{
		ID3D11Texture2D* tex = nullptr;
		ID3D11RenderTargetView* rtv = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		std::int32_t phys = -1;
	};

	// Both bounds checks - logical and physical - are mandatory: the VR array lengths
	// differ from the flat game's, and an out-of-range index reads arbitrary memory.
	// The remap table is rebuilt between game loads, so a logical id resolved once at
	// startup goes stale; resolve on the target-create edge instead.
	[[nodiscard]] PhysRT Lookup(std::int32_t a_logical) noexcept;

	// The texture is derived from the view's GetResource, which adds a reference that
	// belongs to the caller. The two views are borrowed from the engine's slot table and
	// are not ours to release. Every Lookup is paired with one of these.
	void ReleaseLookup(PhysRT& a_rt) noexcept;
}
