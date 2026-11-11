// What the engine does when the machine's memory is nearly full, and what it says. Pure, so the threshold and the words are tested without
// a full machine.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace bx {

// The capture's frame pool and the graphics device need a few hundred megabytes of memory the machine has to have. Below this libobs does
// not report a failure - it faults - so the engine checks first and says something a person can act on.
constexpr int64_t kMinFreeMemoryForVideoMb = 768;

// `free_mb` < 0 means Windows would not say: that is not a reason to refuse.
inline bool capture_memory_ok(int64_t free_mb) { return free_mb < 0 || free_mb >= kMinFreeMemoryForVideoMb; }

// What a person reads when it is not ok. A fresh start has no app to "try again": the window is closed and the person opens it again; a
// running engine only needs the buffer started again. The free figure goes to the log, not to the person.
inline std::string memory_full_message(bool fresh_start) {
  if (fresh_start) return "Your RAM looks full.\n\nClose some apps, then try starting BuffrX again.";
  return "Your RAM looks full. Close some apps, then start the buffer again.";
}

// A probe, not a setting: BUFFRX_TEST_FREE_MB=300 makes the engine read that much free memory, so the refusal can be exercised without
// filling the machine. `text` is the variable's value (null or not a number: the real reading stands).
inline int64_t free_memory_reading(int64_t real_mb, const char* text) {
  if (!text || !*text) return real_mb;
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  return (end && *end == '\0') ? static_cast<int64_t>(value) : real_mb;
}

// While the buffer runs nothing else watches memory: when it falls under the capture's need one warn line names the cause, so a later
// crash in engine.log has a reason beside it. Read every 5 s, said once per episode, never shown to the person (a game is in front).
struct MemoryWatch {
  uint32_t next_ms = 0;
  bool low = false;
  // True when the warn line is due now.
  bool observe(int64_t free_mb, bool buffering, uint32_t now_ms) {
    if (static_cast<int32_t>(now_ms - next_ms) < 0) return false;
    next_ms = now_ms + 5000;
    const bool is_low = buffering && !capture_memory_ok(free_mb);
    const bool say = is_low && !low;
    low = is_low;
    return say;
  }
};

}  // namespace bx
