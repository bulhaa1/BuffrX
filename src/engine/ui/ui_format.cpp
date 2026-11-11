#include "ui_format.h"

#include "../encoder_settings.h"
#include "../config.h"
#include "../recording_watch.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <ctime>

#include "../platform.h"

namespace bx::ui {

std::string format_duration(double seconds) {
  if (!std::isfinite(seconds) || seconds <= 0) return "0:00";
  const long long total = std::llround(seconds);
  const long long hours = total / 3600, minutes = (total % 3600) / 60, rest = total % 60;
  char text[32];
  if (hours > 0) std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", hours, minutes, rest);
  else std::snprintf(text, sizeof(text), "%lld:%02lld", minutes, rest);
  return text;
}

std::string clip_saved_detail(const std::string& folder, const std::string& length) {
  return folder.empty() ? length : folder + "  \xC2\xB7  " + length;
}

std::string performance_summary(int out_height, int fps, int peak_kbps) {
  std::string line = std::to_string(out_height) + "p \xC2\xB7 " + std::to_string(fps) + " fps \xC2\xB7 H.264";
  if (peak_kbps > 0) {
    const int mb = static_cast<int>(std::lround(static_cast<double>(peak_kbps) * 60.0 / 8.0 / 1024.0));
    line += " \xC2\xB7 about " + std::to_string(mb) + " MB a minute";
  }
  return line;
}

std::string format_span(double seconds) {
  const long long s = std::max(0LL, std::llround(seconds));
  if (s < 60) return std::to_string(s) + "s";
  const long long minutes = s / 60, rest = s % 60;
  return rest ? std::to_string(minutes) + "m " + std::to_string(rest) + "s" : std::to_string(minutes) + "m";
}

std::string format_ago(int64_t unix_seconds, int64_t now_unix_seconds) {
  if (unix_seconds <= 0) return {};
  const int64_t elapsed = now_unix_seconds > unix_seconds ? now_unix_seconds - unix_seconds : 0;
  struct Unit { int64_t seconds; const char* name; };
  static const Unit units[] = {{365 * 86400LL, "year"}, {30 * 86400LL, "month"}, {86400, "day"},
                               {3600, "hour"}, {60, "minute"}, {1, "second"}};
  for (const Unit& unit : units) {
    if (elapsed >= unit.seconds || unit.seconds == 1) {
      const int64_t count = elapsed / unit.seconds;
      return std::to_string(count) + " " + unit.name + (count == 1 ? " ago" : "s ago");
    }
  }
  return {};
}

std::string format_bytes(double value) {
  const double mb = 1024.0 * 1024.0, gb = mb * 1024.0;
  char text[32];
  if (!std::isfinite(value) || value <= 0) return "0 KB";
  if (value < mb) {
    std::snprintf(text, sizeof(text), "%lld KB", std::max(1LL, std::llround(value / 1024.0)));
  } else if (value < gb) {
    std::snprintf(text, sizeof(text), "%lld MB", std::llround(value / mb));
  } else {
    std::snprintf(text, sizeof(text), value < 10 * gb ? "%.2f GB" : "%.1f GB", value / gb);
  }
  return text;
}

std::string format_when(int64_t unix_seconds, int64_t now_unix_seconds) {
  if (unix_seconds <= 0) return std::string();
  std::time_t when = static_cast<std::time_t>(unix_seconds), now = static_cast<std::time_t>(now_unix_seconds);
  std::tm at{}, today{};
  localtime_s(&at, &when);
  localtime_s(&today, &now);
  auto same_day = [](const std::tm& a, const std::tm& b) {
    return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
  };
  if (same_day(at, today)) {
    // The time of day in the user's own format ("12:46 PM", "12:46").
    SYSTEMTIME system{};
    system.wYear = static_cast<WORD>(at.tm_year + 1900);
    system.wMonth = static_cast<WORD>(at.tm_mon + 1);
    system.wDay = static_cast<WORD>(at.tm_mday);
    system.wHour = static_cast<WORD>(at.tm_hour);
    system.wMinute = static_cast<WORD>(at.tm_min);
    wchar_t wide[64] = {0};
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &system, nullptr, wide, 64) > 0) {
      char utf8[128] = {0};
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
      return utf8;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", at.tm_hour, at.tm_min);
    return text;
  }
  std::tm yesterday = today;
  yesterday.tm_mday -= 1;
  yesterday.tm_isdst = -1;
  std::time_t yesterday_time = std::mktime(&yesterday);
  std::tm yesterday_local{};
  localtime_s(&yesterday_local, &yesterday_time);
  if (same_day(at, yesterday_local)) return "Yesterday";
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return std::string(months[at.tm_mon]) + " " + std::to_string(at.tm_mday);
}

namespace {

// Moves `at` back to the start of the UTF-8 character it lands in.
size_t character_start(const std::string& text, size_t at) {
  while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
  return at;
}

}  // namespace

std::string short_path(const std::string& path, size_t max_bytes) {
  if (path.size() <= max_bytes) return path;
  const size_t keep = (max_bytes > 1 ? max_bytes - 1 : 0) / 2;
  const size_t head = character_start(path, keep);
  const size_t tail = character_start(path, path.size() - keep);
  return path.substr(0, head) + "\xE2\x80\xA6" + path.substr(tail);
}

std::string lower_ascii(const std::string& value) {
  std::string out = value;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return out;
}

// shortcut: Latin Extended-A (Polish, Czech, Turkish) keeps its lower case, because its pairs do not follow one rule; add them when a library needs it.
unsigned upper_codepoint(unsigned c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;            // à-þ, not ÷
  if (c == 0xFF) return 0x178;                                        // ÿ
  if (c >= 0x3B1 && c <= 0x3C9) return c == 0x3C2 ? 0x3A3 : c - 32;  // Greek; the final sigma is Σ too
  if (c >= 0x430 && c <= 0x44F) return c - 32;                        // Cyrillic а-я
  if (c >= 0x450 && c <= 0x45F) return c - 80;                        // ѐ-џ
  return c;
}

std::string card_title(const std::string& name) {
  for (const char* prefix : {"Replay ", "Screenshot "}) {
    const std::string head(prefix);
    if (name.size() > head.size() && name.compare(0, head.size(), head) == 0 && name[head.size()] >= '0' && name[head.size()] <= '9') {
      return name.substr(head.size());
    }
  }
  return name;
}

std::string file_name(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string file_stem(const std::string& path) {
  const std::string name = file_name(path);
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string quality_label(const std::string& level) {
  if (level == "low") return "Low";
  if (level == "standard" || level == "performance") return "Standard";
  if (level == "high" || level == "balanced") return "High";
  if (level == "highest" || level == "quality") return "Highest";
  if (level == "custom") return "Custom";
  return level;
}

std::string format_bitrate(int kbps) {
  if (kbps < 1000) return std::to_string(std::max(0, kbps)) + " kbps";
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f Mbps", kbps / 1000.0);
  return buffer;
}

std::string codec_name(const std::string& codec) {
  if (codec == "av1") return "AV1";
  if (codec == "hevc") return "HEVC";
  return "H.264";
}

std::string share_size_line(double source_bytes, double out_bytes, int height) {
  const std::string picture = std::to_string(std::max(0, height)) + "p";
  if (out_bytes >= source_bytes) return format_bytes(source_bytes) + " \xC2\xB7 " + picture + " \xC2\xB7 sent as it is";
  return format_bytes(source_bytes) + " \xE2\x86\x92 " + format_bytes(out_bytes) + " \xC2\xB7 " + picture;
}

std::string link_failure(const std::string& reason) {
  if (reason == "unreachable") return "Couldn't reach your friend's PC. It may be off, asleep or offline - ask them to check BuffrX is running.";
  if (reason == "expired") return "This link has expired. Ask for a new one.";
  if (reason == "full") return "Too many people are watching this clip right now. Try again in a minute.";
  if (reason == "refused" || reason == "bad_link") return "This link doesn't work any more - it may have been stopped. Ask for a new one.";
  if (reason == "busy") return "Your friend's PC is busy. Try again in a minute.";
  if (reason == "stalled" || reason == "protocol") return "The clip stopped arriving part-way. Try the link again.";
  if (reason == "too_large") return "That clip is larger than BuffrX accepts (4 GB).";
  if (reason == "not_a_clip") return "What arrived is not a clip BuffrX can play, so it was not kept.";
  if (reason == "gone") return "That clip is no longer on your friend's PC. Ask them for a new link.";
  if (reason == "no_space") return "There isn't enough free space for this clip.";
  if (reason == "io") return "The clip could not be saved to your clips folder.";
  if (reason == "helper") return "Sharing stopped unexpectedly. Try the link again.";
  if (reason.empty()) return "The clip did not arrive.";
  // A reason with no space in it is a code, not a sentence: it never reaches the person as one.
  if (reason.find_first_of(" ") == std::string::npos) return "The clip did not arrive. Try the link again.";
  return reason;  // already a sentence
}

std::string link_expiry(int64_t seconds_left) {
  if (seconds_left <= 0) return "expired";
  // Never more time than is left, and never "0 min" while the link still works.
  const int64_t minutes = std::max<int64_t>(1, seconds_left / 60);
  if (minutes < 60) return "expires in " + std::to_string(minutes) + " min";
  return "expires in " + std::to_string((minutes + 30) / 60) + " h";
}

// What a person can act on while a link is live, in the order it matters: a cap they set themselves,
// then the route, then whether anyone has taken it yet. A direct send says "direct" and nothing else,
// because a line that always warns teaches people to ignore it.
std::string link_advice(bool watching, const std::string& via, int upload_kbps) {
  std::vector<std::string> parts;
  if (upload_kbps > 0) {
    parts.push_back("capped at " + std::to_string((upload_kbps + 500) / 1000) + " Mbit/s - raise it in Settings");
  }
  if (via == "relay") parts.push_back(watching ? "through a relay - slower" : "last send went through a relay");
  else if (watching) parts.push_back("direct");
  else if (parts.empty()) parts.push_back("nobody has opened it yet");
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out += " \xC2\xB7 ";
    out += part;
  }
  return out;
}

std::string share_quality(bool copy, double kept) {  // The share of the source's bits per pixel the fitted file keeps. Above about two thirds a game
  // clip is hard to tell apart; below a quarter the blocks show in every fast pan.
  if (copy) return "Original quality";
  if (kept >= 0.6) return "Close to the original";
  if (kept >= 0.25) return "Softer in fast motion";
  return "Visibly compressed";
}

std::string codec_size_note(const std::string& codec, int width, int height, int fps, const std::string& tier) {
  // H.264 is the baseline every other codec is measured against, so it has nothing to compare to.
  if (codec == "h264" || codec.empty()) return std::string();
  const int here = bitrate_plan(codec, width, height, fps, tier);
  const int h264 = bitrate_plan("h264", width, height, fps, tier);
  if (here <= 0 || h264 <= 0 || h264 == here) return std::string();
  // Rounded, not truncated: truncating 51.7 to "51% smaller" undersells the codec by a point and a
  // half, and the number is the whole reason the codec is on the page.
  const int saved = static_cast<int>(std::lround((h264 - here) * 100.0 / static_cast<double>(h264)));
  // The saving is the reason to choose the codec; the absolute rate is what actually gets encoded.
  return std::to_string(saved) + "% smaller than H.264 at the same quality \xE2\x80\x94 about " + format_bitrate(here) +
         " instead of " + format_bitrate(h264);
}

std::string encoder_name(const std::string& id) {
  if (id.empty()) return "Encoder starting";
  auto has = [&](const char* part) { return id.find(part) != std::string::npos; };
  const std::string vendor = has("nvenc")                ? "NVIDIA NVENC"
                             : has("amf") || has("amd")  ? "AMD AMF"
                             : has("qsv")                ? "Intel Quick Sync"
                             : has("x264")               ? "x264 (CPU)"
                                                         : id;
  const std::string codec = has("av1") ? "AV1" : has("hevc") || has("265") ? "HEVC" : "H.264";
  return vendor + " \xC2\xB7 " + codec;
}

StatusLine status_line(const Json& status, bool connected) {
  StatusLine line;
  if (!connected || !status.is_object() || !status.has("buffer_state")) {
    line.tone = Tone::busy;
    line.text = "Connecting to the engine\xE2\x80\xA6";
    return line;
  }
  const std::string state = status["buffer_state"].as_string("off");
  const std::string error = status["last_error"].as_string();
  if (!error.empty() && state != "on") {
    line.tone = Tone::bad;
    // A code with no space in it is not a sentence: say what it means and where to go next.
    line.text = error.find(' ') == std::string::npos
                    ? "The buffer stopped. Start it again, or open Settings, System, and restart the engine."
                    : error;
    return line;
  }
  if (state == "on") {
    const Json& ring = status["stats"]["ring"];
    line.tone = Tone::ok;
    // The ring always keeps the length asked for, so this is the whole story.
    line.text = format_span(ring["filled"].as_double()) + " of " + format_span(ring["seconds"].as_double()) + " buffered";
  } else if (state == "starting") {
    line.tone = Tone::busy;
    line.text = status["capture"]["mode"].as_string() == "game" ? "Waiting for the game to draw a frame\xE2\x80\xA6"
                                                                 : "Starting the buffer\xE2\x80\xA6";
  } else if (state == "stopping") {
    line.tone = Tone::busy;
    line.text = "Stopping the buffer\xE2\x80\xA6";
  } else {
    line.tone = Tone::idle;
    line.text = "Buffer paused";
  }
  return line;
}

std::string rename_problem(const std::string& name) {
  size_t first = name.find_first_not_of(" \t");
  size_t last = name.find_last_not_of(" \t");
  if (first == std::string::npos) return "A name is needed";
  const std::string trimmed = name.substr(first, last - first + 1);
  if (trimmed.find_first_of("<>:\"/\\|?*") != std::string::npos) return "Names cannot contain < > : \" / \\ | ? *";
  if (trimmed.back() == '.') return "A name cannot end with a dot";
  std::string lower;
  for (char c : trimmed) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  static const char* reserved[] = {"con", "prn", "aux", "nul"};
  bool is_reserved = false;
  for (const char* word : reserved) is_reserved = is_reserved || lower == word;
  if (lower.size() == 4 && (lower.compare(0, 3, "com") == 0 || lower.compare(0, 3, "lpt") == 0) && lower[3] >= '1' &&
      lower[3] <= '9') {
    is_reserved = true;
  }
  if (is_reserved) return "Windows reserves that name";
  if (trimmed.size() > 100) return "At most 100 characters";
  return std::string();
}

std::vector<std::string> keycaps(const std::string& chord) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : chord) {
    if (c == '+') {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    } else if (c != ' ') {
      current.push_back(c);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

// The modifiers held right now, as the recorder shows them before a key completes the chord.
std::string chord_preview(bool ctrl, bool alt, bool shift, bool win) {
  std::string text;
  auto add = [&text](const char* name) {
    if (!text.empty()) text += "+";
    text += name;
  };
  if (ctrl) add("Ctrl");
  if (alt) add("Alt");
  if (shift) add("Shift");
  if (win) add("Win");
  return text;
}

ChordCapture capture_chord(bool ctrl, bool alt, bool shift, bool win, unsigned virtual_key) {
  ChordCapture capture;
  switch (virtual_key) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
      return capture;  // only modifiers so far
    default: break;
  }
  capture.waiting = false;
  UINT modifiers = 0;
  if (ctrl) modifiers |= MOD_CONTROL;
  if (alt) modifiers |= MOD_ALT;
  if (shift) modifiers |= MOD_SHIFT;
  if (win) modifiers |= MOD_WIN;
  capture.chord = format_chord(modifiers, virtual_key);
  // Three parts at most: Ctrl, Alt and the key. A fourth modifier turns a hotkey into a contortion.
  const int parts = 1 + (ctrl ? 1 : 0) + (alt ? 1 : 0) + (shift ? 1 : 0) + (win ? 1 : 0);
  if (capture.chord.find('?') != std::string::npos) {
    capture.problem = "That key cannot be a hotkey";
  } else if (parts > 3) {
    capture.problem = "Up to 3 keys: Ctrl, Alt and the key";
  } else if (modifiers == 0 && (virtual_key < VK_F13 || virtual_key > VK_F24)) {
    capture.problem = "Add Ctrl, Alt or Shift, so it never fires while you type";
  }
  return capture;
}

std::vector<int> limit_chips(int current_seconds) { return with_current({30, 60, 120, 300}, current_seconds); }

int clamp_buffer_seconds(int seconds) {
  return std::max(kBufferSecondsMin, std::min(kBufferSecondsMax, seconds));
}

std::vector<int> with_current(std::vector<int> presets, int current) {
  if (std::find(presets.begin(), presets.end(), current) == presets.end()) presets.push_back(current);
  std::sort(presets.begin(), presets.end());
  return presets;
}

void resolution_for_height(int height, int& width, int& out_height) {
  out_height = height;
  width = static_cast<int>(std::lround(height * 16.0 / 9.0 / 2.0)) * 2;
}

// ---------------------------------------------------------------- the Timeline

namespace {
// The four lengths the engine allows (config.h: timeline_minutes_allowed), in the order the row shows them.
constexpr int kTimelineLengths[] = {0, 30, 60, 120};
constexpr int kTimelineLengthCount = static_cast<int>(sizeof(kTimelineLengths) / sizeof(kTimelineLengths[0]));
}  // namespace

const char* timeline_title() { return "TIMELINE"; }

std::vector<std::string> timeline_length_choices() {
  std::vector<std::string> labels;
  for (int minutes : kTimelineLengths) labels.push_back(timeline_length_label(minutes));
  return labels;
}

int timeline_length_minutes(int index) {
  return index >= 0 && index < kTimelineLengthCount ? kTimelineLengths[index] : 0;
}

int timeline_length_index(int minutes) {
  for (int i = 0; i < kTimelineLengthCount; ++i) {
    if (kTimelineLengths[i] == minutes) return i;
  }
  return 0;  // a length the engine would refuse reads as Off, never as the nearest window
}

std::string timeline_length_label(int minutes) {
  switch (minutes) {
    case 30: return "30 MIN";
    case 60: return "1 H";
    case 120: return "2 H";
    default: return "OFF";
  }
}

std::string timeline_disclosure() {
  // One line for what it keeps, one for what survives: enough to consent, short enough to read at a glance.
  return "Keeps the last stretch of play on disk while a game runs, desktop and microphone included unless muted. It "
         "stays after BuffrX closes.";
}

std::string timeline_hover() {
  // The hover says what it is for; the Help sheet carries the detail.
  return "Keeps recent play so you can save a moment after it happens.";
}

std::string timeline_state_note(const std::string& state) {
  if (state == "stopped") return "Stopped: the buffer is off, and Timeline writes only beside it.";
  if (state == "writing") return "Writing footage to the disk.";
  if (state == "paused_manual") return "Paused: a recording is running.";
  if (state == "paused_rebuild") return "Paused: the settings are being applied.";
  if (state == "paused_disk") return "Stopped: free space on the clip drive is low.";
  if (state == "paused_no_game") return "Waiting for a game.";
  if (state == "paused_idle") return "Paused: no keyboard or mouse for a while. It writes again when you are back.";
  return "Off.";
}

TimelineStatus timeline_status(const std::string& state, const std::string& game, bool everywhere, int minutes) {
  TimelineStatus status;
  const std::string kept = minutes > 0 ? "the last " + timeline_length_label(minutes) : "recent play";
  if (state == "writing") {
    status.tone = 1;
    status.headline = game.empty() ? "Recording the desktop" : "Recording " + game;
    status.detail = "Keeping " + kept + " on the disk. Desktop and microphone sound are included unless muted.";
  } else if (state == "paused_no_game") {
    status.tone = 2;
    status.headline = "Waiting for a game";
    status.detail = everywhere ? "Nothing is recording yet." : "Games only: the desktop is not recorded. Choose Everywhere to record it too.";
  } else if (state == "paused_idle") {
    status.tone = 2;
    status.headline = "Idle";
    status.detail = "No keyboard or mouse for a while. It records again as soon as you are back.";
  } else if (state == "off" || state.empty()) {
    status.headline = "Timeline is off";
    status.detail = "Choose a length below to keep the last stretch of play on the disk.";
  } else {
    status.tone = state == "stopped" ? 0 : 2;
    status.headline = state == "stopped" ? "Stopped" : "Paused";
    // The note starts with the word the headline already says ("Paused: the settings are being applied.").
    status.detail = timeline_state_note(state);
    const size_t colon = status.detail.find(": ");
    if (colon != std::string::npos && colon < 10) {
      status.detail = status.detail.substr(colon + 2);
      if (!status.detail.empty()) status.detail[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(status.detail[0])));
    }
  }
  return status;
}

std::vector<int> timeline_idle_choices() { return {0, 5, 15, 30, 60}; }

std::string timeline_idle_label(int minutes) { return minutes <= 0 ? "Never" : std::to_string(minutes) + " min"; }

std::string record_label(bool manual_active) {
  // The key's meaning is the engine's rule (recording_watch.h), so the label cannot drift from what the press does.
  return buffrx::record_action(manual_active) == buffrx::RecordAction::ManualStop ? "STOP RECORDING" : "RECORD";
}

std::string record_icon(bool manual_active) {
  return buffrx::record_action(manual_active) == buffrx::RecordAction::ManualStop ? "stop" : "record";
}

std::string record_hint(bool manual_active) {
  return buffrx::record_action(manual_active) == buffrx::RecordAction::ManualStop
             ? "Stops the recording and saves it." : "Starts a recording. It runs until you stop it.";
}

std::vector<int> share_limit_choices() { return {20, 50, 100, 500}; }

std::vector<int> quick_clip_choices() { return {10, 15, 20, 30, 45, 60, 90}; }

}  // namespace bx::ui
