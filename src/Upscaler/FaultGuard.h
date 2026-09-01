#pragma once

// Reusable SEH primitives. Everything that touches memory this plugin does not own -
// an engine read, an engine call, a raw D3D sequence - runs inside one of these, so a
// fault costs a log line and a disabled step instead of the session.
//
// Two rules shape every use, and both propagate outward from here.
//
// MSVC forbids __try in a function that needs C++ unwinding (C2712). The guarded body
// is therefore a POD-only function and the __try lives in a separate wrapper. Any
// helper called from inside a guard needs a buffer form rather than one that builds a
// std::string, or the fault path allocates on its way out.
//
// Anything the unwind must not skip - a spin lock, a refcount, a modified engine
// global - goes in __finally. The wrapper's __except still trips the latch.
//
// Deliberately absent: AddVectoredExceptionHandler and SetUnhandledExceptionFilter.
// Buffout owns process-wide crash reporting and the shipped .pdb is what makes it name
// our code; installing a process-wide handler would take that over.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

struct _EXCEPTION_POINTERS;

// Step attribution. A fault that says only "access violation" costs a session; one that
// says "faulted at step 8 (evaluate eye 1)" costs a minute. Each guarded pass declares
// its own `volatile long g_lastStep`, advances it through the body with this, and hands
// LogFault a name table whose indices match.
#define US_FAULT_STEP(a_slot, a_n) ((a_slot) = (a_n))

namespace Upscaler::FaultGuard
{
	// Guarded memcpy, so a raw engine read is one call rather than an ad-hoc __try per
	// site. Returns false and leaves a_dst untouched-or-partial on a fault.
	[[nodiscard]] bool SafeReadBytes(const void* a_src, void* a_dst, std::size_t a_len) noexcept;

	// Guarded memcpy in the other direction. This is only for verified writable engine
	// data; code patches still go through the hook installer and its byte verifier.
	// Returns false when the destination faults. Callers that make a multi-value change
	// must keep their own rollback ledger because a fault can leave a partial write.
	[[nodiscard]] bool SafeWriteBytes(void* a_dst, const void* a_src, std::size_t a_len) noexcept;

	// True when a_address falls inside the game module, using SizeOfImage from the
	// loaded PE headers rather than a guessed .text upper bound.
	[[nodiscard]] bool InModule(std::uintptr_t a_address) noexcept;

	// Module-relative form of an absolute address, or 0 when it is outside the module.
	// An RVA can be pasted straight into Ghidra; an absolute address cannot.
	[[nodiscard]] std::uintptr_t ToRva(std::uintptr_t a_address) noexcept;

	// What the filter captured. POD so it can live in an SEH frame.
	struct FaultRecord
	{
		std::uint32_t code = 0;
		std::uintptr_t address = 0;
		std::uintptr_t rva = 0;
		std::uintptr_t regs[16] = {};  // rax rbx rcx rdx rsi rdi rbp rsp r8..r15
		std::uintptr_t stack[8] = {};  // in-module qwords scanned from rsp, as RVAs
		std::uint32_t stackCount = 0;
		long step = 0;
	};

	// Snapshots the exception code, the faulting address, all 16 general-purpose
	// registers and up to 8 in-module qwords scanned from rsp as probable return
	// addresses. The scan is itself guarded, so a bad rsp does not fault the filter.
	// Always returns EXCEPTION_EXECUTE_HANDLER.
	int CaptureFilter(_EXCEPTION_POINTERS* a_ep, FaultRecord& a_out) noexcept;

	// Emits the step name from a_steps, the code, the address, the RVA, both register
	// lines and the stack RVA list. Builds strings, so it is called after the unwind,
	// never from inside a guard.
	void LogFault(const FaultRecord& a_record, std::span<const std::string_view> a_steps, std::string_view a_what);

	// One saved engine byte and where it came from.
	struct SavedByte
	{
		std::uint8_t* p = nullptr;
		std::uint8_t v = 0;
	};

	// Fixed-capacity ledger of engine globals held in a modified state, so the fault
	// path can put them all back. No allocation and a trivial destructor, which is what
	// makes it legal inside a __finally.
	class RestoreSet
	{
	public:
		static constexpr std::size_t kCapacity = 16;

		// Saves the current value of *a_p. Returns false when the set is full, which is
		// a programming error rather than a runtime condition - raise kCapacity.
		[[nodiscard]] bool Record(std::uint8_t* a_p) noexcept;

		// Writes every saved value back, most recent first, and empties the set.
		// Silent by design: it runs where allocation is illegal. Log Count() before
		// calling if the fault path wants to say how much it undid.
		void RestoreAll() noexcept;

		[[nodiscard]] std::size_t Count() const noexcept { return _count; }

	private:
		SavedByte _saved[kCapacity]{};
		std::size_t _count = 0;
	};

	// Once a step faults it stays disabled for the session and says so once. Retry is
	// capped because a deterministic fault would otherwise re-fault on every attempt;
	// three tries separates a transient fault from a structural one.
	class Latch
	{
	public:
		explicit Latch(std::string_view a_what, std::uint32_t a_retryCap = 3) noexcept :
			_what(a_what), _retryCap(a_retryCap)
		{}

		[[nodiscard]] bool Available() const noexcept { return !_tripped; }

		// Disables the step. Logs on the false-to-true edge only. Not noexcept: it
		// writes to the log, so it belongs after the unwind, not inside a __finally.
		void Trip();

		// Clears the latch if it is tripped and the cap allows. Returns whether it
		// actually cleared, so the caller's counters measure real retries rather than
		// calls. Warns once when the cap is reached.
		[[nodiscard]] bool Retry();

	private:
		std::string_view _what;
		std::uint32_t _retryCap;
		std::uint32_t _retries = 0;
		bool _tripped = false;
	};
}
