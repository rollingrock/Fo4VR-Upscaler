#pragma once

// The per-frame orchestrator, and the only file besides StereoLayout that knows there
// are two eyes. Its first live gate is deliberately colour-only and NullBackend-only:
// unverified temporal inputs stay absent instead of receiving plausible semantics.

#include "Upscaler/Backend/IBackend.h"

#include <cstdint>

#include <d3d11.h>

namespace Upscaler::Evaluate
{
	enum class FailureStep : std::uint32_t
	{
		kNone,
		kInvalidArguments,
		kUnsupportedBackend,
		kUnsupportedConfig,
		kIntermediates,
		kBackendInitialize,
		kBackendCreateViews,
		kStereoGeometry,
		kExtractLeft,
		kEvaluateLeft,
		kReassembleLeft,
		kExtractRight,
		kEvaluateRight,
		kReassembleRight,
	};

	struct State
	{
		bool prepared = false;
		bool active = false;
		std::uint64_t attempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t failures = 0;
		std::uint64_t leftEvaluates = 0;
		std::uint64_t rightEvaluates = 0;
		std::uint32_t prepareCount = 0;
		FailureStep lastFailureStep = FailureStep::kNone;
		std::uintptr_t backend = 0;
		std::uintptr_t device = 0;
		std::uintptr_t context = 0;
	};

	// Sequence: validate the native colour-only control-arm contract, ensure the four
	// descriptor-keyed intermediates, extract left/right, submit one zero-origin eye at
	// a time, then reassemble into a private double-wide output. The caller owns exact
	// D3D11 state isolation and copies the completed private output back only on success.
	//
	// This gate accepts only backend "null", native 1:1 dimensions, and no temporal
	// inputs. That restriction is load-bearing: M3 and the jitter/exposure measurements
	// are still open, so a real reconstruction backend must not cross this function yet.
	[[nodiscard]] bool Frame(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWideColor,
		ID3D11Texture2D* a_doubleWideOutput,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const Backend::Config& a_config) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
	[[nodiscard]] bool Active() noexcept;

	// Render-thread teardown. Safe to call twice.
	void Reset() noexcept;
}
