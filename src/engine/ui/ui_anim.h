// Motion for the native interface: small, time-based and free at rest.
//
// A value that is still moving (and a view that is still appearing) says so, and the window draws at the
// display's rate until everything has arrived; then it stops drawing, as before. Nothing here runs a timer
// or a frame of its own, so a still panel costs nothing.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "ui_anim_math.h"

namespace bx::ui {

namespace anim_detail {
// The motion maps are keyed by the ImGui context that drew the value: its address in the high 32 bits, the item's id
// low. A window's context is destroyed with it and the next window may be given the same address, so its entries are
// purged when it goes (B22). nullptr packs to 0 and belongs to nothing.
inline uint64_t context_key(const void* context) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context)) << 32;
}
inline bool key_belongs(uint64_t key, uint64_t owner) { return (key & ~0xffffffffull) == owner; }
inline bool moved = false;     // something has not arrived: draw again at the display's rate
inline bool spun = false;      // a spinner is on screen: draw again about 30 times a second
inline bool enabled = true;    // off: everything arrives at once (Windows' "show animations" is off)
struct Appear {
  double start = 0;
  int frame = -2;
};
inline std::unordered_map<uint64_t, Appear>& appearing() {
  static std::unordered_map<uint64_t, Appear> slots;
  return slots;
}
// Shorter than a frame at 60 Hz: the first frame after a pause must not jump most of the way.
inline float dt() { return std::min(ImGui::GetIO().DeltaTime, 1.0f / 60.0f); }
}  // namespace anim_detail

inline void anim_enable(bool on) { anim_detail::enabled = on; }
inline bool anim_enabled() { return anim_detail::enabled; }
// Something asked for another frame: read (and cleared) once per rendered frame by the window.
inline bool anim_take() {
  const bool moved = anim_detail::moved;
  anim_detail::moved = false;
  return moved;
}
inline void anim_touch() { anim_detail::moved = true; }
// A spinner moves, but it needs ~30 frames a second, not the display's rate: read once per frame like anim_take.
inline bool anim_take_slow() {
  const bool spun = anim_detail::spun;
  anim_detail::spun = false;
  return spun;
}
inline void anim_touch_slow() { anim_detail::spun = true; }

// A value that follows `target` smoothly. Keyed by `id` in the current window's storage; the first time it is
// seen it starts at the target, so nothing animates in from zero.
inline float anim_to(ImGuiID id, float target, float rate = kRateState) {
  float& value = *ImGui::GetStateStorage()->GetFloatRef(id, target);
  if (!anim_detail::enabled) return value = target;
  value = anim_step(value, target, rate, anim_detail::dt());
  if (value != target) anim_detail::moved = true;
  return value;
}

// The same for the item just drawn (a button's hover, a switch's knob). `salt` tells two values of one item apart.
inline float anim_item(float target, float rate = kRateState, ImGuiID salt = 0x51ed27u) {
  // A pane scrolled out of view skips its items, and the "item just drawn" is then not this one (its id is 0): every
  // cell of a row shared one slot, pulled to 1 by the chosen and to 0 by the others, and never settled - the window
  // drew at the display's rate for as long as the Video page was open. Nothing is drawn there, so nothing animates.
  if (ImGui::GetCurrentWindowRead()->SkipItems) return target;
  return anim_to(ImGui::GetItemID() ^ salt, target, rate);
}

// How far a view that has just appeared has come, 0..1, eased. It starts over whenever the view was not drawn on
// the previous frame (it was closed, or another view stood in its place); while it runs it asks for frames.
inline float appear(ImGuiID id, float seconds = kDurBase) {
  if (!anim_detail::enabled) return 1.0f;
  ImGuiContext* context = ImGui::GetCurrentContext();
  const uint64_t key = anim_detail::context_key(context) ^ id;
  anim_detail::Appear& slot = anim_detail::appearing()[key];
  const int frame = ImGui::GetFrameCount();
  const double now = ImGui::GetTime();
  if (appear_restart(slot.frame, frame)) slot.start = now;
  slot.frame = frame;
  const float t = progress(now - slot.start, seconds);
  if (t < 1.0f) anim_detail::moved = true;
  return ease_out(t);
}

// A value that jumps to 1 while the item just drawn is held down, so the press shows on its own frame, and eases back to 0 at the
// hover rate once it is let go. `salt` tells two values of one item apart.
inline float press_t(ImGuiID salt = 0x9e55u) {
  if (ImGui::GetCurrentWindowRead()->SkipItems) return 0.0f;
  const bool pressed = ImGui::IsItemActive();
  float& value = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID() ^ salt, 0.0f);
  if (pressed || !anim_detail::enabled) {
    value = pressed ? 1.0f : 0.0f;
  } else {
    value = anim_step(value, 0.0f, kRateHover, anim_detail::dt());
    if (value != 0.0f) anim_detail::moved = true;
  }
  return value;
}

namespace anim_detail {
struct Tween {
  float from = 0, to = 0, value = 0;
  double start = 0;
  int frame = 0;  // the frame that last drew it: a lower frame count than this is a context made again
  bool seen = false;
};
// Seconds on a clock that outlives the ImGui contexts. ImGui::GetTime() restarts at 0 whenever a context is made again (a window
// closed and opened, the main and quick panels swapped), while textures and these maps live on: a start stamped on the old clock
// would read as the future, progress would clamp to 0 and the picture would draw at alpha 0 for ever.
inline double wall_seconds() {
  static const auto origin = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}
inline std::unordered_map<uint64_t, Tween>& tweens() {
  static std::unordered_map<uint64_t, Tween> slots;
  return slots;
}
inline uint64_t key_of(ImGuiID id) { return anim_detail::context_key(ImGui::GetCurrentContext()) ^ id; }
}  // namespace anim_detail

// The step shared by tween_to and leave: a value moves to `target` over `seconds` along `curve`, from wherever it was when the
// target last changed, and lands exactly. The first time it is seen it starts at the target: nothing animates in from zero.
inline float tween_step(ImGuiID id, float target, float seconds, float (*curve)(float)) {
  anim_detail::Tween& t = anim_detail::tweens()[anim_detail::key_of(id)];
  const double now = anim_detail::wall_seconds();
  const int frame = ImGui::GetFrameCount();
  if (t.seen && frame < t.frame) t.seen = false;  // the context at this address was made again: start over at the target
  t.frame = frame;
  if (!t.seen) {
    t.from = t.to = t.value = target;
    t.start = now;
    t.seen = true;
    return target;
  }
  if (t.to != target) {
    t.from = t.value;  // the target changed mid-way: the move starts where the value is
    t.to = target;
    t.start = now;
  }
  if (!anim_detail::enabled) return t.value = target;
  t.value = tween_value(t.from, t.to, now - t.start, seconds, curve);
  if (t.value != target) anim_detail::moved = true;
  return t.value;
}

// A fixed-length tween toward `target`, eased out: an arrival, a fold, a shift with a known end. anim_to follows a pointer or a
// state instead.
inline float tween_to(ImGuiID id, float target, float seconds = kDurBase) { return tween_step(id, target, seconds, ease_out); }

// An exit: 1 while the item stays, eased to 0 over `seconds` (ease-in, so a departure speeds up), reversed from where it is if
// `closing` is undone. The owner keeps a popup or a window open while this reads above 0 and closes it at 0 (V2, V7; D6).
inline float leave(ImGuiID id, bool closing, float seconds = kDurFast) {
  return tween_step(id, closing ? 0.0f : 1.0f, seconds, ease_in);
}

namespace anim_detail {
struct Arrivals {
  std::unordered_set<ImGuiID> known;
  std::unordered_map<ImGuiID, double> starts;
  bool primed = false;
  bool fill = false;  // the list was empty and has just been filled: its first items arrive in a stagger (C8)
  int frame = 0;
  ImGuiContext* context = nullptr;  // whose list this is: a window's map entries go with its context (B22)
};
inline std::unordered_map<ImGuiID, Arrivals>& arrival_scopes() {
  static std::unordered_map<ImGuiID, Arrivals> scopes;
  return scopes;
}
}  // namespace anim_detail

// A window's ImGui context has gone: everything the motion maps hold for it goes too. They are keyed by its address,
// which the next window may be given, so a surviving entry would hand the new window a tween that never arrives.
// Safe with nullptr (no entry belongs to it).
inline void anim_purge_context(ImGuiContext* context) {
  if (!context) return;
  const uint64_t owner = anim_detail::context_key(context);
  const auto drop = [owner](auto& slots) {
    for (auto it = slots.begin(); it != slots.end();)
      it = anim_detail::key_belongs(it->first, owner) ? slots.erase(it) : std::next(it);
  };
  drop(anim_detail::appearing());
  drop(anim_detail::tweens());
  auto& scopes = anim_detail::arrival_scopes();
  for (auto it = scopes.begin(); it != scopes.end();) it = it->second.context == context ? scopes.erase(it) : std::next(it);
}

// Tells a list which of its items are new. Call once per frame with the keys of the WHOLE list (not the rows a clipper draws), then
// ask `arrival` for each row drawn. The first call after a window is made has no new items (what is there was there), and an item
// that scrolls into view or comes back after a visit to another page is not new: only a key the list did not have last time is.
inline void arrivals_update(ImGuiID scope, const std::vector<ImGuiID>& keys) {
  anim_detail::Arrivals& a = anim_detail::arrival_scopes()[scope];
  ImGuiContext* context = ImGui::GetCurrentContext();
  const int frame = ImGui::GetFrameCount();
  // Another context, or this one made again at the old address: start over.
  if (a.context != context || frame < a.frame) a = anim_detail::Arrivals();
  a.context = context;
  a.frame = frame;
  const double now = anim_detail::wall_seconds();
  std::unordered_set<ImGuiID> present(keys.begin(), keys.end());
  if (a.primed) {
    if (a.known.empty() && !keys.empty()) a.fill = true;
    else if (!a.known.empty()) a.fill = false;
    for (ImGuiID key : keys)
      if (!a.known.count(key)) a.starts[key] = now;
  }
  a.known = std::move(present);
  a.primed = true;
  for (auto it = a.starts.begin(); it != a.starts.end();) it = now - it->second > 2.0 ? a.starts.erase(it) : std::next(it);
}

// 0..1, eased: how far a new item has come. 1 for every item that is not new.
inline bool arrivals_filling(ImGuiID scope) {
  const auto it = anim_detail::arrival_scopes().find(scope);
  return it != anim_detail::arrival_scopes().end() && it->second.fill;
}

inline float arrival(ImGuiID scope, ImGuiID key, float seconds = kDurBase, float delay = 0.0f) {
  if (!anim_detail::enabled) return 1.0f;
  const auto scopes = anim_detail::arrival_scopes().find(scope);
  if (scopes == anim_detail::arrival_scopes().end()) return 1.0f;
  const auto start = scopes->second.starts.find(key);
  if (start == scopes->second.starts.end()) return 1.0f;
  const float t = progress(anim_detail::wall_seconds() - start->second - delay, seconds);
  if (t < 1.0f) anim_detail::moved = true;
  return ease_out(t);
}

// A thumbnail's fade-in from its surface. The first frame that draws it starts the fade (its texture is there by then), and it eases
// out over `seconds`. One start is kept per texture and never dropped: a texture that comes back under a new handle fades in again.
inline float thumb_fade(uint64_t texture, float seconds = kDurBase) {
  if (!anim_detail::enabled) return 1.0f;
  static std::unordered_map<uint64_t, double> starts;
  const double now = anim_detail::wall_seconds();
  // Bounded: an entry that finished its fade is only needed to say "already shown", so past a few hundred the finished ones go (a
  // texture that comes back after that fades in again, which is also right for a handle the device reused).
  if (starts.size() > 512) {
    for (auto it = starts.begin(); it != starts.end();) it = now - it->second > 1.0 ? starts.erase(it) : std::next(it);
  }
  const double start = starts.emplace(texture, now).first->second;
  const float t = progress(now - start, seconds);
  if (t < 1.0f) anim_detail::moved = true;
  return ease_out(t);
}

// Moves everything a frame drew by (dx, dy) pixels: its vertices and clip rectangles. A view rises or slides without its content
// being laid out again (V1: the window opens 8 px low and rises). The draw data is the frame's own, so nothing is kept.
inline void shift_draw_data(ImDrawData* data, float dx, float dy) {
  if (!data || (dx == 0.0f && dy == 0.0f)) return;
  for (int n = 0; n < data->CmdListsCount; ++n) {
    ImDrawList* list = data->CmdLists[n];
    for (int i = 0; i < list->VtxBuffer.Size; ++i) {
      list->VtxBuffer.Data[i].pos.x += dx;
      list->VtxBuffer.Data[i].pos.y += dy;
    }
    for (int i = 0; i < list->CmdBuffer.Size; ++i) {
      ImDrawCmd& cmd = list->CmdBuffer.Data[i];
      cmd.ClipRect.x += dx;
      cmd.ClipRect.z += dx;
      cmd.ClipRect.y += dy;
      cmd.ClipRect.w += dy;
    }
  }
}

// The same for what one draw list received after `first_vertex`: a part of a view slides (a section, a card, a dialog body) while its
// clip stays where it was. The offset is small, so the part is never clipped by more than it would be at rest.
inline void shift_since(ImDrawList* draw, int first_vertex, float dx, float dy) {
  if (dx == 0.0f && dy == 0.0f) return;
  for (int i = std::max(0, first_vertex); i < draw->VtxBuffer.Size; ++i) {
    draw->VtxBuffer.Data[i].pos.x += dx;
    draw->VtxBuffer.Data[i].pos.y += dy;
  }
}

// Turns whatever one draw list received after `first_vertex` by `angle` radians (clockwise on screen) around `center`: a chevron that
// rotates as a list opens (A6). Icons are stroked paths, so their vertices rotate like any others.
inline void rotate_since(ImDrawList* draw, int first_vertex, ImVec2 center, float angle) {
  if (angle == 0.0f) return;
  const float s = std::sin(angle), c = std::cos(angle);
  for (int i = std::max(0, first_vertex); i < draw->VtxBuffer.Size; ++i) {
    ImVec2& p = draw->VtxBuffer.Data[i].pos;
    const float dx = p.x - center.x, dy = p.y - center.y;
    p = ImVec2(center.x + dx * c - dy * s, center.y + dx * s + dy * c);
  }
}

// Scales whatever one draw list received after first_vertex about center (a key cap that pops as it joins a chord, V8).
inline void scale_since(ImDrawList* draw, int first_vertex, ImVec2 center, float scale) {
  if (scale == 1.0f) return;
  for (int i = std::max(0, first_vertex); i < draw->VtxBuffer.Size; ++i) {
    ImVec2& p = draw->VtxBuffer.Data[i].pos;
    p = ImVec2(center.x + (p.x - center.x) * scale, center.y + (p.y - center.y) * scale);
  }
}

// A tab's incoming content appears immediately, then settles over the base motion duration.
inline float tab_arrival(const char* id, int tab, int& direction) {
  ImGui::PushID(id);
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID previous_id = ImGui::GetID("previous");
  const ImGuiID start_id = ImGui::GetID("start");
  const ImGuiID direction_id = ImGui::GetID("direction");
  const int previous = storage->GetInt(previous_id, -1);
  const float now = static_cast<float>(anim_detail::wall_seconds());
  if (previous < 0) storage->SetFloat(start_id, now - kDurBase);
  else if (previous != tab) {
    storage->SetFloat(start_id, now);
    storage->SetInt(direction_id, tab > previous ? 1 : -1);
  }
  storage->SetInt(previous_id, tab);
  direction = storage->GetInt(direction_id, 1);
  const float t = anim_detail::enabled ? progress(now - storage->GetFloat(start_id, now), kDurBase) : 1.0f;
  if (t < 1.0f) anim_touch();
  ImGui::PopID();
  return ease_out(t);
}

// The ground fading away over a view that is appearing: everything under it (drawn with plain colours, which a
// style alpha would not reach) comes in from the ground. t = 1 draws nothing.
inline void fade_in_from(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 ground, float t) {
  if (t >= 1.0f) return;
  const ImU32 alpha = static_cast<ImU32>(std::clamp((1.0f - t) * 255.0f, 0.0f, 255.0f));
  draw->AddRectFilled(min, max, (ground & 0x00FFFFFFu) | (alpha << 24));
}

// Whatever one draw list received after `first_vertex` (take it from `draw->VtxBuffer.Size` before drawing), at `alpha`.
inline void fade_since(ImDrawList* draw, int first_vertex, float alpha) {
  if (alpha >= 1.0f) return;
  for (int i = std::max(0, first_vertex); i < draw->VtxBuffer.Size; ++i) {
    ImDrawVert& vertex = draw->VtxBuffer[i];
    const ImU32 a = static_cast<ImU32>(static_cast<float>(vertex.col >> 24) * std::clamp(alpha, 0.0f, 1.0f));
    vertex.col = (vertex.col & 0x00FFFFFFu) | (a << 24);
  }
}

// Wheel scrolling that glides: call right after BeginChild of a list that has ImGuiWindowFlags_NoScrollWithMouse (the wheel
// is read here instead). `step` is the distance of one wheel notch in pixels. A scroll set by anything else (the scroll bar, a
// key, scroll_to) is followed, not fought. `wheel` false: a list inside this one has the wheel, and this one only glides.
inline void smooth_scroll(float step, bool wheel = true, float rate = kRateScroll) {
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const float current = ImGui::GetScrollY();
  const ImGuiID target_id = ImGui::GetID("##glide_target");
  const ImGuiID last_id = ImGui::GetID("##glide_last");
  const ImGuiID position_id = ImGui::GetID("##glide_position");
  float target = storage->GetFloat(target_id, current);
  const float last = storage->GetFloat(last_id, current);
  float position = storage->GetFloat(position_id, current);
  if (std::fabs(current - last) > 0.5f) target = position = current;  // moved by someone else
  const ImGuiIO& io = ImGui::GetIO();
  bool precise = false;
  if (wheel && io.MouseWheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
    target = std::clamp(target - io.MouseWheel * step, 0.0f, ImGui::GetScrollMaxY());
    // J1: a precision touchpad reports small deltas (a notch is 1.0): those move the list 1:1, with no glide on top of the finger's own.
    precise = std::fabs(io.MouseWheel) < 0.99f;
  }
  // The list may have got shorter since the target was set: a target past its end is one it can never reach.
  target = std::min(target, ImGui::GetScrollMaxY());
  position = anim_detail::enabled && !precise ? anim_step(position, target, rate, anim_detail::dt()) : target;
  if (std::fabs(target - position) < 0.5f) position = target;
  const float next = std::round(position);
  if (next != current) ImGui::SetScrollY(next);
  if (position != target) anim_detail::moved = true;
  storage->SetFloat(target_id, target);
  storage->SetFloat(position_id, position);
  storage->SetFloat(last_id, next);  // what the list will read back: whole pixels
}

// Where a list should glide to (a card the arrows moved to).
inline void scroll_to_glide(float y) {
  *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("##glide_target"), y) = std::clamp(y, 0.0f, ImGui::GetScrollMaxY());
  anim_detail::moved = true;
}

}  // namespace bx::ui
