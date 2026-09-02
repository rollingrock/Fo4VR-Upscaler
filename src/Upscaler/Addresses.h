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

	// [GHIDRA] 28a72b0 US_ImageSpaceEffectTemporalAA_Render. The concrete native TAA
	// effect virtual chooses the shader variant, updates its parameters and invokes the
	// engine's image-space draw path. Its first instruction is exactly five relocatable
	// bytes (48 89 5C 24 08, MOV [RSP+8],RBX), making this a fail-soft observation
	// boundary that can call the complete original body through a gateway.
	constexpr std::uintptr_t kTemporalAARenderFn = 0x28a72b0;

	// [GHIDRA] 27afee3 in ImageSpaceManager::RenderEffectRange directly calls
	// US_ImageSpaceManager_DispatchEffectObject after loading the current effect,
	// source logical RT and destination logical RT. Bytes: E8 B8 02 00 00. The
	// shared call is filtered against manager effect slot 3 (ImageSpaceEffectHDR),
	// giving a read-only measurement of the pre-tonemap handoff.
	constexpr std::uintptr_t kImageSpaceEffectDispatchCall = 0x27afee3;

	// [GHIDRA] 290c590 US_ImageSpaceEffectDynamicResolution_IsActive. The concrete
	// effect returns false only when both live ratios equal 1.0. Its first instruction
	// is an eight-byte RIP-relative read (F3 0F 10 0D A8 30 37 00), which cannot be
	// copied into the generic gateway without relocation. The hook therefore replaces
	// the complete tiny predicate at entry and mirrors it exactly; it never calls or
	// resumes the original body.
	constexpr std::uintptr_t kDynamicResolutionIsActiveFn = 0x290c590;

	// [GHIDRA] US_ImageSpaceEffectDynamicResolution_Construct @ 290c300 installs the
	// slot-21 vtable and then opts the effect out of dynamic screen geometry:
	//   290c30e  48 8D 05 73 28 7B 00  LEA RAX,[rip+disp32] -> 30beb88 vtable
	//   290c315  48 89 03              MOV [RBX],RAX
	//   290c318  C6 83 A8 00 00 00 00 MOV byte ptr [RBX+0xA8],0
	// The quality transaction resolves the vtable through this verified LEA, validates
	// its render and IsActive entries, and changes only +0xA8 around the accepted frame.
	//
	// [GHIDRA] The vtable itself lives in .rdata at 30beb88. Its entries were read out of
	// Fallout4VR.exe on 2026-09-02 (PE section walk; absolute VA minus image base):
	//   30beb88+0x08  -> 290c5c0  US_ImageSpaceEffectDynamicResolution_Render
	//   30beb88+0x40  -> 290c590  US_ImageSpaceEffectDynamicResolution_IsActive (above)
	// The vtable is labelled US_ImageSpaceEffectDynamicResolution_vtable in fallout4vr_pdb.
	// Hooks checks exactly those two slots against these two RVAs before it touches
	// +0xA8, so the slot offsets are derived here rather than assumed there.
	constexpr std::uintptr_t kDynamicResolutionEffectVtableAnchor = 0x290c30e;
	constexpr std::uintptr_t kDynamicResolutionEffectRenderFn = 0x290c5c0;
	constexpr std::size_t kDynamicResolutionEffectVtableRenderSlot = 0x08;
	constexpr std::size_t kDynamicResolutionEffectVtableIsActiveSlot = 0x40;

	// [GHIDRA] 2908fd7 in US_BSImagespaceShader_DrawPixelPass is the one direct call
	// to the prepared BSRenderPass draw routine. Output selection and pass preparation
	// precede it; virtual shader cleanup follows it. Bytes: E8 64 80 F8 FF. A call-site
	// thunk can therefore observe the still-bound OM/PS state immediately after the
	// vanilla draw, gated by the TemporalAA entry's thread-local scope.
	constexpr std::uintptr_t kImageSpacePixelDrawCall = 0x2908fd7;

	// [GHIDRA] 28a7365 in US_ImageSpaceEffectTemporalAA_Render reads
	// US_g_TAAHistoryParity through `8B 35 51 B7 CF 03` (MOV ESI,[rip+disp32]).
	// US_ImageSpaceEffectTemporalAA_InitInputs proves the corresponding resource
	// contract: parity 0 consumes logical 61 and outputs logical 62; parity 1
	// consumes 62 and outputs 61. The bypass diagnostic resolves the data address
	// from this verified instruction and never hardcodes the global itself.
	constexpr std::uintptr_t kTaaHistoryParityAnchor = 0x28a7365;

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

	// [GHIDRA] 28419c8 in the main renderer, immediately before the direct call to
	// US_BSGraphicsState_UpdateTAAJitter:
	//   28419c8  48 8D 0D E1 10 D6 03   LEA RCX,[rip+0x03d610e1] -> 65a2ab0
	// This is the jitter-state object itself, not a pointer slot. The update routine
	// independently types every field below and masks the stored phase with 7.
	constexpr std::uintptr_t kJitterStateAnchor = 0x28419c8;

	// [GHIDRA][LIVE] US_ImageSpaceEffectDynamicResolution_IsActive @ 290c590.
	// The effect returns active iff either live ratio differs from 1.0:
	//   290c598  F3 0F 10 05 D8 10 FA 00  MOVSS XMM0,[rip+disp32] -> 38ad678 height
	//   290c5a5  F3 0F 10 05 C7 10 FA 00  MOVSS XMM0,[rip+disp32] -> 38ad674 width
	// Both are read anchors; DynamicResolution decodes them and never hardcodes the
	// high-data addresses printed by Ghidra.
	constexpr std::uintptr_t kDynamicResolutionHeightRatioAnchor = 0x290c598;
	constexpr std::uintptr_t kDynamicResolutionWidthRatioAnchor = 0x290c5a5;

	// [GHIDRA][LIVE] US_BSGraphics_GetUseHiddenAreaMesh @ 1d979e0 is the complete
	// accessor for the VR hidden-area-mesh setting:
	//   1d979e0  0F B6 05 19 41 AF 01  MOVZX EAX,byte ptr [rip+disp32] -> 388bb00
	// US_BSGraphics_DrawHiddenAreaMesh @ 1d97650 tests the same byte before binding
	// and drawing the mask geometry. The reduced-input controller decodes the byte
	// from this accessor and never hardcodes the data address.
	constexpr std::uintptr_t kHiddenAreaMeshEnabledAnchor = 0x1d979e0;

	// [GHIDRA] ImageSpaceManager's shared effect dispatcher chooses its static screen
	// triangle at manager+0x28 unless both the effect opts into dynamic geometry and this
	// byte is non-zero, in which case it selects manager+0x38. The static triangle does
	// not cover FO4VR's complete eye targets, so the quality handback must preserve this
	// selector and refresh the dynamic geometry after changing the live ratios:
	//   27b0283  80 BE A8 00 00 00 00  CMP byte ptr [RSI+0xA8],0
	//   27b028c  80 3D 00 D4 0F 01 00  CMP byte ptr [rip+disp32],0 -> 38ad693
	// Decode the byte from the verified instruction rather than hardcoding its data
	// address, just like the four float anchors above.
	constexpr std::uintptr_t kDynamicResolutionImageSelectorAnchor = 0x27b028c;

	// [GHIDRA] 284ebd0 in TS_DrawWorld_RenderSceneStages directly calls
	// US_ImageSpaceManager_UpdateDynamicScreenGeometry(manager). Bytes:
	// E8 AB 1D F6 FF. The callee rewrites manager+0x38's BSDynamicTriShape from
	// the two live ratios. Hooking this call gives the quality handback bracket a
	// byte-verified original it can invoke again after temporarily changing those
	// ratios; otherwise the geometry keeps the reduced values cached earlier in the
	// frame and either crops the reconstructed image or leaves triangular holes.
	constexpr std::uintptr_t kDynamicResolutionGeometryUpdateCall = 0x284ebd0;

	// [GHIDRA] US_RenderWorldImageSpaceSequence @ 284e370 invokes the complete late
	// ImageSpace range (slots 16..22) here with bytes E8 D4 18 F6 FF. Wrapping the call
	// gives the quality transaction a structural cleanup seam after slot 21's dispatcher
	// and the manager's outer source/destination bookkeeping. Slot 22 (Bokeh DoF) is
	// verified inactive in this walk; the pre-world updater remains the stale backstop.
	constexpr std::uintptr_t kLateImageSpaceRangeCall = 0x284e497;

	// [GHIDRA][LIVE] US_ConsoleCommand_DynamicResolution @ 515fc0. Its `width` and
	// `height` subcommands convert the console double to float and store the requested
	// target ratios through these exact instructions:
	//   516106  F3 0F 11 0D 6E 75 39 03  MOVSS [rip+disp32],XMM1 -> 38ad67c width
	//   516164  F3 0F 11 0D 14 75 39 03  MOVSS [rip+disp32],XMM1 -> 38ad680 height
	constexpr std::uintptr_t kDynamicResolutionTargetWidthAnchor = 0x0516106;
	constexpr std::uintptr_t kDynamicResolutionTargetHeightAnchor = 0x0516164;

	constexpr std::size_t kRipDispOffset = 3;
	constexpr std::size_t kRipInstrLen = 7;
	constexpr std::size_t kRipFloatDispOffset = 4;
	constexpr std::size_t kRipFloatInstrLen = 8;

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

	// [GHIDRA] US_BSGraphics_DefineDepthStencilTarget @ 1db96e0 writes the
	// physical index returned by CreateDepthStencilTarget to this table at
	// 1db97a4. TS_RTManager_SelectDepthStencil @ 1db9e40 reads it at 1db9e64.
	// Like colour, depth properties are logical-indexed and resources are
	// physical-indexed; this table is the only bridge between them.
	constexpr std::uintptr_t kRtmDsLogicalToPhys = 0x15fc;  // int32 each

	// [GHIDRA] InitializeTargets @ 1dbb640, and CMP EBX,0x90 in FUN_140b01140.
	// CommonLibF4's lengths are flat-Fallout-4 values and are wrong here.
	constexpr std::int32_t kRenderTargetCount = 144;  // logical ids
	constexpr std::int32_t kDepthStencilCount = 17;   // logical ids

	// [GHIDRA] The physical arrays are one record longer than their logical-id
	// tables, matching the flat engine's layout. Their adjacent bases force the
	// capacities exactly: (0x2598-0xa68)/0x30 = 145 colour records, and
	// (0x3048-0x2598)/0x98 = 18 depth records. DestroyCubeMapRenderTarget @
	// 1d9a010 independently addresses the next array at renderer+0x3048.
	constexpr std::int32_t kPhysicalRenderTargetCount = 145;
	constexpr std::int32_t kPhysicalDepthStencilCount = 18;

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

	// [GHIDRA] The physical depth-stencil record, adjudicated from the complete
	// creation function rather than from live pointer-pattern scanning.
	//
	// US bookmark at BSGraphics::Renderer::CreateDepthStencilTarget @ 1d99880:
	//   1d999c4  48 69 C0 98 00 00 00   IMUL RAX,RAX,0x98       phys stride
	//   1d999d2  4C 8D B8 98 25 00 00   LEA  R15,[RAX+0x2598]   record base
	//   1d999e6  CALL [RAX+0x28]         device slot 5           Texture2D -> +00
	//   1d99ac5  CALL [RAX+0x50]         device slot 10          writable DSV -> +08
	//   1d99ae5  CALL [RAX+0x50]         device slot 10          RO-depth DSV -> +28
	//   1d99b0b  CALL [RAX+0x50]         device slot 10          RO-stencil DSV -> +48
	//   1d99b28  CALL [R10+0x50]         device slot 10          RO-both DSV -> +68
	//   1d99ba2  CALL [RAX+0x38]         device slot 7           depth SRV -> +88
	//   1d99bcc  CALL [R10+0x38]         device slot 7           stencil SRV -> +90
	//
	// Each DSV field above is an array of four pointers, one per texture-array
	// slice. DestroyDepthStencilTarget @ 1d99ef0 releases exactly these fields,
	// independently confirming the creation-side layout.
	constexpr std::uintptr_t kRendererDsTable = 0x2598;
	constexpr std::uintptr_t kRendererDsStride = 0x98;
	constexpr std::uintptr_t kDsRecTexture = 0x00;             // ID3D11Texture2D*
	constexpr std::uintptr_t kDsRecDsv = 0x08;                 // ID3D11DepthStencilView*[4]
	constexpr std::uintptr_t kDsRecDsvReadOnlyDepth = 0x28;    // ID3D11DepthStencilView*[4]
	constexpr std::uintptr_t kDsRecDsvReadOnlyStencil = 0x48;  // ID3D11DepthStencilView*[4]
	constexpr std::uintptr_t kDsRecDsvReadOnlyBoth = 0x68;     // ID3D11DepthStencilView*[4]
	constexpr std::uintptr_t kDsRecSrv = 0x88;                 // ID3D11ShaderResourceView*, depth plane
	constexpr std::uintptr_t kDsRecSrvStencil = 0x90;          // ID3D11ShaderResourceView*, stencil plane

	// [LIVE] The renderer's leading flag bytes. Byte +1 reads 1 in this VR build, which
	// is the cheapest available proof that the resolved address really is the renderer
	// and not a plausible neighbour.
	constexpr std::uintptr_t kRendererStereoFlag = 0x01;
	constexpr std::uintptr_t kRendererAltRenderFlag = 0x02;

	// [GHIDRA] US_BSGraphics_IsScopedPass @ 1d947d0 is exactly
	// `MOVZX EAX,byte ptr [RCX+4]; RET`. US_RenderWorldImageSpaceSequence passes
	// the fixed renderer object and selects color 1 / DS1 when false versus scope
	// color 0x61 / DS 0xC when true.
	constexpr std::uintptr_t kRendererScopedPassFlag = 0x04;

	// --- BSGraphics state layout ---
	//
	// [GHIDRA] Both from ClearColor and from QCurrentRenderTargetWidth/Height: a
	// negative current render-target index means none is bound and the depth-stencil
	// path is taken instead.
	constexpr std::uintptr_t kStateCachedBlock = 0x1ee0;     // the cached D3D state block
	constexpr std::uintptr_t kStateCurrentRtIndex = 0x1f64;  // int32
	constexpr std::uintptr_t kStateCurrentDsIndex = 0x1f68;  // int32

	// [GHIDRA][LIVE] US_BSGraphics_UploadGlobalCameraConstants @ 1d93910 first
	// selects State+0x1ee0, then dereferences +0x6f0. That is the active eye-0
	// ViewData staging pointer at State+0x25d0; eye 1 follows at the verified 0x210
	// stride. At 1d93a7c it selects each +0x90 projection matrix; later reads
	// +0x110/+0x190 into the exact current/previous unjittered CB12 matrices used
	// by the deferred motion-vector shader. The +0x90 finding is recorded with a
	// [US_] comment in the annotated program.
	constexpr std::uintptr_t kStateCameraViewData = 0x25d0;  // ViewData*
	constexpr std::uintptr_t kViewDataStride = 0x210;
	constexpr std::uintptr_t kViewDataProjection = 0x90;           // float4x4
	constexpr std::uintptr_t kViewDataCurrentUnjittered = 0x110;   // float4x4
	constexpr std::uintptr_t kViewDataPreviousUnjittered = 0x190;  // float4x4

	// --- native TAA jitter state layout ---
	//
	// [GHIDRA] US_BSGraphicsState_UpdateTAAJitter @ 1daa1b0 and
	// US_BSGraphics_BuildStereoCameraViewData @ 1daa370. Width and height are per-eye:
	// US_BSGraphics_RegisterRenderTargetDefinitions loads +0x78 then doubles it only
	// while defining double-wide resources.
	constexpr std::uintptr_t kJitterPhase = 0x00;         // uint32, advances modulo 8
	constexpr std::uintptr_t kJitterProjectionX = 0x04;   // float, scene translation
	constexpr std::uintptr_t kJitterProjectionY = 0x08;   // float, scene translation
	constexpr std::uintptr_t kJitterPerEyeWidth = 0x78;   // uint32
	constexpr std::uintptr_t kJitterPerEyeHeight = 0x7c;  // uint32
	constexpr std::uintptr_t kJitterEnabled = 0xa8;       // uint32, nonzero enables
	constexpr std::uintptr_t kJitterDisableCount = 0xac;  // uint32, zero permits update

	// --- render target indices (logical, via the RenderTargetManager remap table) ---
	//
	// [GHIDRA] The engine's own name-to-index table, from the gbuffer console handler at
	// 0x140505982 with its string table at 0x142cd28c0; the help text at 0x142cd2850
	// reads "gbuffer mv(Motion Vectors)". Every pair was read as an instruction, and
	// every row cross-checks the live format table.
	constexpr std::int32_t kRT_BackBuffer = 0;  // the DXGI swap-chain buffer
	// [GHIDRA][LIVE] ImageSpaceEffectHDR receives logical 1 as its source and logical
	// 3 as its output in the normal VR world path. Logical 1 is therefore the exact
	// pre-tonemap color handoff, regardless of the generic table's display name.
	constexpr std::int32_t kRT_PreTonemapColor = 1;
	constexpr std::int32_t kRT_PostTonemapColor = 3;
	constexpr std::int32_t kRT_Submitted = 4;  // the texture handed to IVRCompositor::Submit
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
	constexpr std::int32_t kRT_TaaAlt = 62;

	// [GHIDRA][LIVE] Native TemporalAA and the world G-buffer both consume logical
	// depth-stencil 1. RegisterRenderTargetDefinitions gives it needsStencil=1 and
	// selector=0; CreateDepthStencilTarget maps that tuple to the exact formats below.
	constexpr std::int32_t kDS_MainScene = 1;

	// [LIVE] The formats these two carry. Used as the self-check's consistency assertion
	// rather than as a fact anything depends on: if motion vectors stop being a
	// two-channel half-float, the three tables have been misread and the plugin should
	// say so rather than proceed.
	constexpr std::uint32_t kFormatR16G16Float = 34;         // DXGI_FORMAT_R16G16_FLOAT
	constexpr std::uint32_t kFormatR32Float = 41;            // DXGI_FORMAT_R32_FLOAT
	constexpr std::uint32_t kFormatR24G8Typeless = 44;       // DXGI_FORMAT_R24G8_TYPELESS
	constexpr std::uint32_t kFormatD24UnormS8Uint = 45;      // DXGI_FORMAT_D24_UNORM_S8_UINT
	constexpr std::uint32_t kFormatR24UnormX8Typeless = 46;  // DXGI_FORMAT_R24_UNORM_X8_TYPELESS
}
