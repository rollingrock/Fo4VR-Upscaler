#include "Upscaler/Debug.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"
#include "Upscaler/RenderTargets.h"

#include <d3d11.h>

#include <atomic>
#include <iterator>

namespace Upscaler::Debug
{
	namespace
	{
		std::atomic<std::uint64_t> g_requestedSequence{ 0 };
		std::atomic<std::uint64_t> g_completedSequence{ 0 };
		std::atomic<ObservationPoint> g_requestedPoint{ ObservationPoint::none };
		SRWLOCK g_observationLock = SRWLOCK_INIT;
		BindingObservation g_lastObservation{};

		[[nodiscard]] ObservedTarget Observe(const RenderTargets::PhysRT& a_rt) noexcept
		{
			return {
				a_rt.logical,
				a_rt.phys,
				a_rt.width,
				a_rt.height,
				a_rt.format,
				a_rt.valid,
			};
		}

		[[nodiscard]] ObservedTarget Observe(const RenderTargets::PhysDS& a_ds) noexcept
		{
			std::uint32_t format = 0;
			if (a_ds.tex) {
				D3D11_TEXTURE2D_DESC desc{};
				a_ds.tex->GetDesc(&desc);
				format = static_cast<std::uint32_t>(desc.Format);
			}
			return {
				a_ds.index,
				a_ds.phys,
				a_ds.width,
				a_ds.height,
				format,
				a_ds.valid,
			};
		}

		[[nodiscard]] DepthViewBinding MatchDepthView(
			ID3D11DepthStencilView* a_view,
			const RenderTargets::PhysDS& a_ds) noexcept
		{
			if (!a_view) {
				return DepthViewBinding::none;
			}
			if (a_view == a_ds.dsv) {
				return DepthViewBinding::writable;
			}
			if (a_view == a_ds.dsvReadOnlyDepth) {
				return DepthViewBinding::readOnlyDepth;
			}
			if (a_view == a_ds.dsvReadOnlyStencil) {
				return DepthViewBinding::readOnlyStencil;
			}
			if (a_view == a_ds.dsvReadOnlyBoth) {
				return DepthViewBinding::readOnlyBoth;
			}
			return DepthViewBinding::unmatched;
		}

		[[nodiscard]] RenderTargets::PhysDS FindDepth(
			ID3D11DepthStencilView* a_view,
			DepthViewBinding& a_binding) noexcept
		{
			for (std::int32_t logical = 0; logical < Addr::kDepthStencilCount; ++logical) {
				const auto ds = RenderTargets::LookupDepth(logical);
				const auto binding = MatchDepthView(a_view, ds);
				if (binding != DepthViewBinding::unmatched && binding != DepthViewBinding::none) {
					a_binding = binding;
					return ds;
				}
			}
			a_binding = a_view ? DepthViewBinding::unmatched : DepthViewBinding::none;
			return {};
		}

		[[nodiscard]] std::int32_t FindSrvSlot(
			ID3D11ShaderResourceView* const* a_views,
			std::size_t a_count,
			ID3D11ShaderResourceView* a_wanted) noexcept
		{
			if (!a_wanted) {
				return -1;
			}
			for (std::size_t i = 0; i < a_count; ++i) {
				if (a_views[i] == a_wanted) {
					return static_cast<std::int32_t>(i);
				}
			}
			return -1;
		}

		void Capture(
			BindingObservation& a_out,
			std::uint64_t a_sequence,
			ObservationPoint a_point,
			std::uintptr_t a_renderer,
			std::uint64_t a_eventCall,
			std::uint32_t a_threadId) noexcept
		{
			a_out = {};
			a_out.requestedSequence = a_sequence;
			a_out.completedSequence = a_sequence;
			a_out.captured = true;
			a_out.point = a_point;
			if (a_point == ObservationPoint::clearColor) {
				a_out.clearCall = a_eventCall;
			} else if (a_point == ObservationPoint::temporalAADraw) {
				a_out.temporalAACall = a_eventCall;
			}
			a_out.threadId = a_threadId;
			a_out.renderer = a_renderer;

			const auto currentColor = RenderTargets::Lookup(Engine::CurrentRenderTargetIndex());
			auto currentDepth = RenderTargets::LookupDepth(Engine::CurrentDepthStencilIndex());
			const auto motionVectors = RenderTargets::Lookup(Addr::kRT_MotionVectors);
			const auto linearDepth = RenderTargets::Lookup(Addr::kRT_LinearDepth);
			a_out.currentColor = Observe(currentColor);
			a_out.currentDepth = Observe(currentDepth);
			a_out.motionVectors = Observe(motionVectors);
			a_out.linearDepth = Observe(linearDepth);

			auto* const context = Engine::ImmediateContext();
			a_out.context = reinterpret_cast<std::uintptr_t>(context);
			if (!context) {
				return;
			}

			ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* depthView = nullptr;
			context->OMGetRenderTargets(
				static_cast<UINT>(std::size(renderTargets)), renderTargets, &depthView);

			for (std::size_t i = 0; i < std::size(renderTargets); ++i) {
				if (!renderTargets[i]) {
					continue;
				}
				a_out.omBindings[i].bound = true;
				++a_out.omRenderTargetCount;
				if (renderTargets[i] == currentColor.rtv && a_out.currentColorOmSlot < 0) {
					a_out.currentColorOmSlot = static_cast<std::int32_t>(i);
				}
			}

			a_out.currentDepthOmView = MatchDepthView(depthView, currentDepth);
			if (depthView && a_out.currentDepthOmView == DepthViewBinding::unmatched) {
				currentDepth = FindDepth(depthView, a_out.currentDepthOmView);
				a_out.currentDepth = Observe(currentDepth);
			}

			ID3D11ShaderResourceView* psViews[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
			context->PSGetShaderResources(0, static_cast<UINT>(std::size(psViews)), psViews);
			for (auto* const view : psViews) {
				if (view) {
					++a_out.psSrvCount;
				}
			}
			for (std::size_t i = 0; i < std::size(psViews); ++i) {
				a_out.psBindings[i].bound = psViews[i] != nullptr;
			}

			// Resolve every bound view back to the engine's logical namespace. This is
			// deliberately one-shot work: a full table walk per frame would be needless,
			// while counts alone leave the source/history/effect-mask question open.
			for (std::int32_t logical = 0; logical < Addr::kRenderTargetCount; ++logical) {
				const auto rt = RenderTargets::Lookup(logical);
				if (!rt.valid) {
					continue;
				}
				for (std::size_t i = 0; i < std::size(renderTargets); ++i) {
					if (renderTargets[i] && renderTargets[i] == rt.rtv) {
						a_out.omBindings[i].colorLogical = logical;
					}
				}
				for (std::size_t i = 0; i < std::size(psViews); ++i) {
					if (psViews[i] && psViews[i] == rt.srv) {
						a_out.psBindings[i].colorLogical = logical;
					}
				}
			}
			for (std::int32_t logical = 0; logical < Addr::kDepthStencilCount; ++logical) {
				const auto ds = RenderTargets::LookupDepth(logical);
				if (!ds.valid || !ds.srv) {
					continue;
				}
				for (std::size_t i = 0; i < std::size(psViews); ++i) {
					if (psViews[i] == ds.srv) {
						a_out.psBindings[i].depthLogical = logical;
					}
				}
			}
			a_out.motionVectorsPsSlot = FindSrvSlot(psViews, std::size(psViews), motionVectors.srv);
			a_out.linearDepthPsSlot = FindSrvSlot(psViews, std::size(psViews), linearDepth.srv);
			a_out.currentDepthPsSlot = FindSrvSlot(psViews, std::size(psViews), currentDepth.srv);

			for (auto*& view : psViews) {
				SafeRelease(view);
			}
			for (auto*& view : renderTargets) {
				SafeRelease(view);
			}
			SafeRelease(depthView);
		}

		void TryCapture(
			ObservationPoint a_point,
			std::uintptr_t a_renderer,
			std::uint64_t a_eventCall,
			std::uint32_t a_threadId) noexcept
		{
			const auto requested = g_requestedSequence.load(std::memory_order_acquire);
			if (requested == 0 || requested == g_completedSequence.load(std::memory_order_acquire) ||
				g_requestedPoint.load(std::memory_order_acquire) != a_point) {
				return;
			}
			if (!::TryAcquireSRWLockExclusive(&g_observationLock)) {
				return;
			}

			const auto current = g_requestedSequence.load(std::memory_order_acquire);
			if (current != g_completedSequence.load(std::memory_order_acquire) &&
				g_requestedPoint.load(std::memory_order_acquire) == a_point) {
				Capture(g_lastObservation, current, a_point, a_renderer, a_eventCall, a_threadId);
				g_completedSequence.store(current, std::memory_order_release);
			}
			::ReleaseSRWLockExclusive(&g_observationLock);
		}
	}

	std::uint64_t RequestBindingObservation(ObservationPoint a_point) noexcept
	{
		g_requestedPoint.store(a_point, std::memory_order_relaxed);
		return g_requestedSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	void OnPrimaryClearColor(
		std::uintptr_t a_renderer,
		std::uint64_t a_clearCall,
		std::uint32_t a_threadId) noexcept
	{
		TryCapture(ObservationPoint::clearColor, a_renderer, a_clearCall, a_threadId);
	}

	void OnTemporalAADraw(
		std::uintptr_t a_renderer,
		std::uint64_t a_temporalAACall,
		std::uint32_t a_threadId) noexcept
	{
		TryCapture(ObservationPoint::temporalAADraw, a_renderer, a_temporalAACall, a_threadId);
	}

	std::string_view ObservationPointName(ObservationPoint a_point) noexcept
	{
		switch (a_point) {
		case ObservationPoint::clearColor:
			return "clear-color"sv;
		case ObservationPoint::temporalAADraw:
			return "temporal-aa-draw"sv;
		default:
			return "none"sv;
		}
	}

	BindingObservation BindingObservationSnapshot() noexcept
	{
		::AcquireSRWLockShared(&g_observationLock);
		auto out = g_lastObservation;
		::ReleaseSRWLockShared(&g_observationLock);

		out.requestedSequence = g_requestedSequence.load(std::memory_order_acquire);
		out.completedSequence = g_completedSequence.load(std::memory_order_acquire);
		out.pending = out.requestedSequence != out.completedSequence;
		out.captured = out.completedSequence != 0;
		return out;
	}

	bool DumpTarget(std::int32_t a_logical, std::string_view a_label)
	{
		logger::warn(FMT_STRING("target dump refused for {} (logical {}): pixel readback is not implemented. The binding observation is read-only and does not substitute for a DDS dump. See docs/MODULE_MAP.md."),
			a_label, a_logical);
		return false;
	}

	void SetVisualise(std::string_view a_mode)
	{
		logger::warn(FMT_STRING("visualisation mode '{}' ignored: there is nothing rendered to visualise on this build."), a_mode);
	}
}
