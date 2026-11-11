// The mark that says a manual recording is running: a small red dot in the corner of the screen.
//
// It is a window of the engine's own, not a scene item: the old badge was drawn into the scene, so it
// was burned into every recording it marked. This one is on the desktop, over the game, and never in
// the clip - Windows leaves it out of every capture, the display one included. It appears for manual
// records only - the rolling buffer is not a record - and it never takes focus.
//
// The mark stays on screen over a fullscreen game, borderless and windowed alike (decided with the user, 2026-11-05):
// a topmost window can cost the game its independent flip, and the person chose the mark. It is the bare HUD - a red dot
// and "REC 0:42" in the interface font - and it never takes focus.
#pragma once

#include <windows.h>

#include <cstdint>

namespace bx {

// The dot's alpha at a distance from the middle: solid in the core, gone at the rim, soft in
// between. Pure, so the shape of the mark can be tested without a screen.
double dot_falloff(double distance, double core, double radius);

// What the dot marks: a manual recording, or a Timeline clip whose range is still open. Timeline writing
// by itself is not a recording - nothing has been asked to be kept yet - so it shows no dot.
bool recording_dot_visible(bool manual_active);

class RecMark {
public:
  ~RecMark();

  // Creates the hidden mark window on the calling thread, which must run a message loop (the
  // engine's main thread).
  bool create();
  void destroy();

  // Shows or hides the mark. Called from the engine's tick, on the main thread.
  void set_recording(bool recording);

private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
  void tick();
  void paint();
  void place();

  HWND window_ = nullptr;
  bool recording_ = false;
  bool visible_ = false;
  uint32_t pulse_at_ = 0;
  uint32_t started_at_ = 0;  // the recording clock the HUD shows
  uint32_t fullscreen_checked_ms_ = 0;
};

}  // namespace bx
