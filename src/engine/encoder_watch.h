// Whether the video encoder keeps up with the frames the video thread renders, decided without libobs so
// the rule can be tested.
//
// The bug this exists for: a heavy NVENC setting (AV1, preset p7, the UHQ tune, full-resolution multipass)
// at 120 fps encoded about 70% of the frames on an RTX 5080. libobs skipped the rest, every clip came out
// choppy while the game itself was smooth, and nothing in the product said why. The output's own counters
// show no drop in that case - only the encoder's frame count falls behind the video thread's.

#ifndef BUFFRX_ENCODER_WATCH_H
#define BUFFRX_ENCODER_WATCH_H

#include <cstdint>

namespace buffrx {

// Encoded frames over rendered frames, below which the encoder is not keeping up. Healthy runs sit at
// 0.98 and above; the measured failure was 0.70.
constexpr double kEncoderBehindRatio = 0.90;
// A verdict is taken over a window, and it takes this many bad windows in a row to count: a one-second
// hitch (an alt-tab, a level load) is not an encoder that cannot keep up. Two two-second windows put the
// first verdict at four seconds, so a relief step lands inside the first clip rather than after it.
constexpr uint32_t kEncoderWatchWindowMs = 2000;
constexpr int kEncoderWatchBadWindows = 2;
// Fewer rendered frames than this in a window is too little to judge.
constexpr uint64_t kEncoderWatchMinFrames = 60;

struct EncoderWatch {
  bool primed = false;
  uint32_t window_start_ms = 0;
  uint64_t window_video = 0;
  uint64_t window_encoded = 0;
  int bad_windows = 0;
  bool behind = false;
  double ratio = 1.0;  // the last window's
};

// One sample of the two counters, in the caller's clock. `encoding` is whether the video encoder is
// running at all. Returns true when `behind` changed.
inline bool encoder_watch_feed(EncoderWatch& watch, uint32_t now_ms, uint64_t video_frames, uint64_t encoder_frames,
                               bool encoding) {
  const bool was_behind = watch.behind;
  const bool moved_back = watch.primed && (video_frames < watch.window_video || encoder_frames < watch.window_encoded);
  if (!encoding || moved_back) {
    watch = EncoderWatch{};
    return was_behind;
  }
  if (!watch.primed) {
    watch.primed = true;
    watch.window_start_ms = now_ms;
    watch.window_video = video_frames;
    watch.window_encoded = encoder_frames;
    return false;
  }
  if (now_ms - watch.window_start_ms < kEncoderWatchWindowMs) return false;
  const uint64_t rendered = video_frames - watch.window_video;
  const uint64_t encoded = encoder_frames - watch.window_encoded;
  if (rendered >= kEncoderWatchMinFrames) {
    watch.ratio = static_cast<double>(encoded) / static_cast<double>(rendered);
    if (watch.ratio < kEncoderBehindRatio) {
      if (++watch.bad_windows >= kEncoderWatchBadWindows) watch.behind = true;
    } else {
      watch.bad_windows = 0;
      watch.behind = false;
    }
  }
  watch.window_start_ms = now_ms;
  watch.window_video = video_frames;
  watch.window_encoded = encoder_frames;
  return watch.behind != was_behind;
}

}  // namespace buffrx

#endif  // BUFFRX_ENCODER_WATCH_H
