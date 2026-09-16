# Quality softness isolation

The bridge test associates distant-tree softening with a single rejected frame
that finishes on native TemporalAA, followed by immediate Quality recovery and
a paired history reset. It does not identify which operation changes the image.
This diagnostic build preserves preview 2 rendering and adds one DevBench action.
It is not a claimed visual fix.

## Ownership

`historyReset` with `request=true` uses ResetPolicy::RequestReset, the existing
cross-thread event latch. It requests a jitter-sequence restart and publishes a
reset generation. Evaluate acquires one ticket for both eyes and acknowledges
only after their evaluations/reassemblies succeed. This route neither changes
world-transition generations nor grants a publication lease. The listener makes
no GPU calls and writes no settings. With request absent or false it only reads.

The route preflight requires active Quality, completed handbacks, no pending
reset, no disabled rendering session and no armed fault. Those are observations,
not locks: an event can arrive afterward. Before/after generations in the reply
are also observations, not an exact request receipt. Capture state before and
after each probe and reject contaminated results. An acknowledgement may already
be visible in the response; pending=false does not mean nothing happened.

## Short stationary comparison

Deploy only after clearance, backup, and game exit. Preserve the current profile,
runtime and settings. Use a fresh Quality session and stand still facing the
distant trees. Keep menus closed, wait for settled rendering, and announce each
probe separately to the wearer. Do not run a whole sequence unattended.

Use the DevBench upscaler tool's action objects below. The equivalent HTTP
transport is POST /api/tool/upscaler with the same JSON; discover the live port
from the host, not a guessed instance. DevBench is optional in ordinary use.

1. Baseline: `{"action":"state"}`; record resetPolicy, worldAdmission,
   qualityPixelTransaction, qualityFaultInjection and nativeTaaReplacement.
2. Reset alone: `{"action":"historyReset","request":true}`. Poll state
   until pending clears and acknowledgement advances. With no other events,
   requests, generations, consumptions and both eye reset submissions must each
   advance once; native fallback and fault-trigger counts must not advance.
   World transition/discontinuity generations and cell/menu/scope event counts
   must be unchanged. Ask whether the trees softened and for roughly how long.
3. After settling and a fresh baseline, fallback alone:
   `{"action":"injectQualityFault","faultSeam":"prePublish","faultCount":1}`.
   This existing fault declines one seam draw after its world/bindings validation.
   It invokes native TemporalAA over reconstructed pixels without a cell event.
   The normal fallback close requests no reset. Verify exactly one fault trigger,
   one native fallback and one normal fallback late close, no pending fault,
   no disabled session, and no reset/generation/event changes. Native fallback
   history mismatch/dirty/unrecoverable counters must not advance. If arm is
   declined because the transaction is not idle, verify acceptedCount=0 before
   retrying; never retry an uncertain mutation response automatically.
4. Obtain a separate wearer report before doing any other probe.

Snapshot reads can straddle a render frame. After completion, confirm counters
settle before interpreting them. Stop if either eye becomes incorrect. A pending
fault can be disarmed with faultSeam=none; do not invoke any terminal fault seam.

Both successful probes return to the existing Quality path without a restart.
A fallback-only match implicates native temporal filtering and/or omitted RCAS
or depth resolve; a reset-only match implicates reset/jitter settling. If both
match, their combined effect at a real crossing still needs care. If neither
matches, resume real-crossing investigation rather than suppressing guards.

Only if useful, compare a separately announced fallback-only probe with live
sharpness 0, then restore the recorded 0.5 value. Allow configuration-triggered
resets to settle before the probe. This distinguishes a sharpening contribution,
not native TAA from all other fallback differences. No automatic settings edit
or test launch is part of this candidate.

## Offline and remaining validation

Required offline checks: NGX and SDK-free Release builds, route registration and
read-only/default/argument-path review, existing production-policy tests, package
extraction/hash checks. Runtime route dispatch and visual attribution require
the short cleared headset session above; offline builds cannot establish them.
