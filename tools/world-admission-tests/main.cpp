#include "Upscaler/FoveationPolicy.h"
#include "Upscaler/NativeVetoPolicy.h"
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
			Check(step.requestReset == (mask != 0), "only proven continuity avoids a new reset");
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
		Check(unchangedCellEvent.continued && !unchangedCellEvent.requestReset, "qualified same-cell leave/enter preserves history");
		const auto unreportedCell = AdvanceWorldFrame(a_active, true, 10, 5, Sample(4, 1030), true);
		Check(unreportedCell.continued && !unreportedCell.requestReset, "observed crossing does not depend on resetOnCell event setting");
		auto sameCellDifferentWorld = Sample(2, 1030);
		sameCellDifferentWorld.worldspace = 7;
		Check(!AdvanceWorldFrame(a_active, true, 10, 5, sameCellDifferentWorld, true).admitted, "world change cannot hide behind equal cell/event values");
		Check(!AdvanceWorldFrame(a_active, true, 10, 6, Sample(2, 1030), true).admitted, "hard clock alone prevents continuity");
	}

	AdmissionMachineState RejectCellFrame(const AdmissionMachineState& a_active, WorldRecovery& a_recovery)
	{
		auto state = AdvanceRecoverableWorldFrame(a_active, a_recovery, true, 10, 5, Sample(2, 1030), true).state;
		const auto ticket = MakeTicket(state);
		Check(a_recovery.armed && !a_recovery.rejected && !a_recovery.closed, "completed prior frame arms a one-frame recovery");
		const auto rejected = RejectWorldAdmission(state, a_recovery, ticket, WorldBlockReason::kNoCell, 11, 5, 0);
		Check(!rejected.requestReset && !rejected.state.resetLatched, "cell-only rejection defers without acknowledging a reset");
		state = rejected.state;
		Check(a_recovery.rejected && !a_recovery.closed, "late cell rejection is not yet safe to recover");
		Check(!ValidPublishLease(state, ticket, 11, 5), "rejected frame never regains its publish lease");
		return state;
	}

	void HistoryContinuity(const AdmissionMachineState& a_active)
	{
		for (unsigned mask = 0; mask < 512; ++mask) {
			WorldRecovery recovery{};
			auto state = RejectCellFrame(a_active, recovery);
			const auto rejectedTicket = recovery.ticket;
			auto closedTicket = rejectedTicket;
			if (mask & 1)
				++closedTicket.frameSerial;
			CompleteRejectedWorldFrame(recovery, state, closedTicket, (mask & 2) == 0);
			auto sample = Sample(4, 1044);
			if (mask & 4)
				++sample.worldspace;
			if (mask & 8)
				sample.x += 129.0f;
			if (mask & 16)
				sample.observedAtMs = 1117;
			if (mask & 32)
				sample.exterior = false;
			const auto step = AdvanceRecoverableWorldFrame(state, recovery, (mask & 64) == 0,
				12, (mask & 128) ? 6 : 5, sample, (mask & 256) == 0);
			Check(step.requestReset == (mask != 0), "every missing history proof forces a reset before reuse");
			Check(step.recovered == ((mask & ~2u) == 0), "admission recovery is separate from history continuity");
			Check(!ValidPublishLease(step.state, rejectedTicket, 12, 5), "history preservation never resurrects rejected pixels");
		}
		for (auto reason : { WorldBlockReason::kLoadingMenu, WorldBlockReason::kFaderMenu,
				 WorldBlockReason::kStateReadFault, WorldBlockReason::kReconstructionUnavailable }) {
			WorldRecovery recovery{};
			const auto active = AdvanceRecoverableWorldFrame(a_active, recovery, true, 10, 5, Sample(2, 1030), true).state;
			const auto rejected = RejectWorldAdmission(active, recovery, MakeTicket(active), reason, 11, 5, 2);
			Check(rejected.requestReset && !recovery.armed, "a coincident cell event cannot defer a hard refusal");
		}
		WorldRecovery recovery{};
		auto state = AdvanceRecoverableWorldFrame(a_active, recovery, true, 10, 5, Sample(2, 1030), true).state;
		const auto ticket = MakeTicket(state);
		const auto publishVeto = RejectWorldAdmission(state, recovery, ticket, WorldBlockReason::kTransitionChanged, 11, 5, state.cell);
		Check(!publishVeto.admitted && !publishVeto.requestReset, "final atomic veto defers only the reset, never publication");
		CompleteRejectedWorldFrame(recovery, publishVeto.state, ticket, true);
		auto hardEvent = AdvanceRecoverableWorldFrame(publishVeto.state, recovery, true, 12, 6, Sample(4, 1044), true);
		Check(hardEvent.requestReset && !hardEvent.recovered, "hard event after closure defeats the history receipt");
		recovery = {};
		state = RejectCellFrame(a_active, recovery);
		auto expired = AdvanceRecoverableWorldFrame(state, recovery, false, 11, 5, Sample(0, 1044), true);
		Check(expired.requestReset, "unclosed deferred reset is paid at the next main boundary");
		const auto held = AdvanceRecoverableWorldFrame(expired.state, recovery, false, 11, 5, Sample(0, 1058), true);
		Check(!held.requestReset, "failed continuity does not flood resets while blocked");
	}

	void LateCellRecovery(const AdmissionMachineState& a_active)
	{
		WorldRecovery recovery{};
		auto state = RejectCellFrame(a_active, recovery);
		const auto rejectedTicket = recovery.ticket;
		CompleteRejectedWorldFrame(recovery, state, rejectedTicket);
		Check(recovery.closed, "exact normally restored frame earns recovery receipt");
		auto step = AdvanceRecoverableWorldFrame(state, recovery, true, 12, 5, Sample(4, 1044), true);
		Check(step.admitted && step.recovered && !step.continued && step.requestReset,
			"next clean exterior frame resumes with a paired reset");
		Check(step.state.frameSerial == state.frameSerial + 1 && !step.state.completed,
			"recovery issues a fresh serial and owes its own completion");
		Check(!recovery.armed, "a recovered frame cannot anchor another aborted frame");
		Check(!ValidPublishLease(step.state, rejectedTicket, 12, 5), "old pixels stay unauthorized after recovery");
		state = step.state;
		RejectWorldFrame(recovery, state, MakeTicket(state), WorldBlockReason::kNoCell, 13, 5);
		state = AdvanceAdmission(state, false, 13, 0).state;
		CompleteRejectedWorldFrame(recovery, state, rejectedTicket);
		step = AdvanceRecoverableWorldFrame(state, recovery, true, 14, 5, Sample(6, 1058), true);
		Check(!step.admitted && !step.recovered, "consecutive aborts cannot chain indefinitely");

		for (unsigned mask = 0; mask < 256; ++mask) {
			recovery = {};
			state = RejectCellFrame(a_active, recovery);
			if (!(mask & 1))
				CompleteRejectedWorldFrame(recovery, state, recovery.ticket);
			auto sample = Sample(4, 1044);
			if (mask & 2)
				++sample.player;
			if (mask & 4)
				++sample.worldspace;
			if (mask & 8)
				sample.exterior = false;
			if (mask & 16)
				sample.observedAtMs = 1117;
			step = AdvanceRecoverableWorldFrame(state, recovery, (mask & 32) == 0,
				12, (mask & 64) ? 6 : 5, sample, (mask & 128) == 0);
			Check(step.recovered == (mask == 0) && step.admitted == (mask == 0),
				"recovery requires closure, identity, fresh time, eligibility, hard clock and clean boundary");
			Check(!recovery.armed, "recovery attempt consumes its anchor on both acceptance and rejection");
		}

		for (auto reason : { WorldBlockReason::kFaderMenu, WorldBlockReason::kLoadingMenu,
				 WorldBlockReason::kPipboyMenu, WorldBlockReason::kMenuMode,
				 WorldBlockReason::kFreezeFrameMenuBackground, WorldBlockReason::kNoPlayer,
				 WorldBlockReason::kStateReadFault, WorldBlockReason::kOperatorVeto,
				 WorldBlockReason::kReconstructionUnavailable, WorldBlockReason::kRenderPhaseUnavailable }) {
			recovery = {};
			state = AdvanceRecoverableWorldFrame(a_active, recovery, true, 10, 5, Sample(2, 1030), true).state;
			RejectWorldFrame(recovery, state, MakeTicket(state), reason, 11, 5);
			Check(!recovery.armed, "non-cell refusal discards recovery even with a coincident cell event");
		}
		for (unsigned bad = 0; bad < 4; ++bad) {
			recovery = {};
			state = AdvanceRecoverableWorldFrame(a_active, recovery, true, 10, 5, Sample(2, 1030), true).state;
			auto ticket = MakeTicket(state);
			if (bad == 0)
				--ticket.frameSerial;
			if (bad == 1)
				++ticket.cell;
			RejectWorldFrame(recovery, state, ticket, WorldBlockReason::kNoCell,
				bad == 2 ? 10 : 11, bad == 3 ? 6 : 5);
			Check(!recovery.armed, "stale ticket, missing event or hard discontinuity cannot arm recovery");
		}
		recovery = {};
		state = RejectCellFrame(a_active, recovery);
		auto wrongTicket = recovery.ticket;
		++wrongTicket.frameSerial;
		CompleteRejectedWorldFrame(recovery, state, wrongTicket);
		Check(!recovery.closed, "a different frame cannot complete rejected work");
		CompleteRejectedWorldFrame(recovery, state, recovery.ticket);
		step = AdvanceRecoverableWorldFrame(state, recovery, false, 11, 5, Sample(0, 1044), true);
		step = AdvanceRecoverableWorldFrame(step.state, recovery, true, 12, 5, Sample(4, 1058), true);
		Check(!step.recovered && !step.admitted, "an intervening invalid main frame expires recovery");
	}

	// The DLAA product gate's half of the operator veto. Reduced modes take theirs through
	// the reducer above; DLAA never runs it.
	struct NativeVetoEpisode
	{
		bool open = false;
		unsigned skips = 0;
		unsigned opens = 0;
		unsigned resets = 0;
		unsigned resumes = 0;
	};

	// The HDR call site in miniature: a skip returns before any work, and only a frame
	// that also evaluates successfully closes the episode it was told to close.
	bool NativeVetoFrame(NativeVetoEpisode& a_episode, bool a_latched, bool a_live, bool a_prepared, bool a_evaluates)
	{
		const auto step = Upscaler::HDRRoundTrip::Detail::DecideNativeVeto(a_latched, a_live, a_episode.open, a_prepared);
		if (step.skip) {
			++a_episode.skips;
			if (step.openEpisode) {
				a_episode.open = true;
				++a_episode.opens;
			}
			if (step.requestReset) {
				++a_episode.resets;
			}
			return true;
		}
		if (step.closeEpisode && a_evaluates) {
			a_episode.open = false;
			++a_episode.resumes;
		}
		return false;
	}

	void NativeOperatorVeto()
	{
		for (unsigned mask = 0; mask < 16; ++mask) {
			const bool latched = (mask & 1) != 0;
			const bool live = (mask & 2) != 0;
			const bool episodeOpen = (mask & 4) != 0;
			const bool prepared = (mask & 8) != 0;
			const bool vetoed = latched || live;
			const auto step = Upscaler::HDRRoundTrip::Detail::DecideNativeVeto(latched, live, episodeOpen, prepared);
			Check(step.skip == vetoed, "the latch or the live veto skips the frame, and nothing else does");
			Check(step.openEpisode == (vetoed && !episodeOpen), "only the first vetoed frame opens an episode");
			Check(step.requestReset == (vetoed && !episodeOpen && prepared), "one reset per episode, and only when histories exist");
			Check(step.closeEpisode == (!vetoed && episodeOpen), "only a released frame inside an episode closes it");
			Check(!(step.requestReset && !step.skip), "a reset is requested only on a skipped frame");
		}

		// The two halves agree. The pre-world seam latches only where the HDR gate reads the
		// veto, so a raster drawn without the bias is never reconstructed, whichever way the
		// veto moves between the two seams. Off the honoured path nothing latches and the
		// reconstruction keeps its bias.
		for (unsigned mask = 0; mask < 16; ++mask) {
			const bool mainFrame = (mask & 1) != 0;
			const bool vetoAtPreWorld = (mask & 2) != 0;
			const bool vetoAtHdr = (mask & 4) != 0;
			const bool honoured = (mask & 8) != 0;
			const bool latched = Upscaler::HDRRoundTrip::Detail::LatchNativeVeto(mainFrame, vetoAtPreWorld, honoured);
			const auto step = honoured ?
			                      Upscaler::HDRRoundTrip::Detail::DecideNativeVeto(latched, vetoAtHdr, false, true) :
			                      Upscaler::HDRRoundTrip::Detail::NativeVetoStep{};
			Check(latched == (mainFrame && vetoAtPreWorld && honoured), "only a main frame on the honoured path latches");
			Check(!latched || step.skip, "a latched raster is always skipped");
		}

		// One hold. Engaged after the pre-world seam, so the first frame skips on the live
		// veto alone; released after the seam, so the last skips on the latch alone; the
		// first frame after that declines (a menu's closed jitter gate) and the next one
		// evaluates.
		NativeVetoEpisode hold{};
		Check(NativeVetoFrame(hold, false, true, true, false), "a mid-frame engage skips on the live veto");
		for (unsigned frame = 0; frame < 30; ++frame) {
			Check(NativeVetoFrame(hold, true, true, true, false), "a held veto skips every frame");
		}
		Check(NativeVetoFrame(hold, true, false, true, false), "a mid-frame release skips on the latch");
		Check(!NativeVetoFrame(hold, false, false, true, false) && hold.open, "a declined frame leaves the episode open");
		Check(!NativeVetoFrame(hold, false, false, true, true) && !hold.open, "the first evaluated frame closes it");
		Check(!NativeVetoFrame(hold, false, false, true, true), "after the episode, frames evaluate");
		Check(hold.skips == 32 && hold.opens == 1 && hold.resets == 1 && hold.resumes == 1,
			"one hold: 32 skips, one episode, one reset, one resume");

		// A veto held before the first evaluation has no history to reset.
		NativeVetoEpisode cold{};
		for (unsigned frame = 0; frame < 4; ++frame) {
			Check(NativeVetoFrame(cold, true, true, false, false), "a cold veto still skips");
		}
		Check(!NativeVetoFrame(cold, false, false, false, true), "a cold release evaluates");
		Check(cold.opens == 1 && cold.resets == 0 && cold.resumes == 1, "a cold episode resets nothing");

		// Twenty quick taps: each is its own episode with its own reset and resume.
		NativeVetoEpisode taps{};
		for (unsigned tap = 0; tap < 20; ++tap) {
			Check(NativeVetoFrame(taps, false, true, true, false), "a tap engages mid-frame");
			Check(NativeVetoFrame(taps, true, false, true, false), "and releases mid-frame");
			Check(!NativeVetoFrame(taps, false, false, true, true), "and DLAA comes back");
		}
		Check(taps.skips == 40 && taps.opens == 20 && taps.resets == 20 && taps.resumes == 20,
			"one reset and one resume per tap, never a reset per frame");

		// A reconfiguration clears the episode flag mid-hold. The next vetoed frame opens a
		// fresh episode and owes the new histories their own reset.
		NativeVetoEpisode reconfigured{};
		Check(NativeVetoFrame(reconfigured, true, true, true, false), "a held veto skips");
		reconfigured.open = false;
		Check(NativeVetoFrame(reconfigured, true, true, true, false), "and keeps skipping after the clear");
		Check(reconfigured.opens == 2 && reconfigured.resets == 2, "a cleared episode reopens with its own reset");
	}
}

namespace
{
	// The foveated measurement build's rules (FoveationPolicy.h): who may crop, which source
	// chooses the crop, the cycle the hotkey steps through, and when the render thread resets
	// the histories and the perf window.
	void FoveationRules()
	{
		using namespace Upscaler::Foveation;

		// Only the DLAA product may crop, and each refusal names the first missing condition.
		constexpr Session kProduct{ true, true, true, true, true };
		Check(SessionRefusal(kProduct) == Refusal::kNone, "the DLAA product may crop");
		Check(SessionRefusal({ false, true, true, true, true }) == Refusal::kModeNotDlaa, "a reduced mode or off refuses");
		Check(SessionRefusal({ true, false, true, true, true }) == Refusal::kNotProductPath, "a diagnostic gate refuses");
		Check(SessionRefusal({ true, true, false, true, true }) == Refusal::kQualityNotDlaa, "a non-DLAA quality refuses");
		Check(SessionRefusal({ true, true, true, false, true }) == Refusal::kBackendNotNgx, "an FFX DLAA refuses");
		Check(SessionRefusal({ true, true, true, true, false }) == Refusal::kIntermediates, "the copy path refuses");
		Check(SessionRefusal({ false, false, false, false, false }) == Refusal::kModeNotDlaa, "the mode is reported first");

		// The keys: 1.0 is off, 0.25 up to below 1 crops, anything else is out of range.
		Check(!Cropping(ResolveSetting(1.0, false).request) && !ResolveSetting(1.0, false).outOfRange, "1.0 is off, not a refusal");
		Check(ResolveSetting(0.6, false).request == Request{ 600, false }, "0.6 is 600 thousandths");
		Check(ResolveSetting(0.75, true).request == Request{ 750, true }, "motion rides with the scale");
		Check(ResolveSetting(0.25, false).request == Request{ 250, false }, "the floor is honoured");
		Check(ResolveSetting(0.2, false).outOfRange && !Cropping(ResolveSetting(0.2, false).request), "below the floor is out of range");
		Check(ResolveSetting(1.5, false).outOfRange, "above one is out of range");
		Check(ResolveSetting(std::numeric_limits<double>::quiet_NaN(), false).outOfRange, "not a number is out of range");
		Check(!ResolveSetting(1.0, true).request.moving, "a whole eye never moves");
		Check(Scale(Request{ 600, false }) == 0.6f, "600 thousandths is the scale literal 0.6f");

		// The cycle: the first press is the whole eye, then 75, 60, 50, 60 moving, and round.
		Check(NextCycleIndex(-1) == 0 && NextCycleIndex(0) == 1 && NextCycleIndex(3) == 4 && NextCycleIndex(4) == 0,
			"the cycle starts at the whole eye and wraps");
		Selection selection{};
		Check(Effective(selection) == Request{}, "before any apply nothing crops");
		selection = ApplySetting(selection, ResolveSetting(1.0, false), Refusal::kNone);
		const Request expected[] = { { 1000, false }, { 750, false }, { 600, false }, { 500, false }, { 600, true }, { 1000, false } };
		for (const auto& step : expected) {
			const auto press = ApplyCycle(selection);
			Check(!press.refused, "a press in the DLAA product is never refused");
			selection = press.selection;
			Check(Effective(selection) == step, "each press selects the next step of the cycle");
		}

		// An apply that leaves the keys alone keeps the hotkey's crop; editing them hands the
		// choice back to the keys and restarts the cycle.
		selection = ApplyCycle(selection).selection;
		Check(Effective(selection) == Request{ 750, false }, "one more press: 75%");
		selection = ApplySetting(selection, ResolveSetting(1.0, false), Refusal::kNone);
		Check(Effective(selection) == Request{ 750, false }, "an unrelated apply keeps the hotkey's crop");
		selection = ApplySetting(selection, ResolveSetting(0.5, false), Refusal::kNone);
		Check(Effective(selection) == Request{ 500, false } && !selection.hotkeyActive, "an edited key takes over");
		Check(Effective(ApplyCycle(selection).selection) == Request{}, "and the next press starts the cycle again");

		// A session that refuses: no crop, the press is refused and changes nothing, and the
		// refusal is reported only when a crop was wanted.
		auto refused = ApplySetting(selection, ResolveSetting(0.6, false), Refusal::kModeNotDlaa);
		Check(Effective(refused) == Request{} && ReportedRefusal(refused) == Refusal::kModeNotDlaa, "a refused session crops nothing, out loud");
		const auto refusedPress = ApplyCycle(refused);
		Check(refusedPress.refused && Effective(refusedPress.selection) == Request{}, "a press outside the product is refused");
		Check(ReportedRefusal(ApplySetting(refused, ResolveSetting(1.0, false), Refusal::kModeNotDlaa)) == Refusal::kNone,
			"a whole-eye setting in a reduced mode is not a refusal");
		auto outOfRange = ApplySetting(Selection{}, ResolveSetting(0.1, false), Refusal::kNone);
		Check(Effective(outOfRange) == Request{} && ReportedRefusal(outOfRange) == Refusal::kOutOfRange, "out of range is reported");
		Check(ReportedRefusal(ApplyCycle(outOfRange).selection) == Refusal::kNone, "until the hotkey supersedes it");

		// The packed atomic round-trips every request and generation, and reads anything
		// malformed as whole.
		for (const auto& step : kCycle) {
			Check(Unpack(Pack(step)) == (Cropping(step) ? step : Request{}), "pack round trip");
			Check(Unpack(Pack(step, 12345u)) == (Cropping(step) ? step : Request{}) && UnpackGeneration(Pack(step, 12345u)) == 12345u,
				"the generation rides beside the request without touching it");
		}
		Check(Unpack(0) == Request{} && Unpack(0x10000u) == Request{} && Unpack(1500u) == Request{}, "malformed packs read whole");
		Check(UnpackGeneration(Pack({}, kGenerationLimit)) == 0 && UnpackGeneration(Pack({}, kGenerationLimit - 1)) == kGenerationLimit - 1,
			"the generation wraps at its limit");

		// Every accepted press is a new generation, even one that re-selects the crop in
		// effect; a refused press and a settings apply are not.
		auto pressed = ApplySetting(Selection{}, ResolveSetting(1.0, false), Refusal::kNone);
		Check(pressed.generation == 0, "no press, no generation");
		const auto firstPress = ApplyCycle(pressed);
		Check(Effective(firstPress.selection) == Effective(pressed) && firstPress.selection.generation == 1,
			"the first press re-selects the whole eye, and is still a press of its own");
		pressed = ApplySetting(firstPress.selection, ResolveSetting(1.0, false), Refusal::kNone);
		Check(pressed.generation == 1, "an unrelated apply keeps the generation");
		Check(ApplyCycle(ApplySetting(pressed, ResolveSetting(1.0, false), Refusal::kModeNotDlaa)).selection.generation == 1,
			"a refused press is not a generation");

		// The render thread restarts the measurement on a change, a press or a gap, and asks
		// for a history reset only on a change.
		auto observation = Observe(false, {}, 0, {}, 0, 0);
		Check(!observation.changed && observation.restart && !observation.requestReset && observation.settle == kSettleFrames,
			"a whole-eye launch restarts the measurement and resets nothing");
		observation = Observe(false, {}, 0, Request{ 600, false }, 0, 0);
		Check(observation.changed && observation.restart && observation.requestReset && observation.settle == kSettleFrames, "a cropped launch resets and settles");
		observation = Observe(true, Request{ 600, false }, 3, Request{ 600, false }, 3, 7);
		Check(!observation.changed && !observation.restart && observation.settle == 7, "an unchanged crop keeps counting");
		observation = Observe(true, Request{ 600, false }, 3, Request{ 600, true }, 3, 7);
		Check(observation.changed && observation.restart && observation.requestReset && observation.settle == kSettleFrames, "motion alone is a change");
		observation = Observe(true, {}, 0, {}, 1, 0);
		Check(!observation.changed && observation.restart && !observation.requestReset && observation.settle == kSettleFrames,
			"a press that re-selects the crop in effect restarts without a reset");
		observation = Observe(false, Request{ 600, false }, 3, Request{ 600, false }, 3, 0);
		Check(observation.restart && observation.settle == kSettleFrames, "the first product frame after a gap restarts the measurement");
		std::uint32_t settle = kSettleFrames;
		unsigned windowResets = 0;
		unsigned frames = 0;
		while (settle != 0) {
			const auto tick = TickSettle(settle);
			settle = tick.settle;
			windowResets += tick.resetWindow ? 1u : 0u;
			++frames;
		}
		Check(frames == kSettleFrames && windowResets == 1, "the window resets once, on the last settle frame");
		Check(!TickSettle(0).resetWindow && TickSettle(0).settle == 0, "an idle settle stays idle");
	}
}

int main()
{
	FoveationRules();
	const auto active = Active();
	RejectionMatrix(active);
	TicketOwnership(active);
	Boundaries(active);
	Recovery(active);
	LateCellRecovery(active);
	HistoryContinuity(active);
	NativeOperatorVeto();
	// Synthetic reconstruction of log 6's three eligible cell changes. The log itself
	// does not contain the worldspace/movement/completion observations needed to qualify.
	auto state = active;
	for (unsigned crossing = 1; crossing <= 3; ++crossing) {
		const auto step = AdvanceWorldFrame(state, true, 10 + crossing, 5,
			Sample(2 + crossing, 1016 + crossing * 14), true);
		Check(step.continued && step.admitted && !step.requestReset, "repeated qualified crossing keeps Quality active");
		Check(step.state.stableFrames == 16 && !step.activated, "continuation does not fake a new stabilization");
		state = step.state;
		CompleteWorldFrame(state, MakeTicket(state));
	}
	std::cout << "PASS: " << g_checks << " world-admission checks\n";
}
