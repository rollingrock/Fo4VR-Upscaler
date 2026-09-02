#include "Upscaler/MipBias.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"

#include <atomic>
#include <bit>
#include <cstring>

namespace Upscaler::MipBias
{
	namespace
	{
		std::atomic<std::uintptr_t> g_tableBase{ 0 };
		std::atomic<bool> g_anchorsAgreed{ false };
		std::atomic<std::uint32_t> g_overrideBits{ std::bit_cast<std::uint32_t>(SamplerBias::kOverrideSentinel) };
		std::atomic<bool> g_includeTrilinear{ false };
		std::atomic<bool> g_lowerMinLod{ false };

		// The probe. Requested from the listener thread, serviced on the render thread at
		// the Apply seam, read back through /state or the route itself. The render side
		// only ever tries the lock, so a busy reader costs one frame of latency rather
		// than a stall inside the world hook.
		SRWLOCK g_probeLock = SRWLOCK_INIT;
		Probe g_probe{};
		std::atomic<std::uint64_t> g_probeRequested{ 0 };
		std::atomic<std::uint64_t> g_probeCompleted{ 0 };

		LogGate g_initGate;

		// The PS bind site is an absolute-disp32 SIB rather than a RIP-relative operand,
		// so the table RVA is a literal in the instruction: check the four-byte prefix and
		// read the trailing dword. This is the third opinion the design requires, and it
		// comes from the consumer rather than from either producer.
		[[nodiscard]] bool ReadBindSiteTableRva(std::uintptr_t& a_outRva) noexcept
		{
			static constexpr std::uint8_t kPrefix[] = { 0x4E, 0x8D, 0x0C, 0xC5 };
			std::uint8_t bytes[8] = {};
			const auto address = REL::Module::get().base() + Addr::kSamplerBindPsSite;
			if (!FaultGuard::SafeReadBytes(reinterpret_cast<const void*>(address), bytes, sizeof(bytes))) {
				return false;
			}
			if (std::memcmp(bytes, kPrefix, sizeof(kPrefix)) != 0) {
				return false;
			}
			std::uint32_t rva = 0;
			std::memcpy(&rva, bytes + Addr::kSamplerBindPsDispOffset, sizeof(rva));
			a_outRva = rva;
			return true;
		}

		[[nodiscard]] std::int32_t ReadSlotIndex(std::uintptr_t a_array, std::size_t a_slot) noexcept
		{
			std::int32_t value = -1;
			if (!Engine::ReadI32(a_array + a_slot * sizeof(std::int32_t), value)) {
				return -1;
			}
			return value;
		}

		void FillProbeStage(
			std::uintptr_t a_block,
			std::uintptr_t a_addressModeOffset,
			std::uintptr_t a_filterOffset,
			std::uintptr_t a_mipOffset,
			ProbeSlot (&a_out)[Addr::kStateSamplerSlotCount]) noexcept
		{
			for (std::size_t slot = 0; slot < Addr::kStateSamplerSlotCount; ++slot) {
				ProbeSlot entry{};
				entry.addressMode = ReadSlotIndex(a_block + a_addressModeOffset, slot);
				entry.filter = ReadSlotIndex(a_block + a_filterOffset, slot);
				entry.mip = ReadSlotIndex(a_block + a_mipOffset, slot);
				entry.valid = entry.addressMode >= 0 &&
				              entry.addressMode < static_cast<std::int32_t>(SamplerBias::kAddressModeCount) &&
				              entry.filter >= 0 &&
				              entry.filter < static_cast<std::int32_t>(SamplerBias::kFilterCount) &&
				              entry.mip >= 0 &&
				              entry.mip < static_cast<std::int32_t>(SamplerBias::kMipCount);
				if (entry.valid) {
					// The creation loop's own arithmetic, which is the only thing that
					// turns a bound triple into a table slot.
					entry.index = (entry.addressMode * static_cast<std::int32_t>(SamplerBias::kFilterCount) + entry.filter) *
					                  static_cast<std::int32_t>(SamplerBias::kMipCount) +
					              entry.mip;
					entry.biased = SamplerBias::IsBiased(static_cast<std::size_t>(entry.index));
				}
				a_out[slot] = entry;
			}
		}

		void ServiceProbe() noexcept
		{
			const auto requested = g_probeRequested.load(std::memory_order_acquire);
			if (requested == 0 || requested == g_probeCompleted.load(std::memory_order_relaxed)) {
				return;
			}
			const auto state = Engine::GraphicsState();
			if (state == 0) {
				return;
			}
			if (!::TryAcquireSRWLockExclusive(&g_probeLock)) {
				return;
			}

			const auto block = state + Addr::kStateCachedBlock;
			Probe probe{};
			probe.requestedSequence = requested;
			probe.completedSequence = requested;
			probe.pending = false;
			probe.captured = true;
			probe.stateBlock = block;
			FillProbeStage(
				block,
				Addr::kStateSamplerPsAddressMode,
				Addr::kStateSamplerPsFilter,
				Addr::kStateSamplerPsMip,
				probe.ps);
			FillProbeStage(
				block,
				Addr::kStateSamplerVsAddressMode,
				Addr::kStateSamplerVsFilter,
				Addr::kStateSamplerVsMip,
				probe.vs);
			g_probe = probe;
			::ReleaseSRWLockExclusive(&g_probeLock);
			g_probeCompleted.store(requested, std::memory_order_release);
		}
	}

	bool Initialize()
	{
		// Two RIP-relative producers and one literal consumer. A moved or different build
		// fails an opcode check and disables the feature; a table rewrite at a plausible
		// wrong address would not fault, it would corrupt live render state, which is the
		// one failure this project cannot absorb.
		const auto creation = Engine::RipResolve(
			Addr::kSamplerStateTableCreateAnchor,
			{ 0x4C, 0x8D, 0x25 },
			Addr::kRipDispOffset,
			Addr::kRipInstrLen,
			"sampler-state table (creation)"sv);
		const auto record = Engine::RipResolve(
			Addr::kSamplerStateTableRecordAnchor,
			{ 0x48, 0x8D, 0x0D },
			Addr::kRipDispOffset,
			Addr::kRipInstrLen,
			"sampler-state table (record cache)"sv);
		std::uintptr_t bindRva = 0;
		const bool bindRead = ReadBindSiteTableRva(bindRva);

		const auto base = REL::Module::get().base();
		const bool agreed = creation != 0 && record != 0 && bindRead &&
		                    creation == record &&
		                    creation == base + bindRva &&
		                    bindRva == Addr::kSamplerStateTableRva;
		g_anchorsAgreed.store(agreed, std::memory_order_release);
		if (!agreed) {
			logger::error(
				FMT_STRING("mip bias: the sampler-state table anchors disagree - creation rva {:X}, record rva {:X}, bind literal rva {:X} (expected {:X}); the feature stays inert"),
				creation != 0 ? creation - base : 0,
				record != 0 ? record - base : 0,
				bindRead ? bindRva : 0,
				Addr::kSamplerStateTableRva);
			return false;
		}

		g_tableBase.store(creation, std::memory_order_release);
		logger::info(
			FMT_STRING("mip bias: sampler-state table resolved at rva {:X} from three agreeing anchors; {} entries of stride {}"),
			creation - base,
			SamplerBias::kEntryCount,
			sizeof(void*));
		return true;
	}

	bool Resolved() noexcept
	{
		return g_tableBase.load(std::memory_order_acquire) != 0 &&
		       g_anchorsAgreed.load(std::memory_order_acquire);
	}

	void Configure(float a_overrideBias, bool a_includeTrilinear, bool a_lowerMinLod) noexcept
	{
		g_overrideBits.store(std::bit_cast<std::uint32_t>(a_overrideBias), std::memory_order_relaxed);
		g_includeTrilinear.store(a_includeTrilinear, std::memory_order_relaxed);
		g_lowerMinLod.store(a_lowerMinLod, std::memory_order_relaxed);
	}

	bool Build(
		const Backend::IBackend* a_backend,
		std::uint32_t a_renderWidth,
		std::uint32_t a_outputWidth) noexcept
	{
		if (!Resolved()) {
			if (g_initGate.Ready()) {
				logger::warn("mip bias not built: the sampler-state table anchors did not agree at load"sv);
			}
			return false;
		}
		auto* const device = Engine::Device();
		if (!device) {
			return false;
		}
		if (!SamplerBias::Initialize(g_tableBase.load(std::memory_order_acquire), device)) {
			return false;
		}
		return SamplerBias::Build(
			a_backend,
			a_renderWidth,
			a_outputWidth,
			std::bit_cast<float>(g_overrideBits.load(std::memory_order_relaxed)),
			g_includeTrilinear.load(std::memory_order_relaxed),
			g_lowerMinLod.load(std::memory_order_relaxed));
	}

	void Apply() noexcept
	{
		ServiceProbe();
		SamplerBias::Apply();
	}

	void Restore() noexcept
	{
		SamplerBias::Restore();
	}

	std::uint64_t RequestProbe() noexcept
	{
		return g_probeRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	Probe ProbeSnapshot() noexcept
	{
		::AcquireSRWLockShared(&g_probeLock);
		Probe out = g_probe;
		::ReleaseSRWLockShared(&g_probeLock);

		out.requestedSequence = g_probeRequested.load(std::memory_order_acquire);
		out.completedSequence = g_probeCompleted.load(std::memory_order_acquire);
		out.pending = out.requestedSequence != out.completedSequence;
		out.captured = out.completedSequence != 0;
		return out;
	}

	State Snapshot() noexcept
	{
		State out{};
		out.anchorsResolved = g_tableBase.load(std::memory_order_acquire) != 0;
		out.anchorsAgreed = g_anchorsAgreed.load(std::memory_order_acquire);
		out.tableAddress = g_tableBase.load(std::memory_order_acquire);
		out.table = SamplerBias::Snapshot();
		return out;
	}
}
