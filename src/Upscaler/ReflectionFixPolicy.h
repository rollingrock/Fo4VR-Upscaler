#pragma once

// Whether the HUD says the reduced-mode reflection fix is off, and why, as pure functions,
// so the engine-free harness in tools/world-admission-tests runs the same per-frame step
// HudMessages::PollFrame does, latch included.
//
// The fix is CompositeScale's DFComposite camera correction. It runs only in a reduced
// mode, only with compositeReconstructionScale on, only through the prepared-packet draw
// helper hook Hooks installs at load, only for the shader package ShaderPackage
// qualified, and it declines every draw once the live DFComposite table has differed from
// the audited family. Each input below is a fact one of those modules already publishes;
// nothing here detects anything, and nothing here changes what the correction does.
//
// DLAA and off never get the line. DLAA renders at ratio 1, where the correction passes
// every draw through, and off reconstructs nothing. technique65600ReconstructionScale is
// not an input: it needs compositeReconstructionScale, and on its own it covers the
// composite pass's shaders that sample no cubemap, so the reflection fix runs without it.

#include "Settings/SettingsPolicy.h"

#include <cstdint>
#include <string_view>

namespace Upscaler::HudMessages::Detail
{
	enum class ReflectionFixOff : std::uint32_t
	{
		// No line: not a reduced world, the correction armed and healthy, or the package
		// verdict not in yet.
		kNone = 0,
		// compositeReconstructionScale = false, as Hooks::Configure last published it.
		kSettings = 1,
		// ShaderPackage checked the package the game loads and it did not qualify: a loose
		// or archived replacement, a different or recompressed package, a runtime replacer,
		// or a Data folder or package it could not read. Its line names the likely cause
		// without claiming it.
		kShaderPackage = 2,
		// The package qualified, but the live DFComposite table differs from the audited
		// family, so the correction declines every draw.
		kShaderTable = 3,
		// Hooks found the prepared-packet draw helper entry changed at load and left it
		// alone, so nothing of the correction ever runs. In the one report so far another
		// plugin had hooked that entry; the line names that as the likely cause.
		kHookDeclined = 4,
	};

	// a_reducedWorld: this frame admitted the world of a reduced session. a_hookDeclined is
	// Hooks::CompositeHookDeclined(). a_packageDecided is ShaderPackage::Decided(), never
	// Checked(), which is already true while the archives are still being read.
	//
	// The order is the order the correction is reached in. The setting comes first: it is
	// the player's own choice and it gates everything, the package check included. The hook
	// is next, ahead of the package: it is the only way into the correction, settled at
	// load, so with it missing the package verdict changes nothing and the line need not
	// wait for the scan. A package that did not qualify never arms the correction, and only
	// an armed correction, through the hook, ever reads the table.
	[[nodiscard]] constexpr ReflectionFixOff DecideReflectionFixOff(
		bool a_reducedWorld,
		bool a_settingOn,
		bool a_hookDeclined,
		bool a_packageDecided,
		bool a_packageQualified,
		bool a_tableMismatch) noexcept
	{
		if (!a_reducedWorld) {
			return ReflectionFixOff::kNone;
		}
		if (!a_settingOn) {
			return ReflectionFixOff::kSettings;
		}
		if (a_hookDeclined) {
			return ReflectionFixOff::kHookDeclined;
		}
		if (!a_packageDecided) {
			return ReflectionFixOff::kNone;
		}
		if (!a_packageQualified) {
			return ReflectionFixOff::kShaderPackage;
		}
		return a_tableMismatch ? ReflectionFixOff::kShaderTable : ReflectionFixOff::kNone;
	}

	// Whether a session can show the line at all, from the non-off mode its first apply
	// configured (Configuration::ModeSwitchState::sessionArmedMode, empty when that apply
	// configured off). The name is frozen by that apply, before any world frame, so a live
	// switch cannot race it the way it can race the live mode: a session armed in a reduced
	// mode moves only between reduced modes and off, and one armed in DLAA or off never
	// reaches a reduced mode.
	[[nodiscard]] constexpr bool ReflectionFixSessionEligible(std::string_view a_sessionArmedMode) noexcept
	{
		return Settings::Policy::IsReducedMode(a_sessionArmedMode);
	}

	// One frame's facts. PollFrame loads them one statement each, packageDecided before
	// packageQualified, so a verdict is read only once it is finished.
	struct ReflectionFixFacts
	{
		// The once-per-session latch, as this poll found it.
		bool reported = false;
		// ReflectionFixSessionEligible, captured once at the "active" edge.
		bool reducedSession = false;
		// This frame admitted the world. Off arms no native-TAA bypass, so it admits none.
		bool worldAdmitted = false;
		bool settingOn = false;
		bool hookDeclined = false;
		bool packageDecided = false;
		bool packageQualified = false;
		bool tableMismatch = false;
	};

	struct ReflectionFixPoll
	{
		// The line to queue this frame; kNone for none.
		ReflectionFixOff queue = ReflectionFixOff::kNone;
		// The latch after this frame. A queued line closes it, and a closed latch stays closed.
		bool reported = false;
	};

	// PollFrame's per-frame step. It runs every frame; the caller only makes the latch
	// atomic, so that of two pollers (the render thread and DevBench's /hudmsg) that both
	// find it open, one queues.
	[[nodiscard]] constexpr ReflectionFixPoll PollReflectionFix(const ReflectionFixFacts& a_facts) noexcept
	{
		if (a_facts.reported) {
			return { ReflectionFixOff::kNone, true };
		}
		const auto reason = DecideReflectionFixOff(
			a_facts.reducedSession && a_facts.worldAdmitted,
			a_facts.settingOn,
			a_facts.hookDeclined,
			a_facts.packageDecided,
			a_facts.packageQualified,
			a_facts.tableMismatch);
		return { reason, reason != ReflectionFixOff::kNone };
	}

	// The player-facing line for a reason, empty for kNone. Short enough for one HUD
	// message slot; HudMessages.cpp asserts each one fits whole.
	[[nodiscard]] constexpr std::string_view ReflectionFixOffLine(ReflectionFixOff a_reason) noexcept
	{
		using namespace std::string_view_literals;
		switch (a_reason) {
		case ReflectionFixOff::kSettings:
			return "BlurAway VR: reflection fix off (turned off in the settings); reflections may differ per eye. DLAA is unaffected."sv;
		case ReflectionFixOff::kShaderPackage:
			return "BlurAway VR: reflection fix off (the game's shaders don't match the tested ones, likely a shader mod); reflections may differ per eye. DLAA is unaffected."sv;
		case ReflectionFixOff::kShaderTable:
			return "BlurAway VR: reflection fix off (a mod changed the game's shaders); reflections may differ per eye. DLAA is unaffected."sv;
		case ReflectionFixOff::kHookDeclined:
			return "BlurAway VR: reflection fix off (the game code it hooks was already changed, likely by another mod); reflections may differ per eye. DLAA is unaffected."sv;
		case ReflectionFixOff::kNone:
		default:
			return {};
		}
	}

	// The rules the plugin build itself enforces; the harness walks the whole table.
	// Arguments: reducedWorld, settingOn, hookDeclined, packageDecided, packageQualified, tableMismatch.
	static_assert(DecideReflectionFixOff(false, false, true, true, false, true) == ReflectionFixOff::kNone, "DLAA and off never get the line");
	static_assert(DecideReflectionFixOff(true, false, true, true, false, true) == ReflectionFixOff::kSettings, "the setting is named before anything it gates");
	static_assert(DecideReflectionFixOff(true, true, true, true, true, false) == ReflectionFixOff::kHookDeclined, "a declined hook with a qualified package");
	static_assert(DecideReflectionFixOff(true, true, true, true, false, false) == ReflectionFixOff::kHookDeclined, "the hook outranks the package");
	static_assert(DecideReflectionFixOff(true, true, true, false, false, false) == ReflectionFixOff::kHookDeclined, "and does not wait for the package scan");
	static_assert(DecideReflectionFixOff(true, true, false, true, false, false) == ReflectionFixOff::kShaderPackage, "a package that did not qualify");
	static_assert(DecideReflectionFixOff(true, true, false, true, false, true) == ReflectionFixOff::kShaderPackage, "the package outranks the table");
	static_assert(DecideReflectionFixOff(true, true, false, true, true, true) == ReflectionFixOff::kShaderTable, "a table that differs from the audited family");
	static_assert(DecideReflectionFixOff(true, true, false, true, true, false) == ReflectionFixOff::kNone, "an armed, healthy correction gets no line");
	static_assert(DecideReflectionFixOff(true, true, false, false, false, false) == ReflectionFixOff::kNone, "no package verdict, no claim");
	static_assert(ReflectionFixOffLine(ReflectionFixOff::kNone).empty(), "no reason, no line");
	static_assert(PollReflectionFix({ .reducedSession = true, .worldAdmitted = true, .settingOn = true, .hookDeclined = true }).queue == ReflectionFixOff::kHookDeclined,
		"the step carries the hook through");
	static_assert(ReflectionFixSessionEligible("balanced"), "a reduced session is eligible");
	static_assert(!ReflectionFixSessionEligible("dlaa") && !ReflectionFixSessionEligible(""), "DLAA and off sessions are not");
	static_assert(PollReflectionFix({ .reducedSession = true, .worldAdmitted = true }).queue == ReflectionFixOff::kSettings, "an open latch queues");
	static_assert(PollReflectionFix({ .reducedSession = true, .worldAdmitted = true }).reported, "and closes");
	static_assert(PollReflectionFix({ .reported = true, .reducedSession = true, .worldAdmitted = true }).queue == ReflectionFixOff::kNone, "a closed latch queues nothing");
	static_assert(PollReflectionFix({ .reducedSession = true }).queue == ReflectionFixOff::kNone, "a frame that admitted no world queues nothing");
	static_assert(!PollReflectionFix({}).reported, "nothing queued leaves the latch open");
}
