#include "Upscaler/RenderTargets.h"

#include "Upscaler/Common.h"

#include <d3d11.h>

namespace Upscaler::RenderTargets
{
	namespace
	{
		// One-shot: on this build the answer never changes, and Lookup is a per-frame
		// path the moment it has a body.
		LogGate g_unresolved;
	}

	PhysRT Lookup(std::int32_t) noexcept
	{
		if (g_unresolved.Ready()) {
			logger::warn("render-target resolution unavailable: the remap-table and slot-table offsets are not read yet. See docs/MODULE_MAP.md."sv);
		}
		return PhysRT{};
	}

	void ReleaseLookup(PhysRT& a_rt) noexcept
	{
		SafeRelease(a_rt.tex);
		a_rt.rtv = nullptr;
		a_rt.srv = nullptr;
		a_rt.phys = -1;
	}
}
