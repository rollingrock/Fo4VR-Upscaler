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
// each accepted handback also arms the later slot-21 scale neutralization. An eighth,
// FSR-specific gate evaluates one stereo
// frame into private per-eye outputs. A ninth reassembles one FSR stereo frame into the
// private double-wide target, and a tenth performs one engine handback. The eleventh
// preserves both FSR histories through the same 16-frame private warm-up and 512-frame
// handback window before render-thread teardown. Together they prove placement, lifetime,
// stereo orchestration, exact handback and bounded temporal vendor work before a production
// path is admitted.

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

	enum class HandbackSource : std::uint32_t
	{
		kNone,
		kNgx,
		kFfx,
	};

	// DevBench-only fault seams for deterministic quality-transaction
	// rollback/publication proof. No TOML setting exposes these in normal play.
	enum class QualityFaultSeam : std::uint32_t
	{
		kNone,
		kPreBackup,
		kPostBackup,
		kPostRt1,
		kPostTaaStage,
		kPrePublish,
		kPostPublish,
	};

	// A serial is meaningful only within its tagged backend. The candidate stamp is
	// published after private evaluation and the isolated engine copy, but before TAA and
	// the late ImageSpace range. It admits only that downstream transaction; completed
	// success counters advance later, after the complete frame contract closes.
	struct HandbackStamp
	{
		HandbackSource source = HandbackSource::kNone;
		std::uint64_t serial = 0;
	};

	enum class QualityCommitResult : std::uint32_t
	{
		kFailedBeforePublish = 0,
		kPublished,
		kWorldAdmissionVeto,
	};

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
		std::uint64_t ngxContinuousSkips = 0;
		std::uint64_t ngxContinuousCopies = 0;
		std::uint64_t ngxQualityCandidates = 0;
		std::uint64_t ngxQualityCommits = 0;
		std::uint64_t ngxQualityAborts = 0;
		std::uint64_t qualityPixelResourceBuilds = 0;
		std::uint64_t qualityPixelBackupPairs = 0;
		std::uint64_t qualityPixelEvaluations = 0;
		std::uint64_t qualityPixelCopies = 0;
		std::uint64_t qualityPixelRollbackAttempts = 0;
		std::uint64_t qualityPixelRollbackSuccesses = 0;
		std::uint64_t qualityPixelRollbackFailures = 0;
		std::uint64_t qualityPixelUnrecoverable = 0;
		std::uint64_t qualityFaultArmAttempts = 0;
		std::uint64_t qualityFaultArms = 0;
		std::uint64_t qualityFaultRejections = 0;
		std::uint64_t qualityFaultTriggers = 0;
		std::uint64_t ngxHandbackCandidateSerial = 0;
		std::uint64_t ngxContinuousWarmupTarget = kNgxContinuousWarmupSuccesses;
		std::uint64_t ngxContinuousTargetSuccesses = kDefaultContinuousTargetSuccesses;
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
		std::uint32_t qualityFaultArmedSeam = 0;
		std::uint32_t qualityFaultRemaining = 0;
		std::uint32_t qualityFaultLastTriggeredSeam = 0;

		std::int32_t lastError = 0;
		std::int32_t deviceRemovedReason = 0;
		std::uintptr_t device = 0;
		std::uintptr_t context = 0;
		std::uintptr_t sourceTexture = 0;
		std::uintptr_t privateTexture = 0;
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

	// Atomic and safe from the settings/reload threads. Disabling a prepared raw backend
	// schedules release on the next verified render-thread callback.
	// Closes the render-thread admission epoch before the caller updates any of the
	// coupled DynamicResolution/TAA/HDR settings. Configure publishes the complete HDR
	// snapshot and reopens admission after those sibling components are stable.
	[[nodiscard]] bool BeginConfiguration() noexcept;
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
		float a_sharpness) noexcept;

	// The documented default render scale for a quality name, so the settings layer can
	// resolve renderScaleOverride=0 without a second copy of the name vocabulary. False
	// means the name is not one this build parses.
	[[nodiscard]] bool DefaultRenderScaleForQuality(std::string_view a_quality, double& a_out) noexcept;

	// The same table read as output over render, which is the direction the startup
	// ratio is snapped in. False means the name is not one this build parses.
	[[nodiscard]] bool VendorScalingRatioForQuality(std::string_view a_quality, double& a_out) noexcept;

	// Lock-free terminal close for an interrupted settings transaction.  Clears
	// every render gate, marks the session terminal and normalizes an odd epoch.
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
	// or RT1/RT3 restoration could not be proved. The outer hook must hold the current
	// vanilla HDR dispatch rather than consume uncertain pixels.
	[[nodiscard]] bool ConsumeQualityHDRHold() noexcept;

	// Render-thread one-shot contract for the later ImageSpace dynamic-resolution
	// effect. A successful reduced-input/full-output NGX handback arms it; only the
	// matching effect-slot dispatch consumes it. The effect itself still executes so
	// manager routing remains correct, but its scale is temporarily unity. Every HDR
	// dispatch clears stale state.
	[[nodiscard]] bool ConsumeDynamicUpsampleSuppression() noexcept;

	// Per-source candidate serials are process-lifetime monotonic counters. TAA snapshots
	// its selected source when arming and accepts only baseline + 1 followed by consecutive
	// values. A candidate is not a committed success: Hooks closes or aborts it after the
	// complete HDR/TAA/slot-21/late-range transaction.
	[[nodiscard]] HandbackStamp HandbackCandidateStamp() noexcept;
	[[nodiscard]] std::uint64_t HandbackCandidateSerial(HandbackSource a_source) noexcept;
	[[nodiscard]] std::uint64_t QualityTaaConfigEpoch() noexcept;
	[[nodiscard]] bool QualityRuntimeConfigReady() noexcept;
	[[nodiscard]] QualityCommitResult CommitQualityHandbackCandidate() noexcept;
	[[nodiscard]] bool AbortQualityHandbackCandidate() noexcept;
	[[nodiscard]] std::string_view HandbackSourceName(HandbackSource a_source) noexcept;

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

	// Lock-free diagnostic snapshot for /state.
	[[nodiscard]] State Snapshot() noexcept;
}
