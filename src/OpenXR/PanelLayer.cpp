#include <Windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "Upscaler/OpenXrPanelBridge.h"
#include "Upscaler/PanelPointer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
	namespace Bridge = Upscaler::OpenXrPanelBridge;
	namespace Pointer = Upscaler::PanelPointer;
	constexpr char kLayerName[] = "XR_APILAYER_ROLLINGROCK_upscaler_panel";
	constexpr std::uint32_t kMaxLayers = 64;
	constexpr XrSpaceLocationFlags kPoseValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

#define PANEL_XR_FUNCTIONS(X) \
	X(DestroyInstance) X(CreateSession) X(DestroySession) X(WaitFrame) X(EndFrame) \
	X(StringToPath) X(CreateActionSet) X(DestroyActionSet) X(CreateAction) \
	X(SuggestInteractionProfileBindings) X(AttachSessionActionSets) X(SyncActions) \
	X(CreateActionSpace) X(CreateReferenceSpace) X(DestroySpace) X(LocateSpace) \
	X(GetActionStateBoolean) X(GetActionStateVector2f) X(GetActionStatePose) \
	X(GetSystemProperties) X(EnumerateSwapchainFormats) X(CreateSwapchain) \
	X(DestroySwapchain) X(EnumerateSwapchainImages) X(AcquireSwapchainImage) \
	X(WaitSwapchainImage) X(ReleaseSwapchainImage)
	struct Functions
	{
#define DECLARE_XR(name) PFN_xr##name name = nullptr;
		PANEL_XR_FUNCTIONS(DECLARE_XR)
#undef DECLARE_XR
	};

	// Session state is shared by the application's XR calls and the plugin's render
	// thread. Plugin entry points only try this lock. EndFrame copies metadata and
	// forwards without holding it; all GPU commands remain in Submit on the plugin's
	// render thread. Session destruction follows the application's XR lifetime fence.
	std::mutex g_mutex;
	Functions g_next;
	PFN_xrGetInstanceProcAddr g_nextGet = nullptr;
	XrInstance g_instance = XR_NULL_HANDLE;
	XrSession g_session = XR_NULL_HANDLE;
	XrActionSet g_actions = XR_NULL_HANDLE;
	XrAction g_aim = XR_NULL_HANDLE;
	XrAction g_select = XR_NULL_HANDLE;
	XrAction g_scroll = XR_NULL_HANDLE;
	XrAction g_close = XR_NULL_HANDLE;
	std::array<XrPath, 2> g_hands{};
	XrPath g_touchProfile = XR_NULL_PATH;
	std::array<XrActionSuggestedBinding, 8> g_bindings{};
	std::array<XrSpace, 2> g_aimSpaces{};
	XrSpace g_local = XR_NULL_HANDLE;
	XrSpace g_view = XR_NULL_HANDLE;
	ID3D11Device* g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;
	XrSwapchain g_swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageD3D11KHR> g_images;
	std::uint32_t g_width = 0;
	std::uint32_t g_height = 0;
	std::uint32_t g_imageIndex = 0;
	std::uint32_t g_maxLayers = 0;
	DWORD g_renderThread = 0;
	bool g_acquired = false;
	bool g_releasedImage = false;
	bool g_attached = false;
	bool g_actionsReady = false;
	bool g_visible = false;
	std::atomic<bool> g_hideRequested{ false };
	bool g_quarantine = false;
	bool g_waitRelease = true;
	bool g_closeHeld = false;
	bool g_failed = false;
	bool g_room = true;
	bool g_placed = false;
	bool g_inputCurrent = false;
	std::array<bool, 2> g_pressed{};
	bool g_closeRequested = false;
	float g_scrollY = 0;
	float g_widthMeters = 0.6f;
	float g_distanceMeters = 1.0f;
	XrTime g_time = 0;
	XrPosef g_pose{ { 0, 0, 0, 1 }, { 0, -0.15f, -1 } };
	std::uint64_t g_generation = 0;
	std::uint64_t g_submissions = 0;
	std::uint64_t g_quadFrames = 0;
	XrResult g_lastResult = XR_SUCCESS;

	[[nodiscard]] bool Good(XrResult a_result) noexcept
	{
		g_lastResult = a_result;
		return XR_SUCCEEDED(a_result);
	}

	[[nodiscard]] bool Ready() noexcept
	{
		return g_session && g_device && g_local && g_view && g_attached && g_maxLayers >= 2 && !g_failed;
	}

	void Hide() noexcept
	{
		g_quarantine = g_quarantine || (g_visible && g_attached);
		g_visible = false;
		g_placed = false;
	}

	void ApplyHide() noexcept
	{
		if (g_hideRequested.exchange(false)) { Hide(); }
	}

	void Fail() noexcept
	{
		Hide();
		g_failed = true;
	}

	[[nodiscard]] Pointer::Pose Pose(const XrPosef& a_pose) noexcept
	{
		return { { a_pose.orientation.x, a_pose.orientation.y, a_pose.orientation.z, a_pose.orientation.w },
			{ a_pose.position.x, a_pose.position.y, a_pose.position.z } };
	}

	[[nodiscard]] bool Located(XrSpace a_space, XrSpace a_base, XrPosef& a_pose) noexcept
	{
		if (!g_time || !a_space || !a_base) {
			return false;
		}
		XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
		if (!Good(g_next.LocateSpace(a_space, a_base, g_time, &location)) ||
			(location.locationFlags & kPoseValid) != kPoseValid) {
			return false;
		}
		a_pose = location.pose;
		return true;
	}

	[[nodiscard]] bool Place() noexcept
	{
		if (g_placed) {
			return true;
		}
		if (!g_room) {
			g_pose = { { 0, 0, 0, 1 }, { 0, -0.15f, -g_distanceMeters } };
		} else {
			XrPosef head{};
			if (!Located(g_view, g_local, head)) {
				return false;
			}
			const auto placed = Pointer::Place(Pose(head), g_distanceMeters);
			g_pose = { { placed.orientation.x, placed.orientation.y, placed.orientation.z, placed.orientation.w },
				{ placed.position.x, placed.position.y, placed.position.z } };
		}
		g_placed = true;
		return true;
	}

	void ClearSession() noexcept
	{
		g_visible = false;
		g_quarantine = false;
		g_hideRequested.store(false);
		g_inputCurrent = false;
		g_attached = false;
		g_placed = false;
		g_failed = false;
		g_waitRelease = true;
		g_closeHeld = false;
		g_closeRequested = false;
		g_pressed = {};
		g_scrollY = 0;
		g_time = 0;
		g_acquired = false;
		g_releasedImage = false;
		g_images.clear();
		if (g_swapchain) { g_next.DestroySwapchain(g_swapchain); }
		g_swapchain = XR_NULL_HANDLE;
		for (auto& space : g_aimSpaces) {
			if (space) { g_next.DestroySpace(space); }
			space = XR_NULL_HANDLE;
		}
		if (g_local) { g_next.DestroySpace(g_local); }
		if (g_view) { g_next.DestroySpace(g_view); }
		g_local = g_view = XR_NULL_HANDLE;
		if (g_context) { g_context->Release(); }
		if (g_device) { g_device->Release(); }
		g_context = nullptr;
		g_device = nullptr;
		g_session = XR_NULL_HANDLE;
		g_renderThread = 0;
		g_width = g_height = 0;
		++g_generation;
	}

	[[nodiscard]] bool CreateActions()
	{
		XrActionSetCreateInfo set{ XR_TYPE_ACTION_SET_CREATE_INFO };
		std::strcpy(set.actionSetName, "fo4vr_upscaler_panel");
		std::strcpy(set.localizedActionSetName, "Fo4VR Upscaler Panel");
		set.priority = 100;
		if (!Good(g_next.CreateActionSet(g_instance, &set, &g_actions))) { return false; }
		if (!Good(g_next.StringToPath(g_instance, "/user/hand/left", &g_hands[0])) ||
			!Good(g_next.StringToPath(g_instance, "/user/hand/right", &g_hands[1])) ||
			!Good(g_next.StringToPath(g_instance, "/interaction_profiles/oculus/touch_controller", &g_touchProfile))) { return false; }
		const auto create = [](const char* a_name, XrActionType a_type, XrAction& a_action) {
			XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
			std::strcpy(info.actionName, a_name);
			std::strcpy(info.localizedActionName, a_name);
			info.actionType = a_type;
			info.countSubactionPaths = 2;
			info.subactionPaths = g_hands.data();
			return Good(g_next.CreateAction(g_actions, &info, &a_action));
		};
		if (!create("aim", XR_ACTION_TYPE_POSE_INPUT, g_aim) ||
			!create("select", XR_ACTION_TYPE_BOOLEAN_INPUT, g_select) ||
			!create("scroll", XR_ACTION_TYPE_VECTOR2F_INPUT, g_scroll) ||
			!create("close", XR_ACTION_TYPE_BOOLEAN_INPUT, g_close)) { return false; }
		const char* paths[]{
			"/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose",
			"/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value",
			"/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick",
			"/user/hand/left/input/y/click", "/user/hand/right/input/b/click",
		};
		const XrAction actions[]{ g_aim, g_aim, g_select, g_select, g_scroll, g_scroll, g_close, g_close };
		for (unsigned i = 0; i < g_bindings.size(); ++i) {
			g_bindings[i].action = actions[i];
			if (!Good(g_next.StringToPath(g_instance, paths[i], &g_bindings[i].binding))) { return false; }
		}
		XrInteractionProfileSuggestedBinding suggestion{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggestion.interactionProfile = g_touchProfile;
		suggestion.countSuggestedBindings = static_cast<std::uint32_t>(g_bindings.size());
		suggestion.suggestedBindings = g_bindings.data();
		return Good(g_next.SuggestInteractionProfileBindings(g_instance, &suggestion));
	}

	[[nodiscard]] bool CreateSessionResources(const XrSessionCreateInfo* a_info)
	{
		const auto* next = static_cast<const XrBaseInStructure*>(a_info->next);
		while (next && next->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) { next = next->next; }
		if (!next) { return false; }
		g_device = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next)->device;
		if (!g_device) { return false; }
		g_device->AddRef();
		XrSystemProperties properties{ XR_TYPE_SYSTEM_PROPERTIES };
		if (!Good(g_next.GetSystemProperties(g_instance, a_info->systemId, &properties))) { return false; }
		g_maxLayers = std::min(properties.graphicsProperties.maxLayerCount, kMaxLayers);
		XrReferenceSpaceCreateInfo reference{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		reference.poseInReferenceSpace.orientation.w = 1;
		reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		if (!Good(g_next.CreateReferenceSpace(g_session, &reference, &g_local))) { return false; }
		reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		if (!Good(g_next.CreateReferenceSpace(g_session, &reference, &g_view))) { return false; }
		for (unsigned i = 0; i < g_aimSpaces.size(); ++i) {
			XrActionSpaceCreateInfo space{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
			space.action = g_aim;
			space.subactionPath = g_hands[i];
			space.poseInActionSpace.orientation.w = 1;
			if (!Good(g_next.CreateActionSpace(g_session, &space, &g_aimSpaces[i]))) { return false; }
		}
		return true;
	}

	XrResult XRAPI_CALL CreateSession(XrInstance a_instance, const XrSessionCreateInfo* a_info, XrSession* a_session)
	{
		const auto result = g_next.CreateSession(a_instance, a_info, a_session);
		if (XR_SUCCEEDED(result) && a_instance == g_instance) {
			std::lock_guard lock(g_mutex);
			if (!g_session && g_actionsReady) {
				g_session = *a_session;
				++g_generation;
				try { g_failed = !CreateSessionResources(a_info); } catch (...) { g_failed = true; }
			}
		}
		return result;
	}

	XrResult XRAPI_CALL DestroySession(XrSession a_session)
	{
		{
			std::lock_guard lock(g_mutex);
			if (a_session == g_session) { ClearSession(); }
		}
		return g_next.DestroySession(a_session);
	}

	XrResult XRAPI_CALL DestroyInstance(XrInstance a_instance)
	{
		{
			std::lock_guard lock(g_mutex);
			if (a_instance == g_instance) {
				ClearSession();
				if (g_actions) { g_next.DestroyActionSet(g_actions); }
				g_actions = XR_NULL_HANDLE;
				g_instance = XR_NULL_HANDLE;
			}
		}
		return g_next.DestroyInstance(a_instance);
	}

	XrResult XRAPI_CALL SuggestBindings(XrInstance a_instance, const XrInteractionProfileSuggestedBinding* a_info)
	{
		if (a_instance == g_instance && g_actionsReady && a_info && a_info->interactionProfile == g_touchProfile) {
			try {
				std::vector<XrActionSuggestedBinding> bindings;
				if (a_info->countSuggestedBindings) {
					bindings.assign(a_info->suggestedBindings, a_info->suggestedBindings + a_info->countSuggestedBindings);
				}
				bindings.insert(bindings.end(), g_bindings.begin(), g_bindings.end());
				auto merged = *a_info;
				merged.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
				merged.suggestedBindings = bindings.data();
				const auto result = g_next.SuggestInteractionProfileBindings(a_instance, &merged);
				if (XR_SUCCEEDED(result)) { return result; }
				std::lock_guard lock(g_mutex);
				g_lastResult = result;
				g_actionsReady = false;
				Fail();
			} catch (...) {}
		}
		return g_next.SuggestInteractionProfileBindings(a_instance, a_info);
	}

	XrResult XRAPI_CALL AttachActions(XrSession a_session, const XrSessionActionSetsAttachInfo* a_info)
	{
		if (a_session == g_session && g_actions && !g_failed && a_info) {
			try {
				std::vector<XrActionSet> sets;
				if (a_info->countActionSets) { sets.assign(a_info->actionSets, a_info->actionSets + a_info->countActionSets); }
				sets.push_back(g_actions);
				auto merged = *a_info;
				merged.countActionSets = static_cast<std::uint32_t>(sets.size());
				merged.actionSets = sets.data();
				const auto result = g_next.AttachSessionActionSets(a_session, &merged);
				std::lock_guard lock(g_mutex);
				g_attached = XR_SUCCEEDED(result);
				g_lastResult = result;
				if (XR_SUCCEEDED(result)) { return result; }
				Fail();
			} catch (...) {}
		}
		return g_next.AttachSessionActionSets(a_session, a_info);
	}

	[[nodiscard]] bool Boolean(XrAction a_action, XrPath a_hand, bool& a_value) noexcept
	{
		XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = a_action;
		info.subactionPath = a_hand;
		XrActionStateBoolean value{ XR_TYPE_ACTION_STATE_BOOLEAN };
		if (!Good(g_next.GetActionStateBoolean(g_session, &info, &value))) { return false; }
		a_value = value.isActive && value.currentState;
		return true;
	}

	XrResult XRAPI_CALL SyncActions(XrSession a_session, const XrActionsSyncInfo* a_info)
	{
		bool modal = false;
		{
			std::lock_guard lock(g_mutex);
			ApplyHide();
			modal = a_session == g_session && g_attached && (g_visible || g_quarantine);
		}
		if (!modal) { return g_next.SyncActions(a_session, a_info); }
		// Only our action set is active while the panel owns input. The game's sets
		// remain attached and resume after the closing trigger/buttons are released.
		const XrActiveActionSet set{ g_actions, XR_NULL_PATH };
		auto merged = *a_info;
		merged.countActiveActionSets = 1;
		merged.activeActionSets = &set;
		const auto result = g_next.SyncActions(a_session, &merged);
		std::lock_guard lock(g_mutex);
		g_inputCurrent = false;
		g_pressed = {};
		g_scrollY = 0;
		if (result != XR_SUCCESS) { g_waitRelease = true; return result; }
		bool close[2]{};
		bool valid = true;
		for (unsigned i = 0; i < 2; ++i) {
			valid = Boolean(g_select, g_hands[i], g_pressed[i]) && valid;
			valid = Boolean(g_close, g_hands[i], close[i]) && valid;
		}
		if (!valid) { g_waitRelease = true; return result; }
		const bool closing = close[0] || close[1];
		const bool held = closing || g_pressed[0] || g_pressed[1];
		if (!held) { g_waitRelease = false; g_quarantine = false; }
		if (closing && !g_closeHeld && !g_waitRelease) { g_closeRequested = true; }
		g_closeHeld = closing;
		XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = g_scroll;
		info.subactionPath = g_hands[1];
		XrActionStateVector2f scroll{ XR_TYPE_ACTION_STATE_VECTOR2F };
		if (Good(g_next.GetActionStateVector2f(g_session, &info, &scroll)) && scroll.isActive) {
			g_scrollY = std::abs(scroll.currentState.y) > 0.2f ? scroll.currentState.y : 0;
		}
		g_inputCurrent = true;
		return result;
	}

	XrResult XRAPI_CALL WaitFrame(XrSession a_session, const XrFrameWaitInfo* a_info, XrFrameState* a_state)
	{
		const auto result = g_next.WaitFrame(a_session, a_info, a_state);
		if (XR_SUCCEEDED(result)) {
			std::lock_guard lock(g_mutex);
			if (a_session == g_session) { g_time = a_state->predictedDisplayTime; }
		}
		return result;
	}

	XrResult XRAPI_CALL EndFrame(XrSession a_session, const XrFrameEndInfo* a_info)
	{
		XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
		bool append = false;
		{
			std::lock_guard lock(g_mutex);
			ApplyHide();
			append = a_session == g_session && a_info && a_info->layerCount > 0 && Ready() && g_visible && g_placed &&
				g_releasedImage && a_info->layerCount < g_maxLayers;
			if (append) {
				quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				quad.space = g_room ? g_local : g_view;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage.swapchain = g_swapchain;
				quad.subImage.imageRect.extent = { static_cast<std::int32_t>(g_width), static_cast<std::int32_t>(g_height) };
				quad.pose = g_pose;
				quad.size = { g_widthMeters, g_widthMeters * static_cast<float>(g_height) / static_cast<float>(g_width) };
				++g_quadFrames;
			}
		}
		if (!append) { return g_next.EndFrame(a_session, a_info); }
		std::array<const XrCompositionLayerBaseHeader*, kMaxLayers> layers{};
		for (std::uint32_t i = 0; i < a_info->layerCount; ++i) { layers[i] = a_info->layers[i]; }
		layers[a_info->layerCount] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
		auto frame = *a_info;
		++frame.layerCount;
		frame.layers = layers.data();
		return g_next.EndFrame(a_session, &frame);
	}

	[[nodiscard]] bool Status(Bridge::Status* a_status) noexcept
	{
		if (!a_status || a_status->size != sizeof(*a_status)) { return false; }
		std::unique_lock lock(g_mutex, std::try_to_lock);
		if (!lock.owns_lock()) { return false; }
		ApplyHide();
		a_status->ready = Ready();
		a_status->visible = g_visible;
		a_status->inputAttached = g_attached;
		a_status->lastResult = g_lastResult;
		a_status->generation = g_generation;
		a_status->submissions = g_submissions;
		a_status->quadFrames = g_quadFrames;
		return true;
	}

	[[nodiscard]] bool Show(bool a_visible, float a_width, float a_distance, bool a_room) noexcept
	{
		// Teardown must be able to request a hide even while an XR call owns the lock.
		if (!a_visible) { g_hideRequested.store(true); }
		std::unique_lock lock(g_mutex, std::try_to_lock);
		if (!lock.owns_lock()) { return !a_visible; }
		ApplyHide();
		if (!a_visible) {
			return true;
		}
		if (!Ready() || !std::isfinite(a_width) || !std::isfinite(a_distance)) { return false; }
		if (!g_visible) {
			g_waitRelease = true;
			g_inputCurrent = false;
			g_closeRequested = false;
			g_closeHeld = false;
			g_placed = false;
			g_widthMeters = std::clamp(a_width, 0.2f, 2.0f);
			g_distanceMeters = std::clamp(a_distance, 0.25f, 3.0f);
			g_room = a_room;
		}
		g_visible = true;
		return true;
	}

	[[nodiscard]] bool Input(Bridge::Input* a_input) noexcept
	{
		if (!a_input || a_input->size != sizeof(*a_input)) { return false; }
		std::unique_lock lock(g_mutex, std::try_to_lock);
		if (!lock.owns_lock()) { return false; }
		ApplyHide();
		if (!Ready() || !g_visible) { return false; }
		*a_input = {};
		a_input->close = g_closeRequested;
		g_closeRequested = false;
		if (!Place() || !g_inputCurrent || g_waitRelease) { return true; }
		for (const unsigned hand : { 1u, 0u }) {
			XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
			info.action = g_aim;
			info.subactionPath = g_hands[hand];
			XrActionStatePose poseState{ XR_TYPE_ACTION_STATE_POSE };
			XrPosef aim{};
			if (!Good(g_next.GetActionStatePose(g_session, &info, &poseState)) || !poseState.isActive ||
				!Located(g_aimSpaces[hand], g_room ? g_local : g_view, aim)) { continue; }
			const float height = g_width ? g_widthMeters * static_cast<float>(g_height) / static_cast<float>(g_width) : g_widthMeters;
			const auto hit = Pointer::Intersect(Pose(aim), Pose(g_pose), g_widthMeters, height);
			if (!hit.valid && g_pressed[hand]) { g_waitRelease = true; }
			a_input->valid = hit.valid;
			a_input->hover = hit.hover;
			a_input->x = hit.x;
			a_input->y = hit.y;
			a_input->pressed = hit.valid && g_pressed[hand];
			a_input->scroll = hit.hover ? g_scrollY : 0;
			return true;
		}
		g_waitRelease = true;
		return true;
	}

	[[nodiscard]] bool CreateSwapchain(std::uint32_t a_width, std::uint32_t a_height)
	{
		std::uint32_t count = 0;
		if (!Good(g_next.EnumerateSwapchainFormats(g_session, 0, &count, nullptr)) || count == 0 || count > 128) { return false; }
		std::vector<std::int64_t> formats(count);
		if (!Good(g_next.EnumerateSwapchainFormats(g_session, count, &count, formats.data()))) { return false; }
		std::int64_t format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		if (std::find(formats.begin(), formats.end(), format) == formats.end()) { return false; }
		XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		info.format = format;
		info.sampleCount = info.arraySize = info.faceCount = info.mipCount = 1;
		info.width = a_width;
		info.height = a_height;
		if (!Good(g_next.CreateSwapchain(g_session, &info, &g_swapchain))) { return false; }
		if (!Good(g_next.EnumerateSwapchainImages(g_swapchain, 0, &count, nullptr)) || count == 0 || count > 16) { return false; }
		g_images.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		if (!Good(g_next.EnumerateSwapchainImages(g_swapchain, count, &count,
			reinterpret_cast<XrSwapchainImageBaseHeader*>(g_images.data())))) { return false; }
		g_width = a_width;
		g_height = a_height;
		return true;
	}

	[[nodiscard]] bool SubmitImpl(ID3D11Texture2D* a_texture)
	{
		ApplyHide();
		if (!Ready() || !g_visible || !a_texture) { return false; }
		const auto thread = ::GetCurrentThreadId();
		if (g_renderThread && g_renderThread != thread) { return false; }
		g_renderThread = thread;
		ID3D11Device* owner = nullptr;
		a_texture->GetDevice(&owner);
		const bool same = owner == g_device;
		if (owner) { owner->Release(); }
		if (!same) { return false; }
		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);
		if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.SampleDesc.Count != 1 ||
			desc.ArraySize != 1 || desc.MipLevels != 1 || desc.Width == 0 || desc.Height == 0) { return false; }
		if (!g_context) { g_device->GetImmediateContext(&g_context); }
		if (!g_context || !Place()) { return false; }
		if (!g_swapchain && !CreateSwapchain(desc.Width, desc.Height)) { Fail(); return false; }
		if (desc.Width != g_width || desc.Height != g_height) { return false; }
		if (!g_acquired) {
			XrSwapchainImageAcquireInfo acquire{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			if (!Good(g_next.AcquireSwapchainImage(g_swapchain, &acquire, &g_imageIndex))) { return false; }
			g_acquired = true;
		}
		XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = 0;
		const auto waited = g_next.WaitSwapchainImage(g_swapchain, &wait);
		g_lastResult = waited;
		if (waited == XR_TIMEOUT_EXPIRED) { return false; }
		if (XR_FAILED(waited) || g_imageIndex >= g_images.size()) { Fail(); return false; }
		g_context->CopyResource(g_images[g_imageIndex].texture, a_texture);
		XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		if (!Good(g_next.ReleaseSwapchainImage(g_swapchain, &release))) { Fail(); return false; }
		g_acquired = false;
		g_releasedImage = true;
		++g_submissions;
		return true;
	}

	[[nodiscard]] bool SubmitCpp(ID3D11Texture2D* a_texture) noexcept
	{
		try { return SubmitImpl(a_texture); } catch (...) { Fail(); return false; }
	}

	[[nodiscard]] bool Submit(ID3D11Texture2D* a_texture) noexcept
	{
		if (!g_mutex.try_lock()) { return false; }
		bool submitted = false;
		__try {
			__try { submitted = SubmitCpp(a_texture); }
			__except (EXCEPTION_EXECUTE_HANDLER) { Fail(); }
		} __finally { g_mutex.unlock(); }
		return submitted;
	}

	XrResult XRAPI_CALL GetProc(XrInstance a_instance, const char* a_name, PFN_xrVoidFunction* a_function)
	{
		if (!a_name || !a_function) { return XR_ERROR_VALIDATION_FAILURE; }
#define HOOK_XR(name, hook) if (std::strcmp(a_name, "xr" #name) == 0 && g_next.name) { *a_function = reinterpret_cast<PFN_xrVoidFunction>(hook); return XR_SUCCESS; }
		HOOK_XR(CreateSession, CreateSession)
		HOOK_XR(DestroySession, DestroySession)
		HOOK_XR(DestroyInstance, DestroyInstance)
		HOOK_XR(WaitFrame, WaitFrame)
		HOOK_XR(EndFrame, EndFrame)
		HOOK_XR(SuggestInteractionProfileBindings, SuggestBindings)
		HOOK_XR(AttachSessionActionSets, AttachActions)
		HOOK_XR(SyncActions, SyncActions)
#undef HOOK_XR
		if (std::strcmp(a_name, "xrGetInstanceProcAddr") == 0) {
			*a_function = reinterpret_cast<PFN_xrVoidFunction>(GetProc);
			return XR_SUCCESS;
		}
		return g_nextGet ? g_nextGet(a_instance, a_name, a_function) : XR_ERROR_FUNCTION_UNSUPPORTED;
	}

	XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* a_info,
		const XrApiLayerCreateInfo* a_layer, XrInstance* a_instance)
	{
		if (!a_layer || !a_layer->nextInfo || !a_layer->nextInfo->nextGetInstanceProcAddr ||
			!a_layer->nextInfo->nextCreateApiLayerInstance) { return XR_ERROR_INITIALIZATION_FAILED; }
		auto forwarded = *a_layer;
		forwarded.nextInfo = a_layer->nextInfo->next;
		const auto result = a_layer->nextInfo->nextCreateApiLayerInstance(a_info, &forwarded, a_instance);
		if (XR_FAILED(result)) { return result; }
		g_nextGet = a_layer->nextInfo->nextGetInstanceProcAddr;
		g_instance = *a_instance;
		g_next = {};
		g_actionsReady = false;
		bool complete = true;
#define RESOLVE_XR(name) complete = XR_SUCCEEDED(g_nextGet(g_instance, "xr" #name, reinterpret_cast<PFN_xrVoidFunction*>(&g_next.name))) && g_next.name && complete;
		PANEL_XR_FUNCTIONS(RESOLVE_XR)
#undef RESOLVE_XR
		if (complete) {
			try { g_actionsReady = CreateActions(); } catch (...) { g_actionsReady = false; }
			g_failed = !g_actionsReady;
		} else { g_failed = true; }
		return result;
	}
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
	const XrNegotiateLoaderInfo* a_loader, const char* a_name, XrNegotiateApiLayerRequest* a_request)
{
	if (!a_loader || !a_request || !a_name || std::strcmp(a_name, kLayerName) != 0 ||
		a_loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		a_loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
		a_loader->structSize != sizeof(XrNegotiateLoaderInfo) ||
		a_request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
		a_request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
		a_request->structSize != sizeof(XrNegotiateApiLayerRequest) ||
		a_loader->minInterfaceVersion > 1 || a_loader->maxInterfaceVersion < 1 ||
		a_loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0)) { return XR_ERROR_INITIALIZATION_FAILED; }
	a_request->layerInterfaceVersion = 1;
	a_request->layerApiVersion = XR_MAKE_VERSION(1, 0, 0);
	a_request->getInstanceProcAddr = GetProc;
	a_request->createApiLayerInstance = CreateInstance;
	return XR_SUCCESS;
}

extern "C" __declspec(dllexport) const Bridge::Api* Fo4VRUpscaler_OpenXRPanelApi(
	std::uint32_t a_version, std::uint32_t a_size) noexcept
{
	static const Bridge::Api api{ Bridge::kVersion, sizeof(Bridge::Api), Status, Show, Input, Submit };
	return a_version == api.version && a_size == api.size ? &api : nullptr;
}
