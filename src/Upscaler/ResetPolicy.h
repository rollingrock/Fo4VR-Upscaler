#pragma once

// Decides when accumulated reconstruction history is invalid and carries a paired reset
// from game/UI event threads to the render thread.

#include <cstdint>
#include <string_view>

namespace Upscaler::ResetPolicy
{
	inline constexpr std::uint32_t kQualityWorldStableFrames = 16;

	enum class WorldAdmissionPhase : std::uint32_t
	{
		kBlocked = 0,
		kStabilizing,
		kActive,
	};

	enum class WorldBlockReason : std::uint32_t
	{
		kNone = 0,
		kNoUI,
		kMainMenu,
		kLoadingMenu,
		kFaderMenu,
		kPauseMenu,
		kPipboyMenu,
		kMessageBoxMenu,
		kFreezeFrameMenuBackground,
		kFreezeFramePause,
		kClosingAllMenus,
		kMenuMode,
		kItemMenuMode,
		kNoPlayer,
		kNoCell,
		kTransitionChanged,
		kStateReadFault,
		kStabilizing,
		// Appended last so no existing enumerator value moves. Overrides every other
		// reason: see SetOperatorVeto.
		kOperatorVeto,
		// Appended after kOperatorVeto for the same reason (no existing enumerator
		// moves). A live mode switch denies admission while it publishes the new
		// configuration; the operator's own A/B veto still outranks it - see
		// EngageModeSwitchPending and ApplyModeSwitchBlock.
		kModeSwitchPending,
		// Appended last, same reason again. HDRRoundTrip's own session latch is set: the
		// session is terminal, or suspended for its post-fault cool-down. Nothing would
		// reconstruct the reduced raster those frames produce, so the world renders native
		// until the re-arm - see ApplyReconstructionBlock, which ranks it above a pending
		// switch (a fault outlives a switch window) and below the operator's own veto.
		kReconstructionUnavailable,
		// The pre-world mutation boundary positively read a valid selector zero. No
		// reduced-world token exists and native rendering continues until selector one
		// returns and the ordinary stabilization window completes.
		kRenderPhaseUnavailable,
	};

	struct State
	{
		bool pending = false;
		bool menuSinkInstalled = false;
		bool cellSinkInstalled = false;
		bool resetOnMenu = true;
		bool resetOnLoad = true;
		bool resetOnCell = true;
		std::uint64_t requests = 0;
		std::uint64_t coalesced = 0;
		std::uint64_t consumptions = 0;
		std::uint64_t loadEvents = 0;
		std::uint64_t menuOpenEvents = 0;
		std::uint64_t menuCloseEvents = 0;
		std::uint64_t cellEnterEvents = 0;
		std::uint64_t cellLeaveEvents = 0;
		std::uint64_t leftSubmissions = 0;
		std::uint64_t rightSubmissions = 0;
		std::uint32_t lastCellId = 0;
		std::uint64_t requestedGeneration = 0;
		std::uint64_t acknowledgedGeneration = 0;
		std::uint64_t acquisitions = 0;
		std::uint64_t qualityAbortRequests = 0;
		WorldAdmissionPhase worldAdmissionPhase = WorldAdmissionPhase::kBlocked;
		WorldBlockReason worldBlockReason = WorldBlockReason::kNoUI;
		bool worldAdmitted = false;
		std::uint32_t worldStableFrames = 0;
		std::uint32_t worldStableFramesRequired = kQualityWorldStableFrames;
		std::uintptr_t worldCell = 0;
		std::uint64_t worldTransitionGeneration = 0;
		std::uint64_t worldAdmissionChecks = 0;
		std::uint64_t worldAdmissionDeniedFrames = 0;
		std::uint64_t worldAdmissionActivations = 0;
		std::uint64_t worldAdmissionInvalidations = 0;
		std::uint64_t worldAdmissionResetRequests = 0;
		std::uint64_t worldAdmissionRechecks = 0;
		std::uint64_t worldAdmissionRecheckFailures = 0;
		std::uint64_t worldAdmissionPublishRechecks = 0;
		std::uint64_t worldAdmissionPublishVetoes = 0;
		std::uint32_t worldMenuMode = 0;
		std::uint32_t worldItemMenuMode = 0;
		std::uint32_t worldFreezeFrameMenuBG = 0;
		std::uint32_t worldFreezeFramePause = 0;
		bool worldClosingAllMenus = false;
		std::uint32_t worldOpenBlockingMenuMask = 0;
		bool operatorVetoActive = false;
		std::uint64_t operatorVetoEngagements = 0;
		std::uint64_t operatorVetoReleases = 0;
		std::uint64_t operatorVetoDeniedFrames = 0;
		// stableFrames the admission machine held at the moment it last activated - see
		// g_worldLastActivationStableFrames.
		std::uint32_t worldLastActivationStableFrames = 0;
		bool modeSwitchPending = false;
		std::uint64_t modeSwitchEngagements = 0;
		std::uint64_t modeSwitchReleases = 0;
		std::uint64_t modeSwitchDeniedFrames = 0;
		// Frames denied because HDRRoundTrip's session latch was set - its own counter
		// rather than only the shared deniedFrames total, so a run can tell a fault-denied
		// frame from a menu-denied one without reading the log.
		bool reconstructionUnavailable = false;
		std::uint64_t reconstructionUnavailableDeniedFrames = 0;
		bool renderPhaseUnavailable = false;
		std::uint64_t renderPhaseUnavailableDeniedFrames = 0;
		std::uint64_t renderPhaseInvalidations = 0;
	};

	// How long a live configuration change took to reach a world frame this machine
	// actually admitted. The thread that accepted the change stamps the clock; the
	// pre-world hook stops it on the next admitted frame, which is the one moment both
	// ends are visible in one place. Two independent stamps because a live tier step is
	// both a mode change and a ratio change and neither should read the other's number.
	//
	// The span does NOT include the backend preparation that follows: the pre-world hook
	// runs before the seam where the views are re-created, so a re-created feature's cost
	// lands in Evaluate::Lifecycle::lastPrepareMs and the two add up to what the operator
	// waited for.
	enum class ChangeClock : std::uint32_t
	{
		kModeSwitch = 0,
		kRatio = 1,
	};

	struct ChangeClockState
	{
		// A change has been accepted and no world frame has been admitted since.
		bool pending = false;
		std::uint64_t stamps = 0;
		// The last completed measurement, 0 until one completes.
		float lastToAdmittedMs = 0.0f;
	};

	// Any thread, lock-free, never blocks. A second stamp before the first stops
	// supersedes it: the newer change is the one whose latency is still being waited on.
	void StampChange(ChangeClock a_clock) noexcept;

	[[nodiscard]] ChangeClockState ChangeClockSnapshot(ChangeClock a_clock) noexcept;

	struct Ticket
	{
		std::uint64_t generation = 0;
		bool pending = false;
	};

	struct WorldAdmissionTicket
	{
		std::uint64_t transitionGeneration = 0;
		std::uintptr_t cell = 0;
		bool admitted = false;
	};

	// Atomic and safe from the settings/reload thread.
	void Configure(bool a_resetOnMenu, bool a_resetOnLoad, bool a_resetOnCell) noexcept;

	// Whether a menu that takes typed text is on the stack right now. Read straight off
	// RE::UI's menu map, the same singleton and the same read lock this module's own world
	// observation already takes, and - like that observation, and unlike RE::UI's own
	// GetMenuOpen helper - through a const reference that never touches the menu's
	// Scaleform refcount; see TextInputMenuOpen's body for why a per-frame render-thread
	// AddRef on a game-thread-owned menu is not an option. False when the UI singleton is
	// not up yet.
	//
	// The console is the only free-text menu CommonLibF4 declares a class for
	// (RE::Console::MENU_NAME), and it is the one that matters: it is where a keypad key
	// typed into a command reaches Upscaler::Hotkeys' poll. Other keyboard-driven menus
	// (terminals, holotapes) navigate rather than accept text and are deliberately not
	// listed - a toggle there is a toggle the player meant.
	//
	// Called once per frame from the render thread (Upscaler::Hotkeys::PollFrame); safe
	// from any thread that can afford one hashed lookup under the menu-map read lock.
	[[nodiscard]] bool TextInputMenuOpen() noexcept;

	// Operator A/B veto: written from the DevBench listener thread (and later a hotkey
	// poll), read on the render thread. While active, the world gate denies every frame
	// and reports WorldBlockReason::kOperatorVeto regardless of any other observation, so
	// the engine renders native. Releasing it re-admits through the ordinary stabilizing
	// count, not instantly. Safe from any thread.
	void SetOperatorVeto(bool a_active) noexcept;
	[[nodiscard]] bool OperatorVetoActive() noexcept;

	// A live mode switch's own deny window, engaged from Configuration.cpp (any thread)
	// while it publishes a new mode's runtime inputs, and released once that publish is
	// done. Same shape as the operator veto - the world gate denies every frame and
	// reports WorldBlockReason::kModeSwitchPending - but a distinct reason and a distinct
	// atomic on purpose: a switch must not read as, clobber, or be clobbered by the
	// operator's own A/B veto. See ApplyModeSwitchBlock for the precedence between them.
	//
	// Refcounted, not a plain flag: Configuration.cpp has two independent callers that can
	// each hold this window open at once - a live /mode request (RequestModeSwitch) and
	// Configuration::Apply's own prospective, pre-lock ratio check (which engages before
	// ApplyLockedImpl ever calls Upscaler::DynamicResolution::RequestRatio, itself never
	// engaging or releasing this window) - and each releases on its own schedule. A plain
	// bool let whichever caller finished first release a window the other was still
	// relying on. The window denies admission for as long as the count is above zero;
	// Engage increments, Release decrements, and Release can never drive the count
	// negative - see its own comment. Engage/release your own engagement only; never
	// assume you are the only holder, and never guard an Engage call on whether the
	// window already looks held - the refcount is exactly what makes that check
	// unnecessary now.
	void EngageModeSwitchPending() noexcept;
	void ReleaseModeSwitchPending() noexcept;

	// Game-thread lifecycle entry points. Installation is idempotent and may be retried
	// when RE::UI or the player singleton was not ready at the first message. Cell-event
	// installation is deferred out of synchronous F4SE lifecycle callbacks.
	[[nodiscard]] bool InstallMenuEvents() noexcept;
	[[nodiscard]] bool InstallCellEvents() noexcept;
	void OnGameLoaded();

	// Both eyes are always reset together. Resetting one and not the other is a
	// stereo-rivalry artifact, which is worse than the ghost it fixed.
	//
	// Candidate triggers: menu open and close, save load, cell change, fast travel,
	// resolution change, scope raise. Each becomes a TOML toggle and each reset is
	// logged with its reason, because a wrong reset policy is a documented flicker
	// source and a hardcoded guess is one nobody can audit.
	void RequestReset(std::string_view a_reason);

	// Render-thread seam for a repaired/re-entered world epoch. This deliberately uses
	// the same generation as menu/load/cell requests, so it cannot be lost behind an
	// in-flight stereo evaluation.
	void RequestWorldReset() noexcept;

	// A reduced-NGX evaluation that does not reach publication may have advanced one or
	// both vendor histories. Publish one normal generation so the next successful stereo
	// pair resets both eyes together. This counter is separate for fault-proof accounting.
	void RequestQualityAbortReset() noexcept;

	// Acquire does not consume the request. The returned generation remains pending until
	// the render thread acknowledges it after both eye evaluations and all requested
	// reassemblies have succeeded. A newer request racing that frame has a newer generation
	// and therefore remains pending for the next stereo pair.
	[[nodiscard]] Ticket AcquireReset() noexcept;
	void AcknowledgeReset(Ticket a_ticket) noexcept;
	void RecordSubmission(std::uint32_t a_eye, bool a_reset) noexcept;

	// The HDR/TAA signatures also occur while FO4VR renders a menu freeze-frame, so they
	// are not sufficient proof that the current input is a live world frame. Admission is
	// intentionally fail-closed: a valid player cell, no blocking/freeze menu state, and
	// kQualityWorldStableFrames consecutive observations of the same cell and transition
	// generation are required before any Quality vendor evaluation may start.
	//
	// AdvanceBeforeWorld advances the render-thread-only stability state exactly once
	// at the main pre-world geometry seam. Revalidate does not advance it; call it after
	// the reduced-world write, after vendor evaluation, and again
	// immediately before the first reversible engine mutation. A failed post-evaluate
	// revalidation means the caller must discard that vendor result and request a paired
	// history reset (or abort the prepared Quality transaction, which does so itself).
	//
	// a_reconstructionUnavailable combines the caller-owned handback session latches and
	// HDR availability. a_renderPhaseUnavailable is true only for a fully read, otherwise
	// valid selector-zero snapshot. Both arrive by value because their owners can change
	// later in the frame; this call caches them for the same frame's Revalidate just as it
	// caches the veto. The first reports kReconstructionUnavailable and outranks the
	// second's kRenderPhaseUnavailable label when both deny the same frame.
	[[nodiscard]] WorldAdmissionTicket AdvanceQualityWorldAdmissionBeforeWorld(
		bool a_reconstructionUnavailable,
		bool a_renderPhaseUnavailable) noexcept;
	// Invalidates an already-issued ticket when selector zero lands between the read-only
	// preflight and BeginReducedWorld's final pre-write snapshot. Render-thread only.
	void InvalidateQualityWorldAdmissionForRenderPhase(WorldAdmissionTicket a_ticket) noexcept;
	[[nodiscard]] bool RevalidateQualityWorldAdmission(WorldAdmissionTicket a_ticket) noexcept;
	// Lock-free transition lease check at the last CPU boundary before the real
	// history/RT4 MRT draw. The caller performs the full UI/cell revalidation first;
	// this final check intentionally takes no engine lock while D3D state is active.
	// Separate counters prove that this exact seam actually ran.
	[[nodiscard]] bool RevalidateQualityWorldAdmissionForPublish(WorldAdmissionTicket a_ticket) noexcept;
	[[nodiscard]] std::string_view WorldAdmissionPhaseName(WorldAdmissionPhase a_phase) noexcept;
	[[nodiscard]] std::string_view WorldBlockReasonName(WorldBlockReason a_reason) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
