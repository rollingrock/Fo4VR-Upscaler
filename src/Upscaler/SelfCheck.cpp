#include "Upscaler/SelfCheck.h"

#include "Upscaler/Backend/BackendRegistry.h"
#include "Upscaler/Backend/IBackend.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/StereoLayout.h"

#include <dxgi.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Upscaler::SelfCheck
{
	namespace
	{
		// Duplicated from Settings.h and from the shipped TOML's runtimeDir default,
		// deliberately: the self-check must be able to report the environment even when
		// the settings load failed, so it carries no settings dependency.
		constexpr std::string_view kSettingsPath = "Data/F4SE/Plugins/Fo4VRUpscaler.toml"sv;
		constexpr std::string_view kRuntimeDir = "Data/F4SE/Plugins/Fo4VRUpscaler"sv;

		// One spelling for every fact this build cannot answer, so a reader can grep a
		// single string and get the whole to-do list.
		[[nodiscard]] std::string Blocked(std::string_view a_measurement)
		{
			return fmt::format(FMT_STRING("not yet implemented - blocked on {}"), a_measurement);
		}

		[[nodiscard]] std::string Narrow(const wchar_t* a_wide)
		{
			if (!a_wide || !*a_wide) {
				return {};
			}
			const int n = ::WideCharToMultiByte(CP_UTF8, 0, a_wide, -1, nullptr, 0, nullptr, nullptr);
			if (n <= 1) {
				return {};
			}
			std::string out(static_cast<std::size_t>(n - 1), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_wide, -1, out.data(), n, nullptr, nullptr);
			return out;
		}

		[[nodiscard]] std::string JsonEscape(std::string_view a_in)
		{
			std::string out;
			out.reserve(a_in.size() + 8);
			for (const char c : a_in) {
				switch (c) {
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				default:
					if (static_cast<unsigned char>(c) < 0x20) {
						out += fmt::format(FMT_STRING("\\u{:04x}"), static_cast<unsigned>(static_cast<unsigned char>(c)));
					} else {
						out += c;
					}
					break;
				}
			}
			return out;
		}

		[[nodiscard]] std::string Quote(std::string_view a_in)
		{
			return fmt::format(FMT_STRING("\"{}\""), JsonEscape(a_in));
		}

		[[nodiscard]] std::string JsonArray(const std::vector<std::string>& a_lines)
		{
			std::string out = "[";
			for (std::size_t i = 0; i < a_lines.size(); ++i) {
				if (i != 0) {
					out += ',';
				}
				out += Quote(a_lines[i]);
			}
			out += ']';
			return out;
		}

		// Modules whose presence changes what a bug report means. Presence is stated
		// either way: "absent" is an answer, and a report that only lists what was found
		// leaves the reader unable to tell a clean environment from an unchecked one.
		struct ModuleProbe
		{
			const wchar_t* dll;
			std::string_view note;
		};

		constexpr ModuleProbe kProbes[] = {
			{ L"devbench.dll", "dev tooling host - optional, and never a load-order requirement"sv },
			{ L"truescopes_vr.dll", "another render-path mod on the same deferred resolve"sv },
			{ L"Fallout4Upscaler.dll", "another upscaler mod - both cannot own the render path"sv },
			{ L"vrperfkit.dll", "compositor-level upscaler"sv },
			{ L"nvngx_dlss.dll", "NVIDIA reconstruction runtime, loaded by something already"sv },
			{ L"sl.interposer.dll", "Streamline interposer, loaded by something already"sv },
			{ L"libxess.dll", "Intel reconstruction runtime, loaded by something already"sv },
		};

		// These four are always present in a running D3D11 VR game, so the interesting
		// fact is not presence but which file won: a copy beside the executable is a
		// wrapper (an injector, an overlay, a runtime translation layer) sitting between
		// the game and the real system DLL.
		constexpr const wchar_t* kPathProbes[] = {
			L"d3d11.dll",
			L"dxgi.dll",
			L"openvr_api.dll",
			L"openxr_loader.dll",
		};

		struct BackendFact
		{
			std::string name;
			bool available = false;
			bool supportsSubrects = false;
			bool needsDeviceCreation = false;
			std::string runtimeVersion;
			std::string runtimePath;
			std::string unavailableReason;
		};

		struct Facts
		{
			std::string plugin;
			std::string version;
			std::string built;
			std::string gameVersion;
			std::uintptr_t moduleBase = 0;
			std::string moduleFile;
			bool isVr = false;

			std::string logPath;
			std::string logLevel;

			std::string settingsPath;
			bool settingsPresent = false;
			std::string runtimeDir;
			bool runtimeDirPresent = false;

			std::vector<std::string> adapters;
			std::string adaptersBlocked;
			std::vector<std::string> modules;
			std::vector<std::string> wrappers;
			std::string vrRuntime;

			bool deviceFound = false;
			std::string featureLevel;
			std::string stereoLayout;
			bool stereoResolved = false;

			bool hooksInstalled = false;
			std::size_t hookSites = 0;

			std::vector<BackendFact> backends;
			std::string selection;
		};

		[[nodiscard]] std::string ModulePath(HMODULE a_handle)
		{
			if (!a_handle) {
				return {};
			}
			wchar_t buf[MAX_PATH] = {};
			if (::GetModuleFileNameW(a_handle, buf, static_cast<DWORD>(MAX_PATH)) == 0) {
				return {};
			}
			buf[MAX_PATH - 1] = L'\0';
			return Narrow(buf);
		}

		// Which runtime the headset is actually going through. The file name is not the
		// answer on its own - a translation layer ships under the same name as the
		// runtime it replaces - so the resolved path is what gets printed.
		[[nodiscard]] std::string VrRuntime()
		{
			const auto openvr = ::GetModuleHandleW(L"openvr_api.dll");
			const auto openxr = ::GetModuleHandleW(L"openxr_loader.dll");
			if (!openvr && !openxr) {
				return "no vr runtime module is loaded yet"s;
			}
			std::string out;
			if (openvr) {
				const auto path = ModulePath(openvr);
				out += fmt::format(FMT_STRING("openvr_api.dll from {}"), path.empty() ? "(path unavailable)"s : path);
			}
			if (openxr) {
				if (!out.empty()) {
					out += "; ";
				}
				const auto path = ModulePath(openxr);
				out += fmt::format(FMT_STRING("openxr_loader.dll from {}"), path.empty() ? "(path unavailable)"s : path);
			}
			return out;
		}

		[[nodiscard]] std::string LogPath()
		{
			const auto log = spdlog::default_logger();
			if (!log) {
				return {};
			}
			for (const auto& sink : log->sinks()) {
				if (const auto* file = dynamic_cast<const spdlog::sinks::basic_file_sink_mt*>(sink.get())) {
					return file->filename();
				}
			}
			return {};
		}

		[[nodiscard]] std::string LogLevel()
		{
			const auto log = spdlog::default_logger();
			if (!log) {
				return "(no logger)"s;
			}
			const auto name = spdlog::level::to_string_view(log->level());
			return { name.data(), name.size() };
		}

		[[nodiscard]] bool PathExists(std::string_view a_path)
		{
			std::error_code ec;
			return std::filesystem::exists(std::filesystem::path{ a_path }, ec);
		}

		// The adapter walk. dxgi.dll is already loaded in any D3D11 process, so it is
		// reached with GetModuleHandleW and never LoadLibraryW - this build loads
		// nothing the game did not load for itself.
		void CollectAdapters(Facts& a_facts)
		{
			using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);

			const auto mod = ::GetModuleHandleW(L"dxgi.dll");
			if (!mod) {
				a_facts.adaptersBlocked = "dxgi.dll is not loaded in this process"s;
				return;
			}
			const auto create = reinterpret_cast<CreateFactory_t>(::GetProcAddress(mod, "CreateDXGIFactory1"));
			if (!create) {
				a_facts.adaptersBlocked = "dxgi.dll exports no CreateDXGIFactory1"s;
				return;
			}

			IDXGIFactory1* factory = nullptr;
			const auto hr = create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
			if (FAILED(hr) || !factory) {
				a_facts.adaptersBlocked = fmt::format(FMT_STRING("CreateDXGIFactory1 failed hr=0x{:08X}"), static_cast<std::uint32_t>(hr));
				SafeRelease(factory);
				return;
			}

			for (UINT i = 0; i < 8; ++i) {
				IDXGIAdapter1* adapter = nullptr;
				if (factory->EnumAdapters1(i, &adapter) != S_OK || !adapter) {
					SafeRelease(adapter);
					break;
				}
				DXGI_ADAPTER_DESC1 desc{};
				if (SUCCEEDED(adapter->GetDesc1(&desc))) {
					// The user-mode display driver version, not the vendor's marketing
					// number - it is the one a driver-side regression is bisected on.
					LARGE_INTEGER umd{};
					const bool haveUmd = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
					const auto driver = haveUmd ?
					                        fmt::format(FMT_STRING("{}.{}.{}.{}"),
												HIWORD(umd.HighPart), LOWORD(umd.HighPart),
												HIWORD(umd.LowPart), LOWORD(umd.LowPart)) :
					                        "(not reported)"s;
					a_facts.adapters.push_back(fmt::format(
						FMT_STRING("{} - vendor 0x{:04X} device 0x{:04X}, {} MB dedicated, umd driver {}{}"),
						Narrow(desc.Description), desc.VendorId, desc.DeviceId,
						static_cast<std::uint64_t>(desc.DedicatedVideoMemory) / (1024u * 1024u),
						driver,
						(desc.Flags & static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0 ? ", software adapter"sv : ""sv));
				}
				SafeRelease(adapter);
			}
			SafeRelease(factory);

			if (a_facts.adapters.empty() && a_facts.adaptersBlocked.empty()) {
				a_facts.adaptersBlocked = "no adapters enumerated"s;
			}
		}

		void CollectModules(Facts& a_facts)
		{
			for (const auto& probe : kProbes) {
				const auto handle = ::GetModuleHandleW(probe.dll);
				a_facts.modules.push_back(fmt::format(
					FMT_STRING("{}: {} ({})"),
					Narrow(probe.dll), handle ? "present"sv : "absent"sv, probe.note));
			}

			for (const auto* dll : kPathProbes) {
				const auto handle = ::GetModuleHandleW(dll);
				if (!handle) {
					a_facts.wrappers.push_back(fmt::format(FMT_STRING("{}: not loaded"), Narrow(dll)));
					continue;
				}
				const auto path = ModulePath(handle);
				a_facts.wrappers.push_back(fmt::format(
					FMT_STRING("{}: {}"),
					Narrow(dll), path.empty() ? "(path unavailable)"s : path));
			}
		}

		void CollectBackends(Facts& a_facts)
		{
			for (auto* const backend : Backend::All()) {
				if (!backend) {
					continue;
				}
				// Probe with a null device: it answers "is the runtime present at all",
				// which is the only question that can be asked before a device exists.
				const auto caps = backend->Probe(nullptr);
				BackendFact fact{};
				fact.name = std::string{ backend->Name() };
				fact.available = caps.available;
				fact.supportsSubrects = caps.supportsSubrects;
				fact.needsDeviceCreation = caps.needsDeviceCreation;
				// Caps text fields are fixed buffers; Backend::Text bounds the read.
				fact.runtimeVersion = std::string{ Backend::Text(caps.runtimeVersion) };
				fact.runtimePath = std::string{ Backend::Text(caps.runtimePath) };
				fact.unavailableReason = std::string{ Backend::Text(caps.unavailableReason) };
				a_facts.backends.push_back(std::move(fact));
			}
		}

		[[nodiscard]] Facts Collect()
		{
			Facts f{};

			f.plugin = std::string{ Version::PROJECT };
			f.version = std::string{ Version::NAME };
			f.built = __DATE__ " " __TIME__;

			const auto& gameModule = REL::Module::get();
			f.gameVersion = gameModule.version().string();
			f.moduleBase = gameModule.base();
			f.moduleFile = Narrow(gameModule.filename().data());
			f.isVr = REL::Module::IsVR();

			f.logPath = LogPath();
			f.logLevel = LogLevel();

			f.settingsPath = std::string{ kSettingsPath };
			f.settingsPresent = PathExists(kSettingsPath);
			f.runtimeDir = std::string{ kRuntimeDir };
			f.runtimeDirPresent = PathExists(kRuntimeDir);

			CollectAdapters(f);
			CollectModules(f);
			CollectBackends(f);
			f.vrRuntime = VrRuntime();

			f.deviceFound = Engine::Device() != nullptr;
			f.featureLevel = f.deviceFound ? "reported by the device"s : Blocked("the immediate-context RVA that Engine::Init needs"sv);
			f.stereoResolved = StereoLayout::Resolved();
			f.stereoLayout = f.stereoResolved ?
			                     "resolved"s :
			                     Blocked("one frame capture at the engine's temporal pass, reading the bound colour, depth and motion-vector dimensions - which settles the render resolution and the eye order together"sv);

			f.hooksInstalled = Hooks::Installed();
			f.hookSites = Hooks::SiteCount();

			// Read the registry's verdict back rather than restating it - Select has
			// already run on the availability pass. No device exists at plugin load, so
			// nothing is initialised and the choice can still change once there is an
			// adapter to probe against.
			const auto* const current = Backend::Current();
			f.selection = current ?
			                  std::string{ current->Name() } + " - availability only, not initialised; re-selected once a device is bound" :
			                  "none selected - the registry returned nothing, which cannot happen while the null passthrough is compiled in"s;

			return f;
		}
	}

	void Run()
	{
		const auto f = Collect();

		logger::info(FMT_STRING("self-check: {} {} built {}"), f.plugin, f.version, f.built);
		logger::info(FMT_STRING("self-check: game {} at base {:016X} ({}), vr binary {}"),
			f.gameVersion, f.moduleBase, f.moduleFile, f.isVr ? "yes"sv : "no"sv);
		logger::info(FMT_STRING("self-check: log {} at level {}"),
			f.logPath.empty() ? "(no file sink)"s : f.logPath, f.logLevel);
		logger::info(FMT_STRING("self-check: settings {} - {}"),
			f.settingsPath, f.settingsPresent ? "present"sv : "not found, defaults in use"sv);
		logger::info(FMT_STRING("self-check: runtime staging {} - {}"),
			f.runtimeDir,
			f.runtimeDirPresent ? "present"sv : "absent, which is expected on this build - no backend needs a runtime yet"sv);

		if (f.adapters.empty()) {
			logger::info(FMT_STRING("self-check: gpu adapters unavailable - {}"), f.adaptersBlocked);
		} else {
			for (const auto& line : f.adapters) {
				logger::info(FMT_STRING("self-check: gpu {}"), line);
			}
		}

		logger::info(FMT_STRING("self-check: vr runtime {}"), f.vrRuntime);
		for (const auto& line : f.wrappers) {
			logger::info(FMT_STRING("self-check: module path {}"), line);
		}
		for (const auto& line : f.modules) {
			logger::info(FMT_STRING("self-check: module {}"), line);
		}

		logger::info(FMT_STRING("self-check: d3d11 device {}"), f.deviceFound ? "found"sv : "not found"sv);
		logger::info(FMT_STRING("self-check: d3d11 feature level {}"), f.featureLevel);
		logger::info(FMT_STRING("self-check: stereo layout {}"), f.stereoLayout);

		logger::info(FMT_STRING("self-check: hooks {} ({} sites)"),
			f.hooksInstalled ? "installed"sv : "not installed"sv, f.hookSites);

		for (const auto& b : f.backends) {
			if (b.available) {
				logger::info(FMT_STRING("self-check: backend {} available - runtime {} at {}"),
					b.name,
					b.runtimeVersion.empty() ? "(version not reported)"s : b.runtimeVersion,
					b.runtimePath.empty() ? "(path not reported)"s : b.runtimePath);
			} else {
				// Printed verbatim. The self-check reports what the runtime said, never
				// a paraphrase of it.
				logger::info(FMT_STRING("self-check: backend {} unavailable - {}"),
					b.name,
					b.unavailableReason.empty() ? "(no reason given)"s : b.unavailableReason);
			}
		}
		logger::info(FMT_STRING("self-check: backend selection {}"), f.selection);

		logger::info("self-check: stubbed on this build - Engine, RenderTargets, StereoLayout, Jitter, MipBias, Intermediates, Evaluate, Telemetry, Debug. Real - Hooks, FaultGuard, ResetPolicy, SelfCheck."sv);

		// Anyone who installs this and sees no visual change must find the reason here
		// rather than conclude the mod is broken.
		logger::info("self-check: this build installs no hooks, creates no d3d11 resources and changes no pixels. The log is the deliverable; docs/MODULE_MAP.md lists the measurement each module is waiting on."sv);
	}

	std::string ReportJson()
	{
		const auto f = Collect();

		std::string backends = "[";
		for (std::size_t i = 0; i < f.backends.size(); ++i) {
			const auto& b = f.backends[i];
			if (i != 0) {
				backends += ',';
			}
			backends += fmt::format(
				FMT_STRING(R"({{"name":{},"available":{},"supportsSubrects":{},"needsDeviceCreation":{},"runtimeVersion":{},"runtimePath":{},"unavailableReason":{}}})"),
				Quote(b.name),
				b.available ? "true"sv : "false"sv,
				b.supportsSubrects ? "true"sv : "false"sv,
				b.needsDeviceCreation ? "true"sv : "false"sv,
				Quote(b.runtimeVersion),
				Quote(b.runtimePath),
				Quote(b.unavailableReason));
		}
		backends += ']';

		// Assembled in pieces rather than one format string: the object is long enough
		// that a single template makes a misplaced argument invisible.
		std::string out = "{";
		out += fmt::format(FMT_STRING(R"("plugin":{},"version":{},"built":{},)"),
			Quote(f.plugin), Quote(f.version), Quote(f.built));
		out += fmt::format(FMT_STRING(R"("gameVersion":{},"moduleBase":"{:016X}","moduleFile":{},"vr":{},)"),
			Quote(f.gameVersion), f.moduleBase, Quote(f.moduleFile), f.isVr ? "true"sv : "false"sv);
		out += fmt::format(FMT_STRING(R"("logPath":{},"logLevel":{},)"),
			Quote(f.logPath), Quote(f.logLevel));
		out += fmt::format(FMT_STRING(R"("settingsPath":{},"settingsPresent":{},"runtimeDir":{},"runtimeDirPresent":{},)"),
			Quote(f.settingsPath), f.settingsPresent ? "true"sv : "false"sv,
			Quote(f.runtimeDir), f.runtimeDirPresent ? "true"sv : "false"sv);
		out += fmt::format(FMT_STRING(R"("adapters":{},"adaptersBlocked":{},"moduleProbes":{},"modulePaths":{},"vrRuntime":{},)"),
			JsonArray(f.adapters), Quote(f.adaptersBlocked), JsonArray(f.modules), JsonArray(f.wrappers), Quote(f.vrRuntime));
		out += fmt::format(FMT_STRING(R"("deviceFound":{},"featureLevel":{},"stereoResolved":{},"stereoLayout":{},)"),
			f.deviceFound ? "true"sv : "false"sv, Quote(f.featureLevel),
			f.stereoResolved ? "true"sv : "false"sv, Quote(f.stereoLayout));
		out += fmt::format(FMT_STRING(R"("hooksInstalled":{},"hookSites":{},"backendSelection":{},"backends":{})"),
			f.hooksInstalled ? "true"sv : "false"sv, f.hookSites, Quote(f.selection), backends);
		out += '}';
		return out;
	}
}
