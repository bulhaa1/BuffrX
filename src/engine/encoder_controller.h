// The dynamic encoder controller (plan 008): keeps the video encoder in real time by moving between levels, in both directions, without
// emptying the buffer (the swap itself is ObsHost::swap_encoder and the ring's epochs). Pure: no libobs, no clock of its own, so every rule is
// tested. It replaces the encoder governor (encoder_governor.h), whose steps rebuilt the pipeline, only went down, and were written to config.json.
//
// The levels are relative to what the person set: level 0 is their settings and the ceiling, and every level that follows must lower something
// (an effort step that changes nothing, a frame rate that is already 60, is not a level). The table is built once from the running settings.
//
//   effort    1-3   no look-ahead or multipass, a faster preset, the fastest preset (encoder_settings.cpp governor_overrides)
//   fps       /2 above 60 fps, /3 above 90: every kept picture evenly spaced (obs_encoder_set_frame_rate_divisor), never 119 or 100
//   fps       60 -> 30 only for x264 and Performance mode, last (a hardware encoder at 60 fps is not asked for 30)
//
// No resolution level: obs_encoder_set_scaled_size is unproven on texture encoders, and a clip is one picture size (decided 2026-11-14).
//
// What it reads, once per two-second window, from counters libobs already keeps: encoded over rendered pictures (the encoder falling behind),
// skipped pictures (the encoder is what lost them) and lagged pictures (the game saturates the GPU: effort cannot help, only the rate can).
// A dead encoder (encoder_watch.h's frozen verdict) is never this controller's: it belongs to the fallback ladder.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace buffrx {

enum class LevelKind { base, effort, fps };

struct EncoderLevel {
  LevelKind kind = LevelKind::base;
  int effort = 0;      // the governor effort step this level runs (0 = the person's own settings)
  int divisor = 1;     // frame-rate divisor
};

// What the table is built from. `effort_changes[n]` says whether effort step n (1-3) differs from step n-1 for the running encoder: a step
// that would change nothing (already the fastest preset, no look-ahead to turn off) is skipped.
struct LevelFacts {
  bool effort_changes[4] = {false, false, false, false};
  int fps = 60;               // the canvas rate the person chose, or Performance mode's
  bool divisor_ok = true;     // the vendor's encoder took a divisor when it was tried (a vendor that did not has no fps levels)
  bool half_rate_allowed = false;  // 60 -> 30: x264 and Performance mode only
};

inline std::vector<EncoderLevel> controller_levels(const LevelFacts& facts) {
  std::vector<EncoderLevel> levels(1);
  EncoderLevel at;
  for (int step = 1; step <= 3; ++step) {
    if (!facts.effort_changes[step]) continue;
    at.kind = LevelKind::effort;
    at.effort = step;
    levels.push_back(at);
  }
  if (facts.divisor_ok && facts.fps > 60) {
    at.kind = LevelKind::fps;
    at.divisor = 2;
    levels.push_back(at);
    if (facts.fps > 90) {
      at.divisor = 3;
      levels.push_back(at);
    }
  }
  if (facts.divisor_ok && facts.fps == 60 && facts.half_rate_allowed) {
    at.kind = LevelKind::fps;
    at.divisor = 2;
    levels.push_back(at);
  }
  // Append rate-only branches after the original ladder: old capacity indices keep their meaning.
  // Each branch retains its effort and can return directly to that effort on recovery.
  const auto original = levels;
  for (const auto& effort : original) {
    if (effort.kind == LevelKind::fps || effort.effort == at.effort) continue;
    for (const auto& rate : original) {
      if (rate.kind == LevelKind::fps) levels.push_back({LevelKind::fps, effort.effort, rate.divisor});
    }
  }
  return levels;
}

// How a level is worded in the panel's status line and the log: "effort -1", "120 -> 60 fps".
inline std::string level_label(const EncoderLevel& level, int fps) {
  switch (level.kind) {
    case LevelKind::base: return "full speed";
    case LevelKind::effort: return "effort -" + std::to_string(level.effort);
    case LevelKind::fps: return std::to_string(fps) + " -> " + std::to_string(std::max(1, fps / std::max(1, level.divisor))) + " fps";
  }
  return std::string();
}

// ---------------------------------------------------------------- the rules

constexpr uint32_t kControllerWindowMs = 2000;
constexpr uint64_t kControllerMinFrames = 60;       // fewer pictures in a window than this is too little to judge
constexpr double kStruggleFast = 0.85;              // under this for kDownFastMs: step down
constexpr uint32_t kDownFastMs = 10000;
constexpr double kStruggleSlow = 0.95;              // under this for kDownSlowMs: step down
constexpr uint32_t kDownSlowMs = 30000;
constexpr uint32_t kDownFpsMs = 30000;              // a frame-rate level: 30 s of struggle (under kStruggleSlow)
constexpr double kHealthy = 0.99;
constexpr uint32_t kUpEffortMs = 3 * 60000;         // at or over kHealthy this long: one level back up
constexpr uint32_t kUpFpsMs = 5 * 60000;
constexpr uint32_t kMoveSpacingMs = 30000;          // at least this long between moves
constexpr uint32_t kFpsSpacingMs = 5 * 60000;       // and one frame-rate change per this long
constexpr uint32_t kFailedUpMs = 2 * 60000;         // down again this soon after an up: that try up failed
constexpr uint32_t kMaxWaitMultiple = 8;            // 3 -> 6 -> 12 -> 24 minutes
constexpr double kLagShare = 0.05;                  // lagged pictures over all the video thread's pictures: the game is saturating the GPU

enum class Move { none, down, up, floor_reached };

struct ControllerWindow {
  uint64_t rendered = 0;  // pictures the canvas made in the window (already divided by the divisor in force)
  uint64_t encoded = 0;   // pictures the encoder finished
  uint64_t skipped = 0;   // pictures the video output skipped (the encoder lagged them)
  uint64_t lagged = 0;    // pictures the video thread itself missed (the GPU is saturated)
};

// What stops the controller moving right now. Every one resets the run of windows it was counting: a picture lost during a save or a hook
// attach is not the encoder's to answer for.
struct ControllerHolds {
  bool frozen = false;            // a dead encoder: the fallback ladder's
  bool manual_recording = false;  // one file that never splits
  bool save_busy = false;
  bool device_hold = false;       // the graphics driver just faulted (driver_fault.h)
  bool rebuilding = false;
};

struct ControllerState {
  int level = 0;
  // The windows being counted: how long, in a row, the encoder was under each bar or at/over the healthy one.
  uint32_t window_start_ms = 0;
  bool primed = false;
  uint64_t base_rendered = 0, base_encoded = 0, base_skipped = 0, base_lagged = 0;
  uint32_t under_fast_ms = 0;
  uint32_t under_slow_ms = 0;
  uint32_t healthy_ms = 0;
  bool lag_dominant = false;       // the last window's loss was the game's, not the encoder's
  double ratio = 1.0;
  // Moves and their spacing.
  uint32_t last_move_ms = 0;
  bool moved_ever = false;
  uint32_t last_fps_move_ms = 0;
  bool fps_moved_ever = false;
  uint32_t grace_until_ms = 0;
  bool in_grace = false;
  // The wait before a level may be tried again, in multiples of the base wait, per level index: a try up that fails (down again within
  // two minutes) doubles it for the level it came from.
  std::vector<uint32_t> wait_multiple;
  // Successful down moves, in order. A remembered level has no history, so it
  // recovers rate first and then effort using the stable level table.
  std::vector<int> return_levels;
  uint32_t last_up_ms = 0;
  int last_up_from = -1;           // the level the last up move left
  bool last_up_open = false;       // that try up has not yet held for kFailedUpMs
  bool floor_told = false;
};

// Starts (or restarts) a run: at `level` (0, or the one a memory remembers) with a grace window from `now_ms`. Keeps nothing else.
inline void controller_start(ControllerState& state, size_t level_count, int level, uint32_t now_ms, uint32_t grace_ms) {
  const bool floor_told = state.floor_told;  // once per session, not once per rebuild
  state = ControllerState{};
  state.floor_told = floor_told;
  state.level = std::clamp(level, 0, std::max(0, static_cast<int>(level_count) - 1));
  state.wait_multiple.assign(std::max<size_t>(1, level_count), 1);
  state.grace_until_ms = now_ms + grace_ms;
  state.in_grace = grace_ms > 0;
}

// A capture swap, a hook attach, a wake, a display change, a buffer start, a swap of the encoder itself: nothing moves for `grace_ms`, and
// what was counted is forgotten.
inline void controller_grace(ControllerState& state, uint32_t now_ms, uint32_t grace_ms) {
  const uint32_t until = now_ms + grace_ms;
  // TickCount wraps after 49 days. Compare relative deadlines and replace expired holds.
  if (!state.in_grace || static_cast<int32_t>(now_ms - state.grace_until_ms) >= 0 ||
      static_cast<int32_t>(until - state.grace_until_ms) > 0) state.grace_until_ms = until;
  state.in_grace = true;
  state.primed = false;
  state.under_fast_ms = state.under_slow_ms = state.healthy_ms = 0;
}

inline bool controller_in_grace(const ControllerState& state, uint32_t now_ms) {
  return state.in_grace && static_cast<int32_t>(now_ms - state.grace_until_ms) < 0;
}

// The floor card is once per session; a new run keeps it told.
struct ControllerDecision {
  Move move = Move::none;
  int to = 0;
  std::string reason;
};

namespace controller_detail {

inline bool is_fps_level(const std::vector<EncoderLevel>& levels, int level) {
  return level >= 0 && level < static_cast<int>(levels.size()) && levels[static_cast<size_t>(level)].kind == LevelKind::fps;
}

inline uint32_t elapsed(uint32_t now_ms, uint32_t then_ms) { return now_ms - then_ms; }

}  // namespace controller_detail

// One sample of the counters (cumulative, the caller's own clock). Returns the move to make, if any; the caller makes it (the
// swap) and then calls controller_moved. Windows are two seconds; between them nothing is decided.
inline ControllerDecision controller_feed(ControllerState& state, const std::vector<EncoderLevel>& levels, uint32_t now_ms,
                                          const ControllerWindow& totals, const ControllerHolds& holds) {
  ControllerDecision decision;
  if (levels.size() < 2) return decision;
  if (holds.frozen || holds.manual_recording || holds.save_busy || holds.device_hold || holds.rebuilding) {
    state.primed = false;
    state.under_fast_ms = state.under_slow_ms = state.healthy_ms = 0;
    return decision;
  }
  if (state.in_grace && !controller_in_grace(state, now_ms)) state.in_grace = false;
  if (controller_in_grace(state, now_ms)) {
    state.primed = false;
    state.under_fast_ms = state.under_slow_ms = state.healthy_ms = 0;
    return decision;
  }
  if (!state.primed || totals.rendered < state.base_rendered || totals.encoded < state.base_encoded) {
    state.primed = true;
    state.window_start_ms = now_ms;
    state.base_rendered = totals.rendered;
    state.base_encoded = totals.encoded;
    state.base_skipped = totals.skipped;
    state.base_lagged = totals.lagged;
    return decision;
  }
  const uint32_t span = controller_detail::elapsed(now_ms, state.window_start_ms);
  if (span < kControllerWindowMs) return decision;
  const uint64_t rendered = totals.rendered - state.base_rendered;
  const uint64_t encoded = totals.encoded - state.base_encoded;
  const uint64_t skipped = totals.skipped >= state.base_skipped ? totals.skipped - state.base_skipped : 0;
  const uint64_t lagged = totals.lagged >= state.base_lagged ? totals.lagged - state.base_lagged : 0;
  state.window_start_ms = now_ms;
  state.base_rendered = totals.rendered;
  state.base_encoded = totals.encoded;
  state.base_skipped = totals.skipped;
  state.base_lagged = totals.lagged;
  if (rendered < kControllerMinFrames) return decision;  // too little to judge: the runs carry on as they were

  state.ratio = static_cast<double>(encoded) / static_cast<double>(rendered);
  const uint64_t missing = rendered > encoded ? rendered - encoded : 0;
  // The loss is the game's when more pictures were missed by the video thread than the encoder dropped: lowering the effort of an encoder that was
  // not the problem would only cost quality.
  const double lag_share = static_cast<double>(lagged) / static_cast<double>(rendered + lagged);
  state.lag_dominant = lag_share >= kLagShare && lagged > std::max<uint64_t>(missing, skipped);
  if (state.ratio < kStruggleFast) state.under_fast_ms += span; else state.under_fast_ms = 0;
  if (state.ratio < kStruggleSlow) state.under_slow_ms += span; else state.under_slow_ms = 0;
  // A game drops the odd picture on its own; only a lag share that dominates (kLagShare) stops the climb back, or a busy game would pin a level.
  if (state.ratio >= kHealthy && lag_share < kLagShare) state.healthy_ms += span; else state.healthy_ms = 0;

  const auto& current = levels[static_cast<size_t>(state.level)];
  auto find_level = [&](int effort, int divisor) {
    for (size_t i = 0; i < levels.size(); ++i)
      if (levels[i].effort == effort && levels[i].divisor == divisor) return static_cast<int>(i);
    return -1;
  };
  int next = -1;
  int next_effort = -1;
  for (const auto& level : levels) {
    if (level.divisor == 1 && level.effort > current.effort) {
      next_effort = find_level(level.effort, current.divisor);
      break;
    }
  }
  if (current.kind == LevelKind::fps && !state.lag_dominant && next_effort >= 0) {
    next = next_effort;
  } else if (current.kind == LevelKind::fps) {
    next = find_level(current.effort, current.divisor + 1);
  } else if (state.lag_dominant) {
    next = find_level(current.effort, 2);
  } else {
    for (size_t i = static_cast<size_t>(state.level + 1); i < levels.size(); ++i) {
      if (levels[i].kind == LevelKind::effort || (levels[i].kind == LevelKind::fps && levels[i].effort == current.effort)) {
        next = static_cast<int>(i); break;
      }
    }
  }
  // A try up that has held for two minutes is a success: its level's wait is forgotten.
  if (state.last_up_open && controller_detail::elapsed(now_ms, state.last_up_ms) >= kFailedUpMs) {
    if (state.last_up_from >= 0 && static_cast<size_t>(state.last_up_from) < state.wait_multiple.size()) {
      state.wait_multiple[static_cast<size_t>(state.last_up_from)] = 1;
    }
    state.last_up_open = false;
  }
  const bool spaced = !state.moved_ever || controller_detail::elapsed(now_ms, state.last_move_ms) >= kMoveSpacingMs;
  const auto fps_spaced = [&]() { return !state.fps_moved_ever || controller_detail::elapsed(now_ms, state.last_fps_move_ms) >= kFpsSpacingMs; };

  // Down. The next level is the one asked for: an effort level needs the fast or the slow bar, a frame-rate level thirty seconds under the slow
  // bar and room in the one-change-per-five-minutes rule. A lagging game blocks effort levels (they
  // lower the encoder's cost, which is not what is short), never the frame rate.
  if (next >= 0) {
    const EncoderLevel& target = levels[static_cast<size_t>(next)];
    const bool fps_like = target.divisor != current.divisor;
    const bool struggling = fps_like ? state.under_slow_ms >= kDownFpsMs : (state.under_fast_ms >= kDownFastMs || state.under_slow_ms >= kDownSlowMs);
    const bool blocked_by_lag = state.lag_dominant && !fps_like;
    if (struggling && spaced && !blocked_by_lag && (!fps_like || fps_spaced())) {
      decision.move = Move::down;
      decision.to = next;
      decision.reason = std::to_string(static_cast<int>(state.ratio * 100.0 + 0.5)) + "% of the pictures encoded for " +
                        std::to_string((state.under_fast_ms >= kDownFastMs ? state.under_fast_ms : state.under_slow_ms) / 1000) + " s" +
                        (state.lag_dominant ? " (the game was lagging too)" : "");
      return decision;
    }
  } else if (next_effort < 0 && state.under_slow_ms >= kDownSlowMs && !state.floor_told) {
    // Out of levels and still short: said once.
    state.floor_told = true;
    decision.move = Move::floor_reached;
    decision.to = state.level;
    decision.reason = std::to_string(static_cast<int>(state.ratio * 100.0 + 0.5)) + "% of the pictures encoded at the lowest level";
    return decision;
  }
  // Up. Back one level, the last lowered first, when the encoder has been healthy for the wait the level it would return to has earned.
  if (state.level > 0 && spaced) {
    const EncoderLevel& from = levels[static_cast<size_t>(state.level)];
    const int fallback = from.kind == LevelKind::fps ? find_level(from.effort, from.divisor == 2 ? 1 : from.divisor - 1) : state.level - 1;
    const int previous = state.return_levels.empty() ? fallback : state.return_levels.back();
    const bool fps_like = previous >= 0 && levels[static_cast<size_t>(previous)].divisor != from.divisor;
    const uint32_t base_wait = fps_like ? kUpFpsMs : kUpEffortMs;
    const uint32_t multiple = state.level < static_cast<int>(state.wait_multiple.size()) ? state.wait_multiple[static_cast<size_t>(state.level)] : 1;
    // A frame-rate level coming back is a frame-rate change like the one that lowered it.
    if (previous >= 0 && state.healthy_ms >= base_wait * multiple && (!fps_like || fps_spaced())) {
      decision.move = Move::up;
      decision.to = previous;
      decision.reason = "healthy for " + std::to_string(state.healthy_ms / 1000) + " s";
      return decision;
    }
  }
  return decision;
}

// The caller made the move (the swap went through): the controller learns the new level and starts counting afresh. A move down within two
// minutes of a move up undoes that try, and the level the try came from waits twice as long next time.
inline void controller_moved(ControllerState& state, const std::vector<EncoderLevel>& levels, int to, uint32_t now_ms, uint32_t grace_ms) {
  const int from = state.level;
  const auto& old_level = levels[static_cast<size_t>(from)];
  const auto& new_level = levels[static_cast<size_t>(to)];
  const bool went_down = new_level.effort > old_level.effort || new_level.divisor > old_level.divisor;
  if (went_down && state.last_up_open && controller_detail::elapsed(now_ms, state.last_up_ms) < kFailedUpMs && state.last_up_from >= 0) {
    uint32_t& multiple = state.wait_multiple[static_cast<size_t>(std::min<int>(state.last_up_from, static_cast<int>(state.wait_multiple.size()) - 1))];
    multiple = std::min<uint32_t>(kMaxWaitMultiple, multiple * 2);
    state.last_up_open = false;
  }
  if (went_down) {
    state.return_levels.push_back(from);
  } else {
    if (!state.return_levels.empty()) state.return_levels.pop_back();
    state.last_up_ms = now_ms;
    state.last_up_from = from;
    state.last_up_open = true;
  }
  state.level = std::clamp(to, 0, std::max(0, static_cast<int>(levels.size()) - 1));
  state.last_move_ms = now_ms;
  state.moved_ever = true;
  if (old_level.divisor != new_level.divisor) {
    state.last_fps_move_ms = now_ms;
    state.fps_moved_ever = true;
  }
  controller_grace(state, now_ms, grace_ms);
}

// The swap did not happen: counting carries on (the move may be asked for again after its spacing), but a failed swap is itself a move's worth
// of spacing so the same swap is not retried every window.
inline void controller_move_failed(ControllerState& state, uint32_t now_ms) {
  state.last_move_ms = now_ms;
  state.moved_ever = true;
  state.under_fast_ms = state.under_slow_ms = state.healthy_ms = 0;
}

}  // namespace buffrx
