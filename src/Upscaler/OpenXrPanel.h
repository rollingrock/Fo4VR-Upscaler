#pragma once

#include "Upscaler/OpenXrPanelBridge.h"

namespace Upscaler::OpenXrPanel
{
	// Plugin-load only, before OpenComposite's xrCreateInstance, and only for a session
	// whose settings applied with enabled and panelEnabled set. Extends only the current
	// process's explicit-layer environment; no registry/runtime changes. Declines, with a
	// log line, wherever the extension would make the loader fail instance creation.
	void Bootstrap() noexcept;
	[[nodiscard]] const OpenXrPanelBridge::Api* Resolve() noexcept;
}
