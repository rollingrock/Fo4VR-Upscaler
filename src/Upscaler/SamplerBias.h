#pragma once

// The mechanism half of the mip-LOD-bias feature: everything about a flat table of 320
// ID3D11SamplerState* that does not need to know where in Fallout4VR.exe that table is.
// MipBias owns the other half - the anchors, the settings and the render-path seams.
//
// The split is not decoration. This half links neither CommonLibF4 nor F4SE, so
// tools/subrect-harness drives it against a synthetic table on a real device with no
// game running, which is the only way the apply/restore contract below gets tested at
// all. Pointing it at a table is the caller's responsibility and MipBias is the only
// caller that resolves one from the binary.
//
// Two ownership invariants, both forced by the engine rather than chosen:
//
//  - The originals are engine-owned and are never Released by us. One engine caller
//    copies sampler pointers out of the table by value into an array whose lifetime was
//    not established, so a released original could outlive the slot it came from.
//  - Our duplicates are held for the process lifetime for the same reason, and because
//    the engine's own shutdown releaser walks the table and releases whatever it finds
//    there. Restore must therefore run before that releaser, and it swaps pointers
//    rather than dropping references. A superseded build's duplicates are retired, not
//    released.
//
// Threading: Initialize, Build, Apply and Restore are render-thread only - Build calls
// D3D11, and Apply/Restore write memory the render thread is about to read. Snapshot is
// safe from any thread and reads counters only.
//
// The technique - rewrite the engine's own table in place, exempt by descriptor filter
// class, and bracket the swap around the world pass - follows
// `fallout4-community-shaders-reference/features/Upscaling/src/SamplerBias.cpp` and
// `Upscaling.cpp` (GPL-3.0-or-later). The shape of the idea only; no line was copied,
// and the exemption set, the duplicate count, the guarded swap and the failure
// vocabulary here are ours. See SOURCES.md.

#include <cstddef>
#include <cstdint>

struct ID3D11Device;

namespace Upscaler::Backend
{
	class IBackend;
}

namespace Upscaler::SamplerBias
{
	// The creation loop's own bounds, in the order the index formula uses them.
	inline constexpr std::size_t kFilterCount = 5;
	inline constexpr std::size_t kAddressModeCount = 4;
	inline constexpr std::size_t kMipCount = 16;
	inline constexpr std::size_t kEntryCount = kFilterCount * kAddressModeCount * kMipCount;

	static_assert(kEntryCount == 320, "the engine builds exactly 5 * 4 * 16 sampler states");

	// The five D3D11_FILTER values the creation loop's filter index maps to, in index
	// order. Only ANISOTROPIC is biased by default: index 4 is shadow comparison and must
	// never move, and indices 0 and 1 carry MaxLOD == MinLOD == mip, which makes a
	// negative bias arithmetically dead there.
	inline constexpr std::uint32_t kFilterPoint = 0x00;           // MIN_MAG_MIP_POINT
	inline constexpr std::uint32_t kFilterLinearMipPoint = 0x14;  // MIN_MAG_LINEAR_MIP_POINT
	inline constexpr std::uint32_t kFilterTrilinear = 0x15;       // MIN_MAG_MIP_LINEAR
	inline constexpr std::uint32_t kFilterAnisotropic = 0x55;     // ANISOTROPIC
	inline constexpr std::uint32_t kFilterComparison = 0x94;      // COMPARISON_MIN_MAG_LINEAR_MIP_POINT

	// VERIFIED: FO4VR writes MipLODBias 0.0f into the single descriptor it reuses for all
	// 320 states, outside every loop, and no INI setting reaches that field. This is the
	// vendor formula's NativeBias term, measured rather than assumed.
	inline constexpr float kNativeBias = 0.0f;

	// The vendor formula spells the last term "+ epsilon" and gives no value. A 1/256
	// nudge moves the result toward the less aggressive end of the guide's own band,
	// which is the end that flickers less.
	inline constexpr float kEpsilon = 1.0f / 256.0f;

	// The TOML sentinel meaning "derive it". 0.0 is a legitimate bias, which is why the
	// sentinel is not 0.
	inline constexpr float kOverrideSentinel = 999.0f;

	enum class Failure : std::uint32_t
	{
		kNone,
		kUninitialized,
		kNoDevice,
		kInvalidExtent,
		kNoBias,
		kTableReadFault,
		kNullSlot,
		kIdentityChanged,
		kFilterHistogram,
		kCreateSamplerState,
		kApplyIdentityMismatch,
		kSwapFault,
	};

	struct State
	{
		bool initialized = false;
		bool originalsCached = false;
		bool built = false;
		bool applied = false;
		bool overrideActive = false;
		bool includeTrilinear = false;
		bool lowerMinLod = false;
		std::uintptr_t tableAddress = 0;
		// Counts of the five expected D3D11_FILTER values in index order, plus whatever
		// did not match one of them. A build with anything but 64/64/64/64/64/0 is a
		// different binary or a table another plugin has already rewritten.
		std::uint32_t filterHistogram[kFilterCount] = {};
		std::uint32_t filterOther = 0;
		std::uint32_t entriesTotal = static_cast<std::uint32_t>(kEntryCount);
		std::uint32_t entriesBiased = 0;
		std::uint32_t samplersCreated = 0;
		std::uint32_t retainedSamplers = 0;
		std::uint32_t builtRenderWidth = 0;
		std::uint32_t builtOutputWidth = 0;
		float builtBias = 0.0f;
		float guideBandLow = 0.0f;
		float guideBandHigh = 0.0f;
		std::uint64_t builds = 0;
		std::uint64_t buildDeclines = 0;
		std::uint64_t appliedFrames = 0;
		std::uint64_t restoredFrames = 0;
		std::uint64_t applyDeclines = 0;
		std::uint64_t unbalanced = 0;
		std::uint64_t bandWarnings = 0;
		Failure lastFailure = Failure::kNone;
	};

	// NativeBias + the backend's own log2(render/output) - 1.0 + epsilon. The -1.0 lives
	// inside the backend because vendors disagree about it; see SOURCES.md for the open
	// decision on whether FSR should keep it.
	[[nodiscard]] float Derive(
		const Backend::IBackend& a_backend,
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth) noexcept;

	// The vendor guide's own band: log2(r/d) - 1.0 at the aggressive end, log2(r/d) at
	// the end it says not to go above when chasing flicker and moire. An override outside
	// it is honoured and warned about, never clamped.
	[[nodiscard]] float GuideBandLow(std::uint32_t a_renderWidth, std::uint32_t a_outputWidth) noexcept;
	[[nodiscard]] float GuideBandHigh(std::uint32_t a_renderWidth, std::uint32_t a_outputWidth) noexcept;
	[[nodiscard]] bool InGuideBand(float a_bias, std::uint32_t a_renderWidth, std::uint32_t a_outputWidth) noexcept;

	// Binds a table base and a device. Caches nothing and calls no D3D11 method, so it is
	// safe before the renderer has built anything. Re-binding a different base forgets the
	// previous build without releasing a single sampler.
	[[nodiscard]] bool Initialize(std::uintptr_t a_tableBase, ID3D11Device* a_device) noexcept;

	// Caches the 320 originals, checks the descriptor filter histogram, creates one
	// biased duplicate per marked entry and assembles the table image Apply writes.
	// Idempotent: an unchanged (renderWidth, outputWidth, bias, includeTrilinear,
	// lowerMinLod) tuple returns true having done nothing. A failure leaves nothing
	// applied and nothing installed, and is retried by the next call.
	//
	// a_overrideBias is kOverrideSentinel to derive from a_backend, which may then not be
	// null. Any other value is used verbatim.
	[[nodiscard]] bool Build(
		const Backend::IBackend* a_backend,
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth,
		float a_overrideBias,
		bool a_includeTrilinear,
		bool a_lowerMinLod) noexcept;

	// Pointer swaps, both noexcept, both render-thread only, neither touching D3D11, the
	// log or the settings. Apply verifies that every slot it is about to overwrite still
	// holds the pointer it cached and declines the frame rather than writing blind.
	// Restore is infallible by contract: it runs on every path that can leave the world
	// frame, and leaving a duplicate in the table would hand our object to the engine's
	// shutdown releaser.
	void Apply() noexcept;
	void Restore() noexcept;

	// A duplicate set exists and Apply would install it. False before the first
	// successful Build and after one that declined.
	[[nodiscard]] bool Built() noexcept;

	// Render-thread teardown of the built image: Restore first, then drop it, so Built()
	// reads false afterwards. The duplicates themselves are never released - a pointer
	// that was ever installed may still be held by value in an engine-side copy - so this
	// only retires them, exactly as a rebuild does. Idempotent.
	void ForgetBuild() noexcept;

	[[nodiscard]] bool Applied() noexcept;

	// Whether a table index is in the set Apply rewrites. For the sampler probe, which
	// reports what the engine actually binds.
	[[nodiscard]] bool IsBiased(std::size_t a_index) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
