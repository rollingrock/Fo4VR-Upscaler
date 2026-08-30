#pragma once

// Reads the engine's native TAA projection offset and converts it to the camera-jitter
// pixel convention used at the backend seam, in exactly one place.

#include <cstdint>

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

	struct State
	{
		bool resolved = false;
		std::uintptr_t address = 0;
		std::uint64_t readAttempts = 0;
		std::uint64_t readSuccesses = 0;
		std::uint64_t readFailures = 0;
		std::uint64_t leftSubmissions = 0;
		std::uint64_t rightSubmissions = 0;
		std::uint64_t stereoMatches = 0;
		std::uint64_t stereoMismatches = 0;
		std::uint32_t phaseCount = 8;
		std::uint32_t phase = 0;
		std::uint32_t observedPhaseMask = 0;
		std::uint32_t engineWidth = 0;
		std::uint32_t engineHeight = 0;
		std::uint32_t enabled = 0;
		std::uint32_t disableCount = 0;
		float projectionX = 0.0f;
		float projectionY = 0.0f;
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		float leftJitterX = 0.0f;
		float leftJitterY = 0.0f;
		float rightJitterX = 0.0f;
		float rightJitterY = 0.0f;
		Failure lastFailure = Failure::kNone;
	};

	// DirectSR defines camera jitter J in source pixels through scene translation
	// (-2*Jx/W, +2*Jy/H) after accounting for D3D viewport Y inversion. FO4VR stores
	// exactly that scene translation in the projection matrix.
	[[nodiscard]] constexpr float ProjectionToCameraPixelsX(float a_projectionX, std::uint32_t a_perEyeWidth) noexcept
	{
		return -a_projectionX * static_cast<float>(a_perEyeWidth) * 0.5f;
	}

	[[nodiscard]] constexpr float ProjectionToCameraPixelsY(float a_projectionY, std::uint32_t a_perEyeHeight) noexcept
	{
		return a_projectionY * static_cast<float>(a_perEyeHeight) * 0.5f;
	}

	// Resolves one verified LEA anchor. Pure code-byte reads; safe at plugin load.
	[[nodiscard]] bool Init();

	// Reads one stable native sample, verifies dimensions, enable gates and the exact
	// eight-phase Halton result, then returns render-resolution camera-jitter pixels.
	[[nodiscard]] bool ReadEngine(
		std::uint32_t a_expectedWidth,
		std::uint32_t a_expectedHeight,
		float& a_outX,
		float& a_outY) noexcept;

	// Always false on this build. Reconstruction needs the selected backend's sequence;
	// overwriting the native eight-phase sequence is a later, separately gated milestone.
	[[nodiscard]] bool WriteEngine(float a_x, float a_y) noexcept;

	// The native FO4VR TAA sequence length, proven by the generator's AND 7.
	[[nodiscard]] std::uint32_t PhaseCount() noexcept;

	// Called immediately before each backend submission. This does not change jitter;
	// it proves in live telemetry that both eye contexts received one identical sample.
	void RecordSubmission(std::uint32_t a_eye, float a_x, float a_y) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
