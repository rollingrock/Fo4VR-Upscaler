#pragma once

// Render-path instruments for answering "are the inputs actually the resources and
// bindings we think they are", which no look at the final image can answer. The binding
// observation is real and read-only; target readback/visualisation remains blocked.

#include <cstdint>
#include <array>
#include <string_view>

namespace Upscaler::Debug
{
	enum class ObservationPoint : std::uint32_t
	{
		none = 0,
		clearColor,
		hdrFinalDraw,
		temporalAADraw,
	};

	struct ObservedTarget
	{
		std::int32_t logical = -1;
		std::int32_t phys = -1;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;
		bool valid = false;
	};

	enum class DepthViewBinding : std::uint32_t
	{
		none = 0,
		writable,
		readOnlyDepth,
		readOnlyStencil,
		readOnlyBoth,
		unmatched,
	};

	struct BindingObservation
	{
		struct BoundSlot
		{
			bool bound = false;
			std::int32_t colorLogical = -1;
			std::int32_t depthLogical = -1;
		};

		std::uint64_t requestedSequence = 0;
		std::uint64_t completedSequence = 0;
		bool pending = false;
		bool captured = false;
		ObservationPoint point = ObservationPoint::none;

		std::uint64_t clearCall = 0;
		std::uint64_t hdrCall = 0;
		std::uint64_t temporalAACall = 0;
		std::uint32_t threadId = 0;
		std::uintptr_t renderer = 0;
		std::uintptr_t context = 0;

		ObservedTarget currentColor{};
		ObservedTarget currentDepth{};
		ObservedTarget motionVectors{};
		ObservedTarget linearDepth{};

		std::uint32_t omRenderTargetCount = 0;
		std::int32_t currentColorOmSlot = -1;
		DepthViewBinding currentDepthOmView = DepthViewBinding::none;
		std::array<BoundSlot, 8> omBindings{};

		std::uint32_t psSrvCount = 0;
		std::int32_t motionVectorsPsSlot = -1;
		std::int32_t linearDepthPsSlot = -1;
		std::int32_t currentDepthPsSlot = -1;
		std::array<BoundSlot, 128> psBindings{};
	};

	// Arms a new one-shot. A newer request supersedes an older pending one; the returned
	// sequence is how a caller distinguishes its result from the previous capture.
	[[nodiscard]] std::uint64_t RequestBindingObservation(ObservationPoint a_point) noexcept;

	// Called only by Hooks after it has proved the current ClearColor call is on the
	// primary steady-state thread. It never waits for the reporting thread: a busy result
	// lock defers the observation to the next clear.
	void OnPrimaryClearColor(
		std::uintptr_t a_renderer,
		std::uint64_t a_clearCall,
		std::uint32_t a_threadId) noexcept;

	// Called by the direct pixel-draw call hook after the vanilla draw returns but
	// before BSImagespaceShader performs its virtual cleanup. At that instant the OM
	// output and PS inputs used by TemporalAA are still bound.
	void OnTemporalAADraw(
		std::uintptr_t a_renderer,
		std::uint64_t a_temporalAACall,
		std::uint32_t a_threadId) noexcept;

	// Called by the same generic pixel-draw hook while the HDR effect-dispatch scope is
	// active. Intermediate luminance draws are ignored; the request completes only when
	// the live OM bindings resolve to the dispatch's destination logical target.
	void OnHDRDraw(
		std::uintptr_t a_renderer,
		std::uint64_t a_hdrCall,
		std::int32_t a_destinationLogical,
		std::uint32_t a_threadId) noexcept;

	[[nodiscard]] std::string_view ObservationPointName(ObservationPoint a_point) noexcept;

	// Thread-safe snapshot for /state. No engine or D3D11 access occurs here.
	[[nodiscard]] BindingObservation BindingObservationSnapshot() noexcept;

	// One-shot census of one frame's ImageSpace chain: every effect dispatch and every
	// pixel pass between one pre-world dynamic-geometry update and the next, each draw
	// with its OM targets, its pixel-stage shader resources resolved to the engine's
	// logical namespace, and the viewport it ran under. The three single-point
	// observations above answer "what was bound at this one draw"; this answers "which
	// pass reads which target, at what extent", which is the only way to tell a pass
	// sampling the reduced sub-rect from one sampling the full target.
	//
	// It sees the ImageSpace chain and nothing else, because the effect dispatch and the
	// shared ImageSpace pixel-draw call are the two seams this plugin already owns. World
	// geometry - the water surface and the deferred composite among it - draws through
	// neither and does not appear.
	//
	// Development diagnostic: a recording frame reads D3D11 state per draw and is not a
	// frame-timing measurement.
	inline constexpr std::size_t kCensusSrvSlots = 16;   // recorded slots; higher ones are counted only
	inline constexpr std::size_t kCensusEntries = 256;

	// Mirrors of Addr::kRenderTargetCount / kDepthStencilCount so this header need not pull
	// in Addresses.h. Debug.cpp static_asserts that they still agree.
	inline constexpr std::size_t kCensusColorTargets = 144;
	inline constexpr std::size_t kCensusDepthTargets = 17;

	// One bound shader resource's underlying texture, read straight off the view.
	//
	// This is what lets an extent claim cover every bind rather than only the named ones.
	// A slot that resolves to no logical id still reports the size and format of what it
	// samples, so "no pass samples a colour target at a mismatched extent" becomes a
	// statement about every bound slot instead of only the ones the name table reaches.
	struct CensusSrvDesc
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;     // a DXGI_FORMAT
		std::uint32_t arraySize = 0;  // 2 on an array target
		std::uint32_t mipLevels = 0;
		std::uint32_t dimension = 0;  // a D3D11_RESOURCE_DIMENSION; 3 is texture2D
	};

	// Whether a logical id was nameable at all during this census.
	//
	// Resolution is by pointer identity against one cached view per logical id, so a bind
	// resolves only if the manager held a canonical SRV for that target when the frame
	// opened. Without this table an unresolved slot is ambiguous - "nothing bound it" and
	// "nothing could have named it" look identical - and an absolute negative about a
	// particular id ("no pass binds ssrcolor 7") is not supported by the recording. With
	// it, the negative is checkable per id.
	struct CensusTargetInfo
	{
		bool valid = false;
		bool hasRtv = false;
		bool hasSrv = false;  // the one that decides whether an SRV bind could resolve here
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;
	};

	enum class CensusEntryKind : std::uint32_t
	{
		none = 0,
		dispatch,
		draw,
	};

	struct CensusEntry
	{
		CensusEntryKind kind = CensusEntryKind::none;
		std::uint32_t order = 0;

		// dispatch only. effectSlot is the index into the manager's effect array, the same
		// namespace the HDR (3) and dynamic-resolution (21) hooks already filter on; -1
		// means the pointer matched no scanned slot.
		std::int32_t effectSlot = -1;
		std::int32_t sourceLogical = -1;
		std::int32_t destinationLogical = -1;

		// draw only.
		std::uint32_t omBoundCount = 0;
		std::array<std::int32_t, 8> omColorLogical{};
		std::int32_t depthLogical = -1;
		DepthViewBinding depthView = DepthViewBinding::none;

		std::uint32_t srvBoundCount = 0;      // over all 128 pixel-stage slots
		std::uint32_t srvBoundAboveScan = 0;  // bound in a slot this census does not record
		std::array<bool, kCensusSrvSlots> srvBound{};
		std::array<std::int32_t, kCensusSrvSlots> srvColorLogical{};
		std::array<std::int32_t, kCensusSrvSlots> srvDepthLogical{};
		std::array<CensusSrvDesc, kCensusSrvSlots> srvDesc{};

		std::uint32_t viewportCount = 0;
		float viewportX = 0.0f;
		float viewportY = 0.0f;
		float viewportWidth = 0.0f;
		float viewportHeight = 0.0f;

		std::int32_t currentColorLogical = -1;
		std::uint32_t currentColorWidth = 0;
		std::uint32_t currentColorHeight = 0;
	};

	struct FrameCensus
	{
		std::uint64_t requestedSequence = 0;
		std::uint64_t completedSequence = 0;
		bool pending = false;
		bool recording = false;
		bool truncated = false;
		std::uint32_t dispatches = 0;
		std::uint32_t draws = 0;
		std::uint32_t count = 0;
		std::uint32_t threadId = 0;

		// The name table this census resolved against, snapshotted when the frame opened.
		std::array<CensusTargetInfo, kCensusColorTargets> colorTargets{};
		std::array<CensusTargetInfo, kCensusDepthTargets> depthTargets{};

		std::array<CensusEntry, kCensusEntries> entries{};
	};

	// Arms one census. The returned sequence is how a caller tells its own result from the
	// previous one; a newer request supersedes an older pending one.
	[[nodiscard]] std::uint64_t RequestFrameCensus() noexcept;

	// The render-thread gate the hook sites test before doing any work. One relaxed load.
	[[nodiscard]] bool FrameCensusRecording() noexcept;

	// Called by the pre-world dynamic-geometry update hook on every main-view frame. It
	// closes a recording frame and opens the next one when a census is armed.
	void OnFrameBoundary() noexcept;

	// Called by the ImageSpace effect-dispatch hook before the effect runs, and by the
	// shared ImageSpace pixel-draw hook after the draw returns and before the shader's
	// own cleanup unbinds. Render thread only; both are no-ops unless a census is
	// recording, which the caller has already checked.
	void OnImageSpaceDispatch(
		std::int32_t a_effectSlot,
		std::int32_t a_sourceLogical,
		std::int32_t a_destinationLogical) noexcept;
	void OnImageSpacePixelDraw() noexcept;

	// Thread-safe copy of the last completed census. The structure is tens of kilobytes,
	// so the caller owns the storage.
	void CopyFrameCensus(FrameCensus& a_out) noexcept;

	// A run of samples taken at the UI/menu render entry - the call in Swap that runs
	// Interface3D's renderer list, after every DrawWorld_And_UI and therefore after this
	// plugin's last seam, the late ImageSpace range where EndReducedWorld restores the
	// native ratios.
	//
	// This answers what the census cannot: the census sees the ImageSpace chain, and the
	// HUD is not in it - the compass is absent from the submitted target at both of the
	// capture points this plugin owns, on the reduced path and on native alike. What the
	// UI draw actually runs under - the two engine projection-jitter floats, the four
	// dynamic-resolution ratios, the ImageSpace geometry selector, the hidden-area byte,
	// the viewport and the target - is only readable at the UI entry itself.
	//
	// A run, not a one-shot, because the question is a per-frame pattern: a jitter float
	// that changes every frame and one that is constant are the two hypotheses, and one
	// sample cannot tell them apart.
	inline constexpr std::size_t kUiSamples = 64;

	struct UiSample
	{
		std::uint64_t uiCall = 0;  // the UI-entry call counter, so a gap in the run is visible
		std::int32_t requestedTargetId = -1;  // ECX at the call site: the pass's destination id

		// The engine's own jitter block, read at this instant rather than reported from
		// the last commit.
		bool jitterResolved = false;
		bool jitterRead = false;
		bool jitterTorn = false;
		std::uint32_t jitterPhase = 0;
		std::uint32_t projectionXBits = 0;
		std::uint32_t projectionYBits = 0;
		std::uint32_t jitterPerEyeWidth = 0;
		std::uint32_t jitterPerEyeHeight = 0;
		std::uint32_t jitterEnabled = 0;
		std::uint32_t jitterDisableCount = 0;

		// The dynamic-resolution contract, read at this instant.
		bool ratiosValid = false;
		bool selectorValid = false;
		bool hiddenAreaValid = false;
		bool worldFrameActive = false;
		float liveWidthRatio = 0.0f;
		float liveHeightRatio = 0.0f;
		float targetWidthRatio = 0.0f;
		float targetHeightRatio = 0.0f;
		std::uint8_t imageSelector = 0;
		std::uint8_t hiddenAreaMesh = 0;

		// What the pass is about to run against.
		std::uint32_t viewportCount = 0;
		float viewportX = 0.0f;
		float viewportY = 0.0f;
		float viewportWidth = 0.0f;
		float viewportHeight = 0.0f;
		std::int32_t currentColorLogical = -1;
		std::uint32_t currentColorWidth = 0;
		std::uint32_t currentColorHeight = 0;
		std::uint32_t omBoundCount = 0;
		std::array<std::int32_t, 8> omColorLogical{};
		std::int32_t depthLogical = -1;
		DepthViewBinding depthView = DepthViewBinding::none;
		std::uint32_t depthWidth = 0;
		std::uint32_t depthHeight = 0;
	};

	struct UiObservation
	{
		std::uint64_t requestedSequence = 0;
		std::uint64_t completedSequence = 0;
		std::uint32_t requestedFrames = 0;
		std::uint32_t recorded = 0;
		std::uint32_t threadId = 0;
		std::uint64_t uiCalls = 0;  // every UI entry seen this session, armed or not
		bool armed = false;
		std::array<UiSample, kUiSamples> samples{};
	};

	// Arms one run of up to kUiSamples consecutive UI entries. The returned sequence is
	// how a caller tells its own result from the previous one; a newer request supersedes
	// an older pending one. A count of 0 or above kUiSamples is clamped into range.
	[[nodiscard]] std::uint64_t RequestUiObservation(std::uint32_t a_frames) noexcept;

	// Called by the UI/menu render-entry hook BEFORE the original runs, so the state a
	// sample carries is the state the UI pass is about to draw under. Render thread only.
	//
	// Unlike the census's two seams this one runs once a frame, so it carries no separate
	// gate function: it counts the entry - which is what lets a reader tell "the run
	// recorded 64 of 64" from "the hook never ran" - and returns after one relaxed load
	// when no run is armed.
	void OnUiRenderEntry(std::int32_t a_requestedTargetId) noexcept;

	// Thread-safe copy of the last completed run. Several kilobytes, so the caller owns
	// the storage.
	void CopyUiObservation(UiObservation& a_out) noexcept;

	// Dumps to DDS, never to BMP: a target in an uncommon two-channel format silently
	// defeated a BMP decoder for a whole investigation, and the motion-vector target is
	// the next candidate for the same trap.
	//
	// Live hazard: capturing a render target while a per-frame pass writes it has
	// deadlocked this game before, and an upscaler writes its output target every frame.
	[[nodiscard]] bool DumpTarget(std::int32_t a_logical, std::string_view a_label);

	// Instrumentation has to represent failure states. Non-finite pixels are reported as
	// such and painted a colour that cannot be mistaken for data, never silently
	// coerced: a non-finite value carries the largest possible exponent, so an
	// exponent-threshold darkness test calls it bright.
	void SetVisualise(std::string_view a_mode);
}
