#pragma once

// DevBench - the plugin's own diagnostic routes, so a question about what this
// build is actually doing does not cost a rebuild, a relaunch and a headset-on
// cycle. It ships with the plugin: served through the devbench host when one is
// installed, completely inert when one is not. There is no listener and no
// socket in here.
//
// Modelled on alandtse/devbench, which is GPL-3.0-or-later, so none of its
// source is vendored - only the shape of the idea: a route table answering
// hand-built JSON, no JSON library, inputs pre-decoded as key/value pairs.
//
// Most routes are diagnostics. Six mutate: /ab, /mode, /hotkey, /hudmsg,
// /quality/fault, /config/reload and /config/set. The capture routes arm one
// bounded readback at the existing render-thread seam; no listener-thread
// handler calls D3D11. No route reads raw process memory - the host's own
// `memory` tool does that better, and when we do need a guarded read it goes
// through Upscaler::FaultGuard rather than a second copy of the same __try here.
//
// Two things every mutating route obeys, written down because both were learned
// the expensive way:
//
//  - A value set through the bench survives the next reload. /config/set writes
//    it into the per-user override file the loader layers over the shipped TOML
//    (Settings::Writer, Settings::Paths::OverrideFilePath), so a reload cannot
//    silently revert it and an experiment cannot end up measuring the file
//    rather than the setting.
//  - Anything creating or destroying a D3D11 object, or touching engine state,
//    is marshalled onto the thread that owns it rather than run here: handlers
//    run on the host's listener thread, and F4SE's task interface reaches the
//    main thread, not the render thread - which is the sharper hazard for this
//    project than it ever was for a scope mod. /mode and /hotkey queue to the
//    game thread and report the outcome the queued work resolved to, never an
//    acknowledgement.

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DevBench
{
	// One route: the action an agent types, the internal path it maps to, and
	// the sentence the tool descriptor publishes for it.
	//
	// This is the single table. Route() switches on these paths, and
	// DevBenchClient generates both the descriptor's action enum and its own
	// action -> path dispatch from the same array, so the schema an agent reads
	// cannot drift from the set of routes that answer. Keeping those as two
	// hand-maintained lists is exactly how the reference ended up with four
	// working actions that no agent could discover.
	struct RouteInfo
	{
		std::string_view action;
		std::string_view path;
		std::string_view description;
	};

	[[nodiscard]] std::span<const RouteInfo> Routes() noexcept;

	// One-time init: the uptime baseline and one log line. Call once, from
	// F4SEPlugin_Load. It cannot fail and it is never fatal to the plugin.
	void Init();

	// Run one request and return its JSON body. The 'upscaler' tool registered
	// into the host (see src/DevBenchClient/) answers by running these handlers,
	// so there is exactly one implementation of every answer.
	//
	// a_path is a route in the old HTTP spelling ("/health", "/config/reload",
	// ...) and a_params are the decoded query parameters. An unknown path comes
	// back as the usual {"ok":false,...} error, never as an exception.
	//
	// Threading is the caller's problem: handlers take snapshots under the
	// settings lock or answer from immutable state. A capture handler only arms an
	// atomic/mutex-protected request; its D3D11 copy/map runs later at the TAA hook.
	// The host invokes tool handlers on its listener thread.
	//
	// A route whose data does not exist on this build says so and names what
	// blocks it. None of them invents a number.
	[[nodiscard]] std::string Invoke(
		std::string_view a_path,
		const std::vector<std::pair<std::string, std::string>>& a_params);
}
