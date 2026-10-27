#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "Upscaler/WorkshopDrawEncoding.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace Encoding = Upscaler::WorkshopDrawEncoding;

namespace
{
	enum class Route : std::uint8_t
	{
		drawIndexed,
		draw,
		drawIndexedInstanced,
		drawInstanced,
	};

	enum class Decision : std::uint8_t
	{
		passThrough,
		owned,
		skip,
	};

	struct Arguments
	{
		std::uint32_t count;
		std::uint32_t instances;
		std::uint32_t start;
		std::int32_t base;
		std::uint32_t startInstance;
	};

	struct Probe
	{
		Decision decision{ Decision::passThrough };
		std::uint32_t before{ 0 };
		std::uint32_t native{ 0 };
		std::uint32_t after{ 0 };
		void* context{ nullptr };
		Route route{ Route::drawIndexed };
		Arguments arguments{};
		std::uintptr_t returnAddress{ 0 };
	} g_probe;

	Decision Before(void* a_context, Route a_route, const Arguments& a_arguments) noexcept
	{
		++g_probe.before;
		g_probe.context = a_context;
		g_probe.route = a_route;
		g_probe.arguments = a_arguments;
		return g_probe.decision;
	}

	void After(Decision a_decision) noexcept
	{
		if (a_decision != Decision::passThrough) {
			++g_probe.after;
		}
	}

	extern "C" __declspec(noinline) void DrawIndexedHook(
		void* a_context, std::uint32_t a_count, std::uint32_t a_start, std::int32_t a_base) noexcept
	{
		g_probe.returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
		const Arguments arguments{ a_count, 1, a_start, a_base, 0 };
		const auto decision = Before(a_context, Route::drawIndexed, arguments);
		if (decision != Decision::skip) {
			++g_probe.native;
		}
		After(decision);
	}

	extern "C" __declspec(noinline) void DrawHook(
		void* a_context, std::uint32_t a_count, std::uint32_t a_start) noexcept
	{
		g_probe.returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
		const Arguments arguments{ a_count, 1, a_start, 0, 0 };
		const auto decision = Before(a_context, Route::draw, arguments);
		if (decision != Decision::skip) {
			++g_probe.native;
		}
		After(decision);
	}

	extern "C" __declspec(noinline) void DrawIndexedInstancedHook(
		void* a_context,
		std::uint32_t a_count,
		std::uint32_t a_instances,
		std::uint32_t a_start,
		std::int32_t a_base,
		std::uint32_t a_startInstance) noexcept
	{
		g_probe.returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
		const Arguments arguments{ a_count, a_instances, a_start, a_base, a_startInstance };
		const auto decision = Before(a_context, Route::drawIndexedInstanced, arguments);
		if (decision != Decision::skip) {
			++g_probe.native;
		}
		After(decision);
	}

	extern "C" __declspec(noinline) void DrawInstancedHook(
		void* a_context,
		std::uint32_t a_count,
		std::uint32_t a_instances,
		std::uint32_t a_start,
		std::uint32_t a_startInstance) noexcept
	{
		g_probe.returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
		const Arguments arguments{ a_count, a_instances, a_start, 0, a_startInstance };
		const auto decision = Before(a_context, Route::drawInstanced, arguments);
		if (decision != Decision::skip) {
			++g_probe.native;
		}
		After(decision);
	}

	void Put32(std::uint8_t* a_at, std::uint32_t a_value) noexcept
	{
		std::memcpy(a_at, &a_value, sizeof(a_value));
	}

	bool EmitNonInstancedCaller(
		std::uint8_t* a_output,
		std::uintptr_t a_target,
		std::uint32_t a_primitives,
		std::uint32_t a_start) noexcept
	{
		// Preserve the two nonvolatile registers used by the real function, provide
		// Win64 shadow space, seed RDI/RBP, then enter the exact shared gateway.
		static constexpr std::uint8_t kPrefix[] = {
			0x55, 0x57,              // push rbp; push rdi
			0x48, 0x83, 0xEC, 0x28,  // sub rsp,28h
			0xBF, 0, 0, 0, 0,        // mov edi,primitiveCount
			0xBD, 0, 0, 0, 0         // mov ebp,startIndex
		};
		std::memcpy(a_output, kPrefix, sizeof(kPrefix));
		Put32(a_output + 7, a_primitives);
		Put32(a_output + 12, a_start);
		return Encoding::EncodeRelative(
			a_output + sizeof(kPrefix),
			0xE9,
			reinterpret_cast<std::uintptr_t>(a_output) + sizeof(kPrefix),
			a_target);
	}

	bool EmitInstancedCaller(
		std::uint8_t* a_output,
		std::uintptr_t a_target,
		bool a_indexed,
		bool a_sevenBytePatch = false) noexcept
	{
		std::size_t cursor = 0;
		static constexpr std::uint8_t kSubRsp[] = { 0x48, 0x83, 0xEC, 0x38 };
		std::memcpy(a_output + cursor, kSubRsp, sizeof(kSubRsp));
		cursor += sizeof(kSubRsp);
		// mov edx,21; mov r8d,2; mov r9d,11
		a_output[cursor++] = 0xBA;
		Put32(a_output + cursor, 21);
		cursor += 4;
		a_output[cursor++] = 0x41;
		a_output[cursor++] = 0xB8;
		Put32(a_output + cursor, 2);
		cursor += 4;
		a_output[cursor++] = 0x41;
		a_output[cursor++] = 0xB9;
		Put32(a_output + cursor, 11);
		cursor += 4;
		// The indexed ABI has baseVertex at stack +20h and startInstance at +28h.
		// DrawInstanced has only startInstance at +20h.
		static constexpr std::uint8_t kStore20[] = { 0xC7, 0x44, 0x24, 0x20 };
		std::memcpy(a_output + cursor, kStore20, sizeof(kStore20));
		cursor += sizeof(kStore20);
		Put32(a_output + cursor, a_indexed ? static_cast<std::uint32_t>(-3) : 9);
		cursor += 4;
		if (a_indexed) {
			static constexpr std::uint8_t kStore28[] = { 0xC7, 0x44, 0x24, 0x28 };
			std::memcpy(a_output + cursor, kStore28, sizeof(kStore28));
			cursor += sizeof(kStore28);
			Put32(a_output + cursor, 9);
			cursor += 4;
		}
		if (!Encoding::EncodeRelative(
				a_output + cursor,
				0xE8,
				reinterpret_cast<std::uintptr_t>(a_output) + cursor,
				a_target)) {
			return false;
		}
		cursor += 5;
		if (a_sevenBytePatch) {
			// The dynamic-triangle source instruction is seven bytes; the production
			// replacement is the same rel32 call followed by two NOPs.
			a_output[cursor++] = 0x90;
			a_output[cursor++] = 0x90;
		}
		static constexpr std::uint8_t kReturn[] = { 0x48, 0x83, 0xC4, 0x38, 0xC3 };
		std::memcpy(a_output + cursor, kReturn, sizeof(kReturn));
		return true;
	}

	bool EmitDynamicIndexedCaller(
		std::uint8_t* a_output,
		std::uintptr_t a_target,
		std::uint32_t a_count,
		std::uint32_t a_start,
		std::int32_t a_base) noexcept
	{
		// Preserve RDI and align the stack with 20h of wrapper shadow space. EDX and
		// R9D arrive already live at the game gateway; its displaced mov r8d,edi
		// supplies only the start-index argument.
		static constexpr std::uint8_t kPrefix[] = {
			0x57,                    // push rdi
			0x48, 0x83, 0xEC, 0x20,  // sub rsp,20h
			0xBF, 0, 0, 0, 0,        // mov edi,startIndex
			0xBA, 0, 0, 0, 0,        // mov edx,indexCount
			0x41, 0xB9, 0, 0, 0, 0   // mov r9d,baseVertex
		};
		std::memcpy(a_output, kPrefix, sizeof(kPrefix));
		Put32(a_output + 6, a_start);
		Put32(a_output + 11, a_count);
		Put32(a_output + 17, static_cast<std::uint32_t>(a_base));
		return Encoding::EncodeRelative(
			a_output + sizeof(kPrefix),
			0xE9,
			reinterpret_cast<std::uintptr_t>(a_output) + sizeof(kPrefix),
			a_target);
	}

	bool SameArguments(const Arguments& a_left, const Arguments& a_right) noexcept
	{
		return std::memcmp(&a_left, &a_right, sizeof(Arguments)) == 0;
	}
}

int main()
{
	auto* const code = static_cast<std::uint8_t*>(::VirtualAlloc(
		nullptr, 1024, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!code) {
		std::puts("FAIL: VirtualAlloc");
		return 1;
	}

	auto* const indexedStub = code;
	auto* const drawStub = indexedStub + Encoding::kAbsoluteJumpBytes;
	auto* const indexedInstancedStub = drawStub + Encoding::kAbsoluteJumpBytes;
	auto* const instancedStub = indexedInstancedStub + Encoding::kAbsoluteJumpBytes;
	auto* const indexedGateway = instancedStub + Encoding::kAbsoluteJumpBytes;
	auto* const drawGateway = indexedGateway + Encoding::kDrawIndexedGatewayBytes;
	auto* const dynamicIndexedGateway = drawGateway + Encoding::kDrawGatewayBytes;
	auto* const epilogue = dynamicIndexedGateway + Encoding::kDynamicDrawIndexedGatewayBytes;
	auto* const dynamicEpilogue = epilogue + 16;
	auto* const indexedCaller = dynamicEpilogue + 16;
	auto* const drawCaller = indexedCaller + 32;
	auto* const indexedInstancedCaller = drawCaller + 32;
	auto* const instancedCaller = indexedInstancedCaller + 64;
	auto* const dynamicIndexedCaller = instancedCaller + 64;
	auto* const dynamicIndexedInstancedCaller = dynamicIndexedCaller + 64;

	Encoding::WriteAbsoluteJump(indexedStub, reinterpret_cast<std::uintptr_t>(&DrawIndexedHook));
	Encoding::WriteAbsoluteJump(drawStub, reinterpret_cast<std::uintptr_t>(&DrawHook));
	Encoding::WriteAbsoluteJump(indexedInstancedStub, reinterpret_cast<std::uintptr_t>(&DrawIndexedInstancedHook));
	Encoding::WriteAbsoluteJump(instancedStub, reinterpret_cast<std::uintptr_t>(&DrawInstancedHook));
	if (!Encoding::BuildDrawIndexedGateway(
			indexedGateway,
			reinterpret_cast<std::uintptr_t>(indexedGateway),
			reinterpret_cast<std::uintptr_t>(indexedStub),
			reinterpret_cast<std::uintptr_t>(epilogue)) ||
		!Encoding::BuildDrawGateway(
			drawGateway,
			reinterpret_cast<std::uintptr_t>(drawGateway),
			reinterpret_cast<std::uintptr_t>(drawStub),
			reinterpret_cast<std::uintptr_t>(epilogue)) ||
		!Encoding::BuildDynamicDrawIndexedGateway(
			dynamicIndexedGateway,
			reinterpret_cast<std::uintptr_t>(dynamicIndexedGateway),
			reinterpret_cast<std::uintptr_t>(indexedStub),
			reinterpret_cast<std::uintptr_t>(dynamicEpilogue))) {
		std::puts("FAIL: gateway encoding");
		return 1;
	}
	static constexpr std::uint8_t kEpilogue[] = {
		0x48, 0x83, 0xC4, 0x28,  // add rsp,28h
		0x5F, 0x5D, 0xC3         // pop rdi; pop rbp; ret
	};
	std::memcpy(epilogue, kEpilogue, sizeof(kEpilogue));
	static constexpr std::uint8_t kDynamicEpilogue[] = {
		0x48, 0x83, 0xC4, 0x20,  // add rsp,20h
		0x5F, 0xC3               // pop rdi; ret
	};
	std::memcpy(dynamicEpilogue, kDynamicEpilogue, sizeof(kDynamicEpilogue));
	if (!EmitNonInstancedCaller(indexedCaller, reinterpret_cast<std::uintptr_t>(indexedGateway), 7, 11) ||
		!EmitNonInstancedCaller(drawCaller, reinterpret_cast<std::uintptr_t>(drawGateway), 7, 99) ||
		!EmitInstancedCaller(indexedInstancedCaller, reinterpret_cast<std::uintptr_t>(indexedInstancedStub), true) ||
		!EmitInstancedCaller(instancedCaller, reinterpret_cast<std::uintptr_t>(instancedStub), false) ||
		!EmitDynamicIndexedCaller(
			dynamicIndexedCaller,
			reinterpret_cast<std::uintptr_t>(dynamicIndexedGateway),
			21,
			11,
			-3) ||
		!EmitInstancedCaller(
			dynamicIndexedInstancedCaller,
			reinterpret_cast<std::uintptr_t>(indexedInstancedStub),
			true,
			true)) {
		std::puts("FAIL: caller encoding");
		return 1;
	}
	static constexpr std::uint8_t kDynamicStartIndex[] = { 0x44, 0x8B, 0xC7 };
	constexpr std::size_t kIndexedInstancedPatchOffset = 37;
	if (std::memcmp(dynamicIndexedGateway, kDynamicStartIndex, sizeof(kDynamicStartIndex)) != 0 ||
		dynamicIndexedGateway[3] != 0x68 || dynamicIndexedGateway[8] != 0xC7 ||
		dynamicIndexedGateway[16] != 0xE9 ||
		dynamicIndexedInstancedCaller[kIndexedInstancedPatchOffset] != 0xE8 ||
		dynamicIndexedInstancedCaller[kIndexedInstancedPatchOffset + 5] != 0x90 ||
		dynamicIndexedInstancedCaller[kIndexedInstancedPatchOffset + 6] != 0x90) {
		std::puts("FAIL: dynamic replacement bytes");
		return 1;
	}
	::FlushInstructionCache(::GetCurrentProcess(), code, 1024);

	using Caller = void (*)(void*);
	const std::array<Caller, 6> callers{
		reinterpret_cast<Caller>(indexedCaller),
		reinterpret_cast<Caller>(drawCaller),
		reinterpret_cast<Caller>(indexedInstancedCaller),
		reinterpret_cast<Caller>(instancedCaller),
		reinterpret_cast<Caller>(dynamicIndexedCaller),
		reinterpret_cast<Caller>(dynamicIndexedInstancedCaller),
	};
	const std::array<Route, 6> expectedRoutes{
		Route::drawIndexed,
		Route::draw,
		Route::drawIndexedInstanced,
		Route::drawInstanced,
		Route::drawIndexed,
		Route::drawIndexedInstanced,
	};
	const std::array<Arguments, 6> expected{
		Arguments{ 21, 1, 11, 0, 0 },
		Arguments{ 21, 1, 0, 0, 0 },
		Arguments{ 21, 2, 11, -3, 9 },
		Arguments{ 21, 2, 11, 0, 9 },
		Arguments{ 21, 1, 11, -3, 0 },
		Arguments{ 21, 2, 11, -3, 9 },
	};
	// A wrapper's return address must be engine code, never the stub and gateway block,
	// which has no unwind data. The gateway routes return straight to their continuation.
	const auto stubBlockBegin = reinterpret_cast<std::uintptr_t>(code);
	const auto stubBlockEnd = reinterpret_cast<std::uintptr_t>(epilogue);
	const std::array<std::uintptr_t, 6> expectedReturns{
		reinterpret_cast<std::uintptr_t>(epilogue),
		reinterpret_cast<std::uintptr_t>(epilogue),
		0,
		0,
		reinterpret_cast<std::uintptr_t>(dynamicEpilogue),
		0,
	};
	const auto context = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x12345678));

	for (std::size_t route = 0; route < callers.size(); ++route) {
		for (const auto decision : { Decision::passThrough, Decision::owned, Decision::skip }) {
			g_probe = {};
			g_probe.decision = decision;
			callers[route](context);
			const auto expectedNative = decision == Decision::skip ? 0u : 1u;
			const auto expectedAfter = decision == Decision::passThrough ? 0u : 1u;
			const bool returnInStubBlock = g_probe.returnAddress >= stubBlockBegin &&
			                               g_probe.returnAddress < stubBlockEnd;
			if (g_probe.before != 1 || g_probe.native != expectedNative ||
				g_probe.after != expectedAfter || g_probe.context != context ||
				g_probe.route != expectedRoutes[route] ||
				!SameArguments(g_probe.arguments, expected[route]) ||
				returnInStubBlock ||
				(expectedReturns[route] != 0 && g_probe.returnAddress != expectedReturns[route])) {
				std::printf("FAIL: route=%zu decision=%u\n", route, static_cast<unsigned>(decision));
				return 1;
			}
		}
	}

	::VirtualFree(code, 0, MEM_RELEASE);
	std::puts("PASS: all six workshop draw sites preserve ABI, return into engine code and honor pass/owned/skip");
	return 0;
}
