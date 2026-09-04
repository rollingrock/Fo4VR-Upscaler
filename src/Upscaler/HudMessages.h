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
	// Copies a_text into the ring, truncated to fit a HUD line, and wakes the game-thread
	// drain if it is not already pending. Safe from any thread; never allocates on the
	// caller's own stack beyond a fixed local buffer. A full ring fails soft - the line
	// is dropped and counted (droppedByFull), nothing is disabled. A missing F4SE task
	// interface (or a failed AddTask) does not drop the line at all: it stays queued in
	// the ring, and the drain-pending reservation is released rather than left standing,
	// so the very next Enqueue's own attempt is what actually schedules delivery.
	void Enqueue(std::string_view a_text) noexcept;

	// See the file comment: runs once per frame from the render thread, wired at the
	// same ClearColor seam Hotkeys::PollFrame documents. Safe to call from any thread
	// that can afford two lock-free Snapshot() calls - and, on the frame it detects an
	// edge, the same short-held ring mutex Enqueue takes, per the file comment above.
	// DevBench's /hudmsg route calls it directly too, on demand rather than once per
	// frame.
	void PollFrame() noexcept;

	struct State
	{
		std::uint64_t queued = 0;
		std::uint64_t dispatched = 0;
		std::uint64_t droppedByThrottle = 0;
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
