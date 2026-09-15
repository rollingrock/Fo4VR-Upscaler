# Quality cell continuity preview 1

This candidate addresses a brief return to native rendering when walking across
an exterior cell boundary. Visual acceptance is pending. No game was launched
or live installation changed during offline preparation.

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
ticket; no seamless result is promised for that case.

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
4. Exit normally and preserve the complete log and wearer verdict. Stop early
   if either eye becomes incorrect. Restore the backed-up candidate files only
   after the game exits, if rollback is needed.

Expected evidence: `quality world continuity: retained active` accompanies
qualified crossings, without a new `stabilizing 1/16` episode. DevBench, when
already enabled, also reports `worldAdmission.continuations` and
`discontinuityGeneration`; it is not required or enabled by this candidate.
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
