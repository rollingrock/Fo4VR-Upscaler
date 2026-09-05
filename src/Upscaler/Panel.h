#pragma once

// The in-headset panel: an HMD-attached OpenVR overlay the render thread draws ImGui
// into, once per frame, only while it is visible - and the controls on it.
//
// Why an overlay and not a menu or a mirror-window UI. The panel has to be readable while
// the world is still being rendered - an A/B comparison is worthless if opening the panel
// stops the picture - which rules out a dashboard overlay (the dashboard replaces the
// scene) and the mirror-window ImGui a desktop mod would use (invisible in the headset).
// Drawing into the eye targets would put the panel through the upscaler and through
// whatever clips the HUD. An HMD-relative overlay is composited by the runtime after our
// frame and touches none of that.
//
// Runtime access. The plugin never loads a second OpenVR loader: it asks the
// openvr_api.dll the game itself already mapped, through GetModuleHandleW +
// GetProcAddress("VR_GetGenericInterface"), exactly as Fo4VR-True-Scopes'
// src/TrueScopes/VerdictInput.cpp resolves IVRSystem. No openvr_api.lib is linked, no
// second openvr_api.dll ships, and VR_Init is never called - the game owns the session.
// The version string requested is whatever the compiled header declares, so an SDK bump
// moves the request with it; the string and the EVRInitError are both reported.
//
// Degradation. A missing runtime, a refused interface, a failed CreateOverlay and a
// refused SetOverlayTexture all end the same way: the panel reports itself unavailable
// with the runtime's own error name, the counters say how often, and every other part of
// the mod runs unchanged. Nothing here is ever fatal.
//
// Threads. PollFrame is the only entry that touches D3D11, ImGui or OpenVR, and it runs
// on the verified primary render thread from the ClearColor seam in Hooks.cpp, next to
// Hotkeys::PollFrame and HudMessages::PollFrame. Configure, StartWorker, RequestShow/
// RequestHide/Toggle, InjectEvent and Snapshot publish or read atomics and are safe from
// any thread. Nothing on the seam blocks on the game thread, a worker or a file: all three
// locks this module owns - the capture handoff, the injected-event queue and the control
// rectangle table - are taken with try_to_lock on the render thread, and a frame that
// cannot have one simply does not publish or drain that frame.
//
// Where a click goes. The seam runs Upscaler::PanelModel (engine-free, pure, proved by
// static_assert) over snapshots other modules publish, and turns one input into exactly
// one intent. A settings intent is posted to this module's own worker thread - never run
// on the seam - because Settings::Writer::Set blocks, bounded, on the game thread's drain
// and a frame producer waiting there waits for the thread that would end its wait. The
// A/B pair is the exception and goes straight from the seam to
// Upscaler::ResetPolicy::SetOperatorVeto, which is one atomic store: a comparison the
// operator is holding must not depend on a settings write finishing. One intent is in
// flight at a time; a second click while one is pending is refused with a named reason,
// the way a second mode switch is refused busy rather than queued.
//
// Veto ownership. The panel clears only a veto its own hold or latch engaged, tracked
// here the way Hotkeys' COMPARE tracks its own engagement: a veto standing because of an
// /ab request or the COMPARE key is never cleared by this panel. A hold engaged when the
// panel hides, loses overlay visibility or is torn down is released unconditionally -
// nothing else would ever end it.
//
// Teardown. This tree has no process-exit teardown path - an F4SE plugin is never
// unloaded, and Evaluate::Reset() (the pattern the other render modules follow) runs from
// the render thread on a configuration change, not at exit. So the panel's teardown runs
// at the same seam, on the frame panelEnabled is observed false, in the order the
// overlay's lifetime requires: hide, destroy the overlay, shut ImGui's D3D11 backend down
// and destroy its context, then release the render-target view and the texture. The
// texture is released last because the overlay holds it until DestroyOverlay returns.

#include <cstdint>
#include <string>
#include <vector>

namespace Upscaler::Panel
{
	// The overlay texture is a fixed size: it is the panel's own canvas, not an eye
	// target, so it does not follow the render or output resolution. ImGui's display size
	// and the overlay's mouse scale are both this, which is what makes a mouse event
	// arrive in ImGui's own pixels. Square since the controls landed: the status block
	// plus six mode rows, three sliders, the preset row, the A/B pair, the keys and the
	// result line do not fit in 768 px at this UI scale, and a taller texture keeps the
	// text the same physical size (the overlay's width in metres is what sets that) while
	// making the panel taller in the world rather than making it scroll.
	inline constexpr std::uint32_t kTextureWidth = 1024;
	inline constexpr std::uint32_t kTextureHeight = 1024;

	// One mode button as the model computed it for the last drawn frame. The tag is what
	// the operator reads before the click - "live", or "next launch: <outcome>" in the
	// same outcome vocabulary /mode and /state already use - and it is reported here so a
	// headless run can assert what the panel says without reading its pixels.
	struct ModeTag
	{
		std::string mode;
		std::string label;
		std::string tag;
		std::string outcome;
		bool live = false;
		bool current = false;
	};

	// One control's rectangle in panel pixels, top-left origin - the same coordinates
	// ImGui sees, and the same ones /panel action=event takes. Recorded after the widget
	// is drawn, so a driver can click a control by name without knowing the layout.
	// value carries a slider's live position (mid-drag included) and a checkbox's state,
	// which is what lets a driver converge on a slider value without reimplementing
	// ImGui's own hit mapping.
	struct ControlRect
	{
		std::string name;
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
		double value = 0.0;
		bool active = false;
		bool present = false;
	};

	struct State
	{
		bool enabled = false;
		// The interface lookup: whether openvr_api.dll answered, the exact version string
		// the compiled header made us ask for, and the EVRInitError the loader returned.
		bool runtimeResolved = false;
		std::string interfaceVersion;
		std::int32_t lastInitError = 0;
		bool overlayCreated = false;
		std::uint64_t overlayHandle = 0;
		bool visible = false;
		// The runtime's own name for the last EVROverlayError, through
		// IVROverlay::GetOverlayErrorNameFromEnum - never a table of ours. "None" until
		// one is seen.
		std::string lastOverlayError{ "None" };
		std::uint64_t overlayErrors = 0;
		std::uint64_t framesDrawn = 0;
		std::uint64_t textureSubmits = 0;
		std::uint64_t eventsPolled = 0;
		std::uint64_t eventsMouseMove = 0;
		std::uint64_t eventsMouseButtonDown = 0;
		std::uint64_t eventsMouseButtonUp = 0;
		std::uint64_t eventsScrollSmooth = 0;
		std::uint64_t eventsOverlayShown = 0;
		std::uint64_t eventsOverlayHidden = 0;
		std::uint64_t eventsOther = 0;
		// Synthetic events fed through /panel action=event, counted separately from the
		// polled ones and applied through the identical handler: a headless run proves the
		// pump and the model, and the count says which half of the input it exercised.
		std::uint64_t eventsInjected = 0;
		std::uint64_t eventsInjectedDropped = 0;
		// Real overlay input events counted but not applied because an injected pointer
		// owned the panel at the time - see Panel.cpp's kInjectOwnsPointerMs. Zero in every
		// session nobody is driving through /panel action=event.
		std::uint64_t eventsSuppressedByInjection = 0;
		std::string captureState{ "idle" };
		std::uint64_t captureSequence = 0;
		std::uint64_t captureCompletedSequence = 0;
		std::string capturePath;
		std::string captureError;
		// What the shared-texture measurement found: whether SetOverlayTexture accepted a
		// texture created without D3D11_RESOURCE_MISC_SHARED, and if not, the error it
		// answered without the flag. See Panel.cpp - this is measured, not assumed.
		std::string textureSharedFlag{ "not-measured" };
		std::uint32_t textureWidth = kTextureWidth;
		std::uint32_t textureHeight = kTextureHeight;
		double widthMeters = 0.0;
		double distanceMeters = 0.0;
		std::uint64_t creations = 0;
		std::uint64_t teardowns = 0;
		std::uint64_t creationFailures = 0;
		// Whether the last frame the panel drew included the per-interval timing section,
		// which it does only while perfTimers is on. This is the gate as the panel took
		// it, not as the settings file spells it, so a headless run can prove that the
		// shipped default draws no timing section - and that turning the timers on draws
		// one - without a headset.
		bool telemetryDrawn = false;

		// --- the controls ---------------------------------------------------------
		//
		// Intents. queued counts every one the seam handed the worker, completed every
		// one the worker finished, refused every input the model turned away with a
		// reason (an in-flight intent, or a control it does not own). The three account
		// for every click: a click that is neither completed nor refused is a lost one.
		std::uint64_t intentsQueued = 0;
		std::uint64_t intentsCompleted = 0;
		std::uint64_t intentsRefused = 0;
		std::string lastIntent{ "none" };
		std::string lastRefusal{ "none" };
		bool intentInFlight = false;
		// "not-started" before the first Configuration::Apply, "unavailable" when the
		// thread could not be created (settings intents are refused for the session and
		// the panel says so), "idle" or "busy" otherwise.
		std::string workerState{ "not-started" };

		// The last Settings::Writer::SetResult, in the writer's own five facts plus its
		// reason - and the one line the panel draws from them, so a run can compare what
		// the operator reads against what /config/set answers for the same request.
		bool resultPresent = false;
		bool resultAccepted = false;
		bool resultPersisted = false;
		bool resultTookEffectNow = false;
		bool resultNeedsRestart = false;
		std::string resultKey;
		std::string resultValue;
		std::string resultReason;
		std::string resultLine{ "no change requested yet" };

		// The operator A/B veto as ResetPolicy reports it, and whether this panel is the
		// owner of the standing engagement - never a mirror of the atomic. holdEngaged and
		// latchOn are the panel's own two controls.
		bool vetoActive = false;
		bool vetoOwnedByPanel = false;
		bool holdEngaged = false;
		bool latchOn = false;
		std::uint64_t vetoEngagements = 0;
		std::uint64_t vetoReleases = 0;
		std::uint64_t vetoDeferredToOperator = 0;

		// The six mode buttons as the model computed them for the last drawn frame.
		// Empty until the panel has drawn once.
		std::vector<ModeTag> modes;
	};

	// Publishes the settings the panel draws and places itself with. Safe from any thread
	// - called from Configuration::Apply beside Hotkeys::Configure, which is also where
	// the live values below come from: the panel never reads a global Setting<T>, which a
	// load can be halfway through rewriting. a_enabled false tears the overlay down at the
	// next seam and leaves it down; the panel key is unbound by the same Apply, so the
	// operator cannot ask a disabled panel to show.
	void Configure(
		bool a_enabled,
		double a_widthMeters,
		double a_distanceMeters,
		double a_renderScaleOverride,
		const std::string& a_preset,
		double a_sharpness) noexcept;

	// Starts the panel's settings worker. Idempotent, latched against retry after a
	// failure, and called only from Configuration::Apply() - std::thread's constructor
	// takes the loader lock to run every loaded DLL's thread-attach callback, so no
	// request and no frame may ever create it. A failed start latches the worker
	// unavailable for the session and the panel says so on its result line.
	[[nodiscard]] bool StartWorker() noexcept;

	// The show/hide intent, published as an atomic the seam honours on its next frame.
	// Safe from any thread: the render thread's hotkey poll and DevBench's listener both
	// use them. Toggle is a read-modify-write, so two callers cannot lose one another's
	// flip.
	void RequestShow() noexcept;
	void RequestHide() noexcept;
	void Toggle() noexcept;

	// One synthetic overlay input event, queued for the render thread to apply through
	// the identical handler PollNextOverlayEvent's own events go through. x and y are
	// panel pixels with a top-left origin - ImGui's own coordinates, which is the flip
	// already applied to a real VREvent_Mouse_t. a_button is an ImGui mouse button (0
	// left, 1 right, 2 middle) and a_delta is the scroll amount. Listener-safe; returns
	// false when the bounded queue is full, which is the only way one can be lost.
	enum class EventKind : std::uint32_t
	{
		kMove = 0,
		kDown,
		kUp,
		kScroll,
	};
	[[nodiscard]] bool InjectEvent(EventKind a_kind, float a_x, float a_y, std::uint32_t a_button, float a_delta) noexcept;

	// Render-thread only, once per frame from the ClearColor seam. Hidden costs two atomic
	// loads and the hold release below, and returns: no ImGui frame, no draw, no texture
	// submit, no event poll. The release is not optional - the hold button only stops being
	// active on a frame that draws it, so hiding while holding would leave the world gate
	// denying with nothing left to end it.
	void PollFrame() noexcept;

	// Arms a one-shot readback of the panel texture at the seam. Returns the sequence
	// number to poll Snapshot().captureSequence against; 0 means the request was refused
	// because one is already in flight. Listener-safe.
	[[nodiscard]] std::uint64_t RequestCapture() noexcept;

	// Listener-thread only. Writes a completed capture to disk - the same BMP writer and
	// the same captures directory the stereo capture routes use - and returns the state
	// block with capturePath filled in. Called by DevBench's /panel route; every other
	// reader wants Snapshot().
	[[nodiscard]] State SaveCaptureAndSnapshot();

	// Lock-free read of every published fact. Safe from any thread.
	[[nodiscard]] State Snapshot() noexcept;

	// Where each control landed on the last drawn frame. Layout rather than state, which
	// is why it is its own call and not part of State: /panel action=event is its only
	// consumer, and /state has no use for a rectangle. Safe from any thread; the array is
	// published under a bounded seqlock, so a torn read retries rather than blocking the
	// frame producer. Empty until the panel has drawn once.
	[[nodiscard]] std::vector<ControlRect> ControlRects();
}
