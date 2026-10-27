#include "Upscaler/D3D11StateIsolation.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <utility>

namespace Upscaler::D3D11StateIsolation
{
	namespace
	{
		std::atomic<std::uint32_t> g_uncertainContexts{ 0 };
		std::atomic_bool g_outputMergerStateUncertain{ false };
		std::atomic_bool g_externalStateUncertain{ false };

		[[nodiscard]] bool ReleaseInterface(IUnknown* a_interface) noexcept
		{
			if (!a_interface) {
				return true;
			}
			bool released = false;
			__try {
				a_interface->Release();
				released = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				released = false;
			}
			return released;
		}

		[[nodiscard]] bool ReleaseCapturedViews(DepthStencilOverrideToken& a_token) noexcept
		{
			bool released = true;
			for (auto& renderTarget : a_token.renderTargets) {
				auto* const owned = renderTarget;
				renderTarget = nullptr;
				released = ReleaseInterface(owned) && released;
			}
			auto* const ownedDepth = a_token.depthStencil;
			a_token.depthStencil = nullptr;
			return ReleaseInterface(ownedDepth) && released;
		}

		[[nodiscard]] bool CapturedBindingsMatch(
			const DepthStencilOverrideToken& a_expected,
			const DepthStencilOverrideToken& a_observed) noexcept
		{
			if (a_expected.depthStencil != a_observed.depthStencil) {
				return false;
			}
			for (std::size_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
				if (a_expected.renderTargets[i] != a_observed.renderTargets[i]) {
					return false;
				}
			}
			return true;
		}
	}

	bool BeginDepthStencilOverride(
		ID3D11DeviceContext* a_context,
		DepthStencilOverrideToken& a_token) noexcept
	{
		if (!a_context || a_token.context || a_token.captured || a_token.neutralizeAttempted ||
			HostStateUncertain()) {
			return false;
		}

		a_token.context = a_context;
		a_token.threadId = ::GetCurrentThreadId();
		bool captureReturned = false;
		__try {
			a_context->OMGetRenderTargets(
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
				a_token.renderTargets,
				&a_token.depthStencil);
			captureReturned = true;
			a_token.captured = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			if (a_token.exceptionCode == 0) {
				a_token.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			captureReturned = false;
		}
		if (!captureReturned) {
			if (!ReleaseCapturedViews(a_token)) {
				a_token.stateUncertain = true;
				g_outputMergerStateUncertain.store(true, std::memory_order_seq_cst);
			}
			return false;
		}

		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
			if (a_token.renderTargets[i]) {
				a_token.renderTargetCount = i + 1;
			}
		}
		a_token.hadDepthStencil = a_token.depthStencil != nullptr;
		if (!a_token.depthStencil) {
			a_token.alreadyNull = true;
			if (!ReleaseCapturedViews(a_token)) {
				a_token.stateUncertain = true;
				g_outputMergerStateUncertain.store(true, std::memory_order_seq_cst);
				return false;
			}
			return true;
		}
		if (a_token.renderTargetCount == 0) {
			// This bracket exists only for a prepared colour draw. Mutating a depth-only
			// OM binding here would hide a wrong seam and could turn the host call into an
			// output-less draw, so decline before changing any context state.
			if (!ReleaseCapturedViews(a_token)) {
				a_token.stateUncertain = true;
				g_outputMergerStateUncertain.store(true, std::memory_order_seq_cst);
			}
			return false;
		}

		a_token.neutralizeAttempted = true;
		bool neutralizeReturned = false;
		__try {
			a_context->OMSetRenderTargetsAndUnorderedAccessViews(
				a_token.renderTargetCount,
				a_token.renderTargetCount != 0 ? a_token.renderTargets : nullptr,
				nullptr,
				a_token.renderTargetCount,
				D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
				nullptr,
				nullptr);
			neutralizeReturned = true;
			a_token.active = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			if (a_token.exceptionCode == 0) {
				a_token.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			neutralizeReturned = false;
			a_token.stateUncertain = true;
		}
		return neutralizeReturned;
	}

	bool EndDepthStencilOverride(
		ID3D11DeviceContext* a_context,
		DepthStencilOverrideToken& a_token) noexcept
	{
		if (a_token.alreadyNull) {
			return !a_token.stateUncertain;
		}

		const bool mustRestore = a_token.active || a_token.neutralizeAttempted;
		bool restored = !mustRestore;
		if (mustRestore && a_context && a_context == a_token.context &&
			a_token.threadId == ::GetCurrentThreadId()) {
			bool restoreReturned = false;
			__try {
				a_context->OMSetRenderTargetsAndUnorderedAccessViews(
					a_token.renderTargetCount,
					a_token.renderTargetCount != 0 ? a_token.renderTargets : nullptr,
					a_token.depthStencil,
					a_token.renderTargetCount,
					D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
					nullptr,
					nullptr);
				restoreReturned = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				if (a_token.exceptionCode == 0) {
					a_token.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
				}
				restoreReturned = false;
			}

			if (restoreReturned) {
				DepthStencilOverrideToken observed{};
				bool verifyReturned = false;
				__try {
					a_context->OMGetRenderTargets(
						D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
						observed.renderTargets,
						&observed.depthStencil);
					verifyReturned = true;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					if (a_token.exceptionCode == 0) {
						a_token.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
					}
					verifyReturned = false;
				}
				restored = verifyReturned && CapturedBindingsMatch(a_token, observed);
				a_token.restoreMismatch = verifyReturned && !restored;
				if (!ReleaseCapturedViews(observed)) {
					restored = false;
				}
			}
		}

		a_token.active = false;
		a_token.restored = restored;
		const bool released = ReleaseCapturedViews(a_token);
		if (!restored || !released) {
			a_token.stateUncertain = mustRestore || a_token.stateUncertain || !released;
			if (a_token.stateUncertain) {
				g_outputMergerStateUncertain.store(true, std::memory_order_seq_cst);
			}
			return false;
		}
		a_token.stateUncertain = false;
		return true;
	}

	bool RebindDepthStencil(
		ID3D11DeviceContext* a_context,
		ID3D11DepthStencilView* a_depthStencil,
		DepthStencilRebindResult& a_result) noexcept
	{
		a_result = {};
		if (!a_context || !a_depthStencil || HostStateUncertain()) {
			a_result.declined = true;
			return false;
		}

		// The token type is reused only for its storage and its release helper; no bracket
		// is opened, so nothing here needs an End.
		DepthStencilOverrideToken live{};
		bool captureReturned = false;
		__try {
			a_context->OMGetRenderTargets(
				D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
				live.renderTargets,
				&live.depthStencil);
			captureReturned = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			captureReturned = false;
		}
		if (!captureReturned) {
			if (!ReleaseCapturedViews(live)) {
				g_outputMergerStateUncertain.store(true, std::memory_order_seq_cst);
			}
			return false;
		}
		a_result.captured = true;

		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
			if (live.renderTargets[i]) {
				a_result.renderTargetCount = i + 1;
			}
		}
		if (live.depthStencil == a_depthStencil) {
			a_result.alreadyBound = true;
			const bool released = ReleaseCapturedViews(live);
			if (!released) {
				g_outputMergerStateUncertain.store(true, std::memory_order_seq_cst);
			}
			return released;
		}
		if (a_result.renderTargetCount == 0) {
			a_result.declined = true;
			if (!ReleaseCapturedViews(live)) {
				g_outputMergerStateUncertain.store(true, std::memory_order_seq_cst);
			}
			return false;
		}

		bool bindReturned = false;
		__try {
			a_context->OMSetRenderTargetsAndUnorderedAccessViews(
				a_result.renderTargetCount,
				live.renderTargets,
				a_depthStencil,
				a_result.renderTargetCount,
				D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
				nullptr,
				nullptr);
			bindReturned = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			bindReturned = false;
		}
		a_result.bound = bindReturned;
		const bool released = ReleaseCapturedViews(live);
		if (!released) {
			g_outputMergerStateUncertain.store(true, std::memory_order_seq_cst);
		}
		return bindReturned && released;
	}

	Context::~Context()
	{
		Shutdown();
	}

	bool Context::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_immediate) noexcept
	{
		if (!a_device || !a_immediate || a_immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
			_lastError = E_INVALIDARG;
			return false;
		}
		if (_active) {
			_lastError = E_UNEXPECTED;
			return false;
		}
		if (_poisoned) {
			_lastError = E_UNEXPECTED;
			return false;
		}
		Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;
		a_immediate->GetDevice(contextDevice.GetAddressOf());
		if (contextDevice.Get() != a_device) {
			_lastError = E_INVALIDARG;
			return false;
		}

		Microsoft::WRL::ComPtr<ID3D11Device1> device1;
		_lastError = a_device->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf()));
		if (FAILED(_lastError)) {
			return false;
		}

		Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
		_lastError = a_immediate->QueryInterface(IID_PPV_ARGS(context1.GetAddressOf()));
		if (FAILED(_lastError)) {
			return false;
		}
		if (Ready()) {
			if (context1.Get() != _context1.Get()) {
				_lastError = E_INVALIDARG;
				return false;
			}
			_lastError = S_OK;
			return true;
		}

		const D3D_FEATURE_LEVEL requested = a_device->GetFeatureLevel();
		const UINT flags = (a_device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0 ?
		                       D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED :
		                       0u;

		Microsoft::WRL::ComPtr<ID3DDeviceContextState> privateState;
		D3D_FEATURE_LEVEL chosen = static_cast<D3D_FEATURE_LEVEL>(0);
		_lastError = device1->CreateDeviceContextState(
			flags,
			&requested,
			1,
			D3D11_SDK_VERSION,
			__uuidof(ID3D11Device1),
			&chosen,
			privateState.GetAddressOf());
		if (FAILED(_lastError) || !privateState || chosen != requested) {
			if (SUCCEEDED(_lastError)) {
				_lastError = E_FAIL;
			}
			return false;
		}

		_context1 = std::move(context1);
		_privateState = std::move(privateState);
		_chosenFeatureLevel = chosen;
		_activeThread = 0;
		_lastError = S_OK;
		return true;
	}

	void Context::Shutdown() noexcept
	{
		if (_stateUncertain) {
			static_cast<void>(RecoverUncertainState());
		}
		if (_active) {
			__try {
				static_cast<void>(End());
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				_lastError = static_cast<HRESULT>(::GetExceptionCode());
			}
			if (_stateUncertain) {
				static_cast<void>(RecoverUncertainState());
			}
		}
		if (_stateUncertain) {
			// Releasing a context/state object while the driver may still have it active
			// is a second unbounded vendor call. This process is already terminal; leak the
			// three references for orderly OS teardown instead of compounding the fault.
			_poisoned = true;
			static_cast<void>(_hostState.Detach());
			static_cast<void>(_privateState.Detach());
			static_cast<void>(_context1.Detach());
			return;
		}
		_hostState.Reset();
		_privateState.Reset();
		_context1.Reset();
		// An ambiguous recovery return is deliberately leaked rather than released or
		// reclassified. Explicit Shutdown only forgets that opaque process-lifetime ref.
		_uncertainReturnedState = nullptr;
		_chosenFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(0);
		_activeThread = 0;
		_active = false;
		_poisoned = false;
	}

	bool Context::Begin() noexcept
	{
		if (!Ready() || _active || HostStateUncertain()) {
			_lastError = E_UNEXPECTED;
			return false;
		}

		const std::uint32_t thread = ::GetCurrentThreadId();
		if (_activeThread != 0 && _activeThread != thread) {
			_lastError = RPC_E_WRONG_THREAD;
			return false;
		}

		_hostState.Reset();
		_activeThread = thread;
		MarkStateUncertain();
		_context1->SwapDeviceContextState(_privateState.Get(), _hostState.GetAddressOf());
		if (!_hostState) {
			_lastError = E_FAIL;
			return false;
		}

		_active = true;
		ClearStateUncertain();
		_lastError = S_OK;
		return true;
	}

	bool Context::End() noexcept
	{
		if (!_active || !_context1 || !_hostState) {
			_lastError = E_UNEXPECTED;
			return false;
		}
		if (_activeThread != ::GetCurrentThreadId()) {
			_lastError = RPC_E_WRONG_THREAD;
			return false;
		}

		Microsoft::WRL::ComPtr<ID3DDeviceContextState> privateState;
		MarkStateUncertain();
		_context1->SwapDeviceContextState(_hostState.Get(), privateState.GetAddressOf());
		_active = false;
		ClearStateUncertain();
		// Until every returned state reference is accounted for, a COM cleanup fault
		// must make this object unusable even though the captured host is active again.
		_poisoned = true;
		_hostState.Reset();

		if (!privateState) {
			_privateState.Reset();
			_lastError = E_FAIL;
			return false;
		}

		_privateState = std::move(privateState);
		_poisoned = false;
		_lastError = S_OK;
		return true;
	}

	bool Context::RecoverUncertainState() noexcept
	{
		if (!_stateUncertain) {
			return true;
		}
		if (!_context1 || !_hostState) {
			_lastError = E_UNEXPECTED;
			return false;
		}
		if (_activeThread != ::GetCurrentThreadId()) {
			_lastError = RPC_E_WRONG_THREAD;
			return false;
		}

		ID3DDeviceContextState* privateState = nullptr;
		bool hostRestored = false;
		__try {
			_context1->SwapDeviceContextState(_hostState.Get(), &privateState);
			// A normal return from Swap proves the captured host object is active again.
			// Clear the global interlock before any following COM Release can itself fault.
			hostRestored = true;
			_active = false;
			ClearStateUncertain();
			_poisoned = true;
			_hostState.Reset();
			// The first uncertain Swap may have completed before faulting. Therefore this
			// returned object could be either our former private state or the host state
			// that was already active. Retain its reference opaquely for process teardown;
			// never install it as a reusable private state.
			_uncertainReturnedState = privateState;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			_lastError = static_cast<HRESULT>(::GetExceptionCode());
		}
		if (!hostRestored) {
			return false;
		}

		// The initiating Begin/End still failed even though exact host state was
		// recovered. Keep the operation classified as failed and make a missing returned
		// private object permanently disable reuse of this Context.
		_lastError = E_FAIL;
		return true;
	}

	void Context::MarkStateUncertain() noexcept
	{
		if (!_stateUncertain) {
			_stateUncertain = true;
			g_uncertainContexts.fetch_add(1, std::memory_order_seq_cst);
		}
	}

	void Context::ClearStateUncertain() noexcept
	{
		if (_stateUncertain) {
			_stateUncertain = false;
			g_uncertainContexts.fetch_sub(1, std::memory_order_seq_cst);
		}
	}

	void LatchExternalStateUncertain() noexcept
	{
		g_externalStateUncertain.store(true, std::memory_order_seq_cst);
	}

	bool EndPixelResourceOverride(PixelResourceOverrideToken& a_token) noexcept
	{
		if (!a_token.original) {
			return !a_token.stateUncertain;
		}
		ID3D11ShaderResourceView* observed = nullptr;
		bool restored = !a_token.mutationAttempted;
		__try {
			if (a_token.mutationAttempted && a_token.context && a_token.thread == ::GetCurrentThreadId()) {
				a_token.context->PSSetShaderResources(a_token.slot, 1, &a_token.original);
				a_token.context->PSGetShaderResources(a_token.slot, 1, &observed);
				restored = observed == a_token.original;
			}
			if (observed) {
				observed->Release();
				observed = nullptr;
			}
			if (restored) {
				a_token.original->Release();
				a_token.original = nullptr;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			restored = false;
		}
		a_token.active = false;
		a_token.restored = restored;
		if (!restored) {
			a_token.stateUncertain = true;
			LatchExternalStateUncertain();
		}
		return restored;
	}

	bool BeginPixelResourceOverride(ID3D11DeviceContext* a_context,
		UINT a_slot, ID3D11ShaderResourceView* a_expected,
		ID3D11ShaderResourceView* a_replacement, PixelResourceOverrideToken& a_token) noexcept
	{
		if (a_token.original || a_token.mutationAttempted || !a_context || !a_expected ||
			!a_replacement || a_slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || HostStateUncertain()) {
			return false;
		}
		a_token.context = a_context;
		a_token.slot = a_slot;
		a_token.thread = ::GetCurrentThreadId();
		ID3D11ShaderResourceView* observed = nullptr;
		bool bound = false;
		__try {
			a_context->PSGetShaderResources(a_slot, 1, &a_token.original);
			if (a_token.original == a_expected) {
				a_token.mutationAttempted = true;
				a_context->PSSetShaderResources(a_slot, 1, &a_replacement);
				a_context->PSGetShaderResources(a_slot, 1, &observed);
				bound = observed == a_replacement;
			}
			if (observed) {
				observed->Release();
				observed = nullptr;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			bound = false;
		}
		a_token.active = bound;
		if (!bound) {
			static_cast<void>(EndPixelResourceOverride(a_token));
		}
		return bound;
	}

	bool HostStateUncertain() noexcept
	{
		return g_uncertainContexts.load(std::memory_order_seq_cst) != 0 ||
		       g_outputMergerStateUncertain.load(std::memory_order_seq_cst) ||
		       g_externalStateUncertain.load(std::memory_order_seq_cst);
	}
}
