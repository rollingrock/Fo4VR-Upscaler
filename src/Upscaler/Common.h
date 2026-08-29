#pragma once

// The three helpers every engine-side module wants and nobody should write twice:
// the address cast, the COM release, and the log rate gate. One spelling per idiom -
// copying any of these into a second file is a defect, and the reference mod ended up
// with three copies of Fn by doing exactly that.
//
// This header is plugin-side. It is listed in cmake/headerlist.cmake, not
// backendlist.cmake, and nothing under src/Upscaler/Backend/ may include it: that
// library links neither CommonLibF4 nor F4SE, and Fn below reads the module base
// through REL. REL and GetTickCount64 arrive from the forced PCH, which every plugin
// translation unit receives and the backend library does not.

#include <cstdint>

namespace Upscaler
{
	// Absolute address of an RVA against Fallout4VR.exe. Every engine entry point goes
	// through this rather than a hand-written cast, so the base read has one home and
	// the RVA at the call site stays the only thing worth checking.
	template <class T>
	[[nodiscard]] T Fn(std::uintptr_t a_rva) noexcept
	{
		return reinterpret_cast<T>(REL::Module::get().base() + a_rva);
	}

	// Release and null in one step. Every D3D11 Get*/Query*/GetResource hands back a
	// reference whether it was wanted or not; a leaked view per frame is invisible in a
	// five-minute test and fatal in an hour of play.
	template <class T>
	void SafeRelease(T*& a_p) noexcept
	{
		if (a_p) {
			a_p->Release();
			a_p = nullptr;
		}
	}

	// Gate for a log line on a path that runs per frame or per eye.
	//
	// Two modes. A non-zero interval is a tick floor: Ready() answers true at most once
	// per interval. A zero interval is a one-shot: Ready() answers true once and then
	// never again until Rearm().
	//
	// The rule that matters, because breaking it is what turns a once-per-session line
	// into a per-frame line: a one-shot is re-armed by the enclosing pass, never by a
	// single item's negative case. Re-arming inside a per-item loop means a mixed scene
	// re-arms the gate every frame and the line spams.
	class LogGate
	{
	public:
		explicit LogGate(std::uint64_t a_intervalMs = 0) noexcept :
			_intervalMs(a_intervalMs)
		{}

		[[nodiscard]] bool Ready() noexcept
		{
			if (_intervalMs == 0) {
				if (_fired) {
					return false;
				}
				_fired = true;
				return true;
			}

			const std::uint64_t now = GetTickCount64();
			if (_fired && now - _last < _intervalMs) {
				return false;
			}

			_fired = true;
			_last = now;
			return true;
		}

		void Rearm() noexcept
		{
			_fired = false;
			_last = 0;
		}

	private:
		std::uint64_t _intervalMs;
		std::uint64_t _last = 0;
		bool _fired = false;
	};

	// Heartbeat predicate for a counted event: the first few, then one in three hundred.
	// Enough to prove a path is alive without measuring it in log lines.
	[[nodiscard]] constexpr bool Heartbeat(std::uint64_t a_n) noexcept
	{
		return a_n <= 5 || (a_n % 300) == 0;
	}
}
