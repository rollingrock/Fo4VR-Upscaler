#include "Upscaler/FaultGuard.h"

#include <cstring>
#include <string>

namespace Upscaler::FaultGuard
{
	namespace
	{
		struct ModuleRange
		{
			std::uintptr_t base = 0;
			std::uintptr_t end = 0;
		};

		// SizeOfImage from the loaded PE headers, not a guessed .text upper bound - the
		// bound decides which stack qwords are reported as return addresses, and a wrong
		// one either hides the call path or fills it with noise.
		[[nodiscard]] const ModuleRange& Range() noexcept
		{
			static const ModuleRange s_range = [] {
				ModuleRange r{};
				r.base = REL::Module::get().base();
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(r.base);
				if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
					const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(r.base + static_cast<std::uintptr_t>(dos->e_lfanew));
					if (nt->Signature == IMAGE_NT_SIGNATURE) {
						r.end = r.base + nt->OptionalHeader.SizeOfImage;
					}
				}
				return r;
			}();
			return s_range;
		}
	}

	bool SafeReadBytes(const void* a_src, void* a_dst, std::size_t a_len) noexcept
	{
		__try {
			std::memcpy(a_dst, a_src, a_len);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool SafeWriteBytes(void* a_dst, const void* a_src, std::size_t a_len) noexcept
	{
		__try {
			std::memcpy(a_dst, a_src, a_len);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool InModule(std::uintptr_t a_address) noexcept
	{
		const auto& r = Range();
		return r.end != 0 && a_address >= r.base && a_address < r.end;
	}

	std::uintptr_t ToRva(std::uintptr_t a_address) noexcept
	{
		return InModule(a_address) ? a_address - Range().base : 0;
	}

	int CaptureFilter(_EXCEPTION_POINTERS* a_ep, FaultRecord& a_out) noexcept
	{
		// Hoisted out of the guarded loop below: this frame must stay POD-only.
		const auto base = Range().base;
		const auto end = Range().end;

		if (!a_ep || !a_ep->ExceptionRecord) {
			return EXCEPTION_EXECUTE_HANDLER;
		}

		a_out.code = a_ep->ExceptionRecord->ExceptionCode;
		a_out.address = reinterpret_cast<std::uintptr_t>(a_ep->ExceptionRecord->ExceptionAddress);
		a_out.rva = (end != 0 && a_out.address >= base && a_out.address < end) ? a_out.address - base : 0;

		const auto* c = a_ep->ContextRecord;
		if (!c) {
			return EXCEPTION_EXECUTE_HANDLER;
		}

		const std::uintptr_t regs[16] = {
			c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp,
			c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15
		};
		std::memcpy(a_out.regs, regs, sizeof(regs));

		// Probable return addresses: module-range qwords in the top of the stack. rsp
		// itself can be the reason we are here, so every read is guarded and the scan
		// stops at the first bad one.
		a_out.stackCount = 0;
		if (end == 0) {
			return EXCEPTION_EXECUTE_HANDLER;
		}
		const auto* sp = reinterpret_cast<const std::uintptr_t*>(c->Rsp);
		for (std::uint32_t i = 0; i < 0x100 && a_out.stackCount < 8; ++i) {
			std::uintptr_t v = 0;
			__try {
				v = sp[i];
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				break;
			}
			if (v >= base && v < end) {
				a_out.stack[a_out.stackCount++] = v - base;
			}
		}
		return EXCEPTION_EXECUTE_HANDLER;
	}

	void LogFault(const FaultRecord& a_record, std::span<const std::string_view> a_steps, std::string_view a_what)
	{
		const auto step = a_record.step;
		const auto name = (step >= 0 && static_cast<std::size_t>(step) < a_steps.size()) ? a_steps[static_cast<std::size_t>(step)] : "?"sv;

		logger::critical(
			FMT_STRING("{} faulted at step {} ({}), code {:08X} at {:016X} (rva {:X})"),
			a_what, step, name, a_record.code, a_record.address, a_record.rva);
		logger::critical(
			FMT_STRING("  rax {:016X} rbx {:016X} rcx {:016X} rdx {:016X} rsi {:016X} rdi {:016X} rbp {:016X} rsp {:016X}"),
			a_record.regs[0], a_record.regs[1], a_record.regs[2], a_record.regs[3],
			a_record.regs[4], a_record.regs[5], a_record.regs[6], a_record.regs[7]);
		logger::critical(
			FMT_STRING("  r8  {:016X} r9  {:016X} r10 {:016X} r11 {:016X} r12 {:016X} r13 {:016X} r14 {:016X} r15 {:016X}"),
			a_record.regs[8], a_record.regs[9], a_record.regs[10], a_record.regs[11],
			a_record.regs[12], a_record.regs[13], a_record.regs[14], a_record.regs[15]);

		std::string stack;
		for (std::uint32_t i = 0; i < a_record.stackCount; ++i) {
			stack += fmt::format(FMT_STRING("{}{:X}"), i != 0 ? " " : "", a_record.stack[i]);
		}
		// RVAs, so the list pastes straight into Ghidra.
		logger::critical(FMT_STRING("  stack rvas: {}"), stack.empty() ? "(none in module)"s : stack);
	}

	bool RestoreSet::Record(std::uint8_t* a_p) noexcept
	{
		if (!a_p || _count >= kCapacity) {
			return false;
		}
		_saved[_count].p = a_p;
		_saved[_count].v = *a_p;
		++_count;
		return true;
	}

	void RestoreSet::RestoreAll() noexcept
	{
		// Most recent first, so nested brackets over the same byte unwind in order.
		while (_count != 0) {
			--_count;
			auto& e = _saved[_count];
			if (e.p) {
				*e.p = e.v;
			}
			e.p = nullptr;
			e.v = 0;
		}
	}

	void Latch::Trip()
	{
		if (_tripped) {
			return;
		}
		_tripped = true;
		logger::error(FMT_STRING("{} disabled for this session after a fault"), _what);
	}

	bool Latch::Retry()
	{
		if (!_tripped || _retries >= _retryCap) {
			return false;
		}
		_tripped = false;
		++_retries;
		logger::info(FMT_STRING("{} fault latch cleared - retry {} of {}"), _what, _retries, _retryCap);
		if (_retries == _retryCap) {
			logger::warn(FMT_STRING("{} retry cap reached - the latch will hold for the rest of the session"), _what);
		}
		return true;
	}
}
