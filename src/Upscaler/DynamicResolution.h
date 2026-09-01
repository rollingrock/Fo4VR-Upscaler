#pragma once

// Startup-only ownership of Fallout 4 VR's existing dynamic-resolution ratios.
//
// This is intentionally a diagnostic controller, not the shipping DLSS resolution
// policy. It makes the engine rasterize both packed eyes into a reduced active viewport
// while preserving full-sized render-target allocations. Four floats are reached only
// through verified RIP-relative instructions, written before world rendering, read back
// immediately, and rolled back as a set on any failure. A world load may replace them;
// that frame finishes through the engine's slot-21 effect before the requested set is
// reasserted. Live configuration reload still never changes the latched policy.

#include <cstdint>
#include <string_view>

namespace Upscaler::DynamicResolution
{
	enum class Status : std::uint32_t
	{
		kNotInitialized,
		kDisabled,
		kApplied,
		kRefused,
	};

	enum class Failure : std::uint32_t
	{
		kNone,
		kMasterDisabled,
		kGateDisabled,
		kInvalidScale,
		kAnchorMismatch,
		kInitialReadFault,
		kInitialValueInvalid,
		kWriteFault,
		kReadbackFault,
		kReadbackMismatch,
		kRollbackFault,
	};

	struct State
	{
		bool configured = false;
		bool initialized = false;
		bool masterEnabled = false;
		bool requested = false;
		bool anchorsResolved = false;
		bool initialValuesValid = false;
		bool applied = false;
		bool currentValuesValid = false;
		bool selectorValueValid = false;
		bool readbackMatched = false;
		float requestedScale = 0.0f;
		float beforeWidth = 0.0f;
		float beforeHeight = 0.0f;
		float beforeTargetWidth = 0.0f;
		float beforeTargetHeight = 0.0f;
		float currentWidth = 0.0f;
		float currentHeight = 0.0f;
		float currentTargetWidth = 0.0f;
		float currentTargetHeight = 0.0f;
		std::uint8_t initialImageSelector = 0;
		std::uint8_t currentImageSelector = 0;
		std::uintptr_t widthAddress = 0;
		std::uintptr_t heightAddress = 0;
		std::uintptr_t targetWidthAddress = 0;
		std::uintptr_t targetHeightAddress = 0;
		std::uintptr_t imageSelectorAddress = 0;
		Status status = Status::kNotInitialized;
		Failure failure = Failure::kNone;
		std::uint64_t effectPassthroughAttempts = 0;
		std::uint64_t effectPassthroughSuccesses = 0;
		std::uint64_t effectPassthroughFailures = 0;
		std::uint64_t effectPassthroughRestoreFailures = 0;
		std::uint64_t frameReassertAttempts = 0;
		std::uint64_t frameReassertSuccesses = 0;
		std::uint64_t frameReassertFailures = 0;
	};

	struct EffectPassthroughToken
	{
		float width = 0.0f;
		float height = 0.0f;
		std::uint8_t imageSelector = 0;
		bool active = false;
	};

	// The first call, before Init, is latched for the process. Later calls are treated
	// as live reloads and cannot rewrite engine render state.
	void Configure(bool a_masterEnabled, bool a_requested, double a_scale) noexcept;

	// Resolves all four verified anchors and, only when requested, performs the guarded
	// startup write/readback. Disabled is a successful inert result; refusal is false.
	[[nodiscard]] bool Init();

	// Reads the four live floats again so /state can prove whether the engine retained,
	// quantized or replaced the startup request.
	[[nodiscard]] State Snapshot() noexcept;

	// Effect slot 21 must still run because the manager alternates its source and
	// destination around every active effect. On an accepted full-output vendor
	// handback, present unity live ratios and normal full-resolution image geometry
	// through the post-HDR chain so its routing and copy remain intact without cropping
	// or scaling the already reconstructed image a second time.
	[[nodiscard]] bool BeginEffectPassthrough(EffectPassthroughToken& a_token) noexcept;
	[[nodiscard]] bool EndEffectPassthrough(EffectPassthroughToken& a_token) noexcept;

	// Mirrors the verified slot-21 IsActive predicate from the two live ratios. A
	// guarded read failure chooses active so the manager still routes through and
	// writes the destination selected for this effect.
	[[nodiscard]] bool NativeEffectActive() noexcept;

	// Loading screens can replace the live ratios while retaining our target ratios.
	// A drifted frame must finish with its own ratio, so the HDR seam only skips it;
	// this restores the startup contract after slot 21 has consumed that frame.
	[[nodiscard]] bool FrameContractDrifted() noexcept;
	[[nodiscard]] bool RestoreRequestedAfterEngineEffect() noexcept;

	[[nodiscard]] std::string_view StatusName(Status a_status) noexcept;
	[[nodiscard]] std::string_view FailureName(Failure a_failure) noexcept;
}
