#pragma once

#define NOMMNOSOUND

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#pragma warning(disable: 4100)

#pragma warning(push)
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <toml++/toml.h>
#pragma warning(pop)

namespace logger = F4SE::log;

using namespace std::literals;

namespace pstl
{
	using namespace F4SE::stl;

	// Overwrite a_size bytes at a_from with a jump to a_to. Defined in PCH.cpp,
	// the only translation unit that includes Xbyak. asm_replace fills the site
	// with int3 first, so a short jump never leaves a live instruction tail.
	void asm_jump(std::uintptr_t a_from, std::size_t a_size, std::uintptr_t a_to);
	void asm_replace(std::uintptr_t a_from, std::size_t a_size, std::uintptr_t a_to);

	template <class T>
	void asm_replace(std::uintptr_t a_from)
	{
		asm_replace(a_from, T::size, reinterpret_cast<std::uintptr_t>(T::func));
	}

	// Unlike the place-in-red template this does not call F4SE::AllocTrampoline
	// per hook. On F4SEVR (no branch-pool interface) AllocTrampoline falls back to
	// Trampoline::create, and create/set_trampoline release() the previous buffer -
	// VirtualFreeing every previously written stub. With more than one hook that
	// orphans earlier call sites into freed memory. Allocate the trampoline once
	// in F4SEPlugin_Load with capacity for all hooks before calling this.
	template <class T>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = F4SE::GetTrampoline();
		T::func = trampoline.write_call<5>(a_src, T::thunk);
	}

	// CommonLibF4 RE types only: F must carry the static VTABLE relocation array,
	// which is what makes the slot addressable before an instance exists. A COM
	// interface has no such member, so a swapchain or device-context detour needs
	// a different helper - one that patches the slot through the raw interface
	// pointer under VirtualProtect. Do not reach for this one there.
	template <class F, size_t offset, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[offset] };
		T::func = vtbl.write_vfunc(T::idx, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		write_vfunc<F, 0, T>();
	}

	inline std::string as_string(std::string_view a_view)
	{
		return { a_view.data(), a_view.size() };
	}
}

#define DLLEXPORT __declspec(dllexport)

#include "Version.h"
