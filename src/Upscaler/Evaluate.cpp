#include "Upscaler/Evaluate.h"

#include "Upscaler/CameraMotion.h"
#include "Upscaler/Common.h"
#include "Upscaler/FoveationCrop.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/Intermediates.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/MipBias.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/StereoCapture.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/Telemetry.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Upscaler::Evaluate
{
	namespace
	{
		struct ConfigKey
		{
			std::uint32_t renderWidth = 0;
			std::uint32_t renderHeight = 0;
			std::uint32_t outputWidth = 0;
			std::uint32_t outputHeight = 0;
			Backend::QualityMode quality = Backend::QualityMode::kNativeAA;
			std::uint32_t sharpnessBits = 0;
			std::uint32_t presetLength = 0;
			std::uint32_t runtimeDirLength = 0;
			bool hdr = false;
			bool invertedDepth = false;
			bool jitteredMV = false;
			bool lowResMV = false;
			bool autoExposure = false;
			// Part of the key because NGX honours an output base only when told at
			// creation - VERIFIED by tools/subrect-harness, where a feature created
			// without the flag answers InvalidParameter to any non-zero base
			// (BackendTypes.h cites the guide section) - so a flip must re-create the
			// views, not just re-address them.
			bool subrects = false;
			// The crop's extent is a creation property (BackendTypes.h), so a size change
			// must re-create the views like any other extent change. Its base is not here:
			// a base is a per-evaluate parameter and never re-prepares.
			std::uint32_t cropWidth = 0;
			std::uint32_t cropHeight = 0;
			// Phase 2a's placement is not a creation property, and NGX keeps its views across
			// it (its SameConfig leaves it out). It is here so that a live switch between the
			// measurement crop and foveated DLAA's box at the same extent still passes the
			// preparation, which rebuilds the mip bias for it (MipBias::Build).
			bool cropProjection = false;
			std::uint32_t preExposureBits = 0;
			std::uint32_t exposureScaleBits = 0;
			char preset[260] = {};
			char runtimeDir[1024] = {};
		};

		Backend::IBackend* g_backend = nullptr;    // borrowed singleton
		ID3D11Device* g_device = nullptr;          // borrowed, render-thread only
		ID3D11DeviceContext* g_context = nullptr;  // borrowed, render-thread only
		ConfigKey g_key{};
		bool g_hasKey = false;

		std::atomic<bool> g_prepared{ false };
		std::atomic<bool> g_active{ false };
		std::atomic<bool> g_ownsBackendState{ false };
		// The runtime under the views, which outlives them. True from a successful
		// Initialize until a Shutdown is attempted; the identity above says which backend,
		// device, context and runtime directory it belongs to. `prepared` describes the
		// views, and the two move independently: ReleaseViews leaves this true with
		// nothing prepared, which is the state an operator's toggle back on re-enters
		// without paying for an SDK initialisation.
		std::atomic<bool> g_runtimeUp{ false };
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_successes{ 0 };
		std::atomic<std::uint64_t> g_failures{ 0 };
		std::atomic<std::uint64_t> g_leftEvaluates{ 0 };
		std::atomic<std::uint64_t> g_rightEvaluates{ 0 };
		std::atomic<std::uint64_t> g_leftReassemblies{ 0 };
		std::atomic<std::uint64_t> g_rightReassemblies{ 0 };
		std::atomic<std::uint32_t> g_prepareCount{ 0 };
		std::atomic<std::uint64_t> g_autoExposureSubmissions{ 0 };
		std::atomic<std::uint64_t> g_exposureContractFailures{ 0 };
		std::atomic<std::uint64_t> g_frameTimeSamples{ 0 };
		std::atomic<std::uint64_t> g_frameTimeFallbacks{ 0 };
		std::atomic<float> g_frameTimeDeltaMs{ 0.0f };
		std::atomic<FailureStep> g_lastFailureStep{ FailureStep::kNone };
		std::atomic<std::uintptr_t> g_backendAddress{ 0 };
		std::atomic<std::uintptr_t> g_deviceAddress{ 0 };
		std::atomic<std::uintptr_t> g_contextAddress{ 0 };
		// The backend lifecycle. Written only by the render thread inside
		// PrepareInternal and Reset, read from any thread by LifecycleSnapshot.
		std::atomic<std::uint64_t> g_initializeCount{ 0 };
		std::atomic<std::uint64_t> g_initializeSuccesses{ 0 };
		std::atomic<std::uint64_t> g_createViewsCount{ 0 };
		std::atomic<std::uint64_t> g_destroyViewsCount{ 0 };
		std::atomic<std::uint64_t> g_shutdownCount{ 0 };
		std::atomic<std::uint64_t> g_viewsOnlyPreparations{ 0 };
		std::atomic<std::uint64_t> g_fullPreparations{ 0 };
		std::atomic<std::uint64_t> g_viewsReleases{ 0 };
		std::atomic<float> g_lastInitializeMs{ 0.0f };
		std::atomic<float> g_lastCreateViewsMs{ 0.0f };
		std::atomic<float> g_lastDestroyViewsMs{ 0.0f };
		std::atomic<float> g_lastShutdownMs{ 0.0f };
		std::atomic<float> g_lastPrepareMs{ 0.0f };
		std::atomic<bool> g_lastPrepareViewsOnly{ false };
		// The crop of the last successful frame (CropSnapshot), and the render thread's own
		// path position: the frame index restarts whenever the crop's extent or motion
		// changes, so a moving run always starts from the same base.
		std::atomic<bool> g_cropActive{ false };
		std::atomic<bool> g_cropMoving{ false };
		std::atomic<std::uint32_t> g_cropWidth{ 0 };
		std::atomic<std::uint32_t> g_cropHeight{ 0 };
		std::atomic<std::uint32_t> g_cropLeftX{ 0 };
		std::atomic<std::uint32_t> g_cropLeftY{ 0 };
		std::atomic<std::uint32_t> g_cropRightX{ 0 };
		std::atomic<std::uint32_t> g_cropRightY{ 0 };
		std::atomic<std::uint64_t> g_croppedFrames{ 0 };
		std::atomic<std::uint64_t> g_cropBaseChanges{ 0 };
		std::atomic<std::uint32_t> g_cropMaxBaseStep{ 0 };
		std::uint64_t g_cropFrame = 0;
		bool g_cropHaveLast = false;
		Foveation::Crop g_cropLast{};
		bool g_cropLastMoving = false;
		bool g_cropLastProjection = false;
		Backend::Rect g_cropLastPlace{};
		// Phase 2a's per-eye placement: each eye's projection centre, latched when the crop
		// is first placed and moved only past kReplaceThresholdPx - the projection carries
		// the frame's sub-pixel jitter in the same two terms, which must not move a base. A
		// latch is logged with the two terms it came from, which is what settles their sign
		// on a headset; a re-placement is counted, logged and resets both eyes' histories,
		// since the box's pixels changed under them.
		constexpr float kReplaceThresholdPx = 8.0f;
		Foveation::EyePlacement g_eyePlacement[2]{};
		std::atomic<bool> g_cropProjectionPlaced{ false };
		std::atomic<float> g_publishedCentre[2][2]{};
		std::atomic<std::uint64_t> g_cropReplacements{ 0 };
		// A re-placement's reset, held until a frame's evaluations all succeed, as the reset
		// ticket is: a frame that declines after moving the box still owes it.
		bool g_cropReplacementResetPending = false;
		std::atomic<std::uint64_t> g_historyResetEvaluations{ 0 };
		std::chrono::steady_clock::time_point g_lastFrameTime{};
		float g_lastValidFrameTimeDeltaMs = 1000.0f / 90.0f;
		bool g_hasFrameTime = false;
		LogGate g_renderSizeGate{ 60000 };
		// On every vendor gate a declined frame latches the session off through
		// HDRRoundTrip::Disable, so the latch, not this gate, is what bounds the
		// motion-bind warning to once; the gate only covers a caller that keeps
		// attempting after a decline.
		LogGate g_motionBindGate{ 60000 };
		LogGate g_retentionGate{ 60000 };

		// Wall clock for one backend call, on the thread that made it. Not the GPU
		// timers: these calls load a runtime, allocate vendor state and block, and what
		// is being priced is the render thread's own stall.
		[[nodiscard]] float ElapsedMs(std::chrono::steady_clock::time_point a_start) noexcept
		{
			return std::chrono::duration<float, std::milli>(
				std::chrono::steady_clock::now() - a_start)
			    .count();
		}

		[[nodiscard]] float SampleFrameTimeDeltaMs() noexcept
		{
			constexpr float kDefaultDeltaMs = 1000.0f / 90.0f;
			constexpr float kMinimumDeltaMs = 1.0f;
			constexpr float kMaximumDeltaMs = 100.0f;

			const auto now = std::chrono::steady_clock::now();
			float delta = g_lastValidFrameTimeDeltaMs;
			bool fallback = !g_hasFrameTime;
			if (g_hasFrameTime) {
				const float candidate = std::chrono::duration<float, std::milli>(now - g_lastFrameTime).count();
				if (std::isfinite(candidate) && candidate >= kMinimumDeltaMs && candidate <= kMaximumDeltaMs) {
					delta = candidate;
					g_lastValidFrameTimeDeltaMs = candidate;
				} else {
					fallback = true;
				}
			}
			if (!std::isfinite(delta) || delta < kMinimumDeltaMs || delta > kMaximumDeltaMs) {
				delta = kDefaultDeltaMs;
				g_lastValidFrameTimeDeltaMs = delta;
				fallback = true;
			}
			g_lastFrameTime = now;
			g_hasFrameTime = true;
			g_frameTimeSamples.fetch_add(1, std::memory_order_relaxed);
			if (fallback) {
				g_frameTimeFallbacks.fetch_add(1, std::memory_order_relaxed);
			}
			g_frameTimeDeltaMs.store(delta, std::memory_order_relaxed);
			return delta;
		}

		[[nodiscard]] bool BuildKey(
			const Backend::Config& a_config,
			std::string_view a_runtimeDir,
			ConfigKey& a_out) noexcept
		{
			if (a_config.preset.size() >= sizeof(a_out.preset) || a_runtimeDir.size() >= sizeof(a_out.runtimeDir)) {
				return false;
			}
			a_out = {};
			a_out.renderWidth = a_config.renderWidth;
			a_out.renderHeight = a_config.renderHeight;
			a_out.outputWidth = a_config.outputWidth;
			a_out.outputHeight = a_config.outputHeight;
			a_out.quality = a_config.quality;
			a_out.sharpnessBits = std::bit_cast<std::uint32_t>(a_config.sharpness);
			a_out.presetLength = static_cast<std::uint32_t>(a_config.preset.size());
			a_out.runtimeDirLength = static_cast<std::uint32_t>(a_runtimeDir.size());
			a_out.hdr = a_config.hdr;
			a_out.invertedDepth = a_config.invertedDepth;
			a_out.jitteredMV = a_config.jitteredMV;
			a_out.lowResMV = a_config.lowResMV;
			a_out.autoExposure = a_config.autoExposure;
			a_out.subrects = a_config.enableSubrects;
			a_out.cropWidth = a_config.cropWidth;
			a_out.cropHeight = a_config.cropHeight;
			a_out.cropProjection = a_config.cropProjection;
			a_out.preExposureBits = std::bit_cast<std::uint32_t>(a_config.preExposure);
			a_out.exposureScaleBits = std::bit_cast<std::uint32_t>(a_config.exposureScale);
			if (!a_config.preset.empty()) {
				std::memcpy(a_out.preset, a_config.preset.data(), a_config.preset.size());
			}
			if (!a_runtimeDir.empty()) {
				std::memcpy(a_out.runtimeDir, a_runtimeDir.data(), a_runtimeDir.size());
			}
			return true;
		}

		[[nodiscard]] bool SameKey(const ConfigKey& a_lhs, const ConfigKey& a_rhs) noexcept
		{
			return a_lhs.renderWidth == a_rhs.renderWidth &&
			       a_lhs.renderHeight == a_rhs.renderHeight &&
			       a_lhs.outputWidth == a_rhs.outputWidth &&
			       a_lhs.outputHeight == a_rhs.outputHeight &&
			       a_lhs.quality == a_rhs.quality &&
			       a_lhs.sharpnessBits == a_rhs.sharpnessBits &&
			       a_lhs.presetLength == a_rhs.presetLength &&
			       a_lhs.runtimeDirLength == a_rhs.runtimeDirLength &&
			       a_lhs.hdr == a_rhs.hdr &&
			       a_lhs.invertedDepth == a_rhs.invertedDepth &&
			       a_lhs.jitteredMV == a_rhs.jitteredMV &&
			       a_lhs.lowResMV == a_rhs.lowResMV &&
			       a_lhs.autoExposure == a_rhs.autoExposure &&
			       a_lhs.subrects == a_rhs.subrects &&
			       a_lhs.cropWidth == a_rhs.cropWidth &&
			       a_lhs.cropHeight == a_rhs.cropHeight &&
			       a_lhs.cropProjection == a_rhs.cropProjection &&
			       a_lhs.preExposureBits == a_rhs.preExposureBits &&
			       a_lhs.exposureScaleBits == a_rhs.exposureScaleBits &&
			       std::memcmp(a_lhs.preset, a_rhs.preset, a_lhs.presetLength) == 0 &&
			       std::memcmp(a_lhs.runtimeDir, a_rhs.runtimeDir, a_lhs.runtimeDirLength) == 0;
		}

		// The half of the key that decides whether the runtime under the views has to be
		// loaded again. Everything else in the key describes the views themselves.
		[[nodiscard]] bool SameRuntimeDir(const ConfigKey& a_lhs, const ConfigKey& a_rhs) noexcept
		{
			return a_lhs.runtimeDirLength == a_rhs.runtimeDirLength &&
			       std::memcmp(a_lhs.runtimeDir, a_rhs.runtimeDir, a_lhs.runtimeDirLength) == 0;
		}

		[[nodiscard]] bool Decline(FailureStep a_step) noexcept
		{
			g_active.store(false, std::memory_order_release);
			g_lastFailureStep.store(a_step, std::memory_order_relaxed);
			g_failures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		// The crop a successful frame placed, published at the one instant it is true. A
		// frame without a crop clears it, so CropSnapshot never describes a crop the views
		// no longer carry. Render-thread only.
		void PublishCrop(
			bool a_cropped,
			bool a_moving,
			const Backend::Rect& a_local,
			const Backend::Rect (&a_output)[2]) noexcept
		{
			if (!a_cropped) {
				if (g_cropActive.load(std::memory_order_relaxed)) {
					g_cropActive.store(false, std::memory_order_release);
					g_cropMoving.store(false, std::memory_order_relaxed);
					g_cropWidth.store(0, std::memory_order_relaxed);
					g_cropHeight.store(0, std::memory_order_relaxed);
				}
				g_cropHaveLast = false;
				return;
			}
			if (g_cropFrame != 0) {
				const auto dx = a_local.x > g_cropLastPlace.x ? a_local.x - g_cropLastPlace.x : g_cropLastPlace.x - a_local.x;
				const auto dy = a_local.y > g_cropLastPlace.y ? a_local.y - g_cropLastPlace.y : g_cropLastPlace.y - a_local.y;
				if (dx != 0 || dy != 0) {
					g_cropBaseChanges.fetch_add(1, std::memory_order_relaxed);
				}
				const std::uint32_t step = dx > dy ? dx : dy;
				if (step > g_cropMaxBaseStep.load(std::memory_order_relaxed)) {
					g_cropMaxBaseStep.store(step, std::memory_order_relaxed);
				}
			}
			g_cropLastPlace = a_local;
			++g_cropFrame;
			g_cropMoving.store(a_moving, std::memory_order_relaxed);
			g_cropWidth.store(a_local.w, std::memory_order_relaxed);
			g_cropHeight.store(a_local.h, std::memory_order_relaxed);
			g_cropLeftX.store(a_output[0].x, std::memory_order_relaxed);
			g_cropLeftY.store(a_output[0].y, std::memory_order_relaxed);
			g_cropRightX.store(a_output[1].x, std::memory_order_relaxed);
			g_cropRightY.store(a_output[1].y, std::memory_order_relaxed);
			g_croppedFrames.fetch_add(1, std::memory_order_relaxed);
			g_cropActive.store(true, std::memory_order_release);
		}

		// Read-only confirmation of the render extent the engine was already patched to,
		// against the vendor's own optimal-settings answer for the same quality mode. It
		// never corrects: the dynamic-resolution ratio is a load-time, restart-only patch,
		// so nothing can retarget it mid-session. A mismatch means the compile-time ratio
		// table has drifted from the runtime and belongs in the next bug report.
		//
		// Render-thread only, immediately after Initialize: NGX's optimal-settings query
		// needs the device-bound capability parameters that Initialize creates.
		void ConfirmRenderSize(const Backend::IBackend* a_backend, const Backend::Config& a_config) noexcept
		{
			const auto [optimalWidth, optimalHeight] = a_backend->QueryRenderSize(
				a_config.outputWidth, a_config.outputHeight, a_config.quality);
			if (optimalWidth == a_config.renderWidth && optimalHeight == a_config.renderHeight) {
				return;
			}
			// Echoing the output extent is how a backend says it has no answer: the
			// passthrough control arm never reconstructs, and NGX returns the same pair
			// after logging its own optimal-settings failure. Neither is a table drift.
			if (optimalWidth == a_config.outputWidth && optimalHeight == a_config.outputHeight) {
				return;
			}
			if (g_renderSizeGate.Ready()) {
				logger::warn(
					FMT_STRING("backend '{}' reports optimal render size {}x{} for this quality mode, but the engine is configured for {}x{} - the per-mode ratio table and the runtime disagree"),
					a_backend->Name(),
					optimalWidth,
					optimalHeight,
					a_config.renderWidth,
					a_config.renderHeight);
			}
		}

		// The preset the backend was asked for and the one its runtime answered with,
		// published where they change - once per views creation - and never per frame.
		// The vendor answer is only readable from a device-bound Caps probe, and that
		// probe is render-thread work under the same admission contract CreateViews
		// itself runs under, which is exactly here. atomic<shared_ptr<const string>> is
		// the tree's spelling for a cross-thread string: a reader on the DevBench
		// listener or the panel's own draw can never see a half-written one.
		std::atomic<std::shared_ptr<const std::string>> g_presetRequested{
			std::make_shared<const std::string>("none")
		};
		std::atomic<std::shared_ptr<const std::string>> g_presetReported{
			std::make_shared<const std::string>("none")
		};
		// The quality bucket the mode names and the one the views were created at, from
		// the same probe and published at the same instant. They separate "the preset was
		// honoured" from "the preset was honoured at the bucket the ratio actually needs",
		// which read identically while only one bucket was ever hinted.
		std::atomic<std::shared_ptr<const std::string>> g_bucketRequested{
			std::make_shared<const std::string>("none")
		};
		std::atomic<std::shared_ptr<const std::string>> g_bucketResolved{
			std::make_shared<const std::string>("none")
		};

		void PublishPreset(
			std::atomic<std::shared_ptr<const std::string>>& a_slot,
			std::string_view a_value) noexcept
		{
			try {
				a_slot.store(std::make_shared<const std::string>(a_value), std::memory_order_release);
			} catch (...) {
				// A stale label beside live views is a worse report than a fresh one and
				// a better outcome than a throw out of a noexcept preparation path.
			}
		}

		void ClearPreparedIdentity() noexcept
		{
			PublishPreset(g_presetRequested, "none"sv);
			PublishPreset(g_presetReported, "none"sv);
			PublishPreset(g_bucketRequested, "none"sv);
			PublishPreset(g_bucketResolved, "none"sv);
			// Nothing owns a sequence once the views are gone. Disarming here rather than
			// at each DestroyViews call site keeps one spelling of the teardown.
			Jitter::DisarmBackendSequence();
			// The bias belongs to the configuration that derived it, so a teardown puts
			// the engine's own samplers back and then forgets the build. Restoring alone
			// would leave `MipBias::Built()` true with no reconstruction behind it, and
			// the native one-to-one seam applies through that flag - it would keep biasing
			// the world raster for the rest of the session. Forget releases nothing: a
			// displaced pointer may still be held by value in an engine-side copy, so both
			// sets live for the process and only the table entries move.
			MipBias::Forget();
			g_prepared.store(false, std::memory_order_release);
			g_ownsBackendState.store(false, std::memory_order_release);
			g_backend = nullptr;
			g_device = nullptr;
			g_context = nullptr;
			g_key = {};
			g_hasKey = false;
			g_backendAddress.store(0, std::memory_order_relaxed);
			g_deviceAddress.store(0, std::memory_order_relaxed);
			g_contextAddress.store(0, std::memory_order_relaxed);
			g_lastFrameTime = {};
			g_lastValidFrameTimeDeltaMs = 1000.0f / 90.0f;
			g_hasFrameTime = false;
		}

		void RetainBackendIdentity(
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			const ConfigKey& a_key) noexcept
		{
			g_backend = a_backend;
			g_device = a_device;
			g_context = a_context;
			g_key = a_key;
			g_hasKey = true;
			g_backendAddress.store(reinterpret_cast<std::uintptr_t>(a_backend), std::memory_order_relaxed);
			g_deviceAddress.store(reinterpret_cast<std::uintptr_t>(a_device), std::memory_order_relaxed);
			g_contextAddress.store(reinterpret_cast<std::uintptr_t>(a_context), std::memory_order_relaxed);
			g_ownsBackendState.store(true, std::memory_order_release);
		}

		// The four backend lifecycle calls, each made in exactly one spelling so a call
		// site cannot count one and forget to time it. The counts are calls made, not
		// calls that succeeded - a failure is in the log and in lastFailureStep, and a
		// count that stopped short of a matching call would read as the call never
		// happening.
		[[nodiscard]] bool DestroyViewsTimed(Backend::IBackend* a_backend) noexcept
		{
			const auto start = std::chrono::steady_clock::now();
			g_destroyViewsCount.fetch_add(1, std::memory_order_relaxed);
			const bool released = a_backend->DestroyViews();
			g_lastDestroyViewsMs.store(ElapsedMs(start), std::memory_order_relaxed);
			return released;
		}

		[[nodiscard]] bool ShutdownTimed(Backend::IBackend* a_backend) noexcept
		{
			const auto start = std::chrono::steady_clock::now();
			g_shutdownCount.fetch_add(1, std::memory_order_relaxed);
			// Cleared before the call rather than after a successful one. A declined
			// Shutdown leaves implementation-owned state live, and IBackend asks the
			// caller to retain the identity and retry the release; offering that runtime
			// for reuse here would turn a retry into a silent adoption of a runtime that
			// could not prove it was releasable.
			g_runtimeUp.store(false, std::memory_order_release);
			const bool released = a_backend->Shutdown();
			g_lastShutdownMs.store(ElapsedMs(start), std::memory_order_relaxed);
			return released;
		}

		[[nodiscard]] bool InitializeTimed(
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_immediate,
			const std::filesystem::path& a_runtimeDir)
		{
			const auto start = std::chrono::steady_clock::now();
			g_initializeCount.fetch_add(1, std::memory_order_relaxed);
			const bool initialized = a_backend->Initialize(a_device, a_immediate, a_runtimeDir);
			g_lastInitializeMs.store(ElapsedMs(start), std::memory_order_relaxed);
			if (initialized) {
				g_initializeSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_runtimeUp.store(true, std::memory_order_release);
			}
			return initialized;
		}

		[[nodiscard]] bool CreateViewsTimed(
			Backend::IBackend* a_backend,
			const Backend::Config& a_config,
			std::uint32_t a_viewCount) noexcept
		{
			const auto start = std::chrono::steady_clock::now();
			g_createViewsCount.fetch_add(1, std::memory_order_relaxed);
			const bool created = a_backend->CreateViews(a_config, a_viewCount);
			g_lastCreateViewsMs.store(ElapsedMs(start), std::memory_order_relaxed);
			return created;
		}

		[[nodiscard]] FailureStep PrepareInternal(
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_immediate,
			const D3D11_TEXTURE2D_DESC& a_colorDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			const Backend::Config& a_config,
			std::string_view a_runtimeDir,
			const ConfigKey& a_key) noexcept
		{
			const auto prepareStart = std::chrono::steady_clock::now();
			// The identity behind the views: the backend object, the device and the
			// immediate context this module holds live state on. Nothing about the views'
			// own configuration is in it - that is the key, tested separately below.
			const bool sameIdentity = g_prepared.load(std::memory_order_acquire) &&
			                          g_backend == a_backend && g_device == a_device &&
			                          g_context == a_immediate && g_hasKey;
			if (sameIdentity && SameKey(g_key, a_key)) {
				if (Intermediates::EnsureFor(a_device, a_colorDesc, a_motionDesc, a_config)) {
					return FailureStep::kNone;
				}
				if (!DestroyViewsTimed(g_backend)) {
					return FailureStep::kBackendDestroyViews;
				}
				if (!ShutdownTimed(g_backend)) {
					return FailureStep::kBackendDestroyViews;
				}
				Intermediates::Release();
				ClearPreparedIdentity();
				return FailureStep::kIntermediates;
			}

			// A configuration change re-creates the views. It does not re-create the
			// runtime under them: a vendor SDK is initialised against a device and a
			// runtime directory, neither of which has moved, so tearing it down and
			// loading it again costs half a second of render thread per ratio step and
			// buys nothing. The extents, the quality mode and the preset key the views
			// alone. Shutdown and Initialize therefore run only when the backend object,
			// the device, the immediate context or the runtime directory changed, when
			// no runtime is up yet, and on the fault and teardown paths - Reset, which
			// HDRRoundTrip's recovery machine and FaultGuard both go through, is unchanged
			// and still releases everything.
			//
			// The test is g_runtimeUp and the identity it belongs to, NOT sameIdentity:
			// the operator's toggle to `off` releases the views and leaves the runtime
			// standing (Evaluate::ReleaseViews), so the toggle back on arrives here with
			// nothing prepared and a runtime that is still perfectly good. That case and a
			// live ratio step are the same case, and this is the one line that says so.
			//
			// CreateViews is the whole reconfiguration: IBackend documents it as
			// idempotent and re-creating on a config change, and the NGX backend releases
			// its feature handles before creating the new ones, which is the
			// ReleaseFeature-before-CreateFeature order the SDK expects. A DestroyViews
			// first would only make that release happen twice.
			const bool viewsOnly = g_runtimeUp.load(std::memory_order_acquire) && g_hasKey &&
			                       g_backend == a_backend && g_device == a_device &&
			                       g_context == a_immediate && SameRuntimeDir(g_key, a_key);
			if (!viewsOnly) {
				if (g_backend) {
					if (!DestroyViewsTimed(g_backend)) {
						return FailureStep::kBackendDestroyViews;
					}
					if (!ShutdownTimed(g_backend)) {
						return FailureStep::kBackendDestroyViews;
					}
				}
				Intermediates::Release();
				ClearPreparedIdentity();
			}

			if (!Intermediates::EnsureFor(a_device, a_colorDesc, a_motionDesc, a_config)) {
				// On the views-only path the old views are still standing over
				// intermediates that could not be rebuilt for the new extents. That pairing
				// cannot render, so this failure ends the session's backend state exactly as
				// the same-key path above does.
				if (viewsOnly) {
					if (!DestroyViewsTimed(a_backend)) {
						return FailureStep::kBackendDestroyViews;
					}
					if (!ShutdownTimed(a_backend)) {
						return FailureStep::kBackendDestroyViews;
					}
					Intermediates::Release();
					ClearPreparedIdentity();
				}
				return FailureStep::kIntermediates;
			}
			if (!viewsOnly) {
				try {
					const std::filesystem::path runtimeDir{ a_runtimeDir };
					if (!InitializeTimed(a_backend, a_device, a_immediate, runtimeDir)) {
						if (!ShutdownTimed(a_backend)) {
							RetainBackendIdentity(a_backend, a_device, a_immediate, a_key);
							return FailureStep::kBackendDestroyViews;
						}
						Intermediates::Release();
						return FailureStep::kBackendInitialize;
					}
				} catch (...) {
					if (!ShutdownTimed(a_backend)) {
						RetainBackendIdentity(a_backend, a_device, a_immediate, a_key);
						return FailureStep::kBackendDestroyViews;
					}
					Intermediates::Release();
					return FailureStep::kBackendInitialize;
				}
			}
			ConfirmRenderSize(a_backend, a_config);
			if (!CreateViewsTimed(a_backend, a_config, 2)) {
				// The identity is still retained on the views-only path and the views it
				// names are gone: drop it before the shutdown, so a failed shutdown retains
				// it again through the same call the full path uses and a successful one
				// leaves nothing claiming to be prepared.
				if (viewsOnly) {
					ClearPreparedIdentity();
				}
				if (!ShutdownTimed(a_backend)) {
					RetainBackendIdentity(a_backend, a_device, a_immediate, a_key);
					return FailureStep::kBackendDestroyViews;
				}
				Intermediates::Release();
				return FailureStep::kBackendCreateViews;
			}

			// What was asked for and what the runtime answered, read once, here, where
			// both are true at the same instant. An empty preset in the config means
			// "set nothing and let the runtime and its driver profile decide", which is
			// the same thing the settings file spells "auto".
			PublishPreset(g_presetRequested, a_config.preset.empty() ? "auto"sv : std::string_view{ a_config.preset });
			{
				const auto caps = a_backend->Probe(a_device);
				const std::string_view reported{ caps.presetReported };
				PublishPreset(g_presetReported, reported.empty() ? "unreported"sv : reported);
				const std::string_view requestedBucket{ caps.bucketRequested };
				const std::string_view resolvedBucket{ caps.bucketResolved };
				PublishPreset(g_bucketRequested, requestedBucket.empty() ? "unreported"sv : requestedBucket);
				PublishPreset(g_bucketResolved, resolvedBucket.empty() ? "unreported"sv : resolvedBucket);
			}

			// The sequence is owned by the backend that just created the histories, and
			// the phase count is latched here rather than derived per frame: the pre-world
			// dynamic-resolution hook runs after the jitter call, so a frame's own
			// admission is not yet decided when the commit happens.
			if (Hooks::JitterCommitInstalled()) {
				Jitter::ArmBackendSequence(
					a_backend,
					a_config.renderWidth,
					a_config.outputWidth,
					a_config.outputHeight);
			}

			// The mip bias comes from the same per-eye pair the views were created for,
			// and is built once here rather than per frame. The vendor guide asks for an
			// update whenever the render size changes; in this mod the render size changes
			// only on a configuration change, so this satisfies that exactly rather than
			// approximately. A decline logs its own reason and leaves the table vanilla.
			//
			// Foveated DLAA's box derives its own bias (SamplerBias::kFoveatedBias); the
			// measurement crop, which places no box on the projection, keeps DLAA's. The
			// crop's extent and placement are in the preparation key, so every switch
			// between the box and the full eye (the toggle key, the fail-soft) or the
			// measurement crop prepares again and passes here, and the bias follows the
			// frame: the full eye keeps DLAA's own. The pre-world apply of the frame that
			// switches still carries the previous build; the next frame has the new one.
			//
			// Build restores the engine's samplers before it rebuilds, so a views-only
			// preparation needs no Forget first. It needs one after a decline: the full
			// path reaches here with no build standing at all (ClearPreparedIdentity
			// forgot it), and without this a views-only decline would leave the previous
			// configuration's bias applied to the new render extent, which is a silently
			// wrong LOD rather than the vanilla table a decline promises.
			const bool foveatedBox = Backend::HasCrop(a_config) && a_config.cropProjection;
			if (!MipBias::Build(a_backend, a_config.renderWidth, a_config.outputWidth, foveatedBox) && viewsOnly) {
				MipBias::Forget();
			}

			RetainBackendIdentity(a_backend, a_device, a_immediate, a_key);
			g_prepareCount.fetch_add(1, std::memory_order_relaxed);
			if (viewsOnly) {
				g_viewsOnlyPreparations.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_fullPreparations.fetch_add(1, std::memory_order_relaxed);
			}
			g_lastPrepareViewsOnly.store(viewsOnly, std::memory_order_relaxed);
			g_prepared.store(true, std::memory_order_release);
			// A positive prepared identity transfers jitter validation to the ordinary
			// armed-commit contract. The passive native-NGX startup observer has no work
			// after this point, including when the commit hook is unavailable.
			Jitter::ConfigureNativeStartupTracking(false);
			// Once per configuration change, on the render thread that just stalled for
			// it - not per frame. The numbers together are the hitch an operator reports
			// on a live ratio step, split into the calls that produced it.
			const float prepareMs = ElapsedMs(prepareStart);
			g_lastPrepareMs.store(prepareMs, std::memory_order_relaxed);
			if (viewsOnly) {
				logger::info(
					FMT_STRING("backend preparation: views re-created in {:.1f} ms (create views {:.1f}) with the runtime left up; {} initializations and {} view creations this session"),
					prepareMs,
					g_lastCreateViewsMs.load(std::memory_order_relaxed),
					g_initializeCount.load(std::memory_order_relaxed),
					g_createViewsCount.load(std::memory_order_relaxed));
			} else {
				logger::info(
					FMT_STRING("backend preparation: full re-initialisation in {:.1f} ms (destroy views {:.1f}, shutdown {:.1f}, initialize {:.1f}, create views {:.1f}); {} initializations and {} view creations this session"),
					prepareMs,
					g_lastDestroyViewsMs.load(std::memory_order_relaxed),
					g_lastShutdownMs.load(std::memory_order_relaxed),
					g_lastInitializeMs.load(std::memory_order_relaxed),
					g_lastCreateViewsMs.load(std::memory_order_relaxed),
					g_initializeCount.load(std::memory_order_relaxed),
					g_createViewsCount.load(std::memory_order_relaxed));
			}
			return FailureStep::kNone;
		}
	}

	bool PrepareViews(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config,
		std::string_view a_runtimeDir) noexcept
	{
		if (!a_backend || !a_device || !a_immediate) {
			g_lastFailureStep.store(FailureStep::kInvalidArguments, std::memory_order_relaxed);
			return false;
		}
		ConfigKey key{};
		if (!BuildKey(a_config, a_runtimeDir, key)) {
			g_lastFailureStep.store(FailureStep::kUnsupportedConfig, std::memory_order_relaxed);
			return false;
		}
		const FailureStep result = PrepareInternal(
			a_backend,
			a_device,
			a_immediate,
			a_colorDesc,
			a_motionDesc,
			a_config,
			a_runtimeDir,
			key);
		g_lastFailureStep.store(result, std::memory_order_relaxed);
		return result == FailureStep::kNone;
	}

	namespace
	{
		[[nodiscard]] bool FrameInternal(
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_immediate,
			ID3D11Texture2D* a_doubleWideColor,
			ID3D11Texture2D* a_doubleWideDepth,
			ID3D11Texture2D* a_doubleWideMotionVectors,
			ID3D11Texture2D* a_doubleWideOutput,
			const D3D11_TEXTURE2D_DESC& a_colorDesc,
			const D3D11_TEXTURE2D_DESC& a_depthDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			const Backend::Config& a_config,
			std::string_view a_runtimeDir,
			bool a_requireNull,
			bool a_reassemble) noexcept
		{
			g_attempts.fetch_add(1, std::memory_order_relaxed);
			g_active.store(true, std::memory_order_release);
			if (!a_backend || !a_device || !a_immediate || !a_doubleWideColor || !a_doubleWideDepth ||
				!a_doubleWideMotionVectors || (a_reassemble && !a_doubleWideOutput) ||
				a_doubleWideColor == a_doubleWideDepth ||
				a_doubleWideColor == a_doubleWideMotionVectors ||
				a_doubleWideDepth == a_doubleWideMotionVectors ||
				(a_reassemble &&
					(a_doubleWideColor == a_doubleWideOutput ||
						a_doubleWideDepth == a_doubleWideOutput ||
						a_doubleWideMotionVectors == a_doubleWideOutput))) {
				return Decline(FailureStep::kInvalidArguments);
			}
			if (a_requireNull && a_backend->Name() != "null"sv) {
				return Decline(FailureStep::kUnsupportedBackend);
			}
			const bool nativeAA = a_config.quality == Backend::QualityMode::kNativeAA &&
			                      a_config.renderWidth == a_config.outputWidth &&
			                      a_config.renderHeight == a_config.outputHeight;
			const bool reconstruction = a_config.quality != Backend::QualityMode::kNativeAA &&
			                            a_config.renderWidth <= a_config.outputWidth &&
			                            a_config.renderHeight <= a_config.outputHeight &&
			                            (a_config.renderWidth < a_config.outputWidth ||
											a_config.renderHeight < a_config.outputHeight);
			if ((!nativeAA && !reconstruction) ||
				a_config.outputWidth > UINT32_MAX / 2u ||
				a_config.outputWidth != a_colorDesc.Width / 2u ||
				a_config.outputHeight != a_colorDesc.Height ||
				a_config.invertedDepth || a_config.jitteredMV || !a_config.lowResMV) {
				return Decline(FailureStep::kUnsupportedConfig);
			}
			if (!Backend::IsBackendAutoExposureContract(a_config, nullptr)) {
				g_exposureContractFailures.fetch_add(1, std::memory_order_relaxed);
				return Decline(FailureStep::kExposureContract);
			}
			// The sub-rect path writes the vendor output into the double-wide target
			// itself, so it has no meaning without one: PrivateFrame's caller builds its
			// config with the flag clear, and a caller that did not is a defect.
			const bool subrects = a_config.enableSubrects;
			if (subrects && !a_reassemble) {
				return Decline(FailureStep::kInvalidArguments);
			}
			// A crop is placed by base inside the whole-eye targets, so only the sub-rect
			// path can carry one, and only a well-formed one. The builder never asks for
			// anything else; this is the fail-closed edge if it ever does.
			const bool cropped = Backend::HasCrop(a_config);
			if (cropped && (!subrects || !Backend::IsValidCrop(a_config))) {
				return Decline(FailureStep::kUnsupportedConfig);
			}
			// Fail closed, never fall back: a copy here would hide a contract change in
			// the engine target from every timing table that follows.
			if (subrects && (a_motionDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
				if (g_motionBindGate.Ready()) {
					logger::warn(
						FMT_STRING("stereoMode subrect needs the engine motion target to be shader-readable, but logical 35 carries bind flags 0x{:X}; the frame is declined rather than copied"),
						a_motionDesc.BindFlags);
				}
				return Decline(FailureStep::kMotionSourceNotShaderReadable);
			}
			// A capture that wants the per-eye inputs cannot read them from the engine
			// target later: by the TemporalAA seam RT1 already holds the handed-back
			// frame. On such a frame, and only then, each eye's packed colour rect is
			// retained at the point the colour extract occupies, before that eye's
			// colour-extract mark, so that one frame's leftColorExtract /
			// rightColorExtract interval prices the retention copy and every other
			// frame's reads zero. One atomic, loaded once per frame.
			const bool retainInputs = subrects && StereoCapture::InputRetentionWanted();

			ConfigKey key{};
			if (!BuildKey(a_config, a_runtimeDir, key)) {
				return Decline(FailureStep::kUnsupportedConfig);
			}

			Backend::Rect inputRects[2]{};
			Backend::Rect outputRects[2]{};
			Intermediates::DepthReference depths[2]{};
			for (std::uint32_t index = 0; index < 2; ++index) {
				const auto eye = index == 0 ? StereoLayout::Eye::kLeft : StereoLayout::Eye::kRight;
				outputRects[index] = StereoLayout::EyeRect(eye, a_colorDesc.Width, a_colorDesc.Height);
				// Dynamic resolution scales the one full-width world viewport. FO4VR's
				// instanced stereo shader maps the two eyes into adjacent halves of that
				// reduced viewport, so source eyes are packed at 2*renderWidth. Handback
				// remains split at outputWidth in the full-sized allocation.
				inputRects[index] = StereoLayout::EyeRect(
					eye,
					a_config.renderWidth * 2u,
					a_config.renderHeight);
				depths[index] = Intermediates::ReferenceDepth(a_doubleWideDepth, a_depthDesc, inputRects[index]);
				if (!depths[index].resource) {
					return Decline(index == 0 ? FailureStep::kReferenceDepthLeft : FailureStep::kReferenceDepthRight);
				}
			}

			float jitterX = 0.0f;
			float jitterY = 0.0f;
			// The FULL per-eye output extent, not renderWidth: the engine state holds the
			// full extent and rescales the stored floats by W/floor(W*ratio) on the way to
			// the projection, so the dynamic-resolution ratio cancels in the conversion.
			if (!Jitter::AcquireFrameSample(a_config.outputWidth, a_config.outputHeight, jitterX, jitterY)) {
				return Decline(FailureStep::kReadJitter);
			}

			const FailureStep prepare = PrepareInternal(a_backend, a_device, a_immediate, a_colorDesc, a_motionDesc, a_config, a_runtimeDir, key);
			if (prepare != FailureStep::kNone) {
				return Decline(prepare);
			}
			const float frameTimeDeltaMs = SampleFrameTimeDeltaMs();
			const ResetPolicy::Ticket resetTicket = ResetPolicy::AcquireReset();
			MotionReprojection::Data cameraReprojection[2]{};
			CameraProjection::Data cameraProjection[2]{};
			for (std::uint32_t index = 0; index < 2; ++index) {
				// A failed snapshot still lets backends that do not need projection data
				// use the finite-zero motion fallback. FSR consumes the same zeroed
				// projection contract below and deliberately declines that frame.
				static_cast<void>(CameraMotion::SnapshotEye(
					index,
					cameraReprojection[index],
					cameraProjection[index]));
			}

			// Timing points after each per-eye GPU unit, indexed by eye. No-ops unless the
			// reduced-NGX quality transaction opened an episode.
			constexpr Telemetry::MarkPoint afterColorExtract[2]{
				Telemetry::MarkPoint::kAfterLeftColorExtract,
				Telemetry::MarkPoint::kAfterRightColorExtract,
			};
			constexpr Telemetry::MarkPoint afterMotionExtract[2]{
				Telemetry::MarkPoint::kAfterLeftMotionExtract,
				Telemetry::MarkPoint::kAfterRightMotionExtract,
			};
			constexpr Telemetry::MarkPoint afterEvaluate[2]{
				Telemetry::MarkPoint::kAfterLeftEvaluate,
				Telemetry::MarkPoint::kAfterRightEvaluate,
			};
			constexpr Telemetry::MarkPoint afterReassembly[2]{
				Telemetry::MarkPoint::kAfterLeftReassembly,
				Telemetry::MarkPoint::kAfterRightReassembly,
			};
			// The crop's eye-local rect for this frame, once, so both eyes take the same one.
			// The path position restarts whenever the extent or the motion changes.
			Backend::Rect cropLocal{};
			Backend::Rect cropLocalEye[2]{};
			bool cropReplaced = false;
			Backend::Rect cropOutput[2]{};
			if (cropped) {
				const auto crop = Foveation::FromExtent(a_config.renderWidth, a_config.renderHeight, a_config.cropWidth, a_config.cropHeight);
				if (!g_cropHaveLast || g_cropLast.width != crop.width || g_cropLast.height != crop.height ||
					g_cropLastMoving != a_config.cropMotion || g_cropLastProjection != a_config.cropProjection) {
					// The views stand across a placement switch alone, so its reset is owed here.
					if (Foveation::PlacementSwitchedAlone(g_cropHaveLast, g_cropLast, g_cropLastMoving, g_cropLastProjection, crop,
							a_config.cropMotion, a_config.cropProjection)) {
						g_cropReplacementResetPending = true;
						logger::info(FMT_STRING("foveation: the {}x{} crop moves from {} to {} at the same extent; both histories reset"),
							crop.width,
							crop.height,
							g_cropLastProjection ? "the projection centre"sv : "the image centre"sv,
							a_config.cropProjection ? "the projection centre"sv : "the image centre"sv);
					}
					g_cropFrame = 0;
					g_cropHaveLast = true;
					g_cropLast = crop;
					g_cropLastMoving = a_config.cropMotion;
					g_cropLastProjection = a_config.cropProjection;
					g_eyePlacement[0] = {};
					g_eyePlacement[1] = {};
				}
				cropLocal = Foveation::Place(crop, a_config.cropMotion, g_cropFrame);
				cropLocalEye[0] = cropLocal;
				cropLocalEye[1] = cropLocal;
				if (a_config.cropProjection && !a_config.cropMotion) {
					// Each eye on its own projection centre. A projection that gives no centre
					// keeps the eye on the image centre until one does.
					for (std::uint32_t index = 0; index < 2; ++index) {
						float x = 0.0f;
						float y = 0.0f;
						const bool haveCentre = cameraProjection[index].valid &&
						                        Foveation::ProjectionCentre(cameraProjection[index].centreNdcX, cameraProjection[index].centreNdcY,
													a_config.renderWidth, a_config.renderHeight, x, y);
						const auto step = Foveation::ObserveCentre(g_eyePlacement[index], crop, haveCentre, x, y, kReplaceThresholdPx);
						g_eyePlacement[index] = step.next;
						if (step.replaced) {
							g_cropReplacements.fetch_add(1, std::memory_order_relaxed);
							g_cropReplacementResetPending = true;
						}
						if (step.latchedNow) {
							const bool moved = step.replaced;
							const float fromX = step.fromX;
							const float fromY = step.fromY;
							const auto placed = Foveation::PlaceAt(crop, x, y);
							logger::info(
								FMT_STRING("foveation: the {} eye's {}x{} box {} its projection centre ({:.1f}, {:.1f}) of the {}x{} eye, from P[8] {:.4f} and P[9] {:.4f}: base ({}, {}), the image centre's ({}, {}){}"),
								index == 0 ? "left"sv : "right"sv,
								crop.width,
								crop.height,
								moved ? "re-placed on"sv : "placed on"sv,
								x,
								y,
								a_config.renderWidth,
								a_config.renderHeight,
								cameraProjection[index].centreNdcX,
								cameraProjection[index].centreNdcY,
								placed.x,
								placed.y,
								cropLocal.x,
								cropLocal.y,
								moved ? fmt::format(FMT_STRING(" (moved from ({:.1f}, {:.1f}); both histories reset)"), fromX, fromY) : std::string{});
						}
						if (g_eyePlacement[index].latched) {
							cropLocalEye[index] = Foveation::PlaceAt(crop, g_eyePlacement[index].x, g_eyePlacement[index].y);
						}
					}
				}
				// On either placement: the switch above can owe a reset to a centred crop.
				cropReplaced = g_cropReplacementResetPending;
				if (cropLocalEye[0].w == 0 || cropLocalEye[0].h == 0 || cropLocalEye[1].w == 0 || cropLocalEye[1].h == 0) {
					return Decline(FailureStep::kStereoGeometry);
				}
			}
			for (std::uint32_t index = 0; index < 2; ++index) {
				const auto eye = index == 0 ? StereoLayout::Eye::kLeft : StereoLayout::Eye::kRight;
				const Backend::Rect inputRect = inputRects[index];
				const Backend::Rect outputRect = outputRects[index];
				const Intermediates::Staged staged = Intermediates::Get(eye);
				// Completed motion and private depth are per-eye on both paths; colour
				// and output intermediates exist only on the copy path.
				const bool stagedComplete = subrects ?
				                                (!staged.color && !staged.output) :
				                                (staged.color && staged.output);
				if (inputRect.w == 0 || inputRect.h == 0 || outputRect.w == 0 || outputRect.h == 0 ||
					!staged.depth || !staged.motionVectors || !stagedComplete) {
					return Decline(FailureStep::kStereoGeometry);
				}
				// Nothing is issued between the two marks on the sub-rect path, so the
				// colour-extract interval reads zero there; the interval keeps its name so
				// the two paths share one timing table.
				if (!subrects && !Intermediates::ExtractColor(a_immediate, a_doubleWideColor, inputRect, staged.color)) {
					return Decline(index == 0 ? FailureStep::kExtractLeft : FailureStep::kExtractRight);
				}
				// Diagnostic only, so a failed retention does not decline the frame; the
				// capture reports the missing texture by name instead.
				if (retainInputs && !Intermediates::RetainColor(a_immediate, a_doubleWideColor, inputRect, eye) &&
					g_retentionGate.Ready()) {
					logger::warn(
						FMT_STRING("capture input retention failed for the {} eye, intermediates HRESULT {:08X}; the armed capture will report no retained input"),
						index == 0 ? "left"sv : "right"sv,
						static_cast<std::uint32_t>(Intermediates::Snapshot().lastError));
				}
				Telemetry::Mark(afterColorExtract[index]);
				// A crop's motion and depth go to the vendor zero-origin: NGX does not honour a
				// non-zero motion base, and the crop read at its base ghosted in the headset.
				// The sanitizer writes the crop's pixels alone, straight into crop-sized targets
				// (Intermediates::ExtractCropMotion), so a cropped frame pays for less than the
				// whole eye instead of the whole eye plus a copy.
				Intermediates::CropStaged cropStaged{};
				if (cropped) {
					cropStaged = Intermediates::ExtractCropMotion(
						a_immediate,
						a_doubleWideMotionVectors,
						a_doubleWideDepth,
						inputRect,
						eye,
						cropLocalEye[index],
						cameraReprojection[index]);
					if (!cropStaged.depth || !cropStaged.motionVectors) {
						return Decline(index == 0 ? FailureStep::kExtractMotionLeft : FailureStep::kExtractMotionRight);
					}
				} else if (!Intermediates::ExtractMotion(
							   a_immediate,
							   a_doubleWideMotionVectors,
							   a_doubleWideDepth,
							   inputRect,
							   staged.motionVectors,
							   cameraReprojection[index])) {
					return Decline(index == 0 ? FailureStep::kExtractMotionLeft : FailureStep::kExtractMotionRight);
				}
				Telemetry::Mark(afterMotionExtract[index]);

				Backend::EyeView view{};
				view.index = index;
				view.depth = staged.depth;
				view.motionVectors = staged.motionVectors;
				view.exposure = nullptr;
				view.depthRect = { 0, 0, a_config.renderWidth, a_config.renderHeight };
				// Completed motion is zero-origin on both paths: the sanitizer wrote the
				// eye into its own texture whichever source it read.
				view.mvRect = { 0, 0, a_config.renderWidth, a_config.renderHeight };
				if (subrects) {
					// Colour is the packed eye inside the engine target and the output is
					// this eye's half of the private double-wide, both addressed by base.
					view.color = a_doubleWideColor;
					view.colorRect = inputRect;
					view.output = a_doubleWideOutput;
					view.outputRect = outputRect;
				} else {
					view.color = staged.color;
					view.colorRect = { 0, 0, a_config.renderWidth, a_config.renderHeight };
					view.output = staged.output;
					view.outputRect = { 0, 0, a_config.outputWidth, a_config.outputHeight };
				}
				if (cropped) {
					// The crop rides on the sub-rect path (declined above otherwise): the eye
					// base plus the crop's rect in the double-wide colour and output, which NGX
					// honours, and the staged crop-sized motion and depth at base (0, 0). The
					// motion scale below stays the whole eye's - the staged values are the
					// eye's own, and one pixel of the crop is one pixel of the eye.
					const Backend::Rect& local = cropLocalEye[index];
					view.colorRect = { inputRect.x + local.x, inputRect.y + local.y, local.w, local.h };
					view.outputRect = { outputRect.x + local.x, outputRect.y + local.y, local.w, local.h };
					view.motionVectors = cropStaged.motionVectors;
					view.depth = cropStaged.depth;
					view.mvRect = { 0, 0, local.w, local.h };
					view.depthRect = view.mvRect;
					cropOutput[index] = view.outputRect;
				}
				const auto mvScale = Backend::NormalizedUvMotionScale(a_config.renderWidth, a_config.renderHeight);
				view.mvScaleX = mvScale.x;
				view.mvScaleY = mvScale.y;
				view.jitterX = jitterX;
				view.jitterY = jitterY;
				view.frameTimeDeltaMs = frameTimeDeltaMs;
				view.cameraNear = cameraProjection[index].nearPlane;
				view.cameraFar = cameraProjection[index].farPlane;
				view.cameraFovAngleVertical = cameraProjection[index].verticalFovRadians;
				view.viewSpaceToMetersFactor = CameraMotion::kViewSpaceToMetersFactor;
				// A Phase 2a re-placement resets both eyes in the same frame, as every reset does.
				view.resetHistory = resetTicket.pending || cropReplaced;
				Jitter::RecordSubmission(index, view.jitterX, view.jitterY);
				ResetPolicy::RecordSubmission(index, view.resetHistory);
				if (!Backend::IsBackendAutoExposureContract(a_config, view.exposure)) {
					g_exposureContractFailures.fetch_add(1, std::memory_order_relaxed);
					return Decline(FailureStep::kExposureContract);
				}
				g_autoExposureSubmissions.fetch_add(1, std::memory_order_relaxed);
				if (!a_backend->Evaluate(a_immediate, view)) {
					return Decline(index == 0 ? FailureStep::kEvaluateLeft : FailureStep::kEvaluateRight);
				}
				Telemetry::Mark(afterEvaluate[index]);
				if (index == 0) {
					g_leftEvaluates.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_rightEvaluates.fetch_add(1, std::memory_order_relaxed);
				}

				if (a_reassemble) {
					// The eye is already in place on the sub-rect path; the mark and the
					// counter still record that it landed in the double-wide output.
					if (!subrects && !Intermediates::ReassembleOutput(a_immediate, staged.output, a_doubleWideOutput, outputRect)) {
						return Decline(index == 0 ? FailureStep::kReassembleLeft : FailureStep::kReassembleRight);
					}
					Telemetry::Mark(afterReassembly[index]);
					if (index == 0) {
						g_leftReassemblies.fetch_add(1, std::memory_order_relaxed);
					} else {
						g_rightReassemblies.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}

			// A history reset is a stereo transaction. Do not acknowledge the generation
			// until both backend evaluations and any requested reassemblies succeeded. Every
			// earlier return leaves it pending, so the next attempt resets both eyes again.
			ResetPolicy::AcknowledgeReset(resetTicket);
			if (cropReplaced) {
				g_cropReplacementResetPending = false;
			}
			if (resetTicket.pending || cropReplaced) {
				g_historyResetEvaluations.fetch_add(1, std::memory_order_release);
			}
			PublishCrop(cropped, a_config.cropMotion, cropLocalEye[0], cropOutput);
			const bool projectionPlaced = cropped && a_config.cropProjection && !a_config.cropMotion && g_eyePlacement[0].latched && g_eyePlacement[1].latched;
			g_cropProjectionPlaced.store(projectionPlaced, std::memory_order_relaxed);
			if (projectionPlaced) {
				for (std::uint32_t index = 0; index < 2; ++index) {
					g_publishedCentre[index][0].store(g_eyePlacement[index].x, std::memory_order_relaxed);
					g_publishedCentre[index][1].store(g_eyePlacement[index].y, std::memory_order_relaxed);
				}
			}
			g_successes.fetch_add(1, std::memory_order_relaxed);
			g_lastFailureStep.store(FailureStep::kNone, std::memory_order_relaxed);
			g_active.store(false, std::memory_order_release);
			return true;
		}
	}

	bool Frame(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWideColor,
		ID3D11Texture2D* a_doubleWideDepth,
		ID3D11Texture2D* a_doubleWideMotionVectors,
		ID3D11Texture2D* a_doubleWideOutput,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_depthDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config,
		std::string_view a_runtimeDir) noexcept
	{
		return FrameInternal(
			a_backend,
			a_device,
			a_immediate,
			a_doubleWideColor,
			a_doubleWideDepth,
			a_doubleWideMotionVectors,
			a_doubleWideOutput,
			a_colorDesc,
			a_depthDesc,
			a_motionDesc,
			a_config,
			a_runtimeDir,
			/*a_requireNull=*/true,
			/*a_reassemble=*/true);
	}

	bool PrivateFrame(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWideColor,
		ID3D11Texture2D* a_doubleWideDepth,
		ID3D11Texture2D* a_doubleWideMotionVectors,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_depthDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config,
		std::string_view a_runtimeDir) noexcept
	{
		// The per-eye outputs this entry leaves the result in exist only on the copy
		// path, and this frame runs inside the caller's SEH arm, where a Config copy
		// would carry a destructible string past a fault. The caller therefore builds
		// the config with enableSubrects false, and FrameInternal declines one that is
		// not.
		return FrameInternal(
			a_backend,
			a_device,
			a_immediate,
			a_doubleWideColor,
			a_doubleWideDepth,
			a_doubleWideMotionVectors,
			/*a_doubleWideOutput=*/nullptr,
			a_colorDesc,
			a_depthDesc,
			a_motionDesc,
			a_config,
			a_runtimeDir,
			/*a_requireNull=*/false,
			/*a_reassemble=*/false);
	}

	bool ReassembledFrame(
		Backend::IBackend* a_backend,
		ID3D11Device* a_device,
		ID3D11DeviceContext* a_immediate,
		ID3D11Texture2D* a_doubleWideColor,
		ID3D11Texture2D* a_doubleWideDepth,
		ID3D11Texture2D* a_doubleWideMotionVectors,
		ID3D11Texture2D* a_privateDoubleWideOutput,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_depthDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const Backend::Config& a_config,
		std::string_view a_runtimeDir) noexcept
	{
		return FrameInternal(
			a_backend,
			a_device,
			a_immediate,
			a_doubleWideColor,
			a_doubleWideDepth,
			a_doubleWideMotionVectors,
			a_privateDoubleWideOutput,
			a_colorDesc,
			a_depthDesc,
			a_motionDesc,
			a_config,
			a_runtimeDir,
			/*a_requireNull=*/false,
			/*a_reassemble=*/true);
	}

	State Snapshot() noexcept
	{
		return {
			g_prepared.load(std::memory_order_acquire),
			g_active.load(std::memory_order_acquire),
			g_ownsBackendState.load(std::memory_order_acquire),
			g_attempts.load(std::memory_order_relaxed),
			g_successes.load(std::memory_order_relaxed),
			g_failures.load(std::memory_order_relaxed),
			g_leftEvaluates.load(std::memory_order_relaxed),
			g_rightEvaluates.load(std::memory_order_relaxed),
			g_leftReassemblies.load(std::memory_order_relaxed),
			g_rightReassemblies.load(std::memory_order_relaxed),
			g_prepareCount.load(std::memory_order_relaxed),
			g_autoExposureSubmissions.load(std::memory_order_relaxed),
			g_exposureContractFailures.load(std::memory_order_relaxed),
			g_frameTimeSamples.load(std::memory_order_relaxed),
			g_frameTimeFallbacks.load(std::memory_order_relaxed),
			g_frameTimeDeltaMs.load(std::memory_order_relaxed),
			g_lastFailureStep.load(std::memory_order_relaxed),
			g_backendAddress.load(std::memory_order_relaxed),
			g_deviceAddress.load(std::memory_order_relaxed),
			g_contextAddress.load(std::memory_order_relaxed),
		};
	}

	std::uint64_t HistoryResetEvaluations() noexcept
	{
		return g_historyResetEvaluations.load(std::memory_order_acquire);
	}

	CropState CropSnapshot() noexcept
	{
		CropState out{};
		out.active = g_cropActive.load(std::memory_order_acquire);
		out.moving = g_cropMoving.load(std::memory_order_relaxed);
		out.width = g_cropWidth.load(std::memory_order_relaxed);
		out.height = g_cropHeight.load(std::memory_order_relaxed);
		out.left = { g_cropLeftX.load(std::memory_order_relaxed), g_cropLeftY.load(std::memory_order_relaxed), out.width, out.height };
		out.right = { g_cropRightX.load(std::memory_order_relaxed), g_cropRightY.load(std::memory_order_relaxed), out.width, out.height };
		out.croppedFrames = g_croppedFrames.load(std::memory_order_relaxed);
		out.baseChanges = g_cropBaseChanges.load(std::memory_order_relaxed);
		out.maxBaseStep = g_cropMaxBaseStep.load(std::memory_order_relaxed);
		out.projectionPlaced = g_cropProjectionPlaced.load(std::memory_order_relaxed);
		out.leftCentreX = g_publishedCentre[0][0].load(std::memory_order_relaxed);
		out.leftCentreY = g_publishedCentre[0][1].load(std::memory_order_relaxed);
		out.rightCentreX = g_publishedCentre[1][0].load(std::memory_order_relaxed);
		out.rightCentreY = g_publishedCentre[1][1].load(std::memory_order_relaxed);
		out.replacements = g_cropReplacements.load(std::memory_order_relaxed);
		return out;
	}

	Lifecycle LifecycleSnapshot() noexcept
	{
		return {
			g_initializeCount.load(std::memory_order_relaxed),
			g_initializeSuccesses.load(std::memory_order_relaxed),
			g_createViewsCount.load(std::memory_order_relaxed),
			g_destroyViewsCount.load(std::memory_order_relaxed),
			g_shutdownCount.load(std::memory_order_relaxed),
			g_viewsOnlyPreparations.load(std::memory_order_relaxed),
			g_fullPreparations.load(std::memory_order_relaxed),
			g_viewsReleases.load(std::memory_order_relaxed),
			g_runtimeUp.load(std::memory_order_acquire),
			g_lastInitializeMs.load(std::memory_order_relaxed),
			g_lastCreateViewsMs.load(std::memory_order_relaxed),
			g_lastDestroyViewsMs.load(std::memory_order_relaxed),
			g_lastShutdownMs.load(std::memory_order_relaxed),
			g_lastPrepareMs.load(std::memory_order_relaxed),
			g_lastPrepareViewsOnly.load(std::memory_order_relaxed),
		};
	}

	bool Prepared() noexcept
	{
		return g_prepared.load(std::memory_order_acquire);
	}

	bool Active() noexcept
	{
		return g_active.load(std::memory_order_acquire);
	}

	std::string RequestedPreset() noexcept
	{
		try {
			if (const auto value = g_presetRequested.load(std::memory_order_acquire); value) {
				return *value;
			}
		} catch (...) {
		}
		return {};
	}

	std::string ReportedPreset() noexcept
	{
		try {
			if (const auto value = g_presetReported.load(std::memory_order_acquire); value) {
				return *value;
			}
		} catch (...) {
		}
		return {};
	}

	std::string RequestedBucket() noexcept
	{
		try {
			if (const auto value = g_bucketRequested.load(std::memory_order_acquire); value) {
				return *value;
			}
		} catch (...) {
		}
		return {};
	}

	std::string ResolvedBucket() noexcept
	{
		try {
			if (const auto value = g_bucketResolved.load(std::memory_order_acquire); value) {
				return *value;
			}
		} catch (...) {
		}
		return {};
	}

	bool ReleaseViews() noexcept
	{
		g_active.store(false, std::memory_order_release);
		if (!g_backend) {
			return true;
		}
		if (!DestroyViewsTimed(g_backend)) {
			return false;
		}
		Intermediates::Release();
		// Everything ClearPreparedIdentity does for the views, and nothing it does for
		// the runtime. The sequence, the mip bias and the preset pair all describe views
		// that no longer exist, and the frame-time history restarts with the next
		// preparation; the backend, the device, the context and the key stay, because
		// they are the identity the still-initialised runtime belongs to and the one the
		// next preparation matches its own against. g_ownsBackendState stays true for the
		// same reason: this module still owns SDK state, so a fault still schedules the
		// full teardown that releases it.
		PublishPreset(g_presetRequested, "none"sv);
		PublishPreset(g_presetReported, "none"sv);
		PublishPreset(g_bucketRequested, "none"sv);
		PublishPreset(g_bucketResolved, "none"sv);
		Jitter::DisarmBackendSequence();
		MipBias::Forget();
		g_lastFrameTime = {};
		g_lastValidFrameTimeDeltaMs = 1000.0f / 90.0f;
		g_hasFrameTime = false;
		g_prepared.store(false, std::memory_order_release);
		g_viewsReleases.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool Reset() noexcept
	{
		g_active.store(false, std::memory_order_release);
		if (g_backend && !DestroyViewsTimed(g_backend)) {
			return false;
		}
		if (g_backend && !ShutdownTimed(g_backend)) {
			return false;
		}
		Intermediates::Release();
		ClearPreparedIdentity();
		return true;
	}
}
