#pragma once

// Exact immediate-context state isolation for a mid-frame plugin pass.
//
// D3D11.1 context-state objects were designed for a host and plugin sharing one device:
// activating our private object saves the host state, and activating the returned object
// restores it. This is stronger than a hand-maintained Get*/Set* block, whose correctness
// is bounded by the state its author remembered and whose omissions are hidden behind an
// engine-cache invalidation.

#include <cstdint>

#include <d3d11_1.h>
#include <wrl/client.h>

namespace Upscaler::D3D11StateIsolation
{
	class Context
	{
	public:
		Context() = default;
		~Context();

		Context(const Context&) = delete;
		Context& operator=(const Context&) = delete;
		Context(Context&&) = delete;
		Context& operator=(Context&&) = delete;

		// Creates one private state object for this immediate context. Safe to call again:
		// a successful instance is retained, while a different device/context requires an
		// explicit Shutdown so a device recreation cannot silently reuse stale state.
		[[nodiscard]] bool Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_immediate) noexcept;

		// Attempts to restore the host first if a normal path forgot End, then releases
		// every COM ref. Shutdown must run on the same render thread if a scope is active.
		void Shutdown() noexcept;

		// Begin activates our private state and captures the host's current state object.
		// End restores that exact host object and retains our now-updated private object for
		// the next frame. Nesting and cross-thread use fail closed.
		[[nodiscard]] bool Begin() noexcept;
		[[nodiscard]] bool End() noexcept;

		[[nodiscard]] bool Ready() const noexcept { return _context1 && _privateState; }
		[[nodiscard]] bool Active() const noexcept { return _active; }
		[[nodiscard]] HRESULT LastError() const noexcept { return _lastError; }
		[[nodiscard]] D3D_FEATURE_LEVEL ChosenFeatureLevel() const noexcept { return _chosenFeatureLevel; }

	private:
		Microsoft::WRL::ComPtr<ID3D11DeviceContext1> _context1;
		Microsoft::WRL::ComPtr<ID3DDeviceContextState> _privateState;
		Microsoft::WRL::ComPtr<ID3DDeviceContextState> _hostState;
		D3D_FEATURE_LEVEL _chosenFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(0);
		std::uint32_t _activeThread = 0;
		HRESULT _lastError = S_OK;
		bool _active = false;
	};

	// Normal-path RAII. A guarded vendor call that can raise SEH still needs an explicit
	// __finally restoring End; C++ destructors do not make that stronger guarantee.
	class Scope
	{
	public:
		explicit Scope(Context& a_context) noexcept :
			_context(a_context.Begin() ? &a_context : nullptr)
		{}

		~Scope()
		{
			if (_context) {
				static_cast<void>(_context->End());
			}
		}

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

		[[nodiscard]] explicit operator bool() const noexcept { return _context != nullptr; }

		[[nodiscard]] bool Close() noexcept
		{
			if (!_context) {
				return false;
			}
			Context* const context = _context;
			_context = nullptr;
			return context->End();
		}

	private:
		Context* _context = nullptr;
	};
}
