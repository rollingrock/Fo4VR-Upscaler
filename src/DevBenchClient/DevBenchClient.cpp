#include "DevBenchClient/DevBenchClient.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "DevBench/DevBench.h"
#include "DevBenchClient/DevBenchAPI.h"
#include "Settings/Settings.h"

namespace DevBenchClient
{
	namespace
	{
		std::atomic_bool g_connected{ false };

		// a very small json reader
		//
		// No JSON library in this plugin on purpose (see DevBench.h), and the tool
		// arguments are flat by construction - the host generates them from the
		// schema this file publishes - so this extracts top-level scalars only and
		// refuses anything else. Depth is tracked, so a nested key is never mistaken
		// for a top-level one; malformed input yields "not found", which every
		// caller turns into a stated default or an error.

		void SkipWs(std::string_view a_s, std::size_t& a_i)
		{
			while (a_i < a_s.size() && (a_s[a_i] == ' ' || a_s[a_i] == '\t' || a_s[a_i] == '\n' || a_s[a_i] == '\r')) {
				++a_i;
			}
		}

		// Read a JSON string starting at the opening quote. Handles the escapes JSON
		// requires; \u is passed through as-is because none of our fields is unicode
		// and inventing a transcoding here would be more risk than value.
		bool ReadString(std::string_view a_s, std::size_t& a_i, std::string& a_out)
		{
			if (a_i >= a_s.size() || a_s[a_i] != '"') {
				return false;
			}
			++a_i;
			a_out.clear();
			while (a_i < a_s.size()) {
				const char c = a_s[a_i++];
				if (c == '"') {
					return true;
				}
				if (c != '\\') {
					a_out += c;
					continue;
				}
				if (a_i >= a_s.size()) {
					return false;
				}
				switch (const char e = a_s[a_i++]) {
				case 'n':
					a_out += '\n';
					break;
				case 't':
					a_out += '\t';
					break;
				case 'r':
					a_out += '\r';
					break;
				case 'b':
					a_out += '\b';
					break;
				case 'f':
					a_out += '\f';
					break;
				case '/':
					a_out += '/';
					break;
				case '"':
					a_out += '"';
					break;
				case '\\':
					a_out += '\\';
					break;
				default:
					a_out += '\\';
					a_out += e;
					break;
				}
			}
			return false;
		}

		// The value of a top-level key, as text. Strings come back unquoted; numbers,
		// booleans and null come back verbatim. Absent key -> false.
		bool Field(std::string_view a_json, std::string_view a_key, std::string& a_out)
		{
			int depth = 0;
			std::size_t i = 0;
			while (i < a_json.size()) {
				const char c = a_json[i];
				if (c == '{' || c == '[') {
					++depth;
					++i;
					continue;
				}
				if (c == '}' || c == ']') {
					--depth;
					++i;
					continue;
				}
				if (c != '"') {
					++i;
					continue;
				}
				// A string here is either a key or a value; only a key at depth 1 is
				// ours, and only when a ':' follows it.
				std::string name;
				if (!ReadString(a_json, i, name)) {
					return false;
				}
				SkipWs(a_json, i);
				if (i >= a_json.size() || a_json[i] != ':') {
					continue;  // it was a value, not a key; keep scanning
				}
				++i;
				SkipWs(a_json, i);
				// A payload ending immediately after the colon leaves i at size, and
				// the value dispatch below indexes it. Without this the read runs one
				// past the end of the view.
				if (i >= a_json.size()) {
					return false;
				}

				const bool wanted = (depth == 1 && name == a_key);
				if (a_json[i] == '"') {
					std::string value;
					if (!ReadString(a_json, i, value)) {
						return false;
					}
					if (wanted) {
						a_out = std::move(value);
						return true;
					}
				} else if (a_json[i] == '{' || a_json[i] == '[') {
					// A nested value: fall through so the loop counts its braces. A
					// caller asking for it gets "not found", which is correct - this
					// reader does not do structures.
					if (wanted) {
						return false;
					}
				} else {
					const std::size_t vs = i;
					while (i < a_json.size() && a_json[i] != ',' && a_json[i] != '}' && a_json[i] != ']') {
						++i;
					}
					std::size_t ve = i;
					while (ve > vs && (a_json[ve - 1] == ' ' || a_json[ve - 1] == '\t')) {
						--ve;
					}
					if (wanted) {
						a_out.assign(a_json.substr(vs, ve - vs));
						return true;
					}
				}
			}
			return false;
		}

		[[nodiscard]] std::string Escape(std::string_view a_in)
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

		[[nodiscard]] std::string Quote(std::string_view a_in) { return "\"" + Escape(a_in) + "\""; }

		[[nodiscard]] std::string Error(std::string_view a_msg)
		{
			return "{\"ok\":false,\"error\":" + Quote(a_msg) + "}";
		}

		// The action an empty or malformed args object means. One spelling, read by
		// both the schema and the dispatch.
		constexpr std::string_view kDefaultAction = "state"sv;

		// the descriptor
		//
		// Built from DevBench::Routes(), which is the same array the dispatch below
		// and DevBench::Route() both read. The descriptor IS the agent's only view
		// of this tool, so a hand-written second copy of the action list is a bug
		// waiting to happen - the reference published nine actions while handling
		// thirteen, and the four extras were unreachable in practice.
		//
		// A function-local static: the host may hold the pointer we hand it, so the
		// string has to outlive Register().
		[[nodiscard]] const std::string& Descriptor()
		{
			static const std::string s_descriptor = [] {
				std::string desc =
					"Fo4VR Upscaler diagnostics. This build has five fail-soft render hooks; default settings transform no pixels, while all named development gates are bounded and reported. "
					"Routes report measured state or name the measurement that remains; none invents a number. "
					"Actions:";
				for (const auto& r : DevBench::Routes()) {
					desc += " '";
					desc += r.action;
					desc += "' - ";
					desc += r.description;
				}
				desc +=
					" Absent on purpose rather than answering with a placeholder: perf, addresses, general target readback, jitter, "
					"and mode. Each one arrives with the subsystem it reports on. Whole-frame timings, memory "
					"reads and log tails are the host's own tools; this one does not duplicate them.";

				std::string out = "{\"description\":" + Quote(desc);
				// 'reload' replaces the live settings, so this tool is not read-only.
				out += ",\"readOnly\":false";
				out += ",\"inputSchema\":{\"type\":\"object\",\"properties\":{";
				out += "\"request\":{\"type\":\"boolean\",\"default\":false,\"description\":\"historyReset only: request one paired diagnostic reset; omitted or false only reads counters\"},";
				out += "\"action\":{\"type\":\"string\",\"default\":" + Quote(kDefaultAction) + ",\"enum\":[";
				bool first = true;
				for (const auto& r : DevBench::Routes()) {
					if (!first) {
						out += ",";
					}
					first = false;
					out += Quote(r.action);
				}
				out += "]},\"reset\":{\"type\":\"boolean\",\"default\":false,\"description\":\"perf only: atomically request a clean timing window, then poll perf until resetCompletedGeneration reaches the returned generation\"},\"skipBypasses\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":511,\"default\":0,\"description\":\"captureBypass/captureInputs only: ignore this many accepted bypass frames before readback\"},\"skipHandbacks\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":511,\"default\":0,\"description\":\"captureFfx only: ignore this many fresh accepted FFX handbacks before readback\"},\"jitterPhase\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7,\"description\":\"captureTaa/capturePrivate/capturePipeline/captureBypass/captureInputs/captureFfx only: wait for this verified native eight-phase jitter position\"},\"faultSeam\":{\"type\":\"string\",\"enum\":[\"none\",\"preBackup\",\"postBackup\",\"postRt1\",\"postTaaStage\",\"prePublish\",\"postPublish\",\"copyCloseFailure\",\"seamDrawCloseFailure\",\"captureNotifyFailure\",\"geometryPreflightFailure\",\"geometryRestoreFailure\",\"parityDriftFailure\"],\"description\":\"injectQualityFault only: arm one deterministic quality-transaction seam (pre-RT1 aborts, native fallbacks at postRt1/postTaaStage/prePublish, terminal postPublish); copyCloseFailure/seamDrawCloseFailure/captureNotifyFailure/geometryPreflightFailure/geometryRestoreFailure/parityDriftFailure fire after their real mutating call or validation read instead of declining before it and are session-terminal like postPublish - geometryPreflightFailure/geometryRestoreFailure trip Hooks' own dynamic-geometry latch, and parityDriftFailure trips TAAReplacement's own session latch, neither this file's session latch directly; none disarms\"},\"faultCount\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":64,\"default\":1,\"description\":\"injectQualityFault only: atomically consume this many faults; values above 1 are limited to postTaaStage/prePublish\"},\"veto\":{\"type\":\"boolean\",\"description\":\"ab only: true makes the world gate deny every frame so the engine renders native; false releases it, and the gate re-admits only after the ordinary stabilizing count. Omit to report the current veto state.\"},\"mode\":{\"type\":\"string\",\"enum\":[\"off\",\"dlaa\",\"quality\",\"balanced\",\"performance\",\"ultraperformance\",\"toml\"],\"description\":\"mode only: MUTATES. Requests a live mode switch; 'off' is always reachable, any reduced mode is reachable whenever this session's bracket is armed (bracketArmed - the engine's admission gating is frozen for the process, but a different reduced mode's default ratio is now rewritten into the live engine ratio rather than refused), and 'dlaa' only when bracketArmed is false (refused-bracket-armed otherwise) - a session whose bracket is armed can never reach native output live except through 'off'. 'toml' clears a standing override. Omit to report the current live mode, its source, whether the bracket is armed, the session's armed mode and the last switch outcome.\"},\"censusAction\":{\"type\":\"string\",\"enum\":[\"arm\"],\"description\":\"observeFrame only: arm one read-only census of the next frame's ImageSpace chain. Omit to report the last completed census.\"},\"uiAction\":{\"type\":\"string\",\"enum\":[\"arm\"],\"description\":\"observeUi only: arm one read-only run of samples at the UI render entry - the Interface3D call inside US_RenderWorldImageSpaceSequence, 0x43 bytes after the late ImageSpace range and therefore after this plugin's last seam, where the state the HUD draw runs under is readable and the frame census cannot reach. Not Swap's call into the same renderer list: that one is the no-world path. Omit to report the last completed run.\"},\"uiFrames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":64,\"default\":64,\"description\":\"observeUi only: how many consecutive UI entries the armed run records. A run, not a one-shot, because the question is whether a value changes every frame or is constant.\"},\"uiStencilFrames\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":16,\"default\":0,\"description\":\"observeUi only: how many of the run's FIRST samples also read back DS1's stencil plane, reduced to a per-eye non-zero bounding box and a value histogram. 0 reads none. Each read copies the whole depth-stencil into a staging texture and maps it - about 30 MB and a GPU wait, on the render thread, between the late ImageSpace range and the UI draw - so a run that takes one is not a frame-timing measurement.\"},\"waterAction\":{\"type\":\"string\",\"enum\":[\"arm\"],\"description\":\"observeWater only: arm one read-only run of samples at BSWaterShader's per-geometry constant setup, where the water surface's screen / dynamic-resolution constant and its two per-eye TextureProj matrices are readable. Omit to report the last completed run.\"},\"waterSamples\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32,\"default\":32,\"description\":\"observeWater only: how many consecutive water setups the armed run records. A run, not a one-shot, because a frame contains many water draws and a per-draw constant must be told from a per-frame one.\"},\"hotkeyAction\":{\"type\":\"string\",\"enum\":[\"toggle\",\"compare\",\"panel\"],\"description\":\"hotkey only: MUTATES. Which binding to simulate a raw key sample for: toggle acts on the press edge only (one press is one action), compare is a debounced level held for as long as it reads down, not an edge, and panel is an edge like toggle that flips the in-headset overlay panel between shown and hidden. Must be supplied together with pressed. Omit both to report hotkey state only.\"},\"panelAction\":{\"type\":\"string\",\"enum\":[\"show\",\"hide\",\"toggle\",\"status\",\"capture\",\"event\"],\"description\":\"panel only: MUTATES unless it is status. show, hide and toggle publish the intent the render thread seam honours on its next frame, so a status read straight after one can still report overlayCreated false; capture arms a one-shot readback of the panel texture, after which status is polled until captureState reads saved and capturePath names the file. event injects one synthetic input through the identical handler the runtime's own overlay events go through, taking kind and x/y/button/dy below; every reply also carries the control rectangles from the last drawn frame under controls, so a caller clicks mode-off, ratio-slider or preset-K by name rather than by layout. Omit to report the panel state only.\"},\"kind\":{\"type\":\"string\",\"enum\":[\"move\",\"down\",\"up\",\"scroll\"],\"description\":\"panel action=event only: MUTATES. Which input to inject.\"},\"x\":{\"type\":\"number\",\"description\":\"panel action=event only: the x co-ordinate in panel pixels, TOP-LEFT origin - the same co-ordinates the controls array reports, and the same ones ImGui sees after the flip a real overlay mouse event gets.\"},\"y\":{\"type\":\"number\",\"description\":\"panel action=event only: the y co-ordinate in panel pixels, top-left origin.\"},\"button\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2,\"default\":0,\"description\":\"panel action=event only: 0 left, 1 right, 2 middle.\"},\"dy\":{\"type\":\"number\",\"description\":\"panel action=event only: the scroll amount for kind=scroll.\"},\"pressed\":{\"type\":\"boolean\",\"description\":\"hotkey only: MUTATES. true simulates a key-down sample, false a key-up sample, driving the same debounce/dispatch code the real per-frame render-thread poll uses - a press edge for toggle, one sample of the debounced level for compare. Must be supplied together with hotkeyAction. Omit both to report hotkey state only.\"},\"hudMessage\":{\"type\":\"string\",\"description\":\"hudmsg only: MUTATES. Enqueues this exact line through the bounded-ring/throttle/F4SE-task HUD message path. Omit to leave the queue untouched.\"},\"hudPoll\":{\"type\":\"boolean\",\"description\":\"hudmsg only: MUTATES (internal state only, no pixels or D3D11 state). true calls the world-admission/session-recovery edge detector once against genuine current state. Omit to leave it uncalled.\"},\"key\":{\"type\":\"string\",\"description\":\"configSet only: MUTATES. The declared settings key to change, exactly as /config reports it (either section). Must be supplied together with value.\"},\"value\":{\"type\":\"string\",\"description\":\"configSet only: MUTATES. The new value as text (true/false for a bool key, an integer or number literal, or a bare string with no quoting). Trial-validated against Settings::Validate() before anything is persisted; refused if Validate() would itself alter or reject it. Must be supplied together with key.\"}}}}";
				return out;
			}();
			return s_descriptor;
		}

		// the upscaler tool
		//
		// One tool with an `action`, not five tools: the host's own guidance is to
		// keep the agent-facing surface small, and the same guidance records an A/B
		// finding that a cold agent finds a well-named top-level tool and misses a
		// capability hidden inside an existing base tool's extra arguments.
		//
		// Dispatch remains a route lookup. Action-specific selectors are copied into
		// the route request only for their actions, so unrelated actions cannot
		// accidentally acquire a parameter contract.
		void UpscalerTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			if (!a_write) {
				return;
			}
			try {
				const std::string_view args = (a_argsJson && *a_argsJson) ? std::string_view{ a_argsJson } : std::string_view{ "{}" };

				std::string action;
				if (!Field(args, "action", action) || action.empty()) {
					action.assign(kDefaultAction);  // the overwhelmingly common ask
				}

				for (const auto& r : DevBench::Routes()) {
					if (r.action == action) {
						std::vector<std::pair<std::string, std::string>> params;
						if (action == "historyReset"sv) {
							std::string request;
							if (Field(args, "request", request)) {
								params.emplace_back("request", std::move(request));
							}
						}
						if (action == "perf"sv) {
							std::string reset;
							if (Field(args, "reset", reset)) {
								params.emplace_back("reset", std::move(reset));
							}
						}
						if (action == "captureBypass"sv || action == "captureInputs"sv) {
							std::string skipBypasses;
							if (Field(args, "skipBypasses", skipBypasses)) {
								params.emplace_back("skipBypasses", std::move(skipBypasses));
							}
						}
						if (action == "captureFfx"sv) {
							std::string skipHandbacks;
							if (Field(args, "skipHandbacks", skipHandbacks)) {
								params.emplace_back("skipHandbacks", std::move(skipHandbacks));
							}
						}
						if (action == "captureTaa"sv || action == "capturePrivate"sv || action == "capturePipeline"sv || action == "captureBypass"sv || action == "captureInputs"sv || action == "captureFfx"sv) {
							std::string jitterPhase;
							if (Field(args, "jitterPhase", jitterPhase)) {
								params.emplace_back("jitterPhase", std::move(jitterPhase));
							}
						}
						if (action == "ab"sv) {
							std::string veto;
							if (Field(args, "veto", veto)) {
								params.emplace_back("veto", std::move(veto));
							}
						}
						if (action == "mode"sv) {
							std::string mode;
							if (Field(args, "mode", mode)) {
								params.emplace_back("mode", std::move(mode));
							}
						}
						if (action == "configSet"sv) {
							std::string key;
							if (Field(args, "key", key)) {
								params.emplace_back("key", std::move(key));
							}
							std::string value;
							if (Field(args, "value", value)) {
								params.emplace_back("value", std::move(value));
							}
						}
						if (action == "panel"sv) {
							std::string panelAction;
							if (Field(args, "panelAction", panelAction) && !panelAction.empty()) {
								params.emplace_back("panelAction", std::move(panelAction));
							}
							// panelAction=event's own five. Forwarded here rather than
							// generically because this bridge whitelists every parameter it passes
							// on: a field nothing forwards reaches the route as absent, which is a
							// silent no-op rather than an error.
							for (const auto* const name : { "kind", "x", "y", "button", "dy" }) {
								std::string value;
								if (Field(args, name, value) && !value.empty()) {
									params.emplace_back(name, std::move(value));
								}
							}
						}
						if (action == "observeFrame"sv) {
							std::string censusAction;
							if (Field(args, "censusAction", censusAction) && !censusAction.empty()) {
								params.emplace_back("censusAction", std::move(censusAction));
							}
						}
						if (action == "observeUi"sv) {
							for (const auto* const name : { "uiAction", "uiFrames", "uiStencilFrames" }) {
								std::string value;
								if (Field(args, name, value) && !value.empty()) {
									params.emplace_back(name, std::move(value));
								}
							}
						}
						if (action == "observeWater"sv) {
							for (const auto* const name : { "waterAction", "waterSamples" }) {
								std::string value;
								if (Field(args, name, value) && !value.empty()) {
									params.emplace_back(name, std::move(value));
								}
							}
						}
						if (action == "hotkey"sv) {
							std::string hotkeyAction;
							if (Field(args, "hotkeyAction", hotkeyAction)) {
								params.emplace_back("hotkeyAction", std::move(hotkeyAction));
							}
							std::string pressed;
							if (Field(args, "pressed", pressed)) {
								params.emplace_back("pressed", std::move(pressed));
							}
						}
						if (action == "hudmsg"sv) {
							std::string hudMessage;
							if (Field(args, "hudMessage", hudMessage)) {
								params.emplace_back("hudMessage", std::move(hudMessage));
							}
							std::string hudPoll;
							if (Field(args, "hudPoll", hudPoll)) {
								params.emplace_back("hudPoll", std::move(hudPoll));
							}
						}
						if (action == "injectQualityFault"sv) {
							std::string faultSeam;
							if (Field(args, "faultSeam", faultSeam)) {
								params.emplace_back("faultSeam", std::move(faultSeam));
							}
							std::string faultCount;
							if (Field(args, "faultCount", faultCount)) {
								params.emplace_back("faultCount", std::move(faultCount));
							}
						}
						const std::string body = DevBench::Invoke(r.path, params);
						a_write(a_sink, body.c_str());
						return;
					}
				}

				std::string known;
				for (const auto& r : DevBench::Routes()) {
					if (!known.empty()) {
						known += "|";
					}
					known += r.action;
				}
				a_write(a_sink, Error("unknown action '" + action + "' (" + known + ")").c_str());
			} catch (const std::exception& e) {
				// An exception must not cross the C-ABI, where it would unwind through
				// the host's frames. DevBench::Invoke guards its own handlers; this
				// guards the marshalling either side of it.
				a_write(a_sink, Error(std::string{ "tool threw: " } + e.what()).c_str());
			} catch (...) {
				a_write(a_sink, Error("tool threw a non-standard exception").c_str());
			}
		}
	}

	void Register()
	{
		// Latched so a retry cannot spam the log. Each latch is set by the branch
		// that owns it, and none of them is re-armed - a once-per-session line that
		// re-arms inside the caller's loop is how a single line becomes a per-frame
		// line.
		static bool s_disabledLogged = false;
		static bool s_absentLogged = false;
		static bool s_noInterfaceLogged = false;

		if (g_connected.load()) {
			return;
		}

		if (!*Settings::devBench) {
			if (!s_disabledLogged) {
				s_disabledLogged = true;
				logger::info("devBench is false in the TOML; the upscaler tool was not registered"sv);
			}
			return;
		}

		// Module probe before the dispatch: dispatching to an absent plugin makes
		// CommonLib log "failed to dispatch to devbench" at warn, which reads like
		// an error in a public build. devbench is a development dependency, never a
		// load-order requirement.
		if (!::GetModuleHandleW(L"devbench.dll")) {
			if (!s_absentLogged) {
				s_absentLogged = true;
				logger::info("devbench not present; the upscaler tool was not registered (dev tooling - the mod is unaffected)"sv);
			}
			return;
		}

		auto* api = DevBenchAPI::GetDevBenchInterface001();
		if (!api) {
			if (!s_noInterfaceLogged) {
				s_noInterfaceLogged = true;
				logger::info("devbench present but no interface; the upscaler tool was not registered"sv);
			}
			return;
		}

		const auto build = api->GetBuildNumber();
		// false means a tool of this name was already registered and has just been
		// replaced. Worth a line: two plugins claiming one name is a real conflict
		// and the loser is silent otherwise.
		if (!api->RegisterTool("upscaler", Descriptor().c_str(), &UpscalerTool, nullptr)) {
			logger::warn("a devbench tool named 'upscaler' already existed and was replaced"sv);
		}
		g_connected.store(true);
		logger::info(FMT_STRING("devbench build {} - registered the 'upscaler' tool with {} action(s)"), build, DevBench::Routes().size());
	}

	bool Connected()
	{
		return g_connected.load();
	}
}
