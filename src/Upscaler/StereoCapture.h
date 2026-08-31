#pragma once

// One-shot readback of the submitted double-wide TemporalAA output. The render
// thread owns the D3D11 copy and map; the devbench listener owns file output.

#include "Upscaler/HDRRoundTrip.h"

#include <cstdint>
#include <string>

namespace Upscaler::StereoCapture
{
	inline constexpr std::uint32_t kAnyJitterPhase = 0xFFFFFFFFu;

	struct EyeStats
	{
		double darkPct = 0.0;
		double meanLuma = 0.0;
	};

	struct MotionStats
	{
		double zeroPct = 0.0;
		double nonFinitePct = 0.0;
		double minX = 0.0;
		double maxX = 0.0;
		double minY = 0.0;
		double maxY = 0.0;
		double meanAbsX = 0.0;
		double meanAbsY = 0.0;
	};

	struct DepthStats
	{
		double farPct = 0.0;
		double minDepth = 0.0;
		double maxDepth = 0.0;
	};

	struct Status
	{
		std::uint64_t requestedSequence = 0;
		std::uint64_t completedSequence = 0;
		bool pending = false;
		bool captured = false;
		bool saved = false;
		bool inputsRequested = false;
		bool inputsCaptured = false;
		bool inputsSaved = false;
		bool reassembledRequested = false;
		bool reassembledCaptured = false;
		bool reassembledSaved = false;
		bool ffxHandbackRequested = false;
		bool ffxGateExhausted = false;
		bool bypassed = false;
		bool overwritten = false;
		std::uint64_t temporalAACall = 0;
		std::uint64_t ngxHandbackSerial = 0;
		std::uint64_t requestedFfxHandbackSerial = 0;
		std::uint64_t ffxHandbackSerial = 0;
		std::uint64_t requestedPrivateReassemblySerial = 0;
		std::uint64_t privateReassemblySerial = 0;
		std::uint32_t requestedSkipBypasses = 0;
		std::uint32_t skippedBypasses = 0;
		std::uint32_t requestedSkipHandbacks = 0;
		std::uint32_t skippedHandbacks = 0;
		std::uint32_t requestedJitterPhase = kAnyJitterPhase;
		std::uint32_t jitterPhase = kAnyJitterPhase;
		std::uint32_t skippedJitterPhases = 0;
		std::uint32_t threadId = 0;
		std::uint32_t eyeWidth = 0;
		std::uint32_t eyeHeight = 0;
		std::uint32_t format = 0;
		std::uint32_t d3dHr = 0;
		EyeStats left{};
		EyeStats right{};
		MotionStats leftMotion{};
		MotionStats rightMotion{};
		DepthStats leftDepth{};
		DepthStats rightDepth{};
		EyeStats leftReassembled{};
		EyeStats rightReassembled{};
		std::string leftPath;
		std::string rightPath;
		std::string leftInputColorPath;
		std::string rightInputColorPath;
		std::string leftNgxOutputPath;
		std::string rightNgxOutputPath;
		std::string leftReassembledPath;
		std::string rightReassembledPath;
		std::string leftMotionPath;
		std::string rightMotionPath;
		std::string leftDepthPath;
		std::string rightDepthPath;
		std::string error;
	};

	// Arms one capture. A newer request replaces an older pending request. When
	// a_requireBypass is true, native and post-draw-overwrite frames are ignored.
	// a_skipBypasses then discards that many accepted bypasses before readback.
	// A phase from 0 through 7 waits for that exact verified native jitter phase;
	// kAnyJitterPhase captures the first otherwise-accepted frame.
	// a_includeInputs additionally captures the per-eye staged vendor color,
	// motion, output and the matching engine depth plane for diagnosis.
	// a_waitForInputs leaves the request armed until those private resources exist;
	// use it when a one-shot gate will create them on a later frame.
	// a_includeReassembled additionally waits for the next accepted private double-wide
	// reassembly and captures its two physical halves before render-thread teardown.
	[[nodiscard]] std::uint64_t Request(
		bool a_requireBypass = false,
		std::uint32_t a_skipBypasses = 0,
		std::uint32_t a_jitterPhase = kAnyJitterPhase,
		bool a_includeInputs = false,
		bool a_waitForInputs = false,
		bool a_includeReassembled = false) noexcept;

	// Arms an exact accepted FFX continuous handback capture. The request baselines
	// the per-source serial, can skip later accepted handbacks, and captures the
	// submitted frame plus the matching per-eye inputs, raw outputs, motion and depth.
	[[nodiscard]] std::uint64_t RequestFfx(
		std::uint32_t a_skipHandbacks = 0,
		std::uint32_t a_jitterPhase = kAnyJitterPhase) noexcept;

	// Called after the TemporalAA draw or its accepted replacement and before the
	// outer image-space cleanup. It never waits on the reporting thread.
	void OnTemporalAADraw(
		std::uint64_t a_temporalAACall,
		std::uint32_t a_threadId,
		bool a_bypassed,
		bool a_overwritten,
		HDRRoundTrip::HandbackStamp a_handback) noexcept;

	// Thread-safe listener-thread snapshot. The first call after a successful
	// readback writes the two eye images and releases their CPU pixel storage.
	[[nodiscard]] Status SaveAndSnapshot();
}
