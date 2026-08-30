#pragma once

// Bounded post-draw overwrite of native post-tonemap TemporalAA's two outputs.
//
// The call-site hook enters after Bethesda has bound color 3 plus both TAA MRTs and
// before its normal shader/output cleanup. Bethesda's prepared helper performs the
// binding as part of its draw, so the diagnostic calls vanilla first, then validates
// those live bindings,
// converts the post-tonemap R11G11B10 source to both R8G8B8A8 outputs with a private
// full-screen pixel pass, and hands the exact host D3D11 state back before the caller's
// cleanup continues. It is a diagnostic gate, off by default and bounded to 512 draws.

#include <cstdint>

namespace Upscaler::TAAReplacement
{
	struct State
	{
		bool enabled = false;
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
		std::uint64_t targetSuccesses = 0;
		std::uint64_t lastTemporalAACall = 0;
		std::uint64_t lastHandbackSerial = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t sourceFormat = 0;
		std::uint32_t historyFormat = 0;
		std::uint32_t outputFormat = 0;
		std::int32_t deviceRemovedReason = 0;
		std::uint32_t lastError = 0;
		std::uintptr_t context = 0;
		std::uintptr_t sourceSrv = 0;
		std::uintptr_t historyRtv = 0;
		std::uintptr_t outputRtv = 0;
	};

	void Configure(bool a_enabled, bool a_requireNgxHandback) noexcept;

	// Called only after the shared ImageSpace pixel-draw hook has run Bethesda's prepared
	// draw while its thread-local TemporalAA scope is active. True means the private MRT
	// overwrite ran; false leaves the already-produced vanilla outputs unchanged.
	[[nodiscard]] bool TryOverwrite(
		std::uint64_t a_temporalAACall,
		std::uint64_t a_ngxHandbackSerial) noexcept;

	[[nodiscard]] State Snapshot() noexcept;
}
