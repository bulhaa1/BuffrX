#include "sleep_patch.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../shared/log.h"
#include "sleep_wait.h"

namespace bx {

namespace {

// The last stretch of a frame wait that is spun rather than slept. The timer's wake-up is usually within 100-200 us of the
// deadline on a high-resolution timer; this leaves room for a late one without spending the frame on a spin.
constexpr int64_t kSpinMarginUs = 400;
// A wait's own timeout beyond the coarse wait it asks for. A timer that never fires costs one more pass, never a hung video thread.
constexpr DWORD kWaitSlackMs = 2;
// How often the tracing line is written, when tracing is on.
constexpr int64_t kTraceIntervalUs = 2000000;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

struct TimerHandle {
  HANDLE handle = nullptr;
  ~TimerHandle() {
    if (handle) CloseHandle(handle);
  }
};

HANDLE make_timer() { return CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); }

int64_t us_from_ticks(int64_t ticks, uint64_t freq) {
  if (freq == 0) return 0;
  return static_cast<int64_t>((static_cast<__int128>(ticks) * 1000000) / freq);
}

int64_t ticks_from_us(int64_t us, uint64_t freq) {
  if (freq == 0 || us <= 0) return 0;
  return static_cast<int64_t>((static_cast<__int128>(us) * freq) / 1000000);
}

void spin_until(int64_t stop_tick) {
  LARGE_INTEGER count;
  for (;;) {
    QueryPerformanceCounter(&count);
    if (count.QuadPart >= stop_tick) return;
    YieldProcessor();
  }
}

// BUFFRX_SLEEP_TRACE=1 records what the wait actually did, so the 144 fps spread (18.2 / 0.7 / 19.1% of a core on one binary) can
// be read instead of guessed. One thread-local tally, written as one info line every 2 s: no per-call log, no lock on the path.
struct SleepTrace {
  uint64_t calls = 0;
  uint64_t waits = 0;
  uint64_t coarse_count = 0;
  uint64_t timeouts = 0;
  uint64_t set_failures = 0;
  uint64_t timer_missing = 0;
  uint64_t fallbacks = 0;
  uint64_t cpu_changes = 0;
  int64_t spin_sum_us = 0;
  int64_t spin_max_us = 0;
  int64_t entry_sum_us = 0;
  int64_t entry_earliest_us = 0;
  int64_t entry_late_max_us = 0;
  int64_t wait_sum_us = 0;
  int64_t coarse_sum_us = 0;
  int64_t wake_sum_us = 0;
  int64_t wake_earliest_us = 0;
  int64_t wake_latest_us = 0;
  int64_t exit_late_sum_us = 0;
  int64_t exit_late_max_us = 0;
  int64_t window_start_us = 0;
};

bool trace_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("BUFFRX_SLEEP_TRACE");
    const bool on = value && value[0] == '1';
    if (on) BX_LOG_INFO("sleep trace on (BUFFRX_SLEEP_TRACE=1): one line every 2 s");
    return on;
  }();
  return enabled;
}

SleepTrace& trace_state() {
  thread_local SleepTrace state;
  return state;
}

// The call's remaining time at entry, kept as a signed sample so an early call and a late one both read.
void entry_sample(SleepTrace& trace, int64_t remaining_us) {
  trace.entry_sum_us += remaining_us;
  if (remaining_us < trace.entry_earliest_us) trace.entry_earliest_us = remaining_us;
  if (remaining_us > trace.entry_late_max_us) trace.entry_late_max_us = remaining_us;
}

// Called at the end of one wait when tracing. `now_us` is the counter read for this call's timing.
void trace_maybe_log(SleepTrace& trace, int64_t now_us) {
  if (trace.window_start_us == 0) trace.window_start_us = now_us;
  const int64_t elapsed_us = now_us - trace.window_start_us;
  if (elapsed_us < kTraceIntervalUs) return;
  const int64_t calls = static_cast<int64_t>(trace.calls);
  const int64_t waits = static_cast<int64_t>(trace.waits);
  const int64_t coarse = static_cast<int64_t>(trace.coarse_count);
  BX_LOG_INFO(
      "sleep trace: %lld calls in %lld ms, entry remaining avg %lld us (earliest %+lld, latest %+lld), spin avg %lld us max %lld us, "
      "coarse waits %lld (avg %lld us), wait avg %lld us, "
      "wake early/late avg %+lld us (earliest %+lld, latest %+lld), late exit avg %lld us max %lld us, timeouts %llu, "
      "set failures %llu, timer missing %llu, fallbacks %llu, processor changes %llu",
      calls, elapsed_us / 1000, calls ? trace.entry_sum_us / calls : 0, trace.entry_earliest_us, trace.entry_late_max_us,
      calls ? trace.spin_sum_us / calls : 0, trace.spin_max_us,
      coarse, coarse ? trace.coarse_sum_us / coarse : 0, waits ? trace.wait_sum_us / waits : 0,
      waits ? trace.wake_sum_us / waits : 0, trace.wake_earliest_us, trace.wake_latest_us,
      calls ? trace.exit_late_sum_us / calls : 0, trace.exit_late_max_us,
      trace.timeouts, trace.set_failures, trace.timer_missing, trace.fallbacks, trace.cpu_changes);
  trace = SleepTrace{};
  trace.window_start_us = now_us;
}

// Same contract as libobs's os_sleepto_ns: true when it had to wait for the deadline, false when it had already passed (the
// video thread counts that as a late frame).
bool precise_sleepto_ns(uint64_t time_target) {
  static const uint64_t freq = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
  }();
  const int64_t target = ticks_from_ns(time_target, freq);
  const bool tracing = trace_enabled();
  SleepTrace* trace = tracing ? &trace_state() : nullptr;  // nothing below touches it while tracing is off
  const DWORD cpu_before = tracing ? GetCurrentProcessorNumber() : 0;
  LARGE_INTEGER count;
  QueryPerformanceCounter(&count);
  const int64_t entry_tick = count.QuadPart;
  const int64_t entry_remaining_us = tracing ? us_from_ticks(target - entry_tick, freq) : 0;
  if (count.QuadPart >= target) {
    if (tracing) {
      trace->calls++;
      entry_sample(*trace, entry_remaining_us);
      trace_maybe_log(*trace, us_from_ticks(entry_tick, freq));
    }
    return false;
  }
  thread_local TimerHandle timer;
  if (!timer.handle) timer.handle = make_timer();
  int64_t spin_total_us = 0;
  int64_t wait_total_us = 0;
  int64_t coarse_total_us = 0;
  uint64_t coarse_count = 0;
  DWORD cpu_after = 0;
  bool have_cpu_after = false;
  for (;;) {
    QueryPerformanceCounter(&count);
    if (count.QuadPart >= target) break;
    const int64_t remaining_us = us_from_ticks(target - count.QuadPart, freq);
    const SleepStep step = plan_sleep_step(remaining_us, kSpinMarginUs);
    const int64_t coarse = coarse_wait_100ns(target - count.QuadPart, static_cast<int64_t>(freq), kSpinMarginUs);
    if (step.wait_us <= 0 || coarse == 0) {
      // The last stretch: spin no more than the step's share, then look at the clock again.
      const int64_t spin_ticks = ticks_from_us(step.spin_us, freq);
      const int64_t stop = count.QuadPart + spin_ticks < target ? count.QuadPart + spin_ticks : target;
      const int64_t started = count.QuadPart;
      spin_until(stop);
      QueryPerformanceCounter(&count);
      spin_total_us += us_from_ticks(count.QuadPart - started, freq);
      continue;
    }
    LARGE_INTEGER due;
    due.QuadPart = coarse;
    if (timer.handle && SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
      // Bounded: a timer that never fires is one more pass, not a hung video thread.
      const DWORD timeout = static_cast<DWORD>((step.wait_us + 999) / 1000) + kWaitSlackMs;
      QueryPerformanceCounter(&count);
      const int64_t wait_started = count.QuadPart;
      const DWORD result = WaitForSingleObject(timer.handle, timeout);
      QueryPerformanceCounter(&count);
      wait_total_us += us_from_ticks(count.QuadPart - wait_started, freq);
      coarse_total_us += step.wait_us;
      coarse_count++;
      if (tracing) {
        trace->waits++;
        if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT) {
          const int64_t sample = us_from_ticks(count.QuadPart - target, freq);
          trace->wake_sum_us += sample;
          if (sample < trace->wake_earliest_us) trace->wake_earliest_us = sample;
          if (sample > trace->wake_latest_us) trace->wake_latest_us = sample;
        }
        if (result == WAIT_TIMEOUT) {
          trace->timeouts++;
        } else if (result == WAIT_FAILED) {
          // A timer that will not be waited on is a millisecond, then the loop re-reads the clock.
          Sleep(1);
          trace->fallbacks++;
        }
        cpu_after = GetCurrentProcessorNumber();
        have_cpu_after = true;
      } else if (result == WAIT_FAILED) {
        Sleep(1);
      }
    } else {
      // No timer, or it would not arm: sleep the coarse span minus a millisecond, then re-evaluate. Never spin it.
      const DWORD milliseconds = static_cast<DWORD>(step.wait_us / 1000);
      if (milliseconds > 1) {
        Sleep(milliseconds - 1);
        if (tracing) trace->fallbacks++;
      } else {
        const int64_t spin_ticks = ticks_from_us(step.spin_us, freq);
        const int64_t stop = count.QuadPart + spin_ticks < target ? count.QuadPart + spin_ticks : target;
        const int64_t started = count.QuadPart;
        spin_until(stop);
        spin_total_us += us_from_ticks(stop - started, freq);
      }
      if (tracing) {
        if (!timer.handle) trace->timer_missing++;
        else trace->set_failures++;
      }
    }
  }
  QueryPerformanceCounter(&count);
  if (tracing) {
    trace->calls++;
    entry_sample(*trace, entry_remaining_us);
    trace->spin_sum_us += spin_total_us;
    if (spin_total_us > trace->spin_max_us) trace->spin_max_us = spin_total_us;
    trace->coarse_count += coarse_count;
    trace->coarse_sum_us += coarse_total_us;
    trace->wait_sum_us += wait_total_us;
    if (!have_cpu_after) {
      cpu_after = GetCurrentProcessorNumber();
      have_cpu_after = true;
    }
    if (cpu_after != cpu_before) trace->cpu_changes++;
    const int64_t exit_late_us = us_from_ticks(count.QuadPart - target, freq);
    trace->exit_late_sum_us += exit_late_us;
    if (exit_late_us > trace->exit_late_max_us) trace->exit_late_max_us = exit_late_us;
    trace_maybe_log(*trace, us_from_ticks(count.QuadPart, freq));
  }
  return true;
}

// The first 94 bytes of os_sleepto_ns in the pinned OBS 33.0.1 obs.dll (offset 0xc9bc0 of the 2026-10 release), with the
// RIP-relative displacements masked: they move with the build, the instructions do not.
const uint8_t kExpected[] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x20, 0x80, 0x3d, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8b, 0xd9, 0x75, 0x14, 0x48, 0x8d, 0x0d, 0x00, 0x00, 0x00, 0x00, 0xff, 0x15, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x05, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x48, 0x8b, 0x3d, 0x00, 0x00, 0x00, 0x00, 0xb9, 0x00, 0xca, 0x9a, 0x3b, 0x48, 0x8b, 0xc7, 0x48, 0xf7, 0xe3, 0x48,
    0xf7, 0xf1, 0x48, 0x8d, 0x4c, 0x24, 0x38, 0x48, 0x8b, 0xd8, 0xff, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4c, 0x24, 0x38, 0x48,
    0x3b, 0xcb, 0x40, 0x0f, 0x9c, 0xc6};
// Displacement fields of kExpected: [start, start + 4).
const int kDisplacements[] = {17, 30, 36, 42, 50, 78};

bool matches_pinned(const uint8_t* code) {
  uint8_t mask[sizeof(kExpected)];
  std::memset(mask, 0xff, sizeof(mask));
  for (int start : kDisplacements) std::memset(mask + start, 0x00, 4);
  for (size_t i = 0; i < sizeof(kExpected); ++i) {
    if ((code[i] & mask[i]) != (kExpected[i] & mask[i])) return false;
  }
  return true;
}

}  // namespace

bool install_sleep_patch(std::string& note) {
  // On by default since 2026-11-02. Measured that night on window capture of the present loop (docs/measurements-optimization-baseline.md):
  // the median buffering CPU fell 66.7% at 144 fps (49.32% -> 16.41% of a core), 79% at 120 fps and 72% at 60 fps, with no late or
  // dropped frame in any round. Frame continuity against a fullscreen game (B7) was not measured, because that test was not permitted
  // that night. BUFFRX_NO_SLEEP_PATCH=1 turns it off, which is how a build that is suspected of timing trouble is A/B'd.
  const char* off = std::getenv("BUFFRX_NO_SLEEP_PATCH");
  if (off && off[0] == '1') {
    note = "off: BUFFRX_NO_SLEEP_PATCH=1";
    return false;
  }
  const HMODULE obs = GetModuleHandleA("obs.dll");
  auto* target = obs ? reinterpret_cast<uint8_t*>(GetProcAddress(obs, "os_sleepto_ns")) : nullptr;
  if (!target) {
    note = "os_sleepto_ns was not found in obs.dll";
    return false;
  }
  if (!matches_pinned(target)) {
    note = "os_sleepto_ns does not start with the bytes of the pinned OBS 33.0.1 obs.dll; libobs is left as it is";
    return false;
  }
  HANDLE probe = make_timer();
  if (!probe) {
    note = "this Windows has no high-resolution waitable timer; libobs is left as it is";
    return false;
  }
  CloseHandle(probe);
  // mov rax, imm64 ; jmp rax. The function is 256 bytes long, so 12 fit; nothing runs it yet (the video thread starts at the first
  // obs_reset_video).
  uint8_t jump[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
  const uint64_t address = reinterpret_cast<uint64_t>(&precise_sleepto_ns);
  std::memcpy(jump + 2, &address, sizeof(address));
  DWORD previous = 0;
  if (!VirtualProtect(target, sizeof(jump), PAGE_EXECUTE_READWRITE, &previous)) {
    note = "the function's page could not be made writable";
    return false;
  }
  std::memcpy(target, jump, sizeof(jump));
  VirtualProtect(target, sizeof(jump), previous, &previous);
  FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
  note = "libobs's frame wait now sleeps on a high-resolution timer instead of spinning";
  return true;
}

}  // namespace bx
