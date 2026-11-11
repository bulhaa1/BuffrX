// The seven hotkey actions and the bindings they ship with: one table for the engine's fresh config and
// the interface's Default / Reset buttons, so the two cannot drift apart again (they had).
#pragma once

#include <string>

namespace bx {

struct HotkeyDefault {
  const char* action;
  const char* keyboard;
  const char* pad;
};

// Pairs a game rarely takes together: bumpers save, both stick clicks shoot, View+Menu records and
// View+Y opens the panel. Quick clip, the clip viewer and Marker ship unbound on a pad.
inline constexpr HotkeyDefault kHotkeyDefaults[] = {
    {"save_replay", "Alt+1", "Pad:LB+RB"},
    {"quick_clip", "Ctrl+Alt+1", ""},
    {"screenshot", "Alt+2", "Pad:LS+RS"},
    {"toggle_record", "Alt+3", "Pad:VIEW+MENU"},
    {"toggle_ui", "Alt+X", "Pad:Y+VIEW"},
    {"view_clip", "Alt+4", ""},
    {"add_marker", "Alt+5", ""},
};

inline std::string default_hotkey(const std::string& action, bool controller) {
  for (const HotkeyDefault& row : kHotkeyDefaults) {
    if (action == row.action) return controller ? row.pad : row.keyboard;
  }
  return std::string();
}

}  // namespace bx
