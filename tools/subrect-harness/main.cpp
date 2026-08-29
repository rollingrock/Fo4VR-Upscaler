// Standalone sub-rect harness. It creates its own D3D11 device and drives the shipping
// backend directly - no game, no script extender, no VR runtime, no headset.
//
// The question it exists to settle: FO4VR presents stereo as one double-wide target, so
// either the reconstruction runtime accepts a non-zero output sub-rect base or every
// frame pays two full-resolution copies per eye. Nothing in the research corpus has been
// observed doing a non-zero sub-rect NGX D3D11 evaluate, so the shipping path assumes the
// copies and Caps::supportsSubrects stays false. This harness is what would let that
// assumption be dropped, and it is a proof about the code that ships because it links the
// same backend library the plugin does.
//
// The seam headers are included on both branches on purpose: with no SDK staged the
// harness still type-checks against upscaler_backends, so the green no-SDK build means
// something.

#include "Upscaler/Backend/BackendTypes.h"
#include "Upscaler/Backend/IBackend.h"

#ifdef UPSCALER_NO_NGX

#	include <cstdio>

int main()
{
	std::puts("Sub-rect harness: no NGX SDK is staged, so there is nothing to exercise.");
	std::puts("Stage one with scripts/fetch-sdks.ps1, then configure with -DUPSCALER_ENABLE_NGX=ON.");
	std::puts("Building and running this far with no vendor SDK present is the result being reported.");
	return 0;
}

#else

#	include <catch2/catch_test_macros.hpp>

#	include <iterator>
#	include <wrl/client.h>

namespace
{
	using Microsoft::WRL::ComPtr;

	// The device the whole harness runs on. No swapchain and no window: every result is
	// read back rather than presented, which is also what lets this run unattended.
	bool MakeDevice(ComPtr<ID3D11Device>& a_device, ComPtr<ID3D11DeviceContext>& a_immediate)
	{
		UINT flags = 0;
#	ifdef UPSCALER_HARNESS_D3D_DEBUG
		flags |= D3D11_CREATE_DEVICE_DEBUG;
#	endif

		const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

		const HRESULT hr = D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			flags,
			wanted,
			static_cast<UINT>(std::size(wanted)),
			D3D11_SDK_VERSION,
			a_device.GetAddressOf(),
			&obtained,
			a_immediate.GetAddressOf());

		return SUCCEEDED(hr) && a_device && a_immediate;
	}
}

TEST_CASE("The runtime initialises outside a game process", "[ngx][init]")
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> immediate;
	REQUIRE(MakeDevice(device, immediate));

	SKIP("Not written yet. Probe and Initialize the backend on this device and require Caps::available.");
}

TEST_CASE("Two feature handles share one device with independent histories", "[ngx][stereo]")
{
	SKIP("Not written yet. CreateViews with a view count of two, both with output sub-rects enabled at creation.");
}

TEST_CASE("An evaluate into the right half of a double-wide target writes only that half", "[ngx][subrect]")
{
	// The load-bearing one. Everything else here is scaffolding for this test.
	SKIP("Not written yet. Evaluate with every sub-rect base at the right half of a double-wide checkerboard, then read back and assert the left half is untouched.");
}

TEST_CASE("Immediate-context state survives an evaluate unchanged", "[ngx][state]")
{
	// Verified rather than trusted. The vendor documents this guarantee for D3D11
	// specifically, and it is the single fact that makes mid-frame injection into a
	// cached engine state machine safe at all.
	SKIP("Not written yet. Snapshot render targets, depth-stencil view, shader-resource slots, shader bindings, viewport and scissor either side of the evaluate and compare.");
}

TEST_CASE("A non-zero sub-rect base without sub-rects enabled fails as documented", "[ngx][negative]")
{
	// The negative control. If this succeeds instead, the documentation is stale, the
	// per-eye intermediate copies become an optimisation rather than a requirement, and
	// the saving is two full-resolution copies per eye per frame.
	SKIP("Not written yet. Same evaluate with output sub-rects not enabled at creation; expect the documented failure.");
}

#endif
