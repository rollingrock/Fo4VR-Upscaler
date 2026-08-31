#include "DevBench/DevBench.h"

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <vector>

#include "Settings/Settings.h"
#include "Upscaler/Backend/BackendRegistry.h"
#include "Upscaler/Backend/BackendTypes.h"
#include "Upscaler/Backend/IBackend.h"
#include "Upscaler/Debug.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/SelfCheck.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/StereoCapture.h"
#include "Upscaler/TAAReplacement.h"

namespace DevBench
{
	namespace
	{
		std::uint64_t g_startTick = 0;
		std::atomic<std::uint64_t> g_requests{ 0 };

		// build configuration
		//
		// These come from the compiler, not from a string someone maintains. The
		// backend macros are PUBLIC on upscaler_backends, so the plugin sees the
		// same configuration the backends were built with.
#ifdef NDEBUG
		constexpr std::string_view kBuildConfiguration = "release"sv;
#else
		constexpr std::string_view kBuildConfiguration = "debug"sv;
#endif

		// UPSCALER_NO_NGX is defined when no NGX SDK was staged, in which case the
		// backend compiles as a stub that reports itself unavailable.
#ifdef UPSCALER_NO_NGX
		constexpr bool kNgxCompiled = false;
#else
		constexpr bool kNgxCompiled = true;
#endif

#ifdef UPSCALER_ENABLE_STREAMLINE
		constexpr bool kStreamlineCompiled = true;
#else
		constexpr bool kStreamlineCompiled = false;
#endif

#ifdef UPSCALER_ENABLE_FFX
		constexpr bool kFfxCompiled = true;
#else
		constexpr bool kFfxCompiled = false;
#endif

		// json helpers

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
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
						out += buf;
					} else {
						out += c;
					}
				}
			}
			return out;
		}

		[[nodiscard]] std::string Quote(std::string_view a_in) { return "\"" + JsonEscape(a_in) + "\""; }

		[[nodiscard]] std::string Bool(bool a_v) { return a_v ? "true" : "false"; }

		// JSON has no spelling for a non-finite number. Return the same strings the
		// host's own tools return rather than null, which an agent reads as absent.
		[[nodiscard]] std::string Num(double a_v)
		{
			if (std::isfinite(a_v)) {
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%.6g", a_v);
				return buf;
			}
			if (std::isnan(a_v)) {
				return "\"NaN\"";
			}
			return a_v < 0.0 ? "\"-Inf\"" : "\"Inf\"";
		}

		[[nodiscard]] std::string Err(std::string_view a_msg)
		{
			return "{\"ok\":false,\"error\":" + Quote(a_msg) + "}";
		}

		// Caps carries fixed char buffers rather than std::string, so a backend that
		// fills one to the brim leaves no terminator. Read them bounded.
		template <std::size_t N>
		[[nodiscard]] std::string FromFixed(const char (&a_buf)[N])
		{
			std::size_t n = 0;
			while (n < N && a_buf[n] != '\0') {
				++n;
			}
			return std::string{ a_buf, n };
		}

		// requests

		struct Request
		{
			std::string method;
			std::string path;
			std::vector<std::pair<std::string, std::string>> query;

			[[nodiscard]] const std::string* Get(std::string_view a_key) const
			{
				for (const auto& [k, v] : query) {
					if (a_key == k) {
						return &v;
					}
				}
				return nullptr;
			}

			[[nodiscard]] std::string GetOr(std::string_view a_key, std::string_view a_def) const
			{
				const auto* p = Get(a_key);
				return p ? *p : std::string{ a_def };
			}
		};

		// settings
		//
		// The table is generated from src/Settings/SettingsList.inl - the same
		// X-macro list Settings.h declares the settings from. A key therefore cannot
		// exist in one and be missing from the other, which is the whole reason the
		// list is a separate file: the reference hand-maintained 108 rows next to
		// Settings.h and documented a missing row as "invisible to the bench".

		[[nodiscard]] std::string JsonValue(const Settings::bSetting& a_s) { return Bool(*a_s); }
		[[nodiscard]] std::string JsonValue(const Settings::iSetting& a_s) { return std::to_string(*a_s); }
		[[nodiscard]] std::string JsonValue(const Settings::fSetting& a_s) { return Num(*a_s); }
		[[nodiscard]] std::string JsonValue(const Settings::sSetting& a_s) { return Quote(*a_s); }

		struct SettingRow
		{
			std::string_view name;
			std::string value;  // already JSON-shaped
		};

		// Call with Settings::loadLock held. The lock serialises load() against
		// readers; a string setting reassigned underneath a reader is a torn read,
		// and the listener thread is a different thread from the one that reloads.
		//
		// SettingsList.inl carries no include guard on purpose - it is expanded once
		// by Settings.h to declare the settings and again here to describe them, and
		// a guard would silently make this expansion empty. EmptyTableError below is
		// what turns that into a diagnosis instead of a config route that reports no
		// settings at all.
		[[nodiscard]] std::vector<SettingRow> SettingsSnapshot()
		{
			std::vector<SettingRow> rows;
			// Settings.h expanded the same list with its own definition. Clear it
			// first: a redefinition is a warning, and warnings are errors here.
#undef US_SETTING
#define US_SETTING(a_type, a_key, a_default) rows.push_back(SettingRow{ #a_key, JsonValue(Settings::a_key) });
#include "Settings/SettingsList.inl"
#undef US_SETTING
			return rows;
		}

		[[nodiscard]] std::string EmptyTableError()
		{
			return Err("the generated settings table is empty - src/Settings/SettingsList.inl expanded to nothing here, which is what an include guard on it looks like");
		}

		[[nodiscard]] std::string CaptureJitterPhase(
			const Request& a_req,
			std::uint32_t& a_out)
		{
			a_out = Upscaler::StereoCapture::kAnyJitterPhase;
			const auto* raw = a_req.Get("jitterPhase"sv);
			if (!raw) {
				return {};
			}
			std::uint32_t phase = 0;
			const auto [end, error] = std::from_chars(raw->data(), raw->data() + raw->size(), phase);
			if (error != std::errc{} || end != raw->data() + raw->size() || phase >= Upscaler::Jitter::PhaseCount()) {
				return "jitterPhase must be an integer from 0 through 7";
			}
			a_out = phase;
			return {};
		}

		[[nodiscard]] std::string SettingsObject(const std::vector<SettingRow>& a_rows)
		{
			std::string out = "{";
			bool first = true;
			for (const auto& r : a_rows) {
				if (!first) {
					out += ",";
				}
				first = false;
				out += Quote(r.name) + ":" + r.value;
			}
			out += "}";
			return out;
		}

		[[nodiscard]] std::vector<SettingRow> LockedSnapshot()
		{
			const std::scoped_lock lock(Settings::loadLock);
			return SettingsSnapshot();
		}

		// handlers
		//
		// Every handler takes the Request whether it reads it or not, so the route
		// table can call them uniformly. An unnamed parameter is how a handler says
		// it takes no arguments.

		// Liveness and identity, answered without touching game state - the same
		// reason the host answers its own health check off the main thread. It must
		// still reply when the thing being debugged is wedged.
		[[nodiscard]] std::string HandleHealth(const Request&)
		{
			std::string out = "{\"ok\":true";
			out += ",\"plugin\":" + Quote(Version::PROJECT);
			out += ",\"version\":" + Quote(Version::NAME);
			out += ",\"pid\":" + std::to_string(::GetCurrentProcessId());
			out += ",\"uptimeMs\":" + std::to_string(::GetTickCount64() - g_startTick);
			out += ",\"requests\":" + std::to_string(g_requests.load());
			out += ",\"build\":{\"configuration\":" + Quote(kBuildConfiguration);
			out += ",\"ngx\":" + Bool(kNgxCompiled);
			out += ",\"streamline\":" + Bool(kStreamlineCompiled);
			out += ",\"ffx\":" + Bool(kFfxCompiled);
			out += "}}";
			return out;
		}

		[[nodiscard]] std::string HandleConfig(const Request&)
		{
			const auto rows = LockedSnapshot();
			if (rows.empty()) {
				return EmptyTableError();
			}
			return "{\"ok\":true,\"count\":" + std::to_string(rows.size()) + ",\"settings\":" + SettingsObject(rows) + "}";
		}

		// Re-read the file and say what moved. The before/after pair is the point:
		// "reloaded" on its own does not tell a caller whether the edit landed, and
		// a silent no-op looks identical to a successful reload.
		[[nodiscard]] std::string HandleConfigReload(const Request&)
		{
			const auto before = LockedSnapshot();
			Settings::load();
			Upscaler::HDRRoundTrip::Configure(*Settings::diagnosticHdrRoundTrip, *Settings::diagnosticStereoNull, *Settings::diagnosticNgxCreate, *Settings::diagnosticNgxEvaluate, *Settings::diagnosticNgxReassemble, *Settings::diagnosticNgxHandback, *Settings::diagnosticNgxContinuous, *Settings::runtimeDir);
			Upscaler::TAAReplacement::Configure(*Settings::diagnosticTaaReplacement, *Settings::diagnosticTaaBypass, *Settings::diagnosticNgxContinuous);
			Upscaler::ResetPolicy::Configure(*Settings::resetOnMenu, *Settings::resetOnLoad, *Settings::resetOnCell);
			Upscaler::ScopePolicy::Configure(*Settings::suppressDuringScopeRender);
			static_cast<void>(Upscaler::StereoLayout::Resolve());
			const auto after = LockedSnapshot();
			if (after.empty()) {
				return EmptyTableError();
			}

			// Both snapshots come from the same macro expansion, so the rows line up
			// by index. The name check is belt and braces, not a real case.
			std::string changed;
			std::size_t n = 0;
			const auto count = before.size() < after.size() ? before.size() : after.size();
			for (std::size_t i = 0; i < count; ++i) {
				if (before[i].name != after[i].name || before[i].value == after[i].value) {
					continue;
				}
				if (n > 0) {
					changed += ",";
				}
				++n;
				changed += "{\"key\":" + Quote(after[i].name);
				changed += ",\"before\":" + before[i].value;
				changed += ",\"after\":" + after[i].value + "}";
			}

			logger::info(FMT_STRING("devbench: TOML reloaded on request, {} value(s) changed"), n);

			std::string out = "{\"ok\":true,\"changedCount\":" + std::to_string(n);
			out += ",\"changed\":[" + changed + "]";
			out += ",\"settings\":" + SettingsObject(after);
			out += "}";
			return out;
		}

		// The startup self-check's findings, on demand instead of only in the log.
		// Nested under its own key rather than merged, so the report owns its whole
		// shape and this route owns the ok flag.
		[[nodiscard]] std::string HandleState(const Request&)
		{
			const std::string report = Upscaler::SelfCheck::ReportJson();
			if (report.empty() || report.front() != '{') {
				return Err("the self-check produced no report; see the plugin log for why it did not run");
			}
			return "{\"ok\":true,\"selfCheck\":" + report + "}";
		}

		[[nodiscard]] std::string HandleObserve(const Request&)
		{
			const auto temporal = Upscaler::Hooks::TemporalAAProbe();
			if (!temporal.installed || !temporal.drawInstalled) {
				return Err("the TemporalAA entry/draw observation hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			const auto sequence = Upscaler::Debug::RequestBindingObservation(
				Upscaler::Debug::ObservationPoint::temporalAADraw);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"pending":true,"note":"poll action=state until bindingObservation.completedSequence reaches this sequence"}})"),
				sequence);
		}

		[[nodiscard]] std::string HandleObserveClear(const Request&)
		{
			if (!Upscaler::Hooks::RenderThreadProbe().installed) {
				return Err("the ClearColor observation hook is not installed; check enabled=true and its byte-verification line in Fo4VRUpscaler.log");
			}
			const auto sequence = Upscaler::Debug::RequestBindingObservation(
				Upscaler::Debug::ObservationPoint::clearColor);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"pending":true,"note":"poll action=state until bindingObservation.completedSequence reaches this sequence"}})"),
				sequence);
		}

		[[nodiscard]] std::string HandleObserveHDR(const Request&)
		{
			const auto hdr = Upscaler::Hooks::HDRInputProbe();
			if (!hdr.installed || !hdr.drawInstalled) {
				return Err("the HDR dispatch/generic draw observation hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			const auto sequence = Upscaler::Debug::RequestBindingObservation(
				Upscaler::Debug::ObservationPoint::hdrFinalDraw);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"pending":true,"note":"poll action=state until bindingObservation.completedSequence reaches this sequence"}})"),
				sequence);
		}

		[[nodiscard]] std::string HandleCaptureTAA(const Request& a_req)
		{
			const auto temporal = Upscaler::Hooks::TemporalAAProbe();
			if (!temporal.installed || !temporal.drawInstalled) {
				return Err("the TemporalAA entry/draw hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			std::uint32_t jitterPhase = Upscaler::StereoCapture::kAnyJitterPhase;
			if (const auto error = CaptureJitterPhase(a_req, jitterPhase); !error.empty()) {
				return Err(error);
			}
			const auto sequence = Upscaler::StereoCapture::Request(false, 0, jitterPhase);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"jitterPhase":{},"pending":true,"note":"poll action=captureStatus; the render thread performs D3D11 readback and the status poll writes separate left/right BMPs"}})"),
				sequence,
				jitterPhase == Upscaler::StereoCapture::kAnyJitterPhase ? -1 : static_cast<std::int32_t>(jitterPhase));
		}

		[[nodiscard]] std::string HandleCaptureStatus(const Request&)
		{
			const auto capture = Upscaler::StereoCapture::SaveAndSnapshot();
			std::string out = "{\"ok\":true";
			out += ",\"requestedSequence\":" + std::to_string(capture.requestedSequence);
			out += ",\"completedSequence\":" + std::to_string(capture.completedSequence);
			out += ",\"pending\":" + Bool(capture.pending);
			out += ",\"captured\":" + Bool(capture.captured);
			out += ",\"saved\":" + Bool(capture.saved);
			out += ",\"inputsRequested\":" + Bool(capture.inputsRequested);
			out += ",\"inputsCaptured\":" + Bool(capture.inputsCaptured);
			out += ",\"inputsSaved\":" + Bool(capture.inputsSaved);
			out += ",\"mode\":" + Quote(capture.bypassed ? "bypass"sv : capture.overwritten ? "overwrite"sv : "native"sv);
			out += ",\"temporalAACall\":" + std::to_string(capture.temporalAACall);
			out += ",\"ngxHandbackSerial\":" + std::to_string(capture.ngxHandbackSerial);
			out += ",\"requestedSkipBypasses\":" + std::to_string(capture.requestedSkipBypasses);
			out += ",\"skippedBypasses\":" + std::to_string(capture.skippedBypasses);
			out += ",\"requestedJitterPhase\":" + std::to_string(capture.requestedJitterPhase == Upscaler::StereoCapture::kAnyJitterPhase ? -1 : static_cast<std::int32_t>(capture.requestedJitterPhase));
			out += ",\"jitterPhase\":" + std::to_string(capture.jitterPhase == Upscaler::StereoCapture::kAnyJitterPhase ? -1 : static_cast<std::int32_t>(capture.jitterPhase));
			out += ",\"skippedJitterPhases\":" + std::to_string(capture.skippedJitterPhases);
			out += ",\"threadId\":" + std::to_string(capture.threadId);
			out += ",\"eyeWidth\":" + std::to_string(capture.eyeWidth);
			out += ",\"eyeHeight\":" + std::to_string(capture.eyeHeight);
			out += ",\"format\":" + std::to_string(capture.format);
			out += ",\"d3dHr\":" + std::to_string(capture.d3dHr);
			out += ",\"left\":{\"darkPct\":" + Num(capture.left.darkPct) + ",\"meanLuma\":" + Num(capture.left.meanLuma) + ",\"file\":" + Quote(capture.leftPath) + "}";
			out += ",\"right\":{\"darkPct\":" + Num(capture.right.darkPct) + ",\"meanLuma\":" + Num(capture.right.meanLuma) + ",\"file\":" + Quote(capture.rightPath) + "}";
			out += ",\"leftMotion\":{\"zeroPct\":" + Num(capture.leftMotion.zeroPct) + ",\"nonFinitePct\":" + Num(capture.leftMotion.nonFinitePct) + ",\"minX\":" + Num(capture.leftMotion.minX) + ",\"maxX\":" + Num(capture.leftMotion.maxX) + ",\"minY\":" + Num(capture.leftMotion.minY) + ",\"maxY\":" + Num(capture.leftMotion.maxY) + ",\"meanAbsX\":" + Num(capture.leftMotion.meanAbsX) + ",\"meanAbsY\":" + Num(capture.leftMotion.meanAbsY) + ",\"file\":" + Quote(capture.leftMotionPath) + "}";
			out += ",\"rightMotion\":{\"zeroPct\":" + Num(capture.rightMotion.zeroPct) + ",\"nonFinitePct\":" + Num(capture.rightMotion.nonFinitePct) + ",\"minX\":" + Num(capture.rightMotion.minX) + ",\"maxX\":" + Num(capture.rightMotion.maxX) + ",\"minY\":" + Num(capture.rightMotion.minY) + ",\"maxY\":" + Num(capture.rightMotion.maxY) + ",\"meanAbsX\":" + Num(capture.rightMotion.meanAbsX) + ",\"meanAbsY\":" + Num(capture.rightMotion.meanAbsY) + ",\"file\":" + Quote(capture.rightMotionPath) + "}";
			out += ",\"leftDepth\":{\"farPct\":" + Num(capture.leftDepth.farPct) + ",\"min\":" + Num(capture.leftDepth.minDepth) + ",\"max\":" + Num(capture.leftDepth.maxDepth) + ",\"file\":" + Quote(capture.leftDepthPath) + "}";
			out += ",\"rightDepth\":{\"farPct\":" + Num(capture.rightDepth.farPct) + ",\"min\":" + Num(capture.rightDepth.minDepth) + ",\"max\":" + Num(capture.rightDepth.maxDepth) + ",\"file\":" + Quote(capture.rightDepthPath) + "}";
			out += ",\"leftInputColor\":" + Quote(capture.leftInputColorPath);
			out += ",\"rightInputColor\":" + Quote(capture.rightInputColorPath);
			out += ",\"leftNgxOutput\":" + Quote(capture.leftNgxOutputPath);
			out += ",\"rightNgxOutput\":" + Quote(capture.rightNgxOutputPath);
			out += ",\"error\":" + Quote(capture.error);
			out += "}";
			return out;
		}

		[[nodiscard]] std::string HandleCaptureBypass(const Request& a_req)
		{
			const auto temporal = Upscaler::Hooks::TemporalAAProbe();
			if (!temporal.installed || !temporal.drawInstalled) {
				return Err("the TemporalAA entry/draw hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			std::uint32_t skipBypasses = 0;
			if (const auto* raw = a_req.Get("skipBypasses"sv); raw) {
				const auto [end, error] = std::from_chars(raw->data(), raw->data() + raw->size(), skipBypasses);
				if (error != std::errc{} || end != raw->data() + raw->size() || skipBypasses > 511) {
					return Err("skipBypasses must be an integer from 0 through 511");
				}
			}
			std::uint32_t jitterPhase = Upscaler::StereoCapture::kAnyJitterPhase;
			if (const auto error = CaptureJitterPhase(a_req, jitterPhase); !error.empty()) {
				return Err(error);
			}
			const auto sequence = Upscaler::StereoCapture::Request(true, skipBypasses, jitterPhase);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"skipBypasses":{},"jitterPhase":{},"pending":true,"note":"poll action=captureStatus; native and overwrite frames plus the requested number of accepted bypasses are ignored before render-thread capture"}})"),
				sequence,
				skipBypasses,
				jitterPhase == Upscaler::StereoCapture::kAnyJitterPhase ? -1 : static_cast<std::int32_t>(jitterPhase));
		}

		[[nodiscard]] std::string HandleCaptureInputs(const Request& a_req)
		{
			const auto temporal = Upscaler::Hooks::TemporalAAProbe();
			if (!temporal.installed || !temporal.drawInstalled) {
				return Err("the TemporalAA entry/draw hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			std::uint32_t skipBypasses = 0;
			if (const auto* raw = a_req.Get("skipBypasses"sv); raw) {
				const auto [end, error] = std::from_chars(raw->data(), raw->data() + raw->size(), skipBypasses);
				if (error != std::errc{} || end != raw->data() + raw->size() || skipBypasses > 511) {
					return Err("skipBypasses must be an integer from 0 through 511");
				}
			}
			std::uint32_t jitterPhase = Upscaler::StereoCapture::kAnyJitterPhase;
			if (const auto error = CaptureJitterPhase(a_req, jitterPhase); !error.empty()) {
				return Err(error);
			}
			const auto sequence = Upscaler::StereoCapture::Request(true, skipBypasses, jitterPhase, true);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"skipBypasses":{},"jitterPhase":{},"pending":true,"note":"poll action=captureStatus; the accepted bypass capture also reads the exact per-eye vendor color, motion, raw output and matching scene depth on the render thread, then writes diagnostic BMPs on the listener thread"}})"),
				sequence,
				skipBypasses,
				jitterPhase == Upscaler::StereoCapture::kAnyJitterPhase ? -1 : static_cast<std::int32_t>(jitterPhase));
		}

		// One row per compiled-in backend. The unavailableReason is printed verbatim
		// - the bench reports what the runtime said, never a paraphrase of it.
		[[nodiscard]] std::string HandleBackends(const Request&)
		{
			auto* const device = Upscaler::Engine::Device();
			const auto all = Upscaler::Backend::All();
			const auto* current = Upscaler::Backend::Current();

			std::string out = "{\"ok\":true";
			// Probing with a null device is documented safe and is the normal day-one
			// answer, but it only answers "is the runtime present at all". Say which
			// question was asked rather than letting a caller assume the stronger one.
			out += ",\"deviceBound\":" + Bool(device != nullptr);
			out += ",\"selected\":" + (current ? Quote(current->Name()) : std::string{ "null" });
			out += ",\"count\":" + std::to_string(all.size());
			out += ",\"backends\":[";
			bool first = true;
			for (auto* const backend : all) {
				if (!backend) {
					continue;
				}
				const auto caps = backend->Probe(device);
				if (!first) {
					out += ",";
				}
				first = false;
				out += "{\"name\":" + Quote(backend->Name());
				out += ",\"available\":" + Bool(caps.available);
				out += ",\"unavailableReason\":" + Quote(FromFixed(caps.unavailableReason));
				out += ",\"runtimeVersion\":" + Quote(FromFixed(caps.runtimeVersion));
				out += ",\"runtimePath\":" + Quote(FromFixed(caps.runtimePath));
				out += ",\"supportsSubrects\":" + Bool(caps.supportsSubrects);
				out += ",\"needsDeviceCreation\":" + Bool(caps.needsDeviceCreation);
				out += ",\"supportsExposure\":" + Bool(caps.supportsExposure);
				out += ",\"supportsReactiveMask\":" + Bool(caps.supportsReactiveMask);
				out += ",\"ownsSharpening\":" + Bool(caps.ownsSharpening);
				out += "}";
			}
			out += "]";
			if (all.empty()) {
				out += ",\"note\":\"no backend is compiled in - see cmake/backendlist.cmake and the UPSCALER_ENABLE_* options\"";
			}
			out += "}";
			return out;
		}

		// the route table
		//
		// One list. Route() below expands it into the dispatch, and Routes()
		// publishes it so DevBenchClient can build the tool descriptor and its own
		// action dispatch from the same rows. Adding a route is this list plus a
		// handler, and nothing else can fall out of step.
		//
		// Descriptions are agent-facing: the descriptor is the only view an agent
		// has of what these answers mean.
#define US_DEVBENCH_ROUTES                                                                                                                                                                                                                                                                                              \
	US_ROUTE(state, "/state", HandleState, "Live self-check facts: runtime version, module base, D3D11 and engine-handle resolution, target tables, depth-stencil mapping, the pre-tonemap HDR handoff, native TemporalAA cadence, and the last binding observation. The probes change no render state; unresolved subsystems name what unblocks them.") \
	US_ROUTE(observe, "/observe", HandleObserve, "Arm one read-only binding observation after the native TemporalAA pixel draw and before its cleanup. MUTATES only the instrument request sequence: no pixels or D3D11 state change. Poll action=state and match bindingObservation.completedSequence to the returned sequence.")                                                    \
	US_ROUTE(observeHdr, "/observe/hdr", HandleObserveHDR, "Arm one read-only observation of HDR effect 3's final draw. Intermediate luminance draws are ignored; capture completes only when OM contains the live dispatch destination. MUTATES only the instrument request sequence: no pixels or D3D11 state change.")                                                                                \
	US_ROUTE(observeClear, "/observe/clear", HandleObserveClear, "Arm the older read-only ClearColor observation for comparison. MUTATES only the instrument request sequence: no pixels or D3D11 state change. ClearColor normally sees shadow work, not TemporalAA.")                                                                                                           \
	US_ROUTE(captureTaa, "/capture/taa", HandleCaptureTAA, "Arm one submitted-stereo capture at the post-TemporalAA render-thread seam. Optional jitterPhase=0..7 waits for that verified native phase. MUTATES only the instrument sequence and performs one bounded GPU readback. Poll captureStatus to write separate left/right BMPs and report per-eye darkness/luminance plus native/overwrite/bypass mode.")                                              \
	US_ROUTE(captureBypass, "/capture/bypass", HandleCaptureBypass, "Arm one submitted-stereo capture that ignores native and overwrite frames until an accepted prepared-draw bypass reaches the post-TemporalAA seam. Optional skipBypasses=0..511 ignores that many accepted bypasses; optional jitterPhase=0..7 then waits for the same verified native phase, allowing a settled phase-aligned handback to be selected. MUTATES only the instrument sequence and performs one bounded GPU readback.")                            \
	US_ROUTE(captureInputs, "/capture/inputs", HandleCaptureInputs, "Arm the accepted-bypass capture plus one exact-frame diagnostic readback of both per-eye vendor input colors, motion vectors and raw NGX outputs plus the matching scene depth. Optional skipBypasses=0..511 and jitterPhase=0..7 select the handback. GPU copy/map stays on the render thread; the status poll writes submitted, input, output, motion and depth BMPs off-thread. DEVELOPMENT DIAGNOSTIC: this intentionally stalls one frame and is not a headset benchmark.") \
	US_ROUTE(captureStatus, "/capture/status", HandleCaptureStatus, "Report the stereo capture state. After render-thread readback completes, the first poll writes both eye BMPs under Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler/captures on the listener thread and returns their physical absolute paths.")                                                                                             \
	US_ROUTE(config, "/config", HandleConfig, "Every setting declared in Fo4VRUpscaler.toml and its live value. Generated from the same list the settings themselves are declared from, so it cannot omit a key.")                                                                                                      \
	US_ROUTE(reload, "/config/reload", HandleConfigReload, "Re-read the TOML now and report which values changed, with before and after. MUTATES: it replaces the live settings.")                                                                                                                                      \
	US_ROUTE(backends, "/backends", HandleBackends, "One row per compiled-in backend: name, available, the runtime's own unavailableReason verbatim, and the capability bits. deviceBound says whether a D3D11 device was available to probe against; without one the answer is only whether a runtime is present.")    \
	US_ROUTE(health, "/health", HandleHealth, "Liveness and identity: plugin, version, pid, uptime, requests served, and the build configuration including which backends were compiled in. Touches no game state, so it still answers when the game does not.")

		[[nodiscard]] std::string Route(const Request& a_req)
		{
#define US_ROUTE(a_action, a_path, a_handler, a_desc) \
	if (a_req.path == a_path)                         \
		return a_handler(a_req);
			US_DEVBENCH_ROUTES
#undef US_ROUTE
			return Err("no such route '" + a_req.path + "' - the tool's action enum lists every one");
		}
	}

	std::span<const RouteInfo> Routes() noexcept
	{
		static constexpr RouteInfo s_routes[] = {
#define US_ROUTE(a_action, a_path, a_handler, a_desc) { #a_action, a_path, a_desc },
			US_DEVBENCH_ROUTES
#undef US_ROUTE
		};
		return s_routes;
	}

	void Init()
	{
		g_startTick = ::GetTickCount64();
		logger::info("devbench routes ready - served through the 'upscaler' tool when a devbench host is present"sv);
	}

	std::string Invoke(std::string_view a_path, const std::vector<std::pair<std::string, std::string>>& a_params)
	{
		// Synthesise a Request and hand it to the same Route() every handler hangs
		// off. Nothing here interprets a path or a parameter, on purpose.
		Request req;
		req.method = "GET";
		req.path.assign(a_path);
		req.query = a_params;
		g_requests.fetch_add(1);
		try {
			return Route(req);
		} catch (const std::exception& e) {
			// Never let an exception cross the C-ABI boundary, where it would unwind
			// through the host's frames.
			return Err(std::string{ "handler threw: " } + e.what());
		} catch (...) {
			return Err("handler threw a non-standard exception");
		}
	}
}
