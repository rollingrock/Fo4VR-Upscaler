#include "Settings/Writer.h"

#include "Settings/Configuration.h"
#include "Settings/Settings.h"
#include "Settings/UserPaths.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

// The layered override file this plugin writes.
//
// Nothing under Settings/ wrote a file before this: the mod only ever read its
// configuration. Three things below are load-bearing and each earns its own
// section - validate-before-write, atomic replace, and staying off the render
// thread.
//
// VALIDATE BEFORE WRITE. Settings::Validate() (Settings.h) is the whole-configuration
// consistency pass the loader runs on every read; it mutates whatever globals it is
// given (mode aliasing, the diagnostic mutual-exclusion cascade, range clamps).
// TrialSet below applies exactly one prospective change to the live global
// Setting<T> instances, takes a full snapshot (the "mid" snapshot - the prospective
// merged configuration), calls the real Validate(), takes another snapshot ("after"),
// captures the field's own validated value while the live global still holds it
// (FieldDescriptor::captureLive, Settings.h), and only then restores the live
// globals to what they were before any of this ran - under
// Settings::configurationLock then Settings::loadLock, the same fixed order
// Configuration::Apply() takes, so this can never observe (or leave behind) a
// half-applied settings episode. If Validate() left every field exactly where the
// trial put it, the value is legitimate; if it moved anything - even the very key
// being set, e.g. a clamp - Settings::FirstDifferingKey names it and the write is
// refused. A configuration the next launch would silently rewrite is not one worth
// saving.
//
// The override file below is built from that captured value, never by reading the
// live globals again: TrialSetImpl's restore above runs unconditionally, for a live-
// policy key exactly as much as a restart-policy one (persisting a restart-policy key
// must not make it live), so by the time Set() reaches the file logic the live
// globals hold the pre-request value again. FieldDescriptor::insertValueInto and
// valueEqualsBase (the latter deciding whether the captured value equals the shipped
// default, and so should be erased from the override rather than written) both take
// the captured value as a parameter for exactly this reason - reading
// *Settings::<key> at that point would silently drop or corrupt every write.
//
// ATOMIC REPLACE. The override is rewritten as a whole file every time (its own
// existing content is re-parsed, the one key changes, the rest is carried through
// unmodified) and written to a temporary file beside the target, flushed and closed,
// then replaced onto the target via std::filesystem::rename - never a partial
// in-place edit. A crash between those two steps leaves either the old file or the
// new one, never a truncated hybrid.
//
// NOT FROM THE RENDER THREAD. DevBench's /config/set handler runs on the dev-tooling
// host's own thread, and Configuration::Apply() is already called directly from
// there today (see HandleConfigReload) - settings mutation itself is not new to that
// thread. Only the disk write is different: it is queued through
// F4SE::GetTaskInterface()->AddTask, the same game-thread hop HudMessages.cpp uses for
// its own not-render-thread work, and Set() blocks (bounded, polling) until that queued
// write completes, so the caller can report persisted:true/false truthfully rather than
// optimistically. Nothing else is hopped: the live re-apply below runs
// Configuration::Apply() on the calling thread, which is why the thread this is called
// from is the thread that re-applies.
namespace Settings::Writer
{
	namespace
	{
		// ---- trial validation ----

		struct Trial
		{
			const FieldDescriptor* field = nullptr;
			std::string_view value;
			bool ok = false;
			std::string error;
			std::string literal;
			// The field's own validated value, captured while it was still live - see
			// this file's header comment. Meaningless unless ok is true.
			CapturedValue captured;
		};

		// A plain, captureless function so it converts to Settings::LockedOperation
		// (bool(*)(void*) noexcept) - WithConfigurationLock/WithLoadLock take that
		// exact function-pointer type, not std::function, the same POD-only SEH
		// discipline Settings.h documents for every locked operation.
		[[nodiscard]] bool TrialSetImpl(void* a_context) noexcept
		{
			auto* const trial = static_cast<Trial*>(a_context);
			if (!trial || !trial->field) {
				return false;
			}
			try {
				const auto before = CaptureFullSnapshot();
				const auto beforePresentKeys = PresentKeys();
				const auto beforeModeSource = CurrentModeSource();

				std::string setError;
				if (!trial->field->trySet(trial->value, setError)) {
					trial->error = setError;
					return true;
				}
				// Validate()'s mode/quality/enabled alias resolution reads
				// Settings::WasPresent() - a key this trial is about to introduce for
				// the first time (absent from both the base TOML and any existing
				// override) must read as present for that resolution to see the value
				// this trial just wrote rather than treating it as still unset.
				MarkPresent(trial->field->key);

				const auto mid = CaptureFullSnapshot();
				Validate();
				const auto after = CaptureFullSnapshot();
				trial->literal = trial->field->literal();
				trial->captured = trial->field->captureLive();
				const auto diffKey = FirstDifferingKey(mid, after);

				RestoreFullSnapshot(before);
				PresentKeys() = beforePresentKeys;
				g_modeSource.store(beforeModeSource, std::memory_order_release);

				if (!diffKey.empty()) {
					trial->error = "would change '" + std::string(diffKey) +
					                "' - refusing to persist a configuration the next launch would rewrite";
					return true;
				}
				trial->ok = true;
				return true;
			} catch (const std::exception& e) {
				trial->error = std::string{ "trial validation threw: " } + e.what();
				return true;
			} catch (...) {
				trial->error = "trial validation threw a non-standard exception";
				return true;
			}
		}

		[[nodiscard]] bool RunTrialUnderLoadLock(void* a_context) noexcept
		{
			// configurationLock is already held by the caller (WithConfigurationLock,
			// below) - this acquires loadLock inside it, the fixed
			// configurationLock -> loadLock order Settings.h documents, so a
			// concurrent Configuration::Apply()/load() can never interleave with a
			// trial mid-way.
			return Settings::WithLoadLock(TrialSetImpl, a_context);
		}

		// ---- atomic file replace, performed on whatever thread runs it ----

		[[nodiscard]] bool AtomicReplace(const std::filesystem::path& a_target, const std::string& a_content, std::string& a_error)
		{
			try {
				std::error_code ec;
				std::filesystem::create_directories(a_target.parent_path(), ec);

				auto tempPath = a_target;
				tempPath += L".tmp";
				{
					std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
					if (!out) {
						a_error = "could not open the temporary override file for write";
						return false;
					}
					out.write(a_content.data(), static_cast<std::streamsize>(a_content.size()));
					out.flush();
					if (!out) {
						a_error = "write or flush to the temporary override file failed";
						return false;
					}
				}
				std::filesystem::rename(tempPath, a_target, ec);
				if (ec) {
					a_error = "atomic rename over the override file failed: " + ec.message();
					std::error_code removeEc;
					std::filesystem::remove(tempPath, removeEc);
					return false;
				}
				return true;
			} catch (const std::exception& e) {
				a_error = std::string{ "override write threw: " } + e.what();
				return false;
			} catch (...) {
				a_error = "override write threw a non-standard exception";
				return false;
			}
		}

		struct WriteRequest
		{
			std::filesystem::path targetPath;
			std::string content;
			std::atomic_bool done{ false };
			std::atomic_bool ok{ false };
			std::string error;
		};

		void PerformQueuedWrite(const std::shared_ptr<WriteRequest>& a_request) noexcept
		{
			std::string error;
			const bool ok = AtomicReplace(a_request->targetPath, a_request->content, error);
			a_request->error = error;
			a_request->ok.store(ok, std::memory_order_release);
			a_request->done.store(true, std::memory_order_release);
		}

		// Queues the actual write through the game thread's task queue and blocks
		// (bounded) for it to finish. shared_ptr keeps the request alive even if this
		// wait times out before the queued task runs - the task still completes
		// safely later against its own copy, it just is not waited on any further.
		[[nodiscard]] bool WriteOverrideFile(const std::filesystem::path& a_target, std::string a_content, std::string& a_error)
		{
			auto request = std::make_shared<WriteRequest>();
			request->targetPath = a_target;
			request->content = std::move(a_content);

			const auto* const tasks = F4SE::GetTaskInterface();
			bool queued = false;
			if (tasks) {
				try {
					tasks->AddTask([request] { PerformQueuedWrite(request); });
					queued = true;
				} catch (...) {
					// Falls through to the inline path below, matching HudMessages.cpp's
					// own "a failed AddTask is not a lost write" handling.
				}
			}
			if (!queued) {
				// No task interface (e.g. a devbench-only harness that never reached
				// F4SE::Init) or a failed queue attempt - perform it inline rather than
				// silently dropping a request Settings::Validate() already accepted.
				PerformQueuedWrite(request);
			} else {
				using namespace std::chrono_literals;
				const auto deadline = std::chrono::steady_clock::now() + 5s;
				while (!request->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
					std::this_thread::sleep_for(5ms);
				}
				if (!request->done.load(std::memory_order_acquire)) {
					a_error = "the override write did not complete on the game thread within 5s";
					return false;
				}
			}
			a_error = request->error;
			return request->ok.load(std::memory_order_acquire);
		}

		// ---- override file rebuild ----

		[[nodiscard]] toml::table ParseBaseTable() noexcept
		{
			try {
				return toml::parse_file("Data/F4SE/Plugins/Fo4VRUpscaler.toml"sv);
			} catch (...) {
				// The base TOML failing to parse here is already logged loudly by the
				// ordinary load path; returning an empty table just means
				// valueEqualsBase() below never matches, so the write below never
				// silently drops the user's requested value because "the shipped
				// value" could not be determined.
				return {};
			}
		}

		// An override file that exists but fails to parse is reported rather than
		// silently rebuilt: Set() below refuses the write in that case instead of
		// starting from an empty table, which would silently discard every other key
		// the user had persisted. The ordinary load path (Settings::load(), unrelated
		// to this writer) already tolerates a malformed override on its own, so a
		// user is never locked out of the game over this - only out of writing new
		// settings until the file is repaired or removed.
		struct ExistingOverride
		{
			toml::table table;
			bool malformed = false;
			std::string parseError;
		};

		[[nodiscard]] ExistingOverride ParseExistingOverride(const std::filesystem::path& a_path) noexcept
		{
			ExistingOverride result{};
			std::error_code ec;
			if (a_path.empty() || !std::filesystem::exists(a_path, ec) || ec) {
				return result;
			}
			try {
				result.table = toml::parse_file(a_path.string());
			} catch (const std::exception& e) {
				result.malformed = true;
				result.parseError = e.what();
			}
			return result;
		}

		constexpr std::string_view kOverrideHeader =
			"# Fo4VR Upscaler user overrides.\n"
			"# Generated by the mod - do not hand-edit; comments are not preserved on the\n"
			"# next write. Data/F4SE/Plugins/Fo4VRUpscaler.toml documents every key and its\n"
			"# default; this file holds only the keys changed from that default.\n"sv;
	}

	SetResult Set(std::string_view a_key, std::string_view a_value) noexcept
	{
		SetResult result{};
		result.key.assign(a_key);
		try {
			const FieldDescriptor* field = nullptr;
			for (const auto& f : FieldDescriptors()) {
				if (f.key == a_key) {
					field = &f;
					break;
				}
			}
			if (!field) {
				result.reason = "unknown key '" + std::string(a_key) + "'";
				return result;
			}

			Trial trial{ field, a_value };
			if (!Settings::WithConfigurationLock(RunTrialUnderLoadLock, &trial)) {
				result.reason = trial.error.empty() ?
				                     "trial validation could not run (lock contention or a hardware fault)" :
				                     trial.error;
				return result;
			}
			if (!trial.ok) {
				result.reason = trial.error;
				return result;
			}

			result.accepted = true;
			result.value = trial.literal;

			const auto overridePath = Settings::Paths::OverrideFilePath();
			if (overridePath.empty()) {
				result.reason = "no writable F4SE user directory - Documents redirection appears broken";
				return result;
			}

			auto existing = ParseExistingOverride(overridePath);
			if (existing.malformed) {
				// Refuse rather than rebuild from empty - see ParseExistingOverride's
				// header comment. accepted flips back to false here: the request itself
				// was legitimate, but nothing about it, or anything already in the
				// file, has been touched.
				result.accepted = false;
				result.value.clear();
				result.reason = "Fo4VRUpscalerOverride.toml (" + overridePath.string() +
				                 ") is malformed (" + existing.parseError +
				                 ") - repair or remove it before persisting new settings";
				return result;
			}
			toml::table current = std::move(existing.table);
			const toml::table base = ParseBaseTable();

			if (field->valueEqualsBase(base, trial.captured)) {
				field->eraseFrom(current);
			} else {
				field->insertValueInto(current, trial.captured);
			}
			for (const auto section : { "Fo4VRUpscaler"sv, "Diagnostics"sv }) {
				if (auto* sec = current[section].as_table(); sec && sec->empty()) {
					current.erase(section);
				}
			}

			std::ostringstream oss;
			oss << kOverrideHeader;
			if (!current.empty()) {
				oss << current;
				oss << "\n";
			}

			std::string writeError;
			const bool wrote = WriteOverrideFile(overridePath, oss.str(), writeError);
			if (!wrote) {
				result.reason = "validated, but the override write failed: " + writeError;
				return result;
			}
			result.persisted = true;

			if (field->restart == kRestart) {
				result.needsRestart = true;
				return result;
			}

			const auto applied = Configuration::Apply();
			result.tookEffectNow = applied.runtimeApplied;
			if (!applied.runtimeApplied) {
				result.reason = "persisted, but the live reload did not publish a complete runtime configuration; see Fo4VRUpscaler.log";
			}
			return result;
		} catch (const std::exception& e) {
			result.reason = std::string{ "Set() threw: " } + e.what();
			return result;
		} catch (...) {
			result.reason = "Set() threw a non-standard exception";
			return result;
		}
	}
}
