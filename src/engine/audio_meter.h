// Peak metering for the interface. This changes the display, never the recorded audio.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace bx {

constexpr float kAudioMeterSilenceDb = -100.0f;
constexpr unsigned kAudioMeterSampleMs = 100;
constexpr unsigned kMicrophoneMeterSampleMs = 200;
constexpr float kMicrophoneMeterFloorDb = -42.0f;

// The scale is linear in dBFS, from -60 dBFS to 0 dBFS. A linear amplitude scale hides ordinary speech.
inline float audio_meter_level(float db) {
  if (std::isnan(db)) return 0.0f;
  return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
}

// The microphone's display ignores low background noise and spreads the remaining range over the meter.
inline float microphone_meter_level(float db) {
  if (std::isnan(db)) return 0.0f;
  return std::clamp((db - kMicrophoneMeterFloorDb) / -kMicrophoneMeterFloorDb, 0.0f, 1.0f);
}

// The audio callback can run several times between frames. Keep the largest peak across all channels and callbacks.
inline void audio_meter_accumulate(std::atomic<float>& pending, const float* peaks, size_t channels) {
  float loudest = kAudioMeterSilenceDb;
  for (size_t i = 0; i < channels; ++i) {
    if (!std::isnan(peaks[i])) loudest = std::max(loudest, peaks[i]);
  }
  float previous = pending.load(std::memory_order_relaxed);
  while (loudest > previous && !pending.compare_exchange_weak(previous, loudest, std::memory_order_relaxed)) {}
}

inline float audio_meter_take_peak(std::atomic<float>& pending) {
  return pending.exchange(kAudioMeterSilenceDb, std::memory_order_relaxed);
}

inline bool audio_meter_wanted(bool expanded_visible, bool minimized, bool meter_view, bool closing) {
  return expanded_visible && !minimized && meter_view && !closing;
}

}  // namespace bx
