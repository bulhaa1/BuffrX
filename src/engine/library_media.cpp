// The parts of the library that need FFmpeg: how long a clip is, and its grid thumbnail.
//
// Kept apart from library.cpp so the path guards and name reservation — the pieces the unit tests
// cover — stay free of the media runtime, and so the test binary never needs the FFmpeg DLLs beside it.
#include "library.h"

#include "clip_check.h"
#include "file_final.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <map>
#include <mutex>

#include "../shared/files.h"
#include "../shared/json.h"
#include "../shared/log.h"
#include "media.h"

namespace bx {
namespace {

// Durations are cached by path, size and modification time: listing a library must not re-open every
// file on every request, and a replaced file must not keep its old length. The cache is also written
// to the data root, because opening the whole library through FFmpeg cost 5.5 s for 90 clips and the
// engine paid it again on every start: a fresh process now re-probes only what is new or changed.
struct DurationEntry {
  int64_t size = 0;
  int64_t modified = 0;
  double seconds = 0.0;
  // Whether the engine's own container check says this is a clip at all. The check runs before any
  // decoder sees the file: FFmpeg's demuxer is not safe with a file that is not a whole container
  // (half a recording, a stray .mp4 in the folder), and it took the engine down.
  bool decodable = false;
};

// How many files one listing may open through FFmpeg. A cold library is 90 probes of about 55 ms
// each; the panel draws from what is already known and the next listing carries on from there, so
// nothing waits 5 s for a card. A file whose entry is cached is not counted: it costs no open.
constexpr int kProbeBudget = 6;

std::mutex g_duration_mutex;
std::map<std::string, DurationEntry> g_durations;
bool g_cache_loaded = false;

// The path with one spelling for its separators: the listing's path and the cached one are the same
// file even if the config wrote the folders with forward slashes. Case is kept: two files whose names
// differ only by case are two files, and folding them would give one card the other's duration.
std::string cache_key(const std::string& file) {
  std::string key = file;
  for (char& c : key) {
    if (c == '/') c = '\\';
  }
  return key;
}

std::string cache_file_path() {
  LibraryPaths paths;
  library_paths(paths);
  if (paths.thumbnails_dir.empty()) return std::string();
  std::string root = paths.thumbnails_dir;
  while (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
  const size_t slash = root.find_last_of("\\/");
  if (slash == std::string::npos || slash == 0) return std::string();
  // <data root>\thumbnails -> <data root>\library-cache.json
  return root.substr(0, slash) + "\\library-cache.json";
}

void load_cache_locked() {
  if (g_cache_loaded) return;
  g_cache_loaded = true;
  const std::string path = cache_file_path();
  if (path.empty()) return;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return;
  std::string text;
  char buffer[8192];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
  std::fclose(file);
  Json root;
  std::string error;
  // The cache is an optimisation, never a source of truth. A file that is not the JSON this build
  // wrote - truncated by a crash, edited by hand, left by an older version - is ignored and the
  // listing behaves exactly as if it had never been written.
  if (!Json::parse(text, root, error) || !root.is_object()) {
    BX_LOG_WARN("library cache ignored: %s", error.empty() ? "not an object" : error.c_str());
    return;
  }
  const Json& files = root["files"];
  if (!files.is_object()) return;
  for (const auto& entry : files.fields()) {
    const Json& value = entry.second;
    if (!value.is_object()) continue;
    DurationEntry cached;
    // An entry without a size gets -1, which matches no real file, so the hit test drops it.
    cached.size = value["size"].as_int(-1);
    cached.modified = value["mtime"].as_int(0);
    cached.seconds = value["duration"].as_double(0.0);
    cached.decodable = value["clip"].as_bool(false);
    g_durations[cache_key(entry.first)] = cached;
  }
}

void save_cache_locked() {
  const std::string path = cache_file_path();
  if (path.empty()) return;
  // Entries for files that are gone are forgotten here, so the file cannot grow forever.
  for (auto found = g_durations.begin(); found != g_durations.end();) {
    if (GetFileAttributesA(found->first.c_str()) == INVALID_FILE_ATTRIBUTES) found = g_durations.erase(found);
    else ++found;
  }
  Json files = Json::object();
  for (const auto& entry : g_durations) {
    Json value = Json::object();
    value.set("size", Json(entry.second.size));
    value.set("mtime", Json(entry.second.modified));
    value.set("duration", Json(entry.second.seconds));
    value.set("clip", Json(entry.second.decodable));
    files.set(entry.first, std::move(value));
  }
  Json root = Json::object();
  root.set("version", Json(1));
  root.set("files", std::move(files));
  const std::string text = root.dump(1);
  // Written beside and renamed into place: a crash halfway through must not leave a half file the
  // next start would trust (write_file_atomic, src/shared/files.h).
  write_file_atomic(path, text);
}

}  // namespace

double probe_duration(const std::string& file) {
  // The FFmpeg inside the runtime answers this directly; there is no ffprobe.exe in the package.
  const MediaInfo info = media_probe(file);
  return info.ok ? info.duration_seconds : 0.0;
}

void library_annotate(std::vector<MediaItem>& items, int thumbnail_limit) {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  int made = 0;
  int probed = 0;
  bool dirty = false;
  for (MediaItem& item : items) {
    // What the cache knows holds when the file is the same bytes as when it was probed: a clip replaced
    // in place is probed again, everything else costs no open.
    bool decodable = false;
    bool known = false;
    {
      std::lock_guard<std::mutex> lock(g_duration_mutex);
      load_cache_locked();
      const auto found = g_durations.find(cache_key(item.file));
      if (found != g_durations.end() && found->second.size == item.size && found->second.modified == item.modified) {
        item.duration = found->second.seconds;
        decodable = found->second.decodable;
        known = true;
      }
    }

    // The cheap half of the rule first: a held file, a fresh file at the clips root and anything outside it
    // needs no writer, so no open is spent on it. Doing this after the ask made a 5,000-clip listing open
    // every clip and spend 16 s on it (overnight N10).
    if (annotate_should_skip(item.file, static_cast<uint64_t>(now) * 1000, library_is_held(item.file), false,
                             item.folder.empty(), static_cast<uint64_t>(item.modified) * 1000)) {
      continue;
    }
    // Only a file written in the last minute can still have a writer, and asking opens it.
    if (annotate_writer_probe_needed(static_cast<uint64_t>(now) * 1000, static_cast<uint64_t>(item.modified) * 1000) &&
        !file_writer_closed(item.file)) {
      continue;
    }
    if (!known) {
      // Probing opens the file through FFmpeg, and a cold library holds a listing for seconds. A listing
      // takes a few files and the next one continues: a card without a length yet still draws, and the
      // panel asks for its thumbnail by path when it scrolls in.
      if (probed >= kProbeBudget) continue;
      ++probed;
      // The engine's own container check, before any decoder sees the file. A file that is not a whole
      // container - a recording that was killed, a stray .mp4 - is not handed to FFmpeg at all: the
      // demuxer does not survive one, and it took the whole engine down with it. The check is the
      // engine's own parser, and it costs a few seeks. A file with no bytes at all cannot be a clip, so
      // it is refused without even that.
      if (item.size > 0 && check_clip_file(item.file).has_video_track) {
        decodable = true;
        item.duration = probe_duration(item.file);
      }
      // A probed length of 0 is remembered as 0: a valid file whose container carries no duration must
      // not be opened again on every listing.
      std::lock_guard<std::mutex> lock(g_duration_mutex);
      g_durations[cache_key(item.file)] = DurationEntry{item.size, item.modified, item.duration, decodable};
      dirty = true;
    }
    if (!decodable) continue;
    // A thumbnail already on disk costs one lookup and no decode. Making one is only for the budget; past
    // it the thumbnail stays empty and the panel asks for it by path as the card is drawn. A failed
    // decode used to be retried on every listing, because only successes counted against the budget.
    item.thumbnail = cached_thumbnail(item.file);
    if (item.thumbnail.empty() && made < thumbnail_limit) {
      item.thumbnail = ensure_thumbnail(item.file);
      if (!item.thumbnail.empty()) ++made;
    }
  }
  // One write per listing, and only when this listing learned something.
  if (dirty) {
    std::lock_guard<std::mutex> lock(g_duration_mutex);
    save_cache_locked();
  }
}

std::vector<std::string> ensure_preview_frames(const std::string& file) {
  std::vector<std::string> frames;
  LibraryPaths paths;
  library_paths(paths);
  if (paths.thumbnails_dir.empty() || !file_writer_closed(file) || !check_clip_file(file).has_video_track) return frames;
  CreateDirectoryA(paths.thumbnails_dir.c_str(), nullptr);
  const double length = probe_duration(file);
  if (length <= 0) return frames;
  for (int i = 0; i < kPreviewFrames; ++i) {
    char key[80];
    std::snprintf(key, sizeof(key), "%016llx-f%d.jpg", static_cast<unsigned long long>(library_file_key(file)), i);
    const std::string target = paths.thumbnails_dir + "\\" + key;
    if (GetFileAttributesA(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
      std::string error;
      if (!media_thumbnail(file, target, 320, length * (i + 0.5) / kPreviewFrames, error)) continue;
    }
    frames.push_back(target);
  }
  return frames;
}

std::string cached_thumbnail(const std::string& file) {
  LibraryPaths paths;
  library_paths(paths);
  if (paths.thumbnails_dir.empty()) return std::string();
  char key[64];
  std::snprintf(key, sizeof(key), "%016llx.jpg", static_cast<unsigned long long>(library_file_key(file)));
  const std::string target = paths.thumbnails_dir + "\\" + key;
  return GetFileAttributesA(target.c_str()) != INVALID_FILE_ATTRIBUTES ? target : std::string();
}

std::string ensure_thumbnail(const std::string& file, std::string* reason) {
  LibraryPaths paths;
  library_paths(paths);
  if (paths.thumbnails_dir.empty()) return std::string();
  CreateDirectoryA(paths.thumbnails_dir.c_str(), nullptr);
  const std::string cached = cached_thumbnail(file);
  if (!cached.empty()) return cached;
  char key[64];
  std::snprintf(key, sizeof(key), "%016llx.jpg", static_cast<unsigned long long>(library_file_key(file)));
  const std::string target = paths.thumbnails_dir + "\\" + key;

  // A single frame one second in (a screenshot is its own first frame), scaled to a card size, and
  // frame 0 when a clip is shorter than a second. Failures are silent: a missing thumbnail must never
  // block the library from listing a clip.
  //
  // The engine's own container check comes first. A file that is not a whole container never reaches
  // the decoder: FFmpeg's demuxer does not survive one, and it took the engine down with it.
  const bool image = [&] {
    std::string lower = file;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.size() > 4 && (lower.rfind(".png") == lower.size() - 4 ||
                                 lower.rfind(".jpg") == lower.size() - 4 ||
                                 lower.rfind(".jpeg") == lower.size() - 5);
  }();
  if (!file_writer_closed(file)) return std::string();
  if (!image && !check_clip_file(file).has_video_track) return std::string();
  std::string error;
  const bool made = media_thumbnail(file, target, image ? 480 : 320, image ? 0.0 : 1.0, error) ||
                    (!image && media_thumbnail(file, target, 320, 0.0, error));
  if (!made) {
    if (reason) *reason = error;  // "the media work is busy" or why the frame could not be decoded
    return std::string();
  }
  return GetFileAttributesA(target.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : target;
}

}  // namespace bx
