#pragma once

// The foveated measurement build's rules, as pure decisions, so the engine-free harness in
// tools/world-admission-tests runs the same functions the settings apply, the hotkey and the
// HDR dispatch do. FoveationCrop.h says how large a crop is and where it sits; this file
// says whether there is one at all, which source chose it, and when the render thread resets
// the histories and the perf window around a change.
//
// Who may crop: the DLAA product only. A crop is an NGX DLAA view created at the crop extent
// and addressed by base, so it needs the live mode to be "dlaa" on the product path, native
// AA, the raw NGX backend and the sub-rect stereo path. Anything else refuses the crop and
// reconstructs the whole eye, and the refusal is reported, never silent.
//
// Which source: the [Diagnostics] keys, or the cycle hotkey. The hotkey is runtime only and
// writes no setting; it wins until the settings themselves change, so an unrelated apply
// (a preset, a timer toggle) never undoes a crop the operator just stepped to, while an edit
// of the keys always takes effect.
//
// When: the render thread restarts the measurement - a settle count, and kSettleFrames
// product frames later a perf-window reset - on three events: a different crop, a new
// cycle-key press (each accepted press is a generation, even one that selects the crop
// already in effect, so every press opens a window of its own), and the first product frame
// after a gap in product frames (mode off and back, a re-arm, a veto, a reduced-mode round
// trip), after which the views and histories are new. Only a different crop asks for a
// history reset. The re-creation hitch, the reset frame and the reconvergence therefore all
// fall outside every measurement window.

#include <cstdint>
#include <string_view>

// Deliberately free of every engine and D3D header, FoveationCrop.h's included, so the
// world-admission harness can compile it bare. The one value the two share is the floor
// below, which HDRRoundTrip.cpp asserts is FoveationCrop.h's kMinimumScale.

namespace Upscaler::Foveation
{
	// The lowest scale the keys may ask for, in thousandths.
	inline constexpr std::uint32_t kMinimumSettingMilli = 250;

	enum class Refusal : std::uint32_t
	{
		kNone = 0,
		// The setting is neither 1.0 nor in [0.25, 1).
		kOutOfRange,
		// The live mode is off or a reduced mode.
		kModeNotDlaa,
		// A diagnostic gate drives the session rather than the product path.
		kNotProductPath,
		kQualityNotDlaa,
		// The DLAA product runs on a backend other than raw NGX.
		kBackendNotNgx,
		// stereoMode resolved to the copy path, which has no output sub-rects to place by.
		kIntermediates,
	};

	[[nodiscard]] constexpr std::string_view RefusalName(Refusal a_refusal) noexcept
	{
		switch (a_refusal) {
		case Refusal::kNone:
			return "";
		case Refusal::kOutOfRange:
			return "foveationCropScale is out of range (0.25 to 1.0)";
		case Refusal::kModeNotDlaa:
			return "the live mode is not dlaa";
		case Refusal::kNotProductPath:
			return "a diagnostic gate drives this session";
		case Refusal::kQualityNotDlaa:
			return "quality is not dlaa";
		case Refusal::kBackendNotNgx:
			return "the DLAA backend is not ngx";
		case Refusal::kIntermediates:
			return "stereoMode resolved to intermediates";
		default:
			return "unknown";
		}
	}

	// What the session is, as the settings apply resolved it.
	struct Session
	{
		bool modeDlaa = false;
		bool productPath = false;
		bool qualityDlaa = false;
		bool rawNgx = false;
		bool subrects = false;
	};

	[[nodiscard]] constexpr Refusal SessionRefusal(const Session& a_session) noexcept
	{
		if (!a_session.modeDlaa) {
			return Refusal::kModeNotDlaa;
		}
		if (!a_session.productPath) {
			return Refusal::kNotProductPath;
		}
		if (!a_session.qualityDlaa) {
			return Refusal::kQualityNotDlaa;
		}
		if (!a_session.rawNgx) {
			return Refusal::kBackendNotNgx;
		}
		if (!a_session.subrects) {
			return Refusal::kIntermediates;
		}
		return Refusal::kNone;
	}

	// A crop request in thousandths of the per-axis scale. 1000 is the whole eye, and a
	// whole-eye request never moves.
	struct Request
	{
		std::uint32_t milli = 1000;
		bool moving = false;

		friend constexpr bool operator==(const Request&, const Request&) = default;
	};

	[[nodiscard]] constexpr bool Cropping(const Request& a_request) noexcept
	{
		return a_request.milli < 1000;
	}

	[[nodiscard]] constexpr float Scale(const Request& a_request) noexcept
	{
		return static_cast<float>(a_request.milli) / 1000.0f;
	}

	struct SettingResolution
	{
		Request request{};
		bool outOfRange = false;

		friend constexpr bool operator==(const SettingResolution&, const SettingResolution&) = default;
	};

	// The [Diagnostics] keys as a request. 1.0 is off; anything else outside
	// [kMinimumSettingMilli / 1000, 1) is out of range and also reconstructs the whole eye.
	[[nodiscard]] constexpr SettingResolution ResolveSetting(double a_scale, bool a_moving) noexcept
	{
		SettingResolution out{};
		if (!(a_scale >= static_cast<double>(kMinimumSettingMilli) / 1000.0 && a_scale <= 1.0)) {
			out.outOfRange = true;
			return out;
		}
		const auto milli = static_cast<std::uint32_t>(a_scale * 1000.0 + 0.5);
		if (milli >= 1000) {
			return out;
		}
		out.request = { milli, a_moving };
		return out;
	}

	// The cycle the hotkey steps through: the four Phase 1 crops, then the moving probe.
	inline constexpr Request kCycle[] = {
		{ 1000, false },
		{ 750, false },
		{ 600, false },
		{ 500, false },
		{ 600, true },
	};
	inline constexpr std::int32_t kCycleSteps = static_cast<std::int32_t>(sizeof(kCycle) / sizeof(kCycle[0]));

	// The first press after a launch, or after the settings took over again, is the whole
	// eye, so the baseline window is always the first one measured.
	[[nodiscard]] constexpr std::int32_t NextCycleIndex(std::int32_t a_current) noexcept
	{
		if (a_current < 0 || a_current >= kCycleSteps - 1) {
			return 0;
		}
		return a_current + 1;
	}

	// The whole selection state the settings apply and the hotkey share. Under one lock in
	// HDRRoundTrip; the render thread reads only the packed effective request.
	struct Selection
	{
		SettingResolution setting{};
		bool haveSetting = false;
		bool hotkeyActive = false;
		std::int32_t hotkeyIndex = -1;
		// Phase 2a's toggle key: the hotkey's choice is this request, not a cycle step.
		bool hotkeyToggle = false;
		Request hotkeyRequest{};
		Refusal session = Refusal::kModeNotDlaa;
		// Accepted cycle-key presses, modulo kGenerationLimit. Settings applies leave it.
		std::uint32_t generation = 0;
	};

	// The generation travels in the packed request's top 15 bits.
	inline constexpr std::uint32_t kGenerationLimit = 0x8000u;

	// A settings apply. A changed key hands the choice back to the settings; an unchanged
	// one leaves a hotkey choice standing.
	[[nodiscard]] constexpr Selection ApplySetting(Selection a_selection, SettingResolution a_setting, Refusal a_session) noexcept
	{
		if (!a_selection.haveSetting || !(a_selection.setting == a_setting)) {
			a_selection.hotkeyActive = false;
			a_selection.hotkeyIndex = -1;
			a_selection.hotkeyToggle = false;
			a_selection.hotkeyRequest = {};
		}
		a_selection.setting = a_setting;
		a_selection.haveSetting = true;
		a_selection.session = a_session;
		return a_selection;
	}

	struct CycleResult
	{
		Selection selection{};
		bool refused = false;
	};

	// One press of the cycle key. Refused, changing nothing, wherever the session refuses a
	// crop.
	[[nodiscard]] constexpr CycleResult ApplyCycle(Selection a_selection) noexcept
	{
		if (a_selection.session != Refusal::kNone) {
			return { a_selection, true };
		}
		a_selection.hotkeyIndex = NextCycleIndex(a_selection.hotkeyActive && !a_selection.hotkeyToggle ? a_selection.hotkeyIndex : -1);
		a_selection.hotkeyActive = true;
		a_selection.hotkeyToggle = false;
		a_selection.generation = (a_selection.generation + 1u) % kGenerationLimit;
		return { a_selection, false };
	}

	[[nodiscard]] constexpr Request Effective(const Selection& a_selection) noexcept;

	// One press of Phase 2a's toggle key: the whole eye if a crop is in effect, else the
	// crop the keys ask for - foveated against full eye, nothing else. A setting that asks
	// for no crop leaves nothing to toggle to, so the press is refused, as it is wherever
	// the session refuses a crop. An accepted press is a generation, like a cycle press.
	[[nodiscard]] constexpr CycleResult ApplyToggle(Selection a_selection) noexcept
	{
		if (a_selection.session != Refusal::kNone || !Cropping(a_selection.setting.request)) {
			return { a_selection, true };
		}
		a_selection.hotkeyRequest = Cropping(Effective(a_selection)) ? Request{} : a_selection.setting.request;
		a_selection.hotkeyActive = true;
		a_selection.hotkeyToggle = true;
		a_selection.hotkeyIndex = -1;
		a_selection.generation = (a_selection.generation + 1u) % kGenerationLimit;
		return { a_selection, false };
	}

	[[nodiscard]] constexpr Request Effective(const Selection& a_selection) noexcept
	{
		if (a_selection.session != Refusal::kNone) {
			return {};
		}
		if (a_selection.hotkeyActive && a_selection.hotkeyToggle) {
			return a_selection.hotkeyRequest;
		}
		if (a_selection.hotkeyActive && a_selection.hotkeyIndex >= 0 && a_selection.hotkeyIndex < kCycleSteps) {
			return kCycle[a_selection.hotkeyIndex];
		}
		return a_selection.setting.outOfRange ? Request{} : a_selection.setting.request;
	}

	// What /state reports as refused: the session's reason, else a setting out of range that
	// the hotkey has not superseded. A whole-eye request is not a refusal.
	[[nodiscard]] constexpr Refusal ReportedRefusal(const Selection& a_selection) noexcept
	{
		const bool wanted = a_selection.hotkeyActive || a_selection.setting.outOfRange || Cropping(a_selection.setting.request);
		if (a_selection.session != Refusal::kNone) {
			return wanted ? a_selection.session : Refusal::kNone;
		}
		return a_selection.setting.outOfRange && !a_selection.hotkeyActive ? Refusal::kOutOfRange : Refusal::kNone;
	}

	// The one atomic the render thread loads per dispatch: the thousandths in the low 16
	// bits, the moving flag in bit 16, the press generation in bits 17 to 31. One atomic, so
	// a press can never be seen with half of its request.
	[[nodiscard]] constexpr std::uint32_t Pack(const Request& a_request, std::uint32_t a_generation = 0) noexcept
	{
		return (a_request.milli & 0xFFFFu) | (a_request.moving ? 0x10000u : 0u) | ((a_generation % kGenerationLimit) << 17);
	}

	[[nodiscard]] constexpr std::uint32_t UnpackGeneration(std::uint32_t a_packed) noexcept
	{
		return a_packed >> 17;
	}

	[[nodiscard]] constexpr Request Unpack(std::uint32_t a_packed) noexcept
	{
		Request out{ a_packed & 0xFFFFu, (a_packed & 0x10000u) != 0 };
		// The generation bits are not part of the request.
		if (out.milli == 0 || out.milli >= 1000) {
			return {};
		}
		return out;
	}

	inline constexpr std::uint32_t kSettleFrames = 90;

	struct Observation
	{
		// This product frame sees a crop other than the last one observed.
		bool changed = false;
		// Restart the measurement: a change, a new press generation, or the first product
		// frame after a gap (a_seen false). The settle count starts again.
		bool restart = false;
		// Ask for one paired history reset before this frame's evaluate - only for a change.
		bool requestReset = false;
		std::uint32_t settle = 0;
	};

	// The render thread's per-product-frame observation. a_seen is false on the first
	// product frame after any frame that was not one, so the views and histories may be new
	// there: that always restarts the measurement, and counts as a change - a reset - only
	// when it crops. A press that re-selects the crop in effect restarts without a reset.
	[[nodiscard]] constexpr Observation Observe(
		bool a_seen,
		const Request& a_last,
		std::uint32_t a_lastGeneration,
		const Request& a_current,
		std::uint32_t a_generation,
		std::uint32_t a_settle) noexcept
	{
		Observation out{};
		out.changed = a_seen ? !(a_last == a_current) : Cropping(a_current);
		out.restart = !a_seen || out.changed || a_generation != a_lastGeneration;
		out.requestReset = out.changed;
		out.settle = out.restart ? kSettleFrames : a_settle;
		return out;
	}

	struct SettleStep
	{
		std::uint32_t settle = 0;
		// Reset the perf window now: the settle has just run out.
		bool resetWindow = false;
	};

	// One successful product frame of the settle count.
	[[nodiscard]] constexpr SettleStep TickSettle(std::uint32_t a_settle) noexcept
	{
		if (a_settle == 0) {
			return {};
		}
		return { a_settle - 1u, a_settle == 1u };
	}
}
