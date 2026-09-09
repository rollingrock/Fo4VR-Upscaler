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
	};

	struct PreparedDrawToken
	{
		ID3D11DeviceContext1* context1 = nullptr;  // getter-owned until EndPreparedPacketDraw
		ID3D11Buffer* originalCamera = nullptr;     // getter-owned until EndPreparedPacketDraw
		ID3D11Multithread* multithread = nullptr;   // owned token ref until Leave
		std::uint32_t firstConstant = 0;
		std::uint32_t numConstants = 0;
		std::uint32_t captureIndex = 0xFFFFFFFFu;
		std::uint64_t generation = 0;
		bool entered = false;
		bool callOriginal = true;
		bool privateBound = false;
		bool correctionPrepared = false;
		bool helperCalled = false;
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
		std::string registryRecordsJson{ "[]" };
		std::string unknownPreparedPacketsJson{ "[]" };
		std::string knownNonCubeJson{ "[]" };
		std::string routeObservationsJson{ "[]" };
		std::string observationsJson{ "[]" };
		std::string correctionObservationsJson{ "[]" };
		std::string preliminaryCensusJson{ R"({"counts":{},"capacity":0,"overflow":0,"observations":[]})" };
	};

	// Any thread. Enabling starts a new correction episode; retained D3D objects from
	// an earlier episode are released later on their owner thread at the helper boundary.
	void SetEnabled(bool a_enabled) noexcept;
	// Any thread. Pauses/resumes census and bounded readbacks without resetting the
	// correction episode. A draw already in flight retains its diagnostic snapshot.
	void SetDiagnosticsEnabled(bool a_enabled) noexcept;
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
