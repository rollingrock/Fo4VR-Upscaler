#include "Upscaler/Engine.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/FaultGuard.h"

#include <cstring>

namespace Upscaler::Engine
{
	namespace
	{
		LogGate g_unresolved;
	}

	bool Init()
	{
		if (g_unresolved.Ready()) {
			logger::warn("engine handles unresolved: there is no verified rva for the immediate context or the graphics singletons yet. See docs/MODULE_MAP.md."sv);
		}
		return false;
	}

	ID3D11Device* Device() noexcept
	{
		return nullptr;
	}

	ID3D11DeviceContext* ImmediateContext() noexcept
	{
		return nullptr;
	}

	std::uintptr_t GraphicsState() noexcept
	{
		return 0;
	}

	// CommonLibF4's render-target manager and renderer-data array lengths are flat
	// Fallout 4 values and are wrong for VR. The base offset and the stride do transfer,
	// so these arrays get walked with hand-derived bounds, never with the library struct.
	std::uintptr_t RenderTargetManager() noexcept
	{
		return 0;
	}

	std::uintptr_t Renderer() noexcept
	{
		return 0;
	}

	std::uintptr_t RipResolve(
		std::uintptr_t a_instrRva,
		std::initializer_list<std::uint8_t> a_opcode,
		std::size_t a_dispOffset,
		std::size_t a_instrLen,
		std::string_view a_what)
	{
		if (a_instrRva == 0 || a_opcode.size() == 0 || a_instrLen < a_dispOffset + sizeof(std::int32_t)) {
			logger::critical(FMT_STRING("rip anchor {} is malformed"), a_what);
			return 0;
		}

		const auto addr = REL::Module::get().base() + a_instrRva;

		std::uint8_t bytes[16] = {};
		if (a_instrLen > sizeof(bytes) ||
			!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(addr), bytes, a_instrLen)) {
			logger::critical(FMT_STRING("rip anchor {} at {:016X} is unreadable"), a_what, addr);
			return 0;
		}

		std::size_t i = 0;
		for (const auto b : a_opcode) {
			if (bytes[i++] != b) {
				logger::critical(FMT_STRING("rip anchor mismatch for {} at {:016X}"), a_what, addr);
				return 0;
			}
		}

		std::int32_t disp = 0;
		std::memcpy(&disp, bytes + a_dispOffset, sizeof(disp));
		return addr + a_instrLen + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(disp));
	}
}
