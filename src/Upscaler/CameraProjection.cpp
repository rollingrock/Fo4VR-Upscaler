#include "Upscaler/CameraProjection.h"

#include <cstddef>
#include <cmath>
#include <numbers>

namespace Upscaler::CameraProjection
{
	namespace
	{
		constexpr float kMatrixEpsilon = 1.0e-5f;

		[[nodiscard]] bool Finite(const float* a_values, std::size_t a_count) noexcept
		{
			for (std::size_t i = 0; i < a_count; ++i) {
				if (!std::isfinite(a_values[i])) {
					return false;
				}
			}
			return true;
		}
	}

	bool Build(
		const float (&a_projection)[16],
		bool a_invertedDepth,
		Data& a_out) noexcept
	{
		a_out = {};
		if (!Finite(a_projection, 16)) {
			return false;
		}

		// Row-vector D3D perspective shape. These two terms distinguish the intended
		// matrix from a view, view-projection, orthographic, or transposed matrix.
		if (std::abs(a_projection[11] - 1.0f) > kMatrixEpsilon ||
			std::abs(a_projection[15]) > kMatrixEpsilon) {
			return false;
		}

		const float yScale = a_projection[5];
		const float yOffset = a_projection[9];
		if (!(yScale > 0.0f)) {
			return false;
		}
		const float topTangent = (1.0f - yOffset) / yScale;
		const float bottomTangent = (1.0f + yOffset) / yScale;
		if (!(topTangent > 0.0f) || !(bottomTangent > 0.0f)) {
			return false;
		}
		const float verticalFov = std::atan(topTangent) + std::atan(bottomTangent);
		if (!(verticalFov > 0.0f) || !(verticalFov < std::numbers::pi_v<float>)) {
			return false;
		}

		const float depthA = a_projection[10];
		const float depthB = a_projection[14];
		float nearPlane = 0.0f;
		float farPlane = 0.0f;
		if (a_invertedDepth) {
			// z_ndc = A + B/z, with near -> 1 and far -> 0.
			if (std::abs(depthA) <= kMatrixEpsilon ||
				std::abs(1.0f - depthA) <= kMatrixEpsilon) {
				return false;
			}
			nearPlane = depthB / (1.0f - depthA);
			farPlane = -depthB / depthA;
		} else {
			// z_ndc = A + B/z, with near -> 0 and far -> 1.
			if (std::abs(depthA) <= kMatrixEpsilon ||
				std::abs(depthA - 1.0f) <= kMatrixEpsilon) {
				return false;
			}
			nearPlane = -depthB / depthA;
			farPlane = -depthB / (depthA - 1.0f);
		}
		if (!std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
			!(nearPlane > 0.0f) || !(farPlane > nearPlane)) {
			return false;
		}

		a_out.nearPlane = nearPlane;
		a_out.farPlane = farPlane;
		a_out.verticalFovRadians = verticalFov;
		a_out.valid = true;
		return true;
	}
}
