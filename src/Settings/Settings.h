#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Settings
{
	// Every declared key, for load()'s unknown-key warning - a misspelled TOML
	// key used to be silently ignored. A free function, deliberately: a static
	// member would give each Setting<T> instantiation its own private list.
	[[nodiscard]] inline std::vector<std::string_view>& KnownKeys()
	{
		static std::vector<std::string_view> keys;
		return keys;
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
			Settings::KnownKeys().emplace_back(a_key);
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

	// The keys, their defaults and their documentation live in SettingsList.inl,
	// which DevBench.cpp includes again under a different expansion to build the
	// config route's table.
#define US_SETTING(a_type, a_key, a_default) MAKE_SETTING(a_type, "Fo4VRUpscaler", a_key, a_default);
#include "Settings/SettingsList.inl"

#undef MAKE_SETTING

	// Installed by DevBench so live overrides survive a reload. Without it, every
	// /config/set would be silently reverted by the next TOML reload.
	inline std::function<void()> postLoadHook;

	// Serializes load() itself: the game thread reloads on kGameLoaded and the
	// devbench listener thread reloads on request, and the setting writes (several
	// of them std::string assignments) are not individually thread-safe.
	inline std::mutex loadLock;

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
		ValidateChoice(backend, { "auto"sv, "ngx"sv, "streamline"sv, "ffx"sv, "null"sv }, "auto"sv);
		ValidateChoice(quality, { "dlaa"sv, "quality"sv, "balanced"sv, "performance"sv, "ultraperformance"sv }, "dlaa"sv);
		ValidateChoice(stereoMode, { "intermediates"sv, "subrect"sv }, "intermediates"sv);
		ValidateChoice(eyeOrder, { "auto"sv, "LR"sv, "RL"sv }, "auto"sv);
		ValidateChoice(jitterSource, { "engine"sv, "backend"sv }, "engine"sv);
		ValidateChoice(debugVisualise, { ""sv, "mv"sv, "depth"sv, "jitter"sv, "reactive"sv }, ""sv);

		const int diagnosticCount = (*diagnosticHdrRoundTrip ? 1 : 0) +
		                            (*diagnosticStereoNull ? 1 : 0) +
		                            (*diagnosticNgxCreate ? 1 : 0) +
		                            (*diagnosticNgxEvaluate ? 1 : 0) +
		                            (*diagnosticNgxReassemble ? 1 : 0) +
		                            (*diagnosticNgxHandback ? 1 : 0) +
		                            (*diagnosticNgxContinuous ? 1 : 0) +
		                            (*diagnosticFfxEvaluate ? 1 : 0) +
		                            (*diagnosticFfxReassemble ? 1 : 0);
		if (diagnosticCount > 1) {
			logger::warn("Fo4VRUpscaler.toml: the nine development diagnostics are mutually exclusive; priority is diagnosticHdrRoundTrip, diagnosticStereoNull, diagnosticNgxCreate, diagnosticNgxEvaluate, diagnosticNgxReassemble, diagnosticNgxHandback, diagnosticNgxContinuous, diagnosticFfxEvaluate, then diagnosticFfxReassemble"sv);
			if (*diagnosticHdrRoundTrip) {
				*diagnosticStereoNull = false;
				*diagnosticNgxCreate = false;
				*diagnosticNgxEvaluate = false;
				*diagnosticNgxReassemble = false;
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
			} else if (*diagnosticStereoNull) {
				*diagnosticNgxCreate = false;
				*diagnosticNgxEvaluate = false;
				*diagnosticNgxReassemble = false;
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
			} else if (*diagnosticNgxCreate) {
				*diagnosticNgxEvaluate = false;
				*diagnosticNgxReassemble = false;
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
			} else if (*diagnosticNgxEvaluate) {
				*diagnosticNgxReassemble = false;
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
			} else if (*diagnosticNgxReassemble) {
				*diagnosticNgxHandback = false;
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
			} else if (*diagnosticNgxHandback) {
				*diagnosticNgxContinuous = false;
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
			} else if (*diagnosticNgxContinuous) {
				*diagnosticFfxEvaluate = false;
				*diagnosticFfxReassemble = false;
			} else if (*diagnosticFfxEvaluate) {
				*diagnosticFfxReassemble = false;
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
		if ((*diagnosticFfxEvaluate || *diagnosticFfxReassemble) && *backend != "ffx"sv) {
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml: the FSR diagnostics require backend='ffx', not '{}' - disabling both"), *backend);
			*diagnosticFfxEvaluate = false;
			*diagnosticFfxReassemble = false;
		}
		if (*diagnosticTaaReplacement && *diagnosticTaaBypass) {
			logger::warn("Fo4VRUpscaler.toml: diagnosticTaaReplacement and diagnosticTaaBypass are mutually exclusive - keeping the accepted post-draw replacement and disabling bypass"sv);
			*diagnosticTaaBypass = false;
		}
		if (*diagnosticTaaReplacement &&
			(*diagnosticHdrRoundTrip || *diagnosticStereoNull || *diagnosticNgxCreate ||
				*diagnosticNgxEvaluate || *diagnosticNgxReassemble || *diagnosticNgxHandback ||
				*diagnosticFfxEvaluate || *diagnosticFfxReassemble)) {
			logger::warn("Fo4VRUpscaler.toml: diagnosticTaaReplacement can run alone or with diagnosticNgxContinuous only - disabling it for this configuration"sv);
			*diagnosticTaaReplacement = false;
		}
		if (*diagnosticTaaBypass &&
			(*diagnosticHdrRoundTrip || *diagnosticStereoNull || *diagnosticNgxCreate ||
				*diagnosticNgxEvaluate || *diagnosticNgxReassemble || *diagnosticNgxHandback ||
				*diagnosticFfxEvaluate || *diagnosticFfxReassemble)) {
			logger::warn("Fo4VRUpscaler.toml: diagnosticTaaBypass can run alone or with diagnosticNgxContinuous only - disabling it for this configuration"sv);
			*diagnosticTaaBypass = false;
		}
	}

	// Read at plugin load, again on kGameLoaded, and again on request from the
	// dev tooling. Relative to the process working directory, i.e. the game
	// install, so under MO2 the virtual copy in the mod folder is the one that
	// wins.
	inline void load()
	{
		const std::scoped_lock lock(loadLock);
		toml::table config;
		try {
			config = toml::parse_file("Data/F4SE/Plugins/Fo4VRUpscaler.toml"sv);
		} catch (const std::exception& e) {
			logger::warn(FMT_STRING("Fo4VRUpscaler.toml not loaded ({}), using defaults"), e.what());
			ApplyLogLevel();
			return;
		}

// value<T>() instead of as<T>() so an integer literal fills a float setting
// (sharpness = 1 would otherwise be silently ignored), and a genuinely wrong type
// warns instead of no-oping.
#define LOAD(a_setting)                                                                      \
	if (const auto tweak = config[a_setting.group()][a_setting.key()]; tweak) {              \
		if (const auto value = tweak.value<decltype(a_setting)::value_type>(); value) {      \
			*a_setting = *value;                                                             \
		} else {                                                                             \
			logger::warn(                                                                    \
				FMT_STRING("Fo4VRUpscaler.toml: '{}' has the wrong type - keeping default"), \
				a_setting.key());                                                            \
		}                                                                                    \
	}

		// Same list as the declarations above, so a key can never be declared and
		// then not loaded.
#define US_SETTING(a_type, a_key, a_default) LOAD(a_key)
#include "Settings/SettingsList.inl"

#undef LOAD

		// Name every unrecognized key once, so a typo is a log line instead of a
		// silent no-op.
		if (const auto* tbl = config["Fo4VRUpscaler"].as_table(); tbl) {
			for (const auto& [key, node] : *tbl) {
				const std::string_view k = key.str();
				const auto& known = KnownKeys();
				if (std::find(known.begin(), known.end(), k) == known.end()) {
					logger::warn(FMT_STRING("Fo4VRUpscaler.toml: unknown key '{}' ignored"), k);
				}
			}
		}

		Validate();
		ApplyLogLevel();

		logger::info(
			FMT_STRING("settings: enabled={} logLevel={} backend={} quality={} stereoMode={} eyeOrder={} jitterSource={} failSoft={} diagnosticHdrRoundTrip={} diagnosticStereoNull={} diagnosticNgxCreate={} diagnosticNgxEvaluate={} diagnosticNgxReassemble={} diagnosticNgxHandback={} diagnosticNgxContinuous={} diagnosticFfxEvaluate={} diagnosticFfxReassemble={} diagnosticTaaReplacement={} diagnosticTaaBypass={} perfTimers={}"),
			*enabled, *logLevel, *backend, *quality, *stereoMode, *eyeOrder, *jitterSource, *failSoft, *diagnosticHdrRoundTrip, *diagnosticStereoNull, *diagnosticNgxCreate, *diagnosticNgxEvaluate, *diagnosticNgxReassemble, *diagnosticNgxHandback, *diagnosticNgxContinuous, *diagnosticFfxEvaluate, *diagnosticFfxReassemble, *diagnosticTaaReplacement, *diagnosticTaaBypass, *perfTimers);

		if (postLoadHook) {
			postLoadHook();
		}
	}
}
