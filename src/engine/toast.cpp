#include "toast.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../shared/files.h"
#include "../shared/log.h"
#include "capture_enum.h"
#include "themes.h"

namespace bx {
namespace {

constexpr UINT kShowMessage = WM_APP + 41;
constexpr UINT_PTR kFadeTimer = 1;
constexpr UINT kFadeStepMs = 8;  // a timer is a step at best: 8 ms with the finer clock held, so a fade runs near 100 Hz
constexpr uint32_t kFadeInMs = 160;
constexpr uint32_t kHoldMs = 3400;       // a warning: long enough to read twice
constexpr uint32_t kHoldMsQuiet = 1900;  // a done or info card: read once, out of the way
constexpr uint32_t kFadeOutMs = 260;
// The card in device-independent pixels; scaled by the monitor's DPI when drawn.
constexpr float kWidth = 340.0f;
constexpr float kHeight = 96.0f;  // tag, title, two lines of detail, 16 px of air top and bottom
constexpr float kMargin = 16.0f;

D2D1_COLOR_F color(uint32_t rgb, float alpha = 1.0f) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xff) / 255.0f, static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                      static_cast<float>(rgb & 0xff) / 255.0f, alpha);
}

template <typename T>
void release(T*& pointer) {
  if (pointer) {
    pointer->Release();
    pointer = nullptr;
  }
}

// Created once per process on first use and kept: both factories are cheap to hold and costly to make.
ID2D1Factory* d2d_factory() {
  static ID2D1Factory* factory = [] {
    ID2D1Factory* made = nullptr;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                 reinterpret_cast<void**>(&made)))) {
      return static_cast<ID2D1Factory*>(nullptr);
    }
    return made;
  }();
  return factory;
}

IDWriteFactory* dwrite_factory() {
  static IDWriteFactory* factory = [] {
    IUnknown* made = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &made))) {
      return static_cast<IDWriteFactory*>(nullptr);
    }
    return static_cast<IDWriteFactory*>(made);
  }();
  return factory;
}

// JetBrains Mono from the runtime's own font files, as a private collection: nothing is installed in
// Windows. Built once; null (the card falls back to Consolas) when the files or DirectWrite 3 are missing.
IDWriteFontCollection* mono_collection(const std::string& font_dir) {
  static IDWriteFontCollection* collection = [&]() -> IDWriteFontCollection* {
    if (font_dir.empty()) return nullptr;
    IDWriteFactory5* factory = nullptr;
    if (!dwrite_factory() || FAILED(dwrite_factory()->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(&factory)))) {
      return nullptr;
    }
    IDWriteFontSetBuilder1* builder = nullptr;
    IDWriteFontCollection1* made = nullptr;
    if (SUCCEEDED(factory->CreateFontSetBuilder(&builder))) {
      for (const char* face : {"JetBrainsMonoNL-Medium.ttf", "JetBrainsMonoNL-Bold.ttf", "JetBrainsMonoNL-ExtraBold.ttf"}) {
        const std::string path = font_dir + "\\" + face;
        const std::wstring file_path = wide_path(path);
        IDWriteFontFile* file = nullptr;
        if (SUCCEEDED(factory->CreateFontFileReference(file_path.c_str(), nullptr, &file))) {
          builder->AddFontFile(file);
          file->Release();
        }
      }
      IDWriteFontSet* set = nullptr;
      if (SUCCEEDED(builder->CreateFontSet(&set))) {
        if (FAILED(factory->CreateFontCollectionFromFontSet(set, &made))) made = nullptr;
        set->Release();
      }
      builder->Release();
    }
    factory->Release();
    return made;
  }();
  return collection;
}

void put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

}  // namespace

// A fullscreen game, borderless included: a topmost window over one can take it out of independent
// flip, and the game then stutters in its own frames and in every clip of them. The old rule only
// looked for true exclusive fullscreen, so borderless games - most of them - got the mark and the
// toast, and the recording was choppy from the mark and the five seconds after a save from the card.
// The foreground check names borderless games too; the notification state still catches exclusive
// fullscreen when the foreground window cannot be read.
bool fullscreen_app_running() {
  ForegroundWindow foreground;
  if (foreground_window(foreground) && foreground.fullscreen) return true;
  QUERY_USER_NOTIFICATION_STATE state{};
  return SUCCEEDED(SHQueryUserNotificationState(&state)) &&
         (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY);
}

struct Toast::Card {
  ToastKind kind = ToastKind::success;
  std::string title;
  std::string detail;
  ToastChime chime = ToastChime::none;
  bool sound_enabled = false;
  bool show_over_fullscreen = false;
  bool panel_in_front = false;
  ToastColors colors;
  bool compact = false;
  bool capture_active = false;
  bool sound_while_capturing = true;
};

ToastColors toast_colors(const std::string& theme) {
  const ThemeColors& t = theme_colors(theme);
  ToastColors colors;
  colors.accent = t.acc;
  colors.on_accent = t.on;
  colors.surface = t.tile;
  colors.line = t.line;
  colors.ink = t.fg;
  colors.muted = t.muted;
  colors.warn = t.warn;
  return colors;
}

namespace {

struct ChimeNote {
  double frequency;
  double start_s;
  double gain;
};

// Soft sine notes with fast attacks and exponential decays, as a complete WAV.
std::vector<uint8_t> render_chime(const std::vector<ChimeNote>& notes, double length_s, double decay, double attack_s = 0.060) {
  const uint32_t rate = 44100;
  const uint32_t samples = static_cast<uint32_t>(rate * length_s);
  std::vector<uint8_t> out;
  out.reserve(44 + samples * 2);
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  put32(out, 36 + samples * 2);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(out, 16);
  put16(out, 1);         // PCM
  put16(out, 1);         // mono
  put32(out, rate);
  put32(out, rate * 2);  // byte rate
  put16(out, 2);         // block align
  put16(out, 16);        // bits
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  put32(out, samples * 2);
  const double pi = 3.14159265358979323846;
  for (uint32_t i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / rate;
    double value = 0.0;
    for (const ChimeNote& note : notes) {
      const double local = t - note.start_s;
      if (local < 0.0) continue;
      // A 30 ms attack: the note arrives without a click, which is what makes the ring smooth rather than a beep.
      const double attack = std::min(1.0, local / attack_s);
      value += note.gain * std::sin(2.0 * pi * note.frequency * local) * attack * std::exp(-local * decay);
    }
    put16(out, static_cast<uint16_t>(static_cast<int16_t>(std::clamp(value, -1.0, 1.0) * 32000.0)));
  }
  return out;
}

}  // namespace

const std::vector<uint8_t>& toast_chime_wav(ToastChime chime) {
  // Air (chosen with the user, 2026-11-05): a soft high bell with a long tail, one shape per event. Saved lifts a
  // fifth, started is a single note, stopped is the same note an octave down, an alert is two low beats.
  // One ring each, slow and round: a low partial under the note, a long tail, a 60 ms attack. Fast partials and short
  // decays read as a "beep-beep"; this reads as a ring that fades.
  // Save clip rises C6 -> G6; a quick clip falls the same two notes (chosen with the user, 2026-11-05). Two soft strikes,
  // 140 ms apart, the case the person liked.
  static const std::vector<uint8_t> saved = render_chime({{659.3, 0.0, 0.22}, {987.8, 0.09, 0.15}}, 0.75, 4.0, 0.05);
  static const std::vector<uint8_t> quick = render_chime({{987.8, 0.0, 0.22}, {659.3, 0.09, 0.15}}, 0.75, 4.0, 0.05);
  // Screenshot: Q4, a quick soft lift A4 -> E5, 110 ms apart, shorter tail and a 35 ms attack (chosen with the user).
  static const std::vector<uint8_t> shot = render_chime({{440.0, 0.0, 0.15}, {659.3, 0.11, 0.12}}, 0.50, 7.0, 0.035);
  static const std::vector<uint8_t> started = render_chime({{659.3, 0.0, 0.34}}, 0.40, 6.5);
  static const std::vector<uint8_t> stopped = render_chime({{493.9, 0.0, 0.38}}, 0.42, 6.0);
  static const std::vector<uint8_t> alert = render_chime({{392.0, 0.0, 0.30}, {523.3, 0.20, 0.26}}, 0.50, 7.0);
  static const std::vector<uint8_t> silent;
  switch (chime) {
    case ToastChime::saved: return saved;
    case ToastChime::quick: return quick;
    case ToastChime::shot: return shot;
    case ToastChime::started: return started;
    case ToastChime::stopped: return stopped;
    case ToastChime::alert: return alert;
    case ToastChime::none: break;
  }
  return silent;
}
Toast::~Toast() { destroy(); }

bool Toast::create(std::function<void()> on_click, const std::string& font_dir) {
  on_click_ = std::move(on_click);
  font_dir_ = font_dir;
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &Toast::window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
  window_class.lpszClassName = L"BuffrX.Toast";
  RegisterClassExW(&window_class);
  window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"BuffrX.Toast",
                            L"BuffrX notification", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                            window_class.hInstance, this);
  return window_ != nullptr;
}

void Toast::destroy() {
  hide();
  if (window_) {
    DestroyWindow(window_);
    window_ = nullptr;
  }
}

void Toast::show(ToastKind kind, const std::string& title, const std::string& detail, ToastChime chime,
                 bool sound_enabled, const ToastColors& colors, bool show_over_fullscreen, bool panel_in_front, bool compact,
                 bool capture_active, bool sound_while_capturing) {
  Card* card = new Card{kind, title, detail, chime, sound_enabled, show_over_fullscreen, panel_in_front, colors, compact,
                        capture_active, sound_while_capturing};
  if (!PostMessageW(window_, kShowMessage, 0, reinterpret_cast<LPARAM>(card))) delete card;
}

LRESULT CALLBACK Toast::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  Toast* self = nullptr;
  if (message == WM_NCCREATE) {
    self = static_cast<Toast*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<Toast*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  }
  if (self && self->window_ == window) return self->handle_message(message, wparam, lparam);
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT Toast::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case kShowMessage:
      present(reinterpret_cast<Card*>(lparam));
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;  // a click must never pull focus away from the game
    case WM_LBUTTONUP:
      if (on_click_) on_click_();
      if (visible_ && !leaving_) {
        leaving_ = true;
        fade_out_at_ = GetTickCount();
        set_fine_timer(true);
        SetTimer(window_, kFadeTimer, kFadeStepMs, nullptr);  // the hold's long wait is over: fade now
      }
      return 0;
    case WM_TIMER: {
      if (wparam != kFadeTimer || !visible_) return 0;
      const uint32_t now = GetTickCount();
      if (!leaving_ && now - shown_at_ >= kFadeInMs + hold_ms_) {
        leaving_ = true;
        fade_out_at_ = now;
        set_fine_timer(true);
        SetTimer(window_, kFadeTimer, kFadeStepMs, nullptr);
      }
      if (leaving_) {
        const uint32_t elapsed = now - fade_out_at_;
        if (elapsed >= kFadeOutMs) {
          hide();
          return 0;
        }
        set_alpha(static_cast<BYTE>(255 - elapsed * 255 / kFadeOutMs));
      } else {
        const uint32_t elapsed = now - shown_at_;
        if (elapsed >= kFadeInMs) {
          // Fully in: nothing changes until the fade-out, so the timer sleeps through the hold instead of redrawing at 60 Hz.
          set_alpha(255);
          set_fine_timer(false);
          SetTimer(window_, kFadeTimer, std::max<uint32_t>(kFadeStepMs, kFadeInMs + hold_ms_ - elapsed), nullptr);
        } else {
          set_alpha(static_cast<BYTE>(elapsed * 255 / kFadeInMs));
        }
      }
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

void Toast::present(Card* card) {
  if (!card) return;
  // A success or info card is not worth the game's frame pacing unless the person asked for them: over a
  // fullscreen game the chime, if the notice asked for one, is all the feedback - so it plays then even with
  // the sound setting off. A warning still draws: the user has to see it, and it is rare. The sound is the
  // notice's own request and nothing else's - a skipped card used to force one for every notice, so a
  // background notice over a game (a peak lowered, an encoder step) was an unexplained beep.
  const ToastCardDecision decision =
      toast_card_decision(card->kind, fullscreen_app_running(), card->show_over_fullscreen, card->panel_in_front);
  const bool chimes = toast_chime_plays(decision, card->chime, card->sound_enabled, card->capture_active,
                                        card->sound_while_capturing);
  // What was decided, in the log: a notice that "did not show" used to leave nothing to read.
  const char* chime_note = chimes                                  ? "played"
                           : card->capture_active                  ? "silent (capturing)"
                           : toast_plays_sound(card->chime)        ? "silent (sound off)"
                                                                   : "none asked";
  BX_LOG_INFO("notice \"%s\": card=%s chime=%s", card->title.c_str(),
              decision == ToastCardDecision::draw             ? "drawn"
              : decision == ToastCardDecision::skip_fullscreen ? "skipped (fullscreen game)"
                                                              : "suppressed (panel in front)",
              chime_note);
  if (chimes) {
    const std::vector<uint8_t>& wav = toast_chime_wav(card->chime);
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
  }
  if (decision != ToastCardDecision::draw) {
    delete card;
    return;
  }
  // A warning stays; everything else goes as soon as it has been read (decided with the user, 2026-11-05).
  hold_ms_ = card->compact ? 1200 : card->kind == ToastKind::warning ? kHoldMs : kHoldMsQuiet;
  const bool rendered = render(*card);
  delete card;
  if (!rendered) return;
  shown_at_ = GetTickCount();
  leaving_ = false;
  if (!visible_) {
    visible_ = true;
    set_alpha(0);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
  }
  SetWindowPos(window_, HWND_TOPMOST, position_.x, position_.y, size_.cx, size_.cy,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  set_fine_timer(true);
  SetTimer(window_, kFadeTimer, kFadeStepMs, nullptr);
}

void Toast::set_fine_timer(bool on) {
  if (on == fine_timer_) return;
  fine_timer_ = on;
  if (on) timeBeginPeriod(1);
  else timeEndPeriod(1);
}

bool Toast::render(const Card& card) {
  ID2D1Factory* factory = d2d_factory();
  IDWriteFactory* writer = dwrite_factory();
  if (!factory || !writer) return false;

  // The monitor being played on: where the foreground window is, else the primary one.
  HWND foreground = GetForegroundWindow();
  HMONITOR monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY)
                                : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return false;
  UINT dpi_x = 96;
  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static const auto get_dpi = reinterpret_cast<GetDpiForMonitorFn>(
      reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")));
  UINT dpi_y = 96;
  if (get_dpi) get_dpi(monitor, 0, &dpi_x, &dpi_y);
  const float scale = static_cast<float>(dpi_x) / 96.0f;

  const float card_width = card.compact ? 150.0f : kWidth;
  const float card_height = card.compact ? 38.0f : kHeight;
  const int width = static_cast<int>(std::lround(card_width * scale));
  const int height = static_cast<int>(std::lround(card_height * scale));
  const int margin = static_cast<int>(std::lround(kMargin * scale));
  const POINT position{info.rcWork.right - width - margin, info.rcWork.top + margin};

  // A card replacing one on screen: the old bitmap goes, the window keeps showing it until the new
  // one is pushed.
  if (memory_dc_) {
    SelectObject(memory_dc_, previous_bitmap_);
    DeleteObject(bitmap_);
    DeleteDC(memory_dc_);
    memory_dc_ = nullptr;
    bitmap_ = nullptr;
  }
  BITMAPINFO bitmap_info{};
  bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap_info.bmiHeader.biWidth = width;
  bitmap_info.bmiHeader.biHeight = -height;  // top-down
  bitmap_info.bmiHeader.biPlanes = 1;
  bitmap_info.bmiHeader.biBitCount = 32;
  bitmap_info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  memory_dc_ = CreateCompatibleDC(screen);
  bitmap_ = CreateDIBSection(screen, &bitmap_info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!memory_dc_ || !bitmap_) return false;
  previous_bitmap_ = SelectObject(memory_dc_, bitmap_);
  std::memset(bits, 0, static_cast<size_t>(width) * height * 4);

  const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
      96.0f);
  ID2D1DCRenderTarget* target = nullptr;
  if (FAILED(factory->CreateDCRenderTarget(&properties, &target))) return false;
  const RECT bounds{0, 0, width, height};
  bool ok = SUCCEEDED(target->BindDC(memory_dc_, &bounds));
  ID2D1SolidColorBrush* brush = nullptr;
  IDWriteTextFormat* title_format = nullptr;
  IDWriteTextFormat* detail_format = nullptr;
  IDWriteInlineObject* ellipsis = nullptr;
  if (ok) {
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    // The interface's face; Consolas from Windows when the runtime's files could not be read.
    IDWriteFontCollection* mono = mono_collection(font_dir_);
    const wchar_t* family = mono ? L"JetBrains Mono NL" : L"Consolas";
    ok = SUCCEEDED(target->CreateSolidColorBrush(color(card.colors.surface), &brush)) &&
         SUCCEEDED(writer->CreateTextFormat(family, mono, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &title_format)) &&
         SUCCEEDED(writer->CreateTextFormat(family, mono, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &detail_format));
  }
  if (ok) {
    // One line each, cut with an ellipsis rather than wrapped out of the card.
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (SUCCEEDED(writer->CreateEllipsisTrimmingSign(detail_format, &ellipsis))) {
      title_format->SetTrimming(&trimming, ellipsis);
      detail_format->SetTrimming(&trimming, ellipsis);
    }
    title_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    detail_format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    detail_format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 15.5f, 11.0f);

    const uint32_t tone = card.kind == ToastKind::warning ? card.colors.warn : card.colors.accent;
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    // The card as the interface draws a notice: square, a 2 px frame in the notice's tone, and a filled
    // tag that says what kind of notice it is.
    brush->SetColor(color(card.colors.surface, 0.98f));
    target->FillRectangle(D2D1::RectF(0, 0, card_width, card_height), brush);
    brush->SetColor(color(tone));
    target->DrawRectangle(D2D1::RectF(1.0f, 1.0f, card_width - 1.0f, card_height - 1.0f), brush, 2.0f);
    const wchar_t* tag = card.kind == ToastKind::success ? L"DONE" : card.kind == ToastKind::warning ? L"ALERT" : L"INFO";
    const D2D1_RECT_F tag_box = D2D1::RectF(16.0f, card_height / 2.0f - 11.0f, 60.0f, card_height / 2.0f + 11.0f);
    target->FillRectangle(tag_box, brush);
    detail_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    detail_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    brush->SetColor(color(card.kind == ToastKind::warning ? 0x000000 : card.colors.on_accent));
    target->DrawText(tag, static_cast<UINT32>(wcslen(tag)), detail_format, tag_box, brush);
    detail_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    detail_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // Every letter is a capital, here as in the panel (decided with the user, 2026-10-25).
    std::wstring title = wide_path(card.title);
    std::wstring detail = wide_path(card.detail);
    CharUpperBuffW(title.data(), static_cast<DWORD>(title.size()));
    CharUpperBuffW(detail.data(), static_cast<DWORD>(detail.size()));
    const float text_left = 76.0f;
    const float text_right = card_width - 16.0f;
    brush->SetColor(color(card.colors.ink));
    // One line of detail sits low enough to look centred; two lines need the whole card. 37 characters is one line at this size.
    const bool short_detail = detail.size() <= 37;
    const float title_top = detail.empty() ? (card_height - 20.0f) / 2.0f : short_detail ? 26.0f : 16.0f;
    target->DrawText(title.c_str(), static_cast<UINT32>(title.size()), title_format,
                     D2D1::RectF(text_left, title_top, text_right, title_top + 20.0f), brush);
    if (!detail.empty()) {
      brush->SetColor(color(card.colors.muted));
      target->DrawText(detail.c_str(), static_cast<UINT32>(detail.size()), detail_format,
                       D2D1::RectF(text_left, short_detail ? 51.0f : 41.0f, text_right, short_detail ? 84.0f : 85.0f), brush);
    }
    ok = SUCCEEDED(target->EndDraw());
  }
  release(ellipsis);
  release(detail_format);
  release(title_format);
  release(brush);
  release(target);
  if (!ok) return false;
  position_ = position;
  size_ = SIZE{width, height};
  return true;
}

void Toast::set_alpha(BYTE alpha) {
  if (!memory_dc_) return;
  POINT origin{0, 0};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
  HDC screen = GetDC(nullptr);
  UpdateLayeredWindow(window_, screen, &position_, &size_, memory_dc_, &origin, 0, &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}

void Toast::hide() {
  if (window_) {
    KillTimer(window_, kFadeTimer);
    set_fine_timer(false);
    if (visible_) ShowWindow(window_, SW_HIDE);
  }
  visible_ = false;
  leaving_ = false;
  // Nothing of the card outlives it: the bitmap and its DC go with the window.
  if (memory_dc_) {
    SelectObject(memory_dc_, previous_bitmap_);
    DeleteObject(bitmap_);
    DeleteDC(memory_dc_);
    memory_dc_ = nullptr;
    bitmap_ = nullptr;
  }
}

}  // namespace bx
