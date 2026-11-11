// The Timeline writer's state (docs/timeline-blueprint.md). One pure rule decides whether it records:
// the tick builds the facts it can see and asks timeline_next_state, so the precedence is written once
// and can be checked without libobs, a game or a disk.
//
// The order matters, highest first: Timeline off, the buffer off, a fallback recording occupying its output, a settings rebuild,
// low disk, then no game. While a game runs the desktop and any window are footage too;
// capture_follow.h decides which source delivers it. With "everywhere" chosen, no game is no reason to wait: the desktop
// is the footage, and only a long spell without keyboard or mouse (idle) pauses it.
#pragma once

#include <cstdint>
#include <string>

namespace bx {

enum class TimelineState {
  Off,                // the user's Timeline window is 0 minutes
  Stopped,            // on, but the RAM buffer is not running: Timeline writes only beside it
  Writing,            // segments are being written, whatever is in front
  PausedManual,       // a separate-encoder fallback recording owns the output
  PausedRebuild,      // the pipeline is being rebuilt: the encoders are about to change
  PausedDisk,         // free space is below the guard
  PausedNoGame,       // no detected game is running (games-only Timeline)
  PausedIdle          // everywhere Timeline, no game, and nobody at the keyboard or mouse for the chosen time
};

struct TimelineFacts {
  bool enabled = false;
  bool buffer_on = false;
  bool manual_owns_output = false;
  bool rebuilding = false;
  bool low_disk = false;
  bool game_running = false;
  // "Everywhere": the desktop is footage when no game runs. `idle` is only ever set for that desktop footage.
  bool everywhere = false;
  bool idle = false;
  // The captured game's executable, when one is captured: the session's markers say which game the footage
  // belongs to. The separate game_running flag is the recording gate.
  std::string game_exe;
};

inline TimelineState timeline_next_state(const TimelineFacts& facts) {
  if (!facts.enabled) return TimelineState::Off;
  if (!facts.buffer_on) return TimelineState::Stopped;
  if (facts.manual_owns_output) return TimelineState::PausedManual;
  if (facts.rebuilding) return TimelineState::PausedRebuild;
  if (facts.low_disk) return TimelineState::PausedDisk;
  if (!facts.game_running) {
    if (!facts.everywhere) return TimelineState::PausedNoGame;
    if (facts.idle) return TimelineState::PausedIdle;
  }
  return TimelineState::Writing;
}

inline bool timeline_writes(TimelineState state) { return state == TimelineState::Writing; }

// The wall time between two ticks, in milliseconds. Unsigned arithmetic, so the tick clock's wrap every
// 49.7 days gives the interval that really passed rather than a number near four billion.
inline uint32_t timeline_gap_ms(uint32_t last_write_ms, uint32_t now_ms) {
  return static_cast<uint32_t>(now_ms - last_write_ms);
}

// How long a Timeline session left open while nothing is written is kept: long enough that a settings
// change or a manual recording does not split the session, short enough that footage nobody is coming back
// for is not held in an index. A change of game does not close it: that is a marker.
constexpr uint32_t kTimelineSessionPauseMs = 60000;

inline bool timeline_session_should_close(uint32_t paused_ms) { return paused_ms >= kTimelineSessionPauseMs; }

// The label a resumed writer's gap marker carries. Only a pause spent waiting for a game left the game
// stopped; a rebuild, a low disk or a manual recording paused the writer while the game ran, and the hole
// then reads NO FOOTAGE. An empty label means no marker is written at all. A pending sleep/wake marker
// takes the place of it, because that marker already says what the hole was.
constexpr const char* kTimelineGamePausedLabel = "Game paused";
constexpr const char* kTimelineIdlePausedLabel = "Idle";

inline const char* timeline_gap_label(uint32_t gap_ms, uint32_t threshold_ms, bool wake_pending, bool paused_no_game,
                                      bool paused_idle = false) {
  if (wake_pending || gap_ms <= threshold_ms) return "";
  if (paused_no_game) return kTimelineGamePausedLabel;
  return paused_idle ? kTimelineIdlePausedLabel : "";
}

// Whether nobody has used the keyboard or mouse for `minutes`. Unsigned arithmetic like every tick comparison here,
// so the tick clock's wrap gives the time that really passed. 0 minutes is off. A controller does not count as input
// to Windows, which is why this is only ever asked of desktop footage and never while a game runs.
inline bool timeline_idle_due(uint32_t last_input_ms, uint32_t now_ms, int minutes) {
  return minutes > 0 && static_cast<uint32_t>(now_ms - last_input_ms) >= static_cast<uint32_t>(minutes) * 60000u;
}

// Test-only (BUFFRX_TEST_TIMELINE_PAUSE_MS): true through the pause half of each period, so a probe can drop the
// writer and bring it back on a timer and count what one pause costs in handles. 0 is off.
inline bool timeline_test_pause_due(uint32_t now_ms, int64_t pause_ms) {
  return pause_ms > 0 && (static_cast<int64_t>(now_ms) / pause_ms) % 2 == 0;
}

// A session spans a game, a look at the desktop and the next game: a change of game is a marker in it, not
// a new session. Case-insensitive, and either side empty (the session has no game yet, or the desktop has
// none) is not a change - the label is only worth writing when two real games differ.
inline bool timeline_game_changed(const std::string& tracked_exe, const std::string& game_exe) {
  if (tracked_exe.empty() || game_exe.empty()) return false;
  if (tracked_exe.size() != game_exe.size()) return true;
  for (size_t i = 0; i < tracked_exe.size(); ++i) {
    const unsigned char a = static_cast<unsigned char>(tracked_exe[i]);
    const unsigned char b = static_cast<unsigned char>(game_exe[i]);
    const char lower_a = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : static_cast<char>(a);
    const char lower_b = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : static_cast<char>(b);
    if (lower_a != lower_b) return true;
  }
  return false;
}

// What Alt+4 (the action "view_clip") does: a preview that is open closes; otherwise the preview opens,
// on the newest session when there is one and on its empty state when there is not.
enum class TimelinePress { close_preview, open_preview };

inline TimelinePress timeline_press_action(bool preview_open) {
  return preview_open ? TimelinePress::close_preview : TimelinePress::open_preview;
}

// The id the protocol and the interface use.
inline const char* timeline_state_id(TimelineState state) {
  switch (state) {
    case TimelineState::Off: return "off";
    case TimelineState::Stopped: return "stopped";
    case TimelineState::Writing: return "writing";
    case TimelineState::PausedManual: return "paused_manual";
    case TimelineState::PausedRebuild: return "paused_rebuild";
    case TimelineState::PausedDisk: return "paused_disk";
    case TimelineState::PausedNoGame: return "paused_no_game";
    case TimelineState::PausedIdle: return "paused_idle";
  }
  return "off";
}

}  // namespace bx
