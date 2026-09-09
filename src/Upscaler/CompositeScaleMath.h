#pragma once

// The composite reconstruction scale's arithmetic, with no engine or D3D11 dependency, so
// it can be checked on its own. CompositeScale.cpp is the only caller that also binds
// anything; SelfTest below is the check, and it runs once per session at install.
//
// The composite's pixel shader computes the view position as four dp4 instructions of
// (clip.x, clip.y, depth, 1) against four consecutive rows of the global camera constant
// buffer. Those rows are transpose(XMMatrixInverse(ViewData field)), which is what
// US_BSGraphics_UploadGlobalCameraConstants writes - the transpose is why each stored row
// is the row the dp4 uses rather than a column of the DirectXMath matrix.
//
// Under a reduced raster of ratio r the shader's clip is c = uv*2-1 over uv in [0, r), so
// it is short of the correct clip by exactly c' = (c + 1)/r - 1. That is affine, so the
// whole correction is one post-multiplication by T and the shader is untouched.

#include <DirectXMath.h>

#include <cmath>

namespace Upscaler::CompositeScale
{
	// T, acting on the column vector (clip.x, clip.y, depth, 1). Ratio one is the identity.
	[[nodiscard]] inline DirectX::XMMATRIX ReconstructionScale(
		float a_ratioWidth,
		float a_ratioHeight) noexcept
	{
		const float sx = 1.0f / a_ratioWidth;
		const float sy = 1.0f / a_ratioHeight;
		return DirectX::XMMATRIX(
			sx, 0.0f, 0.0f, sx - 1.0f,
			0.0f, sy, 0.0f, sy - 1.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f);
	}

	// The eight rows as the engine's uploader writes them, from the ViewData field it reads.
	[[nodiscard]] inline DirectX::XMMATRIX EngineRows(const DirectX::XMFLOAT4X4& a_viewData) noexcept
	{
		return DirectX::XMMatrixTranspose(
			DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&a_viewData)));
	}

	// row' . p = row . (T p) for every row, which as a matrix identity is A' = A T.
	[[nodiscard]] inline DirectX::XMMATRIX ScaleRows(
		DirectX::FXMMATRIX a_rows,
		float a_ratioWidth,
		float a_ratioHeight) noexcept
	{
		return DirectX::XMMatrixMultiply(a_rows, ReconstructionScale(a_ratioWidth, a_ratioHeight));
	}

	// One synthetic matrix, the two Quality-ish ratios, and four probe points including the
	// two the shader actually reaches at the packed rectangle's corners. It asserts the
	// identity the whole change rests on: what the scaled rows produce at the reduced
	// clip position is what the engine's own rows produce at the full-raster one.
	[[nodiscard]] inline bool SelfTest() noexcept
	{
		using namespace DirectX;
		// Deliberately not a projection - an arbitrary invertible matrix is a stronger
		// test of the algebra than one whose zeros hide a transposed index.
		const XMFLOAT4X4 synthetic{
			0.9f, 0.1f, -0.2f, 0.3f,
			-0.4f, 1.2f, 0.05f, -0.6f,
			0.15f, -0.25f, 0.8f, 0.45f,
			0.02f, 0.03f, -0.7f, 1.1f
		};
		constexpr float ratioWidth = 0.6667f;
		constexpr float ratioHeight = 0.7301f;
		const XMMATRIX rows = EngineRows(synthetic);
		const XMMATRIX scaled = ScaleRows(rows, ratioWidth, ratioHeight);

		// Clip positions the reduced raster produces, including both corners of the packed
		// rectangle: uv = 0 gives -1, uv = r gives 2r-1.
		const float reduced[4][2] = {
			{ -1.0f, -1.0f },
			{ 2.0f * ratioWidth - 1.0f, 2.0f * ratioHeight - 1.0f },
			{ 0.0f, 0.0f },
			{ 0.25f, -0.5f }
		};
		for (const auto& point : reduced) {
			const float depth = 0.375f;
			// What the shader will compute with the scaled rows at the reduced clip.
			const XMVECTOR reducedClip = XMVectorSet(point[0], point[1], depth, 1.0f);
			const XMVECTOR got = XMVector4Transform(reducedClip, XMMatrixTranspose(scaled));
			// What the engine's own rows produce at the clip the full raster would have
			// given for the same pixel: c' = (c + 1)/r - 1.
			const XMVECTOR fullClip = XMVectorSet(
				(point[0] + 1.0f) / ratioWidth - 1.0f,
				(point[1] + 1.0f) / ratioHeight - 1.0f,
				depth,
				1.0f);
			const XMVECTOR want = XMVector4Transform(fullClip, XMMatrixTranspose(rows));
			XMFLOAT4 gotValue{};
			XMFLOAT4 wantValue{};
			XMStoreFloat4(&gotValue, got);
			XMStoreFloat4(&wantValue, want);
			// Single precision through an inverse and two multiplies; the components here
			// are order 1, so this is a few units in the last place and not a fudge.
			constexpr float tolerance = 1e-4f;
			if (std::fabs(gotValue.x - wantValue.x) > tolerance ||
				std::fabs(gotValue.y - wantValue.y) > tolerance ||
				std::fabs(gotValue.z - wantValue.z) > tolerance ||
				std::fabs(gotValue.w - wantValue.w) > tolerance) {
				return false;
			}
		}

		// Ratio one has to be the identity, or a native frame that reached the pass at all
		// would move.
		const XMMATRIX unity = ScaleRows(rows, 1.0f, 1.0f);
		XMFLOAT4X4 unityValue{};
		XMFLOAT4X4 rowsValue{};
		XMStoreFloat4x4(&unityValue, unity);
		XMStoreFloat4x4(&rowsValue, rows);
		for (int row = 0; row < 4; ++row) {
			for (int column = 0; column < 4; ++column) {
				if (std::fabs(unityValue.m[row][column] - rowsValue.m[row][column]) > 1e-5f) {
					return false;
				}
			}
		}
		return true;
	}
}
