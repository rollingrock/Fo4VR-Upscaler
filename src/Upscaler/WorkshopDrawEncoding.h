#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Upscaler::WorkshopDrawEncoding
{
	constexpr std::size_t kAbsoluteJumpBytes = 14;
	constexpr std::size_t kReturnThenJumpBytes = 18;
	constexpr std::size_t kDrawIndexedGatewayBytes = 9 + kReturnThenJumpBytes;
	constexpr std::size_t kDrawGatewayBytes = 6 + kReturnThenJumpBytes;
	constexpr std::size_t kDynamicDrawIndexedGatewayBytes = 3 + kReturnThenJumpBytes;

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

	// The gateways push the engine continuation as the return address and jump to the
	// wrapper stub. While the wrapper and the driver run, the frame above them is then
	// the engine function itself. VERIFIED (Fallout4VR.exe 1.2.72 exception directory):
	// 0x141d8e9a3 and 0x141d8f498 lie inside the unchained unwind entries of
	// DrawCommittedGeometry and DrawDynamicTriangles, and both begin with a mov restore
	// rather than an epilogue add, so the prolog codes unwind this rsp. A call from the
	// gateway would leave a return address in trampoline memory, for which nothing in
	// this tree or CommonLibF4 registers a function table, and stack walks through a
	// world draw would stop there.
	// A pushed return is not shadow-stack safe; the exe carries no CETCOMPAT marking
	// (VERIFIED: no extended DLL-characteristics debug entry), so shadow stacks are not
	// enforced for it unless forced per process.
	[[nodiscard]] inline bool EncodeReturnThenJump(
		std::uint8_t* a_output,
		std::uintptr_t a_instruction,
		std::uintptr_t a_return,
		std::uintptr_t a_target) noexcept
	{
		static constexpr std::uint8_t kStoreHigh[] = { 0xC7, 0x44, 0x24, 0x04 };  // mov dword [rsp+4],imm32
		const auto low = static_cast<std::uint32_t>(a_return);
		const auto high = static_cast<std::uint32_t>(a_return >> 32);
		a_output[0] = 0x68;  // push imm32, sign-extended; the high half is stored next
		std::memcpy(a_output + 1, &low, sizeof(low));
		std::memcpy(a_output + 5, kStoreHigh, sizeof(kStoreHigh));
		std::memcpy(a_output + 9, &high, sizeof(high));
		return EncodeRelative(a_output + 13, 0xE9, a_instruction + 13, a_target);
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
		const std::size_t cursor = sizeof(kArguments);
		return EncodeReturnThenJump(a_output + cursor, a_gateway + cursor, a_epilogue, a_wrapper);
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
		const std::size_t cursor = sizeof(kArguments);
		return EncodeReturnThenJump(a_output + cursor, a_gateway + cursor, a_epilogue, a_wrapper);
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
		const std::size_t cursor = sizeof(kStartIndex);
		return EncodeReturnThenJump(
			a_output + cursor, a_gateway + cursor, a_continuation, a_wrapper);
	}
}
