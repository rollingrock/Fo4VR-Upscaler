# Quality cell continuity preview 2

This candidate addresses native-rendering intervals while walking across exterior
cells or looking at quick-loot containers. Preview 1 retained some cell crossings
but still visibly dropped out on late cell events. Preview 2 is validated offline;
its headset acceptance is pending and it has not been deployed.

Superseded on history: qualified crossings now preserve history instead of
requesting the paired reset this guide describes. See
`QUALITY_HISTORY_CONTINUITY.md` for the current contract.

## Changes

An active reduced-rendering frame can keep its admission across a cell boundary
when the preceding frame published both eyes and finished restoring the engine,
the player remains in the same exterior worldspace, movement is at most 128
game units and observations are no more than 100 ms apart. These conservative
limits may decline a crossing during a long frame or unusual movement.

The crossing still requests a paired history reset. `resetOnCell=false` does
not disable resets needed by admission. Loading, blocking menus, worldspace
changes, repairs and missing completion retain the full stabilization gate.
Cell events arriving after a frame's admission still invalidate that frame's
ticket. After a cell-only rejection, a normally closed main-world frame can
recover on its next pre-world observation using the last completed exterior
sample. This requires a fresh eligible observation within the same 128-unit /
100-ms limits, no menu or hard transition, and no outstanding pixel or ratio
work. The rejected frame stays unpublished; a new paired history reset is owed.
Recovery cannot chain through consecutive aborted frames. Early pre-world
refusals that never acquired world ownership retain full stabilization.

The item-menu input counter no longer independently blocks Quality. Game RE
identifies its writers as the quick-container HUD's input-context push/pop.
Loading/Pip-Boy/Fader and other blocking menus, frozen backgrounds, UI menu mode,
closing menus, renderer/resource checks and exact frame tickets still gate work.
This does not implement a generic FaderMenu exception.

Tickets now include a frame serial. An old ticket cannot authorize a later
frame merely because its cell and transition generation still match. Full
rechecks cannot advance the stabilization count, and configuration changes
also invalidate continuity. Temporal resource and restoration checks remain
in force.

## Short headset check - only after explicit clearance

Keep the current OpenXR/OFXR/profile/controller configuration and user settings.
Before deploying, confirm the other agents released the game, take exact backups
of the active plugin files, and record the candidate DLL hash. Do not replace
the user's TOML or per-user override with package defaults. The package includes
the required nested OpenXR menu files.

1. Start one fresh **Quality** session with the tester's existing preset,
   sharpness, resolution and runtime. Confirm both eyes are reconstructing.
   A session armed for DLAA needs a restart to change to reduced rendering.
2. Walk across one known exterior boundary and back three times, looking at
   distant detail and nearby foliage. Report any soft flash, black frame,
   stereo difference or trailing at each crossing. Avoid console teleports.
3. Open and close the Pip-Boy once, then use one ordinary loading door and
   return. Native rendering during those blocked transitions is expected;
   return to stable Quality must be clean.
   Also look at and away from one lootable container several times without
   activating it: the quick-loot list should coexist with stable Quality.
4. Exit normally and preserve the complete log and wearer verdict. Stop early
   if either eye becomes incorrect. Restore the backed-up candidate files only
   after the game exits, if rollback is needed.

Expected evidence: `quality world continuity: retained active` accompanies
qualified crossings, without a new `stabilizing 1/16` episode. DevBench, when
already enabled, also reports `worldAdmission.continuations` and
`discontinuityGeneration`; restored late-cell recoveries report `recoveries`
and `quality world continuity: resumed after restored cell rejection`.
DevBench is not required or enabled by this candidate.
Loading/menu controls must still show blocked/stabilizing/active recovery.
Paired resets, clean transaction completion and lack of runtime failures are
supporting evidence; the wearer decides whether the soft flash is resolved.

If a walking flash remains, retain its timestamp and the nearby cell/admission
lines. A paired history reset can have its own visible settling, and this log
alone cannot prove which pixels were displayed.

## Offline validation

The standalone `tools/world-admission-tests` target compiles the same reducer,
continuity predicate and lease checks as production. It covers startup/recovery,
the rejection matrix, finite movement and time bounds, missing completion,
late events, changed world/player identity, and stale frame tickets. Both NGX
and SDK-free Release builds are required. These checks cannot establish engine
event timing, headset image quality or a performance improvement.

## Other collected issues

- The external DLAA performance comparison still requires matching full-eye
  coverage, runtime/preset, resolution and application-versus-output FPS facts.
  This candidate does not claim a speedup.
- NVIDIA's driver override selected M in Cylon log 6 despite the requested K
  hint. Preserve the tester's setting for this check; exact-K qualification
  is a separate comparison.
- Seamless Pip-Boy exclusion and a reactive mask remain unfinished. They need
  separate rendering/input evidence; this candidate retains their existing gates.
