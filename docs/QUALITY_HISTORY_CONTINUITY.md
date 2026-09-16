# Quality history continuity candidate

## Reason for the change

The owner saw distant trees soften during a stationary paired-history-reset probe.
A separate one-frame native TemporalAA fallback, with reset counters unchanged,
produced no reported softening. These observations implicate history reset and
its coupled jitter restart; they do not prove every fallback is invisible.

## Contract

Reduced NGX defers cell-only reset decisions to the render-thread world gate.
Cell events still invalidate in-flight publication tickets immediately. A fresh
main-world observation can preserve history only under the existing same-player,
same-exterior-world, completed-frame, 128-unit/100-ms continuity limits.

A cell-rejected frame can preserve history on next-frame recovery only if the
native fallback consumed its completed paired reconstruction, matched the engine
history parity, returned from its draw, and closed the owned ratio/geometry/pixel
transactions normally. A restored bracket without that evidence can recover
admission but still requests a paired reset. The recovery receipt expires after
one main observation and cannot chain successive rejected frames.

Post-evaluation failures before handback, partial-eye work, dirty/failed closure,
loading, menus, world/player changes and unqualified movement keep their reset
paths. The exact publication lease is never relaxed. A final atomic cell veto
can earn the same recovery receipt; it cannot publish the rejected pixels.

The reset-generation latch is not cleared or acknowledged to suppress cell resets.
Unrelated requests survive, including ones arriving during deferred classification.
DLAA retains immediate resetOnCell behavior. Leaving the reduced world gate also
owes a reset so a deferred event cannot escape into the native mode.

## Offline checks

The engine-free production policy tests exercise 7,326 checks, including missing
or wrong closure receipts, expiry, teleport/time limits, hard events after closure,
coincident menu/cell events, and rejected-ticket publication after recovery.
NGX and SDK-free Release builds are required. These checks establish policy and
build behavior; they do not establish headset appearance or live hook receipts.

## Short headset check

After coordinated deployment, launch normally through the desktop MO2 profile.
Hold Quality, preset K and sharpness 0.5 for comparison; preserve all other settings.
Stand at the same bridge looking at distant trees, then cross it three or four
times in both directions. Report any softening, ghosting, eye mismatch or flash.
Finish with one menu open/close and one ordinary interior load to check that true
discontinuities still settle correctly. Stop if either eye behaves differently.

Capture startup identity and before/after state/logs. Qualified retained crossings
should not create cell reset requests. Qualified recovered fallback logs should
say `history reset false`, with no new reset submission unless an independent
event occurred. Correlate fallbacks, cell events, history-parity mismatches, reset
generations/submissions and session latches; counters alone are not acceptance.

Do not inject reset/fault probes during the walking comparison. Restore the
personal mode preference after exit. This candidate requires deployment and
headset acceptance; offline success is not a visual verdict.
