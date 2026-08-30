#pragma once

// Acquires the engine's graphics handles - the immediate context, the graphics state,
// the render-target manager and the renderer - so nothing else in the plugin has to
// know where they live.
//
// Nothing in this file calls a D3D11 method. Every handle - the device included - comes
// from a global the engine already keeps, reached by decoding a verified instruction's
// RIP displacement and then reading memory. That makes Init() and every accessor safe at
// plugin load and safe on any thread.
//
// The pointers it hands out are a different matter: USING them is render-thread only,
// like every other D3D11 access in this plugin.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace Upscaler::Engine
{
	// Decodes the five anchors in Addresses.h. Pure code-byte reads: no engine object
	// has to exist yet, and no D3D11 call is made. True when all five resolved.
	[[nodiscard]] bool Init();

	[[nodiscard]] bool Resolved() noexcept;

	// The five resolved addresses as module-relative RVAs, for the self-check block.
	// An RVA pastes straight into Ghidra; an absolute address does not.
	[[nodiscard]] std::string HandleSummary();

	// The device DOES have a global, at an address nowhere near the immediate context -
	// an earlier reading of this file concluded there was none because it only looked at
	// the context's neighbours, and settled on ctx->GetDevice instead. That was a COM
	// call, and therefore render-thread only, and it gated measurement M10 behind a
	// render-thread callback that does not exist yet. Reading the global is a plain
	// memory read on any thread, so none of that is needed.
	//
	// Borrowed, never AddRef'd, and read fresh on every call: the engine owns the
	// reference and outlives us.
	[[nodiscard]] ID3D11Device* Device() noexcept;
	[[nodiscard]] ID3D11DeviceContext* ImmediateContext() noexcept;

	// The graphics state is reached through a pointer slot with a fallback slot beside
	// it, exactly as the engine's own accessors do it. The render-target manager and the
	// renderer are objects at fixed addresses, not pointers to them.
	[[nodiscard]] std::uintptr_t GraphicsState() noexcept;
	[[nodiscard]] std::uintptr_t RenderTargetManager() noexcept;
	[[nodiscard]] std::uintptr_t Renderer() noexcept;

	// The renderer's stereo flag byte. Cheap, and the only self-evident proof that the
	// resolved renderer address is the renderer and not a plausible neighbour.
	[[nodiscard]] bool StereoFlag() noexcept;

	// -1 when no colour target is bound, in which case the engine is on its
	// depth-stencil path instead.
	[[nodiscard]] std::int32_t CurrentRenderTargetIndex() noexcept;
	[[nodiscard]] std::int32_t CurrentDepthStencilIndex() noexcept;

	// What a memory-only consistency pass over the three tables found. Every field is
	// an observation, not a requirement: the pass reports and never refuses, so a build
	// whose tables moved produces a diagnosable log rather than a silent decline.
	//
	// It is separate from Init because the render targets do not exist at plugin load.
	// Run it once the game is loaded.
	struct Validation
	{
		bool ran = false;
		std::int32_t remapValid = 0;     // logical ids mapping to a physical slot in range
		std::int32_t propsPopulated = 0;  // properties rows with a non-zero width
		std::int32_t recordsWithSrv = 0;  // physical records carrying a shader resource view
		bool motionVectorsPlausible = false;  // logical 35 resolves, and is a two-channel half float
		bool linearDepthPlausible = false;    // logical 44 resolves, and is a single 32-bit float
		bool stereo = false;
		std::string note;  // one sentence a bug report can be answered from
	};

	[[nodiscard]] Validation Validate() noexcept;

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

	// Guarded typed reads of engine memory, shared with RenderTargets so the SEH
	// bracket has one home. Both answer false and leave the output untouched on a fault
	// or a null address.
	[[nodiscard]] bool ReadU32(std::uintptr_t a_address, std::uint32_t& a_out) noexcept;
	[[nodiscard]] bool ReadI32(std::uintptr_t a_address, std::int32_t& a_out) noexcept;
	[[nodiscard]] bool ReadPtr(std::uintptr_t a_address, void*& a_out) noexcept;
}
