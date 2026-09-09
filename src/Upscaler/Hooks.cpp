#include "Upscaler/Hooks.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/CompositeScale.h"
#include "Upscaler/CompositeScaleMath.h"
#include "Upscaler/Debug.h"
#include "Upscaler/DepthResolve.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Evaluate.h"
#include "Upscaler/FaultGuard.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hotkeys.h"
#include "Upscaler/HudMessages.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/MipBias.h"
#include "Upscaler/Panel.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/StereoCapture.h"
#include "Upscaler/TAAReplacement.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace Upscaler::Hooks
{
	namespace
	{
		bool g_installed = false;
		void EnsureCompositeDrawHooks() noexcept;
		std::size_t g_patched = 0;
		bool g_renderThreadProbeInstalled = false;
		std::atomic<std::uint64_t> g_renderThreadCalls{ 0 };
		std::atomic<std::uint32_t> g_renderThreadId{ 0 };
		std::atomic<std::uint64_t> g_renderThreadMismatches{ 0 };
		std::atomic<std::uintptr_t> g_observedRenderer{ 0 };
		std::atomic<std::uint64_t> g_rendererMismatches{ 0 };
		bool g_temporalAAProbeInstalled = false;
		std::atomic<std::uint64_t> g_temporalAACalls{ 0 };
		std::atomic<std::uint32_t> g_temporalAAThreadId{ 0 };
		std::atomic<std::uint64_t> g_temporalAAThreadMismatches{ 0 };
		std::atomic<std::uintptr_t> g_temporalAAEffect{ 0 };
		std::atomic<std::uint64_t> g_temporalAAEffectMismatches{ 0 };
		std::atomic<std::uint64_t> g_temporalAANativeDraws{ 0 };
		std::atomic<std::uint64_t> g_temporalAAFallbackNativeDraws{ 0 };
		bool g_imageSpacePixelDrawProbeInstalled = false;
		thread_local std::uint64_t g_activeTemporalAACall = 0;
		bool g_hdrInputProbeInstalled = false;
		std::atomic<std::uintptr_t> g_hdrEffect{ 0 };
		std::atomic<std::uint64_t> g_hdrCalls{ 0 };
		std::atomic<std::uint32_t> g_hdrThreadId{ 0 };
		std::atomic<std::uint64_t> g_hdrThreadMismatches{ 0 };
		std::atomic<std::int32_t> g_hdrSourceLogical{ -1 };
		std::atomic<std::int32_t> g_hdrDestinationLogical{ -1 };
		std::atomic<std::uint64_t> g_hdrRouteMismatches{ 0 };
		std::atomic<std::uintptr_t> g_dynamicResolutionEffect{ 0 };
		std::atomic<std::uintptr_t> g_dynamicResolutionEffectVtable{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionCalls{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionSuppressions{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionPassthroughs{ 0 };
		bool g_dynamicResolutionIsActiveProbeInstalled = false;
		bool g_dynamicResolutionGeometryUpdateProbeInstalled = false;
		bool g_lateImageSpaceRangeProbeInstalled = false;
		bool g_jitterCommitInstalled = false;
		bool g_uiRenderProbeInstalled = false;
		std::atomic<std::uint64_t> g_dynamicResolutionGeometryRefreshes{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionGeometryFlagPreflights{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionDynamicGeometryDispatches{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionDynamicGeometryFailures{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionMissingTaaClosures{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionPreWorldCalls{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionPreWorldRepairs{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionPreWorldFailures{ 0 };
		std::atomic<std::uint64_t> g_dynamicResolutionStaleWatchdogRestores{ 0 };
		std::atomic<std::int32_t> g_dynamicResolutionNeutralizedSourceLogical{ -1 };
		std::atomic<std::int32_t> g_dynamicResolutionNeutralizedDestinationLogical{ -1 };
		std::atomic_bool g_unbindDepthStencil{ true };
		std::atomic<std::uint64_t> g_imageSpaceDepthOverrideAttempts{ 0 };
		std::atomic<std::uint64_t> g_imageSpaceDepthOverrideNonNull{ 0 };
		std::atomic<std::uint64_t> g_imageSpaceDepthOverrideAlreadyNull{ 0 };
		std::atomic<std::uint64_t> g_imageSpaceDepthOverrideDraws{ 0 };
		std::atomic<std::uint64_t> g_imageSpaceDepthOverrideRestores{ 0 };
		std::atomic<std::uint64_t> g_imageSpaceDepthOverrideFailures{ 0 };
		std::atomic<std::uint64_t> g_imageSpaceDepthOverrideRestoreMismatches{ 0 };
		std::atomic_bool g_imageSpaceDepthOverrideFailureLogged{ false };
		// The seam-exit rebind experiment. Off in the shipped configuration; see
		// rebindDepthStencilAfterSeam in the TOML for what it is measuring.
		// Ordering: the settings-apply thread publishes it with the release store in
		// Configure (:1980), and the render thread does one acquire load per published
		// frame at the seam's exit (:1352); the self-check reader does another (:2238).
		// The pair orders nothing but the flag itself - it guards no data the render
		// thread has to see first - so it buys only the flip's own visibility, on the
		// next published frame. The five counters below are relaxed for the same
		// reason: nothing reads them to decide anything.
		std::atomic_bool g_rebindDepthStencilAfterSeam{ false };
		std::atomic<std::uint64_t> g_seamDepthRebindAttempts{ 0 };
		std::atomic<std::uint64_t> g_seamDepthRebinds{ 0 };
		std::atomic<std::uint64_t> g_seamDepthRebindAlreadyBound{ 0 };
		std::atomic<std::uint64_t> g_seamDepthRebindDeclines{ 0 };
		std::atomic<std::uint64_t> g_seamDepthRebindFailures{ 0 };
		std::atomic_bool g_seamDepthRebindFailureLogged{ false };
		// The seam-exit stencil clear experiment. Off in the shipped configuration; see
		// clearStencilAfterSeam in the TOML for what it is measuring. Same ordering as the
		// rebind flag above and for the same reason: one release store from the settings
		// thread in Configure, one acquire load per published frame at the seam's exit,
		// and the pair buys only the flip's own visibility on the next published frame.
		// The four counters are relaxed because nothing reads them to decide anything.
		std::atomic_bool g_clearStencilAfterSeam{ false };
		std::atomic<std::uint64_t> g_seamStencilClearAttempts{ 0 };
		std::atomic<std::uint64_t> g_seamStencilClears{ 0 };
		std::atomic<std::uint64_t> g_seamStencilClearDeclines{ 0 };
		std::atomic_bool g_seamStencilClearDeclineLogged{ false };
		// The seam-exit depth-stencil resolve experiment (resolveDepthAfterSeam), read
		// live on exactly the terms of the two keys above. The counters it needs live in
		// DepthResolve beside the objects the pass owns; only the flag is here, because
		// only this file knows what a seam-published reduced frame is.
		std::atomic_bool g_resolveDepthAfterSeam{ false };
		// The composite reconstruction scale (compositeReconstructionScale). Read live
		// exactly like the three keys above, but at a different seam: the deferred
		// composite's own technique setup, which runs during the world and not at the
		// TemporalAA exit. The counters live in CompositeScale beside the copy it owns.
		std::atomic_bool g_compositeReconstructionScale{ false };
		std::atomic_bool g_dynamicResolutionReassertFailureLogged{ false };
		std::atomic_bool g_dynamicGeometrySessionDisabled{ false };
		std::atomic_bool g_dynamicGeometryFailureLogged{ false };
		// First-failure-wins: the reason for the disable that first tripped the latch
		// above this session. Written exactly once, on the render thread that wins the
		// g_dynamicGeometryFailureLogged exchange in DisableDynamicGeometryTransaction,
		// before that guard's log call. Every call site passes a string literal, so the
		// raw pointer stays valid for the life of the process - nothing here owns or
		// frees it. nullptr until the first failure; DynamicGeometryDisableReason() below
		// is the only reader and turns that into "none".
		std::atomic<const char*> g_dynamicGeometryDisableReason{ nullptr };
		thread_local std::uint64_t g_activeHDRCall = 0;
		thread_local std::int32_t g_activeHDRDestination = -1;
		thread_local DynamicResolution::EffectPassthroughToken g_dynamicEffectPassthrough{};
		thread_local bool g_dynamicEffectNeutralizationPending = false;
		thread_local bool g_dynamicNeutralizationSawTaa = false;
		thread_local bool g_dynamicNeutralizationTaaSucceeded = false;
		thread_local bool g_dynamicNeutralizationSawEffect = false;
		thread_local std::uint64_t g_dynamicExpectedTaaCall = 0;
		thread_local std::uintptr_t g_dynamicGeometryEffect = 0;
		thread_local std::uintptr_t g_dynamicGeometryFlagAddress = 0;
		thread_local bool g_dynamicGeometryFlagArmed = false;
		thread_local std::uint64_t g_preWorldEpoch = 0;
		thread_local std::uint64_t g_dynamicTransactionEpoch = 0;
		thread_local DynamicResolution::WorldContractToken g_worldContract{};
		thread_local ResetPolicy::WorldAdmissionTicket g_worldAdmission{};
		thread_local std::uint64_t g_worldContractEpoch = 0;
		thread_local std::uintptr_t g_worldContractManager = 0;
		thread_local bool g_dynamicGeometryRefreshUnrecoverable = false;
		thread_local bool g_qualityImageSpaceDepthOverrideActive = false;
		thread_local bool g_qualityImageSpaceDepthOverrideFailed = false;

		// The hook-struct shape. A call-site hook keeps func, because the original still
		// runs. An entry-replace hook - write_branch<5> over a function that is exactly
		// five bytes - has no func at all, because the original is never called again.
		//
		// Hook the call site when only some callers should change. That is the usual
		// case, and hooking the callee instead would capture callers whose behaviour has
		// to stay vanilla. JitterCommitHook below is the worked example: the callee has
		// two callers and only the main-world one may be touched. Hook the function entry
		// only when every caller including engine job threads must be intercepted, and
		// only when the function is small enough to branch over safely.

		// A call-site patch is five bytes. The cap is what keeps the verifier's read
		// buffer bounded and POD.
		constexpr std::size_t kMaxSiteBytes = 16;
		constexpr std::size_t kEntryPatchBytes = 5;
		constexpr std::size_t kAbsoluteJumpBytes = 14;

		template <std::size_t GatewayBytes, class Hook>
		void InstallEntryProbe(std::uintptr_t a_site)
		{
			static_assert(GatewayBytes >= kEntryPatchBytes);
			// The preserved prefix contains complete relocatable instructions.
			// Preserve it in a gateway, then use an indirect RIP jump past that prefix so no
			// volatile register is consumed and the thunk can call the whole vanilla body.
			auto& trampoline = F4SE::GetTrampoline();
			auto* const gateway = static_cast<std::uint8_t*>(
				trampoline.allocate(GatewayBytes + kAbsoluteJumpBytes));
			std::memcpy(gateway, reinterpret_cast<const void*>(a_site), GatewayBytes);

			static constexpr std::uint8_t kJumpIndirectRip[] = {
				0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
			};
			std::memcpy(gateway + GatewayBytes, kJumpIndirectRip, sizeof(kJumpIndirectRip));
			const auto resume = a_site + GatewayBytes;
			std::memcpy(
				gateway + GatewayBytes + sizeof(kJumpIndirectRip),
				&resume,
				sizeof(resume));
			static_cast<void>(::FlushInstructionCache(
				::GetCurrentProcess(), gateway, GatewayBytes + kAbsoluteJumpBytes));

			Hook::func = reinterpret_cast<std::uintptr_t>(gateway);
			// write_branch's return value is meaningful only when replacing an existing
			// branch. This is a function entry, so the gateway above is the original.
			static_cast<void>(trampoline.write_branch<5>(a_site, Hook::thunk));
		}

		template <class Hook>
		void InstallFiveByteEntryProbe(std::uintptr_t a_site)
		{
			InstallEntryProbe<kEntryPatchBytes, Hook>(a_site);
		}

		// Replaces the main-world "call US_BSGraphicsState_UpdateTAAJitter(state)" at
		// Addr::kJitterUpdateCall. The original runs first and unconditionally: the phase
		// counter at state+0x00, its parity at +0x0C/+0x10 and the gates-closed zeroing
		// path all stay engine-owned, and US_ImageSpaceEffectTemporalAA_Render reads that
		// parity to select its history target. Only the two projection floats are then
		// overwritten, and only while a backend sequence is armed.
		//
		// The site is the call rather than the callee so the other caller - from
		// DisplayLoadingScreen - keeps the vanilla sequence.
		struct JitterCommitHook
		{
			static void thunk(std::uintptr_t a_state)
			{
				func(a_state);
				if (!Jitter::BackendSequenceArmed() ||
					ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
					return;
				}
				// Both outputs are latched into the Jitter telemetry; the hook itself has
				// no use for them, and a decline leaves the engine's own sample standing.
				float committedX = 0.0f;
				float committedY = 0.0f;
				static_cast<void>(Jitter::CommitFrame(committedX, committedY));
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallJitterCommit(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<JitterCommitHook>(a_site);
		}

		// Wraps the UI render entry at Addr::kUiRenderAllCall - the Interface3D call inside
		// US_RenderWorldImageSpaceSequence, 0x43 bytes after the late-range call this file
		// already owns, with only the hidden-area mesh's own branch in between.
		//
		// It exists to answer one question no other seam can: what the HUD draw runs
		// under. The frame census sees the ImageSpace chain and the HUD is not in it, and
		// neither of this plugin's two capture points has the compass in the submitted
		// target on either path, so the jitter floats, the four dynamic-resolution ratios
		// and the viewport at the UI pass are only readable here.
		//
		// The site is an indirect call through a slot the frame setup rewrites every
		// frame (Addresses.h names the writer), so the thunk calls through the slot rather
		// than through a cached pointer: a build that resolved the callee once would keep
		// calling a stale one the engine had replaced. write_call<6> hands back the slot's
		// own address, which is exactly what has to be re-read here.
		//
		// Read-only and inert unless a run is armed: the sample is taken before the
		// original, the original then runs with the argument it was called with, and
		// nothing this hook does is visible to the engine. ECX at the call site is the
		// ImageSpace range's final destination target id and is recorded as such.
		std::uintptr_t g_uiRenderCallbackSlot = 0;
		std::atomic<std::uint64_t> g_uiRenderNullCallbacks{ 0 };

		struct UiRenderAllHook
		{
			using Callback = void(__fastcall*)(std::uint32_t);

			static void thunk(std::uint32_t a_targetId)
			{
				Debug::OnUiRenderEntry(static_cast<std::int32_t>(a_targetId));
				const auto* const slot = reinterpret_cast<Callback*>(g_uiRenderCallbackSlot);
				// A null slot is what the engine itself would have called, so declining is
				// the only behaviour-preserving answer available: calling nothing leaves
				// the frame exactly as the vanilla crash would not have. It is counted
				// because a session that ever sees one has a frame this hook changed.
				if (!slot || !*slot) {
					g_uiRenderNullCallbacks.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				(*slot)(a_targetId);
			}
		};

		void InstallUiRenderProbe(std::uintptr_t a_site)
		{
			g_uiRenderCallbackSlot = F4SE::GetTrampoline().write_call<6>(
				a_site, UiRenderAllHook::thunk);
		}

		struct DynamicResolutionIsActiveProbeHook
		{
			static bool thunk(std::uintptr_t a_effect)
			{
				// An owned frame runs the post-HDR chain at unity, where the vanilla
				// predicate already answers inactive; answering it explicitly is the
				// contract. Inactive makes TemporalAA the last active effect of slots
				// 16..22, and RenderEffectRange routes the last active effect's destination
				// to the range's final target, RT4 - the no-DRS frame shape - so the seam
				// publish and the native fallback both write history and RT4 directly and
				// no slot-21 copy follows them.
				const bool ownsTransaction = g_dynamicEffectNeutralizationPending &&
				                             a_effect == g_dynamicGeometryEffect &&
				                             g_dynamicTransactionEpoch != 0 &&
				                             g_dynamicTransactionEpoch == g_preWorldEpoch &&
				                             ScopePolicy::ClassifyCurrentDispatch() ==
													 ScopePolicy::DispatchScope::kMain;
				return !ownsTransaction && DynamicResolution::NativeEffectActive();
			}
		};

		void InstallDynamicResolutionIsActiveProbe(std::uintptr_t a_site)
		{
			// The first instruction is RIP-relative and cannot be copied into the generic
			// gateway. The replacement above reproduces the complete verified predicate.
			static_cast<void>(F4SE::GetTrampoline().write_branch<5>(
				a_site, DynamicResolutionIsActiveProbeHook::thunk));
		}

		void ObserveClearColor(std::uintptr_t a_renderer) noexcept
		{
			const auto call = g_renderThreadCalls.fetch_add(1, std::memory_order_relaxed) + 1;

			// The context does not exist when Install runs, so the four draw slots the
			// composite correction needs are taken here instead - the first render-thread
			// frame at which a context is resolvable. One relaxed load after that.
			EnsureCompositeDrawHooks();

			const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
			auto expectedThread = std::uint32_t{ 0 };
			if (!g_renderThreadId.compare_exchange_strong(
					expectedThread, thread, std::memory_order_relaxed, std::memory_order_relaxed) &&
				expectedThread != thread) {
				g_renderThreadMismatches.fetch_add(1, std::memory_order_relaxed);
			}

			auto expectedRenderer = std::uintptr_t{ 0 };
			if (!g_observedRenderer.compare_exchange_strong(
					expectedRenderer, a_renderer, std::memory_order_relaxed, std::memory_order_relaxed) &&
				expectedRenderer != a_renderer) {
				g_rendererMismatches.fetch_add(1, std::memory_order_relaxed);
			}

			if (g_renderThreadId.load(std::memory_order_relaxed) == thread) {
				HDRRoundTrip::OnPrimaryRenderThread();
				Debug::OnPrimaryClearColor(a_renderer, call, thread);

				// This is BSGraphics::Renderer::ClearColor's function entry (a Tier 2
				// soft site, patched unconditionally at Install() time - see
				// kSoftSiteStorage below, not gated by live mode), filtered to the one
				// thread this block has already proven is the primary render thread -
				// the same reason HDRRoundTrip::OnPrimaryRenderThread anchors its own
				// per-frame work right here. That thread calls ClearColor every frame
				// regardless of whether the world is admitted, a mode is active, or a
				// menu is open, so this is a genuine once-per-frame seam and not a
				// per-eye or per-job-thread one. On an ordinary frame neither poll
				// below allocates: a GetForegroundWindow/GetWindowThreadProcessId pair,
				// one hashed menu-map lookup, up to two GetAsyncKeyState reads and a
				// couple of atomics for Hotkeys, two lock-free Snapshot() reads for
				// HudMessages. One engine lock IS taken, every frame and deliberately:
				// Hotkeys' text-input gate reads RE::UI's menu map under
				// GetMenuMapRWLock's read side, held across a single lookup that
				// touches no refcount - the same lock, on this same thread, that
				// ResetPolicy's own world observation already takes each frame (see
				// ResetPolicy::TextInputMenuOpen). Two rare frames are the further
				// exceptions, both bounded and both deliberate. A frame that enqueues a HUD line takes
				// HudMessages' own short internal ring mutex - see HudMessages.h for
				// why that lock is accepted here. A frame carrying a TOGGLE press edge
				// builds a mode-switch snapshot's strings, publishes the runtime mode
				// override, writes one log line and takes Configuration's switch-worker
				// mutex to hand the request off; that lock is held for the few
				// instructions that publish a request and nothing else, because the
				// worker releases it before it waits or applies. So this thread pays a
				// hand-off and never the switch itself - which is the point of the
				// worker, the deny window a switch waits on being one only this thread
				// can ever advance. Outside any __try frame and outside every isolated
				// draw scope, same as this whole function.
				Hotkeys::PollFrame();
				HudMessages::PollFrame();
				// The panel is third and last on this seam because it is the only one of
				// the three that can be asked to do real work here: while it is visible
				// it runs an ImGui frame, one isolated draw into its own 1024x768
				// texture, one SetOverlayTexture and one drain of the overlay event
				// queue. Hidden - the default, and the state it is in on every frame
				// nobody has pressed the panel key - it costs two atomic loads and
				// returns, which is why it can sit on a frame producer at all. It runs
				// after the two polls above rather than before so a key press that flips
				// the panel's intent is honoured on this same frame instead of the next
				// one. Like them it is outside every __try frame and outside every
				// isolated draw scope: the panel opens its own isolation scope inside
				// PollFrame and closes it before returning.
				Panel::PollFrame();
			}
		}

		struct ClearColorProbeHook
		{
			static void thunk(std::uintptr_t a_renderer)
			{
				ObserveClearColor(a_renderer);
				func(a_renderer);
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallClearColorProbe(std::uintptr_t a_site)
		{
			InstallFiveByteEntryProbe<ClearColorProbeHook>(a_site);
		}

		[[nodiscard]] std::uint64_t ObserveTemporalAA(std::uintptr_t a_effect) noexcept
		{
			const auto call = g_temporalAACalls.fetch_add(1, std::memory_order_relaxed) + 1;

			const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
			auto expectedThread = std::uint32_t{ 0 };
			if (!g_temporalAAThreadId.compare_exchange_strong(
					expectedThread, thread, std::memory_order_relaxed, std::memory_order_relaxed) &&
				expectedThread != thread) {
				g_temporalAAThreadMismatches.fetch_add(1, std::memory_order_relaxed);
			}

			auto expectedEffect = std::uintptr_t{ 0 };
			if (!g_temporalAAEffect.compare_exchange_strong(
					expectedEffect, a_effect, std::memory_order_relaxed, std::memory_order_relaxed) &&
				expectedEffect != a_effect) {
				g_temporalAAEffectMismatches.fetch_add(1, std::memory_order_relaxed);
			}
			return call;
		}

		[[nodiscard]] std::uintptr_t ResolveDynamicResolutionEffect(std::uintptr_t a_manager) noexcept;
		[[nodiscard]] std::int32_t ResolveEffectSlot(std::uintptr_t a_manager, std::uintptr_t a_effect) noexcept;
		[[nodiscard]] bool EndDynamicNeutralization(bool a_refreshGeometry) noexcept;
		[[nodiscard]] bool RestoreDynamicGeometryFlag() noexcept;
		void DisableDynamicGeometryTransaction(std::string_view a_reason) noexcept;

		struct TemporalAAProbeHook
		{
			static void thunk(
				std::uintptr_t a_effect,
				std::uintptr_t a_source,
				std::uintptr_t a_arg3,
				std::uintptr_t a_arg4)
			{
				const auto call = ObserveTemporalAA(a_effect);
				const auto previousCall = g_activeTemporalAACall;
				const bool qualityMarkerPresent = g_dynamicEffectNeutralizationPending ||
				                                  g_dynamicEffectPassthrough.active ||
				                                  g_dynamicGeometryFlagArmed ||
				                                  g_dynamicTransactionEpoch != 0;
				const bool ownsQualityTransaction = g_dynamicEffectNeutralizationPending &&
				                                     g_dynamicTransactionEpoch != 0 &&
				                                     g_dynamicTransactionEpoch == g_preWorldEpoch;
				if (ownsQualityTransaction && g_dynamicExpectedTaaCall == 0) {
					g_dynamicExpectedTaaCall = call;
				}
				g_activeTemporalAACall = call;
				bool normalReturn = false;
				__try {
					func(a_effect, a_source, a_arg3, a_arg4);
					normalReturn = true;
				} __finally {
					g_activeTemporalAACall = previousCall;
					if (!normalReturn && qualityMarkerPresent) {
						// With RT1 written and no draw yet, the bracket stays up and the late
						// range closes the frame as a native fallback. A frame the seam already
						// published cannot arm one and is released here without a repair, as
						// is a frame that cannot fall back.
						bool fallbackArmed = false;
						__try {
							fallbackArmed = HDRRoundTrip::QualityNativeFallbackArmed() ||
							                HDRRoundTrip::ArmQualityNativeFallback(
												HDRRoundTrip::QualityFallbackSeam::kAbnormalTaaReturn);
						} __except (EXCEPTION_EXECUTE_HANDLER) {
							fallbackArmed = false;
						}
						if (!fallbackArmed) {
							bool geometryRestored = false;
							bool pixelsRestored = false;
							__try {
								geometryRestored = EndDynamicNeutralization(true);
							} __except (EXCEPTION_EXECUTE_HANDLER) {
								geometryRestored = false;
							}
							__try {
								pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
							} __except (EXCEPTION_EXECUTE_HANDLER) {
								pixelsRestored = false;
							}
							if (!geometryRestored || !pixelsRestored) {
								DisableDynamicGeometryTransaction("abnormal TemporalAA return could not close the complete quality transaction"sv);
								if (!geometryRestored && pixelsRestored) {
									ResetPolicy::RequestQualityAbortReset();
								}
							}
						}
					}
				}
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallTemporalAAProbe(std::uintptr_t a_site)
		{
			InstallFiveByteEntryProbe<TemporalAAProbeHook>(a_site);
		}

		void ClearDynamicNeutralizationMarkers() noexcept
		{
			g_dynamicEffectNeutralizationPending = false;
			g_dynamicNeutralizationSawTaa = false;
			g_dynamicNeutralizationTaaSucceeded = false;
			g_dynamicNeutralizationSawEffect = false;
			g_dynamicExpectedTaaCall = 0;
			g_dynamicEffectPassthrough.manager = 0;
			g_dynamicGeometryEffect = 0;
			g_dynamicGeometryFlagAddress = 0;
			g_dynamicTransactionEpoch = 0;
			g_qualityImageSpaceDepthOverrideActive = false;
			g_qualityImageSpaceDepthOverrideFailed = false;
		}

		void DisableDynamicGeometryTransaction(std::string_view a_reason) noexcept
		{
			g_dynamicGeometrySessionDisabled.store(true, std::memory_order_release);
			g_dynamicResolutionDynamicGeometryFailures.fetch_add(1, std::memory_order_relaxed);
			if (!g_dynamicGeometryFailureLogged.exchange(true, std::memory_order_relaxed)) {
				// a_reason is always a string literal (every call site passes one with the
				// "sv" suffix), so its data pointer is valid for the life of the process.
				g_dynamicGeometryDisableReason.store(a_reason.data(), std::memory_order_release);
				logger::error(FMT_STRING("quality handback dynamic-geometry transaction disabled: {}"), a_reason);
			}
		}

		[[nodiscard]] bool ReadEffectByte(std::uintptr_t a_address, std::uint8_t& a_value) noexcept
		{
			return a_address != 0 && FaultGuard::SafeReadBytes(
				reinterpret_cast<const void*>(a_address), &a_value, sizeof(a_value));
		}

		[[nodiscard]] bool WriteEffectByte(std::uintptr_t a_address, std::uint8_t a_value) noexcept
		{
			return a_address != 0 && FaultGuard::SafeWriteBytes(
				reinterpret_cast<void*>(a_address), &a_value, sizeof(a_value));
		}

		[[nodiscard]] std::uintptr_t ResolveDynamicResolutionVtable() noexcept
		{
			auto vtable = g_dynamicResolutionEffectVtable.load(std::memory_order_acquire);
			if (vtable != 0) {
				return vtable;
			}
			vtable = Engine::RipResolve(
				Addr::kDynamicResolutionEffectVtableAnchor,
				{ 0x48, 0x8D, 0x05 },
				Addr::kRipDispOffset,
				Addr::kRipInstrLen,
				"dynamic-resolution effect vtable"sv);
			if (vtable != 0) {
				g_dynamicResolutionEffectVtable.store(vtable, std::memory_order_release);
			}
			return vtable;
		}

		[[nodiscard]] bool PreflightDynamicGeometryFlag(std::uintptr_t a_manager) noexcept
		{
			g_dynamicResolutionGeometryFlagPreflights.fetch_add(1, std::memory_order_relaxed);
			const auto effect = ResolveDynamicResolutionEffect(a_manager);
			const auto expectedVtable = ResolveDynamicResolutionVtable();
			if (effect == 0 || expectedVtable == 0) {
				DisableDynamicGeometryTransaction("slot 21 or its verified vtable did not resolve"sv);
				return false;
			}

			std::uintptr_t actualVtable = 0;
			std::uintptr_t renderEntry = 0;
			std::uintptr_t isActiveEntry = 0;
			const REL::Relocation<std::uintptr_t> expectedRender{ REL::Offset(Addr::kDynamicResolutionEffectRenderFn) };
			const REL::Relocation<std::uintptr_t> expectedIsActive{ REL::Offset(Addr::kDynamicResolutionIsActiveFn) };
			if (!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(effect), &actualVtable, sizeof(actualVtable)) ||
				actualVtable != expectedVtable ||
				!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(actualVtable + Addr::kDynamicResolutionEffectVtableRenderSlot), &renderEntry, sizeof(renderEntry)) ||
				!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(actualVtable + Addr::kDynamicResolutionEffectVtableIsActiveSlot), &isActiveEntry, sizeof(isActiveEntry)) ||
				renderEntry != expectedRender.address() || isActiveEntry != expectedIsActive.address()) {
				DisableDynamicGeometryTransaction("slot 21 vtable identity or its render/IsActive entries did not match"sv);
				return false;
			}

			const auto flagAddress = effect + 0xA8;
			std::uint8_t flag = 0xFF;
			std::uint8_t probe = 0xFF;
			if (!ReadEffectByte(flagAddress, flag) || flag != 0) {
				DisableDynamicGeometryTransaction("slot 21 +0xA8 was unreadable or not engine-owned zero"sv);
				return false;
			}
			g_dynamicGeometryEffect = effect;
			g_dynamicGeometryFlagAddress = flagAddress;
			const bool wroteOne = WriteEffectByte(flagAddress, 1);
			if (wroteOne) {
				// Ownership begins when our write succeeds, not when readback later does.
				// A failed restore therefore remains visible to every watchdog.
				g_dynamicGeometryFlagArmed = true;
			}
			const bool entered = wroteOne && ReadEffectByte(flagAddress, probe) && probe == 1;
			const bool restored = wroteOne && RestoreDynamicGeometryFlag();
			// The guarded 0 -> 1 -> 0 probe above has already run to completion and
			// RestoreDynamicGeometryFlag has already put the byte back to zero when
			// wroteOne is true - this only decides whether the failure branch below fires,
			// after the fact, so kGeometryPreflightFailure can never skip the restore.
			// Reuses the same refusal a genuine entered/restored failure takes rather than
			// inventing a new reason string: the injected and genuine cases are made
			// indistinguishable in effect, same as HDRRoundTrip's copyCloseFailure: the
			// separate qualityFaultInjection.lastTriggeredName channel is what tells them
			// apart. Gated on wroteOne so an arm landing on a frame where the write itself
			// never issued - already disabled above through a different refusal - is not
			// silently consumed on a frame that would prove nothing.
			const bool faultInjected = wroteOne &&
			                           HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kGeometryPreflightFailure);
			if (!entered || !restored || faultInjected) {
				DisableDynamicGeometryTransaction("slot 21 +0xA8 failed the guarded 0 -> 1 -> 0 preflight"sv);
				if (!g_dynamicGeometryFlagArmed) {
					ClearDynamicNeutralizationMarkers();
				}
				return false;
			}
			g_dynamicGeometryEffect = effect;
			g_dynamicGeometryFlagAddress = flagAddress;
			return true;
		}

		[[nodiscard]] bool ArmDynamicGeometryFlag() noexcept
		{
			std::uint8_t flag = 0xFF;
			if (g_dynamicGeometryFlagAddress == 0 ||
				!ReadEffectByte(g_dynamicGeometryFlagAddress, flag) || flag != 0) {
				DisableDynamicGeometryTransaction("slot 21 +0xA8 changed ownership between preflight and commit"sv);
				return false;
			}
			const bool wroteOne = WriteEffectByte(g_dynamicGeometryFlagAddress, 1);
			if (wroteOne) {
				g_dynamicGeometryFlagArmed = true;
			}
			const bool armed = wroteOne && ReadEffectByte(g_dynamicGeometryFlagAddress, flag) && flag == 1;
			if (!armed) {
				if (wroteOne) {
					static_cast<void>(RestoreDynamicGeometryFlag());
				}
				DisableDynamicGeometryTransaction("slot 21 +0xA8 could not enter the committed dynamic-geometry state"sv);
				return false;
			}
			return true;
		}

		[[nodiscard]] bool RestoreDynamicGeometryFlagSilent() noexcept
		{
			if (!g_dynamicGeometryFlagArmed) {
				return true;
			}
			std::uint8_t flag = 0xFF;
			const bool restored = WriteEffectByte(g_dynamicGeometryFlagAddress, 0) &&
			                      ReadEffectByte(g_dynamicGeometryFlagAddress, flag) && flag == 0;
			if (restored) {
				g_dynamicGeometryFlagArmed = false;
				g_dynamicGeometryFlagAddress = 0;
				return true;
			}
			return false;
		}

		[[nodiscard]] bool RestoreDynamicGeometryFlag() noexcept
		{
			if (RestoreDynamicGeometryFlagSilent()) {
				return true;
			}
			DisableDynamicGeometryTransaction("slot 21 +0xA8 did not restore to zero"sv);
			return false;
		}

		[[nodiscard]] bool CloseReducedWorldFrame(
			std::uintptr_t a_manager,
			bool a_refreshGeometry) noexcept;

		struct DynamicResolutionGeometryUpdateHook
		{
			static void thunk(std::uintptr_t a_manager)
			{
				const auto dispatchScope = ScopePolicy::ClassifyCurrentDispatch();
				if (dispatchScope == ScopePolicy::DispatchScope::kScoped) {
					// A scope render may be nested inside the main-world ownership window.
					// It neither advances admission nor steals that outer token.
					func(a_manager);
					return;
				}

				// A census frame is the span between two of these calls, so it covers the
				// whole ImageSpace chain including everything the late range runs after
				// the TemporalAA seam. Idle unless one is armed.
				Debug::OnFrameBoundary();

				// The stale watchdog for the sampler table, ahead of everything that can
				// decide not to run. Restore is a no-op when nothing is applied, and this
				// is the one point in the frame that runs whether or not the previous
				// world frame reached its close.
				MipBias::Restore();

				++g_preWorldEpoch;
				g_dynamicResolutionPreWorldCalls.fetch_add(1, std::memory_order_relaxed);
				const bool staleInner = g_dynamicEffectPassthrough.active || g_dynamicGeometryFlagArmed ||
				                        g_dynamicEffectNeutralizationPending || g_dynamicTransactionEpoch != 0;
				const bool staleWorld = g_worldContract.ownsMutation || g_worldContract.active ||
				                        DynamicResolution::WorldFrameActive();
				const bool stale = staleInner || staleWorld;
				const bool drifted = DynamicResolution::FrameContractDrifted();
				if (stale || drifted) {
					g_dynamicResolutionPreWorldRepairs.fetch_add(1, std::memory_order_relaxed);
				}
				bool transactionRestored = true;
				bool pixelsRestored = true;
				if (staleInner) {
					transactionRestored = EndDynamicNeutralization(false);
					pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
					transactionRestored = transactionRestored && pixelsRestored;
					if (transactionRestored) {
						g_dynamicResolutionStaleWatchdogRestores.fetch_add(1, std::memory_order_relaxed);
					}
				}
				const bool worldRestored = !staleWorld || CloseReducedWorldFrame(a_manager, false);
				const bool contractRestored = worldRestored &&
				                              DynamicResolution::RestoreRequestedBeforeWorld();
				if (!transactionRestored && contractRestored && staleInner) {
					// A short first write attempt must not strand the transaction. The explicit
					// outer restore above may have repaired ratios, so retry token closure once.
					transactionRestored = EndDynamicNeutralization(false) && pixelsRestored;
				}
				const bool repaired = transactionRestored && contractRestored;
				if (repaired && (stale || drifted)) {
					ResetPolicy::RequestWorldReset();
				}
				if (repaired && !g_dynamicEffectPassthrough.active && !g_dynamicGeometryFlagArmed) {
					ClearDynamicNeutralizationMarkers();
				} else if (!repaired) {
					g_dynamicResolutionPreWorldFailures.fetch_add(1, std::memory_order_relaxed);
					DisableDynamicGeometryTransaction("the pre-world stale watchdog could not restore the quality transaction or the reduced-world contract"sv);
				}

				g_worldAdmission = {};
				g_worldContractEpoch = 0;
				g_worldContractManager = 0;
				if (repaired && dispatchScope == ScopePolicy::DispatchScope::kMain &&
					DynamicResolution::WorldAdmissionGated() && QualityHandbackReady()) {
					// One load per dispatch, handed to the gate by value: HDRRoundTrip's own
					// latch is the one QualityHandbackReady above cannot fold in, because
					// that predicate is called from inside that file's dispatch. Without it
					// a disabled or suspended session keeps opening this bracket, and the
					// engine rasterizes the world reduced with the hidden-area mesh off and
					// finishes it with its own slot-21 upsample while nothing reconstructs.
					// The gate denies on it exactly as it denies on the operator veto.
					const bool reconstructionUnavailable = !HDRRoundTrip::ReducedWorldHandbackAvailable();
					const auto admission =
						ResetPolicy::AdvanceQualityWorldAdmissionBeforeWorld(reconstructionUnavailable);
					if (admission.admitted) {
						const bool began = DynamicResolution::BeginReducedWorld(g_worldContract);
						const bool revalidated = began &&
						                         ResetPolicy::RevalidateQualityWorldAdmission(admission);
						if (revalidated) {
							g_worldAdmission = admission;
							g_worldContractEpoch = g_preWorldEpoch;
							g_worldContractManager = a_manager;
							// The world contract has latched, so this frame really will
							// rasterize reduced and the biased samplers are what it
							// should sample with. Everything after this point exits
							// through CloseReducedWorldFrame, which restores first.
							MipBias::Apply();
						} else if (began && !CloseReducedWorldFrame(a_manager, false)) {
							g_dynamicResolutionPreWorldFailures.fetch_add(1, std::memory_order_relaxed);
							DisableDynamicGeometryTransaction("the reduced-world bracket opened but could not be closed after a failed admission revalidation"sv);
						} else if (!began) {
							g_dynamicResolutionPreWorldFailures.fetch_add(1, std::memory_order_relaxed);
							DisableDynamicGeometryTransaction("the reduced-world bracket could not be opened before the world draw"sv);
						}
					}
				} else if (repaired && dispatchScope == ScopePolicy::DispatchScope::kMain &&
					!DynamicResolution::WorldAdmissionGated() && MipBias::Built() &&
					Evaluate::Prepared() && HDRRoundTrip::ReconstructionLive()) {
					// Native one-to-one. There is no reduction contract to latch and
					// therefore no reduced-world bracket, but the vendor formula's -1 term
					// applies whenever the reconstruction runs, so the world raster still
					// has to sample biased. The matching restore is at the late ImageSpace
					// range, and the watchdog above is the backstop for a frame that never
					// reaches it.
					//
					// All three conditions are liveness, and none of them is redundant.
					// Built() falls with the configuration that derived the bias, because
					// Evaluate::ClearPreparedIdentity now forgets the build rather than
					// only restoring it. Prepared() falls when the backend and its views
					// go, which is the same seam but is also false in the window between a
					// failed prepare and the next one. ReconstructionLive() falls when the
					// session fail-closes or a continuous budget completes - neither of
					// which touches the sampler table, so without it this seam would go on
					// biasing a world raster nothing reconstructs.
					MipBias::ApplyNative();
				}
				func(a_manager);
			}

			[[nodiscard]] static bool Refresh(std::uintptr_t a_manager) noexcept
			{
				if (g_dynamicGeometryRefreshUnrecoverable) {
					return false;
				}
				bool refreshed = false;
				std::uint32_t exceptionCode = 0;
				__try {
					func(a_manager);
					refreshed = true;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
					g_dynamicGeometryRefreshUnrecoverable = true;
				}
				if (refreshed) {
					g_dynamicResolutionGeometryRefreshes.fetch_add(1, std::memory_order_relaxed);
					return true;
				}
				logger::error(FMT_STRING("dynamic screen-geometry refresh raised exception {:08X}"), exceptionCode);
				DisableDynamicGeometryTransaction("dynamic screen-geometry refresh raised an SEH exception"sv);
				return false;
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		[[nodiscard]] bool CloseReducedWorldFrame(
			std::uintptr_t a_manager,
			bool a_refreshGeometry) noexcept
		{
			// First statement, before the early return and before the ratio rollback. A
			// failed rollback that also stranded a mip bias is two bugs instead of one,
			// and the engine's shutdown releaser frees whatever is in the table.
			MipBias::Restore();

			if (!g_worldContract.ownsMutation && !g_worldContract.active) {
				g_worldContract = {};
				g_worldAdmission = {};
				g_worldContractEpoch = 0;
				g_worldContractManager = 0;
				return true;
			}

			const bool nativeRestored = DynamicResolution::EndReducedWorld(g_worldContract);
			bool geometryRestored = true;
			if (nativeRestored && a_refreshGeometry &&
				g_dynamicResolutionGeometryUpdateProbeInstalled && a_manager != 0) {
				geometryRestored = DynamicResolutionGeometryUpdateHook::Refresh(a_manager);
			}
			if (nativeRestored && geometryRestored) {
				g_worldContract = {};
				g_worldAdmission = {};
				g_worldContractEpoch = 0;
				g_worldContractManager = 0;
				return true;
			}
			return false;
		}

		void InstallDynamicResolutionGeometryUpdateProbe(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<DynamicResolutionGeometryUpdateHook>(a_site);
		}

		[[nodiscard]] bool BeginDynamicNeutralization(std::uintptr_t a_manager) noexcept
		{
			if (a_manager == 0 || g_preWorldEpoch == 0 ||
				g_dynamicGeometrySessionDisabled.load(std::memory_order_acquire)) {
				return false;
			}
			g_dynamicEffectPassthrough.manager = a_manager;
			g_dynamicTransactionEpoch = g_preWorldEpoch;
			if (!PreflightDynamicGeometryFlag(a_manager)) {
				if (!g_dynamicGeometryFlagArmed) {
					ClearDynamicNeutralizationMarkers();
				}
				return false;
			}
			if (!DynamicResolution::BeginEffectPassthrough(g_dynamicEffectPassthrough)) {
				// A partial unity write keeps the token active until the original ratios are
				// positively restored. Preserve the outer HDR closure markers in that case.
				if (!g_dynamicEffectPassthrough.active) {
					ClearDynamicNeutralizationMarkers();
				}
				return false;
			}
			g_dynamicEffectPassthrough.manager = a_manager;
			g_dynamicTransactionEpoch = g_preWorldEpoch;
			if (!DynamicResolutionGeometryUpdateHook::Refresh(a_manager)) {
				static_cast<void>(EndDynamicNeutralization(false));
				return false;
			}
			if (!ArmDynamicGeometryFlag()) {
				static_cast<void>(EndDynamicNeutralization(true));
				return false;
			}
			return true;
		}

		[[nodiscard]] bool EndDynamicNeutralization(bool a_refreshGeometry) noexcept
		{
			const auto manager = g_dynamicEffectPassthrough.manager;
			const bool effectRestored = RestoreDynamicGeometryFlag();
			const bool ratiosRestored = !g_dynamicEffectPassthrough.active ||
			                            DynamicResolution::EndEffectPassthrough(g_dynamicEffectPassthrough);
			bool geometryRestored = true;
			if (a_refreshGeometry && g_dynamicResolutionGeometryUpdateProbeInstalled && manager != 0) {
				geometryRestored = DynamicResolutionGeometryUpdateHook::Refresh(manager);
			}
			const bool restored = effectRestored && ratiosRestored && geometryRestored &&
			                      !g_dynamicGeometryFlagArmed && !g_dynamicEffectPassthrough.active;
			if (restored) {
				ClearDynamicNeutralizationMarkers();
			}
			return restored;
		}

		void FinishQualityHandbackAfterLateRange(
			std::uintptr_t a_manager,
			bool a_allowCommit) noexcept
		{
			if (!g_dynamicEffectNeutralizationPending && !g_dynamicEffectPassthrough.active &&
				!g_dynamicGeometryFlagArmed && g_dynamicTransactionEpoch == 0) {
				return;
			}
			const bool ownsEpoch = g_dynamicTransactionEpoch != 0 &&
			                       g_dynamicTransactionEpoch == g_preWorldEpoch;
			const bool definiteMain = ScopePolicy::ClassifyCurrentDispatch() ==
			                          ScopePolicy::DispatchScope::kMain;
			const bool nativeFallback = HDRRoundTrip::QualityNativeFallbackArmed();
			const bool publishedAtSeam = HDRRoundTrip::QualityPublishedAtSeam();
			const bool taaSatisfied = g_dynamicNeutralizationSawTaa &&
			                          g_dynamicNeutralizationTaaSucceeded;
			if (!g_dynamicNeutralizationSawTaa) {
				g_dynamicResolutionMissingTaaClosures.fetch_add(1, std::memory_order_relaxed);
			}
			if (g_dynamicNeutralizationSawEffect) {
				// Slot 21 reports inactive on an owned frame, so a dispatch means its
				// predicate and the live ratios disagreed and its copy of logical 1
				// overwrote whatever the seam or the native draw put in RT4.
				DisableDynamicGeometryTransaction("slot 21 dispatched on a frame the TemporalAA seam owns"sv);
			}
			const bool downstreamValid = a_allowCommit && ownsEpoch && definiteMain && taaSatisfied &&
			                             !g_qualityImageSpaceDepthOverrideFailed &&
			                             !g_dynamicNeutralizationSawEffect &&
			                             g_worldContract.active &&
			                             g_worldContractEpoch == g_preWorldEpoch &&
			                             g_worldContractManager == a_manager;
			// Return the inner unity bracket to reduced first, then restore the outer
			// native contract and rebuild native geometry. The pixel accounting is last.
			const bool innerRestored = EndDynamicNeutralization(false);
			const bool worldRestored = CloseReducedWorldFrame(a_manager, true);
			const bool restored = innerRestored && worldRestored;
			// The two real restores above have already run and put the native ratios and
			// the hidden-area byte back; this only decides whether the Disable below fires,
			// after the fact, so the reported failure lands on this existing refusal with
			// the engine genuinely restored underneath. Consumed into its own local first,
			// unconditionally, rather than short-circuited behind `!restored` - a frame
			// where the real restore already failed must still consume an armed fault, the
			// same as every other seam, so it is recorded as fired instead of left armed.
			// Kept out of `restored` itself: `restored` is read again a few lines down by
			// the seam-publish validation, and forcing it false there too would additionally
			// report that unrelated frame as failing to validate even when it genuinely
			// did - a second, misleading reason for the same injected fault.
			const bool restoreFaultInjected =
				HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kGeometryRestoreFailure);
			const bool reportRestoreFailure = !restored || restoreFaultInjected;
			if (reportRestoreFailure) {
				DisableDynamicGeometryTransaction("the late ImageSpace range could not close the ratio/geometry transaction"sv);
			}
			if (nativeFallback) {
				// The engine finished this frame itself - HDR, gamma and TemporalAA ran at
				// unity on the reconstruction and TemporalAA wrote history and RT4 - so the
				// close is the brackets above plus the accounting: no repair, no reset.
				if (!HDRRoundTrip::CloseQualityNativeFallback()) {
					DisableDynamicGeometryTransaction("the native-TemporalAA fallback frame could not close its pixel transaction at the late ImageSpace range"sv);
				}
				return;
			}
			if (publishedAtSeam) {
				// The seam draw was the publish: history and RT4 are already public, so a
				// structural mismatch here cannot un-publish them. It disables the
				// transaction for later frames; this one still commits its accounting.
				if (!downstreamValid || !restored) {
					DisableDynamicGeometryTransaction("the late ImageSpace range could not validate a frame the TemporalAA seam published"sv);
				}
				if (HDRRoundTrip::CommitQualityHandbackCandidate() != HDRRoundTrip::QualityCommitResult::kPublished) {
					if (!HDRRoundTrip::AbortQualityHandbackCandidate()) {
						DisableDynamicGeometryTransaction("a seam-published frame could not close its quality pixel transaction at the late ImageSpace range"sv);
					}
					DisableDynamicGeometryTransaction("the late ImageSpace range closed a seam-published frame without its NGX quality candidate"sv);
				}
				return;
			}
			if (HDRRoundTrip::QualityTransactionUnrecoverable()) {
				// The seam made this frame terminal - a draw issued but not proved, a fault
				// after the publish, or a refusal the fallback could not take - and that
				// close already discarded the candidate, disabled the session and owed the
				// reset. The brackets above are all the late range adds; an abort here
				// could only report the latched phase as a second failure.
				return;
			}
			// Neither published nor fallen back: the TemporalAA draw never reached the
			// seam this frame. RT1 keeps the engine's late intermediate, the range routed
			// its last active effect into RT4, and the abort repairs the selected history
			// from it.
			if (!HDRRoundTrip::AbortQualityHandbackCandidate()) {
				DisableDynamicGeometryTransaction("the rejected late ImageSpace transaction could not close the quality pixel transaction"sv);
			}
		}

		struct LateImageSpaceRangeHook
		{
			static void thunk(
				std::uintptr_t a_manager,
				std::uint32_t a_start,
				std::uint32_t a_end,
				std::uint32_t a_sourceLogical,
				std::uint32_t a_destinationLogical)
			{
				const bool qualityMarkerPresent = g_dynamicEffectNeutralizationPending ||
				                                  g_dynamicEffectPassthrough.active ||
				                                  g_dynamicGeometryFlagArmed ||
				                                  g_dynamicTransactionEpoch != 0;
				const bool worldMarkerPresent = g_worldContract.ownsMutation ||
				                                g_worldContract.active;
				const bool ownsTransaction =
				                             qualityMarkerPresent &&
				                             worldMarkerPresent &&
				                             a_manager == g_dynamicEffectPassthrough.manager &&
				                             g_dynamicTransactionEpoch != 0 &&
				                             g_dynamicTransactionEpoch == g_preWorldEpoch &&
				                             g_worldContractEpoch == g_preWorldEpoch &&
				                             g_worldContractManager == a_manager;
				bool normalReturn = false;
				__try {
					func(a_manager, a_start, a_end, a_sourceLogical, a_destinationLogical);
					if (qualityMarkerPresent) {
						FinishQualityHandbackAfterLateRange(a_manager, ownsTransaction);
					} else if (worldMarkerPresent &&
					           ScopePolicy::ClassifyCurrentDispatch() == ScopePolicy::DispatchScope::kMain &&
					           g_worldContractEpoch == g_preWorldEpoch &&
					           g_worldContractManager == a_manager) {
						// Private warm-up and fail-closed vendor skips still let native slot 21
						// finish the reduced scene. Restore native ownership at this late seam.
						if (!CloseReducedWorldFrame(a_manager, true)) {
							DisableDynamicGeometryTransaction("the late ImageSpace range could not restore the native world contract"sv);
						}
					} else if (ScopePolicy::ClassifyCurrentDispatch() == ScopePolicy::DispatchScope::kMain) {
						// Neither bracket owns this frame, so it is the native one-to-one
						// path and this seam is where its world sampling ends. Restore is a
						// no-op when nothing was applied, and running it here keeps the bias
						// out of the UI that follows and out of any scope render that opens
						// after the main world is done with it.
						MipBias::Restore();
					}
					normalReturn = true;
				} __finally {
					// First, and unconditional. On a native one-to-one session neither
					// marker is present, so every recovery below is skipped and the normal
					// path's Restore inside the __try was never reached - an abnormal
					// return would leave the biased duplicates installed until the next
					// pre-world watchdog, across the UI and any scope render in between.
					// Restore is noexcept and idempotent, so paying for it on the normal
					// path too is cheaper than reasoning about which path we are on.
					MipBias::Restore();
					if (!normalReturn && (qualityMarkerPresent || worldMarkerPresent)) {
						// If an outer engine handler survives the late-range SEH, do not leave
						// unity ratios, dynamic geometry or TLS ownership stranded until the
						// next pre-world watchdog. Best-effort terminal closure is itself
						// SEH-contained and never re-enters the throwing ImageSpace range.
						bool innerRestored = !qualityMarkerPresent;
						bool worldRestored = !worldMarkerPresent;
						__try {
							if (qualityMarkerPresent) {
								innerRestored = EndDynamicNeutralization(false);
							}
							if (worldMarkerPresent) {
								worldRestored = CloseReducedWorldFrame(a_manager, true);
							}
						} __except (EXCEPTION_EXECUTE_HANDLER) {
							innerRestored = false;
							worldRestored = false;
						}
						if (!innerRestored || !worldRestored) {
							DisableDynamicGeometryTransaction("abnormal late ImageSpace return could not close the quality ratio/geometry bracket"sv);
						}
						static_cast<void>(HDRRoundTrip::AbortQualityHandbackCandidate());
					}
				}
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallLateImageSpaceRangeProbe(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<LateImageSpaceRangeHook>(a_site);
		}

		void RecordImageSpaceDepthOverrideFailure(
			const D3D11StateIsolation::DepthStencilOverrideToken& a_token,
			std::string_view a_reason) noexcept
		{
			g_qualityImageSpaceDepthOverrideFailed = true;
			// Through the helper, not a raw store, so the latch and the reason that
			// explains it are always set together. a_reason is a literal at both call
			// sites, which is what the helper's first-failure-wins record requires. The
			// detailed line below is a second condition, not a repeat: it carries the
			// exception code and the mismatch/uncertain flags the helper's line has no
			// room for, and both are latched once per session.
			DisableDynamicGeometryTransaction(a_reason);
			g_imageSpaceDepthOverrideFailures.fetch_add(1, std::memory_order_relaxed);
			if (a_token.restoreMismatch) {
				g_imageSpaceDepthOverrideRestoreMismatches.fetch_add(1, std::memory_order_relaxed);
			}
			if (!g_imageSpaceDepthOverrideFailureLogged.exchange(true, std::memory_order_relaxed)) {
				logger::error(
					FMT_STRING("quality ImageSpace depth-stencil override disabled: {} (exception=0x{:08X}, restoreMismatch={}, stateUncertain={})"),
					a_reason,
					a_token.exceptionCode,
					a_token.restoreMismatch,
					a_token.stateUncertain);
			}
		}

		// The experiment behind rebindDepthStencilAfterSeam. Called only on a frame that
		// published at the TemporalAA seam, only after that seam draw's isolation bracket
		// has closed, and only with the key on - so a native frame, a DLAA frame and a
		// native-fallback frame never reach it. It binds DS logical 1's READ-ONLY-DEPTH
		// view, which is the view a native frame carries into the UI render entry, beside
		// whatever colour targets are bound at that instant; the engine's own state object
		// is untouched, and this is deliberately not a bracket, because a binding the
		// bracket's restore would undo would measure nothing.
		void RebindMainDepthStencilAfterSeam() noexcept
		{
			g_seamDepthRebindAttempts.fetch_add(1, std::memory_order_relaxed);
			const auto depth = RenderTargets::LookupDepth(Addr::kDS_MainScene);
			auto* const context = Engine::ImmediateContext();
			if (!depth.valid || !depth.dsvReadOnlyDepth || !context) {
				g_seamDepthRebindDeclines.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			D3D11StateIsolation::DepthStencilRebindResult result{};
			const bool ok = D3D11StateIsolation::RebindDepthStencil(
				context, depth.dsvReadOnlyDepth, result);
			if (result.alreadyBound) {
				g_seamDepthRebindAlreadyBound.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (ok && result.bound) {
				g_seamDepthRebinds.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (result.declined) {
				g_seamDepthRebindDeclines.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_seamDepthRebindFailures.fetch_add(1, std::memory_order_relaxed);
			}
			// One line per condition, once per session: this runs on every published frame
			// and a per-frame line would be the measurement's own cost.
			if (!g_seamDepthRebindFailureLogged.exchange(true, std::memory_order_relaxed)) {
				logger::warn(
					FMT_STRING("seam-exit depth-stencil rebind did not bind: declined={}, captured={}, renderTargets={}, exception=0x{:08X}"),
					result.declined,
					result.captured,
					result.renderTargetCount,
					result.exceptionCode);
			}
		}

		// The experiment behind clearStencilAfterSeam, and the one the step 9c measurement
		// selected. Gated exactly as the rebind above is: only on a frame that published
		// at the TemporalAA seam, only after that seam draw's isolation bracket has
		// closed, and only with the key on, so a native frame, a DLAA frame and a
		// native-fallback frame never reach it.
		//
		// What it does and why. Measured headless at the SanctuaryExt HUD pose, ten frames
		// per arm: at the UI render entry the reduced path leaves DS logical 1's stencil
		// plane holding 0xFF over exactly the reduced raster's rectangle - 2470x1371 at the
		// origin of the 3704x2056 double-wide - and 0 outside it, while the native arm's
		// stencil is 0 on every one of its 7,615,424 texels. The state the UI pass is
		// entered with on native tests that plane with StencilFunc NOT_EQUAL, reference
		// 0x80 and read mask 0x80, so a texel whose stencil has bit 7 set is a texel the UI
		// does not draw. A rectangle of 0xFF at the reduced extent is therefore a HUD
		// suppressed over the reduced raster's own footprint, which is what the headset saw
		// as a compass missing in the left eye and cut by a vertical edge in the right.
		//
		// One ClearDepthStencilView with D3D11_CLEAR_STENCIL and no depth flag: depth is
		// not touched, and the plane goes to the 0 the native arm measures. It is not a
		// bracket, for the same reason the rebind is not: a clear a restore would undo
		// would change nothing. It clears the WHOLE plane, because D3D11 offers no masked
		// stencil clear - which is safe on this path precisely because the reduced path
		// forces the hidden-area mesh off, so nothing else has written that plane this
		// frame and there is no mask of the engine's own to preserve. On any path that
		// keeps the mesh on, this key would erase it, which is why it is gated to a
		// seam-published reduced frame and defaults off.
		void ClearMainStencilAfterSeam() noexcept
		{
			g_seamStencilClearAttempts.fetch_add(1, std::memory_order_relaxed);
			const auto depth = RenderTargets::LookupDepth(Addr::kDS_MainScene);
			auto* const context = Engine::ImmediateContext();
			if (!depth.valid || !depth.dsv || !context) {
				g_seamStencilClearDeclines.fetch_add(1, std::memory_order_relaxed);
				// One line per condition, once per session: this runs on every published
				// frame and a per-frame line would be the experiment's own cost.
				if (!g_seamStencilClearDeclineLogged.exchange(true, std::memory_order_relaxed)) {
					logger::warn(
						FMT_STRING("seam-exit stencil clear declined: depth {} valid, dsv {}, context {}"),
						depth.valid ? "is"sv : "is not"sv,
						depth.dsv ? "present"sv : "absent"sv,
						context ? "present"sv : "absent"sv);
				}
				return;
			}
			// ClearDepthStencilView returns void and cannot fail at the API surface, so
			// the count below is a count of calls made, not of clears proven. What proves
			// it is the UI-entry stencil readback, which is the instrument this key exists
			// to move.
			context->ClearDepthStencilView(depth.dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
			g_seamStencilClears.fetch_add(1, std::memory_order_relaxed);
		}

		struct ImageSpacePixelDrawProbeHook
		{
			static bool DrawWithDepthStencilNeutralized(
				std::uintptr_t a_renderPass,
				std::uint8_t a_arg2,
				std::uintptr_t a_rendererContext)
			{
				D3D11StateIsolation::DepthStencilOverrideToken token{};
				g_imageSpaceDepthOverrideAttempts.fetch_add(1, std::memory_order_relaxed);
				auto* const context = Engine::ImmediateContext();
				const bool began = D3D11StateIsolation::BeginDepthStencilOverride(context, token);
				if (token.alreadyNull) {
					g_imageSpaceDepthOverrideAlreadyNull.fetch_add(1, std::memory_order_relaxed);
				} else if (token.hadDepthStencil) {
					g_imageSpaceDepthOverrideNonNull.fetch_add(1, std::memory_order_relaxed);
				}
				if (!began) {
					const bool recovered = D3D11StateIsolation::EndDepthStencilOverride(context, token);
					if (token.restored) {
						g_imageSpaceDepthOverrideRestores.fetch_add(1, std::memory_order_relaxed);
					}
					RecordImageSpaceDepthOverrideFailure(
						token,
						recovered ? "the selective OM bind did not complete"sv :
						            "the selective OM bind did not restore exactly"sv);
					return false;
				}
				if (token.alreadyNull) {
					return func(a_renderPass, a_arg2, a_rendererContext);
				}

				g_imageSpaceDepthOverrideDraws.fetch_add(1, std::memory_order_relaxed);
				bool result = false;
				bool restored = false;
				__try {
					result = func(a_renderPass, a_arg2, a_rendererContext);
				} __finally {
					restored = D3D11StateIsolation::EndDepthStencilOverride(context, token);
					if (restored) {
						g_imageSpaceDepthOverrideRestores.fetch_add(1, std::memory_order_relaxed);
					} else {
						RecordImageSpaceDepthOverrideFailure(
							token,
							"the exact RTV/DSV handback could not be proven"sv);
					}
				}
				return restored ? result : false;
			}

			static bool thunk(
				std::uintptr_t a_renderPass,
				std::uint8_t a_arg2,
				std::uintptr_t a_rendererContext)
			{
				// Paired with the draw entry this same hook records after the draw returns.
				// Two records around one pass are what separate "the engine bound nothing
				// for this pass" from "this pass unbound what was there before it".
				if (Debug::FrameCensusRecording()) {
					Debug::OnSeamStateProbe(Debug::CensusProbeSite::pixelDrawEntry);
				}
				const auto call = g_activeTemporalAACall;
				const auto handback = HDRRoundTrip::HandbackCandidateStamp();
				// Main-view ownership was positively latched before RT1 changed. From
				// here until terminal commit/abort, a fresh renderer+4 read may veto the
				// seam publish but must never make the prepared draw fail open into real
				// RT4.
				const bool qualityMarkerActive = g_dynamicEffectNeutralizationPending ||
				                                 g_dynamicEffectPassthrough.active ||
				                                 g_dynamicGeometryFlagArmed ||
				                                 g_dynamicTransactionEpoch != 0;
				// A transaction that wrote RT1 and then lost its seam draw runs the
				// engine's own draw here: HDR and gamma consumed the full-resolution colour
				// at unity, slot 21 reports inactive, so TemporalAA is the range's last
				// active effect and this draw writes the parity-selected history and RT4.
				// It is decided first because a fallback frame still carries every
				// ownership marker of a publishing one.
				const bool nativeFallback = call != 0 && qualityMarkerActive &&
				                            HDRRoundTrip::QualityNativeFallbackArmed() &&
				                            ScopePolicy::ClassifyCurrentDispatch() == ScopePolicy::DispatchScope::kMain;
				const bool ownsQualityTransaction = !nativeFallback &&
				                                     qualityMarkerActive &&
				                                     g_dynamicEffectNeutralizationPending &&
				                                     g_dynamicEffectPassthrough.active &&
				                                     g_dynamicGeometryFlagArmed &&
				                                     call != 0 &&
				                                     g_dynamicExpectedTaaCall != 0 &&
				                                     call == g_dynamicExpectedTaaCall &&
				                                     g_dynamicTransactionEpoch != 0 &&
				                                     g_dynamicTransactionEpoch == g_preWorldEpoch &&
				                                     HDRRoundTrip::QualityRuntimeConfigReady();
				const bool unexpectedQualityTaa = call != 0 && qualityMarkerActive &&
				                                  !ownsQualityTransaction && !nativeFallback;
				// Ownership was latched before RT1 changed. A live settings reload may
				// invalidate the candidate (the seam publish then declines and the frame
				// falls back), but it must never redirect this in-flight frame into the
				// diagnostic bypass path.
				const bool publishAtSeam = ownsQualityTransaction;
				const auto qualityConfigEpoch = ownsQualityTransaction ?
				                                    HDRRoundTrip::QualityTaaConfigEpoch() :
				                                    0;
				TAAReplacement::DrawResult bypass{};
				if (call != 0 && !unexpectedQualityTaa && !nativeFallback) {
					bypass = TAAReplacement::TryBypass(
						call,
						handback,
						publishAtSeam,
						qualityConfigEpoch,
						publishAtSeam ? HDRRoundTrip::QualityWorldAdmission() :
						                ResetPolicy::WorldAdmissionTicket{});
				}
				bool runNative = nativeFallback;
				bool closedByAbort = false;
				bool publishedAtSeam = false;
				bool result = true;
				if (unexpectedQualityTaa) {
					// A second main-view TemporalAA draw inside one owned frame, or one whose
					// configuration moved underneath it. With RT1 written it finishes on the
					// native draw; a frame that cannot fall back closes fail-closed with no
					// draw issued, so RT4 keeps the previous frame's contents for this one
					// frame.
					if (HDRRoundTrip::ArmQualityNativeFallback(HDRRoundTrip::QualityFallbackSeam::kUnexpectedSecondTaaDraw)) {
						runNative = true;
					} else {
						closedByAbort = true;
						const bool geometryRestored = EndDynamicNeutralization(true);
						const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
						if (!geometryRestored || !pixelsRestored) {
							DisableDynamicGeometryTransaction("an unexpected second main TemporalAA draw could not close the quality transaction"sv);
						}
					}
				} else if (ownsQualityTransaction && bypass.issuedUnrecoverable) {
					// The seam draw was issued into public history and RT4 and cannot be
					// proved: a fault after it, an unproved host state, device removal.
					// Running the engine's draw against that state is never fail-soft, so
					// the frame stays as drawn, the session ends, and the late range closes
					// the brackets.
					closedByAbort = true;
					HDRRoundTrip::LatchQualityPublishUnrecoverable();
				} else if (ownsQualityTransaction && bypass.succeeded) {
					publishedAtSeam = HDRRoundTrip::MarkQualityPublishedAtSeam();
					if (!publishedAtSeam) {
						closedByAbort = true;
						HDRRoundTrip::LatchQualityPublishUnrecoverable();
					}
				} else if (ownsQualityTransaction) {
					// The seam publish was refused before issue: the world-admission recheck
					// vetoed it, a DevBench fault stood in for a lost seam path, or the
					// pending episode, parity or bindings did not validate. RT1 still holds
					// the reconstruction and HDR consumed it at unity, so the native draw is
					// the valid frame; an unproved host context is the one thing that
					// refuses it, and that frame closes fail-closed with no draw issued: the
					// walk ends at TemporalAA, so RT4 keeps the previous frame's contents for
					// this one frame, and the session ends.
					const auto seam = bypass.worldAdmissionVeto ?
					                      HDRRoundTrip::QualityFallbackSeam::kSeamAdmissionVeto :
					                  bypass.injectedSeam == HDRRoundTrip::QualityFaultSeam::kPostTaaStage ?
					                      HDRRoundTrip::QualityFallbackSeam::kInjectedPostTaaStage :
					                  bypass.injectedSeam == HDRRoundTrip::QualityFaultSeam::kPrePublish ?
					                      HDRRoundTrip::QualityFallbackSeam::kInjectedPrePublish :
					                      HDRRoundTrip::QualityFallbackSeam::kSeamDrawNotAttempted;
					if (HDRRoundTrip::ArmQualityNativeFallback(seam)) {
						runNative = true;
					} else {
						closedByAbort = true;
						const bool geometryRestored = EndDynamicNeutralization(true);
						const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
						if (!geometryRestored || !pixelsRestored) {
							DisableDynamicGeometryTransaction("the refused TemporalAA-seam publish could not close the quality transaction"sv);
						}
					}
				}
				bool nativeDrew = false;
				if (runNative) {
					TAAReplacement::RecordNativeFallbackDraw(call);
					g_temporalAAFallbackNativeDraws.fetch_add(1, std::memory_order_relaxed);
				}
				if (closedByAbort || publishedAtSeam) {
					// Our draw, or the terminal decision above, stands in for the engine's
					// draw this frame; it runs exactly once per frame either way.
				} else if (bypass.drew && !runNative) {
					result = true;
				} else {
					if (g_qualityImageSpaceDepthOverrideActive) {
						// The neutralised draw can decline before reaching the engine; a fallback
						// frame must not be closed as drawn unless it did.
						result = DrawWithDepthStencilNeutralized(a_renderPass, a_arg2, a_rendererContext);
						nativeDrew = result;
					} else {
						nativeDrew = true;
						result = func(a_renderPass, a_arg2, a_rendererContext);
					}
				}
				// The seam's exit: our draw has stood in for the engine's TemporalAA pass
				// and the seam draw's isolation bracket has closed, so this is the first
				// instant at which a binding of ours survives into the rest of the frame.
				if (publishedAtSeam && g_rebindDepthStencilAfterSeam.load(std::memory_order_acquire)) {
					RebindMainDepthStencilAfterSeam();
				}
				// Independent of the rebind above and read with its own load: the two
				// answer different halves - one restores the BINDING native carries into
				// the UI pass, this one restores the stencil CONTENT native carries - and
				// the headset has already shown that the binding alone does not draw the
				// compass.
				// The composite's patched rows close here at the latest. This is the
				// backstop: the two tight points are the next geometry setup and the water
				// shader's own setup, and a count that lands mostly here would say those
				// are not firing.
				CompositeScale::RestoreRows(CompositeScale::RestoreSite::kSeamExit);
				if (publishedAtSeam && g_clearStencilAfterSeam.load(std::memory_order_acquire)) {
					ClearMainStencilAfterSeam();
				}
				// A live mode change never reaches the release below, because a DLAA,
				// native or native-fallback frame does not publish at the seam. The
				// settings worker asks for the release from there and the render thread
				// performs it here, which is the only thread that ever binds what the pass
				// owns. One relaxed exchange on a frame nobody asked on.
				DepthResolve::ReleaseIfRequested();
				// Third of the three passes on this seam and the superset of the one above.
				// Unlike the two above it is the reduced path's own behaviour and not a
				// measurement: the resolve rewrites DS1's DEPTH plane at the full extent
				// from the reduced raster's packed copy, and zeroes the stencil plane in
				// the same draw because the state it draws under replaces stencil with 0.
				// It runs AFTER the clear so that, with that key also on, the clear is
				// priced and the resolve's own stencil write lands last; on its own it does
				// the clear's job too. Same gate as both keys above - a seam-published
				// reduced frame with the bracket closed - so a native, DLAA or
				// native-fallback frame never reaches it.
				if (publishedAtSeam) {
					if (g_resolveDepthAfterSeam.load(std::memory_order_acquire)) {
						DepthResolve::Run();
					} else {
						// The key off is also a release point: this is the render thread,
						// the pass's objects are only ever bound from here, and a settings
						// thread releasing a texture the render thread might still be using
						// is the hazard this avoids. Release is a no-op once it has run.
						DepthResolve::Release();
					}
				}
				// Recorded on both paths and whether or not the key is on, so the ON and OFF
				// censuses and the native census carry the same row to compare.
				if (call != 0 && Debug::FrameCensusRecording()) {
					Debug::OnSeamStateProbe(Debug::CensusProbeSite::seamExit);
				}
				TAAReplacement::DrawResult overwrite{};
				if (call != 0 && !unexpectedQualityTaa && !ownsQualityTransaction && !runNative && !bypass.drew) {
					overwrite = TAAReplacement::TryOverwrite(call, handback);
				}
				if (call != 0) {
					if (nativeDrew) {
						g_temporalAANativeDraws.fetch_add(1, std::memory_order_relaxed);
					}
					const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
					Debug::OnTemporalAADraw(
						Engine::Renderer(), call, thread);
					StereoCapture::OnTemporalAADraw(
						call, thread, bypass.succeeded && !runNative, overwrite.succeeded, handback);
					if (ownsQualityTransaction || runNative) {
						g_dynamicNeutralizationSawTaa = true;
						g_dynamicNeutralizationTaaSucceeded = runNative ? nativeDrew : publishedAtSeam;
					}
				}
				const auto hdrCall = g_activeHDRCall;
				if (hdrCall != 0) {
					Debug::OnHDRDraw(
						Engine::Renderer(),
						hdrCall,
						g_activeHDRDestination,
						static_cast<std::uint32_t>(::GetCurrentThreadId()));
				}
				// Same instant as the TemporalAA observation above: the draw has returned
				// and BSImagespaceShader has not run its virtual cleanup, so the OM
				// targets and pixel-stage resources this pass used are still bound.
				if (Debug::FrameCensusRecording()) {
					Debug::OnImageSpacePixelDraw();
				}
				return result;
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallImageSpacePixelDrawProbe(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<ImageSpacePixelDrawProbeHook>(a_site);
		}

		// The four screen-space-reflection pixel passes. Each SSLR shader overrides the
		// shared ImageSpace draw the probe above wraps, so none of them reaches that seam
		// and the census could not see the reflection chain at all - which is why a census
		// reporting no reflection pass proved nothing about the frame.
		//
		// Each site is that pass's own five-byte direct call to the SAME prepared-draw
		// executor, so the shape, the signature and the recording instant are identical to
		// the ImageSpace probe: call the original, then record while the OM targets and
		// pixel-stage resources the pass used are still bound. Nothing here is conditional
		// on our own state and nothing is written back, so an SSLR frame is byte-identical
		// with the hooks installed. The per-pass counter is what makes "the chain does not
		// run at this pose" a reading rather than an absence of evidence.
		std::atomic<std::uint64_t> g_sslrPrepassDraws{ 0 };
		std::atomic<std::uint64_t> g_sslrRaytracingDraws{ 0 };
		std::atomic<std::uint64_t> g_sslrBlurHDraws{ 0 };
		std::atomic<std::uint64_t> g_sslrBlurVDraws{ 0 };
		bool g_sslrPrepassProbeInstalled = false;
		bool g_sslrRaytracingProbeInstalled = false;
		bool g_sslrBlurHProbeInstalled = false;
		bool g_sslrBlurVProbeInstalled = false;

		[[nodiscard]] std::atomic<std::uint64_t>& SslrCounter(Debug::SslrPass a_pass) noexcept
		{
			switch (a_pass) {
			case Debug::SslrPass::raytracing:
				return g_sslrRaytracingDraws;
			case Debug::SslrPass::blurH:
				return g_sslrBlurHDraws;
			case Debug::SslrPass::blurV:
				return g_sslrBlurVDraws;
			default:
				return g_sslrPrepassDraws;
			}
		}

		// One template instantiation per pass, because pstl::write_thunk_call keys the
		// saved original off the hook type: four sites sharing one type would each
		// overwrite the previous site's original and the last install would win.
		template <Debug::SslrPass Pass>
		struct SslrPixelDrawProbeHook
		{
			static bool thunk(
				std::uintptr_t a_renderPass,
				std::uint8_t a_arg2,
				std::uintptr_t a_rendererContext)
			{
				// Read once here rather than twice around the draw: the census gate is a
				// relaxed load and the frame boundary that could flip it runs on this same
				// thread, but pairing an entry probe with a draw entry only means anything
				// when both belong to the same recording frame.
				const bool recording = Debug::FrameCensusRecording();
				if (recording) {
					Debug::OnSeamStateProbe(Debug::CensusProbeSite::sslrDrawEntry, Pass);
				}
				const bool result = func(a_renderPass, a_arg2, a_rendererContext);
				SslrCounter(Pass).fetch_add(1, std::memory_order_relaxed);
				if (recording) {
					Debug::OnSslrPixelDraw(Pass);
				}
				return result;
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallSslrPrepassDrawProbe(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<SslrPixelDrawProbeHook<Debug::SslrPass::prepass>>(a_site);
		}

		void InstallSslrRaytracingDrawProbe(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<SslrPixelDrawProbeHook<Debug::SslrPass::raytracing>>(a_site);
		}

		void InstallSslrBlurHDrawProbe(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<SslrPixelDrawProbeHook<Debug::SslrPass::blurH>>(a_site);
		}

		void InstallSslrBlurVDrawProbe(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<SslrPixelDrawProbeHook<Debug::SslrPass::blurV>>(a_site);
		}

		// BSWaterShader's two constant-setup entries. Both are entry sites rather than call
		// sites, because the water SURFACE draw is world geometry reached through queued
		// render-pass lambdas and has no direct call site to wrap - which is exactly why
		// the 2026-09-06 census could not see it. Addresses.h carries the derivation of
		// both entries from the binary's own RTTI and of the constants they write.
		//
		// Each thunk runs the original FIRST and records afterwards, which is not the
		// SSLR probes' ordering by accident: the whole point is the constants the engine
		// has just written, and before the call they still hold the previous draw's. The
		// counter advances whether or not a census is armed, so a zero is a reading.
		std::atomic<std::uint64_t> g_waterSetupGeometryCalls{ 0 };
		std::atomic<std::uint64_t> g_waterSetupTechniqueCalls{ 0 };
		std::atomic<std::uint32_t> g_waterSetupThreadId{ 0 };
		std::atomic<std::uint64_t> g_waterSetupThreadMismatches{ 0 };
		std::atomic<std::uint64_t> g_waterSetupOffRenderThread{ 0 };
		bool g_waterSetupGeometryProbeInstalled = false;
		bool g_waterSetupTechniqueProbeInstalled = false;

		// Which thread the water setup runs on, and whether it is the one ClearColor runs
		// on. The answer decides what a sample may read: the constants are plain memory and
		// are safe anywhere, the D3D11 immediate context is not. The first census taken with
		// these sites installed recorded no water entry at all while the counter above stood
		// at 100,640 for that arm and 723,955 for the session
		// (artifacts/phase5-step9b-census-20260907-1118, state-s9b-quality.json and
		// state-after-world.json in the research repo), which is what this pair exists to
		// explain.
		[[nodiscard]] bool NoteWaterSetupThread() noexcept
		{
			const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
			// A relaxed LOAD before the exchange. The exchange has work to do exactly once,
			// but this site runs hundreds of thousands of times a session on a pool of about
			// twenty worker threads, and a compare_exchange that FAILS is still a locked
			// read-modify-write on a line every one of those threads is touching. After the
			// first call the load answers and nothing is locked. The two counters below are
			// still RMWs, so the probe's residual cost is bounded by them; it is not
			// measured, and docs/MODULE_MAP.md says so rather than implying it is free.
			auto first = g_waterSetupThreadId.load(std::memory_order_relaxed);
			if (first == 0) {
				auto expected = std::uint32_t{ 0 };
				first = g_waterSetupThreadId.compare_exchange_strong(
							expected, thread, std::memory_order_relaxed, std::memory_order_relaxed) ?
				            thread :
				            expected;
			}
			if (first != thread) {
				g_waterSetupThreadMismatches.fetch_add(1, std::memory_order_relaxed);
			}
			const bool onRenderThread =
				g_renderThreadId.load(std::memory_order_relaxed) == thread;
			if (!onRenderThread) {
				g_waterSetupOffRenderThread.fetch_add(1, std::memory_order_relaxed);
			}
			return onRenderThread;
		}

		// Slot 3: (this, pass, shaderState, viewState), void. The third and fourth
		// arguments are what carry the pixel-shader record, the constant groups and the
		// render-target id the engine builds the screen constant from, so they are
		// forwarded to the census verbatim rather than re-resolved from a global.
		struct WaterSetupGeometryProbeHook
		{
			static void thunk(
				std::uintptr_t a_shader,
				std::uintptr_t a_pass,
				std::uintptr_t a_shaderState,
				std::uintptr_t a_viewState)
			{
				// Before the original, not after: the water shader's depth reconstruction
				// reads rows 20-27 with its own ratio-scaled screen constant, so it is the
				// pass a stale composite patch would corrupt, and the rows have to be back
				// before its constants are written.
				CompositeScale::RestoreRows(CompositeScale::RestoreSite::kWater);
				func(a_shader, a_pass, a_shaderState, a_viewState);
				g_waterSetupGeometryCalls.fetch_add(1, std::memory_order_relaxed);
				const bool onRenderThread = NoteWaterSetupThread();
				Debug::OnWaterShaderSetup(
					Debug::WaterSite::setupGeometry, a_shaderState, a_viewState, onRenderThread);
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		// Slot 4: (this, uint32 technique, pass), bool. It writes the same screen constant
		// through the same shader-record byte but is handed neither the shader state nor
		// the view state, so its census entry carries the bindings and the engine sources
		// and no constant values - which is what distinguishes the two sites in a reading.
		struct WaterSetupTechniqueProbeHook
		{
			static bool thunk(
				std::uintptr_t a_shader,
				std::uint32_t a_technique,
				std::uintptr_t a_pass)
			{
				CompositeScale::RestoreRows(CompositeScale::RestoreSite::kWater);
				const bool result = func(a_shader, a_technique, a_pass);
				g_waterSetupTechniqueCalls.fetch_add(1, std::memory_order_relaxed);
				const bool onRenderThread = NoteWaterSetupThread();
				Debug::OnWaterShaderSetup(Debug::WaterSite::setupTechnique, 0, 0, onRenderThread);
				return result;
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallWaterSetupGeometryProbe(std::uintptr_t a_site)
		{
			InstallEntryProbe<Addr::kWaterShaderSetupGeometryPrefixBytes, WaterSetupGeometryProbeHook>(
				a_site);
		}

		void InstallWaterSetupTechniqueProbe(std::uintptr_t a_site)
		{
			InstallEntryProbe<Addr::kWaterShaderSetupTechniquePrefixBytes, WaterSetupTechniqueProbeHook>(
				a_site);
		}

		// The composite reconstruction scale, and the three sites it needs.
		//
		// The composite pass is RECORDED and REPLAYED (MEASURED 2026-09-09: its technique
		// setup runs about 360 times in a 1,240-frame launch and its render pass exactly
		// three, both at ratios 1.0 x 1.0 and both within milliseconds of world entry).
		// So the correction cannot be a binding and cannot be a whole-frame edit either:
		// rows 20-27 are shared, and the deferred light and water passes read them with
		// their own ratio-scaled clip positions and would be double-scaled. It has to be
		// bracketed around the ONE execute that replays the composite's own list.
		//
		//  - slot 2 of the composite's vtable marks the recording on the stack. Nothing is
		//    bound and nothing is patched there.
		//  - the immediate context's FinishCommandList adopts the list produced while that
		//    mark is set, which is what identifies the composite's list rather than
		//    guessing at it.
		//  - the immediate context's ExecuteCommandList patches, executes and restores.
		//
		bool g_compositeScaleInstalled = false;
		// Defined below, beside the composite vtable patch it was written for.
		[[nodiscard]] bool PatchVtableSlot(
			std::uintptr_t a_vtable,
			std::size_t a_slot,
			std::uintptr_t a_thunk,
			std::uintptr_t& a_original) noexcept;

		// The immediate context's four draw entry points, patched in the runtime's own
		// vtable at the indices d3d11.h declares them at: DrawIndexed 12, Draw 13,
		// DrawIndexedInstanced 20, DrawInstanced 21. This is where the composite's
		// correction lives, because it is the only place the rows are still in the
		// allocation the draw will read - the engine's uploader maps the camera buffer with
		// WRITE_DISCARD, so anything written before that upload is in a renamed allocation
		// and gone.
		//
		// A composite draw is recognised by its bound pixel shader: the whole composite
		// family binds a TextureCubeArray at t8, which nothing else in the frame does. The
		// probe is two COM calls, so its verdict is cached per shader object and a draw
		// costs one PSGetShader plus a lookup - and only while the key is on, so an off key
		// costs a single relaxed load per draw.
		constexpr std::size_t kDrawIndexedSlot = 12;
		constexpr std::size_t kDrawSlot = 13;
		constexpr std::size_t kDrawIndexedInstancedSlot = 20;
		constexpr std::size_t kDrawInstancedSlot = 21;
		using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
		using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
		using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
		using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
		DrawFn g_drawOriginal = nullptr;
		DrawIndexedFn g_drawIndexedOriginal = nullptr;
		DrawInstancedFn g_drawInstancedOriginal = nullptr;
		DrawIndexedInstancedFn g_drawIndexedInstancedOriginal = nullptr;
		bool g_drawHooksInstalled = false;

		// EVERY draw is counted here, before any gate, and so is the thread it came in on.
		// The previous build put its counters behind the key and behind a
		// "must be the ClearColor thread" test, and the headset read zero examined draws -
		// which could not distinguish "no draw reaches the thunk" from "the draw is on
		// another thread". Two earlier measurements had already said which: the composite's
		// own geometry setup ran with onRenderThread 0, and the step 9b water instrument
		// saw 48,814 of 48,814 setup calls off that thread. The engine's world D3D work is
		// not on the thread the mod calls the render thread.
		constexpr std::size_t kDrawThreadSlots = 8;
		std::atomic<std::uint32_t> g_drawThreadId[kDrawThreadSlots]{};
		std::atomic<std::uint64_t> g_drawThreadCount[kDrawThreadSlots]{};
		std::atomic<std::uint64_t> g_drawThreadOverflow{ 0 };
		std::atomic<std::uint64_t> g_drawsTotal{ 0 };

		void NoteDrawThread() noexcept
		{
			g_drawsTotal.fetch_add(1, std::memory_order_relaxed);
			const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
			for (std::size_t i = 0; i < kDrawThreadSlots; ++i) {
				auto seen = g_drawThreadId[i].load(std::memory_order_relaxed);
				if (seen == 0) {
					auto expected = std::uint32_t{ 0 };
					if (!g_drawThreadId[i].compare_exchange_strong(
							expected, thread, std::memory_order_relaxed, std::memory_order_relaxed)) {
						seen = expected;
					} else {
						seen = thread;
					}
				}
				if (seen == thread) {
					g_drawThreadCount[i].fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			g_drawThreadOverflow.fetch_add(1, std::memory_order_relaxed);
		}

		// The distinct composite pixel shaders seen, recorded only on a draw already known
		// to be the composite's, so the table is a handful of entries and never a hot path.
		constexpr std::size_t kCompositeShaderSlots = 16;
		std::atomic<std::uintptr_t> g_compositeShaderSeen[kCompositeShaderSlots]{};

		void NoteCompositeShader(ID3D11DeviceContext* a_context) noexcept
		{
			ID3D11PixelShader* shader = nullptr;
			a_context->PSGetShader(&shader, nullptr, nullptr);
			if (!shader) {
				return;
			}
			const auto value = reinterpret_cast<std::uintptr_t>(shader);
			shader->Release();
			std::uint32_t distinct = 0;
			for (std::size_t i = 0; i < kCompositeShaderSlots; ++i) {
				auto seen = g_compositeShaderSeen[i].load(std::memory_order_relaxed);
				if (seen == 0) {
					auto expected = std::uintptr_t{ 0 };
					if (!g_compositeShaderSeen[i].compare_exchange_strong(
							expected, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
						seen = expected;
					} else {
						seen = value;
					}
				}
				if (seen != 0) {
					++distinct;
				}
				if (seen == value) {
					break;
				}
			}
			CompositeScale::NoteCompositeShaderCount(distinct);
		}

		// No cache and no PSGetShader on the common path. For all but a handful of draws
		// nothing is bound at t8 at all, so this is one COM call and a null test; the
		// GetDesc only happens for the draws that already have a cube array there. A cache
		// would have to be safe against several threads now that the thread gate is gone,
		// and it is not worth that for one extra call on a rare draw.
		[[nodiscard]] bool IsCompositeDraw(ID3D11DeviceContext* a_context) noexcept
		{
			ID3D11ShaderResourceView* view = nullptr;
			a_context->PSGetShaderResources(Addr::kCompositeCubeArraySlot, 1, &view);
			if (!view) {
				return false;
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			view->GetDesc(&desc);
			view->Release();
			return desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
		}

		std::atomic_bool g_compositeDrawLogged{ false };

		// True when the caller must restore after issuing the draw. No thread gate: whatever
		// thread issues a draw on the immediate context is the thread the engine is driving
		// that context from at that moment, and a Map issued on the same thread immediately
		// before the draw is correctly ordered against it. The ratio gate stays.
		[[nodiscard]] bool BeginCompositeDraw(ID3D11DeviceContext* a_context) noexcept
		{
			CompositeScale::NoteDrawExamined();
			if (!g_compositeReconstructionScale.load(std::memory_order_acquire)) {
				return false;
			}
			if (!IsCompositeDraw(a_context)) {
				return false;
			}
			CompositeScale::NoteCompositeDraw();
			NoteCompositeShader(a_context);
			if (!g_compositeDrawLogged.exchange(true, std::memory_order_relaxed)) {
				logger::info(
					FMT_STRING("composite reconstruction scale: the first composite draw was seen on thread {}; the ClearColor thread is {}"),
					static_cast<std::uint32_t>(::GetCurrentThreadId()),
					g_renderThreadId.load(std::memory_order_relaxed));
			}
			return CompositeScale::PatchForDraw(true);
		}

		void STDMETHODCALLTYPE DrawThunk(
			ID3D11DeviceContext* a_context, UINT a_vertexCount, UINT a_startVertex)
		{
			NoteDrawThread();
			const bool patched = BeginCompositeDraw(a_context);
			g_drawOriginal(a_context, a_vertexCount, a_startVertex);
			if (patched) {
				CompositeScale::RestoreRows(CompositeScale::RestoreSite::kDraw);
			}
		}

		void STDMETHODCALLTYPE DrawIndexedThunk(
			ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
		{
			NoteDrawThread();
			const bool patched = BeginCompositeDraw(a_context);
			g_drawIndexedOriginal(a_context, a_indexCount, a_startIndex, a_baseVertex);
			if (patched) {
				CompositeScale::RestoreRows(CompositeScale::RestoreSite::kDraw);
			}
		}

		void STDMETHODCALLTYPE DrawInstancedThunk(
			ID3D11DeviceContext* a_context, UINT a_vertexPerInstance, UINT a_instanceCount,
			UINT a_startVertex, UINT a_startInstance)
		{
			NoteDrawThread();
			const bool patched = BeginCompositeDraw(a_context);
			g_drawInstancedOriginal(
				a_context, a_vertexPerInstance, a_instanceCount, a_startVertex, a_startInstance);
			if (patched) {
				CompositeScale::RestoreRows(CompositeScale::RestoreSite::kDraw);
			}
		}

		void STDMETHODCALLTYPE DrawIndexedInstancedThunk(
			ID3D11DeviceContext* a_context, UINT a_indexPerInstance, UINT a_instanceCount,
			UINT a_startIndex, INT a_baseVertex, UINT a_startInstance)
		{
			NoteDrawThread();
			const bool patched = BeginCompositeDraw(a_context);
			g_drawIndexedInstancedOriginal(
				a_context, a_indexPerInstance, a_instanceCount, a_startIndex, a_baseVertex,
				a_startInstance);
			if (patched) {
				CompositeScale::RestoreRows(CompositeScale::RestoreSite::kDraw);
			}
		}

		// [threadId, draws] per slot, plus the total and whatever did not fit. This is the
		// table that says which thread the engine draws the world on.
		[[nodiscard]] std::string DrawThreadCensus()
		{
			std::string out{ "[" };
			bool first = true;
			for (std::size_t i = 0; i < kDrawThreadSlots; ++i) {
				const auto thread = g_drawThreadId[i].load(std::memory_order_relaxed);
				if (thread == 0) {
					continue;
				}
				if (!first) {
					out += ',';
				}
				first = false;
				out += fmt::format(
					FMT_STRING("[{},{}]"),
					thread,
					g_drawThreadCount[i].load(std::memory_order_relaxed));
			}
			out += ']';
			return out;
		}

		std::atomic_bool g_drawHooksTried{ false };

		// Render thread, once. The context does not exist when Install runs, so the four
		// runtime vtable slots are taken at the first frame that has one.
		void EnsureCompositeDrawHooks() noexcept
		{
			if (g_drawHooksTried.load(std::memory_order_relaxed)) {
				return;
			}
			auto* const context = Engine::ImmediateContext();
			if (!context) {
				return;
			}
			g_drawHooksTried.store(true, std::memory_order_relaxed);
			auto* const vtable = *reinterpret_cast<std::uintptr_t**>(context);
			std::uintptr_t originals[4] = {};
			const std::size_t slots[4] = {
				kDrawIndexedSlot, kDrawSlot, kDrawIndexedInstancedSlot, kDrawInstancedSlot
			};
			const std::uintptr_t thunks[4] = {
				reinterpret_cast<std::uintptr_t>(&DrawIndexedThunk),
				reinterpret_cast<std::uintptr_t>(&DrawThunk),
				reinterpret_cast<std::uintptr_t>(&DrawIndexedInstancedThunk),
				reinterpret_cast<std::uintptr_t>(&DrawInstancedThunk)
			};
			for (std::size_t i = 0; i < 4; ++i) {
				if (!PatchVtableSlot(
						reinterpret_cast<std::uintptr_t>(vtable), slots[i], thunks[i], originals[i])) {
					// Put back whatever was already taken: a half-patched draw table would
					// leave some draws examined and some not, which reads like a composite
					// that only sometimes draws.
					for (std::size_t undo = 0; undo < i; ++undo) {
						std::uintptr_t discard = 0;
						static_cast<void>(PatchVtableSlot(
							reinterpret_cast<std::uintptr_t>(vtable), slots[undo],
							originals[undo], discard));
					}
					logger::warn("the immediate context's draw slots were not patched - the composite reconstruction scale is unavailable"sv);
					return;
				}
			}
			g_drawIndexedOriginal = reinterpret_cast<DrawIndexedFn>(originals[0]);
			g_drawOriginal = reinterpret_cast<DrawFn>(originals[1]);
			g_drawIndexedInstancedOriginal = reinterpret_cast<DrawIndexedInstancedFn>(originals[2]);
			g_drawInstancedOriginal = reinterpret_cast<DrawInstancedFn>(originals[3]);
			g_drawHooksInstalled = true;
			logger::info(
				FMT_STRING("composite reconstruction scale: the context's four draw entries hooked at vtable {:016X} slots {}, {}, {} and {}"),
				reinterpret_cast<std::uintptr_t>(vtable),
				kDrawIndexedSlot, kDrawSlot, kDrawIndexedInstancedSlot, kDrawInstancedSlot);
		}

		// Slot 2's own count, kept because it is the number that says the composite pass is
		// recorded once rather than run per frame.
		std::atomic<std::uint64_t> g_compositeRecordings{ 0 };

		// Every member of BSDFCompositeShader, counted. Two of the eight had been measured
		// and both were recording-time, which is not evidence that the class is bypassed:
		// the batch renderer calls a technique setup only when the technique CHANGES, and
		// the composite has one technique per variant, so a once-per-launch count there is
		// what a cached technique looks like. The per-draw members are the geometry pair,
		// and this census is what finds them rather than guessing again.
		//
		// The vtable at 30bf228 holds the class's own members in slots 0, 2, 3, 4, 5, 9, 10
		// and 11; slots 1, 6, 7, 8 and 12-14 point into base classes and other translation
		// units and are left alone. Six integer registers are forwarded verbatim: these
		// prologues spill all four argument homes and read nothing past the fourth, and two
		// more cost nothing.
		using CompositeMember = std::uintptr_t (*)(
			std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

		constexpr std::size_t kCompositeMemberCount = 8;
		constexpr std::size_t kCompositeMemberSlot[kCompositeMemberCount] = { 0, 2, 3, 4, 5, 9, 10, 11 };
		constexpr std::uintptr_t kCompositeMemberRva[kCompositeMemberCount] = {
			0x291cce0, 0x291aef0, 0x291b4c0, 0x2918fc0, 0x2919ac0, 0x2919e30, 0x291a470, 0x291add0
		};
		CompositeMember g_compositeMemberOriginal[kCompositeMemberCount]{};
		std::atomic<std::uint64_t> g_compositeMemberCalls[kCompositeMemberCount]{};
		std::atomic<std::uint64_t> g_compositeMemberOnRenderThread[kCompositeMemberCount]{};
		std::atomic<std::uint64_t> g_compositeMemberReduced[kCompositeMemberCount]{};
		std::atomic<float> g_compositeMemberRatioWidth[kCompositeMemberCount]{};
		std::atomic<float> g_compositeMemberRatioHeight[kCompositeMemberCount]{};

		template <std::size_t Index>
		std::uintptr_t CompositeMemberThunk(
			std::uintptr_t a_shader,
			std::uintptr_t a_arg2,
			std::uintptr_t a_arg3,
			std::uintptr_t a_arg4,
			std::uintptr_t a_arg5,
			std::uintptr_t a_arg6)
		{
			g_compositeMemberCalls[Index].fetch_add(1, std::memory_order_relaxed);
			if (g_renderThreadId.load(std::memory_order_relaxed) ==
				static_cast<std::uint32_t>(::GetCurrentThreadId())) {
				g_compositeMemberOnRenderThread[Index].fetch_add(1, std::memory_order_relaxed);
			}
			// The two live floats only - Snapshot reads thirty atomics besides, and this is
			// a per-draw site on the candidate members.
			float ratioWidth = 0.0f;
			float ratioHeight = 0.0f;
			const bool readable = DynamicResolution::CurrentRatios(ratioWidth, ratioHeight);
			const bool reduced = readable && (ratioWidth != 1.0f || ratioHeight != 1.0f);
			if (reduced) {
				g_compositeMemberReduced[Index].fetch_add(1, std::memory_order_relaxed);
				g_compositeMemberRatioWidth[Index].store(ratioWidth, std::memory_order_relaxed);
				g_compositeMemberRatioHeight[Index].store(ratioHeight, std::memory_order_relaxed);
			}
			// Nothing here patches anything. Slot 3 is the only member that runs at the
			// reduced ratios, but its onRenderThread count is zero: it is RECORDING time,
			// and the engine's uploader maps the camera buffer with WRITE_DISCARD before
			// the replay, which renames the allocation and throws away anything written
			// here. The headset confirmed that on 2026-09-09 - 20,722 applied, every one
			// restored, and no visible change. The census stays because it is what found
			// the shape; the patch lives at the draw.
			if constexpr (kCompositeMemberSlot[Index] == 2) {
				g_compositeRecordings.fetch_add(1, std::memory_order_relaxed);
			}
			return g_compositeMemberOriginal[Index](
				a_shader, a_arg2, a_arg3, a_arg4, a_arg5, a_arg6);
		}

		// One census row per member, as a JSON array so a reading gets all eight at once:
		// [slot, calls, callsOnRenderThread, callsAtReducedRatios, ratioW, ratioH].
		[[nodiscard]] std::string CompositeMemberCensus()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < kCompositeMemberCount; ++i) {
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"([{},{},{},{},{:.6f},{:.6f}])"),
					kCompositeMemberSlot[i],
					g_compositeMemberCalls[i].load(std::memory_order_relaxed),
					g_compositeMemberOnRenderThread[i].load(std::memory_order_relaxed),
					g_compositeMemberReduced[i].load(std::memory_order_relaxed),
					g_compositeMemberRatioWidth[i].load(std::memory_order_relaxed),
					g_compositeMemberRatioHeight[i].load(std::memory_order_relaxed));
			}
			out += ']';
			return out;
		}

		[[nodiscard]] bool PatchVtableSlot(
			std::uintptr_t a_vtable,
			std::size_t a_slot,
			std::uintptr_t a_thunk,
			std::uintptr_t& a_original) noexcept
		{
			const auto slotAddress = a_vtable + a_slot * sizeof(std::uintptr_t);
			std::uintptr_t present = 0;
			if (!FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(slotAddress), &present, sizeof(present)) ||
				present == 0) {
				return false;
			}
			DWORD previous = 0;
			if (!::VirtualProtect(
					reinterpret_cast<void*>(slotAddress), sizeof(std::uintptr_t),
					PAGE_READWRITE, &previous)) {
				return false;
			}
			*reinterpret_cast<std::uintptr_t*>(slotAddress) = a_thunk;
			DWORD restored = 0;
			static_cast<void>(::VirtualProtect(
				reinterpret_cast<void*>(slotAddress), sizeof(std::uintptr_t),
				previous, &restored));
			a_original = present;
			return true;
		}

		bool g_compositeRenderPassPatched = false;

		void InstallCompositeScaleHooks()
		{
			// The arithmetic the whole correction rests on, checked against a synthetic
			// matrix at the two corners of the packed rectangle and at ratio one. A failure
			// here is a build that would write wrong rows into the engine's own buffer, so
			// it declines rather than logging a curiosity.
			if (!CompositeScale::SelfTest()) {
				logger::error("composite reconstruction scale algebra self-test failed - the correction is unavailable"sv);
				return;
			}
			logger::info("composite reconstruction scale algebra self-test passed"sv);

			const REL::Relocation<std::uintptr_t> compositeVtable{
				REL::Offset(Addr::kCompositeShaderVtable)
			};
			const REL::Relocation<std::uintptr_t> renderPass{
				REL::Offset(Addr::kCompositeShaderRenderPassFn)
			};
			std::uintptr_t presentRenderPass = 0;
			const auto renderPassSlot = compositeVtable.address() +
			                            Addr::kCompositeShaderRenderPassSlot * sizeof(std::uintptr_t);
			if (!FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(renderPassSlot),
					&presentRenderPass, sizeof(presentRenderPass)) ||
				presentRenderPass != renderPass.address()) {
				logger::warn(
					FMT_STRING("composite render-pass vtable slot holds {:016X}, expected {:016X} - the composite reconstruction scale is unavailable"),
					presentRenderPass, renderPass.address());
				return;
			}
			// Every slot is verified against the address Addresses.h derives for it before
			// it is written, and it is all eight or none: a half-patched vtable would give
			// a census that reads like a member that never runs.
			CompositeMember thunks[kCompositeMemberCount] = {
				&CompositeMemberThunk<0>, &CompositeMemberThunk<1>, &CompositeMemberThunk<2>,
				&CompositeMemberThunk<3>, &CompositeMemberThunk<4>, &CompositeMemberThunk<5>,
				&CompositeMemberThunk<6>, &CompositeMemberThunk<7>
			};
			for (std::size_t i = 0; i < kCompositeMemberCount; ++i) {
				const REL::Relocation<std::uintptr_t> expected{ REL::Offset(kCompositeMemberRva[i]) };
				std::uintptr_t present = 0;
				const auto slotAddress = compositeVtable.address() +
				                         kCompositeMemberSlot[i] * sizeof(std::uintptr_t);
				if (!FaultGuard::SafeReadBytes(
						reinterpret_cast<const void*>(slotAddress), &present, sizeof(present)) ||
					present != expected.address()) {
					logger::warn(
						FMT_STRING("composite vtable slot {} holds {:016X}, expected {:016X} - the composite reconstruction scale is unavailable"),
						kCompositeMemberSlot[i], present, expected.address());
					return;
				}
			}
			for (std::size_t i = 0; i < kCompositeMemberCount; ++i) {
				std::uintptr_t original = 0;
				if (!PatchVtableSlot(
						compositeVtable.address(), kCompositeMemberSlot[i],
						reinterpret_cast<std::uintptr_t>(thunks[i]), original)) {
					logger::warn("a composite vtable slot was not patched - the composite reconstruction scale is unavailable"sv);
					return;
				}
				g_compositeMemberOriginal[i] = reinterpret_cast<CompositeMember>(original);
			}
			g_compositeRenderPassPatched = true;
			g_compositeScaleInstalled = true;
			logger::info(
				FMT_STRING("composite reconstruction scale: all {} members counted at vtable {:016X}; the context's command-list slots follow at the first render-thread frame"),
				kCompositeMemberCount,
				compositeVtable.address());
		}

		[[nodiscard]] std::uintptr_t ResolveHDREffect(std::uintptr_t a_manager) noexcept
		{
			std::uintptr_t effect = 0;
			std::uintptr_t effects = 0;
			if (a_manager == 0 || !FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(a_manager + 0x18), &effects, sizeof(effects)) ||
				effects == 0 || !FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(effects + 3 * sizeof(std::uintptr_t)), &effect, sizeof(effect)) ||
				effect == 0) {
				return 0;
			}

			g_hdrEffect.store(effect, std::memory_order_relaxed);
			return effect;
		}

		[[nodiscard]] std::uintptr_t ResolveDynamicResolutionEffect(std::uintptr_t a_manager) noexcept
		{
			std::uintptr_t effect = 0;
			std::uintptr_t effects = 0;
			if (a_manager == 0 || !FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(a_manager + 0x18), &effects, sizeof(effects)) ||
				effects == 0 || !FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(effects + 21 * sizeof(std::uintptr_t)), &effect, sizeof(effect)) ||
				effect == 0) {
				return 0;
			}

			g_dynamicResolutionEffect.store(effect, std::memory_order_relaxed);
			return effect;
		}

		// The census names a dispatched effect by its index in the same effect array the
		// two resolvers above read at 3 and 21, so a pass listing speaks the vocabulary
		// the rest of this file already uses. The scan bound is a diagnostic limit, not a
		// length read from the binary: a slot past the real array reads whatever follows
		// it, and whatever that is cannot equal a live effect pointer, so an unmatched
		// dispatch reports -1 rather than a wrong index.
		constexpr std::size_t kEffectSlotScanLimit = 48;

		[[nodiscard]] std::int32_t ResolveEffectSlot(std::uintptr_t a_manager, std::uintptr_t a_effect) noexcept
		{
			std::uintptr_t effects = 0;
			if (a_manager == 0 || a_effect == 0 ||
				!FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(a_manager + 0x18), &effects, sizeof(effects)) ||
				effects == 0) {
				return -1;
			}
			std::uintptr_t slots[kEffectSlotScanLimit]{};
			if (!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(effects), slots, sizeof(slots))) {
				return -1;
			}
			for (std::size_t i = 0; i < kEffectSlotScanLimit; ++i) {
				if (slots[i] == a_effect) {
					return static_cast<std::int32_t>(i);
				}
			}
			return -1;
		}

		[[nodiscard]] std::uint64_t ObserveHDRInput(
			std::uintptr_t a_effect,
			std::int32_t a_sourceLogical,
			std::int32_t a_destinationLogical) noexcept
		{
			const auto call = g_hdrCalls.fetch_add(1, std::memory_order_relaxed) + 1;

			const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
			auto expectedThread = std::uint32_t{ 0 };
			if (!g_hdrThreadId.compare_exchange_strong(
					expectedThread, thread, std::memory_order_relaxed, std::memory_order_relaxed) &&
				expectedThread != thread) {
				g_hdrThreadMismatches.fetch_add(1, std::memory_order_relaxed);
			}

			const auto previousSource =
				g_hdrSourceLogical.exchange(a_sourceLogical, std::memory_order_relaxed);
			const auto previousDestination =
				g_hdrDestinationLogical.exchange(a_destinationLogical, std::memory_order_relaxed);
			if ((previousSource != -1 && previousSource != a_sourceLogical) ||
				(previousDestination != -1 && previousDestination != a_destinationLogical) ||
				g_hdrEffect.load(std::memory_order_relaxed) != a_effect) {
				g_hdrRouteMismatches.fetch_add(1, std::memory_order_relaxed);
			}
			return call;
		}

		struct HDRInputProbeHook
		{
			static void thunk(
				std::uintptr_t a_manager,
				std::uintptr_t a_renderParams,
				std::uintptr_t a_effect,
				std::uint32_t a_sourceLogical,
				std::uint32_t a_destinationLogical,
				std::uintptr_t a_arg6)
			{
				// Every effect the manager dispatches passes here, so this is where a
				// census learns the pass boundaries and the source/destination logical
				// targets the manager chose for each one.
				if (Debug::FrameCensusRecording()) {
					Debug::OnImageSpaceDispatch(
						ResolveEffectSlot(a_manager, a_effect),
						static_cast<std::int32_t>(a_sourceLogical),
						static_cast<std::int32_t>(a_destinationLogical));
				}
				const auto hdrEffect = ResolveHDREffect(a_manager);
				if (a_effect != 0 && a_effect == hdrEffect) {
					if (g_dynamicEffectPassthrough.active || g_dynamicGeometryFlagArmed ||
						g_dynamicEffectNeutralizationPending || g_dynamicTransactionEpoch != 0) {
						const bool geometryRestored = EndDynamicNeutralization(true);
						const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
						if (!geometryRestored || !pixelsRestored) {
							DisableDynamicGeometryTransaction("the next HDR dispatch found a stale transaction that did not restore"sv);
							// Do not run vanilla HDR with unity ratios or partially armed slot-21
							// geometry. Holding this dispatch leaves the previous coherent output
							// in place for the pre-world watchdog to repair on the next frame.
							return;
						}
					}
					if (!g_dynamicEffectPassthrough.active && !g_dynamicGeometryFlagArmed) {
						ClearDynamicNeutralizationMarkers();
					}
					if (ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
						func(
							a_manager,
							a_renderParams,
							a_effect,
							a_sourceLogical,
							a_destinationLogical,
							a_arg6);
						return;
					}
					const auto call = ObserveHDRInput(
						a_effect,
						static_cast<std::int32_t>(a_sourceLogical),
						static_cast<std::int32_t>(a_destinationLogical));
					g_activeHDRCall = call;
					g_activeHDRDestination = static_cast<std::int32_t>(a_destinationLogical);
					bool normalReturn = false;
					// One relaxed load on the shipped path. Everything the reduced
					// transaction does - the vendor evaluate, the private copies and every
					// isolation bracket in it - happens between these two probes, so a
					// depth-stencil view that is bound at the first and gone at the second
					// was dropped inside the transaction and by nothing else. Read once
					// here and passed by value to the second site: a census that closed
					// mid-transaction must not leave one probe of a pair recorded.
					const bool censusRecording = Debug::FrameCensusRecording();
					if (censusRecording) {
						Debug::OnSeamStateProbe(Debug::CensusProbeSite::preTransaction);
					}
					__try {
						HDRRoundTrip::OnDispatch(
							a_manager,
							static_cast<std::int32_t>(a_sourceLogical),
							static_cast<std::int32_t>(a_destinationLogical));
						if (censusRecording) {
							Debug::OnSeamStateProbe(Debug::CensusProbeSite::postTransaction);
						}
						const bool holdCurrentHdr = HDRRoundTrip::ConsumeQualityHDRHold();
						bool closeBeforeVanillaHdr = holdCurrentHdr;
						if (HDRRoundTrip::ConsumeDynamicUpsampleSuppression()) {
							if (!g_dynamicEffectPassthrough.active || !g_dynamicGeometryFlagArmed ||
								g_dynamicGeometryEffect == 0) {
								DisableDynamicGeometryTransaction("HDRRoundTrip committed engine colour without a prepared slot-21 transaction"sv);
								closeBeforeVanillaHdr = true;
							} else {
								g_dynamicEffectNeutralizationPending = true;
								g_qualityImageSpaceDepthOverrideActive =
									g_unbindDepthStencil.load(std::memory_order_acquire);
								g_qualityImageSpaceDepthOverrideFailed = false;
							}
						} else if (g_dynamicEffectPassthrough.active || g_dynamicGeometryFlagArmed ||
							g_dynamicEffectNeutralizationPending || g_dynamicTransactionEpoch != 0) {
							// A normal OnDispatch abort is allowed to return only after the unity
							// ratio/geometry bracket closes. Retry the idempotent close once in the
							// outer HDR hook, where we can suppress vanilla HDR if it still fails.
							closeBeforeVanillaHdr = true;
						}
						if (closeBeforeVanillaHdr) {
							const bool geometryRestored = EndDynamicNeutralization(true);
							const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
							if (holdCurrentHdr || !geometryRestored || !pixelsRestored) {
								DisableDynamicGeometryTransaction("the pre-HDR quality abort could not close the ratio/geometry bracket before vanilla HDR"sv);
								// This is a handled fail-closed hold, not an engine exception. Mark the
								// hook return normal so the finally block only clears TLS ownership.
								normalReturn = true;
								return;
							}
						}
						func(
							a_manager,
							a_renderParams,
							a_effect,
							a_sourceLogical,
							a_destinationLogical,
							a_arg6);
						normalReturn = true;
					} __finally {
						g_activeHDRDestination = -1;
						g_activeHDRCall = 0;
						if (!normalReturn) {
							bool terminalRestored = false;
							__try {
								terminalRestored = EndDynamicNeutralization(true);
							} __except (EXCEPTION_EXECUTE_HANDLER) {
								terminalRestored = false;
							}
							static_cast<void>(HDRRoundTrip::AbortQualityHandbackCandidate());
							if (!terminalRestored) {
								DisableDynamicGeometryTransaction("abnormal HDR return could not close the quality ratio/geometry bracket"sv);
							}
						}
					}
					return;
				}
				const auto dynamicEffect = ResolveDynamicResolutionEffect(a_manager);
				if (a_effect != 0 && a_effect == dynamicEffect) {
					g_dynamicResolutionCalls.fetch_add(1, std::memory_order_relaxed);
					const bool pendingQuality = g_dynamicEffectNeutralizationPending;
					if (!pendingQuality &&
						ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
						func(
							a_manager,
							a_renderParams,
							a_effect,
							a_sourceLogical,
							a_destinationLogical,
							a_arg6);
						return;
					}
					if (pendingQuality && g_dynamicEffectPassthrough.active &&
						g_dynamicGeometryFlagArmed) {
						// Not reached on a correct frame: the IsActive hook reports slot 21
						// inactive while the transaction owns the frame, so the range never
						// dispatches it. If it is dispatched anyway, run it with +0xA8 still
						// armed so its copy at least uses the unity geometry, record what it
						// routed, and let the late range disable the transaction: its copy of
						// logical 1 has overwritten this frame's RT4.
						std::uint8_t geometryFlag = 0xFF;
						const bool geometryReady = a_manager == g_dynamicEffectPassthrough.manager &&
						                           g_dynamicTransactionEpoch != 0 &&
						                           g_dynamicTransactionEpoch == g_preWorldEpoch &&
						                           a_effect == g_dynamicGeometryEffect &&
						                           ReadEffectByte(g_dynamicGeometryFlagAddress, geometryFlag) &&
						                           geometryFlag == 1 &&
						                           DynamicResolution::EffectPassthroughReady();
						if (geometryReady) {
							g_dynamicResolutionNeutralizedSourceLogical.store(
								static_cast<std::int32_t>(a_sourceLogical), std::memory_order_relaxed);
							g_dynamicResolutionNeutralizedDestinationLogical.store(
								static_cast<std::int32_t>(a_destinationLogical), std::memory_order_relaxed);
							bool flagRestored = false;
							bool normalReturn = false;
							__try {
								func(
									a_manager,
									a_renderParams,
									a_effect,
									a_sourceLogical,
									a_destinationLogical,
									a_arg6);
								normalReturn = true;
							} __finally {
								if (normalReturn) {
									flagRestored = RestoreDynamicGeometryFlagSilent();
								} else {
									bool terminalRestored = false;
									__try {
										terminalRestored = EndDynamicNeutralization(true);
									} __except (EXCEPTION_EXECUTE_HANDLER) {
										terminalRestored = false;
									}
									static_cast<void>(HDRRoundTrip::AbortQualityHandbackCandidate());
									if (!terminalRestored) {
										DisableDynamicGeometryTransaction("abnormal slot-21 return could not close the quality ratio/geometry bracket"sv);
									}
								}
							}
							if (!flagRestored) {
								DisableDynamicGeometryTransaction("slot 21 +0xA8 did not restore to zero after its normal dispatch"sv);
								static_cast<void>(HDRRoundTrip::AbortQualityHandbackCandidate());
								return;
							}
							g_dynamicNeutralizationSawEffect = true;
							g_dynamicResolutionDynamicGeometryDispatches.fetch_add(1, std::memory_order_relaxed);
						} else {
							DisableDynamicGeometryTransaction("slot 21 did not retain its committed +0xA8 dynamic-geometry flag"sv);
							const bool geometryRestored = EndDynamicNeutralization(true);
							const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
							if (!geometryRestored || !pixelsRestored) {
								DisableDynamicGeometryTransaction("slot 21 fallback could not first restore requested ratios and geometry"sv);
								return;
							}
							func(
								a_manager,
								a_renderParams,
								a_effect,
								a_sourceLogical,
								a_destinationLogical,
								a_arg6);
							return;
						}
						g_dynamicResolutionSuppressions.fetch_add(1, std::memory_order_relaxed);
						return;
					}
					if (pendingQuality) {
						DisableDynamicGeometryTransaction("dynamic resolution reached a pending quality candidate with a partially lost unity/dynamic-geometry bracket"sv);
						const bool geometryRestored = EndDynamicNeutralization(true);
						const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
						if (!geometryRestored || !pixelsRestored) {
							return;
						}
						func(
							a_manager,
							a_renderParams,
							a_effect,
							a_sourceLogical,
							a_destinationLogical,
							a_arg6);
						return;
					}
					g_dynamicResolutionPassthroughs.fetch_add(1, std::memory_order_relaxed);
					func(
						a_manager,
						a_renderParams,
						a_effect,
						a_sourceLogical,
						a_destinationLogical,
						a_arg6);
					if (!DynamicResolution::RestoreRequestedAfterEngineEffect() &&
						!g_dynamicResolutionReassertFailureLogged.exchange(true, std::memory_order_relaxed)) {
						logger::error("dynamic resolution could not restore its requested ratio after the engine effect"sv);
					}
					return;
				}
				func(
					a_manager,
					a_renderParams,
					a_effect,
					a_sourceLogical,
					a_destinationLogical,
					a_arg6);
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallHDRInputProbe(std::uintptr_t a_site)
		{
			pstl::write_thunk_call<HDRInputProbeHook>(a_site);
		}

		struct Site
		{
			// Zero means the address has not been read out of the binary yet. It is
			// never a value to guess at - see Addresses.h and docs/MODULE_MAP.md.
			std::uintptr_t rva = 0;

			const std::uint8_t* expected = nullptr;
			std::size_t expectedLen = 0;
			std::string_view name{};

			// Tier 2 only: what stops working when this site declines, in the words the
			// user reads in the log.
			std::string_view degradation{};

			// Tier 2 only: the flag the runtime branches on.
			bool* installed = nullptr;

			// Tiers 1 and 2: writes the thunk. One function per site, because
			// write_thunk_call takes the hook struct as a template argument.
			void (*install)(std::uintptr_t a_site) = nullptr;

			// Tier 3 only: the setting that gates the patch, and whether the caller
			// consumes the return value. Choosing the wrong payload is a refcount
			// corruption that surfaces a long way from here.
			bool (*gate)() = nullptr;
			bool nullsReturn = false;
		};

		[[nodiscard]] std::string FormatBytes(const std::uint8_t* a_p, std::size_t a_len)
		{
			std::string out;
			for (std::size_t i = 0; i < a_len; ++i) {
				out += fmt::format(FMT_STRING("{}{:02X}"), i != 0 ? " " : "", a_p[i]);
			}
			return out;
		}

		// Naming the site and printing the bytes actually found is what turns a version
		// mismatch from a bug report into a one-line diagnosis.
		[[nodiscard]] bool VerifyBytes(
			const REL::Relocation<std::uintptr_t>& a_target,
			std::span<const std::uint8_t> a_expected,
			std::string_view a_what)
		{
			const auto len = a_expected.size();
			if (len == 0 || len > kMaxSiteBytes) {
				logger::critical(FMT_STRING("byte check failed at {}: {} expected bytes is outside 1..{}"), a_what, len, kMaxSiteBytes);
				return false;
			}

			// Read through the fault guard rather than dereferencing directly: a wrong
			// address should produce a log line, not a crash inside the verifier that
			// exists to prevent crashes.
			std::uint8_t found[kMaxSiteBytes] = {};
			if (!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(a_target.address()), found, len)) {
				logger::critical(
					FMT_STRING("byte check failed at {} ({:016X}): the site is unreadable - wrong game version? Leaving the game untouched."),
					a_what, a_target.address());
				return false;
			}

			if (!std::equal(a_expected.begin(), a_expected.end(), found)) {
				logger::critical(
					FMT_STRING("byte check failed at {} ({:016X}): expected {}, found {} - wrong game version? Leaving the game untouched."),
					a_what, a_target.address(), FormatBytes(a_expected.data(), len), FormatBytes(found, len));
				return false;
			}
			return true;
		}

		[[nodiscard]] bool CheckSite(const Site& a_site)
		{
			if (a_site.rva == 0) {
				logger::warn(FMT_STRING("{}: no verified address yet - skipped"), a_site.name);
				return false;
			}
			if (!a_site.expected || a_site.expectedLen == 0) {
				logger::critical(FMT_STRING("{}: no original bytes declared, so the site cannot be verified"), a_site.name);
				return false;
			}
			const REL::Relocation<std::uintptr_t> target{ REL::Offset(a_site.rva) };
			return VerifyBytes(target, { a_site.expected, a_site.expectedLen }, a_site.name);
		}

		// Tier 1. Every hard site is verified before any is patched, which is what makes
		// a partial patch impossible: a mismatch anywhere leaves the game untouched.
		constexpr std::span<const Site> kHardSites{};

		// Tier 2. One optional feature each. A mismatch costs that feature and says so.
		static constexpr std::uint8_t kFiveByteEntry[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08  // MOV [RSP+8],RBX
		};
		static constexpr std::uint8_t kImageSpacePixelDrawCall[] = {
			0xE8, 0x64, 0x80, 0xF8, 0xFF
		};
		static constexpr std::uint8_t kImageSpaceEffectDispatchCall[] = {
			0xE8, 0xB8, 0x02, 0x00, 0x00
		};
		static constexpr std::uint8_t kDynamicResolutionIsActiveEntry[] = {
			0xF3, 0x0F, 0x10, 0x0D, 0xA8, 0x30, 0x37, 0x00
		};
		static constexpr std::uint8_t kDynamicResolutionGeometryUpdateCall[] = {
			0xE8, 0xAB, 0x1D, 0xF6, 0xFF
		};
		static constexpr std::uint8_t kLateImageSpaceRangeCall[] = {
			0xE8, 0xD4, 0x18, 0xF6, 0xFF
		};
		static constexpr std::uint8_t kJitterUpdateCall[] = {
			0xE8, 0xDC, 0x87, 0x56, 0xFF
		};
		static constexpr std::uint8_t kUiRenderAllCall[] = {
			0xFF, 0x15, 0x20, 0x78, 0x03, 0x04
		};
		// The four SSLR draw calls. All four are E8 to the same executor from four
		// different overrides, so the displacements differ and the four byte strings are
		// what tell the sites apart.
		static constexpr std::uint8_t kSslrBlurHDrawCall[] = {
			0xE8, 0xE0, 0x5E, 0x0B, 0x00
		};
		static constexpr std::uint8_t kSslrBlurVDrawCall[] = {
			0xE8, 0x60, 0x5E, 0x0B, 0x00
		};
		static constexpr std::uint8_t kSslrPrepassDrawCall[] = {
			0xE8, 0x11, 0x5E, 0x0B, 0x00
		};
		static constexpr std::uint8_t kSslrRaytracingDrawCall[] = {
			0xE8, 0x76, 0x5D, 0x0B, 0x00
		};
		// The two BSWaterShader entries. Seven bytes preserved at the first because its
		// prologue's second instruction ends there; five at the second.
		static constexpr std::uint8_t kWaterShaderSetupGeometryEntry[] = {
			0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08
		};
		static constexpr std::uint8_t kWaterShaderSetupTechniqueEntry[] = {
			0x48, 0x89, 0x5C, 0x24, 0x18
		};
		static constexpr Site kSoftSiteStorage[] = {
			{
				Addr::kClearColorFn,
				kFiveByteEntry,
				std::size(kFiveByteEntry),
				"render-thread ClearColor probe"sv,
				"render-thread measurements and captures are unavailable"sv,
				&g_renderThreadProbeInstalled,
				&InstallClearColorProbe,
				nullptr,
				false,
			},
			{
				Addr::kTemporalAARenderFn,
				kFiveByteEntry,
				std::size(kFiveByteEntry),
				"native TemporalAA entry probe"sv,
				"TemporalAA stage cadence and thread measurements are unavailable"sv,
				&g_temporalAAProbeInstalled,
				&InstallTemporalAAProbe,
				nullptr,
				false,
			},
			{
				Addr::kImageSpaceEffectDispatchCall,
				kImageSpaceEffectDispatchCall,
				std::size(kImageSpaceEffectDispatchCall),
				"pre-tonemap ImageSpace HDR dispatch observation call"sv,
				"HDR input source/destination measurements are unavailable"sv,
				&g_hdrInputProbeInstalled,
				&InstallHDRInputProbe,
				nullptr,
				false,
			},
			{
				Addr::kImageSpacePixelDrawCall,
				kImageSpacePixelDrawCall,
				std::size(kImageSpacePixelDrawCall),
				"ImageSpace pixel-draw observation call"sv,
				"HDR-final and TemporalAA OM/PS binding observations are unavailable"sv,
				&g_imageSpacePixelDrawProbeInstalled,
				&InstallImageSpacePixelDrawProbe,
				nullptr,
				false,
			},
			{
				Addr::kDynamicResolutionIsActiveFn,
				kDynamicResolutionIsActiveEntry,
				std::size(kDynamicResolutionIsActiveEntry),
				"dynamic resolution IsActive entry probe"sv,
				"full-output vendor handbacks cannot preserve effect routing while live ratios are unity"sv,
				&g_dynamicResolutionIsActiveProbeInstalled,
				&InstallDynamicResolutionIsActiveProbe,
				nullptr,
				false,
			},
			{
				Addr::kDynamicResolutionGeometryUpdateCall,
				kDynamicResolutionGeometryUpdateCall,
				std::size(kDynamicResolutionGeometryUpdateCall),
				"dynamic-resolution ImageSpace geometry update call"sv,
				"full-output vendor handbacks cannot refresh the dynamic screen triangle after changing live ratios"sv,
				&g_dynamicResolutionGeometryUpdateProbeInstalled,
				&InstallDynamicResolutionGeometryUpdateProbe,
				nullptr,
				false,
			},
			{
				Addr::kLateImageSpaceRangeCall,
				kLateImageSpaceRangeCall,
				std::size(kLateImageSpaceRangeCall),
				"late ImageSpace range cleanup call"sv,
				"full-output vendor handbacks cannot guarantee post-slot-21 transaction cleanup"sv,
				&g_lateImageSpaceRangeProbeInstalled,
				&InstallLateImageSpaceRangeProbe,
				nullptr,
				false,
			},
			{
				Addr::kJitterUpdateCall,
				kJitterUpdateCall,
				std::size(kJitterUpdateCall),
				"native TAA jitter commit call"sv,
				"backend-owned jitter is unavailable; the reduced-input path stays on the engine's 8-phase sequence"sv,
				&g_jitterCommitInstalled,
				&InstallJitterCommit,
				nullptr,
				false,
			},
			{
				Addr::kUiRenderAllCall,
				kUiRenderAllCall,
				std::size(kUiRenderAllCall),
				"UI render-entry observation call"sv,
				"the state the HUD draw runs under - the jitter floats, the live ratios, the viewport - cannot be read"sv,
				&g_uiRenderProbeInstalled,
				&InstallUiRenderProbe,
				nullptr,
				false,
			},
			{
				Addr::kSslrPrepassDrawCall,
				kSslrPrepassDrawCall,
				std::size(kSslrPrepassDrawCall),
				"SSLR prepass draw observation call"sv,
				"the screen-space-reflection prepass cannot be seen by the frame census"sv,
				&g_sslrPrepassProbeInstalled,
				&InstallSslrPrepassDrawProbe,
				nullptr,
				false,
			},
			{
				Addr::kSslrRaytracingDrawCall,
				kSslrRaytracingDrawCall,
				std::size(kSslrRaytracingDrawCall),
				"SSLR raytracing draw observation call"sv,
				"the pass that writes the reflection colour target cannot be seen by the frame census"sv,
				&g_sslrRaytracingProbeInstalled,
				&InstallSslrRaytracingDrawProbe,
				nullptr,
				false,
			},
			{
				Addr::kSslrBlurHDrawCall,
				kSslrBlurHDrawCall,
				std::size(kSslrBlurHDrawCall),
				"SSLR horizontal blur draw observation call"sv,
				"the reflection horizontal blur cannot be seen by the frame census"sv,
				&g_sslrBlurHProbeInstalled,
				&InstallSslrBlurHDrawProbe,
				nullptr,
				false,
			},
			{
				Addr::kSslrBlurVDrawCall,
				kSslrBlurVDrawCall,
				std::size(kSslrBlurVDrawCall),
				"SSLR vertical blur draw observation call"sv,
				"the reflection vertical blur cannot be seen by the frame census"sv,
				&g_sslrBlurVProbeInstalled,
				&InstallSslrBlurVDrawProbe,
				nullptr,
				false,
			},
			{
				Addr::kWaterShaderSetupGeometryFn,
				kWaterShaderSetupGeometryEntry,
				std::size(kWaterShaderSetupGeometryEntry),
				"water shader per-geometry constant setup entry probe"sv,
				"the water surface's screen and dynamic-resolution constants cannot be read at the draw"sv,
				&g_waterSetupGeometryProbeInstalled,
				&InstallWaterSetupGeometryProbe,
				nullptr,
				false,
			},
			{
				Addr::kWaterShaderSetupTechniqueFn,
				kWaterShaderSetupTechniqueEntry,
				std::size(kWaterShaderSetupTechniqueEntry),
				"water shader per-technique constant setup entry probe"sv,
				"whether the water surface reaches its other constant-setup entry cannot be seen"sv,
				&g_waterSetupTechniqueProbeInstalled,
				&InstallWaterSetupTechniqueProbe,
				nullptr,
				false,
			},
		};
		constexpr std::span<const Site> kSoftSites{ kSoftSiteStorage };

		// Tier 3. Setting-gated raw byte patches, verified the same way.
		constexpr std::span<const Site> kPatchSites{};
	}

	void Configure(
		bool a_unbindDepthStencil,
		bool a_rebindDepthStencilAfterSeam,
		bool a_clearStencilAfterSeam,
		bool a_resolveDepthAfterSeam,
		bool a_compositeReconstructionScale) noexcept
	{
		g_unbindDepthStencil.store(a_unbindDepthStencil, std::memory_order_release);
		g_rebindDepthStencilAfterSeam.store(a_rebindDepthStencilAfterSeam, std::memory_order_release);
		g_clearStencilAfterSeam.store(a_clearStencilAfterSeam, std::memory_order_release);
		g_resolveDepthAfterSeam.store(a_resolveDepthAfterSeam, std::memory_order_release);
		g_compositeReconstructionScale.store(a_compositeReconstructionScale, std::memory_order_release);
		CompositeScale::SetEnabled(a_compositeReconstructionScale);
	}

	bool Install()
	{
		g_patched = 0;

		for (const auto& site : kHardSites) {
			if (!site.install) {
				logger::critical(FMT_STRING("hook install declined: {} declares no installer"), site.name);
				return false;
			}
			if (!CheckSite(site)) {
				logger::critical(FMT_STRING("hook install declined: {} did not verify"), site.name);
				return false;
			}
		}
		for (const auto& site : kHardSites) {
			const REL::Relocation<std::uintptr_t> target{ REL::Offset(site.rva) };
			site.install(target.address());
			++g_patched;
			logger::info(FMT_STRING("{} installed at {:016X}"), site.name, target.address());
		}

		for (const auto& site : kSoftSites) {
			if (site.installed) {
				*site.installed = false;
			}
			if (!site.install || !CheckSite(site)) {
				logger::warn(FMT_STRING("{} not installed - {}"), site.name, site.degradation);
				continue;
			}
			const REL::Relocation<std::uintptr_t> target{ REL::Offset(site.rva) };
			site.install(target.address());
			if (site.installed) {
				*site.installed = true;
			}
			++g_patched;
			logger::info(FMT_STRING("{} installed at {:016X}"), site.name, target.address());
		}

		for (const auto& site : kPatchSites) {
			if (site.gate && !site.gate()) {
				continue;
			}
			if (!CheckSite(site)) {
				continue;
			}
			// xor eax,eax + 3 nops where the caller consumes the return value - a
			// refcount-stored return must be nulled, not left as garbage. Five nops
			// where it does not.
			static constexpr std::uint8_t kNullReturnPatch[] = { 0x33, 0xC0, 0x90, 0x90, 0x90 };
			static constexpr std::uint8_t kNopPatch[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };

			const auto* payload = site.nullsReturn ? kNullReturnPatch : kNopPatch;
			const REL::Relocation<std::uintptr_t> target{ REL::Offset(site.rva) };
			REL::safe_write(target.address(), payload, sizeof(kNopPatch));
			++g_patched;
			logger::info(FMT_STRING("{} patched"), site.name);
		}

		InstallCompositeScaleHooks();
		if (g_compositeRenderPassPatched) {
			++g_patched;
		}

		g_installed = true;
		if (g_patched == 0) {
			logger::info("no hook sites are resolved yet, so nothing was patched and the game is byte-for-byte untouched. docs/MODULE_MAP.md lists the measurement each site waits on."sv);
		}
		return true;
	}

	// Whether anything is actually patched, not whether Install ran. The distinction
	// matters while the site list is empty: Install succeeds and patches nothing, and a
	// reader who sees "installed" next to "0 sites" cannot tell that from a real install
	// that silently declined every site.
	bool Installed() noexcept
	{
		return g_installed && g_patched != 0;
	}

	std::size_t SiteCount() noexcept
	{
		return g_patched;
	}

	RenderThreadProbeState RenderThreadProbe() noexcept
	{
		return {
			g_renderThreadProbeInstalled,
			g_renderThreadCalls.load(std::memory_order_relaxed),
			g_renderThreadId.load(std::memory_order_relaxed),
			g_renderThreadMismatches.load(std::memory_order_relaxed),
			g_observedRenderer.load(std::memory_order_relaxed),
			g_rendererMismatches.load(std::memory_order_relaxed),
		};
	}

	TemporalAAProbeState TemporalAAProbe() noexcept
	{
		return {
			g_temporalAAProbeInstalled,
			g_imageSpacePixelDrawProbeInstalled,
			g_temporalAACalls.load(std::memory_order_relaxed),
			g_temporalAAThreadId.load(std::memory_order_relaxed),
			g_temporalAAThreadMismatches.load(std::memory_order_relaxed),
			g_temporalAAEffect.load(std::memory_order_relaxed),
			g_temporalAAEffectMismatches.load(std::memory_order_relaxed),
			g_temporalAANativeDraws.load(std::memory_order_relaxed),
			g_temporalAAFallbackNativeDraws.load(std::memory_order_relaxed),
		};
	}

	HDRInputProbeState HDRInputProbe() noexcept
	{
		return {
			g_hdrInputProbeInstalled,
			g_imageSpacePixelDrawProbeInstalled,
			g_hdrCalls.load(std::memory_order_relaxed),
			g_hdrThreadId.load(std::memory_order_relaxed),
			g_hdrThreadMismatches.load(std::memory_order_relaxed),
			g_hdrEffect.load(std::memory_order_relaxed),
			g_hdrSourceLogical.load(std::memory_order_relaxed),
			g_hdrDestinationLogical.load(std::memory_order_relaxed),
			g_hdrRouteMismatches.load(std::memory_order_relaxed),
		};
	}

	bool JitterCommitInstalled() noexcept
	{
		return g_jitterCommitInstalled;
	}

	WaterProbeState WaterProbe() noexcept
	{
		return {
			g_waterSetupGeometryProbeInstalled,
			g_waterSetupTechniqueProbeInstalled,
			g_waterSetupGeometryCalls.load(std::memory_order_relaxed),
			g_waterSetupTechniqueCalls.load(std::memory_order_relaxed),
			g_waterSetupThreadId.load(std::memory_order_relaxed),
			g_waterSetupThreadMismatches.load(std::memory_order_relaxed),
			g_waterSetupOffRenderThread.load(std::memory_order_relaxed),
			g_renderThreadId.load(std::memory_order_relaxed),
		};
	}

	SslrProbeState SslrProbe() noexcept
	{
		return {
			g_sslrPrepassProbeInstalled,
			g_sslrRaytracingProbeInstalled,
			g_sslrBlurHProbeInstalled,
			g_sslrBlurVProbeInstalled,
			g_sslrPrepassDraws.load(std::memory_order_relaxed),
			g_sslrRaytracingDraws.load(std::memory_order_relaxed),
			g_sslrBlurHDraws.load(std::memory_order_relaxed),
			g_sslrBlurVDraws.load(std::memory_order_relaxed),
		};
	}

	bool UiRenderProbeInstalled() noexcept
	{
		return g_uiRenderProbeInstalled;
	}

	std::uint64_t UiRenderNullCallbacks() noexcept
	{
		return g_uiRenderNullCallbacks.load(std::memory_order_relaxed);
	}

	bool QualityHandbackReady() noexcept
	{
		const auto taa = TAAReplacement::Snapshot();
		const bool coupledTaaReady = taa.enabled && taa.requireNgxHandback &&
		                             taa.bypassEnabled && !taa.postDrawEnabled &&
		                             !taa.sessionDisabled && !taa.completed;
		return coupledTaaReady && g_hdrInputProbeInstalled && g_dynamicResolutionIsActiveProbeInstalled &&
		       g_dynamicResolutionGeometryUpdateProbeInstalled && g_lateImageSpaceRangeProbeInstalled &&
		       g_temporalAAProbeInstalled && g_imageSpacePixelDrawProbeInstalled &&
		       !g_dynamicGeometrySessionDisabled.load(std::memory_order_acquire);
	}

	ResetPolicy::WorldAdmissionTicket CurrentQualityWorldAdmission() noexcept
	{
		if (!DynamicResolution::WorldAdmissionGated() ||
			!DynamicResolution::WorldFrameActive() || !g_worldContract.active ||
			!g_worldAdmission.admitted || g_worldContractEpoch == 0 ||
			g_worldContractEpoch != g_preWorldEpoch ||
			ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
			return {};
		}
		return g_worldAdmission;
	}

	bool BeginQualityHandback(std::uintptr_t a_manager) noexcept
	{
		if (!QualityHandbackReady() || a_manager == 0 || g_dynamicEffectPassthrough.active ||
			g_dynamicGeometryFlagArmed || g_dynamicEffectNeutralizationPending ||
			g_dynamicTransactionEpoch != 0 ||
			ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
			return false;
		}
		g_dynamicNeutralizationSawTaa = false;
		g_dynamicNeutralizationTaaSucceeded = false;
		g_dynamicNeutralizationSawEffect = false;
		g_dynamicExpectedTaaCall = 0;
		return BeginDynamicNeutralization(a_manager);
	}

	bool QualityHandbackCommitReady(std::uintptr_t a_manager) noexcept
	{
		if (!QualityHandbackReady() || a_manager == 0 ||
			ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain ||
			!g_dynamicEffectPassthrough.active || !g_dynamicGeometryFlagArmed ||
			g_dynamicEffectPassthrough.manager != a_manager || g_dynamicTransactionEpoch == 0 ||
			g_dynamicTransactionEpoch != g_preWorldEpoch ||
			ResolveDynamicResolutionEffect(a_manager) != g_dynamicGeometryEffect ||
			g_dynamicGeometryFlagAddress != g_dynamicGeometryEffect + 0xA8 ||
			!DynamicResolution::EffectPassthroughReady()) {
			return false;
		}

		std::uintptr_t vtable = 0;
		std::uintptr_t renderEntry = 0;
		std::uintptr_t isActiveEntry = 0;
		std::uint8_t flag = 0xFF;
		const auto expectedVtable = ResolveDynamicResolutionVtable();
		const REL::Relocation<std::uintptr_t> expectedRender{ REL::Offset(Addr::kDynamicResolutionEffectRenderFn) };
		const REL::Relocation<std::uintptr_t> expectedIsActive{ REL::Offset(Addr::kDynamicResolutionIsActiveFn) };
		return expectedVtable != 0 &&
		       FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(g_dynamicGeometryEffect), &vtable, sizeof(vtable)) &&
		       vtable == expectedVtable &&
		       FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(vtable + Addr::kDynamicResolutionEffectVtableRenderSlot), &renderEntry, sizeof(renderEntry)) &&
		       FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(vtable + Addr::kDynamicResolutionEffectVtableIsActiveSlot), &isActiveEntry, sizeof(isActiveEntry)) &&
		       renderEntry == expectedRender.address() && isActiveEntry == expectedIsActive.address() &&
		       ReadEffectByte(g_dynamicGeometryFlagAddress, flag) && flag == 1;
	}

	bool AbortQualityHandback() noexcept
	{
		if (!g_dynamicEffectPassthrough.active && !g_dynamicGeometryFlagArmed &&
			!g_dynamicEffectNeutralizationPending && g_dynamicTransactionEpoch == 0) {
			ClearDynamicNeutralizationMarkers();
			return true;
		}
		return EndDynamicNeutralization(true);
	}

	bool DynamicGeometrySessionDisabled() noexcept
	{
		return g_dynamicGeometrySessionDisabled.load(std::memory_order_acquire);
	}

	const char* DynamicGeometryDisableReason() noexcept
	{
		const auto* reason = g_dynamicGeometryDisableReason.load(std::memory_order_acquire);
		return reason ? reason : "none";
	}

	DynamicResolutionDispatchState DynamicResolutionDispatch() noexcept
	{
		DynamicResolutionDispatchState state{};
		state.installed = g_hdrInputProbeInstalled && g_dynamicResolutionIsActiveProbeInstalled &&
		                  g_dynamicResolutionGeometryUpdateProbeInstalled && g_lateImageSpaceRangeProbeInstalled;
		state.qualityReady = QualityHandbackReady();
		state.effect = g_dynamicResolutionEffect.load(std::memory_order_relaxed);
		state.calls = g_dynamicResolutionCalls.load(std::memory_order_relaxed);
		state.suppressions = g_dynamicResolutionSuppressions.load(std::memory_order_relaxed);
		state.passthroughs = g_dynamicResolutionPassthroughs.load(std::memory_order_relaxed);
		state.geometryRefreshes = g_dynamicResolutionGeometryRefreshes.load(std::memory_order_relaxed);
		state.geometryFlagPreflights = g_dynamicResolutionGeometryFlagPreflights.load(std::memory_order_relaxed);
		state.dynamicGeometryDispatches = g_dynamicResolutionDynamicGeometryDispatches.load(std::memory_order_relaxed);
		state.dynamicGeometryFailures = g_dynamicResolutionDynamicGeometryFailures.load(std::memory_order_relaxed);
		state.missingTaaClosures = g_dynamicResolutionMissingTaaClosures.load(std::memory_order_relaxed);
		state.preWorldCalls = g_dynamicResolutionPreWorldCalls.load(std::memory_order_relaxed);
		state.preWorldRepairs = g_dynamicResolutionPreWorldRepairs.load(std::memory_order_relaxed);
		state.preWorldFailures = g_dynamicResolutionPreWorldFailures.load(std::memory_order_relaxed);
		state.staleWatchdogRestores = g_dynamicResolutionStaleWatchdogRestores.load(std::memory_order_relaxed);
		state.neutralizedSourceLogical = g_dynamicResolutionNeutralizedSourceLogical.load(std::memory_order_relaxed);
		state.neutralizedDestinationLogical = g_dynamicResolutionNeutralizedDestinationLogical.load(std::memory_order_relaxed);
		state.imageSpaceDepthOverrideEnabled = g_unbindDepthStencil.load(std::memory_order_acquire);
		state.imageSpaceDepthOverrideAttempts = g_imageSpaceDepthOverrideAttempts.load(std::memory_order_relaxed);
		state.imageSpaceDepthOverrideNonNull = g_imageSpaceDepthOverrideNonNull.load(std::memory_order_relaxed);
		state.imageSpaceDepthOverrideAlreadyNull = g_imageSpaceDepthOverrideAlreadyNull.load(std::memory_order_relaxed);
		state.imageSpaceDepthOverrideDraws = g_imageSpaceDepthOverrideDraws.load(std::memory_order_relaxed);
		state.imageSpaceDepthOverrideRestores = g_imageSpaceDepthOverrideRestores.load(std::memory_order_relaxed);
		state.imageSpaceDepthOverrideFailures = g_imageSpaceDepthOverrideFailures.load(std::memory_order_relaxed);
		state.imageSpaceDepthOverrideRestoreMismatches =
			g_imageSpaceDepthOverrideRestoreMismatches.load(std::memory_order_relaxed);
		state.seamDepthRebindEnabled = g_rebindDepthStencilAfterSeam.load(std::memory_order_acquire);
		state.seamDepthRebindAttempts = g_seamDepthRebindAttempts.load(std::memory_order_relaxed);
		state.seamDepthRebinds = g_seamDepthRebinds.load(std::memory_order_relaxed);
		state.seamDepthRebindAlreadyBound = g_seamDepthRebindAlreadyBound.load(std::memory_order_relaxed);
		state.seamDepthRebindDeclines = g_seamDepthRebindDeclines.load(std::memory_order_relaxed);
		state.seamDepthRebindFailures = g_seamDepthRebindFailures.load(std::memory_order_relaxed);
		state.seamStencilClearEnabled = g_clearStencilAfterSeam.load(std::memory_order_acquire);
		state.seamStencilClearAttempts = g_seamStencilClearAttempts.load(std::memory_order_relaxed);
		state.seamStencilClears = g_seamStencilClears.load(std::memory_order_relaxed);
		state.seamStencilClearDeclines = g_seamStencilClearDeclines.load(std::memory_order_relaxed);
		// The resolve keeps its own counters beside the objects it owns; only the key's
		// live value is this file's, so the two are read at one instant here and reported
		// in one row.
		const auto resolve = DepthResolve::Snapshot();
		state.seamDepthResolveEnabled = g_resolveDepthAfterSeam.load(std::memory_order_acquire);
		state.seamDepthResolveReady = resolve.ready;
		state.seamDepthResolveAttempts = resolve.attempts;
		state.seamDepthResolves = resolve.resolves;
		state.seamDepthResolveDeclines = resolve.declines;
		state.seamDepthResolveLastDecline = resolve.lastDecline;
		state.seamDepthResolveExceptionCode = resolve.exceptionCode;
		state.seamDepthResolveLastHr = resolve.lastHr;
		state.seamDepthResolveFullWidth = resolve.fullWidth;
		state.seamDepthResolveFullHeight = resolve.fullHeight;
		state.seamDepthResolveReducedEyeWidth = resolve.reducedEyeWidth;
		state.seamDepthResolveReducedHeight = resolve.reducedHeight;
		// The composite reconstruction scale keeps its own counters beside the copy it
		// owns, exactly as the resolve above does; the key's live value and the two
		// installed-and-called facts are this file's.
		const auto composite = CompositeScale::Snapshot();
		state.compositeScaleEnabled = g_compositeReconstructionScale.load(std::memory_order_acquire);
		state.compositeScaleInstalled = g_compositeScaleInstalled;
		state.compositeScaleReady = composite.ready;
		state.compositeRecordings = g_compositeRecordings.load(std::memory_order_relaxed);
		state.compositeMemberCensus = CompositeMemberCensus();
		state.compositeScaleAttempts = composite.attempts;
		state.compositeScaleApplied = composite.applied;
		state.compositeScaleDeclines = composite.declines;
		state.compositeScaleRestores = composite.restores;
		state.compositeRestoredAtDraw = composite.restoredAtDraw;
		state.compositeDrawsExamined = composite.drawsExamined;
		state.compositeDraws = composite.compositeDraws;
		state.compositeShaders = composite.compositeShaders;
		state.compositeDrawHooksInstalled = g_drawHooksInstalled;
		state.compositeDrawsTotal = g_drawsTotal.load(std::memory_order_relaxed);
		state.compositeDrawThreadOverflow = g_drawThreadOverflow.load(std::memory_order_relaxed);
		state.compositeDrawThreadCensus = DrawThreadCensus();
		state.compositeClearColorThreadId = g_renderThreadId.load(std::memory_order_relaxed);
		state.compositeRestoredAtWater = composite.restoredAtWater;
		state.compositeRestoredAtSeamExit = composite.restoredAtSeamExit;
		state.compositeScaleLastDecline = composite.lastDecline;
		state.compositeScaleExceptionCode = composite.exceptionCode;
		state.compositeScaleLastHr = composite.lastHr;
		state.compositeScaleRatioWidth = composite.lastRatioWidth;
		state.compositeScaleRatioHeight = composite.lastRatioHeight;
		state.compositeScaleBufferBytes = composite.bufferBytes;
		return state;
	}
}
