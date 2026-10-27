#include "Upscaler/Panel.h"

#include <atomic>
#include <cmath>
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
#include "Upscaler/Intermediates.h"
#include "Upscaler/OpenVrAdmission.h"
#include "Upscaler/OpenXrPanel.h"
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
		// "ahead" is negative Z. The drop itself is the model's own number, spelled once so
		// the head-relative fall-back puts the panel where the room placement would have.
		constexpr float kVerticalOffsetMeters = static_cast<float>(Model::kPanelDropMeters);

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
		// The close button is the one control the operator has to be able to hit without
		// aiming, so it is the widest and the tallest thing on the panel.
		constexpr float kCloseButtonWidth = 460.0f * kUiScale;
		constexpr float kCloseButtonHeight = 32.0f * kUiScale;
		// The timing pane's own height. Fixed rather than "the rest of the window" since
		// the controls sit above it: a pane that took the remainder would push every
		// control off the texture the moment perfTimers came on. Sized so the whole
		// window - status, controls, result line and this - fits the texture without a
		// scrollbar, because a control scrolled out of view is one a recorded rectangle
		// can no longer be clicked at.
		constexpr float kTimerPaneHeight = 45.0f * kUiScale;

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

		using GetGenericInterface_t = void* (*)(const char*, vr::EVRInitError*);

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
		// panelAnchor, reduced to the one bit the placement asks of it. Read once at each
		// show, never per frame.
		std::atomic_bool g_anchorRoom{ true };
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

		// The placement, published by the render thread at each show and read by Snapshot.
		// Relaxed on the numbers - they are reported and decide nothing - and release on
		// the two facts a reader uses to judge them, so a reader that sees placementValid
		// sees the position that made it true.
		std::atomic_bool g_systemResolved{ false };
		std::atomic_bool g_compositorResolved{ false };
		std::atomic_bool g_placementValid{ false };
		std::atomic_bool g_lastShowFellBack{ false };
		std::atomic<double> g_placedX{ 0.0 };
		std::atomic<double> g_placedY{ 0.0 };
		std::atomic<double> g_placedZ{ 0.0 };
		std::atomic<double> g_placedYaw{ 0.0 };
		std::atomic<double> g_hmdX{ 0.0 };
		std::atomic<double> g_hmdY{ 0.0 };
		std::atomic<double> g_hmdZ{ 0.0 };
		std::atomic<std::uint64_t> g_placements{ 0 };
		std::atomic<std::uint64_t> g_placementFallbacks{ 0 };

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
		std::atomic<std::shared_ptr<const std::string>> g_ratioNote{
			std::make_shared<const std::string>("")
		};
		std::atomic<std::shared_ptr<const std::string>> g_anchorName{
			std::make_shared<const std::string>("room")
		};
		std::atomic<std::shared_ptr<const std::string>> g_transformKind{
			std::make_shared<const std::string>("none")
		};
		std::atomic<std::shared_ptr<const std::string>> g_universeName{
			std::make_shared<const std::string>("unknown")
		};
		std::atomic<std::shared_ptr<const std::string>> g_universeSource{
			std::make_shared<const std::string>("none")
		};
		std::atomic<std::shared_ptr<const std::string>> g_placementFallbackReason{
			std::make_shared<const std::string>("none")
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
		// hand-off is the whole protocol. Exactly one intent may wait in front of the slot
		// (see the queue below), and a post that finds both taken is the refusal the model
		// names.
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

		// --- the one-deep queue in front of the slot ---------------------------------
		//
		// Render-thread-owned outright: filled and drained on the seam and nowhere else,
		// so there is no compare-exchange here and one on the slot itself - the slot is
		// handed between two threads, this is not. The atomic beside the buffers exists
		// only so Snapshot and the drawn result line can see that something is waiting;
		// it is written after the buffers and read before them by nobody, because nobody
		// else reads them at all.
		char g_queuedKey[64]{};
		char g_queuedValue[64]{};
		bool g_queuedValid = false;
		std::atomic_bool g_queueWaiting{ false };
		std::atomic<std::uint64_t> g_intentsQueuedBehind{ 0 };
		std::atomic<std::uint64_t> g_intentsCoalesced{ 0 };

		// Which of "a write completed" and "a write was refused" happened last. Two
		// threads write these - the worker stamps a completed result, the render thread
		// stamps a refusal - so neither can order itself against the other by looking at
		// its own counter alone. One shared ticket source settles it: each side takes a
		// number and publishes it, and the model shows the refusal only while its number
		// is the higher one. fetch_add is acq_rel so the number a side publishes is never
		// visible before the state it describes.
		std::atomic<std::uint64_t> g_eventSerial{ 0 };
		std::atomic<std::uint64_t> g_refusalSerial{ 0 };
		std::atomic<std::uint64_t> g_resultSerial{ 0 };
		// Upscaler::PanelModel::Refusal, as a value the draw can hand straight back to the
		// model. The string beside it (g_lastRefusal) is /state's, and both are written
		// together on the render thread.
		std::atomic<std::uint32_t> g_lastRefusalCode{
			static_cast<std::uint32_t>(Model::Refusal::kNone)
		};

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
			// What was actually sent to the writer. Kept beside the result because the
			// writer returns an empty value on every refusal, and the result line has to
			// name the value the operator asked for - see PanelModel::ResultInput::value.
			std::string requestedValue;
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
					outcome->requestedValue = value;
					outcome->reason = result.reason;
					g_lastOutcome.store(
						std::shared_ptr<const SetOutcome>(std::move(outcome)), std::memory_order_release);
					// Stamped after the outcome is published and before the slot is freed,
					// so a reader that sees this number already sees the result it counts.
					g_resultSerial.store(
						g_eventSerial.fetch_add(1, std::memory_order_acq_rel) + 1, std::memory_order_release);
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
		const OpenXrPanelBridge::Api* g_openXr = nullptr;
		std::uint64_t g_openXrGeneration = 0;
		bool g_openXrPressed = false;
		bool g_openXrTriggerHeld = false;
		std::uint64_t g_openXrInputMs = 0;
		// The layer's two refusal counters as last seen, and the close button the profile
		// at the last show maps; see OpenXrPanelBridge::Status.
		std::uint32_t g_openXrUnboundRefusals = 0;
		std::uint32_t g_openXrIdleReleases = 0;
		const char* g_openXrCloseLabel = "B/Y";
		// The two the room-fixed placement needs, resolved through the same loader and
		// both optional: without them the panel falls back to the head-relative transform
		// for that show and says which one was missing.
		vr::IVRSystem* g_system = nullptr;
		vr::IVRCompositor* g_compositor = nullptr;
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
		// frame would snap the handle back under the operator's hand. The raw pair is the
		// pointer position before the step snap, kept so a driver can converge on one.
		float g_ratioSlider = 0.0f;
		float g_sharpnessSlider = 0.0f;
		float g_ratioSliderRaw = 0.0f;
		float g_sharpnessSliderRaw = 0.0f;
		bool g_ratioSliderActive = false;
		bool g_sharpnessSliderActive = false;
		bool g_holdButtonActive = false;

		// What the draw observed about the two sliders this frame, and the write waiting
		// out its quiet window. Both render-thread-owned: the draw fills the frame, the
		// dispatch advances the latch with the frame's own elapsed time, and the model
		// owns every rule between them.
		Model::SliderFrame g_sliderFrame{};
		Model::SliderLatch g_sliderLatch{};

		// Whether this show has already been placed. Cleared when the operator asks the
		// panel to hide and on teardown, never by the visibility flag: the runtime can
		// deliver an OverlayHidden left over from the previous hide a frame after our own
		// ShowOverlay, and the reconciliation that asks for the overlay back would
		// otherwise place it a second time. Two placements a frame apart are harmless and
		// still wrong - the contract is one placement per show.
		bool g_placedForShow = false;

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
		std::string g_publishedRatioNote;
		std::atomic_bool g_ratioOverrideStanding{ false };

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
			double rawValue = 0.0;
			bool active = false;
			bool present = false;
		};
		RectRecord g_rects[Model::kControlCount]{};  // render-thread-owned scratch
		std::mutex g_rectMutex;
		RectRecord g_rectsPublished[Model::kControlCount]{};  // guarded by g_rectMutex

		void RecordRect(Model::ControlId a_control, double a_value, bool a_active, double a_rawValue) noexcept
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
			record.rawValue = a_rawValue;
			record.active = a_active;
			record.present = true;
		}

		void RecordRect(Model::ControlId a_control, double a_value, bool a_active) noexcept
		{
			RecordRect(a_control, a_value, a_active, a_value);
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
		std::vector<InjectedEvent> g_injectDrain;      // render-thread-owned, swapped in
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
			if (g_openXr) {
				static_cast<void>(g_openXr->show(false, 0, 0, true));
				g_openXr = nullptr;
				g_openXrPressed = false;
				g_openXrTriggerHeld = false;
			}
			if (g_overlay && g_handle != vr::k_ulOverlayHandleInvalid) {
				static_cast<void>(Check(g_overlay->HideOverlay(g_handle), "HideOverlay"sv));
				static_cast<void>(Check(g_overlay->DestroyOverlay(g_handle), "DestroyOverlay"sv));
			}
			g_handle = vr::k_ulOverlayHandleInvalid;
			// The three interface pointers are borrowed from the runtime and are not
			// released here; only the panel's own claim on them ends. A later re-create
			// asks the loader again, which is what makes a teardown and a re-enable
			// symmetric.
			g_placementValid.store(false, std::memory_order_release);
			g_placedForShow = false;
			PublishString(g_transformKind, "none"sv);
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

		[[nodiscard]] void* QueryRuntimeInterface(HMODULE a_module, const char* a_version,
			vr::EVRInitError& a_error, bool a_loud) noexcept
		{
			using Validate_t = bool (*)(const char*);
			const auto validate = reinterpret_cast<Validate_t>(::GetProcAddress(a_module, "VR_IsInterfaceVersionValid"));
			const auto get = reinterpret_cast<GetGenericInterface_t>(::GetProcAddress(a_module, "VR_GetGenericInterface"));
			OpenVrAdmission::Result result{};
			auto* resolved = OpenVrAdmission::Query(validate, get, a_version, &a_error, result);
			if (!resolved && a_loud) {
				logger::warn(FMT_STRING("panel: safely refused {} (admission {}, EVRInitError {}); interface unavailable, upscaler continues; use Fo4VRUpscaler.toml for settings"),
					a_version, static_cast<int>(result), static_cast<int>(a_error));
			}
			return resolved;
		}

		// The overlay interface this module asks the game's loader for. Never VR_Init: the
		// game owns the VR session and a second Init would be a second application.
		[[nodiscard]] bool ResolveRuntime() noexcept
		{
			if (g_openXr || g_overlay) {
				return true;
			}
			if (const auto* api = OpenXrPanel::Resolve()) {
				OpenXrPanelBridge::Status state{};
				if (api->status(&state) && state.ready) {
					g_openXr = api;
					g_openXrGeneration = state.generation;
					g_openXrUnboundRefusals = state.unboundRefusals;
					g_openXrIdleReleases = state.idleReleases;
					g_runtimeResolved.store(true, std::memory_order_release);
					if (state.boundProfiles == 0) {
						logger::warn("panel: OpenXR quad ready, but the game suggested no controller profile the panel can bind; it refuses to open on controllers that report a profile"sv);
					} else {
						try {
							std::string bound;
							for (std::uint32_t i = 0; i < OpenXrPanelBridge::kProfileCount; ++i) {
								if ((state.boundProfiles & (1u << i)) != 0) {
									bound += (bound.empty() ? "" : ", ") + std::string(OpenXrPanelBridge::kProfiles[i].path);
								}
							}
							logger::info(FMT_STRING("panel: OpenXR quad ready; controller input bound on {}"), bound);
						} catch (...) {
						}
					}
					return true;
				}
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

			vr::EVRInitError error = vr::VRInitError_None;
			// The version string comes from the compiled header, so an SDK bump moves the
			// request with it rather than leaving a stale literal here.
			auto* const overlay = static_cast<vr::IVROverlay*>(QueryRuntimeInterface(module, vr::IVROverlay_Version, error, loud));
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

		// The two interfaces the room-fixed placement needs, through the same loader and
		// the same version strings the compiled header declares. Neither is required: the
		// panel runs head-anchored without them, which is what it did before the placement
		// existed. Retried at each show for the same reason ResolveRuntime is - the game's
		// session can be younger than the first frame this module draws - and its log line
		// rides the same gate.
		void ResolvePoseInterfaces() noexcept
		{
			if (g_system && g_compositor) {
				return;
			}
			const auto module = ::GetModuleHandleW(L"openvr_api.dll");
			if (!module) {
				return;
			}

			const bool loud = g_lookupFailureGate.Ready();
			vr::EVRInitError error = vr::VRInitError_None;
			if (!g_system) {
				g_system = static_cast<vr::IVRSystem*>(QueryRuntimeInterface(module, vr::IVRSystem_Version, error, loud));
				if (g_system) {
					g_systemResolved.store(true, std::memory_order_release);
					logger::info(FMT_STRING("panel: {} resolved; the panel places itself in the room"), vr::IVRSystem_Version);
				} else if (loud) {
					logger::warn(FMT_STRING("panel: the runtime did not serve {} (EVRInitError {}); the panel stays head-anchored"),
						vr::IVRSystem_Version, static_cast<int>(error));
				}
			}
			if (!g_compositor) {
				error = vr::VRInitError_None;
				g_compositor = static_cast<vr::IVRCompositor*>(QueryRuntimeInterface(module, vr::IVRCompositor_Version, error, loud));
				if (g_compositor) {
					g_compositorResolved.store(true, std::memory_order_release);
				} else if (loud) {
					logger::warn(FMT_STRING("panel: the runtime did not serve {} (EVRInitError {}); the tracking universe is assumed standing"),
						vr::IVRCompositor_Version, static_cast<int>(error));
				}
			}
		}

		[[nodiscard]] std::string_view UniverseName(vr::ETrackingUniverseOrigin a_origin) noexcept
		{
			switch (a_origin) {
			case vr::TrackingUniverseSeated:
				return "seated"sv;
			case vr::TrackingUniverseStanding:
				return "standing"sv;
			case vr::TrackingUniverseRawAndUncalibrated:
				return "raw"sv;
			default:
				return "unknown"sv;
			}
		}

		// The head-relative transform the panel used before it was placed in the room, and
		// the fall-back whenever the room placement cannot be made. Identity rotation with
		// a translation: the overlay's own +Z is its front face, so an overlay placed at
		// -Z with no rotation faces straight back at the eye it is parented to.
		void PlaceOnHead(std::string_view a_reason) noexcept
		{
			const float distance = g_distanceMeters.load(std::memory_order_acquire);
			vr::HmdMatrix34_t transform{};
			transform.m[0][0] = 1.0f;
			transform.m[1][1] = 1.0f;
			transform.m[2][2] = 1.0f;
			transform.m[1][3] = kVerticalOffsetMeters;
			transform.m[2][3] = -distance;
			static_cast<void>(Check(
				g_overlay->SetOverlayTransformTrackedDeviceRelative(
					g_handle, vr::k_unTrackedDeviceIndex_Hmd, &transform),
				"SetOverlayTransformTrackedDeviceRelative"sv));
			PublishString(g_transformKind, "tracked-device-relative"sv);
			PublishString(g_placementFallbackReason, a_reason);
			g_placementValid.store(false, std::memory_order_release);
		}

		// Places the panel once, where the operator is standing and looking, and leaves it
		// there until the next show. The pose is asked of IVRSystem rather than taken from
		// the compositor's last frame poses: this is a one-shot at show time, so a
		// photon-predicted render pose buys nothing over "where the head is now", and
		// GetDeviceToAbsoluteTrackingPose takes the universe as an argument, so the pose
		// and the SetOverlayTransformAbsolute below are expressed in one universe by
		// construction rather than by two calls agreeing. The universe itself is the
		// compositor's, which is the one the game's own frame is composited in.
		//
		// The array is one element and the count says so - the HMD is index 0
		// (k_unTrackedDeviceIndex_Hmd) and the parameter is VR_ARRAY_COUNT-annotated, so
		// the count is what bounds the writes.
		void PlaceInRoom() noexcept
		{
			ResolvePoseInterfaces();
			const auto universe = g_compositor ? g_compositor->GetTrackingSpace() : vr::TrackingUniverseStanding;
			PublishString(g_universeName, UniverseName(universe));
			PublishString(g_universeSource, g_compositor ? "compositor"sv : "default"sv);
			if (!g_system) {
				g_lastShowFellBack.store(true, std::memory_order_release);
				g_placementFallbacks.fetch_add(1, std::memory_order_relaxed);
				PlaceOnHead("ivrsystem-unavailable"sv);
				return;
			}
			vr::TrackedDevicePose_t poses[vr::k_unTrackedDeviceIndex_Hmd + 1]{};
			g_system->GetDeviceToAbsoluteTrackingPose(universe, 0.0f, poses, 1);
			const auto& pose = poses[vr::k_unTrackedDeviceIndex_Hmd];
			if (!pose.bPoseIsValid) {
				g_lastShowFellBack.store(true, std::memory_order_release);
				g_placementFallbacks.fetch_add(1, std::memory_order_relaxed);
				PlaceOnHead("hmd-pose-invalid"sv);
				return;
			}

			// HmdMatrix34_t is row-major with the translation in column 3 (openvr.h 34-42),
			// so the whole pose crosses into the model as twelve doubles and comes back as
			// twelve. Every rule about where the panel ends up - the flatten onto the floor,
			// the drop, the upright basis and both degenerate fall-backs - is decided in
			// PanelModel::ComposeRoomPlacement, where a turned head, a pitched-and-rolled
			// head and a vertical gaze are contracts the compiler checks. Nothing about the
			// geometry is decided here, because the one arm that can drive this headlessly
			// runs against a synthetic pose at yaw zero, where the right basis and three
			// wrong ones are the same matrix.
			const auto& m = pose.mDeviceToAbsoluteTracking.m;
			Model::Matrix34 head{};
			for (int row = 0; row < 3; ++row) {
				for (int column = 0; column < 4; ++column) {
					head.m[row][column] = static_cast<double>(m[row][column]);
				}
			}
			const auto placed = Model::ComposeRoomPlacement(
				head,
				static_cast<double>(g_distanceMeters.load(std::memory_order_acquire)),
				Model::kPanelDropMeters);

			vr::HmdMatrix34_t transform{};
			for (int row = 0; row < 3; ++row) {
				for (int column = 0; column < 4; ++column) {
					transform.m[row][column] = static_cast<float>(placed.transform.m[row][column]);
				}
			}
			if (!Check(g_overlay->SetOverlayTransformAbsolute(g_handle, universe, &transform),
					"SetOverlayTransformAbsolute"sv)) {
				g_lastShowFellBack.store(true, std::memory_order_release);
				g_placementFallbacks.fetch_add(1, std::memory_order_relaxed);
				PlaceOnHead("set-transform-absolute-refused"sv);
				return;
			}

			g_hmdX.store(head.m[0][3], std::memory_order_relaxed);
			g_hmdY.store(head.m[1][3], std::memory_order_relaxed);
			g_hmdZ.store(head.m[2][3], std::memory_order_relaxed);
			g_placedX.store(placed.transform.m[0][3], std::memory_order_relaxed);
			g_placedY.store(placed.transform.m[1][3], std::memory_order_relaxed);
			g_placedZ.store(placed.transform.m[2][3], std::memory_order_relaxed);
			// Zero along the universe's -Z, increasing toward +X: a heading, not a matrix
			// element, so a reader can compare two shows without decoding a rotation. An
			// angle is the one thing the model does not produce - an atan2 is not a constant
			// expression - and it is a report, so it is derived here from the unit heading
			// the model returned.
			g_placedYaw.store(
				std::atan2(placed.forwardX, -placed.forwardZ) * 180.0 / 3.14159265358979323846,
				std::memory_order_relaxed);
			PublishString(g_transformKind, "absolute"sv);
			PublishString(g_placementFallbackReason, "none"sv);
			g_lastShowFellBack.store(false, std::memory_order_release);
			// Released last: a reader that sees this true sees the position that made it
			// so, and every fall-back above cleared it before touching the transform.
			g_placementValid.store(true, std::memory_order_release);
			g_placements.fetch_add(1, std::memory_order_relaxed);
		}

		// One placement, at each show. Which kind is the operator's setting, read here and
		// not per frame, so a live panelAnchor change lands at the next show.
		void PlaceOverlay() noexcept
		{
			if (g_openXr) {
				PublishString(g_transformKind, g_anchorRoom.load() ? "openxr-local"sv : "openxr-view"sv);
				PublishString(g_placementFallbackReason, "none"sv);
				return;
			}
			if (!g_overlay || g_handle == vr::k_ulOverlayHandleInvalid) {
				return;
			}
			if (!g_anchorRoom.load(std::memory_order_acquire)) {
				g_lastShowFellBack.store(false, std::memory_order_release);
				PlaceOnHead("head-anchor-configured"sv);
				return;
			}
			PlaceInRoom();
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
			if (g_openXr) {
				return true;
			}
			vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
			if (!Check(g_overlay->CreateOverlay(kOverlayKey, kOverlayName, &handle), "CreateOverlay"sv)) {
				return false;
			}
			g_handle = handle;

			// An overlay with no transform at all is undefined; the real placement runs
			// at the show, which is the only moment the operator's own pose means
			// anything.
			PlaceOnHead("not-placed-yet"sv);
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
				if (g_overlay) {
					static_cast<void>(Check(g_overlay->DestroyOverlay(g_handle), "DestroyOverlay"sv));
				}
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

		// What is left of the texture to the right of kValueColumn is finite, and ImGui
		// clips to the window: the overlay is a fixed 1024 px texture with NoResize and no
		// horizontal scrollbar, so a value wider than the column is not drawn short - it is
		// drawn cut, mid-glyph, with nothing on screen to say that anything was dropped.
		// The width is asked of the font rather than assumed from a pitch, so this stays
		// right if kUiScale, kValueColumn or the font ever changes; nothing is allocated
		// unless a value actually has to be cut, and the cut is marked. ASCII "..." because
		// the default font is ProggyClean, which has no ellipsis glyph.
		void DrawValue(std::string_view a_value)
		{
			if (a_value.empty()) {
				return;
			}
			const char* const begin = a_value.data();
			const char* const end = begin + a_value.size();
			const float available = ImGui::GetContentRegionAvail().x;
			if (available <= 0.0f || ImGui::CalcTextSize(begin, end).x <= available) {
				ImGui::TextUnformatted(begin, end);
				return;
			}
			static constexpr std::string_view kCut = "...";
			const float cutWidth = ImGui::CalcTextSize(kCut.data(), kCut.data() + kCut.size()).x;
			std::size_t keep = a_value.size();
			while (keep > 0 && ImGui::CalcTextSize(begin, begin + keep).x + cutWidth > available) {
				--keep;
			}
			if (keep > 0) {
				ImGui::TextUnformatted(begin, begin + keep);
				ImGui::SameLine(0.0f, 0.0f);
			}
			ImGui::TextUnformatted(kCut.data(), kCut.data() + kCut.size());
		}

		void Row(const char* a_label, const std::string& a_value)
		{
			ImGui::TextUnformatted(a_label);
			ImGui::SameLine(kValueColumn);
			DrawValue(a_value);
		}

		void RowRaw(const char* a_label, const char* a_value)
		{
			ImGui::TextUnformatted(a_label);
			ImGui::SameLine(kValueColumn);
			DrawValue(a_value ? std::string_view{ a_value } : std::string_view{});
		}

		// The bound panel key as the header spells it: F1..F24 directly, anything else by the
		// keyboard layout's own name. Empty when unbound.
		[[nodiscard]] std::string PanelKeyLabel(std::int64_t a_vk)
		{
			if (a_vk >= VK_F1 && a_vk <= VK_F24) {
				return "F" + std::to_string(a_vk - VK_F1 + 1);
			}
			if (a_vk < 1 || a_vk > 254) {
				return {};
			}
			const auto scan = ::MapVirtualKeyW(static_cast<UINT>(a_vk), MAPVK_VK_TO_VSC);
			char name[64]{};
			if (scan != 0 && ::GetKeyNameTextA(static_cast<LONG>(scan << 16), name, static_cast<int>(sizeof(name))) > 0) {
				return name;
			}
			char fallback[16]{};
			std::snprintf(fallback, sizeof(fallback), "key %02X", static_cast<unsigned>(a_vk));
			return fallback;
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
			// The only place the ways out are written down. While the panel is visible it
			// owns the controllers, so the game sees nothing; the panel key needs the game
			// window focused, which in a headset it usually is not. The OpenXR layer also
			// closes on the controller's B/Y, B or menu button, whichever its profile maps.
			ImGui::SameLine(kValueColumn);
			const auto key = PanelKeyLabel(g_keyPanelVk);
			std::string exits = g_openXr ? std::string(g_openXrCloseLabel) + " or Close" : "Close";
			if (!key.empty()) {
				exits = key + (g_openXr ? ", " : " or ") + exits;
			}
			ImGui::TextUnformatted(exits.c_str());
			ImGui::Separator();
			Row("mode", a_modeSwitch.liveMode + "  (" + a_modeSwitch.modeSource + ")");
			Row("backend", std::string(HDRRoundTrip::StereoBackendName(
							   static_cast<HDRRoundTrip::StereoBackend>(isolation.stereoBackend))));
			// The bucket rides the preset row rather than taking one of its own: a row
			// added here pushes every control below it down, and a control off the bottom
			// of the texture is one the laser can no longer reach. Only the bucket the
			// feature was created at is drawn: the requested bucket is the mode's own and
			// the mode row directly above already names it, so the pair spends the column
			// on a value that is already on screen. VERIFIED 2026-09-05 on 310.7.0.0 - a
			// quality session at renderScaleOverride 0.40 with the compiled default preset
			// reads "auto -> 2147483660  [ultraperformance]", 38 glyphs against a column
			// that holds 34, and 49 glyphs with the pair. DrawValue is what keeps either of
			// those honest on screen; dropping the requested half is what keeps the
			// ordinary row from needing it. /state keeps both buckets.
			Row("preset", Evaluate::RequestedPreset() + " -> " + Evaluate::ReportedPreset() +
							  "  [" + Evaluate::ResolvedBucket() + "]");
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

			// The handle follows, in order: the operator's hand while they are dragging;
			// the value waiting out its quiet window, so a release does not visibly snap
			// back to the old ratio for the fifth of a second before the write lands; and
			// otherwise the live value, un-snapped, because 0.6667 is a real ratio and
			// drawing it as 0.67 would report the mode's own default as something it is
			// not.
			const bool ratioWaiting = g_sliderLatch.pending &&
			                          g_sliderLatch.control == Model::ControlId::kRatioSlider;
			if (!g_ratioSliderActive) {
				g_ratioSlider = static_cast<float>(ratioWaiting ? g_sliderLatch.value : a_out.ratio.value);
			}
			ImGui::TextUnformatted("Ratio");
			ImGui::SameLine(kValueColumn);
			ImGui::SetNextItemWidth(kSliderWidth);
			// NoRoundToFormat because the snap below is the only rounding there is: ImGui
			// would otherwise round the value to the display format's own precision, and
			// two rounding rules on one value is how a slider reports a step it did not
			// take.
			ImGui::SliderFloat("##ratio", &g_ratioSlider,
				static_cast<float>(a_out.ratio.minimum), static_cast<float>(a_out.ratio.maximum), "%.2f",
				ImGuiSliderFlags_NoRoundToFormat);
			g_ratioSliderActive = ImGui::IsItemActive();
			g_ratioSliderRaw = g_ratioSlider;
			if (g_ratioSliderActive) {
				// Snapped only while the pointer owns it: an idle slider shows the live
				// value exactly, and a dragged one steps.
				g_ratioSlider = static_cast<float>(Model::SnapRatio(g_ratioSlider));
			}
			RecordRect(Model::ControlId::kRatioSlider, static_cast<double>(g_ratioSlider), g_ratioSliderActive,
				static_cast<double>(g_ratioSliderRaw));
			if (g_ratioSliderActive) {
				g_sliderFrame.activeControl = Model::ControlId::kRatioSlider;
			}
			// Written once a release has stood for the model's own quiet window, never per
			// tick: every tick of a drag would be a settings write, a file replace and a
			// whole Configuration::Apply, and a laser trigger reads as several releases
			// across one drag.
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				g_sliderFrame.releasedControl = Model::ControlId::kRatioSlider;
				g_sliderFrame.releasedValue = static_cast<double>(g_ratioSliderRaw);
			}
			ImGui::TextUnformatted("render");
			ImGui::SameLine(kValueColumn);
			DrawValue(a_out.ratio.sizeText.View());
			if (a_out.ratio.overrideStanding) {
				ImGui::SameLine();
				ImGui::TextUnformatted(a_out.ratio.note.CStr());
			}
			// Always drawn, standing override or not: a control that comes and goes is one
			// a recorded rectangle cannot be clicked at, and writing the unset value when
			// nothing is set is a no-op the writer answers for itself.
			if (ImGui::Button("use mode ratio", ImVec2(kPresetButtonWidth * 2.0f, 0.0f))) {
				g_pendingAction = Model::Action{
					Model::ActionKind::kButton, Model::ControlId::kUseModeRatio, 0.0, false
				};
			}
			RecordRect(Model::ControlId::kUseModeRatio, 0.0, ImGui::IsItemActive());
			if (a_out.ratio.fullWouldBeRefused) {
				ImGui::SameLine();
				ImGui::TextUnformatted("1.00 beside a reduced mode is a mode change");
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

			const bool sharpnessWaiting = g_sliderLatch.pending &&
			                              g_sliderLatch.control == Model::ControlId::kSharpnessSlider;
			if (!g_sharpnessSliderActive) {
				g_sharpnessSlider =
					static_cast<float>(sharpnessWaiting ? g_sliderLatch.value : a_out.sharpness.value);
			}
			ImGui::TextUnformatted("Sharpness");
			ImGui::SameLine(kValueColumn);
			ImGui::SetNextItemWidth(kSliderWidth);
			ImGui::SliderFloat("##sharpness", &g_sharpnessSlider,
				static_cast<float>(a_out.sharpness.minimum), static_cast<float>(a_out.sharpness.maximum), "%.2f",
				ImGuiSliderFlags_NoRoundToFormat);
			g_sharpnessSliderActive = ImGui::IsItemActive();
			g_sharpnessSliderRaw = g_sharpnessSlider;
			if (g_sharpnessSliderActive) {
				g_sharpnessSlider = static_cast<float>(Model::SnapSharpness(g_sharpnessSlider));
			}
			RecordRect(Model::ControlId::kSharpnessSlider, static_cast<double>(g_sharpnessSlider),
				g_sharpnessSliderActive, static_cast<double>(g_sharpnessSliderRaw));
			if (g_sharpnessSliderActive) {
				g_sliderFrame.activeControl = Model::ControlId::kSharpnessSlider;
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				g_sliderFrame.releasedControl = Model::ControlId::kSharpnessSlider;
				g_sliderFrame.releasedValue = static_cast<double>(g_sharpnessSliderRaw);
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

		// Last row and deliberately the largest target on the panel. It is the only way
		// out that works in a headset: SteamVR's laser owns the controllers while this is
		// up, so the pause menu and the Pip-Boy are unreachable, and the panel key is only
		// read while the game owns the desktop's foreground window - which, with a headset
		// on, it usually does not.
		void DrawCloseRow()
		{
			ImGui::Separator();
			if (ImGui::Button("Close", ImVec2(kCloseButtonWidth, kCloseButtonHeight))) {
				g_pendingAction = Model::Action{
					Model::ActionKind::kButton, Model::ControlId::kClose, 0.0, false
				};
			}
			RecordRect(Model::ControlId::kClose, 0.0, ImGui::IsItemActive());
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

			if (g_publishedRatioNote != a_out.ratio.note.View()) {
				try {
					g_publishedRatioNote.assign(a_out.ratio.note.View());
					PublishString(g_ratioNote, g_publishedRatioNote);
				} catch (...) {
				}
			}
			g_ratioOverrideStanding.store(a_out.ratio.overrideStanding, std::memory_order_relaxed);
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
			const auto intermediates = Intermediates::Snapshot();
			const auto preset = ReadString(g_livePreset);
			// The slot first, the outcome second, and never the other way round. The
			// worker publishes the outcome and only then frees the slot, so a reader that
			// takes the slot first and finds it empty is guaranteed to see the outcome
			// that emptied it. Taking the outcome first can read the previous one and then
			// find the slot already empty, which draws the previous intent's result line as
			// final for a frame instead of "pending" - the one frame in which an operator
			// reads the answer to the click before last as the answer to this one.
			const bool intentInFlight = g_slot.load(std::memory_order_acquire) != kSlotEmpty;
			const auto outcome = g_lastOutcome.load(std::memory_order_acquire);

			Model::Input input{};
			input.modeSwitch.liveMode = modeSwitch.liveMode;
			input.modeSwitch.modeSource = modeSwitch.modeSource;
			input.modeSwitch.sessionArmedMode = modeSwitch.sessionArmedMode;
			input.modeSwitch.lastNonOffMode = modeSwitch.lastNonOffMode;
			input.modeSwitch.bracketArmed = modeSwitch.bracketArmed;
			input.ratio.requestedScale = dynamic.requestedScale;
			// Intermediates publishes the exact Backend::Config dimensions retained by
			// the prepared views. Explicit ratios deliberately leave dynamic.snapped
			// false, and its snapped* fields may describe either kind of snap, so they
			// are not a valid display source in that case. If no views are prepared,
			// the vendor snap is still a complete prediction only when its own validity
			// bit stands; otherwise leave the dimensions zero rather than show stale data.
			if (intermediates.ready) {
				input.ratio.renderWidth = intermediates.renderWidth;
				input.ratio.renderHeight = intermediates.renderHeight;
				input.ratio.outputWidth = intermediates.outputWidth;
				input.ratio.outputHeight = intermediates.outputHeight;
			} else if (dynamic.snapped) {
				input.ratio.renderWidth = dynamic.snappedRenderWidth;
				input.ratio.renderHeight = dynamic.snappedRenderHeight;
				input.ratio.outputWidth = dynamic.snappedOutputWidth;
				input.ratio.outputHeight = dynamic.snappedOutputHeight;
			}
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
				input.lastResult.requestedValue = outcome->requestedValue;
				input.lastResult.reason = outcome->reason;
			}
			input.veto.active = ResetPolicy::OperatorVetoActive();
			input.veto.ownedByPanel = g_panelOwnsVeto.load(std::memory_order_acquire);
			input.keys.toggleVk = g_keyToggleVk;
			input.keys.compareVk = g_keyCompareVk;
			input.keys.panelVk = g_keyPanelVk;
			input.intentInFlight = intentInFlight;
			// Render-thread-owned, so this reads the buffers directly rather than the
			// atomic beside them: the queue is filled and drained on this thread and the
			// atomic exists for Snapshot.
			input.intentQueued = g_queuedValid;
			input.queuedKey = g_queuedValid ? std::string_view{ g_queuedKey } : std::string_view{};
			// A worker that never started is a standing condition, not an event: the panel
			// draws it on every frame rather than waiting for a click to fail silently.
			input.workerAvailable = g_workerRunning.load(std::memory_order_acquire) &&
			                        !g_workerUnavailable.load(std::memory_order_acquire);
			input.lastRefusal = static_cast<Model::Refusal>(g_lastRefusalCode.load(std::memory_order_acquire));
			// Both tickets come from one counter, so the comparison is a total order over
			// the two events rather than a guess from two independent clocks.
			input.refusalIsNewer = g_refusalSerial.load(std::memory_order_acquire) >
			                       g_resultSerial.load(std::memory_order_acquire);
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
			DrawCloseRow();
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

		// The render thread's half of the refusal record: the counter, the code the model
		// reads, the string /state reports, and the ticket that says whether this refusal
		// or the last completed result happened later. kNone is the clear, and it takes no
		// ticket and counts nothing - an accepted post is not a refusal, it only ends the
		// last one.
		void PublishRefusal(Model::Refusal a_refusal) noexcept
		{
			if (a_refusal != Model::Refusal::kNone) {
				g_intentsRefused.fetch_add(1, std::memory_order_relaxed);
			}
			g_lastRefusalCode.store(static_cast<std::uint32_t>(a_refusal), std::memory_order_release);
			PublishString(g_lastRefusal, Model::RefusalName(a_refusal));
			if (a_refusal != Model::Refusal::kNone) {
				// Stamped after the code above, so a reader that sees this number already
				// sees the refusal it counts - the mirror of the worker's own stamp.
				g_refusalSerial.store(
					g_eventSerial.fetch_add(1, std::memory_order_acq_rel) + 1, std::memory_order_release);
			}
		}

		// The queue in front of the worker's slot, both halves render-thread-only.
		void HoldQueuedIntent(std::string_view a_key, std::string_view a_value) noexcept
		{
			CopyInto(g_queuedKey, a_key);
			CopyInto(g_queuedValue, a_value);
			g_queuedValid = true;
			g_queueWaiting.store(true, std::memory_order_release);
		}

		// Posted the first frame the slot frees. Called before this frame's own action is
		// decided, so a click landing on the same frame the queue drains sees the slot
		// occupied and queues behind it rather than overtaking it.
		void DrainQueuedIntent() noexcept
		{
			if (!g_queuedValid) {
				return;
			}
			if (!PostSetKey(std::string_view{ g_queuedKey }, std::string_view{ g_queuedValue })) {
				return;
			}
			g_queuedValid = false;
			g_queueWaiting.store(false, std::memory_order_release);
		}

		// One input, one intent, dispatched after the ImGui frame is closed and outside
		// the isolation scope. A settings intent goes to the worker or waits one deep in
		// front of it; the A/B pair and the close button go straight from here, because
		// none of the three may wait on a settings write.
		void DispatchAction(const Model::Input& a_input, const Model::Action& a_action) noexcept
		{
			if (a_action.kind == Model::ActionKind::kNone) {
				return;
			}
			const auto intent = Model::Decide(a_input, a_action);
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
			case Model::IntentKind::kHidePanel:
				// The show intent, cleared here rather than through the module's own
				// RequestHide so a reader of /state sees the click as an intent like any
				// other. The seam hides the overlay on its next frame.
				g_showIntent.store(0, std::memory_order_release);
				PublishString(g_lastIntent, "hide-panel"sv);
				PublishRefusal(Model::Refusal::kNone);
				break;
			case Model::IntentKind::kSetKey:
				{
					char label[128];
					std::snprintf(label, sizeof(label), "set-key %s = %s (%.*s)", intent.key.CStr(), intent.value.CStr(),
						static_cast<int>(Model::DispositionName(intent.disposition).size()),
						Model::DispositionName(intent.disposition).data());
					if (intent.disposition != Model::Disposition::kImmediate) {
						// Held rather than posted: the worker still owns the slot. Coalescing
						// overwrites the value already waiting on this key, which is why both
						// dispositions take the same call.
						HoldQueuedIntent(intent.key.View(), intent.value.View());
						if (intent.disposition == Model::Disposition::kCoalesced) {
							g_intentsCoalesced.fetch_add(1, std::memory_order_relaxed);
						} else {
							g_intentsQueuedBehind.fetch_add(1, std::memory_order_relaxed);
						}
						PublishString(g_lastIntent, label);
						PublishRefusal(Model::Refusal::kNone);
						break;
					}
					if (PostSetKey(intent.key.View(), intent.value.View())) {
						PublishString(g_lastIntent, label);
						PublishRefusal(Model::Refusal::kNone);
					} else {
						// The worker refused the post. Either it is unavailable for the
						// session or it still owns the previous intent; the model already
						// names the second case, and this names the first.
						const bool unavailable = !g_workerRunning.load(std::memory_order_acquire);
						PublishString(g_lastIntent, label);
						PublishRefusal(unavailable ? Model::Refusal::kWorkerUnavailable :
													 Model::Refusal::kIntentInFlight);
						if (unavailable && g_workerUnavailableGate.Ready()) {
							logger::warn("panel: no settings worker is running, so panel writes are unavailable for this session"sv);
						}
					}
					break;
				}
			case Model::IntentKind::kNone:
			default:
				PublishRefusal(intent.refusal);
				break;
			}
		}

		// The input the dispatch decides against. Read once per dispatch and passed by
		// value, so two dispatches on one frame each see the state their own predecessor
		// left.
		[[nodiscard]] Model::Input ReadDispatchInput() noexcept
		{
			Model::Input input{};
			input.intentInFlight = g_slot.load(std::memory_order_acquire) != kSlotEmpty;
			input.intentQueued = g_queuedValid;
			input.queuedKey = g_queuedValid ? std::string_view{ g_queuedKey } : std::string_view{};
			return input;
		}

		// A hide, a teardown or a lost overlay ends a drag: the panel going away is as
		// final a release as the button coming up, and losing the value the operator just
		// set because they closed the panel a fifth of a second too soon would be worse
		// than the trigger chatter the window exists to absorb.
		void FlushSliderLatch() noexcept
		{
			if (!g_sliderLatch.pending) {
				return;
			}
			const auto fire = Model::Action{ Model::ActionKind::kSliderRelease, g_sliderLatch.control,
				g_sliderLatch.value, false };
			g_sliderLatch = Model::SliderLatch{};
			g_sliderFrame = Model::SliderFrame{};
			DispatchAction(ReadDispatchInput(), fire);
		}

		// The OpenXR layer refuses a show on controllers whose profile carries none of the
		// panel's bindings, and closes a panel whose controls all went inactive. Either way
		// the intent is cleared, so the panel key opens it again instead of the next frame
		// retrying, and the HUD says why no panel is there. True when it let go.
		[[nodiscard]] bool OpenXrLetGo(const OpenXrPanelBridge::Status& a_state) noexcept
		{
			if (a_state.unboundRefusals != g_openXrUnboundRefusals) {
				g_openXrUnboundRefusals = a_state.unboundRefusals;
				const std::string_view profile(a_state.unboundProfile, ::strnlen(a_state.unboundProfile, sizeof(a_state.unboundProfile)));
				logger::warn(FMT_STRING("panel: the controllers report {}, which carries none of the panel's bindings; the panel stays closed so the game keeps its controller input"),
					profile.empty() ? "an unnamed profile"sv : profile);
				HudMessages::Enqueue("Fo4VR Upscaler: the panel cannot read these controllers - use Fo4VRUpscaler.toml (see the log)"sv);
				RequestHide();
				return true;
			}
			if (a_state.idleReleases != g_openXrIdleReleases) {
				g_openXrIdleReleases = a_state.idleReleases;
				logger::info(FMT_STRING("panel: no panel control was active for {} seconds, so the OpenXR layer closed the panel and returned the controllers to the game"),
					OpenXrPanelBridge::kIdleReleaseNanoseconds / 1'000'000'000);
				HudMessages::Enqueue("Fo4VR Upscaler: panel closed - no controller was reaching it"sv);
				RequestHide();
				return true;
			}
			return false;
		}

		void SubmitTexture() noexcept
		{
			if (g_openXr) {
				if (g_openXr->submit(g_texture)) {
					const auto submits = g_textureSubmits.fetch_add(1, std::memory_order_relaxed) + 1;
					if (submits == 1) {
						logger::info("panel: first OpenXR quad texture submitted"sv);
					}
				} else if (g_submitFailureGate.Ready()) {
					OpenXrPanelBridge::Status state{};
					static_cast<void>(g_openXr->status(&state));
					logger::warn(FMT_STRING("panel: OpenXR texture deferred; result {}, ready {}, quad frames {}"),
						state.lastResult, state.ready, state.quadFrames);
				}
				return;
			}
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
			if (g_openXr) {
				OpenXrPanelBridge::Input input{};
				const bool received = g_openXr->input(&input);
				auto& io = ImGui::GetIO();
				io.MouseDrawCursor = received && input.valid;
				if (received && input.valid) {
					io.AddMousePosEvent(input.x * kTextureWidth, input.y * kTextureHeight);
				} else {
					io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
				}
				const bool held = received && input.valid && input.pressed;
				const bool pressed = held && (g_openXrPressed || (input.hover && !g_openXrTriggerHeld));
				if (received) {
					g_openXrTriggerHeld = held;
				}
				if (pressed != g_openXrPressed) {
					io.AddMouseButtonEvent(0, pressed);
					g_openXrPressed = pressed;
				}
				const auto now = ::GetTickCount64();
				const float delta = g_openXrInputMs ? (std::min)(0.1f, static_cast<float>(now - g_openXrInputMs) / 1000.0f) : 0;
				g_openXrInputMs = now;
				if (received && input.hover && input.scroll != 0) {
					io.AddMouseWheelEvent(0, input.scroll * delta * 5);
				}
				if (received && input.close) {
					RequestHide();
				}
				return;
			}
			ImGui::GetIO().MouseDrawCursor = false;
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
		const std::string& a_anchor,
		double a_renderScaleOverride,
		const std::string& a_preset,
		double a_sharpness) noexcept
	{
		g_enabled.store(a_enabled, std::memory_order_release);
		g_widthMeters.store(static_cast<float>(a_widthMeters), std::memory_order_release);
		g_distanceMeters.store(static_cast<float>(a_distanceMeters), std::memory_order_release);
		// Settings::Validate has already reduced this to one of the two words, so
		// anything that is not "head" is the room anchor rather than an error to report a
		// second time.
		g_anchorRoom.store(a_anchor != "head", std::memory_order_release);
		PublishString(g_anchorName, a_anchor);
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
		// Two render-thread-owned bool reads, ahead of everything including the enabled
		// gate: a write the operator asked for is theirs whatever the panel does next, so
		// neither a hide, a close nor panelEnabled going false may strand one. Both return
		// on the first branch in every frame that has nothing waiting, which is every
		// frame of an ordinary session.
		DrainQueuedIntent();
		const bool enabled = g_enabled.load(std::memory_order_acquire);
		if (!enabled) {
			FlushSliderLatch();
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
			FlushSliderLatch();
			// The next show places again, wherever the operator is by then.
			g_placedForShow = false;
			if (g_overlayCreated.load(std::memory_order_acquire) && g_visible.load(std::memory_order_acquire)) {
				if (g_openXr) {
					if (!g_openXr->show(false, 0, 0, true)) {
						return;
					}
					g_openXrPressed = false;
					g_openXrTriggerHeld = false;
					ImGui::SetCurrentContext(g_imgui);
					ImGui::GetIO().AddMouseButtonEvent(0, false);
				} else {
					static_cast<void>(Check(g_overlay->HideOverlay(g_handle), "HideOverlay"sv));
				}
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
		if (g_openXr) {
			OpenXrPanelBridge::Status state{};
			if (g_openXr->status(&state)) {
				if (!state.ready || state.generation != g_openXrGeneration) {
					Teardown();
				} else if (OpenXrLetGo(state)) {
					return;
				}
			}
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
			// Placed before it is shown, so the first composited frame is already where
			// the operator is looking rather than swinging into place afterwards, and once
			// per show rather than once per reconciliation.
			if (!g_placedForShow) {
				PlaceOverlay();
				g_placedForShow = true;
			}
			const bool shown = g_openXr ? g_openXr->show(true, g_widthMeters.load(), g_distanceMeters.load(), g_anchorRoom.load()) :
			                              Check(g_overlay->ShowOverlay(g_handle), "ShowOverlay"sv);
			if (g_openXr) {
				OpenXrPanelBridge::Status state{};
				if (g_openXr->status(&state)) {
					if (!shown) {
						static_cast<void>(OpenXrLetGo(state));
					} else {
						const auto row = state.activeProfile;
						g_openXrCloseLabel = row >= 0 && static_cast<std::uint32_t>(row) < OpenXrPanelBridge::kProfileCount ?
						                         OpenXrPanelBridge::kProfiles[row].closeLabel :
						                         "B/Y";
					}
				}
			}
			if (!shown) {
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
		// Two clocks out of one tick read, and they are not the same number. ImGui wants a
		// plausible frame time and asserts on a non-positive one, so its delta is clamped.
		// The slider's quiet window wants elapsed wall time, and clamping that would make
		// it count frames instead: under the null driver the panel draws several hundred
		// frames a second while GetTickCount64 moves in 16 ms steps, so a clamped 1/90 s
		// per frame runs a 200 ms window out in about 30 ms of real time. The raw tick
		// difference is zero on most frames here and sums correctly over the window, which
		// is the only property that matters to it.
		const std::uint64_t elapsedMs = g_lastFrameTicks == 0 ? 0 : now - g_lastFrameTicks;
		const float delta = g_lastFrameTicks == 0 ?
		                        1.0f / 90.0f :
		                        static_cast<float>(elapsedMs) / 1000.0f;
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
			// The drag's own quiet window, advanced with this frame's elapsed time - the
			// model reads no clock, so the seam hands it one. It can produce one slider
			// release action, and the draw can have produced one button action; both are
			// dispatched, and the queue is what keeps the second from being refused.
			// Decide's inputs are read once per dispatch rather than carried out of the
			// draw deliberately: the worker can finish between the frame that drew the
			// button and this dispatch, and refusing a click against a slot that is
			// already free would lose it.
			const auto step =
				Model::AdvanceSlider(g_sliderLatch, g_sliderFrame, static_cast<double>(elapsedMs));
			g_sliderLatch = step.latch;
			g_sliderFrame = Model::SliderFrame{};
			const auto action = g_pendingAction;
			g_pendingAction = Model::Action{};
			if (step.fire.kind != Model::ActionKind::kNone) {
				DispatchAction(ReadDispatchInput(), step.fire);
			}
			DispatchAction(ReadDispatchInput(), action);
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
			out.anchor = ReadString(g_anchorName);
			out.transformKind = ReadString(g_transformKind);
			out.universe = ReadString(g_universeName);
			out.universeSource = ReadString(g_universeSource);
			out.systemResolved = g_systemResolved.load(std::memory_order_acquire);
			out.compositorResolved = g_compositorResolved.load(std::memory_order_acquire);
			out.placementValid = g_placementValid.load(std::memory_order_acquire);
			out.lastShowFellBack = g_lastShowFellBack.load(std::memory_order_acquire);
			out.placementFallbackReason = ReadString(g_placementFallbackReason);
			out.placedX = g_placedX.load(std::memory_order_relaxed);
			out.placedY = g_placedY.load(std::memory_order_relaxed);
			out.placedZ = g_placedZ.load(std::memory_order_relaxed);
			out.placedYawDegrees = g_placedYaw.load(std::memory_order_relaxed);
			out.hmdX = g_hmdX.load(std::memory_order_relaxed);
			out.hmdY = g_hmdY.load(std::memory_order_relaxed);
			out.hmdZ = g_hmdZ.load(std::memory_order_relaxed);
			out.placements = g_placements.load(std::memory_order_relaxed);
			out.placementFallbacks = g_placementFallbacks.load(std::memory_order_relaxed);
			out.creations = g_creations.load(std::memory_order_relaxed);
			out.teardowns = g_teardowns.load(std::memory_order_relaxed);
			out.creationFailures = g_creationFailures.load(std::memory_order_relaxed);
			out.telemetryDrawn = g_telemetryDrawn.load(std::memory_order_relaxed);

			out.intentsQueued = g_intentsQueued.load(std::memory_order_relaxed);
			out.intentsCompleted = g_intentsCompleted.load(std::memory_order_relaxed);
			out.intentsRefused = g_intentsRefused.load(std::memory_order_relaxed);
			out.intentsQueuedBehind = g_intentsQueuedBehind.load(std::memory_order_relaxed);
			out.intentsCoalesced = g_intentsCoalesced.load(std::memory_order_relaxed);
			out.lastIntent = ReadString(g_lastIntent);
			out.lastRefusal = ReadString(g_lastRefusal);
			const std::uint32_t slot = g_slot.load(std::memory_order_acquire);
			out.intentInFlight = slot != kSlotEmpty;
			out.intentWaiting = g_queueWaiting.load(std::memory_order_acquire);
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
				out.resultRequestedValue = outcome->requestedValue;
				out.resultReason = outcome->reason;
			}
			out.resultLine = ReadString(g_resultLine);
			out.ratioOverrideStanding = g_ratioOverrideStanding.load(std::memory_order_relaxed);
			out.ratioNote = ReadString(g_ratioNote);

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
			rect.rawValue = copy[i].rawValue;
			rect.active = copy[i].active;
			rect.present = true;
			out.push_back(std::move(rect));
		}
		return out;
	}
}
