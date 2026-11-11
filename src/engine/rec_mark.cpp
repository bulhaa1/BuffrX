#include "rec_mark.h"

#include "toast.h"

#include <algorithm>
#include <cmath>

namespace bx {
namespace {

// The recording red, the one the panel's own record dot uses.
constexpr int kRed = 0xE0, kGreen = 0x3A, kBlue = 0x34;

constexpr int kDiameter = 22;
constexpr int kMargin = 16;
constexpr uint32_t kPulseTimer = 1;
// The breathe: the dot is never fully dark and never fully bright, so it reads as alive without
// asking for attention.
constexpr uint32_t kPulsePeriodMs = 1600;

UINT monitor_dpi(HMONITOR monitor) {
  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static const auto get_dpi = reinterpret_cast<GetDpiForMonitorFn>(
      reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")));
  UINT dpi_x = 96;
  UINT dpi_y = 96;
  if (get_dpi) get_dpi(monitor, 0, &dpi_x, &dpi_y);
  return dpi_x;
}

}  // namespace

// The dot's alpha at a distance from the middle: solid in the core, gone at the rim.
double dot_falloff(double distance, double core, double radius) {
  if (distance <= core) return 1.0;
  const double edge = (distance - core) / (radius - core);
  if (edge >= 1.0) return 0.0;
  const double soft = 1.0 - edge;
  return soft * soft;
}

bool recording_dot_visible(bool manual_active) { return manual_active; }

RecMark::~RecMark() { destroy(); }

bool RecMark::create() {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &RecMark::window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"BuffrX.RecMark";
  RegisterClassExW(&window_class);
  window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            L"BuffrX.RecMark", L"BuffrX recording", WS_POPUP, 0, 0, kDiameter, kDiameter, nullptr,
                            nullptr, window_class.hInstance, this);
  if (!window_) return false;
  SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  // The mark is on the screen, not in the recording: Windows leaves it out of every capture, the
  // display one included. BUFFRX_MARK_VISIBLE=1 leaves it in, so a test can see it.
  if (!getenv("BUFFRX_MARK_VISIBLE")) SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE);
  return true;
}

void RecMark::destroy() {
  if (!window_) return;
  KillTimer(window_, kPulseTimer);
  DestroyWindow(window_);
  window_ = nullptr;
}

void RecMark::set_recording(bool recording) {
  if (!window_ || recording == recording_) return;
  recording_ = recording;
  if (!recording) {
    // The pulse timer runs only while a recording does: it used to tick 25 times a second for good, and each tick woke the
    // engine's window thread for nothing.
    KillTimer(window_, kPulseTimer);
    ShowWindow(window_, SW_HIDE);
    visible_ = false;
    return;
  }
  SetTimer(window_, kPulseTimer, 40, nullptr);
  started_at_ = GetTickCount();
  // Shown at once, over a game too (decided with the user, 2026-11-05): a recording must say so on screen wherever the
  // player is. A topmost window can cost a fullscreen game its independent flip; the person asked for the mark, so the
  // mark wins. It is re-placed every second to stay on top after the game takes focus back.
  place();
  ShowWindow(window_, SW_SHOWNOACTIVATE);
  visible_ = true;
  pulse_at_ = GetTickCount();
}

void RecMark::tick() {
  if (!recording_) return;
  const uint32_t now = GetTickCount();
  if (now - fullscreen_checked_ms_ >= 1000) {
    fullscreen_checked_ms_ = now;
    // The mark stays on screen everywhere. Placed again each second so it goes back on top of a game that re-took focus,
    // and so a monitor or mode change moves it with the game.
    place();
    if (!visible_) {
      ShowWindow(window_, SW_SHOWNOACTIVATE);
      visible_ = true;
      pulse_at_ = now;
    }
  }
  paint();
}

void RecMark::place() {
  // The monitor being played on: where the foreground window is, else the primary one.
  HWND foreground = GetForegroundWindow();
  HMONITOR monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY)
                                : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return;
  const float scale = static_cast<float>(monitor_dpi(monitor)) / 96.0f;
  const int size = static_cast<int>(std::lround(kDiameter * scale));
  const int margin = static_cast<int>(std::lround(kMargin * scale));
  // The bare HUD: the dot, then "REC 0:42" beside it in the interface font. The width fits the longest clock a recording
  // reaches (hours), so the window never resizes while it runs.
  const int label_w = static_cast<int>(std::lround(124 * scale));
  SetWindowPos(window_, HWND_TOPMOST, info.rcWork.left + margin, info.rcWork.top + margin, size + label_w, size,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void RecMark::paint() {
  if (!window_ || !visible_) return;
  RECT rect{};
  GetClientRect(window_, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return;

  // The breathe: the dot is a little brighter and a little dimmer, over a second and a half.
  const uint32_t now = GetTickCount();
  const double phase = static_cast<double>((now - pulse_at_) % kPulsePeriodMs) / kPulsePeriodMs;
  const double wave = 0.5 - 0.5 * std::cos(phase * 6.283185307179586);
  const BYTE alpha = static_cast<BYTE>(190 + wave * 65);

  // A per-pixel alpha layered window: only the dot is drawn, the rest of the square is see-through.
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!memory || !bitmap) {
    ReleaseDC(nullptr, screen);
    if (memory) DeleteDC(memory);
    if (bitmap) DeleteObject(bitmap);
    return;
  }
  HGDIOBJ previous = SelectObject(memory, bitmap);

  auto* pixels = static_cast<uint8_t*>(bits);
  const double radius = std::min(width, height) / 2.0;
  const double core = radius * 0.42;
  const double centre_x = std::min(width, height) / 2.0;
  const double centre_y = height / 2.0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double dx = x + 0.5 - centre_x;
      const double dy = y + 0.5 - centre_y;
      const double distance = std::sqrt(dx * dx + dy * dy);
      const double cover = dot_falloff(distance, core, radius);
      uint8_t* pixel = pixels + (static_cast<size_t>(y) * width + x) * 4;
      if (cover <= 0.0) {
        pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
        continue;
      }
      const BYTE a = static_cast<BYTE>(cover * alpha);
      // Premultiplied: the colour is already scaled by the alpha, the way a layered window wants it.
      pixel[0] = static_cast<uint8_t>(kBlue * a / 255);
      pixel[1] = static_cast<uint8_t>(kGreen * a / 255);
      pixel[2] = static_cast<uint8_t>(kRed * a / 255);
      pixel[3] = a;
    }
  }

  // The clock, drawn into a scratch surface with GDI and laid over the dot: the interface font, white, breathing with the
  // dot. GDI writes no alpha, so its luminance becomes the pixel alpha (premultiplied) here.
  {
    const int label_x = std::min(width, height);
    const int label_w = width - label_x;
    if (label_w > 0) {
      BITMAPINFO label_info{};
      label_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      label_info.bmiHeader.biWidth = label_w;
      label_info.bmiHeader.biHeight = -height;
      label_info.bmiHeader.biPlanes = 1;
      label_info.bmiHeader.biBitCount = 32;
      label_info.bmiHeader.biCompression = BI_RGB;
      void* label_bits = nullptr;
      HBITMAP label_bitmap = CreateDIBSection(memory, &label_info, DIB_RGB_COLORS, &label_bits, nullptr, 0);
      HDC label_dc = CreateCompatibleDC(memory);
      if (label_bitmap && label_dc && label_bits) {
        HGDIOBJ label_previous = SelectObject(label_dc, label_bitmap);
        PatBlt(label_dc, 0, 0, label_w, height, BLACKNESS);
        const int dpi = GetDeviceCaps(memory, LOGPIXELSY);
        const int font_px = -MulDiv(13, dpi, 72);
        HFONT font = CreateFontW(font_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ font_previous = SelectObject(label_dc, font);
        SetBkMode(label_dc, TRANSPARENT);
        SetTextColor(label_dc, RGB(255, 255, 255));
        const uint32_t seconds = (GetTickCount() - started_at_) / 1000;
        wchar_t clock[32]{};
        swprintf_s(clock, L"REC %u:%02u", seconds / 60, seconds % 60);
        RECT text_rect{0, 0, label_w, height};
        DrawTextW(label_dc, clock, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        const uint8_t* label = static_cast<const uint8_t*>(label_bits);
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < label_w; ++x) {
            const uint8_t lum = label[(static_cast<size_t>(y) * label_w + x) * 4];
            if (lum <= 8) continue;
            uint8_t* pixel = pixels + (static_cast<size_t>(y) * width + label_x + x) * 4;
            const BYTE a = static_cast<BYTE>(static_cast<int>(lum) * alpha / 255);
            pixel[0] = pixel[1] = pixel[2] = a;
            pixel[3] = a;
          }
        }
        SelectObject(label_dc, font_previous);
        DeleteObject(font);
        SelectObject(label_dc, label_previous);
      }
      if (label_dc) DeleteDC(label_dc);
      if (label_bitmap) DeleteObject(label_bitmap);
    }
  }

  POINT position{};
  GetWindowRect(window_, &rect);
  position.x = rect.left;
  position.y = rect.top;
  SIZE size{width, height};
  POINT source{0, 0};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(window_, screen, &position, &size, memory, &source, 0, &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

LRESULT CALLBACK RecMark::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  RecMark* self = nullptr;
  if (message == WM_NCCREATE) {
    self = static_cast<RecMark*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<RecMark*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  }
  if (self && self->window_ == window) return self->handle_message(message, wparam, lparam);
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT RecMark::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;  // the mark must never pull focus away from the game
    case WM_TIMER:
      if (wparam == kPulseTimer) tick();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

}  // namespace bx
