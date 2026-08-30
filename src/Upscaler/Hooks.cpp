#include "Upscaler/Hooks.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Debug.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"
#include "Upscaler/HDRRoundTrip.h"
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
		thread_local std::uint64_t g_activeHDRCall = 0;
		thread_local std::int32_t g_activeHDRDestination = -1;

		// The hook-struct shape, so the first real hook has nothing to invent:
		//
		//   // Replaces "call FUN_14xxxxxxx(state, x, y)" inside the per-frame camera
		//   // commit - the one caller whose sub-pixel offset we own.
		//   struct JitterCommitHook
		//   {
		//       static void thunk(std::uintptr_t a_state, float a_x, float a_y)
		//       {
		//           func(a_state, a_x, a_y);
		//       }
		//       static inline REL::Relocation<decltype(&thunk)> func;
		//   };
		//
		//   void InstallJitterCommit(std::uintptr_t a_site)
		//   {
		//       pstl::write_thunk_call<JitterCommitHook>(a_site);
		//   }
		//
		// A call-site hook keeps func, because the original still runs. An entry-replace
		// hook - write_branch<5> over a function that is exactly five bytes - has no func
		// at all, because the original is never called again.
		//
		// Hook the call site when only some callers should change. That is the usual
		// case, and hooking the callee instead would capture callers whose behaviour has
		// to stay vanilla. Hook the function entry only when every caller including
		// engine job threads must be intercepted, and only when the function is small
		// enough to branch over safely.

		// A call-site patch is five bytes. The cap is what keeps the verifier's read
		// buffer bounded and POD.
		constexpr std::size_t kMaxSiteBytes = 16;
		constexpr std::size_t kEntryPatchBytes = 5;
		constexpr std::size_t kAbsoluteJumpBytes = 14;

		template <class Hook>
		void InstallFiveByteEntryProbe(std::uintptr_t a_site)
		{
			// The first instruction is exactly five bytes and has no relative operand.
			// Preserve it in a gateway, then use an indirect RIP jump back to +5 so no
			// volatile register is consumed and the thunk can call the whole vanilla body.
			auto& trampoline = F4SE::GetTrampoline();
			auto* const gateway = static_cast<std::uint8_t*>(
				trampoline.allocate(kEntryPatchBytes + kAbsoluteJumpBytes));
			std::memcpy(gateway, reinterpret_cast<const void*>(a_site), kEntryPatchBytes);

			static constexpr std::uint8_t kJumpIndirectRip[] = {
				0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
			};
			std::memcpy(gateway + kEntryPatchBytes, kJumpIndirectRip, sizeof(kJumpIndirectRip));
			const auto resume = a_site + kEntryPatchBytes;
			std::memcpy(
				gateway + kEntryPatchBytes + sizeof(kJumpIndirectRip),
				&resume,
				sizeof(resume));
			static_cast<void>(::FlushInstructionCache(
				::GetCurrentProcess(), gateway, kEntryPatchBytes + kAbsoluteJumpBytes));

			Hook::func = reinterpret_cast<std::uintptr_t>(gateway);
			// write_branch's return value is meaningful only when replacing an existing
			// branch. This is a function entry, so the gateway above is the original.
			static_cast<void>(trampoline.write_branch<5>(a_site, Hook::thunk));
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

		struct TemporalAAProbeHook
		{
			static void thunk(
				std::uintptr_t a_effect,
				std::uintptr_t a_source,
				std::uintptr_t a_arg3,
				std::uintptr_t a_arg4)
			{
				const auto call = ObserveTemporalAA(a_effect);
				g_activeTemporalAACall = call;
				func(a_effect, a_source, a_arg3, a_arg4);
				g_activeTemporalAACall = 0;
			}

			static inline REL::Relocation<decltype(&thunk)> func;
		};

		void InstallTemporalAAProbe(std::uintptr_t a_site)
		{
			InstallFiveByteEntryProbe<TemporalAAProbeHook>(a_site);
		}

		struct ImageSpacePixelDrawProbeHook
		{
			static bool thunk(
				std::uintptr_t a_renderPass,
				std::uint8_t a_arg2,
				std::uintptr_t a_rendererContext)
			{
				const auto call = g_activeTemporalAACall;
				const bool bypassed = call != 0 && TAAReplacement::TryBypass(
					call, HDRRoundTrip::ContinuousCopySerial());
				const bool result = bypassed ? true : func(a_renderPass, a_arg2, a_rendererContext);
				if (call != 0 && !bypassed) {
					static_cast<void>(TAAReplacement::TryOverwrite(
						call, HDRRoundTrip::ContinuousCopySerial()));
				}
				if (call != 0) {
					Debug::OnTemporalAADraw(
						Engine::Renderer(), call, static_cast<std::uint32_t>(::GetCurrentThreadId()));
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
			auto effect = g_hdrEffect.load(std::memory_order_relaxed);
			if (effect != 0) {
				return effect;
			}

			std::uintptr_t effects = 0;
			if (a_manager == 0 || !FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(a_manager + 0x18), &effects, sizeof(effects)) ||
				effects == 0 || !FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(effects + 3 * sizeof(std::uintptr_t)), &effect, sizeof(effect)) ||
				effect == 0) {
				return 0;
			}

			std::uintptr_t expected = 0;
			static_cast<void>(g_hdrEffect.compare_exchange_strong(
				expected, effect, std::memory_order_relaxed, std::memory_order_relaxed));
			return g_hdrEffect.load(std::memory_order_relaxed);
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
				if (a_effect != 0 && a_effect == ResolveHDREffect(a_manager)) {
					const auto call = ObserveHDRInput(
						a_effect,
						static_cast<std::int32_t>(a_sourceLogical),
						static_cast<std::int32_t>(a_destinationLogical));
					g_activeHDRCall = call;
					g_activeHDRDestination = static_cast<std::int32_t>(a_destinationLogical);
					HDRRoundTrip::OnDispatch(
						static_cast<std::int32_t>(a_sourceLogical),
						static_cast<std::int32_t>(a_destinationLogical));
					func(
						a_manager,
						a_renderParams,
						a_effect,
						a_sourceLogical,
						a_destinationLogical,
						a_arg6);
					g_activeHDRDestination = -1;
					g_activeHDRCall = 0;
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
		};
		constexpr std::span<const Site> kSoftSites{ kSoftSiteStorage };

		// Tier 3. Setting-gated raw byte patches, verified the same way.
		constexpr std::span<const Site> kPatchSites{};
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
}
