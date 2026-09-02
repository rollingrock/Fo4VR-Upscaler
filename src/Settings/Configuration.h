#pragma once

#include "Settings/Settings.h"

#include <cstdint>

namespace Configuration
{
	// Observer callbacks run with configurationLock and loadLock held, in that
	// order.  DevBench uses them to take an atomic before/after settings pair
	// without owning the runtime-apply implementation.
	struct Observer
	{
		void* context = nullptr;
		Settings::LockedOperation beforeLoad = nullptr;
		Settings::LockedOperation afterApply = nullptr;
	};

	struct ApplyResult
	{
		bool settingsLoaded = false;
		bool settingsSnapshotted = false;
		bool runtimeConfigurationBegan = false;
		bool runtimeApplied = false;
		bool beforeObserved = false;
		bool afterObserved = false;
		std::uint32_t exceptionCode = 0;
	};

	// One serialized path for plugin load, F4SE lifecycle reloads and DevBench.
	// Settings are fully copied before HDR admission closes.  Any abnormal exit
	// after that close makes Dynamic/TAA/HDR terminal and normalizes both epochs
	// before releasing configurationLock.
	[[nodiscard]] ApplyResult Apply(const Observer* a_observer = nullptr) noexcept;
}
