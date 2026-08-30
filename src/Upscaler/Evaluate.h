#pragma once

// The per-frame orchestrator, and the only file besides StereoLayout that knows there
// are two eyes. Its live gate remains NullBackend-only; measured motion, depth and
// native jitter inputs are present, and v1 explicitly selects backend auto-exposure.

#include "Upscaler/Backend/IBackend.h"

#include <cstdint>

#include <d3d11.h>

namespace Upscaler::Evaluate
{
	enum class FailureStep : std::uint32_t
	{
		kNone,
		kInvalidArguments,
		kUnsupportedBackend,
		kUnsupportedConfig,
		kIntermediates,
		kBackendInitialize,
		kBackendCreateViews,
		kStereoGeometry,
		kExtractLeft,
		kEvaluateLeft,
		kReassembleLeft,
		kExtractRight,
		kEvaluateRight,
		kReassembleRight,
		kExtractMotionLeft,
		kExtractMotionRight,
		kReferenceDepthLeft,
		kReferenceDepthRight,
		kReadJitter,
		kExposureContract,
	};

	struct State
	{
		bool prepared = false;
		bool active = false;
		std::uint64_t attempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t failures = 0;
		std::uint64_t leftEvaluates = 0;
		std::uint64_t rightEvaluates = 0;
		std::uint32_t prepareCount = 0;
		std::uint64_t autoExposureSubmissions = 0;
		std::uint64_t exposureContractFailures = 0;
		FailureStep lastFailureStep = FailureStep::kNone;
		std::uintptr_t backend = 0;
		std::uintptr_t device = 0;
		std::uintptr_t context = 0;
	};

	// Sequence: validate the native control-arm contract, ensure the six descriptor-keyed
	// intermediates, extract colour and motion for each eye, reference each eye's rectangle
	// in the whole depth-stencil resource, then reassemble the evaluated private outputs.
	// The caller owns exact D3D11 state isolation and copies the completed private output
	// back only on success.
	//
	// This gate accepts only backend "null" and native 1:1 dimensions. Motion comes from
	// logical target 35 with the measured unjittered current-to-previous convention. Depth
	// is logical DS1's R24_UNORM_X8_TYPELESS plane, standard-Z and not linearized. Native
	// jitter is converted from FO4VR projection translation to render-pixel camera offset
	// once and submitted identically to both eyes. The input is un-pre-exposed HDR;
	// preExposure/exposureScale are both 1, the exposure texture is null, and the backend
	// auto-exposure flag is mandatory. A real backend still remains separately gated.
	[[nodiscard]] bool Frame(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWideColor,
		ID3D11Texture2D* a_doubleWideDepth,
		ID3D11Texture2D* a_doubleWideMotionVectors,
		ID3D11Texture2D* a_doubleWideOutput,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_depthDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
	[[nodiscard]] bool Active() noexcept;

	// Render-thread teardown. Safe to call twice.
	void Reset() noexcept;
}
