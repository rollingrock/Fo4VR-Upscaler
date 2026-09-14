#pragma once

#include <cstdint>

struct ID3D11Texture2D;

namespace Upscaler::OpenXrPanelBridge
{
	constexpr std::uint32_t kVersion = 1;
	constexpr const wchar_t* kModuleName = L"Fo4VRUpscalerOpenXR.dll";
	constexpr const wchar_t* kLayerName = L"XR_APILAYER_ROLLINGROCK_upscaler_panel";

	struct Status
	{
		std::uint32_t size = sizeof(Status);
		bool ready = false;
		bool visible = false;
		bool inputAttached = false;
		std::int32_t lastResult = 0;
		std::uint64_t generation = 0;
		std::uint64_t submissions = 0;
		std::uint64_t quadFrames = 0;
	};

	struct Input
	{
		std::uint32_t size = sizeof(Input);
		bool valid = false;
		bool hover = false;
		bool pressed = false;
		bool close = false;
		float x = 0.0f;
		float y = 0.0f;
		float scroll = 0.0f;
	};

	// The plugin calls these on its render thread. Busy session transitions decline
	// immediately. Texture submission waits for an XR image with a zero timeout.
	// The layer owns session resources; the plugin owns the submitted source texture.
	struct Api
	{
		std::uint32_t version;
		std::uint32_t size;
		bool (*status)(Status*) noexcept;
		bool (*show)(bool, float, float, bool) noexcept;
		bool (*input)(Input*) noexcept;
		bool (*submit)(ID3D11Texture2D*) noexcept;
	};
	using GetApi = const Api* (*)(std::uint32_t, std::uint32_t) noexcept;
}
