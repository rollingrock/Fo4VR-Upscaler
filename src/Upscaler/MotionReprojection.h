#pragma once

// Engine-independent camera reprojection math shared by the plugin and the standalone
// D3D11 harness. FO4VR and DirectXMath both use row-vector matrices here: a current
// clip-space position reaches previous clip space through inverse(currentVP) * previousVP.

#include <cstdint>

namespace Upscaler::MotionReprojection
{
	struct Data
	{
		float currentClipToPreviousClip[16]{};
		bool valid = false;
	};

	// Both inputs are row-major 4x4 matrices in the exact memory layout consumed by
	// DirectXMath. Failure leaves a_out invalid and zeroed.
	[[nodiscard]] bool Build(
		const float (&a_currentViewProjection)[16],
		const float (&a_previousViewProjection)[16],
		Data& a_out) noexcept;
}
