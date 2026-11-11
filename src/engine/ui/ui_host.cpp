#include "ui_host.h"
#include <dcomp.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cfloat>
#include <cstring>

#include "../../shared/files.h"
#include "../../shared/log.h"
#include "../audio_meter.h"
#include "../platform.h"
#include "../screenshot.h"
#include "../toast.h"
#include "ui_pad_nav.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../link_math.h"
#include "../loop_wait.h"
#include "../update_math.h"
#include "../update_policy.h"
#include "ui_anim.h"
#include "ui_icons.h"
#include "ui_parts.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_window_math.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace bx::ui {

namespace {

// The timer that keeps the engine's heartbeat going while a panel is dragged or resized.
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
constexpr UINT_PTR kModalHeartbeatTimer = 0xB0F1;
constexpr UINT kEditorLeaving = WM_APP + 0x425;
// An editor that is alive but has not put its window inside the panel within this long is not coming: the panel would
// stay on "LOADING EDITOR" with every control disabled, close included. It is let go of and asked to close, and ended
// after the grace period if it still runs.
constexpr uint32_t kEditorLoadTimeoutMs = 6000;
constexpr uint32_t kEditorKillGraceMs = 2000;
// How long an input for a window that is not on screen yet waits for it: long enough for ui.show and a click in one
// breath, short enough that it never lands on a picture the caller was not looking at.
constexpr uint32_t kInputQueueMs = 2000;

const wchar_t* kPanelClass = L"BuffrXPanel";
const wchar_t* kTrayClass = L"BuffrXTrayPanel";
// The child that carries the picture (DirectComposition target). The editor and the clip page are other children of the
// panel and sit over it, which they cannot do over a target on the panel's own window.
const wchar_t* kSurfaceClass = L"BuffrXSurface";
// How long the device and the windows are kept after the last one hides: a quick re-open is instant,
// a closed interface gives everything back.
const uint32_t kReleaseAfterMs = 10000;
// The interface is drawn a fifth larger than its design sizes on top of the monitor's DPI: type, controls,
// gaps and the windows' first sizes grow together (the user asked for a bigger interface, 2026-10-24).
const float kUiScale = 1.2f;
// BUFFRX_NO_PACING=1 gives the old loop back (a 20 ms sleep, no vertical-blank wait, no wake-ups): the A side of a
// frame-pacing measurement (tools/ui-frame-probe.ps1), not a setting.
// Diagnostic switches for the cursor-lag hunt (docs/measurement.md), not settings: BUFFRX_UI_BLT=1 gives the windows the old blt-model
// swap chain with a redirection bitmap (the compositor draws them like any other window, no flip model), BUFFRX_UI_HTCLIENT=1 answers
// every hit test with the client area (no drag header, no resize edges).
bool diag_flag(const char* name) {
  char value[8] = {};
  return GetEnvironmentVariableA(name, value, sizeof(value)) > 0 && value[0] != '0';
}

bool pacing_off() {
  static const bool off = [] {
    char value[8] = {};
    return GetEnvironmentVariableA("BUFFRX_NO_PACING", value, sizeof(value)) > 0 && value[0] != '0';
  }();
  return off;
}

// Microseconds on the performance counter: the clock the frame numbers and the pacing use (GetTickCount steps 15.6 ms).
int64_t now_us() {
  static const int64_t frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return counter.QuadPart / frequency * 1000000 + counter.QuadPart % frequency * 1000000 / frequency;
}

template <typename T>
void release(T*& object) {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

UINT window_dpi(HWND hwnd) {
  using GetDpi = UINT(WINAPI*)(HWND);
  static GetDpi get = reinterpret_cast<GetDpi>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow")));
  const UINT dpi = get && hwnd ? get(hwnd) : 96;
  return dpi ? dpi : 96;
}

UINT system_dpi() {
  HDC screen = GetDC(nullptr);
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

RECT work_area_of(HMONITOR monitor) {
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
  // A monitor that has just gone: the primary screen is a place, an empty rectangle is not.
  return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

RECT work_area_for(HWND hwnd) {
  return work_area_of(hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                           : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
}

bool rect_on_screen(const RECT& rect) {
  POINT corner{rect.left + 40, rect.top + 20};
  return MonitorFromPoint(corner, MONITOR_DEFAULTTONULL) != nullptr;
}

// A rectangle made to fit a work area: never larger than it, and never past its edges. A window wider and
// taller than the screen (the design panel is 2016x1296 physical pixels at 150%) comes back the size of
// the work area at its top left; one whose monitor went away comes back inside the one in use.
RECT fit_to_work(RECT rect, const RECT& work) {
  const LONG work_w = work.right - work.left, work_h = work.bottom - work.top;
  if (work_w <= 0 || work_h <= 0) return rect;
  const LONG width = std::min(rect.right - rect.left, work_w);
  const LONG height = std::min(rect.bottom - rect.top, work_h);
  const LONG x = std::clamp(rect.left, work.left, work.right - width);
  const LONG y = std::clamp(rect.top, work.top, work.bottom - height);
  return RECT{x, y, x + width, y + height};
}

// The panel's place: the stored one when it is still reachable, else centred on the work area, both inside the
// panel's own limits and fitted to it - and its first size never larger than the
// work area it has to live in, at any scale.
RECT panel_place(const RECT& work, float scale, const RECT& stored, bool restore, bool editor = false) {
  const LONG work_w = work.right - work.left, work_h = work.bottom - work.top;
  if (restore) {
    const RECT fitted = fit_to_work(stored, work);
    const PanelSize size = panel_clamp(fitted.right - fitted.left, fitted.bottom - fitted.top,
                                       panel_limits(scale, work_w, work_h, editor));
    const LONG x = std::clamp(fitted.left, work.left, work.right - size.width);
    const LONG y = std::clamp(fitted.top, work.top, work.bottom - size.height);
    return RECT{x, y, x + size.width, y + size.height};
  }
  const PanelSize size = panel_size_for(kPanelWidth, kPanelHeight, scale, work_w, work_h);
  const LONG x = work.left + (work_w - size.width) / 2, y = work.top + (work_h - size.height) / 2;
  return RECT{x, y, x + size.width, y + size.height};
}

// The quick panel's place: by the tray at the bottom right of the work area (a taskbar along the top or the
// left is already taken out of it, so the tray's corner is right on every layout), or where the user dragged
// it. Either way inside the work area. Its size is the user's own (design units, kept in ui.json) inside the limits
// ui_window_math.h sets - 600 design units are 1440 pixels at 200%, more than a laptop's work area, so the limits
// give way to the screen.
RECT tray_place(const RECT& work, float scale, POINT stored, bool moved, float design_width, float design_height) {
  const LONG work_w = work.right - work.left, work_h = work.bottom - work.top;
  const QuickSize size = quick_size_for(design_width, design_height, scale, work_w, work_h);
  const LONG width = size.width, height = size.height;
  const LONG margin = static_cast<LONG>(12 * scale);
  if (moved) return fit_to_work(RECT{stored.x, stored.y, stored.x + width, stored.y + height}, work);
  return fit_to_work(RECT{work.right - width - margin, work.bottom - height - margin, work.right - margin, work.bottom - margin},
                     work);
}

void round_corners(HWND hwnd) {
  // DWMWA_WINDOW_CORNER_PREFERENCE (Windows 11): rounded, like every other window there.
  const DWORD attribute = 33;
  const int round = 2;
  DwmSetWindowAttribute(hwnd, attribute, &round, sizeof(round));
  // A one-pixel frame keeps the system shadow on a frameless window.
  MARGINS margins{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(hwnd, &margins);
}

bool in_rect(const ImVec2& point, const ImVec2& min, const ImVec2& max) {
  return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
}

void collect_glyphs(const std::string& text, std::set<ImWchar>& glyphs, bool& grew) {
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    unsigned codepoint = c;
    size_t length = 1;
    if (c >= 0xF0 && i + 3 < text.size()) {
      codepoint = ((c & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
      length = 4;
    } else if (c >= 0xE0 && i + 2 < text.size()) {
      codepoint = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
      length = 3;
    } else if (c >= 0xC0 && i + 1 < text.size()) {
      codepoint = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
      length = 2;
    }
    i += length;
    // Beyond the base ranges in build_fonts, and inside what ImGui's 16-bit glyphs can carry.
    const bool base = codepoint < 0x250 || (codepoint >= 0x370 && codepoint < 0x500) ||
                      (codepoint >= 0x2000 && codepoint < 0x2070) || (codepoint >= 0x2190 && codepoint < 0x2400);
    if (base || codepoint > 0xFFFF) continue;
    if (glyphs.insert(static_cast<ImWchar>(codepoint)).second) grew = true;
  }
}

}  // namespace

struct UiWindow {
  bool tray = false;
  HWND hwnd = nullptr;
  IDXGISwapChain1* swap = nullptr;
  // The swap chain is handed to DirectComposition (see make_window): the compositor draws it as a visual, like Chromium's windows.
  HWND surface = nullptr;  // the child window the DirectComposition target is made on
  IDCompositionDevice* dcomp_device = nullptr;
  IDCompositionTarget* dcomp_target = nullptr;
  IDCompositionVisual* dcomp_visual = nullptr;
  ID3D11RenderTargetView* target = nullptr;
  ImGuiContext* imgui = nullptr;
  Fonts fonts;
  std::vector<ImWchar> glyph_ranges;
  float scale = 1.0f;
  std::string theme;
  bool visible = false;
  bool dirty = true;
  uint32_t last_message_pulse_ms = 0;
  bool fonts_stale = true;
  // A DPI change marks the fonts due: the atlas is rebuilt by the frame that follows, never inside the DPI message
  // (a cross-DPI drag used to spend 300-640 ms rebuilding it in the message, with the window frozen).
  bool fonts_due = false;
  bool backends = false;
  int64_t settle_us = 0;  // a settle frame is due at this time (0: none): one frame after the last input, for a tooltip to finish
  std::vector<std::pair<ImVec2, ImVec2>> drag, no_drag;
  // Pacing to the display. `pace` is the swap chain's frame-latency waitable object: signalled when the display can
  // take another frame. Null when the machine refuses it, and the window then draws as fast as it is asked to.
  UINT swap_flags = 0;
  HANDLE pace = nullptr;
  bool pace_ready = false;  // the signal was taken and no frame has used it yet
  int64_t last_present_us = 0;
  bool spinning = false;         // a spinner was drawn: the next frame is due in ~33 ms
  bool pointer_only = false;     // the last input was the pointer moving, no button held: an unfocused window's frames come at most ~60 a second
  bool focused = false;          // this window is the foreground window: the user is operating it (set by the frame that draws it)
  // V2 (D6): a hide fades the content out over the exit token; a show during the fade cancels it. `retire_on_hide`: destroyed once hidden.
  bool closing = false;
  int64_t closing_us = 0;
  bool retire_on_hide = false;
  uint64_t digest = 0;           // the draw data of the last presented frame (draw_digest), so an identical one is not presented
  bool digest_valid = false;     // false after a show, a resize or a capture: the next frame is always presented
  // Motion. `opening`: just shown, its first frame starts the fade-in from the ground. `moving`: the last frame left
  // something that has not arrived (a hover, a slide, a scroll), so the next one is asked for at the display's rate.
  bool opening = false;
  bool moving = false;
  bool resize_pending = false;  // a size arrived during a drag and was skipped; applied when the drag ends
  int resize_interval_us = 8000;  // sampled from the window's monitor when a resize starts
  int64_t shown_us = 0;
};

namespace {
// The swap chain holds one frame in flight: the picture on screen is at most one refresh behind the one being drawn.
void bind_pace(UiWindow& window) {
  if (window.pace) CloseHandle(window.pace);
  window.pace = nullptr;
  window.pace_ready = false;
  if (!(window.swap_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) || !window.swap) return;
  IDXGISwapChain2* swap2 = nullptr;
  if (SUCCEEDED(window.swap->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&swap2))) && swap2) {
    swap2->SetMaximumFrameLatency(1);
    window.pace = swap2->GetFrameLatencyWaitableObject();
    swap2->Release();
  }
}
}  // namespace

// The page the clip player runs (ui/dist, which holds index.html): beside the engine's folder in the package
// (<stage>\ui\dist), and in the repository when the engine runs from build\ or runtime\bin\64bit\. BUFFRX_WEB_ROOT names another.
static std::string web_root_folder() {
  if (const char* forced = std::getenv("BUFFRX_WEB_ROOT"); forced && *forced) return forced;
  const std::string dir = executable_directory();
  for (const char* up : {"\\..\\ui\\dist", "\\..\\..\\ui\\dist", "\\..\\..\\..\\ui\\dist"}) {
    const std::string folder = dir + up;
    if (GetFileAttributesA((folder + "\\index.html").c_str()) != INVALID_FILE_ATTRIBUTES) return folder;
  }
  return dir + "\\..\\ui\\dist";  // missing: the clip picture says it could not be shown
}

UiHost::UiHost() : store_(bridge_) { wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

UiHost::~UiHost() {
  web_clip_.release();  // before the panel's window goes: the page is a child of it
  stop();
  if (wake_event_) CloseHandle(wake_event_);
}

bool UiHost::start(UiBridge::Handler handler, const std::string& data_root, const std::string& font_dir, int ui_scale_percent) {
  data_root_ = data_root;
  font_dir_ = font_dir;
  ui_scale_percent_ = std::clamp(ui_scale_percent, 75, 150);
  web_clip_.configure(web_root_folder(), data_root + "\\webview2");
  // The page's Escape closes the clip the way the panel's own Escape does; a state change draws the picture's new state.
  web_clip_.on_close([this] {
    panel_.preview_open = false;
    if (panel_.preview_from_compact) {  // a preview opened from the quick panel goes back to it
      panel_.preview_from_compact = false;
      request("compact");
    }
  });
  web_clip_.on_change([this] {
    if (main_) main_->dirty = true;
    wake();
  });
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = &UiHost::window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIcon(GetModuleHandleW(nullptr), MAKEINTRESOURCE(1));
  window_class.lpszClassName = kPanelClass;
  RegisterClassExW(&window_class);
  window_class.lpszClassName = kTrayClass;
  window_class.style |= CS_DROPSHADOW;
  RegisterClassExW(&window_class);
  {
    WNDCLASSEXW surface_class{};
    surface_class.cbSize = sizeof(surface_class);
    surface_class.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
      if (message == WM_NCHITTEST) return HTTRANSPARENT;  // every mouse message belongs to the panel under it
      if (message == WM_ERASEBKGND) return 1;
      return DefWindowProcW(hwnd, message, wparam, lparam);
    };
    surface_class.hInstance = GetModuleHandleW(nullptr);
    surface_class.lpszClassName = kSurfaceClass;
    RegisterClassExW(&surface_class);
  }
  load_layout();
  // An interrupted download was never verified and must not survive a restart.
  DeleteFileA((data_root_ + "\\update\\BuffrX-Setup.exe.partial").c_str());
  bridge_.set_wake([this] { wake(); });
  bridge_.start(std::move(handler));
  images_.start();
  store_.initialise();
  started_ = true;
  return true;
}

void UiHost::stop() {
  if (!started_) return;
  started_ = false;
  save_layout();
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    if (capture_.pending) {
      capture_.error = "the interface is closing";
      capture_.done = true;
      capture_.pending = false;
      capture_done_.notify_all();
    }
  }
  release_device();
  images_.stop();
  bridge_.stop();
}

// ---------------------------------------------------------------- device and windows

bool UiHost::ensure_device() {
  if (device_) return true;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                     3, D3D11_SDK_VERSION, &device_, nullptr, &context_);
  if (FAILED(result)) {
    // A machine without a usable GPU still gets its interface, drawn by WARP.
    result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
  }
  if (FAILED(result)) {
    BX_LOG_WARN("the interface could not create its graphics device (0x%08lx)", static_cast<unsigned long>(result));
    return false;
  }
  // The mark's edges as the page's browser draws them: one sampled bitmap, tinted by whichever theme is on when the
  // titlebar draws it (ui_widgets draw_app_icon). Triangles alone stair-step at the titlebar's 18 units.
  constexpr int kMarkSize = 128;
  const std::vector<uint8_t> coverage = mark_coverage(kMarkSize);
  // The mark is drawn at about 22 pixels from a 96-pixel bitmap, so a single level would alias again however it is
  // filtered: the chain is box-filtered here and the sampler picks the level that matches the size on screen.
  std::vector<std::vector<uint32_t>> mip_levels;
  std::vector<uint32_t> base(coverage.size(), 0);
  for (size_t i = 0; i < coverage.size(); ++i) base[i] = (static_cast<uint32_t>(coverage[i]) << 24) | 0x00FFFFFFu;
  mip_levels.push_back(std::move(base));
  for (int side = kMarkSize; side > 1; side /= 2) {
    const std::vector<uint32_t>& from = mip_levels.back();
    const int half = side / 2;
    std::vector<uint32_t> to(static_cast<size_t>(half) * static_cast<size_t>(half), 0);
    for (int y = 0; y < half; ++y) {
      for (int x = 0; x < half; ++x) {
        uint32_t sum = 0;
        for (int dy = 0; dy < 2; ++dy) {
          for (int dx = 0; dx < 2; ++dx) {
            sum += from[static_cast<size_t>(y * 2 + dy) * static_cast<size_t>(side) + static_cast<size_t>(x * 2 + dx)] >> 24;
          }
        }
        to[static_cast<size_t>(y) * static_cast<size_t>(half) + static_cast<size_t>(x)] = ((sum / 4) << 24) | 0x00FFFFFFu;
      }
    }
    mip_levels.push_back(std::move(to));
  }
  std::vector<D3D11_SUBRESOURCE_DATA> mark_data;
  mark_data.reserve(mip_levels.size());
  for (size_t level = 0; level < mip_levels.size(); ++level) {
    const int side = kMarkSize >> level;
    mark_data.push_back({mip_levels[level].data(), static_cast<UINT>(side * 4), 0});
  }
  D3D11_TEXTURE2D_DESC mark_description{};
  mark_description.Width = kMarkSize;
  mark_description.Height = kMarkSize;
  mark_description.MipLevels = static_cast<UINT>(mip_levels.size());
  mark_description.ArraySize = 1;
  mark_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  mark_description.SampleDesc.Count = 1;
  mark_description.Usage = D3D11_USAGE_IMMUTABLE;
  mark_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* mark_surface = nullptr;
  ID3D11ShaderResourceView* mark_view = nullptr;
  if (SUCCEEDED(device_->CreateTexture2D(&mark_description, mark_data.data(), &mark_surface))) {
    device_->CreateShaderResourceView(mark_surface, nullptr, &mark_view);
  }
  release(mark_surface);
  mark_texture_ = static_cast<ImTextureID>(reinterpret_cast<intptr_t>(mark_view));
  set_mark_texture(mark_texture_);
  return true;
}

void UiHost::release_device() {
  if (main_ || tray_) save_layout();
  destroy_window(tray_);
  destroy_window(main_);
  images_.clear([](ImTextureID texture) {
    reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<intptr_t>(texture))->Release();
  });
  ID3D11ShaderResourceView* mark_view =
      reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<intptr_t>(mark_texture_));
  release(mark_view);
  mark_texture_ = 0;
  set_mark_texture(0);
  release(context_);
  release(device_);
  release_font_files();  // every atlas went with its window
  store_.active = false;
  hidden_since_ = 0;
}

UiWindow* UiHost::open_window(bool tray) {
  open_t0_us_ = now_us();
  if (!ensure_device()) return nullptr;
  open_marks_[0] = static_cast<double>(now_us() - open_t0_us_) / 1000.0;
  auto window = std::make_unique<UiWindow>();
  window->tray = tray;
  const RECT initial_work = work_area_for(nullptr);
  const float requested_scale = system_dpi() / 96.0f * kUiScale * ui_scale_percent_ / 100.0f;
  const float dpi_scale = tray ? requested_scale : panel_scale_to_fit(requested_scale, initial_work.right - initial_work.left,
                                                                      initial_work.bottom - initial_work.top);
  window->scale = dpi_scale;  // the first messages (WM_GETMINMAXINFO) arrive before it can be read from the window
  RECT rect{};
  DWORD style = 0, ex_style = 0;
  if (tray) {
    const RECT work = work_area_for(nullptr);
    const QuickSize size = quick_size_for(quick_w_, quick_h_, dpi_scale, work.right - work.left, work.bottom - work.top);
    const int width = static_cast<int>(size.width), height = static_cast<int>(size.height);
    const int margin = static_cast<int>(12 * dpi_scale);
    rect = RECT{work.right - width - margin, work.bottom - height - margin, work.right - margin, work.bottom - margin};
    // A thick frame is what lets Windows size a window from its edges; WM_NCCALCSIZE gives the whole window to the client, so
    // none of it is seen. No maximize box: the quick panel has no full-screen state and no edge snap.
    style = WS_POPUP | WS_THICKFRAME;
    ex_style = WS_EX_TOOLWINDOW | (diag_flag("BUFFRX_UI_BLT") ? 0 : WS_EX_NOREDIRECTIONBITMAP) | (quick_pinned_ ? WS_EX_TOPMOST : 0);
  } else {
    if (panel_rect_.right > panel_rect_.left && rect_on_screen(panel_rect_)) {
      rect = panel_rect_;
    } else {
      const RECT work = work_area_for(nullptr);
      const PanelSize size = panel_size_for(kPanelWidth, kPanelHeight, dpi_scale, work.right - work.left, work.bottom - work.top);
      const LONG width = size.width, height = size.height;
      const int x = work.left + (work.right - work.left - width) / 2, y = work.top + (work.bottom - work.top - height) / 2;
      rect = RECT{x, y, x + width, y + height};
    }
    // No maximize box: the panel has its own size ceiling, not a full-screen state.
    style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    // No GDI copy of the window: the swap chain is the only surface, so a drag moves one surface instead of two.
    ex_style = WS_EX_APPWINDOW | (diag_flag("BUFFRX_UI_BLT") ? 0 : WS_EX_NOREDIRECTIONBITMAP);
  }
  window->hwnd = CreateWindowExW(ex_style, tray ? kTrayClass : kPanelClass, L"BuffrX", style, rect.left, rect.top,
                                 rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), this);
  if (!window->hwnd) {
    // The tray icon and the hotkey both come here, and a window that never appears with nothing in the
    // log is the one failure the person cannot report: name the Win32 error instead.
    BX_LOG_WARN("the %s window could not be created (error %lu)", tray ? "quick panel" : "panel",
                static_cast<unsigned long>(GetLastError()));
    return nullptr;
  }
  SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window.get()));
  round_corners(window->hwnd);
  {
    // Windows' own "show animations" switch: with it off, nothing in the interface glides, it just changes.
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    anim_enable(animations != FALSE);
  }
  // Recompute the frame now that WM_NCCALCSIZE answers with the whole window as client area.
  SetWindowPos(window->hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  const RECT own_work = work_area_for(window->hwnd);
  const float own_scale = window_dpi(window->hwnd) / 96.0f * kUiScale * ui_scale_percent_ / 100.0f;
  window->scale = tray ? own_scale : panel_scale_to_fit(own_scale, own_work.right - own_work.left, own_work.bottom - own_work.top);
  // Placed and fitted once its own monitor and DPI are known: the stored place decides which monitor it
  // opens on, and that monitor's work area decides whether the design size fits (a 1120x720 design window
  // is 2016x1296 physical pixels at 150%, which no longer fits a 1366x768 laptop). A place saved on a
  // monitor that is gone is not one: the panel centres on the primary work area instead.
  {
    const RECT work = work_area_for(window->hwnd);
    const bool restore = !tray && panel_rect_.right > panel_rect_.left && panel_rect_.bottom > panel_rect_.top &&
                         rect_on_screen(panel_rect_);
    const RECT placed = tray ? tray_place(work, window->scale, quick_pos_, quick_moved_, quick_w_, quick_h_)
                             : panel_place(work, window->scale, panel_rect_, restore);
    SetWindowPos(window->hwnd, nullptr, placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (!tray) panel_rect_ = placed;
  }

  open_marks_[1] = static_cast<double>(now_us() - open_t0_us_) / 1000.0;  // the window and its placement are made
  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory2* factory = nullptr;
  device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
  if (dxgi_device) dxgi_device->GetAdapter(&adapter);
  if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
  if (factory) {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = diag_flag("BUFFRX_UI_BLT") ? 1 : 2;
    description.SwapEffect = diag_flag("BUFFRX_UI_BLT") ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Paced to the display (see UiWindow::pace); a machine that refuses the flag gets the plain swap chain.
    description.Flags = pacing_off() || diag_flag("BUFFRX_UI_BLT") ? 0 : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // A composition swap chain, as Chromium (Electron, the earlier product) presents: the compositor draws the window as a visual and
    // never promotes it to a direct-flip plane, so a focused panel does not make the display follow its sparse presents (variable
    // refresh dropping to its floor made the whole desktop cursor sluggish while the panel had focus). BUFFRX_UI_NO_DCOMP=1 or a
    // machine without DirectComposition gets the window swap chain below.
    if (!diag_flag("BUFFRX_UI_BLT") && !diag_flag("BUFFRX_UI_NO_DCOMP")) {
      description.Scaling = DXGI_SCALING_STRETCH;
      description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      RECT client{};
      GetClientRect(window->hwnd, &client);  // no window to size the buffers from: a composition swap chain needs its own size
      description.Width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
      description.Height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
      auto build = [&]() -> bool {
        // Keep the opaque panel under its sibling windows. Clipping the composition surface to a
        // WebView's bounds exposed the desktop while the browser was creating its first frame.
        window->surface = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kSurfaceClass, L"", WS_CHILD | WS_VISIBLE, 0, 0,
                                          static_cast<int>(description.Width), static_cast<int>(description.Height), window->hwnd,
                                          nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window->surface) return BX_LOG_WARN("surface window: error %lu", static_cast<unsigned long>(GetLastError())), false;
        SetWindowPos(window->surface, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        HRESULT hr = DCompositionCreateDevice(dxgi_device, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&window->dcomp_device));
        if (FAILED(hr) || !window->dcomp_device) return BX_LOG_WARN("DirectComposition device: 0x%08lx", static_cast<unsigned long>(hr)), false;
        hr = factory->CreateSwapChainForComposition(device_, &description, nullptr, &window->swap);
        if (FAILED(hr) || !window->swap) return BX_LOG_WARN("composition swap chain: 0x%08lx", static_cast<unsigned long>(hr)), false;
        hr = window->dcomp_device->CreateTargetForHwnd(window->surface, TRUE, &window->dcomp_target);
        if (SUCCEEDED(hr)) hr = window->dcomp_device->CreateVisual(&window->dcomp_visual);
        if (SUCCEEDED(hr)) hr = window->dcomp_visual->SetContent(window->swap);
        if (SUCCEEDED(hr)) hr = window->dcomp_target->SetRoot(window->dcomp_visual);
        if (SUCCEEDED(hr)) hr = window->dcomp_device->Commit();
        if (FAILED(hr)) return BX_LOG_WARN("DirectComposition tree: 0x%08lx", static_cast<unsigned long>(hr)), false;
        return true;
      };
      if (!build()) {
        BX_LOG_WARN("the interface could not use DirectComposition; using a window swap chain");
        release(window->dcomp_visual);
        release(window->dcomp_target);
        release(window->dcomp_device);
        release(window->swap);
        if (window->surface) DestroyWindow(window->surface);
        window->surface = nullptr;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        description.Width = description.Height = 0;
      }
    }
    if (!window->swap &&
        (FAILED(factory->CreateSwapChainForHwnd(device_, window->hwnd, &description, nullptr, nullptr, &window->swap)) || !window->swap)) {
      release(window->swap);
      description.Flags = 0;
      factory->CreateSwapChainForHwnd(device_, window->hwnd, &description, nullptr, nullptr, &window->swap);
    }
    window->swap_flags = description.Flags;
    bind_pace(*window);
    factory->MakeWindowAssociation(window->hwnd, DXGI_MWA_NO_ALT_ENTER);
  }
  release(factory);
  release(adapter);
  release(dxgi_device);
  if (!window->swap) {
    BX_LOG_WARN("the interface window could not get a swap chain");
    DestroyWindow(window->hwnd);
    return nullptr;
  }
  ID3D11Texture2D* back = nullptr;
  window->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
  if (back) device_->CreateRenderTargetView(back, nullptr, &window->target);
  release(back);

  open_marks_[2] = static_cast<double>(now_us() - open_t0_us_) / 1000.0;  // the swap chain and its target are made
  ImGuiContext* previous = ImGui::GetCurrentContext();
  window->imgui = ImGui::CreateContext();
  ImGui::SetCurrentContext(window->imgui);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplWin32_Init(window->hwnd);
  ImGui_ImplDX11_Init(device_, context_);
  window->backends = true;
  open_marks_[3] = static_cast<double>(now_us() - open_t0_us_) / 1000.0;  // the interface's context and backends are up
  if (previous) ImGui::SetCurrentContext(previous);

  UiWindow* raw = window.get();
  if (tray) tray_ = std::move(window);
  else main_ = std::move(window);
  return raw;
}

void UiHost::destroy_window(std::unique_ptr<UiWindow>& window) {
  if (!window) return;
  if (window.get() == main_.get()) {
    // The page is a child of this window. A preview does not outlive the window it was opened in: when the panel shows
    // again (the quick panel closes it, or it was hidden), the library is what it shows.
    // A panel destroyed while the hotkey recorder held the keys would leave every global hotkey suspended.
    cancel_recorder();
    web_clip_.release();
    panel_.preview_open = false;
  }
  if (window->imgui) {
    ImGuiContext* context = window->imgui;
    ImGui::SetCurrentContext(context);
    if (window->backends) {
      ImGui_ImplDX11_Shutdown();
      ImGui_ImplWin32_Shutdown();
    }
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(nullptr);
    // The motion maps are keyed by the context's address, which the next window may be given: its entries must not
    // outlive it, or the new window inherits tweens that never arrived.
    anim_purge_context(context);
  }
  release(window->target);
  release(window->swap);
  release(window->dcomp_visual);
  release(window->dcomp_target);
  release(window->dcomp_device);
  window->surface = nullptr;  // a child: it goes with the window
  if (window->pace) CloseHandle(window->pace);
  window->pace = nullptr;
  if (window->hwnd) {
    SetWindowLongPtrW(window->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(window->hwnd);
  }
  if (drawing_ == window.get()) drawing_ = nullptr;
  if (paced_window_ == window.get()) paced_window_ = nullptr;
  window.reset();
}

// ---------------------------------------------------------------- showing and hiding

void UiHost::check_clipboard_for_link() {
  // A link copied in a chat is offered whether or not a window is open: the quick panel opens for it. Only a clipboard
  // another program holds is backed off (below), so the poll is not an open every pass.
  // A clipboard another program holds is not opened again on every pass (~165 a second): it is tried again later.
  const uint32_t at = GetTickCount();
  if (clipboard_retry_at_ && static_cast<int32_t>(at - clipboard_retry_at_) < 0) return;
  HWND owner = main_ ? main_->hwnd : (tray_ ? tray_->hwnd : nullptr);
  const DWORD sequence = GetClipboardSequenceNumber();
  // The first read of a session only learns where the clipboard stands: a link left there from before BuffrX started
  // was copied for somebody else, and offering it again on every launch is the popup nobody asked for.
  if (clipboard_seq_ == 0) {
    clipboard_seq_ = sequence;
    return;
  }
  if (sequence == clipboard_seq_ || !IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    clipboard_seq_ = sequence;
    return;
  }
  if (!OpenClipboard(owner)) {
    clipboard_retry_at_ = at + 1000;  // another program holds it: try again in a second, not on every pass
    return;
  }
  clipboard_retry_at_ = 0;
  clipboard_seq_ = sequence;
  std::string text;
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const wchar_t* wide = static_cast<const wchar_t*>(GlobalLock(data))) {
      const size_t length = wcsnlen(wide, 2048);
      if (length < 2048) {  // a link is a few hundred characters; anything longer is not one
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        text.assign(static_cast<size_t>(std::max(0, bytes)), '\0');
        if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), text.data(), bytes, nullptr, nullptr);
      }
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  LinkTicket ticket;
  // A link this PC copied itself (the Share sheet's own) is not offered back to it, and one link is
  // offered once.
  if (!link_offer_wanted(text, offered_link_, store_.copied_link, last_copied_text()) || !parse_link_url(text, ticket)) return;
  const Json& known_links = store_.links;
  for (const Json& link : known_links["links"].items()) {
    if (link["url"].as_string() == text) return;
  }
  offered_link_ = text;
  panel_.link_offer = text;
  panel_.link_offer_expires = static_cast<int64_t>(ticket.expires);
  store_.dirty = true;
  // With nothing on screen the offer has nowhere to be asked: the quick panel comes up for it, as the tray does for
  // anything else that needs an answer.
  if (!any_visible()) request("compact");
}

void UiHost::open_share_for(const std::string& file) {
  if (file.empty()) return;
  // The library's own entry when it is loaded (its thumbnail, length and size); otherwise one made from
  // the file, and the sheet takes the length from the engine's reply.
  UiMedia item;
  bool found = false;
  for (const UiMedia& clip : store_.clips) {
    if (clip.file == file) {
      item = clip;
      found = true;
      break;
    }
  }
  if (!found) {
    item.file = file;
    item.name = file_stem(file);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const std::wstring wide = utf8_to_wide(file);
    if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
      item.size = (static_cast<int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    std::string extension = file.substr(file.find_last_of('.') == std::string::npos ? file.size() : file.find_last_of('.'));
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    item.clip = extension != ".png" && extension != ".jpg" && extension != ".jpeg";
  }
  panel_.settings.clear();
  panel_.preview_open = false;
  parts::open_share(store_, panel_, item);
  request("main");
}

// The clip the panel's preview plays: the page sits on the picture of the main window only (the quick panel's clips
// open there, as screenshots do). The folders the page may read are the library's own.
ClipPicture UiHost::clip_picture(const std::string& file, const RECT& rect, bool covered) {
  if (editor_mode_) return ClipPicture::playing;
  if (!main_ || !main_->hwnd) return ClipPicture::off;
  const std::string clips = store_.config["clips_dir"].as_string();
  // Recordings are library clips too (clips.list merges them): their base folder is mapped so a
  // recording outside the clips folder plays, whether it is a single file or a session chunk.
  std::string recordings = store_.config["recordings_dir"].as_string();
  if (recordings.empty()) recordings = clips;
  const std::vector<std::string> roots{clips, store_.config["screenshots_dir"].as_string(), recordings};
  // The page wears the panel's own theme: the same id the engine stores, which themes.css keys by.
  return web_clip_.show(main_->hwnd, file, rect, covered, roots, store_.config["theme"].as_string());
}

void UiHost::clip_picture_close() { web_clip_.release(); }

void UiHost::request(const std::string& view) {
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    requests_.push_back(view);
  }
  wake();
}

void UiHost::wake() {
  if (wake_event_) SetEvent(wake_event_);
}

uint32_t UiHost::hidden_deadline_ms() {
  const uint32_t now = GetTickCount();
  uint32_t ms = store_.next_due_ms(now);
  // The device and its textures go 10 s after the last window hides; the pass that first sees them hidden starts that clock.
  if (device_) ms = std::min(ms, hidden_since_ ? ms_until(now, hidden_since_, kReleaseAfterMs) + 1 : 20u);
  return ms;
}

void UiHost::wait(uint32_t idle_ms) {
  passes_.fetch_add(1, std::memory_order_relaxed);
  bool capture_pending = false;
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    capture_pending = capture_.pending;
  }
  // A capture being taken is polled every 20 ms; a window on screen waits for its own next deadline (wait_hint_ms_, set by frame(),
  // at most 100 ms): a quiet panel used to wake 60 times a second for nothing. A message, a request, a reply or an event wakes it.
  // With nothing on screen the loop sleeps to its earliest deadline.
  idle_ms = any_visible() || capture_pending ? std::min<uint32_t>(std::min<uint32_t>(idle_ms, capture_pending ? 20u : 250u), wait_hint_ms_)
                                              : loop_sleep_ms(idle_ms, hidden_deadline_ms());
  if (pacing_off()) {
    Sleep(std::min<uint32_t>(idle_ms, 20));
    return;
  }
  UiWindow* window = paced_window_;
  if (window && window->pace) {
    // The wait is the pacing: the display signals once per frame it takes, and the next pass draws into that slot. Messages
    // that arrive meanwhile are read on the next pass (at most one refresh later), which keeps a fast mouse from spinning
    // the loop faster than the screen. The bound keeps the tick coming round if the signal never does.
    // The wake event is waited on too: a pad edge must not sit until the display's next slot, which is what made the
    // controller feel late while the window was paced.
    if (!window->pace_ready) {
      // The system sends WM_SETCURSOR and WM_NCHITTEST to the window and waits for the reply, so sent messages are answered here,
      // as they arrive (PeekMessage runs them). Posted input (WM_MOUSEMOVE) is left for the next pass, so the pass runs once per
      // vertical blank and not once per mouse move (that was up to 1000 passes a second).
      HANDLE handles[2] = {window->pace, wake_event_};
      const DWORD count = wake_event_ ? 2 : 1;
      for (;;) {
        const DWORD woke = MsgWaitForMultipleObjectsEx(count, handles, 20, QS_SENDMESSAGE, 0);
        if (woke == WAIT_OBJECT_0) {
          window->pace_ready = true;
          break;
        }
        if (woke != WAIT_OBJECT_0 + count) break;  // the wake event or the bound
        MSG answered;
        PeekMessageW(&answered, nullptr, 0, 0, PM_NOREMOVE);
      }
    }
    return;
  }
  if (!wake_event_) {
    Sleep(idle_ms);
    return;
  }
  // Mouse movement wakes the loop only while a window is focused. Left in for every window, a move over a window nobody works in
  // woke the loop once per move: 77 passes a second with the quick panel under a 125 Hz sweep, 18 with this mask (measured with
  // tools/unfocused-panel-probe.ps1). Clicks, keys, the wheel, posted messages and sent messages (WM_SETCURSOR, WM_NCHITTEST,
  // WM_ACTIVATE) still wake it. The wheel is queued with the buttons (QS_MOUSEBUTTON); the SDK has no QS_MOUSEWHEEL flag.
  DWORD wake_mask = QS_KEY | QS_MOUSEBUTTON | QS_POSTMESSAGE | QS_SENDMESSAGE | QS_PAINT | QS_TIMER | QS_HOTKEY;
  if (focused_.load(std::memory_order_relaxed)) wake_mask |= QS_MOUSEMOVE;
  MsgWaitForMultipleObjectsEx(1, &wake_event_, idle_ms, wake_mask, MWMO_INPUTAVAILABLE);
}

bool UiHost::display_ready(UiWindow& window, bool strict) {
  if (!window.pace_ready && window.pace && WaitForSingleObject(window.pace, 0) == WAIT_OBJECT_0) window.pace_ready = true;
  if (strict) return frame_allowed_strict(window.pace != nullptr, window.pace_ready);
  return frame_allowed(window.pace != nullptr, window.pace_ready, now_us() - window.last_present_us);
}

// Why a frame is being drawn, for `ui.frame_stats`: every true flag counts once, so a window that draws with nothing visible
// changing shows which one keeps asking.
void UiHost::note_reasons(bool forced, bool store, bool dirty, bool moving, bool settle, const char* animation, bool images) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  if (forced) frame_stats_.reason("forced");
  if (store) frame_stats_.reason("store");
  if (dirty) frame_stats_.reason("dirty");
  if (moving) frame_stats_.reason("moving");
  if (settle) frame_stats_.reason("settle");
  if (animation) frame_stats_.reason(std::string("anim:") + animation);
  if (images) frame_stats_.reason("images");
}

void UiHost::note_pass(double ms, uint32_t message) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  if (ms > 12.0) ++hitches_over_12_;
  if (ms > 500.0) {
    ++hitches_over_500_;
    BX_LOG_WARN("the window thread held %.0f ms in one pass (last message 0x%04x): %s", ms, static_cast<unsigned>(message), frame_report_.c_str());
  }
  if (ms > worst_pass_ms_) {
    worst_pass_ms_ = ms;
    worst_message_ = message;
    worst_report_ = frame_report_;
  }
}

void UiHost::note_input(InputKind kind) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  frame_stats_.input(now_us(), kind);
}

Json UiHost::frame_stats(bool reset) {
  FrameSummary summary;
  Json reasons = Json::object();
  int64_t presented = 0, skipped = 0, build_avg_us = 0, build_max_us = 0;
  int64_t digest_avg_us = 0, draw_avg_us = 0, present_avg_us = 0;
  double open_ms = 0;
  Json by_kind = Json::object();
  int hitches_12 = 0, hitches_500 = 0;
  double worst_ms = 0;
  uint32_t worst_msg = 0;
  std::string worst_text;
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    summary = frame_stats_.summary();
    for (int k = 0; k < kInputKinds; ++k) {
      const FrameSummary kind = frame_stats_.summary(static_cast<InputKind>(k));
      Json entry = Json::object();
      entry.set("inputs", Json(kind.inputs));
      entry.set("avg_ms", Json(kind.latency_avg_ms));
      entry.set("p95_ms", Json(kind.latency_p95_ms));
      entry.set("max_ms", Json(kind.latency_max_ms));
      by_kind.set(input_kind_name(static_cast<InputKind>(k)), entry);
    }
    presented = frame_stats_.presented();
    skipped = frame_stats_.skipped_frames();
    hitches_12 = hitches_over_12_;
    hitches_500 = hitches_over_500_;
    worst_ms = worst_pass_ms_;
    worst_msg = worst_message_;
    worst_text = worst_report_;
    if (reset) {
      hitches_over_12_ = hitches_over_500_ = 0;
      worst_pass_ms_ = 0;
      worst_message_ = 0;
      worst_report_.clear();
    }
    build_avg_us = frame_stats_.build_avg_us();
    digest_avg_us = frame_stats_.digest_avg_us();
    draw_avg_us = frame_stats_.draw_avg_us();
    present_avg_us = frame_stats_.present_avg_us();
    build_max_us = frame_stats_.build_max_us();
    open_ms = frame_stats_.open_ms();
    for (const auto& entry : frame_stats_.reasons()) reasons.set(entry.first, Json(entry.second));
    if (reset) frame_stats_.reset();
  }
  const int64_t passes = reset ? passes_.exchange(0, std::memory_order_relaxed) : passes_.load(std::memory_order_relaxed);
  Json out = Json::object();
  out.set("passes", Json(passes));
  out.set("paced", Json(paced_.load(std::memory_order_relaxed)));
  out.set("focused", Json(focused_.load(std::memory_order_relaxed)));
  out.set("game_running", Json(game_flag_.load(std::memory_order_relaxed)));
  out.set("frames", Json(summary.frames));
  out.set("presented", Json(presented));
  out.set("skipped", Json(skipped));
  out.set("build_avg_us", Json(build_avg_us));
  out.set("build_max_us", Json(build_max_us));
  out.set("digest_avg_us", Json(digest_avg_us));
  out.set("draw_avg_us", Json(draw_avg_us));
  out.set("present_avg_us", Json(present_avg_us));
  out.set("open_ms", Json(open_ms));
  out.set("reasons", reasons);
  out.set("gap_avg_ms", Json(summary.avg_ms));
  out.set("gap_p95_ms", Json(summary.p95_ms));
  out.set("gap_max_ms", Json(summary.max_ms));
  out.set("inputs", Json(summary.inputs));
  out.set("latency_avg_ms", Json(summary.latency_avg_ms));
  out.set("latency_p95_ms", Json(summary.latency_p95_ms));
  out.set("latency_max_ms", Json(summary.latency_max_ms));
  out.set("latency_by_kind", by_kind);
  // Phase 0.3 hitches (passes of the window thread) and the worst one, with its last message and frame report.
  Json hitches = Json::object();
  hitches.set("over_12_ms", Json(hitches_12));
  hitches.set("over_500_ms", Json(hitches_500));
  out.set("hitches", hitches);
  Json worst = Json::object();
  worst.set("ms", Json(worst_ms));
  worst.set("message", Json(static_cast<int64_t>(worst_msg)));
  worst.set("report", Json(worst_text));
  out.set("worst", worst);
  // Phase 0.1: the last window open, by phase (device, window, swap chain and target, interface backends), in ms.
  Json opened_in = Json::object();
  opened_in.set("device_ms", Json(open_marks_[0]));
  opened_in.set("window_ms", Json(open_marks_[1] - open_marks_[0]));
  opened_in.set("swap_ms", Json(open_marks_[2] - open_marks_[1]));
  opened_in.set("interface_ms", Json(open_marks_[3] - open_marks_[2]));
  opened_in.set("total_ms", Json(open_marks_[3]));
  out.set("open_breakdown", opened_in);
  return out;
}

void UiHost::process_requests() {
  std::vector<std::string> requests;
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    requests.swap(requests_);
  }
  for (const std::string& view : requests) {
    if (view == "editor") start_editor();
    else if (view == "main") show_panel();
    else if (view == "compact") show_tray();
    else if (view == "confirm_buffer_stop") {
      if (main_ && main_->visible) show_panel();
      else show_tray();
      panel_.buffer_stop_open = true;
      store_.dirty = true;
    }
    else if (view == "tray") toggle_tray();
    else if (view == "hide") close_window();
    else if (view == "toggle") {
      if (tray_ && tray_->visible) {
        toggle_tray();  // the quick panel stays open when another app has the focus, so the hotkey is how it goes away
      } else if (main_ && main_->visible && main_->closing) {  // a toggle during the fade reverses it (D6)
        main_->closing = false;
        main_->dirty = true;
      } else if (main_ && main_->visible && !IsIconic(main_->hwnd)) {
        save_layout();
        begin_hide(*main_, false);
      } else {
        show_panel();
      }
    }
  }
}

void UiHost::request_editor(const std::vector<std::string>& files) {
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    editor_files_ = files;
    requests_.push_back("editor");
  }
  wake();
}

void UiHost::size_editor() {
  if (!editor_process_ || !main_ || !editor_mode_) return;
  RECT client{};
  GetClientRect(main_->hwnd, &client);
  const long inset = editor_resize_inset(main_->scale, client.right, client.bottom);
  for (HWND child = GetWindow(main_->hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid == editor_pid_) {
      if (editor_loading_ && IsWindowVisible(child)) {
        editor_loading_ = false;
        editor_loading_since_ = 0;
        main_->dirty = true;
      }
      RECT old{};
      GetWindowRect(child, &old);
      MapWindowPoints(nullptr, main_->hwnd, reinterpret_cast<POINT*>(&old), 2);
      if (old.left != inset || old.top != inset || old.right != client.right - inset || old.bottom != client.bottom - inset)
        SetWindowPos(child, HWND_TOP, inset, inset, client.right - 2 * inset, client.bottom - 2 * inset,
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
      break;
    }
  }
}

void UiHost::start_editor() {
  if (editor_process_) return;
  std::vector<std::string> files;
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    files.swap(editor_files_);
  }
  editor_return_quick_ = tray_ && tray_->visible;
  show_panel();
  if (!main_) return;
  GetWindowRect(main_->hwnd, &editor_return_rect_);
  editor_mode_ = true;
  editor_loading_ = true;
  editor_loading_since_ = GetTickCount();
  editor_returning_ = false;
  main_->dirty = true;
  web_clip_.release();
  if (!launch_editor(files, data_root_, editor_pid_, false, main_->hwnd)) {
    finish_editor();
    store_.toast(UiToast::error, "The editor did not open", "Try opening the editor again.");
    return;
  }
  editor_process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, editor_pid_);
  if (!editor_process_) {
    finish_editor();
    store_.toast(UiToast::error, "The editor did not open", "Its process could not be monitored.");
  }
}

void UiHost::request_editor_close() {
  if (!main_) return;
  for (HWND child = GetWindow(main_->hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid == editor_pid_) { PostMessageW(child, WM_CLOSE, 0, 0); return; }
  }
}

void UiHost::return_from_editor() {
  editor_mode_ = false;
  editor_loading_ = false;
  editor_loading_since_ = 0;
  if (main_ && editor_return_rect_.right > editor_return_rect_.left) {
    SetWindowPos(main_->hwnd, nullptr, editor_return_rect_.left, editor_return_rect_.top,
                 editor_return_rect_.right - editor_return_rect_.left, editor_return_rect_.bottom - editor_return_rect_.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    main_->dirty = true;
  }
}

void UiHost::finish_editor() {
  if (editor_process_) CloseHandle(editor_process_);
  editor_process_ = nullptr;
  editor_pid_ = 0;
  if (editor_mode_) return_from_editor();
  if (editor_return_quick_) show_tray();
  editor_return_quick_ = false;
  editor_returning_ = false;
}

void UiHost::show(const std::string& view) { request(view); }

void UiHost::show_panel() {
  if (!main_ && !open_window(false)) return;
  if (editor_mode_) { ShowWindow(main_->hwnd, SW_RESTORE); SetForegroundWindow(main_->hwnd); return; }
  if (main_->visible && main_->closing) {  // a show during the fade cancels it (D6)
    main_->closing = false;
    main_->dirty = true;
    SetForegroundWindow(main_->hwnd);
    return;
  }
  // Only one window at a time: the quick panel goes away, with its swapchain, its context and its
  // font atlas. Two small windows on the screen at once is the duplicate this replaces.
  if (tray_) {
    save_layout();  // its size is remembered before the window that holds it goes
    destroy_window(tray_);
  }
  // A panel hidden while the display changed (a resolution, a scale, a monitor undocked) is fitted to its
  // work area and its own limits before it appears again, so it can never come back larger than allowed.
  if (IsZoomed(main_->hwnd)) ShowWindow(main_->hwnd, SW_RESTORE);
  {
    RECT current{};
    GetWindowRect(main_->hwnd, &current);
    const RECT placed = panel_place(work_area_for(main_->hwnd), main_->scale, current, true);
    if (!EqualRect(&placed, &current)) {
      SetWindowPos(main_->hwnd, nullptr, placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      panel_rect_ = placed;
    }
  }
  if (!main_->visible) {
    main_->opening = true;
    std::lock_guard<std::mutex> lock(stats_mutex_);
    frame_stats_.opened(now_us());
  }
  ShowWindow(main_->hwnd, IsIconic(main_->hwnd) ? SW_RESTORE : SW_SHOW);
  // The first ShowWindow a window ever gets takes the STARTUPINFO's value instead of its argument -
  // Windows' rule - and the launcher starts the engine with CREATE_NO_WINDOW, so the panel would
  // stay hidden forever. SetWindowPos is not affected by that rule.
  SetWindowPos(main_->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetForegroundWindow(main_->hwnd);
  main_->visible = true;
  main_->dirty = true;
  store_.active = true;
  store_.refresh_library(0);
  store_.refresh_hotkeys();
}

// The quick panel: the tray panel, opened without the tray icon. The one small interface.
void UiHost::show_tray() {
  if (editor_mode_) { show_panel(); return; }
  if (!tray_ && !open_window(true)) return;
  if (tray_->visible && tray_->closing) {  // a show during the fade cancels it (D6)
    tray_->closing = false;
    tray_->dirty = true;
    SetForegroundWindow(tray_->hwnd);
    return;
  }
  // The big panel goes away with the same reason: one window at a time, one set of resources.
  if (main_) {
    save_layout();
    destroy_window(main_);
  }
  const RECT work = work_area_for(nullptr);
  const float scale = tray_->scale;
  const QuickSize size = quick_size_for(quick_w_, quick_h_, scale, work.right - work.left, work.bottom - work.top);
  const int width = static_cast<int>(size.width), height = static_cast<int>(size.height);
  const int margin = static_cast<int>(12 * scale);
  // Placed at the tray each time it has never been moved; after the user drags it, it stays where
  // they put it, at the size they gave it. The place is always fitted to the monitor it lands on, so 200% scaling (600
  // design units are 1440 physical pixels) and a taskbar along the top or the left cannot push it off-screen.
  int x = work.right - width - margin;
  int y = work.bottom - height - margin;
  if (quick_moved_ && quick_pos_.x >= 0) {
    x = quick_pos_.x;
    y = quick_pos_.y;
  }
  // A place on a monitor that has gone since (a laptop undocked) falls back to the primary one.
  const HMONITOR monitor = MonitorFromPoint(POINT{x, y}, MONITOR_DEFAULTTOPRIMARY);
  const RECT placed = tray_place(work_area_of(monitor), scale, POINT{x, y}, true, quick_w_, quick_h_);
  SetWindowPos(tray_->hwnd, quick_pinned_ ? HWND_TOPMOST : HWND_TOP, placed.left, placed.top, placed.right - placed.left,
               placed.bottom - placed.top, SWP_SHOWWINDOW);
  SetForegroundWindow(tray_->hwnd);
  if (!tray_->visible) {
    tray_->opening = true;
    std::lock_guard<std::mutex> lock(stats_mutex_);
    frame_stats_.opened(now_us());
  }
  tray_->visible = true;
  tray_->dirty = true;
  store_.active = true;
  store_.refresh_library(0);
}

// True when another window lies over some part of the quick panel, which is what happens once it is not pinned and
// something else is clicked.
static bool covered_by_another(HWND hwnd) {
  RECT rect{};
  if (!GetWindowRect(hwnd, &rect)) return false;
  const LONG w = rect.right - rect.left, h = rect.bottom - rect.top;
  const POINT points[5] = {{rect.left + w / 2, rect.top + h / 2}, {rect.left + w / 4, rect.top + h / 4}, {rect.left + 3 * w / 4, rect.top + h / 4},
                           {rect.left + w / 4, rect.top + 3 * h / 4}, {rect.left + 3 * w / 4, rect.top + 3 * h / 4}};
  for (const POINT& point : points) {
    const HWND at = GetAncestor(WindowFromPoint(point), GA_ROOT);
    if (at && at != hwnd) return true;
  }
  return false;
}

void UiHost::toggle_tray() {
  if (tray_ && tray_->visible && tray_->closing) {  // a second toggle during the fade reverses it (D6)
    tray_->closing = false;
    tray_->dirty = true;
    return;
  }
  if (tray_ && tray_->visible) {
    // Not pinned, it can be under another window: the tray icon then brings it forward instead of hiding it.
    if (!quick_pinned_ && !IsIconic(tray_->hwnd) && covered_by_another(tray_->hwnd)) {
      SetWindowPos(tray_->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
      SetForegroundWindow(tray_->hwnd);
      tray_->dirty = true;
      return;
    }
    begin_hide(*tray_, false);
    return;
  }
  show_tray();
}

void UiHost::set_pinned(bool pinned) {
  quick_pinned_ = pinned;
  quick_aside_ = false;
  if (tray_ && tray_->hwnd) {
    SetWindowPos(tray_->hwnd, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    tray_->dirty = true;
  }
  save_layout();
}

bool UiHost::any_visible() const { return windows_visible_.load(std::memory_order_relaxed); }

bool UiHost::audio_meter_visible() const {
  const bool visible = main_ && main_->visible;
  const bool minimized = visible && IsIconic(main_->hwnd);
  const bool view = !editor_mode_ && !panel_.preview_open && !panel_.share_open &&
                    (panel_.settings.empty() || panel_.settings == "audio");
  return audio_meter_wanted(visible, minimized, view, visible && main_->closing);
}

void UiHost::set_audio_levels(float desktop, float mic) {
  if (store_.desktop_level == desktop && store_.mic_level == mic) return;
  store_.desktop_level = desktop;
  store_.mic_level = mic;
  store_.dirty = true;
  wake();
}

bool UiHost::foreground_is_ours() const {
  const HWND foreground = GetForegroundWindow();
  if (!foreground) return false;
  for (const HWND ours : {published_main_.load(std::memory_order_relaxed), published_tray_.load(std::memory_order_relaxed)}) {
    if (ours && ours == foreground && !IsIconic(ours)) return true;
  }
  return false;
}

// ---------------------------------------------------------------- UiShell

void UiHost::minimise() {
  if (drawing_ && !drawing_->tray) ShowWindow(drawing_->hwnd, SW_MINIMIZE);
}

void UiHost::cancel_recorder() {
  if (!panel_.recorder_open && panel_.recording.empty()) return;
  // A panel closed mid-rebind: let go of the keys and give the hotkeys back, as Cancel would.
  panel_.recording.clear();
  panel_.recorder_preview.clear();
  panel_.recorder_open = false;
  capture_keys(false);
  Json args = Json::object();
  args.set("suspended", Json(false));
  store_.attempt("hotkeys.suspend", args, "Hotkeys did not come back on");
  if (panel_.recorder_controller) {
    // A panel closed while the pads were being listened to: stop that too, or they stay captured.
    Json capture = Json::object();
    capture.set("on", Json(false));
    store_.attempt("hotkeys.capture", capture, "The controller kept listening");
    panel_.recorder_controller = false;
  }
}

// V2 (D6): the content fades out over kDurFast, then the window hides. A hide does not wait on anything else: the hotkey and the
// tray answer at once, and a second show during the fade cancels it (see show_tray and show_panel).
void UiHost::begin_hide(UiWindow& window, bool retire) {
  // Hiding the panel mid-rebind gives the hotkeys back: nothing else would, and the keyboard stays swallowed.
  if (!window.tray) cancel_recorder();
  if (editor_mode_ && !window.tray) { request_editor_close(); return; }
  if (!window.visible || window.closing) return;
  // A hidden panel plays nothing: the page and its browser go with the window's picture, and the preview with them.
  if (main_ && &window == main_.get()) {
    web_clip_.release();
    panel_.preview_open = false;
  }
  if (!anim_enabled()) {
    ShowWindow(window.hwnd, SW_HIDE);
    window.visible = false;
    if (retire) (window.tray ? retire_tray_ : retire_main_) = true;
    return;
  }
  window.closing = true;
  window.closing_us = now_us();
  window.retire_on_hide = retire;
  window.dirty = true;
  wake();
}

void UiHost::finish_hides() {
  const int64_t now = now_us();
  for (UiWindow* window : {main_.get(), tray_.get()}) {
    if (!window || !window->closing || now - window->closing_us < static_cast<int64_t>(kDurFast * 1e6f)) continue;
    window->closing = false;
    ShowWindow(window->hwnd, SW_HIDE);
    window->visible = false;
    // Destroyed after the frame that draws it: a window cannot be destroyed while it draws.
    if (window->retire_on_hide) (window->tray ? retire_tray_ : retire_main_) = true;
    window->retire_on_hide = false;
  }
}

void UiHost::close_window() {
  panel_.buffer_stop_open = false;
  // Only one window exists at a time; the frame's own draw target wins, else whichever window is up.
  UiWindow* target = drawing_ ? drawing_ : (main_ ? main_.get() : tray_.get());
  if (!target) return;
  if (!target->tray) cancel_recorder();
  // Remember the size before hiding either window: a snap or keyboard resize has no WM_EXITSIZEMOVE.
  save_layout();
  begin_hide(*target, true);
}

void UiHost::no_drag(ImVec2 min, ImVec2 max) {
  if (drawing_) drawing_->no_drag.emplace_back(min, max);
}

void UiHost::drag_area(ImVec2 min, ImVec2 max) {
  if (drawing_) drawing_->drag.emplace_back(min, max);
}

void UiHost::capture_keys(bool on) {
  capturing_keys_ = on;
  keys_.clear();
}

bool UiHost::take_key(bool& ctrl, bool& alt, bool& shift, bool& win, unsigned& virtual_key, bool& down) {
  if (keys_.empty()) return false;
  const Key key = keys_.front();
  keys_.erase(keys_.begin());
  down = key.down;
  ctrl = key.ctrl;
  alt = key.alt;
  shift = key.shift;
  win = key.win;
  virtual_key = key.vk;
  return true;
}

void UiHost::pad_event(uint32_t pressed, uint32_t released) {
  {
    std::lock_guard<std::mutex> lock(pad_mutex_);
    pad_events_.emplace_back(pressed, released);
  }
  pad_pending_ = true;
  pad_edge_ms_ = static_cast<int64_t>(GetTickCount64());
  if (pressed) note_input(InputKind::pad);  // a release answers nothing, so only a press is latency
  wake();
}

void UiHost::inject_pad_nav() {
  ImGuiIO& io = ImGui::GetIO();
  // The pad moves the interface only while one of its windows is in front; a press in a game never
  // reaches it, and the recorder's capture owns the buttons while it listens.
  const bool wanted = any_visible() && foreground_is_ours() && !panel_.recorder_open && !capturing_keys_;
  auto send = [&](uint32_t button, bool down) {
    const PadNav nav = pad_nav_for(button);
    switch (nav) {
      case PadNav::previous_pane:
        if (down) io.AddKeyEvent(ImGuiKey_LeftShift, true);
        io.AddKeyEvent(ImGuiKey_Tab, down);
        if (!down) io.AddKeyEvent(ImGuiKey_LeftShift, false);
        break;
      case PadNav::next_pane: io.AddKeyEvent(ImGuiKey_Tab, down); break;
      case PadNav::up: io.AddKeyEvent(ImGuiKey_UpArrow, down); break;
      case PadNav::down: io.AddKeyEvent(ImGuiKey_DownArrow, down); break;
      case PadNav::left: io.AddKeyEvent(ImGuiKey_LeftArrow, down); break;
      case PadNav::right: io.AddKeyEvent(ImGuiKey_RightArrow, down); break;
      case PadNav::accept: io.AddKeyEvent(ImGuiKey_Enter, down); break;
      case PadNav::back: io.AddKeyEvent(ImGuiKey_Escape, down); break;
      case PadNav::space: io.AddKeyEvent(ImGuiKey_Space, down); break;
      case PadNav::help: io.AddKeyEvent(ImGuiKey_F1, down); break;
      case PadNav::none: break;
    }
  };
  if (wanted != pad_nav_on_) {
    pad_nav_on_ = wanted;
    // The poller reads the pads for navigation alone: no binding has to exist for the pad to move the panel.
    Json args = Json::object();
    args.set("on", Json(wanted));
    store_.bridge_call("pad.navigate", args, [](bool, const Json&, const std::string&) {});
    if (!wanted) {
      for (uint32_t bit = 1; bit; bit <<= 1) {
        if (pad_nav_held_ & bit) send(bit, false);
      }
      pad_nav_held_ = 0;
      pad_nav_dir_ = 0;
    }
  }
  if (!pad_nav_on_) {
    // Not moving the interface now: drop what arrived, or the first press after it comes to the front
    // would fire late, on its own.
    std::lock_guard<std::mutex> lock(pad_mutex_);
    pad_events_.clear();
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> events;
  {
    std::lock_guard<std::mutex> lock(pad_mutex_);
    events.swap(pad_events_);
  }
  if (!events.empty()) {
    // A press that took more than two frames to reach the picture says so once a second: it names the
    // machine, not the code, when someone reports the pad feeling late.
    const int64_t age = static_cast<int64_t>(GetTickCount64()) - pad_edge_ms_;
    if (age > 40 && GetTickCount64() - pad_log_ms_ > 1000) {
      pad_log_ms_ = GetTickCount64();
      BX_LOG_INFO("a controller press waited %lld ms before the panel drew it", static_cast<long long>(age));
    }
  }
  const int64_t now = static_cast<int64_t>(GetTickCount64());
  for (const auto& [pressed, released] : events) {
    for (uint32_t bit = 1; bit; bit <<= 1) {
      if (released & bit) {
        send(bit, false);
        pad_nav_held_ &= ~bit;
        if (pad_nav_dir_ == bit) pad_nav_dir_ = 0;
      }
      if (pressed & bit) {
        send(bit, true);
        pad_nav_held_ |= bit;
        if (pad_nav_for(bit) == PadNav::up || pad_nav_for(bit) == PadNav::down ||
            pad_nav_for(bit) == PadNav::left || pad_nav_for(bit) == PadNav::right) {
          pad_nav_dir_ = bit;
          pad_nav_first_ms_ = now;
          pad_nav_last_ms_ = now;
        }
      }
    }
  }
  // A held direction keeps moving: the release and the press in one frame is a fresh press to the panel.
  if (pad_nav_dir_ && pad_nav_repeat_due(now, pad_nav_first_ms_, pad_nav_last_ms_)) {
    send(pad_nav_dir_, false);
    send(pad_nav_dir_, true);
    pad_nav_last_ms_ = now;
  }
}

// ---------------------------------------------------------------- layout file

void UiHost::load_layout() {
  std::string text;
  if (FILE* file = std::fopen((data_root_ + "\\ui.json").c_str(), "rb")) {
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
  }
  Json layout;
  std::string error;
  if (text.empty() || !Json::parse(text, layout, error)) return;
  const Json& panel = layout["panel"];
  panel_rect_ = RECT{static_cast<LONG>(panel["x"].as_int()), static_cast<LONG>(panel["y"].as_int()),
                     static_cast<LONG>(panel["x"].as_int() + panel["width"].as_int()),
                     static_cast<LONG>(panel["y"].as_int() + panel["height"].as_int())};
  // A place saved in physical pixels is only a place while its monitor is there: a laptop undocked, a
  // display unplugged or a resolution changed leaves a rectangle on no monitor at all. That one is
  // dropped (the panel opens centred on the primary work area); a reachable one is fitted to the work
  // area it is on, so a saved window larger than the screen cannot open that way.
  if (panel_rect_.right <= panel_rect_.left || panel_rect_.bottom <= panel_rect_.top || !rect_on_screen(panel_rect_)) {
    panel_rect_ = RECT{0, 0, 0, 0};
  } else {
    panel_rect_ = fit_to_work(panel_rect_, work_area_of(MonitorFromRect(&panel_rect_, MONITOR_DEFAULTTONEAREST)));
  }
  quick_pos_ = POINT{static_cast<LONG>(layout["quick"]["x"].as_int(-1)), static_cast<LONG>(layout["quick"]["y"].as_int(-1))};
  quick_moved_ = layout["quick"]["moved"].as_bool();
  quick_pinned_ = layout["quick"]["pinned"].as_bool();
  // The size is kept in design units, so a monitor with another scale gives the same panel; a file with none, or one from
  // before it could be sized, opens at the default. The limits are applied when a window is placed.
  const int64_t quick_width = layout["quick"]["width"].as_int(0), quick_height = layout["quick"]["height"].as_int(0);
  if (quick_width > 0 && quick_height > 0) {
    quick_w_ = static_cast<float>(quick_width);
    quick_h_ = static_cast<float>(quick_height);
  }
  // The quick panel's own place, on a monitor that has gone since: it opens by the tray again.
  if (quick_moved_ && (quick_pos_.x < 0 || MonitorFromPoint(quick_pos_, MONITOR_DEFAULTTONULL) == nullptr)) quick_moved_ = false;
  panel_.welcomed = layout["welcomed"].as_bool();
  const Json& update = layout["update"];
  panel_.update_latest = update["latest"].as_string();
  panel_.update_notes = update["notes"].as_string();
  panel_.update_error = update["error"].as_string();
  panel_.update_checked_ms = update["checked_ms"].as_int(0);
  panel_.update_offered = update["offered"].as_string();
  panel_.update_prompted = update["prompted"].as_string();
  panel_.version_seen = update["version_seen"].as_string();
  panel_.pending_version = update["pending_version"].as_string();
  panel_.pending_notes = update["pending_notes"].as_string();
  // A tab this build may have dropped (or one a newer build wrote) lands on the clips: the rail has four.
  panel_.rail_tab = std::clamp(static_cast<int>(layout["rail_tab"].as_int(0)), 0, 3);
}

void UiHost::check_update(bool force) {
  if (panel_.update_checking) return;
  panel_.update_checking = true;
  if (force) panel_.update_offered.clear();
  panel_.update_error.clear();
  panel_.update_checked_ms = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  // Persist the attempt before networking, even if the app closes before its reply arrives.
  save_layout();
  Json args = Json::object();
  args.set("force", Json(force));
  store_.bridge_call("update.check", args, [this](bool ok, const Json& data, const std::string& error) {
    panel_.update_checking = false;
    if (!ok) {
      // A check that could not reach GitHub says so and keeps the answer it already had: a laptop that
      // opened offline must not lose the update it was already told about.
      panel_.update_error = error.empty() ? "GitHub could not be reached." : error;
    } else {
      if (data["found"].as_bool()) {
        panel_.update_latest = data["latest"].as_string();
        panel_.update_notes = data["notes"].as_string();
      }
      else {
        panel_.update_latest.clear();
        panel_.update_notes.clear();
      }
      panel_.update_error = data["error"].as_string();
      panel_.update_checked_ms = data["checked_ms"].as_int(panel_.update_checked_ms);
    }
    save_layout();
  });
}

void UiHost::install_update() {
  // The engine verifies the installer once more, stops the outputs, starts it silently and quits. This request
  // is asynchronous on purpose: the reply would otherwise have to arrive on this very window thread.
  panel_.update_restart_open = false;
  store_.bridge_call("update.install", Json::object(), [this](bool ok, const Json&, const std::string& error) {
    if (!ok) {
      store_.toast(UiToast::error, "The update did not start",
                   error.empty() ? "Try again in a moment, or use the release page." : error);
    }
    store_.dirty = true;
  });
}

void UiHost::save_layout() {
  if (main_ && main_->hwnd && !editor_mode_) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    GetWindowPlacement(main_->hwnd, &placement);
    panel_rect_ = placement.rcNormalPosition;
  }
  if (tray_ && tray_->hwnd) {
    // The quick panel keeps its size in design units so another monitor's scale keeps it; reading the
    // live rect catches a snap or keyboard resize that never sent WM_EXITSIZEMOVE.
    RECT quick{};
    GetWindowRect(tray_->hwnd, &quick);
    quick_w_ = quick_design(quick.right - quick.left, tray_->scale);
    quick_h_ = quick_design(quick.bottom - quick.top, tray_->scale);
  }
  Json layout = Json::object();
  Json panel = Json::object();
  panel.set("x", Json(static_cast<int64_t>(panel_rect_.left)));
  panel.set("y", Json(static_cast<int64_t>(panel_rect_.top)));
  panel.set("width", Json(static_cast<int64_t>(panel_rect_.right - panel_rect_.left)));
  panel.set("height", Json(static_cast<int64_t>(panel_rect_.bottom - panel_rect_.top)));
  layout.set("panel", panel);
  Json quick = Json::object();
  quick.set("x", Json(static_cast<int64_t>(quick_pos_.x)));
  quick.set("y", Json(static_cast<int64_t>(quick_pos_.y)));
  quick.set("moved", Json(quick_moved_));
  quick.set("pinned", Json(quick_pinned_));
  quick.set("width", Json(static_cast<int64_t>(std::lround(quick_w_))));
  quick.set("height", Json(static_cast<int64_t>(std::lround(quick_h_))));
  layout.set("quick", quick);
  layout.set("welcomed", Json(panel_.welcomed));
  layout.set("rail_tab", Json(panel_.rail_tab));
  // The last check survives a restart, so the About page says when it happened and the daily throttle (a
  // later task) has a timestamp to read.
  Json update = Json::object();
  update.set("latest", Json(panel_.update_latest));
  update.set("notes", Json(panel_.update_notes));
  update.set("error", Json(panel_.update_error));
  update.set("checked_ms", Json(panel_.update_checked_ms));
  update.set("offered", Json(panel_.update_offered));
  update.set("prompted", Json(panel_.update_prompted));
  update.set("version_seen", Json(panel_.version_seen));
  update.set("pending_version", Json(panel_.pending_version));
  update.set("pending_notes", Json(panel_.pending_notes));
  layout.set("update", update);
  const std::string path = data_root_ + "\\ui.json";
  write_file_atomic(path, layout.dump(2));
}

// ---------------------------------------------------------------- the frame

bool UiHost::capture(const std::string& view, const std::string& png_path, std::string& error) {
  std::unique_lock<std::mutex> lock(requests_mutex_);
  if (!started_) {
    error = "the interface is not running";
    return false;
  }
  if (capture_.pending) {
    error = "a capture is already running";
    return false;
  }
  capture_ = Capture{};
  capture_.view = view;
  capture_.path = png_path;
  capture_.pending = true;
  wake();
  if (!capture_done_.wait_for(lock, std::chrono::seconds(15), [&] { return capture_.done; })) {
    capture_.pending = false;
    error = "the interface did not draw in time";
    return false;
  }
  error = capture_.error;
  return capture_.ok;
}

bool UiHost::input(const Json& args, std::string& error) {
  if (!started_) {
    error = "the interface is not running";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    inputs_.push_back(InputRequest{args, GetTickCount()});
  }
  note_input(ui_input_kind(args.has("click"), args.has("keys") || args.has("chord") || args.has("text"), args.has("wheel")));
  wake();
  return true;
}

void UiHost::apply_inputs() {
  std::vector<InputRequest> inputs;
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    inputs.swap(inputs_);
  }
  const uint32_t now = GetTickCount();
  std::vector<InputRequest> waiting;
  for (InputRequest& request : inputs) {
    const Json& args = request.args;
    UiWindow* window = args["window"].as_string("main") == "tray" ? tray_.get() : main_.get();
    if (!window || !window->visible || !window->imgui) {
      // The window it is for is not on screen yet (ui.show and the click in one breath): keep it for a moment instead
      // of dropping it, and never so long that it fires at a picture the caller was not looking at.
      if (now - request.queued_ms <= kInputQueueMs) waiting.push_back(request);
      continue;
    }
    // A chord goes where the hotkey recorder listens, the way a real key press does.
    if (args.has("chord")) {
      UINT modifiers = 0, key = 0;
      std::string problem;
      const std::string chord = args["chord"].as_string();
      if (capturing_keys_ && (parse_chord(chord, modifiers, key, problem) || key != 0)) {
        keys_.push_back(Key{(modifiers & MOD_CONTROL) != 0, (modifiers & MOD_ALT) != 0, (modifiers & MOD_SHIFT) != 0,
                            (modifiers & MOD_WIN) != 0, key, true});
      }
    }
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(window->imgui);
    ImGuiIO& io = ImGui::GetIO();
    if (args["click"].is_array() && args["click"].items().size() == 2) {
      const float x = static_cast<float>(args["click"].items()[0].as_double());
      const float y = static_cast<float>(args["click"].items()[1].as_double());
      const int button = static_cast<int>(args["button"].as_int(0));
      // ImGui spreads queued events over frames: hover, press, release, then the pointer leaves again.
      io.AddMousePosEvent(x, y);
      io.AddMouseButtonEvent(button, true);
      io.AddMouseButtonEvent(button, false);
      if (args["double"].as_bool()) {
        io.AddMouseButtonEvent(button, true);
        io.AddMouseButtonEvent(button, false);
      }
      if (!args["stay"].as_bool()) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    // The pointer alone, to hover (a tooltip, a hover state) without pressing anything.
    if (args["move"].is_array() && args["move"].items().size() == 2) {
      io.AddMousePosEvent(static_cast<float>(args["move"].items()[0].as_double()), static_cast<float>(args["move"].items()[1].as_double()));
    }
    // `mods` ("ctrl", "shift", "alt", joined by +) are held for the keys of this call.
    const std::string mods = args["mods"].as_string();
    auto modifier = [&](const char* name, ImGuiKey key, bool down) {  // the virtual modifier keys, as the Win32 backend sends them
      if (mods.find(name) != std::string::npos) io.AddKeyEvent(key, down);
    };
    for (const Json& name : args["keys"].items()) {
      static const std::pair<const char*, ImGuiKey> keys[] = {
          {"Escape", ImGuiKey_Escape}, {"Enter", ImGuiKey_Enter},       {"Tab", ImGuiKey_Tab},
          {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow}, {"Up", ImGuiKey_UpArrow},
          {"Down", ImGuiKey_DownArrow}, {"Backspace", ImGuiKey_Backspace}, {"Delete", ImGuiKey_Delete},
          {"End", ImGuiKey_End},         {"Home", ImGuiKey_Home}, {"Space", ImGuiKey_Space}, {"Comma", ImGuiKey_Comma},
          {"Slash", ImGuiKey_Slash}};
      const std::string text = name.as_string();
      ImGuiKey found = ImGuiKey_None;
      for (const auto& key : keys) {
        if (text == key.first) found = key.second;
      }
      // One capital letter or digit, or F1-F12.
      if (found == ImGuiKey_None && text.size() == 1 && text[0] >= 'A' && text[0] <= 'Z') found = static_cast<ImGuiKey>(ImGuiKey_A + (text[0] - 'A'));
      if (found == ImGuiKey_None && text.size() == 1 && text[0] >= '0' && text[0] <= '9') found = static_cast<ImGuiKey>(ImGuiKey_0 + (text[0] - '0'));
      if (found == ImGuiKey_None && text.size() >= 2 && text[0] == 'F') {
        const int number = std::atoi(text.c_str() + 1);
        if (number >= 1 && number <= 12) found = static_cast<ImGuiKey>(ImGuiKey_F1 + number - 1);
      }
      if (found == ImGuiKey_None) continue;
      modifier("ctrl", ImGuiMod_Ctrl, true);
      modifier("shift", ImGuiMod_Shift, true);
      modifier("alt", ImGuiMod_Alt, true);
      io.AddKeyEvent(found, true);
      io.AddKeyEvent(found, false);
      modifier("ctrl", ImGuiMod_Ctrl, false);
      modifier("shift", ImGuiMod_Shift, false);
      modifier("alt", ImGuiMod_Alt, false);
    }
    // The wheel, for reaching anything below the fold in a screenshot check: `{"wheel":-1}` is one
    // notch up. A notch needs the pointer already over the window, so a "click" with "stay" in the
    // same call positions it first. `wheel_this_frame_` lifts NoScrollWithMouse for this frame only.
    if (args.has("wheel")) {
      io.AddMouseWheelEvent(0.0f, static_cast<float>(args["wheel"].as_double()));
      wheel_this_frame_ = true;
    }
    if (args.has("text")) io.AddInputCharactersUTF8(args["text"].as_string().c_str());
    ImGui::SetCurrentContext(previous);
    // An injected move on its own is the pointer alone, like a real one; anything else is drawn at the display's rate.
    window->pointer_only = args.has("move") && !args.has("click") && !args.has("keys") && !args.has("wheel") && !args.has("text") && !args.has("chord");
    window->dirty = true;
    window->settle_us = now_us() + kSettleUs;
  }
  if (!waiting.empty()) {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    inputs_.insert(inputs_.begin(), waiting.begin(), waiting.end());  // still first in line
  }
}

namespace {
// A digest of what a frame would draw: every list's vertices and indices, every command's clip, texture and range, and the
// colour the target is cleared to. Equal digests mean the pixels would be the same.
uint64_t draw_digest(const ImDrawData* data, const float clear[4]) {
  uint64_t h = digest_bytes(kDigestSeed, clear, 4 * sizeof(float));
  if (!data) return h;
  h = digest_bytes(h, &data->DisplaySize, sizeof(data->DisplaySize));
  for (int n = 0; n < data->CmdListsCount; ++n) {
    const ImDrawList* list = data->CmdLists[n];
    h = digest_bytes(h, list->VtxBuffer.Data, static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
    h = digest_bytes(h, list->IdxBuffer.Data, static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      const ImTextureID texture = cmd.GetTexID();
      h = digest_bytes(h, &cmd.ClipRect, sizeof(cmd.ClipRect));
      h = digest_bytes(h, &texture, sizeof(texture));
      h = digest_bytes(h, &cmd.VtxOffset, sizeof(cmd.VtxOffset));
      h = digest_bytes(h, &cmd.IdxOffset, sizeof(cmd.IdxOffset));
      h = digest_bytes(h, &cmd.ElemCount, sizeof(cmd.ElemCount));
    }
  }
  return h;
}

// Adds "name N ms" to a frame report when a part of the frame took long enough to matter.
void time_part(std::string& report, const char* name, uint32_t started) {
  const uint32_t took = GetTickCount() - started;
  if (took < 20) return;
  if (!report.empty()) report += ", ";
  report += std::string(name) + " " + std::to_string(took) + " ms";
}
}  // namespace

void UiHost::frame() {
  if (!started_) return;
  // The graphics device went away under us (a driver reset, a TDR): every swap chain and texture went with it. The
  // device, the windows and their atlases are rebuilt by this pass rather than drawn into every frame.
  if (device_lost_) {
    device_lost_ = false;
    const bool had_main = main_ && main_->visible, had_tray = tray_ && tray_->visible;
    BX_LOG_WARN("the interface lost its graphics device; rebuilding it");
    // An embedded editor's child window died with the panel. Leave editor mode first, or the rebuilt panel would
    // draw nothing and its close button would post to a window that no longer exists. The editor gets the same
    // grace period as one whose window never arrived.
    if (editor_mode_ && editor_process_) {
      // One already given up on is ended now, not orphaned by this one taking the slot.
      if (editor_end_process_) {
        TerminateProcess(editor_end_process_, 0);
        CloseHandle(editor_end_process_);
        editor_end_process_ = nullptr;
      }
      editor_end_process_ = editor_process_;
      editor_process_ = nullptr;
      editor_end_at_ = GetTickCount() + kEditorKillGraceMs;
      request_editor_close();
      finish_editor();
    }
    release_device();
    if (had_main) show_panel();
    else if (had_tray) show_tray();
  }
  // An editor that was given up on is ended once its grace period is over, unless it went by itself.
  if (editor_end_process_) {
    const bool gone = WaitForSingleObject(editor_end_process_, 0) == WAIT_OBJECT_0;
    if (gone || static_cast<int32_t>(GetTickCount() - editor_end_at_) >= 0) {
      if (!gone) {
        BX_LOG_WARN("the editor did not close after its loading timeout; ending it");
        TerminateProcess(editor_end_process_, 0);
      }
      CloseHandle(editor_end_process_);
      editor_end_process_ = nullptr;
    }
  }
  if (editor_process_) {
    if (WaitForSingleObject(editor_process_, 0) == WAIT_OBJECT_0) {
      DWORD code = 0;
      GetExitCodeProcess(editor_process_, &code);
      const bool returning = editor_returning_;
      finish_editor();
      if (code != 0 && !returning) store_.toast(UiToast::error, "The editor closed unexpectedly", "Open the editor again to continue.");
    }
    else {
      size_editor();
      // Alive but its window never arrived: the panel would stay on "LOADING EDITOR" with every control disabled,
      // close included. Let go of it, ask it to close, and end it after the grace period if it does not.
      const uint32_t at = GetTickCount();
      if (editor_loading_ && !editor_loading_since_) editor_loading_since_ = at;
      if (editor_loading_ && at - editor_loading_since_ > kEditorLoadTimeoutMs) {
        editor_loading_since_ = 0;
        editor_end_process_ = editor_process_;
        editor_process_ = nullptr;
        editor_end_at_ = at + kEditorKillGraceMs;
        request_editor_close();
        finish_editor();
        store_.toast(UiToast::error, "The editor did not open", "Try opening the editor again.");
      }
    }
  }
  // The offer was answered or dismissed: the same clipboard text may be offered again if it is copied
  // again (the clipboard sequence changes), instead of staying muted for the rest of the run.
  if (panel_.link_offer.empty() && !offered_link_.empty()) offered_link_.clear();
  const uint32_t now = GetTickCount();
  frame_report_.clear();
  uint32_t part = GetTickCount();
  process_requests();
  time_part(frame_report_, "opening windows", part);
  finish_hides();  // a hide whose fade is over hides its window now (V2)
  // Windows can mark a quiet, covered panel as not responding even while this loop continues to run.
  // A queued message to the window clears that state. Post from this thread, so a genuinely blocked
  // window thread cannot keep itself looking responsive. No messages are posted while windows are hidden.
  for (UiWindow* window : {main_.get(), tray_.get()}) {
    if (window && window->visible && now - window->last_message_pulse_ms >= 1000) {
      window->last_message_pulse_ms = now;
      if (!IsIconic(window->hwnd)) PostMessageW(window->hwnd, WM_NULL, 0, 0);
    }
  }
  // A pinned quick panel steps aside while a fullscreen game runs: a topmost window over it can cost the game its direct flip (and
  // its focus). It comes back on top when the game is gone.
  if (tray_ && tray_->visible && quick_pinned_ && now - pinned_checked_ms_ > 500) {
    pinned_checked_ms_ = now;
    const bool aside = GetForegroundWindow() != tray_->hwnd && fullscreen_app_running();
    if (aside != quick_aside_) {
      quick_aside_ = aside;
      SetWindowPos(tray_->hwnd, aside ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }
  apply_inputs();
  // A pad press is input like a keystroke: the pass that wakes for it draws it, instead of the edge waiting
  // for some unrelated repaint. This is what made controller navigation feel late.
  const bool pad_urgent = pad_pending_.exchange(false);
  if (pad_urgent) {
    if (main_ && main_->visible) main_->dirty = true;
    if (tray_ && tray_->visible) tray_->dirty = true;
  }

  // A capture opens what it needs and waits for the images on it to load (or a second and a half).
  static uint32_t capture_started = 0;
  Capture capture;
  {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    capture = capture_;
  }
  UiWindow* capture_window = nullptr;
  if (!capture.pending) capture_started = 0;  // a capture that timed out leaves nothing behind
  const bool small_view = capture.view == "tray" || capture.view == "compact";
  if (capture.pending) {
    if (!capture_started) {
      capture_started = now;
      const std::string& view = capture.view;
      if (view == "current") {
        // What is on screen now, as it is: a preview, a dialog, an open section.
      } else if (small_view) {
        if (!tray_ || !tray_->visible) show_tray();
      } else if (view == "share") {
        // The share sheet on the newest clip, as a person would open it from the card's menu.
        panel_.settings.clear();
        panel_.preview_open = false;
        if (!store_.clips.empty()) parts::open_share(store_, panel_, store_.clips.front());
        show_panel();
      } else {
        panel_.settings = view.rfind("settings:", 0) == 0 ? view.substr(9) : std::string();
        panel_.preview_open = false;
        if (panel_.share_open) parts::close_share(store_, panel_);
        show_panel();
      }
    }
    if (capture.view == "current") capture_window = tray_ && tray_->visible ? tray_.get() : main_.get();
    else capture_window = small_view ? tray_.get() : main_.get();
    if (capture_window && !capture_window->visible) capture_window = nullptr;
  }

  wait_hint_ms_ = 250;  // a quiet window's next deadline: the game and pinned checks (500 ms) run on a pass at least every 250 ms
  std::vector<std::pair<std::string, Json>> events;
  part = GetTickCount();
  bool changed = bridge_.pump(events);
  for (const auto& event : events) {
    if (event.first == "ui.show") request(event.second["view"].as_string("main"));
    else if (event.first == "ui.toggle") request("toggle");
    else if (event.first == "ui.share") open_share_for(event.second["file"].as_string());
    else {
      if (event.first == "link.received") {
        // Do not cover an editor that may contain unsaved work.
        received_preview_ = editor_mode_ ? std::string() : event.second["file"].as_string();
        received_preview_until_ = GetTickCount() + 15000;
        if (!received_preview_.empty()) show_panel();
      }
      store_.handle_event(event.first, event.second);
    }
  }
  // Link updates precede a clipboard check, so a link copied by another BuffrX window is not offered back.
  check_clipboard_for_link();
  if (!events.empty()) process_requests();
  changed = store_.tick(now) || changed;
  const int64_t wall_ms = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  if (update_should_check(any_visible(), buffer_active_.load(std::memory_order_relaxed), panel_.update_checking, panel_.update_checked_ms, wall_ms))
    check_update(false);
  // A quit during a download leaves no pending installer. Resume it on this run even if today's check already happened.
  if (kAutoUpdate && !update_resume_checked_ && !panel_.update_checking &&
      (any_visible() || buffer_active_.load(std::memory_order_relaxed)) &&
      store_.status["update"]["state"].as_string() == "idle" &&
      update_is_newer(panel_.update_latest, store_.hello["engine"].as_string())) {
    update_resume_checked_ = true;
    check_update(false);
  }
  // A receive that never reaches a listing (a failed move, a folder that went away) must not wait for
  // the rest of the session and then open the panel over whatever is in front.
  if (!received_preview_.empty() && static_cast<int32_t>(GetTickCount() - received_preview_until_) >= 0) {
    received_preview_.clear();
  }
  if (!received_preview_.empty()) {
    const auto has_file = [&](const std::vector<UiMedia>& items) {
      return std::any_of(items.begin(), items.end(), [&](const UiMedia& item) { return item.file == received_preview_; });
    };
    if (has_file(store_.clips) || has_file(store_.screenshots)) {
      panel_.preview_files = {received_preview_};
      panel_.preview_index = 0;
      panel_.preview_zoom = false;
      panel_.preview_from_compact = false;
      panel_.preview_open = true;
      received_preview_.clear();
      // The event already asked for the panel; asking again from a later listing is what could put it
      // over a game after the person had closed it.
      changed = true;
    }
  }
  time_part(frame_report_, "replies and events", part);
  part = GetTickCount();
  if (device_) {
    // A few textures a frame, not every decoded image at once: a library that scrolls into view
    // used to create dozens of textures in one frame on the window thread.
    const bool uploaded = images_.upload(4, [this](const uint8_t* bgra, int width, int height) -> ImTextureID {
      D3D11_TEXTURE2D_DESC description{};
      description.Width = static_cast<UINT>(width);
      description.Height = static_cast<UINT>(height);
      description.MipLevels = 1;
      description.ArraySize = 1;
      description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      description.SampleDesc.Count = 1;
      description.Usage = D3D11_USAGE_IMMUTABLE;
      description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA data{bgra, static_cast<UINT>(width * 4), 0};
      ID3D11Texture2D* texture = nullptr;
      ID3D11ShaderResourceView* view = nullptr;
      if (SUCCEEDED(device_->CreateTexture2D(&description, &data, &texture))) {
        device_->CreateShaderResourceView(texture, nullptr, &view);
      }
      release(texture);
      return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(view));
    });
    bool released = false;
    // 256 MiB of BGRA textures: a count of 96 entries said nothing about their size, and browsing 4K/8K screenshots
    // at 100% zoom could hold gigabytes in the cache.
    images_.trim([&released](ImTextureID texture) {
      reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<intptr_t>(texture))->Release();
      released = true;
    }, 256u * 1024u * 1024u);
    changed = uploaded || changed;
    // The digest sees texture handles, not pixels, and a freed handle can come back for another picture: the next frame goes out.
    if (uploaded || released) {
      for (UiWindow* window : {main_.get(), tray_.get()}) {
        if (window) window->digest_valid = false;
      }
    }
  }
  time_part(frame_report_, "image uploads", part);

  // The library's characters decide the font atlas: a new one (a game folder in Japanese) rebuilds it.
  // Read only when a list arrives or the capture target changes, never on every pass.
  bool grew = false;
  if (store_.library_version != glyphs_library_version_) {
    glyphs_library_version_ = store_.library_version;
    for (const UiMedia& item : store_.clips) {
      collect_glyphs(item.folder, glyphs_, grew);
      collect_glyphs(item.name, glyphs_, grew);
    }
    for (const UiMedia& item : store_.screenshots) collect_glyphs(item.name, glyphs_, grew);
  }
  const std::string target = store_.status["capture"]["target"].as_string();
  if (target != glyphs_target_) {
    glyphs_target_ = target;
    collect_glyphs(target, glyphs_, grew);
  }
  if (grew) {
    if (main_) main_->fonts_stale = true;
    if (tray_) tray_->fonts_stale = true;
  }

  const int scale_percent = std::clamp(static_cast<int>(store_.config["ui_scale_percent"].as_int(ui_scale_percent_)), 75, 150);
  ui_scale_percent_ = scale_percent;
  for (UiWindow* window : {main_.get(), tray_.get()}) {
    if (!window || !window->hwnd) continue;
    const RECT work = work_area_for(window->hwnd);
    const float requested = window_dpi(window->hwnd) / 96.0f * kUiScale * ui_scale_percent_ / 100.0f;
    const float next = window->tray ? requested : panel_scale_to_fit(requested, work.right - work.left, work.bottom - work.top);
    if (std::fabs(next - window->scale) < 0.0001f) continue;
    const float ratio = window->scale > 0 ? next / window->scale : 1.0f;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    const bool minimized = IsIconic(window->hwnd) && GetWindowPlacement(window->hwnd, &placement);
    RECT rect{};
    if (minimized) rect = placement.rcNormalPosition;
    else GetWindowRect(window->hwnd, &rect);
    rect.right = rect.left + static_cast<LONG>(std::lround((rect.right - rect.left) * ratio));
    rect.bottom = rect.top + static_cast<LONG>(std::lround((rect.bottom - rect.top) * ratio));
    window->scale = next;
    window->fonts_stale = true;
    window->dirty = true;
    panel_.deck_pad = 0;
    panel_.deck_pad_seen = -1;
    panel_.deck_fill_ready = false;
    const RECT placed = window->tray ? tray_place(work, next, quick_pos_, quick_moved_, quick_w_, quick_h_)
                                     : panel_place(work, next, rect, true, editor_mode_);
    if (minimized) {
      placement.rcNormalPosition = placed;
      SetWindowPlacement(window->hwnd, &placement);
    } else {
      SetWindowPos(window->hwnd, nullptr, placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!window->tray) panel_rect_ = placed;
    changed = true;
  }

  windows_visible_.store((main_ && main_->visible) || (tray_ && tray_->visible), std::memory_order_relaxed);
  published_main_.store(main_ && main_->visible ? main_->hwnd : nullptr, std::memory_order_relaxed);
  published_tray_.store(tray_ && tray_->visible ? tray_->hwnd : nullptr, std::memory_order_relaxed);
  const bool store_changed = store_.dirty || changed;
  UiWindow* paced = nullptr;
  // A fullscreen game running while a window is open (it sits on another screen): the interface stops following the display.
  if ((main_ && main_->visible) || (tray_ && tray_->visible)) {
    if (now - game_checked_ms_ > 500) {
      game_checked_ms_ = now;
      game_running_ = fullscreen_app_running();
    }
  } else {
    game_running_ = false;
  }
  game_flag_.store(game_running_, std::memory_order_relaxed);
  for (UiWindow* window : {main_.get(), tray_.get()}) {
    if (!window || !window->visible || IsIconic(window->hwnd)) continue;
    // The picture's surface stays under every other child (the clip page, the editor): a new child or an activation can raise it.
    if (window->surface && GetWindow(window->surface, GW_HWNDNEXT))
      SetWindowPos(window->surface, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const bool forced = window == capture_window;
    ViewEnv env{store_, images_, *this, panel_, now};
    const Animation animation = animation_reason(env);
    const bool images_busy = images_.loading();
    // What is on screen may need a frame by its own period (a blink on the half second, a spinner at ~30 a second); thumbnails
    // still loading keep the old 62 a second.
    const int64_t current_us = now_us();
    const bool animation_due = animation.name && animation_frame_due(animation.period_ms, animation.aligned, current_us, window->last_present_us);
    const bool spinning = window->spinning && animation_frame_due(kSpinnerMs, false, current_us, window->last_present_us);
    const int64_t since_present = current_us - window->last_present_us;
    window->focused = GetForegroundWindow() == window->hwnd;
    // Something asked for this frame (an input, new data), or something moves slowly and is due again.
    // `urgent` follows the display; the settle frame after an input (a tooltip finishing) and the slow animations do not.
    const bool urgent = forced || store_changed || window->dirty || window->moving;
    const bool settle = settle_due(window->settle_us, current_us);
    // A game holds a frame like a slow one only while it runs and this window is not the one the user operates (Phase 1.3).
    const bool game_holds = game_running_ && !window->focused;
    const bool urgent_due = urgent && window_frame_due(window->focused, forced, game_holds, since_present);
    // The loop wakes for the gap's end rather than polling at the 20 ms pass.
    if (urgent && !urgent_due) {
      const int64_t gap = window->focused ? kSlowFrameGapUs : kUnfocusedGapUs;
      wait_hint_ms_ = std::min<uint32_t>(wait_hint_ms_, static_cast<uint32_t>((gap - since_present) / 1000 + 1));
    }
    const bool images_due = images_busy && slow_frame_due(since_present) && (window->focused || since_present >= kUnfocusedGapUs);
    const bool ambient_due = images_due || settle || animation_due || spinning;
    // What is still pending sets the next pass: an animation's period, a spinner's 33 ms, thumbnails still loading, a settle frame.
    if (animation.name) wait_hint_ms_ = std::min<uint32_t>(wait_hint_ms_, std::max<uint32_t>(animation.period_ms, 20u));
    if (window->spinning || window->moving || images_busy) wait_hint_ms_ = std::min<uint32_t>(wait_hint_ms_, window->spinning ? 33u : 20u);
    if (window->settle_us != 0)
      wait_hint_ms_ = std::min<uint32_t>(wait_hint_ms_, static_cast<uint32_t>(std::max<int64_t>(4, (window->settle_us - current_us) / 1000 + 1)));
    if (urgent_due || ambient_due) {
      // A pad edge draws at once: one refresh of waiting for the display's slot is a whole press's worth of delay.
      if (forced || pad_urgent || display_ready(*window, game_holds)) {
        part = GetTickCount();
        note_reasons(forced, store_changed, window->dirty, window->moving, settle, animation.name, images_busy);
        render(*window, false, !game_holds);
        if (settle) window->settle_us = 0;  // drawn: the frame that follows the input is answered
        time_part(frame_report_, window->tray ? "drawing the tray panel" : "drawing the panel", part);
      } else if (urgent) {
        window->dirty = true;  // held back for the display: the store's flag is cleared below, this one is not
      }
      // An asked-for frame may be followed by more: the loop waits for the display's next frame instead of sleeping.
      if (urgent_due && window->pace && !game_holds) paced = window;
    } else if (urgent) {
      window->dirty = true;  // capped while a game runs: the next pass draws it
    }
  }
  store_.dirty = false;

  // Windows that were closed are destroyed here, after the frame that drew them: a closed window
  // gives its swapchain, its ImGui context and its font atlas back.
  if (retire_main_) {
    retire_main_ = false;
    destroy_window(main_);
  }
  if (retire_tray_) {
    retire_tray_ = false;
    destroy_window(tray_);
  }
  paced_window_ = (paced && (paced == main_.get() || paced == tray_.get())) ? paced : nullptr;
  paced_.store((main_ && main_->visible && main_->pace) || (tray_ && tray_->visible && tray_->pace), std::memory_order_relaxed);
  focused_.store((main_ && main_->visible && main_->focused) || (tray_ && tray_->visible && tray_->focused), std::memory_order_relaxed);

  if (capture.pending && capture_window && capture_started &&
      (now - capture_started > 1500 || (now - capture_started > 400 && !images_.loading()))) {
    capture_started = 0;
    render(*capture_window, true);
  } else if (capture.pending && !capture_window && capture_started && now - capture_started > 3000) {
    capture_started = 0;
    std::lock_guard<std::mutex> lock(requests_mutex_);
    // "current" opens nothing: with nothing on screen there is nothing it could have captured, and
    // telling the caller a window could not be opened names the wrong fault.
    capture_.error = capture.view == "current" ? "no window is open" : "the window could not be opened";
    capture_.done = true;
    capture_.pending = false;
    capture_done_.notify_all();
  }

  // Nothing open: let the device, the windows and every texture go after a while.
  if (device_ && !any_visible() && !capture.pending) {
    if (!hidden_since_) hidden_since_ = now;
    store_.active = false;
    if (now - hidden_since_ > kReleaseAfterMs) release_device();
  } else {
    hidden_since_ = 0;
  }
}

void UiHost::render(UiWindow& window, bool for_capture, bool vsync) {
  // No context, no swap chain or no render target (a lost device, a resize that could not make one): nothing to draw
  // into. The next pass rebuilds it instead of presenting a frame that was never drawn.
  if (!window.imgui || !window.swap || !window.target) return;
  ImGui::SetCurrentContext(window.imgui);
  drawing_ = &window;
  const std::string theme = store_.theme();
  if (window.fonts_stale || window.theme != theme) {
    const float scale = window.scale;
    if (window.fonts_stale || scale != window.scale) {
      const uint32_t fonts_started = GetTickCount();
      window.scale = scale;
      ImGui_ImplDX11_InvalidateDeviceObjects();
      if (!build_fonts(ImGui::GetIO().Fonts, font_dir_, window.scale, glyphs_, window.glyph_ranges, window.fonts)) {
        BX_LOG_WARN("the interface fonts were not found in %s; using ImGui's own", font_dir_.c_str());
        ImGui::GetIO().Fonts->Clear();
        ImFont* fallback = ImGui::GetIO().Fonts->AddFontDefault();
        window.fonts = Fonts{fallback, fallback, fallback, fallback, fallback, fallback, fallback, fallback};
      }
      ImGui_ImplDX11_CreateDeviceObjects();
      capitalise_glyphs(ImGui::GetIO().Fonts);
      window.fonts_stale = false;
      time_part(frame_report_, "building the fonts", fonts_started);
    }
    window.theme = theme;
    apply_style(palette_for(theme), window.scale);
  }
  window.fonts_due = false;  // the atlas is rebuilt before anything is drawn with it
  const Palette palette = palette_for(theme);
  set_theme(&palette, &window.fonts, window.scale);

  window.drag.clear();
  window.no_drag.clear();
  const int64_t build_started_us = now_us();
  inject_pad_nav();
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  // The panel never scrolls with the mouse on purpose - a stray wheel over the panel must not move
  // the page under someone's cursor. An injected wheel lifts that for its own frame, so a screenshot
  // check can reach anything below the fold. Nothing else about the frame changes.
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  ImGui::PushFont(window.fonts.body);
  ImGui::Begin(window.tray ? "tray" : "panel", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   // The panel never scrolls with the mouse on purpose - a stray wheel over it must
                   // not move the page under someone's cursor - and ImGui passes that down to every
                   // child, so an injected wheel has nothing to scroll either. The frame a wheel
                   // arrives on lifts it, which is what makes a screenshot check able to reach
                   // anything below the fold.
                   (wheel_this_frame_ ? ImGuiWindowFlags(0) : ImGuiWindowFlags_NoScrollWithMouse));
  ViewEnv env{store_, images_, *this, panel_, GetTickCount()};
  // The monitor's pixels: a preview at 100% is decoded no larger than the display it is shown on (B17).
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  if (GetMonitorInfoW(MonitorFromWindow(window.hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
    env.display_w = static_cast<int>(monitor.rcMonitor.right - monitor.rcMonitor.left);
    env.display_h = static_cast<int>(monitor.rcMonitor.bottom - monitor.rcMonitor.top);
  }
  if (window.tray) { draw_tray(env); maybe_show_whats_new(env); }
  else if (!editor_mode_ || editor_loading_) {
    if (editor_loading_) ImGui::BeginDisabled();
    draw_panel(env);
    maybe_show_whats_new(env);
    maybe_show_update_restart(env);
    if (editor_loading_) ImGui::EndDisabled();
  }
  ImGui::End();
  ImGui::PopFont();
  // A popup lies over the titlebar's drag area and its padding and edge are not items, so Windows took a grab on
  // them for a grab on the titlebar and moved the window (the theme list). Whatever is open is a client area.
  for (const ImGuiPopupData& popup : ImGui::GetCurrentContext()->OpenPopupStack) {
    if (!popup.Window) continue;
    window.no_drag.emplace_back(popup.Window->Pos,
                                ImVec2(popup.Window->Pos.x + popup.Window->Size.x, popup.Window->Pos.y + popup.Window->Size.y));
  }
  // The keyboard drives the panel and the quick panel itself; ImGui's own arrow and Tab navigation joins in only
  // for Settings and the dialogs, where fields and buttons are what the keys move between. The quick panel keeps it
  // too: its switches and buttons used to be mouse-only.
  // A window that has just opened comes in from the ground; anything still moving asks for the next frame.
  const bool just_opened = window.opening;
  if (window.opening) {
    window.opening = false;
    window.shown_us = now_us();
  }
  float rise = 0.0f;  // V1: the content rises 8 px as the window's ground fades away
  if (window.closing) {
    // V2: the ground comes up over the content on the exit token, ease-in: the window leaves sooner than it arrived.
    anim_touch();
    const float leaving = progress(static_cast<double>(now_us() - window.closing_us) / 1e6, kDurFast);
    fade_in_from(ImGui::GetForegroundDrawList(), ImVec2(0, 0), ImGui::GetIO().DisplaySize, palette.canvas, 1.0f - ease_in(leaving));
  }
  if (anim_enabled() && !window.tray) {
    const float opened = ease_out(progress(static_cast<double>(now_us() - window.shown_us) / 1e6, kDurSlow));
    if (opened < 1.0f) {
      anim_touch();
      rise = px(8) * (1.0f - opened);
      fade_in_from(ImGui::GetForegroundDrawList(), ImVec2(0, 0), ImGui::GetIO().DisplaySize, palette.canvas, opened);
    }
  }
  if (!window.tray && editor_loading_) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    draw->AddRectFilled(ImVec2(0, 0), size, with_alpha(palette.canvas, 0.82f));
    const ImVec2 center(size.x * 0.5f, size.y * 0.5f - px(12));
    spinner(draw, center, px(10), palette.accent);
    const char* label = "LOADING EDITOR...";
    const ImVec2 text_size = window.fonts.body->CalcTextSizeA(window.fonts.body->LegacySize, FLT_MAX, 0, label);
    draw->AddText(window.fonts.body, window.fonts.body->LegacySize,
                  ImVec2(center.x - text_size.x * 0.5f, center.y + px(24)), palette.ink, label);
  }
  window.moving = anim_take();
  window.spinning = anim_take_slow();
  ImGui::GetIO().ConfigFlags = (panel_.settings.empty() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
                                   ? ImGui::GetIO().ConfigFlags & ~ImGuiConfigFlags_NavEnableKeyboard
                                   : ImGui::GetIO().ConfigFlags | ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::Render();
  shift_draw_data(ImGui::GetDrawData(), 0.0f, rise);
  if (!for_capture) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    frame_stats_.built(now_us() - build_started_us);
  }

  const float clear[4] = {to_vec(palette.canvas).x, to_vec(palette.canvas).y, to_vec(palette.canvas).z, 1.0f};
  // Same picture as the one on screen: nothing to draw or present (a pointer over a part of the panel that does not react).
  // The first frame after a show, a resize or a capture always goes out.
  int64_t digest_us = 0;
  if (!for_capture) {
    const int64_t digest_t0 = now_us();
    const uint64_t digest = draw_digest(ImGui::GetDrawData(), clear);
    digest_us = now_us() - digest_t0;
    // Not during a drag or resize: the frame per move is what keeps the compositor fed (see WM_MOVING), and a resize's new buffers are blank.
    // A moving value is always presented, the pointer's too: the display's pacing (a presented frame) is what holds it at the
    // refresh rate, and a skipped frame would let the loop spin on a slow fade or hover.
    const bool same = frame_is_repeat(window.digest_valid, digest, window.digest, just_opened, size_move_started_, window.moving);
    window.digest = digest;
    window.digest_valid = true;
    if (same) {
      wheel_this_frame_ = false;
      window.dirty = false;
      window.last_present_us = now_us();  // the frame was built: the cap and the animations count from now
      std::lock_guard<std::mutex> lock(stats_mutex_);
      frame_stats_.skipped(window.last_present_us);
      drawing_ = nullptr;
      return;
    }
  } else {
    window.digest_valid = false;
  }
  const int64_t draw_t0 = now_us();
  context_->OMSetRenderTargets(1, &window.target, nullptr);
  context_->ClearRenderTargetView(window.target, clear);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  const int64_t draw_us = now_us() - draw_t0;
  wheel_this_frame_ = false;  // the wheel lifted NoScrollWithMouse for this frame only

  if (for_capture) {
    std::string error;
    bool ok = false;
    ID3D11Texture2D* back = nullptr;
    window.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    if (back) {
      D3D11_TEXTURE2D_DESC description{};
      back->GetDesc(&description);
      description.Usage = D3D11_USAGE_STAGING;
      description.BindFlags = 0;
      description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      description.MiscFlags = 0;
      ID3D11Texture2D* staging = nullptr;
      if (SUCCEEDED(device_->CreateTexture2D(&description, nullptr, &staging))) {
        context_->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
          std::vector<uint8_t> pixels(static_cast<size_t>(description.Width) * description.Height * 4);
          for (UINT row = 0; row < description.Height; ++row) {
            std::memcpy(pixels.data() + static_cast<size_t>(row) * description.Width * 4,
                        static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
                        static_cast<size_t>(description.Width) * 4);
          }
          context_->Unmap(staging, 0);
          for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xFF;  // opaque, whatever DWM left
          std::string path;
          {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            path = capture_.path;
          }
          ok = encode_image_wic(pixels.data(), static_cast<int>(description.Width), static_cast<int>(description.Height), path,
                                false, error);
        } else {
          error = "the frame could not be read back";
        }
        release(staging);
      } else {
        error = "no staging texture";
      }
      release(back);
    } else {
      error = "the window has no frame";
    }
    std::lock_guard<std::mutex> lock(requests_mutex_);
    capture_.ok = ok;
    capture_.error = error;
    capture_.done = true;
    capture_.pending = false;
    capture_done_.notify_all();
  }

  const uint32_t present_started = GetTickCount();
  // Waiting for the vertical blank lines the picture up with the display; the frame was drawn into the slot the
  // loop waited for, so this does not block. A capture, and a resize (which the system paces itself), do not wait.
  const int64_t present_t0 = now_us();
  const UINT sync = window.pace && vsync && !for_capture ? 1 : 0;
  // A sync interval of 1 waits for the vertical blank. A sleeping display, a covered window (DWM stops taking frames
  // while another window is in front) or a stalled driver can leave that wait without an answer, which hangs the window
  // thread; DO_NOT_WAIT turns it into a skipped frame instead, for an unpaced present too.
  const HRESULT presented = window.swap->Present(sync, DXGI_PRESENT_DO_NOT_WAIT);
  const bool present_skipped = presented == DXGI_ERROR_WAS_STILL_DRAWING || presented == DXGI_STATUS_OCCLUDED;
  // A skipped frame was not seen: offer the same picture again next time instead of counting it as shown.
  if (present_skipped) window.digest_valid = false;
  const int64_t present_us = now_us() - present_t0;
  if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) {
    // A driver reset (a TDR) took the device and every resource with it: the next pass rebuilds the device, the swap
    // chains and the atlases instead of drawing into a device that no longer answers.
    device_lost_ = true;
    BX_LOG_WARN("the interface's present failed (0x%08lx): its device will be rebuilt", static_cast<unsigned long>(presented));
  }
  if (!for_capture) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    frame_stats_.drawn(digest_us, draw_us, present_us);
  }
  time_part(frame_report_, "presenting", present_started);
  // A Present that holds the window thread this long names itself in the log, with what the window was doing (the thread also
  // pumps the tray and the hotkeys).
  if (GetTickCount() - present_started > 200) {
    BX_LOG_WARN("slow present: %u ms (%s window, paced %d, signal taken %d, %lld us since the last one, foreground %d)",
                GetTickCount() - present_started, window.tray ? "tray" : "panel", window.pace && vsync && !for_capture ? 1 : 0,
                window.pace_ready ? 1 : 0, static_cast<long long>(now_us() - window.last_present_us), GetForegroundWindow() == window.hwnd ? 1 : 0);
  }
  window.pace_ready = false;
  window.last_present_us = now_us();
  if (!for_capture && !present_skipped) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    frame_stats_.present(window.last_present_us);
  }
  window.dirty = false;
  drawing_ = nullptr;
}

// ---------------------------------------------------------------- messages

namespace {
// The kind of a window message that counts as input for `ui.frame_stats`: the pointer's moves (and anything else that is not a
// click, a wheel notch or a key), so a focus or leave message is counted as the pointer it comes with.
InputKind message_kind(UINT message) {
  switch (message) {
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      return InputKind::wheel;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
      return InputKind::key;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
      return InputKind::click;
    default:
      return InputKind::pointer;
  }
}
}  // namespace

LRESULT CALLBACK UiHost::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetPropW(hwnd, L"BuffrXHost", create->lpCreateParams);
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto* host = static_cast<UiHost*>(GetPropW(hwnd, L"BuffrXHost"));
  auto* window = reinterpret_cast<UiWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCDESTROY) RemovePropW(hwnd, L"BuffrXHost");
  if (!host || !window) {
    // Before the window is fully set up: the whole window is client area from the start.
    if (message == WM_NCCALCSIZE && wparam) return 0;
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  return host->handle(*window, message, wparam, lparam);
}

LRESULT UiHost::handle(UiWindow& window, UINT message, WPARAM wparam, LPARAM lparam) {
  HWND hwnd = window.hwnd;
  const uint32_t now = GetTickCount();
  switch (message) {
    case kEditorLeaving:
      if (editor_process_ && editor_mode_) {
        editor_returning_ = true;
        return_from_editor();
      }
      return 0;
    case WM_NCCALCSIZE:
      // The whole window is client area. The panel is never maximised: its own size ceiling holds it
      // below the work area, so there is no special rectangle to apply here.
      if (wparam) return 0;
      break;
    case WM_NCACTIVATE:
      return TRUE;  // no frame to repaint
    case WM_NCHITTEST: {
      POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
      ScreenToClient(hwnd, &point);
      RECT client{};
      GetClientRect(hwnd, &client);
      if (!IsZoomed(hwnd)) {
        const int border = static_cast<int>(6 * window.scale);
        const bool left = point.x < border, right = point.x >= client.right - border;
        const bool top = point.y < border, bottom = point.y >= client.bottom - border;
      if (top && left) return HTTOPLEFT;
      if (top && right) return HTTOPRIGHT;
      if (bottom && left) return HTBOTTOMLEFT;
      if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;
      }
      const ImVec2 at(static_cast<float>(point.x), static_cast<float>(point.y));
      for (const auto& area : window.no_drag) {
        if (in_rect(at, area.first, area.second)) return HTCLIENT;
      }
      if (!diag_flag("BUFFRX_UI_HTCLIENT")) {
        for (const auto& area : window.drag) {
          if (in_rect(at, area.first, area.second)) return HTCAPTION;
        }
      }
      return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      if (window.tray) {
        // About 30% either side of the default, never past the work area: the quick panel cannot be made full screen,
        // by a drag, a double click on an edge or a snap (WM_WINDOWPOSCHANGING holds the same limits for those).
        const RECT work = work_area_for(hwnd);
        const QuickLimits limits = quick_limits(window.scale, work.right - work.left, work.bottom - work.top);
        info->ptMinTrackSize = POINT{limits.min_width, limits.min_height};
        info->ptMaxTrackSize = POINT{limits.max_width, limits.max_height};
        info->ptMaxSize = info->ptMaxTrackSize;
        return 0;
      }
      const float scale = window.scale;
      // The panel resizes within its own limits; the editor keeps its vertical resize range.
      // The work area wins when it is smaller: there the panel is the work area and its own panes scroll.
      const RECT work = work_area_for(hwnd);
      const PanelLimits limits = panel_limits(scale, work.right - work.left, work.bottom - work.top, editor_mode_);
      info->ptMinTrackSize = POINT{limits.min_width, limits.min_height};
      info->ptMaxTrackSize = POINT{limits.max_width, limits.max_height};
      // No maximise: a maximise request lands on the ceiling, not on the screen.
      info->ptMaxSize = info->ptMaxTrackSize;
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
        info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
      }
      return 0;
    }
    case WM_WINDOWPOSCHANGING:
      // Whatever asks for a size - a drag, a snap, a programmatic move - gets one inside the limits.
      {
        auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
        if (!(position->flags & SWP_NOSIZE)) {
          const RECT work = work_area_for(hwnd);
          if (window.tray) {
            const QuickSize fitted =
                quick_clamp(position->cx, position->cy, quick_limits(window.scale, work.right - work.left, work.bottom - work.top));
            position->cx = static_cast<int>(fitted.width);
            position->cy = static_cast<int>(fitted.height);
          } else {
            const PanelSize fitted =
                panel_clamp(position->cx, position->cy, panel_limits(window.scale, work.right - work.left, work.bottom - work.top, editor_mode_));
            position->cx = static_cast<int>(fitted.width);
            position->cy = static_cast<int>(fitted.height);
          }
        }
      }
      break;
    case WM_NCLBUTTONDBLCLK:
      // A double click on an edge or the header would maximize the panel. It may only be resized by dragging.
      return 0;
    case WM_SIZE:
      if (editor_mode_ && !window.tray) size_editor();
      if (window.surface && wparam != SIZE_MINIMIZED) {
        RECT whole{};
        GetClientRect(window.hwnd, &whole);
        SetWindowPos(window.surface, nullptr, 0, 0, whole.right, whole.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
      }
      if (!window.tray) {
        // A new size measures the panes afresh: the fill is measured once from the natural layout, so
        // clear both and let the next frame take it.
        panel_.deck_pad = 0;
        panel_.deck_pad_seen = -1;
        panel_.deck_fill_ready = false;
      }
      if (window.swap && wparam != SIZE_MINIMIZED) {
        // A drag on an edge sends a size per mouse step, far more than frames can follow, and each one resizes the buffers
        // and draws the whole panel: the backlog is what made it slow. Between frames the old picture is stretched to the
        // new size by the system; the last size is applied when the drag ends.
        if (size_move_started_ && window.target && now_us() - window.last_present_us < window.resize_interval_us) {
          window.resize_pending = true;
          return 0;
        }
        window.resize_pending = false;
        release(window.target);
        // A window swap chain follows its window when given 0 x 0; a composition one has no window to read, so it is told.
        RECT client{};
        if (window.dcomp_visual) GetClientRect(window.hwnd, &client);
        const HRESULT resized = window.swap->ResizeBuffers(0, static_cast<UINT>(std::max<LONG>(0, client.right - client.left)),
                                                           static_cast<UINT>(std::max<LONG>(0, client.bottom - client.top)), DXGI_FORMAT_UNKNOWN, window.swap_flags);
        bind_pace(window);  // the frame-latency setting and its handle are re-made with the buffers
        window.digest_valid = false;  // the new buffers are blank: the next frame is presented
        if (FAILED(resized)) {
          // There is no target to draw into, and a lost device will not take new buffers: rebuild the device rather
          // than wait for a resize the user may never make.
          device_lost_ = true;
          return 0;
        }
        ID3D11Texture2D* back = nullptr;
        window.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
        if (back) device_->CreateRenderTargetView(back, nullptr, &window.target);
        release(back);
        window.dirty = true;
        // Resizing runs a modal loop that starves the main loop: draw from here so the panel keeps up. Not while the fonts
        // are due: a DPI change is followed by this message, and rebuilding the atlas inside it froze a cross-DPI drag.
        if (window.visible && window.target && !drawing_ && !window.fonts_due) render(window, false, false);
      }
      return 0;
    case WM_MOVING:
    case WM_MOVE:
      if (size_move_started_) {
        // What the drag looked like from in here: moves per half second and the longest wait between two. Even from the
        // start means this thread keeps up and the lag is the compositor's.
        const int64_t at = now_us();
        const size_t bucket = static_cast<size_t>((at - move_t0_us_) / 500000);
        if (bucket < move_counts_.size()) move_counts_[bucket]++;
        if (last_move_us_ && bucket < move_worst_us_.size()) move_worst_us_[bucket] = std::max<int64_t>(move_worst_us_[bucket], at - last_move_us_);
        last_move_us_ = at;
      }
      // A drag runs Windows' own loop and nothing presents in it (a resize draws from WM_SIZE; a move sends none), which leaves
      // the display or the compositor waiting on a window that went quiet. A frame per move, no more than one in ~6 ms and
      // without waiting for the display, keeps it fed.
      if (size_move_started_ && window.visible && window.target && !drawing_ && !window.fonts_due &&
          now_us() - window.last_present_us > 6000) {
        render(window, false, false);
      }
      break;
    case WM_ENTERSIZEMOVE:
      size_move_started_ = now;
      {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        const bool known = GetMonitorInfoW(monitor, &info) &&
                           EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
        window.resize_interval_us = resize_frame_interval_us(known ? static_cast<int>(mode.dmDisplayFrequency) : 0);
      }
      beat_worst_ms_ = 0;
      move_t0_us_ = now_us();
      last_move_us_ = 0;
      move_counts_.fill(0);
      move_worst_us_.fill(0);
      // Windows' move/size loop has the thread now; a timer is the one thing it still dispatches.
      SetTimer(hwnd, kModalHeartbeatTimer, 100, nullptr);
      break;
    case WM_TIMER:
      if (wparam == kModalHeartbeatTimer) {
        if (modal_heartbeat_) {
          const uint32_t beat_started = GetTickCount();
          modal_heartbeat_();
          const uint32_t beat_ms = GetTickCount() - beat_started;
          if (beat_ms > beat_worst_ms_) beat_worst_ms_ = beat_ms;
        }
        return 0;
      }
      break;
    case WM_EXITSIZEMOVE:
      if (window.resize_pending) {
        const uint32_t kept = size_move_started_;
        size_move_started_ = 0;
        handle(window, WM_SIZE, SIZE_RESTORED, 0);
        size_move_started_ = kept;
      }
      KillTimer(hwnd, kModalHeartbeatTimer);
      if (!window.tray) {
        save_layout();
      } else {
        // The quick panel keeps the place the user dragged it to, and the size they gave it.
        RECT moved{};
        GetWindowRect(hwnd, &moved);
        quick_pos_ = POINT{moved.left, moved.top};
        quick_moved_ = true;
        quick_w_ = quick_design(moved.right - moved.left, window.scale);
        quick_h_ = quick_design(moved.bottom - moved.top, window.scale);
        save_layout();
      }
      // Windows runs its own loop while a window is dragged or resized; the engine's loop waits for it. A drag is the person's
      // hand, not a fault: the numbers go to the console at info, and the stall check takes the time off the pass.
      if (size_move_started_) modal_ms_ += now - size_move_started_;
      if (size_move_started_ && now - size_move_started_ > 250) {
        BX_LOG_INFO("the panel was dragged or resized for %u ms (the longest heartbeat in it took %u ms)", now - size_move_started_,
                    beat_worst_ms_);
        std::string moves;
        for (size_t i = 0; i < move_counts_.size() && i * 500 < now - size_move_started_; ++i) {
          moves += " " + std::to_string(move_counts_[i]) + "/" + std::to_string(move_worst_us_[i] / 1000) + "ms";
        }
        BX_LOG_INFO("moves per half second / longest wait between two:%s", moves.c_str());
      }
      size_move_started_ = 0;
      break;
    case WM_DPICHANGED: {
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      // The interface is drawn a fifth larger on top of the monitor's DPI, and the window's size follows
      // that: the scale is set here so the minimum size, the resize border and the hit test are right
      // before the next frame; the fonts and the style are rebuilt by the frame itself.
      const UINT dpi = LOWORD(wparam);  // the new DPI, which is what the message is about
      const RECT proposed_work = work_area_of(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST));
      const float requested = (dpi ? dpi : window_dpi(hwnd)) / 96.0f * kUiScale * ui_scale_percent_ / 100.0f;
      window.scale = window.tray ? requested : panel_scale_to_fit(requested, proposed_work.right - proposed_work.left,
                                                                  proposed_work.bottom - proposed_work.top);
      window.fonts_stale = true;
      // The atlas is rebuilt by the frame that follows, not by the WM_SIZE this message sends: doing it here held the
      // window thread for 300-640 ms inside the DPI message, which froze a cross-DPI drag.
      window.fonts_due = true;
      window.dirty = true;
      window.settle_us = now_us() + kSettleUs;
      if (!IsZoomed(hwnd)) {
        // Windows' suggested rectangle keeps the window's place and size in the new scale. It can still be
        // larger than the monitor it landed on (a 5K display to a 1080p one), so it is fitted to that
        // monitor's work area - which is also what keeps the panel from opening wider than the screen.
        const RECT proposed = *suggested;
        RECT placed = window.tray ? fit_to_work(proposed, proposed_work)
                                  : panel_place(proposed_work, window.scale, proposed, true, editor_mode_);
        if (window.tray) {
          // The quick panel keeps its design size on the new monitor, inside that monitor's limits.
          const QuickSize size = quick_size_for(quick_w_, quick_h_, window.scale, proposed_work.right - proposed_work.left,
                                                proposed_work.bottom - proposed_work.top);
          placed = fit_to_work(RECT{proposed.left, proposed.top, proposed.left + static_cast<LONG>(size.width),
                                    proposed.top + static_cast<LONG>(size.height)},
                               proposed_work);
        }
        SetWindowPos(hwnd, nullptr, placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (window.tray) {
          if (quick_moved_) {
            quick_pos_ = POINT{placed.left, placed.top};
            save_layout();
          }
        } else {
          panel_rect_ = placed;
        }
      }
      return 0;
    }
    case WM_DISPLAYCHANGE: {
      // A monitor was unplugged or a resolution changed while a window is open. A window on the monitor
      // that went away is brought back to one that exists; one now larger than its work area is fitted to
      // it. A maximised window is left alone: Windows re-places that one itself, and the work area it
      // covers is read again by WM_NCCALCSIZE.
      window.fonts_stale = true;
      window.dirty = true;
      if (!IsZoomed(hwnd)) {
        RECT current{};
        GetWindowRect(hwnd, &current);
        const bool lost = MonitorFromRect(&current, MONITOR_DEFAULTTONULL) == nullptr;
        const RECT work = lost ? work_area_for(nullptr) : work_area_for(hwnd);
        RECT fitted = current;
        if (window.tray) {
          // A resolution change moves the limits: the size the user gave it is brought inside the new ones.
          const QuickSize size = quick_clamp(current.right - current.left, current.bottom - current.top,
                                             quick_limits(window.scale, work.right - work.left, work.bottom - work.top));
          fitted.right = fitted.left + static_cast<LONG>(size.width);
          fitted.bottom = fitted.top + static_cast<LONG>(size.height);
        } else {
          // The panel's own ceiling follows the new work area and the active view's limits.
          const PanelSize size = panel_clamp(current.right - current.left, current.bottom - current.top,
                                             panel_limits(window.scale, work.right - work.left, work.bottom - work.top, editor_mode_));
          fitted.right = fitted.left + static_cast<LONG>(size.width);
          fitted.bottom = fitted.top + static_cast<LONG>(size.height);
        }
        const RECT placed = fit_to_work(fitted, work);
        if (!EqualRect(&placed, &current)) {
          SetWindowPos(hwnd, nullptr, placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
          if (window.tray) {
            quick_pos_ = POINT{placed.left, placed.top};
            quick_moved_ = !lost;
          } else {
            panel_rect_ = placed;
          }
        }
      }
      break;  // the ImGui backend and DefWindowProc still want it
    }
    case WM_CLOSE:
      if (!window.tray) {
        cancel_recorder();
        save_layout();
      }
      // Fades out over the exit token, then hides and is destroyed after a frame (finish_hides): the window may be the one drawing.
      begin_hide(window, true);
      return 0;
    case WM_ACTIVATE:
      // The quick panel stays where it is when another app takes the focus: it is a floating widget, closed by
      // its own button, Esc or the tray icon. The panel is never on top; the quick panel always is.
      window.dirty = true;
      break;
    case WM_SYSCOMMAND:
      if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;  // no menu: Alt must never steal focus
      // The panel is resized only by dragging an edge; a maximise from the system menu, Win+Up or the
      // taskbar lands on its ceiling (WM_GETMINMAXINFO) instead of the screen.
      if ((wparam & 0xFFF0) == SC_MAXIMIZE) return 0;
      if (window.tray && (wparam & 0xFFF0) == SC_MINIMIZE) return 0;  // a widget, not a window
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (capturing_keys_) {
        keys_.push_back(Key{GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0, GetKeyState(VK_SHIFT) < 0,
                            GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0, static_cast<unsigned>(wparam), true});
        window.dirty = true;
        window.settle_us = now_us() + kSettleUs;
        return 0;
      }
      break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (capturing_keys_) {
        // The release matters too: the recorder draws Ctrl or Alt the moment it goes down and drops it
        // again if the combination is abandoned.
        keys_.push_back(Key{GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0, GetKeyState(VK_SHIFT) < 0,
                            GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0, static_cast<unsigned>(wparam), false});
        window.dirty = true;
        window.settle_us = now_us() + kSettleUs;
        return 0;
      }
      break;
    case WM_SYSCHAR:
    case WM_CHAR:
      if (capturing_keys_) return 0;
      break;
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  if (window.imgui) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(window.imgui);
    const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);
    ImGui::SetCurrentContext(previous);
    if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
        message == WM_MOUSELEAVE || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
      // A move on its own (no button held, no wheel, no key) wakes only the window the user operates (pointer_move_wakes); a click,
      // a drag, the wheel or a key always wakes it.
      window.pointer_only = message == WM_MOUSEMOVE && (wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0;
      if (pointer_move_wakes(window.pointer_only, window.focused)) {
        window.dirty = true;
        window.settle_us = now_us() + kSettleUs;
        note_input(message_kind(message));
      }
    }
    if (handled) return handled;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace bx::ui
