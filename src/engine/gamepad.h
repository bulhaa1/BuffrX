// Controller hotkeys: the `Pad:A+B` chord rules and the fire-once policy.
// Nothing here calls XInput: the poller (gamepad_poll.cpp) feeds it masks, so every rule below is
// unit-tested without a controller plugged in.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bx {

// Canonical order. The chord text lists buttons in this order, whatever the order they were pressed.
enum PadButton : uint32_t {
  pad_a = 1u << 0,
  pad_b = 1u << 1,
  pad_x = 1u << 2,
  pad_y = 1u << 3,
  pad_lb = 1u << 4,
  pad_rb = 1u << 5,
  pad_lt = 1u << 6,
  pad_rt = 1u << 7,
  pad_view = 1u << 8,
  pad_menu = 1u << 9,
  pad_ls = 1u << 10,
  pad_rs = 1u << 11,
  pad_up = 1u << 12,
  pad_down = 1u << 13,
  pad_left = 1u << 14,
  pad_right = 1u << 15,
};

constexpr size_t kPadMaxButtons = 16;
// A binding is one combination of at most four buttons: five is a grip accident, not a hotkey.
constexpr size_t kPadMaxChordButtons = 4;

// Parses `Pad:A+B` (case-insensitive, spaces allowed around the parts). An empty text is a cleared
// binding: nullopt with an empty problem. A bad text sets `problem` for the row to show.
std::optional<uint32_t> parse_pad_chord(const std::string& text, std::string& problem);

// Canonical text for a mask, in the order above.
std::string format_pad_chord(uint32_t mask);

// Every button of the combo is held.
bool pad_chord_held(uint32_t combo, uint32_t held);

// Two bindings clash when one contains the other (`A` vs `A+B`, `A+B` vs `A+B`). Bindings that only
// share a button (`A+B` and `A+C`) are allowed and both fire on A+B+C.
bool pad_chords_conflict(uint32_t a, uint32_t b);

// XInput button bits and trigger readings to our mask. `held` is the previous mask, which gives each
// trigger its hysteresis.
uint32_t pad_mask_from_xinput(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger,
                              uint32_t held);

struct PadFire {
  size_t index = 0;  // index into the combo list
  uint32_t combo = 0;
};

struct PadState {
  uint32_t held = 0;
  uint32_t held_on = 0;  // bit per combo index: fired for this press, re-armed when it is released
  uint32_t peak = 0;     // buttons held since capture began
  uint64_t last_fire_ms = 0;
};

struct PadFireAt {
  uint64_t at = 0;      // when a combo last fired, for the across-pads guard
  size_t pad = 0;       // which pad fired it: the same pad pressing again is a new press
};

// One poll of one pad. Returns the combos that fire now, at most once per press. While `capturing`
// nothing fires and the buttons held accumulate in `peak` instead.
struct PadRouter {
  PadState pads[4];
  PadFireAt fired_pad_ms_[32] = {};

  std::vector<PadFire> update(size_t pad, uint32_t mask, const std::vector<uint32_t>& combos,
                              bool capturing, uint64_t now_ms);
  // Buttons accumulated while capturing, or 0.
  uint32_t captured() const;
  void reset();  // pads disconnected, and the buttons held while capturing
};

}  // namespace bx
