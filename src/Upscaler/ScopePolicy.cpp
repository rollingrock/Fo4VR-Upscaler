#include "Upscaler/ScopePolicy.h"

#include "Upscaler/Engine.h"

#include <atomic>

namespace Upscaler::ScopePolicy
{
	namespace
	{
		std::atomic_bool g_suppressDuringScopeRender{ true };
		std::atomic<std::uint64_t> g_rendererReadAttempts{ 0 };
		std::atomic<std::uint64_t> g_rendererReadSuccesses{ 0 };
		std::atomic<std::uint64_t> g_rendererReadFailures{ 0 };
		std::atomic<std::uint64_t> g_scopedDispatches{ 0 };
		std::atomic<std::uint64_t> g_suppressedDispatches{ 0 };
	}

	void Configure(bool a_suppressDuringScopeRender) noexcept
	{
		g_suppressDuringScopeRender.store(a_suppressDuringScopeRender, std::memory_order_release);
	}

	bool ShouldSuppressCurrentDispatch() noexcept
	{
		g_rendererReadAttempts.fetch_add(1, std::memory_order_relaxed);
		bool scoped = false;
		if (!Engine::ScopedPass(scoped)) {
			g_rendererReadFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		g_rendererReadSuccesses.fetch_add(1, std::memory_order_relaxed);
		if (!scoped) {
			return false;
		}

		g_scopedDispatches.fetch_add(1, std::memory_order_relaxed);
		if (!g_suppressDuringScopeRender.load(std::memory_order_acquire)) {
			return false;
		}
		const auto suppressed = g_suppressedDispatches.fetch_add(1, std::memory_order_relaxed) + 1;
		if (suppressed == 1) {
			logger::info("scope policy: suppressed the first engine-marked nested scope HDR dispatch"sv);
		}
		return true;
	}

	State Snapshot() noexcept
	{
		return {
			g_suppressDuringScopeRender.load(std::memory_order_acquire),
			g_rendererReadAttempts.load(std::memory_order_relaxed),
			g_rendererReadSuccesses.load(std::memory_order_relaxed),
			g_rendererReadFailures.load(std::memory_order_relaxed),
			g_scopedDispatches.load(std::memory_order_relaxed),
			g_suppressedDispatches.load(std::memory_order_relaxed),
		};
	}
}
