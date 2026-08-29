#pragma once

#include "Upscaler/Backend/IBackend.h"

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
	// Streamline. Stub on this build, and gated off at configure time: it compiles
	// unconditionally, reports itself unavailable, and names what is missing.
	//
	// This is the preferred backend if it can be made to work, because it puts the
	// vendor's own interposer between us and the reconstruction runtime instead of
	// making us the integration.
	//
	// The open question that unblocks it, stated honestly rather than resolved by
	// picking a side: can the framework attach to a D3D11 device that already exists?
	// Two positions, both from permitted sources, and they contradict.
	//
	// - REPORTED: the framework's initialisation must run before any graphics API call,
	//   which an F4SE plugin loaded after device creation cannot do.
	// - REPORTED: a GPL-3.0 reference we are permitted to read (the Fallout 4 Community
	//   Shaders line of work) ships exactly that pattern from a plugin, by hooking the
	//   game's single d3d11 import and using the framework's manual-hooking mode plus an
	//   explicit device-registration call.
	//
	// Neither has been read on this project. The concrete form of the question is
	// whether eUseManualHooking together with slSetD3DDevice can bind an already-created
	// device. It is settled by reading that reference's D3D11 path, which the licence
	// decision permits, and then by one throwaway plugin that attempts initialisation at
	// plugin load and logs the result.
	//
	// Caps::needsDeviceCreation is already declared true here even though the stub can
	// do nothing with it, so that turning this backend on later is a registry decision
	// rather than a rewrite of the plugin entry point.
	//
	// When the real body lands it is the only file in the tree where an sl:: symbol
	// appears. No vendor binaries or headers are committed; see THIRD_PARTY.md, where
	// this component's row is status REPORTED because no vendor licence text has been
	// read by this project yet.
	class StreamlineBackend final : public IBackend
	{
	public:
		[[nodiscard]] std::string_view Name() const noexcept override;
		[[nodiscard]] Caps Probe(ID3D11Device* a_device) const noexcept override;

		[[nodiscard]] bool Initialize(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_immediate,
			const std::filesystem::path& a_runtimeDir) noexcept override;

		[[nodiscard]] bool CreateViews(
			const Config& a_config,
			std::uint32_t a_viewCount) noexcept override;

		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> QueryRenderSize(
			std::uint32_t a_outputWidth,
			std::uint32_t a_outputHeight,
			QualityMode a_quality) const noexcept override;

		[[nodiscard]] float RecommendedMipBias(
			std::uint32_t a_renderWidth,
			std::uint32_t a_outputWidth) const noexcept override;

		[[nodiscard]] std::uint32_t JitterPhaseCount(
			std::uint32_t a_renderWidth,
			std::uint32_t a_outputWidth) const noexcept override;

		void JitterOffset(
			std::uint32_t a_frameIndex,
			std::uint32_t a_phaseCount,
			float& a_outX,
			float& a_outY) const noexcept override;

		[[nodiscard]] bool Evaluate(
			ID3D11DeviceContext* a_immediate,
			const EyeView& a_view) noexcept override;

		void DestroyViews() noexcept override;
		void Shutdown() noexcept override;
	};
}
