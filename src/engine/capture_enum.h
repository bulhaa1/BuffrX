// Win32 window enumeration for the capture pickers and the persisted-target check.
//
// This replaces what used to be done by creating a temporary libobs capture source and reading its
// property list — the path that crashed inside win-capture.dll. Pure Win32, no libobs, so it is
// unit-testable and safe to call from anywhere in the engine. The ids it builds are byte-compatible
// with what libobs's window and game property lists handed out, so saved configs and the wire
// format keep working: "title:class:exe" with ':' escaped to "#3A" and '#' to "#22".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace bx {

struct WindowInfo {
  std::string title;   // "title:class:exe" as the capture sources want it
  std::string label;   // human-readable window title
  std::string exe;     // owning process image name, "unknown" when it cannot be read
  uintptr_t hwnd = 0;      // the window itself, for a caller that follows it instead of looking its program up again
  bool minimized = false;  // out of the way in the taskbar: listed, but it hands over no picture until restored
};

std::string encode_window_part(const std::string& part);
std::string decode_window_part(const std::string& value);
std::string make_window_id(const std::string& title, const std::string& window_class, const std::string& exe);

// Top-level windows the user could pick from: visible, not a tool or child window, with a title. A
// minimised window is listed (it is still one of the user's open apps) and marked as such. Includes
// our own process's windows, as libobs's list did.
std::vector<WindowInfo> enumerate_windows();

// The windows game capture would hook: a fullscreen window whose process is not on the never-a-game denylist.
std::vector<WindowInfo> enumerate_game_windows();

// The fullscreen rule game detection applies, decided without Win32 so it can be tested: the client
// area fills at least 0.9 of its monitor on both axes, and the window is not an ordinary maximized
// window (maximized and still framed — a browser or an editor filling a screen whose taskbar hides).
// OBS's game capture skips that same shape of window. Windows can also report exclusive Direct3D on the
// foreground window even when its client area uses a lower resolution than the desktop.
bool game_fullscreen_rule(double ratio_x, double ratio_y, bool maximized_with_frame, bool exclusive_foreground = false);

// The window in the foreground, as game detection needs it: its game-capture id ("title:class:exe",
// which is what game_capture's "window" setting must be given), its plain title and exe, and whether
// it passes game_fullscreen_rule. False when there is no foreground window.
struct ForegroundWindow {
  std::string id;
  std::string title;
  std::string exe;
  bool fullscreen = false;
  uintptr_t hwnd = 0;  // the window itself, so "is it still there" is one IsWindow call
};
// Cheap enough to call every tick: the process name is read once per window and process, not per call.
bool foreground_window(ForegroundWindow& out);
// Whether that window still exists (it may have been recreated, so a caller falls back to its executable).
bool window_still_exists(uintptr_t hwnd);

// A configured id still resolves to a live, capturable window.
bool window_id_exists(const std::string& id);

// Is the target we were told to capture still there? `process_matters` is true for the game capture,
// which hooks the process rather than the window: another window of the same program is enough to
// say the game is alive. False for the window capture, which is that one window and nothing else.
bool capture_target_alive(const std::string& id, bool process_matters);

// And that window belongs to this process: capturing the engine's own hidden windows is never what
// a saved config meant.
bool window_id_is_own_process(const std::string& id);

// Best-effort client size of a configured window id, before the capture source exists.
bool find_window_size(const std::string& wanted_id, int& width, int& height);

// Load-time reality check for the persisted capture target. A saved window or game that no longer
// exists, or that resolves to this process, flips `capture_mode` to "display" (the dead id is kept
// so the user can see what it was) and fills `reason`. Returns true when nothing changed.
bool validate_capture_target(Config& config, std::string& reason);

}  // namespace bx
