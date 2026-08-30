#pragma once

// The live gate between a read-only HDR observation and real reconstruction.
//
// Every run probes D3D11.1 context-state creation on the verified top-level HDR
// render thread. The optional diagnostics go deliberately boring steps farther: first
// a bounded descriptor-matched full-resource round trip, then a mutually exclusive
// bounded double-wide -> two zero-origin eyes -> NullBackend -> double-wide round trip.
// A third mutually exclusive gate initializes raw NGX and creates two native histories
// without evaluating a frame. A fourth evaluates exactly one private two-eye frame. A
// fifth reassembles one vendor frame into a private double-wide texture but still makes no
// engine color copy. Together they prove placement, lifetime, stereo orchestration, exact
// handback and bounded vendor work before temporal output is admitted.

#include <cstdint>
#include <string_view>

namespace Upscaler::HDRRoundTrip
{
	inline constexpr std::uint64_t kTargetSuccesses = 512;

	struct State
	{
		bool enabled = false;  // byte-preserving full-resource round trip
		bool stereoNullEnabled = false;
		bool ngxCreateEnabled = false;
		bool ngxEvaluateEnabled = false;
		bool ngxReassembleEnabled = false;
		bool isolationAttempted = false;
		bool isolationReady = false;
		bool privateReady = false;
		bool completed = false;
		bool stereoNullCompleted = false;
		bool ngxCreateCompleted = false;
		bool ngxEvaluateCompleted = false;
		bool ngxReassembleCompleted = false;
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

	// Atomic and safe from the settings/reload threads. Disabling does not release a
	// render-thread-owned resource; it merely makes the callback inert.
	void Configure(
		bool a_roundTripEnabled,
		bool a_stereoNullEnabled,
		bool a_ngxCreateEnabled,
		bool a_ngxEvaluateEnabled,
		bool a_ngxReassembleEnabled,
		std::string_view a_runtimeDir) noexcept;

	// Render-thread only, immediately before the complete vanilla HDR dispatch.
	void OnDispatch(std::int32_t a_sourceLogical, std::int32_t a_destinationLogical) noexcept;

	// Lock-free diagnostic snapshot for /state.
	[[nodiscard]] State Snapshot() noexcept;
}
