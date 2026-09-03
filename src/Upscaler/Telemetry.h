#pragma once

// Non-blocking CPU/GPU timing for the accepted reduced-NGX quality transaction.
// Configure and RequestReset publish atomics only. Every D3D11 object and immediate-
// context call stays on the verified render thread, while Report reads atomics only.
//
// One episode is one accepted transaction, Begin to End, and every MarkPoint is hit
// exactly once in enumerator order or the episode is aborted. A point sits after each
// GPU-visible unit the transaction issues, so the interval between consecutive points
// prices that unit alone: the two context-state swaps around each isolated scope, the
// per-eye colour and motion extracts (the motion one includes the sanitizer draw), the
// two NGX evaluates and reassembly copies, the private-to-engine copy, the staged and
// published TAA draws, and the engine's own work in the two gaps between our scopes.
// kAfterRtBackups is kept where the RT1/RT3 backups used to be issued, with nothing
// inside it, so the report keeps its shape and prices the removed copies at zero. The
// reported interval names live in Telemetry.cpp and derive from this order; "total" is
// Begin to kAfterPublish.

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
		kBeforeStagedTaaIsolationBegin = 18,
		kAfterStagedTaaIsolationBegin = 19,
		kBeforeStagedTaaIsolationEnd = 20,
		kAfterStagedTaa = 21,
		kBeforePublish = 22,
		kAfterPublishIsolationBegin = 23,
		kBeforePublishIsolationEnd = 24,
		kAfterPublish = 25,
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
	void Begin() noexcept;
	void Mark(MarkPoint a_point) noexcept;
	void End() noexcept;
	void Abort() noexcept;
	void Collect() noexcept;

	// Listener-safe generation-coherent atomic report. It never touches
	// device/context/query state.
	[[nodiscard]] std::string Report();
}
