#include "Upscaler/Debug.h"

namespace Upscaler::Debug
{
	bool DumpTarget(std::int32_t a_logical, std::string_view a_label)
	{
		logger::warn(FMT_STRING("target dump refused for {} (logical {}): render-target resolution is unavailable. See docs/MODULE_MAP.md."),
			a_label, a_logical);
		return false;
	}

	void SetVisualise(std::string_view a_mode)
	{
		logger::warn(FMT_STRING("visualisation mode '{}' ignored: there is nothing rendered to visualise on this build."), a_mode);
	}
}
