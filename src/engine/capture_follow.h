// Which source delivers the picture while the Timeline records. A rule of its own because the answer
// depends on three things the tick knows and nothing else: whether the Timeline is on, whether the buffer
// wants frames, and whether a captured game is in front. With the Timeline off, or the buffer off, the
// user's capture target is never overruled.
#pragma once

#include <cstdint>

namespace bx {

enum class CaptureTarget {
  leave,    // Timeline or buffer off: capture stays where the user put it
  game,     // a captured game is in front: the game hook or WGC delivers the best picture
  display   // the desktop, or a game that is not in front: the display capture does
};

// `game_in_front` counts the Timeline preview as in front of the game: the preview is excluded from
// capture, so recording the desktop while it covers a game would save the one picture nobody wants.
inline CaptureTarget capture_follow_target(bool timeline_on, bool buffer_on, bool game_captured,
                                           bool game_in_front) {
  if (!timeline_on || !buffer_on) return CaptureTarget::leave;
  return game_captured && game_in_front ? CaptureTarget::game : CaptureTarget::display;
}

// What a swap writes into the capture config. A swap back to the game sets the mode alone: the game's
// own `title:class:exe` id is still in the config from the enter, and a bare window title never hooks -
// writing one here left the ring recording black for as long as the game stayed in front.
struct CaptureWrite {
  const char* capture_mode;  // nullptr: write nothing
  const char* game_title;    // nullptr: leave the title alone, which is every swap
};

inline CaptureWrite capture_follow_write(CaptureTarget target) {
  switch (target) {
    case CaptureTarget::game: return {"game", nullptr};
    case CaptureTarget::display: return {"display", nullptr};
    default: return {nullptr, nullptr};
  }
}

}  // namespace bx
