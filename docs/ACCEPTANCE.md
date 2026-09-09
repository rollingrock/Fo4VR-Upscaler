# Acceptance record

The paragraphs below accumulated in `README.md` while this project was proving one
render seam at a time, and they are kept here as the chronological record rather than
as a description of the current build. They are written in the tense they were written
in; where a statement was overtaken by later work, the sentence says so instead of
being deleted, because what was measured and when is the point of the record.

`docs/MODULE_MAP.md` is the current-state document: every module, whether it is real or
a stub, and the one measurement that unblocks each stub. `README.md` is the user
document. This file is neither.

## From research build to product path

This build installs four fail-soft observation hooks. The temporary
`BSGraphics::Renderer::ClearColor` rendezvous services a request-driven OM/PS
snapshot on its primary steady-state thread. The native
`ImageSpaceEffectTemporalAA::Render` probe measures the exact temporal stage's call
count, thread and effect identity. A direct call hook inside the generic image-space
pixel wrapper samples bindings after vanilla draws and before cleanup. Scoped by the
effect-dispatch hook, it now resolves HDR effect 3's final draw as well as native TAA.
Every site byte-verifies before patching and preserves original execution. The first
world HDR call creates a private D3D11.1 context-state object without activating it.
The default configuration at that point still changed not one pixel. A
development-only, off-by-default gate has also completed 512 descriptor-matched logical-1 -> private ->
logical-1 copy round trips before vanilla HDR, with exact host-state restoration and no
device removal. The next bounded gate now exercises the shipping stereo path too:
logical 1 is extracted into two 1852x2056 zero-origin eyes, each eye is submitted to an
independent `NullBackend` view, and the reassembled 3704x2056 private output is handed
back before vanilla HDR. It completed 512/512 live frames with zero failures, skips,
exceptions or device removal, then became inert while HDR/TAA continued in lockstep.
The optional raw-NGX build has now passed five independent live gates. In `RedRocketExt`
it loaded the exact configured 310.7.0.0 runtime, allocated the six 1852x2056 per-eye
intermediates and created two native-DLAA histories on FO4VR's real D3D11 device. The
second gate extracted live HDR color and motion, referenced both eye extents in the whole
DS1 depth resource, submitted one verified native jitter sample and evaluated both NGX
histories exactly once. Both vendor outputs remained private: no reassembly target
existed and no engine color was copied. The third gate repeated that exact frame and
reassembled both outputs into the private descriptor-matched 3704x2056 texture. Direct
left/right reassembly counters reached 1/1, no copy into logical color 1 existed, and the
gate stayed inert while vanilla HDR/TAA advanced 30 -> 1,938. All gate, evaluate,
exposure, jitter, state, HDR/TAA route/thread and device-health counters stayed clean. The
fourth gate then issued exactly one descriptor-matched copy from that accepted private
texture into logical color 1 before vanilla HDR. Its direct engine-copy counter reached 1;
HDR/TAA completed on the same thread and advanced 2 -> 1,928 while every bounded vendor,
copy and failure counter stayed fixed. The identical one-shot gate then ran three times
through the real headset path at 3760x3996. A transient Red Rocket foliage/dark load
observation did not reproduce at `SanctuaryExt`, which the observer judged indistinguishable
from a normal load; no persistent or stereo-specific artifact was seen. That accepts gross
one-shot visual safety. The seventh diagnostic then preserved both histories through 16
private frames and 512 consecutive handbacks at 7520x3996 double-wide; the observer reported
the full `SanctuaryExt` window looked normal. A subsequent null-driver run crossed
`SanctuaryExt` -> `RedRocketExt`, submitted three paired history resets, completed the same
528 evaluations and tore down both histories and the NGX SDK session on the render thread.
Native TAA originally still ran afterward. The next headless gate coupled those exact
512 NGX handbacks to a format-converting post-TAA output overwrite. It completed 512/512
writes to both the compositor target
and the active history, split exactly 256/256 across histories 61/62, with zero binding,
descriptor, isolation or device-health failures. The 16 private NGX warm-ups remained
native TAA, the next 512 handbacks were aligned one-for-one, and both gates then became
inert while HDR/TAA continued. The same pass now has a separately gated true bypass arm:
the fresh coupled real-headset repeat completed and the observer reported that it looked
normal with no specific issue. The bypass resolves Bethesda's byte-verified history parity,
requires the engine-selected 61/62
output to agree, binds logical 3/history/submitted directly and skips the prepared helper.
Standalone and NGX-coupled null-driver runs each bypassed exactly 512 native draws, split
histories 256/256, with zero parity, target, descriptor, isolation or device failures.
The coupled run aligned exactly after 16 warm-ups and survived normal NGX teardown; both
gates stayed inert while HDR/TAA advanced another 1,118 frames. The observer's fresh
coupled headset pass looked normal. A render-thread capture instrument then saved native,
first-bypass and settled handback-64 left/right eyes headlessly; the settled pair was
non-dark, stereo-coherent and free of clipping, eye swap, seams or corruption. A neon-green
actor marker appeared under both bypass and later vanilla TAA, proving it was scene/UI
content rather than a reconstruction artifact. The deployed game configuration changed no
pixels by default at that point; `mode` replaced that arrangement in Phase 4.

The NGX path has since passed a longer unattended acceptance run: 16 private warm-ups plus
32,768 consecutive two-eye handbacks and matching native-TAA bypasses across
`SanctuaryExt -> RedRocketExt -> SanctuaryExt`, six paired history resets, an exact
16,384/16,384 history split and clean 1/1/0 render-thread teardown. Every vendor, device,
state, route, parity and source-serial failure counter remained zero, and the completed
episode stayed inert while native HDR/TAA advanced another 697 frames. A live-reload
negative control also proved that the configured frame bound latches when an episode starts.
In a controlled headset comparison the tester reported no NGX smear or stereo mismatch,
but distant foliage remained softer than native TAA, especially in motion. Creation-time
NGX preset hints now accept `auto` or one letter; an explicit-J process passed 16+4,096
frames and clean teardown, although static captures did not show a sharpness advantage over
auto. The next native-resolution visual discriminator is therefore auto versus J in
motion; DLSS quality modes use the separate reduced-raster path below.

The diagnostic quality path owns FO4VR's 0.67 render ratio, feeds packed reduced eyes to
two NGX histories and hands full-resolution eyes back before HDR. Its first headset tests
showed cropped and triangular regions. Two defects had been superimposed: FO4VR's
hidden-area mesh was still drawn under reduced ratios, so the triangles were already in the
vendor input, and reduced ratios were owned outside a proven world frame, so menus and
loading inherited them. The world-gated transaction now arms at startup, disables the
hidden-area mesh and applies reduced ratios only after sixteen stable world frames, and
restores native ratios and the hidden-area byte at the late ImageSpace seam before the
staged pixels are published. A fresh menu-to-world run admitted at exactly sixteen stable
frames with balanced ownership and no violations, a 20-stage capture was byte-identical from
private output to engine handback, and one physical Quest run showed coherent world stereo
with no wedge, crop or flash. Per-eye HUD compass clipping, physical frame timing and
subjective reconstruction quality remained open after that run, and the reduced path
still submitted the engine's 8-phase jitter with no mip bias. Both were closed
afterwards: the selected backend owns the sequence, at `ceil(8 * (output / render)^2)`
of the live ratio, and the mip bias is applied inside the world bracket.

The engine-free staging path is also real. It extracts either stereo order into private
zero-origin color, finite camera-completed motion and `R32_FLOAT` depth, then reassembles
the two outputs without mutating the engine-owned inputs. The SDK-free hardware arm was 20 cases / 2,847 checks at that point; it is
21 / 2,928 today. The optional native-D3D11 FSR build verifies and compiles the exact
clean `alandtse/FidelityFX-SDK-DX11` commit `e65b253`, creates one direct FSR 3.1 upscaler
context per eye and is 18 / 2,127 with no failures, skips or device removal. Its hardware
soak preserves both histories through the same 16 + 512 stereo-frame bound used by the
game gate, exercises a paired mid-window reset, proves distinct populated eye outputs and
then tears down, recreates and evaluates again. FO4VR has separately accepted private
evaluation, private reassembly and exactly one engine handback headlessly; the same
one-shot artifact received an affirmative normal-view controller verdict through the real
headset path. The FFX backend is built only with `-DUPSCALER_ENABLE_FFX=ON`; otherwise the DLL
carries its fail-soft stub. The NGX arm was green at 29 / 3,724 with no skips, and is
30 / 3,805 today - the former colour/output-subrect skips are now the
answered sub-rect cases - and its full live/headset evidence above is unchanged. The
bounded FSR game gate then passed a
fresh-process SteamVR-null `SanctuaryExt` run: 16/16 private warm-ups, 512/512 engine
handbacks and copies, 528/528 stereo evaluations and reassemblies per eye, paired reset
1/1 and safe teardown 1/1/0. It remained inert while HDR/TAA advanced another 4,381 calls
in lockstep; every gate, exposure, route, thread, exception and device-health counter stayed
clean, and all backend ownership/resources returned to null. Controlled headset quality
comparison remained open at that point. The source-tagged FFX-to-TAA continuation has now
closed the structural output seam headlessly as well. Fresh post-draw and true-bypass
processes each completed 528/528 successful FFX frames: 16/16 private warm-ups followed by
512/512 consecutive handbacks aligned one-for-one with 512/512 TAA-mode successes and split
256/256 across histories 61/62. Both runs reported zero gate, evaluate, reassembly, route,
thread, validation, parity, target, descriptor, isolation or device-health failures, tore
down at 1/1/0 and stayed frozen while native HDR/TAA continued. Accepted-FFX captures selected
post-draw serial 69 and bypass serial 66; both submitted-eye pairs were visually complete and
stereo-coherent. Against each fresh process's phase-0 static `QASmoke`/Tunnels native pair
(timescale 0, AI off, yaw 0), submitted-eye SSIM was 0.954680/0.954229 left/right for
post-draw and 0.952128/0.951149 for bypass. Those are structural similarity controls, not a
temporal-quality ranking. In a third fresh process, an uncoupled FFX episode first reached
serial 512; the later coupled episode in that same process snapshotted that stale baseline,
then accumulated 1,033 TAA calls and 1,033 no-handback skips with zero replacement attempts
or successes. The later controlled comparison reported FSR head-motion smear and possible
stereo mismatch, so FSR remains a secondary backend pending quality work, and the product
path never selects it: `mode` drives `backend = "ngx"` or `auto`, and an FFX handback is
refused per frame while the engine rasterizes a reduced viewport.

That is the point. The harness - settings, live reload, logging, fault latches,
the address ledger and its provenance rules, the dev tooling, the packaging - was
proven before the first engine hook was written. `docs/MODULE_MAP.md` lists every
module, whether it is real or a stub, and the one measurement that unblocks each
stub. Nothing gets filled in with a plausible guess.

## Cubemap reconstruction: 2026-09-09

**Accepted scope:** the Quality cubemap magnification defect in the controlled
SanctuaryExt profile, with the physical headset. The final diagnostic cleanup
also passes an independent source review, both Release builds and a fresh
diagnostics-off runtime check. This is not general shader-package or material
coverage, and the shipped correction default remains false.

### Cause and correction

VERIFIED from the installed VR shader package: the qualified DFComposite shaders
reconstruct the reflection direction using full-resolution pixel coordinates
without the reduced raster ratio. Earlier patches changed rows 20-27, including
the view-to-world rotation, instead of the reconstruction matrices. They also
had incorrect Y and right-eye translations and no demonstrated GPU ordering for
an in-place NO_OVERWRITE patch/restore.

The replacement copies the complete 1,360-byte camera buffer into a private
version and corrects only whole finite reconstruction matrices: far rows 32-39
and near rows 40-47. Rotation rows 20-22 and all other bytes remain unchanged.
For eye `e`, the affine terms are `sx=1/rx`, `sy=1/ry`,
`tx=(1+2e)*(sx-1)`, `ty=1-sy`; each matrix row becomes
`(x*sx, y*sy, z, w+x*tx+y*ty)`. Invalid source or candidate matrices retain
all 64 original bytes. The compute pass validates the live size/stereo inputs
and viewport basis before applying the transform.

The private buffer is bound only around the actual prepared-packet helper at
`141DA51E0`, caller return `141D98901`. The exact original CB12 pointer and
D3D11.1 range are restored and verified. D3D11 multithread protection and an
Enter/Leave bracket cover preparation, binding, the original helper and restore.
The engine's original camera buffer is never mapped or written by the correction.

The first corrected implementation admitted only DFComposite technique 40.
It briefly appeared correct in the headset, then its count stopped at 92; the
later shader in that run was not captured. A follow-up census measured no
technique-40 sightings and sustained reduced-raster draws using 65576 and 64.
Fresh disassembly qualified 65576: its reconstruction/reflection instructions
match 40, with only additional t11/t12 lighting samples and additions around
them. Both exact IDs are now admitted; 64 remains excluded because it does not
sample the cubemap despite retaining a binding.

The qualified package is `Shaders012_VR.fxp`, SHA-256
`EEA46BB93C047C359451B8852A4AB013A1444D67FE8974813F5C456EF7B7D4EE`,
with the complete 183-record DFComposite family. Technique 65576 maps to DXBC
offset `C9EFF0`, 3,488 bytes, SHA-256
`2D53EC097987377FEE290729F9585B8A32564C9865A482A4EA44ABBE65F22CA7`.
This is conditional package-to-record qualification; live PS bytecode was not
hashed, and the plugin does not enforce that package hash at startup.

### Validation

| Gate | Result and boundary |
|---|---|
| Private-buffer implementation | Independent source review; seven exact-kernel GPU cases with no debug warnings; both builds; NGX harness 30 cases / 3,816 checks and SDK-free harness 21 cases / 2,928 checks passed. |
| Initial ID40 headless controls | Eight captures verified both finite far matrices, exact near-NaN/other-byte preservation, live basis and private/original binding. Off, rearm and native-veto controls passed; 18,274 private helper calls/restores, zero restore failures. |
| Physical Quality acceptance, PID50668 | Four actual-65576 captures passed the independent byte/coordinate checker. The acceptance snapshot recorded 4,590 private helper calls and exact restores, with zero restore failures. The wearer reported "fixed!" after sustained correction. |
| Physical run closeout | 28/30 ordinary checks passed. The final snapshot was blocked by `pipboy-menu`; world admission failed and the mipbias probe timed out. The game exited normally and restored configuration. This runner result remains distinct from the visual pass. |
| Optional diagnostics cleanup, PID29140 | Source review and both Release builds passed. With correction on and diagnostics off, helper calls/restores increased from 419 to 2,100, readiness was true, and captures/census samples stayed zero. No restore failures or terminal uncertainty. All 29/29 ordinary checks passed; normal exit and configuration restoration passed. |

The wearer-accepted diagnostic DLL has SHA-256
`D5FABC3B98AE5FBDDE7B8979D2A7443B96B0B193F4D3EDB687E11C3C1E6115B7`.
The subsequent diagnostics-off cleanup DLL has SHA-256
`3F54637BB0FA54A4B6663F30A0F309736326CACCD54C94B8208304DEC69CCE37`.
Its correction kernel, identity gates and binding/restoration checks are unchanged.
The embedded HLSL SHA-256 is
`927E0CA6C9703D8E2DF7CA1C82DCEB83726B6DD7BA0545F0A930A51D0FF9F346`.

Both `compositeReconstructionScale` and `compositeCorrectionDiagnostics` default
false in the shipped configuration. Only the tested MO2 profile explicitly
enables correction, with diagnostics off; its prior DLAA startup mode and all
unrelated settings were preserved. The production MO2 installation was untouched.

### Remaining limits

- The captured near matrices were non-finite baseline data and were preserved
  exactly. Their valid-use branch and uncaptured matrix masks remain unmeasured.
- Other shader packages, permutations, materials and quality modes need broader
  coverage before general default-on promotion.
- Enabling threading protection affects the context until process exit. Existing
  timing samples do not isolate its cost or the correction's cost.
- A separate exterior-cell transition defect briefly falls back to native during
  the 16-frame world-admission stabilization window. It is not closed by this fix.

Provenance is in [SOURCES.md](../SOURCES.md). Detailed local evidence is retained
in the research repository's `SESSION_2026-09-09_CUBEMAP_AUDIT_CORRECTIONS.md`,
`SESSION_2026-09-09_CUBEMAP_DIAGNOSTIC_IMPLEMENTATION.md`, and
`artifacts/cubemap-diagnostic-20260909/`, especially `id65576-audit/`,
`stage-d-headset/`, `stage-e-normal-use/` and `stage-e-smoke/`.
