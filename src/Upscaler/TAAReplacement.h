#pragma once

// Bounded replacement of native post-tonemap TemporalAA's two outputs.
//
// The accepted post-draw mode calls Bethesda's prepared helper first and overwrites its
// still-bound outputs. The stricter bypass mode resolves the engine's history parity,
// validates color 3, submitted target 4 and the expected 61/62 output directly, then
// performs the same private draw instead of the prepared helper. Both hand the exact
// host D3D11 state back before the outer image-space cleanup continues. They are
// mutually exclusive diagnostic gates, off by default and bounded to 512 draws. Either
// may run alone or require consecutive accepted stamps from one continuous NGX/FFX gate;
// Configure snapshots that source's current serial as the episode baseline.
//
// The reduced-NGX quality transaction uses the bypass draw as its publish: at the
// TemporalAA seam, after a world-admission recheck, the one MRT draw writes the
// parity-selected history and RT4 in place of the engine's draw, and the late ImageSpace
// range only commits the accounting. Slot 21 reports inactive at unity, so TemporalAA is
// the range's last active effect and its destination is RT4, exactly as on a no-DRS frame.
//
// The same draw carries the sharpen pass. The engine's own TemporalAA sharpens its output
// (fTAASharpen / fTAAPostSharpen, ini strings VERIFIED in Fallout4VR.exe) and the bypass
// draw drops that term, so with `sharpness` above zero the seam binds the RCAS pixel
// shader from SharpenShader.h instead of the passthrough: RT4 receives the sharpened
// colour, the parity history the raw value exactly as before. Zero binds the unchanged
// passthrough, and a sharpen shader that fails to compile falls back to it out loud.

#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/ResetPolicy.h"

#include <cstdint>
#include <string_view>

namespace Upscaler::TAAReplacement
{
	// Which pixel shader the last seam draw bound.
	enum class SeamShader : std::uint32_t
	{
		kNone,
		kPassthrough,
		kRcas,
	};

	[[nodiscard]] constexpr std::string_view SeamShaderName(SeamShader a_shader) noexcept
	{
		switch (a_shader) {
		case SeamShader::kPassthrough:
			return "passthrough";
		case SeamShader::kRcas:
			return "rcas";
		default:
			return "none";
		}
	}

	struct State
	{
		bool enabled = false;
		bool postDrawEnabled = false;
		bool bypassEnabled = false;
		bool requireNgxHandback = false;
		bool requireFfxHandback = false;
		bool ready = false;
		bool completed = false;
		bool sessionDisabled = false;
		HDRRoundTrip::HandbackSource handbackSource = HDRRoundTrip::HandbackSource::kNone;
		HDRRoundTrip::HandbackSource currentHandbackSource = HDRRoundTrip::HandbackSource::kNone;
		std::uint64_t attempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t noHandbackSkips = 0;
		std::uint64_t handbackSourceMismatches = 0;
		std::uint64_t handbackSequenceGaps = 0;
		std::uint64_t validationFailures = 0;
		std::uint64_t isolationFailures = 0;
		std::uint32_t validationMask = 0;
		std::uint64_t shaderBuilds = 0;
		std::uint64_t history61Writes = 0;
		std::uint64_t history62Writes = 0;
		std::uint64_t postDrawOverwrites = 0;
		std::uint64_t nativeDrawBypasses = 0;
		// Seam publish draws issued for the reduced-NGX transaction, counted whether or
		// not the frame later committed.
		std::uint64_t qualityPublishDraws = 0;
		std::uint64_t qualityHistoryRepairArms = 0;
		std::uint64_t qualityHistoryRepairAttempts = 0;
		std::uint64_t qualityHistoryRepairCopies = 0;
		std::uint64_t qualityHistoryRepairSuccesses = 0;
		std::uint64_t qualityHistoryRepairFailures = 0;
		// Frames whose pending episode was released to the engine's own TemporalAA draw: the
		// release count, the native draws observed, which history the parity said the
		// engine would write, and the mismatches between that and the engine's own target.
		std::uint64_t qualityNativeFallbackReleases = 0;
		std::uint64_t qualityNativeFallbackDraws = 0;
		std::uint64_t qualityNativeFallbackHistory61Writes = 0;
		std::uint64_t qualityNativeFallbackHistory62Writes = 0;
		std::uint64_t qualityNativeFallbackHistoryMismatches = 0;
		std::uint64_t parityReadAttempts = 0;
		std::uint64_t parityReadFailures = 0;
		std::uint64_t parityInvalidValues = 0;
		std::uint64_t currentRenderTargetMismatches = 0;
		std::uint64_t targetSuccesses = 0;
		// True on the product path (mode != "off" - see HDRRoundTrip::State's
		// productPath). A companion flag beside the numeric target rather than a sentinel
		// value, for the same reason HDRRoundTrip's own continuous target packs one: 0 or a
		// maximal value both read as "a real bound" to a caller that forgets to check, and a
		// bool cannot be misread that way. targetSuccesses stays 0 while this is true.
		bool targetUnbounded = false;
		std::uint64_t lastTemporalAACall = 0;
		std::uint64_t handbackBaseline = 0;
		std::uint64_t lastHandbackSerial = 0;
		std::uint64_t currentHandbackSerial = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t sourceFormat = 0;
		std::uint32_t historyFormat = 0;
		std::uint32_t outputFormat = 0;
		std::uint32_t lastParity = 0xFFFFFFFFu;
		std::int32_t expectedHistoryLogical = -1;
		std::int32_t engineHistoryLogical = -1;
		std::int32_t deviceRemovedReason = 0;
		std::uint32_t lastError = 0;
		std::uintptr_t context = 0;
		std::uintptr_t sourceSrv = 0;
		std::uintptr_t historyRtv = 0;
		std::uintptr_t outputRtv = 0;
		std::uintptr_t parityAddress = 0;
		// The seam sharpen: the setting as configured, what the seam draws with (zero until
		// the RCAS shader and its constant buffer exist, and zero for good once building
		// them has failed), its RCAS stops, the shader the last draw bound, and the compile
		// failures with the last HRESULT they reported.
		float sharpnessRequested = 0.0f;
		float sharpnessEffective = 0.0f;
		float sharpenStops = 0.0f;
		SeamShader sharpenShader = SeamShader::kNone;
		std::uint64_t sharpenCompileFailures = 0;
		std::uint32_t sharpenLastHr = 0;
		// True if the DevBench seamDrawCloseFailure fault's last trigger fired from the
		// TemporalAA-seam publish caller (a_worldAdmission non-null); false if it has
		// never fired, or last fired from the plain bypass/post-draw callers instead -
		// see HDRRoundTrip::QualityFaultSeam::kSeamDrawCloseFailure. Since the seam is
		// session-terminal it can trigger at most once per session, so in practice this
		// is simply whether that one trigger was the interesting caller.
		bool qualitySeamDrawCloseFailureAtPublish = false;
	};

	// a_sharpness is the 0.0..1.0 setting; anything outside that range is off. It is not
	// part of the handback episode: a change re-uploads one constant on the render thread
	// and re-selects the pixel shader, and never restarts the bounded gate.
	[[nodiscard]] bool Configure(
		bool a_postDrawEnabled,
		bool a_bypassEnabled,
		bool a_requireNgxHandback,
		bool a_requireFfxHandback,
		std::uint64_t a_targetSuccesses,
		float a_sharpness,
		// The product path has no target - this gate never self-retires while it
		// is true, whatever a_targetSuccesses says. See HDRRoundTrip::Configure's matching
		// parameter for why this is a separate bool rather than a sentinel target value.
		bool a_unbounded) noexcept;

	// Lock-free terminal close for an interrupted multi-component configuration
	// transaction.  It never takes g_configLock, which the interrupted Configure
	// may have stranded, and always leaves the admission epoch even.
	void AbortConfiguration() noexcept;

	// One TemporalAA-seam attempt. drew means the draw was issued; succeeded means it was
	// issued and the host state and device were proved afterwards. A seam publish refused
	// before issue says why: the world-admission recheck vetoed it, or a DevBench fault
	// (injectedSeam) stood in for a lost seam path. issuedUnrecoverable means public
	// pixels may have changed and nothing about them can be proved.
	struct DrawResult
	{
		bool drew = false;
		bool succeeded = false;
		bool faultInjected = false;
		bool worldAdmissionVeto = false;
		bool issuedUnrecoverable = false;
		HDRRoundTrip::QualityFaultSeam injectedSeam = HDRRoundTrip::QualityFaultSeam::kNone;
	};

	enum class AbortResult : std::uint32_t
	{
		kNoRepairNeeded,
		kHistoryRepaired,
		kUnrecoverable,
	};

	// Render-thread preflight for a reduced-NGX quality transaction. This compiles the
	// private pass and records the parity-selected history before NGX can advance either
	// history or overwrite engine colour. It never writes an engine render target.
	// Returns the exact stable even configuration epoch admitted for this frame, or
	// zero on failure. The seam publish must present the same epoch; a semantically
	// identical off/on reload is still a different episode and fails closed.
	[[nodiscard]] std::uint64_t PrepareQualityBypass() noexcept;

	// Called before Bethesda's prepared draw. In standalone diagnostics, drew=false leaves
	// the caller free to run vanilla. In an owned quality transaction (a_publishAtSeam)
	// the draw is the publish: after the pending episode, parity, bindings and the world
	// admission ticket validate, one MRT draw writes the parity-selected history and RT4
	// in place of the engine's draw. A publish refused before issue (veto, injected fault,
	// validation) hands the frame to the native fallback: the caller releases the episode
	// and runs the prepared draw itself. An issued draw that cannot be proved is terminal.
	[[nodiscard]] DrawResult TryBypass(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback,
		bool a_publishAtSeam,
		std::uint64_t a_qualityConfigEpoch,
		ResetPolicy::WorldAdmissionTicket a_worldAdmission) noexcept;

	// Called only after the shared ImageSpace pixel-draw hook has run Bethesda's prepared
	// draw while its thread-local TemporalAA scope is active, and only for the standalone
	// post-draw diagnostic: an owned quality transaction publishes at the seam or finishes
	// on the native draw, and its caller never reaches this. drew means the private MRT
	// overwrite ran; otherwise the already-produced vanilla outputs are unchanged.
	[[nodiscard]] DrawResult TryOverwrite(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback) noexcept;

	// Render-thread only, from the late ImageSpace range. A seam-published episode is
	// counted as the accepted bypass it is (successes, history writes) and released.
	// False when no published episode is pending. Never touches D3D11.
	[[nodiscard]] bool CommitSeamPublish() noexcept;

	// Render-thread only. A pending episode that never published is abandoned: the parity
	// already advanced past a TemporalAA draw that did not happen, so the selected output
	// history must receive the retained RT4 before native TAA consumes it next frame. A
	// published episode must not reach this - its history is already right.
	[[nodiscard]] AbortResult AbortPendingQualityEpisode() noexcept;

	// Render-thread only. A transaction that already wrote RT1 finishes on the engine's
	// own TemporalAA: the pending episode is dropped without a repair copy, because the
	// native draw writes the parity-selected history itself. Returns whether an episode
	// was pending. Never touches D3D11.
	bool ReleaseStagedQualityForNativeFallback() noexcept;

	// Render-thread only. A seam-published episode closed by an abort path instead of the
	// late range is dropped without a repair or a success count. Returns whether a
	// published episode was pending. Never touches D3D11.
	bool ReleasePublishedQualityEpisode() noexcept;

	// Render-thread only, immediately before the native prepared draw of a fallback
	// frame. Reads the parity the engine selects its output history from, records the
	// expected/engine history pair exactly as the bypass validation does, and counts the
	// draw against that history. Never touches D3D11.
	void RecordNativeFallbackDraw(std::uint64_t a_temporalAACall) noexcept;

	void DiscardHandbackCandidate(HDRRoundTrip::HandbackStamp a_handback) noexcept;

	// Read-only mirror of this module's own one-way latch, for HDRRoundTrip's recovery
	// predicate. This latch has no recovery of its own yet; nothing here clears it.
	[[nodiscard]] bool SessionDisabled() noexcept;

	// The reason for the disable that first tripped the latch above this session, or
	// "none" before any. First-failure-wins: a later disable never replaces this string.
	[[nodiscard]] const char* SessionDisabledReason() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
