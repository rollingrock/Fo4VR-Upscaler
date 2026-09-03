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
| NVIDIA NGX D3D11 client library | NVIDIA's official Streamline release archive | `nvsdk_ngx_d.lib` from Streamline v2.12.0; SHA-256 `31E82B4EC3242EC6E5B42C73E1F5F5E260338A6EE213BD64444F6C2FA364AA84` | NVIDIA RTX SDKs License (v. March 14, 2024) plus RTX Supplement | **VERIFIED** - the staged `nvngx_dlss.license.txt` explicitly governs the NGX SDK; source archive SHA-256 `f5c0a3d8...dcf48` | **Optional and OFF by default.** `UPSCALER_ENABLE_NGX=ON` links the vendor client object code into the plugin and therefore uses the dynamic MSVC runtime required by that library. No NVIDIA header or library is committed or separately packaged. The project's Linking Exception prevents GPL terms reaching the SDK. | `nvngx_dlss.license.txt` staged from `bin/x64/nvngx_dlss.license.txt` by `scripts/fetch-sdks.ps1` |
| devbench cross-plugin API glue | github.com/alandtse/devbench | copied from `C:/repos/devbench/include` | MIT | VERIFIED - `src/DevBenchClient/DevBenchAPI.LICENSE.txt`, "Copyright (c) 2026 Alan Tse and devbench contributors" | `DevBenchAPI.h` / `.cpp`, the C-ABI used to register our dev tooling with a devbench host. The only files in the tree under a licence other than ours. | `src/DevBenchClient/DevBenchAPI.LICENSE.txt` - the MIT notice must travel with the code |
| AMD FidelityFX FSR 1.0 RCAS kernel | AMD FidelityFX SDK, `sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h` (`FsrRcasCon`, `FsrRcasF`, `FSR_RCAS_LIMIT`), read from the DX11 fork clone at `C:/repos/FidelityFX-SDK-DX11-reference` | fork commit `e65b2530631f2afb9a9ac753884926e49e20d608` | MIT | VERIFIED - `docs/license.md` in that clone, "Copyright (c) 2023 Advanced Micro Devices, Inc." | The algorithm and its structure, re-expressed in our own HLSL as the TemporalAA-seam sharpen pixel shader in `src/Upscaler/SharpenShader.h`: the five-tap cross, luma times two, ring min/max, the two clip limiters with the lower-limiter multiplier, the lobe clamp and scale, the resolve, and `exp2(-stops)`. No line vendored; the shader string ships inside the DLL. | The MIT notice and a statement of what was taken are in the header comment of `src/Upscaler/SharpenShader.h` |
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
| NVIDIA NGX / DLSS runtime (`nvngx_dlss.dll`) | NVIDIA's own distribution | **NVIDIA RTX SDKs License** (v. March 14, 2024) plus its RTX Supplement | **VERIFIED** - read from `bin/x64/nvngx_dlss.license.txt` inside NVIDIA's own `streamline-sdk-v2.12.0.zip`, SHA-256 `f5c0a3d8...dcf48`. See the clause notes below. | **User-supplied.** The raw backend requires an explicit directory, refuses a different already-loaded basename, and verifies/logs the final absolute path and file version after feature creation. The hardware gate used the production 310.7.0.0 DLL (SHA-256 `BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E`) from that archive. Redistribution is permitted under conditions (§1c, §2a-d), so keeping it user-supplied is a project choice. |
| NVIDIA Streamline framework, **source** | NVIDIA-RTX/Streamline v2.12.0 | **MIT** | **VERIFIED** - `license.txt` at the archive root is a verbatim MIT licence, "Copyright (c) 2023 NVIDIA CORPORATION" | Not built today. MIT means we could build `sl.interposer`, `sl.common` and `sl.dlss` from source ourselves and ship the results under MIT. |
| NVIDIA Streamline, **prebuilt signed** `sl.*.dll` | same archive, `bin/x64/` | **not separately stated** | **OPEN** - the archive carries exactly two licence files beside binaries, `nvngx_dlss.license.txt` and `nis.license.txt`; none names the `sl.*` DLLs. The README says every plugin except DLSS-G has source, which makes MIT the likely reading - but likely is not read | **User-supplied** until this is settled. Shipping NVIDIA's signed prebuilts and shipping our own MIT build of the same source are different decisions. |
| AMD FidelityFX signed runtimes (`amd_fidelityfx_*.dll`) | AMD FidelityFX SDK 2.3.0 | AMD binary redistribution licence | **VERIFIED** - `docs/license.md` applies binary-only redistribution by default and requires notice retention; the native backend list is DX12, GDK and Vulkan | Not useful to the selected D3D11 route. A future D3D11-to-D3D12 bridge would keep these user-supplied until separately accepted. |
| FidelityFX DirectX 11 community fork (source) | `alandtse/FidelityFX-SDK-DX11` commit `e65b2530631f2afb9a9ac753884926e49e20d608` | MIT | **VERIFIED** - exact root `LICENSE.txt` and per-file notices read from a clean external clone; this exact commit is also pinned by the permitted `alandtse/open-shaders` reference | Selected and implemented optional source route. CMake refuses any other commit or tracked modification, builds the direct FSR 3.1 upscaler plus D3D11 backend from the external clone, and leaves the default build SDK-free. The MIT notice must accompany a release containing that optional object code. |
| Intel XeSS D3D11 runtime (`libxess_dx11.dll`) | Intel XeSS SDK 3.0.2 | Intel Simplified Software License, October 2022 | **VERIFIED** - the official `LICENSE.txt` permits use and unmodified binary redistribution with copyright/licence retention and no endorsement; reverse engineering and modification are prohibited | **User-supplied.** No XeSS backend exists. The official D3D11 path is Intel Arc-only and cannot be accepted on this RTX 4090 host. |

Vendor binaries come from the vendor's own distribution or they do not come at
all. Repacked, leaked or otherwise unauthorised copies of any vendor runtime are
not supported. `scripts/sdk-manifest.psd1` pins any vendor archive we fetch by URL
and SHA-256, and now has its first entry: Streamline v2.12.0, whose terms are read
and recorded above.

### The NVIDIA RTX SDKs License, clause by clause

Read 2026-08-29 from the vendor's own archive. These are the clauses that decide
what this project may do, quoted rather than paraphrased.

**It permits redistribution.** §1(c) grants the right to "Distribute any software
and materials within the SDK ... **as incorporated in object code format into a
software application**", subject to §2. §2(a) requires that "An application must
have **material additional functionality**, beyond the included portions of the
SDK" - a render-path integration plainly does. §2(c) requires distributing "subject
to the terms at least as protective as the terms of this license", which is why
`fetch-sdks.ps1` refuses to stage a runtime without its licence text.

**§4(b) forbids standalone redistribution:** "you may not distribute or sublicense
the SDK as a stand-alone product." Inside a mod with material additional
functionality, that is satisfied; a "DLSS files" archive on its own is not.

**§4(e) is the clause that matters to a GPL project.** "You may not use the SDK in
any manner that would cause it to become subject to an open source software
license. As examples, licenses that require as a condition of use, modification,
and/or distribution that the SDK be: (i) disclosed or distributed in source code
form; (ii) licensed for the purpose of making derivative works; or (iii)
redistributable at no charge."

GPL-3.0 requires exactly those things of a combined work. **This is precisely what
our Linking Exception exists to prevent**: it stops our licence reaching the vendor
binary, so §4(e) can be satisfied. Without that exception the two licences would be
in direct conflict, and the project would have no lawful way to ship a DLSS runtime
at all. `EXCEPTIONS.md` is therefore load-bearing here, not decorative.

**Supplement §4 is an action item, not a constraint.** "You are required to notify
NVIDIA **prior to commercial release** of an application (**including a plug-in to
a commercial application**) that incorporates, or is based on, the DLSS SDK ...
Please send notifications to: https://developer.nvidia.com/sw-notification and
provide ... company name, publisher and developer name, NVIDIA SDK used,
application name, platform, scheduled ship date, and weblink to product/video."

A Nexus mod is a plug-in to a commercial application. Whether a free mod is a
"commercial release" is arguable; notifying anyway costs one form. **This belongs
in the release checklist.**

**Supplement §7.1(b) is also a release action.** An application incorporating the
DLSS or NGX SDK must attribute the SDK and include NVIDIA marks on its splash screen,
about box if present, and game credits. A plugin has no independent splash/about UI and
cannot silently edit Fallout 4 VR's credits; the exact compliant attribution placement
must therefore be settled with NVIDIA before public release. Passing the hardware gate
does not settle that distribution obligation.

**Supplement §1** limits the DLSS SDK to "systems with NVIDIA GPUs", which is
inherent to DLSS and constrains nothing we would otherwise do.

**One trap the archive confirms.** `bin/x64/` and `bin/x64/development/` carry the
same file names - production and watermarked development builds of every DLL,
including a 59 MB `nvngx_dlss.dll` in one and a 70 MB one in the other. Any
manifest entry spells the full archive-relative path, never the leaf.

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
2. ~~No vendor SDK licence text has been read by this project.~~ **Partly closed
   2026-08-29.** NVIDIA's are read from NVIDIA's own archive and quoted above:
   Streamline's source is MIT, and the DLSS runtime is under the NVIDIA RTX SDKs
   License, which permits redistribution inside an application with material
   additional functionality. AMD's and Intel's terms are still unread, and the
   licence covering NVIDIA's **prebuilt signed** `sl.*.dll` is still OPEN - the
   archive states one for `nvngx_dlss` and `nis` and for nothing else.
3. Nothing is flipped to "shipped" by this reading. What changed is that
   user-supplied is now a deliberate choice rather than a forced one, and the
   choice is the project owner's to make.
