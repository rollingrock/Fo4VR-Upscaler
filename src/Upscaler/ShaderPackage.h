#pragma once

// Whether the shader package the game loads is the one the composite correction was audited
// against. The correction rewrites camera rows for 183 DFComposite records by ID and pixel-shader
// pointer; a package or replacer that keeps those IDs but changes the bytecode would be corrected
// blindly, so the correction runs only while this module says the package is the audited one.

#include <string>

namespace Upscaler::ShaderPackage
{
	// Plugin load, before the first Configuration::Apply. Finds the Shaders012_VR.fxp the game
	// will load - a loose override, another archive carrying it, or the record in the game's own
	// VR shader archive - and compares its bytes with the audited package. File reads only.
	void Qualify() noexcept;

	// kPostPostLoad, once every plugin is loaded. A known runtime shader replacer swaps bytecode
	// at creation, which no package hash can see. Returns Qualified() afterwards.
	[[nodiscard]] bool ProbeRuntimeReplacers() noexcept;

	// Any thread. False until Qualify has run, and false once either check has failed.
	[[nodiscard]] bool Qualified() noexcept;
	[[nodiscard]] std::string Reason();
}
