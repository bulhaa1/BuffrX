// Which display the user chose, and when the pipeline has to be rebuilt for it.
//
// The choice is saved twice: as the index the interface clicks (`display_id`) and as the Windows GDI
// device name (`display_name`, \\.\DISPLAY1). The name survives a replug or a rearranged desktop; the
// index is the fallback for a config that has no name yet. Pure logic, no libobs, unit-tested.
#pragma once

#include <string>
#include <vector>

#include "config.h"

namespace bx {

struct DisplayInfo {
  int id = 0;              // position in EnumDisplayMonitors order
  std::string device;      // \\.\DISPLAY1
  std::string name;        // "Primary display (2560x1440)"
  int width = 0;
  int height = 0;
  bool primary = false;
};

struct DisplayPick {
  int id = 0;
  std::string device;
  std::string name;
  bool found = false;
  // The choice is gone and the primary stands in: the caller says so, it is not the user's display.
  bool primary_fallback = false;
};

// The display a saved choice means now: the device name when it still exists, else the index, else the
// primary so the buffer keeps working, else nothing.
inline DisplayPick pick_display(const std::vector<DisplayInfo>& displays, int id, const std::string& device) {
  DisplayPick pick;
  if (displays.empty()) return pick;
  if (!device.empty()) {
    for (const DisplayInfo& display : displays) {
      if (display.device != device) continue;
      pick.id = display.id;
      pick.device = display.device;
      pick.name = display.name;
      pick.found = true;
      return pick;
    }
  }
  for (const DisplayInfo& display : displays) {
    if (display.id != id) continue;
    pick.id = display.id;
    pick.device = display.device;
    pick.name = display.name;
    pick.found = true;
    return pick;
  }
  for (const DisplayInfo& display : displays) {
    if (!display.primary) continue;
    pick.id = display.id;
    pick.device = display.device;
    pick.name = display.name;
    pick.found = true;
    pick.primary_fallback = true;
    return pick;
  }
  return pick;
}

// What to save as `display_name`: an explicit name is never overwritten (it may be the only thing that
// still identifies the monitor after a rearrange), an empty one adopts the chosen card's device, and an
// absent index stays empty rather than guessing another monitor.
inline std::string resolved_display_name(const std::vector<DisplayInfo>& displays, int id, const std::string& current) {
  if (!current.empty()) return current;
  const DisplayPick pick = pick_display(displays, id, std::string());
  if (!pick.found || pick.primary_fallback) return std::string();
  return pick.device;
}

// The live capture is on a different display than the saved choice now: unplugged (the primary took
// over) or moved back. Nothing to do while no display resolves at all.
inline bool display_follow_needed(const std::string& created_device, const std::string& wanted_device) {
  return !wanted_device.empty() && created_device != wanted_device;
}

// A switch between two displays costs a video rebuild (the ring empties), so it is only asked for when
// the new screen's native size differs from the canvas. Performance mode owns the picture at its own
// size, and a mode that is not display capture has no display canvas.
inline bool display_switch_needs_replan(const Config& before, const Config& after, int target_width, int target_height,
                                        int canvas_width, int canvas_height, bool canvas_is_output) {
  if (canvas_is_output) return false;
  if (before.capture_mode != "display" || after.capture_mode != "display") return false;
  if (before.display_id == after.display_id && before.display_name == after.display_name) return false;
  if (target_width <= 0 || target_height <= 0) return false;
  return target_width != canvas_width || target_height != canvas_height;
}

}  // namespace bx
