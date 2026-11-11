// Opt-in: telling a windowed game from any other window. Nothing about a window says "game"; the libraries its
// process loaded do. A game talks to the GPU (Direct3D or Vulkan) and to a controller, a sound engine or a
// platform client; an office program, an editor or a browser shell loads the first and not the second. Both
// have to be present, so a window that merely draws with Direct3D is not taken for a game.
//
// Best effort, and said so in the settings: a process that refuses to show its module list (anti-cheat
// protected games) is not recognised, and the person picks that game under Capture instead.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace bx {

inline bool modules_look_like_game(const std::vector<std::string>& modules) {
  static const char* const kGraphics[] = {"d3d9.dll", "d3d11.dll", "d3d12.dll", "vulkan-1.dll"};
  static const char* const kGameLibraries[] = {"xinput1_3.dll",  "xinput1_4.dll",  "xinput9_1_0.dll", "dinput8.dll",
                                               "xaudio2_7.dll",  "xaudio2_8.dll",  "xaudio2_9.dll",   "steam_api.dll",
                                               "steam_api64.dll", "eossdk-win64-shipping.dll"};
  bool graphics = false;
  bool game_library = false;
  for (std::string name : modules) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* wanted : kGraphics) graphics = graphics || name == wanted;
    for (const char* wanted : kGameLibraries) game_library = game_library || name == wanted;
  }
  return graphics && game_library;
}

}  // namespace bx
