# Third-party components

BlurAway VR (`Fo4VR-Upscaler`) is GPL-3.0-or-later with the exceptions in [EXCEPTIONS.md](EXCEPTIONS.md).
This file lists the other components it uses. The mod archive carries the full licence
text of every component it contains in `F4SE/Plugins/Fo4VRUpscaler/NOTICES.txt`.

## In the shipped DLLs

| Component | Version | Licence | Use |
|---|---|---|---|
| NVIDIA NGX D3D11 client library | `nvsdk_ngx_d.lib` from NVIDIA Streamline SDK v2.12.0 | NVIDIA RTX SDKs License and RTX Supplement | DLSS/DLAA backend, statically linked into `fo4vr_upscaler.dll`. `-DUPSCALER_ENABLE_NGX=OFF` builds without it. |
| CommonLibF4 (rollingrock fork) | submodule `a3f0713e49eb` | MIT | F4SE interface, address resolution, hooks |
| Dear ImGui with its DX11 backend | 1.90.2 | MIT | In-headset panel |
| ProggyClean font | as bundled with Dear ImGui 1.90.2 | MIT, Copyright (c) 2004, 2005 Tristan Grimmer | The panel's text. Compiled in through Dear ImGui as its default font. |
| spdlog, with fmt | 1.13.0, 10.2.1 | MIT | Logging |
| toml++ | 3.4.0 | MIT | Reads `Fo4VRUpscaler.toml` |
| xbyak | 6.73 | BSD-3-Clause | Generates hook code at runtime |
| rsm-mmio, rapidcsv | 2.0.0, 8.80 | MIT, BSD-3-Clause | Used by CommonLibF4 |
| OpenVR SDK header | 2.5.1 | BSD-3-Clause | Header only. The plugin calls the `openvr_api.dll` the game already loaded. |
| devbench API glue (`src/DevBenchClient/DevBenchAPI.*`) | `rollingrock/devbench` `01e33302a8fd` | MIT | Optional devbench tool registration, off by default. Unmodified, with its licence beside it. |
| AMD FidelityFX FSR 1 RCAS | FidelityFX SDK DX11 fork `e65b2530631f` | MIT | Sharpen kernel, rewritten in our HLSL in `src/Upscaler/SharpenShader.h`, which carries AMD's notice. |
| OpenXR SDK headers (`external/openxr`) | `release-1.1.36` (`8899a91c17ce`) | Apache-2.0 OR MIT; MIT selected | The OpenXR panel layer, `Fo4VRUpscalerOpenXR.dll`. No OpenXR loader is linked or shipped. |

Also declared in `vcpkg.json` but not referenced by the plugin's source: args 6.4.6
(MIT), boost-stl-interfaces 1.84.0 (BSL-1.0), frozen 1.1.1 (Apache-2.0), nowide 11.3.0
(BSL-1.0), robin-hood-hashing 3.11.5 (MIT), rsm-binary-io 2.0.6 (MIT) and srell 3.010
(BSD-2-Clause). Their notices ship in `NOTICES.txt` too. vcpkg versions come from the
manifest baseline `b4a3d89`, with openvr pinned by an override.

## Shipped unmodified in the mod archive

| Component | Version | Licence | Use |
|---|---|---|---|
| NVIDIA DLSS runtime, `F4SE/Plugins/Fo4VRUpscaler/nvngx_dlss.dll` | 310.7.0.0, SHA-256 `BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E` | NVIDIA RTX SDKs License, in `nvngx_dlss.license.txt` beside it | The DLSS/DLAA runtime the plugin loads. Copied unmodified from `bin/x64/nvngx_dlss.dll` in NVIDIA's Streamline SDK v2.12.0. |

The runtime is NVIDIA's software. It is not covered by the GPL, and it must not be
redistributed on its own, apart from this mod. The DLL and its licence file ship only in
the `-mod.7z`.

## Build inputs that do not ship

| Component | Version | Licence | Use |
|---|---|---|---|
| FidelityFX SDK DX11 fork (`alandtse/FidelityFX-SDK-DX11`) | `e65b2530631f` | MIT | FSR 3.1 backend, only with `-DUPSCALER_ENABLE_FFX=ON`. Off by default and not in the release. A build that ships it must carry its MIT notice. |
| NVIDIA Streamline SDK | v2.12.0 | MIT (framework source); RTX SDKs License (NGX, DLSS) | `scripts/fetch-sdks.ps1` stages it into the git-ignored `.sdk-cache`. It supplies the NGX headers, the client library and the bundled runtime, and `tools/streamline-probe` uses it. Only the NGX client library and the runtime above ship. |
| Catch2 | 3.5.4 | BSL-1.0 | Installed by `vcpkg.json`. Used only by CommonLibF4's optional tests. |

## Supplied by you, not in the mod archive

- Fallout 4 VR 1.2.72, F4SEVR, the VR Address Library for F4SEVR and the Microsoft Visual
  C++ x64 redistributable, each under its own terms. `openvr_api.dll` ships with the game.
- Optional: OpenComposite, which uses the bundled OpenXR panel layer.

## NVIDIA

The release links the NGX client library into `fo4vr_upscaler.dll` and ships the DLSS runtime
beside it, under sections 1(c) and 2(a) of the NVIDIA RTX SDKs License and its RTX
Supplement. The full text is in `nvngx_dlss.license.txt` and in `NOTICES.txt`. The Linking
Exception in `EXCEPTIONS.md` names both as Proprietary Runtime Libraries, so the plugin can
use them without their source becoming part of this project's Corresponding Source.

The attribution required by RTX Supplement 7.1(b) is placed as plain text in the README, on
the Nexus page and in `NOTICES.txt`: **Includes NVIDIA DLSS, redistributed unmodified under
the NVIDIA RTX SDKs License. NVIDIA, NVIDIA RTX and DLSS are trademarks of NVIDIA
Corporation; this mod is not sponsored or endorsed by NVIDIA.**

## Credits

- `scripts/fetch-sdks.ps1` and `scripts/package-release.ps1` follow the shape of the
  same-named scripts in [fallout4-community-shaders](https://github.com/northaxosky/fallout4-community-shaders) (GPL-3.0).
- The wording of `EXCEPTIONS.md` follows [devbench](https://github.com/alandtse/devbench) and Community Shaders.
