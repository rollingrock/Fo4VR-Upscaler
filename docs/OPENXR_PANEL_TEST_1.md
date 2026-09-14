# OpenXR menu preview 1

Experimental build for OpenComposite + VDXR + Quest Touch controllers, including
OFXR. The menu has passed offline tests and the local headset check described below. This
build retains the runtime compatibility and performance diagnostics from the
previous candidate. It makes no measured FPS improvement claim.

## Install

Close Fallout and install the complete preview `-mod.7z` package, allowing
it to override the previous upscaler. Keep the working NVIDIA runtime and your
existing preset. The full package defaults to DLAA K and normal diagnostic settings.
If preserving a customized TOML, set `panelEnabled = true` under `[Fo4VRUpscaler]`
and keep `hotkeyPanelVk = 122` (F11).

The package includes the plugin DLL/PDB plus the OpenXR layer DLL/PDB/JSON under
`F4SE/Plugins/Fo4VRUpscaler/OpenXR`. Copying only the plugin DLL is insufficient.
No extra OpenXR loader download or global layer registration is needed. The plugin
adds the layer to this game's process environment before OpenComposite initializes.
A fresh game launch is required.

For the already accepted local setup: Virtual Desktop uses VDXR, SteamVR is closed,
OpenComposite and OFXR remain enabled, and VR Performance Toolkit and ReShade remain
disabled. Change none of those while checking the menu.

## Two-minute check

1. Load a safe save, close the console, lower the weapon and focus the game window.
   Press F11. Confirm a readable menu appears in both eyes about a metre ahead.
2. Point the right controller at it. The on-panel cursor should follow. Use the
   trigger to select and the right thumbstick to scroll. Check that clicking does
   not fire or move the player. Left aiming is a fallback when right tracking is
   unavailable; this candidate does not draw a controller laser outside the panel.
3. Change sharpness slightly, check the result line, then restore it. Confirm the
   setting persists after closing and reopening. Try the normal native comparison
   control and restore the original mode.
4. Slowly turn and tilt your head: the room-anchored menu should remain upright and
   fixed. Close with B, Y, the menu close button or F11; release the trigger/buttons.
   Check normal controls, the weapon, scope and Pip-Boy, with no accidental shot.
5. Reopen and close once more with OFXR active. Look for missing/duplicated menu
   frames, flicker, eye mismatch or new hitches. Then close the game normally.

If anything fails, stop that check and send `Fo4VRUpscaler.log`, a short description
and any crash log. Useful lines are `OpenXR quad/input layer enabled`, `OpenXR quad
and Touch controller input ready`, and `first OpenXR quad texture submitted`.
The first line means setup was requested, not that the headset displayed the menu.
`OpenXR texture deferred` records a bounded submission failure or timeout.

To roll back, restore the previous complete upscaler mod and its TOML while Fallout
is closed. No registry or active-runtime restoration is required. Setting
`panelEnabled = false` disables menu interaction; removing the new package and
restarting also removes its process layer registration.

## Offline evidence and limits

Both NGX and SDK-free Release builds compile. The standalone
`tools/openxr-panel-tests` executable loads the production layer DLL and checks
negotiation, binding/action-set chaining, input ownership and release, tracking
recovery, real D3D11 WARP texture copy, zero-timeout acquisition retry, projection
preservation, a separate generated-frame caller, session cleanup, optional input
setup rejection and simulated SEH recovery. These checks use a mock OpenXR runtime;
those tests alone do not establish VDXR loader integration or live OFXR compatibility.

The existing SteamVR overlay path remains available. The new input bindings target
Quest Touch; other controller profiles are not qualified. This layer uses the
game's existing XR instance/session and graphics device. It never creates a second
session, edits projection images, or issues GPU work on OFXR's frame thread.

## Local acceptance, 2026-09-14

Quest 3 + VDXR + OpenComposite + OFXR V068, process 59412: the owner confirmed
correct menu display in both eyes and working controller controls, then made
settings changes and closed it successfully. Logs record the first XR quad
submission, sharpness 0.50 -> 0.45 -> 0.50 and preset M/K/M/L/M, all accepted,
persisted and applied immediately. DLAA remained active. This qualifies the basic
local menu interaction; another controller/runtime combination, long-session
behavior and a full regression are not implied.

The first test package omitted `./` from the layer DLL path and did not load the
layer. The corrected manifest loads beside its DLL and is included in this preview.
Install the full preview, not that superseded test package. No global layer or
runtime registration change was needed.
