// The Linux interface host: GLFW windows and an OpenGL 3.3 context drawing the same Dear ImGui
// views the Windows build draws with D3D11. The panel is frameless and dragged by its own titlebar;
// the quick panel is a second, smaller window. Embedded WebView2 previews are absent; the panel shows
// its poster placeholder and the external editor opens clips.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ui_bridge.h"
#include "ui_theme.h"
#include "ui_images.h"
#include "ui_store.h"
#include "ui_views.h"

struct GLFWwindow;

namespace bx::ui {

class UiHost : public UiShell {
public:
  UiHost();
  ~UiHost() override;

  bool start(UiBridge::Handler handler, const std::string& data_root, const std::string& font_dir, int ui_scale_percent);
  void stop();

  void request(const std::string& view);
  void request_editor(const std::vector<std::string>& files);
  void push_event(const std::string& frame) {
    if (started_) bridge_.push_event(frame);
  }
  void frame();
  void wait(uint32_t idle_ms);
  void wake();
  Json frame_stats(bool reset);
  void note_pass(double ms, uint32_t message);
  const std::string& last_frame_report() const { return frame_report_; }
  void set_modal_heartbeat(std::function<void()> beat) { modal_heartbeat_ = std::move(beat); }
  uint32_t take_modal_ms() { return modal_ms_.exchange(0); }
  bool capture(const std::string& view, const std::string& png_path, std::string& error);
  bool any_visible() const;
  bool audio_meter_visible() const;
  void set_audio_levels(float desktop, float mic);
  void set_buffer_active(bool active) { buffer_active_.store(active, std::memory_order_relaxed); }
  bool foreground_is_ours() const;
  bool input(const Json& args, std::string& error);

  void minimise() override;
  void close_window() override;
  void show(const std::string& view) override;
  void no_drag(ImVec2 min, ImVec2 max) override;
  void drag_area(ImVec2 min, ImVec2 max) override;
  void capture_keys(bool on) override;
  bool take_key(bool& ctrl, bool& alt, bool& shift, bool& win, unsigned& virtual_key, bool& down) override;
  void pad_event(uint32_t pressed, uint32_t released);
  void save_layout() override;
  void check_update(bool force) override;
  void install_update() override;
  bool pinned() const override { return quick_pinned_; }
  void set_pinned(bool pinned) override;
  ClipPicture clip_picture(const std::string& file, const RECT& rect, bool covered) override;
  void clip_picture_close() override;

private:
  struct Window;
  struct KeyEvent {
    bool ctrl = false, alt = false, shift = false, win = false;
    unsigned key = 0;
    bool down = false;
  };

  Window* create_window(bool tray, int width, int height);
  void destroy_window(Window*& window);
  void draw_window(Window& window, bool for_capture);
  void process_requests();
  void apply_theme(Window& window);
  void load_layout();
  void save_layout_now();

  static void on_key(GLFWwindow* handle, int key, int scancode, int action, int mods);
  static void on_mouse_button(GLFWwindow* handle, int button, int action, int mods);
  static void on_cursor_pos(GLFWwindow* handle, double x, double y);
  static void on_close(GLFWwindow* handle);
  static void on_framebuffer_size(GLFWwindow* handle, int width, int height);

  UiBridge bridge_;
  UiStore store_{bridge_};
  ImageCache images_;
  PanelState panel_;
  bool fonts_ready_ = false;
  std::string data_root_, font_dir_;
  std::atomic<bool> started_{false};
  std::atomic<bool> buffer_active_{false};
  int ui_scale_percent_ = 100;
  float scale_ = 1.0f;

  Window* main_ = nullptr;
  Window* tray_ = nullptr;
  std::string pending_view_ = "main";
  std::vector<std::string> editor_files_;
  std::string capture_path_;
  bool quick_pinned_ = false;
  float quick_w_ = 0, quick_h_ = 0;

  std::mutex requests_mutex_;
  std::vector<std::string> requests_;
  std::mutex wake_mutex_;
  std::condition_variable wake_cv_;
  bool woken_ = false;

  // Titlebar dragging.
  ImVec2 drag_min_{0, 0}, drag_max_{0, 0};
  std::vector<std::pair<ImVec2, ImVec2>> no_drag_;
  bool dragging_ = false;
  double drag_start_x_ = 0, drag_start_y_ = 0, drag_offset_x_ = 0, drag_offset_y_ = 0;

  // Hotkey recording.
  bool capturing_keys_ = false;
  std::mutex keys_mutex_;
  std::deque<KeyEvent> keys_;

  std::atomic<uint32_t> passes_{0};
  std::atomic<uint32_t> modal_ms_{0};
  std::string frame_report_;
  uint64_t last_frame_ms_ = 0;
  float desktop_level_ = 0.0f, mic_level_ = 0.0f;
  std::function<void()> modal_heartbeat_;
};

}  // namespace bx::ui
