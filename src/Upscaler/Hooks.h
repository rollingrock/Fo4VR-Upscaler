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
// This build has one soft site: a temporary BSGraphics::Renderer::ClearColor entry
// detour used only as a render-thread rendezvous. It preserves the complete original
// function, changes no render state and is explicitly not the final upscaler injection
// point. A byte mismatch leaves the plugin loaded with the probe unavailable.

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

	[[nodiscard]] bool Install();

	// True once Install has run to completion, whether or not it patched anything.
	[[nodiscard]] bool Installed() noexcept;

	// How many sites Install actually patched. Zero on this build, and the self-check
	// prints it so "nothing happened" is a stated result rather than an inference.
	[[nodiscard]] std::size_t SiteCount() noexcept;

	// Lock-free snapshot of the temporary ClearColor rendezvous. The mismatch counts
	// remain zero when every observation came from one render thread and one renderer.
	[[nodiscard]] RenderThreadProbeState RenderThreadProbe() noexcept;
}
