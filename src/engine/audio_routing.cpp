#include "audio_routing.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "../shared/files.h"

#include "audio_layout.h"

namespace bx {
namespace {

std::string lower_ascii(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// "Discord.exe" -> "Discord", "cs2.exe" -> "cs2". The title a route was picked with is only the
// fallback: a window title changes with the channel or the level, an image name does not.
std::string app_display_name(const AudioApp& app) {
  if (audio_app_is_game(app)) return "Game";
  std::string name = app.exe;
  const size_t slash = name.find_last_of("\\/");
  if (slash != std::string::npos) name = name.substr(slash + 1);
  const std::string lower = lower_ascii(name);
  if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
  if (name.empty() || lower == "unknown") name = app.title;
  if (name.empty()) name = "Application";
  return name;
}

std::string join(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out += " + ";
    out += part;
  }
  return out;
}

}  // namespace

bool audio_app_is_game(const AudioApp& app) { return app.kind == kAudioGameKind; }

std::string audio_app_key(const AudioApp& app) {
  return audio_app_is_game(app) ? std::string(kAudioGameKind) : lower_ascii(app.exe);
}

double audio_app_volume(double value) {
  if (!std::isfinite(value)) return 1.0;
  return std::clamp(value, 0.0, 1.0);
}

AudioApp current_game_app() {
  AudioApp app;
  app.kind = kAudioGameKind;
  app.title = "Current game";
  app.track = 0;  // assigned by slot position
  return app;
}

std::vector<AudioApp> normalize_audio_apps(const std::vector<AudioApp>& apps) {
  std::vector<AudioApp> out;
  std::vector<std::string> seen;
  bool game_seen = false;
  for (const AudioApp& app : apps) {
    if (out.size() >= static_cast<size_t>(kAudioAppStoredMax)) break;
    AudioApp normalized = app;
    if (audio_app_is_game(normalized)) {
      // One Current game slot: a second one would capture the same program twice.
      if (game_seen) continue;
      game_seen = true;
      normalized.window.clear();
      normalized.exe.clear();
      normalized.title.clear();
      normalized.track = 0;  // assigned by slot position
      out.push_back(std::move(normalized));
      continue;
    }
    if (normalized.exe.empty() || normalized.window.empty()) continue;
    normalized.kind.clear();  // anything but "game" is a named program
    const std::string key = lower_ascii(normalized.exe);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
    seen.push_back(key);
    out.push_back(std::move(normalized));
  }
  return out;
}

bool audio_apps_equal(const std::vector<AudioApp>& left, const std::vector<AudioApp>& right) {
  const std::vector<AudioApp> a = normalize_audio_apps(left);
  const std::vector<AudioApp> b = normalize_audio_apps(right);
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (audio_app_key(a[i]) != audio_app_key(b[i]) || a[i].window != b[i].window) return false;
  }
  return true;
}

bool audio_app_controls_equal(const std::vector<AudioApp>& left, const std::vector<AudioApp>& right) {
  const std::vector<AudioApp> a = normalize_audio_apps(left);
  const std::vector<AudioApp> b = normalize_audio_apps(right);
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (audio_app_key(a[i]) != audio_app_key(b[i]) || a[i].window != b[i].window) return false;
    if (audio_app_volume(a[i].volume) != audio_app_volume(b[i].volume) || a[i].muted != b[i].muted) return false;
  }
  return true;
}

std::vector<AudioApp> effective_audio_apps(const Config& config) {
  if (config.audio_capture_mode != "apps") return {};
  std::vector<AudioApp> selected = normalize_audio_apps(config.audio_apps);
  if (selected.size() > static_cast<size_t>(kAudioAppMax)) selected.resize(kAudioAppMax);
  return selected;
}

std::vector<AudioApp> over_limit_audio_apps(const Config& config) {
  if (config.audio_capture_mode != "apps") return {};
  const std::vector<AudioApp> all = normalize_audio_apps(config.audio_apps);
  if (all.size() <= static_cast<size_t>(kAudioAppMax)) return {};
  return std::vector<AudioApp>(all.begin() + kAudioAppMax, all.end());
}

AudioRoutingPlan plan_audio_routing(const Config& config, const GameAudioTarget& game) {
  // Automatic mode has its own plan, built from the lanes the host is carrying. A caller that has only
  // the config gets the empty six-track shape: never the desktop fallback, which could record an
  // excluded program or BuffrX's own playback.
  if (config.audio_capture_mode == "auto") return plan_auto_audio_routing(config, {});
  AudioRoutingPlan plan;
  plan.apps = effective_audio_apps(config);
  plan.track_count = audio_track_count(config);
  // Windows process loopback captures an app, and nothing captures "the desktop except these apps".
  // Keeping the loopback on beside app tracks would record those apps twice, so it goes off while the
  // app mode records. In desktop mode the loopback is what records, even with routes kept for later.
  plan.desktop_captured = config.audio_capture_mode != "apps";
  const bool desktop_separate = plan.desktop_captured && config.separate_tracks;
  // With app routes the desktop's track is free, so the microphone takes track 2 and the apps start at 3.
  // Without them the old layout is kept exactly: desktop on 2, microphone on 3.
  const int mic_track = plan.desktop_captured ? 3 : 2;
  const bool mic_separate = (config.separate_tracks || !plan.desktop_captured) && plan.track_count >= mic_track;
  plan.desktop_mixers = (1u << 0) | (desktop_separate ? (1u << 1) : 0u);
  plan.mic_mixers = (1u << 0) | (mic_separate ? (1u << (mic_track - 1)) : 0u);
  for (size_t i = 0; i < plan.apps.size(); ++i) {
    AudioApp& app = plan.apps[i];
    const int track = kAudioAppFirstTrack + static_cast<int>(i);
    app.track = track;  // the slot owns its track, never a stored number
    plan.app_mixers.push_back((1u << 0) | (1u << (track - 1)));
    if (audio_app_is_game(app)) {
      plan.game_slot = static_cast<int>(i);
      plan.game_track = track;
      if (game.active) {
        app.exe = game.exe;
        app.window = game.window;
        app.title = game.title;
      } else {
        // No game BuffrX is capturing: the slot is selected but has no source. It is never filled with
        // the desktop or an unrelated program, and the track stays silent until the game is captured -
        // while its label stays "Game", because a file's labels describe its lanes whatever was playing.
        app.exe.clear();
        app.window.clear();
        app.title.clear();
      }
    }
  }
  // A game already selected by name is captured by that slot. The Current game slot then reserves its lane
  // but makes no source: one process tree is never recorded twice. Its label stays the generic Game lane.
  if (plan.game_slot >= 0 && game.active) {
    for (size_t i = 0; i < plan.apps.size(); ++i) {
      if (i == static_cast<size_t>(plan.game_slot) || audio_app_is_game(plan.apps[i])) continue;
      if (lower_ascii(plan.apps[i].exe) == lower_ascii(game.exe)) {
        plan.game_covered_slot = static_cast<int>(i);
        break;
      }
    }
    if (plan.game_covered_slot >= 0) {
      plan.apps[static_cast<size_t>(plan.game_slot)].exe.clear();
      plan.apps[static_cast<size_t>(plan.game_slot)].window.clear();
    }
  }

  std::vector<std::vector<std::string>> names(static_cast<size_t>(plan.track_count));
  std::vector<std::vector<std::string>> sources(static_cast<size_t>(plan.track_count));
  if (!names.empty()) {
    names[0].push_back("Mix");
    // Device switches apply live without rebuilding the encoders. Reserve their identity even while
    // disabled, so enabling one later cannot make the saved mix disagree with its track labels.
    if (plan.desktop_captured) sources[0].push_back("desktop");
    if (mic_separate || config.mic_enabled) sources[0].push_back("microphone");
  }
  if (desktop_separate) {
    names[1].push_back("Desktop");
    sources[1].push_back("desktop");
  }
  if (mic_separate) {
    names[static_cast<size_t>(mic_track - 1)].push_back("Microphone");
    sources[static_cast<size_t>(mic_track - 1)].push_back("microphone");
  }
  for (size_t i = 0; i < plan.apps.size(); ++i) {
    const AudioApp& app = plan.apps[i];
    const size_t index = static_cast<size_t>(app.track - 1);
    const std::string source = audio_app_is_game(app) ? std::string(kAudioGameSource) : "app:" + app.exe;
    names[index].push_back(app_display_name(app));
    sources[index].push_back(source);
    if (!names[0].empty()) sources[0].push_back(source);
  }

  for (int track = 1; track <= plan.track_count; ++track) {
    AudioTrackInfo info;
    info.track = track;
    info.sources = sources[static_cast<size_t>(track - 1)];
    const std::vector<std::string>& named = names[static_cast<size_t>(track - 1)];
    if (track == 1) {
      info.role = "mix";
      info.title = "BuffrX Mix";
    } else if (named.empty()) {
      info.role = "empty";
      info.title = "BuffrX Track " + std::to_string(track);
    } else {
      info.role = named.size() == 1 ? (named[0] == "Desktop" ? "desktop" : named[0] == "Microphone" ? "microphone" : "application")
                                    : "combined";
      info.title = "BuffrX " + join(named);
    }
    plan.tracks.push_back(std::move(info));
  }
  return plan;
}

AudioRoutingPlan plan_auto_audio_routing(const Config& config, const std::vector<AutoAudioLane>& lanes) {
  AudioRoutingPlan plan;
  plan.track_count = 6;
  plan.desktop_captured = false;  // no loopback fallback: it would record an excluded program
  const bool mic = config.mic_enabled;
  plan.mic_mixers = (1u << 0) | (mic ? (1u << 1) : 0u);

  std::vector<std::vector<std::string>> names(6);
  std::vector<std::vector<std::string>> sources(6);
  std::vector<std::string> roles(6, "empty");
  std::vector<bool> present(6, false);
  sources[0].push_back("microphone");
  names[0].push_back("Mix");
  if (mic) {
    names[1].push_back("Microphone");
    sources[1].push_back("microphone");
    roles[1] = "microphone";
    present[1] = true;
  }
  for (const AutoAudioLane& lane : lanes) {
    if (lane.track < 3 || lane.track > 6) continue;
    const size_t index = static_cast<size_t>(lane.track - 1);
    const std::string bus = auto_bus_source(lane.track);
    names[index].push_back(lane.other ? std::string("Other audio") : lane.name);
    sources[index].push_back(bus);
    roles[index] = lane.other ? "combined" : "application";
    present[index] = true;
    sources[0].push_back(bus);
  }

  for (int track = 1; track <= 6; ++track) {
    AudioTrackInfo info;
    info.track = track;
    info.sources = sources[static_cast<size_t>(track - 1)];
    if (track == 1) {
      info.role = "mix";
      info.title = "BuffrX Mix";
    } else if (!present[static_cast<size_t>(track - 1)]) {
      info.role = "empty";
      info.title = "BuffrX Track " + std::to_string(track);
    } else {
      info.role = roles[static_cast<size_t>(track - 1)];
      info.title = "BuffrX " + join(names[static_cast<size_t>(track - 1)]);
    }
    plan.tracks.push_back(std::move(info));
  }
  return plan;
}

Json audio_apps_json(const std::vector<AudioApp>& apps) {
  Json array = Json::array();
  for (const AudioApp& app : apps) {
    Json entry = Json::object();
    if (audio_app_is_game(app)) {
      entry.set("kind", Json(std::string(kAudioGameKind)));
    } else {
      entry.set("window", Json(app.window));
      entry.set("exe", Json(app.exe));
    }
    entry.set("title", Json(app.title));
    entry.set("volume", Json(audio_app_volume(app.volume)));
    entry.set("muted", Json(app.muted));
    entry.set("track", Json(app.track));
    array.push(entry);
  }
  return array;
}

Json audio_track_metadata(const AudioRoutingPlan& plan) {
  Json json = Json::object();
  json.set("version", Json(2));
  json.set("generator", Json(std::string("BuffrX")));
  Json tracks = Json::array();
  for (const AudioTrackInfo& track : plan.tracks) {
    Json entry = Json::object();
    entry.set("index", Json(track.track));  // 1-based, as the product and OBS count tracks
    entry.set("role", Json(track.role));
    entry.set("title", Json(track.title));
    Json list = Json::array();
    for (const std::string& source : track.sources) list.push(Json(source));
    entry.set("sources", list);
    tracks.push(entry);
  }
  json.set("tracks", tracks);
  return json;
}

std::string audio_sidecar_path(const std::string& media_path) {
  return media_path + kAudioSidecarSuffix;
}

namespace {
AudioLayout layout_from_plan(const AudioRoutingPlan& plan) {
  AudioLayout layout;
  layout.buffrx = true;
  for (const AudioTrackInfo& track : plan.tracks) {
    AudioTrackLabel label;
    label.index = track.track;
    label.role = track.role;
    label.title = track.title;
    label.sources = track.sources;
    layout.tracks.push_back(std::move(label));
  }
  return layout;
}
}  // namespace

bool write_audio_sidecar(const std::string& media_path, const AudioRoutingPlan& plan) {
  if (media_path.empty() || plan.track_count <= 1) return true;
  return write_audio_layout(media_path, layout_from_plan(plan));
}

bool write_audio_sidecar_pending(const std::string& media_path, const AudioRoutingPlan& plan) {
  if (media_path.empty() || plan.track_count <= 1) return true;
  return write_audio_layout_pending(media_path, layout_from_plan(plan));
}

}  // namespace bx
