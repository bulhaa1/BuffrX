// When to lower the encoder's cost, decided without libobs so the rule can be tested.
//
// An encoder that cannot keep up does not fail: libobs skips the frames it has no time for, and the clip
// comes out with a third of them missing while the game itself runs smoothly. A person with a heavy NVENC
// preset (AV1, p7, UHQ, full-resolution multipass at 120 fps) had exactly that, and a warning did not fix
// it. The governor steps the encoder down by itself: a faster preset, then no multipass and no look-ahead,
// then the fastest the encoder has. Each step is a rebuild of the encoder, so it is spaced, and the step
// reached is remembered so the next launch starts where the last one settled.

#ifndef BUFFRX_ENCODER_GOVERNOR_H
#define BUFFRX_ENCODER_GOVERNOR_H

#include <cstdint>

namespace buffrx {

constexpr int kMaxGovernorStep = 3;
// The relief ladder continues past the encoder rungs: 4 drops the capture to 60 fps and 5 to 30, never the
// resolution. A frame-rate rung needs a video reset, so it is only entered while no source is live; mid-session
// the ladder stops at kMaxGovernorStep and the saved step applies at the next start.
constexpr int kMaxReliefStep = 5;
// After a step the new encoder is given this long to settle (the watch needs two four-second windows to
// reach a verdict, and a rebuild empties the ring) before it may be judged and stepped again.
constexpr uint32_t kGovernorCooldownMs = 20000;

// Whether to take the next step now. `last_step_ms` is the tick clock when the previous step was taken (0
// when none was). The clock is a 32-bit tick count, so the interval is taken as an unsigned difference.
// A step rebuilds the encoder, and a rebuild closes the file a manual recording is writing and opens
// another: a recording in progress holds the ladder until it ends, so the clip stays one file.
inline bool governor_should_step(int step, bool behind, uint32_t now_ms, uint32_t last_step_ms,
                                 bool manual_recording = false) {
  if (manual_recording) return false;
  if (!behind || step >= kMaxReliefStep) return false;
  if (last_step_ms == 0) return true;
  return now_ms - last_step_ms >= kGovernorCooldownMs;
}

// The capture rate a relief step runs, from the one chosen. Resolution is never part of the ladder.
inline int relief_capture_fps(int configured_fps, int step) {
  if (step >= 5) return 30;
  if (step >= 4) return configured_fps > 60 ? 60 : configured_fps;
  return configured_fps;
}

}  // namespace buffrx

#endif  // BUFFRX_ENCODER_GOVERNOR_H
