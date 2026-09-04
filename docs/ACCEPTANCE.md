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
