#pragma once

// The in-headset panel's reducer: what the panel draws, and which intent one input
// produces. Pure, total and engine-free - no ImGui, no OpenVR, no CommonLibF4, no
// spdlog, no allocation. Its only dependency is Settings/SettingsPolicy.h, which is
// itself engine-free, and that is deliberate: the admissibility rule a mode button
// reports before the click has to be the same rule the writer's apply runs after it, and
// two copies of that rule is exactly the divergence a panel exists to avoid.
//
// Every input is a plain copy of a snapshot some other module already publishes and
// /state already reports. Nothing here reads a global, takes a lock or looks at the
// clock, so Panel.cpp can run the whole reducer on the render thread's frame producer
// once per visible frame, and every contract below is a static_assert rather than a test
// nobody runs - the shape Settings::Policy uses to prove ResolveModeSwitch.
//
// The text type is a fixed-capacity buffer rather than std::string for both reasons at
// once: a literal type is what makes the contracts compile-time, and no allocation is
// what makes it safe on the frame producer. Appending past the capacity truncates; a
// truncated diagnostic line is a worse label, never a corrupt one.

#include "Settings/SettingsPolicy.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Upscaler::PanelModel
{
	using namespace std::string_view_literals;

	template <std::size_t N>
	struct Text
	{
		static_assert(N > 1, "a Text needs room for at least one character and its terminator");

		char chars[N]{};
		std::size_t length = 0;

		constexpr void Append(std::string_view a_text) noexcept
		{
			for (const char c : a_text) {
				if (length + 1 >= N) {
					return;
				}
				chars[length++] = c;
			}
		}

		constexpr void AppendUnsigned(std::uint64_t a_value) noexcept
		{
			char digits[24]{};
			std::size_t count = 0;
			do {
				digits[count++] = static_cast<char>('0' + static_cast<char>(a_value % 10u));
				a_value /= 10u;
			} while (a_value != 0u && count < sizeof(digits));
			while (count > 0) {
				--count;
				Append(std::string_view(&digits[count], 1));
			}
		}

		constexpr void AppendSigned(std::int64_t a_value) noexcept
		{
			if (a_value < 0) {
				Append("-"sv);
				AppendUnsigned(static_cast<std::uint64_t>(-(a_value + 1)) + 1u);
				return;
			}
			AppendUnsigned(static_cast<std::uint64_t>(a_value));
		}

		// Half-up at the requested precision. Deliberately not snprintf: this has to be
		// constexpr for the contracts below, and the value it produces is the text handed
		// to Settings::Writer::Set, so the panel and /config/set send the same characters
		// for the same slider position.
		constexpr void AppendFixed(double a_value, unsigned a_decimals) noexcept
		{
			if (a_value < 0.0) {
				Append("-"sv);
				a_value = -a_value;
			}
			std::uint64_t scale = 1u;
			for (unsigned i = 0; i < a_decimals; ++i) {
				scale *= 10u;
			}
			const auto scaled = static_cast<std::uint64_t>(a_value * static_cast<double>(scale) + 0.5);
			AppendUnsigned(scaled / scale);
			if (a_decimals == 0) {
				return;
			}
			Append("."sv);
			std::uint64_t remainder = scaled % scale;
			for (unsigned place = a_decimals; place > 0; --place) {
				std::uint64_t divisor = 1u;
				for (unsigned i = 1; i < place; ++i) {
					divisor *= 10u;
				}
				const char digit = static_cast<char>('0' + static_cast<char>(remainder / divisor));
				Append(std::string_view(&digit, 1));
				remainder %= divisor;
			}
		}

		[[nodiscard]] constexpr std::string_view View() const noexcept
		{
			return std::string_view(chars, length);
		}

		[[nodiscard]] constexpr const char* CStr() const noexcept { return chars; }

		[[nodiscard]] constexpr bool operator==(std::string_view a_other) const noexcept
		{
			return View() == a_other;
		}
	};

	// Long enough for the longest tag the outcome vocabulary can produce -
	// "next launch: refused-needs-reduced-session" is 42 characters.
	using Label = Text<64>;
	// Long enough for the writer's own reason text, which runs to about 170 characters
	// for the two ratio refusals.
	using ResultText = Text<320>;

	// Every clickable thing on the panel, in draw order. The six mode ids are contiguous
	// so the button table below can be indexed from kModeOff; nothing else depends on the
	// ordering. The names are the wire names /panel action=status publishes rectangles
	// under, which is how a driver clicks a control without knowing the layout.
	enum class ControlId : std::uint32_t
	{
		kNone = 0,
		kModeOff,
		kModeDlaa,
		kModeQuality,
		kModeBalanced,
		kModePerformance,
		kModeUltraPerformance,
		kRatioSlider,
		kPresetAuto,
		kPresetF,
		kPresetJ,
		kPresetK,
		kPresetL,
		kPresetM,
		kSharpnessSlider,
		kHoldNative,
		kNativeLatch,
		kTimers,
		kCount
	};

	inline constexpr std::size_t kModeCount = 6;
	inline constexpr std::size_t kPresetCount = 6;
	inline constexpr std::size_t kControlCount = static_cast<std::size_t>(ControlId::kCount);

	[[nodiscard]] constexpr std::string_view ControlName(ControlId a_control) noexcept
	{
		switch (a_control) {
		case ControlId::kModeOff:
			return "mode-off"sv;
		case ControlId::kModeDlaa:
			return "mode-dlaa"sv;
		case ControlId::kModeQuality:
			return "mode-quality"sv;
		case ControlId::kModeBalanced:
			return "mode-balanced"sv;
		case ControlId::kModePerformance:
			return "mode-performance"sv;
		case ControlId::kModeUltraPerformance:
			return "mode-ultraperformance"sv;
		case ControlId::kRatioSlider:
			return "ratio-slider"sv;
		case ControlId::kPresetAuto:
			return "preset-auto"sv;
		case ControlId::kPresetF:
			return "preset-F"sv;
		case ControlId::kPresetJ:
			return "preset-J"sv;
		case ControlId::kPresetK:
			return "preset-K"sv;
		case ControlId::kPresetL:
			return "preset-L"sv;
		case ControlId::kPresetM:
			return "preset-M"sv;
		case ControlId::kSharpnessSlider:
			return "sharpness-slider"sv;
		case ControlId::kHoldNative:
			return "hold-native"sv;
		case ControlId::kNativeLatch:
			return "native-latch"sv;
		case ControlId::kTimers:
			return "timers"sv;
		case ControlId::kNone:
		case ControlId::kCount:
		default:
			return "none"sv;
		}
	}

	// --- the settings keys the panel writes, spelled once ---------------------------
	//
	// These are Settings key names, checked against SettingsList.inl by the writer
	// itself: an unknown key comes back refused with the key named, so a typo here is a
	// visible refusal on the result line rather than a silent no-op.
	inline constexpr std::string_view kModeKey = "mode"sv;
	inline constexpr std::string_view kRatioKey = "renderScaleOverride"sv;
	inline constexpr std::string_view kPresetKey = "preset"sv;
	inline constexpr std::string_view kSharpnessKey = "sharpness"sv;
	inline constexpr std::string_view kTimersKey = "perfTimers"sv;

	// The slider bounds. 0.30 is DynamicResolution's own floor and 1.00 its ceiling -
	// see RatioRequestOutcome::kRefusedOutOfRange - and 1.00 is inside the key's range
	// but is a mode change beside a reduced mode, which is what fullWouldBeRefused warns
	// about before the click.
	inline constexpr double kRatioMinimum = 0.30;
	inline constexpr double kRatioMaximum = 1.00;
	// Four places, because quality's own default ratio is 0.6667 and a slider that
	// could not express it would refuse to put the panel back where it started.
	inline constexpr unsigned kRatioDecimals = 4;
	inline constexpr double kSharpnessMinimum = 0.0;
	inline constexpr double kSharpnessMaximum = 1.0;
	inline constexpr unsigned kSharpnessDecimals = 3;

	// --- inputs ----------------------------------------------------------------------

	// Configuration::ModeSwitchState, copied. The views point at that snapshot's own
	// strings, so it must outlive the call - Panel.cpp holds it as a frame local.
	struct ModeSwitchInput
	{
		std::string_view liveMode;
		std::string_view modeSource;
		std::string_view sessionArmedMode;
		std::string_view lastNonOffMode;
		bool bracketArmed = false;
	};

	// Upscaler::DynamicResolution::State, the fields the panel draws.
	struct RatioInput
	{
		double requestedScale = 0.0;
		std::uint32_t renderWidth = 0;
		std::uint32_t renderHeight = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
	};

	// The live settings values, as Configuration::Apply published them - never read out
	// of the global Setting<T> instances, which a load can be halfway through rewriting.
	struct LiveInput
	{
		std::string_view preset;
		double renderScaleOverride = 0.0;
		double sharpness = 0.0;
		bool perfTimers = false;
	};

	// Settings::Writer::SetResult, as the panel worker published it. present is false
	// until one intent has completed.
	struct ResultInput
	{
		bool present = false;
		bool accepted = false;
		bool persisted = false;
		bool tookEffectNow = false;
		bool needsRestart = false;
		std::string_view key;
		std::string_view value;
		std::string_view reason;
	};

	// ResetPolicy's operator veto, and whether this panel is the owner of the standing
	// engagement. ownedByPanel is the panel's own record, never a mirror of the atomic:
	// a veto engaged through /ab or the COMPARE key is not the panel's to clear.
	struct VetoInput
	{
		bool active = false;
		bool ownedByPanel = false;
	};

	struct KeysInput
	{
		std::int64_t toggleVk = 0;
		std::int64_t compareVk = 0;
		std::int64_t panelVk = 0;
	};

	struct Input
	{
		ModeSwitchInput modeSwitch;
		RatioInput ratio;
		LiveInput live;
		ResultInput lastResult;
		VetoInput veto;
		KeysInput keys;
		// True while the worker still owns an intent. The one fact that turns a second
		// SetKey into a refusal, and the one the result line reads as "pending".
		bool intentInFlight = false;
		// The panel's own hold and latch, as the draw last observed them. They decide
		// which A/B control is drawn engaged, never whether the veto is engaged - that is
		// ResetPolicy's atomic, reported in VetoInput.
		bool holdEngaged = false;
		bool latchOn = false;
	};

	// --- outputs ---------------------------------------------------------------------

	struct ModeButton
	{
		ControlId control = ControlId::kNone;
		std::string_view mode;
		Label label;
		// "live" when a click takes effect this session, "next launch: <outcome>" when
		// the admissibility rule refuses it and only the file changes.
		Label tag;
		bool live = false;
		bool current = false;
		Settings::Policy::SwitchOutcome outcome = Settings::Policy::SwitchOutcome::kUnchanged;
	};

	struct PresetEntry
	{
		ControlId control = ControlId::kNone;
		std::string_view value;
		Label label;
		Label note;
		bool current = false;
	};

	struct RatioControl
	{
		double value = 0.0;
		double minimum = kRatioMinimum;
		double maximum = kRatioMaximum;
		// Whether writing 1.00 here would be refused: 1.00 beside a reduced mode is a
		// mode change, not a ratio, and the writer's trial resolves it away rather than
		// saving it - see Settings::Policy::RenderScaleOverrideIsModeChange. Judged
		// against the mode actually running, which is what the operator is looking at;
		// the writer judges the merged file layer and its refusal is the authority.
		bool fullWouldBeRefused = false;
		Label sizeText;
	};

	struct SharpnessControl
	{
		double value = 0.0;
		double minimum = kSharpnessMinimum;
		double maximum = kSharpnessMaximum;
	};

	struct Output
	{
		ModeButton modes[kModeCount]{};
		RatioControl ratio;
		PresetEntry presets[kPresetCount]{};
		SharpnessControl sharpness;
		bool timersOn = false;
		bool holdEngaged = false;
		bool latchOn = false;
		bool vetoActive = false;
		bool vetoOwnedByPanel = false;
		Label keysText;
		ResultText resultLine;
	};

	// --- intents ---------------------------------------------------------------------

	enum class IntentKind : std::uint32_t
	{
		kNone = 0,
		// One Settings::Writer::Set(key, value). Only the panel worker runs it.
		kSetKey,
		// ResetPolicy::SetOperatorVeto, straight from the seam. engaged says which way.
		kVetoHold,
		kVetoLatch,
	};

	enum class Refusal : std::uint32_t
	{
		kNone = 0,
		// The worker still owns the previous intent. Named rather than queued, for the
		// same reason a second mode switch is refused kBusy rather than stacked.
		kIntentInFlight,
		kUnknownControl,
	};

	[[nodiscard]] constexpr std::string_view IntentKindName(IntentKind a_kind) noexcept
	{
		switch (a_kind) {
		case IntentKind::kSetKey:
			return "set-key"sv;
		case IntentKind::kVetoHold:
			return "veto-hold"sv;
		case IntentKind::kVetoLatch:
			return "veto-latch"sv;
		case IntentKind::kNone:
		default:
			return "none"sv;
		}
	}

	[[nodiscard]] constexpr std::string_view RefusalName(Refusal a_refusal) noexcept
	{
		switch (a_refusal) {
		case Refusal::kIntentInFlight:
			return "intent-in-flight"sv;
		case Refusal::kUnknownControl:
			return "unknown-control"sv;
		case Refusal::kNone:
		default:
			return "none"sv;
		}
	}

	struct Intent
	{
		IntentKind kind = IntentKind::kNone;
		Text<32> key;
		// The raw text Settings::Writer::Set parses - the same characters /config/set's
		// own value parameter carries, not the quoted TOML literal the writer answers
		// with.
		Text<32> value;
		bool engaged = false;
		Refusal refusal = Refusal::kNone;
	};

	enum class ActionKind : std::uint32_t
	{
		kNone = 0,
		kButton,
		// A slider that was released after an edit; value carries where it landed.
		kSliderRelease,
		// The hold button became active or stopped being active; engaged says which.
		kHoldEdge,
		// A checkbox flipped; engaged is its new state.
		kToggle,
	};

	struct Action
	{
		ActionKind kind = ActionKind::kNone;
		ControlId control = ControlId::kNone;
		double value = 0.0;
		bool engaged = false;
	};

	// --- the reducers -----------------------------------------------------------------

	namespace detail
	{
		struct ModeRow
		{
			ControlId control;
			std::string_view mode;
			std::string_view label;
		};

		inline constexpr ModeRow kModeRows[kModeCount] = {
			{ ControlId::kModeOff, "off"sv, "Off"sv },
			{ ControlId::kModeDlaa, "dlaa"sv, "DLAA"sv },
			{ ControlId::kModeQuality, "quality"sv, "Quality"sv },
			{ ControlId::kModeBalanced, "balanced"sv, "Balanced"sv },
			{ ControlId::kModePerformance, "performance"sv, "Performance"sv },
			{ ControlId::kModeUltraPerformance, "ultraperformance"sv, "Ultra"sv },
		};

		struct PresetRow
		{
			ControlId control;
			std::string_view value;
			std::string_view label;
			std::string_view note;
		};

		// The letters the backend's PresetValue accepts, plus "auto". K carries the one
		// annotation there is a measurement behind: it is the sharper preset, and VERIFIED
		// in the headset on an RTX 4090 at 3760x3996 per eye it costs about two
		// milliseconds a frame against F (DLAA 10.39 ms on F, 12.35 ms on K).
		inline constexpr PresetRow kPresetRows[kPresetCount] = {
			{ ControlId::kPresetAuto, "auto"sv, "auto"sv, ""sv },
			{ ControlId::kPresetF, "F"sv, "F"sv, ""sv },
			{ ControlId::kPresetJ, "J"sv, "J"sv, ""sv },
			{ ControlId::kPresetK, "K"sv, "K"sv, "sharper, about 2 ms slower"sv },
			{ ControlId::kPresetL, "L"sv, "L"sv, ""sv },
			{ ControlId::kPresetM, "M"sv, "M"sv, ""sv },
		};

		[[nodiscard]] constexpr double Clamp(double a_value, double a_low, double a_high) noexcept
		{
			return a_value < a_low ? a_low : (a_value > a_high ? a_high : a_value);
		}
	}

	[[nodiscard]] constexpr Label ModeTag(
		std::string_view a_currentMode,
		std::string_view a_candidate,
		bool a_bracketArmed,
		std::string_view a_sessionArmedMode,
		Settings::Policy::SwitchOutcome& a_outcome,
		bool& a_live) noexcept
	{
		const auto decision = Settings::Policy::ResolveModeSwitch(
			a_currentMode, a_candidate, a_bracketArmed, a_sessionArmedMode);
		a_outcome = decision.outcome;
		// kApplied and kUnchanged are the two outcomes that leave the session running the
		// mode the button names; every other one leaves the file changed and the session
		// where it was, which is what "next launch" means here.
		a_live = decision.outcome == Settings::Policy::SwitchOutcome::kApplied ||
		         decision.outcome == Settings::Policy::SwitchOutcome::kUnchanged;
		Label tag{};
		if (a_live) {
			tag.Append("live"sv);
		} else {
			tag.Append("next launch: "sv);
			tag.Append(Settings::Policy::SwitchOutcomeName(decision.outcome));
		}
		return tag;
	}

	[[nodiscard]] constexpr ResultText ComposeResultLine(const Input& a_input) noexcept
	{
		ResultText line{};
		if (a_input.intentInFlight) {
			line.Append("pending"sv);
			return line;
		}
		if (!a_input.lastResult.present) {
			line.Append("no change requested yet"sv);
			return line;
		}
		line.Append(a_input.lastResult.key);
		line.Append(" = "sv);
		line.Append(a_input.lastResult.value);
		line.Append(a_input.lastResult.accepted ? ": accepted"sv : ": refused"sv);
		line.Append(a_input.lastResult.persisted ? ", persisted"sv : ", not persisted"sv);
		if (a_input.lastResult.tookEffectNow) {
			line.Append(", took effect now"sv);
		}
		if (a_input.lastResult.needsRestart) {
			line.Append(", needs restart"sv);
		}
		if (!a_input.lastResult.reason.empty()) {
			line.Append(" - "sv);
			line.Append(a_input.lastResult.reason);
		}
		return line;
	}

	// What to draw, from what other modules already published. Total: every field of the
	// result is written on every path.
	[[nodiscard]] constexpr Output Reduce(const Input& a_input) noexcept
	{
		Output out{};

		for (std::size_t i = 0; i < kModeCount; ++i) {
			const auto& row = detail::kModeRows[i];
			ModeButton button{};
			button.control = row.control;
			button.mode = row.mode;
			button.label.Append(row.label);
			button.current = a_input.modeSwitch.liveMode == row.mode;
			button.tag = ModeTag(
				a_input.modeSwitch.liveMode,
				row.mode,
				a_input.modeSwitch.bracketArmed,
				a_input.modeSwitch.sessionArmedMode,
				button.outcome,
				button.live);
			out.modes[i] = button;
		}

		// The slider follows renderScaleOverride, which is what it writes. Zero means
		// "the mode's own default", so the live ratio is what the operator is actually
		// looking at and what the handle should start on.
		const double ratioValue = a_input.live.renderScaleOverride > 0.0 ?
		                              a_input.live.renderScaleOverride :
		                              a_input.ratio.requestedScale;
		out.ratio.value = detail::Clamp(ratioValue, kRatioMinimum, kRatioMaximum);
		out.ratio.minimum = kRatioMinimum;
		out.ratio.maximum = kRatioMaximum;
		out.ratio.fullWouldBeRefused =
			Settings::Policy::RenderScaleOverrideIsModeChange(kRatioMaximum, a_input.modeSwitch.liveMode);
		out.ratio.sizeText.AppendUnsigned(a_input.ratio.renderWidth);
		out.ratio.sizeText.Append(" x "sv);
		out.ratio.sizeText.AppendUnsigned(a_input.ratio.renderHeight);
		out.ratio.sizeText.Append("  ->  "sv);
		out.ratio.sizeText.AppendUnsigned(a_input.ratio.outputWidth);
		out.ratio.sizeText.Append(" x "sv);
		out.ratio.sizeText.AppendUnsigned(a_input.ratio.outputHeight);

		for (std::size_t i = 0; i < kPresetCount; ++i) {
			const auto& row = detail::kPresetRows[i];
			PresetEntry entry{};
			entry.control = row.control;
			entry.value = row.value;
			entry.label.Append(row.label);
			entry.note.Append(row.note);
			entry.current = a_input.live.preset == row.value;
			out.presets[i] = entry;
		}

		out.sharpness.value = detail::Clamp(a_input.live.sharpness, kSharpnessMinimum, kSharpnessMaximum);
		out.sharpness.minimum = kSharpnessMinimum;
		out.sharpness.maximum = kSharpnessMaximum;

		out.timersOn = a_input.live.perfTimers;
		out.holdEngaged = a_input.holdEngaged;
		out.latchOn = a_input.latchOn;
		out.vetoActive = a_input.veto.active;
		out.vetoOwnedByPanel = a_input.veto.ownedByPanel;

		out.keysText.Append("toggle "sv);
		out.keysText.AppendSigned(a_input.keys.toggleVk);
		out.keysText.Append("   compare "sv);
		out.keysText.AppendSigned(a_input.keys.compareVk);
		out.keysText.Append("   panel "sv);
		out.keysText.AppendSigned(a_input.keys.panelVk);

		out.resultLine = ComposeResultLine(a_input);
		return out;
	}

	// One input, exactly one intent. The veto pair never touches the worker, so an
	// in-flight SetKey cannot refuse them: an A/B the operator is holding must not depend
	// on a settings write finishing.
	[[nodiscard]] constexpr Intent Decide(const Input& a_input, const Action& a_action) noexcept
	{
		Intent intent{};
		switch (a_action.kind) {
		case ActionKind::kHoldEdge:
			intent.kind = IntentKind::kVetoHold;
			intent.engaged = a_action.engaged;
			return intent;
		case ActionKind::kToggle:
			if (a_action.control == ControlId::kNativeLatch) {
				intent.kind = IntentKind::kVetoLatch;
				intent.engaged = a_action.engaged;
				return intent;
			}
			if (a_action.control == ControlId::kTimers) {
				if (a_input.intentInFlight) {
					intent.refusal = Refusal::kIntentInFlight;
					return intent;
				}
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kTimersKey);
				intent.value.Append(a_action.engaged ? "true"sv : "false"sv);
				return intent;
			}
			intent.refusal = Refusal::kUnknownControl;
			return intent;
		case ActionKind::kButton:
			break;
		case ActionKind::kSliderRelease:
			break;
		case ActionKind::kNone:
		default:
			return intent;
		}

		if (a_input.intentInFlight) {
			intent.refusal = Refusal::kIntentInFlight;
			return intent;
		}

		if (a_action.kind == ActionKind::kSliderRelease) {
			if (a_action.control == ControlId::kRatioSlider) {
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kRatioKey);
				intent.value.AppendFixed(
					detail::Clamp(a_action.value, kRatioMinimum, kRatioMaximum), kRatioDecimals);
				return intent;
			}
			if (a_action.control == ControlId::kSharpnessSlider) {
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kSharpnessKey);
				intent.value.AppendFixed(
					detail::Clamp(a_action.value, kSharpnessMinimum, kSharpnessMaximum), kSharpnessDecimals);
				return intent;
			}
			intent.refusal = Refusal::kUnknownControl;
			return intent;
		}

		for (const auto& row : detail::kModeRows) {
			if (row.control == a_action.control) {
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kModeKey);
				intent.value.Append(row.mode);
				return intent;
			}
		}
		for (const auto& row : detail::kPresetRows) {
			if (row.control == a_action.control) {
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kPresetKey);
				intent.value.Append(row.value);
				return intent;
			}
		}
		intent.refusal = Refusal::kUnknownControl;
		return intent;
	}

	namespace detail
	{
		// A session that started in a reduced mode: its bracket is armed and frozen, so
		// "dlaa" is the one mode it can never reach live.
		[[nodiscard]] consteval Input ArmedQualitySession()
		{
			Input in{};
			in.modeSwitch.liveMode = "quality"sv;
			in.modeSwitch.modeSource = "explicit"sv;
			in.modeSwitch.sessionArmedMode = "quality"sv;
			in.modeSwitch.bracketArmed = true;
			in.live.preset = "F"sv;
			in.live.renderScaleOverride = 0.0;
			in.ratio.requestedScale = 0.6667;
			return in;
		}

		// A session that started native: no bracket was ever armed, so every reduced mode
		// needs a restart and only "dlaa" and "off" are reachable.
		[[nodiscard]] consteval Input UnarmedDlaaSession()
		{
			Input in{};
			in.modeSwitch.liveMode = "dlaa"sv;
			in.modeSwitch.modeSource = "explicit"sv;
			in.modeSwitch.sessionArmedMode = ""sv;
			in.modeSwitch.bracketArmed = false;
			in.live.preset = "F"sv;
			return in;
		}

		// By value, not by reference: an immediate invocation's result has to be a
		// constant expression, and a reference into the caller's own local Output is not
		// one.
		[[nodiscard]] constexpr ModeButton ModeOf(const Output& a_out, ControlId a_control) noexcept
		{
			for (const auto& button : a_out.modes) {
				if (button.control == a_control) {
					return button;
				}
			}
			return a_out.modes[0];
		}

		[[nodiscard]] consteval bool DlaaRefusedOnArmedSessionContract()
		{
			const auto out = Reduce(ArmedQualitySession());
			const auto dlaa = ModeOf(out, ControlId::kModeDlaa);
			return !dlaa.live && dlaa.tag == "next launch: refused-bracket-armed"sv &&
			       dlaa.outcome == Settings::Policy::SwitchOutcome::kRefusedBracketArmed;
		}

		[[nodiscard]] consteval bool ReducedNeedsReducedSessionContract()
		{
			const auto out = Reduce(UnarmedDlaaSession());
			const auto quality = ModeOf(out, ControlId::kModeQuality);
			return !quality.live && quality.tag == "next launch: refused-needs-reduced-session"sv &&
			       quality.outcome == Settings::Policy::SwitchOutcome::kRefusedNeedsReducedSession;
		}

		[[nodiscard]] consteval bool OffIsAlwaysLiveContract()
		{
			// The one mode reachable from every session - see ResolveModeSwitch's own
			// contract for why. Both session shapes, so a future rule that made "off"
			// conditional on the bracket fails here rather than in a headset.
			const auto armed = Reduce(ArmedQualitySession());
			const auto unarmed = Reduce(UnarmedDlaaSession());
			return ModeOf(armed, ControlId::kModeOff).live &&
			       ModeOf(armed, ControlId::kModeOff).tag == "live"sv &&
			       ModeOf(unarmed, ControlId::kModeOff).live &&
			       ModeOf(unarmed, ControlId::kModeOff).tag == "live"sv;
		}

		[[nodiscard]] consteval bool CurrentModeIsLiveContract()
		{
			// The mode already running reads live, not "next launch": clicking it is
			// kUnchanged, which changes nothing and refuses nothing.
			const auto out = Reduce(ArmedQualitySession());
			const auto quality = ModeOf(out, ControlId::kModeQuality);
			return quality.current && quality.live && quality.tag == "live"sv &&
			       quality.outcome == Settings::Policy::SwitchOutcome::kUnchanged;
		}

		[[nodiscard]] consteval bool SecondSetKeyRefusedContract()
		{
			Input in = ArmedQualitySession();
			in.intentInFlight = true;
			const auto button = Decide(in, Action{ ActionKind::kButton, ControlId::kModeOff, 0.0, false });
			const auto slider =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kRatioSlider, 0.58, false });
			return button.kind == IntentKind::kNone && button.refusal == Refusal::kIntentInFlight &&
			       slider.kind == IntentKind::kNone && slider.refusal == Refusal::kIntentInFlight;
		}

		[[nodiscard]] consteval bool VetoIntentsIgnoreInFlightContract()
		{
			Input in = ArmedQualitySession();
			in.intentInFlight = true;
			const auto hold = Decide(in, Action{ ActionKind::kHoldEdge, ControlId::kHoldNative, 0.0, true });
			const auto release = Decide(in, Action{ ActionKind::kHoldEdge, ControlId::kHoldNative, 0.0, false });
			const auto latch = Decide(in, Action{ ActionKind::kToggle, ControlId::kNativeLatch, 0.0, true });
			return hold.kind == IntentKind::kVetoHold && hold.engaged && hold.refusal == Refusal::kNone &&
			       release.kind == IntentKind::kVetoHold && !release.engaged &&
			       release.refusal == Refusal::kNone &&
			       latch.kind == IntentKind::kVetoLatch && latch.engaged && latch.refusal == Refusal::kNone;
		}

		[[nodiscard]] consteval bool RatioFullFlaggedBesideReducedContract()
		{
			return Reduce(ArmedQualitySession()).ratio.fullWouldBeRefused &&
			       !Reduce(UnarmedDlaaSession()).ratio.fullWouldBeRefused;
		}

		[[nodiscard]] consteval bool IntentValuesContract()
		{
			const auto in = ArmedQualitySession();
			const auto mode = Decide(in, Action{ ActionKind::kButton, ControlId::kModeQuality, 0.0, false });
			const auto preset = Decide(in, Action{ ActionKind::kButton, ControlId::kPresetK, 0.0, false });
			const auto ratio =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kRatioSlider, 0.58, false });
			const auto sharp =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kSharpnessSlider, 0.25, false });
			const auto timers = Decide(in, Action{ ActionKind::kToggle, ControlId::kTimers, 0.0, true });
			return mode.kind == IntentKind::kSetKey && mode.key == "mode"sv && mode.value == "quality"sv &&
			       preset.key == "preset"sv && preset.value == "K"sv &&
			       ratio.key == "renderScaleOverride"sv && ratio.value == "0.5800"sv &&
			       sharp.key == "sharpness"sv && sharp.value == "0.250"sv &&
			       timers.key == "perfTimers"sv && timers.value == "true"sv;
		}

		[[nodiscard]] consteval bool RatioSliderClampsContract()
		{
			// A release outside the key's own range is clamped rather than sent: the
			// writer would refuse it, and refusing on the panel's own terms says the same
			// thing without a round trip.
			const auto in = ArmedQualitySession();
			const auto low =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kRatioSlider, 0.1, false });
			const auto high =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kRatioSlider, 1.4, false });
			return low.value == "0.3000"sv && high.value == "1.0000"sv;
		}

		[[nodiscard]] consteval bool ResultLineContract()
		{
			Input pending = ArmedQualitySession();
			pending.intentInFlight = true;
			Input done = ArmedQualitySession();
			done.lastResult = ResultInput{ true, true, true, true, false, "mode"sv, "\"off\""sv, ""sv };
			return Reduce(pending).resultLine == "pending"sv &&
			       Reduce(done).resultLine == "mode = \"off\": accepted, persisted, took effect now"sv &&
			       Reduce(ArmedQualitySession()).resultLine == "no change requested yet"sv;
		}

		[[nodiscard]] consteval bool FixedTextContract()
		{
			Text<32> whole{};
			whole.AppendFixed(1.0, 4);
			Text<32> rounded{};
			rounded.AppendFixed(0.6667, 4);
			Text<32> zero{};
			zero.AppendFixed(0.0, 3);
			Text<32> truncated{};
			truncated.Append("0123456789"sv);
			truncated.Append("0123456789"sv);
			truncated.Append("0123456789"sv);
			truncated.Append("0123456789"sv);
			return whole == "1.0000"sv && rounded == "0.6667"sv && zero == "0.000"sv &&
			       truncated.length == 31;
		}

		static_assert(DlaaRefusedOnArmedSessionContract());
		static_assert(ReducedNeedsReducedSessionContract());
		static_assert(OffIsAlwaysLiveContract());
		static_assert(CurrentModeIsLiveContract());
		static_assert(SecondSetKeyRefusedContract());
		static_assert(VetoIntentsIgnoreInFlightContract());
		static_assert(RatioFullFlaggedBesideReducedContract());
		static_assert(IntentValuesContract());
		static_assert(RatioSliderClampsContract());
		static_assert(ResultLineContract());
		static_assert(FixedTextContract());
		static_assert(ControlName(ControlId::kModeUltraPerformance) == "mode-ultraperformance"sv);
		static_assert(ControlName(ControlId::kNone) == "none"sv);
		static_assert(RefusalName(Refusal::kIntentInFlight) == "intent-in-flight"sv);
		static_assert(IntentKindName(IntentKind::kSetKey) == "set-key"sv);
	}
}
