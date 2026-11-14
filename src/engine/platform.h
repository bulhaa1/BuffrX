// Windows platform services the engine owns: the tray icon, global hotkeys, notifications,
// single-instance control, folder actions and the UI launcher.
//
// All of it lives in the engine process on purpose. Closing the UI must not stop capture, and capture
// must not depend on a window being open.
#pragma once

#ifndef _WIN32
#include "platform_linux.h"
#else

#include <windows.h>

#include <functional>
#include <map>
#include <string>
#include <mutex>
#include <vector>

namespace bx {

struct HotkeyBinding {
  std::string action;
  std::string chord;
  UINT modifiers = 0;
  UINT virtual_key = 0;
  bool registered = false;
  bool valid = false;
  std::string problem;
};

// Parses a chord like "Alt+C" or "Ctrl+Shift+F10" into modifiers and a virtual key.
bool parse_chord(const std::string& chord, UINT& modifiers, UINT& virtual_key, std::string& problem);
std::string format_chord(UINT modifiers, UINT virtual_key);

// WTS_SESSION_LOCK and WTS_SESSION_UNLOCK, named here because this header cannot take wtsapi32.h.
constexpr unsigned long kWtsSessionLock = 0x7;
constexpr unsigned long kWtsSessionUnlock = 0x8;

// The command a WM_WTSSESSION_CHANGE wParam means: 100 the session locked, 101 it unlocked, 0 nothing.
// Only these two touch the buffer; a logoff, a disconnect or a logon is not a lock state.
inline int session_lock_command(unsigned long change) {
  if (change == kWtsSessionLock) return 100;
  if (change == kWtsSessionUnlock) return 101;
  return 0;
}

// What the tray menu shows, handed over by the engine's tick: the menu is drawn in the theme's colours and says what is
// running, so it is built from this snapshot and never asks the engine (the window thread must not wait for its lock).
struct TrayMenuState {
  std::string theme = "clutch";      // a themes.h id
  std::string buffer_state = "off";  // off, starting, on, stopping
  bool recording = false;            // a manual recording runs
  double running_seconds = 0;        // how long that recording or clip has run
  int quick_seconds = 20;
};

class PlatformWindow {
public:
  using ActionHandler = std::function<void(const std::string& action)>;
  using CommandHandler = std::function<void(int command)>;
  // Sleep and wake, from WM_POWERBROADCAST: false when the machine goes to sleep, true when it is back.
  // The handler must only queue work - a recovery rebuild takes seconds and the engine lock.
  using PowerHandler = std::function<void(bool resumed)>;
  // Called every 100 ms while the tray menu holds the thread (TrackPopupMenu's own loop still dispatches a
  // timer): the tick and the status keep going, exactly as they do through a panel drag.
  using ModalHeartbeat = std::function<void()>;

  ~PlatformWindow();

  // Creates the hidden window that receives hotkeys, tray messages and lock/display notifications.
  bool create(const std::string& title, ActionHandler on_action, CommandHandler on_command);

  // Registers every binding; chords held by another application are retried by retry_pending().
  void apply_hotkeys(const std::vector<HotkeyBinding>& bindings);
  // Re-registers any chord that could not be taken earlier. Called from the tick loop.
  void retry_pending();
  // While the interface records a new chord, every hotkey is let go, so pressing the old chord to
  // rebind it does not also save a clip. Resuming registers them all again.
  void suspend_hotkeys(bool suspended);
  // A deliberate pause is not a failure: the interface hides "not registered" while it records.
  bool hotkeys_suspended() const { return suspended_; }
  std::vector<HotkeyBinding> hotkeys() const;

  void add_tray_icon(const std::string& tooltip);
  void update_tray_tooltip(const std::string& tooltip);
  void remove_tray_icon();
  void show_tray_menu();

  void notify(const std::string& title, const std::string& text, bool with_sound);

  // The tray icon wears a small dot while something runs: 0 none, 1 buffering (green), 2 recording (red). Window thread.
  void set_tray_activity(int level);
  // Any thread: the snapshot the next tray menu is built from. The font directory holds the interface's faces for its text.
  void set_tray_state(const TrayMenuState& state);
  void set_menu_font_dir(const std::string& dir) { menu_font_dir_ = dir; }
  void set_power_handler(PowerHandler handler) { power_handler_ = std::move(handler); }
  void set_modal_heartbeat(ModalHeartbeat handler) { modal_heartbeat_ = std::move(handler); }

  HWND handle() const { return window_; }

  // The alpha's tray menu commands.
  enum MenuCommand {
    kMenuOpenUi = 1,
    kMenuSaveClip = 2,
    kMenuQuickClip = 3,
    kMenuScreenshot = 4,
    kMenuToggleRecording = 5,
    kMenuOpenClipsFolder = 6,
    kMenuQuit = 8,
    kMenuTrayPanel = 9,
    kMenuOpenEditor = 10,
    kMenuToggleBuffer = 11,
    kMenuTimeline = 13,
    kMenuQuitNow = 14,  // Windows is ending the session: quit with no confirmation box
  };

private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

  // The themed tray menu: owner-drawn, so its colours are the theme's. The entries live here while the menu is up.
  struct MenuEntry {
    int command = 0;
    int kind = 0;  // 0 item, 1 separator, 2 header, 3 switch
    std::wstring text, right;
    bool checked = false, live = false, hot = false;  // live: the red square; hot: the right-hand text is in the recording colour
  };
  struct MenuPalette {
    COLORREF ground = 0, tile = 0, raised = 0, line = 0, fg = 0, muted = 0, acc = 0, on = 0, rec = 0;
  };
  void measure_menu_item(MEASUREITEMSTRUCT* item);
  void draw_menu_item(DRAWITEMSTRUCT* item);
  std::string chord_of(const std::string& action) const;
  std::vector<MenuEntry> menu_entries_;
  MenuPalette menu_colors_;
  HFONT menu_font_ = nullptr, menu_bold_ = nullptr;
  int menu_dpi_ = 96;
  int menu_width_ = 0;
  std::string menu_font_dir_;
  bool menu_fonts_added_ = false;
  mutable std::mutex tray_state_mutex_;
  TrayMenuState tray_state_;

  HWND window_ = nullptr;
  ActionHandler on_action_;
  CommandHandler on_command_;
  PowerHandler power_handler_;
  ModalHeartbeat modal_heartbeat_;
  std::vector<HotkeyBinding> bindings_;
  bool tray_added_ = false;
  int tray_level_ = 0;
  HICON tray_base_icon_ = nullptr, tray_dot_icon_ = nullptr;
  bool suspended_ = false;
  uint32_t suspended_at_ = 0;  // a suspension the interface never ended (it closed) lapses after 2 min
  DWORD thread_ = 0;                  // the thread that owns window_ (and so the hotkeys)
  mutable std::mutex bindings_mutex_;  // bindings_ is read by pipe threads
  std::string tooltip_;
};

// True when this process created the engine mutex, meaning it is the primary instance.
bool acquire_single_instance();
void release_single_instance();

void open_folder(const std::string& path);
void reveal_file(const std::string& path);
void open_file(const std::string& path);

// A failure the person has to see: a message box with the facts, and - when `url` is given - a Yes button
// that opens the page where the missing piece is installed. A failure that stops BuffrX must never exist
// only as a line in engine.log. BUFFRX_TEST_NO_DIALOGS=1 answers No at once, so a probe never waits on a click.
void fatal_notice(const std::string& text, const std::string& url = std::string());

// Whether the editor can run at all: it is a WebView2 window, and that runtime is a separate download
// Windows usually has. Cached for the run; false when BUFFRX_TEST_NO_WEBVIEW2 is set.
bool webview2_available();
// Says the editor needs the WebView2 runtime, with a Yes button that opens the download page. Shown before
// any window is hidden, so a machine without it is told why instead of watching the panel disappear.
void notice_editor_needs_webview2();

// Launches the editor process with the given clip files.
// `preview`: the clip plays first (the panel draws no video); Edit turns the window into the editor.
bool launch_editor(const std::vector<std::string>& files, const std::string& data_root, DWORD& pid, bool preview = false, HWND parent = nullptr);
// Starts the Timeline preview (buffrx-ui.exe --timeline <session>): the on-demand replay window, its own
// process so closing it frees the player's memory. `process` is the handle (the caller closes it) and `pid`
// what the writer's foreground exemption compares against.
bool launch_timeline(const std::string& session, void*& process, DWORD& pid);

// Puts text on the clipboard (CF_UNICODETEXT). `text` is UTF-8.
bool copy_text_to_clipboard(const std::string& text);
// The last text this process wrote to the clipboard, empty when it wrote none. A link this PC copied
// comes back through the clipboard watcher, and offering it to the person who just copied it is wrong.
std::string last_copied_text();
// Puts files on the clipboard as files (CF_HDROP), so they paste into Explorer, Discord or a chat.
bool copy_files_to_clipboard(const std::vector<std::string>& files);
// A secret sealed to this Windows account (DPAPI, with BuffrX's own entropy), as base64 for a JSON
// file: another account, another machine or a copy of the file cannot open it. "" when sealing fails.
std::string protect_secret(const std::string& secret);
// Opens what protect_secret sealed. False for anything else, including a seal that was changed.
bool unprotect_secret(const std::string& sealed, std::string& secret);

// Points buffrx:// at this engine for the current user (HKCU\Software\Classes\buffrx), so a click on a
// BuffrX link starts it with "--open-link <url>". Rewritten only when it differs.
bool register_link_scheme(const std::string& engine_exe);

// The system folder picker; "" when cancelled. Paths are UTF-8.
std::string pick_folder(const std::string& title, const std::string& start);
// The system picker for one program: only *.exe, "" when cancelled.
std::string pick_executable(const std::string& title, const std::string& start);

bool set_launch_on_startup(bool enabled, const std::string& exe_path);
bool on_battery_power();

// How much physical memory the machine has free, in MB, or -1 when Windows will not say. The
// capture needs a few hundred megabytes for its frame pool, and a machine with nothing left does
// not fail politely: libobs faults inside the graphics device's creation.
int64_t system_free_memory_mb();

// Whether this process is running elevated. True when the engine was started with administrator
// rights, which is what an anti-cheat title and a game an installer launched as admin both need.
// Reported on status.get so the interface can show the state that is true rather than the setting
// that will apply at the next launch.
bool process_is_elevated();

// Ask Windows to start this executable with its original arguments and an administrator token.
// Returns ERROR_SUCCESS, ERROR_CANCELLED if the user declined, or another Win32 error.
DWORD relaunch_as_administrator();
// Full image paths of the processes owning visible top-level windows, each once.
std::vector<std::string> window_process_paths();

std::string executable_path();
std::string executable_directory();

}  // namespace bx

#endif  // _WIN32
