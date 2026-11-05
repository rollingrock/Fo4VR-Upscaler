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
// fall outside every measurement window. All of it only while foveation is in play
// (InPlay): with every key at its default a DLAA frame observes nothing, logs nothing and
// resets no window.

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
		// Phase 2a: foveationSize is outside [0.5, 0.95].
		kSizeOutOfRange,
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
		case Refusal::kSizeOutOfRange:
			return "foveationSize is out of range (0.5 to 0.95)";
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
		// Which key was out of range, as the refusal names it: the measurement key's, or
		// Phase 2a's foveationSize (ResolveProductSize).
		Refusal rangeRefusal = Refusal::kOutOfRange;

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

	// Phase 2a's product keys as a request: foveationSize, honoured in [0.5, 0.95] and refused,
	// as itself, anywhere else.
	inline constexpr double kProductSizeMin = 0.5;
	inline constexpr double kProductSizeMax = 0.95;

	[[nodiscard]] constexpr SettingResolution ResolveProductSize(double a_size) noexcept
	{
		if (!(a_size >= kProductSizeMin && a_size <= kProductSizeMax)) {
			SettingResolution out{};
			out.outOfRange = true;
			out.rangeRefusal = Refusal::kSizeOutOfRange;
			return out;
		}
		return ResolveSetting(a_size, false);
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

	// The generation travels in bits 17 to 30 of the packed request, beside the held flag.
	inline constexpr std::uint32_t kGenerationLimit = 0x4000u;

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

	// What one press of the foveation key does. The key is Phase 2a's toggle when
	// foveationToggleVk is bound, and the measurement build's cycle key otherwise. The toggle
	// key only ever toggles the product: with foveation off, or overridden by the measurement
	// key below 1.0, it is refused and nothing cycles. The cycle key cycles, except that with
	// the product on it toggles too - the product owns the crop then.
	enum class KeyAnswer : std::uint32_t
	{
		kToggle = 0,
		kCycle,
		kProductOff,
		kMeasurementOverrides,
		// The seam failed soft this session (SeamHealth): foveation stays off.
		kSeamFailed,
	};

	[[nodiscard]] constexpr KeyAnswer AnswerFoveationKey(bool a_toggleKey, bool a_product, bool a_measurementOverrides, bool a_seamFailed = false) noexcept
	{
		if (a_product) {
			return a_seamFailed ? KeyAnswer::kSeamFailed : KeyAnswer::kToggle;
		}
		if (!a_toggleKey) {
			return KeyAnswer::kCycle;
		}
		return a_measurementOverrides ? KeyAnswer::kMeasurementOverrides : KeyAnswer::kProductOff;
	}

	[[nodiscard]] constexpr std::string_view KeyAnswerRefusal(KeyAnswer a_answer) noexcept
	{
		switch (a_answer) {
		case KeyAnswer::kProductOff:
			return "foveation is off (foveation = false)";
		case KeyAnswer::kMeasurementOverrides:
			return "foveationCropScale below 1.0 overrides foveation";
		case KeyAnswer::kSeamFailed:
			return "the foveated seam failed this session, so foveation is off until the game restarts";
		default:
			return "";
		}
	}

	// Phase 2a's fail-soft (plan section 6). A seam blend that fails leaves the game's own
	// TemporalAA in the box for that frame - softer, not broken - so one failure needs
	// nothing. A fault that persists or keeps coming back turns foveation off for the
	// session, and DLAA reconstructs the whole eye as with the toggle at full eye. A leaky
	// score: each failure adds kSeamFailureWeight, each blend takes one away, and reaching
	// kSeamFailureLimit ends it.
	// - Three failures in a row trip it: 33 ms at 90 Hz, the shortest run that tells a
	//   standing fault from a one-off.
	// - A failure on every other frame trips it by the seventh frame, and any sustained rate
	//   above one frame in eleven trips it in the end.
	// - An isolated failure, or one a second, never accumulates.
	// A blend that could not even be built (its shader or states) ends it at once.
	inline constexpr std::uint32_t kSeamFailureWeight = 10;
	inline constexpr std::uint32_t kSeamFailureLimit = 30;

	enum class SeamFailure : std::uint32_t
	{
		kNone = 0,
		kBlends,
		kBuild,
	};

	struct SeamHealth
	{
		std::uint32_t score = 0;
		std::uint64_t failures = 0;
		bool failedSoft = false;
		SeamFailure cause = SeamFailure::kNone;
	};

	[[nodiscard]] constexpr SeamHealth ObserveSeamBlend(SeamHealth a_health, bool a_blended, bool a_buildFailed) noexcept
	{
		if (a_health.failedSoft) {
			return a_health;
		}
		if (a_buildFailed) {
			++a_health.failures;
			a_health.failedSoft = true;
			a_health.cause = SeamFailure::kBuild;
			return a_health;
		}
		if (a_blended) {
			a_health.score = a_health.score > 0 ? a_health.score - 1 : 0;
			return a_health;
		}
		++a_health.failures;
		a_health.score += kSeamFailureWeight;
		if (a_health.score >= kSeamFailureLimit) {
			a_health.failedSoft = true;
			a_health.cause = SeamFailure::kBlends;
		}
		return a_health;
	}

	// Phase 2a's reset fade. A DLAA history reset restarts the box's history while the game's
	// TemporalAA outside it keeps its own, so for the few frames DLAA needs to converge again
	// the box would show against its surround - the flash the owner saw at each scope raise and
	// lower (headset run 1, 2026-11-04). After a reset the seam's box weight ramps from 0, where
	// the box shows the game's own TemporalAA image, to 1, full DLAA, over a_fadeFrames product
	// frames: frame k after the reset weighs k / a_fadeFrames. The feather is unchanged; the fade
	// multiplies it. Every reset counts - scope, menu, load, cell, re-placement, size change -
	// because they all reach DLAA as one resetHistory, which Evaluate counts.
	// Off (0) by default. Headset run 2 found that a fade of 10 frames did not change the flash
	// on a scope's lowering, and that full-eye DLAA flashed the same: the flash is the reset's,
	// not the box's, and resetOnScope off is what removed it. The fade stays as a development key.
	inline constexpr std::uint32_t kDefaultResetFadeFrames = 0;
	inline constexpr std::uint32_t kMaxResetFadeFrames = 120;

	struct ResetFade
	{
		// Evaluate's count of evaluations that reset the history, as last seen, and the frames
		// since; it starts settled, so a session with no reset never fades.
		std::uint64_t resets = 0;
		std::uint32_t frame = kMaxResetFadeFrames;
	};

	struct ResetFadeStep
	{
		ResetFade next{};
		float alpha = 1.0f;
		// A fade began this frame.
		bool started = false;
	};

	// One foveated product frame. a_resets is Evaluate's running count; a change is a reset on
	// this frame's evaluation (or on frames this did not see, which restarts the fade the same).
	[[nodiscard]] constexpr ResetFadeStep ObserveResetFade(ResetFade a_fade, std::uint64_t a_resets, std::uint32_t a_fadeFrames) noexcept
	{
		ResetFadeStep out{};
		out.next = a_fade;
		if (a_resets != a_fade.resets) {
			out.next.resets = a_resets;
			out.next.frame = 0;
			out.started = a_fadeFrames > 0;
		} else if (out.next.frame < a_fadeFrames) {
			++out.next.frame;
		}
		if (a_fadeFrames == 0 || out.next.frame >= a_fadeFrames) {
			out.alpha = 1.0f;
		} else {
			out.alpha = static_cast<float>(out.next.frame) / static_cast<float>(a_fadeFrames);
		}
		return out;
	}

	[[nodiscard]] constexpr std::string_view SeamFailureName(SeamFailure a_cause) noexcept
	{
		switch (a_cause) {
		case SeamFailure::kBlends:
			return "blends";
		case SeamFailure::kBuild:
			return "build";
		default:
			return "";
		}
	}

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

	// A key's choice stands - a cycle step, or the toggle's either side - in a session that
	// may crop. The render thread reads it beside the request, because the cycle's 100% and
	// the toggle's full eye are whole-eye requests that are still foveation in play.
	[[nodiscard]] constexpr bool KeyChoiceHeld(const Selection& a_selection) noexcept
	{
		return a_selection.session == Refusal::kNone && a_selection.hotkeyActive;
	}

	// What /state reports as refused: the session's reason, else a setting out of range that
	// the hotkey has not superseded. A whole-eye request is not a refusal.
	[[nodiscard]] constexpr Refusal ReportedRefusal(const Selection& a_selection) noexcept
	{
		const bool wanted = a_selection.hotkeyActive || a_selection.setting.outOfRange || Cropping(a_selection.setting.request);
		if (a_selection.session != Refusal::kNone) {
			return wanted ? a_selection.session : Refusal::kNone;
		}
		return a_selection.setting.outOfRange && !a_selection.hotkeyActive ? a_selection.setting.rangeRefusal : Refusal::kNone;
	}

	// The one atomic the render thread loads per dispatch: the thousandths in the low 16
	// bits, the moving flag in bit 16, the press generation in bits 17 to 30 and
	// KeyChoiceHeld in bit 31. One atomic, so a press can never be seen with half of its
	// request.
	inline constexpr std::uint32_t kHeldBit = 0x80000000u;

	[[nodiscard]] constexpr std::uint32_t Pack(const Request& a_request, std::uint32_t a_generation = 0, bool a_held = false) noexcept
	{
		return (a_request.milli & 0xFFFFu) | (a_request.moving ? 0x10000u : 0u) | ((a_generation % kGenerationLimit) << 17) |
		       (a_held ? kHeldBit : 0u);
	}

	[[nodiscard]] constexpr std::uint32_t UnpackGeneration(std::uint32_t a_packed) noexcept
	{
		return (a_packed >> 17) & (kGenerationLimit - 1u);
	}

	[[nodiscard]] constexpr bool UnpackHeld(std::uint32_t a_packed) noexcept
	{
		return (a_packed & kHeldBit) != 0;
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

	// Whether foveation takes part in a product frame: this frame crops, the last observed
	// one did, or a key's choice stands (KeyChoiceHeld). The product's own flag is not
	// needed: its box crops, its toggled full eye is held, and a product that crops nothing
	// (a refused session, the fail-soft) has nothing to measure.
	[[nodiscard]] constexpr bool InPlay(const Request& a_current, bool a_held, const Request& a_last) noexcept
	{
		return Cropping(a_current) || a_held || Cropping(a_last);
	}

	// The render thread's observation of one product frame, gated on InPlay. Out of play it
	// is quiet: no change, no restart, no reset and no settle, so a DLAA session with every
	// foveation key at its default logs no foveation line and resets no perf window, however
	// many gaps (loads, compare-key vetoes) it has. A frame that leaves play - the crop
	// turned off live, the fail-soft, the keys handed back to a whole-eye setting - still
	// reports its change and asks for the history reset the crop's end needs, but restarts
	// nothing and drops any settle: there is nothing left to measure.
	[[nodiscard]] constexpr Observation ObserveInPlay(
		bool a_seen,
		const Request& a_last,
		std::uint32_t a_lastGeneration,
		const Request& a_current,
		std::uint32_t a_generation,
		bool a_held,
		std::uint32_t a_settle) noexcept
	{
		if (!InPlay(a_current, a_held, a_last)) {
			return {};
		}
		auto out = Observe(a_seen, a_last, a_lastGeneration, a_current, a_generation, a_settle);
		if (!Cropping(a_current) && !a_held) {
			out.restart = false;
			out.settle = 0;
		}
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
