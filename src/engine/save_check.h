#pragma once

#include <algorithm>
#include <cstdint>

namespace bx {

// A clip that cannot carry a picture: the 2026-11-06 regression saved six minutes of black at 37-44 kbps
// while healthy motion on the same setting (2560x1440, 120 fps) measured about 1,400 kbps. The floor is a
// two-hundredth of the peak the encoder was allowed (24,773 -> 123 kbps), never below 60, so a quiet scene
// at a low peak is not named while a clip that is almost certainly black is. Zero means unknown on either
// side, and unknown is never an alarm.
inline bool clip_may_be_black(uint64_t video_kbps, int peak_kbps) {
  if (video_kbps == 0 || peak_kbps <= 0) return false;
  const uint64_t floor = std::max<uint64_t>(60, static_cast<uint64_t>(peak_kbps) / 200);
  return video_kbps < floor;
}

}  // namespace bx
