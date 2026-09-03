// The one list of TOML keys.
//
// Included twice with different US_SETTING definitions: Settings.h declares and
// loads the settings from it, DevBench.cpp builds the config route's table from
// it. One list, two expansions, so the declared set and the bench's view of it
// cannot drift - the reference repo hand-maintained the second copy and the two
// disagreed.
//
//   US_SETTING(a_type, a_section, a_key, a_default)
//     a_type     bSetting | iSetting | fSetting | sSetting. Doubles as the kind
//                the config route switches on, so a string setting is readable
//                and writable there instead of invisible.
//     a_section  the TOML table name and the /config group, as a quoted string
//                literal. Exactly two exist: "Fo4VRUpscaler" (what a player
//                needs to touch to play) and "Diagnostics" (the regression
//                harness's keys; nothing in it is needed to play).
//     a_key      the TOML key and the C++ identifier, spelled once.
//     a_default  the value used when the key is absent from both tables.
//
// Comments here are the user documentation for each knob and must say the same
// thing as the shipped TOML. Keys this build parses but does not act on say so
// and name the measurement that switches them on.
//
// Every key written before section tags existed sat under [Fo4VRUpscaler],
// including every deployed instance and recorded artifact. A Diagnostics key is
// still read there if its own table does not have it - Settings.h logs one
// deprecation line per key when that fallback fires - so an old TOML keeps
// working exactly as before; only a fresh copy of the shipped file moves the
// diagnostics into their own table.
//
// No include guard, on purpose. US_SETTING is undefined at the bottom, so a
// second expansion in the same translation unit is a plain re-include.

#ifndef US_SETTING
#	error "define US_SETTING(a_type, a_section, a_key, a_default) before including SettingsList.inl"
#endif

// --- honoured by this build ---

// Master switch. false = no hooks are installed and nothing is patched. The
// self-check, the settings routes and the dev tooling stay live, so a disabled
// install still writes a diagnosable log. Read once at load; restart to change.
//
// Deprecated: mode is the switch now. enabled continues to gate hook
// installation exactly as it does today - it is not rewired - but enabled =
// false additionally forces mode to "off" and enabled = true logs a deprecation
// notice. Prefer mode = "off" in a new configuration.
US_SETTING(bSetting, "Fo4VRUpscaler", enabled, true)

// "off" / "dlaa" (native anti-aliasing, ratio 1.0) / "quality" / "balanced" /
// "performance" / "ultraperformance". The activation surface: this build parses,
// validates and reports mode, and refuses it alongside an armed diagnostic gate,
// but does not yet select a backend or drive orchestration from it - the
// diagnostic gates below still do that. mode is parsed, validated and reported
// but does not yet select the render path.
// dlaa is the eventual default and the honest one: it feeds four times the
// input pixels of the numbers most published benchmarks were measured at and
// has no mechanism to give frames back. quality/balanced/performance/
// ultraperformance name a reconstruction ratio, from least to most reduced.
// If mode is absent from the file and the deprecated quality key is present,
// quality's value is used and the log names the substitution. If both are
// present and disagree, mode wins and the log names the conflict. See enabled
// above for how enabled = false interacts with mode.
US_SETTING(sSetting, "Fo4VRUpscaler", mode, std::string("dlaa"))

// 0 = off, 1 = warnings and worse, 2 = info, 3 = trace. Level 3 logs every frame
// decision and is a measurable cost; it exists so a bug report can carry a trace
// instead of a guess. The startup banner is written before this is parsed, so it
// always appears.
US_SETTING(iSetting, "Diagnostics", logLevel, std::int64_t(2))

// Register the plugin's tools into the devbench host when it is present. When it
// is absent this is a no-op and the log says so in one line - the dev tooling is
// never a load-order requirement.
US_SETTING(bSetting, "Diagnostics", devBench, true)

// Staged policy key for normal activation. The current bounded diagnostics always
// session-disable on their first fault; false is parsed and reported but does not
// re-arm them. Never ship false when this policy is connected.
US_SETTING(bSetting, "Fo4VRUpscaler", failSoft, true)

// DEVELOPMENT ONLY and startup-only. When true, renderScaleOverride must be in
// 0.30..1.00 and is written into Fallout 4 VR's byte-verified live and target
// dynamic-resolution width/height ratios before world rendering. Raw NGX consumes the
// two packed eye regions inside that reduced viewport and writes full-sized outputs.
// After each accepted handback, the post-HDR chain runs with temporary unity live scale;
// FO4VR's selected dynamic ImageSpace geometry is regenerated at unity and again after
// restoration so the complete stereo target is covered without a second upscale. A load-time
// ratio replacement skips one vendor frame and the requested four-float contract is
// reasserted after slot 21. The selected backend's own jitter sequence replaces the native
// eight-phase one in the main world for as long as it has live views. Restart to change
// this policy; false leaves all engine values untouched. Requires a non-DLAA quality mode:
// dlaa resolves to no reduction, so the reduced-world bracket is not armed for it whatever
// this key says, and its mip bias is applied and restored at the native one-to-one seams
// instead - the pre-world screen-geometry update and the late ImageSpace range.
US_SETTING(bSetting, "Diagnostics", diagnosticDynamicResolution, false)

// Development gate, off in every shipped/default configuration. When true, the
// verified top-level HDR callback performs exactly 512 descriptor-matched
// CopyResource round trips (logical 1 -> private -> logical 1) under D3D11.1
// context-state isolation, then makes itself inert. No shader or conversion runs;
// vanilla HDR still executes 1 -> 3. Any inconsistency disables the experiment for
// the session. This setting is safe to live-reload but enabling it is intentionally
// noisy in the log and /state.
US_SETTING(bSetting, "Diagnostics", diagnosticHdrRoundTrip, false)

// Development gate after diagnosticHdrRoundTrip. When true, the same verified HDR
// callback extracts both eyes into the shipping zero-origin intermediates, evaluates
// them independently through NullBackend, reassembles a private double-wide output
// and hands identical colour bytes back before vanilla HDR. Exactly 512 successful
// frames, then inert. The measured unjittered motion target is staged and supplied;
// logical DS1's standard-Z R24 depth plane stays whole and each eye supplies its exact
// source rectangle. NullBackend accepts and ignores both. Mutually exclusive with
// diagnosticHdrRoundTrip. Native jitter and the explicit backend-auto exposure contract
// are supplied; sharpening and temporal history are absent because NullBackend is the arm.
US_SETTING(bSetting, "Diagnostics", diagnosticStereoNull, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs one
// configured-quality raw-NGX preparation on the real FO4VR device: allocate the per-eye intermediates,
// initialize from runtimeDir, and create two independent DLAA histories under exact
// D3D11.1 state isolation. It does not extract, evaluate, reassemble or copy a frame.
// Requires backend="ngx" and is mutually exclusive with both copy diagnostics.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxCreate, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// configured-quality two-eye evaluation after the same explicit raw-NGX preparation.
// Native inputs use full eye extents; reconstruction inputs use packed reduced regions.
// Colour and motion are copied into private eye textures, DS1 is borrowed with exact eye
// extents, and vendor output stays private: no reassembly and no engine colour handback.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxEvaluate, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// configured-quality two-eye evaluation and reassembles both vendor outputs into a private
// descriptor-matched double-wide texture. The engine colour resource is never written.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxReassemble, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// configured-quality two-eye evaluation, private reassembly and descriptor-matched copy into
// logical colour 1 immediately before vanilla HDR. This is deliberately pixel-changing.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxHandback, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true first evaluates and
// reassembles 16 consecutive raw-NGX frames into the private double-wide resource
// without touching engine colour, then hands back exactly diagnosticContinuousFrames
// consecutive frames.
// Both histories stay alive across the complete bounded window. A failure before
// handback disables the gate and leaves engine colour untouched; a health failure
// detected after a completed copy cannot undo that copy.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxContinuous, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-AA FSR 3.1 evaluation on FO4VR's live D3D11 device. Colour, completed
// motion and zero-origin R32_FLOAT depth are private per-eye resources; both FSR outputs
// remain private and render-thread teardown runs on the next HDR frame. There is no
// reassembly or engine-colour copy. Requires backend="ffx" and an FFX-enabled build.
US_SETTING(bSetting, "Diagnostics", diagnosticFfxEvaluate, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-AA FSR 3.1 evaluation and reassembles both vendor outputs into the
// descriptor-matched private double-wide texture. The engine colour resource is never
// written. Render-thread teardown runs on the next HDR frame. Requires backend="ffx"
// and an FFX-enabled build.
US_SETTING(bSetting, "Diagnostics", diagnosticFfxReassemble, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-AA FSR 3.1 evaluation, reassembles both outputs into the private
// descriptor-matched double-wide texture and copies that result exactly once into
// logical engine colour 1 before vanilla HDR. Render-thread teardown runs on the next
// HDR frame. Requires backend="ffx" and an FFX-enabled build.
US_SETTING(bSetting, "Diagnostics", diagnosticFfxHandback, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true first evaluates and
// reassembles 16 consecutive native-AA FSR 3.1 frames into the private double-wide
// resource without touching engine colour, then hands back exactly
// diagnosticContinuousFrames consecutive frames. Both per-eye histories stay alive
// across the complete bounded window.
// A failure before handback disables the gate and leaves engine colour untouched; a
// health failure detected after a completed copy cannot undo that copy.
// Safe-release-checked teardown begins on the next eligible HDR frame after
// completion and retries once if GPU release is deferred. Requires backend="ffx" and an
// FFX-enabled build; mutually exclusive with every other pre-HDR diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticFfxContinuous, false)

// DEVELOPMENT ONLY. Number of accepted engine-handback frames used by either
// continuous vendor gate and by its coupled native-TAA replacement. The default
// preserves the already-accepted 512-frame proof. A larger value enables a bounded
// unattended soak without turning a research gate into an unbounded shipping path.
// Set before launch; changing it during an active episode is not a history reset.
US_SETTING(iSetting, "Diagnostics", diagnosticContinuousFrames, 512)

// DEVELOPMENT ONLY. false in the shipped configuration. After Bethesda's native draw,
// overwrites exactly diagnosticContinuousFrames TemporalAA results with a private
// full-screen format-converting pass that writes both the active 61/62 history target
// and compositor target 4, then
// becomes inert. The native draw still incurs its cost in this diagnostic.
// Alone it exercises every TAA frame. Together with diagnosticNgxContinuous or
// diagnosticFfxContinuous it snapshots that source's accepted-serial baseline, then runs
// only on baseline + 1 and consecutive source-tagged handbacks. It cannot be combined
// with any other pre-HDR diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticTaaReplacement, false)

// DEVELOPMENT ONLY. false in the shipped configuration. Replaces, rather than follows,
// exactly diagnosticContinuousFrames native prepared TAA draws. It resolves Bethesda's
// live history parity from
// a byte-verified RIP anchor, requires the engine's selected output to match the proved
// 61/62 ping-pong, then binds logical color 3 plus the expected history and submitted
// target 4 directly under exact state isolation. A validation/preparation failure runs
// vanilla instead. Alone it exercises every TAA frame; with diagnosticNgxContinuous or
// diagnosticFfxContinuous it aligns exactly to consecutive accepted handbacks from that
// source. Mutually exclusive with diagnosticTaaReplacement.
US_SETTING(bSetting, "Diagnostics", diagnosticTaaBypass, false)

// --- parsed, range-checked, live-reloaded and reported ---
//
// Most entries below remain staged ahead of the subsystem that consumes them. Each
// such key names the measurement that switches it on; eyeOrder is now honoured.

// "auto" / "ngx" / "streamline" / "ffx" / "null". auto probes in that order and
// takes the first that reports itself available; the log names every backend and
// why each was or was not chosen. "null" is the passthrough control arm - it
// installs the hooks and evaluates nothing, so an A/B against it measures our own
// overhead rather than the reconstruction.
// Inert until: a backend has something to evaluate.
US_SETTING(sSetting, "Fo4VRUpscaler", backend, std::string("auto"))

// Where the mod looks for vendor runtime DLLs. They are user-supplied and loaded
// by explicit path, never by search order, so a stray copy elsewhere on PATH
// cannot quietly win a bug report. The resolved path and the version the runtime
// actually reported are both logged.
// Inert until: a backend is selected.
US_SETTING(sSetting, "Fo4VRUpscaler", runtimeDir, std::string("Data/F4SE/Plugins/Fo4VRUpscaler"))

// Reconstruction preset. Raw NGX accepts "auto" or one letter, case-insensitive.
// "auto" supplies the SDK default sentinel so the runtime and its driver profile decide;
// the log names both the request and the parameter value reported after creation.
// Preset overrides forced by a driver profile are the largest source of contradictory
// folklore in this ecosystem. Deliberately a free string, not a fixed list: preset letters
// have been added, deprecated and removed inside eighteen months, and a
// compile-time list is how an integration gets stuck a preset behind.
// "auto" resolves today to K for dlaa, balanced and quality, M for performance and
// L for ultraperformance, and NVIDIA may change those defaults in a driver update
// without any change here. The log line after feature creation reports the value the
// runtime actually resolved, which is how such a change becomes visible.
// Inert until: a backend is selected.
US_SETTING(sSetting, "Fo4VRUpscaler", preset, std::string("auto"))

// 0..1. Our own RCAS sharpen at the TemporalAA seam, applied to the submitted target on
// every frame this plugin draws (DLAA and the reduced modes alike). The engine's own
// TemporalAA sharpens its output and the seam draw would otherwise drop that term, which
// through the lenses showed as native looking sharper than the reconstruction - an
// observation, not a measurement. Mapped to RCAS stops as 2 * (1 - sharpness): 1.0 is
// the kernel's maximum, 0.5 one stop down. 0 is off and bit-identical to no pass at all -
// the unsharpened shader stays bound. Never handed to a vendor runtime: raw NGX's own
// sharpen is deprecated and is forced to zero regardless. Sharpening after temporal
// reconstruction amplifies whatever the reconstruction got wrong; raise it last, not
// first. The default is an owner decision for the Phase 4 buttons, not a measurement.
US_SETTING(fSetting, "Fo4VRUpscaler", sharpness, 0.5)

// "dlaa" (native anti-aliasing, ratio 1.0) / "quality" / "balanced" /
// "performance" / "ultraperformance". dlaa is the honest default and it is the
// expensive one: it feeds four times the input pixels of the numbers most
// published benchmarks were measured at and has no mechanism to give frames back.
// If the frame time goes over budget the fix is to drop the headset or runtime
// resolution, not this - native anti-aliasing at a reduced output is strictly
// worse than a reconstruction mode at full output.
// Raw-NGX diagnostics honour this. A non-DLAA mode additionally requires
// diagnosticDynamicResolution and a valid reduced renderScaleOverride at startup.
//
// Deprecated: mode replaces this as the activation surface (see mode above, which
// shares this vocabulary plus "off"). quality is still declared and still drives
// what it drives today - nothing about ratio selection changes in this step - but
// a fresh configuration should set mode instead.
US_SETTING(sSetting, "Fo4VRUpscaler", quality, std::string("dlaa"))

// 0 = use the documented default ratio for the chosen quality mode - 0.6667,
// 0.5800, 0.5000 and 0.3333 for quality, balanced, performance and ultraperformance
// respectively, which is what each mode's training assumed - and then snap it, once
// the per-eye output extent is known, to whatever ratio makes the engine land on the
// vendor's own optimal render size. The plugin floors width times ratio when it
// configures the vendor, so the unsnapped default misses that size by a pixel: 1852
// by 0.6667 gives 1234 where the runtime asks for 1235. Any value in 0.30..1.00
// overrides it and is written verbatim with no snap, for measurement rather than
// tuning. Honoured at startup only when diagnosticDynamicResolution is true, and
// dlaa is left alone because its ratio is 1.0 and it is not a reduced mode. The
// engine's ratio is restart-only, so if the runtime's optimal size still disagrees
// after the snap the log says so and the session keeps running at the value it has.
US_SETTING(fSetting, "Fo4VRUpscaler", renderScaleOverride, 0.0)

// 999 = derive the texture mip bias from the configured per-eye render and output
// widths using the backend's own formula (they differ between vendors), plus the
// engine's own native bias, which is a measured 0.0. Any value in -8..8 is used
// verbatim, and 0.0 is a legitimate value, which is why the sentinel is not 0.
// The derived bias is what keeps textures sharp at reduced resolution, but vendor
// guidance warns it can produce flicker and moire - this is the escape hatch, and
// the band worth sweeping is log2(render/output)-1 up to log2(render/output). A
// value outside that band is honoured and logged, never clamped.
// A dlaa session is biased too, at -1 plus the engine's native bias, because the
// vendor's formula keeps that term at ratio 1.0. It has no reduced-world bracket to hang
// the swap on, so it applies at the pre-world screen-geometry update and restores at the
// late ImageSpace range instead; /state's mipBias.nativeApplyActive is how a run proves
// which of the two seams ran.
US_SETTING(fSetting, "Fo4VRUpscaler", mipBiasOverride, 999.0)

// false = bias only the anisotropic sampler class, which is the world material
// class. true also biases the trilinear class, which the engine shares between world
// material sampling and full-screen linear sampling, so a full-screen pass would
// start sampling its source one mip level sharper than it was written. Turn it on
// only if the sampler probe shows world geometry binding the trilinear class.
US_SETTING(bSetting, "Fo4VRUpscaler", mipBiasIncludeTrilinear, false)

// false = leave every sampler's MinLOD alone. FO4 sets MinLOD to the sampler's own
// mip index, which is exactly the global mip-bias clamp vendor guidance tells
// integrators to disable - with it in place a negative bias reaches nothing on 15 of
// every 16 entries. It is also the engine's texture-degrade mechanism
// (iTextureDegradeDistance0/1), so true trades that mechanism for the full bias and
// is off by default rather than because the clamp is harmless.
US_SETTING(bSetting, "Fo4VRUpscaler", mipBiasLowerMinLod, false)

// "subrect" reconstructs each eye in place against the double-wide targets using
// per-resource sub-rectangle bases: no colour copy in, no output copy out, and the
// motion sanitizer reads the engine target directly. "intermediates" copies each eye
// into its own zero-origin staging texture first. subrect is the default because the
// NGX runtime honours the bases bit for bit on D3D11 when asked at creation (measured
// against the zero-origin path by tools/subrect-harness). On a backend that cannot
// address them - ffx, streamline, and the ngx stub of a build compiled without the
// NGX SDK, which is the default build - the plugin resolves to intermediates for the
// session and says so in the log, and /state reports both the requested and the
// effective mode. A flip takes effect on reload and re-creates the backend views.
US_SETTING(sSetting, "Fo4VRUpscaler", stereoMode, std::string("subrect"))

// "auto" / "LR" / "RL". Which half of the double-wide frame is the left eye. auto
// uses the left-first layout directly verified from FO4VR 1.2.72's shipping deferred
// shader bytecode and compositor submit bounds. The override stays as a checkable
// recovery control and takes effect on reload.
US_SETTING(sSetting, "Fo4VRUpscaler", eyeOrder, std::string("auto"))

// "engine" reads the game's own projection translation and converts it to render-pixel
// camera jitter at the backend seam. That conversion is proven, and its units, signs and
// amplitude are already right at every ratio; only the eight-phase sequence length is
// too short for reconstruction.
// The selected backend now owns the sequence whenever it has live views and the engine's
// jitter call site is patched: it writes its own sample into the same two floats, and the
// evaluation refuses any frame where the engine no longer holds that exact sample. Both
// spellings therefore describe the same read path, and neither selects between them.
// Inert until: there is a reason to run the shorter engine sequence deliberately.
US_SETTING(sSetting, "Fo4VRUpscaler", jitterSource, std::string("engine"))

// 0 = use the vendor minimum for the ratio. Sequence length scales with the
// square of the upscale ratio, so a sequence tuned for one-to-one temporal
// anti-aliasing is far too short for reconstruction, and the symptom is shimmer
// rather than an error message.
// The armed count already comes from the selected backend's own formula - eight at
// dlaa, eighteen at quality - so a shorter or longer count is an experiment nothing
// currently needs.
// Inert until: an experiment needs a count the backend would not choose.
US_SETTING(iSetting, "Fo4VRUpscaler", jitterPhaseOverride, std::int64_t(0))

// Build a per-pixel reactive mask so reticles, muzzle flash and particles are
// reconstructed as the transient things they are. This is the alternative to the
// current answer in this ecosystem, which is to switch the upscaler off entirely
// while you are aiming.
// Inert until: the input colour range is known and the mask pass exists.
US_SETTING(bSetting, "Fo4VRUpscaler", reactiveMask, true)

// Skip reconstruction while the Pip-Boy or a 3D interface is up. Whether the
// engine's motion vectors stay valid across an open menu is one of the
// measurements this key waits on, and reconstructing stale vectors smears text -
// which is the one thing a Pip-Boy is for.
// Inert until: the interface render paths are identified.
US_SETTING(bSetting, "Fo4VRUpscaler", excludePipboy, true)

// The game's own radial density mask writes a checkerboard that a temporal
// reconstructor reads as real detail and smears across the periphery. Turning it
// off is a precondition, not a preference.
// Inert until: the setting's engine-side location is confirmed.
US_SETTING(bSetting, "Fo4VRUpscaler", forceRadialDensityMaskOff, true)

// Reduced rasterization leaves the headset's hidden-area regions unwritten, but
// reconstruction consumes a rectangular eye. False keeps the engine setting intact
// and makes reduced-input modes decline rather than publish incomplete pixels.
// Startup-only; restart after changing it.
US_SETTING(bSetting, "Fo4VRUpscaler", forceHiddenAreaMeshOff, true)

// Experimental raw-context depth-stencil bracket for prepared ImageSpace draws.
// Engine state commits may rebind the cached DSV before the draw, so this is disabled
// until a cache-native bracket has complete call-site coverage and draw-time proof.
US_SETTING(bSetting, "Fo4VRUpscaler", unbindDepthStencil, false)

// Reset both eye histories when a menu opens or closes. Both, always together:
// resetting one eye and not the other is a stereo-rivalry artifact, which is
// worse than the ghost it fixed.
US_SETTING(bSetting, "Fo4VRUpscaler", resetOnMenu, true)

// Reset after the F4SE game-loaded lifecycle edge.
US_SETTING(bSetting, "Fo4VRUpscaler", resetOnLoad, true)

// Reset when the player enters a different cell. This engine-owned event catches
// direct COC and worldspace transitions independently of menu timing.
US_SETTING(bSetting, "Fo4VRUpscaler", resetOnCell, true)

// Reset on a True Scopes scope-raise edge. The scope render is a second,
// mid-frame world render at a different field of view; the history from before it
// is not history of this view.
// Inert until: there is a history to reset.
US_SETTING(bSetting, "Fo4VRUpscaler", resetOnScope, true)

// Skip our evaluate entirely while True Scopes' own deferred resolve is on the
// stack. Conservative by design - the two mods share a render thread and a
// render-target set, and coexisting quietly is worth more than the frame we would
// save.
// Wired to Fallout4VR's own renderer+4 scoped-pass selector. The interaction still
// needs a live True Scopes acceptance pass.
US_SETTING(bSetting, "Fo4VRUpscaler", suppressDuringScopeRender, true)

// Per-stage GPU timestamp queries plus CPU marks, reported by the dev tooling.
// Flip off then on to reset the accumulators for a clean measurement window. This
// is the only way to answer what this mod actually costs without a debugger.
// Inert until: there are stages to time.
US_SETTING(bSetting, "Diagnostics", perfTimers, false)

// "" / "mv" / "depth" / "jitter" / "reactive". Draws the named input over the
// frame, in the headset. Motion vectors are the input most likely to be wrong and
// the one nobody can check by looking at the final image.
// Inert until: the inputs are identified.
US_SETTING(sSetting, "Diagnostics", debugVisualise, std::string(""))

#undef US_SETTING
