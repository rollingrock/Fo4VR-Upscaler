#include "Upscaler/FoveationPolicy.h"
#include "Upscaler/NativeVetoPolicy.h"
#include "Upscaler/ReflectionFixPolicy.h"
#include "Upscaler/WorldAdmissionPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

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

	using Upscaler::HudMessages::Detail::DecideReflectionFixOff;
	using Upscaler::HudMessages::Detail::PollReflectionFix;
	using Upscaler::HudMessages::Detail::ReflectionFixFacts;
	using Upscaler::HudMessages::Detail::ReflectionFixOff;
	using Upscaler::HudMessages::Detail::ReflectionFixOffLine;
	using Upscaler::HudMessages::Detail::ReflectionFixSessionEligible;

	// What PollFrame keeps between frames: the latch and the eligibility captured at the
	// active edge. PollReflectionFix decides everything else, the latch's next value included.
	struct ReflectionFixSession
	{
		bool reported = false;
		bool reducedSession = false;
		unsigned lines = 0;
		ReflectionFixOff last = ReflectionFixOff::kNone;
	};

	ReflectionFixSession SessionArmedIn(std::string_view a_sessionArmedMode)
	{
		ReflectionFixSession session{};
		session.reducedSession = ReflectionFixSessionEligible(a_sessionArmedMode);
		return session;
	}

	// One frame through the same step PollFrame calls. True when it queued a line.
	bool ReflectionFixFrame(ReflectionFixSession& a_session, bool a_worldAdmitted, bool a_settingOn,
		bool a_decided, bool a_qualified, bool a_mismatch, bool a_hookDeclined = false)
	{
		ReflectionFixFacts facts{};
		facts.reported = a_session.reported;
		facts.reducedSession = a_session.reducedSession;
		facts.worldAdmitted = a_worldAdmitted;
		facts.settingOn = a_settingOn;
		facts.hookDeclined = a_hookDeclined;
		facts.packageDecided = a_decided;
		facts.packageQualified = a_qualified;
		facts.tableMismatch = a_mismatch;
		const auto poll = PollReflectionFix(facts);
		a_session.reported = poll.reported;
		if (poll.queue == ReflectionFixOff::kNone) {
			return false;
		}
		++a_session.lines;
		a_session.last = poll.queue;
		return true;
	}

	// The rules as they stood before the hook was an input: setting, package verdict,
	// package, table. Kept as a reference so the table below can show that with the hook
	// installed nothing about the other reasons moved.
	constexpr ReflectionFixOff DecideWithoutHook(bool a_reduced, bool a_setting, bool a_decided, bool a_qualified, bool a_mismatch)
	{
		if (!a_reduced) {
			return ReflectionFixOff::kNone;
		}
		if (!a_setting) {
			return ReflectionFixOff::kSettings;
		}
		if (!a_decided) {
			return ReflectionFixOff::kNone;
		}
		if (!a_qualified) {
			return ReflectionFixOff::kShaderPackage;
		}
		return a_mismatch ? ReflectionFixOff::kShaderTable : ReflectionFixOff::kNone;
	}

	void ReflectionFixLine()
	{
		unsigned fired = 0;
		for (unsigned mask = 0; mask < 64; ++mask) {
			const bool reduced = (mask & 1) != 0;
			const bool setting = (mask & 2) != 0;
			const bool hookDeclined = (mask & 4) != 0;
			const bool decided = (mask & 8) != 0;
			const bool qualified = (mask & 16) != 0;
			const bool mismatch = (mask & 32) != 0;
			const auto reason = DecideReflectionFixOff(reduced, setting, hookDeclined, decided, qualified, mismatch);
			Check(reduced || reason == ReflectionFixOff::kNone, "DLAA and off never get the line");
			Check(!(reduced && !setting) || reason == ReflectionFixOff::kSettings, "the setting off is named first");
			Check(!(reduced && setting && hookDeclined) || reason == ReflectionFixOff::kHookDeclined,
				"a declined hook is named next, whatever the package and the table say");
			Check(!(reduced && setting && !hookDeclined && !decided) || reason == ReflectionFixOff::kNone, "no finished package verdict, no claim");
			Check(!(reduced && setting && !hookDeclined && decided && !qualified) || reason == ReflectionFixOff::kShaderPackage,
				"a package that did not qualify is named, whatever the table says");
			Check(!(reduced && setting && !hookDeclined && decided && qualified) ||
					  reason == (mismatch ? ReflectionFixOff::kShaderTable : ReflectionFixOff::kNone),
				"an armed, hooked correction gets the line only for a table mismatch");
			Check(ReflectionFixOffLine(reason).empty() == (reason == ReflectionFixOff::kNone), "a line exactly when there is a reason");
			Check(hookDeclined || reason == DecideWithoutHook(reduced, setting, decided, qualified, mismatch),
				"with the hook installed, the setting, package and table reasons are unchanged");
			fired += reason != ReflectionFixOff::kNone ? 1u : 0u;
		}
		// Of the 32 reduced rows: 16 with the setting off, 8 declined hooks, then of the 8
		// left 2 failed packages and 1 mismatch.
		Check(fired == 27, "27 of 64 rows fire");

		// The per-frame step over every combination of its eight facts.
		unsigned queued = 0;
		for (unsigned mask = 0; mask < 256; ++mask) {
			ReflectionFixFacts facts{};
			facts.reported = (mask & 1) != 0;
			facts.reducedSession = (mask & 2) != 0;
			facts.worldAdmitted = (mask & 4) != 0;
			facts.settingOn = (mask & 8) != 0;
			facts.hookDeclined = (mask & 16) != 0;
			facts.packageDecided = (mask & 32) != 0;
			facts.packageQualified = (mask & 64) != 0;
			facts.tableMismatch = (mask & 128) != 0;
			const auto poll = PollReflectionFix(facts);
			const auto reason = DecideReflectionFixOff(facts.reducedSession && facts.worldAdmitted, facts.settingOn,
				facts.hookDeclined, facts.packageDecided, facts.packageQualified, facts.tableMismatch);
			Check(!facts.reported || (poll.queue == ReflectionFixOff::kNone && poll.reported), "a closed latch queues nothing and stays closed");
			Check(facts.reported || poll.queue == reason, "an open latch queues exactly the decision");
			Check(poll.reported == (facts.reported || poll.queue != ReflectionFixOff::kNone), "only a queued line closes the latch");
			Check((facts.reducedSession && facts.worldAdmitted) || poll.queue == ReflectionFixOff::kNone,
				"only an admitted frame of a reduced session queues");
			queued += poll.queue != ReflectionFixOff::kNone ? 1u : 0u;
		}
		// The 27 firing rows, open latch, eligible and admitted: one each.
		Check(queued == 27, "27 of 256 polls queue");

		// Eligibility comes from the mode the first apply armed, never the live one.
		using namespace std::string_view_literals;
		constexpr std::string_view kEligible[] = { "quality"sv, "balanced"sv, "performance"sv, "ultraperformance"sv };
		constexpr std::string_view kIneligible[] = { "dlaa"sv, "off"sv, ""sv, "Balanced"sv, "nonsense"sv };
		for (const auto mode : kEligible) {
			Check(ReflectionFixSessionEligible(mode), "a session armed in a reduced mode is eligible");
		}
		for (const auto mode : kIneligible) {
			Check(!ReflectionFixSessionEligible(mode), "a session armed in DLAA, in off or in nothing known is not");
		}

		constexpr ReflectionFixOff kReasons[] = {
			ReflectionFixOff::kSettings, ReflectionFixOff::kShaderPackage, ReflectionFixOff::kShaderTable,
			ReflectionFixOff::kHookDeclined
		};
		for (const auto reason : kReasons) {
			const auto line = ReflectionFixOffLine(reason);
			Check(line.starts_with("BlurAway VR: reflection fix off ("sv), "every line opens the same way");
			Check(line.find("reflections may differ per eye"sv) != std::string_view::npos, "every line says what the player sees");
			Check(line.ends_with("DLAA is unaffected."sv), "every line says DLAA is unaffected");
			for (const auto other : kReasons) {
				Check(other == reason || ReflectionFixOffLine(other) != line, "each reason has its own line");
			}
		}
		Check(ReflectionFixOffLine(ReflectionFixOff::kSettings).find("settings"sv) != std::string_view::npos,
			"the settings line says it was the settings");
		// An unreadable Data folder or a recompressed vanilla archive fails the same check,
		// so the line says what the check found and calls a mod only the likely cause.
		Check(ReflectionFixOffLine(ReflectionFixOff::kShaderPackage).find("don't match the tested ones, likely a shader mod"sv) != std::string_view::npos,
			"the package line states what the check found, not a cause");
		// The byte check sees changed code, not who changed it; another mod is the likely cause.
		Check(ReflectionFixOffLine(ReflectionFixOff::kHookDeclined).find("already changed, likely by another mod"sv) != std::string_view::npos,
			"the hook line states what the byte check found, not a cause");

		// A shader mod at launch: the verdict is in before any world frame. The stabilizing
		// frames are not admitted and say nothing; the first admitted frame fires, once.
		ReflectionFixSession shaderMod = SessionArmedIn("quality");
		for (unsigned frame = 0; frame < 16; ++frame) {
			Check(!ReflectionFixFrame(shaderMod, false, true, true, false, false), "a frame not yet admitted says nothing");
		}
		Check(ReflectionFixFrame(shaderMod, true, true, true, false, false), "the first admitted frame fires");
		for (unsigned frame = 0; frame < 100; ++frame) {
			Check(!ReflectionFixFrame(shaderMod, true, true, true, false, false), "and no frame after it");
		}
		Check(shaderMod.lines == 1 && shaderMod.last == ReflectionFixOff::kShaderPackage, "one shader-mod line");

		// The table is read at the correction's first composite draw, which can land after the
		// first admitted frame. Those frames are an armed, healthy correction until it does.
		ReflectionFixSession table = SessionArmedIn("balanced");
		Check(!ReflectionFixFrame(table, true, true, true, true, false), "armed and not yet read: no line");
		Check(!ReflectionFixFrame(table, true, true, true, true, false), "still no line");
		Check(ReflectionFixFrame(table, true, true, true, true, true), "the mismatch fires on the next admitted frame");
		Check(!ReflectionFixFrame(table, true, true, true, true, true), "once");
		Check(table.lines == 1 && table.last == ReflectionFixOff::kShaderTable, "one table line");

		// Healthy for a whole session: nothing, ever.
		ReflectionFixSession healthy = SessionArmedIn("performance");
		for (unsigned frame = 0; frame < 1000; ++frame) {
			Check(!ReflectionFixFrame(healthy, (frame % 7) != 0, true, true, true, false), "a healthy correction never fires");
		}
		Check(healthy.lines == 0, "no line for a healthy session");

		// DLAA and off sessions: not eligible, so nothing fires even on a frame that reports
		// the world admitted, whatever the other facts say.
		for (const auto mode : { "dlaa"sv, ""sv }) {
			ReflectionFixSession ineligible = SessionArmedIn(mode);
			for (unsigned mask = 0; mask < 64; ++mask) {
				Check(!ReflectionFixFrame(ineligible, (mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0, (mask & 16) != 0, (mask & 32) != 0),
					"a DLAA or off session never fires");
			}
			Check(ineligible.lines == 0 && !ineligible.reported, "no line, and the latch stays open");
		}

		// The race the armed mode closes: a reduced session switched to off on the very frame
		// of its first admission. Off admits no world frame, so the line waits; the session is
		// still eligible, so it fires on the first admitted frame once a reduced mode is back.
		ReflectionFixSession switched = SessionArmedIn("balanced");
		for (unsigned frame = 0; frame < 30; ++frame) {
			Check(!ReflectionFixFrame(switched, false, false, true, true, false), "off admits nothing, so nothing fires");
		}
		Check(ReflectionFixFrame(switched, true, false, true, true, false), "back in a reduced mode, the line fires");
		Check(switched.lines == 1 && switched.last == ReflectionFixOff::kSettings, "one settings line after the round trip");

		// A vanilla install mid-scan: checked but not yet decided, and Qualified() still its
		// initial false. No line; the verdict that follows qualifies, and still no line.
		ReflectionFixSession scanning = SessionArmedIn("quality");
		for (unsigned frame = 0; frame < 30; ++frame) {
			Check(!ReflectionFixFrame(scanning, true, true, false, false, false), "a scan in progress is no verdict");
		}
		Check(!ReflectionFixFrame(scanning, true, true, true, true, false), "the qualified verdict says nothing");
		Check(scanning.lines == 0 && !scanning.reported, "a vanilla install never gets the line");

		// Turned off live after a healthy start: one settings line, and toggling it again
		// says nothing more.
		ReflectionFixSession toggled = SessionArmedIn("balanced");
		Check(!ReflectionFixFrame(toggled, true, true, true, true, false), "healthy start");
		Check(ReflectionFixFrame(toggled, true, false, true, true, false), "turning it off fires");
		Check(!ReflectionFixFrame(toggled, true, true, true, true, false), "turning it back on is silent");
		Check(!ReflectionFixFrame(toggled, true, false, true, true, false), "and so is turning it off again");
		Check(toggled.lines == 1 && toggled.last == ReflectionFixOff::kSettings, "one settings line");

		// Off at launch with a shader mod installed: the package is never checked, and the
		// line names the setting.
		ReflectionFixSession both = SessionArmedIn("ultraperformance");
		Check(ReflectionFixFrame(both, true, false, false, false, false), "the setting off fires without a package verdict");
		Check(both.last == ReflectionFixOff::kSettings, "and names the setting");

		// The field case: Balanced, the package qualified and the correction armed, but
		// another plugin's jump already sat on the helper entry, so the hook declined at
		// load. Without the hook input every frame read as healthy and nothing was said.
		ReflectionFixSession hooked = SessionArmedIn("balanced");
		for (unsigned frame = 0; frame < 16; ++frame) {
			Check(!ReflectionFixFrame(hooked, false, true, true, true, false, true), "stabilizing frames say nothing");
		}
		Check(ReflectionFixFrame(hooked, true, true, true, true, false, true), "the first admitted frame names the hook");
		Check(!ReflectionFixFrame(hooked, true, true, true, true, false, true), "once");
		Check(hooked.lines == 1 && hooked.last == ReflectionFixOff::kHookDeclined, "one hook line");

		// The hook is settled at load, so the line does not wait for the package scan, and
		// with a shader mod as well it still names the hook: no package can arm a
		// correction that has no way in.
		ReflectionFixSession hookedScanning = SessionArmedIn("quality");
		Check(ReflectionFixFrame(hookedScanning, true, true, false, false, false, true), "a declined hook fires mid-scan");
		Check(hookedScanning.last == ReflectionFixOff::kHookDeclined, "and names the hook");
		ReflectionFixSession hookedAndModded = SessionArmedIn("performance");
		Check(ReflectionFixFrame(hookedAndModded, true, true, true, false, false, true), "a declined hook and a failed package");
		Check(hookedAndModded.last == ReflectionFixOff::kHookDeclined, "name the hook");

		// The player's own setting still comes first.
		ReflectionFixSession hookedButOff = SessionArmedIn("balanced");
		Check(ReflectionFixFrame(hookedButOff, true, false, true, true, false, true), "the setting off with a declined hook");
		Check(hookedButOff.last == ReflectionFixOff::kSettings, "names the setting");
	}
	void ScopeMenuReset()
	{
		Check(!MenuEventResets("ScopeMenu", true, true, false), "with a live feed, ScopeMenu follows resetOnScope off");
		Check(MenuEventResets("ScopeMenu", true, false, true), "with a live feed, ScopeMenu follows resetOnScope on");
		Check(MenuEventResets("ScopeMenu", false, true, false), "before the feed steps, the game's ScopeMenu follows resetOnMenu");
		Check(!MenuEventResets("ScopeMenu", false, false, true), "before the feed steps, resetOnScope does not reach ScopeMenu");
		Check(MenuEventResets("PipboyMenu", true, true, false), "other menus follow resetOnMenu");
		Check(!MenuEventResets("PipboyMenu", true, false, true), "resetOnScope does not reach other menus");
		Check(MenuEventResets("", true, true, false), "an unnamed menu follows resetOnMenu");
		Check(!MenuEventResets("scopemenu", true, false, true), "the menu name match is exact");
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

		// Phase 2a's toggle: foveated against full eye at the keys' size, and back; refused
		// where there is nothing to toggle to; a key edit hands the choice back to the keys.
		{
			auto toggled = ApplySetting(Selection{}, ResolveSetting(0.85, false), Refusal::kNone);
			Check(Effective(toggled) == Request{ 850, false }, "the 2a keys crop at their size");
			auto press = ApplyToggle(toggled);
			Check(!press.refused && Effective(press.selection) == Request{} && press.selection.generation == 1, "a toggle press goes to the full eye");
			press = ApplyToggle(press.selection);
			Check(!press.refused && Effective(press.selection) == Request{ 850, false } && press.selection.generation == 2, "and back to the crop");
			Check(Effective(ApplySetting(ApplyToggle(press.selection).selection, ResolveSetting(0.85, false), Refusal::kNone)) == Request{}, "an unrelated apply keeps the toggle's full eye");
			const auto edited = ApplySetting(ApplyToggle(press.selection).selection, ResolveSetting(0.75, false), Refusal::kNone);
			Check(Effective(edited) == Request{ 750, false } && !edited.hotkeyActive && !edited.hotkeyToggle, "an edited key takes over from the toggle");
			Check(ApplyToggle(ApplySetting(Selection{}, ResolveSetting(1.0, false), Refusal::kNone)).refused, "no crop asked for, nothing to toggle to");
			Check(ApplyToggle(ApplySetting(Selection{}, ResolveSetting(0.85, false), Refusal::kModeNotDlaa)).refused, "a toggle outside the product is refused");
			const auto cycled = ApplyCycle(ApplyToggle(toggled).selection);
			Check(!cycled.selection.hotkeyToggle && Effective(cycled.selection) == Request{}, "a cycle press after a toggle starts the cycle at the whole eye");
		}

		// Phase 2a's key answer: the toggle key toggles the product and nothing else; the cycle
		// key cycles, and toggles while the product owns the crop.
		{
			Check(AnswerFoveationKey(true, true, false) == KeyAnswer::kToggle, "the toggle key toggles the product");
			Check(AnswerFoveationKey(true, false, false) == KeyAnswer::kProductOff, "the toggle key with foveation off is refused, no cycle");
			Check(AnswerFoveationKey(true, false, true) == KeyAnswer::kMeasurementOverrides, "the toggle key under the measurement key is refused, no cycle");
			Check(AnswerFoveationKey(false, false, false) == KeyAnswer::kCycle && AnswerFoveationKey(false, false, true) == KeyAnswer::kCycle,
				"the cycle key cycles without the product, whatever foveation says");
			Check(AnswerFoveationKey(false, true, false) == KeyAnswer::kToggle, "the cycle key toggles while the product owns the crop");
			Check(AnswerFoveationKey(true, true, false, true) == KeyAnswer::kSeamFailed && AnswerFoveationKey(false, true, false, true) == KeyAnswer::kSeamFailed,
				"after the fail-soft either key is refused");
			Check(AnswerFoveationKey(false, false, false, true) == KeyAnswer::kCycle, "the fail-soft is the product's alone");
			Check(KeyAnswerRefusal(KeyAnswer::kProductOff) == std::string_view{ "foveation is off (foveation = false)" } &&
					  KeyAnswerRefusal(KeyAnswer::kMeasurementOverrides) == std::string_view{ "foveationCropScale below 1.0 overrides foveation" } &&
					  KeyAnswerRefusal(KeyAnswer::kToggle).empty() && KeyAnswerRefusal(KeyAnswer::kCycle).empty(),
				"each refusal says why");
		}

		// Phase 2a's fail-soft: one failure is nothing, three in a row end foveation, an
		// intermittent fault ends it, isolated ones never accumulate, a build failure ends it
		// at once, and the end is final.
		{
			auto observe = [](SeamHealth a_health, const char* a_pattern) {
				for (const char* c = a_pattern; *c != '\0'; ++c) {
					a_health = ObserveSeamBlend(a_health, *c == 'b', false);
				}
				return a_health;
			};
			auto health = observe({}, "f");
			Check(!health.failedSoft && health.score == kSeamFailureWeight && health.failures == 1, "one failure keeps foveation on");
			Check(!observe({}, "ff").failedSoft && observe({}, "fff").failedSoft && observe({}, "fff").cause == SeamFailure::kBlends,
				"three failures in a row turn it off");
			Check(!observe({}, "ffbbbbbbbbbbbbbbbbbbbbbf").failedSoft, "two failures, then twenty blends, then one: the score drained");
			Check(observe({}, "fbfbfbf").failedSoft && !observe({}, "fbfbfb").failedSoft, "a failure every other frame turns it off on the seventh frame");
			std::string oneInEleven;
			std::string oneInTen;
			for (int i = 0; i < 400; ++i) {
				oneInEleven += i % 11 == 0 ? 'f' : 'b';
				oneInTen += i % 10 == 0 ? 'f' : 'b';
			}
			Check(!observe({}, oneInEleven.c_str()).failedSoft, "one failure in eleven frames never accumulates");
			Check(observe({}, oneInTen.c_str()).failedSoft, "one in ten does, in the end");
			health = ObserveSeamBlend({}, false, true);
			Check(health.failedSoft && health.cause == SeamFailure::kBuild, "a build failure turns it off at once");
			Check(observe(health, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb").failedSoft, "and nothing turns it back on");
			Check(SeamFailureName(SeamFailure::kBlends) == std::string_view{ "blends" } && SeamFailureName(SeamFailure::kBuild) == std::string_view{ "build" },
				"/state names the cause");
		}

		// Phase 2a's reset fade: no reset, no fade; a reset ramps the box 0, 1/N ... 1; a reset
		// during the ramp restarts it; 0 is off; resets the seam did not see count the same.
		{
			constexpr std::uint32_t kN = 10;
			auto step = ObserveResetFade({}, 0, kN);
			Check(step.alpha == 1.0f && !step.started, "no reset ever: the box is DLAA");
			ResetFade fade = step.next;
			step = ObserveResetFade(fade, 1, kN);
			Check(step.started && step.alpha == 0.0f, "the reset frame shows the engine's image");
			fade = step.next;
			bool ramp = true;
			for (std::uint32_t k = 1; k <= kN + 3; ++k) {
				step = ObserveResetFade(fade, 1, kN);
				fade = step.next;
				const float want = k >= kN ? 1.0f : static_cast<float>(k) / static_cast<float>(kN);
				ramp = ramp && !step.started && step.alpha == want;
			}
			Check(ramp, "then k / N each frame, and 1 from frame N on");
			fade = ObserveResetFade(ObserveResetFade(ObserveResetFade(fade, 2, kN).next, 2, kN).next, 2, kN).next;
			step = ObserveResetFade(fade, 3, kN);
			Check(step.started && step.alpha == 0.0f, "a reset during the ramp restarts it");
			step = ObserveResetFade(step.next, 7, kN);
			Check(step.started && step.alpha == 0.0f, "resets the seam did not see restart it the same");
			step = ObserveResetFade({}, 5, 0);
			Check(!step.started && step.alpha == 1.0f, "0 frames is off");
			step = ObserveResetFade({ 5, 3 }, 5, 2);
			Check(step.alpha == 1.0f, "a shorter fade set mid-ramp ends at once");
			Check(kDefaultResetFadeFrames == 0 && kMaxResetFadeFrames == 120, "the default is off, and the cap");
			step = ObserveResetFade({}, 1, kDefaultResetFadeFrames);
			Check(!step.started && step.alpha == 1.0f, "at the default a reset starts no fade");
		}

		// Phase 2a's size key: honoured in [0.5, 0.95], and an out-of-range value is refused
		// as foveationSize, not as the measurement key.
		{
			Check(ResolveProductSize(0.75).request == Request{ 750, false } && !ResolveProductSize(0.75).outOfRange, "0.75, the default, is a 75% box");
			Check(ResolveProductSize(0.85).request == Request{ 850, false } && !ResolveProductSize(0.85).outOfRange, "0.85 is an 85% box");
			Check(ResolveProductSize(0.5).request == Request{ 500, false } && ResolveProductSize(0.95).request == Request{ 950, false }, "both ends are honoured");
			for (const double bad : { 0.3, 0.97, 1.0, 0.0, -1.0, std::numeric_limits<double>::quiet_NaN() }) {
				const auto r = ResolveProductSize(bad);
				Check(r.outOfRange && !Cropping(r.request) && r.rangeRefusal == Refusal::kSizeOutOfRange, "a size outside 0.5-0.95 is refused as foveationSize");
			}
			Check(ReportedRefusal(ApplySetting(Selection{}, ResolveProductSize(0.97), Refusal::kNone)) == Refusal::kSizeOutOfRange, "the refusal names foveationSize");
			Check(RefusalName(Refusal::kSizeOutOfRange) == std::string_view{ "foveationSize is out of range (0.5 to 0.95)" }, "and says its range");
			Check(ReportedRefusal(ApplySetting(Selection{}, ResolveSetting(1.5, false), Refusal::kNone)) == Refusal::kOutOfRange, "the measurement key is still named as itself");
			Check(ReportedRefusal(ApplySetting(Selection{}, ResolveProductSize(0.85), Refusal::kModeNotDlaa)) == Refusal::kModeNotDlaa, "a session refusal is still the session's");
		}

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

	// The render thread's foveation bookkeeping, as HDRRoundTrip drives ObserveInPlay on each
	// product frame and TickSettle on each published one, counting what it would log and ask for.
	struct FoveationThread
	{
		bool seen = false;
		Upscaler::Foveation::Request last{};
		std::uint32_t lastGeneration = 0;
		std::uint32_t settle = 0;
		unsigned restartLines = 0;
		unsigned changeLines = 0;
		unsigned historyResets = 0;
		unsigned windowResets = 0;

		// A frame that did not reach the observation: a load, a compare-key veto, a skip.
		void Gap() { seen = false; }

		void Frame(const Upscaler::Foveation::Request& a_current, std::uint32_t a_generation, bool a_held)
		{
			using namespace Upscaler::Foveation;
			const auto observation = ObserveInPlay(seen, last, lastGeneration, a_current, a_generation, a_held, settle);
			seen = true;
			last = a_current;
			lastGeneration = a_generation;
			settle = observation.settle;
			restartLines += observation.restart && !observation.changed ? 1u : 0u;
			changeLines += observation.changed ? 1u : 0u;
			historyResets += observation.requestReset ? 1u : 0u;
			const auto tick = TickSettle(settle);
			settle = tick.settle;
			windowResets += tick.resetWindow ? 1u : 0u;
		}

		[[nodiscard]] unsigned Lines() const { return restartLines + changeLines + windowResets; }
	};

	// Foveation off is silent: with every key at its default a DLAA session observes nothing
	// through any number of gaps, and every way out of play (off live, the fail-soft, a key
	// edit) asks for at most the one reset the crop's end needs and opens no settle.
	void FoveationOffPath()
	{
		using namespace Upscaler::Foveation;
		const Request box{ 750, false };

		// Every key at its default, a gap every 37 frames: the headless keys-off log's lines
		// 146 and 1446 ("measurement restarted", then "settled ..., perf window reset") came
		// from exactly this, through Observe ungated.
		{
			FoveationThread thread;
			for (unsigned frame = 0; frame < 2000; ++frame) {
				if (frame % 37 == 0) {
					thread.Gap();
				}
				thread.Frame({}, 0, false);
			}
			Check(thread.Lines() == 0 && thread.historyResets == 0 && thread.settle == 0, "foveation off logs nothing and resets nothing through gaps");
			const auto ungated = Observe(false, {}, 0, {}, 0, 0);
			Check(ungated.restart && ungated.settle == kSettleFrames, "ungated, the same gap restarted the measurement");
			Check(!InPlay({}, false, {}) && InPlay(box, false, {}) && InPlay({}, true, {}) && InPlay({}, false, box), "what puts foveation in play");
		}

		// The product on, then off live from the box: one change and its reset, no settle, and
		// silence after it however many gaps follow.
		{
			FoveationThread thread;
			for (unsigned frame = 0; frame < 200; ++frame) {
				thread.Frame(box, 0, false);
			}
			Check(thread.changeLines == 1 && thread.historyResets == 1 && thread.windowResets == 1, "the box: one change, one reset, one settled window");
			thread.Frame({}, 0, false);
			Check(thread.changeLines == 2 && thread.historyResets == 2 && thread.settle == 0, "off live: the crop's end is a change with its reset, and no settle");
			const auto lines = thread.Lines();
			for (unsigned frame = 0; frame < 1000; ++frame) {
				if (frame % 50 == 0) {
					thread.Gap();
				}
				thread.Frame({}, 0, false);
			}
			Check(thread.Lines() == lines && thread.historyResets == 2 && thread.settle == 0, "and silence after it");
		}

		// Off live in the middle of a settle: the settle is dropped, so no window reset follows.
		{
			FoveationThread thread;
			thread.Frame(box, 0, false);
			for (unsigned frame = 0; frame < 20; ++frame) {
				thread.Frame(box, 0, false);
			}
			Check(thread.settle != 0, "the settle is running");
			for (unsigned frame = 0; frame < 200; ++frame) {
				thread.Frame({}, 0, false);
			}
			Check(thread.windowResets == 0 && thread.settle == 0, "leaving play mid-settle drops it");
		}

		// The fail-soft: HDRRoundTrip hands the frame no request and no held choice, even after
		// toggle presses, so it leaves play exactly as off live does.
		{
			FoveationThread thread;
			thread.Frame(box, 0, false);
			thread.Frame({}, 1, true);
			thread.Frame(box, 2, true);
			const auto resets = thread.historyResets;
			thread.Frame({}, 2, false);
			Check(thread.historyResets == resets + 1 && thread.settle == 0, "the fail-soft's crop end resets once and settles nothing");
			const auto lines = thread.Lines();
			for (unsigned frame = 0; frame < 500; ++frame) {
				if (frame % 40 == 0) {
					thread.Gap();
				}
				thread.Frame({}, 2, false);
			}
			Check(thread.Lines() == lines, "and is silent after");
		}

		// Leaving play across a gap: the crop ended while nothing was observed, so there is no
		// change to report and nothing restarts.
		{
			FoveationThread thread;
			thread.Frame(box, 0, false);
			const auto lines = thread.Lines();
			const auto resets = thread.historyResets;
			thread.Gap();
			thread.Frame({}, 0, false);
			Check(thread.Lines() == lines && thread.historyResets == resets && thread.settle == 0, "a crop that ended in a gap leaves quietly");
		}

		// Still in play: the toggle's full eye is held, so it settles a window and restarts after
		// a gap; a cycle press at 100% from silence restarts as a press, not as a gap.
		{
			FoveationThread thread;
			thread.Frame(box, 0, false);
			thread.Frame({}, 1, true);
			Check(thread.changeLines == 2 && thread.historyResets == 2 && thread.settle != 0, "the toggle's full eye is a change in play");
			for (unsigned frame = 0; frame < kSettleFrames; ++frame) {
				thread.Frame({}, 1, true);
			}
			Check(thread.windowResets == 1, "and settles its window");
			thread.Gap();
			thread.Frame({}, 1, true);
			Check(thread.restartLines == 1 && thread.settle != 0, "and restarts after a gap");

			FoveationThread cycle;
			for (unsigned frame = 0; frame < 10; ++frame) {
				cycle.Frame({}, 0, false);
			}
			const auto press = ObserveInPlay(cycle.seen, cycle.last, cycle.lastGeneration, {}, 1, true, cycle.settle);
			Check(cycle.seen && press.restart && !press.changed && press.settle == kSettleFrames, "a cycle press at 100% restarts as a press");
			cycle.Gap();
			cycle.Frame(box, 0, false);
			Check(cycle.changeLines == 1 && cycle.historyResets == 1, "a crop after silence and a gap is a change with its reset");
		}

		// Who holds a choice, and the held bit beside the request and the generation.
		{
			const auto keys = ApplySetting(Selection{}, ResolveSetting(0.75, false), Refusal::kNone);
			Check(!KeyChoiceHeld(keys), "the keys alone hold nothing");
			const auto toggled = ApplyToggle(keys).selection;
			Check(KeyChoiceHeld(toggled) && KeyChoiceHeld(ApplyCycle(keys).selection), "a toggle or a cycle press holds");
			Check(!KeyChoiceHeld(ApplySetting(toggled, ResolveSetting(1.0, false), Refusal::kNone)), "an edited key hands the choice back");
			Check(!KeyChoiceHeld(ApplySetting(toggled, ResolveSetting(0.75, false), Refusal::kModeNotDlaa)), "a refused session holds nothing");
			const auto packed = Pack({}, kGenerationLimit - 1, true);
			Check(UnpackHeld(packed) && Unpack(packed) == Request{} && UnpackGeneration(packed) == kGenerationLimit - 1, "the held bit rides beside the request and the generation");
			Check(!UnpackHeld(Pack(box, kGenerationLimit - 1, false)) && Unpack(Pack(box, 7, true)) == box && UnpackGeneration(Pack(box, 7, true)) == 7,
				"and neither touches it");
		}
	}
}

int main()
{
	FoveationRules();
	FoveationOffPath();
	const auto active = Active();
	RejectionMatrix(active);
	TicketOwnership(active);
	Boundaries(active);
	Recovery(active);
	LateCellRecovery(active);
	HistoryContinuity(active);
	NativeOperatorVeto();
	ReflectionFixLine();
	ScopeMenuReset();
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
