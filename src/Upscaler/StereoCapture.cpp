#include "Upscaler/StereoCapture.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Intermediates.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/RenderTargets.h"
#include "Upscaler/TAAReplacement.h"

#include <ShlObj.h>
#include <Windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Upscaler::StereoCapture
{
	namespace
	{
		struct Capture
		{
			Status status{};
			std::vector<std::uint8_t> left;
			std::vector<std::uint8_t> right;
			std::vector<std::uint8_t> leftInputColor;
			std::vector<std::uint8_t> rightInputColor;
			std::vector<std::uint8_t> leftNgxOutput;
			std::vector<std::uint8_t> rightNgxOutput;
			std::vector<std::uint8_t> leftMotion;
			std::vector<std::uint8_t> rightMotion;
			std::vector<std::uint8_t> leftDepth;
			std::vector<std::uint8_t> rightDepth;
			std::vector<std::uint8_t> leftReassembled;
			std::vector<std::uint8_t> rightReassembled;
			std::vector<std::uint8_t> leftPostTonemap;
			std::vector<std::uint8_t> rightPostTonemap;
			std::vector<std::uint8_t> leftPreTonemap;
			std::vector<std::uint8_t> rightPreTonemap;
			std::vector<std::uint8_t> leftHandbackPrivate;
			std::vector<std::uint8_t> rightHandbackPrivate;
			std::vector<std::uint8_t> leftHandbackEngine;
			std::vector<std::uint8_t> rightHandbackEngine;
			bool saveAttempted = false;
			bool requireBypass = false;
			bool includeInputs = false;
			bool waitForInputs = false;
			bool includeReassembled = false;
			bool requireHandbackSeam = false;
			bool requireFfxHandback = false;
			bool awaitingQualityCommit = false;
			std::uint64_t requiredPrivateReassemblySerial = 0;
			std::uint64_t consumedFfxHandbackSerial = 0;
			std::uint32_t remainingBypasses = 0;
			std::uint32_t remainingHandbacks = 0;
			std::uint32_t requiredJitterPhase = kAnyJitterPhase;
		};

		std::atomic<std::uint64_t> g_requestedSequence{ 0 };
		std::atomic<std::uint64_t> g_completedSequence{ 0 };
		std::mutex g_captureLock;
		Capture g_capture{};

		void ResetQualityCaptureAttempt(Capture& a_capture, bool a_countAbort) noexcept
		{
			const auto requestedSequence = a_capture.status.requestedSequence;
			const auto inputsRequested = a_capture.status.inputsRequested;
			const auto reassembledRequested = a_capture.status.reassembledRequested;
			const auto handbackSeamRequested = a_capture.status.handbackSeamRequested;
			const auto requestedPrivateReassemblySerial = a_capture.status.requestedPrivateReassemblySerial;
			const auto requestedSkipBypasses = a_capture.status.requestedSkipBypasses;
			const auto skippedBypasses = a_capture.status.skippedBypasses;
			const auto requestedJitterPhase = a_capture.status.requestedJitterPhase;
			const auto skippedJitterPhases = a_capture.status.skippedJitterPhases;
			const auto abortCount = a_capture.status.handbackTransactionAbortCount + (a_countAbort ? 1u : 0u);

			a_capture.status = {};
			a_capture.status.requestedSequence = requestedSequence;
			a_capture.status.inputsRequested = inputsRequested;
			a_capture.status.reassembledRequested = reassembledRequested;
			a_capture.status.handbackSeamRequested = handbackSeamRequested;
			a_capture.status.requestedPrivateReassemblySerial = requestedPrivateReassemblySerial;
			a_capture.status.requestedSkipBypasses = requestedSkipBypasses;
			a_capture.status.skippedBypasses = skippedBypasses;
			a_capture.status.requestedJitterPhase = requestedJitterPhase;
			a_capture.status.skippedJitterPhases = skippedJitterPhases;
			a_capture.status.handbackTransactionAbortCount = abortCount;
			a_capture.saveAttempted = false;
			a_capture.awaitingQualityCommit = false;
		}

		[[nodiscard]] EyeStats Analyse(
			const std::vector<std::uint8_t>& a_rgba,
			std::uint32_t a_width,
			std::uint32_t a_height) noexcept
		{
			EyeStats out{};
			const auto pixels = static_cast<std::uint64_t>(a_width) * a_height;
			if (pixels == 0 || a_rgba.size() != pixels * 4) {
				return out;
			}

			std::uint64_t dark = 0;
			double luma = 0.0;
			for (std::uint64_t i = 0; i < pixels; ++i) {
				const auto* const p = a_rgba.data() + i * 4;
				const double y = (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0;
				luma += y;
				if (p[0] <= 2 && p[1] <= 2 && p[2] <= 2) {
					++dark;
				}
			}
			out.darkPct = 100.0 * static_cast<double>(dark) / static_cast<double>(pixels);
			out.meanLuma = luma / static_cast<double>(pixels);
			return out;
		}

		[[nodiscard]] bool ReadTight(
			ID3D11Texture2D* a_source,
			DXGI_FORMAT a_format,
			std::uint32_t a_width,
			std::uint32_t a_height,
			std::vector<std::uint8_t>& a_out,
			Status& a_status,
			std::string_view a_label)
		{
			auto* const device = Engine::Device();
			auto* const context = Engine::ImmediateContext();
			if (!a_source || !device || !context || a_width == 0 || a_height == 0) {
				a_status.error = fmt::format("{} texture, device, context or dimensions are unavailable", a_label);
				return false;
			}

			D3D11_TEXTURE2D_DESC desc{};
			a_source->GetDesc(&desc);
			if (desc.Width != a_width || desc.Height != a_height || desc.Format != a_format ||
				desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
				a_status.error = fmt::format("{} texture did not match its exact one-subresource capture contract", a_label);
				return false;
			}

			constexpr std::size_t kBytesPerPixel = 4;
			const auto rowBytes = static_cast<std::size_t>(a_width) * kBytesPerPixel;
			if (rowBytes > (std::numeric_limits<std::size_t>::max)() / a_height) {
				a_status.error = fmt::format("{} capture dimensions overflowed the CPU byte count", a_label);
				return false;
			}
			a_out.resize(rowBytes * a_height);

			D3D11_TEXTURE2D_DESC stagingDesc = desc;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.BindFlags = 0;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			stagingDesc.MiscFlags = 0;
			ID3D11Texture2D* staging = nullptr;
			const auto createHr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
			if (FAILED(createHr) || !staging) {
				a_status.d3dHr = static_cast<std::uint32_t>(createHr);
				a_status.error = fmt::format("CreateTexture2D for {} capture failed with hr=0x{:08X}", a_label, static_cast<std::uint32_t>(createHr));
				return false;
			}

			context->CopyResource(staging, a_source);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const auto mapHr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
			a_status.d3dHr = static_cast<std::uint32_t>(mapHr);
			if (FAILED(mapHr) || !mapped.pData) {
				SafeRelease(staging);
				a_status.error = fmt::format("Map for {} capture failed with hr=0x{:08X}", a_label, static_cast<std::uint32_t>(mapHr));
				return false;
			}

			const auto* const source = static_cast<const std::uint8_t*>(mapped.pData);
			for (std::uint32_t y = 0; y < a_height; ++y) {
				std::memcpy(
					a_out.data() + static_cast<std::size_t>(y) * rowBytes,
					source + static_cast<std::size_t>(y) * mapped.RowPitch,
					rowBytes);
			}
			context->Unmap(staging, 0);
			SafeRelease(staging);
			return true;
		}

		[[nodiscard]] float DecodeUnsignedFloat(std::uint32_t a_bits, std::uint32_t a_mantissaBits) noexcept
		{
			const auto mantissaMask = (1u << a_mantissaBits) - 1u;
			const auto mantissa = a_bits & mantissaMask;
			const auto exponent = (a_bits >> a_mantissaBits) & 0x1Fu;
			if (exponent == 0) {
				return std::ldexp(static_cast<float>(mantissa), 1 - 15 - static_cast<int>(a_mantissaBits));
			}
			if (exponent == 0x1Fu) {
				return mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
			}
			const auto fraction = static_cast<float>(mantissa) / static_cast<float>(1u << a_mantissaBits);
			return std::ldexp(1.0f + fraction, static_cast<int>(exponent) - 15);
		}

		[[nodiscard]] std::uint8_t UnitByte(double a_value) noexcept
		{
			const auto clamped = std::clamp(a_value, 0.0, 1.0);
			return static_cast<std::uint8_t>(std::lround(clamped * 255.0));
		}

		[[nodiscard]] std::uint8_t ToneByte(float a_value) noexcept
		{
			if (!std::isfinite(a_value)) {
				return 255;
			}
			const auto positive = (std::max)(0.0, static_cast<double>(a_value));
			const auto reinhard = positive / (1.0 + positive);
			return UnitByte(std::pow(reinhard, 1.0 / 2.2));
		}

		void VisualiseR11G11B10(
			const std::vector<std::uint8_t>& a_raw,
			std::vector<std::uint8_t>& a_rgba)
		{
			a_rgba.resize(a_raw.size());
			for (std::size_t i = 0; i + 3 < a_raw.size(); i += 4) {
				std::uint32_t packed = 0;
				std::memcpy(&packed, a_raw.data() + i, sizeof(packed));
				const auto r = DecodeUnsignedFloat(packed & 0x7FFu, 6);
				const auto g = DecodeUnsignedFloat((packed >> 11u) & 0x7FFu, 6);
				const auto b = DecodeUnsignedFloat((packed >> 22u) & 0x3FFu, 5);
				a_rgba[i + 0] = ToneByte(r);
				a_rgba[i + 1] = ToneByte(g);
				a_rgba[i + 2] = ToneByte(b);
				a_rgba[i + 3] = 255;
			}
		}

		[[nodiscard]] bool SplitVisualisedStereo(
			const std::vector<std::uint8_t>& a_raw,
			std::uint32_t a_width,
			std::uint32_t a_height,
			std::vector<std::uint8_t>& a_left,
			std::vector<std::uint8_t>& a_right,
			Status& a_status,
			std::string_view a_label)
		{
			if (a_width == 0 || (a_width & 1u) != 0 || a_height == 0 ||
				a_raw.size() != static_cast<std::size_t>(a_width) * a_height * 4u) {
				a_status.error = fmt::format("{} had an invalid even-width R11G11B10 byte contract", a_label);
				return false;
			}

			std::vector<std::uint8_t> wide;
			VisualiseR11G11B10(a_raw, wide);
			const auto eyeWidth = a_width / 2u;
			const auto eyeRowBytes = static_cast<std::size_t>(eyeWidth) * 4u;
			const auto wideRowBytes = eyeRowBytes * 2u;
			const auto eyeBytes = eyeRowBytes * a_height;
			if (wide.size() != wideRowBytes * a_height) {
				a_status.error = fmt::format("{} visualization had an unexpected byte count", a_label);
				return false;
			}
			a_left.resize(eyeBytes);
			a_right.resize(eyeBytes);
			for (std::uint32_t y = 0; y < a_height; ++y) {
				const auto* const row = wide.data() + static_cast<std::size_t>(y) * wideRowBytes;
				std::memcpy(a_left.data() + static_cast<std::size_t>(y) * eyeRowBytes, row, eyeRowBytes);
				std::memcpy(a_right.data() + static_cast<std::size_t>(y) * eyeRowBytes, row + eyeRowBytes, eyeRowBytes);
			}
			return true;
		}

		[[nodiscard]] float HalfToFloat(std::uint16_t a_half) noexcept
		{
			const auto sign = (static_cast<std::uint32_t>(a_half) & 0x8000u) << 16u;
			const auto exponent = (static_cast<std::uint32_t>(a_half) >> 10u) & 0x1Fu;
			auto mantissa = static_cast<std::uint32_t>(a_half) & 0x3FFu;
			std::uint32_t bits = 0;
			if (exponent == 0) {
				if (mantissa == 0) {
					bits = sign;
				} else {
					int unbiasedExponent = -14;
					while ((mantissa & 0x400u) == 0) {
						mantissa <<= 1u;
						--unbiasedExponent;
					}
					mantissa &= 0x3FFu;
					bits = sign | (static_cast<std::uint32_t>(unbiasedExponent + 127) << 23u) | (mantissa << 13u);
				}
			} else if (exponent == 0x1Fu) {
				bits = sign | 0x7F800000u | (mantissa << 13u);
			} else {
				bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
			}
			return std::bit_cast<float>(bits);
		}

		[[nodiscard]] std::uint8_t SignedMotionByte(double a_pixels) noexcept
		{
			const auto encoded = 0.5 + std::clamp(a_pixels / 16.0, -0.5, 0.5);
			return UnitByte(encoded);
		}

		MotionStats VisualiseMotion(
			const std::vector<std::uint8_t>& a_raw,
			std::uint32_t a_width,
			std::uint32_t a_height,
			std::vector<std::uint8_t>& a_rgba)
		{
			MotionStats stats{};
			const auto pixels = static_cast<std::uint64_t>(a_width) * a_height;
			if (a_raw.size() != pixels * 4) {
				return stats;
			}
			a_rgba.resize(a_raw.size());
			double minX = std::numeric_limits<double>::infinity();
			double maxX = -std::numeric_limits<double>::infinity();
			double minY = std::numeric_limits<double>::infinity();
			double maxY = -std::numeric_limits<double>::infinity();
			double sumAbsX = 0.0;
			double sumAbsY = 0.0;
			std::uint64_t finite = 0;
			std::uint64_t zero = 0;
			std::uint64_t nonFinite = 0;
			for (std::uint64_t pixel = 0; pixel < pixels; ++pixel) {
				const auto i = static_cast<std::size_t>(pixel) * 4;
				std::uint16_t halfX = 0;
				std::uint16_t halfY = 0;
				std::memcpy(&halfX, a_raw.data() + i, sizeof(halfX));
				std::memcpy(&halfY, a_raw.data() + i + 2, sizeof(halfY));
				const auto x = static_cast<double>(HalfToFloat(halfX)) * a_width;
				const auto y = static_cast<double>(HalfToFloat(halfY)) * a_height;
				if (!std::isfinite(x) || !std::isfinite(y)) {
					++nonFinite;
					a_rgba[i + 0] = 255;
					a_rgba[i + 1] = 0;
					a_rgba[i + 2] = 255;
					a_rgba[i + 3] = 255;
					continue;
				}
				++finite;
				if (std::abs(x) <= 1.0e-6 && std::abs(y) <= 1.0e-6) {
					++zero;
				}
				minX = (std::min)(minX, x);
				maxX = (std::max)(maxX, x);
				minY = (std::min)(minY, y);
				maxY = (std::max)(maxY, y);
				sumAbsX += std::abs(x);
				sumAbsY += std::abs(y);
				a_rgba[i + 0] = SignedMotionByte(x);
				a_rgba[i + 1] = SignedMotionByte(y);
				a_rgba[i + 2] = UnitByte((std::min)(std::hypot(x, y) / 16.0, 1.0));
				a_rgba[i + 3] = 255;
			}
			stats.zeroPct = pixels == 0 ? 0.0 : 100.0 * static_cast<double>(zero) / static_cast<double>(pixels);
			stats.nonFinitePct = pixels == 0 ? 0.0 : 100.0 * static_cast<double>(nonFinite) / static_cast<double>(pixels);
			if (finite != 0) {
				stats.minX = minX;
				stats.maxX = maxX;
				stats.minY = minY;
				stats.maxY = maxY;
				stats.meanAbsX = sumAbsX / static_cast<double>(finite);
				stats.meanAbsY = sumAbsY / static_cast<double>(finite);
			}
			return stats;
		}

		void VisualiseDepth(
			const std::vector<std::uint8_t>& a_raw,
			std::uint32_t a_fullEyeWidth,
			std::uint32_t a_fullHeight,
			std::uint32_t a_regionWidth,
			std::uint32_t a_regionHeight,
			std::vector<std::uint8_t>& a_left,
			std::vector<std::uint8_t>& a_right,
			DepthStats& a_leftStats,
			DepthStats& a_rightStats)
		{
			constexpr std::uint32_t kDepthMask = 0x00FFFFFFu;
			constexpr double kDepthMaximum = static_cast<double>(kDepthMask);
			const auto fullEyePixels = static_cast<std::uint64_t>(a_fullEyeWidth) * a_fullHeight;
			const auto regionPixels = static_cast<std::uint64_t>(a_regionWidth) * a_regionHeight;
			if (a_regionWidth == 0 || a_regionHeight == 0 ||
				a_regionWidth > a_fullEyeWidth || a_regionHeight > a_fullHeight ||
				a_raw.size() != fullEyePixels * 8) {
				return;
			}
			a_left.resize(static_cast<std::size_t>(regionPixels) * 4);
			a_right.resize(static_cast<std::size_t>(regionPixels) * 4);
			std::uint64_t leftFar = 0;
			std::uint64_t rightFar = 0;
			double leftMin = 1.0;
			double leftMax = 0.0;
			double rightMin = 1.0;
			double rightMax = 0.0;
			const auto doubleWideRowBytes = static_cast<std::size_t>(a_fullEyeWidth) * 8;
			for (std::uint32_t y = 0; y < a_regionHeight; ++y) {
				for (std::uint32_t x = 0; x < a_regionWidth; ++x) {
					const auto destination = (static_cast<std::size_t>(y) * a_regionWidth + x) * 4;
					for (std::uint32_t eye = 0; eye < 2; ++eye) {
						// Match the vendor input rectangle. Dynamic-resolution stereo is
						// packed at x=0 and x=regionWidth inside the full allocation.
						const auto source = static_cast<std::size_t>(y) * doubleWideRowBytes +
						                    (static_cast<std::size_t>(eye) * a_regionWidth + x) * 4;
						std::uint32_t packed = 0;
						std::memcpy(&packed, a_raw.data() + source, sizeof(packed));
						const auto rawDepth = packed & kDepthMask;
						const auto depth = static_cast<double>(rawDepth) / kDepthMaximum;
						const auto distanceLike = -std::log2((std::max)(1.0 - depth, 1.0 / 16777216.0)) / 24.0;
						const auto gray = UnitByte(distanceLike);
						auto& image = eye == 0 ? a_left : a_right;
						image[destination + 0] = gray;
						image[destination + 1] = gray;
						image[destination + 2] = gray;
						image[destination + 3] = 255;
						if (eye == 0) {
							leftFar += rawDepth == kDepthMask ? 1u : 0u;
							leftMin = (std::min)(leftMin, depth);
							leftMax = (std::max)(leftMax, depth);
						} else {
							rightFar += rawDepth == kDepthMask ? 1u : 0u;
							rightMin = (std::min)(rightMin, depth);
							rightMax = (std::max)(rightMax, depth);
						}
					}
				}
			}
			a_leftStats.farPct = 100.0 * static_cast<double>(leftFar) / static_cast<double>(regionPixels);
			a_leftStats.minDepth = leftMin;
			a_leftStats.maxDepth = leftMax;
			a_rightStats.farPct = 100.0 * static_cast<double>(rightFar) / static_cast<double>(regionPixels);
			a_rightStats.minDepth = rightMin;
			a_rightStats.maxDepth = rightMax;
		}

		[[nodiscard]] bool Readback(
			Capture& a_out,
			ID3D11Texture2D* a_submittedOverride = nullptr)
		{
			const auto submitted = RenderTargets::Lookup(Addr::kRT_Submitted);
			auto* const device = Engine::Device();
			auto* const context = Engine::ImmediateContext();
			auto* const captureSource = a_submittedOverride ? a_submittedOverride : submitted.tex;
			if (!submitted.valid || !submitted.tex || !captureSource || !device || !context) {
				a_out.status.error = "submitted target 4, device or immediate context is unavailable";
				return false;
			}

			D3D11_TEXTURE2D_DESC desc{};
			D3D11_TEXTURE2D_DESC submittedDesc{};
			captureSource->GetDesc(&desc);
			submitted.tex->GetDesc(&submittedDesc);
			if (desc.Width == 0 || (desc.Width & 1u) != 0 || desc.Height == 0 ||
				desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.MipLevels != 1 ||
				desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
				desc.Width != submitted.width || desc.Height != submitted.height ||
				desc.Width != submittedDesc.Width || desc.Height != submittedDesc.Height ||
				desc.Format != submittedDesc.Format) {
				a_out.status.error = "submitted target 4 did not match the measured even-width R8G8B8A8_UNORM stereo contract";
				return false;
			}

			D3D11_TEXTURE2D_DESC stagingDesc = desc;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.BindFlags = 0;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			stagingDesc.MiscFlags = 0;

			const auto eyeWidth = desc.Width / 2;
			const auto eyeRowBytes = static_cast<std::size_t>(eyeWidth) * 4;
			const auto eyeBytes = eyeRowBytes * desc.Height;
			a_out.left.resize(eyeBytes);
			a_out.right.resize(eyeBytes);

			ID3D11Texture2D* staging = nullptr;
			const auto createHr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
			if (FAILED(createHr) || !staging) {
				a_out.status.d3dHr = static_cast<std::uint32_t>(createHr);
				a_out.status.error = fmt::format("CreateTexture2D for stereo capture failed with hr=0x{:08X}", static_cast<std::uint32_t>(createHr));
				return false;
			}

			context->CopyResource(staging, captureSource);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const auto mapHr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
			a_out.status.d3dHr = static_cast<std::uint32_t>(mapHr);
			if (FAILED(mapHr) || !mapped.pData) {
				SafeRelease(staging);
				a_out.status.error = fmt::format("Map for stereo capture failed with hr=0x{:08X}", static_cast<std::uint32_t>(mapHr));
				return false;
			}

			const auto* const source = static_cast<const std::uint8_t*>(mapped.pData);
			for (std::uint32_t y = 0; y < desc.Height; ++y) {
				const auto* const row = source + static_cast<std::size_t>(y) * mapped.RowPitch;
				std::memcpy(a_out.left.data() + static_cast<std::size_t>(y) * eyeRowBytes, row, eyeRowBytes);
				std::memcpy(a_out.right.data() + static_cast<std::size_t>(y) * eyeRowBytes, row + eyeRowBytes, eyeRowBytes);
			}
			context->Unmap(staging, 0);
			SafeRelease(staging);

			a_out.status.eyeWidth = eyeWidth;
			a_out.status.eyeHeight = desc.Height;
			a_out.status.format = static_cast<std::uint32_t>(desc.Format);
			a_out.status.left = Analyse(a_out.left, eyeWidth, desc.Height);
			a_out.status.right = Analyse(a_out.right, eyeWidth, desc.Height);
			return true;
		}

		[[nodiscard]] bool ReadbackInputs(Capture& a_out)
		{
			const auto eyeWidth = a_out.status.eyeWidth;
			const auto eyeHeight = a_out.status.eyeHeight;
			const auto intermediates = Intermediates::Snapshot();
			const auto inputWidth = intermediates.renderWidth;
			const auto inputHeight = intermediates.renderHeight;
			const auto left = Intermediates::Get(StereoLayout::Eye::kLeft);
			const auto right = Intermediates::Get(StereoLayout::Eye::kRight);
			const auto depth = RenderTargets::LookupDepth(Addr::kDS_MainScene);
			if (!left.color || !left.motionVectors || !left.output ||
				!right.color || !right.motionVectors || !right.output ||
				inputWidth == 0 || inputHeight == 0 ||
				intermediates.outputWidth != eyeWidth || intermediates.outputHeight != eyeHeight ||
				!depth.valid || !depth.tex || depth.width != eyeWidth * 2u || depth.height != eyeHeight) {
				a_out.status.error = "the matching per-eye vendor intermediates or whole scene depth are unavailable";
				return false;
			}

			std::vector<std::uint8_t> raw;
			a_out.status.vendorInputWidth = inputWidth;
			a_out.status.vendorInputHeight = inputHeight;
			if (!ReadTight(left.color, DXGI_FORMAT_R11G11B10_FLOAT, inputWidth, inputHeight, raw, a_out.status, "left vendor input color"sv)) {
				return false;
			}
			VisualiseR11G11B10(raw, a_out.leftInputColor);
			raw.clear();
			if (!ReadTight(right.color, DXGI_FORMAT_R11G11B10_FLOAT, inputWidth, inputHeight, raw, a_out.status, "right vendor input color"sv)) {
				return false;
			}
			VisualiseR11G11B10(raw, a_out.rightInputColor);
			raw.clear();

			if (!ReadTight(left.output, DXGI_FORMAT_R11G11B10_FLOAT, eyeWidth, eyeHeight, raw, a_out.status, "left raw vendor output"sv)) {
				return false;
			}
			VisualiseR11G11B10(raw, a_out.leftNgxOutput);
			raw.clear();
			if (!ReadTight(right.output, DXGI_FORMAT_R11G11B10_FLOAT, eyeWidth, eyeHeight, raw, a_out.status, "right raw vendor output"sv)) {
				return false;
			}
			VisualiseR11G11B10(raw, a_out.rightNgxOutput);
			raw.clear();

			if (!ReadTight(left.motionVectors, DXGI_FORMAT_R16G16_FLOAT, inputWidth, inputHeight, raw, a_out.status, "left vendor motion"sv)) {
				return false;
			}
			a_out.status.leftMotion = VisualiseMotion(raw, inputWidth, inputHeight, a_out.leftMotion);
			raw.clear();
			if (!ReadTight(right.motionVectors, DXGI_FORMAT_R16G16_FLOAT, inputWidth, inputHeight, raw, a_out.status, "right vendor motion"sv)) {
				return false;
			}
			a_out.status.rightMotion = VisualiseMotion(raw, inputWidth, inputHeight, a_out.rightMotion);
			raw.clear();

			D3D11_TEXTURE2D_DESC depthDesc{};
			depth.tex->GetDesc(&depthDesc);
			if (depthDesc.Format != DXGI_FORMAT_R24G8_TYPELESS ||
				!ReadTight(depth.tex, DXGI_FORMAT_R24G8_TYPELESS, eyeWidth * 2u, eyeHeight, raw, a_out.status, "whole scene depth"sv)) {
				if (a_out.status.error.empty()) {
					a_out.status.error = "whole scene depth did not use the verified R24G8 typeless format";
				}
				return false;
			}
			VisualiseDepth(
				raw,
				eyeWidth,
				eyeHeight,
				inputWidth,
				inputHeight,
				a_out.leftDepth,
				a_out.rightDepth,
				a_out.status.leftDepth,
				a_out.status.rightDepth);

			const auto preTonemap = RenderTargets::Lookup(Addr::kRT_PreTonemapColor);
			if (!preTonemap.valid || !preTonemap.tex ||
				preTonemap.width != eyeWidth * 2u || preTonemap.height != eyeHeight) {
				a_out.status.error = "pre-tonemap logical target 1 is unavailable for matching-frame capture";
				return false;
			}
			raw.clear();
			if (!ReadTight(preTonemap.tex, DXGI_FORMAT_R11G11B10_FLOAT, eyeWidth * 2u, eyeHeight, raw, a_out.status, "pre-tonemap color"sv)) {
				return false;
			}
			std::vector<std::uint8_t> preWide;
			VisualiseR11G11B10(raw, preWide);
			const auto preEyeRowBytes = static_cast<std::size_t>(eyeWidth) * 4;
			const auto preWideRowBytes = preEyeRowBytes * 2;
			const auto preEyeBytes = preEyeRowBytes * eyeHeight;
			if (preWide.size() != preWideRowBytes * eyeHeight) {
				a_out.status.error = "the pre-tonemap visualization had an unexpected byte count";
				return false;
			}
			a_out.leftPreTonemap.resize(preEyeBytes);
			a_out.rightPreTonemap.resize(preEyeBytes);
			for (std::uint32_t y = 0; y < eyeHeight; ++y) {
				const auto* const row = preWide.data() + static_cast<std::size_t>(y) * preWideRowBytes;
				std::memcpy(a_out.leftPreTonemap.data() + static_cast<std::size_t>(y) * preEyeRowBytes, row, preEyeRowBytes);
				std::memcpy(a_out.rightPreTonemap.data() + static_cast<std::size_t>(y) * preEyeRowBytes, row + preEyeRowBytes, preEyeRowBytes);
			}
			a_out.status.leftPreTonemap = Analyse(a_out.leftPreTonemap, eyeWidth, eyeHeight);
			a_out.status.rightPreTonemap = Analyse(a_out.rightPreTonemap, eyeWidth, eyeHeight);

			const auto postTonemap = RenderTargets::Lookup(Addr::kRT_PostTonemapColor);
			if (!postTonemap.valid || !postTonemap.tex ||
				postTonemap.width != eyeWidth * 2u || postTonemap.height != eyeHeight) {
				a_out.status.error = "post-tonemap logical target 3 is unavailable for matching-frame capture";
				return false;
			}
			raw.clear();
			if (!ReadTight(postTonemap.tex, DXGI_FORMAT_R11G11B10_FLOAT, eyeWidth * 2u, eyeHeight, raw, a_out.status, "post-tonemap color"sv)) {
				return false;
			}
			std::vector<std::uint8_t> wide;
			VisualiseR11G11B10(raw, wide);
			const auto eyeRowBytes = static_cast<std::size_t>(eyeWidth) * 4;
			const auto wideRowBytes = eyeRowBytes * 2;
			const auto eyeBytes = eyeRowBytes * eyeHeight;
			if (wide.size() != wideRowBytes * eyeHeight) {
				a_out.status.error = "the post-tonemap visualization had an unexpected byte count";
				return false;
			}
			a_out.leftPostTonemap.resize(eyeBytes);
			a_out.rightPostTonemap.resize(eyeBytes);
			for (std::uint32_t y = 0; y < eyeHeight; ++y) {
				const auto* const row = wide.data() + static_cast<std::size_t>(y) * wideRowBytes;
				std::memcpy(a_out.leftPostTonemap.data() + static_cast<std::size_t>(y) * eyeRowBytes, row, eyeRowBytes);
				std::memcpy(a_out.rightPostTonemap.data() + static_cast<std::size_t>(y) * eyeRowBytes, row + eyeRowBytes, eyeRowBytes);
			}
			a_out.status.leftPostTonemap = Analyse(a_out.leftPostTonemap, eyeWidth, eyeHeight);
			a_out.status.rightPostTonemap = Analyse(a_out.rightPostTonemap, eyeWidth, eyeHeight);
			return true;
		}

		[[nodiscard]] bool ReadbackReassembled(Capture& a_out)
		{
			auto* const texture = HDRRoundTrip::PrivateOutput();
			const auto eyeWidth = a_out.status.eyeWidth;
			const auto eyeHeight = a_out.status.eyeHeight;
			if (!texture || eyeWidth == 0 || eyeHeight == 0) {
				a_out.status.error = "the accepted private double-wide output is unavailable";
				return false;
			}

			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			if (desc.Width != eyeWidth * 2u || desc.Height != eyeHeight ||
				desc.Format != DXGI_FORMAT_R11G11B10_FLOAT || desc.MipLevels != 1 ||
				desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
				a_out.status.error = "the private reassembly target did not match the verified double-wide R11G11B10_FLOAT contract";
				return false;
			}

			std::vector<std::uint8_t> raw;
			if (!ReadTight(texture, DXGI_FORMAT_R11G11B10_FLOAT, desc.Width, desc.Height, raw, a_out.status, "private reassembled vendor output"sv)) {
				return false;
			}
			std::vector<std::uint8_t> wide;
			VisualiseR11G11B10(raw, wide);
			const auto eyeRowBytes = static_cast<std::size_t>(eyeWidth) * 4;
			const auto wideRowBytes = eyeRowBytes * 2;
			const auto eyeBytes = eyeRowBytes * eyeHeight;
			if (wide.size() != wideRowBytes * eyeHeight) {
				a_out.status.error = "the private reassembly visualization had an unexpected byte count";
				return false;
			}
			a_out.leftReassembled.resize(eyeBytes);
			a_out.rightReassembled.resize(eyeBytes);
			for (std::uint32_t y = 0; y < eyeHeight; ++y) {
				const auto* const row = wide.data() + static_cast<std::size_t>(y) * wideRowBytes;
				std::memcpy(a_out.leftReassembled.data() + static_cast<std::size_t>(y) * eyeRowBytes, row, eyeRowBytes);
				std::memcpy(a_out.rightReassembled.data() + static_cast<std::size_t>(y) * eyeRowBytes, row + eyeRowBytes, eyeRowBytes);
			}
			a_out.status.leftReassembled = Analyse(a_out.leftReassembled, eyeWidth, eyeHeight);
			a_out.status.rightReassembled = Analyse(a_out.rightReassembled, eyeWidth, eyeHeight);
			a_out.status.privateReassemblySerial = HDRRoundTrip::PrivateReassemblySerial();
			return true;
		}

		[[nodiscard]] bool PrivateInputsAvailable() noexcept
		{
			const auto left = Intermediates::Get(StereoLayout::Eye::kLeft);
			const auto right = Intermediates::Get(StereoLayout::Eye::kRight);
			const auto depth = RenderTargets::LookupDepth(Addr::kDS_MainScene);
			return left.color && left.motionVectors && left.output &&
			       right.color && right.motionVectors && right.output &&
			       depth.valid && depth.tex;
		}

		void Put16(std::ofstream& a_out, std::uint16_t a_value)
		{
			a_out.write(reinterpret_cast<const char*>(&a_value), sizeof(a_value));
		}

		void Put32(std::ofstream& a_out, std::uint32_t a_value)
		{
			a_out.write(reinterpret_cast<const char*>(&a_value), sizeof(a_value));
		}

		[[nodiscard]] bool WriteBmp(
			const std::filesystem::path& a_path,
			const std::vector<std::uint8_t>& a_rgba,
			std::uint32_t a_width,
			std::uint32_t a_height,
			std::string& a_error)
		{
			const auto pixels = static_cast<std::uint64_t>(a_width) * a_height;
			if (a_width == 0 || a_height == 0 || a_rgba.size() != pixels * 4) {
				a_error = "captured eye storage does not match its dimensions";
				return false;
			}

			std::ofstream out(a_path, std::ios::binary);
			if (!out) {
				a_error = "could not open " + a_path.string();
				return false;
			}

			const auto rowBytes = (a_width * 3u + 3u) & ~3u;
			const auto imageBytes = rowBytes * a_height;
			constexpr std::uint32_t kHeaderBytes = 14u + 40u;
			out.write("BM", 2);
			Put32(out, kHeaderBytes + imageBytes);
			Put32(out, 0);
			Put32(out, kHeaderBytes);
			Put32(out, 40);
			Put32(out, a_width);
			Put32(out, a_height);
			Put16(out, 1);
			Put16(out, 24);
			Put32(out, 0);
			Put32(out, imageBytes);
			Put32(out, 2835);
			Put32(out, 2835);
			Put32(out, 0);
			Put32(out, 0);

			std::vector<std::uint8_t> row(rowBytes, 0);
			for (std::uint32_t y = 0; y < a_height; ++y) {
				const auto* const source = a_rgba.data() +
					static_cast<std::size_t>(a_height - 1u - y) * a_width * 4;
				for (std::uint32_t x = 0; x < a_width; ++x) {
					row[x * 3u + 0] = source[x * 4u + 2];
					row[x * 3u + 1] = source[x * 4u + 1];
					row[x * 3u + 2] = source[x * 4u + 0];
				}
				out.write(reinterpret_cast<const char*>(row.data()), rowBytes);
			}
			if (!out) {
				a_error = "writing " + a_path.string() + " failed";
				return false;
			}
			return true;
		}

		[[nodiscard]] std::string_view ModeName(const Status& a_status) noexcept
		{
			if (a_status.bypassed) {
				return "bypass";
			}
			if (a_status.overwritten) {
				return "overwrite";
			}
			return "native";
		}

		void CompleteExhaustedFfxRequest(
			Capture& a_capture,
			std::uint64_t a_sequence,
			std::uint64_t a_observedSerial) noexcept
		{
			a_capture.status.completedSequence = a_sequence;
			a_capture.status.ffxHandbackSerial = a_observedSerial;
			a_capture.status.ffxGateExhausted = true;
			try {
				a_capture.status.error =
					"captureFfx exhausted the bounded 512-handback FFX gate before its skip, jitter and input filters admitted a frame";
			} catch (...) {
			}
			g_completedSequence.store(a_sequence, std::memory_order_release);
		}

		[[nodiscard]] std::filesystem::path CaptureDirectory()
		{
			PWSTR raw = nullptr;
			if (::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw) != S_OK) {
				return {};
			}
			std::filesystem::path path{ raw };
			::CoTaskMemFree(raw);
			return path / L"My Games" / L"Fallout4VR" / L"F4SE" / L"Fo4VRUpscaler" / L"captures";
		}

		using LockedCaptureOperation = void (*)(void*) noexcept;

		struct CaptureLockOperation
		{
			LockedCaptureOperation operation = nullptr;
			void* context = nullptr;
			bool locked = false;
			bool returned = false;
			std::uint32_t exceptionCode = 0;
		};

		[[nodiscard]] bool TryAcquireCaptureLock() noexcept
		{
			try {
				return g_captureLock.try_lock();
			} catch (...) {
				return false;
			}
		}

		void ReleaseCaptureLock() noexcept
		{
			try {
				g_captureLock.unlock();
			} catch (...) {
			}
		}

		// Render callbacks must never wait for DevBench capture I/O, and a graphics
		// driver SEH must not strand this mutex. Destructible capture/readback state
		// stays behind the noexcept function pointer while this POD shell owns the
		// explicit finally-unlock boundary.
		[[nodiscard]] bool InvokeWithCaptureTryLock(
			LockedCaptureOperation a_operation,
			void* a_context,
			std::uint32_t* a_exceptionCode = nullptr) noexcept
		{
			CaptureLockOperation operation{};
			operation.operation = a_operation;
			operation.context = a_context;
			__try {
				__try {
					if (operation.operation && TryAcquireCaptureLock()) {
						operation.locked = true;
						operation.operation(operation.context);
						operation.returned = true;
					}
				} __finally {
					if (operation.locked) {
						ReleaseCaptureLock();
						operation.locked = false;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				operation.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (a_exceptionCode) {
				*a_exceptionCode = operation.exceptionCode;
			}
			return operation.returned;
		}
	}

	std::uint64_t Request(
		bool a_requireBypass,
		std::uint32_t a_skipBypasses,
		std::uint32_t a_jitterPhase,
		bool a_includeInputs,
		bool a_waitForInputs,
		bool a_includeReassembled,
		bool a_requireHandbackSeam) noexcept
	{
		const std::scoped_lock lock(g_captureLock);
		const auto sequence = g_requestedSequence.load(std::memory_order_relaxed) + 1;
		g_capture = {};
		g_capture.requireBypass = a_requireBypass;
		g_capture.includeInputs = a_includeInputs;
		g_capture.waitForInputs = a_waitForInputs;
		g_capture.includeReassembled = a_includeReassembled;
		g_capture.requireHandbackSeam = a_requireHandbackSeam;
		g_capture.requiredPrivateReassemblySerial =
			a_includeReassembled ? HDRRoundTrip::PrivateReassemblySerial() + 1 : 0;
		g_capture.remainingBypasses = a_skipBypasses;
		g_capture.requiredJitterPhase = a_jitterPhase < Jitter::NativePhaseCount() ? a_jitterPhase : kAnyJitterPhase;
		g_capture.status.requestedSequence = sequence;
		g_capture.status.requestedSkipBypasses = a_skipBypasses;
		g_capture.status.requestedJitterPhase = g_capture.requiredJitterPhase;
		g_capture.status.inputsRequested = a_includeInputs;
		g_capture.status.reassembledRequested = a_includeReassembled;
		g_capture.status.handbackSeamRequested = a_requireHandbackSeam;
		g_capture.status.requestedPrivateReassemblySerial = g_capture.requiredPrivateReassemblySerial;
		g_requestedSequence.store(sequence, std::memory_order_release);
		return sequence;
	}

	std::uint64_t RequestFfx(
		std::uint32_t a_skipHandbacks,
		std::uint32_t a_jitterPhase) noexcept
	{
		const std::scoped_lock lock(g_captureLock);
		const auto sequence = g_requestedSequence.load(std::memory_order_relaxed) + 1;
		const auto baseline = HDRRoundTrip::HandbackCandidateSerial(HDRRoundTrip::HandbackSource::kFfx);
		g_capture = {};
		g_capture.includeInputs = true;
		g_capture.waitForInputs = true;
		g_capture.requireFfxHandback = true;
		g_capture.consumedFfxHandbackSerial = baseline;
		g_capture.remainingHandbacks = a_skipHandbacks;
		g_capture.requiredJitterPhase = a_jitterPhase < Jitter::NativePhaseCount() ? a_jitterPhase : kAnyJitterPhase;
		g_capture.status.requestedSequence = sequence;
		g_capture.status.inputsRequested = true;
		g_capture.status.ffxHandbackRequested = true;
		g_capture.status.requestedFfxHandbackSerial = baseline + a_skipHandbacks + 1;
		g_capture.status.requestedSkipHandbacks = a_skipHandbacks;
		g_capture.status.requestedJitterPhase = g_capture.requiredJitterPhase;
		g_requestedSequence.store(sequence, std::memory_order_release);
		return sequence;
	}

	static void OnQualityHandbackLockedImpl(
		ID3D11Texture2D* a_engineSource,
		ID3D11Texture2D* a_privateOutput,
		std::uint64_t a_privateReassemblySerial,
		HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		const auto requested = g_requestedSequence.load(std::memory_order_acquire);
		if (requested == 0 || requested == g_completedSequence.load(std::memory_order_acquire) ||
			a_handback.source != HDRRoundTrip::HandbackSource::kNgx || a_handback.serial == 0) {
			return;
		}
		const auto current = g_requestedSequence.load(std::memory_order_acquire);
		if (current == g_completedSequence.load(std::memory_order_acquire) ||
			!g_capture.requireHandbackSeam ||
			a_privateReassemblySerial < g_capture.requiredPrivateReassemblySerial ||
			g_capture.awaitingQualityCommit) {
			return;
		}
		if (g_capture.requiredJitterPhase != kAnyJitterPhase) {
			std::uint32_t jitterPhase = kAnyJitterPhase;
			if (!Jitter::ReadCurrentPhase(jitterPhase) || jitterPhase != g_capture.requiredJitterPhase) {
				++g_capture.status.skippedJitterPhases;
				return;
			}
		}
		ResetQualityCaptureAttempt(g_capture, false);
		g_capture.status.handbackNgxSerial = a_handback.serial;

		try {
			D3D11_TEXTURE2D_DESC engineDesc{};
			D3D11_TEXTURE2D_DESC privateDesc{};
			if (!a_engineSource || !a_privateOutput) {
				g_capture.status.error = "the exact handback seam did not provide both resources";
				return;
			}
			a_engineSource->GetDesc(&engineDesc);
			a_privateOutput->GetDesc(&privateDesc);
			if (engineDesc.Width == 0 || (engineDesc.Width & 1u) != 0 || engineDesc.Height == 0 ||
				engineDesc.Width != privateDesc.Width || engineDesc.Height != privateDesc.Height ||
				engineDesc.Format != DXGI_FORMAT_R11G11B10_FLOAT || privateDesc.Format != engineDesc.Format ||
				engineDesc.MipLevels != 1 || privateDesc.MipLevels != 1 ||
				engineDesc.ArraySize != 1 || privateDesc.ArraySize != 1 ||
				engineDesc.SampleDesc.Count != 1 || privateDesc.SampleDesc.Count != 1) {
				g_capture.status.error = "the exact handback seam resources did not share the verified double-wide R11G11B10 contract";
				return;
			}

			std::vector<std::uint8_t> privateRaw;
			std::vector<std::uint8_t> engineRaw;
			g_capture.status.error.clear();
			if (!ReadTight(
					a_privateOutput,
					DXGI_FORMAT_R11G11B10_FLOAT,
					privateDesc.Width,
					privateDesc.Height,
					privateRaw,
					g_capture.status,
					"private output at handback seam"sv) ||
				!ReadTight(
					a_engineSource,
					DXGI_FORMAT_R11G11B10_FLOAT,
					engineDesc.Width,
					engineDesc.Height,
					engineRaw,
					g_capture.status,
					"engine source at handback seam"sv)) {
				return;
			}

			std::uint64_t mismatchBytes = 0;
			for (std::size_t i = 0; i < privateRaw.size(); ++i) {
				mismatchBytes += privateRaw[i] != engineRaw[i] ? 1u : 0u;
			}
			if (!SplitVisualisedStereo(
					privateRaw,
					privateDesc.Width,
					privateDesc.Height,
					g_capture.leftHandbackPrivate,
					g_capture.rightHandbackPrivate,
					g_capture.status,
					"private output at handback seam"sv) ||
				!SplitVisualisedStereo(
					engineRaw,
					engineDesc.Width,
					engineDesc.Height,
					g_capture.leftHandbackEngine,
					g_capture.rightHandbackEngine,
					g_capture.status,
					"engine source at handback seam"sv)) {
				return;
			}

			const auto eyeWidth = engineDesc.Width / 2u;
			g_capture.status.handbackPrivateReassemblySerial = a_privateReassemblySerial;
			g_capture.status.handbackNgxSerial = a_handback.serial;
			g_capture.status.handbackRawBytes = privateRaw.size();
			g_capture.status.handbackRawMismatchBytes = mismatchBytes;
			g_capture.status.handbackRawEqual = mismatchBytes == 0;
			g_capture.status.leftHandbackPrivate = Analyse(
				g_capture.leftHandbackPrivate, eyeWidth, engineDesc.Height);
			g_capture.status.rightHandbackPrivate = Analyse(
				g_capture.rightHandbackPrivate, eyeWidth, engineDesc.Height);
			g_capture.status.leftHandbackEngine = Analyse(
				g_capture.leftHandbackEngine, eyeWidth, engineDesc.Height);
			g_capture.status.rightHandbackEngine = Analyse(
				g_capture.rightHandbackEngine, eyeWidth, engineDesc.Height);
			g_capture.status.handbackSeamCaptured = true;
		} catch (const std::exception& e) {
			g_capture.status.handbackSeamCaptured = false;
			try {
				g_capture.status.error = std::string{ "exact handback seam capture threw: " } + e.what();
			} catch (...) {
			}
		} catch (...) {
			g_capture.status.handbackSeamCaptured = false;
			try {
				g_capture.status.error = "exact handback seam capture threw a non-standard exception";
			} catch (...) {
			}
		}
	}

	struct QualityHandbackCaptureContext
	{
		ID3D11Texture2D* engineSource = nullptr;
		ID3D11Texture2D* privateOutput = nullptr;
		std::uint64_t privateReassemblySerial = 0;
		HDRRoundTrip::HandbackStamp handback{};
	};

	static void OnQualityHandbackLockedThunk(void* a_context) noexcept
	{
		auto* const context = static_cast<QualityHandbackCaptureContext*>(a_context);
		if (context) {
			OnQualityHandbackLockedImpl(
				context->engineSource,
				context->privateOutput,
				context->privateReassemblySerial,
				context->handback);
		}
	}

	void OnQualityHandback(
		ID3D11Texture2D* a_engineSource,
		ID3D11Texture2D* a_privateOutput,
		std::uint64_t a_privateReassemblySerial,
		HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		const auto requested = g_requestedSequence.load(std::memory_order_acquire);
		if (requested == 0 || requested == g_completedSequence.load(std::memory_order_acquire) ||
			a_handback.source != HDRRoundTrip::HandbackSource::kNgx || a_handback.serial == 0) {
			return;
		}

		QualityHandbackCaptureContext context{
			a_engineSource,
			a_privateOutput,
			a_privateReassemblySerial,
			a_handback
		};
		std::uint32_t exceptionCode = 0;
		const bool returned = InvokeWithCaptureTryLock(
			OnQualityHandbackLockedThunk,
			std::addressof(context),
			std::addressof(exceptionCode));
		if (!returned && exceptionCode != 0) {
			try {
				logger::warn(FMT_STRING("stereo capture: exact handback readback raised SEH 0x{:08X}; capture remains pending"), exceptionCode);
			} catch (...) {
			}
		}
	}

	static void OnQualityTransactionCommittedLockedImpl(HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		if (a_handback.source != HDRRoundTrip::HandbackSource::kNgx || a_handback.serial == 0) {
			return;
		}
		const auto requested = g_requestedSequence.load(std::memory_order_acquire);
		if (requested == 0 || requested == g_completedSequence.load(std::memory_order_acquire)) {
			return;
		}
		const auto current = g_requestedSequence.load(std::memory_order_acquire);
		if (!g_capture.requireHandbackSeam || !g_capture.awaitingQualityCommit ||
			current == g_completedSequence.load(std::memory_order_acquire) ||
			g_capture.status.handbackNgxSerial != a_handback.serial ||
			g_capture.status.ngxHandbackSerial != a_handback.serial) {
			return;
		}
		try {
			// OnTemporalAADraw read RT4 straight after the seam draw wrote it. The late
			// range has now committed the transaction, so read the submitted target again:
			// the two must agree, and the proof request completes only on this commit.
			if (!Readback(g_capture)) {
				logger::warn(FMT_STRING("stereo capture: committed quality target readback failed; request remains pending: {}"),
					g_capture.status.error);
				ResetQualityCaptureAttempt(g_capture, true);
				return;
			}
		} catch (const std::exception& e) {
			logger::warn(FMT_STRING("stereo capture: committed quality target readback threw: {}"), e.what());
			ResetQualityCaptureAttempt(g_capture, true);
			return;
		} catch (...) {
			logger::warn("stereo capture: committed quality target readback threw a non-standard exception"sv);
			ResetQualityCaptureAttempt(g_capture, true);
			return;
		}

		g_capture.awaitingQualityCommit = false;
		g_capture.status.handbackTransactionCommitPending = false;
		g_capture.status.handbackTransactionCommitted = true;
		g_capture.status.handbackCommittedNgxSerial = a_handback.serial;
		g_capture.status.completedSequence = current;
		g_completedSequence.store(current, std::memory_order_release);
	}

	struct QualityTransactionCaptureContext
	{
		HDRRoundTrip::HandbackStamp handback{};
	};

	static void OnQualityTransactionCommittedLockedThunk(void* a_context) noexcept
	{
		auto* const context = static_cast<QualityTransactionCaptureContext*>(a_context);
		if (context) {
			OnQualityTransactionCommittedLockedImpl(context->handback);
		}
	}

	void OnQualityTransactionCommitted(HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		if (a_handback.source != HDRRoundTrip::HandbackSource::kNgx || a_handback.serial == 0) {
			return;
		}
		QualityTransactionCaptureContext context{ a_handback };
		std::uint32_t exceptionCode = 0;
		const bool returned = InvokeWithCaptureTryLock(
			OnQualityTransactionCommittedLockedThunk,
			std::addressof(context),
			std::addressof(exceptionCode));
		if (!returned && exceptionCode != 0) {
			try {
				logger::warn(FMT_STRING("stereo capture: committed target notification raised SEH 0x{:08X}; capture remains pending"), exceptionCode);
			} catch (...) {
			}
		}
	}

	static void OnQualityTransactionAbortedLockedImpl(HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		if (a_handback.source != HDRRoundTrip::HandbackSource::kNgx || a_handback.serial == 0) {
			return;
		}
		const auto requested = g_requestedSequence.load(std::memory_order_acquire);
		if (requested == 0 || requested == g_completedSequence.load(std::memory_order_acquire)) {
			return;
		}
		if (!g_capture.requireHandbackSeam ||
			(g_capture.status.handbackNgxSerial != a_handback.serial &&
			 g_capture.status.ngxHandbackSerial != a_handback.serial)) {
			return;
		}
		ResetQualityCaptureAttempt(g_capture, true);
	}

	static void OnQualityTransactionAbortedLockedThunk(void* a_context) noexcept
	{
		auto* const context = static_cast<QualityTransactionCaptureContext*>(a_context);
		if (context) {
			OnQualityTransactionAbortedLockedImpl(context->handback);
		}
	}

	void OnQualityTransactionAborted(HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		if (a_handback.source != HDRRoundTrip::HandbackSource::kNgx || a_handback.serial == 0) {
			return;
		}
		QualityTransactionCaptureContext context{ a_handback };
		std::uint32_t exceptionCode = 0;
		const bool returned = InvokeWithCaptureTryLock(
			OnQualityTransactionAbortedLockedThunk,
			std::addressof(context),
			std::addressof(exceptionCode));
		if (!returned && exceptionCode != 0) {
			try {
				logger::warn(FMT_STRING("stereo capture: abort notification raised SEH 0x{:08X}; capture remains pending"), exceptionCode);
			} catch (...) {
			}
		}
	}

	static void OnTemporalAADrawLockedImpl(
		std::uint64_t a_temporalAACall,
		std::uint32_t a_threadId,
		bool a_bypassSucceeded,
		bool a_overwriteSucceeded,
		HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		const auto requested = g_requestedSequence.load(std::memory_order_acquire);
		if (requested == 0 || requested == g_completedSequence.load(std::memory_order_acquire)) {
			return;
		}
		const auto current = g_requestedSequence.load(std::memory_order_acquire);
		if (current != g_completedSequence.load(std::memory_order_acquire)) {
			if (g_capture.requireFfxHandback) {
				if (a_handback.source != HDRRoundTrip::HandbackSource::kFfx ||
					a_handback.serial <= g_capture.consumedFfxHandbackSerial) {
					if (a_handback.source == HDRRoundTrip::HandbackSource::kFfx &&
						a_handback.serial >= HDRRoundTrip::ContinuousTargetSuccesses()) {
						CompleteExhaustedFfxRequest(g_capture, current, a_handback.serial);
					}
					return;
				}
				const auto freshHandbacks = a_handback.serial - g_capture.consumedFfxHandbackSerial;
				g_capture.consumedFfxHandbackSerial = a_handback.serial;
				if (g_capture.remainingHandbacks > 0) {
					const auto skipped = static_cast<std::uint32_t>((std::min)(
						freshHandbacks,
						static_cast<std::uint64_t>(g_capture.remainingHandbacks)));
					g_capture.remainingHandbacks -= skipped;
					g_capture.status.skippedHandbacks += skipped;
					if (freshHandbacks <= skipped) {
						if (a_handback.serial >= HDRRoundTrip::ContinuousTargetSuccesses()) {
							CompleteExhaustedFfxRequest(g_capture, current, a_handback.serial);
						}
						return;
					}
				}
			}
			if (g_capture.requireBypass && !a_bypassSucceeded) {
				return;
			}
			if (g_capture.requireBypass && g_capture.remainingBypasses > 0) {
				--g_capture.remainingBypasses;
				++g_capture.status.skippedBypasses;
				return;
			}
			std::uint32_t jitterPhase = kAnyJitterPhase;
			const bool phaseRead = Jitter::ReadCurrentPhase(jitterPhase);
			if (g_capture.requiredJitterPhase != kAnyJitterPhase &&
				(!phaseRead || jitterPhase != g_capture.requiredJitterPhase)) {
				++g_capture.status.skippedJitterPhases;
				if (g_capture.requireFfxHandback &&
					a_handback.serial >= HDRRoundTrip::ContinuousTargetSuccesses()) {
					CompleteExhaustedFfxRequest(g_capture, current, a_handback.serial);
				}
				return;
			}
			if (g_capture.waitForInputs && !PrivateInputsAvailable()) {
				if (g_capture.requireFfxHandback &&
					a_handback.serial >= HDRRoundTrip::ContinuousTargetSuccesses()) {
					CompleteExhaustedFfxRequest(g_capture, current, a_handback.serial);
				}
				return;
			}
			if (g_capture.includeReassembled &&
				HDRRoundTrip::PrivateReassemblySerial() < g_capture.requiredPrivateReassemblySerial) {
				return;
			}
			if (g_capture.requireHandbackSeam &&
				(!g_capture.status.handbackSeamCaptured ||
				 g_capture.status.handbackNgxSerial != a_handback.serial ||
				 g_capture.status.handbackPrivateReassemblySerial != HDRRoundTrip::PrivateReassemblySerial())) {
				return;
			}
			g_capture.status.requestedSequence = current;
			g_capture.status.temporalAACall = a_temporalAACall;
			g_capture.status.ngxHandbackSerial =
				a_handback.source == HDRRoundTrip::HandbackSource::kNgx ? a_handback.serial : 0;
			g_capture.status.ffxHandbackSerial =
				a_handback.source == HDRRoundTrip::HandbackSource::kFfx ? a_handback.serial : 0;
			g_capture.status.jitterPhase = phaseRead ? jitterPhase : kAnyJitterPhase;
			g_capture.status.threadId = a_threadId;
			g_capture.status.bypassed = a_bypassSucceeded;
			g_capture.status.overwritten = a_overwriteSucceeded;
			try {
				// A reduced-NGX bypass has already written RT4 at this seam, so the
				// submitted target itself is the capture source in every mode.
				g_capture.status.captured = Readback(g_capture);
				if (g_capture.status.captured && g_capture.includeInputs) {
					g_capture.status.inputsCaptured = ReadbackInputs(g_capture);
				}
				if (g_capture.status.captured && g_capture.includeReassembled) {
					g_capture.status.reassembledCaptured = ReadbackReassembled(g_capture);
				}
			} catch (const std::exception& e) {
				g_capture.status.captured = false;
				try {
					g_capture.status.error = std::string{ "stereo capture threw: " } + e.what();
				} catch (...) {
				}
			} catch (...) {
				g_capture.status.captured = false;
				try {
					g_capture.status.error = "stereo capture threw a non-standard exception";
				} catch (...) {
				}
			}
			if (g_capture.requireHandbackSeam) {
				g_capture.awaitingQualityCommit = true;
				g_capture.status.handbackTransactionCommitPending = true;
				g_capture.status.handbackTransactionCommitted = false;
				g_capture.status.handbackCommittedNgxSerial = 0;
			} else {
				g_capture.status.completedSequence = current;
				g_completedSequence.store(current, std::memory_order_release);
			}
		}
	}

	struct TemporalAACaptureContext
	{
		std::uint64_t temporalAACall = 0;
		std::uint32_t threadId = 0;
		bool bypassSucceeded = false;
		bool overwriteSucceeded = false;
		HDRRoundTrip::HandbackStamp handback{};
	};

	static void OnTemporalAADrawLockedThunk(void* a_context) noexcept
	{
		auto* const context = static_cast<TemporalAACaptureContext*>(a_context);
		if (context) {
			OnTemporalAADrawLockedImpl(
				context->temporalAACall,
				context->threadId,
				context->bypassSucceeded,
				context->overwriteSucceeded,
				context->handback);
		}
	}

	void OnTemporalAADraw(
		std::uint64_t a_temporalAACall,
		std::uint32_t a_threadId,
		bool a_bypassSucceeded,
		bool a_overwriteSucceeded,
		HDRRoundTrip::HandbackStamp a_handback) noexcept
	{
		const auto requested = g_requestedSequence.load(std::memory_order_acquire);
		if (requested == 0 || requested == g_completedSequence.load(std::memory_order_acquire)) {
			return;
		}

		TemporalAACaptureContext context{
			a_temporalAACall,
			a_threadId,
			a_bypassSucceeded,
			a_overwriteSucceeded,
			a_handback
		};
		std::uint32_t exceptionCode = 0;
		const bool returned = InvokeWithCaptureTryLock(
			OnTemporalAADrawLockedThunk,
			std::addressof(context),
			std::addressof(exceptionCode));
		if (!returned && exceptionCode != 0) {
			try {
				logger::warn(FMT_STRING("stereo capture: TemporalAA readback raised SEH 0x{:08X}; capture remains pending"), exceptionCode);
			} catch (...) {
			}
		}
	}

	Status SaveAndSnapshot()
	{
		const std::scoped_lock lock(g_captureLock);
		if (g_capture.status.captured &&
			(!g_capture.requireHandbackSeam || g_capture.status.handbackTransactionCommitted) &&
			!g_capture.saveAttempted) {
			g_capture.saveAttempted = true;
			std::error_code ec;
			auto directory = CaptureDirectory();
			if (directory.empty()) {
				ec = std::make_error_code(std::errc::no_such_file_or_directory);
			} else {
				std::filesystem::create_directories(directory, ec);
			}
			if (ec) {
				g_capture.status.error = "could not create capture directory: " + ec.message();
			} else {
				const auto stem = g_capture.status.ffxHandbackRequested ?
					fmt::format(
						"taa_pid{}_{:04}_{}_ffx{:04}",
						::GetCurrentProcessId(),
						g_capture.status.completedSequence,
						ModeName(g_capture.status),
						g_capture.status.ffxHandbackSerial) :
					fmt::format(
						"taa_pid{}_{:04}_{}_ngx{:04}",
						::GetCurrentProcessId(),
						g_capture.status.completedSequence,
						ModeName(g_capture.status),
						g_capture.status.ngxHandbackSerial);
				const auto left = directory / (stem + "_left.bmp");
				const auto right = directory / (stem + "_right.bmp");
				std::string error;
				if (WriteBmp(left, g_capture.left, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
					WriteBmp(right, g_capture.right, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error)) {
					g_capture.status.leftPath = left.string();
					g_capture.status.rightPath = right.string();
					g_capture.status.saved = true;
					if (g_capture.status.inputsRequested && g_capture.status.inputsCaptured) {
						const auto leftInput = directory / (stem + "_input_left.bmp");
						const auto rightInput = directory / (stem + "_input_right.bmp");
						const auto leftOutput = directory / (stem + "_vendorout_left.bmp");
						const auto rightOutput = directory / (stem + "_vendorout_right.bmp");
						const auto leftMotion = directory / (stem + "_motion_left.bmp");
						const auto rightMotion = directory / (stem + "_motion_right.bmp");
						const auto leftDepth = directory / (stem + "_depth_left.bmp");
						const auto rightDepth = directory / (stem + "_depth_right.bmp");
						const auto leftPostTonemap = directory / (stem + "_posttonemap_left.bmp");
						const auto rightPostTonemap = directory / (stem + "_posttonemap_right.bmp");
						const auto leftPreTonemap = directory / (stem + "_pretonemap_left.bmp");
						const auto rightPreTonemap = directory / (stem + "_pretonemap_right.bmp");
						if (WriteBmp(leftInput, g_capture.leftInputColor, g_capture.status.vendorInputWidth, g_capture.status.vendorInputHeight, error) &&
							WriteBmp(rightInput, g_capture.rightInputColor, g_capture.status.vendorInputWidth, g_capture.status.vendorInputHeight, error) &&
							WriteBmp(leftOutput, g_capture.leftNgxOutput, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(rightOutput, g_capture.rightNgxOutput, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(leftMotion, g_capture.leftMotion, g_capture.status.vendorInputWidth, g_capture.status.vendorInputHeight, error) &&
							WriteBmp(rightMotion, g_capture.rightMotion, g_capture.status.vendorInputWidth, g_capture.status.vendorInputHeight, error) &&
							WriteBmp(leftDepth, g_capture.leftDepth, g_capture.status.vendorInputWidth, g_capture.status.vendorInputHeight, error) &&
							WriteBmp(rightDepth, g_capture.rightDepth, g_capture.status.vendorInputWidth, g_capture.status.vendorInputHeight, error) &&
							WriteBmp(leftPostTonemap, g_capture.leftPostTonemap, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(rightPostTonemap, g_capture.rightPostTonemap, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(leftPreTonemap, g_capture.leftPreTonemap, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(rightPreTonemap, g_capture.rightPreTonemap, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error)) {
							g_capture.status.leftInputColorPath = leftInput.string();
							g_capture.status.rightInputColorPath = rightInput.string();
							g_capture.status.leftNgxOutputPath = leftOutput.string();
							g_capture.status.rightNgxOutputPath = rightOutput.string();
							g_capture.status.leftMotionPath = leftMotion.string();
							g_capture.status.rightMotionPath = rightMotion.string();
							g_capture.status.leftDepthPath = leftDepth.string();
							g_capture.status.rightDepthPath = rightDepth.string();
							g_capture.status.leftPostTonemapPath = leftPostTonemap.string();
							g_capture.status.rightPostTonemapPath = rightPostTonemap.string();
							g_capture.status.leftPreTonemapPath = leftPreTonemap.string();
							g_capture.status.rightPreTonemapPath = rightPreTonemap.string();
							g_capture.status.inputsSaved = true;
						} else {
							g_capture.status.error = std::move(error);
						}
					}
					if (g_capture.status.reassembledRequested && g_capture.status.reassembledCaptured) {
						const auto leftReassembled = directory / (stem + "_reassembled_left.bmp");
						const auto rightReassembled = directory / (stem + "_reassembled_right.bmp");
						if (WriteBmp(leftReassembled, g_capture.leftReassembled, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(rightReassembled, g_capture.rightReassembled, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error)) {
							g_capture.status.leftReassembledPath = leftReassembled.string();
							g_capture.status.rightReassembledPath = rightReassembled.string();
							g_capture.status.reassembledSaved = true;
						} else {
							g_capture.status.error = std::move(error);
						}
					}
					if (g_capture.status.handbackSeamRequested && g_capture.status.handbackSeamCaptured) {
						const auto leftPrivate = directory / (stem + "_handbackprivate_left.bmp");
						const auto rightPrivate = directory / (stem + "_handbackprivate_right.bmp");
						const auto leftEngine = directory / (stem + "_handbackengine_left.bmp");
						const auto rightEngine = directory / (stem + "_handbackengine_right.bmp");
						if (WriteBmp(leftPrivate, g_capture.leftHandbackPrivate, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(rightPrivate, g_capture.rightHandbackPrivate, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(leftEngine, g_capture.leftHandbackEngine, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error) &&
							WriteBmp(rightEngine, g_capture.rightHandbackEngine, g_capture.status.eyeWidth, g_capture.status.eyeHeight, error)) {
							g_capture.status.leftHandbackPrivatePath = leftPrivate.string();
							g_capture.status.rightHandbackPrivatePath = rightPrivate.string();
							g_capture.status.leftHandbackEnginePath = leftEngine.string();
							g_capture.status.rightHandbackEnginePath = rightEngine.string();
							g_capture.status.handbackSeamSaved = true;
						} else {
							g_capture.status.error = std::move(error);
						}
					}
					g_capture.left.clear();
					g_capture.right.clear();
					g_capture.left.shrink_to_fit();
					g_capture.right.shrink_to_fit();
					g_capture.leftInputColor.clear();
					g_capture.rightInputColor.clear();
					g_capture.leftNgxOutput.clear();
					g_capture.rightNgxOutput.clear();
					g_capture.leftMotion.clear();
					g_capture.rightMotion.clear();
					g_capture.leftDepth.clear();
					g_capture.rightDepth.clear();
					g_capture.leftReassembled.clear();
					g_capture.rightReassembled.clear();
					g_capture.leftPostTonemap.clear();
					g_capture.rightPostTonemap.clear();
					g_capture.leftPreTonemap.clear();
					g_capture.rightPreTonemap.clear();
					g_capture.leftHandbackPrivate.clear();
					g_capture.rightHandbackPrivate.clear();
					g_capture.leftHandbackEngine.clear();
					g_capture.rightHandbackEngine.clear();
					g_capture.leftInputColor.shrink_to_fit();
					g_capture.rightInputColor.shrink_to_fit();
					g_capture.leftNgxOutput.shrink_to_fit();
					g_capture.rightNgxOutput.shrink_to_fit();
					g_capture.leftMotion.shrink_to_fit();
					g_capture.rightMotion.shrink_to_fit();
					g_capture.leftDepth.shrink_to_fit();
					g_capture.rightDepth.shrink_to_fit();
					g_capture.leftReassembled.shrink_to_fit();
					g_capture.rightReassembled.shrink_to_fit();
					g_capture.leftPostTonemap.shrink_to_fit();
					g_capture.rightPostTonemap.shrink_to_fit();
					g_capture.leftPreTonemap.shrink_to_fit();
					g_capture.rightPreTonemap.shrink_to_fit();
					g_capture.leftHandbackPrivate.shrink_to_fit();
					g_capture.rightHandbackPrivate.shrink_to_fit();
					g_capture.leftHandbackEngine.shrink_to_fit();
					g_capture.rightHandbackEngine.shrink_to_fit();
				} else {
					g_capture.status.error = std::move(error);
				}
			}
		}

		auto out = g_capture.status;
		out.requestedSequence = g_requestedSequence.load(std::memory_order_acquire);
		out.completedSequence = g_completedSequence.load(std::memory_order_acquire);
		out.pending = out.requestedSequence != out.completedSequence;
		return out;
	}
}
