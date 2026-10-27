#include "Upscaler/OpenXrPanelBridge.h"
#include "Upscaler/PanelPointer.h"
#include <Windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace
{
	using Microsoft::WRL::ComPtr;
	namespace Bridge = Upscaler::OpenXrPanelBridge;
	namespace Pointer = Upscaler::PanelPointer;
	const XrInstance kInstance = reinterpret_cast<XrInstance>(1);
	const XrSession kSession = reinterpret_cast<XrSession>(2);
	const XrActionSet kGameSet = reinterpret_cast<XrActionSet>(3);
	const XrSwapchain kSwapchain = reinterpret_cast<XrSwapchain>(4);
	std::uintptr_t g_handle = 16;
	std::map<XrAction, std::string> g_actions;
	std::map<std::string, XrPath> g_paths;
	ComPtr<ID3D11Device> g_device;
	ComPtr<ID3D11DeviceContext> g_context;
	std::array<ComPtr<ID3D11Texture2D>, 2> g_images;
	unsigned g_waits = 0;
	unsigned g_acquires = 0;
	unsigned g_releases = 0;
	unsigned g_syncCount = 0;
	unsigned g_bindingCount = 0;
	unsigned g_attachedCount = 0;
	unsigned g_destroyed = 0;
	bool g_timeout = false;
	bool g_waitFault = false;
	bool g_pressed = false;
	bool g_close = false;
	bool g_poseValid = true;
	bool g_rejectBindings = false;
	bool g_rejectAttach = false;
	XrActionSet g_activeSet = XR_NULL_HANDLE;
	XrCompositionLayerQuad g_quad{};
	std::uint32_t g_layerCount = 0;
	const XrCompositionLayerBaseHeader* g_firstLayer = nullptr;
	XrQuaternionf g_orientation{ 0, 0, 0, 1 };
	float g_scroll = 0;

	void Check(bool a_result, const char* a_reason)
	{
		if (!a_result) {
			std::cerr << "FAIL: " << a_reason << '\n';
			std::exit(1);
		}
	}
	bool Near(float a_a, float a_b) { return std::abs(a_a - a_b) < 0.0001f; }
	XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo* a_info, XrInstance* a_instance)
	{
		Check(a_info->nextInfo == nullptr, "layer chain advances exactly once");
		*a_instance = kInstance;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL DestroyInstance(XrInstance) { return XR_SUCCESS; }
	XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* a_session)
	{
		*a_session = kSession;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL DestroySession(XrSession)
	{
		++g_destroyed;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL WaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* a_state)
	{
		a_state->predictedDisplayTime = 1000000;
		a_state->shouldRender = true;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* a_info)
	{
		g_layerCount = a_info->layerCount;
		g_firstLayer = a_info->layerCount ? a_info->layers[0] : nullptr;
		if (a_info->layerCount > 1) {
			Check(a_info->layers[a_info->layerCount - 1]->type == XR_TYPE_COMPOSITION_LAYER_QUAD, "quad appended after scene");
			g_quad = *reinterpret_cast<const XrCompositionLayerQuad*>(a_info->layers[a_info->layerCount - 1]);
		}
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL StringToPath(XrInstance, const char* a_name, XrPath* a_path)
	{
		auto& path = g_paths[a_name];
		if (!path) {
			path = ++g_handle;
		}
		*a_path = path;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL CreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* a_set)
	{
		*a_set = reinterpret_cast<XrActionSet>(++g_handle);
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL DestroyActionSet(XrActionSet) { return XR_SUCCESS; }
	XrResult XRAPI_CALL CreateAction(XrActionSet, const XrActionCreateInfo* a_info, XrAction* a_action)
	{
		*a_action = reinterpret_cast<XrAction>(++g_handle);
		g_actions[*a_action] = a_info->actionName;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL SuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding* a_info)
	{
		g_bindingCount = a_info->countSuggestedBindings;
		return g_rejectBindings && g_bindingCount > 1 ? XR_ERROR_PATH_UNSUPPORTED : XR_SUCCESS;
	}
	XrResult XRAPI_CALL AttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo* a_info)
	{
		g_attachedCount = a_info->countActionSets;
		return g_rejectAttach && g_attachedCount > 1 ? XR_ERROR_ACTIONSETS_ALREADY_ATTACHED : XR_SUCCESS;
	}
	XrResult XRAPI_CALL SyncActions(XrSession, const XrActionsSyncInfo* a_info)
	{
		++g_syncCount;
		g_activeSet = a_info->countActiveActionSets ? a_info->activeActionSets[0].actionSet : XR_NULL_HANDLE;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL CreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* a_space)
	{
		*a_space = reinterpret_cast<XrSpace>(++g_handle);
		return XR_SUCCESS;
	}
	XrSpace g_viewSpace = XR_NULL_HANDLE;
	XrResult XRAPI_CALL CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* a_info, XrSpace* a_space)
	{
		*a_space = reinterpret_cast<XrSpace>(++g_handle);
		if (a_info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
			g_viewSpace = *a_space;
		}
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL DestroySpace(XrSpace) { return XR_SUCCESS; }
	XrResult XRAPI_CALL LocateSpace(XrSpace a_space, XrSpace, XrTime, XrSpaceLocation* a_location)
	{
		a_location->locationFlags = g_poseValid ? XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT : 0;
		a_location->pose = { g_orientation, { 0, a_space == g_viewSpace ? 0 : -0.15f, 0 } };
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL GetActionStateBoolean(XrSession, const XrActionStateGetInfo* a_info, XrActionStateBoolean* a_state)
	{
		a_state->isActive = true;
		a_state->currentState = g_actions[a_info->action] == "select" ? g_pressed : g_close;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL GetActionStateVector2f(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f* a_state)
	{
		a_state->isActive = true;
		a_state->currentState.y = g_scroll;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL GetActionStatePose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* a_state)
	{
		a_state->isActive = true;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* a_properties)
	{
		a_properties->graphicsProperties.maxLayerCount = 16;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL EnumerateSwapchainFormats(XrSession, std::uint32_t a_capacity, std::uint32_t* a_count, std::int64_t* a_formats)
	{
		*a_count = 1;
		if (a_capacity) {
			a_formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		}
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL CreateSwapchain(XrSession, const XrSwapchainCreateInfo* a_info, XrSwapchain* a_swapchain)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_info->width;
		desc.Height = a_info->height;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = static_cast<DXGI_FORMAT>(a_info->format);
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		for (auto& image : g_images) {
			Check(SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &image)), "runtime mock image");
		}
		*a_swapchain = kSwapchain;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL DestroySwapchain(XrSwapchain)
	{
		for (auto& image : g_images) {
			image.Reset();
		}
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL EnumerateSwapchainImages(XrSwapchain, std::uint32_t a_capacity, std::uint32_t* a_count, XrSwapchainImageBaseHeader* a_images)
	{
		*a_count = 2;
		if (a_capacity) {
			auto* images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(a_images);
			for (unsigned i = 0; i < 2; ++i) {
				Check(images[i].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, "image type");
				images[i].texture = g_images[i].Get();
			}
		}
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL AcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* a_index)
	{
		++g_acquires;
		*a_index = 0;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL WaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo* a_info)
	{
		++g_waits;
		Check(a_info->timeout == 0, "render thread never waits for XR image");
		if (g_waitFault) {
			::RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
		}
		return g_timeout ? XR_TIMEOUT_EXPIRED : XR_SUCCESS;
	}
	XrResult XRAPI_CALL ReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*)
	{
		++g_releases;
		return XR_SUCCESS;
	}
	XrResult XRAPI_CALL NextGet(XrInstance, const char* a_name, PFN_xrVoidFunction* a_function)
	{
		// clang-format off
#define FUNCTION(name) if (std::strcmp(a_name, "xr" #name) == 0) { *a_function = reinterpret_cast<PFN_xrVoidFunction>(name); return XR_SUCCESS; }
		FUNCTION(DestroyInstance) FUNCTION(CreateSession) FUNCTION(DestroySession) FUNCTION(WaitFrame) FUNCTION(EndFrame)
		FUNCTION(StringToPath) FUNCTION(CreateActionSet) FUNCTION(DestroyActionSet) FUNCTION(CreateAction)
		FUNCTION(SuggestInteractionProfileBindings) FUNCTION(AttachSessionActionSets) FUNCTION(SyncActions)
		FUNCTION(CreateActionSpace) FUNCTION(CreateReferenceSpace) FUNCTION(DestroySpace) FUNCTION(LocateSpace)
		FUNCTION(GetActionStateBoolean) FUNCTION(GetActionStateVector2f) FUNCTION(GetActionStatePose)
		FUNCTION(GetSystemProperties) FUNCTION(EnumerateSwapchainFormats) FUNCTION(CreateSwapchain)
		FUNCTION(DestroySwapchain) FUNCTION(EnumerateSwapchainImages) FUNCTION(AcquireSwapchainImage)
		FUNCTION(WaitSwapchainImage) FUNCTION(ReleaseSwapchainImage)
#undef FUNCTION
										// clang-format on
										* a_function = nullptr;
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	}
	template <class T>
	T Get(PFN_xrGetInstanceProcAddr a_get, const char* a_name)
	{
		PFN_xrVoidFunction function = nullptr;
		Check(a_get(kInstance, a_name, &function) == XR_SUCCESS && function, a_name);
		return reinterpret_cast<T>(function);
	}
}

int main(int a_count, char** a_args)
{
	Check(a_count == 2, "pass actual layer DLL path");
	const auto module = ::LoadLibraryA(a_args[1]);
	Check(module != nullptr, "load production layer");
	const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(::GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
	const auto getApi = reinterpret_cast<Bridge::GetApi>(::GetProcAddress(module, "Fo4VRUpscaler_OpenXRPanelApi"));
	Check(negotiate && getApi, "layer exports");
	Check(!getApi(2, sizeof(Bridge::Api)) && !getApi(1, 0), "bridge rejects incompatible ABI");
	const auto* api = getApi(1, sizeof(Bridge::Api));
	Check(api != nullptr, "bridge ABI");
	XrNegotiateLoaderInfo loader{ XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(loader), 1, 1, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 1, 0) };
	XrNegotiateApiLayerRequest request{ XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(request) };
	Check(negotiate(&loader, "wrong", &request) == XR_ERROR_INITIALIZATION_FAILED, "reject wrong layer name");
	Check(negotiate(&loader, "XR_APILAYER_ROLLINGROCK_upscaler_panel", &request) == XR_SUCCESS, "negotiate layer");
	XrApiLayerNextInfo next{};
	next.nextGetInstanceProcAddr = NextGet;
	next.nextCreateApiLayerInstance = CreateInstance;
	XrApiLayerCreateInfo chain{};
	chain.nextInfo = &next;
	XrInstanceCreateInfo create{ XR_TYPE_INSTANCE_CREATE_INFO };
	XrInstance instance = XR_NULL_HANDLE;
	Check(request.createApiLayerInstance(&create, &chain, &instance) == XR_SUCCESS && instance == kInstance, "preserve instance result");
	Check(g_bindingCount == 8, "own bindings registered");
	const auto get = request.getInstanceProcAddr;
	const auto suggest = Get<PFN_xrSuggestInteractionProfileBindings>(get, "xrSuggestInteractionProfileBindings");
	XrActionSuggestedBinding gameBinding{ reinterpret_cast<XrAction>(9), 7 };
	XrInteractionProfileSuggestedBinding suggestions{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggestions.interactionProfile = g_paths["/interaction_profiles/oculus/touch_controller"];
	suggestions.countSuggestedBindings = 1;
	suggestions.suggestedBindings = &gameBinding;
	Check(suggest(instance, &suggestions) == XR_SUCCESS && g_bindingCount == 9 && suggestions.countSuggestedBindings == 1, "merge bindings without changing caller");
	Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &g_context)), "WARP device");
	XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	binding.device = g_device.Get();
	XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = 1;
	XrSession session = XR_NULL_HANDLE;
	Check(Get<PFN_xrCreateSession>(get, "xrCreateSession")(instance, &sessionInfo, &session) == XR_SUCCESS, "create session");
	XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &kGameSet;
	Check(Get<PFN_xrAttachSessionActionSets>(get, "xrAttachSessionActionSets")(session, &attach) == XR_SUCCESS && g_attachedCount == 2, "append menu action set");
	Bridge::Status status{};
	Check(api->status(&status) && status.ready, "session ready");
	const auto generation = status.generation;
	const auto sync = Get<PFN_xrSyncActions>(get, "xrSyncActions");
	XrActiveActionSet active{ kGameSet, XR_NULL_PATH };
	XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	sync(session, &syncInfo);
	Check(g_activeSet == kGameSet, "hidden menu leaves gameplay actions active");
	XrFrameState frameState{ XR_TYPE_FRAME_STATE };
	Get<PFN_xrWaitFrame>(get, "xrWaitFrame")(session, nullptr, &frameState);
	Check(api->show(true, 0.6f, 1.0f, true), "show panel");
	g_pressed = true;
	sync(session, &syncInfo);
	Bridge::Input input{};
	Check(api->input(&input) && !input.pressed && g_activeSet != kGameSet, "opening held trigger blocked; gameplay inactive");
	g_pressed = false;
	sync(session, &syncInfo);
	Check(api->input(&input) && input.hover && Near(input.x, 0.5f) && Near(input.y, 0.5f), "right aim hits room panel centre");
	g_pressed = true;
	g_scroll = 0.8f;
	sync(session, &syncInfo);
	Check(api->input(&input) && input.pressed && Near(input.scroll, 0.8f), "trigger and scroll input");
	g_poseValid = false;
	Check(api->input(&input) && !input.valid && !input.pressed, "tracking loss releases pointer");
	g_poseValid = true;
	Check(api->input(&input) && !input.pressed, "held trigger suppressed after tracking recovery");
	g_pressed = false;
	g_scroll = 0;
	sync(session, &syncInfo);

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = 64;
	desc.Height = 32;
	desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	ComPtr<ID3D11Texture2D> texture;
	Check(SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &texture)), "panel texture");
	ComPtr<ID3D11RenderTargetView> rtv;
	Check(SUCCEEDED(g_device->CreateRenderTargetView(texture.Get(), nullptr, &rtv)), "panel RTV");
	const float color[]{ 1, 0, 0, 1 };
	g_context->ClearRenderTargetView(rtv.Get(), color);
	g_timeout = true;
	Check(!api->submit(texture.Get()) && g_acquires == 1 && g_releases == 0, "zero wait timeout retains acquired image");
	g_timeout = false;
	Check(api->submit(texture.Get()) && g_acquires == 1 && g_waits == 2 && g_releases == 1, "retry waits same image and releases once");
	ComPtr<ID3D11Texture2D> readback;
	desc.BindFlags = 0;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	Check(SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &readback)), "readback texture");
	g_context->CopyResource(readback.Get(), g_images[0].Get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	Check(SUCCEEDED(g_context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "test readback");
	const auto* pixel = static_cast<const unsigned char*>(mapped.pData);
	Check(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255, "production copy reaches XR image");
	g_context->Unmap(readback.Get(), 0);
	XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	const XrCompositionLayerBaseHeader* original = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
	XrFrameEndInfo frame{ XR_TYPE_FRAME_END_INFO };
	frame.layerCount = 1;
	frame.layers = &original;
	frame.displayTime = 1000000;
	const auto end = Get<PFN_xrEndFrame>(get, "xrEndFrame");
	Check(end(session, &frame) == XR_SUCCESS && g_layerCount == 2 && g_firstLayer == original && frame.layerCount == 1, "quad preserves projection and original frame");
	Check(g_quad.eyeVisibility == XR_EYE_VISIBILITY_BOTH && Near(g_quad.size.width, 0.6f) && Near(g_quad.size.height, 0.3f), "stereo quad aspect");
	Check(Near(g_quad.pose.position.z, -1) && Near(g_quad.pose.position.y, -0.15f), "room placement");
	std::thread generated([&] { Check(end(session, &frame) == XR_SUCCESS, "generated frame forward"); });
	generated.join();
	Check(g_layerCount == 2, "quad retained on generated frame caller");
	bool wrongThread = true;
	std::thread other([&] { wrongThread = api->submit(texture.Get()); });
	other.join();
	Check(!wrongThread, "GPU submission stays on owning render thread");

	g_close = true;
	sync(session, &syncInfo);
	Check(api->input(&input) && input.close, "B/Y close edge");
	Check(api->input(&input) && !input.close, "close edge consumed once");
	api->show(false, 0, 0, true);
	sync(session, &syncInfo);
	Check(g_activeSet != kGameSet, "held closing button quarantined");
	api->show(false, 0, 0, true);
	sync(session, &syncInfo);
	Check(g_activeSet != kGameSet, "repeated hide retains quarantine");
	g_close = false;
	sync(session, &syncInfo);
	sync(session, &syncInfo);
	Check(g_activeSet == kGameSet, "gameplay resumes after release");
	end(session, &frame);
	Check(g_layerCount == 1, "hidden panel adds no quad");
	g_orientation = { 0, -0.70710678f, 0, 0.70710678f };
	api->show(true, 0.6f, 1, true);
	sync(session, &syncInfo);
	Check(api->input(&input) && input.hover, "turned room placement and ray agree");
	api->submit(texture.Get());
	end(session, &frame);
	Check(Near(g_quad.pose.position.x, 1) && Near(g_quad.pose.orientation.y, -0.70710678f), "quad keeps yaw orientation");
	g_waitFault = true;
	g_pressed = true;
	sync(session, &syncInfo);
	Check(!api->submit(texture.Get()), "GPU-side SEH declined");
	g_waitFault = false;
	Check(api->status(&status) && !status.ready, "SEH releases lock and latches panel only");
	Check(end(session, &frame) == XR_SUCCESS && g_layerCount == 1, "panel failure preserves frame forwarding");
	sync(session, &syncInfo);
	Check(g_activeSet != kGameSet, "panel failure still quarantines held trigger");
	g_pressed = false;
	sync(session, &syncInfo);
	sync(session, &syncInfo);
	Check(g_activeSet == kGameSet, "panel failure returns gameplay after release");
	Check(Get<PFN_xrDestroySession>(get, "xrDestroySession")(session) == XR_SUCCESS && g_destroyed == 1, "session cleanup");
	Check(api->status(&status) && !status.ready && status.generation > generation, "destroy invalidates bridge generation");
	Check(Get<PFN_xrCreateSession>(get, "xrCreateSession")(instance, &sessionInfo, &session) == XR_SUCCESS, "recreate session");
	g_rejectAttach = true;
	Check(Get<PFN_xrAttachSessionActionSets>(get, "xrAttachSessionActionSets")(session, &attach) == XR_SUCCESS && g_attachedCount == 1, "menu attach rejection preserves game attach");
	Check(api->status(&status) && !status.ready, "attach failure disables menu only");
	Get<PFN_xrDestroySession>(get, "xrDestroySession")(session);
	g_rejectBindings = true;
	Check(suggest(instance, &suggestions) == XR_SUCCESS && g_bindingCount == 1, "menu binding rejection preserves game bindings");
	Check(Get<PFN_xrDestroyInstance>(get, "xrDestroyInstance")(instance) == XR_SUCCESS, "instance cleanup");
	const auto back = Pointer::Intersect({ { 0, 1, 0, 0 }, { 0, 0, 0 } }, { { 0, 0, 0, 1 }, { 0, 0, -1 } }, 1, 1);
	Check(!back.valid, "backwards pointer refused");
	std::cout << "PASS: production OpenXR layer negotiation, input, WARP image copy, frame chaining, lifetime and fault tests\n";
}
