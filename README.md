# FO4VR Upscaler

[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=for-the-badge)](LICENSE)

Open-source temporal upscaling and anti-aliasing for Fallout 4 VR, built as an
F4SEVR plugin.

## Status: v0.1.0 - live renderer instrumentation, still no changed pixels

This build installs one fail-soft observation hook at
`BSGraphics::Renderer::ClearColor`. It preserves the complete original function,
creates no D3D11 resources and changes not one pixel. The hook is a temporary
render-thread rendezvous for measurements, explicitly not the final upscaler
injection point.

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

No vendor runtime is needed for this build, because nothing calls one yet. When a
backend exists, its runtime will be user-supplied - see `THIRD_PARTY.md`.

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

Four keys are honoured today: `enabled`, `logLevel`, `devBench` and `backend`.
Everything else - `failSoft` included, which is reported but branched on nowhere
yet - is parsed, range-checked, live-reloadable and reported by the dev tooling,
and is read by nothing. Each of those keys names the measurement that switches it
on. A missing key keeps its
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

## Reporting problems

Log: `Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler.log`. Include it in every
report. It carries the version in its header, the full startup environment report
- game version, module base, resolved paths, which interacting modules are present
and which are absent, every compiled-in backend with its availability and the
verbatim reason when it is unavailable - plus live hook count, thread identity and
whether the observation point ever ran on a second thread.

If the game crashed, include the Buffout crash log too; the shipped `.pdb` is what
makes it name our code.

Because this build changes no pixels on purpose, "I see no difference" is expected
behaviour rather than a bug. The log will say why.

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
