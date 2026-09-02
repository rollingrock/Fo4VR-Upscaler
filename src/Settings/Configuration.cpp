#include "Settings/Configuration.h"

#include "Upscaler/DynamicResolution.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hooks.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/ResetPolicy.h"
#include "Upscaler/ScopePolicy.h"
#include "Upscaler/StereoLayout.h"
#include "Upscaler/TAAReplacement.h"
#include "Upscaler/Telemetry.h"

#include <exception>
#include <memory>
#include <string>

namespace Configuration
{
	namespace
	{
		struct RuntimeSnapshot
		{
			bool enabled = false;
			bool diagnosticDynamicResolution = false;
			double renderScaleOverride = 0.0;
			bool diagnosticTaaReplacement = false;
			bool diagnosticTaaBypass = false;
			bool diagnosticHdrRoundTrip = false;
			bool diagnosticStereoNull = false;
			bool diagnosticNgxCreate = false;
			bool diagnosticNgxEvaluate = false;
			bool diagnosticNgxReassemble = false;
			bool diagnosticNgxHandback = false;
			bool diagnosticNgxContinuous = false;
			bool diagnosticFfxEvaluate = false;
			bool diagnosticFfxReassemble = false;
			bool diagnosticFfxHandback = false;
			bool diagnosticFfxContinuous = false;
			std::uint64_t diagnosticContinuousFrames = 0;
			bool resetOnMenu = false;
			bool resetOnLoad = false;
			bool resetOnCell = false;
			bool suppressDuringScopeRender = false;
			bool forceHiddenAreaMeshOff = true;
			bool unbindDepthStencil = true;
			bool perfTimers = false;
			double sharpness = 0.0;
			std::string runtimeDir;
			std::string preset;
			std::string quality;
		};

		struct ApplyOperation
		{
			const Observer* observer = nullptr;
			RuntimeSnapshot snapshot;
			ApplyResult result{};
			bool locked = false;
			bool returned = false;
			bool callbackResult = false;
		};

		[[nodiscard]] bool CaptureRuntimeSnapshot(void* a_context) noexcept
		{
			auto* const snapshot = static_cast<RuntimeSnapshot*>(a_context);
			if (!snapshot) {
				return false;
			}
			try {
				snapshot->enabled = *Settings::enabled;
				snapshot->diagnosticDynamicResolution = *Settings::diagnosticDynamicResolution;
				snapshot->renderScaleOverride = *Settings::renderScaleOverride;
				snapshot->diagnosticTaaReplacement = *Settings::diagnosticTaaReplacement;
				snapshot->diagnosticTaaBypass = *Settings::diagnosticTaaBypass;
				snapshot->diagnosticHdrRoundTrip = *Settings::diagnosticHdrRoundTrip;
				snapshot->diagnosticStereoNull = *Settings::diagnosticStereoNull;
				snapshot->diagnosticNgxCreate = *Settings::diagnosticNgxCreate;
				snapshot->diagnosticNgxEvaluate = *Settings::diagnosticNgxEvaluate;
				snapshot->diagnosticNgxReassemble = *Settings::diagnosticNgxReassemble;
				snapshot->diagnosticNgxHandback = *Settings::diagnosticNgxHandback;
				snapshot->diagnosticNgxContinuous = *Settings::diagnosticNgxContinuous;
				snapshot->diagnosticFfxEvaluate = *Settings::diagnosticFfxEvaluate;
				snapshot->diagnosticFfxReassemble = *Settings::diagnosticFfxReassemble;
				snapshot->diagnosticFfxHandback = *Settings::diagnosticFfxHandback;
				snapshot->diagnosticFfxContinuous = *Settings::diagnosticFfxContinuous;
				snapshot->diagnosticContinuousFrames =
					static_cast<std::uint64_t>(*Settings::diagnosticContinuousFrames);
				snapshot->resetOnMenu = *Settings::resetOnMenu;
				snapshot->resetOnLoad = *Settings::resetOnLoad;
				snapshot->resetOnCell = *Settings::resetOnCell;
				snapshot->suppressDuringScopeRender = *Settings::suppressDuringScopeRender;
				snapshot->forceHiddenAreaMeshOff = *Settings::forceHiddenAreaMeshOff;
				snapshot->unbindDepthStencil = *Settings::unbindDepthStencil;
				snapshot->perfTimers = *Settings::perfTimers;
				snapshot->sharpness = *Settings::sharpness;
				snapshot->runtimeDir = *Settings::runtimeDir;
				snapshot->preset = *Settings::preset;
				snapshot->quality = *Settings::quality;
				return true;
			} catch (...) {
				return false;
			}
		}

		void AbortRuntimeConfiguration() noexcept
		{
			// These APIs are atomics/guarded writes only.  In particular they do not
			// acquire a component mutex that the interrupted call may have stranded.
			// Performance instrumentation is development-only and therefore fails closed:
			// a rejected reload must never leave a previously enabled timer window running.
			Upscaler::Telemetry::Configure(false);
			// A backend-owned jitter sequence outliving the configuration that armed it
			// would keep writing samples for an extent nothing is preparing any more.
			Upscaler::Jitter::DisarmBackendSequence();
			Upscaler::TAAReplacement::AbortConfiguration();
			Upscaler::HDRRoundTrip::AbortConfiguration();
			Upscaler::DynamicResolution::AbortConfiguration();
		}

		[[nodiscard]] bool ApplyLockedImpl(ApplyOperation& a_operation)
		{
			if (a_operation.observer && a_operation.observer->beforeLoad) {
				a_operation.result.beforeObserved = Settings::WithLoadLock(
					a_operation.observer->beforeLoad,
					a_operation.observer->context,
					&a_operation.result.exceptionCode);
				if (!a_operation.result.beforeObserved) {
					return false;
				}
			}

			a_operation.result.settingsLoaded = Settings::load();
			if (!a_operation.result.settingsLoaded) {
				return false;
			}
			a_operation.result.settingsSnapshotted = Settings::WithLoadLock(
				CaptureRuntimeSnapshot,
				std::addressof(a_operation.snapshot),
				&a_operation.result.exceptionCode);
			if (!a_operation.result.settingsSnapshotted) {
				return false;
			}

			// Set before the call: an SEH anywhere inside BeginConfiguration may
			// already have made the epoch odd, so the outer finally must normalize it.
			a_operation.result.runtimeConfigurationBegan = true;
			if (!Upscaler::HDRRoundTrip::BeginConfiguration()) {
				return false;
			}

			const auto& snapshot = a_operation.snapshot;
			// 0.0 means "use the documented per-quality-mode default ratio" rather than
			// "hand-type one". DLAA is deliberately excluded: its ratio is 1.0, which is
			// not a reduced mode, and DynamicResolution's own 0.30..1.00 validation still
			// owns every other decision about the value.
			double renderScale = snapshot.renderScaleOverride;
			if (renderScale == 0.0 && snapshot.diagnosticDynamicResolution) {
				double defaultScale = 0.0;
				if (Upscaler::HDRRoundTrip::DefaultRenderScaleForQuality(snapshot.quality, defaultScale) &&
					defaultScale < 1.0) {
					renderScale = defaultScale;
					logger::info(FMT_STRING("renderScaleOverride is 0, so quality '{}' uses its documented default ratio {:.4f}"),
						snapshot.quality, renderScale);
				}
			}
			const bool gateReducedNgxToWorld = snapshot.diagnosticDynamicResolution &&
			                                   renderScale < 1.0 &&
			                                   (snapshot.diagnosticNgxHandback || snapshot.diagnosticNgxContinuous);
			Upscaler::DynamicResolution::Configure(
				snapshot.enabled,
				snapshot.diagnosticDynamicResolution,
				renderScale,
				snapshot.forceHiddenAreaMeshOff,
				gateReducedNgxToWorld);
			if (!Upscaler::TAAReplacement::Configure(
					snapshot.diagnosticTaaReplacement,
					snapshot.diagnosticTaaBypass,
					snapshot.diagnosticNgxContinuous,
					snapshot.diagnosticFfxContinuous,
					snapshot.diagnosticContinuousFrames)) {
				return false;
			}
			Upscaler::ResetPolicy::Configure(
				snapshot.resetOnMenu,
				snapshot.resetOnLoad,
				snapshot.resetOnCell);
			Upscaler::ScopePolicy::Configure(snapshot.suppressDuringScopeRender);
			static_cast<void>(Upscaler::StereoLayout::Resolve());
			// This setting participates in the same admission snapshot as the Quality
			// gates. Publish it while HDRRoundTrip's epoch is still odd; Configure below
			// is the final operation that makes the new runtime epoch visible.
			Upscaler::Hooks::Configure(snapshot.unbindDepthStencil);
			if (!Upscaler::HDRRoundTrip::Configure(
				snapshot.diagnosticHdrRoundTrip,
				snapshot.diagnosticStereoNull,
				snapshot.diagnosticNgxCreate,
				snapshot.diagnosticNgxEvaluate,
				snapshot.diagnosticNgxReassemble,
				snapshot.diagnosticNgxHandback,
				snapshot.diagnosticNgxContinuous,
				snapshot.diagnosticFfxEvaluate,
				snapshot.diagnosticFfxReassemble,
				snapshot.diagnosticFfxHandback,
				snapshot.diagnosticFfxContinuous,
				snapshot.diagnosticContinuousFrames,
				snapshot.runtimeDir,
				snapshot.preset,
					snapshot.quality,
					static_cast<float>(snapshot.sharpness))) {
				return false;
			}
			// Atomic intent only. Query creation, collection and release are deferred to
			// the verified render thread by Telemetry itself.
			Upscaler::Telemetry::Configure(snapshot.perfTimers);
			a_operation.result.runtimeApplied = true;

			if (a_operation.observer && a_operation.observer->afterApply) {
				a_operation.result.afterObserved = Settings::WithLoadLock(
					a_operation.observer->afterApply,
					a_operation.observer->context,
					&a_operation.result.exceptionCode);
				if (!a_operation.result.afterObserved) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool ApplyLockedThunk(ApplyOperation& a_operation) noexcept
		{
			try {
				return ApplyLockedImpl(a_operation);
			} catch (const std::exception& e) {
				logger::error(FMT_STRING("configuration apply failed: {}"), e.what());
			} catch (...) {
				logger::error("configuration apply failed with a non-standard C++ exception"sv);
			}
			return false;
		}

		// Kept separate from ApplyOperation because MSVC forbids destructible C++
		// locals in a function containing __try.  This shell is pointer/bool only.
		void RunApplyShell(ApplyOperation* a_operation) noexcept
		{
			struct Shell
			{
				ApplyOperation* operation = nullptr;
				bool locked = false;
			};
			Shell shell{};
			shell.operation = a_operation;
			__try {
				__try {
					if (shell.operation && Settings::detail::Acquire(Settings::configurationLock)) {
						shell.locked = true;
						shell.operation->callbackResult = ApplyLockedThunk(*shell.operation);
						shell.operation->returned = true;
					}
				} __finally {
					__try {
						if (shell.operation &&
							shell.operation->result.runtimeConfigurationBegan &&
							!shell.operation->result.runtimeApplied) {
							AbortRuntimeConfiguration();
						}
					} __finally {
						// The outer lock is never conditional on recovery succeeding.  A
						// second hardware fault in a guarded Dynamic rollback still reaches
						// this nested finally before the exception is reported.
						if (shell.locked) {
							Settings::configurationLock.unlock();
							shell.locked = false;
						}
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				if (shell.operation) {
					shell.operation->result.exceptionCode =
						static_cast<std::uint32_t>(::GetExceptionCode());
				}
			}
		}
	}

	ApplyResult Apply(const Observer* a_observer) noexcept
	{
		ApplyOperation operation{};
		operation.observer = a_observer;
		RunApplyShell(std::addressof(operation));
		if (!operation.returned && operation.result.exceptionCode != 0) {
			logger::error(FMT_STRING("configuration apply raised SEH 0x{:08X}"), operation.result.exceptionCode);
		}
		return operation.result;
	}
}
