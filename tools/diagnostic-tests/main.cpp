#include <Windows.h>
#include <d3d11.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <wrl/client.h>

#include "Upscaler/OpenVrAdmission.h"
#include "Upscaler/Telemetry.h"
#include "Upscaler/Watchdog.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace
{
	ComPtr<ID3D11Device> g_device;
	ComPtr<ID3D11DeviceContext> g_context;
	int g_lookups = 0;
	bool g_lookupFails = false;
	bool g_deviceFault = false;

	void Check(bool a_pass, const char* a_message)
	{
		if (!a_pass) {
			std::cerr << "FAIL: " << a_message << '\n';
			std::exit(1);
		}
	}

	bool Reliable(const char* a_version) { return std::strcmp(a_version, "Supported_001") == 0; }
	bool Unreliable(const char*) { return true; }
	bool NotInitialized(const char*) { return false; }
	void* Lookup(const char*, int* a_error)
	{
		++g_lookups;
		*a_error = g_lookupFails ? 109 : 0;
		return g_lookupFails ? nullptr : &g_lookups;
	}

	std::uint64_t Field(const std::string& a_json, const char* a_key)
	{
		const std::string needle = std::string("\"") + a_key + "\":";
		const auto at = a_json.find(needle);
		Check(at != std::string::npos, a_key);
		return std::stoull(a_json.substr(at + needle.size()));
	}

	void Marks(unsigned a_last)
	{
		for (unsigned i = 1; i <= a_last; ++i) {
			Upscaler::Telemetry::Mark(static_cast<Upscaler::Telemetry::MarkPoint>(i));
		}
	}

	void Drain()
	{
		// This standalone producer has no Present. Submit its work explicitly; the
		// production collector still uses DONOTFLUSH and never waits for a sample.
		g_context->Flush();
		for (unsigned i = 0; i < 1000; ++i) {
			Upscaler::Telemetry::Collect();
			if (Field(Upscaler::Telemetry::Report(), "pending") == 0) {
				return;
			}
			::Sleep(1);
		}
		Check(false, "query drain timeout");
	}
}

namespace
{
	std::atomic_bool g_testLoading{ false };
	bool TestLoading() noexcept { return g_testLoading.load(); }
}

namespace Upscaler::Engine
{
	ID3D11Device* Device() noexcept
	{
		if (g_deviceFault) {
			::RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
		}
		return g_device.Get();
	}
	ID3D11DeviceContext* ImmediateContext() noexcept { return g_context.Get(); }
}

int main()
{
	using namespace Upscaler;
	using OpenVrAdmission::Query;
	using OpenVrAdmission::Result;
	Result result{};
	int error = 0;
	Check(!Query(static_cast<decltype(&Reliable)>(nullptr), &Lookup, "Supported_001", &error, result) && result == Result::kMissingExports, "missing validator");
	Check(!Query(&Reliable, static_cast<decltype(&Lookup)>(nullptr), "Supported_001", &error, result) && result == Result::kMissingExports, "missing lookup");
	Check(!Query(&Unreliable, &Lookup, "Supported_001", &error, result) && result == Result::kUnreliableValidation, "unconditional true validator");
	Check(!Query(&Reliable, &Lookup, "Unsupported_027", &error, result) && result == Result::kUnsupported, "unsupported version");
	Check(!Query(&NotInitialized, &Lookup, "Supported_001", &error, result) && result == Result::kUnsupported, "not initialized");
	Check(g_lookups == 0, "refused paths never call generic lookup");
	Check(Query(&Reliable, &Lookup, "Supported_001", &error, result) && result == Result::kAvailable && g_lookups == 1, "supported runtime");
	g_lookupFails = true;
	Check(!Query(&Reliable, &Lookup, "Supported_001", &error, result) && result == Result::kLookupFailed && error == 109, "lookup decline after validation");

	Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			  nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &g_context)),
		"WARP device");
	Telemetry::Begin(Telemetry::Path::kNative);
	Check(Field(Telemetry::Report(), "begun") == 0, "disabled default");
	Telemetry::Configure(true);
	Telemetry::Begin(Telemetry::Path::kNative);
	Marks(17);
	Telemetry::ExpectNativeSeam(42);
	Check(!Telemetry::NativePending(41) && Telemetry::NativePending(42), "serial binding");
	Telemetry::EndNative(41);
	Check(Field(Telemetry::Report(), "committed") == 0, "wrong serial cannot commit");
	Telemetry::Mark(Telemetry::MarkPoint::kBeforeSeamPublishIsolationBegin);
	Telemetry::Mark(Telemetry::MarkPoint::kAfterSeamPublishIsolationBegin);
	Telemetry::Mark(Telemetry::MarkPoint::kBeforeSeamPublishIsolationEnd);
	Telemetry::EndNative(42);
	Drain();
	const auto native = Telemetry::Report();
	Check(Field(native, "committed") == 1 && Field(native, "completed") == 1, "native CPU and GPU episode");
	Check(native.find("nativeDlaa") != std::string::npos && native.find("nativeSeamClose") != std::string::npos && native.find("engineSeamPublishToLateRange") == std::string::npos, "native boundary naming");
	Check(native.find("\"evaluatePair\":{\"samples\":1,") != std::string::npos && native.find("\"p50Ms\":") != std::string::npos, "one evaluate pair per completed episode");
	std::cout << "native=" << native << '\n';

	Telemetry::Begin(Telemetry::Path::kNative);
	Marks(17);
	Telemetry::ExpectNativeSeam(43);
	Telemetry::AbortNative();
	Check(!Telemetry::NativePending(43) && Field(Telemetry::Report(), "aborted") == 1, "missed seam aborted");
	Telemetry::Begin(Telemetry::Path::kNative);
	Telemetry::Mark(Telemetry::MarkPoint::kAfterLeftEvaluate);
	Telemetry::ExpectNativeSeam(44);
	Telemetry::EndNative(44);
	Check(Field(Telemetry::Report(), "committed") == 1 && Field(Telemetry::Report(), "aborted") == 2, "incomplete marks rejected");
	Drain();

	// Leave unread native work outstanding across the path change.
	Telemetry::Begin(Telemetry::Path::kNative);
	Marks(20);
	Telemetry::ExpectNativeSeam(45);
	Telemetry::EndNative(45);
	Telemetry::Begin();
	Telemetry::AbortNative();
	Marks(22);
	Telemetry::End();
	Drain();
	const auto reduced = Telemetry::Report();
	Check(Field(reduced, "committed") == 1 && Field(reduced, "completed") == 1, "path windows do not mix pending native samples");
	Check(reduced.find("reducedNgx") != std::string::npos && reduced.find("engineSeamPublishToLateRange") != std::string::npos, "reduced boundary unchanged");
	std::cout << "reduced=" << reduced << '\n';

	Telemetry::Begin();
	Marks(3);
	Telemetry::Configure(false);
	Telemetry::Collect();
	Check(Telemetry::Report().find("\"episodeActive\":false") != std::string::npos, "disable retires active query");
	Telemetry::Configure(true);
	Telemetry::Begin(Telemetry::Path::kNative);
	Marks(20);
	Telemetry::ExpectNativeSeam(46);
	Telemetry::EndNative(46);
	const auto reset = Telemetry::RequestReset();
	Drain();
	Check(Field(Telemetry::Report(), "resetCompletedGeneration") == reset && Field(Telemetry::Report(), "committed") == 0, "reset completes after pending drain");
	Check(Telemetry::Report().find("\"evaluatePair\":{\"samples\":0,") != std::string::npos, "a window reset clears the evaluate pair");

	// perfTimersNativeTaa. Off: a timed draw issues nothing and the report carries no block.
	// On: one Begin/End pair lands exactly one sample once its queries resolve, and a window
	// reset clears it.
	const auto nativeTaaCount = [] {
		const auto report = Telemetry::Report();
		const std::string needle = "\"nativeTaaDraw\":{\"count\":";
		const auto at = report.find(needle);
		return at == std::string::npos ? std::uint64_t{ 999 } : std::stoull(report.substr(at + needle.size()));
	};
	Check(Telemetry::Report().find("\"nativeTaaDraw\"") == std::string::npos, "no native TAA block while it is off");
	Telemetry::BeginNativeTaaDraw();
	Telemetry::EndNativeTaaDraw();
	Telemetry::ConfigureNativeTaa(true);
	Check(nativeTaaCount() == 0, "the native TAA block is present and empty once on");
	Telemetry::BeginNativeTaaDraw();
	Telemetry::EndNativeTaaDraw();
	g_context->Flush();
	for (unsigned i = 0; i < 1000 && nativeTaaCount() == 0; ++i) {
		Telemetry::Collect();
		::Sleep(1);
	}
	Check(nativeTaaCount() == 1, "one timed native TAA draw is one sample");
	Check(Telemetry::Report().find("\"droppedNoSlot\":0}") != std::string::npos, "no native TAA draw dropped");
	const auto taaReset = Telemetry::RequestReset();
	Drain();
	Check(Field(Telemetry::Report(), "resetCompletedGeneration") == taaReset && nativeTaaCount() == 0, "a window reset clears the native TAA samples");
	Telemetry::ConfigureNativeTaa(false);
	Check(Telemetry::Report().find("\"nativeTaaDraw\"") == std::string::npos, "the block goes again when switched off");
	std::thread other([] { Telemetry::Begin(); });
	other.join();
	Check(Field(Telemetry::Report(), "threadMismatches") == 1, "wrong thread rejected");

	std::ostringstream logged;
	auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(logged);
	spdlog::set_default_logger(std::make_shared<spdlog::logger>("test", sink));
	Telemetry::LogReport();
	::Sleep(10020);
	Telemetry::LogReport();
	Check(logged.str().find("perf report:") != std::string::npos, "automatic log export");
	const auto length = logged.str().size();
	Telemetry::LogReport();
	Check(logged.str().size() == length, "log rate limit");

	g_deviceFault = true;
	Telemetry::Begin(Telemetry::Path::kNative);
	g_deviceFault = false;
	Telemetry::Collect();
	const auto fault = Telemetry::Report();
	Check(Field(fault, "instrumentationFaults") == 1 && fault.find("\"instrumentationFaulted\":true") != std::string::npos && fault.find("\"episodeActive\":false") != std::string::npos, "instrumentation SEH retires queries");

	// renderWatchdogSeconds. A "render" thread beats, then blocks on an event the way a hung
	// one would; the watchdog must write one report naming that thread with a walked stack,
	// and a resume line once the thread beats again. Reports go to a temporary file.
	{
		wchar_t tempDir[MAX_PATH]{};
		::GetTempPathW(MAX_PATH, tempDir);
		const std::wstring reportFile = std::wstring(tempDir) + L"fo4vr_upscaler_watchdog_test.log";
		::DeleteFileW(reportFile.c_str());
		Watchdog::RedirectReportsForTest(reportFile.c_str());
		Watchdog::RememberMainThread();
		Check(Watchdog::Reports() == 0, "no watchdog report before it starts");
		HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		std::atomic_bool stop{ false };
		std::thread render([&] {
			for (int i = 0; i < 20; ++i) {
				Watchdog::Beat();
				::Sleep(10);
			}
			::WaitForSingleObject(release, INFINITE);
			while (!stop.load()) {
				Watchdog::Beat();
				::Sleep(10);
			}
		});
		g_testLoading.store(true);
		Watchdog::SetLoadingProbe(&TestLoading);
		Watchdog::Configure(0.5);
		::Sleep(1500);
		Check(Watchdog::Reports() == 0 && Watchdog::SuppressedStalls() >= 1, "a stall under a load is held back");
		g_testLoading.store(false);
		for (int i = 0; i < 400 && Watchdog::Reports() == 0; ++i) {
			::Sleep(10);
		}
		Check(Watchdog::Reports() == 1, "one watchdog report for one stall once the load is gone");
		::SetEvent(release);
		::Sleep(600);
		stop.store(true);
		render.join();
		Watchdog::Configure(0.0);
		std::ifstream in(reportFile);
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();
		std::cout << text;
		Check(text.find("heartbeat stopped") != std::string::npos, "the report says the heartbeat stopped");
		Check(text.find("render thread") != std::string::npos && text.find("suspended yes, context yes") != std::string::npos, "the render thread was suspended and read");
		Check(text.find("#01 ") != std::string::npos, "the stack walk found more than one frame");
		Check(text.find("ntdll.dll+") != std::string::npos || text.find("KERNELBASE.dll+") != std::string::npos, "a blocked thread's stack names the wait it is in");
		Check(text.find("heartbeat resumed") != std::string::npos, "a resumed heartbeat is reported");
		::CloseHandle(release);
		::DeleteFileW(reportFile.c_str());
	}
	std::cout << "PASS: OpenVR admission and production telemetry lifecycle (WARP)\n";
}
