#include "Upscaler/Evaluate.h"

#include "Upscaler/Intermediates.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/StereoLayout.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
			bool hdr = false;
			bool invertedDepth = false;
			bool jitteredMV = false;
			bool lowResMV = false;
			bool autoExposure = false;
			std::uint32_t preExposureBits = 0;
			std::uint32_t exposureScaleBits = 0;
			char preset[260] = {};
		};

		Backend::IBackend* g_backend = nullptr;    // borrowed singleton
		ID3D11Device* g_device = nullptr;          // borrowed, render-thread only
		ID3D11DeviceContext* g_context = nullptr;  // borrowed, render-thread only
		ConfigKey g_key{};
		bool g_hasKey = false;

		std::atomic<bool> g_prepared{ false };
		std::atomic<bool> g_active{ false };
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_successes{ 0 };
		std::atomic<std::uint64_t> g_failures{ 0 };
		std::atomic<std::uint64_t> g_leftEvaluates{ 0 };
		std::atomic<std::uint64_t> g_rightEvaluates{ 0 };
		std::atomic<std::uint32_t> g_prepareCount{ 0 };
		std::atomic<std::uint64_t> g_autoExposureSubmissions{ 0 };
		std::atomic<std::uint64_t> g_exposureContractFailures{ 0 };
		std::atomic<FailureStep> g_lastFailureStep{ FailureStep::kNone };
		std::atomic<std::uintptr_t> g_backendAddress{ 0 };
		std::atomic<std::uintptr_t> g_deviceAddress{ 0 };
		std::atomic<std::uintptr_t> g_contextAddress{ 0 };

		const std::filesystem::path g_emptyRuntimeDir;

		[[nodiscard]] bool BuildKey(const Backend::Config& a_config, ConfigKey& a_out) noexcept
		{
			if (a_config.preset.size() >= sizeof(a_out.preset)) {
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
			a_out.hdr = a_config.hdr;
			a_out.invertedDepth = a_config.invertedDepth;
			a_out.jitteredMV = a_config.jitteredMV;
			a_out.lowResMV = a_config.lowResMV;
			a_out.autoExposure = a_config.autoExposure;
			a_out.preExposureBits = std::bit_cast<std::uint32_t>(a_config.preExposure);
			a_out.exposureScaleBits = std::bit_cast<std::uint32_t>(a_config.exposureScale);
			if (!a_config.preset.empty()) {
				std::memcpy(a_out.preset, a_config.preset.data(), a_config.preset.size());
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
			       a_lhs.hdr == a_rhs.hdr &&
			       a_lhs.invertedDepth == a_rhs.invertedDepth &&
			       a_lhs.jitteredMV == a_rhs.jitteredMV &&
			       a_lhs.lowResMV == a_rhs.lowResMV &&
			       a_lhs.autoExposure == a_rhs.autoExposure &&
			       a_lhs.preExposureBits == a_rhs.preExposureBits &&
			       a_lhs.exposureScaleBits == a_rhs.exposureScaleBits &&
			       std::memcmp(a_lhs.preset, a_rhs.preset, a_lhs.presetLength) == 0;
		}

		[[nodiscard]] bool Decline(FailureStep a_step) noexcept
		{
			g_active.store(false, std::memory_order_release);
			g_lastFailureStep.store(a_step, std::memory_order_relaxed);
			g_failures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		[[nodiscard]] FailureStep Prepare(
			Backend::IBackend* a_backend,
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_immediate,
			const D3D11_TEXTURE2D_DESC& a_colorDesc,
			const D3D11_TEXTURE2D_DESC& a_motionDesc,
			const Backend::Config& a_config,
			const ConfigKey& a_key) noexcept
		{
			const bool same = g_prepared.load(std::memory_order_acquire) &&
			                  g_backend == a_backend && g_device == a_device && g_context == a_immediate &&
			                  g_hasKey && SameKey(g_key, a_key);
			if (same) {
				return Intermediates::EnsureFor(a_device, a_colorDesc, a_motionDesc, a_config) ? FailureStep::kNone : FailureStep::kIntermediates;
			}

			if (g_backend) {
				g_backend->DestroyViews();
				g_backend->Shutdown();
			}
			g_prepared.store(false, std::memory_order_release);
			Intermediates::Release();

			if (!Intermediates::EnsureFor(a_device, a_colorDesc, a_motionDesc, a_config)) {
				return FailureStep::kIntermediates;
			}
			if (!a_backend->Initialize(a_device, a_immediate, g_emptyRuntimeDir)) {
				return FailureStep::kBackendInitialize;
			}
			if (!a_backend->CreateViews(a_config, 2)) {
				a_backend->Shutdown();
				return FailureStep::kBackendCreateViews;
			}

			g_backend = a_backend;
			g_device = a_device;
			g_context = a_immediate;
			g_key = a_key;
			g_hasKey = true;
			g_backendAddress.store(reinterpret_cast<std::uintptr_t>(a_backend), std::memory_order_relaxed);
			g_deviceAddress.store(reinterpret_cast<std::uintptr_t>(a_device), std::memory_order_relaxed);
			g_contextAddress.store(reinterpret_cast<std::uintptr_t>(a_immediate), std::memory_order_relaxed);
			g_prepareCount.fetch_add(1, std::memory_order_relaxed);
			g_prepared.store(true, std::memory_order_release);
			return FailureStep::kNone;
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
		const Backend::Config& a_config) noexcept
	{
		g_attempts.fetch_add(1, std::memory_order_relaxed);
		g_active.store(true, std::memory_order_release);
		if (!a_backend || !a_device || !a_immediate || !a_doubleWideColor || !a_doubleWideDepth ||
			!a_doubleWideMotionVectors || !a_doubleWideOutput ||
			a_doubleWideColor == a_doubleWideDepth ||
			a_doubleWideColor == a_doubleWideMotionVectors ||
			a_doubleWideColor == a_doubleWideOutput ||
			a_doubleWideDepth == a_doubleWideMotionVectors ||
			a_doubleWideDepth == a_doubleWideOutput ||
			a_doubleWideMotionVectors == a_doubleWideOutput) {
			return Decline(FailureStep::kInvalidArguments);
		}
		if (a_backend->Name() != "null"sv) {
			return Decline(FailureStep::kUnsupportedBackend);
		}
		if (a_config.quality != Backend::QualityMode::kNativeAA ||
			a_config.renderWidth != a_config.outputWidth ||
			a_config.renderHeight != a_config.outputHeight ||
			a_config.renderWidth != a_colorDesc.Width / 2u ||
			a_config.renderHeight != a_colorDesc.Height ||
			a_config.invertedDepth || a_config.jitteredMV || !a_config.lowResMV) {
			return Decline(FailureStep::kUnsupportedConfig);
		}
		if (!Backend::IsBackendAutoExposureContract(a_config, nullptr)) {
			g_exposureContractFailures.fetch_add(1, std::memory_order_relaxed);
			return Decline(FailureStep::kExposureContract);
		}

		ConfigKey key{};
		if (!BuildKey(a_config, key)) {
			return Decline(FailureStep::kUnsupportedConfig);
		}

		Backend::Rect rects[2]{};
		Intermediates::DepthReference depths[2]{};
		for (std::uint32_t index = 0; index < 2; ++index) {
			const auto eye = index == 0 ? StereoLayout::Eye::kLeft : StereoLayout::Eye::kRight;
			rects[index] = StereoLayout::EyeRect(eye, a_colorDesc.Width, a_colorDesc.Height);
			depths[index] = Intermediates::ReferenceDepth(a_doubleWideDepth, a_depthDesc, rects[index]);
			if (!depths[index].resource) {
				return Decline(index == 0 ? FailureStep::kReferenceDepthLeft : FailureStep::kReferenceDepthRight);
			}
		}

		float jitterX = 0.0f;
		float jitterY = 0.0f;
		if (!Jitter::ReadEngine(a_config.renderWidth, a_config.renderHeight, jitterX, jitterY)) {
			return Decline(FailureStep::kReadJitter);
		}

		const FailureStep prepare = Prepare(a_backend, a_device, a_immediate, a_colorDesc, a_motionDesc, a_config, key);
		if (prepare != FailureStep::kNone) {
			return Decline(prepare);
		}

		for (std::uint32_t index = 0; index < 2; ++index) {
			const auto eye = index == 0 ? StereoLayout::Eye::kLeft : StereoLayout::Eye::kRight;
			const Backend::Rect rect = rects[index];
			const Intermediates::Staged staged = Intermediates::Get(eye);
			if (rect.w == 0 || rect.h == 0 || !staged.color || !staged.motionVectors || !staged.output) {
				return Decline(FailureStep::kStereoGeometry);
			}
			if (!Intermediates::ExtractColor(a_immediate, a_doubleWideColor, rect, staged.color)) {
				return Decline(index == 0 ? FailureStep::kExtractLeft : FailureStep::kExtractRight);
			}
			if (!Intermediates::ExtractMotion(a_immediate, a_doubleWideMotionVectors, rect, staged.motionVectors)) {
				return Decline(index == 0 ? FailureStep::kExtractMotionLeft : FailureStep::kExtractMotionRight);
			}

			Backend::EyeView view{};
			view.index = index;
			view.color = staged.color;
			view.depth = depths[index].resource;
			view.motionVectors = staged.motionVectors;
			view.output = staged.output;
			view.exposure = nullptr;
			view.colorRect = { 0, 0, a_config.renderWidth, a_config.renderHeight };
			view.depthRect = depths[index].rect;
			view.mvRect = view.colorRect;
			view.outputRect = { 0, 0, a_config.outputWidth, a_config.outputHeight };
			const auto mvScale = Backend::NormalizedUvMotionScale(a_config.renderWidth, a_config.renderHeight);
			view.mvScaleX = mvScale.x;
			view.mvScaleY = mvScale.y;
			view.jitterX = jitterX;
			view.jitterY = jitterY;
			Jitter::RecordSubmission(index, view.jitterX, view.jitterY);
			if (!Backend::IsBackendAutoExposureContract(a_config, view.exposure)) {
				g_exposureContractFailures.fetch_add(1, std::memory_order_relaxed);
				return Decline(FailureStep::kExposureContract);
			}
			g_autoExposureSubmissions.fetch_add(1, std::memory_order_relaxed);
			if (!a_backend->Evaluate(a_immediate, view)) {
				return Decline(index == 0 ? FailureStep::kEvaluateLeft : FailureStep::kEvaluateRight);
			}
			if (index == 0) {
				g_leftEvaluates.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_rightEvaluates.fetch_add(1, std::memory_order_relaxed);
			}

			if (!Intermediates::ReassembleOutput(a_immediate, staged.output, a_doubleWideOutput, rect)) {
				return Decline(index == 0 ? FailureStep::kReassembleLeft : FailureStep::kReassembleRight);
			}
		}

		g_successes.fetch_add(1, std::memory_order_relaxed);
		g_lastFailureStep.store(FailureStep::kNone, std::memory_order_relaxed);
		g_active.store(false, std::memory_order_release);
		return true;
	}

	State Snapshot() noexcept
	{
		return {
			g_prepared.load(std::memory_order_acquire),
			g_active.load(std::memory_order_acquire),
			g_attempts.load(std::memory_order_relaxed),
			g_successes.load(std::memory_order_relaxed),
			g_failures.load(std::memory_order_relaxed),
			g_leftEvaluates.load(std::memory_order_relaxed),
			g_rightEvaluates.load(std::memory_order_relaxed),
			g_prepareCount.load(std::memory_order_relaxed),
			g_autoExposureSubmissions.load(std::memory_order_relaxed),
			g_exposureContractFailures.load(std::memory_order_relaxed),
			g_lastFailureStep.load(std::memory_order_relaxed),
			g_backendAddress.load(std::memory_order_relaxed),
			g_deviceAddress.load(std::memory_order_relaxed),
			g_contextAddress.load(std::memory_order_relaxed),
		};
	}

	bool Active() noexcept
	{
		return g_active.load(std::memory_order_acquire);
	}

	void Reset() noexcept
	{
		g_active.store(false, std::memory_order_release);
		g_prepared.store(false, std::memory_order_release);
		if (g_backend) {
			g_backend->DestroyViews();
			g_backend->Shutdown();
		}
		Intermediates::Release();
		g_backend = nullptr;
		g_device = nullptr;
		g_context = nullptr;
		g_key = {};
		g_hasKey = false;
		g_backendAddress.store(0, std::memory_order_relaxed);
		g_deviceAddress.store(0, std::memory_order_relaxed);
		g_contextAddress.store(0, std::memory_order_relaxed);
	}
}
