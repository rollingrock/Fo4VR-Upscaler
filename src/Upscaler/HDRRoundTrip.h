#pragma once

// The live gate between a read-only HDR observation and real reconstruction.
//
// Every run probes D3D11.1 context-state creation on the verified top-level HDR
// render thread. The optional diagnostic goes one deliberately boring step farther:
// for a bounded number of frames it copies logical colour 1 to a descriptor-matched
// private texture and straight back before vanilla HDR runs 1 -> 3. No shader runs and
// the bytes are not transformed. This proves placement, lifetime and exact state
// handback before an upscaler is allowed to own the private output.

#include <cstdint>

namespace Upscaler::HDRRoundTrip
{
	inline constexpr std::uint64_t kTargetSuccesses = 512;

	struct State
	{
		bool enabled = false;
		bool isolationAttempted = false;
		bool isolationReady = false;
		bool privateReady = false;
		bool completed = false;
		bool sessionDisabled = false;

		std::uint64_t isolationAttempts = 0;
		std::uint64_t roundTripAttempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t failures = 0;
		std::uint64_t skips = 0;
		std::uint64_t resourceBuilds = 0;
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
	void Configure(bool a_enabled) noexcept;

	// Render-thread only, immediately before the complete vanilla HDR dispatch.
	void OnDispatch(std::int32_t a_sourceLogical, std::int32_t a_destinationLogical) noexcept;

	// Lock-free diagnostic snapshot for /state.
	[[nodiscard]] State Snapshot() noexcept;
}
