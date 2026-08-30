// The one list of TOML keys.
//
// Included twice with different US_SETTING definitions: Settings.h declares and
// loads the settings from it, DevBench.cpp builds the config route's table from
// it. One list, two expansions, so the declared set and the bench's view of it
// cannot drift - the reference repo hand-maintained the second copy and the two
// disagreed.
//
//   US_SETTING(a_type, a_key, a_default)
//     a_type     bSetting | iSetting | fSetting | sSetting. Doubles as the kind
//                the config route switches on, so a string setting is readable
//                and writable there instead of invisible.
//     a_key      the TOML key and the C++ identifier, spelled once.
//     a_default  the value used when the key is absent.
//
// Comments here are the user documentation for each knob and must say the same
// thing as the shipped TOML. Keys this build parses but does not act on say so
// and name the measurement that switches them on.
//
// No include guard, on purpose. US_SETTING is undefined at the bottom, so a
// second expansion in the same translation unit is a plain re-include.

#ifndef US_SETTING
#	error "define US_SETTING(a_type, a_key, a_default) before including SettingsList.inl"
#endif

// --- honoured by this build ---

// Master switch. false = no hooks are installed and nothing is patched. The
// self-check, the settings routes and the dev tooling stay live, so a disabled
// install still writes a diagnosable log. Read once at load; restart to change.
US_SETTING(bSetting, enabled, true)

// 0 = off, 1 = warnings and worse, 2 = info, 3 = trace. Level 3 logs every frame
// decision and is a measurable cost; it exists so a bug report can carry a trace
// instead of a guess. The startup banner is written before this is parsed, so it
// always appears.
US_SETTING(iSetting, logLevel, std::int64_t(2))

// Register the plugin's tools into the devbench host when it is present. When it
// is absent this is a no-op and the log says so in one line - the dev tooling is
// never a load-order requirement.
US_SETTING(bSetting, devBench, true)

// On any backend fault, disable the evaluate for the rest of the session and pass
// the frame through untouched. false re-arms after each fault, which is how a
// faulting configuration gets bisected by TOML edits instead of restarts. Never
// ship false.
US_SETTING(bSetting, failSoft, true)

// Development gate, off in every shipped/default configuration. When true, the
// verified top-level HDR callback performs exactly 512 descriptor-matched
// CopyResource round trips (logical 1 -> private -> logical 1) under D3D11.1
// context-state isolation, then makes itself inert. No shader or conversion runs;
// vanilla HDR still executes 1 -> 3. Any inconsistency disables the experiment for
// the session. This setting is safe to live-reload but enabling it is intentionally
// noisy in the log and /state.
US_SETTING(bSetting, diagnosticHdrRoundTrip, false)

// Development gate after diagnosticHdrRoundTrip. When true, the same verified HDR
// callback extracts both eyes into the shipping zero-origin intermediates, evaluates
// them independently through NullBackend, reassembles a private double-wide output
// and hands identical colour bytes back before vanilla HDR. Exactly 512 successful
// frames, then inert. The measured unjittered motion target is staged and supplied;
// logical DS1's standard-Z R24 depth plane stays whole and each eye supplies its exact
// source rectangle. NullBackend accepts and ignores both. Mutually exclusive with
// diagnosticHdrRoundTrip. Native jitter and the explicit backend-auto exposure contract
// are supplied; sharpening and temporal history are absent because NullBackend is the arm.
US_SETTING(bSetting, diagnosticStereoNull, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs one native-size
// raw-NGX preparation on the real FO4VR device: allocate the per-eye intermediates,
// initialize from runtimeDir, and create two independent DLAA histories under exact
// D3D11.1 state isolation. It does not extract, evaluate, reassemble or copy a frame.
// Requires backend="ngx" and is mutually exclusive with both copy diagnostics.
US_SETTING(bSetting, diagnosticNgxCreate, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-DLAA evaluation after the same explicit raw-NGX preparation. Colour
// and motion are copied into private eye textures, DS1 is borrowed with exact eye
// extents, and vendor output stays private: no reassembly and no engine colour handback.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, diagnosticNgxEvaluate, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-DLAA evaluation and reassembles both vendor outputs into a private
// descriptor-matched double-wide texture. The engine colour resource is never written.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, diagnosticNgxReassemble, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-DLAA evaluation, private reassembly and descriptor-matched copy into
// logical colour 1 immediately before vanilla HDR. This is deliberately pixel-changing.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, diagnosticNgxHandback, false)

// DEVELOPMENT ONLY. false in the shipped configuration. true first evaluates and
// reassembles 16 consecutive raw-NGX frames into the private double-wide resource
// without touching engine colour, then hands back exactly 512 consecutive frames.
// Both histories stay alive across the complete bounded window. The first failure
// disables the gate for the session and vanilla HDR receives the untouched frame.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, diagnosticNgxContinuous, false)

// DEVELOPMENT ONLY. false in the shipped configuration. After Bethesda's native draw,
// overwrites exactly 512 TemporalAA results with a private full-screen format-converting
// pass that writes both the active 61/62 history target and compositor target 4, then
// becomes inert. The native draw still incurs its cost in this diagnostic.
// Alone it exercises every TAA frame. Together with diagnosticNgxContinuous it runs only
// after that frame's accepted NGX handback, so its 512 draws align exactly with the 512
// vendor copies. It cannot be combined with any of the other six diagnostics.
US_SETTING(bSetting, diagnosticTaaReplacement, false)

// DEVELOPMENT ONLY. false in the shipped configuration. Replaces, rather than follows,
// exactly 512 native prepared TAA draws. It resolves Bethesda's live history parity from
// a byte-verified RIP anchor, requires the engine's selected output to match the proved
// 61/62 ping-pong, then binds logical color 3 plus the expected history and submitted
// target 4 directly under exact state isolation. A validation/preparation failure runs
// vanilla instead. Alone it exercises every TAA frame; with diagnosticNgxContinuous it
// aligns exactly to accepted handbacks. Mutually exclusive with diagnosticTaaReplacement.
US_SETTING(bSetting, diagnosticTaaBypass, false)

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
US_SETTING(sSetting, backend, std::string("auto"))

// Where the mod looks for vendor runtime DLLs. They are user-supplied and loaded
// by explicit path, never by search order, so a stray copy elsewhere on PATH
// cannot quietly win a bug report. The resolved path and the version the runtime
// actually reported are both logged.
// Inert until: a backend is selected.
US_SETTING(sSetting, runtimeDir, std::string("Data/F4SE/Plugins/Fo4VRUpscaler"))

// Reconstruction preset, passed straight through to whatever runtime is loaded.
// "auto" sets nothing and lets the runtime and its driver profile decide - and
// the log then names what actually resolved, which is the point: preset overrides
// forced by a driver profile are the largest source of contradictory folklore in
// this ecosystem. Deliberately a free string, not a fixed list: preset letters
// have been added, deprecated and removed inside eighteen months, and a
// compile-time list is how an integration gets stuck a preset behind.
// Inert until: a backend is selected.
US_SETTING(sSetting, preset, std::string("auto"))

// 0..1, applied by the backend when it sharpens its own output and by our own
// pass when it does not. 0 = off. Sharpening after temporal reconstruction
// amplifies whatever the reconstruction got wrong; raise it last, not first.
// Inert until: there is an output to sharpen.
US_SETTING(fSetting, sharpness, 0.0)

// "dlaa" (native anti-aliasing, ratio 1.0) / "quality" / "balanced" /
// "performance" / "ultraperformance". dlaa is the honest default and it is the
// expensive one: it feeds four times the input pixels of the numbers most
// published benchmarks were measured at and has no mechanism to give frames back.
// If the frame time goes over budget the fix is to drop the headset or runtime
// resolution, not this - native anti-aliasing at a reduced output is strictly
// worse than a reconstruction mode at full output.
// Inert until: the render-resolution path exists.
US_SETTING(sSetting, quality, std::string("dlaa"))

// 0 = use the backend's own optimal render size for the chosen quality mode,
// which is what its training assumed. Any value in 0.30..1.00 overrides it. This
// exists for measurement, not for tuning.
// Inert until: the plugin owns the render resolution.
US_SETTING(fSetting, renderScaleOverride, 0.0)

// 999 = derive the texture mip bias per frame from the real ratio using the
// backend's own formula (they differ between vendors). Any value in -8..8 is used
// verbatim, and 0.0 is a legitimate value, which is why the sentinel is not 0.
// The derived bias is what keeps textures sharp at reduced resolution, but vendor
// guidance warns it can produce flicker and moire - this is the escape hatch.
// Inert until: the sampler-state table is located and a ratio exists.
US_SETTING(fSetting, mipBiasOverride, 999.0)

// "intermediates" copies each eye into its own zero-origin staging texture before
// reconstruction; "subrect" reconstructs directly against the double-wide target
// using per-resource sub-rectangle bases. intermediates is the default because
// one runtime has been documented rejecting non-zero sub-rect offsets on this
// graphics API and the other has no sub-rect bases at any offset. subrect is
// faster and unproven; the self-check refuses it on a backend whose capability
// probe says no.
// Inert until: the sub-rect harness answers, and stereo geometry is resolved.
US_SETTING(sSetting, stereoMode, std::string("intermediates"))

// "auto" / "LR" / "RL". Which half of the double-wide frame is the left eye. auto
// uses the left-first layout directly verified from FO4VR 1.2.72's shipping deferred
// shader bytecode and compositor submit bounds. The override stays as a checkable
// recovery control and takes effect on reload.
US_SETTING(sSetting, eyeOrder, std::string("auto"))

// "engine" reads the game's own sub-pixel offset back out and hands it to the
// backend unchanged; "backend" overwrites it with the sequence the backend was
// trained on. engine is the default because the units of the engine's two offset
// floats on this build are not measured, and writing them at half magnitude
// produces a picture that looks right and samples wrong - the hardest class of
// bug in this project to see. "backend" is required for any mode other than dlaa.
// Inert until: the jitter divisor is measured (per-eye width or double-wide).
US_SETTING(sSetting, jitterSource, std::string("engine"))

// 0 = use the vendor minimum for the ratio. Sequence length scales with the
// square of the upscale ratio, so a sequence tuned for one-to-one temporal
// anti-aliasing is far too short for reconstruction, and the symptom is shimmer
// rather than an error message.
// Inert until: jitterSource can be "backend".
US_SETTING(iSetting, jitterPhaseOverride, std::int64_t(0))

// Build a per-pixel reactive mask so reticles, muzzle flash and particles are
// reconstructed as the transient things they are. This is the alternative to the
// current answer in this ecosystem, which is to switch the upscaler off entirely
// while you are aiming.
// Inert until: the input colour range is known and the mask pass exists.
US_SETTING(bSetting, reactiveMask, true)

// Skip reconstruction while the Pip-Boy or a 3D interface is up. Whether the
// engine's motion vectors stay valid across an open menu is one of the
// measurements this key waits on, and reconstructing stale vectors smears text -
// which is the one thing a Pip-Boy is for.
// Inert until: the interface render paths are identified.
US_SETTING(bSetting, excludePipboy, true)

// The game's own radial density mask writes a checkerboard that a temporal
// reconstructor reads as real detail and smears across the periphery. Turning it
// off is a precondition, not a preference.
// Inert until: the setting's engine-side location is confirmed.
US_SETTING(bSetting, forceRadialDensityMaskOff, true)

// Unbind the depth-stencil across our own fullscreen passes. Leaving it bound
// lets the headset's hidden-area mesh stencil clip our output at the corners.
// Inert until: our own passes exist.
US_SETTING(bSetting, unbindDepthStencil, true)

// Reset both eye histories when a menu opens or closes. Both, always together:
// resetting one eye and not the other is a stereo-rivalry artifact, which is
// worse than the ghost it fixed.
US_SETTING(bSetting, resetOnMenu, true)

// Reset after the F4SE game-loaded lifecycle edge. Cell and fast-travel transitions
// currently rely on their menu edges; a direct player-cell edge remains unverified.
US_SETTING(bSetting, resetOnLoad, true)

// Reset on a True Scopes scope-raise edge. The scope render is a second,
// mid-frame world render at a different field of view; the history from before it
// is not history of this view.
// Inert until: there is a history to reset.
US_SETTING(bSetting, resetOnScope, true)

// Skip our evaluate entirely while True Scopes' own deferred resolve is on the
// stack. Conservative by design - the two mods share a render thread and a
// render-target set, and coexisting quietly is worth more than the frame we would
// save.
// Inert until: both mods run together and the interaction is measured.
US_SETTING(bSetting, suppressDuringScopeRender, true)

// Per-stage GPU timestamp queries plus CPU marks, reported by the dev tooling.
// Flip off then on to reset the accumulators for a clean measurement window. This
// is the only way to answer what this mod actually costs without a debugger.
// Inert until: there are stages to time.
US_SETTING(bSetting, perfTimers, false)

// "" / "mv" / "depth" / "jitter" / "reactive". Draws the named input over the
// frame, in the headset. Motion vectors are the input most likely to be wrong and
// the one nobody can check by looking at the final image.
// Inert until: the inputs are identified.
US_SETTING(sSetting, debugVisualise, std::string(""))

#undef US_SETTING
