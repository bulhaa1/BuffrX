// The quick panel's size, decided without a window so every scale and work area can be checked. Design units are the
// interface's own (kUiScale and the monitor's DPI turn them into pixels); the limits are the size it opens at and a
// small growth either side, and never past the work area, so it cannot grow into a full-screen window.
#pragma once

#include <algorithm>
#include <cmath>

namespace bx::ui {

// Resize messages can arrive just before a refresh. Leave a little room for their timing jitter while
// coalescing faster mouse events; an unknown refresh keeps the previous 8 ms bound.
inline int resize_frame_interval_us(int refresh_hz) {
  return refresh_hz >= 30 && refresh_hz <= 1000 ? 850000 / refresh_hz : 8000;
}

constexpr float kQuickWidth = 368, kQuickHeight = 580;
// The width floor keeps a save row's three buttons (edit, share, show in folder), the Quick clip row's key caps and the whole hint
// bar on screen. The height floor is what the fixed panes (buffer, run, mode, in their tight spacing) and two saves need; the
// default is already small, so the floor is close to it.
constexpr float kQuickMinWidth = 368, kQuickMinHeight = 540;
constexpr float kQuickMaxWidth = 400, kQuickMaxHeight = 620;

struct QuickSize {
  long width = 0, height = 0;  // pixels
};

struct QuickLimits {
  long min_width = 0, min_height = 0, max_width = 0, max_height = 0;  // pixels
};

// The limits in pixels at `scale`. A work area smaller than the floor is the floor (the panel scrolls), and the maximum is never
// below the minimum or past the work area.
inline QuickLimits quick_limits(float scale, long work_width, long work_height) {
  QuickLimits limits;
  limits.min_width = std::min(std::lround(kQuickMinWidth * scale), work_width);
  limits.min_height = std::min(std::lround(kQuickMinHeight * scale), work_height);
  limits.max_width = std::clamp(std::lround(kQuickMaxWidth * scale), limits.min_width, work_width);
  limits.max_height = std::clamp(std::lround(kQuickMaxHeight * scale), limits.min_height, work_height);
  return limits;
}

inline QuickSize quick_clamp(long width, long height, const QuickLimits& limits) {
  return QuickSize{std::clamp(width, limits.min_width, limits.max_width), std::clamp(height, limits.min_height, limits.max_height)};
}

// A size kept in design units (so a move to another monitor keeps it) as pixels at `scale`, inside the limits.
inline QuickSize quick_size_for(float design_width, float design_height, float scale, long work_width, long work_height) {
  return quick_clamp(std::lround(design_width * scale), std::lround(design_height * scale), quick_limits(scale, work_width, work_height));
}

// A window's pixels back to design units, for the file: whole units, so a repeated save does not drift.
inline float quick_design(long pixels, float scale) { return scale > 0 ? std::round(static_cast<float>(pixels) / scale) : 0.0f; }

// The main panel opens at this design size; its own floor and ceiling hold the resize, and the work area always wins.
constexpr float kPanelWidth = 1035, kPanelHeight = 840;
// The floor is the smallest width that keeps the deck and a usable library side by side; it does not
// follow the default down, or the panes would have to break.
constexpr float kPanelMinWidth = 1035, kPanelMinHeight = 840;
// The expanded panel's height is locked (max = min): the deck then has no spare height to leave a gap; the editor can grow taller.
constexpr float kPanelMaxWidth = 1316, kPanelMaxHeight = 840, kEditorMaxHeight = 1200;
constexpr float kMixerTrackHeight = 96;

// Keep the complete panel in the monitor's work area, even when the chosen scale cannot fit there.
inline float panel_scale_to_fit(float requested, long work_width, long work_height) {
  if (work_width <= 0 || work_height <= 0) return requested;
  return std::min({requested, static_cast<float>(work_width) / kPanelMinWidth,
                   static_cast<float>(work_height) / kPanelMinHeight});
}

struct PanelSize {
  long width = 0, height = 0;  // pixels
};

struct PanelLimits {
  long min_width = 0, min_height = 0, max_width = 0, max_height = 0;  // pixels
};

// Leave the parent window's resize border reachable around the embedded editor child.
inline long editor_resize_inset(float scale, long width, long height) {
  const long border = std::max(1L, std::lround(6.0f * scale));
  return std::min(border, std::max(0L, std::min(width, height) / 2));
}

// The panel's limits in pixels at `scale`. A work area smaller than the floor is both the floor and the ceiling:
// the panel fills it and scrolls inside, rather than growing off the screen.
inline PanelLimits panel_limits(float scale, long work_width, long work_height, bool editor = false) {
  PanelLimits limits;
  limits.min_width = std::min(std::lround(kPanelMinWidth * scale), work_width);
  limits.min_height = std::min(std::lround(kPanelMinHeight * scale), work_height);
  limits.max_width = std::clamp(std::lround(kPanelMaxWidth * scale), limits.min_width, work_width);
  limits.max_height = std::clamp(std::lround((editor ? kEditorMaxHeight : kPanelMaxHeight) * scale), limits.min_height, work_height);
  return limits;
}

inline PanelSize panel_clamp(long width, long height, const PanelLimits& limits) {
  return PanelSize{std::clamp(width, limits.min_width, limits.max_width), std::clamp(height, limits.min_height, limits.max_height)};
}

// A size kept in design units as pixels at `scale`, inside the panel's limits.
inline PanelSize panel_size_for(float design_width, float design_height, float scale, long work_width, long work_height) {
  return panel_clamp(std::lround(design_width * scale), std::lround(design_height * scale),
                     panel_limits(scale, work_width, work_height));
}

// The clip grid: up to four columns at `min_card`, then the widest whole-pixel card that leaves `gap`
// between them. A width under one card is one card of the whole width, so the grid never overflows sideways.
struct GridSpec {
  int columns = 1;
  float width = 0;
};

inline GridSpec card_grid(float width, float gap, float min_card) {
  const float space = std::max(0.0f, gap), floor = std::max(1.0f, min_card);
  const int columns = std::clamp(static_cast<int>((width + space) / (floor + space)), 1, 4);
  return GridSpec{columns, std::floor((width - space * (columns - 1)) / columns)};
}

}  // namespace bx::ui
