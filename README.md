# FO4VR Upscaler

[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=for-the-badge)](LICENSE)

Open-source temporal upscaling and anti-aliasing for Fallout 4 VR, built as an
F4SEVR plugin.

## Status: v0.1.0 - bounded raw-NGX history accepted in-headset; reset/lifecycle headless accepted

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
The optional raw-NGX build has now passed four independent live gates. In `RedRocketExt`
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
Native TAA still runs afterward, so this is not yet the production replacement. The
deployed game configuration changes no pixels by default.

The engine-free staging path is also real now. The hardware harness allocates the
shipping descriptor-keyed per-eye textures, extracts both LR and RL layouts, submits
each eye independently through `NullBackend`, and reassembles the double-wide output.
The SDK-free arm passes 11 cases / 208 checks on the hardware adapter, and the same
staging path has passed its bounded live null gate in the game. An optional raw NGX D3D11
build now passes its engine-free hardware gate against NVIDIA's production
`nvngx_dlss.dll` 310.7.0.0: two independent DLAA handles evaluate both
`R11G11B10_FLOAT` eyes with `R16G16_FLOAT` motion and distinct extents in one whole
stereo D24S8 depth resource. Readback completes with no device removal, and the shipping
D3D11.1 isolation restores the broad host-state sentinel after both evaluations. That
arm reports 17 cases / 272 checks, zero failures and two explicit, non-blocking
color/output-subrect skips. One private-output frame has now been reassembled and copied
back in the game. The supervised bounded-history headset check and headless load/reset/
teardown boundary are accepted; native-TAA replacement remains the next live boundary.

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
search order; see `THIRD_PARTY.md`.

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

Fifteen keys are honoured today: `enabled`, `logLevel`, `devBench`, `backend`, `runtimeDir`,
`eyeOrder`, `resetOnMenu`, `resetOnLoad`, and the seven development-only diagnostic switches.
Everything else - `failSoft` included, which is reported but branched
on nowhere yet - is parsed, range-checked, live-reloadable and reported by the dev
tooling, and is read by nothing. Each of those keys names the measurement that
switches it on. A missing key keeps its
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
expected behaviour rather than a bug. Five development gates preserve visible engine
color; `diagnosticNgxHandback` deliberately changes exactly one frame, and
`diagnosticNgxContinuous` changes exactly 512 frames after 16 private warm-up evaluations.
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
