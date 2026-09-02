#include "DevBench/DevBench.h"

#include "Settings/Configuration.h"

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
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/SelfCheck.h"
#include "Upscaler/StereoCapture.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/TAAReplacement.h"
#include "Upscaler/Telemetry.h"

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

		// Call with Settings::configurationLock then Settings::loadLock held.  The
		// outer lock keeps this snapshot on one complete settings/runtime episode;
		// the inner lock prevents a string reassignment underneath the reader.
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
			if (error != std::errc{} || end != raw->data() + raw->size() || phase >= Upscaler::Jitter::NativePhaseCount()) {
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
			struct SnapshotOperation
			{
				std::vector<SettingRow>* rows = nullptr;
			};
			const auto captureWithLoadLock = [](void* a_context) noexcept -> bool {
				return Settings::WithLoadLock(
					[](void* a_innerContext) noexcept -> bool {
						auto* const operation = static_cast<SnapshotOperation*>(a_innerContext);
						if (!operation || !operation->rows) {
							return false;
						}
						try {
							*operation->rows = SettingsSnapshot();
							return true;
						} catch (...) {
							return false;
						}
					},
					a_context);
			};

			std::vector<SettingRow> rows;
			SnapshotOperation operation{ std::addressof(rows) };
			if (!Settings::WithConfigurationLock(captureWithLoadLock, std::addressof(operation))) {
				rows.clear();
			}
			return rows;
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
			struct ReloadSnapshots
			{
				std::vector<SettingRow> before;
				std::vector<SettingRow> after;
			};
			const auto snapshotBefore = [](void* a_context) noexcept -> bool {
				auto* const snapshots = static_cast<ReloadSnapshots*>(a_context);
				try {
					snapshots->before = SettingsSnapshot();
					return true;
				} catch (...) {
					return false;
				}
			};
			const auto snapshotAfter = [](void* a_context) noexcept -> bool {
				auto* const snapshots = static_cast<ReloadSnapshots*>(a_context);
				try {
					snapshots->after = SettingsSnapshot();
					return true;
				} catch (...) {
					return false;
				}
			};

			ReloadSnapshots snapshots{};
			const Configuration::Observer observer{
				std::addressof(snapshots),
				snapshotBefore,
				snapshotAfter
			};
			const auto applied = Configuration::Apply(std::addressof(observer));
			if (!applied.runtimeApplied || !applied.beforeObserved || !applied.afterObserved) {
				return Err(applied.exceptionCode == 0 ?
							   "settings reload did not publish one complete runtime snapshot; see Fo4VRUpscaler.log" :
							   fmt::format(FMT_STRING("settings reload raised SEH 0x{:08X}; the prior snapshot was retained or the diagnostic session was closed"), applied.exceptionCode));
			}
			const auto& before = snapshots.before;
			const auto& after = snapshots.after;
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

		[[nodiscard]] std::string HandlePerf(const Request& a_req)
		{
			std::uint64_t requestedReset = 0;
			if (const auto* const reset = a_req.Get("reset"sv); reset) {
				if (*reset != "true"sv && *reset != "false"sv) {
					return Err("reset must be true or false");
				}
				if (*reset == "true"sv) {
					requestedReset = Upscaler::Telemetry::RequestReset();
				}
			}
			const std::string report = Upscaler::Telemetry::Report();
			if (report.empty() || report.front() != '{') {
				return Err("performance telemetry produced no report");
			}
			std::string out = "{\"ok\":true";
			if (requestedReset != 0) {
				out += ",\"requestedResetGeneration\":" + std::to_string(requestedReset);
			}
			out += ",\"perf\":" + report + "}";
			return out;
		}

		[[nodiscard]] std::string HandleInjectQualityFault(const Request& a_req)
		{
			const auto* const raw = a_req.Get("faultSeam"sv);
			if (!raw) {
				return Err("faultSeam is required: none|preBackup|postBackup|postRt1|postTaaStage|prePublish|postPublish");
			}

			Upscaler::HDRRoundTrip::QualityFaultSeam seam{};
			if (*raw == "none"sv) {
				seam = Upscaler::HDRRoundTrip::QualityFaultSeam::kNone;
			} else if (*raw == "preBackup"sv) {
				seam = Upscaler::HDRRoundTrip::QualityFaultSeam::kPreBackup;
			} else if (*raw == "postBackup"sv) {
				seam = Upscaler::HDRRoundTrip::QualityFaultSeam::kPostBackup;
			} else if (*raw == "postRt1"sv) {
				seam = Upscaler::HDRRoundTrip::QualityFaultSeam::kPostRt1;
			} else if (*raw == "postTaaStage"sv) {
				seam = Upscaler::HDRRoundTrip::QualityFaultSeam::kPostTaaStage;
			} else if (*raw == "prePublish"sv) {
				seam = Upscaler::HDRRoundTrip::QualityFaultSeam::kPrePublish;
			} else if (*raw == "postPublish"sv) {
				seam = Upscaler::HDRRoundTrip::QualityFaultSeam::kPostPublish;
			} else {
				return Err("faultSeam must be none, preBackup, postBackup, postRt1, postTaaStage, prePublish or postPublish");
			}

			std::uint32_t faultCount = 1;
			if (const auto* count = a_req.Get("faultCount"sv); count) {
				const auto [end, error] = std::from_chars(count->data(), count->data() + count->size(), faultCount);
				if (error != std::errc{} || end != count->data() + count->size() || faultCount == 0 ||
					faultCount > Upscaler::HDRRoundTrip::kMaxQualityFaultCount) {
					return Err("faultCount must be an integer from 1 through 64");
				}
			}
			if (seam == Upscaler::HDRRoundTrip::QualityFaultSeam::kNone && faultCount != 1) {
				return Err("faultCount must be 1 when faultSeam is none");
			}
			if (faultCount > 1 && seam != Upscaler::HDRRoundTrip::QualityFaultSeam::kPostTaaStage &&
				seam != Upscaler::HDRRoundTrip::QualityFaultSeam::kPrePublish) {
				return Err("faultCount greater than 1 is supported only for postTaaStage and prePublish");
			}

			const bool accepted = Upscaler::HDRRoundTrip::ArmQualityFault(seam, faultCount);
			const auto state = Upscaler::HDRRoundTrip::Snapshot();
			std::string out = accepted ? "{\"ok\":true" :
			                             "{\"ok\":false,\"error\":\"the counted fault was not armed; require an idle, session-enabled, continuous NGX quality path with no fault already armed\"";
			out += ",\"requested\":" + Quote(*raw);
			out += ",\"requestedCount\":" + std::to_string(faultCount);
			out += ",\"acceptedCount\":" + std::to_string(accepted ? faultCount : 0);
			out += ",\"armed\":" + Quote(Upscaler::HDRRoundTrip::QualityFaultName(
									   static_cast<Upscaler::HDRRoundTrip::QualityFaultSeam>(state.qualityFaultArmedSeam)));
			out += ",\"remaining\":" + std::to_string(state.qualityFaultRemaining);
			out += ",\"lastTriggered\":" + Quote(Upscaler::HDRRoundTrip::QualityFaultName(
											   static_cast<Upscaler::HDRRoundTrip::QualityFaultSeam>(state.qualityFaultLastTriggeredSeam)));
			out += ",\"armAttempts\":" + std::to_string(state.qualityFaultArmAttempts);
			out += ",\"arms\":" + std::to_string(state.qualityFaultArms);
			out += ",\"rejections\":" + std::to_string(state.qualityFaultRejections);
			out += ",\"triggers\":" + std::to_string(state.qualityFaultTriggers);
			out += "}";
			return out;
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

		[[nodiscard]] std::string HandleCapturePrivate(const Request& a_req)
		{
			const auto temporal = Upscaler::Hooks::TemporalAAProbe();
			if (!temporal.installed || !temporal.drawInstalled) {
				return Err("the TemporalAA entry/draw hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			std::uint32_t jitterPhase = Upscaler::StereoCapture::kAnyJitterPhase;
			if (const auto error = CaptureJitterPhase(a_req, jitterPhase); !error.empty()) {
				return Err(error);
			}
			const auto sequence = Upscaler::StereoCapture::Request(
				false,
				0,
				jitterPhase,
				true,
				true);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"jitterPhase":{},"pending":true,"note":"arm this before the private vendor gate; the first matching post-TAA frame reads submitted colour plus both private vendor input colours, motion vectors, raw outputs and scene depth before next-frame teardown"}})"),
				sequence,
				jitterPhase == Upscaler::StereoCapture::kAnyJitterPhase ? -1 : static_cast<std::int32_t>(jitterPhase));
		}

		[[nodiscard]] std::string HandleCaptureReassembled(const Request&)
		{
			const auto temporal = Upscaler::Hooks::TemporalAAProbe();
			if (!temporal.installed || !temporal.drawInstalled) {
				return Err("the TemporalAA entry/draw hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			const auto sequence = Upscaler::StereoCapture::Request(
				false,
				0,
				Upscaler::StereoCapture::kAnyJitterPhase,
				true,
				true,
				true);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"pending":true,"note":"arm this before a private reassembly gate; the first accepted private double-wide serial is split and saved beside its exact vendor inputs and per-eye outputs before next-frame teardown"}})"),
				sequence);
		}

		[[nodiscard]] std::string HandleCapturePipeline(const Request& a_req)
		{
			const auto temporal = Upscaler::Hooks::TemporalAAProbe();
			if (!temporal.installed || !temporal.drawInstalled) {
				return Err("the TemporalAA entry/draw hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			std::uint32_t jitterPhase = Upscaler::StereoCapture::kAnyJitterPhase;
			if (const auto error = CaptureJitterPhase(a_req, jitterPhase); !error.empty()) {
				return Err(error);
			}
			const auto sequence = Upscaler::StereoCapture::Request(
				true,
				0,
				jitterPhase,
				true,
				true,
				true,
				true);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"jitterPhase":{},"pending":true,"note":"the next matching accepted NGX quality transaction freezes private output and engine logical 1 immediately after CopyResource, records raw byte equality and serial identity, then captures the same transaction downstream at the TemporalAA bypass seam"}})"),
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
			out += ",\"reassembledRequested\":" + Bool(capture.reassembledRequested);
			out += ",\"reassembledCaptured\":" + Bool(capture.reassembledCaptured);
			out += ",\"reassembledSaved\":" + Bool(capture.reassembledSaved);
			out += ",\"handbackSeamRequested\":" + Bool(capture.handbackSeamRequested);
			out += ",\"handbackSeamCaptured\":" + Bool(capture.handbackSeamCaptured);
			out += ",\"handbackSeamSaved\":" + Bool(capture.handbackSeamSaved);
			out += ",\"handbackRawEqual\":" + Bool(capture.handbackRawEqual);
			out += ",\"handbackTransactionCommitPending\":" + Bool(capture.handbackTransactionCommitPending);
			out += ",\"handbackTransactionCommitted\":" + Bool(capture.handbackTransactionCommitted);
			out += ",\"mode\":" + Quote(capture.bypassed ? "bypass"sv : capture.overwritten ? "overwrite"sv :
																							  "native"sv);
			out += ",\"temporalAACall\":" + std::to_string(capture.temporalAACall);
			out += ",\"ngxHandbackSerial\":" + std::to_string(capture.ngxHandbackSerial);
			out += ",\"ffxHandbackRequested\":" + Bool(capture.ffxHandbackRequested);
			out += ",\"ffxGateExhausted\":" + Bool(capture.ffxGateExhausted);
			out += ",\"requestedFfxHandbackSerial\":" + std::to_string(capture.requestedFfxHandbackSerial);
			out += ",\"ffxHandbackSerial\":" + std::to_string(capture.ffxHandbackSerial);
			out += ",\"requestedPrivateReassemblySerial\":" + std::to_string(capture.requestedPrivateReassemblySerial);
			out += ",\"privateReassemblySerial\":" + std::to_string(capture.privateReassemblySerial);
			out += ",\"handbackPrivateReassemblySerial\":" + std::to_string(capture.handbackPrivateReassemblySerial);
			out += ",\"handbackNgxSerial\":" + std::to_string(capture.handbackNgxSerial);
			out += ",\"handbackRawBytes\":" + std::to_string(capture.handbackRawBytes);
			out += ",\"handbackRawMismatchBytes\":" + std::to_string(capture.handbackRawMismatchBytes);
			out += ",\"handbackCommittedNgxSerial\":" + std::to_string(capture.handbackCommittedNgxSerial);
			out += ",\"handbackTransactionAbortCount\":" + std::to_string(capture.handbackTransactionAbortCount);
			out += ",\"requestedSkipBypasses\":" + std::to_string(capture.requestedSkipBypasses);
			out += ",\"skippedBypasses\":" + std::to_string(capture.skippedBypasses);
			out += ",\"requestedSkipHandbacks\":" + std::to_string(capture.requestedSkipHandbacks);
			out += ",\"skippedHandbacks\":" + std::to_string(capture.skippedHandbacks);
			out += ",\"requestedJitterPhase\":" + std::to_string(capture.requestedJitterPhase == Upscaler::StereoCapture::kAnyJitterPhase ? -1 : static_cast<std::int32_t>(capture.requestedJitterPhase));
			out += ",\"jitterPhase\":" + std::to_string(capture.jitterPhase == Upscaler::StereoCapture::kAnyJitterPhase ? -1 : static_cast<std::int32_t>(capture.jitterPhase));
			out += ",\"skippedJitterPhases\":" + std::to_string(capture.skippedJitterPhases);
			out += ",\"threadId\":" + std::to_string(capture.threadId);
			out += ",\"eyeWidth\":" + std::to_string(capture.eyeWidth);
			out += ",\"eyeHeight\":" + std::to_string(capture.eyeHeight);
			out += ",\"vendorInputWidth\":" + std::to_string(capture.vendorInputWidth);
			out += ",\"vendorInputHeight\":" + std::to_string(capture.vendorInputHeight);
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
			out += ",\"leftVendorOutput\":" + Quote(capture.leftNgxOutputPath);
			out += ",\"rightVendorOutput\":" + Quote(capture.rightNgxOutputPath);
			out += ",\"leftReassembled\":{\"darkPct\":" + Num(capture.leftReassembled.darkPct) + ",\"meanLuma\":" + Num(capture.leftReassembled.meanLuma) + ",\"file\":" + Quote(capture.leftReassembledPath) + "}";
			out += ",\"rightReassembled\":{\"darkPct\":" + Num(capture.rightReassembled.darkPct) + ",\"meanLuma\":" + Num(capture.rightReassembled.meanLuma) + ",\"file\":" + Quote(capture.rightReassembledPath) + "}";
			out += ",\"leftPostTonemap\":{\"darkPct\":" + Num(capture.leftPostTonemap.darkPct) + ",\"meanLuma\":" + Num(capture.leftPostTonemap.meanLuma) + ",\"file\":" + Quote(capture.leftPostTonemapPath) + "}";
			out += ",\"rightPostTonemap\":{\"darkPct\":" + Num(capture.rightPostTonemap.darkPct) + ",\"meanLuma\":" + Num(capture.rightPostTonemap.meanLuma) + ",\"file\":" + Quote(capture.rightPostTonemapPath) + "}";
			out += ",\"leftPreTonemap\":{\"darkPct\":" + Num(capture.leftPreTonemap.darkPct) + ",\"meanLuma\":" + Num(capture.leftPreTonemap.meanLuma) + ",\"file\":" + Quote(capture.leftPreTonemapPath) + "}";
			out += ",\"rightPreTonemap\":{\"darkPct\":" + Num(capture.rightPreTonemap.darkPct) + ",\"meanLuma\":" + Num(capture.rightPreTonemap.meanLuma) + ",\"file\":" + Quote(capture.rightPreTonemapPath) + "}";
			out += ",\"preTonemapCaptureSemantics\":\"late TemporalAA-time snapshot of logical target 1 after intervening ImageSpace reuse; not the private-to-engine copy seam\"";
			out += ",\"leftHandbackPrivate\":{\"darkPct\":" + Num(capture.leftHandbackPrivate.darkPct) + ",\"meanLuma\":" + Num(capture.leftHandbackPrivate.meanLuma) + ",\"file\":" + Quote(capture.leftHandbackPrivatePath) + "}";
			out += ",\"rightHandbackPrivate\":{\"darkPct\":" + Num(capture.rightHandbackPrivate.darkPct) + ",\"meanLuma\":" + Num(capture.rightHandbackPrivate.meanLuma) + ",\"file\":" + Quote(capture.rightHandbackPrivatePath) + "}";
			out += ",\"leftHandbackEngine\":{\"darkPct\":" + Num(capture.leftHandbackEngine.darkPct) + ",\"meanLuma\":" + Num(capture.leftHandbackEngine.meanLuma) + ",\"file\":" + Quote(capture.leftHandbackEnginePath) + "}";
			out += ",\"rightHandbackEngine\":{\"darkPct\":" + Num(capture.rightHandbackEngine.darkPct) + ",\"meanLuma\":" + Num(capture.rightHandbackEngine.meanLuma) + ",\"file\":" + Quote(capture.rightHandbackEnginePath) + "}";
			// Compatibility aliases for existing NGX capture consumers.
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

		[[nodiscard]] std::string HandleCaptureFfx(const Request& a_req)
		{
			const auto temporal = Upscaler::Hooks::TemporalAAProbe();
			if (!temporal.installed || !temporal.drawInstalled) {
				return Err("the TemporalAA entry/draw hooks are not both installed; check enabled=true and their byte-verification lines in Fo4VRUpscaler.log");
			}
			std::uint32_t skipHandbacks = 0;
			if (const auto* raw = a_req.Get("skipHandbacks"sv); raw) {
				const auto [end, error] = std::from_chars(raw->data(), raw->data() + raw->size(), skipHandbacks);
				if (error != std::errc{} || end != raw->data() + raw->size() || skipHandbacks > 511) {
					return Err("skipHandbacks must be an integer from 0 through 511");
				}
			}
			std::uint32_t jitterPhase = Upscaler::StereoCapture::kAnyJitterPhase;
			if (const auto error = CaptureJitterPhase(a_req, jitterPhase); !error.empty()) {
				return Err(error);
			}
			const auto sequence = Upscaler::StereoCapture::RequestFfx(skipHandbacks, jitterPhase);
			return fmt::format(
				FMT_STRING(R"({{"ok":true,"sequence":{},"skipHandbacks":{},"jitterPhase":{},"pending":true,"note":"poll action=captureStatus; the request baselines the accepted FFX serial, consumes only fresh FFX handbacks, and captures submitted colour plus exact per-eye inputs, raw outputs, motion and depth"}})"),
				sequence,
				skipHandbacks,
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
			const auto all = Upscaler::Backend::All();
			const auto* current = Upscaler::Backend::Current();

			std::string out = "{\"ok\":true";
			// Listener-thread diagnostics never inspect render-owned mutable backend state.
			// The null-device probe is an immutable build/runtime-description path; live
			// device-bound health belongs to the admitted render telemetry in /state.
			out += ",\"deviceBound\":false";
			out += ",\"selected\":" + (current ? Quote(current->Name()) : std::string{ "null" });
			out += ",\"count\":" + std::to_string(all.size());
			out += ",\"backends\":[";
			bool first = true;
			for (auto* const backend : all) {
				if (!backend) {
					continue;
				}
				const auto caps = backend->Probe(nullptr);
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
#define US_DEVBENCH_ROUTES                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
	US_ROUTE(state, "/state", HandleState, "Live self-check facts: runtime version, module base, D3D11 and engine-handle resolution, target tables, depth-stencil mapping, the pre-tonemap HDR handoff, native TemporalAA cadence, and the last binding observation. The probes change no render state; unresolved subsystems name what unblocks them.")                                                                                                                                                                                                                                                                                                                                                    \
	US_ROUTE(perf, "/perf", HandlePerf, "Listener-safe CPU QPC and nonblocking D3D11 timestamp-disjoint timing for committed reduced-NGX quality transactions. Reports labeled vendor, backup/copy, staged-TAA, pre-publication, publication and total intervals. Optional reset=true atomically requests a clean window; poll until resetCompletedGeneration reaches requestedResetGeneration. Query creation/release and DONOTFLUSH collection remain render-thread-only.")                                                                                                                                                                        \
	US_ROUTE(injectQualityFault, "/quality/fault", HandleInjectQualityFault, "DEVELOPMENT MUTATION: atomically arm deterministic NGX quality-transaction faults at preBackup, postBackup, postRt1, postTaaStage, prePublish or postPublish. faultCount defaults to 1; 2..64 is accepted only for postTaaStage/prePublish, and none disarms the batch. Only one seam may be armed, and only while continuous NGX quality is ready and idle. Poll action=state and inspect selfCheck.qualityFaultInjection to prove every trigger was consumed and classify rollback versus post-publication session disable.")                                                                                               \
	US_ROUTE(observe, "/observe", HandleObserve, "Arm one read-only binding observation after the native TemporalAA pixel draw and before its cleanup. MUTATES only the instrument request sequence: no pixels or D3D11 state change. Poll action=state and match bindingObservation.completedSequence to the returned sequence.")                                                                                                                                                                                                                                                                                                                                                                          \
	US_ROUTE(observeHdr, "/observe/hdr", HandleObserveHDR, "Arm one read-only observation of HDR effect 3's final draw. Intermediate luminance draws are ignored; capture completes only when OM contains the live dispatch destination. MUTATES only the instrument request sequence: no pixels or D3D11 state change.")                                                                                                                                                                                                                                                                                                                                                                                   \
	US_ROUTE(observeClear, "/observe/clear", HandleObserveClear, "Arm the older read-only ClearColor observation for comparison. MUTATES only the instrument request sequence: no pixels or D3D11 state change. ClearColor normally sees shadow work, not TemporalAA.")                                                                                                                                                                                                                                                                                                                                                                                                                                     \
	US_ROUTE(captureTaa, "/capture/taa", HandleCaptureTAA, "Arm one submitted-stereo capture at the post-TemporalAA render-thread seam. Optional jitterPhase=0..7 waits for that verified native phase. MUTATES only the instrument sequence and performs one bounded GPU readback. Poll captureStatus to write separate left/right BMPs and report per-eye darkness/luminance plus native/overwrite/bypass mode.")                                                                                                                                                                                                                                                                                         \
	US_ROUTE(capturePrivate, "/capture/private", HandleCapturePrivate, "Arm before a one-shot private vendor gate. Optional jitterPhase=0..7 waits for that verified native phase, allowing a phase-matched native/null control. The first matching post-TemporalAA frame captures submitted colour plus both zero-origin vendor input colours, completed motion, raw private outputs and matching depth before next-frame teardown. No bypass or engine-colour handback is required. Poll captureStatus to write the files. DEVELOPMENT DIAGNOSTIC: this intentionally stalls one frame and is not a headset benchmark.")                                                                                  \
	US_ROUTE(captureReassembled, "/capture/reassembled", HandleCaptureReassembled, "Arm before a one-shot private reassembly gate. The request remains pending until a new accepted private double-wide serial exists, then captures and splits that exact resource beside the submitted frame, vendor inputs, per-eye outputs, completed motion and depth before next-frame teardown. No engine-colour handback is required. DEVELOPMENT DIAGNOSTIC: this intentionally stalls one frame and is not a headset benchmark.")                                                                                                                                                                                 \
	US_ROUTE(capturePipeline, "/capture/pipeline", HandleCapturePipeline, "Arm one exact accepted NGX quality transaction. Optional jitterPhase=0..7 waits for that verified native phase. Immediately after the isolated private-to-engine CopyResource, freeze both double-wide R11G11B10 resources, record raw byte equality plus private/NGX serial identity, then capture vendor inputs, private output, late logical-1 diagnostic, post-tonemap and final submitted eyes. The request completes only when the matching late ImageSpace transaction commits; an abort discards that attempt and retries. DEVELOPMENT DIAGNOSTIC: this intentionally stalls one frame and is not a headset benchmark.") \
	US_ROUTE(captureBypass, "/capture/bypass", HandleCaptureBypass, "Arm one submitted-stereo capture that ignores native and overwrite frames until an accepted prepared-draw bypass reaches the post-TemporalAA seam. Optional skipBypasses=0..511 ignores that many accepted bypasses; optional jitterPhase=0..7 then waits for the same verified native phase, allowing a settled phase-aligned handback to be selected. MUTATES only the instrument sequence and performs one bounded GPU readback.")                                                                                                                                                                                                  \
	US_ROUTE(captureInputs, "/capture/inputs", HandleCaptureInputs, "Arm the accepted-bypass capture plus one exact-frame diagnostic readback of both per-eye vendor input colors, motion vectors and raw NGX outputs plus the matching scene depth. Optional skipBypasses=0..511 and jitterPhase=0..7 select the handback. GPU copy/map stays on the render thread; the status poll writes submitted, input, output, motion and depth BMPs off-thread. DEVELOPMENT DIAGNOSTIC: this intentionally stalls one frame and is not a headset benchmark.")                                                                                                                                                       \
	US_ROUTE(captureFfx, "/capture/ffx", HandleCaptureFfx, "Baseline the accepted FFX continuous-handback serial, then arm one exact fresh-FFX capture. Optional skipHandbacks=0..511 consumes later accepted FFX serials; optional jitterPhase=0..7 filters after each fresh serial is consumed. Captures submitted colour plus the same-frame per-eye inputs, raw FFX outputs, motion and depth while reporting the actual native/overwrite/bypass mode. DEVELOPMENT DIAGNOSTIC: this intentionally stalls one frame and is not a headset benchmark.")                                                                                                                                                    \
	US_ROUTE(captureStatus, "/capture/status", HandleCaptureStatus, "Report the stereo capture state. After render-thread readback completes, the first poll writes both eye BMPs under Documents/My Games/Fallout4VR/F4SE/Fo4VRUpscaler/captures on the listener thread and returns their physical absolute paths.")                                                                                                                                                                                                                                                                                                                                                                                       \
	US_ROUTE(config, "/config", HandleConfig, "Every setting declared in Fo4VRUpscaler.toml and its live value. Generated from the same list the settings themselves are declared from, so it cannot omit a key.")                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
	US_ROUTE(reload, "/config/reload", HandleConfigReload, "Re-read the TOML now and report which values changed, with before and after. MUTATES: it replaces the live settings.")                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
	US_ROUTE(backends, "/backends", HandleBackends, "One immutable build/pre-device descriptor per compiled-in backend. deviceBound is false: listener-thread diagnostics never inspect live render-owned backend state, so vendor runtime and adapter availability remain conservative until the render-thread telemetry reports them.")                                                                                                                                                                                                                                                                                                                                                                                        \
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
