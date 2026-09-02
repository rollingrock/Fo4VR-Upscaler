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
	// A selective output-merger bracket for a host draw whose shaders, geometry and
	// render targets are already prepared. Unlike Context, this preserves the live
	// host state and changes only the bound depth-stencil view. OMGetRenderTargets
	// AddRefs every captured view. A successful active Begin, and a failed Begin with
	// neutralizeAttempted, require EndDepthStencilOverride. The already-null success
	// releases its references inside Begin and needs no End. The token is POD so a caller
	// can keep it across an MSVC __try/__finally boundary.
	struct DepthStencilOverrideToken
	{
		ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* depthStencil = nullptr;
		ID3D11DeviceContext* context = nullptr;  // borrowed
		UINT renderTargetCount = 0;
		std::uint32_t threadId = 0;
		std::uint32_t exceptionCode = 0;
		bool captured = false;
		bool hadDepthStencil = false;
		bool alreadyNull = false;
		bool neutralizeAttempted = false;
		bool active = false;
		bool restored = false;
		bool restoreMismatch = false;
		bool stateUncertain = false;
	};

	// Rebinds the exact live RTV array with a null DSV while retaining any output-
	// merger UAV bindings. A null DSV is a successful no-op. End restores and verifies
	// the exact RTV/DSV identities, then releases every getter-owned COM reference.
	[[nodiscard]] bool BeginDepthStencilOverride(
		ID3D11DeviceContext* a_context,
		DepthStencilOverrideToken& a_token) noexcept;
	[[nodiscard]] bool EndDepthStencilOverride(
		ID3D11DeviceContext* a_context,
		DepthStencilOverrideToken& a_token) noexcept;

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

		// A driver fault can escape SwapDeviceContextState after the context changed but
		// before the call returned. In that interval neither Begin's return value nor a
		// draw-issued flag proves which state is active. The guarded caller must attempt
		// this best-effort handback and treat a false result as terminal for every user of
		// the shared immediate context.
		[[nodiscard]] bool RecoverUncertainState() noexcept;

		[[nodiscard]] bool Ready() const noexcept { return _context1 && _privateState && !_poisoned; }
		[[nodiscard]] bool Active() const noexcept { return _active; }
		[[nodiscard]] bool StateUncertain() const noexcept { return _stateUncertain; }
		[[nodiscard]] HRESULT LastError() const noexcept { return _lastError; }
		[[nodiscard]] D3D_FEATURE_LEVEL ChosenFeatureLevel() const noexcept { return _chosenFeatureLevel; }

	private:
		void MarkStateUncertain() noexcept;
		void ClearStateUncertain() noexcept;

		Microsoft::WRL::ComPtr<ID3D11DeviceContext1> _context1;
		Microsoft::WRL::ComPtr<ID3DDeviceContextState> _privateState;
		Microsoft::WRL::ComPtr<ID3DDeviceContextState> _hostState;
		ID3DDeviceContextState* _uncertainReturnedState = nullptr;
		D3D_FEATURE_LEVEL _chosenFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(0);
		std::uint32_t _activeThread = 0;
		HRESULT _lastError = S_OK;
		bool _active = false;
		bool _stateUncertain = false;
		bool _poisoned = false;
	};

	// The FO4VR immediate context is shared by every isolation Context in this plugin.
	// Once any instance cannot prove handback, no other instance may attempt a repair,
	// draw, vendor call, or copy against that same context.
	[[nodiscard]] bool HostStateUncertain() noexcept;

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
