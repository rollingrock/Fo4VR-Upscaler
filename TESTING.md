# Testing the FO4VR Upscaler candidate

This candidate is for Fallout 4 VR 1.2.72, F4SEVR, the VR Address Library, an
NVIDIA RTX GPU and SteamVR. True Scopes and FRIK are optional. Their combined
reference test used True Scopes v0.3.32; report your versions if different.

## Install

1. Install the **`-mod.7z`** candidate archive as a separate MO2 mod.
   The **`-docs.zip`** companion is for reading; extract it outside MO2. The mod's `F4SE/Plugins`
   directory belongs under the game's virtual `Data` directory. Keep the DLL
   and its matching PDB together; the PDB makes crash reports useful.
2. Disable any other upscaler plugin. Keep a copy of your previous upscaler
   configuration if upgrading. Do not combine DLL/PDB files from different builds.
3. Supply NVIDIA's production `nvngx_dlss.dll` in
   `F4SE/Plugins/Fo4VRUpscaler/` inside this mod. It is not in the archive.
   The reference tests used NVIDIA's production 310.7.0.0 runtime; obtain it
   from [NVIDIA Streamline v2.12.0](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.12.0).
   In `streamline-sdk-v2.12.0.zip`, use `bin/x64/nvngx_dlss.dll`, not the
   watermarked development copy. Its SHA256 is
   `BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E`.
   Copy that DLL only; no Streamline interposer is needed.
4. Install the Microsoft Visual C++ x64 redistributable if missing. Launch the
   game normally through F4SEVR. No DevBench installation or listener is needed.

Without a valid runtime, reconstruction stays off and the reason is in
`Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler.log`.

## Candidate settings

Fresh defaults are **DLAA, preset K, sharpness 0.5**. K is shared across modes;
choose F explicitly for a Quality F comparison. Existing overrides retain their
selected preset. K can cost more frame time than F; choose F if needed.
Open the panel with F11 to choose settings; changing between DLAA and a reduced
mode requires the restart described by the panel. Start a new process for that
comparison. Use a scoped weapon and travel normally.

Both audited mountain/reflection corrections are enabled in this candidate.
Their runtime guards still apply; unseen or replacement shaders are not certified.
Developer tooling and shader captures default off. No developer override is
included in the package.

**Upgrading:** settings changed through the panel persist in
`Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscalerOverride.toml` and win over the
package. This is shared across MO2 profiles. If you used a research build, check
for old correction flags, diagnostic gates, `devBench`, mode or preset overrides.
For a clean-default check, close the game and temporarily rename that override
file to a backup. Restore it afterward to recover your preferences. Do not delete
your settings or game saves. Preserve personal DLAA K when you want to keep it.

## Play and report

Use [HEADSET_CHECKLIST.md](HEADSET_CHECKLIST.md) as a guide, not a mandatory full
matrix. Missing DLC, an absent weather event or an awkward location can be skipped.
Normal play, combat, save/load and transitions are useful. No agent check-ins are
required; report anomalies rather than stopping between successful steps.

For a report, include the archive name or `BUILD.json` identity, GPU/driver,
headset/runtime, mode/preset and relevant mod versions, then what happened and
where. Attach the upscaler log and a screenshot or crash log when convenient.
Do not repeatedly reproduce a crash. For moving reflections, describe the motion;
a still screenshot may not capture it.

## Return to your previous setup

Close the game, disable this MO2 mod and re-enable the previous one if used.
Restore the saved configuration/override. For a manual install, remove only this
package's listed files; keep user-supplied runtimes and logs if you want them.
The mod does not need to be uninstalled from a game save.
