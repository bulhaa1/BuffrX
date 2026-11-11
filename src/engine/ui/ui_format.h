// How the native interface words numbers, paths, states and hotkeys: Rewind's conventions ("0:42",
// "1m 30s", "12 mb", "12:46 PM" today / "Yesterday" / "Oct 12" before). Pure functions, no ImGui and
// no window, so tests/engine_tests.cpp guards them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../shared/json.h"
#include "../hotkey_defaults.h"

namespace bx::ui {

// The shipped bindings live in the engine's own table, so Settings' Reset and a fresh config agree.
using bx::default_hotkey;

std::string format_duration(double seconds);  // "0:42", "1:02:03"
std::string format_span(double seconds);      // "45s", "2m", "1m 30s"
std::string format_ago(int64_t unix_seconds, int64_t now_unix_seconds);  // "5 seconds ago", "1 month ago"
std::string format_bytes(double bytes);       // "12 kb", "34 mb", "1.25 gb", "12.3 gb"
// A file time as a person reads it: the time of day today, "Yesterday", else "Oct 12".
std::string format_when(int64_t unix_seconds, int64_t now_unix_seconds);

std::string short_path(const std::string& path, size_t max_bytes = 46);  // "C:\Users…\Clips"
std::string file_name(const std::string& path);
std::string lower_ascii(const std::string& value);  // A-Z only: pane titles, key legends
// The capital a letter is drawn as: the interface is set in capitals, so the atlas points each lower-case glyph at
// its capital. Latin, Latin-1, Greek and Cyrillic; anything else (and a letter with no capital) maps to itself.
unsigned upper_codepoint(unsigned c);
// A clip's name as its card shows it: "Replay 2026-10-20 10-00-00" is "2026-10-20 10-00-00" (the card's first line already says
// what it is), so the date, the part that tells two clips apart, is what fits.
std::string card_title(const std::string& name);
std::string file_stem(const std::string& path);
std::string quality_label(const std::string& level);  // high → High; a version 3 tier reads as its level
std::string encoder_name(const std::string& encoder_id);  // "NVIDIA NVENC · H.264"

// How the encoder page words a rate. Below 1000 kbps is said in kbps, because "0.8 Mbps" reads like a
// mistake; above, one decimal, because the difference between 32 and 35 Mbps is the whole decision.
std::string format_bitrate(int kbps);
// "H.264", "HEVC" or "AV1" as the page writes them, for a sentence rather than a select.
std::string codec_name(const std::string& codec);
// The line under the bitrate row, comparing this codec against H.264 at the same size - the whole
// reason a codec is worth choosing. Empty for H.264, which is the baseline.
std::string codec_size_note(const std::string& codec, int width, int height, int fps, const std::string& tier);

// The share sheet's lines. Size: "143 mb → 19.6 mb · 720p", or "12 mb · 1080p · sent as it is" when the
// file already fits (out_bytes equal to source_bytes). Quality, from the share of the source's bits per
// pixel the fitted file keeps: "Original quality" for a copy, then "Close to the original", "Softer in
// fast motion", "Visibly compressed".
std::string share_size_line(double source_bytes, double out_bytes, int height);
std::string share_quality(bool copy, double kept);

// BuffrX links. Why a clip did not arrive (the helper's reason code, or a sentence the engine wrote,
// which passes through), and "expires in 52 min" / "expires in 3 h" / "expired".
std::string link_failure(const std::string& reason);
std::string link_expiry(int64_t seconds_left);
// The line under a live link: what the send is doing and the one thing that would make it faster.
// "nobody has opened it yet", "direct", "through a relay - slower", and a cap the person set is named
// with where to change it. Empty parts are dropped rather than shown as a blank.
std::string link_advice(bool watching, const std::string& via, int upload_kbps);

// The one line the status strip and the quick panel show.
enum class Tone { ok, idle, busy, bad };
struct StatusLine {
  Tone tone = Tone::busy;
  std::string text;
};
StatusLine status_line(const Json& status, bool connected);

// Why a new clip or screenshot name is refused, or "" when it is fine (Windows' rules).
std::string rename_problem(const std::string& name);

std::vector<std::string> keycaps(const std::string& chord);  // "Ctrl+Alt+1" → Ctrl, Alt, 1

// A key pressed in the hotkey recorder. `waiting` while only modifiers are down; otherwise the chord
// in the engine's format and, when it would fire while typing or takes more than three parts, the
// problem to show.
struct ChordCapture {
  bool waiting = true;
  std::string chord;
  std::string problem;
};
ChordCapture capture_chord(bool ctrl, bool alt, bool shift, bool win, unsigned virtual_key);
// The modifiers held right now, e.g. "Ctrl+Alt", so the box shows the chord building up rather
// than nothing until a letter arrives.
std::string chord_preview(bool ctrl, bool alt, bool shift, bool win);

// The buffer-length chips: Rewind's four plus the current value when it is not one of them.
std::vector<int> limit_chips(int current_seconds);
int clamp_buffer_seconds(int seconds);  // kBufferSecondsMin..kBufferSecondsMax
// A list of presets with the current value added (sorted), for selects that must show what is set.
std::vector<int> with_current(std::vector<int> presets, int current);
// What Performance mode records, in one line: "1080p \xC2\xB7 60 fps \xC2\xB7 H.264 \xC2\xB7 about 95 MB a minute". The memory part is
// left out when the peak rate is unknown (0).
std::string performance_summary(int out_height, int fps, int peak_kbps);
// The second line of the "Clip saved" card: "Replays  \xC2\xB7  0:20". The folder part is left out
// at the library's root. The card has one line; the hotkey is in Settings and the tray menu.
std::string clip_saved_detail(const std::string& folder, const std::string& length);
// 16:9 output size for a picked height, width even ("1080" → 1920x1080).
void resolution_for_height(int height, int& width, int& out_height);

// The Timeline in the native interface (timeline_state.h, docs/timeline-blueprint.md): its Settings row, the four
// lengths the engine offers, what each keeps, and the Record key's label while a session writes.
const char* timeline_title();                        // "TIMELINE"
std::vector<std::string> timeline_length_choices();  // OFF, 30 MIN, 1 H, 2 H
int timeline_length_minutes(int index);              // 0, 30, 60, 120; 0 for an index that does not exist
int timeline_length_index(int minutes);              // the chip a setting selects; OFF for a length not offered
std::string timeline_length_label(int minutes);      // "OFF", "30 MIN", "1 H", "2 H"
// What the setting keeps, in the words the person consents to: footage on the disk, the microphone, and the fact
// that closing BuffrX leaves what is written there until it runs again (no exact deletion is ever promised).
std::string timeline_disclosure();
std::string timeline_hover();
// The writer's state in one line ("Writing", "Paused: the captured game is not in front", ...). Never empty.
std::string timeline_state_note(const std::string& state);
// What Timeline is doing now, for the status card at the top of its settings: a headline and one line under it.
// `game` is the program recorded as a game (empty for the desktop); `everywhere` and `minutes` are the settings.
struct TimelineStatus {
  std::string headline;
  std::string detail;
  // 0 quiet (off, stopped), 1 recording, 2 paused for a reason the person can act on.
  int tone = 0;
};
TimelineStatus timeline_status(const std::string& state, const std::string& game, bool everywhere, int minutes);
// The idle choices, in the order the row shows them: Off, 5, 15, 30, 60 minutes.
std::vector<int> timeline_idle_choices();
std::string timeline_idle_label(int minutes);
// The Record row's label: RECORD, STOP RECORDING while a manual recording runs. The engine's own rule
// (record_action) decides what the key does.
std::string record_label(bool manual_active);
// The same row's glyph (the red square for a recording to come, a stop square for what ends one)
// and the sentence its tooltip says, from the same rule, so the three never disagree about what the press will do.
std::string record_icon(bool manual_active);
std::string record_hint(bool manual_active);

// The choices the sharing and quick-clip rows offer, in the order they draw them: Discord's upload
// limits in MiB, and the quick clip's length in seconds. Shared so the two pages cannot disagree.
std::vector<int> share_limit_choices();
std::vector<int> quick_clip_choices();

}  // namespace bx::ui
