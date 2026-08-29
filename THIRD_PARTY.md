# Third-party components

Every third-party component this project uses, what it is used for, and whether it
ships in our release archive or you supply it yourself. `SOURCES.md` is the
internal provenance ledger; this file is the public face of it.

Licence claims carry a status. **VERIFIED** means someone on this project read the
stated licence text or manifest first-hand and the path is given. **REPORTED**
means the value is secondhand and has not been read here - it must be read before
that component ships or is relied on. Nothing here is promoted from REPORTED to
VERIFIED without a path to the text that was read.

## Compiled into `fo4vr_upscaler.dll`

| Component | Upstream | Version | Licence | Status | How used | Licence text |
|---|---|---|---|---|---|---|
| CommonLibF4 (rollingrock fork) | github.com/rollingrock/CommonLibF4 | submodule `a3f0713e49eb86d30d471431f42a4f5bf730bc79` | MIT | VERIFIED - `external/CommonLibF4/LICENSE`, "Copyright (c) 2019 ryan-rsm-mckenzie" | The F4SE interface layer, address resolution, trampolines, RE types. Statically linked. | `external/CommonLibF4/LICENSE` |
| devbench cross-plugin API glue | github.com/alandtse/devbench | copied from `C:/repos/devbench/include` | MIT | VERIFIED - `src/DevBenchClient/DevBenchAPI.LICENSE.txt`, "Copyright (c) 2026 Alan Tse and devbench contributors" | `DevBenchAPI.h` / `.cpp`, the C-ABI used to register our dev tooling with a devbench host. The only files in the tree under a licence other than ours. | `src/DevBenchClient/DevBenchAPI.LICENSE.txt` - the MIT notice must travel with the code |
| spdlog | vcpkg port `spdlog` | resolved by manifest baseline `b4a3d89` | MIT | VERIFIED - port manifest `license` field | Logging, both through the `logger::` alias and directly in the backend library. | vcpkg installs the upstream notice at `share/spdlog/copyright` |
| tomlplusplus | vcpkg port `tomlplusplus` | baseline `b4a3d89` | MIT | VERIFIED - port manifest | Parsing `Fo4VRUpscaler.toml`. | `share/tomlplusplus/copyright` |
| xbyak | vcpkg port `xbyak` | baseline `b4a3d89` | BSD-3-Clause | VERIFIED - port manifest | Runtime code generation behind `F4SE_SUPPORT_XBYAK` and `pstl::asm_replace`. | `share/xbyak/copyright` |
| rsm-mmio | vcpkg port `rsm-mmio` | baseline `b4a3d89` | MIT | VERIFIED - port manifest | CommonLibF4 dependency. | `share/rsm-mmio/copyright` |
| args | vcpkg port `args` | baseline `b4a3d89` | MIT | VERIFIED - port manifest | CommonLibF4 dependency. | `share/args/copyright` |
| rapidcsv | vcpkg port `rapidcsv` | baseline `b4a3d89` | BSD-3-Clause | VERIFIED - port manifest | CommonLibF4 dependency. | `share/rapidcsv/copyright` |
| boost-stl-interfaces | vcpkg port | baseline `b4a3d89` | BSL-1.0 | VERIFIED - port manifest | Declared in `vcpkg.json`, inherited from the True Scopes manifest. | `share/boost-stl-interfaces/copyright` |
| robin-hood-hashing | vcpkg port | baseline `b4a3d89` | MIT | VERIFIED - port manifest | Declared in `vcpkg.json`, inherited from the True Scopes manifest. | `share/robin-hood-hashing/copyright` |
| rsm-binary-io | vcpkg port | baseline `b4a3d89` | MIT | VERIFIED - port manifest | Declared in `vcpkg.json`, inherited from the True Scopes manifest. | `share/rsm-binary-io/copyright` |
| srell | vcpkg port | baseline `b4a3d89` | BSD-2-Clause | VERIFIED - port manifest | Declared in `vcpkg.json`, inherited from the True Scopes manifest. | `share/srell/copyright` |
| frozen | vcpkg port | baseline `b4a3d89` | Apache-2.0 | VERIFIED - port manifest | Declared in `vcpkg.json`, inherited from the True Scopes manifest. | `share/frozen/copyright` |
| nowide | vcpkg port | baseline `b4a3d89` | not declared by the port | REPORTED - the port manifest carries no `license` field; read `share/nowide/copyright` before a release | Declared in `vcpkg.json`, inherited from the True Scopes manifest. | `share/nowide/copyright` |

Licence values marked VERIFIED against a port manifest were read from
`$VCPKG_ROOT/ports/<port>/vcpkg.json` in the local vcpkg checkout. That is vcpkg's
declaration, not the upstream text; the authoritative notice is the `copyright`
file the port installs, and the release process is what must carry it.

The ports inherited from the True Scopes manifest are declared but not yet
referenced by any of our source. They are listed because they are resolved and
linked by the build, not because we call them. Removing an unused one is a
`vcpkg.json` change with a rebuild, not a licence question.

## Build and test only, not in the shipped DLL

| Component | Upstream | Licence | Status | How used |
|---|---|---|---|---|
| Catch2 | vcpkg port `catch2` | BSL-1.0 | VERIFIED - port manifest | `tools/subrect-harness` only. The harness is not part of a release. |
| vcpkg | github.com/microsoft/vcpkg | MIT | REPORTED - not read on this project | Dependency resolution at configure time. Ships nothing. |

## Runtime components you supply

None of these ship in our release archive. The plugin loads what is present by
explicit path, logs the resolved path and the version the runtime reported, and
runs without any of them.

| Component | Where it comes from | Licence | Status | Disposition |
|---|---|---|---|---|
| Fallout 4 VR (`Fallout4VR.exe` 1.2.72) | Steam | Bethesda EULA | REPORTED | The host process. Nothing of it is copied or redistributed. |
| F4SEVR | f4se.silverlock.org | F4SE terms | REPORTED - not read on this project | Loads the plugin. We link no F4SE code: CommonLibF4 supplies its own interface layer. |
| VR Address Library for F4SEVR | Nexus | REPORTED | REPORTED | Address data F4SEVR/CommonLibF4 consume. Not redistributed. |
| `openvr_api.dll` | shipped with the game | BSD-3-Clause | REPORTED - not read on this project | Present in the process. Not redistributed. |
| NVIDIA NGX / DLSS runtime (`nvngx_dlss.dll`, `nvngx_dlssg.dll`) | NVIDIA's own distribution, or your driver | NVIDIA proprietary, own `nvngx_dlss.license.txt` | REPORTED - the licence text has not been read on this project | **User-supplied.** Never redistributed. Flipping this to shipped is a decision-log entry quoting the clause that permits it, not a default. |
| NVIDIA NGX static libraries (`nvsdk_ngx_s.lib`) | NVIDIA RTX SDK download | NVIDIA proprietary | REPORTED - not read on this project | **Not linked by default.** `UPSCALER_ENABLE_NGX` is OFF, because statically linking vendor object code into the shipped DLL is a redistribution decision, not a build default. No NGX header or library is committed. |
| NVIDIA Streamline (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, other `sl.*.dll`) | NVIDIA-RTX/Streamline releases | framework reported MIT; the DLSS plugin is not | REPORTED - `license.txt` has not been read on this project | **User-supplied.** No Streamline backend is built today. |
| AMD FidelityFX runtime (`amd_fidelityfx_*.dll`) | AMD's own distribution | AMD FidelityFX SDK licence, own `license.md` | REPORTED - not read on this project | **User-supplied** pending reading the terms. |
| FidelityFX DirectX 11 community fork (source) | to be pinned | MIT | REPORTED - no fork pinned yet | Would be built from source, which is why it is the only backend that could produce a self-contained archive. Not pinned, not built. |
| Intel XeSS runtime (`libxess.dll`) | Intel's own distribution | Intel licence | REPORTED - not read on this project | **User-supplied.** No XeSS backend exists. |

Vendor binaries come from the vendor's own distribution or they do not come at
all. Repacked, leaked or otherwise unauthorised copies of any vendor runtime are
not supported. `scripts/sdk-manifest.psd1` pins any vendor archive we fetch by URL
and SHA-256; it currently has zero entries, because no vendor licence text has been
read by this project and entering a URL before reading the terms is the wrong
order.

## Reference implementations read

The project licence permits reading and reusing GPL-3.0 sources with attribution.
These have been read or are permitted to be read. Where a line of any of them is
ever used, it gets a row here and an SPDX header on the file that carries it.

| Reference | Licence | What was taken |
|---|---|---|
| `alandtse/open-shaders` | GPL-3.0 | Nothing yet. Permitted reading. |
| `northaxosky/fallout4-community-shaders` | GPL-3.0 | Nothing yet. Permitted reading. The vendor-SDK fetch-and-pin mechanism follows its shape; any script that does so carries an SPDX header crediting it. |
| OptiScaler | GPL-3.0 | Nothing yet. Permitted reading. |
| `alandtse/devbench` | GPL-3.0, with the two API glue files MIT | The two MIT glue files, vendored with their licence. The route-registry design follows the shape of the idea only; no GPL-3.0 devbench source is vendored. |

`README.md` carries the full disclosure, including the closed prior art that
exists and is not used.

## Notes

- `vcpkg.json` declares `"license": "GPL-3.0-or-later"` and nothing more. Neither
  the Modding Exception nor the Linking Exception has a registered SPDX exception
  identifier, so `GPL-3.0-or-later WITH ...` would not be a valid SPDX expression
  and manifest validation would reject it. The base identifier is correct and
  complete; `EXCEPTIONS.md` is authoritative for the additional permissions. Do
  not "fix" the field.
- Reference clones live outside this tree, at `C:/repos/*-reference`. Compatible
  is not the same as ours, and the provenance ledger only works if the boundary
  is physical.

## Known gaps

Recorded because an unstated gap is the thing this file exists to prevent.

1. There are two archive producers and only one is conveyance-safe.
   `scripts/package-release.ps1` is the audited path: it stages `LICENSE`,
   `EXCEPTIONS.md`, this file, `SOURCE.txt` (repository, commit and a GPL-3 §6(b)
   written offer) and a `licenses/` directory collected from
   `vcpkg_installed/<triplet>/share/<port>/copyright` plus
   `DevBenchAPI.LICENSE.txt`, and refuses to produce an archive if any of them is
   missing. The `if(PACKAGE)` CMake target is the quick path for handing a build
   to a tester: it carries the licence trio but no `licenses/` and no
   `SOURCE.txt`, so it must not be what gets uploaded.
2. No vendor SDK licence text has been read by this project. Every vendor row
   above is REPORTED for that reason, and no row may be flipped to "shipped"
   until its clause is read and quoted.
