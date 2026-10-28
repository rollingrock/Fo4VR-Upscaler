#include "Upscaler/HudMessages.h"

#include "Upscaler/Common.h"
#include "Upscaler/FaultGuard.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/ResetPolicy.h"

#include "Settings/Configuration.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace Upscaler::HudMessages
{
	namespace
	{
		// A HUD line is a status sentence, not a paragraph - this is comfortably wider
		// than anything the call sites below build, with room to spare for a future one.
		constexpr std::size_t kMessageBufferSize = 192;

		// Bounded, and larger than it needs to be for one burst: the throttle below defers
		// rather than drops, so a line queued while another was delivered less than a
		// second ago stays in this ring until its turn instead of disappearing. A queue
		// that waits needs more room than a queue that discards - the observed worst burst
		// (eight mode requests inside 4.4 s) leaves five or six lines waiting at once.
		constexpr std::size_t kQueueCapacity = 16;

		constexpr std::uint64_t kThrottleMs = 1000;

		struct Slot
		{
			char text[kMessageBufferSize];
			// Delivered before any ordinary line already waiting. Only the two lines a
			// player must not miss set it - see HudMessages.h.
			bool important;
		};

		std::mutex g_ringMutex;
		Slot g_ring[kQueueCapacity]{};
		std::size_t g_head = 0;
		std::size_t g_tail = 0;
		std::size_t g_count = 0;
		// g_count, readable without the ring mutex. PollFrame asks "is anything waiting"
		// once per frame on the render thread, and that question must not take a lock to
		// answer. Written only under g_ringMutex, so it can never disagree with g_count for
		// longer than the store itself.
		std::atomic<std::size_t> g_pending{ 0 };

		std::atomic<bool> g_drainPending{ false };

		std::atomic<std::uint64_t> g_queued{ 0 };
		std::atomic<std::uint64_t> g_dispatched{ 0 };
		std::atomic<std::uint64_t> g_droppedByThrottle{ 0 };
		std::atomic<std::uint64_t> g_deferredByThrottle{ 0 };
		std::atomic<std::uint64_t> g_droppedByFull{ 0 };
		std::atomic<std::uint64_t> g_deliveryFaults{ 0 };
		std::atomic<std::uint64_t> g_polls{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionEdges{ 0 };
		std::atomic<std::uint64_t> g_sessionRecoveryEdges{ 0 };
		std::atomic<std::uint64_t> g_lastDispatchTicks{ 0 };

		// atomic<shared_ptr<const string>>, matching Hotkeys.cpp's g_lastAction: readable
		// from any thread without a lock, and a store can never hand a reader a
		// half-written string. Only ever written from Drain, on the game thread.
		std::atomic<std::shared_ptr<const std::string>> g_lastMessage{
			std::make_shared<const std::string>("none")
		};

		void PublishLastMessage(std::string_view a_text) noexcept
		{
			try {
				g_lastMessage.store(std::make_shared<const std::string>(a_text), std::memory_order_release);
			} catch (...) {
				// A failed allocation here means /state reports a stale line, not a wrong
				// one - every counter below still advances.
			}
		}

		// Pushes a_text (already truncated to fit) into the next free slot. Returns
		// false when the ring is full, in which case the caller counts and drops it -
		// a HUD line that cannot be queued is never a reason to disable anything.
		[[nodiscard]] bool Push(std::string_view a_text, bool a_important) noexcept
		{
			std::scoped_lock lock(g_ringMutex);
			if (g_count == kQueueCapacity) {
				return false;
			}
			const auto len = a_text.size() < (kMessageBufferSize - 1) ? a_text.size() : (kMessageBufferSize - 1);
			std::memcpy(g_ring[g_tail].text, a_text.data(), len);
			g_ring[g_tail].text[len] = '\0';
			g_ring[g_tail].important = a_important;
			g_tail = (g_tail + 1) % kQueueCapacity;
			++g_count;
			g_pending.store(g_count, std::memory_order_release);
			return true;
		}

		// Pops the line to deliver next into a_out, a fixed local buffer the caller owns:
		// the oldest important slot when one is waiting, otherwise the oldest slot of any
		// kind. Returns false when the ring is empty.
		//
		// Priority is a reordering, never a discard: an ordinary line an important one
		// jumped ahead of is still delivered, one throttle interval later. Closing the gap
		// by shifting the older slots forward keeps every remaining line in the order it
		// was enqueued, so a burst still reads as a sequence rather than as whatever the
		// selection happened to leave behind.
		[[nodiscard]] bool Pop(char (&a_out)[kMessageBufferSize]) noexcept
		{
			std::scoped_lock lock(g_ringMutex);
			if (g_count == 0) {
				return false;
			}
			std::size_t chosen = g_head;
			for (std::size_t n = 0; n < g_count; ++n) {
				const std::size_t index = (g_head + n) % kQueueCapacity;
				if (g_ring[index].important) {
					chosen = index;
					break;
				}
			}
			std::memcpy(a_out, g_ring[chosen].text, kMessageBufferSize);
			while (chosen != g_head) {
				const std::size_t previous = (chosen + kQueueCapacity - 1) % kQueueCapacity;
				g_ring[chosen] = g_ring[previous];
				chosen = previous;
			}
			g_head = (g_head + 1) % kQueueCapacity;
			--g_count;
			g_pending.store(g_count, std::memory_order_release);
			return true;
		}

		// [GHIDRA] afbe30 US_SendHUDMessage_ShowHUDMessage - the PDB's real,
		// class-qualified name (SendHUDMessage::ShowHUDMessage), found by name via
		// search_functions, not guessed. A free function, no hidden `this`: void
		// ShowHUDMessage(const char* a_message /*RCX*/, const char* a_sound /*RDX,
		// nullable*/, bool a_param3 /*R8B*/, bool a_param4 /*R9B*/). It copies both
		// strings into a stack ShowHUDMessageEvent (BSFixedString ctor twice) and
		// notifies that event's BSTGlobalEvent::EventSource singleton; notifying zero
		// subscribers is a no-op, not a fault, so this is safe to call before any HUD
		// menu exists. Confirmed against the real call site
		// AddCrimeTrackingFactionCrimeGold @ 140df4ac0: ShowHUDMessage(buf, 0, 1) -
		// sound null, a_param3 true. That call site passes three arguments; this
		// module's own call below passes all four, since the signature the hunt found
		// takes four. a_param4's meaning was not established by this session's hunt -
		// DeliverGuarded below passes false for it, the conservative default, rather
		// than guessing at a value no observed call site settles.
		//
		// Used by exactly one translation unit (this one), so per Addresses.h's own
		// rule this stays here rather than in that shared file: "Call-only function
		// RVAs used by exactly one translation unit stay in a file-local constexpr
		// block next to their typedef; this file is for patch sites and for anything
		// used twice."
		constexpr std::uintptr_t kShowHUDMessageFn = 0xafbe30;

		// ShowHUDMessage's own first six bytes, read out of the live process at
		// base+kShowHUDMessageFn: 40 53 48 83 EC 50, which disassembles as
		// PUSH RBX (encoded with a redundant REX prefix, 40 53, not a bare 53) followed
		// by SUB RSP,0x50. Ghidra renders both encodings of that push as "PUSH RBX", so
		// a byte string derived from the mnemonic rather than from the encoded bytes
		// misses the 40 and never matches. Checked once, lazily, against the live
		// process before this module ever calls through the address - a moved or
		// different build fails this check and every line logs instead of being handed
		// to a function this session never verified is still there, matching how every
		// other engine entry point in this tree is anchored.
		[[nodiscard]] bool DeliveryVerified() noexcept
		{
			static std::atomic<bool> s_checked{ false };
			static std::atomic<bool> s_verified{ false };
			bool expected = false;
			if (s_checked.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				constexpr std::uint8_t kExpected[6] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x50 };
				std::uint8_t actual[6]{};
				const auto address = REL::Module::get().base() + kShowHUDMessageFn;
				const bool matched = FaultGuard::SafeReadBytes(
										 reinterpret_cast<const void*>(address), actual, sizeof(actual)) &&
				                     std::memcmp(actual, kExpected, sizeof(kExpected)) == 0;
				s_verified.store(matched, std::memory_order_release);
				if (!matched) {
					logger::warn(FMT_STRING("hud message: ShowHUDMessage's prologue at RVA {:X} did not match the bytes this session read from Fallout4VR.exe; HUD lines will log only for this session"),
						kShowHUDMessageFn);
				}
			}
			return s_verified.load(std::memory_order_acquire);
		}

		// The real call, isolated in its own noexcept function with no destructible
		// locals so __try/__except can sit directly in it (MSVC C2712 forbids __try in a
		// function that needs C++ unwinding). a_text and a_sound are both plain
		// null-terminated engine-owned-by-the-time-it-returns buffers - ShowHUDMessage
		// copies them into BSFixedStrings before this call returns, so nothing here has
		// to outlive it.
		[[nodiscard]] bool DeliverGuarded(const char* a_text) noexcept
		{
			__try {
				using ShowHUDMessageFn = void (*)(const char*, const char*, bool, bool);
				const auto fn = Fn<ShowHUDMessageFn>(kShowHUDMessageFn);
				// a_sound = nullptr, matching the real call site this session read
				// (AddCrimeTrackingFactionCrimeGold). param3 = true also matches that
				// site; param4 = false is this module's own conservative choice - see
				// the derivation comment on kShowHUDMessageFn above for why the hunt
				// left it unsettled.
				fn(a_text, nullptr, true, false);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Delivers at most one line per call, which at this throttle is the same thing as
		// "at most one line per second". A line that arrives inside the interval is left
		// exactly where it is - at the head of the ring - and delivered by the next drain
		// instead of being discarded: two lines popped in one pass used to lose the second
		// unconditionally, which cost four of nine lines in one accepted run and made the
		// HUD's last mode line disagree with the live mode. The one line a player must not
		// miss, "session terminal", lands about ninety frames behind "session suspended" -
		// a coin flip against a one-second throttle at 90 Hz, and a certainty at 120.
		//
		// Nothing here re-schedules itself: PollFrame below runs every frame from the
		// render thread and schedules the next drain the moment the interval has run out.
		void Drain() noexcept
		{
			// Cleared before the work, not after: an Enqueue that lands after this drain
			// has already decided the ring is empty must still see the flag false, or its
			// wakeup would be silently absorbed with nothing left to act on it. The worst
			// this costs is one redundant AddTask against an empty ring.
			g_drainPending.store(false, std::memory_order_release);

			const auto now = ::GetTickCount64();
			const auto last = g_lastDispatchTicks.load(std::memory_order_relaxed);
			if (last != 0 && now - last < kThrottleMs) {
				if (g_pending.load(std::memory_order_acquire) != 0) {
					g_deferredByThrottle.fetch_add(1, std::memory_order_relaxed);
				}
				return;
			}

			char text[kMessageBufferSize];
			if (!Pop(text)) {
				return;
			}
			g_lastDispatchTicks.store(now, std::memory_order_relaxed);
			PublishLastMessage(text);

			if (!DeliveryVerified()) {
				logger::info(FMT_STRING("hud message (log-only): {}"), text);
				g_dispatched.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (DeliverGuarded(text)) {
				g_dispatched.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_deliveryFaults.fetch_add(1, std::memory_order_relaxed);
				logger::warn(FMT_STRING("hud message: delivery faulted for \"{}\"; the line was dropped, nothing else changed"), text);
			}
		}

		// Wake exactly one drain per idle -> busy transition. Concurrent wake attempts
		// while a drain is already pending piggyback on it instead of each queuing their
		// own task.
		//
		// The CAS below only reserves the right to try scheduling a drain - it is not
		// itself proof that one was scheduled. "a drain is pending" and "a task is
		// actually queued" have to stay two different facts: if GetTaskInterface() or
		// AddTask itself comes back empty-handed, the reservation is released again (back
		// to false) rather than left standing. Leaving it standing on a failed attempt
		// would make every later attempt's own CAS fail forever - the flag reading true
		// with no task ever actually queued behind it - which is exactly the state that
		// once permanently disabled delivery for the rest of the session.
		void ScheduleDrain() noexcept
		{
			bool expected = false;
			if (!g_drainPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				return;
			}
			const auto* const tasks = F4SE::GetTaskInterface();
			bool scheduled = false;
			if (tasks) {
				try {
					tasks->AddTask([] { Drain(); });
					scheduled = true;
				} catch (...) {
					// A failed allocation inside AddTask leaves nothing queued - treated
					// exactly like a missing task interface below.
				}
			}
			if (!scheduled) {
				// Nothing was actually scheduled: release the reservation so the next
				// attempt's own CAS can succeed and try again, rather than believing a
				// drain is already in flight when none ever was. Whatever is queued stays
				// queued either way - only the wakeup attempt failed.
				g_drainPending.store(false, std::memory_order_release);

				// Narrow compound case this retry does not cover: if this happens on the
				// very first-ever Enqueue call, and the render-thread ClearColor hook that
				// drives PollFrame never installs either, nothing in this build ever calls
				// either entry point again to retry the drain - the line stays queued,
				// silently, for the rest of the session. Moving the self-check's HUD
				// verdict to fire only from SelfCheck::RunDeferred, after Hooks::Install()
				// has run, removed the one call site that used to hit the
				// missing-task-interface half of this on every launch; it is not otherwise
				// guarded against.
			}
		}

		// The other half of deferring rather than dropping: once the throttle interval has
		// run out and something is still waiting, somebody has to ask for a drain. This is
		// that somebody, called once per frame from PollFrame - lock-free unless it
		// actually schedules, so the render thread pays two atomic loads on a quiet frame.
		void ScheduleDrainIfDue() noexcept
		{
			if (g_pending.load(std::memory_order_acquire) == 0) {
				return;
			}
			const auto last = g_lastDispatchTicks.load(std::memory_order_relaxed);
			if (last != 0 && ::GetTickCount64() - last < kThrottleMs) {
				return;
			}
			ScheduleDrain();
		}

		void EnqueueLine(std::string_view a_text, Priority a_priority) noexcept
		{
			if (!Push(a_text, a_priority == Priority::kImportant)) {
				g_droppedByFull.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			g_queued.fetch_add(1, std::memory_order_relaxed);
			ScheduleDrain();
		}
	}

	void Enqueue(std::string_view a_text, Priority a_priority) noexcept
	{
		EnqueueLine(a_text, a_priority);
	}

	void PollFrame() noexcept
	{
		g_polls.fetch_add(1, std::memory_order_relaxed);

		// Before the edge detection below, not after: a line this poll enqueues is for the
		// next drain, and a line an earlier one deferred has been waiting longer.
		ScheduleDrainIfDue();

		// The live mode on this session's first admitted world frame: the one point
		// where "here is what is about to render" is worth a line on its own, distinct
		// from a /mode request (which gets its own message from RequestModeSwitch) -
		// this is specifically the moment the world gate itself opens for the first
		// time this session.
		static std::atomic<bool> s_worldAdmissionSeen{ false };
		if (ResetPolicy::Snapshot().worldAdmitted) {
			bool expected = false;
			if (s_worldAdmissionSeen.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				const auto mode = Configuration::SwitchSnapshot().liveMode;
				char line[kMessageBufferSize];
				const auto n = std::snprintf(line, sizeof(line), "BlurAway VR: %.*s active",
					static_cast<int>(mode.size()), mode.data());
				if (n > 0) {
					const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
					EnqueueLine(std::string_view(line, len), Priority::kNormal);
					g_worldAdmissionEdges.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		// Session recovery: Off/Preparing/Active are ordinary operation and stay silent;
		// Suspended and Terminal are the two transitions the product wants a line for.
		// Edge-detected against the last state this poll itself observed, so a session
		// that never enters either state never enqueues anything here.
		static std::atomic<std::uint32_t> s_lastSessionState{
			static_cast<std::uint32_t>(HDRRoundTrip::SessionState::kOff)
		};
		const auto isolation = HDRRoundTrip::Snapshot();
		const auto sessionState = static_cast<HDRRoundTrip::SessionState>(isolation.sessionState);
		const auto previous = static_cast<HDRRoundTrip::SessionState>(
			s_lastSessionState.exchange(static_cast<std::uint32_t>(sessionState), std::memory_order_acq_rel));
		if (sessionState != previous &&
			(sessionState == HDRRoundTrip::SessionState::kSuspended ||
				sessionState == HDRRoundTrip::SessionState::kTerminal)) {
			const auto name = HDRRoundTrip::SessionStateName(sessionState);
			// A machine that cannot run the backend at all gets the reason rather than the state.
			const bool unavailable = sessionState == HDRRoundTrip::SessionState::kTerminal &&
			                         static_cast<HDRRoundTrip::RecoveryRefusal>(isolation.recoveryRefusal) ==
			                             HDRRoundTrip::RecoveryRefusal::kBackendUnavailable;
			char line[kMessageBufferSize];
			const auto n = unavailable ?
			                   std::snprintf(line, sizeof(line), "BlurAway VR: unavailable on this PC - rendering natively (see the log)") :
			                   std::snprintf(line, sizeof(line), "BlurAway VR: session %.*s",
								   static_cast<int>(name.size()), name.data());
			if (n > 0) {
				const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
				// The two lines a player must not miss: the reconstruction stopping, and
				// whether it is coming back. Everything else can wait behind them.
				EnqueueLine(std::string_view(line, len), Priority::kImportant);
				g_sessionRecoveryEdges.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	State Snapshot() noexcept
	{
		State out{};
		out.queued = g_queued.load(std::memory_order_relaxed);
		out.dispatched = g_dispatched.load(std::memory_order_relaxed);
		out.droppedByThrottle = g_droppedByThrottle.load(std::memory_order_relaxed);
		out.deferredByThrottle = g_deferredByThrottle.load(std::memory_order_relaxed);
		out.pending = static_cast<std::uint64_t>(g_pending.load(std::memory_order_acquire));
		out.droppedByFull = g_droppedByFull.load(std::memory_order_relaxed);
		out.deliveryFaults = g_deliveryFaults.load(std::memory_order_relaxed);
		if (const auto message = g_lastMessage.load(std::memory_order_acquire); message) {
			out.lastMessage = *message;
		}
		out.deliveryReal = DeliveryVerified();
		out.polls = g_polls.load(std::memory_order_relaxed);
		out.worldAdmissionEdges = g_worldAdmissionEdges.load(std::memory_order_relaxed);
		out.sessionRecoveryEdges = g_sessionRecoveryEdges.load(std::memory_order_relaxed);
		return out;
	}
}
