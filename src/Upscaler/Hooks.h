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
// resolution IsActive entry mirrors the vanilla predicate and, during an accepted
// full-output vendor handback, reports the effect inactive: live ratios are unity, so
// TemporalAA is the range's last active effect and the manager routes its destination to
// RT4, the no-DRS frame shape. A sixth hook retains the original per-frame
// dynamic-geometry updater so the bracket can refresh its cached half-float ratios at
// unity and again after restoration. A byte verified wrapper around the complete late
// ImageSpace range is the structural cleanup seam after the manager's source/destination
// bookkeeping. A byte mismatch leaves only that probe unavailable.
//
// A reduced-NGX transaction publishes at the TemporalAA seam: the shared prepared-draw
// hook rechecks world admission and replaces the engine's draw with one MRT draw of the
// reconstruction into the parity-selected history and RT4, and the late range commits the
// accounting. A transaction that has written RT1 and then loses that draw is not aborted:
// the unity bracket stays up, the hook runs the engine's own TemporalAA on the
// reconstruction, which writes the same two targets, and the late range closes the frame
// without a commit. Nothing restores RT1 or RT3 any more.

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
		// Prepared draws the hook let the engine run under a TemporalAA scope, and the
		// subset taken because a reduced-NGX transaction fell back to the native draw.
		std::uint64_t nativeDraws = 0;
		std::uint64_t fallbackNativeDraws = 0;
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
		// Historical telemetry name: slot-21 dispatches on a frame the transaction
		// owns. Zero by design - the IsActive hook reports the effect inactive at unity
		// - so a count here means the predicate and the live ratios disagreed and one
		// frame showed slot 21's copy of logical 1 instead of the published RT4.
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
		// The logical source and destination the manager handed the last such dispatch,
		// -1 while none has happened. Read live, so /state can show what a stray copy
		// would have routed.
		std::int32_t neutralizedSourceLogical = -1;
		std::int32_t neutralizedDestinationLogical = -1;
		bool imageSpaceDepthOverrideEnabled = false;
		std::uint64_t imageSpaceDepthOverrideAttempts = 0;
		std::uint64_t imageSpaceDepthOverrideNonNull = 0;
		std::uint64_t imageSpaceDepthOverrideAlreadyNull = 0;
		std::uint64_t imageSpaceDepthOverrideDraws = 0;
		std::uint64_t imageSpaceDepthOverrideRestores = 0;
		std::uint64_t imageSpaceDepthOverrideFailures = 0;
		std::uint64_t imageSpaceDepthOverrideRestoreMismatches = 0;
		// The seam-exit depth-stencil rebind experiment (rebindDepthStencilAfterSeam).
		bool seamDepthRebindEnabled = false;
		std::uint64_t seamDepthRebindAttempts = 0;
		std::uint64_t seamDepthRebinds = 0;
		std::uint64_t seamDepthRebindAlreadyBound = 0;
		std::uint64_t seamDepthRebindDeclines = 0;
		std::uint64_t seamDepthRebindFailures = 0;
	};

	// Atomically publishes the two settings the hooks read. They differ in when they
	// take effect, and the difference is load-bearing, so it is stated per setting
	// rather than for the call.
	//
	// a_unbindDepthStencil is latched at admission: it is read only where an accepted
	// Quality handback latches its complete engine-ImageSpace transaction, so an
	// in-flight transaction retains the value it admitted with.
	//
	// a_rebindDepthStencilAfterSeam is read live, one acquire load per published
	// frame at the seam's exit, so a write through the config route takes effect on
	// the next published frame of the session it was written in.
	void Configure(bool a_unbindDepthStencil, bool a_rebindDepthStencilAfterSeam) noexcept;

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

	// Whether the main-world jitter call site is patched. Arming a backend-owned jitter
	// sequence without it would leave nothing to write the sample, and every evaluation
	// would then fail its committed-value check, so the arm site consults this first.
	[[nodiscard]] bool JitterCommitInstalled() noexcept;

	// The four screen-space-reflection draw sites, and how often each pass drew this
	// session whether or not a census was armed. The counts are what separates "the
	// reflection chain does not run at this pose" from "nothing was watching": all four
	// SSLR shaders override the shared ImageSpace draw, so before these sites existed the
	// frame census could not have recorded a reflection pass however many ran.
	struct SslrProbeState
	{
		bool prepassInstalled = false;
		bool raytracingInstalled = false;
		bool blurHInstalled = false;
		bool blurVInstalled = false;
		std::uint64_t prepassDraws = 0;
		std::uint64_t raytracingDraws = 0;
		std::uint64_t blurHDraws = 0;
		std::uint64_t blurVDraws = 0;
	};

	[[nodiscard]] SslrProbeState SslrProbe() noexcept;

	// BSWaterShader's two constant-setup entries, and how often each ran this session
	// whether or not a census was armed. The counts carry the same burden the SSLR ones
	// do: the water SURFACE draw reaches none of this plugin's other seams, so before
	// these sites existed a census that recorded no water pass was describing the seams
	// it had and not the frame, and a zero here is a reading only because the installed
	// flag beside it says something was watching.
	struct WaterProbeState
	{
		bool setupGeometryInstalled = false;
		bool setupTechniqueInstalled = false;
		std::uint64_t setupGeometryCalls = 0;
		std::uint64_t setupTechniqueCalls = 0;
		// The thread the water setup first ran on, how often a later call ran on another,
		// how many ran off the ClearColor thread, and that thread's id. A water sample may
		// read the D3D11 immediate context only when the two ids agree, so these four are
		// what make an empty binding block in a sample a statement rather than a gap.
		std::uint32_t setupThreadId = 0;
		std::uint64_t setupThreadMismatches = 0;
		std::uint64_t setupOffRenderThread = 0;
		std::uint32_t renderThreadId = 0;
	};

	[[nodiscard]] WaterProbeState WaterProbe() noexcept;

	// Whether the UI render-entry call site is patched. Without it the UI-entry
	// observation can never record a sample, so the route that arms one says so instead
	// of returning an empty run.
	[[nodiscard]] bool UiRenderProbeInstalled() noexcept;

	// How many times that hook found the engine's own callback slot empty and therefore
	// called nothing. The site is an indirect call through a slot the frame setup
	// rewrites, so this is the one way the hook can change a frame; a non-zero count is a
	// fact about the session, not a rounding error, and /state carries it.
	[[nodiscard]] std::uint64_t UiRenderNullCallbacks() noexcept;

	// Reduced-input engine colour may change only after every downstream hook has
	// installed and a byte-verified slot-21 transaction has presented unity ratios,
	// rebuilt dynamic geometry, and temporarily opted that effect into the same dynamic
	// triangle - the flag matters only if slot 21 is dispatched against the IsActive
	// contract, in which case its copy at least uses the unity geometry. Begin is
	// render-thread only. Abort is idempotent and restores the effect flag,
	// authoritative requested ratios, and cached geometry.
	[[nodiscard]] bool QualityHandbackReady() noexcept;

	// The pre-world geometry seam advances the fail-closed world reducer exactly once
	// for each main render epoch. HDR may consume only the admitted ticket cached for
	// that same epoch while the reduced-world ownership token is still live.
	[[nodiscard]] ResetPolicy::WorldAdmissionTicket CurrentQualityWorldAdmission() noexcept;
	[[nodiscard]] bool BeginQualityHandback(std::uintptr_t a_manager) noexcept;
	[[nodiscard]] bool QualityHandbackCommitReady(std::uintptr_t a_manager) noexcept;
	[[nodiscard]] bool AbortQualityHandback() noexcept;

	// Read-only mirror of this module's own one-way latch, for HDRRoundTrip's recovery
	// predicate. This latch has no recovery of its own yet; nothing here clears it.
	[[nodiscard]] bool DynamicGeometrySessionDisabled() noexcept;

	// The reason for the disable that first tripped the latch above this session, or
	// "none" before any. First-failure-wins: a later disable advances the failure
	// counter in DynamicResolutionDispatchState but never replaces this string.
	[[nodiscard]] const char* DynamicGeometryDisableReason() noexcept;

	// Slot 21 shares the verified manager dispatch hook with HDR. During a reduced-input/
	// full-output NGX handback the live ratios are unity, the selected dynamic triangle
	// is refreshed for the effects that opt into it, and slot 21 itself reports inactive
	// so the range routes TemporalAA's destination to RT4.
	[[nodiscard]] DynamicResolutionDispatchState DynamicResolutionDispatch() noexcept;
}
