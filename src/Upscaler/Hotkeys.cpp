#include "Upscaler/Hotkeys.h"

#include "Settings/Configuration.h"
#include "Upscaler/ResetPolicy.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace Upscaler::Hotkeys
{
	namespace
	{
		// Chosen, not measured: nothing in this tree characterizes a real keyboard's
		// contact bounce, and this only has to be large enough that two GetAsyncKeyState
		// polls of a genuine flicker land inside one window. At a render thread running
		// well above 60 Hz this is a few milliseconds - short enough that a deliberate
		// press never reads as sluggish.
		constexpr std::uint32_t kDebounceFrames = 6;

		// TOGGLE stays a genuine edge action: one press is one action, and only the
		// transition matters. rawDown always mirrors the physical key exactly - every
		// branch below writes it - so whatever the refractory gate decides about firing,
		// the next genuine hold is still read correctly. A swallowed edge here costs at
		// most one flutter-fast press, never a stuck state: nothing downstream of this
		// key ever waits on a paired release the way the operator veto does, so there is
		// nothing for a lost edge to strand.
		struct ToggleState
		{
			std::mutex mutex;
			bool everObserved = false;
			bool rawDown = false;
			std::uint32_t framesSinceEdge = kDebounceFrames;
		};

		// COMPARE is a level, not an edge - see the file comment. settledDown is the
		// debounced level that ProcessSample reconciles every single poll against
		// dispatchedDown. That reconciliation is what makes this immune to the lost-edge
		// shape TOGGLE's scheme still carries: it never depends on catching one specific
		// poll's transition, only on what the level currently reads, so a poll that
		// could have caught a transition and did not costs nothing - the very next poll
		// still sees the mismatch and corrects it.
		struct CompareState
		{
			std::mutex mutex;
			bool everObserved = false;
			bool rawDown = false;
			// Consecutive polls the raw signal has read not-down, capped at
			// kDebounceFrames. Reset to 0 by any down sample.
			std::uint32_t framesNotDown = kDebounceFrames;
			// The debounced level: true immediately on any down sample (engaging is
			// never delayed), false only once framesNotDown has reached kDebounceFrames
			// - a key reading not-down for a full debounce window, with no exception
			// carved out for how it got there.
			bool settledDown = false;
			// The level this key last actually dispatched Engage/Release for. Comparing
			// settledDown against this, every poll, is the reconciliation.
			bool dispatchedDown = false;
		};

		ToggleState g_toggleState;
		CompareState g_compareState;

		std::atomic<std::int64_t> g_toggleVk{ 0 };
		std::atomic<std::int64_t> g_compareVk{ 0 };

		// Doubles as this module's own tick count and the "frame" a caller sees in
		// /state - real once PollFrame is wired into the render thread, and exactly the
		// simulated sequence number while only SimulateEdge is driving it.
		std::atomic<std::uint64_t> g_frameCounter{ 0 };

		std::atomic<std::uint64_t> g_toggleEdges{ 0 };
		std::atomic<std::uint64_t> g_compareEdges{ 0 };
		std::atomic<std::uint64_t> g_compareEngagements{ 0 };
		std::atomic<std::uint64_t> g_compareDeferredToOperator{ 0 };
		std::atomic<std::uint64_t> g_compareReleases{ 0 };
		std::atomic<std::uint64_t> g_toggleSkippedNoTarget{ 0 };
		std::atomic<std::uint64_t> g_toggleSkippedNoTaskQueue{ 0 };
		std::atomic<std::uint64_t> g_lastActionFrame{ 0 };

		// True only while this key's own press created the operator veto's current
		// engagement - see DispatchCompareEngage/Release. Never set from /ab: that route
		// only ever touches ResetPolicy's atomic directly, so a veto it engages is never
		// mistaken for one this key owns.
		std::atomic_bool g_compareOwnsVeto{ false };

		// atomic<shared_ptr<const string>>, matching Configuration.cpp's g_liveMode:
		// readable from any thread without a lock, and a store can never hand a reader a
		// half-written string.
		std::atomic<std::shared_ptr<const std::string>> g_lastAction{
			std::make_shared<const std::string>("none")
		};
		std::atomic<std::shared_ptr<const std::string>> g_lastToggleOutcome{
			std::make_shared<const std::string>("none")
		};

		void PublishLastAction(std::string_view a_action) noexcept
		{
			try {
				g_lastAction.store(std::make_shared<const std::string>(a_action), std::memory_order_release);
			} catch (...) {
				// A failed allocation here means the label is stale, not wrong - the
				// counters and the frame number below still advance.
			}
		}

		void PublishToggleOutcome(std::string_view a_outcome) noexcept
		{
			try {
				g_lastToggleOutcome.store(std::make_shared<const std::string>(a_outcome), std::memory_order_release);
			} catch (...) {
			}
		}

		[[nodiscard]] std::int64_t ValidateVk(std::int64_t a_requested, std::string_view a_name) noexcept
		{
			if (a_requested == 0) {
				return 0;
			}
			if (a_requested < 1 || a_requested > 0xFE) {
				logger::warn(FMT_STRING("hotkeys: {} = {} is outside the Win32 virtual-key range 1..254; the binding is disabled"),
					a_name, a_requested);
				return 0;
			}
			return a_requested;
		}

		// The one non-off mode this session can ever reach live, or "" when there is
		// none - see the file comment. Computed fresh every call, never cached: it is a
		// pure function of facts Configuration already tracks, and caching it here would
		// only add a second place for it to go stale.
		[[nodiscard]] std::string ComputeRememberedMode(const Configuration::ModeSwitchState& a_snapshot)
		{
			if (a_snapshot.bracketArmed) {
				// Empty when the bracket was armed by a diagnostic gate rather than a
				// product mode (mode was "off" at the time) - Settings::Policy::
				// ResolveModeSwitch's kRefusedNeedsReducedSession case. There is
				// genuinely nothing to switch to.
				return a_snapshot.sessionArmedMode;
			}
			// Unarmed: "dlaa" is reachable regardless of sessionArmedMode (it carries no
			// ratio of its own), and it is the only non-off mode that ever is - see
			// SettingsPolicy.h::ResolveModeSwitch.
			return std::string("dlaa");
		}

		// Runs on the game thread, inside the queued task below: the target mode is
		// resolved from Configuration::SwitchSnapshot() at the moment this task actually
		// executes, never at the moment the key was pressed. A rapid double press
		// queues two of these; each reads the live snapshot for itself when it runs, so
		// the second one sees whatever the first already applied and toggles again from
		// there, rather than both computing the same target from a snapshot taken back
		// at press time - see the note in DispatchToggle.
		void RunQueuedToggle() noexcept
		{
			try {
				const auto snapshot = Configuration::SwitchSnapshot();
				std::string requested;
				if (snapshot.liveMode == "off") {
					requested = ComputeRememberedMode(snapshot);
					if (requested.empty()) {
						g_toggleSkippedNoTarget.fetch_add(1, std::memory_order_relaxed);
						PublishToggleOutcome("skipped-no-reachable-mode"sv);
						logger::warn("hotkey toggle: this session has no non-off mode it can ever reach live - an armed diagnostic gate opened the reduced-world bracket with no product mode behind it - nothing to switch to"sv);
						return;
					}
				} else {
					requested = "off";
				}
				const auto result = Configuration::RequestModeSwitch(requested);
				PublishToggleOutcome(result.lastSwitchOutcome);
			} catch (...) {
				logger::error("hotkey toggle: the queued request could not be applied"sv);
				PublishToggleOutcome("skipped-dispatch-failed"sv);
			}
		}

		void DispatchToggle() noexcept
		{
			const auto* const tasks = F4SE::GetTaskInterface();
			if (!tasks) {
				g_toggleSkippedNoTaskQueue.fetch_add(1, std::memory_order_relaxed);
				PublishToggleOutcome("skipped-no-task-queue"sv);
				logger::warn("hotkey toggle: F4SE task interface unavailable; the request was not queued"sv);
				return;
			}

			// Configuration::RequestModeSwitch takes Settings::configurationLock and
			// spins waiting for the render thread to observe its deny window - calling
			// it here, on the render thread, is exactly the stall/deadlock constraint
			// this module exists to avoid. The task interface is the same game-thread
			// hop main.cpp's own deferred cell-event install already uses. The outcome
			// is not yet known when the key is pressed, so "pending" is published
			// immediately and overwritten with the real outcome once the task runs -
			// /state never goes silent about a request that is still in flight.
			//
			// Nothing about the target is decided here. A rapid double press queues two
			// tasks, and each reads Configuration::SwitchSnapshot() for itself at the
			// moment it actually runs on the game thread rather than acting on a
			// snapshot captured back here at press time - see RunQueuedToggle. The
			// second task therefore sees whatever the first one already applied and
			// toggles again from there, which is what two real presses should do,
			// instead of both computing the same stale target and landing the session
			// where a single press would have.
			PublishToggleOutcome("pending"sv);
			try {
				tasks->AddTask(&RunQueuedToggle);
			} catch (...) {
				logger::error("hotkey toggle: the request could not be dispatched"sv);
			}
		}

		// Precedence against /ab: OperatorVetoActive() is read first, and this key claims
		// ownership only when that read finds the veto not already engaged. An operator's
		// deliberate /ab veto=true is therefore never claimed by a later press, so the
		// paired release below can never clear it. g_compareOwnsVeto is this key's own
		// record of whether ITS press is what turned the veto on, never a mirror of
		// ResetPolicy's atomic itself.
		void DispatchCompareEngage() noexcept
		{
			if (ResetPolicy::OperatorVetoActive()) {
				g_compareOwnsVeto.store(false, std::memory_order_release);
				g_compareDeferredToOperator.fetch_add(1, std::memory_order_relaxed);
				logger::info("hotkey compare: the operator veto is already engaged, most likely through /ab; holding this key does nothing further and releasing it will not clear it"sv);
				return;
			}
			ResetPolicy::SetOperatorVeto(true);
			g_compareOwnsVeto.store(true, std::memory_order_release);
			g_compareEngagements.fetch_add(1, std::memory_order_relaxed);
		}

		// Clears the veto only if this key is still on record as the one that engaged it.
		// A physically released key can therefore never leave the world gate denying
		// forever: either this key owns the standing engagement, in which case releasing
		// the key is exactly what ends it, or it does not, in which case there is nothing
		// here for this key to have stuck in the first place - the operator's own /ab
		// route is what set it and is what clears it.
		void DispatchCompareRelease() noexcept
		{
			bool expected = true;
			if (g_compareOwnsVeto.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
				ResetPolicy::SetOperatorVeto(false);
				g_compareReleases.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// TOGGLE: unchanged edge/refractory debounce. rawDown always takes the newest
		// sample - even inside the refractory window - so a swallowed edge never leaves
		// this action's own bookkeeping out of sync with the physical key; it only ever
		// costs the one flutter-fast press that triggered it. See the ToggleState
		// comment for why that is safe here in a way it was not for COMPARE.
		void ProcessToggleSample(bool a_down, std::uint64_t a_frame) noexcept
		{
			bool pressEdge = false;
			{
				std::scoped_lock lock(g_toggleState.mutex);
				ToggleState& state = g_toggleState;
				if (!state.everObserved) {
					// The very first observation - real or simulated - only establishes
					// a baseline. A key already held when polling starts must not read
					// as a press nobody just made.
					state.everObserved = true;
					state.rawDown = a_down;
					state.framesSinceEdge = kDebounceFrames;
				} else if (a_down != state.rawDown) {
					state.rawDown = a_down;
					if (state.framesSinceEdge >= kDebounceFrames) {
						state.framesSinceEdge = 0;
						pressEdge = a_down;
					}
					// Inside the refractory window: rawDown still took the new sample
					// above, but no edge fires. The very next genuine hold is read
					// correctly regardless, because rawDown is never left stale.
				} else if (state.framesSinceEdge < kDebounceFrames) {
					++state.framesSinceEdge;
				}
			}

			if (!pressEdge) {
				return;
			}
			// TOGGLE acts on the press edge only - a full press-and-release is one
			// action. Acting on release too would make it fire twice per physical
			// press, which is not what "toggle" means for a two-state switch.
			g_toggleEdges.fetch_add(1, std::memory_order_relaxed);
			g_lastActionFrame.store(a_frame, std::memory_order_relaxed);
			PublishLastAction("toggle"sv);
			DispatchToggle();
		}

		// COMPARE: a level action. Every poll updates the debounced level (settledDown)
		// and then reconciles it against dispatchedDown - the level this key last
		// actually acted on - regardless of whether this specific poll is what changed
		// settledDown. That reconciliation, run unconditionally on every single poll, is
		// what guarantees a key reading not-down for a full debounce window always ends
		// this key's own engagement: the correction does not depend on catching any one
		// particular transition, only on what the level currently reads versus what was
		// last dispatched.
		void ProcessCompareSample(bool a_down, std::uint64_t a_frame) noexcept
		{
			bool engage = false;
			bool release = false;
			{
				std::scoped_lock lock(g_compareState.mutex);
				CompareState& state = g_compareState;
				if (!state.everObserved) {
					// COMPARE is a level, not an edge, and its baseline differs from
					// TOGGLE's on purpose. A key already held when polling starts is a
					// genuine held state for a level - the operator is asking for
					// exactly that veto, right now - so the first-ever sample engages
					// when it reads down, the same as any later down sample would.
					// TOGGLE's baseline always swallows instead, because a key already
					// held at startup is not a press anybody made; that rule does not
					// carry over here.
					state.everObserved = true;
					state.rawDown = a_down;
					state.framesNotDown = a_down ? 0 : kDebounceFrames;
					state.settledDown = a_down;
					state.dispatchedDown = a_down;
					engage = a_down;
				} else {
					state.rawDown = a_down;
					if (a_down) {
						// Engaging is never delayed: a real press is trusted the
						// instant it is observed, exactly as before this fix.
						state.framesNotDown = 0;
						state.settledDown = true;
					} else if (state.framesNotDown < kDebounceFrames) {
						++state.framesNotDown;
						if (state.framesNotDown >= kDebounceFrames) {
							state.settledDown = false;
						}
					}
					if (state.settledDown != state.dispatchedDown) {
						engage = state.settledDown;
						release = !state.settledDown;
						state.dispatchedDown = state.settledDown;
					}
				}
			}

			if (!engage && !release) {
				return;
			}
			g_compareEdges.fetch_add(1, std::memory_order_relaxed);
			g_lastActionFrame.store(a_frame, std::memory_order_relaxed);
			if (engage) {
				PublishLastAction("compare-engage"sv);
				DispatchCompareEngage();
			} else {
				PublishLastAction("compare-release"sv);
				DispatchCompareRelease();
			}
		}

		void ProcessSample(Action a_action, bool a_down, std::uint64_t a_frame) noexcept
		{
			if (a_action == Action::kToggle) {
				ProcessToggleSample(a_down, a_frame);
			} else {
				ProcessCompareSample(a_down, a_frame);
			}
		}
	}

	void Configure(std::int64_t a_toggleVk, std::int64_t a_compareVk) noexcept
	{
		g_toggleVk.store(ValidateVk(a_toggleVk, "hotkeyToggleVk"sv), std::memory_order_release);
		g_compareVk.store(ValidateVk(a_compareVk, "hotkeyCompareVk"sv), std::memory_order_release);
	}

	void PollFrame() noexcept
	{
		const std::uint64_t frame = g_frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		if (const auto toggleVk = g_toggleVk.load(std::memory_order_acquire); toggleVk != 0) {
			const bool down = (::GetAsyncKeyState(static_cast<int>(toggleVk)) & 0x8000) != 0;
			ProcessSample(Action::kToggle, down, frame);
		}
		if (const auto compareVk = g_compareVk.load(std::memory_order_acquire); compareVk != 0) {
			const bool down = (::GetAsyncKeyState(static_cast<int>(compareVk)) & 0x8000) != 0;
			ProcessSample(Action::kCompare, down, frame);
		}
	}

	void SimulateEdge(Action a_action, bool a_pressed) noexcept
	{
		const std::uint64_t frame = g_frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		ProcessSample(a_action, a_pressed, frame);
	}

	State Snapshot() noexcept
	{
		State out{};
		out.toggleVk = g_toggleVk.load(std::memory_order_acquire);
		out.compareVk = g_compareVk.load(std::memory_order_acquire);
		out.toggleBound = out.toggleVk != 0;
		out.compareBound = out.compareVk != 0;
		if (const auto action = g_lastAction.load(std::memory_order_acquire); action) {
			out.lastAction = *action;
		}
		out.lastActionFrame = g_lastActionFrame.load(std::memory_order_relaxed);
		out.toggleEdges = g_toggleEdges.load(std::memory_order_relaxed);
		out.compareEdges = g_compareEdges.load(std::memory_order_relaxed);
		out.compareEngagements = g_compareEngagements.load(std::memory_order_relaxed);
		out.compareDeferredToOperator = g_compareDeferredToOperator.load(std::memory_order_relaxed);
		out.compareReleases = g_compareReleases.load(std::memory_order_relaxed);
		out.compareOwnsVeto = g_compareOwnsVeto.load(std::memory_order_acquire);
		out.toggleSkippedNoTarget = g_toggleSkippedNoTarget.load(std::memory_order_relaxed);
		out.toggleSkippedNoTaskQueue = g_toggleSkippedNoTaskQueue.load(std::memory_order_relaxed);
		try {
			out.rememberedMode = ComputeRememberedMode(Configuration::SwitchSnapshot());
		} catch (...) {
			// Leave it empty rather than report a stale guess.
		}
		if (const auto outcome = g_lastToggleOutcome.load(std::memory_order_acquire); outcome) {
			out.lastToggleOutcome = *outcome;
		}
		out.polls = g_frameCounter.load(std::memory_order_relaxed);
		return out;
	}
}
