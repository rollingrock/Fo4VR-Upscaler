#pragma once

// All offsets here are RVAs against Fallout4VR.exe 1.2.72 (image base 0x140000000,
// no ASLR), written without the 0x140 prefix.
//
// Verification tags:
//   [LIVE]   byte-verified in the running game (x64dbg, or devbench's `memory` tool,
//            which reads process memory by `base+rva` expression without touching D3D)
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
// Call-only function RVAs used by exactly one translation unit stay in a file-local
// constexpr block next to their typedef; this file is for patch sites and for anything
// used twice.
//
// ---------------------------------------------------------------------------------
// Why the data addresses below are anchors rather than addresses
//
// Nothing here names a data address directly. Each engine global is reached by decoding
// the RIP displacement of an instruction that references it, after verifying that
// instruction's opcode bytes - see Engine::RipResolve. That costs a few bytes of
// checking at load and buys the one property this project needs: a moved or different
// build fails the opcode check and disables a feature, instead of resolving to a
// plausible wrong address that returns plausible wrong values. This binary has already
// produced one such near-miss, which is why the rule exists.

#include <cstddef>
#include <cstdint>

namespace Upscaler::Addr
{
	// --- code (anchor sites) ---

	// BSGraphics::Renderer::ClearColor(renderer). Four of the five engine handles are
	// reachable from this one function, which is why it is the anchor: one opcode check
	// per handle, all inside a body small enough to read in full.
	//
	//   [GHIDRA][LIVE] 1d8dd80  US_BSGraphics_Renderer_ClearColor
	//     1d8dd8f  48 8B 05 32 7D 4A 04   MOV RAX,[rip+0x044a7d32]  -> 6235ac8  state
	//     1d8dda7  48 8B 1D 12 7D 4A 04   MOV RBX,[rip+0x044a7d12]  -> 6235ac0  state alt
	//     1d8ddb5  48 8B 05 F4 7C 4A 04   MOV RAX,[rip+0x044a7cf4]  -> 6235ab0  ctx
	//     1d8ddbc  48 8D 0D 4D E2 B1 01   LEA RCX,[rip+0x01b1e24d]  -> 38ac010  rtm
	//     1d8ddc3  48 8B 38               MOV RDI,[RAX]             -> vtable
	//     1d8ddeb  48 8B 87 90 01 00 00   MOV RAX,[RDI+0x190]       -> slot 50
	//     1d8de01  48 FF E0               JMP RAX
	//
	// Slot 0x190/8 = 50 of ID3D11DeviceContext is ClearRenderTargetView(pRTV, ColorRGBA)
	// and the call passes exactly (this, RTV, float4). That types 6235ab0 outright,
	// without relying on any symbol.
	constexpr std::uintptr_t kClearColorFn = 0x1d8dd80;

	// The four anchors above. Every one is REX.W + opcode + modrm(disp32), so the
	// displacement is at byte 3 and the instruction is 7 bytes; the modrm byte differs
	// per destination register, which is what makes the three-byte opcode check
	// discriminating rather than decorative.
	constexpr std::uintptr_t kGraphicsStateAnchor = 0x1d8dd8f;        // 48 8B 05 -> qword* to BSGraphics state
	constexpr std::uintptr_t kGraphicsStateAltAnchor = 0x1d8dda7;     // 48 8B 1D -> qword* to the fallback state
	constexpr std::uintptr_t kImmediateContextAnchor = 0x1d8ddb5;     // 48 8B 05 -> qword* to ID3D11DeviceContext
	constexpr std::uintptr_t kRenderTargetManagerAnchor = 0x1d8ddbc;  // 48 8D 0D -> the manager object itself

	// [GHIDRA][LIVE] 1db627e  48 8D 0D BB 30 48 04   LEA RCX,[rip+0x044830bb] -> 6239340
	// The renderer instance - an object at a fixed address, not a pointer to one. This
	// is one of 1274 references to it; this one was taken because it is a plain LEA into
	// RCX with no addressing arithmetic to misread. Confirmed live: the object's byte +1
	// is the stereo flag and reads 1 in this VR build.
	constexpr std::uintptr_t kRendererAnchor = 0x1db627e;

	// [GHIDRA] 1d9967a  48 8B 0D 6F A6 35 04   MOV RCX,[rip+0x0435a66f] -> 1460f3cf0
	//
	// An ID3D11Device* global, and the engine's own device: the very next instructions
	// dereference it to a vtable and call slots 5, 9 and 7 - CreateTexture2D,
	// CreateRenderTargetView, CreateShaderResourceView - to build a render target.
	//
	// This supersedes an earlier conclusion. A search of the addresses immediately around
	// the immediate-context slot found no device and the design settled on
	// ctx->GetDevice, which is a COM call and therefore render-thread only. The global is
	// simply somewhere else. Reading it is a plain memory read on any thread, so the
	// device - and with it the feature level, measurement M10 - is available at plugin
	// load with no COM call at all.
	constexpr std::uintptr_t kD3D11DeviceAnchor = 0x1d9967a;

	constexpr std::size_t kRipDispOffset = 3;
	constexpr std::size_t kRipInstrLen = 7;

	// --- BSGraphics::RenderTargetManager layout ---
	//
	// [LIVE] The manager's first member is the render-target properties array, which is
	// why the accessor call sites LEA the manager into RCX before calling
	// QCurrentRenderTargetWidth: that is a this pointer, not an array base. The whole
	// object then reads coherently, and every offset below was walked in a running game.
	//
	//   +0x0000  RTProperties[144]     0x20 each  -> ends 0x1200
	//   +0x1200  DSProperties[17]      0x18 each  -> ends 0x1398
	//   +0x13bc  logicalToPhysical[]   int32 each
	constexpr std::uintptr_t kRtmProps = 0x0000;
	constexpr std::uintptr_t kRtmPropsStride = 0x20;
	constexpr std::uintptr_t kPropWidth = 0x00;   // uint32
	constexpr std::uintptr_t kPropHeight = 0x04;  // uint32
	constexpr std::uintptr_t kPropFormat = 0x08;  // uint32, a DXGI_FORMAT

	constexpr std::uintptr_t kRtmDsProps = 0x1200;
	constexpr std::uintptr_t kRtmDsPropsStride = 0x18;
	constexpr std::uintptr_t kDsPropArraySize = 0x08;  // uint32; NOT a DXGI_FORMAT

	// [LIVE] The remap is real and it is not the identity: mv 35 -> 32, ldepth 44 -> 43,
	// taa 61 -> 29, submitted 4 -> 6. An entry of -1 means the logical id is unused.
	constexpr std::uintptr_t kRtmLogicalToPhys = 0x13bc;  // int32 each

	// [GHIDRA] InitializeTargets @ 1dbb640, and CMP EBX,0x90 in FUN_140b01140.
	// CommonLibF4's lengths are flat-Fallout-4 values and are wrong here.
	constexpr std::int32_t kRenderTargetCount = 144;
	constexpr std::int32_t kDepthStencilCount = 17;

	// --- BSGraphics::Renderer layout ---
	//
	// [GHIDRA][LIVE] The physical render-target record.
	//
	// ⚠️ CORRECTED. An earlier reading of this file put the base at 0xa70 and every field
	// 8 bytes low, which made kRtRecTexture point at the NEXT target's texture. The RTV
	// and SRV happened to land correctly anyway - both were read as absolute addresses
	// off ClearColor - so the mistake did not show up in any log line, which is exactly
	// how a wrong offset survives. What settles it is the creation function rather than
	// the consumer, because the creation function names the fields by what it stores.
	//
	// US_BSGraphics_CreateRenderTarget @ 1d99560, at 1d99668..1d996cf:
	//
	//   1d99668  4C 8D 2C 7F              LEA  R13,[RDI+RDI*2]        RDI = physical index
	//   1d9966c  49 C1 E5 04              SHL  R13,4                  R13 = 0x30 * phys
	//   1d99670  4D 03 EF                 ADD  R13,R15                R15 = the renderer
	//   1d99673  4D 8D 8D 68 0A 00 00     LEA  R9,[R13+0xa68]         out param
	//   1d9968b  FF 50 28                 CALL [RAX+0x28]             device slot 5, CreateTexture2D
	//   1d99698  4D 8D 8D 78 0A 00 00     LEA  R9,[R13+0xa78]         out param
	//   1d996a9  FF 50 48                 CALL [RAX+0x48]             device slot 9, CreateRenderTargetView
	//   1d996b6  4C 8D 4F 38 ...          R9 = renderer + (phys+0x38)*0x30, which folds to
	//                                     renderer + phys*0x30 + 0xa80, i.e. record + 0x18
	//   1d996cf  FF 50 38                 CALL [RAX+0x38]             device slot 7, CreateShaderResourceView
	//
	// So the base is 0xa68 and the stride is 0x30. The remaining fields were typed live by
	// dereferencing each across several different targets and comparing the COM vtable
	// pointer - same field, same vtable, every time, all five inside d3d11.dll. The counts
	// are how many of the 144 slots carry a non-null value, and are what distinguishes the
	// texture from its copy.
	constexpr std::uintptr_t kRendererRtTable = 0xa68;
	constexpr std::uintptr_t kRendererRtStride = 0x30;
	constexpr std::uintptr_t kRtRecTexture = 0x00;      // ID3D11Texture2D*,           117 of 144
	constexpr std::uintptr_t kRtRecCopyTexture = 0x08;  // ID3D11Texture2D*,             2 of 144
	constexpr std::uintptr_t kRtRecRtv = 0x10;          // ID3D11RenderTargetView*,    123 of 144
	constexpr std::uintptr_t kRtRecSrv = 0x18;          // ID3D11ShaderResourceView*,  123 of 144
	constexpr std::uintptr_t kRtRecCopySrv = 0x20;      // ID3D11ShaderResourceView*,    2 of 144
	constexpr std::uintptr_t kRtRecUav = 0x28;          // ID3D11UnorderedAccessView*,  33 of 144

	// ⚠️ NOT TRUSTED. These came from the same scanning method that put the colour record
	// base 8 bytes low, and an independent Ghidra reading of the depth-stencil creation
	// path disagrees with the base outright - it puts the array at +0x2598, not +0x2670.
	// That disagreement was never adjudicated, so nothing here is believed and
	// RenderTargets::LookupDepth declines rather than hand out possibly-shifted pointers.
	//
	// The colour record was settled by reading the CREATION function, which names each
	// field by what it stores into it, rather than by pattern-matching live memory. Do
	// the same here: find the ID3D11Device::CreateDepthStencilView call sites - device
	// vtable slot 10, so CALL [RAX+0x50] - and read the out-param offsets. Until then
	// these constants exist only so the shape of the eventual fix is visible.
	//
	// What was observed live, for whoever picks this up: a perfect 0x98 period of heap
	// pointers from +0x2670, with fields at +0x08, +0x28, +0x48, +0x50, +0x58, +0x60 and
	// +0x80 relative to that base, typed by vtable as DSV, DSV, SRV, SRV, Texture2D, DSV,
	// DSV. Shifted 8 low - the colour record's mistake - that would read +0x10, +0x30,
	// +0x50, +0x58, +0x60, +0x68, +0x88 off a base of +0x2668.
	constexpr std::uintptr_t kRendererDsTable = 0x2670;
	constexpr std::uintptr_t kRendererDsStride = 0x98;
	constexpr std::uintptr_t kDsRecDsv = 0x08;      // ID3D11DepthStencilView*
	constexpr std::uintptr_t kDsRecSrv = 0x48;      // ID3D11ShaderResourceView* - the depth input
	constexpr std::uintptr_t kDsRecSrvAlt = 0x50;   // ID3D11ShaderResourceView*
	constexpr std::uintptr_t kDsRecTexture = 0x58;  // ID3D11Texture2D*

	// [LIVE] The renderer's leading flag bytes. Byte +1 reads 1 in this VR build, which
	// is the cheapest available proof that the resolved address really is the renderer
	// and not a plausible neighbour.
	constexpr std::uintptr_t kRendererStereoFlag = 0x01;
	constexpr std::uintptr_t kRendererAltRenderFlag = 0x02;

	// --- BSGraphics state layout ---
	//
	// [GHIDRA] Both from ClearColor and from QCurrentRenderTargetWidth/Height: a
	// negative current render-target index means none is bound and the depth-stencil
	// path is taken instead.
	constexpr std::uintptr_t kStateCachedBlock = 0x1ee0;     // the cached D3D state block
	constexpr std::uintptr_t kStateCurrentRtIndex = 0x1f64;  // int32
	constexpr std::uintptr_t kStateCurrentDsIndex = 0x1f68;  // int32

	// --- render target indices (logical, via the RenderTargetManager remap table) ---
	//
	// [GHIDRA] The engine's own name-to-index table, from the gbuffer console handler at
	// 0x140505982 with its string table at 0x142cd28c0; the help text at 0x142cd2850
	// reads "gbuffer mv(Motion Vectors)". Every pair was read as an instruction, and
	// every row cross-checks the live format table.
	constexpr std::int32_t kRT_BackBuffer = 0;  // the DXGI swap-chain buffer
	constexpr std::int32_t kRT_Submitted = 4;   // the texture handed to IVRCompositor::Submit
	constexpr std::int32_t kRT_SsrColor = 7;
	constexpr std::int32_t kRT_Albedo = 28;
	constexpr std::int32_t kRT_Normal = 29;
	constexpr std::int32_t kRT_EnvMap = 32;
	constexpr std::int32_t kRT_Ambient = 33;
	constexpr std::int32_t kRT_Emit = 34;
	constexpr std::int32_t kRT_MotionVectors = 35;
	constexpr std::int32_t kRT_Diffuse = 36;
	constexpr std::int32_t kRT_Specular = 37;
	constexpr std::int32_t kRT_AmbientOcclusion = 40;
	constexpr std::int32_t kRT_LinearDepth = 44;
	constexpr std::int32_t kRT_Taa = 61;

	// [LIVE] The formats these two carry. Used as the self-check's consistency assertion
	// rather than as a fact anything depends on: if motion vectors stop being a
	// two-channel half-float, the three tables have been misread and the plugin should
	// say so rather than proceed.
	constexpr std::uint32_t kFormatR16G16Float = 34;  // DXGI_FORMAT_R16G16_FLOAT
	constexpr std::uint32_t kFormatR32Float = 41;     // DXGI_FORMAT_R32_FLOAT
}
