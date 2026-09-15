#include "Upscaler/WorldAdmissionPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace Upscaler::ResetPolicy;
using namespace Upscaler::ResetPolicy::Detail;

namespace
{
	unsigned g_checks = 0;

	void Check(bool a_condition, const char* a_message)
	{
		++g_checks;
		if (!a_condition) {
			std::cerr << "FAIL: " << a_message << '\n';
			std::exit(1);
		}
	}

	WorldSample Sample(std::uintptr_t a_cell = 2, std::uint64_t a_time = 1000)
	{
		return { 1, a_cell, 3, true, 100.0f, 200.0f, 300.0f, a_time };
	}

	AdmissionMachineState Active()
	{
		AdmissionMachineState state{};
		for (unsigned frame = 1; frame <= kQualityWorldStableFrames; ++frame) {
			const auto step = AdvanceWorldFrame(state, true, 10, 5, Sample(2, 1000 + frame), true);
			Check(step.admitted == (frame == kQualityWorldStableFrames), "startup retains all 16 observations");
			Check(!step.continued, "startup cannot count as continuation");
			state = step.state;
		}
		CompleteWorldFrame(state, MakeTicket(state));
		Check(state.completed, "current published/restored frame earns receipt");
		return state;
	}

	void RejectionMatrix(const AdmissionMachineState& a_active)
	{
		// Every combination independently exercises the production predicate and reducer.
		for (unsigned mask = 0; mask < 512; ++mask) {
			auto state = a_active;
			auto sample = Sample(4, 1030);
			if (mask & 1)
				state.completed = false;
			if (mask & 2)
				sample.worldspace = 6;
			if (mask & 4)
				sample.exterior = false;
			if (mask & 8)
				sample.player = 9;
			if (mask & 16)
				sample.x += 129.0f;
			if (mask & 32)
				sample.observedAtMs += 101;
			const auto step = AdvanceWorldFrame(state, (mask & 64) == 0, 11,
				(mask & 128) ? 6 : 5, sample, (mask & 256) == 0);
			Check(step.admitted == (mask == 0), "only a fully qualified crossing remains admitted");
			Check(step.continued == (mask == 0), "rejected evidence cannot count as continuity");
			Check(!step.state.completed, "each main observation consumes its receipt");
			Check(step.requestReset, "every context change or block retains paired reset request");
		}
	}

	void TicketOwnership(const AdmissionMachineState& a_active)
	{
		const auto ticket = MakeTicket(a_active);
		Check(ValidPublishLease(a_active, ticket, 10, 5), "current ticket may publish");
		Check(!ValidPublishLease(a_active, ticket, 11, 5), "late cell event vetoes publication");
		Check(!ValidPublishLease(a_active, ticket, 10, 6), "hard invalidation alone vetoes publication");
		Check(!ValidPublishLease(a_active, ticket, 11, 6), "late load/menu/config event vetoes publication");
		Check(ValidWorldLease(a_active, ticket, true, 10, 5, Sample(2, 1030)), "current full observation revalidates");
		for (unsigned field = 0; field < 7; ++field) {
			auto observed = Sample(2, 1030);
			if (field == 0)
				++observed.cell;
			if (field == 1)
				++observed.player;
			if (field == 2)
				++observed.worldspace;
			if (field == 3)
				observed.exterior = false;
			Check(!ValidWorldLease(a_active, ticket, field != 4, field == 5 ? 11 : 10,
					  field == 6 ? 6 : 5, observed),
				"late observation cannot reclassify current ticket");
		}
		Check(!MatchesTicket(a_active, {}), "empty ticket rejected");
		auto next = AdvanceWorldFrame(a_active, true, 10, 5, Sample(2, 1030), true).state;
		Check(!MatchesTicket(next, ticket), "previous frame rejected despite identical cell and generation");
		CompleteWorldFrame(next, ticket);
		Check(!next.completed, "stale receipt cannot authorize a crossing");
		CompleteWorldFrame(next, MakeTicket(next));
		Check(next.completed, "current frame receipt accepted");
		for (unsigned field = 0; field < 5; ++field) {
			auto wrong = MakeTicket(next);
			if (field == 0)
				++wrong.cell;
			if (field == 1)
				++wrong.transitionGeneration;
			if (field == 2)
				++wrong.discontinuityGeneration;
			if (field == 3)
				++wrong.frameSerial;
			if (field == 4)
				wrong.admitted = false;
			Check(!MatchesTicket(next, wrong), "each ticket field participates in ownership");
		}
	}

	void Boundaries(const AdmissionMachineState& a_active)
	{
		for (unsigned axis = 0; axis < 3; ++axis) {
			for (float invalid : { std::numeric_limits<float>::quiet_NaN(),
					 std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
					 std::numeric_limits<float>::max() }) {
				auto sample = Sample(4, 1030);
				if (axis == 0)
					sample.x = invalid;
				if (axis == 1)
					sample.y = invalid;
				if (axis == 2)
					sample.z = invalid;
				Check(!AdvanceWorldFrame(a_active, true, 11, 5, sample, true).admitted,
					"nonfinite/huge position cannot establish continuity");
			}
		}
		auto sample = Sample(4, 1116);
		sample.x += 128.0f;
		Check(AdvanceWorldFrame(a_active, true, 11, 5, sample, true).continued, "distance/time limits inclusive");
		sample.y += 1.0f;
		Check(!AdvanceWorldFrame(a_active, true, 11, 5, sample, true).admitted, "limit uses 3D distance");
		sample = Sample(4, 1015);
		Check(!AdvanceWorldFrame(a_active, true, 11, 5, sample, true).admitted, "backward time rejected");
		sample = Sample(0, 1030);
		Check(!AdvanceWorldFrame(a_active, true, 11, 5, sample, true).admitted, "missing cell blocked");
		sample = Sample(4, 1030);
		sample.worldspace = 0;
		Check(!AdvanceWorldFrame(a_active, true, 11, 5, sample, true).admitted, "unknown worldspace cannot continue");
		auto prior = a_active;
		prior.sample.exterior = false;
		Check(!AdvanceWorldFrame(prior, true, 11, 5, Sample(4, 1030), true).admitted, "interior departure cannot continue");
		prior.phase = WorldAdmissionPhase::kStabilizing;
		Check(!AdvanceWorldFrame(prior, true, 11, 5, Sample(4, 1030), true).admitted, "stabilizing gate cannot continue");
	}

	void Recovery(const AdmissionMachineState& a_active)
	{
		auto state = AdvanceWorldFrame(a_active, false, 11, 6, Sample(4, 1030), true).state;
		const auto held = AdvanceWorldFrame(state, false, 11, 6, Sample(4, 1044), true);
		Check(!held.requestReset && !held.admitted, "held block coalesces admission reset");
		state = held.state;
		for (unsigned frame = 1; frame <= 16; ++frame) {
			const auto step = AdvanceWorldFrame(state, true, 11, 6, Sample(4, 1044 + frame * 14), true);
			Check(step.admitted == (frame == 16), "blocked episode recovers through 16 observations");
			Check(!step.continued, "recovery cannot use crossing shortcut");
			state = step.state;
		}
		const auto unchangedCellEvent = AdvanceWorldFrame(a_active, true, 11, 5, Sample(2, 1030), true);
		Check(unchangedCellEvent.continued && unchangedCellEvent.requestReset, "leave/enter event with same observed cell retains reset");
		const auto unreportedCell = AdvanceWorldFrame(a_active, true, 10, 5, Sample(4, 1030), true);
		Check(unreportedCell.continued && unreportedCell.requestReset, "observed crossing does not depend on resetOnCell event setting");
		auto sameCellDifferentWorld = Sample(2, 1030);
		sameCellDifferentWorld.worldspace = 7;
		Check(!AdvanceWorldFrame(a_active, true, 10, 5, sameCellDifferentWorld, true).admitted, "world change cannot hide behind equal cell/event values");
		Check(!AdvanceWorldFrame(a_active, true, 10, 6, Sample(2, 1030), true).admitted, "hard clock alone prevents continuity");
	}
}

int main()
{
	const auto active = Active();
	RejectionMatrix(active);
	TicketOwnership(active);
	Boundaries(active);
	Recovery(active);
	// Synthetic reconstruction of log 6's three eligible cell changes. The log itself
	// does not contain the worldspace/movement/completion observations needed to qualify.
	auto state = active;
	for (unsigned crossing = 1; crossing <= 3; ++crossing) {
		const auto step = AdvanceWorldFrame(state, true, 10 + crossing, 5,
			Sample(2 + crossing, 1016 + crossing * 14), true);
		Check(step.continued && step.admitted && step.requestReset, "repeated qualified crossing keeps Quality active");
		Check(step.state.stableFrames == 16 && !step.activated, "continuation does not fake a new stabilization");
		state = step.state;
		CompleteWorldFrame(state, MakeTicket(state));
	}
	std::cout << "PASS: " << g_checks << " world-admission checks\n";
}
