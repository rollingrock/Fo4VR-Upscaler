#pragma once

// Owns the sub-pixel offset in exactly one place: it reads the engine's native TAA
// projection translation, converts it to the camera-jitter pixel convention used at the
// backend seam, and - once a reconstruction backend has armed its own sequence - writes
// that backend's sample back into the same two floats before the projection is built.
//
// The engine's own sequence is eight phases. A reconstruction ratio needs
// ceil(8*(output/render)^2) of them, which is the only defect in the native path: its
// units, signs and amplitude are already correct at every dynamic-resolution ratio.

#include <cstdint>

namespace Upscaler::Backend
{
	class IBackend;
}

namespace Upscaler::Jitter
{
	enum class Failure : std::uint32_t
	{
		kNone,
		kUnresolved,
		kReadFault,
		kTornRead,
		kDimensionMismatch,
		kDisabled,
		kInvalidValue,
		kSequenceMismatch,
	};

	// Why a backend-owned commit declined. Every value is counted, and every one of them
	// leaves the engine's own sample in place, so a decline costs sequence length rather
	// than a wrong picture.
	enum class WriteFailure : std::uint32_t
	{
		kNone,
		kUnresolved,
		kNotArmed,
		kReadFault,
		kGatesClosed,
		kDimensionMismatch,
		kInvalidValue,
		kWriteFault,
		kReadBackMismatch,
	};

	// The armed sequence is capped so the precomputed offset table stays a fixed-size
	// file-scope array and a nonsense phase count declines instead of allocating. The
	// longest count the vendor table defines is 72, for UltraPerformance.
	inline constexpr std::uint32_t kMaxPhaseCount = 256;

	struct State
	{
		bool resolved = false;
		bool backendSequenceArmed = false;
		std::uintptr_t address = 0;
		std::uint64_t readAttempts = 0;
		std::uint64_t readSuccesses = 0;
		std::uint64_t readFailures = 0;
		std::uint64_t writeAttempts = 0;
		std::uint64_t writeSuccesses = 0;
		std::uint64_t writeMismatches = 0;
		std::uint64_t writeGateDeclines = 0;
		std::uint64_t sequenceRestarts = 0;
		std::uint64_t leftSubmissions = 0;
		std::uint64_t rightSubmissions = 0;
		std::uint64_t stereoMatches = 0;
		std::uint64_t stereoMismatches = 0;
		std::uint32_t phaseCount = 8;
		std::uint32_t phase = 0;
		std::uint32_t enginePhase = 0;
		std::uint32_t distinctPhasesSeen = 0;
		// 128 bits, because UltraPerformance needs 72 and a 32-bit mask would report
		// coverage it cannot represent. Word 0 carries phases 0..63.
		std::uint64_t observedPhaseMask[2] = { 0, 0 };
		std::uint32_t engineWidth = 0;
		std::uint32_t engineHeight = 0;
		std::uint32_t enabled = 0;
		std::uint32_t disableCount = 0;
		float projectionX = 0.0f;
		float projectionY = 0.0f;
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		float committedProjectionX = 0.0f;
		float committedProjectionY = 0.0f;
		float minJitterX = 0.0f;
		float maxJitterX = 0.0f;
		float minJitterY = 0.0f;
		float maxJitterY = 0.0f;
		float leftJitterX = 0.0f;
		float leftJitterY = 0.0f;
		float rightJitterX = 0.0f;
		float rightJitterY = 0.0f;
		// The sign AcquireFrameSample applies to the value it hands the vendor: 1 or -1.
		std::int32_t vendorSign = 1;
		Failure lastFailure = Failure::kNone;
		WriteFailure lastWriteFailure = WriteFailure::kNone;
	};

	// DirectSR defines camera jitter J in source pixels through scene translation
	// (-2*Jx/W, +2*Jy/H) after accounting for D3D viewport Y inversion. FO4VR stores
	// exactly that scene translation in the projection matrix. These two pairs convert
	// between the store and DirectSR camera pixels; they are the engine-side identity
	// the native eight-phase check and the harness rely on. They are NOT the value the
	// vendor takes: NGX and Streamline want the offset applied to the projection, the
	// negation of J, and AcquireFrameSample negates at the seam (measured; see there).
	//
	// W and H are the FULL per-eye extent held at state+0x78/+0x7c - never renderWidth,
	// never W*ratio. BuildStereoCameraViewData multiplies the stored float by
	// W/floor(W*ratio) before it reaches the projection, so the dynamic-resolution ratio
	// cancels in both directions and the applied offset is +-0.5 render pixels at any
	// ratio. Pre-dividing by the ratio here would double-compensate and apply +-0.746 px
	// at 0.67 - a picture that looks right and samples wrong.
	[[nodiscard]] constexpr float ProjectionToCameraPixelsX(float a_projectionX, std::uint32_t a_perEyeWidth) noexcept
	{
		return -a_projectionX * static_cast<float>(a_perEyeWidth) * 0.5f;
	}

	[[nodiscard]] constexpr float ProjectionToCameraPixelsY(float a_projectionY, std::uint32_t a_perEyeHeight) noexcept
	{
		return a_projectionY * static_cast<float>(a_perEyeHeight) * 0.5f;
	}

	// The exact inverse of the pair above, by construction:
	// ProjectionToCameraPixelsX(CameraPixelsToProjectionX(J, W), W) == J.
	[[nodiscard]] constexpr float CameraPixelsToProjectionX(float a_jitterPixelsX, std::uint32_t a_perEyeWidth) noexcept
	{
		return -2.0f * a_jitterPixelsX / static_cast<float>(a_perEyeWidth);
	}

	[[nodiscard]] constexpr float CameraPixelsToProjectionY(float a_jitterPixelsY, std::uint32_t a_perEyeHeight) noexcept
	{
		return 2.0f * a_jitterPixelsY / static_cast<float>(a_perEyeHeight);
	}

	// Resolves one verified LEA anchor. Pure code-byte reads; safe at plugin load.
	[[nodiscard]] bool Init();

	// Publishes the sign AcquireFrameSample applies on top of the vendor convention.
	// 1 hands the vendor the negated camera sample, the offset applied to the projection
	// that NGX asks for and the convention the measurement of 2026-09-04 confirmed; -1
	// hands it the DirectSR camera sample itself, the convention this project used until
	// then, kept so the grid it produces can be reproduced on demand. Nothing else reads
	// it: the engine commit and the engine-side telemetry describe the camera sample.
	// Any value other than 1 or -1 is refused, logged once, and left at 1. Any thread;
	// the settings transaction is the only caller.
	void ConfigureVendorSign(std::int64_t a_sign) noexcept;

	// Reads one stable native sample, verifies dimensions and the enable gates, then
	// returns render-resolution camera-jitter pixels. While a backend sequence is armed
	// the sample must be bit-exactly what CommitFrame wrote this frame; while it is not,
	// it must match the engine's own eight-phase Halton result.
	//
	// The expected extent is the FULL per-eye output extent, not renderWidth: the state
	// holds the full extent and the ratio cancels in the conversion.
	[[nodiscard]] bool AcquireFrameSample(
		std::uint32_t a_expectedWidth,
		std::uint32_t a_expectedHeight,
		float& a_outX,
		float& a_outY) noexcept;

	// Arms the selected backend's own sequence. Render-thread only, immediately after
	// CreateViews succeeds and never per frame: it asks the backend once for its phase
	// count and precomputes the whole offset table, so the render hook never reaches
	// across the backend seam. A null backend, or a count of 0 or 1, or a count above
	// kMaxPhaseCount, all leave this module in read-only native mode.
	// The output extent is latched here and re-checked on every commit against
	// state+0x78/+0x7c, so a resolution change that misses a teardown declines rather
	// than writing a sample scaled for the previous extent.
	void ArmBackendSequence(
		const Backend::IBackend* a_backend,
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight) noexcept;

	void DisarmBackendSequence() noexcept;

	[[nodiscard]] bool BackendSequenceArmed() noexcept;

	// Restarts the armed sequence at phase 0 on the NEXT commit. The reset frame's own
	// jitter was committed before Evaluate decided to reset, and no vendor requires
	// phase 0 at a history reset, so restart-on-next is the correct semantic rather than
	// a concession. Safe from any thread.
	void RequestSequenceRestart() noexcept;

	// Render-thread only, from the main-world jitter call site and only after the
	// original has run. Overwrites the two stored floats with the armed backend's sample
	// for this frame and verifies the write bit-exactly. False means the engine's own
	// sample stands, which is always a safe outcome.
	[[nodiscard]] bool CommitFrame(float& a_outX, float& a_outY) noexcept;

	// The length of the sequence actually in use - the armed backend count, or the
	// native eight while nothing is armed.
	[[nodiscard]] std::uint32_t PhaseCount() noexcept;

	// The native counter's period. This is what bounds state+0x00, so a capture request
	// that selects an engine phase validates against this rather than PhaseCount: the
	// engine keeps advancing modulo 8 whatever sequence we commit into the floats.
	[[nodiscard]] std::uint32_t NativePhaseCount() noexcept;

	// One guarded read of the currently committed native phase. This is the
	// read-only capture-instrument seam: it does not count as a backend jitter
	// submission and does not change the engine or the evaluation telemetry.
	[[nodiscard]] bool ReadCurrentPhase(std::uint32_t& a_outPhase) noexcept;

	// The two projection floats and their gates exactly as the engine holds them right
	// now: no conversion, no bounds test, no telemetry.
	//
	// AcquireFrameSample answers a different question - "is this a sample the vendor may
	// be handed" - and to answer it it counts an attempt, latches a failure and rejects
	// anything outside +-0.5 camera pixels. An instrument that asks what the floats read
	// at some other point in the frame must do none of that: a zero, a stale value or a
	// value the vendor would refuse is exactly the answer it is looking for, and counting
	// its reads beside the per-frame ones would corrupt the jitter telemetry. Hence a
	// separate reader rather than a flag on that one. Any thread.
	struct RawState
	{
		bool resolved = false;
		bool read = false;  // every guarded read succeeded
		bool torn = false;  // the phase changed across the block
		std::uintptr_t address = 0;
		std::uint32_t phase = 0;
		std::uint32_t projectionXBits = 0;
		std::uint32_t projectionYBits = 0;
		std::uint32_t perEyeWidth = 0;
		std::uint32_t perEyeHeight = 0;
		std::uint32_t enabled = 0;
		std::uint32_t disableCount = 0;
	};

	[[nodiscard]] RawState ReadRawState() noexcept;

	// Called immediately before each backend submission. This does not change jitter;
	// it proves in live telemetry that both eye contexts received one identical sample.
	void RecordSubmission(std::uint32_t a_eye, float a_x, float a_y) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
