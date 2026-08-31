#include "Upscaler/HDRRoundTrip.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Backend/BackendRegistry.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Evaluate.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/ScopePolicy.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
		std::atomic<std::shared_ptr<const std::string>> g_ngxRuntimeDir;

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
		std::atomic<std::uint64_t> g_acceptedHandbackStamp{ 0 };
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
		// approach the 2^62 accepted-frame range reserved by this packing.
		void PublishAcceptedContinuousHandback(
			HandbackSource a_source,
			std::uint64_t a_serial) noexcept
		{
			const auto packed = (a_serial << kHandbackSourceBits) |
			                    static_cast<std::uint64_t>(a_source);
			g_acceptedHandbackStamp.store(packed, std::memory_order_release);
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

		[[nodiscard]] bool EnsurePrivate(const RenderTargets::PhysRT& a_source, Gate a_gate) noexcept
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
			result.began = a_isolation.Begin();
			if (!result.began) {
				return result;
			}

			__try {
				__try {
					a_context->CopyResource(a_private, a_source);
					a_context->CopyResource(a_source, a_private);
					result.copied = true;
				} __finally {
					result.ended = a_isolation.End();
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			return result;
		}

		struct StereoResult
		{
			bool began = false;
			bool evaluated = false;
			bool copied = false;
			bool ended = false;
			std::uint32_t exceptionCode = 0;
		};

		struct PrepareResult
		{
			bool began = false;
			bool prepared = false;
			bool ended = false;
			std::uint32_t exceptionCode = 0;
		};

		struct PrivateResult
		{
			bool began = false;
			bool evaluated = false;
			bool copied = false;
			bool ended = false;
			std::uint32_t exceptionCode = 0;
		};

		struct TeardownResult
		{
			bool began = false;
			bool reset = false;
			bool deactivated = false;
			bool ended = false;
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
			result.began = a_isolation.Begin();
			if (!result.began) {
				return result;
			}

			__try {
				__try {
					result.reset = Evaluate::Reset();
					if (result.reset) {
						result.deactivated = Backend::DeactivatePrepared(a_backend);
					}
				} __finally {
					result.ended = a_isolation.End();
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
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
			result.began = a_isolation.Begin();
			if (!result.began) {
				return result;
			}

			__try {
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
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			return result;
		}

		[[nodiscard]] bool RunNgxCreate(
			const D3D11_TEXTURE2D_DESC& a_sourceDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			std::string_view a_seam) noexcept
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
			config.renderWidth = a_sourceDesc.Width / 2u;
			config.renderHeight = a_sourceDesc.Height;
			config.outputWidth = a_sourceDesc.Width / 2u;
			config.outputHeight = a_sourceDesc.Height;
			config.quality = Backend::QualityMode::kNativeAA;
			config.sharpness = 0.0f;
			config.hdr = true;
			config.invertedDepth = false;
			config.jitteredMV = false;
			config.lowResMV = true;
			config.autoExposure = true;
			config.preExposure = 1.0f;
			config.exposureScale = 1.0f;

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
			if (!result.began || !result.prepared || !result.ended || result.exceptionCode != 0) {
				Disable(
					!result.began || !result.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
					result.exceptionCode,
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
			logger::info(FMT_STRING("NGX create-only gate at {}: prepared two independent native DLAA histories at {}x{} per eye, prepare count {}, runtime {} at {}; no frame evaluated or copied"),
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
			result.began = a_isolation.Begin();
			if (!result.began) {
				return result;
			}

			__try {
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
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
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
			result.began = a_isolation.Begin();
			if (!result.began) {
				return result;
			}

			__try {
				__try {
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
				} __finally {
					result.ended = a_isolation.End();
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			return result;
		}

		// Pixel-changing vendor boundary: evaluate and reassemble into the private
		// double-wide texture, then issue one descriptor-matched copy into logical color 1.
		// The caller supplies its own direct-copy counter so one-shot and bounded-continuous
		// policy remain separate. Keep this free of destructible C++ locals so __finally
		// remains the host restoration boundary even if the vendor or D3D11 call raises SEH.
		[[nodiscard]] PrivateResult EvaluateBackendHandbackIsolated(
			D3D11StateIsolation::Context& a_isolation,
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			ID3D11Texture2D* a_engineSource,
			ID3D11Texture2D* a_depth,
			ID3D11Texture2D* a_motionVectors,
			ID3D11Texture2D* a_privateOutput,
			const D3D11_TEXTURE2D_DESC& a_sourceDesc,
			const D3D11_TEXTURE2D_DESC& a_depthDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			const Backend::Config& a_config,
			std::string_view a_runtimeDir,
			std::atomic<std::uint64_t>& a_copyCounter) noexcept
		{
			PrivateResult result{};
			result.began = a_isolation.Begin();
			if (!result.began) {
				return result;
			}

			__try {
				__try {
					result.evaluated = Evaluate::ReassembledFrame(
						a_backend,
						a_device,
						a_context,
						a_engineSource,
						a_depth,
						a_motionVectors,
						a_privateOutput,
						a_sourceDesc,
						a_depthDesc,
						a_motionDesc,
						a_config,
						a_runtimeDir);
					if (result.evaluated) {
						a_context->CopyResource(a_engineSource, a_privateOutput);
						a_copyCounter.fetch_add(1, std::memory_order_relaxed);
						result.copied = true;
					}
				} __finally {
					result.ended = a_isolation.End();
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
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
			result.began = a_isolation.Begin();
			if (!result.began) {
				return result;
			}

			__try {
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
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			return result;
		}
	}

	void Configure(
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
		std::string_view a_runtimeDir) noexcept
	{
		const bool hadRawNgx = g_ngxCreateEnabled.load(std::memory_order_acquire) ||
		                       g_ngxEvaluateEnabled.load(std::memory_order_acquire) ||
		                       g_ngxReassembleEnabled.load(std::memory_order_acquire) ||
		                       g_ngxHandbackEnabled.load(std::memory_order_acquire) ||
		                       g_ngxContinuousEnabled.load(std::memory_order_acquire);
		const bool hasRawNgx = a_ngxCreateEnabled || a_ngxEvaluateEnabled ||
		                       a_ngxReassembleEnabled || a_ngxHandbackEnabled ||
		                       a_ngxContinuousEnabled;
		const bool hadFfx = g_ffxEvaluateEnabled.load(std::memory_order_acquire) ||
		                    g_ffxReassembleEnabled.load(std::memory_order_acquire) ||
		                    g_ffxHandbackEnabled.load(std::memory_order_acquire) ||
		                    g_ffxContinuousEnabled.load(std::memory_order_acquire);
		const bool hasFfx = a_ffxEvaluateEnabled || a_ffxReassembleEnabled ||
		                    a_ffxHandbackEnabled || a_ffxContinuousEnabled;
		g_enabled.store(a_roundTripEnabled, std::memory_order_release);
		g_stereoNullEnabled.store(a_stereoNullEnabled, std::memory_order_release);
		g_ffxEvaluateEnabled.store(a_ffxEvaluateEnabled, std::memory_order_release);
		g_ffxReassembleEnabled.store(a_ffxReassembleEnabled, std::memory_order_release);
		g_ffxHandbackEnabled.store(a_ffxHandbackEnabled, std::memory_order_release);
		g_ffxContinuousEnabled.store(a_ffxContinuousEnabled, std::memory_order_release);
		try {
			auto runtimeDir = std::make_shared<const std::string>(a_runtimeDir);
			g_ngxRuntimeDir.store(std::move(runtimeDir), std::memory_order_release);
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
			logger::error("raw-NGX development diagnostics disabled: runtimeDir snapshot allocation failed"sv);
		}
		const auto owner = Evaluate::Snapshot();
		const bool ownsNgx = owner.ownsBackendState &&
		                     owner.backend == reinterpret_cast<std::uintptr_t>(Backend::Find("ngx"sv));
		const bool ownsFfx = owner.ownsBackendState &&
		                     owner.backend == reinterpret_cast<std::uintptr_t>(Backend::Find("ffx"sv));
		if (hadRawNgx && !hasRawNgx && ownsNgx) {
			g_ngxTeardownPending.store(true, std::memory_order_release);
		}
		if (hadFfx && !hasFfx && ownsFfx) {
			g_ffxTeardownPending.store(true, std::memory_order_release);
		}
	}

	void OnPrimaryRenderThread() noexcept
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
		static_cast<void>(RunNgxCreate(sourceDesc, motionDesc, "primary ClearColor prewarm"sv));
	}

	void OnDispatch(std::int32_t a_sourceLogical, std::int32_t a_destinationLogical) noexcept
	{
		if (ScopePolicy::ShouldSuppressCurrentDispatch()) {
			return;
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
			const HRESULT removed = g_device ? g_device->GetDeviceRemovedReason() : E_POINTER;
			g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
			if (result.reset && result.ended && result.exceptionCode == 0) {
				g_private.Reset();
				g_privateDesc = {};
				g_privateReady.store(false, std::memory_order_release);
				g_privateTexture.store(0, std::memory_order_relaxed);
			}
			const bool releaseDeferred = result.began && !result.reset && result.ended &&
			                             result.exceptionCode == 0 && !FAILED(removed);
			const auto deferredAttempt = releaseDeferred ? (runFfxTeardown ?
				g_ffxTeardownDeferrals.fetch_add(1, std::memory_order_relaxed) + 1 :
				g_ngxTeardownDeferrals.fetch_add(1, std::memory_order_relaxed) + 1) : 0;
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
			if (!result.began || !result.reset || !result.deactivated || !result.ended || result.exceptionCode != 0 || FAILED(removed)) {
				if (runFfxTeardown) {
					g_ffxTeardownFailures.fetch_add(1, std::memory_order_relaxed);
					g_ffxTeardownPending.store(false, std::memory_order_release);
				} else {
					g_ngxTeardownFailures.fetch_add(1, std::memory_order_relaxed);
					g_ngxTeardownPending.store(false, std::memory_order_release);
				}
				g_sessionDisabled.store(true, std::memory_order_release);
				logger::error(FMT_STRING("{} render-thread teardown failed: begin {}, reset {}, deactivate {}, end {}, HRESULT {:08X}, exception {:08X}; vanilla HDR continues"),
					backendName,
					result.began,
					result.reset,
					result.deactivated,
					result.ended,
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
			if (runNgxCreate) {
				g_ngxCreateSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runNgxEvaluate) {
				g_ngxEvaluateSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runNgxReassemble) {
				g_ngxReassembleSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runNgxHandback) {
				g_ngxHandbackSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runNgxContinuous) {
				g_ngxContinuousSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runFfxEvaluate) {
				g_ffxEvaluateSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runFfxReassemble) {
				g_ffxReassembleSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runFfxHandback) {
				g_ffxHandbackSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runFfxContinuous) {
				g_ffxContinuousSkips.fetch_add(1, std::memory_order_relaxed);
			} else if (runStereoNull) {
				g_stereoNullSkips.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_skips.fetch_add(1, std::memory_order_relaxed);
			}
			return;
		}
		if (Engine::Device() != g_device || Engine::ImmediateContext() != g_context) {
			Disable(E_UNEXPECTED, 0, "the engine device or immediate context changed after isolation initialization", gate);
			return;
		}

		const auto source = RenderTargets::Lookup(a_sourceLogical);
		if (!EnsurePrivate(source, gate)) {
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
			static_cast<void>(RunNgxCreate(g_privateDesc, motionDesc, "first HDR fallback"sv));
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

			Backend::Config config{};
			config.renderWidth = g_privateDesc.Width / 2u;
			config.renderHeight = g_privateDesc.Height;
			config.outputWidth = g_privateDesc.Width / 2u;
			config.outputHeight = g_privateDesc.Height;
			config.quality = Backend::QualityMode::kNativeAA;
			config.sharpness = 0.0f;
			config.hdr = true;
			config.invertedDepth = false;
			config.jitteredMV = false;
			config.lowResMV = true;
			config.autoExposure = true;
			config.preExposure = 1.0f;
			config.exposureScale = 1.0f;

			const bool ngxContinuousWarmup = runNgxContinuous &&
			                                 g_ngxContinuousWarmupSuccesses.load(std::memory_order_acquire) < kNgxContinuousWarmupSuccesses;
			const bool ffxContinuousWarmup = runFfxContinuous &&
			                                 g_ffxContinuousWarmupSuccesses.load(std::memory_order_acquire) < kFfxContinuousWarmupSuccesses;
			const bool continuousWarmup = ngxContinuousWarmup || ffxContinuousWarmup;
			const bool requiresCopy = runNgxHandback || runFfxHandback ||
			                          ((runNgxContinuous || runFfxContinuous) && !continuousWarmup);
			PrivateResult result{};
			if (runFfxHandback) {
				g_ffxHandbackAttempts.fetch_add(1, std::memory_order_relaxed);
				result = EvaluateBackendHandbackIsolated(
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
					runtimeDir,
					g_ffxHandbackCopies);
			} else if (runNgxHandback) {
				g_ngxHandbackAttempts.fetch_add(1, std::memory_order_relaxed);
				result = EvaluateBackendHandbackIsolated(
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
					runtimeDir,
					g_ngxHandbackCopies);
			} else if (runNgxContinuous) {
				g_ngxContinuousAttempts.fetch_add(1, std::memory_order_relaxed);
				if (continuousWarmup) {
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
					result = EvaluateBackendHandbackIsolated(
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
						runtimeDir,
						g_ngxContinuousCopies);
				}
			} else if (runFfxContinuous) {
				g_ffxContinuousAttempts.fetch_add(1, std::memory_order_relaxed);
				if (continuousWarmup) {
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
					result = EvaluateBackendHandbackIsolated(
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
						runtimeDir,
						g_ffxContinuousCopies);
				}
			} else if (runNgxReassemble || runFfxReassemble) {
				if (runFfxReassemble) {
					g_ffxReassembleAttempts.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_ngxReassembleAttempts.fetch_add(1, std::memory_order_relaxed);
				}
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
			if (!result.began || !result.evaluated || (requiresCopy && !result.copied) || !result.ended || result.exceptionCode != 0) {
				Disable(
					!result.began || !result.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
					result.exceptionCode,
					!result.began             ? "context-state entry failed for private vendor evaluation"sv :
					!result.ended             ? "exact host-state restoration failed after private vendor evaluation"sv :
					result.exceptionCode != 0 ? "private vendor evaluation raised an SEH exception"sv :
					!result.evaluated         ? "the colour-depth-motion private vendor orchestrator declined"sv :
					!result.copied && requiresCopy ? "private vendor evaluation completed but engine color handback did not run"sv :
					                            "private vendor evaluation failed without a classified step"sv,
					vendorGate);
				return;
			}

			const HRESULT removed = g_device->GetDeviceRemovedReason();
			g_deviceRemovedReason.store(static_cast<std::int32_t>(removed), std::memory_order_relaxed);
			if (FAILED(removed)) {
				Disable(static_cast<std::int32_t>(removed), 0, "the D3D11 device reported removal after private vendor evaluation", vendorGate);
				return;
			}
			const auto caps = backend->Probe(g_device);
			if (!caps.available || !Backend::ActivatePrepared(backend)) {
				Disable(E_FAIL, 0, "the vendor backend evaluated successfully but its adapter-bound capability or registry commit failed", vendorGate);
				return;
			}

			const auto evaluate = Evaluate::Snapshot();
			if (runNgxContinuous) {
				if (continuousWarmup) {
					const auto warmup = g_ngxContinuousWarmupSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
					if (warmup == 1) {
						logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: private temporal warm-up began at {}x{} per eye; engine color remains untouched for {} accepted frames"),
							config.renderWidth,
							config.renderHeight,
							kNgxContinuousWarmupSuccesses);
					}
					if (warmup >= kNgxContinuousWarmupSuccesses) {
						logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: completed {} private warm-up frames with both histories preserved; the next accepted frame begins the {}-frame engine-handback window"),
							warmup,
							kNgxContinuousTargetSuccesses);
					}
				} else {
					const auto successes = g_ngxContinuousSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
					PublishAcceptedContinuousHandback(HandbackSource::kNgx, successes);
					if (successes == 1) {
						logger::info(FMT_STRING("pre-HDR NGX bounded-continuous gate: engine handback active after {} private warm-up frames; target {} consecutive copied frames"),
							g_ngxContinuousWarmupSuccesses.load(std::memory_order_relaxed),
							kNgxContinuousTargetSuccesses);
					}
					if (successes >= kNgxContinuousTargetSuccesses) {
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
						logger::info(FMT_STRING("pre-HDR FSR bounded-continuous gate: completed {} private warm-up frames with both histories preserved; the next accepted frame begins the {}-frame engine-handback window"),
							warmup,
							kFfxContinuousTargetSuccesses);
					}
				} else {
					const auto successes = g_ffxContinuousSuccesses.fetch_add(1, std::memory_order_acq_rel) + 1;
					PublishAcceptedContinuousHandback(HandbackSource::kFfx, successes);
					if (successes == 1) {
						logger::info(FMT_STRING("pre-HDR FSR bounded-continuous gate: engine handback active after {} private warm-up frames; target {} consecutive copied frames"),
							g_ffxContinuousWarmupSuccesses.load(std::memory_order_relaxed),
							kFfxContinuousTargetSuccesses);
					}
					if (successes >= kFfxContinuousTargetSuccesses) {
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
				g_privateReassemblySerial.fetch_add(1, std::memory_order_release);
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
			} else if (runNgxHandback) {
				g_ngxHandbackSuccesses.fetch_add(1, std::memory_order_relaxed);
				g_ngxHandbackCompleted.store(true, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR NGX engine-handback gate: evaluated and reassembled one two-eye native DLAA frame at {}x{} per eye, evaluates {}/{}, reassemblies {}/{}, engine copies {}, prepare count {}, runtime {} at {}; logical color 1 now carries the one-shot result and vanilla HDR follows"),
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
				g_privateReassemblySerial.fetch_add(1, std::memory_order_release);
				logger::info(FMT_STRING("pre-HDR NGX private-reassembly gate: evaluated and reassembled one two-eye native DLAA frame at {}x{} per eye, evaluates {}/{}, reassemblies {}/{}, prepare count {}, runtime {} at {}; private double-wide output was not copied to engine colour, vanilla HDR follows untouched"),
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
				g_privateReassemblySerial.fetch_add(1, std::memory_order_release);
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
				logger::info(FMT_STRING("pre-HDR NGX private-evaluate gate: evaluated one two-eye native DLAA frame at {}x{} per eye, left/right {}/{}, prepare count {}, runtime {} at {}; outputs remain private with no reassembly or engine colour copy, vanilla HDR follows untouched"),
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
			if (!result.began || !result.evaluated || !result.copied || !result.ended || result.exceptionCode != 0) {
				Disable(
					!result.began || !result.ended ? static_cast<std::int32_t>(g_isolation.LastError()) : E_FAIL,
					result.exceptionCode,
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
		if (!result.began || !result.copied || !result.ended || result.exceptionCode != 0) {
			Disable(
				static_cast<std::int32_t>(g_isolation.LastError()),
				result.exceptionCode,
				!result.began ? "context-state entry failed"sv :
				!result.ended ? "exact host-state restoration failed"sv :
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

	HandbackStamp AcceptedContinuousHandbackStamp() noexcept
	{
		const auto packed = g_acceptedHandbackStamp.load(std::memory_order_acquire);
		return {
			static_cast<HandbackSource>(packed & kHandbackSourceMask),
			packed >> kHandbackSourceBits
		};
	}

	std::uint64_t AcceptedContinuousHandbackSerial(HandbackSource a_source) noexcept
	{
		switch (a_source) {
		case HandbackSource::kNgx:
			return g_ngxContinuousSuccesses.load(std::memory_order_acquire);
		case HandbackSource::kFfx:
			return g_ffxContinuousSuccesses.load(std::memory_order_acquire);
		case HandbackSource::kNone:
		default:
			return 0;
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

	std::uint64_t ContinuousCopySerial() noexcept
	{
		return AcceptedContinuousHandbackSerial(HandbackSource::kNgx);
	}

	std::uint64_t PrivateReassemblySerial() noexcept
	{
		return g_privateReassemblySerial.load(std::memory_order_acquire);
	}

	ID3D11Texture2D* PrivateOutput() noexcept
	{
		return g_private.Get();
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
		state.sessionDisabled = g_sessionDisabled.load(std::memory_order_acquire);
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
		state.privateReassemblySerial = g_privateReassemblySerial.load(std::memory_order_relaxed);
		state.ffxTeardownAttempts = g_ffxTeardownAttempts.load(std::memory_order_relaxed);
		state.ffxTeardownSuccesses = g_ffxTeardownSuccesses.load(std::memory_order_relaxed);
		state.ffxTeardownFailures = g_ffxTeardownFailures.load(std::memory_order_relaxed);
		state.targetSuccesses = kTargetSuccesses;
		state.threadId = g_threadId.load(std::memory_order_relaxed);
		state.featureLevel = g_featureLevel.load(std::memory_order_relaxed);
		state.width = g_width.load(std::memory_order_relaxed);
		state.height = g_height.load(std::memory_order_relaxed);
		state.format = g_format.load(std::memory_order_relaxed);
		state.sampleCount = g_sampleCount.load(std::memory_order_relaxed);
		state.lastExceptionCode = g_lastExceptionCode.load(std::memory_order_relaxed);
		state.lastError = g_lastError.load(std::memory_order_relaxed);
		state.deviceRemovedReason = g_deviceRemovedReason.load(std::memory_order_relaxed);
		state.device = g_deviceAddress.load(std::memory_order_relaxed);
		state.context = g_contextAddress.load(std::memory_order_relaxed);
		state.sourceTexture = g_sourceTexture.load(std::memory_order_relaxed);
		state.privateTexture = g_privateTexture.load(std::memory_order_relaxed);
		return state;
	}
}
