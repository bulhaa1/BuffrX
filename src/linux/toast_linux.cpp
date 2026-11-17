// Linux notifications: the desktop's own notification daemon for the card, and a short WAV through
// PulseAudio (or ALSA) for the chime. The card is not drawn in-process; the desktop already draws it.
#include "../engine/toast.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../engine/themes.h"
#include "../shared/log.h"

namespace bx {
namespace {
void put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}
void put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}
struct ChimeNote {
  double frequency;
  double start_s;
  double gain;
};
std::vector<uint8_t> render_chime(const std::vector<ChimeNote>& notes, double length_s, double decay, double attack_s = 0.060) {
  const uint32_t rate = 44100;
  const uint32_t samples = static_cast<uint32_t>(rate * length_s);
  std::vector<uint8_t> out;
  out.reserve(44 + samples * 2);
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  put32(out, 36 + samples * 2);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(out, 16);
  put16(out, 1);
  put16(out, 1);
  put32(out, rate);
  put32(out, rate * 2);
  put16(out, 2);
  put16(out, 16);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  put32(out, samples * 2);
  const double pi = 3.14159265358979323846;
  for (uint32_t i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / rate;
    double value = 0.0;
    for (const auto& note : notes) {
      const double local = t - note.start_s;
      if (local < 0) continue;
      const double attack = attack_s > 0 ? std::min(1.0, local / attack_s) : 1.0;
      value += std::sin(2.0 * pi * note.frequency * local) * note.gain * attack * std::exp(-decay * local);
    }
    const int16_t sample = static_cast<int16_t>(std::max(-1.0, std::min(1.0, value)) * 32767.0);
    put16(out, static_cast<uint16_t>(sample));
  }
  return out;
}
bool command_exists(const char* name) {
  const std::string command = std::string("command -v ") + name + " >/dev/null 2>&1";
  return std::system(command.c_str()) == 0;
}
}  // namespace

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

const std::vector<uint8_t>& toast_chime_wav(ToastChime chime) {
  static const std::vector<uint8_t> saved = render_chime({{659.3, 0.0, 0.22}, {987.8, 0.09, 0.15}}, 0.75, 4.0, 0.05);
  static const std::vector<uint8_t> quick = render_chime({{987.8, 0.0, 0.22}, {659.3, 0.09, 0.15}}, 0.75, 4.0, 0.05);
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

bool fullscreen_app_running() {
  Display* display = XOpenDisplay(nullptr);
  if (!display) return false;
  const Atom active_atom = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
  Atom type = None;
  int format = 0;
  unsigned long count = 0, after = 0;
  unsigned char* data = nullptr;
  bool fullscreen = false;
  if (XGetWindowProperty(display, DefaultRootWindow(display), active_atom, 0, 1, False, XA_WINDOW, &type,
                         &format, &count, &after, &data) == Success && data && count) {
    const Window window = *reinterpret_cast<Window*>(data);
    XWindowAttributes attributes{};
    if (window && XGetWindowAttributes(display, window, &attributes)) {
      fullscreen = attributes.width >= DisplayWidth(display, DefaultScreen(display)) &&
                   attributes.height >= DisplayHeight(display, DefaultScreen(display));
    }
  }
  if (data) XFree(data);
  XCloseDisplay(display);
  return fullscreen;
}

Toast::~Toast() = default;

bool Toast::create(std::function<void()> on_click, const std::string&) {
  on_click_ = std::move(on_click);
  return true;
}
void Toast::destroy() {}

void Toast::show(ToastKind kind, const std::string& title, const std::string& detail, ToastChime chime,
                 bool sound_enabled, const ToastColors&, bool show_over_fullscreen, bool panel_in_front,
                 bool compact, bool capture_active, bool sound_while_capturing) {
  (void)compact;
  const ToastCardDecision decision = toast_card_decision(kind, fullscreen_app_running(), show_over_fullscreen, panel_in_front);
  const bool chime_plays = toast_chime_plays(decision, chime, sound_enabled, capture_active, sound_while_capturing);
  if (decision == ToastCardDecision::draw && !std::getenv("BUFFRX_TEST_NO_DIALOGS")) {
    auto quote = [](const std::string& text) {
      std::string out = "'";
      for (char c : text) {
        if (c == '\'') out += "'\\''";
        else out += c;
      }
      out += "'";
      return out;
    };
    const std::string command = std::string("notify-send -a BuffrX -t 4000 -- ") + quote(title) + " " + quote(detail) + " >/dev/null 2>&1 &";
    std::system(command.c_str());
  }
  if (chime_plays) {
    static int sequence = 0;
    const std::string path = "/tmp/buffrx-chime-" + std::to_string(static_cast<long>(getpid())) + "-" +
                             std::to_string(++sequence) + ".wav";
    const std::vector<uint8_t>& wav = toast_chime_wav(chime);
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file) {
      std::fwrite(wav.data(), 1, wav.size(), file);
      std::fclose(file);
      if (command_exists("paplay")) std::system(("paplay " + path + " >/dev/null 2>&1 &").c_str());
      else if (command_exists("aplay")) std::system(("aplay -q " + path + " >/dev/null 2>&1 &").c_str());
    }
  }
  BX_LOG_INFO("notice \"%s\": %s", title.c_str(), decision == ToastCardDecision::draw ? "drawn" : "suppressed");
}

}  // namespace bx
