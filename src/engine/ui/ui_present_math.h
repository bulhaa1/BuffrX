// What the native interface does when a Present does not show its frame: the outcome of each attempt, and the retry that
// follows it. Pure logic, no window and no GPU, so the rules are unit-tested (tests/ui_present_tests.cpp).
//
// A frame that did not reach the display owns its window until its retry. While the retry is pending, or spent and re-arming,
// the ordinary causes (new data, a moving value, an animation, thumbnails, the settle frame) wait for it: they never ask the
// display at its own rate. Deliberate input, activation, a capture and a pad press draw at once. The fast attempts of an
// episode are bounded, then one attempt a second is made for a minute, then nothing until a deliberate input. A hidden or
// minimized window keeps no retry at all.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace bx::ui {

// What one attempt to present a frame (or to make the target it draws into) came to.
enum class PresentOutcome {
  shown,      // the frame is on the display's queue
  busy,       // DXGI_ERROR_WAS_STILL_DRAWING: the display has not taken the previous frame yet
  occluded,   // DXGI_STATUS_OCCLUDED: the window is covered, so DWM takes no frames for it
  lost,       // DEVICE_REMOVED, DEVICE_RESET or DEVICE_HUNG: the device is gone and every resource with it
  failed,     // any other failure of Present
  no_target,  // the back buffer or its render target could not be made (after a resize, or a device that answers badly)
};

// The retry of a frame that did not show. `due_us` is when the next attempt is due (0: none pending). `misses` counts the attempts
// of the episode that did not show the frame. `gave_up` is set once the fast attempts are spent; from then on a pending retry is a
// slow re-arm, and with nothing pending the window waits for a deliberate input.
struct PresentRetry {
  int64_t due_us = 0;
  int misses = 0;
  bool gave_up = false;
};

// Fast attempts an episode makes before it is spent. Each one is a whole frame of work, so this bounds the first burst.
constexpr int kPresentMisses = 12;
// Slow re-arms after the episode is spent: one a second, so a display that comes back shows the picture within a second, and the
// count keeps the total finite (a minute at most, then a deliberate input is needed).
constexpr int kPresentRearms = 60;
// A display that is still drawing the last frame is asked again soon: long enough that the display has taken it (a slot is
// about a refresh) and past frame_allowed's 50 ms give-up, so the retry never waits on a display that will not signal.
constexpr int64_t kBusyRetryUs = 60000;
// A covered or failing window backs off from this interval, doubling, up to kBackoffMaxUs.
constexpr int64_t kOccludedRetryUs = 250000;
constexpr int64_t kBackoffMaxUs = 1000000;
// Device rebuilds one run of failures may cause before the interface waits for a deliberate input instead (a broken device must not
// loop). A frame that shows starts a new run; a deliberate input on a spent window grants one more rebuild (grant_rebuild), at most
// once per kInputRebuildGapUs.
constexpr int kResourceRebuilds = 3;

inline PresentOutcome classify_present(uint32_t hresult) {
  constexpr uint32_t kOccludedStatus = 0x087A0001u;     // DXGI_STATUS_OCCLUDED
  constexpr uint32_t kStillDrawing = 0x887A000Au;       // DXGI_ERROR_WAS_STILL_DRAWING
  constexpr uint32_t kDeviceRemoved = 0x887A0005u;      // DXGI_ERROR_DEVICE_REMOVED
  constexpr uint32_t kDeviceHung = 0x887A0006u;         // DXGI_ERROR_DEVICE_HUNG
  constexpr uint32_t kDeviceReset = 0x887A0007u;        // DXGI_ERROR_DEVICE_RESET
  // Occlusion is a success code, so it is matched before the failure test.
  if (hresult == kOccludedStatus) return PresentOutcome::occluded;
  if (hresult == kStillDrawing) return PresentOutcome::busy;
  if (hresult == kDeviceRemoved || hresult == kDeviceHung || hresult == kDeviceReset) return PresentOutcome::lost;
  if (static_cast<int32_t>(hresult) < 0) return PresentOutcome::failed;
  return PresentOutcome::shown;
}

// The state after one attempt. A shown frame clears the retry, and so does a lost device (the frame pass rebuilds the device
// instead). Every other outcome schedules the next attempt: fast while the episode lasts, then one a second, then nothing.
inline PresentRetry next_retry(PresentRetry before, PresentOutcome outcome, int64_t now_us) {
  if (outcome == PresentOutcome::shown || outcome == PresentOutcome::lost) return PresentRetry{};
  PresentRetry next;
  next.misses = before.misses + 1;
  next.gave_up = next.misses > kPresentMisses;
  if (next.misses > kPresentMisses + kPresentRearms) return next;  // spent and re-armed: nothing pending until a deliberate input
  int64_t delay = kBusyRetryUs;
  if (next.gave_up) {
    delay = kBackoffMaxUs;
  } else if (outcome != PresentOutcome::busy) {
    // 250 ms, 500 ms, 1 s, then the cap: a covered window costs little, and it stops asking soon.
    const int doublings = std::min(next.misses - 1, 2);
    delay = std::min<int64_t>(kOccludedRetryUs << doublings, kBackoffMaxUs);
  }
  next.due_us = now_us + delay;
  return next;
}

// The attempt that spends the episode: the one that starts the slow re-arms. A device rebuild is asked for at that attempt only.
inline bool retry_gave_up_now(PresentRetry retry) { return retry.gave_up && retry.misses == kPresentMisses + 1; }

// The state of a window whose device cannot be used and that makes no further attempt: only a deliberate input draws it again.
inline PresentRetry retry_spent() {
  PresentRetry spent;
  spent.gave_up = true;
  spent.misses = kPresentMisses + kPresentRearms + 1;
  return spent;
}

inline bool retry_pending(PresentRetry retry) { return retry.due_us != 0; }

// Whether the ordinary causes of a window wait this pass. A window whose frame failed holds them until its retry is due, unless a
// deliberate input, a capture or a pad press asks now. A window without a failure is never held.
inline bool retry_holds(PresentRetry retry, bool due_now, bool deliberate) {
  return (retry.gave_up || retry_pending(retry)) && !due_now && !deliberate;
}

// A retry is due only while the window is on screen: a hidden or minimized window does no GPU work, and the restore that brings it
// back sends its own frame request.
inline bool retry_due(PresentRetry retry, int64_t now_us, bool on_screen) {
  return on_screen && retry_pending(retry) && now_us >= retry.due_us;
}

// The same retry, or nothing when the window is off screen. Leaving the screen forgets the episode, so a restore starts fresh.
inline PresentRetry retry_while_shown(PresentRetry retry, bool on_screen) { return on_screen ? retry : PresentRetry{}; }

// A due retry that the display did not take this pass: no attempt was made, so nothing is counted against the episode. It asks
// again on the busy deadline, which is cheap: a strict-game window waits for a free display slot, not a poll.
inline PresentRetry retry_later(PresentRetry retry, int64_t now_us) {
  retry.due_us = now_us + kBusyRetryUs;
  return retry;
}

// How long the loop may sleep for a pending retry, in milliseconds. At least 4 ms: a retry that is due is drawn or moved later in
// the same pass, so the loop never asks for a zero wait.
inline uint32_t retry_wait_ms(PresentRetry retry, int64_t now_us) {
  if (!retry_pending(retry)) return 0;
  const int64_t ms = (retry.due_us - now_us) / 1000 + 1;
  return static_cast<uint32_t>(std::max<int64_t>(4, ms));
}

// Whether an episode that spent its attempts should rebuild the device. A covered or busy window is not a broken one.
inline bool escalates(PresentOutcome outcome) {
  return outcome == PresentOutcome::failed || outcome == PresentOutcome::no_target;
}

// Whether another device rebuild may be made for the run of failures in progress (`rebuilds_so_far` counts those already made).
inline bool may_rebuild(int rebuilds_so_far) { return rebuilds_so_far < kResourceRebuilds; }

// A deliberate input on a window whose rebuilds are spent may grant one more rebuild, at most once per kInputRebuildGapUs: a held key
// repeats its input some thirty times a second, and each grant is a device rebuild, so the grants must not follow the events.
constexpr int64_t kInputRebuildGapUs = 5000000;
inline bool may_grant_rebuild(int rebuilds_so_far, int64_t now_us, int64_t last_grant_us) {
  return rebuilds_so_far >= kResourceRebuilds && (last_grant_us == 0 || now_us - last_grant_us >= kInputRebuildGapUs);
}
// The budget after such a grant: one rebuild more, and no more than that.
inline int grant_rebuild(int rebuilds_so_far) { return rebuilds_so_far >= kResourceRebuilds ? kResourceRebuilds - 1 : rebuilds_so_far; }

// ---------------------------------------------------------------- test faults (BUFFRX_UI_TEST_PRESENT, BUFFRX_UI_TEST_TARGET)
// For a private native probe only. Each replaces the return of one call (a Present, or a target made), so the real frame, retry and
// rebuild paths run; nothing is lost, hung or reset on the GPU. Both are read once, and both are off when the variable is unset.
enum class PresentFault {
  none,
  busy_once,  // the first Present attempt returns busy; the rest are real
  busy,       // every Present attempt returns busy
  occluded,   // every Present attempt returns occluded
  lost_once,  // the first Present attempt returns device removed; the rest are real
  lost,       // every Present attempt returns device removed (a rebuilt device that fails its first present again)
};

enum class TargetFault {
  none,
  once,        // the first target made fails; the rest are real
  persistent,  // every target made fails
};

inline PresentFault parse_present_fault(const char* text) {
  if (!text) return PresentFault::none;
  if (std::strcmp(text, "busy-once") == 0) return PresentFault::busy_once;
  if (std::strcmp(text, "busy") == 0) return PresentFault::busy;
  if (std::strcmp(text, "occluded") == 0) return PresentFault::occluded;
  if (std::strcmp(text, "lost-once") == 0) return PresentFault::lost_once;
  if (std::strcmp(text, "lost") == 0) return PresentFault::lost;
  return PresentFault::none;
}

inline TargetFault parse_target_fault(const char* text) {
  if (!text) return TargetFault::none;
  if (std::strcmp(text, "once") == 0) return TargetFault::once;
  if (std::strcmp(text, "persistent") == 0) return TargetFault::persistent;
  return TargetFault::none;
}

// Whether Present attempt `attempt` (1 for the first) has its return replaced by `fault`.
inline bool present_fault_applies(PresentFault fault, int attempt) {
  switch (fault) {
    case PresentFault::busy_once:
    case PresentFault::lost_once: return attempt == 1;
    case PresentFault::busy:
    case PresentFault::occluded:
    case PresentFault::lost: return true;
    case PresentFault::none: break;
  }
  return false;
}

// The HRESULT a replaced Present returns: the same code the display gives for that condition.
inline uint32_t present_fault_hresult(PresentFault fault) {
  switch (fault) {
    case PresentFault::busy_once:
    case PresentFault::busy: return 0x887A000Au;     // DXGI_ERROR_WAS_STILL_DRAWING
    case PresentFault::occluded: return 0x087A0001u;  // DXGI_STATUS_OCCLUDED
    case PresentFault::lost_once:
    case PresentFault::lost: return 0x887A0005u;     // DXGI_ERROR_DEVICE_REMOVED
    case PresentFault::none: break;
  }
  return 0;
}

// Whether target call `call` (1 for the first) fails by `fault`.
inline bool target_fault_applies(TargetFault fault, int call) {
  return fault == TargetFault::persistent || (fault == TargetFault::once && call == 1);
}

}  // namespace bx::ui
