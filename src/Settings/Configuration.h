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
	// see RequestModeSwitchAsync.
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

	// What happened to a switch request's own dispatch, as distinct from the switch
	// outcome (ModeSwitchState::lastSwitchOutcome, which only kResolved and a completed
	// kStarted ever move).
	enum class SwitchDispatch : std::uint32_t
	{
		// Decided entirely on the calling thread: unchanged, a refusal, or a rejected
		// mode string. Nothing was published and nothing is in flight, so the returned
		// state is already final.
		kResolved = 0,
		// Accepted. The override is published and the deny window is engaged; the worker
		// owns the wait, the Apply, the window's release and the completion callback.
		kStarted = 1,
		// Another switch is still in flight. Nothing was touched, no counter moved, and
		// the returned state belongs to the switch already running, not to this request -
		// a caller that reports the state without the dispatch reports someone else's
		// outcome as its own.
		kBusy = 2,
		// No switch worker is running, so there is nothing to hand the wait and the apply
		// to. Nothing was left standing.
		kUnavailable = 3,
	};

	// Stable wire spelling, for /mode and anything else that has to say which of the four
	// a request got: "resolved", "started", "busy", "unavailable".
	[[nodiscard]] std::string_view SwitchDispatchName(SwitchDispatch a_dispatch) noexcept;

	// Runs on the switch worker thread once the switch has resolved, with the settled
	// state. A plain function pointer, not a std::function: the callback outlives the
	// request that installed it by design, and nothing here may own a heap capture whose
	// lifetime a caller has to reason about.
	using SwitchCompletion = void (*)(const ModeSwitchState& a_state) noexcept;

	struct SwitchRequest
	{
		SwitchDispatch dispatch = SwitchDispatch::kResolved;
		ModeSwitchState state{};
		// Monotonic, non-zero only for kStarted. RequestModeSwitch waits on it; no other
		// caller needs it.
		std::uint64_t ticket = 0;
	};

	// A live mode-switch request, and the only spelling a frame producer may call. Runs
	// Settings::Policy::ResolveModeSwitch (which validates a_requestedMode against
	// Settings::Policy::IsKnownMode itself) on the calling thread, against the live mode,
	// the session's live bracket-armed state, and the session's armed mode:
	// kRejectedUnknownMode is persisted to /state without counting as a switch attempt;
	// kUnchanged/kRefused* touch no runtime state and are reported before this returns (a
	// refusal is inert, by construction, not by discipline elsewhere); kApplied publishes
	// the runtime override, engages ResetPolicy's mode-switch deny window and hands the
	// wait for that window, the ordinary Configure chain through Apply(), and the window's
	// release to the switch worker. "off" is always reachable. When this session's
	// reduced-world bracket is armed, every reduced mode is reachable - the exact mode the
	// bracket was armed for re-arms unchanged, and any other reduced mode is a live ratio
	// repatch through Upscaler::DynamicResolution::RequestRatio (see Apply()/ApplyLockedImpl
	// in Configuration.cpp) - but "dlaa" is still refused, because the bracket's own arming
	// stays frozen for the process and dlaa has no bracket at all. When the bracket is
	// unarmed, "dlaa" is reachable and every reduced mode is refused.
	//
	// One switch is in flight at a time: a second request while one is running is refused
	// kBusy rather than queued, so a rapid double press cannot stack two configurations
	// behind each other.
	//
	// a_completion, when given, runs on the worker thread with the settled state after a
	// kStarted request finishes - the only way a caller learns an asynchronous outcome,
	// and the reason the hotkey never has to assume one. It may run before this function
	// has returned, so publish any "in flight" placeholder before calling, never after.
	[[nodiscard]] SwitchRequest RequestModeSwitchAsync(
		std::string_view a_requestedMode, SwitchCompletion a_completion = nullptr) noexcept;

	// The blocking spelling: the same request, plus a bounded wait on the worker, so the
	// state that comes back is the settled one rather than a snapshot taken before the
	// switch ran. A kStarted dispatch here means the switch ran and this call waited for
	// it; kResolved, kBusy and kUnavailable mean what they mean above, and a caller that
	// reports the state has to report the dispatch with it.
	//
	// Never call this from a thread the render thread's frame production depends on - not
	// the game thread, not the render thread itself. The deny window the worker waits for
	// is only ever observed by a world frame, so a frame producer waiting here waits for
	// itself; that is what the asynchronous spelling exists to avoid, and what the hotkey
	// uses. The DevBench listener thread is neither, which is why /mode still calls this
	// and still answers with the outcome the switch resolved to.
	[[nodiscard]] SwitchRequest RequestModeSwitch(std::string_view a_requestedMode) noexcept;
}
