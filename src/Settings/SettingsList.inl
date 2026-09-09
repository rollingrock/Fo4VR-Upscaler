// The one list of TOML keys.
//
// Included twice with different US_SETTING definitions: Settings.h declares and
// loads the settings from it, DevBench.cpp builds the config route's table from
// it. One list, two expansions, so the declared set and the bench's view of it
// cannot drift - the reference repo hand-maintained the second copy and the two
// disagreed.
//
//   US_SETTING(a_type, a_section, a_key, a_default, a_restart)
//     a_type     bSetting | iSetting | fSetting | sSetting. Doubles as the kind
//                the config route switches on, so a string setting is readable
//                and writable there instead of invisible.
//     a_section  the TOML table name and the /config group, as a quoted string
//                literal. Exactly two exist: "Fo4VRUpscaler" (what a player
//                needs to touch to play) and "Diagnostics" (the regression
//                harness's keys; nothing in it is needed to play).
//     a_key      the TOML key and the C++ identifier, spelled once.
//     a_default  the value used when the key is absent from both tables.
//     a_restart  Settings::kLive or Settings::kRestart - whether a value written
//                through this key takes effect on the next Configuration::Apply
//                (an ordinary reload, kGameLoaded, /config/reload or an accepted
//                /config/set) or needs the process restarted. Declared here,
//                beside the key itself, so a new key cannot be added without one
//                and the compiler catches every miss - see Settings::FieldDescriptors
//                and Settings.h's RestartPolicy comment for how this was derived.
//
// Comments here are the user documentation for each knob and must say the same
// thing as the shipped TOML. Keys this build parses but does not act on say so
// and name the measurement that switches them on. Where a_default and the shipped
// file's value differ on purpose - preset does - the comment says which is which:
// a_default is what a configuration missing the key entirely gets, and the shipped
// value carries the recommendation.
//
// A per-user Fo4VRUpscalerOverride.toml beside the log (Settings::Paths::
// OverrideFilePath) is layered over the shipped file and wins per key, so a live
// value can come from either. It holds only the keys the mod itself was asked to
// change.
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
#	error "define US_SETTING(a_type, a_section, a_key, a_default, a_restart) before including SettingsList.inl"
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
US_SETTING(bSetting, "Fo4VRUpscaler", enabled, true, Settings::kRestart)

// "off" / "dlaa" (native anti-aliasing, ratio 1.0) / "quality" / "balanced" /
// "performance" / "ultraperformance". The activation surface. A mode other than
// "off" derives the vendor path (backend below), the native-TAA bypass and, for a
// reduced mode, the reduced-world bracket and its ratio - the same runtime inputs
// the [Diagnostics] gates below take, computed here instead of set by hand. The
// gate then runs with no target and no self-retirement: reconstruction continues
// for as long as the game session lasts, not for a bounded frame count. mode is
// refused alongside an armed diagnostic gate - the load forces mode to "off" and
// names the conflict - so the two are never both live; mode = "off" leaves the
// [Diagnostics] gates in charge exactly as before.
// dlaa is the shipped default and the honest one: it feeds four times the
// input pixels of the numbers most published benchmarks were measured at and
// has no mechanism to give frames back. quality/balanced/performance/
// ultraperformance name a reconstruction ratio, from least to most reduced.
// If mode is absent from the file and the deprecated quality key is present,
// quality's value is used and the log names the substitution. If both are
// present and disagree, mode wins and the log names the conflict. See enabled
// above for how enabled = false interacts with mode.
// A reduced mode needs a backend that can reconstruct one: beside backend =
// "ffx", "streamline" or "null" it resolves to "dlaa" and the log names the
// pair, because those paths would leave the world rasterized small with nothing
// putting the pixels back for the whole session. See backend below.
// Live, within one rule: "off" is always reachable, and once a session has
// started, whether it rasterizes a reduced world at all is frozen - so a reduced
// mode is reachable live only on a session that armed the bracket, and "dlaa"
// only on one that did not. A change that breaks that rule - through /mode, a
// /config/set, or an edit to this file plus a reload - is refused with the reason
// named, the session keeps the mode it is running, and the file still decides the
// next launch.
US_SETTING(sSetting, "Fo4VRUpscaler", mode, std::string("dlaa"), Settings::kLive)

// 0 = off, 1 = warnings and worse, 2 = info, 3 = trace. Level 3 logs every frame
// decision and is a measurable cost; it exists so a bug report can carry a trace
// instead of a guess. The startup banner is written before this is parsed, so it
// always appears.
US_SETTING(iSetting, "Diagnostics", logLevel, std::int64_t(2), Settings::kLive)

// Startup-only. Register the plugin's tools into the devbench host when it is present.
// When it is absent this is a no-op and the log says so in one line - the dev tooling
// is never a load-order requirement.
US_SETTING(bSetting, "Diagnostics", devBench, true, Settings::kRestart)

// Connected. true (default): a fault that leaves the quality transaction closed, the
// device intact and TAAReplacement's/Hooks' own latches clear suspends the session
// instead of ending it - the existing safe-release teardown runs, a 90-frame cool-down
// counts down, then the existing preparation path (create, warm-up, evaluate) starts
// again, up to 8 re-arms per session. false: the first fault ends the session for good,
// exactly as before this existed - set it to reproduce that behaviour or to bisect
// whether a re-arm caused a problem a bug report describes. TAAReplacement's and Hooks'
// own latches are themselves one-way and are not re-armed by this setting, so a fault
// that trips either one is terminal regardless of failSoft.
US_SETTING(bSetting, "Fo4VRUpscaler", failSoft, true, Settings::kLive)

// Win32 virtual-key code TOGGLE listens for, polled once per frame with
// GetAsyncKeyState and edge-detected - no controller binding, no OpenVR action
// manifest, no engine address. A press switches the live mode between "off" and the
// mode to come back to: the last non-off mode that was actually live while it is still
// reachable, otherwise whichever reduced mode opened the reduced-world bracket, or
// "dlaa" when no bracket is open (see mode above, and
// Settings::Policy::ResolveModeSwitch). Going straight from one reduced mode to
// another is a /mode request or an override-file change, not this key. The switch
// outcome is reported rather than assumed - a refused switch stays visible in /state
// and the log.
// Neither key is read on a frame where Fallout 4 VR does not own the foreground window
// or the console is open, so a keypad key typed into a command, or into another
// application while the game runs behind it, does nothing. 106 is
// VK_MULTIPLY, the keypad * key. 0 disables the binding; a value outside the Win32
// virtual-key range 1..254 is also disabled, with a warning naming the rejected
// value.
US_SETTING(iSetting, "Fo4VRUpscaler", hotkeyToggleVk, std::int64_t(106), Settings::kLive)

// Win32 virtual-key code COMPARE listens for, polled the same way as
// hotkeyToggleVk. Holding it down engages the operator A/B veto exactly as /ab's
// veto=true does (the world gate denies every frame and the engine renders native);
// releasing it clears only the engagement this key itself created, never a veto set
// deliberately through /ab. 111 is VK_DIVIDE, the keypad / key. 0 disables the
// binding; same out-of-range handling as hotkeyToggleVk.
US_SETTING(iSetting, "Fo4VRUpscaler", hotkeyCompareVk, std::int64_t(111), Settings::kLive)

// The in-headset panel: an OpenVR overlay parented to the headset, drawn only on the
// frames it is showing. false destroys the overlay at the next frame, unbinds
// hotkeyPanelVk, and leaves the rest of the mod running exactly as before - a hidden
// panel already costs nothing per frame, so this switch is for turning the feature off
// rather than for reclaiming anything. A runtime that serves no overlay interface (an
// OpenXR translation layer may not) reports the panel unavailable with the runtime's own
// error name in the log and in the dev tooling's /state, and nothing else changes.
US_SETTING(bSetting, "Fo4VRUpscaler", panelEnabled, true, Settings::kLive)

// Win32 virtual-key code that shows and hides the panel, polled once per frame the same
// way as hotkeyToggleVk and gated the same way - not read while another application owns
// the foreground window or the console is open. 122 is F11. F9 is the vanilla quickload
// key and F10 carries Win32's own menu-bar semantics (both REPORTED - the default control
// map and Microsoft's documentation), so neither is offered as the default. 0 disables
// the binding; a value outside the Win32 virtual-key range 1..254 is also disabled, with
// a warning naming the rejected value. panelEnabled = false unbinds this key whatever it
// says.
US_SETTING(iSetting, "Fo4VRUpscaler", hotkeyPanelVk, std::int64_t(122), Settings::kLive)

// How wide the panel is in the world, in metres. Clamped to 0.2 through 2.0; outside
// that it is reset to 0.6 with a warning. Read when the overlay is created, so a live
// change takes effect the next time the panel is created, not on the next frame it is
// shown.
US_SETTING(fSetting, "Fo4VRUpscaler", panelWidthMeters, 0.6, Settings::kLive)

// Where the panel stays once it is open. "room" places it at each show in front of
// wherever you were looking at that moment and leaves it there, so you can look away from
// it and back at it; "head" keeps it hanging in front of your face and moving with every
// glance. Read at each show, so a change takes effect the next time you open the panel.
// "room" needs the runtime to answer for the headset's own pose - if it will not, that
// show is head-anchored instead and Fo4VRUpscaler.log names the interface that was
// missing. Anything but these two words is reset to "room" with a warning.
US_SETTING(sSetting, "Fo4VRUpscaler", panelAnchor, std::string("room"), Settings::kLive)

// How far ahead of you the panel is placed, in metres, measured from the eye. It also
// sits 0.15 m below the eye line so it does not cover what is being compared. With
// panelAnchor = "room" that is a distance in the room, along where you were looking when
// you opened it, with the head's tilt discarded so the panel stands upright. Clamped to
// 0.3 through 3.0; outside that it is reset to 1.0 with a warning. Read at each show,
// like panelAnchor; panelWidthMeters is read when the overlay is created.
US_SETTING(fSetting, "Fo4VRUpscaler", panelDistanceMeters, 1.0, Settings::kLive)

// DEVELOPMENT ONLY and startup-only. When true, renderScaleOverride - a value in
// 0.30..1.00, or 0 for the quality mode's own default ratio - is written into
// Fallout 4 VR's byte-verified live and target
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
// mode drives the equivalent bracket directly when it is not "off"; this key stays for
// the regression harness (mode = "off").
US_SETTING(bSetting, "Diagnostics", diagnosticDynamicResolution, false, Settings::kRestart)

// Development gate, off in every shipped/default configuration. When true, the
// verified top-level HDR callback performs exactly 512 descriptor-matched
// CopyResource round trips (logical 1 -> private -> logical 1) under D3D11.1
// context-state isolation, then makes itself inert. No shader or conversion runs;
// vanilla HDR still executes 1 -> 3. Any inconsistency disables the experiment for
// the session. This setting is safe to live-reload but enabling it is intentionally
// noisy in the log and /state.
US_SETTING(bSetting, "Diagnostics", diagnosticHdrRoundTrip, false, Settings::kLive)

// Development gate after diagnosticHdrRoundTrip. When true, the same verified HDR
// callback extracts both eyes into the shipping zero-origin intermediates, evaluates
// them independently through NullBackend, reassembles a private double-wide output
// and hands identical colour bytes back before vanilla HDR. Exactly 512 successful
// frames, then inert. The measured unjittered motion target is staged and supplied;
// logical DS1's standard-Z R24 depth plane stays whole and each eye supplies its exact
// source rectangle. NullBackend accepts and ignores both. Mutually exclusive with
// diagnosticHdrRoundTrip. Native jitter and the explicit backend-auto exposure contract
// are supplied; sharpening and temporal history are absent because NullBackend is the arm.
US_SETTING(bSetting, "Diagnostics", diagnosticStereoNull, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs one
// configured-quality raw-NGX preparation on the real FO4VR device: allocate the per-eye intermediates,
// initialize from runtimeDir, and create two independent DLAA histories under exact
// D3D11.1 state isolation. It does not extract, evaluate, reassemble or copy a frame.
// Requires backend="ngx" and is mutually exclusive with both copy diagnostics.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxCreate, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// configured-quality two-eye evaluation after the same explicit raw-NGX preparation.
// Native inputs use full eye extents; reconstruction inputs use packed reduced regions.
// Colour and motion are copied into private eye textures, DS1 is borrowed with exact eye
// extents, and vendor output stays private: no reassembly and no engine colour handback.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxEvaluate, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// configured-quality two-eye evaluation and reassembles both vendor outputs into a private
// descriptor-matched double-wide texture. The engine colour resource is never written.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxReassemble, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// configured-quality two-eye evaluation, private reassembly and descriptor-matched copy into
// logical colour 1 immediately before vanilla HDR. This is deliberately pixel-changing.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
US_SETTING(bSetting, "Diagnostics", diagnosticNgxHandback, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. true first evaluates and
// reassembles 16 consecutive raw-NGX frames into the private double-wide resource
// without touching engine colour, then hands back exactly diagnosticContinuousFrames
// consecutive frames.
// Both histories stay alive across the complete bounded window. A failure before
// handback disables the gate and leaves engine colour untouched; a health failure
// detected after a completed copy cannot undo that copy.
// Requires backend="ngx" and is mutually exclusive with every other diagnostic.
// mode drives the same gate directly, with no bound, when it is not "off" and
// backend resolves to ngx; this key stays for the regression harness (mode = "off").
US_SETTING(bSetting, "Diagnostics", diagnosticNgxContinuous, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-AA FSR 3.1 evaluation on FO4VR's live D3D11 device. Colour, completed
// motion and zero-origin R32_FLOAT depth are private per-eye resources; both FSR outputs
// remain private and render-thread teardown runs on the next HDR frame. There is no
// reassembly or engine-colour copy. Requires backend="ffx" and an FFX-enabled build.
US_SETTING(bSetting, "Diagnostics", diagnosticFfxEvaluate, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-AA FSR 3.1 evaluation and reassembles both vendor outputs into the
// descriptor-matched private double-wide texture. The engine colour resource is never
// written. Render-thread teardown runs on the next HDR frame. Requires backend="ffx"
// and an FFX-enabled build.
US_SETTING(bSetting, "Diagnostics", diagnosticFfxReassemble, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. true performs exactly one
// two-eye native-AA FSR 3.1 evaluation, reassembles both outputs into the private
// descriptor-matched double-wide texture and copies that result exactly once into
// logical engine colour 1 before vanilla HDR. Render-thread teardown runs on the next
// HDR frame. Requires backend="ffx" and an FFX-enabled build.
US_SETTING(bSetting, "Diagnostics", diagnosticFfxHandback, false, Settings::kLive)

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
// mode drives the same gate directly, with no bound, when it is not "off" and
// backend = "ffx"; this key stays for the regression harness (mode = "off").
US_SETTING(bSetting, "Diagnostics", diagnosticFfxContinuous, false, Settings::kLive)

// DEVELOPMENT ONLY. Number of accepted engine-handback frames used by either
// continuous vendor gate and by its coupled native-TAA replacement, on the
// diagnostic path only (mode = "off", one of the two gates above armed instead). A
// mode other than "off" runs the same two gates with no target and no
// self-retirement: reconstruction continues for as long as the game session
// lasts, and this key is not read. The default preserves the already-accepted
// 512-frame proof. A larger value enables a bounded unattended soak without
// turning a research gate into an unbounded shipping path. Set before launch;
// changing it during an active episode is not a history reset.
US_SETTING(iSetting, "Diagnostics", diagnosticContinuousFrames, 512, Settings::kRestart)

// DEVELOPMENT ONLY. false in the shipped configuration. After Bethesda's native draw,
// overwrites exactly diagnosticContinuousFrames TemporalAA results with a private
// full-screen format-converting pass that writes both the active 61/62 history target
// and compositor target 4, then
// becomes inert. The native draw still incurs its cost in this diagnostic.
// Alone it exercises every TAA frame. Together with diagnosticNgxContinuous or
// diagnosticFfxContinuous it snapshots that source's accepted-serial baseline, then runs
// only on baseline + 1 and consecutive source-tagged handbacks. It cannot be combined
// with any other pre-HDR diagnostic. mode is refused alongside an armed
// diagnosticTaaReplacement, the same as the pre-HDR gates above - the load forces mode
// to "off" so the two are never both live; the product path forces the native-TAA
// bypass on its own and has no post-draw replacement of its own to run instead.
US_SETTING(bSetting, "Diagnostics", diagnosticTaaReplacement, false, Settings::kLive)

// DEVELOPMENT ONLY. false in the shipped configuration. Replaces, rather than follows,
// exactly diagnosticContinuousFrames native prepared TAA draws. It resolves Bethesda's
// live history parity from
// a byte-verified RIP anchor, requires the engine's selected output to match the proved
// 61/62 ping-pong, then binds logical color 3 plus the expected history and submitted
// target 4 directly under exact state isolation. A validation/preparation failure runs
// vanilla instead. Alone it exercises every TAA frame; with diagnosticNgxContinuous or
// diagnosticFfxContinuous it aligns exactly to consecutive accepted handbacks from that
// source. Mutually exclusive with diagnosticTaaReplacement.
// mode drives this same bypass directly, with no bound, whenever it is not "off"; this
// key stays for the regression harness (mode = "off"). Explicitly arming it alongside a
// non-off mode is refused the same way: the load forces mode to "off" rather than
// leaving a redundant, unacknowledged bypass request sitting beside the one mode
// already forces.
US_SETTING(bSetting, "Diagnostics", diagnosticTaaBypass, false, Settings::kLive)

// --- parsed, range-checked and reported ---
//
// Most entries below take effect on the next Configuration::Apply; of the six kRestart
// keys only backend and forceHiddenAreaMeshOff are below this line - enabled, devBench,
// diagnosticDynamicResolution and diagnosticContinuousFrames are declared above it,
// which is why the shipped TOML, ordered differently, names five below its own copy of
// this note. Each declaration's own a_restart argument
// is what /config reports. Some remain staged ahead of the subsystem that consumes
// them, and each such key names the measurement that switches it on; eyeOrder is now
// honoured.

// "auto" / "ngx" / "streamline" / "ffx" / "null". auto resolves to ngx. The startup
// probe runs before a D3D11 device exists and NGX cannot answer availability without
// one, so that probe reports "unavailable - a device-bound probe is required" and the
// load-time selection line reads null even on a healthy install: the render thread's
// own initialisation is the real verdict, and it logs the runtime path and the version
// the runtime reported. "null" is the passthrough control arm - it installs the hooks
// and evaluates nothing, so an A/B against it measures our own overhead rather than
// the reconstruction. Restart to change.
// mode derives its vendor path from this key: "ngx" or "auto" drive the raw-NGX
// continuous gate, "ffx" the FSR one - FFX has no reduced-input transaction, and
// "streamline"/"null" are wired into neither. Only ngx/auto can therefore reconstruct a
// reduced world, so a reduced mode beside any of the other three resolves to "dlaa" at
// load with the pair named in the log. Without that resolution dynamic resolution still
// reduced the viewport, the FSR handback declined itself per frame ("FSR engine handback
// is refused while the engine rasterizes a reduced active viewport; its current NativeAA
// path has no reduced-input transaction"), and vanilla's own upsample finished every
// frame of the session - a permanently soft world with nothing saying why. A
// capability-driven fallback from ngx to ffx is open.
US_SETTING(sSetting, "Fo4VRUpscaler", backend, std::string("auto"), Settings::kRestart)

// Where the mod looks for vendor runtime DLLs. They are user-supplied and loaded
// by explicit path, never by search order, so a stray copy elsewhere on PATH
// cannot quietly win a bug report. The resolved path and the version the runtime
// actually reported are both logged.
// This is where nvngx_dlss.dll goes. The release archive does not create the folder:
// the user makes it and puts NVIDIA's own production copy in it.
US_SETTING(sSetting, "Fo4VRUpscaler", runtimeDir, std::string("Data/F4SE/Plugins/Fo4VRUpscaler"), Settings::kLive)

// Reconstruction preset. Raw NGX accepts "auto" or one letter, case-insensitive. One
// value covers every mode; a preset per mode is not a key yet.
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
// The runtime keeps one preset per quality bucket, so this value is pinned on every one
// of them, and the bucket the reconstruction runs in follows the live ratio rather than
// the mode - at renderScaleOverride 0.50 a quality session runs the performance bucket,
// at this preset. Both halves are needed: a ratio outside the mode's own bucket used to
// run that bucket's own default instead, which looks like the preset working and costs
// up to three times as much. /state's preset block names both buckets.
// The shipped TOML ships F, the one preset with a frame-time case behind it: measured
// through the headset at 3760x3996 per eye, Quality at F ran 7.62 / 7.82 ms against
// native's 6.95 / 7.98, while K ran 9.72 / 10.00 with a third of frames doubled, and
// the observer judged K the sharper of the two. "auto" hands the choice to NVIDIA
// instead, which is K today. dlaa at F is not measured yet.
// The compiled default below stays "auto": it is what a configuration with no preset
// key at all gets, and the shipped file is where a measured recommendation belongs.
// Live; the log names the preset the runtime resolved.
US_SETTING(sSetting, "Fo4VRUpscaler", preset, std::string("auto"), Settings::kLive)

// 0..1. Our own RCAS sharpen at the TemporalAA seam, applied to the submitted target on
// every frame this plugin draws (DLAA and the reduced modes alike). The engine's own
// TemporalAA sharpens its output and the seam draw would otherwise drop that term, which
// through the lenses showed as native looking sharper than the reconstruction - an
// observation, not a measurement. Mapped to RCAS stops as 2 * (1 - sharpness): 1.0 is
// the kernel's maximum, 0.5 one stop down. 0 is off and bit-identical to no pass at all -
// the unsharpened shader stays bound. Never handed to a vendor runtime: raw NGX's own
// sharpen is deprecated and is forced to zero regardless. Sharpening after temporal
// reconstruction amplifies whatever the reconstruction got wrong; raise it last, not
// first. The default is an owner decision, not a measurement.
US_SETTING(fSetting, "Fo4VRUpscaler", sharpness, 0.5, Settings::kLive)

// "dlaa" (native anti-aliasing, ratio 1.0) / "quality" / "balanced" /
// "performance" / "ultraperformance". dlaa is the honest default and it is the
// expensive one: it feeds four times the input pixels of the numbers most
// published benchmarks were measured at and has no mechanism to give frames back.
// If the frame time goes over budget the fix is to drop the headset or runtime
// resolution, not this - native anti-aliasing at a reduced output is strictly
// worse than a reconstruction mode at full output.
// Raw-NGX diagnostics honour this. On the diagnostic path a non-DLAA mode
// additionally requires diagnosticDynamicResolution; renderScaleOverride may stay 0,
// which means the mode's own default ratio, snapped. On the product path mode derives
// all of it and this key is not read.
//
// Deprecated: mode replaces this as the activation surface (see mode above, which
// shares this vocabulary plus "off"). On the diagnostic path (mode = "off") quality
// still drives what it drives today - nothing about ratio selection changes there -
// but a mode other than "off" uses mode's own value as the quality name instead and
// ignores this key entirely. A fresh configuration should set mode instead.
US_SETTING(sSetting, "Fo4VRUpscaler", quality, std::string("dlaa"), Settings::kLive)

// 0 = use the documented default ratio for the chosen quality mode - 0.6667,
// 0.5800, 0.5000 and 0.3333 for quality, balanced, performance and ultraperformance
// respectively, which is what each mode's training assumed - and then snap it, once
// the per-eye output extent is known, to whatever ratio makes the engine land on the
// vendor's own optimal render size. The plugin floors width times ratio when it
// configures the vendor, so the unsnapped default misses that size by a pixel: 1852
// by 0.6667 gives 1234 where the runtime asks for 1235. Any value in 0.30..1.00
// overrides it and is written verbatim with no snap, for measurement rather than
// tuning; 0 is a value, not an absence. A number that is neither 0 nor inside
// 0.30..1.00 is refused, named in the log, and 0 is used instead. 1.0 is inside the
// range but is not a ratio - it is dlaa, a mode change - so beside a reduced mode it is
// refused the same way, named in the log, and 0 is used: a reduced bracket written at
// 1.0 disables reconstruction at the session's first frame. Set mode = "dlaa" to render
// at full resolution. Honoured at startup whenever the reduced-world bracket is armed -
// diagnosticDynamicResolution on the diagnostic path (mode = "off"), or a reduced
// mode (quality/balanced/performance/ultraperformance) on the product path - and
// dlaa is left alone either way because its ratio is 1.0 and it is not a reduced
// mode. Live: once the bracket is armed, a changed value here (a fresh
// /config/set, or a live /mode switch to a different reduced mode deriving a
// different default) is rewritten into the engine's live ratio immediately, while
// the world gate briefly denies admission so the write never lands mid-bracket -
// see Upscaler::DynamicResolution::RequestRatio. What stays restart-only is
// whether the bracket exists at all (diagnosticDynamicResolution, and on the
// product path whichever mode this session's first apply configured), never the
// ratio inside it. If the runtime's optimal size still disagrees after the snap
// the log says so and the session keeps running at the value it has.
US_SETTING(fSetting, "Fo4VRUpscaler", renderScaleOverride, 0.0, Settings::kLive)

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
US_SETTING(fSetting, "Fo4VRUpscaler", mipBiasOverride, 999.0, Settings::kLive)

// false = bias only the anisotropic sampler class, which is the world material
// class. true also biases the trilinear class, which the engine shares between world
// material sampling and full-screen linear sampling, so a full-screen pass would
// start sampling its source one mip level sharper than it was written. Turn it on
// only if the sampler probe shows world geometry binding the trilinear class.
US_SETTING(bSetting, "Fo4VRUpscaler", mipBiasIncludeTrilinear, false, Settings::kLive)

// false = leave every sampler's MinLOD alone. FO4 sets MinLOD to the sampler's own
// mip index, which is exactly the global mip-bias clamp vendor guidance tells
// integrators to disable - with it in place a negative bias reaches nothing on 15 of
// every 16 entries. It is also the engine's texture-degrade mechanism
// (iTextureDegradeDistance0/1), so true trades that mechanism for the full bias and
// is off by default rather than because the clamp is harmless.
US_SETTING(bSetting, "Fo4VRUpscaler", mipBiasLowerMinLod, false, Settings::kLive)

// "subrect" reconstructs each eye in place against the double-wide targets using
// per-resource sub-rectangle bases: no colour copy in, no output copy out, and the
// motion sanitizer reads the engine target directly. "intermediates" copies each eye
// into its own zero-origin staging texture first. subrect is the default because the
// NGX runtime honours the bases bit for bit on D3D11 when asked at creation (measured
// against the zero-origin path by tools/subrect-harness). On a backend that cannot
// address them - ffx, streamline, and the ngx stub of a build configured with
// -DUPSCALER_ENABLE_NGX=OFF - the plugin resolves to intermediates for the
// session and says so in the log, and /state reports both the requested and the
// effective mode. A flip takes effect on reload and re-creates the backend views.
US_SETTING(sSetting, "Fo4VRUpscaler", stereoMode, std::string("subrect"), Settings::kLive)

// "auto" / "LR" / "RL". Which half of the double-wide frame is the left eye. auto
// uses the left-first layout directly verified from FO4VR 1.2.72's shipping deferred
// shader bytecode and compositor submit bounds. The override stays as a checkable
// recovery control and takes effect on reload.
US_SETTING(sSetting, "Fo4VRUpscaler", eyeOrder, std::string("auto"), Settings::kLive)

// "engine" reads the game's own projection translation and converts it to render-pixel
// camera jitter at the backend seam. That conversion is proven, and its units, signs and
// amplitude are already right at every ratio; only the eight-phase sequence length is
// too short for reconstruction.
// The selected backend now owns the sequence whenever it has live views and the engine's
// jitter call site is patched: it writes its own sample into the same two floats, and the
// evaluation refuses any frame where the engine no longer holds that exact sample. Both
// spellings therefore describe the same read path, and neither selects between them.
// Inert until: there is a reason to run the shorter engine sequence deliberately.
US_SETTING(sSetting, "Fo4VRUpscaler", jitterSource, std::string("engine"), Settings::kLive)

// 0 = use the vendor minimum for the ratio. Sequence length scales with the
// square of the upscale ratio, so a sequence tuned for one-to-one temporal
// anti-aliasing is far too short for reconstruction, and the symptom is shimmer
// rather than an error message.
// The armed count already comes from the selected backend's own formula. For raw NGX
// that is ceil(8 * (output / render)^2) of the ratio actually in force: eight at dlaa,
// and 18, 24 and 32 at quality, balanced and performance. It follows a live ratio, so
// renderScaleOverride = 0.75 gives 15 - the count is a function of the ratio, not of
// the mode name. A shorter or longer count is an experiment nothing currently needs.
// Inert until: an experiment needs a count the backend would not choose.
US_SETTING(iSetting, "Fo4VRUpscaler", jitterPhaseOverride, std::int64_t(0), Settings::kLive)

// Build a per-pixel reactive mask so reticles, muzzle flash and particles are
// reconstructed as the transient things they are. This is the alternative to the
// current answer in this ecosystem, which is to switch the upscaler off entirely
// while you are aiming.
// Inert until: the input colour range is known and the mask pass exists.
US_SETTING(bSetting, "Fo4VRUpscaler", reactiveMask, true, Settings::kLive)

// Skip reconstruction while the Pip-Boy or a 3D interface is up. Whether the
// engine's motion vectors stay valid across an open menu is one of the
// measurements this key waits on, and reconstructing stale vectors smears text -
// which is the one thing a Pip-Boy is for.
// Inert until: the interface render paths are identified.
US_SETTING(bSetting, "Fo4VRUpscaler", excludePipboy, true, Settings::kLive)

// The game's own radial density mask writes a checkerboard that a temporal
// reconstructor reads as real detail and smears across the periphery. Turning it
// off is a precondition, not a preference.
// Inert until: the setting's engine-side location is confirmed.
US_SETTING(bSetting, "Fo4VRUpscaler", forceRadialDensityMaskOff, true, Settings::kLive)

// Reduced rasterization leaves the headset's hidden-area regions unwritten, but
// reconstruction consumes a rectangular eye. False keeps the engine setting intact
// and makes reduced-input modes decline rather than publish incomplete pixels.
// Startup-only; restart after changing it.
US_SETTING(bSetting, "Fo4VRUpscaler", forceHiddenAreaMeshOff, true, Settings::kRestart)

// Experimental raw-context depth-stencil bracket for prepared ImageSpace draws.
// Engine state commits may rebind the cached DSV before the draw, so this is disabled
// until a cache-native bracket has complete call-site coverage and draw-time proof.
US_SETTING(bSetting, "Fo4VRUpscaler", unbindDepthStencil, false, Settings::kLive)

// Measurement key. A reduced frame reaches the UI render entry with no depth-stencil
// view bound where a native frame carries depth-stencil 1 read-only; with this on, a
// frame that published at the TemporalAA seam rebinds that same read-only view at the
// seam's exit, so the passes after it and the UI pass see what native sees. Off is the
// shipped behaviour and the only behaviour any release has had. It does nothing on a
// native, DLAA or native-fallback frame, because none of those publishes at the seam.
US_SETTING(bSetting, "Diagnostics", rebindDepthStencilAfterSeam, false, Settings::kLive)

// MEASUREMENT KEY, off in every shipped configuration. Clear DS logical 1's STENCIL
// plane - depth untouched - at the seam's exit on a frame that published there. It is
// the other half of the key above: that one restores the depth-stencil BINDING a native
// frame carries into the UI pass, and the headset has already shown the binding alone
// does not draw the compass. This restores the CONTENT. Measured headless at the HUD
// pose: the reduced path enters the UI pass with that plane holding 0xFF over exactly
// the reduced raster's rectangle and 0 outside it, where the native arm's plane is 0
// everywhere, and the state the pass is entered with tests it NOT_EQUAL against
// reference 0x80 under read mask 0x80 - so that rectangle is a HUD the engine declines
// to draw. Like the key above it does nothing on a native, DLAA or native-fallback
// frame, because none of those publishes at the seam. Redundant since the key below
// shipped on: that pass zeroes the same plane in its own draw, so turning this one on as
// well only pays for the clear twice. It stays because it is half the mechanism, and
// half is what a bisection needs.
US_SETTING(bSetting, "Diagnostics", clearStencilAfterSeam, false, Settings::kLive)

// The reduced path's own behaviour, and the one key in this table that is ON by default.
// The reduced raster leaves depth-stencil 1's depth AND stencil packed at the origin of
// the double-wide; the HUD reads that surface at the full extent, so its occlusion test
// runs against a scaled-down copy of the scene sitting in the corner of the frame and
// cuts the compass ribbon. At the same seam exit and under the same gate as the two keys
// above, this resolves DS1 from the packed rectangle to the full extent: one CopyResource
// into a scratch copy, then one full-screen triangle back into DS1 with DepthFunc ALWAYS,
// write-all, and a stencil state that REPLACEs with reference 0 - so the depth plane is
// the scene the later passes expect, and the stencil plane the key above clears is zeroed
// by the same draw. Verified in the headset on 2026-09-08, both ways at 7520x3996: the
// compass draws whole in both eyes with this on and clips with it off.
//
// False is the diagnostic direction, not the shipped one: it reproduces the clipped
// compass, and it is how the pass is priced against a matched arm. Like both keys above
// it does nothing on a native, DLAA or native-fallback frame, because none of those
// publishes at the seam.
US_SETTING(bSetting, "Diagnostics", resolveDepthAfterSeam, true, Settings::kLive)

// The deferred composite's clip-to-view reconstruction, corrected for the reduced raster.
// Its pixel shaders form every environment-cube reflection per screen pixel from
// clip = SV_Position * (1/W, 1/H) * 2 - 1, with no dynamic-resolution term anywhere, and
// then reconstruct a view position through the global camera buffer's rows 20-27. Under
// the reduced raster the pixels sit in a packed rectangle at the origin, so that clip
// position spans [-1, 2r-1) instead of [-1, 1) and every cube reflection is zoomed by 1/r
// about the eye's top-left corner and swings 1/r times as fast with the head. The
// GBuffer reads are correct - the GBuffers are packed the same way - so it is the
// reconstruction alone, which is why the lighting looks right and the puddles do not.
//
// With this on, a copy of the engine's camera buffer carries those eight rows
// post-multiplied by [[1/rw,0,0,1/rw-1],[0,1/rh,0,1/rh-1],[0,0,1,0],[0,0,0,1]] and is
// written into the engine's own camera constant buffer at offset 0x140 for the duration of
// the composite's replay and put back after it; no shader is replaced. Ratio one is the
// identity and declines. Off on a DLAA, native, menu or veto frame, because none of those
// has a reduced-world bracket open.
//
// The correction is bracketed around the replay of the composite's own recorded pass -
// patch the engine's camera buffer, execute, put the rows back - because rows 20-27 are
// shared with the deferred light pass and the water shader and a change that outlived the
// composite would double-scale both.
//
// INERT ON THIS BUILD: measured 2026-09-09, the engine calls neither ExecuteCommandList nor
// FinishCommandList on the immediate context, zero of each across a full launch with both
// slots hooked, so the replay this brackets never happens. The arithmetic and its self-test
// are proven; the replay site is not found. Default false.
US_SETTING(bSetting, "Diagnostics", compositeReconstructionScale, false, Settings::kLive)

// Reset both eye histories when a menu opens or closes. Both, always together:
// resetting one eye and not the other is a stereo-rivalry artifact, which is
// worse than the ghost it fixed.
US_SETTING(bSetting, "Fo4VRUpscaler", resetOnMenu, true, Settings::kLive)

// Reset after the F4SE game-loaded lifecycle edge.
US_SETTING(bSetting, "Fo4VRUpscaler", resetOnLoad, true, Settings::kLive)

// Reset when the player enters a different cell. This engine-owned event catches
// direct COC and worldspace transitions independently of menu timing.
US_SETTING(bSetting, "Fo4VRUpscaler", resetOnCell, true, Settings::kLive)

// Reset on a True Scopes scope-raise edge. The scope render is a second,
// mid-frame world render at a different field of view; the history from before it
// is not history of this view.
// Inert until: there is a history to reset.
US_SETTING(bSetting, "Fo4VRUpscaler", resetOnScope, true, Settings::kLive)

// Skip our evaluate entirely while True Scopes' own deferred resolve is on the
// stack. Conservative by design - the two mods share a render thread and a
// render-target set, and coexisting quietly is worth more than the frame we would
// save.
// Wired to Fallout4VR's own renderer+4 scoped-pass selector. The interaction still
// needs a live True Scopes acceptance pass.
US_SETTING(bSetting, "Fo4VRUpscaler", suppressDuringScopeRender, true, Settings::kLive)

// Per-stage GPU timestamp queries plus CPU marks, reported by the dev tooling.
// Flip off then on to reset the accumulators for a clean measurement window. This
// is the only way to answer what this mod actually costs without a debugger.
// Reported by the dev tooling's /perf route, one interval per GPU-visible stage.
US_SETTING(bSetting, "Diagnostics", perfTimers, false, Settings::kLive)

// "" / "mv" / "depth" / "jitter" / "reactive". Draws the named input over the
// frame, in the headset. Motion vectors are the input most likely to be wrong and
// the one nobody can check by looking at the final image.
// Inert until: the inputs are identified.
US_SETTING(sSetting, "Diagnostics", debugVisualise, std::string(""), Settings::kLive)

// DEVELOPMENT ONLY. 1 = hand the vendor the negated camera sample, the offset
// applied to the projection that NGX documents and that the measurement confirmed:
// the other sign reconstructs a visible 3-pixel grid on every textured surface.
// -1 = hand it the DirectSR camera sample itself, the convention this project used
// before the measurement, kept so that grid can be reproduced on demand. Only the
// value handed to the vendor flips: the engine commit is untouched, so the raster is
// identical either way. Any other value is refused with a warning and 1 is used.
US_SETTING(iSetting, "Diagnostics", jitterVendorSign, std::int64_t(1), Settings::kLive)

#undef US_SETTING
