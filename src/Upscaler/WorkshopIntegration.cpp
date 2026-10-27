#include "Upscaler/WorkshopIntegration.h"

#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/WorkshopDrawHooks.h"
#include "Upscaler/WorkshopMask.h"
#include "Upscaler/WorkshopMaskShader.h"

#include <atomic>
#include <cmath>
#include <wrl/client.h>

namespace Upscaler::WorkshopIntegration
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		std::atomic_bool g_installed{ false }, g_enabled{ false }, g_terminal{ false };
		bool g_drawDiagnostics = false;  // Immutable once startup publishes the draw callbacks.
		std::atomic<std::uint64_t> g_generation{ 1 }, g_captures{ 0 }, g_dispatches{ 0 };
		std::atomic<std::uint64_t> g_draws{ 0 }, g_corrected{ 0 }, g_restores{ 0 }, g_declines{ 0 }, g_missed{ 0 };
		std::atomic<std::uint64_t> g_routes[4]{};
		std::atomic<std::uint64_t> g_pairDraws1To3{ 0 }, g_pairDraws3To1{ 0 };
		std::atomic<std::uint64_t> g_callbacks{ 0 }, g_scopedCallbacks{ 0 }, g_nestedCallbacks{ 0 }, g_bindingDeclines{ 0 };
		std::atomic<std::uint32_t> g_lastDecline{ 0 };
		// Render-thread-owned, with references protecting captured identity from reuse.
		ComPtr<ID3D11Texture2D> g_sourceTexture;
		ComPtr<ID3D11ShaderResourceView> g_sourceView;
		WorkshopMaskShader::Constants g_rect{};
		ID3D11DeviceContext* g_context = nullptr;
		std::uint32_t g_thread = 0, g_sourceLogical = 0, g_destinationLogical = 0;
		std::uint64_t g_epoch = 0, g_capturedGeneration = 0;
		HDRRoundTrip::HandbackStamp g_stamp{};
		thread_local bool g_effect = false;
		thread_local std::uint32_t g_nested = 0;
		bool g_prepared = false, g_seen = false, g_consumed = false, g_bound = false;
		D3D11StateIsolation::PixelResourceOverrideToken g_override{};

		bool OnRenderThread() noexcept
		{
			const auto probe = Hooks::RenderThreadProbe();
			return probe.threadId && probe.threadId == ::GetCurrentThreadId();
		}

		void Decline(std::uint32_t a_reason) noexcept
		{
			g_declines.fetch_add(1, std::memory_order_relaxed);
			g_lastDecline.store(a_reason, std::memory_order_relaxed);
		}

		void Terminal() noexcept
		{
			g_terminal.store(true, std::memory_order_release);
			D3D11StateIsolation::LatchExternalStateUncertain();
		}

		// Getter references are local to this non-SEH helper and always released.
		bool DrawBindingsMatch(ID3D11DeviceContext* a_context)
		{
			if (!g_epoch || a_context != g_context || ::GetCurrentThreadId() != g_thread ||
				ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
				return false;
			}
			const auto stamp = HDRRoundTrip::HandbackCandidateStamp();
			if (!stamp.serial || stamp.serial != g_stamp.serial || stamp.source != g_stamp.source ||
				!HDRRoundTrip::QualityRuntimeConfigReady() ||
				g_capturedGeneration != g_generation.load(std::memory_order_acquire)) {
				return false;
			}
			const auto source = RenderTargets::Lookup(static_cast<std::int32_t>(g_sourceLogical));
			const auto destination = RenderTargets::Lookup(static_cast<std::int32_t>(g_destinationLogical));
			const auto mask = RenderTargets::Lookup(17);
			if (!source.valid || !destination.valid || !mask.valid ||
				mask.tex != g_sourceTexture.Get() || mask.srv != g_sourceView.Get() ||
				source.width != g_rect.outputEyeWidth * 2 || source.height != g_rect.outputHeight ||
				!source.srv || !destination.rtv || destination.width != g_rect.outputEyeWidth * 2 ||
				destination.height != g_rect.outputHeight) {
				return false;
			}
			ID3D11ShaderResourceView* views[2]{};
			a_context->PSGetShaderResources(0, 2, views);
			ComPtr<ID3D11ShaderResourceView> scene, target;
			scene.Attach(views[0]);
			target.Attach(views[1]);
			ComPtr<ID3D11RenderTargetView> output;
			a_context->OMGetRenderTargets(1, output.GetAddressOf(), nullptr);
			D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			a_context->RSGetViewports(&count, viewport);
			return scene.Get() == source.srv && target.Get() == g_sourceView.Get() &&
			       output.Get() == destination.rtv && count == 1 && viewport[0].TopLeftX == 0 &&
			       viewport[0].TopLeftY == 0 && viewport[0].Width == g_rect.outputEyeWidth * 2 &&
			       viewport[0].Height == g_rect.outputHeight;
		}

		WorkshopDrawHooks::Decision BeforeDraw(ID3D11DeviceContext* a_context,
			WorkshopDrawHooks::Route a_route, const WorkshopDrawHooks::DrawArguments&) noexcept
		{
			using Decision = WorkshopDrawHooks::Decision;
			if (g_drawDiagnostics) {
				g_callbacks.fetch_add(1, std::memory_order_relaxed);
			}
			if (!g_effect) {
				return Decision::kPassThrough;
			}
			g_scopedCallbacks.fetch_add(1, std::memory_order_relaxed);
			if (g_terminal.load(std::memory_order_acquire) || D3D11StateIsolation::HostStateUncertain()) {
				Terminal();
				return Decision::kSkip;
			}
			if (g_nested) {
				g_nestedCallbacks.fetch_add(1, std::memory_order_relaxed);
				return Decision::kPassThrough;
			}
			bool matched = false;
			__try {
				matched = DrawBindingsMatch(a_context);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				matched = false;
			}
			if (!matched || g_seen) {
				g_bindingDeclines.fetch_add(1, std::memory_order_relaxed);
				Decline(5);
				return Decision::kPassThrough;
			}
			g_seen = true;
			g_routes[static_cast<std::size_t>(a_route)].fetch_add(1, std::memory_order_relaxed);
			g_draws.fetch_add(1, std::memory_order_relaxed);
			(g_sourceLogical == 1 ? g_pairDraws1To3 : g_pairDraws3To1).fetch_add(1, std::memory_order_relaxed);
			// Read-only route proof remains available with correction disabled.
			if (!g_prepared || !g_enabled.load(std::memory_order_acquire)) {
				return Decision::kOwned;
			}
			g_override = {};
			g_bound = D3D11StateIsolation::BeginPixelResourceOverride(
				a_context, 1, g_sourceView.Get(), WorkshopMask::Output(), g_override);
			if (!g_bound) {
				Decline(6);
				if (g_override.stateUncertain || D3D11StateIsolation::HostStateUncertain()) {
					Terminal();
					return Decision::kSkip;
				}
			}
			return Decision::kOwned;
		}

		void AfterDraw(ID3D11DeviceContext*, WorkshopDrawHooks::Route,
			const WorkshopDrawHooks::DrawArguments&, WorkshopDrawHooks::Decision a_decision) noexcept
		{
			if (!g_effect) {
				return;
			}
			if (g_bound && a_decision == WorkshopDrawHooks::Decision::kOwned) {
				g_corrected.fetch_add(1, std::memory_order_relaxed);
			}
			if (g_override.original) {
				if (!D3D11StateIsolation::EndPixelResourceOverride(g_override)) {
					Terminal();
				} else if (g_bound) {
					g_restores.fetch_add(1, std::memory_order_relaxed);
				}
			}
			g_bound = false;
		}

		void CaptureImpl(std::uint64_t a_epoch)
		{
			const auto dynamic = DynamicResolution::Snapshot();
			if (!a_epoch || !dynamic.worldFrameActive) {
				return;
			}
			const auto mask = RenderTargets::Lookup(17);
			auto* const context = Engine::ImmediateContext();
			if (!context || !mask.valid || !mask.tex || !mask.srv || !mask.rtv) {
				Decline(1);
				return;
			}
			ComPtr<ID3D11RenderTargetView> bound;
			context->OMGetRenderTargets(1, bound.GetAddressOf(), nullptr);
			D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			context->RSGetViewports(&count, viewport);
			// Deliberately only the observed zero-origin main-world contract is admitted.
			if (bound.Get() != mask.rtv || count != 1 || viewport[0].TopLeftX != 0 || viewport[0].TopLeftY != 0 ||
				!std::isfinite(viewport[0].Width) || !std::isfinite(viewport[0].Height) ||
				viewport[0].Width < 2 || viewport[0].Width >= mask.width ||
				viewport[0].Height < 1 || viewport[0].Height > mask.height ||
				mask.width > WorkshopMaskShader::kMaxTextureDimension ||
				mask.height > WorkshopMaskShader::kMaxTextureDimension || (mask.width & 1u) != 0 ||
				std::floor(viewport[0].Width / 2) != viewport[0].Width / 2 ||
				std::floor(viewport[0].Height) != viewport[0].Height) {
				Decline(2);
				return;
			}
			// The vendor transaction is prepared later. Retain the actual raster extent
			// now and require its exact agreement with that transaction at effect12.
			g_rect = { 0, 0, static_cast<std::uint32_t>(viewport[0].Width / 2),
				static_cast<std::uint32_t>(viewport[0].Height), mask.width / 2, mask.height, mask.width, mask.height };
			if (!WorkshopMaskShader::Validate(g_rect)) {
				Decline(2);
				return;
			}
			g_sourceTexture = mask.tex;
			g_sourceView = mask.srv;
			g_context = context;
			g_thread = ::GetCurrentThreadId();
			g_epoch = a_epoch;
			g_capturedGeneration = g_generation.load(std::memory_order_acquire);
			g_captures.fetch_add(1, std::memory_order_relaxed);
		}
	}

	bool Install(bool a_drawDiagnostics) noexcept
	{
		g_drawDiagnostics = a_drawDiagnostics;
		const bool installed = WorkshopDrawHooks::Install(BeforeDraw, AfterDraw);
		g_installed.store(installed, std::memory_order_release);
		return installed;
	}

	void Configure(bool a_correct) noexcept
	{
		g_enabled.store(a_correct, std::memory_order_release);
		g_generation.fetch_add(1, std::memory_order_acq_rel);
		if (!a_correct) {
			WorkshopMask::RequestRelease();
		}
	}

	void ClearFrame() noexcept
	{
		if (!OnRenderThread()) {
			return;
		}
		g_epoch = 0;
		if (g_effect || g_override.original) {
			return;
		}
		g_sourceView.Reset();
		g_sourceTexture.Reset();
		g_epoch = 0;
		g_prepared = false;
		g_consumed = false;
	}

	void CaptureBeforeHDR(std::uint64_t a_epoch) noexcept
	{
		if (!g_installed.load(std::memory_order_acquire) || g_terminal.load(std::memory_order_acquire)) {
			return;
		}
		if (!OnRenderThread()) {
			Decline(3);
			return;
		}
		if (g_effect || g_override.original) {
			g_epoch = 0;
			return;
		}
		ClearFrame();
		WorkshopMask::ReleaseIfRequested();
		__try {
			CaptureImpl(a_epoch);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ClearFrame();
			Decline(1);
		}
	}

	EffectScope BeginEffect(std::uint64_t a_epoch, std::uint32_t a_source,
		std::uint32_t a_destination, bool a_owned) noexcept
	{
		// Effect 12 is the engine's own VATS and workshop highlight pass. After a latch it
		// still runs natively; only an effect already in flight, or a private view that
		// could not be proven unbound, holds it - the latter for the rest of the session,
		// since nothing can prove that binding afterwards.
		if (Unrecoverable()) {
			if (!OnRenderThread()) {
				return EffectScope::native;
			}
			return g_effect || g_override.original ? EffectScope::hold : EffectScope::native;
		}
		if (g_effect) {
			Decline(3);
			// A nested native effect must neither inherit nor close the outer override.
			// Re-entry while its private view is actually bound cannot safely forward.
			if (g_override.original) {
				Terminal();
				return EffectScope::hold;
			}
			g_epoch = 0;  // Nested rendering invalidates the outer mask-content witness.
			++g_nested;
			return EffectScope::nestedNative;
		}
		if (!OnRenderThread()) {
			Decline(3);
			return EffectScope::native;
		}
		// Native ImageSpace alternates these scene targets. Effect 5 changes the
		// parity before effect 12; both orders are captured in the workshop census.
		const bool scenePair = (a_source == 1 && a_destination == 3) ||
		                       (a_source == 3 && a_destination == 1);
		if (!a_owned || !g_installed.load(std::memory_order_acquire) || !g_epoch || g_epoch != a_epoch ||
			g_consumed || !g_sourceView || !scenePair ||
			g_capturedGeneration != g_generation.load(std::memory_order_acquire)) {
			return EffectScope::native;
		}
		g_consumed = true;
		if (::GetCurrentThreadId() != g_thread || Engine::ImmediateContext() != g_context) {
			Decline(3);
			return EffectScope::native;
		}
		g_sourceLogical = a_source;
		g_destinationLogical = a_destination;
		g_stamp = HDRRoundTrip::HandbackCandidateStamp();
		HDRRoundTrip::QualityRenderExtent extent{};
		if (!HDRRoundTrip::StagedQualityRenderExtent(g_stamp, extent) ||
			extent.renderWidth != g_rect.sourceEyeWidth || extent.renderHeight != g_rect.sourceHeight ||
			extent.outputWidth != g_rect.outputEyeWidth || extent.outputHeight != g_rect.outputHeight) {
			Decline(3);
			return EffectScope::native;
		}
		g_seen = false;
		g_prepared = false;
		g_bound = false;
		g_override = {};
		g_dispatches.fetch_add(1, std::memory_order_relaxed);
		if (g_enabled.load(std::memory_order_acquire)) {
			const auto mask = RenderTargets::Lookup(17);
			if (mask.tex != g_sourceTexture.Get() || mask.srv != g_sourceView.Get()) {
				Decline(4);
				return EffectScope::native;
			}
			auto result = WorkshopMask::PrepareResult::declined;
			__try {
				result = WorkshopMask::Prepare(Engine::Device(), g_context,
					g_sourceTexture.Get(), g_sourceView.Get(), g_rect);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				Terminal();
				return EffectScope::hold;
			}
			if (result == WorkshopMask::PrepareResult::hostStateUncertain) {
				Terminal();
				return EffectScope::hold;
			}
			g_prepared = result == WorkshopMask::PrepareResult::prepared;
			if (!g_prepared) {
				Decline(4);
			}
		}
		g_effect = true;
		return EffectScope::owned;
	}

	void EndEffect(EffectScope a_scope) noexcept
	{
		if (a_scope == EffectScope::nestedNative) {
			if (g_nested) {
				--g_nested;
			}
			return;
		}
		if (a_scope != EffectScope::owned || !g_effect) {
			return;
		}
		if (g_override.original && !D3D11StateIsolation::EndPixelResourceOverride(g_override)) {
			Terminal();
		}
		if (!g_seen) {
			g_missed.fetch_add(1, std::memory_order_relaxed);
		}
		g_effect = false;
		g_prepared = false;
		g_bound = false;
	}

	bool Unrecoverable() noexcept { return g_terminal.load(std::memory_order_acquire); }
	bool Installed() noexcept { return g_installed.load(std::memory_order_acquire); }

	std::string SnapshotJson()
	{
		const auto mask = WorkshopMask::GetSnapshot();
		return fmt::format(FMT_STRING(R"({{"hooks":{},"enabled":{},"terminal":{},"captures":{},"dispatches":{},"draws":{},"corrected":{},"restores":{},"declines":{},"missed":{},"lastDecline":{},"routes":[{},{},{},{}],"preparations":{},"preparationDeclines":{},"preparationReason":{},"resourceBuilds":{},"width":{},"height":{},"callbacks":{},"scopedCallbacks":{},"nestedCallbacks":{},"bindingDeclines":{},"pairDraws1To3":{},"pairDraws3To1":{}}})"),
			g_installed.load(), g_enabled.load(), g_terminal.load(), g_captures.load(), g_dispatches.load(),
			g_draws.load(), g_corrected.load(), g_restores.load(), g_declines.load(), g_missed.load(), g_lastDecline.load(),
			g_routes[0].load(), g_routes[1].load(), g_routes[2].load(), g_routes[3].load(),
			mask.prepared, mask.declines, mask.lastDecline, mask.resourceBuilds, mask.outputWidth, mask.outputHeight,
			g_callbacks.load(), g_scopedCallbacks.load(), g_nestedCallbacks.load(), g_bindingDeclines.load(),
			g_pairDraws1To3.load(), g_pairDraws3To1.load());
	}
}
