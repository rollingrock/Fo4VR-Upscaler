#pragma once

#include "Upscaler/OpenXrPanelBridge.h"

namespace Upscaler::OpenXrPanel
{
	// Plugin-load only, before OpenComposite's xrCreateInstance. Extends only the
	// current process's explicit-layer environment; no registry/runtime changes.
	void Bootstrap() noexcept;
	[[nodiscard]] const OpenXrPanelBridge::Api* Resolve() noexcept;
}
