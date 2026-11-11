// How steadily the native interface draws, and when it may draw. Pure logic, no window and no GPU in it,
// so the pacing rule and the numbers `ui.frame_stats` reports are unit-tested.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace bx::ui {

// Gaps between presented frames and the delay from an input to the frame that shows it, in milliseconds.
struct FrameSummary {
  int frames = 0;  // gaps counted
  double avg_ms = 0, p95_ms = 0, max_ms = 0;
  int inputs = 0;  // input-to-frame delays counted
  double latency_avg_ms = 0, latency_p95_ms = 0, latency_max_ms = 0;
};

// What an input was, so `ui.frame_stats` can say which kind is slow: a pointer move, a click, a key, a wheel notch or a pad press.
enum class InputKind : int { pointer = 0, click, key, wheel, pad };
constexpr int kInputKinds = 5;

inline const char* input_kind_name(InputKind kind) {
  switch (kind) {
    case InputKind::pointer: return "pointer";
    case InputKind::click: return "click";
    case InputKind::key: return "key";
    case InputKind::wheel: return "wheel";
    case InputKind::pad: return "pad";
  }
  return "pointer";
}

// The kind of an injected `ui.input`, by what it carries: a click, a key or a wheel notch, else a bare move.
inline InputKind ui_input_kind(bool click, bool key, bool wheel) {
  if (click) return InputKind::click;
  if (key) return InputKind::key;
  if (wheel) return InputKind::wheel;
  return InputKind::pointer;
}

class FrameStats {
public:
  static constexpr size_t kKeep = 512;
  // The window draws on demand, so a long silence is it going idle, not a slow frame: it is no gap.
  static constexpr int64_t kIdleGapUs = 100000;

  // An input reached the interface. Only the first since the last frame counts toward the mixed figure: it waited longest.
  // Each kind also keeps its first waiting input, so a click that arrives with a move reads apart from the move.
  void input(int64_t at_us, InputKind kind) {
    const int k = static_cast<int>(kind);
    if (!kind_waiting_[k]) {
      kind_waiting_[k] = true;
      kind_input_us_[k] = at_us;
    }
    if (waiting_) return;
    waiting_ = true;
    input_us_ = at_us;
  }

  void present(int64_t at_us) {
    ++presented_;
    if (have_last_ && at_us - last_us_ <= kIdleGapUs) keep(gaps_, static_cast<double>(at_us - last_us_) / 1000.0);
    last_us_ = at_us;
    have_last_ = true;
    answer(at_us);
    if (open_waiting_) {
      open_ms_ = static_cast<double>(at_us - open_at_us_) / 1000.0;
      open_waiting_ = false;
    }
  }

  // A window was asked to appear. The frame that first reaches the display closes it, so `open_ms` is the time from `ui.show` to
  // the picture, not the time the interface spent drawing. A show of a window already on screen names nothing and measures nothing.
  void opened(int64_t at_us) {
    open_at_us_ = at_us;
    open_waiting_ = true;
  }
  double open_ms() const { return open_ms_; }

  void reset() { *this = FrameStats(); }

  // Counters beside the gaps. A gap over 100 ms is idle time and is not counted, so a window that draws a few frames a second
  // shows no gaps at all: `presented` counts every frame, `built` the time one took from NewFrame to Render, `reason` why a
  // frame was drawn (an input, new data, a named animation), so a window that draws when nothing changes names its cause.
  int64_t presented() const { return presented_; }
  void built(int64_t build_us) {
    ++builds_;
    build_sum_us_ += build_us;
    build_max_us_ = std::max(build_max_us_, build_us);
  }
  int64_t builds() const { return builds_; }
  int64_t build_avg_us() const { return builds_ ? build_sum_us_ / builds_ : 0; }
  int64_t build_max_us() const { return build_max_us_; }
  // The frame's own parts, microseconds: the digest of the draw data, the draw calls, and the present (averages over the frames
  // drawn since the last reset). The frame costs what these add up to.
  void drawn(int64_t digest_us, int64_t draw_us, int64_t present_us) {
    ++draws_;
    digest_sum_us_ += digest_us;
    draw_sum_us_ += draw_us;
    present_sum_us_ += present_us;
  }
  int64_t digest_avg_us() const { return draws_ ? digest_sum_us_ / draws_ : 0; }
  int64_t draw_avg_us() const { return draws_ ? draw_sum_us_ / draws_ : 0; }
  int64_t present_avg_us() const { return draws_ ? present_sum_us_ / draws_ : 0; }
  // A frame built and found identical to the one on screen: not presented, so no gap. An input waiting for it is answered all the same - the
  // screen already shows what the input would have drawn - and its delay counts, or a skipped frame would read as a late one.
  void skipped(int64_t at_us) {
    ++skipped_;
    answer(at_us);
  }
  int64_t skipped_frames() const { return skipped_; }
  void reason(const std::string& name) { ++reasons_[name]; }
  const std::map<std::string, int64_t>& reasons() const { return reasons_; }

  FrameSummary summary() const {
    FrameSummary out;
    out.frames = static_cast<int>(gaps_.size());
    out.inputs = static_cast<int>(delays_.size());
    describe(gaps_, out.avg_ms, out.p95_ms, out.max_ms);
    describe(delays_, out.latency_avg_ms, out.latency_p95_ms, out.latency_max_ms);
    return out;
  }
  // The input-to-frame delays of one kind (`inputs` and the latency fields); the gaps are the whole window's.
  FrameSummary summary(InputKind kind) const {
    const auto& list = kind_delays_[static_cast<int>(kind)];
    FrameSummary out;
    out.inputs = static_cast<int>(list.size());
    describe(list, out.latency_avg_ms, out.latency_p95_ms, out.latency_max_ms);
    return out;
  }

private:
  static void keep(std::vector<double>& list, double value) {
    list.push_back(value);
    if (list.size() > kKeep) list.erase(list.begin());
  }
  // A frame, drawn or skipped, answers every input waiting for it.
  void answer(int64_t at_us) {
    if (waiting_) {
      keep(delays_, static_cast<double>(at_us - input_us_) / 1000.0);
      waiting_ = false;
    }
    for (int k = 0; k < kInputKinds; ++k) {
      if (!kind_waiting_[k]) continue;
      keep(kind_delays_[k], static_cast<double>(at_us - kind_input_us_[k]) / 1000.0);
      kind_waiting_[k] = false;
    }
  }
  // Nearest-rank 95th percentile.
  static void describe(std::vector<double> list, double& avg, double& p95, double& max) {
    avg = p95 = max = 0;
    if (list.empty()) return;
    std::sort(list.begin(), list.end());
    double sum = 0;
    for (double value : list) sum += value;
    avg = sum / static_cast<double>(list.size());
    p95 = list[static_cast<size_t>(std::ceil(0.95 * static_cast<double>(list.size()))) - 1];
    max = list.back();
  }

  std::vector<double> gaps_, delays_;
  std::vector<double> kind_delays_[kInputKinds];
  bool kind_waiting_[kInputKinds] = {};
  int64_t kind_input_us_[kInputKinds] = {};
  int64_t skipped_ = 0, presented_ = 0, builds_ = 0, build_sum_us_ = 0, build_max_us_ = 0;
  int64_t draws_ = 0, digest_sum_us_ = 0, draw_sum_us_ = 0, present_sum_us_ = 0;
  std::map<std::string, int64_t> reasons_;
  int64_t last_us_ = 0, input_us_ = 0, open_at_us_ = 0;
  double open_ms_ = 0;
  bool have_last_ = false, waiting_ = false, open_waiting_ = false;
};

// A window that is paced to the display draws when the display can take a frame. If it has not been able to
// for this long (the window is hidden behind another, the display is asleep) it draws anyway: waiting for a
// signal that does not come would freeze the picture.
constexpr int64_t kPacingGiveUpUs = 50000;

inline bool frame_allowed(bool paced, bool display_ready, int64_t since_last_present_us) {
  return !paced || display_ready || since_last_present_us > kPacingGiveUpUs;
}

// A window held by a fullscreen game (Phase 1.3) draws only into a free display slot: no give-up, so it never presents unsynced
// while the game's GPU time matters.
inline bool frame_allowed_strict(bool paced, bool display_ready) { return !paced || display_ready; }

// Frames nobody is waiting on (a blinking tag, a spinner) come at most this often: about 60 a second.
constexpr int64_t kSlowFrameGapUs = 16000;

inline bool slow_frame_due(int64_t since_last_present_us) { return since_last_present_us >= kSlowFrameGapUs; }

// A digest of a run of bytes, folded onto `h` (start from kDigestSeed): the interface compares the draw data of a frame with the
// last one it presented, and a frame with the same picture is not presented again. 8 bytes at a time; equal data gives an equal
// digest and a change anywhere gives a different one (the odds of two different frames agreeing are 2^-64).
constexpr uint64_t kDigestSeed = 0xcbf29ce484222325ull;
inline uint64_t digest_bytes(uint64_t h, const void* data, size_t size) {
  const unsigned char* p = static_cast<const unsigned char*>(data);
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t word;
    std::memcpy(&word, p + i, 8);
    h = (h ^ word) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
  }
  uint64_t tail = 0;
  if (i < size) {
    std::memcpy(&tail, p + i, size - i);
    h = (h ^ tail ^ (static_cast<uint64_t>(size - i) << 56)) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
  }
  return h;
}

// Whether a built frame is skipped as the same picture as the one on screen. `digest_valid` is false after a show, a resize, a
// capture, and a thumbnail texture made or freed (a freed texture's handle can come back for another picture, and the digest sees
// handles, not pixels). Never during a window's opening or a drag/resize, and never while something moves: the present is what paces
// a moving value, and a skipped frame does not take the display's slot, so the loop would come round at once and spin on a slow
// fade or hover (the pointer's too, once its frames follow the display).
inline bool frame_is_repeat(bool digest_valid, uint64_t digest, uint64_t shown, bool opening, bool size_move, bool moving) {
  return digest_valid && digest == shown && !opening && !size_move && !moving;
}

// A frame that an animation with a period asks for, on the QPC clock in microseconds (Phase 1.6: sub-16 ms timing is real). A
// spinner (`aligned` false) wants one `period_ms` after the last frame; a blink (`aligned` true) wants one when a multiple of the
// period has passed since the last frame, so it toggles on the beat.
inline bool animation_frame_due(uint32_t period_ms, bool aligned, int64_t now_us, int64_t last_present_us) {
  if (period_ms == 0) return true;
  const int64_t period = static_cast<int64_t>(period_ms) * 1000;
  if (aligned) return now_us / period != last_present_us / period;
  return now_us - last_present_us >= period;
}

// A frame an input or new data asked for. It follows the display, except while a fullscreen game runs: the interface is
// then on another screen or behind the game, and must not take the game's GPU time at the display's rate, so it draws
// like a slow frame.
inline bool asked_frame_due(bool game_running, int64_t since_last_present_us) {
  return !game_running || slow_frame_due(since_last_present_us);
}

// An unfocused window (not the one the user operates) draws its data at most 30 times a second, and draws no hover (Phase 1.7, D7).
constexpr int64_t kUnfocusedGapUs = 33333;

// Whether a window may draw now. A forced capture always may. The focused window follows the display (asked_frame_due: a slow gap
// only while a fullscreen game holds it). An unfocused window waits kUnfocusedGapUs between frames.
inline bool window_frame_due(bool focused, bool forced, bool game_holds, int64_t since_last_present_us) {
  if (forced) return true;
  if (!focused) return since_last_present_us >= kUnfocusedGapUs;
  return asked_frame_due(game_holds, since_last_present_us);
}

// A pointer move alone (no button, wheel or key) wakes an unfocused window for nothing: its hover is not drawn there (D7).
inline bool pointer_move_wakes(bool pointer_only, bool focused) { return !pointer_only || focused; }

// After an input the window draws one more frame, at this time. ImGui shows a tooltip once the pointer has been still for
// HoverStationaryDelay (0.15 s) and then HoverDelayNormal (0.40 s), and once the pointer stops nothing else draws; this frame lets a
// tooltip finish. It replaces the old 300-800 ms tail of frames at 62 a second.
constexpr int64_t kSettleUs = 600000;
// `settle_us` is 0 when no settle frame is waiting.
inline bool settle_due(int64_t settle_us, int64_t now_us) { return settle_us != 0 && now_us >= settle_us; }

}  // namespace bx::ui
