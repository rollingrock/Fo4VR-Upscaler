#pragma once

#include <cstdint>
#include <string>

namespace Upscaler::WorkshopIntegration
{
	// Hook installation and broad draw diagnostics are startup-only; correction is live.
	[[nodiscard]] bool Install(bool a_drawDiagnostics = false) noexcept;
	[[nodiscard]] bool Installed() noexcept;
	void Configure(bool a_correct) noexcept;
	void CaptureBeforeHDR(std::uint64_t a_epoch) noexcept;
	// Called for every exact manager effect12 dispatch; a_owned is true only under
	// positively latched main Quality ownership. hold is the one scope whose caller skips
	// the engine call, and a latched module returns it only while its own binding is live.
	enum class EffectScope
	{
		native,
		owned,
		nestedNative,
		hold
	};
	[[nodiscard]] EffectScope BeginEffect(std::uint64_t a_epoch, std::uint32_t a_source,
		std::uint32_t a_destination, bool a_owned) noexcept;
	void EndEffect(EffectScope a_scope) noexcept;
	void ClearFrame() noexcept;
	[[nodiscard]] bool Unrecoverable() noexcept;
	[[nodiscard]] std::string SnapshotJson();
}
