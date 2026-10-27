#pragma once

// Whether the shader package the game loads is the one the composite correction was audited
// against. The correction rewrites camera rows for 183 DFComposite records by ID and pixel-shader
// pointer; a package or replacer that keeps those IDs but changes the bytecode would be corrected
// blindly, so the correction runs only while this module says the package is the audited one.

#include <string>

namespace Upscaler::ShaderPackage
{
	// Runs the package check the first time it is called; later calls do nothing. It finds the
	// Shaders012_VR.fxp the game will load - a loose override, another archive carrying it, or
	// the record in the game's own VR shader archive - and compares its bytes with the audited
	// package. It reads every general archive's name table, so Configuration::Apply calls it only
	// for a configuration that could use the correction: a product mode at a reduced ratio with
	// compositeReconstructionScale on. A session that never gets there, DLAA's included, reads
	// no archive. File reads only; serialized by this module's own lock.
	void EnsureChecked() noexcept;

	// kPostPostLoad, once every plugin is loaded. A known runtime shader replacer swaps bytecode
	// at creation, which no package hash can see; once one is found the package does not qualify
	// for the rest of the session. True when one was found.
	[[nodiscard]] bool DetectRuntimeReplacer() noexcept;

	// Any thread. False until EnsureChecked has qualified the package, and false once either
	// check has failed.
	[[nodiscard]] bool Qualified() noexcept;
	// Any thread. Whether EnsureChecked has run.
	[[nodiscard]] bool Checked() noexcept;
	[[nodiscard]] std::string Reason();
}
