# SOURCES - provenance ledger

Required by `CLEAN_ROOM_PROTOCOL.md` §5. Every non-obvious design decision in this
repository must be traceable to a permitted source. Append-only; newest at the
bottom of each section.

Status vocabulary: **VERIFIED** (primary source read directly) / **REPORTED**
(secondhand) / **INFERRED** (our own reasoning from permitted inputs).

How this file gets filled, so it does not rot:

- Every Ghidra batch appends a row to **Game binary analysis** before
  `save_program`, naming the `US_` symbol and the function or instruction the
  value was decoded from. An un-recorded finding is a lost finding.
- Every commit that introduces a technique names its source in the commit body,
  and adds the row here if the technique is not already listed.
- Where a design lands where prior art also landed, the row names the external
  constraint that forced it - a vendor guide section, an engine import, a formula
  the SDK specifies. A named constraint is the defence; an unexplained
  coincidence is not.

The research repo's own `SOURCES.md` covers the feasibility phase. This one covers
the implementation. Neither supersedes the other.

## Our own prior work (freely reusable)

| What we needed | Where it came from | Status |
|---|---|---|
| FO4VR plugin skeleton, build system, address resolution, settings framework, logging, packaging, hook install discipline | `C:/repos/Fo4VR-True-Scopes` - the author's own shipped mod, the template this repo is built from | VERIFIED |
| FO4VR deferred renderer, render-target identities, ImageSpace/tonemap path, camera commit, `BSGraphics::State` | `C:/repos/fallout4-scope-in-scope-investigation` - the author's own True Scopes RE. Facts to re-verify against this binary, not constants to paste | VERIFIED |
| F4SEVR trampoline hazard: `AllocTrampoline` falls back to `Trampoline::create`, which releases the previous buffer and orphans earlier stubs, so one allocation serves the whole plugin | `C:/repos/Fo4VR-True-Scopes/src/PCH.h` - the author's own comment, carried forward word for word | VERIFIED |

## Platform API contracts

| What we needed | Where it came from | Status |
|---|---|---|
| Exact D3D11 immediate-context isolation for a plugin pass: a context-state object holds all D3D state and some behavior; swapping activates its saved state and returns the previously active object so the host state can later be restored. The API is immediate-context-only, serialized with other immediate-context calls, does not affect asynchronous objects, and requires the context-state single-threaded flag when the device used its corresponding flag | Microsoft Learn, `ID3D11Device1::CreateDeviceContextState` and `ID3D11DeviceContext1::SwapDeviceContextState`: https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-createdevicecontextstate and https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate. The shipping wrapper is independently accepted by `tools/subrect-harness`: hardware D3D11 adapter, broad host sentinel, private-state persistence, 8 cases / 78 checks total | VERIFIED (API contract and harness); INFERRED (reject manual generic fallback and omit Bethesda cache invalidation after exact restore) |

## Game binary analysis

Every RVA, offset, original-byte string and render-target identity in
`src/Upscaler/Addresses.h` gets a row here. Empty on day one because the file
carries no constants: nothing has been re-found in `Fallout4VR.exe` yet.
`docs/MODULE_MAP.md` lists what is waiting to be found.

| What we needed | Where it came from | `US_` symbol | Status |
|---|---|---|---|
| _(pending - Ghidra sessions on `fallout4vr_pdb` get logged here)_ | | | |
| Depth-stencil logical remap, physical record layout, field identities, and physical capacities: `rtm+0x15fc`; `renderer+0x2598+phys*0x98`; texture `+0x00`; DSV arrays `+0x08/+0x28/+0x48/+0x68`; depth/stencil SRVs `+0x88/+0x90`; 18 physical DS and 145 physical colour records | Complete disassembly plus raw instruction bytes from `CreateDepthStencilTarget` @ `0x141d99880`; remap write in `US_BSGraphics_DefineDepthStencilTarget` @ `0x141db96e0`; remap read in `TS_RTManager_SelectDepthStencil` @ `0x141db9e40`; independent release cross-check in `DestroyDepthStencilTarget` @ `0x141d99ef0`; adjacent cube-map base from `DestroyCubeMapRenderTarget` @ `0x141d9a010`. Re-derived 2026-08-30 and recorded in the Ghidra program before save | `US_BSGraphics_DefineDepthStencilTarget`, `US_upscaler` plate comments/bookmark | VERIFIED |
| Temporary render-thread rendezvous at `BSGraphics::Renderer::ClearColor`, RVA `0x1d8dd80`; entry bytes `48 89 5C 24 08` are one complete instruction and can be displaced into a gateway before resuming at `+5` | Complete function disassembly and raw bytes from `Fallout4VR.exe`; the tail call still proves the vanilla body ends in `ID3D11DeviceContext::ClearRenderTargetView`. Live headless validation saw a rising call count, the exact renderer singleton, and zero renderer mismatches. A Red Rocket load transition used a secondary thread, while the following steady exterior samples did not add mismatches | `US_BSGraphics_Renderer_ClearColor` plate comment / `US_upscaler` bookmark | VERIFIED (bytes and live observations); INFERRED (safe future captures must gate to the primary observed thread) |
| Request-driven binding observation contract: a primary-thread callback uses a try-only lock, snapshots OM and PS state, releases all COM references immediately, and leaves pixel data and D3D state untouched | Implemented against the D3D11 getter ownership contract and live-tested with sixteen main-menu and twenty verified `RedRocketExt` requests. Every request completed without a hang or crash. Repeated 1024x1024 colour/depth targets and absent canonical temporal inputs show that ClearColor observes shadow work, so the mechanism remains diagnostic rather than becoming the injection point | `US_BSGraphics_Renderer_ClearColor`; static TemporalAA symbols are recorded in the research ledger before Ghidra save | VERIFIED (mechanism and observations); INFERRED (shadow-pass classification) |
| Native TemporalAA live cadence: `ImageSpaceEffectTemporalAA::Render` at RVA `0x28a72b0` runs in a normal exterior once per rendered stereo frame, on one stable thread and one stable effect object; it does not run at the main menu | The soft entry probe byte-verified `48 89 5C 24 08`, called the full original through a gateway, and reported zero menu calls after 1,152 ClearColor calls. In verified `RedRocketExt`, a timed window advanced 601 TAA entries while devbench advanced 603 engine frames; the two endpoint pairs were sequential HTTP requests. Thread/effect mismatch counts stayed zero. Static disassembly shows one shader invocation and the configured inputs are the measured double-wide targets | `US_ImageSpaceEffectTemporalAA_Render` @ `0x1428a72b0` | VERIFIED |
| Native TemporalAA's exact live binding map and M13 seam: PS0 color 3, PS1 prior history 61/62, PS2 motion 35, PS3 depth-stencil 1, PS4 mask 33; OM0 opposite/new history 62/61 and OM1 submitted target 4. The direct draw call is RVA `0x2908fd7`, bytes `E8 64 80 F8 FF`, after pass preparation and before shader cleanup | Full disassembly of `US_BSImagespaceShader_DrawPixelPass`; request-driven post-draw captures in verified `RedRocketExt`. The hook calls vanilla first and only observes while the same thread is inside `US_ImageSpaceEffectTemporalAA_Render`. Multiple one-shots saw both directions of the 61/62 ping-pong. PS5/6 were bound but did not match engine RT/DS views | `US_BSImagespaceShader_DrawPixelPass` @ `0x142908e30`; draw call bookmark @ `0x142908fd7` | VERIFIED |
| Color 3 at native TemporalAA is post-tonemap data held in an `R11G11B10_FLOAT` target. The native M13 seam is therefore later than the preferred reconstruction evaluate point | Direct Fallout4VR.exe ordering: manager effect 3 `ImageSpaceEffectHDR` runs in range 0..14 before TemporalAA effect 19 in range 16..22. Its final selected shaders identify themselves as `BSImagespaceShaderHDRTonemapBlendCinematic` / `...Fade`; manager scratch target 3 is the resource observed live at TAA PS0 | Research ledger `SESSION_2026-08-30_DEPTH_AND_RENDER_THREAD_PROBE.md`; Ghidra functions `US_RenderImageSpaceHDR` @ `0x142883360`, shader ctors `0x1427c8780` / `0x1427c8a20` | VERIFIED (stage identity); exact transfer function/range remains open |
| M13 upstream HDR boundary: manager effect 3 receives logical color 1 and writes logical color 3 once per rendered world frame. At its final draw, OM0 is color 3 and PS1 is color 1; PS0 is 74, PS2 ping-pongs 80/81, PS3 is 33 | Fail-soft direct-call hook at RVA `0x27afee3`, expected bytes `E8 B8 02 00 00`, filtering the shared `RenderEffectRange` dispatch against manager effect slot 3 and always calling vanilla. Live `RedRocketExt`: initial HDR/TAA counts 523/523; timed deltas 1378/1378 on the same thread, source 1, destination 3, zero route/thread mismatches. Seven request-driven final-draw captures used the same shared pixel hook as TAA and completed only when OM matched the live HDR destination; all seven agreed except the observed 80/81 ping-pong | `ImageSpaceManager::RenderEffectRange` call @ `0x1427afee3`; `US_ImageSpaceManager_DispatchEffectObject` @ `0x1427b01a0`; final shader invocation @ `0x142883c05`; research session ledger | VERIFIED |
| A plugin-owned SRV cannot directly replace the top-level HDR source object. The dispatcher stores source logical 1 as effect texture 1; `US_RenderImageSpaceHDR` reads that `ImageSpaceTexture`'s `renderTarget` field to construct its luminance input before installing the same source into the final shader. The final shader's first texture is its OM output, so engine texture 1 appears at live PS1 | Direct Fallout4VR.exe control/data flow: `ImageSpaceEffect::SetTexture` @ `0x1428fe630`, `ImageSpaceTexture::SetTextureRenderTarget` @ `0x1428fcc20`, logical-id getter @ `0x1428fcd60`, `US_RenderImageSpaceHDR` @ `0x142883360`; final live binding map above | VERIFIED (constraint); private-output copy-back remains a design candidate |
| M5 stereo geometry: the world deferred prepass and submitted frame are both left-first double-wide. `SV_InstanceID` 0 maps to the left half and 1 to the right, including the tessellated path; the shader's CB8 gate is the engine's active stereo predicate | Bethesda-shipped `Fallout4_VR - Shaders.ba2`, extracted with Bethesda Archive2 and exhaustively disassembled with Microsoft `fxc`; executable identification through `US_LoadShippingShaderPackages`, `US_LoadBSShaderFamilyFromFXP`, and `US_BSDFPrePassShader_ctor`; exact CB8 binding/update through `US_BSGraphics_BindGlobalConstantBuffers` and `US_BSGraphics_CommitD3DState`. Full hashes, offsets and counts are in the research repo's `SESSION_2026-08-30_PRETONEMAP_EYE_ORDER_SHADER_PROOF.md` | VERIFIED |

## Third-party open source

| Component | Licence | How used | Attribution location |
|---|---|---|---|
| CommonLibF4 (rollingrock fork) | MIT | Submodule at `external/CommonLibF4`, statically linked. F4SE interface layer, address resolution, trampolines, RE types | `external/CommonLibF4/LICENSE`, row in `THIRD_PARTY.md` |
| devbench cross-plugin API glue (`DevBenchAPI.h`, `DevBenchAPI.cpp`) | MIT, scoped by upstream to exactly these two files | Vendored verbatim at `src/DevBenchClient/`, so the dev tooling can register without taking devbench's GPL-3.0 | `src/DevBenchClient/DevBenchAPI.LICENSE.txt`, row in `THIRD_PARTY.md` |
| devbench route-registry design (hand-built JSON, name-to-handler registry, no JSON library) | GPL-3.0-or-later | Shape of the idea only; no devbench source vendored beyond the two MIT files above | Header comment in `src/DevBench/DevBench.h`, row in `THIRD_PARTY.md` |
| vcpkg ports resolved by the manifest baseline | see `THIRD_PARTY.md` | Statically linked into the DLL | `THIRD_PARTY.md`, and each port's installed `copyright` file |
| `northaxosky/fallout4-community-shaders` | GPL-3.0 | Shape of two ideas, no source vendored: the pin-and-verify SDK staging mechanism, and the required-files assertion a release archive must pass before it is produced. Its exception wording was also consulted for `EXCEPTIONS.md`, which records where ours deliberately differs | Header comments in `scripts/fetch-sdks.ps1` and `scripts/package-release.ps1`; `EXCEPTIONS.md` |
| `alandtse/open-shaders`, OptiScaler | GPL-3.0 | Permitted reading under the project licence. Not yet read, nothing taken. A file that ever carries a line from one of them gets an SPDX header, both copyrights and a `Derived from` line | This table, when it happens |

## Vendor SDKs

| SDK | Licence / redistribution terms | Shipped or user-supplied | Status |
|---|---|---|---|
| NVIDIA NGX / DLSS (`nvngx_dlss.dll`, `nvsdk_ngx` static libraries) | not read on this project | User-supplied; NGX linkage OFF by default | REPORTED |
| NVIDIA Streamline (`sl.*.dll`) | framework reported MIT, DLSS plugin not | User-supplied; backend not built | REPORTED |
| AMD FidelityFX runtime and the MIT DirectX 11 community fork | not read on this project | User-supplied; no fork pinned | REPORTED |
| Intel XeSS (`libxess.dll`) | not read on this project | User-supplied; no backend | REPORTED |

No vendor archive is pinned in `scripts/sdk-manifest.psd1` yet. A row moves from
REPORTED to VERIFIED only with the clause quoted and the file path that was read.

## Requirements gathered from public reports (behaviour only, never mechanism)

Observed behaviour of other software is a requirement, never design input, and
never evidence of a mechanism. A row here may say what fails; it may not say why
someone else's code failed.

| Observed behaviour | Source | Requirement it implies |
|---|---|---|
| _(pending)_ | | |
