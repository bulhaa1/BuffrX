#include "gamepad.h"

#include <algorithm>
#include <cctype>

namespace bx {
namespace {

const char kPrefix[] = "Pad:";

struct NamedButton {
  const char* name;
  uint32_t bit;
};

// Canonical order, longest names first only where a prefix would clash (none do: LB vs L).
const NamedButton kPadButtons[] = {
    {"A", pad_a},       {"B", pad_b},         {"X", pad_x},       {"Y", pad_y},
    {"LB", pad_lb},     {"RB", pad_rb},       {"LT", pad_lt},     {"RT", pad_rt},
    {"VIEW", pad_view}, {"MENU", pad_menu},   {"LS", pad_ls},     {"RS", pad_rs},
    {"UP", pad_up},     {"DOWN", pad_down},   {"LEFT", pad_left}, {"RIGHT", pad_right},
};

uint32_t button_bit(const std::string& name) {
  for (const NamedButton& button : kPadButtons) {
    if (name == button.name) return button.bit;
  }
  return 0;
}

std::string upper(std::string text) {
  for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

std::string trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

bool starts_with_pad(const std::string& text) {
  return text.size() >= 4 && upper(text.substr(0, 4)) == "PAD:";
}

}  // namespace

std::optional<uint32_t> parse_pad_chord(const std::string& text, std::string& problem) {
  problem.clear();
  const std::string trimmed = trim(text);
  if (trimmed.empty()) return std::nullopt;

  if (!starts_with_pad(trimmed)) {
    problem = "controller buttons look like Pad:A+B";
    return std::nullopt;
  }

  uint32_t mask = 0;
  size_t count = 0;
  const std::string rest = trimmed.substr(4);
  size_t start = 0;
  while (start <= rest.size()) {
    size_t plus = rest.find('+', start);
    if (plus == std::string::npos) plus = rest.size();
    const std::string part = upper(trim(rest.substr(start, plus - start)));
    if (part.empty()) {
      problem = "a controller combination cannot have an empty part";
      return std::nullopt;
    }
    const uint32_t bit = button_bit(part);
    if (!bit) {
      problem = "'" + part + "' is not a controller button";
      return std::nullopt;
    }
    if (mask & bit) {
      problem = "'" + part + "' is listed twice";
      return std::nullopt;
    }
    mask |= bit;
    if (++count > kPadMaxChordButtons) {
      problem = "a controller combination takes up to " + std::to_string(kPadMaxChordButtons) + " buttons";
      return std::nullopt;
    }
    if (plus == rest.size()) break;
    start = plus + 1;
  }

  if (!mask) {
    problem = "pick at least one controller button";
    return std::nullopt;
  }
  return mask;
}

std::string format_pad_chord(uint32_t mask) {
  if (!mask) return std::string();
  std::string text = kPrefix;
  bool first = true;
  for (const NamedButton& button : kPadButtons) {
    if (!(mask & button.bit)) continue;
    if (!first) text += '+';
    first = false;
    text += button.name;
  }
  return text;
}

bool pad_chord_held(uint32_t combo, uint32_t held) {
  return combo != 0 && (held & combo) == combo;
}

bool pad_chords_conflict(uint32_t a, uint32_t b) {
  if (!a || !b) return false;
  return pad_chord_held(a, b) || pad_chord_held(b, a);
}

uint32_t pad_mask_from_xinput(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger,
                              uint32_t held) {
  struct Mapping {
    uint16_t xinput;
    uint32_t bit;
  };
  static const Mapping kButtons[] = {
      {0x0001, pad_up},    {0x0002, pad_down}, {0x0004, pad_left}, {0x0008, pad_right},
      {0x0010, pad_menu},  {0x0020, pad_view}, {0x0040, pad_ls},   {0x0080, pad_rs},
      {0x0100, pad_lb},    {0x0200, pad_rb},   {0x1000, pad_a},    {0x2000, pad_b},
      {0x4000, pad_x},     {0x8000, pad_y},
  };

  uint32_t mask = 0;
  for (const Mapping& mapping : kButtons) {
    if (buttons & mapping.xinput) mask |= mapping.bit;
  }
  // Digital triggers: press at 30/255, stay held until 15 so a finger resting near the line does not
  // chatter.
  const uint8_t left_line = (held & pad_lt) ? 15 : 30;
  const uint8_t right_line = (held & pad_rt) ? 15 : 30;
  if (left_trigger >= left_line) mask |= pad_lt;
  if (right_trigger >= right_line) mask |= pad_rt;
  return mask;
}

std::vector<PadFire> PadRouter::update(size_t pad, uint32_t mask, const std::vector<uint32_t>& combos,
                                       bool capturing, uint64_t now_ms) {
  std::vector<PadFire> fired;
  if (pad >= 4) return fired;
  PadState& state = pads[pad];
  const uint32_t newly = mask & ~state.held;
  state.held = mask;

  for (size_t i = 0; i < combos.size() && i < 32; ++i) {
    if (combos[i] && !pad_chord_held(combos[i], mask)) state.held_on &= ~(uint32_t{1} << i);
  }

  if (!mask) {
    state.peak = 0;
    if (capturing) return fired;
  }

  if (capturing) {
    if (newly) state.peak |= mask;
    return fired;
  }
  if (!newly) return fired;

  constexpr uint64_t kAcrossPadsGuardMs = 250;
  for (size_t i = 0; i < combos.size() && i < 32; ++i) {
    const uint32_t combo = combos[i];
    if (!combo || (state.held_on & (uint32_t{1} << i))) continue;
    if (!(newly & combo) || !pad_chord_held(combo, mask)) continue;
    // A second pad holding the same buttons 250 ms later is the same press, not a second one.
    if (fired_pad_ms_[i].pad != pad && fired_pad_ms_[i].at &&
        now_ms - fired_pad_ms_[i].at < kAcrossPadsGuardMs) {
      state.held_on |= uint32_t{1} << i;
      continue;
    }
    state.held_on |= uint32_t{1} << i;
    state.last_fire_ms = now_ms;
    fired_pad_ms_[i].at = now_ms;
    fired_pad_ms_[i].pad = pad;
    fired.push_back({i, combo});
  }
  return fired;
}

uint32_t PadRouter::captured() const {
  uint32_t peak = 0;
  for (const PadState& pad : pads) peak |= pad.peak;
  return peak;
}

void PadRouter::reset() {
  for (PadState& pad : pads) pad = PadState{};
}

}  // namespace bx
