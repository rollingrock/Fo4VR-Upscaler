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
	// It sees the ImageSpace chain plus the four screen-space-reflection passes, and
	// nothing else. The effect dispatch and the shared ImageSpace pixel-draw call are two
	// seams this plugin already owned; the reflection passes needed four more, because all
	// four SSLR shaders override the shared draw and are therefore invisible at it - a
	// census that reported no reflection pass before those hooks existed was describing
	// the seam, not the frame. World geometry - the water SURFACE and the deferred
	// composite among it - still draws through none of the six and does not appear.
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
		probe,
		// One of the four screen-space-reflection pixel passes. Its own kind rather than a
		// draw, because every earlier reading of this census quotes a draw count for the
		// ImageSpace chain and those passes are not in it.
		sslrDraw,
		// One BSWaterShader constant setup, recorded after the engine's own function has
		// written the water pixel constants. Its own kind for the same reason as sslrDraw:
		// it is world geometry, not the ImageSpace chain whose draw count every earlier
		// reading of this census quotes.
		waterSetup,
	};

	// Which of BSWaterShader's two constant-setup entries a waterSetup entry came from.
	// Both write the screen / dynamic-resolution float4 through the same shader-record
	// byte; only the geometry entry also writes TextureProj, and only it hands us the
	// constant group to read either of them out of.
	enum class WaterSite : std::uint32_t
	{
		none = 0,
		setupGeometry,   // BSWaterShader vtable slot 3, Addr::kWaterShaderSetupGeometryFn
		setupTechnique,  // BSWaterShader vtable slot 4, Addr::kWaterShaderSetupTechniqueFn
	};

	// Where a probe entry was taken. The census's draw entries answer what each pass ran
	// under; these answer what the state was between the passes, which is the only way to
	// tell an engine binding this plugin never had from one our own isolation bracket
	// dropped. Every site is a seam this tree already owns.
	enum class CensusProbeSite : std::uint32_t
	{
		none = 0,
		// The ImageSpace effect-dispatch hook, before HDRRoundTrip::OnDispatch runs. On a
		// reduced frame the whole pre-HDR transaction - every isolation bracket in it -
		// happens after this point and before postTransaction.
		preTransaction,
		// Inside the transaction's engine-colour bracket, immediately after Context::Begin
		// activated our private state object and before ClearState. Reduced path only, by
		// construction: on native the bracket never opens, and its absence is the answer.
		transactionIsolationBegan,
		// After that bracket's End swapped the host state object back in.
		transactionIsolationEnded,
		// The same dispatch hook, after HDRRoundTrip::OnDispatch returned.
		postTransaction,
		// The shared ImageSpace pixel-draw hook, before the draw. Pairs with the draw entry
		// the same hook records after it, so a DSV that appears or disappears across one
		// pass is attributable to that pass rather than to the gap before it.
		pixelDrawEntry,
		// After a reduced frame published at the TemporalAA seam, outside the seam draw's
		// isolation bracket - the point the rebindDepthStencilAfterSeam experiment acts at,
		// recorded whether or not that key is on.
		seamExit,
		// Before one of the four screen-space-reflection pixel passes, at that pass's own
		// prepared-draw call. Pairs with the sslrDraw entry the same hook records after
		// the draw, for the same reason pixelDrawEntry pairs with a draw: it separates
		// "the pass inherited no depth-stencil" from "the pass unbound one".
		sslrDrawEntry,
	};

	// Which of the four screen-space-reflection passes an sslrDraw entry belongs to. Each
	// is a separate vtable override with its own call site (Addresses.h carries the
	// derivation), so the pass is known from the site rather than guessed from what it
	// happened to bind.
	enum class SslrPass : std::uint32_t
	{
		none = 0,
		prepass,
		raytracing,
		blurH,
		blurV,
	};

	// What one BSWaterShader setup wrote into the water pixel shader's screen /
	// dynamic-resolution constant, read back out of the mapped constant array the engine
	// had just written, beside the two engine sources that constant is built from.
	//
	// Reading the constant rather than recomputing it is what makes the entry evidence:
	// a recomputed value would only prove that this plugin can multiply, while the four
	// floats below are the ones the water pixel shader is about to be handed. The source
	// fields are the cross-check - they say what the same float4 would hold if it were
	// rebuilt at the instant of the read, so a disagreement is a stale constant rather
	// than a wrong one.
	struct CensusWaterConstants
	{
		bool stateRead = false;      // the shader state's own pointers came back
		bool recordRead = false;     // the pixel-shader record pointer came back
		bool screenPresent = false;  // its register byte is not 0xFF
		bool screenRead = false;     // and the four floats behind it were readable
		std::uint32_t screenRegister = 0;  // the raw byte: 4 times the float4 register
		float invTargetWidth = 0.0f;
		float invTargetHeight = 0.0f;
		float invLiveWidthRatio = 0.0f;
		float invLiveHeightRatio = 0.0f;

		bool textureProjPresent = false;
		bool textureProjRead = false;
		std::uint32_t textureProjRegister = 0;

		// The two engine sources, read at this instant.
		std::int32_t sourceTargetLogical = -1;  // the id at viewState+0x1c
		std::uint32_t sourceTargetWidth = 0;
		std::uint32_t sourceTargetHeight = 0;
		bool ratiosValid = false;
		float liveWidthRatio = 0.0f;
		float liveHeightRatio = 0.0f;
	};

	// TextureProj in full: the two per-eye 4x4 matrices BSWaterShader writes for the water
	// surface's screen-projected texture coordinate. Kept out of CensusEntry and bounded to
	// a handful of samples because 32 floats on every one of 256 entries would be most of
	// the census's size for a value that does not vary within a frame.
	inline constexpr std::size_t kCensusWaterSamples = 8;

	struct CensusWaterSample
	{
		bool valid = false;
		std::uint32_t entryOrder = 0;
		std::array<float, 16> textureProjLeft{};
		std::array<float, 16> textureProjRight{};
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

		// probe only.
		CensusProbeSite probeSite = CensusProbeSite::none;

		// sslrDraw and the sslrDrawEntry probe.
		SslrPass sslrPass = SslrPass::none;

		// waterSetup only. waterSample indexes FrameCensus::waterSamples, or -1 when this
		// entry carried no TextureProj or the sample array was already full.
		WaterSite waterSite = WaterSite::none;
		std::int32_t waterSample = -1;
		CensusWaterConstants water{};

		// draw and probe.
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
		// Counted separately from draws so the frame's ImageSpace shape - the number every
		// earlier reading quotes - keeps its meaning when the reflection chain appears.
		std::uint32_t sslrDraws = 0;
		// Counted separately for the same reason: the water surface is world geometry and
		// is not part of the ImageSpace draw count either. Two counters, because the two
		// entries run at different rates and a reading that mixed them would be neither.
		std::uint32_t waterGeometrySetups = 0;
		std::uint32_t waterTechniqueSetups = 0;
		std::uint32_t count = 0;
		std::uint32_t threadId = 0;

		// The name table this census resolved against, snapshotted when the frame opened.
		std::array<CensusTargetInfo, kCensusColorTargets> colorTargets{};
		std::array<CensusTargetInfo, kCensusDepthTargets> depthTargets{};

		std::array<CensusEntry, kCensusEntries> entries{};
		std::array<CensusWaterSample, kCensusWaterSamples> waterSamples{};
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

	// Called by each of the four SSLR draw-call hooks after that pass's prepared draw
	// returns and before the shader's own cleanup unbinds - the same instant, and for the
	// same reason, as OnImageSpacePixelDraw. Render thread only; a no-op unless a census
	// is recording on this thread.
	//
	// It exists because the reflection chain overrides the shared ImageSpace draw and is
	// therefore invisible at the seam the census already owns: a census that records no
	// reflection pass at that seam is describing the seam, not the frame.
	void OnSslrPixelDraw(SslrPass a_pass) noexcept;

	// Called by each of the two BSWaterShader constant-setup hooks AFTER the engine's own
	// function has returned, which is the only instant at which the constants it just
	// wrote can be read. Render thread only; a no-op unless a census is recording on this
	// thread.
	//
	// a_shaderState is slot 3's third argument, the object carrying the pixel-shader
	// record and the three pixel constant groups; slot 4 does not receive it and passes
	// zero, which is why only a geometry entry carries constant values. a_viewState is
	// slot 3's fourth argument, the object whose +0x1c is the render-target id the engine
	// built the screen half of the constant from. Every read through either pointer is
	// fault-guarded: these are engine structures reached by offset, and an offset that is
	// wrong on some future build must produce an unread field, not a crash.
	// a_onRenderThread is the caller's own answer to "am I the thread ClearColor runs
	// on", which decides whether this sample may touch the D3D11 immediate context at
	// all. It is passed in rather than decided here because Hooks owns that identity.
	void OnWaterShaderSetup(
		WaterSite a_site,
		std::uintptr_t a_shaderState,
		std::uintptr_t a_viewState,
		bool a_onRenderThread) noexcept;

	// A run of samples taken at BSWaterShader's constant setup, armed and published like
	// the UI-entry run above and for the same reason: the water surface draws through no
	// seam this plugin owned before step 9b, so the constants it runs under are readable
	// only at its own entry, and one sample cannot tell a per-frame constant from a
	// per-draw one.
	//
	// It exists BESIDE the census entry rather than instead of it because the two answer
	// different questions. A census entry is thread-checked against the frame the census
	// opened, which is the right rule for the ImageSpace chain; a water run records
	// whichever thread the setup actually runs on, and says which that was - so a run
	// that comes back with samples from a thread the census could never record on is
	// itself the finding, not a failure.
	inline constexpr std::size_t kWaterSamples = 32;

	// How much of each of the water pixel shader's three constant groups a sample dumps.
	// A constant's position in the mapped array is a shader-record byte, so no water pixel
	// constant can live past float 255; dumping the whole addressable range is what turns
	// "the two registers we looked at agree" into "no water pixel constant differs".
	inline constexpr std::size_t kWaterGroupFloats = 256;
	inline constexpr std::size_t kWaterGroups = 3;

	struct WaterSample
	{
		std::uint64_t setupCall = 0;  // the session-wide counter, so a gap in the run shows
		WaterSite site = WaterSite::none;
		std::uint32_t threadId = 0;
		bool onRenderThread = false;

		CensusWaterConstants water{};
		std::array<float, 16> textureProjLeft{};
		std::array<float, 16> textureProjRight{};

		// The three pixel constant groups in full, read in four-float chunks so a group
		// that ends before the dump does truncates instead of voiding the whole read.
		// groupFloats says how many floats of each were actually readable.
		std::array<std::uint32_t, kWaterGroups> groupFloats{};
		std::array<std::array<float, kWaterGroupFloats>, kWaterGroups> groups{};

		// Everything below is read from the D3D11 immediate context and is therefore
		// recorded ONLY when onRenderThread is true. bindingsRead says which it was, so an
		// all-zero binding block is never mistaken for an unbound pipeline.
		bool bindingsRead = false;
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
		std::uint32_t srvBoundCount = 0;
		std::uint32_t srvBoundAboveScan = 0;
		std::array<bool, kCensusSrvSlots> srvBound{};
		std::array<std::int32_t, kCensusSrvSlots> srvColorLogical{};
		std::array<std::int32_t, kCensusSrvSlots> srvDepthLogical{};
		std::array<CensusSrvDesc, kCensusSrvSlots> srvDesc{};
	};

	struct WaterObservation
	{
		std::uint64_t requestedSequence = 0;
		std::uint64_t completedSequence = 0;
		std::uint32_t requestedSamples = 0;
		std::uint32_t recorded = 0;
		std::uint32_t threadId = 0;
		std::uint64_t setupCalls = 0;  // every water setup seen this session, armed or not
		bool armed = false;
		std::array<WaterSample, kWaterSamples> samples{};
	};

	// Arms one run of up to kWaterSamples consecutive water setups. A count of 0 or above
	// kWaterSamples is clamped into range; the returned sequence is how a caller tells its
	// own result from the previous one.
	[[nodiscard]] std::uint64_t RequestWaterObservation(std::uint32_t a_samples) noexcept;

	// Thread-safe copy of the last completed run. Tens of kilobytes, so the caller owns
	// the storage.
	void CopyWaterObservation(WaterObservation& a_out) noexcept;

	// One output-merger probe between the passes: the bound colour targets and the
	// depth-stencil view, resolved to logical ids against the same one-frame table the
	// draw entries use. No shader-resource scan - a probe is about what the output merger
	// holds when nothing is drawing. Render thread only, and a no-op unless a census is
	// recording on this thread, so a shipped session pays one relaxed load per site.
	// a_pass names the reflection pass for a sslrDrawEntry probe and stays none at every
	// other site, so one probe entry can carry which pass it precedes.
	void OnSeamStateProbe(CensusProbeSite a_site, SslrPass a_pass = SslrPass::none) noexcept;

	// Thread-safe copy of the last completed census. The structure is tens of kilobytes,
	// so the caller owns the storage.
	void CopyFrameCensus(FrameCensus& a_out) noexcept;

	// A run of samples taken at the UI render entry - the Interface3D call at
	// Addr::kUiRenderAllCall, inside US_RenderWorldImageSpaceSequence, 0x43 bytes after
	// the late ImageSpace range where EndReducedWorld restores the native ratios and
	// therefore after this plugin's last seam. (An earlier version of this comment named
	// the call in Swap that runs the same renderer list "after every DrawWorld_And_UI".
	// That site is the measured negative, not this one - both DrawWorld_And_UI calls in
	// Swap branch past it, so it is the no-world path; Addresses.h carries the derivation
	// and the counters.)
	//
	// This answers what the census cannot: the census sees the ImageSpace chain, and the
	// HUD is not in it. Nor can either of this plugin's capture points carry the HUD, by
	// construction rather than by measurement - both are taken at or inside the late
	// range, which this call follows, so a compass in one of those images would be the
	// surprise. What the UI draw actually runs under - the two engine projection-jitter
	// floats, the four dynamic-resolution ratios, the ImageSpace geometry selector, the
	// hidden-area byte, the viewport and the target - is only readable at the UI entry
	// itself.
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
