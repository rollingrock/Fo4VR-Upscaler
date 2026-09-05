#include "Settings/Configuration.h"

#include "Upscaler/DynamicResolution.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/Hotkeys.h"
#include "Upscaler/HudMessages.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/MipBias.h"
#include "Upscaler/Panel.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/TAAReplacement.h"
#include "Upscaler/Telemetry.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Configuration
{
	namespace
	{
		// The live-switch bookkeeping this file owns: what mode is actually running, the
		// non-off mode (if any) this session's first successful apply configured - not
		// necessarily one that armed a bracket, see SettingsPolicy.h::ResolveModeSwitch -
		// and the outcome/count of the last /mode request that reached the admissibility
		// decision. Whether the bracket itself is armed is never tracked here: every
		// caller reads Upscaler::DynamicResolution::WorldAdmissionGated() live, at
		// decision time, so this file cannot disagree with what the engine actually
		// froze. atomic<shared_ptr<const string>> for the strings, matching
		// HDRRoundTrip's g_quality: readable from any thread without a lock, and a store
		// can never hand a reader a half-written string. Only ApplyLockedImpl (under
		// Settings::configurationLock) writes them.
		std::atomic<std::shared_ptr<const std::string>> g_liveMode;
		// The last non-off mode g_liveMode held, which is not the same fact as the mode the
		// session armed: a live switch can move between reduced modes, so the mode a player
		// was running before switching off is the one a toggle back on should return to.
		// Written in the same place and under the same lock as g_liveMode, one store later,
		// so a reader that sees a non-off live mode has already seen this one move with it.
		std::atomic<std::shared_ptr<const std::string>> g_lastNonOffLiveMode;
		std::atomic<std::shared_ptr<const std::string>> g_sessionArmedMode;
		std::atomic_bool g_sessionArmedModeCaptured{ false };
		std::atomic<std::uint32_t> g_lastSwitchOutcome{
			static_cast<std::uint32_t>(Settings::Policy::SwitchOutcome::kUnchanged)
		};
		std::atomic_bool g_hasSwitchOutcome{ false };
		std::atomic<std::uint64_t> g_switchAttempts{ 0 };
		// The Settings::RuntimeModeOverrideEpoch this file last recorded an outcome for.
		// 0 means never - RuntimeModeOverrideEpoch is 1 on a session's first
		// SetRuntimeModeOverride call, so this never collides with "never processed".
		std::atomic<std::uint64_t> g_processedOverrideEpoch{ 0 };

		[[nodiscard]] std::string LiveModeOr(std::string_view a_fallback)
		{
			const auto mode = g_liveMode.load(std::memory_order_acquire);
			return mode ? *mode : std::string(a_fallback);
		}

		[[nodiscard]] std::string SessionArmedModeString()
		{
			const auto mode = g_sessionArmedMode.load(std::memory_order_acquire);
			return mode ? *mode : std::string{};
		}

		[[nodiscard]] std::string LastNonOffLiveModeString()
		{
			const auto mode = g_lastNonOffLiveMode.load(std::memory_order_acquire);
			return mode ? *mode : std::string{};
		}

		// *Settings::mode is a std::string, and the settings layer serializes writes to it
		// with loadLock (Settings.h). ApplyLockedImpl's own corrections below run holding
		// configurationLock, which is the outer lock of the fixed configurationLock ->
		// loadLock order, so taking loadLock around the assignment here is both correct and
		// the only way this write is serialized against a concurrent Settings::load().
		// POD-only and captureless so it converts to Settings::LockedOperation.
		struct ModeAssignment
		{
			const std::string* value = nullptr;
		};

		[[nodiscard]] bool AssignModeImpl(void* a_context) noexcept
		{
			auto* const assignment = static_cast<ModeAssignment*>(a_context);
			if (!assignment || !assignment->value) {
				return false;
			}
			try {
				*Settings::mode = *assignment->value;
				return true;
			} catch (...) {
				return false;
			}
		}

		void AssignModeUnderLoadLock(const std::string& a_mode) noexcept
		{
			ModeAssignment assignment{ std::addressof(a_mode) };
			if (!Settings::WithLoadLock(AssignModeImpl, std::addressof(assignment))) {
				logger::error(FMT_STRING("mode correction to '{}' could not be written back into the settings; /config will report the mode that was refused until the next reload"),
					a_mode);
			}
		}

		struct RuntimeSnapshot
		{
			bool enabled = false;
			bool diagnosticDynamicResolution = false;
			double renderScaleOverride = 0.0;
			bool diagnosticTaaReplacement = false;
			bool diagnosticTaaBypass = false;
			bool diagnosticHdrRoundTrip = false;
			bool diagnosticStereoNull = false;
			bool diagnosticNgxCreate = false;
			bool diagnosticNgxEvaluate = false;
			bool diagnosticNgxReassemble = false;
			bool diagnosticNgxHandback = false;
			bool diagnosticNgxContinuous = false;
			bool diagnosticFfxEvaluate = false;
			bool diagnosticFfxReassemble = false;
			bool diagnosticFfxHandback = false;
			bool diagnosticFfxContinuous = false;
			std::uint64_t diagnosticContinuousFrames = 0;
			bool resetOnMenu = false;
			bool resetOnLoad = false;
			bool resetOnCell = false;
			bool suppressDuringScopeRender = false;
			bool forceHiddenAreaMeshOff = true;
			bool unbindDepthStencil = true;
			bool perfTimers = false;
			double sharpness = 0.0;
			double mipBiasOverride = 999.0;
			bool mipBiasIncludeTrilinear = false;
			bool mipBiasLowerMinLod = false;
			std::string runtimeDir;
			std::string preset;
			std::string quality;
			std::string stereoMode;
			bool failSoft = false;
			std::string mode;
			std::string backend;
			std::int64_t hotkeyToggleVk = 0;
			std::int64_t hotkeyCompareVk = 0;
			std::int64_t hotkeyPanelVk = 0;
			bool panelEnabled = false;
			double panelWidthMeters = 0.0;
			double panelDistanceMeters = 0.0;
			std::int64_t jitterVendorSign = 1;
		};

		struct ApplyOperation
		{
			const Observer* observer = nullptr;
			RuntimeSnapshot snapshot;
			ApplyResult result{};
			bool locked = false;
			bool returned = false;
			bool callbackResult = false;
			// Set by Apply() itself (see its own comment), before RunApplyShell ever takes
			// configurationLock, the moment its prospective, pre-lock ratio check decides
			// this request is likely to change the live ratio - never inside
			// ApplyLockedImpl, which only calls RequestRatio, never engages or waits for
			// the window (see the ratio-change block there). Refcounted on the ResetPolicy
			// side (EngageModeSwitchPending/ReleaseModeSwitchPending), so this engagement
			// stacks safely with a live /mode switch's own (RequestModeSwitch, which may
			// have already engaged the same window before calling this Apply()) - each
			// caller releases only what it itself engaged. POD, checked/cleared from
			// RunApplyShell's __finally so a release can never be skipped on any exit path
			// (a thrown exception, an SEH fault, or an ordinary early return all reach it) -
			// the same reason every other POD flag in this shell lives here rather than as
			// a destructible local.
			bool ratioDenyWindowEngaged = false;
		};

		[[nodiscard]] bool CaptureRuntimeSnapshot(void* a_context) noexcept
		{
			auto* const snapshot = static_cast<RuntimeSnapshot*>(a_context);
			if (!snapshot) {
				return false;
			}
			try {
				snapshot->enabled = *Settings::enabled;
				snapshot->diagnosticDynamicResolution = *Settings::diagnosticDynamicResolution;
				snapshot->renderScaleOverride = *Settings::renderScaleOverride;
				snapshot->diagnosticTaaReplacement = *Settings::diagnosticTaaReplacement;
				snapshot->diagnosticTaaBypass = *Settings::diagnosticTaaBypass;
				snapshot->diagnosticHdrRoundTrip = *Settings::diagnosticHdrRoundTrip;
				snapshot->diagnosticStereoNull = *Settings::diagnosticStereoNull;
				snapshot->diagnosticNgxCreate = *Settings::diagnosticNgxCreate;
				snapshot->diagnosticNgxEvaluate = *Settings::diagnosticNgxEvaluate;
				snapshot->diagnosticNgxReassemble = *Settings::diagnosticNgxReassemble;
				snapshot->diagnosticNgxHandback = *Settings::diagnosticNgxHandback;
				snapshot->diagnosticNgxContinuous = *Settings::diagnosticNgxContinuous;
				snapshot->diagnosticFfxEvaluate = *Settings::diagnosticFfxEvaluate;
				snapshot->diagnosticFfxReassemble = *Settings::diagnosticFfxReassemble;
				snapshot->diagnosticFfxHandback = *Settings::diagnosticFfxHandback;
				snapshot->diagnosticFfxContinuous = *Settings::diagnosticFfxContinuous;
				snapshot->diagnosticContinuousFrames =
					static_cast<std::uint64_t>(*Settings::diagnosticContinuousFrames);
				snapshot->resetOnMenu = *Settings::resetOnMenu;
				snapshot->resetOnLoad = *Settings::resetOnLoad;
				snapshot->resetOnCell = *Settings::resetOnCell;
				snapshot->suppressDuringScopeRender = *Settings::suppressDuringScopeRender;
				snapshot->forceHiddenAreaMeshOff = *Settings::forceHiddenAreaMeshOff;
				snapshot->unbindDepthStencil = *Settings::unbindDepthStencil;
				snapshot->perfTimers = *Settings::perfTimers;
				snapshot->sharpness = *Settings::sharpness;
				snapshot->mipBiasOverride = *Settings::mipBiasOverride;
				snapshot->mipBiasIncludeTrilinear = *Settings::mipBiasIncludeTrilinear;
				snapshot->mipBiasLowerMinLod = *Settings::mipBiasLowerMinLod;
				snapshot->runtimeDir = *Settings::runtimeDir;
				snapshot->preset = *Settings::preset;
				snapshot->quality = *Settings::quality;
				snapshot->stereoMode = *Settings::stereoMode;
				snapshot->failSoft = *Settings::failSoft;
				snapshot->mode = *Settings::mode;
				snapshot->backend = *Settings::backend;
				snapshot->hotkeyToggleVk = *Settings::hotkeyToggleVk;
				snapshot->hotkeyCompareVk = *Settings::hotkeyCompareVk;
				snapshot->hotkeyPanelVk = *Settings::hotkeyPanelVk;
				snapshot->panelEnabled = *Settings::panelEnabled;
				snapshot->panelWidthMeters = *Settings::panelWidthMeters;
				snapshot->panelDistanceMeters = *Settings::panelDistanceMeters;
				snapshot->jitterVendorSign = *Settings::jitterVendorSign;
				return true;
			} catch (...) {
				return false;
			}
		}

		void AbortRuntimeConfiguration() noexcept
		{
			// These APIs are atomics/guarded writes only.  In particular they do not
			// acquire a component mutex that the interrupted call may have stranded.
			// Performance instrumentation is development-only and therefore fails closed:
			// a rejected reload must never leave a previously enabled timer window running.
			Upscaler::Telemetry::Configure(false);
			// A backend-owned jitter sequence outliving the configuration that armed it
			// would keep writing samples for an extent nothing is preparing any more.
			Upscaler::Jitter::DisarmBackendSequence();
			// Same reasoning for the sampler table: a bias derived for a configuration
			// nothing is preparing any more must not stay in the engine's own table. The
			// restore releases nothing, so it is safe from a rejected reload.
			Upscaler::MipBias::Restore();
			Upscaler::TAAReplacement::AbortConfiguration();
			Upscaler::HDRRoundTrip::AbortConfiguration();
			Upscaler::DynamicResolution::AbortConfiguration();
		}

		struct ReducedRatioDerivation
		{
			bool dynamicResolutionInput = false;
			double renderScale = 0.0;
			double vendorScalingRatio = 0.0;
			bool reducing = false;
			bool tookModeDefault = false;
		};

		// 0.0 means "use the documented per-quality-mode default ratio" rather than
		// "hand-typed one". DLAA is deliberately excluded from the lookup: its ratio is
		// 1.0, which is not a reduced mode, and DynamicResolution's own 0.30..1.00
		// validation still owns every other decision about the value. A DLAA session
		// leaves renderScale at 0.0: the lookup above declines and nothing replaces the
		// zero, so `reducing` stays false rather than mistaking "no reduction configured"
		// for one. Pure and side-effect-free on purpose - both the prospective, pre-lock
		// read in Apply() and the authoritative one inside ApplyLockedImpl call this same
		// function, so the two can never derive the ratio two different ways; only
		// ApplyLockedImpl's caller logs what it decided.
		[[nodiscard]] ReducedRatioDerivation DeriveReducedRatio(
			double a_renderScaleOverride, bool a_dynamicResolutionInput, std::string_view a_qualityInput) noexcept
		{
			ReducedRatioDerivation out{};
			out.dynamicResolutionInput = a_dynamicResolutionInput;
			out.renderScale = a_renderScaleOverride;
			// Non-zero only on the default path. A typed override is written verbatim,
			// because a user who names a ratio is not asking for the vendor's.
			if (out.renderScale == 0.0 && a_dynamicResolutionInput) {
				double defaultScale = 0.0;
				if (Upscaler::HDRRoundTrip::DefaultRenderScaleForQuality(a_qualityInput, defaultScale) &&
					defaultScale < 1.0) {
					out.renderScale = defaultScale;
					out.tookModeDefault = true;
					double ratio = 0.0;
					if (Upscaler::HDRRoundTrip::VendorScalingRatioForQuality(a_qualityInput, ratio) &&
						ratio > 1.0) {
						out.vendorScalingRatio = ratio;
					}
				}
			}
			out.reducing = out.renderScale > 0.0 && out.renderScale < 1.0;
			return out;
		}

		// The best-effort mirror of ApplyLockedImpl's own productMode/qualityInput/
		// dynamicResolutionInput derivation (see its mode block), simplified to what a
		// caller outside configurationLock can know: a_mode's own runtime-override/
		// refusal resolution stays inside the lock (it needs the live mode and the
		// authoritative session-armed-mode read, both of which can move between here and
		// there), so this takes a_mode - here, the just-(re)loaded snapshot's own mode
		// field, override already substituted by Validate() - at face value. A wrong
		// guess here (the authoritative recompute later disagrees) is harmless: see
		// Apply()'s own comment on why.
		[[nodiscard]] ReducedRatioDerivation DeriveReducedRatioFromMode(
			const RuntimeSnapshot& a_snapshot, std::string_view a_mode) noexcept
		{
			const bool productMode = a_mode != "off"sv;
			const std::string_view qualityInput = productMode ? a_mode : std::string_view(a_snapshot.quality);
			const bool dynamicResolutionInput =
				productMode ? a_mode != "dlaa"sv : a_snapshot.diagnosticDynamicResolution;
			return DeriveReducedRatio(a_snapshot.renderScaleOverride, dynamicResolutionInput, qualityInput);
		}

		// Whether this configuration has a live reduced-world ratio at all - a bracket that
		// is armed, on a session DynamicResolution actually reached kArmed/kApplied on,
		// deriving a reducing ratio. Only such a session can honour a repatch, matching
		// RequestRatio's own guard exactly (see DynamicResolution.cpp), so a refused or
		// not-yet-initialized one never reports a change it was always going to lose.
		// Split out from ReducedRatioWouldChange below because "nothing to change" and
		// "already at that value" are different answers to a caller persisting the key:
		// one of them means the value it wrote is live, and the other means it is not.
		[[nodiscard]] bool ReducedRatioIsLive(const ReducedRatioDerivation& a_derivation) noexcept
		{
			if (!a_derivation.dynamicResolutionInput || !a_derivation.reducing ||
				!Upscaler::DynamicResolution::WorldAdmissionGated()) {
				return false;
			}
			const auto currentState = Upscaler::DynamicResolution::Snapshot();
			return currentState.status == Upscaler::DynamicResolution::Status::kArmed ||
			       currentState.status == Upscaler::DynamicResolution::Status::kApplied;
		}

		// Whether a live ratio repatch through Upscaler::DynamicResolution::RequestRatio
		// would actually change anything, given a_derivation's own view of what the ratio
		// should be. Shared by the prospective, pre-lock read in Apply() and the
		// authoritative one inside ApplyLockedImpl so "changed" means the same bit
		// comparison in both places.
		[[nodiscard]] bool ReducedRatioWouldChange(const ReducedRatioDerivation& a_derivation) noexcept
		{
			if (!ReducedRatioIsLive(a_derivation)) {
				return false;
			}
			const auto currentState = Upscaler::DynamicResolution::Snapshot();
			return std::bit_cast<std::uint32_t>(static_cast<float>(a_derivation.renderScale)) !=
			           std::bit_cast<std::uint32_t>(currentState.requestedScale) ||
			       std::bit_cast<std::uint32_t>(static_cast<float>(a_derivation.vendorScalingRatio)) !=
			           std::bit_cast<std::uint32_t>(currentState.vendorScalingRatio);
		}

		[[nodiscard]] bool ApplyLockedImpl(ApplyOperation& a_operation)
		{
			if (a_operation.observer && a_operation.observer->beforeLoad) {
				a_operation.result.beforeObserved = Settings::WithLoadLock(
					a_operation.observer->beforeLoad,
					a_operation.observer->context,
					&a_operation.result.exceptionCode);
				if (!a_operation.result.beforeObserved) {
					return false;
				}
			}

			a_operation.result.settingsLoaded = Settings::load();
			if (!a_operation.result.settingsLoaded) {
				return false;
			}
			a_operation.result.settingsSnapshotted = Settings::WithLoadLock(
				CaptureRuntimeSnapshot,
				std::addressof(a_operation.snapshot),
				&a_operation.result.exceptionCode);
			if (!a_operation.result.settingsSnapshotted) {
				return false;
			}

			// Set before the call: an SEH anywhere inside BeginConfiguration may
			// already have made the epoch odd, so the outer finally must normalize it.
			a_operation.result.runtimeConfigurationBegan = true;
			const auto begin = Upscaler::HDRRoundTrip::BeginConfiguration();
			if (begin != Upscaler::HDRRoundTrip::ConfigurationBegin::kBegan) {
				if (begin == Upscaler::HDRRoundTrip::ConfigurationBegin::kDeferred) {
					// Refused before the epoch was touched or a gate cleared, so there is
					// genuinely nothing begun and nothing to undo. Clearing the flag is
					// what keeps RunApplyShell's __finally out of AbortRuntimeConfiguration,
					// whose TAAReplacement abort would latch that module's one-way latch and
					// turn the cool-down this refusal exists to protect into a terminal
					// session with a reason naming the reload.
					a_operation.result.runtimeConfigurationBegan = false;
					a_operation.result.runtimeConfigurationDeferred = true;
				}
				return false;
			}

			const auto& snapshot = a_operation.snapshot;

			// Any mode that differs from the one actually running must clear the
			// admissibility rule the engine's frozen bracket-admission bit imposes: a
			// reduced mode, or "dlaa", is only reachable live under the conditions
			// SettingsPolicy.h::ResolveModeSwitch derives from
			// Upscaler::DynamicResolution::WorldAdmissionGated() - read here, fresh, not
			// from a tracked copy, so this decision can never disagree with what the
			// engine actually froze.
			//
			// How the difference arrived does not change what the engine will do with it.
			// A live /mode request publishes a runtime override that Settings::Validate()
			// substitutes for the TOML's mode key; a /config/set of mode, a hand edit plus
			// /config/reload, or an override file left behind and picked up by kGameLoaded
			// all change the same field with no override standing. Exempting the second
			// group - as this did while the rule was written for /mode alone - let exactly
			// the switches /mode refuses through a different door: "dlaa" onto an armed
			// bracket, or a reduced mode onto a session that armed none, applied live and
			// reported as taken, until BuildNgxConfig disagreed with the frame contract and
			// disabled the session for good. The first apply of a session is not subject to
			// the rule at all: it is the one that decides what "running" means.
			//
			// effectiveMode is what actually derives the runtime configuration below; on a
			// refusal it is the mode already running, not the rejected request, and
			// *Settings::mode is corrected to match so nothing else that reads it -
			// /config's settings table included - is left showing a mode that was never
			// actually applied.
			std::string effectiveMode = snapshot.mode;
			const auto liveMode = g_liveMode.load(std::memory_order_acquire);
			const bool runtimeOverrideActive = Settings::RuntimeModeOverride() != nullptr;
			// One load, one comparison: an initialised session is one a previous apply
			// published a live mode for.
			const bool fileModeDiffers =
				!runtimeOverrideActive && liveMode != nullptr && snapshot.mode != *liveMode;
			if (runtimeOverrideActive || fileModeDiffers) {
				const std::string previousLiveMode = liveMode ? *liveMode : snapshot.mode;
				const std::string armedMode = SessionArmedModeString();
				const bool bracketArmed = Upscaler::DynamicResolution::WorldAdmissionGated();
				const auto decision = Settings::Policy::ResolveModeSwitch(
					previousLiveMode, snapshot.mode, bracketArmed, armedMode);
				a_operation.result.modeDecided = true;
				a_operation.result.modeOutcome = decision.outcome;
				// A standing override is re-read on every apply for as long as it stands
				// (kGameLoaded's own reload included), not only on the request that set
				// it. The epoch tells the two apart, so the switch counter and the log
				// line below advance once per actual /mode request rather than once per
				// unrelated reload that happens to find the same override still in force.
				//
				// The file-driven half has no epoch and never touches the two /state
				// counters: lastSwitchOutcome and switchAttempts are documented as the
				// last /mode request's, and a reload quietly advancing them would report
				// someone else's outcome under that name. Its outcome travels back in
				// ApplyResult instead, for the caller that asked for the change, and its
				// log line fires once per reload that finds the difference - which is once
				// per actual event, not once per frame.
				const std::uint64_t overrideEpoch = Settings::RuntimeModeOverrideEpoch();
				const bool freshRequest = runtimeOverrideActive &&
				                          overrideEpoch != g_processedOverrideEpoch.load(std::memory_order_acquire);
				const bool announce = freshRequest || fileModeDiffers;
				if (freshRequest) {
					g_processedOverrideEpoch.store(overrideEpoch, std::memory_order_release);
					g_lastSwitchOutcome.store(static_cast<std::uint32_t>(decision.outcome), std::memory_order_release);
					g_hasSwitchOutcome.store(true, std::memory_order_release);
					g_switchAttempts.fetch_add(1, std::memory_order_relaxed);
				}
				if (decision.outcome == Settings::Policy::SwitchOutcome::kRefusedNeedsReducedSession) {
					if (announce) {
						logger::warn(FMT_STRING("mode switch refused: '{}' needs a reduced-world session, but this session's bracket is not armed; restart with mode = '{}' in the TOML to use it live - staying on '{}'"),
							snapshot.mode, snapshot.mode, previousLiveMode);
					}
					effectiveMode = previousLiveMode;
					AssignModeUnderLoadLock(effectiveMode);
				} else if (decision.outcome == Settings::Policy::SwitchOutcome::kRefusedBracketArmed) {
					if (announce) {
						logger::warn(FMT_STRING("mode switch refused: this session rasterizes a reduced world and that arming cannot be undone at runtime, so 'dlaa' needs a restart with mode = 'dlaa' in the TOML - staying on '{}'"),
							previousLiveMode);
					}
					effectiveMode = previousLiveMode;
					AssignModeUnderLoadLock(effectiveMode);
				} else if (decision.outcome == Settings::Policy::SwitchOutcome::kUnchanged) {
					if (freshRequest) {
						logger::info(FMT_STRING("mode switch: '{}' is already running - no change"), previousLiveMode);
					}
					effectiveMode = previousLiveMode;
				} else if (decision.outcome == Settings::Policy::SwitchOutcome::kApplied) {
					if (announce) {
						logger::info(FMT_STRING("mode switch: '{}' -> '{}'"), previousLiveMode, decision.resolved);
					}
					effectiveMode = std::string(decision.resolved);
				} else {
					// kRejectedUnknownMode: RequestModeSwitch's own pre-decision already
					// refuses an unknown value before ever publishing an override (see
					// below), and Settings::ValidateMode has already replaced an
					// unrecognized file value with the default, so the authoritative
					// decision should not be able to reach this case in practice. Handled
					// defensively rather than assumed unreachable: stay on whatever is
					// already running.
					if (announce) {
						logger::warn(FMT_STRING("mode switch refused: '{}' is not a recognized mode - staying on '{}'"),
							snapshot.mode, previousLiveMode);
					}
					effectiveMode = previousLiveMode;
					AssignModeUnderLoadLock(effectiveMode);
				}
				// No path may leave an override standing that this decision did not honour:
				// a rejection/refusal reached here only because a concurrent request's
				// pre-decision (see RequestModeSwitch) approved it against conditions - the
				// live mode, chiefly - that had already changed by the time this
				// authoritative recheck ran under configurationLock. Clearing it here, and
				// only on a freshRequest, leaves the steady-state case untouched: once an
				// applied override settles, every later reload re-decides it as kUnchanged
				// (current already equals the override) on a non-fresh pass, and that must
				// keep standing so an unrelated reload does not revert to the TOML's own
				// mode key. Taking configurationLock for this whole request instead was
				// considered and rejected - RequestModeSwitch calls Apply() below, which
				// takes the same lock itself, so holding it across that call would deadlock.
				if (freshRequest && decision.outcome != Settings::Policy::SwitchOutcome::kApplied) {
					Settings::ClearRuntimeModeOverride();
				}
			}

			// mode derives the runtime configuration, in one place. Settings::Validate()
			// already refuses a diagnostic gate armed together with a mode other than "off",
			// so at most one of the two blocks below is ever live; the eleven diagnostic keys
			// decide exactly as before whenever mode is "off". A non-off mode computes the
			// same runtime inputs the diagnostic keys compute - the vendor path, the
			// native-TAA bypass, and for a reduced mode the reduced-world bracket - and feeds
			// them into the same Configure calls below; it does not add a parallel path.
			const bool productMode = effectiveMode != "off"sv;
			// mode's vocabulary is deliberately the same as quality's (plus "off" -
			// SettingsPolicy.h::IsKnownMode), so on the product path it doubles as the
			// quality name DefaultRenderScaleForQuality/VendorScalingRatioForQuality and
			// HDRRoundTrip::Configure's own quality lookup take. Off the product path this
			// stays the deprecated quality key, exactly as it always has.
			std::string_view qualityInput = snapshot.quality;
			bool ngxContinuousInput = snapshot.diagnosticNgxContinuous;
			bool ffxContinuousInput = snapshot.diagnosticFfxContinuous;
			bool taaBypassInput = snapshot.diagnosticTaaBypass;
			bool dynamicResolutionInput = snapshot.diagnosticDynamicResolution;
			// The product path has no target: the continuous gate and its coupled native-TAA
			// bypass both run for as long as the session lasts rather than retiring at a
			// frame count. See HDRRoundTrip::Configure and TAAReplacement::Configure for how
			// that is represented - a companion flag beside the numeric target, not a
			// sentinel value, so /state can say "unbounded" as distinct from "bounded at N"
			// rather than handing a reader a number that looks like an ordinary bound.
			//
			// This is also why the two episode latches those Configure calls gate
			// (HDRRoundTrip's g_continuousTargetState lock bit and TAAReplacement's
			// g_attempts-gated target) never go stale across a live mode switch without any
			// extra unlatch path: continuousUnbounded is productMode, and every mode a live
			// switch can reach (see ResolveModeSwitch - dlaa, off, or the one reduced mode
			// this session armed) either leaves productMode false, where the continuous gate
			// never latches anything at all, or true, where it is true on every apply that
			// could possibly reach the gate. The value the latch would re-see is therefore
			// always identical to what it is already holding; ConfigureContinuousTarget's
			// and TAAReplacement::ConfigureImpl's "requested live change ignored" branches
			// fire on a value equal to the one latched, never a different one.
			const bool continuousUnbounded = productMode;
			if (productMode) {
				qualityInput = effectiveMode;
				// Unconditional: Settings::Validate() forces mode to "off" whenever
				// diagnosticTaaReplacement (or diagnosticTaaBypass) is armed, so productMode
				// and an operator's accepted post-draw replacement never coexist here - this
				// cannot silently override it.
				taaBypassInput = true;
				// DLAA's ratio is 1.0 - no reduced-world bracket to arm, same reasoning as
				// the renderScale guard below for the diagnostic path.
				dynamicResolutionInput = effectiveMode != "dlaa"sv;
				if (snapshot.backend == "ffx"sv) {
					ffxContinuousInput = true;
					ngxContinuousInput = false;
				} else if (snapshot.backend == "ngx"sv || snapshot.backend == "auto"sv) {
					// "auto" resolves to ngx here and only here: every acceptance run to date
					// has used NGX, and there is no device to probe an SDK's real availability
					// against at the settings-apply layer (plugin load included, before the
					// engine has created one). A capability-driven fallback to ffx belongs
					// where the device exists - open, not attempted here.
					ngxContinuousInput = true;
					ffxContinuousInput = false;
				} else {
					// "streamline" and "null" are not wired into the continuous gate at all;
					// mode reports configured and productMode is true, but neither vendor flag
					// above goes true, so nothing reconstructs until one of those backends
					// gains the wiring ngx/ffx already have.
					logger::warn(FMT_STRING("Fo4VRUpscaler.toml: mode = '{}' selected with backend = '{}', which the product path does not yet drive; the session stays configured but nothing reconstructs"),
						effectiveMode, snapshot.backend);
				}
			}

			// Same derivation ApplyOperation's own prospective pass in Apply() already ran
			// once, outside configurationLock, to decide whether to engage the deny window
			// before this call ever took the lock - see DeriveReducedRatio's own comment
			// for why calling it twice, from the same qualityInput/dynamicResolutionInput
			// this authoritative recompute derived above, can never disagree about how the
			// ratio itself is derived, only about which mode/quality snapshot it saw.
			const auto ratioDerivation =
				DeriveReducedRatio(snapshot.renderScaleOverride, dynamicResolutionInput, qualityInput);
			const double renderScale = ratioDerivation.renderScale;
			const double vendorScalingRatio = ratioDerivation.vendorScalingRatio;
			const bool reducing = ratioDerivation.reducing;
			const bool gateReducedNgxToWorld = dynamicResolutionInput && reducing &&
			                                   (snapshot.diagnosticNgxHandback || ngxContinuousInput);
			if (ratioDerivation.tookModeDefault) {
				if (gateReducedNgxToWorld && vendorScalingRatio > 1.0) {
					logger::info(FMT_STRING("renderScaleOverride is 0, so quality '{}' uses its documented default ratio {:.4f}, snapped at the first world frame to the vendor scaling ratio {:.4f}"),
						qualityInput, renderScale, vendorScalingRatio);
				} else {
					// The snap runs from BeginReducedWorld and nowhere else, so a session
					// that does not gate the reduced world never reaches it. Its ratio is
					// written at plugin load, where no render target exists yet and there
					// is therefore no per-eye extent to aim at.
					logger::info(FMT_STRING("renderScaleOverride is 0, so quality '{}' uses its documented default ratio {:.4f}, written unsnapped: this session does not gate the reduced world, and no per-eye output extent is readable at plugin load"),
						qualityInput, renderScale);
				}
			}
			Upscaler::DynamicResolution::Configure(
				snapshot.enabled,
				dynamicResolutionInput,
				renderScale,
				vendorScalingRatio,
				snapshot.forceHiddenAreaMeshOff,
				gateReducedNgxToWorld);
			// Captured immediately after the one call that can ever freeze
			// DynamicResolution's own admission-gated bit: its g_configured latch fires on
			// this process's very first invocation of Configure(), regardless of what the
			// rest of this Apply() does afterward (see DynamicResolution.cpp). Capturing at
			// the end of this function instead - after TAAReplacement::Configure and
			// HDRRoundTrip::Configure below, both of which can fail and return early - left
			// a gap: a failed first apply would still freeze DynamicResolution's bit while
			// leaving the name uncaptured, and a later successful apply could then capture
			// a different name than what actually got frozen. effectiveMode here is exactly
			// what DynamicResolution just saw, whether or not the rest of this apply
			// succeeds. The captured value is deliberately not gated on gateReducedNgxToWorld:
			// it names which non-off mode this session's first apply configured, not
			// whether that configuration went on to arm the bracket - the two facts stay
			// independent, and Upscaler::DynamicResolution::WorldAdmissionGated() read live
			// is the only source of truth for the latter (see SettingsPolicy.h).
			{
				bool expectedNotCaptured = false;
				if (g_sessionArmedModeCaptured.compare_exchange_strong(
						expectedNotCaptured, true, std::memory_order_acq_rel)) {
					g_sessionArmedMode.store(
						std::make_shared<const std::string>(
							productMode ? effectiveMode : std::string{}),
						std::memory_order_release);
				}
			}

			// A session already past its first Configure() (DynamicResolution's own
			// !g_configured latch already fired, so the call above just returned) whose
			// bracket is armed can still take a new ratio live - through
			// Upscaler::DynamicResolution::RequestRatio, not a second Configure. This is
			// what makes SettingsPolicy.h::ResolveModeSwitch's kApplied-for-a-different-
			// reduced-mode row (item 4) and renderScaleOverride's live policy (item 5) both
			// work: both paths recompute renderScale/vendorScalingRatio above exactly as
			// Configure's own startup derivation does, so this one block, run on every
			// apply, is the single place either live change actually lands.
			//
			// The deny window itself is never engaged here. Apply()'s own prospective pass
			// (see its own comment) already decided - outside configurationLock, from a
			// snapshot it reloaded itself - whether this request was likely to change the
			// ratio, and engaged (and recorded in a_operation.ratioDenyWindowEngaged, for
			// RunApplyShell's __finally to release once every Configure call below has run)
			// before this function ever took the lock. That decision can disagree with this
			// one - the mode/quality snapshot it saw was reloaded moments before this
			// authoritative one - but ResetPolicy's engagement is refcounted precisely so
			// that never matters here: RequestRatio below either finds the window already
			// closed (the ordinary case) or refuses inertly via its own WorldFrameActive()
			// check (kRefusedWorldFrameActive) rather than writing a torn ratio, so a missed
			// prospective guess costs a refused live update, never a correctness problem.
			// A live switch request's own engagement stacks the same way - taken by
			// RequestModeSwitchAsync on the requesting thread before the worker ever
			// called this Apply(), released by RunSwitch on the worker afterwards, and
			// untouched here. Each engagement is released exactly once, by whoever owns
			// it; ownership belongs to the request, not to a thread.
			//
			// The answers are kept apart for the caller that persisted the key: a session
			// with no live reduced ratio at all, a ratio that already is the requested one,
			// and a request the module took or refused. "The apply published" is true in
			// every one of them and says nothing about which.
			if (!ReducedRatioIsLive(ratioDerivation)) {
				a_operation.result.ratioEffect = RatioEffect::kNotApplicable;
			} else if (!ReducedRatioWouldChange(ratioDerivation)) {
				a_operation.result.ratioEffect = RatioEffect::kAlreadyLive;
			} else {
				const auto currentState = Upscaler::DynamicResolution::Snapshot();
				const auto outcome = Upscaler::DynamicResolution::RequestRatio(renderScale, vendorScalingRatio);
				if (outcome == Upscaler::DynamicResolution::RatioRequestOutcome::kApplied ||
					outcome == Upscaler::DynamicResolution::RatioRequestOutcome::kUnchanged) {
					a_operation.result.ratioEffect = RatioEffect::kApplied;
				} else {
					a_operation.result.ratioEffect = RatioEffect::kRefused;
				}
				if (outcome == Upscaler::DynamicResolution::RatioRequestOutcome::kApplied) {
					logger::info(FMT_STRING("ratio request: {} -> {} (vendor ratio {} -> {}) applied live"),
						currentState.requestedScale, renderScale, currentState.vendorScalingRatio, vendorScalingRatio);
				} else if (outcome != Upscaler::DynamicResolution::RatioRequestOutcome::kUnchanged) {
					logger::warn(FMT_STRING("ratio request: {} -> {} refused ({})"),
						currentState.requestedScale, renderScale,
						Upscaler::DynamicResolution::RatioRequestOutcomeName(outcome));
				}
			}

			if (!Upscaler::TAAReplacement::Configure(
					snapshot.diagnosticTaaReplacement,
					taaBypassInput,
					ngxContinuousInput,
					ffxContinuousInput,
					snapshot.diagnosticContinuousFrames,
					static_cast<float>(snapshot.sharpness),
					continuousUnbounded)) {
				return false;
			}
			Upscaler::ResetPolicy::Configure(
				snapshot.resetOnMenu,
				snapshot.resetOnLoad,
				snapshot.resetOnCell);
			Upscaler::ScopePolicy::Configure(snapshot.suppressDuringScopeRender);
			// panelEnabled owns the panel key as well as the overlay: a disabled panel is
			// handed 0 here, so the key cannot be pressed to show something that will not
			// be created. The panel's own Configure only publishes atomics - the render
			// thread's seam is what creates or destroys the overlay from them.
			// The three live values beside the placement are the ones the panel's own
			// controls sit on. They come from this snapshot rather than from the global
			// Setting<T> instances because the panel reads them on the render thread, and
			// a load can be halfway through rewriting a global while a frame is being
			// produced.
			Upscaler::Panel::Configure(
				snapshot.panelEnabled,
				snapshot.panelWidthMeters,
				snapshot.panelDistanceMeters,
				snapshot.renderScaleOverride,
				snapshot.preset,
				snapshot.sharpness);
			Upscaler::Hotkeys::Configure(
				snapshot.hotkeyToggleVk,
				snapshot.hotkeyCompareVk,
				snapshot.panelEnabled ? snapshot.hotkeyPanelVk : 0);
			Upscaler::Jitter::ConfigureVendorSign(snapshot.jitterVendorSign);
			// Publication only. The table is rewritten from the render thread at the next
			// backend configuration, never from here.
			Upscaler::MipBias::Configure(
				static_cast<float>(snapshot.mipBiasOverride),
				snapshot.mipBiasIncludeTrilinear,
				snapshot.mipBiasLowerMinLod);
			static_cast<void>(Upscaler::StereoLayout::Resolve());
			// This setting participates in the same admission snapshot as the Quality
			// gates. Publish it while HDRRoundTrip's epoch is still odd; Configure below
			// is the final operation that makes the new runtime epoch visible.
			Upscaler::Hooks::Configure(snapshot.unbindDepthStencil);
			if (!Upscaler::HDRRoundTrip::Configure(
				snapshot.diagnosticHdrRoundTrip,
				snapshot.diagnosticStereoNull,
				snapshot.diagnosticNgxCreate,
				snapshot.diagnosticNgxEvaluate,
				snapshot.diagnosticNgxReassemble,
				snapshot.diagnosticNgxHandback,
				ngxContinuousInput,
				snapshot.diagnosticFfxEvaluate,
				snapshot.diagnosticFfxReassemble,
				snapshot.diagnosticFfxHandback,
				ffxContinuousInput,
				snapshot.diagnosticContinuousFrames,
				snapshot.runtimeDir,
				snapshot.preset,
					qualityInput,
					snapshot.stereoMode,
					snapshot.failSoft,
					effectiveMode,
					continuousUnbounded)) {
				return false;
			}
			// Atomic intent only. Query creation, collection and release are deferred to
			// the verified render thread by Telemetry itself.
			Upscaler::Telemetry::Configure(snapshot.perfTimers);
			a_operation.result.runtimeApplied = true;

			// Only reached once every Configure call above has published successfully, so
			// a failed apply never advances g_liveMode: the session really did keep running
			// in whatever mode was already live. The session-armed-mode capture happens
			// earlier, right after DynamicResolution::Configure - see the comment there.
			g_liveMode.store(
				std::make_shared<const std::string>(effectiveMode), std::memory_order_release);
			// Stored after g_liveMode, never before: a reader that sees a non-off live mode
			// has already seen this one move with it, and a reader that sees "off" is
			// reading the value from before the session was switched off - which is exactly
			// what it is for.
			if (effectiveMode != "off"sv) {
				g_lastNonOffLiveMode.store(
					std::make_shared<const std::string>(effectiveMode), std::memory_order_release);
			}

			if (a_operation.observer && a_operation.observer->afterApply) {
				a_operation.result.afterObserved = Settings::WithLoadLock(
					a_operation.observer->afterApply,
					a_operation.observer->context,
					&a_operation.result.exceptionCode);
				if (!a_operation.result.afterObserved) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool ApplyLockedThunk(ApplyOperation& a_operation) noexcept
		{
			try {
				return ApplyLockedImpl(a_operation);
			} catch (const std::exception& e) {
				logger::error(FMT_STRING("configuration apply failed: {}"), e.what());
			} catch (...) {
				logger::error("configuration apply failed with a non-standard C++ exception"sv);
			}
			return false;
		}

		// Kept separate from ApplyOperation because MSVC forbids destructible C++
		// locals in a function containing __try.  This shell is pointer/bool only.
		void RunApplyShell(ApplyOperation* a_operation) noexcept
		{
			struct Shell
			{
				ApplyOperation* operation = nullptr;
				bool locked = false;
			};
			Shell shell{};
			shell.operation = a_operation;
			__try {
				__try {
					if (shell.operation && Settings::detail::Acquire(Settings::configurationLock)) {
						shell.locked = true;
						shell.operation->callbackResult = ApplyLockedThunk(*shell.operation);
						shell.operation->returned = true;
					}
				} __finally {
					// Released before the Abort/unlock nested frame below, on every exit
					// path (success, an early `return false`, or the __except below) -
					// never left standing past this Apply() call, whatever else it did.
					// Releases only the one engagement this Apply() call itself took
					// (refcounted - see ResetPolicy::ReleaseModeSwitchPending); a live
					// switch request's own engagement, still standing around this whole
					// Apply() call, is untouched here and released by RunSwitch on the
					// switch worker once the Apply it is guarding has returned.
					if (shell.operation && shell.operation->ratioDenyWindowEngaged) {
						Upscaler::ResetPolicy::ReleaseModeSwitchPending();
						shell.operation->ratioDenyWindowEngaged = false;
					}
					__try {
						if (shell.operation &&
							shell.operation->result.runtimeConfigurationBegan &&
							!shell.operation->result.runtimeApplied) {
							AbortRuntimeConfiguration();
						}
					} __finally {
						// The outer lock is never conditional on recovery succeeding.  A
						// second hardware fault in a guarded Dynamic rollback still reaches
						// this nested finally before the exception is reported.
						if (shell.locked) {
							Settings::configurationLock.unlock();
							shell.locked = false;
						}
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				if (shell.operation) {
					shell.operation->result.exceptionCode =
						static_cast<std::uint32_t>(::GetExceptionCode());
				}
			}
		}

		// How long a caller waits for the render thread to actually observe the deny
		// window. Only a world frame writes WorldAdmissionPhase (ResetPolicy's
		// AdvanceQualityWorldAdmissionBeforeWorld, from the pre-world hook), so waiting
		// here is waiting on frame production: a thread the frame producer depends on
		// gains nothing by waiting longer, it just stops the frame that would have ended
		// the wait.
		//
		// The switch worker gets the full budget because it is provably neither the game
		// thread nor the render thread. The prospective ratio pass inside Apply() gets
		// about ten frames at 90 Hz instead, because the thread it runs on is whichever
		// one called Apply() and that set is not provably safe. Two of the callers are:
		// plugin load, which runs before a frame exists, and the DevBench listener thread
		// - both /config/reload and Settings::Writer::Set re-apply on the caller's own
		// thread, only the disk write being hopped to the game thread. The third is the
		// switch worker itself. The fourth is F4SE's lifecycle reload, dispatched by
		// F4SEVR on a thread of its own choosing, and nothing stops a fifth from being a
		// frame producer. That pass reaches the wait at all only when a reload would
		// actually change the ratio, and measured, the gate reports blocked one to two
		// frames after the window engages - so the short budget is still an order of
		// magnitude more than a healthy session needs, and a session where it is not
		// healthy is one where no budget would have helped.
		constexpr auto kModeSwitchBlockedWait = std::chrono::milliseconds(2000);
		constexpr auto kProspectiveRatioBlockedWait = std::chrono::milliseconds(120);

		// Poll interval, not a timing constant: short enough that a gate that blocks in
		// one frame is seen in that frame, long enough not to spin a core.
		constexpr auto kBlockedPollInterval = std::chrono::milliseconds(5);

		// True when the gate reported blocked inside a_budget. A false return is not an
		// error at either call site - both proceed, and both say so in their own log line.
		[[nodiscard]] bool WaitForWorldGateBlocked(std::chrono::milliseconds a_budget) noexcept
		{
			const auto deadline = std::chrono::steady_clock::now() + a_budget;
			for (;;) {
				if (Upscaler::ResetPolicy::Snapshot().worldAdmissionPhase ==
					Upscaler::ResetPolicy::WorldAdmissionPhase::kBlocked) {
					return true;
				}
				if (std::chrono::steady_clock::now() >= deadline) {
					return false;
				}
				std::this_thread::sleep_for(kBlockedPollInterval);
			}
		}

		// One key's value as the two settings files alone resolve it: the override's own
		// (non-legacy) table first, then the base table with Settings::LoadOne's legacy
		// [Fo4VRUpscaler] fallback. a_setting is read for its group()/key() only - its
		// live value is never touched, which is the whole point of this helper.
		template <class T>
		[[nodiscard]] T ReadFileValue(
			const Settings::Setting<T>& a_setting,
			const toml::table& a_base,
			const toml::table* a_override,
			const T& a_default)
		{
			if (a_override) {
				if (const auto tweak = (*a_override)[a_setting.group()][a_setting.key()]; tweak) {
					if (const auto value = tweak.value<T>(); value) {
						return *value;
					}
				}
			}
			return Settings::ResolveBaseOnly(a_setting, a_base, a_default);
		}

		// Prospective pass, entirely outside configurationLock and entirely read-only: it
		// parses the two settings files into locals of its own and asks whether the ratio
		// this apply is about to publish looks like it will differ from what
		// DynamicResolution::Snapshot() reports live right now. Mirrors
		// RequestModeSwitch's own pre-decision shape: a cheap, best-effort read taken
		// before the lock so the wait for the world gate to report blocked never runs
		// while every other settings operation queues behind configurationLock.
		//
		// Read-only is load-bearing, not tidiness. This used to call Settings::load(),
		// which mutates every live Setting<T> under loadLock alone while ApplyLockedImpl
		// writes *Settings::mode under configurationLock alone - two unsynchronised
		// std::string assignments to the same object whenever a game-thread apply and a
		// DevBench-thread one overlapped. It also meant every apply parsed both files,
		// validated and logged the whole settings line twice. Nothing here writes a global,
		// so the live settings are mutated exactly once per apply, by the authoritative
		// load inside the lock, and the settings line fires once.
		//
		// What it derives is deliberately a guess, not a second authority: the runtime
		// override substitutes for the file's mode the same way Validate() does, but the
		// deprecated quality alias, enabled = false, the diagnostic-gate refusal and the
		// backend resolution are not re-run here. A wrong guess in either direction costs
		// nothing but performance, never correctness - see ApplyLockedImpl's own comment on
		// its ratio-change block: a missed engagement still cannot let RequestRatio tear a
		// write, and an unneeded one is released the moment RunApplyShell's __finally runs.
		//
		// A failed parse here (a malformed or absent TOML) simply skips the pass -
		// ApplyLockedImpl's own Settings::load(), under configurationLock, is the
		// authoritative one, and if that one also fails the ratio-change block is never
		// reached at all, so no window is needed regardless.
		void EngageRatioDenyWindowForProspectiveChange(ApplyOperation& a_operation) noexcept
		{
			try {
				toml::table base;
				try {
					base = toml::parse_file("Data/F4SE/Plugins/Fo4VRUpscaler.toml"sv);
				} catch (...) {
					// Logged loudly by the authoritative load; silent here by design - this
					// pass never speaks for the loader.
					return;
				}
				toml::table overrideTable;
				bool haveOverride = false;
				{
					const auto overridePath = Settings::Paths::OverrideFilePath();
					std::error_code existsError;
					if (!overridePath.empty() && std::filesystem::exists(overridePath, existsError) && !existsError) {
						try {
							overrideTable = toml::parse_file(overridePath.string());
							haveOverride = true;
						} catch (...) {
						}
					}
				}
				const toml::table* const overrides = haveOverride ? &overrideTable : nullptr;
				const auto& defaults = Settings::CompiledDefaults();

				RuntimeSnapshot prospective;
				prospective.renderScaleOverride = ReadFileValue(
					Settings::renderScaleOverride, base, overrides, defaults.renderScaleOverride);
				prospective.quality = ReadFileValue(Settings::quality, base, overrides, defaults.quality);
				prospective.diagnosticDynamicResolution = ReadFileValue(
					Settings::diagnosticDynamicResolution, base, overrides, defaults.diagnosticDynamicResolution);
				const auto runtimeOverride = Settings::RuntimeModeOverride();
				prospective.mode = runtimeOverride ?
				                        *runtimeOverride :
				                        ReadFileValue(Settings::mode, base, overrides, defaults.mode);

				const auto derivation = DeriveReducedRatioFromMode(prospective, prospective.mode);
				if (!ReducedRatioWouldChange(derivation)) {
					return;
				}
				Upscaler::ResetPolicy::EngageModeSwitchPending();
				a_operation.ratioDenyWindowEngaged = true;
				if (!WaitForWorldGateBlocked(kProspectiveRatioBlockedWait)) {
					logger::warn("ratio request: world gate did not report blocked within the wait window; requesting anyway"sv);
				}
			} catch (...) {
				// A throwing allocation or a toml++ parse error here means the prospective
				// read never completed - nothing was engaged, so there is nothing to
				// release. ApplyLockedImpl's own authoritative pass still runs
				// unconditionally, catching the same class of failure the same way
				// ApplyLockedThunk does.
			}
		}

		// A live mode switch waits for the render thread to observe its deny window and
		// then runs the whole Configure chain. Neither may happen on a thread the render
		// thread's frame production depends on: a game thread parked in F4SE's task pump
		// produces no world frame, so the wait can only time out, and it takes the frame
		// that would have ended it down with it. That is the defect this worker exists to
		// remove - the hotkey used to hop to the task queue for precisely the reason it
		// must not (see Hotkeys.cpp).
		//
		// One request at a time. inFlight is claimed by the requesting thread before it
		// decides anything - the decision reads the live mode, and a switch already in
		// flight is exactly when that read is stale - and is handed back either by that
		// thread (every early return) or by the worker when the switch completes. A
		// second request meanwhile is refused kBusy rather than queued: two configurations
		// stacked behind one another is how a double press lands a session somewhere
		// neither press asked for.
		struct SwitchWorker
		{
			std::mutex mutex;
			// One condition variable for two different waits: the worker waits for
			// requestPending, a synchronous caller waits for completedTicket to reach its
			// own. Every wake is a notify_all and each side's own predicate sorts it out.
			std::condition_variable signal;
			bool threadStarted = false;
			// Latched by a failed start and never cleared. Creating a thread takes the
			// loader lock, so a retry has to happen on a thread that may take it - and
			// the only threads left to retry on after load are the two that must not.
			// One failure therefore disables live switching for the session instead of
			// arming a retry on the render thread.
			bool startFailed = false;
			bool inFlight = false;
			bool requestPending = false;
			// The mode the pre-decision resolved to, for the HUD line only. The apply
			// itself reads Settings' runtime override, never this copy.
			std::string resolvedMode;
			// What the override pair held before this request published its own, so a
			// failed apply can put back exactly what it displaced instead of clearing.
			// Clearing would drop an override that was already standing and had produced
			// the mode still running, leaving /config reading the TOML's mode key while
			// liveMode reads the other one. Carried through the worker rather than restored
			// on the requesting thread because only the worker learns whether the apply
			// published; the in-flight claim holds until the worker is done with it, so no
			// other request can have written the pair in between.
			std::shared_ptr<const std::string> previousOverride;
			std::uint64_t previousOverrideEpoch = 0;
			SwitchCompletion completion = nullptr;
			std::uint64_t requestTicket = 0;
			std::uint64_t completedTicket = 0;
			std::uint64_t nextTicket = 0;
		};

		// Deliberately never destroyed. The worker thread is detached and parks on signal
		// for the life of the process, so destroying this at DLL detach would either free
		// state a live thread still reads or require joining that thread from a static
		// destructor - which runs under the loader lock and would deadlock against the
		// worker's own thread-detach callbacks. Leaking one object at process exit is the
		// cheaper of the two, and the thread owns nothing process teardown does not
		// reclaim. No atomics here: every field is read and written under mutex.
		[[nodiscard]] SwitchWorker& Worker()
		{
			static SwitchWorker* const worker = new SwitchWorker();
			return *worker;
		}

		// Hands the in-flight claim back and clears the caller's own record of holding it,
		// so a later unwind path in the same call cannot release it a second time.
		void ReleaseSwitchClaim(bool& a_claimed) noexcept
		{
			if (!a_claimed) {
				return;
			}
			a_claimed = false;
			try {
				SwitchWorker& worker = Worker();
				std::lock_guard lock(worker.mutex);
				worker.inFlight = false;
			} catch (...) {
				logger::error("mode switch: the in-flight claim could not be handed back; later switches will be refused"sv);
			}
		}

		// Runs on the worker thread. The requesting thread engaged the deny window and
		// this releases it: ownership belongs to the request, not to a thread -
		// ResetPolicy's refcount only cares that every engagement is released exactly
		// once.
		void RunSwitch(
			const std::string& a_resolvedMode,
			std::shared_ptr<const std::string> a_previousOverride,
			std::uint64_t a_previousOverrideEpoch) noexcept
		{
			bool windowHeld = true;
			try {
				if (!WaitForWorldGateBlocked(kModeSwitchBlockedWait)) {
					logger::warn("mode switch: world gate did not report blocked within the wait window; publishing anyway"sv);
				}
				const auto applied = Apply(nullptr);
				Upscaler::ResetPolicy::ReleaseModeSwitchPending();
				windowHeld = false;
				char line[160];
				int n = 0;
				if (applied.runtimeApplied) {
					n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: mode switched to '%.*s'",
						static_cast<int>(a_resolvedMode.size()), a_resolvedMode.data());
				} else {
					// The apply never reached the block that advances g_liveMode, records
					// the outcome or clears the override it published, so all three are this
					// function's to settle - otherwise the HUD announces a switch that did
					// not happen, /hotkey republishes whatever the previous switch reached,
					// and the rejected override stands shadowing the TOML's mode key while
					// liveMode disagrees with it for the rest of the process.
					//
					// The second Apply is what puts the *settings* back: this one's load()
					// already substituted the rejected override into Settings::mode, so
					// /config would keep reporting a mode nothing is running. Re-running the
					// load with the override restored re-derives that field from what is
					// actually live. Its own result is discarded on purpose - it fails or
					// defers for the same reason this one did, and nothing new is being
					// asked for.
					const auto outcome = applied.runtimeConfigurationDeferred ?
					                         Settings::Policy::SwitchOutcome::kDeferredRecovering :
					                         Settings::Policy::SwitchOutcome::kFailedApply;
					Settings::RestoreRuntimeModeOverride(
						std::move(a_previousOverride), a_previousOverrideEpoch);
					static_cast<void>(Apply(nullptr));
					g_lastSwitchOutcome.store(static_cast<std::uint32_t>(outcome), std::memory_order_release);
					g_hasSwitchOutcome.store(true, std::memory_order_release);
					g_switchAttempts.fetch_add(1, std::memory_order_relaxed);
					const auto liveMode = LiveModeOr("off"sv);
					if (applied.runtimeConfigurationDeferred) {
						logger::warn(FMT_STRING("mode switch to '{}' deferred: the session is in its post-fault cool-down and the reload was refused; still running '{}'"),
							a_resolvedMode, liveMode);
						n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: mode switch to '%.*s' deferred - the session is recovering from a fault",
							static_cast<int>(a_resolvedMode.size()), a_resolvedMode.data());
					} else {
						logger::error(FMT_STRING("mode switch to '{}' did not apply: the configuration reload published nothing; still running '{}'"),
							a_resolvedMode, liveMode);
						n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: mode switch to '%.*s' did not apply - still running '%.*s'",
							static_cast<int>(a_resolvedMode.size()), a_resolvedMode.data(),
							static_cast<int>(liveMode.size()), liveMode.data());
					}
				}
				if (n > 0) {
					const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
					Upscaler::HudMessages::Enqueue(std::string_view(line, len));
				}
			} catch (...) {
				logger::error("mode switch: the switch did not complete"sv);
			}
			if (windowHeld) {
				Upscaler::ResetPolicy::ReleaseModeSwitchPending();
			}
		}

		void SwitchWorkerMain() noexcept
		{
			// Bound once. Worker() is already constructed by the time this thread runs -
			// StartSwitchWorker holds its mutex while creating the thread - and
			// the object is never destroyed, so this reference is good forever.
			SwitchWorker& worker = Worker();
			for (;;) {
				std::string resolvedMode;
				std::shared_ptr<const std::string> previousOverride;
				std::uint64_t previousOverrideEpoch = 0;
				SwitchCompletion completion = nullptr;
				std::uint64_t ticket = 0;
				bool popped = false;
				try {
					{
						std::unique_lock lock(worker.mutex);
						worker.signal.wait(lock, [&worker] { return worker.requestPending; });
						resolvedMode = std::move(worker.resolvedMode);
						worker.resolvedMode.clear();
						previousOverride = std::move(worker.previousOverride);
						worker.previousOverride = nullptr;
						previousOverrideEpoch = worker.previousOverrideEpoch;
						worker.previousOverrideEpoch = 0;
						completion = worker.completion;
						ticket = worker.requestTicket;
						worker.completion = nullptr;
						worker.requestPending = false;
						popped = true;
					}
					RunSwitch(resolvedMode, std::move(previousOverride), previousOverrideEpoch);
					const auto state = SwitchSnapshot();
					if (completion) {
						completion(state);
					}
				} catch (...) {
					logger::error("mode switch: the switch worker caught an exception; the request is still reported complete so the next one is not refused forever"sv);
				}
				if (!popped) {
					// Nothing was claimed, so there is nothing to hand back. Sleep rather
					// than spin: reaching here means the wait itself failed, and retrying
					// it flat out would burn a core for the rest of the session.
					std::this_thread::sleep_for(kBlockedPollInterval);
					continue;
				}
				{
					std::lock_guard lock(worker.mutex);
					worker.completedTicket = ticket;
					worker.inFlight = false;
				}
				worker.signal.notify_all();
			}
		}

		// The only place a thread is ever created. Called from Apply(), which no frame
		// producer reaches, because std::thread's constructor takes the loader lock to
		// run every loaded DLL's thread-attach callback and this holds worker.mutex
		// across it. A request that arrives before the worker exists asks
		// SwitchWorkerRunning() below instead and is refused; it does not start one.
		[[nodiscard]] bool StartSwitchWorker() noexcept
		{
			try {
				SwitchWorker& worker = Worker();
				std::lock_guard lock(worker.mutex);
				if (worker.threadStarted) {
					return true;
				}
				if (worker.startFailed) {
					return false;
				}
				try {
					std::thread(&SwitchWorkerMain).detach();
				} catch (...) {
					// Latched inside the lock, so the very next caller sees the failure
					// rather than trying again on whatever thread it happens to be.
					worker.startFailed = true;
					throw;
				}
				worker.threadStarted = true;
				logger::info("mode switch: switch worker started; a live switch runs there, never on the game or render thread"sv);
				return true;
			} catch (...) {
				logger::error("mode switch: the switch worker could not be started; live mode switches are unavailable for this session"sv);
				return false;
			}
		}

		// Read-only: a requesting thread may be the render thread, and it may not create
		// anything. False means either that no Apply() has run yet (impossible after
		// plugin load) or that the start latched failed, and both answer the request the
		// same way - kUnavailable, nothing published.
		[[nodiscard]] bool SwitchWorkerRunning() noexcept
		{
			try {
				SwitchWorker& worker = Worker();
				std::lock_guard lock(worker.mutex);
				return worker.threadStarted;
			} catch (...) {
				return false;
			}
		}

		// How long a synchronous caller waits for the worker. Long enough to cover the
		// blocked-gate budget plus a full Configure chain, short enough that a stuck
		// switch answers the DevBench route instead of holding its connection open.
		constexpr auto kSynchronousSwitchWait = std::chrono::seconds(10);
	}

	ApplyResult Apply(const Observer* a_observer) noexcept
	{
		ApplyOperation operation{};
		operation.observer = a_observer;
		// The one and only start site, rather than lazily at the first request: the first
		// request can arrive on the render thread (a real key press), and creating a
		// thread there takes the loader lock for every loaded DLL's thread-attach
		// callback. Every session runs Apply() at plugin load, on neither of the two
		// threads that matter. Idempotent, latched against retry after a failure, and the
		// worker's own Apply() call re-enters it holding nothing.
		static_cast<void>(StartSwitchWorker());
		// The panel's settings worker, started from the same place and for the same
		// reason: its first intent arrives from the render thread's own seam, and creating
		// a thread there takes the loader lock. Idempotent and latched against retry, and
		// the worker's own Settings::Writer::Set re-enters this function holding nothing.
		static_cast<void>(Upscaler::Panel::StartWorker());
		EngageRatioDenyWindowForProspectiveChange(operation);
		RunApplyShell(std::addressof(operation));
		if (!operation.returned && operation.result.exceptionCode != 0) {
			logger::error(FMT_STRING("configuration apply raised SEH 0x{:08X}"), operation.result.exceptionCode);
		}
		return operation.result;
	}

	// Every ModeSwitchState field is a std::string, and a throwing allocation inside a
	// noexcept function terminates the process - the same reason ApplyLockedThunk above
	// exists. A diagnostic report is the last thing that should end a session, so both
	// entry points below catch and return an empty state instead.
	ModeSwitchState SwitchSnapshot() noexcept
	{
		try {
			ModeSwitchState state{};
			state.liveMode = LiveModeOr("off"sv);
			state.modeSource = std::string(Settings::ModeSourceName(Settings::CurrentModeSource()));
			state.bracketArmed = Upscaler::DynamicResolution::WorldAdmissionGated();
			state.sessionArmedMode = SessionArmedModeString();
			state.lastNonOffMode = LastNonOffLiveModeString();
			state.switchAttempts = g_switchAttempts.load(std::memory_order_relaxed);
			state.lastSwitchOutcome = g_hasSwitchOutcome.load(std::memory_order_acquire) ?
				std::string(Configuration::SwitchOutcomeName(static_cast<Settings::Policy::SwitchOutcome>(
					g_lastSwitchOutcome.load(std::memory_order_acquire)))) :
				std::string("none");
			return state;
		} catch (...) {
			return {};
		}
	}

	std::string_view SwitchOutcomeName(Settings::Policy::SwitchOutcome a_outcome) noexcept
	{
		// One spelling, in the engine-free header, so the panel's pure model names an
		// outcome the same way this route does - see Settings::Policy::SwitchOutcomeName.
		return Settings::Policy::SwitchOutcomeName(a_outcome);
	}

	std::string_view SwitchDispatchName(SwitchDispatch a_dispatch) noexcept
	{
		switch (a_dispatch) {
		case SwitchDispatch::kStarted:
			return "started"sv;
		case SwitchDispatch::kBusy:
			return "busy"sv;
		case SwitchDispatch::kUnavailable:
			return "unavailable"sv;
		case SwitchDispatch::kResolved:
		default:
			return "resolved"sv;
		}
	}

	SwitchRequest RequestModeSwitchAsync(std::string_view a_requestedMode, SwitchCompletion a_completion) noexcept
	{
		SwitchRequest request{};
		// Declared ahead of the try block, not inside it, so the catch clause below can
		// see them too. The deny-window engagement is refcounted (ResetPolicy::
		// ReleaseModeSwitchPending refuses rather than going negative), so the catch block
		// must release only when this call actually engaged, not unconditionally; and the
		// in-flight claim must be handed back on exactly the paths that took it and never
		// on the one that handed it to the worker. The override pair is what an unwind
		// puts back: this call is the only writer of either while it holds the claim, so
		// restoring both is an exact inverse rather than a guess (see the catch below).
		bool denyWindowEngaged = false;
		bool claimed = false;
		std::shared_ptr<const std::string> previousOverride;
		std::uint64_t previousOverrideEpoch = 0;
		try {
			SwitchWorker& worker = Worker();
			{
				std::lock_guard lock(worker.mutex);
				if (!worker.inFlight) {
					worker.inFlight = true;
					claimed = true;
				}
			}
			if (!claimed) {
				logger::warn(FMT_STRING("mode switch: '{}' refused - a switch is already in flight"), a_requestedMode);
				request.dispatch = SwitchDispatch::kBusy;
				request.state = SwitchSnapshot();
				return request;
			}
			// Cheap pre-decision, outside any lock: decides only whether this request is
			// worth disturbing the world gate for. The authoritative decision - the one that
			// actually derives the runtime configuration - runs again inside ApplyLockedImpl
			// under Settings::configurationLock, so a race against a concurrent switch is
			// self-correcting there (see the override-clearing comment in ApplyLockedImpl),
			// never a correctness problem here. Deciding first and touching nothing for
			// kUnchanged/kRefused*/kRejectedUnknownMode is what makes a refusal genuinely
			// inert: it never engages the deny window, never publishes the override, and
			// never runs Apply() at all. ResolveModeSwitch validates a_requestedMode against
			// Settings::Policy::IsKnownMode itself, so this function does not pre-filter it
			// by hand.
			// Named locals, not temporaries passed inline: ResolveModeSwitch's result carries
			// string_views into whichever argument it picked, so those arguments have to
			// outlive preDecision's use below.
			const std::string preDecisionLiveMode = LiveModeOr("off"sv);
			const std::string preDecisionArmedMode = SessionArmedModeString();
			const bool preDecisionBracketArmed = Upscaler::DynamicResolution::WorldAdmissionGated();
			const auto preDecision = Settings::Policy::ResolveModeSwitch(
				preDecisionLiveMode, a_requestedMode, preDecisionBracketArmed, preDecisionArmedMode);
			if (preDecision.outcome == Settings::Policy::SwitchOutcome::kRejectedUnknownMode) {
				logger::warn(FMT_STRING("mode switch rejected: '{}' is not a recognized mode"), a_requestedMode);
				// Persisted so /state and a bare GET /mode both show it, but not counted as a
				// switch attempt: an unrecognized value never reached the admissibility
				// decision at all.
				g_lastSwitchOutcome.store(static_cast<std::uint32_t>(preDecision.outcome), std::memory_order_release);
				g_hasSwitchOutcome.store(true, std::memory_order_release);
				{
					char line[160];
					const auto n = std::snprintf(line, sizeof(line),
						"Fo4VR Upscaler: mode '%.*s' rejected - not a recognized mode",
						static_cast<int>(a_requestedMode.size()), a_requestedMode.data());
					if (n > 0) {
						const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
						Upscaler::HudMessages::Enqueue(std::string_view(line, len));
					}
				}
				ReleaseSwitchClaim(claimed);
				request.state = SwitchSnapshot();
				return request;
			}
			if (preDecision.outcome != Settings::Policy::SwitchOutcome::kApplied) {
				g_lastSwitchOutcome.store(static_cast<std::uint32_t>(preDecision.outcome), std::memory_order_release);
				g_hasSwitchOutcome.store(true, std::memory_order_release);
				g_switchAttempts.fetch_add(1, std::memory_order_relaxed);
				char line[160];
				int n = 0;
				if (preDecision.outcome == Settings::Policy::SwitchOutcome::kUnchanged) {
					logger::info(FMT_STRING("mode switch: '{}' is already running - no change"), preDecision.resolved);
					n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: mode '%.*s' already running - no change",
						static_cast<int>(preDecision.resolved.size()), preDecision.resolved.data());
				} else {
					logger::warn(FMT_STRING("mode switch refused: requested '{}', staying on '{}' ({})"),
						a_requestedMode, preDecision.resolved, Configuration::SwitchOutcomeName(preDecision.outcome));
					const auto outcomeName = Configuration::SwitchOutcomeName(preDecision.outcome);
					n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: mode switch to '%.*s' refused (%.*s) - staying on '%.*s'",
						static_cast<int>(a_requestedMode.size()), a_requestedMode.data(),
						static_cast<int>(outcomeName.size()), outcomeName.data(),
						static_cast<int>(preDecision.resolved.size()), preDecision.resolved.data());
				}
				if (n > 0) {
					const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
					Upscaler::HudMessages::Enqueue(std::string_view(line, len));
				}
				ReleaseSwitchClaim(claimed);
				request.state = SwitchSnapshot();
				return request;
			}

			// Accepted. Nothing below may run on the calling thread, so the worker has to
			// already exist before anything is published - an override standing with
			// nobody to apply it is worse than a refused request. Asked, never started:
			// this may be the render thread, and Apply() has started the worker off both
			// hot threads long before any key can be pressed.
			if (!SwitchWorkerRunning()) {
				ReleaseSwitchClaim(claimed);
				// Once per session, not once per press: the condition is latched, so a
				// held key would otherwise write this line every frame it repeats.
				static std::atomic<bool> s_unavailableLogged{ false };
				if (!s_unavailableLogged.exchange(true, std::memory_order_acq_rel)) {
					logger::error("mode switch: no switch worker is running; live mode switches are unavailable for this session"sv);
				}
				request.dispatch = SwitchDispatch::kUnavailable;
				request.state = SwitchSnapshot();
				return request;
			}

			// Allocated before anything is published, so the only step left between
			// publishing the override and handing the request off is a std::mutex lock.
			// Everything else that can throw has already thrown by here, with nothing
			// standing to undo.
			std::string resolvedForWorker(preDecision.resolved);
			previousOverride = Settings::RuntimeModeOverride();
			previousOverrideEpoch = Settings::RuntimeModeOverrideEpoch();

			Settings::SetRuntimeModeOverride(a_requestedMode);
			// Engaged here, released by the worker. Doing it on this thread rather than
			// on the worker closes the window between publishing the override and denying
			// the world: an unrelated Apply() landing in that gap would otherwise pick the
			// override up and switch the mode with no deny window at all.
			Upscaler::ResetPolicy::EngageModeSwitchPending();
			denyWindowEngaged = true;
			{
				std::lock_guard lock(worker.mutex);
				// Move, not assign: std::string's and shared_ptr's move assignments are
				// both noexcept, so no step inside this lock can throw with the override
				// already standing. The pair the worker gets is this call's own record of
				// what it displaced - the same pair the catch below would put back - so a
				// failed apply on the far side restores exactly what an unwind on this
				// side would have.
				worker.resolvedMode = std::move(resolvedForWorker);
				worker.previousOverride = previousOverride;
				worker.previousOverrideEpoch = previousOverrideEpoch;
				worker.completion = a_completion;
				worker.requestTicket = ++worker.nextTicket;
				worker.requestPending = true;
				request.ticket = worker.requestTicket;
			}
			// Handed off. The claim and the window are the worker's from here, so this
			// function's own unwind paths must not touch either: clear both flags before
			// anything that could still throw.
			claimed = false;
			denyWindowEngaged = false;
			worker.signal.notify_all();
			request.dispatch = SwitchDispatch::kStarted;
			request.state = SwitchSnapshot();
			return request;
		} catch (...) {
			// A throw before the hand-off means the request published nothing or published
			// an override the worker never took: the two flags say exactly which.
			// Releasing the deny window only when denyWindowEngaged records this call as
			// the current holder - unconditionally releasing a refcounted window would
			// either drop someone else's still-standing engagement or trip
			// ReleaseModeSwitchPending's own unmatched-release guard. The window is
			// engaged one noexcept step after the override is published, so that same
			// flag also means "the override below is mine to put back".
			//
			// Put back, not cleared: clearing would discard whatever override was already
			// standing, which is a live mode change made by a request that reported
			// changing nothing. Both halves are restored, because Apply() reads a bumped
			// epoch as "a fresh request landed" and clears any standing override its
			// authoritative decision does not come out kApplied on - a restored override
			// under this call's bumped epoch would be thrown away by the next reload. The
			// in-flight claim is what makes the pair exact: no other switch can have
			// written either while this one held it.
			if (denyWindowEngaged) {
				Upscaler::ResetPolicy::ReleaseModeSwitchPending();
				Settings::RestoreRuntimeModeOverride(previousOverride, previousOverrideEpoch);
			}
			ReleaseSwitchClaim(claimed);
			logger::error("mode switch could not be reported; nothing was changed"sv);
			return {};
		}
	}

	SwitchRequest RequestModeSwitch(std::string_view a_requestedMode) noexcept
	{
		auto request = RequestModeSwitchAsync(a_requestedMode, nullptr);
		if (request.dispatch != SwitchDispatch::kStarted) {
			// kBusy in particular: request.state is the running switch's, not this
			// request's, and the dispatch travelling back with it is the only thing that
			// says so.
			return request;
		}
		try {
			SwitchWorker& worker = Worker();
			std::unique_lock lock(worker.mutex);
			const bool completed = worker.signal.wait_for(lock, kSynchronousSwitchWait,
				[&worker, ticket = request.ticket] { return worker.completedTicket >= ticket; });
			if (!completed) {
				logger::warn("mode switch: the switch did not complete within the wait window; reporting the state as it stands"sv);
			}
		} catch (...) {
			logger::error("mode switch: waiting for the switch to complete failed; reporting the state as it stands"sv);
		}
		// Re-snapshotted after the wait: the state the asynchronous request returned was
		// taken before the worker had run anything.
		request.state = SwitchSnapshot();
		return request;
	}
}
