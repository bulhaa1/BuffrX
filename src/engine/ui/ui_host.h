// The native interface's windows, inside the engine process: the panel (frameless, dragged by its
// titlebar, close hides it to the tray) and the tray panel by the tray icon, which is also the one
// small interface: the quick panel the tray icon and the panel's quick button open.
// The interface has its own small D3D11 device, separate from libobs's: it is created the first time a
// window opens and let go a few seconds after the last one closes, so a closed interface costs no GPU
// work and no texture memory (AGENTS.md: nothing drawn while no window is open).
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ui_bridge.h"
#include "ui_frame_stats.h"
#include "ui_images.h"
#include "ui_store.h"
#include "ui_views.h"
#include "ui_window_math.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace bx::ui {

struct UiWindow;

class UiHost : public UiShell {
public:
  UiHost();
  ~UiHost() override;

  // `handler` is Protocol::handle; `font_dir` holds the TTF faces; `data_root` keeps ui.json.
  bool start(UiBridge::Handler handler, const std::string& data_root, const std::string& font_dir, int ui_scale_percent);
  void stop();

  // From any thread: "main", "compact" (the quick panel), "tray" (toggle it), "toggle" (the
  // toggle-UI hotkey), "hide".
  void request(const std::string& view);
  void request_editor(const std::vector<std::string>& files);
  void push_event(const std::string& frame) {
    if (started_) bridge_.push_event(frame);  // before start (smoke tests, benchmarks) nothing drains it
  }
  // The main loop, every pass: windows draw only when visible and something changed or moves.
  void frame();
  // The main loop's wait between passes, in place of a fixed sleep. A window with frames still to draw is paced by the
  // display: the loop waits for the swap chain's next free frame (one vertical blank), so the picture follows the
  // monitor's refresh rate. Otherwise it sleeps up to `idle_ms` and wakes the moment a message, a request from another
  // thread or a reply arrives, so a click is not left waiting for the next pass.
  // `idle_ms` is the longest the loop may sleep (the caller's earliest deadline): a window on screen keeps the 20 ms pass,
  // with nothing on screen the interface adds only its own timers.
  void wait(uint32_t idle_ms);
  // From any thread: the loop has work.
  void wake();
  // From any thread: how steadily the windows draw, since the last reset (`ui.frame_stats`).
  Json frame_stats(bool reset);
  // Phase 0.3: one pass of the window thread (messages and frame) took `ms`; `message` is the last one it dispatched.
  void note_pass(double ms, uint32_t message);
  // The parts of the last frame that took noticeable time, for the window thread's stall log.
  const std::string& last_frame_report() const { return frame_report_; }
  // Called every 100 ms while Windows runs its own loop for a dragged or resized panel, when the
  // engine's main loop cannot come round: the tick and the status keep going.
  void set_modal_heartbeat(std::function<void()> beat) { modal_heartbeat_ = std::move(beat); }
  // From the window thread: the time spent in modal drag/size loops since the last call, which the stall check takes off the
  // pass that contained them (the heartbeat kept the tick running through them).
  uint32_t take_modal_ms() { return modal_ms_.exchange(0); }
  // From any thread: draws `view` ("main" | "tray" | "compact" | "settings:<section>", or "current" for
  // what is on screen, untouched) and writes it as a
  // PNG. Opens the window if it is closed. Waits for the main loop to do it.
  bool capture(const std::string& view, const std::string& png_path, std::string& error);
  bool any_visible() const;
  // Window thread only: the deck or Settings > Audio is visible and needs live levels.
  bool audio_meter_visible() const;
  void set_audio_levels(float desktop, float mic);
  // Called by the window-thread status tick, including while no window is visible.
  void set_buffer_active(bool active) { buffer_active_.store(active, std::memory_order_relaxed); }
  // From any thread: a BuffrX window (the panel or the tray panel) is visible, not minimized and is the window the
  // user is looking at right now. Only then is a desktop card redundant: a panel that is merely open - minimized,
  // behind a game - used to count as "in front" through any_visible() and swallowed the card and the chime.
  bool foreground_is_ours() const;
  // From any thread: input handed straight to a window's ImGui queue, never to the system cursor, so a
  // probe can click and type in the interface while someone uses the machine. `args`:
  // {"window":"main"|"tray", "click":[x,y]} | {"keys":["Escape",…]} | {"chord":"Alt+4"} | {"text":"…"}
  bool input(const Json& args, std::string& error);

  // UiShell
  void minimise() override;
  void close_window() override;
  void show(const std::string& view) override;
  void no_drag(ImVec2 min, ImVec2 max) override;
  void drag_area(ImVec2 min, ImVec2 max) override;
  void capture_keys(bool on) override;
  bool take_key(bool& ctrl, bool& alt, bool& shift, bool& win, unsigned& virtual_key, bool& down) override;
  // From another thread: controller button edges for the interface's own navigation.
  void pad_event(uint32_t pressed, uint32_t released);
  void save_layout() override;
  void check_update(bool force) override;
  void install_update() override;
  bool pinned() const override { return quick_pinned_; }
  void set_pinned(bool pinned) override;
  ClipPicture clip_picture(const std::string& file, const RECT& rect, bool covered) override;
  void clip_picture_close() override;

  static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

private:
  std::atomic<bool> buffer_active_{false};
  bool ensure_device();
  void release_device();
  UiWindow* open_window(bool tray);
  void destroy_window(std::unique_ptr<UiWindow>& window);
  void show_panel();
  void cancel_recorder();
  // The pad's buttons as the keys they stand for, queued into ImGui before the frame is built.
  void inject_pad_nav();
  void show_tray();
  void toggle_tray();
  void begin_hide(UiWindow& window, bool retire);
  void finish_hides();
  void render(UiWindow& window, bool for_capture, bool vsync = true);
  // Whether the display can take this window's next frame (always, for a window that is not paced).
  bool display_ready(UiWindow& window, bool strict = false);
  // With no window on screen: the sooner of the store's timers and the device's release.
  uint32_t hidden_deadline_ms();
  // An input reached a window: the time it waits for a frame is what `ui.frame_stats` calls latency, kept under its kind.
  void note_input(InputKind kind);
  void note_reasons(bool forced, bool store, bool dirty, bool moving, bool settle, const char* animation, bool images);
  void process_requests();
  // ui.share (the editor's preview): the panel comes forward with the Share sheet open on that file.
  void open_share_for(const std::string& file);
  // A BuffrX link a friend sent, copied to the clipboard: the panel offers to open it. Read only while
  // the panel is on screen, and only when the clipboard's sequence number says something new is there.
  void check_clipboard_for_link();
  unsigned long clipboard_seq_ = 0;
  // A clipboard another program held is not opened again on every pass: it is tried again after this time.
  uint32_t clipboard_retry_at_ = 0;
  std::string offered_link_;
  std::string received_preview_;
  uint32_t received_preview_until_ = 0;  // a received clip that never reaches a listing is dropped, not latched
  std::vector<std::string> editor_files_;
  HANDLE editor_process_ = nullptr;
  DWORD editor_pid_ = 0;
  RECT editor_return_rect_{};
  bool editor_return_quick_ = false;
  bool editor_mode_ = false;
  bool editor_loading_ = false;
  bool editor_returning_ = false;
  // An editor that is alive but has not put its window inside the panel by this time is not coming: the panel is let
  // go of it, and the process is ended after a grace period if it still runs (frame() does both, never this thread).
  uint32_t editor_loading_since_ = 0;
  HANDLE editor_end_process_ = nullptr;
  uint32_t editor_end_at_ = 0;
  void start_editor();
  void return_from_editor();
  void finish_editor();
  void size_editor();
  void request_editor_close();
  void load_layout();
  LRESULT handle(UiWindow& window, UINT message, WPARAM wparam, LPARAM lparam);

  UiBridge bridge_;
  UiStore store_;
  // The clip the panel plays (WebView2, on demand); released when the clip closes or the panel hides.
  WebClip web_clip_;
  ImageCache images_;
  // The sampled mark, made with the device and freed with it.
  ImTextureID mark_texture_ = 0;
  PanelState panel_;
  bool update_resume_checked_ = false;
  std::string data_root_, font_dir_;
  std::atomic<bool> started_{false};
  // any_visible() from the window thread's own view of `main_` and `tray_`, published for other
  // threads: the notification card asks whether the interface is already in front of the user, and it
  // runs on the save's worker. The window pointers are the window thread's alone.
  std::atomic<bool> windows_visible_{false};
  // The handles of the visible panel and tray panel, published with windows_visible_ for foreground_is_ours().
  std::atomic<HWND> published_main_{nullptr};
  std::atomic<HWND> published_tray_{nullptr};

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  // A present or a resize found the graphics device gone (a driver reset, a TDR): the next pass rebuilds it, its swap
  // chains and the atlases instead of drawing into a device that no longer answers.
  bool device_lost_ = false;
  std::unique_ptr<UiWindow> main_, tray_;
  UiWindow* drawing_ = nullptr;  // the window a view is drawing into right now
  // The visible window that still has frames to draw and is paced by the display: wait() waits for its next frame.
  UiWindow* paced_window_ = nullptr;
  bool game_running_ = false;  // a fullscreen game runs while a window is open: frames are capped, not display-paced
  uint32_t game_checked_ms_ = 0;
  std::atomic<bool> game_flag_{false};  // game_running_, for ui.frame_stats
  HANDLE wake_event_ = nullptr;  // set by wake(); what the loop's idle wait listens to besides messages
  std::atomic<bool> paced_{false};  // a visible window has a display-paced swap chain (for ui.frame_stats)
  std::atomic<bool> focused_{false};  // a visible window of ours is the foreground window: the pointer frames follow the display (ui.frame_stats)
  std::atomic<int64_t> passes_{0};  // main-loop passes since the last ui.frame_stats reset: a busy wait would show here
  mutable std::mutex stats_mutex_;
  FrameStats frame_stats_;
  // Phase 0.3 hitches: passes over 12 ms and over 500 ms, and the worst pass with its message and frame report.
  int hitches_over_12_ = 0, hitches_over_500_ = 0;
  double worst_pass_ms_ = 0;
  uint32_t worst_message_ = 0;
  std::string worst_report_;
  // Phase 0.1: where the last window open spent its time, from the call to the end of the backend setup (ms).
  double open_marks_[4] = {0, 0, 0, 0};
  int64_t open_t0_us_ = 0;
  uint32_t hidden_since_ = 0;
  uint32_t wait_hint_ms_ = 20;  // the sooner the next pass wants (a capped frame's remaining wait), set by frame()
  // Where the last frame's time went, for the stall log: "fonts 640 ms, open 120 ms, upload 3 ms".
  std::string frame_report_;
  uint32_t size_move_started_ = 0;
  std::atomic<uint32_t> modal_ms_{0};
  int64_t move_t0_us_ = 0, last_move_us_ = 0;
  std::array<int, 16> move_counts_{};
  std::array<int64_t, 16> move_worst_us_{};
  uint32_t beat_worst_ms_ = 0;  // the slowest modal heartbeat of the current drag: each one holds the drag still while it runs
  std::function<void()> modal_heartbeat_;
  std::set<ImWchar> glyphs_;     // characters the library needs beyond the base set
  int glyphs_library_version_ = -1;
  std::string glyphs_target_;

  // Placement, remembered in ui.json.
  RECT panel_rect_{0, 0, 0, 0};
  int ui_scale_percent_ = 100;
  // The quick panel's own place, once the user has dragged it: after that it opens where they left it
  // instead of at the tray.
  POINT quick_pos_{-1, -1};
  bool quick_moved_ = false;
  // Its size, in design units (ui_window_math.h gives the default and the limits): kept when the user sizes it and
  // across restarts and monitors.
  float quick_w_ = kQuickWidth, quick_h_ = kQuickHeight;
  bool quick_aside_ = false;   // pinned, but stepped aside for a fullscreen game
  uint32_t pinned_checked_ms_ = 0;
  bool quick_pinned_ = false;  // above every other window; off by default (2026-10-31)
  // Windows that are closed but not yet destroyed: a window may not be destroyed while it draws.
  bool retire_main_ = false, retire_tray_ = false;

  // Cross-thread requests.
  mutable std::mutex requests_mutex_;
  std::vector<std::string> requests_;
  struct Capture {
    std::string view, path, error;
    bool pending = false, done = false, ok = false;
  };
  Capture capture_;
  std::condition_variable capture_done_;
  // An input carries the time it was asked for: one for a window that is not up yet waits a moment instead of being
  // dropped (a probe's ui.show and its click in one breath), and never so long that it fires at a stale picture.
  struct InputRequest {
    Json args;
    uint32_t queued_ms = 0;
  };
  std::vector<InputRequest> inputs_;
  void apply_inputs();

  // The hotkey recorder's keys.
  bool capturing_keys_ = false;
  struct Key {
    bool ctrl, alt, shift, win;
    unsigned vk;
    bool down;
  };
  std::vector<Key> keys_;
  // Controller navigation: edges from the poller's thread, held buttons, and the held direction's repeat.
  std::mutex pad_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> pad_events_;
  // A pad edge arrived and no frame has drawn it yet: the next pass draws at once instead of waiting
  // for something else to change. Without it a press sat until an unrelated repaint, which read as lag.
  std::atomic<bool> pad_pending_{false};
  // When the last pad edge arrived, so a press that reaches the picture late can name itself in the log.
  std::atomic<int64_t> pad_edge_ms_{0};
  int64_t pad_log_ms_ = 0;
  bool pad_nav_on_ = false;
  uint32_t pad_nav_held_ = 0;
  uint32_t pad_nav_dir_ = 0;
  int64_t pad_nav_first_ms_ = 0;
  int64_t pad_nav_last_ms_ = 0;
  // Set by an injected wheel and cleared after the frame it applies to, so ui.input can scroll the
  // panel (which otherwise ignores the mouse wheel by design) for exactly the frames that ask.
  bool wheel_this_frame_ = false;
};

}  // namespace bx::ui
