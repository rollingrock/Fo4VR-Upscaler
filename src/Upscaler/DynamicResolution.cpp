#include "Upscaler/DynamicResolution.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"

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
		float g_requestedScale = 0.0f;
		std::atomic_bool g_initialized{ false };

		std::uintptr_t g_width = 0;
		std::uintptr_t g_height = 0;
		std::uintptr_t g_targetWidth = 0;
		std::uintptr_t g_targetHeight = 0;
		std::array<float, 4> g_before{};
		bool g_initialValuesValid = false;
		bool g_readbackMatched = false;
		bool g_applied = false;
		Status g_status = Status::kNotInitialized;
		Failure g_failure = Failure::kNone;
		std::atomic<std::uint64_t> g_effectPassthroughAttempts{ 0 };
		std::atomic<std::uint64_t> g_effectPassthroughSuccesses{ 0 };
		std::atomic<std::uint64_t> g_effectPassthroughFailures{ 0 };
		std::atomic<std::uint64_t> g_effectPassthroughRestoreFailures{ 0 };
		std::atomic<std::uint64_t> g_frameReassertAttempts{ 0 };
		std::atomic<std::uint64_t> g_frameReassertSuccesses{ 0 };
		std::atomic<std::uint64_t> g_frameReassertFailures{ 0 };

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

		[[nodiscard]] bool Plausible(float a_value) noexcept
		{
			return std::isfinite(a_value) && a_value > 0.0f && a_value <= 1.0f;
		}

		[[nodiscard]] bool WriteFloat(std::uintptr_t a_address, float a_value) noexcept
		{
			return a_address != 0 &&
			       FaultGuard::SafeWriteBytes(reinterpret_cast<void*>(a_address), &a_value, sizeof(a_value));
		}

		[[nodiscard]] bool RestoreOriginals() noexcept
		{
			const bool writes = WriteFloat(g_targetWidth, g_before[2]) &&
			                    WriteFloat(g_targetHeight, g_before[3]) &&
			                    WriteFloat(g_width, g_before[0]) &&
			                    WriteFloat(g_height, g_before[1]);
			std::array<float, 4> restored{};
			return writes && ReadAll(restored) && restored == g_before;
		}

		[[nodiscard]] bool MatchesRequested(const std::array<float, 4>& a_values) noexcept
		{
			const auto requested = std::bit_cast<std::uint32_t>(g_requestedScale);
			return std::all_of(a_values.begin(), a_values.end(), [requested](float a_value) {
				return std::bit_cast<std::uint32_t>(a_value) == requested;
			});
		}

		void Refuse(Failure a_failure, std::string_view a_reason)
		{
			g_status = Status::kRefused;
			g_failure = a_failure;
			logger::error(FMT_STRING("dynamic resolution diagnostic refused: {}"), a_reason);
		}
	}

	void Configure(bool a_masterEnabled, bool a_requested, double a_scale) noexcept
	{
		const float scale = static_cast<float>(a_scale);
		if (g_initialized.load(std::memory_order_acquire)) {
			if (a_masterEnabled != g_masterEnabled || a_requested != g_requested ||
				std::bit_cast<std::uint32_t>(scale) != std::bit_cast<std::uint32_t>(g_requestedScale)) {
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
		g_requestedScale = scale;
	}

	bool Init()
	{
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

		if (g_width == 0 || g_height == 0 || g_targetWidth == 0 || g_targetHeight == 0) {
			Refuse(Failure::kAnchorMismatch, "one or more verified RIP anchors did not match"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		if (!ReadAll(g_before)) {
			Refuse(Failure::kInitialReadFault, "one or more initial ratio reads faulted"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}
		g_initialValuesValid = std::all_of(g_before.begin(), g_before.end(), Plausible);
		if (!g_initialValuesValid) {
			Refuse(Failure::kInitialValueInvalid, "an initial ratio was non-finite or outside (0, 1]"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		logger::info(
			FMT_STRING("dynamic resolution: anchors width {:X}, height {:X}, target width {:X}, target height {:X}; initial {}/{}, targets {}/{}"),
			FaultGuard::ToRva(g_width), FaultGuard::ToRva(g_height),
			FaultGuard::ToRva(g_targetWidth), FaultGuard::ToRva(g_targetHeight),
			g_before[0], g_before[1], g_before[2], g_before[3]);

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
		if (!std::isfinite(g_requestedScale) || g_requestedScale < 0.30f || g_requestedScale > 1.0f) {
			Refuse(Failure::kInvalidScale, "renderScaleOverride must be 0.30..1.00 when the diagnostic is enabled"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		// Targets first, live values second. At plugin load there is no world render, but
		// this ordering also leaves a coherent destination if that lifecycle ever moves.
		const bool wrote = WriteFloat(g_targetWidth, g_requestedScale) &&
		                   WriteFloat(g_targetHeight, g_requestedScale) &&
		                   WriteFloat(g_width, g_requestedScale) &&
		                   WriteFloat(g_height, g_requestedScale);
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

		g_readbackMatched = std::all_of(readback.begin(), readback.end(), [](float a_value) {
			return std::bit_cast<std::uint32_t>(a_value) == std::bit_cast<std::uint32_t>(g_requestedScale);
		});
		if (!g_readbackMatched) {
			const bool rolledBack = RestoreOriginals();
			Refuse(rolledBack ? Failure::kReadbackMismatch : Failure::kRollbackFault,
				rolledBack ? "post-write values did not match exactly; all four original values were restored"sv :
							 "post-write values did not match and the original-value rollback did not verify"sv);
			g_initialized.store(true, std::memory_order_release);
			return false;
		}

		g_applied = true;
		g_status = Status::kApplied;
		g_failure = Failure::kNone;
		logger::info(FMT_STRING("dynamic resolution diagnostic: applied startup ratio {} to live width/height and both targets; restart restores engine ownership"), g_requestedScale);
		g_initialized.store(true, std::memory_order_release);
		return true;
	}

	State Snapshot() noexcept
	{
		State state{};
		state.configured = g_configured;
		state.initialized = g_initialized.load(std::memory_order_acquire);
		state.masterEnabled = g_masterEnabled;
		state.requested = g_requested;
		state.anchorsResolved = g_width != 0 && g_height != 0 && g_targetWidth != 0 && g_targetHeight != 0;
		state.initialValuesValid = g_initialValuesValid;
		state.applied = g_applied;
		state.readbackMatched = g_readbackMatched;
		state.requestedScale = g_requestedScale;
		state.beforeWidth = g_before[0];
		state.beforeHeight = g_before[1];
		state.beforeTargetWidth = g_before[2];
		state.beforeTargetHeight = g_before[3];
		state.widthAddress = g_width;
		state.heightAddress = g_height;
		state.targetWidthAddress = g_targetWidth;
		state.targetHeightAddress = g_targetHeight;
		state.status = g_status;
		state.failure = g_failure;
		state.effectPassthroughAttempts = g_effectPassthroughAttempts.load(std::memory_order_relaxed);
		state.effectPassthroughSuccesses = g_effectPassthroughSuccesses.load(std::memory_order_relaxed);
		state.effectPassthroughFailures = g_effectPassthroughFailures.load(std::memory_order_relaxed);
		state.effectPassthroughRestoreFailures = g_effectPassthroughRestoreFailures.load(std::memory_order_relaxed);
		state.frameReassertAttempts = g_frameReassertAttempts.load(std::memory_order_relaxed);
		state.frameReassertSuccesses = g_frameReassertSuccesses.load(std::memory_order_relaxed);
		state.frameReassertFailures = g_frameReassertFailures.load(std::memory_order_relaxed);

		std::array<float, 4> current{};
		state.currentValuesValid = state.anchorsResolved && ReadAll(current);
		if (state.currentValuesValid) {
			state.currentWidth = current[0];
			state.currentHeight = current[1];
			state.currentTargetWidth = current[2];
			state.currentTargetHeight = current[3];
		}
		return state;
	}

	bool BeginEffectPassthrough(EffectPassthroughToken& a_token) noexcept
	{
		g_effectPassthroughAttempts.fetch_add(1, std::memory_order_relaxed);
		a_token = {};
		if (!g_applied || !ReadFloat(g_width, a_token.width) || !ReadFloat(g_height, a_token.height) ||
			!Plausible(a_token.width) || !Plausible(a_token.height)) {
			g_effectPassthroughFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		constexpr float unity = 1.0f;
		const bool wroteWidth = WriteFloat(g_width, unity);
		const bool wroteHeight = WriteFloat(g_height, unity);
		float readWidth = 0.0f;
		float readHeight = 0.0f;
		const bool matched = wroteWidth && wroteHeight &&
		                     ReadFloat(g_width, readWidth) && ReadFloat(g_height, readHeight) &&
		                     readWidth == unity && readHeight == unity;
		if (!matched) {
			const bool restoredWidth = WriteFloat(g_width, a_token.width);
			const bool restoredHeight = WriteFloat(g_height, a_token.height);
			if (!restoredWidth || !restoredHeight) {
				g_effectPassthroughRestoreFailures.fetch_add(1, std::memory_order_relaxed);
			}
			g_effectPassthroughFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		a_token.active = true;
		g_effectPassthroughSuccesses.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool EndEffectPassthrough(EffectPassthroughToken& a_token) noexcept
	{
		if (!a_token.active) {
			return false;
		}

		const bool wroteWidth = WriteFloat(g_width, a_token.width);
		const bool wroteHeight = WriteFloat(g_height, a_token.height);
		float readWidth = 0.0f;
		float readHeight = 0.0f;
		const bool restored = wroteWidth && wroteHeight &&
		                      ReadFloat(g_width, readWidth) && ReadFloat(g_height, readHeight) &&
		                      std::bit_cast<std::uint32_t>(readWidth) == std::bit_cast<std::uint32_t>(a_token.width) &&
		                      std::bit_cast<std::uint32_t>(readHeight) == std::bit_cast<std::uint32_t>(a_token.height);
		a_token.active = false;
		if (!restored) {
			g_effectPassthroughRestoreFailures.fetch_add(1, std::memory_order_relaxed);
		}
		return restored;
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
		std::array<float, 4> current{};
		return g_applied && ReadAll(current) &&
		       std::all_of(current.begin(), current.end(), Plausible) &&
		       !MatchesRequested(current);
	}

	bool RestoreRequestedAfterEngineEffect() noexcept
	{
		if (!g_applied) {
			return true;
		}

		std::array<float, 4> current{};
		if (!ReadAll(current)) {
			g_frameReassertAttempts.fetch_add(1, std::memory_order_relaxed);
			g_frameReassertFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (MatchesRequested(current)) {
			return true;
		}

		g_frameReassertAttempts.fetch_add(1, std::memory_order_relaxed);
		const bool wrote = WriteFloat(g_targetWidth, g_requestedScale) &&
		                   WriteFloat(g_targetHeight, g_requestedScale) &&
		                   WriteFloat(g_width, g_requestedScale) &&
		                   WriteFloat(g_height, g_requestedScale);
		std::array<float, 4> readback{};
		if (!wrote || !ReadAll(readback) || !MatchesRequested(readback)) {
			g_frameReassertFailures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		g_frameReassertSuccesses.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	std::string_view StatusName(Status a_status) noexcept
	{
		switch (a_status) {
		case Status::kDisabled:
			return "disabled"sv;
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
}
