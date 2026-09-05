#pragma once

// The in-headset panel's vehicle: an HMD-attached OpenVR overlay the render thread draws
// ImGui into, once per frame, only while it is visible.
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
// Hotkeys::PollFrame and HudMessages::PollFrame. Configure, RequestShow/RequestHide/
// Toggle and Snapshot publish or read atomics and are safe from any thread. Nothing on
// the seam blocks on the game thread, a worker or a file: the one lock this module owns
// is taken with try_to_lock on the render thread and only on the single frame a capture
// completes.
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

namespace Upscaler::Panel
{
	// The overlay texture is a fixed size: it is the panel's own canvas, not an eye
	// target, so it does not follow the render or output resolution. ImGui's display size
	// and the overlay's mouse scale are both this, which is what makes a mouse event
	// arrive in ImGui's own pixels.
	inline constexpr std::uint32_t kTextureWidth = 1024;
	inline constexpr std::uint32_t kTextureHeight = 768;

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
	};

	// Publishes the four settings. Safe from any thread - called from
	// Configuration::Apply beside Hotkeys::Configure. a_enabled false tears the overlay
	// down at the next seam and leaves it down; the panel key is unbound by the same
	// Apply, so the operator cannot ask a disabled panel to show.
	void Configure(bool a_enabled, double a_widthMeters, double a_distanceMeters) noexcept;

	// The show/hide intent, published as an atomic the seam honours on its next frame.
	// Safe from any thread: the render thread's hotkey poll and DevBench's listener both
	// use them. Toggle is a read-modify-write, so two callers cannot lose one another's
	// flip.
	void RequestShow() noexcept;
	void RequestHide() noexcept;
	void Toggle() noexcept;

	// Render-thread only, once per frame from the ClearColor seam. Hidden costs two
	// atomic loads and returns: no ImGui frame, no draw, no texture submit, no event
	// poll.
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
}
