#include "Settings/Configuration.h"

#include "Upscaler/DynamicResolution.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/Hotkeys.h"
#include "Upscaler/HudMessages.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/MipBias.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/TAAReplacement.h"
#include "Upscaler/Telemetry.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

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

		[[nodiscard]] std::string_view SwitchOutcomeName(Settings::Policy::SwitchOutcome a_outcome) noexcept
		{
			switch (a_outcome) {
			case Settings::Policy::SwitchOutcome::kApplied:
				return "applied"sv;
			case Settings::Policy::SwitchOutcome::kUnchanged:
				return "unchanged"sv;
			case Settings::Policy::SwitchOutcome::kRefusedNeedsReducedSession:
				return "refused-needs-reduced-session"sv;
			case Settings::Policy::SwitchOutcome::kRefusedRatioChange:
				return "refused-ratio-change"sv;
			case Settings::Policy::SwitchOutcome::kRefusedBracketArmed:
				return "refused-bracket-armed"sv;
			case Settings::Policy::SwitchOutcome::kRejectedUnknownMode:
				return "rejected-unknown-mode"sv;
			default:
				return "unknown"sv;
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
		};

		struct ApplyOperation
		{
			const Observer* observer = nullptr;
			RuntimeSnapshot snapshot;
			ApplyResult result{};
			bool locked = false;
			bool returned = false;
			bool callbackResult = false;
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
			if (!Upscaler::HDRRoundTrip::BeginConfiguration()) {
				return false;
			}

			const auto& snapshot = a_operation.snapshot;

			// A live /mode request stands in for the TOML's mode key (Settings::Validate()
			// substitutes it before alias resolution - see Settings.h) but must still clear
			// the admissibility rule the engine's frozen bracket-admission bit imposes: a
			// reduced mode, or "dlaa", is only reachable live under the conditions
			// SettingsPolicy.h::ResolveModeSwitch derives from
			// Upscaler::DynamicResolution::WorldAdmissionGated() - read here, fresh, not
			// from a tracked copy, so this decision can never disagree with what the
			// engine actually froze. An ordinary TOML-only reload (no override active) is
			// exempt by design - it changes nothing about how mode has always been read
			// from the file, and this rule exists to keep a *live* switch honourable, not
			// to add new refusal semantics to hand-editing the file. effectiveMode is what
			// actually derives the runtime configuration below; on a refusal it is the
			// mode already running, not the rejected request, and *Settings::mode is
			// corrected to match so nothing else that reads it - /config's settings table
			// included - is left showing a mode that was never actually applied.
			std::string effectiveMode = snapshot.mode;
			const bool runtimeOverrideActive = Settings::RuntimeModeOverride() != nullptr;
			if (runtimeOverrideActive) {
				const std::string previousLiveMode = LiveModeOr(snapshot.mode);
				const std::string armedMode = SessionArmedModeString();
				const bool bracketArmed = Upscaler::DynamicResolution::WorldAdmissionGated();
				const auto decision = Settings::Policy::ResolveModeSwitch(
					previousLiveMode, snapshot.mode, bracketArmed, armedMode);
				// A standing override is re-read on every apply for as long as it stands
				// (kGameLoaded's own reload included), not only on the request that set
				// it. The epoch tells the two apart, so the switch counter and the log
				// line below advance once per actual /mode request rather than once per
				// unrelated reload that happens to find the same override still in force.
				const std::uint64_t overrideEpoch = Settings::RuntimeModeOverrideEpoch();
				const bool freshRequest =
					overrideEpoch != g_processedOverrideEpoch.load(std::memory_order_acquire);
				if (freshRequest) {
					g_processedOverrideEpoch.store(overrideEpoch, std::memory_order_release);
					g_lastSwitchOutcome.store(static_cast<std::uint32_t>(decision.outcome), std::memory_order_release);
					g_hasSwitchOutcome.store(true, std::memory_order_release);
					g_switchAttempts.fetch_add(1, std::memory_order_relaxed);
				}
				if (decision.outcome == Settings::Policy::SwitchOutcome::kRefusedNeedsReducedSession) {
					if (freshRequest) {
						logger::warn(FMT_STRING("mode switch refused: '{}' needs a reduced-world session, but this session's bracket is not armed; restart with mode = '{}' in the TOML to use it live - staying on '{}'"),
							snapshot.mode, snapshot.mode, previousLiveMode);
					}
					effectiveMode = previousLiveMode;
					*Settings::mode = effectiveMode;
				} else if (decision.outcome == Settings::Policy::SwitchOutcome::kRefusedRatioChange) {
					if (freshRequest) {
						logger::warn(FMT_STRING("mode switch refused: this session armed '{}', and the engine reduction ratio is frozen to it for the process - a switch to '{}' needs a restart with mode = '{}' in the TOML - staying on '{}'"),
							armedMode, snapshot.mode, snapshot.mode, previousLiveMode);
					}
					effectiveMode = previousLiveMode;
					*Settings::mode = effectiveMode;
				} else if (decision.outcome == Settings::Policy::SwitchOutcome::kRefusedBracketArmed) {
					if (freshRequest) {
						logger::warn(FMT_STRING("mode switch refused: this session rasterizes a reduced world and that arming cannot be undone at runtime, so 'dlaa' needs a restart with mode = 'dlaa' in the TOML - staying on '{}'"),
							previousLiveMode);
					}
					effectiveMode = previousLiveMode;
					*Settings::mode = effectiveMode;
				} else if (decision.outcome == Settings::Policy::SwitchOutcome::kUnchanged) {
					if (freshRequest) {
						logger::info(FMT_STRING("mode switch: '{}' is already running - no change"), previousLiveMode);
					}
					effectiveMode = previousLiveMode;
				} else if (decision.outcome == Settings::Policy::SwitchOutcome::kApplied) {
					if (freshRequest) {
						logger::info(FMT_STRING("mode switch: '{}' -> '{}'"), previousLiveMode, decision.resolved);
					}
					effectiveMode = std::string(decision.resolved);
				} else {
					// kRejectedUnknownMode: RequestModeSwitch's own pre-decision already
					// refuses an unknown value before ever publishing an override (see
					// below), so the authoritative decision should not be able to reach
					// this case in practice. Handled defensively rather than assumed
					// unreachable: stay on whatever is already running.
					if (freshRequest) {
						logger::warn(FMT_STRING("mode switch refused: '{}' is not a recognized mode - staying on '{}'"),
							snapshot.mode, previousLiveMode);
					}
					effectiveMode = previousLiveMode;
					*Settings::mode = effectiveMode;
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

			// 0.0 means "use the documented per-quality-mode default ratio" rather than
			// "hand-type one". DLAA is deliberately excluded: its ratio is 1.0, which is
			// not a reduced mode, and DynamicResolution's own 0.30..1.00 validation still
			// owns every other decision about the value.
			double renderScale = snapshot.renderScaleOverride;
			// Non-zero only on the default path. A typed override is written verbatim,
			// because a user who names a ratio is not asking for the vendor's.
			double vendorScalingRatio = 0.0;
			bool tookModeDefault = false;
			if (renderScale == 0.0 && dynamicResolutionInput) {
				double defaultScale = 0.0;
				if (Upscaler::HDRRoundTrip::DefaultRenderScaleForQuality(qualityInput, defaultScale) &&
					defaultScale < 1.0) {
					renderScale = defaultScale;
					tookModeDefault = true;
					double ratio = 0.0;
					if (Upscaler::HDRRoundTrip::VendorScalingRatioForQuality(qualityInput, ratio) &&
						ratio > 1.0) {
						vendorScalingRatio = ratio;
					}
				}
			}
			// A DLAA session leaves renderScale at 0.0: its ratio is 1.0, so the lookup
			// above declines and nothing replaces the zero. `renderScale < 1.0` on its own
			// calls that a reduction and arms the reduced-world bracket for a session that
			// has no reduction to bracket - and then neither bracket applies the mip bias,
			// because the gated arm never latches a contract and the native arm is switched
			// off by the same flag. Requiring an actual reduction sends a DLAA session down
			// the native seam whatever dynamicResolutionInput says.
			const bool reducing = renderScale > 0.0 && renderScale < 1.0;
			const bool gateReducedNgxToWorld = dynamicResolutionInput && reducing &&
			                                   (snapshot.diagnosticNgxHandback || ngxContinuousInput);
			if (tookModeDefault) {
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
			Upscaler::Hotkeys::Configure(snapshot.hotkeyToggleVk, snapshot.hotkeyCompareVk);
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
	}

	ApplyResult Apply(const Observer* a_observer) noexcept
	{
		ApplyOperation operation{};
		operation.observer = a_observer;
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
			state.switchAttempts = g_switchAttempts.load(std::memory_order_relaxed);
			state.lastSwitchOutcome = g_hasSwitchOutcome.load(std::memory_order_acquire) ?
				std::string(SwitchOutcomeName(static_cast<Settings::Policy::SwitchOutcome>(
					g_lastSwitchOutcome.load(std::memory_order_acquire)))) :
				std::string("none");
			return state;
		} catch (...) {
			return {};
		}
	}

	ModeSwitchState RequestModeSwitch(std::string_view a_requestedMode) noexcept
	{
		try {
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
				return SwitchSnapshot();
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
						a_requestedMode, preDecision.resolved, SwitchOutcomeName(preDecision.outcome));
					const auto outcomeName = SwitchOutcomeName(preDecision.outcome);
					n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: mode switch to '%.*s' refused (%.*s) - staying on '%.*s'",
						static_cast<int>(a_requestedMode.size()), a_requestedMode.data(),
						static_cast<int>(outcomeName.size()), outcomeName.data(),
						static_cast<int>(preDecision.resolved.size()), preDecision.resolved.data());
				}
				if (n > 0) {
					const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
					Upscaler::HudMessages::Enqueue(std::string_view(line, len));
				}
				return SwitchSnapshot();
			}

			Settings::SetRuntimeModeOverride(a_requestedMode);
			Upscaler::ResetPolicy::SetModeSwitchPending(true);
			// Best-effort wait for the render thread to actually observe the deny window and
			// close world admission before publishing the new configuration - avoids handing
			// a reduced-world bracket in flight a mismatched render extent mid-frame. Not the
			// only safety net: HDRRoundTrip::BeginConfiguration and TAAReplacement's own
			// config-epoch spin (see the module map) already keep Apply() below from racing
			// an in-flight evaluation regardless of whether this wait ever ends, so a timeout
			// here logs and proceeds rather than blocking the switch forever.
			{
				using namespace std::chrono_literals;
				const auto deadline = std::chrono::steady_clock::now() + 2s;
				bool blocked = false;
				while (std::chrono::steady_clock::now() < deadline) {
					if (Upscaler::ResetPolicy::Snapshot().worldAdmissionPhase ==
						Upscaler::ResetPolicy::WorldAdmissionPhase::kBlocked) {
						blocked = true;
						break;
					}
					std::this_thread::sleep_for(5ms);
				}
				if (!blocked) {
					logger::warn("mode switch: world gate did not report blocked within the wait window; publishing anyway"sv);
				}
			}

			static_cast<void>(Apply(nullptr));
			Upscaler::ResetPolicy::SetModeSwitchPending(false);
			{
				char line[160];
				const auto n = std::snprintf(line, sizeof(line), "Fo4VR Upscaler: mode switched to '%.*s'",
					static_cast<int>(preDecision.resolved.size()), preDecision.resolved.data());
				if (n > 0) {
					const auto len = static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;
					Upscaler::HudMessages::Enqueue(std::string_view(line, len));
				}
			}
			return SwitchSnapshot();
		} catch (...) {
			// A failed allocation here means the request never published anything: the
			// override, the deny window and Apply() all come after the pre-decision's own
			// string copies. Releasing the deny window anyway is harmless when it was
			// never engaged and necessary when it was.
			Upscaler::ResetPolicy::SetModeSwitchPending(false);
			logger::error("mode switch could not be reported; nothing was changed"sv);
			return {};
		}
	}
}
