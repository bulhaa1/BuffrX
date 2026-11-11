// The arithmetic of the video thread's frame wait (sleep_patch.cpp). libobs waits for each frame with Sleep(ms - 1) and then spins
// on QueryPerformanceCounter until the deadline, every frame: 26-35% of a core at 144 fps, 5-12% at 60 (docs/measurement.md). The
// replacement sleeps on a high-resolution timer until a short margin before the deadline and spins only that margin. Pure, so the
// conversions are tested; the Win32 calls live in sleep_patch.cpp.
#pragma once

#include <cstdint>

namespace bx {

// QueryPerformanceCounter ticks for a libobs timestamp in nanoseconds (os_gettime_ns is the counter scaled to ns). 128-bit:
// ns x freq passes 64 bits after a few hours of uptime.
inline int64_t ticks_from_ns(uint64_t ns, uint64_t freq) {
  return static_cast<int64_t>((static_cast<unsigned __int128>(ns) * freq) / 1000000000ull);
}

// The coarse part of a wait of `remaining_ticks`: the relative due time for SetWaitableTimer (negative, 100 ns units) that ends
// `margin_us` before the deadline, or 0 when the wait is no longer than the margin, which is all spin.
inline int64_t coarse_wait_100ns(int64_t remaining_ticks, int64_t freq, int64_t margin_us) {
  if (remaining_ticks <= 0 || freq <= 0) return 0;
  const int64_t margin_ticks = freq * margin_us / 1000000;
  if (remaining_ticks <= margin_ticks) return 0;
  return -static_cast<int64_t>((static_cast<__int128>(remaining_ticks - margin_ticks) * 10000000) / freq);
}

// One pass of the frame wait: sleep `wait_us` on the timer, then spin at most `spin_us`. The spin never crosses the margin, so a
// timer that wakes early costs a shorter wait on the next pass rather than a spin across everything that is left (the 18% of a
// core at 144 fps an early wake used to cost).
struct SleepStep {
  int64_t wait_us = 0;
  int64_t spin_us = 0;
};

inline SleepStep plan_sleep_step(int64_t remaining_us, int64_t margin_us) {
  if (remaining_us <= 0) return {0, 0};
  if (margin_us < 0) margin_us = 0;
  if (remaining_us <= margin_us) return {0, remaining_us};
  return {remaining_us - margin_us, margin_us};
}

}  // namespace bx
