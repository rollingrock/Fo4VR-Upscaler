# BlurAway VR

[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=for-the-badge)](LICENSE)

An F4SEVR plugin that adds NVIDIA DLSS to Fallout 4 VR. By default it runs DLAA, which
anti-aliases at the headset's full resolution. DLAA costs GPU time and gives no frames back;
if you are short on frame time, the reduced modes (`quality` to `ultraperformance`) render
fewer pixels and upscale them. This is an early release; see
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

Files and the repository use the working name `fo4vr_upscaler` / `Fo4VR-Upscaler`.

## Requirements

- Fallout 4 VR 1.2.72. Other versions are refused, and the log says so.
- F4SEVR and the VR Address Library for F4SEVR.
- An NVIDIA RTX GPU and the Microsoft Visual C++ x64 redistributable.

NVIDIA's DLSS runtime, `nvngx_dlss.dll` 310.7.0.0, is included; you do not need to download
it from NVIDIA.

## Install, update, uninstall

Install `fo4vr_upscaler_0.1.3_<commit>-mod.7z` as an MO2 mod, or extract it into
`<Fallout 4 VR folder>/Data`. It adds `fo4vr_upscaler.dll`, its `.pdb`, `Fo4VRUpscaler.toml`
and a `Fo4VRUpscaler/` folder to `Data/F4SE/Plugins`. That folder holds the DLSS runtime,
the OpenXR panel layer and the licence files. Disable any other upscaler plugin.

To update, install the new archive over the old one. To uninstall, remove the MO2 mod, or
delete the three files and the folder above. Under MO2, also delete
`F4SE/Plugins/Fo4VRUpscaler/Fo4VRUpscalerNGX` from Overwrite: NVIDIA's runtime creates that
data folder at launch. Neither touches your settings in Documents (below). Saves do not
depend on the mod.

## Configuration

`Data/F4SE/Plugins/Fo4VRUpscaler.toml` ships with the mod. Changes you make in the panel go to
`Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscalerOverride.toml`, which holds only the changed
keys and wins over the shipped file. Updates leave it alone. Delete it to restore the defaults.
Both files are re-read each time a save loads. The main keys, under `[Fo4VRUpscaler]`:

| Key | Default | Meaning |
|---|---|---|
| `mode` | `"dlaa"` | `off`, `dlaa`, `quality` (0.667), `balanced` (0.58), `performance` (0.5), `ultraperformance` (0.333). The number is the render ratio. |
| `preset` | `"K"` | DLSS preset, `auto` or one letter, shared by every mode. K is sharper than F and costs more GPU time. |
| `sharpness` | `0.5` | Sharpening, `0.0` to `1.0`. `0` is off. |
| `renderScaleOverride` | `0.0` | `0` uses the mode's ratio. `0.30` to just under `1.00` sets a custom ratio for a reduced mode. |
| `hotkeyToggleVk` | `106` (keypad `*`) | Switches between `off` and this session's mode. |
| `hotkeyCompareVk` | `111` (keypad `/`) | Shows the native image while held, in reduced modes only. In DLAA it does nothing; compare with `hotkeyToggleVk` instead. |
| `hotkeyPanelVk` | `122` (F11) | Shows or hides the in-headset panel. |
| `panelEnabled` | `true` | `false` removes the panel and unbinds its key. |
| `panelAnchor` | `"room"` | `room` leaves the panel where it opened. `head` makes it follow your head. |
| `failSoft` | `true` | A recoverable fault pauses reconstruction and retries, up to 8 times a session. |

Hotkeys are Win32 virtual-key codes, and `0` unbinds one. They work only while the game window
has focus and the console is closed. F1 to F12 are `112` to `123`. Mode switches:

| Session started in | Can switch live to | Needs a restart |
|---|---|---|
| `off` or `dlaa` | `off`, `dlaa` | any reduced mode |
| a reduced mode | `off`, any reduced mode, a new ratio | `dlaa` |

`enabled`, `backend`, `forceHiddenAreaMeshOff`, `workshopMaskHooks`, `devBench`,
`diagnosticDynamicResolution` and `diagnosticContinuousFrames` also need a restart. Leave
all other keys, and everything under `[Diagnostics]` except `logLevel`, at their defaults.
The comments in the shipped TOML describe every key.

If you are short on frame rate, try turning on Hardware-accelerated GPU scheduling in Windows
(Settings > System > Display > Graphics, then restart the PC). One tester, Cyl0n_Surf3r, saw a
considerable boost from it.

## In-headset panel

F11 opens the panel about a metre ahead of you, a little below eye level. While it is open
it takes the controllers: the game gets no controller input, so the Pip-Boy and pause menu
will not open. F11 only works while the game window has focus, which it often does not with
a headset on, so use the **Close** button to hide the panel and give the controllers back.

- **Mode**: Off, DLAA, Quality, Balanced, Performance, Ultra. A button marked `next launch`
  cannot apply now; clicking it saves the choice for the next launch.
- **Ratio**: a custom render ratio, with the render size beside it. **use mode ratio** clears it.
- **Preset**: auto, F, J, K, L, M. **Sharpness**: `0` to `1`.
- **Hold for native** shows the native image while held. **Native** keeps it until clicked again.
  Both work in reduced modes only; in DLAA, compare by switching between **Off** and **DLAA**.
- **timers** shows the mod's per-stage costs.

The bottom line says whether your last change was accepted, saved, and applied now or at the
next launch. `panelDistanceMeters` (`1.0`) and `panelWidthMeters` (`0.6`) place and size it.

- **SteamVR:** the panel is a SteamVR overlay. Point a controller at it and click with the laser.
- **OpenComposite:** the mod loads its own OpenXR layer from `Fo4VRUpscaler/OpenXR`, for this
  game only. The right controller points (the left if the right is not tracked), the trigger
  selects and the right thumbstick scrolls (the trackpad on Vive wands). B or Y closes it: B on
  Index, Menu on Vive wands and the original Windows Mixed Reality controllers. The layer is
  set up at launch, so restart after installing or after turning `panelEnabled` on.

## Reporting problems

Attach `Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler.log` to every report, with the
archive name, GPU and driver, headset and runtime, mode and preset, and your other rendering
or VR mods with their versions. For a detailed trace, set `logLevel = 3` under `[Diagnostics]`,
then back to `2`; it costs performance. After a crash, include the crash log too, and keep
`fo4vr_upscaler.pdb` next to the DLL so the crash log can name the mod's code.

If DLAA does not engage, the `ngx backend: unavailable -` line says why, for example
`ngx backend: unavailable - nvngx_dlss.dll is absent from the explicit runtime directory`.
The early lines `backend selection null` and `backend ngx: unavailable - a device-bound probe
is required` appear on every launch and are normal.

## Building

You need Visual Studio 2022, CMake, vcpkg and PowerShell 7. Set both `VCPKG_ROOT` and
`VCPKG_INSTALLATION_ROOT` to your vcpkg folder; different parts of the build read each one.

```
git clone --recurse-submodules https://github.com/rollingrock/Fo4VR-Upscaler
cd Fo4VR-Upscaler
pwsh -File scripts/fetch-sdks.ps1
cmake --preset vs2022-windows-vcpkg-vr
cmake --build buildvr --config Release

# SDK-free build: no NVIDIA download; DLSS reports itself unavailable
cmake --preset vs2022-windows-vcpkg-vr-nongx
cmake --build buildvr-nongx --config Release
```

`fetch-sdks.ps1` downloads and verifies NVIDIA's Streamline release into the git-ignored
`.sdk-cache`. Output goes to `buildvr/Release`. CI runs the SDK-free build on pushes to `main` and on pull requests.

A local build does not copy the DLSS runtime. To test one, copy
`.sdk-cache/streamline/nvngx_dlss.dll` into `Data/F4SE/Plugins/Fo4VRUpscaler`. Release
archives are built with `pwsh -File scripts/build-release.ps1`, which builds from mapped drive
letters so the DLLs and PDBs name no local folder, then packages them with the runtime.

## License and credits

GPL-3.0-or-later with the project's Modding and Linking Exceptions: see [LICENSE](LICENSE),
[EXCEPTIONS.md](EXCEPTIONS.md) and [THIRD_PARTY.md](THIRD_PARTY.md). Copyright (C) 2026
rollingrock and Fo4VR-Upscaler contributors. The complete corresponding source for every
release is this repository; `SOURCE.txt` in the mod archive names the exact commit.

The bundled `nvngx_dlss.dll` is NVIDIA's software under `nvngx_dlss.license.txt` beside it,
not under the GPL, and may not be redistributed apart from this mod.

Includes NVIDIA DLSS, redistributed unmodified under the NVIDIA RTX SDKs License. NVIDIA, NVIDIA RTX and DLSS are trademarks of NVIDIA Corporation; this mod is not sponsored or endorsed by NVIDIA.

Built on the [rollingrock/CommonLibF4](https://github.com/rollingrock/CommonLibF4) VR fork,
with thanks to alandtse/open-shaders, northaxosky/fallout4-community-shaders and
Tpiac/F4R_Upscaling. Sharpening uses AMD FidelityFX RCAS; the panel uses Dear ImGui.
