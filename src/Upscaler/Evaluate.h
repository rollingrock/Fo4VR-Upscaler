#pragma once

// The per-frame orchestrator, and the only file besides StereoLayout that knows there
// are two eyes. The color-handback control remains NullBackend-only. A separately
// policy-gated entry can evaluate a real backend into private per-eye outputs only;
// both paths use measured motion, depth, native jitter and backend auto-exposure.

#include "Upscaler/Backend/IBackend.h"

#include <cstdint>
#include <string>
#include <string_view>

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
		kBackendDestroyViews,
		// Sub-rect path only: the engine motion target carries no shader-resource bind,
		// so the sanitizer cannot read it in place. Fails closed rather than copying.
		kMotionSourceNotShaderReadable,
	};

	struct State
	{
		bool prepared = false;
		bool active = false;
		bool ownsBackendState = false;
		std::uint64_t attempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t failures = 0;
		std::uint64_t leftEvaluates = 0;
		std::uint64_t rightEvaluates = 0;
		std::uint64_t leftReassemblies = 0;
		std::uint64_t rightReassemblies = 0;
		std::uint32_t prepareCount = 0;
		std::uint64_t autoExposureSubmissions = 0;
		std::uint64_t exposureContractFailures = 0;
		std::uint64_t frameTimeSamples = 0;
		std::uint64_t frameTimeFallbacks = 0;
		float frameTimeDeltaMs = 0.0f;
		FailureStep lastFailureStep = FailureStep::kNone;
		std::uintptr_t backend = 0;
		std::uintptr_t device = 0;
		std::uintptr_t context = 0;
	};

	// Sequence: validate the native control-arm contract, ensure the descriptor-keyed
	// intermediates, stage colour and camera-completed motion for each eye, reference
	// each eye's rectangle in the whole depth-stencil resource, evaluate, then deliver
	// each eye into the double-wide output. With a_config.enableSubrects the colour is
	// read and the output written in place in the double-wide targets, so the colour
	// extract and the output reassembly are not issued and the sanitizer reads engine
	// motion at the eye's base; without it each eye is copied into and out of its
	// zero-origin intermediates. The telemetry mark points are the same on both paths.
	// On a frame where StereoCapture wants the per-eye inputs, the sub-rect path retains
	// each eye's packed colour rect through Intermediates::RetainColor before that eye's
	// colour-extract mark; every other frame issues no copy there.
	// The caller owns exact D3D11 state isolation and copies the completed private output
	// back only on success.
	//
	// This gate accepts only backend "null" and native 1:1 dimensions. Motion comes from
	// logical target 35 with the measured unjittered current-to-previous convention. Depth
	// is logical DS1's R24_UNORM_X8_TYPELESS plane, standard-Z and not linearized. Native
	// jitter is converted from FO4VR projection translation to render-pixel camera offset
	// once and submitted identically to both eyes. The input is un-pre-exposed HDR;
	// preExposure/exposureScale are both 1, the exposure texture is null, and the backend
	// auto-exposure flag is mandatory. A real backend with reassembly or colour handback
	// remains separately gated.
	// a_runtimeDir is copied into the preparation key and passed to Initialize exactly;
	// an empty value is the NullBackend contract.
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
		const Backend::Config& a_config,
		std::string_view a_runtimeDir = {}) noexcept;

	// Same validated two-eye input path, but vendor output remains in the two private
	// per-eye output textures. There is no reassembly target and no engine resource can
	// be written, so this entry needs the copy path: a_config.enableSubrects must be
	// false, and a config with it set is declined at kInvalidArguments. The caller
	// clears it where the config is built rather than this entry copying the config,
	// because PrivateFrame runs inside the caller's SEH arm, where a Config copy would
	// carry a destructible string past a fault. Backend-name admission is the caller's
	// bounded-gate policy.
	[[nodiscard]] bool PrivateFrame(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWideColor,
		ID3D11Texture2D* a_doubleWideDepth,
		ID3D11Texture2D* a_doubleWideMotionVectors,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_depthDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config,
		std::string_view a_runtimeDir) noexcept;

	// Same real-backend policy seam as PrivateFrame, but reassembles the two vendor
	// outputs into a caller-owned private double-wide texture. The caller must keep that
	// destination private; this function never copies it into an engine resource.
	[[nodiscard]] bool ReassembledFrame(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWideColor,
		ID3D11Texture2D* a_doubleWideDepth,
		ID3D11Texture2D* a_doubleWideMotionVectors,
		ID3D11Texture2D* a_privateDoubleWideOutput,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_depthDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config,
		std::string_view a_runtimeDir) noexcept;

	// Creates the descriptor-keyed intermediates and two backend histories without
	// extracting, evaluating, reassembling or copying a frame. This is the separately
	// bounded live create gate; callers still own D3D11 state isolation and backend-name
	// policy. A changed runtime directory is part of the preparation key.
	[[nodiscard]] bool PrepareViews(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config,
		std::string_view a_runtimeDir) noexcept;

	[[nodiscard]] State Snapshot() noexcept;

	// The `prepared` field of Snapshot on its own, for the per-frame render-thread
	// callers that only need to know whether a backend and its views are still standing.
	// Snapshot reads twenty atomics; this reads one.
	[[nodiscard]] bool Prepared() noexcept;

	[[nodiscard]] bool Active() noexcept;

	// The preset the live views were created with, and the one the backend's runtime
	// reported back for them - "auto" when nothing was requested, "unreported" when the
	// backend does not answer, and "none" when no views exist. Both are published at the
	// one instant they change (a successful CreateViews) and read lock-free from any
	// thread. A requested letter that comes back as a different value is a silent vendor
	// fallback, which is why the pair is reported and never collapsed into one field.
	[[nodiscard]] std::string RequestedPreset() noexcept;
	[[nodiscard]] std::string ReportedPreset() noexcept;

	// Render-thread teardown. Safe to call twice. A false return preserves the backend
	// identity and owned resources because the backend could not prove a safe release.
	[[nodiscard]] bool Reset() noexcept;
}
