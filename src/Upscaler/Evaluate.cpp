#include "Upscaler/Evaluate.h"

namespace Upscaler::Evaluate
{
	namespace
	{
		// The gate is closed on this build and there is nothing that can open it: no
		// hook calls Frame, and every module it would drive reports unresolved.
		[[nodiscard]] bool Gate() noexcept
		{
			return false;
		}
	}

	bool Frame()
	{
		// The gate is the first statement of the real body too. Nothing below it exists.
		return Gate();
	}

	bool Active() noexcept
	{
		return false;
	}
}
