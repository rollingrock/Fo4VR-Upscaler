# 0.1.1 release candidate


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

Qualified exterior cell crossings can retain active Quality with a paired
history reset. Loading, blocked interfaces and unqualified crossings keep the
native stabilization gate. Frame-bound tickets also reject stale work from a
previous frame. Owner testing found the earlier softening improved; a brief
bridge flicker remains unisolated. See `docs/QUALITY_CONTINUITY_TEST_1.md`.

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
