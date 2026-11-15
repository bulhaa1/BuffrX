// "Games only" (Settings > Source), decided without libobs so it can be tested.
//
// The buffer is wanted, but only while a game is on screen: nothing is captured of the desktop. The engine
// pauses the buffer a few seconds after the game is gone and starts it again when the next game fills the
// screen. A paused buffer is an idle engine (sources released, canvas to 5 fps), so waiting costs nothing.

#ifndef BUFFRX_GAME_ONLY_H
#define BUFFRX_GAME_ONLY_H

#include <cstdint>

namespace buffrx {

// How long no game has to be there before the buffer pauses. Long enough that a loading screen, a launcher
// or an alt-tab that briefly hides the game does not drop the footage; short enough that the desktop is barely
// in the ring when the game is closed.
constexpr uint32_t kGameOnlyPauseMs = 5000;

enum class GameOnlyAction { none, pause, resume };

struct GameOnlyView {
  bool enabled = false;         // the setting is on and game detection is on
  bool buffer_wanted = false;   // the ring is running or waiting for its first frame
  bool waiting = false;         // this policy paused the buffer and has not resumed it
  bool game_here = false;       // a game is being captured, or one fills the screen now
  bool recording_busy = false;  // a manual recording runs: nothing here ends it
  uint32_t no_game_ms = 0;      // how long game_here has been false (0 while it is true)
};

inline GameOnlyAction game_only_decide(const GameOnlyView& view) {
  // Turned off while waiting: the buffer the person asked for comes back at once.
  if (view.waiting && !view.enabled) return GameOnlyAction::resume;
  if (!view.enabled) return GameOnlyAction::none;
  if (view.waiting) return view.game_here ? GameOnlyAction::resume : GameOnlyAction::none;
  // The buffer is off by the person's own hand: that is not this policy's to change.
  if (!view.buffer_wanted) return GameOnlyAction::none;
  if (view.game_here || view.recording_busy) return GameOnlyAction::none;
  return view.no_game_ms >= kGameOnlyPauseMs ? GameOnlyAction::pause : GameOnlyAction::none;
}

}  // namespace buffrx

#endif
