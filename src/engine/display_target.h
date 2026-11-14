// Which display the user chose, and when the pipeline has to be rebuilt for it.
//
// The choice is saved twice: as the index the interface clicks (`display_id`) and as the Windows GDI
// device name (`display_name`, \\.\DISPLAY1). The name survives a replug or a rearranged desktop; the
// index is the fallback for a config that has no name yet. Pure logic, no libobs, unit-tested.
#pragma once

#include <string>
#include <vector>

#include "config.h"
#include "gpu.h"

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
  int width = 0;
  int height = 0;
  bool found = false;
  // The choice is gone and the primary stands in: the caller says so, it is not the user's display.
  bool primary_fallback = false;
  // The device the config asked for, kept through a fallback so a replug is chosen again with no fix-up.
  std::string requested_device;
};

// The display a saved choice means now. The device name is the choice: it is followed wherever the
// enumeration moves it. A name that is not here means that screen is gone, and the primary stands in
// (marked as such) rather than whatever monitor holds the old index now - that is another display, and
// capturing it records a screen nobody chose. An index is read only for a config that never saved a
// name. Nothing at all when the desk has no display.
inline DisplayPick pick_display(const std::vector<DisplayInfo>& displays, int id, const std::string& device) {
  DisplayPick pick;
  pick.requested_device = device;
  if (displays.empty()) return pick;
  if (!device.empty()) {
    for (const DisplayInfo& display : displays) {
      if (display.device != device) continue;
      pick.id = display.id;
      pick.device = display.device;
      pick.name = display.name;
      pick.width = display.width;
      pick.height = display.height;
      pick.found = true;
      return pick;
    }
    for (const DisplayInfo& display : displays) {
      if (!display.primary) continue;
      pick.id = display.id;
      pick.device = display.device;
      pick.name = display.name;
      pick.width = display.width;
      pick.height = display.height;
      pick.found = true;
      pick.primary_fallback = true;
      return pick;
    }
    return pick;
  }
  for (const DisplayInfo& display : displays) {
    if (display.id != id) continue;
    pick.id = display.id;
    pick.device = display.device;
    pick.name = display.name;
    pick.width = display.width;
    pick.height = display.height;
    pick.found = true;
    return pick;
  }
  for (const DisplayInfo& display : displays) {
    if (!display.primary) continue;
    pick.id = display.id;
    pick.device = display.device;
    pick.name = display.name;
    pick.width = display.width;
    pick.height = display.height;
    pick.found = true;
    pick.primary_fallback = true;
    return pick;
  }
  return pick;
}

// Whether a picker's index/device pair still describes one display. Between drawing the cards and the
// click the desktop can be rearranged or a screen unplugged: the number then belongs to another display,
// and taking the pair would capture a monitor nobody chose. An empty device is an index-only selection.
inline bool display_selection_current(const std::vector<DisplayInfo>& displays, int id, const std::string& device) {
  for (const DisplayInfo& display : displays) {
    if (display.id != id) continue;
    return device.empty() || display.device == device;
  }
  return false;
}

// What to save as `display_name`: an explicit name is never overwritten (it may be the only thing that
// still identifies the monitor after a rearrange, and a missing one is kept so a replug returns to it),
// an empty one adopts the device at the chosen index, and an absent index stays empty rather than
// guessing another monitor.
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

// The displays the running video device can copy with DXGI duplication. Its adapter's output list is
// fixed when the device is created (libobs resolves a monitor with device->adapter->EnumOutputs), so a
// display on another card, or one attached after the device was built, is not in it. The source then
// falls back to Windows Graphics Capture, which can deliver no frame at all: the ring keeps encoding
// black frames while the sound goes on.
inline bool device_outputs_have(const std::vector<std::string>& outputs, const std::string& device) {
  if (device.empty()) return false;
  for (const std::string& output : outputs) {
    if (output == device) return true;
  }
  return false;
}

// Whether the running device can duplicate `device`. `known` says whether the adapter's output list was actually
// read: an enumeration that failed is no evidence that a screen is missing, and rebuilding the video every two
// seconds on no evidence costs more than the mistake it guards.
inline bool device_can_duplicate(const std::vector<std::string>& outputs, bool known, const std::string& device) {
  if (device.empty()) return false;
  if (!known) return true;
  return device_outputs_have(outputs, device);
}

// What the desk looks like for the display being followed, as one string: where the choice resolves (or that it is
// gone), its size, and which adapters list it. A screen that comes back changes an adapter's list; a rearrange
// changes the position; another screen under the same name changes the size. Two equal stamps are the same failed
// state, so a refused rebuild stays refused; a different one is a new episode and starts the count again. Limit: a
// replug that leaves every one of these identical cannot be told from the old state, so it does not reset the count.
inline std::string display_topology_signature(const DisplayPick& display, const std::vector<GpuAdapterInfo>& adapters) {
  std::string signature = display.found ? (display.primary_fallback ? "fallback:" : "found:") : "missing:";
  signature += display.device + "|" + std::to_string(display.id) + "|" + std::to_string(display.width) + "x" +
               std::to_string(display.height) + "|" + display.requested_device;
  for (const GpuAdapterInfo& adapter : adapters) {
    signature += "|" + std::to_string(adapter.index) + (adapter.outputs_known ? "k" : "u");
    for (const std::string& output : adapter.outputs) signature += ":" + output;
  }
  return signature;
}

// Whether the follow check asks for another rebuild for this display. A rebuild is a video reset and an emptied
// ring, so it waits for a manual recording to end. One that failed is retried in half a minute - and a display the device
// has refused `max_attempts` times stops asking until the desk changes (display_topology_signature is what says so).
inline bool display_replan_due(const std::string& failed_for, const std::string& wanted, int attempts, uint32_t now_ms,
                               uint32_t retry_at_ms, uint32_t retry_ms, int max_attempts, bool manual_recording = false) {
  if (wanted.empty() || manual_recording) return false;
  if (failed_for != wanted) return true;
  if (attempts >= max_attempts) return false;
  return now_ms - retry_at_ms >= retry_ms;  // wraps with the tick clock, and still measures the wait
}

// Whether a failed rebuild is worth a card. A rebuild that fails is retried, and the person must not get a card for
// every pass: the first failure for a screen is said once, and the one that stops the retries is said once. A different
// screen starts its own count (`told_for` is what was last said).
inline bool display_replan_report_due(const std::string& told_for, const std::string& wanted, int attempts, int max_attempts) {
  if (wanted.empty() || attempts <= 0) return false;
  if (told_for != wanted) return true;
  return attempts >= max_attempts;
}

// Whether the display capture needs the device on the screen's own card. DXGI duplication does; Windows Graphics
// Capture does not - it crosses cards by itself - so a person who chose WGC explicitly must not have the video
// reset (and the ring emptied) every time the follow check looks.
inline bool display_capture_needs_card(const std::string& display_method) { return display_method != "wgc"; }

// Whether a display capture whose picture is now a different pixel size from the canvas has to be re-planned. A Native
// output is the capture's own size (config.h: plan_canvas), so another size means the canvas - and the encoder's
// output - keep the old resolution until the video is built again. A configured output is fitted inside a canvas of
// its own shape, so it needs nothing: the picture is scaled there correctly whatever the screen's size. An unknown
// display size (0) is not a request to guess.
inline bool display_native_size_needs_replan(int target_width, int target_height, int canvas_width, int canvas_height,
                                             bool output_configured, bool follows_game = false) {
  if (output_configured || follows_game) return false;
  if (target_width <= 0 || target_height <= 0) return false;
  return target_width != canvas_width || target_height != canvas_height;
}

// Whether the follow check's rebuild has anything to change. `device_reaches_display` is the device rule already
// answered (an explicit Windows Graphics Capture choice reaches every screen), and `native_size_stale` says a Native
// output no longer matches the display's own size. A device that reaches the display with the right size needs no
// video reset - the ring is not emptied and a recording is not split for nothing.
inline bool display_replan_action_needed(bool device_reaches_display, bool native_size_stale) {
  return !device_reaches_display || native_size_stale;
}

// A switch between two displays costs a video rebuild (the ring empties, a recording is split), so it is only asked
// for when the picture would otherwise be wrong. Always when the running device cannot duplicate the new screen,
// because swapping only the source leaves a capture on the wrong card (Windows Graphics Capture, which has delivered
// no frame here) - and that moves the device even with the canvas owned by Performance mode or a fixed output.
// Otherwise a canvas that already has the configured output's shape needs nothing: the new capture is fitted inside
// it and the output scale stays even, whatever its native size. A configured output of another shape re-plans the
// canvas, and a Native output is the capture's own size, so any size change re-plans it. Coming from a mode that had
// no display canvas (a window or a game), the automatic follow swaps the source under the running ring and never comes
// through here at all (set_capture_follow); `explicit_switch` says this is the person changing the target to a
// display, where a Native output has to be planned at the new screen's own size or the session keeps the old one.
inline bool display_switch_needs_replan(const Config& before, const Config& after, int target_width, int target_height,
                                        int canvas_width, int canvas_height, bool canvas_is_output,
                                        bool device_sees_display, bool output_configured = false,
                                        bool explicit_switch = false) {
  if (after.capture_mode != "display") return false;
  const bool was_display = before.capture_mode == "display";
  if (was_display && !explicit_switch && before.display_id == after.display_id && before.display_name == after.display_name) return false;
  if (!device_sees_display) return true;
  if (canvas_is_output) return false;
  if (!was_display) {
    if (!explicit_switch) return false;  // the follow layer's own swap: the source is fitted to the running canvas
    return display_native_size_needs_replan(target_width, target_height, canvas_width, canvas_height, output_configured);
  }
  if (target_width <= 0 || target_height <= 0) return false;
  if (output_configured) return !same_picture_shape(target_width, target_height, canvas_width, canvas_height);
  return target_width != canvas_width || target_height != canvas_height;
}

}  // namespace bx
