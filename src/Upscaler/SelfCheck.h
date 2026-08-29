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

	// The same facts as one JSON object, for the dev tooling's state route.
	[[nodiscard]] std::string ReportJson();
}
