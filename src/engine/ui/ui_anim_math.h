// The arithmetic of the interface's motion, with no ImGui in it so it is unit-tested.
// Motion is time-based (real seconds), never frame-based: a frame every 6 ms and a frame every 30 ms
// arrive at the same place at the same moment.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bx::ui {

// A spinner has 8 cells and a bright head: about 30 frames a second move it smoothly.
inline constexpr uint32_t kSpinnerMs = 33;

// One step of exponential smoothing toward `target`: it covers 1 - e^(-rate*dt) of the distance, so
// `rate` 16 settles in about 0.2 s and 30 in about 0.1 s. A value within a thousandth of the target lands
// on it, so a value that has arrived stops asking for frames.
inline float anim_step(float value, float target, float rate, float dt) {
  const float next = value + (target - value) * (1.0f - std::exp(-rate * std::max(0.0f, dt)));
  return std::fabs(target - next) < 0.002f ? target : next;
}

// One step of a scroll glide. ImGui keeps a scroll offset in whole pixels (it rounds what SetScrollY is given), so a
// smoothed step that rounds back to where the list already is never arrives: 2 -> 1.77 -> 2 -> 1.77 ... asked for a
// frame every vsync for as long as the window was open (the engine's UI thread sat in Present at 100% of a core).
// Closer than a pixel is there, and every step moves at least one pixel.
inline float glide_step(float current, float target, float rate, float dt) {
  const float next = anim_step(current, target, rate, dt);
  if (std::fabs(target - next) < 1.0f) return target;
  if (std::round(next) == std::round(current)) return current + (target > current ? 1.0f : -1.0f);
  return next;
}

// Ease-out cubic: fast at the start, settles softly. `t` is progress 0..1.
inline float ease_out(float t) {
  const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
  return 1.0f - u * u * u;
}

// Progress 0..1 of a transition that began `elapsed` seconds ago and lasts `seconds`.
inline float progress(double elapsed, float seconds) {
  return seconds <= 0.0f ? 1.0f : std::clamp(static_cast<float>(elapsed / seconds), 0.0f, 1.0f);
}

// The motion tokens (docs/ui-fluidity-plan-2026-11-07.md, 6.2). ui/src/motion.css carries the same values; test_motion holds the two
// together. Durations are seconds; rates are per second, for exponential retargeting (a step covers 1 - e^(-rate*dt) of the way).
constexpr float kDurFast = 0.090f;    // hovers, a press's release, a switch's knob, a tab's underline, every exit
constexpr float kDurBase = 0.140f;    // menus, popovers, tab content, a card's insert or removal, a thumbnail's fade-in
constexpr float kDurSlow = 0.220f;    // a whole view arriving, a window opening: the ceiling for any transition
constexpr float kRateHover = 30.0f;   // a hover or a press follows its target
constexpr float kRateState = 24.0f;   // a state: a switch, a chosen cell, a selected tab
constexpr float kRateLayout = 18.0f;  // a height or a fold
constexpr float kRateScroll = 26.0f;  // a scroll glide
constexpr float kDistancePx = 8.0f;   // a rise or a slide at 1x; the caller scales it
constexpr float kPressScale = 0.985f; // a pressed control's scale
constexpr float kPopScale = 0.96f;    // a card or a chip popping in
constexpr int kStaggerMs = 30;        // between the items of a list that fills in
constexpr int kStaggerMax = 6;        // at most this many items stagger; the rest arrive together

// Exit curve: ease-in cubic, so a departure starts slowly and speeds up.
inline float ease_in(float t) {
  const float u = std::clamp(t, 0.0f, 1.0f);
  return u * u * u;
}

// A fixed-length move from `from` to `to`, `elapsed` seconds in, along `curve` (eased out by default). It lands exactly on `to`, so it
// stops asking for frames.
inline float tween_value(float from, float to, double elapsed, float seconds, float (*curve)(float) = ease_out) {
  const float p = progress(elapsed, seconds);
  return p >= 1.0f ? to : from + (to - from) * curve(p);
}

// Whether a view's kept appearance must start over. The slot belongs to the frame sequence that drew it, and a window's
// ImGui context can be made again at the address the last one had: its frame count then runs behind the stored one, the
// elapsed time reads as negative, progress clamps to 0, and the view stays behind its ground - the panel's whole body
// plain canvas until the view is closed and opened again.
inline bool appear_restart(int stored_frame, int frame) { return stored_frame < frame - 1 || stored_frame > frame; }

}  // namespace bx::ui
