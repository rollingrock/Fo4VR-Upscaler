#pragma once

// Whether the HUD says the reduced-mode reflection fix is off, and why, as one pure
// decision, so the engine-free harness in tools/world-admission-tests runs the same
// function HudMessages::PollFrame does.
//
// The fix is CompositeScale's DFComposite camera correction. It runs only in a reduced
// mode, only with compositeReconstructionScale on, only for the shader package
// ShaderPackage qualified, and it declines every draw once the live DFComposite table
// has differed from the audited family. Each input below is a fact one of those modules
// already publishes; nothing here detects anything, and nothing here changes what the
// correction does.
//
// DLAA and off never get the line. DLAA renders at ratio 1, where the correction passes
// every draw through, and off reconstructs nothing. technique65600ReconstructionScale is
// not an input: it needs compositeReconstructionScale, and on its own it covers the
// composite pass's shaders that sample no cubemap, so the reflection fix runs without it.

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
		// or archived replacement, a different package, or a runtime replacer.
		kShaderPackage = 2,
		// The package qualified, but the live DFComposite table differs from the audited
		// family, so the correction declines every draw.
		kShaderTable = 3,
	};

	// a_reducedWorld: this frame admitted the world of a session whose live mode is a
	// reduced one. The order is the order the correction itself fails in: the setting
	// gates everything, a package that did not qualify never arms the correction, and only
	// an armed correction ever reads the table.
	[[nodiscard]] constexpr ReflectionFixOff DecideReflectionFixOff(
		bool a_reducedWorld,
		bool a_settingOn,
		bool a_packageChecked,
		bool a_packageQualified,
		bool a_tableMismatch) noexcept
	{
		if (!a_reducedWorld) {
			return ReflectionFixOff::kNone;
		}
		if (!a_settingOn) {
			return ReflectionFixOff::kSettings;
		}
		if (!a_packageChecked) {
			return ReflectionFixOff::kNone;
		}
		if (!a_packageQualified) {
			return ReflectionFixOff::kShaderPackage;
		}
		return a_tableMismatch ? ReflectionFixOff::kShaderTable : ReflectionFixOff::kNone;
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
			return "BlurAway VR: reflection fix off (a shader mod replaces the game's shaders); reflections may differ per eye. DLAA is unaffected."sv;
		case ReflectionFixOff::kShaderTable:
			return "BlurAway VR: reflection fix off (a mod changed the game's shaders); reflections may differ per eye. DLAA is unaffected."sv;
		case ReflectionFixOff::kNone:
		default:
			return {};
		}
	}

	// The rules the plugin build itself enforces; the harness walks the whole table.
	static_assert(DecideReflectionFixOff(false, false, true, false, true) == ReflectionFixOff::kNone, "DLAA and off never get the line");
	static_assert(DecideReflectionFixOff(true, false, true, false, true) == ReflectionFixOff::kSettings, "the setting is named before anything it gates");
	static_assert(DecideReflectionFixOff(true, true, true, false, false) == ReflectionFixOff::kShaderPackage, "a package that did not qualify");
	static_assert(DecideReflectionFixOff(true, true, true, false, true) == ReflectionFixOff::kShaderPackage, "the package outranks the table");
	static_assert(DecideReflectionFixOff(true, true, true, true, true) == ReflectionFixOff::kShaderTable, "a table that differs from the audited family");
	static_assert(DecideReflectionFixOff(true, true, true, true, false) == ReflectionFixOff::kNone, "an armed, healthy correction gets no line");
	static_assert(DecideReflectionFixOff(true, true, false, false, false) == ReflectionFixOff::kNone, "no package verdict, no claim");
	static_assert(ReflectionFixOffLine(ReflectionFixOff::kNone).empty(), "no reason, no line");
}
