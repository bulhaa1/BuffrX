// The recording store: one directory per session, holding the chunk files and a JSON index.
//
// The index is rewritten whole through a temporary file and then moved into place, so a crash leaves
// either the previous index or the new one — never half of one. Recovery does not need to repair an
// index; it only needs to notice that the last session never set closed.
#include "session_store.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../shared/files.h"
#include "dir_listing.h"
#include "../shared/json.h"
#include "../shared/log.h"
#include "media_math.h"
#include "timeline_pins.h"
#include "timeline_retention.h"
#include "timeline_thumbs.h"

namespace bx {
namespace {

// Set by session_store_init (under the engine lock), read by commands that run outside it: always
// read through root_now(), as a copy.
std::string g_root;
std::mutex g_root_mutex;
std::string root_now() {
  std::lock_guard<std::mutex> lock(g_root_mutex);
  return g_root;
}

// One engine writes every index, and a reader's recovery is a write too: every decision about an index
// and every rewrite of one is serialized here, so two workers can no longer reason from the same index
// and then overwrite each other's files. Recursive because a sweep loads a session (which may recover it)
// and then rewrites the same index.
std::recursive_mutex g_store_mutex;

std::string join(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  const char last = directory.back();
  if (last == '\\' || last == '/') return directory + name;
  return directory + "\\" + name;
}

bool directory_exists(const std::string& path) {
  const DWORD attributes = GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Creates exactly one directory level: the parent must already exist, so an unusable root can never
// grow a tree of directories somewhere nobody asked for.
bool ensure_directory(const std::string& path) {
  if (path.empty()) return false;
  if (directory_exists(path)) return true;
  const size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos || !directory_exists(path.substr(0, slash))) return false;
  return CreateDirectoryA(path.c_str(), nullptr) != 0 || directory_exists(path);
}

// A usable root is absolute, at least one level below a drive or share root, and its parent already
// exists. Anything else is how a session ends up written outside the library.
bool root_is_usable(const std::string& root) {
  if (root.size() < 4) return false;
  const bool drive_path = root[1] == ':' && (root[2] == '\\' || root[2] == '/');
  const bool unc_path = root.rfind("\\\\", 0) == 0;
  if (!drive_path && !unc_path) return false;
  const size_t slash = root.find_last_of("\\/");
  if (slash == std::string::npos || slash < 2) return false;
  return directory_exists(root.substr(0, slash));
}

// The root counts as ready only once it exists: `session_store_init` creates it one level under its
// parent, and a begin against a root that could not be created must fail rather than write elsewhere.
bool root_ready(const std::string& root) { return root_is_usable(root) && directory_exists(root); }

// True when `path` sits inside `root`. Both are compared case-insensitively and `root` must end at a
// separator, so "…\sessions-old" is never mistaken for a child of "…\sessions". A `/` and a `\` are the same
// separator: the splitter reports the footage file with backslashes while the sessions root is spelled with
// whatever the configured clips folder used, and a plain compare read one directory as two - a retention
// sweep then dropped chunks from the index without renaming or deleting their files, and the footage stayed
// on disk for ever.
bool path_inside(const std::string& path, const std::string& root) {
  if (path.empty() || root.empty() || path.size() <= root.size()) return false;
  const auto normalise = [](std::string& text) {
    for (char& character : text) {
      if (character == '/') character = '\\';
      character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
  };
  std::string lhs = path;
  std::string rhs = root;
  normalise(lhs);
  normalise(rhs);
  // A text prefix is not containment: "<root>\media\..\..\victim" passes it, and DeleteFileA follows
  // the `..` out of the sessions root. An index is user-editable, so a parent component is refused
  // outright rather than resolved.
  for (size_t start = 0; start <= lhs.size();) {
    size_t end = lhs.find('\\', start);
    if (end == std::string::npos) end = lhs.size();
    if (end - start == 2 && lhs[start] == '.' && lhs[start + 1] == '.') return false;
    if (end == lhs.size()) break;
    start = end + 1;
  }
  if (rhs.back() != '\\') rhs.push_back('\\');
  return lhs.compare(0, rhs.size(), rhs) == 0;
}

// The same file spelled two ways. The splitter reports a file with backslashes, a directory walk joins its names onto whatever the sessions
// root was configured with (which can mix `/` and `\`), and Windows ignores case; a plain string compare called one file two.
bool same_file(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char x = a[i] == '/' ? '\\' : a[i];
    const char y = b[i] == '/' ? '\\' : b[i];
    if (std::tolower(static_cast<unsigned char>(x)) != std::tolower(static_cast<unsigned char>(y))) return false;
  }
  return true;
}

// When the file was last written, in milliseconds since the epoch, or 0 when it cannot be read. A crashed
// writer's last byte is the moment its footage ended, and only the file remembers it.
int64_t last_write_ms(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  ULARGE_INTEGER stamp{};
  stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
  stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
  if (stamp.QuadPart == 0) return 0;
  // FILETIME counts 100 ns steps from 1601; the wall clock the sessions store counts from 1970.
  return static_cast<int64_t>(stamp.QuadPart / 10000) - 11644473600000LL;
}

int64_t file_size(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  LARGE_INTEGER size{};
  size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
  size.LowPart = data.nFileSizeLow;
  return size.QuadPart;
}

bool read_text(const std::string& path, std::string& out) {
  FILE* file = nullptr;
  // The index is replaced by a move, and a reader that opens exactly then is refused by Windows. Three
  // tries over a few milliseconds turn that into a read; a session that is really gone still answers
  // false, after the same few milliseconds.
  for (int attempt = 0; attempt < 3 && !file; ++attempt) {
    if (attempt > 0) Sleep(5);
    file = std::fopen(path.c_str(), "rb");
  }
  if (!file) return false;
  out.clear();
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, read);
  std::fclose(file);
  return true;
}

bool write_text(const std::string& path, const std::string& text) {
  // The unique temporary name and the retried move both live in write_file_atomic (src/shared/files.h),
  // which every other file the engine replaces shares.
  return write_file_atomic(path, text);
}

std::string session_directory(const std::string& id) { return join(root_now(), id); }
std::string session_index_path(const std::string& id) { return join(join(root_now(), id), "index.json"); }
// A session's markers sit in their own file beside the chunk index: adding or removing one then leaves the
// index - the file every chunk append rewrites - byte for byte and untimed. The index still carries the
// markers too, so a reader of either file sees a whole session.
std::string session_markers_path(const std::string& id) { return join(join(root_now(), id), "markers.json"); }

// The state file a session's writer leaves while it owns it: <id>\writer.state, one token per process run.
// Its presence is what keeps a read pure. A live writer publishes its own chunks, and a reader's recovery
// used to file the chunk that writer was still holding a second time; a token from an earlier run never
// matches, so a session a dead engine left behind is recovered once, on its next read.
std::string writer_state_path(const std::string& id) { return join(session_directory(id), "writer.state"); }

// One value per process run: the process id and the tick it started on, so a reused process id alone
// cannot make an old state file look like a live writer.
const std::string& writer_token() {
  static const std::string token = [] {
    return std::to_string(GetCurrentProcessId()) + " " +
           std::to_string(static_cast<unsigned long>(GetTickCount()));
  }();
  return token;
}

bool writer_is_live(const std::string& id) {
  // Read with no retries: a session with no state file is the common case, and every listing asks this of
  // every session. Only the token matters, so a fixed buffer is enough.
  FILE* file = std::fopen(writer_state_path(id).c_str(), "rb");
  if (!file) return false;
  char text[64] = {0};
  const size_t read = std::fread(text, 1, sizeof(text) - 1, file);
  std::fclose(file);
  std::string token(text, read);
  while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) token.pop_back();
  return token == writer_token();
}

// "BuffrX 2026-10-18 14-03-22" — the same shape as the configured filename format. Two sessions that
// begin inside the same second get distinct directories rather than sharing one.
std::string make_session_id() {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  char stamp[64];
  std::snprintf(stamp, sizeof(stamp), "BuffrX %04d-%02d-%02d %02d-%02d-%02d", now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond);
  std::string id = stamp;
  for (int suffix = 2; directory_exists(session_directory(id)) && suffix < 1000; ++suffix) {
    id = std::string(stamp) + " (" + std::to_string(suffix) + ")";
  }
  return id;
}

// A new marker's identity: eight random bytes as lowercase hex. Random rather than a counter, so an id a
// marker had is never handed to another after a removal, and the whole point - telling two markers with the
// same moment, kind and label apart - holds on both sides of a removal.
std::string new_marker_id() {
  uint8_t bytes[8] = {};
  BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  std::string id;
  for (const uint8_t byte : bytes) {
    char pair[3];
    std::snprintf(pair, sizeof(pair), "%02x", byte);
    id += pair;
  }
  return id;
}

Json markers_to_json(const std::vector<SessionMarker>& markers) {
  Json array = Json::array();
  for (const SessionMarker& marker : markers) {
    Json item = Json::object();
    item.set("at_ms", Json(static_cast<int64_t>(marker.at_ms)));
    item.set("kind", Json(marker.kind));
    item.set("label", Json(marker.label));
    // Only when there is one: a marker from before ids must read back exactly as it was written.
    if (!marker.id.empty()) item.set("id", Json(marker.id));
    array.push(std::move(item));
  }
  return array;
}

// The markers out of one document. Both the index and the marker file hold them under "markers", so a
// sidecar can be read with exactly the rule the index uses.
void markers_from_json(const Json& json, std::vector<SessionMarker>& out) {
  out.clear();
  for (const Json& item : json["markers"].items()) {
    if (!item.is_object()) continue;
    SessionMarker marker;
    marker.at_ms = static_cast<uint32_t>(item["at_ms"].as_int(0));
    marker.kind = item["kind"].as_string();
    marker.label = item["label"].as_string();
    marker.id = item["id"].as_string();
    out.push_back(marker);
  }
}

Json entry_to_json(const SessionEntry& entry) {
  Json json = Json::object();
  json.set("id", Json(entry.id));
  json.set("kind", Json(entry.kind));
  json.set("app_id", Json(entry.app_id));
  json.set("profile", Json(entry.profile));
  json.set("started_ms", Json(static_cast<int64_t>(entry.started_ms)));
  json.set("started_at", Json(entry.started_at));
  json.set("ended_ms", Json(static_cast<int64_t>(entry.ended_ms)));
  json.set("closed", Json(entry.closed));

  Json chunks = Json::array();
  for (const SessionChunk& chunk : entry.chunks) {
    Json item = Json::object();
    item.set("file", Json(chunk.file));
    item.set("bytes", Json(chunk.bytes));
    item.set("start_ms", Json(static_cast<int64_t>(chunk.start_ms)));
    item.set("end_ms", Json(static_cast<int64_t>(chunk.end_ms)));
    item.set("wall_start_ms", Json(chunk.wall_start_ms));
    item.set("wall_end_ms", Json(chunk.wall_end_ms));
    item.set("epoch", Json(static_cast<int64_t>(chunk.epoch)));
    item.set("published", Json(chunk.published));
    chunks.push(std::move(item));
  }
  json.set("chunks", std::move(chunks));

  Json markers = markers_to_json(entry.markers);
  json.set("markers", std::move(markers));
  return json;
}

bool json_to_entry(const Json& json, SessionEntry& out) {
  if (!json.is_object()) return false;
  out.id = json["id"].as_string();
  out.kind = json["kind"].as_string();
  out.app_id = json["app_id"].as_string();
  out.profile = json["profile"].as_string();
  out.started_ms = static_cast<uint32_t>(json["started_ms"].as_int(0));
  out.started_at = json["started_at"].as_int(0);
  out.ended_ms = static_cast<uint32_t>(json["ended_ms"].as_int(0));
  out.closed = json["closed"].as_bool(false);

  out.chunks.clear();
  for (const Json& item : json["chunks"].items()) {
    if (!item.is_object()) continue;
    SessionChunk chunk;
    chunk.file = item["file"].as_string();
    chunk.bytes = item["bytes"].as_int(0);
    chunk.start_ms = static_cast<uint32_t>(item["start_ms"].as_int(0));
    chunk.end_ms = static_cast<uint32_t>(item["end_ms"].as_int(0));
    // An index written before the wall clock existed measured time only on the tick clock, which
    // restarts at every boot; `start_ms` is that absolute tick, not an offset. The session's start is
    // the one wall time such an index kept, so a chunk's wall time is that start plus its offset from
    // `started_ms`. An index older still has no `started_ms`: no offset can be known, so the chunk
    // takes the session's start rather than underflowing.
    const int64_t session_wall_ms = out.started_at * 1000;
    const auto wall_at = [&](uint32_t tick_ms) {
      if (out.started_ms == 0) return session_wall_ms;
      return session_wall_ms + static_cast<int64_t>(tick_ms - out.started_ms);
    };
    chunk.wall_start_ms = item.has("wall_start_ms") ? item["wall_start_ms"].as_int(0) : wall_at(chunk.start_ms);
    chunk.wall_end_ms = item.has("wall_end_ms") ? item["wall_end_ms"].as_int(0) : wall_at(chunk.end_ms);
    chunk.epoch = static_cast<uint32_t>(item["epoch"].as_int(0));
    // A chunk written to an index before this field existed had already been closed and filed.
    chunk.published = item.has("published") ? item["published"].as_bool(true) : true;
    if (!chunk.file.empty()) out.chunks.push_back(chunk);
  }

  out.markers.clear();
  markers_from_json(json, out.markers);
  return true;
}

bool write_index(const SessionEntry& entry, std::string& error) {
  // Compact: the index is rewritten with every chunk (about every 8 s), and nobody reads it by eye.
  if (!write_text(session_index_path(entry.id), entry_to_json(entry).dump())) {
    error = "could not write the session index";
    BX_LOG_WARN("session index write failed for %s", entry.id.c_str());
    return false;
  }
  return true;
}

// A marker operation writes this file and nothing else. The index is not touched, so its bytes and its
// last write time stay as they were.
bool write_markers(const SessionEntry& entry, std::string& error) {
  Json json = Json::object();
  json.set("markers", markers_to_json(entry.markers));
  if (!write_text(session_markers_path(entry.id), json.dump())) {
    error = "could not write the session markers";
    BX_LOG_WARN("session marker write failed for %s", entry.id.c_str());
    return false;
  }
  return true;
}

// The markers kept beside the index. A missing sidecar (a session from before it existed, or one just
// closed) leaves the index's own markers in place, which is the migration: the next marker operation
// writes the index's markers and the change into the sidecar together. A truncated or unreadable sidecar
// is ignored the same way - it is a marker file that is half written, never a chunk that is lost, because
// the chunks only ever come from the index.
void read_markers(const std::string& id, std::vector<SessionMarker>& markers) {
  // No retries, unlike the index: most sessions have no marker file, and every listing asks for one.
  FILE* file = std::fopen(session_markers_path(id).c_str(), "rb");
  if (!file) return;
  std::string text;
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
  std::fclose(file);
  Json json;
  std::string error;
  if (!Json::parse(text, json, error) || !json.is_object() || !json["markers"].is_array()) {
    // Said once per session: the panel reads a session every couple of seconds, and one cut-short file
    // must not fill the log.
    static std::set<std::string> warned;
    if (warned.insert(id).second) BX_LOG_WARN("the marker file of %s is unreadable; the index's own markers are used", id.c_str());
    return;
  }
  markers_from_json(json, markers);
}

// A chunk's size is only final once the muxer's child process has closed the file, so every size is
// re-measured from disk at the points where the index is meant to be the truth: closing and loading.
//
// One directory listing carries every size in a folder: `timeline.get` loads the session the preview shows several times a
// second, and a stat per chunk (1440 for two hours, about 70 ms) on every call held the store's lock against the writer's
// publishing. A name the listing does not hold is asked of the file itself.
void refresh_chunk_sizes(SessionEntry& entry) {
  const DirListing listing;
  for (SessionChunk& chunk : entry.chunks) {
    int64_t measured = listing.size(chunk.file);
    if (measured < 0) measured = file_size(chunk.file);
    if (measured > 0) chunk.bytes = measured;
  }
}

}  // namespace

// The index's JSON half for a caller outside this file (the mutation harness): the loaders above stay private.
bool session_entry_from_json(const Json& json, SessionEntry& out) { return json_to_entry(json, out); }
Json session_entry_json(const SessionEntry& entry) { return entry_to_json(entry); }

int64_t SessionEntry::bytes() const {
  int64_t total = 0;
  for (const SessionChunk& chunk : chunks) total += chunk.bytes;
  return total;
}

void session_store_init(const std::string& sessions_root) {
  std::string root = sessions_root;
  while (root.size() > 3 && (root.back() == '\\' || root.back() == '/')) root.pop_back();
  {
    std::lock_guard<std::mutex> lock(g_root_mutex);
    g_root = root;
  }
  // One level only, and only when the parent already exists: an unusable root creates nothing at all.
  if (root_is_usable(root)) {
    if (!ensure_directory(root)) BX_LOG_WARN("the session root could not be created: '%s'", root.c_str());
  } else {
    BX_LOG_WARN("the session root is not usable: '%s'", root.c_str());
  }
}

std::string session_store_root() { return root_now(); }

bool session_id_is_safe(const std::string& id) {
  if (id.empty() || id == "." || id == "..") return false;
  for (char character : id) {
    switch (character) {
      case '\\':
      case '/':
      case ':':
      case '*':
      case '?':
      case '"':
      case '<':
      case '>':
      case '|':
        return false;
      default:
        break;
    }
  }
  return true;
}

std::string session_media_dir(const std::string& id) { return join(session_directory(id), "media"); }
std::string session_thumbs_dir(const std::string& id) { return join(session_directory(id), "thumbs"); }
std::string session_writer_state_path(const std::string& id) { return writer_state_path(id); }

void session_thumbs_remove(const std::string& id, const std::string& prefix) {
  if (!session_id_is_safe(id)) return;
  const std::string directory = session_thumbs_dir(id);
  WIN32_FIND_DATAA found{};
  HANDLE search = FindFirstFileA((directory + "\\*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    const std::string name = found.cFileName;
    if (!prefix.empty() && name.rfind(prefix, 0) != 0) continue;
    DeleteFileA(join(directory, name).c_str());
  } while (FindNextFileA(search, &found) != 0);
  FindClose(search);
  // Only the folder of a session that is going away, or one a sweep emptied, is removed: the strip of a
  // session that still has footage keeps its folder for the next call.
  RemoveDirectoryA(directory.c_str());
}

bool session_store_begin(const std::string& kind, SessionEntry& out, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  out = SessionEntry{};
  const std::string root = root_now();
  if (!root_ready(root)) {
    // The folder can be deleted or moved while the engine runs. The store creates it again - one
    // level, the same rule it has always used - rather than refusing every recording with "no
    // usable root" until the engine is restarted, which is what happened: a person who deleted
    // their recordings folder could not record again at all.
    if (!ensure_directory(root)) {
      // Only a missing parent stops it: nothing may be invented where the user did not ask.
      const size_t slash = root.find_last_of("\\/");
      const std::string parent = slash == std::string::npos ? root : root.substr(0, slash);
      error = "the recordings folder " + parent + " does not exist";
      BX_LOG_WARN("session store refused to begin: %s", error.c_str());
      return false;
    }
    BX_LOG_INFO("the session root was missing and has been created again: '%s'", root.c_str());
  }
  out.kind = kind;
  out.started_ms = GetTickCount();
  out.started_at = static_cast<int64_t>(std::time(nullptr));
  out.id = make_session_id();
  if (!ensure_directory(session_directory(out.id)) || !ensure_directory(session_media_dir(out.id))) {
    error = "could not create the session directory";
    BX_LOG_WARN("session directory create failed: %s", session_media_dir(out.id).c_str());
    out = SessionEntry{};
    return false;
  }
  // A session whose state file could not be written still records: the file only keeps a reader from
  // recovering it, so a warning is the whole cost.
  if (!write_text(writer_state_path(out.id), writer_token() + "\n")) {
    BX_LOG_WARN("the writer state file of %s could not be written", out.id.c_str());
  }
  return write_index(out, error);
}

bool session_store_append_chunk(SessionEntry& entry, const SessionChunk& chunk, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  entry.chunks.push_back(chunk);
  // The size is the file's own, the one the store counts and deletes, not a caller's estimate.
  const int64_t measured = file_size(chunk.file);
  if (measured > 0) entry.chunks.back().bytes = measured;
  return write_index(entry, error);
}

std::vector<const SessionChunk*> session_visible_chunks(const SessionEntry& entry) {
  std::vector<const SessionChunk*> visible;
  for (const SessionChunk& chunk : entry.chunks) {
    if (chunk.published) visible.push_back(&chunk);
  }
  return visible;
}

bool session_store_add_marker(SessionEntry& entry, const std::string& kind, const std::string& label,
                              uint32_t now_ms, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  SessionMarker marker;
  marker.at_ms = now_ms;
  marker.kind = kind;
  marker.label = label;
  marker.id = new_marker_id();
  entry.markers.push_back(marker);
  // The sidecar is the truth, so a marker it refused must not stay in the caller's session: the next
  // chunk append would write it into the index, where a sidecar-preferring reader would hide it, and a
  // retry would then duplicate it.
  if (!write_markers(entry, error)) {
    entry.markers.pop_back();
    return false;
  }
  return true;
}

bool session_store_remove_marker(SessionEntry& entry, uint32_t at_ms, const std::string& kind,
                                 const std::string& label, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  SessionEntry next = entry;
  const auto marker = std::find_if(next.markers.begin(), next.markers.end(), [&](const SessionMarker& item) {
    return item.at_ms == at_ms && item.kind == kind && item.label == label;
  });
  if (marker == next.markers.end()) {
    error = "marker not found";
    return false;
  }
  next.markers.erase(marker);
  if (!write_markers(next, error)) return false;
  entry = std::move(next);
  error.clear();
  return true;
}

bool session_store_remove_marker(SessionEntry& entry, const std::string& marker_id, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  if (marker_id.empty()) {
    error = "a marker id is needed";
    return false;
  }
  SessionEntry next = entry;
  const auto marker = std::find_if(next.markers.begin(), next.markers.end(), [&](const SessionMarker& item) {
    return item.id == marker_id;
  });
  if (marker == next.markers.end()) {
    error = "marker not found";
    return false;
  }
  next.markers.erase(marker);
  if (!write_markers(next, error)) return false;
  entry = std::move(next);
  error.clear();
  return true;
}

bool session_store_close(SessionEntry& entry, uint32_t now_ms, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  entry.ended_ms = now_ms;
  entry.closed = true;
  entry.interrupted = false;
  refresh_chunk_sizes(entry);
  // The index now carries every marker, so the sidecar is not needed beside it any more.
  if (!write_index(entry, error)) return false;
  DeleteFileA(session_markers_path(entry.id).c_str());
  // The writer has let go: the next read may recover whatever it had to leave out of the index.
  DeleteFileA(writer_state_path(entry.id).c_str());
  return true;
}

std::vector<SessionEntry> session_store_load(std::string& error) {
  std::vector<SessionEntry> entries;
  error.clear();
  if (root_now().empty()) return entries;

  WIN32_FIND_DATAA found{};
  HANDLE search = FindFirstFileA(join(root_now(), "*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return entries;
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
    const std::string name = found.cFileName;
    if (name == "." || name == "..") continue;
    SessionEntry entry;
    std::string ignored;
    if (session_store_load_one(name, entry, ignored)) entries.push_back(entry);
  } while (FindNextFileA(search, &found) != 0);
  FindClose(search);

  std::sort(entries.begin(), entries.end(), [](const SessionEntry& a, const SessionEntry& b) {
    return session_older(b, a);
  });
  return entries;
}

// The chunks a crashed session left on disk: every video file in its media folder that its index
// does not already know about. The files are the recording, so they are found and indexed rather
// than lost with the engine that was writing them.
void recover_chunks(SessionEntry& entry) {
  const std::string media = session_media_dir(entry.id);
  WIN32_FIND_DATAA found{};
  HANDLE search = FindFirstFileA((media + "\\*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  std::vector<std::string> files;
  do {
    const std::string name = found.cFileName;
    if (name == "." || name == "..") continue;
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::string lower = [&] {
      std::string out = name;
      for (char& character : out) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
      return out;
    }();
    if (lower.size() < 4 || lower.rfind(".mp4") != lower.size() - 4) continue;
    files.push_back(join(media, name));
  } while (FindNextFileA(search, &found) != 0);
  FindClose(search);
  if (files.empty()) return;
  std::sort(files.begin(), files.end());
  // Two files left open by an overlap have no durable boundary. Keep both on disk rather than
  // publishing two invented, overlapping ranges that make a save select the wrong footage.
  const size_t unknown = static_cast<size_t>(std::count_if(files.begin(), files.end(), [&](const std::string& file) {
    return std::none_of(entry.chunks.begin(), entry.chunks.end(), [&](const SessionChunk& chunk) {
      return same_file(chunk.file, file);
    });
  }));
  if (unknown > 1) {
    static std::set<std::string> warned;
    if (warned.insert(entry.id).second)
      BX_LOG_WARN("the interrupted session %s has %zu unindexed chunks with unknown boundaries; files are kept in %s",
                  entry.id.c_str(), unknown, media.c_str());
    return;
  }
  // The chunk a crash left behind ran from the moment the last published chunk closed until the engine
  // died. Starting it at the session's start instead claimed the whole session in one file: a 90 second
  // session whose tail chunk held two seconds of footage reported 90 seconds of playable video, and a
  // trim taken from it ran to the end of the file.
  uint32_t resumed_ms = entry.started_ms;
  for (const SessionChunk& known : entry.chunks) resumed_ms = std::max(resumed_ms, known.end_ms);
  bool added = false;
  for (const std::string& file : files) {
    const bool known = std::any_of(entry.chunks.begin(), entry.chunks.end(), [&](const SessionChunk& chunk) {
      return same_file(chunk.file, file);
    });
    if (known) continue;
    SessionChunk chunk;
    chunk.file = file;
    chunk.bytes = file_size(file);
    // More than one unknown file cannot have its own start recovered, so every one of them begins where the
    // known footage stops - the last of them is the one that was open.
    chunk.start_ms = resumed_ms;
    // Wall time is the session's start plus the offset on the tick clock, never the tick itself.
    const int64_t session_wall_ms = entry.started_at * 1000;
    chunk.wall_start_ms = session_wall_ms + (chunk.start_ms - entry.started_ms);
    // The chunk ended when its writer last wrote to it, which the file itself records: the moment recovery
    // runs is seconds later, and a tail claiming those extra seconds made a cut near the end of a session
    // run past the footage it holds. An end at or before its start (a clock that has not moved a step yet)
    // leaves it one millisecond of footage, never zero.
    int64_t wall_end_ms = last_write_ms(file);
    if (wall_end_ms <= 0) wall_end_ms = entry.ended_ms ? (session_wall_ms + (entry.ended_ms - entry.started_ms))
                                                       : (session_wall_ms + (GetTickCount() - entry.started_ms));
    chunk.wall_end_ms = wall_end_ms;
    const int64_t duration = chunk.wall_end_ms - chunk.wall_start_ms;
    if (duration <= 0 || duration > UINT32_MAX - chunk.start_ms) {
      chunk.end_ms = chunk.start_ms + 1;
      chunk.wall_end_ms = chunk.wall_start_ms + 1;
    } else chunk.end_ms = chunk.start_ms + static_cast<uint32_t>(duration);
    // A dead writer is not holding the file any more: what it left behind is footage.
    chunk.published = true;
    entry.chunks.push_back(chunk);
    added = true;
  }
  if (!added) return;
  std::string write_error;
  if (!write_index(entry, write_error)) {
    BX_LOG_WARN("the recovered chunks of %s could not be written: %s", entry.id.c_str(), write_error.c_str());
  } else {
    BX_LOG_INFO("recovered %d chunk(s) of the interrupted session %s", static_cast<int>(entry.chunks.size()),
                entry.id.c_str());
  }
}

bool session_store_load_one(const std::string& id, SessionEntry& out, std::string& error) {
  if (!session_id_is_safe(id)) {
    error = "unsafe session id";
    return false;
  }
  // The read and the decision it leads to are one step, held against the writer: an index read here and
  // then rewritten from stale text would drop the chunk a worker published in between.
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  std::string text;
  if (!read_text(session_index_path(id), text)) {
    error = "no session index";
    return false;
  }
  Json json;
  if (!Json::parse(text, json, error)) return false;
  if (!json_to_entry(json, out)) {
    error = "the session index is not an object";
    return false;
  }
  if (out.id.empty()) out.id = id;
  // The id in the file builds this session's thumbs and media paths. The folder that was opened is
  // already checked, but the index's own word is what the paths use, so it is checked too: an index that
  // names another folder (or an absolute path) is refused here, the one place every reader goes through.
  if (!session_id_is_safe(out.id)) {
    error = "the session index names an unsafe id";
    BX_LOG_WARN("refusing session index %s: its id is not safe", id.c_str());
    return false;
  }
  // The sidecar is the newer word on the markers; the chunks are the index's and are never read from it.
  read_markers(out.id, out.markers);
  // Derived, never stored: an index that exists with closed == false is what a crash leaves behind.
  out.interrupted = !out.closed;
  // A session the engine died in the middle of has chunks on disk that its index never learned about: the
  // index is written when a chunk closes, and a killed engine never got there. Those files are the
  // recording, so they are picked up here - but only when no writer owns the session any more. A live
  // writer publishes its own chunks, and recovering for it filed the chunk it was still holding a second
  // time. A read of a live session therefore writes nothing at all.
  if (out.interrupted && !writer_is_live(out.id)) recover_chunks(out);
  refresh_chunk_sizes(out);
  return true;
}

namespace {
// Footage an interrupted retention sweep renamed to "<chunk>.expired" and never deleted: the index no
// longer names it, so sweeping the media folder is the only thing that finds it.
void remove_expired_leftovers(const std::string& media) {
  WIN32_FIND_DATAA leftover{};
  HANDLE search = FindFirstFileA((media + "\\*.expired").c_str(), &leftover);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    DeleteFileA(join(media, leftover.cFileName).c_str());
  } while (FindNextFileA(search, &leftover) != 0);
  FindClose(search);
}
}  // namespace

bool session_store_delete(const SessionEntry& entry, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  if (root_now().empty() || !session_id_is_safe(entry.id)) {
    error = "refusing to delete outside the sessions root";
    BX_LOG_WARN("refusing to delete session %s", entry.id.c_str());
    return false;
  }
  const std::string directory = session_directory(entry.id);
  const std::string media = session_media_dir(entry.id);
  // Only files inside this session's own directory are ever removed, so a tampered index cannot turn
  // a delete into a file shredder.
  for (const SessionChunk& chunk : entry.chunks) {
    if (path_inside(chunk.file, directory)) DeleteFileA(chunk.file.c_str());
  }
  // Footage the open session dropped and a crash kept from deleting is still on disk as "<chunk>.expired": it goes with the session.
  remove_expired_leftovers(media);
  DeleteFileA(session_index_path(entry.id).c_str());
  // The markers live beside the index and would keep the session's own directory from ever being removed.
  DeleteFileA(session_markers_path(entry.id).c_str());
  // The writer's state file lives in the session's own directory: left behind, it would keep that
  // directory from ever being removed.
  DeleteFileA(writer_state_path(entry.id).c_str());
  // The preview's pictures go with the footage; a thumbs folder left behind keeps the session's own
  // directory from ever being removed, and a session deleted for space would leak it for good.
  session_thumbs_remove(entry.id, std::string());
  RemoveDirectoryA(media.c_str());
  RemoveDirectoryA(directory.c_str());
  error.clear();
  return true;
}

bool session_store_drop_chunks(SessionEntry& entry, const std::vector<size_t>& expired, std::vector<DroppedChunk>& dropped,
                               std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  dropped.clear();
  if (expired.empty()) return true;
  SessionEntry next = entry;
  next.chunks.clear();
  const std::string directory = session_directory(entry.id);
  for (size_t index = 0; index < entry.chunks.size(); ++index) {
    if (std::find(expired.begin(), expired.end(), index) != expired.end()) {
      // The file is renamed to "<file>.expired" before the index forgets it, so a crash between the two leaves nothing recovery
      // would take for footage (it indexes only .mp4 files it does not know). The worker deletes the renamed file. A file Windows
      // holds cannot be renamed: it stays in the list and is tried again at the next sweep.
      const std::string& file = entry.chunks[index].file;
      const bool inside = path_inside(file, directory);
      const bool gone = GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES;
      const bool renamed = inside && !gone && MoveFileExA(file.c_str(), (file + ".expired").c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
      if (inside && !gone && !renamed) {
        next.chunks.push_back(entry.chunks[index]);
        continue;
      }
      dropped.push_back(DroppedChunk{entry.id, entry.chunks[index]});
    } else {
      next.chunks.push_back(entry.chunks[index]);
    }
  }
  if (!write_index(next, error)) {
    dropped.clear();
    return false;
  }
  entry.chunks.swap(next.chunks);
  return true;
}

// The open session's own trim. The window and the leases are read, and the expired chunks are reserved, in one short
// step under the retention gate; the rename and the index write that follow wait on the file system with the gate free,
// so a save asking for a lease is answered at once - refused while these chunks are on their way.
bool session_store_drop_expired(SessionEntry& entry, int minutes, int64_t now_wall_ms,
                                std::vector<DroppedChunk>& dropped, std::string& error) {
  const std::string directory = session_directory(entry.id);
  TimelineRetentionTicket removing = TimelineRetentionTicket::reserve_chunks(
      entry.chunks, entry.id, minutes, now_wall_ms,
      [&directory](const SessionChunk& chunk) { return path_inside(chunk.file, directory); });
  return session_store_drop_chunks(entry, removing.reserved(), dropped, error);
}

void session_store_delete_chunk_files(const std::vector<DroppedChunk>& dropped, std::vector<DroppedChunk>& still_held) {
  // No store lock: this touches files and their pictures only, never an index or the store's own state. Held under it, a
  // delete that stalled a disk would stall the tick's next index write, which takes the store's lock.
  still_held.clear();
  for (const DroppedChunk& item : dropped) {
    if (!session_id_is_safe(item.session_id) || !path_inside(item.chunk.file, session_directory(item.session_id))) {
      continue;
    }
    // The file was renamed when its chunk was dropped (session_store_drop_chunks); a chunk dropped by another route still has its own name.
    const std::string renamed = item.chunk.file + ".expired";
    const bool had_renamed = GetFileAttributesA(renamed.c_str()) != INVALID_FILE_ATTRIBUTES;
    const std::string& target = had_renamed ? renamed : item.chunk.file;
    if (DeleteFileA(target.c_str()) != 0 || GetFileAttributesA(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
      session_thumbs_remove(item.session_id, timeline_thumb_key(item.chunk.file));
    } else {
      still_held.push_back(item);
    }
  }
  if (!still_held.empty()) BX_LOG_WARN("timeline footage is still open and was not pruned: %s", still_held.front().chunk.file.c_str());
}

namespace {
// The disk wait a test can hold open (timeline_retention.h). Nothing in a shipped build calls it.
inline void retention_pause(const char* stage, const std::string& path) {
#ifdef BUFFRX_TEST_HOOKS
  if (timeline_store_pause_hook()) timeline_store_pause_hook()(stage, path);
#else
  (void)stage;
  (void)path;
#endif
}

// Whether an interrupted session is old enough for its empty shell to go: after its retention window, and only when
// recovery found no media (timeline_pins.h's rule, read on a caller's copy).
inline bool interrupted_shell_expired(const SessionEntry& entry, int minutes, int64_t now_wall_ms) {
  return entry.interrupted && entry.started_at > 0 &&
         now_wall_ms - entry.started_at * 1000 > static_cast<int64_t>(minutes) * 60000;
}

// Drops the chunk files a sweep has deleted from the session's index as it says NOW, never from the snapshot the
// sweep decided on: a chunk the writer published (or recovery found) while the files were being deleted stays
// indexed and on disk, and a session another route already removed is not written back - rewriting it would
// recreate its index in a directory a failed delete left behind. `exists` is false when there is no index left.
// Called with the store's lock held, which is what write_index expects.
void update_index_after_delete_locked(const std::string& id, const std::vector<std::string>& gone, bool& exists,
                                      SessionEntry& fresh, std::string& error) {
  exists = false;
  error.clear();
  std::string load_error;
  if (!session_store_load_one(id, fresh, load_error)) return;
  if (fresh.kind != "timeline") return;
  exists = true;
  const size_t before = fresh.chunks.size();
  fresh.chunks.erase(std::remove_if(fresh.chunks.begin(), fresh.chunks.end(),
                                    [&gone](const SessionChunk& chunk) {
                                      return std::any_of(gone.begin(), gone.end(), [&](const std::string& file) {
                                        return same_file(chunk.file, file);
                                      });
                                    }),
                     fresh.chunks.end());
  if (fresh.chunks.size() == before) return;
  write_index(fresh, error);
}
}  // namespace

bool session_store_prune_timeline(int minutes, int64_t now_wall_ms, std::string& error,
                                 const std::string& open_id) {
  error.clear();
  if (minutes <= 0) return true;
  std::string load_error;
  std::vector<SessionEntry> entries = session_store_load(load_error);
  if (!load_error.empty()) error = load_error;
  const auto media_empty = [](const SessionEntry& entry) {
    WIN32_FIND_DATAA found{};
    HANDLE search = FindFirstFileA((session_media_dir(entry.id) + "\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
      const DWORD reason = GetLastError();
      return reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND;
    }
    bool empty = true;
    for (;;) {
      const std::string name = found.cFileName;
      if (name != "." && name != "..") { empty = false; break; }
      if (!FindNextFileA(search, &found)) {
        if (GetLastError() != ERROR_NO_MORE_FILES) empty = false;
        break;
      }
    }
    FindClose(search);
    return empty;
  };
  for (SessionEntry& entry : entries) {
    // A normal recording is the person's footage; only the Timeline's temporary store is pruned here.
    if (entry.kind != "timeline") continue;
    // The session being written trims its own list (session_store_drop_chunks): see the declaration.
    if (!open_id.empty() && entry.id == open_id) continue;
    // A crash between a chunk's rename and its delete leaves "<chunk>.expired" that the index no
    // longer names; sweep it before deciding this session's media is empty.
    if (!writer_is_live(entry.id)) remove_expired_leftovers(session_media_dir(entry.id));
    // A previously emptied session has no expired chunks to trigger the normal cleanup below. An
    // interrupted session is eligible only after its retention window and after recovery found no media.
    const bool interrupted_expired = interrupted_shell_expired(entry, minutes, now_wall_ms);
    const std::string directory = session_directory(entry.id);
    if ((entry.closed || interrupted_expired) && entry.chunks.empty() && !writer_is_live(entry.id) && media_empty(entry)) {
      // Nothing is left to keep: the whole session goes. Its removal is reserved like a chunk's, so a lease asked for
      // while its index and directory are being deleted is refused instead of granted over footage that is going.
      TimelineRetentionTicket removing = TimelineRetentionTicket::reserve_session(entry.id, now_wall_ms);
      if (!removing.active()) continue;  // another removal owns this session; the next sweep finishes it
      // The snapshot above may be older than this claim, so the decision is made again on the index as it is now: a
      // chunk published or recovered meanwhile means the footage is not empty after all and must not be deleted.
      SessionEntry fresh;
      bool index_exists = false;
      std::string load_error;
      {
        std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
        index_exists = session_store_load_one(entry.id, fresh, load_error);
      }
      if (!index_exists || !fresh.chunks.empty() || writer_is_live(fresh.id) || !media_empty(fresh) ||
          !(fresh.closed || interrupted_shell_expired(fresh, minutes, now_wall_ms))) {
        continue;
      }
      std::string delete_error;
      retention_pause("session", directory);
      if (!session_store_delete(fresh, delete_error) && error.empty()) error = delete_error;
      continue;
    }
    // Candidates by the window and by the leases as they stand now, reserved in one short step under the gate. The
    // deletes and the index write that follow wait on the file system with the gate free, so a save that asks for a
    // lease meanwhile is answered at once - refused while this session's footage is going (timeline_retention.h).
    TimelineRetentionTicket removing = TimelineRetentionTicket::reserve_chunks(
        entry.chunks, entry.id, minutes, now_wall_ms,
        [&directory](const SessionChunk& chunk) { return path_inside(chunk.file, directory); });
    if (!removing.active()) continue;
    std::vector<char> gone(entry.chunks.size(), 0);
    for (size_t index : removing.reserved()) {
      const SessionChunk& chunk = entry.chunks[index];
      retention_pause("delete", chunk.file);
      // A file that is already gone leaves the index too: the index is the truth about what exists.
      gone[index] = DeleteFileA(chunk.file.c_str()) != 0 ||
                    GetFileAttributesA(chunk.file.c_str()) == INVALID_FILE_ATTRIBUTES;
      if (gone[index]) {
        // Its pictures go with it. They are named after the chunk, so a later chunk can never be served one of them -
        // but they would sit in the session's folder until the session itself expired.
        session_thumbs_remove(entry.id, timeline_thumb_key(chunk.file));
        continue;
      }
      // A file Windows still holds stays in the index and is named: a delete that did not happen is never reported as
      // one, or the preview would point at nothing.
      if (error.empty()) error = "footage is still open: " + chunk.file;
      BX_LOG_WARN("timeline footage is still open and was not pruned: %s", chunk.file.c_str());
    }
    bool removed_any = false;
    std::vector<std::string> gone_files;
    for (size_t index = 0; index < entry.chunks.size(); ++index) {
      if (!gone[index]) continue;
      removed_any = true;
      gone_files.push_back(entry.chunks[index].file);
    }
    if (!removed_any) continue;  // nothing went: the reservation is dropped and the next sweep tries again
    // The index is written before the reservation ends: a lease that read the index before this must see the removal's
    // number change, never the old list with the footage already gone. It is rewritten from what it says at this
    // moment, never from the snapshot this pass read before it waited on the disk.
    retention_pause("index", session_index_path(entry.id));
    SessionEntry fresh;
    bool index_exists = false;
    std::string write_error;
    {
      std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
      update_index_after_delete_locked(entry.id, gone_files, index_exists, fresh, write_error);
    }
    if (!write_error.empty() && error.empty()) error = write_error;
    // Gone: another route removed the session while this sweep waited, and writing its index back would resurrect it.
    if (!index_exists) continue;
    // A closed or abandoned session with no footage left is an empty directory and an index nobody reads. The decision
    // is made on the index as it is now: a chunk published or recovered while the files were going keeps the session.
    if (!(fresh.closed || interrupted_shell_expired(fresh, minutes, now_wall_ms)) || !fresh.chunks.empty() ||
        writer_is_live(fresh.id) || !media_empty(fresh)) {
      continue;
    }
    removing.release();
    TimelineRetentionTicket whole = TimelineRetentionTicket::reserve_session(entry.id, now_wall_ms);
    if (!whole.active()) continue;
    std::string delete_error;
    retention_pause("session", directory);
    if (!session_store_delete(fresh, delete_error) && error.empty()) error = delete_error;
  }
  return error.empty();
}

bool session_store_erase_timeline(const std::string& id, std::vector<std::string>& locked, std::string& error,
                                  const TimelineRetentionTicket* claim) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  locked.clear();
  error.clear();
  SessionEntry entry;
  if (!session_store_load_one(id, entry, error)) return false;
  if (entry.kind != "timeline") {
    error = "only a timeline session can be erased";
    BX_LOG_WARN("refusing to erase %s: it is not a timeline session", id.c_str());
    return false;
  }
  // The whole session is going. A lease asked for it while this runs is refused rather than granted over files that are
  // being deleted, and it is answered at once: the gate is not held across any of the deletes below. A lease granted
  // before this point is a reader still using the footage (a save, a preview, an open editor), so the erase is refused
  // instead of pulling its files away; the one exception is the ring's own dedupe lease, which is renewed every second
  // and would otherwise keep the person's erase out for as long as the buffer runs. The claim and a lease are decided
  // under the one gate, so no check-then-reserve gap is left for a lease to slip through (timeline_retention.h).
  TimelineRetentionTicket own_claim;
  if (claim == nullptr || !claim->active()) {
    std::string leased_by;
    own_claim = TimelineRetentionTicket::claim_session(entry.id, static_cast<int64_t>(std::time(nullptr)) * 1000,
                                                       kTimelineRingLeaseOwner, leased_by);
    if (!own_claim.active()) {
      error = leased_by.empty() ? "that footage is being removed; try again in a moment"
                                : "that footage is in use by a save or a preview; try again in a moment";
      BX_LOG_WARN("timeline session %s was not erased: %s", id.c_str(), error.c_str());
      return false;
    }
  }
  const std::string directory = session_directory(entry.id);
  const std::string media = session_media_dir(entry.id);
  // The wait a test holds open: the claim is already in force here, so a lease asked for meanwhile is refused.
  retention_pause("session", directory);
  // Every file in the media directory is this session's temporary footage: an indexed chunk and one whose
  // writer has not published it yet. Deleting by directory means a tampered index can never reach outside
  // the session, and a saved library clip, which lives elsewhere, is never touched.
  WIN32_FIND_DATAA found{};
  HANDLE search = FindFirstFileA((media + "\\*").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
      const std::string file = join(media, found.cFileName);
      if (DeleteFileA(file.c_str()) != 0) continue;
      if (GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;  // already gone
      locked.push_back(file);
    } while (FindNextFileA(search, &found) != 0);
    FindClose(search);
  }
  // The preview's pictures are derived and never locked by anything that matters: they go even while a chunk
  // is still held, so the retry that finishes the erase finds only what it must wait for.
  session_thumbs_remove(entry.id, std::string());
  if (!locked.empty()) {
    // The index keeps exactly the files Windows still holds, so the rest of the session is gone and what
    // is left can be erased once the writer has let go.
    SessionEntry remaining = entry;
    remaining.chunks.clear();
    for (const SessionChunk& chunk : entry.chunks) {
      if (std::find(locked.begin(), locked.end(), chunk.file) != locked.end()) remaining.chunks.push_back(chunk);
    }
    std::string write_error;
    if (!write_index(remaining, write_error)) {
      error = write_error;
      return false;
    }
    error = "the session is still open: " + locked.front();
    BX_LOG_WARN("timeline session %s was not erased: %s", id.c_str(), error.c_str());
    return false;
  }
  // The media folder first, then the index (a directory holding it is not empty), then the session's own
  // directory. Anything left behind after that is reported, never claimed.
  const bool media_gone = RemoveDirectoryA(media.c_str()) != 0 || !directory_exists(media);
  DeleteFileA(session_index_path(entry.id).c_str());
  // A marker file left behind is what keeps the session's own directory from being removed.
  DeleteFileA(session_markers_path(entry.id).c_str());
  // The writer's state file sits beside the index, so the session's own directory cannot go without it.
  // It goes only once the footage has: a session Windows still holds must not read back as dead.
  if (media_gone) DeleteFileA(writer_state_path(entry.id).c_str());
  const bool directory_gone = RemoveDirectoryA(directory.c_str()) != 0 || !directory_exists(directory);
  if (media_gone && directory_gone) {
    BX_LOG_INFO("timeline session erased: %s", id.c_str());
    return true;
  }
  if (!directory_gone) {
    // The index goes back, so the session can still be found and a later erase can finish the job.
    std::string rewrite_error;
    write_index(entry, rewrite_error);
  }
  error = "the session directory could not be removed: " + directory;
  BX_LOG_WARN("timeline session %s was not erased: %s", id.c_str(), error.c_str());
  return false;
}


}  // namespace bx

namespace bx {

std::string marker_kind_for_command(const std::string& command) {
  if (command == "buffer.save") return "clip";
  if (command == "screenshot.take") return "screenshot";
  if (command == "recording.start") return "recording";
  if (command == "profiles.apply") return "profile";
  // The game capture starting on a game: the Timeline names its session after it and pins the switch.
  if (command == "game.capture") return "game";
  return "";
}

bool session_older(const SessionEntry& a, const SessionEntry& b) {
  if (a.started_at != 0 && b.started_at != 0 && a.started_at != b.started_at) return a.started_at < b.started_at;
  return a.id < b.id;
}

bool session_prune(int64_t budget_bytes, const std::string& open_id, std::vector<std::string>& removed,
                   int64_t& freed_bytes, int64_t& total_bytes, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(g_store_mutex);
  removed.clear();
  freed_bytes = 0;
  total_bytes = 0;
  error.clear();
  std::vector<SessionEntry> entries = session_store_load(error);
  // The Timeline's footage has its own window and pins (timeline_pins.h); this budget counts and prunes
  // real recordings only, so Timeline can never take a clip's place in it.
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const SessionEntry& entry) { return entry.kind == "timeline"; }),
                entries.end());
  for (SessionEntry& entry : entries) {
    total_bytes += entry.bytes();
    // A crashed session is finished footage; the live one reads as "interrupted" too and must not be.
    entry.closed = entry.id != open_id && (entry.closed || entry.interrupted);
  }
  for (size_t index : retention_prune_order(entries, budget_bytes)) {
    std::string reason;
    if (!session_store_delete(entries[index], reason)) {
      BX_LOG_WARN("session %s was not pruned: %s", entries[index].id.c_str(), reason.c_str());
      if (error.empty()) error = reason;
      continue;
    }
    removed.push_back(entries[index].id);
    freed_bytes += entries[index].bytes();
    total_bytes -= entries[index].bytes();
    BX_LOG_INFO("pruned session %s (%lld bytes)", entries[index].id.c_str(),
                static_cast<long long>(entries[index].bytes()));
  }
  return error.empty();
}

bool recording_space_ok(const std::string& clips_dir, int64_t free_bytes, int min_free_gb, std::string& reason) {
  reason.clear();
  if (min_free_gb <= 0 || free_bytes < 0) return true;
  const int64_t floor = static_cast<int64_t>(min_free_gb) * 1024 * 1024 * 1024;
  if (free_bytes >= floor) return true;
  char text[256];
  std::snprintf(text, sizeof(text), "only %.1f GB free where recordings go (%s); recording needs at least %d GB",
                static_cast<double>(free_bytes) / (1024.0 * 1024 * 1024), clips_dir.c_str(), min_free_gb);
  reason = text;
  return false;
}

}  // namespace bx
