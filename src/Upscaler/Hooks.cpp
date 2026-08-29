#include "Upscaler/Hooks.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/FaultGuard.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Upscaler::Hooks
{
	namespace
	{
		bool g_installed = false;
		std::size_t g_patched = 0;

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
		constexpr std::span<const Site> kSoftSites{};

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

	bool Installed() noexcept
	{
		return g_installed;
	}

	std::size_t SiteCount() noexcept
	{
		return g_patched;
	}
}
