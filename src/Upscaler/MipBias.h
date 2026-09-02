#pragma once

// Rewrites Fallout4VR's own sampler-state table with a reduced-resolution mip LOD bias
// and puts it back, once per world frame.
//
// This half owns what only the game gives it: the three anchors that agree on where the
// table is, the settings, the two render-path seams and the telemetry. `SamplerBias`
// owns the table mechanism and is engine-free so the harness can drive it.
//
// The restore path matters more than the apply path. The engine's shutdown releaser
// walks the same 320 slots and releases whatever is in them, so a duplicate still
// installed at shutdown is our object handed to the engine and the engine's original
// leaked. Restore is therefore infallible by contract, runs first on every path that can
// leave the world frame, and never depends on anything else having succeeded.

#include <cstddef>
#include <cstdint>

#include "Upscaler/Addresses.h"
#include "Upscaler/SamplerBias.h"

namespace Upscaler::Backend
{
	class IBackend;
}

namespace Upscaler::MipBias
{
	// What the engine's cached state block says is bound in one sampler slot, and where
	// that lands in the table. `valid` is false when a field is out of the creation
	// loop's range, which for the vertex stage means the probe read the wrong offsets
	// rather than that the engine bound something strange.
	struct ProbeSlot
	{
		std::int32_t addressMode = -1;
		std::int32_t filter = -1;
		std::int32_t mip = -1;
		std::int32_t index = -1;
		bool biased = false;
		bool valid = false;
	};

	struct Probe
	{
		std::uint64_t requestedSequence = 0;
		std::uint64_t completedSequence = 0;
		bool pending = false;
		bool captured = false;
		std::uintptr_t stateBlock = 0;
		ProbeSlot ps[Addr::kStateSamplerSlotCount]{};
		ProbeSlot vs[Addr::kStateSamplerSlotCount]{};
	};

	struct State
	{
		bool anchorsResolved = false;
		bool anchorsAgreed = false;
		std::uintptr_t tableAddress = 0;
		SamplerBias::State table{};
	};

	// Resolves the three anchors and requires all three to name the same table. Pure
	// code-byte reads: no engine object has to exist and no D3D11 call is made, so this
	// is safe at plugin load and on any thread. False leaves the feature inert.
	[[nodiscard]] bool Initialize();

	[[nodiscard]] bool Resolved() noexcept;

	// Publishes the three TOML values. Any thread; the render path reads them at Build.
	void Configure(float a_overrideBias, bool a_includeTrilinear, bool a_lowerMinLod) noexcept;

	// Render-thread only, from the backend-configuration transaction. The widths are the
	// per-eye pair the backend was configured with, never the live dynamic-resolution
	// float: the quality path forces that ratio to identity around its own handback, so
	// reading it here would derive a bias for a resolution nothing is rendering.
	[[nodiscard]] bool Build(
		const Backend::IBackend* a_backend,
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth) noexcept;

	// Render-thread only. Apply also services one armed sampler probe, which is the only
	// point in the frame where the engine's per-slot selection is both settled and about
	// to be used.
	void Apply() noexcept;
	void Restore() noexcept;

	// Arms one read-only render-thread read of the cached state block's sampler
	// selection. Changes no render state. A newer request supersedes an older pending
	// one; the returned sequence is how a caller tells its result from the last one.
	[[nodiscard]] std::uint64_t RequestProbe() noexcept;
	[[nodiscard]] Probe ProbeSnapshot() noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
