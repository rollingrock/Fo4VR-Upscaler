#include "Upscaler/OpenXrPanel.h"

#include <Windows.h>
#include <filesystem>
#include <string>

namespace Upscaler::OpenXrPanel
{
	namespace
	{
		[[nodiscard]] std::wstring Environment(const wchar_t* a_name)
		{
			const auto length = ::GetEnvironmentVariableW(a_name, nullptr, 0);
			if (!length) {
				return {};
			}
			std::wstring value(length, L'\0');
			const auto written = ::GetEnvironmentVariableW(a_name, value.data(), length);
			if (!written || written >= length) {
				return {};
			}
			value.resize(written);
			return value;
		}
	}

	void Bootstrap() noexcept
	{
		try {
			HMODULE module = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Bootstrap), &module)) {
				return;
			}
			wchar_t path[32768]{};
			const auto length = ::GetModuleFileNameW(module, path, 32768);
			if (!length || length >= 32768) {
				return;
			}
			const auto folder = std::filesystem::path(path).parent_path() / L"Fo4VRUpscaler" / L"OpenXR";
			if (!std::filesystem::is_regular_file(folder / L"Fo4VRUpscalerOpenXR.json") ||
				!std::filesystem::is_regular_file(folder / OpenXrPanelBridge::kModuleName)) {
				logger::info("panel: optional OpenXR layer files absent; OpenVR menu path retained"sv);
				return;
			}
			const auto oldPaths = Environment(L"XR_API_LAYER_PATH");
			const auto oldLayers = Environment(L"XR_ENABLE_API_LAYERS");
			const auto paths = folder.wstring() + (oldPaths.empty() ? L"" : L";" + oldPaths);
			const auto layers = std::wstring(OpenXrPanelBridge::kLayerName) + (oldLayers.empty() ? L"" : L";" + oldLayers);
			if (!::SetEnvironmentVariableW(L"XR_API_LAYER_PATH", paths.c_str())) {
				logger::warn("panel: could not set the process OpenXR layer path"sv);
				return;
			}
			if (!::SetEnvironmentVariableW(L"XR_ENABLE_API_LAYERS", layers.c_str())) {
				::SetEnvironmentVariableW(L"XR_API_LAYER_PATH", oldPaths.empty() ? nullptr : oldPaths.c_str());
				logger::warn("panel: could not enable the process OpenXR menu layer"sv);
				return;
			}
			logger::info("panel: OpenXR quad/input layer enabled for this process; existing layers retained"sv);
		} catch (...) {
			logger::warn("panel: OpenXR layer setup unavailable; OpenVR menu path retained"sv);
		}
	}

	const OpenXrPanelBridge::Api* Resolve() noexcept
	{
		const auto module = ::GetModuleHandleW(OpenXrPanelBridge::kModuleName);
		if (!module) {
			return nullptr;
		}
		const auto get = reinterpret_cast<OpenXrPanelBridge::GetApi>(::GetProcAddress(module, "Fo4VRUpscaler_OpenXRPanelApi"));
		const auto* api = get ? get(OpenXrPanelBridge::kVersion, sizeof(OpenXrPanelBridge::Api)) : nullptr;
		return api && api->version == OpenXrPanelBridge::kVersion && api->size >= sizeof(OpenXrPanelBridge::Api) ? api : nullptr;
	}
}
