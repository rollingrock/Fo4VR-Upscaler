#include "Upscaler/CameraMotion.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"

#include <atomic>

namespace Upscaler::CameraMotion
{
	namespace
	{
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_successes{ 0 };
		std::atomic<std::uint64_t> g_failures{ 0 };
		std::atomic<std::uintptr_t> g_staging{ 0 };
		std::atomic<Failure> g_lastFailure{ Failure::kNone };

		[[nodiscard]] bool Fail(Failure a_failure) noexcept
		{
			g_lastFailure.store(a_failure, std::memory_order_relaxed);
			g_failures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	bool SnapshotEye(
		std::uint32_t a_eye,
		MotionReprojection::Data& a_out) noexcept
	{
		a_out = {};
		g_attempts.fetch_add(1, std::memory_order_relaxed);
		if (a_eye >= 2) {
			return Fail(Failure::kInvalidEye);
		}

		const auto state = Engine::GraphicsState();
		if (state == 0) {
			return Fail(Failure::kStateUnavailable);
		}

		void* stagingRaw = nullptr;
		if (!Engine::ReadPtr(state + Addr::kStateCameraViewData, stagingRaw) || !stagingRaw) {
			return Fail(Failure::kStagingUnavailable);
		}
		const auto staging = reinterpret_cast<std::uintptr_t>(stagingRaw);
		g_staging.store(staging, std::memory_order_relaxed);
		const auto eyeBase = staging + static_cast<std::uintptr_t>(a_eye) * Addr::kViewDataStride;

		float current[16]{};
		float previous[16]{};
		if (!FaultGuard::SafeReadBytes(
				reinterpret_cast<const void*>(eyeBase + Addr::kViewDataCurrentUnjittered),
				current,
				sizeof(current)) ||
			!FaultGuard::SafeReadBytes(
				reinterpret_cast<const void*>(eyeBase + Addr::kViewDataPreviousUnjittered),
				previous,
				sizeof(previous))) {
			return Fail(Failure::kReadFault);
		}

		void* stagingAfterRaw = nullptr;
		if (!Engine::ReadPtr(state + Addr::kStateCameraViewData, stagingAfterRaw) ||
			reinterpret_cast<std::uintptr_t>(stagingAfterRaw) != staging) {
			return Fail(Failure::kStagingChanged);
		}
		if (!MotionReprojection::Build(current, previous, a_out)) {
			return Fail(Failure::kInvalidMatrix);
		}

		g_lastFailure.store(Failure::kNone, std::memory_order_relaxed);
		g_successes.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	State Snapshot() noexcept
	{
		return {
			g_attempts.load(std::memory_order_relaxed),
			g_successes.load(std::memory_order_relaxed),
			g_failures.load(std::memory_order_relaxed),
			g_staging.load(std::memory_order_relaxed),
			g_lastFailure.load(std::memory_order_relaxed),
		};
	}
}
