#include "Upscaler/Debug.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"
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

		// Resolved once when a census frame opens rather than per draw: a logical id's
		// views do not change inside a frame, and 144 + 17 lookups per draw would price
		// the instrument rather than the passes it is measuring.
		std::array<RenderTargets::PhysRT, Addr::kRenderTargetCount> g_censusColorTable{};
		std::array<RenderTargets::PhysDS, Addr::kDepthStencilCount> g_censusDepthTable{};

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

		void OpenCensusFrame(FrameCensus& a_census, std::uint64_t a_sequence) noexcept
		{
			for (std::int32_t logical = 0; logical < Addr::kRenderTargetCount; ++logical) {
				g_censusColorTable[static_cast<std::size_t>(logical)] = RenderTargets::Lookup(logical);
			}
			for (std::int32_t logical = 0; logical < Addr::kDepthStencilCount; ++logical) {
				g_censusDepthTable[static_cast<std::size_t>(logical)] = RenderTargets::LookupDepth(logical);
			}
			a_census.requestedSequence = a_sequence;
			a_census.completedSequence = 0;
			a_census.pending = true;
			a_census.recording = true;
			a_census.truncated = false;
			a_census.dispatches = 0;
			a_census.draws = 0;
			a_census.count = 0;
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
				published->count = a_census.count;
				published->threadId = a_census.threadId;
				const auto used = a_census.count < kCensusEntries ? a_census.count : kCensusEntries;
				for (std::uint32_t i = 0; i < used; ++i) {
					published->entries[i] = a_census.entries[i];
				}
				::ReleaseSRWLockExclusive(&g_censusLock);
			}
			g_censusCompleted.store(a_census.requestedSequence, std::memory_order_release);
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
		if (!g_censusRecord.load(std::memory_order_relaxed)) {
			auto* const buffer = new (std::nothrow) FrameCensus{};
			if (!buffer) {
				return 0;
			}
			g_censusRecord.store(buffer, std::memory_order_relaxed);
		}
		if (!g_censusPublished.load(std::memory_order_relaxed)) {
			auto* const buffer = new (std::nothrow) FrameCensus{};
			if (!buffer) {
				return 0;
			}
			g_censusPublished.store(buffer, std::memory_order_relaxed);
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

	void CopyFrameCensus(FrameCensus& a_out) noexcept
	{
		a_out = {};
		if (auto* const published = g_censusPublished.load(std::memory_order_acquire)) {
			::AcquireSRWLockShared(&g_censusLock);
			a_out.truncated = published->truncated;
			a_out.dispatches = published->dispatches;
			a_out.draws = published->draws;
			a_out.count = published->count;
			a_out.threadId = published->threadId;
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
