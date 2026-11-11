// The Timeline preview is a window of its own, so it cannot be drawn over a game that runs in exclusive
// fullscreen: Windows gives that display to the game alone, and the game loses it the moment another
// window takes the foreground. Which games do this cannot be asked of Windows from outside - a borderless
// game fills its monitor too - so it is observed instead: open the preview, look at the game's own window a
// moment later, and say it once when that window has left the screen. The preview stays where it is (it is
// usable, the game is one keystroke away), and the game is put back when the preview ends. A game that has
// said so is not probed or announced again.
#pragma once

namespace bx {

// Long enough for `buffrx-ui` to create, place and activate its window. The game is looked at once.
constexpr unsigned kTimelineOverlayProbeMs = 1200;

// Whether the game is looked at after the preview opens. A game that already said it cannot be covered is
// not probed again: the person was told what happens and pressed the key anyway.
inline bool timeline_overlay_probe_wanted(bool game_in_front, bool explained) {
  return game_in_front && !explained;
}

// Whether the game is put back: the window is asked, not a memory of taking it away, so a person who
// minimized the game themselves is left alone.
inline bool timeline_overlay_restore_wanted(bool restore_armed, bool window_hidden) {
  return restore_armed && window_hidden;
}

}  // namespace bx
