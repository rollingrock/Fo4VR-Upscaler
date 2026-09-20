#pragma once

// Private workshop-outline mask remap. The pass reads an engine-owned RGBA8
// texture through a caller-retained SRV and writes only a module-owned texture.
// It never changes an engine render-target record or the source resource.

#include "Upscaler/WorkshopMaskShader.h"

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace Upscaler::WorkshopMask
{
	enum class PrepareResult : std::uint32_t
	{
		prepared = 0,
		declined = 1,
		hostStateUncertain = 2,
	};

	// The last ordinary reason Prepare declined. Host-state uncertainty is reported by
	// PrepareResult instead: callers must treat it as terminal for every shared-context
	// feature rather than as an optional mask-remap miss.
	enum class Decline : std::uint32_t
	{
		none = 0,
		invalidArguments = 1,
		notImmediateContext = 2,
		deviceMismatch = 3,
		sourceIdentity = 4,
		sourceDescriptor = 5,
		sourceView = 6,
		geometry = 7,
		identityGeometry = 8,
		resourceCreation = 9,
		isolation = 10,
		privateDraw = 11,
		failureLatched = 12,
		wrongThread = 13,
	};

	struct Snapshot
	{
		std::uint64_t attempts = 0;
		std::uint64_t prepared = 0;
		std::uint64_t declines = 0;
		std::uint64_t resourceBuilds = 0;
		std::uint64_t resourceFailures = 0;
		std::uint64_t draws = 0;
		std::uint64_t drawFailures = 0;
		std::uint64_t isolationFailures = 0;
		std::uint64_t hostStateUncertain = 0;
		std::uint64_t releases = 0;
		std::uint32_t lastDecline = 0;  // a Decline value
		std::uint32_t exceptionCode = 0;
		std::int32_t lastHr = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		std::uint32_t format = 0;
		bool ready = false;
		bool outputValid = false;
		bool failureLatched = false;
	};

	// Render thread. The source texture and view remain caller-owned. The geometry must
	// describe two contiguous source-eye rectangles in a texture whose allocation is
	// exactly the full output allocation. A prepared result publishes one private SRV.
	[[nodiscard]] PrepareResult Prepare(
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_sourceTexture,
		ID3D11ShaderResourceView* a_sourceSrv,
		const WorkshopMaskShader::Constants& a_geometry) noexcept;

	// Borrowed. Non-null only after Prepare returned prepared, and valid until the next
	// Prepare call, Release, or serviced release request. The module owns the reference.
	[[nodiscard]] ID3D11ShaderResourceView* Output() noexcept;

	// Render thread. Drops module-owned objects and clears bounded failure latches. If
	// shared host state is already uncertain, objects are retained rather than released
	// out from under an unprovable context and the request remains pending.
	void Release() noexcept;

	// Any thread. The render thread services this before its next Prepare, or explicitly
	// through ReleaseIfRequested when the correction is disabled and Prepare does not run.
	void RequestRelease() noexcept;
	void ReleaseIfRequested() noexcept;

	[[nodiscard]] Snapshot GetSnapshot() noexcept;
}
