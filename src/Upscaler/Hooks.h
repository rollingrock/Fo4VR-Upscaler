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
// This build has seven soft sites. A temporary BSGraphics::Renderer::ClearColor entry
// detour is a render-thread rendezvous; a native ImageSpaceEffectTemporalAA entry
// detour measures the exact stage's live cadence; a direct call hook captures bindings
// after its vanilla pixel draw and, only during an accepted full-output Quality episode,
// brackets that prepared draw with an exact RTV-preserving null DSV; an ImageSpace
// effect-dispatch hook records source/destination ids only for manager effect 3
// (HDR/tonemap). The dynamic
// resolution IsActive entry preserves vanilla except during an accepted full-output
// vendor handback, where it keeps effect routing reachable while live ratios are unity.
// A sixth hook retains the original per-frame dynamic-geometry updater so the bracket
// can refresh its cached half-float ratios at unity and again after restoration. A byte
// verified wrapper around the complete late ImageSpace range is the structural cleanup
// seam after slot 21 and the manager's source/destination bookkeeping. A byte mismatch
// leaves only that probe unavailable.

#include <cstddef>
#include <cstdint>

#include "Upscaler/ResetPolicy.h"

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
		bool drawInstalled = false;
		std::uint64_t calls = 0;
		std::uint32_t threadId = 0;
		std::uint64_t threadMismatches = 0;
		std::uintptr_t effect = 0;
		std::uint64_t effectMismatches = 0;
	};

	struct HDRInputProbeState
	{
		bool installed = false;
		bool drawInstalled = false;
		std::uint64_t calls = 0;
		std::uint32_t threadId = 0;
		std::uint64_t threadMismatches = 0;
		std::uintptr_t effect = 0;
		std::int32_t sourceLogical = -1;
		std::int32_t destinationLogical = -1;
		std::uint64_t routeMismatches = 0;
	};

	struct DynamicResolutionDispatchState
	{
		bool installed = false;
		bool qualityReady = false;
		std::uintptr_t effect = 0;
		std::uint64_t calls = 0;
		// Historical telemetry name: counts executions whose scale was neutralized.
		// Slot 21 itself was still dispatched and wrote its selected destination.
		std::uint64_t suppressions = 0;
		std::uint64_t passthroughs = 0;
		std::uint64_t geometryRefreshes = 0;
		std::uint64_t geometryFlagPreflights = 0;
		std::uint64_t dynamicGeometryDispatches = 0;
		std::uint64_t dynamicGeometryFailures = 0;
		std::uint64_t missingTaaClosures = 0;
		std::uint64_t preWorldCalls = 0;
		std::uint64_t preWorldRepairs = 0;
		std::uint64_t preWorldFailures = 0;
		std::uint64_t staleWatchdogRestores = 0;
		bool imageSpaceDepthOverrideEnabled = false;
		std::uint64_t imageSpaceDepthOverrideAttempts = 0;
		std::uint64_t imageSpaceDepthOverrideNonNull = 0;
		std::uint64_t imageSpaceDepthOverrideAlreadyNull = 0;
		std::uint64_t imageSpaceDepthOverrideDraws = 0;
		std::uint64_t imageSpaceDepthOverrideRestores = 0;
		std::uint64_t imageSpaceDepthOverrideFailures = 0;
		std::uint64_t imageSpaceDepthOverrideRestoreMismatches = 0;
	};

	// Atomically publishes the setting used only when a later accepted Quality
	// handback latches its complete engine-ImageSpace transaction. An in-flight
	// transaction retains the value it admitted with.
	void Configure(bool a_unbindDepthStencil) noexcept;

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

	// Lock-free snapshot of the manager effect-3 dispatch. Source and destination are
	// the logical RT ids passed to vanilla immediately before ImageSpaceEffectHDR runs.
	[[nodiscard]] HDRInputProbeState HDRInputProbe() noexcept;

	// Reduced-input engine colour may change only after every downstream hook has
	// installed and a byte-verified slot-21 transaction has presented unity ratios,
	// rebuilt dynamic geometry, and temporarily opted that effect into the same dynamic
	// triangle. Begin is render-thread only. Abort is idempotent and restores the effect
	// flag, authoritative requested ratios, and cached geometry.
	[[nodiscard]] bool QualityHandbackReady() noexcept;

	// The pre-world geometry seam advances the fail-closed world reducer exactly once
	// for each main render epoch. HDR may consume only the admitted ticket cached for
	// that same epoch while the reduced-world ownership token is still live.
	[[nodiscard]] ResetPolicy::WorldAdmissionTicket CurrentQualityWorldAdmission() noexcept;
	[[nodiscard]] bool BeginQualityHandback(std::uintptr_t a_manager) noexcept;
	[[nodiscard]] bool QualityHandbackCommitReady(std::uintptr_t a_manager) noexcept;
	[[nodiscard]] bool AbortQualityHandback() noexcept;

	// Slot 21 shares the verified manager dispatch hook with HDR. During a reduced-input/
	// full-output NGX handback, its routing draw remains active while its redundant scale
	// is neutralized with unity live ratios and the selected dynamic triangle is refreshed.
	[[nodiscard]] DynamicResolutionDispatchState DynamicResolutionDispatch() noexcept;
}
