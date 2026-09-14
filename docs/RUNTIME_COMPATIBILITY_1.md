# RC1 runtime compatibility 1

This candidate fixes RC1 rejecting successful NGX feature creation when NVIDIA
selects an updated runtime from its cache instead of retaining a module named
`nvngx_dlss.dll`. It retains the supplied-runtime requirement and conflicting
preloaded-runtime check. It does not change NVIDIA settings, force a DLL load,
or select a cached binary itself.

1. Close Fallout. Keep the original RC1 DLL/PDB for restoration. Copy the
   `F4SE/Plugins/fo4vr_upscaler.dll` and matching `fo4vr_upscaler.pdb` from this
   archive over the pair in your existing mod, replacing diagnostic 1.
2. Keep the supplied `Fo4VRUpscaler/nvngx_dlss.dll` and NVIDIA settings unchanged
   for this comparison. Launch through MO2/F4SEVR with DLAA K selected.
3. Load a save, look around for 20-30 seconds, then exit. Send the fresh
   `Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler.log` before another launch,
   and say whether both eyes stayed correct without repeated hitches or
   suspension messages. No location/weather regression is needed.

The identifying log marker is `ngx runtime compatibility 1`. With a cache-loaded
runtime, the expected new message is `continuing with NGX-selected runtime`,
followed by successful two-eye creation and reconstruction. The old repeated
`did not expose a loaded nvngx_dlss.dll` failure should be absent.

The panel's reported preset now says `hint K` when K is the parameter-map
readback. NVIDIA can override that hint; it is not effective-preset proof. Named
module versions are labeled `(module)`, and missing module metadata is reported
as `unreported (NGX-selected)`. The retained vendor log gives additional
runtime/override evidence. This build does not parse vendor log text to decide
whether reconstruction is allowed or to claim an effective preset.

Both NGX and SDK-free Release builds pass, and the updated standalone harness
builds. The hardware harness was not rerun because a local Fallout session was
active. The previous diagnostic's 30 cases/3,816 passing hardware checks are
historical evidence, not validation of this binary. NVIDIA cache-path evaluation
and headset acceptance remain open until the tester runs this candidate.

The main archive retains the matching PDB, unchanged DLAA K default TOML and
consolidated notices. No NVIDIA runtime is bundled. This companion document ZIP
is for reading, not MO2 installation. The local RC1 installation was not changed.
