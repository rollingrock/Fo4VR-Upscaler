#pragma once

// Non-blocking CPU/GPU timing for accepted reduced-NGX and native-DLAA frames.
// Configure and RequestReset publish atomics only. Every D3D11 object and immediate-
// context call stays on the verified render thread, while Report reads atomics only.
//
// One episode is one accepted transaction, Begin to End, and every MarkPoint is hit
// exactly once in enumerator order or the episode is aborted. A point sits after each
// GPU-visible unit the transaction issues, so the interval between consecutive points
// prices that unit alone: the two context-state swaps around each isolated scope, the
// per-eye colour and motion extracts (the motion one includes the sanitizer draw), the
// two NGX evaluates and reassembly copies, the private-to-engine copy, the one publish
// draw at the TemporalAA seam, and the engine's own work in the two gaps between our
// scopes. kAfterRtBackups is kept where the RT1/RT3 backups used to be issued, with
// nothing inside it, so the report keeps its shape and prices the removed copies at
// zero. The reported interval names live in Telemetry.cpp and derive from this order;
// "total" is Begin to kAfterLateRangeCommit, the point the late ImageSpace range
// commits the transaction. Native DLAA ends at the completed TemporalAA replacement,
// with a separate window and an explicitly named nativeSeamClose interval.

#include <cstdint>
#include <string>

namespace Upscaler::Telemetry
{
	enum class MarkPoint : std::uint32_t
	{
		kNone = 0,  // Mark ignores it, so a shared isolation helper can skip a bracket
		kAfterEvaluateIsolationBegin = 1,
		kAfterLeftColorExtract = 2,
		kAfterLeftMotionExtract = 3,
		kAfterLeftEvaluate = 4,
		kAfterLeftReassembly = 5,
		kAfterRightColorExtract = 6,
		kAfterRightMotionExtract = 7,
		kAfterRightEvaluate = 8,
		kAfterRightReassembly = 9,
		kBeforeEvaluateIsolationEnd = 10,
		kAfterEvaluateIsolationEnd = 11,
		kAfterBackupIsolationBegin = 12,
		kAfterBackupClearState = 13,
		kAfterRtBackups = 14,
		kAfterPrivateToEngineCopy = 15,
		kBeforeBackupIsolationEnd = 16,
		kAfterBackupIsolationEnd = 17,
		kBeforeSeamPublishIsolationBegin = 18,
		kAfterSeamPublishIsolationBegin = 19,
		kBeforeSeamPublishIsolationEnd = 20,
		kAfterSeamPublish = 21,
		kAfterLateRangeCommit = 22,
	};

	// The three points a caller hands to a shared context-state isolation helper so the
	// swap in, the guarded work and the swap out each land in their own interval. kNone
	// skips a bracket. Plain data, so it may cross an MSVC __try boundary.
	struct IsolationMarks
	{
		MarkPoint beforeBegin = MarkPoint::kNone;
		MarkPoint afterBegin = MarkPoint::kNone;
		MarkPoint beforeEnd = MarkPoint::kNone;
	};

	// Safe on a settings/listener thread. A false -> true transition starts a fresh
	// measurement window when the render thread next observes it.
	void Configure(bool a_enabled) noexcept;

	// Listener-safe clean-window request. The render thread completes it only when no
	// quality episode or unread query is outstanding, so samples cannot cross windows.
	[[nodiscard]] std::uint64_t RequestReset() noexcept;

	// Render-thread only. Begin is immediately before vendor work. Query commands do not
	// Flush, and all GetData calls use DONOTFLUSH. Mark is a no-op unless an episode is
	// active, so a call on a path that never Begins costs two atomic loads.
	enum class Path : std::uint32_t
	{
		kReduced,
		kNative,
	};
	void Begin(Path a_path = Path::kReduced) noexcept;

	// Render-thread only. The HDR publisher arms its exact serial; only that native
	// handback may close the episode at TemporalAA. AbortNative retires a missed seam
	// before the next main HDR dispatch, without touching a reduced episode.
	void ExpectNativeSeam(std::uint64_t a_serial) noexcept;
	[[nodiscard]] bool NativePending(std::uint64_t a_serial) noexcept;
	void EndNative(std::uint64_t a_serial) noexcept;
	void AbortNative() noexcept;
	void Mark(MarkPoint a_point) noexcept;
	void End() noexcept;
	void Abort() noexcept;
	void Collect() noexcept;

	// Render-thread only, outside an episode. Six log reports per window at 10 s intervals.
	void LogReport() noexcept;

	// Listener-safe generation-coherent atomic report. It never touches
	// device/context/query state.
	[[nodiscard]] std::string Report();

	// Whether perfTimers is on. This module's own fact, not a copy of the setting: /perf
	// reports the same bit as "requestedEnabled", so a caller that reads this and a
	// caller that reads /perf cannot disagree about it. It exists so a caller that only
	// wants a report while the timers are running can ask before paying for one - Report
	// and IntervalSummary each build a string on every call. Safe from any thread.
	[[nodiscard]] bool Enabled() noexcept;

	// The same per-interval numbers Report puts under "stages", formatted for a
	// fixed-width display instead of for a parser: one line per interval that has CPU
	// samples, naming it and its mean CPU and GPU cost in milliseconds. The panel draws
	// this. Nothing is derived here that /perf does not already report, and the
	// formatting lives in the module that owns the numbers rather than in the one that
	// draws them. Same thread safety as Report.
	[[nodiscard]] std::string IntervalSummary();
}
