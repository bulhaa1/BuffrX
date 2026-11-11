// How long the main loop may sleep. It used to come round every 20 ms whatever was due (about 67 passes a second with nothing
// on screen); it now sleeps until the earliest deadline it owns, and anything else that needs it (a message, a request, a reply,
// an event) wakes it. Pure, so the arithmetic is tested; tick counts are 32-bit and wrap.
#pragma once

#include <algorithm>
#include <cstdint>

namespace bx {

// Milliseconds until `period` has passed since `since`, 0 when it already has. Wrap-safe.
inline uint32_t ms_until(uint32_t now, uint32_t since, uint32_t period) {
  const uint32_t elapsed = now - since;
  return elapsed >= period ? 0 : period - elapsed;
}

// The sleep for one pass: the earliest of the deadlines, at least 1 ms (a due deadline runs on the next pass, not in a spin),
// at most `ceiling`.
inline uint32_t loop_sleep_ms(uint32_t ceiling, uint32_t earliest_deadline_ms) {
  return std::max<uint32_t>(1, std::min(ceiling, earliest_deadline_ms));
}

}  // namespace bx
