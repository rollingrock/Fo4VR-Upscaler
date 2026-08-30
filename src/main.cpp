#include "DevBench/DevBench.h"
#include "DevBenchClient/DevBenchClient.h"
#include "Settings/Settings.h"
#include "Upscaler/Backend/BackendRegistry.h"
#include "Upscaler/Engine.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/SelfCheck.h"
#include "Upscaler/StereoLayout.h"

#include <ShlObj.h>

#include <filesystem>
#include <system_error>

namespace
{
	// Documents/My Games/Fallout4VR/F4SE, built here rather than through
	// F4SE::log::log_directory: that helper reads F4SE::GetSaveFolderName, which
	// is empty until F4SE::Init has run in Load, and the refusals below are
	// written from Query.
	[[nodiscard]] std::filesystem::path LogDirectory()
	{
		PWSTR raw = nullptr;
		if (::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw) != S_OK) {
			return {};
		}
		std::filesystem::path path{ raw };
		::CoTaskMemFree(raw);
		return path / L"My Games" / L"Fallout4VR" / L"F4SE";
	}

	// Called from both entry points; the second call is a no-op.
	void InitializeLog()
	{
		static bool done = false;
		if (done) {
			return;
		}
		done = true;

		auto path = LogDirectory();
		if (path.empty()) {
			// Broken install (no Documents redirection?): run without a log file
			// rather than CTD before logging exists.
			return;
		}

		std::error_code ec;
		std::filesystem::create_directories(path, ec);
		path /= L"Fo4VRUpscaler.log";

		try {
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);

			// Bootstrap level. The TOML has not been parsed yet, so everything up
			// to Settings::load is written and ApplyLogLevel narrows it after.
			// flush_on matches: a CTD must not lose the last lines.
			const auto level = spdlog::level::trace;

			auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
			log->set_level(level);
			log->flush_on(level);

			spdlog::set_default_logger(std::move(log));
			spdlog::set_pattern("[%Y-%m-%d %T.%e][%-16s:%-4#][%L]: %v"s);
		} catch (const std::exception&) {
			// Read-only or redirected Documents. Stay logless rather than throw
			// out of an F4SE entry point.
		}
	}

	void MessageHandler(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		// kGameLoaded, not kGameDataReady: F4SEVR never dispatches the latter
		// (field-observed message sequence is 0, 1, 9). It repeats across save
		// loads, which is what makes it the live-reload trigger - load() is
		// idempotent and re-applies any dev-tooling overrides through
		// Settings::postLoadHook.
		if (a_msg->type == F4SE::MessagingInterface::kGameLoaded ||
			a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			Settings::load();
			Upscaler::HDRRoundTrip::Configure(*Settings::diagnosticHdrRoundTrip, *Settings::diagnosticStereoNull, *Settings::diagnosticNgxCreate, *Settings::diagnosticNgxEvaluate, *Settings::diagnosticNgxReassemble, *Settings::diagnosticNgxHandback, *Settings::diagnosticNgxContinuous, *Settings::runtimeDir);
			static_cast<void>(Upscaler::StereoLayout::Resolve());
			// The two facts the load-time block could not answer. One-shot, so it costs
			// nothing on the repeat dispatches that make load() the reload edge.
			Upscaler::SelfCheck::RunDeferred();
		}
		// Register our tools into alandtse/devbench at kPostPostLoad, not kPostLoad.
		// F4SEVR's RegisterListener(sender = nullptr) snapshots the listener slots that
		// exist at that moment and Dispatch only walks the sender's own slot, so a plugin
		// loading after devbench can dispatch into an empty list. devbench re-registers at
		// kPostLoad once the plugin list is complete; kPostPostLoad is dispatched right
		// after kPostLoad for exactly this second-phase handshake and does not depend on
		// load order. A no-op if devbench is absent.
		if (a_msg->type == F4SE::MessagingInterface::kPostPostLoad) {
			// Registration itself is coexistence-neutral. SelfCheck reports interacting
			// renderer modules; the byte-verified hooks preserve vanilla, and the only
			// mutation is the explicit bounded diagnostic guarded at the HDR callback.
			if (*Settings::devBench) {
				DevBenchClient::Register();
			} else {
				logger::info("devBench=false - not registering into the dev tooling host"sv);
			}
		}
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	// Before the refusals below, or they land on spdlog's default stdout logger
	// and are invisible in a GUI process - in exactly the case where the reason
	// matters most.
	InitializeLog();

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	// Pack MAJOR.MINOR.PATCH decimally (0.1.0 -> 1000) so Buffout/crash logs
	// name the real build instead of 0.
	a_info->version = Version::MAJOR * 1000000 + Version::MINOR * 1000 + Version::PATCH;

	if (!REL::Module::IsVR()) {
		logger::critical("Fo4VR Upscaler requires Fallout 4 VR - unsupported binary, plugin disabled itself"sv);
		return false;
	}

	if (a_f4se->IsEditor()) {
		logger::critical("Loaded in editor, marking as incompatible"sv);
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < (REL::Module::IsF4() ? F4SE::RUNTIME_LATEST : F4SE::RUNTIME_LATEST_VR)) {
		logger::critical(FMT_STRING("Unsupported runtime version {}"), ver.string());
		return false;
	}

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	InitializeLog();
	// The false is a_log: it stops CommonLib installing its own logger over ours.
	F4SE::Init(a_f4se, false);

	logger::info(FMT_STRING("{} v{}.{}.{} {} {} is loading"), Version::PROJECT, Version::MAJOR, Version::MINOR, Version::PATCH, __DATE__, __TIME__);
	const auto runtimeVer = REL::Module::get().version();
	logger::info(FMT_STRING("Fallout 4 VR v{}.{}.{}"), runtimeVer[0], runtimeVer[1], runtimeVer[2]);

	if (!REL::Module::IsVR()) {
		// Query already refuses a non-VR binary. Repeated here because every
		// address, render target and stereo assumption below is VR-only, and a
		// loader that ignores the Query verdict must not reach any of them.
		logger::critical("Not a Fallout 4 VR binary - nothing installed"sv);
		return false;
	}

	Settings::load();
	Upscaler::HDRRoundTrip::Configure(*Settings::diagnosticHdrRoundTrip, *Settings::diagnosticStereoNull, *Settings::diagnosticNgxCreate, *Settings::diagnosticNgxEvaluate, *Settings::diagnosticNgxReassemble, *Settings::diagnosticNgxHandback, *Settings::diagnosticNgxContinuous, *Settings::runtimeDir);
	static_cast<void>(Upscaler::StereoLayout::Resolve());

	// Availability only: there is no device at plugin load, so this answers "is
	// the runtime present at all" and nothing more. Select logs one line per
	// backend and never fails - the null passthrough is the floor - and the
	// self-check reads the verdict back from the registry.
	static_cast<void>(Upscaler::Backend::Select(*Settings::backend, /*a_device=*/nullptr));

	// Address resolution only: it decodes five engine globals out of verified code
	// bytes and makes no D3D11 call, so it is safe here even though the renderer has
	// not started. What those handles point at is checked later, from the deferred
	// self-check, once the render targets exist.
	static_cast<void>(Upscaler::Engine::Init());
	static_cast<void>(Upscaler::Jitter::Init());

	Upscaler::SelfCheck::Run();

	F4SE::GetMessagingInterface()->RegisterListener(MessageHandler);

	// Dev tooling. Failure here is never fatal - the mod works without it.
	DevBench::Init();

	if (!*Settings::enabled) {
		// Deliberately not the reference mod's early return, which would have
		// skipped everything above. The log and the self-check are this build's
		// product, so the kill switch stops hooks and leaves diagnostics live.
		logger::info("enabled=false - no hooks installed, nothing patched; the self-check, the settings routes and the dev tooling stay live. Restart after re-enabling."sv);
		return true;
	}

	// One allocation for all hook stubs and entry gateways - never per-hook, see the
	// write_thunk_call note in PCH.h. The final injection sites are not fixed yet and
	// undersizing silently orphans stubs, so the budget is rounded up.
	F4SE::AllocTrampoline(512);

	if (!Upscaler::Hooks::Install()) {
		logger::critical("hook install failed - plugin inactive"sv);
		return true;  // stay loaded so the log tells the story, but do nothing
	}

	logger::info("Fo4VR Upscaler loaded"sv);
	return true;
}
