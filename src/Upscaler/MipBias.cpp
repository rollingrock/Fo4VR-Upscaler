#include "Upscaler/MipBias.h"

#include "Upscaler/Common.h"

namespace Upscaler::MipBias
{
	namespace
	{
		LogGate g_unresolved;
	}

	bool Apply(float)
	{
		if (g_unresolved.Ready()) {
			logger::warn("mip bias not applied: the sampler-state table has not been located. See docs/MODULE_MAP.md."sv);
		}
		return false;
	}

	void Restore()
	{
		// Nothing was ever rewritten, so there is nothing to put back.
	}
}
