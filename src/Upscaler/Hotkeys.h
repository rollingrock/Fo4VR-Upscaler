#pragma once

// Two keyboard-only bindings, read with GetAsyncKeyState: no controller input, no OpenVR
// action manifest, no engine address and no input hook. This reads process-wide desktop
// key state the same way any other application does - which is why PollFrame never reads a
// physical key unless this process owns the foreground window and no menu that takes typed
// text is open. GetAsyncKeyState reports the physical key irrespective of focus (REPORTED,
// from Microsoft's own documentation: it returns zero only for a non-active desktop, a UIPI
// block, or a missing desktop right), so without those two checks a keypad key typed into
// the in-game console, or into another application while the game runs behind it, drives
// the mod. Both are read once per frame and counted when they decline, so a run can tell a
// declining poll from an absent one.
//
// A suppressed frame is not a skipped frame. COMPARE, being a level, is still sampled -
// as not-down, without reading the physical key - so the reconciliation below keeps its
// guarantee across a suppression window and a key held when focus left cannot leave the
// operator veto standing. TOGGLE, being an edge, is genuinely skipped: it has nothing to
// strand, and a synthetic release would manufacture a press edge when focus returns to a
// key that never came up. See PollFrame.
//
// TOGGLE switches the live mode between "off" and the non-off mode to come back to: the
// last non-off mode that was actually live while that mode is still reachable, otherwise
// Configuration::SwitchSnapshot's sessionArmedMode while a reduced-world bracket is open,
// "dlaa" while none is, and nothing at all when a diagnostic gate opened the bracket - see
// ComputeRememberedMode and Settings::Policy::ResolveModeSwitch. A session is not limited
// to one reachable reduced mode: a live /mode request or a persisted change can move
// between them, and the toggle returns to whichever one was running rather than to the one
// the session started in. Moving from one reduced mode straight to another is still a
// /mode request or an override-file change, not this key. The request goes through
// Configuration::RequestModeSwitchAsync: the
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
// PANEL is the third binding and the simplest: an edge action on the same debounce as
// TOGGLE, dispatching Upscaler::Panel::Toggle(), which flips one atomic the render
// thread's own panel poll honours on its next frame. It starts and stops nothing, waits
// on nothing and can refuse nothing - a press that lands while the overlay is still being
// created simply flips the intent, and the seam creates it when it gets there.
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
		kPanel = 2,
	};

	// Publishes the three configured VK codes. 0 disables a binding; a value outside the
	// Win32 virtual-key range 1..254 is also treated as disabled, with a logged warning
	// naming the rejected value. Safe from any thread - call it from the settings apply
	// path the way every other module's Configure is called.
	//
	// a_panelVk is passed as 0 by the apply path whenever panelEnabled is false, so a
	// disabled panel has no key at all rather than a key that shows nothing.
	void Configure(std::int64_t a_toggleVk, std::int64_t a_compareVk, std::int64_t a_panelVk) noexcept;

	// One real GetAsyncKeyState read per configured, bound key, debounced independently
	// per action - TOGGLE edge-detected, COMPARE reconciled as a level, see the file
	// comment - then dispatched through the same code SimulateEdge uses. Runs once per
	// frame from the render thread. Reads no key at all on a frame where this process does
	// not own the foreground window or a text-input menu is open; the poll itself still
	// counts, and the reason is reported.
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
		std::int64_t panelVk = 0;
		bool toggleBound = false;
		bool compareBound = false;
		bool panelBound = false;
		std::string lastAction{ "none" };
		std::uint64_t lastActionFrame = 0;
		std::uint64_t toggleEdges = 0;
		std::uint64_t compareEdges = 0;
		std::uint64_t panelEdges = 0;
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
		// Polls that read no key because of each gate, and the two answers the most recent
		// poll got. polls counts every poll including these, so a run proves the wire
		// exists and the gate declines from the same pair of numbers.
		std::uint64_t pollsSuppressedNotForeground = 0;
		std::uint64_t pollsSuppressedTextInput = 0;
		bool windowForeground = false;
		bool textInputOpen = false;
	};

	[[nodiscard]] State Snapshot() noexcept;
}
