#pragma once

// Decides when accumulated reconstruction history is invalid and carries a paired reset
// from game/UI event threads to the render thread.

#include <cstdint>
#include <string_view>

namespace Upscaler::ResetPolicy
{
	struct State
	{
		bool pending = false;
		bool menuSinkInstalled = false;
		bool cellSinkInstalled = false;
		bool resetOnMenu = true;
		bool resetOnLoad = true;
		bool resetOnCell = true;
		std::uint64_t requests = 0;
		std::uint64_t coalesced = 0;
		std::uint64_t consumptions = 0;
		std::uint64_t loadEvents = 0;
		std::uint64_t menuOpenEvents = 0;
		std::uint64_t menuCloseEvents = 0;
		std::uint64_t cellEnterEvents = 0;
		std::uint64_t cellLeaveEvents = 0;
		std::uint64_t leftSubmissions = 0;
		std::uint64_t rightSubmissions = 0;
		std::uint32_t lastCellId = 0;
	};

	// Atomic and safe from the settings/reload thread.
	void Configure(bool a_resetOnMenu, bool a_resetOnLoad, bool a_resetOnCell) noexcept;

	// Game-thread lifecycle entry points. Installation is idempotent and may be retried
	// when RE::UI or the player singleton was not ready at the first message. Cell-event
	// installation is deferred out of synchronous F4SE lifecycle callbacks.
	[[nodiscard]] bool InstallMenuEvents() noexcept;
	[[nodiscard]] bool InstallCellEvents() noexcept;
	void OnGameLoaded();

	// Both eyes are always reset together. Resetting one and not the other is a
	// stereo-rivalry artifact, which is worse than the ghost it fixed.
	//
	// Candidate triggers: menu open and close, save load, cell change, fast travel,
	// resolution change, scope raise. Each becomes a TOML toggle and each reset is
	// logged with its reason, because a wrong reset policy is a documented flicker
	// source and a hardcoded guess is one nobody can audit.
	void RequestReset(std::string_view a_reason);

	[[nodiscard]] bool ConsumeReset() noexcept;
	void RecordSubmission(std::uint32_t a_eye, bool a_reset) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
