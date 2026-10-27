#include "Upscaler/Telemetry.h"

#include "Upscaler/Engine.h"

#include <spdlog/spdlog.h>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

namespace Upscaler::Telemetry
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		constexpr std::size_t kRingSize = 8;
		// Point 0 is Begin; the rest are MarkPoint enumerators in order. One timestamp
		// query per point per slot, and one bit per point in the episode mask.
		constexpr std::size_t kPointCount = static_cast<std::size_t>(MarkPoint::kAfterLateRangeCommit) + 1u;
		// Every consecutive pair, then total.
		constexpr std::size_t kIntervalCount = kPointCount;
		static_assert(kPointCount <= 32, "the episode point mask is 32 bits wide");
		constexpr std::uint32_t kAllPointMask = static_cast<std::uint32_t>((1ull << kPointCount) - 1ull);

		struct QuerySlot
		{
			ComPtr<ID3D11Query> disjoint;
			std::array<ComPtr<ID3D11Query>, kPointCount> timestamps;
			bool pending = false;
			bool valid = false;
		};

		struct ActiveEpisode
		{
			bool active = false;
			std::uint64_t nativeSerial = 0;
			std::int32_t slot = -1;
			std::uint64_t configurationVersion = 0;
			std::uint32_t pointMask = 0;
			std::array<std::int64_t, kPointCount> qpc{};
		};

		struct AtomicStats
		{
			std::atomic<std::uint64_t> count{ 0 };
			std::atomic<std::uint64_t> sumNanoseconds{ 0 };
			std::atomic<std::uint64_t> minNanoseconds{ (std::numeric_limits<std::uint64_t>::max)() };
			std::atomic<std::uint64_t> maxNanoseconds{ 0 };
		};

		struct StatsSnapshot
		{
			std::uint64_t count = 0;
			std::uint64_t sumNanoseconds = 0;
			std::uint64_t minNanoseconds = (std::numeric_limits<std::uint64_t>::max)();
			std::uint64_t maxNanoseconds = 0;
		};

		struct ReportSnapshot
		{
			std::uint64_t sequence = 0;
			// True when the bounded seqlock read gave up: the fields below may straddle
			// one mutation window. See ReadReportSnapshot.
			bool torn = false;
			bool renderEnabled = false;
			Path path = Path::kReduced;
			bool episodeActive = false;
			std::uint64_t renderConfigurationVersion = 0;
			std::uint64_t resetCompletedGeneration = 0;
			std::uint64_t window = 0;
			std::uint64_t begun = 0;
			std::uint64_t committed = 0;
			std::uint64_t aborted = 0;
			std::uint32_t pendingSlots = 0;
			std::uint64_t gpuCompleted = 0;
			std::uint64_t gpuDroppedNoSlot = 0;
			std::uint64_t queryCreateFailures = 0;
			std::uint64_t queryReadFailures = 0;
			std::uint64_t disjointSamples = 0;
			std::uint64_t invalidSamples = 0;
			std::array<StatsSnapshot, kIntervalCount> cpuStats;
			std::array<StatsSnapshot, kIntervalCount> gpuStats;
		};

		std::atomic<Path> g_path{ Path::kReduced };
		std::atomic_bool g_requestedEnabled{ false };
		std::atomic<std::uint64_t> g_configurationVersion{ 0 };
		std::atomic_bool g_renderEnabled{ false };
		std::atomic<std::uint64_t> g_renderConfigurationVersion{ 0 };
		std::atomic_bool g_episodeActive{ false };
		std::atomic_bool g_faultRetirementPending{ false };
		std::atomic<std::uint64_t> g_resetRequestedGeneration{ 0 };
		std::atomic<std::uint64_t> g_resetCompletedGeneration{ 0 };
		std::atomic<std::uint64_t> g_window{ 0 };
		// Even values name stable listener snapshots. The verified render thread is the
		// sole writer; Report retries if it overlaps one of these deliberately tiny
		// atomic-only publication regions. RunGuarded repairs an odd value after SEH.
		std::atomic<std::uint64_t> g_statsSequence{ 0 };
		std::atomic<std::uint32_t> g_renderThreadId{ 0 };
		std::atomic<std::uint64_t> g_threadMismatches{ 0 };
		std::atomic<std::uint64_t> g_begun{ 0 };
		std::atomic<std::uint64_t> g_committed{ 0 };
		std::atomic<std::uint64_t> g_aborted{ 0 };
		std::atomic<std::uint64_t> g_gpuCompleted{ 0 };
		std::atomic<std::uint64_t> g_gpuDroppedNoSlot{ 0 };
		std::atomic<std::uint64_t> g_queryCreateFailures{ 0 };
		std::atomic<std::uint64_t> g_queryReadFailures{ 0 };
		std::atomic<std::uint64_t> g_disjointSamples{ 0 };
		std::atomic<std::uint64_t> g_invalidSamples{ 0 };
		std::atomic_bool g_faulted{ false };
		std::atomic<std::uint64_t> g_instrumentationFaults{ 0 };
		std::atomic<std::uint32_t> g_lastExceptionCode{ 0 };
		std::atomic<std::uint32_t> g_pendingSlots{ 0 };
		std::array<AtomicStats, kIntervalCount> g_cpuStats;
		std::array<AtomicStats, kIntervalCount> g_gpuStats;

		// Render-thread-owned state. Report deliberately has no path to these values.
		std::array<QuerySlot, kRingSize> g_slots;
		ActiveEpisode g_active;
		ComPtr<ID3D11Device> g_device;
		ComPtr<ID3D11DeviceContext> g_context;
		std::uint64_t g_appliedConfigurationVersion = 0;
		std::int64_t g_qpcFrequency = 0;

		[[nodiscard]] constexpr std::array<std::pair<std::size_t, std::size_t>, kIntervalCount> BuildIntervals() noexcept
		{
			std::array<std::pair<std::size_t, std::size_t>, kIntervalCount> intervals{};
			for (std::size_t i = 0; i + 1 < kPointCount; ++i) {
				intervals[i] = { i, i + 1 };
			}
			intervals[kIntervalCount - 1] = { 0u, kPointCount - 1 };
			return intervals;
		}

		constexpr auto kIntervals = BuildIntervals();

		// Indexed by MarkPoint value; markOrder in the report.
		constexpr std::array<const char*, kPointCount> kPointNames{
			"begin",
			"afterEvaluateIsolationBegin",
			"afterLeftColorExtract",
			"afterLeftMotionExtract",
			"afterLeftEvaluate",
			"afterLeftReassembly",
			"afterRightColorExtract",
			"afterRightMotionExtract",
			"afterRightEvaluate",
			"afterRightReassembly",
			"beforeEvaluateIsolationEnd",
			"afterEvaluateIsolationEnd",
			"afterBackupIsolationBegin",
			"afterBackupClearState",
			"afterRtBackups",
			"afterPrivateToEngineCopy",
			"beforeBackupIsolationEnd",
			"afterBackupIsolationEnd",
			"beforeSeamPublishIsolationBegin",
			"afterSeamPublishIsolationBegin",
			"beforeSeamPublishIsolationEnd",
			"afterSeamPublish",
			"afterLateRangeCommit",
		};
		static_assert(std::string_view{ kPointNames.back() } == std::string_view{ "afterLateRangeCommit" },
			"kPointNames must end at MarkPoint::kAfterLateRangeCommit; add the name when adding a point");

		// Interval i is point i to point i+1; the name says what that span contains.
		// "total" must keep its name: the headless runner reads perf.stages.gpu.total.
		constexpr std::array<const char*, kIntervalCount> kIntervalNames{
			"evaluateIsolationBegin",  // swap to the private context state
			"leftColorExtract",        // one per-eye colour CopySubresourceRegion
			"leftMotionExtract",       // raw-motion copy, constants, sanitizer draw
			"leftEvaluate",            // one NGX evaluate
			"leftReassembly",          // one per-eye copy into the private double-wide
			"rightColorExtract",
			"rightMotionExtract",
			"rightEvaluate",
			"rightReassembly",
			"evaluateFrameClose",            // reset acknowledgement and counters, no GPU work
			"evaluateIsolationEnd",          // swap back to the host state
			"backupIsolationBegin",          // device/admission/preflight checks, then the swap
			"backupClearState",              // ClearState on the private state
			"rtBackupsRemoved",              // nothing: the RT1/RT3 backups used to be here
			"privateToEngineCopy",           // double-wide CopyResource, private into RT1
			"backupClose",                   // fault-seam check only, no GPU work
			"backupIsolationEnd",            // swap back to the host state
			"engineHdrToSeamPublish",        // the engine's own HDR/ImageSpace work up to the TemporalAA seam
			"seamPublishIsolationBegin",     // seam validation and the admission recheck, then the swap
			"seamPublishDraw",               // full-screen triangle into history and RT4 in place of the engine's draw
			"seamPublishIsolationEnd",       // swap back, then the hook's bookkeeping
			"engineSeamPublishToLateRange",  // the engine's own work up to the late-range commit; slot 21 is inactive
			"total",
		};
		static_assert(std::string_view{ kIntervalNames.back() } == std::string_view{ "total" },
			"kIntervalNames must end with total; add the interval name when adding a point");

		using GuardedOperation = void (*)(void*) noexcept;

		void BeginStatsMutation() noexcept
		{
			g_statsSequence.fetch_add(1, std::memory_order_acq_rel);
		}

		void EndStatsMutation() noexcept
		{
			g_statsSequence.fetch_add(1, std::memory_order_release);
		}

		void RepairStatsSequenceAfterFault() noexcept
		{
			auto sequence = g_statsSequence.load(std::memory_order_acquire);
			while ((sequence & 1u) != 0 &&
				   !g_statsSequence.compare_exchange_weak(
					   sequence, sequence + 1u, std::memory_order_release, std::memory_order_acquire)) {
			}
		}

		[[nodiscard]] bool RunGuarded(GuardedOperation a_operation, void* a_context) noexcept
		{
			bool returned = false;
			std::uint32_t exceptionCode = 0;
			__try {
				a_operation(a_context);
				returned = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (!returned) {
				RepairStatsSequenceAfterFault();
				g_lastExceptionCode.store(exceptionCode, std::memory_order_relaxed);
				g_instrumentationFaults.fetch_add(1, std::memory_order_relaxed);
				g_faulted.store(true, std::memory_order_release);
				BeginStatsMutation();
				g_renderEnabled.store(false, std::memory_order_release);
				EndStatsMutation();
				g_faultRetirementPending.store(true, std::memory_order_release);
			}
			return returned;
		}

		void AtomicMin(std::atomic<std::uint64_t>& a_target, std::uint64_t a_value) noexcept
		{
			auto current = a_target.load(std::memory_order_relaxed);
			while (a_value < current &&
				   !a_target.compare_exchange_weak(current, a_value, std::memory_order_relaxed)) {
			}
		}

		void AtomicMax(std::atomic<std::uint64_t>& a_target, std::uint64_t a_value) noexcept
		{
			auto current = a_target.load(std::memory_order_relaxed);
			while (a_value > current &&
				   !a_target.compare_exchange_weak(current, a_value, std::memory_order_relaxed)) {
			}
		}

		void AddSampleUnlocked(AtomicStats& a_stats, std::uint64_t a_nanoseconds) noexcept
		{
			a_stats.sumNanoseconds.fetch_add(a_nanoseconds, std::memory_order_relaxed);
			AtomicMin(a_stats.minNanoseconds, a_nanoseconds);
			AtomicMax(a_stats.maxNanoseconds, a_nanoseconds);
			a_stats.count.fetch_add(1, std::memory_order_release);
		}

		[[nodiscard]] std::uint64_t ScaleToNanoseconds(
			std::uint64_t a_delta,
			std::uint64_t a_frequency) noexcept
		{
			if (a_frequency == 0) {
				return 0;
			}
			const long double value =
				static_cast<long double>(a_delta) * 1000000000.0L /
				static_cast<long double>(a_frequency);
			if (!std::isfinite(value) || value < 0.0L ||
				value > static_cast<long double>((std::numeric_limits<std::uint64_t>::max)())) {
				return 0;
			}
			return static_cast<std::uint64_t>(value + 0.5L);
		}

		void ResetStatsUnlocked() noexcept
		{
			g_begun.store(0, std::memory_order_relaxed);
			g_committed.store(0, std::memory_order_relaxed);
			g_aborted.store(0, std::memory_order_relaxed);
			g_gpuCompleted.store(0, std::memory_order_relaxed);
			g_gpuDroppedNoSlot.store(0, std::memory_order_relaxed);
			g_queryCreateFailures.store(0, std::memory_order_relaxed);
			g_queryReadFailures.store(0, std::memory_order_relaxed);
			g_disjointSamples.store(0, std::memory_order_relaxed);
			g_invalidSamples.store(0, std::memory_order_relaxed);
			for (auto* group : { &g_cpuStats, &g_gpuStats }) {
				for (auto& stats : *group) {
					stats.count.store(0, std::memory_order_relaxed);
					stats.sumNanoseconds.store(0, std::memory_order_relaxed);
					stats.minNanoseconds.store(
						(std::numeric_limits<std::uint64_t>::max)(),
						std::memory_order_relaxed);
					stats.maxNanoseconds.store(0, std::memory_order_relaxed);
				}
			}
		}

		void ResetQueries() noexcept
		{
			g_active = {};
			for (auto& slot : g_slots) {
				slot = {};
			}
			g_context.Reset();
			g_device.Reset();
			BeginStatsMutation();
			g_episodeActive.store(false, std::memory_order_release);
			g_pendingSlots.store(0, std::memory_order_release);
			EndStatsMutation();
		}

		[[nodiscard]] bool OnRenderThread() noexcept
		{
			const auto current = static_cast<std::uint32_t>(::GetCurrentThreadId());
			auto expected = std::uint32_t{ 0 };
			if (g_renderThreadId.compare_exchange_strong(
					expected, current, std::memory_order_acq_rel)) {
				return true;
			}
			if (expected != current) {
				g_threadMismatches.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			return true;
		}

		void CloseActiveQuery(bool a_valid) noexcept
		{
			if (g_active.slot < 0 ||
				static_cast<std::size_t>(g_active.slot) >= g_slots.size()) {
				return;
			}
			auto& slot = g_slots[static_cast<std::size_t>(g_active.slot)];
			if (g_context && slot.disjoint) {
				g_context->End(slot.disjoint.Get());
				slot.pending = true;
				slot.valid = a_valid;
				BeginStatsMutation();
				g_pendingSlots.fetch_add(1, std::memory_order_release);
				EndStatsMutation();
			} else {
				slot.pending = false;
				slot.valid = false;
			}
		}

		void AbortActiveForReconfiguration() noexcept
		{
			if (!g_active.active) {
				return;
			}
			CloseActiveQuery(false);
			g_active = {};
			BeginStatsMutation();
			g_episodeActive.store(false, std::memory_order_release);
			g_aborted.fetch_add(1, std::memory_order_relaxed);
			EndStatsMutation();
		}

		void ApplyConfiguration() noexcept
		{
			const auto requestedVersion = g_configurationVersion.load(std::memory_order_acquire);
			if (requestedVersion == g_appliedConfigurationVersion) {
				return;
			}
			AbortActiveForReconfiguration();
			ResetQueries();
			g_appliedConfigurationVersion = requestedVersion;
			const bool enabled = g_requestedEnabled.load(std::memory_order_acquire);
			BeginStatsMutation();
			ResetStatsUnlocked();
			g_renderEnabled.store(enabled, std::memory_order_release);
			g_renderConfigurationVersion.store(requestedVersion, std::memory_order_release);
			g_resetCompletedGeneration.store(
				g_resetRequestedGeneration.load(std::memory_order_acquire),
				std::memory_order_release);
			g_window.fetch_add(1, std::memory_order_release);
			EndStatsMutation();
		}

		[[nodiscard]] bool BindCurrentDevice() noexcept
		{
			auto* const device = Engine::Device();
			auto* const context = Engine::ImmediateContext();
			if (!device || !context || FAILED(device->GetDeviceRemovedReason())) {
				return false;
			}
			if ((g_device && g_device.Get() != device) || (g_context && g_context.Get() != context)) {
				AbortActiveForReconfiguration();
				ResetQueries();
				BeginStatsMutation();
				g_invalidSamples.fetch_add(1, std::memory_order_relaxed);
				EndStatsMutation();
			}
			if (!g_device) {
				g_device = device;
			}
			if (!g_context) {
				g_context = context;
			}
			return true;
		}

		[[nodiscard]] bool EnsureQueries(QuerySlot& a_slot) noexcept
		{
			if (a_slot.disjoint && a_slot.timestamps[0]) {
				return true;
			}
			if (!g_device) {
				return false;
			}
			a_slot = {};
			D3D11_QUERY_DESC desc{};
			desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
			if (FAILED(g_device->CreateQuery(&desc, a_slot.disjoint.GetAddressOf())) ||
				!a_slot.disjoint) {
				a_slot = {};
				BeginStatsMutation();
				g_queryCreateFailures.fetch_add(1, std::memory_order_relaxed);
				EndStatsMutation();
				return false;
			}
			desc.Query = D3D11_QUERY_TIMESTAMP;
			for (auto& timestamp : a_slot.timestamps) {
				if (FAILED(g_device->CreateQuery(&desc, timestamp.GetAddressOf())) || !timestamp) {
					a_slot = {};
					BeginStatsMutation();
					g_queryCreateFailures.fetch_add(1, std::memory_order_relaxed);
					EndStatsMutation();
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] QuerySlot* FindFreeSlot(std::int32_t& a_index) noexcept
		{
			for (std::size_t i = 0; i < g_slots.size(); ++i) {
				if (!g_slots[i].pending) {
					a_index = static_cast<std::int32_t>(i);
					return &g_slots[i];
				}
			}
			return nullptr;
		}

		[[nodiscard]] bool BuildCpuEpisode(
			std::array<std::uint64_t, kIntervalCount>& a_samples) noexcept
		{
			if (g_qpcFrequency <= 0) {
				return false;
			}
			for (std::size_t i = 0; i < kIntervals.size(); ++i) {
				const auto [begin, end] = kIntervals[i];
				if (g_active.qpc[end] < g_active.qpc[begin]) {
					return false;
				}
				a_samples[i] = ScaleToNanoseconds(
					static_cast<std::uint64_t>(g_active.qpc[end] - g_active.qpc[begin]),
					static_cast<std::uint64_t>(g_qpcFrequency));
			}
			return true;
		}

		void FreePendingSlotUnlocked(QuerySlot& a_slot) noexcept
		{
			a_slot.pending = false;
			a_slot.valid = false;
			g_pendingSlots.fetch_sub(1, std::memory_order_release);
		}

		void CollectSlot(QuerySlot& a_slot) noexcept
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			const HRESULT disjointResult = g_context->GetData(
				a_slot.disjoint.Get(),
				&disjoint,
				sizeof(disjoint),
				D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (disjointResult == S_FALSE) {
				return;
			}
			if (FAILED(disjointResult)) {
				BeginStatsMutation();
				g_queryReadFailures.fetch_add(1, std::memory_order_relaxed);
				FreePendingSlotUnlocked(a_slot);
				EndStatsMutation();
				return;
			}
			if (!a_slot.valid) {
				BeginStatsMutation();
				FreePendingSlotUnlocked(a_slot);
				EndStatsMutation();
				return;
			}
			if (disjoint.Disjoint || disjoint.Frequency == 0) {
				BeginStatsMutation();
				g_disjointSamples.fetch_add(1, std::memory_order_relaxed);
				FreePendingSlotUnlocked(a_slot);
				EndStatsMutation();
				return;
			}

			std::array<std::uint64_t, kPointCount> timestamps{};
			for (std::size_t i = 0; i < timestamps.size(); ++i) {
				const HRESULT result = g_context->GetData(
					a_slot.timestamps[i].Get(),
					&timestamps[i],
					sizeof(timestamps[i]),
					D3D11_ASYNC_GETDATA_DONOTFLUSH);
				if (result == S_FALSE) {
					return;
				}
				if (FAILED(result)) {
					BeginStatsMutation();
					g_queryReadFailures.fetch_add(1, std::memory_order_relaxed);
					FreePendingSlotUnlocked(a_slot);
					EndStatsMutation();
					return;
				}
			}

			std::array<std::uint64_t, kIntervalCount> samples{};
			for (std::size_t i = 0; i < kIntervals.size(); ++i) {
				const auto [begin, end] = kIntervals[i];
				if (timestamps[end] < timestamps[begin]) {
					BeginStatsMutation();
					g_invalidSamples.fetch_add(1, std::memory_order_relaxed);
					FreePendingSlotUnlocked(a_slot);
					EndStatsMutation();
					return;
				}
				samples[i] = ScaleToNanoseconds(
					timestamps[end] - timestamps[begin],
					disjoint.Frequency);
			}
			BeginStatsMutation();
			for (std::size_t i = 0; i < samples.size(); ++i) {
				AddSampleUnlocked(g_gpuStats[i], samples[i]);
			}
			g_gpuCompleted.fetch_add(1, std::memory_order_relaxed);
			FreePendingSlotUnlocked(a_slot);
			EndStatsMutation();
		}

		void CompleteResetIfQuiescent() noexcept
		{
			const auto requested = g_resetRequestedGeneration.load(std::memory_order_acquire);
			if (requested == g_resetCompletedGeneration.load(std::memory_order_acquire) ||
				g_active.active || g_pendingSlots.load(std::memory_order_acquire) != 0) {
				return;
			}
			BeginStatsMutation();
			ResetStatsUnlocked();
			g_window.fetch_add(1, std::memory_order_release);
			g_resetCompletedGeneration.store(requested, std::memory_order_release);
			EndStatsMutation();
		}

		[[nodiscard]] std::string Number(double a_value)
		{
			char buffer[64]{};
			std::snprintf(buffer, sizeof(buffer), "%.6f", a_value);
			return buffer;
		}

		[[nodiscard]] StatsSnapshot ReadStats(const AtomicStats& a_stats) noexcept
		{
			return {
				a_stats.count.load(std::memory_order_relaxed),
				a_stats.sumNanoseconds.load(std::memory_order_relaxed),
				a_stats.minNanoseconds.load(std::memory_order_relaxed),
				a_stats.maxNanoseconds.load(std::memory_order_relaxed)
			};
		}

		// How many times a seqlock read retries before it gives up and hands back what it
		// last saw. Bounded deliberately, and this is the whole reason: the sole writer of
		// g_statsSequence is the verified render thread, and since the panel arrived this
		// reader runs on that thread too. An unbounded retry is therefore a hang with no
		// timeout on the frame producer the moment any caller reads a snapshot from inside
		// a mutation window this same thread opened - the loop waits for itself. No call
		// site does that today: every Begin/EndStatsMutation region is atomic-only, calls
		// nothing and cannot contain a Report or an IntervalSummary. The bound is what
		// makes that a stale line on a diagnostic panel if it ever stops being true,
		// instead of a frozen headset. Sixty-four is far past what a genuine cross-thread
		// overlap needs: the writer's window is a handful of relaxed stores.
		constexpr int kSnapshotAttempts = 64;

		// A read that exhausts the bound sets `torn` and hands back the last fields it
		// managed to load, which /perf prints as "snapshotTorn": the values beside it may
		// straddle one mutation. A caller that cares reads again; nothing today ever sees
		// it true, because nothing today can make the loop retry more than once or twice.
		[[nodiscard]] ReportSnapshot ReadReportSnapshot() noexcept
		{
			ReportSnapshot snapshot{};
			snapshot.torn = true;
			for (int attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
				const auto before = g_statsSequence.load(std::memory_order_acquire);
				if ((before & 1u) != 0) {
					snapshot.sequence = before;
					continue;
				}
				snapshot.path = g_path.load(std::memory_order_relaxed);
				snapshot.renderEnabled = g_renderEnabled.load(std::memory_order_relaxed);
				snapshot.episodeActive = g_episodeActive.load(std::memory_order_relaxed);
				snapshot.renderConfigurationVersion =
					g_renderConfigurationVersion.load(std::memory_order_relaxed);
				snapshot.resetCompletedGeneration =
					g_resetCompletedGeneration.load(std::memory_order_relaxed);
				snapshot.window = g_window.load(std::memory_order_relaxed);
				snapshot.begun = g_begun.load(std::memory_order_relaxed);
				snapshot.committed = g_committed.load(std::memory_order_relaxed);
				snapshot.aborted = g_aborted.load(std::memory_order_relaxed);
				snapshot.pendingSlots = g_pendingSlots.load(std::memory_order_relaxed);
				snapshot.gpuCompleted = g_gpuCompleted.load(std::memory_order_relaxed);
				snapshot.gpuDroppedNoSlot = g_gpuDroppedNoSlot.load(std::memory_order_relaxed);
				snapshot.queryCreateFailures = g_queryCreateFailures.load(std::memory_order_relaxed);
				snapshot.queryReadFailures = g_queryReadFailures.load(std::memory_order_relaxed);
				snapshot.disjointSamples = g_disjointSamples.load(std::memory_order_relaxed);
				snapshot.invalidSamples = g_invalidSamples.load(std::memory_order_relaxed);
				for (std::size_t i = 0; i < kIntervalCount; ++i) {
					snapshot.cpuStats[i] = ReadStats(g_cpuStats[i]);
					snapshot.gpuStats[i] = ReadStats(g_gpuStats[i]);
				}
				const auto after = g_statsSequence.load(std::memory_order_acquire);
				snapshot.sequence = after;
				if (before == after && (after & 1u) == 0) {
					snapshot.torn = false;
					return snapshot;
				}
			}
			return snapshot;
		}

		[[nodiscard]] std::string StatsJson(const StatsSnapshot& a_stats)
		{
			const auto count = a_stats.count;
			const auto sum = a_stats.sumNanoseconds;
			const auto min = a_stats.minNanoseconds;
			const auto max = a_stats.maxNanoseconds;
			constexpr double divisor = 1000000.0;
			std::string out = "{\"count\":" + std::to_string(count);
			out += ",\"meanMs\":" + Number(count == 0 ? 0.0 :
														static_cast<double>(sum) / static_cast<double>(count) / divisor);
			out += ",\"minMs\":" + Number(count == 0 || min == (std::numeric_limits<std::uint64_t>::max)() ?
											  0.0 :
											  static_cast<double>(min) / divisor);
			out += ",\"maxMs\":" + Number(count == 0 ? 0.0 : static_cast<double>(max) / divisor);
			out += "}";
			return out;
		}
	}

	void Configure(bool a_enabled) noexcept
	{
		const bool previous = g_requestedEnabled.exchange(a_enabled, std::memory_order_acq_rel);
		if (previous != a_enabled) {
			g_configurationVersion.fetch_add(1, std::memory_order_release);
		}
	}

	std::uint64_t RequestReset() noexcept
	{
		return g_resetRequestedGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	void BeginImpl(Path a_path) noexcept
	{
		if (!OnRenderThread()) {
			return;
		}
		ApplyConfiguration();
		CompleteResetIfQuiescent();
		if (!g_renderEnabled.load(std::memory_order_acquire) || g_active.active ||
			g_resetRequestedGeneration.load(std::memory_order_acquire) !=
				g_resetCompletedGeneration.load(std::memory_order_acquire)) {
			return;
		}
		if (g_path.load(std::memory_order_relaxed) != a_path) {
			ResetQueries();
			BeginStatsMutation();
			ResetStatsUnlocked();
			g_path.store(a_path, std::memory_order_relaxed);
			g_window.fetch_add(1, std::memory_order_release);
			EndStatsMutation();
		}
		if (!BindCurrentDevice()) {
			return;
		}
		if (g_qpcFrequency <= 0) {
			LARGE_INTEGER frequency{};
			if (!::QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
				return;
			}
			g_qpcFrequency = frequency.QuadPart;
		}

		g_active = {};
		g_active.active = true;
		g_active.configurationVersion = g_appliedConfigurationVersion;
		LARGE_INTEGER now{};
		::QueryPerformanceCounter(&now);
		g_active.qpc[0] = now.QuadPart;
		g_active.pointMask = 1u;
		BeginStatsMutation();
		g_episodeActive.store(true, std::memory_order_release);
		g_begun.fetch_add(1, std::memory_order_relaxed);
		EndStatsMutation();

		std::int32_t slotIndex = -1;
		auto* const slot = FindFreeSlot(slotIndex);
		if (!slot || !EnsureQueries(*slot)) {
			BeginStatsMutation();
			g_gpuDroppedNoSlot.fetch_add(1, std::memory_order_relaxed);
			EndStatsMutation();
			return;
		}
		g_active.slot = slotIndex;
		g_context->Begin(slot->disjoint.Get());
		g_context->End(slot->timestamps[0].Get());
	}

	void MarkImpl(MarkPoint a_point) noexcept
	{
		if (!OnRenderThread()) {
			return;
		}
		ApplyConfiguration();
		if (!g_active.active ||
			g_active.configurationVersion != g_appliedConfigurationVersion) {
			return;
		}
		const auto point = static_cast<std::size_t>(a_point);
		if (point == 0 || point >= kPointCount) {
			return;
		}
		const auto priorBit = 1u << (point - 1u);
		const auto pointBit = 1u << point;
		if ((g_active.pointMask & priorBit) == 0 ||
			(g_active.pointMask & pointBit) != 0) {
			return;
		}
		LARGE_INTEGER now{};
		::QueryPerformanceCounter(&now);
		g_active.qpc[point] = now.QuadPart;
		g_active.pointMask |= pointBit;
		if (g_active.slot >= 0 && g_context) {
			auto& slot = g_slots[static_cast<std::size_t>(g_active.slot)];
			g_context->End(slot.timestamps[point].Get());
		}
	}

	void EndImpl() noexcept
	{
		if (!OnRenderThread()) {
			return;
		}
		ApplyConfiguration();
		if (!g_active.active) {
			return;
		}
		const bool complete =
			g_active.configurationVersion == g_appliedConfigurationVersion &&
			g_active.pointMask == kAllPointMask;
		std::array<std::uint64_t, kIntervalCount> cpuSamples{};
		const bool cpuValid = complete && BuildCpuEpisode(cpuSamples);
		CloseActiveQuery(complete);
		g_active = {};
		BeginStatsMutation();
		g_episodeActive.store(false, std::memory_order_release);
		if (complete) {
			g_committed.fetch_add(1, std::memory_order_relaxed);
			if (cpuValid) {
				for (std::size_t i = 0; i < cpuSamples.size(); ++i) {
					AddSampleUnlocked(g_cpuStats[i], cpuSamples[i]);
				}
			} else {
				g_invalidSamples.fetch_add(1, std::memory_order_relaxed);
			}
		} else {
			g_aborted.fetch_add(1, std::memory_order_relaxed);
		}
		EndStatsMutation();
	}

	void AbortImpl() noexcept
	{
		if (!OnRenderThread()) {
			return;
		}
		ApplyConfiguration();
		if (!g_active.active) {
			return;
		}
		CloseActiveQuery(false);
		g_active = {};
		BeginStatsMutation();
		g_episodeActive.store(false, std::memory_order_release);
		g_aborted.fetch_add(1, std::memory_order_relaxed);
		EndStatsMutation();
	}

	void CollectImpl() noexcept
	{
		if (!OnRenderThread()) {
			return;
		}
		ApplyConfiguration();
		if (!g_renderEnabled.load(std::memory_order_acquire)) {
			CompleteResetIfQuiescent();
			return;
		}
		if (!BindCurrentDevice() || !g_context) {
			return;
		}
		for (auto& slot : g_slots) {
			if (slot.pending) {
				CollectSlot(slot);
			}
		}
		CompleteResetIfQuiescent();
	}

	void RetireFaultedStateImpl() noexcept
	{
		if (!OnRenderThread()) {
			return;
		}
		// A telemetry exception makes the exact query phase unknowable. Do not issue an
		// immediate-context End from that uncertain state. Releasing only telemetry-owned
		// COM references on the verified render thread retires the instrumentation without
		// entering, rolling back, or otherwise touching the Quality transaction.
		ResetQueries();
		const auto requestedVersion = g_configurationVersion.load(std::memory_order_acquire);
		g_appliedConfigurationVersion = requestedVersion;
		BeginStatsMutation();
		g_renderEnabled.store(false, std::memory_order_release);
		g_renderConfigurationVersion.store(requestedVersion, std::memory_order_release);
		g_resetCompletedGeneration.store(
			g_resetRequestedGeneration.load(std::memory_order_acquire),
			std::memory_order_release);
		g_window.fetch_add(1, std::memory_order_release);
		EndStatsMutation();
		g_faultRetirementPending.store(false, std::memory_order_release);
	}

	namespace
	{
		void BeginThunk(void* a_path) noexcept { BeginImpl(*static_cast<const Path*>(a_path)); }
		void EndThunk(void*) noexcept { EndImpl(); }
		void AbortThunk(void*) noexcept { AbortImpl(); }
		void CollectThunk(void*) noexcept { CollectImpl(); }
		void RetireFaultedStateThunk(void*) noexcept { RetireFaultedStateImpl(); }

		struct MarkOperation
		{
			MarkPoint point{};
		};

		void MarkThunk(void* a_context) noexcept
		{
			const auto* const operation = static_cast<const MarkOperation*>(a_context);
			if (operation) {
				MarkImpl(operation->point);
			}
		}

		[[nodiscard]] bool DisabledAndSettled() noexcept
		{
			return !g_requestedEnabled.load(std::memory_order_acquire) &&
			       !g_renderEnabled.load(std::memory_order_acquire) &&
			       !g_episodeActive.load(std::memory_order_acquire) &&
			       g_pendingSlots.load(std::memory_order_acquire) == 0 &&
			       g_configurationVersion.load(std::memory_order_acquire) ==
			           g_renderConfigurationVersion.load(std::memory_order_acquire) &&
			       g_resetRequestedGeneration.load(std::memory_order_acquire) ==
			           g_resetCompletedGeneration.load(std::memory_order_acquire);
		}
	}

	void Begin(Path a_path) noexcept
	{
		if (g_faulted.load(std::memory_order_acquire) || DisabledAndSettled()) {
			return;
		}
		static_cast<void>(RunGuarded(BeginThunk, &a_path));
	}

	void ExpectNativeSeam(std::uint64_t a_serial) noexcept
	{
		if (g_episodeActive.load(std::memory_order_acquire) && OnRenderThread() &&
			g_path.load(std::memory_order_relaxed) == Path::kNative) {
			g_active.nativeSerial = a_serial;
		}
	}

	bool NativePending(std::uint64_t a_serial) noexcept
	{
		return g_episodeActive.load(std::memory_order_acquire) && OnRenderThread() &&
		       g_path.load(std::memory_order_relaxed) == Path::kNative &&
		       a_serial != 0 && g_active.nativeSerial == a_serial;
	}

	void EndNative(std::uint64_t a_serial) noexcept
	{
		if (NativePending(a_serial)) {
			Mark(MarkPoint::kAfterSeamPublish);
			Mark(MarkPoint::kAfterLateRangeCommit);
			End();
		}
	}

	void AbortNative() noexcept
	{
		if (g_path.load(std::memory_order_relaxed) == Path::kNative) {
			Abort();
		}
	}

	void Mark(MarkPoint a_point) noexcept
	{
		if (a_point == MarkPoint::kNone ||
			g_faulted.load(std::memory_order_acquire) ||
			!g_episodeActive.load(std::memory_order_acquire)) {
			return;
		}
		MarkOperation operation{ a_point };
		static_cast<void>(RunGuarded(MarkThunk, &operation));
	}

	void End() noexcept
	{
		if (g_faulted.load(std::memory_order_acquire) ||
			!g_episodeActive.load(std::memory_order_acquire)) {
			return;
		}
		static_cast<void>(RunGuarded(EndThunk, nullptr));
	}

	void Abort() noexcept
	{
		if (g_faulted.load(std::memory_order_acquire) ||
			!g_episodeActive.load(std::memory_order_acquire)) {
			return;
		}
		static_cast<void>(RunGuarded(AbortThunk, nullptr));
	}

	void Collect() noexcept
	{
		if (g_faultRetirementPending.load(std::memory_order_acquire)) {
			static_cast<void>(RunGuarded(RetireFaultedStateThunk, nullptr));
			return;
		}
		if (g_faulted.load(std::memory_order_acquire) || DisabledAndSettled()) {
			return;
		}
		static_cast<void>(RunGuarded(CollectThunk, nullptr));
	}

	void LogReport() noexcept
	{
		if (!Enabled() || !OnRenderThread() || g_episodeActive.load(std::memory_order_acquire)) {
			return;
		}
		static std::uint64_t s_window = 0;
		static std::uint64_t s_lastMs = 0;
		static unsigned s_reports = 0;
		const auto now = ::GetTickCount64();
		const auto window = g_window.load(std::memory_order_acquire);
		if (s_window != window) {
			s_window = window;
			s_lastMs = now;
			s_reports = 0;
		}
		if (s_reports >= 6 || now - s_lastMs < 10000) {
			return;
		}
		s_lastMs = now;
		++s_reports;
		try {
			spdlog::info("perf report: {}", Report());
		} catch (...) {}
	}

	std::string Report()
	{
		const auto snapshot = ReadReportSnapshot();
		std::string out = "{\"requestedEnabled\":";
		out += g_requestedEnabled.load(std::memory_order_acquire) ? "true" : "false";
		out += ",\"renderEnabled\":";
		out += snapshot.renderEnabled ? "true" : "false";
		out += ",\"path\":\"";
		out += snapshot.path == Path::kNative ? "nativeDlaa" : "reducedNgx";
		out += "\"";
		out += ",\"episodeActive\":";
		out += snapshot.episodeActive ? "true" : "false";
		out += ",\"configurationVersion\":" +
		       std::to_string(g_configurationVersion.load(std::memory_order_acquire));
		out += ",\"renderConfigurationVersion\":" +
		       std::to_string(snapshot.renderConfigurationVersion);
		out += ",\"resetRequestedGeneration\":" +
		       std::to_string(g_resetRequestedGeneration.load(std::memory_order_acquire));
		out += ",\"resetCompletedGeneration\":" +
		       std::to_string(snapshot.resetCompletedGeneration);
		out += ",\"window\":" + std::to_string(snapshot.window);
		out += ",\"snapshotSequence\":" +
		       std::to_string(snapshot.sequence);
		out += ",\"snapshotTorn\":";
		out += snapshot.torn ? "true" : "false";
		out += ",\"renderThreadId\":" + std::to_string(g_renderThreadId.load(std::memory_order_acquire));
		out += ",\"threadMismatches\":" + std::to_string(g_threadMismatches.load(std::memory_order_relaxed));
		out += ",\"instrumentationFaulted\":";
		out += g_faulted.load(std::memory_order_acquire) ? "true" : "false";
		out += ",\"faultRetirementPending\":";
		out += g_faultRetirementPending.load(std::memory_order_acquire) ? "true" : "false";
		out += ",\"instrumentationFaults\":" + std::to_string(g_instrumentationFaults.load(std::memory_order_relaxed));
		out += ",\"lastExceptionCode\":" + std::to_string(g_lastExceptionCode.load(std::memory_order_relaxed));
		out += ",\"episodes\":{\"begun\":" + std::to_string(snapshot.begun);
		out += ",\"committed\":" + std::to_string(snapshot.committed);
		out += ",\"aborted\":" + std::to_string(snapshot.aborted);
		out += "},\"gpuQueries\":{\"ringSize\":" + std::to_string(kRingSize);
		out += ",\"pending\":" + std::to_string(snapshot.pendingSlots);
		out += ",\"completed\":" + std::to_string(snapshot.gpuCompleted);
		out += ",\"droppedNoSlot\":" + std::to_string(snapshot.gpuDroppedNoSlot);
		out += ",\"createFailures\":" + std::to_string(snapshot.queryCreateFailures);
		out += ",\"readFailures\":" + std::to_string(snapshot.queryReadFailures);
		out += ",\"disjoint\":" + std::to_string(snapshot.disjointSamples);
		out += ",\"invalid\":" + std::to_string(snapshot.invalidSamples);
		out += ",\"timestampsPerSlot\":" + std::to_string(kPointCount);
		out += ",\"getDataFlags\":\"D3D11_ASYNC_GETDATA_DONOTFLUSH\"}";
		out += ",\"markOrder\":[";
		for (std::size_t i = 0; i < kPointCount; ++i) {
			if (i != 0) {
				out += ",";
			}
			out += "\"" + std::string{ snapshot.path == Path::kNative && i == kPointCount - 1 ? "afterNativeSeamComplete" : kPointNames[i] } + "\"";
		}
		out += "]";
		out += ",\"stages\":{\"cpu\":{";
		for (std::size_t i = 0; i < kIntervalCount; ++i) {
			if (i != 0) {
				out += ",";
			}
			out += "\"" + std::string{ snapshot.path == Path::kNative && i == kIntervalCount - 2 ? "nativeSeamClose" : kIntervalNames[i] } + "\":" + StatsJson(snapshot.cpuStats[i]);
		}
		out += "},\"gpu\":{";
		for (std::size_t i = 0; i < kIntervalCount; ++i) {
			if (i != 0) {
				out += ",";
			}
			out += "\"" + std::string{ snapshot.path == Path::kNative && i == kIntervalCount - 2 ? "nativeSeamClose" : kIntervalNames[i] } + "\":" + StatsJson(snapshot.gpuStats[i]);
		}
		out += "}}}";
		return out;
	}

	bool Enabled() noexcept
	{
		return g_requestedEnabled.load(std::memory_order_acquire);
	}

	std::string IntervalSummary()
	{
		const auto snapshot = ReadReportSnapshot();
		std::string out;
		// One column layout, not a table widget: the drawn font is fixed width, so the
		// name is padded to a column and the two numbers follow it. Thirty is the longest
		// name in kIntervalNames plus a little; a name past that just costs one space.
		constexpr std::size_t kNameColumn = 30;
		for (std::size_t i = 0; i < kIntervalCount; ++i) {
			const auto& cpu = snapshot.cpuStats[i];
			if (cpu.count == 0) {
				// An interval with no samples has nothing to price. Skipping it is what
				// keeps this short enough to read in a headset: a window that has not
				// committed an episode prints nothing at all rather than 23 zeroes.
				continue;
			}
			std::string name{ snapshot.path == Path::kNative && i == kIntervalCount - 2 ? "nativeSeamClose" : kIntervalNames[i] };
			if (name.size() < kNameColumn) {
				name.append(kNameColumn - name.size(), ' ');
			}
			out += name;
			out += "cpu ";
			out += Number(static_cast<double>(cpu.sumNanoseconds) /
						  static_cast<double>(cpu.count) / 1000000.0);
			const auto& gpu = snapshot.gpuStats[i];
			out += "   gpu ";
			out += gpu.count == 0 ?
			           std::string{ "-" } :
			           Number(static_cast<double>(gpu.sumNanoseconds) /
							  static_cast<double>(gpu.count) / 1000000.0);
			out += '\n';
		}
		if (out.empty()) {
			out = "no committed episode in this window yet";
		}
		return out;
	}
}
