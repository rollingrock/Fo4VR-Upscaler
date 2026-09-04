#pragma once

#include <ShlObj.h>

#include <filesystem>

// The one real (non-MO2-virtualized) place this plugin may write. main.cpp resolves it
// for the plugin's own log; the settings override writer (Settings/Writer.h) reuses the
// exact same function rather than composing a second path by hand.
namespace Settings::Paths
{
	// Documents/My Games/Fallout4VR/F4SE. Not Data/F4SE/Plugins/<mod>: under Mod
	// Organizer, Data is a virtual filesystem and a write into it lands in the active
	// profile's overwrite folder rather than the mod's own, while this Documents path is
	// a real filesystem location every plugin here already treats as writable - it is
	// where Fo4VRUpscaler.log itself lives. Empty when Known Folder resolution fails
	// (e.g. a broken Documents redirection); callers must treat empty as "no safe place
	// to write" and refuse rather than invent a fallback.
	[[nodiscard]] inline std::filesystem::path F4SEUserDirectory()
	{
		PWSTR raw = nullptr;
		if (::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw) != S_OK) {
			return {};
		}
		std::filesystem::path path{ raw };
		::CoTaskMemFree(raw);
		return path / L"My Games" / L"Fallout4VR" / L"F4SE";
	}

	// The layered settings override the writer (Settings/Writer.h) persists user changes
	// into and the loader (Settings.h) reads after the shipped TOML, letting it win per
	// key. Beside the plugin's own log, not under Data, and it survives a mod update that
	// replaces Data because it is per-user state outside the mod's own folder entirely.
	[[nodiscard]] inline std::filesystem::path OverrideFilePath()
	{
		const auto dir = F4SEUserDirectory();
		return dir.empty() ? std::filesystem::path{} : dir / L"Fo4VRUpscalerOverride.toml";
	}
}
