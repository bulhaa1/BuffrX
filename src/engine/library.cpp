#include "library.h"

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "../shared/files.h"
#include "../shared/log.h"
#include "session_store.h"

namespace bx {
namespace {

// Set by library_init (under the engine lock) and read by commands that run outside it, so the paths
// are only ever read as a whole copy.
LibraryPaths g_paths;
std::mutex g_paths_mutex;
LibraryPaths paths_now() {
  std::lock_guard<std::mutex> lock(g_paths_mutex);
  return g_paths;
}

// Saves in flight (library_hold): compared case-insensitively, as Windows paths are.
std::vector<std::string> g_held;
std::mutex g_held_mutex;
std::string held_key(const std::string& file) {
  std::string key;
  for (char c : file) key.push_back(c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return key;
}

std::string lower(const std::string& value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool is_directory(const std::string& path) {
  DWORD attributes = GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensure_directory(const std::string& path) {
  if (path.empty()) return false;
  if (is_directory(path)) return true;
  std::string partial;
  for (size_t i = 0; i < path.size(); ++i) {
    partial.push_back(path[i]);
    const bool last = i + 1 == path.size();
    if (path[i] == '\\' || path[i] == '/' || last) {
      if (partial.size() <= 3) continue;
      if (!is_directory(partial) && !CreateDirectoryA(partial.c_str(), nullptr) &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
      }
    }
  }
  return is_directory(path);
}

bool has_extension(const std::string& file, const std::vector<std::string>& extensions) {
  const std::string lower_file = lower(file);
  for (const auto& extension : extensions) {
    if (lower_file.size() >= extension.size() &&
        lower_file.compare(lower_file.size() - extension.size(), extension.size(), extension) == 0) {
      return true;
    }
  }
  return false;
}

int64_t file_size(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  LARGE_INTEGER size;
  size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
  size.LowPart = data.nFileSizeLow;
  return size.QuadPart;
}

int64_t file_modified(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  ULARGE_INTEGER time;
  time.HighPart = data.ftLastWriteTime.dwHighDateTime;
  time.LowPart = data.ftLastWriteTime.dwLowDateTime;
  // FILETIME is 100 ns ticks since 1601; convert to unix seconds.
  return static_cast<int64_t>(time.QuadPart / 10000000ULL) - 11644473600LL;
}

std::string stem_of(const std::string& file) {
  size_t slash = file.find_last_of("\\/");
  std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
  size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

uint64_t fnv1a(const std::string& text) {
  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}



}  // namespace

int thumbnail_budget(int visible_count, int listed_count) {
  // What the panel can draw, plus one screen of lookahead so a scroll does not stall, and never more
  // clips than exist. Zero visible means nothing is on screen, so nothing is decoded up front. It
  // lives here rather than beside library_annotate because it is pure policy: this file is the one
  // the test binary links, and library_media.cpp needs the FFmpeg DLLs.
  if (visible_count <= 0 || listed_count <= 0) return 0;
  return std::min(listed_count, visible_count + std::max(visible_count, 8));
}

bool annotate_should_skip(const std::string& path, uint64_t now_ms, bool held, bool writer_open, bool in_clips_root,
                          uint64_t modified_ms) {
  if (path.empty()) return true;  // nothing to open
  // A file a save is trimming and moving is never opened: FFmpeg holding it for a thumbnail made the
  // save's trim and move fail.
  if (held) return true;
  // Anything a writer still holds waits for the next listing, wherever it is: a save older than four
  // seconds can still be growing, and so can a recording chunk or an export.
  if (writer_open) return true;
  // A raw replay lands in the clips root (finished clips live in folders) and is still being written
  // for a moment before the save knows its path, so a fresh root file waits too.
  if (!in_clips_root) return false;
  const int64_t age_ms = static_cast<int64_t>(now_ms) - static_cast<int64_t>(modified_ms);
  return age_ms < 4000;
}

bool annotate_writer_probe_needed(uint64_t now_ms, uint64_t modified_ms) {
  // A writer moves the mtime on every write, and a save's trim or a chunk's mux is seconds long, so a file
  // whose mtime has not moved for a minute cannot be held by one: the listing lists it without opening it.
  // Before this, every clip of a 5,000-clip library was opened here and a cold listing took 16 s (overnight
  // N10, docs/overnight-2026-11-02.md).
  const int64_t age_ms = static_cast<int64_t>(now_ms) - static_cast<int64_t>(modified_ms);
  return age_ms < 60000;
}

LibraryPaths library_paths_from(const Config& config, const std::string& data_root) {
  LibraryPaths paths;
  paths.clips_dir = config.clips_dir;
  paths.screenshots_dir = config.screenshots_dir;
  paths.thumbnails_dir = data_root + "\\thumbnails";
  paths.recordings_dir = recordings_base_dir(config);
  return paths;
}

void library_init(const LibraryPaths& paths) {
  LibraryPaths resolved = paths;
  if (resolved.recordings_dir.empty()) resolved.recordings_dir = resolved.clips_dir;
  {
    std::lock_guard<std::mutex> lock(g_paths_mutex);
    g_paths = resolved;
  }
  ensure_directory(resolved.clips_dir);
  ensure_directory(resolved.screenshots_dir);
  ensure_directory(resolved.thumbnails_dir);
  ensure_directory(resolved.recordings_dir);
}

void library_paths(LibraryPaths& out) { out = paths_now(); }

uint64_t library_file_key(const std::string& file) { return fnv1a(file); }

std::string media_folder_name(const std::string& title) {
  std::string text = title;
  // A trailing " - Something" is a renderer or class tag ("Counter-Strike 2 - Direct3D 11"), dropped
  // only when something precedes it.
  const size_t dash = text.rfind(" - ");
  if (dash != std::string::npos && dash > 0) text = text.substr(0, dash);
  std::string kept;
  for (unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) continue;
    if (std::strchr("<>:\"/\\|?*", static_cast<char>(c)) != nullptr) continue;
    kept.push_back(static_cast<char>(c));
  }
  // Collapse whitespace runs and trim, then the 100-character cut, then no trailing dot or space
  // (Windows silently strips them, so the folder would not be the one asked for).
  std::string collapsed;
  for (char c : kept) {
    const bool space = c == ' ' || c == '\t';
    if (space && (collapsed.empty() || collapsed.back() == ' ')) continue;
    collapsed.push_back(space ? ' ' : c);
  }
  if (collapsed.size() > 100) collapsed.resize(100);
  while (!collapsed.empty() && (collapsed.back() == ' ' || collapsed.back() == '.')) collapsed.pop_back();
  if (collapsed.empty()) return "Unknown";
  static const char* const reserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                         "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                         "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  const std::string upper_base = [&] {
    std::string base = collapsed.substr(0, collapsed.find('.'));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return base;
  }();
  for (const char* name : reserved) {
    if (upper_base == name) return "Unknown";
  }
  return collapsed;
}

std::string timestamped_stem(const std::string& prefix, int year, int month, int day, int hour, int minute,
                             int second) {
  char text[64];
  std::snprintf(text, sizeof(text), " %04d-%02d-%02d %02d-%02d-%02d", year, month, day, hour, minute, second);
  return prefix + text;
}

std::string timestamped_stem_now(const std::string& prefix) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  return timestamped_stem(prefix, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
}

std::string reserve_output_path(const std::string& directory, const std::string& stem, const std::string& extension) {
  ensure_directory(directory);
  // The stem can be a client's name for a clip or a screenshot, so it is stripped the way library_rename
  // strips a new name: a separator or a `..` in it would put the file outside the directory asked for.
  std::string cleaned;
  for (char c : stem) {
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') continue;
    if (static_cast<unsigned char>(c) < 0x20) continue;  // a pasted name brings no control characters along
    cleaned.push_back(c);
  }
  for (size_t dot = cleaned.find(".."); dot != std::string::npos; dot = cleaned.find("..")) cleaned.erase(dot, 2);
  // Windows drops a trailing space or dot and refuses the DOS device names, and a name stripped to nothing
  // would be a file called ".mp4": each of those was a save that failed, or a clip nobody could find.
  while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) cleaned.pop_back();
  while (!cleaned.empty() && cleaned.front() == ' ') cleaned.erase(0, 1);
  if (cleaned.empty()) cleaned = "Clip";
  std::string device = cleaned.substr(0, cleaned.find('.'));
  for (char& c : device) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" ||
      (device.size() == 4 && (device.rfind("COM", 0) == 0 || device.rfind("LPT", 0) == 0) && device[3] >= '1' &&
       device[3] <= '9')) {
    cleaned = "_" + cleaned;
  }
  std::string candidate = directory + "\\" + cleaned + extension;
  int counter = 1;
  while (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
    candidate = directory + "\\" + cleaned + " (" + std::to_string(counter++) + ")" + extension;
  }
  return candidate;
}

namespace {

// The same path written two ways is the same path: the library keeps Windows separators and a
// client may well send the forward slashes libobs asks for. Comparing the two forms literally
// refused a file that was in the library, which is how a command that worked from the panel
// failed from anywhere else.
// `..` is resolved before the comparison: a prefix test alone let "<clips>\..\..\anything" and a
// sibling folder named "<clips>-old" pass as library paths, and delete, rename and open trust this.
std::string normalise_path(const std::string& path) {
  if (path.empty()) return std::string();
  const DWORD needed = GetFullPathNameA(path.c_str(), 0, nullptr, nullptr);
  if (needed == 0) return std::string();
  std::string out(needed, '\0');
  const DWORD written = GetFullPathNameA(path.c_str(), needed, out.data(), nullptr);
  if (written == 0 || written >= needed) return std::string();
  out.resize(written);
  out = lower(out);
  std::replace(out.begin(), out.end(), '/', '\\');
  while (out.size() > 1 && out.back() == '\\') out.pop_back();
  return out;
}

// One rule for "inside a folder", so delete, rename and the library check cannot drift apart. The
// separator after the root is what makes it a folder of its own: a prefix alone let "<root>-old"
// pass as "<root>".
bool path_inside_root(const std::string& path, const std::string& root) {
  const std::string normal_path = normalise_path(path);
  const std::string normal_root = normalise_path(root);
  return !normal_root.empty() && normal_path.size() > normal_root.size() + 1 &&
         normal_path.compare(0, normal_root.size(), normal_root) == 0 && normal_path[normal_root.size()] == '\\';
}

}  // namespace

bool library_owns_path(const std::string& file) {
  const LibraryPaths paths = paths_now();
  return path_inside_root(file, paths.clips_dir) || path_inside_root(file, paths.screenshots_dir) ||
         path_inside_root(file, paths.recordings_dir) || path_inside_root(file, session_store_root());
}

namespace {

std::vector<MediaItem> list_directory(const std::string& directory, const std::vector<std::string>& extensions,
                                      const std::string& folder = std::string()) {
  std::vector<MediaItem> items;
  if (directory.empty()) return items;
  WIN32_FIND_DATAA entry{};
  HANDLE find = FindFirstFileA((directory + "\\*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return items;
  do {
    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::string name = entry.cFileName;
    if (!has_extension(name, extensions)) continue;
    // A save's trim writes beside the clip before it replaces it; never list the half-made copy.
    if (lower(name).find(".trim.") != std::string::npos || lower(name).find(".part") != std::string::npos) continue;
    MediaItem item;
    item.file = directory + "\\" + name;
    if (library_is_held(item.file)) continue;  // the save pipeline is still trimming and moving it
    item.name = stem_of(item.file);
    item.size = file_size(item.file);
    item.modified = file_modified(item.file);
    item.folder = folder;
    items.push_back(item);
  } while (FindNextFileA(find, &entry));
  FindClose(find);
  std::sort(items.begin(), items.end(),
            [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
  return items;
}

const std::vector<std::string>& video_extensions() {
  static const std::vector<std::string> extensions = {".mp4", ".mkv", ".mov", ".avi", ".webm"};
  return extensions;
}

}  // namespace

void library_hold(const std::string& file) {
  std::lock_guard<std::mutex> lock(g_held_mutex);
  g_held.push_back(held_key(file));
}

void library_release(const std::string& file) {
  std::lock_guard<std::mutex> lock(g_held_mutex);
  const auto found = std::find(g_held.begin(), g_held.end(), held_key(file));
  if (found != g_held.end()) g_held.erase(found);
}

bool library_is_held(const std::string& file) {
  std::lock_guard<std::mutex> lock(g_held_mutex);
  return std::find(g_held.begin(), g_held.end(), held_key(file)) != g_held.end();
}

std::vector<MediaItem> list_clips() {
  // The root (clips from builds before the folder layout) and one level of folders: a game folder,
  // Display Capture, Game Capture, Edits. Recordings, Screenshots and the session store are not clips
  // and have their own listings.
  const LibraryPaths paths = paths_now();
  std::vector<MediaItem> items = list_directory(paths.clips_dir, video_extensions());
  if (paths.clips_dir.empty()) return items;
  const std::string sessions = lower(session_store_root());
  const std::string screenshots = lower(paths.screenshots_dir);
  const std::string thumbnails = lower(paths.thumbnails_dir);
  WIN32_FIND_DATAA entry{};
  HANDLE find = FindFirstFileA((paths.clips_dir + "\\*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return items;
  do {
    if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    const std::string name = entry.cFileName;
    if (name == "." || name == ".." || name[0] == '.') continue;
    const std::string folder_path = paths.clips_dir + "\\" + name;
    const std::string lower_path = lower(folder_path);
    if (lower(name) == lower(kRecordingsFolder) || lower(name) == lower(kScreenshotsFolder) ||
        lower(name) == "sessions" || lower_path == sessions || lower_path == screenshots || lower_path == thumbnails) {
      continue;
    }
    const std::vector<MediaItem> inside = list_directory(folder_path, video_extensions(), name);
    items.insert(items.end(), inside.begin(), inside.end());
  } while (FindNextFileA(find, &entry));
  FindClose(find);
  std::sort(items.begin(), items.end(),
            [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
  return items;
}

std::vector<MediaItem> list_recordings() {
  const std::vector<std::string>& extensions = video_extensions();
  std::vector<MediaItem> items;
  // The two layouts this product writes: single-file recordings, and one media directory per session.
  // A general recursive walk would pick up whatever a user drops in the folder tree, which the delete
  // path does not expect.
  const std::vector<MediaItem> single = list_directory(paths_now().recordings_dir + "\\" + kRecordingsFolder, extensions,
                                                       kRecordingsFolder);
  items.insert(items.end(), single.begin(), single.end());

  std::string error;
  for (const SessionEntry& entry : session_store_load(error)) {
    // The Timeline's footage is temporary and its own store's; the recordings list is the person's.
    if (entry.kind == "timeline") continue;
    const std::vector<MediaItem> chunks = list_directory(session_media_dir(entry.id), extensions, kRecordingsFolder);
    items.insert(items.end(), chunks.begin(), chunks.end());
  }
  std::sort(items.begin(), items.end(),
            [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
  return items;
}

void library_recordings_usage(int64_t& bytes, int& count) {
  const std::vector<MediaItem> items = list_recordings();
  bytes = 0;
  count = static_cast<int>(items.size());
  for (const MediaItem& item : items) bytes += item.size;
}

void library_sessions_usage(int64_t& bytes, int& count) {
  std::string error;
  const std::vector<SessionEntry> sessions = session_store_load(error);
  bytes = 0;
  count = static_cast<int>(sessions.size());
  for (const SessionEntry& entry : sessions) bytes += entry.bytes();
}

std::vector<MediaItem> list_screenshots() {
  const std::string root = paths_now().screenshots_dir;
  if (root.empty()) return {};
  std::vector<MediaItem> items = list_directory(root, {".png", ".jpg", ".jpeg"});
  WIN32_FIND_DATAA entry{};
  HANDLE find = FindFirstFileA((root + "\\*").c_str(), &entry);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
      const std::string name = entry.cFileName;
      if (name == "." || name == ".." || name[0] == '.') continue;
      const auto inside = list_directory(root + "\\" + name, {".png", ".jpg", ".jpeg"}, name);
      items.insert(items.end(), inside.begin(), inside.end());
    } while (FindNextFileA(find, &entry));
    FindClose(find);
  }
  std::sort(items.begin(), items.end(), [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
  return items;
}

namespace {

// Runs a file operation, and runs it again for a moment if another reader holds the file. A clip is
// opened briefly by the engine itself - its length is read after a save, a thumbnail is decoded when a
// card scrolls into view - and FFmpeg opens without sharing delete, so a rename or a delete landing in
// that instant failed with "could not be renamed" although nothing was wrong.
template <typename Operation>
bool retry_while_shared(Operation operation) {
  for (int attempt = 0;; ++attempt) {
    if (operation()) return true;
    const DWORD error = GetLastError();
    if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) || attempt >= 20) return false;
    Sleep(100);
  }
}

// The cached pictures of one file, keyed by its path: the card's thumbnail and the preview frames.
// A rename moves the file, so the old paths must go or nothing ever lists them again.
void delete_cached_images(const std::string& file) {
  char key[64];
  std::snprintf(key, sizeof(key), "%016llx.jpg", static_cast<unsigned long long>(fnv1a(file)));
  DeleteFileA((paths_now().thumbnails_dir + "\\" + key).c_str());
  for (int i = 0; i < kPreviewFrames; ++i) {
    std::snprintf(key, sizeof(key), "%016llx-f%d.jpg", static_cast<unsigned long long>(fnv1a(file)), i);
    DeleteFileA((paths_now().thumbnails_dir + "\\" + key).c_str());
  }
}

}  // namespace

bool library_delete(const std::string& file) {
  if (!library_can_remove(file)) return false;
  if (!retry_while_shared([&] { return DeleteFileA(file.c_str()) != FALSE; })) return false;
  // Its thumbnail and preview frames, if any, go with it.
  delete_cached_images(file);
  return true;
}

bool library_can_remove(const std::string& file) {
  if (!library_owns_path(file)) {
    BX_LOG_WARN("refusing to delete a file outside the library: %s", file.c_str());
    return false;
  }
  // A chunk is one piece of a session: deleting it on its own would leave an index pointing at a file
  // that is gone. Sessions are deleted whole, which the session screen does.
  if (path_inside_root(file, session_store_root())) {
    BX_LOG_WARN("refusing to delete one chunk of a session: %s", file.c_str());
    return false;
  }
  const DWORD attributes = GetFileAttributesA(file.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool library_trash(const std::string& file) {
  if (!library_can_remove(file)) return false;
  // The shell item this goes through refuses a path that mixes the engine's two separators.
  const std::string native = bx::native_path(file);
  const std::wstring wide = utf8_to_wide(native);

  // IFileOperation is apartment-threaded. A command worker has no COM yet; a thread that already has
  // it in another mode keeps it, because the operation still works through COM's own marshalling.
  const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialise = initialised == S_OK || initialised == S_FALSE;
  bool recycled = false;
  HRESULT create = E_FAIL;
  HRESULT item_result = E_FAIL;
  HRESULT flags_result = E_FAIL;
  HRESULT delete_result = E_FAIL;
  HRESULT perform_result = E_FAIL;
  BOOL aborted = FALSE;
  IFileOperation* operation = nullptr;
  IShellItem* item = nullptr;
  create = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation));
  if (SUCCEEDED(create)) item_result = SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&item));
  if (SUCCEEDED(create) && SUCCEEDED(item_result)) {
    // FOFX_RECYCLEONDELETE makes a volume without a Recycle Bin fail instead of deleting for good.
    const DWORD flags = FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    flags_result = operation->SetOperationFlags(flags);
    if (SUCCEEDED(flags_result)) delete_result = operation->DeleteItem(item, nullptr);
    if (SUCCEEDED(delete_result)) perform_result = operation->PerformOperations();
    if (SUCCEEDED(perform_result)) perform_result = operation->GetAnyOperationsAborted(&aborted);
    if (SUCCEEDED(perform_result)) recycled = aborted == FALSE && GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES;
  }
  if (item) item->Release();
  if (operation) operation->Release();
  if (uninitialise) CoUninitialize();
  if (!recycled) {
    // The numbers are what says why: a COM mode the worker already had, a shell item that would not parse, or
    // an operation the shell refused. One line, so a person's "it did not delete" has an answer in the log.
    BX_LOG_WARN("could not move a file to the Recycle Bin: %s (com 0x%08lx, create 0x%08lx, item 0x%08lx, flags 0x%08lx, "
                "delete 0x%08lx, perform 0x%08lx, aborted %d)",
                file.c_str(), static_cast<unsigned long>(initialised), static_cast<unsigned long>(create),
                static_cast<unsigned long>(item_result), static_cast<unsigned long>(flags_result),
                static_cast<unsigned long>(delete_result), static_cast<unsigned long>(perform_result),
                static_cast<int>(aborted));
    return false;
  }
  delete_cached_images(file);
  return true;
}

void library_forget_cached_images(const std::string& file) {
  delete_cached_images(file);
}

bool library_rename(const std::string& file, const std::string& new_name, std::string& result_path) {
  if (!library_owns_path(file)) {
    BX_LOG_WARN("refusing to rename a file outside the library: %s", file.c_str());
    return false;
  }
  // A chunk is one piece of a session: renaming it on its own would leave an index pointing at a file
  // that is gone, exactly as deleting it would. A session has no name of its own to change.
  if (path_inside_root(file, session_store_root())) {
    BX_LOG_WARN("refusing to rename one chunk of a session: %s", file.c_str());
    return false;
  }
  std::string cleaned;
  for (char c : new_name) {
    // A control byte is refused, not stripped: a NUL ends the name at the Win32 call while the reply
    // would still carry it, so the file took a name the library's extension filter never lists.
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f) return false;
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') continue;
    cleaned.push_back(c);
  }
  // The caller may pass a name with an extension; keep the original extension regardless.
  size_t last_dot = cleaned.find_last_of('.');
  if (last_dot != std::string::npos) cleaned = cleaned.substr(0, last_dot);
  // A name that is only an extension (".mp4") leaves no base name, and the file would be called ".mp4".
  if (cleaned.empty()) return false;
  size_t slash = file.find_last_of("\\/");
  const std::string directory = slash == std::string::npos ? std::string() : file.substr(0, slash);
  size_t dot = file.find_last_of('.');
  const std::string extension = dot == std::string::npos ? std::string() : file.substr(dot);
  const std::string target = directory + "\\" + cleaned + extension;
  // A name the file already has is not a collision: the Rename sheet reopens with the stem in its field, so its second
  // press sends that same name and must answer the file. A change of case alone is a real rename, which MoveFileA
  // applies to the same file; only a different file already sitting at the target is refused.
  const bool same_name = lower(target) == lower(file);
  if (same_name && target == file) {
    result_path = file;
    return true;
  }
  if (!same_name && GetFileAttributesA(target.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
  if (!retry_while_shared([&] { return MoveFileA(file.c_str(), target.c_str()) != FALSE; })) return false;
  // The caches are keyed by the old path, so the new name starts without them and nothing else
  // would ever free the old ones.
  delete_cached_images(file);
  result_path = target;
  return true;
}

void library_storage_usage(int64_t& clips_bytes, int64_t& screenshots_bytes, int& clip_count, int& screenshot_count) {
  clips_bytes = 0;
  screenshots_bytes = 0;
  auto clips = list_clips();
  auto shots = list_screenshots();
  clip_count = static_cast<int>(clips.size());
  screenshot_count = static_cast<int>(shots.size());
  for (const auto& item : clips) clips_bytes += item.size;
  for (const auto& item : shots) screenshots_bytes += item.size;
}

int64_t volume_free_bytes(const std::string& path) {
  if (path.empty()) return 0;
  ULARGE_INTEGER free_to_caller{};
  if (!GetDiskFreeSpaceExA(path.c_str(), &free_to_caller, nullptr, nullptr)) return 0;
  return static_cast<int64_t>(free_to_caller.QuadPart);
}

}  // namespace bx


