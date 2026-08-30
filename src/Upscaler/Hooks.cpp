#include "Upscaler/Hooks.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/FaultGuard.h"

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
		constexpr std::size_t kClearColorPatchBytes = 5;
		constexpr std::size_t kAbsoluteJumpBytes = 14;

		void ObserveClearColor(std::uintptr_t a_renderer) noexcept
		{
			g_renderThreadCalls.fetch_add(1, std::memory_order_relaxed);

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
			// The first instruction is exactly five bytes. Build a gateway containing
			// that displaced instruction followed by an absolute, register-preserving
			// jump back to +5; the thunk can therefore call the complete vanilla body.
			auto& trampoline = F4SE::GetTrampoline();
			auto* const gateway = static_cast<std::uint8_t*>(
				trampoline.allocate(kClearColorPatchBytes + kAbsoluteJumpBytes));
			std::memcpy(gateway, reinterpret_cast<const void*>(a_site), kClearColorPatchBytes);

			static constexpr std::uint8_t kJumpIndirectRip[] = {
				0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
			};
			std::memcpy(gateway + kClearColorPatchBytes, kJumpIndirectRip, sizeof(kJumpIndirectRip));
			const auto resume = a_site + kClearColorPatchBytes;
			std::memcpy(
				gateway + kClearColorPatchBytes + sizeof(kJumpIndirectRip),
				&resume,
				sizeof(resume));
			static_cast<void>(::FlushInstructionCache(
				::GetCurrentProcess(), gateway, kClearColorPatchBytes + kAbsoluteJumpBytes));

			ClearColorProbeHook::func = reinterpret_cast<std::uintptr_t>(gateway);
			// write_branch's return value is meaningful only when replacing an existing
			// branch. This is a function entry, so the gateway above is the original.
			static_cast<void>(trampoline.write_branch<5>(a_site, ClearColorProbeHook::thunk));
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
		static constexpr std::uint8_t kClearColorEntry[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08  // MOV [RSP+8],RBX
		};
		static constexpr Site kSoftSiteStorage[] = {
			{
				Addr::kClearColorFn,
				kClearColorEntry,
				std::size(kClearColorEntry),
				"render-thread ClearColor probe"sv,
				"render-thread measurements and captures are unavailable"sv,
				&g_renderThreadProbeInstalled,
				&InstallClearColorProbe,
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
}
