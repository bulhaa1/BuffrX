// The pad's place in the interface: which buttons move it, and how a held direction repeats. Header
// only and free of ImGui, so the mapping and the repeat rule are unit-tested without a controller.
#pragma once

#include <cstdint>

#include "../gamepad.h"

namespace bx::ui {

enum class PadNav { none, up, down, left, right, accept, back, space, previous_pane, next_pane, help };

// One button, one intent. Anything else is none: the pad does not try to be a pointer.
inline PadNav pad_nav_for(uint32_t button) {
  switch (button) {
    case pad_up: return PadNav::up;
    case pad_down: return PadNav::down;
    case pad_left: return PadNav::left;
    case pad_right: return PadNav::right;
    case pad_a: return PadNav::accept;
    case pad_b: return PadNav::back;
    case pad_x: return PadNav::space;
    case pad_lb: return PadNav::previous_pane;
    case pad_rb: return PadNav::next_pane;
    case pad_view: return PadNav::help;
    default: return PadNav::none;
  }
}

// A held direction moves at once, waits 120 ms, then moves every 40 ms.
inline bool pad_nav_repeat_due(int64_t now_ms, int64_t first_at_ms, int64_t last_ms) {
  return now_ms - first_at_ms >= 120 && now_ms - last_ms >= 40;
}

}  // namespace bx::ui
