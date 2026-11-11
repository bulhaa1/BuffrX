// When an idle engine goes to sleep, decided without libobs so it can be tested.
//
// Idle already meant: no buffer, no recording, the capture and audio sources released after a grace
// (ObsHost::release_idle_sources). What was left running was libobs's video thread, at the capture frame rate,
// rendering an empty canvas - and libobs ends every frame by spinning for the last millisecond or two
// (os_sleepto_ns), so the cost is not the rendering but the frame rate itself: measured with the buffer off,
// 0.9% of a core at 10 fps, about 7% at 60 and 26-35% at 144. Sleep drops the canvas to a few frames a second
// (a video reset: outputs and encoders go with it, which also frees the GPU encoder) and the next start builds
// it all again from the config, about 70 ms measured.

#ifndef BUFFRX_IDLE_POLICY_H
#define BUFFRX_IDLE_POLICY_H

#include <cstdint>

namespace buffrx {

// The canvas rate while the engine sleeps. Low enough that the spin is a rounding error (a 5 fps frame is
// 200 ms and the spin is about 2 ms of it), high enough that the video thread, which cannot be interrupted
// mid-sleep, joins within a frame when the engine wakes.
constexpr int kDeepIdleFps = 5;
// A capture rate at or under this is already cheap: it is not worth a video reset to save under 1% of a core.
constexpr int kDeepIdleCheapFps = 10;
// A failed attempt (the video would not reset) is not tried again every tick.
constexpr uint32_t kDeepIdleRetryMs = 60000;

inline int deep_idle_fps(int capture_fps) {
  if (capture_fps <= 0) return kDeepIdleFps;
  return capture_fps < kDeepIdleFps ? capture_fps : kDeepIdleFps;
}

inline bool deep_idle_worthwhile(int capture_fps) { return capture_fps > kDeepIdleCheapFps; }

struct IdleView {
  bool buffer_wanted = false;     // the ring is running or waiting for its first frame
  bool recording = false;         // a manual recording runs or waits for its first frame
  bool sources_released = false;  // the capture and audio sources are gone (the grace has passed)
  bool asleep = false;            // already at the idle rate
};

// `failed_at_ms` is when the last attempt failed (0: none).
inline bool deep_idle_due(const IdleView& view, int capture_fps, uint32_t now_ms, uint32_t failed_at_ms) {
  if (view.asleep || view.buffer_wanted || view.recording || !view.sources_released) return false;
  if (!deep_idle_worthwhile(capture_fps)) return false;
  if (failed_at_ms != 0 && now_ms - failed_at_ms < kDeepIdleRetryMs) return false;
  return true;
}

}  // namespace buffrx

#endif  // BUFFRX_IDLE_POLICY_H
