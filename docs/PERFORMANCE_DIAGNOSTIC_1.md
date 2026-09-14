# RC1 performance diagnostic 1

This builds on the NGX runtime compatibility fix that Cylon successfully tested.
It adds DLAA CPU/GPU stage timings and safely declines F11 interfaces when the
OpenVR loader cannot validate them reliably. It is a diagnostic build, not a
claimed FPS improvement. Full OpenXR menu rendering/input remains pending.

## Short test

1. Close Fallout. Back up the current working DLL/PDB. Replace them with the
   matching pair from this archive in the existing upscaler mod. Keep your
   current `Fo4VRUpscaler/nvngx_dlss.dll`, DLAA/preset choice, NVIDIA overrides,
   OFXR and VD settings unchanged. No new external library is required.
2. In your existing `F4SE/Plugins/Fo4VRUpscaler.toml`, set
   `perfTimers = true` under `[Diagnostics]`. If you install the whole archive,
   this is already enabled, but its default remains DLAA K: retain your current
   preset for this comparison. In the personal override file, check that
   `[Diagnostics] perfTimers` does not override this with `false`.
   Personal file: `Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscalerOverride.toml`.
3. Launch through your usual MO2/F4SEVR setup. Load the same save and look around
   at the same spot for about 75 seconds. Keep the scope lowered and menus
   closed during this timing sample. Note the FPS counter/range you observe.
4. With `[Fo4VRUpscaler] panelEnabled = true`, press F11 once. On the affected
   OpenComposite build, the expected result is no menu and no crash. The log
   should say `panel: safely refused`. On SteamVR, the menu should still open.
   Briefly check scope and Pip-Boy return, then close Fallout normally.
5. Send the fresh `Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler.log` before
   another launch overwrites it. Include whether F11 crashed, whether scope/view
   stayed correct, and your FPS counter, VD resolution/refresh/reprojection and
   NVIDIA override settings if readily available.

The log contains `perf report:` followed by JSON, up to six reports at ten-second
intervals per measurement window. A fresh launch gives a fresh window. A personal
`perfTimers = false` override or disabled upscaler prevents useful samples; send
the log anyway if the reports are absent. After collecting it, set
`perfTimers = false` for normal play or a controlled FPS comparison.

Keep the working OpenComposite + VDXR + OFXR configuration. The owner's local
comparison became smooth after disabling VR Performance Toolkit's game-root
proxy; it stayed smooth when this upscaler was restored. This does not establish
the cause of a smaller FPS difference on another machine.

## What the reports mean

- `path: nativeDlaa` spans the productive DLAA evaluation through the completed
  TemporalAA replacement draw. Initial private warm-up is excluded. Each eye's
  evaluation, motion preparation, copy and draw intervals remain separate.
- `path: reducedNgx` retains the existing reduced-resolution transaction boundary
  at the late-range commit. The native final interval is named `nativeSeamClose`;
  it does not include engine work after that seam. Do not compare these totals
  as if they covered identical portions of the frame.
- CPU numbers are elapsed QPC intervals; GPU numbers are nonblocking D3D11
  timestamp results. Neither is total game frame time, headset FPS or generated
  OFXR FPS. `engineHdrToSeamPublish` includes intervening engine work. Timers add
  overhead; this pass locates cost rather than proving a speedup.
- Each completed native episode must match the exact published handback serial.
  Incomplete episodes are discarded, mode changes start separate windows and
  unavailable GPU samples remain missing rather than being reported as zero.
- F11 admission checks the validator with an invalid interface name before any
  generic lookup. A loader that claims that name exists is refused, including
  OpenComposite revision a27e7e6a. No incompatible interface vtable is cast to a
  newer ABI. This guard does not fix unsupported interfaces requested by other mods.

Both NGX and SDK-free Release builds pass. The standalone WARP harness compiles
the actual telemetry module and passes native/reduced GPU collection, serial
matching, missed/incomplete episodes, pending-query mode changes, disable/reset,
wrong-thread rejection, bounded log export and instrumentation-SEH retirement.
Fake runtime tests prove rejected admission never calls generic interface lookup.
These checks do not replace headset acceptance on Cylon's runtime. The owner's
running game and installed settings were left unchanged.

The main archive retains the matching PDB and required notices. Documentation is
in the companion ZIP; neither archive includes NVIDIA runtime DLLs. No GitHub
push accompanies this candidate.

## Source basis

Own `Telemetry`, `HDRRoundTrip`, `Evaluate`, `TAAReplacement` and `Panel` source,
plus these primary API references. No competing upscaler source or binary was read:

- [OpenComposite pinned loader](https://gitlab.com/znixian/OpenOVR/-/blob/a27e7e6a64bdcd1eff6b7fba1ea2ea34bcf1273d/OCOVR/openvr_api.cpp): unsupported lookup abort and unconditional validation.
- [Valve OpenVR 2.5.1 loader](https://github.com/ValveSoftware/openvr/blob/v2.5.1/src/openvr_api_public.cpp): version validation and generic lookup contracts.
- [Microsoft D3D11 asynchronous query flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag): DONOTFLUSH collection semantics.
