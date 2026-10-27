#pragma once

#include <cstdint>

struct ID3D11Texture2D;

namespace Upscaler::OpenXrPanelBridge
{
	constexpr std::uint32_t kVersion = 2;
	constexpr const wchar_t* kModuleName = L"Fo4VRUpscalerOpenXR.dll";
	constexpr const wchar_t* kLayerName = L"XR_APILAYER_ROLLINGROCK_upscaler_panel";

	// The controller profiles the layer can carry the panel's bindings on. It never suggests
	// a profile of its own: it adds its bindings to the game's own suggestion for any profile
	// in this table, so the profile the runtime picks is still the game's choice. Every hand
	// also binds /input/aim/pose. Component paths are VERIFIED against the OpenXR registry
	// (xr.xml, OpenXR-SDK-Source main a9f14c4, read 2026-10-27). Bit i of
	// Status::boundProfiles is row i.
	struct Profile
	{
		const char* path;
		const char* select;  // both hands
		const char* scroll;  // both hands; nullptr when the profile has no stick or pad
		const char* closeLeft;
		const char* closeRight;
		const char* closeLabel;  // what the panel header calls the close button
	};
	// clang-format off
	inline constexpr Profile kProfiles[] = {
		{ "/interaction_profiles/oculus/touch_controller",         "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/meta/touch_controller_plus",      "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/meta/touch_plus_controller",      "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/facebook/touch_controller_pro",   "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/meta/touch_pro_controller",       "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/valve/index_controller",          "/input/trigger/value", "/input/thumbstick", "/input/b/click",    "/input/b/click",    "B" },
		{ "/interaction_profiles/htc/vive_controller",             "/input/trigger/value", "/input/trackpad",   "/input/menu/click", "/input/menu/click", "Menu" },
		{ "/interaction_profiles/htc/vive_cosmos_controller",      "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/htc/vive_focus3_controller",      "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/microsoft/motion_controller",     "/input/trigger/value", "/input/thumbstick", "/input/menu/click", "/input/menu/click", "Menu" },
		{ "/interaction_profiles/hp/mixed_reality_controller",     "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/samsung/odyssey_controller",      "/input/trigger/value", "/input/thumbstick", "/input/menu/click", "/input/menu/click", "Menu" },
		{ "/interaction_profiles/bytedance/pico4_controller",      "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/bytedance/pico_neo3_controller",  "/input/trigger/value", "/input/thumbstick", "/input/y/click",    "/input/b/click",    "B/Y" },
		{ "/interaction_profiles/khr/simple_controller",           "/input/select/click",  nullptr,             "/input/menu/click", "/input/menu/click", "Menu" },
	};
	// clang-format on
	constexpr std::uint32_t kProfileCount = static_cast<std::uint32_t>(sizeof(kProfiles) / sizeof(kProfiles[0]));
	static_assert(kProfileCount <= 32, "Status::boundProfiles holds one bit per profile");

	// A visible panel whose controls have all read inactive for this long closes itself, so
	// a runtime that picked a profile without the panel's bindings cannot hold the game's
	// input. Measured in the runtime's predicted display time.
	constexpr std::int64_t kIdleReleaseNanoseconds = 3'000'000'000;

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
		// Bit i: the game suggested kProfiles[i] and the runtime accepted the panel's
		// bindings on it.
		std::uint32_t boundProfiles = 0;
		// The kProfiles row the last show found on a hand; -1 when no hand reported one.
		std::int32_t activeProfile = -1;
		// Both count up for the life of the layer, so a reader compares against its own
		// last value: shows refused because each hand's current profile carries none of
		// the panel's bindings, and panels closed by kIdleReleaseNanoseconds.
		std::uint32_t unboundRefusals = 0;
		std::uint32_t idleReleases = 0;
		// The profile path the last refused show found, NUL-terminated; empty when the
		// runtime would not name it.
		char unboundProfile[256]{};
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
