#include "Upscaler/HDRRoundTrip.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Backend/BackendRegistry.h"
#include "Upscaler/Common.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Evaluate.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/StereoCapture.h"
#include "Upscaler/TAAReplacement.h"
#include "Upscaler/Telemetry.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Upscaler::HDRRoundTrip
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		constexpr std::uint64_t kTeardownMaxAttempts = 2;

		D3D11StateIsolation::Context g_isolation;
		ComPtr<ID3D11Texture2D> g_private;
		D3D11_TEXTURE2D_DESC g_privateDesc{};
		ID3D11Device* g_device = nullptr;          // borrowed, render-thread only
		ID3D11DeviceContext* g_context = nullptr;  // borrowed, render-thread only

		std::atomic<bool> g_enabled{ false };
		std::atomic<bool> g_stereoNullEnabled{ false };
		std::atomic<bool> g_ngxCreateEnabled{ false };
		std::atomic<bool> g_ngxEvaluateEnabled{ false };
		std::atomic<bool> g_ngxReassembleEnabled{ false };
		std::atomic<bool> g_ngxHandbackEnabled{ false };
		std::atomic<bool> g_ngxContinuousEnabled{ false };
		std::atomic<bool> g_ffxEvaluateEnabled{ false };
		std::atomic<bool> g_ffxReassembleEnabled{ false };
		std::atomic<bool> g_ffxHandbackEnabled{ false };
		std::atomic<bool> g_ffxContinuousEnabled{ false };
		std::atomic<std::uint64_t> g_configurationEpoch{ 0 };
		std::atomic<std::uint64_t> g_activeConfigurationEntries{ 0 };
		std::atomic<bool> g_configurationHadRawNgx{ false };
		std::atomic<bool> g_configurationHadFfx{ false };
		std::atomic<bool> g_isolationAttempted{ false };
		std::atomic<bool> g_isolationReady{ false };
		std::atomic<bool> g_privateReady{ false };
		std::atomic<bool> g_completed{ false };
		std::atomic<bool> g_stereoNullCompleted{ false };
		std::atomic<bool> g_ngxCreateCompleted{ false };
		std::atomic<bool> g_ngxEvaluateCompleted{ false };
		std::atomic<bool> g_ngxReassembleCompleted{ false };
		std::atomic<bool> g_ngxHandbackCompleted{ false };
		std::atomic<bool> g_ngxContinuousCompleted{ false };
		std::atomic<bool> g_ngxTeardownPending{ false };
		std::atomic<bool> g_ngxTeardownCompleted{ false };
		std::atomic<bool> g_ffxEvaluateCompleted{ false };
		std::atomic<bool> g_ffxReassembleCompleted{ false };
		std::atomic<bool> g_ffxHandbackCompleted{ false };
		std::atomic<bool> g_ffxContinuousCompleted{ false };
		std::atomic<bool> g_ffxTeardownPending{ false };
		std::atomic<bool> g_ffxTeardownCompleted{ false };
		std::atomic<bool> g_sessionDisabled{ false };
		// The recovery predicate's own bookkeeping. failSoft mirrors Settings::failSoft,
		// published by Configure exactly like every other cross-thread setting here: one
		// load on the settings thread, passed by value, stored once. suspendFramesLeft > 0
		// means Suspended and counting down; rearmCount/suspensionCount are lifetime
		// counters for /state; recoveryRefusal names the predicate that last refused a
		// re-arm, kNone while never disabled or currently suspended.
		std::atomic<bool> g_failSoft{ false };
		std::atomic<std::uint32_t> g_suspendFramesLeft{ 0 };
		std::atomic<std::uint32_t> g_rearmCount{ 0 };
		std::atomic<std::uint32_t> g_suspensionCount{ 0 };
		std::atomic<std::uint32_t> g_recoveryRefusal{
			static_cast<std::uint32_t>(RecoveryRefusal::kNone)
		};
		// Render-thread only, touched from TickSessionRecovery alone: whether last frame's
		// tick already observed the session disabled, so a false -> true edge is classified
		// exactly once, after the frame that raised it has fully unwound.
		bool g_recoveryLastObservedDisabled = false;
		// Render-thread only, set once per suspend alongside g_suspendFramesLeft: which
		// backend's owned teardown (if any) this cool-down is waiting to resolve. Needed
		// because g_ngxTeardownCompleted/g_ffxTeardownCompleted are lifetime latches shared
		// with every other caller of the teardown path - reading the wrong one, or reading
		// either one when this suspend never owned a backend at all, would answer a
		// different cycle's question.
		enum class RecoveryTeardownTarget : std::uint32_t
		{
			kNone,
			kNgx,
			kFfx,
		};
		RecoveryTeardownTarget g_recoveryTeardownTarget = RecoveryTeardownTarget::kNone;
		// True from the frame a fault is classified Suspended until the frame the re-arm
		// resolves it, either way. Written release-only on the render thread inside
		// TickSessionRecovery/TryResolveRearm, read acquire on the settings threads by
		// BeginConfiguration and Configure: the release at the suspend edge publishes the
		// cool-down that the acquire load has to see, so a reload landing after the
		// classification can never read a stale false and wipe the gates the re-arm needs.
		// A reload landing BEFORE the edge is classified reads false and takes the ordinary
		// fail-closed abort, which is correct - at that instant the session has not yet
		// decided it can recover, and failSoft = false never will.
		std::atomic<bool> g_recoverySuspensionActive{ false };
		std::atomic<std::shared_ptr<const std::string>> g_ngxRuntimeDir;
		std::atomic<std::shared_ptr<const std::string>> g_ngxPreset;
		std::atomic<std::shared_ptr<const std::string>> g_quality;
		// Published by Configure, read on the render thread by every config builder and
		// by EnsurePrivate; the resolution against Caps::supportsSubrects happens once,
		// on the settings thread, so a session never runs one eye down each path.
		std::atomic<bool> g_stereoSubrectsRequested{ false };
		std::atomic<bool> g_stereoSubrects{ false };
		std::atomic<std::uint32_t> g_stereoBackend{ static_cast<std::uint32_t>(StereoBackend::kNone) };
		// Bit 63 locks the target at the first continuous attempt, same as before. Bit 62 is
		// new: the product path has no target, and Configuration.cpp's mode-derivation block
		// warns against expressing that as a sentinel number nobody can tell apart from a
		// real bound - so it gets its own bit instead. The low 62 bits still hold a genuine
		// numeric target when that bit is clear, and are left 0 when it is set: there is
		// nothing for them to mean once unbounded.
		constexpr std::uint64_t kContinuousTargetLocked = 1ull << 63;
		constexpr std::uint64_t kContinuousTargetUnbounded = 1ull << 62;
		constexpr std::uint64_t kContinuousTargetMask = ~(kContinuousTargetLocked | kContinuousTargetUnbounded);
		std::atomic<std::uint64_t> g_continuousTargetState{ kDefaultContinuousTargetSuccesses };
		// The mode Configure() last published, mirrored for /state as a canonical
		// literal (SettingsPolicy.h's six-value IsKnownMode vocabulary, so no allocation is
		// needed - see CanonicalModeLiteral below) and the derived product-path bool beside
		// it. Settings thread writes at Configure(); the listener thread reads through
		// Snapshot().
		std::atomic<const char*> g_mode{ "off" };
		std::atomic<bool> g_productPath{ false };

		std::atomic<std::uint64_t> g_isolationAttempts{ 0 };
		std::atomic<std::uint64_t> g_roundTripAttempts{ 0 };
		std::atomic<std::uint64_t> g_successes{ 0 };
		std::atomic<std::uint64_t> g_failures{ 0 };
		std::atomic<std::uint64_t> g_skips{ 0 };
		std::atomic<std::uint64_t> g_resourceBuilds{ 0 };
		std::atomic<std::uint64_t> g_stereoNullAttempts{ 0 };
		std::atomic<std::uint64_t> g_stereoNullSuccesses{ 0 };
		std::atomic<std::uint64_t> g_stereoNullFailures{ 0 };
		std::atomic<std::uint64_t> g_stereoNullSkips{ 0 };
		std::atomic<std::uint64_t> g_ngxCreateAttempts{ 0 };
		std::atomic<std::uint64_t> g_ngxCreateSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ngxCreateFailures{ 0 };
		std::atomic<std::uint64_t> g_ngxCreateSkips{ 0 };
		std::atomic<std::uint64_t> g_ngxEvaluateAttempts{ 0 };
		std::atomic<std::uint64_t> g_ngxEvaluateSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ngxEvaluateFailures{ 0 };
		std::atomic<std::uint64_t> g_ngxEvaluateSkips{ 0 };
		std::atomic<std::uint64_t> g_ngxReassembleAttempts{ 0 };
		std::atomic<std::uint64_t> g_ngxReassembleSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ngxReassembleFailures{ 0 };
		std::atomic<std::uint64_t> g_ngxReassembleSkips{ 0 };
		std::atomic<std::uint64_t> g_ngxHandbackAttempts{ 0 };
		std::atomic<std::uint64_t> g_ngxHandbackSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ngxHandbackFailures{ 0 };
		std::atomic<std::uint64_t> g_ngxHandbackSkips{ 0 };
		std::atomic<std::uint64_t> g_ngxHandbackCopies{ 0 };
		std::atomic<std::uint64_t> g_ngxContinuousAttempts{ 0 };
		std::atomic<std::uint64_t> g_ngxContinuousWarmupSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ngxContinuousSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ngxContinuousFailures{ 0 };
		std::atomic<std::uint64_t> g_ngxContinuousSkips{ 0 };
		std::atomic<std::uint64_t> g_ngxContinuousCopies{ 0 };
		std::atomic<std::uint64_t> g_handbackCandidateStamp{ 0 };
		std::atomic<std::uint64_t> g_ngxHandbackCandidateSerial{ 0 };
		std::atomic<std::uint64_t> g_ffxHandbackCandidateSerial{ 0 };
		std::atomic<std::uint64_t> g_ngxQualityCandidates{ 0 };
		std::atomic<std::uint64_t> g_ngxQualityCommits{ 0 };
		std::atomic<std::uint64_t> g_ngxQualityAborts{ 0 };
		// Commits plus aborts as read at the moment a capture-notification failure disables the
		// session. From outside the process this is the only way to tell that the transaction
		// finished before the disable rather than merely in the same frame: a reader comparing
		// this to the final total sees them equal when the order held and one short when it did
		// not. Written on the render thread at the three capture-disable sites, read by the
		// listener through Snapshot.
		std::atomic<std::uint64_t> g_qualityCaptureDisableAtTransactions{ 0 };
		std::atomic_bool g_ngxQualityCandidatePending{ false };
		std::atomic<std::uint64_t> g_ngxTeardownAttempts{ 0 };
		std::atomic<std::uint64_t> g_ngxTeardownSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ngxTeardownFailures{ 0 };
		std::atomic<std::uint64_t> g_ngxTeardownDeferrals{ 0 };
		std::atomic<std::uint64_t> g_ffxEvaluateAttempts{ 0 };
		std::atomic<std::uint64_t> g_ffxEvaluateSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ffxEvaluateFailures{ 0 };
		std::atomic<std::uint64_t> g_ffxEvaluateSkips{ 0 };
		std::atomic<std::uint64_t> g_ffxReassembleAttempts{ 0 };
		std::atomic<std::uint64_t> g_ffxReassembleSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ffxReassembleFailures{ 0 };
		std::atomic<std::uint64_t> g_ffxReassembleSkips{ 0 };
		std::atomic<std::uint64_t> g_ffxHandbackAttempts{ 0 };
		std::atomic<std::uint64_t> g_ffxHandbackSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ffxHandbackFailures{ 0 };
		std::atomic<std::uint64_t> g_ffxHandbackSkips{ 0 };
		std::atomic<std::uint64_t> g_ffxHandbackCopies{ 0 };
		std::atomic<std::uint64_t> g_ffxContinuousAttempts{ 0 };
		std::atomic<std::uint64_t> g_ffxContinuousWarmupSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ffxContinuousSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ffxContinuousFailures{ 0 };
		std::atomic<std::uint64_t> g_ffxContinuousSkips{ 0 };
		std::atomic<std::uint64_t> g_ffxContinuousCopies{ 0 };
		std::atomic<std::uint64_t> g_privateReassemblySerial{ 0 };
		std::atomic<std::uint64_t> g_ffxTeardownAttempts{ 0 };
		std::atomic<std::uint64_t> g_ffxTeardownSuccesses{ 0 };
		std::atomic<std::uint64_t> g_ffxTeardownFailures{ 0 };
		std::atomic<std::uint64_t> g_ffxTeardownDeferrals{ 0 };
		std::atomic<std::uint64_t> g_dynamicUpsampleSuppressionsArmed{ 0 };
		thread_local bool g_suppressDynamicUpsample = false;
		enum class PendingQualityHandback : std::uint32_t
		{
			kNone,
			kNgxOneShot,
			kNgxContinuous,
		};
		thread_local PendingQualityHandback g_pendingQualityHandback = PendingQualityHandback::kNone;
		thread_local std::uint64_t g_pendingQualityTarget = 0;
		// Carried alongside g_pendingQualityTarget from StageNgxQualityHandback (pre-HDR) to
		// CommitQualityHandbackCandidateImpl (the late ImageSpace range, same render thread,
		// same frame): whether this transaction's continuous gate has no target at all. The
		// commit-time completion check reads this explicitly rather than inferring "no
		// target" from a target of 0, for the same reason g_continuousTargetState packs an
		// unbounded bit instead of using a sentinel value - see ConfigureContinuousTarget.
		thread_local bool g_pendingQualityUnbounded = false;
		thread_local bool g_holdCurrentHDRDispatch = false;

		// kEngineColourDirty means RT1 holds the reconstruction. Nothing restores it: the
		// TemporalAA seam either publishes it (kPublishedAtSeam, history and RT4 written by
		// our draw; the late range then commits), arms kNativeFallback, where the engine's
		// own TemporalAA finishes the frame at unity, or hits one of the terminal seams.
		// Values are stable: the fault-matrix tools compare them numerically.
		enum class QualityPixelPhase : std::uint32_t
		{
			kIdle,
			kPrepared,
			kEvaluating,
			kEvaluated,
			kEngineColourDirty,
			kCommitted,
			kUnrecoverable,
			kNativeFallback,
			kPublishedAtSeam,
		};

		struct QualityPixelTransaction
		{
			QualityPixelPhase phase = QualityPixelPhase::kIdle;
			QualityFallbackSeam fallbackSeam = QualityFallbackSeam::kNone;
			std::int32_t sourceLogical = -1;
			std::uint64_t taaConfigEpoch = 0;
			std::uint64_t runtimeConfigEpoch = 0;
			ResetPolicy::WorldAdmissionTicket worldAdmission{};
			bool resetRequested = false;
			ComPtr<ID3D11Texture2D> source;
			ComPtr<ID3D11Texture2D> postTonemap;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			D3D11_TEXTURE2D_DESC postTonemapDesc{};
		};

		thread_local QualityPixelTransaction g_qualityPixels{};
		std::atomic<std::uint64_t> g_qualityPixelEvaluations{ 0 };
		std::atomic<std::uint64_t> g_qualityPixelCopies{ 0 };
		std::atomic<std::uint64_t> g_qualityPixelNativeFallbacks{ 0 };
		std::atomic<std::uint64_t> g_qualityPixelFallbackLateCloses{ 0 };
		std::atomic<std::uint64_t> g_qualityPixelFallbackAbortCloses{ 0 };
		std::atomic<std::uint64_t> g_qualityPixelFallbackSeamCounts[kQualityFallbackSeamCount]{};
		std::atomic<std::uint32_t> g_qualityPixelLastFallbackSeam{
			static_cast<std::uint32_t>(QualityFallbackSeam::kNone)
		};
		std::atomic<std::uint64_t> g_qualityPixelSeamPublishes{ 0 };
		std::atomic<std::uint64_t> g_qualityPixelSeamPublishAbortCloses{ 0 };
		std::atomic<std::uint64_t> g_qualityPixelDirtyCloses{ 0 };
		std::atomic<std::uint64_t> g_qualityPixelUnrecoverable{ 0 };
		LogGate g_fallbackLogGate{ 10000 };
		LogGate g_seamCommitGate{ 10000 };
		std::atomic<std::uint32_t> g_qualityPixelPhase{
			static_cast<std::uint32_t>(QualityPixelPhase::kIdle)
		};
		std::atomic<std::uint64_t> g_qualityFaultArmState{ 0 };
		std::atomic<std::uint32_t> g_qualityFaultLastTriggeredSeam{
			static_cast<std::uint32_t>(QualityFaultSeam::kNone)
		};
		std::atomic<std::uint64_t> g_qualityFaultArmAttempts{ 0 };
		std::atomic<std::uint64_t> g_qualityFaultArms{ 0 };
		std::atomic<std::uint64_t> g_qualityFaultRejections{ 0 };
		std::atomic<std::uint64_t> g_qualityFaultTriggers{ 0 };

		[[nodiscard]] constexpr std::uint64_t QualityFaultArmState(
			QualityFaultSeam a_seam,
			std::uint32_t a_remaining) noexcept
		{
			return (static_cast<std::uint64_t>(a_remaining) << 32) |
			       static_cast<std::uint32_t>(a_seam);
		}

		[[nodiscard]] constexpr QualityFaultSeam QualityFaultArmSeam(std::uint64_t a_state) noexcept
		{
			return static_cast<QualityFaultSeam>(static_cast<std::uint32_t>(a_state));
		}

		[[nodiscard]] constexpr std::uint32_t QualityFaultArmRemaining(std::uint64_t a_state) noexcept
		{
			return static_cast<std::uint32_t>(a_state >> 32);
		}

		std::atomic<std::uint32_t> g_threadId{ 0 };
		std::atomic<std::uint32_t> g_featureLevel{ 0 };
		std::atomic<std::uint32_t> g_width{ 0 };
		std::atomic<std::uint32_t> g_height{ 0 };
		std::atomic<std::uint32_t> g_format{ 0 };
		std::atomic<std::uint32_t> g_sampleCount{ 0 };
		std::atomic<std::uint32_t> g_lastExceptionCode{ 0 };

		std::atomic<std::int32_t> g_lastError{ S_OK };
		std::atomic<std::int32_t> g_deviceRemovedReason{ S_OK };
		std::atomic<std::uintptr_t> g_deviceAddress{ 0 };
		std::atomic<std::uintptr_t> g_contextAddress{ 0 };
		std::atomic<std::uintptr_t> g_sourceTexture{ 0 };
		std::atomic<std::uintptr_t> g_privateTexture{ 0 };

		thread_local std::uint32_t g_runtimeConfigurationEntryDepth = 0;
		thread_local std::uint64_t g_runtimeConfigurationEntryEpoch = 0;
		thread_local bool g_runtimeConfigurationOwnsAdmission = false;

		using ConfigurationEntryToken = RuntimeConfigurationEntryToken;

		[[nodiscard]] ConfigurationEntryToken EnterConfigurationEntry() noexcept
		{
			return EnterRuntimeConfiguration();
		}

		void LeaveConfigurationEntry(ConfigurationEntryToken& a_token) noexcept
		{
			LeaveRuntimeConfiguration(a_token);
		}

		[[nodiscard]] std::uint64_t TargetFromState(std::uint64_t a_state) noexcept
		{
			return a_state & kContinuousTargetMask;
		}

		[[nodiscard]] bool ContinuousTargetUnbounded(std::uint64_t a_state) noexcept
		{
			return (a_state & kContinuousTargetUnbounded) != 0;
		}

		// A short phrase, not a bare number, so a log line reads "begins a 512-frame
		// engine-handback window" or "begins an unbounded engine-handback window" - never a
		// number a reader has to already know the convention to interpret. Allocates; only
		// used at the continuous gate's own milestone log lines, never per frame.
		[[nodiscard]] std::string ContinuousTargetPhrase(bool a_unbounded, std::uint64_t a_target) noexcept
		{
			try {
				return a_unbounded ? std::string("an unbounded") : fmt::format("a {}-frame", a_target);
			} catch (...) {
				return a_unbounded ? "an unbounded" : "a bounded";
			}
		}

		[[nodiscard]] std::uint64_t LatchContinuousTargetState() noexcept
		{
			return g_continuousTargetState.fetch_or(kContinuousTargetLocked, std::memory_order_acq_rel);
		}

		void ConfigureContinuousTarget(std::uint64_t a_requested, bool a_unbounded) noexcept
		{
			// The product path never lets a_requested (diagnosticContinuousFrames, unused
			// there) leak into the stored word: the low bits stay 0 so nothing downstream can
			// read a stale or accidental number as a real bound.
			const auto requested = a_unbounded ? 0ull : (a_requested == 0 ? kDefaultContinuousTargetSuccesses : a_requested);
			const auto desired = requested | (a_unbounded ? kContinuousTargetUnbounded : 0ull);
			auto state = g_continuousTargetState.load(std::memory_order_acquire);
			while ((state & kContinuousTargetLocked) == 0) {
				if (g_continuousTargetState.compare_exchange_weak(state, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
					return;
				}
			}
			const auto latchedUnbounded = ContinuousTargetUnbounded(state);
			const auto latchedTarget = TargetFromState(state);
			if (latchedUnbounded != a_unbounded || (!latchedUnbounded && latchedTarget != requested)) {
				logger::info(FMT_STRING("pre-HDR bounded-continuous gate: target remains latched at {}; requested live change to {} is ignored after the episode began"),
					ContinuousTargetPhrase(latchedUnbounded, latchedTarget),
					ContinuousTargetPhrase(a_unbounded, requested));
			}
		}

		// SettingsPolicy.h's IsKnownMode vocabulary, mirrored to a canonical literal so
		// g_mode can be a plain atomic<const char*> - the same technique
		// dynamicGeometryDisableReason uses for a Disable() reason, safe because every value
		// is static storage. Settings::Validate() already refuses anything outside this set,
		// so the fallback is defensive, not a real path.
		[[nodiscard]] const char* CanonicalModeLiteral(std::string_view a_mode) noexcept
		{
			if (a_mode == "dlaa"sv) {
				return "dlaa";
			}
			if (a_mode == "quality"sv) {
				return "quality";
			}
			if (a_mode == "balanced"sv) {
				return "balanced";
			}
			if (a_mode == "performance"sv) {
				return "performance";
			}
			if (a_mode == "ultraperformance"sv) {
				return "ultraperformance";
			}
			return "off";
		}

		[[nodiscard]] bool SameDesc(const D3D11_TEXTURE2D_DESC& a_lhs, const D3D11_TEXTURE2D_DESC& a_rhs) noexcept
		{
			return a_lhs.Width == a_rhs.Width &&
			       a_lhs.Height == a_rhs.Height &&
			       a_lhs.MipLevels == a_rhs.MipLevels &&
			       a_lhs.ArraySize == a_rhs.ArraySize &&
			       a_lhs.Format == a_rhs.Format &&
			       a_lhs.SampleDesc.Count == a_rhs.SampleDesc.Count &&
			       a_lhs.SampleDesc.Quality == a_rhs.SampleDesc.Quality &&
			       a_lhs.Usage == a_rhs.Usage &&
			       a_lhs.BindFlags == a_rhs.BindFlags &&
			       a_lhs.CPUAccessFlags == a_rhs.CPUAccessFlags &&
			       a_lhs.MiscFlags == a_rhs.MiscFlags;
		}

		void SetQualityPixelPhase(QualityPixelPhase a_phase) noexcept
		{
			g_qualityPixels.phase = a_phase;
			g_qualityPixelPhase.store(static_cast<std::uint32_t>(a_phase), std::memory_order_release);
		}

		void ClearQualityPixelTransaction() noexcept
		{
			g_qualityPixels = {};
			g_qualityPixelPhase.store(
				static_cast<std::uint32_t>(QualityPixelPhase::kIdle),
				std::memory_order_release);
		}

		void RequestQualityResetOnce() noexcept
		{
			if (!g_qualityPixels.resetRequested) {
				g_qualityPixels.resetRequested = true;
				ResetPolicy::RequestQualityAbortReset();
			}
		}

		[[nodiscard]] bool PrepareQualityPixelTransaction(
			std::int32_t a_sourceLogical,
			ID3D11Texture2D* a_source,
			std::uint64_t a_taaConfigEpoch,
			std::uint64_t a_runtimeConfigEpoch,
			ResetPolicy::WorldAdmissionTicket a_worldAdmission) noexcept
		{
			if (g_qualityPixels.phase != QualityPixelPhase::kIdle || !g_device || !g_context ||
				!a_source || a_sourceLogical != Addr::kRT_PreTonemapColor ||
				a_taaConfigEpoch == 0 || (a_taaConfigEpoch & 1u) != 0 ||
				(a_runtimeConfigEpoch & 1u) != 0 || !a_worldAdmission.admitted ||
				a_worldAdmission.transitionGeneration == 0 || a_worldAdmission.cell == 0) {
				return false;
			}
			const auto source = RenderTargets::Lookup(a_sourceLogical);
			const auto postTonemap = RenderTargets::Lookup(Addr::kRT_PostTonemapColor);
			if (!source.valid || source.tex != a_source || !postTonemap.valid || !postTonemap.tex ||
				postTonemap.tex == a_source || a_source == g_private.Get() ||
				postTonemap.tex == g_private.Get()) {
				return false;
			}
			D3D11_TEXTURE2D_DESC sourceDesc{};
			D3D11_TEXTURE2D_DESC postTonemapDesc{};
			a_source->GetDesc(&sourceDesc);
			postTonemap.tex->GetDesc(&postTonemapDesc);
			if (sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
				sourceDesc.Width != postTonemapDesc.Width || sourceDesc.Height != postTonemapDesc.Height ||
				sourceDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
				postTonemapDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT ||
				sourceDesc.MipLevels != 1 || postTonemapDesc.MipLevels != 1 ||
				sourceDesc.ArraySize != 1 || postTonemapDesc.ArraySize != 1 ||
				sourceDesc.SampleDesc.Count != 1 || postTonemapDesc.SampleDesc.Count != 1 ||
				sourceDesc.Usage != D3D11_USAGE_DEFAULT || postTonemapDesc.Usage != D3D11_USAGE_DEFAULT ||
				sourceDesc.CPUAccessFlags != 0 || postTonemapDesc.CPUAccessFlags != 0) {
				return false;
			}

			SetQualityPixelPhase(QualityPixelPhase::kPrepared);
			g_qualityPixels.sourceLogical = a_sourceLogical;
			g_qualityPixels.taaConfigEpoch = a_taaConfigEpoch;
			g_qualityPixels.runtimeConfigEpoch = a_runtimeConfigEpoch;
			g_qualityPixels.worldAdmission = a_worldAdmission;
			g_qualityPixels.source = a_source;
			g_qualityPixels.postTonemap = postTonemap.tex;
			g_qualityPixels.sourceDesc = sourceDesc;
			g_qualityPixels.postTonemapDesc = postTonemapDesc;
			return true;
		}

		void MarkQualityEvaluationStarted() noexcept
		{
			if (g_qualityPixels.phase == QualityPixelPhase::kPrepared) {
				SetQualityPixelPhase(QualityPixelPhase::kEvaluating);
				g_qualityPixelEvaluations.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void MarkQualityEvaluationSucceeded() noexcept
		{
			if (g_qualityPixels.phase == QualityPixelPhase::kEvaluating) {
				SetQualityPixelPhase(QualityPixelPhase::kEvaluated);
			}
		}

		struct QualityCopyResult
		{
			bool began = false;
			bool engineWriteIssued = false;
			bool ended = false;
			bool stateUncertain = false;
			std::uint32_t exceptionCode = 0;
			QualityFaultSeam injectedFault = QualityFaultSeam::kNone;
		};

		void LatchQualityPixelUnrecoverable() noexcept
		{
			const bool requestReset = !g_qualityPixels.resetRequested;
			g_qualityPixels.resetRequested = true;
			// A COM Release may itself be the fault. Retain the physical references, but
			// remove every logical route by which this transaction could be reused.
			g_qualityPixels.sourceLogical = -1;
			g_qualityPixels.taaConfigEpoch = 0;
			g_qualityPixels.runtimeConfigEpoch = 0;
			g_qualityPixels.sourceDesc = {};
			g_qualityPixels.postTonemapDesc = {};
			if (g_qualityPixels.phase != QualityPixelPhase::kUnrecoverable) {
				SetQualityPixelPhase(QualityPixelPhase::kUnrecoverable);
				g_qualityPixelUnrecoverable.fetch_add(1, std::memory_order_relaxed);
			}
			g_sessionDisabled.store(true, std::memory_order_release);
			if (requestReset) {
				__try {
					ResetPolicy::RequestQualityAbortReset();
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					const auto exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
					g_lastExceptionCode.store(exceptionCode, std::memory_order_relaxed);
					g_lastError.store(static_cast<std::int32_t>(exceptionCode), std::memory_order_relaxed);
				}
			}
		}

		// The one engine-colour mutation of the transaction: the private double-wide into
		// RT1. kAfterRtBackups is still marked, with nothing inside it, where the RT1/RT3
		// backups used to be issued, so the timing report keeps its interval list and its
		// total and that interval prices the removed copies at zero.
		[[nodiscard]] QualityCopyResult CopyQualityPixelsToEngine(
			ID3D11Texture2D* a_privateOutput) noexcept
		{
			QualityCopyResult result{};
			if (g_qualityPixels.phase != QualityPixelPhase::kEvaluated || !a_privateOutput ||
				!g_qualityPixels.source || !g_qualityPixels.postTonemap) {
				return result;
			}
			const auto source = RenderTargets::Lookup(g_qualityPixels.sourceLogical);
			const auto postTonemap = RenderTargets::Lookup(Addr::kRT_PostTonemapColor);
			if (!source.valid || source.tex != g_qualityPixels.source.Get() ||
				!postTonemap.valid || postTonemap.tex != g_qualityPixels.postTonemap.Get()) {
				return result;
			}
			if (ConsumeQualityFault(QualityFaultSeam::kPreBackup)) {
				result.injectedFault = QualityFaultSeam::kPreBackup;
				return result;
			}

			__try {
				result.began = g_isolation.Begin();
				if (result.began) {
					__try {
						Telemetry::Mark(Telemetry::MarkPoint::kAfterBackupIsolationBegin);
						g_context->ClearState();
						Telemetry::Mark(Telemetry::MarkPoint::kAfterBackupClearState);
						Telemetry::Mark(Telemetry::MarkPoint::kAfterRtBackups);
						if (ConsumeQualityFault(QualityFaultSeam::kPostBackup)) {
							result.injectedFault = QualityFaultSeam::kPostBackup;
						} else {
							// From this point onward an SEH may mean the driver accepted the write
							// even if CopyResource never returns to set a trailing flag. Classify RT1
							// as dirty before issuing it: a dirty RT1 is finished by the native
							// fallback, never restored, so the classification only has to err
							// towards "written".
							SetQualityPixelPhase(QualityPixelPhase::kEngineColourDirty);
							result.engineWriteIssued = true;
							g_context->CopyResource(g_qualityPixels.source.Get(), a_privateOutput);
							Telemetry::Mark(Telemetry::MarkPoint::kAfterPrivateToEngineCopy);
							if (ConsumeQualityFault(QualityFaultSeam::kPostRt1)) {
								result.injectedFault = QualityFaultSeam::kPostRt1;
							}
						}
						Telemetry::Mark(Telemetry::MarkPoint::kBeforeBackupIsolationEnd);
					} __finally {
						result.ended = g_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (g_isolation.StateUncertain()) {
				static_cast<void>(g_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			// The real End() above already ran and genuinely restored the host context;
			// this only overwrites the flag the caller reads, after the fact, so RT1 is
			// dirty underneath a reported close failure exactly as a genuine one would
			// leave it. Gated on engineWriteIssued so an arm that lands on a frame where
			// Begin() itself failed - the copy never issuing - is not silently consumed
			// on a frame that would prove nothing.
			if (result.engineWriteIssued &&
				ConsumeQualityFault(QualityFaultSeam::kCopyCloseFailure)) {
				result.injectedFault = QualityFaultSeam::kCopyCloseFailure;
				result.ended = false;
			}
			if (result.engineWriteIssued) {
				g_qualityPixelCopies.fetch_add(1, std::memory_order_relaxed);
			}
			return result;
		}

		[[nodiscard]] bool AbortQualityPixelsImpl() noexcept
		{
			if (D3D11StateIsolation::HostStateUncertain()) {
				LatchQualityPixelUnrecoverable();
				return false;
			}
			if (g_qualityPixels.phase == QualityPixelPhase::kPublishedAtSeam) {
				// The seam draw wrote the history the next frame reads and RT4. A watchdog
				// or an abnormal return closing this frame has nothing to repair and owes no
				// reset; only the commit accounting is lost.
				static_cast<void>(TAAReplacement::ReleasePublishedQualityEpisode());
				g_qualityPixelSeamPublishAbortCloses.fetch_add(1, std::memory_order_relaxed);
				ClearQualityPixelTransaction();
				return true;
			}
			const auto taaAbort = TAAReplacement::AbortPendingQualityEpisode();
			if (taaAbort == TAAReplacement::AbortResult::kUnrecoverable) {
				LatchQualityPixelUnrecoverable();
				return false;
			}
			if (g_qualityPixels.phase == QualityPixelPhase::kIdle) {
				return true;
			}
			if (g_qualityPixels.phase == QualityPixelPhase::kUnrecoverable) {
				// Publication has already crossed a point from which exact pixel/state
				// recovery cannot be proved. Never let a later stale-watchdog abort erase
				// that classification and accidentally re-arm quality.
				g_sessionDisabled.store(true, std::memory_order_release);
				return false;
			}
			if (g_qualityPixels.phase == QualityPixelPhase::kNativeFallback) {
				// A fallback frame closed by an abort path rather than the late range: the
				// engine finished it on its own, so there is nothing to restore and no
				// history to repair, only a different close count.
				g_qualityPixelFallbackAbortCloses.fetch_add(1, std::memory_order_relaxed);
				ClearQualityPixelTransaction();
				return true;
			}
			if (g_qualityPixels.phase == QualityPixelPhase::kEngineColourDirty) {
				// RT1 holds the reconstruction and nothing puts the reduced raster back.
				// Only a caller that could not arm the fallback reaches this, and the
				// reduced bracket then closes over a full-resolution RT1 for one frame.
				g_qualityPixelDirtyCloses.fetch_add(1, std::memory_order_relaxed);
			}
			if (g_qualityPixels.phase != QualityPixelPhase::kPrepared) {
				RequestQualityResetOnce();
			}
			ClearQualityPixelTransaction();
			return true;
		}

		[[nodiscard]] bool AbortQualityPixels() noexcept
		{
			// One centralized close for every private/pre-publication failure path. This
			// emits only a query End on the render thread; the invalid episode is drained
			// later with DONOTFLUSH and never enters the timing aggregates.
			Telemetry::Abort();
			bool restored = false;
			__try {
				restored = AbortQualityPixelsImpl();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				const auto exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
				g_lastExceptionCode.store(exceptionCode, std::memory_order_relaxed);
				g_lastError.store(static_cast<std::int32_t>(exceptionCode), std::memory_order_relaxed);
				// This shell includes prepared/evaluated cleanup, whose ComPtr release can
				// fault even though no engine target was ever touched.
				LatchQualityPixelUnrecoverable();
				restored = false;
			}
			return restored;
		}

		[[nodiscard]] bool AbortQualityPixelsForDispatch() noexcept
		{
			const bool restored = AbortQualityPixels();
			if (!restored) {
				g_holdCurrentHDRDispatch = true;
			}
			return restored;
		}

		// After the fallback is armed inside the HDR dispatch, the outer hook still has to
		// mark the neutralization pending exactly as it does for a publishing frame, so the
		// same one-shot request carries it.
		void ArmNativeFallbackDispatch() noexcept
		{
			g_suppressDynamicUpsample = true;
			g_dynamicUpsampleSuppressionsArmed.fetch_add(1, std::memory_order_relaxed);
		}

		[[nodiscard]] bool ParseQuality(std::string_view a_text, Backend::QualityMode& a_out) noexcept
		{
			if (a_text == "dlaa"sv) {
				a_out = Backend::QualityMode::kNativeAA;
				return true;
			}
			if (a_text == "quality"sv) {
				a_out = Backend::QualityMode::kQuality;
				return true;
			}
			if (a_text == "balanced"sv) {
				a_out = Backend::QualityMode::kBalanced;
				return true;
			}
			if (a_text == "performance"sv) {
				a_out = Backend::QualityMode::kPerformance;
				return true;
			}
			if (a_text == "ultraperformance"sv) {
				a_out = Backend::QualityMode::kUltraPerformance;
				return true;
			}
			return false;
		}

		[[nodiscard]] std::string_view QualityName(Backend::QualityMode a_quality) noexcept
		{
			switch (a_quality) {
			case Backend::QualityMode::kNativeAA:
				return "dlaa"sv;
			case Backend::QualityMode::kQuality:
				return "quality"sv;
			case Backend::QualityMode::kBalanced:
				return "balanced"sv;
			case Backend::QualityMode::kPerformance:
				return "performance"sv;
			case Backend::QualityMode::kUltraPerformance:
				return "ultraperformance"sv;
			default:
				return "unknown"sv;
			}
		}

		[[nodiscard]] bool BuildNgxConfig(
			const D3D11_TEXTURE2D_DESC& a_sourceDesc,
			Backend::Config& a_out,
			bool a_subrects) noexcept
		{
			if (a_sourceDesc.Width < 2 || (a_sourceDesc.Width & 1u) != 0 || a_sourceDesc.Height == 0) {
				return false;
			}

			a_out = {};
			a_out.outputWidth = a_sourceDesc.Width / 2u;
			a_out.outputHeight = a_sourceDesc.Height;
			a_out.renderWidth = a_out.outputWidth;
			a_out.renderHeight = a_out.outputHeight;
			const auto quality = g_quality.load(std::memory_order_acquire);
			if (!quality || !ParseQuality(*quality, a_out.quality)) {
				return false;
			}

			const auto dynamic = DynamicResolution::Snapshot();
			if (dynamic.applied) {
				constexpr float tolerance = 0.000001f;
				if (!dynamic.currentValuesValid ||
					!std::isfinite(dynamic.currentWidth) || !std::isfinite(dynamic.currentHeight) ||
					dynamic.currentWidth <= 0.0f || dynamic.currentWidth > 1.0f ||
					dynamic.currentHeight <= 0.0f || dynamic.currentHeight > 1.0f ||
					std::abs(dynamic.currentWidth - dynamic.currentTargetWidth) > tolerance ||
					std::abs(dynamic.currentHeight - dynamic.currentTargetHeight) > tolerance ||
					std::abs(dynamic.currentWidth - dynamic.requestedScaleX) > tolerance ||
					std::abs(dynamic.currentHeight - dynamic.requestedScaleY) > tolerance) {
					return false;
				}
				a_out.renderWidth = static_cast<std::uint32_t>(
					std::floor(static_cast<double>(a_out.outputWidth) * dynamic.currentWidth));
				a_out.renderHeight = static_cast<std::uint32_t>(
					std::floor(static_cast<double>(a_out.outputHeight) * dynamic.currentHeight));
			}

			const bool reduced = a_out.renderWidth < a_out.outputWidth || a_out.renderHeight < a_out.outputHeight;
			if (a_out.renderWidth == 0 || a_out.renderHeight == 0 ||
				(a_out.quality == Backend::QualityMode::kNativeAA) != !reduced) {
				return false;
			}

			const auto preset = g_ngxPreset.load(std::memory_order_acquire);
			a_out.preset = preset ? *preset : "auto";
			// Raw NGX's own sharpen/soften pass is deprecated: DLSS Programming Guide
			// rev 310.6.0 section 3.11 points integrators at the NIS SDK instead. The
			// `sharpness` setting belongs to our own RCAS pass at the TemporalAA seam
			// (TAAReplacement), so the vendor config is always zero here.
			// NgxD3D11Backend::CreateViews and Evaluate each enforce zero independently;
			// both of those are defence in depth and stay.
			a_out.sharpness = 0.0f;
			a_out.hdr = true;
			a_out.invertedDepth = false;
			// The motion vectors at logical 35 come from the unjittered matrices at
			// ViewData +0x110/+0x190, so this stays false and NGX's MVJittered feature
			// flag stays unset. Backend-owned jitter does not change that.
			a_out.jitteredMV = false;
			a_out.lowResMV = true;
			a_out.autoExposure = true;
			a_out.preExposure = 1.0f;
			a_out.exposureScale = 1.0f;
			// Resolved by Configure against the NGX probe and loaded once per dispatch by
			// the caller, so every builder in a frame sees one value. Part of Evaluate's
			// preparation key, so a reload that flips it re-creates the feature handles.
			a_out.enableSubrects = a_subrects;
			return true;
		}

		enum class Gate
		{
			roundTrip,
			stereoNull,
			ngxCreate,
			ngxEvaluate,
			ngxReassemble,
			ngxHandback,
			ngxContinuous,
			ffxEvaluate,
			ffxReassemble,
			ffxHandback,
			ffxContinuous,
		};

		[[nodiscard]] bool IsVendorGate(Gate a_gate) noexcept
		{
			return a_gate == Gate::ngxCreate || a_gate == Gate::ngxEvaluate ||
			       a_gate == Gate::ngxReassemble || a_gate == Gate::ngxHandback ||
			       a_gate == Gate::ngxContinuous || a_gate == Gate::ffxEvaluate ||
			       a_gate == Gate::ffxReassemble || a_gate == Gate::ffxHandback ||
			       a_gate == Gate::ffxContinuous;
		}

		inline constexpr std::uint64_t kHandbackSourceBits = 2;
		inline constexpr std::uint64_t kHandbackSourceMask =
			(1ull << kHandbackSourceBits) - 1;

		// One release-published word keeps source and serial coherent for both the
		// render-thread consumer and listener-thread telemetry. A process cannot
		// approach the 2^62 candidate-frame range reserved by this packing.
		void PublishHandbackCandidate(
			HandbackSource a_source,
			std::uint64_t a_serial) noexcept
		{
			const auto packed = (a_serial << kHandbackSourceBits) |
			                    static_cast<std::uint64_t>(a_source);
			g_handbackCandidateStamp.store(packed, std::memory_order_release);
		}

		void ClearHandbackCandidate(HandbackStamp a_candidate) noexcept
		{
			if (a_candidate.source == HandbackSource::kNone || a_candidate.serial == 0) {
				return;
			}
			auto expected = (a_candidate.serial << kHandbackSourceBits) |
			                static_cast<std::uint64_t>(a_candidate.source);
			static_cast<void>(g_handbackCandidateStamp.compare_exchange_strong(
				expected,
				0,
				std::memory_order_acq_rel,
				std::memory_order_acquire));
		}

		void StageNgxQualityHandback(
			PendingQualityHandback a_kind,
			std::uint64_t a_target,
			bool a_unbounded) noexcept
		{
			const auto serial = g_ngxHandbackCandidateSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
			g_pendingQualityHandback = a_kind;
			g_pendingQualityTarget = a_target;
			g_pendingQualityUnbounded = a_unbounded;
			g_ngxQualityCandidates.fetch_add(1, std::memory_order_relaxed);
			g_ngxQualityCandidatePending.store(true, std::memory_order_release);
			PublishHandbackCandidate(HandbackSource::kNgx, serial);
		}

		void ScheduleOwnedVendorTeardown() noexcept
		{
			const auto owner = Evaluate::Snapshot();
			if (!owner.ownsBackendState) {
				return;
			}
			if (owner.backend == reinterpret_cast<std::uintptr_t>(Backend::Find("ngx"sv))) {
				g_ngxTeardownPending.store(true, std::memory_order_release);
			} else if (owner.backend == reinterpret_cast<std::uintptr_t>(Backend::Find("ffx"sv))) {
				g_ffxTeardownPending.store(true, std::memory_order_release);
			}
		}

		// Every gate this session can arm, read live. Off in ComputeSessionState/Snapshot
		// means none of these; the re-arm resolution below uses the same test to catch a
		// settings reload that wiped them mid cool-down.
		[[nodiscard]] bool AnyGateEnabledNow() noexcept
		{
			return g_enabled.load(std::memory_order_acquire) ||
			       g_stereoNullEnabled.load(std::memory_order_acquire) ||
			       g_ngxCreateEnabled.load(std::memory_order_acquire) ||
			       g_ngxEvaluateEnabled.load(std::memory_order_acquire) ||
			       g_ngxReassembleEnabled.load(std::memory_order_acquire) ||
			       g_ngxHandbackEnabled.load(std::memory_order_acquire) ||
			       g_ngxContinuousEnabled.load(std::memory_order_acquire) ||
			       g_ffxEvaluateEnabled.load(std::memory_order_acquire) ||
			       g_ffxReassembleEnabled.load(std::memory_order_acquire) ||
			       g_ffxHandbackEnabled.load(std::memory_order_acquire) ||
			       g_ffxContinuousEnabled.load(std::memory_order_acquire);
		}

		// The recovery predicate. Every one of Disable's ~47 call sites and the handful of
		// raw g_sessionDisabled stores elsewhere in this file (LatchQualityPixelUnrecoverable,
		// the capture-notify-failure sites, a failed render-thread teardown) sets the same
		// atomic; this is the one place that decides what a *new* disable event means. It is
		// deliberately not called from any of those sites - see TickSessionRecovery below for
		// why the read has to happen later, once the frame that faulted has fully unwound.
		// Called twice in one recovery cycle - once at the suspend edge, and again
		// immediately before a re-arm actually clears the latch. The suspend-time call
		// decides whether to suspend at all; the re-arm-time call decides whether to
		// actually re-arm. A sibling latch or a removed device can each become true only
		// during the cool-down between those two calls, so both calls are load-bearing even
		// though they run the same five checks.
		[[nodiscard]] RecoveryRefusal EvaluateRecoveryPredicate(bool a_failSoft) noexcept
		{
			// 4. failSoft off means every fault is terminal - today's behaviour, unchanged.
			// Checked first so the other four predicates, which do real work, are skipped
			// on the common (failSoft = false) path.
			if (!a_failSoft) {
				return RecoveryRefusal::kFailSoftDisabled;
			}
			// 1. The quality pixel transaction must have closed, one way or the other. Read
			// here rather than at the store site: a native-fallback or seam-publish close
			// runs later in the same frame's pipeline (the TemporalAA seam, the late
			// ImageSpace range), strictly before HDRRoundTrip's own dispatch is entered
			// again, so by the time this runs the phase is the transaction's settled
			// outcome. kUnrecoverable is a phase too, and it never clears on its own, which
			// is exactly why it refuses here rather than needing a special case.
			const auto phase = static_cast<QualityPixelPhase>(
				g_qualityPixelPhase.load(std::memory_order_acquire));
			if (phase != QualityPixelPhase::kIdle) {
				return RecoveryRefusal::kMidTransaction;
			}
			// 2. The device itself has to still be there. Queried live: GetDeviceRemovedReason
			// is the one D3D11 call defined to be safe after removal, and the cached
			// g_deviceRemovedReason is not refreshed by every one of the raw-store sites.
			if (!g_device || FAILED(g_device->GetDeviceRemovedReason())) {
				return RecoveryRefusal::kDeviceRemoved;
			}
			// 3. TAAReplacement's and Hooks' own one-way latches have no recovery of their
			// own yet - the suspend machine here covers only this file's g_sessionDisabled. A fault
			// that tripped either one leaves a session that cannot publish even if this
			// latch re-arms, so read-only is the correct and complete treatment here.
			if (TAAReplacement::SessionDisabled() || Hooks::DynamicGeometrySessionDisabled()) {
				return RecoveryRefusal::kSiblingLatchTripped;
			}
			// 5. Bounded, so a session that keeps faulting eventually settles on Terminal
			// instead of re-arming forever.
			if (g_rearmCount.load(std::memory_order_acquire) >= kRearmsPerSession) {
				return RecoveryRefusal::kRearmBudgetExhausted;
			}
			return RecoveryRefusal::kNone;
		}

		// Render-thread only, called from TickSessionRecovery once the cool-down has
		// counted down to zero. Retried every frame until it resolves one way or the other -
		// the countdown reaching zero is necessary but not sufficient for a re-arm: the
		// teardown scheduled at suspend runs on its own eligible-frame cadence and can still
		// be retrying past kTeardownMaxAttempts well after frame 90.
		//
		// Returns whether it cleared g_sessionDisabled. The caller needs that answer rather
		// than the value it loaded at the top of the tick: the preparation path runs later
		// in this same dispatch and can fault again, and only a caller that knows the latch
		// went false can record the edge state that makes that second fault a fresh
		// false -> true edge next frame.
		[[nodiscard]] bool TryResolveRearm() noexcept
		{
			const bool ngxPending = g_ngxTeardownPending.load(std::memory_order_acquire);
			const bool ffxPending = g_ffxTeardownPending.load(std::memory_order_acquire);
			if (ngxPending || ffxPending) {
				// Still retrying (or still waiting for the next eligible HDR frame to run
				// the attempt at all). Nothing to classify yet; come back next frame.
				return false;
			}
			const bool teardownSucceeded =
				g_recoveryTeardownTarget == RecoveryTeardownTarget::kNone ||
				(g_recoveryTeardownTarget == RecoveryTeardownTarget::kNgx &&
					g_ngxTeardownCompleted.load(std::memory_order_acquire)) ||
				(g_recoveryTeardownTarget == RecoveryTeardownTarget::kFfx &&
					g_ffxTeardownCompleted.load(std::memory_order_acquire));
			// Pending is false but the completed flag this cycle cleared at suspend never
			// got set: the teardown reached its failure branch instead of its success one
			// (HDRRoundTrip.cpp's owned-teardown handling - reset/deactivate/end all false-
			// checked together). That branch leaves g_private/g_privateDesc/g_privateReady,
			// and any live vendor histories, exactly as they were when the fault fired - not
			// provably safe to hand back to a fresh preparation pass. g_privateReady is
			// checked too: a round-trip session can hold the private texture with no backend
			// ever owned, a case ScheduleOwnedVendorTeardown correctly does nothing for, and
			// that texture is equally not ours to reuse blind.
			if (!teardownSucceeded || g_privateReady.load(std::memory_order_acquire)) {
				g_recoveryRefusal.store(
					static_cast<std::uint32_t>(RecoveryRefusal::kTeardownFailed),
					std::memory_order_release);
				g_recoverySuspensionActive.store(false, std::memory_order_release);
				logger::error("pre-HDR quality session terminal: owned vendor teardown did not complete cleanly, so the private texture, views and any vendor histories could not be proven released; the session will not re-arm"sv);
				return false;
			}
			// Re-evaluate fresh, right before the latch actually clears, rather than trusting
			// the verdict from the suspend edge 90 frames ago. A sibling latch tripping or
			// the device going away mid cool-down must refuse here even though every
			// predicate held at suspend time.
			const bool failSoft = g_failSoft.load(std::memory_order_acquire);
			const auto freshRefusal = EvaluateRecoveryPredicate(failSoft);
			if (freshRefusal != RecoveryRefusal::kNone) {
				g_recoveryRefusal.store(static_cast<std::uint32_t>(freshRefusal), std::memory_order_release);
				g_recoverySuspensionActive.store(false, std::memory_order_release);
				// failSoft = false must stay silent - see the suspend-edge branch in
				// TickSessionRecovery for the matching guard and why.
				if (freshRefusal != RecoveryRefusal::kFailSoftDisabled) {
					logger::info(FMT_STRING("pre-HDR quality session terminal at re-arm: {} - held at suspend, no longer holds"),
						RecoveryRefusalName(freshRefusal));
				}
				return false;
			}
			g_rearmCount.fetch_add(1, std::memory_order_relaxed);
			// A settings reload observed while disabled runs AbortConfiguration, which
			// clears every gate-enabled flag unconditionally - a condition that holds
			// throughout this entire cool-down. Detect that here rather than clearing the
			// latch blind: ComputeSessionState reports kOff either way (no gate enabled), so
			// recoveryRefusal is the only place a run can tell "re-armed into nothing" apart
			// from a session that was simply never configured.
			if (!AnyGateEnabledNow()) {
				g_recoveryRefusal.store(
					static_cast<std::uint32_t>(RecoveryRefusal::kGatesClearedDuringCooldown),
					std::memory_order_release);
				logger::warn(FMT_STRING("pre-HDR quality session re-armed ({} of {} this session) with every gate clear; a settings reload during the cool-down cleared them, so nothing will prepare until reconfigured"),
					g_rearmCount.load(std::memory_order_relaxed),
					kRearmsPerSession);
			} else {
				g_recoveryRefusal.store(
					static_cast<std::uint32_t>(RecoveryRefusal::kNone),
					std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR quality session re-armed ({} of {} this session); the existing preparation path runs again on the next eligible frame"),
					g_rearmCount.load(std::memory_order_relaxed),
					kRearmsPerSession);
			}
			// This cycle is over, so the next one starts from nothing: the target says
			// which lifetime "completed" latch to read, and leaving the spent one standing
			// is what let a second cool-down read the first cycle's outcome and re-arm onto
			// a backend whose own teardown had failed. The suspend edge sets both again.
			g_recoveryTeardownTarget = RecoveryTeardownTarget::kNone;
			g_recoverySuspensionActive.store(false, std::memory_order_release);
			// Clearing this is the entire re-arm: every gate below already checks
			// g_sessionDisabled before it runs, and a succeeded teardown already reset
			// g_privateReady, so the existing preparation path (EnsurePrivate, the
			// create-only gate) runs again from here exactly as it does on a cold start.
			// Whether the 16-frame warm-up repeats depends on whether it had already
			// finished before this fault: g_ngxContinuousWarmupSuccesses is a lifetime
			// counter the recovery machine does not touch.
			g_sessionDisabled.store(false, std::memory_order_release);
			return true;
		}

		// Render-thread only, once per verified kMain HDR dispatch. A newly observed
		// false -> true edge on g_sessionDisabled is classified exactly once: Suspended
		// schedules the existing safe-release vendor teardown and starts the cool-down
		// (the same path a completed continuous gate already uses; no second preparation
		// route exists), Terminal only records which predicate refused. Counting down lives
		// here too; resolving the countdown into an actual re-arm is TryResolveRearm above,
		// so the whole recovery lifecycle has one render-thread owner and cannot be driven
		// from two places at once.
		void TickSessionRecovery() noexcept
		{
			const bool disabledNow = g_sessionDisabled.load(std::memory_order_acquire);
			// What this tick leaves behind for the next one's edge test. Only the re-arm
			// branch below moves it away from disabledNow, and only because the re-arm
			// clears the latch inside this same tick.
			bool observedDisabled = disabledNow;
			if (disabledNow && !g_recoveryLastObservedDisabled) {
				const bool failSoft = g_failSoft.load(std::memory_order_acquire);
				const auto refusal = EvaluateRecoveryPredicate(failSoft);
				if (refusal == RecoveryRefusal::kNone) {
					g_suspensionCount.fetch_add(1, std::memory_order_relaxed);
					g_suspendFramesLeft.store(kRearmCooldownFrames, std::memory_order_release);
					g_recoveryRefusal.store(
						static_cast<std::uint32_t>(RecoveryRefusal::kNone),
						std::memory_order_release);
					// Remember which backend's teardown (if any) this cool-down is
					// waiting on, and clear its lifetime "completed" latch so TryResolveRearm
					// reads this cycle's outcome rather than a survivor from an earlier
					// engage/disengage cycle in the same session (those latches are never
					// otherwise reset - see their declarations).
					const auto owner = Evaluate::Snapshot();
					if (owner.ownsBackendState &&
						owner.backend == reinterpret_cast<std::uintptr_t>(Backend::Find("ngx"sv))) {
						g_recoveryTeardownTarget = RecoveryTeardownTarget::kNgx;
						g_ngxTeardownCompleted.store(false, std::memory_order_release);
					} else if (owner.ownsBackendState &&
						owner.backend == reinterpret_cast<std::uintptr_t>(Backend::Find("ffx"sv))) {
						g_recoveryTeardownTarget = RecoveryTeardownTarget::kFfx;
						g_ffxTeardownCompleted.store(false, std::memory_order_release);
					} else {
						g_recoveryTeardownTarget = RecoveryTeardownTarget::kNone;
					}
					ScheduleOwnedVendorTeardown();
					// Published last, after the cool-down and the teardown target are both
					// in place: the settings threads read it to decide whether a reload may
					// clear this session's gates, and a reload that sees true must be
					// looking at a fully-formed cool-down.
					g_recoverySuspensionActive.store(true, std::memory_order_release);
					logger::info(FMT_STRING("pre-HDR quality session suspended after a recoverable fault; re-arm {} of {} begins after a {}-frame cool-down"),
						g_rearmCount.load(std::memory_order_relaxed) + 1,
						kRearmsPerSession,
						kRearmCooldownFrames);
				} else {
					g_suspendFramesLeft.store(0, std::memory_order_release);
					g_recoveryRefusal.store(static_cast<std::uint32_t>(refusal), std::memory_order_release);
					g_recoverySuspensionActive.store(false, std::memory_order_release);
					// failSoft = false must reproduce the pre-recovery fault-matrix verdict
					// bit for bit - Disable() (or the teardown-failure branch) already
					// logged this fault and set g_sessionDisabled on its own. This predicate
					// existing at all must not add a further log line on that path; the state
					// fields above are enough for /state to still report Terminal.
					if (refusal != RecoveryRefusal::kFailSoftDisabled) {
						logger::info(FMT_STRING("pre-HDR quality session terminal: {} refused re-arm"),
							RecoveryRefusalName(refusal));
					}
				}
			} else if (disabledNow && g_suspendFramesLeft.load(std::memory_order_acquire) > 0) {
				// The teardown scheduled at suspend runs on its own on the frames in
				// between - OnDispatchImpl's teardown branch checks g_sessionDisabled
				// itself and is not gated on this cool-down.
				g_suspendFramesLeft.fetch_sub(1, std::memory_order_acq_rel);
			} else if (disabledNow && g_suspendFramesLeft.load(std::memory_order_acquire) == 0 &&
				static_cast<RecoveryRefusal>(g_recoveryRefusal.load(std::memory_order_acquire)) ==
					RecoveryRefusal::kNone) {
				// The cool-down has expired (or a previous attempt this frame group found
				// the teardown not yet settled). disabledNow && suspendFramesLeft == 0 &&
				// refusal == kNone identifies exactly this window - it cannot be confused
				// with "never suspended" (disabledNow is false there) or "already terminal"
				// (refusal is already set there).
				if (TryResolveRearm()) {
					// The latch cleared inside that call, so disabledNow is already stale:
					// this dispatch runs its preparation path a few hundred lines below and
					// can fault again before the next tick. Recording false is what makes
					// that second fault a fresh false -> true edge - recording the loaded
					// true instead skipped the whole suspend classification for it and left
					// this branch re-running every frame on the previous cycle's teardown
					// bookkeeping.
					observedDisabled = false;
				}
			}
			g_recoveryLastObservedDisabled = observedDisabled;
		}

		void Disable(
			std::int32_t a_error,
			std::uint32_t a_exception,
			std::string_view a_reason,
			Gate a_gate) noexcept
		{
			g_lastError.store(a_error, std::memory_order_relaxed);
			g_lastExceptionCode.store(a_exception, std::memory_order_relaxed);
			if (a_gate == Gate::stereoNull) {
				g_stereoNullFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxCreate) {
				g_ngxCreateFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxEvaluate) {
				g_ngxEvaluateFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxReassemble) {
				g_ngxReassembleFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxHandback) {
				g_ngxHandbackFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxContinuous) {
				g_ngxContinuousFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ffxEvaluate) {
				g_ffxEvaluateFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ffxReassemble) {
				g_ffxReassembleFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ffxHandback) {
				g_ffxHandbackFailures.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ffxContinuous) {
				g_ffxContinuousFailures.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_failures.fetch_add(1, std::memory_order_relaxed);
			}
			if (IsVendorGate(a_gate)) {
				ScheduleOwnedVendorTeardown();
			}
			const bool already = g_sessionDisabled.exchange(true, std::memory_order_acq_rel);
			if (!already) {
				logger::error(FMT_STRING("pre-HDR development gate disabled for this session: {} (HRESULT {:08X}, exception {:08X}); vanilla HDR continues"),
					a_reason,
					static_cast<std::uint32_t>(a_error),
					a_exception);
			}
		}

		void RecordSkip(Gate a_gate) noexcept
		{
			if (a_gate == Gate::stereoNull) {
				g_stereoNullSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxCreate) {
				g_ngxCreateSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxEvaluate) {
				g_ngxEvaluateSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxReassemble) {
				g_ngxReassembleSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxHandback) {
				g_ngxHandbackSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ngxContinuous) {
				g_ngxContinuousSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ffxEvaluate) {
				g_ffxEvaluateSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ffxReassemble) {
				g_ffxReassembleSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ffxHandback) {
				g_ffxHandbackSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (a_gate == Gate::ffxContinuous) {
				g_ffxContinuousSkips.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_skips.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void ProbeIsolation() noexcept
		{
			bool expected = false;
			if (!g_isolationAttempted.compare_exchange_strong(
					expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
				return;
			}

			g_isolationAttempts.fetch_add(1, std::memory_order_relaxed);
			g_threadId.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
			g_device = Engine::Device();
			g_context = Engine::ImmediateContext();
			g_deviceAddress.store(reinterpret_cast<std::uintptr_t>(g_device), std::memory_order_relaxed);
			g_contextAddress.store(reinterpret_cast<std::uintptr_t>(g_context), std::memory_order_relaxed);

			const bool ready = g_isolation.Initialize(g_device, g_context);
			const auto hr = g_isolation.LastError();
			const auto feature = static_cast<std::uint32_t>(g_isolation.ChosenFeatureLevel());
			g_lastError.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);
			g_featureLevel.store(feature, std::memory_order_relaxed);
			g_isolationReady.store(ready, std::memory_order_release);

			if (ready) {
				logger::info(FMT_STRING("d3d11 state isolation: live context-state capability ready on primary render thread {}, feature level {:X}, device {:016X}, context {:016X}; no state activated and no pixels changed"),
					g_threadId.load(std::memory_order_relaxed),
					feature,
					reinterpret_cast<std::uintptr_t>(g_device),
					reinterpret_cast<std::uintptr_t>(g_context));
			} else {
				logger::warn(FMT_STRING("d3d11 state isolation unavailable on primary render thread {}: HRESULT {:08X}, device {:016X}, context {:016X}; future render mutation will decline"),
					g_threadId.load(std::memory_order_relaxed),
					static_cast<std::uint32_t>(hr),
					reinterpret_cast<std::uintptr_t>(g_device),
					reinterpret_cast<std::uintptr_t>(g_context));
			}
		}

		[[nodiscard]] bool EnsurePrivate(const RenderTargets::PhysRT& a_source, Gate a_gate, bool a_subrects) noexcept
		{
			if (!a_source.valid || !a_source.tex || a_source.width == 0 || a_source.height == 0) {
				Disable(E_INVALIDARG, 0, "logical source 1 did not resolve to a complete live texture", a_gate);
				return false;
			}

			D3D11_TEXTURE2D_DESC desc{};
			a_source.tex->GetDesc(&desc);
			if (desc.Width != a_source.width || desc.Height != a_source.height ||
				static_cast<std::uint32_t>(desc.Format) != a_source.format ||
				desc.Width == 0 || desc.Height == 0 || desc.MipLevels == 0 || desc.ArraySize == 0 ||
				desc.SampleDesc.Count == 0 || desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0) {
				Disable(E_INVALIDARG, 0, "logical source 1 properties and its D3D11 texture descriptor disagreed or were not copy-safe", a_gate);
				return false;
			}

			g_sourceTexture.store(reinterpret_cast<std::uintptr_t>(a_source.tex), std::memory_order_relaxed);
			// On the sub-rect path the vendor writes each eye straight into this texture
			// through an unordered-access view, so the private copy must carry that bind
			// whatever its source carries. VERIFIED (self-check, deferred, 1.2.72):
			// logical 1 already resolves an unordered-access view, so its descriptor has
			// the bind and this OR changes nothing there; it stays because the vendor's
			// requirement is ours to meet rather than the engine's to happen to satisfy.
			// The bind flags are the one field where the private copy may depart from
			// its source; the format, extent, mips, array and sample count it must share
			// for CopyResource stay verbatim, and the comparison below sees the adjusted
			// descriptor so this does not rebuild every frame.
			if (a_subrects) {
				desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			}
			if (g_private && SameDesc(g_privateDesc, desc)) {
				return true;
			}

			ComPtr<ID3D11Texture2D> replacement;
			const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, replacement.GetAddressOf());
			if (FAILED(hr) || !replacement) {
				Disable(static_cast<std::int32_t>(FAILED(hr) ? hr : E_FAIL), 0, "descriptor-matched private texture creation failed", a_gate);
				return false;
			}

			g_private = std::move(replacement);
			g_privateDesc = desc;
			g_width.store(desc.Width, std::memory_order_relaxed);
			g_height.store(desc.Height, std::memory_order_relaxed);
			g_format.store(static_cast<std::uint32_t>(desc.Format), std::memory_order_relaxed);
			g_sampleCount.store(desc.SampleDesc.Count, std::memory_order_relaxed);
			g_privateTexture.store(reinterpret_cast<std::uintptr_t>(g_private.Get()), std::memory_order_relaxed);
			g_resourceBuilds.fetch_add(1, std::memory_order_relaxed);
			g_privateReady.store(true, std::memory_order_release);
			logger::info(FMT_STRING("pre-HDR diagnostic: created private copy target {}x{}, dxgi {}, samples {}, texture {:016X}"),
				desc.Width,
				desc.Height,
				static_cast<std::uint32_t>(desc.Format),
				desc.SampleDesc.Count,
				reinterpret_cast<std::uintptr_t>(g_private.Get()));
			return true;
		}

		struct CopyResult
		{
			bool began = false;
			bool copied = false;
			bool ended = false;
			bool stateUncertain = false;
			std::uint32_t exceptionCode = 0;
		};

		// No C++ objects requiring unwinding may enter this function: the explicit
		// __finally is what guarantees host-state restoration if a D3D call raises SEH.
		[[nodiscard]] CopyResult CopyIsolated(
			D3D11StateIsolation::Context& a_isolation,
			ID3D11DeviceContext* a_context,
			ID3D11Texture2D* a_source,
			ID3D11Texture2D* a_private) noexcept
		{
			CopyResult result{};
			__try {
				result.began = a_isolation.Begin();
				if (result.began) {
					__try {
						a_context->CopyResource(a_private, a_source);
						a_context->CopyResource(a_source, a_private);
						result.copied = true;
					} __finally {
						result.ended = a_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_isolation.StateUncertain()) {
				static_cast<void>(a_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			return result;
		}

		struct StereoResult
		{
			bool began = false;
			bool evaluated = false;
			bool copied = false;
			bool ended = false;
			bool stateUncertain = false;
			std::uint32_t exceptionCode = 0;
		};

		struct PrepareResult
		{
			bool began = false;
			bool prepared = false;
			bool ended = false;
			bool stateUncertain = false;
			std::uint32_t exceptionCode = 0;
		};

		struct PrivateResult
		{
			bool began = false;
			bool evaluated = false;
			bool copied = false;
			bool ended = false;
			bool stateUncertain = false;
			std::uint32_t exceptionCode = 0;
		};

		struct TeardownResult
		{
			bool began = false;
			bool reset = false;
			bool deactivated = false;
			bool ended = false;
			bool stateUncertain = false;
			std::uint32_t exceptionCode = 0;
		};

		// Releases vendor histories and intermediates on their owning render thread.
		// Keep this free of destructible C++ locals so __finally remains the host-state
		// restoration boundary if a vendor release raises SEH.
		[[nodiscard]] TeardownResult TeardownBackendIsolated(
			D3D11StateIsolation::Context& a_isolation,
			Backend::IBackend* a_backend) noexcept
		{
			TeardownResult result{};
			__try {
				result.began = a_isolation.Begin();
				if (result.began) {
					__try {
						result.reset = Evaluate::Reset();
						if (result.reset) {
							result.deactivated = Backend::DeactivatePrepared(a_backend);
						}
					} __finally {
						result.ended = a_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_isolation.StateUncertain()) {
				static_cast<void>(a_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			return result;
		}

		// Creates resources and vendor histories inside the same exact state sandbox the
		// eventual evaluate uses. No frame resource is read or written. Keep this free of
		// destructible locals so __finally remains the host-state restoration boundary.
		[[nodiscard]] PrepareResult PrepareBackendIsolated(
			D3D11StateIsolation::Context& a_isolation,
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			const D3D11_TEXTURE2D_DESC& a_sourceDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			const Backend::Config& a_config,
			std::string_view a_runtimeDir) noexcept
		{
			PrepareResult result{};
			__try {
				result.began = a_isolation.Begin();
				if (result.began) {
					__try {
						result.prepared = Evaluate::PrepareViews(
							a_backend,
							a_device,
							a_context,
							a_sourceDesc,
							a_motionDesc,
							a_config,
							a_runtimeDir);
					} __finally {
						result.ended = a_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_isolation.StateUncertain()) {
				static_cast<void>(a_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			return result;
		}

		[[nodiscard]] bool RunNgxCreate(
			const D3D11_TEXTURE2D_DESC& a_sourceDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			std::string_view a_seam,
			bool a_subrects) noexcept
		{
			const auto runtimeDir = g_ngxRuntimeDir.load(std::memory_order_acquire);
			if (!runtimeDir || runtimeDir->empty()) {
				Disable(E_INVALIDARG, 0, "diagnosticNgxCreate has no explicit runtimeDir snapshot", Gate::ngxCreate);
				return false;
			}
			Backend::IBackend* const ngx = Backend::Find("ngx"sv);
			if (!ngx) {
				Disable(E_NOINTERFACE, 0, "the backend registry has no raw NGX entry", Gate::ngxCreate);
				return false;
			}

			Backend::Config config{};
			if (!BuildNgxConfig(a_sourceDesc, config, a_subrects)) {
				Disable(E_INVALIDARG, 0, "NGX configuration did not match the startup dynamic-resolution and quality contract", Gate::ngxCreate);
				return false;
			}

			g_ngxCreateAttempts.fetch_add(1, std::memory_order_relaxed);
			const PrepareResult result = PrepareBackendIsolated(
				g_isolation,
				ngx,
				g_device,
				g_context,
				a_sourceDesc,
				a_motionDesc,
				config,
				*runtimeDir);
			if (!result.began || !result.prepared || !result.ended ||
				result.stateUncertain || result.exceptionCode != 0) {
				Disable(
					!result.began || !result.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
					result.exceptionCode,
					result.stateUncertain     ? "exact host context state became unprovable during NGX create-only preparation"sv :
					!result.began             ? "context-state entry failed for NGX create-only preparation"sv :
					!result.ended             ? "exact host-state restoration failed after NGX create-only preparation"sv :
					result.exceptionCode != 0 ? "NGX create-only preparation raised an SEH exception"sv :
												"raw NGX initialization, staging or two-view creation declined"sv,
					Gate::ngxCreate);
				return false;
			}

			const HRESULT removed = g_device->GetDeviceRemovedReason();
			g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
			if (FAILED(removed)) {
				Disable(static_cast<std::int32_t>(removed), 0, "the D3D11 device reported removal after NGX create-only preparation", Gate::ngxCreate);
				return false;
			}
			const auto caps = ngx->Probe(g_device);
			if (!caps.available || !Backend::ActivatePrepared(ngx)) {
				Disable(E_FAIL, 0, "raw NGX prepared successfully but its adapter-bound capability or registry commit failed", Gate::ngxCreate);
				return false;
			}

			g_ngxCreateSuccesses.fetch_add(1, std::memory_order_relaxed);
			g_ngxCreateCompleted.store(true, std::memory_order_release);
			const auto evaluate = Evaluate::Snapshot();
			logger::info(FMT_STRING("NGX create-only gate at {}: prepared two independent configured-quality histories at {}x{} per eye, prepare count {}, runtime {} at {}; no frame evaluated or copied"),
				a_seam,
				config.renderWidth,
				config.renderHeight,
				evaluate.prepareCount,
				Backend::Text(caps.runtimeVersion).empty() ? std::string_view{ "version unreported" } : Backend::Text(caps.runtimeVersion),
				Backend::Text(caps.runtimePath).empty() ? std::string_view{ "path unreported" } : Backend::Text(caps.runtimePath));
			return true;
		}

		// Evaluates both eyes into descriptor-keyed private output textures. No
		// double-wide destination is supplied, so neither reassembly nor engine colour
		// handback is reachable. Keep this free of destructible C++ locals.
		[[nodiscard]] PrivateResult EvaluateBackendPrivateIsolated(
			D3D11StateIsolation::Context& a_isolation,
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			ID3D11Texture2D* a_source,
			ID3D11Texture2D* a_depth,
			ID3D11Texture2D* a_motionVectors,
			const D3D11_TEXTURE2D_DESC& a_sourceDesc,
			const D3D11_TEXTURE2D_DESC& a_depthDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			const Backend::Config& a_config,
			std::string_view a_runtimeDir) noexcept
		{
			PrivateResult result{};
			__try {
				result.began = a_isolation.Begin();
				if (result.began) {
					__try {
						result.evaluated = Evaluate::PrivateFrame(
							a_backend,
							a_device,
							a_context,
							a_source,
							a_depth,
							a_motionVectors,
							a_sourceDesc,
							a_depthDesc,
							a_motionDesc,
							a_config,
							a_runtimeDir);
					} __finally {
						result.ended = a_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_isolation.StateUncertain()) {
				static_cast<void>(a_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			return result;
		}

		// Evaluates and reassembles both eyes into the caller's private descriptor-matched
		// double-wide texture. No engine copy is present in this boundary. Keep this free
		// of destructible C++ locals so __finally remains the restoration boundary.
		[[nodiscard]] PrivateResult EvaluateBackendReassembledIsolated(
			D3D11StateIsolation::Context& a_isolation,
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			ID3D11Texture2D* a_source,
			ID3D11Texture2D* a_depth,
			ID3D11Texture2D* a_motionVectors,
			ID3D11Texture2D* a_privateOutput,
			const D3D11_TEXTURE2D_DESC& a_sourceDesc,
			const D3D11_TEXTURE2D_DESC& a_depthDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			const Backend::Config& a_config,
			std::string_view a_runtimeDir) noexcept
		{
			PrivateResult result{};
			__try {
				result.began = a_isolation.Begin();
				if (result.began) {
					__try {
						Telemetry::Mark(Telemetry::MarkPoint::kAfterEvaluateIsolationBegin);
						result.evaluated = Evaluate::ReassembledFrame(
							a_backend,
							a_device,
							a_context,
							a_source,
							a_depth,
							a_motionVectors,
							a_privateOutput,
							a_sourceDesc,
							a_depthDesc,
							a_motionDesc,
							a_config,
							a_runtimeDir);
						Telemetry::Mark(Telemetry::MarkPoint::kBeforeEvaluateIsolationEnd);
					} __finally {
						result.ended = a_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_isolation.StateUncertain()) {
				static_cast<void>(a_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			return result;
		}

		struct EngineCopyResult
		{
			bool began = false;
			bool copied = false;
			bool ended = false;
			bool stateUncertain = false;
			std::uint32_t exceptionCode = 0;
		};

		// The vendor boundary ends with a private, descriptor-matched double-wide texture.
		// This separate guarded operation is the only engine-colour mutation, and it runs
		// only after evaluation, host-state restoration, device health, backend activation,
		// a fresh four-ratio contract check, and the complete slot-21 transaction preflight.
		[[nodiscard]] EngineCopyResult CopyPrivateToEngine(
			D3D11StateIsolation::Context& a_isolation,
			ID3D11DeviceContext* a_context,
			ID3D11Texture2D* a_engineSource,
			ID3D11Texture2D* a_privateOutput) noexcept
		{
			EngineCopyResult result{};
			if (!a_context || !a_engineSource || !a_privateOutput || a_engineSource == a_privateOutput) {
				return result;
			}
			__try {
				result.began = a_isolation.Begin();
				if (result.began) {
					__try {
						// The private context state may retain vendor bindings from the prior
						// evaluation. Clear only that isolated state before the copy; End restores
						// the exact host state that vanilla HDR expects.
						a_context->ClearState();
						a_context->CopyResource(a_engineSource, a_privateOutput);
						result.copied = true;
					} __finally {
						result.ended = a_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_isolation.StateUncertain()) {
				static_cast<void>(a_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			return result;
		}

		// Like CopyIsolated, this function must stay free of destructible C++ locals.
		// Evaluate's live control arm is intentionally written to the same constraint.
		[[nodiscard]] StereoResult EvaluateStereoIsolated(
			D3D11StateIsolation::Context& a_isolation,
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			ID3D11Texture2D* a_source,
			ID3D11Texture2D* a_depth,
			ID3D11Texture2D* a_motionVectors,
			ID3D11Texture2D* a_private,
			const D3D11_TEXTURE2D_DESC& a_sourceDesc,
			const D3D11_TEXTURE2D_DESC& a_depthDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			const Backend::Config& a_config) noexcept
		{
			StereoResult result{};
			__try {
				result.began = a_isolation.Begin();
				if (result.began) {
					__try {
						result.evaluated = Evaluate::Frame(
							a_backend,
							a_device,
							a_context,
							a_source,
							a_depth,
							a_motionVectors,
							a_private,
							a_sourceDesc,
							a_depthDesc,
							a_motionDesc,
							a_config);
						if (result.evaluated) {
							a_context->CopyResource(a_source, a_private);
							result.copied = true;
						}
					} __finally {
						result.ended = a_isolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_isolation.StateUncertain()) {
				static_cast<void>(a_isolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			return result;
		}

		struct CaptureNotificationResult
		{
			bool returned = false;
			std::uint32_t exceptionCode = 0;
		};

		// Capture is diagnostic and must never own authoritative transaction state.
		// Keep its D3D readback/mutex path behind an SEH boundary after commit/abort
		// bookkeeping has already reached a terminal state.
		[[nodiscard]] CaptureNotificationResult NotifyQualityCapture(
			HandbackStamp a_candidate,
			bool a_committed) noexcept
		{
			CaptureNotificationResult result{};
			__try {
				if (a_committed) {
					StereoCapture::OnQualityTransactionCommitted(a_candidate);
				} else {
					StereoCapture::OnQualityTransactionAborted(a_candidate);
				}
				result.returned = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			// The real capture notification above already ran to completion; this only
			// makes the caller believe it failed, so the three downstream conservative
			// disables are reached with the pixel transaction already committed or
			// cleanly aborted, never by actually raising SEH here.
			if (result.returned && ConsumeQualityFault(QualityFaultSeam::kCaptureNotifyFailure)) {
				result.returned = false;
			}
			return result;
		}
	}

	RuntimeConfigurationEntryToken EnterRuntimeConfiguration() noexcept
	{
		RuntimeConfigurationEntryToken token{};
		if (g_runtimeConfigurationEntryDepth != 0) {
			++g_runtimeConfigurationEntryDepth;
			token.entered = true;
			token.ownsAdmission = false;
			token.epoch = g_runtimeConfigurationEntryEpoch;
			return token;
		}

		const auto epoch = g_configurationEpoch.load(std::memory_order_seq_cst);
		if ((epoch & 1u) != 0) {
			return token;
		}
		g_activeConfigurationEntries.fetch_add(1, std::memory_order_seq_cst);
		if (g_configurationEpoch.load(std::memory_order_seq_cst) != epoch) {
			g_activeConfigurationEntries.fetch_sub(1, std::memory_order_seq_cst);
			return token;
		}

		g_runtimeConfigurationEntryDepth = 1;
		g_runtimeConfigurationEntryEpoch = epoch;
		g_runtimeConfigurationOwnsAdmission = true;
		token.entered = true;
		token.ownsAdmission = true;
		token.epoch = epoch;
		return token;
	}

	void LeaveRuntimeConfiguration(RuntimeConfigurationEntryToken& a_token) noexcept
	{
		if (!a_token.entered) {
			return;
		}
		if (g_runtimeConfigurationEntryDepth != 0 &&
			a_token.epoch == g_runtimeConfigurationEntryEpoch) {
			--g_runtimeConfigurationEntryDepth;
			if (g_runtimeConfigurationOwnsAdmission && g_runtimeConfigurationEntryDepth == 0) {
				g_runtimeConfigurationEntryEpoch = 0;
				g_runtimeConfigurationOwnsAdmission = false;
				g_activeConfigurationEntries.fetch_sub(1, std::memory_order_seq_cst);
			}
		}
		a_token.entered = false;
		a_token.ownsAdmission = false;
		a_token.epoch = 0;
	}

	void AbortConfiguration() noexcept
	{
		// This is terminal for every vendor gate. Preserve the old configuration's
		// ownership long enough to arm render-thread teardown; otherwise a failure after
		// BeginConfiguration cleared the gates could strand live NGX/FFX histories with
		// no remaining evidence that they need release.
		ScheduleOwnedVendorTeardown();
		g_enabled.store(false, std::memory_order_release);
		g_stereoNullEnabled.store(false, std::memory_order_release);
		g_ngxCreateEnabled.store(false, std::memory_order_release);
		g_ngxEvaluateEnabled.store(false, std::memory_order_release);
		g_ngxReassembleEnabled.store(false, std::memory_order_release);
		g_ngxHandbackEnabled.store(false, std::memory_order_release);
		g_ngxContinuousEnabled.store(false, std::memory_order_release);
		g_ffxEvaluateEnabled.store(false, std::memory_order_release);
		g_ffxReassembleEnabled.store(false, std::memory_order_release);
		g_ffxHandbackEnabled.store(false, std::memory_order_release);
		g_ffxContinuousEnabled.store(false, std::memory_order_release);
		g_configurationHadRawNgx.store(false, std::memory_order_release);
		g_configurationHadFfx.store(false, std::memory_order_release);
		g_sessionDisabled.store(true, std::memory_order_release);

		auto epoch = g_configurationEpoch.load(std::memory_order_seq_cst);
		while ((epoch & 1u) != 0 &&
			   !g_configurationEpoch.compare_exchange_weak(
				   epoch, epoch + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
		}
	}

	ConfigurationBegin BeginConfiguration() noexcept
	{
		// Reloading from inside an admitted render callback would otherwise wait on
		// its own central count.  Fail closed instead of self-deadlocking.
		if (g_runtimeConfigurationEntryDepth != 0) {
			return ConfigurationBegin::kFailed;
		}
		if (g_sessionDisabled.load(std::memory_order_acquire)) {
			// A suspended session is not a failed one. Aborting here would clear every
			// gate and, through the caller's interrupted-transaction path, latch
			// TAAReplacement's one-way latch - which the re-arm predicate then reads as a
			// tripped sibling and refuses, so any reload landing inside the cool-down
			// converted a recoverable fault into a terminal session with a reason naming
			// the reload rather than the fault. Refuse instead, having touched nothing:
			// the epoch is still even and every gate still stands, so the caller has
			// nothing to undo and the re-arm finds the session exactly as it left it.
			// kGatesClearedDuringCooldown stays reachable - the second check below, and
			// Configure's own, still abort - but only from a disable that lands after this
			// call already made the epoch odd.
			if (g_recoverySuspensionActive.load(std::memory_order_acquire)) {
				logger::warn("settings reload refused: the pre-HDR quality session is in its post-fault cool-down and clearing its gates now would end the re-arm; re-issue the change once /state reports the session active"sv);
				return ConfigurationBegin::kDeferred;
			}
			AbortConfiguration();
			return ConfigurationBegin::kFailed;
		}
		auto epoch = g_configurationEpoch.load(std::memory_order_seq_cst);
		for (;;) {
			while ((epoch & 1u) != 0) {
				std::this_thread::yield();
				epoch = g_configurationEpoch.load(std::memory_order_seq_cst);
			}
			if (g_configurationEpoch.compare_exchange_weak(
					epoch, epoch + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
				break;
			}
		}

		const bool hadRawNgx = g_ngxCreateEnabled.load(std::memory_order_acquire) ||
		                       g_ngxEvaluateEnabled.load(std::memory_order_acquire) ||
		                       g_ngxReassembleEnabled.load(std::memory_order_acquire) ||
		                       g_ngxHandbackEnabled.load(std::memory_order_acquire) ||
		                       g_ngxContinuousEnabled.load(std::memory_order_acquire);
		const bool hadFfx = g_ffxEvaluateEnabled.load(std::memory_order_acquire) ||
		                    g_ffxReassembleEnabled.load(std::memory_order_acquire) ||
		                    g_ffxHandbackEnabled.load(std::memory_order_acquire) ||
		                    g_ffxContinuousEnabled.load(std::memory_order_acquire);
		g_configurationHadRawNgx.store(hadRawNgx, std::memory_order_release);
		g_configurationHadFfx.store(hadFfx, std::memory_order_release);

		// The odd epoch rejects new render entries. Clear every gate as a second
		// fail-closed layer, then wait only on the configuration thread until any
		// callback admitted by the previous complete snapshot has returned.
		g_enabled.store(false, std::memory_order_release);
		g_stereoNullEnabled.store(false, std::memory_order_release);
		g_ngxCreateEnabled.store(false, std::memory_order_release);
		g_ngxEvaluateEnabled.store(false, std::memory_order_release);
		g_ngxReassembleEnabled.store(false, std::memory_order_release);
		g_ngxHandbackEnabled.store(false, std::memory_order_release);
		g_ngxContinuousEnabled.store(false, std::memory_order_release);
		g_ffxEvaluateEnabled.store(false, std::memory_order_release);
		g_ffxReassembleEnabled.store(false, std::memory_order_release);
		g_ffxHandbackEnabled.store(false, std::memory_order_release);
		g_ffxContinuousEnabled.store(false, std::memory_order_release);
		while (g_activeConfigurationEntries.load(std::memory_order_seq_cst) != 0) {
			std::this_thread::yield();
		}
		// A disable that landed while this call was waiting, so the epoch is already odd
		// and every gate above is already cleared. Unlike the pre-mutation check, there is
		// state to normalize whether the session is suspended or terminal, so this one
		// aborts either way.
		if (g_sessionDisabled.load(std::memory_order_acquire)) {
			AbortConfiguration();
			return ConfigurationBegin::kFailed;
		}
		return ConfigurationBegin::kBegan;
	}

	bool Configure(
		bool a_roundTripEnabled,
		bool a_stereoNullEnabled,
		bool a_ngxCreateEnabled,
		bool a_ngxEvaluateEnabled,
		bool a_ngxReassembleEnabled,
		bool a_ngxHandbackEnabled,
		bool a_ngxContinuousEnabled,
		bool a_ffxEvaluateEnabled,
		bool a_ffxReassembleEnabled,
		bool a_ffxHandbackEnabled,
		bool a_ffxContinuousEnabled,
		std::uint64_t a_continuousTargetSuccesses,
		std::string_view a_runtimeDir,
		std::string_view a_ngxPreset,
		std::string_view a_quality,
		std::string_view a_stereoMode,
		bool a_failSoft,
		std::string_view a_mode,
		bool a_continuousUnbounded) noexcept
	{
		// Published unconditionally, even on the fail-closed paths below: a reload that
		// only changes failSoft while the session is otherwise idle must still take effect.
		g_failSoft.store(a_failSoft, std::memory_order_release);
		// Same: /state must keep reporting which mode armed this session even on a
		// fail-closed path below, the way failSoft does immediately above. Configuration.cpp
		// computes a_continuousUnbounded as exactly a_mode != "off", so it doubles as the
		// productPath bool without a second string compare here.
		g_mode.store(CanonicalModeLiteral(a_mode), std::memory_order_release);
		g_productPath.store(a_continuousUnbounded, std::memory_order_release);
		if (g_sessionDisabled.load(std::memory_order_acquire)) {
			// No suspended/terminal distinction here, unlike BeginConfiguration: the only
			// caller reaches this with its own Begin already returned kBegan, so the epoch
			// is odd and the gates are already cleared. That is a disable which arrived
			// mid-apply, not a reload that found a cool-down, and it leaves state to
			// normalize whichever way the session is going.
			AbortConfiguration();
			return false;
		}
		if ((g_configurationEpoch.load(std::memory_order_seq_cst) & 1u) == 0) {
			if (BeginConfiguration() != ConfigurationBegin::kBegan) {
				return false;
			}
		}
		const bool hadRawNgx = g_configurationHadRawNgx.exchange(false, std::memory_order_acq_rel);
		const bool hadFfx = g_configurationHadFfx.exchange(false, std::memory_order_acq_rel);
		const bool hasFfx = a_ffxEvaluateEnabled || a_ffxReassembleEnabled ||
		                    a_ffxHandbackEnabled || a_ffxContinuousEnabled;
		g_enabled.store(a_roundTripEnabled, std::memory_order_release);
		g_stereoNullEnabled.store(a_stereoNullEnabled, std::memory_order_release);
		g_ffxEvaluateEnabled.store(a_ffxEvaluateEnabled, std::memory_order_release);
		g_ffxReassembleEnabled.store(a_ffxReassembleEnabled, std::memory_order_release);
		g_ffxHandbackEnabled.store(a_ffxHandbackEnabled, std::memory_order_release);
		g_ffxContinuousEnabled.store(a_ffxContinuousEnabled, std::memory_order_release);
		ConfigureContinuousTarget(a_continuousTargetSuccesses, a_continuousUnbounded);
		{
			// A resolution, not a refusal: a backend that cannot address sub-rects keeps
			// working on the copy path, and the log says which path this session runs.
			// Resolved against the backend the session's gates drive - the FFX and raw
			// NGX gates each name theirs by registry lookup whatever `backend` selected,
			// and only the null control arm evaluates through the selected backend, which
			// the first configuration at plugin load runs before Select has chosen.
			const bool requestedSubrects = a_stereoMode == "subrect"sv;
			const bool hasRawNgx = a_ngxCreateEnabled || a_ngxEvaluateEnabled || a_ngxReassembleEnabled ||
			                       a_ngxHandbackEnabled || a_ngxContinuousEnabled;
			Backend::IBackend* const stereoBackend = hasFfx    ? Backend::Find("ffx"sv) :
			                                         hasRawNgx ? Backend::Find("ngx"sv) :
			                                                     Backend::Current();
			const std::string_view backendName = stereoBackend ? stereoBackend->Name() : "none"sv;
			const bool advertised = stereoBackend && stereoBackend->Probe(nullptr).supportsSubrects;
			const bool effective = requestedSubrects && advertised;
			const StereoBackend which = !stereoBackend             ? StereoBackend::kNone :
			                            backendName == "ngx"sv  ? StereoBackend::kNgx :
			                            backendName == "ffx"sv  ? StereoBackend::kFfx :
			                            backendName == "null"sv ? StereoBackend::kNull :
			                                                      StereoBackend::kOther;
			g_stereoSubrectsRequested.store(requestedSubrects, std::memory_order_release);
			g_stereoSubrects.store(effective, std::memory_order_release);
			g_stereoBackend.store(static_cast<std::uint32_t>(which), std::memory_order_release);
			if (requestedSubrects && !effective && stereoBackend) {
				logger::warn(FMT_STRING("stereoMode 'subrect' requested, but backend '{}' does not advertise sub-rect bases; this session copies each eye through zero-origin intermediates instead"),
					backendName);
			} else if (requestedSubrects && !stereoBackend) {
				logger::info("stereoMode 'subrect' requested before any backend is selected; resolved to intermediates until the settings are re-applied at game-data-ready or game-loaded"sv);
			} else {
				logger::info(FMT_STRING("stereoMode: requested '{}', effective '{}' on backend '{}'"),
					a_stereoMode,
					effective ? "subrect"sv : "intermediates"sv,
					backendName);
			}
		}
		try {
			auto runtimeDir = std::make_shared<const std::string>(a_runtimeDir);
			auto preset = std::make_shared<const std::string>(a_ngxPreset);
			auto quality = std::make_shared<const std::string>(a_quality);
			g_ngxRuntimeDir.store(std::move(runtimeDir), std::memory_order_release);
			g_ngxPreset.store(std::move(preset), std::memory_order_release);
			g_quality.store(std::move(quality), std::memory_order_release);
			g_ngxCreateEnabled.store(a_ngxCreateEnabled, std::memory_order_release);
			g_ngxEvaluateEnabled.store(a_ngxEvaluateEnabled, std::memory_order_release);
			g_ngxReassembleEnabled.store(a_ngxReassembleEnabled, std::memory_order_release);
			g_ngxHandbackEnabled.store(a_ngxHandbackEnabled, std::memory_order_release);
			g_ngxContinuousEnabled.store(a_ngxContinuousEnabled, std::memory_order_release);
		} catch (...) {
			g_ngxCreateEnabled.store(false, std::memory_order_release);
			g_ngxEvaluateEnabled.store(false, std::memory_order_release);
			g_ngxReassembleEnabled.store(false, std::memory_order_release);
			g_ngxHandbackEnabled.store(false, std::memory_order_release);
			g_ngxContinuousEnabled.store(false, std::memory_order_release);
			g_ngxRuntimeDir.store({}, std::memory_order_release);
			g_ngxPreset.store({}, std::memory_order_release);
			g_quality.store({}, std::memory_order_release);
			logger::error("raw-NGX development diagnostics disabled: runtimeDir, preset or quality snapshot allocation failed"sv);
		}
		const bool effectiveHasRawNgx =
			g_ngxCreateEnabled.load(std::memory_order_acquire) ||
			g_ngxEvaluateEnabled.load(std::memory_order_acquire) ||
			g_ngxReassembleEnabled.load(std::memory_order_acquire) ||
			g_ngxHandbackEnabled.load(std::memory_order_acquire) ||
			g_ngxContinuousEnabled.load(std::memory_order_acquire);
		const auto owner = Evaluate::Snapshot();
		const bool ownsNgx = owner.ownsBackendState &&
		                     owner.backend == reinterpret_cast<std::uintptr_t>(Backend::Find("ngx"sv));
		const bool ownsFfx = owner.ownsBackendState &&
		                     owner.backend == reinterpret_cast<std::uintptr_t>(Backend::Find("ffx"sv));
		if (hadRawNgx && !effectiveHasRawNgx && ownsNgx) {
			g_ngxTeardownPending.store(true, std::memory_order_release);
		}
		if (hadFfx && !hasFfx && ownsFfx) {
			g_ffxTeardownPending.store(true, std::memory_order_release);
		}
		g_configurationEpoch.fetch_add(1, std::memory_order_seq_cst);
		return true;
	}

	bool DefaultRenderScaleForQuality(std::string_view a_quality, double& a_out) noexcept
	{
		Backend::QualityMode quality = Backend::QualityMode::kNativeAA;
		if (!ParseQuality(a_quality, quality)) {
			return false;
		}
		a_out = static_cast<double>(Backend::DefaultRenderScale(quality));
		return true;
	}

	bool VendorScalingRatioForQuality(std::string_view a_quality, double& a_out) noexcept
	{
		Backend::QualityMode quality = Backend::QualityMode::kNativeAA;
		if (!ParseQuality(a_quality, quality)) {
			return false;
		}
		a_out = static_cast<double>(Backend::VendorScalingRatio(quality));
		return true;
	}

	void OnPrimaryRenderThreadImpl() noexcept
	{
		if (!g_ngxCreateEnabled.load(std::memory_order_acquire) ||
			g_ngxCreateCompleted.load(std::memory_order_acquire) ||
			g_sessionDisabled.load(std::memory_order_acquire)) {
			return;
		}

		// Startup reaches this hook before all renderer globals and named textures are
		// populated. That is normal: leave the create gate armed and retry on the next
		// primary frame instead of converting initialization order into a session fault.
		ID3D11Device* const device = Engine::Device();
		ID3D11DeviceContext* const context = Engine::ImmediateContext();
		if (!device || !context) {
			g_ngxCreateSkips.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const auto source = RenderTargets::Lookup(1);
		const auto motionVectors = RenderTargets::Lookup(Addr::kRT_MotionVectors);
		if (!source.valid || !source.tex || !motionVectors.valid || !motionVectors.tex) {
			g_ngxCreateSkips.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		D3D11_TEXTURE2D_DESC sourceDesc{};
		D3D11_TEXTURE2D_DESC motionDesc{};
		source.tex->GetDesc(&sourceDesc);
		motionVectors.tex->GetDesc(&motionDesc);
		const bool validSource =
			sourceDesc.Width >= 2 && (sourceDesc.Width & 1u) == 0 && sourceDesc.Height != 0 &&
			sourceDesc.Width == source.width && sourceDesc.Height == source.height &&
			static_cast<std::uint32_t>(sourceDesc.Format) == source.format &&
			sourceDesc.MipLevels == 1 && sourceDesc.ArraySize == 1 &&
			sourceDesc.Format != DXGI_FORMAT_UNKNOWN && sourceDesc.SampleDesc.Count == 1 &&
			sourceDesc.Usage == D3D11_USAGE_DEFAULT && sourceDesc.CPUAccessFlags == 0 && sourceDesc.MiscFlags == 0;
		const bool validMotion =
			motionDesc.Width == sourceDesc.Width && motionDesc.Height == sourceDesc.Height &&
			motionDesc.MipLevels == 1 && motionDesc.ArraySize == 1 &&
			motionDesc.Format == DXGI_FORMAT_R16G16_FLOAT && motionDesc.SampleDesc.Count == 1 &&
			motionDesc.Usage == D3D11_USAGE_DEFAULT && motionDesc.CPUAccessFlags == 0 && motionDesc.MiscFlags == 0;
		if (!validSource || !validMotion) {
			g_ngxCreateSkips.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		ProbeIsolation();
		if (!g_isolationReady.load(std::memory_order_acquire) || !g_device || !g_context) {
			Disable(g_lastError.load(std::memory_order_relaxed), 0, "the required D3D11.1 context-state capability is absent", Gate::ngxCreate);
			return;
		}
		if (device != g_device || context != g_context || Engine::Device() != g_device || Engine::ImmediateContext() != g_context) {
			Disable(E_UNEXPECTED, 0, "the engine device or immediate context changed during NGX create-only prewarm", Gate::ngxCreate);
			return;
		}

		g_sourceTexture.store(reinterpret_cast<std::uintptr_t>(source.tex), std::memory_order_relaxed);
		g_width.store(sourceDesc.Width, std::memory_order_relaxed);
		g_height.store(sourceDesc.Height, std::memory_order_relaxed);
		g_format.store(static_cast<std::uint32_t>(sourceDesc.Format), std::memory_order_relaxed);
		g_sampleCount.store(sourceDesc.SampleDesc.Count, std::memory_order_relaxed);
		// This entry's one consumer of the stereo flag; the HDR dispatch loads its own
		// copy once per frame for the same reason.
		static_cast<void>(RunNgxCreate(sourceDesc, motionDesc, "primary ClearColor prewarm"sv, g_stereoSubrects.load(std::memory_order_acquire)));
	}

	// POD-only SEH shell: configuration reload must never wait forever because a
	// driver/engine fault skipped a C++ destructor while this entry was admitted.
	void OnPrimaryRenderThread() noexcept
	{
		// Query polling is independent of the HDR configuration admission and must keep
		// draining after an episode completes or settings close the render gates.
		Telemetry::Collect();
		auto configurationEntry = EnterConfigurationEntry();
		if (!configurationEntry.entered) {
			return;
		}
		__try {
			OnPrimaryRenderThreadImpl();
		} __finally {
			LeaveConfigurationEntry(configurationEntry);
		}
	}

	void OnDispatchImpl(
		std::uintptr_t a_manager,
		std::int32_t a_sourceLogical,
		std::int32_t a_destinationLogical,
		std::uint64_t a_runtimeConfigEpoch) noexcept
	{
		if (ScopePolicy::ClassifyCurrentDispatch() != ScopePolicy::DispatchScope::kMain) {
			return;
		}
		// Once per verified kMain frame, ahead of every early return below, so a fault
		// raised anywhere in the previous frame - including later in this same function -
		// is classified on a predictable cadence rather than however each early return
		// happens to fall.
		TickSessionRecovery();
		// Effect slot 21 is later in the same manager pass. Never let an unconsumed
		// request leak across frames or across a suppressed scope dispatch.
		g_suppressDynamicUpsample = false;
		if (g_pendingQualityHandback != PendingQualityHandback::kNone) {
			if (!AbortQualityHandbackCandidate()) {
				g_holdCurrentHDRDispatch = true;
				return;
			}
		}
		ProbeIsolation();
		const bool runRoundTrip = g_enabled.load(std::memory_order_acquire) &&
		                          !g_completed.load(std::memory_order_acquire);
		const bool runStereoNull = g_stereoNullEnabled.load(std::memory_order_acquire) &&
		                           !g_stereoNullCompleted.load(std::memory_order_acquire);
		const bool runNgxCreate = g_ngxCreateEnabled.load(std::memory_order_acquire) &&
		                          !g_ngxCreateCompleted.load(std::memory_order_acquire);
		const bool runNgxEvaluate = g_ngxEvaluateEnabled.load(std::memory_order_acquire) &&
		                            !g_ngxEvaluateCompleted.load(std::memory_order_acquire);
		const bool runNgxReassemble = g_ngxReassembleEnabled.load(std::memory_order_acquire) &&
		                              !g_ngxReassembleCompleted.load(std::memory_order_acquire);
		const bool runNgxHandback = g_ngxHandbackEnabled.load(std::memory_order_acquire) &&
		                            !g_ngxHandbackCompleted.load(std::memory_order_acquire);
		const bool runNgxContinuous = g_ngxContinuousEnabled.load(std::memory_order_acquire) &&
		                              !g_ngxContinuousCompleted.load(std::memory_order_acquire);
		const bool runFfxEvaluate = g_ffxEvaluateEnabled.load(std::memory_order_acquire) &&
		                            !g_ffxEvaluateCompleted.load(std::memory_order_acquire);
		const bool runFfxReassemble = g_ffxReassembleEnabled.load(std::memory_order_acquire) &&
		                              !g_ffxReassembleCompleted.load(std::memory_order_acquire);
		const bool runFfxHandback = g_ffxHandbackEnabled.load(std::memory_order_acquire) &&
		                            !g_ffxHandbackCompleted.load(std::memory_order_acquire);
		const bool runFfxContinuous = g_ffxContinuousEnabled.load(std::memory_order_acquire) &&
		                              !g_ffxContinuousCompleted.load(std::memory_order_acquire);
		const bool runNgxTeardown = g_ngxTeardownPending.load(std::memory_order_acquire);
		const bool runFfxTeardown = g_ffxTeardownPending.load(std::memory_order_acquire);
		if (!runRoundTrip && !runStereoNull && !runNgxCreate && !runNgxEvaluate && !runNgxReassemble && !runNgxHandback && !runNgxContinuous && !runFfxEvaluate && !runFfxReassemble && !runFfxHandback && !runFfxContinuous && !runNgxTeardown && !runFfxTeardown) {
			return;
		}
		if (runNgxTeardown || runFfxTeardown) {
			if (a_sourceLogical != 1 || a_destinationLogical != 3) {
				return;
			}
			if (runFfxTeardown) {
				g_ffxTeardownAttempts.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_ngxTeardownAttempts.fetch_add(1, std::memory_order_relaxed);
			}
			const std::string_view backendName = runFfxTeardown ? "ffx"sv : "ngx"sv;
			Backend::IBackend* const backend = Backend::Find(backendName);
			const TeardownResult result = TeardownBackendIsolated(g_isolation, backend);
			g_lastExceptionCode.store(result.exceptionCode, std::memory_order_relaxed);
			const HRESULT removed = result.stateUncertain ? E_UNEXPECTED :
			                                                (g_device ? g_device->GetDeviceRemovedReason() : E_POINTER);
			g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
			if (result.reset && result.ended && !result.stateUncertain && result.exceptionCode == 0) {
				g_private.Reset();
				g_privateDesc = {};
				g_privateReady.store(false, std::memory_order_release);
				g_privateTexture.store(0, std::memory_order_relaxed);
			}
			const bool releaseDeferred = result.began && !result.reset && result.ended && !result.stateUncertain &&
			                             result.exceptionCode == 0 && !FAILED(removed);
			const auto deferredAttempt = releaseDeferred ? (runFfxTeardown ?
																   g_ffxTeardownDeferrals.fetch_add(1, std::memory_order_relaxed) + 1 :
																   g_ngxTeardownDeferrals.fetch_add(1, std::memory_order_relaxed) + 1) :
			                                               0;
			if (releaseDeferred && deferredAttempt < kTeardownMaxAttempts) {
				if (runFfxTeardown) {
					g_ffxTeardownFailures.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_ngxTeardownFailures.fetch_add(1, std::memory_order_relaxed);
				}
				logger::warn(FMT_STRING("{} render-thread teardown attempt {}/{} could not yet prove safe backend release; live histories and backend identity were retained, selection remains unchanged and teardown will retry on the next eligible HDR frame"),
					backendName,
					deferredAttempt,
					kTeardownMaxAttempts);
				return;
			}
			if (runFfxTeardown) {
				g_ffxTeardownDeferrals.store(0, std::memory_order_relaxed);
			} else {
				g_ngxTeardownDeferrals.store(0, std::memory_order_relaxed);
			}
			if (!result.began || !result.reset || !result.deactivated || !result.ended ||
				result.stateUncertain || result.exceptionCode != 0 || FAILED(removed)) {
				if (runFfxTeardown) {
					g_ffxTeardownFailures.fetch_add(1, std::memory_order_relaxed);
					g_ffxTeardownPending.store(false, std::memory_order_release);
				} else {
					g_ngxTeardownFailures.fetch_add(1, std::memory_order_relaxed);
					g_ngxTeardownPending.store(false, std::memory_order_release);
				}
				g_sessionDisabled.store(true, std::memory_order_release);
				logger::error(FMT_STRING("{} render-thread teardown failed: begin {}, reset {}, deactivate {}, end {}, state uncertain {}, HRESULT {:08X}, exception {:08X}; vanilla HDR continues"),
					backendName,
					result.began,
					result.reset,
					result.deactivated,
					result.ended,
					result.stateUncertain,
					static_cast<std::uint32_t>(FAILED(removed) ? removed : g_isolation.LastError()),
					result.exceptionCode);
				return;
			}

			if (runFfxTeardown) {
				g_ffxTeardownSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ffxTeardownPending.store(false, std::memory_order_release);
				g_ffxTeardownCompleted.store(true, std::memory_order_release);
			} else {
				g_ngxTeardownSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ngxTeardownPending.store(false, std::memory_order_release);
				g_ngxTeardownCompleted.store(true, std::memory_order_release);
			}
			logger::info(FMT_STRING("{} render-thread teardown completed: both histories, per-eye intermediates and backend session released; backend selection returned to null and vanilla HDR continues"), backendName);
			return;
		}
		if (g_sessionDisabled.load(std::memory_order_acquire)) {
			return;
		}
		const int gateCount = (runRoundTrip ? 1 : 0) + (runStereoNull ? 1 : 0) +
		                      (runNgxCreate ? 1 : 0) + (runNgxEvaluate ? 1 : 0) +
		                      (runNgxReassemble ? 1 : 0) + (runNgxHandback ? 1 : 0) +
		                      (runNgxContinuous ? 1 : 0) + (runFfxEvaluate ? 1 : 0) +
		                      (runFfxReassemble ? 1 : 0) + (runFfxHandback ? 1 : 0) +
		                      (runFfxContinuous ? 1 : 0);
		const Gate gate = runNgxCreate     ? Gate::ngxCreate :
		                  runNgxEvaluate   ? Gate::ngxEvaluate :
		                  runNgxReassemble ? Gate::ngxReassemble :
		                  runNgxHandback   ? Gate::ngxHandback :
		                  runNgxContinuous ? Gate::ngxContinuous :
		                  runFfxEvaluate   ? Gate::ffxEvaluate :
		                  runFfxReassemble ? Gate::ffxReassemble :
		                  runFfxHandback   ? Gate::ffxHandback :
		                  runFfxContinuous ? Gate::ffxContinuous :
		                  runStereoNull    ? Gate::stereoNull :
		                                     Gate::roundTrip;
		if (gateCount != 1) {
			Disable(E_INVALIDARG, 0, "the pre-HDR development gates cannot run together", gate);
			return;
		}

		if (!g_isolationReady.load(std::memory_order_acquire) || !g_device || !g_context) {
			Disable(g_lastError.load(std::memory_order_relaxed), 0, "the required D3D11.1 context-state capability is absent", gate);
			return;
		}
		if (a_sourceLogical != 1 || a_destinationLogical != 3) {
			RecordSkip(gate);
			return;
		}
		if (Engine::Device() != g_device || Engine::ImmediateContext() != g_context) {
			Disable(E_UNEXPECTED, 0, "the engine device or immediate context changed after isolation initialization", gate);
			return;
		}

		// One load per dispatch. Configure publishes this flag from the F4SE messaging
		// thread; a flip landing between two loads would create the private target
		// without the unordered-access bind the config then asks for, and that decline
		// latches the session off. Every consumer below takes this value.
		const bool stereoSubrects = g_stereoSubrects.load(std::memory_order_acquire);
		const auto source = RenderTargets::Lookup(a_sourceLogical);
		if (!EnsurePrivate(source, gate, stereoSubrects)) {
			return;
		}

		if (runNgxCreate) {
			const auto motionVectors = RenderTargets::Lookup(Addr::kRT_MotionVectors);
			if (!motionVectors.valid || !motionVectors.tex) {
				Disable(E_INVALIDARG, 0, "logical motion target 35 did not resolve for NGX create-only preparation", Gate::ngxCreate);
				return;
			}
			D3D11_TEXTURE2D_DESC motionDesc{};
			motionVectors.tex->GetDesc(&motionDesc);
			static_cast<void>(RunNgxCreate(g_privateDesc, motionDesc, "first HDR fallback"sv, stereoSubrects));
			return;
		}

		if (runNgxEvaluate || runNgxReassemble || runNgxHandback || runNgxContinuous || runFfxEvaluate || runFfxReassemble || runFfxHandback || runFfxContinuous) {
			const Gate vendorGate = runNgxHandback   ? Gate::ngxHandback :
			                        runNgxContinuous ? Gate::ngxContinuous :
			                        runNgxReassemble ? Gate::ngxReassemble :
			                        runNgxEvaluate   ? Gate::ngxEvaluate :
			                        runFfxReassemble ? Gate::ffxReassemble :
			                        runFfxHandback   ? Gate::ffxHandback :
			                        runFfxContinuous ? Gate::ffxContinuous :
			                                           Gate::ffxEvaluate;
			const bool isFfx = runFfxEvaluate || runFfxReassemble || runFfxHandback || runFfxContinuous;
			const auto runtimeSnapshot = g_ngxRuntimeDir.load(std::memory_order_acquire);
			if (!isFfx && (!runtimeSnapshot || runtimeSnapshot->empty())) {
				Disable(E_INVALIDARG, 0, "the selected NGX frame gate has no explicit runtimeDir snapshot", vendorGate);
				return;
			}
			const std::string_view runtimeDir = isFfx ? std::string_view{} : std::string_view{ *runtimeSnapshot };
			const std::string_view backendName = isFfx ? "ffx"sv : "ngx"sv;
			Backend::IBackend* const backend = Backend::Find(backendName);
			if (!backend) {
				Disable(E_NOINTERFACE, 0, "the backend registry has no requested live-gate entry", vendorGate);
				return;
			}

			const auto depth = RenderTargets::LookupDepth(Addr::kDS_MainScene);
			const auto motionVectors = RenderTargets::Lookup(Addr::kRT_MotionVectors);
			if (!depth.valid || !depth.tex || !depth.srv) {
				Disable(E_INVALIDARG, 0, "logical depth-stencil 1 did not resolve to a live texture and depth-plane SRV for vendor evaluation", vendorGate);
				return;
			}
			if (!motionVectors.valid || !motionVectors.tex) {
				Disable(E_INVALIDARG, 0, "logical motion target 35 did not resolve for vendor evaluation", vendorGate);
				return;
			}

			D3D11_TEXTURE2D_DESC depthDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
			depth.tex->GetDesc(&depthDesc);
			depth.srv->GetDesc(&depthSrvDesc);
			motionVectors.tex->GetDesc(&motionDesc);
			if (depth.width != g_privateDesc.Width || depth.height != g_privateDesc.Height || depth.arraySize != 1 ||
				depthDesc.Width != g_privateDesc.Width || depthDesc.Height != g_privateDesc.Height ||
				depthDesc.MipLevels != 1 || depthDesc.ArraySize != 1 ||
				depthDesc.Format != DXGI_FORMAT_R24G8_TYPELESS || depthDesc.SampleDesc.Count != 1 ||
				depthDesc.Usage != D3D11_USAGE_DEFAULT || depthDesc.CPUAccessFlags != 0 || depthDesc.MiscFlags != 0 ||
				(depthDesc.BindFlags & (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) !=
					(D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE) ||
				depthSrvDesc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
				depthSrvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
				Disable(E_INVALIDARG, 0, "logical depth-stencil 1 did not match the verified double-wide R24G8/R24 depth-plane contract for vendor evaluation", vendorGate);
				return;
			}

			ResetPolicy::WorldAdmissionTicket worldAdmission{};
			const bool gatedNgxQualityAttempt = !isFfx &&
			                                    (runNgxHandback || runNgxContinuous) &&
			                                    DynamicResolution::WorldAdmissionGated();
			if (gatedNgxQualityAttempt) {
				// Consult the epoch-bound pre-world ticket before reading the reduced
				// contract. Blocked/loading frames are intentionally native and must not
				// be misreported as ratio or hidden-area invariant failures.
				worldAdmission = Hooks::CurrentQualityWorldAdmission();
				if (!worldAdmission.admitted) {
					RecordSkip(vendorGate);
					return;
				}
			}

			Backend::Config config{};
			if (!isFfx) {
				// The pre-world updater normally repairs load drift before rasterization.
				// Revalidate positively here as well: any fault or selector mismatch keeps
				// the vendor result private and lets vanilla finish this frame.
				if (!DynamicResolution::FrameContractReadyForHandback()) {
					RecordSkip(vendorGate);
					return;
				}
				// The private-evaluate gate leaves its result in the per-eye outputs,
				// which exist only on the copy path, so its config clears the flag here:
				// PrivateFrame runs inside the SEH arm and cannot copy the config itself.
				if (!BuildNgxConfig(g_privateDesc, config, stereoSubrects && !runNgxEvaluate)) {
					Disable(E_INVALIDARG, 0, "NGX frame configuration did not match the startup dynamic-resolution and quality contract", vendorGate);
					return;
				}
			} else {
				const auto dynamicResolution = DynamicResolution::Snapshot();
				if ((runFfxHandback || runFfxContinuous) && dynamicResolution.applied &&
					dynamicResolution.requestedScale < 1.0f) {
					Disable(E_NOTIMPL, 0, "FSR engine handback is refused while the engine rasterizes a reduced active viewport; its current NativeAA path has no reduced-input transaction", vendorGate);
					return;
				}
				config.renderWidth = g_privateDesc.Width / 2u;
				config.renderHeight = g_privateDesc.Height;
				config.outputWidth = g_privateDesc.Width / 2u;
				config.outputHeight = g_privateDesc.Height;
				config.quality = Backend::QualityMode::kNativeAA;
				config.hdr = true;
				config.invertedDepth = false;
				config.jitteredMV = false;
				config.lowResMV = true;
				config.autoExposure = true;
				config.preExposure = 1.0f;
				config.exposureScale = 1.0f;
			}

			const bool reducedNgxQualityAttempt = !isFfx &&
			                                      (runNgxHandback || runNgxContinuous) &&
			                                      (config.renderWidth < config.outputWidth ||
														config.renderHeight < config.outputHeight);
			if (reducedNgxQualityAttempt) {
				if (!worldAdmission.admitted) {
					RecordSkip(vendorGate);
					return;
				}
			}

			const bool ngxContinuousWarmup = runNgxContinuous &&
			                                 g_ngxContinuousWarmupSuccesses.load(std::memory_order_acquire) < kNgxContinuousWarmupSuccesses;
			const bool ffxContinuousWarmup = runFfxContinuous &&
			                                 g_ffxContinuousWarmupSuccesses.load(std::memory_order_acquire) < kFfxContinuousWarmupSuccesses;
			const bool continuousWarmup = ngxContinuousWarmup || ffxContinuousWarmup;
			const bool requiresCopy = runNgxHandback || runFfxHandback ||
			                          ((runNgxContinuous || runFfxContinuous) && !continuousWarmup);
			const bool reducedNgxHandback = !isFfx && requiresCopy &&
			                                (config.renderWidth < config.outputWidth ||
												config.renderHeight < config.outputHeight);
			const auto continuousState = (runNgxContinuous || runFfxContinuous) ?
			                                 LatchContinuousTargetState() :
			                                 g_continuousTargetState.load(std::memory_order_acquire);
			const auto continuousTargetSuccesses = TargetFromState(continuousState);
			const bool continuousUnbounded = ContinuousTargetUnbounded(continuousState);
			if (reducedNgxHandback) {
				std::uint64_t qualityTaaConfigEpoch = 0;
				if (!Hooks::QualityHandbackReady() ||
					!DynamicResolution::FrameContractReadyForHandback() ||
					(qualityTaaConfigEpoch = TAAReplacement::PrepareQualityBypass()) == 0 ||
					!PrepareQualityPixelTransaction(
						a_sourceLogical,
						source.tex,
						qualityTaaConfigEpoch,
						a_runtimeConfigEpoch,
						worldAdmission)) {
					static_cast<void>(AbortQualityPixelsForDispatch());
					RecordSkip(vendorGate);
					return;
				}
				MarkQualityEvaluationStarted();
				Telemetry::Begin();
			}

			PrivateResult result{};
			if (runFfxHandback) {
				g_ffxHandbackAttempts.fetch_add(1, std::memory_order_relaxed);
			} else if (runNgxHandback) {
				g_ngxHandbackAttempts.fetch_add(1, std::memory_order_relaxed);
			} else if (runNgxContinuous) {
				g_ngxContinuousAttempts.fetch_add(1, std::memory_order_relaxed);
			} else if (runFfxContinuous) {
				g_ffxContinuousAttempts.fetch_add(1, std::memory_order_relaxed);
			} else if (runNgxReassemble || runFfxReassemble) {
				if (runFfxReassemble) {
					g_ffxReassembleAttempts.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_ngxReassembleAttempts.fetch_add(1, std::memory_order_relaxed);
				}
			}
			if (runNgxReassemble || runNgxHandback || runNgxContinuous ||
				runFfxReassemble || runFfxHandback || runFfxContinuous) {
				result = EvaluateBackendReassembledIsolated(
					g_isolation,
					backend,
					g_device,
					g_context,
					source.tex,
					depth.tex,
					motionVectors.tex,
					g_private.Get(),
					g_privateDesc,
					depthDesc,
					motionDesc,
					config,
					runtimeDir);
			} else {
				if (isFfx) {
					g_ffxEvaluateAttempts.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_ngxEvaluateAttempts.fetch_add(1, std::memory_order_relaxed);
				}
				result = EvaluateBackendPrivateIsolated(
					g_isolation,
					backend,
					g_device,
					g_context,
					source.tex,
					depth.tex,
					motionVectors.tex,
					g_privateDesc,
					depthDesc,
					motionDesc,
					config,
					runtimeDir);
			}
			if (!result.began || !result.evaluated || !result.ended ||
				result.stateUncertain || result.exceptionCode != 0) {
				if (reducedNgxHandback) {
					static_cast<void>(AbortQualityPixelsForDispatch());
				}
				Disable(
					!result.began || !result.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
					result.exceptionCode,
					result.stateUncertain     ? "exact host context state became unprovable during private vendor evaluation"sv :
					!result.began             ? "context-state entry failed for private vendor evaluation"sv :
					!result.ended             ? "exact host-state restoration failed after private vendor evaluation"sv :
					result.exceptionCode != 0 ? "private vendor evaluation raised an SEH exception"sv :
					!result.evaluated         ? "the colour-depth-motion private vendor orchestrator declined"sv :
												"private vendor evaluation failed without a classified step"sv,
					vendorGate);
				return;
			}
			if (reducedNgxHandback) {
				Telemetry::Mark(Telemetry::MarkPoint::kAfterEvaluateIsolationEnd);
			}

			const HRESULT removed = g_device->GetDeviceRemovedReason();
			g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
			if (FAILED(removed)) {
				if (reducedNgxHandback) {
					static_cast<void>(AbortQualityPixelsForDispatch());
				}
				Disable(static_cast<std::int32_t>(removed), 0, "the D3D11 device reported removal after private vendor evaluation", vendorGate);
				return;
			}
			if (reducedNgxQualityAttempt &&
				!ResetPolicy::RevalidateQualityWorldAdmission(worldAdmission)) {
				if (reducedNgxHandback) {
					static_cast<void>(AbortQualityPixelsForDispatch());
				} else {
					// Warm-up evaluates privately and has no Quality pixel transaction to
					// abort, but the vendor histories still advanced on a frame that no
					// longer belongs to the admitted world epoch.
					ResetPolicy::RequestQualityAbortReset();
				}
				RecordSkip(vendorGate);
				return;
			}
			const auto caps = backend->Probe(g_device);
			if (!caps.available || !Backend::ActivatePrepared(backend)) {
				if (reducedNgxHandback) {
					static_cast<void>(AbortQualityPixelsForDispatch());
				}
				Disable(E_FAIL, 0, "the vendor backend evaluated successfully but its adapter-bound capability or registry commit failed", vendorGate);
				return;
			}
			const bool reassembled = runNgxReassemble || runNgxHandback || runNgxContinuous ||
			                         runFfxReassemble || runFfxHandback || runFfxContinuous;
			if (reassembled) {
				g_privateReassemblySerial.fetch_add(1, std::memory_order_release);
			}
			if (reducedNgxHandback) {
				MarkQualityEvaluationSucceeded();
			}
			bool qualityTransactionBegan = false;
			if (requiresCopy) {
				const auto currentSource = RenderTargets::Lookup(a_sourceLogical);
				if (!currentSource.valid || !currentSource.tex || currentSource.tex != source.tex ||
					currentSource.width != g_privateDesc.Width || currentSource.height != g_privateDesc.Height ||
					currentSource.format != static_cast<std::uint32_t>(g_privateDesc.Format)) {
					if (reducedNgxHandback) {
						static_cast<void>(AbortQualityPixelsForDispatch());
					}
					Disable(E_UNEXPECTED, 0, "logical pre-tonemap colour changed identity during private vendor evaluation", vendorGate);
					return;
				}
				auto copySource = currentSource;
				if (reducedNgxHandback) {
					if (!Hooks::QualityHandbackReady()) {
						static_cast<void>(AbortQualityPixelsForDispatch());
						Disable(E_NOINTERFACE, 0, "the complete HDR/TAA/slot-21/pre-world quality transaction infrastructure is unavailable", vendorGate);
						return;
					}
					if (!DynamicResolution::FrameContractReadyForHandback()) {
						static_cast<void>(AbortQualityPixelsForDispatch());
						RecordSkip(vendorGate);
						return;
					}
					qualityTransactionBegan = Hooks::BeginQualityHandback(a_manager);
					if (!qualityTransactionBegan) {
						static_cast<void>(AbortQualityPixelsForDispatch());
						if (DynamicResolution::FrameContractDrifted()) {
							RecordSkip(vendorGate);
						} else if (!Hooks::QualityHandbackReady()) {
							Disable(E_FAIL, 0, "slot 21 failed its byte-scoped dynamic-geometry transaction preflight before engine colour mutation", vendorGate);
						} else {
							RecordSkip(vendorGate);
						}
						return;
					}

					const auto finalSource = RenderTargets::Lookup(a_sourceLogical);
					D3D11_TEXTURE2D_DESC finalDesc{};
					if (!finalSource.valid || !finalSource.tex || finalSource.tex != source.tex ||
						finalSource.width != g_privateDesc.Width || finalSource.height != g_privateDesc.Height ||
						finalSource.format != static_cast<std::uint32_t>(g_privateDesc.Format) ||
						!Hooks::QualityHandbackCommitReady(a_manager)) {
						const bool geometryRestored = Hooks::AbortQualityHandback();
						const bool pixelsRestored = AbortQualityPixelsForDispatch();
						if (!geometryRestored || !pixelsRestored) {
							g_holdCurrentHDRDispatch = true;
						}
						RecordSkip(vendorGate);
						return;
					}
					finalSource.tex->GetDesc(&finalDesc);
					if (finalDesc.Width != g_privateDesc.Width || finalDesc.Height != g_privateDesc.Height ||
						finalDesc.Format != g_privateDesc.Format || finalDesc.MipLevels != g_privateDesc.MipLevels ||
						finalDesc.ArraySize != g_privateDesc.ArraySize ||
						finalDesc.SampleDesc.Count != g_privateDesc.SampleDesc.Count) {
						const bool geometryRestored = Hooks::AbortQualityHandback();
						const bool pixelsRestored = AbortQualityPixelsForDispatch();
						if (!geometryRestored || !pixelsRestored) {
							g_holdCurrentHDRDispatch = true;
						}
						RecordSkip(vendorGate);
						return;
					}
					copySource = finalSource;
				}

				if (reducedNgxHandback) {
					if (!ResetPolicy::RevalidateQualityWorldAdmission(worldAdmission)) {
						const bool geometryRestored = Hooks::AbortQualityHandback();
						const bool pixelsRestored = AbortQualityPixelsForDispatch();
						if (!geometryRestored || !pixelsRestored) {
							g_holdCurrentHDRDispatch = true;
						}
						RecordSkip(vendorGate);
						return;
					}
					const QualityCopyResult copy = CopyQualityPixelsToEngine(g_private.Get());
					const bool copyClean = !copy.stateUncertain && copy.exceptionCode == 0;
					const bool preRt1InjectedFault = copyClean &&
					                                 ((copy.injectedFault == QualityFaultSeam::kPreBackup &&
															!copy.began && !copy.engineWriteIssued && !copy.ended) ||
														 (copy.injectedFault == QualityFaultSeam::kPostBackup &&
															 copy.began && !copy.engineWriteIssued && copy.ended));
					if (preRt1InjectedFault) {
						// RT1 is untouched, so this is the ordinary abort: close the unity
						// bracket and let vanilla finish the reduced frame.
						bool geometryRestored = true;
						if (qualityTransactionBegan) {
							geometryRestored = Hooks::AbortQualityHandback();
						}
						const bool pixelsRestored = AbortQualityPixelsForDispatch();
						if (!geometryRestored || !pixelsRestored) {
							g_holdCurrentHDRDispatch = true;
						}
						RecordSkip(vendorGate);
						return;
					}
					const bool rt1Written = copyClean && copy.began && copy.engineWriteIssued && copy.ended;
					if (rt1Written && copy.injectedFault == QualityFaultSeam::kPostRt1) {
						// The reconstruction is in RT1 and the host state is proved. Closing
						// the bracket here would hand vanilla HDR a full-resolution RT1 at
						// reduced ratios, so the frame finishes on the engine's own path
						// instead, with the bracket up.
						if (ArmQualityNativeFallback(QualityFallbackSeam::kInjectedPostRt1)) {
							ArmNativeFallbackDispatch();
							return;
						}
						bool geometryRestored = true;
						if (qualityTransactionBegan) {
							geometryRestored = Hooks::AbortQualityHandback();
						}
						const bool pixelsRestored = AbortQualityPixelsForDispatch();
						if (!geometryRestored || !pixelsRestored) {
							g_holdCurrentHDRDispatch = true;
						}
						RecordSkip(vendorGate);
						return;
					}
					if (!rt1Written) {
						// engineWriteIssued was raised before CopyResource, so the write may
						// already have landed. With the host state proved restored the frame
						// still finishes natively; the session is disabled either way, because
						// the copy scope did not close cleanly. Disabled is not necessarily
						// terminal: this seam closes the transaction, so failSoft can suspend
						// and re-arm it - see TickSessionRecovery.
						if (copy.engineWriteIssued && !copy.stateUncertain &&
							ArmQualityNativeFallback(QualityFallbackSeam::kCopyClose)) {
							ArmNativeFallbackDispatch();
							Disable(
								!copy.began || !copy.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
								copy.exceptionCode,
								"the private-to-engine copy scope did not close cleanly after the RT1 write; this frame finishes on native TemporalAA",
								vendorGate);
							return;
						}
						bool geometryRestored = true;
						if (qualityTransactionBegan) {
							geometryRestored = Hooks::AbortQualityHandback();
						}
						const bool pixelsRestored = AbortQualityPixelsForDispatch();
						if (!geometryRestored || !pixelsRestored) {
							g_holdCurrentHDRDispatch = true;
						}
						Disable(
							!copy.began || !copy.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
							copy.exceptionCode,
							"the private-to-engine copy did not close safely",
							vendorGate);
						return;
					}
					Telemetry::Mark(Telemetry::MarkPoint::kAfterBackupIsolationEnd);
				} else {
					const EngineCopyResult copy = CopyPrivateToEngine(g_isolation, g_context, copySource.tex, g_private.Get());
					if (!copy.began || !copy.copied || !copy.ended ||
						copy.stateUncertain || copy.exceptionCode != 0) {
						Disable(
							!copy.began || !copy.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
							copy.exceptionCode,
							"the isolated private-to-engine colour copy or exact host-state restoration failed",
							vendorGate);
						return;
					}
				}
				const HRESULT copyRemoved = g_device->GetDeviceRemovedReason();
				g_deviceRemovedReason.store(static_cast<std::int32_t>(copyRemoved), std::memory_order_relaxed);
				if (FAILED(copyRemoved)) {
					if (reducedNgxHandback &&
						ArmQualityNativeFallback(QualityFallbackSeam::kDeviceRemovedAfterCopy)) {
						// Terminal by design: the frame still finishes on the native path with
						// the bracket up, the session ends, and the paired reset stays owed.
						ArmNativeFallbackDispatch();
						g_qualityPixelUnrecoverable.fetch_add(1, std::memory_order_relaxed);
						RequestQualityResetOnce();
						Disable(static_cast<std::int32_t>(copyRemoved), 0, "the D3D11 device reported removal after engine colour handback; this frame finishes on native TemporalAA", vendorGate);
						return;
					}
					bool geometryRestored = true;
					if (qualityTransactionBegan) {
						geometryRestored = Hooks::AbortQualityHandback();
					}
					if (reducedNgxHandback) {
						const bool pixelsRestored = AbortQualityPixelsForDispatch();
						if (!geometryRestored || !pixelsRestored) {
							g_holdCurrentHDRDispatch = true;
						}
					}
					Disable(static_cast<std::int32_t>(copyRemoved), 0, "the D3D11 device reported removal after engine colour handback", vendorGate);
					return;
				}
				result.copied = true;
				if (runNgxHandback) {
					g_ngxHandbackCopies.fetch_add(1, std::memory_order_relaxed);
				} else if (runNgxContinuous) {
					g_ngxContinuousCopies.fetch_add(1, std::memory_order_relaxed);
				} else if (runFfxHandback) {
					g_ffxHandbackCopies.fetch_add(1, std::memory_order_relaxed);
				} else if (runFfxContinuous) {
					g_ffxContinuousCopies.fetch_add(1, std::memory_order_relaxed);
				}
				if (reducedNgxHandback) {
					StageNgxQualityHandback(
						runNgxContinuous ? PendingQualityHandback::kNgxContinuous :
										   PendingQualityHandback::kNgxOneShot,
						continuousTargetSuccesses,
						continuousUnbounded);
					StereoCapture::OnQualityHandback(
						copySource.tex,
						g_private.Get(),
						g_privateReassemblySerial.load(std::memory_order_acquire),
						HandbackCandidateStamp());
					g_suppressDynamicUpsample = true;
					g_dynamicUpsampleSuppressionsArmed.fetch_add(1, std::memory_order_relaxed);
				}
			}

			const auto evaluate = Evaluate::Snapshot();
			if (runNgxContinuous) {
				if (continuousWarmup) {
					const auto warmup = g_ngxContinuousWarmupSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
					if (warmup == 1) {
						logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: private temporal warm-up began at {}x{} -> {}x{} per eye in {} mode; engine color remains untouched for {} accepted frames"),
							config.renderWidth,
							config.renderHeight,
							config.outputWidth,
							config.outputHeight,
							QualityName(config.quality),
							kNgxContinuousWarmupSuccesses);
					}
					if (warmup >= kNgxContinuousWarmupSuccesses) {
						logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: completed {} private warm-up frames with both histories preserved; the next accepted frame begins {} engine-handback window"),
							warmup,
							ContinuousTargetPhrase(continuousUnbounded, continuousTargetSuccesses));
					}
				} else if (!reducedNgxHandback) {
					const auto successes = g_ngxContinuousSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
					const auto candidate = g_ngxHandbackCandidateSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
					PublishHandbackCandidate(HandbackSource::kNgx, candidate);
					if (successes == 1) {
						const std::string target = continuousUnbounded ? std::string("unbounded") : fmt::format(FMT_STRING("{}"), continuousTargetSuccesses);
						logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: engine handback active after {} private warm-up frames; target {} consecutive copied frames"),
							g_ngxContinuousWarmupSuccesses.load(std::memory_order_relaxed),
							target);
					}
					// The product path (continuousUnbounded) never self-retires: successes
					// keeps counting for /state, but the gate that would tear the session down
					// at the target never fires. See ConfigureContinuousTarget - the low bits
					// of the packed target are 0 here, on purpose, so this guard is load-bearing
					// and not merely defensive against a stale 0.
					if (!continuousUnbounded && successes >= continuousTargetSuccesses) {
						g_ngxContinuousCompleted.store(true, std::memory_order_release);
						g_ngxTeardownPending.store(true, std::memory_order_release);
						logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: completed {} private warm-up frames and {} consecutive engine handbacks with {} direct copies; the gate is now inert and safe-release teardown begins on the next eligible HDR frame"),
							g_ngxContinuousWarmupSuccesses.load(std::memory_order_relaxed),
							successes,
							g_ngxContinuousCopies.load(std::memory_order_relaxed));
					}
				}
			} else if (runFfxContinuous) {
				if (continuousWarmup) {
					const auto warmup = g_ffxContinuousWarmupSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
					if (warmup == 1) {
						logger::info(FMT_STRING("pre-HDR FSR bounded-continuous gate: private temporal warm-up began at {}x{} per eye; engine color remains untouched for {} accepted frames"),
							config.renderWidth,
							config.renderHeight,
							kFfxContinuousWarmupSuccesses);
					}
					if (warmup >= kFfxContinuousWarmupSuccesses) {
						logger::info(FMT_STRING("pre-HDR FSR bounded-continuous gate: completed {} private warm-up frames with both histories preserved; the next accepted frame begins {} engine-handback window"),
							warmup,
							ContinuousTargetPhrase(continuousUnbounded, continuousTargetSuccesses));
					}
				} else {
					const auto successes = g_ffxContinuousSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
					const auto candidate = g_ffxHandbackCandidateSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
					PublishHandbackCandidate(HandbackSource::kFfx, candidate);
					if (successes == 1) {
						const std::string target = continuousUnbounded ? std::string("unbounded") : fmt::format(FMT_STRING("{}"), continuousTargetSuccesses);
						logger::info(FMT_STRING("pre-HDR FSR bounded-continuous gate: engine handback active after {} private warm-up frames; target {} consecutive copied frames"),
							g_ffxContinuousWarmupSuccesses.load(std::memory_order_relaxed),
							target);
					}
					// See the NGX branch above: the product path never self-retires, and the
					// packed target's low bits are 0 while unbounded, so this guard is
					// load-bearing.
					if (!continuousUnbounded && successes >= continuousTargetSuccesses) {
						g_ffxContinuousCompleted.store(true, std::memory_order_release);
						g_ffxTeardownPending.store(true, std::memory_order_release);
						logger::info(FMT_STRING("pre-HDR FSR bounded-continuous gate: completed {} private warm-up frames and {} consecutive engine handbacks with {} direct copies; the gate is now inert and safe-release teardown begins on the next eligible HDR frame"),
							g_ffxContinuousWarmupSuccesses.load(std::memory_order_relaxed),
							successes,
							g_ffxContinuousCopies.load(std::memory_order_relaxed));
					}
				}
			} else if (runFfxHandback) {
				g_ffxHandbackSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ffxHandbackCompleted.store(true, std::memory_order_release);
				g_ffxTeardownPending.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR FSR engine-handback gate: evaluated and reassembled one two-eye native-AA frame at {}x{} per eye, evaluates {}/{}, reassemblies {}/{}, engine copies {}, prepare count {}, runtime {}; logical color 1 now carries the one-shot result, vanilla HDR follows and safe-release teardown begins on the next eligible HDR frame"),
					config.renderWidth,
					config.renderHeight,
					evaluate.leftEvaluates,
					evaluate.rightEvaluates,
					evaluate.leftReassemblies,
					evaluate.rightReassemblies,
					g_ffxHandbackCopies.load(std::memory_order_relaxed),
					evaluate.prepareCount,
					Backend::Text(caps.runtimeVersion).empty() ? std::string_view{ "version unreported" } : Backend::Text(caps.runtimeVersion));
			} else if (runNgxHandback && !reducedNgxHandback) {
				g_ngxHandbackSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ngxHandbackCompleted.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR NGX engine-handback gate: evaluated and reassembled one configured-quality two-eye frame at {}x{} per eye, evaluates {}/{}, reassemblies {}/{}, engine copies {}, prepare count {}, runtime {} at {}; logical color 1 now carries the one-shot result and vanilla HDR follows"),
					config.renderWidth,
					config.renderHeight,
					evaluate.leftEvaluates,
					evaluate.rightEvaluates,
					evaluate.leftReassemblies,
					evaluate.rightReassemblies,
					g_ngxHandbackCopies.load(std::memory_order_relaxed),
					evaluate.prepareCount,
					Backend::Text(caps.runtimeVersion).empty() ? std::string_view{ "version unreported" } : Backend::Text(caps.runtimeVersion),
					Backend::Text(caps.runtimePath).empty() ? std::string_view{ "path unreported" } : Backend::Text(caps.runtimePath));
			} else if (runNgxReassemble) {
				g_ngxReassembleSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ngxReassembleCompleted.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR NGX private-reassembly gate: evaluated and reassembled one configured-quality two-eye frame at {}x{} per eye, evaluates {}/{}, reassemblies {}/{}, prepare count {}, runtime {} at {}; private double-wide output was not copied to engine colour, vanilla HDR follows untouched"),
					config.renderWidth,
					config.renderHeight,
					evaluate.leftEvaluates,
					evaluate.rightEvaluates,
					evaluate.leftReassemblies,
					evaluate.rightReassemblies,
					evaluate.prepareCount,
					Backend::Text(caps.runtimeVersion).empty() ? std::string_view{ "version unreported" } : Backend::Text(caps.runtimeVersion),
					Backend::Text(caps.runtimePath).empty() ? std::string_view{ "path unreported" } : Backend::Text(caps.runtimePath));
			} else if (runFfxReassemble) {
				g_ffxReassembleSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ffxReassembleCompleted.store(true, std::memory_order_release);
				g_ffxTeardownPending.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR FSR private-reassembly gate: evaluated and reassembled one two-eye native-AA frame at {}x{} per eye, evaluates {}/{}, reassemblies {}/{}, prepare count {}, runtime {}; private double-wide output was not copied to engine colour, vanilla HDR follows untouched and safe-release teardown begins on the next eligible HDR frame"),
					config.renderWidth,
					config.renderHeight,
					evaluate.leftEvaluates,
					evaluate.rightEvaluates,
					evaluate.leftReassemblies,
					evaluate.rightReassemblies,
					evaluate.prepareCount,
					Backend::Text(caps.runtimeVersion).empty() ? std::string_view{ "version unreported" } : Backend::Text(caps.runtimeVersion));
			} else if (runFfxEvaluate) {
				g_ffxEvaluateSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ffxEvaluateCompleted.store(true, std::memory_order_release);
				g_ffxTeardownPending.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR FSR private-evaluate gate: evaluated one two-eye native-AA frame at {}x{} per eye, left/right {}/{}, prepare count {}, runtime {}; outputs remain private with no reassembly or engine colour copy, vanilla HDR follows untouched and safe-release teardown begins on the next eligible HDR frame"),
					config.renderWidth,
					config.renderHeight,
					evaluate.leftEvaluates,
					evaluate.rightEvaluates,
					evaluate.prepareCount,
					Backend::Text(caps.runtimeVersion).empty() ? std::string_view{ "version unreported" } : Backend::Text(caps.runtimeVersion));
			} else {
				g_ngxEvaluateSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ngxEvaluateCompleted.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR NGX private-evaluate gate: evaluated one configured-quality two-eye frame at {}x{} per eye, left/right {}/{}, prepare count {}, runtime {} at {}; outputs remain private with no reassembly or engine colour copy, vanilla HDR follows untouched"),
					config.renderWidth,
					config.renderHeight,
					evaluate.leftEvaluates,
					evaluate.rightEvaluates,
					evaluate.prepareCount,
					Backend::Text(caps.runtimeVersion).empty() ? std::string_view{ "version unreported" } : Backend::Text(caps.runtimeVersion),
					Backend::Text(caps.runtimePath).empty() ? std::string_view{ "path unreported" } : Backend::Text(caps.runtimePath));
			}
			return;
		}

		if (runStereoNull) {
			const auto depth = RenderTargets::LookupDepth(Addr::kDS_MainScene);
			const auto motionVectors = RenderTargets::Lookup(Addr::kRT_MotionVectors);
			if (!depth.valid || !depth.tex || !depth.srv) {
				Disable(E_INVALIDARG, 0, "logical depth-stencil 1 did not resolve to a live texture and depth-plane SRV", Gate::stereoNull);
				return;
			}
			if (!motionVectors.valid || !motionVectors.tex) {
				Disable(E_INVALIDARG, 0, "logical motion target 35 did not resolve to a live texture", Gate::stereoNull);
				return;
			}
			D3D11_TEXTURE2D_DESC depthDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
			depth.tex->GetDesc(&depthDesc);
			depth.srv->GetDesc(&depthSrvDesc);
			motionVectors.tex->GetDesc(&motionDesc);
			if (depth.width != g_privateDesc.Width || depth.height != g_privateDesc.Height || depth.arraySize != 1 ||
				depthDesc.Width != g_privateDesc.Width || depthDesc.Height != g_privateDesc.Height ||
				depthDesc.MipLevels != 1 || depthDesc.ArraySize != 1 ||
				depthDesc.Format != DXGI_FORMAT_R24G8_TYPELESS || depthDesc.SampleDesc.Count != 1 ||
				depthDesc.Usage != D3D11_USAGE_DEFAULT || depthDesc.CPUAccessFlags != 0 || depthDesc.MiscFlags != 0 ||
				(depthDesc.BindFlags & (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)) !=
					(D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE) ||
				depthSrvDesc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
				depthSrvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
				Disable(E_INVALIDARG, 0, "logical depth-stencil 1 did not match the verified double-wide R24G8/R24 depth-plane contract", Gate::stereoNull);
				return;
			}

			Backend::Config config{};
			config.renderWidth = g_privateDesc.Width / 2u;
			config.renderHeight = g_privateDesc.Height;
			config.outputWidth = g_privateDesc.Width / 2u;
			config.outputHeight = g_privateDesc.Height;
			config.quality = Backend::QualityMode::kNativeAA;
			config.hdr = true;
			config.invertedDepth = false;
			config.jitteredMV = false;
			config.lowResMV = true;
			config.autoExposure = true;
			config.preExposure = 1.0f;
			config.exposureScale = 1.0f;
			config.enableSubrects = stereoSubrects;

			g_stereoNullAttempts.fetch_add(1, std::memory_order_relaxed);
			const StereoResult result = EvaluateStereoIsolated(
				g_isolation,
				Backend::Current(),
				g_device,
				g_context,
				source.tex,
				depth.tex,
				motionVectors.tex,
				g_private.Get(),
				g_privateDesc,
				depthDesc,
				motionDesc,
				config);
			if (!result.began || !result.evaluated || !result.copied || !result.ended ||
				result.stateUncertain || result.exceptionCode != 0) {
				Disable(
					!result.began || !result.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
					result.exceptionCode,
					result.stateUncertain     ? "exact host context state became unprovable during stereo null evaluation"sv :
					!result.began             ? "context-state entry failed for stereo null evaluation"sv :
					!result.ended             ? "exact host-state restoration failed after stereo null evaluation"sv :
					result.exceptionCode != 0 ? "stereo null evaluation raised an SEH exception"sv :
												"the colour-depth-motion stereo null orchestrator declined"sv,
					Gate::stereoNull);
				return;
			}

			const HRESULT removed = g_device->GetDeviceRemovedReason();
			g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
			if (FAILED(removed)) {
				Disable(static_cast<std::int32_t>(removed), 0, "the D3D11 device reported removal after stereo null evaluation", Gate::stereoNull);
				return;
			}

			const auto successes = g_stereoNullSuccesses.fetch_add(1, std::memory_order_relaxed) + 1;
			if (successes == 1) {
				logger::info("pre-HDR stereo null diagnostic: first isolated colour and motion inputs plus standard-Z DS1 per-eye depth rectangles -> two NullBackend views -> double-wide colour -> logical 1 handback completed; vanilla HDR follows"sv);
			}
			if (successes >= kTargetSuccesses) {
				g_stereoNullCompleted.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR stereo null diagnostic: completed {} successful two-eye control-arm frames with no device removal; the diagnostic is now inert for this session"), successes);
			}
			return;
		}

		g_roundTripAttempts.fetch_add(1, std::memory_order_relaxed);
		const CopyResult result = CopyIsolated(g_isolation, g_context, source.tex, g_private.Get());
		if (!result.began || !result.copied || !result.ended ||
			result.stateUncertain || result.exceptionCode != 0) {
			Disable(
				static_cast<std::int32_t>(g_isolation.LastError()),
				result.exceptionCode,
				result.stateUncertain ? "exact host context state became unprovable during copy round trip"sv :
				!result.began         ? "context-state entry failed"sv :
				!result.ended         ? "exact host-state restoration failed"sv :
										"CopyResource raised an SEH exception"sv,
				Gate::roundTrip);
			return;
		}

		const HRESULT removed = g_device->GetDeviceRemovedReason();
		g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
		if (FAILED(removed)) {
			Disable(static_cast<std::int32_t>(removed), 0, "the D3D11 device reported removal after the copy round trip", Gate::roundTrip);
			return;
		}

		const auto successes = g_successes.fetch_add(1, std::memory_order_relaxed) + 1;
		if (successes == 1) {
			logger::info("pre-HDR diagnostic: first byte-preserving logical 1 -> private -> logical 1 round trip completed under exact state isolation; vanilla HDR follows unchanged"sv);
		}
		if (successes >= kTargetSuccesses) {
			g_completed.store(true, std::memory_order_release);
			logger::info(FMT_STRING("pre-HDR diagnostic: completed {} successful isolated round trips with no device removal; the diagnostic is now inert for this session"), successes);
		}
	}

	// POD-only SEH shell for the complete pre-HDR entry. Any escaping engine/driver
	// fault releases the admission count before the outer hook handles the frame.
	void OnDispatch(
		std::uintptr_t a_manager,
		std::int32_t a_sourceLogical,
		std::int32_t a_destinationLogical) noexcept
	{
		g_holdCurrentHDRDispatch = false;
		auto configurationEntry = EnterConfigurationEntry();
		if (!configurationEntry.entered) {
			return;
		}
		__try {
			OnDispatchImpl(
				a_manager,
				a_sourceLogical,
				a_destinationLogical,
				configurationEntry.epoch);
		} __finally {
			LeaveConfigurationEntry(configurationEntry);
		}
	}

	bool ConsumeQualityHDRHold() noexcept
	{
		const bool hold = g_holdCurrentHDRDispatch;
		g_holdCurrentHDRDispatch = false;
		return hold;
	}

	bool ConsumeDynamicUpsampleSuppression() noexcept
	{
		const bool suppress = g_suppressDynamicUpsample;
		g_suppressDynamicUpsample = false;
		return suppress;
	}

	HandbackStamp HandbackCandidateStamp() noexcept
	{
		const auto packed = g_handbackCandidateStamp.load(std::memory_order_acquire);
		return {
			static_cast<HandbackSource>(packed & kHandbackSourceMask),
			packed >> kHandbackSourceBits
		};
	}

	std::uint64_t HandbackCandidateSerial(HandbackSource a_source) noexcept
	{
		switch (a_source) {
		case HandbackSource::kNgx:
			return g_ngxHandbackCandidateSerial.load(std::memory_order_acquire);
		case HandbackSource::kFfx:
			return g_ffxHandbackCandidateSerial.load(std::memory_order_acquire);
		case HandbackSource::kNone:
		default:
			return 0;
		}
	}

	std::uint64_t QualityTaaConfigEpoch() noexcept
	{
		return g_qualityPixels.phase != QualityPixelPhase::kIdle ?
		           g_qualityPixels.taaConfigEpoch :
		           0;
	}

	bool QualityRuntimeConfigReady() noexcept
	{
		if (g_qualityPixels.phase == QualityPixelPhase::kIdle) {
			return false;
		}
		const auto epoch = g_configurationEpoch.load(std::memory_order_seq_cst);
		return (epoch & 1u) == 0 && epoch == g_qualityPixels.runtimeConfigEpoch;
	}

	ResetPolicy::WorldAdmissionTicket QualityWorldAdmission() noexcept
	{
		return g_qualityPixels.phase == QualityPixelPhase::kEngineColourDirty ?
		           g_qualityPixels.worldAdmission :
		           ResetPolicy::WorldAdmissionTicket{};
	}

	bool MarkQualityPublishedAtSeam() noexcept
	{
		if (g_qualityPixels.phase != QualityPixelPhase::kEngineColourDirty) {
			return false;
		}
		SetQualityPixelPhase(QualityPixelPhase::kPublishedAtSeam);
		g_qualityPixelSeamPublishes.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool QualityPublishedAtSeam() noexcept
	{
		return g_qualityPixels.phase == QualityPixelPhase::kPublishedAtSeam;
	}

	bool QualityTransactionUnrecoverable() noexcept
	{
		return g_qualityPixels.phase == QualityPixelPhase::kUnrecoverable;
	}

	namespace
	{
		struct QualityCommitOperation
		{
			HandbackStamp candidate{};
			bool candidateKnown = false;
			bool publicPixelsIssued = false;
			bool resetRequested = false;
			bool commitCounted = false;
			bool abortCounted = false;
		};

		void LatchIssuedCommitFailure(
			QualityCommitOperation* a_operation,
			std::uint32_t a_exceptionCode) noexcept
		{
			g_lastExceptionCode.store(a_exceptionCode, std::memory_order_relaxed);
			g_lastError.store(static_cast<std::int32_t>(a_exceptionCode), std::memory_order_relaxed);
			LatchQualityPixelUnrecoverable();
			a_operation->resetRequested = true;
			g_pendingQualityHandback = PendingQualityHandback::kNone;
			g_pendingQualityTarget = 0;
			g_pendingQualityUnbounded = false;
			g_ngxQualityCandidatePending.store(false, std::memory_order_release);
			if (!a_operation->commitCounted && !a_operation->abortCounted) {
				a_operation->abortCounted = true;
				g_ngxQualityAborts.fetch_add(1, std::memory_order_relaxed);
			}
			const auto candidate = a_operation->candidateKnown ?
			                           a_operation->candidate :
			                           HandbackCandidateStamp();
			ClearHandbackCandidate(candidate);
		}
	}

	bool CommitQualityHandbackCandidateImpl(
		std::uint64_t a_runtimeConfigEpoch,
		QualityCommitOperation* a_operation) noexcept
	{
		if (g_qualityPixels.phase != QualityPixelPhase::kPublishedAtSeam ||
			a_runtimeConfigEpoch == 0 || a_runtimeConfigEpoch != g_qualityPixels.runtimeConfigEpoch) {
			return false;
		}
		const auto pending = g_pendingQualityHandback;
		if (pending == PendingQualityHandback::kNone) {
			return false;
		}
		const auto candidate = HandbackCandidateStamp();
		if (candidate.source != HandbackSource::kNgx || candidate.serial == 0) {
			return false;
		}
		a_operation->candidate = candidate;
		a_operation->candidateKnown = true;
		// The seam draw already wrote history and RT4; this is the accounting. Publish
		// that fact before any COM release, counter, capture or diagnostic can raise
		// another exception.
		a_operation->publicPixelsIssued = true;
		Telemetry::Mark(Telemetry::MarkPoint::kAfterLateRangeCommit);
		if (!TAAReplacement::CommitSeamPublish() && g_seamCommitGate.Ready()) {
			logger::warn("pre-HDR NGX quality transaction committed at the late range, but the TemporalAA replacement had no published episode left to count"sv);
		}
		SetQualityPixelPhase(QualityPixelPhase::kCommitted);
		ClearQualityPixelTransaction();
		const auto target = g_pendingQualityTarget;
		const auto unbounded = g_pendingQualityUnbounded;
		g_pendingQualityHandback = PendingQualityHandback::kNone;
		g_pendingQualityTarget = 0;
		g_pendingQualityUnbounded = false;
		g_ngxQualityCandidatePending.store(false, std::memory_order_release);
		a_operation->commitCounted = true;
		g_ngxQualityCommits.fetch_add(1, std::memory_order_relaxed);
		ClearHandbackCandidate(candidate);

		if (pending == PendingQualityHandback::kNgxOneShot) {
			const auto successes = g_ngxHandbackSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
			g_ngxHandbackCompleted.store(true, std::memory_order_release);
			logger::info(FMT_STRING("pre-HDR NGX engine-handback gate: committed candidate {} only after the complete HDR/TAA/late-range transaction closed"), successes);
		} else {
			const auto successes = g_ngxContinuousSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
			if (successes == 1) {
				const std::string targetText = unbounded ? std::string("unbounded") : fmt::format(FMT_STRING("{}"), target);
				logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: first complete quality transaction committed after {} private warm-up frames; target {} frames"),
					g_ngxContinuousWarmupSuccesses.load(std::memory_order_relaxed),
					targetText);
			}
			// unbounded, not "target != 0": see g_pendingQualityUnbounded's declaration for
			// why this reads the explicit flag staged alongside target rather than treating a
			// target of 0 as the signal - the product path stores 0 there on purpose, but a
			// reader should never have to know that convention to get this right.
			if (!unbounded && successes >= target) {
				g_ngxContinuousCompleted.store(true, std::memory_order_release);
				g_ngxTeardownPending.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: committed {} complete quality transactions from {} candidates and {} direct copies; safe-release teardown begins on the next eligible HDR frame"),
					successes,
					g_ngxQualityCandidates.load(std::memory_order_relaxed),
					g_ngxContinuousCopies.load(std::memory_order_relaxed));
			}
		}
		const auto capture = NotifyQualityCapture(candidate, true);
		if (!capture.returned) {
			g_lastExceptionCode.store(capture.exceptionCode, std::memory_order_relaxed);
			g_qualityCaptureDisableAtTransactions.store(
				g_ngxQualityCommits.load(std::memory_order_relaxed) +
					g_ngxQualityAborts.load(std::memory_order_relaxed),
				std::memory_order_relaxed);
			ResetPolicy::RequestQualityAbortReset();
			g_qualityPixelUnrecoverable.fetch_add(1, std::memory_order_relaxed);
			g_sessionDisabled.store(true, std::memory_order_release);
			logger::error(FMT_STRING("pre-HDR NGX quality pixels committed, but optional capture raised SEH 0x{:08X}; authoritative ownership is clear and quality is disabled conservatively"), capture.exceptionCode);
		}
		Telemetry::End();
		return true;
	}

	QualityCommitResult CommitQualityHandbackCandidate() noexcept
	{
		// Publication is a runtime-configuration entry just like pre-HDR evaluation.
		// The POD token remains admitted through the real MRT draw and all terminal
		// bookkeeping, and __finally releases it even if an SEH escapes. Therefore
		// BeginConfiguration either closes first (this declines) or waits for the
		// already-admitted publication to reach a classified terminal state.
		ConfigurationEntryToken configurationEntry{};
		QualityCommitOperation operation{};
		bool terminal = false;
		__try {
			configurationEntry = EnterConfigurationEntry();
			if (configurationEntry.entered) {
				__try {
					terminal = CommitQualityHandbackCandidateImpl(
						configurationEntry.epoch,
						&operation);
				} __finally {
					LeaveConfigurationEntry(configurationEntry);
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Telemetry::Abort();
			const auto exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			if (operation.publicPixelsIssued) {
				// Public history/RT4 already contain the new frame. Keep every remaining
				// COM reference alive, clear only logical ownership, and report a terminal
				// handled frame so no caller treats it as a private abort.
				LatchIssuedCommitFailure(&operation, exceptionCode);
				terminal = true;
			} else {
				g_lastExceptionCode.store(exceptionCode, std::memory_order_relaxed);
				g_lastError.store(static_cast<std::int32_t>(exceptionCode), std::memory_order_relaxed);
				terminal = false;
			}
		}
		return terminal ? QualityCommitResult::kPublished :
		                  QualityCommitResult::kFailedBeforePublish;
	}

	namespace
	{
		struct QualityAbortOperation
		{
			HandbackStamp candidate{};
			bool candidateKnown = false;
			bool candidatePending = false;
			bool abortCounted = false;
		};

		[[nodiscard]] bool AbortQualityHandbackCandidateImpl(
			QualityAbortOperation* a_operation) noexcept
		{
			g_suppressDynamicUpsample = false;
			const bool pixelsRestored = AbortQualityPixels();
			if (g_pendingQualityHandback == PendingQualityHandback::kNone) {
				if (!pixelsRestored) {
					logger::error("pre-HDR NGX quality abort could not close the pixel transaction; quality remains disabled"sv);
				}
				return pixelsRestored;
			}
			a_operation->candidatePending = true;
			a_operation->candidate = HandbackCandidateStamp();
			a_operation->candidateKnown = true;
			TAAReplacement::DiscardHandbackCandidate(a_operation->candidate);
			g_pendingQualityHandback = PendingQualityHandback::kNone;
			g_pendingQualityTarget = 0;
			g_pendingQualityUnbounded = false;
			g_ngxQualityCandidatePending.store(false, std::memory_order_release);
			a_operation->abortCounted = true;
			g_ngxQualityAborts.fetch_add(1, std::memory_order_relaxed);
			ClearHandbackCandidate(a_operation->candidate);
			const auto capture = NotifyQualityCapture(a_operation->candidate, false);
			if (!capture.returned) {
				g_lastExceptionCode.store(capture.exceptionCode, std::memory_order_relaxed);
				g_qualityCaptureDisableAtTransactions.store(
					g_ngxQualityCommits.load(std::memory_order_relaxed) +
						g_ngxQualityAborts.load(std::memory_order_relaxed),
					std::memory_order_relaxed);
				g_sessionDisabled.store(true, std::memory_order_release);
				logger::error(FMT_STRING("pre-HDR NGX quality abort ownership cleared, but optional capture notification raised SEH 0x{:08X}; quality is disabled conservatively"), capture.exceptionCode);
			}
			if (!pixelsRestored) {
				logger::error("pre-HDR NGX quality candidate was cleared after an unrecoverable pixel-transaction close"sv);
			}
			return pixelsRestored;
		}

		void LatchAbortOperationFailure(
			QualityAbortOperation* a_operation,
			std::uint32_t a_exceptionCode) noexcept
		{
			g_lastExceptionCode.store(a_exceptionCode, std::memory_order_relaxed);
			g_lastError.store(static_cast<std::int32_t>(a_exceptionCode), std::memory_order_relaxed);
			LatchQualityPixelUnrecoverable();
			g_suppressDynamicUpsample = false;
			g_pendingQualityHandback = PendingQualityHandback::kNone;
			g_pendingQualityTarget = 0;
			g_pendingQualityUnbounded = false;
			g_ngxQualityCandidatePending.store(false, std::memory_order_release);
			if (a_operation->candidatePending && !a_operation->abortCounted) {
				g_ngxQualityAborts.fetch_add(1, std::memory_order_relaxed);
			}
			const auto candidate = a_operation->candidateKnown ?
			                           a_operation->candidate :
			                           HandbackCandidateStamp();
			ClearHandbackCandidate(candidate);
		}

		[[nodiscard]] bool ArmQualityNativeFallbackImpl(
			QualityFallbackSeam a_seam,
			QualityAbortOperation* a_operation) noexcept
		{
			const auto seamIndex = static_cast<std::size_t>(a_seam);
			if (g_qualityPixels.phase != QualityPixelPhase::kEngineColourDirty ||
				a_seam == QualityFallbackSeam::kNone || seamIndex >= kQualityFallbackSeamCount ||
				D3D11StateIsolation::HostStateUncertain()) {
				return false;
			}
			// The episode never reaches publish; its queries drain outside the aggregates.
			Telemetry::Abort();
			static_cast<void>(TAAReplacement::ReleaseStagedQualityForNativeFallback());
			if (g_pendingQualityHandback != PendingQualityHandback::kNone) {
				// The TAA seams arm after the candidate was staged. It is discarded exactly
				// like an aborted one, so the serial contract stays consecutive and a
				// pending pipeline capture retries on the next transaction.
				a_operation->candidatePending = true;
				a_operation->candidate = HandbackCandidateStamp();
				a_operation->candidateKnown = true;
				TAAReplacement::DiscardHandbackCandidate(a_operation->candidate);
				g_pendingQualityHandback = PendingQualityHandback::kNone;
				g_pendingQualityTarget = 0;
				g_pendingQualityUnbounded = false;
				g_ngxQualityCandidatePending.store(false, std::memory_order_release);
				a_operation->abortCounted = true;
				g_ngxQualityAborts.fetch_add(1, std::memory_order_relaxed);
				ClearHandbackCandidate(a_operation->candidate);
				const auto capture = NotifyQualityCapture(a_operation->candidate, false);
				if (!capture.returned) {
					g_lastExceptionCode.store(capture.exceptionCode, std::memory_order_relaxed);
					g_qualityCaptureDisableAtTransactions.store(
						g_ngxQualityCommits.load(std::memory_order_relaxed) +
							g_ngxQualityAborts.load(std::memory_order_relaxed),
						std::memory_order_relaxed);
					g_sessionDisabled.store(true, std::memory_order_release);
					logger::error(FMT_STRING("pre-HDR NGX quality candidate released to native TemporalAA, but optional capture notification raised SEH 0x{:08X}; quality is disabled conservatively"), capture.exceptionCode);
				}
			}
			g_qualityPixels.fallbackSeam = a_seam;
			SetQualityPixelPhase(QualityPixelPhase::kNativeFallback);
			const auto fallbacks = g_qualityPixelNativeFallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
			g_qualityPixelFallbackSeamCounts[seamIndex].fetch_add(1, std::memory_order_relaxed);
			g_qualityPixelLastFallbackSeam.store(static_cast<std::uint32_t>(a_seam), std::memory_order_release);
			if (g_fallbackLogGate.Ready()) {
				logger::info(FMT_STRING("pre-HDR NGX quality transaction finished on native TemporalAA at seam {} (fallback {} this session); the reconstruction in RT1 goes through the engine's own HDR, gamma and TemporalAA at unity, which writes history and RT4 itself"),
					QualityFallbackSeamName(a_seam),
					fallbacks);
			}
			return true;
		}

		[[nodiscard]] bool CloseQualityNativeFallbackImpl() noexcept
		{
			if (g_qualityPixels.phase != QualityPixelPhase::kNativeFallback) {
				return false;
			}
			g_suppressDynamicUpsample = false;
			g_qualityPixelFallbackLateCloses.fetch_add(1, std::memory_order_relaxed);
			ClearQualityPixelTransaction();
			return true;
		}

		void LatchQualityPublishUnrecoverableImpl(QualityAbortOperation* a_operation) noexcept
		{
			Telemetry::Abort();
			// The seam draw was issued into public history and RT4, and its outcome or
			// the host state after it cannot be proved. The paired reset stays owed even
			// though quality is now disabled for the session.
			LatchQualityPixelUnrecoverable();
			g_suppressDynamicUpsample = false;
			if (g_pendingQualityHandback != PendingQualityHandback::kNone) {
				a_operation->candidatePending = true;
				a_operation->candidate = HandbackCandidateStamp();
				a_operation->candidateKnown = true;
				TAAReplacement::DiscardHandbackCandidate(a_operation->candidate);
				g_pendingQualityHandback = PendingQualityHandback::kNone;
				g_pendingQualityTarget = 0;
				g_pendingQualityUnbounded = false;
				g_ngxQualityCandidatePending.store(false, std::memory_order_release);
				a_operation->abortCounted = true;
				g_ngxQualityAborts.fetch_add(1, std::memory_order_relaxed);
				ClearHandbackCandidate(a_operation->candidate);
				const auto capture = NotifyQualityCapture(a_operation->candidate, false);
				if (!capture.returned) {
					g_lastExceptionCode.store(capture.exceptionCode, std::memory_order_relaxed);
				}
			}
			logger::error("pre-HDR NGX quality transaction crossed the TemporalAA-seam publish but could not prove its outcome or the host state after it; quality is disabled for the session"sv);
		}
	}

	void LatchQualityPublishUnrecoverable() noexcept
	{
		QualityAbortOperation operation{};
		__try {
			LatchQualityPublishUnrecoverableImpl(&operation);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			LatchAbortOperationFailure(&operation, static_cast<std::uint32_t>(::GetExceptionCode()));
		}
	}

	bool AbortQualityHandbackCandidate() noexcept
	{
		QualityAbortOperation operation{};
		bool pixelsRestored = false;
		__try {
			pixelsRestored = AbortQualityHandbackCandidateImpl(&operation);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			LatchAbortOperationFailure(&operation, static_cast<std::uint32_t>(::GetExceptionCode()));
			pixelsRestored = false;
		}
		return pixelsRestored;
	}

	bool ArmQualityNativeFallback(QualityFallbackSeam a_seam) noexcept
	{
		QualityAbortOperation operation{};
		bool armed = false;
		__try {
			armed = ArmQualityNativeFallbackImpl(a_seam, &operation);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			LatchAbortOperationFailure(&operation, static_cast<std::uint32_t>(::GetExceptionCode()));
			armed = false;
		}
		return armed;
	}

	bool QualityNativeFallbackArmed() noexcept
	{
		return g_qualityPixels.phase == QualityPixelPhase::kNativeFallback;
	}

	bool CloseQualityNativeFallback() noexcept
	{
		bool closed = false;
		__try {
			closed = CloseQualityNativeFallbackImpl();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			const auto exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			g_lastExceptionCode.store(exceptionCode, std::memory_order_relaxed);
			g_lastError.store(static_cast<std::int32_t>(exceptionCode), std::memory_order_relaxed);
			LatchQualityPixelUnrecoverable();
			g_suppressDynamicUpsample = false;
			closed = false;
		}
		return closed;
	}

	std::string_view QualityFallbackSeamName(QualityFallbackSeam a_seam) noexcept
	{
		switch (a_seam) {
		case QualityFallbackSeam::kNone:
			return "none"sv;
		case QualityFallbackSeam::kInjectedPostRt1:
			return "injectedPostRt1"sv;
		case QualityFallbackSeam::kCopyClose:
			return "copyClose"sv;
		case QualityFallbackSeam::kDeviceRemovedAfterCopy:
			return "deviceRemovedAfterCopy"sv;
		case QualityFallbackSeam::kSeamDrawNotAttempted:
			return "seamDrawNotAttempted"sv;
		case QualityFallbackSeam::kSeamAdmissionVeto:
			return "seamAdmissionVeto"sv;
		case QualityFallbackSeam::kInjectedPostTaaStage:
			return "injectedPostTaaStage"sv;
		case QualityFallbackSeam::kInjectedPrePublish:
			return "injectedPrePublish"sv;
		case QualityFallbackSeam::kUnexpectedSecondTaaDraw:
			return "unexpectedSecondTaaDraw"sv;
		case QualityFallbackSeam::kAbnormalTaaReturn:
			return "abnormalTaaReturn"sv;
		default:
			return "unknown"sv;
		}
	}

	std::string_view HandbackSourceName(HandbackSource a_source) noexcept
	{
		switch (a_source) {
		case HandbackSource::kNgx:
			return "ngx"sv;
		case HandbackSource::kFfx:
			return "ffx"sv;
		case HandbackSource::kNone:
			return "none"sv;
		default:
			return "unknown"sv;
		}
	}

	bool ArmQualityFault(QualityFaultSeam a_seam, std::uint32_t a_count) noexcept
	{
		g_qualityFaultArmAttempts.fetch_add(1, std::memory_order_relaxed);
		if (a_seam == QualityFaultSeam::kNone) {
			g_qualityFaultArmState.store(0, std::memory_order_release);
			return true;
		}

		const auto raw = static_cast<std::uint32_t>(a_seam);
		// copyCloseFailure, seamDrawCloseFailure, captureNotifyFailure,
		// geometryPreflightFailure, geometryRestoreFailure and parityDriftFailure each
		// reach a Disable rather than a counted native fallback, so a count above 1 has
		// nothing to repeat: leaving them out of supportsCountedArm is what rejects one
		// below. Whether the Disable ends the session is a separate question, decided by
		// EvaluateRecoveryPredicate at the false -> true edge and not by the seam's name -
		// copyCloseFailure and captureNotifyFailure disable at phase kIdle and therefore
		// suspend and re-arm under failSoft, while seamDrawCloseFailure (kUnrecoverable)
		// and the three sibling-latch seams are terminal whatever failSoft says. With
		// failSoft off all six are terminal, as postPublish always is.
		// The exception is copyCloseFailure with failSoft on: it disables at phase kIdle,
		// so the session suspends and re-arms, and the arm state survives the whole
		// cool-down (nothing in Disable or the recovery machine clears it). A count of two
		// therefore fires the second fault on the first re-armed frame that reaches the
		// copy, which is the only way to exercise a second suspension from outside. With
		// failSoft off the first one is terminal and a second could never be consumed, so
		// the count is refused there rather than accepted and silently stranded. The
		// re-arm budget, not the count, is what bounds how many can actually fire.
		const bool supportsCountedArm =
			a_seam == QualityFaultSeam::kPostTaaStage || a_seam == QualityFaultSeam::kPrePublish ||
			(a_seam == QualityFaultSeam::kCopyCloseFailure &&
				g_failSoft.load(std::memory_order_acquire));
		if (a_count == 0 || a_count > kMaxQualityFaultCount ||
			(a_count > 1 && !supportsCountedArm) ||
			raw > static_cast<std::uint32_t>(QualityFaultSeam::kParityDriftFailure) ||
			g_sessionDisabled.load(std::memory_order_acquire) ||
			!g_ngxContinuousEnabled.load(std::memory_order_acquire) ||
			g_ngxContinuousCompleted.load(std::memory_order_acquire) ||
			g_qualityPixelPhase.load(std::memory_order_acquire) !=
				static_cast<std::uint32_t>(QualityPixelPhase::kIdle) ||
			g_ngxQualityCandidatePending.load(std::memory_order_acquire) ||
			!Hooks::QualityHandbackReady()) {
			g_qualityFaultRejections.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		auto expected = std::uint64_t{ 0 };
		if (!g_qualityFaultArmState.compare_exchange_strong(
				expected,
				QualityFaultArmState(a_seam, a_count),
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			g_qualityFaultRejections.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		g_qualityFaultArms.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool ConsumeQualityFault(QualityFaultSeam a_seam) noexcept
	{
		if (a_seam == QualityFaultSeam::kNone) {
			return false;
		}
		auto current = g_qualityFaultArmState.load(std::memory_order_acquire);
		while (QualityFaultArmSeam(current) == a_seam) {
			const auto remaining = QualityFaultArmRemaining(current);
			if (remaining == 0) {
				return false;
			}
			const auto next = remaining == 1 ? std::uint64_t{ 0 } :
			                                   QualityFaultArmState(a_seam, remaining - 1);
			if (g_qualityFaultArmState.compare_exchange_weak(
					current,
					next,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				g_qualityFaultLastTriggeredSeam.store(
					static_cast<std::uint32_t>(a_seam),
					std::memory_order_release);
				g_qualityFaultTriggers.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		}
		return false;
	}

	std::string_view QualityFaultName(QualityFaultSeam a_seam) noexcept
	{
		switch (a_seam) {
		case QualityFaultSeam::kNone:
			return "none"sv;
		case QualityFaultSeam::kPreBackup:
			return "preBackup"sv;
		case QualityFaultSeam::kPostBackup:
			return "postBackup"sv;
		case QualityFaultSeam::kPostRt1:
			return "postRt1"sv;
		case QualityFaultSeam::kPostTaaStage:
			return "postTaaStage"sv;
		case QualityFaultSeam::kPrePublish:
			return "prePublish"sv;
		case QualityFaultSeam::kPostPublish:
			return "postPublish"sv;
		case QualityFaultSeam::kCopyCloseFailure:
			return "copyCloseFailure"sv;
		case QualityFaultSeam::kSeamDrawCloseFailure:
			return "seamDrawCloseFailure"sv;
		case QualityFaultSeam::kCaptureNotifyFailure:
			return "captureNotifyFailure"sv;
		case QualityFaultSeam::kGeometryPreflightFailure:
			return "geometryPreflightFailure"sv;
		case QualityFaultSeam::kGeometryRestoreFailure:
			return "geometryRestoreFailure"sv;
		case QualityFaultSeam::kParityDriftFailure:
			return "parityDriftFailure"sv;
		default:
			return "unknown"sv;
		}
	}

	std::uint64_t ContinuousCopySerial() noexcept
	{
		return g_ngxContinuousSuccesses.load(std::memory_order_acquire);
	}

	std::uint64_t ContinuousTargetSuccesses() noexcept
	{
		// StereoCapture's FFX capture request is this function's only caller outside
		// /state, and it uses the return value as a bound: a serial at or past it means the
		// continuous gate has retired and the request will never be satisfied, so give up
		// rather than wait forever. On the product path there is no retirement to wait out,
		// so the maximal value - never reached by a live serial - is the correct answer for
		// that comparison. This is not the /state representation: Snapshot() below reports
		// continuousTargetUnbounded as its own field precisely so a human reader is never
		// handed this sentinel and asked to guess what it means.
		const auto state = g_continuousTargetState.load(std::memory_order_acquire);
		return ContinuousTargetUnbounded(state) ? (std::numeric_limits<std::uint64_t>::max)() : TargetFromState(state);
	}

	std::uint64_t PrivateReassemblySerial() noexcept
	{
		return g_privateReassemblySerial.load(std::memory_order_acquire);
	}

	ID3D11Texture2D* PrivateOutput() noexcept
	{
		return g_private.Get();
	}

	bool ReducedWorldHandbackAvailable() noexcept
	{
		// One acquire load, deliberately narrower than ReconstructionLive below: the
		// completion flags it also reads belong to the bounded diagnostic gates, whose
		// runs already depend on the world staying admitted after a budget spends itself.
		// The latch is the whole question here, and it covers both cases the world gate
		// has to deny for - a terminal session, and every frame of a suspended cool-down,
		// since the re-arm is the only writer that clears it.
		return !g_sessionDisabled.load(std::memory_order_acquire);
	}

	bool ReconstructionLive() noexcept
	{
		// Every fail-closed path in this file - Disable, AbortConfiguration, the failed
		// render-thread teardown - ends by setting this, so one load covers all of them.
		if (g_sessionDisabled.load(std::memory_order_acquire)) {
			return false;
		}
		// A spent continuous budget schedules the vendor teardown that releases both
		// histories. The frames after it are vanilla, whatever the gate flags still say.
		// The one-shot gates complete WITHOUT scheduling a teardown, so their completion
		// flags count here too: a create-only or single-evaluate gate that has finished
		// leaves the backend prepared and the bias built while nothing reconstructs.
		return !g_ngxContinuousCompleted.load(std::memory_order_acquire) &&
		       !g_ffxContinuousCompleted.load(std::memory_order_acquire) &&
		       !g_ngxCreateCompleted.load(std::memory_order_acquire) &&
		       !g_ngxEvaluateCompleted.load(std::memory_order_acquire) &&
		       !g_ngxReassembleCompleted.load(std::memory_order_acquire) &&
		       !g_ngxHandbackCompleted.load(std::memory_order_acquire) &&
		       !g_ffxEvaluateCompleted.load(std::memory_order_acquire) &&
		       !g_ffxReassembleCompleted.load(std::memory_order_acquire) &&
		       !g_ffxHandbackCompleted.load(std::memory_order_acquire);
	}

	namespace
	{
		// Off is no gate enabled, whatever sessionDisabled says: a session with nothing
		// armed is not the thing that faulted. Otherwise Suspended/Terminal take priority
		// over Preparing/Active, and Preparing is a gate enabled but the private
		// double-wide/backend views not yet built.
		//
		// A sibling latch is terminal even though this file's own latch is clear. Hooks and
		// TAAReplacement both fold their latch into the readiness the reduced dispatch
		// tests, so tripping one stops the reduced path by making that dispatch decline
		// rather than by faulting: no Disable runs here, no edge is raised, and without
		// this the state would read Active while nothing reconstructs. Neither latch has a
		// recovery, so the state is terminal rather than suspended.
		// Preparing rather than Active also covers the window after a re-arm where the
		// private texture exists again but the backend has not finished re-creating its
		// views: the jitter sequence is armed at the end of that, so a reader who took
		// Active to mean "reconstructing" would see a native-looking frame and no reason
		// for it. Both signals are required.
		//
		// The cool-down reaching zero is not the end of a suspension. TryResolveRearm is
		// retried every frame from there and legitimately waits - for a deferred vendor
		// teardown that has not settled yet - with sessionDisabled still set,
		// suspendFramesLeft at 0 and no refusal recorded. Reading that window as Terminal
		// told a poller a session was dead one frame before it re-armed, and made
		// HudMessages' own edge detector announce "session terminal" on every successful
		// recovery. a_recoveryResolving is that window, and only that window: the suspend
		// edge is what sets the suspension-active bit, and every path that settles the
		// cycle - a refusal, a failed teardown, the re-arm itself - clears it in the same
		// call that records its outcome. A session disabled before the recovery tick has
		// classified it has the bit clear, so a failSoft = false fault still reads Terminal
		// from the first frame rather than passing through a suspension it never had.
		[[nodiscard]] SessionState ComputeSessionState(
			bool a_anyGateEnabled,
			bool a_privateReady,
			bool a_backendPrepared,
			bool a_sessionDisabled,
			bool a_siblingLatchTripped,
			std::uint32_t a_suspendFramesLeft,
			bool a_recoveryResolving) noexcept
		{
			if (!a_anyGateEnabled) {
				return SessionState::kOff;
			}
			if (a_sessionDisabled) {
				return (a_suspendFramesLeft > 0 || a_recoveryResolving) ?
				           SessionState::kSuspended :
				           SessionState::kTerminal;
			}
			if (a_siblingLatchTripped) {
				return SessionState::kTerminal;
			}
			return a_privateReady && a_backendPrepared ? SessionState::kActive :
			                                             SessionState::kPreparing;
		}
	}

	State Snapshot() noexcept
	{
		State state{};
		state.enabled = g_enabled.load(std::memory_order_acquire);
		state.stereoNullEnabled = g_stereoNullEnabled.load(std::memory_order_acquire);
		state.ngxCreateEnabled = g_ngxCreateEnabled.load(std::memory_order_acquire);
		state.ngxEvaluateEnabled = g_ngxEvaluateEnabled.load(std::memory_order_acquire);
		state.ngxReassembleEnabled = g_ngxReassembleEnabled.load(std::memory_order_acquire);
		state.ngxHandbackEnabled = g_ngxHandbackEnabled.load(std::memory_order_acquire);
		state.ngxContinuousEnabled = g_ngxContinuousEnabled.load(std::memory_order_acquire);
		state.ffxEvaluateEnabled = g_ffxEvaluateEnabled.load(std::memory_order_acquire);
		state.ffxReassembleEnabled = g_ffxReassembleEnabled.load(std::memory_order_acquire);
		state.ffxHandbackEnabled = g_ffxHandbackEnabled.load(std::memory_order_acquire);
		state.ffxContinuousEnabled = g_ffxContinuousEnabled.load(std::memory_order_acquire);
		state.isolationAttempted = g_isolationAttempted.load(std::memory_order_acquire);
		state.isolationReady = g_isolationReady.load(std::memory_order_acquire);
		state.privateReady = g_privateReady.load(std::memory_order_acquire);
		state.completed = g_completed.load(std::memory_order_acquire);
		state.stereoNullCompleted = g_stereoNullCompleted.load(std::memory_order_acquire);
		state.ngxCreateCompleted = g_ngxCreateCompleted.load(std::memory_order_acquire);
		state.ngxEvaluateCompleted = g_ngxEvaluateCompleted.load(std::memory_order_acquire);
		state.ngxReassembleCompleted = g_ngxReassembleCompleted.load(std::memory_order_acquire);
		state.ngxHandbackCompleted = g_ngxHandbackCompleted.load(std::memory_order_acquire);
		state.ngxContinuousCompleted = g_ngxContinuousCompleted.load(std::memory_order_acquire);
		state.ngxTeardownPending = g_ngxTeardownPending.load(std::memory_order_acquire);
		state.ngxTeardownCompleted = g_ngxTeardownCompleted.load(std::memory_order_acquire);
		state.ffxEvaluateCompleted = g_ffxEvaluateCompleted.load(std::memory_order_acquire);
		state.ffxReassembleCompleted = g_ffxReassembleCompleted.load(std::memory_order_acquire);
		state.ffxHandbackCompleted = g_ffxHandbackCompleted.load(std::memory_order_acquire);
		state.ffxContinuousCompleted = g_ffxContinuousCompleted.load(std::memory_order_acquire);
		state.ffxTeardownPending = g_ffxTeardownPending.load(std::memory_order_acquire);
		state.ffxTeardownCompleted = g_ffxTeardownCompleted.load(std::memory_order_acquire);
		state.ngxQualityCandidatePending = g_ngxQualityCandidatePending.load(std::memory_order_acquire);
		state.sessionDisabled = g_sessionDisabled.load(std::memory_order_acquire);
		state.failSoft = g_failSoft.load(std::memory_order_acquire);
		state.taaReplacementSessionDisabled = TAAReplacement::SessionDisabled();
		state.taaReplacementDisableReason = TAAReplacement::SessionDisabledReason();
		state.dynamicGeometrySessionDisabled = Hooks::DynamicGeometrySessionDisabled();
		state.dynamicGeometryDisableReason = Hooks::DynamicGeometryDisableReason();
		state.resolvedMode = g_mode.load(std::memory_order_acquire);
		state.productPath = g_productPath.load(std::memory_order_acquire);
		state.suspendFramesLeft = g_suspendFramesLeft.load(std::memory_order_acquire);
		state.rearmCount = g_rearmCount.load(std::memory_order_relaxed);
		state.suspensionCount = g_suspensionCount.load(std::memory_order_relaxed);
		state.recoveryRefusal = g_recoveryRefusal.load(std::memory_order_acquire);
		state.isolationAttempts = g_isolationAttempts.load(std::memory_order_relaxed);
		state.roundTripAttempts = g_roundTripAttempts.load(std::memory_order_relaxed);
		state.successes = g_successes.load(std::memory_order_relaxed);
		state.failures = g_failures.load(std::memory_order_relaxed);
		state.skips = g_skips.load(std::memory_order_relaxed);
		state.resourceBuilds = g_resourceBuilds.load(std::memory_order_relaxed);
		state.stereoNullAttempts = g_stereoNullAttempts.load(std::memory_order_relaxed);
		state.stereoNullSuccesses = g_stereoNullSuccesses.load(std::memory_order_relaxed);
		state.stereoNullFailures = g_stereoNullFailures.load(std::memory_order_relaxed);
		state.stereoNullSkips = g_stereoNullSkips.load(std::memory_order_relaxed);
		state.ngxCreateAttempts = g_ngxCreateAttempts.load(std::memory_order_relaxed);
		state.ngxCreateSuccesses = g_ngxCreateSuccesses.load(std::memory_order_relaxed);
		state.ngxCreateFailures = g_ngxCreateFailures.load(std::memory_order_relaxed);
		state.ngxCreateSkips = g_ngxCreateSkips.load(std::memory_order_relaxed);
		state.ngxEvaluateAttempts = g_ngxEvaluateAttempts.load(std::memory_order_relaxed);
		state.ngxEvaluateSuccesses = g_ngxEvaluateSuccesses.load(std::memory_order_relaxed);
		state.ngxEvaluateFailures = g_ngxEvaluateFailures.load(std::memory_order_relaxed);
		state.ngxEvaluateSkips = g_ngxEvaluateSkips.load(std::memory_order_relaxed);
		state.ngxReassembleAttempts = g_ngxReassembleAttempts.load(std::memory_order_relaxed);
		state.ngxReassembleSuccesses = g_ngxReassembleSuccesses.load(std::memory_order_relaxed);
		state.ngxReassembleFailures = g_ngxReassembleFailures.load(std::memory_order_relaxed);
		state.ngxReassembleSkips = g_ngxReassembleSkips.load(std::memory_order_relaxed);
		state.ngxHandbackAttempts = g_ngxHandbackAttempts.load(std::memory_order_relaxed);
		state.ngxHandbackSuccesses = g_ngxHandbackSuccesses.load(std::memory_order_relaxed);
		state.ngxHandbackFailures = g_ngxHandbackFailures.load(std::memory_order_relaxed);
		state.ngxHandbackSkips = g_ngxHandbackSkips.load(std::memory_order_relaxed);
		state.ngxHandbackCopies = g_ngxHandbackCopies.load(std::memory_order_relaxed);
		state.ngxContinuousAttempts = g_ngxContinuousAttempts.load(std::memory_order_relaxed);
		state.ngxContinuousWarmupSuccesses = g_ngxContinuousWarmupSuccesses.load(std::memory_order_relaxed);
		state.ngxContinuousSuccesses = g_ngxContinuousSuccesses.load(std::memory_order_relaxed);
		state.ngxContinuousFailures = g_ngxContinuousFailures.load(std::memory_order_relaxed);
		state.ngxContinuousSkips = g_ngxContinuousSkips.load(std::memory_order_relaxed);
		state.ngxContinuousCopies = g_ngxContinuousCopies.load(std::memory_order_relaxed);
		state.ngxQualityCandidates = g_ngxQualityCandidates.load(std::memory_order_relaxed);
		state.ngxQualityCommits = g_ngxQualityCommits.load(std::memory_order_relaxed);
		state.ngxQualityAborts = g_ngxQualityAborts.load(std::memory_order_relaxed);
		state.qualityPixelEvaluations = g_qualityPixelEvaluations.load(std::memory_order_relaxed);
		state.qualityPixelCopies = g_qualityPixelCopies.load(std::memory_order_relaxed);
		state.qualityPixelNativeFallbacks = g_qualityPixelNativeFallbacks.load(std::memory_order_relaxed);
		state.qualityPixelFallbackLateCloses = g_qualityPixelFallbackLateCloses.load(std::memory_order_relaxed);
		state.qualityPixelFallbackAbortCloses = g_qualityPixelFallbackAbortCloses.load(std::memory_order_relaxed);
		for (std::size_t i = 0; i < kQualityFallbackSeamCount; ++i) {
			state.qualityPixelFallbackSeamCounts[i] = g_qualityPixelFallbackSeamCounts[i].load(std::memory_order_relaxed);
		}
		state.qualityPixelSeamPublishes = g_qualityPixelSeamPublishes.load(std::memory_order_relaxed);
		state.qualityPixelSeamPublishAbortCloses = g_qualityPixelSeamPublishAbortCloses.load(std::memory_order_relaxed);
		state.qualityPixelDirtyCloses = g_qualityPixelDirtyCloses.load(std::memory_order_relaxed);
		state.qualityPixelUnrecoverable = g_qualityPixelUnrecoverable.load(std::memory_order_relaxed);
		state.qualityFaultArmAttempts = g_qualityFaultArmAttempts.load(std::memory_order_relaxed);
		state.qualityFaultArms = g_qualityFaultArms.load(std::memory_order_relaxed);
		state.qualityFaultRejections = g_qualityFaultRejections.load(std::memory_order_relaxed);
		state.qualityFaultTriggers = g_qualityFaultTriggers.load(std::memory_order_relaxed);
		state.qualityCaptureDisableAtTransactions =
			g_qualityCaptureDisableAtTransactions.load(std::memory_order_relaxed);
		state.ngxHandbackCandidateSerial = g_ngxHandbackCandidateSerial.load(std::memory_order_relaxed);
		{
			const auto continuousState = g_continuousTargetState.load(std::memory_order_relaxed);
			state.ngxContinuousTargetSuccesses = TargetFromState(continuousState);
			state.continuousTargetUnbounded = ContinuousTargetUnbounded(continuousState);
		}
		state.ngxTeardownAttempts = g_ngxTeardownAttempts.load(std::memory_order_relaxed);
		state.ngxTeardownSuccesses = g_ngxTeardownSuccesses.load(std::memory_order_relaxed);
		state.ngxTeardownFailures = g_ngxTeardownFailures.load(std::memory_order_relaxed);
		state.ffxEvaluateAttempts = g_ffxEvaluateAttempts.load(std::memory_order_relaxed);
		state.ffxEvaluateSuccesses = g_ffxEvaluateSuccesses.load(std::memory_order_relaxed);
		state.ffxEvaluateFailures = g_ffxEvaluateFailures.load(std::memory_order_relaxed);
		state.ffxEvaluateSkips = g_ffxEvaluateSkips.load(std::memory_order_relaxed);
		state.ffxReassembleAttempts = g_ffxReassembleAttempts.load(std::memory_order_relaxed);
		state.ffxReassembleSuccesses = g_ffxReassembleSuccesses.load(std::memory_order_relaxed);
		state.ffxReassembleFailures = g_ffxReassembleFailures.load(std::memory_order_relaxed);
		state.ffxReassembleSkips = g_ffxReassembleSkips.load(std::memory_order_relaxed);
		state.ffxHandbackAttempts = g_ffxHandbackAttempts.load(std::memory_order_relaxed);
		state.ffxHandbackSuccesses = g_ffxHandbackSuccesses.load(std::memory_order_relaxed);
		state.ffxHandbackFailures = g_ffxHandbackFailures.load(std::memory_order_relaxed);
		state.ffxHandbackSkips = g_ffxHandbackSkips.load(std::memory_order_relaxed);
		state.ffxHandbackCopies = g_ffxHandbackCopies.load(std::memory_order_relaxed);
		state.ffxContinuousAttempts = g_ffxContinuousAttempts.load(std::memory_order_relaxed);
		state.ffxContinuousWarmupSuccesses = g_ffxContinuousWarmupSuccesses.load(std::memory_order_relaxed);
		state.ffxContinuousSuccesses = g_ffxContinuousSuccesses.load(std::memory_order_relaxed);
		state.ffxContinuousFailures = g_ffxContinuousFailures.load(std::memory_order_relaxed);
		state.ffxContinuousSkips = g_ffxContinuousSkips.load(std::memory_order_relaxed);
		state.ffxContinuousCopies = g_ffxContinuousCopies.load(std::memory_order_relaxed);
		state.ffxContinuousTargetSuccesses = TargetFromState(g_continuousTargetState.load(std::memory_order_relaxed));
		state.privateReassemblySerial = g_privateReassemblySerial.load(std::memory_order_relaxed);
		state.ffxTeardownAttempts = g_ffxTeardownAttempts.load(std::memory_order_relaxed);
		state.ffxTeardownSuccesses = g_ffxTeardownSuccesses.load(std::memory_order_relaxed);
		state.ffxTeardownFailures = g_ffxTeardownFailures.load(std::memory_order_relaxed);
		state.targetSuccesses = kTargetSuccesses;
		state.dynamicUpsampleSuppressionsArmed = g_dynamicUpsampleSuppressionsArmed.load(std::memory_order_relaxed);
		state.threadId = g_threadId.load(std::memory_order_relaxed);
		state.featureLevel = g_featureLevel.load(std::memory_order_relaxed);
		state.width = g_width.load(std::memory_order_relaxed);
		state.height = g_height.load(std::memory_order_relaxed);
		state.format = g_format.load(std::memory_order_relaxed);
		state.sampleCount = g_sampleCount.load(std::memory_order_relaxed);
		state.lastExceptionCode = g_lastExceptionCode.load(std::memory_order_relaxed);
		state.qualityPixelPhase = g_qualityPixelPhase.load(std::memory_order_acquire);
		state.qualityPixelLastFallbackSeam = g_qualityPixelLastFallbackSeam.load(std::memory_order_acquire);
		const auto qualityFaultArmState = g_qualityFaultArmState.load(std::memory_order_acquire);
		state.qualityFaultArmedSeam = static_cast<std::uint32_t>(QualityFaultArmSeam(qualityFaultArmState));
		state.qualityFaultRemaining = QualityFaultArmRemaining(qualityFaultArmState);
		state.qualityFaultLastTriggeredSeam = g_qualityFaultLastTriggeredSeam.load(std::memory_order_acquire);
		state.lastError = g_lastError.load(std::memory_order_relaxed);
		state.deviceRemovedReason = g_deviceRemovedReason.load(std::memory_order_relaxed);
		state.device = g_deviceAddress.load(std::memory_order_relaxed);
		state.context = g_contextAddress.load(std::memory_order_relaxed);
		state.sourceTexture = g_sourceTexture.load(std::memory_order_relaxed);
		state.privateTexture = g_privateTexture.load(std::memory_order_relaxed);
		state.stereoSubrectsRequested = g_stereoSubrectsRequested.load(std::memory_order_acquire);
		state.stereoSubrectsEffective = g_stereoSubrects.load(std::memory_order_acquire);
		state.stereoBackend = g_stereoBackend.load(std::memory_order_acquire);
		const bool anyGateEnabled = state.enabled || state.stereoNullEnabled ||
		                            state.ngxCreateEnabled || state.ngxEvaluateEnabled ||
		                            state.ngxReassembleEnabled || state.ngxHandbackEnabled ||
		                            state.ngxContinuousEnabled || state.ffxEvaluateEnabled ||
		                            state.ffxReassembleEnabled || state.ffxHandbackEnabled ||
		                            state.ffxContinuousEnabled;
		const bool siblingLatchTripped =
			state.taaReplacementSessionDisabled || state.dynamicGeometrySessionDisabled;
		// Both halves read here rather than inside ComputeSessionState so the reducer stays
		// a pure function of its arguments: a suspension that is still resolving is one
		// this file's own recovery tick opened and has not settled, with no refusal
		// recorded against it.
		const bool recoveryResolving =
			g_recoverySuspensionActive.load(std::memory_order_acquire) &&
			static_cast<RecoveryRefusal>(state.recoveryRefusal) == RecoveryRefusal::kNone;
		state.sessionState = static_cast<std::uint32_t>(ComputeSessionState(
			anyGateEnabled,
			state.privateReady,
			Evaluate::Prepared(),
			state.sessionDisabled,
			siblingLatchTripped,
			state.suspendFramesLeft,
			recoveryResolving));
		// A sibling latch stops the reduced path without ever raising a fault here, so the
		// refusal that names why is set on the way out rather than by the recovery tick.
		// Anything the tick already recorded outranks it: that came from a real fault.
		if (siblingLatchTripped &&
			static_cast<RecoveryRefusal>(state.recoveryRefusal) == RecoveryRefusal::kNone) {
			state.recoveryRefusal = static_cast<std::uint32_t>(RecoveryRefusal::kSiblingLatchTripped);
		}
		return state;
	}

	std::string_view StereoBackendName(StereoBackend a_backend) noexcept
	{
		switch (a_backend) {
		case StereoBackend::kNgx:
			return "ngx"sv;
		case StereoBackend::kFfx:
			return "ffx"sv;
		case StereoBackend::kNull:
			return "null"sv;
		case StereoBackend::kOther:
			return "other"sv;
		case StereoBackend::kNone:
		default:
			return "none"sv;
		}
	}

	std::string_view RecoveryRefusalName(RecoveryRefusal a_refusal) noexcept
	{
		switch (a_refusal) {
		case RecoveryRefusal::kMidTransaction:
			return "midTransaction"sv;
		case RecoveryRefusal::kDeviceRemoved:
			return "deviceRemoved"sv;
		case RecoveryRefusal::kSiblingLatchTripped:
			return "siblingLatchTripped"sv;
		case RecoveryRefusal::kFailSoftDisabled:
			return "failSoftDisabled"sv;
		case RecoveryRefusal::kRearmBudgetExhausted:
			return "rearmBudgetExhausted"sv;
		case RecoveryRefusal::kTeardownFailed:
			return "teardownFailed"sv;
		case RecoveryRefusal::kGatesClearedDuringCooldown:
			return "gatesClearedDuringCooldown"sv;
		case RecoveryRefusal::kNone:
		default:
			return "none"sv;
		}
	}

	std::string_view SessionStateName(SessionState a_state) noexcept
	{
		switch (a_state) {
		case SessionState::kPreparing:
			return "preparing"sv;
		case SessionState::kActive:
			return "active"sv;
		case SessionState::kSuspended:
			return "suspended"sv;
		case SessionState::kTerminal:
			return "terminal"sv;
		case SessionState::kOff:
		default:
			return "off"sv;
		}
	}
}
