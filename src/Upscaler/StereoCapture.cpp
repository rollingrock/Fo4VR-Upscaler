#include "Upscaler/StereoCapture.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Jitter.h"
#include "Upscaler/RenderTargets.h"

#include <ShlObj.h>
#include <Windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
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
			bool saveAttempted = false;
			bool requireBypass = false;
			std::uint32_t remainingBypasses = 0;
			std::uint32_t requiredJitterPhase = kAnyJitterPhase;
		};

		std::atomic<std::uint64_t> g_requestedSequence{ 0 };
		std::atomic<std::uint64_t> g_completedSequence{ 0 };
		std::mutex g_captureLock;
		Capture g_capture{};

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

		[[nodiscard]] bool Readback(Capture& a_out)
		{
			const auto submitted = RenderTargets::Lookup(Addr::kRT_Submitted);
			auto* const device = Engine::Device();
			auto* const context = Engine::ImmediateContext();
			if (!submitted.valid || !submitted.tex || !device || !context) {
				a_out.status.error = "submitted target 4, device or immediate context is unavailable";
				return false;
			}

			D3D11_TEXTURE2D_DESC desc{};
			submitted.tex->GetDesc(&desc);
			if (desc.Width == 0 || (desc.Width & 1u) != 0 || desc.Height == 0 ||
				desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.MipLevels != 1 ||
				desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
				desc.Width != submitted.width || desc.Height != submitted.height) {
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

			context->CopyResource(staging, submitted.tex);
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
	}

	std::uint64_t Request(
		bool a_requireBypass,
		std::uint32_t a_skipBypasses,
		std::uint32_t a_jitterPhase) noexcept
	{
		const std::scoped_lock lock(g_captureLock);
		const auto sequence = g_requestedSequence.load(std::memory_order_relaxed) + 1;
		g_capture = {};
		g_capture.requireBypass = a_requireBypass;
		g_capture.remainingBypasses = a_skipBypasses;
		g_capture.requiredJitterPhase = a_jitterPhase < Jitter::PhaseCount() ? a_jitterPhase : kAnyJitterPhase;
		g_capture.status.requestedSequence = sequence;
		g_capture.status.requestedSkipBypasses = a_skipBypasses;
		g_capture.status.requestedJitterPhase = g_capture.requiredJitterPhase;
		g_requestedSequence.store(sequence, std::memory_order_release);
		return sequence;
	}

	void OnTemporalAADraw(
		std::uint64_t a_temporalAACall,
		std::uint32_t a_threadId,
		bool a_bypassed,
		bool a_overwritten,
		std::uint64_t a_ngxHandbackSerial) noexcept
	{
		const auto requested = g_requestedSequence.load(std::memory_order_acquire);
		if (requested == 0 || requested == g_completedSequence.load(std::memory_order_acquire)) {
			return;
		}
		std::unique_lock lock(g_captureLock, std::try_to_lock);
		if (!lock.owns_lock()) {
			return;
		}

		const auto current = g_requestedSequence.load(std::memory_order_acquire);
		if (current != g_completedSequence.load(std::memory_order_acquire)) {
			if (g_capture.requireBypass && !a_bypassed) {
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
				return;
			}
			g_capture.status.requestedSequence = current;
			g_capture.status.completedSequence = current;
			g_capture.status.temporalAACall = a_temporalAACall;
			g_capture.status.ngxHandbackSerial = a_ngxHandbackSerial;
			g_capture.status.jitterPhase = phaseRead ? jitterPhase : kAnyJitterPhase;
			g_capture.status.threadId = a_threadId;
			g_capture.status.bypassed = a_bypassed;
			g_capture.status.overwritten = a_overwritten;
			try {
				g_capture.status.captured = Readback(g_capture);
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
			g_completedSequence.store(current, std::memory_order_release);
		}
	}

	Status SaveAndSnapshot()
	{
		const std::scoped_lock lock(g_captureLock);
		if (g_capture.status.captured && !g_capture.saveAttempted) {
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
				const auto stem = fmt::format(
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
					g_capture.left.clear();
					g_capture.right.clear();
					g_capture.left.shrink_to_fit();
					g_capture.right.shrink_to_fit();
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
