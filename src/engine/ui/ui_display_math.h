#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace bx::ui {

struct DisplayCardSlot {
  std::size_t index = 0;
  float x = 0, y = 0;
};

struct DisplayCardLayout {
  float width = 0, height = 0;
  std::vector<DisplayCardSlot> slots;
};

// The primary stays in the middle. Other displays fill its right and left, then centered rows below.
// A narrow pane stacks cards; the settings page scrolls vertically without dropping any display.
inline DisplayCardLayout display_card_layout(std::size_t count, std::size_t primary, float available) {
  DisplayCardLayout layout;
  if (count == 0 || available <= 0) return layout;
  constexpr float gap = 8, card_height = 52;
  const std::size_t columns = available >= 3 * 180 + 2 * gap ? 3 : 1;
  layout.width = std::min(220.0f, (available - (columns - 1) * gap) / columns);
  primary = primary < count ? primary : 0;
  layout.slots.push_back({primary, (available - layout.width) / 2, 0});
  std::vector<std::size_t> others;
  for (std::size_t i = 0; i < count; ++i) if (i != primary) others.push_back(i);
  std::size_t next = 0;
  if (columns == 3) {
    for (const int side : {1, -1}) {
      if (next == others.size()) break;
      layout.slots.push_back({others[next++], (available - layout.width) / 2 + side * (layout.width + gap), 0});
    }
  }
  std::size_t row = 1;
  while (next < others.size()) {
    const std::size_t used = std::min(columns, others.size() - next);
    const float start = (available - used * layout.width - (used - 1) * gap) / 2;
    for (std::size_t col = 0; col < used; ++col)
      layout.slots.push_back({others[next++], start + col * (layout.width + gap), row * (card_height + gap)});
    ++row;
  }
  layout.height = row * (card_height + gap) - gap;
  return layout;
}

}  // namespace bx::ui
