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
// On this build the site list is empty: Install writes nothing, logs one line saying
// so, and returns true.

#include <cstddef>

namespace Upscaler::Hooks
{
	[[nodiscard]] bool Install();

	// True once Install has run to completion, whether or not it patched anything.
	[[nodiscard]] bool Installed() noexcept;

	// How many sites Install actually patched. Zero on this build, and the self-check
	// prints it so "nothing happened" is a stated result rather than an inference.
	[[nodiscard]] std::size_t SiteCount() noexcept;
}
