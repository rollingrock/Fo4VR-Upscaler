#pragma once

// The live gate between a read-only HDR observation and real reconstruction.
//
// Every run probes D3D11.1 context-state creation on the verified primary render
// thread. The optional diagnostics go deliberately boring steps farther: first
// a bounded descriptor-matched full-resource round trip, then a mutually exclusive
// bounded double-wide -> two zero-origin eyes -> NullBackend -> double-wide round trip.
// A third mutually exclusive gate initializes raw NGX and creates two per-eye histories
// without evaluating a frame. A fourth evaluates exactly one private two-eye frame. A
// fifth reassembles one vendor frame into a private double-wide texture but still makes no
// engine color copy. A sixth copies exactly one accepted private result back to logical
// color 1. The seventh preserves both histories through 16 private warm-up frames and a
// configurable bounded handback window, consumes paired load/menu/cell reset requests and
// begins safe-release teardown on the next eligible HDR frame. In reconstruction modes,
// input eyes are packed in the engine's reduced viewport and output remains full-sized;
// each accepted handback also arms the unity ratio bracket the post-HDR chain runs
// under, with slot 21 reporting inactive so TemporalAA is the last active effect. An eighth,
// FSR-specific gate evaluates one stereo
// frame into private per-eye outputs. A ninth reassembles one FSR stereo frame into the
// private double-wide target, and a tenth performs one engine handback. The eleventh
// preserves both FSR histories through the same 16-frame private warm-up and 512-frame
// handback window before render-thread teardown. Together they prove placement, lifetime,
// stereo orchestration, exact handback and bounded temporal vendor work before a production
// path is admitted.

#include "Upscaler/ResetPolicy.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

struct ID3D11Texture2D;

namespace Upscaler::HDRRoundTrip
{
	inline constexpr std::uint64_t kTargetSuccesses = 512;
	inline constexpr std::uint64_t kNgxContinuousWarmupSuccesses = 16;
	inline constexpr std::uint64_t kDefaultContinuousTargetSuccesses = 512;
	inline constexpr std::uint64_t kFfxContinuousWarmupSuccesses = 16;
	inline constexpr std::uint32_t kMaxQualityFaultCount = 64;
	// INFERRED: no measurement backs these numbers yet; tune both from a fault run once
	// one exists. kRearmsPerSession bounds how many times one session may suspend and
	// re-arm before a further fault goes terminal; kRearmCooldownFrames is how long a
	// suspend counts down, in frames, before the existing preparation path runs again.
	inline constexpr std::uint32_t kRearmsPerSession = 8;
	inline constexpr std::uint32_t kRearmCooldownFrames = 90;

	enum class HandbackSource : std::uint32_t
	{
		kNone,
		kNgx,
		kFfx,
	};

	// DevBench-only fault seams for deterministic quality-transaction abort, native
	// fallback and publication proof. No TOML setting exposes these in normal play.
	// preBackup and postBackup both fire before the RT1 write (the backups they once
	// bracketed are gone) and abort the frame to vanilla. postRt1 fires after it, inside
	// the HDR dispatch, and finishes the frame on the native fallback. The next two fire
	// at the TemporalAA seam, where the transaction publishes: postTaaStage before the
	// seam draw is attempted, prePublish after its bindings and world admission were
	// validated and immediately before the draw is issued - both finish on the native
	// fallback. postPublish fires after the seam draw wrote history and RT4 and is
	// terminal by design.
	//
	// The next three fire AFTER the real mutating call each stands beside, instead of
	// declining before it: the CopyResource/Draw/capture notification all genuinely run,
	// and only the close or notify result the caller reads is forced to report failure.
	// That drives the real Disable branch at each seam with correct pixels underneath,
	// which the seams above cannot: preBackup/postBackup/postRt1/postTaaStage/prePublish
	// all take an early return before the branch that calls Disable. Appended after
	// kPostPublish so no existing value moves. copyCloseFailure stands beside
	// CopyQualityPixelsToEngine's isolation End() and reaches HDRRoundTrip.cpp's
	// !rt1Written branch; seamDrawCloseFailure stands beside DrawBindingsIsolated's
	// isolation End() in TAAReplacement.cpp and reaches DrawBindings' !draw.ended branch;
	// captureNotifyFailure stands beside NotifyQualityCapture's SEH shell and reaches the
	// three conservative disables downstream of it. All three are session-terminal today,
	// like postPublish, so a run may not arm one with a count above 1.
	//
	// The next two are consumed from Hooks.cpp, not this file, but arm and are named
	// through this same vocabulary and route - Hooks has no fault vocabulary of its own.
	// Both follow the same after-the-real-call shape: geometryPreflightFailure stands
	// beside PreflightDynamicGeometryFlag's guarded 0 -> 1 -> 0 probe, after the byte is
	// already restored, and reaches that function's own "failed the guarded 0 -> 1 -> 0
	// preflight" refusal; geometryRestoreFailure stands beside the late ImageSpace range's
	// ratio/geometry restore in FinishQualityHandbackAfterLateRange, after both restores
	// have genuinely run, and reaches that function's "could not close the ratio/geometry
	// transaction" Disable. Both trip Hooks' own g_dynamicGeometrySessionDisabled latch,
	// which has no recovery, so both are session-terminal like the three above.
	//
	// kParityDriftFailure stands beside TAAReplacement.cpp's own byte-verified TAA
	// history-parity guard, at TryBypassImpl's a_publishAtSeam branch: the parity read
	// genuinely happens and genuinely matches the admitted candidate, and only the
	// comparison result is forced to report drift, reaching that guard's own
	// "the native-TAA history parity changed" Disable with repairBothHistories still set,
	// same as a real drift would. Unlike the three post-mutation seams above, no engine
	// call is forced to report a failure it did not have - this simulates a REPORT of a
	// validation failure, not a close failure. Session-terminal like the rest of this
	// group. Appended after kGeometryRestoreFailure so no existing value moves.
	enum class QualityFaultSeam : std::uint32_t
	{
		kNone,
		kPreBackup,
		kPostBackup,
		kPostRt1,
		kPostTaaStage,
		kPrePublish,
		kPostPublish,
		kCopyCloseFailure,
		kSeamDrawCloseFailure,
		kCaptureNotifyFailure,
		kGeometryPreflightFailure,
		kGeometryRestoreFailure,
		kParityDriftFailure,
	};

	// Why a reduced-NGX transaction that had already written RT1 finished on the engine's
	// own TemporalAA instead of the seam publish. Recorded on the frame and counted per
	// seam, so a fault run can prove which seam each fallback came from.
	enum class QualityFallbackSeam : std::uint32_t
	{
		kNone,
		kInjectedPostRt1,
		kCopyClose,
		kDeviceRemovedAfterCopy,
		kSeamDrawNotAttempted,
		kSeamAdmissionVeto,
		kInjectedPostTaaStage,
		kInjectedPrePublish,
		kUnexpectedSecondTaaDraw,
		kAbnormalTaaReturn,
		kCount,
	};
	inline constexpr std::size_t kQualityFallbackSeamCount =
		static_cast<std::size_t>(QualityFallbackSeam::kCount);

	// A serial is meaningful only within its tagged backend. The candidate stamp is
	// published after private evaluation and the isolated engine copy, but before TAA and
	// the late ImageSpace range. It admits only that downstream transaction; completed
	// success counters advance later, after the complete frame contract closes.
	struct HandbackStamp
	{
		HandbackSource source = HandbackSource::kNone;
		std::uint64_t serial = 0;
	};

	// Exact per-eye dimensions used by this transaction's vendor evaluation. Unlike
	// the default-ratio snap cache, these also describe an explicit slider ratio.
	struct QualityRenderExtent
	{
		std::uint32_t outputWidth = 0, outputHeight = 0;
		std::uint32_t renderWidth = 0, renderHeight = 0;
	};
	// Render thread only, while the reconstructed colour awaits seam publication.
	// Used to match a pre-HDR mask viewport to the exact evaluated transaction.
	[[nodiscard]] bool StagedQualityRenderExtent(
		HandbackStamp a_expected, QualityRenderExtent& a_out) noexcept;
	// Render thread only, after seam publication and before the late-range close.
	// Rejects stale stamps, configuration changes and replaced source resources.
	[[nodiscard]] bool PublishedQualityRenderExtent(
		HandbackStamp a_expected, QualityRenderExtent& a_out) noexcept;

	// The late range's answer for a transaction the seam already published: kPublished
	// once the accounting closed, kFailedBeforePublish when no seam-published candidate
	// was there to commit (the caller then closes the frame without a repair).
	enum class QualityCommitResult : std::uint32_t
	{
		kFailedBeforePublish = 0,
		kPublished,
	};

	// The backend the stereoMode setting was resolved against: the one the session's
	// vendor gates drive, or the selected backend for the null control arm.
	enum class StereoBackend : std::uint32_t
	{
		kNone,
		kNgx,
		kFfx,
		kNull,
		kOther,
	};

	// Whether a fault that would disable the session may instead suspend and re-arm.
	// Evaluated twice in a recovery cycle - once at the suspend edge and again immediately
	// before the latch clears, because a sibling latch or a device removal during the
	// cool-down must refuse a re-arm the suspend-time verdict had allowed. Both calls run
	// after the frame that raised the fault has fully unwound: the quality
	// pixel phase by then reflects the transaction's settled outcome (a native fallback or
	// a seam publish already closed by its own late-range hook), not a mid-callstack
	// snapshot taken at the exact store. kNone means every predicate held, or the session
	// was never disabled.
	enum class RecoveryRefusal : std::uint32_t
	{
		kNone,
		kMidTransaction,        // the quality pixel phase was not kIdle when classified
		kDeviceRemoved,         // the D3D11 device no longer reports itself intact
		kSiblingLatchTripped,   // TAAReplacement's or Hooks' own one-way latch is already set
		kFailSoftDisabled,      // failSoft is false: every fault is terminal
		kRearmBudgetExhausted,  // kRearmsPerSession re-arms already spent this session
		// The owned vendor teardown scheduled at suspend reached its failure branch or left
		// the private texture/views live instead of completing: giving the vendor state back
		// is not provably safe, so the session may not reuse it. Distinct
		// from kSiblingLatchTripped/kDeviceRemoved - this is our own teardown's outcome, not
		// a sibling module's latch or the device.
		kTeardownFailed,
		// The re-arm itself succeeded - the teardown completed, the predicate held fresh -
		// but every gate-enabled flag was clear at that instant, so nothing will prepare.
		// AbortConfiguration wipes them on any settings reload observed while the session is
		// disabled, which spans the whole cool-down. Reported so a run can tell this apart
		// from a session that was simply never configured; sessionDisabled is false and
		// SessionState reads kOff either way.
		kGatesClearedDuringCooldown,
		// No backend identity was owned, but the isolated Evaluate/plugin-private cleanup
		// did not prove both its Reset and exact host-state restoration. Kept distinct from
		// kTeardownFailed so a failed vendor release is never relabelled as plugin cleanup.
		kCleanupFailed,
	};

	// Derived from the existing gate/readiness flags and the recovery bookkeeping below,
	// not independently driven - nothing here can desync from the signals it reads. Off
	// is no gate enabled; Preparing is a gate enabled but the private double-wide/backend
	// views not yet built; Active is both; Suspended and Terminal read sessionDisabled
	// plus suspendFramesLeft.
	enum class SessionState : std::uint32_t
	{
		kOff,
		kPreparing,
		kActive,
		kSuspended,
		kTerminal,
	};

	[[nodiscard]] std::string_view RecoveryRefusalName(RecoveryRefusal a_refusal) noexcept;
	[[nodiscard]] std::string_view SessionStateName(SessionState a_state) noexcept;

	struct State
	{
		bool enabled = false;  // byte-preserving full-resource round trip
		bool stereoNullEnabled = false;
		bool ngxCreateEnabled = false;
		bool ngxEvaluateEnabled = false;
		bool ngxReassembleEnabled = false;
		bool ngxHandbackEnabled = false;
		bool ngxContinuousEnabled = false;
		bool ffxEvaluateEnabled = false;
		bool ffxReassembleEnabled = false;
		bool ffxHandbackEnabled = false;
		bool ffxContinuousEnabled = false;
		bool isolationAttempted = false;
		bool isolationReady = false;
		bool privateReady = false;
		bool completed = false;
		bool stereoNullCompleted = false;
		bool ngxCreateCompleted = false;
		bool ngxEvaluateCompleted = false;
		bool ngxReassembleCompleted = false;
		bool ngxHandbackCompleted = false;
		bool ngxContinuousCompleted = false;
		bool ngxTeardownPending = false;
		bool ngxTeardownCompleted = false;
		bool ffxEvaluateCompleted = false;
		bool ffxReassembleCompleted = false;
		bool ffxHandbackCompleted = false;
		bool ffxContinuousCompleted = false;
		bool ffxTeardownPending = false;
		bool ffxTeardownCompleted = false;
		bool ngxQualityCandidatePending = false;
		bool sessionDisabled = false;
		// Connected: failSoft as last published by Configure. false reproduces every
		// fault-matrix verdict bit for bit - the session goes Terminal on the first fault,
		// exactly as before the recovery predicate existed.
		bool failSoft = false;
		// TAAReplacement's and Hooks' own one-way latches, read-only. The suspend and re-arm
		// machine covers only this file's g_sessionDisabled; a fault that tripped either
		// sibling latch leaves a session that cannot publish even if this one re-arms, so
		// the predicate refuses whenever either is set.
		bool taaReplacementSessionDisabled = false;
		bool dynamicGeometrySessionDisabled = false;
		// The mode Configuration.cpp resolved this session from - "off" when the
		// [Diagnostics] gates decide instead. One of the six literals IsKnownMode accepts
		// (SettingsPolicy.h), mirrored as a canonical C-string pointer the way
		// dynamicGeometryDisableReason below is: no allocation, and safe past the settings
		// snapshot's own lifetime because every value is a static string. productPath is
		// resolvedMode != "off"; kept as its own bool so a reader does not have to compare
		// strings to answer "which surface armed this session".
		std::string_view resolvedMode = "off";
		bool productPath = false;
		// The reason for the disable that first tripped the latch above this session,
		// mirrored read-only from Hooks::DynamicGeometryDisableReason() - "none" before
		// any. First-failure-wins, so a seam wired to the wrong Disable call reads a
		// different string than the one it is documented to reach.
		std::string_view dynamicGeometryDisableReason = "none";
		// Same, mirrored read-only from TAAReplacement::SessionDisabledReason() for the
		// sibling latch above.
		std::string_view taaReplacementDisableReason = "none";

		std::uint64_t isolationAttempts = 0;
		std::uint64_t roundTripAttempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t failures = 0;
		std::uint64_t skips = 0;
		std::uint64_t resourceBuilds = 0;
		std::uint64_t stereoNullAttempts = 0;
		std::uint64_t stereoNullSuccesses = 0;
		std::uint64_t stereoNullFailures = 0;
		std::uint64_t stereoNullSkips = 0;
		std::uint64_t ngxCreateAttempts = 0;
		std::uint64_t ngxCreateSuccesses = 0;
		std::uint64_t ngxCreateFailures = 0;
		std::uint64_t ngxCreateSkips = 0;
		std::uint64_t ngxEvaluateAttempts = 0;
		std::uint64_t ngxEvaluateSuccesses = 0;
		std::uint64_t ngxEvaluateFailures = 0;
		std::uint64_t ngxEvaluateSkips = 0;
		std::uint64_t ngxReassembleAttempts = 0;
		std::uint64_t ngxReassembleSuccesses = 0;
		std::uint64_t ngxReassembleFailures = 0;
		std::uint64_t ngxReassembleSkips = 0;
		std::uint64_t ngxHandbackAttempts = 0;
		std::uint64_t ngxHandbackSuccesses = 0;
		std::uint64_t ngxHandbackFailures = 0;
		std::uint64_t ngxHandbackSkips = 0;
		std::uint64_t ngxHandbackCopies = 0;
		std::uint64_t ngxContinuousAttempts = 0;
		std::uint64_t ngxContinuousWarmupSuccesses = 0;
		std::uint64_t ngxContinuousSuccesses = 0;
		std::uint64_t ngxContinuousFailures = 0;
		bool nativeScopeEpisode = false;
		std::uint64_t nativeScopeSkips = 0;
		std::uint64_t nativeScopeResetRequests = 0;
		std::uint64_t nativeScopeResumes = 0;
		std::uint64_t nativeScopeLastMarker = 0;
		std::uint32_t nativeScopeLastThread = 0;
		std::uint64_t nativeScopeRejectedJitter = 0;
		bool nativeJitterGateEpisode = false;
		std::uint64_t nativeJitterGateDeclines = 0;
		std::uint64_t nativeJitterGateStaleDeferrals = 0;
		std::uint64_t nativeJitterGateResetRequests = 0;
		std::uint64_t nativeJitterGateResumes = 0;
		std::uint64_t ngxContinuousSkips = 0;
		std::uint64_t ngxContinuousCopies = 0;
		std::uint64_t ngxQualityCandidates = 0;
		std::uint64_t ngxQualityCommits = 0;
		std::uint64_t ngxQualityAborts = 0;
		std::uint64_t qualityPixelEvaluations = 0;
		std::uint64_t qualityPixelCopies = 0;
		// Frames that wrote RT1 and then finished on native TemporalAA, split by the seam
		// that armed the fallback and by who closed it: the late ImageSpace range (the
		// normal case) or an abort path such as a watchdog.
		std::uint64_t qualityPixelNativeFallbacks = 0;
		std::uint64_t qualityPixelFallbackLateCloses = 0;
		std::uint64_t qualityPixelFallbackAbortCloses = 0;
		std::uint64_t qualityPixelFallbackSeamCounts[kQualityFallbackSeamCount] = {};
		// Frames the TemporalAA seam published into the parity-selected history and RT4
		// with one draw, and the subset a watchdog or an abnormal return closed instead of
		// the late range. A published frame needs no repair and no reset either way: the
		// seam draw wrote the history the next frame reads.
		std::uint64_t qualityPixelSeamPublishes = 0;
		std::uint64_t qualityPixelSeamPublishAbortCloses = 0;
		// An engine-colour-dirty transaction closed without a commit and without arming the
		// fallback: a frame that reached the late range with neither a seam publish nor a
		// fallback (the TemporalAA draw was never dispatched) and any watchdog that catches
		// such a frame. RT1 keeps the engine's own late intermediate, the range routed its
		// last active effect into RT4, and the selected history is repaired from RT4;
		// nothing displayed or consumed depends on RT1 or RT3 after that point, so putting
		// the reduced raster back would change nothing. An expected count, not an error.
		std::uint64_t qualityPixelDirtyCloses = 0;
		std::uint64_t qualityPixelUnrecoverable = 0;
		std::uint64_t qualityFaultArmAttempts = 0;
		std::uint64_t qualityFaultArms = 0;
		std::uint64_t qualityFaultRejections = 0;
		std::uint64_t qualityFaultTriggers = 0;
		// Commits plus aborts as read where a capture-notification failure disabled the
		// session. Equal to the final total when the transaction finished first, one short
		// when it did not - the ordering a poll of the two counters alone cannot show.
		std::uint64_t qualityCaptureDisableAtTransactions = 0;
		std::uint64_t ngxHandbackCandidateSerial = 0;
		// Native candidates retired unconsumed at the next publish: frames whose TemporalAA
		// seam never took the handback and ran vanilla TemporalAA instead.
		std::uint64_t nativeCandidatesRetired = 0;
		std::uint64_t ngxContinuousWarmupTarget = kNgxContinuousWarmupSuccesses;
		std::uint64_t ngxContinuousTargetSuccesses = kDefaultContinuousTargetSuccesses;
		// True on the product path (mode != "off"). g_continuousTargetState packs
		// this beside the numeric target's own lock bit instead of using a sentinel value -
		// 0 or a maximal uint64 would both read as "bounded at a very large/small number" to
		// a caller that forgets to check for it, and a companion flag cannot be misread that
		// way. ngxContinuousTargetSuccesses/ffxContinuousTargetSuccesses stay 0 while this is
		// true: the successes >= target check that would self-retire the gate never runs, so
		// the number has nothing to mean. One field for both backends because the target
		// state is one atomic shared between them - only one of the two gates a session
		// actually runs.
		bool continuousTargetUnbounded = false;
		std::uint64_t ngxTeardownAttempts = 0;
		std::uint64_t ngxTeardownSuccesses = 0;
		std::uint64_t ngxTeardownFailures = 0;
		std::uint64_t ffxEvaluateAttempts = 0;
		std::uint64_t ffxEvaluateSuccesses = 0;
		std::uint64_t ffxEvaluateFailures = 0;
		std::uint64_t ffxEvaluateSkips = 0;
		std::uint64_t ffxReassembleAttempts = 0;
		std::uint64_t ffxReassembleSuccesses = 0;
		std::uint64_t ffxReassembleFailures = 0;
		std::uint64_t ffxReassembleSkips = 0;
		std::uint64_t ffxHandbackAttempts = 0;
		std::uint64_t ffxHandbackSuccesses = 0;
		std::uint64_t ffxHandbackFailures = 0;
		std::uint64_t ffxHandbackSkips = 0;
		std::uint64_t ffxHandbackCopies = 0;
		std::uint64_t ffxContinuousAttempts = 0;
		std::uint64_t ffxContinuousWarmupSuccesses = 0;
		std::uint64_t ffxContinuousSuccesses = 0;
		std::uint64_t ffxContinuousFailures = 0;
		std::uint64_t ffxContinuousSkips = 0;
		std::uint64_t ffxContinuousCopies = 0;
		std::uint64_t ffxContinuousWarmupTarget = kFfxContinuousWarmupSuccesses;
		std::uint64_t ffxContinuousTargetSuccesses = kDefaultContinuousTargetSuccesses;
		std::uint64_t privateReassemblySerial = 0;
		std::uint64_t ffxTeardownAttempts = 0;
		std::uint64_t ffxTeardownSuccesses = 0;
		std::uint64_t ffxTeardownFailures = 0;
		std::uint64_t unownedRecoveryAttempts = 0;
		std::uint64_t unownedRecoverySuccesses = 0;
		std::uint64_t unownedRecoveryFailures = 0;
		std::uint64_t targetSuccesses = kTargetSuccesses;
		std::uint64_t dynamicUpsampleSuppressionsArmed = 0;

		std::uint32_t threadId = 0;
		std::uint32_t featureLevel = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;
		std::uint32_t sampleCount = 0;
		std::uint32_t lastExceptionCode = 0;
		std::uint32_t qualityPixelPhase = 0;
		std::uint32_t qualityPixelLastFallbackSeam = 0;
		std::uint32_t qualityFaultArmedSeam = 0;
		std::uint32_t qualityFaultRemaining = 0;
		std::uint32_t qualityFaultLastTriggeredSeam = 0;

		// The suspend/re-arm state machine (SCOPE - HDRRoundTrip.cpp's own g_sessionDisabled
		// only; see RecoveryRefusal/SessionState above).
		std::uint32_t sessionState = 0;       // SessionState
		std::uint32_t recoveryRefusal = 0;    // RecoveryRefusal; kNone unless the last
											  // disable was classified terminal, or the
											  // last re-arm landed with every gate clear
		std::uint32_t suspendFramesLeft = 0;  // cool-down remaining while Suspended
		std::uint32_t rearmCount = 0;         // successful re-arms this session
		std::uint32_t suspensionCount = 0;    // faults classified recoverable this session

		std::int32_t lastError = 0;
		std::int32_t deviceRemovedReason = 0;
		std::uintptr_t device = 0;
		std::uintptr_t context = 0;
		std::uintptr_t sourceTexture = 0;
		std::uintptr_t privateTexture = 0;

		// stereoMode as read from the settings and as resolved against the backend's
		// Caps::supportsSubrects; the two differ only when the setting asked for
		// sub-rects on a backend that cannot address them.
		bool stereoSubrectsRequested = false;
		bool stereoSubrectsEffective = false;
		std::uint32_t stereoBackend = 0;  // StereoBackend
	};

	// Public POD admission shared by HDR and every TAA replacement render entry.
	// Nested entries on the same render thread inherit the outer stable epoch and
	// keep one central active-count ownership until the outermost leave.
	struct RuntimeConfigurationEntryToken
	{
		bool entered = false;
		bool ownsAdmission = false;
		std::uint64_t epoch = 0;
	};

	[[nodiscard]] RuntimeConfigurationEntryToken EnterRuntimeConfiguration() noexcept;
	void LeaveRuntimeConfiguration(RuntimeConfigurationEntryToken& a_token) noexcept;

	// What BeginConfiguration below did. kDeferred is the one outcome that leaves nothing
	// to undo: the reload is refused before the epoch is touched or a gate is cleared, so
	// its caller must NOT run the interrupted-transaction abort. Aborting there would wipe
	// this session's gates and latch TAAReplacement's one-way latch, which turns a
	// cool-down that was about to re-arm into a terminal session.
	enum class ConfigurationBegin : std::uint32_t
	{
		kBegan,     // the epoch is odd and the gates are cleared; Configure must follow
		kFailed,    // fail-closed; the caller's abort path owns whatever was mutated
		kDeferred,  // a suspended session is still counting down; nothing was touched
	};

	// Atomic and safe from the settings/reload threads. Disabling a prepared raw backend
	// schedules release on the next verified render-thread callback.
	// Closes the render-thread admission epoch before the caller updates any of the
	// coupled DynamicResolution/TAA/HDR settings. Configure publishes the complete HDR
	// snapshot and reopens admission after those sibling components are stable.
	[[nodiscard]] ConfigurationBegin BeginConfiguration() noexcept;
	[[nodiscard]] bool Configure(
		bool a_roundTripEnabled,
		bool a_stereoNullEnabled,
		bool a_ngxCreateEnabled,
		bool a_ngxEvaluateEnabled,
		bool a_ngxReassembleEnabled,
		bool a_ngxHandbackEnabled,
		bool a_ngxContinuousEnabled,
		bool a_ffxEvaluateEnabled,
		bool a_ffxReassembleEnabled,
		bool a_ffxHandbackEnabled,
		bool a_ffxContinuousEnabled,
		std::uint64_t a_continuousTargetSuccesses,
		std::string_view a_runtimeDir,
		std::string_view a_ngxPreset,
		std::string_view a_quality,
		std::string_view a_stereoMode,
		bool a_failSoft,
		// The resolved Settings::mode ("off" on the diagnostic path), mirrored for
		// /state, and whether this session's continuous gate has a target at all. The two
		// are independent parameters rather than one derived from the other so a caller
		// cannot report a mode /state disagrees with the gate's own bounded/unbounded fact.
		std::string_view a_mode,
		bool a_continuousUnbounded) noexcept;

	// The documented default render scale for a quality name, so the settings layer can
	// resolve renderScaleOverride=0 without a second copy of the name vocabulary. False
	// means the name is not one this build parses.
	[[nodiscard]] bool DefaultRenderScaleForQuality(std::string_view a_quality, double& a_out) noexcept;

	// The same table read as output over render, which is the direction the startup
	// ratio is snapped in. False means the name is not one this build parses.
	[[nodiscard]] bool VendorScalingRatioForQuality(std::string_view a_quality, double& a_out) noexcept;

	// Lock-free close for an interrupted settings transaction. Clears every render gate
	// and normalizes an odd epoch. Disables the session exactly like any other fault: with
	// failSoft false this is terminal, as before; with failSoft true the recovery
	// predicate gets the same say over it as over a quality-transaction fault, because it
	// is the same g_sessionDisabled latch.
	void AbortConfiguration() noexcept;

	// Render-thread only, from the verified primary ClearColor seam. The create-only
	// diagnostic uses the first frame whose native colour and motion descriptors exist
	// to initialize NGX before a world load reaches the HDR dispatch. Missing startup
	// resources are a retryable skip; the HDR seam remains the exact fallback.
	void OnPrimaryRenderThread() noexcept;

	// Render-thread only, immediately before the complete vanilla HDR dispatch.
	void OnDispatch(
		std::uintptr_t a_manager,
		std::int32_t a_sourceLogical,
		std::int32_t a_destinationLogical) noexcept;
	// True only when this HDR entry encountered an owned quality abort whose TAA/history
	// release or host-state recovery could not be proved. The outer hook must hold the
	// current vanilla HDR dispatch rather than consume uncertain pixels.
	[[nodiscard]] bool ConsumeQualityHDRHold() noexcept;

	// Render-thread one-shot contract for the later ImageSpace dynamic-resolution
	// effect. A successful reduced-input/full-output NGX handback arms it; only the
	// matching effect-slot dispatch consumes it. The effect itself still executes so
	// manager routing remains correct, but its scale is temporarily unity. Every HDR
	// dispatch clears stale state.
	[[nodiscard]] bool ConsumeDynamicUpsampleSuppression() noexcept;

	// Per-source candidate serials are process-lifetime monotonic counters. TAA snapshots
	// its selected source when arming and accepts only baseline + 1 followed by consecutive
	// values. A candidate is not a committed success: the TemporalAA seam publishes it,
	// and Hooks commits or aborts it after the complete HDR/TAA/late-range transaction.
	[[nodiscard]] HandbackStamp HandbackCandidateStamp() noexcept;
	[[nodiscard]] std::uint64_t HandbackCandidateSerial(HandbackSource a_source) noexcept;
	[[nodiscard]] std::uint64_t QualityTaaConfigEpoch() noexcept;
	[[nodiscard]] bool QualityRuntimeConfigReady() noexcept;
	[[nodiscard]] QualityCommitResult CommitQualityHandbackCandidate() noexcept;
	[[nodiscard]] bool AbortQualityHandbackCandidate() noexcept;
	[[nodiscard]] std::string_view HandbackSourceName(HandbackSource a_source) noexcept;

	// Render-thread only, at the TemporalAA seam. The world-admission ticket the in-flight
	// transaction was admitted with (empty unless RT1 holds the reconstruction), so the
	// seam can recheck admission before its one public draw. MarkQualityPublishedAtSeam
	// records that the draw wrote the parity-selected history and RT4; from then on the
	// late range only closes the brackets and commits, and any abort path releases the
	// frame without a repair or a reset. LatchQualityPublishUnrecoverable is the terminal
	// close for a seam draw that was issued but whose outcome or host state cannot be
	// proved: the candidate is discarded, the session disabled and the paired reset owed.
	// QualityTransactionUnrecoverable reports that latch - from the terminal close or from
	// a fail-closed abort over an unproved host state - so the late range closes only its
	// brackets: everything else is already done, and a further abort could only report
	// the latch as a second failure.
	[[nodiscard]] ResetPolicy::WorldAdmissionTicket QualityWorldAdmission() noexcept;
	[[nodiscard]] bool MarkQualityPublishedAtSeam() noexcept;
	[[nodiscard]] bool QualityPublishedAtSeam() noexcept;
	void LatchQualityPublishUnrecoverable() noexcept;
	[[nodiscard]] bool QualityTransactionUnrecoverable() noexcept;

	// Render-thread only. Once RT1 holds the reconstruction, a failure before the seam
	// draw arms the native fallback instead of aborting: the unity ratio/geometry bracket
	// stays up, the engine's own HDR, gamma and TemporalAA consume the full-resolution
	// colour, and the native draw writes the parity-selected history and RT4 itself,
	// because slot 21 reports inactive at unity and TemporalAA is the range's last active
	// effect. The pending episode and any candidate are released without a repair copy
	// or a paired reset. False when the transaction is not in the engine-colour-dirty
	// phase or the host context state is unproved; the caller then keeps its fail-closed
	// abort.
	[[nodiscard]] bool ArmQualityNativeFallback(QualityFallbackSeam a_seam) noexcept;
	[[nodiscard]] bool QualityNativeFallbackArmed() noexcept;
	// The late ImageSpace range closes a fallback frame once the brackets are down: no
	// publish, no repair, no reset, only the accounting. False when nothing was armed.
	[[nodiscard]] bool CloseQualityNativeFallback() noexcept;
	[[nodiscard]] std::string_view QualityFallbackSeamName(QualityFallbackSeam a_seam) noexcept;

	// Listener-thread counted arming is lock-free and succeeds only while the coupled NGX
	// quality path is ready and idle. Render-thread consumption is one CAS and cannot
	// log, allocate or call back into DevBench from inside a D3D11 isolation scope.
	[[nodiscard]] bool ArmQualityFault(QualityFaultSeam a_seam, std::uint32_t a_count = 1) noexcept;
	[[nodiscard]] bool ConsumeQualityFault(QualityFaultSeam a_seam) noexcept;
	[[nodiscard]] std::string_view QualityFaultName(QualityFaultSeam a_seam) noexcept;
	[[nodiscard]] std::uint64_t ContinuousTargetSuccesses() noexcept;

	// Legacy NGX-only scalar retained for source compatibility. New consumers must use
	// the tagged contract so an earlier run of another backend cannot masquerade as this
	// frame's accepted handback.
	[[nodiscard]] std::uint64_t ContinuousCopySerial() noexcept;

	// Render-thread diagnostic seam. The serial advances only after both eyes were
	// accepted into the descriptor-matched private double-wide output. The borrowed
	// texture remains valid until the next HDR teardown callback.
	[[nodiscard]] std::uint64_t PrivateReassemblySerial() noexcept;
	[[nodiscard]] ID3D11Texture2D* PrivateOutput() noexcept;

	// Whether this session still expects a reconstruction to run on the frames that
	// follow. False once the session has fail-closed (`Disable`, `AbortConfiguration`, a
	// failed render-thread teardown) and false once a continuous budget has spent itself,
	// because after that the vendor is torn down and the engine renders vanilla. Three
	// acquire loads; a per-frame render-thread predicate, not a Snapshot substitute.
	//
	// It says nothing about whether this particular frame is admitted - that is the
	// world-contract bracket's job. It says only that the session as a whole still has a
	// reconstruction behind it, which is what the seams that bias sampling need to know
	// before they mutate engine state on a frame nothing will reconstruct.
	[[nodiscard]] bool ReconstructionLive() noexcept;

	// Whether the reduced-world bracket may still be opened for this session. False for
	// the whole of a disabled session - terminal, and every frame of a suspended
	// cool-down, since the latch stays set until the re-arm clears it. The pre-world hook
	// loads it once per dispatch and hands it to the world gate, which denies the frame
	// and reports WorldBlockReason::kReconstructionUnavailable exactly as the operator
	// veto denies: without it the engine keeps rasterizing the world at the reduced ratio
	// with the hidden-area mesh forced off and finishes it with its own slot-21 upsample,
	// because this file's own latch is the one thing Hooks::QualityHandbackReady() cannot
	// fold in (that predicate is called from inside this file's dispatch).
	[[nodiscard]] bool ReducedWorldHandbackAvailable() noexcept;
	// Render-thread only. No staged pixels or candidate remain after normal closure.
	[[nodiscard]] bool QualityPixelWorkClosed() noexcept;

	// Lock-free diagnostic snapshot for /state.
	[[nodiscard]] State Snapshot() noexcept;

	[[nodiscard]] std::string_view StereoBackendName(StereoBackend a_backend) noexcept;
}
