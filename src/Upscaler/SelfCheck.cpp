#include "Upscaler/SelfCheck.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Backend/BackendRegistry.h"
#include "Upscaler/Backend/IBackend.h"
#include "Upscaler/CameraMotion.h"
#include "Upscaler/Common.h"
#include "Upscaler/Debug.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Evaluate.h"
#include "Upscaler/FaultGuard.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/Intermediates.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/MipBias.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/TAAReplacement.h"

#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <filesystem>
#include <iterator>
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

		[[nodiscard]] std::string JsonFloat(float a_value)
		{
			return std::isfinite(a_value) ? fmt::format(FMT_STRING("{}"), a_value) : "null"s;
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

		[[nodiscard]] std::string ObservedTargetJson(const Debug::ObservedTarget& a_target)
		{
			return fmt::format(
				FMT_STRING(R"({{"logical":{},"phys":{},"width":{},"height":{},"dxgiFormat":{},"valid":{}}})"),
				a_target.logical,
				a_target.phys,
				a_target.width,
				a_target.height,
				a_target.format,
				a_target.valid ? "true"sv : "false"sv);
		}

		template <std::size_t N>
		[[nodiscard]] std::string BoundSlotsJson(
			const std::array<Debug::BindingObservation::BoundSlot, N>& a_slots)
		{
			std::string out = "[";
			bool first = true;
			for (std::size_t i = 0; i < a_slots.size(); ++i) {
				const auto& binding = a_slots[i];
				if (!binding.bound) {
					continue;
				}
				if (!first) {
					out += ',';
				}
				first = false;
				out += fmt::format(
					FMT_STRING(R"({{"slot":{},"colorLogical":{},"depthLogical":{}}})"),
					i, binding.colorLogical, binding.depthLogical);
			}
			out += ']';
			return out;
		}

		[[nodiscard]] std::string_view DepthViewName(Debug::DepthViewBinding a_binding) noexcept
		{
			switch (a_binding) {
			case Debug::DepthViewBinding::none:
				return "none"sv;
			case Debug::DepthViewBinding::writable:
				return "writable"sv;
			case Debug::DepthViewBinding::readOnlyDepth:
				return "readOnlyDepth"sv;
			case Debug::DepthViewBinding::readOnlyStencil:
				return "readOnlyStencil"sv;
			case Debug::DepthViewBinding::readOnlyBoth:
				return "readOnlyBoth"sv;
			case Debug::DepthViewBinding::unmatched:
				return "unmatched"sv;
			}
			return "invalid"sv;
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
			{ L"libxess_dx11.dll", "Intel D3D11 reconstruction runtime, loaded by something already"sv },
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

			bool engineResolved = false;
			std::string engineHandles;
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
				// Listener-thread reports request only immutable build/pre-device fields;
				// live runtime and adapter health comes from render-thread telemetry.
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

			f.engineResolved = Engine::Resolved();
			f.engineHandles = Engine::HandleSummary();
			auto* const device = Engine::Device();
			f.deviceFound = device != nullptr;
			// M10, answered. The device comes from an engine global rather than from
			// ctx->GetDevice, so no COM call and no render-thread callback is involved.
			f.featureLevel = f.deviceFound ?
			                     fmt::format(FMT_STRING("0x{:X}"), static_cast<unsigned>(device->GetFeatureLevel())) :
			                     "device slot resolves but is null - expected before the renderer starts"s;
			f.stereoResolved = StereoLayout::Resolved();
			f.stereoLayout = f.stereoResolved ?
			                     std::string{ StereoLayout::Summary() } :
			                     Blocked("StereoLayout::Resolve; the verified FO4VR default and both explicit overrides are implemented"sv);

			f.hooksInstalled = Hooks::Installed();
			f.hookSites = Hooks::SiteCount();

			// Read the registry's verdict back rather than restating it. Select runs on
			// the startup availability pass. A bounded live gate may later initialise
			// either the null control arm or raw NGX and atomically commit that already
			// adapter-checked backend.
			const auto* const current = Backend::Current();
			const auto evaluate = Evaluate::Snapshot();
			if (current && evaluate.prepared && evaluate.backend == reinterpret_cast<std::uintptr_t>(current)) {
				f.selection = std::string{ current->Name() } + " - initialised for a bounded two-view live gate";
			} else if (current) {
				f.selection = std::string{ current->Name() } + " - startup availability selection; not initialised";
			} else {
				f.selection = "none selected - the registry returned nothing, which cannot happen while the null passthrough is compiled in"s;
			}

			return f;
		}

		// The proof, rather than the assertion, that the three render-target tables were
		// read correctly. Each row walks the whole chain - the manager's remap for the
		// physical slot, the manager's properties array for the shape, the renderer's
		// record for the views - on a target the engine names itself. It calls no D3D11
		// method and AddRefs nothing, so it is safe to run from wherever the deferred
		// pass runs.
		//
		// Two rows carry their own answer key. Motion vectors must come back as a
		// two-channel half float and linear depth as a single 32-bit float; no other
		// buffer in the set would plausibly carry either. If those two are right, all
		// three tables are based correctly, and if they are wrong the log says so
		// instead of the plugin proceeding on a misread.
		struct NamedTarget
		{
			std::int32_t logical;
			std::string_view name;
		};

		constexpr NamedTarget kNamedTargets[] = {
			{ Addr::kRT_BackBuffer, "backbuffer"sv },
			{ Addr::kRT_PreTonemapColor, "pre-tonemap"sv },
			{ Addr::kRT_Submitted, "submitted"sv },
			{ Addr::kRT_Albedo, "albedo"sv },
			{ Addr::kRT_Normal, "normal"sv },
			{ Addr::kRT_MotionVectors, "mv"sv },
			{ Addr::kRT_LinearDepth, "ldepth"sv },
			{ Addr::kRT_Taa, "taa"sv },
		};

		struct DepthSummary
		{
			std::int32_t allocated = 0;
			RenderTargets::PhysDS widest{};
		};

		[[nodiscard]] DepthSummary InspectDepthTargets() noexcept
		{
			DepthSummary summary{};
			for (std::int32_t i = 0; i < Addr::kDepthStencilCount; ++i) {
				const auto ds = RenderTargets::LookupDepth(i);
				if (!ds.valid || ds.width == 0) {
					continue;
				}
				++summary.allocated;
				if (ds.width > summary.widest.width) {
					summary.widest = ds;
				}
			}
			return summary;
		}

		void ReportRenderTargets()
		{
			if (!Engine::Resolved()) {
				logger::info("self-check (deferred): render targets not read - the engine handles did not resolve"sv);
				return;
			}

			const auto v = Engine::Validate();
			logger::info(FMT_STRING("self-check (deferred): tables - {} of {} logical ids map to a slot, {} carry dimensions, {} resolve to a shader resource view; stereo flag {}"),
				v.remapValid, Addr::kRenderTargetCount, v.propsPopulated, v.recordsWithSrv,
				v.stereo ? "set"sv : "clear"sv);
			logger::info(FMT_STRING("self-check (deferred): tables - {}"), v.note);

			for (const auto& t : kNamedTargets) {
				const auto rt = RenderTargets::Lookup(t.logical);
				if (!rt.valid) {
					logger::info(FMT_STRING("self-check (deferred): target {} (logical {}) did not resolve - slot {}, which is normal for an unused id"),
						t.name, t.logical, rt.phys);
					continue;
				}
				logger::info(FMT_STRING("self-check (deferred): target {} logical {} -> slot {}, {}x{}, dxgi {}, rtv {}, srv {}, uav {}"),
					t.name, t.logical, rt.phys, rt.width, rt.height, rt.format,
					rt.rtv ? "yes"sv : "no"sv,
					rt.srv ? "yes"sv : "no"sv,
					rt.uav ? "yes"sv : "no"sv);
			}

			// Depth is a required upscaler input and lives in a separate array with its
			// own stride, so it gets its own line rather than being assumed to follow.
			const auto depth = InspectDepthTargets();
			if (!depth.widest.valid) {
				logger::info("self-check (deferred): depth-stencil - nothing allocated, which means the array offset is wrong or the renderer has not started"sv);
				return;
			}

			logger::info(FMT_STRING("self-check (deferred): depth-stencil - {} of {} allocated; widest is logical {} -> slot {}, {}x{} (array {}), dsv {}, srv {}, tex {}"),
				depth.allocated, Addr::kDepthStencilCount, depth.widest.index, depth.widest.phys,
				depth.widest.width, depth.widest.height, depth.widest.arraySize,
				depth.widest.dsv ? "yes"sv : "no"sv,
				depth.widest.srv ? "yes"sv : "no"sv,
				depth.widest.tex ? "yes"sv : "no"sv);
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

		logger::info(FMT_STRING("self-check: engine handles {}"), f.engineHandles);
		const auto dynamicResolution = DynamicResolution::Snapshot();
		logger::info(
			FMT_STRING("self-check: dynamic resolution status {}, reason {}, requested {}, scale {} (x {}, y {}; vendor ratio {}, snapped {} to {}x{} of {}x{}), anchors {}, applied {}, world-gated {}, world-active {}, current {}/{}, targets {}/{}, image selector {} ({}), hidden-area mesh {} ({}; required {}, force-off {}, applied {}, readback {})"),
			DynamicResolution::StatusName(dynamicResolution.status),
			DynamicResolution::FailureName(dynamicResolution.failure),
			dynamicResolution.requested ? "yes"sv : "no"sv,
			dynamicResolution.requestedScale,
			dynamicResolution.requestedScaleX,
			dynamicResolution.requestedScaleY,
			dynamicResolution.vendorScalingRatio,
			dynamicResolution.snapped ? "yes"sv : "no"sv,
			dynamicResolution.snappedRenderWidth,
			dynamicResolution.snappedRenderHeight,
			dynamicResolution.snappedOutputWidth,
			dynamicResolution.snappedOutputHeight,
			dynamicResolution.anchorsResolved ? "resolved"sv : "unresolved"sv,
			dynamicResolution.applied ? "yes"sv : "no"sv,
			dynamicResolution.worldAdmissionGated ? "yes"sv : "no"sv,
			dynamicResolution.worldFrameActive ? "yes"sv : "no"sv,
			dynamicResolution.currentWidth, dynamicResolution.currentHeight,
			dynamicResolution.currentTargetWidth, dynamicResolution.currentTargetHeight,
			dynamicResolution.currentImageSelector,
			dynamicResolution.selectorValueValid ? "valid"sv : "invalid"sv,
			dynamicResolution.currentHiddenAreaMesh,
			dynamicResolution.hiddenAreaMeshValueValid ? "valid"sv : "invalid"sv,
			dynamicResolution.hiddenAreaMeshRequired ? "yes"sv : "no"sv,
			dynamicResolution.forceHiddenAreaMeshOff ? "yes"sv : "no"sv,
			dynamicResolution.hiddenAreaMeshApplied ? "yes"sv : "no"sv,
			dynamicResolution.hiddenAreaMeshReadbackMatched ? "matched"sv : "not-matched"sv);
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

		logger::info("self-check: stubbed on this build - Debug pixel readback. Real - Engine, RenderTargets, StereoLayout, Intermediates colour/raw-motion/camera-completed-motion/output, DS1 per-eye depth references, guarded current/previous camera matrices, backend-owned jitter sequencing and projection round trip, world-bracketed sampler mip bias, explicit backend auto-exposure contract, Evaluate null control, D3D11StateIsolation, Hooks, Debug binding observation, nonblocking quality-transaction CPU/GPU telemetry, FaultGuard, ResetPolicy, ScopePolicy, SelfCheck. Normal activation remains gated on a controlled temporal headset-quality pass."sv);

		// Anyone who installs this and sees no visual change must find the reason here
		// rather than conclude the mod is broken.
		logger::info("self-check: this build may install fail-soft ClearColor, pre-tonemap HDR/dynamic-resolution dispatch, native TemporalAA entry, prepared-draw call and dynamic-resolution IsActive hooks after this startup report. The first world HDR call probes private D3D11.1 state isolation. Default settings change no pixels; eleven pre-HDR diagnostics are mutually exclusive, and the two mutually exclusive TAA modes may run alone or couple to exactly one bounded continuous NGX/FSR source through accepted source-tagged handback serials. NGX creation evaluates no frame; NGX private evaluation has no reassembly target; NGX private reassembly makes no engine colour copy; NGX handback deliberately copies exactly one reconstructed frame into logical colour 1; NGX bounded-continuous warms 16 private frames then copies exactly the configured accepted-frame target. FSR private evaluation has no reassembly target; FSR private reassembly makes no engine colour copy; FSR handback deliberately copies exactly one reconstructed frame into logical colour 1; FSR bounded-continuous uses the same configurable bound. All evaluation gates submit null exposure with auto-exposure enabled and both exposure scalars fixed at 1."sv);
	}

	void RunDeferred()
	{
		static bool s_done = false;
		if (s_done) {
			return;
		}
		s_done = true;

		Facts f;
		CollectAdapters(f);
		CollectModules(f);

		if (f.adapters.empty()) {
			logger::info(FMT_STRING("self-check (deferred): gpu adapters still unavailable - {}"), f.adaptersBlocked);
		} else {
			for (const auto& line : f.adapters) {
				logger::info(FMT_STRING("self-check (deferred): gpu {}"), line);
			}
		}

		// Re-probed because the load-order snapshot taken at plugin load undercounts:
		// anything below us had not loaded yet, so an "absent" there was a timing
		// artefact rather than a fact about the install.
		for (const auto& line : f.modules) {
			logger::info(FMT_STRING("self-check (deferred): module {}"), line);
		}

		ReportRenderTargets();
		const auto probe = Hooks::RenderThreadProbe();
		logger::info(FMT_STRING("self-check (deferred): render-thread probe - installed {}, calls {}, thread {}, thread mismatches {}, renderer {:016X}, renderer mismatches {}"),
			probe.installed ? "yes"sv : "no"sv,
			probe.calls,
			probe.threadId,
			probe.threadMismatches,
			probe.renderer,
			probe.rendererMismatches);
		const auto temporal = Hooks::TemporalAAProbe();
		logger::info(FMT_STRING("self-check (deferred): TemporalAA probe - entry installed {}, draw installed {}, calls {}, thread {}, thread mismatches {}, effect {:016X}, effect mismatches {}, native draws {}, fallback native draws {}"),
			temporal.installed ? "yes"sv : "no"sv,
			temporal.drawInstalled ? "yes"sv : "no"sv,
			temporal.calls,
			temporal.threadId,
			temporal.threadMismatches,
			temporal.effect,
			temporal.effectMismatches,
			temporal.nativeDraws,
			temporal.fallbackNativeDraws);
		const auto hdr = Hooks::HDRInputProbe();
		logger::info(FMT_STRING("self-check (deferred): HDR input probe - installed {}, draw hook {}, calls {}, thread {}, thread mismatches {}, effect {:016X}, source {}, destination {}, route mismatches {}"),
			hdr.installed ? "yes"sv : "no"sv,
			hdr.drawInstalled ? "yes"sv : "no"sv,
			hdr.calls,
			hdr.threadId,
			hdr.threadMismatches,
			hdr.effect,
			hdr.sourceLogical,
			hdr.destinationLogical,
			hdr.routeMismatches);
		const auto dynamicDispatch = Hooks::DynamicResolutionDispatch();
		logger::info(FMT_STRING("self-check (deferred): dynamic-resolution quality transaction - installed {}, quality ready {}, effect {:016X}, calls {}, suppressions {}, passthroughs {}, geometry refreshes {}, flag preflights {}, dynamic dispatches/failures {}/{}, missing-TAA closures {}, pre-world calls/repairs/failures {}/{}/{}, stale watchdog restores {}"),
			dynamicDispatch.installed ? "yes"sv : "no"sv,
			dynamicDispatch.qualityReady ? "yes"sv : "no"sv,
			dynamicDispatch.effect,
			dynamicDispatch.calls,
			dynamicDispatch.suppressions,
			dynamicDispatch.passthroughs,
			dynamicDispatch.geometryRefreshes,
			dynamicDispatch.geometryFlagPreflights,
			dynamicDispatch.dynamicGeometryDispatches,
			dynamicDispatch.dynamicGeometryFailures,
			dynamicDispatch.missingTaaClosures,
			dynamicDispatch.preWorldCalls,
			dynamicDispatch.preWorldRepairs,
			dynamicDispatch.preWorldFailures,
			dynamicDispatch.staleWatchdogRestores);
		const auto isolation = HDRRoundTrip::Snapshot();
		logger::info(FMT_STRING("self-check (deferred): pre-HDR isolated round trip - enabled {}, isolation attempted {}, isolation ready {}, private ready {}, completed {}, session disabled {}, successes {}/{}, failures {}, skips {}, resource builds {}, dynamic-upsample suppression arms {}, thread {}, feature level {:X}, HRESULT {:08X}, device removed {:08X}, exception {:08X}, {}x{} dxgi {}, samples {}"),
			isolation.enabled ? "yes"sv : "no"sv,
			isolation.isolationAttempted ? "yes"sv : "no"sv,
			isolation.isolationReady ? "yes"sv : "no"sv,
			isolation.privateReady ? "yes"sv : "no"sv,
			isolation.completed ? "yes"sv : "no"sv,
			isolation.sessionDisabled ? "yes"sv : "no"sv,
			isolation.successes,
			isolation.targetSuccesses,
			isolation.failures,
			isolation.skips,
			isolation.resourceBuilds,
			isolation.dynamicUpsampleSuppressionsArmed,
			isolation.threadId,
			isolation.featureLevel,
			static_cast<std::uint32_t>(isolation.lastError),
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode,
			isolation.width,
			isolation.height,
			isolation.format,
			isolation.sampleCount);
		const auto evaluate = Evaluate::Snapshot();
		const auto intermediates = Intermediates::Snapshot();
		const auto cameraMotion = CameraMotion::Snapshot();
		const auto jitter = Jitter::Snapshot();
		logger::info(FMT_STRING("self-check (deferred): vendor orchestrator - prepared {}, active {}, owns backend state {}, backend/device/context {:016X}/{:016X}/{:016X}"),
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.active ? "yes"sv : "no"sv,
			evaluate.ownsBackendState ? "yes"sv : "no"sv,
			evaluate.backend,
			evaluate.device,
			evaluate.context);
		logger::info(FMT_STRING("self-check (deferred): stereo mode - requested {}, effective {} on backend '{}', intermediates built for sub-rects {}"),
			isolation.stereoSubrectsRequested ? "subrect"sv : "intermediates"sv,
			isolation.stereoSubrectsEffective ? "subrect"sv : "intermediates"sv,
			HDRRoundTrip::StereoBackendName(static_cast<HDRRoundTrip::StereoBackend>(isolation.stereoBackend)),
			intermediates.subrects ? "yes"sv : "no"sv);
		logger::info(FMT_STRING("self-check (deferred): stereo null gate - enabled {}, completed {}, successes {}/{}, failures {}, skips {}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, evaluate failures {}, last step {}, intermediates ready {}, builds {}, HRESULT {:08X}"),
			isolation.stereoNullEnabled ? "yes"sv : "no"sv,
			isolation.stereoNullCompleted ? "yes"sv : "no"sv,
			isolation.stereoNullSuccesses,
			isolation.targetSuccesses,
			isolation.stereoNullFailures,
			isolation.stereoNullSkips,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(intermediates.lastError));
		logger::info(FMT_STRING("self-check (deferred): camera motion - attempts {}, successes {}, failures {}, active staging {:016X}, last failure {}, completion/fallback draws {}/{}"),
			cameraMotion.attempts,
			cameraMotion.successes,
			cameraMotion.failures,
			cameraMotion.staging,
			static_cast<std::uint32_t>(cameraMotion.lastFailure),
			intermediates.motionCompletionDraws,
			intermediates.motionFallbackDraws);
		logger::info(FMT_STRING("self-check (deferred): NGX create-only gate - enabled {}, completed {}, successes {}, attempts {}, failures {}, skips {}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ngxCreateEnabled ? "yes"sv : "no"sv,
			isolation.ngxCreateCompleted ? "yes"sv : "no"sv,
			isolation.ngxCreateSuccesses,
			isolation.ngxCreateAttempts,
			isolation.ngxCreateFailures,
			isolation.ngxCreateSkips,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): NGX private-evaluate gate - enabled {}, completed {}, successes {}, attempts {}, failures {}, skips {}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, evaluate failures {}, last step {}, auto-exposure submissions {}, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ngxEvaluateEnabled ? "yes"sv : "no"sv,
			isolation.ngxEvaluateCompleted ? "yes"sv : "no"sv,
			isolation.ngxEvaluateSuccesses,
			isolation.ngxEvaluateAttempts,
			isolation.ngxEvaluateFailures,
			isolation.ngxEvaluateSkips,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.autoExposureSubmissions,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): FSR private-evaluate gate - enabled {}, completed {}, successes {}, attempts {}, failures {}, skips {}, teardown pending/completed {}/{}, teardown attempts/successes/failures {}/{}/{}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, evaluate failures {}, last step {}, auto-exposure submissions {}, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ffxEvaluateEnabled ? "yes"sv : "no"sv,
			isolation.ffxEvaluateCompleted ? "yes"sv : "no"sv,
			isolation.ffxEvaluateSuccesses,
			isolation.ffxEvaluateAttempts,
			isolation.ffxEvaluateFailures,
			isolation.ffxEvaluateSkips,
			isolation.ffxTeardownPending ? "yes"sv : "no"sv,
			isolation.ffxTeardownCompleted ? "yes"sv : "no"sv,
			isolation.ffxTeardownAttempts,
			isolation.ffxTeardownSuccesses,
			isolation.ffxTeardownFailures,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.autoExposureSubmissions,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): FSR private-reassembly gate - enabled {}, completed {}, successes {}, attempts {}, failures {}, skips {}, private serial {}, teardown pending/completed {}/{}, teardown attempts/successes/failures {}/{}/{}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, reassemblies {}/{}, evaluate failures {}, last step {}, auto-exposure submissions {}, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ffxReassembleEnabled ? "yes"sv : "no"sv,
			isolation.ffxReassembleCompleted ? "yes"sv : "no"sv,
			isolation.ffxReassembleSuccesses,
			isolation.ffxReassembleAttempts,
			isolation.ffxReassembleFailures,
			isolation.ffxReassembleSkips,
			isolation.privateReassemblySerial,
			isolation.ffxTeardownPending ? "yes"sv : "no"sv,
			isolation.ffxTeardownCompleted ? "yes"sv : "no"sv,
			isolation.ffxTeardownAttempts,
			isolation.ffxTeardownSuccesses,
			isolation.ffxTeardownFailures,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.leftReassemblies,
			evaluate.rightReassemblies,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.autoExposureSubmissions,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): FSR engine-handback gate - enabled {}, completed {}, successes {}, attempts {}, failures {}, skips {}, copies {}, private serial {}, teardown pending/completed {}/{}, teardown attempts/successes/failures {}/{}/{}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, reassemblies {}/{}, evaluate failures {}, last step {}, auto-exposure submissions {}, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ffxHandbackEnabled ? "yes"sv : "no"sv,
			isolation.ffxHandbackCompleted ? "yes"sv : "no"sv,
			isolation.ffxHandbackSuccesses,
			isolation.ffxHandbackAttempts,
			isolation.ffxHandbackFailures,
			isolation.ffxHandbackSkips,
			isolation.ffxHandbackCopies,
			isolation.privateReassemblySerial,
			isolation.ffxTeardownPending ? "yes"sv : "no"sv,
			isolation.ffxTeardownCompleted ? "yes"sv : "no"sv,
			isolation.ffxTeardownAttempts,
			isolation.ffxTeardownSuccesses,
			isolation.ffxTeardownFailures,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.leftReassemblies,
			evaluate.rightReassemblies,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.autoExposureSubmissions,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): FSR bounded-continuous gate - enabled {}, completed {}, warm-up {}/{}, handbacks {}/{}, attempts {}, failures {}, skips {}, engine copies {}, teardown pending/completed {}/{}, teardown attempts/successes/failures {}/{}/{}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, reassemblies {}/{}, evaluate failures {}, last step {}, auto-exposure submissions {}, frame-time samples {}, fallbacks {}, last delta {} ms, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ffxContinuousEnabled ? "yes"sv : "no"sv,
			isolation.ffxContinuousCompleted ? "yes"sv : "no"sv,
			isolation.ffxContinuousWarmupSuccesses,
			isolation.ffxContinuousWarmupTarget,
			isolation.ffxContinuousSuccesses,
			isolation.ffxContinuousTargetSuccesses,
			isolation.ffxContinuousAttempts,
			isolation.ffxContinuousFailures,
			isolation.ffxContinuousSkips,
			isolation.ffxContinuousCopies,
			isolation.ffxTeardownPending ? "yes"sv : "no"sv,
			isolation.ffxTeardownCompleted ? "yes"sv : "no"sv,
			isolation.ffxTeardownAttempts,
			isolation.ffxTeardownSuccesses,
			isolation.ffxTeardownFailures,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.leftReassemblies,
			evaluate.rightReassemblies,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.autoExposureSubmissions,
			evaluate.frameTimeSamples,
			evaluate.frameTimeFallbacks,
			evaluate.frameTimeDeltaMs,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): NGX private-reassembly gate - enabled {}, completed {}, successes {}, attempts {}, failures {}, skips {}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, reassemblies {}/{}, evaluate failures {}, last step {}, auto-exposure submissions {}, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ngxReassembleEnabled ? "yes"sv : "no"sv,
			isolation.ngxReassembleCompleted ? "yes"sv : "no"sv,
			isolation.ngxReassembleSuccesses,
			isolation.ngxReassembleAttempts,
			isolation.ngxReassembleFailures,
			isolation.ngxReassembleSkips,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.leftReassemblies,
			evaluate.rightReassemblies,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.autoExposureSubmissions,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): NGX engine-handback gate - enabled {}, completed {}, successes {}, attempts {}, failures {}, skips {}, engine copies {}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, reassemblies {}/{}, evaluate failures {}, last step {}, auto-exposure submissions {}, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ngxHandbackEnabled ? "yes"sv : "no"sv,
			isolation.ngxHandbackCompleted ? "yes"sv : "no"sv,
			isolation.ngxHandbackSuccesses,
			isolation.ngxHandbackAttempts,
			isolation.ngxHandbackFailures,
			isolation.ngxHandbackSkips,
			isolation.ngxHandbackCopies,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.leftReassemblies,
			evaluate.rightReassemblies,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.autoExposureSubmissions,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): NGX bounded-continuous gate - enabled {}, completed {}, warm-up {}/{}, committed handbacks {}/{}, attempts {}, failures {}, skips {}, engine copies {}, candidate pending {}, candidates/commits/aborts {}/{}/{}, candidate serial {}, teardown pending/completed {}/{}, teardown attempts/successes/failures {}/{}/{}, prepared {}, prepare count {}, evaluate attempts {}, left/right calls {}/{}, reassemblies {}/{}, evaluate failures {}, last step {}, auto-exposure submissions {}, frame-time samples {}, fallbacks {}, last delta {} ms, intermediates ready {}, builds {}, device removed {:08X}, exception {:08X}"),
			isolation.ngxContinuousEnabled ? "yes"sv : "no"sv,
			isolation.ngxContinuousCompleted ? "yes"sv : "no"sv,
			isolation.ngxContinuousWarmupSuccesses,
			isolation.ngxContinuousWarmupTarget,
			isolation.ngxContinuousSuccesses,
			isolation.ngxContinuousTargetSuccesses,
			isolation.ngxContinuousAttempts,
			isolation.ngxContinuousFailures,
			isolation.ngxContinuousSkips,
			isolation.ngxContinuousCopies,
			isolation.ngxQualityCandidatePending ? "yes"sv : "no"sv,
			isolation.ngxQualityCandidates,
			isolation.ngxQualityCommits,
			isolation.ngxQualityAborts,
			isolation.ngxHandbackCandidateSerial,
			isolation.ngxTeardownPending ? "yes"sv : "no"sv,
			isolation.ngxTeardownCompleted ? "yes"sv : "no"sv,
			isolation.ngxTeardownAttempts,
			isolation.ngxTeardownSuccesses,
			isolation.ngxTeardownFailures,
			evaluate.prepared ? "yes"sv : "no"sv,
			evaluate.prepareCount,
			evaluate.attempts,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.leftReassemblies,
			evaluate.rightReassemblies,
			evaluate.failures,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.autoExposureSubmissions,
			evaluate.frameTimeSamples,
			evaluate.frameTimeFallbacks,
			evaluate.frameTimeDeltaMs,
			intermediates.ready ? "yes"sv : "no"sv,
			intermediates.resourceBuilds,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		logger::info(FMT_STRING("self-check (deferred): quality pixel transaction - phase {}, evaluations {}, RT1 copies {}, native fallbacks {} (late/abort closes {}/{}, last seam {}), dirty closes {}, unrecoverable {}"),
			isolation.qualityPixelPhase,
			isolation.qualityPixelEvaluations,
			isolation.qualityPixelCopies,
			isolation.qualityPixelNativeFallbacks,
			isolation.qualityPixelFallbackLateCloses,
			isolation.qualityPixelFallbackAbortCloses,
			HDRRoundTrip::QualityFallbackSeamName(static_cast<HDRRoundTrip::QualityFallbackSeam>(isolation.qualityPixelLastFallbackSeam)),
			isolation.qualityPixelDirtyCloses,
			isolation.qualityPixelUnrecoverable);
		logger::info(FMT_STRING("self-check (deferred): quality fault injection - armed {} ({}) remaining {}, last triggered {} ({}), arm attempts/arms/rejections {}/{}/{}, triggers {}"),
			HDRRoundTrip::QualityFaultName(static_cast<HDRRoundTrip::QualityFaultSeam>(isolation.qualityFaultArmedSeam)),
			isolation.qualityFaultArmedSeam,
			isolation.qualityFaultRemaining,
			HDRRoundTrip::QualityFaultName(static_cast<HDRRoundTrip::QualityFaultSeam>(isolation.qualityFaultLastTriggeredSeam)),
			isolation.qualityFaultLastTriggeredSeam,
			isolation.qualityFaultArmAttempts,
			isolation.qualityFaultArms,
			isolation.qualityFaultRejections,
			isolation.qualityFaultTriggers);
		logger::info(FMT_STRING("self-check (deferred): session recovery - state {}, failSoft {}, suspend frames left {}, re-arms {}/{}, cool-down {} frames, suspensions {}, last refusal {}, sibling latches taaReplacement/dynamicGeometry {}/{}, dynamicGeometry reason {}"),
			HDRRoundTrip::SessionStateName(static_cast<HDRRoundTrip::SessionState>(isolation.sessionState)),
			isolation.failSoft ? "yes"sv : "no"sv,
			isolation.suspendFramesLeft,
			isolation.rearmCount,
			HDRRoundTrip::kRearmsPerSession,
			HDRRoundTrip::kRearmCooldownFrames,
			isolation.suspensionCount,
			HDRRoundTrip::RecoveryRefusalName(static_cast<HDRRoundTrip::RecoveryRefusal>(isolation.recoveryRefusal)),
			isolation.taaReplacementSessionDisabled ? "yes"sv : "no"sv,
			isolation.dynamicGeometrySessionDisabled ? "yes"sv : "no"sv,
			isolation.dynamicGeometryDisableReason);
		const auto taaReplacement = TAAReplacement::Snapshot();
		logger::info(FMT_STRING("self-check (deferred): native-TAA replacement - enabled {}, post-draw/bypass {}/{}, required/current handback {}/{}, require NGX/FFX {}/{}, baseline {}, last/current serial {}/{}, ready {}, completed {}, session disabled {}, successes {}/{}, attempts {}, no-handback skips {}, source mismatches {}, sequence gaps {}, validation/isolation failures {}/{}, validation mask {:08X}, shader builds {}, history 61/62 writes {}/{}, post-draw overwrites/native bypasses {}/{}, parity reads/failures/invalid {}/{}/{}, last parity {}, engine/expected history {}/{}, current-target mismatches {}, last TAA call {}, {}x{} source/history/output dxgi {}/{}/{}, parity address {:016X}, HRESULT {:08X}, device removed {:08X}"),
			taaReplacement.enabled ? "yes"sv : "no"sv,
			taaReplacement.postDrawEnabled ? "yes"sv : "no"sv,
			taaReplacement.bypassEnabled ? "yes"sv : "no"sv,
			HDRRoundTrip::HandbackSourceName(taaReplacement.handbackSource),
			HDRRoundTrip::HandbackSourceName(taaReplacement.currentHandbackSource),
			taaReplacement.requireNgxHandback ? "yes"sv : "no"sv,
			taaReplacement.requireFfxHandback ? "yes"sv : "no"sv,
			taaReplacement.handbackBaseline,
			taaReplacement.lastHandbackSerial,
			taaReplacement.currentHandbackSerial,
			taaReplacement.ready ? "yes"sv : "no"sv,
			taaReplacement.completed ? "yes"sv : "no"sv,
			taaReplacement.sessionDisabled ? "yes"sv : "no"sv,
			taaReplacement.successes,
			taaReplacement.targetSuccesses,
			taaReplacement.attempts,
			taaReplacement.noHandbackSkips,
			taaReplacement.handbackSourceMismatches,
			taaReplacement.handbackSequenceGaps,
			taaReplacement.validationFailures,
			taaReplacement.isolationFailures,
			taaReplacement.validationMask,
			taaReplacement.shaderBuilds,
			taaReplacement.history61Writes,
			taaReplacement.history62Writes,
			taaReplacement.postDrawOverwrites,
			taaReplacement.nativeDrawBypasses,
			taaReplacement.parityReadAttempts,
			taaReplacement.parityReadFailures,
			taaReplacement.parityInvalidValues,
			taaReplacement.lastParity,
			taaReplacement.engineHistoryLogical,
			taaReplacement.expectedHistoryLogical,
			taaReplacement.currentRenderTargetMismatches,
			taaReplacement.lastTemporalAACall,
			taaReplacement.width,
			taaReplacement.height,
			taaReplacement.sourceFormat,
			taaReplacement.historyFormat,
			taaReplacement.outputFormat,
			taaReplacement.parityAddress,
			taaReplacement.lastError,
			static_cast<std::uint32_t>(taaReplacement.deviceRemovedReason));
		logger::info(FMT_STRING("self-check (deferred): transactional TAA operation counters - seam publish draws {}, history repair arms/attempts/copies/successes/failures {}/{}/{}/{}/{}, native fallback releases/draws {}/{}, fallback history 61/62 writes {}/{}, fallback history mismatches {}"),
			taaReplacement.qualityPublishDraws,
			taaReplacement.qualityHistoryRepairArms,
			taaReplacement.qualityHistoryRepairAttempts,
			taaReplacement.qualityHistoryRepairCopies,
			taaReplacement.qualityHistoryRepairSuccesses,
			taaReplacement.qualityHistoryRepairFailures,
			taaReplacement.qualityNativeFallbackReleases,
			taaReplacement.qualityNativeFallbackDraws,
			taaReplacement.qualityNativeFallbackHistory61Writes,
			taaReplacement.qualityNativeFallbackHistory62Writes,
			taaReplacement.qualityNativeFallbackHistoryMismatches);
		logger::info(FMT_STRING("self-check (deferred): TemporalAA-seam sharpen - requested {:.3f}, effective {:.3f}, {:.1f} stops, shader {}, compile failures {}, HRESULT {:08X}"),
			taaReplacement.sharpnessRequested,
			taaReplacement.sharpnessEffective,
			taaReplacement.sharpenStops,
			TAAReplacement::SeamShaderName(taaReplacement.sharpenShader),
			taaReplacement.sharpenCompileFailures,
			taaReplacement.sharpenLastHr);
		const auto reset = ResetPolicy::Snapshot();
		logger::info(FMT_STRING("self-check (deferred): history reset policy - pending {}, menu/cell sinks {}/{}, settings menu/load/cell {}/{}/{}, requests {}, coalesced {}, paired acknowledgements {}, generation requested/acknowledged {}/{}, acquisitions {}, load events {}, menu open/close {}/{}, cell enter/leave {}/{} last {:08X}, left/right reset submissions {}/{}"),
			reset.pending ? "yes"sv : "no"sv,
			reset.menuSinkInstalled ? "yes"sv : "no"sv,
			reset.cellSinkInstalled ? "yes"sv : "no"sv,
			reset.resetOnMenu ? "yes"sv : "no"sv,
			reset.resetOnLoad ? "yes"sv : "no"sv,
			reset.resetOnCell ? "yes"sv : "no"sv,
			reset.requests,
			reset.coalesced,
			reset.consumptions,
			reset.requestedGeneration,
			reset.acknowledgedGeneration,
			reset.acquisitions,
			reset.loadEvents,
			reset.menuOpenEvents,
			reset.menuCloseEvents,
			reset.cellEnterEvents,
			reset.cellLeaveEvents,
			reset.lastCellId,
			reset.leftSubmissions,
			reset.rightSubmissions);
		logger::info(FMT_STRING("self-check (deferred): history reset policy - quality-abort requests {}"),
			reset.qualityAbortRequests);
		logger::info(FMT_STRING("self-check (deferred): quality world admission - phase {}, reason {}, admitted {}, stable {}/{}, generation {}, cell {:X}, checks/denied/activations {}/{}/{}, invalidations/reset requests {}/{}, rechecks/failures {}/{}, publish rechecks/vetoes {}/{}, UI menu/item/freezeBG/freezePause/closing/mask {}/{}/{}/{}/{}/{:02X}"),
			ResetPolicy::WorldAdmissionPhaseName(reset.worldAdmissionPhase),
			ResetPolicy::WorldBlockReasonName(reset.worldBlockReason),
			reset.worldAdmitted ? "yes"sv : "no"sv,
			reset.worldStableFrames,
			reset.worldStableFramesRequired,
			reset.worldTransitionGeneration,
			reset.worldCell,
			reset.worldAdmissionChecks,
			reset.worldAdmissionDeniedFrames,
			reset.worldAdmissionActivations,
			reset.worldAdmissionInvalidations,
			reset.worldAdmissionResetRequests,
			reset.worldAdmissionRechecks,
			reset.worldAdmissionRecheckFailures,
			reset.worldAdmissionPublishRechecks,
			reset.worldAdmissionPublishVetoes,
			reset.worldMenuMode,
			reset.worldItemMenuMode,
			reset.worldFreezeFrameMenuBG,
			reset.worldFreezeFramePause,
			reset.worldClosingAllMenus ? "yes"sv : "no"sv,
			reset.worldOpenBlockingMenuMask);
		const auto scope = ScopePolicy::Snapshot();
		logger::info(FMT_STRING("self-check (deferred): scope policy - suppress {}, renderer reads success/attempt/failure {}/{}/{}, scoped/suppressed HDR dispatches {}/{}"),
			scope.suppressDuringScopeRender ? "yes"sv : "no"sv,
			scope.rendererReadSuccesses,
			scope.rendererReadAttempts,
			scope.rendererReadFailures,
			scope.scopedDispatches,
			scope.suppressedDispatches);
		logger::info(FMT_STRING("self-check (deferred): jitter - resolved {}, armed {}, reads {}/{}, failures {}, phase {}/{}, engine phase {}, distinct {}, mask {:016X}{:016X}, engine {}x{}, gates {}/{}, pixel ({}, {}), bounds x [{}, {}] y [{}, {}], eye submissions {}/{}, stereo matches {}, mismatches {}, last failure {}"),
			jitter.resolved ? "yes"sv : "no"sv,
			jitter.backendSequenceArmed ? "yes"sv : "no"sv,
			jitter.readSuccesses,
			jitter.readAttempts,
			jitter.readFailures,
			jitter.phase,
			jitter.phaseCount,
			jitter.enginePhase,
			jitter.distinctPhasesSeen,
			jitter.observedPhaseMask[1],
			jitter.observedPhaseMask[0],
			jitter.engineWidth,
			jitter.engineHeight,
			jitter.enabled,
			jitter.disableCount,
			jitter.jitterX,
			jitter.jitterY,
			jitter.minJitterX,
			jitter.maxJitterX,
			jitter.minJitterY,
			jitter.maxJitterY,
			jitter.leftSubmissions,
			jitter.rightSubmissions,
			jitter.stereoMatches,
			jitter.stereoMismatches,
			static_cast<std::uint32_t>(jitter.lastFailure));
		// The recovered offsets are the committed projection floats run back through the
		// FULL per-eye extent. They must equal the pixel pair above at every live ratio,
		// which is the whole ratio-cancellation claim in one line a headless run can read.
		const auto liveDynamicResolution = DynamicResolution::Snapshot();
		const float recoveredJitterX = jitter.engineWidth != 0 ?
		                                   Jitter::ProjectionToCameraPixelsX(jitter.committedProjectionX, jitter.engineWidth) :
		                                   0.0f;
		const float recoveredJitterY = jitter.engineHeight != 0 ?
		                                   Jitter::ProjectionToCameraPixelsY(jitter.committedProjectionY, jitter.engineHeight) :
		                                   0.0f;
		logger::info(FMT_STRING("self-check (deferred): jitter commit - writes {}/{}, mismatches {}, gate declines {}, sequence restarts {}, committed projection ({}, {}), recovered pixel ({}, {}), live ratio {}/{}, last write failure {}"),
			jitter.writeSuccesses,
			jitter.writeAttempts,
			jitter.writeMismatches,
			jitter.writeGateDeclines,
			jitter.sequenceRestarts,
			jitter.committedProjectionX,
			jitter.committedProjectionY,
			recoveredJitterX,
			recoveredJitterY,
			liveDynamicResolution.currentWidth,
			liveDynamicResolution.currentHeight,
			static_cast<std::uint32_t>(jitter.lastWriteFailure));

		// samplersCreated equal to entriesBiased with builtBias inside the guide band is
		// the machine-checkable proof that the feature is live; appliedFrames rising with
		// restoredFrames and unbalanced staying zero is the proof that it is balanced.
		const auto mip = MipBias::Snapshot();
		logger::info(FMT_STRING("self-check (deferred): mip bias - anchors resolved {}, agreed {}, table rva {:X}, native apply path {} ({} applies), originals cached {}, filter histogram {}/{}/{}/{}/{} other {}, entries {} biased {}, samplers created {} retained {}, built bias {} band [{}, {}] for {} -> {} per eye, override {}, trilinear {}, MinLOD lowering {}, applied/restored {}/{}, apply declines {}, unbalanced {}, band warnings {}, builds {}/{} declined, last failure {}"),
			mip.anchorsResolved ? "yes"sv : "no"sv,
			mip.anchorsAgreed ? "yes"sv : "no"sv,
			mip.tableAddress != 0 ? FaultGuard::ToRva(mip.tableAddress) : 0,
			mip.nativeApplyActive ? "active"sv : "inactive"sv,
			mip.nativeApplies,
			mip.table.originalsCached ? "yes"sv : "no"sv,
			mip.table.filterHistogram[0],
			mip.table.filterHistogram[1],
			mip.table.filterHistogram[2],
			mip.table.filterHistogram[3],
			mip.table.filterHistogram[4],
			mip.table.filterOther,
			mip.table.entriesTotal,
			mip.table.entriesBiased,
			mip.table.samplersCreated,
			mip.table.retainedSamplers,
			mip.table.builtBias,
			mip.table.guideBandLow,
			mip.table.guideBandHigh,
			mip.table.builtRenderWidth,
			mip.table.builtOutputWidth,
			mip.table.overrideActive ? "yes"sv : "no"sv,
			mip.table.includeTrilinear ? "included"sv : "exempt"sv,
			mip.table.lowerMinLod ? "on"sv : "off"sv,
			mip.table.appliedFrames,
			mip.table.restoredFrames,
			mip.table.applyDeclines,
			mip.table.unbalanced,
			mip.table.bandWarnings,
			mip.table.builds,
			mip.table.buildDeclines,
			static_cast<std::uint32_t>(mip.table.lastFailure));
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

		// The engine handles and what the tables look like through them. This is here so
		// an automated check can assert the whole resolution chain from outside the game
		// - which is how every other claim in this project has been made checkable.
		const auto validation = Engine::Validate();
		out += fmt::format(
			FMT_STRING(R"("engineResolved":{},"engineHandles":{},"tables":{{"ran":{},"remapValid":{},"propsPopulated":{},"recordsWithSrv":{},"motionVectorsPlausible":{},"linearDepthPlausible":{},"stereo":{},"note":{}}},)"),
			f.engineResolved ? "true"sv : "false"sv, Quote(f.engineHandles),
			validation.ran ? "true"sv : "false"sv,
			validation.remapValid, validation.propsPopulated, validation.recordsWithSrv,
			validation.motionVectorsPlausible ? "true"sv : "false"sv,
			validation.linearDepthPlausible ? "true"sv : "false"sv,
			validation.stereo ? "true"sv : "false"sv,
			Quote(validation.note));

		const auto dynamicResolution = DynamicResolution::Snapshot();
		out += fmt::format(
			FMT_STRING(R"("dynamicResolution":{{"configured":{},"initialized":{},"masterEnabled":{},"requested":{},"worldAdmissionGated":{},"worldFrameActive":{},"status":{},"reason":{},"anchorsResolved":{},"initialValuesValid":{},"applied":{},"requestedContractReady":{},"readbackMatched":{},"currentValuesValid":{},"selectorValueValid":{},"requestedScale":{},"requestedScaleX":{},"requestedScaleY":{},"vendorScalingRatio":{},"snapped":{},"snappedOutputWidth":{},"snappedOutputHeight":{},"snappedRenderWidth":{},"snappedRenderHeight":{},"hiddenAreaMesh":{{"forceOff":{},"required":{},"applied":{},"readbackMatched":{},"valueValid":{}}},"effectPassthrough":{{"attempts":{},"successes":{},"failures":{},"restoreFailures":{}}},"frameReassert":{{"attempts":{},"successes":{},"failures":{}}},"worldFrame":{{"beginAttempts":{},"beginSuccesses":{},"beginFailures":{},"endAttempts":{},"endSuccesses":{},"endFailures":{}}},"contract":{{"checks":{},"failures":{},"selectorInvariantFailures":{},"hiddenAreaMeshInvariantFailures":{}}},"addresses":{{"width":"{:016X}","height":"{:016X}","targetWidth":"{:016X}","targetHeight":"{:016X}","imageSelector":"{:016X}","hiddenAreaMesh":"{:016X}","widthRva":"{:X}","heightRva":"{:X}","targetWidthRva":"{:X}","targetHeightRva":"{:X}","imageSelectorRva":"{:X}","hiddenAreaMeshRva":"{:X}"}},"before":{{"width":{},"height":{},"targetWidth":{},"targetHeight":{},"imageSelector":{},"hiddenAreaMesh":{}}},"current":{{"width":{},"height":{},"targetWidth":{},"targetHeight":{},"imageSelector":{},"hiddenAreaMesh":{}}}}},)"),
			dynamicResolution.configured ? "true"sv : "false"sv,
			dynamicResolution.initialized ? "true"sv : "false"sv,
			dynamicResolution.masterEnabled ? "true"sv : "false"sv,
			dynamicResolution.requested ? "true"sv : "false"sv,
			dynamicResolution.worldAdmissionGated ? "true"sv : "false"sv,
			dynamicResolution.worldFrameActive ? "true"sv : "false"sv,
			Quote(DynamicResolution::StatusName(dynamicResolution.status)),
			Quote(DynamicResolution::FailureName(dynamicResolution.failure)),
			dynamicResolution.anchorsResolved ? "true"sv : "false"sv,
			dynamicResolution.initialValuesValid ? "true"sv : "false"sv,
			dynamicResolution.applied ? "true"sv : "false"sv,
			dynamicResolution.requestedContractReady ? "true"sv : "false"sv,
			dynamicResolution.readbackMatched ? "true"sv : "false"sv,
			dynamicResolution.currentValuesValid ? "true"sv : "false"sv,
			dynamicResolution.selectorValueValid ? "true"sv : "false"sv,
			JsonFloat(dynamicResolution.requestedScale),
			JsonFloat(dynamicResolution.requestedScaleX),
			JsonFloat(dynamicResolution.requestedScaleY),
			JsonFloat(dynamicResolution.vendorScalingRatio),
			dynamicResolution.snapped ? "true"sv : "false"sv,
			dynamicResolution.snappedOutputWidth,
			dynamicResolution.snappedOutputHeight,
			dynamicResolution.snappedRenderWidth,
			dynamicResolution.snappedRenderHeight,
			dynamicResolution.forceHiddenAreaMeshOff ? "true"sv : "false"sv,
			dynamicResolution.hiddenAreaMeshRequired ? "true"sv : "false"sv,
			dynamicResolution.hiddenAreaMeshApplied ? "true"sv : "false"sv,
			dynamicResolution.hiddenAreaMeshReadbackMatched ? "true"sv : "false"sv,
			dynamicResolution.hiddenAreaMeshValueValid ? "true"sv : "false"sv,
			dynamicResolution.effectPassthroughAttempts,
			dynamicResolution.effectPassthroughSuccesses,
			dynamicResolution.effectPassthroughFailures,
			dynamicResolution.effectPassthroughRestoreFailures,
			dynamicResolution.frameReassertAttempts,
			dynamicResolution.frameReassertSuccesses,
			dynamicResolution.frameReassertFailures,
			dynamicResolution.worldBeginAttempts,
			dynamicResolution.worldBeginSuccesses,
			dynamicResolution.worldBeginFailures,
			dynamicResolution.worldEndAttempts,
			dynamicResolution.worldEndSuccesses,
			dynamicResolution.worldEndFailures,
			dynamicResolution.contractChecks,
			dynamicResolution.contractFailures,
			dynamicResolution.selectorInvariantFailures,
			dynamicResolution.hiddenAreaMeshInvariantFailures,
			dynamicResolution.widthAddress, dynamicResolution.heightAddress,
			dynamicResolution.targetWidthAddress, dynamicResolution.targetHeightAddress,
			dynamicResolution.imageSelectorAddress, dynamicResolution.hiddenAreaMeshAddress,
			FaultGuard::ToRva(dynamicResolution.widthAddress),
			FaultGuard::ToRva(dynamicResolution.heightAddress),
			FaultGuard::ToRva(dynamicResolution.targetWidthAddress),
			FaultGuard::ToRva(dynamicResolution.targetHeightAddress),
			FaultGuard::ToRva(dynamicResolution.imageSelectorAddress),
			FaultGuard::ToRva(dynamicResolution.hiddenAreaMeshAddress),
			JsonFloat(dynamicResolution.beforeWidth), JsonFloat(dynamicResolution.beforeHeight),
			JsonFloat(dynamicResolution.beforeTargetWidth), JsonFloat(dynamicResolution.beforeTargetHeight),
			dynamicResolution.initialImageSelector, dynamicResolution.initialHiddenAreaMesh,
			JsonFloat(dynamicResolution.currentWidth), JsonFloat(dynamicResolution.currentHeight),
			JsonFloat(dynamicResolution.currentTargetWidth), JsonFloat(dynamicResolution.currentTargetHeight),
			dynamicResolution.currentImageSelector, dynamicResolution.currentHiddenAreaMesh);

		std::string targets = "[";
		for (std::size_t i = 0; i < std::size(kNamedTargets); ++i) {
			const auto& t = kNamedTargets[i];
			const auto rt = RenderTargets::Lookup(t.logical);
			if (i != 0) {
				targets += ',';
			}
			targets += fmt::format(
				FMT_STRING(R"({{"name":{},"logical":{},"phys":{},"width":{},"height":{},"dxgiFormat":{},"rtv":{},"srv":{},"uav":{},"valid":{}}})"),
				Quote(t.name), t.logical, rt.phys, rt.width, rt.height, rt.format,
				rt.rtv != nullptr ? "true"sv : "false"sv,
				rt.srv != nullptr ? "true"sv : "false"sv,
				rt.uav != nullptr ? "true"sv : "false"sv,
				rt.valid ? "true"sv : "false"sv);
		}
		targets += ']';
		out += fmt::format(FMT_STRING(R"("namedTargets":{},)"), targets);

		const auto depth = InspectDepthTargets();
		out += fmt::format(
			FMT_STRING(R"("depthStencil":{{"allocated":{},"logicalCount":{},"widestLogical":{},"widestPhys":{},"width":{},"height":{},"arraySize":{},"dsv":{},"srv":{},"texture":{},"valid":{}}},)"),
			depth.allocated, Addr::kDepthStencilCount,
			depth.widest.index, depth.widest.phys, depth.widest.width, depth.widest.height,
			depth.widest.arraySize,
			depth.widest.dsv != nullptr ? "true"sv : "false"sv,
			depth.widest.srv != nullptr ? "true"sv : "false"sv,
			depth.widest.tex != nullptr ? "true"sv : "false"sv,
			depth.widest.valid ? "true"sv : "false"sv);

		const auto probe = Hooks::RenderThreadProbe();
		out += fmt::format(
			FMT_STRING(R"("renderThreadProbe":{{"installed":{},"calls":{},"threadId":{},"threadMismatches":{},"renderer":"{:016X}","rendererMismatches":{}}},)"),
			probe.installed ? "true"sv : "false"sv,
			probe.calls,
			probe.threadId,
			probe.threadMismatches,
			probe.renderer,
			probe.rendererMismatches);

		const auto temporal = Hooks::TemporalAAProbe();
		out += fmt::format(
			FMT_STRING(R"("temporalAAProbe":{{"installed":{},"drawInstalled":{},"calls":{},"threadId":{},"threadMismatches":{},"effect":"{:016X}","effectMismatches":{},"nativeDraws":{},"fallbackNativeDraws":{}}},)"),
			temporal.installed ? "true"sv : "false"sv,
			temporal.drawInstalled ? "true"sv : "false"sv,
			temporal.calls,
			temporal.threadId,
			temporal.threadMismatches,
			temporal.effect,
			temporal.effectMismatches,
			temporal.nativeDraws,
			temporal.fallbackNativeDraws);
		const auto hdr = Hooks::HDRInputProbe();
		out += fmt::format(
			FMT_STRING(R"("hdrInputProbe":{{"installed":{},"drawInstalled":{},"calls":{},"threadId":{},"threadMismatches":{},"effect":"{:016X}","sourceLogical":{},"destinationLogical":{},"routeMismatches":{}}},)"),
			hdr.installed ? "true"sv : "false"sv,
			hdr.drawInstalled ? "true"sv : "false"sv,
			hdr.calls,
			hdr.threadId,
			hdr.threadMismatches,
			hdr.effect,
			hdr.sourceLogical,
			hdr.destinationLogical,
			hdr.routeMismatches);
		const auto isolation = HDRRoundTrip::Snapshot();
		const auto dynamicDispatch = Hooks::DynamicResolutionDispatch();
		out += fmt::format(
			FMT_STRING(R"("dynamicResolutionDispatch":{{"installed":{},"qualityReady":{},"effect":"{:016X}","calls":{},"suppressions":{},"passthroughs":{},"geometryRefreshes":{},"geometryFlagPreflights":{},"dynamicGeometryDispatches":{},"dynamicGeometryFailures":{},"missingTaaClosures":{},"preWorldCalls":{},"preWorldRepairs":{},"preWorldFailures":{},"staleWatchdogRestores":{},"neutralizedSourceLogical":{},"neutralizedDestinationLogical":{},"suppressionArms":{},"imageSpaceDepthOverrideEnabled":{},"imageSpaceDepthOverrideAttempts":{},"imageSpaceDepthOverrideNonNull":{},"imageSpaceDepthOverrideAlreadyNull":{},"imageSpaceDepthOverrideDraws":{},"imageSpaceDepthOverrideRestores":{},"imageSpaceDepthOverrideFailures":{},"imageSpaceDepthOverrideRestoreMismatches":{}}},)"),
			dynamicDispatch.installed ? "true"sv : "false"sv,
			dynamicDispatch.qualityReady ? "true"sv : "false"sv,
			dynamicDispatch.effect,
			dynamicDispatch.calls,
			dynamicDispatch.suppressions,
			dynamicDispatch.passthroughs,
			dynamicDispatch.geometryRefreshes,
			dynamicDispatch.geometryFlagPreflights,
			dynamicDispatch.dynamicGeometryDispatches,
			dynamicDispatch.dynamicGeometryFailures,
			dynamicDispatch.missingTaaClosures,
			dynamicDispatch.preWorldCalls,
			dynamicDispatch.preWorldRepairs,
			dynamicDispatch.preWorldFailures,
			dynamicDispatch.staleWatchdogRestores,
			dynamicDispatch.neutralizedSourceLogical,
			dynamicDispatch.neutralizedDestinationLogical,
			isolation.dynamicUpsampleSuppressionsArmed,
			dynamicDispatch.imageSpaceDepthOverrideEnabled ? "true"sv : "false"sv,
			dynamicDispatch.imageSpaceDepthOverrideAttempts,
			dynamicDispatch.imageSpaceDepthOverrideNonNull,
			dynamicDispatch.imageSpaceDepthOverrideAlreadyNull,
			dynamicDispatch.imageSpaceDepthOverrideDraws,
			dynamicDispatch.imageSpaceDepthOverrideRestores,
			dynamicDispatch.imageSpaceDepthOverrideFailures,
			dynamicDispatch.imageSpaceDepthOverrideRestoreMismatches);
		out += fmt::format(
			FMT_STRING(R"("d3d11StateIsolation":{{"attempted":{},"ready":{},"attempts":{},"threadId":{},"featureLevel":{},"lastError":{},"lastErrorHex":"{:08X}","device":"{:016X}","context":"{:016X}"}},)"),
			isolation.isolationAttempted ? "true"sv : "false"sv,
			isolation.isolationReady ? "true"sv : "false"sv,
			isolation.isolationAttempts,
			isolation.threadId,
			isolation.featureLevel,
			isolation.lastError,
			static_cast<std::uint32_t>(isolation.lastError),
			isolation.device,
			isolation.context);
		out += fmt::format(
			FMT_STRING(R"("preHdrRoundTrip":{{"enabled":{},"privateReady":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"targetSuccesses":{},"failures":{},"skips":{},"resourceBuilds":{},"width":{},"height":{},"dxgiFormat":{},"sampleCount":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}","sourceTexture":"{:016X}","privateTexture":"{:016X}"}},)"),
			isolation.enabled ? "true"sv : "false"sv,
			isolation.privateReady ? "true"sv : "false"sv,
			isolation.completed ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.roundTripAttempts,
			isolation.successes,
			isolation.targetSuccesses,
			isolation.failures,
			isolation.skips,
			isolation.resourceBuilds,
			isolation.width,
			isolation.height,
			isolation.format,
			isolation.sampleCount,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode,
			isolation.sourceTexture,
			isolation.privateTexture);
		const auto evaluate = Evaluate::Snapshot();
		const auto intermediates = Intermediates::Snapshot();
		const auto cameraMotion = CameraMotion::Snapshot();
		const auto jitter = Jitter::Snapshot();
		out += fmt::format(
			FMT_STRING(R"("preHdrStereoNull":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"targetSuccesses":{},"failures":{},"skips":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.stereoNullEnabled ? "true"sv : "false"sv,
			isolation.stereoNullCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.stereoNullAttempts,
			isolation.stereoNullSuccesses,
			isolation.targetSuccesses,
			isolation.stereoNullFailures,
			isolation.stereoNullSkips,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		std::string fallbackSeams;
		for (std::size_t seam = 1; seam < HDRRoundTrip::kQualityFallbackSeamCount; ++seam) {
			fallbackSeams += fmt::format(
				FMT_STRING(R"({}"{}":{})"),
				seam == 1 ? ""sv : ","sv,
				HDRRoundTrip::QualityFallbackSeamName(static_cast<HDRRoundTrip::QualityFallbackSeam>(seam)),
				isolation.qualityPixelFallbackSeamCounts[seam]);
		}
		out += fmt::format(
			FMT_STRING(R"("qualityPixelTransaction":{{"phase":{},"evaluations":{},"engineCopies":{},"seamPublishes":{},"seamPublishAbortCloses":{},"nativeFallbacks":{},"fallbackLateCloses":{},"fallbackAbortCloses":{},"lastFallbackSeam":{},"lastFallbackSeamName":"{}","fallbackSeams":{{{}}},"dirtyCloses":{},"unrecoverable":{}}},)"),
			isolation.qualityPixelPhase,
			isolation.qualityPixelEvaluations,
			isolation.qualityPixelCopies,
			isolation.qualityPixelSeamPublishes,
			isolation.qualityPixelSeamPublishAbortCloses,
			isolation.qualityPixelNativeFallbacks,
			isolation.qualityPixelFallbackLateCloses,
			isolation.qualityPixelFallbackAbortCloses,
			isolation.qualityPixelLastFallbackSeam,
			HDRRoundTrip::QualityFallbackSeamName(static_cast<HDRRoundTrip::QualityFallbackSeam>(isolation.qualityPixelLastFallbackSeam)),
			fallbackSeams,
			isolation.qualityPixelDirtyCloses,
			isolation.qualityPixelUnrecoverable);
		out += fmt::format(
			FMT_STRING(R"("qualityFaultInjection":{{"armedSeam":{},"armedName":"{}","remaining":{},"lastTriggeredSeam":{},"lastTriggeredName":"{}","armAttempts":{},"arms":{},"rejections":{},"triggers":{},"captureDisableAtTransactions":{}}},)"),
			isolation.qualityFaultArmedSeam,
			HDRRoundTrip::QualityFaultName(static_cast<HDRRoundTrip::QualityFaultSeam>(isolation.qualityFaultArmedSeam)),
			isolation.qualityFaultRemaining,
			isolation.qualityFaultLastTriggeredSeam,
			HDRRoundTrip::QualityFaultName(static_cast<HDRRoundTrip::QualityFaultSeam>(isolation.qualityFaultLastTriggeredSeam)),
			isolation.qualityFaultArmAttempts,
			isolation.qualityFaultArms,
			isolation.qualityFaultRejections,
			isolation.qualityFaultTriggers,
			isolation.qualityCaptureDisableAtTransactions);
		out += fmt::format(
			FMT_STRING(R"("sessionRecovery":{{"state":"{}","failSoft":{},"suspendFramesLeft":{},"rearmCount":{},"rearmBudget":{},"cooldownFrames":{},"suspensionCount":{},"recoveryRefusal":"{}","taaReplacementSessionDisabled":{},"taaReplacementDisableReason":"{}","dynamicGeometrySessionDisabled":{},"dynamicGeometryDisableReason":"{}"}},)"),
			HDRRoundTrip::SessionStateName(static_cast<HDRRoundTrip::SessionState>(isolation.sessionState)),
			isolation.failSoft ? "true"sv : "false"sv,
			isolation.suspendFramesLeft,
			isolation.rearmCount,
			HDRRoundTrip::kRearmsPerSession,
			HDRRoundTrip::kRearmCooldownFrames,
			isolation.suspensionCount,
			HDRRoundTrip::RecoveryRefusalName(static_cast<HDRRoundTrip::RecoveryRefusal>(isolation.recoveryRefusal)),
			isolation.taaReplacementSessionDisabled ? "true"sv : "false"sv,
			isolation.taaReplacementDisableReason,
			isolation.dynamicGeometrySessionDisabled ? "true"sv : "false"sv,
			isolation.dynamicGeometryDisableReason);
		out += fmt::format(
			FMT_STRING(R"("preHdrNgxCreate":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"failures":{},"skips":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ngxCreateEnabled ? "true"sv : "false"sv,
			isolation.ngxCreateCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ngxCreateAttempts,
			isolation.ngxCreateSuccesses,
			isolation.ngxCreateFailures,
			isolation.ngxCreateSkips,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		out += fmt::format(
			FMT_STRING(R"("preHdrNgxEvaluate":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"failures":{},"skips":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ngxEvaluateEnabled ? "true"sv : "false"sv,
			isolation.ngxEvaluateCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ngxEvaluateAttempts,
			isolation.ngxEvaluateSuccesses,
			isolation.ngxEvaluateFailures,
			isolation.ngxEvaluateSkips,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		out += fmt::format(
			FMT_STRING(R"("preHdrFfxEvaluate":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"failures":{},"skips":{},"teardownPending":{},"teardownCompleted":{},"teardownAttempts":{},"teardownSuccesses":{},"teardownFailures":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ffxEvaluateEnabled ? "true"sv : "false"sv,
			isolation.ffxEvaluateCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ffxEvaluateAttempts,
			isolation.ffxEvaluateSuccesses,
			isolation.ffxEvaluateFailures,
			isolation.ffxEvaluateSkips,
			isolation.ffxTeardownPending ? "true"sv : "false"sv,
			isolation.ffxTeardownCompleted ? "true"sv : "false"sv,
			isolation.ffxTeardownAttempts,
			isolation.ffxTeardownSuccesses,
			isolation.ffxTeardownFailures,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		out += fmt::format(
			FMT_STRING(R"("preHdrFfxReassemble":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"failures":{},"skips":{},"privateReassemblySerial":{},"teardownPending":{},"teardownCompleted":{},"teardownAttempts":{},"teardownSuccesses":{},"teardownFailures":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ffxReassembleEnabled ? "true"sv : "false"sv,
			isolation.ffxReassembleCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ffxReassembleAttempts,
			isolation.ffxReassembleSuccesses,
			isolation.ffxReassembleFailures,
			isolation.ffxReassembleSkips,
			isolation.privateReassemblySerial,
			isolation.ffxTeardownPending ? "true"sv : "false"sv,
			isolation.ffxTeardownCompleted ? "true"sv : "false"sv,
			isolation.ffxTeardownAttempts,
			isolation.ffxTeardownSuccesses,
			isolation.ffxTeardownFailures,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		out += fmt::format(
			FMT_STRING(R"("preHdrFfxHandback":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"failures":{},"skips":{},"copies":{},"privateReassemblySerial":{},"teardownPending":{},"teardownCompleted":{},"teardownAttempts":{},"teardownSuccesses":{},"teardownFailures":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ffxHandbackEnabled ? "true"sv : "false"sv,
			isolation.ffxHandbackCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ffxHandbackAttempts,
			isolation.ffxHandbackSuccesses,
			isolation.ffxHandbackFailures,
			isolation.ffxHandbackSkips,
			isolation.ffxHandbackCopies,
			isolation.privateReassemblySerial,
			isolation.ffxTeardownPending ? "true"sv : "false"sv,
			isolation.ffxTeardownCompleted ? "true"sv : "false"sv,
			isolation.ffxTeardownAttempts,
			isolation.ffxTeardownSuccesses,
			isolation.ffxTeardownFailures,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		out += fmt::format(
			FMT_STRING(R"("preHdrFfxContinuous":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"warmupSuccesses":{},"warmupTarget":{},"successes":{},"targetSuccesses":{},"failures":{},"skips":{},"copies":{},"teardownPending":{},"teardownCompleted":{},"teardownAttempts":{},"teardownSuccesses":{},"teardownFailures":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ffxContinuousEnabled ? "true"sv : "false"sv,
			isolation.ffxContinuousCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ffxContinuousAttempts,
			isolation.ffxContinuousWarmupSuccesses,
			isolation.ffxContinuousWarmupTarget,
			isolation.ffxContinuousSuccesses,
			isolation.ffxContinuousTargetSuccesses,
			isolation.ffxContinuousFailures,
			isolation.ffxContinuousSkips,
			isolation.ffxContinuousCopies,
			isolation.ffxTeardownPending ? "true"sv : "false"sv,
			isolation.ffxTeardownCompleted ? "true"sv : "false"sv,
			isolation.ffxTeardownAttempts,
			isolation.ffxTeardownSuccesses,
			isolation.ffxTeardownFailures,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		out += fmt::format(
			FMT_STRING(R"("preHdrNgxReassemble":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"failures":{},"skips":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ngxReassembleEnabled ? "true"sv : "false"sv,
			isolation.ngxReassembleCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ngxReassembleAttempts,
			isolation.ngxReassembleSuccesses,
			isolation.ngxReassembleFailures,
			isolation.ngxReassembleSkips,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		out += fmt::format(
			FMT_STRING(R"("preHdrNgxHandback":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"failures":{},"skips":{},"copies":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ngxHandbackEnabled ? "true"sv : "false"sv,
			isolation.ngxHandbackCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ngxHandbackAttempts,
			isolation.ngxHandbackSuccesses,
			isolation.ngxHandbackFailures,
			isolation.ngxHandbackSkips,
			isolation.ngxHandbackCopies,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		out += fmt::format(
			FMT_STRING(R"("preHdrNgxContinuous":{{"enabled":{},"completed":{},"sessionDisabled":{},"attempts":{},"warmupSuccesses":{},"warmupTarget":{},"successes":{},"targetSuccesses":{},"failures":{},"skips":{},"copies":{},"qualityCandidatePending":{},"qualityCandidates":{},"qualityCommits":{},"qualityAborts":{},"handbackCandidateSerial":{},"teardownPending":{},"teardownCompleted":{},"teardownAttempts":{},"teardownSuccesses":{},"teardownFailures":{},"deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","lastExceptionCode":"{:08X}"}},)"),
			isolation.ngxContinuousEnabled ? "true"sv : "false"sv,
			isolation.ngxContinuousCompleted ? "true"sv : "false"sv,
			isolation.sessionDisabled ? "true"sv : "false"sv,
			isolation.ngxContinuousAttempts,
			isolation.ngxContinuousWarmupSuccesses,
			isolation.ngxContinuousWarmupTarget,
			isolation.ngxContinuousSuccesses,
			isolation.ngxContinuousTargetSuccesses,
			isolation.ngxContinuousFailures,
			isolation.ngxContinuousSkips,
			isolation.ngxContinuousCopies,
			isolation.ngxQualityCandidatePending ? "true"sv : "false"sv,
			isolation.ngxQualityCandidates,
			isolation.ngxQualityCommits,
			isolation.ngxQualityAborts,
			isolation.ngxHandbackCandidateSerial,
			isolation.ngxTeardownPending ? "true"sv : "false"sv,
			isolation.ngxTeardownCompleted ? "true"sv : "false"sv,
			isolation.ngxTeardownAttempts,
			isolation.ngxTeardownSuccesses,
			isolation.ngxTeardownFailures,
			isolation.deviceRemovedReason,
			static_cast<std::uint32_t>(isolation.deviceRemovedReason),
			isolation.lastExceptionCode);
		const auto taaReplacement = TAAReplacement::Snapshot();
		out += fmt::format(
			FMT_STRING(R"("nativeTaaReplacement":{{"enabled":{},"postDrawEnabled":{},"bypassEnabled":{},"requireNgxHandback":{},"requireFfxHandback":{},"handbackSource":"{}","currentHandbackSource":"{}","ready":{},"completed":{},"sessionDisabled":{},"attempts":{},"successes":{},"targetSuccesses":{},"noHandbackSkips":{},"handbackSourceMismatches":{},"handbackSequenceGaps":{},"handbackBaseline":{},"validationFailures":{},"isolationFailures":{},"validationMask":{},"validationMaskHex":"{:08X}","shaderBuilds":{},"history61Writes":{},"history62Writes":{},"postDrawOverwrites":{},"nativeDrawBypasses":{},"qualityPublishDraws":{},"qualityHistoryRepairArms":{},"qualityHistoryRepairAttempts":{},"qualityHistoryRepairCopies":{},"qualityHistoryRepairSuccesses":{},"qualityHistoryRepairFailures":{},"qualityNativeFallbackReleases":{},"qualityNativeFallbackDraws":{},"qualityNativeFallbackHistory61Writes":{},"qualityNativeFallbackHistory62Writes":{},"qualityNativeFallbackHistoryMismatches":{},"parityReadAttempts":{},"parityReadFailures":{},"parityInvalidValues":{},"currentRenderTargetMismatches":{},"lastParity":{},"expectedHistoryLogical":{},"engineHistoryLogical":{},"lastTemporalAACall":{},"lastHandbackSerial":{},"currentHandbackSerial":{},"width":{},"height":{},"sourceFormat":{},"historyFormat":{},"outputFormat":{},"lastError":{},"lastErrorHex":"{:08X}","deviceRemovedReason":{},"deviceRemovedReasonHex":"{:08X}","context":"{:016X}","sourceSrv":"{:016X}","historyRtv":"{:016X}","outputRtv":"{:016X}","parityAddress":"{:016X}","qualitySeamDrawCloseFailureAtPublish":{},"sharpen":{{"requested":{:.6f},"effective":{:.6f},"stops":{:.6f},"shader":"{}","compileFailures":{},"lastHr":{},"lastHrHex":"{:08X}"}}}},)"),
			taaReplacement.enabled ? "true"sv : "false"sv,
			taaReplacement.postDrawEnabled ? "true"sv : "false"sv,
			taaReplacement.bypassEnabled ? "true"sv : "false"sv,
			taaReplacement.requireNgxHandback ? "true"sv : "false"sv,
			taaReplacement.requireFfxHandback ? "true"sv : "false"sv,
			HDRRoundTrip::HandbackSourceName(taaReplacement.handbackSource),
			HDRRoundTrip::HandbackSourceName(taaReplacement.currentHandbackSource),
			taaReplacement.ready ? "true"sv : "false"sv,
			taaReplacement.completed ? "true"sv : "false"sv,
			taaReplacement.sessionDisabled ? "true"sv : "false"sv,
			taaReplacement.attempts,
			taaReplacement.successes,
			taaReplacement.targetSuccesses,
			taaReplacement.noHandbackSkips,
			taaReplacement.handbackSourceMismatches,
			taaReplacement.handbackSequenceGaps,
			taaReplacement.handbackBaseline,
			taaReplacement.validationFailures,
			taaReplacement.isolationFailures,
			taaReplacement.validationMask,
			taaReplacement.validationMask,
			taaReplacement.shaderBuilds,
			taaReplacement.history61Writes,
			taaReplacement.history62Writes,
			taaReplacement.postDrawOverwrites,
			taaReplacement.nativeDrawBypasses,
			taaReplacement.qualityPublishDraws,
			taaReplacement.qualityHistoryRepairArms,
			taaReplacement.qualityHistoryRepairAttempts,
			taaReplacement.qualityHistoryRepairCopies,
			taaReplacement.qualityHistoryRepairSuccesses,
			taaReplacement.qualityHistoryRepairFailures,
			taaReplacement.qualityNativeFallbackReleases,
			taaReplacement.qualityNativeFallbackDraws,
			taaReplacement.qualityNativeFallbackHistory61Writes,
			taaReplacement.qualityNativeFallbackHistory62Writes,
			taaReplacement.qualityNativeFallbackHistoryMismatches,
			taaReplacement.parityReadAttempts,
			taaReplacement.parityReadFailures,
			taaReplacement.parityInvalidValues,
			taaReplacement.currentRenderTargetMismatches,
			taaReplacement.lastParity,
			taaReplacement.expectedHistoryLogical,
			taaReplacement.engineHistoryLogical,
			taaReplacement.lastTemporalAACall,
			taaReplacement.lastHandbackSerial,
			taaReplacement.currentHandbackSerial,
			taaReplacement.width,
			taaReplacement.height,
			taaReplacement.sourceFormat,
			taaReplacement.historyFormat,
			taaReplacement.outputFormat,
			taaReplacement.lastError,
			taaReplacement.lastError,
			taaReplacement.deviceRemovedReason,
			static_cast<std::uint32_t>(taaReplacement.deviceRemovedReason),
			taaReplacement.context,
			taaReplacement.sourceSrv,
			taaReplacement.historyRtv,
			taaReplacement.outputRtv,
			taaReplacement.parityAddress,
			taaReplacement.qualitySeamDrawCloseFailureAtPublish ? "true"sv : "false"sv,
			taaReplacement.sharpnessRequested,
			taaReplacement.sharpnessEffective,
			taaReplacement.sharpenStops,
			TAAReplacement::SeamShaderName(taaReplacement.sharpenShader),
			taaReplacement.sharpenCompileFailures,
			taaReplacement.sharpenLastHr,
			taaReplacement.sharpenLastHr);
		const auto resetPolicy = ResetPolicy::Snapshot();
		out += fmt::format(
			FMT_STRING(R"("resetPolicy":{{"pending":{},"menuSinkInstalled":{},"cellSinkInstalled":{},"resetOnMenu":{},"resetOnLoad":{},"resetOnCell":{},"requests":{},"coalesced":{},"consumptions":{},"requestedGeneration":{},"acknowledgedGeneration":{},"acquisitions":{},"qualityAbortRequests":{},"loadEvents":{},"menuOpenEvents":{},"menuCloseEvents":{},"cellEnterEvents":{},"cellLeaveEvents":{},"lastCellId":{},"leftSubmissions":{},"rightSubmissions":{}}},)"),
			resetPolicy.pending ? "true"sv : "false"sv,
			resetPolicy.menuSinkInstalled ? "true"sv : "false"sv,
			resetPolicy.cellSinkInstalled ? "true"sv : "false"sv,
			resetPolicy.resetOnMenu ? "true"sv : "false"sv,
			resetPolicy.resetOnLoad ? "true"sv : "false"sv,
			resetPolicy.resetOnCell ? "true"sv : "false"sv,
			resetPolicy.requests,
			resetPolicy.coalesced,
			resetPolicy.consumptions,
			resetPolicy.requestedGeneration,
			resetPolicy.acknowledgedGeneration,
			resetPolicy.acquisitions,
			resetPolicy.qualityAbortRequests,
			resetPolicy.loadEvents,
			resetPolicy.menuOpenEvents,
			resetPolicy.menuCloseEvents,
			resetPolicy.cellEnterEvents,
			resetPolicy.cellLeaveEvents,
			resetPolicy.lastCellId,
			resetPolicy.leftSubmissions,
			resetPolicy.rightSubmissions);
		out += fmt::format(
			FMT_STRING(R"("worldAdmission":{{"phase":"{}","reason":"{}","admitted":{},"stableFrames":{},"stableFramesRequired":{},"lastActivationStableFrames":{},"transitionGeneration":{},"cell":"{:016X}","checks":{},"deniedFrames":{},"activations":{},"invalidations":{},"resetRequests":{},"rechecks":{},"recheckFailures":{},"publishRechecks":{},"publishVetoes":{},"menuMode":{},"itemMenuMode":{},"freezeFrameMenuBG":{},"freezeFramePause":{},"closingAllMenus":{},"openBlockingMenuMask":{},"operatorVeto":{{"active":{},"engagements":{},"releases":{},"deniedFrames":{}}}}},)"),
			ResetPolicy::WorldAdmissionPhaseName(resetPolicy.worldAdmissionPhase),
			ResetPolicy::WorldBlockReasonName(resetPolicy.worldBlockReason),
			resetPolicy.worldAdmitted ? "true"sv : "false"sv,
			resetPolicy.worldStableFrames,
			resetPolicy.worldStableFramesRequired,
			resetPolicy.worldLastActivationStableFrames,
			resetPolicy.worldTransitionGeneration,
			resetPolicy.worldCell,
			resetPolicy.worldAdmissionChecks,
			resetPolicy.worldAdmissionDeniedFrames,
			resetPolicy.worldAdmissionActivations,
			resetPolicy.worldAdmissionInvalidations,
			resetPolicy.worldAdmissionResetRequests,
			resetPolicy.worldAdmissionRechecks,
			resetPolicy.worldAdmissionRecheckFailures,
			resetPolicy.worldAdmissionPublishRechecks,
			resetPolicy.worldAdmissionPublishVetoes,
			resetPolicy.worldMenuMode,
			resetPolicy.worldItemMenuMode,
			resetPolicy.worldFreezeFrameMenuBG,
			resetPolicy.worldFreezeFramePause,
			resetPolicy.worldClosingAllMenus ? "true"sv : "false"sv,
			resetPolicy.worldOpenBlockingMenuMask,
			resetPolicy.operatorVetoActive ? "true"sv : "false"sv,
			resetPolicy.operatorVetoEngagements,
			resetPolicy.operatorVetoReleases,
			resetPolicy.operatorVetoDeniedFrames);
		const auto scopePolicy = ScopePolicy::Snapshot();
		out += fmt::format(
			FMT_STRING(R"("scopePolicy":{{"suppressDuringScopeRender":{},"rendererReadAttempts":{},"rendererReadSuccesses":{},"rendererReadFailures":{},"scopedDispatches":{},"suppressedDispatches":{}}},)"),
			scopePolicy.suppressDuringScopeRender ? "true"sv : "false"sv,
			scopePolicy.rendererReadAttempts,
			scopePolicy.rendererReadSuccesses,
			scopePolicy.rendererReadFailures,
			scopePolicy.scopedDispatches,
			scopePolicy.suppressedDispatches);
		out += fmt::format(
			FMT_STRING(R"("stereoNullEvaluate":{{"prepared":{},"active":{},"ownsBackendState":{},"attempts":{},"successes":{},"failures":{},"leftEvaluates":{},"rightEvaluates":{},"leftReassemblies":{},"rightReassemblies":{},"prepareCount":{},"autoExposureSubmissions":{},"exposureContractFailures":{},"frameTimeSamples":{},"frameTimeFallbacks":{},"frameTimeDeltaMs":{},"lastFailureStep":{},"backend":"{:016X}","device":"{:016X}","context":"{:016X}"}},)"),
			evaluate.prepared ? "true"sv : "false"sv,
			evaluate.active ? "true"sv : "false"sv,
			evaluate.ownsBackendState ? "true"sv : "false"sv,
			evaluate.attempts,
			evaluate.successes,
			evaluate.failures,
			evaluate.leftEvaluates,
			evaluate.rightEvaluates,
			evaluate.leftReassemblies,
			evaluate.rightReassemblies,
			evaluate.prepareCount,
			evaluate.autoExposureSubmissions,
			evaluate.exposureContractFailures,
			evaluate.frameTimeSamples,
			evaluate.frameTimeFallbacks,
			evaluate.frameTimeDeltaMs,
			static_cast<std::uint32_t>(evaluate.lastFailureStep),
			evaluate.backend,
			evaluate.device,
			evaluate.context);
		out += fmt::format(
			FMT_STRING(R"("jitter":{{"resolved":{},"backendSequenceArmed":{},"state":"{:016X}","readAttempts":{},"readSuccesses":{},"readFailures":{},"writeAttempts":{},"writeSuccesses":{},"writeMismatches":{},"writeGateDeclines":{},"sequenceRestarts":{},"leftSubmissions":{},"rightSubmissions":{},"stereoMatches":{},"stereoMismatches":{},"phaseCount":{},"phase":{},"enginePhase":{},"distinctPhasesSeen":{},"observedPhaseMaskLow":{},"observedPhaseMaskHigh":{},"engineWidth":{},"engineHeight":{},"enabled":{},"disableCount":{},"projectionX":{},"projectionY":{},"committedProjectionX":{},"committedProjectionY":{},"recoveredJitterX":{},"recoveredJitterY":{},"jitterX":{},"jitterY":{},"minJitterX":{},"maxJitterX":{},"minJitterY":{},"maxJitterY":{},"leftJitterX":{},"leftJitterY":{},"rightJitterX":{},"rightJitterY":{},"lastFailure":{},"lastWriteFailure":{}}},)"),
			jitter.resolved ? "true"sv : "false"sv,
			jitter.backendSequenceArmed ? "true"sv : "false"sv,
			jitter.address,
			jitter.readAttempts,
			jitter.readSuccesses,
			jitter.readFailures,
			jitter.writeAttempts,
			jitter.writeSuccesses,
			jitter.writeMismatches,
			jitter.writeGateDeclines,
			jitter.sequenceRestarts,
			jitter.leftSubmissions,
			jitter.rightSubmissions,
			jitter.stereoMatches,
			jitter.stereoMismatches,
			jitter.phaseCount,
			jitter.phase,
			jitter.enginePhase,
			jitter.distinctPhasesSeen,
			jitter.observedPhaseMask[0],
			jitter.observedPhaseMask[1],
			jitter.engineWidth,
			jitter.engineHeight,
			jitter.enabled,
			jitter.disableCount,
			JsonFloat(jitter.projectionX),
			JsonFloat(jitter.projectionY),
			JsonFloat(jitter.committedProjectionX),
			JsonFloat(jitter.committedProjectionY),
			// The committed floats run back through the FULL per-eye extent. These must
			// equal jitterX/jitterY at every live dynamic-resolution ratio; that equality
			// is the ratio-cancellation proof in a form a headless run can assert on.
			JsonFloat(jitter.engineWidth != 0 ? Jitter::ProjectionToCameraPixelsX(jitter.committedProjectionX, jitter.engineWidth) : 0.0f),
			JsonFloat(jitter.engineHeight != 0 ? Jitter::ProjectionToCameraPixelsY(jitter.committedProjectionY, jitter.engineHeight) : 0.0f),
			JsonFloat(jitter.jitterX),
			JsonFloat(jitter.jitterY),
			JsonFloat(jitter.minJitterX),
			JsonFloat(jitter.maxJitterX),
			JsonFloat(jitter.minJitterY),
			JsonFloat(jitter.maxJitterY),
			JsonFloat(jitter.leftJitterX),
			JsonFloat(jitter.leftJitterY),
			JsonFloat(jitter.rightJitterX),
			JsonFloat(jitter.rightJitterY),
			static_cast<std::uint32_t>(jitter.lastFailure),
			static_cast<std::uint32_t>(jitter.lastWriteFailure));

		const auto mip = MipBias::Snapshot();
		out += fmt::format(
			FMT_STRING(R"("mipBias":{{"anchorsResolved":{},"tableAddress":"{:016X}","tableRva":"{:08X}","anchorsAgreed":{},"nativeApplyActive":{},"nativeApplies":{},"originalsCached":{},"filterHistogram":[{},{},{},{},{}],"filterOther":{},"entriesTotal":{},"entriesBiased":{},"samplersCreated":{},"retainedSamplers":{},"built":{},"builtBias":{},"guideBandLow":{},"guideBandHigh":{},"builtRenderWidth":{},"builtOutputWidth":{},"overrideActive":{},"includeTrilinear":{},"lowerMinLod":{},"applied":{},"appliedFrames":{},"restoredFrames":{},"applyDeclines":{},"unbalanced":{},"bandWarnings":{},"builds":{},"buildDeclines":{},"lastFailure":{}}},)"),
			mip.anchorsResolved ? "true"sv : "false"sv,
			mip.tableAddress,
			mip.tableAddress != 0 ? FaultGuard::ToRva(mip.tableAddress) : 0,
			mip.anchorsAgreed ? "true"sv : "false"sv,
			mip.nativeApplyActive ? "true"sv : "false"sv,
			mip.nativeApplies,
			mip.table.originalsCached ? "true"sv : "false"sv,
			mip.table.filterHistogram[0],
			mip.table.filterHistogram[1],
			mip.table.filterHistogram[2],
			mip.table.filterHistogram[3],
			mip.table.filterHistogram[4],
			mip.table.filterOther,
			mip.table.entriesTotal,
			mip.table.entriesBiased,
			mip.table.samplersCreated,
			mip.table.retainedSamplers,
			mip.table.built ? "true"sv : "false"sv,
			JsonFloat(mip.table.builtBias),
			JsonFloat(mip.table.guideBandLow),
			JsonFloat(mip.table.guideBandHigh),
			mip.table.builtRenderWidth,
			mip.table.builtOutputWidth,
			mip.table.overrideActive ? "true"sv : "false"sv,
			mip.table.includeTrilinear ? "true"sv : "false"sv,
			mip.table.lowerMinLod ? "true"sv : "false"sv,
			mip.table.applied ? "true"sv : "false"sv,
			mip.table.appliedFrames,
			mip.table.restoredFrames,
			mip.table.applyDeclines,
			mip.table.unbalanced,
			mip.table.bandWarnings,
			mip.table.builds,
			mip.table.buildDeclines,
			static_cast<std::uint32_t>(mip.table.lastFailure));
		// Requested is the setting, effective is what the session runs; a run that
		// timed the wrong path is caught here rather than by reading the copy intervals.
		out += fmt::format(
			FMT_STRING(R"("stereoMode":{{"requested":"{}","effective":"{}","backend":"{}"}},)"),
			isolation.stereoSubrectsRequested ? "subrect"sv : "intermediates"sv,
			isolation.stereoSubrectsEffective ? "subrect"sv : "intermediates"sv,
			HDRRoundTrip::StereoBackendName(static_cast<HDRRoundTrip::StereoBackend>(isolation.stereoBackend)));
		out += fmt::format(
			FMT_STRING(R"("intermediates":{{"ready":{},"subrects":{},"resourceBuilds":{},"motionSanitizerBuilds":{},"motionSanitizerDraws":{},"motionCompletionDraws":{},"motionFallbackDraws":{},"lastError":{},"lastErrorHex":"{:08X}","renderWidth":{},"renderHeight":{},"outputWidth":{},"outputHeight":{},"dxgiFormat":{},"motionDxgiFormat":{},"leftColor":"{:016X}","rightColor":"{:016X}","leftMotionVectors":"{:016X}","rightMotionVectors":"{:016X}","leftOutput":"{:016X}","rightOutput":"{:016X}","colorRetentions":{},"leftRetainedColor":"{:016X}","rightRetainedColor":"{:016X}"}},)"),
			intermediates.ready ? "true"sv : "false"sv,
			intermediates.subrects ? "true"sv : "false"sv,
			intermediates.resourceBuilds,
			intermediates.motionSanitizerBuilds,
			intermediates.motionSanitizerDraws,
			intermediates.motionCompletionDraws,
			intermediates.motionFallbackDraws,
			intermediates.lastError,
			static_cast<std::uint32_t>(intermediates.lastError),
			intermediates.renderWidth,
			intermediates.renderHeight,
			intermediates.outputWidth,
			intermediates.outputHeight,
			intermediates.format,
			intermediates.motionFormat,
			intermediates.leftColor,
			intermediates.rightColor,
			intermediates.leftMotionVectors,
			intermediates.rightMotionVectors,
			intermediates.leftOutput,
			intermediates.rightOutput,
			intermediates.colorRetentions,
			intermediates.leftRetainedColor,
			intermediates.rightRetainedColor);
		out += fmt::format(
			FMT_STRING(R"("cameraMotion":{{"attempts":{},"successes":{},"failures":{},"staging":"{:016X}","lastFailure":{}}},)"),
			cameraMotion.attempts,
			cameraMotion.successes,
			cameraMotion.failures,
			cameraMotion.staging,
			static_cast<std::uint32_t>(cameraMotion.lastFailure));

		const auto observation = Debug::BindingObservationSnapshot();
		out += fmt::format(
			FMT_STRING(R"("bindingObservation":{{"requestedSequence":{},"completedSequence":{},"pending":{},"captured":{},"point":{},"clearCall":{},"hdrCall":{},"temporalAACall":{},"threadId":{},"renderer":"{:016X}","context":"{:016X}","currentColor":{},"currentDepth":{},"motionVectors":{},"linearDepth":{},"omRenderTargetCount":{},"currentColorOmSlot":{},"currentDepthOmView":{},"omBindings":{},"psSrvCount":{},"motionVectorsPsSlot":{},"linearDepthPsSlot":{},"currentDepthPsSlot":{},"psBindings":{}}},)"),
			observation.requestedSequence,
			observation.completedSequence,
			observation.pending ? "true"sv : "false"sv,
			observation.captured ? "true"sv : "false"sv,
			Quote(Debug::ObservationPointName(observation.point)),
			observation.clearCall,
			observation.hdrCall,
			observation.temporalAACall,
			observation.threadId,
			observation.renderer,
			observation.context,
			ObservedTargetJson(observation.currentColor),
			ObservedTargetJson(observation.currentDepth),
			ObservedTargetJson(observation.motionVectors),
			ObservedTargetJson(observation.linearDepth),
			observation.omRenderTargetCount,
			observation.currentColorOmSlot,
			Quote(DepthViewName(observation.currentDepthOmView)),
			BoundSlotsJson(observation.omBindings),
			observation.psSrvCount,
			observation.motionVectorsPsSlot,
			observation.linearDepthPsSlot,
			observation.currentDepthPsSlot,
			BoundSlotsJson(observation.psBindings));
		out += fmt::format(FMT_STRING(R"("hooksInstalled":{},"hookSites":{},"backendSelection":{},"backends":{})"),
			f.hooksInstalled ? "true"sv : "false"sv, f.hookSites, Quote(f.selection), backends);
		out += '}';
		return out;
	}
}
