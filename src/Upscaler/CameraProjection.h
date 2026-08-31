#pragma once

// Engine-independent extraction of the camera scalars temporal reconstruction APIs
// require. FO4VR stores row-vector D3D perspective matrices in BSGraphics::ViewData;
// keeping the algebra here makes the executable-facing read small and gives the
// headless harness exact coverage of both standard and inverted depth.

namespace Upscaler::CameraProjection
{
	struct Data
	{
		float nearPlane = 0.0f;
		float farPlane = 0.0f;
		float verticalFovRadians = 0.0f;
		bool valid = false;
	};

	// a_projection is a row-major D3D perspective matrix in the memory layout used by
	// DirectXMath. Off-centre vertical frusta are accepted; infinite-far projections
	// are deliberately rejected until the neutral backend config carries that flag.
	// Failure leaves a_out invalid and zeroed.
	[[nodiscard]] bool Build(
		const float (&a_projection)[16],
		bool a_invertedDepth,
		Data& a_out) noexcept;
}
