#include "capture_enum.h"

#include "game_watch.h"
#include "process_cache.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

namespace bx {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool equal_ci(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::string utf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(bytes, '\0');
  if (bytes > 0) {
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), bytes, nullptr, nullptr);
  }
  return out;
}

std::string process_image_name(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return std::string();
  wchar_t path[MAX_PATH * 2] = {0};
  DWORD length = static_cast<DWORD>(std::size(path));
  std::string name;
  if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0) {
    const std::wstring full(path, length);
    const size_t slash = full.find_last_of(L"\\/");
    name = utf8(slash == std::wstring::npos ? full : full.substr(slash + 1));
  }
  CloseHandle(process);
  return name;
}

}  // namespace

// The hash is escaped first, or a decoded id could not survive a second round trip: "#3A" in a real
// title must encode to "#223A", not pass through as a colon. Each scan resumes past the escape it
// just inserted — the escape itself contains the search character.
std::string encode_window_part(const std::string& part) {
  std::string out = part;
  for (size_t at = out.find('#'); at != std::string::npos; at = out.find('#', at + 3)) out.replace(at, 1, "#22");
  for (size_t at = out.find(':'); at != std::string::npos; at = out.find(':', at + 3)) out.replace(at, 1, "#3A");
  return out;
}

std::string decode_window_part(const std::string& value) {
  std::string out = value;
  for (size_t at = out.find("#3A"); at != std::string::npos; at = out.find("#3A", at)) out.replace(at, 3, ":");
  for (size_t at = out.find("#22"); at != std::string::npos; at = out.find("#22", at)) out.replace(at, 3, "#");
  return out;
}

std::string make_window_id(const std::string& title, const std::string& window_class, const std::string& exe) {
  return encode_window_part(title) + ":" + encode_window_part(window_class) + ":" + encode_window_part(exe);
}

namespace {

struct WindowIdentity {
  std::string title;
  std::string window_class;
  std::string exe;
};

// Splits a configured id into its three decoded parts. An id without the two separators is treated as
// a title alone, which is what a hand-written config usually means.
bool split_window_key(const std::string& key, WindowIdentity& out) {
  const size_t first = key.find(':');
  if (first == std::string::npos) {
    out.title = decode_window_part(key);
    return !key.empty();
  }
  const size_t second = key.find(':', first + 1);
  if (second == std::string::npos) {
    out.title = decode_window_part(key.substr(0, first));
    out.window_class = decode_window_part(key.substr(first + 1));
    return !out.title.empty() || !out.window_class.empty();
  }
  out.title = decode_window_part(key.substr(0, first));
  out.window_class = decode_window_part(key.substr(first + 1, second - first - 1));
  out.exe = decode_window_part(key.substr(second + 1));
  return true;
}

// The foreground is sampled every tick and the capture target is checked every 2 s over every capturable window, and
// OpenProcess + QueryFullProcessImageName per look is most of what either costs. The process behind a window does not change,
// so it is read once per (window, process): process_cache.h.
std::string cached_process_image_name(HWND window) {
  static ProcessNameCache cache;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return cache.get(reinterpret_cast<uintptr_t>(window), pid, [](uint32_t id) { return process_image_name(id); });
}

bool window_identity(HWND window, WindowIdentity& out) {
  wchar_t title[1024] = {0};
  wchar_t window_class[256] = {0};
  // InternalGetWindowText, never GetWindowText: for a window of this process GetWindowText *sends*
  // WM_GETTEXT to the thread that owns it. These run on a pipe worker that holds the engine lock, the
  // engine's own panel and toast are top-level windows, and their thread is the one that waits for that
  // lock every tick - so the two waited on each other and the engine answered nobody again. This reads
  // the caption Windows stores and sends nothing.
  InternalGetWindowText(window, title, sizeof(title) / sizeof(wchar_t));
  GetClassNameW(window, window_class, sizeof(window_class) / sizeof(wchar_t));
  if (title[0] == L'\0' && window_class[0] == L'\0') return false;
  out.title = utf8(title);
  out.window_class = utf8(window_class);

  out.exe = cached_process_image_name(window);
  if (out.exe.empty()) out.exe = "unknown";
  return true;
}

// The same validity rules the capture list applies: a hidden, tool or child window is not something the
// user could have picked from it. A minimised window is still one of the user's open apps - seven of them
// sat in the taskbar while the picker offered three windows - so the list keeps it and marks it. Game
// detection does not: a minimised window is not in front of the player.
bool window_capturable(HWND window, bool allow_minimized = false) {
  if (!IsWindowVisible(window)) return false;
  if (!allow_minimized && IsIconic(window)) return false;
  if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return false;
  if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0) return false;
  return true;
}

// Lower is better, -1 is no match: class and exe must agree, and the title only breaks the tie — the
// same shape of match libobs performs when it resolves a configured window id.
int window_rating(HWND window, const WindowIdentity& wanted) {
  WindowIdentity current;
  if (!window_identity(window, current)) return -1;
  if (!wanted.window_class.empty() && equal_ci(wanted.window_class, current.window_class) &&
      equal_ci(wanted.exe, current.exe)) {
    return equal_ci(wanted.title, current.title) ? 0 : 1;
  }
  if (!wanted.title.empty() && equal_ci(wanted.title, current.title)) return 2;
  return -1;
}

HWND best_window_match(const WindowIdentity& wanted) {
  struct Lookup {
    const WindowIdentity* wanted = nullptr;
    HWND best = nullptr;
    int best_rating = 0x7FFFFFFF;
  } lookup{&wanted, nullptr, 0x7FFFFFFF};
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto* lookup = reinterpret_cast<Lookup*>(parameter);
        if (!window_capturable(window, true)) return TRUE;
        const int rating = window_rating(window, *lookup->wanted);
        if (rating < 0 || rating >= lookup->best_rating) return TRUE;
        lookup->best_rating = rating;
        lookup->best = window;
        return rating == 0 ? FALSE : TRUE;
      },
      reinterpret_cast<LPARAM>(&lookup));
  return lookup.best;
}

// Fullscreen normally means the window's client area fills the monitor it sits on, 0.9 on both axes — the
// ratio and the containing-monitor comparison Rewind uses, which handles multi-monitor setups and
// taskbar-inset windows. Exclusive Direct3D may instead leave a smaller client at the game's resolution.
bool window_is_fullscreen(HWND window) {
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (!monitor) return false;
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) return false;
  const int monitor_width = info.rcMonitor.right - info.rcMonitor.left;
  const int monitor_height = info.rcMonitor.bottom - info.rcMonitor.top;
  if (monitor_width <= 0 || monitor_height <= 0) return false;
  RECT client{};
  if (!GetClientRect(window, &client)) return false;
  const double ratio_x = static_cast<double>(client.right - client.left) / monitor_width;
  const double ratio_y = static_cast<double>(client.bottom - client.top) / monitor_height;
  const bool maximized_with_frame =
      IsZoomed(window) && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
  if (game_fullscreen_rule(ratio_x, ratio_y, maximized_with_frame)) return true;
  // This shell state is system-wide, so it must never promote a background window to a game. The
  // foreground window must also be a visible capture candidate; presentation mode and QUNS_BUSY are
  // deliberately not treated as exclusive Direct3D.
  if (maximized_with_frame || window != GetForegroundWindow() || !window_capturable(window)) return false;
  // Some exclusive games switch the active display mode while monitor bounds still describe the desktop.
  // Compare with that mode too, but only when it is genuinely smaller than the reported monitor.
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth > 0 && mode.dmPelsHeight > 0 &&
      (mode.dmPelsWidth < static_cast<DWORD>(monitor_width) || mode.dmPelsHeight < static_cast<DWORD>(monitor_height)) &&
      game_fullscreen_rule(static_cast<double>(client.right - client.left) / mode.dmPelsWidth,
                           static_cast<double>(client.bottom - client.top) / mode.dmPelsHeight, false)) return true;
  QUERY_USER_NOTIFICATION_STATE state{};
  const bool exclusive = SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
  return game_fullscreen_rule(ratio_x, ratio_y, maximized_with_frame, exclusive);
}

std::vector<WindowInfo> enumerate(bool games_only) {
  std::vector<WindowInfo> out;
  struct EnumState {
    bool games_only;
    std::vector<WindowInfo>* out;
  } state{games_only, &out};
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto* state = reinterpret_cast<EnumState*>(parameter);
        const bool games_only = state->games_only;
        auto* out = state->out;
        if (!window_capturable(window, !games_only)) return TRUE;
        if (games_only) {
          // OBS synthesizes this window for some sources; never a real capture target.
          wchar_t title[64] = {0};
          InternalGetWindowText(window, title, 64);  // sends nothing; see window_identity
          if (equal_ci(utf8(title), "d3dproxywindow")) return TRUE;
          if (!window_is_fullscreen(window)) return TRUE;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        WindowIdentity identity;
        if (!window_identity(window, identity)) return TRUE;
        if (identity.title.empty()) return TRUE;
        if (games_only) {
          if (pid == 0) return TRUE;
          // One denylist for the whole engine (game_watch.cpp). This file used to carry a second copy that had
          // drifted: it lacked the product's own process names, so a maximised BuffrX panel was offered as a game.
          if (game_process_excluded(identity.exe)) return TRUE;
        }
        WindowInfo info;
        info.title = make_window_id(identity.title, identity.window_class, identity.exe);
        info.label = identity.title;
        info.exe = identity.exe;
        info.minimized = IsIconic(window) != FALSE;
        info.hwnd = reinterpret_cast<uintptr_t>(window);
        out->push_back(info);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&state));
  return out;
}

}  // namespace

bool game_fullscreen_rule(double ratio_x, double ratio_y, bool maximized_with_frame, bool exclusive_foreground) {
  // An exclusive game may use a lower resolution than the desktop, but not an arbitrary small window:
  // the shell's D3D state is system-wide, and "any foreground window" promoted Notepad to a game.
  return !maximized_with_frame &&
         ((ratio_x >= 0.9 && ratio_y >= 0.9) ||
          (exclusive_foreground && ratio_x >= 0.5 && ratio_y >= 0.5));
}

namespace {

}  // namespace

bool window_still_exists(uintptr_t hwnd) { return hwnd != 0 && IsWindow(reinterpret_cast<HWND>(hwnd)) != FALSE; }

bool foreground_window(ForegroundWindow& out) {
  out = ForegroundWindow{};
  const HWND window = GetForegroundWindow();
  if (!window) return false;
  WindowIdentity identity;
  {
    // Same reads as window_identity, with the process name from the cache.
    wchar_t title[1024] = {0};
    wchar_t window_class[256] = {0};
    InternalGetWindowText(window, title, sizeof(title) / sizeof(wchar_t));  // sends nothing; see window_identity
    GetClassNameW(window, window_class, sizeof(window_class) / sizeof(wchar_t));
    if (title[0] == L'\0' && window_class[0] == L'\0') return false;
    identity.title = utf8(title);
    identity.window_class = utf8(window_class);
    identity.exe = cached_process_image_name(window);
    if (identity.exe.empty()) identity.exe = "unknown";
  }
  out.hwnd = reinterpret_cast<uintptr_t>(window);
  // The id is built exactly the way the window lists build theirs, so game capture can resolve it:
  // a bare title has no class, and game capture then waits for a window that never matches.
  out.id = make_window_id(identity.title, identity.window_class, identity.exe);
  out.title = identity.title;
  out.exe = identity.exe == "unknown" ? "" : identity.exe;
  out.fullscreen = window_is_fullscreen(window);
  return true;
}

std::vector<WindowInfo> enumerate_windows() { return enumerate(false); }

std::vector<WindowInfo> enumerate_game_windows() { return enumerate(true); }

bool window_id_exists(const std::string& id) {
  WindowIdentity wanted;
  if (!split_window_key(id, wanted)) return false;
  return best_window_match(wanted) != nullptr;
}

bool capture_target_alive(const std::string& id, bool process_matters) {
  // The target we were told to capture: is it still there?
  if (id.empty()) return true;
  WindowIdentity wanted;
  if (!split_window_key(id, wanted)) return true;  // an id we cannot read: leave the target alone
  if (best_window_match(wanted)) return true;
  if (!process_matters) return false;
  // The game capture hooks the process, not the window: another window of the same program is enough
  // to say the game is still running.
  if (wanted.exe.empty()) return false;
  for (const WindowInfo& window : enumerate_windows()) {
    if (equal_ci(window.exe, wanted.exe)) return true;
  }
  return false;
}

bool window_id_is_own_process(const std::string& id) {
  WindowIdentity wanted;
  if (!split_window_key(id, wanted)) return false;
  const HWND window = best_window_match(wanted);
  if (!window) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return pid != 0 && pid == GetCurrentProcessId();
}

bool find_window_size(const std::string& wanted_id, int& width, int& height) {
  WindowIdentity wanted;
  if (!split_window_key(wanted_id, wanted)) return false;
  const HWND window = best_window_match(wanted);
  if (!window) return false;
  RECT rect{};
  if (!GetClientRect(window, &rect)) return false;
  width = rect.right - rect.left;
  height = rect.bottom - rect.top;
  return width > 0 && height > 0;
}

bool validate_capture_target(Config& config, std::string& reason) {
  const std::string saved_id = config.capture_mode == "game" ? config.game_title : config.window_id;
  if (config.capture_mode != "window" && config.capture_mode != "game") return true;
  if (config.capture_mode == "game" && config.game_title.empty()) return true;  // "any fullscreen game"

  bool exists = false;
  bool own = false;
  if (config.capture_mode == "game") {
    // A saved game target must be a window the game hook would take: fullscreen and not on the
    // never-a-game denylist. This is what keeps SearchHost.exe out.
    for (const auto& game : enumerate_game_windows()) {
      if (game.title == config.game_title) {
        exists = true;
        break;
      }
    }
    own = window_id_is_own_process(config.game_title);
  } else {
    exists = window_id_exists(config.window_id);
    own = window_id_is_own_process(config.window_id);
  }
  if (exists && !own) return true;

  reason = own ? "the saved capture target resolves to BuffrX itself"
               : "the saved capture target is not on screen right now";
  config.capture_mode = "display";
  return false;
}

}  // namespace bx
