#pragma once

// The live gate between a read-only HDR observation and real reconstruction.
//
// Every run probes D3D11.1 context-state creation on the verified top-level HDR
// render thread. The optional diagnostics go deliberately boring steps farther: first
// a bounded descriptor-matched full-resource round trip, then a mutually exclusive
// bounded double-wide -> two zero-origin eyes -> NullBackend -> double-wide round trip.
// No shader runs and the bytes are not transformed. Together they prove placement,
// lifetime, stereo orchestration and exact handback before a temporal runtime may own
// the private output.

#include <cstdint>

namespace Upscaler::HDRRoundTrip
{
	inline constexpr std::uint64_t kTargetSuccesses = 512;

	struct State
	{
		bool enabled = false;  // byte-preserving full-resource round trip
		bool stereoNullEnabled = false;
		bool isolationAttempted = false;
		bool isolationReady = false;
		bool privateReady = false;
		bool completed = false;
		bool stereoNullCompleted = false;
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
	void Configure(bool a_roundTripEnabled, bool a_stereoNullEnabled) noexcept;

	// Render-thread only, immediately before the complete vanilla HDR dispatch.
	void OnDispatch(std::int32_t a_sourceLogical, std::int32_t a_destinationLogical) noexcept;

	// Lock-free diagnostic snapshot for /state.
	[[nodiscard]] State Snapshot() noexcept;
}
