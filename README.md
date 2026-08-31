# FO4VR Upscaler

[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=for-the-badge)](LICENSE)

Open-source temporal upscaling and anti-aliasing for Fallout 4 VR, built as an
F4SEVR plugin.

## Status: v0.1.0 - sustained NGX/DLAA accepted; non-DLAA resolution ownership remains

This build installs four fail-soft observation hooks. The temporary
`BSGraphics::Renderer::ClearColor` rendezvous services a request-driven OM/PS
snapshot on its primary steady-state thread. The native
`ImageSpaceEffectTemporalAA::Render` probe measures the exact temporal stage's call
count, thread and effect identity. A direct call hook inside the generic image-space
pixel wrapper samples bindings after vanilla draws and before cleanup. Scoped by the
effect-dispatch hook, it now resolves HDR effect 3's final draw as well as native TAA.
Every site byte-verifies before patching and preserves original execution. The first
world HDR call creates a private D3D11.1 context-state object without activating it.
The default configuration still changes not one pixel. A development-only,
off-by-default gate has also completed 512 descriptor-matched logical-1 -> private ->
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
content rather than a reconstruction artifact. The deployed game configuration changes no
pixels by default.

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
auto. The next visual discriminator is therefore auto versus J in motion, while actual
DLSS quality modes still require ownership of the game's render resolution and jitter.

The engine-free staging path is also real. It extracts either stereo order into private
zero-origin color, finite camera-completed motion and `R32_FLOAT` depth, then reassembles
the two outputs without mutating the engine-owned inputs. The SDK-free hardware arm is
15 cases / 978 checks. The optional native-D3D11 FSR build verifies and compiles the exact
clean `alandtse/FidelityFX-SDK-DX11` commit `e65b253`, creates one direct FSR 3.1 upscaler
context per eye and is 18 / 2,122 with no failures, skips or device removal. Its hardware
soak preserves both histories through the same 16 + 512 stereo-frame bound used by the
game gate, exercises a paired mid-window reset, proves distinct populated eye outputs and
then tears down, recreates and evaluates again. FO4VR has separately accepted private
evaluation, private reassembly and exactly one engine handback headlessly; the same
one-shot artifact received an affirmative normal-view controller verdict through the real
headset path. The default build contains only its fail-soft stub. The optional NGX arm
remains green at 22 / 1,051 with two explicit, non-blocking color/output-subrect skips, and
its full live/headset evidence above is unchanged. The bounded FSR game gate then passed a
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
stereo mismatch, so FSR remains a secondary backend pending quality work; normal activation
remains open.

That is the point. The harness - settings, live reload, logging, fault latches,
the address ledger and its provenance rules, the dev tooling, the packaging - was
proven before the first engine hook was written. `docs/MODULE_MAP.md` lists every
module, whether it is real or a stub, and the one measurement that unblocks each
stub. Nothing gets filled in with a plausible guess.

## Requirements

- Fallout 4 VR, version 1.2.72. The plugin refuses any other binary and says so
  in the log.
- F4SEVR.
- VR Address Library for F4SEVR.

No vendor runtime is needed for the default build. The optional raw NGX build requires a
user-supplied production `nvngx_dlss.dll` and refuses to select a different copy by DLL
search order. The optional FSR build instead compiles an exact clean external source clone
of the pinned MIT D3D11 fork; it loads no AMD runtime. See `THIRD_PARTY.md`.

## Install

Install the archive as a normal MO2 mod, or extract it into the game folder. It
contains:

```
F4SE/Plugins/fo4vr_upscaler.dll
F4SE/Plugins/fo4vr_upscaler.pdb    (keep it - it makes crash logs name our code)
F4SE/Plugins/Fo4VRUpscaler.toml
```

## Configuration

`Data/F4SE/Plugins/Fo4VRUpscaler.toml`, read relative to the game's working
directory, so under MO2 the virtual copy in your mod folder is the one that wins.

All 40 keys are parsed, range-checked and reported by the dev tooling. Most are
live-reloadable; `enabled` and `devBench` are startup-only. The 13 development-only
diagnostic switches and the implemented lifecycle/compatibility controls are honoured.
Remaining staged tuning keys - `failSoft` included - name the
measurement that will switch each one on rather than silently pretending to work. A
missing key keeps its
default, an unknown key is named in the log rather than ignored, and a key of the
wrong type is named and the default kept. Nothing in this file can crash the game.

Settings are re-read when the game finishes loading, and on request from the dev
tooling.

## Building

CMake, vcpkg, and the [rollingrock/CommonLibF4](https://github.com/rollingrock/CommonLibF4)
VR fork as the `external/CommonLibF4` submodule.

```
git clone --recurse-submodules https://github.com/rollingrock/Fo4VR-Upscaler
cd Fo4VR-Upscaler
cmake --preset vs2022-windows-vcpkg-vr
cmake --build buildvr --config Release
```

Output lands in `buildvr/Release`. Set both `VCPKG_ROOT` and
`VCPKG_INSTALLATION_ROOT` - different files in this chain read different names, and
missing one of them fails the configure in a way that does not name itself.
`CMakeUserPresets.json` (git-ignored) is where a local deploy path goes.

The tree builds green with no vendor SDK present. That is a requirement, not a
convenience: a build only the author can perform is not open source. Every backend
compiles either as its real implementation or as a stub that reports itself
unavailable with a reason, decided by a compile definition, so a disabled backend
still type-checks against the interface and cannot rot.

For the optional raw NGX research build, stage the SHA-256-pinned official NVIDIA archive
into the git-ignored cache and configure a separate, non-deploying tree:

```
pwsh -File scripts/fetch-sdks.ps1
cmake --preset vs2022-windows-vcpkg-vr -B buildvr-ngx -DBUILD_TOOLS=ON -DUPSCALER_ENABLE_NGX=ON -DMO2_INSTALL_PATH=
cmake --build buildvr-ngx --config Release
buildvr-ngx\tools\Release\subrect_harness.exe
```

This links NVIDIA's optional client library, so its licence and release obligations in
`THIRD_PARTY.md` apply. Nothing in `.sdk-cache` is packaged.

## Reporting problems

Log: `Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler.log`. Include it in every
report. It carries the version in its header, the full startup environment report
- game version, module base, resolved paths, which interacting modules are present
and which are absent, every compiled-in backend with its availability and the
verbatim reason when it is unavailable - plus live ClearColor and TemporalAA
counts, thread identities and mismatch counts.

If the game crashed, include the Buffout crash log too; the shipped `.pdb` is what
makes it name our code.

Because the default configuration transforms no pixels, "I see no difference" is
expected behaviour rather than a bug. The private/read-only development gates preserve
visible engine color. `diagnosticNgxHandback` and `diagnosticFfxHandback` deliberately
change exactly one frame; `diagnosticNgxContinuous` and `diagnosticFfxContinuous` change
exactly `diagnosticContinuousFrames` frames after 16 private warm-up evaluations. The
default remains the accepted 512-frame regression window; larger values provide bounded
unattended soaks, and the active target cannot change after the episode begins.
`diagnosticFfxEvaluate` evaluates
one native-AA stereo frame into private FSR outputs and tears the backend down on the next
HDR frame without reassembly or engine-colour writes;
`diagnosticTaaReplacement` overwrites both native-TAA outputs for the same bound after the
native draw and can be coupled exactly to consecutive accepted NGX or FSR continuous
handbacks; `diagnosticTaaBypass` uses the same pass for that bound instead of the native
prepared draw. Each coupled episode snapshots its selected source's accepted serial before
the pre-HDR gate is enabled, so stale process-lifetime copy counts cannot admit a frame.
Every gate is off by default and stops after its bound; the log and `/state` say explicitly
which ran.

## Provenance and disclosure

This project is built as *documented independent implementation with disclosure*.
The full rules of engagement are in the research repository's
`CLEAN_ROOM_PROTOCOL.md`; `SOURCES.md` here is the per-subsystem ledger and
`THIRD_PARTY.md` the component ledger. What that means in practice:

**GPL-3.0 references.** The project licence permits reading and reusing
`alandtse/open-shaders`, `northaxosky/fallout4-community-shaders` and OptiScaler
with attribution. Of those, only `fallout4-community-shaders` has been read, and
only its licence files, its packaging and SDK-fetch scripts, and its list of
engine anchors - which, for a VR binary, is a list of functions to re-find rather
than a specification to port. Its SHA-256-pinned vendor-fetch mechanism is
followed here as the shape of an idea, and any script that follows it carries an
SPDX header crediting it. No line of its code is in this repository. Where a line
of any permitted reference is ever used, the file carries an SPDX header, both
copyrights and a `Derived from` line, and gets a row in `THIRD_PARTY.md`.

**Closed prior art.** A closed-source temporal upscaler for Fallout 4 VR already
exists. None of it is used here. No binary of it has been disassembled,
decompiled, debugged or API-traced, and its symbol file has never been opened.
Its author separately publishes permissively licensed sources for a different
game; a summary of those was read once, on 2026-08-29, during unrelated
compatibility research for another mod and before this repository existed. That is
recorded rather than concealed, and it is why this project claims documented
independent implementation and not pure clean-room. Nothing from that reading is
used: the implementation derives from our own analysis of `Fallout4VR.exe`, from
vendor documentation, and from the GPL-3.0 references above. Reading those
repositories is off by default going forward.

**Convergent design.** Some choices are forced from outside - by the engine, by
Direct3D 11, or by a vendor's integration guide. Where our design lands where
someone else's also landed, the source comment names the constraint that forced
it and `SOURCES.md` carries the row. A named constraint is the whole defence; the
same code with no explanation is not.

**Engine constants.** Every RVA, offset and original-byte string that describes
`Fallout4VR.exe` carries a verification tag and a one-line derivation naming the
function or instruction it was decoded from. `src/Upscaler/Addresses.h` ships with
those rules and no constants at all, and it stays that way until values are read
from this binary. Nothing is copied from another repository's notes.

## License

Licensed under **GPL-3.0-or-later** with the project
[Modding and Linking Exceptions](EXCEPTIONS.md). See [LICENSE](LICENSE) for the
full text, and [THIRD_PARTY.md](THIRD_PARTY.md) for third-party components and
which runtime files you must supply yourself.

Copyright (C) 2026 rollingrock and Fo4VR-Upscaler contributors.

The complete corresponding source for any release is this repository:
`https://github.com/rollingrock/Fo4VR-Upscaler`.
