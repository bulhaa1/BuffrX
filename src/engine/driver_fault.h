// What the engine does when the graphics driver resets or libobs gives up, decided without libobs so every rule is tested.
//
// The case this exists for: an AMD user on av1_texture_amf, whose driver reset (TDR) in the middle of a game. libobs-d3d11
// rebuilds its device after a reset and, when the rebuild fails, calls bcrash(), whose default handler is exit(0): the
// engine vanished with exit code 0 and nothing in the log. When it did survive, the encoder watch read the dead encoder
// (0% of the frames) as a slow one, stepped it down and rebuilt it on the engine lock while the driver was still
// resetting, and the engine hung. None of that is a slow encoder. These are the rules that keep a dead one apart.

#ifndef BUFFRX_DRIVER_FAULT_H
#define BUFFRX_DRIVER_FAULT_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "capacity_memory.h"

namespace buffrx {

// --- libobs's log lines ---------------------------------------------------------------------------------------
//
// libobs's blog() is called from every thread it owns, and a failing driver can make one call site fire per frame.
// At most this many lines per format string per window reach engine.log; the rest are counted and said once.
constexpr int kLogLinesPerWindow = 5;
constexpr uint32_t kLogWindowMs = 60000;

class LogLimiter {
public:
  // True when the line may be written. `suppressed` is set (else 0) to the number of lines of this format that the
  // window which just ended swallowed: the caller writes that summary before the line. Keyed by the format string's
  // address (libobs's are literals), so one noisy call site cannot drown the rest. Not thread-safe: the caller locks.
  bool admit(const void* key, uint32_t now_ms, int& suppressed) {
    suppressed = 0;
    Slot* slot = nullptr;
    Slot* spare = nullptr;  // an empty slot, else the one whose window began longest ago
    for (Slot& candidate : slots_) {
      if (candidate.key == key) { slot = &candidate; break; }
      if (!spare || (spare->key != nullptr && (candidate.key == nullptr || now_ms - candidate.start_ms > now_ms - spare->start_ms))) spare = &candidate;
    }
    if (!slot) {
      slot = spare;  // an evicted slot loses its tally: 32 call sites at once is not a case worth a map
      *slot = Slot{key, now_ms, 0, 0};
    }
    if (now_ms - slot->start_ms >= kLogWindowMs) {
      suppressed = slot->dropped;
      *slot = Slot{key, now_ms, 0, 0};
    }
    if (slot->count < kLogLinesPerWindow) {
      ++slot->count;
      return true;
    }
    ++slot->dropped;
    return false;
  }

private:
  struct Slot {
    const void* key = nullptr;
    uint32_t start_ms = 0;
    int count = 0;
    int dropped = 0;
  };
  Slot slots_[32];
};

// libobs-d3d11 writes this when the driver took the device away (d3d11-rebuild.cpp, blog(LOG_WARNING, "Device
// Remove/Reset!  Rebuilding all assets...")). Matches the text of OBS 33.0.1: recheck it whenever the OBS pin moves.
inline bool is_device_lost_line(const char* message) {
  return message && std::strncmp(message, "Device Remove/Reset!", 20) == 0;
}

// --- When the engine leaves things alone ----------------------------------------------------------------------
//
// After the device was lost, a stall restart, a game-capture stall stop or an output that stopped on an encode error,
// the encoder counters are not telling the truth about the encoder, and rebuilding it is the worst thing to do.
constexpr uint32_t kGovernorHoldMs = 60000;
// The stall watchdog gives the driver this long to come back before it restarts an output on top of it.
constexpr uint32_t kDeviceLostHoldMs = 10000;

inline bool within_ms(uint32_t now_ms, uint32_t at_ms, uint32_t span_ms) { return at_ms != 0 && now_ms - at_ms < span_ms; }

inline bool governor_held(uint32_t now_ms, uint32_t last_fault_ms) { return within_ms(now_ms, last_fault_ms, kGovernorHoldMs); }
inline bool stall_restart_held(uint32_t now_ms, uint32_t device_lost_ms) {
  return within_ms(now_ms, device_lost_ms, kDeviceLostHoldMs);
}

// --- When video will not start ---------------------------------------------------------------------------------
//
// A driver that stays broken made every automatic start retry ten obs_reset_video calls, twice a second, forever.
// After a failure nothing automatic tries again until the deadline; a press the person made may try once at once.
struct VideoInitBackoff {
  int failures = 0;
  uint32_t next_ms = 0;  // 0: no deadline
};

constexpr uint32_t kVideoBackoffMs[] = {2000, 4000, 8000, 16000, 32000, 60000};

inline bool video_init_allowed(const VideoInitBackoff& backoff, uint32_t now_ms) {
  return backoff.failures == 0 || backoff.next_ms == 0 || static_cast<int32_t>(now_ms - backoff.next_ms) >= 0;
}
inline uint32_t video_init_wait_ms(const VideoInitBackoff& backoff, uint32_t now_ms) {
  return video_init_allowed(backoff, now_ms) ? 0 : backoff.next_ms - now_ms;
}
inline void video_init_failed(VideoInitBackoff& backoff, uint32_t now_ms) {
  const int last = static_cast<int>(sizeof(kVideoBackoffMs) / sizeof(kVideoBackoffMs[0])) - 1;
  const uint32_t delay = kVideoBackoffMs[backoff.failures < last ? backoff.failures : last];
  if (backoff.failures < 1000) ++backoff.failures;
  backoff.next_ms = now_ms + delay;
  if (backoff.next_ms == 0) backoff.next_ms = 1;
}
inline void video_init_ok(VideoInitBackoff& backoff) { backoff = VideoInitBackoff{}; }
// A deliberate press: one try now. If it fails the deadline is set again from the failure count.
inline void video_init_allow_now(VideoInitBackoff& backoff) { backoff.next_ms = 0; }

// --- After libobs gave up (bcrash) -----------------------------------------------------------------------------
//
// bcrash cannot be survived: the process ends. What can be decided is whether BuffrX comes back by itself. It does when
// something was being captured (the person expects it to still be), and never twice in ten minutes: a driver that
// crashes the engine again right away needs the person, not a loop.
constexpr int64_t kCrashRelaunchWindowS = 600;
constexpr int64_t kCrashHistoryKeepS = 3600;
constexpr size_t kCrashHistoryMax = 8;

struct CrashEntry {
  int64_t at_s = 0;
  bool relaunched = false;
};

struct CrashDecision {
  bool relaunch = false;
  bool repeated = false;  // a relaunch already happened in the last ten minutes
};

inline CrashDecision crash_decide(const std::vector<CrashEntry>& history, bool capture_wanted, int64_t now_s) {
  CrashDecision decision;
  for (const CrashEntry& entry : history) {
    if (entry.relaunched && now_s >= entry.at_s && now_s - entry.at_s < kCrashRelaunchWindowS) decision.repeated = true;
  }
  decision.relaunch = capture_wanted && !decision.repeated;
  return decision;
}

// What the next start says about the end: "restarted" (this is the relaunch), "repeated" (it gave up after a second
// crash, said at the next manual start) or "closed" (it ended and nothing was being captured, so nothing came back).
inline const char* crash_card_for(const CrashDecision& decision) {
  if (decision.relaunch) return "restarted";
  return decision.repeated ? "repeated" : "closed";
}

struct CrashMarker {
  int64_t time_s = 0;
  std::string message;
  bool capture_wanted = false;
  std::string card;  // empty once the card was shown
  std::vector<CrashEntry> history;
};

// The marker as it stands after one more crash: older entries past the keep window drop, the list is bounded.
inline CrashMarker crash_marker_after(const CrashMarker& before, const std::string& message, bool capture_wanted,
                                      int64_t now_s, const CrashDecision& decision) {
  CrashMarker marker;
  marker.time_s = now_s;
  marker.message = message.substr(0, 400);
  marker.capture_wanted = capture_wanted;
  marker.card = crash_card_for(decision);
  for (const CrashEntry& entry : before.history) {
    if (now_s >= entry.at_s && now_s - entry.at_s < kCrashHistoryKeepS) marker.history.push_back(entry);
  }
  marker.history.push_back({now_s, decision.relaunch});
  while (marker.history.size() > kCrashHistoryMax) marker.history.erase(marker.history.begin());
  return marker;
}

inline bx::Json crash_marker_to_json(const CrashMarker& marker) {
  bx::Json json = bx::Json::object();
  json.set("time", bx::Json(marker.time_s));
  json.set("message", bx::Json(marker.message));
  json.set("capture_wanted", bx::Json(marker.capture_wanted));
  json.set("card", bx::Json(marker.card));
  bx::Json history = bx::Json::array();
  for (const CrashEntry& entry : marker.history) {
    bx::Json item = bx::Json::object();
    item.set("at", bx::Json(entry.at_s));
    item.set("relaunched", bx::Json(entry.relaunched));
    history.push(item);
  }
  json.set("history", history);
  return json;
}

inline CrashMarker crash_marker_from_json(const bx::Json& json) {
  CrashMarker marker;
  if (!json.is_object()) return marker;
  marker.time_s = json["time"].as_int(0);
  marker.message = json["message"].as_string();
  marker.capture_wanted = json["capture_wanted"].as_bool(false);
  marker.card = json["card"].as_string();
  if (json["history"].is_array()) {
    for (const bx::Json& item : json["history"].items()) marker.history.push_back({item["at"].as_int(0), item["relaunched"].as_bool(false)});
  }
  return marker;
}

inline bool crash_marker_read(const std::string& path, CrashMarker& marker) {
  bx::Json json;
  if (!bx::capacity_parse_file(path, json)) return false;
  marker = crash_marker_from_json(json);
  return true;
}

// The card the next start draws. Wording is here so a test holds it: the person's first question is whether the clips
// survived, and a driver reset cost them nothing that was on disk.
struct CrashCard {
  std::string title;
  std::string detail;
};

// libobs-d3d11's bcrash after a reset it could not rebuild from starts with this (d3d11-rebuild.cpp, OBS 33.0.1). libobs also
// calls bcrash for an allocation or a thread it could not make: those are not a driver reset and are not called one.
inline bool crash_was_driver_reset(const std::string& message) { return message.rfind("Failed to recreate D3D11", 0) == 0; }

inline CrashCard crash_card_text(const std::string& card, const std::string& message) {
  const bool driver = crash_was_driver_reset(message);
  if (card == "restarted") {
    return {driver ? "BuffrX restarted after a graphics driver reset" : "BuffrX restarted after an internal error",
            "Your clips are safe. The buffer is running again."};
  }
  if (card == "repeated") {
    if (!driver) return {"BuffrX stopped after repeated errors", "Your clips are safe. Start BuffrX again when you are ready."};
    return {"BuffrX stopped after repeated graphics driver resets",
            "Your clips are safe. Try H.264 on the Video page and Window capture for the game, then start BuffrX again."};
  }
  return {driver ? "BuffrX closed after a graphics driver reset" : "BuffrX closed after an internal error", "Your clips are safe."};
}

}  // namespace buffrx

#endif  // BUFFRX_DRIVER_FAULT_H
