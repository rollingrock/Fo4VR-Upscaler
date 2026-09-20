#include "Upscaler/WorkshopDrawHooks.h"
#include "Upscaler/WorkshopDrawEncoding.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Upscaler::WorkshopDrawHooks
{
	namespace
	{
		// [GHIDRA] Fallout4VR.exe 1.2.72, image base 0x140000000.
		// US_BSGraphics_DrawCommittedGeometry @ 0x141d8e7c0 commits state at
		// 0x141d8e84a, performs IA setup, and reaches exactly one of these sites.
		constexpr std::uintptr_t kDrawIndexedInstancedCallRva = 0x1d8e907;
		constexpr std::uintptr_t kDrawIndexedGatewayRva = 0x1d8e919;
		constexpr std::uintptr_t kDrawInstancedCallRva = 0x1d8e988;
		constexpr std::uintptr_t kDrawGatewayRva = 0x1d8e997;
		constexpr std::uintptr_t kCommonEpilogueRva = 0x1d8e9a3;
		// US_BSGraphics_DrawDynamicTriangles @ 0x141d8f330 commits state at
		// 0x141d8f392, performs IA setup, and reaches one of these two sites.
		constexpr std::uintptr_t kDynamicDrawIndexedInstancedCallRva = 0x1d8f465;
		constexpr std::uintptr_t kDynamicDrawIndexedGatewayRva = 0x1d8f491;
		constexpr std::uintptr_t kDynamicDrawIndexedContinuationRva = 0x1d8f498;

		static constexpr std::array<std::uint8_t, 6> kDrawIndexedInstancedExpected{
			0xFF, 0x90, 0xA0, 0x00, 0x00, 0x00
		};
		static constexpr std::array<std::uint8_t, 6> kDrawInstancedExpected{
			0xFF, 0x90, 0xA8, 0x00, 0x00, 0x00
		};
		static constexpr std::array<std::uint8_t, 17> kDrawIndexedExpected{
			0x8D, 0x14, 0x7F,              // lea edx,[rdi+rdi*2]
			0x45, 0x33, 0xC9,              // xor r9d,r9d
			0x48, 0x8B, 0x01,              // mov rax,[rcx]
			0x44, 0x8B, 0xC5,              // mov r8d,ebp
			0xFF, 0x50, 0x60,              // call [rax+60h]
			0xEB, 0x79                     // jmp 141d8e9a3
		};
		static constexpr std::array<std::uint8_t, 12> kDrawExpected{
			0x8D, 0x14, 0x7F,              // lea edx,[rdi+rdi*2]
			0x45, 0x33, 0xC0,              // xor r8d,r8d
			0x48, 0x8B, 0x01,              // mov rax,[rcx]
			0xFF, 0x50, 0x68               // call [rax+68h]
		};
		static constexpr std::array<std::uint8_t, 7> kDynamicDrawIndexedInstancedExpected{
			0x41, 0xFF, 0x92, 0xA0, 0x00, 0x00, 0x00  // call [r10+0a0h]
		};
		static constexpr std::array<std::uint8_t, 7> kDynamicDrawIndexedExpected{
			0x44, 0x8B, 0xC7,              // mov r8d,edi
			0x41, 0xFF, 0x52, 0x60         // call [r10+60h]
		};

		constexpr std::size_t kWrapperCount = 4;
		constexpr std::size_t kTrampolineBytes =
			kWrapperCount * WorkshopDrawEncoding::kAbsoluteJumpBytes +
			WorkshopDrawEncoding::kDrawIndexedGatewayBytes +
			WorkshopDrawEncoding::kDrawGatewayBytes +
			WorkshopDrawEncoding::kDynamicDrawIndexedGatewayBytes;

		BeforeDrawCallback g_before = nullptr;
		AfterDrawCallback g_after = nullptr;
		std::atomic_bool g_installed{ false };
		bool g_installAttempted = false;

		[[nodiscard]] Decision Before(
			ID3D11DeviceContext* a_context,
			Route a_route,
			const DrawArguments& a_arguments) noexcept
		{
			if (!g_installed.load(std::memory_order_acquire) || !g_before) {
				return Decision::kPassThrough;
			}

			const auto decision = g_before(a_context, a_route, a_arguments);
			return decision == Decision::kOwned || decision == Decision::kSkip ?
			           decision :
			           Decision::kPassThrough;
		}

		void After(
			ID3D11DeviceContext* a_context,
			Route a_route,
			const DrawArguments& a_arguments,
			Decision a_decision) noexcept
		{
			if (a_decision != Decision::kPassThrough && g_after) {
				g_after(a_context, a_route, a_arguments, a_decision);
			}
		}

		void DrawIndexedThunk(
			ID3D11DeviceContext* a_context,
			UINT a_indexCount,
			UINT a_startIndex,
			INT a_baseVertex) noexcept
		{
			const DrawArguments arguments{
				a_indexCount, 1u, a_startIndex, a_baseVertex, 0u
			};
			const auto decision = Before(a_context, Route::kDrawIndexed, arguments);

			__try {
				if (decision != Decision::kSkip) {
					a_context->DrawIndexed(a_indexCount, a_startIndex, a_baseVertex);
				}
			} __finally {
				After(a_context, Route::kDrawIndexed, arguments, decision);
			}
		}

		void DrawThunk(
			ID3D11DeviceContext* a_context,
			UINT a_vertexCount,
			UINT a_startVertex) noexcept
		{
			const DrawArguments arguments{
				a_vertexCount, 1u, a_startVertex, 0, 0u
			};
			const auto decision = Before(a_context, Route::kDraw, arguments);

			__try {
				if (decision != Decision::kSkip) {
					a_context->Draw(a_vertexCount, a_startVertex);
				}
			} __finally {
				After(a_context, Route::kDraw, arguments, decision);
			}
		}

		void DrawIndexedInstancedThunk(
			ID3D11DeviceContext* a_context,
			UINT a_indexCountPerInstance,
			UINT a_instanceCount,
			UINT a_startIndex,
			INT a_baseVertex,
			UINT a_startInstance) noexcept
		{
			const DrawArguments arguments{
				a_indexCountPerInstance,
				a_instanceCount,
				a_startIndex,
				a_baseVertex,
				a_startInstance
			};
			const auto decision = Before(
				a_context, Route::kDrawIndexedInstanced, arguments);

			__try {
				if (decision != Decision::kSkip) {
					a_context->DrawIndexedInstanced(
						a_indexCountPerInstance,
						a_instanceCount,
						a_startIndex,
						a_baseVertex,
						a_startInstance);
				}
			} __finally {
				After(
					a_context,
					Route::kDrawIndexedInstanced,
					arguments,
					decision);
			}
		}

		void DrawInstancedThunk(
			ID3D11DeviceContext* a_context,
			UINT a_vertexCountPerInstance,
			UINT a_instanceCount,
			UINT a_startVertex,
			UINT a_startInstance) noexcept
		{
			const DrawArguments arguments{
				a_vertexCountPerInstance,
				a_instanceCount,
				a_startVertex,
				0,
				a_startInstance
			};
			const auto decision = Before(
				a_context, Route::kDrawInstanced, arguments);

			__try {
				if (decision != Decision::kSkip) {
					a_context->DrawInstanced(
						a_vertexCountPerInstance,
						a_instanceCount,
						a_startVertex,
						a_startInstance);
				}
			} __finally {
				After(
					a_context,
					Route::kDrawInstanced,
					arguments,
					decision);
			}
		}

		template <std::size_t N>
		[[nodiscard]] bool Matches(
			std::uintptr_t a_address,
			const std::array<std::uint8_t, N>& a_expected) noexcept
		{
			return std::memcmp(
				reinterpret_cast<const void*>(a_address),
				a_expected.data(),
				N) == 0;
		}

	}

	bool Install(
		BeforeDrawCallback a_before,
		AfterDrawCallback a_after) noexcept
	{
		if (g_installAttempted) {
			return false;
		}
		g_installAttempted = true;

		const REL::Relocation<std::uintptr_t> drawIndexedInstancedSite{
			REL::Offset(kDrawIndexedInstancedCallRva)
		};
		const REL::Relocation<std::uintptr_t> drawIndexedSite{
			REL::Offset(kDrawIndexedGatewayRva)
		};
		const REL::Relocation<std::uintptr_t> drawInstancedSite{
			REL::Offset(kDrawInstancedCallRva)
		};
		const REL::Relocation<std::uintptr_t> drawSite{
			REL::Offset(kDrawGatewayRva)
		};
		const REL::Relocation<std::uintptr_t> epilogue{
			REL::Offset(kCommonEpilogueRva)
		};
		const REL::Relocation<std::uintptr_t> dynamicDrawIndexedInstancedSite{
			REL::Offset(kDynamicDrawIndexedInstancedCallRva)
		};
		const REL::Relocation<std::uintptr_t> dynamicDrawIndexedSite{
			REL::Offset(kDynamicDrawIndexedGatewayRva)
		};
		const REL::Relocation<std::uintptr_t> dynamicDrawIndexedContinuation{
			REL::Offset(kDynamicDrawIndexedContinuationRva)
		};

		// All six full spans are checked before any allocation or executable write.
		// The non-instanced regions contain complete instructions, and Ghidra has no
		// incoming branch/xref to an address inside any displaced span.
		if (!Matches(drawIndexedInstancedSite.address(), kDrawIndexedInstancedExpected) ||
			!Matches(drawIndexedSite.address(), kDrawIndexedExpected) ||
			!Matches(drawInstancedSite.address(), kDrawInstancedExpected) ||
			!Matches(drawSite.address(), kDrawExpected) ||
			!Matches(
				dynamicDrawIndexedInstancedSite.address(),
				kDynamicDrawIndexedInstancedExpected) ||
			!Matches(dynamicDrawIndexedSite.address(), kDynamicDrawIndexedExpected)) {
			return false;
		}

		auto& trampoline = F4SE::GetTrampoline();
		if (trampoline.free_size() < kTrampolineBytes) {
			return false;
		}

		auto* const block = static_cast<std::uint8_t*>(
			trampoline.allocate(kTrampolineBytes));
		auto* const drawIndexedStub = block;
		auto* const drawStub = drawIndexedStub + WorkshopDrawEncoding::kAbsoluteJumpBytes;
		auto* const drawIndexedInstancedStub = drawStub + WorkshopDrawEncoding::kAbsoluteJumpBytes;
		auto* const drawInstancedStub = drawIndexedInstancedStub + WorkshopDrawEncoding::kAbsoluteJumpBytes;
		auto* const drawIndexedGateway = drawInstancedStub + WorkshopDrawEncoding::kAbsoluteJumpBytes;
		auto* const drawGateway = drawIndexedGateway + WorkshopDrawEncoding::kDrawIndexedGatewayBytes;
		auto* const dynamicDrawIndexedGateway = drawGateway + WorkshopDrawEncoding::kDrawGatewayBytes;

		WorkshopDrawEncoding::WriteAbsoluteJump(
			drawIndexedStub,
			reinterpret_cast<std::uintptr_t>(&DrawIndexedThunk));
		WorkshopDrawEncoding::WriteAbsoluteJump(
			drawStub,
			reinterpret_cast<std::uintptr_t>(&DrawThunk));
		WorkshopDrawEncoding::WriteAbsoluteJump(
			drawIndexedInstancedStub,
			reinterpret_cast<std::uintptr_t>(&DrawIndexedInstancedThunk));
		WorkshopDrawEncoding::WriteAbsoluteJump(
			drawInstancedStub,
			reinterpret_cast<std::uintptr_t>(&DrawInstancedThunk));

		std::array<std::uint8_t, WorkshopDrawEncoding::kDrawIndexedGatewayBytes> indexedGatewayBytes{};
		if (!WorkshopDrawEncoding::BuildDrawIndexedGateway(
				indexedGatewayBytes.data(),
				reinterpret_cast<std::uintptr_t>(drawIndexedGateway),
				reinterpret_cast<std::uintptr_t>(drawIndexedStub),
				epilogue.address())) {
			return false;
		}
		std::memcpy(
			drawIndexedGateway,
			indexedGatewayBytes.data(),
			indexedGatewayBytes.size());

		std::array<std::uint8_t, WorkshopDrawEncoding::kDrawGatewayBytes> gatewayBytes{};
		if (!WorkshopDrawEncoding::BuildDrawGateway(
				gatewayBytes.data(),
				reinterpret_cast<std::uintptr_t>(drawGateway),
				reinterpret_cast<std::uintptr_t>(drawStub),
				epilogue.address())) {
			return false;
		}
		std::memcpy(drawGateway, gatewayBytes.data(), gatewayBytes.size());

		std::array<std::uint8_t, WorkshopDrawEncoding::kDynamicDrawIndexedGatewayBytes>
			dynamicIndexedGatewayBytes{};
		if (!WorkshopDrawEncoding::BuildDynamicDrawIndexedGateway(
				dynamicIndexedGatewayBytes.data(),
				reinterpret_cast<std::uintptr_t>(dynamicDrawIndexedGateway),
				reinterpret_cast<std::uintptr_t>(drawIndexedStub),
				dynamicDrawIndexedContinuation.address())) {
			return false;
		}
		std::memcpy(
			dynamicDrawIndexedGateway,
			dynamicIndexedGatewayBytes.data(),
			dynamicIndexedGatewayBytes.size());

		std::array<std::uint8_t, 6> indexedInstancedPatch{};
		indexedInstancedPatch.fill(0x90);
		if (!WorkshopDrawEncoding::EncodeRelative(
				indexedInstancedPatch.data(),
				0xE8,
				drawIndexedInstancedSite.address(),
				reinterpret_cast<std::uintptr_t>(drawIndexedInstancedStub))) {
			return false;
		}

		std::array<std::uint8_t, 7> dynamicIndexedInstancedPatch{};
		dynamicIndexedInstancedPatch.fill(0x90);
		if (!WorkshopDrawEncoding::EncodeRelative(
				dynamicIndexedInstancedPatch.data(),
				0xE8,
				dynamicDrawIndexedInstancedSite.address(),
				reinterpret_cast<std::uintptr_t>(drawIndexedInstancedStub))) {
			return false;
		}

		std::array<std::uint8_t, 7> dynamicIndexedPatch{};
		dynamicIndexedPatch.fill(0x90);
		if (!WorkshopDrawEncoding::EncodeRelative(
				dynamicIndexedPatch.data(),
				0xE9,
				dynamicDrawIndexedSite.address(),
				reinterpret_cast<std::uintptr_t>(dynamicDrawIndexedGateway))) {
			return false;
		}

		std::array<std::uint8_t, 6> instancedPatch{};
		instancedPatch.fill(0x90);
		if (!WorkshopDrawEncoding::EncodeRelative(
				instancedPatch.data(),
				0xE8,
				drawInstancedSite.address(),
				reinterpret_cast<std::uintptr_t>(drawInstancedStub))) {
			return false;
		}

		std::array<std::uint8_t, 17> indexedPatch{};
		indexedPatch.fill(0x90);
		if (!WorkshopDrawEncoding::EncodeRelative(
				indexedPatch.data(),
				0xE9,
				drawIndexedSite.address(),
				reinterpret_cast<std::uintptr_t>(drawIndexedGateway))) {
			return false;
		}

		std::array<std::uint8_t, 12> drawPatch{};
		drawPatch.fill(0x90);
		if (!WorkshopDrawEncoding::EncodeRelative(
				drawPatch.data(),
				0xE9,
				drawSite.address(),
				reinterpret_cast<std::uintptr_t>(drawGateway))) {
			return false;
		}

		static_cast<void>(::FlushInstructionCache(
			::GetCurrentProcess(), block, kTrampolineBytes));

		// The wrappers remain unconditional native forwarding while these writes are
		// in progress. Publish callbacks only after the last complete site is patched.
		REL::safe_write(
			drawIndexedInstancedSite.address(),
			indexedInstancedPatch.data(),
			indexedInstancedPatch.size());
		REL::safe_write(
			drawIndexedSite.address(),
			indexedPatch.data(),
			indexedPatch.size());
		REL::safe_write(
			drawInstancedSite.address(),
			instancedPatch.data(),
			instancedPatch.size());
		REL::safe_write(
			drawSite.address(),
			drawPatch.data(),
			drawPatch.size());
		REL::safe_write(
			dynamicDrawIndexedInstancedSite.address(),
			dynamicIndexedInstancedPatch.data(),
			dynamicIndexedInstancedPatch.size());
		REL::safe_write(
			dynamicDrawIndexedSite.address(),
			dynamicIndexedPatch.data(),
			dynamicIndexedPatch.size());

		g_before = a_before;
		g_after = a_after;
		g_installed.store(true, std::memory_order_release);
		return true;
	}

	bool Installed() noexcept
	{
		return g_installed.load(std::memory_order_acquire);
	}
}
