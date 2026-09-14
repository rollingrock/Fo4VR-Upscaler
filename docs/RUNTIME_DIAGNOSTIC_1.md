# RC1 runtime diagnostic 1

This is a diagnostic build for the reported loaded-DLSS-runtime lookup failure.
It adds logging; it does not remove the runtime check or claim to fix the issue.
Keep the NVIDIA App settings as they are for this capture.

1. Close Fallout 4 VR. From the diagnostic mod archive, copy both
   `F4SE/Plugins/fo4vr_upscaler.dll` and `fo4vr_upscaler.pdb` over the matching
   files in your existing RC1 mod. Keep the original RC1 pair for restoration.
   Leave `F4SE/Plugins/Fo4VRUpscaler/nvngx_dlss.dll` where it is.
2. Launch through MO2/F4SEVR with DLAA K selected. If previous experiments left
   the mode off or another preset selected, select DLAA K in the panel, then
   close and restart once. Load a save, wait about 20 seconds, and exit normally.
   There is no need for a location/weather checklist or repeated preset changes.
3. Send `Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler.log` before launching
   again. Look for `ngx runtime diagnostic 1` to confirm the diagnostic ran.
   Restore the original RC1 DLL/PDB after collecting the log.

No new SDK DLL, developer listener, diagnostic setting or headset capture is
required. The mod archive retains the matching PDB and consolidated notices.
The companion documentation ZIP is for reading, not MO2 installation.

The log records the expected runtime path/version, exact Win32 lookup results,
per-eye NGX creation results, up to 512 vendor messages per initialization and
view-creation phase, and one bounded snapshot of relevant loaded modules. A
module listed in that snapshot is not proof that NGX selected it. Log volume
and preparation timing can increase; this build is for the short capture.

Runtime selection, file-identity acceptance, feature creation flags, history
ownership, fallback policy and DLAA K defaults remain as in RC1. Local NGX and
SDK-free Release builds pass. The existing standalone hardware harness passes
30 cases and 3,816 checks, with no failures or skips. The external tester's
failure has not been reproduced locally, and this is not headset acceptance.
