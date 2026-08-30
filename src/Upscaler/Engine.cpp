#include "Upscaler/Engine.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/FaultGuard.h"
#include "Upscaler/RenderTargets.h"

#include <d3d11.h>

#include <cstring>

namespace Upscaler::Engine
{
	namespace
	{
		// Resolved once at load. These are addresses, not values: the two state entries
		// and the context entry are the addresses of pointer slots, and the manager and
		// renderer entries are the addresses of the objects themselves. Reading through
		// the slots on every call rather than caching their contents is deliberate - the
		// engine owns those slots and may swap them.
		std::uintptr_t g_stateSlot = 0;
		std::uintptr_t g_stateAltSlot = 0;
		std::uintptr_t g_ctxSlot = 0;
		std::uintptr_t g_rtm = 0;
		std::uintptr_t g_renderer = 0;
		std::uintptr_t g_deviceSlot = 0;
		bool g_resolved = false;

		LogGate g_deviceGate;
	}

	bool ReadU32(std::uintptr_t a_address, std::uint32_t& a_out) noexcept
	{
		std::uint32_t v = 0;
		if (a_address == 0 ||
			!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(a_address), &v, sizeof(v))) {
			return false;
		}
		a_out = v;
		return true;
	}

	bool ReadI32(std::uintptr_t a_address, std::int32_t& a_out) noexcept
	{
		std::int32_t v = 0;
		if (a_address == 0 ||
			!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(a_address), &v, sizeof(v))) {
			return false;
		}
		a_out = v;
		return true;
	}

	bool ReadPtr(std::uintptr_t a_address, void*& a_out) noexcept
	{
		void* v = nullptr;
		if (a_address == 0 ||
			!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(a_address), &v, sizeof(v))) {
			return false;
		}
		a_out = v;
		return true;
	}

	bool Init()
	{
		if (g_resolved) {
			return true;
		}

		// Four of the five come out of one function, BSGraphics::Renderer::ClearColor,
		// which is small enough to read end to end and whose tail call to
		// ID3D11DeviceContext vtable slot 50 is what types the context slot in the first
		// place. The fifth is a plain LEA of the renderer elsewhere in BSGraphics.
		g_stateSlot = RipResolve(Addr::kGraphicsStateAnchor, { 0x48, 0x8B, 0x05 },
			Addr::kRipDispOffset, Addr::kRipInstrLen, "BSGraphics state"sv);
		g_stateAltSlot = RipResolve(Addr::kGraphicsStateAltAnchor, { 0x48, 0x8B, 0x1D },
			Addr::kRipDispOffset, Addr::kRipInstrLen, "BSGraphics state (fallback slot)"sv);
		g_ctxSlot = RipResolve(Addr::kImmediateContextAnchor, { 0x48, 0x8B, 0x05 },
			Addr::kRipDispOffset, Addr::kRipInstrLen, "ID3D11DeviceContext slot"sv);
		g_rtm = RipResolve(Addr::kRenderTargetManagerAnchor, { 0x48, 0x8D, 0x0D },
			Addr::kRipDispOffset, Addr::kRipInstrLen, "BSGraphics::RenderTargetManager"sv);
		g_renderer = RipResolve(Addr::kRendererAnchor, { 0x48, 0x8D, 0x0D },
			Addr::kRipDispOffset, Addr::kRipInstrLen, "BSGraphics::Renderer"sv);
		g_deviceSlot = RipResolve(Addr::kD3D11DeviceAnchor, { 0x48, 0x8B, 0x0D },
			Addr::kRipDispOffset, Addr::kRipInstrLen, "ID3D11Device slot"sv);

		g_resolved = g_stateSlot != 0 && g_stateAltSlot != 0 && g_ctxSlot != 0 &&
		             g_rtm != 0 && g_renderer != 0 && g_deviceSlot != 0;

		if (!g_resolved) {
			// RipResolve has already said which anchor failed and why. This line exists
			// so a reader who greps for the module name finds the consequence too.
			logger::error("engine handles unresolved - every render module stays inert. The anchor that failed is logged above."sv);
			return false;
		}

		logger::info(FMT_STRING("engine: device slot rva {:X}, ctx slot rva {:X}, state rva {:X} (alt {:X}), rtm rva {:X}, renderer rva {:X}"),
			FaultGuard::ToRva(g_deviceSlot),
			FaultGuard::ToRva(g_ctxSlot),
			FaultGuard::ToRva(g_stateSlot),
			FaultGuard::ToRva(g_stateAltSlot),
			FaultGuard::ToRva(g_rtm),
			FaultGuard::ToRva(g_renderer));

		// The device is a plain memory read, so the feature level - measurement M10 -
		// is answerable here rather than waiting for a render-thread callback that does
		// not exist. GetFeatureLevel is not a context method and does not touch context
		// state; it reads a value the device was created with.
		if (auto* const device = Device()) {
			logger::info(FMT_STRING("engine: d3d11 device feature level {:X}"),
				static_cast<unsigned>(device->GetFeatureLevel()));
		} else {
			logger::info("engine: the device slot resolves but is still null - expected before the renderer starts"sv);
		}
		return true;
	}

	bool Resolved() noexcept
	{
		return g_resolved;
	}

	std::string HandleSummary()
	{
		if (!g_resolved) {
			return "unresolved - no render module can do anything";
		}
		return fmt::format(
			FMT_STRING("device slot {:X}, ctx slot {:X}, state {:X} (alt {:X}), rtm {:X}, renderer {:X}"),
			FaultGuard::ToRva(g_deviceSlot),
			FaultGuard::ToRva(g_ctxSlot),
			FaultGuard::ToRva(g_stateSlot),
			FaultGuard::ToRva(g_stateAltSlot),
			FaultGuard::ToRva(g_rtm),
			FaultGuard::ToRva(g_renderer));
	}

	ID3D11DeviceContext* ImmediateContext() noexcept
	{
		void* p = nullptr;
		if (!ReadPtr(g_ctxSlot, p)) {
			return nullptr;
		}
		return static_cast<ID3D11DeviceContext*>(p);
	}

	ID3D11Device* Device() noexcept
	{
		// Read fresh from the engine's own slot, and never AddRef'd: the engine owns the
		// reference and outlives us, so taking one would only be a leak with extra steps.
		void* p = nullptr;
		if (!ReadPtr(g_deviceSlot, p)) {
			if (g_deviceGate.Ready()) {
				logger::warn("engine: the device slot is unreadable"sv);
			}
			return nullptr;
		}
		return static_cast<ID3D11Device*>(p);
	}

	std::uintptr_t GraphicsState() noexcept
	{
		// The engine's own accessors take the first slot and fall back to the second
		// when it is null. Both slots currently hold the same object; the fallback is
		// reproduced anyway, because copying the engine's own branch is free and
		// guessing which slot is authoritative is not.
		void* p = nullptr;
		if (ReadPtr(g_stateSlot, p) && p) {
			return reinterpret_cast<std::uintptr_t>(p);
		}
		if (ReadPtr(g_stateAltSlot, p) && p) {
			return reinterpret_cast<std::uintptr_t>(p);
		}
		return 0;
	}

	// CommonLibF4's render-target manager and renderer-data array lengths are flat
	// Fallout 4 values and are wrong for VR. The base offset and the stride do transfer,
	// so these arrays get walked with hand-derived bounds, never with the library struct.
	std::uintptr_t RenderTargetManager() noexcept
	{
		return g_rtm;
	}

	std::uintptr_t Renderer() noexcept
	{
		return g_renderer;
	}

	bool StereoFlag() noexcept
	{
		if (g_renderer == 0) {
			return false;
		}
		std::uint8_t b = 0;
		if (!FaultGuard::SafeReadBytes(
				reinterpret_cast<const void*>(g_renderer + Addr::kRendererStereoFlag), &b, sizeof(b))) {
			return false;
		}
		return b != 0;
	}

	std::int32_t CurrentRenderTargetIndex() noexcept
	{
		const auto state = GraphicsState();
		std::int32_t idx = -1;
		if (state == 0 || !ReadI32(state + Addr::kStateCurrentRtIndex, idx)) {
			return -1;
		}
		return idx;
	}

	Validation Validate() noexcept
	{
		Validation v;
		if (!g_resolved) {
			v.note = "handles unresolved";
			return v;
		}
		v.ran = true;
		v.stereo = StereoFlag();

		for (std::int32_t logical = 0; logical < Addr::kRenderTargetCount; ++logical) {
			std::int32_t phys = -1;
			if (ReadI32(g_rtm + Addr::kRtmLogicalToPhys + static_cast<std::uintptr_t>(logical) * 4, phys) &&
				phys >= 0 && phys < Addr::kRenderTargetCount) {
				++v.remapValid;
			}

			std::uint32_t width = 0;
			if (ReadU32(g_rtm + Addr::kRtmProps + static_cast<std::uintptr_t>(logical) * Addr::kRtmPropsStride + Addr::kPropWidth, width) &&
				width != 0) {
				++v.propsPopulated;
			}

			void* srv = nullptr;
			if (phys >= 0 && phys < Addr::kRenderTargetCount &&
				ReadPtr(g_renderer + Addr::kRendererRtTable + static_cast<std::uintptr_t>(phys) * Addr::kRendererRtStride + Addr::kRtRecSrv, srv) &&
				srv) {
				++v.recordsWithSrv;
			}
		}

		// Two named targets carry a format that no other buffer in the set would
		// plausibly carry, which makes them the cheapest possible end-to-end assertion:
		// each one exercises the remap, the properties array and the record together.
		const auto mv = RenderTargets::Lookup(Addr::kRT_MotionVectors);
		v.motionVectorsPlausible = mv.valid && mv.srv != nullptr && mv.format == Addr::kFormatR16G16Float;

		const auto depth = RenderTargets::Lookup(Addr::kRT_LinearDepth);
		v.linearDepthPlausible = depth.valid && depth.srv != nullptr && depth.format == Addr::kFormatR32Float;

		if (v.remapValid == 0 || v.propsPopulated == 0) {
			v.note = "the render-target tables read as empty - either the renderer has not started yet, or the table offsets are wrong for this build";
		} else if (!v.motionVectorsPlausible || !v.linearDepthPlausible) {
			v.note = "the tables are populated but the two named targets did not come back with their expected formats - treat any target identity from this build as unproven";
		} else {
			v.note = "remap, properties and resource records agree, and both named targets resolved with the format the engine's own name table implies";
		}
		return v;
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
