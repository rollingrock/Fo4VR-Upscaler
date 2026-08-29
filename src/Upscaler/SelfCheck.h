#pragma once

// The startup environment report, and the whole of what this build does. It runs once
// at load and again on request from the dev tooling, writes a block of info lines a bug
// report can be answered from without a second round trip, and never fails: a line it
// cannot answer says so and names the measurement it is waiting on, rather than
// printing a wrong answer or nothing at all.
//
// The block is bounded and one-shot. Nothing here is ever called per frame.

#include <string>

namespace Upscaler::SelfCheck
{
	// Writes the environment block to the log.
	void Run();

	// A second pass once the game is loaded, logging only what Run could not answer at
	// plugin load. Two facts are worth waiting for: DXGI refuses adapter enumeration that
	// early, so the GPU - the first thing any bug report needs - is missing from the
	// block; and plugins below us in the load order have not loaded yet, so the module
	// probes undercount. One-shot, and silent when Run already had the answers.
	void RunDeferred();

	// The same facts as one JSON object, for the dev tooling's state route.
	[[nodiscard]] std::string ReportJson();
}
