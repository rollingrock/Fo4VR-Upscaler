# 0.1.5 - the HUD says when the reflection fix is off

- In reduced modes, the HUD now says when the reflection fix is off and why: the game's
  shaders don't match the tested ones (usually a shader mod such as FO4VR Community Shaders),
  a mod changed the game's shaders in memory, or `compositeReconstructionScale = false`.
  Reflections can then differ between the eyes. DLAA is not affected.

# 0.1.4 - the compare controls work in DLAA

- Keypad `/` and the panel's **Hold for native** and **Native** now show the game's own image
  in DLAA too. In 0.1.3 they did nothing in DLAA.
- DLAA shows `BlurAway VR: dlaa active` on the HUD when it starts, as the reduced modes do.
- The log's self-check reports whether CheekyFoveatedDLSS is loaded. Do not combine the two
  mods: both drive the same DLSS calls.

# 0.1.3 - first public release

- DLAA at the headset's full resolution, on by default, with preset K and sharpening at 0.5.
- DLSS Quality, Balanced, Performance and Ultra Performance, plus a custom render ratio.
- NVIDIA's DLSS runtime (`nvngx_dlss.dll` 310.7.0.0) is included, so nothing needs downloading from NVIDIA.
- An in-headset settings panel on F11, for SteamVR and for OpenComposite.
- Hotkeys to turn the mod off and on, and to hold for the native image.
- Settings you change in the panel are saved in Documents, so mod updates keep them.
- Corrections for HUD occlusion, workshop outlines and reflections in reduced modes, on by default.
- A recoverable fault pauses reconstruction and retries instead of ending it.
