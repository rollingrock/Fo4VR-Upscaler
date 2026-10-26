#include "Settings/Writer.h"

#include "Settings/Configuration.h"
#include "Settings/Settings.h"
#include "Settings/UserPaths.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
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
// THE TRIAL JUDGES THE FILES, NOT THE SESSION. *Settings::mode is not the file layer's
// mode. Two separate mechanisms move it away, and both outlive the request that caused
// them:
//
//   - Settings::Validate() substitutes a standing live /mode override for *Settings::mode
//     on every load and writes it into that global, so while one stands the file's own
//     mode is no longer recoverable from the live settings;
//   - Configuration::ApplyLockedImpl refuses a file-driven mode change this session cannot
//     make (a frozen reduced bracket, or a reduced mode on a session that armed none) and
//     corrects *Settings::mode back to the mode that is actually running, while the
//     override file keeps the refused value. That divergence has no override standing
//     behind it and lasts for the rest of the session.
//
// So the trial runs Validate(a_consultRuntimeOverride = false) and, unconditionally,
// first puts the file layer's own mode back into the live global - resolved through the
// same Policy::ResolveModeAliases the loader uses, so an absent mode key still resolves
// the way a fresh load would. Unconditionally, rather than only while an override stands,
// because the refusal above is the longer-lived of the two divergences and looks exactly
// like agreement from here: nothing in the live globals says "this is the session's mode,
// not the file's". When the two already agree the install is a self-assignment and costs
// nothing. Without both halves the writer refuses values the next launch would keep - a
// renderScaleOverride of 1.0 beside a persisted mode = 'dlaa', named as though the loader
// would resolve it away - and accepts values it would rewrite, such as that same 1.0
// beside a persisted reduced mode, or a diagnostic gate beside a base mode that the gate
// will force to "off".
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
			// The mode the two settings files resolve to, and whether either file declares
			// the key at all - read from the tables Set() parsed, never from the live
			// globals. Installed before Validate() runs, on every trial: a standing runtime
			// override and a refused file-driven mode change both leave the live *mode
			// disagreeing with the file layer, and only the first of the two is visible
			// from here. See this file's header comment.
			std::string fileMode;
			bool fileModeDeclared = false;
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

				{
					// The file layer's own mode, resolved exactly as a fresh load would
					// resolve it: the deprecated quality alias and enabled = false still
					// apply, and their inputs (*quality, *enabled and their presence flags)
					// are file-truthful in the live globals already - neither a runtime
					// override nor a refused mode change rewrites those. Restored with
					// everything else below. Unconditional on purpose; see this file's
					// header comment for the two divergences it covers, only one of which
					// this code could detect if it wanted to.
					const auto alias = Policy::ResolveModeAliases(
						trial->fileMode, trial->fileModeDeclared,
						*Settings::quality, WasPresent("quality"sv),
						*Settings::enabled, WasPresent("enabled"sv));
					*Settings::mode = std::string(alias.mode);
				}

				std::string setError;
				if (!trial->field->trySet(trial->value, setError)) {
					// The file mode above is already installed on this path, so this early
					// return has to restore exactly as the accepted path below does: a value
					// that failed to parse must leave the live settings untouched.
					RestoreFullSnapshot(before);
					PresentKeys() = beforePresentKeys;
					g_modeSource.store(beforeModeSource, std::memory_order_release);
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
				Validate(/*a_consultRuntimeOverride=*/false);
				const auto after = CaptureFullSnapshot();
				trial->literal = trial->field->literal();
				trial->captured = trial->field->captureLive();
				const auto diffKey = FirstDifferingKey(mid, after);

				RestoreFullSnapshot(before);
				PresentKeys() = beforePresentKeys;
				g_modeSource.store(beforeModeSource, std::memory_order_release);

				if (!diffKey.empty()) {
					// Two different refusals wearing one sentence before: the requested
					// value being one the loader itself resolves away, and the requested
					// value being fine while some other key in the merged file layer is
					// already contradictory. The first names what the loader would resolve
					// it to; the second names the key that has to be settled first.
					if (diffKey == trial->field->key) {
						trial->error = "'" + std::string(diffKey) + "' = " + std::string(trial->value) +
						                " is not a value this configuration keeps - loading it resolves the key to " +
						                trial->literal + "; see Fo4VRUpscaler.log for the rule that moved it";
					} else {
						trial->error = "would change '" + std::string(diffKey) +
						                "' - refusing to persist a configuration the next launch would rewrite; settle '" +
						                std::string(diffKey) + "' first (see Fo4VRUpscaler.log for the rule that moves it)";
					}
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
			// Claimed exactly once, by whichever of the two sides gets there first: the
			// queued task claims it to write, and the waiter below claims it to abandon the
			// request when its budget expires. Whoever loses the exchange does nothing.
			// Without it a task that runs after the wait timed out still replaces the file,
			// while Set() has already told its caller persisted = false - a half-state
			// where the answer and the disk disagree for the rest of the session.
			std::atomic_bool claimed{ false };
			std::atomic_bool done{ false };
			std::atomic_bool ok{ false };
			std::string error;
		};

		void PerformQueuedWrite(const std::shared_ptr<WriteRequest>& a_request) noexcept
		{
			bool expected = false;
			if (!a_request->claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				// Abandoned before this task ran. Nothing is written and nothing is
				// reported: the waiter already answered its caller.
				return;
			}
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
					bool expected = false;
					if (request->claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
						// The task had not started. It never will now, so nothing was
						// written and the file is exactly as it was.
						a_error = "the game thread did not run the queued override write within 5s; nothing was written";
						return false;
					}
					// The task claimed the request first and is inside the replace right
					// now. Cancelling is no longer possible, so wait a short grace period
					// for it rather than reporting an outcome the disk may contradict.
					const auto graceDeadline = std::chrono::steady_clock::now() + 1s;
					while (!request->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < graceDeadline) {
						std::this_thread::sleep_for(5ms);
					}
					if (!request->done.load(std::memory_order_acquire)) {
						a_error = "the override write was still in flight when the 5s wait expired; the file may or may not have been replaced - re-read it before writing again";
						return false;
					}
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
				result.table = toml::parse_file(a_path.wstring());  // VERIFIED (toml++ parse_file): a narrow path is widened as UTF-8; string() is the ANSI code page
			} catch (const std::exception& e) {
				result.malformed = true;
				result.parseError = e.what();
			}
			return result;
		}

		// mode as the two files alone resolve it, with no runtime override anywhere in it:
		// the override file's own (non-legacy) table first, then the base table with
		// LoadOne's legacy [Fo4VRUpscaler] fallback. Mirrors Settings::LoadOne's precedence
		// exactly - the override wins per key, and only the base falls back to the legacy
		// table, because the file this writer emits never uses that table. declared is what
		// Validate()'s alias resolver reads as WasPresent("mode").
		struct FileMode
		{
			std::string value;
			bool declared = false;
		};

		[[nodiscard]] FileMode ResolveFileMode(const toml::table& a_base, const toml::table* a_override)
		{
			if (a_override) {
				if (const auto tweak = (*a_override)[Settings::mode.group()][Settings::mode.key()]; tweak) {
					if (const auto value = tweak.value<std::string>(); value) {
						return FileMode{ *value, true };
					}
				}
			}
			const auto declared =
				a_base[Settings::mode.group()][Settings::mode.key()].is_string() ||
				a_base[kLegacyGroup][Settings::mode.key()].is_string();
			return FileMode{
				ResolveBaseOnly(Settings::mode, a_base, CompiledDefaults().mode),
				declared
			};
		}

		// Serializes the whole parse-modify-write of the override file, trial included, so
		// the table a call rebuilds is the one it just read and the value it validated is
		// still the file it writes into. Two concurrent /config/set calls each read the
		// file, change their own key and write the result back; without this the second
		// write is built from a table read before the first one landed, and that key is
		// silently gone. Taken outside Settings::configurationLock/loadLock and never taken
		// by anything that holds either, so it stays a leaf.
		std::mutex g_overrideFileLock;

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

			const auto overridePath = Settings::Paths::OverrideFilePath();

			// Held from the parse below through the write at the bottom - see
			// g_overrideFileLock.
			std::scoped_lock fileLock(g_overrideFileLock);

			auto existing = ParseExistingOverride(overridePath);
			const toml::table base = ParseBaseTable();
			const auto fileMode = ResolveFileMode(base, existing.malformed ? nullptr : &existing.table);

			Trial trial{ field, a_value };
			trial.fileMode = fileMode.value;
			trial.fileModeDeclared = fileMode.declared;
			if (!Settings::WithConfigurationLock(RunTrialUnderLoadLock, &trial)) {
				result.reason = trial.error.empty() ?
				                     "trial validation could not run (lock contention or a hardware fault)" :
				                     trial.error;
				return result;
			}
			if (!trial.ok) {
				result.reason = trial.error;
				// Validate()'s own lines name Fo4VRUpscaler.toml, because they are written
				// for the loader; this one names where the refused value actually came from,
				// so a reader of the log can tell a bad file from a bad request.
				logger::warn(FMT_STRING("settings write refused: '{}' = '{}' arrived from a route, not from Fo4VRUpscaler.toml - {}"),
					a_key, a_value, result.reason);
				return result;
			}

			result.accepted = true;
			result.value = trial.literal;

			if (overridePath.empty()) {
				result.reason = "no writable F4SE user directory - Documents redirection appears broken";
				return result;
			}

			if (existing.malformed) {
				// Refuse rather than rebuild from empty - see ParseExistingOverride's
				// header comment. accepted flips back to false here: the request itself
				// was legitimate, but nothing about it, or anything already in the
				// file, has been touched.
				result.accepted = false;
				result.value.clear();
				// UTF-8, not string(): the ANSI conversion throws on a path outside the code page.
				const auto pathUtf8 = overridePath.u8string();
				result.reason = "Fo4VRUpscalerOverride.toml (" +
				                std::string(reinterpret_cast<const char*>(pathUtf8.data()), pathUtf8.size()) +
				                 ") is malformed (" + existing.parseError +
				                 ") - repair or remove it before persisting new settings";
				return result;
			}
			toml::table current = std::move(existing.table);

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
			if (applied.runtimeConfigurationDeferred) {
				// Not a failure: the value is on disk and validated, and the reload was
				// refused only so a post-fault cool-down keeps the gates its re-arm needs.
				// The next reload after the session re-arms picks it up.
				result.reason = "persisted, but the live reload was refused while the session is recovering from a fault; re-issue it once /state reports the session active";
			} else if (!applied.runtimeApplied) {
				result.reason = "persisted, but the live reload did not publish a complete runtime configuration; see Fo4VRUpscaler.log";
			} else {
				// A published configuration is not the same fact as this key's own effect
				// landing. Two live keys have a path of their own that can decline while
				// the apply still succeeds - the ratio repatch and the mode-switch
				// admissibility rule - and reporting the apply's verdict for either is how
				// a caller gets told a value took effect that did not. needsRestart is the
				// honest other half: the value IS on disk, and the next launch reads it.
				if (field->key == Settings::renderScaleOverride.key()) {
					switch (applied.ratioEffect) {
					case Configuration::RatioEffect::kApplied:
					case Configuration::RatioEffect::kAlreadyLive:
						break;
					case Configuration::RatioEffect::kRefused:
						result.tookEffectNow = false;
						result.needsRestart = true;
						result.reason = "persisted, but the live ratio request was refused - see Fo4VRUpscaler.log for which refusal; the next launch reads the value";
						break;
					case Configuration::RatioEffect::kNotApplicable:
					default:
						result.tookEffectNow = false;
						result.needsRestart = true;
						result.reason = "persisted, but this session has no live reduced-world ratio to change - its mode is 'off' or 'dlaa', or its bracket was never armed; the next launch reads the value";
						break;
					}
				} else if (field->key == Settings::mode.key()) {
					// The honest test for mode is not which outcome the admissibility rule
					// reached but whether the session ended up running the value that was
					// persisted. The two differ whenever a live /mode override is standing:
					// the rule sees no change at all (the override is what the apply reads),
					// while the file now holds a mode nothing is running.
					const auto liveMode = Configuration::SwitchSnapshot().liveMode;
					if (liveMode != a_value) {
						result.tookEffectNow = false;
						result.needsRestart = true;
						result.reason = applied.modeDecided ?
						                     "persisted, but not applied now (" +
						                         std::string(Configuration::SwitchOutcomeName(applied.modeOutcome)) +
						                         ") - this session is still running '" + liveMode +
						                         "'; the next launch starts in the persisted mode" :
						                     "persisted, but a live mode switch is standing, so this session keeps running '" +
						                         liveMode +
						                         "' - the next launch starts in the persisted mode unless a /mode request replaces it first";
					}
				}
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
