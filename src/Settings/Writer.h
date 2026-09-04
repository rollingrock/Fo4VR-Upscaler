#pragma once

#include <string>
#include <string_view>

// The one place this plugin ever writes a settings file. See Writer.cpp's header
// comment for the full design; this file is the public surface DevBench's
// /config/set route (src/DevBench/DevBench.cpp) calls into.
namespace Settings::Writer
{
	// The outcome of one Set() call. accepted is false when the requested value
	// itself is refused - an unknown key, a value that does not parse for the key's
	// type, or a value Settings::Validate() would itself have rewritten (see
	// Writer.cpp for what "would have" means here) - and also when a legitimate,
	// validated value cannot be written because the existing override file exists but
	// fails to parse (Writer.cpp's ParseExistingOverride): that case is reported as a
	// refusal, with a reason naming the file, rather than risk silently discarding
	// every other key already persisted there. persisted/tookEffectNow/needsRestart
	// are independent facts a caller reports alongside accepted: a legitimate,
	// acceptable value can still fail to persist for an unrelated reason (a disk or
	// Documents-redirection problem), and an accepted, persisted value either takes
	// effect immediately (RestartPolicy::kLive) or waits for the next launch
	// (kRestart) - never both.
	struct SetResult
	{
		bool accepted = false;
		bool persisted = false;
		bool tookEffectNow = false;
		bool needsRestart = false;
		std::string key;
		// The canonical TOML-literal text of the value actually validated/persisted -
		// e.g. an int typed "5.0" comes back "5". Empty when !accepted.
		std::string value;
		// Populated whenever there is something to say beyond the four bools above:
		// always set on a refusal, and set on an accepted request that could not be
		// persisted or, once persisted, could not be applied live.
		std::string reason;
	};

	// MUTATES on acceptance. Validates a_value for a_key by trial-applying it to the
	// live settings and running the real Settings::Validate() over the result (see
	// Writer.cpp); a value Validate() would alter or reject is refused and nothing on
	// disk is touched, and so is a value refused because the existing override file
	// cannot be parsed. Otherwise the value is written into the layered override file
	// (Settings::Paths::OverrideFilePath()) via a temp-file-then-atomic-rename
	// replace performed on the game thread, never the calling thread; a key whose
	// SettingsList.inl restart policy is Live is then applied immediately through
	// Configuration::Apply() - the same reload path /config/reload already uses - so
	// a headless run and a future interface both take one path. Safe to call from any
	// thread; each call is independent, matching Configuration::RequestModeSwitch.
	[[nodiscard]] SetResult Set(std::string_view a_key, std::string_view a_value) noexcept;
}
