// Games whose capture hook delivered nothing, remembered so the next capture starts on the window instead of waiting
// 1.5 s for a hook that will not come (hook_memory.json in the data root, entries expire after 14 days).
//
// One failure is a loading screen or a slow injection; the same executable failing in two separate captures is the
// game (anti-cheat, a renderer the hook does not know). Decided without libobs.

#ifndef BUFFRX_HOOK_MEMORY_H
#define BUFFRX_HOOK_MEMORY_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "capacity_memory.h"

namespace buffrx {

constexpr int64_t kHookMemoryExpiryS = 14 * 24 * 3600;
constexpr int kHookMemoryFailures = 2;
// shortcut: the skip is off. A skipped hook is never tried, so a game that missed twice on a slow load (an exclusive-fullscreen
// one has nothing else that can capture it) would stay off its hook for 14 days with no way to be forgotten. Misses before the
// 2026-11-14 hook staging fix (Vulkan games never hooked) are also still in users' files. Turn it on once a skipped game is
// retried on its hook now and then (every Nth capture), so a working hook can clear its entry.
constexpr bool kHookMemorySkips = false;

struct HookEntry {
  std::string exe;  // lower case
  int failures = 0;
  int64_t last_s = 0;
};

inline std::string hook_exe_of(const std::string& window_id) {
  // The id is "title:class:exe": the executable is after the last colon.
  const size_t colon = window_id.rfind(':');
  std::string exe = colon == std::string::npos ? window_id : window_id.substr(colon + 1);
  std::transform(exe.begin(), exe.end(), exe.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return exe;
}

inline bool hook_entry_expired(const HookEntry& entry, int64_t now_s) { return now_s - entry.last_s >= kHookMemoryExpiryS; }

// A hook that delivered nothing in one capture of `exe`. A stale entry starts over.
inline void hook_memory_record(std::vector<HookEntry>& memory, const std::string& exe, int64_t now_s) {
  if (exe.empty()) return;
  memory.erase(std::remove_if(memory.begin(), memory.end(), [&](const HookEntry& e) { return hook_entry_expired(e, now_s); }),
               memory.end());
  for (HookEntry& entry : memory) {
    if (entry.exe != exe) continue;
    ++entry.failures;
    entry.last_s = now_s;
    return;
  }
  memory.push_back({exe, 1, now_s});
}

// The hook delivered a frame for `exe`: its earlier misses were a slow start, not a hook that cannot work. True when an
// entry went, so the caller writes the file only then. Without this, a game that loads slowly twice lost its hook for
// two weeks, and an exclusive-fullscreen game has nothing else that can capture it.
inline bool hook_memory_forget(std::vector<HookEntry>& memory, const std::string& exe) {
  const size_t before = memory.size();
  memory.erase(std::remove_if(memory.begin(), memory.end(), [&](const HookEntry& e) { return e.exe == exe; }), memory.end());
  return memory.size() != before;
}

// True when this capture of `exe` should start straight on the window.
inline bool hook_memory_skips_hook(const std::vector<HookEntry>& memory, const std::string& exe, int64_t now_s) {
  for (const HookEntry& entry : memory) {
    if (entry.exe == exe && entry.failures >= kHookMemoryFailures && !hook_entry_expired(entry, now_s)) return true;
  }
  return false;
}

inline std::vector<HookEntry> hook_memory_load(const std::string& path, int64_t now_s) {
  std::vector<HookEntry> memory;
  bx::Json json;
  if (!bx::capacity_parse_file(path, json) || !json["games"].is_array()) return memory;
  for (const bx::Json& item : json["games"].items()) {
    HookEntry entry{item["exe"].as_string(), static_cast<int>(item["failures"].as_int(0)), item["last"].as_int(0)};
    if (!entry.exe.empty() && !hook_entry_expired(entry, now_s)) memory.push_back(entry);
  }
  return memory;
}

inline void hook_memory_save(const std::string& path, const std::vector<HookEntry>& memory) {
  if (path.empty()) return;
  bx::Json games = bx::Json::array();
  for (const HookEntry& entry : memory) {
    bx::Json item = bx::Json::object();
    item.set("exe", bx::Json(entry.exe));
    item.set("failures", bx::Json(entry.failures));
    item.set("last", bx::Json(entry.last_s));
    games.push(item);
  }
  bx::Json json = bx::Json::object();
  json.set("games", games);
  bx::write_file_atomic(path, json.dump(2));
}

}  // namespace buffrx

#endif  // BUFFRX_HOOK_MEMORY_H
