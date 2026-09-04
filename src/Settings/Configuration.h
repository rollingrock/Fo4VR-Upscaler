#pragma once

#include "Settings/Settings.h"
#include "Settings/SettingsPolicy.h"

#include <cstdint>
#include <string>

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

	// The live mode-switch facts /state and /mode both read: what is actually running
	// now, which Settings::Policy::ModeSource produced it, whether this session's
	// reduced-world bracket is armed (Upscaler::DynamicResolution::WorldAdmissionGated(),
	// read live - never a tracked copy), the non-off mode this session's first successful
	// apply configured (empty when that first apply configured "off" - see
	// Settings::Policy::ResolveModeSwitch for why bracketArmed and sessionArmedMode are
	// reported as two independent facts rather than folded into one), and the
	// outcome/count of the last /mode request that reached the admissibility decision.
	// switchAttempts is 0 and lastSwitchOutcome is "none" until the first /mode request;
	// an unknown mode string updates lastSwitchOutcome without advancing switchAttempts -
	// see RequestModeSwitch.
	struct ModeSwitchState
	{
		std::string liveMode;
		std::string modeSource;
		bool bracketArmed = false;
		std::string sessionArmedMode;
		std::string lastSwitchOutcome;
		std::uint64_t switchAttempts = 0;
	};

	[[nodiscard]] ModeSwitchState SwitchSnapshot() noexcept;

	// A live /mode request. Runs Settings::Policy::ResolveModeSwitch (which validates
	// a_requestedMode against Settings::Policy::IsKnownMode itself) against the live mode,
	// the session's live bracket-armed state, and the session's armed mode:
	// kRejectedUnknownMode is persisted to /state without counting as a switch attempt;
	// kUnchanged/kRefused* touch no runtime state and return immediately (a refusal is
	// inert, by construction, not by discipline elsewhere); kApplied publishes the runtime
	// override, forces ResetPolicy's mode-switch deny window, runs the ordinary Configure
	// chain through Apply(), and releases the window. "off" is always reachable. When this
	// session's reduced-world bracket is armed, every reduced mode is reachable - the exact
	// mode the bracket was armed for re-arms unchanged, and any other reduced mode is a
	// live ratio repatch through Upscaler::DynamicResolution::RequestRatio (see
	// Apply()/ApplyLockedImpl in Configuration.cpp) - but "dlaa" is still refused, because
	// the bracket's own arming stays frozen for the process and dlaa has no bracket at all.
	// When the bracket is unarmed, "dlaa" is reachable and every reduced mode is refused.
	// Safe to call from any thread; each call is independent (no queuing), and the
	// authoritative decision inside Apply() clears a standing override whenever it
	// disagrees with this function's own pre-decision - see Configuration.cpp.
	[[nodiscard]] ModeSwitchState RequestModeSwitch(std::string_view a_requestedMode) noexcept;
}
