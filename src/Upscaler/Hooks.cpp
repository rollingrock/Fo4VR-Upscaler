#include "Upscaler/Hooks.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/Debug.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Jitter.h"
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
		thread_local std::uint64_t g_activeHDRCall = 0;
		thread_local std::int32_t g_activeHDRDestination = -1;
		thread_local DynamicResolution::EffectPassthroughToken g_dynamicEffectPassthrough{};
		thread_local bool g_dynamicEffectNeutralizationPending = false;
		thread_local bool g_dynamicNeutralizationSawTaa = false;
		thread_local bool g_dynamicNeutralizationTaaSucceeded = false;
		thread_local bool g_dynamicNeutralizationSawEffect = false;
		thread_local bool g_dynamicNeutralizationSlot21Valid = false;
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
				const bool ownsTransaction = g_dynamicEffectNeutralizationPending &&
				                             a_effect == g_dynamicGeometryEffect &&
				                             g_dynamicTransactionEpoch != 0 &&
				                             g_dynamicTransactionEpoch == g_preWorldEpoch &&
				                             ScopePolicy::ClassifyCurrentDispatch() ==
													 ScopePolicy::DispatchScope::kMain;
				return ownsTransaction || DynamicResolution::NativeEffectActive();
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
			g_dynamicNeutralizationSlot21Valid = false;
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
			if (!entered || !restored) {
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
					g_dynamicGeometrySessionDisabled.store(true, std::memory_order_release);
				}

				g_worldAdmission = {};
				g_worldContractEpoch = 0;
				g_worldContractManager = 0;
				if (repaired && dispatchScope == ScopePolicy::DispatchScope::kMain &&
					DynamicResolution::WorldAdmissionGated() && QualityHandbackReady()) {
					const auto admission = ResetPolicy::AdvanceQualityWorldAdmissionBeforeWorld();
					if (admission.admitted) {
						const bool began = DynamicResolution::BeginReducedWorld(g_worldContract);
						const bool revalidated = began &&
						                         ResetPolicy::RevalidateQualityWorldAdmission(admission);
						if (revalidated) {
							g_worldAdmission = admission;
							g_worldContractEpoch = g_preWorldEpoch;
							g_worldContractManager = a_manager;
						} else if (began && !CloseReducedWorldFrame(a_manager, false)) {
							g_dynamicResolutionPreWorldFailures.fetch_add(1, std::memory_order_relaxed);
							g_dynamicGeometrySessionDisabled.store(true, std::memory_order_release);
						} else if (!began) {
							g_dynamicResolutionPreWorldFailures.fetch_add(1, std::memory_order_relaxed);
							g_dynamicGeometrySessionDisabled.store(true, std::memory_order_release);
						}
					}
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
			// BeginQualityHandback admits only the exact NGX-coupled bypass contract.
			// Do not reread live settings here: Configure may have advanced its epoch while
			// this frame was in flight, in which case staged publication itself fails closed.
			const bool taaSatisfied = g_dynamicNeutralizationSawTaa &&
			                          g_dynamicNeutralizationTaaSucceeded;
			if (!g_dynamicNeutralizationSawTaa) {
				g_dynamicResolutionMissingTaaClosures.fetch_add(1, std::memory_order_relaxed);
			}
			if (!g_dynamicNeutralizationSawEffect) {
				DisableDynamicGeometryTransaction("the accepted frame completed slots 16..22 without dispatching slot 21"sv);
			}
			const bool downstreamValid = a_allowCommit && ownsEpoch && definiteMain && taaSatisfied &&
			                             !g_qualityImageSpaceDepthOverrideFailed &&
			                             g_dynamicNeutralizationSawEffect &&
			                             g_dynamicNeutralizationSlot21Valid &&
			                             g_worldContract.active &&
			                             g_worldContractEpoch == g_preWorldEpoch &&
			                             g_worldContractManager == a_manager;
			// Return the inner unity bracket to reduced first, then restore the outer
			// native contract and rebuild native geometry. Pixel publication is last.
			const bool innerRestored = EndDynamicNeutralization(false);
			const bool worldRestored = CloseReducedWorldFrame(a_manager, true);
			const bool restored = innerRestored && worldRestored;
			if (!restored) {
				DisableDynamicGeometryTransaction("the late ImageSpace range could not close the ratio/geometry transaction"sv);
			}
			if (downstreamValid && restored) {
				if (HDRRoundTrip::ConsumeQualityFault(HDRRoundTrip::QualityFaultSeam::kPrePublish)) {
					if (!HDRRoundTrip::AbortQualityHandbackCandidate()) {
						DisableDynamicGeometryTransaction("the injected pre-publication abort could not restore all quality pixels"sv);
					}
					return;
				}
				const auto commit = HDRRoundTrip::CommitQualityHandbackCandidate();
				if (commit != HDRRoundTrip::QualityCommitResult::kPublished) {
					const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
					if (commit == HDRRoundTrip::QualityCommitResult::kWorldAdmissionVeto) {
						// Menu/cell state changed after the pre-copy recheck. This is an
						// expected fail-closed frame veto: retain the prior submitted image,
						// reset both vendor histories and allow admission to stabilize again.
						if (!pixelsRestored) {
							DisableDynamicGeometryTransaction("a final world-admission veto could not restore all quality pixels"sv);
						}
					} else {
						DisableDynamicGeometryTransaction("the late ImageSpace range closed without the matching NGX quality candidate"sv);
					}
				}
			} else {
				if (!HDRRoundTrip::AbortQualityHandbackCandidate()) {
					DisableDynamicGeometryTransaction("the rejected late ImageSpace transaction could not restore all quality pixels"sv);
				}
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
					}
					normalReturn = true;
				} __finally {
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
			g_dynamicGeometrySessionDisabled.store(true, std::memory_order_release);
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
				// commit but must never make the prepared draw fail open into real RT4.
				const bool qualityMarkerActive = g_dynamicEffectNeutralizationPending ||
				                                 g_dynamicEffectPassthrough.active ||
				                                 g_dynamicGeometryFlagArmed ||
				                                 g_dynamicTransactionEpoch != 0;
				const bool ownsQualityTransaction = qualityMarkerActive &&
				                                     g_dynamicEffectNeutralizationPending &&
				                                     g_dynamicEffectPassthrough.active &&
				                                     g_dynamicGeometryFlagArmed &&
				                                     call != 0 &&
				                                     g_dynamicExpectedTaaCall != 0 &&
				                                     call == g_dynamicExpectedTaaCall &&
				                                     g_dynamicTransactionEpoch != 0 &&
				                                     g_dynamicTransactionEpoch == g_preWorldEpoch &&
				                                     HDRRoundTrip::QualityRuntimeConfigReady();
				const bool unexpectedQualityTaa = call != 0 && qualityMarkerActive && !ownsQualityTransaction;
				// Ownership was latched before RT1 changed. A live settings reload may
				// invalidate the candidate (publication then fails closed), but it must
				// never redirect this in-flight frame into the non-staged MRT path.
				const bool deferQualityTaa = ownsQualityTransaction;
				const auto qualityConfigEpoch = ownsQualityTransaction ?
				                                    HDRRoundTrip::QualityTaaConfigEpoch() :
				                                    0;
				TAAReplacement::DrawResult bypass{};
				if (call != 0 && !unexpectedQualityTaa) {
					bypass = TAAReplacement::TryBypass(
						call, handback, deferQualityTaa, qualityConfigEpoch);
				}
				bool qualityStillActive = ownsQualityTransaction;
				bool result = true;
				if (unexpectedQualityTaa) {
					// A second main-view TAA draw inside one owned quality frame cannot be
					// allowed to mutate real history/RT4. Hold the prior submitted frame,
					// restore RT1/RT3 and close the ratio/geometry bracket fail-closed.
					const bool geometryRestored = EndDynamicNeutralization(true);
					const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
					qualityStillActive = false;
					if (!geometryRestored || !pixelsRestored) {
						DisableDynamicGeometryTransaction("an unexpected second main TemporalAA draw could not restore the quality transaction"sv);
					}
				} else if (bypass.faultInjected) {
					// The private stage draw completed, but this DevBench one-shot forces
					// the same fail-closed abort used by a later pre-publication veto. Hold
					// the prior RT4, restore RT1/RT3 and close the geometry bracket.
					const bool geometryRestored = EndDynamicNeutralization(true);
					const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
					qualityStillActive = false;
					if (!geometryRestored || !pixelsRestored) {
						DisableDynamicGeometryTransaction("the injected post-TAA-stage abort could not restore the quality transaction"sv);
					}
				} else if (ownsQualityTransaction && !bypass.drew) {
					// The private stage was not touched. Restore RT1/RT3 and the dynamic
					// bracket, but retain the prior submitted frame. RT3's backup predates
					// this frame's HDR pass, so native TAA must not consume it as though it
					// were a freshly reconstructed current frame.
					const bool geometryRestored = EndDynamicNeutralization(true);
					const bool pixelsRestored = HDRRoundTrip::AbortQualityHandbackCandidate();
					qualityStillActive = false;
					if (!geometryRestored || !pixelsRestored) {
						DisableDynamicGeometryTransaction("the staged TAA pre-draw failure could not restore the quality transaction while holding the prior submitted frame"sv);
					}
				} else if (bypass.drew) {
					result = true;
				} else {
					result = g_qualityImageSpaceDepthOverrideActive ?
					             DrawWithDepthStencilNeutralized(a_renderPass, a_arg2, a_rendererContext) :
					             func(a_renderPass, a_arg2, a_rendererContext);
				}
				TAAReplacement::DrawResult overwrite{};
				if (call != 0 && !unexpectedQualityTaa && !ownsQualityTransaction && !bypass.drew) {
					overwrite = TAAReplacement::TryOverwrite(call, handback, deferQualityTaa);
				}
				if (call != 0) {
					const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
					Debug::OnTemporalAADraw(
						Engine::Renderer(), call, thread);
					StereoCapture::OnTemporalAADraw(
						call, thread, bypass.succeeded, overwrite.succeeded, handback);
					if (qualityStillActive) {
						g_dynamicNeutralizationSawTaa = true;
						g_dynamicNeutralizationTaaSucceeded = bypass.succeeded || overwrite.succeeded;
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
						std::uint8_t geometryFlag = 0xFF;
						const bool geometryReady = a_manager == g_dynamicEffectPassthrough.manager &&
						                           g_dynamicTransactionEpoch != 0 &&
						                           g_dynamicTransactionEpoch == g_preWorldEpoch &&
						                           a_effect == g_dynamicGeometryEffect &&
						                           ReadEffectByte(g_dynamicGeometryFlagAddress, geometryFlag) &&
						                           geometryFlag == 1 &&
						                           DynamicResolution::EffectPassthroughReady();
						if (geometryReady) {
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
							g_dynamicNeutralizationSlot21Valid = true;
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
		g_dynamicNeutralizationSlot21Valid = false;
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
