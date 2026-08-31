#pragma once

// The live gate between a read-only HDR observation and real reconstruction.
//
// Every run probes D3D11.1 context-state creation on the verified primary render
// thread. The optional diagnostics go deliberately boring steps farther: first
// a bounded descriptor-matched full-resource round trip, then a mutually exclusive
// bounded double-wide -> two zero-origin eyes -> NullBackend -> double-wide round trip.
// A third mutually exclusive gate initializes raw NGX and creates two native histories
// without evaluating a frame. A fourth evaluates exactly one private two-eye frame. A
// fifth reassembles one vendor frame into a private double-wide texture but still makes no
// engine color copy. A sixth copies exactly one accepted private result back to logical
// color 1. The seventh preserves both histories through 16 private warm-up frames and a
// 512-frame bounded handback window, consumes paired load/menu reset requests and begins
// safe-release teardown on the next eligible HDR frame. An eighth, FSR-specific gate evaluates one stereo
// frame into private per-eye outputs. A ninth reassembles one FSR stereo frame into the
// private double-wide target, and a tenth performs one engine handback. The eleventh
// preserves both FSR histories through the same 16-frame private warm-up and 512-frame
// handback window before render-thread teardown. Together they prove placement, lifetime,
// stereo orchestration, exact handback and bounded temporal vendor work before a production
// path is admitted.

#include <cstdint>
#include <string_view>

struct ID3D11Texture2D;

namespace Upscaler::HDRRoundTrip
{
	inline constexpr std::uint64_t kTargetSuccesses = 512;
	inline constexpr std::uint64_t kNgxContinuousWarmupSuccesses = 16;
	inline constexpr std::uint64_t kNgxContinuousTargetSuccesses = 512;
	inline constexpr std::uint64_t kFfxContinuousWarmupSuccesses = 16;
	inline constexpr std::uint64_t kFfxContinuousTargetSuccesses = 512;

	enum class HandbackSource : std::uint32_t
	{
		kNone,
		kNgx,
		kFfx,
	};

	// A serial is meaningful only within its tagged continuous backend. The stamp is
	// published after evaluation, host-state restoration, device-health validation and
	// backend activation have all succeeded; raw CopyResource counters are not admission
	// evidence for downstream diagnostics.
	struct HandbackStamp
	{
		HandbackSource source = HandbackSource::kNone;
		std::uint64_t serial = 0;
	};

	struct State
	{
		bool enabled = false;  // byte-preserving full-resource round trip
		bool stereoNullEnabled = false;
		bool ngxCreateEnabled = false;
		bool ngxEvaluateEnabled = false;
		bool ngxReassembleEnabled = false;
		bool ngxHandbackEnabled = false;
		bool ngxContinuousEnabled = false;
		bool ffxEvaluateEnabled = false;
		bool ffxReassembleEnabled = false;
		bool ffxHandbackEnabled = false;
		bool ffxContinuousEnabled = false;
		bool isolationAttempted = false;
		bool isolationReady = false;
		bool privateReady = false;
		bool completed = false;
		bool stereoNullCompleted = false;
		bool ngxCreateCompleted = false;
		bool ngxEvaluateCompleted = false;
		bool ngxReassembleCompleted = false;
		bool ngxHandbackCompleted = false;
		bool ngxContinuousCompleted = false;
		bool ngxTeardownPending = false;
		bool ngxTeardownCompleted = false;
		bool ffxEvaluateCompleted = false;
		bool ffxReassembleCompleted = false;
		bool ffxHandbackCompleted = false;
		bool ffxContinuousCompleted = false;
		bool ffxTeardownPending = false;
		bool ffxTeardownCompleted = false;
		bool sessionDisabled = false;

		std::uint64_t isolationAttempts = 0;
		std::uint64_t roundTripAttempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t failures = 0;
		std::uint64_t skips = 0;
		std::uint64_t resourceBuilds = 0;
		std::uint64_t stereoNullAttempts = 0;
		std::uint64_t stereoNullSuccesses = 0;
		std::uint64_t stereoNullFailures = 0;
		std::uint64_t stereoNullSkips = 0;
		std::uint64_t ngxCreateAttempts = 0;
		std::uint64_t ngxCreateSuccesses = 0;
		std::uint64_t ngxCreateFailures = 0;
		std::uint64_t ngxCreateSkips = 0;
		std::uint64_t ngxEvaluateAttempts = 0;
		std::uint64_t ngxEvaluateSuccesses = 0;
		std::uint64_t ngxEvaluateFailures = 0;
		std::uint64_t ngxEvaluateSkips = 0;
		std::uint64_t ngxReassembleAttempts = 0;
		std::uint64_t ngxReassembleSuccesses = 0;
		std::uint64_t ngxReassembleFailures = 0;
		std::uint64_t ngxReassembleSkips = 0;
		std::uint64_t ngxHandbackAttempts = 0;
		std::uint64_t ngxHandbackSuccesses = 0;
		std::uint64_t ngxHandbackFailures = 0;
		std::uint64_t ngxHandbackSkips = 0;
		std::uint64_t ngxHandbackCopies = 0;
		std::uint64_t ngxContinuousAttempts = 0;
		std::uint64_t ngxContinuousWarmupSuccesses = 0;
		std::uint64_t ngxContinuousSuccesses = 0;
		std::uint64_t ngxContinuousFailures = 0;
		std::uint64_t ngxContinuousSkips = 0;
		std::uint64_t ngxContinuousCopies = 0;
		std::uint64_t ngxContinuousWarmupTarget = kNgxContinuousWarmupSuccesses;
		std::uint64_t ngxContinuousTargetSuccesses = kNgxContinuousTargetSuccesses;
		std::uint64_t ngxTeardownAttempts = 0;
		std::uint64_t ngxTeardownSuccesses = 0;
		std::uint64_t ngxTeardownFailures = 0;
		std::uint64_t ffxEvaluateAttempts = 0;
		std::uint64_t ffxEvaluateSuccesses = 0;
		std::uint64_t ffxEvaluateFailures = 0;
		std::uint64_t ffxEvaluateSkips = 0;
		std::uint64_t ffxReassembleAttempts = 0;
		std::uint64_t ffxReassembleSuccesses = 0;
		std::uint64_t ffxReassembleFailures = 0;
		std::uint64_t ffxReassembleSkips = 0;
		std::uint64_t ffxHandbackAttempts = 0;
		std::uint64_t ffxHandbackSuccesses = 0;
		std::uint64_t ffxHandbackFailures = 0;
		std::uint64_t ffxHandbackSkips = 0;
		std::uint64_t ffxHandbackCopies = 0;
		std::uint64_t ffxContinuousAttempts = 0;
		std::uint64_t ffxContinuousWarmupSuccesses = 0;
		std::uint64_t ffxContinuousSuccesses = 0;
		std::uint64_t ffxContinuousFailures = 0;
		std::uint64_t ffxContinuousSkips = 0;
		std::uint64_t ffxContinuousCopies = 0;
		std::uint64_t ffxContinuousWarmupTarget = kFfxContinuousWarmupSuccesses;
		std::uint64_t ffxContinuousTargetSuccesses = kFfxContinuousTargetSuccesses;
		std::uint64_t privateReassemblySerial = 0;
		std::uint64_t ffxTeardownAttempts = 0;
		std::uint64_t ffxTeardownSuccesses = 0;
		std::uint64_t ffxTeardownFailures = 0;
		std::uint64_t targetSuccesses = kTargetSuccesses;

		std::uint32_t threadId = 0;
		std::uint32_t featureLevel = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;
		std::uint32_t sampleCount = 0;
		std::uint32_t lastExceptionCode = 0;

		std::int32_t lastError = 0;
		std::int32_t deviceRemovedReason = 0;
		std::uintptr_t device = 0;
		std::uintptr_t context = 0;
		std::uintptr_t sourceTexture = 0;
		std::uintptr_t privateTexture = 0;
	};

	// Atomic and safe from the settings/reload threads. Disabling a prepared raw backend
	// schedules release on the next verified render-thread callback.
	void Configure(
		bool a_roundTripEnabled,
		bool a_stereoNullEnabled,
		bool a_ngxCreateEnabled,
		bool a_ngxEvaluateEnabled,
		bool a_ngxReassembleEnabled,
		bool a_ngxHandbackEnabled,
		bool a_ngxContinuousEnabled,
		bool a_ffxEvaluateEnabled,
		bool a_ffxReassembleEnabled,
		bool a_ffxHandbackEnabled,
		bool a_ffxContinuousEnabled,
		std::string_view a_runtimeDir) noexcept;

	// Render-thread only, from the verified primary ClearColor seam. The create-only
	// diagnostic uses the first frame whose native colour and motion descriptors exist
	// to initialize NGX before a world load reaches the HDR dispatch. Missing startup
	// resources are a retryable skip; the HDR seam remains the exact fallback.
	void OnPrimaryRenderThread() noexcept;

	// Render-thread only, immediately before the complete vanilla HDR dispatch.
	void OnDispatch(std::int32_t a_sourceLogical, std::int32_t a_destinationLogical) noexcept;

	// Accepted continuous-gate handback contract. Per-source serials are process-lifetime
	// monotonic counters; consumers snapshot their selected source when arming and accept
	// only baseline + 1 followed by consecutive values. The tagged latest stamp is
	// release-published only after the complete health path above succeeds.
	[[nodiscard]] HandbackStamp AcceptedContinuousHandbackStamp() noexcept;
	[[nodiscard]] std::uint64_t AcceptedContinuousHandbackSerial(HandbackSource a_source) noexcept;
	[[nodiscard]] std::string_view HandbackSourceName(HandbackSource a_source) noexcept;

	// Legacy NGX-only scalar retained for source compatibility. New consumers must use
	// the tagged contract so an earlier run of another backend cannot masquerade as this
	// frame's accepted handback.
	[[nodiscard]] std::uint64_t ContinuousCopySerial() noexcept;

	// Render-thread diagnostic seam. The serial advances only after both eyes were
	// accepted into the descriptor-matched private double-wide output. The borrowed
	// texture remains valid until the next HDR teardown callback.
	[[nodiscard]] std::uint64_t PrivateReassemblySerial() noexcept;
	[[nodiscard]] ID3D11Texture2D* PrivateOutput() noexcept;

	// Lock-free diagnostic snapshot for /state.
	[[nodiscard]] State Snapshot() noexcept;
}
