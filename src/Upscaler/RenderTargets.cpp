#include "Upscaler/RenderTargets.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"

#include <d3d11.h>

namespace Upscaler::RenderTargets
{
	namespace
	{
		// One-shot: on this build the answer never changes, and Lookup is a per-frame
		// path the moment anything calls it in anger.
		LogGate g_unresolved;

		template <class T>
		[[nodiscard]] T* ReadView(std::uintptr_t a_record, std::uintptr_t a_field) noexcept
		{
			void* p = nullptr;
			if (!Engine::ReadPtr(a_record + a_field, p)) {
				return nullptr;
			}
			return static_cast<T*>(p);
		}
	}

	PhysRT Lookup(std::int32_t a_logical) noexcept
	{
		PhysRT rt;
		rt.logical = a_logical;

		if (!Engine::Resolved()) {
			if (g_unresolved.Ready()) {
				logger::warn("render-target resolution unavailable: the engine handles did not resolve. See the anchor mismatch logged at load."sv);
			}
			return rt;
		}

		if (a_logical < 0 || a_logical >= Addr::kRenderTargetCount) {
			return rt;
		}

		const auto rtm = Engine::RenderTargetManager();
		const auto renderer = Engine::Renderer();
		if (rtm == 0 || renderer == 0) {
			return rt;
		}

		const auto logicalIdx = static_cast<std::uintptr_t>(a_logical);

		// The dimensions and format are indexed by the LOGICAL id, not the physical
		// slot. Mixing the two is the single easiest mistake to make here, and it fails
		// quietly: a wrong row still reads as plausible dimensions.
		static_cast<void>(Engine::ReadU32(rtm + Addr::kRtmProps + logicalIdx * Addr::kRtmPropsStride + Addr::kPropWidth, rt.width));
		static_cast<void>(Engine::ReadU32(rtm + Addr::kRtmProps + logicalIdx * Addr::kRtmPropsStride + Addr::kPropHeight, rt.height));
		static_cast<void>(Engine::ReadU32(rtm + Addr::kRtmProps + logicalIdx * Addr::kRtmPropsStride + Addr::kPropFormat, rt.format));

		std::int32_t phys = -1;
		if (!Engine::ReadI32(rtm + Addr::kRtmLogicalToPhys + logicalIdx * 4, phys)) {
			return rt;
		}
		// -1 is the engine's own "this logical id is unused", and is a normal answer for
		// 21 of the 144 slots, not a failure.
		if (phys < 0 || phys >= Addr::kPhysicalRenderTargetCount) {
			return rt;
		}
		rt.phys = phys;

		const auto record = renderer + Addr::kRendererRtTable +
		                    static_cast<std::uintptr_t>(phys) * Addr::kRendererRtStride;

		rt.tex = ReadView<ID3D11Texture2D>(record, Addr::kRtRecTexture);
		rt.rtv = ReadView<ID3D11RenderTargetView>(record, Addr::kRtRecRtv);
		rt.srv = ReadView<ID3D11ShaderResourceView>(record, Addr::kRtRecSrv);
		rt.uav = ReadView<ID3D11UnorderedAccessView>(record, Addr::kRtRecUav);

		// Valid means the lookup arithmetic completed and landed on a record the engine
		// has populated - not that the target is the one the caller wanted. A record
		// with neither a view nor a texture is an unallocated slot, and saying so is
		// more useful than returning a struct of nulls that looks like a fault.
		rt.valid = rt.rtv != nullptr || rt.srv != nullptr || rt.tex != nullptr;
		return rt;
	}

	PhysDS LookupDepth(std::int32_t a_index) noexcept
	{
		PhysDS ds;
		ds.index = a_index;

		if (!Engine::Resolved() || a_index < 0 || a_index >= Addr::kDepthStencilCount) {
			return ds;
		}

		const auto rtm = Engine::RenderTargetManager();
		const auto renderer = Engine::Renderer();
		if (rtm == 0 || renderer == 0) {
			return ds;
		}

		// Properties are indexed by the LOGICAL id, just like colour properties.
		const auto idx = static_cast<std::uintptr_t>(a_index);
		const auto props = rtm + Addr::kRtmDsProps + idx * Addr::kRtmDsPropsStride;
		static_cast<void>(Engine::ReadU32(props + Addr::kPropWidth, ds.width));
		static_cast<void>(Engine::ReadU32(props + Addr::kPropHeight, ds.height));
		static_cast<void>(Engine::ReadU32(props + Addr::kDsPropArraySize, ds.arraySize));

		std::int32_t phys = -1;
		if (!Engine::ReadI32(rtm + Addr::kRtmDsLogicalToPhys + idx * 4, phys)) {
			return ds;
		}
		// -1 is normal for an unallocated logical id. Physical depth records have
		// one more slot than the logical table; the adjacent cube-map base proves it.
		if (phys < 0 || phys >= Addr::kPhysicalDepthStencilCount) {
			return ds;
		}
		ds.phys = phys;

		const auto record = renderer + Addr::kRendererDsTable +
		                    static_cast<std::uintptr_t>(phys) * Addr::kRendererDsStride;
		ds.tex = ReadView<ID3D11Texture2D>(record, Addr::kDsRecTexture);
		ds.dsv = ReadView<ID3D11DepthStencilView>(record, Addr::kDsRecDsv);
		ds.dsvReadOnlyDepth = ReadView<ID3D11DepthStencilView>(record, Addr::kDsRecDsvReadOnlyDepth);
		ds.dsvReadOnlyStencil = ReadView<ID3D11DepthStencilView>(record, Addr::kDsRecDsvReadOnlyStencil);
		ds.dsvReadOnlyBoth = ReadView<ID3D11DepthStencilView>(record, Addr::kDsRecDsvReadOnlyBoth);
		ds.srv = ReadView<ID3D11ShaderResourceView>(record, Addr::kDsRecSrv);

		ds.valid = ds.dsv != nullptr || ds.dsvReadOnlyDepth != nullptr ||
		           ds.dsvReadOnlyStencil != nullptr || ds.dsvReadOnlyBoth != nullptr ||
		           ds.srv != nullptr || ds.tex != nullptr;
		return ds;
	}
}
