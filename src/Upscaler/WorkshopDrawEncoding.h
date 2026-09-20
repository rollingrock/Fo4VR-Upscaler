#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Upscaler::WorkshopDrawEncoding
{
	constexpr std::size_t kAbsoluteJumpBytes = 14;
	constexpr std::size_t kDrawIndexedGatewayBytes = 19;
	constexpr std::size_t kDrawGatewayBytes = 16;
	constexpr std::size_t kDynamicDrawIndexedGatewayBytes = 13;

	[[nodiscard]] inline bool EncodeRelative(
		std::uint8_t* a_output,
		std::uint8_t a_opcode,
		std::uintptr_t a_instruction,
		std::uintptr_t a_target) noexcept
	{
		const auto next = a_instruction + 5;
		const auto displacement =
			static_cast<std::int64_t>(a_target) - static_cast<std::int64_t>(next);
		if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
			displacement > (std::numeric_limits<std::int32_t>::max)()) {
			return false;
		}

		a_output[0] = a_opcode;
		const auto relative = static_cast<std::int32_t>(displacement);
		std::memcpy(a_output + 1, &relative, sizeof(relative));
		return true;
	}

	inline void WriteAbsoluteJump(
		std::uint8_t* a_output,
		std::uintptr_t a_target) noexcept
	{
		static constexpr std::uint8_t kJumpIndirectRip[] = {
			0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
		};
		std::memcpy(a_output, kJumpIndirectRip, sizeof(kJumpIndirectRip));
		std::memcpy(
			a_output + sizeof(kJumpIndirectRip),
			&a_target,
			sizeof(a_target));
	}

	[[nodiscard]] inline bool BuildDrawIndexedGateway(
		std::uint8_t* a_output,
		std::uintptr_t a_gateway,
		std::uintptr_t a_wrapper,
		std::uintptr_t a_epilogue) noexcept
	{
		static constexpr std::uint8_t kArguments[] = {
			0x8D, 0x14, 0x7F,              // lea edx,[rdi+rdi*2]
			0x45, 0x33, 0xC9,              // xor r9d,r9d
			0x44, 0x8B, 0xC5               // mov r8d,ebp
		};
		std::memcpy(a_output, kArguments, sizeof(kArguments));
		std::size_t cursor = sizeof(kArguments);
		if (!EncodeRelative(a_output + cursor, 0xE8, a_gateway + cursor, a_wrapper)) {
			return false;
		}
		cursor += 5;
		return EncodeRelative(a_output + cursor, 0xE9, a_gateway + cursor, a_epilogue);
	}

	[[nodiscard]] inline bool BuildDrawGateway(
		std::uint8_t* a_output,
		std::uintptr_t a_gateway,
		std::uintptr_t a_wrapper,
		std::uintptr_t a_epilogue) noexcept
	{
		static constexpr std::uint8_t kArguments[] = {
			0x8D, 0x14, 0x7F,              // lea edx,[rdi+rdi*2]
			0x45, 0x33, 0xC0               // xor r8d,r8d
		};
		std::memcpy(a_output, kArguments, sizeof(kArguments));
		std::size_t cursor = sizeof(kArguments);
		if (!EncodeRelative(a_output + cursor, 0xE8, a_gateway + cursor, a_wrapper)) {
			return false;
		}
		cursor += 5;
		return EncodeRelative(a_output + cursor, 0xE9, a_gateway + cursor, a_epilogue);
	}

	[[nodiscard]] inline bool BuildDynamicDrawIndexedGateway(
		std::uint8_t* a_output,
		std::uintptr_t a_gateway,
		std::uintptr_t a_wrapper,
		std::uintptr_t a_continuation) noexcept
	{
		static constexpr std::uint8_t kStartIndex[] = {
			0x44, 0x8B, 0xC7  // mov r8d,edi
		};
		std::memcpy(a_output, kStartIndex, sizeof(kStartIndex));
		std::size_t cursor = sizeof(kStartIndex);
		if (!EncodeRelative(a_output + cursor, 0xE8, a_gateway + cursor, a_wrapper)) {
			return false;
		}
		cursor += 5;
		return EncodeRelative(
			a_output + cursor, 0xE9, a_gateway + cursor, a_continuation);
	}
}
