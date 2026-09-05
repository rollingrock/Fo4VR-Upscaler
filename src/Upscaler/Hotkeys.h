#pragma once

// Two keyboard-only bindings, read with GetAsyncKeyState: no controller input, no OpenVR
// action manifest, no engine address and no input hook. This reads process-wide desktop
// key state the same way any other application does.
//
// TOGGLE switches the live mode between "off" and the mode this session was armed with:
// Configuration::SwitchSnapshot's sessionArmedMode while a reduced-world bracket is open,
// "dlaa" while none is, and nothing at all when a diagnostic gate opened the bracket - see
// ComputeRememberedMode and Settings::Policy::ResolveModeSwitch. That is one target, not a
// set worth remembering by hand, which is why this key has no history of its own. Moving
// from one reduced mode straight to another is a /mode request or an override-file change
// instead. The request goes through Configuration::RequestModeSwitchAsync: the
// admissibility decision runs on the polling thread, the wait for the world gate and the
// configuration apply run on Configuration's own switch worker. Neither the render thread
// nor the game thread ever runs a switch - the game thread parked in an F4SE task
// produces no world frame, so the deny window it waits for can never be observed - and the
// outcome this key reports is the one that worker resolved to, never an assumed one.
//
// COMPARE holds the operator A/B veto (Upscaler::ResetPolicy::SetOperatorVeto) engaged
// for as long as the key reads down, and is driven as a level, not an edge: every poll
// reconciles the debounced key state against what this key last actually engaged or
// released, rather than acting only on a specific poll's transition. Engaging is instant
// - a down sample is trusted the moment it is observed - but the key has to read not-down
// for a full debounce window before a release is dispatched. That reconciliation, run
// unconditionally on every poll, is what guarantees a key that is genuinely up eventually
// clears its own engagement no matter what any individual sample did - a transition a
// debounce window happened to swallow can never leave the veto owned by a key that is no
// longer held. Ownership is tracked locally so a release only ever clears the engagement
// this key itself created - a veto standing because of a deliberate /ab request is never
// cleared by a key coming up. See ProcessCompareSample and DispatchCompareEngage/Release
// in Hotkeys.cpp for exactly how.
//
// PollFrame is called once per frame from the render thread, from the verified primary
// ClearColor seam in Hooks.cpp - the same per-frame hook HDRRoundTrip::OnPrimaryRenderThread
// already uses - so it runs every frame regardless of live mode or menu state. SimulateEdge
// drives the identical debounce/dispatch code from DevBench's /hotkey route, so every
// behaviour above is exercised headlessly with no physical key and no live wire-in
// required.

#include <cstdint>
#include <string>

namespace Upscaler::Hotkeys
{
	enum class Action : std::uint32_t
	{
		kToggle = 0,
		kCompare = 1,
	};

	// Publishes the two configured VK codes. 0 disables a binding; a value outside the
	// Win32 virtual-key range 1..254 is also treated as disabled, with a logged warning
	// naming the rejected value. Safe from any thread - call it from the settings apply
	// path the way every other module's Configure is called.
	void Configure(std::int64_t a_toggleVk, std::int64_t a_compareVk) noexcept;

	// One real GetAsyncKeyState read per configured, bound key, debounced independently
	// per action - TOGGLE edge-detected, COMPARE reconciled as a level, see the file
	// comment - then dispatched through the same code SimulateEdge uses. Runs once per
	// frame from the render thread.
	void PollFrame() noexcept;

	// Feeds one simulated raw key sample through the identical debounce/dispatch code
	// PollFrame uses. a_pressed is the simulated raw state (true = down), not a demand to
	// fire anything - repeating the state already on file is correctly read as no change,
	// exactly as a held physical key would be.
	void SimulateEdge(Action a_action, bool a_pressed) noexcept;

	struct State
	{
		std::int64_t toggleVk = 0;
		std::int64_t compareVk = 0;
		bool toggleBound = false;
		bool compareBound = false;
		std::string lastAction{ "none" };
		std::uint64_t lastActionFrame = 0;
		std::uint64_t toggleEdges = 0;
		std::uint64_t compareEdges = 0;
		std::uint64_t compareEngagements = 0;
		std::uint64_t compareDeferredToOperator = 0;
		std::uint64_t compareReleases = 0;
		bool compareOwnsVeto = false;
		std::uint64_t toggleSkippedNoTarget = 0;
		std::uint64_t toggleSkippedInFlight = 0;
		std::uint64_t toggleSkippedDispatchFailed = 0;
		std::string rememberedMode;
		std::string lastToggleOutcome{ "none" };
		std::uint64_t polls = 0;
	};

	[[nodiscard]] State Snapshot() noexcept;
}
