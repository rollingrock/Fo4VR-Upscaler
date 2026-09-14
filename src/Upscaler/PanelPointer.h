#pragma once

#include <algorithm>
#include <cmath>

namespace Upscaler::PanelPointer
{
	struct Vec3 { float x, y, z; };
	struct Quat { float x, y, z, w; };
	struct Pose { Quat orientation; Vec3 position; };
	struct Hit { bool valid = false; bool hover = false; float x = 0; float y = 0; };

	[[nodiscard]] inline Vec3 Rotate(Quat a_q, Vec3 a_v) noexcept
	{
		const Vec3 t{ 2 * (a_q.y * a_v.z - a_q.z * a_v.y),
			2 * (a_q.z * a_v.x - a_q.x * a_v.z), 2 * (a_q.x * a_v.y - a_q.y * a_v.x) };
		return { a_v.x + a_q.w * t.x + a_q.y * t.z - a_q.z * t.y,
			a_v.y + a_q.w * t.y + a_q.z * t.x - a_q.x * t.z,
			a_v.z + a_q.w * t.z + a_q.x * t.y - a_q.y * t.x };
	}

	[[nodiscard]] inline Pose Place(Pose a_head, float a_distance) noexcept
	{
		auto forward = Rotate(a_head.orientation, { 0, 0, -1 });
		const float length = std::hypot(forward.x, forward.z);
		if (length < 0.001f) {
			forward = { 0, 0, -1 };
		} else {
			forward.x /= length;
			forward.z /= length;
		}
		const float halfYaw = 0.5f * std::atan2(-forward.x, -forward.z);
		return { { 0, std::sin(halfYaw), 0, std::cos(halfYaw) },
			{ a_head.position.x + forward.x * a_distance, a_head.position.y - 0.15f,
				a_head.position.z + forward.z * a_distance } };
	}

	[[nodiscard]] inline Hit Intersect(Pose a_aim, Pose a_panel, float a_width, float a_height) noexcept
	{
		if (!(a_width > 0) || !(a_height > 0)) {
			return {};
		}
		const auto direction = Rotate(a_aim.orientation, { 0, 0, -1 });
		const auto normal = Rotate(a_panel.orientation, { 0, 0, 1 });
		const Vec3 delta{ a_panel.position.x - a_aim.position.x,
			a_panel.position.y - a_aim.position.y, a_panel.position.z - a_aim.position.z };
		const float denominator = direction.x * normal.x + direction.y * normal.y + direction.z * normal.z;
		if (!(denominator < -0.0001f)) {
			return {};
		}
		const float distance = (delta.x * normal.x + delta.y * normal.y + delta.z * normal.z) / denominator;
		if (!(distance > 0) || !std::isfinite(distance)) {
			return {};
		}
		const Vec3 point{ direction.x * distance - delta.x,
			direction.y * distance - delta.y, direction.z * distance - delta.z };
		const auto& q = a_panel.orientation;
		const auto local = Rotate({ -q.x, -q.y, -q.z, q.w }, point);
		const float x = 0.5f + local.x / a_width;
		const float y = 0.5f - local.y / a_height;
		if (!std::isfinite(x) || !std::isfinite(y)) {
			return {};
		}
		return { true, x >= 0 && x <= 1 && y >= 0 && y <= 1,
			std::clamp(x, -0.1f, 1.1f), std::clamp(y, -0.1f, 1.1f) };
	}
}
