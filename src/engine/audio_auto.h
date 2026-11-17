// Automatic app-audio routing: which programs get the isolated lanes, what falls into Other audio, and
// what a recorded lane is called after the fact.
//
// Automatic mode is the normal mode. Windows' own audio-session events say which programs are playing
// sound (audio_watch.h); this file turns that list into a stable six-track plan: track 1 the mix, track 2
// the microphone, tracks 3..5 up to three individually captured programs, track 6 every remaining
// program together ("Other audio"). Nothing here samples a meter or enumerates a device: it is a policy
// over a snapshot and a small memory of where each program was last placed.
//
// The rules that make it safe:
//  - A program's sound is captured once. Sessions of one executable are one group, so two browser
//    profiles never record the same program twice.
//  - A group is rooted at the topmost ancestor with the same executable. It never climbs into Explorer,
//    Steam or another program, because ascending to a different executable is how unrelated apps would
//    be combined. A group can hold several disjoint roots (two browser profiles, one exe): each root is
//    its own process tree, the group stays one row, one preference and one bus, and a tree already
//    inside another root is not captured a second time.
//  - BuffrX's own players (the engine, the editor, the sharing helper, the muxer, and the WebView2
//    children below the editor) are never captured, so a clip never records BuffrX playing a clip.
//  - A process-loopback capture takes the whole tree below its root, and Windows cannot subtract one
//    descendant from it. A root known to hold an excluded program or BuffrX player is blocked. A new
//    descendant can be captured until the next session event or periodic process snapshot detects it;
//    Windows process loopback cannot enforce descendant exclusions atomically.
//  - A lane assignment is sticky. A program arriving never moves one that is already captured; a freed
//    lane may be taken by the captured game first.
//  - An excluded program stays excluded even when it has no session; there is no desktop-loopback
//    fallback. If process audio is unsupported, the isolation is off and the status says why.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bx {

// A program's saved controls, keyed by the lower-case executable name. Volume is 0..1; muted silences
// the capture; excluded keeps the program out of automatic capture entirely.
struct AutoAudioPref {
  double volume = 1.0;
  bool muted = false;
  bool excluded = false;
  bool operator==(const AutoAudioPref& other) const {
    return volume == other.volume && muted == other.muted && excluded == other.excluded;
  }
  bool operator!=(const AutoAudioPref& other) const { return !(*this == other); }
};
using AutoAudioPrefs = std::map<std::string, AutoAudioPref>;

// The lower-case executable name a preference is keyed by ("Discord.exe" -> "discord.exe").
std::string auto_pref_key(const std::string& exe);
// A gain held finite and inside 0..1, so a hand-edited file cannot hand libobs a NaN.
double auto_volume(double value);
// "Discord.exe" -> "Discord": what a lane row is called.
std::string auto_display_name(const std::string& exe);

// One session as the watcher reports it (audio_watch.h WatchedSession, kept separate so this policy has
// no COM dependency).
struct AutoAudioSession {
  std::string key;
  uint32_t pid = 0;
  std::string exe;
  bool active = false;
  bool system_sounds = false;
  uint64_t changed_ms = 0;
};

// One process in the snapshot the grouping reads. `parent_pid` and `created_ms` identify the tree; a
// reused process id cannot pass itself off as an ancestor that was already gone.
struct AutoProcessNode {
  uint32_t pid = 0;
  uint32_t parent_pid = 0;
  std::string exe;
  uint64_t created_ms = 0;
};

// One process tree a group captures. `created_ms` is the identity of the process behind `pid`: a reused
// id with a new creation time is a different program, and the host replaces that capture rather than
// keeping a failed one.
struct AutoAudioRoot {
  uint32_t pid = 0;
  uint64_t created_ms = 0;
  bool operator==(const AutoAudioRoot& other) const {
    return pid == other.pid && created_ms == other.created_ms;
  }
  bool operator!=(const AutoAudioRoot& other) const { return !(*this == other); }
};

// Keep a live process capture if the identity still matches; an unavailable creation time is not evidence
// of PID reuse. Membership changes to other roots do not affect this decision.
inline bool auto_root_retain(const AutoAudioRoot& existing, const AutoAudioRoot& wanted) {
  return existing.pid == wanted.pid &&
         (existing.created_ms == 0 || wanted.created_ms == 0 || existing.created_ms == wanted.created_ms);
}

// A programme inside an Other audio lane, or an isolated lane's own app.
struct AutoAudioApp {
  std::string exe;
  std::string name;
  std::string state;  // connected | waiting | failed
  uint32_t pid = 0;   // the first of the program's process trees, at this snapshot
  double volume = 1.0;
  bool muted = false;
  std::vector<AutoAudioRoot> roots;  // every disjoint process tree of this executable
};

struct AutoAudioLane {
  int track = 0;          // 3..6
  std::string exe;        // the isolated program, empty for Other
  std::string name;       // "Discord", "Other audio"
  std::string state;      // connected | waiting | failed
  uint32_t pid = 0;
  double volume = 1.0;
  bool muted = false;
  bool other = false;     // track 6: every remaining program, mixed
  std::vector<AutoAudioRoot> roots;  // the process trees the lane's bus captures
  std::vector<AutoAudioApp> apps;
};

struct AutoAudioUncaptured {
  std::string exe;
  std::string name;
  std::string state;  // failed | unsupported | blocked
  std::string error;
};

struct AutoAudioPlan {
  bool on = false;       // automatic mode wants capture
  bool watching = false; // the session watcher is running (filled by the caller)
  bool unsupported = false;
  std::string error;
  std::vector<AutoAudioLane> lanes;            // only lanes that carry something now
  std::vector<AutoAudioUncaptured> uncaptured;
};

// The lane each program was given, kept across snapshots so arrivals do not reshuffle the mixer.
struct AutoLaneMemory {
  std::map<std::string, int> lane_of;  // lower-case exe -> track 3..5
};

struct AutoAudioInputs {
  std::vector<AutoAudioSession> sessions;
  std::vector<AutoProcessNode> processes;
  AutoAudioPrefs prefs;
  uint64_t now_ms = 0;
  uint32_t grace_ms = 1500;      // an inactive session stays visible this long
  std::string game_exe;          // the captured game, preferred for a freed isolated lane
  // The group controls: applied to every Other audio member in addition to its own saved gain.
  double other_volume = 1.0;
  bool other_muted = false;
};

// The plan a snapshot produces. Pure: no COM, no libobs, no device. `memory` is updated in place with
// the assignments this plan made.
AutoAudioPlan plan_auto_audio(const AutoAudioInputs& inputs, AutoLaneMemory& memory);

// The processes a grouping needs: every session's process plus its ancestors, from one Toolhelp
// snapshot. Only the executable name and parent are kept, which is all the policy reads.
std::vector<AutoProcessNode> auto_process_snapshot();
// The identity of the session set a process snapshot was taken for: one line per non-system session, with
// its key, pid and executable. A state change (playing / paused) does not change it; a program appearing,
// leaving or relaunching does. It is what decides whether another full process walk is worth its cost.
std::vector<std::string> auto_session_identities(const std::vector<AutoAudioSession>& sessions);
// Whether grouping needs a new process snapshot: the first time, when the session identities changed, or
// while an active session's process is not in the snapshot yet (a program that has just started). Grace
// expiry and state changes are answered from the snapshot already held.
bool auto_snapshot_due(const std::vector<AutoAudioSession>& sessions,
                       const std::vector<std::string>& snapshot_for, bool have_snapshot,
                       const std::vector<AutoProcessNode>& processes);
// Also refresh a stable tree periodically: a child can arrive without creating a new audio session.
inline bool auto_tree_refresh_due(uint64_t now_ms, uint64_t last_ms) {
  return now_ms < last_ms || now_ms - last_ms >= 5000;
}
// Places a Windows 32-bit tick (a session index's or a recording's own) on the 64-bit clock the journal
// uses. Every such span is minutes old at most, far inside one 32-bit wrap (49.7 days).
uint64_t auto_tick64(uint32_t low_ms, uint64_t now_ms);
// Whether an executable is one of BuffrX's own players and must never be captured.
bool auto_own_executable(const std::string& exe);
// Whether a process is below one of BuffrX's own players: the WebView2 children of the editor are the
// one case where the executable alone does not name the owner.
bool auto_own_process(const AutoProcessNode& node, const std::map<uint32_t, AutoProcessNode>& nodes);

// Whether a capture failure means "this Windows cannot do process audio" rather than "this program
// failed once": the status uses it to say unsupported instead of naming one app.
bool auto_error_is_unsupported(const std::string& error);

// The stable source id of an automatic lane. A recorded file's sidecar names these, not a program that
// happened to be on the lane at one moment: the lane is what was recorded, and its title summarises who
// occupied it. `auto:bus:3` .. `auto:bus:6`.
std::string auto_bus_source(int track);
bool auto_bus_track(const std::string& source, int& track);

// The occupants of each lane over time. A lane can hold A, then B within one recording; the sidecar must
// name both ("A + B") and the mix must still decompose into the same bus ids, so the editor can level the
// lane without losing its honesty. The journal is bounded: ring saves read a window, a manual recording
// keeps its own complete summary until it stops, and old entries are pruned.
class AutoJournal {
 public:
  struct Entry {
    int track = 0;
    std::string name;
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;  // 0 while the occupant is still there
  };

  // Applies one plan at `now_ms`: an occupant that left is closed, a new one starts.
  void observe(const AutoAudioPlan& plan, uint64_t now_ms);
  // A recording's own lifetime: the summary it keeps is reset and every current occupant starts now.
  void start_output(uint64_t now_ms);
  void end_output(uint64_t now_ms);
  // The names that were on `track` at any moment inside [start_ms, end_ms], oldest first and without
  // duplicates.
  std::vector<std::string> names_in_span(int track, uint64_t start_ms, uint64_t end_ms) const;
  // Every name this output has carried on `track` since start_output, even when the rolling journal has
  // already pruned it: a long manual recording keeps the names it actually recorded.
  std::vector<std::string> output_names(int track) const;
  // Forgets entries that ended before `now_ms - keep_ms`. An open occupant is never pruned.
  void prune(uint64_t now_ms, uint64_t keep_ms);
  void clear();

 private:
  std::vector<Entry> entries_;
  std::map<int, std::string> current_;                  // track -> current occupant name
  std::map<int, std::vector<std::string>> summary_;     // track -> names seen while an output runs
  bool output_ = false;
};

}  // namespace bx
