// The keys a hotkey may use with no modifier, and what each one costs. A lone hotkey fires in every
// app, so the rule is the same on every platform and lives here once: the Windows and Linux platform
// layers both apply it, and the editor shows the same warning whichever one is running.
#pragma once

#include <windows.h>

namespace bx {

// What a lone key costs, for the line under it in the recorder: `none` is a key that may not stand alone.
enum class LoneKeyKind { none, function, idle, insert, editing, keypad, character };

inline LoneKeyKind lone_key_kind(unsigned virtual_key) {
  if (virtual_key >= VK_F1 && virtual_key <= VK_F24) return LoneKeyKind::function;
  if (virtual_key == VK_PAUSE || virtual_key == VK_SCROLL) return LoneKeyKind::idle;
  if (virtual_key == VK_INSERT) return LoneKeyKind::insert;
  switch (virtual_key) {
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DELETE: return LoneKeyKind::editing;
    case VK_MULTIPLY: case VK_DIVIDE: case VK_ADD: case VK_SUBTRACT: case VK_DECIMAL: return LoneKeyKind::keypad;
    case VK_OEM_PERIOD: case VK_OEM_COMMA: case VK_OEM_4: case VK_OEM_6: return LoneKeyKind::character;
    default: break;
  }
  if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9) return LoneKeyKind::keypad;
  return LoneKeyKind::none;
}

inline bool lone_key_allowed(unsigned virtual_key) {
  // Letters, digits, Space, Tab, Enter, Esc, Backspace and the arrows stay out: they belong to every chat box. What
  // stands alone is chosen by the owner (2026-11-15): the F-keys, Pause, Scroll Lock, Insert, the editing keys (Home,
  // End, PageUp, PageDown, Delete), the numpad's digits and operators, and . , [ ]. Numpad Enter is not here:
  // RegisterHotKey cannot tell it from Enter. `lone_key_kind` says what each one costs.
  return lone_key_kind(virtual_key) != LoneKeyKind::none;
}

}  // namespace bx
