#include "Upscaler/DynamicResolution.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Backend/BackendTypes.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"
#include "Upscaler/RenderTargets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>

namespace Upscaler::DynamicResolution
{
	namespace
	{
		bool g_configured = false;
		bool g_masterEnabled = false;
		bool g_requested = false;
		bool g_forceHiddenAreaMeshOff = false;
		bool g_hiddenAreaMeshRequired = false;
		// The configured scale, and the two the engine is actually given. They are equal
		// until the vendor snap runs at the first admitted world frame, and stay equal for
		// the whole session when the user typed an explicit override - or until RequestRatio
		// rewrites all three for a new generation (see g_ratioGeneration below).
		//
		// Everything a ratio request writes is written on the settings/configuration thread
		// (RequestRatio) or the render thread (SnapRequestedRatios) and read by `Snapshot`
		// from any thread, so every one of these is atomic rather than plain - g_requestedScale
		// and g_vendorScalingRatio used to be written once, by Configure(), before any other
		// reader could exist; RequestRatio can now write them again while Snapshot() is live on
		// another thread. The ratios are float bit patterns for the same reason
		// `MipBias::g_overrideBits` is: every comparison in this file is an exact bit
		// comparison, and `std::atomic<float>` would invite an arithmetic one. `g_snapped` is
		// stored last with release and loaded first with acquire, so a reader that sees it true
		// sees the whole set.
		std::atomic<std::uint32_t> g_requestedScaleBits{ 0 };
		std::atomic<std::uint32_t> g_requestedScaleXBits{ 0 };
		std::atomic<std::uint32_t> g_requestedScaleYBits{ 0 };
		std::atomic<std::uint32_t> g_vendorScalingRatioBits{ 0 };
		// Bumped every time g_requestedScaleBits/g_vendorScalingRatioBits are given a fresh
		// pair of inputs to snap against: once by Configure's own startup latch (generation
		// 1) and once per accepted RequestRatio call thereafter. SnapRequestedRatios
		// compares this against g_snappedGeneration so "snapped" means "decided - success or
		// a latched give-up - for the current inputs" rather than "decided once, ever": a
		// changed ratio must be able to ask the vendor snap the same question again, but a
		// repeated failure for the same inputs still must not log every frame - see
		// SnapRequestedRatios's own comment, which is the property this generation counter
		// exists to preserve across a ratio change rather than only across retries of one
		// unchanging request.
		std::atomic<std::uint64_t> g_ratioGeneration{ 0 };
		std::atomic<std::uint64_t> g_snappedGeneration{ 0 };
		std::atomic_bool g_snapped{ false };
		std::atomic<std::uint32_t> g_snapOutputWidth{ 0 };
		std::atomic<std::uint32_t> g_snapOutputHeight{ 0 };
		std::atomic<std::uint32_t> g_snapRenderWidth{ 0 };
		std::atomic<std::uint32_t> g_snapRenderHeight{ 0 };
		std::atomic_bool g_initialized{ false };
		std::atomic_bool g_worldAdmissionGated{ false };
		std::atomic_bool g_worldFrameActive{ false };
		std::atomic_bool g_admissionDisabled{ false };
		std::atomic_bool g_abortRestorePending{ false };

		std::uintptr_t g_width = 0;
		std::uintptr_t g_height = 0;
		std::uintptr_t g_targetWidth = 0;
		std::uintptr_t g_targetHeight = 0;
		std::uintptr_t g_imageSelector = 0;
		std::uintptr_t g_hiddenAreaMesh = 0;
		std::array<float, 4> g_before{};
		std::uint8_t g_initialImageSelector = 0;
		std::uint8_t g_initialHiddenAreaMesh = 0;
		bool g_initialValuesValid = false;
		std::atomic_bool g_readbackMatched{ false };
		std::atomic_bool g_hiddenAreaMeshReadbackMatched{ false };
		std::atomic_bool g_hiddenAreaMeshApplied{ false };
		std::atomic_bool g_applied{ false };
		Status g_status = Status::kNotInitialized;
		Failure g_failure = Failure::kNone;
		std::atomic<std::uint64_t> g_effectPassthroughAttempts{ 0 };
		std::atomic<std::uint64_t> g_effectPassthroughSuccesses{ 0 };
		std::atomic<std::uint64_t> g_effectPassthroughFailures{ 0 };
		std::atomic<std::uint64_t> g_effectPassthroughRestoreFailures{ 0 };
		std::atomic<std::uint64_t> g_frameReassertAttempts{ 0 };
		std::atomic<std::uint64_t> g_frameReassertSuccesses{ 0 };
		std::atomic<std::uint64_t> g_frameReassertFailures{ 0 };
		std::atomic<std::uint64_t> g_worldBeginAttempts{ 0 };
		std::atomic<std::uint64_t> g_worldBeginSuccesses{ 0 };
		std::atomic<std::uint64_t> g_worldBeginFailures{ 0 };
		std::atomic<std::uint64_t> g_worldEndAttempts{ 0 };
		std::atomic<std::uint64_t> g_worldEndSuccesses{ 0 };
		std::atomic<std::uint64_t> g_worldEndFailures{ 0 };
		std::atomic<std::uint64_t> g_contractChecks{ 0 };
		std::atomic<std::uint64_t> g_contractFailures{ 0 };
		std::atomic<std::uint64_t> g_selectorInvariantFailures{ 0 };
		std::atomic<std::uint64_t> g_hiddenAreaMeshInvariantFailures{ 0 };

		[[nodiscard]] float RequestedScaleX() noexcept
		{
			return std::bit_cast<float>(g_requestedScaleXBits.load(std::memory_order_acquire));
		}

		[[nodiscard]] float RequestedScaleY() noexcept
		{
			return std::bit_cast<float>(g_requestedScaleYBits.load(std::memory_order_acquire));
		}

		[[nodiscard]] float RequestedScale() noexcept
		{
			return std::bit_cast<float>(g_requestedScaleBits.load(std::memory_order_acquire));
		}

		[[nodiscard]] float VendorScalingRatio() noexcept
		{
			return std::bit_cast<float>(g_vendorScalingRatioBits.load(std::memory_order_acquire));
		}

		[[nodiscard]] bool ReadFloat(std::uintptr_t a_address, float& a_out) noexcept
		{
			std::uint32_t bits = 0;
			if (!Engine::ReadU32(a_address, bits)) {
				return false;
			}
			a_out = std::bit_cast<float>(bits);
			return true;
		}

		[[nodiscard]] bool ReadAll(std::array<float, 4>& a_out) noexcept
		{
			return ReadFloat(g_width, a_out[0]) &&
			       ReadFloat(g_height, a_out[1]) &&
			       ReadFloat(g_targetWidth, a_out[2]) &&
			       ReadFloat(g_targetHeight, a_out[3]);
		}

		[[nodiscard]] bool ReadByte(std::uintptr_t a_address, std::uint8_t& a_out) noexcept
		{
			return a_address != 0 &&
			       FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(a_address), &a_out, sizeof(a_out));
		}

		[[nodiscard]] bool Plausible(float a_value) noexcept
		{
			return std::isfinite(a_value) && a_value > 0.0f && a_value <= 1.0f;
		}

		[[nodiscard]] bool WriteFloat(std::uintptr_t a_address, float a_value) noexcept
		{
			return a_address != 0 &&
			       FaultGuard::SafeWriteBytes(reinterpret_cast<void*>(a_address), &a_value, sizeof(a_value));
		}

		[[nodiscard]] bool WriteByte(std::uintptr_t a_address, std::uint8_t a_value) noexcept
		{
			return a_address != 0 &&
			       FaultGuard::SafeWriteBytes(reinterpret_cast<void*>(a_address), &a_value, sizeof(a_value));
		}

		[[nodiscard]] bool HiddenAreaMeshContractReady() noexcept
		{
			if (!g_hiddenAreaMeshRequired) {
				return true;
			}
			std::uint8_t current = 0xFF;
			return g_hiddenAreaMeshApplied.load(std::memory_order_acquire) &&
			       ReadByte(g_hiddenAreaMesh, current) && current == 0;
		}

		[[nodiscard]] bool RestoreOriginals() noexcept
		{
			const bool wroteTargetWidth = WriteFloat(g_targetWidth, g_before[2]);
			const bool wroteTargetHeight = WriteFloat(g_targetHeight, g_before[3]);
			const bool wroteWidth = WriteFloat(g_width, g_before[0]);
			const bool wroteHeight = WriteFloat(g_height, g_before[1]);
			bool hiddenAreaRestored = true;
			if (g_hiddenAreaMeshApplied.load(std::memory_order_acquire)) {
				std::uint8_t restoredHiddenArea = 0xFF;
				hiddenAreaRestored = WriteByte(g_hiddenAreaMesh, g_initialHiddenAreaMesh) &&
				                     ReadByte(g_hiddenAreaMesh, restoredHiddenArea) &&
				                     restoredHiddenArea == g_initialHiddenAreaMesh;
			}
			std::array<float, 4> restored{};
			const bool restoredAll = wroteTargetWidth && wroteTargetHeight && wroteWidth && wroteHeight &&
			                         hiddenAreaRestored && ReadAll(restored) && restored == g_before;
			if (restoredAll) {
				g_hiddenAreaMeshApplied.store(false, std::memory_order_release);
			}
			return restoredAll;
		}

		[[nodiscard]] std::array<float, 4> CapturedRatios(
			const WorldContractToken& a_token) noexcept
		{
			return {
				a_token.beforeWidth,
				a_token.beforeHeight,
				a_token.beforeTargetWidth,
				a_token.beforeTargetHeight,
			};
		}

		[[nodiscard]] bool RatiosMatchExactly(
			const std::array<float, 4>& a_left,
			const std::array<float, 4>& a_right) noexcept
		{
			for (std::size_t index = 0; index < a_left.size(); ++index) {
				if (std::bit_cast<std::uint32_t>(a_left[index]) !=
					std::bit_cast<std::uint32_t>(a_right[index])) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool RestoreCapturedWorldContract(
			WorldContractToken& a_token) noexcept
		{
			if (!a_token.captured || !a_token.ownsMutation) {
				return false;
			}

			// Keep the mask disabled until every ratio has returned to its native value.
			// This ordering makes each intermediate state conservative even though five
			// distinct engine globals cannot be written by one CPU instruction.
			const bool wroteTargetWidth = WriteFloat(g_targetWidth, a_token.beforeTargetWidth);
			const bool wroteTargetHeight = WriteFloat(g_targetHeight, a_token.beforeTargetHeight);
			const bool wroteWidth = WriteFloat(g_width, a_token.beforeWidth);
			const bool wroteHeight = WriteFloat(g_height, a_token.beforeHeight);
			const bool wroteHiddenArea = WriteByte(g_hiddenAreaMesh, a_token.beforeHiddenAreaMesh);

			std::array<float, 4> restored{};
			std::uint8_t restoredHiddenArea = 0xFF;
			std::uint8_t selector = 0xFF;
			const auto expected = CapturedRatios(a_token);
			const bool restoredAll = wroteTargetWidth && wroteTargetHeight && wroteWidth &&
			                         wroteHeight && wroteHiddenArea && ReadAll(restored) &&
			                         RatiosMatchExactly(restored, expected) &&
			                         ReadByte(g_hiddenAreaMesh, restoredHiddenArea) &&
			                         restoredHiddenArea == a_token.beforeHiddenAreaMesh &&
			                         ReadByte(g_imageSelector, selector) && selector == 1;
			if (restoredAll) {
				a_token.active = false;
				a_token.ownsMutation = false;
				g_worldFrameActive.store(false, std::memory_order_release);
				g_hiddenAreaMeshApplied.store(false, std::memory_order_release);
				g_applied.store(false, std::memory_order_release);
			}
			return restoredAll;
		}

		// The four floats are width, height, target width, target height, so the axes
		// interleave rather than split. Exact bit comparisons, as everywhere here: the
		// snapped ratios differ from the configured one by a few ulps and a tolerance
		// would call a stale value a match.
		[[nodiscard]] bool MatchesRequested(const std::array<float, 4>& a_values) noexcept
		{
			const auto x = g_requestedScaleXBits.load(std::memory_order_acquire);
			const auto y = g_requestedScaleYBits.load(std::memory_order_acquire);
			return std::bit_cast<std::uint32_t>(a_values[0]) == x &&
			       std::bit_cast<std::uint32_t>(a_values[1]) == y &&
			       std::bit_cast<std::uint32_t>(a_values[2]) == x &&
			       std::bit_cast<std::uint32_t>(a_values[3]) == y;
		}

		// One-shot, render-thread only, from the first admitted world frame. The per-eye
		// output extent is not known at plugin load, so the ratio the engine is given
		// cannot be aimed at the vendor's render size until the renderer has built its
		// targets. Logical 1 is the double-wide colour target the NGX create path already
		// measures the eye pair from; the jitter state carries the same extent but is
		// armed later, inside the backend configuration this snap has to precede.
		//
		// A frame where the target is not up yet leaves the flag clear and retries: the
		// configured ratio is written meanwhile, which is exactly today's behaviour.
		void SnapRequestedRatios() noexcept
		{
			// Acquire the generation first: it is stored last (with release) by whichever
			// of Configure/RequestRatio most recently changed the inputs, so a value read
			// here is paired with every ratio/vendor-ratio bit that produced it. "snapped
			// for these inputs" rather than "snapped ever" is what lets a changed ratio
			// (RequestRatio bumping the generation) re-run this decision, while a repeated
			// call for inputs already decided - success or a latched give-up - still costs
			// only two atomic loads and no log line, exactly as the one-shot version did.
			const auto generation = g_ratioGeneration.load(std::memory_order_acquire);
			const bool alreadyDecided = g_snapped.load(std::memory_order_acquire) &&
			                            g_snappedGeneration.load(std::memory_order_acquire) == generation;
			if (alreadyDecided || !(VendorScalingRatio() > 1.0f)) {
				return;
			}
			const auto colour = RenderTargets::Lookup(1);
			if (!colour.valid || colour.width < 2 || (colour.width & 1u) != 0 || colour.height == 0) {
				return;
			}

			const auto vendorScalingRatio = VendorScalingRatio();
			const auto eyeWidth = colour.width / 2u;
			const auto eyeHeight = colour.height;
			const auto x = Backend::SnapRenderScale(eyeWidth, vendorScalingRatio);
			const auto y = Backend::SnapRenderScale(eyeHeight, vendorScalingRatio);
			if (!x.valid || !y.valid || !(x.ratio > 0.0f) || x.ratio > 1.0f ||
				!(y.ratio > 0.0f) || y.ratio > 1.0f) {
				// Latch anyway, for this generation. A retry cannot produce a different
				// answer for the same extent, and a per-frame recomputation of a failing
				// snap is a log line every frame for a decision that will not change -
				// until the generation itself changes, which is exactly what lets a later
				// RequestRatio ask the question again instead of inheriting this refusal
				// forever.
				g_snappedGeneration.store(generation, std::memory_order_release);
				g_snapped.store(true, std::memory_order_release);
				logger::warn(
					FMT_STRING("dynamic resolution: no engine ratio reproduces the vendor render size for a {}x{} eye at scaling ratio {}; the configured ratio {} stands"),
					eyeWidth, eyeHeight, vendorScalingRatio, RequestedScale());
				return;
			}

			// Ratios and extents first, `g_snapped` last with release: a listener thread
			// that acquires a true flag has already seen everything that produced it.
			g_requestedScaleXBits.store(std::bit_cast<std::uint32_t>(x.ratio), std::memory_order_release);
			g_requestedScaleYBits.store(std::bit_cast<std::uint32_t>(y.ratio), std::memory_order_release);
			g_snapOutputWidth.store(eyeWidth, std::memory_order_release);
			g_snapOutputHeight.store(eyeHeight, std::memory_order_release);
			g_snapRenderWidth.store(x.renderExtent, std::memory_order_release);
			g_snapRenderHeight.store(y.renderExtent, std::memory_order_release);
			g_snappedGeneration.store(generation, std::memory_order_release);
			g_snapped.store(true, std::memory_order_release);
			logger::info(
				FMT_STRING("dynamic resolution: snapped the ratio to the vendor optimum - a {}x{} eye at scaling ratio {} renders {}x{}; engine ratios {} horizontal and {} vertical replace the configured {}"),
				eyeWidth, eyeHeight, vendorScalingRatio,
				x.renderExtent, y.renderExtent,
				x.ratio, y.ratio, RequestedScale());
		}

		[[nodiscard]] bool RequestedContractReady(bool a_record) noexcept
		{
			if (a_record) {
				g_contractChecks.fetch_add(1, std::memory_order_relaxed);
			}
			std::array<float, 4> current{};
			std::uint8_t selector = 0xFF;
			const bool selectorReady = ReadByte(g_imageSelector, selector) && selector == 1;
			const bool hiddenAreaReady = HiddenAreaMeshContractReady();
			const bool ready = g_applied.load(std::memory_order_acquire) && ReadAll(current) &&
			                   std::all_of(current.begin(), current.end(), Plausible) &&
			                   MatchesRequested(current) && selectorReady && hiddenAreaReady;
			if (a_record && !ready) {
				g_contractFailures.fetch_add(1, std::memory_order_relaxed);
				if (!selectorReady) {
					g_selectorInvariantFailures.fetch_add(1, std::memory_order_relaxed);
				}
				if (!hiddenAreaReady) {
					g_hiddenAreaMeshInvariantFailures.fetch_add(1, std::memory_order_relaxed);
				}
			}
			return ready;
		}

		[[nodiscard]] bool PassthroughContractReady(bool a_record) noexcept
		{
			if (a_record) {
				g_contractChecks.fetch_add(1, std::memory_order_relaxed);
			}
			std::array<float, 4> current{};
			std::uint8_t selector = 0xFF;
			constexpr float unity = 1.0f;
			const bool selectorReady = ReadByte(g_imageSelector, selector) && selector == 1;
			const bool hiddenAreaReady = HiddenAreaMeshContractReady();
			const bool ready = g_applied.load(std::memory_order_acquire) && ReadAll(current) &&
			                   std::bit_cast<std::uint32_t>(current[0]) == std::bit_cast<std::uint32_t>(unity) &&
			                   std::bit_cast<std::uint32_t>(current[1]) == std::bit_cast<std::uint32_t>(unity) &&
			                   std::bit_cast<std::uint32_t>(current[2]) == g_requestedScaleXBits.load(std::memory_order_acquire) &&
			                   std::bit_cast<std::uint32_t>(current[3]) == g_requestedScaleYBits.load(std::memory_order_acquire) &&
			                   selectorReady && hiddenAreaReady;
			if (a_record && !ready) {
				g_contractFailures.fetch_add(1, std::memory_order_relaxed);
				if (!selectorReady) {
					g_selectorInvariantFailures.fetch_add(1, std::memory_order_relaxed);
				}
				if (!hiddenAreaReady) {
					g_hiddenAreaMeshInvariantFailures.fetch_add(1, std::memory_order_relaxed);
				}
			}
			return ready;
		}

		[[nodiscard]] bool RestoreRequestedRatios() noexcept
		{
			if (!g_applied.load(std::memory_order_acquire)) {
				return true;
			}

			std::array<float, 4> current{};
			const bool ratiosReadable = ReadAll(current);
			if (ratiosReadable && MatchesRequested(current) && HiddenAreaMeshContractReady()) {
				return true;
			}

			g_frameReassertAttempts.fetch_add(1, std::memory_order_relaxed);
			const bool wroteHiddenArea = !g_hiddenAreaMeshRequired || WriteByte(g_hiddenAreaMesh, 0);
			const bool wroteTargetWidth = WriteFloat(g_targetWidth, RequestedScaleX());
			const bool wroteTargetHeight = WriteFloat(g_targetHeight, RequestedScaleY());
			const bool wroteWidth = WriteFloat(g_width, RequestedScaleX());
			const bool wroteHeight = WriteFloat(g_height, RequestedScaleY());
			const bool wrote = wroteHiddenArea && wroteTargetWidth && wroteTargetHeight && wroteWidth && wroteHeight;
			std::array<float, 4> readback{};
			const bool restored = wrote && ReadAll(readback) && MatchesRequested(readback) &&
			                      HiddenAreaMeshContractReady();
			if (!restored) {
				g_frameReassertFailures.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			g_frameReassertSuccesses.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// Render-thread only.  The configuration/listener thread merely closes
		// admission and posts this request; only the verified pre-world/effect seams
		// may touch engine ratio globals or the non-atomic latched controller state.
		[[nodiscard]] bool RestoreAfterConfigurationAbort() noexcept
		{
			if (!g_abortRestorePending.load(std::memory_order_acquire)) {
				return true;
			}
			if (!g_applied.load(std::memory_order_acquire) &&
				!g_hiddenAreaMeshApplied.load(std::memory_order_acquire)) {
				g_abortRestorePending.store(false, std::memory_order_release);
				return true;
			}

			const bool restored = g_initialValuesValid && RestoreOriginals();
			if (!restored) {
				g_status = Status::kRefused;
				g_failure = Failure::kRollbackFault;
				return false;
			}
			g_applied.store(false, std::memory_order_release);
			g_worldFrameActive.store(false, std::memory_order_release);
			g_status = Status::kRefused;
			g_failure = Failure::kGateDisabled;
			g_abortRestorePending.store(false, std::memory_order_release);
			return true;
		}

		void Refuse(Failure a_failure, std::string_view a_reason)
		{
			g_status = Status::kRefused;
			g_failure = a_failure;
			logger::error(FMT_STRING("dynamic resolution diagnostic refused: {}"), a_reason);
		}
	}

	void Configure(
		bool a_masterEnabled,
		bool a_requested,
		double a_scale,
		double a_vendorScalingRatio,
		bool a_forceHiddenAreaMeshOff,
		bool a_gateToWorldAdmission) noexcept
	{
		if (g_admissionDisabled.load(std::memory_order_acquire)) {
			return;
		}
		const float scale = static_cast<float>(a_scale);
		const float ratio = std::isfinite(a_vendorScalingRatio) && a_vendorScalingRatio > 1.0 &&
		                            a_vendorScalingRatio <= 8.0 ?
		                        static_cast<float>(a_vendorScalingRatio) :
		                        0.0f;
		if (g_initialized.load(std::memory_order_acquire)) {
			// scale/ratio disagreeing with what is live no longer means "ignored; restart
			// is required" on its own - RequestRatio (see below) is the live path for
			// exactly that disagreement now. This warning stays for the other five
			// parameters, which remain restart-only, so it only fires when one of those
			// actually changed; a scale/ratio-only difference is expected whenever a live
			// ratio request has moved the session past what Configure was first given.
			if (a_masterEnabled != g_masterEnabled || a_requested != g_requested ||
				a_forceHiddenAreaMeshOff != g_forceHiddenAreaMeshOff ||
				a_gateToWorldAdmission != g_worldAdmissionGated.load(std::memory_order_acquire)) {
				logger::warn("dynamic resolution diagnostic: live configuration change ignored; restart is required"sv);
			}
			return;
		}

		if (g_configured) {
			return;
		}
		g_configured = true;
		g_masterEnabled = a_masterEnabled;
		g_requested = a_requested;
		g_requestedScaleBits.store(std::bit_cast<std::uint32_t>(scale), std::memory_order_release);
		g_requestedScaleXBits.store(std::bit_cast<std::uint32_t>(scale), std::memory_order_release);
		g_requestedScaleYBits.store(std::bit_cast<std::uint32_t>(scale), std::memory_order_release);
		g_vendorScalingRatioBits.store(std::bit_cast<std::uint32_t>(ratio), std::memory_order_release);
		// Generation 1: the first thing SnapRequestedRatios ever has to decide for. Stored
		// last, with release, matching every other generation-bumping write in this file.
		g_ratioGeneration.store(1, std::memory_order_release);
		g_forceHiddenAreaMeshOff = a_forceHiddenAreaMeshOff;
		g_worldAdmissionGated.store(a_gateToWorldAdmission, std::memory_order_release);
	}

	void AbortConfiguration() noexcept
	{
		// Cross-thread portion is atomics only.  The render thread owns the guarded
		// all-four rollback through RestoreAfterConfigurationAbort().
		g_admissionDisabled.store(true, std::memory_order_release);
		g_abortRestorePending.store(true, std::memory_order_release);
	}

	bool Init()
	{
		if (g_admissionDisabled.load(std::memory_order_acquire)) {
			return false;
		}
		if (g_initialized.load(std::memory_order_acquire)) {
			return g_status != Status::kRefused;
		}

		g_width = Engine::RipResolve(
			Addr::kDynamicResolutionWidthRatioAnchor,
			{ 0xF3, 0x0F, 0x10, 0x05 },
			Addr::kRipFloatDispOffset,
			Addr::kRipFloatInstrLen,
			"dynamic resolution live width ratio"sv);
		g_height = Engine::RipResolve(
			Addr::kDynamicResolutionHeightRatioAnchor,
			{ 0xF3, 0x0F, 0x10, 0x05 },
			Addr::kRipFloatDispOffset,
			Addr::kRipFloatInstrLen,
			"dynamic resolution live height ratio"sv);
		g_targetWidth = Engine::RipResolve(
			Addr::kDynamicResolutionTargetWidthAnchor,
			{ 0xF3, 0x0F, 0x11, 0x0D },
			Addr::kRipFloatDispOffset,
			Addr::kRipFloatInstrLen,
			"dynamic resolution target width ratio"sv);
		g_targetHeight = Engine::RipResolve(
			Addr::kDynamicResolutionTargetHeightAnchor,
			{ 0xF3, 0x0F, 0x11, 0x0D },
			Addr::kRipFloatDispOffset,
			Addr::kRipFloatInstrLen,
			"dynamic resolution target height ratio"sv);
		g_imageSelector = Engine::RipResolve(
			Addr::kDynamicResolutionImageSelectorAnchor,
			{ 0x80, 0x3D },
			2,
			7,
			"dynamic resolution image-geometry selector"sv);
		g_hiddenAreaMesh = Engine::RipResolve(
			Addr::kHiddenAreaMeshEnabledAnchor,
			{ 0x0F, 0xB6, 0x05 },
			Addr::kRipDispOffset,
			Addr::kRipInstrLen,
			"VR hidden-area-mesh setting"sv);

		if (g_width == 0 || g_height == 0 || g_targetWidth == 0 || g_targetHeight == 0 ||
			g_imageSelector == 0 || g_hiddenAreaMesh == 0) {
			Refuse(Failure::kAnchorMismatch, "one or more verified RIP anchors did not match"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		if (!ReadAll(g_before) || !ReadByte(g_imageSelector, g_initialImageSelector) ||
			!ReadByte(g_hiddenAreaMesh, g_initialHiddenAreaMesh)) {
			Refuse(Failure::kInitialReadFault, "one or more initial ratio, selector or hidden-area-mesh reads faulted"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}
		g_initialValuesValid = std::all_of(g_before.begin(), g_before.end(), Plausible) &&
		                       g_initialImageSelector <= 1 && g_initialHiddenAreaMesh <= 1;
		if (!g_initialValuesValid) {
			Refuse(Failure::kInitialValueInvalid, "an initial ratio was non-finite/outside (0, 1], or a selector byte was not boolean"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		logger::info(
			FMT_STRING("dynamic resolution: anchors width {:X}, height {:X}, target width {:X}, target height {:X}, image selector {:X}, hidden-area mesh {:X}; initial {}/{}, targets {}/{}, selector {}, hidden-area mesh {}"),
			FaultGuard::ToRva(g_width), FaultGuard::ToRva(g_height),
			FaultGuard::ToRva(g_targetWidth), FaultGuard::ToRva(g_targetHeight),
			FaultGuard::ToRva(g_imageSelector), FaultGuard::ToRva(g_hiddenAreaMesh),
			g_before[0], g_before[1], g_before[2], g_before[3],
			g_initialImageSelector, g_initialHiddenAreaMesh);

		if (!g_masterEnabled) {
			g_status = Status::kDisabled;
			g_failure = Failure::kMasterDisabled;
			logger::info("dynamic resolution diagnostic: inert because enabled=false"sv);
			g_initialized.store(true, std::memory_order_release);
			return true;
		}
		if (!g_requested) {
			g_status = Status::kDisabled;
			g_failure = Failure::kGateDisabled;
			logger::info("dynamic resolution diagnostic: off; no engine ratio was written"sv);
			g_initialized.store(true, std::memory_order_release);
			return true;
		}
		if (g_initialImageSelector != 1) {
			Refuse(Failure::kInitialValueInvalid, "the verified ImageSpace dynamic-geometry selector was not one"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}
		if (!std::isfinite(RequestedScale()) || RequestedScale() < 0.30f || RequestedScale() > 1.0f) {
			Refuse(Failure::kInvalidScale, "renderScaleOverride must be 0.30..1.00 when the diagnostic is enabled"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}
		g_hiddenAreaMeshRequired = RequestedScale() < 1.0f;
		if (g_hiddenAreaMeshRequired && !g_forceHiddenAreaMeshOff) {
			Refuse(Failure::kHiddenAreaPolicyDisabled, "reduced input requires forceHiddenAreaMeshOff=true"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}
		if (g_worldAdmissionGated.load(std::memory_order_acquire)) {
			g_status = Status::kArmed;
			g_failure = Failure::kNone;
			g_applied.store(false, std::memory_order_release);
			g_hiddenAreaMeshApplied.store(false, std::memory_order_release);
			g_worldFrameActive.store(false, std::memory_order_release);
			logger::info(
				FMT_STRING("dynamic resolution diagnostic: world-admission gated ratio {} armed; startup engine ratios and hidden-area mesh remain unchanged"),
				RequestedScale());
			if (VendorScalingRatio() > 1.0f) {
				logger::info(
					FMT_STRING("dynamic resolution: that ratio is the per-mode default, so it is snapped per axis to vendor scaling ratio {} once the per-eye output extent is readable"),
					VendorScalingRatio());
			}
			g_initialized.store(true, std::memory_order_release);
			return true;
		}

		g_hiddenAreaMeshReadbackMatched = !g_hiddenAreaMeshRequired;
		if (g_hiddenAreaMeshRequired) {
			// From the first possible write onward rollback owns this byte. It is disabled
			// before the ratios so no world frame can observe reduced rasterization while
			// the engine is still masking pixels the rectangular reconstruction consumes.
			g_hiddenAreaMeshApplied.store(true, std::memory_order_release);
			const bool wroteHiddenArea = WriteByte(g_hiddenAreaMesh, 0);
			std::uint8_t hiddenAreaReadback = 0xFF;
			if (!wroteHiddenArea) {
				const bool rolledBack = RestoreOriginals();
				Refuse(rolledBack ? Failure::kWriteFault : Failure::kRollbackFault,
					rolledBack ? "the hidden-area mesh write faulted and its original value was restored"sv :
					             "the hidden-area mesh write faulted and original-value rollback did not verify"sv);
				g_initialized.store(true, std::memory_order_release);
				return false;
			}
			if (!ReadByte(g_hiddenAreaMesh, hiddenAreaReadback)) {
				const bool rolledBack = RestoreOriginals();
				Refuse(rolledBack ? Failure::kReadbackFault : Failure::kRollbackFault,
					rolledBack ? "the hidden-area mesh readback faulted and its original value was restored"sv :
					             "the hidden-area mesh readback faulted and original-value rollback did not verify"sv);
				g_initialized.store(true, std::memory_order_release);
				return false;
			}
			g_hiddenAreaMeshReadbackMatched = hiddenAreaReadback == 0;
			if (!g_hiddenAreaMeshReadbackMatched) {
				const bool rolledBack = RestoreOriginals();
				Refuse(rolledBack ? Failure::kReadbackMismatch : Failure::kRollbackFault,
					rolledBack ? "the hidden-area mesh readback was nonzero and its original value was restored"sv :
					             "the hidden-area mesh readback was nonzero and original-value rollback did not verify"sv);
				g_initialized.store(true, std::memory_order_release);
				return false;
			}
		}

		// Targets first, live values second. At plugin load there is no world render, but
		// this ordering also leaves a coherent destination if that lifecycle ever moves.
		// Both axes still carry the configured scale here: the snap needs a per-eye output
		// extent and no render target exists yet, so this path is unchanged.
		const bool wroteTargetWidth = WriteFloat(g_targetWidth, RequestedScaleX());
		const bool wroteTargetHeight = WriteFloat(g_targetHeight, RequestedScaleY());
		const bool wroteWidth = WriteFloat(g_width, RequestedScaleX());
		const bool wroteHeight = WriteFloat(g_height, RequestedScaleY());
		const bool wrote = wroteTargetWidth && wroteTargetHeight && wroteWidth && wroteHeight;
		if (!wrote) {
			const bool rolledBack = RestoreOriginals();
			Refuse(rolledBack ? Failure::kWriteFault : Failure::kRollbackFault,
				rolledBack ? "a guarded write faulted; all four original values were restored"sv :
							 "a guarded write faulted and the original-value rollback did not verify"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		std::array<float, 4> readback{};
		if (!ReadAll(readback)) {
			const bool rolledBack = RestoreOriginals();
			Refuse(rolledBack ? Failure::kReadbackFault : Failure::kRollbackFault,
				rolledBack ? "post-write readback faulted; all four original values were restored"sv :
							 "post-write readback faulted and the original-value rollback did not verify"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		g_readbackMatched = MatchesRequested(readback);
		if (!g_readbackMatched) {
			const bool rolledBack = RestoreOriginals();
			Refuse(rolledBack ? Failure::kReadbackMismatch : Failure::kRollbackFault,
				rolledBack ? "post-write values did not match exactly; all four original values were restored"sv :
							 "post-write values did not match and the original-value rollback did not verify"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}
		if (!HiddenAreaMeshContractReady()) {
			const bool rolledBack = RestoreOriginals();
			Refuse(rolledBack ? Failure::kReadbackMismatch : Failure::kRollbackFault,
				rolledBack ? "the hidden-area mesh changed during startup and all original values were restored"sv :
				             "the hidden-area mesh changed during startup and original-value rollback did not verify"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		g_applied.store(true, std::memory_order_release);
		g_status = Status::kApplied;
		g_failure = Failure::kNone;
		logger::info(FMT_STRING("dynamic resolution diagnostic: applied startup ratio {} to live width/height and both targets; hidden-area mesh {}; restart restores engine ownership"),
			RequestedScale(),
			g_hiddenAreaMeshRequired ? "disabled"sv : "unchanged"sv);
		g_initialized.store(true, std::memory_order_release);
		return true;
	}

	bool WorldAdmissionGated() noexcept
	{
		return g_worldAdmissionGated.load(std::memory_order_acquire);
	}

	bool WorldFrameActive() noexcept
	{
		return g_worldFrameActive.load(std::memory_order_acquire);
	}

	bool BeginReducedWorld(WorldContractToken& a_token) noexcept
	{
		g_worldBeginAttempts.fetch_add(1, std::memory_order_relaxed);
		if (!g_initialized.load(std::memory_order_acquire) || !WorldAdmissionGated() ||
			g_admissionDisabled.load(std::memory_order_acquire) || !g_masterEnabled ||
			!g_requested || g_status == Status::kRefused || a_token.ownsMutation ||
			a_token.active || WorldFrameActive() ||
			g_applied.load(std::memory_order_acquire)) {
			g_worldBeginFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		// Before the capture and before any write, so this frame is already rasterizing at
		// the snapped ratio rather than one frame behind it.
		SnapRequestedRatios();

		a_token = {};
		std::array<float, 4> before{};
		std::uint8_t selector = 0xFF;
		std::uint8_t hiddenAreaMesh = 0xFF;
		if (!ReadAll(before) || !ReadByte(g_imageSelector, selector) || selector != 1 ||
			!ReadByte(g_hiddenAreaMesh, hiddenAreaMesh) || hiddenAreaMesh > 1 ||
			!std::all_of(before.begin(), before.end(), Plausible)) {
			g_status = Status::kRefused;
			g_failure = Failure::kInitialReadFault;
			g_worldBeginFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		a_token.beforeWidth = before[0];
		a_token.beforeHeight = before[1];
		a_token.beforeTargetWidth = before[2];
		a_token.beforeTargetHeight = before[3];
		a_token.beforeHiddenAreaMesh = hiddenAreaMesh;
		a_token.captured = true;
		// Ownership begins before the first possible write. A failed verification may
		// therefore leave this token live for the late seam or next-frame watchdog.
		a_token.ownsMutation = true;

		g_readbackMatched.store(false, std::memory_order_release);
		g_hiddenAreaMeshReadbackMatched.store(!g_hiddenAreaMeshRequired, std::memory_order_release);
		bool wroteHiddenArea = true;
		bool readHiddenArea = true;
		std::uint8_t hiddenAreaReadback = hiddenAreaMesh;
		if (g_hiddenAreaMeshRequired) {
			g_hiddenAreaMeshApplied.store(true, std::memory_order_release);
			wroteHiddenArea = WriteByte(g_hiddenAreaMesh, 0);
			readHiddenArea = wroteHiddenArea && ReadByte(g_hiddenAreaMesh, hiddenAreaReadback);
			g_hiddenAreaMeshReadbackMatched.store(
				readHiddenArea && hiddenAreaReadback == 0,
				std::memory_order_release);
		}

		bool wroteTargetWidth = false;
		bool wroteTargetHeight = false;
		bool wroteWidth = false;
		bool wroteHeight = false;
		if (wroteHiddenArea && readHiddenArea &&
			(!g_hiddenAreaMeshRequired || hiddenAreaReadback == 0)) {
			// The rectangular consumer becomes active only after the mask is known off.
			// Targets lead live values so no future frame can observe a live ratio whose
			// destination policy still belongs to the captured native contract.
			wroteTargetWidth = WriteFloat(g_targetWidth, RequestedScaleX());
			wroteTargetHeight = WriteFloat(g_targetHeight, RequestedScaleY());
			wroteWidth = WriteFloat(g_width, RequestedScaleX());
			wroteHeight = WriteFloat(g_height, RequestedScaleY());
		}

		const bool wroteAll = wroteHiddenArea && wroteTargetWidth && wroteTargetHeight &&
		                      wroteWidth && wroteHeight;
		std::array<float, 4> readback{};
		const bool readAll = wroteAll && ReadAll(readback);
		const bool ratiosMatched = readAll && MatchesRequested(readback);
		g_readbackMatched.store(ratiosMatched, std::memory_order_release);
		const bool hiddenMatched = !g_hiddenAreaMeshRequired ||
		                           (readHiddenArea && hiddenAreaReadback == 0);
		if (wroteAll && ratiosMatched && hiddenMatched) {
			a_token.active = true;
			g_applied.store(true, std::memory_order_release);
			g_worldFrameActive.store(true, std::memory_order_release);
			g_status = Status::kApplied;
			g_failure = Failure::kNone;
			g_worldBeginSuccesses.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		const bool anyWriteFault = !wroteHiddenArea || !wroteTargetWidth ||
		                           !wroteTargetHeight || !wroteWidth || !wroteHeight;
		const Failure failure = anyWriteFault ? Failure::kWriteFault :
		                        (!readHiddenArea || !readAll) ? Failure::kReadbackFault :
		                                                               Failure::kReadbackMismatch;
		const bool rolledBack = RestoreCapturedWorldContract(a_token);
		g_status = Status::kRefused;
		g_failure = rolledBack ? failure : Failure::kRollbackFault;
		g_worldBeginFailures.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	bool EndReducedWorld(WorldContractToken& a_token) noexcept
	{
		if (!a_token.ownsMutation && !a_token.active) {
			return true;
		}
		g_worldEndAttempts.fetch_add(1, std::memory_order_relaxed);
		if (!WorldAdmissionGated() || !a_token.captured || !a_token.ownsMutation) {
			g_worldEndFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		const bool restored = RestoreCapturedWorldContract(a_token);
		if (!restored) {
			g_status = Status::kRefused;
			g_failure = Failure::kRollbackFault;
			g_worldEndFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		g_status = Status::kArmed;
		g_failure = Failure::kNone;
		g_worldEndSuccesses.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	RatioRequestOutcome RequestRatio(double a_scale, double a_vendorScalingRatio) noexcept
	{
		// The three refusals the plan names: never armed, refused at startup, and
		// admission disabled - checked together with g_masterEnabled/g_requested so an
		// admission-gated bit left true by Configure() ahead of an early kDisabled Init()
		// (master or the gate itself off) cannot be mistaken for an armed session. Only
		// kArmed/kApplied are a live reduced-world session; kDisabled and
		// kNotInitialized are not "armed" whatever the raw gate bit says.
		if (g_admissionDisabled.load(std::memory_order_acquire) ||
			!g_worldAdmissionGated.load(std::memory_order_acquire) || !g_masterEnabled ||
			!g_requested || (g_status != Status::kArmed && g_status != Status::kApplied)) {
			return RatioRequestOutcome::kRefusedNotArmed;
		}

		const float scale = static_cast<float>(a_scale);
		// Excludes 1.0 deliberately: that is dlaa, a mode change the frozen admissibility
		// rule still owns (SettingsPolicy.h::ResolveModeSwitch), never this entry point -
		// letting it through here would leave g_hiddenAreaMeshRequired's process-lifetime
		// freeze (computed once in Init from RequestedScale() < 1.0f) representing a ratio
		// that is no longer live.
		if (!std::isfinite(scale) || scale < 0.30f || !(scale < 1.0f)) {
			return RatioRequestOutcome::kRefusedOutOfRange;
		}

		// Defence in depth for the caller's own deny-window guarantee (see this
		// function's header comment): refuse rather than risk a torn write across an
		// in-flight bracket if the wait that was supposed to close it timed out.
		if (WorldFrameActive()) {
			return RatioRequestOutcome::kRefusedWorldFrameActive;
		}

		const float ratio = std::isfinite(a_vendorScalingRatio) && a_vendorScalingRatio > 1.0 &&
		                            a_vendorScalingRatio <= 8.0 ?
		                        static_cast<float>(a_vendorScalingRatio) :
		                        0.0f;
		const auto scaleBits = std::bit_cast<std::uint32_t>(scale);
		const auto ratioBits = std::bit_cast<std::uint32_t>(ratio);
		if (scaleBits == g_requestedScaleBits.load(std::memory_order_acquire) &&
			ratioBits == g_vendorScalingRatioBits.load(std::memory_order_acquire)) {
			return RatioRequestOutcome::kUnchanged;
		}

		// Both axes take the literal scale immediately, exactly as Configure's own
		// startup write does - a frame between this call and the next admitted world
		// frame therefore still rasterizes a plausible ratio. SnapRequestedRatios
		// replaces them per axis at that next admitted frame when a vendor ratio stands;
		// the new generation, stored last with release, is what tells it these are
		// fresh inputs rather than the ones it already decided.
		g_requestedScaleBits.store(scaleBits, std::memory_order_release);
		g_vendorScalingRatioBits.store(ratioBits, std::memory_order_release);
		g_requestedScaleXBits.store(scaleBits, std::memory_order_release);
		g_requestedScaleYBits.store(scaleBits, std::memory_order_release);
		g_ratioGeneration.fetch_add(1, std::memory_order_acq_rel);
		if (ratio > 1.0f) {
			logger::info(
				FMT_STRING("dynamic resolution: ratio request accepted - {} replaces the live requested scale, snapped per axis to vendor scaling ratio {} at the next admitted world frame"),
				scale, ratio);
		} else {
			logger::info(
				FMT_STRING("dynamic resolution: ratio request accepted - {} replaces the live requested scale verbatim, unsnapped"),
				scale);
		}
		return RatioRequestOutcome::kApplied;
	}

	State Snapshot() noexcept
	{
		State state{};
		state.configured = g_configured;
		state.initialized = g_initialized.load(std::memory_order_acquire);
		state.masterEnabled = g_masterEnabled;
		state.requested = g_requested;
		state.worldAdmissionGated = g_worldAdmissionGated.load(std::memory_order_acquire);
		state.worldFrameActive = g_worldFrameActive.load(std::memory_order_acquire);
		state.forceHiddenAreaMeshOff = g_forceHiddenAreaMeshOff;
		state.hiddenAreaMeshRequired = g_hiddenAreaMeshRequired;
		state.anchorsResolved = g_width != 0 && g_height != 0 && g_targetWidth != 0 &&
		                        g_targetHeight != 0 && g_imageSelector != 0 && g_hiddenAreaMesh != 0;
		state.initialValuesValid = g_initialValuesValid;
		state.applied = g_applied;
		state.hiddenAreaMeshApplied = g_hiddenAreaMeshApplied;
		state.requestedContractReady = RequestedContractReady(false);
		state.readbackMatched = g_readbackMatched;
		state.hiddenAreaMeshReadbackMatched = g_hiddenAreaMeshReadbackMatched;
		state.requestedScale = RequestedScale();
		state.vendorScalingRatio = VendorScalingRatio();
		// Acquire the flag first: it was released last, so a true here means the four
		// extents and both ratios read AFTER it are the ones that produced it.
		const bool snapped = g_snapped.load(std::memory_order_acquire);
		state.requestedScaleX = RequestedScaleX();
		state.requestedScaleY = RequestedScaleY();
		state.snappedRenderWidth = g_snapRenderWidth.load(std::memory_order_acquire);
		state.snappedRenderHeight = g_snapRenderHeight.load(std::memory_order_acquire);
		state.snappedOutputWidth = g_snapOutputWidth.load(std::memory_order_acquire);
		state.snappedOutputHeight = g_snapOutputHeight.load(std::memory_order_acquire);
		state.snapped = snapped && state.snappedRenderWidth != 0;
		state.beforeWidth = g_before[0];
		state.beforeHeight = g_before[1];
		state.beforeTargetWidth = g_before[2];
		state.beforeTargetHeight = g_before[3];
		state.widthAddress = g_width;
		state.heightAddress = g_height;
		state.targetWidthAddress = g_targetWidth;
		state.targetHeightAddress = g_targetHeight;
		state.imageSelectorAddress = g_imageSelector;
		state.initialImageSelector = g_initialImageSelector;
		state.hiddenAreaMeshAddress = g_hiddenAreaMesh;
		state.initialHiddenAreaMesh = g_initialHiddenAreaMesh;
		state.status = g_status;
		state.failure = g_failure;
		state.effectPassthroughAttempts = g_effectPassthroughAttempts.load(std::memory_order_relaxed);
		state.effectPassthroughSuccesses = g_effectPassthroughSuccesses.load(std::memory_order_relaxed);
		state.effectPassthroughFailures = g_effectPassthroughFailures.load(std::memory_order_relaxed);
		state.effectPassthroughRestoreFailures = g_effectPassthroughRestoreFailures.load(std::memory_order_relaxed);
		state.frameReassertAttempts = g_frameReassertAttempts.load(std::memory_order_relaxed);
		state.frameReassertSuccesses = g_frameReassertSuccesses.load(std::memory_order_relaxed);
		state.frameReassertFailures = g_frameReassertFailures.load(std::memory_order_relaxed);
		state.worldBeginAttempts = g_worldBeginAttempts.load(std::memory_order_relaxed);
		state.worldBeginSuccesses = g_worldBeginSuccesses.load(std::memory_order_relaxed);
		state.worldBeginFailures = g_worldBeginFailures.load(std::memory_order_relaxed);
		state.worldEndAttempts = g_worldEndAttempts.load(std::memory_order_relaxed);
		state.worldEndSuccesses = g_worldEndSuccesses.load(std::memory_order_relaxed);
		state.worldEndFailures = g_worldEndFailures.load(std::memory_order_relaxed);
		state.contractChecks = g_contractChecks.load(std::memory_order_relaxed);
		state.contractFailures = g_contractFailures.load(std::memory_order_relaxed);
		state.selectorInvariantFailures = g_selectorInvariantFailures.load(std::memory_order_relaxed);
		state.hiddenAreaMeshInvariantFailures = g_hiddenAreaMeshInvariantFailures.load(std::memory_order_relaxed);

		std::array<float, 4> current{};
		state.currentValuesValid = state.anchorsResolved && ReadAll(current);
		if (state.currentValuesValid) {
			state.currentWidth = current[0];
			state.currentHeight = current[1];
			state.currentTargetWidth = current[2];
			state.currentTargetHeight = current[3];
		}
		state.selectorValueValid = ReadByte(g_imageSelector, state.currentImageSelector) &&
		                           state.currentImageSelector <= 1;
		state.hiddenAreaMeshValueValid = ReadByte(g_hiddenAreaMesh, state.currentHiddenAreaMesh) &&
		                                 state.currentHiddenAreaMesh <= 1;
		return state;
	}

	bool BeginEffectPassthrough(EffectPassthroughToken& a_token) noexcept
	{
		g_effectPassthroughAttempts.fetch_add(1, std::memory_order_relaxed);
		a_token = {};
		if (g_admissionDisabled.load(std::memory_order_acquire) ||
			!FrameContractReadyForHandback()) {
			g_effectPassthroughFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		constexpr float unity = 1.0f;
		// From the first possible write onward this token owns restoration. If either
		// write or readback fails, callers must keep the outer HDR bracket alive until
		// the requested ratios are positively restored.
		a_token.active = true;
		const bool wroteWidth = WriteFloat(g_width, unity);
		const bool wroteHeight = WriteFloat(g_height, unity);
		const bool matched = wroteWidth && wroteHeight && PassthroughContractReady(true);
		if (!matched) {
			if (RestoreRequestedRatios()) {
				a_token.active = false;
			} else {
				g_effectPassthroughRestoreFailures.fetch_add(1, std::memory_order_relaxed);
			}
			g_effectPassthroughFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		g_effectPassthroughSuccesses.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool EffectPassthroughReady() noexcept
	{
		return !g_admissionDisabled.load(std::memory_order_acquire) &&
		       PassthroughContractReady(true);
	}

	bool EndEffectPassthrough(EffectPassthroughToken& a_token) noexcept
	{
		if (!a_token.active) {
			return false;
		}

		const bool restored = RestoreRequestedRatios();
		if (!restored) {
			g_effectPassthroughRestoreFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		a_token.active = false;
		return true;
	}

	bool NativeEffectActive() noexcept
	{
		float width = 0.0f;
		float height = 0.0f;
		if (!ReadFloat(g_width, width) || !ReadFloat(g_height, height)) {
			return true;
		}

		constexpr float unity = 1.0f;
		const auto orderedNotEqualToUnity = [](float a_value) noexcept {
			return !std::isnan(a_value) && a_value != unity;
		};
		return orderedNotEqualToUnity(height) || orderedNotEqualToUnity(width);
	}

	bool FrameContractDrifted() noexcept
	{
		if (g_abortRestorePending.load(std::memory_order_acquire)) {
			return true;
		}
		std::array<float, 4> current{};
		return !g_admissionDisabled.load(std::memory_order_acquire) &&
		       g_applied && (!ReadAll(current) || !MatchesRequested(current) ||
		                     !HiddenAreaMeshContractReady());
	}

	bool FrameContractReadyForHandback() noexcept
	{
		if (g_admissionDisabled.load(std::memory_order_acquire)) {
			return false;
		}
		// Native one-to-one evaluation owns no ratio contract: when dynamic resolution
		// was never requested, nothing was written and there is nothing to verify. The
		// world-gated build had required an applied reduced contract here, which made
		// every DLAA frame skip; a requested-but-refused reduction still fails closed
		// below because g_applied stays false.
		if (!g_requested) {
			return true;
		}
		return RequestedContractReady(true);
	}

	bool RestoreRequestedBeforeWorld() noexcept
	{
		if (g_admissionDisabled.load(std::memory_order_acquire)) {
			return RestoreAfterConfigurationAbort();
		}
		return RestoreRequestedRatios();
	}

	bool RestoreRequestedAfterEngineEffect() noexcept
	{
		if (g_admissionDisabled.load(std::memory_order_acquire)) {
			return RestoreAfterConfigurationAbort();
		}
		return RestoreRequestedRatios();
	}

	std::string_view StatusName(Status a_status) noexcept
	{
		switch (a_status) {
		case Status::kDisabled:
			return "disabled"sv;
		case Status::kArmed:
			return "armed"sv;
		case Status::kApplied:
			return "applied"sv;
		case Status::kRefused:
			return "refused"sv;
		default:
			return "notInitialized"sv;
		}
	}

	std::string_view FailureName(Failure a_failure) noexcept
	{
		switch (a_failure) {
		case Failure::kMasterDisabled:
			return "masterDisabled"sv;
		case Failure::kGateDisabled:
			return "gateDisabled"sv;
		case Failure::kInvalidScale:
			return "invalidScale"sv;
		case Failure::kHiddenAreaPolicyDisabled:
			return "hiddenAreaPolicyDisabled"sv;
		case Failure::kAnchorMismatch:
			return "anchorMismatch"sv;
		case Failure::kInitialReadFault:
			return "initialReadFault"sv;
		case Failure::kInitialValueInvalid:
			return "initialValueInvalid"sv;
		case Failure::kWriteFault:
			return "writeFault"sv;
		case Failure::kReadbackFault:
			return "readbackFault"sv;
		case Failure::kReadbackMismatch:
			return "readbackMismatch"sv;
		case Failure::kRollbackFault:
			return "rollbackFault"sv;
		default:
			return "none"sv;
		}
	}

	std::string_view RatioRequestOutcomeName(RatioRequestOutcome a_outcome) noexcept
	{
		switch (a_outcome) {
		case RatioRequestOutcome::kApplied:
			return "applied"sv;
		case RatioRequestOutcome::kUnchanged:
			return "unchanged"sv;
		case RatioRequestOutcome::kRefusedOutOfRange:
			return "refusedOutOfRange"sv;
		case RatioRequestOutcome::kRefusedNotArmed:
			return "refusedNotArmed"sv;
		case RatioRequestOutcome::kRefusedWorldFrameActive:
			return "refusedWorldFrameActive"sv;
		default:
			return "unknown"sv;
		}
	}
}
