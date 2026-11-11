// The panel's live timeline and its recent actions, decided without a window so tests/engine_tests.cpp can hold them:
// the strip's window onto the footage, its ruler and filmstrip grid, the chosen range, the live edge, and the history
// the library already is. Times are the Timeline session's clock (milliseconds since it began), as timeline.get reports.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ui_store.h"

namespace bx::ui {

struct LiveRange {
  int64_t start_ms = 0, end_ms = 0;
};

// A step a person reads (1, 2, 5, 10, 15, 30 s, then minutes, up to an hour): the first one at least `want_ms`.
inline int64_t nice_step_ms(int64_t want_ms) {
  static const int64_t steps[] = {1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000, 600000, 900000, 1800000, 3600000};
  for (const int64_t step : steps) {
    if (step >= want_ms) return step;
  }
  return 3600000;
}

// The ruler's spacing: labels at least `min_px` apart on a strip `width_px` wide that shows `span_ms`.
inline int64_t tick_step_ms(int64_t span_ms, float width_px, float min_px) {
  if (width_px <= 0.0f) return nice_step_ms(span_ms);
  return nice_step_ms(static_cast<int64_t>(std::ceil(static_cast<double>(span_ms) * min_px / width_px)));
}

// The filmstrip's slots: `tile_ms` wide on a grid anchored at the session's start, so a view that slides with the live edge
// asks only for the slot that came into view, never for a whole new set of pictures. Returns each slot's start.
inline std::vector<int64_t> strip_slots(int64_t view_start, int64_t view_end, int64_t tile_ms) {
  std::vector<int64_t> out;
  if (tile_ms <= 0 || view_end <= view_start) return out;
  for (int64_t k = std::max<int64_t>(0, view_start) / tile_ms; k * tile_ms < view_end; ++k) out.push_back(k * tile_ms);
  return out;
}

// The zoom steps the wheel and the buttons walk through.
inline const std::vector<int64_t>& live_spans() {
  static const std::vector<int64_t> spans = {15000, 30000, 60000, 120000, 300000, 600000, 1800000, 3600000, 7200000};
  return spans;
}

// One zoom step out (`direction` > 0) or in, never wider than the first span that shows all of `footage_ms`.
inline int64_t zoom_span(int64_t current, int direction, int64_t footage_ms) {
  const std::vector<int64_t>& spans = live_spans();
  size_t widest = 0;
  while (widest + 1 < spans.size() && spans[widest] < footage_ms) ++widest;
  size_t at = 0;
  while (at + 1 < spans.size() && spans[at] < current) ++at;
  if (direction > 0 && at < widest) ++at;
  else if (direction < 0 && at > 0) --at;
  return spans[std::min(at, widest)];
}

// The window onto the footage [lo, hi]: `span_ms` wide and ending at `end_ms`, kept inside the footage. Footage shorter than
// the span sits at the right, with the live edge, and the room before it is empty.
inline LiveRange view_window(int64_t end_ms, int64_t span_ms, int64_t lo, int64_t hi) {
  int64_t end = std::min(end_ms, hi);
  int64_t start = end - span_ms;
  if (start < lo) {
    start = hi - lo >= span_ms ? lo : hi - span_ms;
    end = start + span_ms;
  }
  return {start, end};
}

// The range inside the footage [lo, hi] and at least `min_ms` long (all of it when the footage is shorter). A range too
// short grows forward, then back from the end of the footage.
inline LiveRange clamp_range(int64_t start, int64_t end, int64_t lo, int64_t hi, int64_t min_ms) {
  if (hi - lo <= min_ms) return {lo, hi};
  if (end < start) std::swap(start, end);
  start = std::clamp(start, lo, hi);
  end = std::clamp(end, lo, hi);
  if (end - start < min_ms) {
    end = start + min_ms;
    if (end > hi) {
      end = hi;
      start = hi - min_ms;
    }
  }
  return {start, end};
}

// One edge dragged to `t`: it stops `min_ms` short of the other one and at the footage's ends.
inline LiveRange move_edge(LiveRange range, bool start_edge, int64_t t, int64_t lo, int64_t hi, int64_t min_ms) {
  if (start_edge) range.start_ms = std::clamp(t, lo, std::max(lo, range.end_ms - min_ms));
  else range.end_ms = std::clamp(t, std::min(hi, range.start_ms + min_ms), hi);
  return range;
}

// The whole range moved by `delta`, keeping its length, stopped at the footage's ends.
inline LiveRange shift_range(LiveRange range, int64_t delta, int64_t lo, int64_t hi) {
  const int64_t length = std::min(range.end_ms - range.start_ms, hi - lo);
  const int64_t start = std::clamp(range.start_ms + delta, lo, std::max(lo, hi - length));
  return {start, start + length};
}

// The newest moment of a session being written, on its clock: the wall clock since it started, held 1.5 s back
// (`started_at` is whole seconds and the muxer holds a little), never before what is already published.
inline int64_t live_edge_ms(int64_t now_unix_ms, int64_t started_at_s, int64_t published_end_ms) {
  return std::max(published_end_ms, now_unix_ms - started_at_s * 1000 - 1500);
}

// ---------------------------------------------------------------- recent actions

enum class ActionKind { clip, timeline_clip, screenshot, record_start, record_stop, edit, received };

struct RecentAction {
  ActionKind kind = ActionKind::clip;
  int64_t when = 0;  // unix seconds
  double seconds = 0;
  std::string file;  // "" for a recording still running
  bool clip = true;
};

inline bool starts_with(const std::string& value, const char* head) { return value.rfind(head, 0) == 0; }

// What saved a clip, from where the engine files it and the name it gives it.
inline ActionKind clip_action_kind(const UiMedia& item) {
  if (item.folder == "Edits") return ActionKind::edit;
  if (item.folder == "Shared with me") return ActionKind::received;
  if (item.folder == "Recordings" || starts_with(item.name, "Recording ")) return ActionKind::record_stop;
  if (starts_with(item.name, "BuffrX ")) return ActionKind::timeline_clip;  // timeline.save's own name
  return ActionKind::clip;
}

// The history is the library: every clip and screenshot as the save it was, a recording as its start (its end less its
// length) and its stop, and a recording still running (`recording_since`, unix seconds, 0 when none) as a start with no file
// yet. Newest first, at most `limit`.
inline std::vector<RecentAction> recent_actions(const std::vector<UiMedia>& clips, const std::vector<UiMedia>& shots,
                                                int64_t recording_since, size_t limit) {
  std::vector<RecentAction> out;
  out.reserve(clips.size() + shots.size() + 1);
  for (const UiMedia& item : clips) {
    const ActionKind kind = clip_action_kind(item);
    out.push_back({kind, item.mtime, item.duration, item.file, true});
    if (kind == ActionKind::record_stop && item.duration > 0) {
      out.push_back({ActionKind::record_start, item.mtime - static_cast<int64_t>(std::lround(item.duration)), item.duration, item.file, true});
    }
  }
  for (const UiMedia& item : shots) out.push_back({ActionKind::screenshot, item.mtime, 0, item.file, false});
  if (recording_since > 0) out.push_back({ActionKind::record_start, recording_since, 0, std::string(), true});
  // A start and its stop at the same second: the stop is the newer of the two.
  std::stable_sort(out.begin(), out.end(), [](const RecentAction& a, const RecentAction& b) {
    if (a.when != b.when) return a.when > b.when;
    return a.kind == ActionKind::record_stop && b.kind == ActionKind::record_start;
  });
  if (out.size() > limit) out.resize(limit);
  return out;
}

}  // namespace bx::ui
