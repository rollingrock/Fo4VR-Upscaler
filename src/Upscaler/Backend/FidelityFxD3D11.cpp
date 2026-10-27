#include "Upscaler/Backend/FidelityFxD3D11.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

// UPSCALER_ENABLE_FFX is defined by CMakeLists.txt from the option of the same name,
// which is OFF by default. The file compiles either way so the stub keeps type-checking
// against IBackend and cannot rot.
#ifndef UPSCALER_ENABLE_FFX

namespace Upscaler::Backend
{
	namespace
	{
		constexpr std::string_view kUnavailable = "the pinned FidelityFX DX11 source build is not integrated yet";
	}

	FidelityFxD3D11Backend::~FidelityFxD3D11Backend()
	{
		Shutdown();
	}

	std::string_view FidelityFxD3D11Backend::Name() const noexcept
	{
		return "ffx";
	}

	Caps FidelityFxD3D11Backend::Probe(ID3D11Device*) const noexcept
	{
		Caps caps{};
		caps.available = false;

		// Permanently false for this backend, not pending a measurement. See the header.
		caps.supportsSubrects = false;

		caps.needsDeviceCreation = false;
		SetText(caps.unavailableReason, kUnavailable);
		return caps;
	}

	bool FidelityFxD3D11Backend::Initialize(ID3D11Device*, ID3D11DeviceContext*, const std::filesystem::path&) noexcept
	{
		spdlog::warn("ffx backend: {}", kUnavailable);
		return false;
	}

	bool FidelityFxD3D11Backend::CreateViews(const Config&, std::uint32_t) noexcept
	{
		return false;
	}

	std::pair<std::uint32_t, std::uint32_t> FidelityFxD3D11Backend::QueryRenderSize(
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight,
		QualityMode) const noexcept
	{
		// Neutral, not zero - a zero here would be a divide-by-zero waiting for the one
		// caller that forgets to check availability first.
		return { a_outputWidth, a_outputHeight };
	}

	float FidelityFxD3D11Backend::RecommendedMipBias(std::uint32_t, std::uint32_t) const noexcept
	{
		return 0.0f;
	}

	std::uint32_t FidelityFxD3D11Backend::JitterPhaseCount(std::uint32_t, std::uint32_t) const noexcept
	{
		return 1;
	}

	void FidelityFxD3D11Backend::JitterOffset(std::uint32_t, std::uint32_t, float& a_outX, float& a_outY) const noexcept
	{
		a_outX = 0.0f;
		a_outY = 0.0f;
	}

	bool FidelityFxD3D11Backend::Evaluate(ID3D11DeviceContext*, const EyeView&) noexcept
	{
		return false;
	}

	bool FidelityFxD3D11Backend::DestroyViews() noexcept
	{
		return true;
	}

	bool FidelityFxD3D11Backend::Shutdown() noexcept
	{
		return true;
	}
}

#else

#	include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#	include <FidelityFX/host/ffx_fsr3upscaler.h>

#	include <Windows.h>

#	include <algorithm>
#	include <chrono>
#	include <cmath>
#	include <cstdlib>
#	include <limits>
#	include <new>
#	include <thread>
#	include <vector>
#	include <wrl/client.h>

// The pinned DX11 fork's header declares this parameter pointer-to-const, but
// ffx_dx11.cpp exports pointer-to-non-const.  On MSVC those are different C++
// symbols.  Keep the compatibility declaration local to this translation unit;
// FindFfxSdk.cmake pins the only source revision for which it is valid.
FFX_API FfxResource ffxGetResourceDX11_Fsr31(
	ID3D11Resource* a_resource,
	FfxResourceDescription a_description,
	wchar_t const* a_name,
	FfxResourceStates a_state);

namespace Upscaler::Backend
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		constexpr std::string_view kNeedsInitialization =
			"the pinned FidelityFX DX11 source is compiled but has not initialized on a matching D3D11 device";
		constexpr std::string_view kRuntimeVersion = "FSR 3.1.5 DX11 source e65b253";

		struct View
		{
			FfxFsr3UpscalerContext context{};
			bool created = false;
		};

		struct State
		{
			ComPtr<ID3D11Device> device;
			ComPtr<ID3D11DeviceContext> immediate;
			ComPtr<ID3D11Query> idleQuery;
			void* scratch = nullptr;
			std::size_t scratchSize = 0;
			FfxInterface backendInterface{};
			std::vector<View> views;
			Config config;
			Caps caps;
			bool hasConfig = false;
		};

		[[nodiscard]] State* GetState(void* a_state) noexcept
		{
			return static_cast<State*>(a_state);
		}

		[[nodiscard]] const State* GetState(const void* a_state) noexcept
		{
			return static_cast<const State*>(a_state);
		}

		void SetUnavailable(State& a_state, std::string_view a_reason) noexcept
		{
			a_state.caps.available = false;
			SetText(a_state.caps.unavailableReason, a_reason);
		}

		[[nodiscard]] bool ContextBelongsTo(ID3D11DeviceContext* a_context, ID3D11Device* a_device) noexcept
		{
			if (!a_context || !a_device) {
				return false;
			}
			ID3D11Device* owner = nullptr;
			a_context->GetDevice(&owner);
			const bool same = owner == a_device;
			if (owner) {
				owner->Release();
			}
			return same;
		}

		[[nodiscard]] bool ValidQuality(QualityMode a_quality) noexcept
		{
			switch (a_quality) {
			case QualityMode::kNativeAA:
			case QualityMode::kQuality:
			case QualityMode::kBalanced:
			case QualityMode::kPerformance:
			case QualityMode::kUltraPerformance:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] FfxFsr3UpscalerQualityMode Quality(QualityMode a_quality) noexcept
		{
			return static_cast<FfxFsr3UpscalerQualityMode>(static_cast<int>(a_quality));
		}

		[[nodiscard]] bool SameConfig(const Config& a_lhs, const Config& a_rhs) noexcept
		{
			return a_lhs.renderWidth == a_rhs.renderWidth &&
			       a_lhs.renderHeight == a_rhs.renderHeight &&
			       a_lhs.outputWidth == a_rhs.outputWidth &&
			       a_lhs.outputHeight == a_rhs.outputHeight &&
			       a_lhs.quality == a_rhs.quality &&
			       a_lhs.preset == a_rhs.preset &&
			       a_lhs.sharpness == a_rhs.sharpness &&
			       a_lhs.hdr == a_rhs.hdr &&
			       a_lhs.invertedDepth == a_rhs.invertedDepth &&
			       a_lhs.jitteredMV == a_rhs.jitteredMV &&
			       a_lhs.lowResMV == a_rhs.lowResMV &&
			       a_lhs.autoExposure == a_rhs.autoExposure &&
			       a_lhs.preExposure == a_rhs.preExposure &&
			       a_lhs.exposureScale == a_rhs.exposureScale &&
			       a_lhs.enableSubrects == a_rhs.enableSubrects;
		}

		[[nodiscard]] bool Describe(ID3D11Resource* a_resource, D3D11_TEXTURE2D_DESC& a_out) noexcept
		{
			if (!a_resource) {
				return false;
			}
			ID3D11Texture2D* texture = nullptr;
			const HRESULT hr = a_resource->QueryInterface(
				__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
			if (FAILED(hr) || !texture) {
				return false;
			}
			texture->GetDesc(&a_out);
			texture->Release();
			return true;
		}

		[[nodiscard]] bool IsPlainTexture(const D3D11_TEXTURE2D_DESC& a_desc) noexcept
		{
			return a_desc.MipLevels == 1 && a_desc.ArraySize == 1 &&
			       a_desc.SampleDesc.Count == 1 && a_desc.Usage == D3D11_USAGE_DEFAULT &&
			       a_desc.CPUAccessFlags == 0 && a_desc.MiscFlags == 0;
		}

		[[nodiscard]] bool ExactRect(const Rect& a_rect, std::uint32_t a_width, std::uint32_t a_height) noexcept
		{
			return a_rect.x == 0 && a_rect.y == 0 && a_rect.w == a_width && a_rect.h == a_height;
		}

		void LogFailure(std::string_view a_operation, FfxErrorCode a_result) noexcept
		{
			spdlog::warn("ffx backend: {} failed with FfxErrorCode {:08X}",
				a_operation,
				static_cast<std::uint32_t>(a_result));
		}

		[[nodiscard]] FfxErrorCode ProtectedCreate(
			FfxFsr3UpscalerContext* a_context,
			FfxFsr3UpscalerContextDescription* a_description,
			bool& a_fault) noexcept
		{
			a_fault = false;
			FfxErrorCode result = FFX_ERROR_BACKEND_API_ERROR;
			__try {
				result = ffxFsr3UpscalerContextCreate(a_context, a_description);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_fault = true;
			}
			return result;
		}

		[[nodiscard]] FfxErrorCode ProtectedDispatch(
			FfxFsr3UpscalerContext* a_context,
			const FfxFsr3UpscalerDispatchDescription* a_description,
			bool& a_fault) noexcept
		{
			a_fault = false;
			FfxErrorCode result = FFX_ERROR_BACKEND_API_ERROR;
			__try {
				result = ffxFsr3UpscalerContextDispatch(a_context, a_description);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_fault = true;
			}
			return result;
		}

		[[nodiscard]] FfxErrorCode ProtectedDestroy(FfxFsr3UpscalerContext* a_context, bool& a_fault) noexcept
		{
			a_fault = false;
			FfxErrorCode result = FFX_ERROR_BACKEND_API_ERROR;
			__try {
				result = ffxFsr3UpscalerContextDestroy(a_context);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_fault = true;
			}
			return result;
		}

		[[nodiscard]] bool WaitForGpuIdle(State& a_state) noexcept
		{
			if (a_state.views.empty()) {
				return true;
			}
			if (!a_state.immediate || !a_state.idleQuery) {
				return false;
			}
			a_state.immediate->End(a_state.idleQuery.Get());
			a_state.immediate->Flush();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (std::chrono::steady_clock::now() < deadline) {
				const HRESULT status = a_state.immediate->GetData(a_state.idleQuery.Get(), nullptr, 0, 0);
				if (status == S_OK) {
					return true;
				}
				if (FAILED(status)) {
					return false;
				}
				std::this_thread::yield();
			}
			return false;
		}

		[[nodiscard]] bool ReleaseViews(State& a_state) noexcept
		{
			if (a_state.views.empty()) {
				a_state.backendInterface = {};
				std::free(a_state.scratch);
				a_state.scratch = nullptr;
				a_state.scratchSize = 0;
				a_state.config = {};
				a_state.hasConfig = false;
				return true;
			}
			if (!WaitForGpuIdle(a_state)) {
				spdlog::warn("ffx backend: GPU idle wait timed out; retaining contexts and scratch memory");
				return false;
			}

			bool allDestroyed = true;
			for (View& view : a_state.views) {
				if (!view.created) {
					continue;
				}
				bool fault = false;
				const FfxErrorCode result = ProtectedDestroy(&view.context, fault);
				if (fault || result != FFX_OK) {
					allDestroyed = false;
					if (!fault) {
						LogFailure("ffxFsr3UpscalerContextDestroy", result);
					}
				} else {
					view.created = false;
				}
			}
			if (!allDestroyed) {
				SetUnavailable(a_state, "a FidelityFX context faulted or failed during destruction");
				return false;
			}

			a_state.views.clear();
			a_state.backendInterface = {};
			std::free(a_state.scratch);
			a_state.scratch = nullptr;
			a_state.scratchSize = 0;
			a_state.config = {};
			a_state.hasConfig = false;
			return true;
		}

		[[nodiscard]] FfxResource Wrap(
			ID3D11Resource* a_resource,
			const wchar_t* a_name,
			FfxResourceStates a_state) noexcept
		{
			if (!a_resource) {
				return {};
			}
			return ffxGetResourceDX11_Fsr31(
				a_resource,
				GetFfxResourceDescriptionDX11(a_resource),
				a_name,
				a_state);
		}
	}

	FidelityFxD3D11Backend::~FidelityFxD3D11Backend()
	{
		Shutdown();
	}

	std::string_view FidelityFxD3D11Backend::Name() const noexcept
	{
		return "ffx";
	}

	Caps FidelityFxD3D11Backend::Probe(ID3D11Device* a_device) const noexcept
	{
		// The asynchronous self-check has no render admission and therefore must not
		// dereference mutable backend State while Initialize/Shutdown can replace it.
		// A real device probe is issued only by the admitted render/configuration path.
		if (!a_device) {
			Caps caps{};
			caps.supportsSubrects = false;
			caps.needsDeviceCreation = false;
			caps.ownsSharpening = true;
			SetText(caps.runtimeVersion, kRuntimeVersion);
			SetText(caps.unavailableReason, "a device-bound probe is required; asynchronous reports do not inspect live FidelityFX state");
			return caps;
		}
		const State* state = GetState(_state);
		if (!state) {
			Caps caps{};
			caps.supportsSubrects = false;
			caps.needsDeviceCreation = false;
			caps.ownsSharpening = true;
			SetText(caps.runtimeVersion, kRuntimeVersion);
			SetText(caps.unavailableReason, kNeedsInitialization);
			return caps;
		}
		Caps caps = state->caps;
		if (caps.available && state->device.Get() != a_device) {
			caps.available = false;
			SetText(caps.unavailableReason, "FidelityFX was initialized for a different D3D11 device");
		}
		return caps;
	}

	bool FidelityFxD3D11Backend::Initialize(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		const std::filesystem::path&) noexcept
	{
		Shutdown();
		if (_state) {
			spdlog::warn("ffx backend: Initialize declined because prior live state could not be released safely");
			return false;
		}
		_state = new (std::nothrow) State{};
		State* state = GetState(_state);
		if (!state) {
			spdlog::warn("ffx backend: state allocation failed");
			return false;
		}

		state->caps.supportsSubrects = false;
		state->caps.needsDeviceCreation = false;
		state->caps.supportsExposure = false;
		state->caps.supportsReactiveMask = false;
		state->caps.ownsSharpening = true;
		SetText(state->caps.runtimeVersion, kRuntimeVersion);

		if (!a_device || !a_immediate || !ContextBelongsTo(a_immediate, a_device) ||
			a_device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
			SetUnavailable(*state, "Initialize requires a matching feature-level-11 D3D11 device and immediate context");
			return false;
		}

		D3D11_QUERY_DESC query{};
		query.Query = D3D11_QUERY_EVENT;
		const HRESULT queryResult = a_device->CreateQuery(&query, state->idleQuery.GetAddressOf());
		if (FAILED(queryResult) || !state->idleQuery) {
			SetUnavailable(*state, "the D3D11 event query required for safe context teardown could not be created");
			return false;
		}

		state->device = a_device;
		state->immediate = a_immediate;
		state->caps.available = true;
		state->caps.unavailableReason[0] = '\0';
		spdlog::info("ffx backend: initialized pinned {} on the supplied D3D11 device", kRuntimeVersion);
		return true;
	}

	bool FidelityFxD3D11Backend::CreateViews(const Config& a_config, std::uint32_t a_viewCount) noexcept
	{
		State* state = GetState(_state);
		if (!state || !state->caps.available || !state->device || !state->immediate) {
			spdlog::warn("ffx backend: CreateViews declined before successful Initialize");
			return false;
		}
		if (a_viewCount == 0 || a_viewCount > 4 ||
			a_config.renderWidth == 0 || a_config.renderHeight == 0 ||
			a_config.outputWidth == 0 || a_config.outputHeight == 0 ||
			a_config.renderWidth > a_config.outputWidth || a_config.renderHeight > a_config.outputHeight ||
			!a_config.hdr || !ValidQuality(a_config.quality) ||
			!std::isfinite(a_config.sharpness) || a_config.sharpness < 0.0f || a_config.sharpness > 1.0f ||
			!IsBackendAutoExposureContract(a_config, nullptr) ||
			(!a_config.preset.empty() && a_config.preset != "auto") ||
			a_config.enableSubrects) {
			// Evaluate below takes zero-origin exact rects only, so a caller asking for
			// sub-rect bases is told no here rather than at the first frame.
			spdlog::warn("ffx backend: CreateViews declined an unsupported config or view count");
			return false;
		}

		try {
			if (state->hasConfig && state->views.size() == a_viewCount && SameConfig(state->config, a_config)) {
				return true;
			}
			if (!ReleaseViews(*state)) {
				return false;
			}

			state->scratchSize = ffxGetScratchMemorySizeDX11(a_viewCount);
			state->scratch = std::calloc(1, state->scratchSize);
			if (!state->scratch) {
				SetUnavailable(*state, "FidelityFX backend scratch allocation failed");
				return false;
			}
			const FfxErrorCode interfaceResult = ffxGetInterfaceDX11(
				&state->backendInterface,
				ffxGetDeviceDX11_Fsr31(state->device.Get()),
				state->scratch,
				state->scratchSize,
				a_viewCount);
			if (interfaceResult != FFX_OK) {
				LogFailure("ffxGetInterfaceDX11", interfaceResult);
				SetUnavailable(*state, "the FidelityFX D3D11 backend interface rejected its scratch buffer or device");
				std::free(state->scratch);
				state->scratch = nullptr;
				state->scratchSize = 0;
				return false;
			}

			state->views.resize(a_viewCount);
			for (std::uint32_t index = 0; index < a_viewCount; ++index) {
				FfxFsr3UpscalerContextDescription description{};
				description.maxRenderSize = { a_config.renderWidth, a_config.renderHeight };
				description.maxUpscaleSize = { a_config.outputWidth, a_config.outputHeight };
				description.flags = 0;
				if (a_config.hdr) {
					description.flags |= FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
				}
				if (!a_config.lowResMV) {
					description.flags |= FFX_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
				}
				if (a_config.jitteredMV) {
					description.flags |= FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
				}
				if (a_config.invertedDepth) {
					description.flags |= FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
				}
				if (a_config.autoExposure) {
					description.flags |= FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
				}
				description.backendInterface = state->backendInterface;

				bool fault = false;
				const FfxErrorCode createResult = ProtectedCreate(
					&state->views[index].context,
					&description,
					fault);
				if (fault || createResult != FFX_OK) {
					if (!fault) {
						LogFailure("ffxFsr3UpscalerContextCreate", createResult);
					}
					SetUnavailable(*state, fault ?
											   "a structured exception occurred while creating a FidelityFX context" :
											   "FidelityFX rejected a per-eye context description");
					static_cast<void>(ReleaseViews(*state));
					return false;
				}
				state->views[index].created = true;
			}

			state->config = a_config;
			state->hasConfig = true;
			spdlog::info("ffx backend: created {} independent FSR 3.1.5 view(s), {}x{} in to {}x{} out",
				a_viewCount,
				a_config.renderWidth,
				a_config.renderHeight,
				a_config.outputWidth,
				a_config.outputHeight);
			return true;
		} catch (...) {
			SetUnavailable(*state, "a C++ exception occurred while creating FidelityFX views");
			static_cast<void>(ReleaseViews(*state));
			return false;
		}
	}

	std::pair<std::uint32_t, std::uint32_t> FidelityFxD3D11Backend::QueryRenderSize(
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight,
		QualityMode a_quality) const noexcept
	{
		if (a_outputWidth == 0 || a_outputHeight == 0 || !ValidQuality(a_quality)) {
			return { a_outputWidth, a_outputHeight };
		}
		std::uint32_t width = a_outputWidth;
		std::uint32_t height = a_outputHeight;
		const FfxErrorCode result = ffxFsr3UpscalerGetRenderResolutionFromQualityMode(
			&width,
			&height,
			a_outputWidth,
			a_outputHeight,
			Quality(a_quality));
		if (result != FFX_OK || width == 0 || height == 0) {
			return { a_outputWidth, a_outputHeight };
		}
		return { width, height };
	}

	float FidelityFxD3D11Backend::RecommendedMipBias(
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth) const noexcept
	{
		if (a_renderWidth == 0 || a_outputWidth == 0) {
			return 0.0f;
		}
		return static_cast<float>(std::log2(static_cast<double>(a_renderWidth) / a_outputWidth) - 1.0);
	}

	std::uint32_t FidelityFxD3D11Backend::JitterPhaseCount(
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth) const noexcept
	{
		if (a_renderWidth == 0 || a_outputWidth == 0 ||
			a_renderWidth > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) ||
			a_outputWidth > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
			return 1;
		}
		const std::int32_t phases = ffxFsr3UpscalerGetJitterPhaseCount(
			static_cast<std::int32_t>(a_renderWidth),
			static_cast<std::int32_t>(a_outputWidth));
		return phases > 0 ? static_cast<std::uint32_t>(phases) : 1u;
	}

	void FidelityFxD3D11Backend::JitterOffset(
		std::uint32_t a_frameIndex,
		std::uint32_t a_phaseCount,
		float& a_outX,
		float& a_outY) const noexcept
	{
		a_outX = 0.0f;
		a_outY = 0.0f;
		if (a_frameIndex > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) ||
			a_phaseCount == 0 ||
			a_phaseCount > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
			return;
		}
		float ffxX = 0.0f;
		float ffxY = 0.0f;
		if (ffxFsr3UpscalerGetJitterOffset(
				&ffxX,
				&ffxY,
				static_cast<std::int32_t>(a_frameIndex),
				static_cast<std::int32_t>(a_phaseCount)) == FFX_OK) {
			// The engine-facing canonical sign converts back into the projection
			// translation prescribed by the FSR documentation.
			a_outX = -ffxX;
			a_outY = -ffxY;
		}
	}

	bool FidelityFxD3D11Backend::Evaluate(
		ID3D11DeviceContext* a_immediate,
		const EyeView& a_view) noexcept
	{
		State* state = GetState(_state);
		if (!state || !state->caps.available || !state->hasConfig || !a_immediate ||
			a_immediate != state->immediate.Get() || a_view.index >= state->views.size() ||
			!state->views[a_view.index].created || !ContextBelongsTo(a_immediate, state->device.Get())) {
			spdlog::warn("ffx backend: Evaluate declined before matching Initialize and CreateViews");
			return false;
		}

		const Config& config = state->config;
		if (!a_view.color || !a_view.depth || !a_view.motionVectors || !a_view.output ||
			a_view.reactiveMask || a_view.transparencyMask ||
			!IsBackendAutoExposureContract(config, a_view.exposure) ||
			!ExactRect(a_view.colorRect, config.renderWidth, config.renderHeight) ||
			!ExactRect(a_view.depthRect, config.renderWidth, config.renderHeight) ||
			!ExactRect(a_view.mvRect, config.renderWidth, config.renderHeight) ||
			!ExactRect(a_view.outputRect, config.outputWidth, config.outputHeight) ||
			a_view.mvScaleX != static_cast<float>(config.renderWidth) ||
			a_view.mvScaleY != static_cast<float>(config.renderHeight) ||
			!std::isfinite(a_view.jitterX) || !std::isfinite(a_view.jitterY) ||
			!std::isfinite(a_view.frameTimeDeltaMs) || a_view.frameTimeDeltaMs < 0.0f ||
			!std::isfinite(a_view.cameraNear) || !std::isfinite(a_view.cameraFar) ||
			!std::isfinite(a_view.cameraFovAngleVertical) || !std::isfinite(a_view.viewSpaceToMetersFactor) ||
			!(a_view.cameraNear > 0.0f) || !(a_view.cameraFar > a_view.cameraNear) ||
			!(a_view.cameraFovAngleVertical > 0.0f) ||
			!(a_view.cameraFovAngleVertical < 3.14159265358979323846f) ||
			!(a_view.viewSpaceToMetersFactor > 0.0f)) {
			spdlog::warn("ffx backend: Evaluate declined an incomplete or inconsistent per-eye contract");
			return false;
		}

		D3D11_TEXTURE2D_DESC color{};
		D3D11_TEXTURE2D_DESC depth{};
		D3D11_TEXTURE2D_DESC motion{};
		D3D11_TEXTURE2D_DESC output{};
		if (!Describe(a_view.color, color) || !Describe(a_view.depth, depth) ||
			!Describe(a_view.motionVectors, motion) || !Describe(a_view.output, output) ||
			!IsPlainTexture(color) || !IsPlainTexture(depth) ||
			!IsPlainTexture(motion) || !IsPlainTexture(output) ||
			color.Width != config.renderWidth || color.Height != config.renderHeight ||
			depth.Width != config.renderWidth || depth.Height != config.renderHeight ||
			motion.Width != config.renderWidth || motion.Height != config.renderHeight ||
			output.Width != config.outputWidth || output.Height != config.outputHeight ||
			color.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
			depth.Format != DXGI_FORMAT_R32_FLOAT ||
			motion.Format != DXGI_FORMAT_R16G16_FLOAT ||
			output.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
			(color.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
			(depth.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
			(motion.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
			(output.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0 ||
			a_view.color == a_view.output || a_view.depth == a_view.output ||
			a_view.motionVectors == a_view.output) {
			spdlog::warn("ffx backend: Evaluate declined incompatible D3D11 resource descriptors");
			return false;
		}

		FfxFsr3UpscalerDispatchDescription dispatch{};
		dispatch.commandList = ffxGetCommandListDX11(a_immediate);
		dispatch.color = Wrap(a_view.color, L"Fo4VR_FSR_Color", FFX_RESOURCE_STATE_COMPUTE_READ);
		dispatch.depth = Wrap(a_view.depth, L"Fo4VR_FSR_Depth", FFX_RESOURCE_STATE_COMPUTE_READ);
		dispatch.motionVectors = Wrap(a_view.motionVectors, L"Fo4VR_FSR_Motion", FFX_RESOURCE_STATE_COMPUTE_READ);
		dispatch.exposure = {};
		dispatch.reactive = {};
		dispatch.transparencyAndComposition = {};
		dispatch.output = Wrap(a_view.output, L"Fo4VR_FSR_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
		// FO4VR's projection translation and the FSR documented jitter convention
		// have opposite signs on both axes; the neutral seam carries the actual
		// engine-facing camera offset.
		dispatch.jitterOffset = { -a_view.jitterX, -a_view.jitterY };
		dispatch.motionVectorScale = { a_view.mvScaleX, a_view.mvScaleY };
		dispatch.renderSize = { config.renderWidth, config.renderHeight };
		dispatch.upscaleSize = { config.outputWidth, config.outputHeight };
		dispatch.enableSharpening = config.sharpness > 0.0f;
		dispatch.sharpness = config.sharpness;
		dispatch.frameTimeDelta = a_view.frameTimeDeltaMs;
		dispatch.preExposure = config.preExposure;
		dispatch.reset = a_view.resetHistory;
		dispatch.cameraNear = a_view.cameraNear;
		dispatch.cameraFar = a_view.cameraFar;
		dispatch.cameraFovAngleVertical = a_view.cameraFovAngleVertical;
		dispatch.viewSpaceToMetersFactor = a_view.viewSpaceToMetersFactor;
		dispatch.flags = 0;

		bool fault = false;
		const FfxErrorCode result = ProtectedDispatch(
			&state->views[a_view.index].context,
			&dispatch,
			fault);
		if (fault || result != FFX_OK) {
			if (!fault) {
				LogFailure("ffxFsr3UpscalerContextDispatch", result);
			} else {
				SetUnavailable(*state, "a structured exception occurred during FidelityFX dispatch");
				spdlog::warn("ffx backend: structured exception quarantined the backend");
			}
			return false;
		}
		return true;
	}

	bool FidelityFxD3D11Backend::DestroyViews() noexcept
	{
		State* state = GetState(_state);
		return !state || ReleaseViews(*state);
	}

	bool FidelityFxD3D11Backend::Shutdown() noexcept
	{
		State* state = GetState(_state);
		if (!state) {
			return true;
		}
		if (!ReleaseViews(*state)) {
			spdlog::warn("ffx backend: shutdown retained unsafe live state for a later retry");
			return false;
		}
		delete state;
		_state = nullptr;
		spdlog::info("ffx backend: shut down");
		return true;
	}
}

#endif
