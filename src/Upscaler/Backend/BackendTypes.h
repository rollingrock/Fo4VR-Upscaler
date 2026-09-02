#pragma once

// The vocabulary the backend seam speaks. Plain structs, plain enums, and the two
// helpers that read and write the fixed text buffers below. No vendor type appears
// here or in IBackend.h - that is the property that makes a backend swap a swap.
//
// This header belongs to upscaler_backends, which links neither CommonLibF4 nor F4SE
// and receives no precompiled header, so it includes everything it uses.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

namespace Upscaler::Backend
{
	// A pixel rectangle in the coordinate space of the resource it describes. Staged
	// colour, motion and output are zero-origin unless supportsSubrects permits otherwise.
	// Depth is independent: D3D11 cannot rectangle-copy a depth-stencil resource, so its
	// rect may name one eye in the whole renderer-owned texture. A backend that cannot
	// consume that depth extent must decline or convert it. Zero size means whole resource.
	struct Rect
	{
		std::uint32_t x = 0;
		std::uint32_t y = 0;
		std::uint32_t w = 0;
		std::uint32_t h = 0;
	};

	struct MotionVectorScale
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	// Converts normalized per-view UV offsets to render-resolution pixels. This is the
	// canonical seam scale; each backend translates it to the units its API requests.
	[[nodiscard]] constexpr MotionVectorScale NormalizedUvMotionScale(
		std::uint32_t a_renderWidth,
		std::uint32_t a_renderHeight) noexcept
	{
		return { static_cast<float>(a_renderWidth), static_cast<float>(a_renderHeight) };
	}

	// Reconstruction ratio, named the way the vendors name it. kNativeAA is ratio 1.0 -
	// it costs frames and buys clarity, it does not give frames back, and the shipped
	// default says so.
	enum class QualityMode
	{
		kNativeAA,
		kQuality,
		kBalanced,
		kPerformance,
		kUltraPerformance
	};

	// The documented default render scale for each mode - render extent over output
	// extent, per axis.
	//
	// VERIFIED (DLSS Programming Guide rev 310.6.0 section 3.7.1.1, the scaling-ratio row
	// of the jitter-phase table): the guide gives target/render ratios 1.0, 1.5, 1.724,
	// 2.0 and 3.0, and these are their reciprocals. This is the startup default, never
	// the source of truth - IBackend::QueryRenderSize asks the runtime itself, and a
	// disagreement is logged rather than corrected, because the engine's
	// dynamic-resolution ratio is a load-time, restart-only patch.
	[[nodiscard]] constexpr float DefaultRenderScale(QualityMode a_quality) noexcept
	{
		switch (a_quality) {
		case QualityMode::kQuality:
			return 0.6667f;
		case QualityMode::kBalanced:
			return 0.5800f;
		case QualityMode::kPerformance:
			return 0.5000f;
		case QualityMode::kUltraPerformance:
			return 0.3333f;
		case QualityMode::kNativeAA:
		default:
			return 1.0f;
		}
	}

	// What a backend advertises before it is chosen, and what the startup self-check
	// prints. Fixed char buffers rather than std::string: Caps is read from inside the
	// fault-guarded paths, and a frame that needs C++ unwinding cannot host a __try, so
	// the type stays POD.
	struct Caps
	{
		// The runtime is present AND this adapter supports it. Nothing else in this
		// struct is meaningful while this is false.
		bool available = false;

		// false means the stage layer MUST copy each eye into its own zero-origin
		// intermediate before Evaluate. This is the default answer, not the fallback:
		// no permitted source has reported a non-zero subrect base working on this
		// graphics API, and one runtime carries no subrect bases at any offset.
		bool supportsSubrects = false;

		// The backend must see device creation, so the plugin has to hook the game's
		// d3d11 import before the device exists. Declared on every backend, not only
		// the one that needs it, so turning that backend on is a registry decision
		// rather than a rewrite of the entry point.
		bool needsDeviceCreation = false;

		// An exposure texture is honoured. Not every preset of every runtime consumes
		// one, and handing over a texture that is ignored is not free.
		bool supportsExposure = false;

		// A reactive or transparency mask is honoured.
		bool supportsReactiveMask = false;

		// The backend sharpens its own output. false means run our own pass.
		bool ownsSharpening = false;

		// The version the runtime reported, not the version we asked for. Logged
		// verbatim, because a driver profile can substitute another.
		char runtimeVersion[64] = {};

		// The absolute path the runtime was loaded from. Resolved explicitly and never
		// by search order, so a stray copy elsewhere cannot quietly win a bug report.
		char runtimePath[260] = {};

		// Printed verbatim by the self-check when available is false. Truncation is
		// acceptable; an empty reason is not.
		char unavailableReason[192] = {};
	};

	// Per-eye dimensions, always. The backend is never told the double-wide size,
	// because it is never told there are two eyes.
	struct Config
	{
		std::uint32_t renderWidth = 0;
		std::uint32_t renderHeight = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;

		QualityMode quality = QualityMode::kNativeAA;

		// Deliberately an open string, never a compile-time list. Preset names have
		// been added, deprecated and removed inside a single runtime's lifetime; a
		// closed list is how an integration gets stuck one preset behind the model it
		// exists to expose. Empty means set nothing and let the runtime and its driver
		// profile decide, and the resolved value is logged.
		std::string preset;

		float sharpness = 0.0f;

		// Colour range of the input. Wrong here is washed-out or clipped output rather
		// than a failure, which is why it is a declared field and not an assumption.
		bool hdr = false;

		// Kept explicit so the depth convention is visible at the seam rather than
		// buried in a backend.
		bool invertedDepth = false;

		// The motion vectors still carry the jitter the projection applied. FO4VR's
		// logical target 35 does not, so its stage contract sets this false.
		bool jitteredMV = false;

		// Motion vectors are at render resolution rather than output resolution.
		bool lowResMV = true;

		// Ask the backend to derive exposure itself rather than consume an exposure
		// texture. FO4VR's shared adapted-luminance pair is produced later, inside
		// vanilla HDR, so the pre-HDR reconstruction path deliberately selects this
		// mode for v1 instead of feeding a frame-late texture with different semantics.
		bool autoExposure = true;

		// The input is not pre-exposed and no additional exposure scale is applied.
		// Keep both scalars explicit at the backend seam: NGX and Streamline consume
		// them even when they derive exposure internally, and silently defaulting them
		// independently in each backend is an avoidable colour-domain disagreement.
		float preExposure = 1.0f;
		float exposureScale = 1.0f;
	};

	// The closed v1 exposure contract. The HDR input is not pre-exposed, so both
	// scalars are deliberately exact rather than merely positive defaults.
	[[nodiscard]] constexpr bool IsBackendAutoExposureContract(
		const Config& a_config,
		ID3D11Resource* a_exposure) noexcept
	{
		return a_config.autoExposure && a_exposure == nullptr &&
		       a_config.preExposure == 1.0f && a_config.exposureScale == 1.0f;
	}

	// One eye's worth of work. Two of these are submitted per frame and the backend
	// never learns that the other exists - which is precisely why it cannot get stereo
	// wrong.
	struct EyeView
	{
		// 0 = left, 1 = right. Selects which reconstruction context is used.
		std::uint32_t index = 0;

		ID3D11Resource* color = nullptr;
		ID3D11Resource* depth = nullptr;
		ID3D11Resource* motionVectors = nullptr;
		ID3D11Resource* output = nullptr;

		// Optional. Passing null is normal and must never be an error.
		ID3D11Resource* exposure = nullptr;
		ID3D11Resource* reactiveMask = nullptr;
		ID3D11Resource* transparencyMask = nullptr;

		// Colour, motion and output are zero-origin unless supportsSubrects says otherwise.
		// Depth may reference a rectangle in a whole depth-stencil resource independently.
		Rect colorRect;
		Rect depthRect;
		Rect mvRect;
		Rect outputRect;

		// Sub-pixel offset in render-resolution pixels, the units every runtime's
		// evaluate contract is written in. Converting from whatever the engine stores
		// is the stage layer's job, and getting that conversion wrong produces a
		// picture that looks right and samples wrong - so it happens in one place,
		// above this seam.
		float jitterX = 0.0f;
		float jitterY = 0.0f;

		// Multipliers taking the motion-vector texture's stored units into
		// render-resolution pixels. The canonical direction is the sample's current
		// position to its previous-frame source position. A backend translates these
		// canonical units to its API: for example, Streamline receives {1,1} when the
		// stored values are already normalized rather than these pixel multipliers.
		float mvScaleX = 0.0f;
		float mvScaleY = 0.0f;

		float frameTimeDeltaMs = 0.0f;

		// Projection scalars in the same view-space units as depth reconstruction.
		// They are per-eye because VR projection matrices can be asymmetric. Backends
		// that do not consume them simply ignore them; FSR rejects a zero/invalid set.
		float cameraNear = 0.0f;
		float cameraFar = 0.0f;
		float cameraFovAngleVertical = 0.0f;
		float viewSpaceToMetersFactor = 0.0f;

		// Discard accumulated history. Both eyes are always reset together; resetting
		// one and not the other is a stereo-rivalry artifact, which is worse than the
		// ghost it fixed.
		bool resetHistory = false;
	};

	// Caps text fields are fixed buffers so the struct stays POD, which costs one
	// helper each way. They live here rather than in each backend so there is one
	// spelling: four copies of a four-line string copy is how a truncation bug ends up
	// in only one of them.
	inline void SetText(char* a_dst, std::size_t a_size, std::string_view a_text) noexcept
	{
		if (!a_dst || a_size == 0) {
			return;
		}

		const std::size_t n = a_text.size() < a_size - 1 ? a_text.size() : a_size - 1;
		if (n != 0) {
			std::memcpy(a_dst, a_text.data(), n);
		}
		a_dst[n] = '\0';
	}

	template <std::size_t N>
	inline void SetText(char (&a_dst)[N], std::string_view a_text) noexcept
	{
		SetText(a_dst, N, a_text);
	}

	// Reads a Caps text field back for logging. A buffer filled to the last byte has no
	// terminator, so the length is bounded here rather than trusted at the call site.
	[[nodiscard]] inline std::string_view Text(const char* a_src, std::size_t a_size) noexcept
	{
		if (!a_src || a_size == 0) {
			return {};
		}

		const auto* end = static_cast<const char*>(std::memchr(a_src, '\0', a_size));
		return end ? std::string_view{ a_src, static_cast<std::size_t>(end - a_src) } : std::string_view{ a_src, a_size };
	}

	template <std::size_t N>
	[[nodiscard]] inline std::string_view Text(const char (&a_src)[N]) noexcept
	{
		return Text(a_src, N);
	}
}
