// The deck's lower half: a compact live timeline of the Timeline session being written - its filmstrip to scrub, a range to
// choose and save as a clip - and the recent actions under it. Every action is the protocol's own command (timeline.get,
// timeline.thumbs, timeline.save, timeline.preview, timeline.open). Nothing is read while the panel is hidden, the session is
// read every 2 s while it shows, and only a slot that came into view asks for a picture (ui_live_math.h).
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iterator>
#include <set>

#include "ui_parts.h"
#include "ui_window_math.h"

namespace bx::ui {

namespace {

using namespace parts;

constexpr int64_t kShortest = 1000;  // the shortest range Save keeps
constexpr int64_t kJoin = 1000;      // chunks this close are one stretch of footage; a wider hole shows as a gap
// How many times a moment is asked for before its cell is left empty. A reply that made no picture may
// give an attempt back, but a moment no reply ever makes (a damaged chunk) must not be asked for for ever.
constexpr int kThumbTries = 3;

// How often a moment was asked for, without adding it to the map: reading a slot that has no picture must not grow the
// strip's state for the whole session (B19).
int asked_count(const LiveStrip& strip, int64_t moment) {
  const auto found = strip.asked.find(moment);
  return found == strip.asked.end() ? 0 : found->second;
}

// Forgets the pictures far from what is on screen: the view, the range being saved and one screen either side stay. A
// session an hour long would otherwise keep every slot it ever showed, and an asked count for every one of them (B19).
void prune_thumbs(LiveStrip& strip, const LiveRange& view) {
  const int64_t margin = view.end_ms - view.start_ms;
  const int64_t lo = std::min(view.start_ms, strip.range.start_ms) - margin;
  const int64_t hi = std::max(view.end_ms, strip.range.end_ms) + margin;
  for (auto it = strip.thumbs.begin(); it != strip.thumbs.end();)
    it = it->first < lo || it->first > hi ? strip.thumbs.erase(it) : std::next(it);
  for (auto it = strip.asked.begin(); it != strip.asked.end();)
    it = it->first < lo || it->first > hi ? strip.asked.erase(it) : std::next(it);
  for (auto it = strip.missed.begin(); it != strip.missed.end();)
    it = it->first < lo || it->first > hi ? strip.missed.erase(it) : std::next(it);
}

int64_t unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// "4:17": the session's own clock, as the Timeline page and the editor say it.
std::string at_label(int64_t ms) { return format_duration(static_cast<double>(std::max<int64_t>(0, ms) / 1000)); }

void session_command(ViewEnv& env, const char* command, const char* failure) {
  Json args = Json::object();
  args.set("session", Json(env.panel.live.session));
  env.store.attempt(command, args, failure);
}

// One timeline.get. The list (every session's index) is read for the first read, after the writer's state changed and every
// eighth time; otherwise only the session shown is read.
void read_strip(ViewEnv& env) {
  LiveStrip& strip = env.panel.live;
  const bool listed = strip.list_next || strip.session.empty() || strip.reads % 8 == 0;
  Json args = Json::object();
  if (!listed) {
    args.set("session", Json(strip.session));
    args.set("list", Json(false));
  }
  ++strip.reads;
  strip.list_next = false;
  strip.reading = true;
  // The panel's state, never `env`: the reply runs frames later.
  env.store.bridge_call("timeline.get", args, [&strip, listed](bool ok, const Json& data, const std::string&) {
    strip.reading = false;
    strip.read_due = GetTickCount() + (ok ? 2000 : 4000);
    if (!ok) {
      strip.list_next = true;  // the session was erased, most likely: find the next one
      return;
    }
    const std::string id = data["session"].as_string();
    if (id != strip.session) {
      // Another session: its pictures, range and view are its own.
      strip.session = id;
      strip.started_at = 0;
      strip.thumbs.clear();
      strip.asked.clear();
      strip.missed.clear();
      strip.thumbs_retry = 0;
      strip.range_set = false;
      strip.follow = true;
      strip.view_follow = true;
      strip.drag = 0;
    }
    if (listed) {
      for (const Json& session : data["sessions"].items()) {
        if (session["id"].as_string() == id) strip.started_at = session["started_at"].as_int();
      }
    }
    const std::string current = data["current_session"].as_string();
    strip.state = data["state"].as_string();
    strip.live = !id.empty() && current == id;
    if (!current.empty() && current != id) {
      // A new session began: show it now, not at the next read, so the one that stopped is not on screen inside it.
      strip.list_next = true;
      strip.read_due = GetTickCount();
    }
    strip.lo = data["available_start_ms"].as_int();
    strip.hi = data["available_end_ms"].as_int();
    strip.spans.clear();
    for (const Json& chunk : data["chunks"].items()) {
      const int64_t start = chunk["start_ms"].as_int(), end = chunk["end_ms"].as_int();
      if (end <= start) continue;
      if (!strip.spans.empty() && start - strip.spans.back().end_ms <= kJoin) strip.spans.back().end_ms = std::max(strip.spans.back().end_ms, end);
      else strip.spans.push_back({start, end});
    }
  });
}

// The pictures the strip is missing, at most 24 a call and one call at a time (the engine draws six new ones a call).
void ask_thumbs(ViewEnv& env, const std::vector<int64_t>& want) {
  LiveStrip& strip = env.panel.live;
  if (strip.thumbs_reading || strip.saving || want.empty()) return;
  // 0 is "nothing pending": the signed compare alone would read a machine whose uptime is over 24.8 days
  // (GetTickCount past 2^31) as always waiting, and the strip would never ask for a picture at all.
  if (strip.thumbs_retry != 0 && static_cast<int32_t>(GetTickCount() - strip.thumbs_retry) < 0) return;
  Json times = Json::array();
  std::vector<int64_t> requested;
  int count = 0;
  for (const int64_t t : want) {
    if (count == 24) break;
    times.push(Json(t));
    requested.push_back(t);
    ++strip.asked[t];
    ++count;
  }
  Json args = Json::object();
  args.set("session", Json(strip.session));
  args.set("times_ms", times);
  strip.thumbs_reading = true;
  const std::string session = strip.session;
  env.store.bridge_call("timeline.thumbs", args, [&strip, session, requested](bool ok, const Json& data, const std::string&) {
    strip.thumbs_reading = false;
    if (!ok || session != strip.session) return;
    std::set<int64_t> made;
    for (const Json& thumb : data["thumbs"].items()) {
      const int64_t at = thumb["at_ms"].as_int();
      strip.thumbs[at] = thumb["file"].as_string();
      made.insert(at);
    }
    // An empty reply is the media lock busy (or the one moment a damaged chunk cannot make): the
    // two-second backoff below bounds that, and its attempts are never spent, so the strip recovers
    // after a save. A partial reply is the engine's six-a-call fill: its leftovers are refunded three
    // times, so a slot no reply ever makes stops being asked for.
    const bool whole_reply_empty = made.empty();
    for (const int64_t at : requested) {
      if (made.count(at)) {
        strip.missed.erase(at);
        continue;
      }
      auto asked = strip.asked.find(at);
      if (asked == strip.asked.end() || asked->second <= 0) continue;
      if (whole_reply_empty || ++strip.missed[at] <= kThumbTries) --asked->second;
    }
    strip.thumbs_retry = made.empty() ? GetTickCount() + 2000 : 0;
  });
}

void save_range(ViewEnv& env) {
  LiveStrip& strip = env.panel.live;
  if (strip.saving || strip.session.empty()) return;
  strip.saving = true;
  Json args = Json::object();
  args.set("session", Json(strip.session));
  args.set("start_ms", Json(strip.range.start_ms));
  args.set("end_ms", Json(strip.range.end_ms));
  const double seconds = static_cast<double>(strip.range.end_ms - strip.range.start_ms) / 1000.0;
  UiStore& store = env.store;
  // A range that reaches the live edge waits for the open chunk to close (up to 10 s): the spinner says so.
  store.bridge_call("timeline.save", args, [&strip, &store, seconds](bool ok, const Json&, const std::string& error) {
    strip.saving = false;
    if (!ok) {
      store.toast(UiToast::error, "Clip not saved", error);
      return;
    }
    store.toast(UiToast::success, "Clip saved", clip_saved_detail(std::string(), format_duration(seconds)));
    store.refresh_library(0);
  });
}

// A framed square for the strip's tools: an icon, or a glyph ("[", "]") when there is no icon for it.
bool tool(const char* id, const char* icon, const char* glyph, const std::string& tip, bool disabled = false) {
  const float side = px(24);
  const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + side, a.y + side);
  ImGui::BeginDisabled(disabled);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(side, side));
  const bool hovered = ImGui::IsItemHovered() && !disabled;
  ImGui::EndDisabled();
  const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  tooltip(tip);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (hover > 0.0f) draw->AddRectFilled(a, b, with_alpha(pal().raised, hover));
  draw->AddRect(a, b, mix(pal().line, pal().accent, hover), 0.0f, 0, 1.0f);
  const ImU32 fg = disabled ? pal().faint : mix(pal().muted, pal().ink, hover);
  if (icon) {
    const bool solid = std::string(icon) == "play" || std::string(icon) == "pause";  // drawn filled, like the transport's
    draw_icon(draw, icon, ImVec2(a.x + (side - px(13)) / 2, a.y + (side - px(13)) / 2), px(13), fg, 1.7f, solid);
  } else {
    ImFont* font = fonts().strong;
    draw->AddText(font, font->LegacySize, ImVec2(a.x + (side - text_width(font, glyph)) / 2, a.y + (side - font->LegacySize) / 2), fg, glyph);
  }
  return pressed && !disabled;
}

// The filled Save chip: the strip's one primary action. A spinner while the save runs.
bool save_chip(float width, bool busy) {
  const float h = px(24);
  const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + h);
  ImGui::BeginDisabled(busy);
  const bool pressed = ImGui::InvisibleButton("##livesave", ImVec2(width, h));
  const bool hovered = ImGui::IsItemHovered() && !busy;
  ImGui::EndDisabled();
  const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  tooltip("Save the chosen range as a clip");
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(a, b, mix(pal().accent, mix(pal().accent, pal().ink, 0.22f), hover));
  const float icon = px(13);
  // B9: while it works a light sweeps the chip's foot; when it is done the icon pops as a check for a moment.
  static bool was_busy = false;
  static double saved_at = -10.0;
  const double now_s = anim_detail::wall_seconds();
  if (was_busy && !busy) saved_at = now_s;
  was_busy = busy;
  const bool just_saved = !busy && now_s - saved_at < 0.9;
  if (busy) {
    const float t = static_cast<float>(std::fmod(now_s * 1.4, 1.0));
    const float bar = width * 0.35f;
    draw->PushClipRect(a, b, true);
    draw->AddRectFilled(ImVec2(a.x - bar + (width + bar) * t, b.y - px(2)), ImVec2(a.x + (width + bar) * t, b.y), with_alpha(pal().on_accent, 0.8f));
    draw->PopClipRect();
    anim_touch_slow();
  }
  if (busy) {
    spinner(draw, ImVec2(a.x + px(8) + icon / 2, a.y + h / 2), px(5), pal().on_accent);
  } else if (just_saved) {
    const float pop = ease_out(progress(now_s - saved_at, kDurBase));
    const int v0 = draw->VtxBuffer.Size;
    draw_icon(draw, "check", ImVec2(a.x + px(8), a.y + (h - icon) / 2), icon, pal().on_accent, 1.7f);
    scale_since(draw, v0, ImVec2(a.x + px(8) + icon / 2, a.y + h / 2), 0.6f + 0.4f * pop);
    anim_touch_slow();
  } else {
    draw_icon(draw, "save", ImVec2(a.x + px(8), a.y + (h - icon) / 2), icon, pal().on_accent, 1.7f);
  }
  draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(a.x + px(8) + icon + px(6), a.y + (h - fonts().strong->LegacySize) / 2),
                pal().on_accent, just_saved ? "saved" : "save");
  return pressed && !busy;
}

// The moment a time on the strip shows a picture of: inside the published stretch that holds it, or -1 in a gap.
int64_t picture_moment(const LiveStrip& strip, int64_t t) {
  for (const LiveRange& span : strip.spans) {
    if (t >= span.start_ms && t < span.end_ms) return t;
  }
  return -1;
}

}  // namespace

void draw_live_timeline(ViewEnv& env) {
  UiStore& store = env.store;
  LiveStrip& strip = env.panel.live;
  strip.tick_ms = 0;
  strip.drawn_at = env.now_ms;
  const bool enabled = store.config["timeline_minutes"].as_int(0) > 0;
  const std::string state = store.status["timeline"]["state"].as_string("off");
  if (enabled && store.connected) {
    if (state != strip.seen) {
      strip.seen = state;
      strip.list_next = true;
      strip.read_due = env.now_ms;
    }
    if (!strip.reading && static_cast<int32_t>(env.now_ms - strip.read_due) >= 0) read_strip(env);
  }
  const bool writing = strip.live && state == "writing";
  const int64_t edge = writing && strip.started_at > 0 ? live_edge_ms(unix_ms(), strip.started_at, strip.hi) : strip.hi;
  const bool footage = enabled && !strip.session.empty() && edge - strip.lo >= kShortest;
  // The range: the last quick clip's length at first, its end riding the live edge until it is moved. Kept before the pane
  // opens, so the pane's title can say it.
  if (footage) {
    if (!strip.range_set) {
      strip.want = store.config["quick_clip_seconds"].as_int(20) * 1000;
      strip.range_set = true;
      strip.follow = true;
    }
    // Riding the live edge, the range keeps the chosen length, or all the footage while there is less of it.
    if (strip.follow && strip.drag == 0) strip.range = {edge - strip.want, edge};
    strip.range = clamp_range(strip.range.start_ms, strip.range.end_ms, strip.lo, edge, kShortest);
  }
  std::string aside;
  if (footage) {
    aside = at_label(strip.range.start_ms) + " - " + at_label(strip.range.end_ms) + " (" +
            format_span(static_cast<double>(strip.range.end_ms - strip.range.start_ms) / 1000.0) + ")";
    if (!writing) aside += strip.live ? " paused" : " ended";
  }
  if (state == "paused_no_game") aside = "waiting for a game";
  if (state == "paused_idle") aside = "idle";
  const float row_h = px(24), strip_h = px(46), ruler_h = px(18), gap = px(8);
  // One row holds the length chips and, while there is footage, the strip's tools; the strip opens under it when the
  // Timeline is on and folds away when it is off, the pane's height following and what it reveals fading in with it.
  const float open = anim_to(ImGui::GetID("##livestrip_open"), enabled ? 1.0f : 0.0f, kRateLayout);
  const float body_h = gap + strip_h + ruler_h;
  begin_pane("livestrip", "timeline", ImVec2(-1, std::round(row_h + body_h * open) + px(26)), false, aside);
  const float w = ImGui::GetContentRegionAvail().x;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // How long the Timeline keeps footage on the disk, in the buffer limit's own words (OFF, 30M, 1H, 2H).
  static const std::vector<std::string> lengths = {"off", "30m", "1h", "2h"};
  int chosen = timeline_length_index(static_cast<int>(store.config["timeline_minutes"].as_int(0)));
  if (segmented("##timelinelength", lengths, chosen, 38.0f, 24, fonts().small)) {
    Json patch = Json::object();
    patch.set("timeline_minutes", Json(timeline_length_minutes(chosen)));
    store.set_config(patch);
  }
  tooltip(timeline_hover(), 220);
  const float chips_end = ImGui::GetItemRectMax().x;
  if (!footage) {
    // What the strip would show, at the row's right.
    const std::string note = !enabled ? "nothing kept" : state == "paused_no_game" ? "waiting for a game" : state == "paused_idle" ? "idle" : !store.buffer_on() && strip.session.empty() ? "start the buffer" : "waiting for footage";
    const float note_w = text_width(fonts().small, note);
    if (note != aside && origin.x + w - note_w > chips_end + px(12)) {
      draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(origin.x + w - note_w, origin.y + (row_h - fonts().small->LegacySize) / 2),
                    pal().muted, note.c_str());
    }
  }
  if (open < 0.01f) {
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + row_h));
    end_pane();
    return;
  }
  const int body_v = draw->VtxBuffer.Size;
  const ImVec2 s0(origin.x, origin.y + row_h + gap), s1(origin.x + w, s0.y + strip_h);

  if (!footage) {
    // An empty filmstrip in the strip's place while the first seconds arrive.
    draw->AddRectFilled(s0, s1, pal().canvas);
    const float tile = strip_h * 16.0f / 9.0f;
    for (float x = s0.x + tile; x < s1.x; x += tile) draw->AddLine(ImVec2(x, s0.y), ImVec2(x, s1.y), pal().line, 1.0f);
    draw->AddRect(s0, s1, pal().line, 0.0f, 0, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, s1.y + ruler_h));
    fade_since(draw, body_v, open);
    end_pane();
    return;
  }

  if (strip.view_follow) strip.view_end = edge;
  // B8: the span on screen eases to its goal (fast token); a wheel zoom keeps the moment under the pointer where it is while it does.
  if (strip.shown_span <= 0.0) strip.shown_span = static_cast<double>(strip.span);
  {
    const double goal = static_cast<double>(strip.span);
    if (strip.shown_span != goal) {
      if (!anim_enabled()) {
        strip.shown_span = goal;
      } else {
        const double k = 1.0 - std::exp(-static_cast<double>(kRateHover) * std::min<double>(ImGui::GetIO().DeltaTime, 0.05));
        strip.shown_span += (goal - strip.shown_span) * k;
        if (std::fabs(goal - strip.shown_span) < std::max(2.0, goal * 0.002)) strip.shown_span = goal;
        anim_touch();
      }
    }
  }
  const int64_t shown = static_cast<int64_t>(std::llround(strip.shown_span));
  if (strip.zoom_frac >= 0.0) {
    if (strip.shown_span == static_cast<double>(strip.span)) {
      strip.view_end = strip.zoom_t + static_cast<int64_t>(strip.zoom_frac * static_cast<double>(strip.span));
      strip.view_follow = strip.view_end >= edge;
      strip.zoom_frac = -1.0;
      if (strip.view_follow) strip.view_end = edge;
    } else {
      strip.view_end = strip.zoom_t + static_cast<int64_t>(strip.zoom_frac * static_cast<double>(shown));
    }
  }
  LiveRange view = view_window(strip.view_end, shown, strip.lo, edge);
  // The pictures far from the view go, once each time the view moves (B19).
  if (view.start_ms != strip.pruned_start || view.end_ms != strip.pruned_end) {
    strip.pruned_start = view.start_ms;
    strip.pruned_end = view.end_ms;
    prune_thumbs(strip, view);
  }
  double per_px = static_cast<double>(view.end_ms - view.start_ms) / std::max(1.0f, w);
  const auto x_of = [&](int64_t t) { return origin.x + static_cast<float>(static_cast<double>(t - view.start_ms) / per_px); };
  const auto t_of = [&](float x) { return view.start_ms + static_cast<int64_t>(std::llround((x - origin.x) * per_px)); };
  const auto pin = [&](int64_t end) { return end >= edge - 250; };  // a range ending at the live edge keeps riding it

  // ---- the tools, right-aligned on the chips' row: pause / live, zoom out and in, the full Timeline, Save
  const float save_w = px(8) + px(13) + px(6) + text_width(fonts().strong, "save") + px(10);
  const float tool_step = px(24) + px(4);
  // A narrow deck gives up the zoom buttons first (the wheel still zooms).
  const bool zoom_tools = origin.x + w - save_w - px(6) - tool_step * 4 >= chips_end + px(8);
  float x = origin.x + w - save_w - px(6) - tool_step * (zoom_tools ? 4 : 2);
  const auto place = [&] {
    ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
    x += tool_step;
  };
  // Live: the view and the range ride the live edge. Pause holds both still to pick from; play goes back to live.
  const bool riding = strip.view_follow && strip.follow;
  place();
  if (tool("##livepause", riding ? "pause" : "play", nullptr, riding ? "Pause: hold the strip still" : "Back to live")) {
    strip.view_follow = strip.follow = !riding;
    strip.view_end = edge;
  }
  const auto zoom = [&](int direction) {
    const int64_t before = strip.span;
    strip.span = zoom_span(strip.span, direction, edge - strip.lo);
    if (strip.span == before) return;
    // In on the range, out from where the view ends.
    strip.view_end = direction < 0 ? (strip.range.start_ms + strip.range.end_ms) / 2 + strip.span / 2 : view.end_ms;
    strip.view_follow = strip.view_end >= edge;
  };
  if (zoom_tools) {
    place();
    if (tool("##livezoomout", "minus", nullptr, "Show more footage (the wheel zooms too)")) zoom(1);
    place();
    if (tool("##livezoomin", "plus", nullptr, "Zoom in on the range")) zoom(-1);
  }
  place();
  if (tool("##liveopen", "expand", nullptr, "Open the full Timeline on this footage")) session_command(env, "timeline.open", "The Timeline did not open");
  ImGui::SetCursorScreenPos(ImVec2(origin.x + w - save_w, origin.y));
  if (save_chip(save_w, strip.saving)) save_range(env);

  // ---- the strip: input first, so this frame draws the range where the pointer put it
  ImGui::SetCursorScreenPos(s0);
  ImGui::InvisibleButton("##livestrip", ImVec2(w, strip_h + ruler_h));
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  if (hovered) strip.wheel = true;
  const ImGuiIO& io = ImGui::GetIO();
  const float mx = std::clamp(io.MousePos.x, s0.x, s1.x);
  const int64_t mt = std::clamp(t_of(mx), strip.lo, edge);
  const float grab = px(7);
  float xs = x_of(strip.range.start_ms), xe = x_of(strip.range.end_ms);
  if (ImGui::IsItemActivated()) {
    strip.dragged = false;
    strip.drag_at = mt;
    strip.drag_from = strip.range;
    strip.drag = std::fabs(mx - xs) <= grab ? 1 : std::fabs(mx - xe) <= grab ? 2 : mx > xs && mx < xe ? 3 : 4;
  }
  if (active && strip.drag != 0) {
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, px(3))) strip.dragged = true;
    if (strip.dragged) {
      if (strip.drag == 1) {
        strip.range = move_edge(strip.range, true, mt, strip.lo, edge, kShortest);
      } else {
        strip.range = strip.drag == 2 ? move_edge(strip.drag_from, false, mt, strip.lo, edge, kShortest)
                      : strip.drag == 3 ? shift_range(strip.drag_from, mt - strip.drag_at, strip.lo, edge)
                                        : clamp_range(strip.drag_at, mt, strip.lo, edge, kShortest);
        strip.follow = pin(strip.range.end_ms);
      }
      strip.want = strip.range.end_ms - strip.range.start_ms;
    }
  }
  if (ImGui::IsItemDeactivated()) {
    strip.drag = 0;
  } else if (!active) {
    strip.drag = 0;  // a press the strip never saw end (the window hid under it) does not hold the range still
  }
  // The wheel zooms around the pointer; Shift+wheel, or a sideways wheel, moves along the footage.
  if (hovered && !active) {
    const float pan = io.KeyShift ? io.MouseWheel : -io.MouseWheelH;
    if (pan != 0.0f) {
      strip.view_end = view.end_ms - static_cast<int64_t>(pan * static_cast<float>(strip.span) / 5.0f);
      strip.view_follow = strip.view_end >= edge;
    } else if (io.MouseWheel != 0.0f) {
      const int64_t before = strip.span;
      strip.span = zoom_span(strip.span, io.MouseWheel < 0 ? 1 : -1, edge - strip.lo);
      if (strip.span != before) {
        const double right = static_cast<double>(s1.x - mx) / std::max(1.0f, w);
        strip.view_end = mt + static_cast<int64_t>(right * static_cast<double>(strip.span));
        strip.view_follow = strip.view_end >= edge;
        strip.zoom_t = mt;
        strip.zoom_frac = right;
      }
    }
    if (strip.view_follow) strip.view_end = edge;
    view = view_window(strip.view_end, shown, strip.lo, edge);
    per_px = static_cast<double>(view.end_ms - view.start_ms) / std::max(1.0f, w);
  }
  xs = x_of(strip.range.start_ms);
  xe = x_of(strip.range.end_ms);

  // ---- the filmstrip: one picture a slot, from the slot's middle, on the grid the session's start anchors
  std::vector<int64_t> want;
  draw->AddRectFilled(s0, s1, pal().canvas);
  draw->PushClipRect(s0, s1, true);
  const float tile_w = strip_h * 16.0f / 9.0f;
  const int64_t tile_ms = nice_step_ms(static_cast<int64_t>(std::ceil(static_cast<double>(view.end_ms - view.start_ms) / std::max(1.0f, w / tile_w))));
  for (const int64_t slot : strip_slots(view.start_ms, view.end_ms, tile_ms)) {
    const float a = x_of(slot), b = x_of(slot + tile_ms);
    for (const LiveRange& span : strip.spans) {
      const int64_t from = std::max(slot, span.start_ms), to = std::min(slot + tile_ms, span.end_ms);
      if (to <= from) continue;
      const int64_t moment = std::clamp(slot + tile_ms / 2, span.start_ms, span.end_ms - 1);
      draw->PushClipRect(ImVec2(std::max(s0.x, x_of(from)), s0.y), ImVec2(std::min(s1.x, x_of(to)), s1.y), true);
      const auto found = strip.thumbs.find(moment);
      if (found != strip.thumbs.end()) {
        picture(env, found->second, "film", ImVec2(a, s0.y), ImVec2(b - a, strip_h), 0.0f);
      } else {
        draw->AddRectFilled(ImVec2(a, s0.y), ImVec2(b, s1.y), pal().raised);
        if (asked_count(strip, moment) < kThumbTries) want.push_back(moment);
      }
      draw->PopClipRect();
    }
    draw->AddLine(ImVec2(a, s0.y), ImVec2(a, s1.y), pal().canvas, std::max(1.0f, px(1)));  // the frames' seams
  }
  // What the writer still holds: footage, not yet a picture. Light stripes up to the live edge.
  if (edge > strip.hi) {
    const float a = std::max(s0.x, x_of(strip.hi)), b = std::min(s1.x, x_of(edge));
    draw->AddRectFilled(ImVec2(a, s0.y), ImVec2(b, s1.y), pal().raised);
    draw->PushClipRect(ImVec2(a, s0.y), ImVec2(b, s1.y), true);
    for (float sx = a - strip_h; sx < b; sx += px(8)) {
      draw->AddLine(ImVec2(sx, s1.y), ImVec2(sx + strip_h, s0.y), with_alpha(pal().accent, 0.18f), px(2));
    }
    draw->PopClipRect();
  }
  // Outside the range is dimmed; the range wears the accent frame with a handle at each end.
  const ImU32 dim = with_alpha(pal().canvas, 0.62f);
  if (xs > s0.x) draw->AddRectFilled(s0, ImVec2(xs, s1.y), dim);
  if (xe < s1.x) draw->AddRectFilled(ImVec2(xe, s0.y), s1, dim);
  const float frame = px(2);
  draw->AddRect(ImVec2(xs, s0.y + frame / 2), ImVec2(xe, s1.y - frame / 2), pal().accent, 0.0f, 0, frame);
  for (int side = 0; side < 2; ++side) {
    const bool lit = (strip.drag == side + 1) || (hovered && !active && std::fabs(mx - (side == 0 ? xs : xe)) <= grab);
    // B7: a handle thickens and its fill brightens while it is grabbed or under the pointer (the fast token).
    const float lit_t = anim_to(ImGui::GetID(side == 0 ? "##handle_start" : "##handle_end"), lit ? 1.0f : 0.0f, kRateHover);
    const float handle = px(7) + px(2) * lit_t;
    const float hx = side == 0 ? xs : xe - handle;
    draw->AddRectFilled(ImVec2(hx, s0.y), ImVec2(hx + handle, s1.y), mix(pal().accent, pal().ink, 0.25f * lit_t));
    const float cx = hx + handle / 2, cy = (s0.y + s1.y) / 2;
    draw->AddLine(ImVec2(cx, cy - px(7)), ImVec2(cx, cy + px(7)), pal().on_accent, std::max(1.0f, px(1)));
  }
  // Where the pointer is.
  if (hovered && !active) draw->AddLine(ImVec2(mx, s0.y), ImVec2(mx, s1.y), with_alpha(pal().ink, 0.5f), 1.0f);
  draw->PopClipRect();
  draw->AddRect(s0, s1, pal().line, 0.0f, 0, 1.0f);

  // ---- the ruler: the session's clock, labelled where the labels fit
  const int64_t step = tick_step_ms(view.end_ms - view.start_ms, w, px(64));
  const int64_t minor = step / 5 >= 1000 ? step / 5 : step / 2;
  ImFont* small = fonts().small;
  for (int64_t t = std::max<int64_t>(0, (view.start_ms + minor - 1) / minor * minor); t <= view.end_ms; t += minor) {
    const float tx = x_of(t);
    const bool major = t % step == 0;
    draw->AddLine(ImVec2(tx, s1.y), ImVec2(tx, s1.y + px(major ? 5 : 3)), major ? pal().line_strong : pal().line, 1.0f);
    if (!major) continue;
    const std::string label_text = at_label(t);
    const float lw = text_width(small, label_text);
    const float lx = std::clamp(tx - lw / 2, s0.x, s1.x - lw);
    draw->AddText(small, small->LegacySize, ImVec2(lx, s1.y + px(5)), pal().muted, label_text.c_str());
  }

  // ---- scrubbing: the picture at the pointer, a second at a time
  if (hovered && !active) {
    const int64_t grain = std::max<int64_t>(1000, tile_ms / 4);
    const int64_t snapped = mt / grain * grain + grain / 2;
    const int64_t moment = picture_moment(strip, snapped < strip.hi ? snapped : mt);
    const auto found = moment >= 0 ? strip.thumbs.find(moment) : strip.thumbs.end();
    if (moment >= 0 && found == strip.thumbs.end() && asked_count(strip, moment) < kThumbTries) want.insert(want.begin(), moment);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(4), px(4)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, pal().raised);
    ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
    // B6: the picture follows the pointer on the hover token instead of jumping with it.
    {
      const ImGuiID follow = ImGui::GetID("##scrub_follow");
      const ImVec2 target(io.MousePos.x + px(14), io.MousePos.y + px(18));
      ImGui::SetNextWindowPos(ImVec2(anim_to(follow ^ 0x1u, target.x, kRateHover), anim_to(follow ^ 0x2u, target.y, kRateHover)));
    }
    ImGui::BeginTooltip();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 size(px(176), px(99));
    if (found != strip.thumbs.end()) {
      picture(env, found->second, "film", at, size, 0.0f);
    } else {
      ImDrawList* tip = ImGui::GetWindowDrawList();
      tip->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), pal().canvas);
      if (moment >= 0) draw_icon(tip, "film", ImVec2(at.x + (size.x - px(22)) / 2, at.y + (size.y - px(22)) / 2), px(22), pal().line_strong);
    }
    ImGui::Dummy(size);
    const std::string when = at_label(mt) + (moment < 0 ? (mt > strip.hi ? "  being written" : "  no footage") : "");
    text(fonts().small, pal().ink, when);
    ImGui::EndTooltip();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
  }
  ask_thumbs(env, want);
  // The live edge moves a pixel every `per_px` ms: draw that often while it is on screen (never faster than 10 a second).
  if (writing && strip.view_follow) strip.tick_ms = static_cast<uint32_t>(std::clamp(per_px, 100.0, 500.0));
  ImGui::SetCursorScreenPos(ImVec2(origin.x, s1.y + ruler_h));
  fade_since(draw, body_v, open);
  end_pane();
}

namespace {

// One channel of the AUDIO pane, a mixer strip `w` wide: mute at the top, the level in cells beside the volume fader, the
// volume, and the channel's name, which opens the device list. `prefix` is the config's ("desktop" or "mic").
void channel_strip(ViewEnv& env, const char* prefix, const char* name, const char* list_key, const char* icon_on,
                   const char* icon_off, float w, float h) {
  UiStore& store = env.store;
  const Json& config = store.config;
  const std::string key(prefix);
  const bool enabled = config[key + "_enabled"].as_bool(true), muted = config[key + "_muted"].as_bool();
  const bool silent = !enabled || muted;
  ImGui::PushID(prefix);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(ImVec2(pos.x + (w - px(28)) / 2, pos.y));
  const std::string noun = key == "mic" ? "the microphone" : "the game and desktop sound";
  if (icon_button("##mute", silent ? icon_off : icon_on, (!enabled ? "Record " : muted ? "Unmute " : "Mute ") + noun, !silent, 28, 15)) {
    Json patch = Json::object();
    if (enabled) {
      patch.set(key + "_muted", Json(!muted));
    } else {
      patch.set(key + "_enabled", Json(true));
      patch.set(key + "_muted", Json(false));
    }
    store.set_config(patch);
  }
  // The meter and the fader share the middle; the volume and the name sit under them.
  const float label_h = fonts().small->LegacySize;
  const float track_top = pos.y + px(34);
  // Keep the controls together at the bottom and use extra pane height for longer faders and meters.
  const float track_h = std::max(px(kMixerTrackHeight), h - px(58) - label_h * 2);
  // B10: a muted or off strip dims on the hover token instead of switching, and its cells empty with it.
  const float dim = anim_to(ImGui::GetID("##silent"), silent ? 1.0f : 0.0f, kRateHover);
  // B12: the level is fed every 100 ms for the speaker and 200 ms for the mic: a rise shows at once and the fall is a fixed rate between samples,
  // drawn at about 30 a second only while a cell is still lit above the sample.
  const float sample = (key == "mic" ? store.mic_level : store.desktop_level) * (1.0f - dim);
  float& held = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("##held"), 0.0f);
  if (sample >= held || !anim_enabled()) {
    held = sample;
  } else {
    held = std::max(sample, held - 1.1f * std::min(ImGui::GetIO().DeltaTime, 0.1f));
    if (held > sample + 0.002f) anim_touch_slow();
  }
  const float level = held;
  const float pair_x = pos.x + (w - px(10) - px(6) - px(20)) / 2;
  ImGui::SetCursorScreenPos(ImVec2(pair_x, track_top));
  const int meter_cells = std::max(16, static_cast<int>(std::lround(unpx(track_h) / 6.0f)));
  vcells(unpx(track_h), level, meter_cells, 10);
  ImGui::SetCursorScreenPos(ImVec2(pair_x + px(16), track_top));
  float volume = static_cast<float>(config[key + "_volume"].as_double(1.0));
  if (slider_vertical_commit("##volume", volume, unpx(track_h), !enabled)) set(env, (key + "_volume").c_str(), Json(static_cast<double>(volume)));
  // The fader took the wheel's hover: the deck must not scroll under it while the hand is here.
  if (ImGui::IsItemHovered()) env.panel.live.audio_hover = true;
  const std::string percent = std::to_string(static_cast<int>(std::lround(volume * 100))) + "%";
  tooltip(std::string(name) + " volume: " + percent);
  const float percent_y = track_top + track_h + px(4);
  const float pw = text_width(fonts().small, percent);
  draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(pos.x + (w - pw) / 2, percent_y), mix(pal().ink, pal().faint, dim), percent.c_str());
  // The name picks the device: a framed chip with a chevron, so it reads as a control before it is hovered. The list is the
  // Windows default, then the devices the engine lists (read when the list first opens).
  const float chip_h = label_h + px(8);
  const float name_y = track_top + track_h + label_h + px(10);
  const ImVec2 chip_min(pos.x, name_y), chip_max(pos.x + w, name_y + chip_h);
  ImGui::SetCursorScreenPos(chip_min);
  if (ImGui::InvisibleButton("##device", ImVec2(w, chip_h))) {
    // Read again when the list is old: a headset plugged in after the deck opened never appeared until
    // the engine restarted. Five seconds keeps an open/close of the chip from asking every time.
    if ((env.panel.devices.is_null() || GetTickCount() - env.panel.devices_at > 5000) && !env.panel.devices_loading) {
      env.panel.devices_loading = true;
      UiStore& store = env.store;
      store.bridge_call("devices.list", Json::object(), [&panel = env.panel, &store](bool ok, const Json& data, const std::string&) {
        panel.devices_loading = false;
        panel.devices_at = GetTickCount();
        if (ok) panel.devices = data;
        else store.toast(UiToast::error, "The device list did not load", "Close BuffrX and open it again, then try once more.");
      });
    }
    ImGui::OpenPopup("##devices");
  }
  const bool open = ImGui::IsPopupOpen("##devices");
  const float over = anim_item(ImGui::IsItemHovered() || open ? 1.0f : 0.0f, kRateHover);
  tooltip(std::string("Choose the ") + (key == "mic" ? "microphone" : "speakers or headset"));
  if (over > 0.0f) draw->AddRectFilled(chip_min, chip_max, with_alpha(pal().raised, over));
  draw->AddRect(chip_min, chip_max, mix(pal().line_strong, pal().accent, over), 0.0f, 0, 1.0f);
  // A select's corner mark (a strip is too narrow for the name and a chevron side by side).
  const float nw = text_width(fonts().small, name), corner = px(5);
  const ImU32 ink = mix(pal().ink, pal().accent, over);
  draw->PushClipRect(chip_min, chip_max, true);
  draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(pos.x + std::max(px(2), (w - nw) / 2), name_y + (chip_h - label_h) / 2), ink, name);
  draw->PopClipRect();
  draw->AddTriangleFilled(ImVec2(chip_max.x - px(1) - corner, chip_max.y - px(1)), ImVec2(chip_max.x - px(1), chip_max.y - px(1) - corner),
                          ImVec2(chip_max.x - px(1), chip_max.y - px(1)), mix(pal().muted, pal().accent, over));
  // The list opens above the chip (the pane sits at the window's foot): one tight row a device in the small face, long names
  // cut short (the whole name on hover).
  ImGui::SetNextWindowPos(ImVec2(chip_min.x, chip_min.y - px(2)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(3), px(3)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, pal().surface);
  ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
  if (ImGui::BeginPopup("##devices")) {
    const std::string current = config[key + "_device"].as_string();
    std::vector<std::string> ids{"default"}, labels{"Windows default"};
    for (const Json& device : env.panel.devices[list_key].items()) {
      const std::string id = device["id"].as_string();
      if (id == "default" || id.empty()) continue;
      ids.push_back(id);
      labels.push_back(device["name"].as_string(id));
    }
    ImFont* small = fonts().small;
    const float row_w = px(220), row_h = px(22);
    if (env.panel.devices.is_null()) {
      ImGui::Dummy(ImVec2(row_w, px(4)));
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(8));
      text(small, pal().muted, "Reading the devices\xE2\x80\xA6");
      ImGui::Dummy(ImVec2(row_w, px(4)));
    }
    ImDrawList* menu = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < ids.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const bool chosen = ids[i] == (current.empty() ? "default" : current);
      if (ImGui::InvisibleButton("##pick", ImVec2(row_w, row_h))) {
        set(env, (key + "_device").c_str(), Json(ids[i]));
        ImGui::CloseCurrentPopup();
      }
      const bool hovered = ImGui::IsItemHovered();
      if (hovered) menu->AddRectFilled(at, ImVec2(at.x + row_w, at.y + row_h), pal().raised);
      if (chosen) draw_icon(menu, "check", ImVec2(at.x + px(5), at.y + (row_h - px(11)) / 2), px(11), pal().accent);
      ImGui::SetCursorScreenPos(ImVec2(at.x + px(21), at.y + (row_h - small->LegacySize) / 2));
      text_ellipsis(small, chosen ? pal().accent : hovered ? pal().ink : pal().muted, labels[i], row_w - px(27));
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + row_h));
      if (hovered && text_width(small, labels[i]) > row_w - px(27)) {
        ImGui::SetTooltip("%s", labels[i].c_str());
      }
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(row_w, 0));
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
  ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
  ImGui::Dummy(ImVec2(w, 0));
  ImGui::PopID();
}

}  // namespace

void draw_audio(ViewEnv& env, ImVec2 size) {
  begin_pane("audio", "audio", size);
  env.panel.live.audio_hover = false;
  if (env.store.config.is_null()) {
    end_pane();
    return;
  }
  const ImVec2 area = ImGui::GetContentRegionAvail();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float gap = px(8), w = std::floor((area.x - gap) / 2);
  channel_strip(env, "desktop", "speaker", "outputs", "speaker", "speakerOff", w, area.y);
  // The seam between the two strips.
  ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + w + gap / 2, at.y), ImVec2(at.x + w + gap / 2, at.y + area.y), pal().line, 1.0f);
  ImGui::SetCursorScreenPos(ImVec2(at.x + w + gap, at.y));
  channel_strip(env, "mic", "mic", "inputs", "mic", "micOff", w, area.y);
  end_pane();
}

void draw_history(ViewEnv& env, ImVec2 size) {
  UiStore& store = env.store;
  LiveStrip& strip = env.panel.live;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  // A standalone recording, independent of Timeline.
  const Json& status = store.status;
  if (status["recording"].as_bool()) {
    const double running = status["recording_seconds"].as_double();
    if (strip.recording_since == 0) strip.recording_since = now - static_cast<int64_t>(running);
  } else {
    strip.recording_since = 0;
  }
  // The history is the library (and a recording still running): rebuilt only when either changed.
  if (strip.recent_version != store.library_version || strip.recent_since != strip.recording_since) {
    strip.recent = recent_actions(store.clips, store.screenshots, strip.recording_since, 60);
    strip.recent_version = store.library_version;
    strip.recent_since = strip.recording_since;
  }
  begin_pane("history", "history", size);
  const ImVec2 area = ImGui::GetContentRegionAvail();
  if (strip.recent.empty()) {
    text_wrapped(fonts().small, pal().muted,
                 store.library_loaded ? "Nothing yet. Clips, screenshots and recordings show here as you make them." : "Reading the library...",
                 area.x);
    end_pane();
    return;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##recentlist", ImVec2(0, area.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  smooth_scroll(px(78));
  // The list takes the wheel only when it has something to scroll: otherwise the deck behind it does.
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetScrollMaxY() > 0.0f) strip.wheel = true;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  const float w = ImGui::GetContentRegionAvail().x, row_h = px(26);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  // B13: a row is keyed by its file, so a new save is the row that arrives; rows that scroll into view or come back after another
  // page are not new (arrivals_update sees the whole list, the clipper only the rows on screen).
  const ImGuiID history_scope = ImHashStr("history_rows");
  const auto row_key = [](const RecentAction& action, int i) {
    return action.file.empty() ? ImHashStr(("##now" + std::to_string(i)).c_str()) : ImHashStr(action.file.c_str());
  };
  {
    std::vector<ImGuiID> keys;
    keys.reserve(strip.recent.size());
    for (size_t i = 0; i < strip.recent.size(); ++i) keys.push_back(row_key(strip.recent[i], static_cast<int>(i)));
    arrivals_update(history_scope, keys);
  }
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(strip.recent.size()), row_h);
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const RecentAction action = strip.recent[static_cast<size_t>(i)];  // a copy: a click may rebuild the list
      const float arrived = arrival(history_scope, row_key(action, i));
      ImGui::PushID(i);
      const ImVec2 pos = ImGui::GetCursorScreenPos(), max(pos.x + w, pos.y + row_h);
      const int row_v = draw->VtxBuffer.Size;
      const bool pressed = ImGui::InvisibleButton("##action", ImVec2(w, row_h));
      const bool hovered = ImGui::IsItemHovered() && !action.file.empty();
      const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
      if (hover > 0.0f) draw->AddRectFilled(pos, max, with_alpha(pal().raised, hover));
      if (i + 1 < static_cast<int>(strip.recent.size())) draw->AddLine(ImVec2(pos.x, max.y - 1), ImVec2(max.x, max.y - 1), with_alpha(pal().line, 0.6f), 1.0f);
      const char* icon = "save";
      const char* what = "Clip saved";
      ImU32 tint = pal().accent;
      switch (action.kind) {
        case ActionKind::clip: break;
        case ActionKind::timeline_clip: icon = "history"; what = "Timeline clip"; break;
        case ActionKind::screenshot: icon = "camera"; what = "Screenshot"; tint = pal().ink; break;
        case ActionKind::record_start: icon = "record"; what = "Recording started"; tint = pal().rec; break;
        case ActionKind::record_stop: icon = "stop"; what = "Recording stopped"; tint = pal().rec; break;
        case ActionKind::edit: icon = "scissors"; what = "Edit saved"; tint = pal().ink; break;
        case ActionKind::received: icon = "share"; what = "Clip received"; tint = pal().ink; break;
      }
      const float glyph = px(14), cy = pos.y + row_h / 2;
      const ImVec2 icon_at(pos.x + px(6), cy - glyph / 2);
      if (action.kind == ActionKind::record_start) {
        draw->AddCircleFilled(ImVec2(icon_at.x + glyph / 2, cy), px(5), tint);
      } else if (action.kind == ActionKind::record_stop) {
        draw->AddRectFilled(ImVec2(icon_at.x + px(3), cy - px(4)), ImVec2(icon_at.x + px(11), cy + px(4)), tint);
      } else {
        draw_icon(draw, icon, icon_at, glyph, tint, 1.6f);
      }
      const std::string detail = action.file.empty()           ? "now"
                                 : action.kind == ActionKind::screenshot || action.kind == ActionKind::record_start || action.seconds <= 0
                                     ? std::string()
                                     : "(" + format_span(action.seconds) + ")";
      const std::string when = format_when(action.when, now);
      const float when_w = text_width(fonts().small, when);
      const float tx = pos.x + px(30), right = max.x - px(6) - when_w - px(10);
      draw->PushClipRect(ImVec2(tx, pos.y), ImVec2(right, max.y), true);
      ImFont* body = fonts().body;
      draw->AddText(body, body->LegacySize, ImVec2(tx, cy - body->LegacySize / 2), pal().ink, what);
      // The length only where it fits whole: a cut "(1S" reads as a mistake.
      if (!detail.empty() && tx + text_width(body, what) + px(8) + text_width(fonts().small, detail) <= right) {
        draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(tx + text_width(body, what) + px(8), cy - fonts().small->LegacySize / 2),
                      action.file.empty() ? pal().rec : pal().muted, detail.c_str());
      }
      draw->PopClipRect();
      draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(max.x - px(6) - when_w, cy - fonts().small->LegacySize / 2), pal().muted,
                    when.c_str());
      if (!action.file.empty()) tooltip(file_name(action.file));
      // B13: a new row fades in and settles down 8 px (the base token).
      shift_since(draw, row_v, 0.0f, px(8) * (1.0f - arrived));
      fade_since(draw, row_v, arrived);
      ImGui::PopID();
      if (pressed && !action.file.empty()) {
        if (const UiMedia* found = find_media(env, action.file)) {
          const UiMedia item = *found;
          open_preview(env, item, item.clip ? store.clips : store.screenshots);
        }
      }
    }
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  end_pane();
}

}  // namespace bx::ui
