#include "audio_layout.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../shared/files.h"
#include "../shared/json.h"
#include "../shared/log.h"

namespace bx {
namespace {

// A sidecar is six small objects. Anything larger is not one this product wrote, and is not parsed:
// a file beside a clip must never be able to make the editor read for as long as it likes.
constexpr size_t kMaxSidecarBytes = 64 * 1024;
constexpr size_t kMaxTracks = 16;
constexpr size_t kMaxSources = 16;
constexpr size_t kMaxText = 120;
constexpr DWORD kIdentityBytes = 16 * 1024;

uint64_t filetime_ticks(const FILETIME& time) {
  return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | static_cast<uint64_t>(time.dwLowDateTime);
}

uint64_t fnv1a(const uint8_t* data, size_t count) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < count; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

// One read at one offset, from an already open handle.
bool read_at(HANDLE file, uint64_t offset, uint8_t* buffer, DWORD count) {
  LARGE_INTEGER at{};
  at.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) return false;
  DWORD read = 0;
  return ReadFile(file, buffer, count, &read, nullptr) != 0 && read == count;
}

std::string hex64(uint64_t value) {
  char text[17] = {};
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

bool parse_hex64(const std::string& text, uint64_t& value) {
  if (text.size() != 16) return false;
  uint64_t parsed = 0;
  for (char c : text) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    if (digit < 0) return false;
    parsed = (parsed << 4) | static_cast<uint64_t>(digit);
  }
  value = parsed;
  return true;
}

Json identity_json(const MediaIdentity& identity) {
  Json json = Json::object();
  json.set("size", Json(identity.size));
  json.set("mtime", Json(identity.mtime));
  json.set("head", Json(hex64(identity.head)));
  json.set("tail", Json(hex64(identity.tail)));
  return json;
}

bool identity_from_json(const Json& json, MediaIdentity& identity) {
  if (!json.is_object() || !json["size"].is_number() || !json["mtime"].is_number()) return false;
  identity.size = json["size"].as_int(-1);
  identity.mtime = json["mtime"].as_int(0);
  identity.valid = identity.size >= 0 && parse_hex64(json["head"].as_string(), identity.head) &&
                   parse_hex64(json["tail"].as_string(), identity.tail);
  return identity.valid;
}

bool same_identity(const MediaIdentity& left, const MediaIdentity& right) {
  return left.valid && right.valid && left.size == right.size && left.mtime == right.mtime &&
         left.head == right.head && left.tail == right.tail;
}

bool read_text(const std::string& path, std::string& text) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  text.clear();
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, read);
    if (text.size() > kMaxSidecarBytes) {
      std::fclose(file);
      return false;
    }
  }
  std::fclose(file);
  return !text.empty();
}

bool known_role(const std::string& role) {
  return role == "mix" || role == "desktop" || role == "microphone" || role == "application" ||
         role == "combined" || role == "empty";
}

std::string clipped(const std::string& text) { return text.size() > kMaxText ? text.substr(0, kMaxText) : text; }

// True when the text names this product's sidecar, whatever else is wrong with it. Used only to decide
// whether an existing file may be deleted or replaced: a file nobody here wrote is never removed, and a
// version 1 sidecar (the layout before media identity) is still this product's own.
bool declares_buffrx(const Json& json) {
  const int version = static_cast<int>(json["version"].as_int(0));
  return json.is_object() && json["generator"].as_string() == "BuffrX" && (version == 1 || version == 2) &&
         json["tracks"].is_array();
}

// True when a file exists at the sidecar name and this product did not write it. Such a file is never
// opened over or deleted.
bool foreign_sidecar(const std::string& sidecar, bool& exists) {
  exists = GetFileAttributesA(sidecar.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (!exists) return false;
  std::string text;
  Json json;
  std::string parse_error;
  return !read_text(sidecar, text) || !Json::parse(text, json, parse_error) || !declares_buffrx(json);
}

// Reads one track entry into a label. False when it is not one this product could have written.
bool read_track(const Json& entry, int index, AudioTrackLabel& label) {
  if (!entry.is_object()) return false;
  if (entry["index"].as_int(0) != index) return false;
  const std::string role = entry["role"].as_string();
  if (!known_role(role)) return false;
  label.index = index;
  label.role = role;
  label.title = clipped(entry["title"].as_string());
  const Json& sources = entry["sources"];
  if (!sources.is_array() || sources.items().size() > kMaxSources) return false;
  for (const Json& source : sources.items()) {
    const std::string text = source.as_string();
    if (text.empty() || text.size() > kMaxText) return false;
    label.sources.push_back(text);
  }
  return true;
}

// Reads every track entry of a sidecar this product wrote. False when one is not a shape it could have
// written; a caller then treats the whole file as an import.
bool read_tracks(const Json& json, AudioLayout& layout) {
  const std::vector<Json>& tracks = json["tracks"].items();
  if (tracks.empty() || tracks.size() > kMaxTracks) return false;
  AudioLayout read;
  read.buffrx = true;
  for (size_t i = 0; i < tracks.size(); ++i) {
    AudioTrackLabel label;
    if (!read_track(tracks[i], static_cast<int>(i) + 1, label)) return false;
    read.tracks.push_back(std::move(label));
  }
  read.aggregate = read.tracks.size() >= 2 && read.tracks.front().role == "mix";

  // The mix can be rebuilt only when each of its sources is exactly one of the tracks after it, and no
  // track holds something the mix does not. `combined` (two apps on one track) is fine: it is one entry.
  if (read.aggregate && !read.tracks.front().sources.empty()) {
    std::vector<std::string> mix = read.tracks.front().sources;
    bool complete = true;
    for (size_t track = 1; track < read.tracks.size() && complete; ++track) {
      for (const std::string& source : read.tracks[track].sources) {
        if (std::find(mix.begin(), mix.end(), source) == mix.end()) complete = false;
      }
    }
    for (const std::string& source : mix) {
      size_t found = 0;
      for (size_t track = 1; track < read.tracks.size(); ++track) {
        found += static_cast<size_t>(std::count(read.tracks[track].sources.begin(), read.tracks[track].sources.end(), source));
      }
      if (found != 1) complete = false;
    }
    read.complete = complete;
  }
  layout = std::move(read);
  return true;
}

Json sidecar_json(const AudioLayout& layout, const MediaIdentity* identity) {
  Json json = Json::object();
  json.set("version", Json(2));
  json.set("generator", Json(std::string("BuffrX")));
  if (identity) json.set("media", identity_json(*identity));
  Json tracks = Json::array();
  for (const AudioTrackLabel& track : layout.tracks) {
    Json entry = Json::object();
    entry.set("index", Json(track.index));
    entry.set("role", Json(track.role));
    entry.set("title", Json(track.title));
    Json sources = Json::array();
    for (const std::string& source : track.sources) sources.push(Json(source));
    entry.set("sources", sources);
    tracks.push(entry);
  }
  json.set("tracks", tracks);
  return json;
}

bool write_layout(const std::string& media_path, const AudioLayout& layout, bool bind) {
  if (media_path.empty() || layout.tracks.size() <= 1) return true;
  if (!layout.buffrx) return false;
  const std::string sidecar = media_path + kAudioLayoutSuffix;
  bool exists = false;
  if (foreign_sidecar(sidecar, exists)) {
    BX_LOG_WARN("the audio track labels for %s were not written: a file this product did not write is already there",
                media_path.c_str());
    return false;
  }
  MediaIdentity identity;
  if (bind && !media_identity(media_path, identity)) {
    BX_LOG_WARN("the audio track labels for %s were not written: its final bytes could not be measured",
                media_path.c_str());
    return false;
  }
  const Json json = sidecar_json(layout, bind ? &identity : nullptr);
  if (write_file_atomic(sidecar, json.dump(2))) return true;
  BX_LOG_WARN("the audio track labels for %s were not written", media_path.c_str());
  return false;
}

}  // namespace

bool media_identity(const std::string& path, MediaIdentity& identity) {
  identity = MediaIdentity{};
  if (path.empty()) return false;
  HANDLE file = CreateFileW(wide_path(path).c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION before{};
  if (!GetFileInformationByHandle(file, &before)) {
    CloseHandle(file);
    return false;
  }
  const uint64_t size = (static_cast<uint64_t>(before.nFileSizeHigh) << 32) | static_cast<uint64_t>(before.nFileSizeLow);
  const uint64_t mtime = filetime_ticks(before.ftLastWriteTime);
  const DWORD head_len = static_cast<DWORD>(std::min<uint64_t>(size, kIdentityBytes));
  const DWORD tail_len =
      size > kIdentityBytes ? static_cast<DWORD>(std::min<uint64_t>(size - kIdentityBytes, kIdentityBytes)) : 0;
  std::vector<uint8_t> head(head_len);
  std::vector<uint8_t> tail(tail_len);
  const bool read = (head_len == 0 || read_at(file, 0, head.data(), head_len)) &&
                    (tail_len == 0 || read_at(file, size - tail_len, tail.data(), tail_len));
  BY_HANDLE_FILE_INFORMATION after{};
  const bool stable = GetFileInformationByHandle(file, &after) &&
                      size == ((static_cast<uint64_t>(after.nFileSizeHigh) << 32) |
                               static_cast<uint64_t>(after.nFileSizeLow)) &&
                      mtime == filetime_ticks(after.ftLastWriteTime);
  CloseHandle(file);
  if (!read || !stable) return false;
  identity.size = static_cast<int64_t>(size);
  identity.mtime = static_cast<int64_t>(mtime);
  identity.head = fnv1a(head.data(), head.size());
  identity.tail = fnv1a(tail.data(), tail.size());
  identity.valid = true;
  return true;
}

AudioLayout imported_audio_layout(int audio_streams) {
  AudioLayout layout;
  const int count = std::min<int>(std::max(audio_streams, 0), static_cast<int>(kMaxTracks));
  for (int i = 0; i < count; ++i) {
    AudioTrackLabel label;
    label.index = i + 1;
    label.role = "track";
    layout.tracks.push_back(std::move(label));
  }
  return layout;
}

bool read_bound_audio_layout(const std::string& media_path, AudioLayout& layout) {
  layout = AudioLayout{};
  if (media_path.empty()) return false;
  std::string text;
  if (!read_text(media_path + kAudioLayoutSuffix, text)) return false;
  Json json;
  std::string parse_error;
  if (!Json::parse(text, json, parse_error) || !declares_buffrx(json)) return false;
  // Version 1 named no media identity. A layout that cannot be tied to these exact bytes is an import:
  // the old file may be gone, or another file with the same stream count may be sitting here now.
  if (json["version"].as_int(0) != 2) return false;
  MediaIdentity stored;
  if (!identity_from_json(json["media"], stored)) return false;
  MediaIdentity current;
  if (!media_identity(media_path, current) || !same_identity(stored, current)) return false;
  return read_tracks(json, layout);
}

AudioLayout read_audio_layout(const std::string& media_path, int audio_streams) {
  AudioLayout layout = imported_audio_layout(audio_streams);
  if (media_path.empty() || audio_streams <= 0) return layout;
  AudioLayout read;
  if (!read_bound_audio_layout(media_path, read)) return layout;
  // The display count decides which labels can be drawn; a sidecar that names a different number of
  // streams is stale even when its identity was once right (an edited container keeps its bytes).
  if (read.tracks.size() != static_cast<size_t>(audio_streams) ||
      read.tracks.size() > kMaxTracks) {
    return layout;
  }
  return read;
}

AudioGains audio_gains(const std::vector<double>& volumes, const std::vector<bool>& muted,
                       const std::vector<bool>& solo, int tracks) {
  AudioGains gains;
  gains.values.assign(static_cast<size_t>(std::max(0, tracks)), 1.0);
  const bool any_solo = std::any_of(solo.begin(), solo.end(), [](bool on) { return on; });
  for (size_t i = 0; i < gains.values.size(); ++i) {
    const double volume = i < volumes.size() ? std::clamp(volumes[i], 0.0, 4.0) : 1.0;
    const bool silent = any_solo ? !(i < solo.size() && solo[i]) : (i < muted.size() && muted[i]);
    gains.values[i] = silent ? 0.0 : volume;
    if (std::abs(gains.values[i] - 1.0) > 0.0001) gains.changed = true;
  }
  return gains;
}

AudioMixPlan audio_mix_plan(const AudioLayout& layout, const AudioGains& gains, std::string& error) {
  error.clear();
  if (!layout.aggregate || !gains.changed) return AudioMixPlan::keep;
  if (layout.complete) return AudioMixPlan::recompute;
  error =
      "This file's mix holds sound that has no track of its own, so its levels cannot be changed. "
      "Record it with separate tracks to mix it here.";
  return AudioMixPlan::refuse;
}

bool write_audio_layout(const std::string& media_path, const AudioLayout& layout) {
  return write_layout(media_path, layout, true);
}

bool write_audio_layout_pending(const std::string& media_path, const AudioLayout& layout) {
  return write_layout(media_path, layout, false);
}

bool bind_audio_layout(const std::string& media_path) {
  if (media_path.empty()) return true;
  const std::string sidecar = media_path + kAudioLayoutSuffix;
  std::string text;
  if (!read_text(sidecar, text)) {
    // No sidecar is not a failure: a single-track recording never had one.
    return GetFileAttributesA(sidecar.c_str()) == INVALID_FILE_ATTRIBUTES;
  }
  Json json;
  std::string parse_error;
  if (!Json::parse(text, json, parse_error) || !declares_buffrx(json) || json["version"].as_int(0) != 2) {
    BX_LOG_WARN("the audio track labels beside %s are not this product's, so they were not bound",
                media_path.c_str());
    return false;
  }
  AudioLayout layout;
  if (!read_tracks(json, layout)) {
    BX_LOG_WARN("the audio track labels beside %s could not be read back", media_path.c_str());
    return false;
  }
  // The media is final here: every writer is closed before a chunk is published, and every save or
  // export binds after its last move.
  return write_layout(media_path, layout, true);
}

bool audio_sidecar_is_ours(const std::string& media_path) {
  if (media_path.empty()) return false;
  std::string text;
  if (!read_text(media_path + kAudioLayoutSuffix, text)) return false;
  Json json;
  std::string parse_error;
  return Json::parse(text, json, parse_error) && declares_buffrx(json);
}

void remove_audio_layout(const std::string& media_path) {
  if (media_path.empty()) return;
  const std::string sidecar = media_path + kAudioLayoutSuffix;
  std::string text;
  if (!read_text(sidecar, text)) return;
  Json json;
  std::string parse_error;
  if (!Json::parse(text, json, parse_error) || !declares_buffrx(json)) return;
  DeleteFileA(sidecar.c_str());
}

bool same_audio_layout(const AudioLayout& left, const AudioLayout& right) {
  if (left.tracks.size() != right.tracks.size()) return false;
  for (size_t i = 0; i < left.tracks.size(); ++i) {
    if (left.tracks[i].index != right.tracks[i].index || left.tracks[i].role != right.tracks[i].role ||
        left.tracks[i].title != right.tracks[i].title || left.tracks[i].sources != right.tracks[i].sources) {
      return false;
    }
  }
  return true;
}

JoinAudioLayout join_audio_layout(const std::vector<AudioLayout>& inputs) {
  JoinAudioLayout result;
  if (inputs.empty()) return result;
  const size_t tracks = inputs.front().tracks.size();
  for (const AudioLayout& one : inputs) {
    if (one.tracks.size() != tracks) {
      result.error = "the clips have different audio tracks";
      return result;
    }
  }
  // A single stream has no mix to describe, and a file with no audio has nothing to carry.
  if (tracks <= 1) return result;

  const auto aggregate = [](const AudioLayout& one) {
    return one.buffrx && one.aggregate && one.tracks.size() > 1;
  };
  size_t aggregates = 0;
  for (const AudioLayout& one : inputs) aggregates += aggregate(one) ? 1 : 0;
  if (aggregates == 0) return result;  // imports joined with imports: no sidecar, unchanged
  if (aggregates != inputs.size()) {
    result.error =
        "These clips hold their sound differently: one starts with a full mix and the other has only "
        "separate tracks. A join cannot combine them without changing how they sound. Export the clips "
        "separately, or join clips that hold their sound the same way.";
    return result;
  }

  bool identical = true;
  for (size_t i = 1; i < inputs.size() && identical; ++i) identical = same_audio_layout(inputs[i], inputs.front());
  if (identical) {
    result.carries = true;
    result.layout = inputs.front();
    return result;
  }

  // Every input is a BuffrX aggregate: stream 1 is a whole mix, and a later stream is a stem lane. The
  // join writes those lanes in order, so the output can be described positionally.
  AudioLayout out;
  out.buffrx = true;
  out.aggregate = true;
  bool complete = true;
  for (const AudioLayout& one : inputs) complete = complete && one.complete;

  AudioTrackLabel mix;
  mix.index = 1;
  mix.role = "mix";
  std::string mix_title = inputs.front().tracks.front().title;
  for (size_t i = 1; i < inputs.size(); ++i) {
    if (inputs[i].tracks.front().title != mix_title) mix_title.clear();
  }
  mix.title = mix_title.empty() ? "BuffrX Mix" : mix_title;

  for (size_t lane = 1; lane < tracks; ++lane) {
    const AudioTrackLabel& first = inputs.front().tracks[lane];
    bool same_source = true;
    bool same_title = true;
    bool any_source = false;
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (!inputs[i].tracks[lane].sources.empty()) any_source = true;
      if (i == 0) continue;
      if (inputs[i].tracks[lane].role != first.role || inputs[i].tracks[lane].sources != first.sources) {
        same_source = false;
      }
      if (inputs[i].tracks[lane].title != first.title) same_title = false;
    }
    AudioTrackLabel label;
    label.index = static_cast<int>(lane) + 1;
    if (!any_source) {
      // No clip put sound on this lane: it is empty in the output too, whatever its label once said.
      label.role = "empty";
      label.title = "BuffrX Track " + std::to_string(lane + 1);
    } else if (same_source) {
      // One physical source stayed on this lane in every clip: keep its identity. A cosmetic title
      // difference (a longer device name) does not change that, so a role's own name is used instead.
      label.role = first.role;
      label.sources = first.sources;
      if (same_title && !first.title.empty()) {
        label.title = first.title;
      } else if (first.sources.size() == 1 &&
                 first.sources.front() == "auto:bus:" + std::to_string(lane + 1)) {
        // An automatic bus is stable, but its occupants change. Keep their ordered, distinct names
        // across chunk boundaries, including chunks that already name several occupants.
        std::vector<std::string> names;
        for (const AudioLayout& input : inputs) {
          std::string title = input.tracks[lane].title;
          if (title.rfind("BuffrX ", 0) == 0) title.erase(0, 7);
          size_t start = 0;
          while (start < title.size()) {
            const size_t end = title.find(" + ", start);
            const std::string name = title.substr(start, end == std::string::npos ? end : end - start);
            if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            if (end == std::string::npos) break;
            start = end + 3;
          }
        }
        label.title = "BuffrX ";
        for (const std::string& name : names) {
          if (label.title.size() > 7) label.title += " + ";
          label.title += name;
        }
        label.title = clipped(label.title);
      } else if (first.role == "desktop") {
        label.title = "BuffrX Desktop";
      } else if (first.role == "microphone") {
        label.title = "BuffrX Microphone";
      } else {
        label.title = "BuffrX Track " + std::to_string(lane + 1);
      }
    } else {
      // The lane carried different sources across the clips. No single name is true for all of it, so it
      // gets a logical id of its own (`join:track:<ordinal>`): honest to the output, never a claim about
      // a physical source. The mix can still be rebuilt from the output's own stems.
      label.role = "combined";
      label.title = "BuffrX Track " + std::to_string(lane + 1);
      label.sources.push_back("join:track:" + std::to_string(lane + 1));
    }
    for (const std::string& source : label.sources) mix.sources.push_back(source);
    out.tracks.push_back(std::move(label));
  }
  // An incomplete input's mix holds sound no stem carries. The output keeps the same mix-first meaning,
  // but says so, so no level change is allowed and playback uses the mix alone (nothing is heard twice).
  if (!complete) mix.sources.push_back("join:unlisted");
  out.tracks.insert(out.tracks.begin(), std::move(mix));
  out.complete = complete;
  result.carries = true;
  result.layout = std::move(out);
  return result;
}

}  // namespace bx
