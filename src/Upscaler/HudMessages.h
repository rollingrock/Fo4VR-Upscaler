#pragma once

// Bounded pipeline from the render thread and the settings thread to a single line of
// player-facing HUD text, delivered on the game thread. Nothing here is a source of
// facts - every call site hands this module a line it already knows to be true, built
// from a snapshot another module already publishes.
//
// Enqueue copies into a fixed ring of fixed-size buffers under a short-held mutex, so
// neither producer thread ever allocates to report a line. Now that PollFrame is wired
// into the render thread (see below), the render thread itself takes that same mutex
// whenever PollFrame's own edge detection enqueues a line - accepted the same way
// TAAReplacement's g_configLock and StereoCapture's g_captureLock already are in this
// tree: a short, uncontended, allocation-free critical section is an ordinary
// render-thread cost, not a stall risk. Once something is queued, Enqueue itself
// arranges the one game-thread hop (F4SE::GetTaskInterface()->AddTask) that drains the
// ring and, throttled to one line per second, calls the delivery point. No render-thread
// or settings-thread caller has to know that hop exists.
//
// The throttle defers, it does not drop: one line is delivered per drain, and anything
// that arrives inside the interval waits its turn in the ring until PollFrame - running
// every frame - schedules the next drain. A dropping throttle lost the second of any two
// lines in one pass and any line inside a second of the last, which in practice meant a
// HUD reading "off" while the live mode was "quality", and the "session terminal" line
// landing about ninety frames behind "session suspended" surviving on a coin flip at
// 90 Hz and never at 120. Priority::kImportant jumps the queue for the two lines a player
// must not miss - the session-recovery transitions and the startup self-check's verdict -
// without discarding what it overtook.
//
// PollFrame detects the two facts that are true only for an instant - the live mode on
// this session's first admitted world frame, and a session-recovery transition into
// Suspended or Terminal - by reading ResetPolicy::Snapshot() and HDRRoundTrip::Snapshot(),
// both already safe from any thread. It runs once per frame from the render thread, from
// the verified primary ClearColor seam in Hooks.cpp - the same per-frame hook
// HDRRoundTrip::OnPrimaryRenderThread already uses, so this runs every frame regardless
// of live mode or menu state. DevBench's /hudmsg route can still invoke it directly on
// demand, which reads the same genuine current state, just not on that schedule.

#include <cstdint>
#include <string>
#include <string_view>

namespace Upscaler::HudMessages
{
	// Where a line sits in the queue when more than one is waiting. kImportant is for the
	// lines whose loss changes what a player believes about the session - the
	// session-recovery transitions and the startup self-check's verdict - and nothing else:
	// marking every line important is the same as marking none.
	enum class Priority : std::uint32_t
	{
		kNormal = 0,
		kImportant = 1,
	};

	// Copies a_text into the ring, truncated to fit a HUD line, and wakes the game-thread
	// drain if it is not already pending. Safe from any thread; never allocates on the
	// caller's own stack beyond a fixed local buffer. A full ring fails soft - the line
	// is dropped and counted (droppedByFull), nothing is disabled. A missing F4SE task
	// interface (or a failed AddTask) does not drop the line at all: it stays queued in
	// the ring, and the drain-pending reservation is released rather than left standing,
	// so the very next Enqueue or PollFrame is what actually schedules delivery.
	void Enqueue(std::string_view a_text, Priority a_priority = Priority::kNormal) noexcept;

	// See the file comment: runs once per frame from the render thread, wired at the
	// same ClearColor seam Hotkeys::PollFrame documents. Safe to call from any thread
	// that can afford two lock-free Snapshot() calls - and, on the frame it detects an
	// edge, the same short-held ring mutex Enqueue takes, per the file comment above.
	// DevBench's /hudmsg route calls it directly too, on demand rather than once per
	// frame. It is also what releases a deferred line: every call schedules a drain when
	// the throttle interval has run out and something is still waiting, so a build where
	// this is never called delivers the first line of a burst and holds the rest.
	void PollFrame() noexcept;

	struct State
	{
		std::uint64_t queued = 0;
		std::uint64_t dispatched = 0;
		// Lines the throttle discarded. Zero for the life of this build - the throttle
		// defers now - and kept so a run that finds it non-zero has caught a regression
		// rather than an unexplained gap between queued and dispatched.
		std::uint64_t droppedByThrottle = 0;
		// Drains that found the interval had not run out and left the queue standing. A
		// rate, not a failure: it counts how often delivery was postponed, never how much.
		std::uint64_t deferredByThrottle = 0;
		// Lines waiting right now.
		std::uint64_t pending = 0;
		std::uint64_t droppedByFull = 0;
		std::uint64_t deliveryFaults = 0;
		std::string lastMessage{ "none" };
		// True once this session has byte-verified ShowHUDMessage's prologue and is
		// therefore actually calling it; false means every drained line only logs.
		bool deliveryReal = false;
		std::uint64_t polls = 0;
		std::uint64_t worldAdmissionEdges = 0;
		std::uint64_t sessionRecoveryEdges = 0;
	};

	[[nodiscard]] State Snapshot() noexcept;
}
