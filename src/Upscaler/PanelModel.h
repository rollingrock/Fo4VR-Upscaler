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
		kUseModeRatio,
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
		// Last row, and the only control that closes the panel. While the panel is
		// visible SteamVR's laser mode owns the controllers and nothing reaches the game
		// (VERIFIED in the headset), so the pause menu, the Pip-Boy and the panel key are
		// all unreachable from inside the headset - the panel has to be closable by the
		// same laser that opened everything else on it.
		kClose,
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
		case ControlId::kUseModeRatio:
			return "use-mode-ratio"sv;
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
		case ControlId::kClose:
			return "close"sv;
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

	// The step a laser-driven slider writes on. A controller pointed at a panel a metre
	// away carries the whole arm's tremor into the widget: aiming a sharpness slider at
	// 0.7 landed at 0.917 in the headset, and the ratio slider moved to 0.5785 while it
	// was not even the target. A step wide enough to swallow that tremor is what makes
	// the value the operator reads the value they aimed at; below the step the slider is
	// reporting the arm, not the intent.
	inline constexpr double kRatioStep = 0.01;
	inline constexpr double kSharpnessStep = 0.05;

	// How long the button has to read up before a release counts as final. The trigger
	// reads as several releases across one drag - two drags wrote eleven times in the
	// headset - and every one of those was a settings write, a file replace and a whole
	// Configuration::Apply. A new activation of the same slider inside this window
	// cancels the pending write and the drag continues, so the operator's own pause is
	// what ends a drag rather than the trigger's contact bounce.
	inline constexpr double kSliderQuietMs = 200.0;

	// Nearest step, halves upward. Half-up rather than half-even because this is a
	// pointer position an operator aimed at: the nearest step is the only answer that
	// reads as "where I let go", and a tie broken toward the even step would put the
	// handle somewhere they can see it is not. The caller clamps first, so the argument
	// is never negative and the truncation below is a floor.
	[[nodiscard]] constexpr double SnapToStep(double a_value, double a_step) noexcept
	{
		if (a_step <= 0.0) {
			return a_value;
		}
		const auto steps = static_cast<std::int64_t>(a_value / a_step + 0.5);
		return static_cast<double>(steps) * a_step;
	}

	[[nodiscard]] constexpr double ClampTo(double a_value, double a_low, double a_high) noexcept
	{
		return a_value < a_low ? a_low : (a_value > a_high ? a_high : a_value);
	}

	// Clamped, snapped, clamped again: the snap can carry the top of the range past it
	// (0.999 on a 0.01 step is 1.00, which is inside, but a wider step would not be), and
	// a value outside the key's own range is a refusal the panel can answer without a
	// round trip.
	[[nodiscard]] constexpr double SnapRatio(double a_value) noexcept
	{
		return ClampTo(
			SnapToStep(ClampTo(a_value, kRatioMinimum, kRatioMaximum), kRatioStep),
			kRatioMinimum, kRatioMaximum);
	}

	[[nodiscard]] constexpr double SnapSharpness(double a_value) noexcept
	{
		return ClampTo(
			SnapToStep(ClampTo(a_value, kSharpnessMinimum, kSharpnessMaximum), kSharpnessStep),
			kSharpnessMinimum, kSharpnessMaximum);
	}

	// The snap a control owns, so the slider latch below and Decide cannot disagree about
	// what a release is worth. A control that is not a slider snaps nothing.
	[[nodiscard]] constexpr double SnapForControl(ControlId a_control, double a_value) noexcept
	{
		switch (a_control) {
		case ControlId::kRatioSlider:
			return SnapRatio(a_value);
		case ControlId::kSharpnessSlider:
			return SnapSharpness(a_value);
		default:
			return a_value;
		}
	}

	// --- where the panel hangs in the room ---------------------------------------------
	//
	// The arithmetic behind the room-fixed placement, lifted out of Panel.cpp so it can be
	// proved rather than only read: twelve numbers in, twelve out, no OpenVR type and no
	// engine type anywhere near it. Panel.cpp copies the pose in, copies the result into an
	// HmdMatrix34_t and hands that to SetOverlayTransformAbsolute; every rule about where
	// the panel ends up lives here, with a contract, because the one arm that can exercise
	// it headlessly runs against a synthetic pose at yaw zero - where the correct matrix and
	// three wrong ones are the same matrix.
	//
	// The convention is OpenVR's own (openvr.h 34-42): a row-major 3x4 with the translation
	// in column 3, +X right, +Y up, -Z forward, right-handed. A device's forward is
	// therefore its negated third column and its up is its second.
	struct Matrix34
	{
		double m[3][4]{};
	};

	// How far below the eye the panel hangs, and how short a horizontal component counts as
	// no heading at all. One drop for both anchors: the head-relative fall-back has to put
	// the panel where the room placement would have at the moment of the show, or the two
	// anchors would not be the same panel in the same place.
	inline constexpr double kPanelDropMeters = -0.15;
	inline constexpr double kHeadingDegenerateLength = 1.0e-4;

	// Which vector the panel's heading was taken from.
	enum class Heading : std::uint32_t
	{
		// The ordinary case: the head's own forward, flattened onto the floor.
		kGaze = 0,
		// The gaze is vertical and carries no heading at all. The head's up axis does - it
		// lies in the sagittal plane, so flattened it is the body's own facing - but it
		// points behind the operator when they are looking up and in front of them when
		// they are looking down, so it is flipped by the sign of the gaze's own y. That
		// flip is the limit of the gaze heading as the head tilts toward vertical, which is
		// the pose an operator actually arrives from; without it a panel opened while
		// looking straight up is placed behind them.
		kHeadUp,
		// Neither carried a heading, which means the pose was not a rotation. The
		// universe's own -Z stands in, so the panel is somewhere rather than nowhere.
		kUniverseForward,
	};

	// A square root that is a constant expression, because the basis below has to be one and
	// std::sqrt is not (and <cmath> is not a dependency this header will take). Newton from
	// a seed of max(value, 1), returning as soon as the iterate stops moving and bounded by
	// a count so the function is total on every input including a NaN. Over what it is asked
	// for here - the length of a row of a rotation - that convergence is a handful of steps.
	[[nodiscard]] constexpr double Sqrt(double a_value) noexcept
	{
		if (!(a_value > 0.0)) {
			return 0.0;
		}
		double guess = a_value > 1.0 ? a_value : 1.0;
		for (int i = 0; i < 64; ++i) {
			const double next = 0.5 * (guess + a_value / guess);
			if (next == guess) {
				return guess;
			}
			guess = next;
		}
		return guess;
	}

	struct RoomPlacement
	{
		Matrix34 transform{};
		// The heading the panel faces, as a unit vector on the floor plane. Panel.cpp turns
		// it into the reported yaw; nothing here needs an angle, and an atan2 is not a
		// constant expression either.
		double forwardX = 0.0;
		double forwardZ = -1.0;
		Heading heading = Heading::kGaze;
	};

	// Where the panel goes, given where the head is. The position is the head's own,
	// a_distanceMeters along its forward flattened onto the floor, a_dropMeters below the
	// eye. The rotation is upright and facing that heading: the overlay's +Z is its front
	// face, so its z axis is the negated flattened forward, its y axis is the universe's up
	// and its x axis is y cross z. Pitch and roll of the head are discarded on purpose - a
	// panel that inherited them would hang at whatever angle the head happened to be at
	// when it was shown, and the operator asked for one that stands in the room.
	[[nodiscard]] constexpr RoomPlacement ComposeRoomPlacement(
		const Matrix34& a_head, double a_distanceMeters, double a_dropMeters) noexcept
	{
		RoomPlacement out{};
		double forwardX = -a_head.m[0][2];
		const double forwardY = -a_head.m[1][2];
		double forwardZ = -a_head.m[2][2];
		double length = Sqrt(forwardX * forwardX + forwardZ * forwardZ);
		if (length < kHeadingDegenerateLength) {
			const double flip = forwardY > 0.0 ? -1.0 : 1.0;
			forwardX = flip * a_head.m[0][1];
			forwardZ = flip * a_head.m[2][1];
			length = Sqrt(forwardX * forwardX + forwardZ * forwardZ);
			out.heading = Heading::kHeadUp;
		}
		if (length < kHeadingDegenerateLength) {
			forwardX = 0.0;
			forwardZ = -1.0;
			length = 1.0;
			out.heading = Heading::kUniverseForward;
		}
		forwardX /= length;
		forwardZ /= length;
		out.forwardX = forwardX;
		out.forwardZ = forwardZ;
		out.transform.m[0][0] = -forwardZ;
		out.transform.m[1][0] = 0.0;
		out.transform.m[2][0] = forwardX;
		out.transform.m[0][1] = 0.0;
		out.transform.m[1][1] = 1.0;
		out.transform.m[2][1] = 0.0;
		out.transform.m[0][2] = -forwardX;
		out.transform.m[1][2] = 0.0;
		out.transform.m[2][2] = -forwardZ;
		out.transform.m[0][3] = a_head.m[0][3] + forwardX * a_distanceMeters;
		out.transform.m[1][3] = a_head.m[1][3] + a_dropMeters;
		out.transform.m[2][3] = a_head.m[2][3] + forwardZ * a_distanceMeters;
		return out;
	}

	// --- inputs ----------------------------------------------------------------------

	// Why an input produced no intent. It is both an output of Decide() and an input to
	// the next frame's Reduce() - the line an operator reads has to be able to say a
	// click was refused - which is why it is declared here, above the inputs, rather
	// than beside the intents it also belongs to.
	enum class Refusal : std::uint32_t
	{
		kNone = 0,
		// The worker still owns one intent and another is already queued behind it. One
		// pending intent is kept because a laser trigger reads as more presses than the
		// operator made - four of thirteen presses were refused as in-flight in the
		// headset - and a click the operator meant is worth a frame of latency. A third
		// is still refused rather than stacked: a queue deeper than one stops being a
		// double click and starts being a script nobody typed.
		kIntentInFlight,
		kUnknownControl,
		// No settings worker is running at all, so nothing this panel writes can ever
		// land this session. Decided outside this header - Decide() cannot know it, the
		// thread either started or it did not - and reported back in through
		// Input::workerAvailable so the drawn result line says it rather than leaving the
		// operator clicking a control that answers nothing.
		kWorkerUnavailable,
	};

	[[nodiscard]] constexpr std::string_view RefusalName(Refusal a_refusal) noexcept
	{
		switch (a_refusal) {
		case Refusal::kIntentInFlight:
			return "intent-in-flight"sv;
		case Refusal::kUnknownControl:
			return "unknown-control"sv;
		case Refusal::kWorkerUnavailable:
			return "worker-unavailable"sv;
		case Refusal::kNone:
		default:
			return "none"sv;
		}
	}

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
		// The canonical TOML literal the writer persisted - and empty on every refusal,
		// because a refused value was never canonicalised. The result line therefore
		// falls back to requestedValue: the one line an operator reads when a change is
		// refused must name the value they asked for, not an empty space where it was.
		std::string_view value;
		// What the intent actually carried to Settings::Writer::Set, kept by the worker
		// beside the result so the fallback above needs no second source.
		std::string_view requestedValue;
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
		// True while the worker still owns an intent, and true while one is queued behind
		// it. Together they decide whether a click runs now, waits, coalesces or is
		// refused, and either of them is what the result line reads as "pending" - a
		// queued write has not happened yet and saying otherwise would report the write
		// before last as the answer to this click.
		bool intentInFlight = false;
		bool intentQueued = false;
		// The key the queued intent carries. A second click on the same key replaces its
		// value rather than being refused: two presses of one button mean the later one,
		// and writing both would send the file through a value the operator passed over.
		std::string_view queuedKey;
		// False when no settings worker is running - the thread failed to start, or it
		// has not been started yet. A panel whose writes cannot land has to say so on the
		// line the operator reads, not only in /state.
		bool workerAvailable = true;
		// The last action that produced no intent, and whether it happened after the last
		// completed result. Both are needed: a refusal that is older than the result on
		// screen is not what last happened to a write, and showing it would report a
		// stale event as the current one. The seam decides "newer" from one monotonic
		// counter both sides stamp; this header only reads the answer.
		Refusal lastRefusal = Refusal::kNone;
		bool refusalIsNewer = false;
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
		// Whether renderScaleOverride is standing. An explicit ratio outlives a mode
		// click - a quality session ran at 0.627 after the slider was moved, and a
		// quality launch after a dlaa session still ran at the ratio the dlaa session
		// wrote - so the row says so, and the use-mode-ratio button beside it is how the
		// operator gives the mode its own ratio back.
		bool overrideStanding = false;
		Label note;
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
		// The show intent, cleared from the seam. Not a settings write and never queued:
		// the one control that closes a panel which is holding every controller input in
		// the room cannot be the one that waits behind a file replace.
		kHidePanel,
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
		case IntentKind::kHidePanel:
			return "hide-panel"sv;
		case IntentKind::kNone:
		default:
			return "none"sv;
		}
	}

	// Where a settings intent goes. The seam owns the one-deep queue; this says which of
	// its three doors the intent takes, so the policy stays here and Panel.cpp stays a
	// dispatcher.
	enum class Disposition : std::uint32_t
	{
		// The worker's slot is free: post it now.
		kImmediate = 0,
		// The slot is busy and nothing is waiting: hold it and post it when the slot
		// frees.
		kQueued,
		// The slot is busy and the same key is already waiting: replace the waiting
		// value with this one.
		kCoalesced,
	};

	[[nodiscard]] constexpr std::string_view DispositionName(Disposition a_disposition) noexcept
	{
		switch (a_disposition) {
		case Disposition::kQueued:
			return "queued"sv;
		case Disposition::kCoalesced:
			return "coalesced"sv;
		case Disposition::kImmediate:
		default:
			return "immediate"sv;
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
		// Meaningful only for kSetKey; every other kind runs straight off the seam.
		Disposition disposition = Disposition::kImmediate;
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

	// --- one write per drag -------------------------------------------------------------
	//
	// The widget state one frame observed, as the draw saw it. activeControl is the slider
	// the pointer is currently holding, releasedControl the one whose value changed and
	// whose button came up on this frame; both are kNone on an ordinary frame.
	struct SliderFrame
	{
		ControlId activeControl = ControlId::kNone;
		ControlId releasedControl = ControlId::kNone;
		double releasedValue = 0.0;
	};

	// The write waiting out its quiet window. Owned by the seam, advanced only through
	// AdvanceSlider, and carried across frames: this is the whole state a drag has.
	struct SliderLatch
	{
		ControlId control = ControlId::kNone;
		double value = 0.0;
		double quietMs = 0.0;
		bool pending = false;
	};

	struct SliderStep
	{
		SliderLatch latch;
		// kNone until the window elapses; one kSliderRelease action when it does.
		Action fire;
	};

	// The timer is a frame-time accumulator rather than a clock read, so this stays pure:
	// the seam already knows how long its own frame took and hands it in. One laser drives
	// one slider, so a release of a different slider takes the latch from whatever was
	// waiting - but it does not throw that write away: a drag the operator finished and a
	// second slider they moved a tenth of a second later are two values they set, the two
	// carry different keys so the one-deep queue absorbs the pair, and silently dropping
	// one would be the opposite of the rule the queue exists for.
	[[nodiscard]] constexpr SliderStep AdvanceSlider(
		SliderLatch a_latch, const SliderFrame& a_frame, double a_elapsedMs) noexcept
	{
		SliderStep step{};
		step.latch = a_latch;
		// A new activation of the slider that is waiting cancels its write: the operator
		// is still dragging and the release that started this window was the trigger's,
		// not theirs.
		if (step.latch.pending && a_frame.activeControl == step.latch.control) {
			step.latch.pending = false;
			step.latch.quietMs = 0.0;
		}
		if (a_frame.releasedControl != ControlId::kNone) {
			// The other slider's pending write, finished here rather than overwritten. Not
			// the same control: a second release of the slider already waiting is the same
			// drag continuing, and its later value replaces the earlier one - which is what
			// one write per drag means.
			if (step.latch.pending && step.latch.control != a_frame.releasedControl) {
				step.fire =
					Action{ ActionKind::kSliderRelease, step.latch.control, step.latch.value, false };
			}
			step.latch.pending = true;
			step.latch.control = a_frame.releasedControl;
			step.latch.value = SnapForControl(a_frame.releasedControl, a_frame.releasedValue);
			step.latch.quietMs = 0.0;
			return step;
		}
		if (step.latch.pending) {
			step.latch.quietMs += a_elapsedMs;
			if (step.latch.quietMs >= kSliderQuietMs) {
				step.fire = Action{ ActionKind::kSliderRelease, step.latch.control, step.latch.value, false };
				step.latch.pending = false;
				step.latch.quietMs = 0.0;
			}
		}
		return step;
	}

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

	// The one line the operator reads. Five conditions in the order the operator needs
	// them: a session that can never write at all comes before anything about a single
	// write, a write still running comes before its own stale predecessor, and a refusal
	// is shown only while it is the newest thing that happened to a write.
	[[nodiscard]] constexpr ResultText ComposeResultLine(const Input& a_input) noexcept
	{
		ResultText line{};
		if (!a_input.workerAvailable) {
			line.Append("settings worker unavailable - this session cannot write settings from the panel; see Fo4VRUpscaler.log"sv);
			return line;
		}
		if (a_input.intentInFlight || a_input.intentQueued) {
			line.Append("pending"sv);
			return line;
		}
		if (a_input.lastRefusal != Refusal::kNone && a_input.refusalIsNewer) {
			line.Append("refused: "sv);
			line.Append(RefusalName(a_input.lastRefusal));
			return line;
		}
		if (!a_input.lastResult.present) {
			line.Append("no change requested yet"sv);
			return line;
		}
		line.Append(a_input.lastResult.key);
		line.Append(" = "sv);
		// The writer canonicalises on acceptance and returns nothing on a refusal, so the
		// requested text stands in - see ResultInput::value.
		line.Append(a_input.lastResult.value.empty() ? a_input.lastResult.requestedValue :
													   a_input.lastResult.value);
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
		out.ratio.overrideStanding = a_input.live.renderScaleOverride > 0.0;
		if (out.ratio.overrideStanding) {
			out.ratio.note.Append("override standing"sv);
		}
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

	// The admission rule, applied once a settings intent's key is known. The queue is one
	// deep; a second click on the key already waiting replaces its value; anything past
	// that is refused by name.
	[[nodiscard]] constexpr Intent Admit(const Input& a_input, Intent a_intent) noexcept
	{
		if (a_intent.kind != IntentKind::kSetKey) {
			return a_intent;
		}
		if (!a_input.intentInFlight) {
			a_intent.disposition = Disposition::kImmediate;
			return a_intent;
		}
		if (!a_input.intentQueued) {
			a_intent.disposition = Disposition::kQueued;
			return a_intent;
		}
		if (a_input.queuedKey == a_intent.key.View()) {
			a_intent.disposition = Disposition::kCoalesced;
			return a_intent;
		}
		Intent refused{};
		refused.refusal = Refusal::kIntentInFlight;
		return refused;
	}

	// One input, exactly one intent. The veto pair and the close button never touch the
	// worker, so a settings write in flight cannot refuse them: an A/B the operator is
	// holding must not depend on a file replace finishing, and neither must the only
	// control that gives the controllers back to the game.
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
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kTimersKey);
				intent.value.Append(a_action.engaged ? "true"sv : "false"sv);
				return Admit(a_input, intent);
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

		if (a_action.kind == ActionKind::kSliderRelease) {
			if (a_action.control == ControlId::kRatioSlider) {
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kRatioKey);
				intent.value.AppendFixed(SnapRatio(a_action.value), kRatioDecimals);
				return Admit(a_input, intent);
			}
			if (a_action.control == ControlId::kSharpnessSlider) {
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kSharpnessKey);
				intent.value.AppendFixed(SnapSharpness(a_action.value), kSharpnessDecimals);
				return Admit(a_input, intent);
			}
			intent.refusal = Refusal::kUnknownControl;
			return intent;
		}

		if (a_action.control == ControlId::kClose) {
			intent.kind = IntentKind::kHidePanel;
			return intent;
		}
		if (a_action.control == ControlId::kUseModeRatio) {
			intent.kind = IntentKind::kSetKey;
			intent.key.Append(kRatioKey);
			// 0 is the key's own "no override" value - Settings::Validate accepts it
			// verbatim and every other value in 0.30..1.00 - so this is an unset, not a
			// ratio, and the mode's own ratio is what the next apply derives.
			intent.value.Append("0"sv);
			return Admit(a_input, intent);
		}
		for (const auto& row : detail::kModeRows) {
			if (row.control == a_action.control) {
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kModeKey);
				intent.value.Append(row.mode);
				return Admit(a_input, intent);
			}
		}
		for (const auto& row : detail::kPresetRows) {
			if (row.control == a_action.control) {
				intent.kind = IntentKind::kSetKey;
				intent.key.Append(kPresetKey);
				intent.value.Append(row.value);
				return Admit(a_input, intent);
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

		// A click that lands while one write is running is kept, not turned away: the
		// laser's own double click is the case this exists for.
		[[nodiscard]] consteval bool SecondSetKeyQueuedContract()
		{
			Input in = ArmedQualitySession();
			in.intentInFlight = true;
			const auto button = Decide(in, Action{ ActionKind::kButton, ControlId::kModeOff, 0.0, false });
			const auto slider =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kRatioSlider, 0.58, false });
			return button.kind == IntentKind::kSetKey && button.disposition == Disposition::kQueued &&
			       button.refusal == Refusal::kNone && button.value == "off"sv &&
			       slider.kind == IntentKind::kSetKey && slider.disposition == Disposition::kQueued;
		}

		// A third is refused by name. One deep is a double click; deeper is a script.
		[[nodiscard]] consteval bool ThirdSetKeyRefusedContract()
		{
			Input in = ArmedQualitySession();
			in.intentInFlight = true;
			in.intentQueued = true;
			in.queuedKey = "mode"sv;
			const auto other = Decide(in, Action{ ActionKind::kButton, ControlId::kPresetK, 0.0, false });
			return other.kind == IntentKind::kNone && other.refusal == Refusal::kIntentInFlight;
		}

		// Two presses of one button mean the later one. Writing both would send the file
		// through a value the operator passed over on the way.
		[[nodiscard]] consteval bool CoalesceSameKeyContract()
		{
			Input in = ArmedQualitySession();
			in.intentInFlight = true;
			in.intentQueued = true;
			in.queuedKey = "mode"sv;
			const auto again =
				Decide(in, Action{ ActionKind::kButton, ControlId::kModeBalanced, 0.0, false });
			return again.kind == IntentKind::kSetKey && again.disposition == Disposition::kCoalesced &&
			       again.key == "mode"sv && again.value == "balanced"sv && again.refusal == Refusal::kNone;
		}

		// A queued write has not happened yet, so the line says pending rather than
		// showing the answer to the click before last as the answer to this one.
		[[nodiscard]] consteval bool QueuedReadsPendingContract()
		{
			Input queued = ArmedQualitySession();
			queued.lastResult.present = true;
			queued.lastResult.accepted = true;
			queued.lastResult.key = "mode"sv;
			queued.lastResult.value = "\"off\""sv;
			queued.intentQueued = true;
			return Reduce(queued).resultLine == "pending"sv;
		}

		// The two controls that must never wait behind a settings write: an A/B the
		// operator is holding, and the button that gives the controllers back.
		[[nodiscard]] consteval bool CloseIsNeverQueuedContract()
		{
			Input in = ArmedQualitySession();
			in.intentInFlight = true;
			in.intentQueued = true;
			in.queuedKey = "mode"sv;
			const auto close = Decide(in, Action{ ActionKind::kButton, ControlId::kClose, 0.0, false });
			return close.kind == IntentKind::kHidePanel && close.refusal == Refusal::kNone;
		}

		// The button that gives the mode its own ratio back writes the key's unset value,
		// not a ratio of the panel's choosing.
		[[nodiscard]] consteval bool UseModeRatioUnsetsContract()
		{
			const auto intent = Decide(
				ArmedQualitySession(), Action{ ActionKind::kButton, ControlId::kUseModeRatio, 0.0, false });
			return intent.kind == IntentKind::kSetKey && intent.key == "renderScaleOverride"sv &&
			       intent.value == "0"sv;
		}

		// The row says an explicit ratio is standing only while one is, and says it in the
		// words the panel draws.
		[[nodiscard]] consteval bool RatioOverrideStandingContract()
		{
			Input standing = ArmedQualitySession();
			standing.live.renderScaleOverride = 0.58;
			const auto withOverride = Reduce(standing);
			const auto without = Reduce(ArmedQualitySession());
			return withOverride.ratio.overrideStanding && withOverride.ratio.note == "override standing"sv &&
			       !without.ratio.overrideStanding && without.ratio.note.length == 0;
		}

		// The rounding rule, through the text the writer actually receives: the ratio
		// slider on 0.01 and the sharpness slider on 0.05, nearest step, halves upward.
		[[nodiscard]] consteval bool SliderSnapContract()
		{
			const auto in = ArmedQualitySession();
			const auto ratio =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kRatioSlider, 0.6271, false });
			const auto sharpDown =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kSharpnessSlider, 0.917, false });
			const auto sharpUp =
				Decide(in, Action{ ActionKind::kSliderRelease, ControlId::kSharpnessSlider, 0.93, false });
			return ratio.value == "0.6300"sv && sharpDown.value == "0.900"sv && sharpUp.value == "0.950"sv;
		}

		// The trigger's chatter, at the cadence the headset produced it: a release, a new
		// activation 50 ms later, a second release, then quiet. One write, carrying the
		// second release's value.
		[[nodiscard]] consteval bool SliderOneWritePerDragContract()
		{
			SliderLatch latch{};
			int fires = 0;
			Action last{};
			const auto advance = [&](const SliderFrame& a_frame, double a_ms) {
				const auto step = AdvanceSlider(latch, a_frame, a_ms);
				latch = step.latch;
				if (step.fire.kind != ActionKind::kNone) {
					++fires;
					last = step.fire;
				}
			};
			advance(SliderFrame{ ControlId::kNone, ControlId::kRatioSlider, 0.7031 }, 11.0);
			advance(SliderFrame{}, 50.0);
			advance(SliderFrame{ ControlId::kRatioSlider, ControlId::kNone, 0.0 }, 11.0);
			advance(SliderFrame{ ControlId::kNone, ControlId::kRatioSlider, 0.6271 }, 11.0);
			for (int i = 0; i < 4; ++i) {
				advance(SliderFrame{}, 60.0);
			}
			for (int i = 0; i < 4; ++i) {
				advance(SliderFrame{}, 60.0);
			}
			const auto intent = Decide(ArmedQualitySession(), last);
			return fires == 1 && last.control == ControlId::kRatioSlider && intent.value == "0.6300"sv;
		}

		// And nothing fires before the window is out: a release read at 190 ms is still a
		// drag the operator has not finished.
		[[nodiscard]] consteval bool SliderWaitsOutTheWindowContract()
		{
			SliderLatch latch{};
			auto step = AdvanceSlider(latch, SliderFrame{ ControlId::kNone, ControlId::kSharpnessSlider, 0.917 }, 11.0);
			latch = step.latch;
			step = AdvanceSlider(latch, SliderFrame{}, 190.0);
			const bool quiet = step.fire.kind == ActionKind::kNone && step.latch.pending;
			step = AdvanceSlider(step.latch, SliderFrame{}, 20.0);
			return quiet && step.fire.kind == ActionKind::kSliderRelease &&
			       step.fire.control == ControlId::kSharpnessSlider && !step.latch.pending;
		}

		// And a release of the other slider inside that window is a write, not a loss: it
		// fires the one that was waiting and takes the latch itself. Two keys, so the
		// one-deep queue in front of the worker carries both.
		[[nodiscard]] consteval bool SliderCrossReleaseFiresBothContract()
		{
			SliderLatch latch{};
			auto step =
				AdvanceSlider(latch, SliderFrame{ ControlId::kNone, ControlId::kRatioSlider, 0.6271 }, 11.0);
			const bool nothingYet = step.fire.kind == ActionKind::kNone;
			step = AdvanceSlider(step.latch, SliderFrame{}, 50.0);
			// The sharpness slider is released fifty milliseconds later, well inside the
			// ratio slider's own quiet window.
			step = AdvanceSlider(
				step.latch, SliderFrame{ ControlId::kNone, ControlId::kSharpnessSlider, 0.917 }, 11.0);
			const auto ratioIntent = Decide(ArmedQualitySession(), step.fire);
			const bool ratioFired = step.fire.kind == ActionKind::kSliderRelease &&
			                        step.fire.control == ControlId::kRatioSlider &&
			                        ratioIntent.key == "renderScaleOverride"sv &&
			                        ratioIntent.value == "0.6300"sv;
			const bool sharpnessWaiting =
				step.latch.pending && step.latch.control == ControlId::kSharpnessSlider;
			step = AdvanceSlider(step.latch, SliderFrame{}, 210.0);
			const auto sharpnessIntent = Decide(ArmedQualitySession(), step.fire);
			return nothingYet && ratioFired && sharpnessWaiting &&
			       step.fire.kind == ActionKind::kSliderRelease &&
			       step.fire.control == ControlId::kSharpnessSlider &&
			       sharpnessIntent.key == "sharpness"sv && sharpnessIntent.value == "0.900"sv;
		}

		// --- the room placement -------------------------------------------------------
		//
		// A tolerance rather than equality: the basis divides by a Newton square root, so
		// the numbers below are the geometry to well past what a metre of overlay can show,
		// not bit patterns.
		[[nodiscard]] constexpr bool NearlyEqual(double a_left, double a_right, double a_tolerance) noexcept
		{
			const double difference = a_left - a_right;
			return (difference < 0.0 ? -difference : difference) <= a_tolerance;
		}

		[[nodiscard]] constexpr bool TransformIs(
			const Matrix34& a_actual, const Matrix34& a_expected) noexcept
		{
			for (int row = 0; row < 3; ++row) {
				for (int column = 0; column < 4; ++column) {
					if (!NearlyEqual(a_actual.m[row][column], a_expected.m[row][column], 1.0e-9)) {
						return false;
					}
				}
			}
			return true;
		}

		// A head at 1.7 m looking straight down the universe's -Z: the panel is a metre
		// ahead, 0.15 m below the eye, and its rotation is the identity - which is exactly
		// what the head-relative transform this replaced produced for the same pose, and is
		// why the two anchors are the same panel in the same place.
		[[nodiscard]] consteval bool RoomPlacementLevelHeadContract()
		{
			Matrix34 head{};
			head.m[0][0] = 1.0;
			head.m[1][1] = 1.0;
			head.m[2][2] = 1.0;
			head.m[1][3] = 1.7;
			const auto placed = ComposeRoomPlacement(head, 1.0, kPanelDropMeters);
			Matrix34 expected{};
			expected.m[0][0] = 1.0;
			expected.m[1][1] = 1.0;
			expected.m[2][2] = 1.0;
			expected.m[1][3] = 1.55;
			expected.m[2][3] = -1.0;
			return TransformIs(placed.transform, expected) && placed.heading == Heading::kGaze &&
			       NearlyEqual(placed.forwardX, 0.0, 1.0e-9) && NearlyEqual(placed.forwardZ, -1.0, 1.0e-9);
		}

		// The same head yawed ninety degrees, standing at (2, 1.6, -3), a metre and a
		// quarter away. Its forward is -X, so the panel is at x - 1.25 and its own x axis
		// is -Z: a sign error in either the x or the z column moves the panel's face and
		// this is the contract that sees it. Yaw zero cannot - there the correct basis and
		// three wrong ones are all the identity.
		[[nodiscard]] consteval Matrix34 TurnedHeadPlacement()
		{
			Matrix34 expected{};
			expected.m[0][2] = 1.0;
			expected.m[1][1] = 1.0;
			expected.m[2][0] = -1.0;
			expected.m[0][3] = 0.75;
			expected.m[1][3] = 1.45;
			expected.m[2][3] = -3.0;
			return expected;
		}

		[[nodiscard]] consteval bool RoomPlacementTurnedHeadContract()
		{
			Matrix34 head{};
			// R_y(90): +X right becomes -Z, forward becomes -X.
			head.m[0][2] = 1.0;
			head.m[1][1] = 1.0;
			head.m[2][0] = -1.0;
			head.m[0][3] = 2.0;
			head.m[1][3] = 1.6;
			head.m[2][3] = -3.0;
			const auto placed = ComposeRoomPlacement(head, 1.25, kPanelDropMeters);
			return TransformIs(placed.transform, TurnedHeadPlacement()) &&
			       placed.heading == Heading::kGaze && NearlyEqual(placed.forwardX, -1.0, 1.0e-9) &&
			       NearlyEqual(placed.forwardZ, 0.0, 1.0e-9);
		}

		// The same yaw with thirty degrees of pitch and forty-five of roll on top, from the
		// same position: the same panel, in the same place, standing upright. Pitch changes
		// the third column and roll changes the first two, and neither may reach the result
		// - a panel that inherited them would hang at the angle the head happened to be at.
		[[nodiscard]] consteval bool RoomPlacementDiscardsPitchAndRollContract()
		{
			constexpr double kSin30 = 0.5;
			constexpr double kCos30 = 0.8660254037844386;
			constexpr double kRoot2Over2 = 0.7071067811865476;
			Matrix34 head{};
			// R_y(90) * R_x(30) * R_z(45), column by column.
			head.m[0][0] = kSin30 * kRoot2Over2;
			head.m[1][0] = kCos30 * kRoot2Over2;
			head.m[2][0] = -kRoot2Over2;
			head.m[0][1] = kSin30 * kRoot2Over2;
			head.m[1][1] = kCos30 * kRoot2Over2;
			head.m[2][1] = kRoot2Over2;
			head.m[0][2] = kCos30;
			head.m[1][2] = -kSin30;
			head.m[2][2] = 0.0;
			head.m[0][3] = 2.0;
			head.m[1][3] = 1.6;
			head.m[2][3] = -3.0;
			const auto placed = ComposeRoomPlacement(head, 1.25, kPanelDropMeters);
			return TransformIs(placed.transform, TurnedHeadPlacement()) &&
			       placed.heading == Heading::kGaze;
		}

		// A gaze with no heading at all, both ways. Looking straight up the head's own up
		// axis points behind the operator and looking straight down it points in front of
		// them, so the flip by the gaze's y is what makes both of them place the panel
		// where looking level would have - which is the limit of the gaze heading as the
		// head tilts toward vertical, and the pose an operator arrives from.
		[[nodiscard]] consteval bool RoomPlacementVerticalGazeContract()
		{
			Matrix34 up{};
			// R_x(+90): forward becomes +Y, up becomes +Z.
			up.m[0][0] = 1.0;
			up.m[1][2] = -1.0;
			up.m[2][1] = 1.0;
			up.m[1][3] = 1.7;
			Matrix34 down{};
			// R_x(-90): forward becomes -Y, up becomes -Z.
			down.m[0][0] = 1.0;
			down.m[1][2] = 1.0;
			down.m[2][1] = -1.0;
			down.m[1][3] = 1.7;
			Matrix34 expected{};
			expected.m[0][0] = 1.0;
			expected.m[1][1] = 1.0;
			expected.m[2][2] = 1.0;
			expected.m[1][3] = 1.55;
			expected.m[2][3] = -1.0;
			const auto lookingUp = ComposeRoomPlacement(up, 1.0, kPanelDropMeters);
			const auto lookingDown = ComposeRoomPlacement(down, 1.0, kPanelDropMeters);
			return TransformIs(lookingUp.transform, expected) &&
			       lookingUp.heading == Heading::kHeadUp &&
			       TransformIs(lookingDown.transform, expected) &&
			       lookingDown.heading == Heading::kHeadUp;
		}

		// A pose that is not a rotation - every axis zero, which is what an uninitialised
		// matrix reads as. The universe's own -Z stands in, so the panel is placed a metre
		// from wherever the position says rather than at the origin of nothing.
		[[nodiscard]] consteval bool RoomPlacementDegeneratePoseContract()
		{
			Matrix34 head{};
			head.m[0][3] = 1.0;
			head.m[1][3] = 2.0;
			head.m[2][3] = 3.0;
			const auto placed = ComposeRoomPlacement(head, 2.0, kPanelDropMeters);
			Matrix34 expected{};
			expected.m[0][0] = 1.0;
			expected.m[1][1] = 1.0;
			expected.m[2][2] = 1.0;
			expected.m[0][3] = 1.0;
			expected.m[1][3] = 1.85;
			expected.m[2][3] = 1.0;
			return TransformIs(placed.transform, expected) &&
			       placed.heading == Heading::kUniverseForward;
		}

		[[nodiscard]] consteval bool SqrtContract()
		{
			return NearlyEqual(Sqrt(4.0), 2.0, 1.0e-12) && NearlyEqual(Sqrt(2.0), 1.4142135623730951, 1.0e-12) &&
			       NearlyEqual(Sqrt(0.75), 0.8660254037844386, 1.0e-12) && Sqrt(0.0) == 0.0 &&
			       Sqrt(-1.0) == 0.0;
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
			done.lastResult.present = true;
			done.lastResult.accepted = true;
			done.lastResult.persisted = true;
			done.lastResult.tookEffectNow = true;
			done.lastResult.key = "mode"sv;
			done.lastResult.value = "\"off\""sv;
			done.lastResult.requestedValue = "off"sv;
			return Reduce(pending).resultLine == "pending"sv &&
			       Reduce(done).resultLine == "mode = \"off\": accepted, persisted, took effect now"sv &&
			       Reduce(ArmedQualitySession()).resultLine == "no change requested yet"sv;
		}

		// A refused write returns no canonical value, and the line an operator reads on a
		// refusal is the one that most needs to name what they asked for.
		[[nodiscard]] consteval bool RefusedValueFallsBackContract()
		{
			Input refused = ArmedQualitySession();
			refused.lastResult.present = true;
			refused.lastResult.key = "renderScaleOverride"sv;
			refused.lastResult.value = ""sv;
			refused.lastResult.requestedValue = "1.0000"sv;
			refused.lastResult.reason = "not a value this configuration keeps"sv;
			return Reduce(refused).resultLine ==
			       "renderScaleOverride = 1.0000: refused, not persisted - not a value this configuration keeps"sv;
		}

		// A panel whose worker never started says so on the line, on every frame, whatever
		// else is or is not on it - including a result an earlier session state left
		// behind and an in-flight flag that can no longer be cleared by anyone.
		[[nodiscard]] consteval bool WorkerUnavailableContract()
		{
			Input down = ArmedQualitySession();
			down.workerAvailable = false;
			Input downWithResult = down;
			downWithResult.lastResult.present = true;
			downWithResult.lastResult.accepted = true;
			downWithResult.lastResult.key = "mode"sv;
			downWithResult.lastResult.value = "\"off\""sv;
			Input downInFlight = down;
			downInFlight.intentInFlight = true;
			const auto expected =
				"settings worker unavailable - this session cannot write settings from the panel; see Fo4VRUpscaler.log"sv;
			return Reduce(down).resultLine == expected &&
			       Reduce(downWithResult).resultLine == expected &&
			       Reduce(downInFlight).resultLine == expected;
		}

		// A refusal is what last happened to a write only while nothing has completed
		// since. Older than the result on screen, it is history and the result stands.
		[[nodiscard]] consteval bool RefusalShownOnlyWhenNewerContract()
		{
			Input base = ArmedQualitySession();
			base.lastResult.present = true;
			base.lastResult.accepted = true;
			base.lastResult.persisted = true;
			base.lastResult.tookEffectNow = true;
			base.lastResult.key = "mode"sv;
			base.lastResult.value = "\"off\""sv;
			base.lastRefusal = Refusal::kIntentInFlight;
			Input newer = base;
			newer.refusalIsNewer = true;
			return Reduce(base).resultLine == "mode = \"off\": accepted, persisted, took effect now"sv &&
			       Reduce(newer).resultLine == "refused: intent-in-flight"sv;
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
		static_assert(SecondSetKeyQueuedContract());
		static_assert(ThirdSetKeyRefusedContract());
		static_assert(CoalesceSameKeyContract());
		static_assert(QueuedReadsPendingContract());
		static_assert(CloseIsNeverQueuedContract());
		static_assert(UseModeRatioUnsetsContract());
		static_assert(RatioOverrideStandingContract());
		static_assert(SliderSnapContract());
		static_assert(SliderOneWritePerDragContract());
		static_assert(SliderWaitsOutTheWindowContract());
		static_assert(SliderCrossReleaseFiresBothContract());
		static_assert(SqrtContract());
		static_assert(RoomPlacementLevelHeadContract());
		static_assert(RoomPlacementTurnedHeadContract());
		static_assert(RoomPlacementDiscardsPitchAndRollContract());
		static_assert(RoomPlacementVerticalGazeContract());
		static_assert(RoomPlacementDegeneratePoseContract());
		static_assert(VetoIntentsIgnoreInFlightContract());
		static_assert(RatioFullFlaggedBesideReducedContract());
		static_assert(IntentValuesContract());
		static_assert(RatioSliderClampsContract());
		static_assert(ResultLineContract());
		static_assert(RefusedValueFallsBackContract());
		static_assert(WorkerUnavailableContract());
		static_assert(RefusalShownOnlyWhenNewerContract());
		static_assert(FixedTextContract());
		static_assert(ControlName(ControlId::kModeUltraPerformance) == "mode-ultraperformance"sv);
		static_assert(ControlName(ControlId::kUseModeRatio) == "use-mode-ratio"sv);
		static_assert(ControlName(ControlId::kClose) == "close"sv);
		static_assert(ControlName(ControlId::kNone) == "none"sv);
		static_assert(DispositionName(Disposition::kCoalesced) == "coalesced"sv);
		static_assert(IntentKindName(IntentKind::kHidePanel) == "hide-panel"sv);
		static_assert(RefusalName(Refusal::kIntentInFlight) == "intent-in-flight"sv);
		static_assert(RefusalName(Refusal::kWorkerUnavailable) == "worker-unavailable"sv);
		static_assert(IntentKindName(IntentKind::kSetKey) == "set-key"sv);
	}
}
