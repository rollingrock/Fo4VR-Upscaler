#pragma once

// Resolves a logical render-target index to live D3D11 objects: a remap table in the
// render-target manager gives a physical slot, and that slot indexes a stride-based
// record hanging off the renderer. The dimensions and format come from a third table,
// the manager's properties array, which is indexed by the LOGICAL id - so a lookup
// touches two different index spaces and both bounds checks are mandatory.
//
// Everything here is a plain memory read. No D3D11 method is called, nothing is
// AddRef'd, so a lookup is safe on any thread and costs nothing to discard. USING the
// returned pointers is a different matter and is render-thread only, like every other
// D3D11 access in this plugin.
//
// Ownership: nothing. Every pointer below is borrowed from the engine's own record and
// must not be released. That is a change from the design this file started with, which
// derived the texture from a view's GetResource and therefore owned a reference. The
// record turned out to carry the texture directly, which removes both the COM call and
// a whole class of per-frame leak - so there is no teardown call, because there is
// nothing to tear down.
//
// The one target that has no texture pointer in its record is linear depth. A caller
// that specifically needs an ID3D11Resource for it gets one from the SRV with
// GetResource, on the render thread, and owns that reference.

#include <cstdint>

struct ID3D11DepthStencilView;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;

namespace Upscaler::RenderTargets
{
	struct PhysRT
	{
		ID3D11Texture2D* tex = nullptr;           // borrowed; null on a few targets
		ID3D11RenderTargetView* rtv = nullptr;    // borrowed
		ID3D11ShaderResourceView* srv = nullptr;  // borrowed
		ID3D11UnorderedAccessView* uav = nullptr; // borrowed; only 33 of 144 have one

		std::int32_t logical = -1;
		std::int32_t phys = -1;

		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;  // a DXGI_FORMAT, from the manager's properties array

		bool valid = false;  // the remap and both bounds checks passed
	};

	struct PhysDS
	{
		ID3D11Texture2D* tex = nullptr;           // borrowed
		ID3D11DepthStencilView* dsv = nullptr;    // borrowed
		ID3D11ShaderResourceView* srv = nullptr;  // borrowed - the depth input

		std::int32_t index = -1;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t arraySize = 0;

		bool valid = false;
	};

	// Both bounds checks - logical and physical - are mandatory: the VR array lengths
	// differ from the flat game's, and an out-of-range index reads arbitrary memory.
	//
	// On the question of caching: the remap table was measured on this build to be
	// byte-identical across two separate process launches and across a menu-to-exterior
	// transition within one run, so a logical id does not go stale the way the flat
	// game's lore says it does. Resolving fresh is kept anyway - it is three reads, and
	// the measurement covered one configuration, not every configuration.
	[[nodiscard]] PhysRT Lookup(std::int32_t a_logical) noexcept;

	// The depth-stencil side. **Partial on this build.** The dimensions and array size
	// come back; the views do not, and `valid` is always false.
	//
	// The properties half is read the same way the colour properties are and is
	// trustworthy. The resource half is not: its offsets came from pattern-matching live
	// memory, which is the method that put the colour record 8 bytes low, and a separate
	// reading of the creation path disagrees with the base outright. A pointer that is
	// one field off is still a valid COM object, so it does not fault where the mistake
	// is - it fails somewhere else entirely. Declining is cheaper.
	[[nodiscard]] PhysDS LookupDepth(std::int32_t a_index) noexcept;
}
