#pragma once

#include <d3d11.h>

#include <cstdint>

namespace Upscaler::WorkshopDrawHooks
{
	// Experimental game-call-site surface for the workshop-mask investigation.
	// These are the four ID3D11DeviceContext routes used by
	// US_BSGraphics_DrawCommittedGeometry in Fallout4VR.exe 1.2.72. The hooks do
	// not patch the COM vtable and do not claim that every prepared geometry type
	// reaches this helper.
	enum class Route : std::uint8_t
	{
		kDrawIndexed,
		kDraw,
		kDrawIndexedInstanced,
		kDrawInstanced,
	};

	struct DrawArguments
	{
		std::uint32_t countPerInstance{ 0 };
		std::uint32_t instanceCount{ 0 };
		std::uint32_t startIndexOrVertex{ 0 };
		std::int32_t baseVertex{ 0 };
		std::uint32_t startInstance{ 0 };
	};

	enum class Decision : std::uint8_t
	{
		kPassThrough,
		kOwned,
		kSkip,
	};

	using BeforeDrawCallback = Decision (*)(
		ID3D11DeviceContext* a_context,
		Route a_route,
		const DrawArguments& a_arguments) noexcept;

	using AfterDrawCallback = void (*)(
		ID3D11DeviceContext* a_context,
		Route a_route,
		const DrawArguments& a_arguments,
		Decision a_decision) noexcept;

	// Verifies every complete replacement span before allocating or writing any
	// code. Installation becomes visible to the wrappers only after all six game
	// sites are patched. A mismatch leaves the executable byte-for-byte untouched.
	[[nodiscard]] bool Install(
		BeforeDrawCallback a_before,
		AfterDrawCallback a_after) noexcept;

	[[nodiscard]] bool Installed() noexcept;
}
