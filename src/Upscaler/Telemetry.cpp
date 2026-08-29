#include "Upscaler/Telemetry.h"

namespace Upscaler::Telemetry
{
	void Begin()
	{
	}

	void Mark(std::uint32_t, std::string_view)
	{
	}

	void End()
	{
	}

	void Collect()
	{
	}

	std::string Report()
	{
		return R"({"enabled":false,"reason":"no d3d11 device and no timed stages on this build"})"s;
	}
}
