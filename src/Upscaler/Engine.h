#pragma once

// Acquires the D3D11 device and immediate context and the engine's graphics singletons,
// so nothing else in the plugin has to know where they live.
// Blocked on: the immediate-context and singleton RVAs, re-found in Fallout4VR.exe and
// entered in Addresses.h with a tag and a derivation.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace Upscaler::Engine
{
	// The device is never read from a global. It is obtained from the immediate context
	// with GetDevice, which removes one address from the ledger.
	[[nodiscard]] bool Init();

	[[nodiscard]] ID3D11Device* Device() noexcept;
	[[nodiscard]] ID3D11DeviceContext* ImmediateContext() noexcept;

	// BSGraphics::State, the render-target manager and the renderer are objects at fixed
	// addresses, not pointers to them.
	[[nodiscard]] std::uintptr_t GraphicsState() noexcept;
	[[nodiscard]] std::uintptr_t RenderTargetManager() noexcept;
	[[nodiscard]] std::uintptr_t Renderer() noexcept;

	// Decodes the RIP-relative operand of the instruction at a_instrRva, after verifying
	// its opcode bytes. Returns 0 and logs on a mismatch, so a moved anchor disables one
	// feature instead of silently pointing at the wrong memory. Preferred over a
	// hardcoded data address wherever a stable code anchor exists: Ghidra's labels for
	// high .data and .bss in this dump are section-shifted, and the code bytes are not.
	//
	// a_dispOffset is the byte offset of the displacement within the instruction,
	// a_instrLen the instruction's total length - the displacement is relative to the
	// address of the next instruction.
	[[nodiscard]] std::uintptr_t RipResolve(
		std::uintptr_t a_instrRva,
		std::initializer_list<std::uint8_t> a_opcode,
		std::size_t a_dispOffset,
		std::size_t a_instrLen,
		std::string_view a_what);
}
