// The engine's own notification card: "Clip saved", "Screenshot saved", "Recording started".
//
// Drawn by the engine with Direct2D and DirectWrite into a per-pixel-alpha layered window, so a
// notification needs no interface process, no WebView and no Windows toast history: it appears at the
// top right of the monitor being played on, stays ~3 s, fades, and is gone with every byte it used. It
// never takes focus. Over a fullscreen game, borderless included, a topmost card can cost the game its
// independent flip and stutter the game and the clip, so there the card is skipped unless the person asked
// for it (notify_over_fullscreen). A chime plays for a notice that asked for one when the sound setting is on,
// and also when its card was skipped over a game, because then the chime is the only way to know a clip was
// saved; each event has its own, so the ear says which one it was. While the buffer, a recording or Timeline is
// capturing, no chime plays - the card still draws - because the sound would be recorded into the clip. A notice
// that asked for no sound makes none, drawn or skipped. A click brings BuffrX forward. One card at a time: a new
// notification replaces the one on screen.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bx {

enum class ToastKind { success, info, warning };

// Which sound a notice asks for. `none` is the default of every notice: a background notice (the buffer
// lowered its peak, an encoder step) is a card and nothing else - over a game, where the card is skipped, it
// used to be an unexplained beep.
enum class ToastChime { none, saved, quick, shot, started, stopped, alert };
inline bool toast_plays_sound(ToastChime chime) { return chime != ToastChime::none; }
// The chime plays when the notice asked for one and the person has the sound on - or the card was skipped over
// a fullscreen game, where nothing else says the clip was saved. Never while the buffer, a recording or Timeline
// is capturing: the chime would be recorded into the clip.
inline bool toast_should_play(ToastChime chime, bool sound_enabled, bool card_skipped, bool capture_active,
                                bool sound_while_capturing = false) {
  if (!toast_plays_sound(chime)) return false;
  if (capture_active && !sound_while_capturing) return false;
  return sound_enabled || card_skipped;
}

// Over a fullscreen app the card is skipped, except a warning: a saved clip or a started recording is
// not worth the game's frame pacing, but a problem has to be seen.
inline bool toast_visible_over_fullscreen(ToastKind kind) { return kind == ToastKind::warning; }

// Whether a card is drawn: always off a fullscreen app; over one, a warning, or any card when the person
// asked for them (`show_over_fullscreen`, the notify_over_fullscreen setting).
inline bool toast_card_visible(ToastKind kind, bool over_fullscreen, bool show_over_fullscreen) {
  return !over_fullscreen || show_over_fullscreen || toast_visible_over_fullscreen(kind);
}

// What happens to one notice's card. `skip_panel`: a BuffrX window is really in front of the user and draws its own
// card for a clip or screenshot saved, so the desktop card would be a second one. A warning never defers to the
// panel (it has no card for a hotkey failure), and a panel that is merely open - minimized, behind a game - is not
// "in front": it used to swallow the card and the chime together.
enum class ToastCardDecision { draw, skip_fullscreen, skip_panel };

inline ToastCardDecision toast_card_decision(ToastKind kind, bool over_fullscreen, bool show_over_fullscreen,
                                             bool panel_in_front) {
  if (panel_in_front && kind != ToastKind::warning) return ToastCardDecision::skip_panel;
  return toast_card_visible(kind, over_fullscreen, show_over_fullscreen) ? ToastCardDecision::draw
                                                                         : ToastCardDecision::skip_fullscreen;
}

// Whether the chime plays: only for a notice that asked for one, when the sound is on - or when the card was skipped
// over a fullscreen game, where the chime is the only sign. The panel's own card is silent, but it is in front of
// the user, so nothing is forced then. `capture_active` holds every chime: the buffer, a recording or Timeline is
// writing, and the sound would end up in the file.
inline bool toast_chime_plays(ToastCardDecision decision, ToastChime chime, bool sound_enabled, bool capture_active,
                               bool sound_while_capturing = false) {
  return toast_should_play(chime, sound_enabled, decision == ToastCardDecision::skip_fullscreen, capture_active,
                           sound_while_capturing);
}

// 0xRRGGBB colours of one theme, from the table every surface paints from (themes.h).
struct ToastColors {
  uint32_t accent = 0xff6b2c;
  uint32_t on_accent = 0x140a04;
  uint32_t surface = 0x141517;
  uint32_t line = 0x2b2e33;
  uint32_t ink = 0xe6e2da;
  uint32_t muted = 0x8f8b84;
  uint32_t warn = 0xf2c14e;
};

// The card's colours for a stored theme id; an older cockpit or an unknown id paints the main theme.
ToastColors toast_colors(const std::string& theme);

// True while a fullscreen app owns the foreground, borderless games included: a topmost window can
// take such a game out of independent flip, and the stutter shows in the game and in every clip. The
// notification card and the recording mark stay away then (a chime a notice asked for still plays).
bool fullscreen_app_running();

// A short chime as a complete in-memory WAV (44.1 kHz, 16-bit mono), played with PlaySound from memory, no
// file on disk. Saved is Rewind's two notes (1046.5 Hz, then 1568 Hz 60 ms later); started climbs, stopped
// falls, an alert is two flat pulses. `none` is an empty sound that is never played.
const std::vector<uint8_t>& toast_chime_wav(ToastChime chime);

class Toast {
public:
  ~Toast();

  // Creates the hidden card window on the calling thread, which must run a message loop (the engine's
  // main thread). `on_click` runs on that thread.
  // `font_dir` holds the interface's JetBrains Mono files; without them the card is set in Consolas.
  bool create(std::function<void()> on_click, const std::string& font_dir = std::string());
  void destroy();

  // Thread-safe: queues the card to the window's thread. `detail` is one line under the title.
  // `chime` is what the notice asks for; `sound_enabled` is the person's setting. `show_over_fullscreen` draws
  // the card even over a fullscreen game (the person's choice, at the cost described above). `panel_in_front`: a
  // BuffrX window is really in front of the user (UiHost::foreground_is_ours) and draws its own card for a success
  // or info notice. `capture_active`: the buffer, a recording or Timeline is writing, so the chime is held (the card
  // still draws). Every notice logs what was decided.
  void show(ToastKind kind, const std::string& title, const std::string& detail, ToastChime chime, bool sound_enabled,
            const ToastColors& colors, bool show_over_fullscreen = false, bool panel_in_front = false, bool compact = false,
            bool capture_active = false, bool sound_while_capturing = true);

private:
  struct Card;
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
  void present(Card* card);
  bool render(const Card& card);
  void set_alpha(BYTE alpha);
  void hide();
  void set_fine_timer(bool on);

  HWND window_ = nullptr;
  std::function<void()> on_click_;
  std::string font_dir_;
  // The rendered card, kept for the fades: a DIB section selected into a memory DC.
  HDC memory_dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ previous_bitmap_ = nullptr;
  POINT position_{};
  SIZE size_{};
  // Fade state, driven by a 15 ms timer while the card is up.
  uint32_t shown_at_ = 0;
  uint32_t hold_ms_ = 3400;
  uint32_t fade_out_at_ = 0;
  bool visible_ = false;
  bool leaving_ = false;
  bool fine_timer_ = false;  // timeBeginPeriod(1) is held: only while the card fades, so the fade steps every ~8 ms
};

}  // namespace bx
