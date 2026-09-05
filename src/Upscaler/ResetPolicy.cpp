#include "Upscaler/ResetPolicy.h"

#include "Upscaler/Jitter.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

namespace Upscaler::ResetPolicy
{
	namespace
	{
		// Requests may arrive on game/UI threads while the render thread is evaluating a
		// stereo pair. A generation is acknowledged only after the whole pair succeeds;
		// deriving pending state from these counters makes a racing request impossible to
		// clear accidentally.
		std::atomic<std::uint64_t> g_requestedGeneration{ 0 };
		std::atomic<std::uint64_t> g_acknowledgedGeneration{ 0 };
		std::atomic_bool g_menuSinkInstalled{ false };
		std::atomic_bool g_cellSinkInstalled{ false };
		std::atomic_bool g_resetOnMenu{ true };
		std::atomic_bool g_resetOnLoad{ true };
		std::atomic_bool g_resetOnCell{ true };
		std::atomic<std::uint64_t> g_requests{ 0 };
		std::atomic<std::uint64_t> g_coalesced{ 0 };
		std::atomic<std::uint64_t> g_consumptions{ 0 };
		std::atomic<std::uint64_t> g_acquisitions{ 0 };
		std::atomic<std::uint64_t> g_loadEvents{ 0 };
		std::atomic<std::uint64_t> g_menuOpenEvents{ 0 };
		std::atomic<std::uint64_t> g_menuCloseEvents{ 0 };
		std::atomic<std::uint64_t> g_cellEnterEvents{ 0 };
		std::atomic<std::uint64_t> g_cellLeaveEvents{ 0 };
		std::atomic<std::uint64_t> g_leftSubmissions{ 0 };
		std::atomic<std::uint64_t> g_rightSubmissions{ 0 };
		std::atomic<std::uint64_t> g_qualityAbortRequests{ 0 };
		std::atomic<std::uint32_t> g_lastCellId{ 0 };

		// Menu events are an immediate invalidation edge, but not the authority for
		// admission. A sink installed after LoadingMenu opened cannot reconstruct that
		// state, so every render-frame decision below also reads UI::menuMap under the
		// engine's own read lock.
		std::atomic<std::uint64_t> g_worldTransitionGeneration{ 1 };
		std::atomic<std::uint64_t> g_worldAdmissionChecks{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionDeniedFrames{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionActivations{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionInvalidations{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionResetRequests{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionRechecks{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionRecheckFailures{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionPublishRechecks{ 0 };
		std::atomic<std::uint64_t> g_worldAdmissionPublishVetoes{ 0 };
		// The stableFrames value the admission machine held at the moment it last activated -
		// the number of consecutive eligible frames the gate actually counted before that
		// admission. Distinguishes a correct re-entry through kStabilizing (this equals
		// kQualityWorldStableFrames) from a machine that admitted on the first eligible frame.
		// Set alongside g_worldAdmissionActivations; render thread owns both.
		std::atomic<std::uint32_t> g_worldLastActivationStableFrames{ 0 };
		std::atomic<std::uint32_t> g_worldAdmissionPhase{
			static_cast<std::uint32_t>(WorldAdmissionPhase::kBlocked)
		};
		std::atomic<std::uint32_t> g_worldBlockReason{
			static_cast<std::uint32_t>(WorldBlockReason::kNoUI)
		};
		std::atomic_bool g_worldAdmitted{ false };
		std::atomic<std::uint32_t> g_worldStableFrames{ 0 };
		std::atomic<std::uintptr_t> g_worldCell{ 0 };
		std::atomic<std::uint32_t> g_worldMenuMode{ 0 };
		std::atomic<std::uint32_t> g_worldItemMenuMode{ 0 };
		std::atomic<std::uint32_t> g_worldFreezeFrameMenuBG{ 0 };
		std::atomic<std::uint32_t> g_worldFreezeFramePause{ 0 };
		std::atomic_bool g_worldClosingAllMenus{ false };
		std::atomic<std::uint32_t> g_worldOpenBlockingMenuMask{ 0 };

		// Operator A/B veto. Written from the DevBench listener thread (and later a hotkey
		// poll); read once per frame on the render thread, see g_cachedOperatorVeto below.
		std::atomic_bool g_operatorVeto{ false };
		std::atomic<std::uint64_t> g_operatorVetoEngagements{ 0 };
		std::atomic<std::uint64_t> g_operatorVetoReleases{ 0 };
		std::atomic<std::uint64_t> g_operatorVetoDeniedFrames{ 0 };

		// A live mode switch's own deny window - see EngageModeSwitchPending. Written from
		// whatever thread drives Configuration::RequestModeSwitch or Configuration::Apply's
		// own ratio-change engagement; read once per frame on the render thread, cached the
		// same way the operator veto is (g_cachedModeSwitchPending).
		//
		// A refcount, not a bool: two independent callers in Configuration.cpp can each
		// hold this window open at once (see ResetPolicy.h's own comment on
		// EngageModeSwitchPending), and each must release exactly what it engaged. The
		// window is denying admission for as long as this is above zero - the cached read
		// below, and every other reader, compares against zero rather than reading a flag
		// directly.
		std::atomic<std::uint32_t> g_modeSwitchPendingCount{ 0 };
		// g_modeSwitchEngagements/g_modeSwitchReleases count the 0-to-1 and 1-to-0
		// transitions only, exactly as they did when this was a plain bool - a nested
		// second engagement while the window is already open, or a release that still
		// leaves another holder standing, advances neither counter, so both drivers
		// asserting on "how many times did the window actually open/close" keep working
		// unchanged.
		std::atomic<std::uint64_t> g_modeSwitchEngagements{ 0 };
		std::atomic<std::uint64_t> g_modeSwitchReleases{ 0 };
		std::atomic<std::uint64_t> g_modeSwitchDeniedFrames{ 0 };

		// The frame's single atomic load of g_operatorVeto, cached by
		// AdvanceQualityWorldAdmissionBeforeWorld. RevalidateQualityWorldAdmission and any
		// other same-frame recheck reuse this cached value instead of loading again, so a
		// listener-thread SetOperatorVeto write cannot land between two loads and split one
		// frame's admission decision. Render thread owns every read and write of this
		// variable.
		bool g_cachedOperatorVeto = false;
		// Same reasoning and same render-thread ownership, for g_modeSwitchPendingCount.
		bool g_cachedModeSwitchPending = false;
		// Same again, for the caller's own load of HDRRoundTrip's session latch. This one
		// arrives as an argument rather than being loaded here: the latch lives in another
		// translation unit and the pre-world hook that owns the dispatch is the natural
		// place for the one load the whole frame uses.
		bool g_cachedReconstructionUnavailable = false;
		// The same value again, for /state. Separate from the cache above because Snapshot
		// runs on the DevBench listener thread and the cache is a plain render-thread
		// variable: reading that one from another thread would be a race, and every other
		// field Snapshot reports is an atomic for exactly this reason.
		std::atomic<bool> g_reconstructionUnavailable{ false };
		std::atomic<std::uint64_t> g_reconstructionUnavailableDeniedFrames{ 0 };

		enum BlockingMenuMask : std::uint32_t
		{
			kMainMenuBit = 1u << 0,
			kLoadingMenuBit = 1u << 1,
			kFaderMenuBit = 1u << 2,
			kPauseMenuBit = 1u << 3,
			kPipboyMenuBit = 1u << 4,
			kMessageBoxMenuBit = 1u << 5,
		};

		struct WorldObservation
		{
			WorldBlockReason reason = WorldBlockReason::kNone;
			std::uint64_t transitionGeneration = 0;
			std::uintptr_t cell = 0;
			std::uint32_t menuMode = 0;
			std::uint32_t itemMenuMode = 0;
			std::uint32_t freezeFrameMenuBG = 0;
			std::uint32_t freezeFramePause = 0;
			bool closingAllMenus = false;
			std::uint32_t openBlockingMenuMask = 0;
		};

		struct AdmissionMachineState
		{
			WorldAdmissionPhase phase = WorldAdmissionPhase::kBlocked;
			std::uint64_t transitionGeneration = 0;
			std::uintptr_t cell = 0;
			std::uint32_t stableFrames = 0;
			bool resetLatched = false;
		};

		struct AdmissionStep
		{
			AdmissionMachineState state{};
			bool admitted = false;
			bool requestReset = false;
			bool activated = false;
		};

		// This reducer contains no engine access. The compile-time contracts below exercise
		// the exact blocked -> stable -> active behavior independently of Fallout and NGX.
		[[nodiscard]] constexpr AdmissionStep AdvanceAdmission(
			AdmissionMachineState a_previous,
			bool a_worldEligible,
			std::uint64_t a_transitionGeneration,
			std::uintptr_t a_cell,
			std::uint32_t a_requiredStableFrames = kQualityWorldStableFrames) noexcept
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
				a_previous.cell != a_cell;
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

		[[nodiscard]] constexpr std::uint32_t BlockingMenuBit(std::string_view a_name) noexcept
		{
			return a_name == "MainMenu"sv       ? kMainMenuBit :
			       a_name == "LoadingMenu"sv    ? kLoadingMenuBit :
			       a_name == "FaderMenu"sv      ? kFaderMenuBit :
			       a_name == "PauseMenu"sv      ? kPauseMenuBit :
			       a_name == "PipboyMenu"sv     ? kPipboyMenuBit :
			       a_name == "MessageBoxMenu"sv ? kMessageBoxMenuBit :
			                                          0;
		}

		[[nodiscard]] constexpr WorldBlockReason BlockingMenuReason(std::uint32_t a_mask) noexcept
		{
			// Transition/fade routes outrank ordinary modal menus when several overlap.
			return (a_mask & kLoadingMenuBit) != 0 ? WorldBlockReason::kLoadingMenu :
			       (a_mask & kFaderMenuBit) != 0 ? WorldBlockReason::kFaderMenu :
			       (a_mask & kMainMenuBit) != 0 ? WorldBlockReason::kMainMenu :
			       (a_mask & kPauseMenuBit) != 0 ? WorldBlockReason::kPauseMenu :
			       (a_mask & kPipboyMenuBit) != 0 ? WorldBlockReason::kPipboyMenu :
			       (a_mask & kMessageBoxMenuBit) != 0 ? WorldBlockReason::kMessageBoxMenu :
			                                                WorldBlockReason::kNone;
		}

		// The one place the veto overrides an observed block reason. Pulled out as a pure
		// function so ObserveWorld and OperatorVetoReducerContract's asserts below exercise
		// exactly the same code, not a copy of its logic.
		[[nodiscard]] constexpr WorldBlockReason ApplyOperatorVeto(
			WorldBlockReason a_observed, bool a_operatorVeto) noexcept
		{
			return a_operatorVeto ? WorldBlockReason::kOperatorVeto : a_observed;
		}

		static_assert(
			ApplyOperatorVeto(WorldBlockReason::kNone, true) == WorldBlockReason::kOperatorVeto);
		static_assert(
			ApplyOperatorVeto(WorldBlockReason::kMainMenu, true) ==
			WorldBlockReason::kOperatorVeto);
		static_assert(
			ApplyOperatorVeto(WorldBlockReason::kStateReadFault, true) ==
			WorldBlockReason::kOperatorVeto);
		static_assert(ApplyOperatorVeto(WorldBlockReason::kNone, false) == WorldBlockReason::kNone);
		static_assert(
			ApplyOperatorVeto(WorldBlockReason::kMainMenu, false) == WorldBlockReason::kMainMenu);
		static_assert(
			ApplyOperatorVeto(WorldBlockReason::kStateReadFault, false) ==
			WorldBlockReason::kStateReadFault);

		// The mode-switch deny window's own override, applied after ApplyOperatorVeto so
		// the operator's own A/B veto always wins the reported reason when both are active
		// at once: the operator's veto is a deliberate, explicit comparison the researcher
		// is standing in front of, and an automatic background mode switch must never
		// paper over it in /state. Both reasons deny admission identically either way (see
		// AdvanceAdmission - it only ever sees eligible/ineligible), so this ordering only
		// decides which label a simultaneous case reports, never whether the frame is
		// admitted.
		[[nodiscard]] constexpr WorldBlockReason ApplyModeSwitchBlock(
			WorldBlockReason a_afterVeto, bool a_modeSwitchPending) noexcept
		{
			return a_afterVeto == WorldBlockReason::kOperatorVeto ?
			           a_afterVeto :
			           a_modeSwitchPending ? WorldBlockReason::kModeSwitchPending : a_afterVeto;
		}

		static_assert(
			ApplyModeSwitchBlock(WorldBlockReason::kNone, true) ==
			WorldBlockReason::kModeSwitchPending);
		static_assert(
			ApplyModeSwitchBlock(WorldBlockReason::kMainMenu, true) ==
			WorldBlockReason::kModeSwitchPending);
		static_assert(
			ApplyModeSwitchBlock(WorldBlockReason::kNone, false) == WorldBlockReason::kNone);
		// The operator veto outranks a pending switch even when both fire the same frame.
		static_assert(
			ApplyModeSwitchBlock(WorldBlockReason::kOperatorVeto, true) ==
			WorldBlockReason::kOperatorVeto);

		// The last override in the chain, applied after the switch window's: HDRRoundTrip's
		// session latch is set, so nothing would reconstruct the reduced raster this frame
		// would produce. It outranks a pending switch, whose window is a frame or two while
		// a fault lasts a 90-frame cool-down or the rest of the session, and it yields to
		// the operator's veto for the same reason the switch window does - the operator is
		// standing in front of a deliberate comparison and /state must say so. Like the two
		// above it only decides the label: all three deny the frame identically.
		[[nodiscard]] constexpr WorldBlockReason ApplyReconstructionBlock(
			WorldBlockReason a_afterModeSwitch, bool a_reconstructionUnavailable) noexcept
		{
			return a_afterModeSwitch == WorldBlockReason::kOperatorVeto ?
			           a_afterModeSwitch :
			           a_reconstructionUnavailable ? WorldBlockReason::kReconstructionUnavailable :
			                                         a_afterModeSwitch;
		}

		static_assert(
			ApplyReconstructionBlock(WorldBlockReason::kNone, true) ==
			WorldBlockReason::kReconstructionUnavailable);
		static_assert(
			ApplyReconstructionBlock(WorldBlockReason::kMainMenu, true) ==
			WorldBlockReason::kReconstructionUnavailable);
		// A fault outranks the switch window it may well have arrived during.
		static_assert(
			ApplyReconstructionBlock(WorldBlockReason::kModeSwitchPending, true) ==
			WorldBlockReason::kReconstructionUnavailable);
		static_assert(
			ApplyReconstructionBlock(WorldBlockReason::kNone, false) == WorldBlockReason::kNone);
		static_assert(
			ApplyReconstructionBlock(WorldBlockReason::kOperatorVeto, true) ==
			WorldBlockReason::kOperatorVeto);

		[[nodiscard]] consteval bool StableFrameReducerContract()
		{
			AdmissionMachineState state{};
			for (std::uint32_t frame = 1; frame <= kQualityWorldStableFrames; ++frame) {
				const auto step = AdvanceAdmission(state, true, 7, 0x1234);
				state = step.state;
				if (step.admitted != (frame == kQualityWorldStableFrames)) {
					return false;
				}
			}
			const auto blocked = AdvanceAdmission(state, false, 8, 0);
			if (blocked.admitted || blocked.state.phase != WorldAdmissionPhase::kBlocked ||
				blocked.state.stableFrames != 0 || !blocked.requestReset) {
				return false;
			}
			const auto rearm = AdvanceAdmission(blocked.state, true, 8, 0x1234);
			return !rearm.admitted && rearm.state.phase == WorldAdmissionPhase::kStabilizing &&
			       rearm.state.stableFrames == 1 && !rearm.requestReset;
		}

		[[nodiscard]] consteval bool ContextChangeReducerContract()
		{
			AdmissionMachineState active{
				WorldAdmissionPhase::kActive, 3, 0x1000, kQualityWorldStableFrames, false
			};
			const auto generationChanged = AdvanceAdmission(active, true, 4, 0x1000);
			const auto cellChanged = AdvanceAdmission(active, true, 3, 0x2000);
			return !generationChanged.admitted && generationChanged.requestReset &&
			       generationChanged.state.phase == WorldAdmissionPhase::kStabilizing &&
			       generationChanged.state.stableFrames == 1 &&
			       !cellChanged.admitted && cellChanged.requestReset &&
			       cellChanged.state.phase == WorldAdmissionPhase::kStabilizing;
		}

		// The veto is applied to the observation (see ObserveWorld's ApplyOperatorVeto call),
		// not to this reducer: AdvanceAdmission only ever sees an eligible/ineligible bool, so
		// this contract constrains the blocked and re-entry behaviour of a vetoed frame -
		// that it is blocked exactly like any other ineligible one (one paired history reset,
		// zero stable frames) and that release re-enters through kStabilizing rather than
		// jumping back to kActive, all the way to the activating frame - and nothing about the
		// veto's wiring. The wiring (that a vetoed frame's reason really is overridden, and an
		// unvetoed one is not) is constrained by the ApplyOperatorVeto static_asserts above and
		// by the runner's ab-veto-* assertions.
		[[nodiscard]] consteval bool OperatorVetoReducerContract()
		{
			AdmissionMachineState active{
				WorldAdmissionPhase::kActive, 3, 0x1000, kQualityWorldStableFrames, false
			};
			const auto vetoed = AdvanceAdmission(active, false, 3, 0x1000);
			if (vetoed.admitted || vetoed.state.phase != WorldAdmissionPhase::kBlocked ||
				vetoed.state.stableFrames != 0 || !vetoed.requestReset) {
				return false;
			}
			const auto released = AdvanceAdmission(vetoed.state, true, 3, 0x1000);
			if (released.admitted ||
			    released.state.phase != WorldAdmissionPhase::kStabilizing ||
			    released.state.stableFrames != 1 || released.requestReset) {
				return false;
			}
			// Continue re-entry through the ordinary stabilizing count to the activating
			// frame - the invariant g_worldLastActivationStableFrames reports at runtime (see
			// AdvanceQualityWorldAdmissionBeforeWorld), proved here at compile time too.
			AdmissionMachineState state = released.state;
			AdmissionStep step{};
			for (std::uint32_t frame = 2; frame <= kQualityWorldStableFrames; ++frame) {
				step = AdvanceAdmission(state, true, 3, 0x1000);
				state = step.state;
			}
			return step.activated && step.state.stableFrames == kQualityWorldStableFrames;
		}

		static_assert(StableFrameReducerContract());
		static_assert(ContextChangeReducerContract());
		static_assert(OperatorVetoReducerContract());
		static_assert(BlockingMenuBit("LoadingMenu"sv) == kLoadingMenuBit);
		static_assert(BlockingMenuBit("HUDMenu"sv) == 0);

		AdmissionMachineState g_worldMachine{};  // render-thread only

		void SignalWorldTransition() noexcept
		{
			g_worldTransitionGeneration.fetch_add(1, std::memory_order_acq_rel);
			g_worldAdmissionInvalidations.fetch_add(1, std::memory_order_relaxed);
		}

		[[nodiscard]] WorldObservation ObserveWorldImpl()
		{
			WorldObservation observation{};
			const auto generationBefore = g_worldTransitionGeneration.load(std::memory_order_acquire);
			observation.transitionGeneration = generationBefore;

			auto* const ui = RE::UI::GetSingleton();
			if (!ui) {
				observation.reason = WorldBlockReason::kNoUI;
				return observation;
			}

			observation.menuMode = ui->menuMode;
			observation.itemMenuMode = ui->itemMenuMode.load_unchecked();
			observation.freezeFrameMenuBG = ui->freezeFrameMenuBG;
			observation.freezeFramePause = ui->freezeFramePause;
			observation.closingAllMenus = ui->closingAllMenus;
			{
				RE::BSAutoReadLock lock{ RE::UI::GetMenuMapRWLock() };
				for (const auto& entry : ui->menuMap) {
					const auto& menu = entry.second.menu;
					const char* const name = entry.first.c_str();
					if (menu && menu->OnStack() && name && *name) {
						observation.openBlockingMenuMask |= BlockingMenuBit(name);
					}
				}
			}

			observation.reason = BlockingMenuReason(observation.openBlockingMenuMask);
			if (observation.reason == WorldBlockReason::kNone && observation.freezeFrameMenuBG != 0) {
				observation.reason = WorldBlockReason::kFreezeFrameMenuBackground;
			} else if (observation.reason == WorldBlockReason::kNone && observation.freezeFramePause != 0) {
				observation.reason = WorldBlockReason::kFreezeFramePause;
			} else if (observation.reason == WorldBlockReason::kNone && observation.closingAllMenus) {
				observation.reason = WorldBlockReason::kClosingAllMenus;
			} else if (observation.reason == WorldBlockReason::kNone && observation.menuMode != 0) {
				observation.reason = WorldBlockReason::kMenuMode;
			} else if (observation.reason == WorldBlockReason::kNone && observation.itemMenuMode != 0) {
				observation.reason = WorldBlockReason::kItemMenuMode;
			}

			if (observation.reason == WorldBlockReason::kNone) {
				auto* const player = RE::PlayerCharacter::GetSingleton();
				if (!player) {
					observation.reason = WorldBlockReason::kNoPlayer;
				} else {
					auto* const cell = player->GetParentCell();
					if (!cell) {
						observation.reason = WorldBlockReason::kNoCell;
					} else {
						observation.cell = reinterpret_cast<std::uintptr_t>(cell);
					}
				}
			}

			const auto generationAfter = g_worldTransitionGeneration.load(std::memory_order_acquire);
			observation.transitionGeneration = generationAfter;
			if (generationAfter != generationBefore && observation.reason == WorldBlockReason::kNone) {
				observation.reason = WorldBlockReason::kTransitionChanged;
			}
			return observation;
		}

		// All three overrides are the frame's single cached loads (see g_cachedOperatorVeto
		// / g_cachedModeSwitchPending / g_cachedReconstructionUnavailable), never fresh
		// atomic reads here. Ordinary UI/cell observation still runs unconditionally above,
		// so /state keeps reporting truthful menu facts while blocked; only .reason is
		// overridden, and the veto overrides every other reason including a state-read
		// fault, with the session latch next and the pending switch last (see
		// ApplyReconstructionBlock and ApplyModeSwitchBlock for the ranking).
		[[nodiscard]] WorldObservation ObserveWorld(
			bool a_operatorVeto,
			bool a_modeSwitchPending,
			bool a_reconstructionUnavailable) noexcept
		{
			WorldObservation observation{};
			__try {
				observation = ObserveWorldImpl();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				observation = {};
				observation.reason = WorldBlockReason::kStateReadFault;
				observation.transitionGeneration =
					g_worldTransitionGeneration.load(std::memory_order_acquire);
			}
			observation.reason = ApplyReconstructionBlock(
				ApplyModeSwitchBlock(
					ApplyOperatorVeto(observation.reason, a_operatorVeto), a_modeSwitchPending),
				a_reconstructionUnavailable);
			return observation;
		}

		void PublishWorldObservation(const WorldObservation& a_observation) noexcept
		{
			g_worldMenuMode.store(a_observation.menuMode, std::memory_order_relaxed);
			g_worldItemMenuMode.store(a_observation.itemMenuMode, std::memory_order_relaxed);
			g_worldFreezeFrameMenuBG.store(a_observation.freezeFrameMenuBG, std::memory_order_relaxed);
			g_worldFreezeFramePause.store(a_observation.freezeFramePause, std::memory_order_relaxed);
			g_worldClosingAllMenus.store(a_observation.closingAllMenus, std::memory_order_relaxed);
			g_worldOpenBlockingMenuMask.store(a_observation.openBlockingMenuMask, std::memory_order_relaxed);
		}

		void PublishWorldMachine(
			const AdmissionMachineState& a_state,
			WorldBlockReason a_reason) noexcept
		{
			g_worldAdmissionPhase.store(static_cast<std::uint32_t>(a_state.phase), std::memory_order_release);
			g_worldBlockReason.store(static_cast<std::uint32_t>(a_reason), std::memory_order_release);
			g_worldAdmitted.store(a_state.phase == WorldAdmissionPhase::kActive, std::memory_order_release);
			g_worldStableFrames.store(a_state.stableFrames, std::memory_order_relaxed);
			g_worldCell.store(a_state.cell, std::memory_order_relaxed);
		}

		void RequestWorldAdmissionReset() noexcept
		{
			if (g_requestedGeneration.load(std::memory_order_acquire) !=
				g_acknowledgedGeneration.load(std::memory_order_acquire)) {
				return;
			}
			g_worldAdmissionResetRequests.fetch_add(1, std::memory_order_relaxed);
			try {
				RequestReset("quality world admission epoch"sv);
			} catch (...) {
				// RequestReset publishes its generation before its only fallible logging.
			}
		}

		// VERIFIED: PlayerCharacter::PlayerCharacter initializes the first
		// event-source lock at player + 0x4D0 and the
		// following sources begin at +0x528, +0x580 and +0x5D8 (0x58-byte stride).
		// CommonLibF4's PlayerCharacter static_cast places this base at +0x4C8;
		// using that static_cast treats a vtable pointer as BSSpinLock state and
		// spins forever. This plugin is VR-only, so use the measured VR base.
		constexpr std::ptrdiff_t kPlayerCellEventSourceOffset = 0x4D0;

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				const char* name = a_event.menuName.c_str();
				if (a_event.opening) {
					g_menuOpenEvents.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_menuCloseEvents.fetch_add(1, std::memory_order_relaxed);
				}
				if (name && BlockingMenuBit(name) != 0) {
					SignalWorldTransition();
				}
				if (g_resetOnMenu.load(std::memory_order_acquire)) {
					const std::string reason = fmt::format(
						FMT_STRING("menu {}: {}"),
						a_event.opening ? "opened"sv : "closed"sv,
						name && *name ? std::string_view{ name } : "unnamed"sv);
					RequestReset(reason);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		MenuSink g_menuSink;

		class CellSink : public RE::BSTEventSink<RE::BGSActorCellEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::BGSActorCellEvent& a_event,
				RE::BSTEventSource<RE::BGSActorCellEvent>*) override
			{
				g_lastCellId.store(a_event.cellID, std::memory_order_relaxed);
				SignalWorldTransition();
				if (a_event.flags == RE::BGSActorCellEvent::CellFlag::kEnter) {
					g_cellEnterEvents.fetch_add(1, std::memory_order_relaxed);
					if (g_resetOnCell.load(std::memory_order_acquire)) {
						RequestReset(fmt::format(FMT_STRING("player entered cell {:08X}"), a_event.cellID));
					}
				} else if (a_event.flags == RE::BGSActorCellEvent::CellFlag::kLeave) {
					g_cellLeaveEvents.fetch_add(1, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		CellSink g_cellSink;
	}

	void Configure(bool a_resetOnMenu, bool a_resetOnLoad, bool a_resetOnCell) noexcept
	{
		g_resetOnMenu.store(a_resetOnMenu, std::memory_order_release);
		g_resetOnLoad.store(a_resetOnLoad, std::memory_order_release);
		g_resetOnCell.store(a_resetOnCell, std::memory_order_release);
	}

	bool TextInputMenuOpen() noexcept
	{
		try {
			auto* const ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			// Interned once and never released: a BSFixedString built per call would
			// re-enter the engine's string table on every frame for a name that never
			// changes, and a function-local static with a destructor would drop an engine
			// string reference at DLL detach, under the loader lock, for no gain.
			static const RE::BSFixedString* const name =
				new RE::BSFixedString{ RE::Console::MENU_NAME };
			return ui->GetMenuOpen(*name);
		} catch (...) {
			// A UI read that throws is not a reason to stop polling keys; report "no text
			// input" and let the caller behave exactly as it does with the console closed.
			return false;
		}
	}

	bool InstallMenuEvents() noexcept
	{
		if (g_menuSinkInstalled.load(std::memory_order_acquire)) {
			return true;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuSink);
		g_menuSinkInstalled.store(true, std::memory_order_release);
		logger::info("history reset policy: MenuOpenCloseEvent sink installed"sv);
		return true;
	}

	bool InstallCellEvents() noexcept
	{
		if (g_cellSinkInstalled.load(std::memory_order_acquire)) {
			return true;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}
		auto* source = reinterpret_cast<RE::BSTEventSource<RE::BGSActorCellEvent>*>(
			reinterpret_cast<std::byte*>(player) + kPlayerCellEventSourceOffset);
		source->RegisterSink(&g_cellSink);
		g_cellSinkInstalled.store(true, std::memory_order_release);
		logger::info("history reset policy: player BGSActorCellEvent sink installed"sv);
		return true;
	}

	void OnGameLoaded()
	{
		g_loadEvents.fetch_add(1, std::memory_order_relaxed);
		SignalWorldTransition();
		if (g_resetOnLoad.load(std::memory_order_acquire)) {
			RequestReset("game loaded"sv);
		}
	}

	void RequestReset(std::string_view a_reason)
	{
		g_requests.fetch_add(1, std::memory_order_relaxed);
		// Every paired history reset funnels through here, so this is the one place the
		// armed jitter sequence restarts. It restarts on the NEXT commit, not this frame:
		// this frame's sample was written before anything decided to reset, and no vendor
		// requires phase 0 at a reset. Both eyes share one sample and one reset, so there
		// is no stereo divergence to arrange.
		Jitter::RequestSequenceRestart();
		const auto acknowledged = g_acknowledgedGeneration.load(std::memory_order_acquire);
		const auto previous = g_requestedGeneration.fetch_add(1, std::memory_order_acq_rel);
		const bool alreadyPending = previous != acknowledged;
		if (alreadyPending) {
			g_coalesced.fetch_add(1, std::memory_order_relaxed);
		}

		// Logged on a best-effort rising edge only. Generation accounting, rather than
		// this diagnostic edge, is authoritative if an acknowledgement races the request.
		if (!alreadyPending) {
			try {
				logger::info(FMT_STRING("history reset requested: {}"), a_reason);
			} catch (...) {
				// The request was published before formatting/logging. Never let diagnostics
				// prevent the render thread from resetting both histories.
			}
		}
	}

	void RequestWorldReset() noexcept
	{
		try {
			RequestReset("pre-world state repair"sv);
		} catch (...) {
			// RequestReset publishes before its only fallible diagnostic operation. This is
			// an extra fail-closed boundary for a render-thread hook.
		}
	}

	void RequestQualityAbortReset() noexcept
	{
		g_qualityAbortRequests.fetch_add(1, std::memory_order_relaxed);
		try {
			RequestReset("aborted reduced-NGX quality transaction"sv);
		} catch (...) {
			// The generation is published before RequestReset's only fallible diagnostics.
		}
	}

	Ticket AcquireReset() noexcept
	{
		const auto requested = g_requestedGeneration.load(std::memory_order_acquire);
		const auto acknowledged = g_acknowledgedGeneration.load(std::memory_order_acquire);
		if (requested == acknowledged) {
			return {};
		}
		g_acquisitions.fetch_add(1, std::memory_order_relaxed);
		return { requested, true };
	}

	void AcknowledgeReset(Ticket a_ticket) noexcept
	{
		if (!a_ticket.pending) {
			return;
		}

		auto acknowledged = g_acknowledgedGeneration.load(std::memory_order_acquire);
		while (acknowledged < a_ticket.generation &&
		       !g_acknowledgedGeneration.compare_exchange_weak(
			       acknowledged,
			       a_ticket.generation,
			       std::memory_order_acq_rel,
			       std::memory_order_acquire)) {
		}
		if (acknowledged < a_ticket.generation) {
			// Retain the historical field name in State/JSON for compatibility. A
			// "consumption" now means a successfully acknowledged stereo pair.
			g_consumptions.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void RecordSubmission(std::uint32_t a_eye, bool a_reset) noexcept
	{
		if (!a_reset) {
			return;
		}
		if (a_eye == 0) {
			g_leftSubmissions.fetch_add(1, std::memory_order_relaxed);
		} else if (a_eye == 1) {
			g_rightSubmissions.fetch_add(1, std::memory_order_relaxed);
		}
	}

	WorldAdmissionTicket AdvanceQualityWorldAdmissionBeforeWorld(
		bool a_reconstructionUnavailable) noexcept
	{
		g_worldAdmissionChecks.fetch_add(1, std::memory_order_relaxed);
		// One load per frame each. Cached below for RevalidateQualityWorldAdmission and any
		// other same-frame recheck, so a listener-thread SetOperatorVeto/
		// EngageModeSwitchPending write cannot land between two loads and split this
		// frame's decision. Render thread owns all three caches; the third arrives already
		// loaded from the pre-world hook for the same reason.
		const bool operatorVeto = g_operatorVeto.load(std::memory_order_acquire);
		g_cachedOperatorVeto = operatorVeto;
		const bool modeSwitchPending = g_modeSwitchPendingCount.load(std::memory_order_acquire) > 0;
		g_cachedModeSwitchPending = modeSwitchPending;
		g_cachedReconstructionUnavailable = a_reconstructionUnavailable;
		g_reconstructionUnavailable.store(a_reconstructionUnavailable, std::memory_order_release);
		const WorldObservation observation =
			ObserveWorld(operatorVeto, modeSwitchPending, a_reconstructionUnavailable);
		PublishWorldObservation(observation);

		const AdmissionMachineState previous = g_worldMachine;
		const auto previousReason = static_cast<WorldBlockReason>(
			g_worldBlockReason.load(std::memory_order_acquire));
		const bool eligible = observation.reason == WorldBlockReason::kNone;
		const AdmissionStep step = AdvanceAdmission(
			previous,
			eligible,
			observation.transitionGeneration,
			observation.cell);
		g_worldMachine = step.state;

		const WorldBlockReason publishedReason = !eligible ? observation.reason :
		                                               step.admitted ? WorldBlockReason::kNone :
		                                                               WorldBlockReason::kStabilizing;
		PublishWorldMachine(step.state, publishedReason);
		if (step.requestReset) {
			RequestWorldAdmissionReset();
		}
		if (step.activated) {
			g_worldAdmissionActivations.fetch_add(1, std::memory_order_relaxed);
			g_worldLastActivationStableFrames.store(
				step.state.stableFrames, std::memory_order_relaxed);
		}
		if (!step.admitted) {
			g_worldAdmissionDeniedFrames.fetch_add(1, std::memory_order_relaxed);
			if (publishedReason == WorldBlockReason::kOperatorVeto) {
				g_operatorVetoDeniedFrames.fetch_add(1, std::memory_order_relaxed);
			}
			if (publishedReason == WorldBlockReason::kModeSwitchPending) {
				g_modeSwitchDeniedFrames.fetch_add(1, std::memory_order_relaxed);
			}
			if (publishedReason == WorldBlockReason::kReconstructionUnavailable) {
				g_reconstructionUnavailableDeniedFrames.fetch_add(1, std::memory_order_relaxed);
			}
		}

		if (step.state.phase != previous.phase || publishedReason != previousReason) {
			logger::info(
				FMT_STRING("quality world admission: phase {}, reason {}, stable {}/{}, generation {}, cell {:X}"),
				WorldAdmissionPhaseName(step.state.phase),
				WorldBlockReasonName(publishedReason),
				step.state.stableFrames,
				kQualityWorldStableFrames,
				step.state.transitionGeneration,
				step.state.cell);
		}

		return {
			step.state.transitionGeneration,
			step.state.cell,
			step.admitted,
		};
	}

	static bool RevalidateQualityWorldAdmissionImpl(WorldAdmissionTicket a_ticket) noexcept
	{
		g_worldAdmissionRechecks.fetch_add(1, std::memory_order_relaxed);
		// Reuses the frame's cached loads rather than fresh atomic reads - see
		// g_cachedOperatorVeto / g_cachedModeSwitchPending.
		const WorldObservation observation =
			ObserveWorld(g_cachedOperatorVeto, g_cachedModeSwitchPending,
				g_cachedReconstructionUnavailable);
		PublishWorldObservation(observation);
		const bool valid = a_ticket.admitted &&
		                   g_worldMachine.phase == WorldAdmissionPhase::kActive &&
		                   observation.reason == WorldBlockReason::kNone &&
		                   observation.transitionGeneration == a_ticket.transitionGeneration &&
		                   observation.cell == a_ticket.cell &&
		                   g_worldMachine.transitionGeneration == a_ticket.transitionGeneration &&
		                   g_worldMachine.cell == a_ticket.cell;
		if (valid) {
			return true;
		}

		g_worldAdmissionRecheckFailures.fetch_add(1, std::memory_order_relaxed);
		const AdmissionMachineState previous = g_worldMachine;
		const bool eligible = observation.reason == WorldBlockReason::kNone;
		AdmissionStep step = AdvanceAdmission(
			previous,
			eligible,
			observation.transitionGeneration,
			observation.cell);
		if (step.admitted) {
			// A malformed/stale ticket must never be converted into permission merely because
			// the current observation remained eligible.
			step = AdvanceAdmission(
				previous,
				false,
				observation.transitionGeneration,
				observation.cell);
		}
		g_worldMachine = step.state;
		const WorldBlockReason publishedReason = observation.reason != WorldBlockReason::kNone ?
		                                                   observation.reason :
		                                                   WorldBlockReason::kTransitionChanged;
		PublishWorldMachine(step.state, publishedReason);
		logger::warn(
			FMT_STRING("quality world admission changed during staged Quality work: reason {}, ticket generation/cell {}/{:X}, current {}/{:X}; result remains private"),
			WorldBlockReasonName(publishedReason),
			a_ticket.transitionGeneration,
			a_ticket.cell,
			observation.transitionGeneration,
			observation.cell);
		return false;
	}

	bool RevalidateQualityWorldAdmission(WorldAdmissionTicket a_ticket) noexcept
	{
		return RevalidateQualityWorldAdmissionImpl(a_ticket);
	}

	bool RevalidateQualityWorldAdmissionForPublish(WorldAdmissionTicket a_ticket) noexcept
	{
		g_worldAdmissionPublishRechecks.fetch_add(1, std::memory_order_relaxed);
		const bool valid = a_ticket.admitted &&
		                   g_worldMachine.phase == WorldAdmissionPhase::kActive &&
		                   g_worldMachine.transitionGeneration == a_ticket.transitionGeneration &&
		                   g_worldMachine.cell == a_ticket.cell &&
		                   g_worldTransitionGeneration.load(std::memory_order_acquire) ==
			                   a_ticket.transitionGeneration;
		if (!valid) {
			g_worldAdmissionPublishVetoes.fetch_add(1, std::memory_order_relaxed);
		}
		return valid;
	}

	std::string_view WorldAdmissionPhaseName(WorldAdmissionPhase a_phase) noexcept
	{
		switch (a_phase) {
		case WorldAdmissionPhase::kBlocked:
			return "blocked"sv;
		case WorldAdmissionPhase::kStabilizing:
			return "stabilizing"sv;
		case WorldAdmissionPhase::kActive:
			return "active"sv;
		default:
			return "unknown"sv;
		}
	}

	std::string_view WorldBlockReasonName(WorldBlockReason a_reason) noexcept
	{
		switch (a_reason) {
		case WorldBlockReason::kNone:
			return "none"sv;
		case WorldBlockReason::kNoUI:
			return "no-ui"sv;
		case WorldBlockReason::kMainMenu:
			return "main-menu"sv;
		case WorldBlockReason::kLoadingMenu:
			return "loading-menu"sv;
		case WorldBlockReason::kFaderMenu:
			return "fader-menu"sv;
		case WorldBlockReason::kPauseMenu:
			return "pause-menu"sv;
		case WorldBlockReason::kPipboyMenu:
			return "pipboy-menu"sv;
		case WorldBlockReason::kMessageBoxMenu:
			return "message-box-menu"sv;
		case WorldBlockReason::kFreezeFrameMenuBackground:
			return "freeze-frame-menu-background"sv;
		case WorldBlockReason::kFreezeFramePause:
			return "freeze-frame-pause"sv;
		case WorldBlockReason::kClosingAllMenus:
			return "closing-all-menus"sv;
		case WorldBlockReason::kMenuMode:
			return "menu-mode"sv;
		case WorldBlockReason::kItemMenuMode:
			return "item-menu-mode"sv;
		case WorldBlockReason::kNoPlayer:
			return "no-player"sv;
		case WorldBlockReason::kNoCell:
			return "no-cell"sv;
		case WorldBlockReason::kTransitionChanged:
			return "transition-changed"sv;
		case WorldBlockReason::kStateReadFault:
			return "state-read-fault"sv;
		case WorldBlockReason::kStabilizing:
			return "stabilizing"sv;
		case WorldBlockReason::kOperatorVeto:
			return "operator-veto"sv;
		case WorldBlockReason::kModeSwitchPending:
			return "mode-switch-pending"sv;
		case WorldBlockReason::kReconstructionUnavailable:
			return "reconstruction-unavailable"sv;
		default:
			return "unknown"sv;
		}
	}

	void SetOperatorVeto(bool a_active) noexcept
	{
		const bool previous = g_operatorVeto.exchange(a_active, std::memory_order_acq_rel);
		if (previous == a_active) {
			return;
		}
		if (a_active) {
			g_operatorVetoEngagements.fetch_add(1, std::memory_order_relaxed);
			logger::info("operator veto engaged: world gate denies every frame, engine renders native"sv);
		} else {
			g_operatorVetoReleases.fetch_add(1, std::memory_order_relaxed);
			logger::info("operator veto released: world gate re-admits after stabilizing"sv);
		}
	}

	bool OperatorVetoActive() noexcept
	{
		return g_operatorVeto.load(std::memory_order_acquire);
	}

	void EngageModeSwitchPending() noexcept
	{
		// 0-to-1 only: a caller that finds the window already held by someone else still
		// holds its own count, but the world gate was already denying every frame before
		// this call, so neither the log line nor the engagement counter fires again for it.
		const auto previous = g_modeSwitchPendingCount.fetch_add(1, std::memory_order_acq_rel);
		if (previous == 0) {
			g_modeSwitchEngagements.fetch_add(1, std::memory_order_relaxed);
			logger::info("mode switch pending: world gate denies every frame while the new configuration publishes"sv);
		}
	}

	void ReleaseModeSwitchPending() noexcept
	{
		// Compare-exchange loop rather than a plain fetch_sub: a release must never drive
		// the count negative, whether from a caller releasing a window it never engaged or
		// one releasing twice by mistake. Found at zero, it is a bug in the caller - logged
		// once and dropped rather than acted on, so a stray extra release can never force
		// an early release of a window another engagement is still relying on (which an
		// unsigned wraparound to a huge count, or a signed one going negative and never
		// recovering, would both risk).
		auto current = g_modeSwitchPendingCount.load(std::memory_order_acquire);
		for (;;) {
			if (current == 0) {
				logger::error("mode switch pending: release with no matching engagement - ignored"sv);
				return;
			}
			if (g_modeSwitchPendingCount.compare_exchange_weak(
					current, current - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
				break;
			}
		}
		if (current == 1) {
			g_modeSwitchReleases.fetch_add(1, std::memory_order_relaxed);
			logger::info("mode switch complete: world gate re-admits after stabilizing"sv);
		}
	}

	State Snapshot() noexcept
	{
		const auto requestedGeneration = g_requestedGeneration.load(std::memory_order_acquire);
		const auto acknowledgedGeneration = g_acknowledgedGeneration.load(std::memory_order_acquire);
		return {
			requestedGeneration != acknowledgedGeneration,
			g_menuSinkInstalled.load(std::memory_order_acquire),
			g_cellSinkInstalled.load(std::memory_order_acquire),
			g_resetOnMenu.load(std::memory_order_acquire),
			g_resetOnLoad.load(std::memory_order_acquire),
			g_resetOnCell.load(std::memory_order_acquire),
			g_requests.load(std::memory_order_relaxed),
			g_coalesced.load(std::memory_order_relaxed),
			g_consumptions.load(std::memory_order_relaxed),
			g_loadEvents.load(std::memory_order_relaxed),
			g_menuOpenEvents.load(std::memory_order_relaxed),
			g_menuCloseEvents.load(std::memory_order_relaxed),
			g_cellEnterEvents.load(std::memory_order_relaxed),
			g_cellLeaveEvents.load(std::memory_order_relaxed),
			g_leftSubmissions.load(std::memory_order_relaxed),
			g_rightSubmissions.load(std::memory_order_relaxed),
			g_lastCellId.load(std::memory_order_relaxed),
			requestedGeneration,
			acknowledgedGeneration,
			g_acquisitions.load(std::memory_order_relaxed),
			g_qualityAbortRequests.load(std::memory_order_relaxed),
			static_cast<WorldAdmissionPhase>(g_worldAdmissionPhase.load(std::memory_order_acquire)),
			static_cast<WorldBlockReason>(g_worldBlockReason.load(std::memory_order_acquire)),
			g_worldAdmitted.load(std::memory_order_acquire),
			g_worldStableFrames.load(std::memory_order_relaxed),
			kQualityWorldStableFrames,
			g_worldCell.load(std::memory_order_relaxed),
			g_worldTransitionGeneration.load(std::memory_order_acquire),
			g_worldAdmissionChecks.load(std::memory_order_relaxed),
			g_worldAdmissionDeniedFrames.load(std::memory_order_relaxed),
			g_worldAdmissionActivations.load(std::memory_order_relaxed),
			g_worldAdmissionInvalidations.load(std::memory_order_relaxed),
			g_worldAdmissionResetRequests.load(std::memory_order_relaxed),
			g_worldAdmissionRechecks.load(std::memory_order_relaxed),
			g_worldAdmissionRecheckFailures.load(std::memory_order_relaxed),
			g_worldAdmissionPublishRechecks.load(std::memory_order_relaxed),
			g_worldAdmissionPublishVetoes.load(std::memory_order_relaxed),
			g_worldMenuMode.load(std::memory_order_relaxed),
			g_worldItemMenuMode.load(std::memory_order_relaxed),
			g_worldFreezeFrameMenuBG.load(std::memory_order_relaxed),
			g_worldFreezeFramePause.load(std::memory_order_relaxed),
			g_worldClosingAllMenus.load(std::memory_order_relaxed),
			g_worldOpenBlockingMenuMask.load(std::memory_order_relaxed),
			g_operatorVeto.load(std::memory_order_acquire),
			g_operatorVetoEngagements.load(std::memory_order_relaxed),
			g_operatorVetoReleases.load(std::memory_order_relaxed),
			g_operatorVetoDeniedFrames.load(std::memory_order_relaxed),
			g_worldLastActivationStableFrames.load(std::memory_order_relaxed),
			g_modeSwitchPendingCount.load(std::memory_order_acquire) > 0,
			g_modeSwitchEngagements.load(std::memory_order_relaxed),
			g_modeSwitchReleases.load(std::memory_order_relaxed),
			g_modeSwitchDeniedFrames.load(std::memory_order_relaxed),
			// The input the last world-gate decision used, not a fresh read of
			// HDRRoundTrip's latch.
			g_reconstructionUnavailable.load(std::memory_order_acquire),
			g_reconstructionUnavailableDeniedFrames.load(std::memory_order_relaxed),
		};
	}
}
