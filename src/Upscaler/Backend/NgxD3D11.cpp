#include "Upscaler/Backend/NgxD3D11.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>

// UPSCALER_NO_NGX is defined whenever the optional SDK is disabled or its headers and
// x64 library are not both staged. The stub keeps the normal open-source build free of
// vendor build inputs.
#ifdef UPSCALER_NO_NGX

namespace Upscaler::Backend
{
	namespace
	{
		constexpr std::string_view kUnavailable = "no complete NGX SDK staged - enable UPSCALER_ENABLE_NGX after scripts/fetch-sdks.ps1 stages the official archive under .sdk-cache/streamline";
	}

	NgxD3D11Backend::~NgxD3D11Backend()
	{
		Shutdown();
	}

	std::string_view NgxD3D11Backend::Name() const noexcept
	{
		return "ngx";
	}

	Caps NgxD3D11Backend::Probe(ID3D11Device*) const noexcept
	{
		Caps caps{};
		caps.available = false;
		caps.supportsSubrects = false;
		caps.needsDeviceCreation = false;
		SetText(caps.unavailableReason, kUnavailable);
		return caps;
	}

	bool NgxD3D11Backend::Initialize(ID3D11Device*, ID3D11DeviceContext*, const std::filesystem::path&) noexcept
	{
		spdlog::warn("ngx backend: {}", kUnavailable);
		return false;
	}

	bool NgxD3D11Backend::CreateViews(const Config&, std::uint32_t) noexcept
	{
		return false;
	}

	std::pair<std::uint32_t, std::uint32_t> NgxD3D11Backend::QueryRenderSize(
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight,
		QualityMode) const noexcept
	{
		return { a_outputWidth, a_outputHeight };
	}

	float NgxD3D11Backend::RecommendedMipBias(std::uint32_t, std::uint32_t) const noexcept
	{
		return 0.0f;
	}

	std::uint32_t NgxD3D11Backend::JitterPhaseCount(std::uint32_t, std::uint32_t) const noexcept
	{
		return 1;
	}

	void NgxD3D11Backend::JitterOffset(std::uint32_t, std::uint32_t, float& a_outX, float& a_outY) const noexcept
	{
		a_outX = 0.0f;
		a_outY = 0.0f;
	}

	bool NgxD3D11Backend::Evaluate(ID3D11DeviceContext*, const EyeView&) noexcept
	{
		return false;
	}

	void NgxD3D11Backend::DestroyViews() noexcept
	{}

	void NgxD3D11Backend::Shutdown() noexcept
	{}
}

#else

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

namespace Upscaler::Backend
{
	namespace
	{
		constexpr std::string_view kNeedsInitialization = "raw NGX is compiled but not initialized with a D3D11 device and explicit runtime directory; live selection remains locked";
		constexpr char kProjectId[] = "b7f4e0c2-9d13-4a5e-8c77-2f6a1e3d9b04";
		constexpr char kEngineVersion[] = "0.1.0";
		constexpr wchar_t kRuntimeName[] = L"nvngx_dlss.dll";
		constexpr std::uint32_t kMaxRuntimeLogLines = 24;
		std::atomic_uint32_t g_runtimeLogLines{ 0 };

		struct State
		{
			ID3D11Device* device = nullptr;
			ID3D11DeviceContext* immediate = nullptr;
			NVSDK_NGX_Parameter* parameters = nullptr;
			std::vector<NVSDK_NGX_Handle*> handles;
			Config config;
			std::filesystem::path expectedRuntimePath;
			bool sdkInitialized = false;
			bool hasConfig = false;
			Caps caps;
		};

		[[nodiscard]] State* GetState(void* a_state) noexcept
		{
			return static_cast<State*>(a_state);
		}

		[[nodiscard]] const State* GetState(const void* a_state) noexcept
		{
			return static_cast<const State*>(a_state);
		}

		[[nodiscard]] std::string_view ResultName(NVSDK_NGX_Result a_result) noexcept
		{
			switch (a_result) {
			case NVSDK_NGX_Result_Success:
				return "success";
			case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
				return "feature not supported";
			case NVSDK_NGX_Result_FAIL_PlatformError:
				return "platform error";
			case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:
				return "feature already exists";
			case NVSDK_NGX_Result_FAIL_FeatureNotFound:
				return "feature not found";
			case NVSDK_NGX_Result_FAIL_InvalidParameter:
				return "invalid parameter";
			case NVSDK_NGX_Result_FAIL_NotInitialized:
				return "not initialized";
			case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
				return "unsupported input format";
			case NVSDK_NGX_Result_FAIL_RWFlagMissing:
				return "output UAV flag missing";
			case NVSDK_NGX_Result_FAIL_MissingInput:
				return "missing input";
			case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
				return "unable to initialize feature";
			case NVSDK_NGX_Result_FAIL_OutOfDate:
				return "driver or feature library out of date";
			case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:
				return "out of GPU memory";
			case NVSDK_NGX_Result_FAIL_UnsupportedFormat:
				return "unsupported format";
			case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:
				return "application data path is not writable";
			case NVSDK_NGX_Result_FAIL_UnsupportedParameter:
				return "unsupported parameter";
			case NVSDK_NGX_Result_FAIL_Denied:
				return "feature denied";
			case NVSDK_NGX_Result_FAIL_NotImplemented:
				return "not implemented";
			default:
				return "unclassified result";
			}
		}

		void SetUnavailable(State& a_state, std::string_view a_reason) noexcept
		{
			a_state.caps.available = false;
			SetText(a_state.caps.unavailableReason, a_reason);
		}

		void LogFailure(std::string_view a_call, NVSDK_NGX_Result a_result) noexcept
		{
			spdlog::warn("ngx backend: {} failed - {} (0x{:08X})", a_call, ResultName(a_result), static_cast<std::uint32_t>(a_result));
		}

		void NVSDK_CONV NgxLogCallback(const char* a_message, NVSDK_NGX_Logging_Level a_level, NVSDK_NGX_Feature a_source)
		{
			try {
				const std::uint32_t line = g_runtimeLogLines.fetch_add(1, std::memory_order_relaxed);
				if (line > kMaxRuntimeLogLines) {
					return;
				}
				if (line == kMaxRuntimeLogLines) {
					spdlog::info("ngx runtime: further vendor diagnostics suppressed for this initialization");
					return;
				}
				const std::string_view message = a_message ? std::string_view{ a_message } : std::string_view{ "(null message)" };
				if (a_level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE) {
					spdlog::debug("ngx runtime [{}]: {}", static_cast<int>(a_source), message);
				} else {
					spdlog::info("ngx runtime [{}]: {}", static_cast<int>(a_source), message);
				}
			} catch (...) {
				// A vendor callback may arrive on any thread and must not throw across its ABI.
			}
		}

		[[nodiscard]] std::filesystem::path AbsoluteCanonical(
			const std::filesystem::path& a_path,
			std::error_code& a_ec)
		{
			a_ec.clear();
			auto absolute = std::filesystem::absolute(a_path, a_ec);
			if (a_ec) {
				return {};
			}
			auto canonical = std::filesystem::weakly_canonical(absolute, a_ec);
			return a_ec ? std::filesystem::path{} : canonical;
		}

		[[nodiscard]] std::string Utf8(const std::filesystem::path& a_path)
		{
			const std::wstring wide = a_path.wstring();
			if (wide.empty() || wide.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
				return {};
			}

			const int wideSize = static_cast<int>(wide.size());
			const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, nullptr, 0, nullptr, nullptr);
			if (size <= 0) {
				return {};
			}
			std::string out(static_cast<std::size_t>(size), '\0');
			if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, out.data(), size, nullptr, nullptr) != size) {
				return {};
			}
			return out;
		}

		[[nodiscard]] std::filesystem::path LoadedRuntimePath()
		{
			const HMODULE module = GetModuleHandleW(kRuntimeName);
			if (!module) {
				return {};
			}

			std::vector<wchar_t> buffer(32768, L'\0');
			const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0 || static_cast<std::size_t>(length) >= buffer.size()) {
				return {};
			}
			return std::filesystem::path{ std::wstring_view{ buffer.data(), length } };
		}

		[[nodiscard]] bool SameFile(const std::filesystem::path& a_lhs, const std::filesystem::path& a_rhs) noexcept
		{
			std::error_code ec;
			const bool equivalent = std::filesystem::equivalent(a_lhs, a_rhs, ec);
			if (!ec) {
				return equivalent;
			}
			return _wcsicmp(a_lhs.c_str(), a_rhs.c_str()) == 0;
		}

		[[nodiscard]] std::string RuntimeVersion(const std::filesystem::path& a_path)
		{
			DWORD ignored = 0;
			const DWORD size = GetFileVersionInfoSizeW(a_path.c_str(), &ignored);
			if (size == 0) {
				return {};
			}

			std::vector<std::uint8_t> data(size);
			if (!GetFileVersionInfoW(a_path.c_str(), 0, size, data.data())) {
				return {};
			}

			VS_FIXEDFILEINFO* info = nullptr;
			UINT infoSize = 0;
			if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || !info || infoSize < sizeof(VS_FIXEDFILEINFO)) {
				return {};
			}

			char version[64]{};
			const int length = std::snprintf(version, sizeof(version), "%u.%u.%u.%u",
				HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
				HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
			return length > 0 && static_cast<std::size_t>(length) < sizeof(version) ?
				std::string{ version, static_cast<std::size_t>(length) } : std::string{};
		}

		void ReleaseViews(State& a_state) noexcept
		{
			for (NVSDK_NGX_Handle* handle : a_state.handles) {
				if (handle) {
					const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_ReleaseFeature(handle);
					if (NVSDK_NGX_FAILED(result)) {
						LogFailure("NVSDK_NGX_D3D11_ReleaseFeature", result);
					}
				}
			}
			a_state.handles.clear();
			a_state.hasConfig = false;
		}

		void ReleaseSdk(State& a_state) noexcept
		{
			ReleaseViews(a_state);
			if (a_state.parameters) {
				const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_DestroyParameters(a_state.parameters);
				if (NVSDK_NGX_FAILED(result)) {
					LogFailure("NVSDK_NGX_D3D11_DestroyParameters", result);
				}
				a_state.parameters = nullptr;
			}
			if (a_state.sdkInitialized && a_state.device) {
				const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Shutdown1(a_state.device);
				if (NVSDK_NGX_FAILED(result)) {
					LogFailure("NVSDK_NGX_D3D11_Shutdown1", result);
				}
			}
			a_state.sdkInitialized = false;
			if (a_state.immediate) {
				a_state.immediate->Release();
				a_state.immediate = nullptr;
			}
			if (a_state.device) {
				a_state.device->Release();
				a_state.device = nullptr;
			}
		}

		[[nodiscard]] NVSDK_NGX_PerfQuality_Value Quality(QualityMode a_quality) noexcept
		{
			switch (a_quality) {
			case QualityMode::kNativeAA:
				return NVSDK_NGX_PerfQuality_Value_DLAA;
			case QualityMode::kQuality:
				return NVSDK_NGX_PerfQuality_Value_MaxQuality;
			case QualityMode::kBalanced:
				return NVSDK_NGX_PerfQuality_Value_Balanced;
			case QualityMode::kPerformance:
				return NVSDK_NGX_PerfQuality_Value_MaxPerf;
			case QualityMode::kUltraPerformance:
				return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
			default:
				return NVSDK_NGX_PerfQuality_Value_DLAA;
			}
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
			       a_lhs.exposureScale == a_rhs.exposureScale;
		}

		[[nodiscard]] bool Describe(ID3D11Resource* a_resource, D3D11_TEXTURE2D_DESC& a_out) noexcept
		{
			if (!a_resource) {
				return false;
			}
			ID3D11Texture2D* texture = nullptr;
			const HRESULT hr = a_resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
			if (FAILED(hr) || !texture) {
				return false;
			}
			texture->GetDesc(&a_out);
			texture->Release();
			return true;
		}

		[[nodiscard]] bool IsPlainTexture(const D3D11_TEXTURE2D_DESC& a_desc) noexcept
		{
			return a_desc.MipLevels == 1 && a_desc.ArraySize == 1 && a_desc.SampleDesc.Count == 1;
		}

		[[nodiscard]] bool ExactRect(const Rect& a_rect, std::uint32_t a_width, std::uint32_t a_height) noexcept
		{
			return a_rect.x == 0 && a_rect.y == 0 && a_rect.w == a_width && a_rect.h == a_height;
		}

		[[nodiscard]] bool RectFits(const Rect& a_rect, const D3D11_TEXTURE2D_DESC& a_desc) noexcept
		{
			return a_rect.w != 0 && a_rect.h != 0 &&
			       static_cast<std::uint64_t>(a_rect.x) + a_rect.w <= a_desc.Width &&
			       static_cast<std::uint64_t>(a_rect.y) + a_rect.h <= a_desc.Height;
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

		[[nodiscard]] double RadicalInverse(std::uint32_t a_index, std::uint32_t a_base) noexcept
		{
			double result = 0.0;
			double fraction = 1.0 / static_cast<double>(a_base);
			while (a_index != 0) {
				result += static_cast<double>(a_index % a_base) * fraction;
				a_index /= a_base;
				fraction /= static_cast<double>(a_base);
			}
			return result;
		}
	}

	NgxD3D11Backend::~NgxD3D11Backend()
	{
		Shutdown();
	}

	std::string_view NgxD3D11Backend::Name() const noexcept
	{
		return "ngx";
	}

	Caps NgxD3D11Backend::Probe(ID3D11Device* a_device) const noexcept
	{
		const State* state = GetState(_state);
		if (!state) {
			Caps caps{};
			caps.supportsSubrects = false;
			caps.needsDeviceCreation = false;
			caps.supportsExposure = true;
			SetText(caps.unavailableReason, kNeedsInitialization);
			return caps;
		}

		Caps caps = state->caps;
		if (caps.available && a_device && state->device != a_device) {
			caps.available = false;
			SetText(caps.unavailableReason, "NGX was initialized for a different D3D11 device");
		}
		return caps;
	}

	bool NgxD3D11Backend::Initialize(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		const std::filesystem::path& a_runtimeDir) noexcept
	{
		Shutdown();
		_state = new (std::nothrow) State{};
		State* state = GetState(_state);
		if (!state) {
			spdlog::warn("ngx backend: state allocation failed");
			return false;
		}

		state->caps.supportsSubrects = false;
		state->caps.needsDeviceCreation = false;
		state->caps.supportsExposure = true;
		state->caps.supportsReactiveMask = false;
		state->caps.ownsSharpening = false;

		try {
			if (!a_device || !a_immediate || !ContextBelongsTo(a_immediate, a_device)) {
				SetUnavailable(*state, "Initialize requires a matching D3D11 device and immediate context");
				return false;
			}
			if (a_runtimeDir.empty()) {
				SetUnavailable(*state, "Initialize requires an explicit runtime directory");
				return false;
			}

			std::error_code ec;
			const auto runtimeDir = AbsoluteCanonical(a_runtimeDir, ec);
			if (ec || runtimeDir.empty() || !std::filesystem::is_directory(runtimeDir, ec) || ec) {
				SetUnavailable(*state, "the explicit NGX runtime directory does not exist");
				return false;
			}

			state->expectedRuntimePath = AbsoluteCanonical(runtimeDir / kRuntimeName, ec);
			if (ec || state->expectedRuntimePath.empty() || !std::filesystem::is_regular_file(state->expectedRuntimePath, ec) || ec) {
				SetUnavailable(*state, "nvngx_dlss.dll is absent from the explicit runtime directory");
				return false;
			}

			const auto alreadyLoaded = LoadedRuntimePath();
			if (!alreadyLoaded.empty() && !SameFile(alreadyLoaded, state->expectedRuntimePath)) {
				SetUnavailable(*state, "a different nvngx_dlss.dll is already loaded in the process");
				return false;
			}

			const auto applicationData = runtimeDir / L"Fo4VRUpscalerNGX";
			std::filesystem::create_directories(applicationData, ec);
			if (ec || !std::filesystem::is_directory(applicationData, ec) || ec) {
				SetUnavailable(*state, "the NGX application data directory cannot be created");
				return false;
			}

			const std::wstring runtimeWide = runtimeDir.wstring();
			const wchar_t* searchPaths[] = { runtimeWide.c_str() };
			NVSDK_NGX_FeatureCommonInfo featureInfo{};
			featureInfo.PathListInfo.Path = searchPaths;
			featureInfo.PathListInfo.Length = 1;
			g_runtimeLogLines.store(0, std::memory_order_relaxed);
			featureInfo.LoggingInfo.LoggingCallback = &NgxLogCallback;
			featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
			featureInfo.LoggingInfo.DisableOtherLoggingSinks = true;

			const std::wstring applicationDataWide = applicationData.wstring();
			const NVSDK_NGX_Result init = NVSDK_NGX_D3D11_Init_with_ProjectID(
				kProjectId,
				NVSDK_NGX_ENGINE_TYPE_CUSTOM,
				kEngineVersion,
				applicationDataWide.c_str(),
				a_device,
				&featureInfo);
			if (NVSDK_NGX_FAILED(init)) {
				LogFailure("NVSDK_NGX_D3D11_Init_with_ProjectID", init);
				SetUnavailable(*state, ResultName(init));
				return false;
			}
			state->sdkInitialized = true;
			state->device = a_device;
			state->device->AddRef();
			state->immediate = a_immediate;
			state->immediate->AddRef();

			const NVSDK_NGX_Result parameters = NVSDK_NGX_D3D11_GetCapabilityParameters(&state->parameters);
			if (NVSDK_NGX_FAILED(parameters) || !state->parameters) {
				LogFailure("NVSDK_NGX_D3D11_GetCapabilityParameters", parameters);
				SetUnavailable(*state, ResultName(parameters));
				ReleaseSdk(*state);
				return false;
			}

			int available = 0;
			const NVSDK_NGX_Result availability = NVSDK_NGX_Parameter_GetI(
				state->parameters,
				NVSDK_NGX_Parameter_SuperSampling_Available,
				&available);
			if (NVSDK_NGX_FAILED(availability) || available == 0) {
				if (NVSDK_NGX_FAILED(availability)) {
					LogFailure("SuperSampling.Available", availability);
				}
				SetUnavailable(*state, available == 0 ? "the current adapter or driver reports DLSS unavailable" : ResultName(availability));
				ReleaseSdk(*state);
				return false;
			}

			state->caps.available = true;
			state->caps.unavailableReason[0] = '\0';
			spdlog::info("ngx backend: initialized on the supplied D3D11 device; feature DLL path will be verified when the first views are created");
			return true;
		} catch (...) {
			SetUnavailable(*state, "an exception occurred while initializing raw NGX");
			ReleaseSdk(*state);
			return false;
		}
	}

	bool NgxD3D11Backend::CreateViews(const Config& a_config, std::uint32_t a_viewCount) noexcept
	{
		State* state = GetState(_state);
		if (!state || !state->caps.available || !state->sdkInitialized || !state->parameters || !state->immediate) {
			spdlog::warn("ngx backend: CreateViews declined before successful Initialize");
			return false;
		}
		if (a_viewCount == 0 || a_viewCount > 4 || a_config.renderWidth == 0 || a_config.renderHeight == 0 ||
			a_config.outputWidth == 0 || a_config.outputHeight == 0 || !a_config.hdr ||
			!ValidQuality(a_config.quality) || a_config.sharpness != 0.0f ||
			!IsBackendAutoExposureContract(a_config, nullptr) ||
			(!a_config.preset.empty() && a_config.preset != "auto")) {
			spdlog::warn("ngx backend: CreateViews declined an unsupported config or view count");
			return false;
		}

		try {
			if (state->hasConfig && state->handles.size() == a_viewCount && SameConfig(state->config, a_config)) {
				return true;
			}

			ReleaseViews(*state);
			state->handles.reserve(a_viewCount);
			for (std::uint32_t index = 0; index < a_viewCount; ++index) {
				NVSDK_NGX_DLSS_Create_Params create{};
				create.Feature.InWidth = a_config.renderWidth;
				create.Feature.InHeight = a_config.renderHeight;
				create.Feature.InTargetWidth = a_config.outputWidth;
				create.Feature.InTargetHeight = a_config.outputHeight;
				create.Feature.InPerfQualityValue = Quality(a_config.quality);
				create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_None;
				if (a_config.hdr) {
					create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
				}
				if (a_config.lowResMV) {
					create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
				}
				if (a_config.jitteredMV) {
					create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
				}
				if (a_config.invertedDepth) {
					create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
				}
				if (a_config.autoExposure) {
					create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
				}
				create.InEnableOutputSubrects = false;

				NVSDK_NGX_Handle* handle = nullptr;
				const NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(state->immediate, &handle, state->parameters, &create);
				if (NVSDK_NGX_FAILED(result) || !handle || std::find(state->handles.begin(), state->handles.end(), handle) != state->handles.end()) {
					if (NVSDK_NGX_FAILED(result)) {
						LogFailure("NGX_D3D11_CREATE_DLSS_EXT", result);
					}
					ReleaseViews(*state);
					return false;
				}
				state->handles.push_back(handle);
			}

			const auto loadedRuntime = LoadedRuntimePath();
			if (loadedRuntime.empty() || !SameFile(loadedRuntime, state->expectedRuntimePath)) {
				SetUnavailable(*state, loadedRuntime.empty() ? "DLSS feature creation did not expose a loaded nvngx_dlss.dll" : "NGX loaded nvngx_dlss.dll from outside the explicit runtime directory");
				ReleaseViews(*state);
				return false;
			}

			std::error_code ec;
			const auto runtimePath = AbsoluteCanonical(loadedRuntime, ec);
			const auto& reportedPath = runtimePath.empty() ? loadedRuntime : runtimePath;
			SetText(state->caps.runtimePath, Utf8(reportedPath));
			SetText(state->caps.runtimeVersion, RuntimeVersion(reportedPath));
			state->config = a_config;
			state->hasConfig = true;
			spdlog::info("ngx backend: created {} independent DLSS view(s), {}x{} in to {}x{} out; runtime {} at {}",
				a_viewCount,
				a_config.renderWidth,
				a_config.renderHeight,
				a_config.outputWidth,
				a_config.outputHeight,
				Text(state->caps.runtimeVersion).empty() ? std::string_view{ "version unreported" } : Text(state->caps.runtimeVersion),
				Text(state->caps.runtimePath));
			return true;
		} catch (...) {
			ReleaseViews(*state);
			spdlog::warn("ngx backend: an exception occurred while creating views");
			return false;
		}
	}

	std::pair<std::uint32_t, std::uint32_t> NgxD3D11Backend::QueryRenderSize(
		std::uint32_t a_outputWidth,
		std::uint32_t a_outputHeight,
		QualityMode a_quality) const noexcept
	{
		const State* state = GetState(_state);
		if (!state || !state->parameters || a_outputWidth == 0 || a_outputHeight == 0 || !ValidQuality(a_quality)) {
			return { a_outputWidth, a_outputHeight };
		}

		unsigned int optimalWidth = a_outputWidth;
		unsigned int optimalHeight = a_outputHeight;
		unsigned int maxWidth = a_outputWidth;
		unsigned int maxHeight = a_outputHeight;
		unsigned int minWidth = a_outputWidth;
		unsigned int minHeight = a_outputHeight;
		float sharpness = 0.0f;
		const NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
			state->parameters,
			a_outputWidth,
			a_outputHeight,
			Quality(a_quality),
			&optimalWidth,
			&optimalHeight,
			&maxWidth,
			&maxHeight,
			&minWidth,
			&minHeight,
			&sharpness);
		if (NVSDK_NGX_FAILED(result) || optimalWidth == 0 || optimalHeight == 0) {
			LogFailure("NGX_DLSS_GET_OPTIMAL_SETTINGS", result);
			return { a_outputWidth, a_outputHeight };
		}
		return { optimalWidth, optimalHeight };
	}

	float NgxD3D11Backend::RecommendedMipBias(std::uint32_t a_renderWidth, std::uint32_t a_outputWidth) const noexcept
	{
		if (a_renderWidth == 0 || a_outputWidth == 0) {
			return 0.0f;
		}
		return static_cast<float>(std::log2(static_cast<double>(a_renderWidth) / a_outputWidth) - 1.0);
	}

	std::uint32_t NgxD3D11Backend::JitterPhaseCount(std::uint32_t a_renderWidth, std::uint32_t a_outputWidth) const noexcept
	{
		if (a_renderWidth == 0 || a_outputWidth == 0) {
			return 1;
		}
		const double ratio = (std::max)(static_cast<double>(a_outputWidth) / a_renderWidth, 1.0);
		const double phases = std::ceil(8.0 * ratio * ratio);
		return phases >= (std::numeric_limits<std::uint32_t>::max)() ?
			(std::numeric_limits<std::uint32_t>::max)() : static_cast<std::uint32_t>(phases);
	}

	void NgxD3D11Backend::JitterOffset(
		std::uint32_t a_frameIndex,
		std::uint32_t a_phaseCount,
		float& a_outX,
		float& a_outY) const noexcept
	{
		const std::uint32_t count = a_phaseCount == 0 ? 1 : a_phaseCount;
		const std::uint32_t sample = (a_frameIndex % count) + 1;
		a_outX = static_cast<float>(0.5 - RadicalInverse(sample, 2));
		a_outY = static_cast<float>(RadicalInverse(sample, 3) - 0.5);
	}

	bool NgxD3D11Backend::Evaluate(ID3D11DeviceContext* a_immediate, const EyeView& a_view) noexcept
	{
		State* state = GetState(_state);
		if (!state || !state->hasConfig || !a_immediate || a_immediate != state->immediate ||
			a_view.index >= state->handles.size() || !ContextBelongsTo(a_immediate, state->device)) {
			spdlog::warn("ngx backend: Evaluate declined before a matching Initialize and CreateViews");
			return false;
		}

		const Config& config = state->config;
		if (!a_view.color || !a_view.depth || !a_view.motionVectors || !a_view.output ||
			a_view.reactiveMask || a_view.transparencyMask ||
			!IsBackendAutoExposureContract(config, a_view.exposure) ||
			!ExactRect(a_view.colorRect, config.renderWidth, config.renderHeight) ||
			!ExactRect(a_view.mvRect, config.renderWidth, config.renderHeight) ||
			!ExactRect(a_view.outputRect, config.outputWidth, config.outputHeight) ||
			a_view.depthRect.w != config.renderWidth || a_view.depthRect.h != config.renderHeight ||
			a_view.mvScaleX != static_cast<float>(config.renderWidth) ||
			a_view.mvScaleY != static_cast<float>(config.renderHeight) ||
			!std::isfinite(a_view.jitterX) || !std::isfinite(a_view.jitterY) ||
			!std::isfinite(a_view.frameTimeDeltaMs) || a_view.frameTimeDeltaMs < 0.0f) {
			spdlog::warn("ngx backend: Evaluate declined an incomplete or inconsistent per-eye contract");
			return false;
		}

		D3D11_TEXTURE2D_DESC color{};
		D3D11_TEXTURE2D_DESC depth{};
		D3D11_TEXTURE2D_DESC motion{};
		D3D11_TEXTURE2D_DESC output{};
		if (!Describe(a_view.color, color) || !Describe(a_view.depth, depth) ||
			!Describe(a_view.motionVectors, motion) || !Describe(a_view.output, output) ||
			!IsPlainTexture(color) || !IsPlainTexture(depth) || !IsPlainTexture(motion) || !IsPlainTexture(output) ||
			color.Width != config.renderWidth || color.Height != config.renderHeight ||
			motion.Width != config.renderWidth || motion.Height != config.renderHeight ||
			output.Width != config.outputWidth || output.Height != config.outputHeight ||
			!RectFits(a_view.depthRect, depth) ||
			(color.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
			(depth.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
			(motion.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
			(output.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0) {
			spdlog::warn("ngx backend: Evaluate declined incompatible D3D11 resource descriptors");
			return false;
		}

		NVSDK_NGX_D3D11_DLSS_Eval_Params evaluate{};
		evaluate.Feature.pInColor = a_view.color;
		evaluate.Feature.pInOutput = a_view.output;
		evaluate.Feature.InSharpness = 0.0f;
		evaluate.pInDepth = a_view.depth;
		evaluate.pInMotionVectors = a_view.motionVectors;
		evaluate.InJitterOffsetX = a_view.jitterX;
		evaluate.InJitterOffsetY = a_view.jitterY;
		evaluate.InRenderSubrectDimensions.Width = config.renderWidth;
		evaluate.InRenderSubrectDimensions.Height = config.renderHeight;
		evaluate.InReset = a_view.resetHistory ? 1 : 0;
		evaluate.InMVScaleX = a_view.mvScaleX;
		evaluate.InMVScaleY = a_view.mvScaleY;
		evaluate.pInExposureTexture = nullptr;
		evaluate.InDepthSubrectBase.X = a_view.depthRect.x;
		evaluate.InDepthSubrectBase.Y = a_view.depthRect.y;
		evaluate.InPreExposure = config.preExposure;
		evaluate.InExposureScale = config.exposureScale;
		evaluate.InFrameTimeDeltaInMsec = a_view.frameTimeDeltaMs;

		const NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(
			a_immediate,
			state->handles[a_view.index],
			state->parameters,
			&evaluate);
		if (NVSDK_NGX_FAILED(result)) {
			LogFailure("NGX_D3D11_EVALUATE_DLSS_EXT", result);
			return false;
		}
		return true;
	}

	void NgxD3D11Backend::DestroyViews() noexcept
	{
		State* state = GetState(_state);
		if (state) {
			ReleaseViews(*state);
		}
	}

	void NgxD3D11Backend::Shutdown() noexcept
	{
		State* state = GetState(_state);
		if (!state) {
			return;
		}
		ReleaseSdk(*state);
		delete state;
		_state = nullptr;
		spdlog::info("ngx backend: shut down");
	}
}

#endif
