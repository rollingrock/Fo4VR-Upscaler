#include "Upscaler/Panel.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <d3d11.h>

#include "Settings/Configuration.h"
#include "Settings/Writer.h"
#include "Upscaler/Common.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/Evaluate.h"
#include "Upscaler/HDRRoundTrip.h"
#include "Upscaler/Hotkeys.h"
#include "Upscaler/HudMessages.h"
#include "Upscaler/PanelModel.h"
#include "Upscaler/ResetPolicy.h"
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
		namespace Model = Upscaler::PanelModel;

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

		// Where a status row's value starts, and how wide the three widget groups are.
		// Chosen so three mode buttons and six preset buttons each fit one 1024 px row
		// with the style's own spacing; nothing here is measured, and nothing downstream
		// depends on the numbers - a driver clicks by the rectangle the draw records.
		constexpr float kValueColumn = 260.0f * kUiScale;
		constexpr float kModeButtonWidth = 148.0f * kUiScale;
		constexpr float kPresetButtonWidth = 70.0f * kUiScale;
		constexpr float kSliderWidth = 230.0f * kUiScale;
		constexpr float kHoldButtonWidth = 200.0f * kUiScale;
		// The timing pane's own height. Fixed rather than "the rest of the window" since
		// the controls sit above it: a pane that took the remainder would push every
		// control off the texture the moment perfTimers came on. Sized so the whole
		// window - status, controls, result line and this - fits the texture without a
		// scrollbar, because a control scrolled out of view is one a recorded rectangle
		// can no longer be clicked at.
		constexpr float kTimerPaneHeight = 60.0f * kUiScale;

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
		// The live settings values the panel's controls sit on. Published by the same
		// apply that publishes every other module's configuration, never read out of the
		// global Setting<T> instances - a load can be halfway through rewriting those, and
		// the panel runs on the frame producer.
		std::atomic<double> g_liveRenderScaleOverride{ 0.0 };
		std::atomic<double> g_liveSharpness{ 0.0 };

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
		std::atomic<std::uint64_t> g_eventsInjected{ 0 };
		std::atomic<std::uint64_t> g_eventsInjectedDropped{ 0 };
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
		// store can never hand a reader a half-written string. All published on change
		// only, never per frame.
		std::atomic<std::shared_ptr<const std::string>> g_lastOverlayError{
			std::make_shared<const std::string>("None")
		};
		std::atomic<std::shared_ptr<const std::string>> g_interfaceVersion{
			std::make_shared<const std::string>(vr::IVROverlay_Version)
		};
		std::atomic<std::shared_ptr<const std::string>> g_textureSharedFlag{
			std::make_shared<const std::string>("not-measured")
		};
		std::atomic<std::shared_ptr<const std::string>> g_livePreset{
			std::make_shared<const std::string>("auto")
		};
		std::atomic<std::shared_ptr<const std::string>> g_lastIntent{
			std::make_shared<const std::string>("none")
		};
		std::atomic<std::shared_ptr<const std::string>> g_lastRefusal{
			std::make_shared<const std::string>("none")
		};
		std::atomic<std::shared_ptr<const std::string>> g_resultLine{
			std::make_shared<const std::string>("no change requested yet")
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

		// --- the settings worker ----------------------------------------------------
		//
		// Settings::Writer::Set blocks, bounded, on the game thread draining a queued
		// write. From the render thread that is a stall of up to its whole budget on the
		// frame producer, so every intent that touches the writer is handed to this one
		// thread instead. The A/B veto is deliberately not one of them: it is a single
		// atomic store and goes straight from the seam.
		//
		// One slot, and the seam never waits for it. The four states below are handed
		// between exactly two threads - the render thread owns empty -> filling -> ready
		// and the worker owns ready -> running -> empty - so one compare_exchange on each
		// hand-off is the whole protocol. A post that finds the slot occupied is the
		// refusal the model already names; nothing is queued behind it, for the same
		// reason a second mode switch is refused busy rather than stacked.
		constexpr std::uint32_t kSlotEmpty = 0;
		constexpr std::uint32_t kSlotFilling = 1;
		constexpr std::uint32_t kSlotReady = 2;
		constexpr std::uint32_t kSlotRunning = 3;

		std::atomic<std::uint32_t> g_slot{ kSlotEmpty };
		// Written by the render thread only while it holds kSlotFilling and read by the
		// worker only while it holds kSlotRunning, so the state store/load pair above is
		// the whole synchronisation these need.
		char g_slotKey[64]{};
		char g_slotValue[64]{};

		std::mutex g_workerStartMutex;
		bool g_workerThreadStarted = false;
		bool g_workerStartFailed = false;
		std::atomic_bool g_workerRunning{ false };
		std::atomic_bool g_workerUnavailable{ false };

		std::atomic<std::uint64_t> g_intentsQueued{ 0 };
		std::atomic<std::uint64_t> g_intentsCompleted{ 0 };
		std::atomic<std::uint64_t> g_intentsRefused{ 0 };

		// Settings::Writer::SetResult, copied out of the worker's own call. Published
		// before the slot is freed, so a reader that sees the slot empty sees the result
		// that emptied it.
		struct SetOutcome
		{
			bool accepted = false;
			bool persisted = false;
			bool tookEffectNow = false;
			bool needsRestart = false;
			std::string key;
			std::string value;
			std::string reason;
		};
		std::atomic<std::shared_ptr<const SetOutcome>> g_lastOutcome{ nullptr };

		void CopyInto(char (&a_buffer)[64], std::string_view a_text) noexcept
		{
			const std::size_t count = a_text.size() < sizeof(a_buffer) - 1 ? a_text.size() : sizeof(a_buffer) - 1;
			std::memcpy(a_buffer, a_text.data(), count);
			a_buffer[count] = '\0';
		}

		void WorkerMain() noexcept
		{
			for (;;) {
				g_slot.wait(kSlotEmpty, std::memory_order_acquire);
				std::uint32_t expected = kSlotReady;
				if (!g_slot.compare_exchange_strong(
						expected, kSlotRunning, std::memory_order_acq_rel, std::memory_order_acquire)) {
					// kSlotFilling: the render thread has claimed the slot and is still
					// writing the two buffers. That window is a pair of memcpys, so yield
					// rather than park - parking here would need the producer to notify
					// twice for one intent.
					std::this_thread::yield();
					continue;
				}
				try {
					std::string key{ g_slotKey };
					std::string value{ g_slotValue };
					const auto result = Settings::Writer::Set(key, value);
					auto outcome = std::make_shared<SetOutcome>();
					outcome->accepted = result.accepted;
					outcome->persisted = result.persisted;
					outcome->tookEffectNow = result.tookEffectNow;
					outcome->needsRestart = result.needsRestart;
					outcome->key = result.key;
					outcome->value = result.value;
					outcome->reason = result.reason;
					g_lastOutcome.store(
						std::shared_ptr<const SetOutcome>(std::move(outcome)), std::memory_order_release);
					logger::info(FMT_STRING("panel: '{}' = '{}' -> accepted {}, persisted {}, took effect now {}, needs restart {}{}{}"),
						key, value, result.accepted, result.persisted, result.tookEffectNow, result.needsRestart,
						result.reason.empty() ? "" : " - ", result.reason);
				} catch (...) {
					logger::error("panel: the settings worker caught an exception; the slot is freed so the next intent is not refused forever"sv);
				}
				g_intentsCompleted.fetch_add(1, std::memory_order_relaxed);
				// Freed last, and after the outcome is published: the slot being empty is
				// what tells the next frame the result it can now draw is this one's.
				g_slot.store(kSlotEmpty, std::memory_order_release);
			}
		}

		// The render thread's own post. A compare_exchange, two memcpys and a notify - no
		// lock, no allocation, no wait. False is the refusal, not a failure.
		[[nodiscard]] bool PostSetKey(std::string_view a_key, std::string_view a_value) noexcept
		{
			if (!g_workerRunning.load(std::memory_order_acquire)) {
				return false;
			}
			std::uint32_t expected = kSlotEmpty;
			if (!g_slot.compare_exchange_strong(
					expected, kSlotFilling, std::memory_order_acq_rel, std::memory_order_acquire)) {
				return false;
			}
			CopyInto(g_slotKey, a_key);
			CopyInto(g_slotValue, a_value);
			g_slot.store(kSlotReady, std::memory_order_release);
			g_slot.notify_one();
			g_intentsQueued.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// --- the operator veto ------------------------------------------------------
		//
		// Ownership is tracked exactly the way Hotkeys' COMPARE tracks its own: this panel
		// claims the veto only when it finds one not already standing, and releases only
		// an engagement it still owns. A veto set through /ab or by the COMPARE key is
		// therefore never cleared by the panel's hold coming up.
		std::atomic_bool g_panelOwnsVeto{ false };
		std::atomic<std::uint64_t> g_vetoEngagements{ 0 };
		std::atomic<std::uint64_t> g_vetoReleases{ 0 };
		std::atomic<std::uint64_t> g_vetoDeferred{ 0 };
		std::atomic_bool g_holdPublished{ false };
		std::atomic_bool g_latchPublished{ false };

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

		// The two A/B controls, and the level this panel last acted on. Reconciled every
		// frame from (hold || latch), the same unconditional reconciliation COMPARE runs -
		// a transition swallowed by a hidden frame can never leave the world gate denying
		// with nobody holding it.
		bool g_holdEngaged = false;
		bool g_latchOn = false;
		bool g_vetoDispatched = false;

		// The two sliders' own values. They live across frames because a drag has to: the
		// live value is copied in only while the widget is not being dragged, or every
		// frame would snap the handle back under the operator's hand.
		float g_ratioSlider = 0.0f;
		float g_sharpnessSlider = 0.0f;
		bool g_ratioSliderActive = false;
		bool g_sharpnessSliderActive = false;
		bool g_holdButtonActive = false;

		// The three VK codes the panel prints, and when they were last read.
		// Hotkeys::Snapshot derives its remembered mode from a whole
		// Configuration::SwitchSnapshot of its own, and this draw already takes one - two
		// per visible frame on the frame producer, for three integers that move only when a
		// settings apply moves them. Read on a floor instead.
		std::int64_t g_keyToggleVk = 0;
		std::int64_t g_keyCompareVk = 0;
		std::int64_t g_keyPanelVk = 0;
		std::uint64_t g_lastKeysMs = 0;
		constexpr std::uint64_t kKeysRefreshMs = 1000;

		// The one input the draw produced this frame, dispatched after the ImGui frame is
		// closed and outside the isolation scope: nothing about a settings write or a veto
		// belongs inside a bracket that is holding the engine's pipeline state.
		Model::Action g_pendingAction{};

		// The last published mode tags and result line, so both are republished on change
		// only - building a vector of six strings every frame would allocate on the frame
		// producer for a value that moves about once a minute.
		Model::ModeButton g_publishedModes[Model::kModeCount]{};
		bool g_publishedModesValid = false;
		std::string g_publishedResultLine{ "no change requested yet" };

		LogGate g_lookupFailureGate{ 30000 };
		LogGate g_createFailureGate{ 30000 };
		LogGate g_submitFailureGate{ 30000 };
		LogGate g_isolationFailureGate{ 30000 };
		LogGate g_vetoDeferralGate{ 30000 };
		LogGate g_workerUnavailableGate{ 30000 };

		// --- the control rectangles --------------------------------------------------
		//
		// The draw writes into a render-thread-owned scratch array with no lock at all, and
		// the finished frame is copied into the published one under try_to_lock - the same
		// division of labour the capture handoff makes with this thread. A frame the
		// listener is mid-read simply is not published, which costs a frame of staleness on
		// a table that changes only when the layout does, and never a wait on the frame
		// producer. A seqlock was the first shape here and was wrong: a reader spins its
		// whole retry budget inside one draw, so a read that lands in the write window
		// answers empty instead of retrying into the next frame.
		struct RectRecord
		{
			float minX = 0.0f;
			float minY = 0.0f;
			float maxX = 0.0f;
			float maxY = 0.0f;
			double value = 0.0;
			bool active = false;
			bool present = false;
		};
		RectRecord g_rects[Model::kControlCount]{};  // render-thread-owned scratch
		std::mutex g_rectMutex;
		RectRecord g_rectsPublished[Model::kControlCount]{};  // guarded by g_rectMutex

		void RecordRect(Model::ControlId a_control, double a_value, bool a_active) noexcept
		{
			const auto index = static_cast<std::size_t>(a_control);
			if (index >= Model::kControlCount) {
				return;
			}
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			RectRecord& record = g_rects[index];
			record.minX = min.x;
			record.minY = min.y;
			record.maxX = max.x;
			record.maxY = max.y;
			record.value = a_value;
			record.active = a_active;
			record.present = true;
		}

		// try_to_lock, never a wait: this is the frame producer. A listener still reading
		// means this frame's table is not published, which is a frame of staleness on a
		// table that only moves when the layout does.
		void PublishRects() noexcept
		{
			std::unique_lock lock(g_rectMutex, std::try_to_lock);
			if (!lock.owns_lock()) {
				return;
			}
			for (std::size_t i = 0; i < Model::kControlCount; ++i) {
				g_rectsPublished[i] = g_rects[i];
			}
		}

		// --- injected input ------------------------------------------------------------
		//
		// A synthetic event arrives on the DevBench listener thread, which may not touch
		// ImGui: only the render thread may. The listener appends here and the seam drains
		// it in the same place PollNextOverlayEvent's own events are applied, through the
		// same handler, so a headless click and a laser click take one code path. The
		// drain is try_to_lock and never a wait - the same division of labour the capture
		// handoff already makes with this thread.
		struct InjectedEvent
		{
			EventKind kind = EventKind::kMove;
			float x = 0.0f;
			float y = 0.0f;
			float delta = 0.0f;
			std::uint32_t button = 0;
		};
		constexpr std::size_t kInjectCapacity = 256;
		// How long an injected pointer owns the panel. VERIFIED 2026-09-05 under SteamVR's
		// null driver, with no headset and no controller in the room: the runtime feeds this
		// overlay a steady stream of VREvent_MouseMove of its own - thousands of them across
		// a ninety-second hold, tens a second against a frame rate several times that - so a
		// synthetic click that is not given priority is dragged off its widget by the
		// runtime's own pointer before the release lands, and ImGui reports no click at all.
		// While the window is open a real mouse event is counted and not applied; every
		// injected event refreshes it, so a driver holding a button keeps the panel by
		// injecting a move now and then. Nothing opens this window except /panel
		// action=event, so the laser is untouched in a session nobody is driving headlessly.
		constexpr std::uint64_t kInjectOwnsPointerMs = 2000;
		std::mutex g_injectMutex;
		std::vector<InjectedEvent> g_injectQueue;
		std::vector<InjectedEvent> g_injectDrain;  // render-thread-owned, swapped in
		std::uint64_t g_injectOwnsPointerUntilMs = 0;  // render-thread-owned
		std::atomic<std::uint64_t> g_eventsSuppressedByInjection{ 0 };

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

		// The six mode tags as the last drawn frame computed them, republished on change.
		std::atomic<std::shared_ptr<const std::vector<ModeTag>>> g_modeTags{ nullptr };

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

		// --- veto dispatch ------------------------------------------------------------

		void ReconcileVeto(bool a_want) noexcept
		{
			if (a_want == g_vetoDispatched) {
				return;
			}
			g_vetoDispatched = a_want;
			if (a_want) {
				// Precedence against /ab and the COMPARE key: the veto is claimed only
				// when this read finds none standing, so the release below can never clear
				// somebody else's.
				if (ResetPolicy::OperatorVetoActive()) {
					g_panelOwnsVeto.store(false, std::memory_order_release);
					g_vetoDeferred.fetch_add(1, std::memory_order_relaxed);
					if (g_vetoDeferralGate.Ready()) {
						logger::info("panel: the operator veto is already engaged elsewhere; the panel's own hold does nothing further and releasing it will not clear it"sv);
					}
					return;
				}
				ResetPolicy::SetOperatorVeto(true);
				g_panelOwnsVeto.store(true, std::memory_order_release);
				g_vetoEngagements.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			bool expected = true;
			if (g_panelOwnsVeto.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
				ResetPolicy::SetOperatorVeto(false);
				g_vetoReleases.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// A hold nobody can see is a hold nobody can release: the button only stops being
		// active on a frame that draws it, so a hidden, invisible or torn-down panel
		// releases it here instead. The latch is deliberately not dropped on a hide - it
		// is a latch, and the operator can show the panel again to clear it - but a
		// teardown takes both, because a disabled panel has no control left to clear one
		// with.
		void ReleaseHold() noexcept
		{
			if (g_holdEngaged) {
				g_holdEngaged = false;
				g_holdPublished.store(false, std::memory_order_release);
			}
			ReconcileVeto(g_latchOn);
		}

		void ReleaseAllVeto() noexcept
		{
			if (!g_holdEngaged && !g_latchOn && !g_vetoDispatched) {
				return;
			}
			g_holdEngaged = false;
			g_latchOn = false;
			g_holdPublished.store(false, std::memory_order_release);
			g_latchPublished.store(false, std::memory_order_release);
			ReconcileVeto(false);
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
			ReleaseAllVeto();
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

		// --- the drawn window ---------------------------------------------------------

		void Row(const char* a_label, const std::string& a_value)
		{
			ImGui::TextUnformatted(a_label);
			ImGui::SameLine(kValueColumn);
			ImGui::TextUnformatted(a_value.c_str());
		}

		void RowRaw(const char* a_label, const char* a_value)
		{
			ImGui::TextUnformatted(a_label);
			ImGui::SameLine(kValueColumn);
			ImGui::TextUnformatted(a_value);
		}

		// Every value here comes out of a snapshot some other module already publishes.
		// Nothing is derived, compared or recomputed: the panel and DevBench are two
		// clients of one surface, and a number the panel showed that /state did not would
		// be a second source of truth.
		void DrawStatus(const Configuration::ModeSwitchState& a_modeSwitch)
		{
			const auto isolation = HDRRoundTrip::Snapshot();
			const auto hud = HudMessages::Snapshot();

			ImGui::TextUnformatted("Fo4VR Upscaler");
			ImGui::Separator();
			Row("mode", a_modeSwitch.liveMode + "  (" + a_modeSwitch.modeSource + ")");
			Row("backend", std::string(HDRRoundTrip::StereoBackendName(
								static_cast<HDRRoundTrip::StereoBackend>(isolation.stereoBackend))));
			Row("preset", Evaluate::RequestedPreset() + "  ->  " + Evaluate::ReportedPreset());
			Row("session",
				std::string(HDRRoundTrip::SessionStateName(
					static_cast<HDRRoundTrip::SessionState>(isolation.sessionState))) +
					"  (" +
					std::string(HDRRoundTrip::RecoveryRefusalName(
						static_cast<HDRRoundTrip::RecoveryRefusal>(isolation.recoveryRefusal))) +
					")");
			Row("last HUD line", hud.lastMessage);
		}

		// The controls. Everything drawn here comes from the model's output and every
		// input leaves as one Model::Action - this function contains no policy: which
		// mode is reachable, what a slider release is worth and whether an intent is
		// refused are all decided in PanelModel.h, where they are proved at compile time.
		void DrawControls(const Model::Output& a_out)
		{
			char buffer[96];

			// No header row: a button with no note under it is one that takes effect now,
			// and the notes below say which ones do not. The window has to fit the texture
			// - a control scrolled out of view is one its recorded rectangle can no longer
			// be clicked at - so every row here earns its height.
			ImGui::Separator();
			for (std::size_t i = 0; i < Model::kModeCount; ++i) {
				const auto& button = a_out.modes[i];
				if (i % 3 != 0) {
					ImGui::SameLine();
				}
				if (button.current) {
					std::snprintf(buffer, sizeof(buffer), "> %s", button.label.CStr());
				} else {
					std::snprintf(buffer, sizeof(buffer), "%s", button.label.CStr());
				}
				if (ImGui::Button(buffer, ImVec2(kModeButtonWidth, 0.0f))) {
					g_pendingAction = Model::Action{ Model::ActionKind::kButton, button.control, 0.0, false };
				}
				RecordRect(button.control, 0.0, ImGui::IsItemActive());
			}
			for (const auto& button : a_out.modes) {
				if (!button.live) {
					std::snprintf(buffer, sizeof(buffer), "%s: %s", button.label.CStr(), button.tag.CStr());
					ImGui::TextUnformatted(buffer);
				}
			}

			if (!g_ratioSliderActive) {
				g_ratioSlider = static_cast<float>(a_out.ratio.value);
			}
			ImGui::TextUnformatted("Ratio");
			ImGui::SameLine(kValueColumn);
			ImGui::SetNextItemWidth(kSliderWidth);
			ImGui::SliderFloat("##ratio", &g_ratioSlider,
				static_cast<float>(a_out.ratio.minimum), static_cast<float>(a_out.ratio.maximum), "%.4f");
			g_ratioSliderActive = ImGui::IsItemActive();
			RecordRect(Model::ControlId::kRatioSlider, static_cast<double>(g_ratioSlider), g_ratioSliderActive);
			// Written on release, never per tick: every tick of a drag would be a settings
			// write, a file replace and a whole Configuration::Apply.
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				g_pendingAction = Model::Action{
					Model::ActionKind::kSliderRelease, Model::ControlId::kRatioSlider,
					static_cast<double>(g_ratioSlider), false
				};
			}
			RowRaw("render", a_out.ratio.sizeText.CStr());
			if (a_out.ratio.fullWouldBeRefused) {
				ImGui::TextUnformatted("1.00 is a mode change beside a reduced mode - the writer refuses it");
			}

			ImGui::TextUnformatted("Preset");
			for (const auto& entry : a_out.presets) {
				if (entry.note.length != 0) {
					ImGui::SameLine(kValueColumn);
					std::snprintf(buffer, sizeof(buffer), "%s: %s", entry.label.CStr(), entry.note.CStr());
					ImGui::TextUnformatted(buffer);
				}
			}
			for (std::size_t i = 0; i < Model::kPresetCount; ++i) {
				const auto& entry = a_out.presets[i];
				if (i != 0) {
					ImGui::SameLine();
				}
				if (entry.current) {
					std::snprintf(buffer, sizeof(buffer), "> %s", entry.label.CStr());
				} else {
					std::snprintf(buffer, sizeof(buffer), "%s", entry.label.CStr());
				}
				if (ImGui::Button(buffer, ImVec2(kPresetButtonWidth, 0.0f))) {
					g_pendingAction = Model::Action{ Model::ActionKind::kButton, entry.control, 0.0, false };
				}
				RecordRect(entry.control, 0.0, ImGui::IsItemActive());
			}

			if (!g_sharpnessSliderActive) {
				g_sharpnessSlider = static_cast<float>(a_out.sharpness.value);
			}
			ImGui::TextUnformatted("Sharpness");
			ImGui::SameLine(kValueColumn);
			ImGui::SetNextItemWidth(kSliderWidth);
			ImGui::SliderFloat("##sharpness", &g_sharpnessSlider,
				static_cast<float>(a_out.sharpness.minimum), static_cast<float>(a_out.sharpness.maximum), "%.3f");
			g_sharpnessSliderActive = ImGui::IsItemActive();
			RecordRect(Model::ControlId::kSharpnessSlider, static_cast<double>(g_sharpnessSlider), g_sharpnessSliderActive);
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				g_pendingAction = Model::Action{
					Model::ActionKind::kSliderRelease, Model::ControlId::kSharpnessSlider,
					static_cast<double>(g_sharpnessSlider), false
				};
			}

			ImGui::Separator();
			ImGui::TextUnformatted("A/B");
			static_cast<void>(ImGui::Button("Hold for native", ImVec2(kHoldButtonWidth, 0.0f)));
			// The edge is the button's own active state, not its click: a hold is the one
			// control here whose whole value is what happens between press and release.
			const bool holdActive = ImGui::IsItemActive();
			RecordRect(Model::ControlId::kHoldNative, holdActive ? 1.0 : 0.0, holdActive);
			if (holdActive != g_holdButtonActive) {
				g_holdButtonActive = holdActive;
				g_pendingAction = Model::Action{
					Model::ActionKind::kHoldEdge, Model::ControlId::kHoldNative, 0.0, holdActive
				};
			}
			ImGui::SameLine();
			bool latch = a_out.latchOn;
			if (ImGui::Checkbox("Native", &latch)) {
				g_pendingAction = Model::Action{
					Model::ActionKind::kToggle, Model::ControlId::kNativeLatch, 0.0, latch
				};
			}
			RecordRect(Model::ControlId::kNativeLatch, latch ? 1.0 : 0.0, ImGui::IsItemActive());
			if (a_out.vetoActive) {
				ImGui::SameLine();
				ImGui::TextUnformatted(a_out.vetoOwnedByPanel ? "native (this panel)" : "native (held elsewhere)");
			}

			bool timers = a_out.timersOn;
			if (ImGui::Checkbox("timers", &timers)) {
				g_pendingAction = Model::Action{
					Model::ActionKind::kToggle, Model::ControlId::kTimers, 0.0, timers
				};
			}
			RecordRect(Model::ControlId::kTimers, timers ? 1.0 : 0.0, ImGui::IsItemActive());
			ImGui::SameLine(kValueColumn);
			ImGui::TextUnformatted(a_out.keysText.CStr());

			ImGui::Separator();
			ImGui::TextWrapped("%s", a_out.resultLine.CStr());
		}

		// Publishes the two model outputs /state reports, on change only: building six
		// strings and a std::string every frame would allocate on the frame producer for
		// values that move about once a minute.
		void PublishModelOutput(const Model::Output& a_out) noexcept
		{
			bool changed = !g_publishedModesValid;
			for (std::size_t i = 0; i < Model::kModeCount && !changed; ++i) {
				const auto& a = g_publishedModes[i];
				const auto& b = a_out.modes[i];
				changed = a.mode != b.mode || a.live != b.live || a.current != b.current ||
				          a.outcome != b.outcome || a.tag.View() != b.tag.View();
			}
			if (changed) {
				try {
					auto tags = std::make_shared<std::vector<ModeTag>>();
					tags->reserve(Model::kModeCount);
					for (const auto& button : a_out.modes) {
						ModeTag tag{};
						tag.mode.assign(button.mode);
						tag.label.assign(button.label.View());
						tag.tag.assign(button.tag.View());
						tag.outcome.assign(Settings::Policy::SwitchOutcomeName(button.outcome));
						tag.live = button.live;
						tag.current = button.current;
						tags->push_back(std::move(tag));
					}
					g_modeTags.store(
						std::shared_ptr<const std::vector<ModeTag>>(std::move(tags)), std::memory_order_release);
					for (std::size_t i = 0; i < Model::kModeCount; ++i) {
						g_publishedModes[i] = a_out.modes[i];
					}
					g_publishedModesValid = true;
				} catch (...) {
					// A stale tag list is a worse report, never a wrong one: the next
					// frame tries again because g_publishedModes was not advanced.
				}
			}

			if (g_publishedResultLine != a_out.resultLine.View()) {
				try {
					g_publishedResultLine.assign(a_out.resultLine.View());
					PublishString(g_resultLine, g_publishedResultLine);
				} catch (...) {
				}
			}
		}

		// Its caller wraps it, because every row here builds a std::string and this runs
		// under a noexcept frame producer: an escaped bad_alloc would terminate the
		// process over a diagnostic line. Caught between Begin and End the ImGui frame is
		// still well formed - the window has been opened and will be closed either way.
		void DrawWindowGuarded()
		{
			// Held as locals for the whole call: the model's inputs are string_views into
			// these snapshots, so they have to outlive the reduce and the draw.
			const auto modeSwitch = Configuration::SwitchSnapshot();
			const auto dynamic = DynamicResolution::Snapshot();
			const auto preset = ReadString(g_livePreset);
			const auto outcome = g_lastOutcome.load(std::memory_order_acquire);

			Model::Input input{};
			input.modeSwitch.liveMode = modeSwitch.liveMode;
			input.modeSwitch.modeSource = modeSwitch.modeSource;
			input.modeSwitch.sessionArmedMode = modeSwitch.sessionArmedMode;
			input.modeSwitch.lastNonOffMode = modeSwitch.lastNonOffMode;
			input.modeSwitch.bracketArmed = modeSwitch.bracketArmed;
			input.ratio.requestedScale = dynamic.requestedScale;
			input.ratio.renderWidth = dynamic.snappedRenderWidth;
			input.ratio.renderHeight = dynamic.snappedRenderHeight;
			input.ratio.outputWidth = dynamic.snappedOutputWidth;
			input.ratio.outputHeight = dynamic.snappedOutputHeight;
			input.live.preset = preset;
			input.live.renderScaleOverride = g_liveRenderScaleOverride.load(std::memory_order_acquire);
			input.live.sharpness = g_liveSharpness.load(std::memory_order_acquire);
			input.live.perfTimers = g_timersOn;
			if (outcome) {
				input.lastResult.present = true;
				input.lastResult.accepted = outcome->accepted;
				input.lastResult.persisted = outcome->persisted;
				input.lastResult.tookEffectNow = outcome->tookEffectNow;
				input.lastResult.needsRestart = outcome->needsRestart;
				input.lastResult.key = outcome->key;
				input.lastResult.value = outcome->value;
				input.lastResult.reason = outcome->reason;
			}
			input.veto.active = ResetPolicy::OperatorVetoActive();
			input.veto.ownedByPanel = g_panelOwnsVeto.load(std::memory_order_acquire);
			input.keys.toggleVk = g_keyToggleVk;
			input.keys.compareVk = g_keyCompareVk;
			input.keys.panelVk = g_keyPanelVk;
			input.intentInFlight = g_slot.load(std::memory_order_acquire) != kSlotEmpty;
			input.holdEngaged = g_holdEngaged;
			input.latchOn = g_latchOn;

			const auto out = Model::Reduce(input);
			DrawStatus(modeSwitch);
			DrawControls(out);
			PublishModelOutput(out);

			ImGui::Separator();
			// The timing section is drawn only when the timers are running, which is what
			// the setting, the README and this panel's own /state block all promise. The
			// value drawn is whatever the fetch in PollFrame last put in g_telemetryText:
			// the fetch and this draw read the same latch on the same frame, so the
			// section cannot show a window that has since been switched off.
			if (g_timersOn) {
				// A fixed-height child so the controls above never move: 23 intervals do
				// not fit under them, and a panel that shifts when the timers come on is
				// worse than one whose bottom pane clips. The pane scrolls, and the
				// smooth-scroll events the pump already feeds ImGui are what scroll it.
				if (ImGui::BeginChild("timers", ImVec2(0.0f, kTimerPaneHeight), ImGuiChildFlags_Border)) {
					ImGui::TextUnformatted(g_telemetryText.c_str());
				}
				ImGui::EndChild();
			} else {
				ImGui::TextUnformatted("timers off");
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
			// The rectangle table is rewritten whole between these two increments, so a
			// reader either sees this frame's layout or retries - never half of two.
			for (auto& record : g_rects) {
				record.present = false;
			}
			if (ImGui::Begin("Fo4VR Upscaler##panel", nullptr, kFlags)) {
				try {
					DrawWindowGuarded();
				} catch (...) {
					ImGui::TextUnformatted("status unavailable");
				}
			}
			ImGui::End();
			PublishRects();
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

		// One input, one intent, dispatched after the ImGui frame is closed and outside
		// the isolation scope. A settings intent goes to the worker; the A/B pair goes
		// straight to ResetPolicy, because neither may wait on a settings write.
		void DispatchPendingAction(const Model::Input& a_input) noexcept
		{
			if (g_pendingAction.kind == Model::ActionKind::kNone) {
				return;
			}
			const auto action = g_pendingAction;
			g_pendingAction = Model::Action{};
			const auto intent = Model::Decide(a_input, action);
			switch (intent.kind) {
			case Model::IntentKind::kVetoHold:
				g_holdEngaged = intent.engaged;
				g_holdPublished.store(intent.engaged, std::memory_order_release);
				PublishString(g_lastIntent, intent.engaged ? "veto-hold engaged"sv : "veto-hold released"sv);
				break;
			case Model::IntentKind::kVetoLatch:
				g_latchOn = intent.engaged;
				g_latchPublished.store(intent.engaged, std::memory_order_release);
				PublishString(g_lastIntent, intent.engaged ? "veto-latch on"sv : "veto-latch off"sv);
				break;
			case Model::IntentKind::kSetKey: {
				char label[96];
				std::snprintf(label, sizeof(label), "set-key %s = %s", intent.key.CStr(), intent.value.CStr());
				if (PostSetKey(intent.key.View(), intent.value.View())) {
					PublishString(g_lastIntent, label);
					PublishString(g_lastRefusal, "none"sv);
				} else {
					// The worker refused the post. Either it is unavailable for the
					// session or it still owns the previous intent; the model already
					// names the second case, and this names the first.
					g_intentsRefused.fetch_add(1, std::memory_order_relaxed);
					const bool unavailable = !g_workerRunning.load(std::memory_order_acquire);
					PublishString(g_lastIntent, label);
					PublishString(g_lastRefusal,
						unavailable ? "worker-unavailable"sv : Model::RefusalName(Model::Refusal::kIntentInFlight));
					if (unavailable && g_workerUnavailableGate.Ready()) {
						logger::warn("panel: no settings worker is running, so panel writes are unavailable for this session"sv);
					}
				}
				break;
			}
			case Model::IntentKind::kNone:
			default:
				g_intentsRefused.fetch_add(1, std::memory_order_relaxed);
				PublishString(g_lastRefusal, Model::RefusalName(intent.refusal));
				break;
			}
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

		// The one handler. A laser event and an injected one differ only in where their
		// co-ordinates came from - by the time they reach here both are ImGui pixels with
		// a top-left origin - so a headless run exercises the pump the headset uses.
		void ApplyInputEvent(EventKind a_kind, float a_x, float a_y, int a_button, float a_delta) noexcept
		{
			ImGuiIO& io = ImGui::GetIO();
			switch (a_kind) {
			case EventKind::kMove:
				io.AddMousePosEvent(a_x, a_y);
				break;
			case EventKind::kDown:
				io.AddMousePosEvent(a_x, a_y);
				io.AddMouseButtonEvent(a_button, true);
				break;
			case EventKind::kUp:
				io.AddMousePosEvent(a_x, a_y);
				io.AddMouseButtonEvent(a_button, false);
				break;
			case EventKind::kScroll:
				io.AddMouseWheelEvent(0.0f, a_delta);
				break;
			}
		}

		void DrainInjectedEvents() noexcept
		{
			// try_to_lock, never a wait: this is the frame producer, and a listener still
			// appending costs one frame of latency rather than a stall.
			std::unique_lock lock(g_injectMutex, std::try_to_lock);
			if (!lock.owns_lock() || g_injectQueue.empty()) {
				return;
			}
			g_injectDrain.swap(g_injectQueue);
			lock.unlock();
			for (const auto& event : g_injectDrain) {
				ApplyInputEvent(event.kind, event.x, event.y, static_cast<int>(event.button), event.delta);
				g_eventsInjected.fetch_add(1, std::memory_order_relaxed);
			}
			if (!g_injectDrain.empty()) {
				g_injectOwnsPointerUntilMs = ::GetTickCount64() + kInjectOwnsPointerMs;
			}
			g_injectDrain.clear();
		}

		void PumpEvents() noexcept
		{
			ImGui::SetCurrentContext(g_imgui);
			// One load per dispatch, used by value for the whole drain: whether an injected
			// pointer currently owns the panel. See kInjectOwnsPointerMs.
			const bool injectedOwnsPointer = ::GetTickCount64() < g_injectOwnsPointerUntilMs;
			vr::VREvent_t event{};
			while (g_overlay->PollNextOverlayEvent(g_handle, &event, sizeof(event))) {
				g_eventsPolled.fetch_add(1, std::memory_order_relaxed);
				switch (event.eventType) {
				case vr::VREvent_MouseMove:
					g_eventsMouseMove.fetch_add(1, std::memory_order_relaxed);
					if (injectedOwnsPointer) {
						g_eventsSuppressedByInjection.fetch_add(1, std::memory_order_relaxed);
						break;
					}
					ApplyInputEvent(EventKind::kMove, event.data.mouse.x, FlipY(event.data.mouse.y), 0, 0.0f);
					break;
				case vr::VREvent_MouseButtonDown:
					g_eventsMouseButtonDown.fetch_add(1, std::memory_order_relaxed);
					if (injectedOwnsPointer) {
						g_eventsSuppressedByInjection.fetch_add(1, std::memory_order_relaxed);
						break;
					}
					ApplyInputEvent(EventKind::kDown, event.data.mouse.x, FlipY(event.data.mouse.y),
						ImGuiButton(event.data.mouse.button), 0.0f);
					break;
				case vr::VREvent_MouseButtonUp:
					g_eventsMouseButtonUp.fetch_add(1, std::memory_order_relaxed);
					if (injectedOwnsPointer) {
						g_eventsSuppressedByInjection.fetch_add(1, std::memory_order_relaxed);
						break;
					}
					ApplyInputEvent(EventKind::kUp, event.data.mouse.x, FlipY(event.data.mouse.y),
						ImGuiButton(event.data.mouse.button), 0.0f);
					break;
				case vr::VREvent_ScrollSmooth:
					g_eventsScrollSmooth.fetch_add(1, std::memory_order_relaxed);
					if (injectedOwnsPointer) {
						g_eventsSuppressedByInjection.fetch_add(1, std::memory_order_relaxed);
						break;
					}
					ApplyInputEvent(EventKind::kScroll, 0.0f, 0.0f, 0, event.data.scroll.ydelta);
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
			DrainInjectedEvents();
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
			// allocates four megabytes - both once per capture request, on a route no
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

	void Configure(
		bool a_enabled,
		double a_widthMeters,
		double a_distanceMeters,
		double a_renderScaleOverride,
		const std::string& a_preset,
		double a_sharpness) noexcept
	{
		g_enabled.store(a_enabled, std::memory_order_release);
		g_widthMeters.store(static_cast<float>(a_widthMeters), std::memory_order_release);
		g_distanceMeters.store(static_cast<float>(a_distanceMeters), std::memory_order_release);
		g_liveRenderScaleOverride.store(a_renderScaleOverride, std::memory_order_release);
		g_liveSharpness.store(a_sharpness, std::memory_order_release);
		PublishString(g_livePreset, a_preset);
	}

	bool StartWorker() noexcept
	{
		try {
			std::lock_guard lock(g_workerStartMutex);
			if (g_workerThreadStarted) {
				return true;
			}
			if (g_workerStartFailed) {
				return false;
			}
			try {
				std::thread(&WorkerMain).detach();
			} catch (...) {
				// Latched inside the lock, so the very next caller sees the failure
				// rather than trying again on whatever thread it happens to be.
				g_workerStartFailed = true;
				g_workerUnavailable.store(true, std::memory_order_release);
				throw;
			}
			g_workerThreadStarted = true;
			g_workerRunning.store(true, std::memory_order_release);
			logger::info("panel: settings worker started; a panel write runs there, never on the render or game thread"sv);
			return true;
		} catch (...) {
			logger::error("panel: the settings worker could not be started; panel writes are unavailable for this session"sv);
			return false;
		}
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

	bool InjectEvent(EventKind a_kind, float a_x, float a_y, std::uint32_t a_button, float a_delta) noexcept
	{
		try {
			std::lock_guard lock(g_injectMutex);
			if (g_injectQueue.size() >= kInjectCapacity) {
				g_eventsInjectedDropped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			g_injectQueue.push_back(InjectedEvent{ a_kind, a_x, a_y, a_delta, a_button });
			return true;
		} catch (...) {
			g_eventsInjectedDropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	void PollFrame() noexcept
	{
		const bool enabled = g_enabled.load(std::memory_order_acquire);
		if (!enabled) {
			if (g_overlayCreated.load(std::memory_order_acquire)) {
				Teardown();
			} else {
				// A disabled panel that never created an overlay can still be holding a
				// veto its last enabled frame engaged.
				ReleaseAllVeto();
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
			// A hidden panel costs the two atomic loads above, the hold release below and
			// nothing else: no ImGui frame, no draw, no texture submit and no event poll.
			// The release is not optional - the hold button only stops being active on a
			// frame that draws it, so hiding while holding would leave the world gate
			// denying with no control left to end it.
			ReleaseHold();
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
			ReleaseHold();
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
		if (now - g_lastKeysMs >= kKeysRefreshMs || g_lastKeysMs == 0) {
			g_lastKeysMs = now;
			const auto keys = Hotkeys::Snapshot();
			g_keyToggleVk = keys.toggleVk;
			g_keyCompareVk = keys.compareVk;
			g_keyPanelVk = keys.panelVk;
		}
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
		// Outside the isolation scope and after the ImGui frame is closed: neither a
		// settings post nor a veto store belongs inside a bracket holding the engine's
		// pipeline state, and neither touches D3D11 at all.
		{
			// Decide reads exactly one field of the input - whether an intent is in flight -
			// and it is read here rather than carried out of the draw deliberately: the
			// worker can finish between the frame that drew the button and this dispatch,
			// and refusing a click against a slot that is already free would lose it.
			Model::Input dispatchInput{};
			dispatchInput.intentInFlight = g_slot.load(std::memory_order_acquire) != kSlotEmpty;
			DispatchPendingAction(dispatchInput);
		}
		// The reconciliation, run unconditionally on every visible frame: whatever the
		// hold and the latch currently read is what the veto is, so a swallowed edge
		// corrects itself on the next frame instead of stranding the world gate.
		ReconcileVeto(g_holdEngaged || g_latchOn);
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
			out.eventsInjected = g_eventsInjected.load(std::memory_order_relaxed);
			out.eventsInjectedDropped = g_eventsInjectedDropped.load(std::memory_order_relaxed);
			out.eventsSuppressedByInjection = g_eventsSuppressedByInjection.load(std::memory_order_relaxed);
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

			out.intentsQueued = g_intentsQueued.load(std::memory_order_relaxed);
			out.intentsCompleted = g_intentsCompleted.load(std::memory_order_relaxed);
			out.intentsRefused = g_intentsRefused.load(std::memory_order_relaxed);
			out.lastIntent = ReadString(g_lastIntent);
			out.lastRefusal = ReadString(g_lastRefusal);
			const std::uint32_t slot = g_slot.load(std::memory_order_acquire);
			out.intentInFlight = slot != kSlotEmpty;
			if (g_workerUnavailable.load(std::memory_order_acquire)) {
				out.workerState = "unavailable";
			} else if (!g_workerRunning.load(std::memory_order_acquire)) {
				out.workerState = "not-started";
			} else {
				out.workerState = out.intentInFlight ? "busy" : "idle";
			}
			if (const auto outcome = g_lastOutcome.load(std::memory_order_acquire); outcome) {
				out.resultPresent = true;
				out.resultAccepted = outcome->accepted;
				out.resultPersisted = outcome->persisted;
				out.resultTookEffectNow = outcome->tookEffectNow;
				out.resultNeedsRestart = outcome->needsRestart;
				out.resultKey = outcome->key;
				out.resultValue = outcome->value;
				out.resultReason = outcome->reason;
			}
			out.resultLine = ReadString(g_resultLine);

			out.vetoActive = ResetPolicy::OperatorVetoActive();
			out.vetoOwnedByPanel = g_panelOwnsVeto.load(std::memory_order_acquire);
			out.holdEngaged = g_holdPublished.load(std::memory_order_acquire);
			out.latchOn = g_latchPublished.load(std::memory_order_acquire);
			out.vetoEngagements = g_vetoEngagements.load(std::memory_order_relaxed);
			out.vetoReleases = g_vetoReleases.load(std::memory_order_relaxed);
			out.vetoDeferredToOperator = g_vetoDeferred.load(std::memory_order_relaxed);
			if (const auto tags = g_modeTags.load(std::memory_order_acquire); tags) {
				out.modes = *tags;
			}
		} catch (...) {
			// Every string field above allocates, and a diagnostic report is the last
			// thing that should end a session. A partial block beats a terminate.
		}
		return out;
	}

	std::vector<ControlRect> ControlRects()
	{
		std::vector<ControlRect> out;
		RectRecord copy[Model::kControlCount]{};
		{
			std::scoped_lock lock(g_rectMutex);
			for (std::size_t i = 0; i < Model::kControlCount; ++i) {
				copy[i] = g_rectsPublished[i];
			}
		}
		out.reserve(Model::kControlCount);
		for (std::size_t i = 0; i < Model::kControlCount; ++i) {
			if (!copy[i].present) {
				continue;
			}
			ControlRect rect{};
			rect.name.assign(Model::ControlName(static_cast<Model::ControlId>(i)));
			rect.x = copy[i].minX;
			rect.y = copy[i].minY;
			rect.width = copy[i].maxX - copy[i].minX;
			rect.height = copy[i].maxY - copy[i].minY;
			rect.value = copy[i].value;
			rect.active = copy[i].active;
			rect.present = true;
			out.push_back(std::move(rect));
		}
		return out;
	}
}
