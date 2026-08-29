#pragma once

// All offsets here are RVAs against Fallout4VR.exe 1.2.72 (image base 0x140000000,
// no ASLR), written without the 0x140 prefix.
//
// Verification tags:
//   [LIVE]   byte-verified in the running game (x64dbg)
//   [GHIDRA] verified against dump code bytes (which match the live process exactly)
//
// An untagged constant is unverified and is treated as a defect in review. A tag is
// followed by a one-line derivation naming the function or the instruction the value
// was decoded from, and for a patch site by the original bytes, so Hooks::Install can
// byte-verify before it writes anything.
//
// Ghidra's data labels for high .data/.bss in the unpacked dump are systematically
// wrong (section layout mismatch in the imported image). Every data RVA here comes
// from RIP-displacement decoding of code bytes or a live memory read - never from a
// bare DAT_ label.
//
// Reversing artefacts carry a US_ prefix in the Ghidra project so a later session can
// tell this project's findings from the tool's guesses, and the comment beside a
// constant names the US_ symbol it corresponds to.
//
// Naming: k + PascalCase. The suffix carries the kind - ...CallSite for a call
// instruction to patch, ...Fn for a function entry, ...Instance / ...Global /
// ...Singleton for data, kRT_... for a logical render-target index.
//
// This file is deliberately EMPTY of values. Nothing may be entered here from another
// repository's notes, from an address-library id belonging to flat Fallout 4, or from
// arithmetic on a flat-game offset. Every entry is re-found in Fallout4VR.exe and
// tagged. See docs/MODULE_MAP.md for the list of sites still to find.
//
// Call-only function RVAs used by exactly one translation unit stay in a file-local
// constexpr block next to their typedef; this file is for patch sites and for anything
// used twice.

#include <cstdint>

namespace Upscaler::Addr
{
	// --- code (patch/hook sites) ---

	// --- data ---

	// --- render target indices (logical, via the RenderTargetManager remap table) ---
}
