// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Streamline probe. It answers measurement M11 and nothing else:
//
//   Can Streamline initialise from a plugin that loads AFTER the D3D11 device and the
//   swap chain already exist?
//
// That is the one question that decides whether this project can use Streamline at all,
// because an F4SEVR plugin always loads late. NVIDIA's own manual-hooking guide says
// slInit must be called "before any of the hooks mentioned in section 2.0 could be
// triggered (like for example, DXGI calls to create swap-chain)", and the mandatory hook
// list begins with IDXGIFactory::CreateSwapChain. Our load order violates that on its
// face. The same guide then says, in a note, that "unlike regular SL integrations, the
// D3D device can be created before or after slInit is called". Those two statements do
// not settle each other, which is exactly why this is an experiment and not a reading
// exercise.
//
// It runs with no game, no headset, no hook and no mod installed - the same reason
// tools/subrect-harness exists. It creates its own device and its own hidden-window swap
// chain, so the failure modes are ours to read rather than the engine's to hide.
//
// Three cases, each in its own PROCESS. Streamline keeps global state across
// slInit/slShutdown, and a case that poisons that state would silently decide the next
// one. The default run spawns itself once per case.
//
//   1  init first, then device and swap chain    the documented order - the control arm
//   2  device and swap chain first, then init    THE F4SEVR CASE - this is the question
//   3  as case 2 but without eUseManualHooking   what the naive integration does
//
// A case reports the exact sl::Result at every step, then what the runtime itself says
// about DLSS: whether FeatureRequirementFlags::eD3D11Supported is set, and how many
// viewports the feature will carry - which is the other thing this project needs to know,
// since two eyes want two independent histories.
//
// Nothing here ships. The runtime it loads is staged into the git-ignored .sdk-cache by
// scripts/fetch-sdks.ps1 from NVIDIA's own release archive, pinned by SHA-256.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include "sl.h"
#include "sl_consts.h"
#include "sl_helpers.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
	// ---- reporting -------------------------------------------------------------------

	int g_failures = 0;

	void Say(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		std::vprintf(a_fmt, args);
		va_end(args);
		std::printf("\n");
		std::fflush(stdout);
	}

	void Step(const char* a_what, sl::Result a_result)
	{
		const bool ok = a_result == sl::Result::eOk;
		if (!ok) {
			++g_failures;
		}
		Say("  [%s] %-34s %s", ok ? " ok " : "FAIL", a_what, sl::getResultAsStr(a_result));
	}

	// ---- the interposer, loaded dynamically ------------------------------------------
	//
	// Dynamic rather than linking sl.interposer.lib, because that is the shape a mod is
	// forced into: the game already links dxgi.lib and d3d11.lib, and an injected plugin
	// cannot retroactively replace the process's import table. If Streamline only works
	// when it owns those imports, this project cannot use it, and the probe should find
	// that out the same way the plugin would.

	struct Interposer
	{
		HMODULE module = nullptr;

		PFun_slInit* init = nullptr;
		PFun_slShutdown* shutdown = nullptr;
		PFun_slSetD3DDevice* setD3DDevice = nullptr;
		PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
		PFun_slIsFeatureLoaded* isFeatureLoaded = nullptr;
		PFun_slGetFeatureRequirements* getFeatureRequirements = nullptr;
		PFun_slGetFeatureVersion* getFeatureVersion = nullptr;
		PFun_slUpgradeInterface* upgradeInterface = nullptr;

		[[nodiscard]] bool Ok() const noexcept
		{
			return module && init && shutdown && setD3DDevice && isFeatureSupported &&
			       getFeatureRequirements;
		}
	};

	template <class T>
	void Resolve(HMODULE a_module, const char* a_name, T*& a_out)
	{
		a_out = reinterpret_cast<T*>(::GetProcAddress(a_module, a_name));
		if (!a_out) {
			Say("  [FAIL] %s is not exported by sl.interposer.dll", a_name);
			++g_failures;
		}
	}

	bool LoadInterposer(const std::filesystem::path& a_dir, Interposer& a_out)
	{
		const auto dll = a_dir / "sl.interposer.dll";
		if (!std::filesystem::exists(dll)) {
			Say("  [SKIP] %s is not staged. Run scripts/fetch-sdks.ps1 first.", dll.string().c_str());
			return false;
		}

		// The plugin DLLs sit beside the interposer, and Streamline finds them through
		// Preferences::pathsToPlugins below. AddDllDirectory is still worth doing so the
		// interposer's own dependencies resolve from the same place.
		::SetDllDirectoryW(a_dir.c_str());

		a_out.module = ::LoadLibraryW(dll.c_str());
		if (!a_out.module) {
			Say("  [FAIL] LoadLibrary failed on sl.interposer.dll, GetLastError=%lu", ::GetLastError());
			++g_failures;
			return false;
		}

		Resolve(a_out.module, "slInit", a_out.init);
		Resolve(a_out.module, "slShutdown", a_out.shutdown);
		Resolve(a_out.module, "slSetD3DDevice", a_out.setD3DDevice);
		Resolve(a_out.module, "slIsFeatureSupported", a_out.isFeatureSupported);
		Resolve(a_out.module, "slIsFeatureLoaded", a_out.isFeatureLoaded);
		Resolve(a_out.module, "slGetFeatureRequirements", a_out.getFeatureRequirements);
		Resolve(a_out.module, "slGetFeatureVersion", a_out.getFeatureVersion);
		Resolve(a_out.module, "slUpgradeInterface", a_out.upgradeInterface);
		return a_out.Ok();
	}

	// ---- a device and a swap chain, with a hidden window ------------------------------

	struct Gfx
	{
		HWND window = nullptr;
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		IDXGISwapChain* swapChain = nullptr;

		void Release()
		{
			if (swapChain) {
				swapChain->Release();
				swapChain = nullptr;
			}
			if (context) {
				context->Release();
				context = nullptr;
			}
			if (device) {
				device->Release();
				device = nullptr;
			}
			if (window) {
				::DestroyWindow(window);
				window = nullptr;
			}
		}
	};

	LRESULT CALLBACK ProbeWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_w, LPARAM a_l)
	{
		return ::DefWindowProcW(a_hwnd, a_msg, a_w, a_l);
	}

	bool CreateGfx(Gfx& a_out)
	{
		static const wchar_t* kClass = L"Fo4VRUpscalerStreamlineProbe";

		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = ProbeWndProc;
		wc.hInstance = ::GetModuleHandleW(nullptr);
		wc.lpszClassName = kClass;
		::RegisterClassExW(&wc);  // a duplicate registration is harmless here

		a_out.window = ::CreateWindowExW(
			0, kClass, L"streamline probe", WS_OVERLAPPEDWINDOW,
			0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
		if (!a_out.window) {
			Say("  [FAIL] CreateWindowEx failed, GetLastError=%lu", ::GetLastError());
			++g_failures;
			return false;
		}

		DXGI_SWAP_CHAIN_DESC desc{};
		desc.BufferCount = 2;
		desc.BufferDesc.Width = 320;
		desc.BufferDesc.Height = 240;
		desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.OutputWindow = a_out.window;
		desc.SampleDesc.Count = 1;
		desc.Windowed = TRUE;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

		const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL got{};

		// Deliberately D3D11CreateDeviceAndSwapChain, the same call the guide warns
		// "will result in that device being automatically assigned to SL". Case 2 needs
		// that to have happened before slInit, because that is the game's situation.
		const HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			wanted, static_cast<UINT>(std::size(wanted)), D3D11_SDK_VERSION,
			&desc, &a_out.swapChain, &a_out.device, &got, &a_out.context);

		if (FAILED(hr)) {
			Say("  [FAIL] D3D11CreateDeviceAndSwapChain hr=0x%08lX", static_cast<unsigned long>(hr));
			++g_failures;
			return false;
		}

		Say("  [ ok ] device and swap chain created, feature level 0x%X", static_cast<unsigned>(got));
		return true;
	}

	// ---- what the runtime itself says about DLSS --------------------------------------

	void ReportDlss(const Interposer& a_sl)
	{
		sl::FeatureRequirements req{};
		const auto reqResult = a_sl.getFeatureRequirements(sl::kFeatureDLSS, req);
		Step("slGetFeatureRequirements(DLSS)", reqResult);

		if (reqResult == sl::Result::eOk) {
			const auto has = [&](sl::FeatureRequirementFlags a_flag) {
				return (static_cast<uint32_t>(req.flags) & static_cast<uint32_t>(a_flag)) != 0;
			};
			// This is the answer the whole project has been reading documentation for.
			// The runtime is the authority, not the docs and not a plugin manifest.
			Say("         D3D11 supported by sl.dlss : %s", has(sl::FeatureRequirementFlags::eD3D11Supported) ? "YES" : "no");
			Say("         D3D12 supported            : %s", has(sl::FeatureRequirementFlags::eD3D12Supported) ? "yes" : "no");
			Say("         Vulkan supported           : %s", has(sl::FeatureRequirementFlags::eVulkanSupported) ? "yes" : "no");
			Say("         v-sync must be off         : %s", has(sl::FeatureRequirementFlags::eVSyncOffRequired) ? "yes" : "no");
			// Two eyes want two independent reconstruction histories. One viewport would
			// force the per-eye-intermediate path and rule out a two-viewport design.
			Say("         maxNumViewports            : %u", req.maxNumViewports);
			Say("         required buffer tags       : %u", req.numRequiredTags);
			Say("         driver detected/required   : %u.%u.%u / %u.%u.%u",
				req.driverVersionDetected.major, req.driverVersionDetected.minor, req.driverVersionDetected.build,
				req.driverVersionRequired.major, req.driverVersionRequired.minor, req.driverVersionRequired.build);
		}

		if (a_sl.getFeatureVersion) {
			sl::FeatureVersion ver{};
			if (a_sl.getFeatureVersion(sl::kFeatureDLSS, ver) == sl::Result::eOk) {
				Say("         sl.dlss version            : %u.%u.%u",
					ver.versionSL.major, ver.versionSL.minor, ver.versionSL.build);
			}
		}

		if (a_sl.isFeatureLoaded) {
			bool loaded = false;
			if (a_sl.isFeatureLoaded(sl::kFeatureDLSS, loaded) == sl::Result::eOk) {
				Say("         sl.dlss loaded             : %s", loaded ? "yes" : "no");
			}
		}
	}

	// ---- the cases --------------------------------------------------------------------

	// Streamline explains itself in its own log and nowhere else. A probe that reports
	// only the sl::Result reports the symptom; this reports the reason, which is the
	// difference between "DLSS is not supported" and "this console app is not what NGX
	// expects". Errors and warnings only - verbose SL logging is thousands of lines.
	bool g_showSlLog = false;

	void SlLog(sl::LogType a_type, const char* a_msg)
	{
		if (!a_msg) {
			return;
		}
		const bool interesting = a_type == sl::LogType::eError || a_type == sl::LogType::eWarn;
		if (!interesting && !g_showSlLog) {
			return;
		}
		std::string line = a_msg;
		while (!line.empty() && (line.back() == 0x0A || line.back() == 0x0D)) {
			line.pop_back();
		}
		if (line.empty()) {
			return;
		}
		Say("         sl[%s] %s", a_type == sl::LogType::eError ? "error" : a_type == sl::LogType::eWarn ? "warn " : "info ", line.c_str());
	}

	sl::Result CallInit(const Interposer& a_sl, const std::filesystem::path& a_dir, bool a_manualHooking)
	{
		static std::wstring pluginPath;
		pluginPath = a_dir.wstring();
		const wchar_t* paths[] = { pluginPath.c_str() };

		static const sl::Feature features[] = { sl::kFeatureDLSS };

		sl::Preferences pref{};
		pref.showConsole = false;
		pref.logLevel = sl::LogLevel::eVerbose;
		pref.pathsToPlugins = paths;
		pref.numPathsToPlugins = 1;
		pref.pathToLogsAndData = nullptr;  // stdout only; a log file would hide the answer
		pref.logMessageCallback = &SlLog;
		// Documented Optional, set anyway: NGX gates on application identity, and an
		// unidentified console app is a plausible reason for a per-adapter refusal that
		// the general check passes.
		pref.projectId = "b7f4e0c2-9d13-4a5e-8c77-2f6a1e3d9b04";
		pref.featuresToLoad = features;
		pref.numFeaturesToLoad = 1;
		pref.engine = sl::EngineType::eCustom;
		pref.engineVersion = "fo4vr-upscaler-probe";
		// The default in the header is eD3D12. Saying D3D11 out loud matters: the header
		// notes that slGetFeatureRequirements behaviour depends on this being right.
		pref.renderAPI = sl::RenderAPI::eD3D11;

		// eDisableCLStateTracking is on by default and stays on - on D3D11 the host is
		// responsible for restoring context state after an evaluate either way, and this
		// project would rather do that itself than have a runtime do it invisibly.
		if (a_manualHooking) {
			pref.flags |= sl::PreferenceFlags::eUseManualHooking;
		}

		return a_sl.init(pref, sl::kSDKVersion);
	}

	// The general check the guide shows first: a null adapter asks whether the feature
	// is possible at all on this OS and driver, before any adapter is chosen. Running it
	// as well as the per-adapter one is what separates "this GPU cannot" from "this
	// configuration cannot", and those have very different consequences for us.
	void GeneralSupport(const Interposer& a_sl)
	{
		sl::AdapterInfo none{};
		Step("slIsFeatureSupported(DLSS), no adapter", a_sl.isFeatureSupported(sl::kFeatureDLSS, none));
	}

	int RunCase(int a_case, const std::filesystem::path& a_dir)
	{
		Interposer sl{};
		if (!LoadInterposer(a_dir, sl)) {
			Say("  case %d could not run: the interposer did not load or is missing exports", a_case);
			return 2;
		}

		Gfx gfx{};
		const bool manualHooking = a_case != 3;

		if (a_case == 1) {
			// The documented order. If this fails, the problem is the probe or the
			// machine, not the load order - which is the whole point of a control arm.
			Step("slInit, before any device", CallInit(sl, a_dir, manualHooking));
			GeneralSupport(sl);
			if (!CreateGfx(gfx)) {
				sl.shutdown();
				return 2;
			}
		} else {
			// The F4SEVR situation: the device and the swap chain already exist, and
			// IDXGIFactory::CreateSwapChain - the first mandatory hook in the manual
			// hooking list - has already been and gone.
			if (!CreateGfx(gfx)) {
				return 2;
			}
			Step(a_case == 2 ? "slInit, AFTER swap chain (manual)" : "slInit, AFTER swap chain (auto)",
				CallInit(sl, a_dir, manualHooking));
			GeneralSupport(sl);
		}

		Step("slSetD3DDevice", sl.setD3DDevice(gfx.device));

		// slIsFeatureSupported wants a real adapter LUID. Passing an empty AdapterInfo
		// answers eErrorFeatureNotSupported for every feature on every machine, which
		// reads exactly like a real negative - the first run of this probe reported one
		// in all three cases before the LUID was filled in. Pull it off the device that
		// was actually created.
		LUID luid{};
		bool haveLuid = false;
		{
			IDXGIDevice* dxgiDevice = nullptr;
			if (SUCCEEDED(gfx.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice) {
				IDXGIAdapter* dxgiAdapter = nullptr;
				if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) && dxgiAdapter) {
					DXGI_ADAPTER_DESC adapterDesc{};
					if (SUCCEEDED(dxgiAdapter->GetDesc(&adapterDesc))) {
						luid = adapterDesc.AdapterLuid;
						haveLuid = true;
						char name[128]{};
						::WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
						Say("  [ ok ] adapter %s", name);
					}
					dxgiAdapter->Release();
				}
				dxgiDevice->Release();
			}
		}
		if (!haveLuid) {
			Say("  [FAIL] could not read the adapter LUID - the support query below is meaningless");
			++g_failures;
		}

		sl::AdapterInfo adapter{};
		adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
		adapter.deviceLUIDSizeInBytes = sizeof(luid);
		Step("slIsFeatureSupported(DLSS)", sl.isFeatureSupported(sl::kFeatureDLSS, adapter));

		ReportDlss(sl);

		// Whether an already-created swap chain can be upgraded in place is the escape
		// hatch the guide gestures at for integrations that do not create their own. If
		// this succeeds, a mod could route the engine's Present through the proxy; if it
		// does not, the manual-hooking path needs the hooks called by hand instead.
		if (sl.upgradeInterface && gfx.swapChain) {
			void* iface = gfx.swapChain;
			Step("slUpgradeInterface(existing swapchain)", sl.upgradeInterface(&iface));
			Say("         proxy differs from native : %s", iface != gfx.swapChain ? "YES" : "no");
		}

		Step("slShutdown", sl.shutdown());
		gfx.Release();
		return g_failures == 0 ? 0 : 1;
	}

	const char* CaseName(int a_case)
	{
		switch (a_case) {
		case 1:
			return "CONTROL - slInit first, then device and swap chain (the documented order)";
		case 2:
			return "THE QUESTION - device and swap chain first, then slInit with manual hooking";
		case 3:
			return "device and swap chain first, then slInit WITHOUT manual hooking";
		default:
			return "unknown";
		}
	}
}

int wmain(int argc, wchar_t** argv)
{
	std::filesystem::path dir = L".sdk-cache/streamline";
	int only = 0;

	for (int i = 1; i < argc; ++i) {
		const std::wstring arg = argv[i];
		if (arg == L"--case" && i + 1 < argc) {
			only = ::_wtoi(argv[++i]);
		} else if (arg == L"--dir" && i + 1 < argc) {
			dir = argv[++i];
		}
	}

	dir = std::filesystem::absolute(dir);

	if (only != 0) {
		Say("");
		Say("case %d: %s", only, CaseName(only));
		return RunCase(only, dir);
	}

	// Parent: one child per case, so Streamline's global state cannot leak between them.
	Say("streamline probe - measurement M11");
	Say("runtime directory: %s", dir.string().c_str());
	Say("");
	Say("Each case runs in its own process. Streamline keeps global state across");
	Say("slInit/slShutdown, and a poisoned case would silently decide the next one.");

	wchar_t self[MAX_PATH]{};
	if (::GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
		Say("could not find my own path; run with --case 1|2|3 instead");
		return 2;
	}

	int worst = 0;
	int results[4] = { 0, 0, 0, 0 };
	for (int c = 1; c <= 3; ++c) {
		std::wstring cmd = L"\"";
		cmd += self;
		cmd += L"\" --case ";
		cmd += std::to_wstring(c);
		cmd += L" --dir \"";
		cmd += dir.wstring();
		cmd += L"\"";

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};

		std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
		buffer.push_back(L'\0');

		if (!::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
			Say("case %d: CreateProcess failed, GetLastError=%lu", c, ::GetLastError());
			results[c] = 2;
			worst = 2;
			continue;
		}
		::WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 0;
		::GetExitCodeProcess(pi.hProcess, &code);
		::CloseHandle(pi.hProcess);
		::CloseHandle(pi.hThread);

		results[c] = static_cast<int>(code);
		if (results[c] > worst) {
			worst = results[c];
		}
	}

	Say("");
	Say("summary");
	for (int c = 1; c <= 3; ++c) {
		const char* verdict = results[c] == 0 ? "all steps ok" :
		                      results[c] == 1 ? "ran, at least one step failed" :
		                                        "could not run";
		Say("  case %d: %-32s %s", c, verdict, CaseName(c));
	}
	Say("");
	Say("Read case 2. If case 1 succeeds and case 2 does not, Streamline cannot be");
	Say("initialised from an F4SEVR plugin and raw NGX D3D11 is the answer. If both");
	Say("succeed, the load-order requirement in the manual-hooking guide is not");
	Say("enforced at init, and the remaining question is whether presentCommon is");
	Say("reached every frame - which needs the engine, not this probe.");
	return worst;
}
