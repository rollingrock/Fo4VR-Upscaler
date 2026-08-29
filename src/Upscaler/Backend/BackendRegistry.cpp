#include "Upscaler/Backend/BackendRegistry.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <iterator>
#include <span>
#include <string_view>

#include "Upscaler/Backend/FidelityFxD3D11.h"
#include "Upscaler/Backend/NgxD3D11.h"
#include "Upscaler/Backend/NullBackend.h"
#include "Upscaler/Backend/Streamline.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

namespace Upscaler::Backend
{
	namespace
	{
		constexpr std::string_view kAuto = "auto";
		constexpr std::string_view kProbeOrder = "ngx, streamline, ffx, null";

		// Written once by Select on the load thread and read later from the render
		// path, so it is atomic rather than a plain pointer.
		std::atomic<IBackend*> g_current{ nullptr };

		void LogCaps(const IBackend& a_backend, const Caps& a_caps)
		{
			if (!a_caps.available) {
				// Verbatim, and with a visible marker when a backend broke its own
				// contract by leaving the reason empty.
				const std::string_view reason = Text(a_caps.unavailableReason);
				spdlog::info("backend {}: unavailable - {}", a_backend.Name(), reason.empty() ? std::string_view{ "no reason given" } : reason);
				return;
			}

			const std::string_view version = Text(a_caps.runtimeVersion);
			const std::string_view path = Text(a_caps.runtimePath);

			if (path.empty()) {
				spdlog::info("backend {}: available, runtime version {}", a_backend.Name(), version.empty() ? std::string_view{ "unreported" } : version);
			} else {
				spdlog::info("backend {}: available, runtime version {} loaded from {}", a_backend.Name(), version.empty() ? std::string_view{ "unreported" } : version, path);
			}
		}
	}

	std::span<IBackend* const> All()
	{
		// Function-local so there is no static-initialisation-order question and the
		// logger exists by the time any of these can run.
		static NgxD3D11Backend s_ngx;
		static StreamlineBackend s_streamline;
		static FidelityFxD3D11Backend s_ffx;
		static NullBackend s_null;

		static IBackend* const s_all[] = { &s_ngx, &s_streamline, &s_ffx, &s_null };
		return { s_all, std::size(s_all) };
	}

	IBackend* Select(std::string_view a_requested, ID3D11Device* a_device)
	{
		const std::span<IBackend* const> all = All();
		const bool explicitRequest = !a_requested.empty() && a_requested != kAuto;

		IBackend* firstAvailable = nullptr;
		IBackend* requested = nullptr;
		bool requestedAvailable = false;

		spdlog::info("probing backends in order {} with {}", kProbeOrder, a_device ? "a device" : "no device yet, so availability only");

		for (IBackend* backend : all) {
			const Caps caps = backend->Probe(a_device);
			LogCaps(*backend, caps);

			if (caps.available && !firstAvailable) {
				firstAvailable = backend;
			}

			if (explicitRequest && backend->Name() == a_requested) {
				requested = backend;
				requestedAvailable = caps.available;
			}
		}

		IBackend* chosen = nullptr;

		if (explicitRequest) {
			if (!requested) {
				spdlog::warn("no backend is named {}; falling back to automatic selection", a_requested);
			} else if (!requestedAvailable) {
				spdlog::warn("backend {} was asked for by name but reports itself unavailable; falling back to automatic selection", a_requested);
			} else {
				chosen = requested;
				spdlog::info("selected backend {} because it was asked for by name", chosen->Name());
			}
		}

		if (!chosen) {
			chosen = firstAvailable;
			if (chosen) {
				spdlog::info("selected backend {}: first available in probe order {}", chosen->Name(), kProbeOrder);
			}
		}

		if (!chosen) {
			// Unreachable: the null backend is always available. Say so out loud rather
			// than return null and make every caller test for it.
			chosen = all.back();
			spdlog::critical("no backend reported itself available, not even {}; using it anyway", chosen->Name());
		}

		g_current.store(chosen, std::memory_order_release);
		return chosen;
	}

	IBackend* Current() noexcept
	{
		return g_current.load(std::memory_order_acquire);
	}
}
