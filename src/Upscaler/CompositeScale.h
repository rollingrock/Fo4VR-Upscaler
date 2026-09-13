#pragma once

// Default-off experimental correction for bytecode-audited DFComposite pixel shaders.
// It observes the prepared-packet boundary, copies the full engine camera buffer on the GPU,
// corrects only whole finite reconstruction matrices in a private buffer, binds that buffer
// for the actual helper draw, and restores the exact original CB12 binding in __finally.

#include <cstdint>
#include <string>

struct ID3D11Buffer;
struct ID3D11DeviceContext1;
struct ID3D11Multithread;

namespace Upscaler::CompositeScale
{
	enum class Decline : std::uint32_t
	{
		kNone = 0,
		kNoPass = 1,
		kNotComposite = 2,
		kNoContext = 3,
		kRatiosUnreadable = 4,
		kBindingsMissing = 5,
		kBufferSize = 6,
		kReadbackFailed = 7,
		kWrongCaller = 8,
		kWrongTechnique = 9,
		kNativeRatio = 10,
		kUnsupportedContext = 11,
		kProtectionFailed = 12,
		kResourceBuildFailed = 13,
		kPrivatePreparationFailed = 14,
		kPrivateBindFailed = 15,
		kOriginalRestoreFailed = 16,
		kStateUncertain = 17,
		kStereoBasisFailed = 18,
		kOutsideCameraContract = 19,
		kFamilyDisabled = 20,
		kStaleEpisode = 21,
		kTableMismatch = 22,
		kNotPrepared = 23,
	};

	struct PreparedDrawToken
	{
		ID3D11DeviceContext1* context1 = nullptr;  // getter-owned until EndPreparedPacketDraw
		ID3D11Buffer* originalCamera = nullptr;     // getter-owned until EndPreparedPacketDraw
		ID3D11Multithread* multithread = nullptr;   // owned token ref until Leave
		std::uint32_t firstConstant = 0;
		std::uint32_t numConstants = 0;
		std::uint32_t captureIndex = 0xFFFFFFFFu;
		std::uint32_t technique65600CaptureIndex = 0xFFFFFFFFu;
		std::uint32_t coverageIndex = 0xFFFFFFFFu;
		std::uint32_t coverageDecline = 0;
		std::uint32_t coverageLegacyDecline = 0;
		bool coverageAdmitted = false;
		bool coverageRestored = false;
		std::uint64_t generation = 0;
		std::uint64_t technique65600CorrectionEpoch = 0;
		bool entered = false;
		bool callOriginal = true;
		bool privateBound = false;
		bool correctionPrepared = false;
		bool helperCalled = false;
		bool technique65600Correction = false;
		bool technique65600MaskKnown = false;
		bool technique65600ExpectedMask = false;
	};

	struct State
	{
		bool diagnosticsEnabled = false;
		std::uint64_t attempts = 0;
		std::uint64_t applied = 0;  // historical field name: successful observation captures
		std::uint64_t declines = 0;
		std::uint64_t restores = 0;
		std::uint32_t lastDecline = 0;
		std::uint32_t exceptionCode = 0;
		std::int32_t lastHr = 0;
		float lastRatioWidth = 0.0f;
		float lastRatioHeight = 0.0f;
		std::uint32_t bufferBytes = 0;
		bool ready = false;
		std::uint64_t restoredAtDraw = 0;
		std::uint64_t restoredAtWater = 0;
		std::uint64_t restoredAtSeamExit = 0;
		std::uint64_t drawsExamined = 0;
		std::uint64_t compositeDraws = 0;
		std::uint32_t compositeShaders = 0;
		std::uint32_t captureFailures = 0;
		std::uint32_t nativeSamples = 0;
		std::uint32_t reducedSamples = 0;
		bool captureCapped = false;
		std::uint32_t routeOverflow = 0;
		std::uint64_t preparedPacketDrawRoutes = 0;
		std::uint32_t registryRecordCount = 0;
		std::uint32_t registryOverflow = 0;
		std::uint32_t unknownPreparedPacketOverflow = 0;
		std::uint64_t knownNonCubeMatches = 0;
		std::uint32_t knownNonCubeOverflow = 0;
		std::uint32_t tableSnapshotCapacity = 0;
		std::uint32_t tableSnapshotRecords = 0;
		std::uint32_t tableSnapshotFailures = 0;
		bool tableSnapshotComplete = false;
		std::uint64_t correctionCandidates = 0;
		std::uint64_t correctionAttempts = 0;
		// Helper invocations made under a verified private CB12 binding. A bounded GPU
		// mask is the only evidence that any reconstruction matrix changed.
		std::uint64_t correctedDraws = 0;
		std::uint64_t correctionPassThroughs = 0;
		std::uint64_t privateBindVerifications = 0;
		std::uint64_t originalRestoreVerifications = 0;
		std::uint64_t originalRestoreFailures = 0;
		std::uint64_t resourceBuilds = 0;
		std::uint64_t resourceReleases = 0;
		std::uint64_t resourceEpoch = 0;
		std::uint64_t protectionAttempts = 0;
		std::uint64_t protectionEnables = 0;
		std::uint64_t protectionEnters = 0;
		std::uint64_t protectionLeaves = 0;
		std::uint32_t correctionCaptureCount = 0;
		std::uint32_t lastCapturedCorrectedMask = 0;
		std::uint32_t lastCapturedSkippedMask = 0;
		bool lastCapturedMaskKnown = false;
		bool maskKnownForAllCorrectedDraws = false;
		bool completeReconstruction = false;
		bool multithreadPriorProtected = false;
		bool multithreadEnabledByModule = false;
		bool multithreadProtected = false;
		bool globalProtectionCostActive = false;
		bool releasePending = false;
		bool terminalStateUncertain = false;
		std::uint64_t technique65600DiagnosticEpoch = 0;
		std::uint64_t technique65600EligibleDraws = 0;
		std::uint64_t technique65600EligibilityDeclines = 0;
		std::uint64_t technique65600Attempts = 0;
		std::uint64_t technique65600Successes = 0;
		std::uint64_t technique65600Failures = 0;
		std::uint32_t technique65600LastFailure = 0;
		std::int32_t technique65600LastHr = 0;
		std::uint32_t technique65600ExceptionCode = 0;
		std::uint32_t technique65600CaptureCount = 0;
		bool technique65600AttemptCapped = false;
		bool technique65600CaptureCapped = false;
		bool technique65600CorrectionEnabled = false;
		std::uint64_t technique65600CorrectionEpoch = 0;
		std::uint64_t technique65600CorrectionCandidates = 0;
		std::uint64_t technique65600CorrectionAttempts = 0;
		std::uint64_t technique65600PrivateDraws = 0;
		std::uint64_t technique65600CorrectedDraws = 0;
		std::uint64_t technique65600CorrectionPassThroughs = 0;
		std::uint64_t technique65600CorrectionDeclines = 0;
		std::uint32_t technique65600CorrectionLastDecline = 0;
		std::int32_t technique65600CorrectionLastHr = 0;
		std::uint32_t technique65600CorrectionExceptionCode = 0;
		std::uint64_t technique65600PrivateBindVerifications = 0;
		std::uint64_t technique65600OriginalRestoreVerifications = 0;
		std::uint64_t technique65600OriginalRestoreFailures = 0;
		std::uint32_t technique65600DimensionReadAttempts = 0;
		std::uint32_t technique65600DimensionReadFailures = 0;
		bool technique65600DimensionCacheValid = false;
		float technique65600NativeWidth = 0.0f;
		float technique65600NativeHeight = 0.0f;
		std::uint32_t technique65600CorrectionCaptureAttempts = 0;
		std::uint32_t technique65600CorrectionCaptureCount = 0;
		bool technique65600CorrectionCaptureCapped = false;
		std::uint32_t technique65600LastCorrectedMask = 0;
		std::uint32_t technique65600LastSkippedMask = 0;
		bool technique65600LastMaskKnown = false;
		bool technique65600ExpectedPartialReconstruction = false;
		bool technique65600MaskKnownForAllPrivateDraws = false;
		std::string familyCoverageJson{ "{}" };
		std::string registryRecordsJson{ "[]" };
		std::string unknownPreparedPacketsJson{ "[]" };
		std::string knownNonCubeJson{ "[]" };
		std::string routeObservationsJson{ "[]" };
		std::string observationsJson{ "[]" };
		std::string correctionObservationsJson{ "[]" };
		std::string technique65600ObservationsJson{ "[]" };
		std::string technique65600CorrectionObservationsJson{ "[]" };
		std::string preliminaryCensusJson{ R"({"counts":{},"capacity":0,"overflow":0,"observations":[]})" };
	};

	// Any thread. Enabling starts a new correction episode; retained D3D objects from
	// an earlier episode are released later on their owner thread at the helper boundary.
	void SetEnabled(bool a_enabled) noexcept;
	// Any thread. Pauses/resumes census and bounded readbacks without resetting the
	// correction episode. Each rising edge clears and re-arms only the ID 65600 observer;
	// its separate epoch rejects publication by a draw from an earlier arm.
	void SetDiagnosticsEnabled(bool a_enabled) noexcept;
	// Any thread. Enables the audited non-cubemap family. The legacy key and exact-65600
	// telemetry remain compatible; exact 65600 retains its protected epoch and bounded
	// native-dimension read. A rising edge clears only that exact experiment's evidence.
	void SetTechnique65600CorrectionEnabled(bool a_enabled) noexcept;
	[[nodiscard]] bool Enabled() noexcept;

	// These two calls bracket the original helper in the hook. PreparedDrawToken is POD so
	// the hook can guarantee EndPreparedPacketDraw with MSVC __try/__finally.
	void BeginPreparedPacketDraw(
		std::uintptr_t a_callerReturn,
		std::uintptr_t a_packet,
		PreparedDrawToken& a_token) noexcept;
	void EndPreparedPacketDraw(PreparedDrawToken& a_token) noexcept;

	// Stage-A compatibility observer retained for archived diagnostic consumers.
	void NoteDrawRoute(std::uintptr_t a_pass, std::uintptr_t a_shaderState) noexcept;
	void ObservePreparedPacketDraw(
		std::uintptr_t a_callerReturn,
		std::uintptr_t a_packet) noexcept;

	// The exact-family table registry AddRefs its device and pixel shaders. Both calls only
	// request deferred owner-thread cleanup; the process-wide D3D multithread setting is
	// deliberately never changed back to false.
	void Release() noexcept;
	void RequestRelease() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
