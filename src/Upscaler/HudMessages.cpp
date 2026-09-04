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

		// Small and bounded on purpose: this is a rate of at most one delivered line per
		// second (see kThrottleMs), so a handful of slots is more than any burst of the
		// product's own messages - self-check, one mode change, one admission line, one
		// recovery line - can fill before the drain catches up.
		constexpr std::size_t kQueueCapacity = 8;

		constexpr std::uint64_t kThrottleMs = 1000;

		struct Slot
		{
			char text[kMessageBufferSize];
		};

		std::mutex g_ringMutex;
		Slot g_ring[kQueueCapacity]{};
		std::size_t g_head = 0;
		std::size_t g_tail = 0;
		std::size_t g_count = 0;

		std::atomic<bool> g_drainPending{ false };

		std::atomic<std::uint64_t> g_queued{ 0 };
		std::atomic<std::uint64_t> g_dispatched{ 0 };
		std::atomic<std::uint64_t> g_droppedByThrottle{ 0 };
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
		[[nodiscard]] bool Push(std::string_view a_text) noexcept
		{
			std::scoped_lock lock(g_ringMutex);
			if (g_count == kQueueCapacity) {
				return false;
			}
			const auto len = a_text.size() < (kMessageBufferSize - 1) ? a_text.size() : (kMessageBufferSize - 1);
			std::memcpy(g_ring[g_tail].text, a_text.data(), len);
			g_ring[g_tail].text[len] = '\0';
			g_tail = (g_tail + 1) % kQueueCapacity;
			++g_count;
			return true;
		}

		// Pops the oldest slot into a_out, a fixed local buffer the caller owns. Returns
		// false when the ring is empty.
		[[nodiscard]] bool Pop(char (&a_out)[kMessageBufferSize]) noexcept
		{
			std::scoped_lock lock(g_ringMutex);
			if (g_count == 0) {
				return false;
			}
			std::memcpy(a_out, g_ring[g_head].text, kMessageBufferSize);
			g_head = (g_head + 1) % kQueueCapacity;
			--g_count;
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

		void Drain() noexcept
		{
			// Cleared before the loop, not after: an Enqueue that lands after this drain
			// has already decided the ring is empty must still see the flag false, or its
			// wakeup would be silently absorbed with nothing left to act on it. The worst
			// this costs is one redundant AddTask against an empty ring.
			g_drainPending.store(false, std::memory_order_release);

			const bool verified = DeliveryVerified();

			char text[kMessageBufferSize];
			while (Pop(text)) {
				const auto now = ::GetTickCount64();
				const auto last = g_lastDispatchTicks.load(std::memory_order_relaxed);
				if (last != 0 && now - last < kThrottleMs) {
					g_droppedByThrottle.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				g_lastDispatchTicks.store(now, std::memory_order_relaxed);
				PublishLastMessage(text);

				if (!verified) {
					logger::info(FMT_STRING("hud message (log-only): {}"), text);
					g_dispatched.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				if (DeliverGuarded(text)) {
					g_dispatched.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_deliveryFaults.fetch_add(1, std::memory_order_relaxed);
					logger::warn(FMT_STRING("hud message: delivery faulted for \"{}\"; the line was dropped, nothing else changed"), text);
				}
			}
		}

		void EnqueueLine(std::string_view a_text) noexcept
		{
			if (!Push(a_text)) {
				g_droppedByFull.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			g_queued.fetch_add(1, std::memory_order_relaxed);

			// Wake exactly one drain per idle -> busy transition. Concurrent enqueues
			// while a drain is already pending piggyback on it instead of each queuing
			// their own task - Drain() above always empties the ring in one pass, so a
			// second task would only find nothing left to do.
			//
			// The CAS above only reserves the right to try scheduling a drain - it is
			// not itself proof that one was scheduled. "a drain is pending" and "a task
			// is actually queued" have to stay two different facts: if GetTaskInterface()
			// or AddTask itself comes back empty-handed below, the reservation is
			// released again (back to false) rather than left standing. Leaving it
			// standing on a failed attempt would make every later Enqueue's own CAS fail
			// forever - the flag reading true with no task ever actually queued behind
			// it - which is exactly the state that once permanently disabled delivery
			// for the rest of the session.
			bool expected = false;
			if (g_drainPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				const auto* const tasks = F4SE::GetTaskInterface();
				bool scheduled = false;
				if (tasks) {
					try {
						tasks->AddTask([] { Drain(); });
						scheduled = true;
					} catch (...) {
						// A failed allocation inside AddTask leaves nothing queued -
						// treated exactly like a missing task interface below.
					}
				}
				if (!scheduled) {
					// Nothing was actually scheduled: release the reservation so the
					// next Enqueue's own CAS can succeed and try again, rather than
					// believing a drain is already in flight when none ever was. The
					// line already pushed above stays queued either way - only the
					// wakeup attempt failed, not the enqueue.
					g_drainPending.store(false, std::memory_order_release);

					// Narrow compound case this retry does not cover: if this happens
					// on the very first-ever Enqueue call, and the render-thread
					// ClearColor hook that drives PollFrame's own edge-triggered
					// Enqueue calls never installs either, nothing in this build ever
					// calls Enqueue again to retry the drain - the line stays queued,
					// silently, for the rest of the session. Moving the self-check's
					// HUD verdict to fire only from SelfCheck::RunDeferred, after
					// Hooks::Install() has run, removed the one call site that used to
					// hit the missing-task-interface half of this on every launch; it
					// is not otherwise guarded against.
				}
			}
		}
	}

	void Enqueue(std::string_view a_text) noexcept
	{
		EnqueueLine(a_text);
	}

	void PollFrame() noexcept
	{
		g_polls.fetch_add(1, std::memory_order_relaxed);

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
				const auto n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: %.*s active",
					static_cast<int>(mode.size()), mode.data());
				if (n > 0) {
					const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
					EnqueueLine(std::string_view(line, len));
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
		const auto sessionState = static_cast<HDRRoundTrip::SessionState>(HDRRoundTrip::Snapshot().sessionState);
		const auto previous = static_cast<HDRRoundTrip::SessionState>(
			s_lastSessionState.exchange(static_cast<std::uint32_t>(sessionState), std::memory_order_acq_rel));
		if (sessionState != previous &&
			(sessionState == HDRRoundTrip::SessionState::kSuspended ||
				sessionState == HDRRoundTrip::SessionState::kTerminal)) {
			const auto name = HDRRoundTrip::SessionStateName(sessionState);
			char line[kMessageBufferSize];
			const auto n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: session %.*s",
				static_cast<int>(name.size()), name.data());
			if (n > 0) {
				const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
				EnqueueLine(std::string_view(line, len));
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
