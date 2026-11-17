// The audio mixer's array edits, kept pure so the panel's faders and menus can be checked without a window.
// An app entry is written whole and every other entry and key is carried through: the engine's own fields
// (window, exe, title, track) and a hand-edited key the person added are never dropped by an adjustment.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "../audio_routing.h"
#include "../../shared/json.h"

namespace bx::ui {

// The level a strip shows for an app whose entry predates the field, or a hand-edited value outside 0..1.
inline double audio_app_volume(const Json& app) {
  return std::clamp(app["volume"].as_double(1.0), 0.0, 1.0);
}

inline bool audio_app_muted(const Json& app) { return app["muted"].as_bool(false); }

// One pick in the expanded mixer: the program's window and name, its slot placeholder, and the two mixer
// fields at their defaults (full volume, unmuted).
inline Json audio_app_entry(const Json& window) {
  Json entry = Json::object();
  entry.set("window", Json(window["id"].as_string()));
  entry.set("exe", Json(window["exe"].as_string()));
  entry.set("title", Json(window["name"].as_string()));
  entry.set("track", Json(2));  // the slot's position decides the real track; kept for older builds
  entry.set("volume", Json(1.0));
  entry.set("muted", Json(false));
  return entry;
}

// Add (append, until `limit`) or replace one slot. The other entries keep their order and every key.
inline Json audio_apps_with_entry(const Json& apps, const Json& entry, int replace_slot, size_t limit) {
  const std::vector<Json>& items = apps.items();
  Json out = Json::array();
  bool placed = false;
  for (size_t i = 0; i < items.size(); ++i) {
    if (replace_slot >= 0 && i == static_cast<size_t>(replace_slot)) {
      out.push(entry);
      placed = true;
    } else {
      out.push(items[i]);
    }
  }
  if (!placed && replace_slot < 0 && items.size() < limit) out.push(entry);
  return out;
}

// One field of one slot, leaving the entry's other fields and every other entry as they were.
inline Json audio_apps_with_field(const Json& apps, size_t slot, const char* key, Json value) {
  const std::vector<Json>& items = apps.items();
  Json out = Json::array();
  for (size_t i = 0; i < items.size(); ++i) {
    Json entry = items[i];
    if (i == slot) entry.set(key, value);
    out.push(entry);
  }
  return out;
}

inline Json audio_apps_without(const Json& apps, size_t slot) {
  const std::vector<Json>& items = apps.items();
  Json out = Json::array();
  for (size_t i = 0; i < items.size(); ++i) {
    if (i != slot) out.push(items[i]);
  }
  return out;
}

// A strip's one-line state: short enough for a 76-unit channel, and only ever what the engine reported.
// The full sentence (audio_slot_hint) stays in the strip's hover card, so the line never has to carry a
// reason. Muted and off come first: a channel that is not heard says so before anything about capture.
enum class StripState { off, muted, saved, failed, recording, ready, shared, waiting };

inline StripState audio_strip_state(bool enabled, bool muted, bool recorded, const std::string& slot_state,
                                    bool recording_now) {
  if (!enabled) return StripState::off;
  if (muted) return StripState::muted;
  if (!recorded) return StripState::saved;
  if (slot_state == "failed") return StripState::failed;
  if (slot_state == "connected") return recording_now ? StripState::recording : StripState::ready;
  if (slot_state == "covered") return StripState::shared;
  return StripState::waiting;
}

inline const char* audio_strip_label(StripState state) {
  switch (state) {
    case StripState::off: return "Off";
    case StripState::muted: return "Muted";
    case StripState::saved: return "Saved";
    case StripState::failed: return "Failed";
    case StripState::recording: return "Recording";
    case StripState::ready: return "Ready";
    case StripState::shared: return "Shared";
    case StripState::waiting: return "Waiting";
  }
  return "";
}

// "Reset volume" is offered only when it would change something: a channel already at full gain, or one
// that is off, has nothing to reset.
inline bool audio_volume_at_full(double volume) { return volume >= 0.9995; }

// The reset writes the one field: the mute and every other entry are carried through untouched, so a
// muted channel stays muted and comes back at the gain the person chose.
inline Json audio_apps_with_volume_reset(const Json& apps, size_t slot) {
  return audio_apps_with_field(apps, slot, "volume", Json(1.0));
}

// ---------------------------------------------------------------- automatic app audio
// `config.audio_capture_mode == "auto"` is the normal experience: every application that plays sound is
// captured on a lane of its own and is heard again after a restart, whatever process id it has. A program
// is known by its lowercased executable, never by a slot or lane index, so a lane that leaves while a hand
// is on the mixer can never be mistaken for the one that took its place. Every override below is written
// into config.audio_app_prefs as a patch addressed by that key.

inline bool audio_is_auto(const Json& config) { return config["audio_capture_mode"].as_string() == "auto"; }

// The preferences key: the executable, lowercased. An entry the person made for "Firefox.EXE" is the one
// "firefox.exe" reads and the one the engine merges into.
inline std::string audio_exe_key(std::string exe) {
  for (char& c : exe) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 'A' && u <= 'Z') c = static_cast<char>(u + 32);
  }
  return exe;
}

inline bool audio_pref_has(const Json& prefs, const std::string& exe) { return prefs.has(audio_exe_key(exe)); }
inline double audio_pref_volume(const Json& prefs, const std::string& exe) {
  return std::clamp(prefs[audio_exe_key(exe)]["volume"].as_double(1.0), 0.0, 1.0);
}
inline bool audio_pref_muted(const Json& prefs, const std::string& exe) {
  return prefs[audio_exe_key(exe)]["muted"].as_bool(false);
}
inline bool audio_pref_excluded(const Json& prefs, const std::string& exe) {
  return prefs[audio_exe_key(exe)]["excluded"].as_bool(false);
}

// One field of one program. The patch carries only what changed, so the other fields of this program and
// every other program stay as they are when the engine merges it.
inline Json audio_volume_field(double volume) {
  Json fields = Json::object();
  fields.set("volume", Json(std::clamp(volume, 0.0, 1.0)));
  return fields;
}
inline Json audio_muted_field(bool muted) {
  Json fields = Json::object();
  fields.set("muted", Json(muted));
  return fields;
}
inline Json audio_excluded_field(bool excluded) {
  Json fields = Json::object();
  fields.set("excluded", Json(excluded));
  return fields;
}
inline Json audio_fields_with(Json fields, const Json& more) {
  for (const auto& field : more.fields()) fields.set(field.first, field.second);
  return fields;
}

// The one write the panel sends: `store.set_config(audio_pref_patch("firefox.exe", audio_muted_field(true)))`.
inline Json audio_pref_patch(const std::string& exe, const Json& fields) {
  Json entry = Json::object();
  entry.set(audio_exe_key(exe), fields);
  Json patch = Json::object();
  patch.set("audio_app_prefs", entry);
  return patch;
}

// The merge the engine performs, kept pure so a test proves it: the named program's fields change and every
// other program, and every other field of this one, is carried through untouched.
inline Json audio_prefs_merged(const Json& prefs, const std::string& exe, const Json& fields) {
  Json out = prefs.fields().empty() ? Json::object() : prefs;
  const std::string key = audio_exe_key(exe);
  Json entry = prefs[key].fields().empty() ? Json::object() : prefs[key];
  for (const auto& field : fields.fields()) entry.set(field.first, field.second);
  out.set(key, entry);
  return out;
}

// Every program the person excluded, in a stable order, and the one patch that brings them all back. An
// exclude is a preference, not a deletion: the program's volume and mute are still there when it returns.
inline std::vector<std::string> audio_excluded(const Json& prefs) {
  std::vector<std::string> out;
  for (const auto& field : prefs.fields()) {
    if (field.second["excluded"].as_bool()) out.push_back(field.first);
  }
  std::sort(out.begin(), out.end());
  return out;
}
inline Json audio_restore_excluded_patch(const Json& prefs) {
  Json entry = Json::object();
  for (const std::string& exe : audio_excluded(prefs)) entry.set(exe, audio_excluded_field(false));
  Json patch = Json::object();
  patch.set("audio_app_prefs", entry);
  return patch;
}

// The Other group's own controls: one level and one mute for the lane the overflow apps share. A member's
// own volume and mute sit in the preferences above, so the two never overwrite each other.
inline double audio_other_volume(const Json& config) {
  return std::clamp(config["audio_other_volume"].as_double(1.0), 0.0, 1.0);
}
inline bool audio_other_muted(const Json& config) { return config["audio_other_muted"].as_bool(false); }
inline Json audio_other_volume_patch(double volume) {
  Json patch = Json::object();
  patch.set("audio_other_volume", Json(std::clamp(volume, 0.0, 1.0)));
  return patch;
}
inline Json audio_other_muted_patch(bool muted) {
  Json patch = Json::object();
  patch.set("audio_other_muted", Json(muted));
  return patch;
}

// A member of the Other group, as its own row reads it: its own gain and mute, which are what that row
// writes. The engine reports each member's effective level and mute (its own value combined with the group's),
// so a row that read the reported value would show the group's gain on every member and its first drag would
// jump away from the pointer. A member with no preference of its own is at full gain and unmuted.
inline double audio_member_volume(const Json& prefs, const std::string& exe) {
  return audio_pref_has(prefs, exe) ? audio_pref_volume(prefs, exe) : 1.0;
}
inline bool audio_member_muted(const Json& prefs, const std::string& exe) {
  return audio_pref_has(prefs, exe) ? audio_pref_muted(prefs, exe) : false;
}

// Whether the lanes on screen are what a running capture writes. `status.audio.recorded` is the legacy apps-mode
// flag (a saved selection that is not being written); an automatic lane is written by the buffer or a recording,
// so while one runs its state is Recording/Ready/Failed/Waiting, never the "Saved" of a stored route.
inline bool audio_lanes_recorded(bool auto_mode, bool reported_recorded, bool capturing) {
  return auto_mode ? capturing : reported_recorded;
}

// A program the engine could not capture (`status.audio.auto.uncaptured`): its name and the one sentence why.
// `blocked` is a process tree that holds an excluded program or BuffrX's own playback, so none of it is
// recorded; `failed` and `unsupported` are capture failures. A mixer that drew only its lanes would claim
// every program is being recorded, so both the mixer and Settings say these out loud.
struct AutoWarning {
  std::string name;
  std::string reason;
  bool blocked = false;
};

inline std::string audio_uncaptured_name(const Json& entry) {
  const std::string name = entry["name"].as_string();
  if (!name.empty()) return name;
  const std::string exe = entry["exe"].as_string();
  return exe.empty() ? std::string("An app") : exe;
}
// The engine's own sentence without its "Not captured: " lead, because the row already says that.
inline std::string audio_uncaptured_reason(const Json& entry) {
  std::string error = entry["error"].as_string();
  const std::string lead = "Not captured: ";
  if (error.rfind(lead, 0) == 0) error = error.substr(lead.size());
  if (!error.empty()) return error;
  if (entry["state"].as_string() == "unsupported") return "Process audio is not supported on this PC";
  if (entry["state"].as_string() == "blocked") return "Its process tree holds a program that is not captured";
  return "Audio capture failed";
}
// At most `limit` of them, in the engine's order and without one name twice.
inline std::vector<AutoWarning> audio_uncaptured_warnings(const Json& audio_status, size_t limit) {
  std::vector<AutoWarning> out;
  for (const Json& entry : audio_status["auto"]["uncaptured"].items()) {
    if (out.size() >= limit) break;
    AutoWarning warning;
    warning.name = audio_uncaptured_name(entry);
    warning.reason = audio_uncaptured_reason(entry);
    warning.blocked = entry["state"].as_string() == "blocked";
    bool seen = false;
    for (const AutoWarning& one : out) seen = seen || one.name == warning.name;
    if (!seen) out.push_back(std::move(warning));
  }
  return out;
}
inline size_t audio_uncaptured_total(const Json& audio_status) { return audio_status["auto"]["uncaptured"].items().size(); }
// One line of the warning block: a head line, one line per warning, and a "+N more" line. The mixer's popup is
// sized from the same numbers, so a long list can never outgrow the box it is drawn in.
constexpr float kAutoWarningLine = 15.0f;
inline float audio_uncaptured_height(size_t shown, size_t total) {
  if (total == 0) return 0.0f;
  float height = kAutoWarningLine;  // the head line
  height += static_cast<float>(shown) * kAutoWarningLine;
  if (total > shown) height += kAutoWarningLine;
  return height + 4.0f;  // and the gap under the block
}

// The lanes the mixer draws. The pipe contract puts them at status.audio.lanes; the engine's own status
// carries the same array inside status.audio.auto, so both shapes are read and neither is guessed from.
inline const Json& audio_lanes_of(const Json& audio_status) {
  const Json& lanes = audio_status["lanes"];
  if (!lanes.items().empty()) return lanes;
  return audio_status["auto"]["lanes"];
}

// One lane from status.audio.lanes. The Other lane has no executable; its members are in `apps`.
inline bool audio_lane_other(const Json& lane) { return lane["other"].as_bool(); }
inline std::string audio_lane_key(const Json& lane) {
  return audio_lane_other(lane) ? std::string("other") : audio_exe_key(lane["exe"].as_string());
}
inline std::string audio_lane_title(const Json& lane) {
  const std::string name = lane["name"].as_string();
  if (!name.empty()) return name;
  return audio_lane_other(lane) ? std::string("Other audio") : audio_exe_key(lane["exe"].as_string());
}

// The rows' order: the engine's, frozen while a hand is in the mixer so a stream that appears cannot move
// the row under the pointer. A lane that left is dropped and a new one joins after the ones already shown.
inline std::vector<std::string> audio_stable_order(const std::vector<std::string>& previous,
                                                   const std::vector<std::string>& current, bool frozen) {
  if (!frozen || previous.empty()) return current;
  std::vector<std::string> out;
  for (const std::string& key : previous) {
    if (std::find(current.begin(), current.end(), key) != current.end()) out.push_back(key);
  }
  for (const std::string& key : current) {
    if (std::find(out.begin(), out.end(), key) == out.end()) out.push_back(key);
  }
  return out;
}

// What the automatic mixer says when it has no row to show. `capturing` is the buffer or a recording: with
// neither, the watcher is off and an empty list means nothing is being looked for, not that nothing plays.
enum class AutoAudioState { idle, waiting, active, unsupported, error };

inline AutoAudioState audio_auto_state(const Json& audio_status, bool capturing) {
  const Json& auto_info = audio_status["auto"];
  if (auto_info["unsupported"].as_bool()) return AutoAudioState::unsupported;
  if (!auto_info["error"].as_string().empty()) return AutoAudioState::error;
  if (!capturing) return AutoAudioState::idle;
  if (!audio_lanes_of(audio_status).items().empty() || !auto_info["sources"].items().empty()) return AutoAudioState::active;
  return AutoAudioState::waiting;
}

// The calm line for that state; empty while there are rows to show.
inline const char* audio_auto_line(AutoAudioState state) {
  switch (state) {
    case AutoAudioState::idle: return "Start the buffer or recording to detect app audio.";
    case AutoAudioState::waiting: return "Nothing playing";
    case AutoAudioState::unsupported: return "App audio cannot be captured on this PC. Desktop sound can be recorded instead.";
    case AutoAudioState::error: return "App audio could not start.";
    case AutoAudioState::active: return "";
  }
  return "";
}

}  // namespace bx::ui
