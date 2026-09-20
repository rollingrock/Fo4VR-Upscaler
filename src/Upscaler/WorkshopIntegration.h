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
	// Called only for exact manager effect12, with positively latched main Quality ownership.
	enum class EffectScope { native, owned, nestedNative, hold };
	[[nodiscard]] EffectScope BeginEffect(std::uint64_t a_epoch, std::uint32_t a_source,
		std::uint32_t a_destination, bool a_owned) noexcept;
	void EndEffect(EffectScope a_scope) noexcept;
	void ClearFrame() noexcept;
	[[nodiscard]] bool Unrecoverable() noexcept;
	[[nodiscard]] std::string SnapshotJson();
}
