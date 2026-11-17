// The deck's lower half: a compact live timeline of the Timeline session being written - its filmstrip to scrub, a range to
// choose and save as a clip - and the recent actions under it. Every action is the protocol's own command (timeline.get,
// timeline.thumbs, timeline.save, timeline.preview, timeline.open). Nothing is read while the panel is hidden, the session is
// read every 2 s while it shows, and only a slot that came into view asks for a picture (ui_live_math.h).
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <iterator>
#include <set>

#include "../audio_routing.h"
#include "ui_audio_math.h"
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

// All audio modes share the same channel geometry and popup spacing.
constexpr float kMixerStripWidth = 76.0f;
constexpr float kMixerStripHeight = 198.0f;
constexpr float kMixerGap = 8.0f;
constexpr float kMixerChannelGap = 16.0f;
constexpr float kMixerPaddingX = 12.0f;
constexpr float kMixerPaddingY = 10.0f;
constexpr float kMixerBodyTop = 42.0f;

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
      // The last quick clip's length. A buffer too short for any of them starts the range at the whole buffer instead
      // of a one-second sliver, since 0 is not a length.
      const QuickClipRow quick = quick_clip_row(static_cast<int>(store.config["quick_clip_seconds"].as_int(20)),
                                                static_cast<int>(store.config["buffer_seconds"].as_int(60)));
      strip.want = (quick.selected >= 0 ? quick.values[static_cast<size_t>(quick.selected)]
                                        : static_cast<int>(store.config["buffer_seconds"].as_int(60))) * 1000;
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
// `inactive` says this strip is not what records (the output device while Selected apps records): it reads as
// off and its controls are disabled, because they would change nothing.
void channel_strip(ViewEnv& env, const char* prefix, const char* name, const char* list_key, const char* icon_on,
                   const char* icon_off, float w, float h, bool inactive = false) {
  UiStore& store = env.store;
  const Json& config = store.config;
  const std::string key(prefix);
  const bool enabled = !inactive && config[key + "_enabled"].as_bool(true), muted = config[key + "_muted"].as_bool();
  const bool silent = !enabled || muted;
  ImGui::PushID(prefix);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(ImVec2(pos.x + (w - px(28)) / 2, pos.y));
  const std::string noun = key == "mic" ? "the microphone" : "the output device";
  const std::string mute_label =
      inactive ? std::string("Your output device is not recorded in Selected apps mode")
               : ((!enabled ? "Record " : muted ? "Unmute " : "Mute ") + noun);
  if (icon_button("##mute", silent ? icon_off : icon_on, mute_label, !silent, 28, 15)) {
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
  // Keep the controls together at the bottom and use extra pane height for longer faders and meters. The
  // compact pane has one row for the mixer button above these strips, so the floor is lower than the mixer's.
  const float track_h = std::max(px(56), h - px(58) - label_h * 2);
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
  tooltip(inactive ? std::string("Not recorded while Selected apps records") : std::string(name) + " volume: " + percent);
  const float percent_y = track_top + track_h + px(4);
  const float pw = text_width(fonts().small, percent);
  draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(pos.x + (w - pw) / 2, percent_y), mix(pal().ink, pal().faint, dim), percent.c_str());
  // The name picks the device: a framed chip with a chevron, so it reads as a control before it is hovered. The list is the
  // Windows default, then the devices the engine lists (read when the list first opens).
  const float chip_h = label_h + px(8);
  const float name_y = track_top + track_h + label_h + px(10);
  const ImVec2 chip_min(pos.x, name_y), chip_max(pos.x + w, name_y + chip_h);
  ImGui::SetCursorScreenPos(chip_min);
  if (ImGui::InvisibleButton("##device", ImVec2(w, chip_h)) && !inactive) {
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
  tooltip(inactive ? std::string("Selected apps records only the apps you add and the microphone. Change what is recorded in Settings > Audio.")
                   : std::string("Choose the ") + (key == "mic" ? "microphone" : "speakers or headset"));
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

namespace {

// One lane of status.audio.lanes, copied: the reply that brought it may be replaced between frames, and a
// row must draw the lane it was laid out with.
bool auto_lane(const Json& audio_status, const std::string& key, Json& lane) {
  for (const Json& item : audio_lanes_of(audio_status).items()) {
    if (audio_lane_key(item) == key) {
      lane = item;
      return true;
    }
  }
  return false;
}

// The value a lane draws: the person's own preference while it exists (written the moment a control moves),
// otherwise what the engine last reported for the lane. This is what keeps a fader from jumping back while
// the next status arrives.
double lane_volume(const Json& config, const Json& lane) {
  const std::string exe = lane["exe"].as_string();
  const Json& prefs = config["audio_app_prefs"];
  if (!exe.empty() && audio_pref_has(prefs, exe)) return audio_pref_volume(prefs, exe);
  return std::clamp(lane["volume"].as_double(1.0), 0.0, 1.0);
}
bool lane_muted(const Json& config, const Json& lane) {
  const std::string exe = lane["exe"].as_string();
  const Json& prefs = config["audio_app_prefs"];
  if (!exe.empty() && audio_pref_has(prefs, exe)) return audio_pref_muted(prefs, exe);
  return lane["muted"].as_bool(false);
}
// What a lane is doing, in the short sentence the row and the strip's hover card use. No track number and
// no encoder detail: this surface is the person's own.
std::string lane_state_text(const Json& lane, bool capturing) {
  const std::string state = lane["state"].as_string();
  if (state == "failed") return "Audio capture failed";
  if (state == "waiting") return "Waiting for this app to play sound";
  if (state == "covered") return "Shared with another app's capture";
  return capturing ? "Recording on its own track" : "Ready when the buffer or a recording runs";
}

// The marks a lane's row and strip say after its state.
std::string lane_marks(bool muted) {
  std::string out;
  if (muted) out += " \xC2\xB7 muted";
  return out;
}

void set_audio_prefs(ViewEnv& env, const std::string& exe, const Json& fields) {
  env.store.set_config(audio_pref_patch(exe, fields));
}

}  // namespace

// A mixer edit writes the whole selection back: the engine's config.set takes one array, and every entry the
// person is not touching keeps its place and its fields.
void set_audio_apps(ViewEnv& env, const Json& list) {
  Json patch = Json::object();
  patch.set("audio_apps", list);
  patch.set("audio_apps_chosen", Json(true));
  env.store.set_config(patch);
}

// A strip in the expanded mixer. `activity_exe` names a program whose measured activity feeds
// the meter; empty means the meter is the captured level (the desktop or the microphone). `tip` is the full
// sentence the strip's one-line state stands for; `structural_blocked` says a manual recording refuses an app
// add, replace or remove, while its gain and mute stay live.
struct StripView {
  std::string id;
  std::string name;
  std::string tip;
  StripState state = StripState::off;
  const char* icon_on = "speaker";
  const char* icon_off = "speakerOff";
  std::string activity_exe;
  float volume = 1.0f;
  float level = 0.0f;
  bool enabled = true;
  bool muted = false;
  bool inactive = false;
  bool structural_blocked = false;
  int slot = -1;
  // Automatic app audio: the program whose preferences this strip writes (empty for the microphone), its
  // or the Other group's one level and mute.
  std::string pref_exe;
  bool other_group = false;
  // The microphone: an input, not a program's output. Its channel sits on a tile of its own.
  bool input = false;
};

// The full name and state on hover: the strip's own width only has room for the start of each. The card is
// drawn on the foreground list, because a tooltip window opened inside a popup can be hidden by it.
void strip_hint(const std::string& text) {
  if (text.empty()) return;
  ImFont* font = fonts().small;
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  float width = 0.0f;
  for (const std::string& line : lines) width = std::max(width, text_width(font, line));
  const float line_h = font->LegacySize + px(2);
  const ImVec2 size(width + px(16), lines.size() * line_h + px(10));
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const ImVec2 view = ImGui::GetMainViewport()->WorkPos;
  const ImVec2 view_size = ImGui::GetMainViewport()->WorkSize;
  float x = mouse.x + px(14);
  if (x + size.x > view.x + view_size.x - px(6)) x = mouse.x - size.x - px(14);
  x = std::max(view.x + px(6), x);
  float y = mouse.y + px(18);
  y = std::max(view.y + px(6), std::min(y, view.y + view_size.y - size.y - px(6)));
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->AddRectFilled(ImVec2(x, y), ImVec2(x + size.x, y + size.y), pal().raised);
  draw->AddRect(ImVec2(x, y), ImVec2(x + size.x, y + size.y), pal().line_strong);
  for (size_t i = 0; i < lines.size(); ++i) {
    draw->AddText(font, font->LegacySize, ImVec2(x + px(8), y + px(5) + i * line_h), pal().ink, lines[i].c_str());
  }
}

// The one-cell-wide channel of a mixer: the name and its state at the top, a vertical fader with the level in
// cells beside it, the percentage, and the mute at the foot. It commits its volume when the hand lets go, never
// on every frame of a drag. The name owns the full width: the strip's own menu sits at the foot beside the mute,
// where it costs no name its letters, and every channel starts its fader at the same height so five of them read
// as one row. Automatic app strips also offer a direct exclude control.
void expanded_strip(ViewEnv& env, StripView& strip, float w, float h) {
  ImGui::PushID(strip.id.c_str());
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float bottom = pos.y + h;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const bool silent = !strip.enabled || strip.muted;
  const float dim = anim_to(ImGui::GetID("##silent"), silent ? 1.0f : 0.0f, kRateHover);
  const float name_h = fonts().strong->LegacySize, small_h = fonts().small->LegacySize;
  if (strip.input) {
    // The input's tile reaches into the gap beside it, so the microphone reads apart from the programs.
    const ImVec2 a(pos.x - px(4), pos.y - px(6)), b(pos.x + w + px(4), bottom + px(4));
    draw->AddRectFilled(a, b, mix(pal().surface, pal().raised, 0.45f));  // the meter's empty cells are raised: they must show on it
    draw->AddRect(a, b, pal().line_strong);
  }

  // Name over its state, across the whole channel: a program's name is only cut when it is genuinely longer
  // than 76 units, and the whole of it is on hover.
  const float name_room = w - px(2);
  const float name_w = text_width(fonts().strong, strip.name);
  const ImU32 name_ink = mix(pal().ink, pal().faint, dim);
  if (name_w <= name_room) {
    draw->AddText(fonts().strong, name_h, ImVec2(pos.x + (w - name_w) / 2, pos.y), name_ink, strip.name.c_str());
  } else {
    ImGui::SetCursorScreenPos(ImVec2(pos.x + px(1), pos.y));
    text_ellipsis(fonts().strong, name_ink, strip.name, name_room);
  }
  const float state_y = pos.y + name_h + px(2);
  const char* state_text = audio_strip_label(strip.state);
  ImU32 state_color = strip.state == StripState::failed       ? pal().danger
                      : strip.state == StripState::recording  ? pal().accent
                      : strip.state == StripState::ready || strip.state == StripState::shared ? pal().muted
                                                              : pal().faint;
  // A muted channel says so at full strength while the rest of it dims: the icon alone was too quiet.
  if (strip.state != StripState::muted) state_color = mix(state_color, pal().faint, dim);
  else state_color = pal().ink;
  draw->PushClipRect(ImVec2(pos.x + px(1), state_y), ImVec2(pos.x + w - px(1), state_y + small_h), true);
  const float label_x = pos.x + std::max(px(1), (w - text_width(fonts().small, state_text)) / 2);
  draw->AddText(fonts().small, small_h, ImVec2(label_x, state_y), state_color, state_text);
  draw->PopClipRect();
  // Every channel's header is these two lines, whether or not it has a program speaker: the faders below line up.
  const float header_h = name_h + px(2) + small_h;
  // The full name and state on hover: the strip's own width only has room for a short line.
  {
    std::string full = strip.name;
    if (!strip.tip.empty()) full += "\n" + strip.tip;
    full += "\nVolume " + std::to_string(static_cast<int>(std::lround(strip.volume * 100))) + "%" +
            (strip.muted ? ", muted" : "");
    if (ImGui::IsMouseHoveringRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + w, pos.y + header_h + px(6))) &&
        !ImGui::IsAnyItemActive()) {
      strip_hint(full);
    }
  }

  // The fader and the meter share the middle; the percentage, the foot's mute and the menu sit under them.
  const float foot_h = px(26);
  const float percent_y = bottom - foot_h - small_h - px(6);
  const float track_top = pos.y + header_h + px(8);
  const float track_h = std::max(px(64), percent_y - px(5) - track_top);
  const float pair_x = pos.x + (w - px(10) - px(6) - px(20)) / 2;
  // The level is fed every 100 ms for the speaker and 200 ms for the mic; an app's activity arrives on the
  // sessions read. A rise shows at once, the fall is a fixed rate between samples, and the panel is only asked
  // for another frame while a cell is still lit above the sample.
  float sample = (strip.inactive ? 0.0f : strip.level) * (1.0f - dim);
  float& held = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("##held"), 0.0f);
  if (sample >= held || !anim_enabled()) {
    held = sample;
  } else {
    held = std::max(sample, held - 1.1f * std::min(ImGui::GetIO().DeltaTime, 0.1f));
    if (held > sample + 0.002f) anim_touch_slow();
  }
  const float level = held;
  ImGui::SetCursorScreenPos(ImVec2(pair_x, track_top));
  const int meter_cells = std::max(16, static_cast<int>(std::lround(unpx(track_h) / 6.0f)));
  vcells(unpx(track_h), level, meter_cells, 10);
  ImGui::SetCursorScreenPos(ImVec2(pair_x + px(16), track_top));
  float volume = strip.volume;
  if (slider_vertical_commit("##volume", volume, unpx(track_h), !strip.enabled || strip.inactive)) {
    if (strip.slot >= 0) {
      set_audio_apps(env, audio_apps_with_field(env.store.config["audio_apps"], static_cast<size_t>(strip.slot),
                                               "volume", Json(static_cast<double>(volume))));
    } else if (!strip.pref_exe.empty()) {
      set_audio_prefs(env, strip.pref_exe, audio_volume_field(volume));
    } else if (strip.other_group) {
      env.store.set_config(audio_other_volume_patch(volume));
    } else {
      set(env, (strip.id + "_volume").c_str(), Json(static_cast<double>(volume)));
    }
  }
  // The fader took the wheel's hover: the deck must not scroll under it while the hand is here.
  if (ImGui::IsItemHovered()) env.panel.live.audio_hover = true;
  const std::string percent = std::to_string(static_cast<int>(std::lround(volume * 100))) + "%";
  const float pw = text_width(fonts().small, percent);
  draw->AddText(fonts().small, small_h, ImVec2(pos.x + (w - pw) / 2, percent_y), mix(pal().ink, pal().faint, dim), percent.c_str());

  // The meter shows activity; the foot holds mute and, for automatic apps, exclude.
  const bool can_exclude = !strip.pref_exe.empty();
  // A manual app slot: swap it for another program, or take it out. Both rebuild the encoder's audio, so a
  // manual recording holds them back (the reason is the tooltip).
  const bool can_edit_slot = strip.slot >= 0;
  // Three buttons must fit a 76-unit strip: narrower and closer together.
  const float gap = px(can_edit_slot ? 2 : 4), mute_w = px(can_edit_slot ? 22 : 26);
  const int extras = (can_exclude ? 1 : 0) + (can_edit_slot ? 2 : 0);
  float fx = pos.x + (w - (mute_w * (1 + extras) + gap * extras)) / 2;
  const float foot_y = bottom - foot_h;
  ImGui::SetCursorScreenPos(ImVec2(fx, foot_y));
  ImGui::BeginDisabled(strip.inactive);
  const std::string mute_tip = strip.inactive  ? std::string("This channel is not recorded in this mode")
                               : !strip.enabled ? "Turn on " + strip.name
                               : strip.muted    ? "Unmute " + strip.name
                                                : "Mute " + strip.name;
  const bool muted_pressed = icon_button("##mute", silent ? strip.icon_off : strip.icon_on, mute_tip, !silent, unpx(mute_w), 14);
  ImGui::EndDisabled();
  if (muted_pressed) {
    if (strip.slot >= 0) {
      Json apps = env.store.config["audio_apps"];
      set_audio_apps(env, audio_apps_with_field(apps, static_cast<size_t>(strip.slot), "muted",
                                               Json(!audio_app_muted(apps.items()[static_cast<size_t>(strip.slot)]))));
    } else if (!strip.pref_exe.empty()) {
      set_audio_prefs(env, strip.pref_exe, audio_muted_field(!strip.muted));
    } else if (strip.other_group) {
      env.store.set_config(audio_other_muted_patch(!strip.muted));
    } else {
      Json patch = Json::object();
      if (strip.enabled) {
        patch.set(strip.id + "_muted", Json(!strip.muted));
      } else {
        patch.set(strip.id + "_enabled", Json(true));
        patch.set(strip.id + "_muted", Json(false));
      }
      env.store.set_config(patch);
    }
  }
  if (can_exclude) {
    ImGui::SetCursorScreenPos(ImVec2(fx + mute_w + gap, foot_y));
    if (icon_button("##exclude", "close", "Exclude this app: stop capturing it until you restore it", false, 26, 12, true))
      set_audio_prefs(env, strip.pref_exe, audio_excluded_field(true));
  }
  if (can_edit_slot) {
    const char* blocked = "Stop the recording first: changing apps rebuilds the encoder.";
    ImGui::BeginDisabled(strip.structural_blocked);
    ImGui::SetCursorScreenPos(ImVec2(fx + mute_w + gap, foot_y));
    const bool swap = icon_button("##replace", "refresh", strip.structural_blocked ? blocked : "Replace " + strip.name + " with another app", false, unpx(mute_w), 12);
    ImGui::SetCursorScreenPos(ImVec2(fx + 2 * (mute_w + gap), foot_y));
    const bool remove = icon_button("##remove", "close", strip.structural_blocked ? blocked : "Remove " + strip.name + " from the recording", false, unpx(mute_w), 12, true);
    ImGui::EndDisabled();
    AudioMixerState& mixer = env.panel.live.mixer;
    if (swap) {
      mixer.pick = true;
      mixer.pick_replace = strip.slot;
      mixer.search.clear();
      mixer.first = mixer.programs.is_null();
    }
    if (remove) set_audio_apps(env, audio_apps_without(env.store.config["audio_apps"], static_cast<size_t>(strip.slot)));
  }
  ImGui::SetCursorScreenPos(ImVec2(pos.x, bottom));
  ImGui::PopID();
}

// One compact source on the collapsed AUDIO pane, for Selected apps: the program's own speaker, its name and
// state, and a mute. A press on the row opens the mixer; the mute is its own target.
bool compact_audio_row(ViewEnv& env, const std::string& id, const std::string& name, const std::string& status,
                       const std::string& activity_exe, bool failed, bool connected, bool muted, bool enabled, float level,
                       float w, float h, const std::function<void()>& toggle_mute, const char* icon_on = "mic",
                       const char* icon_off = "micOff") {
  ImGui::PushID(id.c_str());
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 max(pos.x + w, pos.y + h);
  const bool pressed = ImGui::InvisibleButton("##row", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const float over = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (over > 0.0f) draw->AddRectFilled(pos, max, with_alpha(pal().raised, over));
  draw->AddLine(ImVec2(pos.x, max.y - 1), ImVec2(max.x, max.y - 1), pal().line, 1.0f);
  const float cy = pos.y + h / 2;
  const float right = max.x - px(26);
  draw->PushClipRect(ImVec2(pos.x + px(4), pos.y), ImVec2(right - px(12), max.y), true);
  draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(pos.x + px(4), cy - fonts().small->LegacySize / 2),
                muted || !enabled ? pal().faint : pal().ink, name.c_str());
  draw->PopClipRect();
  // A two-pixel meter along the row's foot: the app's own activity, or the captured level of the microphone.
  const float bar_x0 = pos.x + px(4), bar_x1 = right - px(14);
  draw->AddRectFilled(ImVec2(bar_x0, max.y - px(4)), ImVec2(bar_x1, max.y - px(2)), pal().raised);
  if (level > 0.0f && !muted && enabled) {
    const float fill = std::clamp(level, 0.0f, 1.0f) * (bar_x1 - bar_x0);
    draw->AddRectFilled(ImVec2(bar_x0, max.y - px(4)), ImVec2(bar_x0 + fill, max.y - px(2)), pal().ok);
  }
  // A state dot says connected, waiting or failed without spending the row's width on a sentence; the sentence
  // is the tooltip. A measured silence is never drawn as a disconnected app.
  const ImU32 dot = failed ? pal().danger : connected ? pal().ok : pal().faint;
  draw->AddCircleFilled(ImVec2(right - px(6), cy), px(2.0f), failed ? dot : with_alpha(dot, muted ? 0.35f : 0.9f));
  ImGui::SetCursorScreenPos(ImVec2(max.x - px(24), cy - px(11)));
  if (icon_button("##mute", muted || !enabled ? icon_off : icon_on, muted ? "Unmute for the recording" : "Mute for the recording",
                  !muted && enabled, 22, 12)) {
    toggle_mute();
  }
  if (hovered && !ImGui::IsAnyItemActive()) tooltip_rect(pos, max, name + "\n" + status);
  ImGui::PopID();
  return pressed;
}

// The app list the expanded mixer shows: search, the running programs loudest first, and one pick. The four-slot
// limit is held here, and a pick is a direct add or replace, not a tick waiting for Apply.
void draw_audio_picker(ViewEnv& env, const Json& apps, size_t used, size_t slot_limit, float width, float height) {
  LiveStrip& live = env.panel.live;
  AudioMixerState& mixer = live.mixer;
  const bool replace = mixer.pick_replace >= 0;
  const bool addable = used < slot_limit;
  // The snapshot: read only while this list is on screen. A refresh keeps the rows it already has.
  const bool first = mixer.programs.is_null();
  if (first || env.now_ms - mixer.programs_at >= 3000) {
    if (!mixer.programs_loading) {
      mixer.programs_at = env.now_ms;
      mixer.programs_loading = true;
      UiStore& store = env.store;
      AudioMixerState* into = &mixer;
      store.bridge_call("sources.windows", Json::object(), [&store, into](bool ok, const Json& reply, const std::string&) {
        into->programs_loading = false;
        into->first = false;
        if (ok) into->programs = reply;
        store.dirty = true;
      });
    }
  }
  // Rows are ordered by what is making sound now, frozen while the pointer is in the list.
  const bool frozen = env.now_ms - mixer.activity_at < 1500;
  if (mixer.order.empty() || env.now_ms - mixer.order_at >= 1000) {
    std::vector<Json> choices = audio_program_choices(mixer.programs);
    std::sort(choices.begin(), choices.end(), [&](const Json& left, const Json& right) {
      const bool loud_left = program_sounding(mixer.sessions, left["exe"].as_string());
      const bool loud_right = program_sounding(mixer.sessions, right["exe"].as_string());
      if (loud_left != loud_right) return loud_left;
      return lower_text(left["exe"].as_string()) < lower_text(right["exe"].as_string());
    });
    std::vector<std::string> current;
    for (const Json& choice : choices) current.push_back(choice["exe"].as_string());
    mixer.order = audio_stable_order(mixer.order, current, frozen);
    mixer.order_at = env.now_ms;
  }

  // The popup's header names this list and holds the way back; the list starts with its search.
  const ImVec2 at = ImGui::GetCursorScreenPos();
  if (text_field("##mixsearch", mixer.search, unpx(width), "Search apps", 64)) mixer.activity_at = env.now_ms;
  // A pick is a structural change: it rebuilds the encoder, so a manual recording refuses it. Said here, once,
  // at the moment the choice is made rather than as a warning over the whole mixer.
  const bool structural_blocked = env.store.status["recording"].as_bool();
  // A rebuild restarts the buffer and its footage goes with it. Said once, at the point of the choice, and only
  // while a buffer is actually running.
  const bool buffer_lost = !structural_blocked && env.store.status["buffering"].as_bool();
  const std::string note = structural_blocked ? "Stop the recording before changing apps."
                                            : "Restarts the buffer and clears its footage.";
  const float note_h = (structural_blocked || buffer_lost)
                           ? fonts().small->CalcTextSizeA(fonts().small->LegacySize, FLT_MAX, width, note.c_str()).y
                           : 0.0f;
  const float note_gap = note_h > 0.0f ? px(kMixerGap) : 0.0f;
  ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + px(30)));
  const float list_h = std::max(1.0f, height - px(30) - note_gap - note_h);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, px(3)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(6), px(4)));
  ImGui::BeginChild("##mixprograms", ImVec2(width, list_h), ImGuiChildFlags_None, ImGuiWindowFlags_None);
  wheel_owner_rect(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                                                   ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) mixer.activity_at = env.now_ms;
  const bool first_load = mixer.first || (mixer.programs.is_null() && mixer.programs_loading);
  if (first_load) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    spinner(ImGui::GetWindowDrawList(), ImVec2(pos.x + width / 2, pos.y + list_h / 2), px(8), pal().accent);
  } else {
    const std::string search = lower_text(mixer.search);
    int drawn = 0;
    for (const std::string& exe : mixer.order) {
      if (!search.empty() && lower_text(exe).find(search) == std::string::npos &&
          lower_text(program_name(exe)).find(search) == std::string::npos) {
        continue;
      }
      Json window = Json::object();
      for (const Json& choice : mixer.programs.items()) {
        if (same_program(choice["exe"].as_string(), exe)) {
          window = choice;
          break;
        }
      }
      if (window.is_null() || window["exe"].as_string().empty()) continue;
      const bool chosen = std::any_of(apps.items().begin(), apps.items().end(), [&](const Json& app) {
        return same_program(app["exe"].as_string(), exe);
      });
      const bool replacing_this =
          replace && static_cast<size_t>(mixer.pick_replace) < apps.items().size() &&
          same_program(apps.items()[static_cast<size_t>(mixer.pick_replace)]["exe"].as_string(), exe);
      const bool selectable = (replace ? (!chosen || replacing_this) : (!chosen && addable)) && !structural_blocked;
      ImGui::PushID(exe.c_str());
      const ImVec2 min = ImGui::GetCursorScreenPos();
      const float row_h = px(30), row_w = ImGui::GetContentRegionAvail().x;
      const bool pressed = ImGui::InvisibleButton("##program", ImVec2(row_w, row_h)) && selectable;
      const bool hovered = ImGui::IsItemHovered();
      if (hovered) mixer.activity_at = env.now_ms;
      ImDrawList* draw = ImGui::GetWindowDrawList();
      if (hovered) draw->AddRectFilled(min, ImVec2(min.x + row_w, min.y + row_h), pal().raised);
      audio_speaker_icon(mixer.sessions, exe, ImVec2(min.x + px(5), min.y + (row_h - px(13)) / 2), px(13), env.now_ms,
                         env.panel);
      draw->PushClipRect(ImVec2(min.x + px(22), min.y), ImVec2(min.x + row_w - px(52), min.y + row_h), true);
      draw->AddText(fonts().strong, fonts().strong->LegacySize,
                    ImVec2(min.x + px(22), min.y + (row_h - fonts().strong->LegacySize) / 2), pal().ink,
                    program_name(exe).c_str());
      draw->PopClipRect();
      const char* mark = structural_blocked ? "" : replacing_this ? "Current" : chosen ? "In use" : selectable ? "" : "Full";
      const float mark_w = text_width(fonts().small, mark);
      draw->AddText(fonts().small, fonts().small->LegacySize,
                    ImVec2(min.x + row_w - px(10) - mark_w, min.y + (row_h - fonts().small->LegacySize) / 2),
                    replacing_this ? pal().accent : chosen ? pal().muted : pal().faint, mark);
      if (hovered) {
        const float peak = program_peak(mixer.sessions, exe);
        std::string tip = exe + "\n" + window["name"].as_string();
        if (structural_blocked) tip = std::string("Stop the recording first: changing apps rebuilds the encoder.\n") + tip;
        else if (!selectable) tip = std::string(chosen ? "Already selected in another slot\n" : "The four slots are full\n") + tip;
        else tip += peak < 0.0f ? "\nAudio activity is unavailable"
                   : program_sounding(mixer.sessions, exe) ? "\nMaking sound now"
                                                           : "\nNo sound right now";
        tooltip_rect(min, ImVec2(min.x + row_w, min.y + row_h), tip);
      }
      if (pressed) {
        const Json list = audio_apps_with_entry(apps, audio_app_entry(window), replace ? mixer.pick_replace : -1, slot_limit);
        set_audio_apps(env, list);
        mixer.pick = false;
        mixer.pick_replace = -1;
      }
      ImGui::PopID();
      ++drawn;
    }
    if (drawn == 0) {
      text(fonts().small, pal().faint,
           mixer.order.empty() ? "No application windows found. Open the app, then try again." : "No app matches that search.");
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  if (note_h > 0.0f) {
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + px(30) + list_h + note_gap));
    text_wrapped(fonts().small, structural_blocked ? pal().danger : pal().muted, note, width);
  }
}

// One member of the Other lane: its own level, mute and exclude. Everything is written by the
// executable, so a lane that comes and goes never moves an edit onto another program.
void other_member_row(ViewEnv& env, const Json& member, float width) {
  const std::string exe = member["exe"].as_string();
  const Json& config = env.store.config;
  const Json& prefs = config["audio_app_prefs"];
  // The member's own gain, which is what this fader writes. The engine reports each member's effective level
  // (its own gain times the Other group's), so reading that here would show the group's gain on every member
  // and the first drag would jump away from the pointer.
  const double volume = audio_member_volume(prefs, exe);
  // Its own mute, for the same reason as its gain: the engine reports a member as muted when the group is
  // muted, and this control is the member's own switch.
  const bool muted = audio_member_muted(prefs, exe);
  const bool failed = member["state"].as_string() == "failed";
  ImGui::PushID(exe.c_str());
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float row_h = px(26), cy = pos.y + row_h / 2;
  const float name_min = pos.x + px(2), name_w = px(122);
  ImGui::SetCursorScreenPos(ImVec2(name_min, cy - fonts().small->LegacySize / 2));
  text_ellipsis(fonts().small, muted ? pal().faint : pal().ink, program_name(exe), name_w);
  if (ImGui::IsMouseHoveringRect(ImVec2(name_min, pos.y), ImVec2(name_min + name_w, pos.y + row_h)) &&
      !ImGui::IsAnyItemActive()) {
    strip_hint(program_name(exe) + "\n" + exe + "\n" +
               (failed ? std::string("Audio capture failed") : muted ? std::string("Muted") : std::string("Played through the Other audio track")) +
               "\nVolume " + std::to_string(static_cast<int>(std::lround(volume * 100))) + "%");
  }
  // Its own level, committed when the hand lets go like every other fader here.
  int percent = static_cast<int>(std::lround(volume * 100.0));
  const float slider_w = px(96), item_h = px(18);
  ImGui::SetCursorScreenPos(ImVec2(pos.x + px(132), cy - item_h / 2));
  const bool volume_released = slider_commit("##membervolume", percent, 0, 100, "%d%%", unpx(slider_w));
  if (ImGui::IsItemHovered()) {
    env.panel.live.audio_hover = true;
    env.panel.live.mixer.activity_at = env.now_ms;
  }
  tooltip(program_name(exe) + " volume: " + std::to_string(static_cast<int>(std::lround(volume * 100))) + "%");
  if (volume_released) set_audio_prefs(env, exe, audio_volume_field(percent / 100.0));
  // The two overrides at the row's right: mute and exclude.
  float ix = pos.x + width - px(20);
  const float iy = pos.y + (row_h - px(20)) / 2;
  ImGui::SetCursorScreenPos(ImVec2(ix, iy));
  if (icon_button("##memberexclude", "close", "Exclude this app: stop capturing it until you restore it", false, 20,
                  11, true)) {
    set_audio_prefs(env, exe, audio_excluded_field(true));
  }
  ix -= px(22);
  ImGui::SetCursorScreenPos(ImVec2(ix, iy));
  if (icon_button("##membermute", muted ? "speakerOff" : "speaker", muted ? "Unmute" : "Mute", !muted, 20, 11)) {
    set_audio_prefs(env, exe, audio_muted_field(!muted));
  }
  // Close the layout at the row's own foot: ImGui 1.92 wants an item after a placed cursor.
  ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + row_h));
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::PopID();
}

// The automatic mixer: the lanes the engine reports (up to three apps, the Other group, the microphone), no
// picker and no Add: an app joins and leaves as Windows says it starts and stops playing. The Other group's
// members open under it, each with its own level and the two overrides.
void draw_auto_mixer(ViewEnv& env, float width) {
  UiStore& store = env.store;
  const Json& config = store.config;
  const Json& status = store.status;
  const Json& audio_status = status["audio"];
  const Json& lanes = audio_lanes_of(audio_status);
  const Json& prefs = config["audio_app_prefs"];
  LiveStrip& live = env.panel.live;
  AudioMixerState& mixer = live.mixer;
  const bool capturing = store.buffer_on() || status["recording"].as_bool();
  // An automatic lane is what a running capture writes, so it says Recording/Ready/Failed/Waiting; the reported
  // `recorded` flag is the legacy apps-mode one and would label every live lane "Saved".
  const bool recorded = audio_lanes_recorded(audio_is_auto(config), audio_status["recorded"].as_bool(true), capturing);
  const AutoAudioState state = audio_auto_state(audio_status, capturing);

  // The lanes' order, frozen while a hand is in the mixer: a stream that starts cannot move the row under
  // the pointer. Keys are executable keys, never positions.
  std::vector<std::string> keys;
  for (const Json& lane : lanes.items()) keys.push_back(audio_lane_key(lane));
  const bool frozen = env.now_ms - mixer.activity_at < 1500;
  mixer.lane_order = audio_stable_order(mixer.lane_order, keys, frozen);
  std::vector<Json> app_lanes;
  Json other;
  bool has_other = false;
  for (const std::string& key : mixer.lane_order) {
    Json lane;
    if (!auto_lane(audio_status, key, lane)) continue;
    if (audio_lane_other(lane)) {
      other = lane;
      has_other = true;
    } else if (app_lanes.size() < 3) {
      // The engine keeps three app lanes of its own; a fourth would not fit five channels on this deck.
      app_lanes.push_back(lane);
    }
  }
  const Json& members = other["apps"];
  const bool has_members = has_other && !members.items().empty();

  const ImVec2 start = ImGui::GetCursorScreenPos();
  float y = start.y;
  if (state != AutoAudioState::active) {
    const float inset = state == AutoAudioState::waiting
                            ? std::max(0.0f, (width - text_width(fonts().small, audio_auto_line(state))) / 2)
                            : 0.0f;
    ImGui::SetCursorScreenPos(ImVec2(start.x + inset, y));
    text_wrapped(fonts().small, pal().muted, audio_auto_line(state), width);
    y = ImGui::GetCursorScreenPos().y + px(6);
  }

  const int strips = static_cast<int>(app_lanes.size()) + (has_other ? 1 : 0) + 1;
  const float strip_gap = px(kMixerChannelGap), strip_h = px(kMixerStripHeight);
  const float strip_w = std::min(px(kMixerStripWidth), (width - (strips - 1) * strip_gap) / strips);
  const float total = strips * strip_w + (strips - 1) * strip_gap;
  float x = start.x + (width - total) * 0.5f;
  const auto place = [&](StripView& strip) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    expanded_strip(env, strip, strip_w, strip_h);
    x += strip_w + strip_gap;
  };
  for (const Json& lane : app_lanes) {
    const std::string exe = lane["exe"].as_string();
    StripView strip;
    strip.id = "lane" + audio_exe_key(exe);
    strip.name = audio_lane_title(lane);
    strip.volume = static_cast<float>(lane_volume(config, lane));
    strip.level = std::clamp(program_peak(mixer.sessions, exe), 0.0f, 1.0f);
    strip.muted = lane_muted(config, lane);
    strip.tip = lane_state_text(lane, capturing) + lane_marks(strip.muted);
    strip.activity_exe = exe;
    strip.state = audio_strip_state(true, strip.muted, recorded, lane["state"].as_string(), capturing);
    strip.pref_exe = exe;
    place(strip);
  }
  if (has_other) {
    StripView strip;
    strip.id = "otheraudio";
    strip.name = "Other";
    strip.volume = static_cast<float>(audio_other_volume(config));
    strip.muted = audio_other_muted(config);
    strip.other_group = true;
    strip.tip = "Apps without a track of their own share this one.\nOpen its list to set each app's level.";
    strip.state = audio_strip_state(true, strip.muted, recorded, other["state"].as_string("connected"), capturing);
    place(strip);
  }
  {
    StripView mic;
    mic.id = "mic";
    mic.name = "MIC";
    mic.icon_on = "mic";
    mic.input = true;
    mic.icon_off = "micOff";
    mic.tip = "The microphone";
    mic.volume = static_cast<float>(config["mic_volume"].as_double(1.0));
    mic.level = store.mic_level;
    mic.enabled = config["mic_enabled"].as_bool();
    mic.muted = config["mic_muted"].as_bool();
    mic.state = audio_strip_state(mic.enabled, mic.muted, true, "connected", capturing);
    place(mic);
  }

  y += strip_h;
  // Programs the engine could not capture: named, with the reason. Without this the mixer would draw only its
  // lanes and read as if every program were being recorded.
  const size_t uncaptured = audio_uncaptured_total(audio_status);
  const std::vector<AutoWarning> warnings = audio_uncaptured_warnings(audio_status, 3);
  const std::vector<std::string> excluded = audio_excluded(prefs);
  if (uncaptured > 0 || has_other || !excluded.empty() || audio_status["auto"]["performance_override"].as_bool())
    y += px(kMixerGap);
  if (uncaptured > 0) {
    ImGui::SetCursorScreenPos(ImVec2(start.x, y));
    text(fonts().strong, pal().danger, "NOT RECORDED");
    y += px(kAutoWarningLine);
    for (const AutoWarning& warning : warnings) {
      ImGui::SetCursorScreenPos(ImVec2(start.x, y));
      text_ellipsis(fonts().small, pal().danger, warning.name + ": " + warning.reason, width);
      if (ImGui::IsMouseHoveringRect(ImVec2(start.x, y), ImVec2(start.x + width, y + px(kAutoWarningLine))) &&
          !ImGui::IsAnyItemActive()) {
        strip_hint(warning.name + "\n" + warning.reason);
      }
      y += px(kAutoWarningLine);
    }
    if (uncaptured > warnings.size()) {
      ImGui::SetCursorScreenPos(ImVec2(start.x, y));
      text_ellipsis(fonts().small, pal().muted,
                    "+" + std::to_string(uncaptured - warnings.size()) + " more apps are not recorded", width);
      y += px(kAutoWarningLine);
    }
    y += px(4);
  }
  if (has_members) {
    // The Other group's disclosure: one row, and its members under it in their own short list.
    ImGui::SetCursorScreenPos(ImVec2(start.x, y));
    const float head_h = px(24);
    const bool pressed = ImGui::InvisibleButton("##otherapps", ImVec2(width, head_h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered) draw->AddRectFilled(ImVec2(start.x, y), ImVec2(start.x + width, y + head_h), pal().raised);
    draw_icon(draw, mixer.other_open ? "chevronDown" : "chevronRight",
              ImVec2(start.x + px(2), y + (head_h - px(12)) / 2), px(12), pal().muted, 1.5f);
    draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(start.x + px(18), y + (head_h - fonts().strong->LegacySize) / 2),
                  pal().ink, "OTHER APPS");
    const std::string count = std::to_string(members.items().size());
    const float count_w = text_width(fonts().small, count);
    draw->AddText(fonts().small, fonts().small->LegacySize,
                  ImVec2(start.x + width - px(8) - count_w, y + (head_h - fonts().small->LegacySize) / 2), pal().muted,
                  count.c_str());
    tooltip("Apps without a track of their own share one editable track. Set each app's level here.");
    if (pressed) mixer.other_open = !mixer.other_open;
    y += head_h + px(2);
    if (mixer.other_open) {
      std::vector<std::string> current;
      for (const Json& member : members.items()) current.push_back(audio_exe_key(member["exe"].as_string()));
      mixer.member_order = audio_stable_order(mixer.member_order, current, frozen);
      const int rows = static_cast<int>(mixer.member_order.size());
      const float list_h = std::min(rows, 4) * px(29) + px(4);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, px(3)));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, px(2)));
      ImGui::BeginChild("##othermembers", ImVec2(width, list_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
      wheel_owner_rect(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                                                       ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
      ImGuiListClipper clipper;
      clipper.Begin(rows, px(29));
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          const std::string& key = mixer.member_order[static_cast<size_t>(i)];
          for (const Json& member : members.items()) {
            if (audio_exe_key(member["exe"].as_string()) != key) continue;
            other_member_row(env, member, width);
            break;
          }
        }
      }
      ImGui::EndChild();
      ImGui::PopStyleVar(2);
      y += list_h + px(4);
    }
  } else if (has_other) {
    ImGui::SetCursorScreenPos(ImVec2(start.x, y));
    text_ellipsis(fonts().small, pal().muted, "Other apps appear here when they play sound", width);
    y += px(20);
  }

  // Excluded programs are a preference, not a deletion, so they come back here in one press.
  if (!excluded.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(start.x, y));
    const std::string label =
        "Restore " + std::to_string(excluded.size()) + (excluded.size() == 1 ? " excluded app" : " excluded apps");
    if (button(label.c_str(), Variant::ghost, "undo", false, 0, 24)) store.set_config(audio_restore_excluded_patch(prefs));
    // The label is drawn by the button; the names are behind the pointer.
    std::string names;
    for (const std::string& exe : excluded) names += (names.empty() ? "" : "\n") + program_name(exe);
    tooltip("Bring these apps back:\n" + names);
    y += px(26);
  }
  if (audio_status["auto"]["performance_override"].as_bool()) {
    ImGui::SetCursorScreenPos(ImVec2(start.x, y));
    text_wrapped(fonts().small, pal().muted,
                 "Performance mode is on: app audio is recorded as one mix until it ends. Its lanes return after.",
                 width);
    y += px(32);
  }
  ImGui::SetCursorScreenPos(ImVec2(start.x, y));

  // Holds belong to programs on screen: a lane that left drops its own.
  std::vector<std::string> keep;
  for (const Json& lane : lanes.items()) {
    if (!lane["exe"].as_string().empty()) keep.push_back(lane["exe"].as_string());
  }
  audio_speaker_prune(keep);
  ImGui::Dummy(ImVec2(0, 0));
}

// The expanded mixer's own window: wider than the pane, anchored above it, inside the panel. It is an ImGui
// popup, so an outside click and Escape close it without inventing a second window or a modal.
void draw_audio_mixer(ViewEnv& env, const Json& apps, bool selected_apps, bool performance_on,
                      const std::string& problem, const std::string& game_exe) {
  LiveStrip& live = env.panel.live;
  AudioMixerState& mixer = live.mixer;
  // The automatic experience has its own mixer: lanes rather than a selection, and no picker at all.
  if (audio_is_auto(env.store.config)) {
    draw_auto_mixer(env, ImGui::GetContentRegionAvail().x);
    return;
  }
  const size_t slot_limit = static_cast<size_t>(kAudioAppMax);
  const size_t used = std::min(apps.items().size(), slot_limit);
  const size_t legacy_over = apps.items().size() > slot_limit ? apps.items().size() - slot_limit : 0;
  const bool addable = used < slot_limit && legacy_over == 0;
  // A manual recording refuses a structural app change (it rebuilds the encoder): the actions that would
  // make one are disabled with the reason, while every fader and mute stays live.
  const bool recording_now = env.store.status["recording"].as_bool();
  const bool structural_blocked = recording_now || env.store.status["rebuilding"].as_bool();
  const char* kStopFirst = recording_now ? "Stop the recording first: changing apps rebuilds the encoder."
                                         : "Wait for the encoder to finish rebuilding.";

  if (structural_blocked) mixer.pick = false;
  if (mixer.pick) {
    draw_audio_picker(env, apps, used, slot_limit, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
  } else {
    // Selected apps: an empty slot is a strip-shaped add cell beside the channels, not a row under them.
    const bool add_cell = selected_apps && !performance_on && addable;
    const int strips = (selected_apps && !performance_on ? static_cast<int>(used) + 1 : 2) + (add_cell ? 1 : 0);
    const float strip_gap = px(kMixerChannelGap), strip_h = px(kMixerStripHeight);
    const float strip_w = std::min(px(kMixerStripWidth), (ImGui::GetContentRegionAvail().x - (strips - 1) * strip_gap) / strips);
    const float total = strips * strip_w + (strips - 1) * strip_gap;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float x = start.x + (ImGui::GetContentRegionAvail().x - total) * 0.5f;
    const Json& config = env.store.config;
    const Json& status = env.store.status;
    const Json& audio_status = status["audio"];
    const std::vector<Json>& slot_health = audio_status["slots"].items();
    const bool desktop = config["desktop_enabled"].as_bool(), mic = config["mic_enabled"].as_bool();
    const auto draw_desktop = [&] {
      StripView strip;
      strip.id = "desktop";
      strip.name = "DESKTOP";
      strip.tip = performance_on ? "Desktop audio, recorded by Performance mode" : "All desktop sound";
      strip.volume = static_cast<float>(config["desktop_volume"].as_double(1.0));
      strip.level = env.store.desktop_level;
      strip.enabled = desktop;
      strip.muted = config["desktop_muted"].as_bool();
      strip.state = audio_strip_state(desktop, strip.muted, true, "connected", recording_now);
      expanded_strip(env, strip, strip_w, strip_h);
    };
    const auto draw_mic = [&] {
      StripView strip;
      strip.id = "mic";
      strip.name = "MIC";
      strip.tip = performance_on ? "The microphone, recorded by Performance mode" : "The microphone";
      strip.icon_on = "mic";
      strip.input = true;
      strip.icon_off = "micOff";
      strip.volume = static_cast<float>(config["mic_volume"].as_double(1.0));
      strip.level = env.store.mic_level;
      strip.enabled = mic;
      strip.muted = config["mic_muted"].as_bool();
      strip.state = audio_strip_state(mic, strip.muted, true, "connected", recording_now);
      expanded_strip(env, strip, strip_w, strip_h);
    };
    if (selected_apps && !performance_on) {
      for (size_t i = 0; i < used; ++i) {
        const Json& app = apps.items()[i];
        const bool game = app["kind"].as_string() == kAudioGameKind;
        StripView strip;
        strip.id = "app" + std::to_string(i);
        strip.name = program_name(app["exe"].as_string());
        const std::string title = app["title"].as_string();
        strip.tip = audio_slot_hint(status, i, game, strip.name);
        // The window's own title is worth a line only when it says more than the program's name.
        if (!title.empty() && lower_text(title) != lower_text(strip.name)) strip.tip += "\n" + title;
        strip.activity_exe = i < slot_health.size() && !slot_health[i]["exe"].as_string().empty()
                                 ? slot_health[i]["exe"].as_string()
                                 : app["exe"].as_string();
        strip.volume = static_cast<float>(audio_app_volume(app));
        strip.level = std::clamp(program_peak(mixer.sessions, strip.activity_exe), 0.0f, 1.0f);
        strip.muted = audio_app_muted(app);
        const std::string slot_state = i < slot_health.size() ? slot_health[i]["state"].as_string("waiting") : "waiting";
        strip.state = audio_strip_state(true, strip.muted, audio_status["recorded"].as_bool(true), slot_state,
                                        recording_now);
        strip.structural_blocked = structural_blocked;
        strip.slot = static_cast<int>(i);
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        expanded_strip(env, strip, strip_w, strip_h);
        x += strip_w + strip_gap;
      }
      if (add_cell) {
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        ImGui::BeginDisabled(structural_blocked);
        const bool pressed = ImGui::InvisibleButton("##mixeradd", ImVec2(strip_w, strip_h));
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        const float hover = anim_item(hovered && !structural_blocked ? 1.0f : 0.0f, kRateHover);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 a(x - px(4), start.y - px(6)), b(x + strip_w + px(4), start.y + strip_h + px(4));
        if (hover > 0.0f) draw->AddRectFilled(a, b, mix(pal().surface, pal().raised, hover));
        // A dashed frame: a place for a channel, not a channel.
        const ImU32 edge = mix(pal().line, pal().line_strong, hover);
        const float dash = px(4);
        for (float t = a.x; t < b.x; t += dash * 2) {
          draw->AddLine(ImVec2(t, a.y + 0.5f), ImVec2(std::min(t + dash, b.x), a.y + 0.5f), edge);
          draw->AddLine(ImVec2(t, b.y - 0.5f), ImVec2(std::min(t + dash, b.x), b.y - 0.5f), edge);
        }
        for (float t = a.y; t < b.y; t += dash * 2) {
          draw->AddLine(ImVec2(a.x + 0.5f, t), ImVec2(a.x + 0.5f, std::min(t + dash, b.y)), edge);
          draw->AddLine(ImVec2(b.x - 0.5f, t), ImVec2(b.x - 0.5f, std::min(t + dash, b.y)), edge);
        }
        const ImU32 ink = structural_blocked ? pal().faint : mix(pal().muted, pal().accent, hover);
        const float glyph = px(16), mid_y = start.y + strip_h / 2;
        draw_icon(draw, "plus", ImVec2(x + (strip_w - glyph) / 2, mid_y - glyph - px(6)), glyph, ink);
        const char* add_label = "ADD APP";
        draw->AddText(fonts().strong, fonts().strong->LegacySize,
                      ImVec2(x + (strip_w - text_width(fonts().strong, add_label)) / 2, mid_y + px(2)), ink, add_label);
        const std::string capacity = std::to_string(used) + " / " + std::to_string(slot_limit);
        draw->AddText(fonts().small, fonts().small->LegacySize,
                      ImVec2(x + (strip_w - text_width(fonts().small, capacity)) / 2,
                             mid_y + px(6) + fonts().strong->LegacySize), pal().faint, capacity.c_str());
        tooltip(structural_blocked ? std::string(kStopFirst)
                                   : "Add a running app on its own track (" + capacity + " slots used).");
        if (pressed) {
          mixer.pick = true;
          mixer.pick_replace = -1;
          mixer.search.clear();
          mixer.first = mixer.programs.is_null();
        }
        x += strip_w + strip_gap;
      }
      ImGui::SetCursorScreenPos(ImVec2(x, start.y));
      draw_mic();
    } else {
      ImGui::SetCursorScreenPos(ImVec2(x, start.y));
      draw_desktop();
      ImGui::SetCursorScreenPos(ImVec2(x + strip_w + strip_gap, start.y));
      draw_mic();
    }
    // A line under the strips only when there is one: otherwise the strips are the popup's foot.
    const bool footer = performance_on || (selected_apps && (!addable || problem == "game_missing"));
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + strip_h + (footer ? px(kMixerGap) : 0.0f)));
    if (selected_apps && !performance_on) {
      const float row_w = ImGui::GetContentRegionAvail().x;
      // Only what the add cell cannot say: four apps chosen, or saved apps past the limit.
      if (!addable) {
        text_wrapped(fonts().small, pal().muted,
                     legacy_over > 0 ? "Remove the saved apps that are not recorded first."
                                     : "All 4 app slots are used. Use a strip's swap or X to replace or remove one.", row_w);
      }
      if (problem == "game_missing") {
        text_wrapped(fonts().small, pal().danger, "Game audio is not recorded. Add " +
                     (game_exe.empty() ? std::string("the game") : program_name(game_exe)) + ".", row_w);
      }
    } else if (performance_on) {
      text_wrapped(fonts().small, pal().muted,
                   std::to_string(used) + (used == 1 ? " saved app returns when it ends." : " saved apps return when it ends."),
                   ImGui::GetContentRegionAvail().x);
    }

  }
  // Holds belong to programs on screen: a pruned row does not leave one behind for the session.
  std::vector<std::string> keep;
  for (const Json& app : env.store.config["audio_apps"].items()) keep.push_back(app["exe"].as_string());
  if (mixer.pick) keep.insert(keep.end(), mixer.order.begin(), mixer.order.end());
  audio_speaker_prune(keep);
  // Rows are placed with SetCursorScreenPos: ImGui 1.92 wants an item after the last one, or it draws its
  // "placed cursor" error into the popup. A zero-size dummy closes the layout without adding space.
  ImGui::Dummy(ImVec2(0, 0));
}

void draw_audio(ViewEnv& env, ImVec2 size) {
  begin_pane("audio", "audio", size);
  LiveStrip& live = env.panel.live;
  live.audio_hover = false;
  live.audio_drawn_at = env.now_ms;
  if (env.store.config.is_null()) {
    end_pane();
    return;
  }
  const Json& config = env.store.config;
  const Json apps = config["audio_apps"].is_array() ? config["audio_apps"] : Json::array();
  const std::string mode = config["audio_capture_mode"].as_string("desktop");
  const bool auto_mode = audio_is_auto(config);
  const bool selected_apps = !auto_mode && mode == "apps";
  const bool performance_on = env.store.status["performance"]["active"].as_bool();
  // While Performance mode runs, its own mixed track is what records: the pane shows desktop and microphone
  // (the sources it really uses) and the app strips wait for it to end. In the automatic experience the
  // lanes stay what the engine reports; its own performance_override says if they are one mix instead.
  const bool apps_mode = selected_apps && !performance_on;
  const size_t slot_limit = static_cast<size_t>(kAudioAppMax);
  const size_t used = std::min(apps.items().size(), slot_limit);
  AudioMixerState& mixer = live.mixer;
  // Settings > Audio's "Manage audio" sends the panel back here with its mixer open.
  const bool mixer_available = true;
  bool force_mixer = false;
  if (env.panel.audio_mixer_open) {
    env.panel.audio_mixer_open = false;
    force_mixer = mixer_available;
  }
  // A hand anywhere on this pane holds the lane order still: an app that starts playing must not move the
  // row under the pointer.
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) mixer.activity_at = env.now_ms;
  if (auto_mode) mixer.pick = false;  // the automatic experience has no app picker at all

  // A program's own meter is read only while this pane (or its mixer) shows app sources, on the same 500 ms
  // beat Settings uses. A hidden panel draws nothing and asks for nothing.
  if (auto_mode || apps_mode || mixer.pick) {
    if (env.now_ms - mixer.sessions_at >= 500 && !mixer.sessions_loading) {
      mixer.sessions_at = env.now_ms;
      mixer.sessions_loading = true;
      UiStore& store = env.store;
      AudioMixerState* into = &mixer;
      store.bridge_call("audio.sessions", Json::object(), [&store, into](bool ok, const Json& reply, const std::string&) {
        into->sessions_loading = false;
        if (ok) into->sessions = reply;
        store.dirty = true;
      });
    }
  }

  const ImVec2 area = ImGui::GetContentRegionAvail();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  bool open_mixer = false;
  bool open_picker = false;
  // The way in is explicit and always in the same place: the pane is the overview, the mixer is the depth.
  ImGui::SetCursorScreenPos(ImVec2(at.x, at.y));
  if (mixer_available && button("Mixer##openaudio", Variant::ghost, "expand", false, unpx(area.x), 22)) open_mixer = true;
  const float header = mixer_available ? px(26) : 0.0f;
  const float body_h = area.y - header;

  if (auto_mode) {
    // The automatic overview: one row per lane the engine reports, then the microphone. Nothing is added or
    // chosen here: an app arrives and leaves as Windows says it starts and stops playing.
    const Json& audio_status = env.store.status["audio"];
    const bool capturing = env.store.buffer_on() || env.store.status["recording"].as_bool();
    const AutoAudioState state = audio_auto_state(audio_status, capturing);
    const float row_h = px(28), row_gap = px(2);
    float y = at.y + header;
    if (state == AutoAudioState::active) {
      std::vector<std::string> keys;
      for (const Json& lane : audio_lanes_of(audio_status).items()) keys.push_back(audio_lane_key(lane));
      const bool frozen = env.now_ms - mixer.activity_at < 1500;
      mixer.lane_order = audio_stable_order(mixer.lane_order, keys, frozen);
      for (const std::string& key : mixer.lane_order) {
        Json lane;
        if (!auto_lane(audio_status, key, lane)) continue;
        const bool other = audio_lane_other(lane);
        const std::string exe = lane["exe"].as_string();
        const bool muted = other ? audio_other_muted(config) : lane_muted(config, lane);
        const std::string lane_state = lane["state"].as_string();
        const bool failed = lane_state == "failed";
        const bool connected = lane_state == "connected" || lane_state == "covered";
        const std::string hint =
            (other ? std::string("Apps without a track of their own share this one") : lane_state_text(lane, capturing)) +
            lane_marks(muted);
        ImGui::SetCursorScreenPos(ImVec2(at.x, y));
        const std::string row_id = other ? std::string("lanerow-other") : "lanerow" + audio_exe_key(exe);
        if (compact_audio_row(env, row_id, other ? "Other audio" : audio_lane_title(lane), hint,
                              other ? std::string() : exe, failed, connected, muted, true,
                              other ? 0.0f : std::clamp(program_peak(mixer.sessions, exe), 0.0f, 1.0f), area.x, row_h,
                              [&env, exe, other, muted] {
                                if (other) env.store.set_config(audio_other_muted_patch(!muted));
                                else set_audio_prefs(env, exe, audio_muted_field(!muted));
                              }, "speaker", "speakerOff")) {
          open_mixer = true;
          if (other) mixer.other_open = true;
        }
        y += row_h + row_gap;
      }
    } else {
      const float inset = state == AutoAudioState::waiting
                              ? std::max(0.0f, (area.x - text_width(fonts().small, audio_auto_line(state))) / 2)
                              : 0.0f;
      ImGui::SetCursorScreenPos(ImVec2(at.x + inset, y));
      text_wrapped(fonts().small, pal().muted, audio_auto_line(state), area.x);
      y = ImGui::GetCursorScreenPos().y + px(8);
    }
    // The microphone is always one press away, and always says what it is doing.
    const bool mic = config["mic_enabled"].as_bool(), mic_muted = config["mic_muted"].as_bool();
    ImGui::SetCursorScreenPos(ImVec2(at.x, y));
    if (compact_audio_row(env, "microw", "Microphone", mic ? "Recording the microphone" : "The microphone is off", "",
                          false, mic, mic_muted, mic, env.store.mic_level, area.x, row_h, [&env, mic, mic_muted] {
                            Json patch = Json::object();
                            if (mic) patch.set("mic_muted", Json(!mic_muted));
                            else { patch.set("mic_enabled", Json(true)); patch.set("mic_muted", Json(false)); }
                            env.store.set_config(patch);
                          })) {
      open_mixer = true;
    }
  } else if (apps_mode) {
    const Json& status = env.store.status;
    const Json& audio_status = status["audio"];
    const std::vector<Json>& slot_health = audio_status["slots"].items();
    const float row_h = px(28), row_gap = px(2);
    if (used == 0) {
      // An empty selection is one row-sized add cell where the first app will sit; the microphone stays under it.
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + header));
      if (button("Add app##audioempty", Variant::normal, "plus", false, unpx(area.x), 28)) {
        open_picker = true;
        mixer.pick = true;
        mixer.pick_replace = -1;
        mixer.search.clear();
        mixer.first = mixer.programs.is_null();
      }
      tooltip("Add the apps you want on their own tracks. All sound together is in Settings.");
    } else {
      for (size_t i = 0; i < used; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + header + i * (row_h + row_gap)));
        const Json& app = apps.items()[i];
        const bool game = app["kind"].as_string() == kAudioGameKind;
        const std::string exe = app["exe"].as_string();
        const std::string name = game ? std::string("Current game") : program_name(exe);
        const bool failed = i < slot_health.size() && slot_health[i]["state"].as_string() == "failed";
        const std::string state = i < slot_health.size() ? slot_health[i]["state"].as_string() : std::string();
        const bool connected = state == "connected" || state == "covered";
        const std::string activity = i < slot_health.size() && !slot_health[i]["exe"].as_string().empty()
                                         ? slot_health[i]["exe"].as_string()
                                         : exe;
        const float peak = std::clamp(program_peak(mixer.sessions, activity), 0.0f, 1.0f);
        const Json& entry = apps.items()[i];
        if (compact_audio_row(env, "approw" + std::to_string(i), name, audio_slot_hint(status, i, game, name), activity,
                              failed, connected, audio_app_muted(entry), true, peak, area.x, row_h, [&env, entry, i] {
                                set_audio_apps(env, audio_apps_with_field(env.store.config["audio_apps"], i, "muted",
                                                                         Json(!audio_app_muted(entry))));
                              }, "speaker", "speakerOff")) {
          open_mixer = true;
        }
      }
    }
    const bool mic = config["mic_enabled"].as_bool(), mic_muted = config["mic_muted"].as_bool();
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + header + std::max<size_t>(used, 1) * (row_h + row_gap)));
    if (compact_audio_row(env, "microw", "Microphone", mic ? "Recording the microphone" : "The microphone is off", "",
                          false, mic, mic_muted, mic, env.store.mic_level, area.x, row_h, [&env, mic, mic_muted] {
                            Json patch = Json::object();
                            if (mic) patch.set("mic_muted", Json(!mic_muted));
                            else { patch.set("mic_enabled", Json(true)); patch.set("mic_muted", Json(false)); }
                            env.store.set_config(patch);
                          })) {
      open_mixer = true;
    }
  } else {
    // Desktop mode, and Performance mode's own desktop mix: the two sources that really record, side by side.
    const float gap = px(8), w = std::floor((area.x - gap) / 2);
    const ImVec2 strips_at(at.x, at.y + header);
    ImGui::SetCursorScreenPos(strips_at);
    channel_strip(env, "desktop", "speaker", "outputs", "speaker", "speakerOff", w, body_h);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + w + gap / 2, strips_at.y), ImVec2(at.x + w + gap / 2, strips_at.y + body_h),
                                        pal().line, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(at.x + w + gap, strips_at.y));
    channel_strip(env, "mic", "mic", "inputs", "mic", "micOff", w, body_h);
  }

  // The popup: anchored above the pane and clamped to the panel, so a short window still shows all of it.
  const bool warning = apps_mode && !performance_on && env.store.status["audio"]["problem_steady"].as_bool() &&
                       env.store.status["audio"]["problem"].as_string() == "game_missing";
  int strips = apps_mode ? static_cast<int>(used) + 1 + (!performance_on && used < slot_limit ? 1 : 0) : 2;
  int auto_extra = 0;   // the automatic mixer's own lines under the lanes
  if (auto_mode) {
    int app_lanes = 0, other_rows = 0;
    bool other = false;
    for (const Json& lane : audio_lanes_of(env.store.status["audio"]).items()) {
      if (audio_lane_other(lane)) {
        other = true;
        other_rows = static_cast<int>(lane["apps"].items().size());
      } else {
        ++app_lanes;
      }
    }
    app_lanes = std::min(app_lanes, 3);  // three app lanes, the Other group and the microphone: five channels
    strips = app_lanes + (other ? 1 : 0) + 1;
    const bool capturing = env.store.buffer_on() || env.store.status["recording"].as_bool();
    if (audio_auto_state(env.store.status["audio"], capturing) != AutoAudioState::active) auto_extra += 40;
    if (other_rows > 0) auto_extra += 26;
    if (other_rows > 0 && mixer.other_open) auto_extra += std::min(other_rows, 4) * 29 + 8;
    if (!audio_excluded(config["audio_app_prefs"]).empty()) auto_extra += 26;
    if (env.store.status["audio"]["auto"]["performance_override"].as_bool()) auto_extra += 34;
    auto_extra += static_cast<int>(std::lround(audio_uncaptured_height(
        audio_uncaptured_warnings(env.store.status["audio"], 3).size(), audio_uncaptured_total(env.store.status["audio"]))));
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float margin = px(8);
  const float strips_w = px(2 * kMixerPaddingX + 8) + strips * px(kMixerStripWidth) + std::max(0, strips - 1) * px(kMixerChannelGap);
  const float popup_w = std::min(viewport->Size.x - margin * 2,
      mixer.pick ? px(340) : std::max(px(200), strips_w));
  const std::string warning_text = "Game audio is not recorded. Add " +
      (env.store.status["audio"]["game_exe"].as_string().empty() ? std::string("the game") :
       program_name(env.store.status["audio"]["game_exe"].as_string())) + ".";
  const float warning_h = warning ? fonts().small->CalcTextSizeA(fonts().small->LegacySize, FLT_MAX,
      popup_w - px(24), warning_text.c_str()).y + px(6) : 0.0f;
  const float desired_h = mixer.pick ? px(340)
      : px(kMixerBodyTop + kMixerStripHeight + kMixerPaddingY + (auto_mode ? auto_extra : 0)) + warning_h;
  const float fitted_h = !mixer.pick && mixer.content_h > 0.0f ? mixer.content_h : desired_h;
  const float popup_h = std::min(fitted_h, viewport->Size.y - margin * 2);
  const ImVec2 pane_pos = ImGui::GetWindowPos();
  const ImVec2 pane_size = ImGui::GetWindowSize();
  float px_pos = pane_pos.x + pane_size.x - popup_w;
  float py_pos = pane_pos.y - popup_h - px(6);
  px_pos = std::clamp(px_pos, viewport->Pos.x + margin, std::max(viewport->Pos.x + margin, viewport->Pos.x + viewport->Size.x - popup_w - margin));
  py_pos = std::max(py_pos, viewport->Pos.y + margin);
  if (py_pos + popup_h > viewport->Pos.y + viewport->Size.y - margin)
    py_pos = std::max(viewport->Pos.y + margin, viewport->Pos.y + viewport->Size.y - margin - popup_h);
  if (mixer.reopen && mixer_available) {
    open_mixer = true;
    mixer.reopen = false;
  }
  if (mixer_available && (open_mixer || open_picker || force_mixer)) {
    if (open_picker) {
      mixer.pick = true;
      mixer.pick_replace = -1;
    }
    mixer.first = mixer.programs.is_null();
    ImGui::OpenPopup("##audiomixer");
  }
  if (ImGui::IsPopupOpen("##audiomixer")) {
    ImGui::SetNextWindowPos(ImVec2(px_pos, py_pos), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(popup_w, popup_h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(kMixerPaddingX), px(kMixerPaddingY)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(6), px(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, pal().surface);
    ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
    if (ImGui::BeginPopup("##audiomixer", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
      ImGui::SetScrollY(0.0f);
      if (!mixer_available) { mixer.pick = false; mixer.reopen = false; ImGui::CloseCurrentPopup(); }
      if (mixer_available) {
      const ImVec2 pop = ImGui::GetWindowPos();
      const ImVec2 content = ImGui::GetContentRegionAvail();
      if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        live.audio_hover = true;
        live.mixer.activity_at = env.now_ms;  // a hand in the mixer freezes the lane order
      }
      if (mixer.pick) {
        // The picker is a step inside the mixer: one header, a way back, and the same close as the mixer.
        ImGui::SetCursorScreenPos(ImVec2(pop.x + px(kMixerPaddingX), pop.y + px(kMixerPaddingY)));
        if (icon_button("##pickback", "chevronLeft", "Back to the mixer", false, 22, 12)) mixer.pick = false;
        ImGui::SameLine(0, px(6));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(3));
        text(fonts().strong, pal().ink, mixer.pick_replace >= 0 ? "REPLACE AN APP" : "ADD AN APP");
      } else {
        ImGui::SetCursorScreenPos(ImVec2(pop.x + px(12), pop.y + px(10) + (px(22) - fonts().strong->LegacySize) / 2));
        text(fonts().strong, pal().ink, "AUDIO MIXER");
        ImGui::SetCursorScreenPos(ImVec2(pop.x + px(12) + content.x - px(48), pop.y + px(10)));
        if (icon_button("##audiosettings", "settings", "Audio settings: devices, filters and tracks", false, 22, 13)) {
          env.panel.settings = "audio";
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::SetCursorScreenPos(ImVec2(pop.x + px(12) + content.x - px(22), pop.y + px(10)));
      if (icon_button("##mixerclose", "close", "Close the mixer (Esc)", false, 22, 13)) ImGui::CloseCurrentPopup();
      ImGui::SetCursorScreenPos(ImVec2(pop.x + px(kMixerPaddingX), pop.y + px(kMixerBodyTop)));
      draw_audio_mixer(env, apps, selected_apps, performance_on,
                       warning ? "game_missing" : "", env.store.status["audio"]["game_exe"].as_string());
      // The strips view is sized to what it drew: the next frame's popup has no empty foot.
      if (!mixer.pick) mixer.content_h = ImGui::GetCurrentWindow()->DC.CursorMaxPos.y - pop.y + px(kMixerPaddingY);
      }
      ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
  }
  if (mixer.pick && !ImGui::IsPopupOpen("##audiomixer")) mixer.pick = false;
  if (!auto_mode) {
    std::vector<std::string> keep;
    for (const Json& app : config["audio_apps"].items()) keep.push_back(app["exe"].as_string());
    if (mixer.pick) keep.insert(keep.end(), mixer.order.begin(), mixer.order.end());
    audio_speaker_prune(keep);
  }
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
