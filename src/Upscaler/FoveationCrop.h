#pragma once

// The foveated measurement build's crop geometry: how large a per-eye crop DLAA
// reconstructs at a given scale, where it sits, and the fixed path the moving-crop probe
// takes. Engine-free and pure, so the hardware harness and the stage layer share one
// spelling of it and a harness assertion about it is an assertion about what ships.
//
// The extent is aligned down to a multiple of 8 and every base is even. The vendor guide
// names no alignment rule for sub-rectangles (as recorded from rev 310.6.0); the
// alignment is a precaution, and it costs at most seven pixels per axis.

#include "Upscaler/Backend/BackendTypes.h"

#include <cstdint>

namespace Upscaler::Foveation
{
	// Below this a crop is refused rather than measured: the evidence asked for runs from
	// one half to the whole eye, and a sliver says nothing about either.
	inline constexpr float kMinimumScale = 0.25f;
	inline constexpr std::uint32_t kExtentAlignment = 8;

	// The moving probe. Horizontal and vertical triangle waves at different steps, so
	// the base visits the whole span in both axes, plus one jump to the mirrored base
	// every kJumpPeriod frames - about once a second at 90 Hz - so a large single-frame
	// change is exercised as well as a small steady one. Both steps are even, so every
	// base stays even.
	inline constexpr std::uint32_t kMoveStepX = 4;
	inline constexpr std::uint32_t kMoveStepY = 2;
	inline constexpr std::uint64_t kJumpPeriod = 90;
	inline constexpr std::uint64_t kJumpPhase = 45;

	struct Crop
	{
		// Zero is no crop: the whole eye is reconstructed, exactly as without this file.
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		// The base ranges over [0, spanX] x [0, spanY] inside the eye.
		std::uint32_t spanX = 0;
		std::uint32_t spanY = 0;
	};

	[[nodiscard]] constexpr bool Active(const Crop& a_crop) noexcept
	{
		return a_crop.width != 0 && a_crop.height != 0;
	}

	// The crop for an eye at a per-axis scale. A scale that is not below one, is below
	// kMinimumScale or is not a number gives no crop; so does one that rounds to the whole
	// eye or to nothing.
	[[nodiscard]] constexpr Crop Size(std::uint32_t a_eyeWidth, std::uint32_t a_eyeHeight, float a_scale) noexcept
	{
		if (!(a_scale >= kMinimumScale && a_scale < 1.0f) || a_eyeWidth == 0 || a_eyeHeight == 0) {
			return {};
		}
		const auto aligned = [](std::uint32_t a_extent, float a_by) noexcept {
			const auto scaled = static_cast<std::uint32_t>(static_cast<double>(a_extent) * static_cast<double>(a_by));
			return scaled & ~(kExtentAlignment - 1u);
		};
		Crop crop{};
		crop.width = aligned(a_eyeWidth, a_scale);
		crop.height = aligned(a_eyeHeight, a_scale);
		if (crop.width == 0 || crop.height == 0 || (crop.width == a_eyeWidth && crop.height == a_eyeHeight)) {
			return {};
		}
		crop.spanX = (a_eyeWidth - crop.width) & ~1u;
		crop.spanY = (a_eyeHeight - crop.height) & ~1u;
		return crop;
	}

	// The crop for views already created at a given extent, with the spans Size would have
	// given it - what the stage layer holds, since its config carries the extent rather than
	// the scale. An extent that is empty, or no smaller than the eye, gives no crop.
	[[nodiscard]] constexpr Crop FromExtent(
		std::uint32_t a_eyeWidth,
		std::uint32_t a_eyeHeight,
		std::uint32_t a_cropWidth,
		std::uint32_t a_cropHeight) noexcept
	{
		if (a_cropWidth == 0 || a_cropHeight == 0 || a_cropWidth > a_eyeWidth || a_cropHeight > a_eyeHeight ||
			(a_cropWidth == a_eyeWidth && a_cropHeight == a_eyeHeight)) {
			return {};
		}
		return { a_cropWidth, a_cropHeight, (a_eyeWidth - a_cropWidth) & ~1u, (a_eyeHeight - a_cropHeight) & ~1u };
	}

	// One axis of the moving path: a triangle wave over [0, a_span] at a_step per frame.
	[[nodiscard]] constexpr std::uint32_t Triangle(std::uint64_t a_frame, std::uint32_t a_step, std::uint32_t a_span) noexcept
	{
		if (a_span == 0) {
			return 0;
		}
		const std::uint64_t period = 2ull * a_span;
		const std::uint64_t phase = (a_frame * a_step) % period;
		return static_cast<std::uint32_t>(phase <= a_span ? phase : period - phase);
	}

	// The crop's rect in eye-local pixels for frame a_frame: centred, or on the moving
	// path. A caller adds the eye's own base inside a double-wide resource. Both eyes of
	// a frame take the same rect, so the crop is stereo-symmetric. No crop gives an empty
	// rect, which every backend rejects.
	[[nodiscard]] constexpr Backend::Rect Place(const Crop& a_crop, bool a_moving, std::uint64_t a_frame) noexcept
	{
		if (!Active(a_crop)) {
			return {};
		}
		if (!a_moving) {
			return { (a_crop.spanX / 2u) & ~1u, (a_crop.spanY / 2u) & ~1u, a_crop.width, a_crop.height };
		}
		std::uint32_t x = Triangle(a_frame, kMoveStepX, a_crop.spanX);
		std::uint32_t y = Triangle(a_frame, kMoveStepY, a_crop.spanY);
		if (a_frame % kJumpPeriod == kJumpPhase) {
			x = a_crop.spanX - x;
			y = a_crop.spanY - y;
		}
		return { x, y, a_crop.width, a_crop.height };
	}
}
