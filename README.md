# FO4VR Upscaler

[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=for-the-badge)](LICENSE)

Open-source temporal upscaling and anti-aliasing for Fallout 4 VR, built as an
F4SEVR plugin.

## Status: v0.1.1 - release candidate; DLAA on by default with a DLSS runtime

The shipped configuration reconstructs. `mode = "dlaa"` anti-aliases at the headset's
own resolution on the first world frame the plugin admits, provided you have put
NVIDIA's `nvngx_dlss.dll` where `runtimeDir` points; without that file the log says so
and nothing on screen changes. `mode = "off"` installs the hooks and changes not one
pixel, which is the control arm rather than a broken install.

Start with [TESTING.md](TESTING.md) for installation and the candidate settings.
[KNOWN_ISSUES.md](KNOWN_ISSUES.md) lists the limits; [RELEASE_NOTES.md](RELEASE_NOTES.md) records this candidate.
The archive name and `BUILD.json` identify the candidate label, source commit and hashes.

It is still a development build: it has been through headless regression on every seam
and one operator's headset, not through a public release. The record of what was
measured, in the order it was measured, is `docs/ACCEPTANCE.md`; `docs/MODULE_MAP.md`
lists every module, whether it is real or a stub, and the one measurement that unblocks
each stub. Nothing gets filled in with a plausible guess.

## Requirements

- Fallout 4 VR, version 1.2.72. The plugin refuses any other binary and says so
  in the log.
- F4SEVR.
- VR Address Library for F4SEVR.
- An NVIDIA RTX GPU, and NVIDIA's own `nvngx_dlss.dll` - a production build, not a
  watermarked development one; 310.7.0.0 is the version tested here. Get it from
  NVIDIA's own distribution. The plugin loads it by explicit path and refuses a
  different copy that is already loaded in the process, so a stray DLL elsewhere
  cannot quietly win a bug report.
- The Microsoft Visual C++ x64 redistributable. The release DLL links NVIDIA's client
  library, which is built against the dynamic CRT, so ours is too. The game already
  requires it.

The optional FSR build compiles a pinned MIT D3D11 fork and loads no AMD runtime.
It is experimental, not included in this candidate, and never an automatic fallback.
A separately built FFX backend can be selected explicitly; reduced modes there
currently resolve to native AA. See `THIRD_PARTY.md` for every component and who supplies it.

## Install

Install the archive as a normal MO2 mod, or extract its contents into the game's
`Data` directory (so `Data/F4SE/Plugins/fo4vr_upscaler.dll` exists). The `-mod.7z` download contains:

```
F4SE/Plugins/fo4vr_upscaler.dll
F4SE/Plugins/fo4vr_upscaler.pdb
F4SE/Plugins/Fo4VRUpscaler.toml
LICENSE.txt       (GPL and linking/modding exceptions)
NOTICES.txt       (consolidated third-party notices)
SOURCE.txt        (exact binary-source commit and source offer)
BUILD.json        (binary and packaging identity, file hashes)
```

The matching `-docs.zip` contains this README, installation/testing instructions,
the checklist, known issues, release notes and technical records. Extract it
somewhere convenient; **do not install the documentation ZIP into MO2**. The PDB
stays with the main mod so crash reports can identify our code. Neither package
contains NVIDIA's runtime.

Then create the runtime folder and put NVIDIA's DLL in it - the archive does not create
it, because no vendor binary ships here:

```
F4SE/Plugins/Fo4VRUpscaler/nvngx_dlss.dll
```

## Configuration

**Two files.** `Data/F4SE/Plugins/Fo4VRUpscaler.toml` is the shipped one, read relative
to the game's working directory, so under MO2 the virtual copy in your mod folder is
the one that wins. Anything changed through the mod itself is written instead to
`Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscalerOverride.toml`, which holds only the
keys that changed and wins per key over the shipped file - so a mod update that
replaces the shipped file leaves your settings alone. Delete the override to go back to
the defaults. A malformed override is ignored at load, and while it is malformed no new
value can be written.

**`mode`** is the switch: `off`, `dlaa`, `quality`, `balanced`, `performance`,
`ultraperformance`, default `dlaa`. `dlaa` reconstructs at full resolution; the four
reduced modes name a render ratio, least to most reduced. `ultraperformance` (0.3333) has a structural startup check, but broad headset
acceptance is missing. Quality F and DLAA K have the main current visual evidence;
other modes are exploratory candidate coverage, not equivalent acceptance.

Which of those changes can be made on a running session:

| From | To | |
|---|---|---|
| anything | `off` | live, always |
| `off` | the mode this session was armed with | live |
| one reduced mode | another reduced mode | live; the new ratio is written into the engine while the world gate briefly denies admission |
| `dlaa` | any reduced mode | restart - whether the reduced-world bracket exists at all is fixed for the process |
| a reduced mode | `dlaa` | refused; go through `off` and restart |

**`backend`** - `auto` (the default) resolves to `ngx`. Restart to change. The startup
line reads `backend selection null` on every launch, healthy or not: that probe runs
before a D3D11 device exists and NGX cannot answer without one. The render thread's own
initialisation is the real verdict, and it logs the runtime path and version.

**`preset`** - `auto` or one letter, case-insensitive, one value for every mode; a
per-mode preset is not a key yet. The shipped and compiled value is `K`, matching
the default DLAA K setup. It also remains K when selecting a reduced mode; choose
F explicitly for Quality F or a lower-cost preset. `auto` leaves the choice to NVIDIA's runtime/driver.
The log reports the requested hint and parameter readback; it cannot prove the
driver's internal model choice or detect every driver override.

**`sharpness`** - `0.0` to `1.0`, default `0.5`, live. Our own RCAS pass at the seam
where the frame is published, because the engine's TemporalAA sharpens its output and
the replacement draw would otherwise drop that term. `0` is off and bit-identical to no
pass. Sharpening after temporal reconstruction amplifies whatever the reconstruction got
wrong: raise it last, not first.

**`renderScaleOverride`** - `0` (the default) means the chosen mode's own ratio, snapped
so the engine lands on the size the runtime asks for. Any value in `0.30` to `1.00`
applies verbatim to whichever reduced mode is running, so `mode = "balanced"` with `0.5`
renders at `0.5`. It is live once a reduced session is running, and ignored entirely by
`dlaa`, whose ratio is 1.0.

**Hotkeys** are Win32 virtual-key codes and are **bound by default**: `hotkeyToggleVk =
106` is keypad `*` and switches between `off` and this session's armed mode;
`hotkeyCompareVk = 111` is keypad `/` and renders native for as long as it is held,
which is the A/B; `hotkeyPanelVk = 122` is `F11` and shows or hides the in-headset panel
below. Set any of them to `0` to unbind. All three are read only while Fallout 4 VR owns
the foreground window and no text-input menu is open, so a key pressed in another
application does nothing here.
Common codes: `106` keypad `*`, `107` keypad `+`, `109` keypad `-`, `111` keypad `/`,
`112`-`123` are `F1`-`F12`.

## In-headset panel

`F11` (`hotkeyPanelVk`) opens a small panel about a metre ahead of you and a little below
eye level. It is a SteamVR overlay, so the world keeps rendering behind it - that is the
whole point of it being an overlay rather than a menu.

The panel is placed **in the room**, not on your head: when you open it, it appears in
front of wherever you were looking at that moment, standing upright, and it stays there
while you look around. Open it again and it is placed again, in front of you. Set
`panelAnchor = "head"` for the older behaviour, where it hangs in front of your face and
follows every glance.

While the panel is up, SteamVR's laser owns the controllers: the game gets no controller
input at all, so the pause menu and the Pip-Boy will not open, and `F11` is only read
while the game window has the desktop's focus, which with a headset on it usually does
not. That is what the large **Close** button at the bottom is for, and the header says so.
It is the way out.

At the top it shows status: the live mode and where it came from, the reconstruction
backend, the preset asked for and the one the runtime answered with, whether the session
is running, suspended or stopped and why, and the last line the mod put on the HUD.

Below that are the controls. Point a controller at the panel and the SteamVR laser
clicks them.

- **Mode.** Six buttons - off, DLAA, quality, balanced, performance, ultra. A button
  with no note under it takes effect the moment you click it. A button that cannot take
  effect this session gets a note saying `next launch: <reason>`, and clicking it still
  writes the setting, so the next time you start the game you get what you asked for.
  Which ones those are depends on how the session started: a session that started in a
  reduced mode can never reach DLAA without a restart, and a session that started native
  can never reach a reduced mode without one.
- **Ratio.** A slider from `0.30` to `1.00` over `renderScaleOverride`, in steps of
  `0.01`, with the render size it landed on beside it. It is written once per drag, and
  only after you have held still for a fifth of a second - a trigger reads as several
  releases on the way up, and each one would otherwise be a settings write. `1.00` beside
  a reduced mode is a mode change rather than a ratio, and the panel says so before you
  try it.
- **use mode ratio.** A ratio you set here outlives a mode click and a restart: click
  quality after moving the slider and quality runs at the ratio you set, not at its own.
  The row says `override standing` while one is, and this button clears it -
  `renderScaleOverride` goes back to `0` and the mode's own ratio takes over.
- **Preset.** `auto`, F, J, K, L, M. K is the sharper one and costs about two
  milliseconds a frame more than F - measured in the headset on an RTX 4090 at
  3760x3996 per eye, where DLAA ran 10.39 ms a frame on F and 12.35 ms on K.
- **Sharpness.** A slider from `0` to `1` in steps of `0.05`, written the same way as the
  ratio: once per drag, a fifth of a second after you stop moving it.
- **A/B.** `Hold for native` renders the world native for as long as you hold the
  button, and reconstructs again when you let go - the same comparison the
  `hotkeyCompareVk` key does, without a keyboard. `Native` beside it latches the same
  thing on until you click it again. Letting go, hiding the panel or turning the panel
  off all end a hold, and turning the panel off ends the latch as well - a panel that is
  gone has no control left to clear one with. Hiding it does not: the latch is a latch.
- **timers** turns `perfTimers` on and off, and the pane at the bottom shows the
  per-stage costs while it is on.
- The three hotkey codes are shown read-only.
- **Close** hides the panel and gives the controllers back to the game.

The last line is the result of the last thing you changed, in the words the settings
writer itself used: the key, the value you asked for, whether it was accepted, whether
it was written to disk, whether it took effect now or needs a restart, and the reason
when there is one. While a change is still being applied it reads `pending`. One click
may wait behind the one being applied, so a double click on the laser is not lost -
clicking the same button twice keeps the second value - but a third inside one change is
refused, and that refusal is named on the same line until the next change replaces it. If
the panel's settings worker could not be started at all, the line says so instead, on
every frame: nothing you click can be written this session, and `Fo4VRUpscaler.log` says
why.

A mode you click here takes the same care a mode change made any other way takes: the
world stops being reconstructed for the moment the new configuration publishes, the
temporal history is thrown away rather than carried across the change, and the world is
re-admitted only after it has been stable again for a full sixteen frames.

`panelWidthMeters` (default `0.6`), `panelDistanceMeters` (default `1.0`, now measured in
the room at the moment you open it) and `panelAnchor` (default `"room"`, or `"head"`)
place it; `panelEnabled = false` removes it entirely and unbinds the key. While it is
hidden it costs nothing per frame. If your runtime serves no overlay interface - an
OpenXR translation layer might not - the panel reports itself unavailable in
`Fo4VRUpscaler.log`, names the runtime's own error, and everything else runs unchanged.
If it serves overlays but will not answer for the headset's pose, that show is
head-anchored instead and the log names the interface that was missing.

**HUD lines** appear for four things: the mode going active on the first admitted world
frame, the outcome of a mode switch (applied, unchanged, refused with a reason, or an
unrecognised name), reconstruction suspending or stopping for the session, and a startup
check that found a problem. At most one line per second reaches the game. There is no
key to turn them off.

**`failSoft`** (default `true`) lets a recoverable fault suspend and re-arm
reconstruction instead of ending it for the session; `false` reproduces the older
behaviour, one fault and done, which is how you bisect whether a re-arm caused something
a bug report describes.

**`[Diagnostics]`** contains regression gates, logging controls and rendering fixes.
The thirteen bounded regression gates are off by default; arming one forces `mode`
to `off` and names the conflict. Other keys in this table can coexist with normal
play: `resolveDepthAfterSeam` defaults on to correct reduced-resolution HUD
occlusion. `diagnosticDynamicResolution` feeds only the `mode = "off"` diagnostic
path and is ignored whenever a product mode is active.

**Camera reconstruction scale.** `[Diagnostics] compositeReconstructionScale` is a
live correction enabled for candidate testing for the audited DFComposite cubemap family at reduced
resolution. The legacy-named `technique65600ReconstructionScale` additionally
enables the audited non-cubemap camera effects. The family table covers 141
reconstruction records with per-record buffer and coordinate contracts; the
reference candidate passed the recorded mountain, reflection and scope checks;
broader coverage remains open. Results and limits are preserved in [the acceptance record](docs/ACCEPTANCE.md).

Both keys default on in this candidate. The runtime checks the full
expected registry and actual bindings but cannot fingerprint live shader bytes;
replacement shader packages need separate qualification. Native and DLAA draws
pass through. Enabling the correction may turn on D3D11 threading protection
until game exit, with a possible CPU cost after switching the correction off.

`compositeCorrectionDiagnostics` is a separate live setting, default false.
Leave it off for normal play; the correction does not need evidence capture.

**Restart semantics.** Six keys need the game restarted: `enabled`, `devBench`,
`backend`, `forceHiddenAreaMeshOff`, `diagnosticDynamicResolution` and
`diagnosticContinuousFrames`. Everything else takes effect on the next reload, and
`/config` reports each key's own policy rather than leaving you to guess.

Settings are parsed, range-checked and reported by the dev tooling. A missing key
keeps its default, an unknown key is named in the log rather than ignored, and a key of
the wrong type is named and the default kept. Nothing in either file can crash the game.
Settings are re-read at load, when the game finishes loading, on an accepted change from
the dev tooling, and on request.

## Building

CMake, vcpkg, and the [rollingrock/CommonLibF4](https://github.com/rollingrock/CommonLibF4)
VR fork as the `external/CommonLibF4` submodule.

```
git clone --recurse-submodules https://github.com/rollingrock/Fo4VR-Upscaler
cd Fo4VR-Upscaler
pwsh -File scripts/fetch-sdks.ps1
cmake --preset vs2022-windows-vcpkg-vr
cmake --build buildvr --config Release
```

Output lands in `buildvr/Release`. Set both `VCPKG_ROOT` and
`VCPKG_INSTALLATION_ROOT` - different files in this chain read different names, and
missing one of them fails the configure in a way that does not name itself.
`CMakeUserPresets.json` (git-ignored) is where a local deploy path goes.

`UPSCALER_ENABLE_NGX` is ON by default, so the configure needs NVIDIA's SHA-256-pinned
official archive staged into the git-ignored cache by `fetch-sdks.ps1`, and it fails
with that instruction if it is not there. That links NVIDIA's client library, so its
licence and release obligations in `THIRD_PARTY.md` apply, and the DLL uses the dynamic
MSVC runtime that library requires. Nothing in `.sdk-cache` is ever packaged.

The SDK-free build needs no vendor account and no staged runtime:

```
cmake --preset vs2022-windows-vcpkg-vr-nongx
cmake --build buildvr-nongx --config Release
```

That is a requirement, not a convenience: a build only the author can perform is not
open source, and it is what CI runs on every push. Every backend compiles either as its
real implementation or as a stub that reports itself unavailable with a reason, decided
by a compile definition, so a disabled backend still type-checks against the interface
and cannot rot. `UPSCALER_ENABLE_NGX` is a cached option, so switching an existing tree
between the two needs a clean configure, not just a new flag.

## Reporting problems

Log: `Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler.log`. Include it in every
report. It carries the version in its header, the full startup environment report
- game version, module base, resolved paths, which interacting modules are present
and which are absent, every compiled-in backend with its availability and the
verbatim reason when it is unavailable - plus live ClearColor and TemporalAA
counts, thread identities and mismatch counts. `logLevel = 3` under `[Diagnostics]`
logs every frame decision, at a measurable cost; it exists so a report can carry a
trace instead of a guess.

If the game crashed, include the Buffout crash log too; the shipped `.pdb` is what
makes it name our code.

If DLAA does not engage, two lines say why. The render thread's own
`ngx backend: unavailable - ...` line carries the runtime's reason verbatim: a missing
`nvngx_dlss.dll` in `runtimeDir` reads `ngx backend: unavailable - nvngx_dlss.dll is
absent from the explicit runtime directory`. It is logged when the backend initialises,
which is well after the startup report. The in-headset `startup check` line reports
whether the engine, the device, the stereo geometry and the hooks resolved. The
load-time lines - `backend selection null`, and any `backend ngx unavailable - a
device-bound probe is required` - are answered before a D3D11 device exists and are not
a verdict on the runtime: see `backend` above.

### Development only

The thirteen bounded regression gates under `[Diagnostics]` are off by default.
Arming one forces `mode` to `off`; each stops after its own bound, and the log and
`/state` identify the active gate. The rendering fixes documented above and the
cubemap evidence setting below are separate from those mode-forcing gates.

`compositeCorrectionDiagnostics=true` records a shader census and up to four
source/private-buffer captures per correction episode. It can run alongside a
product Quality mode. Diagnostic off/on pauses and resumes the same episode;
correction off/on starts a fresh one. `/state` reports
`dynamicResolutionDispatch.compositeCorrectionDiagnosticsEnabled`, capture counts,
and the bounded preliminary census. With diagnostics off, those samples do not
advance; correction and binding-restoration counters still do. Captures can stall
the render thread, so leave this off when measuring ordinary performance.

`diagnosticNgxHandback` and `diagnosticFfxHandback` deliberately change exactly one
frame; `diagnosticNgxContinuous` and `diagnosticFfxContinuous` change exactly
`diagnosticContinuousFrames` frames after 16 private warm-up evaluations, the default
being the accepted 512-frame regression window. `diagnosticFfxEvaluate` evaluates one
native-AA stereo frame into private FSR outputs and tears the backend down on the next
HDR frame without reassembly or engine-colour writes. `diagnosticTaaReplacement`
overwrites both native-TAA outputs for the same bound after the native draw and can be
coupled exactly to consecutive accepted NGX or FSR continuous handbacks;
`diagnosticTaaBypass` uses the same pass for that bound instead of the native prepared
draw. Each coupled episode snapshots its selected source's accepted serial before the
pre-HDR gate is enabled, so stale process-lifetime copy counts cannot admit a frame.

## Provenance and disclosure

This project is built as *documented independent implementation with disclosure*.
The full rules of engagement are in the research repository's
`CLEAN_ROOM_PROTOCOL.md`; `SOURCES.md` here is the per-subsystem ledger and
`THIRD_PARTY.md` the component ledger. What that means in practice:

**GPL-3.0 references.** The project licence permits reading and reusing
`alandtse/open-shaders`, `northaxosky/fallout4-community-shaders` and OptiScaler
with attribution. Those that have been read are read for design only, and each one
has a row in `SOURCES.md` naming the files read and the ideas taken - which, for a
VR binary, is a list of functions to re-find rather than a specification to port.
`fallout4-community-shaders`'s SHA-256-pinned vendor-fetch mechanism is followed
here as the shape of an idea, and any script that follows it carries an SPDX header
crediting it. No line of any of their code is in this repository. Where a line of a
permitted reference is ever used, the file carries an SPDX header, both copyrights
and a `Derived from` line, and gets a row in `THIRD_PARTY.md`.

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
those rules, and every constant in it carries its tag and the one-line derivation
naming what it was decoded from. Nothing is copied from another repository's notes.

## License

Licensed under **GPL-3.0-or-later** with the project
[Modding and Linking Exceptions](EXCEPTIONS.md). See [LICENSE](LICENSE) for the
full text, and [THIRD_PARTY.md](THIRD_PARTY.md) for third-party components and
which runtime files you must supply yourself.

Copyright (C) 2026 rollingrock and Fo4VR-Upscaler contributors.

The complete corresponding source for any release is this repository:
`https://github.com/rollingrock/Fo4VR-Upscaler`.
