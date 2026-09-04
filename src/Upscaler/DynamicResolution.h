#pragma once

// Guarded ownership of Fallout 4 VR's existing dynamic-resolution ratios.
//
// This is intentionally a diagnostic controller, not the shipping DLSS resolution
// policy. It makes the engine rasterize both packed eyes into a reduced active viewport
// while preserving full-sized render-target allocations. Four floats and the VR
// hidden-area-mesh byte are reached only through verified RIP-relative instructions,
// written before an admitted main-world render, read back immediately, and rolled back
// as a set after the matching late ImageSpace range or on any failure. Menus, loading,
// and stabilization frames retain engine-native state. Live configuration reload never
// changes the latched policy.

#include <cstdint>
#include <string_view>

namespace Upscaler::DynamicResolution
{
	enum class Status : std::uint32_t
	{
		kNotInitialized,
		kDisabled,
		kArmed,
		kApplied,
		kRefused,
	};

	enum class Failure : std::uint32_t
	{
		kNone,
		kMasterDisabled,
		kGateDisabled,
		kInvalidScale,
		kHiddenAreaPolicyDisabled,
		kAnchorMismatch,
		kInitialReadFault,
		kInitialValueInvalid,
		kWriteFault,
		kReadbackFault,
		kReadbackMismatch,
		kRollbackFault,
	};

	// The outcome of a RequestRatio call - see its own comment below for the guarantees
	// the caller must hold before making one.
	enum class RatioRequestOutcome : std::uint32_t
	{
		kApplied,                  // the new scale/vendor ratio is now what the next
		                           // admitted world frame (and SnapRequestedRatios, if a
		                           // vendor ratio stands) will write
		kUnchanged,                // the request exactly repeats the live scale and vendor
		                           // ratio; nothing was touched
		kRefusedOutOfRange,        // a_scale is not finite, or not in [0.30, 1.00) - 1.00
		                           // itself is dlaa, a mode change, not a ratio request
		kRefusedNotArmed,          // WorldAdmissionGated() is false, the module never
		                           // reached Status::kArmed/kApplied, or admission is
		                           // disabled (AbortConfiguration ran) - inert by design,
		                           // never a half-applied write
		kRefusedWorldFrameActive,  // a reduced-world bracket was still open at the moment
		                           // of the write - the caller's deny window did not
		                           // close in time; refused rather than risking a torn
		                           // write across an in-flight frame
	};

	struct State
	{
		bool configured = false;
		bool initialized = false;
		bool masterEnabled = false;
		bool requested = false;
		bool worldAdmissionGated = false;
		bool worldFrameActive = false;
		bool forceHiddenAreaMeshOff = false;
		bool hiddenAreaMeshRequired = false;
		bool anchorsResolved = false;
		bool initialValuesValid = false;
		bool applied = false;
		bool hiddenAreaMeshApplied = false;
		bool requestedContractReady = false;
		bool currentValuesValid = false;
		bool selectorValueValid = false;
		bool hiddenAreaMeshValueValid = false;
		bool readbackMatched = false;
		bool hiddenAreaMeshReadbackMatched = false;
		// requestedScale is what Configure latched. The two per-axis values are what is
		// actually written and compared, and they differ from it once the vendor snap has
		// run - the whole point of the snap is that one shared float cannot hit the
		// vendor's rounded render extent on both axes.
		bool snapped = false;
		float requestedScale = 0.0f;
		float requestedScaleX = 0.0f;
		float requestedScaleY = 0.0f;
		float vendorScalingRatio = 0.0f;
		std::uint32_t snappedOutputWidth = 0;
		std::uint32_t snappedOutputHeight = 0;
		std::uint32_t snappedRenderWidth = 0;
		std::uint32_t snappedRenderHeight = 0;
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
		std::uint8_t initialHiddenAreaMesh = 0;
		std::uint8_t currentHiddenAreaMesh = 0;
		std::uintptr_t widthAddress = 0;
		std::uintptr_t heightAddress = 0;
		std::uintptr_t targetWidthAddress = 0;
		std::uintptr_t targetHeightAddress = 0;
		std::uintptr_t imageSelectorAddress = 0;
		std::uintptr_t hiddenAreaMeshAddress = 0;
		Status status = Status::kNotInitialized;
		Failure failure = Failure::kNone;
		std::uint64_t effectPassthroughAttempts = 0;
		std::uint64_t effectPassthroughSuccesses = 0;
		std::uint64_t effectPassthroughFailures = 0;
		std::uint64_t effectPassthroughRestoreFailures = 0;
		std::uint64_t frameReassertAttempts = 0;
		std::uint64_t frameReassertSuccesses = 0;
		std::uint64_t frameReassertFailures = 0;
		std::uint64_t worldBeginAttempts = 0;
		std::uint64_t worldBeginSuccesses = 0;
		std::uint64_t worldBeginFailures = 0;
		std::uint64_t worldEndAttempts = 0;
		std::uint64_t worldEndSuccesses = 0;
		std::uint64_t worldEndFailures = 0;
		std::uint64_t contractChecks = 0;
		std::uint64_t contractFailures = 0;
		std::uint64_t selectorInvariantFailures = 0;
		std::uint64_t hiddenAreaMeshInvariantFailures = 0;
	};

	struct EffectPassthroughToken
	{
		std::uintptr_t manager = 0;
		bool active = false;
	};

	// Render-thread ownership record for one admitted main-world frame. Capturing the
	// engine values at the seam, instead of assuming startup unity forever, preserves
	// any native load-time policy update. ownsMutation is raised before the first write
	// and remains raised when rollback cannot be verified, so the next watchdog can retry
	// EndReducedWorld without losing the only exact restoration values.
	struct WorldContractToken
	{
		float beforeWidth = 0.0f;
		float beforeHeight = 0.0f;
		float beforeTargetWidth = 0.0f;
		float beforeTargetHeight = 0.0f;
		std::uint8_t beforeHiddenAreaMesh = 0;
		bool captured = false;
		bool ownsMutation = false;
		bool active = false;
	};

	// The first call, before Init, is latched for the process. Later calls are treated
	// as live reloads and cannot rewrite engine render state.
	//
	// a_vendorScalingRatio is the mode's output/render ratio when a_scale came from the
	// per-mode default table, and 0 when the user typed a scale. Non-zero asks for the
	// per-axis snap at the first admitted world frame, once the per-eye output extent is
	// readable; zero writes a_scale on both axes verbatim, which is what an explicit
	// override means.
	void Configure(
		bool a_masterEnabled,
		bool a_requested,
		double a_scale,
		double a_vendorScalingRatio,
		bool a_forceHiddenAreaMeshOff,
		bool a_gateToWorldAdmission) noexcept;

	// Lock-free terminal close used by the outer configuration transaction.  New
	// handback admission stops first; any applied startup ratios are then restored
	// through the existing guarded all-four rollback.
	void AbortConfiguration() noexcept;

	// Resolves and validates the verified ratio, selector and hidden-area anchors.
	// Admission-gated Quality mode arms without writing; standalone diagnostic mode
	// retains its guarded startup write. Disabled is a successful inert result.
	[[nodiscard]] bool Init();

	// When admission gating is enabled, Init only resolves and validates the controller.
	// The verified pre-world hook owns Begin, and the matching late main-ImageSpace seam
	// owns End. No game/UI/settings thread may call either function.
	[[nodiscard]] bool WorldAdmissionGated() noexcept;
	[[nodiscard]] bool WorldFrameActive() noexcept;
	[[nodiscard]] bool BeginReducedWorld(WorldContractToken& a_token) noexcept;
	[[nodiscard]] bool EndReducedWorld(WorldContractToken& a_token) noexcept;

	// A live rewrite of only the requested scale, the two per-axis scale bit fields and
	// the vendor scaling ratio - Configure's own !g_configured latch is untouched, and
	// every other frozen fact this module owns (g_worldAdmissionGated, g_masterEnabled,
	// g_requested, g_forceHiddenAreaMeshOff, g_hiddenAreaMeshRequired) stays exactly what
	// the session's first Configure()/Init() decided. a_scale must be in [0.30, 1.00) -
	// 1.00 is dlaa, a mode change the frozen admissibility rule still owns, never this
	// entry point. a_vendorScalingRatio carries the same meaning Configure's own
	// parameter does: 0 writes a_scale verbatim on both axes (an explicit override,
	// never snapped), and a value over 1.0 asks for the per-mode default's usual
	// per-axis vendor snap, re-run at the next admitted world frame because this call
	// also bumps the generation SnapRequestedRatios latches against - see its own
	// comment. Refuses inertly (no partial write) when the bracket was never armed,
	// when Init() never reached kArmed/kApplied, or when AbortConfiguration() has run.
	//
	// Threading contract: call this from the settings/configuration thread, never the
	// render thread - BeginReducedWorld/EndReducedWorld remain render-thread-only and
	// this function does not coordinate with them through any lock, only through the
	// atomics they already read. The caller must first engage a deny window that makes
	// the world gate report WorldBlockReason::kModeSwitchPending (Upscaler::ResetPolicy::
	// EngageModeSwitchPending()) and wait for admission to actually report blocked
	// before calling this - otherwise a concurrent BeginReducedWorld could read the two
	// per-axis bits mid-write and rasterize a torn combination of the old and new ratio.
	// As a second, defence-in-depth line: this function itself refuses
	// (kRefusedWorldFrameActive) rather than writing anything if WorldFrameActive() is
	// still true when it runs, in case the caller's wait timed out. The caller is
	// responsible for releasing the deny window afterward, once every other runtime
	// input this configuration episode publishes (HDRRoundTrip's quality included) has
	// also landed - not merely once this call returns - so a reduced-world frame can
	// never reopen mid-episode against a still-mismatched quality/ratio pair.
	[[nodiscard]] RatioRequestOutcome RequestRatio(double a_scale, double a_vendorScalingRatio) noexcept;

	// Reads the four live floats again so /state can prove whether the engine retained,
	// quantized or replaced the startup request.
	[[nodiscard]] State Snapshot() noexcept;

	// Effect slot 21 must still run because the manager alternates its source and
	// destination around every active effect. On an accepted full-output vendor
	// handback, present unity live ratios through the post-HDR chain so its routing and
	// copy remain intact without scaling the already reconstructed image a second time.
	// Stock FO4VR initializes the ImageSpace dynamic-geometry selector to one and static
	// analysis found no direct executable write xref; this module therefore never writes
	// it. Hooks refreshes the selected dynamic geometry immediately after this call and
	// again after EndEffectPassthrough, because
	// its half-float ratio data was cached earlier in the frame.
	[[nodiscard]] bool BeginEffectPassthrough(EffectPassthroughToken& a_token) noexcept;
	[[nodiscard]] bool EndEffectPassthrough(EffectPassthroughToken& a_token) noexcept;
	[[nodiscard]] bool EffectPassthroughReady() noexcept;

	// Mirrors the verified slot-21 IsActive predicate from the two live ratios. A
	// guarded read failure chooses active so the manager still routes through and
	// writes the destination selected for this effect.
	[[nodiscard]] bool NativeEffectActive() noexcept;

	// Positive admission contract for a reduced-input/full-output engine handback. A
	// read fault, an inexact ratio, selector != 1, or an enabled hidden-area mesh all
	// decline the copy.
	[[nodiscard]] bool FrameContractReadyForHandback() noexcept;

	// A drift includes faults and implausible values, not just a different plausible value.
	// In admission-gated mode the per-world updater first closes a stale prior-frame token;
	// standalone diagnostic mode instead reasserts its latched startup request.
	[[nodiscard]] bool FrameContractDrifted() noexcept;
	[[nodiscard]] bool RestoreRequestedBeforeWorld() noexcept;
	[[nodiscard]] bool RestoreRequestedAfterEngineEffect() noexcept;

	[[nodiscard]] std::string_view StatusName(Status a_status) noexcept;
	[[nodiscard]] std::string_view FailureName(Failure a_failure) noexcept;
	[[nodiscard]] std::string_view RatioRequestOutcomeName(RatioRequestOutcome a_outcome) noexcept;
}
