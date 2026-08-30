#pragma once

// Owns all double-wide geometry: the only place a per-eye rectangle is computed.
// FO4VR 1.2.72's shipping deferred-prepass bytecode and compositor submit bounds prove
// that the game is left-first. The explicit reverse-order path is retained as a recovery
// override, never as an alternate guess.

#include <cstdint>
#include <string_view>

#include "Upscaler/Backend/BackendTypes.h"

namespace Upscaler::StereoLayout
{
	// The eye is always named. Never a bare int, never a bool isRight.
	enum class Eye
	{
		kLeft = 0,
		kRight = 1
	};

	enum class Order
	{
		kLeftRight,
		kRightLeft
	};

	// Pure geometry shared with the standalone harness. Invalid dimensions and invalid
	// enum values fail closed to a zero rect; an odd double-wide width cannot be split
	// into two equal eyes without silently dropping a column.
	[[nodiscard]] constexpr Backend::Rect RectFor(
		Eye a_eye,
		Order a_order,
		std::uint32_t a_width,
		std::uint32_t a_height) noexcept
	{
		if (a_width < 2 || (a_width & 1u) != 0 || a_height == 0) {
			return {};
		}
		if (a_eye != Eye::kLeft && a_eye != Eye::kRight) {
			return {};
		}
		if (a_order != Order::kLeftRight && a_order != Order::kRightLeft) {
			return {};
		}

		const std::uint32_t eyeWidth = a_width / 2u;
		const bool leftHalf =
			(a_eye == Eye::kLeft && a_order == Order::kLeftRight) ||
			(a_eye == Eye::kRight && a_order == Order::kRightLeft);
		return Backend::Rect{ leftHalf ? 0u : eyeWidth, 0u, eyeWidth, a_height };
	}

	[[nodiscard]] bool Resolve();

	[[nodiscard]] bool Resolved() noexcept;

	// Stable storage, suitable for the self-check and logs.
	[[nodiscard]] std::string_view Summary() noexcept;

	// Zeroed while unresolved or for invalid dimensions. Both eyes come through here
	// indexed, never as two hand-written blocks - divergent eye paths are how one eye
	// ends up a frame stale, and a stale eye in VR is a nausea bug rather than a rendering
	// bug.
	[[nodiscard]] Backend::Rect EyeRect(Eye a_eye, std::uint32_t a_width, std::uint32_t a_height) noexcept;
}
