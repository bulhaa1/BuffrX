// The tray panel (the quick panel): opens by the tray icon and stays until its minimize or close button, Esc or the tray icon (on top only while pinned). Everything a player wants without
// opening BuffrX, set out as a launcher: the buffer as it fills, a run list worked from the keyboard
// (up and down to move, Enter to run), every save (clips and screenshots, newest first, scrolled), and a hint bar saying
// which keys do what. One window with a 2 px accent frame, as a focused window has on a tiling desktop. The window is moved by its
// header and sized from its edges, about 30% either side of the size it opens at (ui_window_math.h); everything here lays out from
// the width and height it is given.
#include <windows.h>

#include <algorithm>
#include <ctime>

#include "imgui_internal.h"
#include "ui_parts.h"
#include "ui_window_math.h"

namespace bx::ui {

using namespace parts;

namespace {

constexpr float kGap = 8;

struct RunItem {
  const char* id;
  std::string icon, title, detail, tip;
  const char* action;
  bool live;  // drawn in the recording colour while what the row started is running
};

// A paging key's request for the saves' list, in pages (a large number is the top or the bottom); the list applies it on the next frame.
float list_scroll = 0.0f;

// The saves of one tab (0 clips, 1 screenshots), newest first. Pointers into the store's list, rebuilt when a list arrives (a
// thumbnail's reply edits an item in place, so a pointer stays good) - sorting a long library every frame is what a scrolled
// list cannot afford.
int saves_tab = 0;
const std::vector<const UiMedia*>& saves_of(const UiStore& store, int tab) {
  static std::vector<const UiMedia*> lists[2];
  static int versions[2] = {-1, -1};
  static size_t sizes[2] = {0, 0};
  const std::vector<UiMedia>& source = tab == 0 ? store.clips : store.screenshots;
  if (versions[tab] != store.library_version || sizes[tab] != source.size()) {
    lists[tab].clear();
    lists[tab].reserve(source.size());
    for (const UiMedia& item : source)
      if (!shared_item(item)) lists[tab].push_back(&item);  // received files live under Shared, in the panel
    std::stable_sort(lists[tab].begin(), lists[tab].end(), [](const UiMedia* a, const UiMedia* b) { return a->mtime > b->mtime; });
    versions[tab] = store.library_version;
    sizes[tab] = source.size();
  }
  return lists[tab];
}

// Whether the library holds anything a link brought in for this tab: the empty list then has a better
// sentence than "nothing saved yet", because the saves exist, under Shared in the panel.
bool has_shared(const UiStore& store, int tab) {
  const std::vector<UiMedia>& source = tab == 0 ? store.clips : store.screenshots;
  for (const UiMedia& item : source)
    if (shared_item(item)) return true;
  return false;
}

// The keyboard's place: 0 the run list, 1 the saves. Tab moves between them; the focused one wears the accent border.
int zone = 0;
// The saves' row the keys are on, and whether it just moved (the list then scrolls to it).
int saves_cursor = 0;
bool cursor_moved = false;

// A save plays in the viewer of BuffrX itself (never the machine's default player): in the main panel's preview, the same
// place a screenshot opens.
void open_save(ViewEnv& env, const UiMedia& item) {
  // The viewer pages through what the list could show, so a left or right never lands on a hidden shared file.
  std::vector<UiMedia> list;
  for (const UiMedia& save : item.clip ? env.store.clips : env.store.screenshots)
    if (!shared_item(save)) list.push_back(save);
  open_preview(env, item, list);
  env.panel.preview_from_compact = true;
  env.shell.show("main");
}

// One save: its picture (a click plays it), what it is and when, and the things to do with it. `w` is the row's width.
void recent_row(ViewEnv& env, const UiMedia& item, int index, float w, float row_h, int64_t now) {
  UiStore& store = env.store;
  const Palette& p = pal();
  ImGui::PushID(item.file.c_str());
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const bool current = zone == 1 && index == saves_cursor;
  // The hover lights the whole row, so the words and the gap beside the picture read as part of the same click target.
  const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, ImVec2(min.x + w, min.y + row_h));
  if (current || hovered) {
    draw->AddRectFilled(ImVec2(min.x - px(4), min.y - px(3)), ImVec2(min.x + w + px(2), min.y + row_h + px(3)), p.raised);
    if (cursor_moved) {
      ImGui::SetScrollHereY(0.5f);
      cursor_moved = false;
    }
  }
  // The picture gives way before the words do when the window is narrow.
  const ImVec2 thumb_size(std::round(std::clamp(w * 0.3f, px(50), px(62))), row_h);
  // The whole row plays it, not the square alone. The buttons on the right are submitted over this one and take the click
  // where they sit.
  ImGui::SetNextItemAllowOverlap();
  if (ImGui::InvisibleButton("play", ImVec2(w, row_h))) {
    zone = 1;
    saves_cursor = index;
    open_save(env, item);
  }
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  tooltip("Play");
  thumbnail(env, item, min, thumb_size, 0);
  const bool lit = hovered || current || item.file == store.newest;
  const float lit_t = anim_to(ImGui::GetID("lit"), lit ? 1.0f : 0.0f, kRateState);
  draw->AddRect(min, ImVec2(min.x + thumb_size.x, min.y + thumb_size.y), mix(p.line, p.accent, lit_t), 0, 0, 1.0f + (px(2) - 1.0f) * lit_t);

  const float buttons = px(72);
  const float text_x = min.x + thumb_size.x + px(8);
  const float text_w = std::max(px(24), min.x + w - buttons - px(6) - text_x);
  // The two lines sit centred against the picture; a narrow window keeps the time whole and drops the size, never half a number.
  const float block = fonts().label->LegacySize + ImGui::GetStyle().ItemSpacing.y + fonts().tiny->LegacySize;
  ImGui::SetCursorScreenPos(ImVec2(text_x, min.y + std::floor((row_h - block) / 2)));
  ImGui::BeginGroup();
  // The name is the row's own: a rename shows here and on the panel's cards, not only in a tooltip.
  const std::string title = (item.clip ? format_duration(item.duration) + " " : std::string()) + item.name;
  text_ellipsis(fonts().label, p.ink, title, text_w);
  const std::string when = format_when(item.mtime, now);
  const std::string meta = when + " \xC2\xB7 " + format_bytes(static_cast<double>(item.size));
  text_ellipsis(fonts().tiny, p.muted, text_width(fonts().tiny, meta) <= text_w ? meta : when, text_w);
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(ImVec2(min.x + w - buttons, min.y + (row_h - px(24)) / 2));
  if (icon_button("##edit", "scissors", "Edit (E)", false, 24, 12)) open_editor(env, {item.file});
  ImGui::SameLine(0, 0);
  // The sheet needs room for its picture and buttons, so it opens in the panel; only one window exists at a time, and the
  // panel draws it.
  if (icon_button("##share", "share", "Share (S)", false, 24, 12)) open_share(env, item);
  ImGui::SameLine(0, 0);
  if (icon_button("##reveal", "folder", "Show in folder", false, 24, 12))
    run_media_action(env, "reveal", item, item.clip ? store.clips : store.screenshots);
  // The row is one item to the list's clipper, whatever was placed inside it.
  ImGui::SetCursorScreenPos(min);
  ImGui::Dummy(ImVec2(w, row_h));
  ImGui::PopID();
}

// The hint bar's keys, the focused part's own, most useful first. They are laid out at the width the window has, so a resize wraps
// them instead of leaving the last ones out.
struct TrayHint {
  const char* key;
  const char* word;
};
const TrayHint kRunHints[] = {{"tab", "next"}, {"\xE2\x86\x91\xE2\x86\x93", "move"}, {"enter", "select"},
                              {"b", "buffer"}, {"m", "mic"}};
const TrayHint kBufferHints[] = {{"tab", "next"}, {"enter", "select"}, {"b", "buffer"}};
const TrayHint kSaveHints[] = {{"tab", "next"}, {"\xE2\x86\x91\xE2\x86\x93", "move"}, {"\xE2\x86\x90\xE2\x86\x92", "tabs"},
                               {"enter", "select"}, {"e", "edit"}, {"s", "share"}};
constexpr int kRunHintCount = static_cast<int>(sizeof(kRunHints) / sizeof(TrayHint));
constexpr int kBufferHintCount = static_cast<int>(sizeof(kBufferHints) / sizeof(TrayHint));
constexpr int kSaveHintCount = static_cast<int>(sizeof(kSaveHints) / sizeof(TrayHint));

float hint_key_width(const TrayHint& hint) {
  return text_width(fonts().strong, hint.key) * fonts().small->LegacySize / fonts().strong->LegacySize;
}
float hint_width(const TrayHint& hint) { return hint_key_width(hint) + px(5) + text_width(fonts().small, hint.word); }
// The gap between hints at this width: the widest list always stays on one line, so a narrow window closes the gaps (to a floor)
// instead of wrapping or dropping a hint.
float hint_gap(const TrayHint* hints, int count, float available) {
  float sum = 0;
  for (int i = 0; i < count; ++i) sum += hint_width(hints[i]);
  const float gaps = static_cast<float>(count > 1 ? count - 1 : 1);
  return ImClamp((available - sum) / gaps, px(2), px(6));
}

}  // namespace

void draw_tray(ViewEnv& env) {
  UiStore& store = env.store;
  const Json& status = store.status;
  timeline_notices(store);  // a writer state change is said once, by whichever window is open
  const Palette& p = pal();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float frame = px(2);
  draw->AddRectFilled(ImVec2(0, 0), display, p.canvas);
  draw->AddRect(ImVec2(frame / 2, frame / 2), ImVec2(display.x - frame / 2, display.y - frame / 2), p.accent, 0, 0, frame);
  const float gap = px(kGap), side = frame + gap;
  const float inner_w = display.x - side * 2;
  // A short window tightens the run list and the gaps between panes, so the saves keep room to scroll in.
  const bool tight = display.y < px(590);
  const float pane_gap = tight ? px(6) : gap;

  const std::string state = !store.connected ? "disconnected" : status["buffer_state"].as_string("off");
  const bool on = state == "on";

  // Header. The empty part of it drags the window; the buttons do not.
  const float header = px(34);
  env.shell.drag_area(ImVec2(0, 0), ImVec2(display.x - px(128), header));
  // The icon alone: what the buffer is doing is in its own pane, and a status word up here only said it twice.
  draw_app_icon(draw, ImVec2(side + px(15), frame + header / 2), px(15));
  const bool recording = status["recording"].as_bool();
  // The microphone's mute, beside the buffer's switch while something is capturing (M from the keyboard).
  const bool mic_enabled = store.config["mic_enabled"].as_bool(), mic_muted = store.config["mic_muted"].as_bool();
  const bool mic_silent = !mic_enabled || mic_muted;
  const auto toggle_mic = [&] {
    Json patch = Json::object();
    if (mic_enabled) {
      patch.set("mic_muted", Json(!mic_muted));
    } else {
      patch.set("mic_enabled", Json(true));
      patch.set("mic_muted", Json(false));
    }
    store.set_config(patch);
  };
  // Pin, expand, minimize to the tray, close: the quick panel is an ordinary window until it is pinned.
  const std::string update_text = update_state_text(env);
  const bool update_offered = !update_text.empty();
  if (update_offered) {
    draw->AddText(fonts().label, fonts().label->LegacySize,
                  ImVec2(side + px(32), frame + (header - fonts().label->LegacySize) / 2), p.accent,
                  update_text.c_str());
  }
  ImGui::SetCursorScreenPos(ImVec2(display.x - side - px(112), frame + (header - px(28)) / 2));
  if (icon_button("##pin", "pin", env.shell.pinned() ? "Unpin: let other windows cover it" : "Pin on top of other windows (steps aside for a fullscreen game)",
                  env.shell.pinned(), 28, 14)) {
    env.shell.set_pinned(!env.shell.pinned());
  }
  env.shell.no_drag(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  ImGui::SameLine(0, 0);
  if (icon_button("##open", "expand", "Expand BuffrX", false, 28, 14)) {
    env.shell.show("main");
    env.shell.close_window();
  }
  env.shell.no_drag(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  ImGui::SameLine(0, 0);
  if (icon_button("##minimise", "minus", "Minimize to the tray", false, 28, 14)) env.shell.close_window();
  env.shell.no_drag(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  ImGui::SameLine(0, 0);
  if (icon_button("##close", "close", "Close (Esc)", false, 28, 14, true)) env.shell.close_window();
  env.shell.no_drag(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

  // The hint bar is one row at every width: the gaps close before anything wraps or drops.
  const float hints_x = side + px(2);
  // The update badge sits at the far right of this bar; the hints stop short of it instead of running under it.
  const float hints_right = display.x - side - px(2) - (update_offered ? px(30) : 0.0f);
  const float hints_h = px(26);
  ImGui::SetCursorScreenPos(ImVec2(side, frame + header + px(2)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), pane_gap));
  // A scrollbar for a work area shorter than the panes need: 470 design units are 1128 pixels at 200%, more than a laptop's
  // work area has. Without it the last panes were simply clipped. The wheel does not move it (the saves' list is what scrolls).
  ImGui::BeginChild("##trayscroll", ImVec2(inner_w, display.y - frame * 2 - header - px(2) - hints_h - gap), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::Dummy(ImVec2(0, px(2)));

  // The buffer: the seconds, the switch, the ring filling in cells.
  {
    const std::string codec = lower_ascii(codec_name(store.config["codec"].as_string("h264")));
    const std::string aside = on ? codec + " \xC2\xB7 " + std::to_string(status["capture"]["fps"].as_int()) + " fps" : std::string();
    begin_pane("##buffer", "buffer", ImVec2(-1, 0), zone == 2, aside);
    const float w = ImGui::GetContentRegionAvail().x;
    const double filled = status["stats"]["ring"]["filled"].as_double();
    const double seconds = store.config["buffer_seconds"].as_double(status["stats"]["ring"]["seconds"].as_double(60));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* pane = ImGui::GetWindowDrawList();
    const std::string big = on ? format_span(filled) : state == "starting" ? "..." : "off";
    // The switch as two cells, on and off, the live one filled; the seconds end where it begins.
    const float cell_h = px(22);
    const float on_w = text_width(fonts().label, "on") + px(16), off_w = text_width(fonts().label, "off") + px(16);
    const ImVec2 min(at.x + w - on_w - off_w, at.y + (fonts().big->LegacySize - cell_h) / 2);
    const bool show_mic = on || recording;
    pane->PushClipRect(at, ImVec2(min.x - px(show_mic ? 42 : 6), at.y + fonts().big->LegacySize + px(6)), true);
    pane->AddText(fonts().big, fonts().big->LegacySize, at, on ? p.ink : p.muted, big.c_str());
    const std::string of = on ? "/ " + format_span(seconds) : "";
    pane->AddText(fonts().body, fonts().body->LegacySize,
                  ImVec2(at.x + text_width(fonts().big, big) + px(8), at.y + fonts().big->LegacySize - fonts().body->LegacySize - px(4)),
                  p.muted, of.c_str());
    pane->PopClipRect();
    if (show_mic) {
      ImGui::SetCursorScreenPos(ImVec2(min.x - px(36), min.y + (cell_h - px(28)) / 2));
      if (icon_button("##mic", mic_silent ? "micOff" : "mic",
                      !mic_enabled ? "Turn the microphone on (M)" : mic_muted ? "Unmute the microphone (M)" : "Mute the microphone (M)",
                      !mic_silent, 28, 15)) {
        toggle_mic();
      }
    }
    {
      ImGui::SetCursorScreenPos(min);
      const bool busy = !store.connected || state == "starting" || state == "stopping";
      ImGui::BeginDisabled(busy);
      const bool pressed = ImGui::InvisibleButton("##switch", ImVec2(on_w + off_w, cell_h));
      const bool hovered = ImGui::IsItemHovered();
      ImGui::EndDisabled();
      tooltip(on ? "Stop the buffer" : "Start the buffer");
      const bool lit = on || state == "starting";
      pane->AddRectFilled(min, ImVec2(min.x + on_w, min.y + cell_h), lit ? p.accent : 0);
      pane->AddRectFilled(ImVec2(min.x + on_w, min.y), ImVec2(min.x + on_w + off_w, min.y + cell_h), lit ? 0 : p.raised);
      pane->AddRect(min, ImVec2(min.x + on_w + off_w, min.y + cell_h), hovered ? p.muted : p.line);
      pane->AddText(fonts().label, fonts().label->LegacySize, ImVec2(min.x + px(8), min.y + (cell_h - fonts().label->LegacySize) / 2),
                    lit ? p.on_accent : p.muted, "on");
      pane->AddText(fonts().label, fonts().label->LegacySize,
                    ImVec2(min.x + on_w + px(8), min.y + (cell_h - fonts().label->LegacySize) / 2), lit ? p.muted : p.ink, "off");
      if (pressed && !busy) toggle_buffer(env);
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + fonts().big->LegacySize + px(8)));
    cells(unpx(w), on && seconds > 0 ? static_cast<float>(filled / seconds) : 0.0f, 40, 9);
    end_pane();
  }

  // The run list: the selected row is filled with the accent; arrows move it, Enter runs it.
  // The ticker follows the standalone recording, independently of Timeline.
  const double running = recording ? status["recording_seconds"].as_double() : 0.0;
  const int item_count = 5;
  const RunItem items[item_count] = {
      {"save", "save", "Save clip", "", "", "save_replay", false},
      {"quick", "history", "Quick clip", "", "", "quick_clip", false},
      {"shot", "camera", "Screenshot", "", "", "screenshot", false},
      {"record", record_icon(recording), record_label(recording),
       recording ? format_duration(running) : "", record_hint(recording),
       "toggle_record", recording},
      {"timeline", "film", "Timeline", "", "", "view_clip", false}};
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID selected_id = ImGui::GetID("##selected");
  int selected = storage->GetInt(selected_id, 0);
  auto run = [&](int i) {
    if (i == 0) save_clip(env);
    else if (i == 1) quick_clip(env);
    else if (i == 2) screenshot(env);
    else if (i == 3) toggle_recording(env);
    else timeline_action(env);
  };
  {
    begin_pane("##run", "run", ImVec2(-1, 0), zone == 0);
    const float w = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < item_count; ++i) {
      const RunItem& item = items[i];
      ImGui::PushID(item.id);
      const ImVec2 min = ImGui::GetCursorScreenPos();
      const float h = tight ? px(27) : px(32);
      const bool pressed = ImGui::InvisibleButton("row", ImVec2(w, h));
      const float press = press_t();
      if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y > 0) {
        selected = i;
      }
      if (!item.tip.empty()) tooltip(item.tip);
      ImGui::PopID();
      ImDrawList* pane = ImGui::GetWindowDrawList();
      // The accent row is where Enter lands, so it shows only while the run list has the keyboard; otherwise a hovered row is raised.
      const bool sel = zone == 0 && selected == i;
      // The accent follows the keyboard from row to row and a hovered row warms up, instead of both switching on in one frame.
      ImGui::PushID(item.id);
      const float sel_t = tween_to(ImGui::GetID("sel"), sel ? 1.0f : 0.0f, 0.070f);
      const float hov_t = tween_to(ImGui::GetID("hov"), ImGui::IsMouseHoveringRect(min, ImVec2(min.x + w, min.y + h)) ? 1.0f : 0.0f, 0.070f);
      ImGui::PopID();
      if (hov_t > 0.0f && sel_t < 1.0f) pane->AddRectFilled(min, ImVec2(min.x + w, min.y + h), with_alpha(p.raised, hov_t * (1.0f - sel_t)));
      if (sel_t > 0.0f) pane->AddRectFilled(min, ImVec2(min.x + w, min.y + h), with_alpha(p.accent, sel_t));
      const ImU32 fg = mix(item.live ? p.rec : p.ink, p.on_accent, sel_t);
      const float glyph = px(15);
      const ImVec2 icon(min.x + px(8) + px(1) * press, min.y + (h - glyph) / 2 + px(1) * press);
      if (i == 0 && draw_save_feedback(env, pane, icon, glyph, fg)) {
      } else if (item.icon == "record") {
        // BuffrX's own recording red, the mark's own, in every theme: filled while something records, a hollow box otherwise.
        const ImVec2 corner(icon.x + px(3), icon.y + px(3));
        const ImVec2 edge(icon.x + glyph - px(3), icon.y + glyph - px(3));
        const ImU32 red = sel ? p.on_accent : kRecordRed;
        if (item.live) pane->AddRectFilled(corner, edge, red);
        else pane->AddRect(corner, edge, red, 0.0f, 0, 1.7f);
      } else {
        draw_icon(pane, item.icon, icon, glyph, fg, 1.7f, item.icon == "stop" || item.icon == "play");
      }
      const float tx = min.x + px(32);
      const std::string chord = store.chord(item.action);
      // The key caps are the point of the row and are never dropped; the detail (a running recording's time) shows only when the
      // title, the detail and the caps all fit. Nothing is cut in half.
      const float cw = chord.empty() ? 0.0f : chord_width(chord) + px(12);
      const float title_w = text_width(fonts().strong, item.title);
      const float detail_w = item.detail.empty() ? 0.0f : px(8) + text_width(fonts().small, "\xC2\xB7 " + item.detail);
      const float room = w - px(32) - px(6);
      const bool caps = cw > 0 && title_w + cw <= room;
      const bool detail = detail_w > 0 && title_w + detail_w + (caps ? cw : 0.0f) <= room;
      pane->PushClipRect(ImVec2(tx, min.y), ImVec2(min.x + w - (caps ? cw : px(6)), min.y + h), true);
      pane->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(tx, min.y + (h - fonts().strong->LegacySize) / 2), fg, item.title.c_str());
      if (detail) {
        pane->AddText(fonts().small, fonts().small->LegacySize,
                      ImVec2(tx + title_w + px(8), min.y + (h - fonts().small->LegacySize) / 2),
                      sel ? with_alpha(p.on_accent, 0.72f) : p.muted, ("\xC2\xB7 " + item.detail).c_str());
      }
      pane->PopClipRect();
      if (caps) {
        ImGui::SetCursorScreenPos(ImVec2(min.x + w - cw + px(6), min.y + (h - px(20)) / 2));
        chord_button("##cap", chord, "", false, 20, sel);
      }
      ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + h + px(1)));
      if (pressed) {
        zone = 0;
        selected = i;
        run(i);
      }
    }
    end_pane();
  }

  // Every save, newest first. The list is its own scrolling child, so the wheel moves it and nothing else; only the rows on screen
  // are drawn and asked for a picture.
  const std::vector<const UiMedia*>* recent_ptr = nullptr;
  {
    const float remaining = std::max(px(96), ImGui::GetContentRegionAvail().y - px(1));
    begin_pane("##recent", "recent", ImVec2(-1, remaining), zone == 1);
    // Clips and screenshots apart, each with its count, in the smallest cells the panel has; "Shots" when the window is too
    // narrow for the whole word.
    {
      const std::string clips = "Clips " + std::to_string(saves_of(store, 0).size());
      std::string shots = "Screenshots " + std::to_string(saves_of(store, 1).size());
      const float avail = ImGui::GetContentRegionAvail().x;
      if (text_width(fonts().small, clips) + text_width(fonts().small, shots) + px(32) > avail) {
        shots = "Shots " + std::to_string(saves_of(store, 1).size());
      }
      int chosen = saves_tab;
      if (segmented("##savestab", {clips, shots}, chosen, 0.0f, 24, fonts().small, 8) && chosen != saves_tab) {
        saves_tab = chosen;
        zone = 1;
        saves_cursor = 0;
        list_scroll = -1000.0f;  // each tab opens at its newest
      }
      ImGui::Dummy(ImVec2(0, px(2)));
    }
    const auto& recent = saves_of(store, saves_tab);
    recent_ptr = &recent;
    int tab_direction = 1;
    const float tab_t = tab_arrival("##recent_transition", saves_tab, tab_direction);
    const ImVec2 area = ImGui::GetContentRegionAvail();
    if (recent.empty()) {
      ImDrawList* tab_draw = ImGui::GetWindowDrawList();
      const int tab_vertex = tab_draw->VtxBuffer.Size;
      const std::string message =
          has_shared(store, saves_tab)
              ? (saves_tab == 0 ? "Received clips are under Shared in the panel." : "Received screenshots are under Shared in the panel.")
              : (saves_tab == 0 ? "No clips yet. Save clip keeps the last moments." : "No screenshots yet. Screenshot takes one.");
      text_wrapped(fonts().small, p.muted, message, area.x);
      fade_since(tab_draw, tab_vertex, tab_t);
      shift_since(tab_draw, tab_vertex, px(6) * (1.0f - tab_t) * tab_direction, 0.0f);
    } else {
      const float row_h = px(40);
      const int64_t now = static_cast<int64_t>(std::time(nullptr));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::BeginChild("##recentlist", ImVec2(0, area.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar();
      smooth_scroll(px(72), true, 32.0f);
      ImDrawList* tab_draw = ImGui::GetWindowDrawList();
      const int tab_vertex = tab_draw->VtxBuffer.Size;
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(4)));
      // Page Up, Page Down, Home and End (read below, once nothing else wants the keys) move the list a page at a time.
      if (list_scroll != 0.0f) {
        scroll_to_glide(ImGui::GetScrollY() + list_scroll * ImGui::GetWindowHeight() * 0.9f);
        list_scroll = 0.0f;
      }
      const float w = ImGui::GetContentRegionAvail().x;
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(recent.size()), row_h + ImGui::GetStyle().ItemSpacing.y);
      // The row the keys are on is always drawn, so the list can scroll to it from anywhere.
      saves_cursor = std::clamp(saves_cursor, 0, static_cast<int>(recent.size()) - 1);
      if (cursor_moved) clipper.IncludeItemByIndex(saves_cursor);
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          recent_row(env, *recent[static_cast<size_t>(i)], i, w, row_h, now);
        }
      }
      ImGui::PopStyleVar();
      fade_since(tab_draw, tab_vertex, tab_t);
      shift_since(tab_draw, tab_vertex, px(6) * (1.0f - tab_t) * tab_direction, 0.0f);
      ImGui::EndChild();
    }
    end_pane();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // The keys, read only while nothing else wants them (a popup, a text field). Tab (Shift+Tab goes back) moves between the run list, the
  // saves and the buffer's switch (the focused one has the accent border); each has its own keys, and Enter does the obvious thing in
  // it. B and M flip the buffer and the mic from anywhere. The hint bar says which keys the focused part answers to.
  const auto& recent = *recent_ptr;
  const int saves_count = static_cast<int>(recent.size());
  if (!env.panel.buffer_stop_open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !ImGui::GetIO().WantTextInput) {
    const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key); };
    if (pressed(ImGuiKey_Tab)) {
      zone = (zone + (ImGui::GetIO().KeyShift ? 2 : 1)) % 3;  // run, saves, buffer
      if (zone == 1) cursor_moved = true;  // arriving brings the list to its row
    }
    if (zone == 0) {
      if (pressed(ImGuiKey_DownArrow)) selected = (selected + 1) % item_count;
      if (pressed(ImGuiKey_UpArrow)) selected = (selected + item_count - 1) % item_count;
      if (pressed(ImGuiKey_Home)) selected = 0;
      if (pressed(ImGuiKey_End)) selected = item_count - 1;
      if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Space)) run(selected);
    } else if (zone == 2) {
      // The buffer's switch: Enter, Space or either arrow flips it (B does from anywhere).
      if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Space) || pressed(ImGuiKey_LeftArrow) ||
          pressed(ImGuiKey_RightArrow)) {
        toggle_buffer(env);
      }
    } else {
      const auto move = [&](int to) {
        if (saves_count == 0) return;
        saves_cursor = std::clamp(to, 0, saves_count - 1);
        cursor_moved = true;
      };
      if (pressed(ImGuiKey_DownArrow)) move(saves_cursor + 1);
      if (pressed(ImGuiKey_UpArrow)) move(saves_cursor - 1);
      if (pressed(ImGuiKey_PageDown)) move(saves_cursor + 4);
      if (pressed(ImGuiKey_PageUp)) move(saves_cursor - 4);
      if (pressed(ImGuiKey_Home)) move(0);
      if (pressed(ImGuiKey_End)) move(saves_count - 1);
      // Left and Right change tab, from either side of it.
      if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_RightArrow)) {
        saves_tab = pressed(ImGuiKey_RightArrow) ? 1 : 0;
        anim_touch();  // The keyboard changes tabs after the list was drawn.
        saves_cursor = 0;
        list_scroll = -1000.0f;
      }
    }
    // E and S act on the row the keys are on in the saves, and on the newest from the run list.
    const UiMedia* target = saves_count == 0 ? nullptr : recent[static_cast<size_t>(zone == 1 ? saves_cursor : 0)];
    if (zone == 1 && target && (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Space))) open_save(env, *target);
    if (target && pressed(ImGuiKey_E)) open_editor(env, {target->file});
    if (target && pressed(ImGuiKey_S)) open_share(env, *target);
    if (pressed(ImGuiKey_B)) toggle_buffer(env);
    if (pressed(ImGuiKey_M) && (on || recording)) toggle_mic();
    if (pressed(ImGuiKey_Escape)) env.shell.close_window();
  }
  storage->SetInt(selected_id, selected);

  // The hint bar: which keys do what.
  const float top = display.y - frame - hints_h;
  draw->AddRectFilled(ImVec2(frame, top), ImVec2(display.x - frame, display.y - frame), p.surface);
  draw->AddLine(ImVec2(frame, top), ImVec2(display.x - frame, top), p.line);
  // The keys of the focused part, most useful first, on one line at every width.
  const TrayHint* hints = zone == 0 ? kRunHints : zone == 2 ? kBufferHints : kSaveHints;
  const int hint_count = zone == 0 ? kRunHintCount : zone == 2 ? kBufferHintCount : kSaveHintCount;
  const float hint_step = hint_gap(hints, hint_count, hints_right - hints_x);
  const float ty = top + (hints_h - fonts().small->LegacySize) / 2;
  float x = hints_x;
  for (int h = 0; h < hint_count; ++h) {
    const TrayHint& hint = hints[h];
    draw->AddText(fonts().strong, fonts().small->LegacySize, ImVec2(x, ty), p.accent, hint.key);
    x += hint_key_width(hint) + px(5);
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(x, ty), p.muted, hint.word);
    x += text_width(fonts().small, hint.word) + hint_step;
  }
  // The update badge lives in the bottom-right corner of the quick panel, so it stays put at every size and
  // never competes with the window buttons.
  if (update_offered) {
    const float badge = px(22);
    ImGui::SetCursorScreenPos(ImVec2(display.x - side - badge, top + (hints_h - badge) / 2));
    update_badge(env, true);
  }
  // The share sheet and the other dialogs open here, over the quick panel: no need to expand it first.
  media_dialogs(env);
  draw_toasts(store, ImVec2(display.x - side, top - gap), std::min(300.0f, unpx(inner_w)));
}

}  // namespace bx::ui
