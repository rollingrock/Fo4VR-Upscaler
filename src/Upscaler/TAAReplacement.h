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

#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/ResetPolicy.h"

#include <cstdint>

namespace Upscaler::TAAReplacement
{
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
		std::uint64_t qualityStageDraws = 0;
		std::uint64_t qualityPublishDraws = 0;
		std::uint64_t qualityHistoryRepairArms = 0;
		std::uint64_t qualityHistoryRepairAttempts = 0;
		std::uint64_t qualityHistoryRepairCopies = 0;
		std::uint64_t qualityHistoryRepairSuccesses = 0;
		std::uint64_t qualityHistoryRepairFailures = 0;
		std::uint64_t parityReadAttempts = 0;
		std::uint64_t parityReadFailures = 0;
		std::uint64_t parityInvalidValues = 0;
		std::uint64_t currentRenderTargetMismatches = 0;
		std::uint64_t targetSuccesses = 0;
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
	};

	[[nodiscard]] bool Configure(
		bool a_postDrawEnabled,
		bool a_bypassEnabled,
		bool a_requireNgxHandback,
		bool a_requireFfxHandback,
		std::uint64_t a_targetSuccesses) noexcept;

	// Lock-free terminal close for an interrupted multi-component configuration
	// transaction.  It never takes g_configLock, which the interrupted Configure
	// may have stranded, and always leaves the admission epoch even.
	void AbortConfiguration() noexcept;

	struct DrawResult
	{
		bool drew = false;
		bool succeeded = false;
		bool faultInjected = false;
	};

	enum class PublishResult : std::uint32_t
	{
		kNotIssued,
		kSucceeded,
		kIssuedUnrecoverable,
		kWorldAdmissionVeto,
	};

	enum class AbortResult : std::uint32_t
	{
		kNoRepairNeeded,
		kHistoryRepaired,
		kUnrecoverable,
	};

	// Render-thread preflight for a reduced-NGX quality transaction. This compiles the
	// private pass and allocates its descriptor-matched RGBA8 stage before NGX can advance
	// either history or overwrite engine colour. It never writes an engine render target.
	// Returns the exact stable even configuration epoch admitted for this frame, or
	// zero on failure. Deferred staging must present the same epoch; a semantically
	// identical off/on reload is still a different episode and fails closed.
	[[nodiscard]] std::uint64_t PrepareQualityBypass() noexcept;

	// Called before Bethesda's prepared draw. In standalone diagnostics, drew=false leaves
	// the caller free to run vanilla. In a deferred owned-quality transaction, the caller
	// must suppress vanilla even when staging did not draw so the retained history/RT1/RT3
	// transaction can repair or hold fail-closed. stateUncertain is likewise terminal:
	// running the native helper against an unproved host context is never fail-soft.
	[[nodiscard]] DrawResult TryBypass(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback,
		bool a_deferSuccess,
		std::uint64_t a_qualityConfigEpoch) noexcept;

	// Called only after the shared ImageSpace pixel-draw hook has run Bethesda's prepared
	// draw while its thread-local TemporalAA scope is active. True means the private MRT
	// overwrite ran; false leaves the already-produced vanilla outputs unchanged.
	[[nodiscard]] DrawResult TryOverwrite(
		std::uint64_t a_temporalAACall,
		HDRRoundTrip::HandbackStamp a_handback,
		bool a_deferSuccess) noexcept;

	// Reduced NGX renders the TAA replacement into one private RGBA8 stage. The late
	// transaction publishes that stage to the selected real history and submitted target
	// with one MRT draw. Once that draw is issued, a later state/device failure is reported
	// as unrecoverable because the public pixels may already have changed. A context-state
	// activation fault whose exact host-state recovery cannot be proved is equally terminal
	// even when no MRT draw can be proved issued; the historical kIssuedUnrecoverable name
	// covers both terminal cases.
	[[nodiscard]] PublishResult PublishStagedQualitySuccess(
		HDRRoundTrip::HandbackStamp a_handback,
		ResetPolicy::WorldAdmissionTicket a_worldAdmission) noexcept;
	[[nodiscard]] AbortResult AbortStagedQualitySuccess() noexcept;
	[[nodiscard]] ID3D11Texture2D* StagedQualityOutput() noexcept;
	void DiscardHandbackCandidate(HDRRoundTrip::HandbackStamp a_handback) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
