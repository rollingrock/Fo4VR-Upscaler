#include "Upscaler/Backend/NullBackend.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

namespace Upscaler::Backend
{
	std::string_view NullBackend::Name() const noexcept
	{
		return "null";
	}

	Caps NullBackend::Probe(ID3D11Device*) const noexcept
	{
		Caps caps{};
		caps.available = true;

		// It copies pixels and nothing else, so any rectangle is a rectangle it can
		// honour. Saying true here is what lets the stage layer exercise its subrect
		// path against the control arm before a vendor backend can accept one.
		caps.supportsSubrects = true;

		caps.needsDeviceCreation = false;
		caps.supportsExposure = false;
		caps.supportsReactiveMask = false;
		caps.ownsSharpening = false;

		SetText(caps.runtimeVersion, "n/a");

		// runtimePath stays empty: there is no runtime, and an invented path in the
		// self-check block would be the one line in it that lies.
		return caps;
	}

	bool NullBackend::Initialize(ID3D11Device*, ID3D11DeviceContext*, const std::filesystem::path&) noexcept
	{
		// Deliberately permissive about a null device and context. This backend is the
		// fail-soft target, so it must come up even on the path where device acquisition
		// failed - the log still gets to say the plugin reached the backend layer.
		// Evaluate is handed the context it needs and checks it there.
		if (!_initialized) {
			_initialized = true;
			spdlog::info("null backend: initialised. It loads no runtime and reconstructs nothing");
		}

		return true;
	}

	bool NullBackend::CreateViews(const Config& a_config, std::uint32_t a_viewCount) noexcept
	{
		if (!_initialized) {
			spdlog::warn("null backend: CreateViews called before Initialize");
			return false;
		}

		if (a_viewCount == 0) {
			spdlog::warn("null backend: CreateViews asked for zero views");
			return false;
		}

		_config = a_config;
		_viewCount = a_viewCount;

		spdlog::info("null backend: {} view(s) at {}x{} in, {}x{} out per eye",
			a_viewCount,
			a_config.renderWidth,
			a_config.renderHeight,
			a_config.outputWidth,
			a_config.outputHeight);

		return true;
	}

	std::pair<std::uint32_t, std::uint32_t> NullBackend::QueryRenderSize(
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight,
		QualityMode) const noexcept
	{
		// Every quality mode is ratio 1.0 here. A control arm that quietly rendered at
		// a reduced size would make an A/B measurement compare two different pictures.
		return { a_outputWidth, a_outputHeight };
	}

	float NullBackend::RecommendedMipBias(std::uint32_t, std::uint32_t) const noexcept
	{
		return 0.0f;
	}

	std::uint32_t NullBackend::JitterPhaseCount(std::uint32_t, std::uint32_t) const noexcept
	{
		// One, not zero: the caller takes a modulo of this.
		return 1;
	}

	void NullBackend::JitterOffset(std::uint32_t, std::uint32_t, float& a_outX, float& a_outY) const noexcept
	{
		a_outX = 0.0f;
		a_outY = 0.0f;
	}

	bool NullBackend::Evaluate(ID3D11DeviceContext* a_immediate, const EyeView& a_view) noexcept
	{
		// Both declines below are bounded rather than rate-gated: each is a caller
		// defect, not a scene condition, and Evaluate is a per-frame path where an
		// unbounded warn is its own bug.
		if (!_initialized || _viewCount == 0) {
			if (_declineLogs < 5) {
				++_declineLogs;
				spdlog::warn("null backend: evaluate declined - Initialize and CreateViews have not both run");
			}
			return false;
		}

		if (!a_immediate || !a_view.color || !a_view.output || a_view.index >= _viewCount) {
			if (_declineLogs < 5) {
				++_declineLogs;
				spdlog::warn("null backend: evaluate declined for eye {} - context, colour or output was null, or the eye index is out of range for {} view(s)",
					a_view.index,
					_viewCount);
			}
			return false;
		}

		if (!_loggedFirstEvaluate) {
			_loggedFirstEvaluate = true;
			spdlog::info("null backend: passthrough evaluate is running. Depth, motion vectors, exposure, jitter and history resets are accepted and ignored");
		}

		// Same resource in and out is the normal case when the caller is measuring its
		// own overhead. There is nothing to copy and nothing that can fail.
		if (a_view.color == a_view.output) {
			return true;
		}

		if (a_view.colorRect.w == 0 || a_view.colorRect.h == 0) {
			a_immediate->CopyResource(a_view.output, a_view.color);
			return true;
		}

		// Subrect form. supportsSubrects is true for this backend, so the stage layer
		// is allowed to point both rects at one half of a double-wide target without
		// copying that half out first.
		D3D11_BOX box{};
		box.left = a_view.colorRect.x;
		box.top = a_view.colorRect.y;
		box.front = 0;
		box.right = a_view.colorRect.x + a_view.colorRect.w;
		box.bottom = a_view.colorRect.y + a_view.colorRect.h;
		box.back = 1;

		a_immediate->CopySubresourceRegion(
			a_view.output,
			0,
			a_view.outputRect.x,
			a_view.outputRect.y,
			0,
			a_view.color,
			0,
			&box);

		return true;
	}

	void NullBackend::DestroyViews() noexcept
	{
		_config = Config{};
		_viewCount = 0;
		_loggedFirstEvaluate = false;
	}

	void NullBackend::Shutdown() noexcept
	{
		DestroyViews();

		if (_initialized) {
			_initialized = false;
			spdlog::info("null backend: shut down");
		}
	}
}
