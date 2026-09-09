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

	// [GHIDRA] The four screen-space-reflection (SSLR) pixel passes, each at its own
	// direct call to the SAME prepared-draw routine the ImageSpace seam above targets.
	//
	// Why they need their own sites at all: the shared draw at 2908fd7 lives inside
	// US_BSImagespaceShader_DrawPixelPass @ 2908e30, which is installed in vtable slot 1
	// of the third base of ~145 ImagespaceShader classes - BSImagespaceShaderTemporalAA's
	// copy at 30aca58+8 holds 2908e30 - but ALL FOUR SSLR shaders override that slot:
	//   30b3dd8+8 -> 27db250  US_BSImagespaceShaderSSLRRaytracing_DrawPixelPass
	//   30b3f98+8 -> 27db200  US_BSImagespaceShaderSSLRPrepass_DrawPixelPass
	//   30b3b08+8 -> 27db100  US_BSImagespaceShaderSSLRBlurH_DrawPixelPass
	//   30b3c88+8 -> 27db180  US_BSImagespaceShaderSSLRBlurV_DrawPixelPass
	// So a census taken at the shared seam is silent about the reflection chain by
	// construction, not by measurement, and that silence says nothing about whether the
	// chain ran. Each override stages its own render target through the manager at
	// 38ac010 and then issues one five-byte E8 to 2891040, the same shared executor:
	//   27db26d  48 8D 0D ...          LEA RCX,[38ac010]   render-target manager
	//   27db278  41 8D 42 07           LEA R8D,[RDX+7]     logical colour 7 (ssrcolor)
	//   27db274  41 8D 4A 03           LEA R9D,[RDX+3]     mode 3
	//   27db27c  E8 ...                stage slot 0
	//   27db296  E8 ...                stage slot 1 = -1 (unbind)
	//   27db2a2  E8 ...                apply staged targets
	//   27db2c5  E8 76 5D 0B 00        CALL 2891040        <- the draw
	// BlurH stages logical 8 (27db128 LEA R8D,[RDX+8], draw at 27db15b, E8 E0 5E 0B 00),
	// BlurV logical 9 (27db1a8 LEA R8D,[RDX+9], draw at 27db1db, E8 60 5E 0B 00), and the
	// prepass stages nothing of its own (draw at 27db22a, E8 11 5E 0B 00).
	//
	// Observation sites only: each thunk calls the original and records, exactly as the
	// ImageSpace pixel-draw probe does.
	constexpr std::uintptr_t kSslrBlurHDrawCall = 0x27db15b;
	constexpr std::uintptr_t kSslrBlurVDrawCall = 0x27db1db;
	constexpr std::uintptr_t kSslrPrepassDrawCall = 0x27db22a;
	constexpr std::uintptr_t kSslrRaytracingDrawCall = 0x27db2c5;

	// [GHIDRA] The water surface's own shader, and the two entries at which it writes the
	// water pixel constants. Found from the binary's own RTTI rather than from a call
	// site: the complete object locator at 3291b90 (signature 1, offset 0, cdOffset 0,
	// type descriptor 3926380, whose name string at 3926390 is ".?AVBSWaterShader@@") is
	// pointed at by the qword at 30bae90, so BSWaterShader's vtable begins at 30bae98 and
	// its slots 3 and 4 are 28a0270 and 289cc50. Each of those two pointers appears in
	// exactly one vtable image-wide, so neither entry is shared with another shader.
	//
	// This is the site the 2026-09-06 census could not find. The water SURFACE draw is
	// world geometry reached through queued render-pass lambdas that the tool never
	// recovered as functions, so there is no call site to wrap; the shader's own
	// constant-setup entries are reachable and are what a census needs, because the
	// constants are what the question is about.
	//
	//   30bae98 +0x18 -> 28a0270   slot 3, four pointer arguments, returns void
	//   30bae98 +0x20 -> 289cc50   slot 4, (this, uint32 technique, pass*), returns bool
	//
	// Both are entry sites and both preserved prefixes are complete relocatable
	// instructions with no RIP-relative operand:
	//   28a0270  48 8B C4              MOV RAX,RSP
	//   28a0273  48 89 48 08           MOV [RAX+8],RCX      (7 bytes preserved)
	//   289cc50  48 89 5C 24 18        MOV [RSP+0x18],RBX   (5 bytes preserved)
	constexpr std::uintptr_t kWaterShaderSetupGeometryFn = 0x28a0270;
	constexpr std::size_t kWaterShaderSetupGeometryPrefixBytes = 7;
	constexpr std::uintptr_t kWaterShaderSetupTechniqueFn = 0x289cc50;
	constexpr std::size_t kWaterShaderSetupTechniquePrefixBytes = 5;

	// [GHIDRA] Slot 3's third argument is the shader state and its fourth the view state.
	// The shader state holds the pixel-shader record at +0x78 and three pixel constant
	// groups at +0x150, +0x158 and +0x160, each a small object whose +0x10 is the mapped
	// float array the engine writes constants into. A constant's position in that array
	// is a byte in the shader record, 4 times the float4 register, and 0xFF means the
	// compiled shader does not use it.
	//
	// The screen / dynamic-resolution constant goes into the +0x150 group, written at
	// 28a038a..28a0407 when psRecord+0x6f is not 0xFF:
	//   28a038a  0F B6 47 6F           MOVZX EAX,[RDI+0x6f]     the register byte
	//   28a0392  49 8B 48 10           MOV RCX,[R8+0x10]        R8 = shaderState+0x150
	//   28a0399  48 63 46 1C           MOVSXD RAX,[RSI+0x1c]    the current target id
	//   28a03b4  4C 8D 0D 55 BC 00 01  LEA R9,[38ac010]         render-target manager
	//   28a03bb  42 8B 04 08           MOV EAX,[RAX+R9]         that target's width
	//   28a03a8  F3 0F 5E 15 ...       DIVSS XMM2,[38ad674]     1 / live width ratio
	//   28a03f4  F3 0F 5E 05 ...       DIVSS XMM0,[38ad678]     1 / live height ratio
	// so the float4 is (1/targetWidth, 1/targetHeight, 1/liveWidthRatio,
	// 1/liveHeightRatio). Slot 4 writes the same four floats into the register named by
	// the SAME byte psRecord+0x6f, at 289d013..289d094, reaching the extents by CALLING
	// the manager's width and height accessors 1db9b90 / 1db9bf0 with that same props
	// table as the RECEIVER - 289d031 and 289d03d each LEA RCX,[38ac010] before their
	// call - rather than indexing the table inline the way slot 3 does. Same table, same
	// two ratio globals at 289d06a / 289d072; only the route to the extents differs. (An
	// earlier wording here said "rather than the props table", which read as though slot 4
	// bypassed it. It does not.)
	//
	// Slot 4 is INSTALLED AND HAS NEVER RUN in any measurement this project has taken:
	// every /state read and every water run at the Sanctuary river reports
	// setupTechniqueCalls 0 while setupGeometryCalls stands in the hundreds of thousands
	// (artifacts/phase5-step9b-fix-waterpair3-20260907-1231 in the research repo: 0
	// against 239,725 for the session). It is hooked so that a pose which does reach it is
	// seen,
	// not because it carries any reading here.
	//
	// The engine's own name for that constant is not recoverable from the string table.
	// BSWaterShaderPixelConstants::GetString's literals survive at 30c1b68..30c1cc8
	// (TextureProj, ShallowColor, DeepColor, WorldSpaceDeepColor, HorizonSkyColor,
	// LowerSkyColor, UpperSkyColor, LightSiltColor, DarkSiltColor, PosAdjust,
	// ReflectPlane, ProjData, VarAmounts, SunDir, SunColor, NumLights, LightPos,
	// WaterParams, WaterParams2, WaterParams3, WaterParams4, NormalsAmplitude), and five
	// register bytes can be matched to that order by what slot 3 writes through them - a
	// per-eye plane at 0x62 (ReflectPlane, position 10), the negated sun direction rotated
	// by the view at 0x65 (SunDir, 13), a 2.2-power colour times an intensity at 0x66
	// (SunColor, 14), the clamped light count at 0x67 (NumLights, 15) and the per-light
	// position array at 0x68 (LightPos, 16), which puts position 0 at byte 0x58. That
	// correspondence is INFERRED from the writes, and it stops at 0x69: the per-light
	// colour written there has no literal in the block, because the linker pooled the name
	// with another shader's identical one, so every position after it is shifted by an
	// unknown number of pooled entries. The bytes 0x6f and 0x70 are past the last
	// surviving literal, GetString itself has no
	// reference left in the image, and no pointer table to those strings exists - so the
	// two constants below are named by what the code writes through them, which is read
	// out of the instructions above, and not by an engine name, which is not there to
	// read. (BSDFCompositeShader's own pixel-constant block does carry the literal
	// "DynamicResolutionParams" at 30c3848; that is a different shader's list and is not
	// evidence for this one's name.)
	constexpr std::uintptr_t kWaterShaderStatePixelRecord = 0x78;
	constexpr std::uintptr_t kWaterShaderStateScreenConstantGroup = 0x150;
	// The middle group, at +0x158: slot 3 writes the water body's own colours through it
	// (the sky, silt and deep/shallow colours at psRecord+0x59..0x60 and 0x5c..0x5e).
	// Named here only so the whole three-group dump can find it.
	constexpr std::uintptr_t kWaterShaderStateMaterialConstantGroup = 0x158;
	constexpr std::uintptr_t kWaterShaderStateGeometryConstantGroup = 0x160;
	constexpr std::uintptr_t kWaterConstantGroupMapped = 0x10;
	constexpr std::uintptr_t kWaterViewStateTargetIndex = 0x1c;
	constexpr std::uintptr_t kWaterPsRegTextureProj = 0x58;
	constexpr std::uintptr_t kWaterPsRegScreenDynamicResolution = 0x6f;
	constexpr std::uint8_t kWaterPsRegAbsent = 0xFF;

	// [GHIDRA] 28a1340 is slot 3's TextureProj setter, called when psRecord+0x58 is not
	// 0xFF, writing into the +0x160 group. It builds TWO 4x4 matrices, one per eye: the
	// geometry's world transform, its rotation rows scaled by the transform's own uniform
	// scale at +0x3c and its translation row reduced by that eye's camera position
	// (viewState+0x390 and +0x39c), multiplied by the view state's matrix at +0x310 for
	// the first eye and +0x350 for the second. All 32 floats land at that one register.
	// Nothing in it reads either live ratio - which is the fact the measurement is about.
	constexpr std::uintptr_t kWaterShaderSetTextureProjFn = 0x28a1340;

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

	// [GHIDRA] 28419cf, the call instruction the LEA above feeds:
	//   28419cf  E8 DC 87 56 FF   CALL US_BSGraphicsState_UpdateTAAJitter (1daa1b0)
	// It sits in FUN_142841530, which is reached only from the first call of
	// DrawWorld_And_UI at d87ace - before RenderSceneStages at d87b52 and therefore
	// before US_BSGraphics_BuildStereoCameraViewData consumes the two floats in the
	// same frame. Hooking this call site rather than the callee leaves the only other
	// caller - 1314e9f, from DisplayLoadingScreen - running vanilla.
	constexpr std::uintptr_t kJitterUpdateCall = 0x28419cf;

	// [GHIDRA] 1daa1b0 US_BSGraphicsState_UpdateTAAJitter, the callee itself. Not
	// patched on this build; recorded because it is the fallback if a future need
	// forces an entry detour. Its prefix is six relocatable bytes with no RIP-relative
	// operand and no internal branch target, so InstallEntryProbe<6> can displace it
	// and resume at 1daa1b6:
	//   1daa1b0  40 57            PUSH RDI
	//   1daa1b2  48 83 EC 20      SUB RSP,0x20
	// An entry detour would also capture the loading-screen caller, which must stay
	// vanilla, so the call site above is the site actually used.
	constexpr std::uintptr_t kJitterUpdateFn = 0x1daa1b0;
	constexpr std::size_t kJitterUpdateEntryPrefixBytes = 6;

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

	// [GHIDRA][LIVE] US_RenderWorldImageSpaceSequence_Interface3DCall @ 284e4da - the UI
	// render entry of a steady-state frame, and the seam the HUD-clip measurement reads
	// its state at. It is in the SAME function as the late range above, 0x43 bytes later:
	//   284e497  E8 D4 18 F6 FF        CALL 27afd70            the late ImageSpace range
	//   284e49c  48 8D 0D 6D DB 05 01  LEA RCX,[1438ac010]     the render-target manager
	//   284e4b1  E8 2A 95 54 FF        CALL 1d979e0            GetUseHiddenAreaMesh
	//   284e4b8  74 1A                 JZ 284e4d4              mesh off: skip its draw
	//   284e4d4  8B 0D DA 77 03 04     MOV ECX,[146885cb4]     the range's final target
	//   284e4da  FF 15 20 78 03 04     CALL qword ptr [146885d00]
	// The only branch in that span is the hidden-area-mesh JZ, which lands at 284e4d4, so
	// this call runs exactly as often as the late range this plugin already hooks - which
	// is what makes the UI pass reachable at all from a per-frame instrument.
	//
	// The target is an indirect call through a function-pointer slot the frame setup
	// rewrites, and WHAT THE SLOT HOLDS IS THE DL=1 THUNK, NOT THE RENDERER ITSELF. This
	// is the correction the 2026-09-06 review forced: the first spelling of this block
	// said the setup "stores TS_Interface3D_RenderAll into 146885d00", and that name was
	// attached in the Ghidra project to BOTH the thunk and its jump target, so it named no
	// single address. The disassembly, read this round:
	//   140d895ef  48 8D 0D 2A 7F D7 FF  LEA RCX,[140b01520]   the DL=0 wrapper
	//   140d895f6  E8 65 5A AC 01        CALL 14284f060        -> MOV [146885cf8],RCX
	//   140d895fb  48 8D 0D 4E 7F D7 FF  LEA RCX,[140b01550]   the DL=1 thunk
	//   140d89602  E8 69 5A AC 01        CALL 14284f070        -> MOV [146885d00],RCX
	//   140b01550  B2 01                 MOV DL,0x1
	//   140b01552  E9 C9 16 00 00        JMP 140b02c20         TS_Interface3D_RenderAll
	// So 146885d00 holds 140b01550 (US_Interface3D_RenderAll_DL1Thunk in the Ghidra project
	// after this round's disambiguation; 140b02c20 keeps the True Scopes name), 146885cf8
	// holds 140b01520, the DL=0 wrapper called at 284e461 BEFORE the late range, and
	// 146885d08 holds US_ProcessLocalMapBetweenImageSpaceRanges. Two consequences, both
	// load-bearing: a hook must call THROUGH the slot rather than cache the pointer,
	// because the setup rewrites it every frame; and it must forward only the ECX this
	// site loads, because the callee's second argument (DL) is supplied by the thunk the
	// slot holds and never by this call site - resolving 140b02c20 and calling it directly
	// would pass DL undefined. The patch is a six-byte indirect call replacing a six-byte
	// indirect call.
	//
	// MEASURED NEGATIVE, and the reason this address is not the obvious one: Swap @ d831f0
	// also calls that same DL=1 thunk, at d8390b, and a hook there fired 32 times in a
	// whole session while the world path ran 23,348 times: /state uiObservation.uiCalls 32
	// against dynamicResolutionDispatch.preWorldCalls 23,348 and
	// qualityPixelTransaction.seamPublishes 23,186, in the research repo's
	// artifacts/phase5-step7-measure-20260906-1704/state-after-world.json. (The earlier
	// citation here - "against 11,455 world frames", "advanced 303 in five seconds" - is
	// withdrawn: no saved file carries either figure. The conclusion is unchanged and the
	// ratio is larger, not smaller.) Both DrawWorld_And_UI calls in Swap - d837cc and
	// d83814 - jump straight to d83928 (E9 26 01 00 00 and E9 07 01 00 00), past the whole
	// d83821..d8390b block, so that site is the no-world path: menus and loading screens.
	//
	// ECX at the site is the ImageSpace range's own final destination target id, the one
	// US_ImageSpaceManager_SetRangeFinalDestination published, so the UI draws into the
	// same target the range resolved into rather than into a target of its own.
	constexpr std::uintptr_t kUiRenderAllCall = 0x284e4da;

	// [GHIDRA][LIVE] US_ConsoleCommand_DynamicResolution @ 515fc0. Its `width` and
	// `height` subcommands convert the console double to float and store the requested
	// target ratios through these exact instructions:
	//   516106  F3 0F 11 0D 6E 75 39 03  MOVSS [rip+disp32],XMM1 -> 38ad67c width
	//   516164  F3 0F 11 0D 14 75 39 03  MOVSS [rip+disp32],XMM1 -> 38ad680 height
	constexpr std::uintptr_t kDynamicResolutionTargetWidthAnchor = 0x0516106;
	constexpr std::uintptr_t kDynamicResolutionTargetHeightAnchor = 0x0516164;

	// [GHIDRA] US_BSGraphics_SamplerStateTable at 62350a0 - one flat array of 320
	// ID3D11SamplerState*, stride 8, extent 62350a0..6235aa0. CreateSamplerStateObjects
	// @ 1dbf040 fills it once from a triple loop (filter < 5 at 1dbf1c9, address mode < 4
	// at 1dbf1d4, mip < 16 at 1dbf1eb) and US_BSGraphics_ReleaseRenderStateObjects @
	// 1dbf220 releases whatever is in it at shutdown (1dbf326 48 8D 1D 73 5D 47 04).
	//
	// Three independent instructions name the same base. MipBias resolves all three and
	// refuses to touch a slot unless they agree, because a table rewrite at a plausible
	// wrong address would corrupt state rather than fault:
	//
	//   1dbf0b8  4C 8D 25 E1 5F 47 04      LEA R12,[rip+0x04475fe1]     -> 62350a0
	//   292f944  48 8D 0D 55 57 90 03      LEA RCX,[rip+0x03905755]     -> 62350a0
	//   1d9bbb0  4E 8D 0C C5 A0 50 23 06   LEA R9,[0x62350a0 + R8*8]
	//
	// The third is not RIP-relative. It is an absolute-disp32 SIB - modrm 0C, SIB C5
	// (scale 8, index R8 through REX.X, no base) - so the table RVA is the trailing
	// dword and the check is a literal comparison rather than a third RipResolve.
	//
	// The index is idx = (addressMode*5 + filter)*16 + mip (1dbf122 LEA R14,[RSI+RSI*4]
	// is addressMode*5; 1dbf1b5 SHL RAX,4; 1dbf1b9 ADD RAX,R15 is mip), so the byte
	// sub-strides are 0x280 per address mode, 0x80 per filter and 8 per mip index.
	//
	// Filter index to D3D11_FILTER: 0 -> 0x00, 1 -> 0x14, 2 -> 0x15, 3 -> 0x55
	// ANISOTROPIC, 4 -> 0x94 COMPARISON. MinLOD is (float)mip for every entry and MaxLOD
	// is FLT_MAX for filters 2/3/4 but (float)mip for 0 and 1, so a negative bias is
	// arithmetically dead on filter indices 0 and 1 and is clamped at MinLOD on every
	// mip-index run above 0. That MinLOD is exactly the "global clamp for the mipmap
	// bias" the vendor guide tells integrators to disable.
	//
	// MipLODBias itself is written once, outside all three loops, at
	// 1dbf085 C7 45 C0 00 00 00 00 (MOV dword ptr [RBP-0x40],0x0). The descriptor base is
	// RBP-0x50 (1dbf1a5 LEA RDX,[RBP-0x50] is pSamplerDesc), so RBP-0x40 is desc+0x10.
	// All 320 states therefore ship with bias 0 and no INI knob writes it, which is what
	// makes the vendor formula's NativeBias term VERIFIED rather than assumed.
	//
	// HAZARD, and the reason nothing this plugin displaces is ever released:
	// 1d97bab 49 8B 84 CA A0 50 23 06 (MOV RAX,[R10 + RCX*8 + 0x62350a0]) copies sampler
	// pointers out of the table by value into a caller-supplied array. Both sets are held
	// for the process lifetime and only the pointers in the table are swapped.
	constexpr std::uintptr_t kSamplerStateTableCreateAnchor = 0x1dbf0b8;
	constexpr std::uintptr_t kSamplerStateTableRecordAnchor = 0x292f944;
	constexpr std::uintptr_t kSamplerBindPsSite = 0x1d9bbb0;
	constexpr std::size_t kSamplerBindPsDispOffset = 4;  // after 4E 8D 0C C5
	constexpr std::uintptr_t kSamplerStateTableRva = 0x62350a0;

	// [GHIDRA] US_BSGraphics_CommitD3DState @ 1d9b5d0 binds samplers from four
	// per-stage dirty-mask loops, each of which passes the address of the table slot
	// itself and NumSamplers = 1, so D3D reads the live slot at call time and a table
	// rewrite needs no cache invalidation anywhere. The per-slot selection lives in the
	// cached state block as three parallel int32[16] arrays per stage:
	//   1d9bb8a / 1d9bb96 / 1d9bba1   PS address mode +0x360, filter +0x3a0, mip +0x460
	//   1d9bbb0                       the LEA above, then ADD R9,RSI (image base)
	//   1d9bbc1                       CALL [R10+0x50], PSSetSamplers (vtable slot 10)
	// The VS arrays do not follow the uniform per-stage spacing the other three stages
	// share, so the probe reports VS separately and treats a nonsensical VS triple as a
	// limit of the probe rather than a finding.
	constexpr std::uintptr_t kStateSamplerPsAddressMode = 0x360;  // int32[16]
	constexpr std::uintptr_t kStateSamplerPsFilter = 0x3a0;       // int32[16]
	constexpr std::uintptr_t kStateSamplerPsMip = 0x460;          // int32[16]
	constexpr std::uintptr_t kStateSamplerVsAddressMode = 0x0dc;  // int32[16]
	constexpr std::uintptr_t kStateSamplerVsFilter = 0x11c;       // int32[16]
	constexpr std::uintptr_t kStateSamplerVsMip = 0x1e0;          // int32[16]
	constexpr std::size_t kStateSamplerSlotCount = 16;

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

	// [GHIDRA] The cached block's depth-stencil selection, all four fields read out of
	// US_RTManager_SelectDepthStencil @ 1db9e40, which is the WHOLE function - it takes
	// the manager in RCX, a selection index in EDX, a view mode in R8D and a slice in
	// R9D, resolves the index through the manager's own id table, and writes nothing to
	// D3D11:
	//   1db9e40  MOV R10,[146235ac8] / JNZ / MOV R10,[146235ac0]   the state, both slots
	//   1db9e53  ADD R10,0x1ee0                                    the cached block
	//   1db9e5d  MOV [R10+0x88],EDX          the selection index  (= kStateCurrentDsIndex)
	//   1db9e64  MOV ECX,[RCX+RAX*4+0x15fc]  manager id table, indexed by that index
	//   1db9e6b  CMP [R10+0x4c],ECX          the resolved id already cached
	//   1db9e71  CMP [R10+0x7c],R8D          the view mode already cached
	//   1db9e77  CMP R8D,0x3                 mode 3 is the only self-idempotent one
	//   1db9e7d  CMP [R10+0x50],R9D          the slice already cached
	//   1db9e83  OR [R10],0x1 / MOV [R10+0x4c],ECX / +0x7c,R8D / +0x50,R9D
	// So the engine re-applies its depth-stencil to D3D11 only when this call CHANGES
	// one of the three cached values (or the mode is not 3): the dirty word at the block
	// head is what a later state flush consumes. Reading all four says whether a
	// selection took effect or was skipped as already-current, which is the difference
	// between "the engine rebound the depth-stencil" and "the engine believed it was
	// already bound".
	//
	// Why this matters at the UI pass, and where the call sites are: inside
	// US_RenderWorldImageSpaceSequence @ 284e370 this selection runs three times, and one
	// of the three is the JZ-guarded block between the late ImageSpace range and the UI
	// render entry:
	//   284e4b1  CALL 1d979e0            US_BSGraphics_GetUseHiddenAreaMesh
	//   284e4b8  JZ 284e4d4              mesh off: skip the block entirely
	//   284e4ba  LEA R8D,[RSI-0x5e]      mode 3   (RSI is 0x61, set at 284e416)
	//   284e4be  LEA RCX,[1438ac010]     the render-target manager
	//   284e4c5  XOR R9D,R9D             slice 0
	//   284e4c8  XOR EDX,EDX             selection index 0
	//   284e4cf  CALL 1db9e40            US_RTManager_SelectDepthStencil
	// The guarded thing is therefore a depth-stencil SELECTION, not a mask draw - the
	// hidden-area mask itself is drawn by US_BSGraphics_DrawHiddenAreaMesh @ 1d97650
	// during the world, which is what forceHiddenAreaMeshOff suppresses. The same
	// selection with the same arguments already ran at 284e3c8, at the top of the
	// function, which is why the block is normally a no-op that sets no dirty bit.
	constexpr std::uintptr_t kStateCachedDirtyFlags = 0x1ee0;    // uint32, = kStateCachedBlock + 0x00
	constexpr std::uintptr_t kStateCachedDsResolvedId = 0x1f2c;  // int32,  = kStateCachedBlock + 0x4c
	constexpr std::uintptr_t kStateCachedDsSlice = 0x1f30;       // int32,  = kStateCachedBlock + 0x50
	constexpr std::uintptr_t kStateCachedDsMode = 0x1f5c;        // int32,  = kStateCachedBlock + 0x7c

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

	// --- the global camera constant buffer the deferred composite reads as cb12 ---
	//
	// [GHIDRA][LIVE] US_BSGraphics_UploadGlobalCameraConstants @ 1d93910 maps the buffer
	// and fills it:
	//   1d93968  MOV RDX,[146235ac8]            the graphics state slot Engine resolves
	//   1d93987  MOV RDX,[RDX + 0x1ea8]         the ID3D11Buffer bound as cb12
	//   1d939ac  CALL [RAX+0x70]                ID3D11DeviceContext::Map, WRITE_DISCARD
	// The two reconstruction matrices the composite's pixel shaders apply to
	// (clip.x, clip.y, depth, 1) are written as transposed inverses of two ViewData
	// fields, read as instructions rather than as a decompiled argument:
	//   1d93c65  LEA R8,[R13 + 0x50]            eye-0 ViewData + 0x50, the far matrix
	//   1d93c72  CALL 1401915c0                 XMMatrixInverse(out, null, R8)
	//   1d93c8f..1d93cdb                        the four-SHUFPS XMMatrixTranspose
	//   1d93c84  LEA RCX,[R12 + 0x140]          destination, 0x40 bytes: CB12 rows 20-23
	//   1d93b75  LEA RBX,[R13 + 0xd0]           eye-0 ViewData + 0xd0, the near matrix
	//   1d93d5b  LEA RCX,[R12 + 0x180]          destination, 0x40 bytes: CB12 rows 24-27
	// R13 is the active eye-0 ViewData staging pointer, reached at 1d939d4 through
	// state + 0x1ee0 + 0x6f0, which is kStateCameraViewData above. The shader picks the
	// far pair when device depth is above 0.01 and the near pair otherwise. What is stored
	// is the TRANSPOSE, which is what makes each stored row the row the shader's dp4 uses.
	constexpr std::uintptr_t kStateGlobalCameraConstantBuffer = 0x1ea8;  // ID3D11Buffer*
	constexpr std::uintptr_t kViewDataFarViewProjection = 0x50;          // float4x4
	constexpr std::uintptr_t kViewDataNearViewProjection = 0xd0;         // float4x4
	constexpr std::uintptr_t kGlobalCameraFarReconstructionOffset = 0x140;   // CB12 rows 20-23
	constexpr std::uintptr_t kGlobalCameraNearReconstructionOffset = 0x180;  // CB12 rows 24-27
	constexpr std::uint32_t kCameraConstantBufferSlot = 12;

	// --- BSDFCompositeShader, the deferred lighting composite ---
	//
	// [GHIDRA] The class has eight members. Its vtable's RTTI locator pointer sits at
	// 30bf220, so the vtable itself begins at 30bf228 with Func0 @ 291cce0 in slot 0;
	// slots 2, 3, 4 and 5 hold the class's Func2..Func5 in order, which is what fixes the
	// numbering. The two slots below were read as the qwords at their own addresses:
	//   30bf270  30 9E 91 42 01 00 00 00   -> 142919e30  technique setup, slot 9
	//   30bf278  70 A4 91 42 01 00 00 00   -> 14291a470  technique restore, slot 10
	// The setup maps the VS and PS slot-2 constant buffers and ends with
	// VSSetConstantBuffers(2, 1, &cb) at 291a410 and PSSetConstantBuffers(2, 1, &cb) at
	// 291a447, each guarded by the shader object's own cached binding at +0x630 / +0x648 -
	// so the engine caches what it binds per slot and does not rebind an unchanged one.
	//
	// Slots 9 and 10 are NOT the draw-time site, which cost a round to learn and is
	// recorded here so it is not learned twice: MEASURED 2026-09-09 over three headless
	// launches, slot 9 is called about 360 times in a 1,240-frame launch, off the render
	// thread and on more than one thread, and the engine's live dynamic-resolution ratios
	// read 1.0 x 1.0 at every one of those calls. They are technique-change and precache
	// calls. The per-frame site is slot 2 below.
	//
	// Slot 2 is the composite's render pass. It selects the G-buffer targets by id -
	// 0x1c, 0x1d, 0x20, 0x21, 0x22, 0x24, 0x25 on the main path and 0x62..0x6c under
	// US_BSGraphics_IsScopedPass @ 1427aeeb0 - and ends by calling the shared pass
	// executor FUN_142813a00, which six shader classes' slot 2 share. Wrapping this one
	// call is what puts a binding around the composite's own draws.
	//   30bf238  f0 ae 91 42 01 00 00 00   -> 14291aef0  render pass, slot 2
	//   30bf270  30 9e 91 42 01 00 00 00   -> 142919e30  technique setup, slot 9
	//   30bf278  70 a4 91 42 01 00 00 00   -> 14291a470  technique restore, slot 10
	// The vtable itself begins at 30bf228: its RTTI locator pointer sits at 30bf220, slot
	// 0 is Func0 @ 291cce0, and slots 2 to 5 hold Func2..Func5 in order.
	//
	// None of the three is entry-hookable - slot 10's first instruction is a seven-byte
	// RIP-relative load and slot 2's prologue spills all four argument homes - so the
	// vtable slot is patched instead.
	constexpr std::uintptr_t kCompositeShaderVtable = 0x30bf228;
	constexpr std::size_t kCompositeShaderRenderPassSlot = 2;
	constexpr std::uintptr_t kCompositeShaderRenderPassFn = 0x291aef0;
	constexpr std::size_t kCompositeShaderSetupTechniqueSlot = 9;
	constexpr std::size_t kCompositeShaderRestoreTechniqueSlot = 10;
	constexpr std::uintptr_t kCompositeShaderSetupTechniqueFn = 0x2919e30;
	constexpr std::uintptr_t kCompositeShaderRestoreTechniqueFn = 0x291a470;

	// --- native TAA jitter state layout ---
	//
	// [GHIDRA] US_BSGraphicsState_UpdateTAAJitter @ 1daa1b0 and
	// US_BSGraphics_BuildStereoCameraViewData @ 1daa370.
	//
	// +0x78 and +0x7c are the FULL per-eye extent - 1852 x 2056 on this headset - and
	// dynamic resolution never touches them. Their only writers are 1d8c1d3 (display
	// and back-buffer setup, which also fills +0x80/+0x84) and 1d9c37f / 1d9c3a4 (a
	// resize path that stores or takes a CMOVA maximum). US_BSGraphics_RegisterRender-
	// TargetDefinitions loads +0x78 at 28a4a97 and doubles it with ADD EBX,EBX only
	// while defining double-wide resources.
	//
	// BuildStereoCameraViewData does not use the stored floats directly. It rescales
	// each by the full extent over the floored reduced extent before assembling the
	// projection's fourth row:
	//   1daa588  W * live width ratio [38ad674], floored  = renderWidth
	//   1daa5d6  DIVSS  -> W / floor(W*rW)
	//   1daa5f4  MULSS  -> rowX = [state+0x04] * W / floor(W*rW)
	//   1daa54f/1daa581/1daa590  the same three steps for Y with [38ad678]
	//   1daa605  fourth row assembled as (rowX, rowY, 0, w)
	// The ratio therefore cancels: a store of (2h-1)/W applies exactly +-0.5 RENDER
	// pixels at any ratio. A writer must divide by the full W from +0x78, never by
	// renderWidth and never by W*ratio - pre-dividing by the ratio double-compensates
	// and applies +-0.746 px at 0.67.
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
	// [GHIDRA] The reflection chain's own outputs, read as instructions from the four
	// SSLR draw overrides named above: Raytracing stages 7, BlurH 8, BlurV 9. The string
	// table gives names to 6, 7, 9 and 10 (`ssrnphit`, `ssrcolor`, `ssrblur`, `ssrdepth`)
	// and skips 8, so 8 is named here for what the binary shows it being written by.
	constexpr std::int32_t kRT_SsrNPhit = 6;
	constexpr std::int32_t kRT_SsrColor = 7;
	constexpr std::int32_t kRT_SsrBlurIntermediate = 8;
	constexpr std::int32_t kRT_SsrBlur = 9;
	constexpr std::int32_t kRT_SsrDepth = 10;
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
