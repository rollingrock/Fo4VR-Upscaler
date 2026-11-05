# 0.1.5 (in progress, not released yet) - the HUD says when the reflection fix is off

- In reduced modes, the HUD now says when the reflection fix is off and why: the game's
  shaders don't match the tested ones (usually a shader mod such as FO4VR Community Shaders),
  a mod changed the game's shaders in memory, another plugin already hooks the game code the
  fix needs, or `compositeReconstructionScale = false`. Reflections can then differ between
  the eyes. DLAA is not affected.
- When another plugin has already hooked game code this mod needs, the log now names it:
  search for `already hooked by`.
- If another plugin hooked the game function the reflection fix needs first, the fix now
  installs in front of that plugin's hook instead of switching itself off, so both mods keep
  working. Dynamic Bottle Liquids is one such plugin: with it installed, Balanced and the
  other reduced modes had reflections that differed between the eyes, and that is fixed.
  `hookChainForeign = false` restores the old behaviour.
- Raising or lowering a True Scopes scope no longer flashes. The DLSS history is no longer
  reset when the scope goes up or down: `resetOnScope` is now off by default, and with True
  Scopes it also covers the game's scope menu opening and closing, which `resetOnMenu` used
  to. `resetOnScope = true` brings the resets back.
- Experimental foveated DLAA, off by default. `foveation = true` turns it on, in DLAA only:
  DLAA covers a box on each eye's own view centre, the game's own anti-aliasing does the
  rest, and a soft edge joins the two. The box is 75% of the eye per axis by default
  (`foveationSize`, 0.5 to 0.95). At 75% it saved about 1 ms per frame on an RTX 4090 at
  3072x3264 per eye. `foveationToggleVk` binds a key that switches between foveated and the
  full eye. The edge can be visible at some sizes.

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
