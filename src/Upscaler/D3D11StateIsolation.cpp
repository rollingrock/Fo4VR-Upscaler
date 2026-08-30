#include "Upscaler/D3D11StateIsolation.h"

#include <Windows.h>

#include <utility>

namespace Upscaler::D3D11StateIsolation
{
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
		if (_active) {
			static_cast<void>(End());
		}
		_hostState.Reset();
		_privateState.Reset();
		_context1.Reset();
		_chosenFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(0);
		_activeThread = 0;
		_active = false;
	}

	bool Context::Begin() noexcept
	{
		if (!Ready() || _active) {
			_lastError = E_UNEXPECTED;
			return false;
		}

		const std::uint32_t thread = ::GetCurrentThreadId();
		if (_activeThread != 0 && _activeThread != thread) {
			_lastError = RPC_E_WRONG_THREAD;
			return false;
		}

		_hostState.Reset();
		_context1->SwapDeviceContextState(_privateState.Get(), _hostState.GetAddressOf());
		if (!_hostState) {
			_lastError = E_FAIL;
			return false;
		}

		_activeThread = thread;
		_active = true;
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
		_context1->SwapDeviceContextState(_hostState.Get(), privateState.GetAddressOf());
		_hostState.Reset();
		_active = false;

		if (!privateState) {
			_privateState.Reset();
			_lastError = E_FAIL;
			return false;
		}

		_privateState = std::move(privateState);
		_lastError = S_OK;
		return true;
	}
}
