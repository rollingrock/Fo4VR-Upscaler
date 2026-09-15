#pragma once

#include "Upscaler/ResetPolicy.h"

#include <cmath>

namespace Upscaler::ResetPolicy::Detail
{
	// Conservative admission limits, not engine speed or cell-size constants.
	inline constexpr double kContinuityMaxDistance = 128.0;
	inline constexpr std::uint64_t kContinuityMaxIntervalMs = 100;

	struct WorldSample
	{
		std::uintptr_t player = 0;
		std::uintptr_t cell = 0;
		std::uintptr_t worldspace = 0;
		bool exterior = false;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		std::uint64_t observedAtMs = 0;
	};

	struct AdmissionMachineState
	{
		WorldAdmissionPhase phase = WorldAdmissionPhase::kBlocked;
		std::uint64_t transitionGeneration = 0;
		std::uintptr_t cell = 0;
		std::uint32_t stableFrames = 0;
		bool resetLatched = false;
		WorldSample sample{};
		std::uint64_t discontinuityGeneration = 0;
		std::uint64_t frameSerial = 0;
		bool completed = false;
	};

	struct AdmissionStep
	{
		AdmissionMachineState state{};
		bool admitted = false;
		bool requestReset = false;
		bool activated = false;
		bool continued = false;
	};

	// Shared by production admission and the engine-free policy tests.
	[[nodiscard]] constexpr AdmissionStep AdvanceAdmission(
		AdmissionMachineState a_previous,
		bool a_worldEligible,
		std::uint64_t a_transitionGeneration,
		std::uintptr_t a_cell,
		std::uint32_t a_requiredStableFrames = kQualityWorldStableFrames,
		bool a_continueContext = false,
		bool a_contextChanged = false) noexcept
	{
		AdmissionStep result{ a_previous };
		if (!a_worldEligible || a_cell == 0) {
			result.requestReset = !a_previous.resetLatched;
			result.state.phase = WorldAdmissionPhase::kBlocked;
			result.state.transitionGeneration = a_transitionGeneration;
			result.state.cell = a_cell;
			result.state.stableFrames = 0;
			result.state.resetLatched = true;
			return result;
		}

		const bool contextChanged =
			a_previous.phase == WorldAdmissionPhase::kBlocked ||
			a_previous.transitionGeneration != a_transitionGeneration ||
			a_previous.cell != a_cell || a_contextChanged;
		if (contextChanged && a_continueContext && a_previous.phase == WorldAdmissionPhase::kActive) {
			result.state.transitionGeneration = a_transitionGeneration;
			result.state.cell = a_cell;
			result.admitted = true;
			result.requestReset = true;
			result.continued = true;
			return result;
		}
		if (contextChanged) {
			result.requestReset = !a_previous.resetLatched;
			result.state.phase = WorldAdmissionPhase::kStabilizing;
			result.state.transitionGeneration = a_transitionGeneration;
			result.state.cell = a_cell;
			result.state.stableFrames = 1;
			result.state.resetLatched = true;
			if (a_requiredStableFrames <= 1) {
				result.state.phase = WorldAdmissionPhase::kActive;
				result.state.resetLatched = false;
				result.admitted = true;
				result.activated = true;
			}
			return result;
		}

		if (a_previous.phase == WorldAdmissionPhase::kActive) {
			result.admitted = true;
			result.state.resetLatched = false;
			return result;
		}

		if (result.state.stableFrames < a_requiredStableFrames) {
			++result.state.stableFrames;
		}
		if (result.state.stableFrames >= a_requiredStableFrames) {
			result.state.phase = WorldAdmissionPhase::kActive;
			result.state.resetLatched = false;
			result.admitted = true;
			result.activated = true;
		}
		return result;
	}

	[[nodiscard]] inline bool CanContinueExterior(
		const AdmissionMachineState& a_previous,
		const WorldSample& a_current,
		std::uint64_t a_discontinuityGeneration,
		bool a_cleanBoundary) noexcept
	{
		const auto& before = a_previous.sample;
		if (!a_cleanBoundary || !a_previous.completed ||
			a_previous.phase != WorldAdmissionPhase::kActive ||
			a_previous.discontinuityGeneration != a_discontinuityGeneration ||
			before.player == 0 || before.player != a_current.player ||
			before.cell == 0 || a_current.cell == 0 ||
			!before.exterior || !a_current.exterior ||
			before.worldspace == 0 || before.worldspace != a_current.worldspace ||
			before.observedAtMs == 0 || a_current.observedAtMs < before.observedAtMs ||
			a_current.observedAtMs - before.observedAtMs > kContinuityMaxIntervalMs) {
			return false;
		}
		const double dx = static_cast<double>(a_current.x) - before.x;
		const double dy = static_cast<double>(a_current.y) - before.y;
		const double dz = static_cast<double>(a_current.z) - before.z;
		const double distanceSquared = dx * dx + dy * dy + dz * dz;
		return std::isfinite(distanceSquared) &&
		       distanceSquared <= kContinuityMaxDistance * kContinuityMaxDistance;
	}

	[[nodiscard]] inline AdmissionStep AdvanceWorldFrame(
		const AdmissionMachineState& a_previous,
		bool a_eligible,
		std::uint64_t a_transitionGeneration,
		std::uint64_t a_discontinuityGeneration,
		const WorldSample& a_sample,
		bool a_cleanBoundary) noexcept
	{
		const bool changed = a_previous.discontinuityGeneration != a_discontinuityGeneration ||
		                     a_previous.sample.player != a_sample.player ||
		                     a_previous.sample.worldspace != a_sample.worldspace ||
		                     a_previous.sample.exterior != a_sample.exterior;
		const bool continuous = CanContinueExterior(a_previous, a_sample,
			a_discontinuityGeneration, a_cleanBoundary);
		auto step = AdvanceAdmission(a_previous, a_eligible, a_transitionGeneration,
			a_sample.cell, kQualityWorldStableFrames, continuous, changed);
		step.state.sample = a_sample;
		step.state.discontinuityGeneration = a_discontinuityGeneration;
		step.state.frameSerial = a_previous.frameSerial + 1;
		step.state.completed = false;
		return step;
	}

	[[nodiscard]] constexpr WorldAdmissionTicket MakeTicket(const AdmissionMachineState& a_state) noexcept
	{
		return { a_state.transitionGeneration, a_state.cell,
			a_state.phase == WorldAdmissionPhase::kActive,
			a_state.discontinuityGeneration, a_state.frameSerial };
	}

	[[nodiscard]] constexpr bool MatchesTicket(
		const AdmissionMachineState& a_state, WorldAdmissionTicket a_ticket) noexcept
	{
		return a_ticket.admitted && a_ticket.frameSerial != 0 &&
		       a_state.phase == WorldAdmissionPhase::kActive &&
		       a_state.transitionGeneration == a_ticket.transitionGeneration &&
		       a_state.discontinuityGeneration == a_ticket.discontinuityGeneration &&
		       a_state.cell == a_ticket.cell && a_state.frameSerial == a_ticket.frameSerial;
	}

	constexpr void CompleteWorldFrame(AdmissionMachineState& a_state, WorldAdmissionTicket a_ticket) noexcept
	{
		a_state.completed = MatchesTicket(a_state, a_ticket);
	}

	[[nodiscard]] constexpr bool ValidPublishLease(
		const AdmissionMachineState& a_state, WorldAdmissionTicket a_ticket,
		std::uint64_t a_transitionGeneration, std::uint64_t a_discontinuityGeneration) noexcept
	{
		return MatchesTicket(a_state, a_ticket) &&
		       a_ticket.transitionGeneration == a_transitionGeneration &&
		       a_ticket.discontinuityGeneration == a_discontinuityGeneration;
	}

	[[nodiscard]] constexpr bool ValidWorldLease(
		const AdmissionMachineState& a_state, WorldAdmissionTicket a_ticket,
		bool a_eligible, std::uint64_t a_transitionGeneration,
		std::uint64_t a_discontinuityGeneration, const WorldSample& a_sample) noexcept
	{
		return a_eligible && ValidPublishLease(a_state, a_ticket, a_transitionGeneration, a_discontinuityGeneration) &&
		       a_sample.cell == a_ticket.cell && a_sample.player == a_state.sample.player &&
		       a_sample.worldspace == a_state.sample.worldspace &&
		       a_sample.exterior == a_state.sample.exterior;
	}
}
