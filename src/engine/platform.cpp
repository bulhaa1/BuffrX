#include "platform.h"

#include <thread>

#include <commdlg.h>
#include <dpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "../shared/files.h"
#include "../shared/log.h"
#include "game_watch.h"
#include "elevation_policy.h"
#include "themes.h"
#include "webview.h"

namespace bx {
namespace {

const wchar_t* kWindowClass = L"BuffrX.Engine.MessageWindow";
const UINT kTrayCallbackMessage = WM_APP + 1;
const UINT kHotkeyBaseId = 0x5100;
// Hotkeys belong to the window's own thread: RegisterHotKey refuses a window another thread created,
// so a change from a pipe thread is posted here and applied by the window procedure.
const UINT kApplyHotkeysMessage = WM_APP + 2;
const UINT kSuspendHotkeysMessage = WM_APP + 3;
// The tray menu holds the thread in TrackPopupMenu; a timer is the one thing that loop still dispatches.
constexpr UINT_PTR kMenuHeartbeatTimer = 0xB0F2;


UINT virtual_key_from_name(const std::string& name, bool& ok) {
  ok = true;
  if (name.size() == 1) {
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (c >= 'A' && c <= 'Z') return static_cast<UINT>(c);
    if (c >= '0' && c <= '9') return static_cast<UINT>(c);
    ok = false;
    return 0;
  }
  if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
    int number = std::atoi(name.c_str() + 1);
    if (number >= 1 && number <= 24) return static_cast<UINT>(VK_F1 + number - 1);
  }
  struct Entry {
    const char* name;
    UINT key;
  };
  static const Entry entries[] = {
      {"SPACE", VK_SPACE},       {"TAB", VK_TAB},         {"ENTER", VK_RETURN},    {"ESC", VK_ESCAPE},
      {"ESCAPE", VK_ESCAPE},     {"BACKSPACE", VK_BACK},  {"INSERT", VK_INSERT},   {"DELETE", VK_DELETE},
      {"HOME", VK_HOME},         {"END", VK_END},         {"PAGEUP", VK_PRIOR},    {"PAGEDOWN", VK_NEXT},
      {"UP", VK_UP},             {"DOWN", VK_DOWN},       {"LEFT", VK_LEFT},       {"RIGHT", VK_RIGHT},
      {"PRINTSCREEN", VK_SNAPSHOT}, {"SCROLLLOCK", VK_SCROLL}, {"PAUSE", VK_PAUSE},
      {"NUM0", VK_NUMPAD0}, {"NUM1", VK_NUMPAD1}, {"NUM2", VK_NUMPAD2}, {"NUM3", VK_NUMPAD3}, {"NUM4", VK_NUMPAD4},
      {"NUM5", VK_NUMPAD5}, {"NUM6", VK_NUMPAD6}, {"NUM7", VK_NUMPAD7}, {"NUM8", VK_NUMPAD8}, {"NUM9", VK_NUMPAD9},
  };
  std::string upper;
  for (char c : name) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  for (const auto& entry : entries) {
    if (upper == entry.name) return entry.key;
  }
  ok = false;
  return 0;
}

std::string key_name_from_virtual(UINT key) {
  if (key >= 'A' && key <= 'Z') return std::string(1, static_cast<char>(key));
  if (key >= '0' && key <= '9') return std::string(1, static_cast<char>(key));
  if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
  // Every name virtual_key_from_name accepts comes back out, so a recorded chord always parses.
  if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return "Num" + std::to_string(key - VK_NUMPAD0);
  switch (key) {
    case VK_SNAPSHOT: return "PrintScreen";
    case VK_SCROLL: return "ScrollLock";
    case VK_PAUSE: return "Pause";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Esc";
    case VK_BACK: return "Backspace";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    default: return "?";
  }
}

}  // namespace

bool parse_chord(const std::string& chord, UINT& modifiers, UINT& virtual_key, std::string& problem) {
  modifiers = 0;
  virtual_key = 0;
  problem.clear();
  std::vector<std::string> tokens;
  std::string current;
  for (char c : chord) {
    if (c == '+' || c == '-') {
      tokens.push_back(current);
      current.clear();
    } else if (c != ' ') {
      current.push_back(c);
    }
  }
  if (!current.empty()) tokens.push_back(current);
  if (tokens.size() < 2) {
    problem = "a chord needs a modifier and a key";
    return false;
  }
  for (size_t i = 0; i < tokens.size() - 1; ++i) {
    std::string token;
    for (char c : tokens[i]) token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (token == "CTRL" || token == "CONTROL") modifiers |= MOD_CONTROL;
    else if (token == "ALT") modifiers |= MOD_ALT;
    else if (token == "SHIFT") modifiers |= MOD_SHIFT;
    else if (token == "WIN") modifiers |= MOD_WIN;
    else {
      problem = "unknown modifier: " + tokens[i];
      return false;
    }
  }
  bool ok = false;
  virtual_key = virtual_key_from_name(tokens.back(), ok);
  if (!ok) {
    problem = "unknown key: " + tokens.back();
    return false;
  }
  if (modifiers == 0 && (virtual_key < VK_F13 || virtual_key > VK_F24)) {
    problem = "a lone key would fire while typing";
    return false;
  }
  modifiers |= MOD_NOREPEAT;
  return true;
}

std::string format_chord(UINT modifiers, UINT virtual_key) {
  std::string out;
  if (modifiers & MOD_CONTROL) out += "Ctrl+";
  if (modifiers & MOD_ALT) out += "Alt+";
  if (modifiers & MOD_SHIFT) out += "Shift+";
  if (modifiers & MOD_WIN) out += "Win+";
  out += key_name_from_virtual(virtual_key);
  return out;
}

std::string executable_path() {
  char buffer[MAX_PATH * 2] = {0};
  DWORD length = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
  return std::string(buffer, length);
}

std::string executable_directory() {
  std::string path = executable_path();
  size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool webview2_available() {
  static const bool available = [] {
    // A probe fakes a machine without the runtime.
    if (GetEnvironmentVariableA("BUFFRX_TEST_NO_WEBVIEW2", nullptr, 0) != 0) return false;
    // The editor is a WebView2 window. Without that runtime Tauri cannot create an environment and the process
    // dies a moment after it starts, which used to happen after the panel had already stepped aside. The loader
    // shipped beside this exe answers from whatever runtime Windows has installed. Loaded by full path: a bare
    // name makes Windows search the working directory too, and a portable copy missing its loader would then
    // run a stranger's DLL inside BuffrX.
    const std::string loader_path = executable_directory() + "\\WebView2Loader.dll";
    HMODULE loader = LoadLibraryA(loader_path.c_str());
    if (!loader) return false;
    using VersionFn = HRESULT(WINAPI*)(PCWSTR, LPWSTR*);
    auto version = reinterpret_cast<VersionFn>(
        reinterpret_cast<void*>(GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString")));
    if (!version) return false;
    LPWSTR text = nullptr;
    const bool present = buffrx::webview2_present(static_cast<long>(version(nullptr, &text)), text);
    if (text) CoTaskMemFree(text);
    return present;  // the loader stays loaded: the answer is cached for the run
  }();
  return available;
}

void notice_editor_needs_webview2() {
  fatal_notice(
      "The editor needs a free Microsoft part called WebView2, and this PC does not have it.\n\n"
      "Everything else still works: buffering, clips, screenshots and the panel.",
      "https://go.microsoft.com/fwlink/p/?LinkId=2124703");
}

bool launch_editor(const std::vector<std::string>& files, const std::string& data_root, DWORD& pid, bool preview, HWND parent) {
  // Checked here as well as at the panel, so every path to the editor - a card's menu, the tray, the page -
  // fails with a reason instead of a process that starts and dies.
  if (!webview2_available()) {
    notice_editor_needs_webview2();
    return false;
  }
  // The editor is the UI binary in its editor role: its own process, ended with its window.
  const std::string path = executable_directory() + "\\buffrx-ui.exe";
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    BX_LOG_WARN("editor binary not found at %s", path.c_str());
    return false;
  }
  std::string command = "\"" + path + "\" --editor";
  if (parent) command += " --parent " + std::to_string(reinterpret_cast<uintptr_t>(parent));
  if (preview) command += " --preview";
  for (const auto& file : files) {
    // A path with a quote in it would end the argument early and turn the rest into flags of its own.
    // Windows cannot make such a name, but a path that never reached the file system can still say one.
    if (file.find('"') != std::string::npos) {
      BX_LOG_WARN("not opening in the editor, its path has a quote in it: %s", file.c_str());
      continue;
    }
    command += " \"" + file + "\"";
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessA(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    BX_LOG_ERROR("could not launch the editor: %lu", GetLastError());
    return false;
  }
  pid = process.dwProcessId;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  (void)data_root;
  return true;
}

bool launch_timeline(const std::string& session, void*& process, DWORD& pid) {
  process = nullptr;
  pid = 0;
  // Every path that spawns buffrx-ui.exe asks this first: the editor at launch_editor, the preview here. A
  // preview started without the runtime dies the moment it builds its window, and the caller would be told
  // it opened.
  if (!webview2_available()) {
    notice_editor_needs_webview2();
    return false;
  }
  const std::string path = executable_directory() + "\\buffrx-ui.exe";
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    BX_LOG_WARN("timeline binary not found at %s", path.c_str());
    return false;
  }
  // The session id was checked as a directory name before it got here: no quote can reach the command line.
  // No session is an empty Timeline: the preview opens on its "nothing recorded yet" state and adopts the
  // first session that appears.
  std::string command = "\"" + path + "\" --timeline";
  if (!session.empty()) command += " \"" + session + "\"";
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessA(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
    BX_LOG_ERROR("could not launch the timeline preview: %lu", GetLastError());
    return false;
  }
  CloseHandle(info.hThread);
  process = info.hProcess;
  pid = info.dwProcessId;
  return true;
}

PlatformWindow::~PlatformWindow() {
  if (window_) WTSUnRegisterSessionNotification(window_);
  remove_tray_icon();
  if (window_) DestroyWindow(window_);
}

bool PlatformWindow::create(const std::string& title, ActionHandler on_action, CommandHandler on_command) {
  on_action_ = std::move(on_action);
  on_command_ = std::move(on_command);
  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = &PlatformWindow::window_proc;
  cls.hInstance = GetModuleHandleA(nullptr);
  cls.lpszClassName = kWindowClass;
  RegisterClassExW(&cls);
  window_ = CreateWindowExW(0, kWindowClass, wide_path(title).c_str(), WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                            GetModuleHandleA(nullptr), this);
  if (!window_) {
    BX_LOG_ERROR("could not create the message window: %lu", GetLastError());
    return false;
  }
  // The message window only sees WM_WTSSESSION_CHANGE if it asks for it: without this call the
  // pause-on-lock setting silently does nothing.
  if (!WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION)) {
    BX_LOG_WARN("session lock notifications unavailable: pause-on-lock will not fire (%lu)", GetLastError());
  }
  thread_ = GetCurrentThreadId();
  // Bindings given before the window existed are registered now, on this window.
  std::vector<HotkeyBinding> pending;
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    pending = bindings_;
  }
  if (!pending.empty()) apply_hotkeys(pending);
  return true;
}

LRESULT CALLBACK PlatformWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return TRUE;
  }
  auto* self = reinterpret_cast<PlatformWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self) return self->handle_message(message, wparam, lparam);
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT PlatformWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_MEASUREITEM && lparam && reinterpret_cast<MEASUREITEMSTRUCT*>(lparam)->CtlType == ODT_MENU) {
    measure_menu_item(reinterpret_cast<MEASUREITEMSTRUCT*>(lparam));
    return TRUE;
  }
  if (message == WM_DRAWITEM && lparam && reinterpret_cast<DRAWITEMSTRUCT*>(lparam)->CtlType == ODT_MENU) {
    draw_menu_item(reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
    return TRUE;
  }
  if (message == kApplyHotkeysMessage) {
    std::unique_ptr<std::vector<HotkeyBinding>> bindings(reinterpret_cast<std::vector<HotkeyBinding>*>(lparam));
    if (bindings) apply_hotkeys(*bindings);
    return 0;
  }
  if (message == kSuspendHotkeysMessage) {
    suspend_hotkeys(wparam != 0);
    return 0;
  }
  if (message == WM_HOTKEY) {
    // One id per action (its position in the list), never per key: Alt+1 and Ctrl+Alt+1 share the
    // key "1", and a key-based id made the second registration fail as "in use by another
    // application". The action runs outside the lock (it takes the engine lock).
    std::string action;
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      const size_t index = static_cast<size_t>(wparam) - kHotkeyBaseId;
      if (index < bindings_.size() && bindings_[index].registered) action = bindings_[index].action;
    }
    if (!action.empty() && on_action_) {
      // The action reaches into the engine (a recording, a config change). An exception here runs
      // on the main thread but outside the tick's guard, and an exception on a thread ends the
      // process - so it is caught here.
      try {
        on_action_(action);
      } catch (const std::exception& failure) {
        BX_LOG_ERROR("the hotkey action '%s' failed: %s", action.c_str(), failure.what());
      } catch (...) {
        BX_LOG_ERROR("the hotkey action '%s' failed with an unknown error", action.c_str());
      }
    }
    return 0;
  }
  if (message == WM_POWERBROADCAST) {
    // Sleep and wake. The recovery is a full pipeline rebuild - seconds, and under the engine lock - so this
    // thread only reports what happened; the handler queues the action, exactly as a hotkey does.
    if (wparam == PBT_APMSUSPEND) {
      if (power_handler_) {
        try {
          power_handler_(false);
        } catch (...) {
        }
      }
    } else if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
      if (power_handler_) {
        try {
          power_handler_(true);
        } catch (...) {
        }
      }
    }
    return TRUE;
  }
  if (message == kTrayCallbackMessage) {
    switch (LOWORD(lparam)) {
      case WM_LBUTTONUP:
        if (on_command_) {
          try {
            on_command_(kMenuTrayPanel);  // left click: the quick tray panel
          } catch (const std::exception& failure) {
            BX_LOG_ERROR("the tray command failed: %s", failure.what());
          } catch (...) {
            BX_LOG_ERROR("the tray command failed with an unknown error");
          }
        }
        return 0;
      case WM_RBUTTONUP:
      case WM_CONTEXTMENU: {
        show_tray_menu();
        return 0;
      }
      default:
        return 0;
    }
  }
  if (message == WM_ENDSESSION) {
    // Windows is ending the session: nothing may block the logoff with a box, and the engine has to let go of
    // its outputs the way Quit does. wParam FALSE means the shutdown was cancelled after all, so nothing ends.
    if (wparam == FALSE) return 0;
    if (on_command_) on_command_(kMenuQuitNow);
    PostQuitMessage(0);
    return 0;
  }
  if (message == WM_WTSSESSION_CHANGE) {
    // Only a lock and an unlock are a lock state. Logoff, disconnect and logon are not: treating them as an
    // unlock started the buffer on a machine nobody was sitting at.
    const int command = session_lock_command(static_cast<unsigned long>(wparam));
    if (command != 0 && on_command_) on_command_(command);
    return 0;
  }
  if (message == WM_TIMER && wparam == kMenuHeartbeatTimer) {
    // The tray menu's own loop has the thread: the tick and the status still run, as they do through a drag.
    if (modal_heartbeat_) {
      try {
        modal_heartbeat_();
      } catch (...) {
      }
    }
    return 0;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

void PlatformWindow::apply_hotkeys(const std::vector<HotkeyBinding>& bindings) {
  if (!window_) {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    bindings_ = bindings;
    for (auto& binding : bindings_) binding.registered = false;
    return;
  }
  if (GetCurrentThreadId() != thread_) {
    PostMessageW(window_, kApplyHotkeysMessage, 0, reinterpret_cast<LPARAM>(new std::vector<HotkeyBinding>(bindings)));
    return;
  }
  std::lock_guard<std::mutex> lock(bindings_mutex_);
  for (size_t i = 0; i < bindings_.size(); ++i) {
    if (bindings_[i].registered) UnregisterHotKey(window_, static_cast<int>(kHotkeyBaseId + i));
  }
  bindings_ = bindings;
  for (size_t i = 0; i < bindings_.size(); ++i) {
    HotkeyBinding& binding = bindings_[i];
    binding.registered = false;
    // BUFFRX_NO_HOTKEYS: a throwaway engine for a probe must not take the chords from the person's own engine.
    if (!binding.valid || suspended_ || std::getenv("BUFFRX_NO_HOTKEYS")) continue;
    if (RegisterHotKey(window_, static_cast<int>(kHotkeyBaseId + i), binding.modifiers, binding.virtual_key)) {
      binding.registered = true;
    } else {
      // Another application holds the chord right now; retry_pending picks it up later.
      binding.problem = "in use by another application";
      BX_LOG_WARN("hotkey %s (%s) unavailable: %lu", binding.chord.c_str(), binding.action.c_str(), GetLastError());
    }
  }
}

void PlatformWindow::suspend_hotkeys(bool suspended) {
  if (window_ && GetCurrentThreadId() != thread_) {
    PostMessageW(window_, kSuspendHotkeysMessage, suspended ? 1 : 0, 0);
    return;
  }
  std::vector<HotkeyBinding> again;
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    if (suspended == suspended_) return;
    suspended_ = suspended;
    suspended_at_ = suspended ? GetTickCount() : 0;
    if (suspended) {
      for (size_t i = 0; i < bindings_.size(); ++i) {
        if (bindings_[i].registered) UnregisterHotKey(window_, static_cast<int>(kHotkeyBaseId + i));
        bindings_[i].registered = false;
      }
      return;
    }
    again = bindings_;
  }
  apply_hotkeys(again);
}

void PlatformWindow::retry_pending() {
  bool lapsed = false;
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    lapsed = suspended_ && GetTickCount() - suspended_at_ > 120000;
  }
  if (lapsed) {
    BX_LOG_WARN("hotkey suspension was never ended; registering the hotkeys again");
    suspend_hotkeys(false);
  }
  std::lock_guard<std::mutex> lock(bindings_mutex_);
  if (suspended_ || !window_ || std::getenv("BUFFRX_NO_HOTKEYS")) return;
  for (size_t i = 0; i < bindings_.size(); ++i) {
    HotkeyBinding& binding = bindings_[i];
    if (binding.registered || !binding.valid) continue;
    if (RegisterHotKey(window_, static_cast<int>(kHotkeyBaseId + i), binding.modifiers, binding.virtual_key)) {
      binding.registered = true;
      binding.problem.clear();
      BX_LOG_INFO("hotkey %s (%s) registered on retry", binding.chord.c_str(), binding.action.c_str());
    }
  }
}

std::vector<HotkeyBinding> PlatformWindow::hotkeys() const {
  std::lock_guard<std::mutex> lock(bindings_mutex_);
  return bindings_;
}

void PlatformWindow::add_tray_icon(const std::string& tooltip) {
  tooltip_ = tooltip.size() >= 128 ? tooltip.substr(0, 120) : tooltip;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window_;
  data.uID = 1;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = kTrayCallbackMessage;
  // The BuffrX icon from the exe's own resources, at the tray's size; the stock one only if that is missing.
  data.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
  if (!data.hIcon) data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  tray_base_icon_ = data.hIcon;
  wcsncpy_s(data.szTip, wide_path(tooltip_).c_str(), _TRUNCATE);
  tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
  if (!tray_added_) BX_LOG_WARN("tray icon could not be added");
}

void PlatformWindow::update_tray_tooltip(const std::string& tooltip) {
  if (!tray_added_ || tooltip == tooltip_) return;
  tooltip_ = tooltip.size() >= 128 ? tooltip.substr(0, 120) : tooltip;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window_;
  data.uID = 1;
  data.uFlags = NIF_TIP;
  wcsncpy_s(data.szTip, wide_path(tooltip_).c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &data);
}



namespace {

// `base` with a dot in its lower right corner: the icon's own pixels read back, the dot laid over them with a dark rim so it
// shows on any taskbar, and a new icon made from the result. Null if anything fails (the plain icon then stays).
HICON icon_with_dot(HICON base, COLORREF color) {
  ICONINFO info{};
  if (!GetIconInfo(base, &info)) return nullptr;
  BITMAP bitmap{};
  GetObject(info.hbmColor, sizeof(bitmap), &bitmap);
  const int size = bitmap.bmWidth;
  HICON result = nullptr;
  if (size >= 8 && bitmap.bmHeight == size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    header.bmiHeader.biWidth = size;
    header.bmiHeader.biHeight = -size;  // top-down
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    const int rows = GetDIBits(dc, info.hbmColor, 0, static_cast<UINT>(size), pixels.data(), &header, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (rows == size) {
      const float radius = size * 0.26f;
      const float cx = size - radius - 0.5f, cy = size - radius - 0.5f;
      const uint32_t dot = 0xFF000000u | (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
      for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
          const float distance = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
          uint32_t& pixel = pixels[static_cast<size_t>(y) * size + x];
          if (distance <= radius + 1.2f) pixel = distance <= radius ? dot : 0xFF0B0C0Du;  // the dot, then its rim
        }
      }
      HBITMAP color_bitmap = CreateBitmap(size, size, 1, 32, pixels.data());
      ICONINFO made{};
      made.fIcon = TRUE;
      made.hbmMask = info.hbmMask;
      made.hbmColor = color_bitmap;
      result = CreateIconIndirect(&made);
      DeleteObject(color_bitmap);
    }
  }
  DeleteObject(info.hbmColor);
  DeleteObject(info.hbmMask);
  return result;
}

}  // namespace

void PlatformWindow::set_tray_activity(int level) {
  if (level == tray_level_) return;
  tray_level_ = level;
  if (!tray_added_ || !tray_base_icon_) return;
  HICON next = tray_base_icon_;
  HICON dotted = level == 0 ? nullptr : icon_with_dot(tray_base_icon_, level == 2 ? RGB(0xff, 0x3b, 0x4a) : RGB(0x3d, 0xd6, 0x6b));
  if (dotted) next = dotted;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window_;
  data.uID = 1;
  data.uFlags = NIF_ICON;
  data.hIcon = next;
  Shell_NotifyIconW(NIM_MODIFY, &data);
  if (tray_dot_icon_) DestroyIcon(tray_dot_icon_);
  tray_dot_icon_ = dotted;
}

void PlatformWindow::remove_tray_icon() {
  if (!tray_added_) return;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window_;
  data.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &data);
  tray_added_ = false;
}

void PlatformWindow::set_tray_state(const TrayMenuState& state) {
  std::lock_guard<std::mutex> lock(tray_state_mutex_);
  tray_state_ = state;
}

std::string PlatformWindow::chord_of(const std::string& action) const {
  std::lock_guard<std::mutex> lock(bindings_mutex_);
  for (const HotkeyBinding& binding : bindings_) {
    if (binding.action == action && binding.valid) return binding.chord;
  }
  return std::string();
}

namespace {

COLORREF colorref(uint32_t rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

std::wstring upper(std::wstring text) {
  if (!text.empty()) CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
  return text;
}

std::wstring clock_text(double seconds) {
  const int total = static_cast<int>(seconds < 0 ? 0 : seconds);
  wchar_t buffer[24];
  swprintf(buffer, 24, L"%d:%02d", total / 60, total % 60);
  return buffer;
}

}  // namespace

// The tray menu, in the theme's colours and the interface's face: what is running at the top, the things a player wants from the
// tray in the middle (each with its key), the two switches, then the folder and Quit. It is built from the engine's last snapshot
// (set_tray_state): the window thread never waits for the engine lock.
void PlatformWindow::show_tray_menu() {
  TrayMenuState state;
  {
    std::lock_guard<std::mutex> lock(tray_state_mutex_);
    state = tray_state_;
  }
  const bool running = state.recording;
  const ThemeColors& theme = theme_colors(state.theme);
  menu_colors_ = {colorref(theme.ground), colorref(theme.tile), colorref(theme.raised), colorref(theme.line), colorref(theme.fg),
                  colorref(theme.muted),  colorref(theme.acc),  colorref(theme.on),     colorref(theme.rec)};
  HDC screen = GetDC(nullptr);
  menu_dpi_ = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  const auto scaled = [&](int value) { return MulDiv(value, menu_dpi_, 96); };

  // JetBrains Mono, privately, from the runtime's own faces; whatever GDI picks if they are missing.
  if (!menu_fonts_added_ && !menu_font_dir_.empty()) {
    menu_fonts_added_ = true;
    for (const char* face : {"JetBrainsMonoNL-Medium.ttf", "JetBrainsMonoNL-Bold.ttf"}) {
      AddFontResourceExW(wide_path(menu_font_dir_ + "\\" + face).c_str(), FR_PRIVATE, nullptr);
    }
  }
  if (menu_font_) DeleteObject(menu_font_);
  if (menu_bold_) DeleteObject(menu_bold_);
  menu_font_ = CreateFontW(-scaled(12), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"JetBrains Mono NL");
  menu_bold_ = CreateFontW(-scaled(12), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"JetBrains Mono NL");

  const auto entry = [&](int command, const std::string& text, const std::string& action = std::string(), int kind = 0) {
    MenuEntry e;
    e.command = command;
    e.kind = kind;
    e.text = upper(wide_path(text));
    if (!action.empty()) e.right = upper(wide_path(chord_of(action)));
    return e;
  };
  const auto separator = [&] {
    MenuEntry e;
    e.kind = 1;
    return e;
  };
  menu_entries_.clear();
  MenuEntry header = entry(0, "BuffrX", "", 2);
  header.right = running ? L"REC " + clock_text(state.running_seconds)
                         : upper(wide_path(state.buffer_state == "on" ? "buffer on" : state.buffer_state == "off" ? "buffer off" : state.buffer_state));
  header.hot = running;
  menu_entries_.push_back(header);
  menu_entries_.push_back(entry(kMenuOpenUi, "Expand BuffrX"));
  menu_entries_.push_back(entry(kMenuOpenEditor, "Open editor"));
  menu_entries_.push_back(separator());
  menu_entries_.push_back(entry(kMenuSaveClip, "Save clip"));
  menu_entries_.push_back(entry(kMenuQuickClip, "Quick clip \xC2\xB7 " + std::to_string(state.quick_seconds) + "s"));
  menu_entries_.push_back(entry(kMenuScreenshot, "Screenshot"));
  MenuEntry record = entry(kMenuToggleRecording, running ? "Stop recording" : "Record");
  record.live = true;  // the red square
  if (running) {
    record.right = clock_text(state.running_seconds);
    record.hot = true;
  }
  menu_entries_.push_back(record);
  menu_entries_.push_back(entry(kMenuTimeline, "Timeline"));
  menu_entries_.push_back(separator());
  MenuEntry buffer = entry(kMenuToggleBuffer, "Buffer", "", 3);
  buffer.checked = state.buffer_state == "on" || state.buffer_state == "starting";
  menu_entries_.push_back(buffer);
  menu_entries_.push_back(separator());
  menu_entries_.push_back(entry(kMenuOpenClipsFolder, "Open clips folder"));
  menu_entries_.push_back(entry(kMenuQuit, "Quit"));

  // The widest line sets the menu's width.
  HDC dc = GetDC(window_);
  HGDIOBJ old = SelectObject(dc, menu_font_);
  int widest = 0;
  for (const MenuEntry& e : menu_entries_) {
    SIZE left{}, right{};
    GetTextExtentPoint32W(dc, e.text.c_str(), static_cast<int>(e.text.size()), &left);
    GetTextExtentPoint32W(dc, e.right.c_str(), static_cast<int>(e.right.size()), &right);
    widest = std::max<int>(widest, static_cast<int>(left.cx + right.cx) + (e.right.empty() ? 0 : scaled(28)));
  }
  SelectObject(dc, old);
  ReleaseDC(window_, dc);
  menu_width_ = std::max(scaled(190), widest + scaled(48));

  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  HBRUSH background = CreateSolidBrush(menu_colors_.tile);
  MENUINFO info{};
  info.cbSize = sizeof(info);
  info.fMask = MIM_BACKGROUND;
  info.hbrBack = background;
  SetMenuInfo(menu, &info);
  for (size_t i = 0; i < menu_entries_.size(); ++i) {
    const MenuEntry& e = menu_entries_[i];
    UINT flags = MF_OWNERDRAW;
    if (e.kind == 1 || e.kind == 2) flags |= MF_GRAYED;  // a header and a seam are not for choosing
    AppendMenuW(menu, flags, static_cast<UINT_PTR>(e.command), reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(i)));
  }
  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(window_);
  // TrackPopupMenu runs its own loop for as long as the menu is up. Without this the tick, the watchdog, the
  // game detection and the Timeline split polling stop with it; a timer is still dispatched to this window.
  SetTimer(window_, kMenuHeartbeatTimer, 100, nullptr);
  const int command = static_cast<int>(
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, window_, nullptr));
  KillTimer(window_, kMenuHeartbeatTimer);
  PostMessageW(window_, WM_NULL, 0, 0);  // the documented nudge that lets the menu close cleanly
  DestroyMenu(menu);
  DeleteObject(background);
  menu_entries_.clear();
  if (command != 0 && on_command_) on_command_(command);
}

void PlatformWindow::measure_menu_item(MEASUREITEMSTRUCT* item) {
  const size_t index = static_cast<size_t>(item->itemData);
  const int kind = index < menu_entries_.size() ? menu_entries_[index].kind : 0;
  const auto scaled = [&](int value) { return static_cast<UINT>(MulDiv(value, menu_dpi_, 96)); };
  item->itemWidth = static_cast<UINT>(menu_width_);
  item->itemHeight = kind == 1 ? scaled(7) : kind == 2 ? scaled(28) : scaled(25);
}

void PlatformWindow::draw_menu_item(DRAWITEMSTRUCT* item) {
  const size_t index = static_cast<size_t>(item->itemData);
  if (index >= menu_entries_.size()) return;
  const MenuEntry& e = menu_entries_[index];
  const MenuPalette& c = menu_colors_;
  const auto scaled = [&](int value) { return MulDiv(value, menu_dpi_, 96); };
  const HDC dc = item->hDC;
  const RECT r = item->rcItem;
  const bool selected = (item->itemState & ODS_SELECTED) != 0 && e.kind != 1 && e.kind != 2;
  const auto fill = [&](const RECT& area, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &area, brush);
    DeleteObject(brush);
  };
  fill(r, selected ? c.acc : e.kind == 2 ? c.ground : c.tile);
  if (e.kind == 1) {
    const int mid = r.top + (r.bottom - r.top) / 2;
    fill(RECT{r.left + scaled(12), mid, r.right - scaled(12), mid + 1}, c.line);
    return;
  }
  if (e.kind == 2) fill(RECT{r.left, r.bottom - 1, r.right, r.bottom}, c.line);
  SetBkMode(dc, TRANSPARENT);
  const COLORREF text_color = selected ? c.on : c.fg;
  const COLORREF quiet = selected ? c.on : c.muted;
  // The mark in the left column: a switch is a filled square when on and a hollow box when off; Record is the red square.
  const int box = scaled(9);
  const int left_pad = scaled(12);
  const int left = r.left + left_pad;
  const int cy = (r.top + r.bottom) / 2;
  if (e.kind == 3) {
    if (e.checked) {
      fill(RECT{left, cy - box / 2, left + box, cy + box / 2}, selected ? c.on : c.acc);
    } else {
      const RECT mark{left, cy - box / 2, left + box, cy + box / 2};
      // Same stroke as the filled square, so the hollow box does not thin out at high DPI. Rectangle leaves the
      // right and bottom edges unpainted, so they step in by one to keep the box the same size.
      HPEN pen = CreatePen(PS_SOLID, scaled(2), selected ? c.on : c.muted);
      HGDIOBJ old_pen = SelectObject(dc, pen);
      HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, mark.left, mark.top, mark.right - 1, mark.bottom - 1);
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(pen);
    }
  } else if (e.kind == 0 && e.live) {
    fill(RECT{r.left + left_pad, cy - box / 2, r.left + left_pad + box, cy + box / 2}, selected ? c.on : c.rec);
  }
  HGDIOBJ old = SelectObject(dc, e.kind == 2 ? menu_bold_ : menu_font_);
  RECT text_area{r.left + (e.kind == 2 ? left_pad : scaled(32)), r.top, r.right - scaled(14), r.bottom};
  const UINT flags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
  SetTextColor(dc, text_color);
  DrawTextW(dc, e.text.c_str(), static_cast<int>(e.text.size()), &text_area, flags | DT_LEFT);
  if (!e.right.empty()) {
    SetTextColor(dc, e.hot && !selected ? c.rec : quiet);
    DrawTextW(dc, e.right.c_str(), static_cast<int>(e.right.size()), &text_area, flags | DT_RIGHT);
  }
  SelectObject(dc, old);
}

void PlatformWindow::notify(const std::string& title, const std::string& text, bool with_sound) {
  if (!tray_added_) return;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = window_;
  data.uID = 1;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = with_sound ? NIIF_INFO : (NIIF_INFO | NIIF_NOSOUND);
  wcsncpy_s(data.szInfoTitle, wide_path(title).c_str(), _TRUNCATE);
  wcsncpy_s(data.szInfo, wide_path(text).c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

namespace {
HANDLE g_instance_mutex = nullptr;
}  // namespace

bool acquire_single_instance() {
  g_instance_mutex = CreateMutexA(nullptr, FALSE, "Local\\BuffrX.Engine.SingleInstance");
  if (!g_instance_mutex) return true;
  if (GetLastError() == ERROR_ALREADY_EXISTS) return false;
  return true;
}

void release_single_instance() {
  if (g_instance_mutex) {
    CloseHandle(g_instance_mutex);
    g_instance_mutex = nullptr;
  }
}

namespace {
// The shell can block on a slow or disconnected path. Never call it from the window thread: a detached
// thread keeps the panel responsive even when Explorer takes seconds to answer.
void shell_open_async(const std::string& verb, const std::string& file, const std::string& parameters) {
  try {
    std::thread([verb, file, parameters]() {
      const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      ShellExecuteA(nullptr, verb.c_str(), file.c_str(), parameters.empty() ? nullptr : parameters.c_str(), nullptr,
                    SW_SHOWNORMAL);
      if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
  } catch (...) {
    ShellExecuteA(nullptr, verb.c_str(), file.c_str(), parameters.empty() ? nullptr : parameters.c_str(), nullptr,
                  SW_SHOWNORMAL);
  }
}
}  // namespace

void open_folder(const std::string& path) { shell_open_async("open", path, std::string()); }

void reveal_file(const std::string& path) {
  shell_open_async("open", "explorer.exe", "/select,\"" + path + "\"");
}

void open_file(const std::string& path) {
  shell_open_async("open", path, std::string());
}

void fatal_notice(const std::string& text, const std::string& url) {
  // A probe answers for itself: a box it never clicks would hang the run.
  if (GetEnvironmentVariableA("BUFFRX_TEST_NO_DIALOGS", nullptr, 0) != 0) return;
  if (url.empty()) {
    MessageBoxA(nullptr, text.c_str(), "BuffrX", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
    return;
  }
  const std::string message = text + "\n\nOpen the download page now?";
  if (MessageBoxA(nullptr, message.c_str(), "BuffrX", MB_ICONERROR | MB_YESNO | MB_SETFOREGROUND) == IDYES) {
    shell_open_async("open", url, std::string());
  }
}

namespace {

// BuffrX's own entropy: a blob sealed by another program on this account does not open as ours.
DATA_BLOB secret_entropy() {
  static const char kEntropy[] = "BuffrX share secret v1";
  return DATA_BLOB{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
}

}  // namespace

std::string protect_secret(const std::string& secret) {
  DATA_BLOB in{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
  DATA_BLOB entropy = secret_entropy();
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"BuffrX", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::string();
  DWORD length = 0;
  std::string text;
  if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length) && length > 0) {
    text.resize(length);
    if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text.data(), &length)) {
      text.resize(length);
    } else {
      text.clear();
    }
  }
  LocalFree(out.pbData);
  return text;
}

bool unprotect_secret(const std::string& sealed, std::string& secret) {
  secret.clear();
  DWORD length = 0;
  if (sealed.empty() ||
      !CryptStringToBinaryA(sealed.c_str(), static_cast<DWORD>(sealed.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr)) {
    return false;
  }
  std::string blob(length, '\0');
  if (!CryptStringToBinaryA(sealed.c_str(), static_cast<DWORD>(sealed.size()), CRYPT_STRING_BASE64,
                            reinterpret_cast<BYTE*>(blob.data()), &length, nullptr, nullptr)) {
    return false;
  }
  DATA_BLOB in{length, reinterpret_cast<BYTE*>(blob.data())};
  DATA_BLOB entropy = secret_entropy();
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
  secret.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return true;
}

namespace {

// One default string value under HKCU\Software\Classes, written only when it differs.
bool set_class_value(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
  HKEY handle = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + key).c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE,
                      nullptr, &handle, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t current[1024] = {0};
  DWORD size = sizeof(current) - sizeof(wchar_t);
  DWORD type = 0;
  const bool same = RegQueryValueExW(handle, name, nullptr, &type, reinterpret_cast<BYTE*>(current), &size) == ERROR_SUCCESS &&
                    type == REG_SZ && value == current;
  const bool ok = same || RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  RegCloseKey(handle);
  return ok;
}

}  // namespace

bool register_link_scheme(const std::string& engine_exe) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, engine_exe.c_str(), -1, nullptr, 0);
  std::wstring exe(static_cast<size_t>(std::max(0, length - 1)), L'\0');
  if (length > 1) MultiByteToWideChar(CP_UTF8, 0, engine_exe.c_str(), -1, exe.data(), length);
  // The URL is passed as one quoted argument; the engine refuses a command line that is anything more
  // than "--open-link <url>" (main.cpp), so a URL cannot smuggle in another flag.
  return set_class_value(L"buffrx", nullptr, L"URL:BuffrX link") && set_class_value(L"buffrx", L"URL Protocol", L"") &&
         set_class_value(L"buffrx\\DefaultIcon", nullptr, L"\"" + exe + L"\",0") &&
         set_class_value(L"buffrx\\shell\\open\\command", nullptr, L"\"" + exe + L"\" --open-link \"%1\"");
}

namespace {
// Kept out of the clipboard itself: the watcher reads the clipboard on its own thread of frames.
std::string& last_copied() {
  static std::string text;
  return text;
}
}  // namespace

bool copy_text_to_clipboard(const std::string& text) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (length <= 0) return false;
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(length) * sizeof(wchar_t));
  if (!memory) return false;
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, static_cast<wchar_t*>(GlobalLock(memory)), length);
  GlobalUnlock(memory);
  if (!OpenClipboard(nullptr)) {
    GlobalFree(memory);
    return false;
  }
  EmptyClipboard();
  const bool ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
  CloseClipboard();
  if (!ok) GlobalFree(memory);
  if (ok) last_copied() = text;
  return ok;
}

std::string last_copied_text() { return last_copied(); }

bool copy_files_to_clipboard(const std::vector<std::string>& files) {
  // CF_HDROP: a DROPFILES header followed by a double-null-terminated list of wide paths, the format
  // Explorer, Discord and every chat client paste as files.
  std::wstring list;
  for (const std::string& file : files) {
    list += wide_path(bx::native_path(file));  // the shell formats want the separator the shell uses
    list.push_back(L'\0');
  }
  list.push_back(L'\0');
  const size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!memory) return false;
  auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
  drop->pFiles = sizeof(DROPFILES);
  drop->fWide = TRUE;
  std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
  GlobalUnlock(memory);
  if (!OpenClipboard(nullptr)) {
    GlobalFree(memory);
    return false;
  }
  EmptyClipboard();
  const bool ok = SetClipboardData(CF_HDROP, memory) != nullptr;
  CloseClipboard();
  if (!ok) GlobalFree(memory);
  return ok;
}

namespace {
// The one picker: folders for the saves, a single *.exe for the games list.
std::string pick_path(const std::string& title, const std::string& start, bool folders) {
  std::string chosen;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | (folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST) | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(wide_path(title).c_str());
    if (!folders) {
      COMDLG_FILTERSPEC filter[] = {{L"Programs (*.exe)", L"*.exe"}, {L"All files (*.*)", L"*.*"}};
      dialog->SetFileTypes(2, filter);
      dialog->SetFileTypeIndex(1);
    }
    IShellItem* folder = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(wide_path(start).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
      dialog->SetFolder(folder);
      folder->Release();
    }
    if (SUCCEEDED(dialog->Show(GetForegroundWindow()))) {
      IShellItem* result = nullptr;
      if (SUCCEEDED(dialog->GetResult(&result))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          chosen = wide_to_utf8(path);
          CoTaskMemFree(path);
        }
        result->Release();
      }
    }
    dialog->Release();
  }
  if (SUCCEEDED(com)) CoUninitialize();
  return chosen;
}
}  // namespace

std::string pick_folder(const std::string& title, const std::string& start) {
  return pick_path(title, start, true);
}

std::string pick_executable(const std::string& title, const std::string& start) {
  return pick_path(title, start, false);
}

bool set_launch_on_startup(bool enabled, const std::string& exe_path) {
  HKEY key = nullptr;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  bool ok = true;
  if (enabled) {
    // --autostart: the engine starts buffering at sign-in and opens the panel unless "start in the
    // tray" is on.
    std::string startup_exe = exe_path;
    const size_t slash = exe_path.find_last_of("\\/");
    if (slash != std::string::npos) {
      const std::string candidate = exe_path.substr(0, slash) + "\\..\\..\\BuffrX.exe";
      char resolved[MAX_PATH * 2] = {};
      const DWORD length = GetFullPathNameA(candidate.c_str(), sizeof(resolved), resolved, nullptr);
      if (length > 0 && length < sizeof(resolved) && GetFileAttributesA(resolved) != INVALID_FILE_ATTRIBUTES)
        startup_exe = resolved;
    }
    const std::string value = "\"" + startup_exe + "\" --autostart";
    ok = RegSetValueExA(key, "BuffrX", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>(value.size() + 1)) == ERROR_SUCCESS;
  } else {
    ok = RegDeleteValueA(key, "BuffrX") == ERROR_SUCCESS || GetLastError() == ERROR_FILE_NOT_FOUND;
  }
  RegCloseKey(key);
  return ok;
}

bool process_is_elevated() {
  // The only honest question is whether this process's token is an administrator one; comparing
  // against the process's own token is what makes it true for a split token too.
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0;
  if (token) CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

DWORD relaunch_as_administrator() {
  std::vector<wchar_t> path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) return GetLastError() ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
  const std::wstring arguments = elevation_arguments(GetCommandLineW());
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"runas";
  info.lpFile = path.data();
  info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
  info.nShow = SW_HIDE;  // the engine is a console executable; its own windows are shown separately
  if (!ShellExecuteExW(&info)) return GetLastError();
  if (info.hProcess) CloseHandle(info.hProcess);
  return ERROR_SUCCESS;
}

bool on_battery_power() {
  SYSTEM_POWER_STATUS status{};
  if (!GetSystemPowerStatus(&status)) return false;
  return status.ACLineStatus == 0;
}

int64_t system_free_memory_mb() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return -1;
  return static_cast<int64_t>(status.ullAvailPhys) / (1024 * 1024);
}

namespace {

std::string process_path(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return "";
  char path[MAX_PATH * 2] = {0};
  DWORD length = sizeof(path);
  std::string out;
  if (QueryFullProcessImageNameA(process, 0, path, &length) && length > 0) out.assign(path, length);
  CloseHandle(process);
  return out;
}

BOOL CALLBACK collect_window_process(HWND window, LPARAM param) {
  if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  auto* paths = reinterpret_cast<std::vector<std::string>*>(param);
  const std::string path = pid != 0 ? process_path(pid) : "";
  if (!path.empty() && std::find(paths->begin(), paths->end(), path) == paths->end()) paths->push_back(path);
  return TRUE;
}

}  // namespace

std::vector<std::string> window_process_paths() {
  std::vector<std::string> paths;
  EnumWindows(&collect_window_process, reinterpret_cast<LPARAM>(&paths));
  return paths;
}

}  // namespace bx
