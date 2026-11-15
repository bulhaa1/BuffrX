// The display-replan decisions, checked at the three points the engine acts on them: apply() (a settings change),
// check_display_present() (the 2 s follow check) and rebuild_for_display() (the device move).
//
// It lives in a file of its own while another change owns tests/engine_tests.cpp: the same checks move into that
// harness with one include and one call when that change lands. Built as buffrx-display-tests, which needs nothing
// but the pure headers (no libobs, no GPU, no window), so it runs anywhere.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/engine/display_target.h"

namespace {

int failures = 0;

void check(bool condition, const char* name) {
  std::printf("  %s %s\n", condition ? "ok  " : "FAIL", name);
  if (!condition) ++failures;
}

bx::Config display_config(int id, const char* name) {
  bx::Config config;
  config.capture_mode = "display";
  config.display_id = id;
  config.display_name = name;
  return config;
}

// ---------------------------------------------------------------- a switch into a display (finding 1)

void test_switch_into_display() {
  std::printf("display: a switch into a display\n");
  const bx::Config before = display_config(1, "\\\\.\\DISPLAY2");
  const bx::Config after = display_config(0, "\\\\.\\DISPLAY1");

  // The reported hole: a live window or game capture on one card, then a display on another. The old rule
  // returned false before it ever looked at the device, so the source swapped under the wrong card's device.
  bx::Config window = before;
  window.capture_mode = "window";
  check(bx::display_switch_needs_replan(window, after, 2560, 1440, 1920, 1080, false, false),
        "window capture -> a display the device cannot duplicate moves the device");
  bx::Config game = before;
  game.capture_mode = "game";
  check(bx::display_switch_needs_replan(game, after, 2560, 1440, 1920, 1080, false, false),
        "game capture -> a display the device cannot duplicate moves the device");
  check(bx::display_switch_needs_replan(game, after, 2560, 1440, 1920, 1080, true, false),
        "and it moves it even while performance mode owns the canvas");

  // The same switch on a card that already sees the display: the source swaps under the ring, as before.
  check(!bx::display_switch_needs_replan(window, after, 1920, 1080, 1920, 1080, false, true),
        "window capture -> a same-size display the device duplicates keeps the buffer");
  check(!bx::display_switch_needs_replan(game, after, 1920, 1080, 1920, 1080, false, true),
        "game capture -> a same-size display the device duplicates keeps the buffer");

  // A Native output is the display's own pixel size. The person's own switch from a window or a game to a display of
  // another size therefore has to plan the video again, or the whole session records at the old resolution. The
  // automatic follow never comes through here (it swaps the source under the ring), so this is the explicit case only.
  check(bx::display_switch_needs_replan(window, after, 2560, 1440, 1920, 1080, false, true, false, true),
        "window capture -> a native-output display of another size re-plans the canvas");
  check(bx::display_switch_needs_replan(game, after, 2560, 1440, 1920, 1080, false, true, false, true),
        "game capture -> a native-output display of another size re-plans the canvas");
  // The automatic follow (the alt-tab layer) reaches the same function with the old modes: it swaps the source under
  // the running ring and the canvas keeps the size it was planned at.
  check(!bx::display_switch_needs_replan(game, after, 2560, 1440, 1920, 1080, false, true, false, false),
        "the automatic follow from a game never re-plans");
  check(!bx::display_switch_needs_replan(game, after, 1920, 1080, 1920, 1080, false, true, false, true),
        "a native output whose canvas already has the new display's size keeps the buffer");
  check(bx::display_switch_needs_replan(after, after, 2560, 1440, 1920, 1080, false, true, false, true),
        "explicitly choosing the followed display replans a stale native canvas");
  check(!bx::display_native_size_needs_replan(2560, 1440, 1920, 1080, false, true),
        "an automatic game follow keeps the game canvas without repeated resets");
  check(!bx::display_switch_needs_replan(game, after, 0, 0, 1920, 1080, false, true, false, true),
        "a display whose size is not known is not guessed at");
  // A configured output is fitted inside its own canvas shape: another native size is the correct picture there.
  check(!bx::display_switch_needs_replan(window, after, 2560, 1440, 1920, 1080, true, true, true, true),
        "a window or game -> a fixed-output display keeps the buffer");
  check(!bx::display_switch_needs_replan(game, after, 3440, 1440, 1920, 1080, false, true, true, true),
        "a fixed output keeps its canvas for a capture of another shape");
  // ... but a switch to a card that cannot duplicate the screen still moves the device, fixed output or not.
  check(bx::display_switch_needs_replan(window, after, 1920, 1080, 1920, 1080, true, false, true),
        "a configured output still moves the device to a screen it cannot reach");
}

// ---------------------------------------------------------------- the display-to-display rule, unchanged

void test_switch_between_displays() {
  std::printf("display: a switch between displays\n");
  const bx::Config before = display_config(1, "\\\\.\\DISPLAY2");
  const bx::Config after = display_config(0, "\\\\.\\DISPLAY1");

  check(!bx::display_switch_needs_replan(before, before, 1920, 1080, 1920, 1080, false, false),
        "the same choice never rebuilds, even for a display the device cannot see");
  check(bx::display_switch_needs_replan(before, after, 2560, 1440, 1920, 1080, false, true),
        "another size rebuilds the canvas");
  check(!bx::display_switch_needs_replan(before, after, 1920, 1080, 1920, 1080, false, true),
        "the same size keeps the buffer when the device already sees it");
  check(bx::display_switch_needs_replan(before, after, 1920, 1080, 1920, 1080, false, false),
        "a same-size switch to another card moves the device");
  check(bx::display_switch_needs_replan(before, after, 2560, 1440, 1920, 1080, true, false),
        "another card moves the device even when performance mode owns the canvas");
  check(!bx::display_switch_needs_replan(before, after, 2560, 1440, 1920, 1080, true, true),
        "performance mode owns the canvas otherwise");
  bx::Config window = after;
  window.capture_mode = "window";
  check(!bx::display_switch_needs_replan(before, window, 2560, 1440, 1920, 1080, false, false),
        "leaving a display never replans for a display canvas");
}

// ---------------------------------------------------------------- a configured output vs a native one

void test_fixed_output_switches() {
  std::printf("display: a fixed output is not reset for a native size it does not need\n");
  const bx::Config before = display_config(1, "\\\\.\\DISPLAY2");
  const bx::Config after = display_config(0, "\\\\.\\DISPLAY1");

  // A canvas that already has the configured output's shape: the new capture is fitted inside it and the output
  // scale stays even, whatever the new screen's native size. No ring is emptied and no recording split for it.
  check(!bx::display_switch_needs_replan(before, after, 1280, 720, 2560, 1440, true, true, true),
        "a fixed output whose canvas has its shape keeps the buffer for another size");
  // The same decision without the canvas having been recognised as output-shaped: the shapes agree, so no reset.
  check(!bx::display_switch_needs_replan(before, after, 1920, 1080, 1280, 720, false, true, true),
        "a fixed output keeps the buffer when the canvas shares the new capture's shape");
  // A capture of another shape than the canvas: the canvas has to re-plan to keep the output's shape.
  check(bx::display_switch_needs_replan(before, after, 3440, 1440, 1920, 1080, false, true, true),
        "a fixed output re-plans the canvas for a capture of another shape");
  // ... unless the canvas is already the output's shape: the new capture is fitted with bars, the correct picture.
  check(!bx::display_switch_needs_replan(before, after, 3440, 1440, 1920, 1080, true, true, true),
        "an output-shaped canvas keeps the buffer for a capture of another shape");
  // Native output: the canvas is the capture's own size, so another size re-plans it.
  check(bx::display_switch_needs_replan(before, after, 1920, 1080, 2560, 1440, false, true, false),
        "a native output re-plans for the new display's own size");
  check(!bx::display_switch_needs_replan(before, after, 2560, 1440, 2560, 1440, false, true, false),
        "and not for the size it already has");
  // The follow layer's ordinary alt-tab: the display was and is the same choice, so nothing is reset.
  check(!bx::display_switch_needs_replan(before, before, 1280, 720, 2560, 1440, false, true, false),
        "an ordinary game alt-tab never re-plans");
}

// ---------------------------------------------------------------- the follow check's native-size resets (finding 2)

void test_native_size_follow() {
  std::printf("display: a display whose own size no longer fits the canvas\n");
  // A Native output is the capture's own size: the canvas (and the encoder's output) has to be planned again when the
  // screen that comes back - or the primary standing in for one that left - delivers another size.
  check(bx::display_native_size_needs_replan(2560, 1440, 1920, 1080, false),
        "a native output whose canvas is not the display's size is re-planned");
  check(!bx::display_native_size_needs_replan(2560, 1440, 2560, 1440, false),
        "the size it already has asks for nothing");
  check(!bx::display_native_size_needs_replan(2560, 1440, 1920, 1080, true),
        "a configured output keeps its canvas whatever the screen's size");
  check(!bx::display_native_size_needs_replan(0, 0, 1920, 1080, false),
        "an unknown display size is never a size change");
  check(!bx::display_native_size_needs_replan(1920, 0, 1920, 1080, false), "half a size is not a size");

  // The follow check's action: the device that reaches the display and a canvas of the right size has nothing to do -
  // no video reset, no emptied ring, no split recording. Windows Graphics Capture reaches every screen by itself, so
  // there only the size can ask for the reset.
  check(!bx::display_replan_action_needed(true, false), "a reached display at the canvas's size needs no reset");
  check(bx::display_replan_action_needed(false, false), "a display the device cannot reach moves the device");
  check(bx::display_replan_action_needed(true, true), "a display of another size re-plans the canvas");
  check(bx::display_replan_action_needed(false, true), "a display on another card at another size does both");
  const bool wgc_reaches = true;  // display_capture_needs_card("wgc") == false: the device rule is already answered
  check(bx::display_replan_action_needed(wgc_reaches, bx::display_native_size_needs_replan(2560, 1440, 1920, 1080, false)),
        "WGC with another display size is rebuilt for the size alone");
  check(!bx::display_replan_action_needed(wgc_reaches, bx::display_native_size_needs_replan(1920, 1080, 1920, 1080, false)),
        "WGC at the canvas's size is never reset for a card");
}

// ---------------------------------------------------------------- what the running device can see

void test_device_outputs() {
  std::printf("display: what the running device can see\n");
  const std::vector<std::string> outputs = {"\\\\.\\DISPLAY1"};
  check(bx::device_outputs_have(outputs, "\\\\.\\DISPLAY1"), "a device's own output list covers its screens");
  check(!bx::device_outputs_have(outputs, "\\\\.\\DISPLAY2"), "a screen on another card is not in the list");
  check(!bx::device_outputs_have({}, "\\\\.\\DISPLAY1"), "no listed output covers nothing");
  check(!bx::device_outputs_have(outputs, ""), "an unresolved display is not an output");

  check(bx::device_can_duplicate(outputs, true, "\\\\.\\DISPLAY1"), "a listed screen is duplicated by the device");
  check(!bx::device_can_duplicate(outputs, true, "\\\\.\\DISPLAY2"), "an unlisted screen is not");
  // No enumeration means no fact: resetting the video every two seconds for a reading nobody took is worse than
  // trusting the device.
  check(bx::device_can_duplicate(outputs, false, "\\\\.\\DISPLAY2"), "an unknown output list never claims a screen is missing");
  check(bx::device_can_duplicate({}, false, "\\\\.\\DISPLAY1"), "nor does an empty one from a failed enumeration");
  check(!bx::device_can_duplicate({}, true, ""), "an unresolved display is never on the device");

  // The person's own choice: Windows Graphics Capture crosses cards by itself, so it needs no card of its own and
  // must never have the video reset (the ring emptied) for one. Duplication, chosen or automatic, does.
  check(!bx::display_capture_needs_card("wgc"), "an explicit WGC choice needs no card");
  check(bx::display_capture_needs_card("dxgi"), "an explicit duplication choice needs the screen's card");
  check(bx::display_capture_needs_card("auto"), "the automatic method duplicates first, so it needs the card too");
}

// ---------------------------------------------------------------- when the tick asks again (finding 3's backoff)

void test_replan_retry() {
  std::printf("display: when the follow check asks again\n");
  const std::string screen = "\\\\.\\DISPLAY2";
  const uint32_t retry_ms = 30000;
  const int cap = 3;

  check(bx::display_replan_due("", screen, 0, 1000, 0, retry_ms, cap), "nothing failed for this screen: ask at once");
  check(!bx::display_replan_due(screen, screen, 1, 1000, 1000, retry_ms, cap),
        "a rebuild that just failed is not asked for again on the next check");
  check(!bx::display_replan_due(screen, screen, 1, 30000, 1000, retry_ms, cap), "nor before half a minute");
  check(bx::display_replan_due(screen, screen, 1, 31000, 1000, retry_ms, cap), "half a minute later it is tried");
  check(!bx::display_replan_due(screen, screen, cap, 999999, 1000, retry_ms, cap),
        "a screen the device has refused enough times is not reset for again");
  check(bx::display_replan_due(screen, "\\\\.\\DISPLAY3", cap, 1000, 1000, retry_ms, cap),
        "another screen starts its own count, so a replug is tried at once");
  check(!bx::display_replan_due(screen, "", 0, 1000, 0, retry_ms, cap), "no resolved display, nothing to move");
  // The clock wraps (49.7 days): an elapsed-time comparison that ignores that stops retrying for ever.
  check(bx::display_replan_due(screen, screen, 1, 29744u, 0xFFFFFF00u, retry_ms, cap), "a wrapped clock still measures the wait");
}

// ---------------------------------------------------------------- which display a saved choice means now (plan 001, step 1)

bx::DisplayInfo desktop_display(int id, const char* device, bool primary) {
  bx::DisplayInfo display;
  display.id = id;
  display.device = device;
  display.width = primary ? 2560 : 1920;
  display.height = primary ? 1440 : 1080;
  display.primary = primary;
  display.name = std::string(primary ? "Primary display" : "Display") + " (" + std::to_string(display.width) + "x" +
                 std::to_string(display.height) + ")";
  return display;
}

void test_display_selection() {
  std::printf("display: which display a saved choice means now\n");
  // The desk when DISPLAY2 was chosen at position 1.
  const std::vector<bx::DisplayInfo> before = {
      desktop_display(0, "\\\\.\\DISPLAY1", true),
      desktop_display(1, "\\\\.\\DISPLAY2", false),
      desktop_display(2, "\\\\.\\DISPLAY3", false),
  };
  // Windows reordered the enumeration: DISPLAY2 is first now, and DISPLAY3 holds position 1.
  const std::vector<bx::DisplayInfo> reordered = {
      desktop_display(0, "\\\\.\\DISPLAY2", false),
      desktop_display(1, "\\\\.\\DISPLAY3", false),
      desktop_display(2, "\\\\.\\DISPLAY1", true),
  };

  const bx::DisplayPick moved = bx::pick_display(reordered, 1, "\\\\.\\DISPLAY2");
  check(moved.found && moved.id == 0 && moved.device == "\\\\.\\DISPLAY2" && !moved.primary_fallback,
        "a saved device is followed to its new index");

  // The saved screen is unplugged, and another screen now sits on the old index. The index is not the
  // choice: capturing DISPLAY3 there would record a monitor nobody picked.
  const std::vector<bx::DisplayInfo> unplugged = {
      desktop_display(0, "\\\\.\\DISPLAY1", true),
      desktop_display(1, "\\\\.\\DISPLAY3", false),
  };
  const bx::DisplayPick gone = bx::pick_display(unplugged, 1, "\\\\.\\DISPLAY2");
  check(gone.found && gone.primary_fallback && gone.id == 0 && gone.device == "\\\\.\\DISPLAY1",
        "a saved device that is gone captures the primary, never the display now on its old index");
  check(gone.requested_device == "\\\\.\\DISPLAY2", "the missing device stays the saved choice, so a replug returns to it");

  // Only a config that never saved a device uses its index.
  check(bx::pick_display(before, 1, "").device == "\\\\.\\DISPLAY2", "an index is the choice while no device was saved");
  check(bx::pick_display(reordered, 1, "").device == "\\\\.\\DISPLAY3", "a legacy index means today's display at that position");

  // An index-only client means "the display at this position now", even when the number is the one stored:
  // an explicit index must not be overruled by an older saved name.
  check(bx::display_selection_current(reordered, 1, ""), "an index-only selection is valid while that position exists");
  check(bx::resolved_display_name(reordered, 1, "") == "\\\\.\\DISPLAY3", "an explicit index adopts today's device there");
  check(!bx::display_selection_current(reordered, 7, ""), "an index the desk does not have is not a selection");

  // A card drawn before a reorder carries a stale number: taking it would capture another screen.
  check(!bx::display_selection_current(reordered, 1, "\\\\.\\DISPLAY2"),
        "a pair whose index now holds another display is stale");
  check(!bx::display_selection_current(reordered, 9, "\\\\.\\DISPLAY2"), "a pair whose index is gone is stale");
  check(bx::display_selection_current(reordered, 0, "\\\\.\\DISPLAY2"), "a pair that still agrees is taken");
  check(!bx::display_selection_current(reordered, 1, "\\\\.\\DISPLAY2"), "and a moved device needs a refreshed pair");

  // The screen comes back under the same name: the saved choice is used again.
  const bx::DisplayPick back = bx::pick_display(before, 1, "\\\\.\\DISPLAY2");
  check(back.found && back.id == 1 && back.device == "\\\\.\\DISPLAY2" && !back.primary_fallback,
        "a reconnect under the same name is chosen again");
  check(bx::resolved_display_name(unplugged, 1, "\\\\.\\DISPLAY2") == "\\\\.\\DISPLAY2",
        "a missing saved device stays the saved choice for when it returns");

  const bx::DisplayPick none = bx::pick_display({}, 0, "\\\\.\\DISPLAY2");
  check(!none.found, "no display at all is not a pick, so the caller can say capture is unavailable");
  const std::vector<bx::DisplayInfo> no_primary = {desktop_display(0, "\\\\.\\DISPLAY3", false)};
  check(!bx::pick_display(no_primary, 0, "\\\\.\\DISPLAY2").found,
        "a screen that is not the primary never stands in for a missing choice");
}

// ---------------------------------------------------------------- what a failed rebuild is allowed to say

void test_replan_reporting() {
  std::printf("display: what a failed rebuild is allowed to say\n");
  const std::string screen = "\\\\.\\DISPLAY2";
  const int cap = 3;
  check(bx::display_replan_report_due("", screen, 1, cap), "the first failure for a screen is said");
  check(!bx::display_replan_report_due(screen, screen, 1, cap), "a retry of the same failure is not a second card");
  check(!bx::display_replan_report_due(screen, screen, 2, cap), "nor is the middle attempt");
  check(bx::display_replan_report_due(screen, screen, cap, cap), "the attempt that stops the retries is said once");
  check(bx::display_replan_report_due(screen, "\\\\.\\DISPLAY3", 1, cap), "another screen starts its own report");
  check(!bx::display_replan_report_due("", "", 1, cap), "no resolved display has nothing to report");
  check(!bx::display_replan_report_due("", screen, 0, cap), "a rebuild that was never tried has nothing to report");
}

// ---------------------------------------------------------------- a refused display is retried when the desk changes

void test_topology_reset() {
  std::printf("display: what counts as the desk changing\n");
  const std::vector<bx::DisplayInfo> desk = {desktop_display(0, "\\\\.\\DISPLAY1", true)};
  const bx::DisplayPick pick = bx::pick_display(desk, 0, "\\\\.\\DISPLAY1");
  bx::GpuAdapterInfo card;
  card.index = 0;
  card.outputs = {"\\\\.\\DISPLAY1"};
  card.outputs_known = true;
  const std::string stamp = bx::display_topology_signature(pick, {card});
  check(stamp == bx::display_topology_signature(pick, {card}), "the same desk reads the same stamp");

  // The screen comes back under the same name: the adapter lists it again. This is the replug the retry cap used
  // to suppress for the rest of the run.
  bx::GpuAdapterInfo empty = card;
  empty.outputs.clear();
  check(bx::display_topology_signature(pick, {empty}) != stamp, "a screen listed again changes the stamp");
  // A card whose output list could not be read is different evidence from one that drives no screen.
  bx::GpuAdapterInfo unread = card;
  unread.outputs_known = false;
  check(bx::display_topology_signature(pick, {unread}) != stamp, "an unreadable output list is different evidence");
  // A rearrange moves the display's position.
  const std::vector<bx::DisplayInfo> reordered = {desktop_display(0, "\\\\.\\DISPLAY2", false),
                                                  desktop_display(1, "\\\\.\\DISPLAY1", true)};
  const bx::DisplayPick moved = bx::pick_display(reordered, 1, "\\\\.\\DISPLAY1");
  check(bx::display_topology_signature(moved, {card}) != stamp, "a rearrange changes the stamp");
  // Another screen under the same name is another size.
  std::vector<bx::DisplayInfo> swapped = desk;
  swapped[0].width = 1280;
  swapped[0].height = 720;
  check(bx::display_topology_signature(bx::pick_display(swapped, 0, "\\\\.\\DISPLAY1"), {card}) != stamp,
        "another screen under the same name changes the stamp");
  // A screen that is gone (the primary stands in) is its own state.
  const bx::DisplayPick gone = bx::pick_display(desk, 0, "\\\\.\\DISPLAY2");
  check(bx::display_topology_signature(gone, {card}) != stamp, "a missing choice is its own stamp");

  // The rule the follow check applies: an unchanged stamp keeps the refusal at the cap, and a changed one (the
  // check clears the count) asks again at once. A person's own choice clears it the same way (reset_display_replan).
  const std::string screen = "\\\\.\\DISPLAY2";
  const uint32_t retry_ms = 30000;
  const int cap = 3;
  check(!bx::display_replan_due(screen, screen, cap, 999999, 1000, retry_ms, cap),
        "the same failed desk is not reset for again at the cap");
  check(bx::display_replan_due("", screen, 0, 1000, 0, retry_ms, cap),
        "a cleared refusal (desk changed, or the person chose again) asks at once");
}

}  // namespace

int main() {
  check(!bx::display_replan_due("", "screen", 0, 1000, 0, 30000, 3, true),
        "an automatic display rebuild waits while a manual recording runs");
  check(bx::display_replan_due("", "screen", 0, 1000, 0, 30000, 3, false),
        "the deferred display rebuild is due when the manual recording ends");
  test_switch_into_display();
  test_switch_between_displays();
  test_fixed_output_switches();
  test_native_size_follow();
  test_device_outputs();
  test_replan_retry();
  test_display_selection();
  test_replan_reporting();
  test_topology_reset();
  std::printf("%s\n", failures == 0 ? "all display replan checks passed" : "display replan checks FAILED");
  return failures == 0 ? 0 : 1;
}
