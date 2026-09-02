#include "Upscaler/SamplerBias.h"

#include "Upscaler/Backend/IBackend.h"

#include <spdlog/spdlog.h>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>

namespace Upscaler::SamplerBias
{
	namespace
	{
		// The table image and everything derived from it is render-thread only. The
		// atomics below exist for Snapshot, which any thread may call and which reads
		// counters rather than pointers.
		ID3D11SamplerState* g_originals[kEntryCount] = {};
		ID3D11SamplerState* g_duplicates[kEntryCount] = {};
		ID3D11SamplerState* g_image[kEntryCount] = {};
		D3D11_SAMPLER_DESC g_descs[kEntryCount] = {};
		bool g_marked[kEntryCount] = {};

		// The marked entries are contiguous per (address mode, filter) pair, so the swap
		// is a handful of memcpy rather than 320 stores. One run per marked pair.
		struct Run
		{
			std::size_t begin = 0;
			std::size_t count = 0;
		};

		Run g_runs[kAddressModeCount * kFilterCount] = {};
		std::size_t g_runCount = 0;

		std::atomic<std::uintptr_t> g_tableBase{ 0 };
		std::atomic<ID3D11Device*> g_device{ nullptr };
		std::atomic<bool> g_initialized{ false };
		std::atomic<bool> g_originalsCached{ false };
		std::atomic<bool> g_built{ false };
		std::atomic<bool> g_applied{ false };
		std::atomic<bool> g_overrideActive{ false };
		std::atomic<bool> g_includeTrilinear{ false };
		std::atomic<bool> g_lowerMinLod{ false };
		std::atomic<std::uint32_t> g_histogram[kFilterCount] = {};
		std::atomic<std::uint32_t> g_histogramOther{ 0 };
		std::atomic<std::uint32_t> g_entriesBiased{ 0 };
		std::atomic<std::uint32_t> g_samplersCreated{ 0 };
		std::atomic<std::uint32_t> g_retainedSamplers{ 0 };
		std::atomic<std::uint32_t> g_builtRenderWidth{ 0 };
		std::atomic<std::uint32_t> g_builtOutputWidth{ 0 };
		std::atomic<std::uint32_t> g_builtBiasBits{ 0 };
		std::atomic<std::uint32_t> g_bandLowBits{ 0 };
		std::atomic<std::uint32_t> g_bandHighBits{ 0 };
		std::atomic<std::uint64_t> g_builds{ 0 };
		std::atomic<std::uint64_t> g_buildDeclines{ 0 };
		std::atomic<std::uint64_t> g_appliedFrames{ 0 };
		std::atomic<std::uint64_t> g_restoredFrames{ 0 };
		std::atomic<std::uint64_t> g_applyDeclines{ 0 };
		std::atomic<std::uint64_t> g_unbalanced{ 0 };
		std::atomic<std::uint64_t> g_bandWarnings{ 0 };
		std::atomic<Failure> g_lastFailure{ Failure::kNone };

		constexpr std::uint32_t kExpectedFilters[kFilterCount] = {
			kFilterPoint,
			kFilterLinearMipPoint,
			kFilterTrilinear,
			kFilterAnisotropic,
			kFilterComparison,
		};

		// 64 of each: the creation loop visits every (address mode, mip) pair once per
		// filter index, and that is 4 * 16.
		constexpr std::uint32_t kExpectedFilterCount = static_cast<std::uint32_t>(kAddressModeCount * kMipCount);

		[[nodiscard]] float LoadFloat(const std::atomic<std::uint32_t>& a_bits) noexcept
		{
			return std::bit_cast<float>(a_bits.load(std::memory_order_relaxed));
		}

		void StoreFloat(std::atomic<std::uint32_t>& a_bits, float a_value) noexcept
		{
			a_bits.store(std::bit_cast<std::uint32_t>(a_value), std::memory_order_relaxed);
		}

		// The guarded primitives. Each body is POD only, which is what lets __try live in
		// the same function; anything that needs C++ unwinding has to stay outside.
		[[nodiscard]] bool GuardedReadSlots(const void* a_src, void** a_dst, std::size_t a_count) noexcept
		{
			__try {
				std::memcpy(a_dst, a_src, a_count * sizeof(void*));
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] bool GuardedVerifySlots(
			const void* const* a_slots,
			const void* const* a_expected,
			std::size_t a_count) noexcept
		{
			__try {
				for (std::size_t i = 0; i < a_count; ++i) {
					if (a_slots[i] != a_expected[i]) {
						return false;
					}
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] bool GuardedWriteSlots(void** a_slots, const void* const* a_source, std::size_t a_count) noexcept
		{
			__try {
				std::memcpy(a_slots, a_source, a_count * sizeof(void*));
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] bool GuardedGetDescs(
			ID3D11SamplerState* const* a_samplers,
			D3D11_SAMPLER_DESC* a_out,
			std::size_t a_count) noexcept
		{
			__try {
				for (std::size_t i = 0; i < a_count; ++i) {
					a_samplers[i]->GetDesc(&a_out[i]);
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] void** SlotAddress(std::size_t a_index) noexcept
		{
			return reinterpret_cast<void**>(
				g_tableBase.load(std::memory_order_relaxed) + a_index * sizeof(void*));
		}

		[[nodiscard]] bool Fail(Failure a_failure) noexcept
		{
			g_lastFailure.store(a_failure, std::memory_order_relaxed);
			return false;
		}

		void ForgetBuild() noexcept
		{
			// The duplicates are deliberately not released here. An engine caller copies
			// sampler pointers out of the table by value, so a pointer that was ever
			// installed has to stay alive for the process. Superseded duplicates are
			// counted and abandoned; a rebuild happens on a configuration change, not per
			// frame, so the retained count is small and bounded by that.
			std::uint32_t retired = 0;
			for (auto& duplicate : g_duplicates) {
				if (duplicate) {
					++retired;
					duplicate = nullptr;
				}
			}
			if (retired != 0) {
				g_retainedSamplers.fetch_add(retired, std::memory_order_relaxed);
			}
			g_runCount = 0;
			g_entriesBiased.store(0, std::memory_order_relaxed);
			g_built.store(false, std::memory_order_release);
		}

		// The reference discipline: refuse while any slot is null and refuse if the
		// identity of a cached slot ever changes underneath us. Between them they cover
		// the two states this feature cannot survive - a table the renderer has not built
		// yet, and a table something else has rewritten or the engine has re-created.
		[[nodiscard]] bool CacheOriginals() noexcept
		{
			void* fresh[kEntryCount] = {};
			if (!GuardedReadSlots(reinterpret_cast<const void*>(g_tableBase.load(std::memory_order_relaxed)),
					fresh, kEntryCount)) {
				return Fail(Failure::kTableReadFault);
			}
			for (const auto* slot : fresh) {
				if (!slot) {
					return Fail(Failure::kNullSlot);
				}
			}
			if (g_originalsCached.load(std::memory_order_acquire)) {
				for (std::size_t i = 0; i < kEntryCount; ++i) {
					if (fresh[i] != g_originals[i]) {
						return Fail(Failure::kIdentityChanged);
					}
				}
				return true;
			}
			for (std::size_t i = 0; i < kEntryCount; ++i) {
				g_originals[i] = static_cast<ID3D11SamplerState*>(fresh[i]);
			}
			g_originalsCached.store(true, std::memory_order_release);
			return true;
		}

		[[nodiscard]] bool Classify(bool a_includeTrilinear) noexcept
		{
			if (!GuardedGetDescs(g_originals, g_descs, kEntryCount)) {
				return Fail(Failure::kTableReadFault);
			}

			std::uint32_t histogram[kFilterCount] = {};
			std::uint32_t other = 0;
			for (const auto& desc : g_descs) {
				bool matched = false;
				for (std::size_t f = 0; f < kFilterCount; ++f) {
					if (static_cast<std::uint32_t>(desc.Filter) == kExpectedFilters[f]) {
						++histogram[f];
						matched = true;
						break;
					}
				}
				if (!matched) {
					++other;
				}
			}
			for (std::size_t f = 0; f < kFilterCount; ++f) {
				g_histogram[f].store(histogram[f], std::memory_order_relaxed);
			}
			g_histogramOther.store(other, std::memory_order_relaxed);

			bool expected = other == 0;
			for (const auto count : histogram) {
				expected = expected && count == kExpectedFilterCount;
			}
			if (!expected) {
				spdlog::warn(
					"sampler bias declined: filter histogram is {}/{}/{}/{}/{} plus {} unrecognized, not {} of each - this is a different build or the table has already been rewritten",
					histogram[0],
					histogram[1],
					histogram[2],
					histogram[3],
					histogram[4],
					other,
					kExpectedFilterCount);
				return Fail(Failure::kFilterHistogram);
			}

			for (std::size_t i = 0; i < kEntryCount; ++i) {
				const auto filter = static_cast<std::uint32_t>(g_descs[i].Filter);
				g_marked[i] = filter == kFilterAnisotropic ||
				              (a_includeTrilinear && filter == kFilterTrilinear);
			}
			return true;
		}

		void BuildRuns() noexcept
		{
			g_runCount = 0;
			std::size_t i = 0;
			while (i < kEntryCount) {
				if (!g_marked[i]) {
					++i;
					continue;
				}
				const std::size_t begin = i;
				while (i < kEntryCount && g_marked[i]) {
					++i;
				}
				if (g_runCount < std::size(g_runs)) {
					g_runs[g_runCount++] = Run{ begin, i - begin };
				}
			}
		}

		[[nodiscard]] bool CreateDuplicates(ID3D11Device* a_device, float a_bias, bool a_lowerMinLod) noexcept
		{
			ID3D11SamplerState* fresh[kEntryCount] = {};
			std::uint32_t created = 0;
			for (std::size_t i = 0; i < kEntryCount; ++i) {
				if (!g_marked[i]) {
					continue;
				}
				D3D11_SAMPLER_DESC desc = g_descs[i];
				desc.MipLODBias = a_bias;
				if (a_lowerMinLod) {
					// FO4's per-sampler MinLOD is (float)mipIndex, which is precisely the
					// global clamp the vendor guide tells integrators to disable. It is
					// also the engine's texture-degrade mechanism, so moving it is opt-in.
					desc.MinLOD = (std::max)(0.0f, desc.MinLOD + a_bias);
				}
				const HRESULT hr = a_device->CreateSamplerState(&desc, &fresh[i]);
				if (FAILED(hr) || !fresh[i]) {
					spdlog::error("sampler bias declined: CreateSamplerState for table entry {} failed, hr=0x{:08X}",
						i, static_cast<std::uint32_t>(hr));
					// These were never installed, so releasing them breaks no invariant.
					for (auto& partial : fresh) {
						if (partial) {
							partial->Release();
							partial = nullptr;
						}
					}
					return Fail(Failure::kCreateSamplerState);
				}
				++created;
			}

			ForgetBuild();
			for (std::size_t i = 0; i < kEntryCount; ++i) {
				g_duplicates[i] = fresh[i];
				g_image[i] = fresh[i] ? fresh[i] : g_originals[i];
			}
			BuildRuns();
			g_entriesBiased.store(created, std::memory_order_relaxed);
			g_samplersCreated.fetch_add(created, std::memory_order_relaxed);
			return true;
		}
	}

	float Derive(
		const Backend::IBackend& a_backend,
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth) noexcept
	{
		if (a_renderWidth == 0 || a_outputWidth == 0) {
			return 0.0f;
		}
		return a_backend.RecommendedMipBias(a_renderWidth, a_outputWidth) + kNativeBias + kEpsilon;
	}

	float GuideBandHigh(std::uint32_t a_renderWidth, std::uint32_t a_outputWidth) noexcept
	{
		if (a_renderWidth == 0 || a_outputWidth == 0) {
			return 0.0f;
		}
		return static_cast<float>(std::log2(static_cast<double>(a_renderWidth) / a_outputWidth));
	}

	float GuideBandLow(std::uint32_t a_renderWidth, std::uint32_t a_outputWidth) noexcept
	{
		return GuideBandHigh(a_renderWidth, a_outputWidth) - 1.0f;
	}

	bool InGuideBand(float a_bias, std::uint32_t a_renderWidth, std::uint32_t a_outputWidth) noexcept
	{
		// kEpsilon is inside the band by construction, so the comparison carries it too
		// rather than reporting the derived value as one step out of its own band.
		return std::isfinite(a_bias) &&
		       a_bias >= GuideBandLow(a_renderWidth, a_outputWidth) - kEpsilon &&
		       a_bias <= GuideBandHigh(a_renderWidth, a_outputWidth) + kEpsilon;
	}

	bool Initialize(std::uintptr_t a_tableBase, ID3D11Device* a_device) noexcept
	{
		if (a_tableBase == 0) {
			return Fail(Failure::kUninitialized);
		}
		if (!a_device) {
			return Fail(Failure::kNoDevice);
		}
		if (g_tableBase.load(std::memory_order_relaxed) != a_tableBase) {
			// A different table is a different set of originals. Forget the build; the
			// samplers themselves are still never released.
			Restore();
			ForgetBuild();
			g_originalsCached.store(false, std::memory_order_release);
			g_tableBase.store(a_tableBase, std::memory_order_relaxed);
		}
		g_device.store(a_device, std::memory_order_relaxed);
		g_initialized.store(true, std::memory_order_release);
		g_lastFailure.store(Failure::kNone, std::memory_order_relaxed);
		return true;
	}

	bool Build(
		const Backend::IBackend* a_backend,
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth,
		float a_overrideBias,
		bool a_includeTrilinear,
		bool a_lowerMinLod) noexcept
	{
		if (!g_initialized.load(std::memory_order_acquire)) {
			g_buildDeclines.fetch_add(1, std::memory_order_relaxed);
			return Fail(Failure::kUninitialized);
		}
		auto* const device = g_device.load(std::memory_order_relaxed);
		if (!device) {
			g_buildDeclines.fetch_add(1, std::memory_order_relaxed);
			return Fail(Failure::kNoDevice);
		}
		// Both extents are per eye. The whole packed stereo target is rasterized in one
		// viewport, so a double-wide width here would halve the ratio and silently derive
		// a bias one full mip level out.
		if (a_renderWidth == 0 || a_outputWidth == 0 || a_renderWidth > a_outputWidth) {
			g_buildDeclines.fetch_add(1, std::memory_order_relaxed);
			return Fail(Failure::kInvalidExtent);
		}

		const bool overrideActive = a_overrideBias != kOverrideSentinel;
		float bias = 0.0f;
		if (overrideActive) {
			bias = a_overrideBias;
		} else if (a_backend) {
			bias = Derive(*a_backend, a_renderWidth, a_outputWidth);
		} else {
			g_buildDeclines.fetch_add(1, std::memory_order_relaxed);
			return Fail(Failure::kNoBias);
		}
		if (!std::isfinite(bias)) {
			g_buildDeclines.fetch_add(1, std::memory_order_relaxed);
			return Fail(Failure::kNoBias);
		}

		const bool unchanged = g_built.load(std::memory_order_acquire) &&
		                       g_builtRenderWidth.load(std::memory_order_relaxed) == a_renderWidth &&
		                       g_builtOutputWidth.load(std::memory_order_relaxed) == a_outputWidth &&
		                       g_builtBiasBits.load(std::memory_order_relaxed) == std::bit_cast<std::uint32_t>(bias) &&
		                       g_includeTrilinear.load(std::memory_order_relaxed) == a_includeTrilinear &&
		                       g_lowerMinLod.load(std::memory_order_relaxed) == a_lowerMinLod;
		if (unchanged) {
			return true;
		}

		if (!InGuideBand(bias, a_renderWidth, a_outputWidth)) {
			g_bandWarnings.fetch_add(1, std::memory_order_relaxed);
			spdlog::warn(
				"sampler bias {:.4f} is outside the vendor guide's band [{:.4f}, {:.4f}] for {} -> {} per eye; honouring it, but flicker and moire are what the band exists to bound",
				bias,
				GuideBandLow(a_renderWidth, a_outputWidth),
				GuideBandHigh(a_renderWidth, a_outputWidth),
				a_renderWidth,
				a_outputWidth);
		}

		// Never rebuild the image the table is currently holding. Restore first so the
		// identity check below compares against the engine's own pointers.
		Restore();

		if (!CacheOriginals() || !Classify(a_includeTrilinear) ||
			!CreateDuplicates(device, bias, a_lowerMinLod)) {
			g_buildDeclines.fetch_add(1, std::memory_order_relaxed);
			// Bounded by construction: a build happens on a configuration change, not per
			// frame. The reason is the whole point of the line - a null slot means the
			// renderer has not filled the table yet and the next configuration retries,
			// while a changed identity means something else owns it now and never will.
			spdlog::warn("sampler bias declined with failure {} for {} -> {} per eye; the engine's own samplers stand",
				static_cast<std::uint32_t>(g_lastFailure.load(std::memory_order_relaxed)),
				a_renderWidth,
				a_outputWidth);
			return false;
		}

		g_overrideActive.store(overrideActive, std::memory_order_relaxed);
		g_includeTrilinear.store(a_includeTrilinear, std::memory_order_relaxed);
		g_lowerMinLod.store(a_lowerMinLod, std::memory_order_relaxed);
		g_builtRenderWidth.store(a_renderWidth, std::memory_order_relaxed);
		g_builtOutputWidth.store(a_outputWidth, std::memory_order_relaxed);
		StoreFloat(g_builtBiasBits, bias);
		StoreFloat(g_bandLowBits, GuideBandLow(a_renderWidth, a_outputWidth));
		StoreFloat(g_bandHighBits, GuideBandHigh(a_renderWidth, a_outputWidth));
		g_builds.fetch_add(1, std::memory_order_relaxed);
		g_lastFailure.store(Failure::kNone, std::memory_order_relaxed);
		g_built.store(true, std::memory_order_release);

		spdlog::info(
			"sampler bias built: {} of {} entries take MipLODBias {:.4f} for {} -> {} per eye ({}), trilinear {}, MinLOD lowering {}",
			g_entriesBiased.load(std::memory_order_relaxed),
			kEntryCount,
			bias,
			a_renderWidth,
			a_outputWidth,
			overrideActive ? "override" : "derived",
			a_includeTrilinear ? "included" : "exempt",
			a_lowerMinLod ? "on" : "off");
		return true;
	}

	void Apply() noexcept
	{
		if (!g_built.load(std::memory_order_acquire)) {
			return;
		}
		if (g_applied.load(std::memory_order_relaxed)) {
			g_unbalanced.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		// Verify every run before writing any of them, so a table the engine re-created
		// under us costs a declined frame rather than 64 freed pointers written back into
		// live state.
		for (std::size_t r = 0; r < g_runCount; ++r) {
			const auto& run = g_runs[r];
			if (!GuardedVerifySlots(
					reinterpret_cast<const void* const*>(SlotAddress(run.begin)),
					reinterpret_cast<const void* const*>(&g_originals[run.begin]),
					run.count)) {
				g_applyDeclines.fetch_add(1, std::memory_order_relaxed);
				g_lastFailure.store(Failure::kApplyIdentityMismatch, std::memory_order_relaxed);
				return;
			}
		}

		bool wrote = false;
		for (std::size_t r = 0; r < g_runCount; ++r) {
			const auto& run = g_runs[r];
			if (!GuardedWriteSlots(
					SlotAddress(run.begin),
					reinterpret_cast<const void* const*>(&g_image[run.begin]),
					run.count)) {
				g_lastFailure.store(Failure::kSwapFault, std::memory_order_relaxed);
				break;
			}
			wrote = true;
		}
		if (!wrote) {
			g_applyDeclines.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		// A partial write still counts as applied. Restore has to run over it, because a
		// duplicate left in the table is what the engine's shutdown releaser would free.
		g_applied.store(true, std::memory_order_relaxed);
		g_appliedFrames.fetch_add(1, std::memory_order_relaxed);
	}

	void Restore() noexcept
	{
		if (!g_applied.load(std::memory_order_relaxed)) {
			return;
		}
		for (std::size_t r = 0; r < g_runCount; ++r) {
			const auto& run = g_runs[r];
			// Unconditional. A slot that no longer holds our duplicate is a reason to
			// write the original back, not a reason to stop.
			if (!GuardedWriteSlots(
					SlotAddress(run.begin),
					reinterpret_cast<const void* const*>(&g_originals[run.begin]),
					run.count)) {
				g_lastFailure.store(Failure::kSwapFault, std::memory_order_relaxed);
			}
		}
		g_applied.store(false, std::memory_order_relaxed);
		g_restoredFrames.fetch_add(1, std::memory_order_relaxed);
	}

	bool Applied() noexcept
	{
		return g_applied.load(std::memory_order_relaxed);
	}

	bool IsBiased(std::size_t a_index) noexcept
	{
		return a_index < kEntryCount && g_built.load(std::memory_order_acquire) && g_marked[a_index];
	}

	State Snapshot() noexcept
	{
		State out{};
		out.initialized = g_initialized.load(std::memory_order_acquire);
		out.originalsCached = g_originalsCached.load(std::memory_order_acquire);
		out.built = g_built.load(std::memory_order_acquire);
		out.applied = g_applied.load(std::memory_order_relaxed);
		out.overrideActive = g_overrideActive.load(std::memory_order_relaxed);
		out.includeTrilinear = g_includeTrilinear.load(std::memory_order_relaxed);
		out.lowerMinLod = g_lowerMinLod.load(std::memory_order_relaxed);
		out.tableAddress = g_tableBase.load(std::memory_order_relaxed);
		for (std::size_t f = 0; f < kFilterCount; ++f) {
			out.filterHistogram[f] = g_histogram[f].load(std::memory_order_relaxed);
		}
		out.filterOther = g_histogramOther.load(std::memory_order_relaxed);
		out.entriesBiased = g_entriesBiased.load(std::memory_order_relaxed);
		out.samplersCreated = g_samplersCreated.load(std::memory_order_relaxed);
		out.retainedSamplers = g_retainedSamplers.load(std::memory_order_relaxed);
		out.builtRenderWidth = g_builtRenderWidth.load(std::memory_order_relaxed);
		out.builtOutputWidth = g_builtOutputWidth.load(std::memory_order_relaxed);
		out.builtBias = LoadFloat(g_builtBiasBits);
		out.guideBandLow = LoadFloat(g_bandLowBits);
		out.guideBandHigh = LoadFloat(g_bandHighBits);
		out.builds = g_builds.load(std::memory_order_relaxed);
		out.buildDeclines = g_buildDeclines.load(std::memory_order_relaxed);
		out.appliedFrames = g_appliedFrames.load(std::memory_order_relaxed);
		out.restoredFrames = g_restoredFrames.load(std::memory_order_relaxed);
		out.applyDeclines = g_applyDeclines.load(std::memory_order_relaxed);
		out.unbalanced = g_unbalanced.load(std::memory_order_relaxed);
		out.bandWarnings = g_bandWarnings.load(std::memory_order_relaxed);
		out.lastFailure = g_lastFailure.load(std::memory_order_relaxed);
		return out;
	}
}
