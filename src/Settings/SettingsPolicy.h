#pragma once

#include <string_view>

// Engine-free settings policy: the mode/quality/enabled alias resolution and the
// gate-versus-mode refusal, as pure functions over std::string_view and bools. No
// toml, no spdlog, no CommonLibF4 - Validate() in Settings.h is the only caller and
// owns every log line these decisions provoke. Proved with consteval contracts and
// static_assert below, the way Upscaler::ResetPolicy::AdvanceAdmission proves the
// world-admission reducer.

namespace Settings::Policy
{
	using namespace std::string_view_literals;

	// The full quality vocabulary plus "off". Shared by mode; quality keeps its own
	// narrower ValidateChoice call in Settings.h since it does not accept "off".
	[[nodiscard]] constexpr bool IsKnownMode(std::string_view a_mode) noexcept
	{
		return a_mode == "off"sv || a_mode == "dlaa"sv || a_mode == "quality"sv ||
		       a_mode == "balanced"sv || a_mode == "performance"sv ||
		       a_mode == "ultraperformance"sv;
	}

	// Which rule, if any, moved the resolved mode away from the raw parsed value.
	// Validate() reports this alongside the resolved mode; only one of these is ever
	// the reason a given load ended where it did, because ResolveModeAliases applies
	// them in a fixed order and each later rule overrides an earlier one's result.
	// kGateForced is set outside ResolveModeAliases, by Validate() after
	// ResolveGateRefusal below - it is the last word regardless of what the alias
	// resolver decided.
	enum class ModeSource : std::uint8_t
	{
		kExplicit,         // mode's own parsed value, untouched by any alias below
		kQualityAlias,     // mode was absent, the deprecated quality key's value was used
		kEnabledFalse,     // the deprecated enabled = false forced mode to "off"
		kGateForced,       // an armed pre-HDR diagnostic gate forced mode to "off"
		kRuntimeOverride,  // a live /mode switch stands in for the TOML's mode this load -
		                   // see Settings::SetRuntimeModeOverride and Configuration.cpp
	};

	struct ModeAliasResult
	{
		std::string_view mode;
		ModeSource source = ModeSource::kExplicit;
		bool qualityAliasApplied = false;  // mode absent, quality present - one log line
		bool modeQualityConflict = false;  // both present and disagree - mode wins, one log line
		bool enabledFalseForcedOff = false;  // enabled = false present - one log line
		bool enabledTrueDeprecated = false;  // enabled = true present - one log line
	};

	// Pure alias resolution over already-parsed values and file-presence flags -
	// Validate() resolves presence via Settings::WasPresent() and passes the raw
	// parsed *mode/*quality/*enabled values in; it applies the result and logs.
	//
	// Order matters and is deliberate: the quality alias only fires when mode itself
	// was absent, so an explicit mode always beats it (and disagreement is reported,
	// not silently resolved by taking the earlier key). enabled = false is applied
	// last and unconditionally forces "off" over whatever the quality alias chose,
	// because enabled is the one legacy key that used to mean "nothing runs" and mode
	// must keep meaning that too.
	[[nodiscard]] constexpr ModeAliasResult ResolveModeAliases(
		std::string_view a_mode,
		bool a_modePresent,
		std::string_view a_quality,
		bool a_qualityPresent,
		bool a_enabled,
		bool a_enabledPresent) noexcept
	{
		ModeAliasResult result{ a_mode, ModeSource::kExplicit, false, false, false, false };

		if (a_qualityPresent) {
			if (!a_modePresent) {
				result.mode = a_quality;
				result.source = ModeSource::kQualityAlias;
				result.qualityAliasApplied = true;
			} else if (a_mode != a_quality) {
				result.modeQualityConflict = true;
			}
		}

		if (a_enabledPresent) {
			if (!a_enabled) {
				result.mode = "off"sv;
				result.source = ModeSource::kEnabledFalse;
				result.enabledFalseForcedOff = true;
			} else {
				result.enabledTrueDeprecated = true;
			}
		}

		return result;
	}

	// The diagnostic gates are the regression harness, and the runner sets
	// mode = "off" whenever it arms one. A gate armed alongside any other mode is a
	// contradiction the load refuses rather than silently picking a winner -
	// Validate() calls this after the diagnostic mutual-exclusion cascade, with
	// a_anyGateArmed already reduced to one bool.
	[[nodiscard]] constexpr std::string_view ResolveGateRefusal(
		std::string_view a_mode, bool a_anyGateArmed) noexcept
	{
		return (a_anyGateArmed && a_mode != "off"sv) ? "off"sv : a_mode;
	}

	// DynamicResolution::WorldAdmissionGated() (src/Upscaler/DynamicResolution.cpp) is
	// frozen for the process at the session's first Configure() call: whatever that first
	// call decided about gating the reduced-world bracket to world admission is what the
	// engine holds for the rest of the process, independent of which mode is live at any
	// later moment. A reduced mode (anything but "off"/"dlaa") is therefore reachable by a
	// live switch whenever the bracket is armed, whether or not it is the exact mode the
	// session armed: DynamicResolution::RequestRatio (see its own header comment) can
	// repatch the engine ratio live now, so a different reduced mode's default ratio is a
	// live rewrite rather than a restart - only the bracket's own arming stays frozen, not
	// the ratio inside it.
	//
	// "dlaa" carries no ratio of its own, but a session whose bracket is armed still
	// cannot reach it live. Hooks.cpp gates BeginReducedWorld on
	// DynamicResolution::WorldAdmissionGated() && QualityHandbackReady(), and the product
	// path forces QualityHandbackReady() true for every non-off mode including "dlaa"
	// (Configuration.cpp sets taaBypassInput = true unconditionally on the product path).
	// So a session that armed a reduced mode and then live-switched to "dlaa" would still
	// have the bracket open every frame while HDRRoundTrip published a native quality - a
	// mismatch BuildNgxConfig catches and disables the session over, permanently. Only
	// "off" is reachable from every session unconditionally: the product path's else
	// branch leaves taaBypassInput at the diagnostic key (false in shipping
	// configuration), which turns QualityHandbackReady() false and stops the bracket from
	// opening at all - TAAReplacement::Configure re-publishes that flag on every apply,
	// unlike DynamicResolution's one-shot freeze.
	//
	// a_currentMode is whatever is actually running now (the live mode, not necessarily
	// the TOML's), a_requestedMode is the /mode ask, a_bracketArmed is
	// DynamicResolution::WorldAdmissionGated() read at decision time (never a separately
	// tracked copy), and a_sessionArmedMode is the non-off mode this session's first
	// successful apply configured, or "" when that first apply configured "off". The two
	// facts are independent - a session that armed "dlaa" at startup gates no bracket, so
	// a_bracketArmed is false for it even though a_sessionArmedMode is not empty - and
	// both are reported in /state. Total and pure: Configuration.cpp is the only caller
	// that turns the outcome into a runtime effect or a log line - specifically, the one
	// that derives the new ratio and calls DynamicResolution::RequestRatio with it when
	// this decision comes back kApplied for a bracket-armed session.
	enum class SwitchOutcome : std::uint8_t
	{
		kApplied,                     // a real change the session can honour
		kUnchanged,                   // requested == current; no work, not an error
		kRefusedNeedsReducedSession,  // a reduced mode requested by a session with no bracket armed
		kRefusedBracketArmed,         // "dlaa" requested while this session's bracket is armed
		kRejectedUnknownMode,         // a_requestedMode is outside IsKnownMode's vocabulary
	};

	struct ModeSwitchDecision
	{
		std::string_view resolved;  // what is (or remains) running after this decision
		SwitchOutcome outcome = SwitchOutcome::kUnchanged;
	};

	[[nodiscard]] constexpr ModeSwitchDecision ResolveModeSwitch(
		std::string_view a_currentMode,
		std::string_view a_requestedMode,
		bool a_bracketArmed,
		std::string_view a_sessionArmedMode) noexcept
	{
		if (a_requestedMode == a_currentMode) {
			return { a_currentMode, SwitchOutcome::kUnchanged };
		}
		if (!IsKnownMode(a_requestedMode)) {
			return { a_currentMode, SwitchOutcome::kRejectedUnknownMode };
		}
		if (a_requestedMode == "off"sv) {
			return { a_requestedMode, SwitchOutcome::kApplied };
		}
		if (a_bracketArmed) {
			if (a_requestedMode == a_sessionArmedMode) {
				return { a_requestedMode, SwitchOutcome::kApplied };
			}
			if (a_requestedMode == "dlaa"sv) {
				return { a_currentMode, SwitchOutcome::kRefusedBracketArmed };
			}
			// An empty armed mode with the bracket armed is the diagnostic path: the gates,
			// not a product mode, opened the bracket, and Validate() forces mode to "off"
			// while any of them is armed. There is no armed reduced mode to repatch toward,
			// so this stays the no-reduced-session refusal - DynamicResolution::RequestRatio
			// also refuses inertly here (Status never reaches kArmed/kApplied on the
			// diagnostic path unless mode itself gated it, which Validate() already
			// contradicts), so refusing the switch decision first is the honest answer, not
			// merely a cheaper one.
			if (a_sessionArmedMode.empty()) {
				return { a_currentMode, SwitchOutcome::kRefusedNeedsReducedSession };
			}
			// A different reduced mode than the one this session armed. The bracket's own
			// arming is still frozen - only the ratio inside it changes - so this is now a
			// live rewrite through DynamicResolution::RequestRatio rather than a refusal:
			// Configuration.cpp derives a_requestedMode's default ratio exactly as it does
			// for any other apply and requests it once this decision reports kApplied.
			return { a_requestedMode, SwitchOutcome::kApplied };
		}
		if (a_requestedMode == "dlaa"sv) {
			return { a_requestedMode, SwitchOutcome::kApplied };
		}
		return { a_currentMode, SwitchOutcome::kRefusedNeedsReducedSession };
	}

	namespace detail
	{
		[[nodiscard]] consteval bool QualityAliasContract()
		{
			const auto result =
				ResolveModeAliases("dlaa"sv, false, "balanced"sv, true, true, false);
			return result.mode == "balanced"sv && result.source == ModeSource::kQualityAlias &&
			       result.qualityAliasApplied && !result.modeQualityConflict &&
			       !result.enabledFalseForcedOff;
		}

		[[nodiscard]] consteval bool ModeWinsOnConflictContract()
		{
			const auto result =
				ResolveModeAliases("quality"sv, true, "balanced"sv, true, true, false);
			return result.mode == "quality"sv && result.source == ModeSource::kExplicit &&
			       result.modeQualityConflict && !result.qualityAliasApplied;
		}

		[[nodiscard]] consteval bool EnabledFalseForcesOffContract()
		{
			// The alias would have taken quality's value first; enabled = false must
			// still win over it.
			const auto result =
				ResolveModeAliases("dlaa"sv, false, "quality"sv, true, false, true);
			return result.mode == "off"sv && result.source == ModeSource::kEnabledFalse &&
			       result.enabledFalseForcedOff && result.qualityAliasApplied;
		}

		[[nodiscard]] consteval bool EnabledTrueIsDeprecationOnlyContract()
		{
			const auto result =
				ResolveModeAliases("balanced"sv, true, "balanced"sv, false, true, true);
			return result.mode == "balanced"sv && result.source == ModeSource::kExplicit &&
			       result.enabledTrueDeprecated && !result.enabledFalseForcedOff;
		}

		[[nodiscard]] consteval bool GateForcesOffContract()
		{
			return ResolveGateRefusal("quality"sv, true) == "off"sv &&
			       ResolveGateRefusal("dlaa"sv, true) == "off"sv;
		}

		[[nodiscard]] consteval bool GateLeavesModeAloneContract()
		{
			// A gate already resolved to "off" stays "off"; an unarmed gate never
			// touches any mode.
			return ResolveGateRefusal("off"sv, true) == "off"sv &&
			       ResolveGateRefusal("quality"sv, false) == "quality"sv;
		}

		static_assert(QualityAliasContract());
		static_assert(ModeWinsOnConflictContract());
		static_assert(EnabledFalseForcesOffContract());
		static_assert(EnabledTrueIsDeprecationOnlyContract());
		static_assert(GateForcesOffContract());
		static_assert(GateLeavesModeAloneContract());

		static_assert(IsKnownMode("off"sv));
		static_assert(IsKnownMode("dlaa"sv));
		static_assert(IsKnownMode("ultraperformance"sv));
		static_assert(!IsKnownMode("nonsense"sv));
		static_assert(!IsKnownMode(""sv));
		static_assert(!IsKnownMode("Off"sv));  // case-sensitive on purpose, like every other ValidateChoice value

		[[nodiscard]] consteval bool SwitchSameModeIsUnchangedContract()
		{
			// requested == current wins regardless of bracket state or armed mode.
			const auto r = ResolveModeSwitch("quality"sv, "quality"sv, true, "quality"sv);
			return r.resolved == "quality"sv && r.outcome == SwitchOutcome::kUnchanged;
		}

		[[nodiscard]] consteval bool SwitchUnknownModeRejectedContract()
		{
			// An unrecognised value is rejected before any bracket/armed-mode reasoning
			// runs, and touches neither the resolved mode nor the session state.
			const auto armed = ResolveModeSwitch("quality"sv, "nonsense"sv, true, "quality"sv);
			const auto unarmed = ResolveModeSwitch("off"sv, "nonsense"sv, false, ""sv);
			return armed.resolved == "quality"sv && armed.outcome == SwitchOutcome::kRejectedUnknownMode &&
			       unarmed.resolved == "off"sv && unarmed.outcome == SwitchOutcome::kRejectedUnknownMode;
		}

		[[nodiscard]] consteval bool SwitchToOffAlwaysAppliedContract()
		{
			// "off" is reachable from any session, bracket armed or not - it is the one
			// mode that always turns QualityHandbackReady() false (see the comment above
			// ResolveModeSwitch), so it never leaves the bracket open against a mismatched
			// published quality.
			const auto fromUnarmed = ResolveModeSwitch("dlaa"sv, "off"sv, false, ""sv);
			const auto fromArmedReduced = ResolveModeSwitch("quality"sv, "off"sv, true, "quality"sv);
			return fromUnarmed.resolved == "off"sv && fromUnarmed.outcome == SwitchOutcome::kApplied &&
			       fromArmedReduced.resolved == "off"sv && fromArmedReduced.outcome == SwitchOutcome::kApplied;
		}

		[[nodiscard]] consteval bool SwitchDlaaAppliedOnlyWhenBracketUnarmedContract()
		{
			// A session whose first apply armed no bracket (started "off" or "dlaa") can
			// reach "dlaa" live; a session whose bracket is armed cannot - see
			// kRefusedBracketArmed and the comment above ResolveModeSwitch for why that
			// combination would leave the bracket open against a published native quality.
			const auto reachable = ResolveModeSwitch("off"sv, "dlaa"sv, false, ""sv);
			const auto refused = ResolveModeSwitch("quality"sv, "dlaa"sv, true, "quality"sv);
			return reachable.resolved == "dlaa"sv && reachable.outcome == SwitchOutcome::kApplied &&
			       refused.resolved == "quality"sv && refused.outcome == SwitchOutcome::kRefusedBracketArmed;
		}

		[[nodiscard]] consteval bool SwitchReducedNeedsArmedBracketContract()
		{
			// The session's first apply armed no bracket (started "off"/"dlaa") and a
			// reduced mode is requested live.
			const auto r = ResolveModeSwitch("off"sv, "quality"sv, false, ""sv);
			return r.resolved == "off"sv && r.outcome == SwitchOutcome::kRefusedNeedsReducedSession;
		}

		[[nodiscard]] consteval bool SwitchReducedModeChangeAppliedContract()
		{
			// The session armed "quality"; asking for a different reduced mode is now
			// applied - DynamicResolution::RequestRatio can repatch the engine ratio live,
			// so the bracket staying frozen no longer means the ratio inside it does.
			// Returning to the armed mode itself is honoured exactly as before.
			const auto changed = ResolveModeSwitch("off"sv, "balanced"sv, true, "quality"sv);
			const auto returned = ResolveModeSwitch("off"sv, "quality"sv, true, "quality"sv);
			// A diagnostic session opens the bracket with no armed product mode at all; that
			// is still the no-reduced-session refusal, not a ratio change between two named
			// modes - there is no default ratio to derive for an empty armed mode.
			const auto diagnostic = ResolveModeSwitch("off"sv, "quality"sv, true, ""sv);
			return changed.resolved == "balanced"sv && changed.outcome == SwitchOutcome::kApplied &&
			       returned.resolved == "quality"sv && returned.outcome == SwitchOutcome::kApplied &&
			       diagnostic.resolved == "off"sv &&
			       diagnostic.outcome == SwitchOutcome::kRefusedNeedsReducedSession;
		}

		static_assert(SwitchSameModeIsUnchangedContract());
		static_assert(SwitchUnknownModeRejectedContract());
		static_assert(SwitchToOffAlwaysAppliedContract());
		static_assert(SwitchDlaaAppliedOnlyWhenBracketUnarmedContract());
		static_assert(SwitchReducedNeedsArmedBracketContract());
		static_assert(SwitchReducedModeChangeAppliedContract());
	}
}
