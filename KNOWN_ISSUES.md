# Known issues and limits

A first release, tested on few setups. Other hardware and mod lists may hit problems not listed here.

## DLSS runtime

- The mod ships NVIDIA's `nvngx_dlss.dll` 310.7.0.0 in `F4SE/Plugins/Fo4VRUpscaler`, the
  folder `runtimeDir` names. Under MO2, a file at that path from a higher-priority mod or
  from Overwrite replaces the bundled copy.
- Without an RTX GPU, or if the runtime cannot be used, the HUD reads `unavailable on this PC -
  rendering natively` and the game stays native for the session. The log gives the reason.
- If another mod already loaded a different `nvngx_dlss.dll`, this mod refuses it and stays
  native. Disable other upscaler plugins.
- NVIDIA's driver can load a newer DLSS runtime from its own cache instead of the bundled
  file. That works; the log then reports the runtime path as `unreported (NGX-selected)`.
- The panel shows the preset requested and the one the runtime reported. A driver-level DLSS
  override can still change the model without the mod knowing.
- FSR, XeSS and frame generation are not provided.

## Modes

- Switching between DLAA and a reduced mode needs a restart (see the README).
- In reduced modes the game renders natively while the Pip-Boy, pause menu, a message box or
  a loading screen is open, and for a moment after it closes.
- Reduced modes can show trails or flicker on foliage, particles and thin edges. There is no
  reactive mask yet. A brief flicker at some exterior cell crossings is not fixed yet.
- Ultra Performance and custom ratios have had the least testing. At a custom ratio the log
  warns that "the per-mode ratio table and the runtime disagree"; that is harmless.
- `excludePipboy`, `reactiveMask` and `debugVisualise` are reserved and do nothing yet.

## Other mods

- The reduced-mode reflection correction needs the game's own `Shaders012_VR.fxp` and no
  FO4VR Community Shaders (CSAH VR). Otherwise it stays off, reflections can look wrong in
  reduced modes, and the log says why. DLAA is not affected.
- True Scopes 0.3.28 or later lets the mod reset history when a scope is raised or lowered.
  The picture inside a scope is drawn by the game or True Scopes, not upscaled.
- ReShade, VR Performance Toolkit and other rendering mods are untested. Hitches while
  travelling have been reported; the cause is not confirmed.

## In-headset panel

- Under OpenComposite the panel binds Quest Touch, Index, Vive, Windows Mixed Reality and
  Reverb G2 controllers. Only Quest 3 Touch has been tried. On a controller it cannot read,
  it stays closed and the HUD says so; use the TOML instead. It closes itself if the
  controllers stop reporting for three seconds.
- The OpenXR layer is not loaded if the game runs as administrator, or if
  `XR_ENABLE_API_LAYERS` already names registry-installed layers. The log says which.
- The settings override in Documents is shared by every MO2 profile.
