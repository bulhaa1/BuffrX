#include "config.h"
#include "encoder_governor.h"
#include "encoder_settings.h"
#include "game_watch.h"
#include "media_math.h"
#include "performance.h"
#include "themes.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>

#include "../shared/files.h"
#include "../shared/log.h"

namespace bx {
namespace {

bool directory_exists(const std::string& path) {
  DWORD attributes = GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// A directory that merely exists may still be unwritable: a copy unzipped into Program Files passes an
// existence check and then every write fails. Ask for a real write before calling it usable.
bool directory_writable(const std::string& path) {
  const std::string probe = path + "\\.buffrx-write";
  HANDLE file = CreateFileA(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);  // FILE_FLAG_DELETE_ON_CLOSE removes the probe
  return true;
}

bool ensure_directory(const std::string& path) {
  if (path.empty()) return false;
  if (directory_exists(path)) return directory_writable(path);
  std::string partial;
  for (size_t i = 0; i < path.size(); ++i) {
    partial.push_back(path[i]);
    const bool last = i + 1 == path.size();
    if (path[i] == '\\' || path[i] == '/' || last) {
      if (partial.size() <= 3) continue;  // skip "C:\" style prefixes
      if (!directory_exists(partial) && !CreateDirectoryA(partial.c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
      }
    }
  }
  return directory_exists(path) && directory_writable(path);
}

std::string executable_directory() {
  char buffer[MAX_PATH] = {0};
  DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  std::string path(buffer, length);
  size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool read_known_folder(REFKNOWNFOLDERID id, std::string& out) {
  PWSTR wide = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &wide)) || !wide) return false;
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    // The conversion found nothing to write, so there is no buffer to fill: `&value[0]` on an empty
    // string would be out of bounds.
    CoTaskMemFree(wide);
    return false;
  }
  std::string value(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, &value[0], needed, nullptr, nullptr);
  CoTaskMemFree(wide);
  if (!value.empty() && value.back() == '\0') value.pop_back();
  out = value;
  return !out.empty();
}

Json strings_to_json(const std::vector<std::string>& values) {
  Json json = Json::array();
  for (const std::string& value : values) json.push(Json(value));
  return json;
}

void strings_from_json(const Json& json, std::vector<std::string>& values) {
  values.clear();
  for (const Json& item : json.items()) {
    if (item.is_string()) values.push_back(item.as_string());
  }
}

}  // namespace

bool resolve_data_root(std::string& data_root, bool& portable) {
  const std::string exe_dir = executable_directory();
  const std::string marker = exe_dir + "\\portable.flag";
  if (directory_exists(exe_dir) && GetFileAttributesA(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
    portable = true;
    data_root = exe_dir + "\\data";
    return ensure_directory(data_root);
  }
  portable = false;
#ifdef _WIN32
  char local[MAX_PATH] = {0};
  if (GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH) == 0) return false;
  data_root = std::string(local) + "\\BuffrX";
#else
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  if (xdg && *xdg) data_root = std::string(xdg) + "/buffrx";
  else if (home && *home) data_root = std::string(home) + "/.local/share/buffrx";
  else return false;
#endif
  return ensure_directory(data_root);
}

void apply_path_defaults(Config& config, const std::string& data_root) {
  std::string videos;
  if (!read_known_folder(FOLDERID_Videos, videos)) videos = data_root + "\\media";
  if (config.clips_dir.empty()) config.clips_dir = videos + "\\BuffrX";
  if (config.screenshots_dir.empty()) config.screenshots_dir = config.clips_dir + "\\Screenshots";
}

CanvasPlan plan_canvas(const Config& config, int target_width, int target_height) {
  // A size counts only as a complete pair of positive dimensions: half a size is a mistake, not a
  // request for the other half to be guessed.
  const bool target_known = target_width > 0 && target_height > 0;
  const bool configured = config.output_width > 0 && config.output_height > 0;

  CanvasPlan plan;
  if (target_known) {
    plan.canvas_width = target_width;
    plan.canvas_height = target_height;
  } else if (configured) {
    plan.canvas_width = config.output_width;
    plan.canvas_height = config.output_height;
  } else {
    plan.canvas_width = 1920;
    plan.canvas_height = 1080;
  }
  plan.output_width = configured ? config.output_width : plan.canvas_width;
  plan.output_height = configured ? config.output_height : plan.canvas_height;
  return plan;
}

void encoder_size(int output_width, int output_height, int canvas_width, int canvas_height, int& width, int& height) {
  // The configured output when both halves are there, else the canvas the video thread scales to.
  if (output_width > 0 && output_height > 0) {
    width = output_width;
    height = output_height;
  } else {
    width = canvas_width > 0 ? canvas_width : 1920;
    height = canvas_height > 0 ? canvas_height : 1080;
  }
}

void encoder_size(const Config& config, int canvas_width, int canvas_height, int& width, int& height) {
  encoder_size(config.output_width, config.output_height, canvas_width, canvas_height, width, height);
}

std::string capture_window_target(const Config& config) {
  if (config.capture_mode == "window") return config.window_id;
  if (config.capture_mode == "game") return config.game_title;
  return {};
}

bool capture_fills_canvas(const std::string& stretch_mode, int source_width, int source_height, int canvas_width,
                          int canvas_height) {
  if (source_width <= 0 || source_height <= 0 || canvas_width <= 0 || canvas_height <= 0) return false;
  // "stretch" is the person's own choice, the only one: the picture is resized to the canvas' shape
  // whatever that does to it. "auto" and "fit" keep the picture's own shape (scale inner, centred):
  // never stretched, never cropped, so a 21:9 game on a 16:9 canvas is letterboxed.
  return stretch_mode == "stretch";
}

std::string recordings_base_dir(const Config& config) {
  return config.recordings_dir.empty() ? config.clips_dir : config.recordings_dir;
}

std::string recordings_folder(const Config& config) { return recordings_base_dir(config) + "\\" + kRecordingsFolder; }

std::string sessions_dir(const Config& config) { return recordings_base_dir(config) + "\\sessions"; }

Json config_to_json(const Config& c) {
  Json json = Json::object();
  json.set("capture_mode", Json(c.capture_mode));
  json.set("display_id", Json(c.display_id));
  json.set("display_name", Json(c.display_name));
  json.set("window_id", Json(c.window_id));
  json.set("window_name", Json(c.window_name));
  json.set("game_title", Json(c.game_title));
  json.set("capture_fps", Json(c.capture_fps));
  json.set("output_width", Json(c.output_width));
  json.set("output_height", Json(c.output_height));
  json.set("capture_cursor", Json(c.capture_cursor));
  json.set("stretch_mode", Json(c.stretch_mode));
  json.set("codec", Json(c.codec));
  json.set("encoder", Json(c.encoder));
  json.set("quality", Json(c.quality));
  json.set("peak_mode", Json(c.peak_mode));
  json.set("custom_kbps", Json(c.custom_kbps));
  json.set("video_preset", Json(c.video_preset));
  json.set("keyframe_seconds", Json(c.keyframe_seconds));
  json.set("encoder_speed_step", Json(c.encoder_speed_step));
  json.set("encoder_mode", Json(c.encoder_mode));
  json.set("display_method", Json(c.display_method));
  json.set("scale_filter", Json(c.scale_filter));
  json.set("encoder_options", c.encoder_options);
  json.set("muxer_settings", Json(c.muxer_settings));
  json.set("buffer_seconds", Json(c.buffer_seconds));
  json.set("quick_clip_seconds", Json(c.quick_clip_seconds));
  json.set("timeline_minutes", Json(c.timeline_minutes));
  json.set("timeline_keep_hours", Json(c.timeline_keep_hours));
  json.set("timeline_windowed_games", Json(c.timeline_windowed_games));
  json.set("timeline_everywhere", Json(c.timeline_everywhere));
  json.set("timeline_idle_minutes", Json(c.timeline_idle_minutes));
  json.set("start_buffering", Json(c.start_buffering));
  json.set("desktop_enabled", Json(c.desktop_enabled));
  json.set("desktop_volume", Json(c.desktop_volume));
  json.set("desktop_muted", Json(c.desktop_muted));
  json.set("desktop_device", Json(c.desktop_device));
  json.set("mic_enabled", Json(c.mic_enabled));
  json.set("mic_volume", Json(c.mic_volume));
  json.set("mic_gain_db", Json(c.mic_gain_db));
  json.set("mic_muted", Json(c.mic_muted));
  json.set("mic_device", Json(c.mic_device));
  json.set("mic_noise_suppress", Json(c.mic_noise_suppress));
  json.set("mic_noise_gate", Json(c.mic_noise_gate));
  json.set("separate_tracks", Json(c.separate_tracks));
  json.set("audio_tracks", Json(c.audio_tracks));
  json.set("audio_bitrate_kbps", Json(c.audio_bitrate_kbps));
  json.set("hotkey_save", Json(c.hotkey_save));
  json.set("hotkey_quick", Json(c.hotkey_quick));
  json.set("hotkey_screenshot", Json(c.hotkey_screenshot));
  json.set("hotkey_record", Json(c.hotkey_record));
  json.set("hotkey_toggle_ui", Json(c.hotkey_toggle_ui));
  json.set("hotkey_view_clip", Json(c.hotkey_view_clip));
  json.set("hotkey_marker", Json(c.hotkey_marker));
  json.set("hotkey_save_pad", Json(c.hotkey_save_pad));
  json.set("hotkey_quick_pad", Json(c.hotkey_quick_pad));
  json.set("hotkey_screenshot_pad", Json(c.hotkey_screenshot_pad));
  json.set("hotkey_record_pad", Json(c.hotkey_record_pad));
  json.set("hotkey_toggle_ui_pad", Json(c.hotkey_toggle_ui_pad));
  json.set("hotkey_view_clip_pad", Json(c.hotkey_view_clip_pad));
  json.set("hotkey_marker_pad", Json(c.hotkey_marker_pad));
  json.set("clips_dir", Json(c.clips_dir));
  json.set("screenshots_dir", Json(c.screenshots_dir));
  json.set("recordings_dir", Json(c.recordings_dir));
  json.set("theme", Json(c.theme));
  json.set("ui_scale_percent", Json(c.ui_scale_percent));
  json.set("filename_format", Json(c.filename_format));
  json.set("screenshot_format", Json(c.screenshot_format));
  json.set("container", Json(c.container));
  json.set("share_limit_mib", Json(c.share_limit_mib));
  json.set("share_link_page", Json(c.share_link_page));
  json.set("share_sender_name", Json(c.share_sender_name));
  json.set("share_upload_kbps", Json(c.share_upload_kbps));
  json.set("notifications", Json(c.notifications));
  json.set("notification_sound", Json(c.notification_sound));
  json.set("notify_sound_while_recording", Json(c.notify_sound_while_recording));
  json.set("notify_over_fullscreen", Json(c.notify_over_fullscreen));
  json.set("performance_when", Json(performance_when_name(c.performance_when)));
  json.set("performance_games", strings_to_json(c.performance_games));
  json.set("launch_on_startup", Json(c.launch_on_startup));
  json.set("start_minimized", Json(c.start_minimized));
  json.set("run_as_admin", Json(c.run_as_admin));
  json.set("pause_on_lock", Json(c.pause_on_lock));
  json.set("pause_on_battery", Json(c.pause_on_battery));
  json.set("crash_recovery", Json(c.crash_recovery));
  json.set("codec_auto_picked", Json(c.codec_auto_picked));
  json.set("chunk_seconds", Json(c.chunk_seconds));
  json.set("recordings_budget_gb", Json(c.recordings_budget_gb));
  json.set("min_free_gb", Json(c.min_free_gb));
  json.set("auto_game_capture", Json(c.auto_game_capture));
  json.set("game_exclusions", strings_to_json(c.game_exclusions));
  json.set("active_profile", Json(c.active_profile));
  json.set("config_version", Json(c.config_version));
  return json;
}

void config_apply_audio(const Json& j, Config& c) {
  if (!j.is_object()) return;
  if (j.has("desktop_enabled")) c.desktop_enabled = j["desktop_enabled"].as_bool(c.desktop_enabled);
  if (j.has("desktop_volume")) c.desktop_volume = j["desktop_volume"].as_double(c.desktop_volume);
  if (j.has("desktop_muted")) c.desktop_muted = j["desktop_muted"].as_bool(c.desktop_muted);
  if (j.has("desktop_device")) c.desktop_device = j["desktop_device"].as_string(c.desktop_device);
  if (j.has("mic_enabled")) c.mic_enabled = j["mic_enabled"].as_bool(c.mic_enabled);
  if (j.has("mic_volume")) c.mic_volume = j["mic_volume"].as_double(c.mic_volume);
  if (j.has("mic_gain_db")) c.mic_gain_db = static_cast<int>(j["mic_gain_db"].as_int(c.mic_gain_db));
  if (j.has("mic_muted")) c.mic_muted = j["mic_muted"].as_bool(c.mic_muted);
  if (j.has("mic_device")) c.mic_device = j["mic_device"].as_string(c.mic_device);
  if (j.has("separate_tracks")) c.separate_tracks = j["separate_tracks"].as_bool(c.separate_tracks);
  if (j.has("audio_tracks")) c.audio_tracks = static_cast<int>(j["audio_tracks"].as_int(c.audio_tracks));
  if (j.has("mic_noise_suppress")) c.mic_noise_suppress = j["mic_noise_suppress"].as_bool(c.mic_noise_suppress);
  if (j.has("mic_noise_gate")) c.mic_noise_gate = j["mic_noise_gate"].as_bool(c.mic_noise_gate);
  if (j.has("audio_bitrate_kbps")) c.audio_bitrate_kbps = static_cast<int>(j["audio_bitrate_kbps"].as_int(c.audio_bitrate_kbps));
  c.audio_tracks = std::clamp(c.audio_tracks, 1, 6);
  c.desktop_volume = std::clamp(c.desktop_volume, 0.0, 1.0);
  c.mic_volume = std::clamp(c.mic_volume, 0.0, 1.0);
  c.mic_gain_db = std::clamp(c.mic_gain_db, -12, 20);
}

void config_from_json(const Json& j, Config& c) {
  if (!j.is_object()) return;
  if (j.has("capture_mode")) {
    const std::string mode = j["capture_mode"].as_string(c.capture_mode);
    if (mode == "display" || mode == "window" || mode == "game") c.capture_mode = mode;
  }
  if (j.has("display_id")) c.display_id = static_cast<int>(j["display_id"].as_int(c.display_id));
  if (j.has("display_name")) c.display_name = j["display_name"].as_string(c.display_name);
  if (j.has("window_id")) c.window_id = j["window_id"].as_string(c.window_id);
  if (j.has("window_name")) c.window_name = j["window_name"].as_string(c.window_name);
  if (j.has("game_title")) c.game_title = j["game_title"].as_string(c.game_title);
  if (j.has("capture_fps")) c.capture_fps = static_cast<int>(j["capture_fps"].as_int(c.capture_fps));
  if (j.has("output_width")) c.output_width = static_cast<int>(j["output_width"].as_int(c.output_width));
  if (j.has("output_height")) c.output_height = static_cast<int>(j["output_height"].as_int(c.output_height));
  if (j.has("capture_cursor")) c.capture_cursor = j["capture_cursor"].as_bool(c.capture_cursor);
  if (j.has("stretch_mode")) {
    const std::string mode = j["stretch_mode"].as_string(c.stretch_mode);
    if (mode == "auto" || mode == "stretch" || mode == "fit") c.stretch_mode = mode;
  }
  if (j.has("codec")) {
    const std::string codec = j["codec"].as_string(c.codec);
    if (codec == "h264" || codec == "hevc" || codec == "av1") c.codec = codec;
  }
  if (j.has("encoder")) {
    // The vendor names the engine can choose from. An older config's "x264" names the encoder, not the
    // vendor, and mean the software one; anything else is dropped instead of becoming a request no
    // candidate answers (which silently fell back to the automatic vendor).
    const std::string vendor = j["encoder"].as_string(c.encoder);
    if (vendor == "auto" || vendor == "nvenc" || vendor == "amf" || vendor == "qsv" || vendor == "software") {
      c.encoder = vendor;
    } else if (vendor == "x264") {
      c.encoder = "software";
    }
  }
  // rate_control, save_accuracy and audio_codec were read here once and never consulted. They are
  // deliberately ignored rather than parsed: an older file carrying them loads without them.
  if (j.has("quality")) {
    const std::string level = j["quality"].as_string(c.quality);
    if (level == "low" || level == "standard" || level == "high" || level == "highest" || level == "custom") {
      c.quality = level;
    }
  } else if (j.has("quality_tier")) {
    // Version 3's tiers, wherever they still arrive from: a file, a saved profile, an older client.
    const std::string tier = j["quality_tier"].as_string();
    c.quality = tier == "performance" ? "standard" : tier == "quality" ? "highest" : "high";
  }
  if (j.has("custom_kbps")) c.custom_kbps = static_cast<int>(j["custom_kbps"].as_int(c.custom_kbps));
  // Version 3's explicit rate under "Fixed size" was the user's own choice, so it becomes custom. Under
  // constant quality the same field was only the ladder's cap, not a choice, and is left behind.
  if (!j.has("quality") && j["bitrate_kbps"].as_int(0) > 0 && j["duration_first"].as_bool(true)) {
    c.quality = "custom";
    c.custom_kbps = static_cast<int>(j["bitrate_kbps"].as_int(0));
  }
  if (j.has("peak_mode")) {
    const std::string mode = j["peak_mode"].as_string(c.peak_mode);
    if (mode == "auto" || mode == "fixed") c.peak_mode = mode;
  }
  // Version 4's Custom level: High, with the rate the user set as a fixed peak.
  if (c.quality == "custom") {
    c.quality = "high";
    if (c.custom_kbps > 0) c.peak_mode = "fixed";
  }
  // A file from before the presets has none: its rate and level decide (fixed or hand-built opens as Custom).
  c.video_preset = j.has("video_preset") ? j["video_preset"].as_string() : std::string();
  c.video_preset = video_preset_from_config(c);
  if (j.has("keyframe_seconds")) c.keyframe_seconds = static_cast<int>(j["keyframe_seconds"].as_int(c.keyframe_seconds));
  if (j.has("encoder_speed_step")) c.encoder_speed_step = static_cast<int>(j["encoder_speed_step"].as_int(c.encoder_speed_step));
  if (j.has("encoder_mode")) {
    const std::string mode = j["encoder_mode"].as_string(c.encoder_mode);
    if (mode == "auto" || mode == "fast" || mode == "balanced" || mode == "quality" || mode == "custom") c.encoder_mode = mode;
  }
  if (j.has("display_method")) {
    const std::string method = j["display_method"].as_string(c.display_method);
    if (method == "auto" || method == "dxgi" || method == "wgc") c.display_method = method;
  }
  if (j.has("scale_filter")) {
    const std::string filter = j["scale_filter"].as_string(c.scale_filter);
    if (filter == "bilinear" || filter == "bicubic" || filter == "lanczos") c.scale_filter = filter;
  }
  if (j["encoder_options"].is_object()) {
    // A blank value is a key that never worked (an integer list written as text): it is dropped, not kept as
    // a "change" that did nothing.
    Json kept = Json::object();
    for (const auto& [name, value] : j["encoder_options"].fields()) {
      if (value.is_string() && value.as_string().empty()) continue;
      kept.set(name, value);
    }
    c.encoder_options = kept;
  }
  if (j.has("muxer_settings")) c.muxer_settings = j["muxer_settings"].as_string(c.muxer_settings);
  if (j.has("buffer_seconds")) c.buffer_seconds = static_cast<int>(j["buffer_seconds"].as_int(c.buffer_seconds));
  if (j.has("quick_clip_seconds")) c.quick_clip_seconds = static_cast<int>(j["quick_clip_seconds"].as_int(c.quick_clip_seconds));
  if (j.has("timeline_minutes")) {
    // A window is one of four lengths. Anything else - a hand-edited 45, a negative - is off, never the
    // nearest window: the disk work is sized from this.
    const int minutes = static_cast<int>(j["timeline_minutes"].as_int(0));
    c.timeline_minutes = timeline_minutes_allowed(minutes) ? minutes : 0;
  }
  if (j.has("timeline_keep_hours")) {
    // The choices Settings offers; anything else keeps the value already in force, never a guess.
    const int hours = static_cast<int>(j["timeline_keep_hours"].as_int(c.timeline_keep_hours));
    if (hours == 0 || hours == 1 || hours == 6 || hours == 24 || hours == 168) c.timeline_keep_hours = hours;
  }
  if (j.has("timeline_windowed_games")) c.timeline_windowed_games = j["timeline_windowed_games"].as_bool(c.timeline_windowed_games);
  if (j.has("timeline_everywhere")) c.timeline_everywhere = j["timeline_everywhere"].as_bool(c.timeline_everywhere);
  if (j.has("timeline_idle_minutes")) {
    const int idle = static_cast<int>(j["timeline_idle_minutes"].as_int(c.timeline_idle_minutes));
    if (timeline_idle_minutes_allowed(idle)) c.timeline_idle_minutes = idle;
  }
  if (j.has("start_buffering")) c.start_buffering = j["start_buffering"].as_bool(c.start_buffering);
  config_apply_audio(j, c);
  if (j.has("run_as_admin")) c.run_as_admin = j["run_as_admin"].as_bool(c.run_as_admin);
  if (j.has("hotkey_save")) c.hotkey_save = j["hotkey_save"].as_string(c.hotkey_save);
  if (j.has("hotkey_quick")) c.hotkey_quick = j["hotkey_quick"].as_string(c.hotkey_quick);
  if (j.has("hotkey_screenshot")) c.hotkey_screenshot = j["hotkey_screenshot"].as_string(c.hotkey_screenshot);
  if (j.has("hotkey_record")) c.hotkey_record = j["hotkey_record"].as_string(c.hotkey_record);
  if (j.has("hotkey_toggle_ui")) c.hotkey_toggle_ui = j["hotkey_toggle_ui"].as_string(c.hotkey_toggle_ui);
  if (j.has("hotkey_view_clip")) c.hotkey_view_clip = j["hotkey_view_clip"].as_string(c.hotkey_view_clip);
  if (j.has("hotkey_marker")) c.hotkey_marker = j["hotkey_marker"].as_string(c.hotkey_marker);
  if (j.has("hotkey_save_pad")) c.hotkey_save_pad = j["hotkey_save_pad"].as_string(c.hotkey_save_pad);
  if (j.has("hotkey_quick_pad")) c.hotkey_quick_pad = j["hotkey_quick_pad"].as_string(c.hotkey_quick_pad);
  if (j.has("hotkey_screenshot_pad")) {
    c.hotkey_screenshot_pad = j["hotkey_screenshot_pad"].as_string(c.hotkey_screenshot_pad);
  }
  if (j.has("hotkey_record_pad")) c.hotkey_record_pad = j["hotkey_record_pad"].as_string(c.hotkey_record_pad);
  if (j.has("hotkey_toggle_ui_pad")) {
    c.hotkey_toggle_ui_pad = j["hotkey_toggle_ui_pad"].as_string(c.hotkey_toggle_ui_pad);
  }
  if (j.has("hotkey_view_clip_pad")) {
    c.hotkey_view_clip_pad = j["hotkey_view_clip_pad"].as_string(c.hotkey_view_clip_pad);
  }
  if (j.has("hotkey_marker_pad")) c.hotkey_marker_pad = j["hotkey_marker_pad"].as_string(c.hotkey_marker_pad);
  if (j.has("clips_dir")) c.clips_dir = j["clips_dir"].as_string(c.clips_dir);
  if (j.has("screenshots_dir")) c.screenshots_dir = j["screenshots_dir"].as_string(c.screenshots_dir);
  if (j.has("recordings_dir")) c.recordings_dir = j["recordings_dir"].as_string(c.recordings_dir);
  if (j.has("theme")) {
    const std::string theme = j["theme"].as_string(c.theme);
    const bool cockpit = theme == "auto" || theme == "green" || theme == "red" || theme == "blue";
    if (is_theme_id(theme) || cockpit) c.theme = theme;
  }
  if (j.has("ui_scale_percent")) {
    c.ui_scale_percent = static_cast<int>(std::clamp<int64_t>(j["ui_scale_percent"].as_int(c.ui_scale_percent), 75, 150));
  }
  if (j.has("filename_format")) c.filename_format = j["filename_format"].as_string(c.filename_format);
  if (j.has("screenshot_format")) c.screenshot_format = j["screenshot_format"].as_string("png") == "jpg" ? "jpg" : "png";
  if (j.has("container")) {
    const std::string container = j["container"].as_string(c.container);
    if (container == "mp4" || container == "mkv" || container == "mov") c.container = container;
  }
  if (j.has("share_limit_mib")) c.share_limit_mib = static_cast<int>(j["share_limit_mib"].as_int(c.share_limit_mib));
  if (j.has("share_link_page")) {
    const std::string page = j["share_link_page"].as_string(c.share_link_page);
    // An empty setting means "the default page", not "app-only links": an old config.json that stored
    // "" keeps producing clickable https links.
    if (!page.empty()) c.share_link_page = page;
  }
  if (j.has("share_sender_name")) c.share_sender_name = j["share_sender_name"].as_string(c.share_sender_name);
  if (j.has("share_upload_kbps")) c.share_upload_kbps = static_cast<int>(j["share_upload_kbps"].as_int(c.share_upload_kbps));
  if (j.has("notifications")) c.notifications = j["notifications"].as_bool(c.notifications);
  if (j.has("notification_sound")) c.notification_sound = j["notification_sound"].as_bool(c.notification_sound);
  if (j.has("notify_sound_while_recording")) c.notify_sound_while_recording = j["notify_sound_while_recording"].as_bool(c.notify_sound_while_recording);
  if (j.has("codec_auto_picked")) c.codec_auto_picked = j["codec_auto_picked"].as_bool(c.codec_auto_picked);
  if (j.has("notify_over_fullscreen")) c.notify_over_fullscreen = j["notify_over_fullscreen"].as_bool(c.notify_over_fullscreen);
  if (j.has("performance_games")) {
    std::vector<std::string> listed;
    strings_from_json(j["performance_games"], listed);
    c.performance_games.clear();
    for (const std::string& name : listed) {
      const std::string normal = normalized_exe_name(name);
      if (normal.empty() || std::find(c.performance_games.begin(), c.performance_games.end(), normal) != c.performance_games.end()) continue;
      c.performance_games.push_back(normal);
    }
  }
  if (j.has("performance_when")) {
    c.performance_when = parse_performance_when(j["performance_when"].as_string());
  } else if (j.has("performance_mode")) {
    // Older files: the switch was always-on, or the game list implied the automatic choice.
    c.performance_when = j["performance_mode"].as_bool(false) ? PerformanceWhen::always
                        : (c.performance_games.empty() ? PerformanceWhen::off : PerformanceWhen::games);
  }
  if (j.has("launch_on_startup")) c.launch_on_startup = j["launch_on_startup"].as_bool(c.launch_on_startup);
  if (j.has("start_minimized")) c.start_minimized = j["start_minimized"].as_bool(c.start_minimized);
  if (j.has("pause_on_lock")) c.pause_on_lock = j["pause_on_lock"].as_bool(c.pause_on_lock);
  if (j.has("pause_on_battery")) c.pause_on_battery = j["pause_on_battery"].as_bool(c.pause_on_battery);
  if (j.has("crash_recovery")) c.crash_recovery = j["crash_recovery"].as_bool(c.crash_recovery);
  if (j.has("chunk_seconds")) c.chunk_seconds = static_cast<int>(j["chunk_seconds"].as_int(c.chunk_seconds));
  // `sessions.prune` hands whatever is stored straight to the pruner, and the pruner reads zero as
  // unlimited: a negative budget would otherwise delete every recording but the newest.
  if (j.has("recordings_budget_gb"))
    c.recordings_budget_gb = static_cast<int>(
        std::clamp<int64_t>(j["recordings_budget_gb"].as_int(c.recordings_budget_gb), 0, 10000));
  if (j.has("min_free_gb")) c.min_free_gb = static_cast<int>(j["min_free_gb"].as_int(c.min_free_gb));
  if (j.has("auto_game_capture")) c.auto_game_capture = j["auto_game_capture"].as_bool(c.auto_game_capture);
  if (j.has("game_exclusions")) strings_from_json(j["game_exclusions"], c.game_exclusions);
  if (j.has("active_profile")) c.active_profile = static_cast<int>(j["active_profile"].as_int(c.active_profile));
  if (j.has("config_version")) c.config_version = static_cast<int>(j["config_version"].as_int(c.config_version));
  // Kept inside what the engine can do, whoever wrote the value. A buffer of 99999 s or 0 fps is not a
  // setting, it is a file someone edited by hand, and the engine must still start.
  c.buffer_seconds = std::clamp(c.buffer_seconds, kBufferSecondsMin, kBufferSecondsMax);
  c.quick_clip_seconds = std::clamp(c.quick_clip_seconds, 5, 1800);
  c.capture_fps = std::clamp(c.capture_fps, 1, 240);
  c.keyframe_seconds = std::clamp(c.keyframe_seconds, 1, 10);
  c.chunk_seconds = chunk_seconds_clamped(c.chunk_seconds, c.keyframe_seconds);
  c.min_free_gb = std::clamp(c.min_free_gb, 0, 1000);
  // The ladder goes past the encoder rungs: 4 is 60 fps and 5 is 30, and a machine that settled there must
  // start there again. Clamping to kMaxGovernorStep turned a remembered 4 or 5 into a rebuild of the same
  // encoder at the next launch, which threw away exactly what the memory was for.
  c.encoder_speed_step = std::clamp(c.encoder_speed_step, 0, buffrx::kMaxReliefStep);
  if (c.custom_kbps != 0) c.custom_kbps = std::clamp(c.custom_kbps, 1000, 200000);
  // A size no device can make fails every video variant in turn, and the engine then cannot start -
  // which is also when there is no interface left to fix the file with. Zero still means the native
  // size; else the side is capped at 8K and made even, which every encoder needs.
  c.output_width = std::clamp(c.output_width, 0, 7680) & ~1;
  c.output_height = std::clamp(c.output_height, 0, 7680) & ~1;
  c.share_limit_mib = std::clamp(c.share_limit_mib, 5, 500);
  c.share_upload_kbps = std::clamp(c.share_upload_kbps, 0, 1000000);
}

bool load_config(const std::string& path, Config& config, std::string& error) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return true;  // first run: defaults stand
  std::string text;
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
  std::fclose(file);
  Json json;
  if (!Json::parse(text, json, error)) {
    // The engine carries on with defaults and writes them back on the next change, which would throw
    // the user's own file away for good. A copy is kept beside it first, so the settings can be put
    // back by hand and the reason looked at. A second unreadable file does not go on top of the first:
    // that copy is what the person had before, and the only record of it.
    std::string kept = path + ".unreadable";
    for (int n = 2; n < 100 && GetFileAttributesA(kept.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) {
      kept = path + ".unreadable-" + std::to_string(n);
    }
    if (CopyFileA(path.c_str(), kept.c_str(), FALSE)) error += "; the file was kept as " + kept;
    return false;
  }
  config.config_version = 1;  // a file with no version predates versioning
  config_from_json(json, config);
  migrate_config(config);
  return true;
}

void migrate_config(Config& config) {
  if (config.config_version < 2) {
    if (config.container == "mkv") config.container = "mp4";
    if (config.hotkey_save == "Alt+C") config.hotkey_save = "Alt+1";
    if (config.hotkey_quick == "Alt+Z") config.hotkey_quick = "Ctrl+Alt+1";
    if (config.hotkey_screenshot == "Alt+S") config.hotkey_screenshot = "Alt+2";
    if (config.hotkey_record == "Alt+R") config.hotkey_record = "Alt+3";
  }
  // Version 5: the encoder page keeps only the rows that matter and its presets are modes. A file that carries its
  // own speed rows keeps them, as Custom, so nothing the person chose changes underneath them.
  if (config.config_version < 5 && config.encoder_options.is_object()) {
    for (const char* key : {"preset", "multipass", "lookahead", "adaptive_quantization", "tune", "quality", "target_usage", "rate_control"}) {
      if (config.encoder_options.has(key)) {
        config.encoder_mode = "custom";
        break;
      }
    }
  }
  // Version 3 has nothing to carry over: rate_control, save_accuracy and audio_codec were never read,
  // so dropping them loses no choice the user could act on. The next save writes them no more.
  // Version 6 adds the Timeline's window, off by default, so a version 5 file has nothing to carry over
  // either: the default (0) and a stored invalid value (0, in config_from_json) are already off.
  // Version 7 adds the crash-recovery tail, on by default, which a file that never had the key already reads.
  // Version 8 moves the two old shipped pad bindings (Record unbound, panel View+Menu) to the intended pair.
  if (config.config_version < 8 && config.hotkey_record_pad.empty() && config.hotkey_toggle_ui_pad == "Pad:VIEW+MENU") {
    config.hotkey_record_pad = "Pad:VIEW+MENU";
    config.hotkey_toggle_ui_pad = "Pad:Y+VIEW";
  }
  config.config_version = Config::kConfigVersion;
}

bool timeline_idle_minutes_allowed(int minutes) { return minutes == 0 || minutes == 5 || minutes == 15 || minutes == 30 || minutes == 60; }

bool timeline_minutes_allowed(int minutes) { return minutes == 0 || minutes == 30 || minutes == 60 || minutes == 120; }

int scale_type_for_filter(const std::string& filter) {
  // obs_scale_type: DISABLE 0, POINT 1, BICUBIC 2, BILINEAR 3, LANCZOS 4, AREA 5.
  if (filter == "lanczos") return 4;
  if (filter == "bilinear") return 3;
  return 2;
}

int capture_method_for(const std::string& method) {
  if (method == "dxgi") return 1;
  if (method == "wgc") return 2;
  return 0;
}

bool capture_rate_is_even(int capture_fps, int refresh_hz) {
  if (refresh_hz <= 0 || capture_fps <= 0) return true;
  if (capture_fps >= refresh_hz - 1) return true;
  for (int multiple = 1; multiple <= 16; ++multiple) {
    if (std::abs(refresh_hz - multiple * capture_fps) <= multiple) return true;
  }
  return false;
}

bool save_config(const std::string& path, const Config& config, std::string& error) {
  // A short write (a full disk) must never replace the good config with half a file: the next start
  // would fail to parse it and run on defaults, losing every setting. write_file_atomic checks the
  // write and deletes its temporary on any failure (src/shared/files.h).
  if (!write_file_atomic(path, config_to_json(config).dump(2))) {
    error = "cannot write config file";
    return false;
  }
  return true;
}

unsigned config_change_bits(const Config& before, const Config& after) {
  unsigned changed = 0;
  const auto note = [&changed](bool differs, unsigned bits) {
    if (differs) changed |= bits;
  };
  note(before.codec != after.codec, kConfigSpeedInput | kConfigRebuilds);
  note(before.encoder != after.encoder, kConfigSpeedInput | kConfigRebuilds);
  note(before.capture_fps != after.capture_fps, kConfigSpeedInput | kConfigRebuilds);
  note(before.output_width != after.output_width, kConfigSpeedInput | kConfigRebuilds);
  note(before.output_height != after.output_height, kConfigSpeedInput | kConfigRebuilds);
  note(before.quality != after.quality, kConfigSpeedInput | kConfigRebuilds);
  note(before.keyframe_seconds != after.keyframe_seconds, kConfigSpeedInput | kConfigRebuilds);
  note(before.encoder_mode != after.encoder_mode, kConfigSpeedInput | kConfigRebuilds);
  note(before.encoder_options.dump() != after.encoder_options.dump(), kConfigSpeedInput | kConfigRebuilds);
  note(before.scale_filter != after.scale_filter, kConfigRebuilds);
  note(before.encoder_speed_step != after.encoder_speed_step, kConfigRebuilds);
  note(before.custom_kbps != after.custom_kbps, kConfigRebuilds);
  note(before.peak_mode != after.peak_mode, kConfigRebuilds);
  note(before.buffer_seconds != after.buffer_seconds, kConfigRebuilds);
  note(before.separate_tracks != after.separate_tracks, kConfigRebuilds);
  note(before.muxer_settings != after.muxer_settings, kConfigRebuilds);
  // The container, the keyframe and the audio encoder settings are part of what the outputs are built
  // from (ObsHost::apply's encoder_changed and audio_changed): leaving them out of this table is what
  // let a settings change rebuild the encoder under a manual recording.
  note(before.container != after.container, kConfigRebuilds);
  note(before.audio_tracks != after.audio_tracks, kConfigRebuilds);
  note(before.audio_bitrate_kbps != after.audio_bitrate_kbps, kConfigRebuilds);
  return changed;
}

Json profile_patch(const Json& patch) {
  // A profile is how a game is captured and encoded (Rewind's: buffer length, frame rate, quality),
  // nothing else: applying one must never change hotkeys, devices, folders, notifications or the
  // game switching that applies it. An allow-list, so a new setting stays out unless it belongs here.
  static const char* const kProfileKeys[] = {
      "buffer_seconds", "quick_clip_seconds", "capture_fps",   "output_width",  "output_height",
      "stretch_mode",   "codec",              "encoder",       "quality",       "custom_kbps",
      "peak_mode",      "video_preset",
      "keyframe_seconds", "encoder_mode", "scale_filter",
      // Version 3's names, kept so a profile saved before quality levels still applies (config_from_json
      // translates them).
      "quality_tier",   "bitrate_kbps",       "duration_first", "encoder_options"};
  Json filtered = Json::object();
  for (const auto& [key, value] : patch.fields()) {
    for (const char* name : kProfileKeys) {
      if (key == name) {
        filtered.set(key, value);
        break;
      }
    }
  }
  return filtered;
}

}  // namespace bx
