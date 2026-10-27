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

	// The backend's own lifecycle, counted and priced apart from the frame counters
	// above. A live configuration change is render-thread work the operator feels as a
	// hitch, and the two counts are what say which kind of work it was: re-creating the
	// views under a runtime that stayed up, or tearing the runtime down and initialising
	// it again. The milliseconds are a steady clock around the backend calls themselves,
	// so a slow one can be named rather than inferred from the log's timestamps.
	struct Lifecycle
	{
		std::uint64_t initializeCount = 0;
		// Initialize calls that succeeded. Zero beside a non-zero initializeCount is a
		// backend that has never started in this process.
		std::uint64_t initializeSuccesses = 0;
		std::uint64_t createViewsCount = 0;
		std::uint64_t destroyViewsCount = 0;
		std::uint64_t shutdownCount = 0;
		// One preparation each: a views-only reconfiguration against a full one that went
		// through Shutdown and Initialize.
		std::uint64_t viewsOnlyPreparations = 0;
		std::uint64_t fullPreparations = 0;
		// Views released with the runtime left standing - the operator's toggle to `off`.
		// Its pair is a views-only preparation on the way back on, with initializeCount
		// unmoved between the two.
		std::uint64_t viewsReleases = 0;
		// Whether an initialised runtime is standing right now, views or no views. False
		// beside a non-zero initializeCount is a session that shut its runtime down.
		bool runtimeUp = false;
		float lastInitializeMs = 0.0f;
		float lastCreateViewsMs = 0.0f;
		float lastDestroyViewsMs = 0.0f;
		float lastShutdownMs = 0.0f;
		// The whole preparation, teardown included. This is what a ratio step's hitch is
		// made of; the four above say which call it came from.
		float lastPrepareMs = 0.0f;
		bool lastPrepareViewsOnly = false;
	};

	// Lock-free, any thread. Every field is published by the render thread at the one
	// instant it changes.
	[[nodiscard]] Lifecycle LifecycleSnapshot() noexcept;

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

	// The vendor quality bucket the live mode names, and the one the views were actually
	// created at. Same spellings and same publication instant as the preset pair above.
	// They differ whenever the live ratio has moved out of the mode's own bucket, which
	// is the state a preset pinned on only one bucket is silently wrong in.
	[[nodiscard]] std::string RequestedBucket() noexcept;
	[[nodiscard]] std::string ResolvedBucket() noexcept;

	// Render-thread teardown. Safe to call twice. A false return preserves the backend
	// identity and owned resources because the backend could not prove a safe release.
	[[nodiscard]] bool Reset() noexcept;

	// The views half of Reset, for a configuration that closed the vendor's gates while
	// the runtime under those views is still perfectly good - the operator's toggle to
	// `off`. Releases the histories, the per-eye intermediates, the jitter sequence and
	// the mip bias; keeps the initialised runtime and the identity it belongs to, so the
	// way back on is a CreateViews rather than half a second of SDK initialisation on the
	// render thread. Same thread and same contract as Reset: render-thread only, safe to
	// call twice, and a false return retains everything for a later retry. A fault or a
	// device change still goes through Reset.
	[[nodiscard]] bool ReleaseViews() noexcept;
}
