#include "Upscaler/Jitter.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Upscaler::Jitter
{
	namespace
	{
		constexpr std::uint32_t kPhaseCount = 8;
		constexpr float kSequenceTolerance = 0.00025f;
		constexpr float kBoundsTolerance = 0.00001f;

		std::atomic<std::uintptr_t> g_state{ 0 };
		std::atomic<std::uint64_t> g_readAttempts{ 0 };
		std::atomic<std::uint64_t> g_readSuccesses{ 0 };
		std::atomic<std::uint64_t> g_readFailures{ 0 };
		std::atomic<std::uint64_t> g_leftSubmissions{ 0 };
		std::atomic<std::uint64_t> g_rightSubmissions{ 0 };
		std::atomic<std::uint64_t> g_stereoMatches{ 0 };
		std::atomic<std::uint64_t> g_stereoMismatches{ 0 };
		std::atomic<std::uint32_t> g_phase{ 0 };
		std::atomic<std::uint32_t> g_observedPhaseMask{ 0 };
		std::atomic<std::uint32_t> g_engineWidth{ 0 };
		std::atomic<std::uint32_t> g_engineHeight{ 0 };
		std::atomic<std::uint32_t> g_enabled{ 0 };
		std::atomic<std::uint32_t> g_disableCount{ 0 };
		std::atomic<std::uint32_t> g_projectionXBits{ 0 };
		std::atomic<std::uint32_t> g_projectionYBits{ 0 };
		std::atomic<std::uint32_t> g_jitterXBits{ 0 };
		std::atomic<std::uint32_t> g_jitterYBits{ 0 };
		std::atomic<std::uint32_t> g_leftJitterXBits{ 0 };
		std::atomic<std::uint32_t> g_leftJitterYBits{ 0 };
		std::atomic<std::uint32_t> g_rightJitterXBits{ 0 };
		std::atomic<std::uint32_t> g_rightJitterYBits{ 0 };
		std::atomic<Failure> g_lastFailure{ Failure::kNone };

		LogGate g_readGate;
		LogGate g_writeGate;

		[[nodiscard]] float RadicalInverse(std::uint32_t a_base, std::uint32_t a_index) noexcept
		{
			const float inverseBase = 1.0f / static_cast<float>(a_base);
			float inversePower = inverseBase;
			float result = 0.0f;
			while (a_index != 0) {
				result += static_cast<float>(a_index % a_base) * inversePower;
				a_index /= a_base;
				inversePower *= inverseBase;
			}
			return result;
		}

		[[nodiscard]] bool Fail(Failure a_failure, std::string_view a_why) noexcept
		{
			g_lastFailure.store(a_failure, std::memory_order_relaxed);
			g_readFailures.fetch_add(1, std::memory_order_relaxed);
			if (g_readGate.Ready()) {
				logger::warn(FMT_STRING("jitter read declined: {}"), a_why);
			}
			return false;
		}

		void StoreFloat(std::atomic<std::uint32_t>& a_target, float a_value) noexcept
		{
			a_target.store(std::bit_cast<std::uint32_t>(a_value), std::memory_order_relaxed);
		}

		[[nodiscard]] float LoadFloat(const std::atomic<std::uint32_t>& a_source) noexcept
		{
			return std::bit_cast<float>(a_source.load(std::memory_order_relaxed));
		}
	}

	bool Init()
	{
		if (g_state.load(std::memory_order_acquire) != 0) {
			return true;
		}

		const auto state = Engine::RipResolve(
			Addr::kJitterStateAnchor,
			{ 0x48, 0x8D, 0x0D },
			Addr::kRipDispOffset,
			Addr::kRipInstrLen,
			"native TAA jitter state"sv);
		g_state.store(state, std::memory_order_release);
		if (state == 0) {
			logger::error("jitter: native state unresolved; every temporal submission will fail closed"sv);
			return false;
		}

		logger::info(FMT_STRING("jitter: native state resolved at rva {:X}; 8-phase projection-to-camera conversion is armed"),
			state - REL::Module::get().base());
		return true;
	}

	bool ReadEngine(
		std::uint32_t a_expectedWidth,
		std::uint32_t a_expectedHeight,
		float& a_outX,
		float& a_outY) noexcept
	{
		g_readAttempts.fetch_add(1, std::memory_order_relaxed);
		const auto state = g_state.load(std::memory_order_acquire);
		if (state == 0) {
			return Fail(Failure::kUnresolved, "native state is unresolved"sv);
		}

		std::uint32_t phaseBefore = 0;
		std::uint32_t projectionXBits = 0;
		std::uint32_t projectionYBits = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t enabled = 0;
		std::uint32_t disableCount = 0;
		std::uint32_t phaseAfter = 0;
		if (!Engine::ReadU32(state + Addr::kJitterPhase, phaseBefore) ||
			!Engine::ReadU32(state + Addr::kJitterProjectionX, projectionXBits) ||
			!Engine::ReadU32(state + Addr::kJitterProjectionY, projectionYBits) ||
			!Engine::ReadU32(state + Addr::kJitterPerEyeWidth, width) ||
			!Engine::ReadU32(state + Addr::kJitterPerEyeHeight, height) ||
			!Engine::ReadU32(state + Addr::kJitterEnabled, enabled) ||
			!Engine::ReadU32(state + Addr::kJitterDisableCount, disableCount) ||
			!Engine::ReadU32(state + Addr::kJitterPhase, phaseAfter)) {
			return Fail(Failure::kReadFault, "one or more guarded state reads failed"sv);
		}

		g_phase.store(phaseAfter, std::memory_order_relaxed);
		g_engineWidth.store(width, std::memory_order_relaxed);
		g_engineHeight.store(height, std::memory_order_relaxed);
		g_enabled.store(enabled, std::memory_order_relaxed);
		g_disableCount.store(disableCount, std::memory_order_relaxed);
		g_projectionXBits.store(projectionXBits, std::memory_order_relaxed);
		g_projectionYBits.store(projectionYBits, std::memory_order_relaxed);

		if (phaseBefore != phaseAfter || phaseAfter >= kPhaseCount) {
			return Fail(Failure::kTornRead, "phase changed during the snapshot or was outside 0..7"sv);
		}
		if (a_expectedWidth == 0 || a_expectedHeight == 0 ||
			width != a_expectedWidth || height != a_expectedHeight) {
			return Fail(Failure::kDimensionMismatch, "engine dimensions do not match the staged per-eye extent"sv);
		}
		if (enabled == 0 || disableCount != 0) {
			return Fail(Failure::kDisabled, "native jitter enable gates are closed"sv);
		}

		const float projectionX = std::bit_cast<float>(projectionXBits);
		const float projectionY = std::bit_cast<float>(projectionYBits);
		const float jitterX = ProjectionToCameraPixelsX(projectionX, width);
		const float jitterY = ProjectionToCameraPixelsY(projectionY, height);
		if (!std::isfinite(projectionX) || !std::isfinite(projectionY) ||
			!std::isfinite(jitterX) || !std::isfinite(jitterY) ||
			std::abs(jitterX) > 0.5f + kBoundsTolerance ||
			std::abs(jitterY) > 0.5f + kBoundsTolerance) {
			return Fail(Failure::kInvalidValue, "projection or converted pixel jitter is not finite and bounded"sv);
		}

		const std::uint32_t index = phaseAfter + 1;
		const float expectedX = 0.5f - RadicalInverse(2, index);
		const float expectedY = RadicalInverse(3, index) - 0.5f;
		if (std::abs(jitterX - expectedX) > kSequenceTolerance ||
			std::abs(jitterY - expectedY) > kSequenceTolerance) {
			return Fail(Failure::kSequenceMismatch, "converted sample does not match the native Halton phase"sv);
		}

		StoreFloat(g_jitterXBits, jitterX);
		StoreFloat(g_jitterYBits, jitterY);
		g_observedPhaseMask.fetch_or(1u << phaseAfter, std::memory_order_relaxed);
		g_lastFailure.store(Failure::kNone, std::memory_order_relaxed);
		g_readSuccesses.fetch_add(1, std::memory_order_relaxed);
		a_outX = jitterX;
		a_outY = jitterY;
		return true;
	}

	bool WriteEngine(float, float) noexcept
	{
		if (g_writeGate.Ready()) {
			logger::warn("jitter writes are refused: native readback is proven, but backend-owned reconstruction sequencing is not armed"sv);
		}
		return false;
	}

	std::uint32_t PhaseCount() noexcept
	{
		return kPhaseCount;
	}

	void RecordSubmission(std::uint32_t a_eye, float a_x, float a_y) noexcept
	{
		const auto x = std::bit_cast<std::uint32_t>(a_x);
		const auto y = std::bit_cast<std::uint32_t>(a_y);
		if (a_eye == 0) {
			g_leftJitterXBits.store(x, std::memory_order_relaxed);
			g_leftJitterYBits.store(y, std::memory_order_relaxed);
			g_leftSubmissions.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (a_eye != 1) {
			g_stereoMismatches.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		g_rightJitterXBits.store(x, std::memory_order_relaxed);
		g_rightJitterYBits.store(y, std::memory_order_relaxed);
		g_rightSubmissions.fetch_add(1, std::memory_order_relaxed);
		if (x == g_leftJitterXBits.load(std::memory_order_relaxed) &&
			y == g_leftJitterYBits.load(std::memory_order_relaxed)) {
			g_stereoMatches.fetch_add(1, std::memory_order_relaxed);
		} else {
			g_stereoMismatches.fetch_add(1, std::memory_order_relaxed);
		}
	}

	State Snapshot() noexcept
	{
		const auto state = g_state.load(std::memory_order_acquire);
		return {
			state != 0,
			state,
			g_readAttempts.load(std::memory_order_relaxed),
			g_readSuccesses.load(std::memory_order_relaxed),
			g_readFailures.load(std::memory_order_relaxed),
			g_leftSubmissions.load(std::memory_order_relaxed),
			g_rightSubmissions.load(std::memory_order_relaxed),
			g_stereoMatches.load(std::memory_order_relaxed),
			g_stereoMismatches.load(std::memory_order_relaxed),
			kPhaseCount,
			g_phase.load(std::memory_order_relaxed),
			g_observedPhaseMask.load(std::memory_order_relaxed),
			g_engineWidth.load(std::memory_order_relaxed),
			g_engineHeight.load(std::memory_order_relaxed),
			g_enabled.load(std::memory_order_relaxed),
			g_disableCount.load(std::memory_order_relaxed),
			LoadFloat(g_projectionXBits),
			LoadFloat(g_projectionYBits),
			LoadFloat(g_jitterXBits),
			LoadFloat(g_jitterYBits),
			LoadFloat(g_leftJitterXBits),
			LoadFloat(g_leftJitterYBits),
			LoadFloat(g_rightJitterXBits),
			LoadFloat(g_rightJitterYBits),
			g_lastFailure.load(std::memory_order_relaxed),
		};
	}
}
