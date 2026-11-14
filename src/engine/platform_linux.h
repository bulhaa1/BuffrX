// Linux platform services the engine owns: global hotkeys, notifications, a GTK status-icon tray,
// single-instance control, folder actions, the editor launcher and the clipboard. Some desktops do not
// show a status icon; the panel also opens from the launcher, the toggle hotkey, or `--show`.
#pragma once

#include "linux/compat/windows.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bx {

struct HotkeyBinding {
  std::string action;
  std::string chord;
  unsigned modifiers = 0;
  unsigned virtual_key = 0;
  bool registered = false;
  bool valid = false;
  std::string problem;
};

bool parse_chord(const std::string& chord, unsigned& modifiers, unsigned& virtual_key, std::string& problem);
std::string format_chord(unsigned modifiers, unsigned virtual_key);

struct TrayMenuState {
  std::string theme = "clutch";
  std::string buffer_state = "off";
  bool recording = false;
  double running_seconds = 0;
  int quick_seconds = 20;
};

class PlatformWindow {
public:
  using ActionHandler = std::function<void(const std::string& action)>;
  using CommandHandler = std::function<void(int command)>;
  using PowerHandler = std::function<void(bool resumed)>;
  using ModalHeartbeat = std::function<void()>;

  PlatformWindow();
  ~PlatformWindow();

  bool create(const std::string& title, ActionHandler on_action, CommandHandler on_command);
  void apply_hotkeys(const std::vector<HotkeyBinding>& bindings);
  void retry_pending();
  void suspend_hotkeys(bool suspended);
  bool hotkeys_suspended() const { return suspended_; }
  std::vector<HotkeyBinding> hotkeys() const;

  void add_tray_icon(const std::string& tooltip);
  void update_tray_tooltip(const std::string& tooltip);
  void remove_tray_icon();
  void show_tray_menu();

  void notify(const std::string& title, const std::string& text, bool with_sound);
  void set_tray_activity(int level) { activity_ = level; }
  void set_tray_state(const TrayMenuState& state);
  void set_menu_font_dir(const std::string& dir) { menu_font_dir_ = dir; }
  void set_power_handler(PowerHandler handler) { power_handler_ = std::move(handler); }
  void set_modal_heartbeat(ModalHeartbeat handler) { modal_heartbeat_ = std::move(handler); }
  unsigned long handle() const;

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
    kMenuQuitNow = 14,
  };

  // The tray's GTK callbacks run on their own thread and hand commands to the engine through these.
  void tray_action(const std::string& action) { if (on_action_) on_action_(action); }
  void tray_command(int command) { if (on_command_) on_command_(command); }
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  ActionHandler on_action_;
  CommandHandler on_command_;
  PowerHandler power_handler_;
  ModalHeartbeat modal_heartbeat_;
  std::vector<HotkeyBinding> bindings_;
  mutable std::mutex bindings_mutex_;
  TrayMenuState tray_state_;
  std::string menu_font_dir_;
  std::string tooltip_;
  bool suspended_ = false;
  int activity_ = 0;
  bool tray_added_ = false;
};

bool acquire_single_instance();
void release_single_instance();

void open_folder(const std::string& path);
void reveal_file(const std::string& path);
void open_file(const std::string& path);
void fatal_notice(const std::string& text, const std::string& url = std::string());

bool webview2_available();
void notice_editor_needs_webview2();
bool launch_editor(const std::vector<std::string>& files, const std::string& data_root, DWORD& pid,
                   bool preview = false, void* parent = nullptr);
bool launch_timeline(const std::string& session, void*& process, DWORD& pid);

bool copy_text_to_clipboard(const std::string& text);
std::string last_copied_text();
bool copy_files_to_clipboard(const std::vector<std::string>& files);
std::string protect_secret(const std::string& secret);
bool unprotect_secret(const std::string& sealed, std::string& secret);

bool register_link_scheme(const std::string& engine_exe);

std::string pick_folder(const std::string& title, const std::string& start);
std::string pick_executable(const std::string& title, const std::string& start);

bool set_launch_on_startup(bool enabled, const std::string& exe_path);
bool on_battery_power();

int64_t system_free_memory_mb();
bool process_is_elevated();
unsigned long relaunch_as_administrator();
std::vector<std::string> window_process_paths();

std::string executable_path();
std::string executable_directory();

// The OBS ffmpeg muxer helper, staged out of the package and the shared /tmp namespace. The engine
// calls `stage_obs_muxer` before libobs starts; `obs_muxer_path` is the copy it made, or "".
bool stage_obs_muxer(const std::string& source, std::string& problem);
std::string obs_muxer_path();

}  // namespace bx
