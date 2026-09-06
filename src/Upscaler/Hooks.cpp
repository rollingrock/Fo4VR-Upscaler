#include "Upscaler/Hooks.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/Debug.h"
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
					__try {
						HDRRoundTrip::OnDispatch(
							a_manager,
							static_cast<std::int32_t>(a_sourceLogical),
							static_cast<std::int32_t>(a_destinationLogical));
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
		};
		constexpr std::span<const Site> kSoftSites{ kSoftSiteStorage };

		// Tier 3. Setting-gated raw byte patches, verified the same way.
		constexpr std::span<const Site> kPatchSites{};
	}

	void Configure(bool a_unbindDepthStencil) noexcept
	{
		g_unbindDepthStencil.store(a_unbindDepthStencil, std::memory_order_release);
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
		return state;
	}
}
