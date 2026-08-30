#include "Upscaler/StereoLayout.h"

#include "Settings/Settings.h"
#include "Upscaler/Common.h"

#include <atomic>

namespace Upscaler::StereoLayout
{
	namespace
	{
		enum class Resolution : std::uint8_t
		{
			kUnresolved,
			kVerifiedLeftRight,
			kOverrideLeftRight,
			kOverrideRightLeft
		};

		std::atomic<Resolution> g_resolution{ Resolution::kUnresolved };
		LogGate g_unresolved;
		LogGate g_invalidDimensions{ 10000 };

		[[nodiscard]] constexpr Order ToOrder(Resolution a_resolution) noexcept
		{
			return a_resolution == Resolution::kOverrideRightLeft ? Order::kRightLeft : Order::kLeftRight;
		}
	}

	bool Resolve()
	{
		Resolution next = Resolution::kUnresolved;
		const std::string_view configured = *Settings::eyeOrder;
		if (configured == "auto") {
			next = Resolution::kVerifiedLeftRight;
		} else if (configured == "LR") {
			next = Resolution::kOverrideLeftRight;
		} else if (configured == "RL") {
			next = Resolution::kOverrideRightLeft;
		}

		const Resolution previous = g_resolution.exchange(next, std::memory_order_acq_rel);
		if (next == Resolution::kUnresolved) {
			if (g_unresolved.Ready()) {
				logger::warn(FMT_STRING("stereo layout unresolved: eyeOrder '{}' is not auto, LR or RL"), configured);
			}
			return false;
		}

		g_unresolved.Rearm();
		if (previous != next) {
			logger::info(FMT_STRING("stereo layout resolved: {}"), Summary());
		}
		return true;
	}

	bool Resolved() noexcept
	{
		return g_resolution.load(std::memory_order_acquire) != Resolution::kUnresolved;
	}

	std::string_view Summary() noexcept
	{
		switch (g_resolution.load(std::memory_order_acquire)) {
		case Resolution::kVerifiedLeftRight:
			return "left-first (LR), verified FO4VR 1.2.72 default (eyeOrder=auto)"sv;
		case Resolution::kOverrideLeftRight:
			return "left-first (LR), explicit override"sv;
		case Resolution::kOverrideRightLeft:
			return "right-first (RL), explicit override"sv;
		default:
			return "unresolved"sv;
		}
	}

	Backend::Rect EyeRect(Eye a_eye, std::uint32_t a_width, std::uint32_t a_height) noexcept
	{
		const Resolution resolution = g_resolution.load(std::memory_order_acquire);
		if (resolution == Resolution::kUnresolved) {
			if (g_unresolved.Ready()) {
				logger::warn("stereo layout requested before Resolve; returning an empty rectangle"sv);
			}
			return {};
		}

		const Backend::Rect rect = RectFor(a_eye, ToOrder(resolution), a_width, a_height);
		if (rect.w == 0 && g_invalidDimensions.Ready()) {
			logger::warn(
				FMT_STRING("stereo layout rejected dimensions {}x{} or an invalid eye; a double-wide width must be positive and even"),
				a_width,
				a_height);
		}
		return rect;
	}
}
