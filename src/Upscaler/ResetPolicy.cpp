#include "Upscaler/ResetPolicy.h"

#include <atomic>

namespace Upscaler::ResetPolicy
{
	namespace
	{
		// Requested on the game thread, consumed on the render thread.
		std::atomic_bool g_pending{ false };
	}

	void RequestReset(std::string_view a_reason)
	{
		// Logged on the rising edge only. The triggers are per-frame edges, and a line
		// per call would be a line per frame the moment two of them overlap.
		if (!g_pending.exchange(true, std::memory_order_relaxed)) {
			logger::info(FMT_STRING("history reset requested: {}"), a_reason);
		}
	}

	bool ConsumeReset() noexcept
	{
		return g_pending.exchange(false, std::memory_order_relaxed);
	}
}
