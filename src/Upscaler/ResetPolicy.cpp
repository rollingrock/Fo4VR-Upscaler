#include "Upscaler/ResetPolicy.h"

#include <atomic>
#include <string>

namespace Upscaler::ResetPolicy
{
	namespace
	{
		// Requested on the game thread, consumed on the render thread.
		std::atomic_bool g_pending{ false };
		std::atomic_bool g_menuSinkInstalled{ false };
		std::atomic_bool g_resetOnMenu{ true };
		std::atomic_bool g_resetOnLoad{ true };
		std::atomic<std::uint64_t> g_requests{ 0 };
		std::atomic<std::uint64_t> g_coalesced{ 0 };
		std::atomic<std::uint64_t> g_consumptions{ 0 };
		std::atomic<std::uint64_t> g_loadEvents{ 0 };
		std::atomic<std::uint64_t> g_menuOpenEvents{ 0 };
		std::atomic<std::uint64_t> g_menuCloseEvents{ 0 };
		std::atomic<std::uint64_t> g_leftSubmissions{ 0 };
		std::atomic<std::uint64_t> g_rightSubmissions{ 0 };

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent& a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event.opening) {
					g_menuOpenEvents.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_menuCloseEvents.fetch_add(1, std::memory_order_relaxed);
				}
				if (g_resetOnMenu.load(std::memory_order_acquire)) {
					const char* name = a_event.menuName.c_str();
					const std::string reason = fmt::format(
						FMT_STRING("menu {}: {}"),
						a_event.opening ? "opened"sv : "closed"sv,
						name && *name ? std::string_view{ name } : "unnamed"sv);
					RequestReset(reason);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		MenuSink g_menuSink;
	}

	void Configure(bool a_resetOnMenu, bool a_resetOnLoad) noexcept
	{
		g_resetOnMenu.store(a_resetOnMenu, std::memory_order_release);
		g_resetOnLoad.store(a_resetOnLoad, std::memory_order_release);
	}

	bool InstallMenuEvents() noexcept
	{
		if (g_menuSinkInstalled.load(std::memory_order_acquire)) {
			return true;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuSink);
		g_menuSinkInstalled.store(true, std::memory_order_release);
		logger::info("history reset policy: MenuOpenCloseEvent sink installed"sv);
		return true;
	}

	void OnGameLoaded()
	{
		g_loadEvents.fetch_add(1, std::memory_order_relaxed);
		if (g_resetOnLoad.load(std::memory_order_acquire)) {
			RequestReset("game loaded"sv);
		}
	}

	void RequestReset(std::string_view a_reason)
	{
		g_requests.fetch_add(1, std::memory_order_relaxed);
		// Logged on the rising edge only. The triggers are per-frame edges, and a line
		// per call would be a line per frame the moment two of them overlap.
		if (!g_pending.exchange(true, std::memory_order_relaxed)) {
			logger::info(FMT_STRING("history reset requested: {}"), a_reason);
		} else {
			g_coalesced.fetch_add(1, std::memory_order_relaxed);
		}
	}

	bool ConsumeReset() noexcept
	{
		const bool consumed = g_pending.exchange(false, std::memory_order_acq_rel);
		if (consumed) {
			g_consumptions.fetch_add(1, std::memory_order_relaxed);
		}
		return consumed;
	}

	void RecordSubmission(std::uint32_t a_eye, bool a_reset) noexcept
	{
		if (!a_reset) {
			return;
		}
		if (a_eye == 0) {
			g_leftSubmissions.fetch_add(1, std::memory_order_relaxed);
		} else if (a_eye == 1) {
			g_rightSubmissions.fetch_add(1, std::memory_order_relaxed);
		}
	}

	State Snapshot() noexcept
	{
		return {
			g_pending.load(std::memory_order_acquire),
			g_menuSinkInstalled.load(std::memory_order_acquire),
			g_resetOnMenu.load(std::memory_order_acquire),
			g_resetOnLoad.load(std::memory_order_acquire),
			g_requests.load(std::memory_order_relaxed),
			g_coalesced.load(std::memory_order_relaxed),
			g_consumptions.load(std::memory_order_relaxed),
			g_loadEvents.load(std::memory_order_relaxed),
			g_menuOpenEvents.load(std::memory_order_relaxed),
			g_menuCloseEvents.load(std::memory_order_relaxed),
			g_leftSubmissions.load(std::memory_order_relaxed),
			g_rightSubmissions.load(std::memory_order_relaxed),
		};
	}
}
