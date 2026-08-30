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
