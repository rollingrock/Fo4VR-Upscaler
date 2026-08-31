#pragma once

// Guarded access to FO4VR's active stereo ViewData staging records. The engine's own
// global-camera uploader reads these same matrices into deferred-shader CB12, making
// this a CPU snapshot of the exact camera inputs used to produce logical RT35.

#include "Upscaler/MotionReprojection.h"

#include <cstdint>

namespace Upscaler::CameraMotion
{
	enum class Failure : std::uint32_t
	{
		kNone,
		kInvalidEye,
		kStateUnavailable,
		kStagingUnavailable,
		kReadFault,
		kStagingChanged,
		kInvalidMatrix,
	};

	struct State
	{
		std::uint64_t attempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t failures = 0;
		std::uintptr_t staging = 0;
		Failure lastFailure = Failure::kNone;
	};

	[[nodiscard]] bool SnapshotEye(
		std::uint32_t a_eye,
		MotionReprojection::Data& a_out) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
