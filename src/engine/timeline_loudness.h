// The Timeline bar's loud moments (docs/protocol.md, `timeline.loudness`).
//
// Each published chunk is decoded once for its sound: one byte per second of footage, the RMS level of the
// loudest audio track mapped from -60 dBFS (0) to 0 dBFS (255). The byte file lives beside the chunk's
// thumbnails under the same key, so retention that removes a chunk's pictures removes its levels too. A chunk's
// footage never changes after its writer let go, so a cache file is never stale.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bx {

// A call may read the sound of this many new chunks: eight seconds of AAC is milliseconds of work, and a two
// hour session fills in over a minute of polls without a single long command.
constexpr size_t kTimelineLoudnessPerCall = 8;
constexpr int kTimelineLoudnessBucketMs = 1000;
// Longest chunk the reader keeps levels for; a chunk is eight seconds, so this is a guard, not a limit.
constexpr size_t kTimelineLoudnessMaxBuckets = 600;
constexpr float kTimelineLoudnessFloorDb = -60.0f;

// One level byte from a linear RMS (0..1): silence and anything under -60 dBFS is 0, full scale is 255.
inline uint8_t timeline_loudness_byte(float rms) {
  if (!(rms > 0.0f)) return 0;
  const float db = 20.0f * std::log10(rms);
  if (db <= kTimelineLoudnessFloorDb) return 0;
  if (db >= 0.0f) return 255;
  return static_cast<uint8_t>(std::lround((db - kTimelineLoudnessFloorDb) / -kTimelineLoudnessFloorDb * 255.0f));
}

inline std::vector<uint8_t> timeline_loudness_bytes(const std::vector<float>& rms) {
  std::vector<uint8_t> out;
  out.reserve(std::min(rms.size(), kTimelineLoudnessMaxBuckets));
  for (size_t i = 0; i < rms.size() && i < kTimelineLoudnessMaxBuckets; ++i) out.push_back(timeline_loudness_byte(rms[i]));
  return out;
}

// Read the requested moment first; absent a position, retain the newest-first order.
inline std::vector<size_t> timeline_loudness_order(const std::vector<int64_t>& starts, int64_t at_ms) {
  std::vector<size_t> order;
  order.reserve(starts.size());
  for (size_t i = starts.size(); i-- > 0;) order.push_back(i);
  if (at_ms >= 0 && !starts.empty()) {
    int64_t anchor = *std::min_element(starts.begin(), starts.end());
    for (int64_t start : starts) if (start <= at_ms && start > anchor) anchor = start;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      const uint64_t da = starts[a] >= anchor ? static_cast<uint64_t>(starts[a] - anchor) : static_cast<uint64_t>(anchor - starts[a]);
      const uint64_t db = starts[b] >= anchor ? static_cast<uint64_t>(starts[b] - anchor) : static_cast<uint64_t>(anchor - starts[b]);
      return da < db;
    });
  }
  return order;
}

}  // namespace bx
