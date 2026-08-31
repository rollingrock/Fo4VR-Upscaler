#include "Upscaler/MotionReprojection.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstring>

namespace Upscaler::MotionReprojection
{
	namespace
	{
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
		const float (&a_currentViewProjection)[16],
		const float (&a_previousViewProjection)[16],
		Data& a_out) noexcept
	{
		a_out = {};
		if (!Finite(a_currentViewProjection, 16) || !Finite(a_previousViewProjection, 16)) {
			return false;
		}

		using namespace DirectX;
		XMFLOAT4X4 current{};
		XMFLOAT4X4 previous{};
		std::memcpy(&current, a_currentViewProjection, sizeof(current));
		std::memcpy(&previous, a_previousViewProjection, sizeof(previous));

		XMVECTOR determinant{};
		const XMMATRIX inverseCurrent = XMMatrixInverse(&determinant, XMLoadFloat4x4(&current));
		const float determinantX = XMVectorGetX(determinant);
		if (!std::isfinite(determinantX) || std::abs(determinantX) <= 1.0e-12f ||
			XMMatrixIsNaN(inverseCurrent) || XMMatrixIsInfinite(inverseCurrent)) {
			return false;
		}

		const XMMATRIX clipToPrevious = XMMatrixMultiply(inverseCurrent, XMLoadFloat4x4(&previous));
		if (XMMatrixIsNaN(clipToPrevious) || XMMatrixIsInfinite(clipToPrevious)) {
			return false;
		}

		XMFLOAT4X4 result{};
		XMStoreFloat4x4(&result, clipToPrevious);
		std::memcpy(a_out.currentClipToPreviousClip, &result, sizeof(result));
		if (!Finite(a_out.currentClipToPreviousClip, 16)) {
			a_out = {};
			return false;
		}
		a_out.valid = true;
		return true;
	}
}
