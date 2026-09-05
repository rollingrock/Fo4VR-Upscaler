#include "Upscaler/Panel.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <d3d11.h>

#include "Settings/Configuration.h"
#include "Upscaler/Common.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Evaluate.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/HudMessages.h"
#include "Upscaler/StereoCapture.h"
#include "Upscaler/Telemetry.h"

// The only translation unit in the tree that includes any of these three. The same
// discipline IBackend.h holds for vendor SDK headers: grep imgui.h, imgui_impl_dx11.h or
// openvr.h under src/ and exactly one file answers, so no ImGui type and no vr:: type can
// leak into a header some other module includes.
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <openvr.h>

namespace Upscaler::Panel
{
	namespace
	{
		// The overlay key is a process-wide identifier in the runtime's own namespace;
		// reverse-DNS-ish and unique to this mod so two overlays can never collide.
		constexpr const char* kOverlayKey = "rollingrock.fo4vr_upscaler.panel";
		constexpr const char* kOverlayName = "Fo4VR Upscaler";

		// Above the scene application's own overlays. Sort order is unsigned and larger
		// wins; nothing else in this process creates an overlay, so any high value does,
		// and a round one is easier to recognise in a runtime log than 0xFFFF.
		constexpr std::uint32_t kSortOrder = 100;

		// The overlay sits a little below the gaze so it does not cover what the operator
		// is comparing. OpenVR tracked-device space is +X right, +Y up, -Z forward, so
		// "ahead" is negative Z.
		constexpr float kVerticalOffsetMeters = -0.15f;

		// ImGui's bitmap font is 13 px. At 1024 px across a 0.6 m overlay that is
		// unreadable at a metre, so everything is scaled. What the operator can actually
		// read is a headset question, not a headless one - this is the starting point,
		// not a measured value.
		constexpr float kUiScale = 2.0f;

		constexpr float kClearColor[4] = { 0.055f, 0.065f, 0.085f, 1.0f };

		// A failed creation is retried, not latched off: openvr_api.dll may not be mapped
		// yet the first time somebody asks, and the compositor can refuse while it is
		// still starting. Retrying every frame would log a wall, so attempts are floored.
		constexpr std::uint64_t kCreateRetryFloorMs = 2000;

		// Telemetry::IntervalSummary builds a string each call. It prices intervals over a
		// window, so re-reading it every frame buys nothing and allocates on the frame
		// producer; the panel refreshes it on a floor instead, and only while the timers
		// are on - Telemetry::Enabled() is the same bit /perf reports as requestedEnabled,
		// so the panel cannot disagree with /perf about whether they are running. The text
		// is the module's own, unparsed and uncomputed here.
		//
		// One thread note, because this is the first call in the tree to read that module
		// from the render thread rather than from the DevBench listener. Telemetry's
		// report read is a seqlock retry whose only writer is the verified render thread,
		// so a read taken from inside a mutation window this same thread had opened would
		// be waiting for itself. It cannot be: every Begin/EndStatsMutation region in
		// Telemetry.cpp is atomic-only and calls nothing, so no such region can contain a
		// seam dispatch, and this call sits at the ClearColor seam with no window open.
		// Telemetry bounds the retry anyway, so if that ever stops being true the cost is
		// a stale panel line rather than a stalled frame - see ReadReportSnapshot.
		constexpr std::uint64_t kTelemetryRefreshMs = 500;

		using GetGenericInterface_t = void*(*)(const char*, vr::EVRInitError*);

		// --- published state ------------------------------------------------------
		//
		// Every atomic below is written by exactly one thread and read by others.
		// release on the write, acquire on the read, wherever a reader must see
		// something else that was published before it; relaxed on the pure counters,
		// which are reported and never used to decide anything.

		// Written by Configure from the settings apply path, read by the render thread
		// once per frame and by Snapshot. The width and distance are floats rather than
		// doubles because that is what SetOverlayWidthInMeters and the transform take.
		std::atomic_bool g_enabled{ true };
		std::atomic<float> g_widthMeters{ 0.6f };
		std::atomic<float> g_distanceMeters{ 1.0f };

		// The show intent. An unsigned counter rather than a bool so Toggle is one
		// read-modify-write: two callers flipping at once cannot lose each other's flip,
		// which a load/store pair around a bool would.
		std::atomic<std::uint32_t> g_showIntent{ 0 };

		std::atomic_bool g_runtimeResolved{ false };
		std::atomic<std::int32_t> g_lastInitError{ 0 };
		std::atomic_bool g_overlayCreated{ false };
		std::atomic<std::uint64_t> g_overlayHandle{ 0 };
		std::atomic_bool g_visible{ false };
		std::atomic<std::uint64_t> g_overlayErrors{ 0 };
		std::atomic<std::uint64_t> g_framesDrawn{ 0 };
		std::atomic<std::uint64_t> g_textureSubmits{ 0 };
		std::atomic<std::uint64_t> g_eventsPolled{ 0 };
		std::atomic<std::uint64_t> g_eventsMouseMove{ 0 };
		std::atomic<std::uint64_t> g_eventsMouseButtonDown{ 0 };
		std::atomic<std::uint64_t> g_eventsMouseButtonUp{ 0 };
		std::atomic<std::uint64_t> g_eventsScrollSmooth{ 0 };
		std::atomic<std::uint64_t> g_eventsOverlayShown{ 0 };
		std::atomic<std::uint64_t> g_eventsOverlayHidden{ 0 };
		std::atomic<std::uint64_t> g_eventsOther{ 0 };
		std::atomic<std::uint64_t> g_creations{ 0 };
		std::atomic<std::uint64_t> g_teardowns{ 0 };
		std::atomic<std::uint64_t> g_creationFailures{ 0 };

		// Whether the last frame the panel drew included the per-interval timing section.
		// Written on the render thread inside the draw and read by Snapshot; relaxed like
		// the counters, because it decides nothing - it exists so a headless run can tell
		// which of the two branches the panel actually took, which is the only way the
		// perfTimers gate on the section above is provable without a headset.
		std::atomic_bool g_telemetryDrawn{ false };

		// atomic<shared_ptr<const string>>, the same spelling Hotkeys.cpp and
		// Configuration.cpp use for a cross-thread string: readable without a lock, and a
		// store can never hand a reader a half-written string. All four are published on
		// change only, never per frame.
		std::atomic<std::shared_ptr<const std::string>> g_lastOverlayError{
			std::make_shared<const std::string>("None")
		};
		std::atomic<std::shared_ptr<const std::string>> g_interfaceVersion{
			std::make_shared<const std::string>(vr::IVROverlay_Version)
		};
		std::atomic<std::shared_ptr<const std::string>> g_textureSharedFlag{
			std::make_shared<const std::string>("not-measured")
		};

		void PublishString(
			std::atomic<std::shared_ptr<const std::string>>& a_slot,
			std::string_view a_value) noexcept
		{
			try {
				a_slot.store(std::make_shared<const std::string>(a_value), std::memory_order_release);
			} catch (...) {
				// A failed allocation leaves the label stale, not wrong; the counters
				// beside it still advance.
			}
		}

		[[nodiscard]] std::string ReadString(
			const std::atomic<std::shared_ptr<const std::string>>& a_slot) noexcept
		{
			try {
				if (const auto value = a_slot.load(std::memory_order_acquire); value) {
					return *value;
				}
			} catch (...) {
			}
			return {};
		}

		// --- render-thread-owned state --------------------------------------------
		//
		// One ownership spelling: raw ID3D11* at file scope, created once behind the
		// Ensure latch below, released in one place. Nothing here is touched off the
		// render thread.

		vr::IVROverlay* g_overlay = nullptr;  // borrowed from the runtime, never released
		vr::VROverlayHandle_t g_handle = vr::k_ulOverlayHandleInvalid;
		ID3D11Texture2D* g_texture = nullptr;
		ID3D11RenderTargetView* g_rtv = nullptr;
		ID3D11Texture2D* g_staging = nullptr;
		D3D11StateIsolation::Context g_isolation;
		ImGuiContext* g_imgui = nullptr;
		bool g_imguiBackendReady = false;
		bool g_textureIsShared = false;
		bool g_sharedFlagMeasured = false;
		std::uint64_t g_lastCreateAttemptMs = 0;
		std::uint64_t g_lastFrameTicks = 0;
		std::uint64_t g_lastTelemetryMs = 0;
		std::string g_telemetryText;
		// The perfTimers gate, read once per frame in PollFrame and used by both the
		// fetch and the draw so the two cannot disagree within a frame. Render-thread
		// only, like everything else in this block.
		bool g_timersOn = false;
		// Set true the frame a draw lands in the texture, false whenever the texture is
		// (re-)created or released. Nothing is submitted to the compositor or read back
		// before it is true: D3D11 leaves a fresh render target's contents undefined, so
		// a frame that never drew - an isolation refusal on the first visible frame -
		// would otherwise hand the headset undefined memory and count a submit against a
		// frame that does not exist.
		bool g_texturePainted = false;

		LogGate g_lookupFailureGate{ 30000 };
		LogGate g_createFailureGate{ 30000 };
		LogGate g_submitFailureGate{ 30000 };
		LogGate g_isolationFailureGate{ 30000 };

		// --- capture handoff --------------------------------------------------------
		//
		// The render thread copies the panel texture into a staging texture, maps it and
		// fills the vector below; the DevBench listener writes the file. The mutex is
		// taken with try_to_lock on the render thread and only on the one frame a capture
		// completes, so the frame producer can never wait on the listener - the request
		// simply stays armed for the next frame. It is the same division of labour
		// StereoCapture uses: the render thread owns the D3D11 copy and map, the listener
		// owns file output.
		std::mutex g_captureMutex;
		std::vector<std::uint8_t> g_capturePixels;
		std::atomic<std::uint64_t> g_captureRequested{ 0 };
		std::atomic<std::uint64_t> g_captureCompleted{ 0 };
		std::atomic<std::uint64_t> g_captureSaved{ 0 };
		std::atomic<std::shared_ptr<const std::string>> g_capturePath{
			std::make_shared<const std::string>("")
		};
		std::atomic<std::shared_ptr<const std::string>> g_captureError{
			std::make_shared<const std::string>("")
		};

		// --- overlay error plumbing -------------------------------------------------

		// Counts the error and publishes the runtime's own name for it. Returns true when
		// the call succeeded, so a caller can write `if (!Check(...)) { ... }` and read
		// like the HRESULT checks everywhere else in this tree.
		[[nodiscard]] bool Check(vr::EVROverlayError a_error, std::string_view a_what) noexcept
		{
			if (a_error == vr::VROverlayError_None) {
				return true;
			}
			g_overlayErrors.fetch_add(1, std::memory_order_relaxed);
			const char* const name = g_overlay ? g_overlay->GetOverlayErrorNameFromEnum(a_error) : nullptr;
			try {
				std::string label{ a_what };
				label += ": ";
				label += name ? name : "unnamed";
				PublishString(g_lastOverlayError, label);
			} catch (...) {
			}
			return false;
		}

		// --- lifecycle --------------------------------------------------------------

		void ReleaseImGui() noexcept
		{
			if (g_imgui) {
				// Both calls below act on the current context, not on a handle, so it
				// has to be ours before either runs.
				ImGui::SetCurrentContext(g_imgui);
			}
			if (g_imguiBackendReady) {
				ImGui_ImplDX11_Shutdown();
				g_imguiBackendReady = false;
			}
			if (g_imgui) {
				ImGui::DestroyContext(g_imgui);
				g_imgui = nullptr;
			}
		}

		// Order matters and is the one thing this function exists to get right: the
		// overlay holds our texture until DestroyOverlay returns, so it is hidden and
		// destroyed before anything is released. ImGui's backend releases its own device
		// objects in Shutdown, which is why that runs before our own views go.
		void Teardown() noexcept
		{
			if (g_overlay && g_handle != vr::k_ulOverlayHandleInvalid) {
				static_cast<void>(Check(g_overlay->HideOverlay(g_handle), "HideOverlay"sv));
				static_cast<void>(Check(g_overlay->DestroyOverlay(g_handle), "DestroyOverlay"sv));
			}
			g_handle = vr::k_ulOverlayHandleInvalid;
			ReleaseImGui();
			SafeRelease(g_rtv);
			SafeRelease(g_texture);
			SafeRelease(g_staging);
			g_texturePainted = false;
			g_telemetryDrawn.store(false, std::memory_order_relaxed);
			g_isolation.Shutdown();
			g_overlayCreated.store(false, std::memory_order_release);
			g_overlayHandle.store(0, std::memory_order_relaxed);
			g_visible.store(false, std::memory_order_release);
			g_teardowns.fetch_add(1, std::memory_order_relaxed);
			g_lastCreateAttemptMs = 0;
			g_createFailureGate.Rearm();
			g_submitFailureGate.Rearm();
			logger::info("panel: overlay torn down"sv);
		}

		// The one interface this module asks the game's loader for. Never VR_Init: the
		// game owns the VR session and a second Init would be a second application.
		[[nodiscard]] bool ResolveRuntime() noexcept
		{
			if (g_overlay) {
				return true;
			}
			// The lookup itself is retried every time somebody asks - openvr_api.dll may
			// only be mapped later, and the runtime can refuse while it is still starting
			// - but the line it writes is floored: once, and then at most once per gate
			// interval. EnsureCreated floors the attempt itself at two seconds on top.
			const bool loud = g_lookupFailureGate.Ready();

			const auto module = ::GetModuleHandleW(L"openvr_api.dll");
			if (!module) {
				if (loud) {
					logger::warn("panel: openvr_api.dll is not loaded in this process; the panel is unavailable"sv);
				}
				return false;
			}
			const auto get = reinterpret_cast<GetGenericInterface_t>(
				::GetProcAddress(module, "VR_GetGenericInterface"));
			if (!get) {
				if (loud) {
					logger::warn("panel: openvr_api.dll exports no VR_GetGenericInterface; the panel is unavailable"sv);
				}
				return false;
			}
			vr::EVRInitError error = vr::VRInitError_None;
			// The version string comes from the compiled header, so an SDK bump moves the
			// request with it rather than leaving a stale literal here.
			auto* const overlay = static_cast<vr::IVROverlay*>(get(vr::IVROverlay_Version, &error));
			g_lastInitError.store(static_cast<std::int32_t>(error), std::memory_order_relaxed);
			if (!overlay) {
				if (loud) {
					logger::warn(FMT_STRING("panel: the runtime did not serve {} (EVRInitError {}); the panel is unavailable"),
						vr::IVROverlay_Version, static_cast<int>(error));
				}
				return false;
			}
			g_overlay = overlay;
			g_runtimeResolved.store(true, std::memory_order_release);
			logger::info(FMT_STRING("panel: {} resolved through the game's own openvr_api.dll"), vr::IVROverlay_Version);
			return true;
		}

		// D3D11_RESOURCE_MISC_SHARED is not set, and that is a measurement rather than a
		// guess. VERIFIED against SteamVR's null driver on 2026-09-05: a texture created
		// with no misc flags at all was accepted by SetOverlayTexture, submitted every
		// frame for the length of a run, and read back showing the drawn panel - the
		// state block's textureSharedFlag carries that answer verbatim. The flag was
		// expected to be needed for the compositor's cross-process open and is not, at
		// least in-process on this runtime.
		//
		// SubmitTexture keeps the other half of the measurement rather than deleting it:
		// on the first refusal of an unshared texture it names the runtime's error,
		// re-creates with the flag and retries once. That branch has never fired here,
		// and it exists for the runtime this has not been run against - OpenComposite
		// serves overlays through a translated OpenXR session, and nothing in this
		// project has measured what its compositor needs. A blank overlay with no
		// explanation is the failure it prevents.
		[[nodiscard]] bool CreateTexture(ID3D11Device* a_device, bool a_shared) noexcept
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kTextureWidth;
			desc.Height = kTextureHeight;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = a_shared ? D3D11_RESOURCE_MISC_SHARED : 0u;

			ID3D11Texture2D* texture = nullptr;
			HRESULT hr = a_device->CreateTexture2D(&desc, nullptr, &texture);
			if (FAILED(hr)) {
				logger::warn(FMT_STRING("panel: CreateTexture2D failed hr=0x{:08X} (shared={})"),
					static_cast<std::uint32_t>(hr), a_shared);
				return false;
			}
			ID3D11RenderTargetView* rtv = nullptr;
			hr = a_device->CreateRenderTargetView(texture, nullptr, &rtv);
			if (FAILED(hr)) {
				logger::warn(FMT_STRING("panel: CreateRenderTargetView failed hr=0x{:08X}"),
					static_cast<std::uint32_t>(hr));
				SafeRelease(texture);
				return false;
			}
			SafeRelease(g_rtv);
			SafeRelease(g_texture);
			g_texture = texture;
			g_rtv = rtv;
			g_textureIsShared = a_shared;
			// A brand-new render target holds nothing D3D11 defines. This is the one
			// place that is true, and clearing the flag here is what stops the shared-flag
			// retry path from submitting or reading back the replacement texture before a
			// frame has been drawn into it.
			g_texturePainted = false;
			return true;
		}

		[[nodiscard]] bool CreateOverlay() noexcept
		{
			vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
			if (!Check(g_overlay->CreateOverlay(kOverlayKey, kOverlayName, &handle), "CreateOverlay"sv)) {
				return false;
			}
			g_handle = handle;

			const float distance = g_distanceMeters.load(std::memory_order_acquire);
			// Identity rotation with a translation: the overlay's own +Z is its front
			// face, so an overlay placed at -Z with no rotation faces straight back at
			// the eye it is parented to.
			vr::HmdMatrix34_t transform{};
			transform.m[0][0] = 1.0f;
			transform.m[1][1] = 1.0f;
			transform.m[2][2] = 1.0f;
			transform.m[1][3] = kVerticalOffsetMeters;
			transform.m[2][3] = -distance;
			static_cast<void>(Check(
				g_overlay->SetOverlayTransformTrackedDeviceRelative(
					handle, vr::k_unTrackedDeviceIndex_Hmd, &transform),
				"SetOverlayTransformTrackedDeviceRelative"sv));
			static_cast<void>(Check(
				g_overlay->SetOverlayWidthInMeters(handle, g_widthMeters.load(std::memory_order_acquire)),
				"SetOverlayWidthInMeters"sv));
			static_cast<void>(Check(g_overlay->SetOverlaySortOrder(handle, kSortOrder), "SetOverlaySortOrder"sv));
			static_cast<void>(Check(
				g_overlay->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_Mouse),
				"SetOverlayInputMethod"sv));
			// The units mouse events arrive in (openvr.h 4258-4260). Set to the texture
			// size so a VREvent_Mouse_t is already in ImGui's own pixels and the only
			// conversion left is the vertical flip.
			vr::HmdVector2_t mouseScale{};
			mouseScale.v[0] = static_cast<float>(kTextureWidth);
			mouseScale.v[1] = static_cast<float>(kTextureHeight);
			static_cast<void>(Check(g_overlay->SetOverlayMouseScale(handle, &mouseScale), "SetOverlayMouseScale"sv));
			// The system-wide laser mouse is activated whenever a visible overlay carries
			// this flag and its input method is not none (openvr.h 3917-3919); the second
			// flag is what turns trackpad scrolling into VREvent_ScrollSmooth.
			static_cast<void>(Check(
				g_overlay->SetOverlayFlag(handle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true),
				"SetOverlayFlag(MakeOverlaysInteractiveIfVisible)"sv));
			static_cast<void>(Check(
				g_overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRSmoothScrollEvents, true),
				"SetOverlayFlag(SendVRSmoothScrollEvents)"sv));
			// ImGui writes sRGB-ish bytes into an UNORM target with no conversion, so the
			// compositor must be told to display them as they are rather than treat them
			// as linear.
			static_cast<void>(Check(
				g_overlay->SetOverlayTextureColorSpace(handle, vr::ColorSpace_Gamma),
				"SetOverlayTextureColorSpace"sv));
			return true;
		}

		[[nodiscard]] bool EnsureImGui(ID3D11Device* a_device, ID3D11DeviceContext* a_context) noexcept
		{
			if (g_imguiBackendReady) {
				return true;
			}
			if (!g_imgui) {
				IMGUI_CHECKVERSION();
				g_imgui = ImGui::CreateContext();
				if (!g_imgui) {
					return false;
				}
			}
			ImGui::SetCurrentContext(g_imgui);
			ImGuiIO& io = ImGui::GetIO();
			// No platform backend: there is no window, no message loop and no WndProc to
			// hook, so DisplaySize, DeltaTime and every input event are supplied by this
			// file. Saving an ini beside the game would also be a write we never asked
			// for.
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(static_cast<float>(kTextureWidth), static_cast<float>(kTextureHeight));
			io.FontGlobalScale = kUiScale;
			ImGui::GetStyle().ScaleAllSizes(kUiScale);
			if (!ImGui_ImplDX11_Init(a_device, a_context)) {
				ImGui::DestroyContext(g_imgui);
				g_imgui = nullptr;
				return false;
			}
			g_imguiBackendReady = true;
			return true;
		}

		[[nodiscard]] bool EnsureCreated() noexcept
		{
			if (g_overlayCreated.load(std::memory_order_acquire)) {
				return true;
			}
			const std::uint64_t now = ::GetTickCount64();
			if (g_lastCreateAttemptMs != 0 && now - g_lastCreateAttemptMs < kCreateRetryFloorMs) {
				return false;
			}
			g_lastCreateAttemptMs = now;

			auto* const device = Engine::Device();
			auto* const context = Engine::ImmediateContext();
			if (!device || !context) {
				if (g_createFailureGate.Ready()) {
					logger::warn("panel: the engine device is not resolved yet; the overlay is not created"sv);
				}
				g_creationFailures.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (!ResolveRuntime()) {
				g_creationFailures.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (!g_isolation.Initialize(device, context)) {
				if (g_createFailureGate.Ready()) {
					logger::warn(FMT_STRING("panel: state isolation could not be initialized (hr=0x{:08X}); the overlay is not created"),
						static_cast<std::uint32_t>(g_isolation.LastError()));
				}
				g_creationFailures.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (!CreateTexture(device, /*a_shared=*/false)) {
				g_creationFailures.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (!CreateOverlay()) {
				SafeRelease(g_rtv);
				SafeRelease(g_texture);
				g_creationFailures.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (!EnsureImGui(device, context)) {
				if (g_createFailureGate.Ready()) {
					logger::warn("panel: the ImGui D3D11 backend could not be initialized; the overlay is not created"sv);
				}
				static_cast<void>(Check(g_overlay->DestroyOverlay(g_handle), "DestroyOverlay"sv));
				g_handle = vr::k_ulOverlayHandleInvalid;
				SafeRelease(g_rtv);
				SafeRelease(g_texture);
				g_creationFailures.fetch_add(1, std::memory_order_relaxed);
				return false;
			}

			g_overlayHandle.store(static_cast<std::uint64_t>(g_handle), std::memory_order_relaxed);
			g_overlayCreated.store(true, std::memory_order_release);
			g_creations.fetch_add(1, std::memory_order_relaxed);
			logger::info(FMT_STRING("panel: overlay '{}' created, handle {}, {}x{} texture"),
				kOverlayKey, static_cast<std::uint64_t>(g_handle), kTextureWidth, kTextureHeight);
			return true;
		}

		// --- the drawn status section ------------------------------------------------

		void Row(const char* a_label, const std::string& a_value)
		{
			ImGui::TextUnformatted(a_label);
			ImGui::SameLine(260.0f * kUiScale);
			ImGui::TextUnformatted(a_value.c_str());
		}

		// Every value here comes out of a snapshot some other module already publishes.
		// Nothing is derived, compared or recomputed: the panel and DevBench are two
		// clients of one surface, and a number the panel showed that /state did not would
		// be a second source of truth.
		//
		// The cost is four lock-free snapshot reads and a handful of small string copies,
		// paid only while the panel is visible and on a frame that is already running an
		// ImGui draw. None of the four takes a lock this thread could wait on.
		//
		// Its caller wraps it, because every row below builds a std::string and this runs
		// under a noexcept frame producer: an escaped bad_alloc would terminate the
		// process over a diagnostic line. Caught between Begin and End the ImGui frame is
		// still well formed - the window has been opened and will be closed either way.
		void DrawStatusGuarded()
		{
			const auto modeSwitch = Configuration::SwitchSnapshot();
			const auto isolation = HDRRoundTrip::Snapshot();
			const auto dynamic = DynamicResolution::Snapshot();
			const auto hud = HudMessages::Snapshot();

			ImGui::TextUnformatted("Fo4VR Upscaler");
			ImGui::Separator();
			Row("mode", modeSwitch.liveMode + "  (" + modeSwitch.modeSource + ")");
			Row("backend", std::string(HDRRoundTrip::StereoBackendName(
								static_cast<HDRRoundTrip::StereoBackend>(isolation.stereoBackend))));
			Row("preset requested", Evaluate::RequestedPreset());
			Row("preset reported", Evaluate::ReportedPreset());
			Row("render size",
				std::to_string(dynamic.snappedRenderWidth) + " x " + std::to_string(dynamic.snappedRenderHeight) +
					"  ->  " + std::to_string(dynamic.snappedOutputWidth) + " x " +
					std::to_string(dynamic.snappedOutputHeight));
			Row("ratio", std::to_string(dynamic.requestedScale));
			Row("session",
				std::string(HDRRoundTrip::SessionStateName(
					static_cast<HDRRoundTrip::SessionState>(isolation.sessionState))) +
					"  (" +
					std::string(HDRRoundTrip::RecoveryRefusalName(
						static_cast<HDRRoundTrip::RecoveryRefusal>(isolation.recoveryRefusal))) +
					")");
			Row("last HUD line", hud.lastMessage);
			ImGui::Separator();
			// The timing section is drawn only when the timers are running, which is what
			// the setting, the README and this panel's own /state block all promise. The
			// value drawn is whatever the fetch above last put in g_telemetryText: the
			// fetch and this draw read the same gate on the same frame, so the section
			// cannot show a window that has since been switched off.
			if (g_timersOn) {
				// A fixed-height child so the status rows above never move: 23 intervals
				// do not fit under them, and a status block that shifts down when the
				// timers come on is a worse panel than one whose bottom pane clips. The
				// pane scrolls, and the smooth-scroll events the pump already feeds ImGui
				// are what scroll it.
				if (ImGui::BeginChild("timers", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Border)) {
					ImGui::TextUnformatted(g_telemetryText.c_str());
				}
				ImGui::EndChild();
			} else {
				ImGui::TextUnformatted("timers off");
			}
		}

		void DrawStatus() noexcept
		{
			try {
				DrawStatusGuarded();
			} catch (...) {
				ImGui::TextUnformatted("status unavailable");
			}
		}

		void BuildFrame(float a_deltaSeconds)
		{
			ImGui::SetCurrentContext(g_imgui);
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(static_cast<float>(kTextureWidth), static_cast<float>(kTextureHeight));
			io.DeltaTime = a_deltaSeconds;

			ImGui_ImplDX11_NewFrame();
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(io.DisplaySize);
			constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
			                                    ImGuiWindowFlags_NoSavedSettings |
			                                    ImGuiWindowFlags_NoBringToFrontOnFocus;
			if (ImGui::Begin("Fo4VR Upscaler##panel", nullptr, kFlags)) {
				DrawStatus();
			}
			ImGui::End();
			ImGui::Render();
		}

		// The whole ImGui frame runs inside the isolation bracket, not only the draw
		// call: ImGui's DX11 backend issues immediate-context work of its own (its
		// per-frame vertex and index buffer maps, and the font upload on the first frame),
		// and the bracket has to cover every one of them. Building widgets touches no
		// D3D11, so it costs nothing to have it inside.
		//
		// No __try anywhere in this function, deliberately. ImGui is a C++ library with
		// destructible state on the stack and MSVC will not accept it in a frame that
		// needs unwinding; a bracket that cannot hold ImGui is worse than none, because
		// it would have to be dropped exactly where the risk is. The draw touches only
		// our own texture and view, never an engine resource, so there is no engine
		// memory here for SEH to protect.
		//
		// Returns whether a frame actually landed in the texture. The caller submits and
		// reads back only on a true, because a refused scope leaves the texture untouched
		// and D3D11 defines nothing about a render target that has never been drawn into.
		[[nodiscard]] bool DrawAndSubmit(ID3D11DeviceContext* a_context, float a_deltaSeconds) noexcept
		{
			D3D11StateIsolation::Scope scope(g_isolation);
			if (!scope) {
				if (g_isolationFailureGate.Ready()) {
					logger::warn(FMT_STRING("panel: state isolation refused a scope (hr=0x{:08X}); this frame is not drawn"),
						static_cast<std::uint32_t>(g_isolation.LastError()));
				}
				return false;
			}
			BuildFrame(a_deltaSeconds);

			D3D11_VIEWPORT viewport{};
			viewport.Width = static_cast<float>(kTextureWidth);
			viewport.Height = static_cast<float>(kTextureHeight);
			viewport.MaxDepth = 1.0f;
			a_context->RSSetViewports(1, &viewport);
			a_context->OMSetRenderTargets(1, &g_rtv, nullptr);
			a_context->ClearRenderTargetView(g_rtv, kClearColor);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			// Unbind before the scope closes so our texture is not still an output when
			// the compositor reads it; a resource cannot be an RTV and a shader resource
			// at once and a release runtime drops the bind silently.
			ID3D11RenderTargetView* const none = nullptr;
			a_context->OMSetRenderTargets(1, &none, nullptr);
			static_cast<void>(scope.Close());
			g_framesDrawn.fetch_add(1, std::memory_order_relaxed);
			g_texturePainted = true;
			// Published after the draw that used it, so a reader that sees the counter
			// advance sees which branch that frame took.
			g_telemetryDrawn.store(g_timersOn, std::memory_order_relaxed);
			return true;
		}

		void SubmitTexture() noexcept
		{
			vr::Texture_t texture{};
			texture.handle = g_texture;
			texture.eType = vr::TextureType_DirectX;
			texture.eColorSpace = vr::ColorSpace_Gamma;
			vr::EVROverlayError error = g_overlay->SetOverlayTexture(g_handle, &texture);
			if (error != vr::VROverlayError_None && !g_sharedFlagMeasured && !g_textureIsShared) {
				// The measurement. The first refusal of a texture created with no misc
				// flags is the only evidence that decides whether
				// D3D11_RESOURCE_MISC_SHARED is needed here, so it is recorded by name
				// rather than inferred from a retry succeeding.
				const char* const name = g_overlay->GetOverlayErrorNameFromEnum(error);
				g_overlayErrors.fetch_add(1, std::memory_order_relaxed);
				g_sharedFlagMeasured = true;
				logger::info(FMT_STRING("panel: SetOverlayTexture refused an unshared texture ({}); re-creating it with D3D11_RESOURCE_MISC_SHARED"),
					name ? name : "unnamed");
				auto* const device = Engine::Device();
				if (device && CreateTexture(device, /*a_shared=*/true)) {
					try {
						PublishString(g_textureSharedFlag,
							std::string("shared-required (unshared answered ") + (name ? name : "unnamed") + ")");
					} catch (...) {
					}
					// Nothing has been drawn into the new texture yet, so it is not
					// submitted on this frame. The next one draws into it and submits it
					// through the ordinary path above; the flag is latched, so this
					// branch cannot be taken twice.
					return;
				}
			}
			if (error != vr::VROverlayError_None) {
				if (g_submitFailureGate.Ready()) {
					static_cast<void>(Check(error, "SetOverlayTexture"sv));
					logger::warn(FMT_STRING("panel: SetOverlayTexture failed ({}); the overlay stays blank"),
						ReadString(g_lastOverlayError));
				} else {
					g_overlayErrors.fetch_add(1, std::memory_order_relaxed);
				}
				return;
			}
			if (!g_sharedFlagMeasured) {
				g_sharedFlagMeasured = true;
				PublishString(g_textureSharedFlag, "not-required (accepted without D3D11_RESOURCE_MISC_SHARED)"sv);
				logger::info("panel: SetOverlayTexture accepted a texture created without D3D11_RESOURCE_MISC_SHARED"sv);
			}
			g_textureSubmits.fetch_add(1, std::memory_order_relaxed);
		}

		// --- the event pump ----------------------------------------------------------

		// VREvent_Mouse_t co-ordinates are GL space with the bottom left of the texture at
		// 0,0 (openvr.h 1097); ImGui's are top-left pixels. The mouse scale set at
		// creation already puts x and y in texture pixels, so the flip is the whole
		// conversion.
		[[nodiscard]] float FlipY(float a_y) noexcept
		{
			return static_cast<float>(kTextureHeight) - a_y;
		}

		[[nodiscard]] int ImGuiButton(std::uint32_t a_button) noexcept
		{
			switch (a_button) {
			case vr::VRMouseButton_Right:
				return 1;
			case vr::VRMouseButton_Middle:
				return 2;
			case vr::VRMouseButton_Left:
			default:
				return 0;
			}
		}

		void PumpEvents() noexcept
		{
			ImGui::SetCurrentContext(g_imgui);
			ImGuiIO& io = ImGui::GetIO();
			vr::VREvent_t event{};
			while (g_overlay->PollNextOverlayEvent(g_handle, &event, sizeof(event))) {
				g_eventsPolled.fetch_add(1, std::memory_order_relaxed);
				switch (event.eventType) {
				case vr::VREvent_MouseMove:
					g_eventsMouseMove.fetch_add(1, std::memory_order_relaxed);
					io.AddMousePosEvent(event.data.mouse.x, FlipY(event.data.mouse.y));
					break;
				case vr::VREvent_MouseButtonDown:
					g_eventsMouseButtonDown.fetch_add(1, std::memory_order_relaxed);
					io.AddMousePosEvent(event.data.mouse.x, FlipY(event.data.mouse.y));
					io.AddMouseButtonEvent(ImGuiButton(event.data.mouse.button), true);
					break;
				case vr::VREvent_MouseButtonUp:
					g_eventsMouseButtonUp.fetch_add(1, std::memory_order_relaxed);
					io.AddMousePosEvent(event.data.mouse.x, FlipY(event.data.mouse.y));
					io.AddMouseButtonEvent(ImGuiButton(event.data.mouse.button), false);
					break;
				case vr::VREvent_ScrollSmooth:
					g_eventsScrollSmooth.fetch_add(1, std::memory_order_relaxed);
					io.AddMouseWheelEvent(event.data.scroll.xdelta, event.data.scroll.ydelta);
					break;
				case vr::VREvent_OverlayShown:
					g_eventsOverlayShown.fetch_add(1, std::memory_order_relaxed);
					g_visible.store(true, std::memory_order_release);
					break;
				case vr::VREvent_OverlayHidden:
					g_eventsOverlayHidden.fetch_add(1, std::memory_order_relaxed);
					g_visible.store(false, std::memory_order_release);
					break;
				default:
					g_eventsOther.fetch_add(1, std::memory_order_relaxed);
					break;
				}
			}
		}

		// --- capture -----------------------------------------------------------------

		[[nodiscard]] bool EnsureStaging(ID3D11Device* a_device) noexcept
		{
			if (g_staging) {
				return true;
			}
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kTextureWidth;
			desc.Height = kTextureHeight;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			const HRESULT hr = a_device->CreateTexture2D(&desc, nullptr, &g_staging);
			if (FAILED(hr)) {
				logger::warn(FMT_STRING("panel: capture staging texture failed hr=0x{:08X}"),
					static_cast<std::uint32_t>(hr));
				g_staging = nullptr;
				return false;
			}
			return true;
		}

		void ServiceCapture(ID3D11Device* a_device, ID3D11DeviceContext* a_context) noexcept
		{
			const std::uint64_t requested = g_captureRequested.load(std::memory_order_acquire);
			if (requested == 0 || requested == g_captureCompleted.load(std::memory_order_relaxed)) {
				return;
			}
			// try_to_lock, never a wait: this is the frame producer. A listener still
			// holding the buffer means the request stays armed and the next frame takes
			// it, which costs one frame and never a stall.
			std::unique_lock lock(g_captureMutex, std::try_to_lock);
			if (!lock.owns_lock()) {
				return;
			}
			if (!EnsureStaging(a_device)) {
				PublishString(g_captureError, "the staging texture could not be created"sv);
				g_captureCompleted.store(requested, std::memory_order_release);
				return;
			}
			// No isolation bracket around these three: CopyResource, Map and Unmap bind
			// nothing and change no pipeline state, so there is no host state to save.
			// The Map does stall this thread until the copy retires, and the assign below
			// allocates three megabytes - both once per capture request, on a route no
			// shipped configuration reaches, which is the same bargain StereoCapture's
			// readback already makes.
			a_context->CopyResource(g_staging, g_texture);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const HRESULT hr = a_context->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped);
			if (FAILED(hr)) {
				try {
					PublishString(g_captureError,
						"mapping the staging texture failed hr=0x" + std::to_string(static_cast<std::uint32_t>(hr)));
				} catch (...) {
				}
				g_captureCompleted.store(requested, std::memory_order_release);
				return;
			}
			try {
				g_capturePixels.assign(static_cast<std::size_t>(kTextureWidth) * kTextureHeight * 4u, 0u);
				const auto* const source = static_cast<const std::uint8_t*>(mapped.pData);
				for (std::uint32_t y = 0; y < kTextureHeight; ++y) {
					std::memcpy(
						g_capturePixels.data() + static_cast<std::size_t>(y) * kTextureWidth * 4u,
						source + static_cast<std::size_t>(y) * mapped.RowPitch,
						static_cast<std::size_t>(kTextureWidth) * 4u);
				}
				PublishString(g_captureError, ""sv);
			} catch (...) {
				g_capturePixels.clear();
				PublishString(g_captureError, "the capture buffer could not be allocated"sv);
			}
			a_context->Unmap(g_staging, 0);
			g_captureCompleted.store(requested, std::memory_order_release);
		}
	}

	void Configure(bool a_enabled, double a_widthMeters, double a_distanceMeters) noexcept
	{
		g_enabled.store(a_enabled, std::memory_order_release);
		g_widthMeters.store(static_cast<float>(a_widthMeters), std::memory_order_release);
		g_distanceMeters.store(static_cast<float>(a_distanceMeters), std::memory_order_release);
	}

	void RequestShow() noexcept
	{
		g_showIntent.store(1, std::memory_order_release);
	}

	void RequestHide() noexcept
	{
		g_showIntent.store(0, std::memory_order_release);
	}

	void Toggle() noexcept
	{
		g_showIntent.fetch_xor(1, std::memory_order_acq_rel);
	}

	void PollFrame() noexcept
	{
		const bool enabled = g_enabled.load(std::memory_order_acquire);
		if (!enabled) {
			if (g_overlayCreated.load(std::memory_order_acquire)) {
				Teardown();
			}
			return;
		}
		// One load per dispatch, used by value for the rest of the frame - the same rule
		// every other flag on this seam follows.
		const bool wanted = (g_showIntent.load(std::memory_order_acquire) & 1u) != 0u;
		if (!wanted) {
			if (g_overlayCreated.load(std::memory_order_acquire) && g_visible.load(std::memory_order_acquire)) {
				static_cast<void>(Check(g_overlay->HideOverlay(g_handle), "HideOverlay"sv));
				g_visible.store(false, std::memory_order_release);
			}
			// A hidden panel costs the two atomic loads above and nothing else: no ImGui
			// frame, no draw, no texture submit and no event poll.
			return;
		}
		if (!EnsureCreated()) {
			return;
		}
		// Shown whenever the intent says shown and the flag says it is not. The flag is
		// also written by the OverlayShown/OverlayHidden events, so this is a
		// reconciliation rather than a one-shot: if something else ever hides an overlay
		// the operator asked for, the next frame asks for it back, and the event counters
		// say how often that happened.
		if (!g_visible.load(std::memory_order_acquire)) {
			if (!Check(g_overlay->ShowOverlay(g_handle), "ShowOverlay"sv)) {
				return;
			}
			g_visible.store(true, std::memory_order_release);
		}

		auto* const device = Engine::Device();
		auto* const context = Engine::ImmediateContext();
		if (!device || !context) {
			return;
		}

		const std::uint64_t now = ::GetTickCount64();
		const float delta = g_lastFrameTicks == 0 ?
			1.0f / 90.0f :
			static_cast<float>(now - g_lastFrameTicks) / 1000.0f;
		g_lastFrameTicks = now;
		// One load per dispatch, latched into a render-thread-owned bool, which is the
		// rule `wanted` above follows: the fetch below and the draw inside BuildFrame both
		// read the latch and never the setting, so a perfTimers change landing mid-frame
		// cannot make the panel fetch a report it then does not draw, or draw one it did
		// not fetch.
		g_timersOn = Telemetry::Enabled();
		if (!g_timersOn) {
			// Nothing is fetched and nothing is kept while the timers are off, which is
			// the shipped default. The section draws "timers off" from the empty state,
			// and the string this module was holding is given back rather than left to
			// go stale for the rest of the session.
			g_telemetryText.clear();
			g_lastTelemetryMs = 0;
		} else if (now - g_lastTelemetryMs >= kTelemetryRefreshMs || g_telemetryText.empty()) {
			g_lastTelemetryMs = now;
			try {
				g_telemetryText = Telemetry::IntervalSummary();
			} catch (...) {
				g_telemetryText.clear();
			}
		}

		// ImGui asserts on a non-positive delta and a tick clock can report zero between
		// two frames; clamp to something a frame could plausibly have taken.
		if (DrawAndSubmit(context, delta > 0.0f ? delta : 1.0f / 90.0f)) {
			// Only a frame that drew is submitted. A refused isolation scope leaves the
			// texture holding the last frame that did draw - which the compositor is
			// already showing - and on the first visible frame leaves it holding nothing
			// D3D11 defines, which is why the submit waits for the first real draw rather
			// than merely for a frame. framesDrawn and textureSubmits therefore advance
			// together, which is the invariant /state's panel block exists to show.
			SubmitTexture();
		}
		PumpEvents();
		// Last, and only on a visible frame that has painted the texture at least once: a
		// capture is a readback of what was drawn, so an armed request simply waits until
		// the panel is shown again. That is what /panel's own description tells a caller
		// to expect, and it is the same reason a never-drawn texture is not read back.
		if (g_texturePainted) {
			ServiceCapture(device, context);
		}
	}

	std::uint64_t RequestCapture() noexcept
	{
		const std::uint64_t requested = g_captureRequested.load(std::memory_order_acquire);
		if (requested != g_captureCompleted.load(std::memory_order_acquire)) {
			return 0;
		}
		return g_captureRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	State SaveCaptureAndSnapshot()
	{
		const std::uint64_t completed = g_captureCompleted.load(std::memory_order_acquire);
		if (completed != 0 && completed != g_captureSaved.load(std::memory_order_acquire)) {
			std::vector<std::uint8_t> pixels;
			{
				std::scoped_lock lock(g_captureMutex);
				pixels.swap(g_capturePixels);
			}
			if (pixels.empty()) {
				g_captureSaved.store(completed, std::memory_order_release);
			} else {
				auto directory = StereoCapture::CaptureDirectory();
				std::string error;
				if (directory.empty()) {
					PublishString(g_captureError, "no writable captures directory could be resolved"sv);
				} else {
					std::error_code ec;
					std::filesystem::create_directories(directory, ec);
					const auto path = directory /
						("panel_" + std::to_string(::GetTickCount64()) + "_" + std::to_string(completed) + ".bmp");
					if (StereoCapture::WriteBmp(path, pixels, kTextureWidth, kTextureHeight, error)) {
						PublishString(g_capturePath, path.string());
						PublishString(g_captureError, ""sv);
					} else {
						PublishString(g_captureError, error);
					}
				}
				g_captureSaved.store(completed, std::memory_order_release);
			}
		}
		return Snapshot();
	}

	State Snapshot() noexcept
	{
		State out{};
		try {
			out.enabled = g_enabled.load(std::memory_order_acquire);
			out.runtimeResolved = g_runtimeResolved.load(std::memory_order_acquire);
			out.interfaceVersion = ReadString(g_interfaceVersion);
			out.lastInitError = g_lastInitError.load(std::memory_order_relaxed);
			out.overlayCreated = g_overlayCreated.load(std::memory_order_acquire);
			out.overlayHandle = g_overlayHandle.load(std::memory_order_relaxed);
			out.visible = g_visible.load(std::memory_order_acquire);
			out.lastOverlayError = ReadString(g_lastOverlayError);
			out.overlayErrors = g_overlayErrors.load(std::memory_order_relaxed);
			out.framesDrawn = g_framesDrawn.load(std::memory_order_relaxed);
			out.textureSubmits = g_textureSubmits.load(std::memory_order_relaxed);
			out.eventsPolled = g_eventsPolled.load(std::memory_order_relaxed);
			out.eventsMouseMove = g_eventsMouseMove.load(std::memory_order_relaxed);
			out.eventsMouseButtonDown = g_eventsMouseButtonDown.load(std::memory_order_relaxed);
			out.eventsMouseButtonUp = g_eventsMouseButtonUp.load(std::memory_order_relaxed);
			out.eventsScrollSmooth = g_eventsScrollSmooth.load(std::memory_order_relaxed);
			out.eventsOverlayShown = g_eventsOverlayShown.load(std::memory_order_relaxed);
			out.eventsOverlayHidden = g_eventsOverlayHidden.load(std::memory_order_relaxed);
			out.eventsOther = g_eventsOther.load(std::memory_order_relaxed);
			out.captureSequence = g_captureRequested.load(std::memory_order_acquire);
			out.captureCompletedSequence = g_captureCompleted.load(std::memory_order_acquire);
			const std::uint64_t saved = g_captureSaved.load(std::memory_order_acquire);
			out.capturePath = ReadString(g_capturePath);
			out.captureError = ReadString(g_captureError);
			if (out.captureSequence == 0) {
				out.captureState = "idle";
			} else if (out.captureSequence != out.captureCompletedSequence) {
				out.captureState = "armed";
			} else if (saved != out.captureCompletedSequence) {
				out.captureState = "captured";
			} else if (!out.captureError.empty()) {
				out.captureState = "failed";
			} else {
				out.captureState = "saved";
			}
			out.textureSharedFlag = ReadString(g_textureSharedFlag);
			out.widthMeters = g_widthMeters.load(std::memory_order_acquire);
			out.distanceMeters = g_distanceMeters.load(std::memory_order_acquire);
			out.creations = g_creations.load(std::memory_order_relaxed);
			out.teardowns = g_teardowns.load(std::memory_order_relaxed);
			out.creationFailures = g_creationFailures.load(std::memory_order_relaxed);
			out.telemetryDrawn = g_telemetryDrawn.load(std::memory_order_relaxed);
		} catch (...) {
			// Every string field above allocates, and a diagnostic report is the last
			// thing that should end a session. A partial block beats a terminate.
		}
		return out;
	}
}
