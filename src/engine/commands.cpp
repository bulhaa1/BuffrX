#include "commands.h"
#include "protocol_commands.h"
#include "update_feed.h"
#include "update_math.h"
#include "update_apply.h"
#include "update_download.h"
#include "update_pending.h"
#include "update_policy.h"
#include "public_links.h"

#include <windows.h>
#include <shellapi.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <psapi.h>
#include <set>
#include <vector>

#include "../shared/files.h"
#include "../shared/log.h"
#include "gpu.h"
#include "capture_follow.h"
#include "capacity_memory.h"
#include "clip_check.h"
#include "command_policy.h"
#include "encoder_settings.h"
#include "lock_watch.h"
#include "library.h"
#include "media.h"
#include "media_export.h"
#include "save_publish.h"
#include "session_store.h"
#include "timeline_overlay.h"
#include "timeline_pins.h"
#include "timeline_retention.h"
#include "windowed_game.h"
#include <fstream>
#include <iterator>

#include "dir_listing.h"
#include "timeline_loudness.h"
#include "timeline_thumbs.h"
#include "link.h"
#include "short_link.h"
#include "share.h"
#include "obs_host.h"
#include "recovery_policy.h"

namespace bx {
namespace {

// The protocol version is a number on the wire: a client compares it, it does not parse it.
constexpr int64_t kProtocolVersion = 1;

// The download thread's only way into the interface: one atomic percent. No lock, no window, no engine.
void update_download_progress(void* context, int64_t received, int64_t total) {
  auto* percent = static_cast<std::atomic<int>*>(context);
  if (total <= 0) return;
  const int value = static_cast<int>(std::clamp<int64_t>(received * 100 / total, 0, 100));
  percent->store(value, std::memory_order_relaxed);
}

bool update_space_available(const std::string& directory) {
  ULARGE_INTEGER free_bytes{};
  if (!GetDiskFreeSpaceExA(directory.c_str(), &free_bytes, nullptr, nullptr)) return true;
  return free_bytes.QuadPart >= static_cast<ULONGLONG>(kUpdateMinFreeBytes);
}

// The cards the panel can show at once, which is how many thumbnails a listing decodes up front. The
// rail lays them out in columns, so this is generous rather than exact: it is a decode budget, not a
// layout measurement.
constexpr int kDefaultVisibleCards = 24;

// A thumbnail is wanted, not large: the Timeline preview's strip draws it in a cell, and 24 are asked for at
// once. Small enough that a missing one costs a decode of one frame and nothing else.
constexpr int kTimelineThumbWidth = 240;

bool read_file(const std::string& path, std::string& out) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, read);
  std::fclose(file);
  return true;
}

std::string file_name_of(const std::string& path) {
  size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// One rule for a command that names footage: the index is read, and only a Timeline entry - not a clip
// session - is handed on. `what` completes "only timeline footage ", so each command still says what
// it wanted the entry for.
bool load_timeline_entry(const std::string& id, SessionEntry& entry, std::string& error, const char* what) {
  if (!session_store_load_one(id, entry, error)) return false;
  if (entry.kind == "timeline") return true;
  error = std::string("only timeline footage ") + what;
  return false;
}

// The strip's spans: one per playable chunk, moved onto the session's own clock (timeline_thumbs.h
// decides which moments become pictures from here).
std::vector<TimelineThumbSpan> timeline_thumb_spans(const SessionEntry& entry, const std::vector<size_t>& playable) {
  std::vector<TimelineThumbSpan> spans;
  spans.reserve(playable.size());
  for (size_t index : playable) {
    const SessionChunk& chunk = entry.chunks[index];
    TimelineThumbSpan span;
    span.chunk_index = index;
    span.key = timeline_thumb_key(chunk.file);
    span.start_ms = timeline_session_ms(chunk.start_ms, entry.started_ms);
    span.end_ms = timeline_session_ms(chunk.end_ms, entry.started_ms);
    spans.push_back(std::move(span));
  }
  return spans;
}

Json media_json(const std::vector<MediaItem>& items, bool with_thumbnails) {
  Json array = Json::array();
  for (const auto& item : items) {
    Json entry = Json::object();
    entry.set("file", Json(item.file));
    entry.set("name", Json(item.name));
    entry.set("size", Json(item.size));
    entry.set("mtime", Json(item.modified));
    entry.set("duration", Json(item.duration));
    // The library folder: the game name for a clip ("Counter-Strike 2"), "Display Capture",
    // "Recordings", "Edits"; empty for a file at the root.
    entry.set("folder", Json(item.folder));
    if (with_thumbnails && !item.thumbnail.empty()) {
      // Thumbnails are served by the shell's virtual host, never as absolute paths.
      entry.set("thumbnail", Json("https://buffrx.local/thumbnails/" + file_name_of(item.thumbnail)));
    }
    array.push(entry);
  }
  return array;
}

Json audio_json(const Config& c) {
  Json audio = Json::object();
  audio.set("desktop_enabled", Json(c.desktop_enabled));
  audio.set("desktop_volume", Json(c.desktop_volume));
  audio.set("desktop_muted", Json(c.desktop_muted));
  audio.set("desktop_device", Json(c.desktop_device));
  audio.set("mic_enabled", Json(c.mic_enabled));
  audio.set("mic_volume", Json(c.mic_volume));
  audio.set("mic_gain_db", Json(c.mic_gain_db));
  audio.set("mic_muted", Json(c.mic_muted));
  audio.set("mic_device", Json(c.mic_device));
  audio.set("separate_tracks", Json(c.separate_tracks));
  audio.set("audio_tracks", Json(c.audio_tracks));
  audio.set("mic_noise_suppress", Json(c.mic_noise_suppress));
  audio.set("mic_noise_gate", Json(c.mic_noise_gate));
  audio.set("audio_bitrate_kbps", Json(c.audio_bitrate_kbps));
  return audio;
}

// The graphics vendor and adapter name, read once: hello is answered many times, DXGI is not asked
// every time.
Json gpu_json() {
  static const Json cached = [] {
    Json gpu = Json::object();
    uint32_t vendor_id = 0;
    std::string name;
    if (gpu_adapter(vendor_id, name)) {
      gpu.set("vendor", Json(gpu_vendor_from_id(vendor_id)));
      gpu.set("name", Json(name));
    } else {
      gpu.set("vendor", Json(std::string("other")));
      gpu.set("name", Json(std::string("")));
    }
    return gpu;
  }();
  return cached;
}

// The seven bindings as config fields: one row per action, so the engine's registration, the pad
// lookup, the setter and the JSON report cannot disagree about which field an action means.
struct HotkeyField {
  const char* action;
  const char* name;  // the word a conflict names it by ("save clip")
  std::string Config::*keyboard;
  std::string Config::*pad;
};
constexpr HotkeyField kHotkeyFields[] = {
    {"save_replay", "save clip", &Config::hotkey_save, &Config::hotkey_save_pad},
    {"quick_clip", "quick clip", &Config::hotkey_quick, &Config::hotkey_quick_pad},
    {"screenshot", "screenshot", &Config::hotkey_screenshot, &Config::hotkey_screenshot_pad},
    {"toggle_record", "record", &Config::hotkey_record, &Config::hotkey_record_pad},
    {"toggle_ui", "panel", &Config::hotkey_toggle_ui, &Config::hotkey_toggle_ui_pad},
    {"view_clip", "timeline", &Config::hotkey_view_clip, &Config::hotkey_view_clip_pad},
    {"add_marker", "marker", &Config::hotkey_marker, &Config::hotkey_marker_pad},
};

const HotkeyField* hotkey_field_for(const std::string& action) {
  for (const HotkeyField& field : kHotkeyFields) {
    if (action == field.action) return &field;
  }
  return nullptr;
}

std::string hotkey_action_name(const std::string& action) {
  const HotkeyField* field = hotkey_field_for(action);
  return field ? field->name : action;
}

// The seven actions the engine binds, in the order `apply_hotkeys` registers them and `hotkeys.set`
// reports a conflict in.
const std::vector<std::string>& all_hotkey_actions() {
  static const std::vector<std::string> actions = [] {
    std::vector<std::string> list;
    for (const HotkeyField& field : kHotkeyFields) list.emplace_back(field.action);
    return list;
  }();
  return actions;
}

bool known_hotkey_action(const std::string& action) { return hotkey_field_for(action) != nullptr; }

std::string pad_chord_for(const Config& c, const std::string& action) {
  const HotkeyField* field = hotkey_field_for(action);
  return field ? c.*(field->pad) : std::string();
}

bool set_hotkey_chord(Config& c, const std::string& action, bool controller, const std::string& chord) {
  const HotkeyField* field = hotkey_field_for(action);
  if (!field) return false;
  c.*(controller ? field->pad : field->keyboard) = chord;
  return true;
}

Json hotkeys_json(const Config& c, const std::vector<HotkeyBinding>& bindings,
                  const std::map<std::string, std::string>& pad_problems, int controllers,
                  bool pad_available, bool suspended = false) {
  Json json = Json::object();
  for (const HotkeyField& field : kHotkeyFields) {
    json.set(field.action, Json(c.*(field.keyboard)));
    json.set(std::string(field.action) + "_pad", Json(c.*(field.pad)));
  }
  Json problems = Json::object();
  for (const auto& binding : bindings) {
    // A cleared binding is unbound on purpose, not broken.
    if (binding.chord.empty()) continue;
    if (!binding.valid || (!binding.registered && !suspended)) {
      problems.set(binding.action, Json(binding.problem.empty() ? std::string("not registered") : binding.problem));
    }
  }
  json.set("problems", problems);
  Json pad_problem_json = Json::object();
  for (const auto& entry : pad_problems) pad_problem_json.set(entry.first, Json(entry.second));
  json.set("pad_problems", pad_problem_json);
  json.set("controllers", Json(controllers));
  json.set("pad_available", Json(pad_available));
  return json;
}

}  // namespace

Protocol::Protocol(ObsHost& host, PlatformWindow& window, std::string data_root, bool portable, std::string config_path)
    : host_(host),
      window_(window),
      data_root_(std::move(data_root)),
      config_path_(std::move(config_path)),
      portable_(portable) {
  profiles_path_ = data_root_ + "\\profiles.json";
  // An update that installed itself before this run leaves its installer and state behind; one that failed
  // keeps them, so the next start can try again and the card can say why.
  update_cleanup_applied(data_root_, kEngineVersion);
  PendingUpdate pending;
  std::string pending_error;
  if (update_pending_read(data_root_, pending, pending_error)) {
    update_version_ = pending.version;
    if (update_install_exhausted(pending.install_attempts)) {
      update_state_ = "failed";
      update_state_error_ = pending.error.empty() ? "the update did not install after three tries" : pending.error;
    } else if (GetFileAttributesA(update_failure_path(data_root_).c_str()) != INVALID_FILE_ATTRIBUTES) {
      update_state_ = "failed";
      update_state_error_ = "The update could not be installed. Try again.";
    } else if (GetFileAttributesA(pending.setup.c_str()) != INVALID_FILE_ATTRIBUTES && pending.error.empty()) {
      update_state_ = "ready";
      update_state_error_ = pending.error;
      if (kAutoUpdate) update_make_apply_marker(data_root_);
    } else if (GetFileAttributesA(pending.setup.c_str()) == INVALID_FILE_ATTRIBUTES) {
      // An interrupted or quarantined download is fetched again on this run's next update check.
      update_pending_clear(data_root_);
      update_state_ = "idle";
    } else {
      update_state_ = "failed";
      update_state_error_ = pending.error;
      update_drop_apply_marker(data_root_);
    }
  }
}

Protocol::~Protocol() { stop_update_download(); }

void Protocol::stop_update_download() {
  update_stop_.store(true, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(update_thread_mutex_);
  if (update_thread_.joinable()) update_thread_.join();
}

void Protocol::set_update_state(const std::string& state, const std::string& error) {
  std::lock_guard<std::mutex> lock(update_mutex_);
  update_state_ = state;
  update_state_error_ = error;
}

Json Protocol::update_json() {
  Json json = Json::object();
  {
    std::lock_guard<std::mutex> lock(update_mutex_);
    json.set("state", Json(update_state_));
    json.set("version", Json(update_version_));
    json.set("error", Json(update_state_error_));
  }
  json.set("percent", Json(update_percent_.load(std::memory_order_relaxed)));
  return json;
}

void Protocol::maybe_start_update_download(const Json& feed) {
  if (!kAutoUpdate) return;
  std::lock_guard<std::mutex> worker_lock(update_thread_mutex_);
  if (update_stop_.load(std::memory_order_relaxed)) return;
  const std::string latest = feed["latest"].as_string();
  const std::string url = feed["url"].as_string();
  const std::string digest = feed["digest"].as_string();
  const std::string notes = feed["notes"].as_string();
  if (latest.empty() || !update_is_newer(latest, kEngineVersion)) return;

  bool start = false;
  {
    std::lock_guard<std::mutex> lock(update_mutex_);
    if (update_running_) return;
    // Keep a ready installer stable until it is installed; a newer download must not replace it during restart.
    if (update_state_ == "ready") {
      PendingUpdate ready;
      std::string ready_error;
      if (update_pending_read(data_root_, ready, ready_error) &&
          GetFileAttributesA(ready.setup.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    }
    // A pending file from an earlier run decides two things: a finished download is not fetched again, and an
    // update that failed to install three times is not downloaded again (the card says why).
    PendingUpdate existing;
    std::string existing_error;
    if (update_pending_read(data_root_, existing, existing_error) && existing.version == latest) {
      update_version_ = latest;
      if (update_install_exhausted(existing.install_attempts)) {
        update_state_ = "failed";
        update_state_error_ = existing.error.empty() ? "the update did not install after three tries" : existing.error;
        return;
      }
      if (existing.error.empty() && GetFileAttributesA(existing.setup.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(update_failure_path(data_root_).c_str());
        update_state_ = "ready";
        update_state_error_ = existing.error;
        update_make_apply_marker(data_root_);
        return;
      }
    }
    if (!update_is_installable(latest, kEngineVersion, digest)) {
      // A release whose asset carries no digest is still announced; it opens in the browser, because nothing
      // is run that cannot be checked.
      update_state_ = "manual";
      update_state_error_ = feed["error"].as_string();
      update_version_ = latest;
      return;
    }
    if (!update_space_available(update_dir(data_root_))) {
      update_state_ = "failed";
      update_state_error_ = "not enough free space for the update";
      update_version_ = latest;
      return;
    }
    update_version_ = latest;
    update_state_ = "downloading";
    update_state_error_.clear();
    update_percent_.store(0, std::memory_order_relaxed);
    update_running_ = true;
    start = true;
  }
  if (!start) return;
  // A previous worker has finished (update_running_ was false): joining it here costs nothing.
  if (update_thread_.joinable()) update_thread_.join();
  update_thread_ = std::thread([this, latest, url, digest, notes] {
    run_update_download(latest, url, digest, notes);
  });
}

void Protocol::run_update_download(std::string version, std::string url, std::string digest, std::string notes) {
  CreateDirectoryA(update_dir(data_root_).c_str(), nullptr);
  const std::string dest = update_installer_path(data_root_);
  std::string last_error;
  for (int attempt = 1; attempt <= kUpdateDownloadAttempts && !update_stop_.load(std::memory_order_relaxed); ++attempt) {
    update_percent_.store(0, std::memory_order_relaxed);
    set_update_state("downloading");
    const DownloadOutcome out =
        download_verified(url, digest, dest, 300000, &update_download_progress, &update_percent_, &update_stop_);
    if (out.ok) {
      PendingUpdate pending;
      pending.version = version;
      pending.setup = dest;
      pending.sha256 = digest;
      pending.notes = notes;
      pending.download_attempts = attempt;
      pending.downloaded_ms = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
      if (!update_pending_write(data_root_, pending) || !update_make_apply_marker(data_root_)) {
        last_error = "the downloaded update could not be kept";
        break;
      }
      DeleteFileA(update_failure_path(data_root_).c_str());
      update_percent_.store(100, std::memory_order_relaxed);
      set_update_state("ready");
      // A card for a window that is closed: the panel has its own toast and sheet when it is open.
      if (notifier) notifier("UPDATE READY", version + " is ready. Restart BuffrX to install it.");
      {
        std::lock_guard<std::mutex> lock(update_mutex_);
        update_running_ = false;
      }
      return;
    }
    last_error = out.error;
    if (out.error == "the update was cancelled" || update_stop_.load(std::memory_order_relaxed)) break;
    // Two minutes before another try, in a loop that notices a quit instead of sleeping through it.
    if (attempt < kUpdateDownloadAttempts)
      for (int waited = 0; waited < kUpdateRetryMs / 100 && !update_stop_.load(std::memory_order_relaxed); ++waited) Sleep(100);
  }
  set_update_state("failed", last_error.empty() ? "the update did not download" : last_error);
  {
    std::lock_guard<std::mutex> lock(update_mutex_);
    update_running_ = false;
  }
}

bool Protocol::spawn_pending_installer(std::string& error) {
  PendingUpdate pending;
  if (!update_pending_read(data_root_, pending, error)) return false;
  if (update_install_exhausted(pending.install_attempts)) {
    error = "the update did not install after three tries";
    return false;
  }
  if (!update_pending_verify(pending, error)) {
    DeleteFileA(pending.setup.c_str());
    error = error.empty() ? "the downloaded update did not check out" : error;
    pending.error = error;
    update_pending_write(data_root_, pending);
    update_drop_apply_marker(data_root_);
    set_update_state("failed", error);
    return false;
  }
  pending.install_attempts += 1;
  if (!update_pending_write(data_root_, pending)) {
    error = "the update state could not be saved";
    return false;
  }
  const InstallOutcome started = spawn_update_installer(data_root_, runtime_root_, false);
  if (!started.ok) {
    pending.error = started.error;
    update_pending_write(data_root_, pending);
    update_drop_apply_marker(data_root_);
    set_update_state("failed", started.error);
    error = started.error;
    return false;
  }
  set_update_state("installing");
  return true;
}


bool Protocol::load_settings(std::string& error) {
  if (!load_config(config_path_, config_, error)) {
    // A config file that cannot be parsed must not leave the engine pathless: without resolved paths a
    // recording has nowhere to go, and the session store would be handed an empty root.
    apply_path_defaults(config_, data_root_);
    return false;
  }
  apply_path_defaults(config_, data_root_);
  return true;
}

bool Protocol::persist(std::string& error) { return save_config(config_path_, config_, error); }

std::vector<std::string> Protocol::capabilities() { return protocol_capabilities(); }

std::string Protocol::make_event(const std::string& name, const Json& data) {
  Json event = Json::object();
  event.set("event", Json(name));
  event.set("data", data);
  return event.dump();
}

Json Protocol::config_json() { return config_to_json(config_); }

void Protocol::set_links(LinkService* links) {
  links_ = links;
  if (links_) links_->configure(config_.share_link_page, config_.share_sender_name, config_.share_upload_kbps);
}

// Whether a visible window belongs to this executable: the game still runs.
static bool game_window_alive(const std::string& exe) {
  for (const std::string& path : window_process_paths()) {
    if (process_path_is(path, exe)) return true;
  }
  return false;
}

// Whether the program still has a fullscreen window of its own: a game closed and reopened, as against a launcher or
// tray window of the same program, which says nothing about the game.
static bool game_fullscreen_alive(const std::string& exe) {
  const std::string wanted = normalized_exe_name(exe);
  for (const WindowInfo& window : enumerate_game_windows()) {
    if (normalized_exe_name(window.exe) == wanted) return true;
  }
  return false;
}

// The libraries a process has loaded, by name. Empty when the process will not say (anti-cheat protected games).
static std::vector<std::string> process_module_names(DWORD pid) {
  std::vector<std::string> names;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process) return names;
  HMODULE modules[1024];
  DWORD needed = 0;
  if (EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), 1024);
    for (size_t index = 0; index < count; ++index) {
      char name[MAX_PATH] = {0};
      if (GetModuleBaseNameA(process, modules[index], name, MAX_PATH) > 0) names.emplace_back(name);
    }
  }
  CloseHandle(process);
  return names;
}

std::string Protocol::poll_windowed_game(uint32_t now_ms, bool other_game_running) {
  if (!config_.timeline_windowed_games || other_game_running) {
    // A fullscreen game decides on its own, and a switch turned off forgets what it adopted.
    timeline_windowed_hwnd_ = 0;
    timeline_windowed_exe_.clear();
    return std::string();
  }
  if (timeline_windowed_hwnd_ != 0 &&
      (!window_still_exists(timeline_windowed_hwnd_) || game_process_excluded(timeline_windowed_exe_, config_.game_exclusions))) {
    timeline_windowed_hwnd_ = 0;
    timeline_windowed_exe_.clear();
  }
  if (timeline_windowed_hwnd_ == 0 && (timeline_windowed_scan_ms_ == 0 || now_ms - timeline_windowed_scan_ms_ >= 5000)) {
    timeline_windowed_scan_ms_ = now_ms;
    ForegroundWindow front;
    if (foreground_window(front) && !front.exe.empty() && !game_process_excluded(front.exe, config_.game_exclusions)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(reinterpret_cast<HWND>(front.hwnd), &pid);
      if (pid != 0 && pid != GetCurrentProcessId()) {
        auto verdict = timeline_windowed_verdicts_.find(front.exe);
        if (verdict == timeline_windowed_verdicts_.end() || now_ms - verdict->second.second >= 60000) {
          const bool game = modules_look_like_game(process_module_names(pid));
          verdict = timeline_windowed_verdicts_.insert_or_assign(front.exe, std::make_pair(game, now_ms)).first;
        }
        if (verdict->second.first) {
          timeline_windowed_hwnd_ = front.hwnd;
          timeline_windowed_exe_ = front.exe;
          BX_LOG_INFO("timeline: windowed game %s", front.exe.c_str());
        }
      }
    }
  }
  return timeline_windowed_exe_;
}

bool Protocol::game_in_focus() const {
  return game_capture_.capturing ||
         (!observed_exe_.empty() && !game_process_excluded(observed_exe_, config_.game_exclusions));
}

bool Protocol::take_screenshot(const std::string& format, std::string& path, std::string& error) {
  ForegroundWindow foreground;
  std::string game;
  if (foreground_window(foreground)) {
    game = screenshot_game_title(foreground.exe, foreground.title, foreground.fullscreen, config_.game_exclusions);
    const std::string exe = normalized_exe_name(foreground.exe);
    // A click in BuffrX moves focus away from the game; the capture detector still knows the live game.
    if (game.empty() && (exe == "buffrx-engine" || exe == "buffrx-ui") && game_capture_.capturing &&
        active_game_alive(GetTickCount())) {
      game = game_capture_.active_title.empty() ? game_capture_.active_exe : game_capture_.active_title;
    }
  }
  return host_.take_screenshot(format, path, error, game);
}

Json Protocol::timeline_status_json() const {
  Json timeline = Json::object();
  timeline.set("enabled", Json(config_.timeline_minutes > 0));
  timeline.set("state", Json(std::string(timeline_state_id(host_.timeline_state()))));
  timeline.set("minutes", Json(config_.timeline_minutes));
  timeline.set("everywhere", Json(config_.timeline_everywhere));
  // The program Timeline is recording as a game, so the person can see what it took for one.
  const std::string recorded = !timeline_windowed_exe_.empty() ? timeline_windowed_exe_
                               : timeline_game_.capturing    ? timeline_game_.active_exe
                                                              : game_capture_.capturing ? game_capture_.active_exe : std::string();
  timeline.set("game", Json(host_.timeline_state() == TimelineState::Writing ? recorded : std::string()));
  // Retained for older clients; Timeline selections now live in the preview, not in the Record key.
  timeline.set("clip_open", Json(false));
  timeline.set("clip_seconds", Json(0));
  // A session still owns its output while paused for a rebuild or the disk.
  timeline.set("writer", Json(host_.timeline_writer_active()));
  return timeline;
}

Json Protocol::status_json() {
  const EngineStats stats = host_.stats();
  Json json = Json::object();
  json.set("buffering", Json(stats.buffer_active));
  json.set("buffer_state", Json(stats.buffer_state));
  json.set("asleep", Json(stats.asleep));
  json.set("recording", Json(stats.recording));
  json.set("recording_pending", Json(stats.recording_pending));
  json.set("recording_seconds", Json(stats.recording_seconds));
  // The Timeline's setting, writer state and legacy clip fields; `timeline.changed` carries
  // the same object.
  json.set("timeline", timeline_status_json());
  // The automatic update's state: `{state, percent, version, error}`. The panel reads it from here, so a
  // download that finishes with no window open is ready the moment one opens.
  json.set("update", update_json());
  json.set("capture_ready", Json(stats.capture_ready));
  Json capture = Json::object();
  capture.set("mode", Json(stats.capture_mode));
  capture.set("target", Json(stats.capture_target));
  capture.set("width", Json(stats.width));
  capture.set("height", Json(stats.height));
  capture.set("fps", Json(stats.fps));
  capture.set("refresh_hz", Json(stats.display_hz));
  json.set("capture", capture);
  Json samples = Json::object();
  samples.set("capture_fps", Json(stats.capture_fps));
  samples.set("encoder_fps", Json(stats.encoder_fps));
  samples.set("total_frames", Json(stats.total_frames));
  samples.set("total_bytes", Json(static_cast<int64_t>(stats.total_bytes)));
  samples.set("dropped", Json(stats.dropped_frames));
  // libobs's own video-thread and encoder counters: the output's own totals can read clean while the
  // video thread is late, so both views are on the wire.
  samples.set("video_frames", Json(static_cast<int64_t>(stats.video_frames)));
  samples.set("video_lagged", Json(static_cast<int64_t>(stats.video_lagged)));
  samples.set("encoder_frames", Json(static_cast<int64_t>(stats.encoder_frames)));
  samples.set("render_time_ms", Json(stats.render_time_ms));
  samples.set("active_fps", Json(stats.active_fps));
  samples.set("encoder_behind", Json(stats.encoder_behind));
  samples.set("encoder_ratio", Json(stats.encoder_ratio));
  samples.set("process_mb", Json(std::round(stats.process_mb * 10.0) / 10.0));
  samples.set("restarts", Json(stats.restarts));
  samples.set("saved_clips", Json(stats.saved_clips));
  Json ring = Json::object();
  ring.set("seconds", Json(stats.ring_seconds));
  ring.set("measured", Json(stats.ring_measured));
  ring.set("used_bytes", Json(static_cast<int64_t>(stats.ring_used_bytes)));
  ring.set("held_seconds", Json(stats.ring_held_seconds));
  ring.set("avg_kbps", Json(stats.ring_avg_kbps));
  // The two tiers: footage in RAM, footage in the reserved file (`disk` is off / on / unavailable), bytes written to it since the buffer started,
  // and whether the disk could not take what was due (the footage then waits in RAM).
  ring.set("ram_bytes", Json(static_cast<int64_t>(stats.ring_ram_bytes)));
  ring.set("disk_bytes", Json(static_cast<int64_t>(stats.ring_disk_bytes)));
  ring.set("spilled_bytes", Json(static_cast<int64_t>(stats.ring_spilled_bytes)));
  ring.set("received_bytes", Json(static_cast<int64_t>(stats.ring_received_bytes)));
  // The Timeline dedupe: footage held by chunk files instead of RAM or the ring file, bytes that left RAM that way, and the chunks matched / waiting.
  ring.set("chunk_bytes", Json(static_cast<int64_t>(stats.ring_chunk_bytes)));
  ring.set("deduped_bytes", Json(static_cast<int64_t>(stats.ring_deduped_bytes)));
  ring.set("chunks_linked", Json(stats.ring_chunks_linked));
  ring.set("chunks_waiting", Json(stats.ring_chunks_waiting));
  ring.set("disk", Json(stats.ring_disk_state == 1 ? "on" : (stats.ring_disk_state == 2 ? "unavailable" : "off")));
  ring.set("stalled", Json(stats.ring_stalled));
  ring.set("filled", Json(stats.ring_filled));
  ring.set("bitrate_kbps", Json(stats.bitrate_kbps));
  samples.set("ring", ring);
  Json levels = Json::object();
  levels.set("desktop", Json(stats.level_desktop));
  levels.set("mic", Json(stats.level_mic));
  levels.set("active", Json(stats.audio_meters_active));
  samples.set("levels", levels);
  json.set("stats", samples);
  json.set("encoder", Json(stats.encoder_id));
  Json performance = Json::object();
  performance.set("active", Json(host_.performance_active()));
  performance.set("reason", Json(std::string(host_.performance_reason())));
  {
    const PerformancePlan plan = host_.performance_plan_now();
    performance.set("height", Json(plan.height));
    performance.set("width", Json(plan.width));
    performance.set("fps", Json(plan.fps));
    performance.set("peak_kbps", Json(plan.peak_kbps));
  }
  json.set("performance", performance);
  json.set("last_error", Json(stats.last_error));
  Json game = Json::object();
  // What the focus detector sees right now: the panel shows a "detected" line from this. Nothing is
  // reported unless it is a game candidate: a browser or terminal in the foreground is not a detection.
  const bool detected = game_capture_.capturing ||
                        (!observed_exe_.empty() && !game_process_excluded(observed_exe_, config_.game_exclusions));
  game.set("detected_exe", Json(detected ? (game_capture_.capturing ? game_capture_.active_exe : observed_exe_) : ""));
  game.set("detected_title", Json(detected ? (game_capture_.capturing ? game_capture_.active_title : observed_title_) : ""));
  game.set("capturing_game", Json(game_capture_.capturing));
  game.set("auto_capture", Json(config_.auto_game_capture));
  json.set("game", game);
  // The exclusion list (about 110 fixed names) is not here: it is read once by `game.exclusions` when the page that shows it
  // opens, instead of riding on a status that is built twice a second.
  // True while the pipeline is being torn down and rebuilt (a resolution, codec or encoder change).
  // The buffer is empty for that moment by construction, and the interface says so rather than
  // showing a ring that is about to be discarded.
  json.set("rebuilding", Json(host_.rebuilding()));
  // Whether this process is elevated, not whether the user asked to be: the setting says what will
  // happen at the next launch, and this says what is true now. An anti-cheat title or a game the
  // installer started elevated will only capture if this is true.
  json.set("is_elevated", Json(process_is_elevated()));
  return json;
}

Json Protocol::apply_patch(const Json& patch, std::string& error) {
  const Config before = config_;
  config_from_json(patch, config_);
  // Partial patches that do not touch the quality choice must not turn a Custom preset back into its quality level.
  if (!patch.has("video_preset") && !patch.has("quality") && !patch.has("quality_tier") && !patch.has("peak_mode"))
    config_.video_preset = before.video_preset;
  if (patch.has("audio")) config_apply_audio(patch["audio"], config_);
  apply_path_defaults(config_, data_root_);
  // The monitor is saved by its Windows device name, because the index in EnumDisplayMonitors order
  // moves when a display is unplugged, replugged or rearranged: a stale index can point at another
  // screen without anyone noticing. The name is what the capture source matches on.
  // The index is resolved only while capturing a display, but a changed index must clear the old name
  // in every mode: pick_display prefers the name, so a stale one sent a later display switch to the
  // monitor that used to hold that index.
  const bool display_id_changed = patch.has("display_id") && config_.display_id != before.display_id;
  if (display_id_changed) config_.display_name.clear();
  if (config_.capture_mode == "display") {
    config_.display_name = resolved_display_name(host_.displays(), config_.display_id, config_.display_name);
  }
  // A custom rate was chosen for the picture it gave in the codec in use. Moving from H.264 to AV1 at
  // the same 60 Mbps would spend twice what AV1 needs for that picture, so the rate is carried across
  // by the codecs' bits per pixel. A preset needs nothing: its rate follows the codec by itself.
  if (before.codec != config_.codec && config_.peak_mode == "fixed" && config_.custom_kbps > 0 &&
      !patch.has("custom_kbps")) {
    const int carried = rescale_for_codec(config_.custom_kbps, before.codec, config_.codec);
    BX_LOG_INFO("codec changed %s -> %s: the custom rate goes from %d to %d kbps", before.codec.c_str(),
                config_.codec.c_str(), config_.custom_kbps, carried);
    config_.custom_kbps = carried;
  }
  // Capture target changes are deliberately absent: the host swaps the source under a running ring
  // (ObsHost::apply), and asking for an output restart here is what used to empty the buffer on every
  // capture switch, game detection included.
  // What the governor settled on belongs to the settings that made the encoder slow. Change them (a lighter
  // preset, a lower frame rate, another codec) and it starts again from what was chosen. One table also
  // decides whether the pipeline is rebuilt; the field lists used to be written twice and drifted.
  const unsigned changed = config_change_bits(before, config_);
  const bool speed_inputs_changed = (changed & kConfigSpeedInput) != 0;
  const bool restart = (changed & kConfigRebuilds) != 0;
  if (speed_inputs_changed && !patch.has("encoder_speed_step")) config_.encoder_speed_step = 0;
  if (bx::config_change_waits_for_recording(restart, before.performance_when != config_.performance_when,
                                            host_.recording_busy())) {
    config_ = before;
    error = "Stop the recording first: these settings rebuild the encoder.";
    return Json::object();
  }
  if (!host_.apply(config_, restart, error)) {
    // A refused patch changed nothing: the values it wrote are not in force, and leaving them in the
    // config would show a setting the engine never took (and write it out with the next successful patch).
    config_ = before;
    return Json::object();
  }
  // A step this machine settled on is remembered between launches, so the patch that puts it back to full
  // speed must also drop the record for these settings: otherwise the next launch reads it and steps down
  // again, and "Start from full speed again" looks like it did nothing. Done after apply, so a patch the
  // running recording refused leaves the memory alone.
  if (config_.encoder_speed_step == 0 && (speed_inputs_changed || patch.has("encoder_speed_step"))) {
    bx::capacity_forget_key(
        data_root_ + "\\capacity.json",
        bx::capacity_key(config_.codec, config_.encoder, config_.output_width, config_.output_height,
                         config_.capture_fps, config_.video_preset));
  }
  follow_library_paths(before);
  if (links_ && (before.share_link_page != config_.share_link_page || before.share_sender_name != config_.share_sender_name ||
                 before.share_upload_kbps != config_.share_upload_kbps)) {
    links_->configure(config_.share_link_page, config_.share_sender_name, config_.share_upload_kbps);
  }
  if (before.launch_on_startup != config_.launch_on_startup &&
      !set_launch_on_startup(config_.launch_on_startup, executable_path())) {
    BX_LOG_WARN("the start-with-Windows entry could not be changed");
  }
  if (!persist(error)) BX_LOG_WARN("settings could not be written: %s", error.c_str());
  // Every configuration change comes through here, whoever asked for it: the panel, a hotkey, a
  // profile switch, another client on the pipe. The interface is told, because a settings page that
  // shows the old values after a profile switch is how "changing the preset does nothing" happens.
  if (broadcaster) broadcaster(make_event("config.changed", Json::object()));
  return config_json();
}

void Protocol::note_governor_step(int step) {
  config_.encoder_speed_step = step;
  std::string error;
  if (!persist(error)) BX_LOG_WARN("settings could not be written: %s", error.c_str());
  if (broadcaster) broadcaster(make_event("config.changed", Json::object()));
}

void Protocol::check_capture_target() {
  // Only a pinned target can go away: "any fullscreen game" has no window to close, and the display
  // is always there.
  const bool pinned = config_.capture_mode == "window" ||
                      (config_.capture_mode == "game" && !config_.game_title.empty());
  if (!pinned) return;
  const std::string& target = config_.capture_mode == "game" ? config_.game_title : config_.window_id;
  if (capture_target_alive(target, config_.capture_mode == "game")) return;
  // The same fallback the launch path uses (validate_capture_target): the dead id is kept, so the
  // user can see what it was, and the capture goes back to the display so the buffer keeps working.
  Json patch = Json::object();
  patch.set("capture_mode", Json("display"));
  std::string error;
  apply_patch(patch, error);
  if (!error.empty()) {
    BX_LOG_WARN("the capture target closed and the fallback failed: %s", error.c_str());
    return;
  }
  BX_LOG_INFO("the capture target closed: capturing the display instead");
  if (notifier) {
    notifier(config_.capture_mode == "game" ? "The game closed" : "The window closed",
             "Capturing the display until you pick another");
  }
}

void Protocol::poll_game(uint32_t now_ms) {
  // The target we were told to capture: the game closed, the app quit. The engine has to know, or it
  // keeps claiming to record a window that is not there any more.
  if (now_ms - last_target_check_ >= 2000) {
    last_target_check_ = now_ms;
    check_capture_target();
  }
  // Detection switched off while a game was being captured: hand capture back to what it was.
  if (!config_.auto_game_capture && game_capture_.capturing) {
    // Timeline still needs the same running game after automatic capture is switched off, including
    // when the game is behind another window and would not be rediscovered as the foreground.
    if (config_.timeline_minutes > 0 && host_.buffer_wanted()) {
      timeline_game_ = game_capture_;
      timeline_game_hwnd_ = active_game_hwnd_;
      timeline_alive_cached_ = true;
      timeline_alive_checked_ms_ = now_ms;
    }
    game_capture_ = GameCaptureState{};
    if (config_.capture_mode == "game") {
      Json patch = Json::object();
      const std::string mode = before_capture_mode_.empty() || before_capture_mode_ == "game" ? "display" : before_capture_mode_;
      patch.set("capture_mode", Json(mode));
      std::string error;
      apply_patch(patch, error);
      if (!error.empty()) BX_LOG_WARN("capture not restored after detection was turned off: %s", error.c_str());
    }
    // Detection went away while a listed game was captured: release the game layer before the early return.
    update_performance_for_game();
  }
  if (!config_.auto_game_capture) return;
  // Game capture looks at the foreground every tick: a game is on the buffer half a second after it fills
  // the screen, not after two polls five seconds apart.
  poll_game_capture(now_ms);
  update_performance_for_game();
}

// The Timeline state's tick: game presence is independent of the selected capture source.
void Protocol::poll_timeline(uint32_t now_ms) {
  TimelineFacts facts;
  facts.enabled = config_.timeline_minutes > 0;
  if (facts.enabled) {
    facts.buffer_on = host_.buffer_wanted();
    if (facts.buffer_on && config_.auto_game_capture) {
      facts.game_running = game_capture_.capturing && active_game_alive(now_ms);
      if (facts.game_running) {
        facts.game_exe = game_capture_.active_exe;
        timeline_game_ = game_capture_;
        timeline_game_hwnd_ = active_game_hwnd_;
        timeline_alive_cached_ = true;
        timeline_alive_checked_ms_ = now_ms;
      } else if (timeline_game_.capturing) {
        if (!window_still_exists(timeline_game_hwnd_) &&
            (timeline_alive_checked_ms_ == 0 || now_ms - timeline_alive_checked_ms_ >= 1000)) {
          timeline_alive_checked_ms_ = now_ms;
          timeline_alive_cached_ = game_window_alive(timeline_game_.active_exe);
        }
        facts.game_running = window_still_exists(timeline_game_hwnd_) || timeline_alive_cached_;
        if (facts.game_running) facts.game_exe = timeline_game_.active_exe;
        else timeline_game_ = GameCaptureState{};
      }
    } else if (facts.buffer_on) {
      // The capture detector normally runs only with auto capture enabled. Observe with its same
      // settle and exclusion rules here, but do not apply its enter/leave capture decisions.
      ForegroundWindow sample;
      const bool have = foreground_window(sample);
      if (have) foreground_ = sample;
      if (timeline_game_.capturing && !window_still_exists(timeline_game_hwnd_) &&
          (timeline_alive_checked_ms_ == 0 || now_ms - timeline_alive_checked_ms_ >= 1000)) {
        timeline_alive_checked_ms_ = now_ms;
        timeline_alive_cached_ = game_fullscreen_alive(timeline_game_.active_exe);
      }
      const bool alive = !timeline_game_.capturing || window_still_exists(timeline_game_hwnd_) || timeline_alive_cached_;
      const bool excluded = !have || game_process_excluded(sample.exe, config_.game_exclusions);
      const GameCaptureDecision decision = game_capture_observe(timeline_game_, now_ms,
          have ? sample.exe : std::string(), have ? sample.title : std::string(),
          have && sample.fullscreen, excluded, alive);
      if (decision == GameCaptureDecision::enter) {
        timeline_game_hwnd_ = sample.hwnd;
        timeline_alive_cached_ = true;
        timeline_alive_checked_ms_ = now_ms;
      } else if (decision == GameCaptureDecision::leave) {
        timeline_game_hwnd_ = 0;
        timeline_alive_cached_ = false;
      }
      // The buffer can start while an already-running game is behind another window. Use the same
      // fullscreen list as automatic capture, at a bounded rate, without changing the capture target.
      if (!timeline_game_.capturing && timeline_game_.candidate_exe.empty() &&
          (timeline_scan_ms_ == 0 || now_ms - timeline_scan_ms_ >= 5000)) {
        timeline_scan_ms_ = now_ms;
        for (const WindowInfo& window : enumerate_game_windows()) {
          if (game_process_excluded(window.exe, config_.game_exclusions)) continue;
          timeline_game_.capturing = true;
          timeline_game_.active_exe = window.exe;
          timeline_game_.active_title = window.label;
          // Followed by its window: the program having some other window (a launcher, a tray icon) is not the game running.
          timeline_game_hwnd_ = window.hwnd;
          timeline_alive_cached_ = true;
          timeline_alive_checked_ms_ = now_ms;
          break;
        }
      }
      // "Not a game" in Settings: what was adopted is let go at once.
      if (timeline_game_.capturing && game_process_excluded(timeline_game_.active_exe, config_.game_exclusions)) {
        timeline_game_ = GameCaptureState{};
        timeline_game_hwnd_ = 0;
        timeline_alive_cached_ = false;
      }
      facts.game_running = timeline_game_.capturing;
      if (facts.game_running) facts.game_exe = timeline_game_.active_exe;
      // A manually chosen game need not be fullscreen. Its window must still exist; a stale configured
      // target alone is not evidence that a game is running.
      std::string pinned;
      if (config_.capture_mode == "game") {
        const size_t colon = config_.game_title.find_last_of(':');
        if (colon != std::string::npos) pinned = decode_window_part(config_.game_title.substr(colon + 1));
      }
      if (pinned != timeline_pinned_exe_ || timeline_pinned_checked_ms_ == 0 || now_ms - timeline_pinned_checked_ms_ >= 1000) {
        timeline_pinned_exe_ = pinned;
        timeline_pinned_checked_ms_ = now_ms;
        timeline_pinned_alive_ = !pinned.empty() && !game_process_excluded(pinned, config_.game_exclusions) && game_window_alive(pinned);
      }
      if (!facts.game_running && timeline_pinned_alive_) {
        facts.game_running = true;
        facts.game_exe = pinned;
      }
      if (config_.capture_mode == "game" && facts.game_running) {
        const bool in_front = (have && normalized_exe_name(sample.exe) == normalized_exe_name(facts.game_exe)) ||
                              timeline_preview_is_foreground();
        if (follow_display_ == in_front) {
          std::string follow_error;
          if (host_.set_capture_follow(!in_front, true, follow_error)) follow_display_ = !in_front;
          else BX_LOG_WARN("capture could not follow focus: %s", follow_error.c_str());
        }
      } else if (follow_display_) {
        restore_follow_capture("no game is running", true);
      }
    }
    if (facts.buffer_on) {
      const std::string windowed = poll_windowed_game(now_ms, facts.game_running);
      if (!windowed.empty()) {
        facts.game_running = true;
        facts.game_exe = windowed;
      }
    }
    // Existing writer probes use a synthetic desktop or test window rather than a game.
    static const int test_timeline_game = [] {
      const char* value = std::getenv("BUFFRX_TEST_TIMELINE_GAME");
      if (value && std::string(value) == "1") return 1;
      if (value && std::string(value) == "0") return 0;
      return -1;
    }();
    if (facts.buffer_on && test_timeline_game >= 0) facts.game_running = test_timeline_game == 1;
    // A bounded simulated game lets a probe exercise the game-exit save without closing a real game.
    static const uint32_t test_game_ms = [] {
      const char* value = std::getenv("BUFFRX_TEST_TIMELINE_GAME_MS");
      return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    static const uint32_t test_game_start = now_ms;
    if (facts.buffer_on && test_game_ms) facts.game_running = now_ms - test_game_start < test_game_ms;
    // Everywhere: the desktop is footage when no game runs. Only desktop footage can be idle: a controller is not input to
    // Windows, so a game being played with a pad must never be paused for it.
    facts.everywhere = config_.timeline_everywhere;
    if (facts.everywhere && !facts.game_running && config_.timeline_idle_minutes > 0) {
      LASTINPUTINFO last_input{};
      last_input.cbSize = sizeof(last_input);
      if (GetLastInputInfo(&last_input)) facts.idle = timeline_idle_due(last_input.dwTime, now_ms, config_.timeline_idle_minutes);
    }
    // The shared-encoder manual output runs beside Timeline. Only the separate-encoder fallback owns this slot.
    facts.manual_owns_output = host_.recording_blocks_timeline();
    facts.rebuilding = host_.rebuilding();
    // The disk guard is sampled once a minute: below the floor the writer stops, and it starts again
    // only a gigabyte above it, so a disk sitting on the line does not flap.
    if (low_disk_checked_ms_ == 0 || now_ms - low_disk_checked_ms_ >= 60000) {
      low_disk_checked_ms_ = now_ms;
      const std::string directory = recordings_base_dir(config_);
      const int64_t free_bytes = volume_free_bytes(directory);
      std::string reason;
      if (!recording_space_ok(directory, free_bytes, config_.min_free_gb, reason)) {
        if (!low_disk_) BX_LOG_WARN("timeline paused: %s", reason.c_str());
        low_disk_ = true;
      } else if (free_bytes > 0) {
        const int64_t resume_at =
            static_cast<int64_t>(std::max(0, config_.min_free_gb)) * 1024 * 1024 * 1024 + (1024LL * 1024 * 1024);
        if (free_bytes >= resume_at) {
          if (low_disk_) BX_LOG_INFO("timeline resumed: %.1f GB free", free_bytes / 1073741824.0);
          low_disk_ = false;
        }
      }
    }
    facts.low_disk = low_disk_;
    // Test knob (N1): BUFFRX_TEST_TIMELINE_PAUSE_MS drops the writer and brings it back on a timer, so a probe can
    // count what one pause/resume costs in handles. Read once; a test only, never a setting.
    static const int64_t test_pause_ms = [] {
      const char* text = std::getenv("BUFFRX_TEST_TIMELINE_PAUSE_MS");
      return text ? std::atoll(text) : int64_t{0};
    }();
    if (timeline_test_pause_due(now_ms, test_pause_ms)) facts.rebuilding = true;
    // Retention sweeps once a minute, whatever the session state: a Timeline left writing for hours still
    // lets its old footage go. The pins a save or a preview holds are handed to the host with the sweep.
    const int64_t now_wall_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
    if (timeline_prune_due(timeline_prune_last_ms_, now_wall_ms)) {
      timeline_prune_last_ms_ = now_wall_ms;
      host_.prune_timeline_now(now_wall_ms);
    }
  } else {
    // Timeline off: footage it left behind expires on the keep-hours clock, at start and then hourly, not only
    // at the next recording boundary (which may never come).
    const int64_t now_wall_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
    if (timeline_prune_last_ms_ == 0 || now_wall_ms - timeline_prune_last_ms_ >= 3600000) {
      timeline_prune_last_ms_ = now_wall_ms;
      host_.prune_idle_now();
    }
  }
  if (!facts.enabled || !facts.buffer_on) {
    timeline_game_ = GameCaptureState{};
    timeline_game_hwnd_ = 0;
    timeline_alive_cached_ = false;
    timeline_alive_checked_ms_ = 0;
    timeline_scan_ms_ = 0;
    timeline_pinned_exe_.clear();
    timeline_pinned_checked_ms_ = 0;
    timeline_pinned_alive_ = false;
    timeline_windowed_hwnd_ = 0;
    timeline_windowed_exe_.clear();
  }
  host_.poll_timeline(now_ms, facts);
  // `timeline.changed` marks a state change - waiting, writing, paused, off - never one event per chunk:
  // the preview polls `timeline.get` for new footage, which costs a bounded disk read and wakes nobody.
  const TimelineState state = host_.timeline_state();
  const bool enabled = config_.timeline_minutes > 0;
  if (state != last_timeline_state_ || enabled != last_timeline_enabled_) {
    last_timeline_state_ = state;
    last_timeline_enabled_ = enabled;
    if (broadcaster) broadcaster(make_event("timeline.changed", timeline_status_json()));
  }
}

TimelineState Protocol::timeline_state() const { return host_.timeline_state(); }

// The preview's process id is 0 until Task 10 launches it, so nothing is exempt yet. Compared by process,
// not by window title: a title is the game's or the preview's to change.
bool Protocol::timeline_preview_is_foreground() const {
  if (timeline_preview_pid_ == 0 || foreground_.hwnd == 0) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(reinterpret_cast<HWND>(foreground_.hwnd), &pid);
  return pid == timeline_preview_pid_;
}

namespace {

// Names of clips being saved right now. reserve_output_path only sees files on disk, and a save's clip appears
// there at its very end, so two saves running together would pick one final name and the second would find it
// taken: the claim keeps this process's own saves off each other's names.
std::mutex g_save_names_mutex;
std::set<std::string> g_save_names;

struct SaveNameClaim {
  std::vector<std::string> paths;
  ~SaveNameClaim() {
    if (paths.empty()) return;
    std::lock_guard<std::mutex> lock(g_save_names_mutex);
    for (const std::string& path : paths) g_save_names.erase(path);
  }
};

// The first name free on disk that no save in flight holds. The claim ends with the save. A save that has to
// retry after a name was taken in the meantime claims again, and every name it ever held is released together.
// Empty when no name could be held: a save that ran out of names has no name to publish under, and it must not
// hand one back that another save in this process owns.
std::string claim_save_path(const std::string& directory, const std::string& stem, const std::string& extension,
                            SaveNameClaim& claim) {
  std::lock_guard<std::mutex> lock(g_save_names_mutex);
  return bx::claim_save_name(
      stem, [&](const std::string& name) { return reserve_output_path(directory, name, extension); },
      [&](const std::string& path) {
        // Only a name this save really inserted is its own to release: the old loop returned the last refused name
        // once it ran out of attempts, and this claim's destructor then erased another save's hold on it.
        if (!g_save_names.insert(path).second) return false;
        claim.paths.push_back(path);
        return true;
      });
}

// The number in a staging name: this process, plus a counter so two saves of this process staging one after
// another never ask for one name. The name itself is still reserved on disk (reserve_output_path below).
std::atomic<unsigned long long> g_staging_seq{0};

// A file created by this save or by nobody: CreateFileA with CREATE_NEW fails when anything holds the name, so the
// creation itself is the claim. Asking reserve_output_path whether a name exists is not one - another writer could
// create it before the join truncated it - and a cleanup that only knew a path could then delete that writer's file.
bool create_new_file(const std::string& path) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  return true;
}

// The files one staged copy owns: the copy itself, then - for a copy a join child writes - its two sidecars, named by
// media.h so the child and this save cannot disagree about them. The library drops every name holding ".part", so a
// staged copy and its sidecars are never listed.
std::vector<std::string> staging_file_set(const std::string& copy, bool join_sidecars) {
  std::vector<std::string> files{copy};
  if (join_sidecars) {
    files.push_back(media_join_result_path(copy));
    files.push_back(media_join_spec_path(copy));
  }
  return files;
}

// A staging name this save owns before a child writes it and its cleanup deletes it. Empty when no candidate could be
// claimed: the caller then writes nothing, instead of writing over or deleting a file another writer made.
std::string reserve_staging_path(const std::string& directory, const std::string& stem, const std::string& extension,
                                 bool join_sidecars) {
  return bx::claim_staging_path(
      [&] {
        const std::string copy =
            reserve_output_path(directory, save_staging_stem(stem, GetCurrentProcessId(), ++g_staging_seq), extension);
        return staging_file_set(copy, join_sidecars);
      },
      create_new_file, [](const std::string& path) { DeleteFileA(path.c_str()); });
}

}  // namespace

// A range of the Timeline becomes a clip in the library. The newest footage is not readable while its
// segment is open, so a live range that reaches past the published edge asks the host to close the segment
// now and waits for the index to publish it - with a deadline, and then an honest refusal. The range is
// never quietly shortened, and every source chunk is held for as long as the copy reads it (a read guard,
// not just a lease), so a retention sweep or an erase cannot take a file out from under the decoder.
bool Protocol::timeline_save(const Json& args, Json& out, std::string& error) {
  const std::string asked = args.has("session") ? args["session"].as_string() : "";
  const int64_t start_ms = args["start_ms"].as_int(-1);
  const int64_t end_ms = args["end_ms"].as_int(-1);
  if (start_ms < 0 || end_ms <= start_ms) {
    error = "the range is empty";
    return false;
  }
  std::string session_id = asked;
  if (session_id.empty()) {
    std::string list_error;
    for (const SessionEntry& candidate : session_store_load(list_error)) {
      if (candidate.kind != "timeline") continue;
      session_id = candidate.id;
      break;
    }
  }
  if (session_id.empty()) {
    error = "there is no timeline footage to save";
    return false;
  }
  SessionEntry entry;
  // The number this save's lease is checked against, captured before the index is read: a removal that begins or ends
  // between the read and the lease refuses the lease, instead of leaving this save holding footage that is already gone
  // (timeline_retention.h). The wait for a live segment below reads the index again and takes a fresh number with it.
  uint64_t removal_seen = timeline_removal_seen(session_id);
  if (!load_timeline_entry(session_id, entry, error, "is saved this way")) return false;
  // The range arrives on the session's own clock, the numbers `timeline.get` reports; the index keeps
  // absolute tick times, which wrap every 49.7 days. The chunks are put on the session's clock instead
  // (timeline_on_session_clock), so no sum with a tick can pass 32 bits and every comparison is on one clock.
  // Where the playable footage ends: the newest published chunk whose writer has let go of its file and
  // whose file is still there. A chunk that was pruned, deleted or moved after it was indexed is not
  // footage - planning from it named a file the copy could not open halfway through the range - so it is
  // dropped here, once, and the edge and the plan both read what is left. `timeline_save_view` is the rule.
  SessionEntry usable;
  int64_t available_end = 0;
  const auto on_disk = [](const std::string& file) {
    return GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES;
  };
  const auto refresh = [&](const SessionEntry& session) {
    available_end = timeline_save_view(timeline_on_session_clock(session), usable, DirListing());
    if (usable.chunks.size() != session.chunks.size()) {
      BX_LOG_WARN("%d indexed chunk(s) of %s are no longer on disk and are not saved from",
                  static_cast<int>(session.chunks.size() - usable.chunks.size()), session.id.c_str());
    }
  };
  refresh(entry);
  const bool live = host_.timeline_writer_active() && host_.current_session_id() == entry.id;
  if (live && end_ms > available_end) {
    // A refused rotation (the output could not split) still gets the splitter's own cut, at most one segment away.
    const bool rotation_asked = host_.request_timeline_rotation();
    const uint32_t deadline = GetTickCount() + (rotation_asked ? 10000u : 9000u);
    while (static_cast<int32_t>(GetTickCount() - deadline) < 0) {
      Sleep(100);
      SessionEntry reloaded;
      std::string reload_error;
      const uint64_t read_at = timeline_removal_seen(session_id);
      if (!session_store_load_one(session_id, reloaded, reload_error)) break;
      removal_seen = read_at;
      entry = reloaded;
      refresh(entry);
      if (end_ms <= available_end) break;
      // The writer stopped (the game left, Timeline was turned off, the disk guard tripped): no segment
      // will close on its own, and the wait would only burn the deadline.
      if (!host_.timeline_writer_active()) break;
    }
  }
  const TimelineSavePlan plan = timeline_save_plan(usable, start_ms, end_ms, available_end, error);
  if (plan.files.empty()) {
    BX_LOG_INFO("timeline save of %s [%lld, %lld) refused: %s", session_id.c_str(),
                static_cast<long long>(start_ms), static_cast<long long>(end_ms), error.c_str());
    return false;
  }
  // The files this copy reads are held until it is done. The lease below proves the index this save planned from is
  // current, and the read guard (timeline_retention.h) is what protects the copy itself: the lease expires after
  // fifteen minutes, but the join may run for thirty (media.h), so a sweep or an erase could otherwise take a source
  // chunk out from under the decoder. The guard has no clock - it stands exactly as long as this save does, and every
  // way out of the function drops it (its destructor), so an erase is delayed, never blocked for good.
  const int64_t now_wall_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
  TimelinePin pin;
  pin.owner = kTimelineSaveOwner;
  pin.session_id = session_id;
  bool have_span = false;
  for (const SessionChunk& chunk : entry.chunks) {
    if (std::find(plan.files.begin(), plan.files.end(), chunk.file) == plan.files.end()) continue;
    if (!have_span) {
      pin.start_ms = chunk.wall_start_ms;
      pin.end_ms = chunk.wall_end_ms;
      have_span = true;
    } else {
      pin.start_ms = std::min(pin.start_ms, chunk.wall_start_ms);
      pin.end_ms = std::max(pin.end_ms, chunk.wall_end_ms);
    }
  }
  if (!have_span) {
    // An index too old to carry wall times: the session's own start is the wall clock the chunk offsets
    // are measured from.
    const int64_t base = entry.started_at * 1000;
    pin.start_ms = base + start_ms;
    pin.end_ms = base + end_ms;
  }
  pin.expires_ms = now_wall_ms + 15 * 60000;
  std::string lease_id;
  // Covers the whole session for the copy's real lifetime. Taken before the lease, under the one gate: a removal that
  // already owns any part of this session is refused here, and once it is in force no new removal can start, so the
  // lease below is decided against a session that cannot change under it.
  TimelineReadGuard read_guard;
  {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    // The lease is granted in one step under the retention gate, checked against the number this save read its index
    // under: a sweep that reserved any of these files refuses it, so a save never reads footage that is going
    // (timeline_retention.h). Each save owns its lease, so two saves of one session cannot release each other's.
    read_guard = TimelineReadGuard::take(session_id);
    pin.owner = std::string(kTimelineSaveOwner) + " " + std::to_string(++timeline_lease_seq_);
    pin.id = "save " + session_id + " " + std::to_string(now_wall_ms) + " " + std::to_string(timeline_lease_seq_);
    lease_id = read_guard.active() ? host_.take_timeline_lease(pin, now_wall_ms, removal_seen) : std::string();
    if (lease_id.empty()) {
      error = "the timeline is removing that footage; try again in a moment";
      BX_LOG_WARN("timeline save of %s refused: %s", session_id.c_str(), error.c_str());
      return false;
    }
  }
  struct PinRelease {
    Protocol& protocol;
    std::string id;
    ~PinRelease() {
      std::lock_guard<std::mutex> lock(protocol.engine_mutex_);
      protocol.host_.release_timeline_lease(id, static_cast<int64_t>(std::time(nullptr)) * 1000);
    }
  } release{*this, lease_id};

  // The pin is in force: every file the plan copies is read once more here. A chunk that was pruned or
  // moved between the listing and this moment fails the save with its own name, before an output file
  // exists, so a bad save leaves nothing half-written in the library - and the pin keeps the sweep from
  // taking one during the copy.
  for (const std::string& file : plan.files) {
    if (on_disk(file)) continue;
    error = "a timeline chunk is no longer on disk: " + file;
    BX_LOG_WARN("timeline save of %s refused: %s", session_id.c_str(), error.c_str());
    return false;
  }

  std::string clips_dir, container;
  {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    clips_dir = config_.clips_dir;
    container = config_.container;
  }
  const std::string stem = args.has("name") && !args["name"].as_string().empty()
                               ? args["name"].as_string()
                               : "BuffrX " + session_id.substr(session_id.find(' ') + 1);
  // The join writes beside the clip under a name the library never lists (`list_directory` drops any
  // name holding `.part`), and the clip takes its own name only once the whole file has verified: a
  // half-made copy is never visible, and a failed save never reaches the name the library would show.
  // The staging name keeps the container's extension, because the muxer is chosen from the path, and it is
  // this save's own, claimed by creating it: a part an earlier crash or another program left is neither
  // opened over nor deleted, and neither is the join's spec or result beside it.
  const std::string staged = reserve_staging_path(clips_dir, stem, "." + container, true);
  if (staged.empty()) {
    error = "the saved clip could not be staged beside the clips";
    BX_LOG_WARN("timeline save of %s failed: %s", session_id.c_str(), error.c_str());
    return false;
  }
  // A change of stream layout inside the range (the plan splits on a rebuild of the encoders, the copy checks the streams
  // themselves) keeps the part that holds the end of the range: stream-copied, nothing re-encoded. The copy runs in a
  // child process, so the engine's media lock is never taken here: listings and thumbnails answer while a save joins.
  MediaJoinRequest join;
  join.files = plan.files;
  join.output = staged;
  join.start_in_first_seconds = static_cast<double>(plan.start_in_first_ms) / 1000.0;
  join.end_in_last_seconds = static_cast<double>(plan.end_in_last_ms) / 1000.0;
  MediaJoinResult joined;
  if (!media_join_process(join, joined, error)) {
    BX_LOG_WARN("timeline save of %s failed: %s", session_id.c_str(), error.c_str());
    return false;
  }
  const size_t dropped_head = joined.dropped_head;
  int64_t cut_head_ms = plan.cut_head_ms;
  if (dropped_head > 0 && dropped_head < plan.files.size()) {
    for (const SessionChunk& chunk : usable.chunks) {
      if (chunk.file == plan.files[dropped_head]) cut_head_ms = std::max<int64_t>(0, static_cast<int64_t>(chunk.start_ms) - start_ms);
    }
  }
  const ClipCheck verified = check_clip_file(staged);
  if (!verified.ok) {
    DeleteFileA(staged.c_str());
    error = "the saved clip did not verify: " + verified.reason;
    BX_LOG_WARN("%s", error.c_str());
    return false;
  }
  // Verified whole: only now does it take the name the library lists. The name is claimed at the moment of the
  // move, not before the join, because a replay save or any other writer may take a name the save would have
  // picked while a long range joins. The move never replaces, so a name taken since is refused and the copy
  // tries the next configured name: the other writer keeps its file and this save keeps every byte of its own.
  //
  // A rename within one folder is a directory entry change, so a reader sees the clip or nothing, never half of it.
  SaveNameClaim name_claim;
  const bx::SavePublishResult published = bx::publish_staged_clip(
      staged, [&] { return claim_save_path(clips_dir, stem, "." + container, name_claim); },
      [](const std::string& from, const std::string& to, unsigned long* move_error) {
        if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        if (move_error) *move_error = GetLastError();
        return false;
      });
  if (published.path.empty()) {
    // Every name was refused. The footage stays in its staging file, out of the library and named here, so a copy
    // that could not be published is never deleted to make the failure look tidy.
    error = "the saved clip could not be put in place; its footage is kept at " + staged;
    BX_LOG_WARN("timeline save of %s failed: %s (%lu)", session_id.c_str(), error.c_str(), published.error);
    return false;
  }
  const std::string path = published.path;
  BX_LOG_INFO("timeline save of %s: %s (%lld bytes, %zu chunks)", session_id.c_str(), path.c_str(),
              static_cast<long long>(verified.bytes), plan.files.size());
  if (broadcaster) broadcaster(make_event("library.changed", Json::object()));
  // The saved range is a clip marker at its start, so the strip shows what was already taken.
  std::string clip_name = path.substr(path.find_last_of("\\/") + 1);
  clip_name = clip_name.substr(0, clip_name.find_last_of('.'));
  std::string mark_error;
  if (mark_timeline(session_id, "clip", clip_name, start_ms, mark_error).empty()) {
    BX_LOG_WARN("the clip marker was not indexed: %s", mark_error.c_str());
  }
  out = Json::object();
  out.set("file", Json(path));
  out.set("bytes", Json(verified.bytes));
  out.set("chunks", Json(static_cast<int64_t>(plan.files.size() - dropped_head)));
  // The start the clip really has when a settings change cut the front off: the session clock, like the range it was asked for.
  out.set("start_ms", Json(start_ms + cut_head_ms));
  out.set("cut_head_ms", Json(cut_head_ms));
  // Pauses inside the saved range: the clip skips them, so it is this much shorter than the range asked for.
  int64_t gap_ms = 0;
  for (size_t index = dropped_head + 1; index < plan.gap_before_ms.size(); ++index) gap_ms += plan.gap_before_ms[index];
  out.set("gap_ms", Json(gap_ms));
  return true;
}

// The Record key starts or stops a standalone recording in every Timeline state.
buffrx::RecordAction Protocol::record_pressed(std::string& error) {
  const bool manual = host_.stats().recording;  // manual only: a Timeline segment is not a recording
  const buffrx::RecordAction action = buffrx::record_action(manual);
  switch (action) {
    case buffrx::RecordAction::ManualStop:
      host_.stop_recording();
      if (broadcaster) broadcaster(make_event("recording.changed", Json(false)));
      break;
    case buffrx::RecordAction::ManualStart:
      if (!host_.start_recording(error)) break;
      mark("recording.start", "start");
      if (broadcaster) broadcaster(make_event("recording.changed", Json(true)));
      break;
  }
  return action;
}

bool Protocol::active_game_alive(uint32_t now_ms) {
  if (window_still_exists(active_game_hwnd_)) return true;  // the usual answer, with no window list
  if (alive_checked_ms_ != 0 && now_ms - alive_checked_ms_ < 1000) return alive_cached_;
  alive_checked_ms_ = now_ms;
  alive_cached_ = game_window_alive(game_capture_.active_exe);
  return alive_cached_;
}

// Ends the follow swap: the host's layer is cleared, and the source goes back to the user's own target at once when a source
// is wanted. The config was never changed by the swap, so there is nothing to write back.
void Protocol::restore_follow_capture(const char* why, bool reconfigure) {
  std::string error;
  if (!host_.set_capture_follow(false, reconfigure, error)) {
    BX_LOG_WARN("capture could not be restored after the Timeline: %s", error.c_str());  // still following: asked again next tick
    return;
  }
  follow_display_ = false;
  BX_LOG_INFO("capture restored after the Timeline: %s (%s)", config_.capture_mode.c_str(), why);
}

void Protocol::poll_game_capture(uint32_t now_ms) {
  // Nothing wants frames: the foreground is only read to remember the last game (a buffer started from the panel
  // starts on it), and once a second is plenty. While the buffer runs, every tick.
  const bool wanted = host_.capture_wanted();
  if (!wanted && idle_sample_ms_ != 0 && now_ms - idle_sample_ms_ < 1000) return;
  idle_sample_ms_ = now_ms;
  ForegroundWindow sample;
  const bool have = foreground_window(sample);
  const std::string exe = have ? sample.exe : std::string();
  const bool fullscreen = have && sample.fullscreen;
  // The links' upload cap and the panel's detected line read this: nothing else polls the foreground.
  if (have) {
    observed_exe_ = sample.exe;
    observed_title_ = sample.title;
  }
  // The built-in list already carries this product line and the capture tools, so the engine's own
  // window can never be mistaken for a game.
  const bool excluded = game_process_excluded(exe, config_.game_exclusions);
  // Remembered even while nothing is captured: the buffer is usually started from the panel, which is in
  // front of the game by then, and it should start on the game, not on the display.
  if (fullscreen && !exe.empty() && !excluded) {
    last_game_id_ = sample.id;
    last_game_exe_ = exe;
    last_game_title_ = sample.title;
    last_game_seen_ms_ = GetTickCount();
  }
  // The buffer is off and nothing is being recorded: the engine does not capture the game. The game
  // is still detected and still shown, but a capture source with nobody watching costs CPU and GPU
  // for frames that are thrown away - and it holds the sound card open, which puts BuffrX in the
  // user's mixer. The capture waits for the buffer (and buffer.start picks the game it starts on).
  if (!wanted) {
    // A stopped buffer must not leave capture swapped to the display: the next start would snapshot the
    // swap as the user's own target and keep recording the desktop with the Timeline off.
    if (follow_display_) restore_follow_capture("the buffer stopped", false);
    if (game_capture_.capturing) {
      if (!active_game_alive(now_ms)) game_capture_ = GameCaptureState{};  // it closed while nothing was captured
    } else {
      game_capture_.candidate_exe.clear();
      game_capture_.candidate_title.clear();
      game_capture_.candidate_since_ms = 0;
    }
    return;
  }
  const bool was_capturing = game_capture_.capturing;
  const bool active_alive = !was_capturing || active_game_alive(now_ms);
  const GameCaptureDecision decision =
      game_capture_observe(game_capture_, now_ms, exe, sample.title, fullscreen, excluded, active_alive);
  if (decision == GameCaptureDecision::none) {
    // Nothing to detect, but the picture still has to follow the foreground while the Timeline records:
    // a game that lost focus hands capture to the display, and takes it back when it is in front again.
    // Detection state is untouched, so the return needs no new hook settle.
    const bool same_game = game_capture_.capturing && !exe.empty() &&
                           normalized_exe_name(exe) == normalized_exe_name(game_capture_.active_exe);
    // The preview is excluded from every capture, so a game behind it is still the picture its user opened
    // it to see: counting the preview as "not the game" would swap the ring to the desktop under it.
    const bool in_front =
        game_capture_.capturing && ((same_game && fullscreen) || timeline_preview_is_foreground());
    const CaptureTarget want =
        capture_follow_target(config_.timeline_minutes > 0, true, game_capture_.capturing, in_front);
    if (want == CaptureTarget::display && config_.capture_mode == "game") {
      // The layer in the host, over the config: nothing is written, so a crash mid-follow leaves the user's target as it was,
      // and a rebuild (a settings change, a governor step) keeps the swap instead of undoing it.
      std::string follow_error;
      if (host_.set_capture_follow(true, true, follow_error)) {
        follow_display_ = true;
        BX_LOG_INFO("capture follows focus: display");
      } else {
        BX_LOG_WARN("capture could not follow focus: %s", follow_error.c_str());
      }
    } else if (want == CaptureTarget::game && follow_display_) {
      // The entered game's own "title:class:exe" id is still in the config (it was never replaced), so the source returns to the
      // hook that resolves it; a bare window title never hooks and left the ring on black. Cleared only once the swap happened, so
      // a refused one is asked again next tick.
      std::string follow_error;
      if (host_.set_capture_follow(false, true, follow_error)) {
        follow_display_ = false;
        BX_LOG_INFO("capture follows focus: game");
      } else {
        BX_LOG_WARN("capture could not follow focus back: %s", follow_error.c_str());
      }
    } else if (want == CaptureTarget::leave && follow_display_) {
      restore_follow_capture("the Timeline turned off", true);
    }
    return;
  }

  if (follow_display_) {
    // A detection decides the target itself: the layer goes before the patch below swaps the source.
    follow_display_ = false;
    std::string ignored;
    host_.set_capture_follow(false, false, ignored);
  }
  std::string error;
  if (decision == GameCaptureDecision::enter) {
    // A hand-over (another game while one is captured) keeps the target that was there before the first one. The follow swap
    // never touched the config, so the snapshot is the user's own target.
    if (!was_capturing) {
      before_capture_mode_ = config_.capture_mode;
      before_capture_title_ = config_.game_title;
    }
    Json patch = Json::object();
    patch.set("capture_mode", Json("game"));
    // The full "title:class:exe" id: game capture resolves its target by class and exe, so a bare
    // title never hooks and the buffer would sit on an empty source.
    patch.set("game_title", Json(sample.id));
    const uint32_t started = GetTickCount();
    apply_patch(patch, error);
    active_game_hwnd_ = sample.hwnd;
    alive_cached_ = true;
    alive_checked_ms_ = now_ms;
    if (!error.empty()) {
      BX_LOG_WARN("game capture not applied: %s", error.c_str());
    } else {
      BX_LOG_INFO("game %s: capturing \"%s\" (%s), switched in %u ms", was_capturing ? "hand-over" : "detected",
                  sample.title.c_str(), exe.c_str(), GetTickCount() - started);
      mark("game.capture", sample.title);
      if (notifier) notifier("Capturing " + media_folder_name(sample.title), "Clips go to its own folder");
    }
  } else {
    Json patch = Json::object();
    const std::string mode = before_capture_mode_.empty() ? "display" : before_capture_mode_;
    patch.set("capture_mode", Json(mode));
    if (mode == "game") patch.set("game_title", Json(before_capture_title_));
    apply_patch(patch, error);
    active_game_hwnd_ = 0;
    if (!error.empty()) BX_LOG_WARN("capture not restored after the game: %s", error.c_str());
    else BX_LOG_INFO("game left: capture restored to %s", mode.c_str());
  }
  if (broadcaster) {
    Json data = Json::object();
    data.set("decision", Json(std::string(decision == GameCaptureDecision::enter ? "enter" : "leave")));
    data.set("exe", Json(decision == GameCaptureDecision::enter ? exe : std::string()));
    data.set("title", Json(decision == GameCaptureDecision::enter ? sample.title : std::string()));
    data.set("handover", Json(decision == GameCaptureDecision::enter && was_capturing));
    broadcaster(make_event("game.changed", data));
  }
}

namespace {

// A game window the preview took off the screen: minimized, or hidden (some exclusive games hide their
// window rather than minimize it when another takes the display).
bool window_left_screen(uintptr_t hwnd) {
  if (!window_still_exists(hwnd)) return false;
  const HWND window = reinterpret_cast<HWND>(hwnd);
  return IsIconic(window) != FALSE || IsWindowVisible(window) == FALSE;
}

// Puts a game back the way the preview found it. SW_RESTORE also activates, which is what a minimized game
// needs; SetForegroundWindow is the belt for one that only hid.
bool put_game_back(uintptr_t hwnd) {
  if (!window_left_screen(hwnd)) return false;
  const HWND window = reinterpret_cast<HWND>(hwnd);
  ShowWindow(window, SW_RESTORE);
  SetForegroundWindow(window);
  return true;
}

}  // namespace

void Protocol::ask_timeline_preview_to_close() {
  if (!timeline_process_ || timeline_close_asked_ms_ != 0) return;
  // Asked politely first: the page clears its player and exits by itself; poll_timeline_preview ends it if
  // it has not within two seconds.
  timeline_close_asked_ms_ = GetTickCount() == 0 ? 1 : GetTickCount();
  if (broadcaster) broadcaster(make_event("timeline.close", Json::object()));
}

bool Protocol::open_timeline_preview(const std::string& session, std::string& error) {
  if (timeline_process_) {
    error = "the Timeline preview is already open";
    return false;
  }
  // Let timeline.get choose the current writer after the window starts. Scanning for published
  // chunks here can select yesterday's session while today's first chunk is still being written.
  const std::string id = session;
  // No session is not a refusal: Alt+4 opens the Timeline on its empty state, which adopts the newest
  // session when one appears. Only a session id that could escape its folder is refused.
  if (!id.empty() && !session_id_is_safe(id)) {
    error = "that is not a timeline session";
    return false;
  }
  void* process = nullptr;
  DWORD pid = 0;
  // The window that is about to lose the foreground, read before the preview takes it. A game in exclusive
  // fullscreen cannot be covered, and nothing says which kind it is until it is tried (timeline_overlay.h).
  // Read here, never from `foreground_`, which is sampled only while automatic game capture is off. Every
  // opener arms this - Alt+4 and the panel's and editor's `timeline.open` alike - or a game those actions
  // pushed aside is never put back.
  ForegroundWindow in_front;
  const bool have_front = foreground_window(in_front);
  const bool game_in_front =
      have_front && in_front.fullscreen && !game_process_excluded(in_front.exe, config_.game_exclusions);
  const bool explained = game_in_front && !timeline_exclusive_exe_.empty() &&
                         normalized_exe_name(in_front.exe) == timeline_exclusive_exe_;
  if (!launch_timeline(id, process, pid)) {
    error = "the Timeline preview could not be started";
    return false;
  }
  timeline_process_ = process;
  timeline_preview_pid_ = pid;  // the writer's exemption: its window in front is not leaving the game
  timeline_close_asked_ms_ = 0;
  timeline_restore_hwnd_ = game_in_front ? in_front.hwnd : 0;
  timeline_overlay_hwnd_ = timeline_overlay_probe_wanted(game_in_front, explained) ? in_front.hwnd : 0;
  timeline_overlay_due_ms_ = timeline_overlay_hwnd_ ? GetTickCount() + kTimelineOverlayProbeMs : 0;
  if (timeline_overlay_hwnd_) {
    timeline_overlay_exe_ = in_front.exe;
    timeline_overlay_title_ = media_folder_name(in_front.title);
  }
  // A note, not an info line: "my game did not come back" is answered by these two lines, and only note and warn
  // reach the log a person reads.
  BX_LOG_NOTE("timeline preview: opened on %s (game_in_front %d, restore 0x%llx)", id.c_str(), static_cast<int>(game_in_front), static_cast<unsigned long long>(timeline_restore_hwnd_));
  return true;
}

void Protocol::timeline_pressed() {
  if (timeline_press_action(timeline_process_ != nullptr) == TimelinePress::close_preview) {
    ask_timeline_preview_to_close();
    return;
  }
  if (config_.timeline_minutes <= 0) {
    if (notifier) notifier("TIMELINE IS OFF", "Turn on Timeline in Settings to use it.");
    return;
  }
  // The window that is about to lose the foreground is read and armed by open_timeline_preview, so every
  // opener gets it (this one and `timeline.open`).
  std::string error;
  if (!open_timeline_preview(std::string(), error)) {
    // The launch says why in a box when the machine cannot show the page at all; the card is the record the
    // person still has once that box is dismissed, and the only word for every other reason.
    BX_LOG_WARN("the Timeline preview did not open: %s", error.c_str());
    // The card speaks to a person; the engine's own wording stays in the line above.
    if (notifier) {
      const bool already_open = error == "the Timeline preview is already open";
      notifier("TIMELINE DID NOT OPEN", already_open ? "It is already open. Look for it on your screen."
                                                     : "Close BuffrX and open it again, then press Alt+4.");
    }
    return;
  }
}

void Protocol::end_timeline_preview() {
  if (timeline_process_) {
    CloseHandle(static_cast<HANDLE>(timeline_process_));
    timeline_process_ = nullptr;
  }
  timeline_preview_pid_ = 0;
  timeline_close_asked_ms_ = 0;
  // The preview may have taken the game off the screen before it was known to be exclusive: ending it gives
  // the game back, so the person never lands on the desktop.
  const bool put_back = put_game_back(timeline_restore_hwnd_);
  BX_LOG_NOTE("timeline preview: closed, game back %d (hwnd 0x%llx, still off screen %d)", static_cast<int>(put_back), static_cast<unsigned long long>(timeline_restore_hwnd_), static_cast<int>(window_left_screen(timeline_restore_hwnd_)));
  timeline_restore_hwnd_ = 0;
  timeline_overlay_hwnd_ = 0;
  timeline_overlay_due_ms_ = 0;
  timeline_overlay_exe_.clear();
  timeline_overlay_title_.clear();
}

void Protocol::poll_timeline_preview(uint32_t now_ms) {
  if (!timeline_process_) return;
  HANDLE process = static_cast<HANDLE>(timeline_process_);
  if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
    BX_LOG_INFO("timeline preview: closed");
    end_timeline_preview();
    return;
  }
  // The preview's window has had a moment to take the foreground. A game in exclusive fullscreen leaves the
  // screen for it: that game cannot be covered, so it is said once and the preview stays open on the desktop.
  // The next press for the same game opens without probing or saying it again, and the game is put back
  // whenever the preview ends.
  if (timeline_overlay_hwnd_ && static_cast<int32_t>(now_ms - timeline_overlay_due_ms_) >= 0) {
    const uintptr_t game = timeline_overlay_hwnd_;
    timeline_overlay_hwnd_ = 0;
    timeline_overlay_due_ms_ = 0;
    if (timeline_overlay_restore_wanted(true, window_left_screen(game))) {
      timeline_exclusive_exe_ = normalized_exe_name(timeline_overlay_exe_);
      BX_LOG_INFO("timeline preview: %s is exclusive fullscreen; the preview stays on the desktop", timeline_overlay_title_.c_str());
      // One line each: the card cuts what does not fit.
      if (notifier) {
        notifier(timeline_overlay_title_ + " IS EXCLUSIVE FULLSCREEN",
                 "Set Borderless Windowed to see Timeline over it.");
      }
    }
    timeline_overlay_exe_.clear();
    timeline_overlay_title_.clear();
  }
  if (timeline_close_asked_ms_ != 0 && now_ms - timeline_close_asked_ms_ > 2000) {
    BX_LOG_WARN("timeline preview: it did not close when asked; ending it");
    TerminateProcess(process, 0);
    end_timeline_preview();
  }
}

// The one manual buffer stop: the Timeline session ends with the buffer, not after its pause grace. Every
// internal stop (a sleep, the lock, a rebuild, a settings change, low memory) keeps the session, because it
// calls ObsHost::stop_buffer directly.
namespace {
void stop_buffer_with_timeline(ObsHost& host) {
  host.end_timeline();
  host.stop_buffer();
}
}  // namespace

bool Protocol::toggle_buffer(std::string& error) {
  if (host_.buffer_wanted()) {
    stop_buffer_with_timeline(host_);
    return true;
  }
  start_on_running_game();
  std::string start_error;
  // A start only waiting for its first frame is accepted, as buffer.start does.
  if (!host_.start_buffer(start_error) && host_.stats().buffer_state != "starting") {
    error = start_error;
    return false;
  }
  return true;
}

bool Protocol::game_is_performance_listed(const std::string& exe) const {
  if (exe.empty()) return false;
  const std::string name = normalized_exe_name(exe);
  return std::find(config_.performance_games.begin(), config_.performance_games.end(), name) != config_.performance_games.end();
}

void Protocol::update_performance_for_game() {
  const bool listed = config_.performance_when == PerformanceWhen::games && game_capture_.capturing &&
                      game_is_performance_listed(game_capture_.active_exe);
  const PerformanceDecision decision =
      performance_decide(config_.performance_when, listed, host_.recording_busy(), host_.performance_active());
  if (decision.deferred) return;  // after the recording
  std::string error;
  if (!host_.set_performance_game(listed, error)) BX_LOG_WARN("Performance mode could not be switched: %s", error.c_str());
}

void Protocol::start_on_running_game() {
  if (!config_.auto_game_capture) return;
  const uint32_t now = GetTickCount();
  if (game_capture_.capturing) {
    if (active_game_alive(now)) return;  // already on a game that runs
    game_capture_ = GameCaptureState{};  // it closed while the buffer was off
  }
  std::string id, exe, title;
  uintptr_t hwnd = 0;
  ForegroundWindow foreground;
  if (foreground_window(foreground) && foreground.fullscreen && !game_process_excluded(foreground.exe, config_.game_exclusions)) {
    id = foreground.id;
    exe = foreground.exe;
    title = foreground.title;
    hwnd = foreground.hwnd;
  } else if (!last_game_id_.empty() && now - last_game_seen_ms_ < 60000 && game_window_alive(last_game_exe_)) {
    // The panel is in front: the game seen a moment ago, still running, is what the user means.
    id = last_game_id_;
    exe = last_game_exe_;
    title = last_game_title_;
  } else {
    // BuffrX started after the game, and the game is not in front: any fullscreen game already on screen.
    for (const WindowInfo& window : enumerate_game_windows()) {
      if (game_process_excluded(window.exe, config_.game_exclusions)) continue;
      id = window.title;
      exe = window.exe;
      title = window.label;
      break;
    }
  }
  if (id.empty()) return;
  if (!(config_.capture_mode == "game" && config_.game_title == id)) {
    before_capture_mode_ = config_.capture_mode;
    before_capture_title_ = config_.game_title;
    Json patch = Json::object();
    patch.set("capture_mode", Json("game"));
    patch.set("game_title", Json(id));
    std::string patch_error;
    apply_patch(patch, patch_error);
    if (!patch_error.empty()) {
      BX_LOG_WARN("could not start on the game: %s", patch_error.c_str());
      return;
    }
  } else {
    before_capture_mode_ = "display";  // already on it (a pinned game): leaving goes back to the display
    before_capture_title_.clear();
  }
  // The game is the active one from the first frame, so leaving it, handing over and alt-tabbing behave as if
  // the foreground poll had entered it.
  game_capture_.capturing = true;
  game_capture_.active_exe = exe;
  game_capture_.active_title = title;
  game_capture_.candidate_exe.clear();
  game_capture_.candidate_title.clear();
  game_capture_.candidate_since_ms = 0;
  active_game_hwnd_ = hwnd;
  alive_cached_ = true;
  alive_checked_ms_ = now;
  // A listed game starts the buffer in Performance mode, instead of building it twice. A manual recording in
  // progress is not split for it: the tick applies the switch once the recording ends
  // (update_performance_for_game), the same deferral the game poll uses.
  if (config_.performance_when == PerformanceWhen::games && !host_.recording_busy()) {
    std::string performance_error;
    host_.set_performance_game(game_is_performance_listed(exe), performance_error);
  }
  BX_LOG_INFO("the buffer starts on the game \"%s\" (%s)", title.c_str(), exe.c_str());
}

Json Protocol::game_state_json() {
  Json observed = Json::object();
  observed.set("exe", Json(observed_exe_));
  observed.set("title", Json(observed_title_));
  Json state = Json::object();
  state.set("observed", std::move(observed));
  state.set("auto_game_capture", Json(config_.auto_game_capture));
  state.set("capturing_game", Json(game_capture_.capturing));
  state.set("active_game", Json(game_capture_.active_title));
  Json exclusions = Json::array();
  for (const std::string& exe : config_.game_exclusions) exclusions.push(Json(exe));
  state.set("exclusions", std::move(exclusions));
  return state;
}

void Protocol::follow_library_paths(const Config& before) {
  if (before.clips_dir == config_.clips_dir && before.screenshots_dir == config_.screenshots_dir &&
      recordings_base_dir(before) == recordings_base_dir(config_)) {
    return;
  }
  // The library lists, guards and deletes by these paths, and sessions live under the recordings folder:
  // a moved folder has to move them too, or the library keeps showing the old one.
  library_init(library_paths_from(config_, data_root_));
  CreateDirectoryA(config_.clips_dir.c_str(), nullptr);
  CreateDirectoryA(config_.screenshots_dir.c_str(), nullptr);
  // A session being recorded keeps its index beside its chunks; the host moves the store when it ends.
  if (host_.current_session_id().empty()) session_store_init(sessions_dir(config_));
  if (broadcaster) broadcaster(make_event("library.changed", Json::object()));
}

void Protocol::apply_hotkeys() {
  std::vector<HotkeyBinding> bindings;
  auto add = [&bindings](const std::string& action, const std::string& chord) {
    HotkeyBinding binding;
    binding.action = action;
    binding.chord = chord;
    std::string problem;
    binding.valid = parse_chord(chord, binding.modifiers, binding.virtual_key, problem);
    binding.problem = problem;
    bindings.push_back(binding);
  };
  for (const HotkeyField& field : kHotkeyFields) add(field.action, config_.*(field.keyboard));
  window_.apply_hotkeys(bindings);

  // The controller half. Every action with a pad chord that parses becomes a binding; one that does not
  // says why on its row, like a keyboard chord in use by another app. A binding whose buttons contain
  // another's would fire both on the same press, so the later one is refused and said so.
  pad_problems_.clear();
  std::vector<PadBinding> pads;
  for (const std::string& action : all_hotkey_actions()) {
    const std::string chord = pad_chord_for(config_, action);
    if (chord.empty()) continue;
    std::string problem;
    const std::optional<uint32_t> combo = parse_pad_chord(chord, problem);
    if (!combo) {
      pad_problems_[action] = problem;
      continue;
    }
    bool clashes = false;
    for (size_t i = 0; i < pads.size(); ++i) {
      if (!pad_chords_conflict(pads[i].combo, *combo)) continue;
      pad_problems_[action] = std::string("overlaps ") + hotkey_action_name(pads[i].action);
      clashes = true;
      break;
    }
    if (clashes) continue;
    pads.push_back({action, *combo});
  }
  pad_poller_.set_bindings(std::move(pads));
}

void Protocol::set_pad_sinks(PadPoller::ActionSink action, PadPoller::CaptureSink capture) {
  pad_poller_.set_action_sink(std::move(action));
  pad_poller_.set_capture_sink(std::move(capture));
}

void Protocol::mark(const std::string& command, const std::string& label) {
  const std::string kind = marker_kind_for_command(command);
  if (kind.empty()) return;
  std::string session_id;
  SessionMarker marker;
  std::string error;
  if (!host_.add_marker(kind, label, session_id, marker, error) && error != "no session open") {
    BX_LOG_WARN("the %s marker was not indexed: %s", kind.c_str(), error.c_str());
  }
}

std::string Protocol::mark_timeline(const std::string& session_id, const std::string& kind,
                                    const std::string& label, int64_t session_ms, std::string& error) {
  SessionEntry entry;
  if (!load_timeline_entry(session_id, entry, error, "is marked this way")) return std::string();
  const uint32_t tick = entry.started_ms + static_cast<uint32_t>(std::max<int64_t>(0, session_ms));
  SessionMarker marker;
  if (host_.add_marker_at(session_id, kind, label, tick, marker, error)) return marker.id;
  // A closed session has no writer, so its index is written here; the host owns only the open one, and
  // writing that one from this copy would be overwritten at its next chunk.
  if (error != "not the open session") return std::string();
  error.clear();
  if (!session_store_add_marker(entry, kind, label, tick, error)) return std::string();
  Json data = Json::object();
  data.set("id", Json(session_id));
  data.set("at_ms", Json(static_cast<int64_t>(tick)));
  data.set("kind", Json(kind));
  data.set("label", Json(label));
  if (broadcaster) broadcaster(make_event("markers.changed", data));
  return entry.markers.back().id;
}

bool Protocol::read_profiles(Json& profiles) {
  std::string text;
  if (!read_file(profiles_path_, text) || text.empty()) {
    profiles = Json::array();
    return true;
  }
  std::string error;
  if (!Json::parse(text, profiles, error)) {
    BX_LOG_WARN("profiles file unreadable: %s", error.c_str());
    profiles = Json::array();
  }
  // Profiles are the four numbered slots, so a name an older build wrote is dropped here and is
  // gone from the file on the next write.
  for (Json& entry : profiles.items()) {
    Json kept = Json::object();
    if (entry.has("patch")) kept.set("patch", entry["patch"]);
    entry = kept;
  }
  return true;
}

bool Protocol::write_profiles(const Json& profiles, std::string& error) {
  if (!write_file_atomic(profiles_path_, profiles.dump(2))) {
    error = "could not write profiles";
    return false;
  }
  return true;
}

Json Protocol::profiles_json() {
  Json profiles;
  const bool exists = GetFileAttributesA(profiles_path_.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (exists && read_profiles(profiles) && profiles.is_array()) return profiles;
  // First run: four starting points, as Rewind shipped them (buffer, frame rate, quality), so the
  // panel's 1-4 chips mean something before anyone has saved a profile.
  struct Seed {
    int buffer;
    int fps;
    const char* quality;
  };
  const Seed seeds[] = {{60, 60, "high"}, {180, 60, "high"}, {60, 120, "high"}, {60, 30, "standard"}};
  Json seeded = Json::array();
  for (const Seed& seed : seeds) {
    Json patch = Json::object();
    patch.set("buffer_seconds", Json(seed.buffer));
    patch.set("capture_fps", Json(seed.fps));
    patch.set("quality", Json(std::string(seed.quality)));
    Json entry = Json::object();
    entry.set("patch", patch);
    seeded.push(entry);
  }
  std::string error;
  write_profiles(seeded, error);
  return seeded;
}

std::string Protocol::handle(const std::string& payload) {
  Json request;
  std::string parse_error;
  if (!Json::parse(payload, request, parse_error)) {
    Json response = Json::object();
    response.set("id", Json(0));
    response.set("ok", Json(false));
    response.set("error", Json("malformed request: " + parse_error));
    return response.dump();
  }
  const int64_t id = request["id"].as_int(0);
  const std::string command = request["cmd"].as_string();
  const Json args = request["args"];

  std::string error;
  Json data;
  const uint32_t asked = GetTickCount();
  uint32_t locked = asked;
  const bool needs_lock = command_needs_engine_lock(command);
  try {
    // See command_policy.h: the main thread's tick holds the same lock around its libobs work.
    std::unique_lock<std::mutex> lock(engine_mutex_, std::defer_lock);
    if (needs_lock) lock.lock();
    if (needs_lock && update_shutting_down() && command == "recording.start") {
      error = "BuffrX is restarting to install an update";
    }
    locked = GetTickCount();
    if (!error.empty()) {
      data = Json::object();
    } else if (needs_lock) {
      const LockWatch::Scope held(engine_lock_watch(), command);
      data = handle_command(command, args, error);
    } else {
      data = handle_command(command, args, error);
    }
  } catch (const std::exception& exception) {
    error = std::string("engine error: ") + exception.what();
  }
  // The window, the tray and the tick all wait on this lock, so a command that holds it for long is the
  // panel freezing. Written down with its name: "the app stopped responding for a second" then has a
  // cause in the log instead of a guess. A folder picker waits for the person, not the engine.
  const uint32_t waited = locked - asked;
  const uint32_t ran = GetTickCount() - locked;
  if (command != "app.pick_folder" && command != "app.pick_file" && (waited > 750 || (needs_lock && ran > 750) || ran > 5000)) {
    BX_LOG_WARN("slow command %s: waited %u ms for the engine, ran %u ms%s", command.c_str(), waited, ran,
                needs_lock ? " holding it" : "");
  }

  Json response = Json::object();
  response.set("id", Json(id));
  response.set("ok", Json(error.empty()));
  if (error.empty()) {
    response.set("data", data);
  } else {
    response.set("error", Json(error));
  }
  return response.dump();
}

Json Protocol::handle_command(const std::string& command, const Json& args, std::string& error) {
  BX_LOG_INFO("command: %s", command.c_str());
  if (command == "hello") {
    Json data = Json::object();
    data.set("protocol", Json(kProtocolVersion));
    data.set("engine", Json(kEngineVersion));
    data.set("obs", Json(host_.obs_version()));
    data.set("data_root", Json(data_root_));
    data.set("portable", Json(portable_));
    data.set("gpu", gpu_json());
    Json capability = Json::array();
    for (const auto& name : capabilities()) capability.push(Json(name));
    data.set("capabilities", capability);
    return data;
  }
  if (command == "status.get") return status_json();
  if (command == "config.get") return config_json();
  // {patch:{...}} as docs/protocol.md specifies; a bare object of fields is still accepted (older
  // clients and the acceptance harness send that).
  if (command == "config.set") return apply_patch(args["patch"].is_object() ? args["patch"] : args, error);
  if (command == "encoder.properties") return host_.encoder_properties(args["configured"].as_bool(false));
  if (command == "encoder.set") {
    // One advanced encoder knob, by its libobs setting name: exactly what encoder.properties listed.
    const std::string name = args["name"].as_string();
    if (name.empty()) {
      error = "encoder.set needs a setting name";
      return Json::object();
    }
    Json patch = Json::object();
    Json options = config_.encoder_options;
    options.set(name, args.has("value") ? args["value"] : Json());
    patch.set("encoder_options", options);
    patch.set("encoder_mode", Json("custom"));  // a row set by hand is the person's own choice
    patch.set("video_preset", Json("custom"));
    apply_patch(patch, error);
    if (!error.empty()) return Json::object();
    return host_.encoder_properties(true);
  }
  if (command == "config.reset") {
    Config defaults;
    apply_path_defaults(defaults, data_root_);
    return apply_patch(config_to_json(defaults), error);
  }

  if (command == "sources.displays") {
    Json array = Json::array();
    for (const auto& display : host_.displays()) {
      Json entry = Json::object();
      entry.set("id", Json(display.id));
      entry.set("name", Json(display.name));
      entry.set("device", Json(display.device));
      entry.set("width", Json(display.width));
      entry.set("height", Json(display.height));
      entry.set("primary", Json(display.primary));
      array.push(entry);
    }
    return array;
  }
  if (command == "sources.windows") {
    Json array = Json::array();
    for (const auto& window : host_.windows()) {
      Json entry = Json::object();
      entry.set("id", Json(window.title));
      entry.set("name", Json(window.label));
      entry.set("exe", Json(window.exe));
      entry.set("minimized", Json(window.minimized));
      array.push(entry);
    }
    return array;
  }
  if (command == "sources.games") {
    Json array = Json::array();
    for (const auto& window : host_.game_windows()) {
      Json entry = Json::object();
      entry.set("title", Json(window.title));
      entry.set("name", Json(window.label));
      array.push(entry);
    }
    return array;
  }
  if (command == "capture.set") {
    Json patch = Json::object();
    if (args.has("mode")) patch.set("capture_mode", Json(args["mode"].as_string("display")));
    if (args.has("display_id")) patch.set("display_id", Json(static_cast<int64_t>(args["display_id"].as_int(0))));
    if (args.has("window_id")) patch.set("window_id", Json(args["window_id"].as_string()));
    if (args.has("window_name")) patch.set("window_name", Json(args["window_name"].as_string()));
    if (args.has("game_title")) patch.set("game_title", Json(args["game_title"].as_string()));
    apply_patch(patch, error);
    if (!error.empty()) return Json::object();
    return status_json();
  }

  if (command == "buffer.start") {
    start_on_running_game();
    // A start that is only waiting for its first frame is accepted: the intent is recorded and the
    // watchdog starts the ring when the source has a frame (status reads buffer_state "starting").
    std::string start_error;
    if (!host_.start_buffer(start_error) && host_.stats().buffer_state != "starting") {
      error = start_error;
      return Json::object();
    }
    if (broadcaster) broadcaster(make_event("status", status_json()));
    return status_json();
  }
  if (command == "buffer.stop") {
    stop_buffer_with_timeline(host_);
    if (broadcaster) broadcaster(make_event("status", status_json()));
    return status_json();
  }
  if (command == "buffer.save") {
    const int seconds = static_cast<int>(args["seconds"].as_int(config_.buffer_seconds));
    const int code = host_.save_clip(seconds, error);
    if (code != 0) return Json::object();
    mark(command, std::to_string(seconds) + " s");
    Json data = Json::object();
    data.set("queued", Json(true));
    data.set("seconds", Json(seconds));
    return data;
  }
  if (command == "recording.start") {
    // The record key, whatever asked for it, starts or stops a standalone recording.
    record_pressed(error);
    return status_json();
  }
  if (command == "recording.stop") {
    // A stop affects the manual recording only, regardless of Timeline state.
    const bool was_recording = host_.stats().recording;
    host_.stop_recording();
    if (was_recording && broadcaster) broadcaster(make_event("recording.changed", Json(false)));
    return status_json();
  }
  if (command == "screenshot.take") {
    std::string path;
    const std::string format = args["format"].as_string(config_.screenshot_format);
    if (!take_screenshot(format, path, error)) return Json::object();
    mark(command, file_name_of(path));
    Json data = Json::object();
    data.set("file", Json(path));
    return data;
  }

  if (command == "clips.list") {
    // One library: the clips the ring saved and every recording, in one list, newest first.
    // Phase 0.8: each part is timed, and a list over 200 ms says where its time went.
    const auto started = std::chrono::steady_clock::now();
    auto ms_since = [](std::chrono::steady_clock::time_point at) {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at).count();
    };
    std::vector<MediaItem> clips = list_clips();
    const double listed_ms = ms_since(started);
    const std::vector<MediaItem> recordings = list_recordings();
    clips.insert(clips.end(), recordings.begin(), recordings.end());
    std::sort(clips.begin(), clips.end(),
              [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
    const double sorted_ms = ms_since(started);
    // The listing decodes no thumbnail: the ones on disk are attached, and each card the panel draws
    // without one asks for it (clips.thumbnail). Decoding here cost 195-673 ms of every 230-clip list.
    library_annotate(clips, 0);
    const double total_ms = ms_since(started);
    if (total_ms > 200.0) {
      BX_LOG_WARN("clips.list took %.0f ms for %d clips: listing %.0f, recordings and sort %.0f, thumbnails %.0f", total_ms,
                  static_cast<int>(clips.size()), listed_ms, sorted_ms - listed_ms, total_ms - sorted_ms);
    }
    return media_json(clips, true);
  }
  if (command == "clips.thumbnail") {
    // One clip's thumbnail, generated on demand because its card is on screen.
    if (!library_owns_path(args["file"].as_string())) {
      error = "that file is not in the BuffrX library";
      return Json::object();
    }
    std::string reason;
    const std::string thumbnail = ensure_thumbnail(args["file"].as_string(), &reason);
    // The media lock's own answer is passed on, because it is the one failure the card retries: any
    // other reason is final and the card keeps its film icon.
    if (thumbnail.empty() && reason.find("busy") != std::string::npos) error = reason;
    Json data = Json::object();
    data.set("thumbnail", Json(thumbnail));
    return data;
  }
  if (command == "clips.frames") {
    // The share sheet's looping preview: stills across one clip, made on the lock-free worker.
    if (!library_owns_path(args["file"].as_string())) {
      error = "that file is not in the BuffrX library";
      return Json::object();
    }
    Json frames = Json::array();
    for (const std::string& frame : ensure_preview_frames(args["file"].as_string())) frames.push(Json(frame));
    Json data = Json::object();
    data.set("frames", frames);
    return data;
  }
  if (command == "screenshots.list") {
    std::vector<MediaItem> shots = list_screenshots();
    // The same budget clips.list passes: a few 480 px thumbnails up front, so the rail never ships
    // multi-MB PNGs to the page.
    library_annotate(shots, thumbnail_budget(kDefaultVisibleCards, static_cast<int>(shots.size())));
    return media_json(shots, true);
  }
  if (command == "storage.usage") {
    int64_t clips_bytes = 0, screenshots_bytes = 0;
    int clip_count = 0, screenshot_count = 0;
    library_storage_usage(clips_bytes, screenshots_bytes, clip_count, screenshot_count);
    int64_t recordings_bytes = 0, sessions_bytes = 0;
    int recordings_count = 0, sessions_count = 0;
    library_recordings_usage(recordings_bytes, recordings_count);
    library_sessions_usage(sessions_bytes, sessions_count);
    Json result = Json::object();
    result.set("clips_bytes", Json(clips_bytes));
    result.set("clips_count", Json(clip_count));
    result.set("screenshots_bytes", Json(screenshots_bytes));
    result.set("screenshots_count", Json(screenshot_count));
    result.set("recordings_bytes", Json(recordings_bytes));
    result.set("recordings_count", Json(recordings_count));
    result.set("sessions_bytes", Json(sessions_bytes));
    result.set("sessions_count", Json(sessions_count));
    result.set("sessions_root", Json(session_store_root()));
    result.set("free_bytes", Json(volume_free_bytes(config_.clips_dir)));
    result.set("clips_dir", Json(config_.clips_dir));
    result.set("screenshots_dir", Json(config_.screenshots_dir));
    return result;
  }
  if (command == "sessions.list") {
    Json result = Json::object();
    Json sessions = Json::array();
    for (const SessionEntry& entry : session_store_load(error)) {
      Json s = Json::object();
      s.set("id", Json(entry.id));
      s.set("kind", Json(entry.kind));
      s.set("app_id", Json(entry.app_id));
      s.set("profile", Json(entry.profile));
      s.set("started_ms", Json(static_cast<int64_t>(entry.started_ms)));
      s.set("ended_ms", Json(static_cast<int64_t>(entry.ended_ms)));
      s.set("closed", Json(entry.closed));
      s.set("interrupted", Json(entry.interrupted));
      s.set("bytes", Json(entry.bytes()));
      s.set("chunk_count", Json(static_cast<int64_t>(entry.chunks.size())));
      s.set("marker_count", Json(static_cast<int64_t>(entry.markers.size())));
      sessions.push(std::move(s));
    }
    result.set("sessions", std::move(sessions));
    return result;
  }
  if (command == "session.get") {
    const std::string id = args.has("id") ? args["id"].as_string() : "";
    SessionEntry entry;
    if (!session_store_load_one(id, entry, error)) return Json::object();
    Json result = Json::object();
    result.set("id", Json(entry.id));
    result.set("kind", Json(entry.kind));
    result.set("app_id", Json(entry.app_id));
    result.set("profile", Json(entry.profile));
    result.set("started_ms", Json(static_cast<int64_t>(entry.started_ms)));
    result.set("ended_ms", Json(static_cast<int64_t>(entry.ended_ms)));
    result.set("closed", Json(entry.closed));
    result.set("interrupted", Json(entry.interrupted));
    result.set("bytes", Json(entry.bytes()));
    result.set("chunk_count", Json(static_cast<int64_t>(entry.chunks.size())));
    result.set("marker_count", Json(static_cast<int64_t>(entry.markers.size())));
    Json chunks = Json::array();
    for (const SessionChunk& chunk : entry.chunks) {
      Json c = Json::object();
      c.set("file", Json(chunk.file));
      c.set("bytes", Json(chunk.bytes));
      c.set("start_ms", Json(static_cast<int64_t>(chunk.start_ms)));
      c.set("end_ms", Json(static_cast<int64_t>(chunk.end_ms)));
      chunks.push(std::move(c));
    }
    result.set("chunks", std::move(chunks));
    Json markers = Json::array();
    for (const SessionMarker& marker : entry.markers) {
      Json m = Json::object();
      m.set("marker_id", Json(marker.id));
      m.set("at_ms", Json(static_cast<int64_t>(marker.at_ms)));
      m.set("kind", Json(marker.kind));
      m.set("label", Json(marker.label));
      markers.push(std::move(m));
    }
    result.set("markers", std::move(markers));
    return result;
  }
  if (command == "timeline.get") {
    // A bounded snapshot of the Timeline's own store, for the preview to refresh while it plays. Every
    // time is the session's own clock - milliseconds since it began - because the index keeps absolute
    // tick times no client can interpret. Only published chunks are footage: the newest one's writer
    // still holds its file, so it never moves `available_end_ms`.
    Json data = Json::object();
    int minutes = 0;
    std::string state;
    ObsHost::LiveChunk live;
    std::string live_session;
    bool waiting_for_session = false;
    {
      std::lock_guard<std::mutex> lock(engine_mutex_);
      minutes = config_.timeline_minutes;
      state = timeline_state_id(host_.timeline_state());
      live = host_.live_chunk();
      if (host_.timeline_writer_active()) live_session = host_.current_session_id();
      // Until the writer's session is open, an older one is never offered in its place, whether the writer is about to
      // start or is being rebuilt for the new game (the encoders change as a game is captured).
      waiting_for_session = minutes > 0 && host_.buffer_wanted() && live_session.empty() &&
                            (state == "writing" || state == "stopped" || state == "paused_rebuild");
    }
    data.set("enabled", Json(minutes > 0));
    data.set("state", Json(state));
    data.set("minutes", Json(minutes));
    // The chunk a writer holds right now is not footage yet, but its flushed fragments are: the preview
    // reads them by byte range so live play is one fragment behind, not one chunk.
    data.set("chunk_seconds", Json(live.chunk_seconds));
    data.set("current_session", Json(live_session));

    std::string load_error;
    std::vector<SessionEntry> sessions;
    const std::string asked = args.has("session") ? args["session"].as_string() : "";
    // `list:false` with a session: the page's every-second refresh reads that one index, not every
    // session's, and keeps the list it already has (it asks for the list again every few polls).
    const bool listed = asked.empty() || !args.has("list") || args["list"].as_bool(true);
    if (listed) {
      for (SessionEntry& entry : session_store_load(load_error)) {
        if (entry.kind == "timeline") sessions.push_back(std::move(entry));
      }
    } else {
      SessionEntry one;
      std::string one_error;
      if (session_store_load_one(asked, one, one_error) && one.kind == "timeline") sessions.push_back(std::move(one));
    }
    Json list = Json::array();
    for (const SessionEntry& entry : sessions) {
      size_t published = 0;
      for (const SessionChunk& chunk : entry.chunks) {
        if (chunk.published) ++published;
      }
      const int64_t ended_at =
          entry.closed && entry.ended_ms >= entry.started_ms
              ? entry.started_at + static_cast<int64_t>(static_cast<uint32_t>(entry.ended_ms - entry.started_ms)) / 1000
              : 0;
      Json summary = Json::object();
      summary.set("id", Json(entry.id));
      // The first game captured in it names the session; a desktop-only session has no title of its own.
      std::string title;
      for (const SessionMarker& marker : entry.markers) {
        if (marker.kind == "game" && !marker.label.empty()) {
          title = marker.label;
          break;
        }
      }
      summary.set("title", Json(title));
      summary.set("started_at", Json(entry.started_at));
      summary.set("ended_at", Json(ended_at));
      summary.set("bytes", Json(entry.bytes()));
      summary.set("published_chunks", Json(static_cast<int64_t>(published)));
      // How much footage is still in the session, from its index alone: the panel's cards say how long a
      // session is, and a session whose writer has not published a chunk yet is zero seconds of it. The
      // disk is checked for the session that is played, never for every session listed.
      int64_t span_start = 0, span_end = 0, span_bytes = 0;
      timeline_published_span(entry, span_start, span_end, span_bytes);
      summary.set("duration_ms", Json(span_end > span_start ? span_end - span_start : 0));
      list.push(std::move(summary));
    }
    if (listed) data.set("sessions", std::move(list));

    const SessionEntry* selected = timeline_choose_session(sessions, asked, live_session, waiting_for_session);
    if (!asked.empty() && !selected) {
      error = "no such timeline session";
      return Json::object();
    }
    int64_t available_start = 0, available_end = 0, bytes = 0;
    Json chunks = Json::array();
    if (selected) {
      // A chunk named here must open. A published chunk whose file was deleted, pruned or moved is not
      // footage any more: it is left out of the list and out of the span - so `available_start_ms` and
      // `available_end_ms` stay true - and said once in the log. Leaving it in would give the page a
      // segment that never decodes and a save a file FFmpeg cannot open.
      const std::vector<size_t> playable = timeline_playable_chunks(*selected, DirListing());
      timeline_published_span(*selected, playable, available_start, available_end, bytes);
      size_t published = 0;
      for (const SessionChunk& chunk : selected->chunks) {
        if (chunk.published) ++published;
      }
      if (playable.size() != published) {
        BX_LOG_WARN("%d published chunk(s) of %s are no longer on disk and are not offered as footage",
                    static_cast<int>(published - playable.size()), selected->id.c_str());
      }
      for (size_t index : playable) {
        const SessionChunk& chunk = selected->chunks[index];
        const int64_t chunk_start = timeline_session_ms(chunk.start_ms, selected->started_ms);
        const int64_t chunk_end = timeline_session_ms(chunk.end_ms, selected->started_ms);
        Json c = Json::object();
        c.set("file", Json(chunk.file));
        c.set("start_ms", Json(chunk_start));
        c.set("end_ms", Json(chunk_end));
        c.set("duration_ms", Json(chunk_end > chunk_start ? chunk_end - chunk_start : 0));
        c.set("bytes", Json(chunk.bytes));
        c.set("epoch", Json(static_cast<int64_t>(chunk.epoch)));
        c.set("published", Json(true));
        chunks.push(std::move(c));
      }
      if (!live.file.empty() && live_session == selected->id) {
        Json l = Json::object();
        l.set("file", Json(live.file));
        l.set("start_ms", Json(timeline_session_ms(live.start_ms, selected->started_ms)));
        l.set("bytes", Json(live.bytes));
        data.set("live", std::move(l));
      }
    }
    data.set("session", Json(selected ? selected->id : std::string()));
    data.set("available_start_ms", Json(available_start));
    data.set("available_end_ms", Json(available_end));
    data.set("bytes", Json(bytes));
    data.set("chunks", std::move(chunks));
    if (!load_error.empty()) BX_LOG_WARN("the timeline listing was incomplete: %s", load_error.c_str());
    return data;
  }
  if (command == "timeline.poster") {
    // One picture for a session's card in the panel's Timeline tab: the middle of its surviving footage,
    // cut and cached through the strip's own path (timeline_thumbs.h), so a card costs one decode and then
    // one stat. File work, outside the engine lock, and bounded by the media lock's deadline.
    const std::string id = args.has("session") ? args["session"].as_string() : "";
    if (id.empty()) {
      error = "timeline.poster needs a session";
      return Json::object();
    }
    SessionEntry entry;
    if (!load_timeline_entry(id, entry, error, "has a poster")) return Json::object();
    const auto on_disk = [](const std::string& file) {
      return GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const std::vector<size_t> playable = timeline_playable_chunks(entry, DirListing());
    const std::vector<TimelineThumbSpan> spans = timeline_thumb_spans(entry, playable);
    int64_t span_start = 0, span_end = 0, span_bytes = 0;
    timeline_published_span(entry, playable, span_start, span_end, span_bytes);
    if (span_end <= span_start) {
      error = "that timeline session has no footage left";
      return Json::object();
    }
    const int64_t at_ms = span_start + (span_end - span_start) / 2;
    const std::vector<TimelineThumb> plan = timeline_thumb_plan(spans, {at_ms}, span_start, span_end);
    if (plan.empty()) {
      error = "that timeline session has no footage left";
      return Json::object();
    }
    const std::string directory = session_thumbs_dir(entry.id);
    CreateDirectoryA(directory.c_str(), nullptr);
    const TimelineThumb& thumb = plan.front();
    const std::string file = directory + "\\" + thumb.name;
    bool ready = on_disk(file);  // the cache is the picture: a repeat is this stat and nothing else
    std::string make_error;
    if (!ready) {
      const std::string& source = entry.chunks[thumb.chunk_index].file;
      if (check_clip_file(source).has_video_track) {
        media_thumbnail(source, file, kTimelineThumbWidth, static_cast<double>(thumb.offset_ms) / 1000.0,
                        make_error);
        ready = on_disk(file);
      }
    }
    if (!ready) {
      // A busy media lock is passed on as itself: the card retries that answer and no other.
      error = make_error.find("busy") != std::string::npos ? make_error : "the poster could not be made";
      return Json::object();
    }
    Json data = Json::object();
    data.set("session", Json(entry.id));
    data.set("file", Json(file));
    data.set("at_ms", Json(at_ms));
    return data;
  }
  if (command == "timeline.edit") {
    std::lock_guard<std::mutex> editor_gate(timeline_editor_gate_);
    const std::string id = args["session"].as_string();
    if (id.empty()) {
      error = "timeline.edit needs a session";
      return Json::object();
    }
    if (timeline_erasing_ == id) {
      // An erase of this footage is already under way and has let go of the gate to wait for its files.
      // Opening an editor on it now would hold a lease on files that are being deleted.
      error = "that footage is being erased; try again in a moment";
      return Json::object();
    }
    SessionEntry entry;
    if (!load_timeline_entry(id, entry, error, "opens this way")) return Json::object();
    {
      std::lock_guard<std::mutex> lock(engine_mutex_);
      if (host_.timeline_writer_active() && host_.current_session_id() == id) {
        error = "Live footage is not editable yet. Trim it in Timeline and save a clip.";
        return Json::object();
      }
    }
    // Protect the handoff before the editor starts. The editor renews this owner-scoped lease while
    // its sequence uses the footage; an editor that never starts lets it expire on its own.
    Json lease = Json::object();
    lease.set("session", Json(id));
    lease.set("owner", Json("editor:" + id));
    lease.set("start_ms", Json(static_cast<int64_t>(0)));
    lease.set("end_ms", Json(static_cast<int64_t>(315360000000LL)));
    handle_command("timeline.pin", lease, error);
    if (!error.empty()) return Json::object();
    const std::vector<std::string> files{session_media_dir(id)};
    Json data = Json::object();
    data.set("session", Json(id));
    if (editor_router && editor_router(files, false)) {
      data.set("forwarded", Json(true));
      return data;
    }
    DWORD pid = 0;
    if (!launch_editor(files, data_root_, pid, false)) {
      // The lease keeps this session's chunks pinned for its ten minutes, and its owner is how it is freed:
      // an editor that never opened must not leave the footage locked against erase.
      Json release = Json::object();
      release.set("owner", Json("editor:" + id));
      std::string ignored;
      handle_command("timeline.unpin", release, ignored);
      error = "the editor could not be started";
      return Json::object();
    }
    data.set("pid", Json(static_cast<int64_t>(pid)));
    return data;
  }
  if (command == "timeline.edit_source") {
    const std::string id = args["session"].as_string();
    if (id.empty()) {
      error = "timeline.edit_source needs a session";
      return Json::object();
    }
    {
      std::lock_guard<std::mutex> lock(engine_mutex_);
      if (host_.timeline_writer_active() && host_.current_session_id() == id) {
        error = "Live footage is not editable yet. Trim it in Timeline and save a clip.";
        return Json::object();
      }
    }
    SessionEntry entry;
    if (!load_timeline_entry(id, entry, error, "opens this way")) return Json::object();
    for (const SessionChunk& chunk : entry.chunks) {
      if (chunk.published && GetFileAttributesA(chunk.file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "a timeline chunk is no longer on disk: " + chunk.file;
        return Json::object();
      }
    }
    Json get = Json::object();
    get.set("session", Json(id));
    get.set("list", Json(false));
    Json data = handle_command("timeline.get", get, error);
    if (error.empty() && data["chunks"].items().empty()) error = "that timeline session has no footage left";
    return data;
  }
  if (command == "timeline.preview") {
    // The ordinary preview window, playing a session instead of a file. The session's media folder is the
    // handle: `editor.open` forwards it unchanged to a window that is already open, and the page recognises
    // it as a session (ui/src/views/Preview.tsx). The folder is under the data root, which the editor's
    // media scheme already serves.
    const std::string id = args.has("session") ? args["session"].as_string() : "";
    if (id.empty()) {
      error = "timeline.preview needs a session";
      return Json::object();
    }
    SessionEntry entry;
    if (!load_timeline_entry(id, entry, error, "opens this way")) return Json::object();
    if (timeline_playable_chunks(entry, DirListing()).empty()) {
      error = "that timeline session has no footage left";
      return Json::object();
    }
    const std::vector<std::string> files{session_media_dir(entry.id)};
    Json data = Json::object();
    data.set("session", Json(entry.id));
    if (editor_router && editor_router(files, true)) {
      data.set("forwarded", Json(true));
      return data;
    }
    DWORD pid = 0;
    if (!launch_editor(files, data_root_, pid, true)) {
      error = "the preview could not be started";
      return Json::object();
    }
    data.set("pid", Json(static_cast<int64_t>(pid)));
    return data;
  }
  if (command == "timeline.open") {
    // The fullscreen overlay on a chosen session, which is only what the preview's Trim reaches: reading a
    // session's footage never leaves the ordinary preview window.
    const std::string id = args.has("session") ? args["session"].as_string() : "";
    if (!open_timeline_preview(id, error)) return Json::object();
    Json data = Json::object();
    data.set("session", Json(id));
    data.set("opened", Json(true));
    return data;
  }
  if (command == "timeline.thumbs") {
    // The preview's strip: one JPEG per requested moment, cut from the published chunk that holds it and
    // written under the session's own `thumbs` folder. File work only, and outside the engine lock
    // (command_policy.h); the media lock already bounds the decode and gives it a deadline, so a strip that
    // cannot be made costs a strip cell, never a stalled engine. At most kTimelineThumbsDecoded new pictures
    // per call: the first refresh of a long strip is short, and the rest arrives on the next poll.
    std::string session_id = args.has("session") ? args["session"].as_string() : "";
    if (session_id.empty()) {
      std::string list_error;
      for (const SessionEntry& candidate : session_store_load(list_error)) {
        if (candidate.kind != "timeline") continue;
        session_id = candidate.id;
        break;
      }
    }
    std::vector<int64_t> times;
    const bool asked_times = args.has("times_ms") && args["times_ms"].is_array();
    if (asked_times) {
      for (const Json& item : args["times_ms"].items()) {
        if (!item.is_number()) continue;
        times.push_back(item.as_int(0));
        if (times.size() >= kTimelineThumbsPerCall) break;  // 24 is what one strip may ask for
      }
    } else {
      error = "timeline.thumbs needs times_ms";
      return Json::object();
    }
    if (session_id.empty()) {
      // Nothing has been recorded yet: no session is not a failure, it is an empty strip.
      Json data = Json::object();
      data.set("session", Json(std::string()));
      data.set("thumbs", Json::array());
      return data;
    }
    SessionEntry entry;
    if (!load_timeline_entry(session_id, entry, error, "has thumbnails")) return Json::object();
    // Only a published chunk that is still on disk may be decoded: an unpublished one is being written, and
    // a missing one has nothing to read. A gap between two chunks names no picture.
    const auto on_disk = [](const std::string& file) {
      return GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const std::vector<size_t> playable = timeline_playable_chunks(entry, DirListing());
    const std::vector<TimelineThumbSpan> spans = timeline_thumb_spans(entry, playable);
    int64_t span_start = 0, span_end = 0, span_bytes = 0;
    timeline_published_span(entry, playable, span_start, span_end, span_bytes);
    const std::vector<TimelineThumb> plan = timeline_thumb_plan(spans, times, span_start, span_end);
    const std::string directory = session_thumbs_dir(entry.id);
    // Its parent is the session's own folder, which exists: one level, never a tree invented somewhere else.
    if (!plan.empty()) CreateDirectoryA(directory.c_str(), nullptr);
    size_t made = 0;
    Json list = Json::array();
    for (const TimelineThumb& thumb : plan) {
      const std::string file = directory + "\\" + thumb.name;
      bool ready = on_disk(file);  // the picture is the cache: a repeat costs this stat and nothing else
      if (!ready && made < kTimelineThumbsDecoded) {
        ++made;
        const std::string& source = entry.chunks[thumb.chunk_index].file;
        // The engine's own container check before any decoder sees the file, as the library does: a chunk
        // recovered from a crashed session can be half a container, and FFmpeg's demuxer does not survive
        // one. The frame is at the requested offset inside that chunk, never inside the session.
        if (check_clip_file(source).has_video_track) {
          std::string make_error;
          media_thumbnail(source, file, kTimelineThumbWidth, static_cast<double>(thumb.offset_ms) / 1000.0,
                          make_error);
          ready = on_disk(file);
        }
      }
      if (!ready) continue;  // silent: a cell with no picture is the answer, never a failed command
      Json item = Json::object();
      item.set("at_ms", Json(thumb.at_ms));
      item.set("file", Json(file));
      list.push(std::move(item));
    }
    Json data = Json::object();
    data.set("session", Json(entry.id));
    data.set("thumbs", std::move(list));
    return data;
  }
  if (command == "timeline.loudness") {
    // The bar's loud moments: one byte per second of each published chunk, read from its sound once and kept
    // beside the chunk's thumbnails (timeline_loudness.h). File work, outside the engine lock like the
    // thumbnails; at most kTimelineLoudnessPerCall new chunks are read per call, near the playhead first, and `pending`
    // says how many are still unread, so the page asks again until it is zero.
    std::string session_id = args.has("session") ? args["session"].as_string() : "";
    if (session_id.empty()) {
      Json data = Json::object();
      data.set("session", Json(std::string()));
      data.set("chunks", Json::array());
      data.set("pending", Json(0));
      return data;
    }
    SessionEntry entry;
    if (!load_timeline_entry(session_id, entry, error, "has levels")) return Json::object();
    const auto on_disk = [](const std::string& file) {
      return GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    std::vector<size_t> playable = timeline_playable_chunks(entry, DirListing());
    // The page asks for the stretch it is drawing (and a view of margin either side), not the whole session: a two-hour
    // session is 1440 chunks, and reading every one's cache and sound on each call is work for a bar that shows a few minutes.
    if (args.has("from_ms") && args.has("to_ms")) {
      const int64_t from_ms = args["from_ms"].as_int(0), to_ms = args["to_ms"].as_int(0);
      playable.erase(std::remove_if(playable.begin(), playable.end(),
                                    [&](size_t index) {
                                      const SessionChunk& chunk = entry.chunks[index];
                                      return timeline_session_ms(chunk.end_ms, entry.started_ms) < from_ms ||
                                             timeline_session_ms(chunk.start_ms, entry.started_ms) > to_ms;
                                    }),
                     playable.end());
    }
    const std::string directory = session_thumbs_dir(entry.id);
    if (!playable.empty()) CreateDirectoryA(directory.c_str(), nullptr);
    size_t made = 0, pending = 0;
    Json list = Json::array();
    std::vector<int64_t> starts;
    starts.reserve(playable.size());
    for (size_t index : playable) starts.push_back(timeline_session_ms(entry.chunks[index].start_ms, entry.started_ms));
    const int64_t preferred = args.has("at_ms") ? args["at_ms"].as_int(-1) : -1;
    for (size_t at : timeline_loudness_order(starts, preferred)) {
      const SessionChunk& chunk = entry.chunks[playable[at]];
      const std::string cache = directory + "\\" + timeline_thumb_key(chunk.file) + ".lv";
      std::vector<uint8_t> levels;
      bool have = false;
      if (on_disk(cache)) {
        std::ifstream in(cache, std::ios::binary);
        levels.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        have = !levels.empty();
        // An empty cache is what a full disk or a crash left: read again rather than trusted for ever.
        if (!have) DeleteFileA(cache.c_str());
      }
      if (!have && made < kTimelineLoudnessPerCall) {
        ++made;
        if (check_clip_file(chunk.file).has_video_track) {
          std::vector<float> rms;
          std::string read_error;
          if (media_loudness(chunk.file, rms, read_error)) {
            levels = timeline_loudness_bytes(rms);
            // Whole or not at all: a reader never meets half a file, and a failed write is just not cached.
            if (!write_file_atomic(cache, std::string(levels.begin(), levels.end()))) {
              BX_LOG_WARN("the loudness of %s could not be cached", chunk.file.c_str());
            }
            have = !levels.empty();
          }
        }
      }
      if (!have) {
        ++pending;
        continue;
      }
      Json item = Json::object();
      item.set("start_ms", Json(static_cast<int64_t>(timeline_session_ms(chunk.start_ms, entry.started_ms))));
      Json values = Json::array();
      for (uint8_t level : levels) values.push(Json(static_cast<int64_t>(level)));
      item.set("levels", std::move(values));
      list.push(std::move(item));
    }
    Json data = Json::object();
    data.set("session", Json(entry.id));
    data.set("bucket_ms", Json(kTimelineLoudnessBucketMs));
    data.set("chunks", std::move(list));
    data.set("pending", Json(static_cast<int64_t>(pending)));
    return data;
  }
  if (command == "timeline.pin") {
    // A bounded lease on a range: a preview, a selection or an export asks retention to keep the files it
    // reads for ten minutes and renews it while it still needs them. The lease is the owner's: a second pin
    // from the same owner on the same session renews rather than stacks.
    const std::string asked = args.has("session") ? args["session"].as_string() : "";
    // Clamped before the session's start is added: a hand-written or hostile millisecond count would
    // overflow that sum and land a lease on footage nobody asked for.
    const int64_t start_ms = std::clamp<int64_t>(args["start_ms"].as_int(-1), 0, UINT32_MAX);
    const int64_t end_ms = std::clamp<int64_t>(args["end_ms"].as_int(-1), 0, UINT32_MAX);
    if (end_ms <= start_ms) {
      error = "the range is empty";
      return Json::object();
    }
    std::string session_id = asked;
    if (session_id.empty()) {
      std::string list_error;
      for (const SessionEntry& candidate : session_store_load(list_error)) {
        if (candidate.kind != "timeline") continue;
        session_id = candidate.id;
        break;
      }
    }
    if (session_id.empty()) {
      error = "there is no timeline footage to pin";
      return Json::object();
    }
    // The number the lease is checked against, read before the index: a removal that begins or ends between this read
    // and the grant refuses the lease, so a client is never told it holds footage a sweep has already taken away
    // (timeline_retention.h).
    const uint64_t removal_seen = timeline_removal_seen(session_id);
    SessionEntry entry;
    if (!load_timeline_entry(session_id, entry, error, "is pinned this way")) return Json::object();
    const std::string owner =
        args.has("owner") && !args["owner"].as_string().empty() ? args["owner"].as_string() : "client";
    const int64_t now_wall_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
    TimelinePin pin;
    pin.owner = owner;
    pin.session_id = session_id;
    // Session milliseconds to the wall clock the retention rule compares with the chunk index.
    pin.start_ms = entry.started_at * 1000 + start_ms;
    pin.end_ms = entry.started_at * 1000 + end_ms;
    pin.expires_ms = now_wall_ms + 10 * 60000;
    std::string lease_id;
    {
      std::lock_guard<std::mutex> lock(engine_mutex_);
      pin.id = "lease " + std::to_string(now_wall_ms) + " " + std::to_string(++timeline_lease_seq_);
      lease_id = host_.take_timeline_lease(pin, now_wall_ms, removal_seen);
      if (lease_id.empty()) {
        error = "that footage is being removed; try again in a moment";
        return Json::object();
      }
    }
    Json data = Json::object();
    data.set("id", Json(lease_id));
    data.set("owner", Json(owner));
    data.set("session", Json(session_id));
    data.set("start_ms", Json(start_ms));
    data.set("end_ms", Json(end_ms));
    data.set("ttl_ms", Json(static_cast<int64_t>(600000)));
    data.set("expires_ms", Json(pin.expires_ms));
    BX_LOG_INFO("timeline lease %s held by %s on %s [%lld, %lld)", lease_id.c_str(), owner.c_str(),
                session_id.c_str(), static_cast<long long>(start_ms), static_cast<long long>(end_ms));
    return data;
  }
  if (command == "timeline.unpin") {
    // Released by lease id, or by owner (one session's lease when `session` is given: what a closing
    // preview does). An unknown id releases nothing and says so with `released: 0`, not an error.
    const std::string id = args.has("id") ? args["id"].as_string() : "";
    const std::string owner = args.has("owner") ? args["owner"].as_string() : "";
    if (id.empty() && owner.empty()) {
      error = "timeline.unpin needs a lease id or an owner";
      return Json::object();
    }
    const std::string session_id = args.has("session") ? args["session"].as_string() : "";
    const int64_t now_wall_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
    size_t released = 0;
    {
      std::lock_guard<std::mutex> lock(engine_mutex_);
      if (!id.empty()) {
        released = host_.release_timeline_lease(id, now_wall_ms) ? 1 : 0;
      } else {
        released = host_.release_timeline_leases(owner, session_id, now_wall_ms);
      }
    }
    Json data = Json::object();
    data.set("released", Json(static_cast<int64_t>(released)));
    return data;
  }
  if (command == "timeline.save") {
    // Task 8's body: a range on the session's clock, copied into the library as a normal clip. Already
    // outside the engine lock (command_policy.h), so its wait for the open segment stalls nothing.
    Json out = Json::object();
    // `notify`: the preview has already gone back to the game, so the engine's own notice is the answer.
    const bool notify = args.has("notify") && args["notify"].as_bool(false);
    if (!timeline_save(args, out, error)) {
      if (notify && notifier) notifier("CLIP NOT SAVED", error);
      return Json::object();
    }
    if (notify && notifier) {
      const std::string file = out["file"].as_string();
      notifier("CLIP SAVED", file.substr(file.find_last_of("\\/") + 1));
    }
    return out;
  }
  if (command == "timeline.erase") {
    // Stops this session's writer under the lock, then deletes outside it: a stop needs the engine, a
    // delete waits on the file system. Only the Timeline's own temporary store is ever removed; a normal
    // recording and a saved clip are refused by the store.
    const std::string id = args.has("session") ? args["session"].as_string() : "";
    if (id.empty()) {
      error = "timeline.erase needs a session";
      return Json::object();
    }
    // Held for the whole erase, the settle wait and every retry: a lease asked for meanwhile is refused, and a save
    // or preview that was already reading the footage keeps the erase out.
    TimelineRetentionTicket erasing;
    {
      // The gate is held for the decision only, never for the wait below: a writer letting go and then a
      // file system letting go can take seconds, and an editor opening behind that 6-second gate said
      // nothing for all of it. What the wait needs instead is the claim, which the edit below answers.
      std::lock_guard<std::mutex> editor_gate(timeline_editor_gate_);
      std::lock_guard<std::mutex> lock(engine_mutex_);
      const int64_t now_wall_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
      if (timeline_editor_holds(host_.timeline_pins(), id, now_wall_ms)) {
        error = "that footage is open in the editor; close the editor before erasing it";
        return Json::object();
      }
      // The claim is the whole session, decided under the retention gate against the leases in force: a save, a
      // preview or an editor lease granted before this point is a reader still using the footage, and the erase is
      // refused rather than pulling its files away. A lease asked for after this point is refused by the claim
      // instead, so the two can never both be granted (timeline_retention.h). The ring's own dedupe lease is
      // ignored here: it renews every second and would otherwise keep the erase out for as long as the buffer runs.
      std::string leased_by;
      erasing = TimelineRetentionTicket::claim_session(id, now_wall_ms, kTimelineRingLeaseOwner, leased_by);
      if (!erasing.active()) {
        error = leased_by.empty() ? "that footage is being removed; try again in a moment"
                                  : "that footage is in use by a save or a preview; try again in a moment";
        return Json::object();
      }
      if (host_.timeline_writer_active() && host_.current_session_id() == id) {
        BX_LOG_INFO("timeline erase stops the writer on %s", id.c_str());
        host_.close_timeline(GetTickCount());
      }
      timeline_erasing_ = id;
    }
    // A stop the writer answered releases its file in the muxer's own time; the session's index closes
    // with it. Wait for that moment, then delete - retried, because Windows holds a file until it lets go.
    const uint32_t settle_deadline = GetTickCount() + 4000;
    while (host_.current_session_id() == id && static_cast<int32_t>(GetTickCount() - settle_deadline) < 0) {
      Sleep(100);
    }
    std::vector<std::string> locked;
    std::string erase_error;
    bool erased = session_store_erase_timeline(id, locked, erase_error, &erasing);
    for (int attempt = 0; !erased && !locked.empty() && attempt < 8; ++attempt) {
      Sleep(250);
      erased = session_store_erase_timeline(id, locked, erase_error, &erasing);
    }
    {
      // The claim ends with the attempt, whatever it answered, or a later edit of this session would be
      // refused by an erase that already gave up.
      std::lock_guard<std::mutex> editor_gate(timeline_editor_gate_);
      if (timeline_erasing_ == id) timeline_erasing_.clear();
    }
    if (!erased) {
      error = erase_error.empty() ? "the timeline session could not be erased" : erase_error;
      return Json::object();
    }
    Json changed;
    {
      std::lock_guard<std::mutex> lock(engine_mutex_);
      changed = timeline_status_json();
    }
    if (broadcaster) broadcaster(make_event("timeline.changed", changed));
    Json data = Json::object();
    data.set("session", Json(id));
    data.set("erased", Json(true));
    return data;
  }
  if (command == "markers.add") {
    // Through the host, which owns the open session's index; see ObsHost::add_marker.
    const std::string kind = args.has("kind") ? args["kind"].as_string() : "marker";
    const std::string label = args.has("label") ? args["label"].as_string() : "";
    if (args.has("session") && args.has("at_ms")) {
      // The Timeline's own mark: at its playhead, in the session it shows, on the session's clock.
      const std::string session = args["session"].as_string();
      const int64_t at_ms = args["at_ms"].as_int(-1);
      if (at_ms < 0) {
        error = "at_ms must be a moment in the session";
        return Json::object();
      }
      const std::string marker_id = mark_timeline(session, kind, label, at_ms, error);
      if (marker_id.empty()) return Json::object();
      Json result = Json::object();
      result.set("id", Json(session));
      result.set("marker_id", Json(marker_id));
      result.set("at_ms", Json(at_ms));
      result.set("kind", Json(kind));
      result.set("label", Json(label));
      return result;
    }
    std::string id;
    SessionMarker marker;
    if (!host_.add_marker(kind, label, id, marker, error)) return Json::object();
    Json result = Json::object();
    result.set("id", Json(id));
    result.set("marker_id", Json(marker.id));
    result.set("at_ms", Json(static_cast<int64_t>(marker.at_ms)));
    result.set("kind", Json(marker.kind));
    result.set("label", Json(marker.label));
    return result;
  }
  if (command == "markers.remove") {
    const std::string id = args["session"].as_string();
    // A marker's own id, or - as every client before ids sent it - the moment, kind and label.
    const std::string marker_id = args["marker_id"].as_string();
    const int64_t at_ms = args["at_ms"].as_int(-1);
    const std::string kind = args["kind"].as_string();
    const std::string label = args["label"].as_string();
    if (id.empty() || (marker_id.empty() && (at_ms < 0 || at_ms > UINT32_MAX || kind.empty()))) {
      error = "markers.remove needs a session, and a marker_id or a time and kind";
      return Json::object();
    }
    SessionEntry entry;
    if (!session_store_load_one(id, entry, error)) return Json::object();
    if (entry.kind != "timeline") { error = "only timeline markers can be removed here"; return Json::object(); }
    if (marker_id.empty()) {
      const uint32_t tick = entry.started_ms + static_cast<uint32_t>(at_ms);
      if (!host_.remove_marker_at(id, tick, kind, label, error)) {
        if (error != "not the open session") return Json::object();
        error.clear();
        if (!session_store_remove_marker(entry, tick, kind, label, error)) return Json::object();
      }
    } else if (!host_.remove_marker_by_id(id, marker_id, error)) {
      // The open session's list lives in the host: a removal written behind its back would come back at
      // its next chunk, and only the host can pick the duplicate the id names.
      if (error != "not the open session") return Json::object();
      error.clear();
      if (!session_store_remove_marker(entry, marker_id, error)) return Json::object();
    }
    Json changed = Json::object();
    changed.set("id", Json(id));
    if (broadcaster) broadcaster(make_event("markers.changed", changed));
    Json result = Json::object();
    result.set("removed", Json(true));
    return result;
  }
  if (command == "clips.export_range") {
    // A moment in a session becomes a clip: the chunks that cover the window, stream-copied into one
    // file in the clips directory. Never from an open session, whose last chunk is still being written.
    const std::string id = args["id"].as_string();
    // Clamped before the session's start is added: `INT64_MAX` from a client overflowed the sum and the
    // cast that follows, so the range met the chunks as some other window.
    const int64_t start_ms = std::clamp<int64_t>(args["start_ms"].as_int(-1), 0, UINT32_MAX);
    const int64_t end_ms = std::clamp<int64_t>(args["end_ms"].as_int(-1), 0, UINT32_MAX);
    SessionEntry entry;
    if (!session_store_load_one(id, entry, error)) return Json::object();
    // The live session reads back as "interrupted" (its index is not closed yet), so ask the host.
    if (id == host_.current_session_id() || (!entry.closed && !entry.interrupted)) {
      error = "the session is still recording";
      return Json::object();
    }
    ConcatPlan plan;
    // The window is session time - the clock timeline.get, the markers and the editor's cut use - so it
    // becomes absolute ticks before it meets the chunks. Reading the two clocks as one refused every
    // documented range with "the range covers no recorded footage".
    const int64_t started_ms = entry.started_ms;
    const int64_t start_abs = started_ms + start_ms;
    const int64_t end_abs = started_ms + end_ms;
    if (start_ms < 0 || end_ms <= start_ms || end_abs > UINT32_MAX ||
        !concat_plan_for_range(entry.chunks, static_cast<uint32_t>(start_abs), static_cast<uint32_t>(end_abs),
                               plan)) {
      error = "the range covers no recorded footage";
      return Json::object();
    }
    const std::string first = plan.files.front();
    const size_t dot = first.find_last_of('.');
    // This command runs outside the engine lock (it can take seconds); copy the two settings it needs.
    std::string clips_dir, container;
    {
      std::lock_guard<std::mutex> lock(engine_mutex_);
      clips_dir = config_.clips_dir;
      container = config_.container;
    }
    const std::string extension = dot == std::string::npos ? "." + container : first.substr(dot);
    const std::string stem = args.has("name") && !args["name"].as_string().empty() ? args["name"].as_string()
                                                                                    : "BuffrX " + id.substr(id.find(' ') + 1);
    // Two workers can export at once, so the staged copy is this export's own name, claimed by creating it: a
    // name another writer made in between is refused and the attempt moves on, never truncated. The join writes
    // under a name the library never lists, and only a verified file takes the name a person sees: a crash
    // mid-export leaves a .part, never a broken clip.
    const std::string staged = reserve_staging_path(clips_dir, stem, extension, false);
    if (staged.empty()) {
      error = "the exported clip could not be staged beside the clips";
      BX_LOG_WARN("range export from %s failed: %s", id.c_str(), error.c_str());
      return Json::object();
    }
    // A stream copy across chunks can carry the first chunk's AV1 sequence header into later chunks
    // that have a different one; the MP4 muxer then leaves an unfinalized file. Re-encode the range
    // to one common target instead, so the export still works.
    auto reencode_range = [&]() -> bool {
      // The staged name is this export's from before the join: the export writes it in place (its own temporaries sit
      // beside it), so the claim is never dropped and re-taken in between.
      ExportJob job;
      job.output = staged;
      if (plan.files.size() == 1) {
        job.input = plan.files.front();
        job.start = plan.start_in_first;
        job.end = plan.end_in_last;
      } else {
        job.inputs.reserve(plan.files.size());
        for (size_t index = 0; index < plan.files.size(); ++index) {
          ExportInput input;
          input.input = plan.files[index];
          if (index == 0) input.start = plan.start_in_first;
          if (index + 1 == plan.files.size()) input.end = plan.end_in_last;
          job.inputs.push_back(std::move(input));
        }
      }
      std::string encoder_name;
      if (!media_export(job, [](double) {}, encoder_name, error)) {
        if (error.empty()) error = "the range could not be re-encoded";
        return false;
      }
      return true;
    };
    if (!media_concat(plan.files, staged, plan.start_in_first, plan.end_in_last, error)) {
      BX_LOG_WARN("range export from %s failed: %s; re-encoding the range", id.c_str(), error.c_str());
      error.clear();
      if (!reencode_range()) {
        DeleteFileA(staged.c_str());
        BX_LOG_WARN("range export from %s could not be re-encoded: %s", id.c_str(), error.c_str());
        return Json::object();
      }
    }
    ClipCheck verified = check_clip_file(staged);
    if (!verified.ok) {
      BX_LOG_WARN("range export from %s did not verify (%s); re-encoding the range", id.c_str(),
                  verified.reason.c_str());
      error.clear();
      if (!reencode_range()) {
        DeleteFileA(staged.c_str());
        BX_LOG_WARN("range export from %s could not be re-encoded: %s", id.c_str(), error.c_str());
        return Json::object();
      }
      verified = check_clip_file(staged);
    }
    if (!verified.ok) {
      DeleteFileA(staged.c_str());
      error = "the exported clip did not verify: " + verified.reason;
      BX_LOG_WARN("%s (file: %s)", error.c_str(), staged.c_str());
      return Json::object();
    }
    // Verified whole: only now does it take the name the library lists. The name is claimed at the moment of the move,
    // not before a join that can take minutes, because a replay save or another export can take a name meanwhile: the
    // move never replaces, so a refused name keeps that writer's bytes and this export settles on the next name. A
    // reader sees the clip or nothing, never half of it.
    SaveNameClaim name_claim;
    const bx::SavePublishResult published = bx::publish_staged_clip(
        staged, [&] { return claim_save_path(clips_dir, stem, extension, name_claim); },
        [](const std::string& from, const std::string& to, unsigned long* move_error) {
          if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
          if (move_error) *move_error = GetLastError();
          return false;
        });
    if (published.path.empty()) {
      // Every name was refused. The footage stays in its staging file, out of the library and named here, so a copy
      // that could not be published is never deleted to make the failure look tidy.
      error = "the exported clip could not be put in place";
      BX_LOG_WARN("%s (staged: %s, %lu)", error.c_str(), staged.c_str(), published.error);
      return Json::object();
    }
    const std::string path = published.path;
    BX_LOG_INFO("range export from %s: %s (%lld bytes, %zu chunks)", id.c_str(), path.c_str(),
                static_cast<long long>(verified.bytes), plan.files.size());
    if (broadcaster) broadcaster(make_event("library.changed", Json::object()));
    Json data = Json::object();
    data.set("file", Json(path));
    data.set("bytes", Json(verified.bytes));
    data.set("chunks", Json(static_cast<int64_t>(plan.files.size())));
    return data;
  }
  if (command == "game.state") return game_state_json();
  if (command == "game.exclusions") {
    // The denylist the product ships and the entries the user added, so the panel can show what is already excluded instead
    // of leaving a person to find out by watching a browser take the capture.
    Json list = Json::array();
    for (const std::string& name : exclusion_entries(config_.game_exclusions, builtin_exclusions())) {
      Json entry = Json::object();
      entry.set("name", Json(name));
      entry.set("builtin", Json(exclusion_is_builtin(name)));
      list.push(entry);
    }
    Json data = Json::object();
    data.set("exclusions", std::move(list));
    return data;
  }
  if (command == "sessions.prune") {
    // Zero is unlimited to the pruner, so a budget that is not a positive number must reach it as zero:
    // a negative `recordings_budget_gb` handed through would delete every recording but the newest.
    const int64_t budget = config_.recordings_budget_gb > 0
                               ? static_cast<int64_t>(config_.recordings_budget_gb) * 1024 * 1024 * 1024
                               : 0;
    std::vector<std::string> removed;
    int64_t freed = 0;
    int64_t total = 0;
    if (!session_prune(budget, host_.current_session_id(), removed, freed, total, error)) return Json::object();
    if (!removed.empty() && broadcaster) broadcaster(make_event("library.changed", Json::object()));
    Json ids = Json::array();
    for (const std::string& id : removed) ids.push(Json(id));
    Json data = Json::object();
    data.set("removed", std::move(ids));
    data.set("freed_bytes", Json(freed));
    data.set("total_bytes", Json(total));
    data.set("budget_bytes", Json(budget));
    return data;
  }
  if (command == "markers.list") {
    Json result = Json::array();
    const std::string id = args.has("id") ? args["id"].as_string() : "";
    for (const SessionEntry& entry : session_store_load(error)) {
      if (!id.empty() && entry.id != id) continue;
      for (const SessionMarker& marker : entry.markers) {
        Json m = Json::object();
        m.set("id", Json(entry.id));
        m.set("marker_id", Json(marker.id));
        m.set("at_ms", Json(static_cast<int64_t>(marker.at_ms)));
        m.set("kind", Json(marker.kind));
        m.set("label", Json(marker.label));
        result.push(std::move(m));
      }
    }
    return result;
  }
  if (command == "clips.rename" || command == "screenshots.rename") {
    std::string result;
    if (!library_rename(args["file"].as_string(), args["name"].as_string(), result)) {
      error = "the file could not be renamed";
      return Json::object();
    }
    if (broadcaster) broadcaster(make_event("library.changed", Json::object()));
    Json data = Json::object();
    data.set("file", Json(result));
    return data;
  }
  if (command == "clips.trash" || command == "screenshots.trash") {
    if (!library_trash(args["file"].as_string())) {
      error = "the file could not be moved to the Recycle Bin";
      return Json::object();
    }
    if (broadcaster) broadcaster(make_event("library.changed", Json::object()));
    return Json::object();
  }
  if (command == "clips.delete") {
    if (!library_delete(args["file"].as_string())) {
      error = "the file could not be deleted";
      return Json::object();
    }
    if (broadcaster) broadcaster(make_event("library.changed", Json::object()));
    return Json::object();
  }
  if (command == "clips.reveal" || command == "screenshots.reveal") {
    // Only files in the library: this hands a path to the shell to reveal, which must never be any path.
    if (!library_owns_path(args["file"].as_string())) {
      error = "that file is not in the BuffrX library";
      return Json::object();
    }
    reveal_file(args["file"].as_string());
    return Json::object();
  }
  if (command == "clips.open") {
    // Only files in the library: this hands a path to the shell to open, which must never be any path.
    if (!library_owns_path(args["file"].as_string())) {
      error = "that file is not in the BuffrX library";
      return Json::object();
    }
    open_file(args["file"].as_string());
    return Json::object();
  }
  if (command == "clips.copy") {
    std::vector<std::string> files;
    for (const auto& item : args["files"].items()) {
      if (!item.as_string().empty()) files.push_back(item.as_string());
    }
    if (files.empty() && !args["file"].as_string().empty()) files.push_back(args["file"].as_string());
    for (const std::string& file : files) {
      if (!library_owns_path(file)) {
        error = "that file is not in the BuffrX library";
        return Json::object();
      }
    }
    if (files.empty() || !copy_files_to_clipboard(files)) {
      error = "the clipboard could not be set";
      return Json::object();
    }
    return Json::object();
  }
  if (command.rfind("link.", 0) == 0) {
    // BuffrX links (link.h): app-to-app sharing through the sharing helper. Outside the engine lock;
    // only the two settings it needs are read under it.
    if (!links_) {
      error = "links are not available";
      return Json::object();
    }
    Json data = Json::object();
    const std::string id = args["id"].as_string();
    if (command == "link.share") {
      links_->share(args["file"].as_string(), static_cast<int>(args["minutes"].as_int(60)), data, error);
      // The long link is already complete and in `data`. A short one is an improvement on it, never a
      // prerequisite: every failure leaves the reply exactly as the share made it.
      if (error.empty()) {
        // A reused link may already be short: only a link page link that still carries its ticket can be
        // shortened, or the service is asked about something it cannot read and the sheet says so for no
        // reason.
        const std::string long_url = data["url"].as_string();
        const std::string fragment = link_fragment(long_url);
        if (!fragment.empty()) {
          const ShortLink attempt = shorten_link(long_url, data["expires"].as_int());
          if (attempt.url != long_url) {
            links_->set_short_url(data["id"].as_string(), attempt.url, fragment);
            data.set("url", Json(attempt.url));
          }
          if (!attempt.note.empty()) data.set("note", Json(attempt.note));
        }
      }
    } else if (command == "link.copy") {
      const Json state = links_->list();
      std::string url;
      for (const Json& link : state["links"].items()) {
        if (!id.empty() && link["id"].as_string() == id && link["expires"].as_int() > static_cast<int64_t>(std::time(nullptr))) {
          url = link["url"].as_string();
          break;
        }
      }
      if (url.empty()) error = "that link is no longer live";
      else {
        // The native clipboard watcher must recognise our own link before Windows reports the change.
        if (broadcaster) broadcaster(make_event("link.changed", state));
        if (!copy_text_to_clipboard(url)) error = "the clipboard could not be set";
      }
    } else if (command == "link.stop") {
      links_->stop(id, error);
    } else if (command == "link.list") {
      data = links_->list();
    } else if (command == "link.open") {
      std::string clips_dir;
      std::string screenshots_dir;
      {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        clips_dir = config_.clips_dir;
        screenshots_dir = config_.screenshots_dir;
      }
      links_->open(args["url"].as_string(), clips_dir, screenshots_dir, data, error);
    } else if (command == "link.answer") {
      links_->answer(id, args["accept"].as_bool(false), error);
    } else if (command == "link.cancel") {
      links_->cancel(id, error);
    } else {
      error = "unknown command: " + command;
    }
    return error.empty() ? data : Json::object();
  }
  if (command.rfind("share.", 0) == 0) {
    // Sharing (share.h): a clip made to fit an upload limit, copied or posted to the user's webhook.
    // Outside the engine lock: it reads files and starts an export process, never libobs.
    if (!share_) {
      error = "sharing is not available";
      return Json::object();
    }
    Json data = Json::object();
    const std::string id = args["id"].as_string();
    if (command == "share.prepare") {
      int limit = 0;
      {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        limit = config_.share_limit_mib;
      }
      // The same bounds the stored setting is kept in: a request may ask for a different fit, not for a
      // limit the sheet could never show or one too small to hold any picture.
      if (args.has("limit_mib")) limit = std::clamp(static_cast<int>(args["limit_mib"].as_int(limit)), 5, 500);
      share_->prepare(args["file"].as_string(), limit, args["fit"].as_bool(false), data, error);
    } else if (command == "share.get") {
      share_->get(id, data, error);
    } else if (command == "share.copy") {
      share_->copy(id, error);
    } else if (command == "share.discord") {
      share_->post_discord(id, args["message"].as_string(), args["webhook_id"].as_string(), data, error);
    } else if (command == "share.cancel") {
      share_->cancel(id, error);
    } else if (command == "share.webhook.get") {
      data = share_->webhook_json();
    } else if (command == "share.webhook.set") {
      if (share_->set_webhook(args["url"].as_string(), args["name"].as_string(), args["add"].as_bool(false), error)) data = share_->webhook_json();
    } else if (command == "share.webhook.clear") {
      if (share_->clear_webhook(args["webhook_id"].as_string(), error)) data = share_->webhook_json();
    } else {
      error = "unknown command: " + command;
    }
    return error.empty() ? data : Json::object();
  }
  if (command == "library.notify") {
    // The editor wrote a file into the library: every interface lists it again.
    if (broadcaster) broadcaster(make_event("library.changed", Json::object()));
    return Json::object();
  }
  if (command == "app.pick_folder") {
    Json data = Json::object();
    data.set("path", Json(pick_folder(args["title"].as_string("Choose a folder"), args["start"].as_string())));
    return data;
  }
  if (command == "app.pick_file") {
    Json data = Json::object();
    data.set("path", Json(pick_executable(args["title"].as_string("Choose a program"), args["start"].as_string())));
    return data;
  }

  if (command == "hotkeys.get") {
    return hotkeys_json(config_, window_.hotkeys(), pad_problems_, pad_poller_.connected(),
                        pad_poller_.available(), window_.hotkeys_suspended());
  }
  if (command == "hotkeys.capture") {
    // While the recorder listens, a pad press is a chord to show, not an action to run.
    pad_poller_.set_capturing(args["on"].as_bool(true));
    return Json::object();
  }
  if (command == "pad.navigate") {
    // The interface is in front: the pad moves it, and the poller runs for that alone.
    pad_poller_.set_navigating(args["on"].as_bool(true));
    return Json::object();
  }
  if (command == "hotkeys.suspend") {
    window_.suspend_hotkeys(args["suspended"].as_bool(true));
    return Json::object();
  }
  if (command == "hotkeys.set") {
    const std::string action = args["action"].as_string();
    const std::string chord = args["chord"].as_string();
    const bool controller = args["input"].as_string() == "controller";
    // Named before the chord is judged: a caller that sent a config field name instead of an action was
    // told "a chord needs a modifier and a key", which describes nothing it got wrong.
    if (!known_hotkey_action(action)) {
      error = "unknown hotkey action: " + action;
      return Json::object();
    }
    if (controller) {
      // An empty chord clears the binding; that is a valid state, not a bad one.
      if (chord.empty()) {
        set_hotkey_chord(config_, action, true, std::string());
      } else {
        std::string problem;
        const std::optional<uint32_t> combo = parse_pad_chord(chord, problem);
        if (!combo) {
          error = problem;
          return Json::object();
        }
        for (const std::string& other : all_hotkey_actions()) {
          if (action == other) continue;
          const std::string other_chord = pad_chord_for(config_, other);
          if (other_chord.empty()) continue;
          std::string ignored;
          const std::optional<uint32_t> other_combo = parse_pad_chord(other_chord, ignored);
          if (!other_combo || !pad_chords_conflict(*other_combo, *combo)) continue;
          error = "overlaps " + hotkey_action_name(other);
          return Json::object();
        }
        set_hotkey_chord(config_, action, true, format_pad_chord(*combo));
      }
    } else {
      if (!chord.empty()) {
        UINT modifiers = 0;
        UINT key = 0;
        std::string problem;
        if (!parse_chord(chord, modifiers, key, problem)) {
          error = problem;
          return Json::object();
        }
      }
      set_hotkey_chord(config_, action, false, chord);
    }
    apply_hotkeys();
    if (!persist(error)) BX_LOG_WARN("settings could not be written: %s", error.c_str());
    return hotkeys_json(config_, window_.hotkeys(), pad_problems_, pad_poller_.connected(),
                        pad_poller_.available(), window_.hotkeys_suspended());
  }

  if (command == "devices.list") {
    Json data = Json::object();
    Json outputs = Json::array();
    for (const auto& device : host_.audio_devices(false)) {
      Json entry = Json::object();
      entry.set("id", Json(device.id));
      entry.set("name", Json(device.name));
      entry.set("default", Json(device.is_default));
      outputs.push(entry);
    }
    Json inputs = Json::array();
    for (const auto& device : host_.audio_devices(true)) {
      Json entry = Json::object();
      entry.set("id", Json(device.id));
      entry.set("name", Json(device.name));
      entry.set("default", Json(device.is_default));
      inputs.push(entry);
    }
    data.set("outputs", outputs);
    data.set("inputs", inputs);
    return data;
  }
  if (command == "audio.get") return audio_json(config_);
  if (command == "audio.set") {
    Json patch = Json::object();
    patch.set("audio", args);
    apply_patch(patch, error);
    if (!error.empty()) return Json::object();
    return audio_json(config_);
  }

  if (command == "profiles.list") return profiles_json();
  if (command == "profiles.add") {
    Json profiles = profiles_json();
    if (profiles.items().size() >= 4) {
      error = "up to four profiles";  // the panel's 1-4 chips
      return Json::object();
    }
    Json entry = Json::object();
    entry.set("patch", profile_patch(config_json()));
    profiles.push(entry);
    if (!write_profiles(profiles, error)) return Json::object();
    return profiles;
  }
  if (command == "profiles.update") {
    // {index, patch} merges capture/encoding settings into it.
    Json profiles = profiles_json();
    const int64_t index = args["index"].as_int(-1);
    if (index < 0 || index >= static_cast<int64_t>(profiles.items().size())) {
      error = "no such profile";
      return Json::object();
    }
    Json updated = Json::array();
    int64_t at = 0;
    for (const auto& profile : profiles.items()) {
      Json entry = profile;
      if (at == index) {
        Json patch = profile["patch"].is_object() ? profile["patch"] : Json::object();
        for (const auto& [key, value] : profile_patch(args["patch"]).fields()) patch.set(key, value);
        entry.set("patch", patch);
      }
      updated.push(entry);
      ++at;
    }
    if (!write_profiles(updated, error)) return Json::object();
    // Editing the profile in force applies the edit (Rewind's behaviour).
    if (index == config_.active_profile) {
      apply_patch(profile_patch(args["patch"]), error);
      if (!error.empty()) return Json::object();
    }
    return updated;
  }
  if (command == "profiles.save") {
    Json profiles = profiles_json();
    const int64_t index = args["index"].as_int(-1);
    if (index < 0 || index >= static_cast<int64_t>(profiles.items().size())) {
      error = "no such profile";
      return Json::object();
    }
    Json updated = Json::array();
    int64_t at = 0;
    for (const auto& profile : profiles.items()) {
      Json entry = profile;
      if (at == index) entry.set("patch", profile_patch(config_json()));
      updated.push(entry);
      ++at;
    }
    if (!write_profiles(updated, error)) return Json::object();
    return updated;
  }
  if (command == "profiles.remove") {
    Json profiles = profiles_json();
    const int64_t index = args["index"].as_int(-1);
    Json updated = Json::array();
    int64_t at = 0;
    if (index < 0 || index >= static_cast<int64_t>(profiles.items().size())) {
      error = "no such profile";
      return Json::object();
    }
    if (profiles.items().size() <= 1) {
      error = "the last profile stays";
      return Json::object();
    }
    for (const auto& profile : profiles.items()) {
      if (at != index) updated.push(profile);
      ++at;
    }
    if (!write_profiles(updated, error)) return Json::object();
    // The active index follows the list: removing the active one leaves none active.
    if (config_.active_profile == index) config_.active_profile = -1;
    else if (config_.active_profile > index) --config_.active_profile;
    if (!persist(error)) return Json::object();
    return updated;
  }
  if (command == "profiles.apply") {
    Json profiles = profiles_json();
    const int64_t index = args["index"].as_int(-1);
    if (index < 0 || index >= static_cast<int64_t>(profiles.items().size())) {
      error = "no such profile";
      return Json::object();
    }
    const Json& profile = profiles.items()[static_cast<size_t>(index)];
    apply_patch(profile_patch(profile["patch"]), error);
    if (!error.empty()) return Json::object();
    config_.active_profile = static_cast<int>(index);
    if (!persist(error)) BX_LOG_WARN("settings could not be written: %s", error.c_str());
    error.clear();
    mark(command, "profile " + std::to_string(index + 1));
    return status_json();
  }

  if (command == "ui.show") {
    // From a second interface launch (or any client): ask the running interface to bring a view up.
    const std::string view = args["view"].as_string("main");
    if (view == "timeline") {
      // The Timeline preview is a process of its own, not a view of this window: this is the press Alt+4 makes (open on
      // the newest footage or on its empty state, close one that is open), so the interface's TIMELINE entry and the
      // hotkey can never disagree about what the key does.
      timeline_pressed();
      Json data = Json::object();
      data.set("view", Json(view));
      return data;
    }
    if (view != "main" && view != "tray" && view != "compact" && view != "hide") {
      error = "unknown view: " + view;
      return Json::object();
    }
    Json data = Json::object();
    data.set("view", Json(view));
    if (broadcaster) broadcaster(make_event("ui.show", data));
    return data;
  }
  if (command == "ui.share") {
    // The editor's clip preview asks for the Share sheet. The native panel draws it, so the request
    // becomes an event the interface acts on, the way ui.show does. Library files only: the sheet
    // fits and sends the file it is given.
    const std::string file = args["file"].as_string();
    if (!library_owns_path(file)) {
      error = "that file is not in the BuffrX library";
      return Json::object();
    }
    Json data = Json::object();
    data.set("file", Json(file));
    if (broadcaster) broadcaster(make_event("ui.share", data));
    return data;
  }
  if (command == "ui.capture") {
    // A panel regression is checked without hands: the view is drawn by the interface itself and saved.
    const std::string view = args["view"].as_string("main");
    const std::string file = args["file"].as_string();
    if (file.empty()) {
      error = "a file to write is needed";
      return Json::object();
    }
    // The probe's output is a PNG and nothing else: a command that writes a file must not write any
    // other kind of file to any path it is handed.
    const size_t slash = file.find_last_of("/\\");
    const size_t dot = file.find_last_of('.');
    std::string extension;
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) extension = file.substr(dot);
    for (char& character : extension) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (extension != ".png") {
      error = "the capture file must end in .png";
      return Json::object();
    }
    if (!ui_capturer) {
      error = "the interface is not running";
      return Json::object();
    }
    std::string capture_error;
    if (!ui_capturer(view, file, capture_error)) {
      error = "the interface could not be captured: " + capture_error;
      return Json::object();
    }
    Json data = Json::object();
    data.set("file", Json(file));
    return data;
  }
  if (command == "ui.input") {
    std::string input_error;
    if (!ui_inputter || !ui_inputter(args, input_error)) {
      error = input_error.empty() ? std::string("the interface is not running") : input_error;
      return Json::object();
    }
    return Json::object();
  }
  if (command == "ui.frame_stats") {
    if (!ui_frame_stats) {
      error = "the interface is not running";
      return Json::object();
    }
    return ui_frame_stats(args["reset"].as_bool());
  }
  if (command == "editor.open") {
    std::vector<std::string> files;
    for (const auto& item : args["files"].items()) {
      if (!item.as_string().empty()) files.push_back(item.as_string());
    }
    if (files.empty() && !args["file"].as_string().empty()) files.push_back(args["file"].as_string());
    // The same rule every other command that names a file follows: a client may open what the library owns,
    // never a path of its own choosing. Everything else about the editor is handed over untouched.
    for (const auto& file : files) {
      if (!library_owns_path(file)) {
        error = "that file is not in the BuffrX library";
        return Json::object();
      }
    }
    // No files opens the editor empty, ready for a drop. An editor that is already open gets the files
    // instead of a second window starting. `preview`: a clip clicked in the panel plays first.
    const bool preview = args["preview"].as_bool();
    if (editor_router && editor_router(files, preview)) {
      Json data = Json::object();
      data.set("forwarded", Json(true));
      return data;
    }
    DWORD pid = 0;
    if (!launch_editor(files, data_root_, pid, preview)) {
      error = "the editor could not be started";
      return Json::object();
    }
    Json data = Json::object();
    data.set("pid", Json(static_cast<int64_t>(pid)));
    return data;
  }

  if (command == "app.open_url") {
    // Only the web opens here: a link the interface shows is http(s), and nothing else is a "link".
    const std::string url = args["url"].as_string();
    if (!is_public_link(url)) {
      error = "that address is not a BuffrX link";
      return Json::object();
    }
    if (reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
      error = "the address did not open";
      return Json::object();
    }
    return Json::object();
  }

  if (command == "update.notes") {
    // The version is the running engine's own, never a caller-supplied URL.
    const std::string version = kEngineVersion;
    UpdateFeed feed = fetch_update_feed("https://api.github.com/repos/bulhaa1/BuffrX/releases/tags/" + version,
                                        5000, std::string("BuffrX/") + version);
    if (!feed.ok && feed.error == "no release published yet")
      feed = fetch_update_feed("https://api.github.com/repos/bulhaa1/BuffrX/releases/tags/v" + version,
                               5000, std::string("BuffrX/") + version);
    Json result = Json::object();
    result.set("found", Json(feed.ok));
    result.set("version", Json(version));
    result.set("notes", Json(feed.notes));
    result.set("error", Json(feed.ok ? std::string() : feed.error));
    return result;
  }

  if (command == "update.check") {
    // One request to GitHub on the worker that already serves this command. The panel keeps its own frame.
    const int64_t now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::system_clock::now().time_since_epoch()).count());
    Json reply;
    {
      std::lock_guard<std::mutex> update_lock(update_mutex_);
      if (!args["force"].as_bool(false) && !update_cache_.is_null() &&
          !update_check_due(update_cache_["checked_ms"].as_int(), now)) {
        reply = update_cache_;
      }
    }
    if (reply.is_null()) {
      const UpdateFeed feed = fetch_update_feed(update_feed_url(), 5000, std::string("BuffrX/") + kEngineVersion);
      Json data = Json::object();
      data.set("found", Json(feed.ok));
      data.set("latest", Json(feed.latest));
      data.set("notes", Json(feed.notes));
      data.set("url", Json(feed.url));
      data.set("digest", Json(feed.digest));
      data.set("error", Json(feed.error));
      data.set("checked_ms", Json(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count())));
      {
        std::lock_guard<std::mutex> update_lock(update_mutex_);
        update_cache_ = data;
      }
      reply = data;
    }
    // The fetch is over before this: the download is a thread of its own and a cached answer starts it too,
    // so an app that stayed open overnight still picks the release up on the next panel open.
    maybe_start_update_download(reply);
    reply.set("install", update_json());
    return reply;
  }

  if (command == "update.install") {
    // Restart now: verify the file once more, then let the main loop stop the outputs and close. The
    // installer is started from the action worker, after the writing has stopped.
    if (!kAutoUpdate) {
      error = "this build does not install updates by itself";
      return Json::object();
    }
    {
      std::lock_guard<std::mutex> lock(update_mutex_);
      if (update_running_) {
        error = "the update is still downloading";
        return Json::object();
      }
    }
    PendingUpdate pending;
    std::string pending_error;
    if (!update_pending_read(data_root_, pending, pending_error)) {
      error = pending_error;
      return Json::object();
    }
    if (update_install_exhausted(pending.install_attempts)) {
      error = "the update did not install after three tries; open the release page to install it by hand";
      return Json::object();
    }
    std::string verify_error;
    if (!update_pending_verify(pending, verify_error)) {
      error = verify_error.empty() ? "the downloaded update did not check out" : verify_error;
      DeleteFileA(pending.setup.c_str());
      update_pending_clear(data_root_);
      set_update_state("failed", error + ". Try again to download it again.");
      return Json::object();
    }
    // `update.install` needs the engine lock, so `handle` already holds it here: locking it again would
    // deadlock the command worker.
    if (host_.recording_busy()) {
      error = "stop the recording first: the update will wait until it ends";
      return Json::object();
    }
    if (!action_queue || !action_queue(QueuedAction{"install_update"})) {
      error = "BuffrX is busy. Try restarting for the update again.";
      return Json::object();
    }
    Json accepted = Json::object();
    accepted.set("accepted", Json(true));
    return accepted;
  }

  if (command == "app.open_folder") {
    const std::string kind = args["kind"].as_string("clips");
    // A folder that does not exist yet (Recordings is made by the first recording) is created first: ShellExecute on a
    // missing path opens Explorer on the engine's own working directory instead.
    const auto open_made = [](const std::string& path) {
      CreateDirectoryA(path.c_str(), nullptr);
      open_folder(path);
    };
    if (kind == "clips") open_made(config_.clips_dir);
    else if (kind == "recordings") open_made(recordings_folder(config_));
    else if (kind == "screenshots") open_made(config_.screenshots_dir);
    else if (kind == "data") open_folder(data_root_);
    else if (kind == "logs") open_folder(data_root_ + "\\logs");
    else if (kind == "timeline") {
      // The Timeline's own store: a session's media folder when one is named, else the store's root. Never
      // a path the caller built, so the argument cannot reach outside the store.
      const std::string session = args.has("session") ? args["session"].as_string() : "";
      if (!session.empty() && !session_id_is_safe(session)) {
        error = "that is not a timeline session";
        return Json::object();
      }
      open_folder(session.empty() ? session_store_root() : session_media_dir(session));
    } else {
      error = "unknown folder";
      return Json::object();
    }
    return Json::object();
  }

  if (command == "engine.restart") {
    // A recording or a Timeline session is being written, and a restart tears libobs down (recovery_policy.h). The
    // person asked for the restart, so it is refused with a reason rather than blocking on a question nobody sees.
    // The rebuild itself is handed to the action worker (main.cpp): doing it here held the engine lock for the whole
    // shutdown and initialize, so the reply came only after it - and so did the tick, the hotkeys and every other
    // client. The worker rebuilds, starts the buffer again if it was running, and its tail broadcasts `status`.
    if (!action_queue) {
      error = "the engine cannot restart itself right now";
      return Json::object();
    }
    if (!request_restart(host_.recording_busy(), host_.timeline_writer_active(), host_.stats().buffer_active,
                         action_queue, error)) {
      return Json::object();
    }
    Json data = Json::object();
    data.set("accepted", Json(true));
    return data;
  }

  error = "unknown command: " + command;
  return Json::object();
}

}  // namespace bx

