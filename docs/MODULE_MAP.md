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
| `Data/F4SE/Plugins/Fo4VRUpscaler.toml` | partial | Seven keys honoured (`enabled`, `logLevel`, `devBench`, `backend`, `eyeOrder`, `diagnosticHdrRoundTrip`, `diagnosticStereoNull`). Both diagnostics are off by default, mutually exclusive and bounded to 512 byte-preserving colour frames. The stereo gate stages measured motion beside colour and supplies validated per-eye extents into logical DS1 while `NullBackend` ignores both inputs. Everything else is parsed, range-checked, live-reloadable and reported; each staged key names what switches it on. |
| `src/Upscaler/Common.h` | real | The single `Fn<T>(rva)` cast helper, `SafeRelease`, and the log rate-gate helper. One copy, in a header, because two `.cpp` files cannot otherwise share them. |
| `src/Upscaler/Addresses.h` | real | The tag legend, the derivation rules and the naming convention, and the first constants: five anchor sites, the three render-target table layouts and the engine's own logical target ids. Nothing here is a bare data address - each global is reached by decoding a verified instruction's RIP displacement. |
| `src/Upscaler/Hooks.h`, `.cpp` | partial | The byte-verifier and three-tier fail-soft install are real. Four soft sites cover `BSGraphics::Renderer::ClearColor`, the manager's effect-dispatch call at RVA `27afee3`, native `ImageSpaceEffectTemporalAA::Render`, and the shared pixel-draw call at RVA `2908fd7`. The entries call complete originals through gateways; both call hooks preserve vanilla. Effect dispatch is filtered to HDR manager slot 3 and supplies a thread-local scope, letting the shared draw observer distinguish HDR's final draw from its luminance intermediates and from TAA. It also calls the separately gated `HDRRoundTrip` immediately before vanilla HDR. |
| `src/Upscaler/FaultGuard.h`, `.cpp` | real | Guarded engine-memory/code reads, the fault record with registers and stack RVAs, step attribution, and the retry-capped latch. Engine address resolution, target lookup and hook-byte verification use its fail-soft read boundary. |
| `src/Upscaler/SelfCheck.h`, `.cpp` | real | The day-one deliverable: the startup environment block, and the same facts as JSON for the dev tooling. |
| `src/DevBench/DevBench.h`, `.cpp` | real | Eight actions from one X-macro table: state, TemporalAA observation, final-HDR observation, legacy ClearColor observation, config, reload, backends and health. Observation actions only arm a one-shot; `/state` reports its sequence, capture point and resolved bound slots. `/config/reload` is the only configuration mutation. |
| `src/DevBenchClient/DevBenchClient.h`, `.cpp` | real | Registers one `upscaler` tool, descriptor generated from the same table the dispatcher switches on. |
| `src/DevBenchClient/DevBenchAPI.*` | vendored | MIT glue, copied byte for byte from upstream with its licence beside it. Never reformat it. |
| `scripts/fetch-sdks.ps1`, `package-release.ps1` | real | Fetch verifies the vendor URL and archive SHA-256, requires the recorded licence texts, stages into a git-ignored cache and never packages from that cache. The release packager remains the conveyance-safe path. |
| `scripts/sdk-manifest.psd1` | real | Pins NVIDIA's official Streamline v2.12.0 archive and stages the public Streamline set, the raw NGX header/client-library closure, the production probe runtime and all governing notices. No vendor file is committed. |

## Backends

| Module | State | On this build |
|---|---|---|
| `Backend/BackendTypes.h`, `IBackend.h` | real | The POD vocabulary and the seam. Names no vendor type; grep any vendor prefix and exactly one file answers. |
| `Backend/BackendRegistry.h`, `.cpp` | real | Probes in order, logs one line per backend with its availability and the verbatim reason when unavailable, honours an explicit name over auto-selection. Never fails: worst case it selects `null`. |
| `Backend/NullBackend.h`, `.cpp` | real | Passthrough, always available. Both the fail-soft target and the A/B control arm, so our own overhead can be measured separately from any reconstruction. |
| `Backend/NgxD3D11.h`, `.cpp` | **real behind an optional compile gate; live selection locked** | The default SDK-free build is a fail-soft stub. With `UPSCALER_ENABLE_NGX=ON`, the raw D3D11 body initializes against an explicit runtime directory, rejects a conflicting loaded basename, verifies/logs the final DLL path and version, queries capabilities/render sizes, creates one independent handle per eye, derives phase count/jitter/mip bias and evaluates the exact HDR color, motion, whole-depth extent, reset, timing and auto-exposure contract. Production 310.7.0.0 passed two-handle/two-eye hardware evaluation. `Probe` remains unavailable before explicit initialization, so the startup registry cannot select it accidentally and no live game build activates it yet. |
| `Backend/Streamline.h`, `.cpp` | gated | Still a stub, but **M11 is answered and the answer is yes** - see below. `needsDeviceCreation` should become false when a body is written: the probe shows late attachment works. What is not yet shown is that `presentCommon` runs every frame. |
| `Backend/FidelityFxD3D11.h`, `.cpp` | gated | Reports unavailable because no DirectX 11 fork is pinned. Blocked by **M14**. |
| `tools/streamline-probe` | real | Answers **M11** with no game, no headset and no hook: three cases, each in its own process, reporting every `sl::Result` plus what the runtime itself says about DLSS. Only builds when `scripts/fetch-sdks.ps1` has staged the SDK, so the tree still configures with no vendor SDK present. |
| `tools/subrect-harness` | real for the activation contract; subrect optimization deferred | The SDK-free hardware arm remains 11 cases / 208 checks. The optional raw-NGX arm is 17 cases / 272 checks, 0 failures and 2 explicit skips: production `nvngx_dlss.dll` 310.7.0.0 initializes outside a game, two feature handles coexist, both eyes evaluate `R11G11B10_FLOAT` HDR color/output plus `R16G16_FLOAT` motion against distinct origins of one whole stereo `R24G8_TYPELESS` depth resource, readback completes without device removal, and the shipping D3D11.1 isolation restores the broad host sentinel after both evaluates. Only non-zero color/output subrect experiments remain skipped; `supportsSubrects=false` makes them copy-removal work rather than an activation dependency. WARP still covers the SDK-free copy semantics and reports that limitation. |

## Render modules

Seven are real as of 2026-08-30; the rest still name the one measurement that
unblocks them.

| Module | Blocked by | What it does, or will do |
|---|---|---|
| `Engine.h`, `.cpp` | **real** | Resolves the device, immediate context, graphics state and fallback slot, render-target manager and renderer from opcode-verified anchors. Four handles come out of `BSGraphics::Renderer::ClearColor`, whose tail call to `ID3D11DeviceContext` vtable slot 50 types the context slot; the device has its own independently verified global. `Validate()` walks all three target tables and reports. |
| `RenderTargets.h`, `.cpp` | **real** | Resolves a logical target to a live texture, RTV, SRV and UAV, plus its dimensions and DXGI format, and does the same for the depth-stencil array. Plain memory reads: no D3D11 call, nothing AddRef'd. The ownership rule turned out simpler than planned - the engine's record carries the texture directly, so every pointer is borrowed and there is no teardown call at all. |
| `StereoLayout.h`, `.cpp` | **real** | Owns all double-wide geometry and is the only place per-eye rectangles are computed. Uses M5's verified left-first default, honours LR/RL overrides, and fails closed on unresolved, odd or zero geometry. The pure rectangle function is exercised by the standalone harness. |
| `D3D11StateIsolation.h`, `.cpp` | **real** | Owns one private D3D11.1 context-state object and swaps it with the exact state previously active on FO4VR's immediate context. Initialization verifies the device/context pair, immediate-context type, feature level and D3D11.1 interfaces; nested, cross-thread and stale-device use fail closed. The private state intentionally persists between scopes. A normal path may use `Scope`; a vendor boundary that can raise SEH must call `End` from `__finally`. No generic manual fallback is allowed, and exact handback requires no Bethesda state-cache invalidation. |
| `HDRRoundTrip.h`, `.cpp` | **real diagnostics** | Owns the verified top-level HDR callback, exact context-state scope and descriptor-identical private double-wide handback. The first off-by-default gate performs 512 full-resource round trips. The second feeds colour, logical motion 35, logical DS1, one verified native jitter sample and the explicit backend-auto exposure contract through `Evaluate`'s two-eye null control arm for 512 frames. DS1 must resolve with the exact `R24G8_TYPELESS` resource and `R24_UNORM_X8_TYPELESS` depth SRV contract before state isolation begins. Both gates validate route, descriptor, device/context identity and device removal, sit inside explicit SEH cleanup, then become inert. Latest live acceptance before the exposure counters were added: jitter-extended 512/512, all eight phases, 512 matched stereo samples, one prepare/build, zero failures/skips/exceptions/removal/mismatches, then fixed while HDR/TAA advanced 1,259 -> 3,621. |
| `Jitter.h`, `.cpp` | **real native-input path; writes still refused** | Resolves the native jitter state through an opcode-checked LEA, reads one stable sample, requires the exact per-eye dimensions and open native enable gates, converts projection-space scene translation to render-pixel camera jitter, and verifies the result against the engine's eight-phase Halton sequence. `Evaluate` reads once and records identical left/right submissions. Hardware is 10/201 green; live is 512/512 reads and stereo matches, all eight phases observed, zero failures/mismatches. Replacing the native sequence remains refused until a reconstruction backend owns the longer ratio-dependent sequence. |
| `MipBias.h`, `.cpp` | **M8** | Rewrites sampler states with a derived bias and restores them. The restore path matters as much as the apply path. |
| `Intermediates.h`, `.cpp` | **real, live accepted for colour/motion/output/depth reference** | Engine-free descriptor-keyed ownership of two zero-origin colour, two `R16G16_FLOAT` motion and two output textures. Output explicitly adds SRV/UAV binds instead of assuming the engine source carried the UAV required by reconstruction. A full replacement set is built before the old key is released. Shared extract/reassembly helpers validate format, subresource/sample shape and rectangle bounds before issuing copies. D3D11 forbids a partial depth-stencil copy, so `ReferenceDepth` instead validates exact DS1 identity/descriptor and returns the whole borrowed texture plus one eye's extent without a GPU operation. SDK-free LR/RL staging is 11/208; raw NGX accepted the same color/output formats and whole-depth extent shape offline. Colour/motion/output intermediates remain the v1 path while the M9 copy-removal experiment is deferred. |
| `Evaluate.h`, `.cpp` | **real null control; live reconstruction activation still locked** | The per-frame orchestrator and, with `StereoLayout`, the only code that knows there are two eyes. Its accepted live gate still permits only `NullBackend` and native 1:1 colour/output. It extracts measured unjittered current-to-previous motion beside colour, supplies normalized-UV-to-render-pixel scale `(renderWidth, renderHeight)`, references verified standard-Z DS1 with exact left/right extents, reads native jitter once and submits the same render-pixel sample to both eyes, requires null exposure plus backend auto-exposure and exact 1/1 scalars, then reassembles colour into a private target; the engine texture is copied only after both eyes succeed. SDK-free hardware is 11/208 and the raw NGX backend-specific create/evaluate contract is now green offline at 17/272 with two explicit subrect skips. Live activation still requires an explicit runtime-path lifecycle and a separately bounded game gate; it is not implied by compiling the backend. |
| `ResetPolicy.h`, `.cpp` | nothing structural | Nearly real: the request/consume latch works and every reset is logged with its reason. It has no triggers wired because there is no history to reset. |
| `Telemetry.h`, `.cpp` | nothing structural | Per-stage GPU and CPU timing. Reports disabled with a reason because there are no stages to time. The design - a ring of timestamp-disjoint query pairs collected without flushing - must be built, not borrowed; the dev-tooling host has no GPU stage timers on this platform. |
| `Debug.h`, `.cpp` | partial | The request-driven binding observation is real: the render callback never waits, snapshots OM/PS bindings, resolves every bound view back to logical color/depth ids, releases every temporary COM reference and performs no readback. The default point is after native TAA's draw and before cleanup; ClearColor remains available for comparison. DDS target dumping remains prohibited on the unsafe loaded-scene devbench path; a render-thread visualiser is not yet implemented. |

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

**M3 - motion vectors. TAKEN, 2026-08-30.** Logical target `0x23` / 35 is
`SV_Target5` of the six-target deferred MRT and is `R16G16_FLOAT`. Across 318 shipping
deferred PS permutations the write is:

```text
(currentNdc - previousNdc) * (-0.5, +0.5) = previousUV - currentUV
```

The stored direction is current sample to previous-frame source position, in normalized
per-eye UV units. `EYEINDEX*4` selects independent matrix quartets. The executable's
constant-buffer uploader supplies previous/current `viewProjUnjittered` for both eyes, so
the vectors explicitly exclude temporal jitter. The canonical backend seam converts stored
UV to render pixels with `(eyeWidth, eyeHeight)`; a vendor backend then translates that to
its own API convention rather than passing the pixel multipliers blindly.

The old `0x21` / accumulator-group `0x1a/0x1b` velocity claim is withdrawn. Those groups
do bind `0x21`, but the verified native TAA motion resource is `0x23`; the groups' exact
purpose remains open. Full hashes, shader instructions, executable upload offsets and live
six-texture acceptance are in the research ledger's
`SESSION_2026-08-30_MOTION_VECTOR_ENCODING.md`.

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

**M7 - the jitter divisor. TAKEN.** `state+0x78/+0x7c` are integer per-eye width and
height; the full-screen RT-definition path doubles only width after loading it. Native
generation stores projection-space scene translations
`((2*Halton(2,n)-1)/W, (2*Halton(3,n)-1)/H)` and advances an eight-phase counter with
`AND 7`. FO4VR post-multiplies both eye projections by the same fourth-row translation.
The backend seam wants camera jitter in render pixels, so DirectSR's scene/camera and
viewport-Y convention gives `J=(-engineX*W/2, +engineY*H/2)`, equivalently
`(0.5-Halton2, Halton3-0.5)`. The live reader verified `W=1852`, `H=2056`, gates `1/0`,
all eight phases and 512 identical stereo submissions. Writes remain refused because a
reconstruction ratio requires the selected backend's longer sequence; the old
CommonLib-by-value accessor is not used.

**Exposure input. TAKEN, 2026-08-30.** FO4VR computes its terminal 1x1
adapted-luminance ping-pong and updates the persistent tonemap input later inside HDR,
after this integration's pre-HDR evaluate/handback point. That `R11G11B10_FLOAT`
engine resource has not been proved to carry a vendor exposure multiplier, and feeding
its prior value would be frame-late by construction. The v1 seam therefore requires
backend auto-exposure, a null exposure texture and exact `preExposure=1`,
`exposureScale=1`. The harness rejects any drift. Stereo auto-exposure remains an
explicit first-backend measurement risk; a shared engine-derived value requires a
separate transfer-function proof and A/B before admission.

**M8 - the sampler-state table.** Where the engine keeps the sampler states whose
mip LOD bias has to be rewritten and restored.
*Taken by:* Ghidra from the sampler-set call sites.
*Recorded as:* tagged constants. A resolved render ratio is needed too; M5 now
provides the per-eye output width, while backend configuration still has to select
the input size.

**M9 - NGX colour/output sub-rects on D3D11. DEFERRED OPTIMIZATION.** Whether an evaluate with non-zero
colour/output sub-rect bases succeeds and writes only the half it was given. This no
longer governs depth: D3D11 forbids a partial depth copy, and Streamline explicitly
forwards the tagged depth extent to NGX's depth-subrect base parameters.
*Taken by:* `tools/subrect-harness` case 3, with an SDK staged, no game and no
headset. Case 5 is the negative control.
*Recorded as:* a `SOURCES.md` row and `Caps::supportsSubrects`. Nothing in the
research corpus has ever observed such an evaluate succeeding, so the answer is
presumed no and the per-eye copies are the default until this says otherwise.

The raw NGX activation contract is no longer blocked by this measurement. Production
310.7.0.0 accepted two zero-origin eye inputs/outputs and the whole stereo depth resource
with distinct left/right bases. The backend therefore advertises `supportsSubrects=false`
and pays the known per-eye copies; the two remaining harness cases are reported as skips,
not passes. M9 can only remove copies, not change correctness.

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

**M12 - NVIDIA vendor licence terms. TAKEN 2026-08-29; other vendors remain backend-specific.** What Streamline's `license.txt`,
`nvngx_dlss.license.txt` and AMD's `license.md` actually permit.
*Taken by:* fetching the vendor's own published archive and reading them. No
leaked or repacked copy, ever.
*Recorded as:* `THIRD_PARTY.md` rows moving from REPORTED to VERIFIED with the
clause quoted, `sdk-manifest.psd1` entries pinned by URL and SHA-256, and a
decision-log entry for anything that becomes shippable. Statically linking the NGX
library into our DLL is part of this decision, not a build default - which is why
`UPSCALER_ENABLE_NGX` is OFF.

The pinned NVIDIA archive is now staged and read: Streamline source is MIT; the raw NGX
headers, `nvsdk_ngx_d.lib` and DLSS runtime are governed by the NVIDIA RTX SDKs License
and Supplement. `THIRD_PARTY.md` records redistribution, GPL/Linking-Exception,
notification and attribution obligations. AMD and Intel terms remain open until either
backend is selected; they do not block raw NGX development.

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

**Live pre-HDR handback gate - TAKEN, 2026-08-30.** FO4VR's actual feature-level
11.1 device and immediate context created the private state on the same stable thread as
HDR and TemporalAA (`HRESULT 0`). The development-only round-trip gate then copied the
live 3704x2056 `R11G11B10_FLOAT` logical target 1 into one descriptor-matched private
texture and back before vanilla HDR. It completed its exact 512-frame bound with one
resource build, zero route/thread mismatches, failures, skips, SEH exceptions or device
removal. Eight seconds after automatic completion, its counters remained frozen at 512
while HDR and TAA advanced together from 1,532 to 2,558. The deployed test configuration
was restored to false afterward. This proves the live context capability, placement,
resource lifetime, copyback and post-gate vanilla continuation; it does not yet prove a
backend evaluate or per-eye staging.

**Live stereo null gate - TAKEN, extended 2026-08-30.** The mutually exclusive,
off-by-default gate uses the same exact state scope to extract logical colour 1 and logical
motion 35 into two 1852x2056 zero-origin resources each, call independent `NullBackend`
views, and reassemble one private 3704x2056 colour output before copying it back. The
depth extension validates logical DS1 as `R24G8_TYPELESS` with an
`R24_UNORM_X8_TYPELESS` SRV, then supplies the same whole resource with exact left/right
extents because D3D11 forbids a partial depth copy. Exact DLL SHA-256
`105C904F5CACBCA8A3F97D4299B348F6E58706EBA7A81DCB6833D83CF0D0F90F` completed 512
frames with one backend prepare, one six-texture build, 512 left and 512 right evaluates,
and zero declines, skips, exceptions, device removal or route/thread mismatches. After
automatic completion all gate/evaluate/build counters stayed fixed while HDR and TAA
advanced together 1,239 -> 3,811. This takes colour/output orchestration, exact motion
staging, standard-Z depth identity/geometry and private handback; native jitter input is
now also live accepted and exposure is closed offline. Backend activation and temporal
history remain blocked.

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
