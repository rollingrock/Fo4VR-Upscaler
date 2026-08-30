# Module map

What exists, what is a stub, and the one thing that unblocks each stub.

This file exists to stop a later session filling a stub with a plausible guess.
Every stub below names exactly one blocking measurement, and every measurement
names how it is taken and what a recorded answer looks like. A stub is filled in
when its measurement is recorded - in `Addresses.h` with a tag and a derivation,
or in `SOURCES.md` with a row - and not before. "It is probably the same as the
flat game" is not a measurement.

Day-one states:

- **real** - implemented and doing its job on this build.
- **partial** - the parts that can be correct without a measurement are correct;
  the rest declines.
- **stub** - logs what is missing, returns failure, changes nothing.
- **gated** - compiled, but its real body is behind a compile definition that is
  off.

## Plumbing

| Module | State | On this build |
|---|---|---|
| `src/main.cpp` | real | Log init before the version gate, VR and version refusal, message handler, load sequence. `enabled=false` skips only the trampoline and hook install; the self-check, the message listener and the dev tooling still run, and one line says hooks are off by config while diagnostics are live. |
| `src/PCH.h`, `src/PCH.cpp` | real | Includes, the `pstl` helpers, and the one-trampoline rule that must not be lost. |
| `src/Settings/Settings.h`, `SettingsList.inl` | real | Header-only TOML settings with unknown-key warnings, range clamps, live reload under a lock, and a post-load hook so live overrides survive a reload. One X-macro list, expanded twice, so the declarations and the dev-tooling table cannot drift. |
| `Data/F4SE/Plugins/Fo4VRUpscaler.toml` | partial | Five keys honoured (`enabled`, `logLevel`, `devBench`, `backend`, `eyeOrder`). Everything else is parsed, range-checked, live-reloadable and reported; each staged key names what switches it on. |
| `src/Upscaler/Common.h` | real | The single `Fn<T>(rva)` cast helper, `SafeRelease`, and the log rate-gate helper. One copy, in a header, because two `.cpp` files cannot otherwise share them. |
| `src/Upscaler/Addresses.h` | real | The tag legend, the derivation rules and the naming convention, and the first constants: five anchor sites, the three render-target table layouts and the engine's own logical target ids. Nothing here is a bare data address - each global is reached by decoding a verified instruction's RIP displacement. |
| `src/Upscaler/Hooks.h`, `.cpp` | partial | The byte-verifier and three-tier fail-soft install are real. Four soft sites cover `BSGraphics::Renderer::ClearColor`, the manager's effect-dispatch call at RVA `27afee3`, native `ImageSpaceEffectTemporalAA::Render`, and the shared pixel-draw call at RVA `2908fd7`. The entries call complete originals through gateways; both call hooks preserve vanilla. Effect dispatch is filtered to HDR manager slot 3 and supplies a thread-local scope, letting the shared draw observer distinguish HDR's final draw from its luminance intermediates and from TAA. They settle **M6** and both sides of **M13** without changing render state. |
| `src/Upscaler/FaultGuard.h`, `.cpp` | real | Guarded reads, the fault record with registers and stack RVAs, step attribution, and the retry-capped latch. No call sites yet because nothing touches engine memory. |
| `src/Upscaler/SelfCheck.h`, `.cpp` | real | The day-one deliverable: the startup environment block, and the same facts as JSON for the dev tooling. |
| `src/DevBench/DevBench.h`, `.cpp` | real | Eight actions from one X-macro table: state, TemporalAA observation, final-HDR observation, legacy ClearColor observation, config, reload, backends and health. Observation actions only arm a one-shot; `/state` reports its sequence, capture point and resolved bound slots. `/config/reload` is the only configuration mutation. |
| `src/DevBenchClient/DevBenchClient.h`, `.cpp` | real | Registers one `upscaler` tool, descriptor generated from the same table the dispatcher switches on. |
| `src/DevBenchClient/DevBenchAPI.*` | vendored | MIT glue, copied byte for byte from upstream with its licence beside it. Never reformat it. |
| `scripts/fetch-sdks.ps1`, `package-release.ps1` | real | Working, with nothing to fetch: the manifest has zero entries until **M12**. |
| `scripts/sdk-manifest.psd1` | real, empty | Schema and a commented example, no entries. Blocked by **M12**. |

## Backends

| Module | State | On this build |
|---|---|---|
| `Backend/BackendTypes.h`, `IBackend.h` | real | The POD vocabulary and the seam. Names no vendor type; grep any vendor prefix and exactly one file answers. |
| `Backend/BackendRegistry.h`, `.cpp` | real | Probes in order, logs one line per backend with its availability and the verbatim reason when unavailable, honours an explicit name over auto-selection. Never fails: worst case it selects `null`. |
| `Backend/NullBackend.h`, `.cpp` | real | Passthrough, always available. Both the fail-soft target and the A/B control arm, so our own overhead can be measured separately from any reconstruction. |
| `Backend/NgxD3D11.h`, `.cpp` | gated | `UPSCALER_ENABLE_NGX` is OFF and no SDK is staged, so `Probe` reports unavailable with a reason. Blocked by **M12**, then **M9**. |
| `Backend/Streamline.h`, `.cpp` | gated | Still a stub, but **M11 is answered and the answer is yes** - see below. `needsDeviceCreation` should become false when a body is written: the probe shows late attachment works. What is not yet shown is that `presentCommon` runs every frame. |
| `Backend/FidelityFxD3D11.h`, `.cpp` | gated | Reports unavailable because no DirectX 11 fork is pinned. Blocked by **M14**. |
| `tools/streamline-probe` | real | Answers **M11** with no game, no headset and no hook: three cases, each in its own process, reporting every `sl::Result` plus what the runtime itself says about DLSS. Only builds when `scripts/fetch-sdks.ps1` has staged the SDK, so the tree still configures with no vendor SDK present. |
| `tools/subrect-harness` | partial | Eight cases and 78 checks run with no vendor SDK and pass on hardware: exact LR/RL geometry and invalid-shape refusal, the measured 3704x2056 allocation, isolated per-eye writes, coexistence, the backend call-order contract, and D3D11.1 host/plugin context-state isolation. The state case plants a broad host sentinel across shader resources, constant buffers, IA, SO, OM, RS and predication; proves private state is active inside the scope; then proves the host state is restored and the private state persists independently across a second scope. Those settle *our* rect arithmetic and state-isolation wrapper. The five vendor cases still need a staged SDK and are the instrument for **M9**. Falls back to WARP so CI can run it, and says which driver it used. |

## Render modules

Four are real as of 2026-08-30; the rest still name the one measurement that
unblocks them.

| Module | Blocked by | What it does, or will do |
|---|---|---|
| `Engine.h`, `.cpp` | **real** | Resolves the device, immediate context, graphics state and fallback slot, render-target manager and renderer from opcode-verified anchors. Four handles come out of `BSGraphics::Renderer::ClearColor`, whose tail call to `ID3D11DeviceContext` vtable slot 50 types the context slot; the device has its own independently verified global. `Validate()` walks all three target tables and reports. |
| `RenderTargets.h`, `.cpp` | **real** | Resolves a logical target to a live texture, RTV, SRV and UAV, plus its dimensions and DXGI format, and does the same for the depth-stencil array. Plain memory reads: no D3D11 call, nothing AddRef'd. The ownership rule turned out simpler than planned - the engine's record carries the texture directly, so every pointer is borrowed and there is no teardown call at all. |
| `StereoLayout.h`, `.cpp` | **real** | Owns all double-wide geometry and is the only place per-eye rectangles are computed. Uses M5's verified left-first default, honours LR/RL overrides, and fails closed on unresolved, odd or zero geometry. The pure rectangle function is exercised by the standalone harness. |
| `D3D11StateIsolation.h`, `.cpp` | **real** | Owns one private D3D11.1 context-state object and swaps it with the exact state previously active on FO4VR's immediate context. Initialization verifies the device/context pair, immediate-context type, feature level and D3D11.1 interfaces; nested, cross-thread and stale-device use fail closed. The private state intentionally persists between scopes. A normal path may use `Scope`; a vendor boundary that can raise SEH must call `End` from `__finally`. No generic manual fallback is allowed, and exact handback requires no Bethesda state-cache invalidation. |
| `Jitter.h`, `.cpp` | **M7** | Reads the engine's sub-pixel offset, and eventually writes it. Writes always refuse on this build; reading back a vanilla offset is safe, writing one at half magnitude is not, and the failure is invisible. |
| `MipBias.h`, `.cpp` | **M8** | Rewrites sampler states with a derived bias and restores them. The restore path matters as much as the apply path. |
| `Intermediates.h`, `.cpp` | nothing structural | Per-eye zero-origin staging textures and the copies in and out. M4 and M5 are taken; this is now implementation work. It remains the default path, not a fallback, until M9 proves a vendor's sub-rect path. |
| `Evaluate.h`, `.cpp` | **M3** | The per-frame orchestrator and, with `StereoLayout`, the only code that knows there are two eyes - so a backend cannot get stereo wrong because it cannot see it. M6's cadence and seam are taken; motion-vector encoding remains. |
| `ResetPolicy.h`, `.cpp` | nothing structural | Nearly real: the request/consume latch works and every reset is logged with its reason. It has no triggers wired because there is no history to reset. |
| `Telemetry.h`, `.cpp` | nothing structural | Per-stage GPU and CPU timing. Reports disabled with a reason because there are no stages to time. The design - a ring of timestamp-disjoint query pairs collected without flushing - must be built, not borrowed; the dev-tooling host has no GPU stage timers on this platform. |
| `Debug.h`, `.cpp` | partial / **M3** | The request-driven binding observation is real: the render callback never waits, snapshots OM/PS bindings, resolves every bound view back to logical color/depth ids, releases every temporary COM reference and performs no readback. The default point is after native TAA's draw and before cleanup; ClearColor remains available for comparison. DDS target dumping and input visualisation remain blocked by **M3**. |

## The measurements

Each one is a single question with a single answer. Where it lands is what makes
it checkable: if the artefact named under "recorded as" does not exist, the
measurement has not been taken.

**M1 - engine handles. TAKEN, 2026-08-29.** All four globals plus the renderer are
in `Addresses.h` as opcode-verified anchors, and the plugin resolves them at load:

```
engine: ctx slot rva 6235AB0, state rva 6235AC8 (alt 6235AC0), rtm rva 38AC010, renderer rva 6239340
```

The context is typed by the bytes, not by a symbol: `ClearColor` loads the global,
dereferences it to a vtable and tail-calls `vtbl[0x190]` - slot 50,
`ID3D11DeviceContext::ClearRenderTargetView` - with `(this, RTV, float4)`. The
renderer is confirmed live by its stereo flag byte reading 1.

The device is independently resolved from its own global at RVA `60f3cf0`, so no
`ctx->GetDevice` COM call is needed. **M10 is answered.** The temporary ClearColor
probe now supplies a measured render-thread rendezvous for later visual instruments.

**Render-thread rendezvous - TAKEN, 2026-08-30.** A byte-verified five-byte entry
detour at `ClearColor` calls a gateway containing the displaced instruction and then
the complete vanilla body. In a live headless run, calls rose from 450 to 993 over
three seconds with renderer `146239340` and no mismatches. During `coc RedRocketExt`,
421 calls arrived from a secondary thread; across the next nine seconds in the loaded
exterior, calls rose from 8063 to 10251 while that mismatch count stayed fixed.
Therefore the point is single-threaded in steady rendering but not across a load
transition. Future capture work must gate to the primary observed thread.

**M2 - render-target tables. TAKEN, 2026-08-29.** The whole chain is in
`Addresses.h` and the plugin walks it in-game:

```
tables - 123 of 144 logical ids map to a slot, 123 carry dimensions, 123 resolve to a shader resource view; stereo flag set
target mv     logical 35 -> slot 32, 3704x2056, dxgi 34, rtv yes, srv yes, uav no
target ldepth logical 44 -> slot 43, 3704x2056, dxgi 41, rtv yes, srv yes, uav yes
depth-stencil - 15 of 17 allocated; widest is logical 1 -> slot 2, 3704x2056 (array 1), dsv yes, srv yes, tex yes
```

**Depth-stencil resource correction, 2026-08-30.** The earlier live scan's
`renderer+0x2670` base was withdrawn and is now superseded. The complete creation
and destruction functions prove `renderer+0x2598+physical*0x98`, with the depth
texture at record `+0x00`, writable DSV slice zero at `+0x08`, and depth-plane SRV
at `+0x88`. Logical depth ids remap through `rtm+0x15fc`; they must never index the
physical records directly. The adjacent array bases also prove 145 physical colour
records and 18 physical depth records, one more than their 144/17 logical tables.
The same result is exposed as the `depthStencil` object in the devbench `/state`
response, so the mapping and required views can be asserted without parsing the log.

Motion vectors coming back as a two-channel half float and linear depth as a single
32-bit float is the answer key: no other buffer in the set would plausibly carry
either, so if those two are right all three tables are based correctly. Every one of
those numbers was also read independently through devbench before a line of this
code existed, and the two paths agree exactly.

The manager turned out to be the object at the address previously recorded as the
"properties array base" - the properties array is simply its first member.

WARNING RETRACTED. This entry used to say the remap table shifts between game
loads, so a logical id captured once at startup is a bug. On this build it does not
shift: byte-identical across two separate process launches and across a
menu-to-exterior transition within one run, 144 of 144 entries both times.
Re-resolving per use is kept anyway - it costs three reads, and the measurement
covered one configuration rather than every configuration.

**M3 - motion vectors.** Which target carries them, its format, and the encoding -
units, sign convention, and whether the engine's own jitter is already in them.
*Taken by:* a RenderDoc capture of the unmodded game, then a DDS dump through
`Debug` once **M2** lands.
*Recorded as:* a `SOURCES.md` row naming target and format, and a dumped frame
whose vectors visibly track camera motion.

**M4 - colour input. TAKEN, 2026-08-30.** The engine's raw HDR scene-colour
targets are `R11G11B10_FLOAT`, at logical 1, 3, 15 and 16 (and `diff` 36, `spec` 37,
38, 39). Logical 1 and 3 also carry a UAV, so they are compute-writable. Read live
from the properties table in a loaded exterior.

**`R11G11B10_FLOAT` has no alpha channel**, so a reactive mask cannot ride in alpha
and needs its own surface. That is the decision this measurement existed to make.

The native temporal draw reads color target 3 and writes ping-pong target 61/62 plus
submitted target 4. Static ImageSpace ordering and the executable's shader identities
prove that this use of target 3 is downstream of
`BSImagespaceShaderHDRTonemapBlendCinematic` (or its Fade variant): it contains
post-tonemap values stored in a float surface. Format and semantic range are distinct.

The upstream side is now live-verified too: ImageSpace manager effect 3 receives
logical target 1 and writes logical target 3. Source 1 is the exact pre-tonemap color
handoff; it is 3704x2056 `R11G11B10_FLOAT` and has RTV/SRV/UAV views.

**M5 - stereo layout of the pre-tonemap targets. TAKEN, 2026-08-30.** They share
the submitted frame's double-wide layout, and the left eye is the left half.

- **Dimensions: yes.** The G-buffer set the engine binds as MRT slots 0-5 -
  albedo 28, normal 29, envmap 32, ambient 33, emit 34, **motion vectors 35** -
  are all `3704 x 2056`, read from the engine's own descriptor table in a running
  game. Linear depth is index 44, same size. So the pre-tonemap targets do share
  the submitted frame's double-wide layout.
- **Eye order of the submitted frame: left eye is the left half**, `x 0..1851`,
  from the per-eye `IVRCompositor::Submit` bounds and corroborated by the
  hidden-area mesh. Adversarially verified.
- **Eye order of the pre-tonemap targets: left-first.** Bethesda's shipping
  `BSDFPrePassShader` bytecode was exhaustively classified: 268 raster VS and all
  69 DS permutations map instance 0 to `0.5*x - 0.5*w` and instance 1 to
  `0.5*x + 0.5*w`; 44 tessellation-feed VS carry the same eye bit to those DS.
  The executable independently binds the live stereo predicate buffer as VS/DS
  CB8, closing the shader gate as well as the formula.
*Recorded as:* `SESSION_2026-08-30_PRETONEMAP_EYE_ORDER_SHADER_PROOF.md` and rows
in the research repo's `SOURCES.md`. `StereoLayout` now uses LR for `auto`; the
explicit override stays because a recovery path must remain checkable.

**M6 - the engine's temporal-AA dispatch. TAKEN, 2026-08-30.** FO4VR has a
concrete `ImageSpaceEffectTemporalAA::Render` virtual at RVA `28a72b0`. Its
configured inputs are two history targets (61/62), motion 35, depth-stencil 1 and
effect mask 33; all temporal image inputs used here are the measured 3704x2056
double-wide resources. A byte-verified entry probe stayed at zero in the main
menu, then ran in `RedRocketExt` on one stable thread and one stable effect object.
Across a timed exterior window, devbench's engine frame number advanced 603 while
this entry advanced 601; the endpoints are separate HTTP requests, so the two-call
skew is expected. The effect virtual contains one shader invocation, not an eye
loop. The native effect therefore runs once per rendered stereo frame, over the
double-wide target, rather than once per eye.

This answers cadence, but does not bless the vanilla TAA output as the upscaler
injection point. The complete live map identifies the native temporal seam, and static
shader ordering proves it is post-tonemap. A preferred DLSS/FSR/XeSS integration must
move earlier to the input side of the HDR/tonemap stage.

**M7 - the jitter divisor.** Whether the engine's two sub-pixel offset floats are
scaled by per-eye width or by double-wide width.
*Taken by:* a single live read of the two offset floats and the two adjacent
screen-size floats while jitter is active, cross-checked against the engine's own
screen dimensions.
*Recorded as:* a `SOURCES.md` row and the comment at the one place jitter is
written. Second item in the same work: the CommonLibF4 fork's `BSGraphics::State`
accessor returns the state struct by value, so a write through it mutates a
temporary and does nothing. Fix the fork before this file gets a real body.

**M8 - the sampler-state table.** Where the engine keeps the sampler states whose
mip LOD bias has to be rewritten and restored.
*Taken by:* Ghidra from the sampler-set call sites.
*Recorded as:* tagged constants. A resolved render ratio is needed too; M5 now
provides the per-eye output width, while backend configuration still has to select
the input size.

**M9 - NGX sub-rects on D3D11.** Whether an evaluate with non-zero sub-rect bases
succeeds and writes only the half it was given.
*Taken by:* `tools/subrect-harness` case 3, with an SDK staged, no game and no
headset. Case 5 is the negative control.
*Recorded as:* a `SOURCES.md` row and `Caps::supportsSubrects`. Nothing in the
research corpus has ever observed such an evaluate succeeding, so the answer is
presumed no and the per-eye copies are the default until this says otherwise.

**M10 - device feature level. TAKEN, 2026-08-29.** `0xB100`, i.e.
`D3D_FEATURE_LEVEL_11_1`, read from a running game and reported in the self-check.

It came free with a correction rather than with M1 as planned. The device does have
a global, at `60f3cf0`; the earlier conclusion that it did not only looked at the
immediate context's neighbours. Reading it is a plain memory read on any thread, so
there is no COM call and no render-thread callback in the path at all.

A backend that needs compute features the device was not created for must still
report unavailable rather than fail at dispatch.

**M11 - Streamline from a plugin. TAKEN, 2026-08-29. The answer is YES.**

`tools/streamline-probe` settles it with no game, no headset and no hook. Three
cases, each in its own process because Streamline keeps global state across
`slInit`/`slShutdown`. All three pass every step. Case 2 is the F4SEVR situation -
device and swap chain first, then init:

```
[ ok ] slInit, AFTER swap chain (manual)      Result::eOk
[ ok ] slSetD3DDevice                         Result::eOk
[ ok ] slIsFeatureSupported(DLSS)             Result::eOk
[ ok ] slUpgradeInterface(existing swapchain) Result::eOk   proxy differs: YES
       D3D11 supported by sl.dlss : YES       maxNumViewports : 4
```

The manual-hooking guide requires `slInit` to precede swap-chain creation. **It is
not enforced at init.** And `maxNumViewports` of 4 means two eyes with two
independent histories sits comfortably inside the API.

⚠️ **The trap.** Without `Preferences::projectId`, the per-adapter
`slIsFeatureSupported` answers `eErrorFeatureNotSupported` on a 4090 while the
null-adapter query answers `eOk`. It reads exactly like "this GPU cannot do DLSS"
and means "this application has no identity". The probe sets a `projectId` and
installs a `logMessageCallback` so the reason is visible rather than the symptom.

*Still open, and it is M11's remainder:* whether Streamline's `presentCommon()` is
reached every frame inside the engine. The guide is emphatic that it must be. The
probe cannot test it; that needs the game, and it is entangled with the
render-thread callback decision.

The device-creation interposer below is therefore **not** required for attachment.
The research strand recommends hooking `D3D11CreateDeviceAndSwapChain` anyway, for
the different reason of guaranteeing `presentCommon` - that is a live option, not a
settled need.

**M12 - vendor licence terms.** What Streamline's `license.txt`,
`nvngx_dlss.license.txt` and AMD's `license.md` actually permit.
*Taken by:* fetching the vendor's own published archive and reading them. No
leaked or repacked copy, ever.
*Recorded as:* `THIRD_PARTY.md` rows moving from REPORTED to VERIFIED with the
clause quoted, `sdk-manifest.psd1` entries pinned by URL and SHA-256, and a
decision-log entry for anything that becomes shippable. Statically linking the NGX
library into our DLL is part of this decision, not a build default - which is why
`UPSCALER_ENABLE_NGX` is OFF.

**M13 - native temporal replacement seam. TAKEN, 2026-08-30.** The generic
`US_BSImagespaceShader_DrawPixelPass` prepares its `BSRenderPass`, then makes one
direct five-byte call at RVA `2908fd7` (`E8 64 80 F8 FF`) before virtual shader
cleanup. The TemporalAA entry gateway supplies a thread-local scope, so this shared
call site can distinguish TAA from every other image-space draw without guessing a
shader object address.

A live post-draw one-shot resolved the complete relevant binding map:

```text
PS0 scene source       logical 3
PS1 history            logical 61 or 62
PS2 motion             logical 35
PS3 depth              depth-stencil 1
PS4 effect mask        logical 33
OM0 new history        logical 62 or 61 (opposite PS1)
OM1 submitted stereo   logical 4
```

PS5/PS6 are bound non-render-target textures and remain deliberately reported as
unmatched. Twelve earlier and eight full-map samples showed the 61/62 ping-pong in
both directions. The current hook still always calls vanilla. The seam is verified
post-tonemap, so it remains a diagnostic/passthrough point while the preferred
pre-tonemap evaluate location is traced.

**M13 - upstream HDR boundary. TAKEN, 2026-08-30.** `RenderEffectRange` calls
`US_ImageSpaceManager_DispatchEffectObject` directly at RVA `27afee3`, bytes
`E8 B8 02 00 00`. That call carries manager, effect object, source logical RT and
destination logical RT. The fail-soft hook reads manager effect slot 3 once, filters
the shared call to that exact `ImageSpaceEffectHDR` object, records the ids and calls
vanilla unchanged.

In `RedRocketExt`, the route was logical **1 → 3**. An initial sample had 523 HDR
calls and 523 TAA calls. A timed window advanced both by exactly 1,378 on the same
thread, with zero route or thread mismatches; devbench observed 1,370 frame-counter
transitions across separately sampled endpoints. This is the concrete pre-tonemap
handoff.

The scoped final-draw observer then completed only when OM contained the dispatch
destination. Seven live samples all resolved OM0 color 3, PS0 color 74, PS1 color 1,
PS3 color 33; PS2 alternated between color 80 and 81. The source that reconstruction
must replace is therefore PS1, not a guessed slot.

Directly substituting a plugin-owned SRV into the top-level HDR effect is not a valid
contract. The generic dispatcher installs an `ImageSpaceTexture` for source 1, and HDR
immediately extracts that object's engine logical RT id to build its luminance chain;
an external texture has no such id. The leading implementation candidate is instead:
evaluate from logical 1 into a private output, and only after success copy the finished
frame back into logical 1 under exact D3D11 state isolation. Vanilla HDR then executes
unchanged as `1 -> 3`. Descriptor/output-extent validation and exact D3D11 state
isolation still precede that mutation; M5 is now taken.

**D3D11 state isolation - TAKEN, 2026-08-30.** The required mechanism is
`ID3D11Device1::CreateDeviceContextState` plus
`ID3D11DeviceContext1::SwapDeviceContextState`, using the game's exact feature level
and immediate context. Microsoft documents the object as holding all D3D state and the
swap API specifically as the way a plugin saves and later restores a host's previously
active state. The standalone hardware harness independently exercised our wrapper with
a broad sentinel spanning shader resources and constant buffers, IA, SO, OM, RS and
predication. It also proved that our private state remains separate across frames.

A hand-maintained `Get*`/`Set*` block is rejected as a generic fallback: its correctness
ends at the last state its author remembered, and an engine-cache invalidation cannot
repair an omitted restore. If D3D11.1 context-state creation is unavailable, a future
render mutation declines and vanilla continues. After a successful swap back, Bethesda's
cached state and the real immediate-context state are still aligned, so the plugin does
not invalidate or force-rebind the engine cache. Asynchronous objects are outside the
swap contract; plugin passes must not begin or end host queries.

**M14 - a FidelityFX DirectX 11 fork.** Which community fork to pin. The official
SDK has no DirectX 11 backend; the forks differ sharply in health, and the one a
nearby integration pins is roughly twenty months stale.
*Taken by:* comparing fork activity and API surface, then pinning one.
*Recorded as:* a `THIRD_PARTY.md` row. The build problem comes with it: that code
needs `/EHa`, the preset puts `/EHsc` in the global flags, and appending `/EHa`
yields an MSVC override warning - the fix is to strip `/EHsc` for those sources,
not to append.

## Not created, deliberately

Each of these is a decision, not a measurement. None has a file, and none should
get one until its decision is recorded.

| Not created | Decided by |
|---|---|
| Device-creation interposer | **M11**. If Streamline can initialise late, nothing interposes device creation and this file never exists. Only a backend reporting `needsDeviceCreation` justifies it. |
| Our own sharpening pass | Whether the selected backend reports `ownsSharpening`. If every shipped backend sharpens its own output, a second pass is cost with no benefit. |
| Reactive-mask pass | **M4**, plus a design for what marks a pixel reactive in this engine's render order. The `reactiveMask` key exists; the pass does not. |
| Interface and Pip-Boy exclusion pass | Identifying the interface render paths. Motion vectors do not update while menus are open, so reconstructing them smears text - but the exclusion needs a real boundary, not a guess. |
| Dynamic-resolution ownership | Whether the plugin takes over the render resolution at all, which decides whether `renderScaleOverride` and the non-`dlaa` quality modes mean anything. |
