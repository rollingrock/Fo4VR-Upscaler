# Fo4VR-Upscaler - agent rules

Open-source temporal upscaling and anti-aliasing for Fallout 4 VR. F4SEVR plugin,
GPL-3.0-or-later with the Modding and Linking Exceptions.

Read `docs/STYLE.md` before writing a comment or a line of code, and
`docs/MODULE_MAP.md` before touching anything under `src/Upscaler/`. The module
map is what stops a session filling a stub with a plausible guess: every stub
names the one measurement that unblocks it, and until that measurement exists the
stub stays a stub.

## 1. The protocol wins

`CLEAN_ROOM_PROTOCOL.md` in the research repo is the authority on what may be
read, reversed and reused. Its authoritative copy is machine-local and
unpublished, at `C:/repos/fo4vr-upscaler-dlss-research/CLEAN_ROOM_PROTOCOL.md`;
`README.md` here carries the public disclosure that the protocol requires. If
anything in this repo appears to conflict with the protocol, the protocol wins and
the conflict gets flagged rather than resolved silently. Deviations are appended
to its §7 decision log, dated - that log is append-only and a silent deviation is
the one failure it exists to prevent.

Never load, decompile, debug or API-trace any closed prior-art binary, and never
open the symbol file that ships beside one - the protocol names the file, and it
is a complete symbol dump that would hand us that mod's internals for free.
Running such a mod as a user to check whether something crashes is a different
activity and is allowed; a symbolicated crash log naming its internals answers
"did it crash?" and nothing else.

## 2. No fabricated addresses

No RVA, offset, original-byte string, struct field offset or render-target
identity enters this tree unless it was read out of `Fallout4VR.exe` in the
session that adds it. Not from another repository's notes, not from a flat
Fallout 4 address-library id, not from arithmetic on a flat-game offset.

`src/Upscaler/Addresses.h` defines the tag vocabulary and the derivation rules in
its own header comment, and is the single copy of them - do not restate them here
or in `docs/STYLE.md`. What this file adds is the gate: a constant with no tag and
no one-line derivation naming the function or instruction it came from is
rejected in review. A stub that needs an address says so in a comment and returns
a failure. It does not invent one.

## 3. Ghidra

Every session, before any analysis call:

```
list_instances()                      # discover running Ghidra processes
connect_instance("fallout4vr_pdb")    # the annotated FO4VR project
list_open_programs()                  # confirm image base 140000000
```

The analysis tools do not exist until `connect_instance` runs. Always connect
explicitly: the bridge auto-discovers every running Ghidra, an unrelated project
is sometimes open, and analysis calls that land on the wrong binary return
confident nonsense. Never accept a project upgrade prompt - stop and ask.

**Read the disassembly for any global address, never the decompiled argument.**
This project's decompiler prints wrong globals - it renders
`LEA RCX,[0x1438ac010]` as `&DAT_14384fd30`, and `0x146239340` as
`&DAT_1461e0900`, at four-plus sites, reproduced independently by separate
readers. It appears to resolve to a nearest-preceding symbol rather than the
operand. A wrong base does not fault; it returns plausible values, which is the
one failure mode this project cannot absorb. It nearly cost us the render-target
properties base.

Name findings with a `US_` prefix so a later session can tell our work from the
tool's guesses. **Every batch appends a row to `SOURCES.md` under "Game binary
analysis" before `save_program`** - the `US_` symbol, what was needed, and the
function or instruction it was decoded from. An un-recorded finding is a lost
finding, and the ledger is what makes the disclosure posture auditable.

## 4. The three cmake list files

Every `.cpp` and `.h` is listed in exactly one of `cmake/headerlist.cmake`,
`cmake/sourcelist.cmake` or `cmake/backendlist.cmake`. A new source file that is
not listed silently does not build, with no warning - the classic mistake in this
chain. Listing a file in two of them is a duplicate-symbol link failure. Backend
files go in `backendlist.cmake` only: that library links neither CommonLibF4 nor
F4SE, so the harness can exercise a backend with no game running.

## 5. Build and deploy

```
cmake --preset vs2022-windows-vcpkg-vr
cmake --build buildvr --config Release
```

Both `VCPKG_ROOT` and `VCPKG_INSTALLATION_ROOT` must be set. Output is
`buildvr/Release/fo4vr_upscaler.dll`. Set `MO2_INSTALL_PATH` in
`CMakeUserPresets.json` (git-ignored) to deploy to a test instance on build.
`UPSCALER_ENABLE_NGX` is ON by default and the configure fails if
`scripts/fetch-sdks.ps1` has not staged NVIDIA's archive. With
`-DUPSCALER_ENABLE_NGX=OFF` (preset `vs2022-windows-vcpkg-vr-nongx`, and what CI
runs) the tree must configure and build with no vendor SDK staged; if a change
breaks that, the change is wrong.

## 6. Commits

No AI-attribution trailers, ever. No `Co-Authored-By`, no "Generated with", in
commit messages, tags or PR bodies. Re-read the message before committing and
confirm neither line is there. `docs/STYLE.md` has the rest of the format,
including the `Source:` line that any commit introducing a technique carries.

## Target

| | |
|---|---|
| Game | Fallout 4 VR `1.2.72`, x64 |
| Binary | `H:/SteamLibrary/steamapps/common/Fallout 4 VR/Fallout4VR.exe` |
| Renderer | Direct3D 11, Creation Engine (BSGraphics) |
| VR runtime | OpenVR / SteamVR; OpenComposite optionally translates to OpenXR |
| Loader | F4SEVR, VR Address Library |
| DLL / TOML / log | `fo4vr_upscaler.dll` / `Fo4VRUpscaler.toml` / `Fo4VRUpscaler.log` |

The D3D11 constraint is the defining technical fact of this project: most modern
temporal upscaling SDKs are DX12/Vulkan-first.

## Prior work worth mining before starting anything

These are large - grep, do not read end to end.

- `C:/repos/Fo4VR-True-Scopes` - the author's shipped FO4VR render-path mod and
  the template this repo follows.
- `C:/repos/fallout4-scope-in-scope-investigation` - the True Scopes RE record:
  deferred resolve, render-target identities, ImageSpace path, camera commit.
  Facts to re-verify here, not constants to paste.
- `C:/repos/fo4vr-upscaler-dlss-research` - this project's feasibility research,
  the protocol, and the requirements.
- `C:/repos/devbench` - the dev tooling host and the canonical copy of the MIT
  glue vendored under `src/DevBenchClient/`.
