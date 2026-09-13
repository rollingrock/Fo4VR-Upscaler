# Release-candidate self-test guide

**Open coverage guide, carried into Phase 6 on 2026-09-13.** Phase 5 is closed;
these unchecked items remain open. Use this with the next prepared candidate.
It is guidance for normal play, not a required full matrix for every tester.
Work at your own pace; no per-item messages, screenshots or native comparisons
are needed when everything looks right. Stop whenever you want.

Tester/date: __________  Candidate version/hash: __________
GPU/driver: __________  Headset/runtime: __________  DLSS runtime version: __________
Mod list/profile: __________  Mode/preset: __________

Record what is available once; the package should identify its build and supported
settings. Use a separate copy of this guide for each tester/build. Quality F and
DLAA K are the main comparison modes; start with the mode you normally use and
start fresh when switching between DLAA and Quality. Another mode is useful
coverage when the candidate supports it; label it rather than counting it as F/K.

Use your usual travel and a weapon with a working optic. Keep any test-only travel
or weather changes out of regular saves. If setup, a DLC, location or optic is
missing or awkward, skip it; do not spend headset time fixing fixtures. Ordinary
play in comparable locations is useful too. Note substitutions by the row.

## What to look for

At each stop, look around normally, then make a slow pan and head tilt. Briefly
check each eye on its own if something looks odd. About 20-30 seconds of actual
observation is enough for this quick screen; loading and weather settling are extra.

- [ ] Both eyes show comparable lighting, shadows and distant surface detail.
- [ ] Water, puddles and shiny surfaces agree between eyes; reflections move normally.
- [ ] Fog, rain, smoke, sunlight and bright signs/windows appear consistently.
- [ ] Trees, fences and weapon edges stay stable; no conspicuous trails or flicker.
- [ ] Raise/lower the scope once; the live picture and normal world return correctly.
- [ ] Open/close the Pip-Boy once; hands, world and both eyes return correctly.
- [ ] No new repeated hitches, black eye, crash or suspended/terminal messages.

These are the shared checks for every row below, not a full weapon battery to
repeat at every location. Skip effects that are absent and mark them **not seen**.

## Suggested route

Tick each mode after the local checks. Enter and leave through a normal door
where listed, checking the view on both sides. A teleport inside does not check
the loading transition. Split the route across as many short visits as you like.

| Stop | Extra thing to notice | Quality F | DLAA K |
|---|---|---|---|
| Revere shore / station | Shoreline, wet metal, dark station, return outside | [ ] | [ ] |
| Far Harbor waterfront / Last Plank | Dense fog, water, wet surfaces; door in and out | [ ] | [ ] |
| Nuka-Town / Fizztop | Bright signs, shiny surfaces, interior/exterior lighting | [ ] | [ ] |
| Corvega interior / exterior | Industrial lights, shadows, pipes and metal; door both ways | [ ] | [ ] |
| Glowing Sea | Distant terrain, haze and radiation-weather effects | [ ] | [ ] |
| Diamond City main gate, if conveniently accessible | Normal city entrance and exit | [ ] | [ ] |

**New setup, once:** check a distant landscape and wet reflective surface, use
your normal scope through a few raises/lowers, shots, holster/redraw and a weapon
switch, then open/close the Pip-Boy and take one normal interior/exterior door.
Use True Scopes/FRIK if installed. Vanilla scopes are a separate configuration;
do not remove working mods mid-session just to fill a row.

**Existing owner baseline:** the saved mountain view, misty puddles, Diamond City
reflections, market/Inn round trip, full True Scopes/FRIK interactions and vanilla
scopes already passed in Quality F and DLAA K. Revere Quality shore, scope and
station entry also passed; return outside remains open. The owner need not repeat
those full tests on the unchanged build. These passes are context for another
tester, not results to pre-fill on another setup.

## Weather and lighting encountered during play

Use a view with a nearby shiny surface and distant skyline. Wait until the
effect is actually visible, then use the shared checks. No need to force or view
all weather records: the existing 105-record unattended sweep covered mechanical
operation on the reference profile. Different weather mods add new coverage.
Use these rows when convenient; do not wait around for a missing event.

| Sequence / effect | Quality F | DLAA K |
|---|---|---|
| Clear -> rain or misty rain -> clear: wetness and reflection recovery | [ ] | [ ] |
| Light -> dense fog: both eyes agree, including the distant horizon | [ ] | [ ] |
| Far Harbor rain / radiation mist or fog -> clear | [ ] | [ ] |
| Nuka-World rain or dust -> clear | [ ] | [ ] |
| Dusk/night with lamps or bright windows: bloom, shadows and exposure | [ ] | [ ] |
| A storm/lightning event, if one actually occurs | [ ] / not seen | [ ] / not seen |
| One gradual weather or day/night change, if encountered | [ ] / not seen | [ ] / not seen |

This is a quick anomaly screen. A forced weather switch does not count as a
gradual transition, and an unseen event does not count as passed. Any separate
shipping DLAA K check will be supplied as one short final card.

## Normal play and problem reports

Playing for a longer stretch, with ordinary combat, travel and save/load, is
useful stability coverage. Optional note: time played __________; locations or
events tried __________. There is no required uninterrupted session length.

If something is wrong, stop that item and report it to the owner/developer. If
convenient, take one screenshot and note **location, mode, weather/time, what
looked wrong, whether it affected one eye or both, and what happened just before**.
Include the candidate/setup recorded above. An available upscaler log or crash
log can help; the package should explain where to find it. A brief description
is enough; no diagnosis or repeated failure is required. Leave a crash closed.
For a moving reflection issue, describe the movement even if a still screenshot
cannot show it.

Optional single completion note: **“Build [version], checked [rows] in [mode];
played about [time]; no anomalies.”** Keep unchecked or not-seen items as they
are. No need for agent check-ins between passes.
