#include "ui_host_linux.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../shared/log.h"
#include "../platform_linux.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ui_parts.h"
#include "ui_pad_nav.h"
#include "ui_theme.h"
#include "ui_window_math.h"


namespace bx::ui {

namespace {
constexpr float kUiScale = 1.2f;
constexpr uint32_t kReleaseAfterMs = 10000;

uint32_t now_ms() { return static_cast<uint32_t>(GetTickCount()); }

unsigned virtual_key_from_glfw(int key) {
  if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return static_cast<unsigned>('A' + (key - GLFW_KEY_A));
  if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) return static_cast<unsigned>('0' + (key - GLFW_KEY_0));
  if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24) return static_cast<unsigned>(VK_F1 + (key - GLFW_KEY_F1));
  if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return static_cast<unsigned>(VK_NUMPAD0 + (key - GLFW_KEY_KP_0));
  switch (key) {
    case GLFW_KEY_SPACE: return VK_SPACE;
    case GLFW_KEY_TAB: return VK_TAB;
    case GLFW_KEY_ENTER: return VK_RETURN;
    case GLFW_KEY_ESCAPE: return VK_ESCAPE;
    case GLFW_KEY_BACKSPACE: return VK_BACK;
    case GLFW_KEY_INSERT: return VK_INSERT;
    case GLFW_KEY_DELETE: return VK_DELETE;
    case GLFW_KEY_HOME: return VK_HOME;
    case GLFW_KEY_END: return VK_END;
    case GLFW_KEY_PAGE_UP: return VK_PRIOR;
    case GLFW_KEY_PAGE_DOWN: return VK_NEXT;
    case GLFW_KEY_UP: return VK_UP;
    case GLFW_KEY_DOWN: return VK_DOWN;
    case GLFW_KEY_LEFT: return VK_LEFT;
    case GLFW_KEY_RIGHT: return VK_RIGHT;
    case GLFW_KEY_PRINT_SCREEN: return VK_SNAPSHOT;
    case GLFW_KEY_SCROLL_LOCK: return VK_SCROLL;
    case GLFW_KEY_PAUSE: return VK_PAUSE;
    default: return 0;
  }
}

unsigned glfw_mods(int mods) {
  unsigned result = 0;
  if (mods & GLFW_MOD_CONTROL) result |= MOD_CONTROL;
  if (mods & GLFW_MOD_ALT) result |= MOD_ALT;
  if (mods & GLFW_MOD_SHIFT) result |= MOD_SHIFT;
  if (mods & GLFW_MOD_SUPER) result |= MOD_WIN;
  return result | MOD_NOREPEAT;
}

}  // namespace

struct UiHost::Window {
  GLFWwindow* handle = nullptr;
  ImGuiContext* imgui = nullptr;
  bool tray = false;
  bool visible = false;
  bool fonts_ready = false;
  float scale = 1.0f;
  std::string theme;
  Fonts fonts;
  std::vector<ImWchar> glyph_ranges;
  int width = 0, height = 0;
};

UiHost::UiHost() = default;
UiHost::~UiHost() { stop(); }

bool UiHost::start(UiBridge::Handler handler, const std::string& data_root, const std::string& font_dir,
                   int ui_scale_percent) {
  if (started_) return true;
  data_root_ = data_root;
  font_dir_ = font_dir;
  ui_scale_percent_ = ui_scale_percent > 0 ? ui_scale_percent : 100;
  if (!glfwInit()) {
    BX_LOG_WARN("GLFW could not start: the interface is unavailable");
    return false;
  }
  glfwSetErrorCallback([](int code, const char* text) { BX_LOG_WARN("GLFW error %d: %s", code, text ? text : ""); });
  bridge_.start(std::move(handler));
  bridge_.set_wake([this] { wake(); });
  store_.initialise();
  images_.start();
  started_ = true;
  BX_LOG_INFO("the Linux interface is ready; the panel opens on request");
  return true;
}

void UiHost::stop() {
  if (!started_.exchange(false)) return;
  images_.stop();
  bridge_.stop();
  if (main_) destroy_window(main_);
  if (tray_) destroy_window(tray_);
  glfwTerminate();
}

UiHost::Window* UiHost::create_window(bool tray, int width, int height) {
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
  auto* window = new Window();
  window->tray = tray;
  window->handle = glfwCreateWindow(width, height, tray ? "BuffrX" : "BuffrX", nullptr, nullptr);
  if (!window->handle) {
    delete window;
    BX_LOG_WARN("the interface window could not be created");
    return nullptr;
  }
  glfwSetWindowUserPointer(window->handle, this);
  glfwMakeContextCurrent(window->handle);
  glfwSwapInterval(1);

  float content_scale = 1.0f;
  glfwGetWindowContentScale(window->handle, &content_scale, nullptr);
  window->scale = content_scale * kUiScale * static_cast<float>(ui_scale_percent_) / 100.0f;

  window->imgui = ImGui::CreateContext();
  ImGui::SetCurrentContext(window->imgui);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplGlfw_InitForOpenGL(window->handle, true);
  ImGui_ImplOpenGL3_Init("#version 330 core");
  glfwSetKeyCallback(window->handle, &UiHost::on_key);
  glfwSetMouseButtonCallback(window->handle, &UiHost::on_mouse_button);
  glfwSetCursorPosCallback(window->handle, &UiHost::on_cursor_pos);
  glfwSetWindowCloseCallback(window->handle, &UiHost::on_close);

  const std::string theme = store_.theme();
  std::vector<ImWchar> unused;
  if (!build_fonts(io.Fonts, font_dir_, window->scale, {}, window->glyph_ranges, window->fonts)) {
    BX_LOG_WARN("the interface fonts were not found in %s; using ImGui's own", font_dir_.c_str());
    io.Fonts->Clear();
    io.Fonts->AddFontDefault();
    window->fonts.body = window->fonts.strong = window->fonts.small = window->fonts.tiny = window->fonts.label =
        window->fonts.title = window->fonts.display = window->fonts.numbers = window->fonts.big = io.Fonts->Fonts.front();
  }
  io.Fonts->Build();
  capitalise_glyphs(io.Fonts);
  window->fonts_ready = true;
  apply_style(palette_for(theme), window->scale);
  window->theme = theme;
  window->width = width;
  window->height = height;
  ImGui::SetCurrentContext(nullptr);
  BX_LOG_INFO("an interface window is ready (%dx%d at %.2f)", width, height, window->scale);
  return window;
}

void UiHost::destroy_window(Window*& window) {
  if (!window) return;
  if (window->imgui) {
    ImGui::SetCurrentContext(window->imgui);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(window->imgui);
    ImGui::SetCurrentContext(nullptr);
  }
  if (window->handle) glfwDestroyWindow(window->handle);
  delete window;
  window = nullptr;
}

void UiHost::apply_theme(Window& window) {
  ImGui::SetCurrentContext(window.imgui);
  apply_style(palette_for(store_.theme()), window.scale);
  window.theme = store_.theme();
}

void UiHost::frame() {
  if (!started_) return;
  ImGui::SetCurrentContext(nullptr);
  glfwPollEvents();
  double mouse_x = 0, mouse_y = 0;
  if (dragging_ && main_ && main_->handle) {
    glfwGetCursorPos(main_->handle, &mouse_x, &mouse_y);
    int wx = 0, wy = 0;
    glfwGetWindowPos(main_->handle, &wx, &wy);
    glfwSetWindowPos(main_->handle, static_cast<int>(mouse_x + wx - drag_offset_x_),
                     static_cast<int>(mouse_y + wy - drag_offset_y_));
  }

  std::vector<std::pair<std::string, Json>> events;
  if (bridge_.pump(events)) {
    for (auto& event : events) {
      if (event.first == "ui.show") request(event.second["view"].as_string("main"));
      else store_.handle_event(event.first, event.second);
    }
  }
  process_requests();
  const uint32_t at = now_ms();
  store_.tick(at);
  store_.active = any_visible();
  store_.desktop_level = desktop_level_;
  store_.mic_level = mic_level_;
  if (main_) draw_window(*main_, false);
  if (tray_) draw_window(*tray_, false);

  if (!any_visible() && main_ && at - last_frame_ms_ > kReleaseAfterMs) {
    // Keep the window and its context: a re-open is one request away, and the memory is small on Linux.
    last_frame_ms_ = at;
  }
}

void UiHost::draw_window(Window& window, bool for_capture) {
  if (!window.handle) return;
  const bool visible = window.visible || for_capture;
  if (!visible) return;
  glfwMakeContextCurrent(window.handle);
  ImGui::SetCurrentContext(window.imgui);
  if (!fonts_ready_) {
    ImGuiIO& io = ImGui::GetIO();
    if (!build_fonts(io.Fonts, font_dir_, window.scale, {}, window.glyph_ranges, window.fonts)) {
      io.Fonts->AddFontDefault();
      window.fonts.body = window.fonts.strong = window.fonts.small = window.fonts.tiny = window.fonts.label =
          window.fonts.title = window.fonts.display = window.fonts.numbers = window.fonts.big = io.Fonts->Fonts.front();
    }
    io.Fonts->Build();
    capitalise_glyphs(io.Fonts);
    fonts_ready_ = window.fonts_ready = true;
  }
  if (window.theme != store_.theme()) apply_theme(window);
  const Palette palette = palette_for(store_.theme());
  set_theme(&palette, &window.fonts, window.scale);
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  drag_min_ = ImVec2(0, 0);
  drag_max_ = ImVec2(0, 0);
  no_drag_.clear();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin(window.tray ? "##buffrx-quick" : "##buffrx-panel", nullptr, flags);
  int display_w = static_cast<int>(io.DisplaySize.x), display_h = static_cast<int>(io.DisplaySize.y);
  ViewEnv env{store_, images_, *this, panel_, now_ms(), display_w, display_h};
  if (window.tray) {
    draw_tray(env);
  } else {
    draw_panel(env);
    maybe_show_whats_new(env);
    maybe_show_update_restart(env);
  }
  ImGui::End();
  ImGui::PopStyleVar();
  images_.upload(8, [](const uint8_t* bgra, int width, int height) -> ImTextureID {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
    return static_cast<ImTextureID>(static_cast<intptr_t>(texture));
  });
  images_.trim(
      [](ImTextureID texture) {
        const GLuint id = static_cast<GLuint>(texture);
        glDeleteTextures(1, &id);
      },
      256ull * 1024ull * 1024ull);

  ImGui::Render();
  int framebuffer_w = 0, framebuffer_h = 0;
  glfwGetFramebufferSize(window.handle, &framebuffer_w, &framebuffer_h);
  glViewport(0, 0, framebuffer_w, framebuffer_h);
  const ImVec4 ground = to_vec(palette.canvas);
  glClearColor(ground.x, ground.y, ground.z, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(window.handle);
  ImGui::SetCurrentContext(nullptr);
  ++passes_;
}

void UiHost::process_requests() {
  std::vector<std::string> requests;
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    requests.swap(requests_);
  }
  for (const std::string& view : requests) {
    if (view == "editor") {
      std::vector<std::string> files;
      {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        files.swap(editor_files_);
      }
      DWORD pid = 0;
      launch_editor(files, data_root_, pid);
    } else if (view == "main" || view.rfind("settings:", 0) == 0) {
      if (view.rfind("settings:", 0) == 0) {
        panel_.settings = view.substr(9);
        panel_.settings_loaded.clear();
      }
      if (!main_) {
        int work_w = 0, work_h = 0;
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        glfwGetMonitorWorkarea(monitor, nullptr, nullptr, &work_w, &work_h);
        const float scale = 1.0f * kUiScale * static_cast<float>(ui_scale_percent_) / 100.0f;
        const PanelSize size = panel_size_for(kPanelWidth, kPanelHeight, scale, work_w, work_h);
        main_ = create_window(false, static_cast<int>(size.width), static_cast<int>(size.height));
        if (main_) {
          glfwSetWindowPos(main_->handle, std::max(0, (work_w - static_cast<int>(size.width)) / 2),
                           std::max(0, (work_h - static_cast<int>(size.height)) / 2));
        }
      }
      if (main_) {
        glfwShowWindow(main_->handle);
        glfwFocusWindow(main_->handle);
        main_->visible = true;
      }
      if (tray_) {
        tray_->visible = false;
        glfwHideWindow(tray_->handle);
      }
    } else if (view == "compact" || view == "tray") {
      if (view == "tray" && tray_ && tray_->visible) {
        tray_->visible = false;
        glfwHideWindow(tray_->handle);
        continue;
      }
      if (!tray_) {
        int work_w = 0, work_h = 0;
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        glfwGetMonitorWorkarea(monitor, nullptr, nullptr, &work_w, &work_h);
        const float scale = 1.0f * kUiScale * static_cast<float>(ui_scale_percent_) / 100.0f;
        const QuickSize size = quick_size_for(kQuickWidth, kQuickHeight, scale, work_w, work_h);
        tray_ = create_window(true, static_cast<int>(size.width), static_cast<int>(size.height));
        if (tray_) {
          glfwSetWindowPos(tray_->handle, std::max(0, work_w - static_cast<int>(size.width) - 16),
                           std::max(0, work_h - static_cast<int>(size.height) - 16));
        }
      }
      if (tray_) {
        glfwShowWindow(tray_->handle);
        glfwFocusWindow(tray_->handle);
        tray_->visible = true;
      }
    } else if (view == "hide") {
      close_window();
    } else if (view == "toggle") {
      if (any_visible()) {
        close_window();
      } else {
        request(main_ && main_->visible ? "main" : "main");
      }
    } else if (view == "confirm_buffer_stop") {
      panel_.buffer_stop_open = true;
      request("main");
    }
  }
}

void UiHost::request(const std::string& view) {
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    requests_.push_back(view);
  }
  wake();
}

void UiHost::request_editor(const std::vector<std::string>& files) {
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    editor_files_ = files;
    requests_.push_back("editor");
  }
  wake();
}

void UiHost::wake() {
  {
    std::lock_guard<std::mutex> lock(wake_mutex_);
    woken_ = true;
  }
  wake_cv_.notify_all();
  glfwPostEmptyEvent();
}

void UiHost::wait(uint32_t idle_ms) {
  if (any_visible()) {
    glfwWaitEventsTimeout(std::min<uint32_t>(idle_ms, 250u) / 1000.0);
  } else {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    wake_cv_.wait_for(lock, std::chrono::milliseconds(std::min<uint32_t>(idle_ms, 500u)), [this] {
      return woken_;
    });
    woken_ = false;
  }
}

bool UiHost::any_visible() const {
  return (main_ && main_->visible) || (tray_ && tray_->visible);
}

bool UiHost::foreground_is_ours() const {
  if (main_ && main_->visible && glfwGetWindowAttrib(main_->handle, GLFW_FOCUSED)) return true;
  return tray_ && tray_->visible && glfwGetWindowAttrib(tray_->handle, GLFW_FOCUSED);
}

void UiHost::minimise() { if (main_ && main_->handle) glfwIconifyWindow(main_->handle); }

void UiHost::close_window() {
  if (tray_ && tray_->visible) {
    tray_->visible = false;
    glfwHideWindow(tray_->handle);
    return;
  }
  if (main_) {
    save_layout_now();
    main_->visible = false;
    glfwHideWindow(main_->handle);
  }
}

void UiHost::show(const std::string& view) { request(view); }
void UiHost::no_drag(ImVec2 min, ImVec2 max) { no_drag_.emplace_back(min, max); }
void UiHost::drag_area(ImVec2 min, ImVec2 max) {
  drag_min_ = min;
  drag_max_ = max;
}
void UiHost::capture_keys(bool on) {
  capturing_keys_ = on;
  if (on) {
    std::lock_guard<std::mutex> lock(keys_mutex_);
    keys_.clear();
  }
}
bool UiHost::take_key(bool& ctrl, bool& alt, bool& shift, bool& win, unsigned& virtual_key, bool& down) {
  std::lock_guard<std::mutex> lock(keys_mutex_);
  if (keys_.empty()) return false;
  const KeyEvent event = keys_.front();
  keys_.pop_front();
  ctrl = event.ctrl;
  alt = event.alt;
  shift = event.shift;
  win = event.win;
  virtual_key = event.key;
  down = event.down;
  return true;
}
void UiHost::pad_event(uint32_t, uint32_t) {}
void UiHost::save_layout() { save_layout_now(); }
void UiHost::save_layout_now() {
  Json layout = Json::object();
  if (main_ && main_->handle) {
    int x = 0, y = 0;
    glfwGetWindowPos(main_->handle, &x, &y);
    Json panel = Json::object();
    panel.set("x", Json(x));
    panel.set("y", Json(y));
    layout.set("panel", std::move(panel));
  }
  if (tray_ && tray_->handle) {
    int x = 0, y = 0;
    glfwGetWindowPos(tray_->handle, &x, &y);
    Json quick = Json::object();
    quick.set("x", Json(x));
    quick.set("y", Json(y));
    layout.set("quick", std::move(quick));
  }
  std::string text = layout.dump();
  std::string path = data_root_ + "/ui.json";
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file) {
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
  }
}
void UiHost::load_layout() {}
void UiHost::check_update(bool) {}
void UiHost::install_update() {}
void UiHost::set_pinned(bool pinned) { quick_pinned_ = pinned; }
ClipPicture UiHost::clip_picture(const std::string&, const RECT&, bool) { return ClipPicture::off; }
void UiHost::clip_picture_close() {}

void UiHost::on_key(GLFWwindow* handle, int key, int scancode, int action, int mods) {
  auto* host = static_cast<UiHost*>(glfwGetWindowUserPointer(handle));
  ImGui_ImplGlfw_KeyCallback(handle, key, scancode, action, mods);
  if (!host || !host->capturing_keys_ || action == GLFW_REPEAT) return;
  const unsigned virtual_key = virtual_key_from_glfw(key);
  const unsigned modifiers = glfw_mods(mods);
  if (!virtual_key) return;
  KeyEvent event;
  event.ctrl = (modifiers & MOD_CONTROL) != 0;
  event.alt = (modifiers & MOD_ALT) != 0;
  event.shift = (modifiers & MOD_SHIFT) != 0;
  event.win = (modifiers & MOD_WIN) != 0;
  event.key = virtual_key;
  event.down = action == GLFW_PRESS;
  std::lock_guard<std::mutex> lock(host->keys_mutex_);
  host->keys_.push_back(event);
}

void UiHost::on_mouse_button(GLFWwindow* handle, int button, int action, int mods) {
  auto* host = static_cast<UiHost*>(glfwGetWindowUserPointer(handle));
  ImGui_ImplGlfw_MouseButtonCallback(handle, button, action, mods);
  if (!host) return;
  if (button != GLFW_MOUSE_BUTTON_LEFT) return;
  if (action == GLFW_PRESS) {
    double x = 0, y = 0;
    glfwGetCursorPos(handle, &x, &y);
    const ImVec2 point(static_cast<float>(x), static_cast<float>(y));
    const bool in_drag = point.x >= host->drag_min_.x && point.y >= host->drag_min_.y &&
                         point.x <= host->drag_max_.x && point.y <= host->drag_max_.y;
    bool blocked = false;
    for (const auto& rect : host->no_drag_)
      blocked = blocked || (point.x >= rect.first.x && point.y >= rect.first.y && point.x <= rect.second.x &&
                            point.y <= rect.second.y);
    if (in_drag && !blocked) {
      host->dragging_ = true;
      host->drag_offset_x_ = x;
      host->drag_offset_y_ = y;
    }
  } else if (action == GLFW_RELEASE) {
    host->dragging_ = false;
  }
}

void UiHost::on_cursor_pos(GLFWwindow* handle, double x, double y) {
  ImGui_ImplGlfw_CursorPosCallback(handle, x, y);
}
void UiHost::on_close(GLFWwindow* handle) {
  auto* host = static_cast<UiHost*>(glfwGetWindowUserPointer(handle));
  if (!host) return;
  glfwSetWindowShouldClose(handle, GLFW_FALSE);
  glfwHideWindow(handle);
  if (host->main_ && handle == host->main_->handle) host->main_->visible = false;
  if (host->tray_ && handle == host->tray_->handle) host->tray_->visible = false;
}
void UiHost::on_framebuffer_size(GLFWwindow*, int, int) {}

void UiHost::set_audio_levels(float desktop, float mic) {
  desktop_level_ = desktop;
  mic_level_ = mic;
}
bool UiHost::audio_meter_visible() const { return any_visible(); }

Json UiHost::frame_stats(bool reset) {
  Json stats = Json::object();
  stats.set("passes", Json(static_cast<int64_t>(passes_.load())));
  stats.set("visible", Json(any_visible()));
  stats.set("paced", Json(any_visible()));
  if (reset) passes_.store(0);
  return stats;
}
void UiHost::note_pass(double ms, uint32_t) { frame_report_ = std::to_string(ms) + " ms"; }

bool UiHost::capture(const std::string& view, const std::string& png_path, std::string& error) {
  error = "screenshots of the interface are not implemented on Linux";
  (void)view;
  (void)png_path;
  return false;
}

bool UiHost::input(const Json&, std::string& error) {
  error = "scripted interface input is not implemented on Linux";
  return false;
}

}  // namespace bx::ui
