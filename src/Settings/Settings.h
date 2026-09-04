#pragma once

#include "Settings/SettingsPolicy.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Settings
{
	// Which declared keys had a value in the TOML this load, in either their own
	// table or the legacy [Fo4VRUpscaler] fallback - loadImpl() clears this at the
	// start of every load and LoadOne() (below) populates it as each key resolves.
	// The mode/quality/enabled alias resolver in Validate() needs "absent, default
	// in force" distinguished from "present, and happens to equal the default",
	// which Setting<T> alone cannot answer. loadLock (held for the whole of
	// loadImpl(), see load() below) is what makes this safe across the game thread
	// and the devbench listener thread; nothing else may touch it.
	[[nodiscard]] inline std::vector<std::string_view>& PresentKeys()
	{
		static std::vector<std::string_view> keys;
		return keys;
	}

	inline void MarkPresent(std::string_view a_key)
	{
		auto& keys = PresentKeys();
		if (std::find(keys.begin(), keys.end(), a_key) == keys.end()) {
			keys.emplace_back(a_key);
		}
	}

	[[nodiscard]] inline bool WasPresent(std::string_view a_key) noexcept
	{
		const auto& keys = PresentKeys();
		return std::find(keys.begin(), keys.end(), a_key) != keys.end();
	}

	template <class T>
	class Setting
	{
	public:
		using value_type = T;

		Setting(
			std::string_view a_group,
			std::string_view a_key,
			value_type a_default) :
			_group(a_group),
			_key(a_key),
			_value(a_default)
		{
		}

		[[nodiscard]] auto group() const noexcept -> std::string_view { return this->_group; }
		[[nodiscard]] auto key() const noexcept -> std::string_view { return this->_key; }

		template <class Self>
		[[nodiscard]] auto&& get(this Self&& a_self) noexcept
		{
			return std::forward<Self>(a_self)._value;
		}

		template <class Self>
		[[nodiscard]] auto&& operator*(this Self&& a_self) noexcept
		{
			return std::forward<Self>(a_self).get();
		}

	private:
		std::string_view _group;
		std::string_view _key;
		value_type _value;
	};

	using bSetting = Setting<bool>;
	using iSetting = Setting<std::int64_t>;
	using fSetting = Setting<double>;
	using sSetting = Setting<std::string>;

#define MAKE_SETTING(a_type, a_group, a_key, a_default) \
	inline auto a_key = a_type(a_group##sv, #a_key##sv, a_default)

	// The keys, their sections, their defaults and their documentation live in
	// SettingsList.inl, which DevBench.cpp includes again under a different
	// expansion to build the config route's table. a_section becomes the group
	// each Setting<T> loads from and reports - Setting::group() below is that
	// section, not always "Fo4VRUpscaler" any more.
#define US_SETTING(a_type, a_section, a_key, a_default) MAKE_SETTING(a_type, a_section, a_key, a_default);
#include "Settings/SettingsList.inl"

#undef MAKE_SETTING

	// (key, own section) for every declared setting - a second expansion of the
	// same list, built once. loadImpl()'s unknown-key scan is the only reader: it
	// needs each key's own table to tell "sitting where it belongs", "sitting in
	// the legacy [Fo4VRUpscaler] fallback" and "not a declared key at all" apart,
	// which the key name alone cannot answer.
	[[nodiscard]] inline const std::vector<std::pair<std::string_view, std::string_view>>&
	KnownKeySections()
	{
		static const std::vector<std::pair<std::string_view, std::string_view>> sections = [] {
			std::vector<std::pair<std::string_view, std::string_view>> out;
#define US_SETTING(a_type, a_section, a_key, a_default) out.emplace_back(#a_key##sv, a_section##sv);
#include "Settings/SettingsList.inl"
			return out;
		}();
		return sections;
	}

	// Installed by DevBench so live overrides survive a reload. Without it, every
	// /config/set would be silently reverted by the next TOML reload.
	inline std::function<void()> postLoadHook;

	// Serializes load() itself: the game thread reloads on kGameLoaded and the
	// devbench listener thread reloads on request, and the setting writes (several
	// of them std::string assignments) are not individually thread-safe.
	inline std::mutex loadLock;

	// Serializes the complete settings-to-runtime transaction.  The fixed order is
	// configurationLock -> loadLock: readers take the same order, and every writer
	// keeps this outer lock until HDRRoundTrip publishes the terminal even epoch.
	inline std::mutex configurationLock;

	// Which rule, if any, last moved *mode away from its own raw parsed value -
	// Policy::ResolveModeAliases' two (quality alias, enabled = false), or
	// kGateForced when an armed diagnostic gate overrode whatever that resolved
	// to. Validate() is the only writer, set once per load under loadLock;
	// DevBench's listener thread and the render thread only ever read it, hence
	// the atomic rather than a plain enum.
	inline std::atomic<Policy::ModeSource> g_modeSource{ Policy::ModeSource::kExplicit };

	[[nodiscard]] inline Policy::ModeSource CurrentModeSource() noexcept
	{
		return g_modeSource.load(std::memory_order_acquire);
	}

	[[nodiscard]] inline std::string_view ModeSourceName(Policy::ModeSource a_source) noexcept
	{
		switch (a_source) {
		case Policy::ModeSource::kQualityAlias:
			return "quality"sv;
		case Policy::ModeSource::kEnabledFalse:
			return "enabled=false"sv;
		case Policy::ModeSource::kGateForced:
			return "diagnostic-gate"sv;
		case Policy::ModeSource::kRuntimeOverride:
			return "runtime-override"sv;
		case Policy::ModeSource::kExplicit:
		default:
			return "explicit"sv;
		}
	}

	// A live /mode request, standing in for the TOML's mode key on every load from the
	// moment it is set until ClearRuntimeModeOverride() runs - including a /config/reload,
	// which would otherwise silently revert an operator's live choice back to whatever the
	// TOML file says. Only Validate() reads it (see the mode/quality/enabled resolution
	// block below); Configuration.cpp is the only writer, from RequestModeSwitch. The
	// atomic<shared_ptr<const string>> pattern matches HDRRoundTrip's g_quality: readable
	// without a lock, and a store never races a concurrent read into a half-written string.
	inline std::atomic<std::shared_ptr<const std::string>> g_runtimeModeOverride;

	// Bumped by every SetRuntimeModeOverride call, never by clearing it. The override
	// survives every load for as long as it stands, so Configuration.cpp sees it again on
	// every ordinary reload (kGameLoaded included) with no request behind it - this is
	// what lets it tell "a fresh /mode request just landed" from "the standing override
	// is only being re-read", so a switch counter and a log line advance once per actual
	// request instead of once per unrelated reload.
	inline std::atomic<std::uint64_t> g_runtimeModeOverrideEpoch{ 0 };

	inline void SetRuntimeModeOverride(std::string_view a_mode)
	{
		g_runtimeModeOverride.store(
			std::make_shared<const std::string>(a_mode), std::memory_order_release);
		g_runtimeModeOverrideEpoch.fetch_add(1, std::memory_order_acq_rel);
	}

	// The explicit clear path the override needs: without one, a live switch would stand
	// forever, outliving even a restart-worthy TOML edit meant to take over again.
	inline void ClearRuntimeModeOverride() noexcept
	{
		g_runtimeModeOverride.store(nullptr, std::memory_order_release);
	}

	[[nodiscard]] inline std::shared_ptr<const std::string> RuntimeModeOverride() noexcept
	{
		return g_runtimeModeOverride.load(std::memory_order_acquire);
	}

	[[nodiscard]] inline std::uint64_t RuntimeModeOverrideEpoch() noexcept
	{
		return g_runtimeModeOverrideEpoch.load(std::memory_order_acquire);
	}

	using LockedOperation = bool (*)(void*) noexcept;

	namespace detail
	{
		struct ManualLockOperation
		{
			std::mutex* lock = nullptr;
			LockedOperation operation = nullptr;
			void* context = nullptr;
			bool locked = false;
			bool returned = false;
			bool result = false;
			std::uint32_t exceptionCode = 0;
		};

		[[nodiscard]] inline bool Acquire(std::mutex& a_lock) noexcept
		{
			try {
				a_lock.lock();
				return true;
			} catch (...) {
				return false;
			}
		}

		// POD-only SEH shell.  A hardware exception in a settings callback must not
		// strand either mutex; C++ work stays behind a noexcept function pointer.
		[[nodiscard]] inline bool InvokeWithManualLock(
			std::mutex& a_lock,
			LockedOperation a_operation,
			void* a_context,
			std::uint32_t* a_exceptionCode = nullptr) noexcept
		{
			ManualLockOperation operation{};
			operation.lock = std::addressof(a_lock);
			operation.operation = a_operation;
			operation.context = a_context;
			__try {
				__try {
					if (Acquire(*operation.lock)) {
						operation.locked = true;
						operation.result = operation.operation &&
						                   operation.operation(operation.context);
						operation.returned = true;
					}
				} __finally {
					if (operation.locked) {
						operation.lock->unlock();
						operation.locked = false;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				operation.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_exceptionCode) {
				*a_exceptionCode = operation.exceptionCode;
			}
			return operation.returned && operation.result;
		}
	}

	[[nodiscard]] inline bool WithLoadLock(
		LockedOperation a_operation,
		void* a_context,
		std::uint32_t* a_exceptionCode = nullptr) noexcept
	{
		return detail::InvokeWithManualLock(loadLock, a_operation, a_context, a_exceptionCode);
	}

	[[nodiscard]] inline bool WithConfigurationLock(
		LockedOperation a_operation,
		void* a_context,
		std::uint32_t* a_exceptionCode = nullptr) noexcept
	{
		return detail::InvokeWithManualLock(
			configurationLock, a_operation, a_context, a_exceptionCode);
	}

	// InitializeLog runs before the TOML is parsed, so trace is the bootstrap
	// level and this is what honours the user's choice. flush_on stays at the same
	// level: a CTD must not lose the last lines.
	inline void ApplyLogLevel()
	{
		const auto level = [] {
			switch (*logLevel) {
			case 0:
				return spdlog::level::off;
			case 1:
				return spdlog::level::warn;
			case 3:
				return spdlog::level::trace;
			default:
				return spdlog::level::info;
			}
		}();

		if (const auto log = spdlog::default_logger(); log) {
			log->set_level(level);
			log->flush_on(level);
		}
	}

	// An unrecognized value for an enum-ish key is named rather than silently
	// swapped for the default - a value the plugin refused is a bug report
	// somebody can read.
	inline void ValidateChoice(sSetting& a_setting, std::initializer_list<std::string_view> a_allowed, std::string_view a_fallback)
	{
		const std::string_view value = *a_setting;
		if (std::find(a_allowed.begin(), a_allowed.end(), value) != a_allowed.end()) {
			return;
		}
		logger::warn(FMT_STRING("Fo4VRUpscaler.toml: '{}' value '{}' not recognized - using '{}'"), a_setting.key(), value, a_fallback);
		*a_setting = std::string(a_fallback);
	}

	// Same log shape as ValidateChoice above, but asks Policy::IsKnownMode
	// (src/Settings/SettingsPolicy.h) instead of a second hand-maintained literal
	// list - mode's accepted vocabulary has one source of truth, the
	// static_assert'd IsKnownMode, and this is the shipping path that now asks it.
	inline void ValidateMode(sSetting& a_setting, std::string_view a_fallback)
	{
		const std::string_view value = *a_setting;
		if (Policy::IsKnownMode(value)) {
			return;
		}
		logger::warn(FMT_STRING("Fo4VRUpscaler.toml: '{}' value '{}' not recognized - using '{}'"), a_setting.key(), value, a_fallback);
		*a_setting = std::string(a_fallback);
	}

	// Post-load range checks. Each names the offending value and what was used
	// instead; a silently clamped value is a bug report nobody can read.
	inline void Validate()
	{
		if (*logLevel < 0 || *logLevel > 3) {
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: logLevel {} out of range (0 to 3) - using 2"), *logLevel);
			*logLevel = 2;
		}
		if (*sharpness < 0.0 || *sharpness > 1.0) {
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: sharpness {} out of range (0.0 to 1.0) - using 0.0"), *sharpness);
			*sharpness = 0.0;
		}
		// 0 means "the backend's own render size for the quality mode"; anything
		// else is a deliberate measurement override.
		if (*renderScaleOverride != 0.0 && (*renderScaleOverride < 0.30 || *renderScaleOverride > 1.00)) {
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: renderScaleOverride {} out of range (0.30 to 1.00, or 0 for the backend's own) - using 0"), *renderScaleOverride);
			*renderScaleOverride = 0.0;
		}
		// 999 is the derive-it sentinel. 0.0 is a legitimate bias, which is why
		// the sentinel is not 0.
		if (*mipBiasOverride != 999.0 && (*mipBiasOverride < -8.0 || *mipBiasOverride > 8.0)) {
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: mipBiasOverride {} out of range (-8.0 to 8.0, or 999 to derive it) - using 999"), *mipBiasOverride);
			*mipBiasOverride = 999.0;
		}
		if (*jitterPhaseOverride < 0 || *jitterPhaseOverride > 256) {
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: jitterPhaseOverride {} out of range (1 to 256, or 0 for the backend's own) - using 0"), *jitterPhaseOverride);
			*jitterPhaseOverride = 0;
		}
		if (*diagnosticContinuousFrames < 1 || *diagnosticContinuousFrames > 216000) {
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: diagnosticContinuousFrames {} out of range (1 to 216000) - using 512"), *diagnosticContinuousFrames);
			*diagnosticContinuousFrames = 512;
		}
		ValidateChoice(backend, { "auto"sv, "ngx"sv, "streamline"sv, "ffx"sv, "null"sv }, "auto"sv);
		ValidateChoice(quality, { "dlaa"sv, "quality"sv, "balanced"sv, "performance"sv, "ultraperformance"sv }, "dlaa"sv);
		ValidateChoice(stereoMode, { "intermediates"sv, "subrect"sv }, "subrect"sv);
		ValidateChoice(eyeOrder, { "auto"sv, "LR"sv, "RL"sv }, "auto"sv);
		ValidateChoice(jitterSource, { "engine"sv, "backend"sv }, "engine"sv);
		ValidateChoice(debugVisualise, { ""sv, "mv"sv, "depth"sv, "jitter"sv, "reactive"sv }, ""sv);

		// mode/quality/enabled alias resolution. Pure over Policy::ResolveModeAliases
		// (src/Settings/SettingsPolicy.h) - this is the only place that logs. quality
		// is already ValidateChoice'd above, so its value is always in mode's own
		// vocabulary by the time the alias can take it; mode's own raw value still
		// needs its own ValidateMode below in case it was present but garbage.
		{
			// A live override stands in for the raw parsed *mode value and counts as
			// present, so the deprecated quality alias never displaces it - the same
			// priority an explicit mode key already has. enabled = false and an armed
			// diagnostic gate still win over it below: the override changes which mode
			// runs, not whether the kill switch or the regression harness controls it.
			const auto runtimeOverride = RuntimeModeOverride();
			const std::string_view rawMode =
				runtimeOverride ? std::string_view(*runtimeOverride) : std::string_view(*mode);
			const bool rawModePresent = runtimeOverride != nullptr || WasPresent("mode"sv);
			const auto alias = Policy::ResolveModeAliases(
				rawMode, rawModePresent,
				*quality, WasPresent("quality"sv),
				*enabled, WasPresent("enabled"sv));
			*mode = std::string(alias.mode);
			g_modeSource.store(
				runtimeOverride && alias.source == Policy::ModeSource::kExplicit ?
					Policy::ModeSource::kRuntimeOverride :
					alias.source,
				std::memory_order_release);
			if (alias.qualityAliasApplied) {
				logger::warn(FMT_STRING("Fo4VRUpscaler.toml: mode not set - using the deprecated quality = '{}' as mode"), *quality);
			}
			if (alias.modeQualityConflict) {
				logger::warn(FMT_STRING("Fo4VRUpscaler.toml: mode = '{}' and the deprecated quality = '{}' disagree - mode wins"), *mode, *quality);
			}
			if (alias.enabledFalseForcedOff) {
				logger::warn("Fo4VRUpscaler.toml: enabled = false is deprecated - mode is the switch now; forcing mode = 'off'"sv);
			}
			if (alias.enabledTrueDeprecated) {
				logger::warn("Fo4VRUpscaler.toml: enabled is deprecated; mode now selects activation, and enabled continues only to gate hook installation"sv);
			}
		}
		ValidateMode(mode, "dlaa"sv);

		const int diagnosticCount = (*diagnosticHdrRoundTrip ? 1 : 0) +
		                            (*diagnosticStereoNull ? 1 : 0) +
		                            (*diagnosticNgxCreate ? 1 : 0) +
		                            (*diagnosticNgxEvaluate ? 1 : 0) +
		                            (*diagnosticNgxReassemble ? 1 : 0) +
		                            (*diagnosticNgxHandback ? 1 : 0) +
		                            (*diagnosticNgxContinuous ? 1 : 0) +
		                            (*diagnosticFfxEvaluate ? 1 : 0) +
		                            (*diagnosticFfxReassemble ? 1 : 0) +
		                            (*diagnosticFfxHandback ? 1 : 0) +
		                            (*diagnosticFfxContinuous ? 1 : 0);
		if (diagnosticCount > 1) {
			logger::warn("Fo4VRUpscaler.toml: the eleven pre-HDR development diagnostics are mutually exclusive; priority is diagnosticHdrRoundTrip, diagnosticStereoNull, diagnosticNgxCreate, diagnosticNgxEvaluate, diagnosticNgxReassemble, diagnosticNgxHandback, diagnosticNgxContinuous, diagnosticFfxEvaluate, diagnosticFfxReassemble, diagnosticFfxHandback, then diagnosticFfxContinuous"sv);
			if (*diagnosticHdrRoundTrip) {
				*diagnosticStereoNull = false;
				*diagnosticNgxCreate = false;
				*diagnosticNgxEvaluate = false;
				*diagnosticNgxReassemble = false;
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticStereoNull) {
				*diagnosticNgxCreate = false;
				*diagnosticNgxEvaluate = false;
				*diagnosticNgxReassemble = false;
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticNgxCreate) {
				*diagnosticNgxEvaluate = false;
				*diagnosticNgxReassemble = false;
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticNgxEvaluate) {
				*diagnosticNgxReassemble = false;
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticNgxReassemble) {
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticNgxHandback) {
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticNgxContinuous) {
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticFfxEvaluate) {
				*diagnosticFfxReassemble = false;
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticFfxReassemble) {
				*diagnosticFfxHandback = false;
				*diagnosticFfxContinuous = false;
			} else if (*diagnosticFfxHandback) {
				*diagnosticFfxContinuous = false;
			}
		}
		if ((*diagnosticNgxCreate || *diagnosticNgxEvaluate || *diagnosticNgxReassemble || *diagnosticNgxHandback || *diagnosticNgxContinuous) && *backend != "ngx"sv) {
			const bool taaReplacementWasCoupledToContinuous = *diagnosticTaaReplacement && *diagnosticNgxContinuous;
			const bool taaBypassWasCoupledToContinuous = *diagnosticTaaBypass && *diagnosticNgxContinuous;
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: the raw-NGX diagnostics require backend='ngx', not '{}' - disabling all five"), *backend);
			*diagnosticNgxCreate = false;
			*diagnosticNgxEvaluate = false;
			*diagnosticNgxReassemble = false;
			*diagnosticNgxHandback = false;
			*diagnosticNgxContinuous = false;
			if (taaReplacementWasCoupledToContinuous) {
				logger::warn("Fo4VRUpscaler.toml: diagnosticTaaReplacement was coupled to the refused NGX continuous gate - disabling it instead of silently running the standalone arm"sv);
				*diagnosticTaaReplacement = false;
			}
			if (taaBypassWasCoupledToContinuous) {
				logger::warn("Fo4VRUpscaler.toml: diagnosticTaaBypass was coupled to the refused NGX continuous gate - disabling it instead of silently running the standalone arm"sv);
				*diagnosticTaaBypass = false;
			}
		}
		if ((*diagnosticFfxEvaluate || *diagnosticFfxReassemble || *diagnosticFfxHandback || *diagnosticFfxContinuous) && *backend != "ffx"sv) {
			const bool taaReplacementWasCombinedWithFfx = *diagnosticTaaReplacement;
			const bool taaBypassWasCombinedWithFfx = *diagnosticTaaBypass;
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: the FSR diagnostics require backend='ffx', not '{}' - disabling all four"), *backend);
			*diagnosticFfxEvaluate = false;
			*diagnosticFfxReassemble = false;
			*diagnosticFfxHandback = false;
			*diagnosticFfxContinuous = false;
			if (taaReplacementWasCombinedWithFfx) {
				logger::warn("Fo4VRUpscaler.toml: diagnosticTaaReplacement was combined with a refused FSR gate - disabling it instead of silently running the standalone arm"sv);
				*diagnosticTaaReplacement = false;
			}
			if (taaBypassWasCombinedWithFfx) {
				logger::warn("Fo4VRUpscaler.toml: diagnosticTaaBypass was combined with a refused FSR gate - disabling it instead of silently running the standalone arm"sv);
				*diagnosticTaaBypass = false;
			}
		}
		if (*diagnosticTaaReplacement && *diagnosticTaaBypass) {
			logger::warn("Fo4VRUpscaler.toml: diagnosticTaaReplacement and diagnosticTaaBypass are mutually exclusive - keeping the accepted post-draw replacement and disabling bypass"sv);
			*diagnosticTaaBypass = false;
		}
		if (*diagnosticTaaReplacement &&
			(*diagnosticHdrRoundTrip || *diagnosticStereoNull || *diagnosticNgxCreate ||
				*diagnosticNgxEvaluate || *diagnosticNgxReassemble || *diagnosticNgxHandback ||
				*diagnosticFfxEvaluate || *diagnosticFfxReassemble || *diagnosticFfxHandback)) {
			logger::warn("Fo4VRUpscaler.toml: diagnosticTaaReplacement can run alone or with exactly one of diagnosticNgxContinuous/diagnosticFfxContinuous - disabling it for this configuration"sv);
			*diagnosticTaaReplacement = false;
		}
		if (*diagnosticTaaBypass &&
			(*diagnosticHdrRoundTrip || *diagnosticStereoNull || *diagnosticNgxCreate ||
				*diagnosticNgxEvaluate || *diagnosticNgxReassemble || *diagnosticNgxHandback ||
				*diagnosticFfxEvaluate || *diagnosticFfxReassemble || *diagnosticFfxHandback)) {
			logger::warn("Fo4VRUpscaler.toml: diagnosticTaaBypass can run alone or with exactly one of diagnosticNgxContinuous/diagnosticFfxContinuous - disabling it for this configuration"sv);
			*diagnosticTaaBypass = false;
		}

		// A gate armed alongside any non-off mode is a configuration that would silently
		// mean two different things: which one actually runs. Configuration.cpp derives the
		// vendor path, the native-TAA bypass and (for a reduced mode) the reduced-world
		// bracket from mode when it is not "off", and reads the diagnostic gates above unchanged
		// when it is "off" - the two inputs are never both live. The gates are the
		// regression harness and the runner sets mode = "off" whenever it arms one, so
		// refuse by forcing mode off, not by touching the gate: the gate stays exactly what
		// every acceptance run asserts.
		//
		// diagnosticTaaReplacement and diagnosticTaaBypass join the set for the same
		// reason. Configuration.cpp forces the native-TAA bypass unconditionally whenever
		// mode is not "off", without consulting diagnosticTaaReplacement - a legal,
		// unrefused diagnosticTaaReplacement = true sitting next to mode = "quality" would
		// otherwise never run and never say why. Forcing mode to "off" here, the same way
		// the eleven gates already do, keeps the guarantee Validate() makes a few lines up
		// (diagnosticTaaReplacement and diagnosticTaaBypass are mutually exclusive) true on
		// both paths instead of only the diagnostic one.
		{
			const bool anyGateArmed = *diagnosticHdrRoundTrip || *diagnosticStereoNull ||
			                           *diagnosticNgxCreate || *diagnosticNgxEvaluate ||
			                           *diagnosticNgxReassemble || *diagnosticNgxHandback ||
			                           *diagnosticNgxContinuous || *diagnosticFfxEvaluate ||
			                           *diagnosticFfxReassemble || *diagnosticFfxHandback ||
			                           *diagnosticFfxContinuous || *diagnosticTaaReplacement ||
			                           *diagnosticTaaBypass;
			const auto resolved = Policy::ResolveGateRefusal(*mode, anyGateArmed);
			if (resolved != *mode) {
				logger::warn(FMT_STRING("Fo4VRUpscaler.toml: a diagnostic gate is armed and mode = '{}' - the gates are the regression harness, forcing mode = 'off' (the runner sets mode = 'off' whenever it arms one)"), *mode);
				*mode = std::string(resolved);
				g_modeSource.store(Policy::ModeSource::kGateForced, std::memory_order_release);
			}
		}
	}

	// The one legacy table name every key used to sit under, before section tags
	// existed. Every declared key's own table is checked first; this is only the
	// fallback (see LoadOne) and the unknown-key scan's legacy classification.
	constexpr std::string_view kLegacyGroup = "Fo4VRUpscaler"sv;

	// value<T>() instead of as<T>() so an integer literal fills a float setting
	// (sharpness = 1 would otherwise be silently ignored), and a genuinely wrong
	// type warns instead of no-oping. Reads a_setting's own table first; when the
	// key is absent there and a_setting's table is not the legacy one, falls back
	// to [Fo4VRUpscaler] so a TOML written before section tags existed keeps
	// working, with one deprecation line naming the key and its new table. Marks
	// the key present either way, which is what Validate()'s alias resolver reads.
	// When the key is found in its own table, also checks the legacy table for a
	// same-named entry sitting unread beside it - a mid-migration TOML that still
	// has an old value under [Fo4VRUpscaler] would otherwise have that value
	// silently ignored with nothing in the log saying so, which is exactly the
	// failure this settings layer exists to prevent.
	template <class T>
	void LoadOne(Setting<T>& a_setting, const toml::table& a_config)
	{
		if (const auto tweak = a_config[a_setting.group()][a_setting.key()]; tweak) {
			if (const auto value = tweak.value<T>(); value) {
				a_setting.get() = *value;
				MarkPresent(a_setting.key());
				if (a_setting.group() != kLegacyGroup) {
					if (const auto shadowed = a_config[kLegacyGroup][a_setting.key()]; shadowed) {
						if (const auto shadowedValue = shadowed.value<T>(); shadowedValue) {
							logger::warn(
								FMT_STRING("Fo4VRUpscaler.toml: '{}' is set in both [{}] and the legacy [Fo4VRUpscaler] table - using [{}] = {}, ignoring the legacy {}"),
								a_setting.key(), a_setting.group(), a_setting.group(), *value, *shadowedValue);
						} else {
							logger::warn(
								FMT_STRING("Fo4VRUpscaler.toml: '{}' is also set in the legacy [Fo4VRUpscaler] table with the wrong type - using [{}] = {}, ignoring the legacy value"),
								a_setting.key(), a_setting.group(), *value);
						}
					}
				}
			} else {
				logger::warn(
					FMT_STRING("Fo4VRUpscaler.toml: '{}' has the wrong type - keeping default"),
					a_setting.key());
			}
			return;
		}
		if (a_setting.group() == kLegacyGroup) {
			return;
		}
		if (const auto legacy = a_config[kLegacyGroup][a_setting.key()]; legacy) {
			if (const auto value = legacy.value<T>(); value) {
				a_setting.get() = *value;
				MarkPresent(a_setting.key());
				logger::warn(
					FMT_STRING("Fo4VRUpscaler.toml: '{}' found under the legacy [Fo4VRUpscaler] table - move it to [{}]"),
					a_setting.key(), a_setting.group());
			} else {
				logger::warn(
					FMT_STRING("Fo4VRUpscaler.toml: '{}' has the wrong type in the legacy [Fo4VRUpscaler] table - keeping default"),
					a_setting.key());
			}
		}
	}

	// Read at plugin load, again on kGameLoaded, and again on request from the
	// dev tooling. Relative to the process working directory, i.e. the game
	// install, so under MO2 the virtual copy in the mod folder is the one that
	// wins.
	[[nodiscard]] inline bool loadImpl() noexcept
	{
		try {
			toml::table config;
			try {
				config = toml::parse_file("Data/F4SE/Plugins/Fo4VRUpscaler.toml"sv);
			} catch (const std::exception& e) {
				logger::warn(FMT_STRING("Fo4VRUpscaler.toml not loaded ({}), using defaults"), e.what());
				ApplyLogLevel();
				return true;
			}

			PresentKeys().clear();

			// Same list as the declarations above, so a key can never be declared and
			// then not loaded.
#define US_SETTING(a_type, a_section, a_key, a_default) LoadOne(a_key, config);
#include "Settings/SettingsList.inl"

			// Name every key sitting in either table that this build does not recognize
			// as belonging there: either a genuinely misspelled key, or a declared key
			// found in neither its own table nor (when the table being scanned is the
			// legacy one) the [Fo4VRUpscaler] fallback LoadOne already checked. A key
			// LoadOne read from the legacy table logged its own deprecation line above;
			// this loop must not also call that one "unknown".
			const auto scanTable = [](std::string_view a_table, const toml::table* a_tbl) {
				if (!a_tbl) {
					return;
				}
				for (const auto& [key, node] : *a_tbl) {
					const std::string_view k = key.str();
					bool recognized = false;
					for (const auto& [knownKey, knownSection] : KnownKeySections()) {
						if (knownKey != k) {
							continue;
						}
						recognized = knownSection == a_table || a_table == kLegacyGroup;
						break;
					}
					if (!recognized) {
						logger::warn(FMT_STRING("Fo4VRUpscaler.toml: unknown key '{}' ignored"), k);
					}
				}
			};
			scanTable(kLegacyGroup, config[kLegacyGroup].as_table());
			scanTable("Diagnostics"sv, config["Diagnostics"sv].as_table());

		Validate();
		ApplyLogLevel();

		logger::info(
			FMT_STRING("settings: enabled={} mode={} modeSource={} logLevel={} backend={} quality={} preset={} stereoMode={} eyeOrder={} jitterSource={} failSoft={} diagnosticDynamicResolution={} diagnosticHdrRoundTrip={} diagnosticStereoNull={} diagnosticNgxCreate={} diagnosticNgxEvaluate={} diagnosticNgxReassemble={} diagnosticNgxHandback={} diagnosticNgxContinuous={} diagnosticFfxEvaluate={} diagnosticFfxReassemble={} diagnosticFfxHandback={} diagnosticFfxContinuous={} diagnosticContinuousFrames={} diagnosticTaaReplacement={} diagnosticTaaBypass={} perfTimers={}"),
			*enabled, *mode, ModeSourceName(CurrentModeSource()), *logLevel, *backend, *quality, *preset, *stereoMode, *eyeOrder, *jitterSource, *failSoft, *diagnosticDynamicResolution, *diagnosticHdrRoundTrip, *diagnosticStereoNull, *diagnosticNgxCreate, *diagnosticNgxEvaluate, *diagnosticNgxReassemble, *diagnosticNgxHandback, *diagnosticNgxContinuous, *diagnosticFfxEvaluate, *diagnosticFfxReassemble, *diagnosticFfxHandback, *diagnosticFfxContinuous, *diagnosticContinuousFrames, *diagnosticTaaReplacement, *diagnosticTaaBypass, *perfTimers);

			if (postLoadHook) {
				postLoadHook();
			}
			return true;
		} catch (const std::exception& e) {
			logger::error(FMT_STRING("Fo4VRUpscaler.toml reload failed before runtime apply: {}"), e.what());
		} catch (...) {
			logger::error("Fo4VRUpscaler.toml reload failed before runtime apply with a non-standard C++ exception"sv);
		}
		return false;
	}

	[[nodiscard]] inline bool LoadImplThunk(void*) noexcept
	{
		return loadImpl();
	}

	// The manual wrapper is deliberately separate from loadImpl: loadImpl owns all
	// destructible C++ parser state, while this POD SEH shell always releases
	// loadLock even if a malformed dependency or callback raises a hardware fault.
	[[nodiscard]] inline bool load() noexcept
	{
		std::uint32_t exceptionCode = 0;
		const bool loaded = WithLoadLock(LoadImplThunk, nullptr, &exceptionCode);
		if (!loaded && exceptionCode != 0) {
			logger::error(FMT_STRING("Fo4VRUpscaler.toml reload raised SEH 0x{:08X} before runtime apply"), exceptionCode);
		}
		return loaded;
	}
}
