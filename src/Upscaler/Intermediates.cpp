#include "Upscaler/Intermediates.h"

#include "Upscaler/Common.h"

namespace Upscaler::Intermediates
{
	namespace
	{
		LogGate g_unresolved;
	}

	bool EnsureFor(const Backend::Config&)
	{
		if (g_unresolved.Ready()) {
			logger::warn("staging textures not created: there is no d3d11 device and no resolved stereo geometry. See docs/MODULE_MAP.md."sv);
		}
		return false;
	}

	Staged Get(StereoLayout::Eye) noexcept
	{
		return Staged{};
	}

	void Release() noexcept
	{
		// Nothing was created.
	}
}
