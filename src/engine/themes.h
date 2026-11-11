// The fourteen themes: one table the native interface, the notification card and the editor page
// (ui/src/themes.css, checked against this file by the tests) all paint from. Clutch is the main theme
// and comes first. Every colour is 0xRRGGBB.
//
// The roles, as the views use them:
//   ground  the window behind the panes; it shows in the gaps between them
//   tile    a pane's face            raised  a control or a hovered row on a pane
//   line    a pane's 1 px seam       fg      text            muted  secondary text (4.5:1 on every face)
//   acc     the accent: the focused pane's border, the selected row, the fill of a meter
//   on      text on the accent       ok / warn / danger  state      rec  the recording mark
// A theme from an older config (the vendor cockpits: auto, green, red, blue) paints the main theme.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bx {

struct ThemeColors {
  const char* id;
  const char* name;
  const char* hint;  // two or three words, shown beside the name in Settings
  bool light;
  uint32_t ground, tile, raised, line, fg, muted, acc, on, ok, warn, danger, rec;
};

inline const std::vector<ThemeColors>& themes() {
  static const std::vector<ThemeColors> all = {
      // id           name          hint                    light  ground    tile      raised    line      fg        muted     acc       on        ok        warn      danger    rec
      {"clutch", "Clutch", "dark, ember orange", false, 0x0b0c0d, 0x141517, 0x1c1e21, 0x55575a, 0xf5f3ef, 0xaaa7a2, 0xff6b2c, 0x140a04, 0x9bd36b, 0xf2c14e, 0xff4d5e, 0xff4d5e},
      {"overtime", "Overtime", "petrol, aqua", false, 0x081012, 0x0f1a1d, 0x152428, 0x22383d, 0xd6e7e3, 0x7f9f9a, 0x4fd6be, 0x04201b, 0x4fd6be, 0xf0c674, 0xff5f6d, 0xff5f6d},
      {"respawn", "Respawn", "plum ink, pink", false, 0x110e15, 0x19151f, 0x221c2a, 0x362d42, 0xebe2f2, 0x9f94ad, 0xf5a3c7, 0x2a0f1d, 0xa6e3a1, 0xf9d58b, 0xff6b81, 0xff6b81},
      {"afterglow", "Afterglow", "espresso, amber", false, 0x130e0b, 0x1c1511, 0x261d17, 0x3d2e25, 0xf1e4d6, 0xab9582, 0xf2b56b, 0x2a1806, 0xb5c96a, 0xf2b56b, 0xff6a55, 0xff6a55},
      {"killcam", "Killcam", "pure black, white", false, 0x000000, 0x0a0a0a, 0x151515, 0x262626, 0xf2f2f2, 0x8a8a8a, 0xf2f2f2, 0x000000, 0xf2f2f2, 0xffd60a, 0xff3b30, 0xff3b30},
      {"lobby", "Lobby", "light, cobalt", true, 0xd9d6cd, 0xf3f1ec, 0xe6e3dc, 0xc6c2b8, 0x1d1c1a, 0x55524c, 0x2f5bea, 0xffffff, 0x2e7d50, 0x9a6512, 0xc62f2f, 0xd93636},
      {"headshot", "Headshot", "oxblood, signal red", false, 0x0e0809, 0x170d0f, 0x221316, 0x3a1d22, 0xf3e6e3, 0xb19494, 0xff3b4e, 0x1a0306, 0xe8d8b0, 0xffb347, 0xffb347, 0xf3e6e3},
      {"frostbite", "Frostbite", "midnight, ice blue", false, 0x080d16, 0x0e1522, 0x151e2f, 0x24324a, 0xdde8f6, 0x8a9cb6, 0x8ecbff, 0x04172b, 0x7ee0c3, 0xf5d27a, 0xff6b7a, 0xff6b7a},
      {"toxic", "Toxic", "olive black, acid lime", false, 0x0b0d08, 0x12150d, 0x1a1e13, 0x2b321f, 0xe6eed6, 0x97a184, 0xc6f432, 0x141c02, 0xc6f432, 0xffcf3d, 0xff5a4f, 0xff5a4f},
      {"nightshift", "Nightshift", "ink, violet", false, 0x0b0a12, 0x12111c, 0x1a1828, 0x2c2942, 0xe4e0fa, 0x9a95bb, 0x9d8cff, 0x120a33, 0x7fd8be, 0xffc777, 0xff6e8a, 0xff6e8a},
      {"highscore", "Highscore", "navy, arcade yellow", false, 0x0b0d1c, 0x111428, 0x181c36, 0x2a2f55, 0xeef0ff, 0x9ea4cc, 0xffd23f, 0x1f1700, 0x5ef0c0, 0xff9f43, 0xff4f8b, 0xff4f8b},
      {"ghillie", "Ghillie", "moss, sage", false, 0x0d0f0b, 0x141711, 0x1c2018, 0x2e3427, 0xe2e6d8, 0x9aa18c, 0xb5c58e, 0x161c08, 0xb5c58e, 0xe3b55b, 0xe8604c, 0xe8604c},
      {"sandstorm", "Sandstorm", "light, rust", true, 0xddd2bd, 0xf4ede0, 0xe9e0cf, 0xcbbda3, 0x2b2118, 0x5c4c3a, 0xb8400f, 0xfff6ee, 0x3f6b24, 0x8f5a00, 0xb01e1e, 0xc42323},
      {"arcade", "Arcade", "black, cyan", false, 0x050607, 0x0b0d0f, 0x121518, 0x22272c, 0xeaf6f8, 0x8aa0a6, 0x00e1ff, 0x00262c, 0x5cff9d, 0xffd23f, 0xff2e63, 0xff2e63},
  };
  return all;
}

inline const char* default_theme_id() { return "clutch"; }

inline bool is_theme_id(const std::string& id) {
  for (const ThemeColors& t : themes()) {
    if (id == t.id) return true;
  }
  return false;
}

// The theme for a stored id; anything else (an older cockpit, empty, unknown) is the main theme.
inline const ThemeColors& theme_colors(const std::string& id) {
  for (const ThemeColors& t : themes()) {
    if (id == t.id) return t;
  }
  return themes().front();
}

}  // namespace bx
