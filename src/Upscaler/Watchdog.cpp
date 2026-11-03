#include "Upscaler/Watchdog.h"

#include "Settings/UserPaths.h"

#include <Windows.h>
#include <psapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace Upscaler::Watchdog
{
	namespace
	{
		constexpr std::size_t kMaxFrames = 48;

		std::atomic<std::uint32_t> g_stallMilliseconds{ 0 };
		std::atomic<std::uint64_t> g_beats{ 0 };
		std::atomic<std::uint32_t> g_renderThreadId{ 0 };
		std::atomic<std::uint32_t> g_mainThreadId{ 0 };
		std::atomic<std::uint64_t> g_reports{ 0 };
		std::atomic_flag g_started = ATOMIC_FLAG_INIT;
		// Set once by a test before the watchdog starts; read only by the watchdog thread.
		std::wstring g_testReportFile;

		struct Walk
		{
			std::array<std::uint64_t, kMaxFrames> frames{};
			std::size_t count = 0;
			bool suspended = false;
			bool context = false;
			std::uint32_t exceptionCode = 0;
		};

		// POD only: the unwind runs under SEH so a torn stack cannot take the watchdog down.
		void UnwindSuspended(CONTEXT& a_context, Walk& a_out) noexcept
		{
			__try {
				for (std::size_t i = 0; i < kMaxFrames && a_context.Rip != 0; ++i) {
					a_out.frames[a_out.count++] = a_context.Rip;
					DWORD64 imageBase = 0;
					PRUNTIME_FUNCTION function = ::RtlLookupFunctionEntry(a_context.Rip, &imageBase, nullptr);
					if (!function) {
						// A leaf: the return address is at the stack pointer.
						if (a_context.Rsp == 0) {
							break;
						}
						a_context.Rip = *reinterpret_cast<const DWORD64*>(a_context.Rsp);
						a_context.Rsp += 8;
						continue;
					}
					PVOID handlerData = nullptr;
					DWORD64 establisherFrame = 0;
					::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, a_context.Rip, function, &a_context, &handlerData, &establisherFrame, nullptr);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_out.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
		}

		[[nodiscard]] Walk WalkThread(std::uint32_t a_threadId) noexcept
		{
			Walk walk{};
			if (a_threadId == 0 || a_threadId == ::GetCurrentThreadId()) {
				return walk;
			}
			HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, a_threadId);
			if (!thread) {
				return walk;
			}
			if (::SuspendThread(thread) != static_cast<DWORD>(-1)) {
				walk.suspended = true;
				CONTEXT context{};
				context.ContextFlags = CONTEXT_FULL;
				if (::GetThreadContext(thread, &context)) {
					walk.context = true;
					UnwindSuspended(context, walk);
				}
				::ResumeThread(thread);
			}
			::CloseHandle(thread);
			return walk;
		}

		struct Module
		{
			std::uint64_t base = 0;
			std::uint64_t size = 0;
			std::string name;
		};

		// After the threads are running again. EnumProcessModules reads the loader's list
		// without taking the loader lock, so a thread stuck holding it cannot stop this.
		[[nodiscard]] std::string Describe(std::uint64_t a_address, const std::array<HMODULE, 512>& a_modules, DWORD a_count) noexcept
		{
			const HANDLE process = ::GetCurrentProcess();
			for (DWORD i = 0; i < a_count; ++i) {
				MODULEINFO info{};
				if (!::GetModuleInformation(process, a_modules[i], &info, sizeof(info))) {
					continue;
				}
				const auto base = reinterpret_cast<std::uint64_t>(info.lpBaseOfDll);
				if (a_address >= base && a_address < base + info.SizeOfImage) {
					char name[MAX_PATH]{};
					::GetModuleBaseNameA(process, a_modules[i], name, MAX_PATH);
					char text[MAX_PATH + 64]{};
					std::snprintf(text, sizeof(text), "%s+0x%llx", name, static_cast<unsigned long long>(a_address - base));
					return text;
				}
			}
			char text[64]{};
			std::snprintf(text, sizeof(text), "0x%016llx (no module)", static_cast<unsigned long long>(a_address));
			return text;
		}

		void Append(const std::string& a_text) noexcept
		{
			try {
				const auto path = !g_testReportFile.empty() ? std::filesystem::path{ g_testReportFile } : Settings::Paths::F4SEUserDirectory() / L"Fo4VRUpscaler-watchdog.log";
				HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file == INVALID_HANDLE_VALUE) {
					return;
				}
				DWORD written = 0;
				::WriteFile(file, a_text.data(), static_cast<DWORD>(a_text.size()), &written, nullptr);
				::FlushFileBuffers(file);
				::CloseHandle(file);
			} catch (...) {
			}
		}

		[[nodiscard]] std::string Stamp()
		{
			SYSTEMTIME now{};
			::GetLocalTime(&now);
			char text[32]{};
			std::snprintf(text, sizeof(text), "[%04u-%02u-%02u %02u:%02u:%02u.%03u]", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
			return text;
		}

		void Report(std::uint64_t a_beats, double a_stalledSeconds) noexcept
		{
			try {
				const auto renderId = g_renderThreadId.load(std::memory_order_relaxed);
				const auto mainId = g_mainThreadId.load(std::memory_order_relaxed);
				// Both walks first, back to back, while nothing else of ours runs.
				const Walk render = WalkThread(renderId);
				const Walk main = mainId != renderId ? WalkThread(mainId) : Walk{};
				std::array<HMODULE, 512> modules{};
				DWORD needed = 0;
				::EnumProcessModules(::GetCurrentProcess(), modules.data(), static_cast<DWORD>(sizeof(modules)), &needed);
				const DWORD count = (std::min)(static_cast<DWORD>(needed / sizeof(HMODULE)), static_cast<DWORD>(modules.size()));
				std::string out = Stamp();
				char head[256]{};
				std::snprintf(head, sizeof(head), " the render thread's heartbeat stopped %.1f s ago, after %llu frames\n", a_stalledSeconds, static_cast<unsigned long long>(a_beats));
				out += head;
				const auto write = [&](const char* a_label, std::uint32_t a_id, const Walk& a_walk) {
					char line[256]{};
					std::snprintf(line, sizeof(line), "  %s thread %u: suspended %s, context %s, %zu frames%s\n", a_label, a_id,
						a_walk.suspended ? "yes" : "no", a_walk.context ? "yes" : "no", a_walk.count, a_walk.exceptionCode ? " (unwind stopped on an exception)" : "");
					out += line;
					for (std::size_t i = 0; i < a_walk.count; ++i) {
						char frame[32]{};
						std::snprintf(frame, sizeof(frame), "    #%02zu ", i);
						out += frame;
						out += Describe(a_walk.frames[i], modules, count);
						out += "\n";
					}
				};
				write("render", renderId, render);
				if (mainId != renderId) {
					write("main", mainId, main);
				}
				Append(out);
				g_reports.fetch_add(1, std::memory_order_relaxed);
			} catch (...) {
			}
		}

		void Run() noexcept
		{
			using Clock = std::chrono::steady_clock;
			std::uint64_t lastBeats = g_beats.load(std::memory_order_relaxed);
			auto lastChange = Clock::now();
			bool reported = false;
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				const auto stallMs = g_stallMilliseconds.load(std::memory_order_relaxed);
				const auto beats = g_beats.load(std::memory_order_relaxed);
				const auto now = Clock::now();
				if (stallMs == 0 || beats != lastBeats) {
					if (reported && beats != lastBeats) {
						const double out = std::chrono::duration<double>(now - lastChange).count();
						char line[160]{};
						std::snprintf(line, sizeof(line), " the heartbeat resumed after %.1f s\n", out);
						Append(Stamp() + line);
					}
					lastBeats = beats;
					lastChange = now;
					reported = false;
					continue;
				}
				const double stalled = std::chrono::duration<double>(now - lastChange).count();
				if (!reported && beats != 0 && stalled * 1000.0 >= static_cast<double>(stallMs)) {
					Report(beats, stalled);
					reported = true;
				}
			}
		}
	}

	void RememberMainThread() noexcept
	{
		g_mainThreadId.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
	}

	void Configure(double a_seconds) noexcept
	{
		const bool on = a_seconds > 0.0 && a_seconds < 3600.0;
		g_stallMilliseconds.store(on ? static_cast<std::uint32_t>(a_seconds * 1000.0) : 0u, std::memory_order_relaxed);
		if (on && !g_started.test_and_set()) {
			try {
				std::thread(Run).detach();
			} catch (...) {
			}
		}
	}

	void Beat() noexcept
	{
		if (g_stallMilliseconds.load(std::memory_order_relaxed) == 0) {
			return;
		}
		g_renderThreadId.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
		g_beats.fetch_add(1, std::memory_order_relaxed);
	}

	std::uint64_t Reports() noexcept
	{
		return g_reports.load(std::memory_order_relaxed);
	}

	void RedirectReportsForTest(const wchar_t* a_file) noexcept
	{
		try {
			g_testReportFile = a_file ? a_file : L"";
		} catch (...) {
		}
	}
}
