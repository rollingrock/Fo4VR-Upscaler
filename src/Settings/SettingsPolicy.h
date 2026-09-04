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
		kExplicit,      // mode's own parsed value, untouched by any alias below
		kQualityAlias,  // mode was absent, the deprecated quality key's value was used
		kEnabledFalse,  // the deprecated enabled = false forced mode to "off"
		kGateForced,    // an armed pre-HDR diagnostic gate forced mode to "off"
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
	}
}
