#pragma once

#include "Upscaler/Backend/IBackend.h"

#include <span>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

namespace Upscaler::Backend
{
	// Probes every compiled-in backend and picks one. Selection must be explainable
	// from the log alone: one line per backend giving its name, whether it is
	// available, and its unavailableReason verbatim - never a paraphrase, because a
	// paraphrase is what turns a bug report into a second round trip.
	//
	// Probe order is ngx, streamline, ffx, null. The first three are the preference
	// order; null is last because it is always available, which makes it both the
	// fail-soft target and the A/B control arm.

	// Every compiled-in backend, in probe order. Stable for the life of the process.
	[[nodiscard]] std::span<IBackend* const> All();

	// Returns the stable singleton with this exact name without probing, selecting or
	// initializing it. Used by a bounded backend-specific create gate before that
	// backend is eligible for normal registry selection.
	[[nodiscard]] IBackend* Find(std::string_view a_name) noexcept;

	// Probes all of them and selects one. Never throws and never fails: the worst case
	// is that null is selected, so the return value is never null and no caller needs
	// to test for it.
	//
	// a_requested is the backend name from settings. Empty or "auto" means take the
	// first available in probe order; any other value is honoured over auto-selection
	// when that backend exists and is available, and otherwise falls back to auto with
	// a warning naming why.
	//
	// A null a_device requests only immutable build/pre-device descriptors and may
	// conservatively report a vendor backend unavailable. Runtime and adapter
	// availability require a device-bound render-thread call.
	// Selecting does not initialise; Initialize is the caller's next step.
	[[nodiscard]] IBackend* Select(std::string_view a_requested, ID3D11Device* a_device);

	// Commits a backend that a bounded gate has already initialized and adapter-checked.
	// The pointer must be one of All()'s stable singletons. This performs no probing,
	// logging or allocation and is therefore safe at the render-thread commit point.
	[[nodiscard]] bool ActivatePrepared(IBackend* a_backend) noexcept;

	// Returns selection to the always-available null backend after a prepared backend
	// has been shut down. Declines if another backend became current in the meantime.
	[[nodiscard]] bool DeactivatePrepared(IBackend* a_backend) noexcept;

	// The backend the last Select chose, or null before the first Select.
	[[nodiscard]] IBackend* Current() noexcept;
}
