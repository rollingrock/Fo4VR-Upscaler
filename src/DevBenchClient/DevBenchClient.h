#pragma once

// This plugin as a consumer of alandtse/devbench.
//
// devbench is the general test bench: one tool registry reflected onto both MCP
// and REST, with a cross-plugin C-ABI that lets any other plugin register its
// own tools into that same registry. Registering there puts our diagnostics next
// to the host's `memory`, `rendertarget`, `measure` and `log` tools, so one
// session can read our state, dump a render target and sample frame times
// without three different clients.
//
// If devbench is not installed, every function here is a silent no-op. A
// research tool must never become a load-order requirement, and the absence is
// stated in the log in one plain line rather than surfacing as a warning that
// reads like an error in a public build.

namespace DevBenchClient
{
	// Fetch the interface and register the 'upscaler' tool.
	//
	// Call from the F4SE kPostPostLoad handler, NOT kPostLoad. F4SEVR's
	// RegisterListener(sender = nullptr) snapshots the listener slots that exist
	// at that moment and Dispatch only walks the sender's own slot, so a plugin
	// loading before devbench can dispatch into an empty list. devbench
	// re-registers at kPostLoad once the plugin list is complete, and
	// kPostPostLoad is dispatched immediately afterwards for exactly this
	// second-phase handshake. The field-observed F4SEVR message sequence is
	// 0, 1, 9 - kPostLoad, kPostPostLoad, kGameLoaded. kGameDataReady is never
	// dispatched on this platform, so nothing here may wait for it.
	//
	// Safe to call when devbench is absent, and safe to call more than once: the
	// second call is a no-op when the tool is already registered, and the
	// not-present and no-interface lines are latched so a retry cannot spam the
	// log.
	void Register();

	// True once the interface was obtained AND the tool registered.
	[[nodiscard]] bool Connected();
}
