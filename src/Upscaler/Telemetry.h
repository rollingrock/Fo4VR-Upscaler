#pragma once

// Non-blocking CPU/GPU timing for the accepted reduced-NGX quality transaction.
// Configure and RequestReset publish atomics only. Every D3D11 object and immediate-
// context call stays on the verified render thread, while Report reads atomics only.

#include <cstdint>
#include <string>

namespace Upscaler::Telemetry
{
	enum class MarkPoint : std::uint32_t
	{
		kAfterVendorEvaluateReassembly = 1,
		kAfterRtBackupCopy = 2,
		kAfterStagedTaa = 3,
		kBeforePublish = 4,
		kAfterPublish = 5,
	};

	// Safe on a settings/listener thread. A false -> true transition starts a fresh
	// measurement window when the render thread next observes it.
	void Configure(bool a_enabled) noexcept;

	// Listener-safe clean-window request. The render thread completes it only when no
	// quality episode or unread query is outstanding, so samples cannot cross windows.
	[[nodiscard]] std::uint64_t RequestReset() noexcept;

	// Render-thread only. Begin is immediately before vendor work. Query commands do not
	// Flush, and all GetData calls use DONOTFLUSH.
	void Begin() noexcept;
	void Mark(MarkPoint a_point) noexcept;
	void End() noexcept;
	void Abort() noexcept;
	void Collect() noexcept;

	// Listener-safe generation-coherent atomic report. It never touches
	// device/context/query state.
	[[nodiscard]] std::string Report();
}
