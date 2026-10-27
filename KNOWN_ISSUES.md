# Known issues and limits

- Workshop outlines passed the default Quality lamppost/workbench test and the
  integrated DLL's persisted 57% startup test, including head turns and both
  scene-target orders, on September 20. The post-reload lamppost check also passed.
  The correction is enabled by default. Other custom ratios and a fresh launch from
  the unmodified release package/defaults remain unqualified.
- The custom-ratio HUD depth repair passed default -> 57% -> Ratio reset -> 57%,
  fresh startup at persisted 57%, and same-save reload in the headset. A short
  unmodified-package/default smoke test and final performance measurements remain open.
- Cylon reports travel hitches remain with ROCK disabled, although less severe.
  There is no matched frame-time trace or upscaler-disabled travel control yet;
  the cause is unconfirmed. His separate equipment soft lock has no log or
  confirmed ROCK-off reproduction. Keep these two investigations separate.

- The strongest current visual evidence is Quality F and DLAA K on one
  SteamVR setup. Other GPUs, resolutions, controllers and mod lists add new
  coverage. OpenComposite and ReShade compatibility are not qualified here.
- The location/weather checklist remains open for wider self-testing. The
  105-record unattended noon screen checks operation, not visual correctness,
  every lighting phase or natural weather transitions.
- The qualified shader source is Bethesda `ShadersFX/Shaders012_VR.fxp`,
  SHA256 `EEA46BB93C047C359451B8852A4AB013A1444D67FE8974813F5C456EF7B7D4EE`.
  Live bytecode/package identity is not enforced; replacement shaders need
  separate qualification. Corrections can enable D3D11 threading protection
  until process exit; its isolated final-build cost remains unmeasured.
- The cell-continuity preview can retain active Quality across qualified exterior
  crossings. Other transitions retain the 16-observation native stabilization
  window. Owner testing found the earlier softening improved; a brief repeatable
  bridge flicker remains unisolated. See `docs/QUALITY_CONTINUITY_TEST_1.md`.
- Both reconstruction corrections are enabled for candidate testing. Remaining
  shader execution branches and same-ID replacement shader bytecode still need
  live coverage. A shader's presence in the contract table is not a visual pass.
- True Scopes/FRIK interactions and vanilla scope recovery pass on the reference
  setup. Vanilla scope use can use safe native rendering; this candidate does
  not claim a separate temporally reconstructed vanilla scope image.
- Blocking interfaces can temporarily return reduced modes to native rendering.
  Seamless Pip-Boy/interface-only exclusion is unfinished. `excludePipboy` and
  `reactiveMask` are reserved settings, not implemented features; `debugVisualise`
  is also inactive. No reactive mask is supplied for particles/transparency.
- Foliage coverage and thin edges can differ under reduced rendering. Report
  conspicuous trails, flicker or eye mismatch, especially around particles,
  weapons, moving objects and weather effects.
- Crossing between DLAA and reduced rendering requires a restart. Per-user
  overrides survive upgrades and are shared across MO2 profiles.
- The runtime's preset parameter readback does not establish which model the
  driver internally selected. Driver overrides are not comprehensively detected.
- First-class FSR is unfinished and absent from this package. XeSS and frame
  generation are not provided. Other selectable reduced modes have less visual
  coverage than Quality F; UltraPerformance is exploratory.
- This is a first release. Long gameplay, installation on other machines and
  final performance comparisons are still open. No universal
  speedup, stable-release readiness or compatibility with every rendering mod
  is claimed.

OpenXR menu preview: the basic Quest 3 + VDXR + OpenComposite + OFXR interaction
check passed locally on 2026-09-14. This does not qualify all OpenComposite
configurations or supersede the broader coverage limits above. The layer currently
binds Quest Touch controllers only: with other controllers the open panel can take
their input with no cursor, so close it with the panel key (F11 by default) with
the game window focused. The layer is not requested for a game running as
administrator, or when `XR_ENABLE_API_LAYERS` already names another registry-installed
layer; the log names the reason. See `docs/OPENXR_PANEL_TEST_1.md`.

Without an RTX GPU or a usable NVIDIA runtime, the first NGX start-up attempt fails,
the HUD says the upscaler is unavailable on this PC, and the game renders natively
for the rest of the session. The log names the reason.
At any custom render scale the log warns that "the per-mode ratio table and the
runtime disagree"; that line is expected there and harmless.
