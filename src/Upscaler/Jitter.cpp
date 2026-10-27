#include "Upscaler/Jitter.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Backend/IBackend.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"

#include <Windows.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Upscaler::Jitter
{
	namespace
	{
		// The engine's own sequence length, proven by the AND 7 at 1daa1e7 - the only
		// appearance of a sequence length anywhere in the image.
		constexpr std::uint32_t kNativePhaseCount = 8;
		constexpr float kSequenceTolerance = 0.00025f;
		constexpr float kBoundsTolerance = 0.00001f;

		std::atomic<std::uintptr_t> g_state{ 0 };
		std::atomic<std::uint64_t> g_readAttempts{ 0 };
		std::atomic<std::uint64_t> g_readSuccesses{ 0 };
		std::atomic<std::uint64_t> g_readFailures{ 0 };
		std::atomic<std::uint64_t> g_writeAttempts{ 0 };
		std::atomic<std::uint64_t> g_writeSuccesses{ 0 };
		std::atomic<std::uint64_t> g_writeMismatches{ 0 };
		std::atomic<std::uint64_t> g_writeGateDeclines{ 0 };
		std::atomic<std::uint64_t> g_sequenceRestarts{ 0 };
		std::atomic<std::uint64_t> g_leftSubmissions{ 0 };
		std::atomic<std::uint64_t> g_rightSubmissions{ 0 };
		std::atomic<std::uint64_t> g_stereoMatches{ 0 };
		std::atomic<std::uint64_t> g_stereoMismatches{ 0 };
		std::atomic<std::uint32_t> g_phase{ 0 };
		std::atomic<std::uint32_t> g_enginePhase{ 0 };
		std::atomic<std::uint64_t> g_observedPhaseMaskLow{ 0 };
		std::atomic<std::uint64_t> g_observedPhaseMaskHigh{ 0 };
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
		std::atomic<WriteFailure> g_lastWriteFailure{ WriteFailure::kNone };

		// Applied only to the pixels handed to the vendor, never to the engine commit or
		// the telemetry, so the two signs rasterise identical frames and differ only in
		// what the vendor is told. See ConfigureVendorSign.
		std::atomic<std::int32_t> g_vendorSign{ 1 };
		std::atomic<bool> g_vendorSignRefusalLogged{ false };

		// The armed backend sequence. g_phaseCount is the publication point: it is stored
		// with release after the two tables are filled and loaded with acquire before they
		// are read, and zero means read-only native mode. Arming and committing both run
		// on the render thread; a disarm from the settings thread only stores zero here
		// and never touches the tables.
		std::atomic<std::uint32_t> g_phaseCount{ 0 };
		std::atomic<std::uint32_t> g_frameIndex{ 0 };
		std::atomic<bool> g_restartPending{ false };
		std::atomic<std::uint32_t> g_armedOutputWidth{ 0 };
		std::atomic<std::uint32_t> g_armedOutputHeight{ 0 };
		float g_offsetX[kMaxPhaseCount] = {};
		float g_offsetY[kMaxPhaseCount] = {};

		// The exact bits CommitFrame last wrote. AcquireFrameSample requires the engine to
		// still hold them, which is what proves the write survived to evaluation time and
		// what catches another plugin writing the same two floats in the same frame.
		std::atomic<std::uint32_t> g_committedProjectionXBits{ 0 };
		std::atomic<std::uint32_t> g_committedProjectionYBits{ 0 };
		std::atomic<bool> g_committedValid{ false };
		std::atomic<bool> g_scopedEngineUpdatePending{ false };
		std::atomic<std::uint64_t> g_engineUpdateSerial{ 0 };
		std::atomic<std::uint64_t> g_scopedEngineUpdateMarks{ 0 };
		std::atomic<std::uint64_t> g_scopedEngineUpdateResumes{ 0 };
		std::atomic<std::uint64_t> g_scopedEngineUpdateSupersedes{ 0 };
		std::atomic<std::uint64_t> g_scopedEngineStateMismatches{ 0 };
		std::atomic<std::uint64_t> g_scopedSampleClassifications{ 0 };
		std::atomic<std::uint64_t> g_scopedSampleDeferrals{ 0 };
		std::atomic<std::uint64_t> g_scopedSampleRestoredMain{ 0 };
		std::atomic<std::uint64_t> g_scopedSampleRejections{ 0 };
		std::atomic<std::uint64_t> g_lastScopedEngineUpdateSerial{ 0 };
		std::atomic<bool> g_lastScopedSampleReadable{ false };
		std::atomic<std::uint32_t> g_lastScopedSampleXBits{ 0 };
		std::atomic<std::uint32_t> g_lastScopedSampleYBits{ 0 };
		std::atomic<std::uint64_t> g_lastScopedResumeFromSerial{ 0 };
		std::atomic<std::uint64_t> g_lastScopedSupersededFromSerial{ 0 };
		std::atomic<std::uintptr_t> g_lastEngineUpdateState{ 0 };
		std::atomic<std::uint32_t> g_lastEngineUpdateThread{ 0 };
		std::atomic<EngineUpdateProvenance> g_lastEngineUpdateProvenance{ EngineUpdateProvenance::kNone };
		std::atomic<std::uint64_t> g_sequenceMismatchCaptures{ 0 };
		std::atomic<std::uint64_t> g_lastSequenceMismatchEngineUpdateSerial{ 0 };
		std::atomic<EngineUpdateProvenance> g_lastSequenceMismatchEngineUpdateProvenance{ EngineUpdateProvenance::kNone };
		std::atomic<std::uint32_t> g_lastSequenceMismatchCurrentXBits{ 0 };
		std::atomic<std::uint32_t> g_lastSequenceMismatchCurrentYBits{ 0 };
		std::atomic<std::uint32_t> g_lastSequenceMismatchCommittedXBits{ 0 };
		std::atomic<std::uint32_t> g_lastSequenceMismatchCommittedYBits{ 0 };

		// Passive evidence from the post-original main-world writer, enabled only while a
		// configured native NGX startup has no backend sequence. A CAS-owned odd/even
		// generation publishes the fields; separate event and configuration epochs reject
		// missed writers and false->true tracking ABA. Atomics also expose the latched proof
		// to diagnostics on any thread.
		std::atomic<bool> g_nativeStartupTracking{ false };
		std::atomic<std::uint64_t> g_nativeStartupObservations{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupStaleDeferrals{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupWriterEvents{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupConfigEpoch{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupObservationGeneration{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupObservationContentions{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupObservationSerial{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupObservationEventSerial{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupObservationConfigEpoch{ 0 };
		std::atomic<bool> g_nativeStartupObservationReadable{ false };
		std::atomic<NativeStartupProvenance> g_nativeStartupProvenance{ NativeStartupProvenance::kNone };
		std::atomic<std::uintptr_t> g_nativeStartupState{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupThread{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupPhase{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupWidth{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupHeight{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupEnabled{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupDisableCount{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupProjectionXBits{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupProjectionYBits{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupMismatchEvents{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupMismatchGeneration{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupMismatchContentions{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupMismatchSerial{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupMismatchEventSerial{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupMismatchConfigEpoch{ 0 };
		std::atomic<bool> g_nativeStartupMismatchReadable{ false };
		std::atomic<std::uintptr_t> g_nativeStartupMismatchState{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupMismatchThread{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupMismatchPhase{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupMismatchWidth{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupMismatchHeight{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupMismatchEnabled{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupMismatchDisableCount{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupMismatchProjectionXBits{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupMismatchProjectionYBits{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupConsumedMismatchSerial{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupLastDeferralObservationSerial{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupLastDeferralWriterEventSerial{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupLastDeferralConfigEpoch{ 0 };
		std::atomic<std::uint64_t> g_nativeStartupLastDeferralMismatchSerial{ 0 };
		std::atomic<std::uintptr_t> g_nativeStartupLastDeferralClosedState{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralClosedThread{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralClosedPhase{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralClosedWidth{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralClosedHeight{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralClosedEnabled{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralClosedDisableCount{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralClosedProjectionXBits{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralClosedProjectionYBits{ 0 };
		std::atomic<std::uintptr_t> g_nativeStartupLastDeferralOpenState{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralOpenThread{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralOpenPhase{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralOpenWidth{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralOpenHeight{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralOpenEnabled{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralOpenDisableCount{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralOpenProjectionXBits{ 0 };
		std::atomic<std::uint32_t> g_nativeStartupLastDeferralOpenProjectionYBits{ 0 };

		// Written only from the render thread, so plain load/store is enough: there is one
		// writer and every reader is diagnostic.
		std::atomic<bool> g_boundsSeeded{ false };
		std::atomic<std::uint32_t> g_minJitterXBits{ 0 };
		std::atomic<std::uint32_t> g_maxJitterXBits{ 0 };
		std::atomic<std::uint32_t> g_minJitterYBits{ 0 };
		std::atomic<std::uint32_t> g_maxJitterYBits{ 0 };

		LogGate g_readGate;
		LogGate g_writeGate{ 60000 };
		// A separate gate, because a real mod conflict must never be swallowed behind the
		// routine gates-closed line a menu produces every frame.
		LogGate g_writeMismatchGate{ 60000 };

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

		[[nodiscard]] bool FailWrite(WriteFailure a_failure, std::string_view a_why) noexcept
		{
			g_lastWriteFailure.store(a_failure, std::memory_order_relaxed);
			g_committedValid.store(false, std::memory_order_relaxed);
			if (g_writeGate.Ready()) {
				logger::warn(FMT_STRING("jitter commit declined: {}"), a_why);
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

		void ObservePhase(std::uint32_t a_phase) noexcept
		{
			if (a_phase >= kMaxPhaseCount) {
				return;
			}
			auto& word = a_phase < 64 ? g_observedPhaseMaskLow : g_observedPhaseMaskHigh;
			word.fetch_or(std::uint64_t{ 1 } << (a_phase & 63u), std::memory_order_relaxed);
		}

		void ObserveBounds(float a_x, float a_y) noexcept
		{
			if (!g_boundsSeeded.exchange(true, std::memory_order_relaxed)) {
				StoreFloat(g_minJitterXBits, a_x);
				StoreFloat(g_maxJitterXBits, a_x);
				StoreFloat(g_minJitterYBits, a_y);
				StoreFloat(g_maxJitterYBits, a_y);
				return;
			}
			if (a_x < LoadFloat(g_minJitterXBits)) {
				StoreFloat(g_minJitterXBits, a_x);
			}
			if (a_x > LoadFloat(g_maxJitterXBits)) {
				StoreFloat(g_maxJitterXBits, a_x);
			}
			if (a_y < LoadFloat(g_minJitterYBits)) {
				StoreFloat(g_minJitterYBits, a_y);
			}
			if (a_y > LoadFloat(g_maxJitterYBits)) {
				StoreFloat(g_maxJitterYBits, a_y);
			}
		}

		[[nodiscard]] bool WriteU32(std::uintptr_t a_address, std::uint32_t a_value) noexcept
		{
			return FaultGuard::SafeWriteBytes(
				reinterpret_cast<void*>(a_address), &a_value, sizeof(a_value));
		}

		// Publishes only the local coherent snapshot that is about to fail the unarmed
		// native Halton check. This is separate from the loose diagnostics so the recovery
		// predicate can prove what the failed Acquire actually read, not what a later poll
		// happened to see. All contention and configuration races invalidate older records.
		void RecordNativeStartupMismatch(
			std::uintptr_t a_state,
			std::uint32_t a_phase,
			std::uint32_t a_projectionXBits,
			std::uint32_t a_projectionYBits,
			std::uint32_t a_width,
			std::uint32_t a_height,
			std::uint32_t a_enabled,
			std::uint32_t a_disableCount) noexcept
		{
			const auto configEpoch = g_nativeStartupConfigEpoch.load(std::memory_order_acquire);
			if (!g_nativeStartupTracking.load(std::memory_order_acquire) ||
				g_phaseCount.load(std::memory_order_acquire) != 0 ||
				configEpoch != g_nativeStartupConfigEpoch.load(std::memory_order_acquire)) {
				return;
			}
			const auto mismatchEventSerial = g_nativeStartupMismatchEvents.fetch_add(1, std::memory_order_acq_rel) + 1;
			auto generation = g_nativeStartupMismatchGeneration.load(std::memory_order_acquire);
			if ((generation & 1u) != 0 ||
				!g_nativeStartupMismatchGeneration.compare_exchange_strong(
					generation, generation + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
				g_nativeStartupMismatchContentions.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			g_nativeStartupMismatchReadable.store(false, std::memory_order_relaxed);
			g_nativeStartupMismatchState.store(a_state, std::memory_order_relaxed);
			g_nativeStartupMismatchThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
			g_nativeStartupMismatchPhase.store(a_phase, std::memory_order_relaxed);
			g_nativeStartupMismatchWidth.store(a_width, std::memory_order_relaxed);
			g_nativeStartupMismatchHeight.store(a_height, std::memory_order_relaxed);
			g_nativeStartupMismatchEnabled.store(a_enabled, std::memory_order_relaxed);
			g_nativeStartupMismatchDisableCount.store(a_disableCount, std::memory_order_relaxed);
			g_nativeStartupMismatchProjectionXBits.store(a_projectionXBits, std::memory_order_relaxed);
			g_nativeStartupMismatchProjectionYBits.store(a_projectionYBits, std::memory_order_relaxed);
			const auto serial = g_nativeStartupMismatchSerial.load(std::memory_order_relaxed) + 1;
			g_nativeStartupMismatchSerial.store(serial, std::memory_order_relaxed);
			g_nativeStartupMismatchEventSerial.store(mismatchEventSerial, std::memory_order_relaxed);
			g_nativeStartupMismatchConfigEpoch.store(configEpoch, std::memory_order_relaxed);
			const bool stillCurrent =
				g_nativeStartupTracking.load(std::memory_order_acquire) &&
				configEpoch == g_nativeStartupConfigEpoch.load(std::memory_order_acquire);
			g_nativeStartupMismatchReadable.store(stillCurrent, std::memory_order_relaxed);
			g_nativeStartupMismatchGeneration.store(generation + 2, std::memory_order_release);
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

	void ArmBackendSequence(
		const Backend::IBackend* a_backend,
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight) noexcept
	{
		if (!a_backend || a_renderWidth == 0 || a_outputWidth == 0 || a_outputHeight == 0) {
			DisarmBackendSequence();
			return;
		}

		const std::uint32_t count = a_backend->JitterPhaseCount(a_renderWidth, a_outputWidth);
		if (count < 2 || count > kMaxPhaseCount) {
			// One phase is a backend saying it has no sequence of its own - what the
			// passthrough control arm and the SDK-free NGX stub both report. Anything
			// above the cap is a backend the table cannot hold.
			DisarmBackendSequence();
			return;
		}

		// The whole table is built here, once, so the render hook never reaches across
		// the backend seam on a per-frame path inside a fault-guarded region.
		for (std::uint32_t n = 0; n < count; ++n) {
			float x = 0.0f;
			float y = 0.0f;
			a_backend->JitterOffset(n, count, x, y);
			if (!std::isfinite(x) || !std::isfinite(y) ||
				std::abs(x) > 0.5f + kBoundsTolerance || std::abs(y) > 0.5f + kBoundsTolerance) {
				logger::warn(FMT_STRING("jitter: backend sequence rejected - phase {} of {} is not a bounded finite sample"), n, count);
				DisarmBackendSequence();
				return;
			}
			g_offsetX[n] = x;
			g_offsetY[n] = y;
		}

		g_armedOutputWidth.store(a_outputWidth, std::memory_order_relaxed);
		g_armedOutputHeight.store(a_outputHeight, std::memory_order_relaxed);
		g_frameIndex.store(0, std::memory_order_relaxed);
		g_restartPending.store(false, std::memory_order_relaxed);
		g_committedValid.store(false, std::memory_order_relaxed);
		g_scopedEngineUpdatePending.store(false, std::memory_order_release);
		g_lastScopedEngineUpdateSerial.store(0, std::memory_order_relaxed);
		g_lastScopedSampleReadable.store(false, std::memory_order_relaxed);
		g_lastScopedResumeFromSerial.store(0, std::memory_order_relaxed);
		g_lastScopedSupersededFromSerial.store(0, std::memory_order_relaxed);
		g_observedPhaseMaskLow.store(0, std::memory_order_relaxed);
		g_observedPhaseMaskHigh.store(0, std::memory_order_relaxed);
		g_boundsSeeded.store(false, std::memory_order_relaxed);
		g_phaseCount.store(count, std::memory_order_release);
		logger::info(FMT_STRING("jitter: backend sequence armed - {} phases for {} -> {} per eye"),
			count, a_renderWidth, a_outputWidth);
	}

	void DisarmBackendSequence() noexcept
	{
		if (g_phaseCount.exchange(0, std::memory_order_acq_rel) != 0) {
			logger::info("jitter: backend sequence disarmed; the engine's native eight-phase sequence stands"sv);
		}
		g_committedValid.store(false, std::memory_order_relaxed);
		g_scopedEngineUpdatePending.store(false, std::memory_order_release);
		g_lastScopedEngineUpdateSerial.store(0, std::memory_order_relaxed);
		g_lastScopedSampleReadable.store(false, std::memory_order_relaxed);
		g_lastScopedResumeFromSerial.store(0, std::memory_order_relaxed);
		g_lastScopedSupersededFromSerial.store(0, std::memory_order_relaxed);
		g_armedOutputWidth.store(0, std::memory_order_relaxed);
		g_armedOutputHeight.store(0, std::memory_order_relaxed);
	}

	bool BackendSequenceArmed() noexcept
	{
		return g_phaseCount.load(std::memory_order_acquire) != 0;
	}

	void ConfigureNativeStartupTracking(bool a_enabled) noexcept
	{
		g_nativeStartupTracking.store(false, std::memory_order_release);
		const auto configEpoch = g_nativeStartupConfigEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
		// The jitter hook is outside HDRRoundTrip's callback-admission counter. Poison
		// every earlier hook event before changing epochs, even for a disable: an old
		// publisher that completes later then carries an event serial the reader rejects.
		g_nativeStartupWriterEvents.fetch_add(1, std::memory_order_acq_rel);
		g_nativeStartupMismatchEvents.fetch_add(1, std::memory_order_acq_rel);
		if (!a_enabled) {
			// Preserve the last observation and accepted deferral for post-run inspection.
			return;
		}

		// A true configuration runs behind HDRRoundTrip's odd configuration epoch, with
		// admitted render callbacks drained. Still acquire the same bounded writer token
		// the hook uses: if that exclusivity is ever violated, tracking remains off.
		auto generation = g_nativeStartupObservationGeneration.load(std::memory_order_acquire);
		if ((generation & 1u) != 0 ||
			!g_nativeStartupObservationGeneration.compare_exchange_strong(
				generation, generation + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
			g_nativeStartupObservationContentions.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		g_nativeStartupObservationReadable.store(false, std::memory_order_relaxed);
		g_nativeStartupProvenance.store(NativeStartupProvenance::kNone, std::memory_order_relaxed);
		g_nativeStartupState.store(0, std::memory_order_relaxed);
		g_nativeStartupThread.store(0, std::memory_order_relaxed);
		g_nativeStartupObservationEventSerial.store(0, std::memory_order_relaxed);
		g_nativeStartupObservationConfigEpoch.store(configEpoch, std::memory_order_relaxed);
		g_nativeStartupObservationGeneration.store(generation + 2, std::memory_order_release);
		g_nativeStartupTracking.store(true, std::memory_order_release);
	}

	bool NativeStartupTracking() noexcept
	{
		return g_nativeStartupTracking.load(std::memory_order_acquire);
	}

	void ObserveNativeStartupEngineUpdate(
		std::uintptr_t a_state,
		NativeStartupProvenance a_provenance) noexcept
	{
		const auto configEpoch = g_nativeStartupConfigEpoch.load(std::memory_order_acquire);
		if (!NativeStartupTracking() || BackendSequenceArmed() ||
			configEpoch != g_nativeStartupConfigEpoch.load(std::memory_order_acquire)) {
			return;
		}
		// Count every tracked hook event before trying to publish its fields. If a second
		// writer overlaps the bounded CAS, its event serial still advances and poisons the
		// older record; the safety reader can never mistake that record for the latest call.
		const auto writerEventSerial = g_nativeStartupWriterEvents.fetch_add(1, std::memory_order_acq_rel) + 1;
		auto generation = g_nativeStartupObservationGeneration.load(std::memory_order_acquire);
		if ((generation & 1u) != 0 ||
			!g_nativeStartupObservationGeneration.compare_exchange_strong(
				generation, generation + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
			g_nativeStartupObservationContentions.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		std::uint32_t phaseBefore = 0;
		std::uint32_t projectionXBits = 0;
		std::uint32_t projectionYBits = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t enabled = 0;
		std::uint32_t disableCount = 0;
		std::uint32_t phaseAfter = 0;
		const bool ownedState = a_state != 0 && a_state == g_state.load(std::memory_order_acquire);
		const bool readable = ownedState &&
		                      Engine::ReadU32(a_state + Addr::kJitterPhase, phaseBefore) &&
		                      Engine::ReadU32(a_state + Addr::kJitterProjectionX, projectionXBits) &&
		                      Engine::ReadU32(a_state + Addr::kJitterProjectionY, projectionYBits) &&
		                      Engine::ReadU32(a_state + Addr::kJitterPerEyeWidth, width) &&
		                      Engine::ReadU32(a_state + Addr::kJitterPerEyeHeight, height) &&
		                      Engine::ReadU32(a_state + Addr::kJitterEnabled, enabled) &&
		                      Engine::ReadU32(a_state + Addr::kJitterDisableCount, disableCount) &&
		                      Engine::ReadU32(a_state + Addr::kJitterPhase, phaseAfter) &&
		                      phaseBefore == phaseAfter && phaseAfter < kNativePhaseCount;

		g_nativeStartupObservationReadable.store(false, std::memory_order_relaxed);
		g_nativeStartupState.store(a_state, std::memory_order_relaxed);
		g_nativeStartupThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		g_nativeStartupPhase.store(phaseAfter, std::memory_order_relaxed);
		g_nativeStartupWidth.store(width, std::memory_order_relaxed);
		g_nativeStartupHeight.store(height, std::memory_order_relaxed);
		g_nativeStartupEnabled.store(enabled, std::memory_order_relaxed);
		g_nativeStartupDisableCount.store(disableCount, std::memory_order_relaxed);
		g_nativeStartupProjectionXBits.store(projectionXBits, std::memory_order_relaxed);
		g_nativeStartupProjectionYBits.store(projectionYBits, std::memory_order_relaxed);
		g_nativeStartupProvenance.store(
			ownedState ? a_provenance : NativeStartupProvenance::kStateMismatch,
			std::memory_order_relaxed);
		g_nativeStartupObservationReadable.store(readable, std::memory_order_relaxed);
		const auto serial = g_nativeStartupObservations.fetch_add(1, std::memory_order_relaxed) + 1;
		g_nativeStartupObservationSerial.store(serial, std::memory_order_relaxed);
		g_nativeStartupObservationEventSerial.store(writerEventSerial, std::memory_order_relaxed);
		g_nativeStartupObservationConfigEpoch.store(configEpoch, std::memory_order_relaxed);
		if (!NativeStartupTracking() ||
			configEpoch != g_nativeStartupConfigEpoch.load(std::memory_order_acquire)) {
			// A false->true tracking cycle may have completed while this old call was
			// paused. Publish the record as unreadable under its old epoch; the new epoch
			// must receive a new post-original call before it can defer anything.
			g_nativeStartupObservationReadable.store(false, std::memory_order_relaxed);
			g_nativeStartupProvenance.store(NativeStartupProvenance::kNone, std::memory_order_relaxed);
		}
		g_nativeStartupObservationGeneration.store(generation + 2, std::memory_order_release);
	}

	bool IsExactStaleClosedGateSample() noexcept
	{
		if (!NativeStartupTracking() || BackendSequenceArmed() ||
			g_lastFailure.load(std::memory_order_relaxed) != Failure::kSequenceMismatch) {
			return false;
		}

		const auto writerEventsBefore = g_nativeStartupWriterEvents.load(std::memory_order_acquire);
		const auto mismatchEventsBefore = g_nativeStartupMismatchEvents.load(std::memory_order_acquire);
		const auto configEpoch = g_nativeStartupConfigEpoch.load(std::memory_order_acquire);
		const auto generationBefore = g_nativeStartupObservationGeneration.load(std::memory_order_acquire);
		if (generationBefore == 0 || (generationBefore & 1u) != 0) {
			return false;
		}
		const auto observationSerial = g_nativeStartupObservationSerial.load(std::memory_order_relaxed);
		const auto observationEventSerial = g_nativeStartupObservationEventSerial.load(std::memory_order_relaxed);
		const auto observationConfigEpoch = g_nativeStartupObservationConfigEpoch.load(std::memory_order_relaxed);
		const bool observationReadable = g_nativeStartupObservationReadable.load(std::memory_order_relaxed);
		const auto provenance = g_nativeStartupProvenance.load(std::memory_order_relaxed);
		const auto writerState = g_nativeStartupState.load(std::memory_order_relaxed);
		const auto writerThread = g_nativeStartupThread.load(std::memory_order_relaxed);
		const auto writerPhase = g_nativeStartupPhase.load(std::memory_order_relaxed);
		const auto writerWidth = g_nativeStartupWidth.load(std::memory_order_relaxed);
		const auto writerHeight = g_nativeStartupHeight.load(std::memory_order_relaxed);
		const auto writerEnabled = g_nativeStartupEnabled.load(std::memory_order_relaxed);
		const auto writerDisableCount = g_nativeStartupDisableCount.load(std::memory_order_relaxed);
		const auto writerProjectionXBits = g_nativeStartupProjectionXBits.load(std::memory_order_relaxed);
		const auto writerProjectionYBits = g_nativeStartupProjectionYBits.load(std::memory_order_relaxed);
		const auto generationAfter = g_nativeStartupObservationGeneration.load(std::memory_order_acquire);
		if (generationBefore != generationAfter || (generationAfter & 1u) != 0) {
			return false;
		}
		const auto mismatchGenerationBefore = g_nativeStartupMismatchGeneration.load(std::memory_order_acquire);
		if (mismatchGenerationBefore == 0 || (mismatchGenerationBefore & 1u) != 0) {
			return false;
		}
		const auto mismatchSerial = g_nativeStartupMismatchSerial.load(std::memory_order_relaxed);
		const auto mismatchEventSerial = g_nativeStartupMismatchEventSerial.load(std::memory_order_relaxed);
		const auto mismatchConfigEpoch = g_nativeStartupMismatchConfigEpoch.load(std::memory_order_relaxed);
		const bool mismatchReadable = g_nativeStartupMismatchReadable.load(std::memory_order_relaxed);
		const auto mismatchState = g_nativeStartupMismatchState.load(std::memory_order_relaxed);
		const auto mismatchThread = g_nativeStartupMismatchThread.load(std::memory_order_relaxed);
		const auto mismatchPhase = g_nativeStartupMismatchPhase.load(std::memory_order_relaxed);
		const auto mismatchWidth = g_nativeStartupMismatchWidth.load(std::memory_order_relaxed);
		const auto mismatchHeight = g_nativeStartupMismatchHeight.load(std::memory_order_relaxed);
		const auto mismatchEnabled = g_nativeStartupMismatchEnabled.load(std::memory_order_relaxed);
		const auto mismatchDisableCount = g_nativeStartupMismatchDisableCount.load(std::memory_order_relaxed);
		const auto mismatchProjectionXBits = g_nativeStartupMismatchProjectionXBits.load(std::memory_order_relaxed);
		const auto mismatchProjectionYBits = g_nativeStartupMismatchProjectionYBits.load(std::memory_order_relaxed);
		const auto mismatchGenerationAfter = g_nativeStartupMismatchGeneration.load(std::memory_order_acquire);
		if (mismatchGenerationBefore != mismatchGenerationAfter || (mismatchGenerationAfter & 1u) != 0) {
			return false;
		}

		const auto currentState = g_state.load(std::memory_order_acquire);
		const auto currentThread = GetCurrentThreadId();
		std::uint32_t currentPhaseBefore = 0;
		std::uint32_t currentProjectionXBits = 0;
		std::uint32_t currentProjectionYBits = 0;
		std::uint32_t currentWidth = 0;
		std::uint32_t currentHeight = 0;
		std::uint32_t currentEnabled = 0;
		std::uint32_t currentDisableCount = 0;
		std::uint32_t currentPhase = 0;
		const bool currentReadable = currentState != 0 &&
		                             Engine::ReadU32(currentState + Addr::kJitterPhase, currentPhaseBefore) &&
		                             Engine::ReadU32(currentState + Addr::kJitterProjectionX, currentProjectionXBits) &&
		                             Engine::ReadU32(currentState + Addr::kJitterProjectionY, currentProjectionYBits) &&
		                             Engine::ReadU32(currentState + Addr::kJitterPerEyeWidth, currentWidth) &&
		                             Engine::ReadU32(currentState + Addr::kJitterPerEyeHeight, currentHeight) &&
		                             Engine::ReadU32(currentState + Addr::kJitterEnabled, currentEnabled) &&
		                             Engine::ReadU32(currentState + Addr::kJitterDisableCount, currentDisableCount) &&
		                             Engine::ReadU32(currentState + Addr::kJitterPhase, currentPhase) &&
		                             currentPhaseBefore == currentPhase && currentPhase < kNativePhaseCount;
		const auto writerEventsAfter = g_nativeStartupWriterEvents.load(std::memory_order_acquire);
		const auto mismatchEventsAfter = g_nativeStartupMismatchEvents.load(std::memory_order_acquire);
		if (!currentReadable || writerEventsBefore != writerEventsAfter ||
			observationEventSerial != writerEventsAfter ||
			mismatchEventsBefore != mismatchEventsAfter || mismatchEventSerial != mismatchEventsAfter ||
			configEpoch != g_nativeStartupConfigEpoch.load(std::memory_order_acquire) ||
			observationConfigEpoch != configEpoch || mismatchConfigEpoch != configEpoch ||
			!mismatchReadable || mismatchSerial == 0) {
			return false;
		}
		if (observationSerial == 0 || !observationReadable ||
			provenance != NativeStartupProvenance::kMain ||
			writerState != currentState || writerThread != currentThread) {
			return false;
		}

		// The prior main writer must have taken the engine's proven closed-gate zeroing
		// path. The failed read must now see open gates but precisely the same untouched
		// state. A later writer publishes a new observation first, so open-gate, scoped,
		// unknown, unreadable and state-mismatched writers all fail this predicate.
		const bool writerGatesClosed =
			writerEnabled == 0 || writerDisableCount != 0;
		const bool writerSampleZero =
			writerProjectionXBits == 0 && writerProjectionYBits == 0;
		const bool currentGatesOpen =
			currentEnabled != 0 && currentDisableCount == 0;
		const bool mismatchGatesOpen = mismatchEnabled != 0 && mismatchDisableCount == 0;
		const bool failedMatchesWriter =
			mismatchState == writerState && mismatchThread == writerThread && mismatchPhase == writerPhase &&
			mismatchWidth == writerWidth && mismatchHeight == writerHeight &&
			mismatchProjectionXBits == writerProjectionXBits && mismatchProjectionYBits == writerProjectionYBits;
		const bool freshMatchesFailure =
			currentState == mismatchState && currentThread == mismatchThread && currentPhase == mismatchPhase &&
			currentWidth == mismatchWidth && currentHeight == mismatchHeight &&
			currentEnabled == mismatchEnabled && currentDisableCount == mismatchDisableCount &&
			currentProjectionXBits == mismatchProjectionXBits && currentProjectionYBits == mismatchProjectionYBits;
		if (!writerGatesClosed || !writerSampleZero || !mismatchGatesOpen || !currentGatesOpen ||
			!failedMatchesWriter || !freshMatchesFailure || !NativeStartupTracking() ||
			configEpoch != g_nativeStartupConfigEpoch.load(std::memory_order_acquire)) {
			return false;
		}
		auto consumedMismatchSerial = g_nativeStartupConsumedMismatchSerial.load(std::memory_order_acquire);
		if (mismatchSerial <= consumedMismatchSerial ||
			!g_nativeStartupConsumedMismatchSerial.compare_exchange_strong(
				consumedMismatchSerial, mismatchSerial, std::memory_order_acq_rel, std::memory_order_acquire)) {
			return false;
		}

		// Preserve the proof after successful preparation disables live tracking. The
		// release increment is the publication point for both halves of the comparison.
		g_nativeStartupLastDeferralObservationSerial.store(observationSerial, std::memory_order_relaxed);
		g_nativeStartupLastDeferralWriterEventSerial.store(observationEventSerial, std::memory_order_relaxed);
		g_nativeStartupLastDeferralConfigEpoch.store(observationConfigEpoch, std::memory_order_relaxed);
		g_nativeStartupLastDeferralMismatchSerial.store(mismatchSerial, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedState.store(writerState, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedThread.store(writerThread, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedPhase.store(writerPhase, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedWidth.store(writerWidth, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedHeight.store(writerHeight, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedEnabled.store(writerEnabled, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedDisableCount.store(writerDisableCount, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedProjectionXBits.store(writerProjectionXBits, std::memory_order_relaxed);
		g_nativeStartupLastDeferralClosedProjectionYBits.store(writerProjectionYBits, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenState.store(mismatchState, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenThread.store(mismatchThread, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenPhase.store(mismatchPhase, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenWidth.store(mismatchWidth, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenHeight.store(mismatchHeight, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenEnabled.store(mismatchEnabled, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenDisableCount.store(mismatchDisableCount, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenProjectionXBits.store(mismatchProjectionXBits, std::memory_order_relaxed);
		g_nativeStartupLastDeferralOpenProjectionYBits.store(mismatchProjectionYBits, std::memory_order_relaxed);
		g_nativeStartupStaleDeferrals.fetch_add(1, std::memory_order_release);
		return true;
	}

	void RequestSequenceRestart() noexcept
	{
		if (!g_restartPending.exchange(true, std::memory_order_relaxed)) {
			g_sequenceRestarts.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void MarkScopedEngineUpdate(std::uintptr_t a_state) noexcept
	{
		const auto serial = g_engineUpdateSerial.fetch_add(1, std::memory_order_relaxed) + 1;
		g_lastEngineUpdateState.store(a_state, std::memory_order_relaxed);
		g_lastEngineUpdateThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		if (a_state == 0 || a_state != g_state.load(std::memory_order_acquire)) {
			g_committedValid.store(false, std::memory_order_relaxed);
			g_lastScopedSampleReadable.store(false, std::memory_order_relaxed);
			const auto superseded = g_lastScopedEngineUpdateSerial.load(std::memory_order_relaxed);
			if (g_scopedEngineUpdatePending.exchange(false, std::memory_order_acq_rel)) {
				g_lastScopedSupersededFromSerial.store(superseded, std::memory_order_relaxed);
				g_scopedEngineUpdateSupersedes.fetch_add(1, std::memory_order_relaxed);
			}
			g_scopedEngineStateMismatches.fetch_add(1, std::memory_order_relaxed);
			g_lastEngineUpdateProvenance.store(EngineUpdateProvenance::kStateMismatch, std::memory_order_release);
			return;
		}

		std::uint32_t scopedXBits = 0;
		std::uint32_t scopedYBits = 0;
		const bool scopedSampleReadable =
			Engine::ReadU32(a_state + Addr::kJitterProjectionX, scopedXBits) &&
			Engine::ReadU32(a_state + Addr::kJitterProjectionY, scopedYBits);
		g_lastScopedSampleReadable.store(false, std::memory_order_relaxed);
		g_lastScopedSampleXBits.store(scopedXBits, std::memory_order_relaxed);
		g_lastScopedSampleYBits.store(scopedYBits, std::memory_order_relaxed);
		g_lastScopedEngineUpdateSerial.store(serial, std::memory_order_relaxed);
		g_scopedEngineUpdateMarks.fetch_add(1, std::memory_order_relaxed);
		g_lastEngineUpdateProvenance.store(EngineUpdateProvenance::kScoped, std::memory_order_relaxed);
		g_lastScopedSampleReadable.store(scopedSampleReadable, std::memory_order_relaxed);
		g_scopedEngineUpdatePending.store(true, std::memory_order_release);
	}

	EngineUpdateToken BeginMainEngineUpdate(std::uintptr_t a_state) noexcept
	{
		EngineUpdateToken token{};
		token.serial = g_engineUpdateSerial.fetch_add(1, std::memory_order_relaxed) + 1;
		g_lastEngineUpdateState.store(a_state, std::memory_order_relaxed);
		g_lastEngineUpdateThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		g_committedValid.store(false, std::memory_order_relaxed);
		if (g_scopedEngineUpdatePending.exchange(false, std::memory_order_acq_rel)) {
			token.scopedMarkerSerial = g_lastScopedEngineUpdateSerial.load(std::memory_order_relaxed);
			g_lastScopedSupersededFromSerial.store(token.scopedMarkerSerial, std::memory_order_relaxed);
			g_scopedEngineUpdateSupersedes.fetch_add(1, std::memory_order_relaxed);
		}
		if (a_state == 0 || a_state != g_state.load(std::memory_order_acquire)) {
			g_scopedEngineStateMismatches.fetch_add(1, std::memory_order_relaxed);
			g_lastEngineUpdateProvenance.store(EngineUpdateProvenance::kStateMismatch, std::memory_order_release);
			token.scopedMarkerSerial = 0;
			return token;
		}
		token.ownedState = true;
		g_lastEngineUpdateProvenance.store(EngineUpdateProvenance::kMain, std::memory_order_release);
		return token;
	}

	void CompleteMainEngineUpdate(EngineUpdateToken a_token) noexcept
	{
		if (!a_token.ownedState || a_token.scopedMarkerSerial == 0) {
			return;
		}
		g_lastScopedResumeFromSerial.store(a_token.scopedMarkerSerial, std::memory_order_release);
		g_scopedEngineUpdateResumes.fetch_add(1, std::memory_order_relaxed);
	}

	void SupersedeScopedEngineUpdate(std::uintptr_t a_state) noexcept
	{
		g_engineUpdateSerial.fetch_add(1, std::memory_order_relaxed);
		g_lastEngineUpdateState.store(a_state, std::memory_order_relaxed);
		g_lastEngineUpdateThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		g_committedValid.store(false, std::memory_order_relaxed);
		if (g_scopedEngineUpdatePending.exchange(false, std::memory_order_acq_rel)) {
			g_lastScopedSupersededFromSerial.store(
				g_lastScopedEngineUpdateSerial.load(std::memory_order_relaxed), std::memory_order_relaxed);
			g_scopedEngineUpdateSupersedes.fetch_add(1, std::memory_order_relaxed);
		}
		const auto ownedState = g_state.load(std::memory_order_acquire);
		const auto provenance = a_state != 0 && a_state == ownedState ?
		                            EngineUpdateProvenance::kUnknown :
		                            EngineUpdateProvenance::kStateMismatch;
		if (provenance == EngineUpdateProvenance::kStateMismatch) {
			g_scopedEngineStateMismatches.fetch_add(1, std::memory_order_relaxed);
		}
		g_lastEngineUpdateProvenance.store(provenance, std::memory_order_release);
	}

	ScopedSampleStatus ClassifyScopedSample() noexcept
	{
		if (!g_scopedEngineUpdatePending.load(std::memory_order_acquire)) {
			return ScopedSampleStatus::kNone;
		}
		g_scopedSampleClassifications.fetch_add(1, std::memory_order_relaxed);
		const auto markerSerial = g_lastScopedEngineUpdateSerial.load(std::memory_order_relaxed);
		const auto state = g_state.load(std::memory_order_acquire);
		const auto writerState = g_lastEngineUpdateState.load(std::memory_order_relaxed);
		const auto writerThread = g_lastEngineUpdateThread.load(std::memory_order_relaxed);
		const auto writerProvenance = g_lastEngineUpdateProvenance.load(std::memory_order_acquire);
		std::uint32_t currentXBits = 0;
		std::uint32_t currentYBits = 0;
		const bool currentReadable = state != 0 &&
		                             Engine::ReadU32(state + Addr::kJitterProjectionX, currentXBits) &&
		                             Engine::ReadU32(state + Addr::kJitterProjectionY, currentYBits);
		if (!g_scopedEngineUpdatePending.load(std::memory_order_acquire) ||
			markerSerial == 0 || markerSerial != g_lastScopedEngineUpdateSerial.load(std::memory_order_relaxed) ||
			writerProvenance != EngineUpdateProvenance::kScoped || writerState != state ||
			writerThread != GetCurrentThreadId() || !currentReadable) {
			g_scopedSampleRejections.fetch_add(1, std::memory_order_relaxed);
			return ScopedSampleStatus::kUntrusted;
		}

		const auto committedXBits = g_committedProjectionXBits.load(std::memory_order_relaxed);
		const auto committedYBits = g_committedProjectionYBits.load(std::memory_order_relaxed);
		if (g_committedValid.load(std::memory_order_relaxed) &&
			currentXBits == committedXBits && currentYBits == committedYBits) {
			g_scopedEngineUpdatePending.store(false, std::memory_order_release);
			g_scopedSampleRestoredMain.fetch_add(1, std::memory_order_relaxed);
			return ScopedSampleStatus::kNone;
		}

		if (g_lastScopedSampleReadable.load(std::memory_order_relaxed) &&
			currentXBits == g_lastScopedSampleXBits.load(std::memory_order_relaxed) &&
			currentYBits == g_lastScopedSampleYBits.load(std::memory_order_relaxed)) {
			g_scopedSampleDeferrals.fetch_add(1, std::memory_order_relaxed);
			return ScopedSampleStatus::kScoped;
		}

		g_scopedSampleRejections.fetch_add(1, std::memory_order_relaxed);
		return ScopedSampleStatus::kUntrusted;
	}

	bool CommitFrame(float& a_outX, float& a_outY) noexcept
	{
		g_writeAttempts.fetch_add(1, std::memory_order_relaxed);
		const auto state = g_state.load(std::memory_order_acquire);
		if (state == 0) {
			return FailWrite(WriteFailure::kUnresolved, "native state is unresolved"sv);
		}
		const std::uint32_t count = g_phaseCount.load(std::memory_order_acquire);
		if (count < 2) {
			return FailWrite(WriteFailure::kNotArmed, "no backend sequence is armed"sv);
		}

		std::uint32_t enabled = 0;
		std::uint32_t disableCount = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		if (!Engine::ReadU32(state + Addr::kJitterEnabled, enabled) ||
			!Engine::ReadU32(state + Addr::kJitterDisableCount, disableCount) ||
			!Engine::ReadU32(state + Addr::kJitterPerEyeWidth, width) ||
			!Engine::ReadU32(state + Addr::kJitterPerEyeHeight, height)) {
			return FailWrite(WriteFailure::kReadFault, "one or more guarded state reads failed"sv);
		}
		if (enabled == 0 || disableCount != 0) {
			// The original has already zeroed both floats in one store at 1daa277, and
			// BuildStereoCameraViewData's own +0xa8 test at 1daa51e skips applying them.
			// Writing here would desynchronise what we tell the backend from what the
			// engine actually rasterised. This is an engine condition rather than a
			// defect, so it gets its own counter and shares the rate-gated line.
			g_writeGateDeclines.fetch_add(1, std::memory_order_relaxed);
			return FailWrite(WriteFailure::kGatesClosed, "native jitter enable gates are closed"sv);
		}
		if (width == 0 || height == 0 ||
			width != g_armedOutputWidth.load(std::memory_order_relaxed) ||
			height != g_armedOutputHeight.load(std::memory_order_relaxed)) {
			return FailWrite(WriteFailure::kDimensionMismatch,
				"the engine per-eye extent does not match the extent the sequence was armed for"sv);
		}

		if (g_restartPending.exchange(false, std::memory_order_relaxed)) {
			g_frameIndex.store(0, std::memory_order_relaxed);
		}
		const std::uint32_t phase = g_frameIndex.fetch_add(1, std::memory_order_relaxed) % count;
		const float jitterX = g_offsetX[phase];
		const float jitterY = g_offsetY[phase];
		// The divisor is the FULL per-eye extent from state+0x78/+0x7c, never renderWidth
		// and never W*ratio: BuildStereoCameraViewData rescales by W/floor(W*ratio) on the
		// way to the projection, so the ratio cancels exactly.
		const float projectionX = CameraPixelsToProjectionX(jitterX, width);
		const float projectionY = CameraPixelsToProjectionY(jitterY, height);
		if (!std::isfinite(jitterX) || !std::isfinite(jitterY) ||
			!std::isfinite(projectionX) || !std::isfinite(projectionY) ||
			std::abs(jitterX) > 0.5f + kBoundsTolerance ||
			std::abs(jitterY) > 0.5f + kBoundsTolerance) {
			return FailWrite(WriteFailure::kInvalidValue, "the armed sample is not finite and bounded"sv);
		}

		// Two 32-bit stores rather than one 64-bit store: a torn observer then sees at
		// most one stale axis instead of a mixture of two samples.
		const auto projectionXBits = std::bit_cast<std::uint32_t>(projectionX);
		const auto projectionYBits = std::bit_cast<std::uint32_t>(projectionY);
		g_committedValid.store(false, std::memory_order_relaxed);
		if (!WriteU32(state + Addr::kJitterProjectionX, projectionXBits) ||
			!WriteU32(state + Addr::kJitterProjectionY, projectionYBits)) {
			return FailWrite(WriteFailure::kWriteFault, "a guarded store into the jitter state faulted"sv);
		}

		std::uint32_t readBackX = 0;
		std::uint32_t readBackY = 0;
		if (!Engine::ReadU32(state + Addr::kJitterProjectionX, readBackX) ||
			!Engine::ReadU32(state + Addr::kJitterProjectionY, readBackY)) {
			return FailWrite(WriteFailure::kReadFault, "the committed floats could not be read back"sv);
		}
		if (readBackX != projectionXBits || readBackY != projectionYBits) {
			g_writeMismatches.fetch_add(1, std::memory_order_relaxed);
			g_lastWriteFailure.store(WriteFailure::kReadBackMismatch, std::memory_order_relaxed);
			if (g_writeMismatchGate.Ready()) {
				logger::warn(
					FMT_STRING("jitter commit declined: state+{:02X}/{:02X} read back {:08X}/{:08X} after writing {:08X}/{:08X} - another plugin is writing the same floats"),
					Addr::kJitterProjectionX,
					Addr::kJitterProjectionY,
					readBackX,
					readBackY,
					projectionXBits,
					projectionYBits);
			}
			return false;
		}

		g_committedProjectionXBits.store(projectionXBits, std::memory_order_relaxed);
		g_committedProjectionYBits.store(projectionYBits, std::memory_order_relaxed);
		g_committedValid.store(true, std::memory_order_relaxed);
		g_phase.store(phase, std::memory_order_relaxed);
		ObservePhase(phase);
		g_lastWriteFailure.store(WriteFailure::kNone, std::memory_order_relaxed);
		g_writeSuccesses.fetch_add(1, std::memory_order_relaxed);
		a_outX = jitterX;
		a_outY = jitterY;
		return true;
	}

	bool AcquireFrameSample(
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

		g_enginePhase.store(phaseAfter, std::memory_order_relaxed);
		g_engineWidth.store(width, std::memory_order_relaxed);
		g_engineHeight.store(height, std::memory_order_relaxed);
		g_enabled.store(enabled, std::memory_order_relaxed);
		g_disableCount.store(disableCount, std::memory_order_relaxed);
		g_projectionXBits.store(projectionXBits, std::memory_order_relaxed);
		g_projectionYBits.store(projectionYBits, std::memory_order_relaxed);

		// The engine's counter keeps advancing modulo 8 whatever sequence was committed
		// into the floats, and its parity at +0x0C selects the TAA history target, so this
		// stays a check on the engine rather than on our sequence index.
		if (phaseBefore != phaseAfter || phaseAfter >= kNativePhaseCount) {
			return Fail(Failure::kTornRead, "engine phase changed during the snapshot or was outside 0..7"sv);
		}
		// The expected extent is the FULL per-eye output extent. The state holds that
		// extent, dynamic resolution never touches it, and the ratio cancels in the
		// conversion below, so renderWidth here would be wrong even though the frame
		// rasterises at renderWidth.
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

		if (BackendSequenceArmed()) {
			// The proof that this frame's commit survived to evaluation time. A declined
			// commit, a frame whose jitter call this build does not own, or another plugin
			// writing the same two floats each fail closed here.
			const auto committedXBits = g_committedProjectionXBits.load(std::memory_order_relaxed);
			const auto committedYBits = g_committedProjectionYBits.load(std::memory_order_relaxed);
			if (!g_committedValid.load(std::memory_order_relaxed) ||
				projectionXBits != committedXBits || projectionYBits != committedYBits) {
				g_lastSequenceMismatchCurrentXBits.store(projectionXBits, std::memory_order_relaxed);
				g_lastSequenceMismatchCurrentYBits.store(projectionYBits, std::memory_order_relaxed);
				g_lastSequenceMismatchCommittedXBits.store(committedXBits, std::memory_order_relaxed);
				g_lastSequenceMismatchCommittedYBits.store(committedYBits, std::memory_order_relaxed);
				g_lastSequenceMismatchEngineUpdateSerial.store(
					g_engineUpdateSerial.load(std::memory_order_relaxed), std::memory_order_relaxed);
				g_lastSequenceMismatchEngineUpdateProvenance.store(
					g_lastEngineUpdateProvenance.load(std::memory_order_acquire), std::memory_order_relaxed);
				g_sequenceMismatchCaptures.fetch_add(1, std::memory_order_release);
				return Fail(Failure::kSequenceMismatch, "the engine no longer holds the committed backend sample"sv);
			}
		} else {
			const std::uint32_t index = phaseAfter + 1;
			const float expectedX = 0.5f - RadicalInverse(2, index);
			const float expectedY = RadicalInverse(3, index) - 0.5f;
			if (std::abs(jitterX - expectedX) > kSequenceTolerance ||
				std::abs(jitterY - expectedY) > kSequenceTolerance) {
				RecordNativeStartupMismatch(
					state,
					phaseAfter,
					projectionXBits,
					projectionYBits,
					width,
					height,
					enabled,
					disableCount);
				return Fail(Failure::kSequenceMismatch, "converted sample does not match the native Halton phase"sv);
			}
			g_phase.store(phaseAfter, std::memory_order_relaxed);
			ObservePhase(phaseAfter);
		}

		StoreFloat(g_jitterXBits, jitterX);
		StoreFloat(g_jitterYBits, jitterY);
		ObserveBounds(jitterX, jitterY);
		g_lastFailure.store(Failure::kNone, std::memory_order_relaxed);
		g_readSuccesses.fetch_add(1, std::memory_order_relaxed);
		// The one place the vendor sign applies. Everything above records the sample as
		// the engine holds it: DirectSR camera-jitter pixels, the convention the native
		// eight-phase check needs. The vendor's InJitterOffset is the offset applied to
		// the projection, which for the same engine store bits is the NEGATED camera
		// sample. Measured: handing the camera sample itself reconstructs a 3-pixel grid
		// at the aliased input pitch on every textured surface (a spectral spike 8 to
		// 37 times its neighbourhood at 1/3 cycle per pixel, ten captures of ten, every
		// weather); the negated sample shows none (1 to 6 times, ten of ten) and the
		// raster has none. jitterVendorSign = -1 reproduces the old convention.
		const float vendorSign = -static_cast<float>(g_vendorSign.load(std::memory_order_relaxed));
		a_outX = jitterX * vendorSign;
		a_outY = jitterY * vendorSign;
		return true;
	}

	void ConfigureVendorSign(std::int64_t a_sign) noexcept
	{
		if (a_sign == 1 || a_sign == -1) {
			g_vendorSign.store(static_cast<std::int32_t>(a_sign), std::memory_order_relaxed);
			return;
		}
		g_vendorSign.store(1, std::memory_order_relaxed);
		if (!g_vendorSignRefusalLogged.exchange(true, std::memory_order_relaxed)) {
			logger::warn(FMT_STRING("jitter: jitterVendorSign {} refused - only 1 or -1 is meaningful; using 1"), a_sign);
		}
	}

	std::uint32_t PhaseCount() noexcept
	{
		const std::uint32_t armed = g_phaseCount.load(std::memory_order_acquire);
		return armed != 0 ? armed : kNativePhaseCount;
	}

	std::uint32_t NativePhaseCount() noexcept
	{
		return kNativePhaseCount;
	}

	bool ReadCurrentPhase(std::uint32_t& a_outPhase) noexcept
	{
		const auto state = g_state.load(std::memory_order_acquire);
		std::uint32_t phase = 0;
		if (state == 0 || !Engine::ReadU32(state + Addr::kJitterPhase, phase) || phase >= kNativePhaseCount) {
			return false;
		}
		a_outPhase = phase;
		return true;
	}

	RawState ReadRawState() noexcept
	{
		RawState out{};
		const auto state = g_state.load(std::memory_order_acquire);
		out.address = state;
		out.resolved = state != 0;
		if (!out.resolved) {
			return out;
		}

		// The same block AcquireFrameSample reads, in the same order, bracketed by the
		// phase so a reader can tell a coherent sample from one taken across the engine's
		// own update. Unlike that one this reports the torn case instead of failing it:
		// the instrument's question is what the floats read, and "they changed while I
		// looked" is an answer worth publishing rather than discarding.
		std::uint32_t phaseAfter = 0;
		out.read = Engine::ReadU32(state + Addr::kJitterPhase, out.phase) &&
		           Engine::ReadU32(state + Addr::kJitterProjectionX, out.projectionXBits) &&
		           Engine::ReadU32(state + Addr::kJitterProjectionY, out.projectionYBits) &&
		           Engine::ReadU32(state + Addr::kJitterPerEyeWidth, out.perEyeWidth) &&
		           Engine::ReadU32(state + Addr::kJitterPerEyeHeight, out.perEyeHeight) &&
		           Engine::ReadU32(state + Addr::kJitterEnabled, out.enabled) &&
		           Engine::ReadU32(state + Addr::kJitterDisableCount, out.disableCount) &&
		           Engine::ReadU32(state + Addr::kJitterPhase, phaseAfter);
		out.torn = out.read && phaseAfter != out.phase;
		return out;
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
		const auto maskLow = g_observedPhaseMaskLow.load(std::memory_order_relaxed);
		const auto maskHigh = g_observedPhaseMaskHigh.load(std::memory_order_relaxed);
		State out{};
		out.resolved = state != 0;
		out.backendSequenceArmed = BackendSequenceArmed();
		out.address = state;
		out.readAttempts = g_readAttempts.load(std::memory_order_relaxed);
		out.readSuccesses = g_readSuccesses.load(std::memory_order_relaxed);
		out.readFailures = g_readFailures.load(std::memory_order_relaxed);
		out.writeAttempts = g_writeAttempts.load(std::memory_order_relaxed);
		out.writeSuccesses = g_writeSuccesses.load(std::memory_order_relaxed);
		out.writeMismatches = g_writeMismatches.load(std::memory_order_relaxed);
		out.writeGateDeclines = g_writeGateDeclines.load(std::memory_order_relaxed);
		out.sequenceRestarts = g_sequenceRestarts.load(std::memory_order_relaxed);
		out.scopedEngineUpdatePending = g_scopedEngineUpdatePending.load(std::memory_order_acquire);
		out.engineUpdateSerial = g_engineUpdateSerial.load(std::memory_order_relaxed);
		out.scopedEngineUpdateMarks = g_scopedEngineUpdateMarks.load(std::memory_order_relaxed);
		out.scopedEngineUpdateResumes = g_scopedEngineUpdateResumes.load(std::memory_order_relaxed);
		out.scopedEngineUpdateSupersedes = g_scopedEngineUpdateSupersedes.load(std::memory_order_relaxed);
		out.scopedEngineStateMismatches = g_scopedEngineStateMismatches.load(std::memory_order_relaxed);
		out.scopedSampleClassifications = g_scopedSampleClassifications.load(std::memory_order_relaxed);
		out.scopedSampleDeferrals = g_scopedSampleDeferrals.load(std::memory_order_relaxed);
		out.scopedSampleRestoredMain = g_scopedSampleRestoredMain.load(std::memory_order_relaxed);
		out.scopedSampleRejections = g_scopedSampleRejections.load(std::memory_order_relaxed);
		out.lastScopedEngineUpdateSerial = g_lastScopedEngineUpdateSerial.load(std::memory_order_relaxed);
		out.lastScopedSampleReadable = g_lastScopedSampleReadable.load(std::memory_order_relaxed);
		out.lastScopedSampleXBits = g_lastScopedSampleXBits.load(std::memory_order_relaxed);
		out.lastScopedSampleYBits = g_lastScopedSampleYBits.load(std::memory_order_relaxed);
		out.lastScopedResumeFromSerial = g_lastScopedResumeFromSerial.load(std::memory_order_acquire);
		out.lastScopedSupersededFromSerial = g_lastScopedSupersededFromSerial.load(std::memory_order_relaxed);
		out.lastEngineUpdateState = g_lastEngineUpdateState.load(std::memory_order_relaxed);
		out.lastEngineUpdateThread = g_lastEngineUpdateThread.load(std::memory_order_relaxed);
		out.lastEngineUpdateProvenance = g_lastEngineUpdateProvenance.load(std::memory_order_acquire);
		out.sequenceMismatchCaptures = g_sequenceMismatchCaptures.load(std::memory_order_acquire);
		out.lastSequenceMismatchEngineUpdateSerial = g_lastSequenceMismatchEngineUpdateSerial.load(std::memory_order_relaxed);
		out.lastSequenceMismatchEngineUpdateProvenance = g_lastSequenceMismatchEngineUpdateProvenance.load(std::memory_order_relaxed);
		out.lastSequenceMismatchCurrentXBits = g_lastSequenceMismatchCurrentXBits.load(std::memory_order_relaxed);
		out.lastSequenceMismatchCurrentYBits = g_lastSequenceMismatchCurrentYBits.load(std::memory_order_relaxed);
		out.lastSequenceMismatchCommittedXBits = g_lastSequenceMismatchCommittedXBits.load(std::memory_order_relaxed);
		out.lastSequenceMismatchCommittedYBits = g_lastSequenceMismatchCommittedYBits.load(std::memory_order_relaxed);
		out.nativeStartupTracking = g_nativeStartupTracking.load(std::memory_order_acquire);
		out.nativeStartupObservations = g_nativeStartupObservations.load(std::memory_order_relaxed);
		out.nativeStartupStaleDeferrals = g_nativeStartupStaleDeferrals.load(std::memory_order_acquire);
		out.nativeStartupWriterEvents = g_nativeStartupWriterEvents.load(std::memory_order_acquire);
		out.nativeStartupConfigEpoch = g_nativeStartupConfigEpoch.load(std::memory_order_acquire);
		out.nativeStartupObservationGeneration = g_nativeStartupObservationGeneration.load(std::memory_order_acquire);
		out.nativeStartupObservationContentions = g_nativeStartupObservationContentions.load(std::memory_order_relaxed);
		out.nativeStartupObservationSerial = g_nativeStartupObservationSerial.load(std::memory_order_acquire);
		out.nativeStartupObservationEventSerial = g_nativeStartupObservationEventSerial.load(std::memory_order_relaxed);
		out.nativeStartupObservationConfigEpoch = g_nativeStartupObservationConfigEpoch.load(std::memory_order_relaxed);
		out.nativeStartupObservationReadable = g_nativeStartupObservationReadable.load(std::memory_order_relaxed);
		out.nativeStartupProvenance = g_nativeStartupProvenance.load(std::memory_order_relaxed);
		out.nativeStartupState = g_nativeStartupState.load(std::memory_order_relaxed);
		out.nativeStartupThread = g_nativeStartupThread.load(std::memory_order_relaxed);
		out.nativeStartupPhase = g_nativeStartupPhase.load(std::memory_order_relaxed);
		out.nativeStartupWidth = g_nativeStartupWidth.load(std::memory_order_relaxed);
		out.nativeStartupHeight = g_nativeStartupHeight.load(std::memory_order_relaxed);
		out.nativeStartupEnabled = g_nativeStartupEnabled.load(std::memory_order_relaxed);
		out.nativeStartupDisableCount = g_nativeStartupDisableCount.load(std::memory_order_relaxed);
		out.nativeStartupProjectionXBits = g_nativeStartupProjectionXBits.load(std::memory_order_relaxed);
		out.nativeStartupProjectionYBits = g_nativeStartupProjectionYBits.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralObservationSerial = g_nativeStartupLastDeferralObservationSerial.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralWriterEventSerial = g_nativeStartupLastDeferralWriterEventSerial.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralConfigEpoch = g_nativeStartupLastDeferralConfigEpoch.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralMismatchSerial = g_nativeStartupLastDeferralMismatchSerial.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedState = g_nativeStartupLastDeferralClosedState.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedThread = g_nativeStartupLastDeferralClosedThread.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedPhase = g_nativeStartupLastDeferralClosedPhase.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedWidth = g_nativeStartupLastDeferralClosedWidth.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedHeight = g_nativeStartupLastDeferralClosedHeight.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedEnabled = g_nativeStartupLastDeferralClosedEnabled.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedDisableCount = g_nativeStartupLastDeferralClosedDisableCount.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedProjectionXBits = g_nativeStartupLastDeferralClosedProjectionXBits.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralClosedProjectionYBits = g_nativeStartupLastDeferralClosedProjectionYBits.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenState = g_nativeStartupLastDeferralOpenState.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenThread = g_nativeStartupLastDeferralOpenThread.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenPhase = g_nativeStartupLastDeferralOpenPhase.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenWidth = g_nativeStartupLastDeferralOpenWidth.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenHeight = g_nativeStartupLastDeferralOpenHeight.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenEnabled = g_nativeStartupLastDeferralOpenEnabled.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenDisableCount = g_nativeStartupLastDeferralOpenDisableCount.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenProjectionXBits = g_nativeStartupLastDeferralOpenProjectionXBits.load(std::memory_order_relaxed);
		out.nativeStartupLastDeferralOpenProjectionYBits = g_nativeStartupLastDeferralOpenProjectionYBits.load(std::memory_order_relaxed);
		out.leftSubmissions = g_leftSubmissions.load(std::memory_order_relaxed);
		out.rightSubmissions = g_rightSubmissions.load(std::memory_order_relaxed);
		out.stereoMatches = g_stereoMatches.load(std::memory_order_relaxed);
		out.stereoMismatches = g_stereoMismatches.load(std::memory_order_relaxed);
		out.phaseCount = PhaseCount();
		out.phase = g_phase.load(std::memory_order_relaxed);
		out.enginePhase = g_enginePhase.load(std::memory_order_relaxed);
		out.distinctPhasesSeen = static_cast<std::uint32_t>(
			std::popcount(maskLow) + std::popcount(maskHigh));
		out.observedPhaseMask[0] = maskLow;
		out.observedPhaseMask[1] = maskHigh;
		out.engineWidth = g_engineWidth.load(std::memory_order_relaxed);
		out.engineHeight = g_engineHeight.load(std::memory_order_relaxed);
		out.enabled = g_enabled.load(std::memory_order_relaxed);
		out.disableCount = g_disableCount.load(std::memory_order_relaxed);
		out.projectionX = LoadFloat(g_projectionXBits);
		out.projectionY = LoadFloat(g_projectionYBits);
		out.jitterX = LoadFloat(g_jitterXBits);
		out.jitterY = LoadFloat(g_jitterYBits);
		out.committedProjectionX = LoadFloat(g_committedProjectionXBits);
		out.committedProjectionY = LoadFloat(g_committedProjectionYBits);
		out.minJitterX = LoadFloat(g_minJitterXBits);
		out.maxJitterX = LoadFloat(g_maxJitterXBits);
		out.minJitterY = LoadFloat(g_minJitterYBits);
		out.maxJitterY = LoadFloat(g_maxJitterYBits);
		out.leftJitterX = LoadFloat(g_leftJitterXBits);
		out.leftJitterY = LoadFloat(g_leftJitterYBits);
		out.rightJitterX = LoadFloat(g_rightJitterXBits);
		out.rightJitterY = LoadFloat(g_rightJitterYBits);
		out.vendorSign = g_vendorSign.load(std::memory_order_relaxed);
		out.lastFailure = g_lastFailure.load(std::memory_order_relaxed);
		out.lastWriteFailure = g_lastWriteFailure.load(std::memory_order_relaxed);
		return out;
	}
}
