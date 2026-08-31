#pragma once

// Keeps reconstruction out of the engine's nested scope-world render. The engine-owned
// renderer byte is a per-pass pulse, not a persistent scope-raised state, so this module
// deliberately does not guess a scope episode edge for history reset.

#include <cstdint>

namespace Upscaler::ScopePolicy
{
	struct State
	{
		bool suppressDuringScopeRender = true;
		std::uint64_t rendererReadAttempts = 0;
		std::uint64_t rendererReadSuccesses = 0;
		std::uint64_t rendererReadFailures = 0;
		std::uint64_t scopedDispatches = 0;
		std::uint64_t suppressedDispatches = 0;
	};

	// Atomic and safe from the settings/reload thread.
	void Configure(bool a_suppressDuringScopeRender) noexcept;

	// Render-thread only, at the top of the pre-HDR callback. True means the caller
	// must leave this nested scope pass entirely to its owner.
	[[nodiscard]] bool ShouldSuppressCurrentDispatch() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
