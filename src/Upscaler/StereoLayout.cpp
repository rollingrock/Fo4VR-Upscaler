#include "Upscaler/StereoLayout.h"

#include "Upscaler/Common.h"

namespace Upscaler::StereoLayout
{
	namespace
	{
		bool g_resolved = false;
		LogGate g_unresolved;
	}

	bool Resolve()
	{
		if (g_unresolved.Ready()) {
			logger::warn("stereo layout unresolved: the pre-tonemap targets' dimensions and eye order have not been measured. See docs/MODULE_MAP.md."sv);
		}
		g_resolved = false;
		return false;
	}

	bool Resolved() noexcept
	{
		return g_resolved;
	}

	// The eye-order override exists because every compositor-level tool in this space
	// eventually needed one, so whatever this derives has to stay checkable rather than
	// be assumed correct.
	Backend::Rect EyeRect(Eye, std::uint32_t, std::uint32_t) noexcept
	{
		return Backend::Rect{};
	}
}
