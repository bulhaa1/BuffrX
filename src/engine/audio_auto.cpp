// The automatic routing policy. See audio_auto.h for the rules; nothing here touches Windows audio.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "audio_auto.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "audio_watch.h"

namespace bx {
namespace {

std::string lower_ascii(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string file_name_of(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

#ifdef _WIN32
std::string narrow(const wchar_t* text) {
  if (!text) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return std::string();
  std::string out(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}
#endif

// The executables BuffrX itself plays sound through. The sharing helper and the muxer are separate
// processes that link this product; a clip must never record them.
bool own_name(const std::string& exe) {
  const std::string name = lower_ascii(file_name_of(exe));
  // Every product binary is buffrx-*: the engine, the editor, the sharing helper and this probe's own
  // renamed copies. The muxer is OBS's own helper, which libobs starts with this engine.
  return name.rfind("buffrx-", 0) == 0 && name.size() > 7 && name.compare(name.size() - 4, 4, ".exe") == 0
             ? true
             : name == "obs-ffmpeg-mux.exe" || name == "ffmpeg-mux.exe";
}

struct Group {
  std::string key;
  std::string exe;
  std::string name;
  std::vector<AutoAudioRoot> roots;  // every disjoint process tree of this executable
  bool active = false;
  uint64_t changed_ms = 0;
  AutoAudioPref pref;
};

std::string state_of(bool active) { return active ? "connected" : "waiting"; }

// A parent PID can be recycled while its original child is still alive. Such a process cannot be the
// child's parent if it was created later; when Windows denies either timestamp retain the existing rule.
bool valid_parent(const AutoProcessNode& child, const AutoProcessNode& parent) {
  return child.created_ms == 0 || parent.created_ms == 0 || parent.created_ms <= child.created_ms;
}

bool pid_in_tree(const std::map<uint32_t, AutoProcessNode>& nodes, uint32_t root, uint32_t pid) {
  for (int depth = 0; depth < 64 && pid != 0; ++depth) {
    if (pid == root) return true;
    const auto found = nodes.find(pid);
    if (found == nodes.end() || found->second.parent_pid == pid) break;
    const auto parent = nodes.find(found->second.parent_pid);
    if (parent == nodes.end() || !valid_parent(found->second, parent->second)) break;
    pid = found->second.parent_pid;
  }
  return false;
}

// The first process below `root` whose sound this capture must not carry: an excluded program, or one of
// BuffrX's own players. A process-loopback source takes the whole tree below its root and Windows cannot
// subtract one descendant from it (audio_auto.h), so the root is refused rather than recording it.
const AutoProcessNode* forbidden_descendant(const std::map<uint32_t, AutoProcessNode>& nodes,
                                            const std::map<uint32_t, std::vector<uint32_t>>& children,
                                            const AutoAudioPrefs& prefs, uint32_t root, bool& own) {
  std::vector<uint32_t> stack = children.count(root) != 0 ? children.at(root) : std::vector<uint32_t>();
  std::set<uint32_t> seen;
  while (!stack.empty()) {
    const uint32_t pid = stack.back();
    stack.pop_back();
    if (!seen.insert(pid).second) continue;
    const auto found = nodes.find(pid);
    if (found == nodes.end()) continue;
    if (auto_own_executable(found->second.exe)) {
      own = true;
      return &found->second;
    }
    const auto pref = prefs.find(auto_pref_key(found->second.exe));
    if (pref != prefs.end() && pref->second.excluded) {
      own = false;
      return &found->second;
    }
    const auto below = children.find(pid);
    if (below != children.end()) stack.insert(stack.end(), below->second.begin(), below->second.end());
  }
  return nullptr;
}

}  // namespace

std::string auto_pref_key(const std::string& exe) { return lower_ascii(file_name_of(exe)); }

double auto_volume(double value) {
  if (!std::isfinite(value)) return 1.0;
  return std::clamp(value, 0.0, 1.0);
}

std::string auto_display_name(const std::string& exe) {
  std::string name = file_name_of(exe);
  const std::string lower = lower_ascii(name);
  if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
  if (name.empty()) name = "Application";
  return name;
}

bool auto_own_executable(const std::string& exe) { return own_name(exe); }

bool auto_own_process(const AutoProcessNode& node, const std::map<uint32_t, AutoProcessNode>& nodes) {
  uint32_t pid = node.pid;
  for (int depth = 0; depth < 64 && pid != 0; ++depth) {
    const auto found = nodes.find(pid);
    if (found == nodes.end()) break;
    if (own_name(found->second.exe)) return true;
    if (found->second.parent_pid == pid) break;
    const auto parent = nodes.find(found->second.parent_pid);
    if (parent == nodes.end() || !valid_parent(found->second, parent->second)) break;
    pid = found->second.parent_pid;
  }
  return own_name(node.exe);
}

bool auto_error_is_unsupported(const std::string& error) {
  const std::string lower = lower_ascii(error);
  return lower.find("not supported") != std::string::npos || lower.find("notimpl") != std::string::npos ||
         lower.find("0x8000000e") != std::string::npos || lower.find("0x80004002") != std::string::npos ||
         lower.find("unsupported") != std::string::npos;
}

std::string auto_bus_source(int track) { return "auto:bus:" + std::to_string(track); }

bool auto_bus_track(const std::string& source, int& track) {
  const std::string prefix = "auto:bus:";
  if (source.rfind(prefix, 0) != 0) return false;
  const std::string tail = source.substr(prefix.size());
  if (tail.empty() || tail.size() > 2) return false;
  for (char c : tail) {
    if (c < '0' || c > '9') return false;
  }
  const int value = std::stoi(tail);
  if (value < 3 || value > 6) return false;
  track = value;
  return true;
}

std::vector<AutoProcessNode> auto_process_snapshot() {
  std::vector<AutoProcessNode> out;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return out;
  PROCESSENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry)) {
    do {
      AutoProcessNode node;
      node.pid = entry.th32ProcessID;
      node.parent_pid = entry.th32ParentProcessID;
#ifdef _WIN32
      node.exe = narrow(entry.szExeFile);
#else
      node.exe = entry.szExeFile;  // the POSIX snapshot already reports the process name as narrow text
#endif
      // Creation time is the identity a reused process id cannot fake. It is read only for the snapshot
      // the grouping already takes, never on a timer of its own.
#ifdef _WIN32
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, node.pid);
      if (process) {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
          ULARGE_INTEGER ticks{};
          ticks.LowPart = creation.dwLowDateTime;
          ticks.HighPart = creation.dwHighDateTime;
          node.created_ms = ticks.QuadPart / 10000ULL;
        }
        CloseHandle(process);
      }
#endif
      out.push_back(std::move(node));
    } while (Process32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return out;
}

std::vector<std::string> auto_session_identities(const std::vector<AutoAudioSession>& sessions) {
  std::vector<std::string> out;
  out.reserve(sessions.size());
  for (const AutoAudioSession& session : sessions) {
    if (session.system_sounds) continue;
    out.push_back(session.key + "|" + std::to_string(session.pid) + "|" + auto_pref_key(session.exe));
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool auto_snapshot_due(const std::vector<AutoAudioSession>& sessions,
                       const std::vector<std::string>& snapshot_for, bool have_snapshot,
                       const std::vector<AutoProcessNode>& processes) {
  if (!have_snapshot) return true;
  if (auto_session_identities(sessions) != snapshot_for) return true;
  // A program that has just started can have a session before it is in the table. Its process is the one
  // thing this pass needs and does not have, so one more walk is worth it - and only while it is active.
  for (const AutoAudioSession& session : sessions) {
    if (!session.active || session.system_sounds || session.pid == 0) continue;
    bool found = false;
    for (const AutoProcessNode& node : processes) {
      if (node.pid == session.pid) {
        found = true;
        break;
      }
    }
    if (!found) return true;
  }
  return false;
}

uint64_t auto_tick64(uint32_t low_ms, uint64_t now_ms) {
  if (now_ms < 0x100000000ull) return low_ms;  // the 32-bit clock has not wrapped yet
  uint64_t value = (now_ms & ~0xFFFFFFFFull) | low_ms;
  if (value > now_ms) value -= 0x100000000ull;  // a value ahead of now belongs to the previous wrap
  return value;
}

AutoAudioPlan plan_auto_audio(const AutoAudioInputs& inputs, AutoLaneMemory& memory) {
  AutoAudioPlan plan;
  plan.on = true;

  std::map<uint32_t, AutoProcessNode> nodes;
  for (const AutoProcessNode& node : inputs.processes) nodes[node.pid] = node;
  std::map<uint32_t, std::vector<uint32_t>> children;
  for (const AutoProcessNode& node : inputs.processes) {
    const auto parent = nodes.find(node.parent_pid);
    if (node.parent_pid != 0 && node.parent_pid != node.pid && parent != nodes.end() &&
        valid_parent(node, parent->second)) children[node.parent_pid].push_back(node.pid);
  }

  // One group per executable. Two sessions of one program are one row and one bus, because a second
  // capture of the same tree would record the same PCM twice. Two disjoint process trees of one program
  // (two browser profiles) are both kept, as two captures under that one row.
  std::map<std::string, Group> grouped;
  for (const AutoAudioSession& session : inputs.sessions) {
    if (session.system_sounds || session.pid == 0) continue;
    if (!watched_session_visible(session.active, session.changed_ms, inputs.now_ms, inputs.grace_ms)) continue;
    std::string exe = session.exe;
    const auto own = nodes.find(session.pid);
    if (exe.empty() && own != nodes.end()) exe = own->second.exe;
    const std::string key = auto_pref_key(exe);
    if (key.empty()) continue;
    AutoProcessNode node = own != nodes.end() ? own->second : AutoProcessNode{session.pid, 0, exe, 0};
    if (auto_own_process(node, nodes)) continue;

    // The root is the topmost ancestor with the same executable. Climbing into a different program is
    // what would combine a launcher, Explorer or another app's sound into one lane.
    uint32_t root = session.pid;
    for (int depth = 0; depth < 64; ++depth) {
      const auto current = nodes.find(root);
      if (current == nodes.end()) break;
      const auto parent = nodes.find(current->second.parent_pid);
      if (parent == nodes.end() || parent->second.pid == root) break;
      if (!valid_parent(current->second, parent->second)) break;
      if (auto_pref_key(parent->second.exe) != key) break;
      if (auto_own_process(parent->second, nodes)) break;
      root = parent->second.pid;
    }

    const auto pref_it = inputs.prefs.find(key);
    if (pref_it != inputs.prefs.end() && pref_it->second.excluded) continue;

    Group& group = grouped[key];
    if (group.key.empty()) {
      group.key = key;
      group.exe = exe;
      group.name = auto_display_name(exe);
      group.changed_ms = session.changed_ms;
    }
    group.active = group.active || session.active;
    group.changed_ms = std::min(group.changed_ms, session.changed_ms);
    AutoAudioRoot one;
    one.pid = root;
    const auto root_node = nodes.find(root);
    one.created_ms = root_node != nodes.end() ? root_node->second.created_ms : 0;
    if (std::find(group.roots.begin(), group.roots.end(), one) == group.roots.end()) group.roots.push_back(one);
    group.pref = pref_it != inputs.prefs.end() ? pref_it->second : AutoAudioPref{};
  }

  // One root per tree. A root already inside another root of the same program is the same PCM, and the
  // outer capture already takes it.
  for (auto& [key, group] : grouped) {
    (void)key;
    std::sort(group.roots.begin(), group.roots.end(),
              [](const AutoAudioRoot& left, const AutoAudioRoot& right) { return left.pid < right.pid; });
    std::vector<AutoAudioRoot> kept;
    for (size_t index = 0; index < group.roots.size(); ++index) {
      bool covered = false;
      for (size_t other = 0; other < group.roots.size() && !covered; ++other) {
        if (other != index) covered = pid_in_tree(nodes, group.roots[other].pid, group.roots[index].pid);
      }
      if (!covered) kept.push_back(group.roots[index]);
    }
    group.roots = std::move(kept);
  }

  std::vector<Group> groups;
  groups.reserve(grouped.size());
  for (auto& [key, group] : grouped) {
    (void)key;
    if (!group.roots.empty()) groups.push_back(std::move(group));
  }
  std::stable_sort(groups.begin(), groups.end(), [](const Group& left, const Group& right) {
    if (left.changed_ms != right.changed_ms) return left.changed_ms < right.changed_ms;
    return left.key < right.key;
  });

  // A tree known to hold an excluded program, or one of BuffrX's own players, is not captured: Windows'
  // process loopback cannot subtract one descendant. A newly spawned child can be heard until the next
  // snapshot discovers it; the API cannot enforce this decision atomically.
  std::set<std::string> blocked;
  for (Group& group : groups) {
    std::vector<AutoAudioRoot> kept;
    for (const AutoAudioRoot& root : group.roots) {
      bool own = false;
      const AutoProcessNode* leak = forbidden_descendant(nodes, children, inputs.prefs, root.pid, own);
      if (!leak) {
        kept.push_back(root);
        continue;
      }
      if (blocked.insert(group.key).second) {
        AutoAudioUncaptured entry;
        entry.exe = group.exe;
        entry.name = group.name;
        entry.state = "blocked";
        entry.error = own ? "Not captured: BuffrX's own playback runs inside its process tree"
                          : "Not captured: " + auto_display_name(leak->exe) +
                                " is excluded and runs inside its process tree";
        plan.uncaptured.push_back(std::move(entry));
      }
    }
    group.roots = std::move(kept);
  }
  groups.erase(std::remove_if(groups.begin(), groups.end(),
                              [](const Group& group) { return group.roots.empty(); }),
               groups.end());

  // Two process captures must never carry the same PCM. A process-loopback source takes the whole tree
  // below its root, so a root inside another group's tree is already recorded by that group. Only the
  // covered root leaves; a program with another tree of its own keeps that tree and its sound.
  for (Group& group : groups) {
    std::vector<AutoAudioRoot> kept;
    for (const AutoAudioRoot& root : group.roots) {
      bool covered = false;
      for (const Group& other : groups) {
        if (&other == &group) continue;
        for (const AutoAudioRoot& outer : other.roots) {
          if (outer.pid != root.pid && pid_in_tree(nodes, outer.pid, root.pid)) {
            covered = true;
            break;
          }
        }
        if (covered) break;
      }
      if (!covered) kept.push_back(root);
    }
    group.roots = std::move(kept);
  }
  groups.erase(std::remove_if(groups.begin(), groups.end(),
                              [](const Group& group) { return group.roots.empty(); }),
               groups.end());


  // Forgetting a program that is gone frees its lane for the next arrival. A program still present keeps
  // its lane; nothing already captured is moved because another program started.
  std::set<std::string> present;
  for (const Group& group : groups) present.insert(group.key);
  for (auto it = memory.lane_of.begin(); it != memory.lane_of.end();) {
    if (present.count(it->first) == 0) it = memory.lane_of.erase(it);
    else ++it;
  }

  std::vector<int> free_lanes{3, 4, 5};
  std::map<std::string, int> assigned;
  for (const Group& group : groups) {
    const auto kept = memory.lane_of.find(group.key);
    if (kept == memory.lane_of.end()) continue;
    const auto lane = std::find(free_lanes.begin(), free_lanes.end(), kept->second);
    if (lane == free_lanes.end()) continue;
    assigned[group.key] = *lane;
    free_lanes.erase(lane);
  }
  const auto take_lane = [&](const Group& group) {
    if (assigned.count(group.key) != 0 || free_lanes.empty()) return;
    assigned[group.key] = free_lanes.front();
    free_lanes.erase(free_lanes.begin());
  };
  if (!inputs.game_exe.empty()) {
    const std::string game = auto_pref_key(inputs.game_exe);
    for (const Group& group : groups) {
      if (group.key == game) {
        take_lane(group);
        break;
      }
    }
  }
  for (const Group& group : groups) take_lane(group);

  memory.lane_of.clear();
  std::map<int, const Group*> lanes;
  std::vector<const Group*> other;
  for (const Group& group : groups) {
    const auto lane = assigned.find(group.key);
    if (lane == assigned.end()) {
      other.push_back(&group);
      continue;
    }
    memory.lane_of[group.key] = lane->second;
    lanes[lane->second] = &group;
  }

  for (const auto& [track, group] : lanes) {
    AutoAudioLane lane;
    lane.track = track;
    lane.exe = group->exe;
    lane.name = group->name;
    lane.state = state_of(group->active);
    lane.pid = group->roots.front().pid;
    lane.roots = group->roots;
    lane.volume = auto_volume(group->pref.volume);
    lane.muted = group->pref.muted;
    lane.other = false;
    AutoAudioApp app;
    app.exe = group->exe;
    app.name = group->name;
    app.state = lane.state;
    app.pid = lane.pid;
    app.volume = lane.volume;
    app.muted = lane.muted;
    app.roots = group->roots;
    lane.apps.push_back(std::move(app));
    plan.lanes.push_back(std::move(lane));
  }

  if (!other.empty()) {
    AutoAudioLane lane;
    lane.track = 6;
    lane.name = "Other audio";
    lane.other = true;
    lane.volume = auto_volume(inputs.other_volume);
    lane.muted = inputs.other_muted;
    bool any_active = false;
    bool any_waiting = false;
    for (const Group* group : other) {
      AutoAudioApp app;
      app.exe = group->exe;
      app.name = group->name;
      app.state = state_of(group->active);
      app.pid = group->roots.front().pid;
      app.volume = auto_volume(group->pref.volume * auto_volume(inputs.other_volume));
      app.muted = group->pref.muted || inputs.other_muted;
      app.roots = group->roots;
      any_active = any_active || group->active;
      any_waiting = any_waiting || !group->active;
      lane.apps.push_back(std::move(app));
    }
    lane.state = any_active ? "connected" : "waiting";
    (void)any_waiting;
    plan.lanes.push_back(std::move(lane));
  }

  // An excluded program is deliberately absent, not uncaptured. Everything supported is on a bus: the
  // isolated lanes or Other audio, so the finite number of encoders never drops a program's sound.
  return plan;
}

void AutoJournal::observe(const AutoAudioPlan& plan, uint64_t now_ms) {
  std::map<int, std::string> next;
  for (const AutoAudioLane& lane : plan.lanes) {
    if (lane.track < 3 || lane.track > 6) continue;
    std::string name = lane.name;
    if (lane.other && !lane.apps.empty()) {
      // Other audio's title stays one name; its members are reported by the status, not the sidecar.
      name = "Other audio";
    }
    if (!name.empty()) next[lane.track] = name;
  }
  for (auto it = current_.begin(); it != current_.end();) {
    if (next.count(it->first) != 0) {
      ++it;
      continue;
    }
    for (Entry& entry : entries_) {
      if (entry.track == it->first && entry.end_ms == 0) entry.end_ms = now_ms;
    }
    it = current_.erase(it);
  }
  for (const auto& [track, name] : next) {
    const auto current = current_.find(track);
    if (current != current_.end() && current->second == name) continue;
    if (current != current_.end()) {
      for (Entry& entry : entries_) {
        if (entry.track == track && entry.end_ms == 0) entry.end_ms = now_ms;
      }
    }
    Entry entry;
    entry.track = track;
    entry.name = name;
    entry.start_ms = now_ms;
    entries_.push_back(std::move(entry));
    if (output_) {
      std::vector<std::string>& names = summary_[track];
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    current_[track] = name;
  }
}

void AutoJournal::start_output(uint64_t now_ms) {
  output_ = true;
  summary_.clear();
  for (Entry& entry : entries_) {
    if (entry.end_ms == 0) entry.end_ms = now_ms;
  }
  current_.clear();
}

void AutoJournal::end_output(uint64_t now_ms) {
  for (Entry& entry : entries_) {
    if (entry.end_ms == 0) entry.end_ms = now_ms;
  }
  current_.clear();
  output_ = false;
}

std::vector<std::string> AutoJournal::names_in_span(int track, uint64_t start_ms, uint64_t end_ms) const {
  if (end_ms < start_ms) std::swap(start_ms, end_ms);
  std::vector<std::string> names;
  for (const Entry& entry : entries_) {
    if (entry.track != track) continue;
    const uint64_t entry_end = entry.end_ms == 0 ? UINT64_MAX : entry.end_ms;
    if (entry.start_ms > end_ms || entry_end < start_ms) continue;
    if (std::find(names.begin(), names.end(), entry.name) == names.end()) names.push_back(entry.name);
  }
  return names;
}

std::vector<std::string> AutoJournal::output_names(int track) const {
  const auto found = summary_.find(track);
  return found == summary_.end() ? std::vector<std::string>() : found->second;
}

void AutoJournal::prune(uint64_t now_ms, uint64_t keep_ms) {
  const uint64_t floor = now_ms > keep_ms ? now_ms - keep_ms : 0;
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [floor](const Entry& entry) {
                                  return entry.end_ms != 0 && entry.end_ms < floor;
                                }),
                 entries_.end());
}

void AutoJournal::clear() {
  entries_.clear();
  current_.clear();
  summary_.clear();
  output_ = false;
}

}  // namespace bx
