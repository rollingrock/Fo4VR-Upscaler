#pragma once

#include "Upscaler/Backend/BackendTypes.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>
#include <dxgi.h>

namespace Upscaler::Backend
{
	// The seam. Five invariants make swapping a backend a swap rather than a rewrite,
	// and they are worth stating here because every one of them is cheap to hold on day
	// one and expensive to retrofit.
	//
	// 1. Evaluate takes ONE eye. All stereo knowledge - double-wide geometry, per-eye
	//    rectangles, matched jitter phase, paired resets - lives above this interface.
	//    A backend cannot get stereo wrong because it cannot see it.
	// 2. Colour/motion/output subrect versus copy is a Caps bit, not a code path inside
	//    a backend. Per-eye intermediates are the shipping path; a true makes direct
	//    subrects an optimisation. Depth is the exception D3D11 imposes: it cannot be
	//    partially copied, so EyeView::depthRect may address the renderer's whole depth
	//    resource and a backend that cannot consume it must decline or convert it.
	// 3. No vendor type crosses this interface. Only D3D11 resource pointers, plain
	//    data and std::string. Each vendor's symbols appear in exactly one translation
	//    unit, and grepping a vendor prefix must return one file.
	// 4. The preset is a string, never an enum. See Config::preset.
	// 5. Backends link no engine code. This is enforced mechanically: they compile into
	//    a static library that does not link CommonLibF4, so a backend reaching for an
	//    engine type is a build error rather than a review comment.
	//
	// That last invariant decides how a backend logs. F4SE::log - the logger:: alias
	// the rest of the plugin uses - is CommonLibF4, and this library also receives no
	// precompiled header. Inside upscaler_backends logging is raw spdlog::info /
	// spdlog::warn, and the message text names the backend itself, because the source
	// file and line fields the log pattern is built around are not captured across the
	// seam.
	class IBackend
	{
	public:
		virtual ~IBackend() = default;

		IBackend() = default;
		IBackend(const IBackend&) = delete;
		IBackend(IBackend&&) = delete;
		IBackend& operator=(const IBackend&) = delete;
		IBackend& operator=(IBackend&&) = delete;

		// 1. Identity, for the log and for TOML selection.
		// "ngx" / "streamline" / "ffx" / "null".
		[[nodiscard]] virtual std::string_view Name() const noexcept = 0;

		// 2. Side-effect-free capability probe. Must be safe to call with a null device,
		// in which case it answers only "is the runtime present at all" - the startup
		// self-check calls it on every compiled-in backend before a device exists.
		// Never throws, never leaves a runtime loaded that it cannot unload.
		[[nodiscard]] virtual Caps Probe(ID3D11Device* a_device) const noexcept = 0;

		// 3. Load the runtime from an explicit directory and bind the device. Never
		// throws. A false return must have left unavailableReason set on the next Probe.
		[[nodiscard]] virtual bool Initialize(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_immediate,
			const std::filesystem::path& a_runtimeDir) noexcept = 0;

		// 4. Create a_viewCount INDEPENDENT reconstruction contexts, each with its own
		// history. a_viewCount is 2 for stereo. Idempotent, and re-creates when the
		// config changes. Feeding one context a double-wide image would put a hard
		// disparity discontinuity down the middle of its history, which is why this
		// takes a count rather than assuming one.
		[[nodiscard]] virtual bool CreateViews(
			const Config& a_config,
			std::uint32_t a_viewCount) noexcept = 0;

		// 5. The vendor's own optimal render size for a quality mode. Never our
		// arithmetic - the ratios are tuned against the vendor's own training.
		[[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> QueryRenderSize(
			std::uint32_t a_outputWidth,
			std::uint32_t a_outputHeight,
			QualityMode a_quality) const noexcept = 0;

		// 6. The vendor's own mip-bias formula. They differ, which is why this sits here
		// rather than in a shared helper.
		[[nodiscard]] virtual float RecommendedMipBias(
			std::uint32_t a_renderWidth,
			std::uint32_t a_outputWidth) const noexcept = 0;

		// 7 and 8. The backend owns the jitter sequence it was trained on. Split into
		// two calls so the phase count is available to the mip-bias and telemetry paths
		// without a dummy offset call. Phase count scales with the square of the ratio;
		// a sequence tuned for one-to-one temporal anti-aliasing is far too short for
		// reconstruction, and the symptom is shimmer rather than an error. A phase count
		// is never zero - the caller takes a modulo of it.
		[[nodiscard]] virtual std::uint32_t JitterPhaseCount(
			std::uint32_t a_renderWidth,
			std::uint32_t a_outputWidth) const noexcept = 0;

		virtual void JitterOffset(
			std::uint32_t a_frameIndex,
			std::uint32_t a_phaseCount,
			float& a_outX,
			float& a_outY) const noexcept = 0;

		// 9. ONE eye. Called twice per frame. This is the whole point of the seam. Must
		// be called on the thread that owns the immediate context.
		[[nodiscard]] virtual bool Evaluate(
			ID3D11DeviceContext* a_immediate,
			const EyeView& a_view) noexcept = 0;

		// 10. Teardown. Both must be safe to call twice and safe to call from a fault
		// path, because the fault path is exactly when they run. A false DestroyViews
		// retains the live state; the caller must preserve its identity and skip Shutdown
		// so safe release can be retried rather than falsely reported as complete.
		[[nodiscard]] virtual bool DestroyViews() noexcept = 0;
		// False likewise means implementation-owned state remains live and the caller must
		// retain the backend identity for a later render-thread retry.
		virtual bool Shutdown() noexcept = 0;

		// Optional device-creation seam. Declared on the base even though only one
		// backend family needs it, so adding that family later is a backend swap rather
		// than a change to the plugin entry point. Called only when the selected
		// backend's Caps reports needsDeviceCreation. A false return means decline
		// device creation entirely, so the result is the point.
		[[nodiscard]] virtual bool OnPreCreateDevice(
			DXGI_SWAP_CHAIN_DESC* a_desc,
			std::vector<D3D_FEATURE_LEVEL>& a_levels) noexcept
		{
			(void)a_desc;
			(void)a_levels;
			return true;
		}

		virtual void OnPostCreateDevice(
			IDXGIAdapter* a_adapter,
			ID3D11Device** a_device,
			IDXGISwapChain** a_swapChain) noexcept
		{
			(void)a_adapter;
			(void)a_device;
			(void)a_swapChain;
		}
	};
}
