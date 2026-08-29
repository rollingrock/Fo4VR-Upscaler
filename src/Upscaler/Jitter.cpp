#include "Upscaler/Jitter.h"

#include "Upscaler/Common.h"

namespace Upscaler::Jitter
{
	namespace
	{
		LogGate g_readGate;
		LogGate g_writeGate;
	}

	bool ReadEngine(float&, float&) noexcept
	{
		if (g_readGate.Ready()) {
			logger::warn("jitter read unavailable: engine handles are unresolved. See docs/MODULE_MAP.md."sv);
		}
		return false;
	}

	bool WriteEngine(float, float) noexcept
	{
		if (g_writeGate.Ready()) {
			logger::warn("jitter writes are refused until the pixel divisor is measured. See docs/MODULE_MAP.md."sv);
		}
		return false;
	}

	std::uint32_t PhaseCount() noexcept
	{
		return 0;
	}
}
