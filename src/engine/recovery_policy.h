// What survives when the capture goes wrong, and what the first start afterwards says about it. Pure, so every rule
// is tested.
//
// The RAM ring is the buffer, and it is gone with the process. Rewind's answer was to dump the entire ring to disk
// every 0.9 buffer lengths: gigabytes at a long buffer, and a byte written at the clip's own rate for as long as the
// buffer runs, whether anything is wrong or not. BuffrX writes nothing while the capture works and dumps the ring
// once, through the ordinary clip save, at the moment a fault is detected - a stall, an output that died, a failed
// rebuild. That costs no disk in normal use and still answers the case the ring was lost on: the capture broke, so
// save what it held before restarting it (which would empty it). A fault nobody saw coming - a Task Manager kill, a
// power cut - leaves nothing, because nothing was on disk. That is the trade, and it is deliberate: a person who
// buffers all day pays no continuous write for a feature they hope never to use.
//
// This header is the policy only: when the salvage runs, what the first start says, and when a quit needs a
// confirmation. ObsHost owns the save and the ring.

#ifndef BUFFRX_RECOVERY_POLICY_H
#define BUFFRX_RECOVERY_POLICY_H

#include <cstdint>
#include <string>

namespace buffrx {

// One salvage per fault episode. A stalled capture that recovers, stalls again a second later, and recovers again
// would otherwise fill the library with copies of the same minute.
constexpr uint32_t kSalvageCooldownMs = 120000;

// The ring has to hold something worth a file. A buffer that has run for a few ticks is not footage.
constexpr uint32_t kSalvageMinBufferMs = 5000;

// Whether the ring is dumped now.
//   enabled   the person's switch (config.crash_recovery)
//   faulted   something went wrong with the capture: the caller only asks when it did
//   buffer_ms the footage the ring holds, and the time the buffer has been running
//   since_last_ms  time since the last salvage, kSalvageCooldownMs or more to allow one
inline bool salvage_wanted(bool enabled, bool faulted, uint32_t buffer_ms, uint32_t since_last_ms) {
  if (!enabled || !faulted) return false;
  if (buffer_ms < kSalvageMinBufferMs) return false;
  return since_last_ms >= kSalvageCooldownMs;
}

// --- The notice on the first start after an end nobody executed -----------------------------------------------
//
// The question a person actually has is "did I lose my clips", not "how many seconds did you keep", so the card
// answers the first one. A clean exit says nothing at all: it deleted the marker this start found.
struct RecoveryFacts {
  int clips_ok = 0;        // clips the integrity check read through without complaint
  int screenshots_ok = 0;
  int damaged = 0;         // clips or screenshots that would not read
};

inline bool recovery_notice_wanted(const RecoveryFacts& facts) {
  // Only a damaged file is worth a card: a kill or a power cut leaves the marker behind, and the library being intact is
  // the normal case, not news. It used to say "Welcome back" on every start after a Task Manager kill.
  return facts.damaged > 0;
}

inline std::string plural(int count, const char* one, const char* many) {
  return std::to_string(count) + " " + (count == 1 ? one : many);
}

// "Your 3 clips and 2 screenshots are safe. BuffrX closed unexpectedly."
// A damaged file is named in the same breath: the point of the card is that it never says "safe" while something
// is not.
inline std::string recovery_notice_text(const RecoveryFacts& facts) {
  std::string safe;
  if (facts.clips_ok > 0) safe = "Your " + plural(facts.clips_ok, "clip", "clips");
  if (facts.screenshots_ok > 0) {
    safe += safe.empty() ? "Your " : " and ";
    safe += plural(facts.screenshots_ok, "screenshot", "screenshots");
  }
  std::string text;
  const bool one = facts.clips_ok + facts.screenshots_ok == 1;
  if (!safe.empty()) {
    text = safe + (one ? " is safe" : " are safe");
    text += facts.damaged > 0 ? ", but" : ".";
  }
  if (facts.damaged > 0) {
    if (!text.empty()) text += " ";
    text += plural(facts.damaged, "file looks damaged", "files look damaged") + ".";
  }
  if (!text.empty()) text += " ";
  return text + "BuffrX closed unexpectedly.";
}

// --- Quitting while something is being written ---------------------------------------------------------------
//
// Closing the panel is hiding it to the tray, so that is never a question. Quit is: a manual recording or a Timeline
// session is being written, and quitting ends it. Task Manager's kill cannot be intercepted; the fragmented
// containers make that survivable rather than preventable.
inline bool quit_needs_confirm(bool manual_recording, bool timeline_writer) { return manual_recording || timeline_writer; }

}  // namespace buffrx

#endif  // BUFFRX_RECOVERY_POLICY_H
