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
