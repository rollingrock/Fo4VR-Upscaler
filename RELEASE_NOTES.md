# 0.1.2

First public release. The sections below record the changes made during the 0.1.1
candidate cycle, newest first.


## Review fixes

A source review of the beta found failure paths that switched off more than they
were meant to, and two input-handling defects. The normal DLAA K and Quality paths
change only in the workshop draw hook encoding and custom-ratio snapping.

- A rare composite-correction fault no longer stops the engine drawing through the
  shared packet-draw helper. The correction stops for the session and the scene
  renders uncorrected.
- In DLAA, a panel setting change that lands mid-frame no longer leaves vanilla
  TemporalAA layered over DLAA for the rest of the session. If the TAA replacement
  does switch off, DLAA now stops with it and the game renders natively.
- After a workshop-outline fault, the game's own VATS and workshop highlight pass
  keeps running.
- The workshop draw hooks no longer break crash-log stack traces through world
  draws.
- The OpenXR menu layer is requested only when the mod and the panel are enabled at
  launch. It is skipped, with a log line, for an elevated game or when
  `XR_ENABLE_API_LAYERS` already names a registry-installed layer: requesting it
  there would stop VR starting under OpenComposite.
- Settings persist for Windows profiles whose Documents path has non-ASCII
  characters.
- Custom render ratios snap to whole render pixels the way the preset ratios do,
  so the right eye no longer shifts by up to a pixel at ratios such as 0.70.

## September 20 integration candidate

Workshop outline correction is enabled by default after the lamppost/workbench
head-pan test passed. It expands only the mask consumed by the native outline
draw and restores that draw's original binding. Both observed scene-target
orders are supported. Custom ratios now require agreement between the actual
mask viewport and the current evaluated frame dimensions. The integrated DLL's
persisted 57% startup test passed on the lamppost and workbench with head turns
and both scene-target orders; its post-reload lamppost check also passed.

The HUD depth repair uses the exact evaluated dimensions at custom ratios.
The default -> 57% -> reset -> 57% sequence passed headset testing. The panel
also reports prepared dimensions instead of a stale vendor-default ratio cache.
Developer tooling remains off by default. Fresh-start and same-save reload at
persisted 57% passed for the HUD. A fresh launch from the unmodified release
package/defaults and final performance measurements remain open.

## Quality continuity

Qualified exterior cell crossings keep active Quality and preserve history; a
crossing whose frame is rejected resumes on the next frame and resets history
only if that frame's fallback did not close normally. Loading, blocked
interfaces and unqualified crossings keep the native stabilization gate.
Frame-bound tickets also reject stale work from a previous frame. Owner testing
found the earlier softening improved; a brief bridge flicker remains unisolated.
See `docs/QUALITY_HISTORY_CONTINUITY.md`.

The exact candidate label, source commit and DLL/configuration hashes are in
`BUILD.json`. Use that identity when reporting results.

This candidate incorporates the tested scope-history reset, scope/temporal-input
recovery and shader-family/fractional-viewport repairs. The package enables the
tested mountain/reflection corrections and disables developer tooling by default.
Both compiled defaults and the shipped TOML agree: DLAA K is the fresh-install
default requested by the owner. Personal overrides remain persistent. The preset
is shared across modes; Quality F testing selects F explicitly.

This integrates fixes tested after the September 13 checkpoint. Earlier headset
results apply to their recorded builds. The September 20 persisted-57 startup/
reload run is the targeted visual pass for this integrated DLL and custom-ratio
workshop admission; it does not qualify other ratios, machines, release-default
startup or final performance.

Phase 5 is closed by the owner's decision. The remaining visual checklist is a
guide for wider candidate testing, with agent help only when something is wrong.
Original unfinished features remain tracked; see `KNOWN_ISSUES.md`.

## Distribution layout

Install the `-mod.7z` archive. It keeps the DLL, matching PDB and TOML, plus
compact build/source metadata and consolidated licence/notice text. The separate
`-docs.zip` carries guides and research documentation and is not an MO2 mod.
The split archives preserve the packaged runtime files byte-for-byte.
