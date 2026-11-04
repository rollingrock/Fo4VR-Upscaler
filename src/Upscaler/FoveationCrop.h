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

	// Phase 2a. Where an eye's view axis lands, in pixels of its a_eyeWidth x a_eyeHeight
	// image, from the two off-centre terms of its row-vector D3D projection: a_m20 and a_m21,
	// elements [8] and [9] of the row-major matrix. FO4VR builds each eye's projection as a
	// textbook off-centre perspective from IVRSystem::GetProjectionRaw, [8] = -(l+r)/(r-l)
	// and [9] = -(b+t)/(t-b) ([GHIDRA] FUN_141da8e60), so the axis point (0, 0, z)
	// lands at NDC ([8], [9]); NDC y is up and pixel y is down. HMD frusta are wider on the
	// temporal side, so the centre sits nasal of the image centre. A term outside (-1, 1),
	// or not a number, has no centre on the image and gives none.
	[[nodiscard]] constexpr bool ProjectionCentre(
		float a_m20,
		float a_m21,
		std::uint32_t a_eyeWidth,
		std::uint32_t a_eyeHeight,
		float& a_x,
		float& a_y) noexcept
	{
		// NaN fails both comparisons.
		if (!(a_m20 > -1.0f && a_m20 < 1.0f) || !(a_m21 > -1.0f && a_m21 < 1.0f) || a_eyeWidth == 0 || a_eyeHeight == 0) {
			return false;
		}
		a_x = (a_m20 + 1.0f) * 0.5f * static_cast<float>(a_eyeWidth);
		a_y = (1.0f - a_m21) * 0.5f * static_cast<float>(a_eyeHeight);
		return true;
	}

	// Phase 2a. The crop centred on a point of the eye (ProjectionCentre's), clamped so it
	// stays inside, with an even base like every other placement.
	[[nodiscard]] constexpr Backend::Rect PlaceAt(const Crop& a_crop, float a_centreX, float a_centreY) noexcept
	{
		if (!Active(a_crop)) {
			return {};
		}
		const auto axis = [](float a_centre, std::uint32_t a_extent, std::uint32_t a_span) noexcept {
			const float base = a_centre - static_cast<float>(a_extent) * 0.5f;
			if (!(base > 0.0f)) {
				return 0u;
			}
			const auto rounded = static_cast<std::uint32_t>(base + 0.5f);
			return (rounded < a_span ? rounded : a_span) & ~1u;
		};
		return { axis(a_centreX, a_crop.width, a_crop.spanX), axis(a_centreY, a_crop.height, a_crop.spanY), a_crop.width, a_crop.height };
	}

	// Phase 2a. One eye's placement across frames: the projection centre it is latched on,
	// and whether a cropped frame has already been placed without one - on the image centre,
	// because the projection gave no centre yet (CameraMotion::SnapshotEye failed).
	struct EyePlacement
	{
		bool latched = false;
		bool placedUnlatched = false;
		float x = 0.0f;
		float y = 0.0f;
	};

	struct PlacementStep
	{
		EyePlacement next{};
		// A latch happened this frame (logged), and whether it moved a box the history has
		// already seen - a re-placement, which resets both eyes' histories. from is where the
		// box's centre was: the old latch, or the image centre.
		bool latchedNow = false;
		bool replaced = false;
		float fromX = 0.0f;
		float fromY = 0.0f;
	};

	// One cropped frame of one eye. a_haveCentre and (a_x, a_y) are this frame's projection
	// centre (ProjectionCentre). The first centre latches; a latched centre moves only past
	// a_thresholdPx on either axis, since the projection's same two terms carry the frame's
	// sub-pixel jitter. A move is a re-placement. So is the first latch after frames already
	// placed on the image centre, when it lands the box elsewhere: those frames' history is of
	// the centred box. The first latch of a crop with no frame before it is not - the crop's
	// own creation resets the histories.
	[[nodiscard]] constexpr PlacementStep ObserveCentre(
		EyePlacement a_eye,
		const Crop& a_crop,
		bool a_haveCentre,
		float a_x,
		float a_y,
		float a_thresholdPx) noexcept
	{
		PlacementStep out{};
		out.next = a_eye;
		if (a_haveCentre) {
			const auto beyond = [a_thresholdPx](float a_d) noexcept { return a_d > a_thresholdPx || a_d < -a_thresholdPx; };
			const bool moved = a_eye.latched && (beyond(a_x - a_eye.x) || beyond(a_y - a_eye.y));
			if (!a_eye.latched || moved) {
				const Backend::Rect centred = { (a_crop.spanX / 2u) & ~1u, (a_crop.spanY / 2u) & ~1u, a_crop.width, a_crop.height };
				const Backend::Rect placed = PlaceAt(a_crop, a_x, a_y);
				const bool jumped = !a_eye.latched && a_eye.placedUnlatched && (placed.x != centred.x || placed.y != centred.y);
				out.latchedNow = true;
				out.replaced = moved || jumped;
				out.fromX = a_eye.latched ? a_eye.x : static_cast<float>(centred.x) + static_cast<float>(a_crop.width) * 0.5f;
				out.fromY = a_eye.latched ? a_eye.y : static_cast<float>(centred.y) + static_cast<float>(a_crop.height) * 0.5f;
				out.next.latched = true;
				out.next.x = a_x;
				out.next.y = a_y;
			}
		}
		if (!out.next.latched) {
			out.next.placedUnlatched = true;
		}
		return out;
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
