#pragma once

// Bounded replacement of native post-tonemap TemporalAA's two outputs.
//
// The accepted post-draw mode calls Bethesda's prepared helper first and overwrites its
// still-bound outputs. The stricter bypass mode resolves the engine's history parity,
// validates color 3, submitted target 4 and the expected 61/62 output directly, then
// performs the same private draw instead of the prepared helper. Both hand the exact
// host D3D11 state back before the outer image-space cleanup continues. They are
// mutually exclusive diagnostic gates, off by default and bounded to 512 draws.

#include <cstdint>

namespace Upscaler::TAAReplacement
{
	struct State
	{
		bool enabled = false;
		bool postDrawEnabled = false;
		bool bypassEnabled = false;
		bool requireNgxHandback = false;
		bool ready = false;
		bool completed = false;
		bool sessionDisabled = false;
		std::uint64_t attempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t noHandbackSkips = 0;
		std::uint64_t validationFailures = 0;
		std::uint64_t isolationFailures = 0;
		std::uint32_t validationMask = 0;
		std::uint64_t shaderBuilds = 0;
		std::uint64_t history61Writes = 0;
		std::uint64_t history62Writes = 0;
		std::uint64_t postDrawOverwrites = 0;
		std::uint64_t nativeDrawBypasses = 0;
		std::uint64_t parityReadAttempts = 0;
		std::uint64_t parityReadFailures = 0;
		std::uint64_t parityInvalidValues = 0;
		std::uint64_t currentRenderTargetMismatches = 0;
		std::uint64_t targetSuccesses = 0;
		std::uint64_t lastTemporalAACall = 0;
		std::uint64_t lastHandbackSerial = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t sourceFormat = 0;
		std::uint32_t historyFormat = 0;
		std::uint32_t outputFormat = 0;
		std::uint32_t lastParity = 0xFFFFFFFFu;
		std::int32_t expectedHistoryLogical = -1;
		std::int32_t engineHistoryLogical = -1;
		std::int32_t deviceRemovedReason = 0;
		std::uint32_t lastError = 0;
		std::uintptr_t context = 0;
		std::uintptr_t sourceSrv = 0;
		std::uintptr_t historyRtv = 0;
		std::uintptr_t outputRtv = 0;
		std::uintptr_t parityAddress = 0;
	};

	void Configure(
		bool a_postDrawEnabled,
		bool a_bypassEnabled,
		bool a_requireNgxHandback) noexcept;

	// Called before Bethesda's prepared draw. True means the private pass completed far
	// enough that the caller must skip the native helper; false means vanilla must run.
	// A failure before Draw is always false. A failure after Draw is true because running
	// the native helper against an uncertain restored state would not be fail-soft.
	[[nodiscard]] bool TryBypass(
		std::uint64_t a_temporalAACall,
		std::uint64_t a_ngxHandbackSerial) noexcept;

	// Called only after the shared ImageSpace pixel-draw hook has run Bethesda's prepared
	// draw while its thread-local TemporalAA scope is active. True means the private MRT
	// overwrite ran; false leaves the already-produced vanilla outputs unchanged.
	[[nodiscard]] bool TryOverwrite(
		std::uint64_t a_temporalAACall,
		std::uint64_t a_ngxHandbackSerial) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
