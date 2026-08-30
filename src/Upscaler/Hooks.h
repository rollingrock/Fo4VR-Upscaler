#pragma once

// The install contract.
//
// Install verifies the original bytes of every hard site BEFORE it writes a single
// byte, so a partial patch is impossible. It returns false only when a hard site
// mismatches; the caller then logs critical and stays loaded without patching, because
// a plugin that is present and inert can still tell the story in the log, and one that
// refused to load cannot.
//
// Soft sites decline individually. Each logs a warning naming the exact behavioural
// degradation and leaves a flag the runtime branches on, so one wrong byte costs one
// feature rather than the mod.
//
// This build has two soft sites. A temporary BSGraphics::Renderer::ClearColor entry
// detour is a render-thread rendezvous; a native ImageSpaceEffectTemporalAA entry
// detour measures the exact stage's live cadence. Both preserve the complete original
// function and change no render state. A byte mismatch leaves the plugin loaded with
// only that probe unavailable.

#include <cstddef>
#include <cstdint>

namespace Upscaler::Hooks
{
	struct RenderThreadProbeState
	{
		bool installed = false;
		std::uint64_t calls = 0;
		std::uint32_t threadId = 0;
		std::uint64_t threadMismatches = 0;
		std::uintptr_t renderer = 0;
		std::uint64_t rendererMismatches = 0;
	};

	struct TemporalAAProbeState
	{
		bool installed = false;
		std::uint64_t calls = 0;
		std::uint32_t threadId = 0;
		std::uint64_t threadMismatches = 0;
		std::uintptr_t effect = 0;
		std::uint64_t effectMismatches = 0;
	};

	[[nodiscard]] bool Install();

	// True once Install has run to completion, whether or not it patched anything.
	[[nodiscard]] bool Installed() noexcept;

	// How many sites Install actually patched. The self-check prints it so partial
	// fail-soft installation is visible rather than inferred.
	[[nodiscard]] std::size_t SiteCount() noexcept;

	// Lock-free snapshot of the temporary ClearColor rendezvous. The mismatch counts
	// remain zero when every observation came from one render thread and one renderer.
	[[nodiscard]] RenderThreadProbeState RenderThreadProbe() noexcept;

	// Lock-free snapshot of the native TemporalAA effect entry. Calls are counted before
	// the complete vanilla function runs; no arguments or render state are changed.
	[[nodiscard]] TemporalAAProbeState TemporalAAProbe() noexcept;
}
