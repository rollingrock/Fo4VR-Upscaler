#include "Upscaler/Debug.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/RenderTargets.h"

#include <d3d11.h>

#include <array>
#include <atomic>
#include <iterator>
#include <new>

namespace Upscaler::Debug
{
	namespace
	{
		std::atomic<std::uint64_t> g_requestedSequence{ 0 };
		std::atomic<std::uint64_t> g_completedSequence{ 0 };
		std::atomic<ObservationPoint> g_requestedPoint{ ObservationPoint::none };
		SRWLOCK g_observationLock = SRWLOCK_INIT;
		BindingObservation g_lastObservation{};

		// The census. g_censusRequested is written by the listener thread and read by the
		// render thread; g_censusCompleted is the reverse, and its release store is what
		// publishes the buffer g_censusLock guards. g_censusRecording is written only by
		// the render thread at a frame boundary and read only by the render thread on
		// every draw, so relaxed is enough for it - it carries no data of its own.
		std::atomic<std::uint64_t> g_censusRequested{ 0 };
		std::atomic<std::uint64_t> g_censusCompleted{ 0 };
		std::atomic<bool> g_censusRecording{ false };
		SRWLOCK g_censusLock = SRWLOCK_INIT;

		// Both buffers are heap-allocated on the first arm rather than declared as
		// statics: a FrameCensus is 60 KB and two of them are a tenth of this module's
		// whole writable image. The release store on the pointers is paired with the
		// acquire load of g_censusRequested in OnFrameBoundary, which is the only reader
		// that can start using them - a render thread that has not yet seen the request
		// cannot reach them at all.
		std::atomic<FrameCensus*> g_censusRecord{ nullptr };     // render thread only, and only while recording
		std::atomic<FrameCensus*> g_censusPublished{ nullptr };  // guarded by g_censusLock

		// Only the thread that opened a census frame records into it. The ImageSpace draw
		// call this instrument hangs off is not thread-checked by its hook, and reading
		// the immediate context from a second thread while the render thread drives it is
		// the one mistake this file must not make.
		std::atomic<std::uint32_t> g_censusThreadId{ 0 };

		// The UI-entry run. Same publication shape as the census: the listener thread
		// writes g_uiRequested, the render thread writes g_uiCompleted, and the release
		// store on the latter publishes the buffer g_uiLock guards. g_uiRemaining is
		// render-thread-only and carries no data of its own.
		std::atomic<std::uint64_t> g_uiRequested{ 0 };
		std::atomic<std::uint64_t> g_uiCompleted{ 0 };
		std::atomic<std::uint32_t> g_uiRequestedFrames{ 0 };
		// Read once when a run OPENS, into the run's own field, and never again while it
		// records - the same shape as the frame count beside it, and for the same reason:
		// a second arm landing mid-run must not change how many readbacks the run in
		// flight takes. Relaxed on both sides because the value it carries is published by
		// the acquire/release pair on g_uiRequested/g_uiCompleted that already orders the
		// arm against the run.
		std::atomic<std::uint32_t> g_uiRequestedStencilReads{ 0 };
		std::atomic<std::uint64_t> g_uiCalls{ 0 };
		std::atomic<bool> g_uiRecording{ false };
		SRWLOCK g_uiLock = SRWLOCK_INIT;
		std::atomic<UiObservation*> g_uiRecord{ nullptr };     // render thread only, and only while recording
		std::atomic<UiObservation*> g_uiPublished{ nullptr };  // guarded by g_uiLock
		std::atomic<std::uint32_t> g_uiThreadId{ 0 };

		// The water-setup run. Same publication shape again. g_waterThreadId is the thread
		// the run's FIRST sample was taken on rather than a gate: the point of this run is
		// to report which thread the water setup actually runs on, so a later sample from
		// another thread is still recorded, with its own thread id, instead of being
		// dropped the way the census drops one.
		std::atomic<std::uint64_t> g_waterRequested{ 0 };
		std::atomic<std::uint64_t> g_waterCompleted{ 0 };
		std::atomic<std::uint32_t> g_waterRequestedSamples{ 0 };
		std::atomic<std::uint64_t> g_waterSetupCalls{ 0 };
		// idle -> opening -> open -> idle. Three states and not a bool: the opener must
		// finish clearing the buffer before any other thread may record into it.
		constexpr std::uint32_t kWaterRunIdle = 0;
		constexpr std::uint32_t kWaterRunOpening = 1;
		constexpr std::uint32_t kWaterRunOpen = 2;
		std::atomic<std::uint32_t> g_waterRunState{ kWaterRunIdle };
		// The slot claim. The water setup runs on a POOL of worker threads - measured on
		// 2026-09-07, 52,230 of 52,230 calls off the ClearColor thread, the three arms of
		// one session drawing their 32 samples from 3, 15 and 9 distinct thread ids
		// (artifacts/phase5-step9b-fix-waterpair3-20260907-1231 in the research repo; how
		// many of the pool a run happens to catch varies, that it is a pool does not) - so
		// a plain index would be raced by
		// construction: the first run of this instrument published five never-written
		// slots and two carrying the previous run's samples. Every recorder claims its own
		// index with one fetch_add, and the run is published by whichever thread claims
		// the last one.
		std::atomic<std::uint32_t> g_waterNextIndex{ 0 };
		std::atomic<std::uint32_t> g_waterWritten{ 0 };
		SRWLOCK g_waterLock = SRWLOCK_INIT;
		std::atomic<WaterObservation*> g_waterRecord{ nullptr };
		std::atomic<WaterObservation*> g_waterPublished{ nullptr };
		std::atomic<std::uint32_t> g_waterThreadId{ 0 };

		// Resolved once when a census frame opens rather than per draw: a logical id's
		// views do not change inside a frame, and 144 + 17 lookups per draw would price
		// the instrument rather than the passes it is measuring.
		std::array<RenderTargets::PhysRT, Addr::kRenderTargetCount> g_censusColorTable{};
		std::array<RenderTargets::PhysDS, Addr::kDepthStencilCount> g_censusDepthTable{};

		static_assert(kCensusColorTargets == static_cast<std::size_t>(Addr::kRenderTargetCount));
		static_assert(kCensusDepthTargets == static_cast<std::size_t>(Addr::kDepthStencilCount));

		// The resource behind a bound view, so a slot that resolves to no logical id still
		// reports what it samples. Views are read-only here; the AddRef from GetResource is
		// released before returning.
		void DescribeSrv(ID3D11ShaderResourceView* a_view, CensusSrvDesc& a_out) noexcept
		{
			if (!a_view) {
				return;
			}
			ID3D11Resource* resource = nullptr;
			a_view->GetResource(&resource);
			if (!resource) {
				return;
			}
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			resource->GetType(&dimension);
			a_out.dimension = static_cast<std::uint32_t>(dimension);
			// Every dimension the pixel stage can carry, not only 2D: a slot left at 0x0
			// reads as "no extent recorded" and would put an unexplained hole in an extent
			// argument. The colour-grading LUTs bound on this chain are 3D.
			switch (dimension) {
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				{
					ID3D11Texture2D* texture = nullptr;
					if (SUCCEEDED(resource->QueryInterface(
							__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) &&
						texture) {
						D3D11_TEXTURE2D_DESC desc{};
						texture->GetDesc(&desc);
						a_out.width = desc.Width;
						a_out.height = desc.Height;
						a_out.format = static_cast<std::uint32_t>(desc.Format);
						a_out.arraySize = desc.ArraySize;
						a_out.mipLevels = desc.MipLevels;
					}
					SafeRelease(texture);
				}
				break;
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				{
					ID3D11Texture3D* texture = nullptr;
					if (SUCCEEDED(resource->QueryInterface(
							__uuidof(ID3D11Texture3D), reinterpret_cast<void**>(&texture))) &&
						texture) {
						D3D11_TEXTURE3D_DESC desc{};
						texture->GetDesc(&desc);
						a_out.width = desc.Width;
						a_out.height = desc.Height;
						a_out.format = static_cast<std::uint32_t>(desc.Format);
						// A 3D texture has no array; depth is reported here so the slot is
						// fully described by the same five numbers.
						a_out.arraySize = desc.Depth;
						a_out.mipLevels = desc.MipLevels;
					}
					SafeRelease(texture);
				}
				break;
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				{
					ID3D11Texture1D* texture = nullptr;
					if (SUCCEEDED(resource->QueryInterface(
							__uuidof(ID3D11Texture1D), reinterpret_cast<void**>(&texture))) &&
						texture) {
						D3D11_TEXTURE1D_DESC desc{};
						texture->GetDesc(&desc);
						a_out.width = desc.Width;
						a_out.height = 1;
						a_out.format = static_cast<std::uint32_t>(desc.Format);
						a_out.arraySize = desc.ArraySize;
						a_out.mipLevels = desc.MipLevels;
					}
					SafeRelease(texture);
				}
				break;
			case D3D11_RESOURCE_DIMENSION_BUFFER:
				{
					ID3D11Buffer* buffer = nullptr;
					if (SUCCEEDED(resource->QueryInterface(
							__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) &&
						buffer) {
						D3D11_BUFFER_DESC desc{};
						buffer->GetDesc(&desc);
						a_out.width = desc.ByteWidth;  // bytes, not texels
						a_out.height = 1;
						a_out.arraySize = desc.StructureByteStride;
					}
					SafeRelease(buffer);
				}
				break;
			default:
				break;
			}
			SafeRelease(resource);
		}

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
			} else if (a_point == ObservationPoint::hdrFinalDraw) {
				a_out.hdrCall = a_eventCall;
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
			std::uint32_t a_threadId,
			std::int32_t a_expectedOutputLogical = -1) noexcept
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
				BindingObservation candidate{};
				Capture(candidate, current, a_point, a_renderer, a_eventCall, a_threadId);

				bool outputMatches = a_expectedOutputLogical < 0;
				if (!outputMatches) {
					for (const auto& binding : candidate.omBindings) {
						if (binding.bound && binding.colorLogical == a_expectedOutputLogical) {
							outputMatches = true;
							break;
						}
					}
				}
				if (outputMatches) {
					g_lastObservation = candidate;
					g_completedSequence.store(current, std::memory_order_release);
				}
			}
			::ReleaseSRWLockExclusive(&g_observationLock);
		}

		// Null unless a census is recording on this very thread.
		[[nodiscard]] FrameCensus* RecordingBuffer() noexcept
		{
			if (!g_censusRecording.load(std::memory_order_relaxed) ||
				g_censusThreadId.load(std::memory_order_relaxed) != static_cast<std::uint32_t>(::GetCurrentThreadId())) {
				return nullptr;
			}
			return g_censusRecord.load(std::memory_order_relaxed);
		}

		[[nodiscard]] CensusEntry* NextCensusEntry(FrameCensus& a_census, CensusEntryKind a_kind) noexcept
		{
			if (a_census.count >= kCensusEntries) {
				a_census.truncated = true;
				return nullptr;
			}
			auto& entry = a_census.entries[a_census.count];
			entry = {};
			entry.kind = a_kind;
			entry.order = a_census.count;
			++a_census.count;
			return &entry;
		}

		void CaptureCensusViewport(CensusEntry& a_out, ID3D11DeviceContext* a_context) noexcept
		{
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			auto count = static_cast<UINT>(std::size(viewports));
			a_context->RSGetViewports(&count, viewports);
			a_out.viewportCount = count;
			if (count > 0) {
				a_out.viewportX = viewports[0].TopLeftX;
				a_out.viewportY = viewports[0].TopLeftY;
				a_out.viewportWidth = viewports[0].Width;
				a_out.viewportHeight = viewports[0].Height;
			}
			a_out.liveRatiosValid = DynamicResolution::CurrentRatios(
				a_out.liveWidthRatio, a_out.liveHeightRatio);
		}

		// The output-merger half of a census entry: the bound colour targets and the
		// depth-stencil view, resolved against this census frame's own cached table. The
		// draw capture below does the same work plus the pixel-stage scan; a probe wants
		// only this half, and doing 128 PSGetShaderResources for it would cost more than
		// the thing being measured.
		void CaptureCensusOutputMerger(CensusEntry& a_out) noexcept
		{
			auto* const context = Engine::ImmediateContext();
			if (!context) {
				return;
			}
			CaptureCensusViewport(a_out, context);

			const auto currentColorIndex = Engine::CurrentRenderTargetIndex();
			if (currentColorIndex >= 0 && currentColorIndex < Addr::kRenderTargetCount) {
				const auto& current = g_censusColorTable[static_cast<std::size_t>(currentColorIndex)];
				a_out.currentColorLogical = current.valid ? currentColorIndex : -1;
				a_out.currentColorWidth = current.width;
				a_out.currentColorHeight = current.height;
			}

			ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* depthView = nullptr;
			context->OMGetRenderTargets(
				static_cast<UINT>(std::size(renderTargets)), renderTargets, &depthView);

			for (std::size_t i = 0; i < std::size(renderTargets); ++i) {
				a_out.omColorLogical[i] = -1;
				if (renderTargets[i]) {
					++a_out.omBoundCount;
				}
			}
			for (std::int32_t logical = 0; logical < Addr::kRenderTargetCount; ++logical) {
				const auto& rt = g_censusColorTable[static_cast<std::size_t>(logical)];
				if (!rt.valid || !rt.rtv) {
					continue;
				}
				for (std::size_t i = 0; i < std::size(renderTargets); ++i) {
					if (renderTargets[i] == rt.rtv) {
						a_out.omColorLogical[i] = logical;
					}
				}
			}
			for (std::int32_t logical = 0; logical < Addr::kDepthStencilCount; ++logical) {
				const auto& ds = g_censusDepthTable[static_cast<std::size_t>(logical)];
				if (!ds.valid || !depthView || a_out.depthLogical >= 0) {
					continue;
				}
				const auto binding = MatchDepthView(depthView, ds);
				if (binding != DepthViewBinding::unmatched && binding != DepthViewBinding::none) {
					a_out.depthLogical = logical;
					a_out.depthView = binding;
				}
			}
			if (depthView && a_out.depthLogical < 0) {
				a_out.depthView = DepthViewBinding::unmatched;
			}

			for (auto*& view : renderTargets) {
				SafeRelease(view);
			}
			SafeRelease(depthView);
		}

		void CaptureCensusDraw(CensusEntry& a_out) noexcept
		{
			auto* const context = Engine::ImmediateContext();
			if (!context) {
				return;
			}

			const auto currentColorIndex = Engine::CurrentRenderTargetIndex();
			if (currentColorIndex >= 0 && currentColorIndex < Addr::kRenderTargetCount) {
				const auto& current = g_censusColorTable[static_cast<std::size_t>(currentColorIndex)];
				a_out.currentColorLogical = current.valid ? currentColorIndex : -1;
				a_out.currentColorWidth = current.width;
				a_out.currentColorHeight = current.height;
			}

			CaptureCensusViewport(a_out, context);

			ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* depthView = nullptr;
			context->OMGetRenderTargets(
				static_cast<UINT>(std::size(renderTargets)), renderTargets, &depthView);

			ID3D11ShaderResourceView* psViews[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
			context->PSGetShaderResources(0, static_cast<UINT>(std::size(psViews)), psViews);

			for (std::size_t i = 0; i < std::size(renderTargets); ++i) {
				a_out.omColorLogical[i] = -1;
				if (renderTargets[i]) {
					++a_out.omBoundCount;
				}
			}
			for (std::size_t i = 0; i < kCensusSrvSlots; ++i) {
				a_out.srvColorLogical[i] = -1;
				a_out.srvDepthLogical[i] = -1;
				a_out.srvBound[i] = psViews[i] != nullptr;
				a_out.srvDesc[i] = {};
				DescribeSrv(psViews[i], a_out.srvDesc[i]);
			}
			for (std::size_t i = 0; i < std::size(psViews); ++i) {
				if (!psViews[i]) {
					continue;
				}
				++a_out.srvBoundCount;
				if (i >= kCensusSrvSlots) {
					++a_out.srvBoundAboveScan;
				}
			}

			for (std::int32_t logical = 0; logical < Addr::kRenderTargetCount; ++logical) {
				const auto& rt = g_censusColorTable[static_cast<std::size_t>(logical)];
				if (!rt.valid) {
					continue;
				}
				for (std::size_t i = 0; i < std::size(renderTargets); ++i) {
					if (rt.rtv && renderTargets[i] == rt.rtv) {
						a_out.omColorLogical[i] = logical;
					}
				}
				for (std::size_t i = 0; i < kCensusSrvSlots; ++i) {
					if (rt.srv && psViews[i] == rt.srv) {
						a_out.srvColorLogical[i] = logical;
					}
				}
			}
			for (std::int32_t logical = 0; logical < Addr::kDepthStencilCount; ++logical) {
				const auto& ds = g_censusDepthTable[static_cast<std::size_t>(logical)];
				if (!ds.valid) {
					continue;
				}
				if (ds.srv) {
					for (std::size_t i = 0; i < kCensusSrvSlots; ++i) {
						if (psViews[i] == ds.srv) {
							a_out.srvDepthLogical[i] = logical;
						}
					}
				}
				if (depthView && a_out.depthLogical < 0) {
					const auto binding = MatchDepthView(depthView, ds);
					if (binding != DepthViewBinding::unmatched && binding != DepthViewBinding::none) {
						a_out.depthLogical = logical;
						a_out.depthView = binding;
					}
				}
			}
			if (depthView && a_out.depthLogical < 0) {
				a_out.depthView = DepthViewBinding::unmatched;
			}

			for (auto*& view : psViews) {
				SafeRelease(view);
			}
			for (auto*& view : renderTargets) {
				SafeRelease(view);
			}
			SafeRelease(depthView);
		}

		// One guarded read of a pointer-sized field of an engine structure. Every field the
		// water capture below reaches is at an offset decoded from Fallout4VR.exe, so a
		// wrong offset on some future build must leave the field unread, never fault.
		[[nodiscard]] bool ReadPointerField(std::uintptr_t a_base, std::uintptr_t a_offset, std::uintptr_t& a_out) noexcept
		{
			if (a_base == 0) {
				return false;
			}
			std::uintptr_t value = 0;
			if (!FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(a_base + a_offset), &value, sizeof(value))) {
				return false;
			}
			a_out = value;
			return true;
		}

		// The water pixel constants, read back out of the mapped constant array after the
		// engine's own setup wrote them. a_shaderState carries the pixel-shader record and
		// the constant groups; a_viewState carries the render-target id the engine built
		// the screen half of the constant from. Slot 4 has neither, so both are zero there
		// and only the source cross-check is recorded.
		//
		// a_textureProj, when given, receives the 32 floats of the two per-eye TextureProj
		// matrices, and textureProjRead says whether it was filled. Plain memory reads
		// throughout - no D3D11 call, nothing AddRef'd - so this half of a sample is safe
		// on whichever thread the engine runs the water setup on.
		void ReadWaterConstants(
			CensusWaterConstants& a_out,
			std::uintptr_t a_shaderState,
			std::uintptr_t a_viewState,
			float* a_textureProj,
			float* a_groups,
			std::uint32_t* a_groupFloats) noexcept
		{
			const auto dynamicResolution = DynamicResolution::Snapshot();
			a_out.ratiosValid = dynamicResolution.currentValuesValid;
			a_out.liveWidthRatio = dynamicResolution.currentWidth;
			a_out.liveHeightRatio = dynamicResolution.currentHeight;

			if (a_viewState != 0) {
				std::int32_t targetId = -1;
				if (FaultGuard::SafeReadBytes(
						reinterpret_cast<const void*>(a_viewState + Addr::kWaterViewStateTargetIndex),
						&targetId,
						sizeof(targetId)) &&
					targetId >= 0 && targetId < Addr::kRenderTargetCount) {
					const auto rt = RenderTargets::Lookup(targetId);
					a_out.sourceTargetLogical = rt.valid ? targetId : -1;
					a_out.sourceTargetWidth = rt.width;
					a_out.sourceTargetHeight = rt.height;
				}
			}

			if (a_shaderState == 0) {
				return;
			}

			std::uintptr_t record = 0;
			std::uintptr_t screenGroup = 0;
			std::uintptr_t geometryGroup = 0;
			a_out.stateRead =
				ReadPointerField(a_shaderState, Addr::kWaterShaderStatePixelRecord, record) &&
				ReadPointerField(a_shaderState, Addr::kWaterShaderStateScreenConstantGroup, screenGroup) &&
				ReadPointerField(a_shaderState, Addr::kWaterShaderStateGeometryConstantGroup, geometryGroup);
			if (!a_out.stateRead || record == 0) {
				return;
			}

			std::uint8_t registers[2]{ Addr::kWaterPsRegAbsent, Addr::kWaterPsRegAbsent };
			if (!FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(record + Addr::kWaterPsRegTextureProj),
					&registers[0],
					sizeof(registers[0])) ||
				!FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(record + Addr::kWaterPsRegScreenDynamicResolution),
					&registers[1],
					sizeof(registers[1]))) {
				return;
			}
			a_out.recordRead = true;
			a_out.textureProjRegister = registers[0];
			a_out.screenRegister = registers[1];
			a_out.textureProjPresent = registers[0] != Addr::kWaterPsRegAbsent;
			a_out.screenPresent = registers[1] != Addr::kWaterPsRegAbsent;

			// The register byte is a byte offset into the mapped float array, which is how
			// the engine itself indexes it: it stores four times the float4 register.
			if (a_out.screenPresent) {
				std::uintptr_t mapped = 0;
				float values[4]{};
				if (ReadPointerField(screenGroup, Addr::kWaterConstantGroupMapped, mapped) &&
					mapped != 0 &&
					FaultGuard::SafeReadBytes(
						reinterpret_cast<const void*>(mapped + registers[1] * 4u),
						values,
						sizeof(values))) {
					a_out.screenRead = true;
					a_out.invTargetWidth = values[0];
					a_out.invTargetHeight = values[1];
					a_out.invLiveWidthRatio = values[2];
					a_out.invLiveHeightRatio = values[3];
				}
			}

			if (a_out.textureProjPresent && a_textureProj) {
				std::uintptr_t mapped = 0;
				if (ReadPointerField(geometryGroup, Addr::kWaterConstantGroupMapped, mapped) &&
					mapped != 0 &&
					FaultGuard::SafeReadBytes(
						reinterpret_cast<const void*>(mapped + registers[0] * 4u),
						a_textureProj,
						sizeof(float) * 32)) {
					a_out.textureProjRead = true;
				}
			}

			if (!a_groups || !a_groupFloats) {
				return;
			}
			// The whole addressable range of all three groups, four floats at a time: the
			// mapped array's length is not published anywhere this code can read, so the
			// dump has to find its own end rather than assume one, and a chunk that faults
			// is where this group stops.
			const std::uintptr_t groupObjects[kWaterGroups]{
				a_shaderState + Addr::kWaterShaderStateScreenConstantGroup,
				a_shaderState + Addr::kWaterShaderStateMaterialConstantGroup,
				a_shaderState + Addr::kWaterShaderStateGeometryConstantGroup,
			};
			for (std::size_t group = 0; group < kWaterGroups; ++group) {
				std::uintptr_t object = 0;
				std::uintptr_t mapped = 0;
				if (!FaultGuard::SafeReadBytes(
						reinterpret_cast<const void*>(groupObjects[group]), &object, sizeof(object)) ||
					object == 0 ||
					!ReadPointerField(object, Addr::kWaterConstantGroupMapped, mapped) ||
					mapped == 0) {
					continue;
				}
				float* const destination = a_groups + group * kWaterGroupFloats;
				std::uint32_t read = 0;
				for (std::size_t chunk = 0; chunk < kWaterGroupFloats; chunk += 4) {
					if (!FaultGuard::SafeReadBytes(
							reinterpret_cast<const void*>(mapped + chunk * 4u),
							destination + chunk,
							sizeof(float) * 4)) {
						break;
					}
					read += 4;
				}
				a_groupFloats[group] = read;
			}
		}

		// The census half: the same constants, plus the TextureProj matrices parked in the
		// census's own bounded sample array.
		void CaptureWaterConstants(
			CensusEntry& a_out,
			FrameCensus& a_census,
			std::uintptr_t a_shaderState,
			std::uintptr_t a_viewState) noexcept
		{
			float matrices[32]{};
			ReadWaterConstants(a_out.water, a_shaderState, a_viewState, matrices, nullptr, nullptr);
			if (!a_out.water.textureProjRead) {
				return;
			}
			for (std::size_t i = 0; i < kCensusWaterSamples; ++i) {
				if (a_census.waterSamples[i].valid) {
					continue;
				}
				auto& sample = a_census.waterSamples[i];
				sample.valid = true;
				sample.entryOrder = a_out.order;
				for (std::size_t f = 0; f < 16; ++f) {
					sample.textureProjLeft[f] = matrices[f];
					sample.textureProjRight[f] = matrices[f + 16];
				}
				a_out.waterSample = static_cast<std::int32_t>(i);
				return;
			}
		}

		void OpenCensusFrame(FrameCensus& a_census, std::uint64_t a_sequence) noexcept
		{
			for (std::int32_t logical = 0; logical < Addr::kRenderTargetCount; ++logical) {
				const auto index = static_cast<std::size_t>(logical);
				const auto rt = RenderTargets::Lookup(logical);
				g_censusColorTable[index] = rt;
				// Recorded so a reader can tell "nothing bound this id" from "nothing could
				// have named this id": resolution is pointer identity against rt.srv, so an
				// id with no canonical SRV can never resolve however often it is bound.
				a_census.colorTargets[index] = {
					rt.valid,
					rt.rtv != nullptr,
					rt.srv != nullptr,
					rt.width,
					rt.height,
					rt.format,
				};
			}
			for (std::int32_t logical = 0; logical < Addr::kDepthStencilCount; ++logical) {
				const auto index = static_cast<std::size_t>(logical);
				const auto ds = RenderTargets::LookupDepth(logical);
				g_censusDepthTable[index] = ds;
				a_census.depthTargets[index] = {
					ds.valid,
					ds.dsv != nullptr,
					ds.srv != nullptr,
					ds.width,
					ds.height,
					0u,
				};
			}
			a_census.requestedSequence = a_sequence;
			a_census.completedSequence = 0;
			a_census.pending = true;
			a_census.recording = true;
			a_census.truncated = false;
			a_census.dispatches = 0;
			a_census.draws = 0;
			a_census.sslrDraws = 0;
			a_census.waterGeometrySetups = 0;
			a_census.waterTechniqueSetups = 0;
			a_census.count = 0;
			for (auto& sample : a_census.waterSamples) {
				sample = {};
			}
			a_census.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			g_censusThreadId.store(a_census.threadId, std::memory_order_relaxed);
			g_censusRecording.store(true, std::memory_order_relaxed);
		}

		void CloseCensusFrame(FrameCensus& a_census) noexcept
		{
			g_censusRecording.store(false, std::memory_order_relaxed);
			a_census.recording = false;
			a_census.pending = false;
			a_census.completedSequence = a_census.requestedSequence;

			auto* const published = g_censusPublished.load(std::memory_order_relaxed);
			if (published) {
				::AcquireSRWLockExclusive(&g_censusLock);
				// The header, then only the entries this frame filled. Assigning the whole
				// structure would copy 60 KB of stale tail on every census.
				published->requestedSequence = a_census.requestedSequence;
				published->completedSequence = a_census.completedSequence;
				published->pending = false;
				published->recording = false;
				published->truncated = a_census.truncated;
				published->dispatches = a_census.dispatches;
				published->draws = a_census.draws;
				published->sslrDraws = a_census.sslrDraws;
				published->waterGeometrySetups = a_census.waterGeometrySetups;
				published->waterTechniqueSetups = a_census.waterTechniqueSetups;
				published->count = a_census.count;
				published->waterSamples = a_census.waterSamples;
				published->threadId = a_census.threadId;
				published->colorTargets = a_census.colorTargets;
				published->depthTargets = a_census.depthTargets;
				const auto used = a_census.count < kCensusEntries ? a_census.count : kCensusEntries;
				for (std::uint32_t i = 0; i < used; ++i) {
					published->entries[i] = a_census.entries[i];
				}
				::ReleaseSRWLockExclusive(&g_censusLock);
			}
			g_censusCompleted.store(a_census.requestedSequence, std::memory_order_release);
		}

		// The depth-stencil the UI stencil readback reads. DS1 and not "whichever view is
		// bound": on the reduced path nothing is bound at this seam, so a reader that
		// followed the binding would compare a target against no target at all. DS1 is
		// the one the engine's own late chain and its UI selection both name.
		constexpr std::int32_t kUiStencilDepthLogical = 1;

		// One bounded readback of DS1's stencil plane at the UI entry, reduced in place to
		// a per-eye bounding box of the non-zero region and a value histogram.
		//
		// D3D11 mechanics, both from Microsoft's documentation:
		// ID3D11DeviceContext::CopySubresourceRegion does not support depth-stencil
		// resources, so the whole resource moves through CopyResource - which does - and
		// the sub-rect reduction happens here, on the CPU, rather than in the copy. And a
		// D24_UNORM_S8_UINT texel, which is what an R24G8_TYPELESS depth-stencil holds on
		// this build, keeps the stencil in its most significant byte (DXGI_FORMAT), so the
		// stencil plane is byte 3 of every four.
		//
		// Cost: one 30 MB copy, one map that waits on the GPU, and one full scan, on the
		// render thread, between the late ImageSpace range and the UI draw. This is a
		// diagnostic and nothing but a run that explicitly asks for stencil reads ever
		// calls it; the run's own frame times are not a timing measurement while it does.
		// One texel into a bounding box. Shared by the stencil and the depth halves of
		// the readback so the two boxes are built by identical code and a difference
		// between them is a difference in the data.
		void AccumulateEyeBox(UiStencilRead::EyeBox& a_box, std::uint32_t a_x, std::uint32_t a_y) noexcept
		{
			if (!a_box.any) {
				a_box.any = true;
				a_box.minX = a_x;
				a_box.maxX = a_x;
				a_box.minY = a_y;
				a_box.maxY = a_y;
			} else {
				a_box.minX = a_x < a_box.minX ? a_x : a_box.minX;
				a_box.maxX = a_x > a_box.maxX ? a_x : a_box.maxX;
				a_box.minY = a_y < a_box.minY ? a_y : a_box.minY;
				a_box.maxY = a_y > a_box.maxY ? a_y : a_box.maxY;
			}
			++a_box.count;
		}

		void CaptureUiStencil(UiStencilRead& a_out) noexcept
		{
			a_out = {};
			a_out.attempted = true;
			a_out.depthLogical = kUiStencilDepthLogical;

			const auto ds = RenderTargets::LookupDepth(kUiStencilDepthLogical);
			auto* const device = Engine::Device();
			auto* const context = Engine::ImmediateContext();
			if (!ds.valid || !ds.tex || !device || !context) {
				return;
			}

			D3D11_TEXTURE2D_DESC desc{};
			ds.tex->GetDesc(&desc);
			a_out.width = desc.Width;
			a_out.height = desc.Height;
			a_out.format = static_cast<std::uint32_t>(desc.Format);
			// The stencil plane exists only in the 24/8 layout this build allocates DS1
			// in. Refusing rather than assuming is the point: a build whose depth-stencil
			// moved to another format would otherwise have its depth bits reported as a
			// stencil mask, and the whole measurement would read as a plausible answer.
			if (desc.Format != DXGI_FORMAT_R24G8_TYPELESS || desc.Width < 2 ||
				desc.Height == 0 || desc.MipLevels != 1 || desc.ArraySize != 1) {
				return;
			}
			a_out.resolved = true;

			auto stagingDesc = desc;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.BindFlags = 0;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			stagingDesc.MiscFlags = 0;
			ID3D11Texture2D* staging = nullptr;
			auto hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
			if (FAILED(hr) || !staging) {
				a_out.hr = static_cast<std::int32_t>(hr);
				SafeRelease(staging);
				return;
			}

			context->CopyResource(staging, ds.tex);
			a_out.copied = true;

			D3D11_MAPPED_SUBRESOURCE mapped{};
			hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
			if (FAILED(hr) || !mapped.pData) {
				a_out.hr = static_cast<std::int32_t>(hr);
				SafeRelease(staging);
				return;
			}
			a_out.mapped = true;

			// This session's packed rectangle, so the depth half below can separate the
			// region the reduced raster wrote from the region only a resolve can have
			// written. Read once, outside the scan: it cannot change inside one map.
			const auto dynamicResolution = DynamicResolution::Snapshot();
			a_out.packedKnown = dynamicResolution.snapped &&
			                    dynamicResolution.snappedRenderWidth != 0 &&
			                    dynamicResolution.snappedRenderHeight != 0;
			a_out.packedWidth = a_out.packedKnown ? dynamicResolution.snappedRenderWidth * 2u : 0u;
			a_out.packedHeight = a_out.packedKnown ? dynamicResolution.snappedRenderHeight : 0u;

			// A full 256-bucket count first, then the top eight. Matching each texel
			// against a short list instead would be eight compares per texel over seven
			// million of them; two kilobytes of stack is the cheaper half of that trade.
			std::array<std::uint64_t, 256> histogram{};
			const auto midX = desc.Width / 2u;
			const auto* const base = static_cast<const std::uint8_t*>(mapped.pData);
			for (std::uint32_t y = 0; y < desc.Height; ++y) {
				const auto* const row = base + static_cast<std::size_t>(y) * mapped.RowPitch;
				const bool rowInsidePacked = a_out.packedKnown && y < a_out.packedHeight;
				for (std::uint32_t x = 0; x < desc.Width; ++x) {
					const auto* const texel = row + static_cast<std::size_t>(x) * 4u;
					const std::uint8_t stencil = texel[3];
					++histogram[stencil];

					// The depth plane, from the same three bytes. 0xFFFFFF is the far
					// value the engine clears to, so anything else is a texel the raster
					// (or the resolve) wrote.
					const std::uint32_t depth = static_cast<std::uint32_t>(texel[0]) |
					                            (static_cast<std::uint32_t>(texel[1]) << 8) |
					                            (static_cast<std::uint32_t>(texel[2]) << 16);
					const bool insidePacked = rowInsidePacked && x < a_out.packedWidth;
					if (a_out.packedKnown && !insidePacked) {
						++a_out.outsidePacked;
					}
					if (depth == 0x00FFFFFFu) {
						++a_out.depthFar;
					} else {
						++a_out.depthWritten;
						if (a_out.packedKnown && !insidePacked) {
							++a_out.depthWrittenOutsidePacked;
						}
						AccumulateEyeBox(a_out.depthEye[x < midX ? 0u : 1u], x, y);
					}

					if (stencil == 0) {
						continue;
					}
					AccumulateEyeBox(a_out.eye[x < midX ? 0u : 1u], x, y);
				}
			}
			context->Unmap(staging, 0);
			SafeRelease(staging);

			a_out.scanned = static_cast<std::uint64_t>(desc.Width) * desc.Height;
			a_out.nonZero = a_out.scanned - histogram[0];

			// The eight most common values, zero included: "almost everything is zero and
			// one other value covers the mask" is the expected shape and is worth being
			// able to state, not just imply. Selection sort over 256 buckets, eight times.
			std::array<bool, 256> taken{};
			std::uint64_t reported = 0;
			for (std::size_t slot = 0; slot < kUiStencilValues; ++slot) {
				std::size_t best = 256;
				std::uint64_t bestCount = 0;
				for (std::size_t v = 0; v < histogram.size(); ++v) {
					if (taken[v] || histogram[v] == 0 || histogram[v] <= bestCount) {
						continue;
					}
					best = v;
					bestCount = histogram[v];
				}
				if (best == 256) {
					break;
				}
				taken[best] = true;
				a_out.value[slot] = static_cast<std::uint8_t>(best);
				a_out.valueCount[slot] = bestCount;
				reported += bestCount;
				++a_out.distinctValues;
			}
			a_out.valuesOverflow = a_out.scanned - reported;
		}

		// The depth-stencil state object bound when the UI pass is entered. A null object
		// is the D3D11 default state, which is a state and not a failure, so it is
		// reported with Microsoft's documented D3D11_DEPTH_STENCIL_DESC defaults rather
		// than with zeros a reader would have to know to distrust.
		void CaptureUiDepthStencilState(
			UiDepthStencilState& a_out,
			ID3D11DeviceContext* a_context) noexcept
		{
			a_out = {};
			if (!a_context) {
				return;
			}
			ID3D11DepthStencilState* state = nullptr;
			UINT stencilRef = 0;
			a_context->OMGetDepthStencilState(&state, &stencilRef);
			a_out.read = true;
			a_out.stencilRef = stencilRef;

			D3D11_DEPTH_STENCIL_DESC desc{};
			if (state) {
				state->GetDesc(&desc);
				SafeRelease(state);
			} else {
				a_out.defaulted = true;
				desc.DepthEnable = TRUE;
				desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
				desc.DepthFunc = D3D11_COMPARISON_LESS;
				desc.StencilEnable = FALSE;
				desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
				desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
				const D3D11_DEPTH_STENCILOP_DESC face{
					D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
					D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS
				};
				desc.FrontFace = face;
				desc.BackFace = face;
			}

			a_out.depthEnable = desc.DepthEnable != FALSE ? 1u : 0u;
			a_out.depthWriteMask = static_cast<std::uint8_t>(desc.DepthWriteMask);
			a_out.depthFunc = static_cast<std::uint8_t>(desc.DepthFunc);
			a_out.stencilEnable = desc.StencilEnable != FALSE ? 1u : 0u;
			a_out.stencilReadMask = desc.StencilReadMask;
			a_out.stencilWriteMask = desc.StencilWriteMask;
			a_out.frontFailOp = static_cast<std::uint8_t>(desc.FrontFace.StencilFailOp);
			a_out.frontDepthFailOp = static_cast<std::uint8_t>(desc.FrontFace.StencilDepthFailOp);
			a_out.frontPassOp = static_cast<std::uint8_t>(desc.FrontFace.StencilPassOp);
			a_out.frontFunc = static_cast<std::uint8_t>(desc.FrontFace.StencilFunc);
			a_out.backFailOp = static_cast<std::uint8_t>(desc.BackFace.StencilFailOp);
			a_out.backDepthFailOp = static_cast<std::uint8_t>(desc.BackFace.StencilDepthFailOp);
			a_out.backPassOp = static_cast<std::uint8_t>(desc.BackFace.StencilPassOp);
			a_out.backFunc = static_cast<std::uint8_t>(desc.BackFace.StencilFunc);
		}

		// One sample at the UI/menu render entry: the engine state the pass is about to
		// draw under, plus the bindings in effect when it is entered.
		//
		// This resolves against a fresh RenderTargets::Lookup rather than the census's
		// cached tables on purpose. Those tables are a snapshot taken when a census frame
		// opens and are only valid for that one frame; a run spans 64 frames and a mode
		// switch, a ratio change or a cell load inside it would leave them naming views
		// the manager has already replaced. A lookup is three reads and this runs once a
		// frame, so the cache buys nothing here and would buy a wrong answer.
		void CaptureUiSample(
			UiSample& a_out,
			std::int32_t a_requestedTargetId,
			bool a_readStencil) noexcept
		{
			a_out = {};
			a_out.uiCall = g_uiCalls.load(std::memory_order_relaxed);
			a_out.requestedTargetId = a_requestedTargetId;
			for (auto& logical : a_out.omColorLogical) {
				logical = -1;
			}

			const auto jitter = Jitter::ReadRawState();
			a_out.jitterResolved = jitter.resolved;
			a_out.jitterRead = jitter.read;
			a_out.jitterTorn = jitter.torn;
			a_out.jitterPhase = jitter.phase;
			a_out.projectionXBits = jitter.projectionXBits;
			a_out.projectionYBits = jitter.projectionYBits;
			a_out.jitterPerEyeWidth = jitter.perEyeWidth;
			a_out.jitterPerEyeHeight = jitter.perEyeHeight;
			a_out.jitterEnabled = jitter.enabled;
			a_out.jitterDisableCount = jitter.disableCount;

			// Snapshot() is what /state already uses to prove what the engine holds, and
			// it re-reads the four floats, the selector and the hidden-area byte through
			// the same guarded path. Reusing it keeps one reader for those six values.
			const auto dynamicResolution = DynamicResolution::Snapshot();
			a_out.ratiosValid = dynamicResolution.currentValuesValid;
			a_out.selectorValid = dynamicResolution.selectorValueValid;
			a_out.hiddenAreaValid = dynamicResolution.hiddenAreaMeshValueValid;
			a_out.worldFrameActive = dynamicResolution.worldFrameActive;
			a_out.liveWidthRatio = dynamicResolution.currentWidth;
			a_out.liveHeightRatio = dynamicResolution.currentHeight;
			a_out.targetWidthRatio = dynamicResolution.currentTargetWidth;
			a_out.targetHeightRatio = dynamicResolution.currentTargetHeight;
			a_out.imageSelector = dynamicResolution.currentImageSelector;
			a_out.hiddenAreaMesh = dynamicResolution.currentHiddenAreaMesh;

			// The engine's own cached depth-stencil selection, read at the same instant as
			// the byte above and just after the engine's hidden-area-guarded selection has
			// run - so this is what that selection left behind, not what preceded it.
			const auto engineDs = Engine::CachedDepthStencilSelection();
			a_out.engineDsRead = engineDs.read;
			a_out.engineDsDirtyFlags = engineDs.dirtyFlags;
			a_out.engineDsResolvedId = engineDs.resolvedId;
			a_out.engineDsSlice = engineDs.slice;
			a_out.engineDsMode = engineDs.mode;
			a_out.engineDsIndex = engineDs.index;

			const auto currentColorIndex = Engine::CurrentRenderTargetIndex();
			const auto currentColor = RenderTargets::Lookup(currentColorIndex);
			if (currentColor.valid) {
				a_out.currentColorLogical = currentColorIndex;
				a_out.currentColorWidth = currentColor.width;
				a_out.currentColorHeight = currentColor.height;
			}

			auto* const context = Engine::ImmediateContext();
			if (!context) {
				return;
			}

			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			auto viewportCount = static_cast<UINT>(std::size(viewports));
			context->RSGetViewports(&viewportCount, viewports);
			a_out.viewportCount = viewportCount;
			if (viewportCount > 0) {
				a_out.viewportX = viewports[0].TopLeftX;
				a_out.viewportY = viewports[0].TopLeftY;
				a_out.viewportWidth = viewports[0].Width;
				a_out.viewportHeight = viewports[0].Height;
			}

			ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* depthView = nullptr;
			context->OMGetRenderTargets(
				static_cast<UINT>(std::size(renderTargets)), renderTargets, &depthView);

			for (auto* const view : renderTargets) {
				if (view) {
					++a_out.omBoundCount;
				}
			}
			for (std::int32_t logical = 0; logical < Addr::kRenderTargetCount; ++logical) {
				const auto rt = RenderTargets::Lookup(logical);
				if (!rt.valid || !rt.rtv) {
					continue;
				}
				for (std::size_t i = 0; i < std::size(renderTargets); ++i) {
					if (renderTargets[i] == rt.rtv) {
						a_out.omColorLogical[i] = logical;
					}
				}
			}
			for (std::int32_t logical = 0; logical < Addr::kDepthStencilCount; ++logical) {
				const auto ds = RenderTargets::LookupDepth(logical);
				if (!ds.valid || !depthView || a_out.depthLogical >= 0) {
					continue;
				}
				const auto binding = MatchDepthView(depthView, ds);
				if (binding != DepthViewBinding::unmatched && binding != DepthViewBinding::none) {
					a_out.depthLogical = logical;
					a_out.depthView = binding;
					a_out.depthWidth = ds.width;
					a_out.depthHeight = ds.height;
				}
			}
			if (depthView && a_out.depthLogical < 0) {
				a_out.depthView = DepthViewBinding::unmatched;
			}

			for (auto*& view : renderTargets) {
				SafeRelease(view);
			}
			SafeRelease(depthView);

			CaptureUiDepthStencilState(a_out.depthStencilState, context);

			// Last, and only when the run asked for it. Everything above is a handful of
			// reads; this one waits on the GPU.
			if (a_readStencil) {
				CaptureUiStencil(a_out.stencil);
			}
		}

		// The D3D11 half of a water sample. Only ever called with a_sample.onRenderThread
		// true - the immediate context must not be read from a thread that does not drive
		// it, which is the same rule the census and the UI run hold.
		void CaptureWaterBindings(WaterSample& a_sample) noexcept
		{
			auto* const context = Engine::ImmediateContext();
			if (!context) {
				return;
			}
			a_sample.bindingsRead = true;
			for (auto& logical : a_sample.omColorLogical) {
				logical = -1;
			}
			for (std::size_t i = 0; i < kCensusSrvSlots; ++i) {
				a_sample.srvColorLogical[i] = -1;
				a_sample.srvDepthLogical[i] = -1;
			}

			const auto currentColorIndex = Engine::CurrentRenderTargetIndex();
			const auto currentColor = RenderTargets::Lookup(currentColorIndex);
			if (currentColor.valid) {
				a_sample.currentColorLogical = currentColorIndex;
				a_sample.currentColorWidth = currentColor.width;
				a_sample.currentColorHeight = currentColor.height;
			}

			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			auto viewportCount = static_cast<UINT>(std::size(viewports));
			context->RSGetViewports(&viewportCount, viewports);
			a_sample.viewportCount = viewportCount;
			if (viewportCount > 0) {
				a_sample.viewportX = viewports[0].TopLeftX;
				a_sample.viewportY = viewports[0].TopLeftY;
				a_sample.viewportWidth = viewports[0].Width;
				a_sample.viewportHeight = viewports[0].Height;
			}

			ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* depthView = nullptr;
			context->OMGetRenderTargets(
				static_cast<UINT>(std::size(renderTargets)), renderTargets, &depthView);
			ID3D11ShaderResourceView* psViews[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
			context->PSGetShaderResources(0, static_cast<UINT>(std::size(psViews)), psViews);

			for (auto* const view : renderTargets) {
				if (view) {
					++a_sample.omBoundCount;
				}
			}
			for (std::size_t i = 0; i < std::size(psViews); ++i) {
				if (!psViews[i]) {
					continue;
				}
				++a_sample.srvBoundCount;
				if (i >= kCensusSrvSlots) {
					++a_sample.srvBoundAboveScan;
				}
			}
			for (std::size_t i = 0; i < kCensusSrvSlots; ++i) {
				a_sample.srvBound[i] = psViews[i] != nullptr;
				DescribeSrv(psViews[i], a_sample.srvDesc[i]);
			}

			for (std::int32_t logical = 0; logical < Addr::kRenderTargetCount; ++logical) {
				const auto rt = RenderTargets::Lookup(logical);
				if (!rt.valid) {
					continue;
				}
				for (std::size_t i = 0; i < std::size(renderTargets); ++i) {
					if (rt.rtv && renderTargets[i] == rt.rtv) {
						a_sample.omColorLogical[i] = logical;
					}
				}
				for (std::size_t i = 0; i < kCensusSrvSlots; ++i) {
					if (rt.srv && psViews[i] == rt.srv) {
						a_sample.srvColorLogical[i] = logical;
					}
				}
			}
			for (std::int32_t logical = 0; logical < Addr::kDepthStencilCount; ++logical) {
				const auto ds = RenderTargets::LookupDepth(logical);
				if (!ds.valid) {
					continue;
				}
				if (ds.srv) {
					for (std::size_t i = 0; i < kCensusSrvSlots; ++i) {
						if (psViews[i] == ds.srv) {
							a_sample.srvDepthLogical[i] = logical;
						}
					}
				}
				if (depthView && a_sample.depthLogical < 0) {
					const auto binding = MatchDepthView(depthView, ds);
					if (binding != DepthViewBinding::unmatched && binding != DepthViewBinding::none) {
						a_sample.depthLogical = logical;
						a_sample.depthView = binding;
						a_sample.depthWidth = ds.width;
						a_sample.depthHeight = ds.height;
					}
				}
			}
			if (depthView && a_sample.depthLogical < 0) {
				a_sample.depthView = DepthViewBinding::unmatched;
			}

			for (auto*& view : psViews) {
				SafeRelease(view);
			}
			for (auto*& view : renderTargets) {
				SafeRelease(view);
			}
			SafeRelease(depthView);
		}

		void CaptureWaterSample(
			WaterSample& a_sample,
			WaterSite a_site,
			std::uintptr_t a_shaderState,
			std::uintptr_t a_viewState,
			bool a_onRenderThread,
			std::uint64_t a_call) noexcept
		{
			a_sample = {};
			a_sample.setupCall = a_call;
			a_sample.site = a_site;
			a_sample.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			a_sample.onRenderThread = a_onRenderThread;

			float matrices[32]{};
			ReadWaterConstants(
				a_sample.water,
				a_shaderState,
				a_viewState,
				matrices,
				a_sample.groups[0].data(),
				a_sample.groupFloats.data());
			if (a_sample.water.textureProjRead) {
				for (std::size_t i = 0; i < 16; ++i) {
					a_sample.textureProjLeft[i] = matrices[i];
					a_sample.textureProjRight[i] = matrices[i + 16];
				}
			}
			if (a_onRenderThread) {
				CaptureWaterBindings(a_sample);
			}
		}

		// Called by the thread that filled the LAST claimed slot, after its own write has
		// landed. The acquire on g_waterWritten pairs with every recorder's release below,
		// so every sample this copies is fully written before it is read.
		void PublishWaterObservation(WaterObservation& a_run) noexcept
		{
			a_run.completedSequence = a_run.requestedSequence;
			a_run.recorded = g_waterWritten.load(std::memory_order_acquire);

			auto* const published = g_waterPublished.load(std::memory_order_relaxed);
			if (published) {
				::AcquireSRWLockExclusive(&g_waterLock);
				published->requestedSequence = a_run.requestedSequence;
				published->completedSequence = a_run.completedSequence;
				published->requestedSamples = a_run.requestedSamples;
				published->recorded = a_run.recorded;
				published->threadId = a_run.threadId;
				const auto used = a_run.recorded < kWaterSamples ? a_run.recorded : kWaterSamples;
				for (std::uint32_t i = 0; i < used; ++i) {
					published->samples[i] = a_run.samples[i];
				}
				::ReleaseSRWLockExclusive(&g_waterLock);
			}
			// The order of these two stores is the correctness of the run, and the first
			// version had it backwards. A worker that finds the run state idle decides
			// whether to OPEN one by comparing requested with completed, so completed must
			// already carry this run's sequence by the time the state goes idle. Idled
			// first, a worker landing between the two stores read "idle" and "requested !=
			// completed", won the open exchange, and RE-OPENED the run just published -
			// resetting g_waterNextIndex and g_waterWritten, clearing slots already
			// written, and letting stragglers from the first opening add to the new count.
			// That is what published recorded = 35 against requestedSamples = 32 on the
			// quality arm of the 2026-09-07 waterpair run and of the review's independent
			// re-run of it; the reordering is a measured fix, not a theoretical one.
			//
			// Both stores are RELEASE and every reader acquire-loads the state, so the
			// pairing is exact: a worker that sees idle has synchronised with the store
			// below, and everything sequenced before it - the completed store and the
			// published copy taken under the lock - is visible to it. Relaxed here would
			// leave a compiler free to hoist the idling above the completed store and
			// re-open the same window.
			g_waterCompleted.store(a_run.requestedSequence, std::memory_order_release);
			g_waterRunState.store(kWaterRunIdle, std::memory_order_release);
		}

		void PublishUiObservation(UiObservation& a_run) noexcept
		{
			g_uiRecording.store(false, std::memory_order_relaxed);
			a_run.completedSequence = a_run.requestedSequence;

			auto* const published = g_uiPublished.load(std::memory_order_relaxed);
			if (published) {
				::AcquireSRWLockExclusive(&g_uiLock);
				published->requestedSequence = a_run.requestedSequence;
				published->completedSequence = a_run.completedSequence;
				published->requestedFrames = a_run.requestedFrames;
				published->requestedStencilReads = a_run.requestedStencilReads;
				published->stencilReads = a_run.stencilReads;
				published->recorded = a_run.recorded;
				published->threadId = a_run.threadId;
				const auto used = a_run.recorded < kUiSamples ? a_run.recorded : kUiSamples;
				for (std::uint32_t i = 0; i < used; ++i) {
					published->samples[i] = a_run.samples[i];
				}
				::ReleaseSRWLockExclusive(&g_uiLock);
			}
			g_uiCompleted.store(a_run.requestedSequence, std::memory_order_release);
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

	void OnHDRDraw(
		std::uintptr_t a_renderer,
		std::uint64_t a_hdrCall,
		std::int32_t a_destinationLogical,
		std::uint32_t a_threadId) noexcept
	{
		TryCapture(
			ObservationPoint::hdrFinalDraw,
			a_renderer,
			a_hdrCall,
			a_threadId,
			a_destinationLogical);
	}

	std::string_view ObservationPointName(ObservationPoint a_point) noexcept
	{
		switch (a_point) {
		case ObservationPoint::clearColor:
			return "clear-color"sv;
		case ObservationPoint::hdrFinalDraw:
			return "hdr-final-draw"sv;
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

	std::uint64_t RequestFrameCensus() noexcept
	{
		// Allocate before publishing the request. The release on g_censusRequested below
		// is what makes both pointers visible to the render thread, which cannot reach
		// them before it has seen the request.
		//
		// Claimed with a compare-exchange rather than load-then-store: two listener threads
		// arming at once would otherwise both see null, both allocate, and one buffer would
		// be leaked - and, worse, the render thread could be recording into the buffer the
		// loser overwrote. The loser frees its own allocation and uses the winner's.
		const auto claim = [](std::atomic<FrameCensus*>& a_slot) noexcept {
			if (a_slot.load(std::memory_order_acquire)) {
				return true;
			}
			auto* buffer = new (std::nothrow) FrameCensus{};
			if (!buffer) {
				return false;
			}
			FrameCensus* expected = nullptr;
			if (!a_slot.compare_exchange_strong(
					expected, buffer, std::memory_order_acq_rel, std::memory_order_acquire)) {
				delete buffer;
			}
			return true;
		};
		if (!claim(g_censusRecord) || !claim(g_censusPublished)) {
			return 0;
		}
		return g_censusRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	bool FrameCensusRecording() noexcept
	{
		return g_censusRecording.load(std::memory_order_relaxed);
	}

	void OnFrameBoundary() noexcept
	{
		auto* const record = g_censusRecord.load(std::memory_order_relaxed);
		if (g_censusRecording.load(std::memory_order_relaxed)) {
			// Close only on the thread that opened, so a scoped or secondary render pass
			// on another thread cannot truncate the frame being measured.
			if (record && g_censusThreadId.load(std::memory_order_relaxed) ==
			                  static_cast<std::uint32_t>(::GetCurrentThreadId())) {
				CloseCensusFrame(*record);
			}
			return;
		}
		const auto requested = g_censusRequested.load(std::memory_order_acquire);
		if (requested != 0 && requested != g_censusCompleted.load(std::memory_order_acquire)) {
			auto* const buffer = g_censusRecord.load(std::memory_order_acquire);
			if (buffer) {
				OpenCensusFrame(*buffer, requested);
			}
		}
	}

	void OnImageSpaceDispatch(
		std::int32_t a_effectSlot,
		std::int32_t a_sourceLogical,
		std::int32_t a_destinationLogical) noexcept
	{
		auto* const census = RecordingBuffer();
		if (!census) {
			return;
		}
		++census->dispatches;
		if (auto* const entry = NextCensusEntry(*census, CensusEntryKind::dispatch)) {
			entry->effectSlot = a_effectSlot;
			entry->sourceLogical = a_sourceLogical;
			entry->destinationLogical = a_destinationLogical;
		}
	}

	void OnImageSpacePixelDraw() noexcept
	{
		auto* const census = RecordingBuffer();
		if (!census) {
			return;
		}
		++census->draws;
		if (auto* const entry = NextCensusEntry(*census, CensusEntryKind::draw)) {
			CaptureCensusDraw(*entry);
		}
	}

	void OnSslrPixelDraw(SslrPass a_pass) noexcept
	{
		auto* const census = RecordingBuffer();
		if (!census) {
			return;
		}
		// Not added to draws: every earlier reading of this census quotes a draw count for
		// the ImageSpace chain, and a reflection pass is not in that chain.
		++census->sslrDraws;
		if (auto* const entry = NextCensusEntry(*census, CensusEntryKind::sslrDraw)) {
			entry->sslrPass = a_pass;
			CaptureCensusDraw(*entry);
		}
	}

	std::uint64_t RequestWaterObservation(std::uint32_t a_samples) noexcept
	{
		// Same claim shape as the census and the UI run, and for the same reason: two
		// listener threads arming at once would otherwise both allocate, and the recording
		// thread could be writing into the buffer the loser overwrote.
		const auto claim = [](std::atomic<WaterObservation*>& a_slot) noexcept {
			if (a_slot.load(std::memory_order_acquire)) {
				return true;
			}
			auto* buffer = new (std::nothrow) WaterObservation{};
			if (!buffer) {
				return false;
			}
			WaterObservation* expected = nullptr;
			if (!a_slot.compare_exchange_strong(
					expected, buffer, std::memory_order_acq_rel, std::memory_order_acquire)) {
				delete buffer;
			}
			return true;
		};
		if (!claim(g_waterRecord) || !claim(g_waterPublished)) {
			return 0;
		}

		const auto samples = a_samples == 0 ? static_cast<std::uint32_t>(kWaterSamples) :
		                                      (a_samples > kWaterSamples ?
		                                              static_cast<std::uint32_t>(kWaterSamples) :
		                                              a_samples);
		g_waterRequestedSamples.store(samples, std::memory_order_relaxed);
		return g_waterRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	void OnWaterShaderSetup(
		WaterSite a_site,
		std::uintptr_t a_shaderState,
		std::uintptr_t a_viewState,
		bool a_onRenderThread) noexcept
	{
		// Counted first and unconditionally, so a reader who sees an empty run can tell it
		// from a hook that never ran.
		const auto call = g_waterSetupCalls.fetch_add(1, std::memory_order_relaxed) + 1;

		if (auto* const census = RecordingBuffer()) {
			// Not added to draws, for the same reason a reflection pass is not: draws is
			// the ImageSpace chain's shape and the water surface is world geometry.
			if (a_site == WaterSite::setupTechnique) {
				++census->waterTechniqueSetups;
			} else {
				++census->waterGeometrySetups;
			}
			if (auto* const entry = NextCensusEntry(*census, CensusEntryKind::waterSetup)) {
				entry->waterSite = a_site;
				if (a_onRenderThread) {
					CaptureCensusDraw(*entry);
				}
				CaptureWaterConstants(*entry, *census, a_shaderState, a_viewState);
			}
		}

		auto* const run = g_waterRecord.load(std::memory_order_relaxed);
		if (!run) {
			return;
		}

		if (g_waterRunState.load(std::memory_order_acquire) != kWaterRunOpen) {
			// Open the run. The state has three values and not two for a measured reason:
			// while the opener is still clearing the buffer, no other thread may be
			// recording into it. A two-state flag published "open" at the exchange, so
			// every worker that arrived during the clear claimed a slot, captured into it,
			// and had its sample wiped by the clear loop a moment later while its increment
			// of the written count survived - which is how a run published recorded = 32
			// with all thirty-two slots empty on two arms of
			// artifacts/phase5-step9b-fix-waterpair2-20260907-1226 (first taking). The
			// opening state is claimed by the exchange and only turns into kWaterRunOpen
			// once the buffer is initialised, so a recorder never sees a half-built run.
			const auto requested = g_waterRequested.load(std::memory_order_acquire);
			if (requested == 0 || requested == g_waterCompleted.load(std::memory_order_acquire)) {
				return;
			}
			auto expected = kWaterRunIdle;
			if (!g_waterRunState.compare_exchange_strong(
					expected, kWaterRunOpening, std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				// Either another thread is building this run or one is already recording;
				// both cost this call a sample and neither can tear one.
				return;
			}
			// Re-read completed now that the exchange has ordered this thread against the
			// publisher. Redundant given the store order in PublishWaterObservation, and
			// kept because it makes the invariant local to the open: whatever a future
			// publisher does, a run whose sequence is already completed is never re-opened.
			if (g_waterCompleted.load(std::memory_order_acquire) == requested) {
				g_waterRunState.store(kWaterRunIdle, std::memory_order_release);
				return;
			}
			run->requestedSequence = requested;
			run->completedSequence = 0;
			run->requestedSamples = g_waterRequestedSamples.load(std::memory_order_relaxed);
			run->recorded = 0;
			run->threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			// Clear every slot the run may publish. Without this a slot no recorder claims
			// keeps the PREVIOUS run's sample, which is how the first version of this
			// instrument published two Quality samples inside a native arm.
			for (auto& sample : run->samples) {
				sample = {};
			}
			g_waterThreadId.store(run->threadId, std::memory_order_relaxed);
			g_waterNextIndex.store(0, std::memory_order_relaxed);
			g_waterWritten.store(0, std::memory_order_relaxed);
			// The release that hands the finished buffer to every other recorder: their
			// acquire on the state above pairs with it, so the fields written here - the
			// sample count they read, the cleared slots and both zeroed counters - are
			// visible before any of them can claim a slot.
			g_waterRunState.store(kWaterRunOpen, std::memory_order_release);
		}

		const auto requested = run->requestedSamples;
		const auto index = g_waterNextIndex.fetch_add(1, std::memory_order_relaxed);
		if (index >= requested || index >= kWaterSamples) {
			return;
		}
		CaptureWaterSample(
			run->samples[index], a_site, a_shaderState, a_viewState, a_onRenderThread, call);
		// Release, so the publisher's acquire below sees this sample complete. The count is
		// how many slots have been FILLED, not which; the last filler publishes.
		const auto written = g_waterWritten.fetch_add(1, std::memory_order_release) + 1;
		if (written >= requested || written >= kWaterSamples) {
			PublishWaterObservation(*run);
		}
	}

	void CopyWaterObservation(WaterObservation& a_out) noexcept
	{
		a_out = {};
		if (auto* const published = g_waterPublished.load(std::memory_order_acquire)) {
			::AcquireSRWLockShared(&g_waterLock);
			a_out.requestedSamples = published->requestedSamples;
			a_out.recorded = published->recorded;
			a_out.threadId = published->threadId;
			const auto used = published->recorded < kWaterSamples ? published->recorded : kWaterSamples;
			for (std::uint32_t i = 0; i < used; ++i) {
				a_out.samples[i] = published->samples[i];
			}
			::ReleaseSRWLockShared(&g_waterLock);
		}

		a_out.requestedSequence = g_waterRequested.load(std::memory_order_acquire);
		a_out.completedSequence = g_waterCompleted.load(std::memory_order_acquire);
		a_out.armed = a_out.requestedSequence != a_out.completedSequence;
		a_out.setupCalls = g_waterSetupCalls.load(std::memory_order_relaxed);
	}

	void OnSeamStateProbe(CensusProbeSite a_site, SslrPass a_pass) noexcept
	{
		auto* const census = RecordingBuffer();
		if (!census) {
			return;
		}
		// Deliberately not counted in dispatches or draws: those two are the frame's shape,
		// and a probe is an observation of our own, not a pass the engine ran.
		if (auto* const entry = NextCensusEntry(*census, CensusEntryKind::probe)) {
			entry->probeSite = a_site;
			entry->sslrPass = a_pass;
			CaptureCensusOutputMerger(*entry);
		}
	}

	void CopyFrameCensus(FrameCensus& a_out) noexcept
	{
		a_out = {};
		if (auto* const published = g_censusPublished.load(std::memory_order_acquire)) {
			::AcquireSRWLockShared(&g_censusLock);
			a_out.truncated = published->truncated;
			a_out.dispatches = published->dispatches;
			a_out.draws = published->draws;
			a_out.sslrDraws = published->sslrDraws;
			a_out.waterGeometrySetups = published->waterGeometrySetups;
			a_out.waterTechniqueSetups = published->waterTechniqueSetups;
			a_out.count = published->count;
			a_out.threadId = published->threadId;
			a_out.colorTargets = published->colorTargets;
			a_out.depthTargets = published->depthTargets;
			a_out.waterSamples = published->waterSamples;
			const auto used = published->count < kCensusEntries ? published->count : kCensusEntries;
			for (std::uint32_t i = 0; i < used; ++i) {
				a_out.entries[i] = published->entries[i];
			}
			::ReleaseSRWLockShared(&g_censusLock);
		}

		a_out.requestedSequence = g_censusRequested.load(std::memory_order_acquire);
		a_out.completedSequence = g_censusCompleted.load(std::memory_order_acquire);
		a_out.pending = a_out.requestedSequence != a_out.completedSequence;
		a_out.recording = g_censusRecording.load(std::memory_order_relaxed);
	}

	std::uint64_t RequestUiObservation(std::uint32_t a_frames, std::uint32_t a_stencilReads) noexcept
	{
		// Same claim shape as RequestFrameCensus, and for the same reason: two listener
		// threads arming at once would otherwise both allocate, and the render thread
		// could be recording into the buffer the loser overwrote.
		const auto claim = [](std::atomic<UiObservation*>& a_slot) noexcept {
			if (a_slot.load(std::memory_order_acquire)) {
				return true;
			}
			auto* buffer = new (std::nothrow) UiObservation{};
			if (!buffer) {
				return false;
			}
			UiObservation* expected = nullptr;
			if (!a_slot.compare_exchange_strong(
					expected, buffer, std::memory_order_acq_rel, std::memory_order_acquire)) {
				delete buffer;
			}
			return true;
		};
		if (!claim(g_uiRecord) || !claim(g_uiPublished)) {
			return 0;
		}

		const auto frames = a_frames == 0 ? static_cast<std::uint32_t>(kUiSamples) :
		                                    (a_frames > kUiSamples ? static_cast<std::uint32_t>(kUiSamples) : a_frames);
		// Clamped to both bounds rather than refused: a caller asking for more stencil
		// reads than the run has frames wants every frame read, and a caller asking for
		// more than the cap wants as many as the cap allows. Neither is a mistake worth
		// failing an arm over, and the run reports what it actually took.
		auto stencilReads = a_stencilReads > kUiStencilReadsMax ? kUiStencilReadsMax : a_stencilReads;
		stencilReads = stencilReads > frames ? frames : stencilReads;
		g_uiRequestedFrames.store(frames, std::memory_order_relaxed);
		g_uiRequestedStencilReads.store(stencilReads, std::memory_order_relaxed);
		return g_uiRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	void OnUiRenderEntry(std::int32_t a_requestedTargetId) noexcept
	{
		// Counted first and unconditionally: a reader who sees a run of 0 samples needs to
		// know whether the hook never ran or the run simply has not opened yet, and
		// CaptureUiSample reads this same counter back into the sample.
		g_uiCalls.fetch_add(1, std::memory_order_relaxed);

		auto* const run = g_uiRecord.load(std::memory_order_acquire);
		if (!run) {
			return;
		}

		if (g_uiRecording.load(std::memory_order_relaxed)) {
			// Record only on the thread that opened the run, for the reason the census
			// gives: this hook is not thread-checked, and reading the immediate context
			// from a second thread while the render thread drives it is the one mistake
			// this file must not make.
			if (g_uiThreadId.load(std::memory_order_relaxed) !=
				static_cast<std::uint32_t>(::GetCurrentThreadId())) {
				return;
			}
			if (run->recorded < kUiSamples && run->recorded < run->requestedFrames) {
				const bool readStencil = run->stencilReads < run->requestedStencilReads;
				CaptureUiSample(run->samples[run->recorded], a_requestedTargetId, readStencil);
				++run->recorded;
				if (readStencil) {
					++run->stencilReads;
				}
			}
			if (run->recorded >= run->requestedFrames || run->recorded >= kUiSamples) {
				PublishUiObservation(*run);
			}
			return;
		}

		const auto requested = g_uiRequested.load(std::memory_order_acquire);
		if (requested == 0 || requested == g_uiCompleted.load(std::memory_order_acquire)) {
			return;
		}
		run->requestedSequence = requested;
		run->completedSequence = 0;
		run->requestedFrames = g_uiRequestedFrames.load(std::memory_order_relaxed);
		run->requestedStencilReads = g_uiRequestedStencilReads.load(std::memory_order_relaxed);
		run->recorded = 0;
		run->stencilReads = 0;
		run->threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
		g_uiThreadId.store(run->threadId, std::memory_order_relaxed);
		g_uiRecording.store(true, std::memory_order_relaxed);
		if (run->recorded < run->requestedFrames) {
			const bool readStencil = run->stencilReads < run->requestedStencilReads;
			CaptureUiSample(run->samples[run->recorded], a_requestedTargetId, readStencil);
			++run->recorded;
			if (readStencil) {
				++run->stencilReads;
			}
		}
		if (run->recorded >= run->requestedFrames) {
			PublishUiObservation(*run);
		}
	}

	void CopyUiObservation(UiObservation& a_out) noexcept
	{
		a_out = {};
		if (auto* const published = g_uiPublished.load(std::memory_order_acquire)) {
			::AcquireSRWLockShared(&g_uiLock);
			a_out.requestedFrames = published->requestedFrames;
			a_out.requestedStencilReads = published->requestedStencilReads;
			a_out.stencilReads = published->stencilReads;
			a_out.recorded = published->recorded;
			a_out.threadId = published->threadId;
			const auto used = published->recorded < kUiSamples ? published->recorded : kUiSamples;
			for (std::uint32_t i = 0; i < used; ++i) {
				a_out.samples[i] = published->samples[i];
			}
			::ReleaseSRWLockShared(&g_uiLock);
		}

		a_out.requestedSequence = g_uiRequested.load(std::memory_order_acquire);
		a_out.completedSequence = g_uiCompleted.load(std::memory_order_acquire);
		a_out.armed = a_out.requestedSequence != a_out.completedSequence;
		a_out.uiCalls = g_uiCalls.load(std::memory_order_relaxed);
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
