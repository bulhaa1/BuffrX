// The panel: Rewind's two columns (the buffer, actions and options on the left, the library on the
// right) over one status strip, Settings in the same window, the screenshot preview. Every area is a
// pane on the ground, with the gaps between them showing; the buffer's pane holds the focus border.
// The quick panel is the tray panel: there is one small interface, not two.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>

#include "../themes.h"
#include "imgui_internal.h"
#include "ui_widgets.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui_parts.h"
#include "ui_window_math.h"

namespace bx::ui {

void draw_settings(ViewEnv& env, float height);  // ui_settings.cpp
void draw_live_timeline(ViewEnv& env);           // ui_live.cpp
void draw_history(ViewEnv& env, ImVec2 size);    // ui_live.cpp
void draw_audio(ViewEnv& env, ImVec2 size);      // ui_live.cpp

namespace {

using namespace parts;

int64_t now_unix() { return static_cast<int64_t>(std::time(nullptr)); }

constexpr float kGap = 8;  // between panes, and between the panes and the window's edge

// ---------------------------------------------------------------- the deck

// One row of the run list: an icon, the action, what it does, and its chord at the right (which
// rebinds). The primary action is the selected row: filled with the accent.
void action_row(ViewEnv& env, const char* id, const std::string& icon, const std::string& title, const std::string& detail,
                const std::string& tone, bool busy, const std::string& hotkey_action, const std::function<void()>& run,
                bool focused) {
  const Palette& p = pal();
  ImGui::PushID(id);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = px(36);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const bool primary = tone == "primary", recording = tone == "record";
  // Every row is a box of its own, the way the reference sets them: a filled one for the main action, an outline for
  // the rest.
  // A code-less row has no chord to draw, and no "set" cap that would open the recorder for one.
  // The chord in use: the keyboard's, or the pad's when only that is set, so a pad-only binding does not
  // read as an empty one. The chip opens the recorder on the input it shows; the modal can swap it.
  const std::string key_chord = hotkey_action.empty() ? std::string() : env.store.chord(hotkey_action);
  const std::string pad_chord = hotkey_action.empty() ? std::string() : env.store.chord(hotkey_action, true);
  const bool pad_only = key_chord.empty() && !pad_chord.empty();
  const std::string chord = pad_only ? pad_chord.substr(4) : key_chord;
  const float chord_w = hotkey_action.empty() ? 0.0f : chord_width(chord);
  const float button_w = w - chord_w - px(18);
  ImGui::BeginDisabled(busy);
  const bool pressed = ImGui::InvisibleButton("row", ImVec2(button_w, h));
  const bool hovered = ImGui::IsItemHovered();
  const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  // B1: a pressed row sinks a pixel and shades while it is held (press_t); the release settles.
  const float press = press_t();
  ImGui::EndDisabled();
  const ImVec2 max(pos.x + w, pos.y + h);
  if (press > 0.0f) draw->AddRectFilled(pos, max, with_alpha(p.canvas, 0.22f * press));
  if (primary) {
    draw->AddRectFilled(pos, max, mix(p.accent, mix(p.accent, p.ink, 0.22f), hover));
  } else {
    // The hover is a clear step up: the row fills with a wash of the accent, its frame turns accent, and a bar grows in at its left.
    if (hover > 0.0f) draw->AddRectFilled(pos, max, mix(p.surface, mix(p.raised, p.accent, 0.14f), hover));
    draw->AddRect(pos, max, mix(p.line_strong, p.accent, hover), 0.0f, 0, px(1.5f));
    if (hover > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + px(3) * hover, max.y), p.accent);
  }
  if (primary && button_w > px(120)) {
    // Three leaning bars, drawn whole and cut by the row's own edges, in the on-accent ink so they read on every theme.
    const float lean = h * 0.5f, bar = px(5), step = px(11);
    const float right = pos.x + button_w - px(30);
    const ImU32 stripe = with_alpha(p.on_accent, 0.26f + 0.2f * hover);
    draw->PushClipRect(pos, ImVec2(pos.x + button_w, max.y), true);
    for (int i = 0; i < 3; ++i) {
      const float x = right - step * static_cast<float>(i);
      draw->AddQuadFilled(ImVec2(x + lean, pos.y), ImVec2(x + lean + bar, pos.y), ImVec2(x + bar, max.y), ImVec2(x, max.y), stripe);
    }
    draw->PopClipRect();
  }
  // The keyboard's row wears the panes' accent border, so Tab says where Enter will land.
  if (focused) draw->AddRect(pos, ImVec2(pos.x + w, pos.y + h), p.accent, 0.0f, 0, 2.0f);
  const ImU32 fg = primary ? p.on_accent : recording ? p.rec : p.ink;
  const ImU32 sub = primary ? with_alpha(p.on_accent, 0.72f) : p.muted;
  const float glyph = px(16);
  const ImVec2 icon_at(pos.x + px(10) + px(1) * press, pos.y + (h - glyph) / 2 + px(1) * press);
  // When the action is done the icon reads as a check and fades back after 900 ms.
  static std::unordered_map<ImGuiID, bool> row_busy;
  static std::unordered_map<ImGuiID, double> row_done;
  const ImGuiID row_id = ImGui::GetID("row");
  const double now_s = anim_detail::wall_seconds();
  bool& was_busy = row_busy[row_id];
  if (was_busy && !busy) row_done[row_id] = now_s;
  was_busy = busy;
  const auto done_it = row_done.find(row_id);
  const double since_done = done_it == row_done.end() ? 10.0 : now_s - done_it->second;
  const bool checked = !busy && since_done < 0.9;
  if (busy && !primary) {
    const float t = static_cast<float>(std::fmod(now_s * 1.2, 1.0));
    const float bar = button_w * 0.3f;
    draw->PushClipRect(pos, ImVec2(pos.x + button_w, max.y), true);
    draw->AddRectFilled(ImVec2(pos.x - bar + (button_w + bar) * t, max.y - px(2)), ImVec2(pos.x + (button_w + bar) * t, max.y), primary ? p.on_accent : p.accent);
    draw->PopClipRect();
    anim_touch_slow();
  }
  if (primary && draw_save_feedback(env, draw, icon_at, glyph, fg)) {
  } else if (busy) {
    spinner(draw, ImVec2(icon_at.x + glyph / 2, icon_at.y + glyph / 2), px(6), fg);
  } else if (!primary && checked) {
    const float fade = since_done < 0.5 ? 1.0f : 1.0f - ease_in(static_cast<float>((since_done - 0.5) / 0.4));
    draw_icon(draw, "check", icon_at, glyph, with_alpha(fg, fade), 1.7f);
    anim_touch_slow();
  } else if (icon == "record") {
    // The same mark the quick panel draws: BuffrX's own red in every theme, filled while something records and a hollow box
    // otherwise, so the two windows say one thing the same way.
    const ImVec2 corner(icon_at.x + px(3), icon_at.y + px(3));
    const ImVec2 edge(icon_at.x + glyph - px(3), icon_at.y + glyph - px(3));
    const ImU32 red = kRecordRed;
    if (recording) draw->AddRectFilled(corner, edge, red);
    else draw->AddRect(corner, edge, red, 0.0f, 0, 1.7f);
  } else {
    draw_icon(draw, icon, icon_at, glyph, fg, 1.7f, icon == "stop" || icon == "play");
  }
  const float tx = pos.x + px(36);
  const float detail_w = text_width(fonts().small, detail);
  const float detail_x = std::max(tx + text_width(fonts().strong, title) + px(16), pos.x + button_w - detail_w);
  draw->PushClipRect(ImVec2(tx, pos.y), ImVec2(pos.x + button_w, pos.y + h), true);
  draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(tx, pos.y + (h - fonts().strong->LegacySize) / 2), fg, title.c_str());
  draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(detail_x, pos.y + (h - fonts().small->LegacySize) / 2), sub,
                detail.c_str());
  draw->PopClipRect();
  if (!hotkey_action.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(pos.x + w - chord_w - px(8), pos.y + (h - px(20)) / 2));
    const std::string tip = pad_only ? "Change the controller hotkey: " + hotkey_label(hotkey_action)
                                     : "Change the hotkey: " + hotkey_label(hotkey_action);
    if (chord_button("##chord", chord, tip, true, 20, primary)) {
      env.panel.recording = hotkey_action;
      // Open on the input the chip shows: a row bound only on a pad should not ask for a key first.
      env.panel.recorder_controller = pad_only;
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h + px(4)));
  ImGui::PopID();
  if (pressed && run) run();
}

// The buffer length: preset chips, then a field for a custom one. Returns the field's box, so the row below can centre
// its control under it.
ImVec2 limit_picker(ViewEnv& env, const char* id, float cell) {
  ImGui::PushID(id);
  const int seconds = static_cast<int>(env.store.config["buffer_seconds"].as_int(60));
  auto commit = [&](int value) {
    const int clamped = clamp_buffer_seconds(value);
    if (clamped != seconds) set(env, "buffer_seconds", Json(clamped));
  };
  const std::vector<int> chips = limit_chips(seconds);
  std::vector<std::string> labels;
  int selected = -1;
  for (size_t i = 0; i < chips.size(); ++i) {
    labels.push_back(format_span(chips[i]));
    if (chips[i] == seconds) selected = static_cast<int>(i);
  }
  int chosen = selected;
  if (segmented("##chips", labels, chosen, cell, 24, fonts().small) && chosen >= 0) commit(chips[static_cast<size_t>(chosen)]);
  ImGui::SameLine(0, px(8));
  // A custom length: type it, or step by five seconds. It is exactly as tall as the chips beside it.
  ImGuiStorage* storage = ImGui::GetStateStorage();
  static std::string draft;
  const ImGuiID editing_id = ImGui::GetID("editing");
  if (!storage->GetBool(editing_id)) draft = std::to_string(seconds);
  const ImVec2 field = ImGui::GetCursorScreenPos();
  ImGui::PushFont(fonts().small);
  ImGui::SetNextItemWidth(px(46));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, pal().canvas);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), (px(24) - fonts().small->LegacySize) * 0.5f));
  ImGui::InputText("##custom", &draft, ImGuiInputTextFlags_CharsDecimal);
  const float field_w = ImGui::GetItemRectSize().x;
  storage->SetBool(editing_id, ImGui::IsItemActive());
  if (ImGui::IsItemDeactivatedAfterEdit()) commit(draft.empty() ? seconds : std::atoi(draft.c_str()));
  if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) commit(seconds + 5);
  if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) commit(seconds - 5);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::PopFont();
  tooltip("Custom buffer length in seconds. Up and Down step by five.");
  // The unit rides the field's own centre line, so the whole row keeps one baseline.
  ImGui::SetCursorScreenPos(
      ImVec2(field.x + px(46) + px(4), field.y + (px(24) - fonts().small->LegacySize) * 0.5f));
  text(fonts().small, pal().muted, "s");
  ImGui::PopID();
  return ImVec2(field.x, field_w);
}

// A heading's info, as a card rather than a line running across the screen: the title says what is described, the body
// says what it does and what it leaves alone. `IsItemHovered` refers to the control before it.
void info_card(const char* title, const char* body, float wrap = px(270)) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(9)));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, pal().raised);
  ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
  ImGui::BeginTooltip();
  ImGui::PushFont(fonts().strong);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, pal().muted);
  ImGui::PushFont(fonts().small);
  ImGui::PushTextWrapPos(wrap);
  ImGui::TextUnformatted(body);
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  ImGui::PopStyleColor();
  ImGui::EndTooltip();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}

// Performance mode's switch, drawn as the quick panel's buffer switch: three cells, OFF, AUTO and ON, the live one
// filled. The heading's info icon says what the modes do; the switch itself says only why it is locked.
float performance_cell_width(const char* label) { return text_width(fonts().label, label) + px(16); }
float performance_switch_width() {
  return performance_cell_width("off") + performance_cell_width("auto") + performance_cell_width("on");
}

void performance_control(ViewEnv& env) {
  const Json& status = env.store.status;
  const std::string when = env.store.config["performance_when"].as_string("off");
  const int chosen = when == "games" ? 1 : when == "always" ? 2 : 0;
  const Palette& p = pal();
  const float cell_h = px(22);
  const float widths[3] = {performance_cell_width("off"), performance_cell_width("auto"), performance_cell_width("on")};
  const float w = widths[0] + widths[1] + widths[2];
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushID("performance");
  // Rebuilding the encoder behind the switch cuts a running recording in two, so the switch waits for it (and for a rebuild in progress).
  const bool locked = !env.store.connected || status["rebuilding"].as_bool(false) || status["recording"].as_bool(false);
  ImGui::BeginDisabled(locked);
  const bool pressed = ImGui::InvisibleButton("##performance", ImVec2(w, cell_h));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::EndDisabled();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const char* labels[3] = {"off", "auto", "on"};
  float x = at.x;
  int hit = -1;
  for (int i = 0; i < 3; ++i) {
    const float x1 = x + widths[i];
    if (pressed && ImGui::GetIO().MousePos.x >= x && ImGui::GetIO().MousePos.x < x1) hit = i;
    draw->AddRectFilled(ImVec2(x, at.y), ImVec2(x1, at.y + cell_h), chosen == i ? p.accent : p.canvas);
    if (i > 0) draw->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + cell_h), p.line);
    draw->AddText(fonts().label, fonts().label->LegacySize, ImVec2(x + px(8), at.y + (cell_h - fonts().label->LegacySize) / 2),
                  chosen == i ? p.on_accent : p.muted, labels[i]);
    x = x1;
  }
  draw->AddRect(at, ImVec2(at.x + w, at.y + cell_h), hovered ? p.muted : p.line);
  if (status["recording"].as_bool(false)) tooltip("Stop the recording to change this: the encoder is rebuilt, which would cut the recording in two.");
  if (pressed && !locked && hit >= 0 && hit != chosen)
    set(env, "performance_when", Json(hit == 0 ? "off" : hit == 1 ? "games" : "always"));
  ImGui::PopID();
}

void quick_picker(ViewEnv& env, float width = 132.0f, bool strong = false) {
  const int seconds = static_cast<int>(env.store.config["quick_clip_seconds"].as_int(20));
  const std::vector<int> values = with_current(quick_clip_choices(), seconds);
  std::vector<std::string> labels;
  int selected = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    labels.push_back(std::to_string(values[i]) + " seconds");
    if (values[i] == seconds) selected = static_cast<int>(i);
  }
  if (select("##quick", labels, selected, width, false, {}, strong)) set(env, "quick_clip_seconds", Json(values[static_cast<size_t>(selected)]));
  // The length only: the action and its chord live on the row above and in Settings > Hotkeys.
  tooltip("Saves the last " + std::to_string(seconds) + " seconds of play.");
}

void welcome(ViewEnv& env) {
  if (env.panel.welcomed) return;
  // The guide has done its job once there is a clip: it was written for the first save. A replay from the help sheet stays.
  if (!env.panel.guide_replay && env.store.library_loaded && !env.store.clips.empty()) {
    env.panel.welcomed = true;
    env.panel.deck_fill_ready = false;
    env.shell.save_layout();
    return;
  }
  auto keys_of = [&](const char* action, const char* fallback) {
    std::string keys = env.store.chord(action);
    for (size_t at = keys.find('+'); at != std::string::npos; at = keys.find('+', at + 3)) keys.replace(at, 1, " + ");
    return keys.empty() ? std::string(fallback) : keys;
  };
  const std::string folder = env.store.config["clips_dir"].as_string("Videos\\BuffrX");
  const std::string length = format_span(static_cast<int>(env.store.config["buffer_seconds"].as_double(60)));
  // Two lines of what it is and who it is for, then one step per control. Each fits four lines of the small face.
  const std::vector<std::pair<std::string, std::string>> steps = {
      {"What BuffrX does", "It keeps the last few minutes of your screen in memory. When something great happens, you save it after the fact."},
      {"Who it is for", "Players who never hit record in time. No account, no upload, nothing leaves your PC unless you share it."},
      {"Save a clip", "Press " + keys_of("save_replay", "the save hotkey") + " any time, in any game, to keep the last " + length +
                          ". Clips are filed by game in " + (file_name(folder).empty() ? folder : file_name(folder)) + "."},
      {"Record and screenshot", keys_of("toggle_record", "the record key") + " starts and stops a full recording. " + keys_of("screenshot", "the screenshot key") +
                                    " takes a screenshot. Both land in the library."},
      {"Timeline", keys_of("view_clip", "the Timeline key") + " opens Timeline over your game: scrub back through recent footage and save any moment."},
      {"Edit, share, tune", "Click a clip to play it; Trim opens the editor. Its menu shares or renames it. 1-4 switch profiles, the gear opens Settings."},
      {"Keep learning", "Press F1 or ? any time for Help: a fuller guide to every part, with steps and tips."}};
  const int last = static_cast<int>(steps.size()) - 1;
  int& step = env.panel.guide_step;
  step = std::clamp(step, 0, last);
  begin_pane("welcome", "guide " + std::to_string(step + 1) + " / " + std::to_string(steps.size()), ImVec2(-1, 0));
  // V11: a step slides 12 px in the direction of travel and fades in when the step changes (Next or Back).
  static int shown_step = -1;
  static int travel = 1;
  static double step_changed_at = -10.0;
  if (shown_step != step) {
    travel = shown_step < 0 || step > shown_step ? 1 : -1;
    shown_step = step;
    step_changed_at = anim_detail::wall_seconds();
  }
  const float arrived = ease_out(progress(anim_detail::wall_seconds() - step_changed_at, kDurBase));
  if (arrived < 1.0f) anim_touch();
  const float w = ImGui::GetContentRegionAvail().x;
  const float top = ImGui::GetCursorPosY();
  const float text_w = w - px(96);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const int card_v = draw->VtxBuffer.Size;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(12) * (1.0f - arrived) * static_cast<float>(travel));
  text_wrapped(fonts().strong, pal().accent, steps[step].first, text_w);
  text_wrapped(fonts().small, pal().ink, steps[step].second, text_w);
  fade_since(draw, card_v, arrived);
  ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x);
  if (step == 2) tooltip(folder);  // the whole path is one hover away; the sentence names the folder
  ImGui::SetCursorPosY(top);
  ImGui::Dummy(ImVec2(w, px(76)));  // the same height at every step, so the deck below never moves
  const float x = ImGui::GetCursorStartPos().x + w - px(76);
  ImGui::SetCursorPos(ImVec2(x, top));
  if (button((step == last ? "Got it##welcome" : "Next##welcome"), step == last ? Variant::primary : Variant::normal, nullptr, false, 76, 28)) {
    if (step == last) {
      env.panel.welcomed = true;
      env.panel.guide_replay = false;
      // The card is gone, so the deck's natural height changed: measure the fill again.
      env.panel.deck_pad = 0;
      env.panel.deck_pad_seen = -1;
      env.panel.deck_fill_ready = false;
      env.shell.save_layout();
    } else {
      ++step;
      anim_touch();  // the next frame draws the new step and starts its slide
    }
  }
  ImGui::SetCursorPos(ImVec2(x, top + px(34)));
  ImGui::BeginDisabled(step == 0);
  if (button("Back##welcome", Variant::normal, nullptr, false, 76, 28)) {
    --step;
    anim_touch();
  }
  ImGui::EndDisabled();
  end_pane();
  ImGui::Dummy(ImVec2(0, px(kGap - 8)));
}

void buffer_pane(ViewEnv& env, float height) {
  const Json& status = env.store.status;
  const Json& config = env.store.config;
  const std::string state = !env.store.connected ? "disconnected" : status["buffer_state"].as_string("off");
  const double filled = status["stats"]["ring"]["filled"].as_double();
  const double seconds = config["buffer_seconds"].as_double(status["stats"]["ring"]["seconds"].as_double(60));
  // The one focused pane: the buffer, until the arrows are over the library.
  begin_pane("buffer", "buffer", ImVec2(-1, height), env.panel.rail_cursor < 0);
  // The tape across the pane, and its Start / Stop switch at the top right. Nothing else: the rate, the ring and the drops
  // are in the status strip, and the speaker and the mic in the AUDIO pane.
  const float inner_h = ImGui::GetContentRegionAvail().y;
  const float top = ImGui::GetCursorPosY();
  const float tape_w = ImGui::GetContentRegionAvail().x;
  const uint32_t pulse_age = env.store.pulse_at ? env.now_ms - env.store.pulse_at : 100000;
  // The tape takes the pane's whole content: the deck's leftover height lives here, in taller cells.
  if (timeline("##tape", state, filled, seconds, pulse_age, unpx(tape_w), inner_h)) toggle_buffer(env);
  // Start while the buffer is off, Stop while it runs: the one switch, at the tape's top right.
  if (!config.is_null()) {
    const bool on = state == "on" || state == "starting";
    const char* word = on ? "stop" : "start";
    const float icon = px(12), chip_h = px(24);
    const float chip_w = px(7) + icon + px(6) + text_width(fonts().strong, word) + px(9);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Right edge on the tape's, level with the big readout's cap height.
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + tape_w - chip_w, top + px(8)));
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + chip_w, a.y + chip_h);
    const bool pressed = ImGui::InvisibleButton("##bufswitch", ImVec2(chip_w, chip_h));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) draw->AddRectFilled(a, b, pal().accent);
    else draw->AddRect(a, b, pal().accent);
    const ImU32 fg = hovered ? pal().on_accent : pal().accent;
    // B2: the label and icon fade in when the state flips (Start to Stop and back): each state has its own arrival slot.
    const float flipped = appear(ImGui::GetID(on ? "##bufon" : "##bufoff"), kDurBase);
    const int chip_v = draw->VtxBuffer.Size;
    draw_icon(draw, on ? "stop" : "play", ImVec2(a.x + px(7), a.y + (chip_h - icon) / 2), icon, fg, 1.7f, true);
    draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(a.x + px(7) + icon + px(6), a.y + (chip_h - fonts().strong->LegacySize) / 2), fg, word);
    fade_since(draw, chip_v, flipped);
    // No tooltip of its own: the chip sits on the tape, whose tooltip already says the same words, and two hovered items
    // draw two cards.
    if (pressed) toggle_buffer(env);
  }
  end_pane();
}

// The row Enter presses on the deck, in the order action_row draws them.
void deck_run(ViewEnv& env, int row) {
  if (row == 0) save_clip(env);
  else if (row == 1) quick_clip(env);
  else if (row == 2) toggle_recording(env);
  else if (row == 3) screenshot(env);
  else if (row == 4) timeline_action(env);
}

void deck(ViewEnv& env, float width, float height) {
  const Json& status = env.store.status;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(kGap), px(kGap)));
  // A scrollbar for the case the work area is shorter than the panes need (150% on a 1366x768 laptop is
  // about 390 design units): the options at the foot are then reachable by the bar instead of clipped.
  // The wheel moves this column too, through smooth_scroll, so a short window is not a dead end.
  ImGui::BeginChild("deck", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
  // The timeline strip (it zooms), the actions list (it scrolls) and a fader (it takes the wheel) took it last frame: the deck
  // stays put under them.
  const bool inner_wheel = env.panel.live.wheel || env.panel.live.audio_hover;
  env.panel.live.wheel = false;
  smooth_scroll(px(96), !inner_wheel);
  ImGui::Dummy(ImVec2(0, px(2)));  // room for the first pane's title above its border
  // The tape is fixed: count, cells and legend need about two lines of type and a row of cells, and it is the
  // same height at every window size. The other panes hug their content too, so the column ends after the
  // options instead of stretching the ticker.
  const float tape = timeline_height() + px(15) * 2 + px(12);
  buffer_pane(env, tape);
  welcome(env);

  begin_pane("run", "run", ImVec2(-1, 0));
  const bool recording = status["recording"].as_bool();
  // Record starts and stops the same standalone recording regardless of Timeline state.
  action_row(env, "save", "save", "Save clip", "", "primary",
             env.panel.saving && env.now_ms < env.panel.saving_until, "save_replay", [&] { save_clip(env); },
             env.panel.deck_cursor == 0);
  action_row(env, "quick", "history", "Quick clip", "last " + format_span(env.store.config["quick_clip_seconds"].as_int(20)),
             "", false, "quick_clip", [&] { quick_clip(env); }, env.panel.deck_cursor == 1);
  action_row(env, "record", record_icon(recording), record_label(recording),
             recording ? format_duration(status["recording_seconds"].as_double()) : "",
             recording ? "record" : "", false, "toggle_record", [&] { toggle_recording(env); },
             env.panel.deck_cursor == 2);
  action_row(env, "shot", "camera", "Screenshot", "", "", false, "screenshot", [&] { screenshot(env); },
             env.panel.deck_cursor == 3);
  action_row(env, "timeline", "film", "Timeline", "", "", false, "view_clip", [&] { timeline_action(env); },
             env.panel.deck_cursor == 4);
  // The last row's trailing gap would leave more room under it than above the first: take it back.
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(9));
  end_pane();

  // Options: one grid on a single 28-unit control line. Rules separate the rows, a seam splits the pair, and each
  // label sits on its own column's first line, so the two headings and their controls share one y each.
  // The pane's usual 13-unit top, with a foot a little tighter: the ON/OFF line is the last, and a full 13 under it
  // read as slack. The padding is the same above and below, so the top is put back with a spacer.
  begin_pane("options", "options", ImVec2(-1, 0), false, std::string(), 0, false, 9);
  ImGui::Dummy(ImVec2(0, px(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(6)));
  const float wide = ImGui::GetContentRegionAvail().x;
  const float label_h = fonts().label->LegacySize;
  const float control_h = px(24);
  const float row_gap = px(5);
  const auto rule = [&] {
    ImGui::Dummy(ImVec2(0, px(5)));
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(a, ImVec2(a.x + ImGui::GetContentRegionAvail().x, a.y), pal().line, 1.0f);
    // The only rule is above the last row, which has the pane's trimmed foot under it: 2 units here rather than 5
    // leaves the row the same 9-unit gap above as below.
    ImGui::Dummy(ImVec2(0, px(2)));
  };
  // Two rows, two columns, each setting a heading over its control: buffer limit and quick clip, then timeline and
  // performance mode. A seam splits the columns; when the pane is too narrow for the second column it stacks under the first.
  const float chip_cell = 38.0f;  // design units: "30s", "1m", "2m", "5m"
  const float left_w = px(chip_cell * 4 + 70);
  const float gutter = px(16);
  const float right_w = wide - left_w - gutter;
  const bool pair = right_w >= px(100);
  const auto seam = [&](const ImVec2& row, float bottom) {
    if (pair) {
      const float x = row.x + left_w + gutter * 0.5f;
      ImGui::GetWindowDrawList()->AddLine(ImVec2(x, row.y), ImVec2(x, bottom), pal().line, 1.0f);
    }
  };
  // The right column's heading and control, beside the left one (or under it when the pane is narrow).
  const auto right_column = [&](const ImVec2& row, float& bottom, const char* heading, const std::function<void(float)>& control) {
    ImVec2 at = ImVec2(pair ? row.x + left_w + gutter : row.x, pair ? row.y : bottom + row_gap);
    ImGui::SetCursorScreenPos(at);
    label(heading);
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + label_h + row_gap));
    control(pair ? right_w : wide);
    bottom = std::max(bottom, ImGui::GetCursorScreenPos().y);
  };

  // Row 1
  ImVec2 row = ImGui::GetCursorScreenPos();
  label("buffer limit");
  ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + label_h + row_gap));
  limit_picker(env, "limit", chip_cell);
  float bottom = std::max(row.y + label_h + row_gap + control_h, ImGui::GetCursorScreenPos().y);
  right_column(row, bottom, "quick clip", [&](float w) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), (control_h - fonts().body->LegacySize) * 0.5f));
    quick_picker(env, unpx(w), false);
    ImGui::PopStyleVar();
  });
  seam(row, bottom);
  ImGui::SetCursorScreenPos(ImVec2(row.x, bottom));
  rule();

  // Row 2: performance mode across the whole pane (the Timeline's length lives in its own pane below). The heading and
  // its info icon sit at the left, the switch at the pane's right edge, both on the same control line as Row 1.
  row = ImGui::GetCursorScreenPos();
  const float switch_h = px(24);  // the same control line as Row 1
  const float switch_w = performance_switch_width();
  const bool performance_on = env.store.config["performance_when"].as_string("off") != "off";
  ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + (switch_h - label_h) * 0.5f));
  const float heading_w = label("performance mode", performance_on ? pal().accent : 0);
  ImGui::SetCursorScreenPos(ImVec2(row.x + heading_w + px(6), row.y + (switch_h - px(20)) * 0.5f));
  icon_button("##perfhelp", "info", "", false, 20, 15);
  info_card("performance mode",
            "Off: Performance Mode is disabled.\n\nAuto: Activates only while a listed game is running.\n\nOn: "
            "Performance Mode stays enabled.\n\nNote: Reduces recording quality to improve gaming performance. Your "
            "original video settings are restored when Performance Mode turns off.",
            px(330));
  ImGui::SetCursorScreenPos(ImVec2(row.x + wide - switch_w, row.y + (switch_h - px(22)) * 0.5f));
  performance_control(env);
  // End on the switch's own line: the 24-unit row exists to centre it, and its trailing 2 units would only read as
  // padding under the pane's border.
  ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + px(22)));
  ImGui::PopStyleVar();
  end_pane();

  draw_live_timeline(env);
  // History and audio stay the same height as the window changes. The history list and the deck scroll independently
  // when their contents do not fit, so resizing never stretches the mixer or its meters.
  const float foot = px(210);
  const float audio_w = px(140);
  // Pin the pair to the column's bottom. Room for a visible gap goes above the pair; a smaller leftover (pixel rounding)
  // is absorbed by the boxes so their bottom edge lands on the library's border.
  const float room = ImGui::GetContentRegionAvail().y;
  float foot_h = foot;
  if (room - foot >= px(kGap)) ImGui::Dummy(ImVec2(0, room - foot - px(kGap)));
  else foot_h = std::max(foot, room);
  draw_history(env, ImVec2(ImGui::GetContentRegionAvail().x - audio_w - px(kGap), foot_h));
  ImGui::SameLine(0, px(kGap));
  draw_audio(env, ImVec2(audio_w, foot_h));
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- keys

// Nothing else wants the keyboard: no dialog, field, popup, Settings or preview. Alt chords are the global hotkeys.
bool keys_free(const PanelState& panel) {
  const ImGuiIO& io = ImGui::GetIO();
  return panel.settings.empty() && !panel.preview_open && !panel.share_open && !panel.rename_open && !panel.remove_open && !panel.buffer_stop_open &&
         !panel.recorder_open && panel.recording.empty() && panel.link_offer.empty() && !panel.keys_open &&
         !panel.popup_was_open && !io.WantTextInput && !io.KeyAlt && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
}

// The window's keys; the library's own are in rail(), where its list is. The actions (save, quick clip, record,
// screenshot) are the global hotkeys: the panel does not repeat them, it moves around.
void panel_keys(ViewEnv& env) {
  PanelState& panel = env.panel;
  if (!keys_free(panel)) return;
  const ImGuiIO& io = ImGui::GetIO();
  auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
  // Tab walks the panes: the deck, the clips, the screenshots, the Timeline, round again (Shift+Tab the other way).
  // The library comes back to the card it was left on, not the newest.
  if (pressed(ImGuiKey_Tab)) {
    // 0 deck, 1 clips, 2 screenshots, 3 the Timeline, 4 Shared with me; nothing focused (0 and 1 both -1) counts
    // as the last place, so the first Tab lands on the deck.
    const int place = panel.rail_cursor >= 0 ? 1 + panel.rail_tab : panel.deck_cursor >= 0 ? 0 : 4;
    const int next = (place + (io.KeyShift ? 4 : 1)) % 5;
    if (next == 0) {
      panel.deck_cursor = panel.deck_cursor < 0 ? 0 : panel.deck_cursor;
      panel.rail_cursor = -1;
    } else {
      if (panel.rail_cursor >= 0) panel.rail_memory[panel.rail_tab] = panel.rail_cursor;
      panel.deck_cursor = -1;
      panel.rail_tab = next - 1;
      panel.rail_cursor = panel.rail_memory[panel.rail_tab];
    }
    env.shell.save_layout();
  } else if (panel.rail_cursor < 0 && panel.deck_cursor < 0 &&
             (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_DownArrow) ||
              pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_Home) || pressed(ImGuiKey_End))) {
    // Nothing focused yet: a direction takes the deck's first row. A pad whose first press moved
    // nothing read as a dead controller.
    panel.deck_cursor = 0;
  } else if (panel.deck_cursor >= 0 && (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_LeftArrow) ||
                                        pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_RightArrow) ||
                                        pressed(ImGuiKey_Home) || pressed(ImGuiKey_End))) {
    // The deck's five actions: Save clip, Quick clip, Record, Screenshot, Timeline.
    const bool back = pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_LeftArrow);
    const bool forth = pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_RightArrow);
    if (back) panel.deck_cursor = std::max(0, panel.deck_cursor - 1);
    else if (forth) panel.deck_cursor = std::min(4, panel.deck_cursor + 1);
    else panel.deck_cursor = pressed(ImGuiKey_Home) ? 0 : 4;
  } else if (panel.deck_cursor >= 0 &&
             (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Space))) {
    deck_run(env, panel.deck_cursor);
  } else if (pressed(ImGuiKey_Comma)) {
    panel.settings = "capture";
  } else if (pressed(ImGuiKey_F1) || pressed(ImGuiKey_Slash)) {
    panel.keys_open = true;
  } else if (pressed(ImGuiKey_Escape)) {
    // Esc leaves the library or the deck first, then hides the window.
    if (panel.rail_cursor >= 0) panel.rail_cursor = -1;
    else if (panel.deck_cursor >= 0) panel.deck_cursor = -1;
    else env.shell.close_window();
  }
  for (int i = 0; i < 4; ++i) {
    if (pressed(static_cast<ImGuiKey>(ImGuiKey_1 + i)) && i < static_cast<int>(env.store.profiles.size()) &&
        i != env.store.config["active_profile"].as_int(-1)) {
      apply_profile(env, i);
    }
  }
}

// ---------------------------------------------------------------- the rail

// What a Timeline card's menu (and its keyboard) does. Erase is the one that cannot be undone, so the menu asks
// twice - Settings holds the same rule for the same footage - and the first press only arms it.
void run_session_action(ViewEnv& env, const std::string& action, const std::string& id) {
  Json args = Json::object();
  args.set("session", Json(id));
  if (action == "preview") {
    bool live = false;
    for (const UiSession& session : env.store.sessions) {
      if (session.id == id) { live = session.live; break; }
    }
    env.store.attempt(live ? "timeline.open" : "timeline.preview", args, "The footage did not open");
  } else if (action == "trim") {
    for (const UiSession& session : env.store.sessions) {
      if (session.id == id && session.live) {
        env.store.toast(UiToast::info, "Live footage is not editable yet",
                        "Trim it in Timeline and save a clip.");
        return;
      }
    }
    env.store.attempt("timeline.edit", args, "The editor did not open");
  } else if (action == "erase") {
    env.panel.timeline_erase.clear();
    env.store.attempt("timeline.erase", args, "The footage was not erased", [&store = env.store](const Json&) { store.refresh_sessions(0); });
  } else if (action == "arm") {
    env.panel.timeline_erase = id;
    env.panel.timeline_erase_until = env.now_ms + 4000;
  } else if (action == "folder") {
    args.set("kind", Json("timeline"));
    env.store.attempt("app.open_folder", args, "The folder did not open");
  }
}

// The last clip saved, in its own pane above the tabs: the library should not make the person hunt for the thing they
// just made. It is the newest clip on disk (`store.clips` is listed newest first), so it is there after a reopen too.
void last_saved(ViewEnv& env) {
  UiStore& store = env.store;
  if (store.clips.empty()) return;
  const UiMedia item = store.clips.front();  // a copy: the reply's lambda may replace the list under this frame
  const float content = px(76) - px(26);
  // The face is one target, so the pane's own border lights wherever the pointer is on it, not only over the picture.
  begin_pane("lastsaved", "last saved", ImVec2(-1, px(76)), false, std::string(), 0, true);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float inner_w = ImGui::GetContentRegionAvail().x;
  const float thumb_w = std::round(content * 16.0f / 9.0f);
  ImGui::SetCursorScreenPos(pos);
  // The whole face opens the clip. Overlap is allowed so the three framed icons at its right, added after it,
  // take their own clicks: without that the face swallows them and the menu never opens.
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##lastsaved", ImVec2(inner_w, content));
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked();
  const bool menu = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  // The play mark belongs to the picture alone: the rest of the row still opens the clip but does not light it.
  const bool over_picture = hovered && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + thumb_w, pos.y + content));
  const float hover = anim_item(over_picture ? 1.0f : 0.0f, kRateHover);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  // C10: a new save fades its picture, name and time in (the base token); a pane that shows the same clip keeps it.
  const int face_v = draw->VtxBuffer.Size;
  const ImGuiID last_scope = ImHashStr("last_saved");
  arrivals_update(last_scope, {ImHashStr(item.file.c_str())});
  const float arrived = arrival(last_scope, ImHashStr(item.file.c_str()));
  parts::thumbnail(env, item, pos, ImVec2(thumb_w, content), 0);
  if (hover > 0.0f) {
    // The same play mark a card grows, so the pane reads as a thing that opens.
    draw->AddRectFilled(pos, ImVec2(pos.x + thumb_w, pos.y + content), with_alpha(pal().canvas, 0.35f * hover));
    const ImVec2 c(pos.x + thumb_w / 2, pos.y + content / 2);
    const float half = px(16) * (0.6f + 0.4f * hover);
    draw->AddRectFilled(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), with_alpha(pal().accent, hover));
    draw_icon(draw, "play", ImVec2(c.x - px(7.5f), c.y - px(7.5f)), px(15), with_alpha(pal().on_accent, hover), 1.7f, true);
  }
  const std::string size = format_bytes(static_cast<double>(item.size));
  const float side = px(26);
  const float gap = px(6);
  const float buttons_w = side * 3 + gap * 2;
  const float size_pad = px(8);
  const float size_w = text_width(fonts().small, size) + size_pad * 2;
  // The clip's name tells more than its byte count: when the pane is too narrow for both, the size box gives way.
  const bool room_for_size = inner_w - thumb_w - px(12) - buttons_w - px(10) > px(150);
  const float right_w = (room_for_size ? size_w + px(10) : 0.0f) + buttons_w;
  const float text_x = pos.x + thumb_w + px(12);
  const float text_w = std::max(px(40), inner_w - thumb_w - px(12) - right_w - px(12));
  // The monitor and folder identify the newest clip; the file's own name says nothing a person can use.
  const float strong_h = fonts().strong->LegacySize;
  const float title_y = pos.y + (content - strong_h) * 0.5f;
  draw_icon(draw, "monitor", ImVec2(text_x, title_y + (strong_h - px(13)) * 0.5f), px(13), pal().muted, 1.5f);
  ImGui::SetCursorScreenPos(ImVec2(text_x + px(18), title_y));
  text_ellipsis(fonts().strong, pal().ink, item.folder.empty() ? "Clips" : item.folder, text_w - px(18));
  // One band at the pane's right, centred like the text: the size framed, then Edit, Share and the menu.
  const float icon_y = pos.y + (content - side) * 0.5f;
  if (room_for_size) {
    const ImVec2 size_min(pos.x + inner_w - right_w, icon_y);
    draw->AddRect(size_min, ImVec2(size_min.x + size_w, size_min.y + side), pal().line, 0.0f, 0, 1.0f);
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(size_min.x + size_pad, size_min.y + (side - fonts().small->LegacySize) * 0.5f),
                  pal().muted, size.c_str());
  }
  float icon_x = pos.x + inner_w - buttons_w;
  const auto framed = [&](const char* id, const char* icon, const std::string& tip) {
    draw->AddRect(ImVec2(icon_x, icon_y), ImVec2(icon_x + side, icon_y + side), pal().line, 0.0f, 0, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(icon_x, icon_y));
    const bool pressed = icon_button(id, icon, tip, false, 26, 15);
    icon_x += side + gap;
    return pressed;
  };
  if (framed("##lastedit", "scissors", "Trim in editor")) open_editor(env, {item.file});
  if (framed("##lastshare", "share", "Share")) open_share(env, item);
  if (framed("##lastmenu", "dots", "More") || menu) ImGui::OpenPopup("##lastsavedmenu");
  parts::media_menu(env, "##lastsavedmenu", item, store.clips);
  fade_since(draw, face_v, arrived);
  end_pane();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool on_icons = mouse.x >= pos.x + inner_w - right_w && mouse.y >= icon_y && mouse.y <= icon_y + side;
  if (clicked && !on_icons) open_preview(env, item, store.clips);
}

// A link this PC is serving, between the last save and the library. The rail keeps LAST SAVED in place and grows this
// pane only while something is live; the height is animated so the library moves instead of jumping.
void live_link(ViewEnv& env, const LinkView& link, const std::vector<LinkView>& links, const std::vector<size_t>& order,
               size_t chosen, float height, int64_t now, bool live) {
  UiStore& store = env.store;
  const float content = std::max(0.0f, height - px(26));
  std::string aside = link_expiry(link.expires - now);
  if (order.size() > 1) aside += " \xC2\xB7 " + std::to_string(chosen + 1) + "/" + std::to_string(order.size());
  begin_pane("livelink", "live link", ImVec2(-1, height), false, aside);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float inner_w = ImGui::GetContentRegionAvail().x;
  const float thumb_w = std::round(std::max(px(24), content * 16.0f / 9.0f));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const UiMedia* media = find_media(env, link.file);
  if (media) {
    parts::thumbnail(env, *media, pos, ImVec2(thumb_w, content), 0);
  } else {
    draw->AddRectFilled(pos, ImVec2(pos.x + thumb_w, pos.y + content), pal().canvas);
    draw->AddRect(pos, ImVec2(pos.x + thumb_w, pos.y + content), pal().line, 0, 0, px(1));
    draw_icon(draw, "film", ImVec2(pos.x + (thumb_w - px(18)) / 2, pos.y + (content - px(18)) / 2), px(18), pal().muted, 1.6f);
  }
  const float side = px(26);
  const float gap = px(6);
  // No wrap: the arrow that would leave the list is not drawn, so the first and last links are visible
  // as the ends and a two-link list no longer flips back and forth under the pointer.
  const int arrows = order.size() > 1 ? (chosen > 0 ? 1 : 0) + (chosen + 1 < order.size() ? 1 : 0) : 0;
  const int buttons = 2 + arrows;
  const float buttons_w = live ? side * buttons + gap * (buttons - 1) : 0.0f;
  const float text_x = pos.x + thumb_w + px(12);
  const float text_w = std::max(px(40), inner_w - thumb_w - px(12) - buttons_w - px(12));
  const float strong_h = fonts().strong->LegacySize;
  const float line_y = pos.y + std::max(0.0f, (content - (strong_h + px(4) + fonts().small->LegacySize)) * 0.5f);
  const float dot = px(3);
  draw->AddCircleFilled(ImVec2(text_x + dot, line_y + strong_h / 2), dot, pal().danger);
  ImGui::SetCursorScreenPos(ImVec2(text_x + dot * 2 + px(5), line_y));
  text_ellipsis(fonts().strong, pal().ink, link.name, text_w - dot * 2 - px(5));
  ImGui::SetCursorScreenPos(ImVec2(text_x, line_y + strong_h + px(4)));
  text_ellipsis(fonts().small, pal().muted, link_activity(link.watching, link.viewers), text_w);
  const float icon_y = pos.y + (content - side) * 0.5f;
  if (live) {
    float icon_x = pos.x + inner_w - buttons_w;
    const auto framed = [&](const char* id, const char* icon, const std::string& tip) {
      draw->AddRect(ImVec2(icon_x, icon_y), ImVec2(icon_x + side, icon_y + side), pal().line, 0.0f, 0, 1.0f);
      ImGui::SetCursorScreenPos(ImVec2(icon_x, icon_y));
      const bool pressed = icon_button(id, icon, tip, false, 26, 15);
      icon_x += side + gap;
      return pressed;
    };
    if (order.size() > 1 && chosen > 0) {
      if (framed("##linknewer", "chevronLeft", "Previous link")) {
        env.panel.link_chosen = links[order[link_step(chosen, order.size(), -1)]].id;
      }
    }
    if (order.size() > 1 && chosen + 1 < order.size()) {
      if (framed("##linkolder", "chevronRight", "Next link")) {
        env.panel.link_chosen = links[order[link_step(chosen, order.size(), 1)]].id;
      }
    }
    // C11: after a copy the icon reads as a check and the tip says Copied, then it goes back after 1.5 s.
    static double copied_at = -10.0;
    const bool just_copied = anim_detail::wall_seconds() - copied_at < 1.5;
    if (framed("##linkcopy", just_copied ? "check" : "copy", just_copied ? "Copied" : "Copy link")) {
      copy_link(store, link.url);
      copied_at = anim_detail::wall_seconds();
    }
    if (just_copied) anim_touch_slow();
    ImGui::SetCursorScreenPos(ImVec2(icon_x, icon_y));
    link_stop_button(env, "##linkstop", link, 26, 15, true);
  }
  end_pane();
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const float share = link_life_share(link_full_seconds(env.panel.link_book, link.id), link.expires, now);
  const ImVec2 bar_min(min.x + px(1), max.y - px(3));
  const ImVec2 bar_max(max.x - px(1), max.y - px(1));
  draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(bar_min, bar_max, pal().line);
  if (share > 0.0f) draw->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * share, bar_max.y), pal().accent);
  fade_in_from(ImGui::GetForegroundDrawList(), min, max, pal().surface, appear(ImGui::GetID("##livelinkarrive"), kDurBase));
}

void live_link_slot(ViewEnv& env) {
  const std::vector<LinkView> links = link_views(env.store.links);
  const int64_t now = now_unix();
  if (link_book_update(env.panel.link_book, links, now)) env.panel.link_chosen.clear();
  const std::vector<size_t> order = live_link_order(links, env.panel.link_book, now);
  const bool live = !order.empty();
  if (!live && !env.panel.link_last_valid) return;
  const ImGuiID key = ImGui::GetID("##livelinkheight");
  float& height = *ImGui::GetStateStorage()->GetFloatRef(key, 0.0f);
  height = anim_to(key, live ? px(76) : 0.0f, kRateLayout);
  if (height <= 0.5f) {
    env.panel.link_last_valid = false;
    return;
  }
  size_t chosen = 0;
  if (live) {
    for (size_t i = 0; i < order.size(); ++i) {
      if (links[order[i]].id == env.panel.link_chosen) {
        chosen = i;
        break;
      }
    }
    env.panel.link_chosen = links[order[chosen]].id;
    env.panel.link_last = links[order[chosen]];
    env.panel.link_last_valid = true;
  }
  live_link(env, env.panel.link_last, links, order, chosen, height, now, live);
  // The pane spacing already separates the two boxes; the library then starts under this one.
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(4));
}

// V10: the card a screenshot was opened from. Its picture grows out of that rectangle for the first base-token
// beat; set by a card click, and cleared when the preview closes, so a key or menu open does not grow from a stale card.
struct PreviewOrigin {
  bool set = false;
  ImVec2 min, max;
  double at = -10.0;
};
PreviewOrigin g_preview_origin;

// 6.6: the card a click went to shows a sweep along its foot until its picture answers (the lightbox clears it), or 5 s.
struct OpeningState {
  bool set = false;
  std::string file;
  double at = -10.0;
};
OpeningState g_opening;

void rail(ViewEnv& env, float width, float height) {
  PanelState& panel = env.panel;
  UiStore& store = env.store;
  // A new save switches the rail to where it went, so the highlight is visible.
  static std::string followed;
  if (!store.newest.empty() && store.newest != followed) {
    for (const UiMedia& item : store.screenshots) {
      if (item.file == store.newest) panel.rail_tab = shared_item(item) ? 3 : 1;
    }
    for (const UiMedia& item : store.clips) {
      if (item.file == store.newest) panel.rail_tab = shared_item(item) ? 3 : 0;
    }
    followed = store.newest;
  }
  ImGui::BeginChild("railouter", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::Dummy(ImVec2(0, px(2)));
  last_saved(env);
  live_link_slot(env);
  // The library sits close under it: the pane spacing already separates them, so no extra gap.
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(4));
  begin_pane("rail", "", ImVec2(-1, ImGui::GetContentRegionAvail().y), panel.rail_cursor >= 0);
  // The kind tabs keep their labels and counts; the filter and buttons move below them when the rail is narrow.
  const ImVec2 tabs_origin = ImGui::GetCursorScreenPos();
  const float tabs_inner = ImGui::GetContentRegionAvail().x;
  float tabs_end = tabs_origin.x;  // where the last tab stops: the game filter takes what is left of the row
  float tabs_rows = px(32);        // where the row under the tabs starts, from the tabs' top
  bool tabs_packed = false;        // too narrow for one row: the tabs share the first row, the filter and buttons the second
  static std::vector<UiMedia> shared;
  static int shared_version = -1;
  if (shared_version != store.library_version) {
    shared.clear();
    for (const UiMedia& item : store.clips) if (shared_item(item)) shared.push_back(item);
    for (const UiMedia& item : store.screenshots) if (shared_item(item)) shared.push_back(item);
    std::stable_sort(shared.begin(), shared.end(), [](const UiMedia& a, const UiMedia& b) { return a.mtime > b.mtime; });
    shared_version = store.library_version;
  }
  // Clips and screenshots as the tabs count them: a received file belongs to Shared alone.
  size_t local_clips = 0, local_shots = 0;
  for (const UiMedia& item : store.clips) if (!shared_item(item)) ++local_clips;
  for (const UiMedia& item : store.screenshots) if (!shared_item(item)) ++local_shots;
  {
    const ImVec2 pos = tabs_origin;
    const float inner = tabs_inner;
    // The quick panel's own control, so a tab reads the same in both windows.
    constexpr float kTabPad = 8.0f;
    const std::string clips = "clips " + std::to_string(local_clips);
    const std::string wide_shots = "screenshots " + std::to_string(local_shots);
    const std::string timeline = "timeline " + std::to_string(store.sessions.size());
    const std::string shared_label = "shared " + std::to_string(shared.size());
    const auto cell_width = [](const std::string& label) { return text_width(fonts().small, label) + px(kTabPad) * 2; };
    // The clips and screenshots tabs share their row with the game filter, so the tabs pack (shorter labels) when that leaves too little
    // room for the filter, rather than dropping the filter to a row of its own.
    const bool room_for_filter = panel.rail_tab == 0 || panel.rail_tab == 1;
    const float filter_reserve = room_for_filter ? px(130) : 0.0f;
    const bool short_labels = cell_width(clips) + cell_width(wide_shots) + cell_width(timeline) + cell_width(shared_label) >
                              inner - px(60) - filter_reserve;
    // Packed tabs use their measured widths, not equal cells that let a long label run into its neighbour.
    // If even those do not fit, keep all four labels visible on two rows.
    const std::string shots = short_labels ? "shots " + std::to_string(local_shots) : wide_shots;
    const float compact_pad = short_labels ? 4.0f : kTabPad;
    const auto compact_width = [&](const std::string& label) { return text_width(fonts().small, label) + px(compact_pad) * 2; };
    const float tabs_width = compact_width(clips) + compact_width(shots) + compact_width(timeline) + compact_width(shared_label);
    const bool two_rows = tabs_width > inner;
    tabs_packed = two_rows || tabs_width + px(64) + (room_for_filter ? px(104) : 0.0f) > inner;
    tabs_rows = two_rows ? px(58) : tabs_packed ? px(30) : px(32);
    int chosen = panel.rail_tab;
    bool changed = false;
    if (two_rows) {
      int first = chosen < 2 ? chosen : -1;
      if (segmented("##railtabfirst", {clips, shots}, first, 0, 24, fonts().small, compact_pad)) {
        chosen = first;
        changed = true;
      }
      ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + px(28)));
      int second = chosen >= 2 ? chosen - 2 : -1;
      if (segmented("##railtabsecond", {timeline, shared_label}, second, 0, 24, fonts().small, compact_pad)) {
        chosen = second + 2;
        changed = true;
      }
    } else {
      changed = segmented("##railtab", {clips, shots, timeline, shared_label}, chosen, 0, 24, fonts().small, compact_pad);
    }
    if (changed) {
      panel.rail_tab = chosen;
      env.shell.save_layout();
    }
    tabs_end = ImGui::GetItemRectMax().x;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + inner - px(56), pos.y - px(2) + (tabs_packed ? tabs_rows : 0.0f)));
    if (icon_button("##refresh", "refresh", "Refresh the library", false, 28, 14)) store.refresh_library(0);
    ImGui::SameLine(0, 0);
    if (icon_button("##folder", "folder", "Open the folder", false, 28, 14)) {
      Json args = Json::object();
      args.set("kind", Json(panel.rail_tab == 0 || panel.rail_tab == 3 ? "clips" : panel.rail_tab == 1 ? "screenshots" : "timeline"));
      store.attempt("app.open_folder", args, "The folder did not open");
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + tabs_rows + (tabs_packed ? px(34) : 0.0f)));
  }
  // The game filter is one compact list, set in the tab row where the library's own filter belongs. It shows with a
  // single entry when every file sits at its root, so the row keeps the same shape.
  std::map<std::string, size_t> games;
  if (panel.rail_tab == 0 || panel.rail_tab == 1) {
    const bool screenshots = panel.rail_tab == 1;
    const auto& source = screenshots ? store.screenshots : store.clips;
    std::string& game_filter = screenshots ? panel.screenshot_game : panel.game;
    for (const UiMedia& item : source) {
      if (!item.folder.empty() && !shared_item(item)) ++games[item.folder];
    }
    if (store.library_loaded && !game_filter.empty() && !games.count(game_filter)) game_filter.clear();
    // Name on the left, count on the right, a line between rows. The middle dot made the count read as part of the
    // game's name, and the popup was as cramped as the field it dropped from.
    std::vector<std::string> labels{"all games"};
    std::vector<std::string> counts{std::to_string(screenshots ? local_shots : local_clips)};
    std::vector<std::string> names{""};
    int selected = 0;
    for (const auto& [game, count] : games) {
      labels.push_back(lower_ascii(game));
      counts.push_back(std::to_string(count));
      names.push_back(game);
      if (game == game_filter) selected = static_cast<int>(names.size()) - 1;
    }
    // The filter keeps the tab row's right end. The two buttons sit at that row's far right unless the tabs are packed,
    // which moves them to the row below; when even the packed tabs leave no room the filter joins them there, flush
    // with the right edge, and never overlaps a tab.
    const float right_edge = tabs_origin.x + tabs_inner - (tabs_packed ? 0.0f : px(56)) - px(8);
    const float room = unpx(right_edge - tabs_end - px(8));
    const bool beside = !tabs_packed && room >= 96.0f;
    const float width = beside ? std::min(190.0f, room)
                               : tabs_packed ? std::max(80.0f, unpx(tabs_inner - px(64)))
                                             : std::min(190.0f, unpx(tabs_inner));
    if (beside) {
      ImGui::SetCursorScreenPos(ImVec2(right_edge - px(width), tabs_origin.y));
    } else if (tabs_packed) {
      ImGui::SetCursorScreenPos(ImVec2(tabs_origin.x, tabs_origin.y + tabs_rows));
    } else {
      ImGui::SetCursorScreenPos(ImVec2(tabs_origin.x + tabs_inner - px(width), tabs_origin.y + tabs_rows));
    }
    // The filter is the tabs' own height: a taller box beside them read as a different species of control.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), (px(24) - fonts().body->LegacySize) / 2));
    const bool picked = select("##game", labels, selected, width, false, {}, false, counts, true);
    ImGui::PopStyleVar();
    if (picked) game_filter = names[static_cast<size_t>(selected)];
    // Packed, the buttons (and the filter when it did not fit) take the row below, so the content starts under it.
    ImGui::SetCursorScreenPos(
        ImVec2(tabs_origin.x, tabs_origin.y + tabs_rows + (tabs_packed || !beside ? px(36) : 0.0f)));
  }
  std::vector<UiMedia> list;
  if (panel.rail_tab != 2) {
    const auto& source = panel.rail_tab == 0 ? store.clips : panel.rail_tab == 1 ? store.screenshots : shared;
    for (const UiMedia& item : source) {
      if (panel.rail_tab != 3 && shared_item(item)) continue;  // received files live under Shared
      if (panel.rail_tab == 0 && !panel.game.empty() && item.folder != panel.game) continue;
      if (panel.rail_tab == 1 && !panel.screenshot_game.empty() && item.folder != panel.screenshot_game) continue;
      list.push_back(item);
    }
  } else {
    // The Timeline is read again while its tab is on screen: its chunks leave as the retention window moves, and one
    // session can open or end while the panel is open.
    store.refresh_sessions(store.sessions_loaded ? 5000 : 0);
  }
  // The keyboard's place in the grid: arrows move it, and E, S, C, F, F2, Del and Enter act on its card - on the newest
  // one until an arrow has picked another.
  int scroll_to = -1;
  if (keys_free(panel) && !ImGui::GetIO().KeyCtrl && !list.empty() && panel.deck_cursor < 0) {
    const int count = static_cast<int>(list.size());
    int& cursor = panel.rail_cursor;
    cursor = std::min(cursor, count - 1);
    const int before = cursor, columns = std::max(1, panel.rail_columns);
    auto pressed = [](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    if (pressed(ImGuiKey_RightArrow, true)) cursor = std::min(count - 1, cursor + 1);
    if (pressed(ImGuiKey_LeftArrow, true)) cursor = std::max(0, cursor - 1);
    if (pressed(ImGuiKey_DownArrow, true)) cursor = cursor < 0 ? 0 : std::min(count - 1, cursor + columns);
    if (pressed(ImGuiKey_UpArrow, true)) cursor = std::max(0, cursor - columns);
    if (pressed(ImGuiKey_Home)) cursor = 0;
    if (pressed(ImGuiKey_End)) cursor = count - 1;
    if (cursor != before) scroll_to = cursor;
    const UiMedia& target = list[static_cast<size_t>(std::max(0, cursor))];
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) open_preview(env, target, list);
    else if (pressed(ImGuiKey_E)) run_media_action(env, "edit", target, list);
    else if (pressed(ImGuiKey_S)) run_media_action(env, "share", target, list);
    else if (pressed(ImGuiKey_C)) run_media_action(env, "copy", target, list);
    else if (pressed(ImGuiKey_F)) run_media_action(env, "reveal", target, list);
    // Renaming and deleting need a card the arrows picked: with none, "the newest" is a clip nobody chose.
    else if (cursor >= 0 && pressed(ImGuiKey_F2)) run_media_action(env, "rename", target, list);
    else if (cursor >= 0 && pressed(ImGuiKey_Delete)) run_media_action(env, "delete", target, list);
  }
  // The Timeline's list takes the same keys: arrows move, Enter opens the footage, E edits it, F shows it on the disk, Del erases a
  // session after a second press.
  if (panel.rail_tab == 2 && keys_free(panel) && !ImGui::GetIO().KeyCtrl && !store.sessions.empty() && panel.deck_cursor < 0) {
    const int count = static_cast<int>(store.sessions.size());
    int& cursor = panel.rail_cursor;
    cursor = std::min(cursor, count - 1);
    const int before = cursor, columns = std::max(1, panel.rail_columns);
    auto pressed = [](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    if (pressed(ImGuiKey_RightArrow, true)) cursor = std::min(count - 1, cursor + 1);
    if (pressed(ImGuiKey_LeftArrow, true)) cursor = std::max(0, cursor - 1);
    if (pressed(ImGuiKey_DownArrow, true)) cursor = cursor < 0 ? 0 : std::min(count - 1, cursor + columns);
    if (pressed(ImGuiKey_UpArrow, true)) cursor = std::max(0, cursor - columns);
    if (pressed(ImGuiKey_Home)) cursor = 0;
    if (pressed(ImGuiKey_End)) cursor = count - 1;
    if (cursor != before) scroll_to = cursor;
    const UiSession& target = store.sessions[static_cast<size_t>(std::max(0, cursor))];
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Space)) {
      run_session_action(env, "preview", target.id);
    } else if (pressed(ImGuiKey_E)) {
      run_session_action(env, "trim", target.id);
    } else if (pressed(ImGuiKey_F)) {
      run_session_action(env, "folder", target.id);
    } else if (cursor >= 0 && pressed(ImGuiKey_Delete)) {
      const bool armed = panel.timeline_erase == target.id && static_cast<int32_t>(env.now_ms - panel.timeline_erase_until) < 0;
      run_session_action(env, armed ? "erase" : "arm", target.id);
    }
  }
  ImGui::Dummy(ImVec2(0, px(2)));
  int tab_direction = 1;
  const float tab_t = tab_arrival("##rail_transition", panel.rail_tab, tab_direction);
  // Each tab owns its own scroll and layout state, so the previous tab cannot resize or shift its cards.
  const char* card_views[] = {"cards_clips", "cards_screenshots", "cards_timeline", "cards_shared"};
  ImGui::BeginChild(card_views[std::clamp(panel.rail_tab, 0, 3)], ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
  smooth_scroll(px(96), true, 32.0f);
  ImDrawList* tab_draw = ImGui::GetWindowDrawList();
  const int tab_vertex = tab_draw->VtxBuffer.Size;
  const Palette& p = pal();
  // Said over the list as well as on an empty one: the footage is temporary whether or not something is kept now.
  if (panel.rail_tab == 2) {
    text_ellipsis(fonts().small, p.faint, "Temporary footage: the oldest deletes itself. Trim a part in the editor to keep it.",
                  ImGui::GetContentRegionAvail().x);
    ImGui::Dummy(ImVec2(0, px(6)));
  }
  if (!store.library_loaded || (panel.rail_tab == 2 && !store.sessions_loaded)) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    spinner(ImGui::GetWindowDrawList(), ImVec2(pos.x + w / 2, pos.y + px(60)), px(9), p.accent);
    // A word under it: on a slow disk the round spinner alone reads as a frozen list.
    const std::string waiting = "Reading the library\xE2\x80\xA6";
    ImGui::SetCursorScreenPos(ImVec2(pos.x + (w - text_width(fonts().small, waiting)) / 2, pos.y + px(76)));
    text(fonts().small, p.muted, waiting);
  } else if (panel.rail_tab == 2 && store.sessions.empty()) {
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0, px(40)));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 box_min(pos.x + w / 2 - px(24), pos.y), box_max(pos.x + w / 2 + px(24), pos.y + px(48));
    draw->AddRect(box_min, box_max, p.line);
    draw_icon(draw, "film", ImVec2(pos.x + w / 2 - px(10), pos.y + px(14)), px(20), p.muted);
    ImGui::Dummy(ImVec2(0, px(62)));
    // The footage is the engine's, not the person's: it is here only while it is kept, and that is said where it is seen.
    const std::string title = "No footage kept yet";
    const std::string body = "Timeline records gameplay on disk while it is on. Footage older than the window deletes itself.";
    ImGui::SetCursorPosX((w - text_width(fonts().strong, title)) / 2);
    text(fonts().strong, p.ink, title);
    ImGui::SetCursorPosX(std::max(0.0f, (w - text_width(fonts().small, body)) / 2));
    text(fonts().small, p.muted, body);
    ImGui::SetCursorPosX(std::max(0.0f, (w - text_width(fonts().small, "Save a clip from the footage to keep it for good.")) / 2));
    text(fonts().small, p.muted, "Save a clip from the footage to keep it for good.");
    // The deck's Timeline row opens the viewer over a game; this tab is only the list of what is kept.
    const std::string key = store.chord("view_clip");
    if (!key.empty()) {
      const std::string hint = key + " opens it over your game.";
      ImGui::SetCursorPosX(std::max(0.0f, (w - text_width(fonts().small, hint)) / 2));
      text(fonts().small, p.muted, hint);
    }
  } else if (panel.rail_tab == 2) {
    // The Timeline: one card per session, never one per chunk (a half hour of footage is hundreds of chunks). A card is
    // a stretch of play the engine kept, so its length is what is left of it, and its poster is the engine's own still.
    const float gap = px(10), min_card = px(190);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float full = ImGui::GetWindowWidth();
    const float viewport = ImGui::GetContentRegionAvail().y;
    const float inset = px(2);
    const bool bar = avail < full - 0.5f;
    float layout_w = bar ? avail : full;
    GridSpec grid = card_grid(layout_w, gap, min_card);
    if (!bar) {
      const float thumb_h = std::round((grid.width - inset * 2) * 9.0f / 16.0f);
      const float card_h = inset + thumb_h + px(30);
      const int rows = (static_cast<int>(store.sessions.size()) + grid.columns - 1) / grid.columns;
      if (rows * (card_h + gap) + px(12) > viewport) {
        layout_w = full - ImGui::GetStyle().ScrollbarSize;
        grid = card_grid(layout_w, gap, min_card);
      }
    }
    const int columns = grid.columns;
    panel.rail_columns = columns;
    const float card_w = grid.width;
    const float thumb_w = card_w - inset * 2;
    const float thumb_h = std::round(thumb_w * 9.0f / 16.0f);
    const float card_h = inset + thumb_h + px(30);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int64_t now = now_unix();
    for (size_t i = 0; i < store.sessions.size(); ++i) {
      const UiSession& item = store.sessions[i];
      const int column = static_cast<int>(i) % columns, row = static_cast<int>(i) / columns;
      const ImVec2 min(origin.x + column * (card_w + gap), origin.y + row * (card_h + gap));
      const ImVec2 max(min.x + card_w, min.y + card_h);
      ImGui::SetCursorScreenPos(min);
      ImGui::PushID(item.id.c_str());
      if (static_cast<int>(i) == scroll_to) {
        const float top = ImGui::GetWindowPos().y, bottom = top + ImGui::GetWindowHeight();
        if (min.y < top) scroll_to_glide(ImGui::GetScrollY() - (top - min.y) - px(8));
        else if (max.y > bottom) scroll_to_glide(ImGui::GetScrollY() + (max.y - bottom) + px(8));
      }
      if (!ImGui::IsRectVisible(min, max)) {
        ImGui::Dummy(ImVec2(card_w, card_h));
        ImGui::PopID();
        continue;
      }
      const ImVec2 thumb_min(min.x + inset, min.y + inset);
      ImGui::SetCursorScreenPos(thumb_min);
      const bool clicked = ImGui::InvisibleButton("thumb", ImVec2(thumb_w, thumb_h));
      const bool hovered = ImGui::IsItemHovered();
      const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
      const bool menu = ImGui::IsItemClicked(ImGuiMouseButton_Right);
      if (item.poster.empty()) store.refresh_poster(item.id);
      picture(env, item.poster, "film", thumb_min, ImVec2(thumb_w, thumb_h), 0);
      // What is left of the session, not what it once was: the start of it has usually expired.
      const std::string length = format_duration(static_cast<double>(item.duration_ms) / 1000.0);
      const float lw = text_width(fonts().strong, length) + px(12), lh = fonts().strong->LegacySize + px(4);
      const ImVec2 badge(thumb_min.x, thumb_min.y + thumb_h - lh);
      draw->AddRectFilled(badge, ImVec2(badge.x + lw, badge.y + lh), with_alpha(p.canvas, 0.92f));
      draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(badge.x + px(6), badge.y + px(2)), p.ink, length.c_str());
      if (item.live) {
        // The writer is on this session now: the recording mark's dot, with its word, on a chip the poster cannot
        // eat. A bare dot in the corner reads as a speck of the picture.
        const std::string word = "live";
        const float cw = text_width(fonts().small, word) + px(20);
        const float ch = fonts().small->LegacySize + px(6);
        const ImVec2 chip(thumb_min.x + thumb_w - px(6) - cw, thumb_min.y + px(6));
        draw->AddRectFilled(chip, ImVec2(chip.x + cw, chip.y + ch), with_alpha(p.canvas, 0.92f));
        draw->AddCircleFilled(ImVec2(chip.x + px(7), chip.y + ch / 2), px(3), p.danger);
        draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(chip.x + px(13), chip.y + px(3)), p.ink,
                      word.c_str());
      }
      if (hover > 0.0f) {
        draw->AddRectFilled(thumb_min, ImVec2(thumb_min.x + thumb_w, thumb_min.y + thumb_h), with_alpha(p.canvas, 0.35f * hover));
        const ImVec2 c(thumb_min.x + thumb_w / 2, thumb_min.y + thumb_h / 2);
        const float half = px(16) * (0.6f + 0.4f * hover);
        draw->AddRectFilled(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), with_alpha(p.accent, hover));
        draw_icon(draw, "play", ImVec2(c.x - px(7.5f), c.y - px(7.5f)), px(15), with_alpha(p.on_accent, hover), 1.7f, true);
      }
      const bool lit = hovered || static_cast<int>(i) == panel.rail_cursor;
      const float lit_t = anim_to(ImGui::GetID("lit"), lit ? 1.0f : 0.0f, kRateState);
      const float border = 1.0f + (px(2) - 1.0f) * lit_t;
      draw->AddRect(ImVec2(min.x + border / 2, min.y + border / 2), ImVec2(max.x - border / 2, max.y - border / 2),
                    mix(p.line, p.accent, lit_t), 0, 0, border);
      // The same seamed footer the clips carry: what was played, when, how big, and the menu - one row,
      // one seam between each, so a Timeline card and a clip card read as the same kind of thing.
      const std::string size = format_bytes(static_cast<double>(item.bytes));
      const std::string when = format_when(item.started_at, now);
      const float foot_h = card_h - inset - thumb_h;
      const float foot_top = thumb_min.y + thumb_h;
      const float pad = px(7);
      const float size_cell = text_width(fonts().small, size) + pad * 2;
      const float menu_cell = px(28);
      const float when_cell = when.empty() ? 0.0f : text_width(fonts().small, when) + pad * 2;
      const bool show_when = when_cell > 0.0f && thumb_w - size_cell - menu_cell - when_cell >= px(56);
      const float menu_x0 = thumb_min.x + thumb_w - menu_cell;
      const float size_x0 = menu_x0 - size_cell;
      const float when_x0 = size_x0 - when_cell;
      const float name_x0 = thumb_min.x + pad;
      const float name_w = std::max(0.0f, (show_when ? when_x0 : size_x0) - name_x0 - pad);
      const float small_y = foot_top + (foot_h - fonts().small->LegacySize) * 0.5f;
      draw->AddLine(ImVec2(min.x + 1, foot_top), ImVec2(max.x - 1, foot_top), p.line, 1.0f);
      draw->AddLine(ImVec2(menu_x0, foot_top), ImVec2(menu_x0, max.y - 1), p.line, 1.0f);
      draw->AddLine(ImVec2(size_x0, foot_top), ImVec2(size_x0, max.y - 1), p.line, 1.0f);
      if (show_when) draw->AddLine(ImVec2(when_x0, foot_top), ImVec2(when_x0, max.y - 1), p.line, 1.0f);
      ImGui::SetCursorScreenPos(ImVec2(name_x0, small_y));
      // A session with no game marker is a desktop one: its card says so, where its own tab's name would say nothing.
      text_ellipsis(fonts().small, p.ink, item.title.empty() ? "Desktop" : item.title, name_w);
      if (show_when)
        draw->AddText(fonts().small, fonts().small->LegacySize,
                      ImVec2(when_x0 + (when_cell - text_width(fonts().small, when)) * 0.5f, small_y), p.muted, when.c_str());
      draw->AddText(fonts().small, fonts().small->LegacySize,
                    ImVec2(size_x0 + (size_cell - text_width(fonts().small, size)) * 0.5f, small_y), p.muted, size.c_str());
      ImGui::SetCursorScreenPos(ImVec2(menu_x0 + (menu_cell - px(24)) * 0.5f, foot_top + (foot_h - px(24)) * 0.5f));
      if (icon_button("##menu", "dots", "More", false, 24, 14) || menu) ImGui::OpenPopup("##sessionmenu");
      if (hovered) tooltip((item.title.empty() ? std::string("Desktop") : item.title) + ": temporary footage, deleted when its retention window passes.");
      const bool armed = panel.timeline_erase == item.id && static_cast<int32_t>(env.now_ms - panel.timeline_erase_until) < 0;
      session_menu(env, "##sessionmenu", armed, [&env, id = item.id](const char* action) { run_session_action(env, action, id); });
      ImGui::SetCursorScreenPos(min);
      ImGui::Dummy(ImVec2(card_w, card_h));
      ImGui::PopID();
      if (clicked) run_session_action(env, "preview", item.id);
    }
    const int rows = (static_cast<int>(store.sessions.size()) + columns - 1) / columns;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (card_h + gap)));
    ImGui::Dummy(ImVec2(layout_w, px(8)));
  } else if (list.empty()) {
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0, px(40)));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 box_min(pos.x + w / 2 - px(24), pos.y), box_max(pos.x + w / 2 + px(24), pos.y + px(48));
    draw->AddRect(box_min, box_max, p.line);
    draw_icon(draw, panel.rail_tab == 0 ? "film" : "image", ImVec2(pos.x + w / 2 - px(10), pos.y + px(14)), px(20), p.muted);
    ImGui::Dummy(ImVec2(0, px(62)));
    // A filter that matches nothing says so, and says how to see the rest: "No clips yet" would be a lie.
    const bool filtered = (panel.rail_tab == 0 && !panel.game.empty()) ||
                          (panel.rail_tab == 1 && !panel.screenshot_game.empty());
    const std::string chord = env.store.chord("screenshot");
    const std::string title = filtered ? (panel.rail_tab == 0 ? "No clips from this game" : "No screenshots from this game") : panel.rail_tab == 3 ? "Nothing shared yet" : panel.rail_tab == 0 ? "No clips yet" : "No screenshots yet";
    const std::string body = filtered ? "Pick all games to see the rest."
                             : panel.rail_tab == 3 ? "Clips and screenshots shared with you appear here."
                             : panel.rail_tab == 0 ? "Press Save clip and the last moment lands here."
                                                   : "Press Screenshot, or " + (chord.empty() ? std::string("its hotkey") : chord) +
                                                         ", and it lands here.";
    ImGui::SetCursorPosX((w - text_width(fonts().strong, title)) / 2);
    text(fonts().strong, p.ink, title);
    ImGui::SetCursorPosX(std::max(0.0f, (w - text_width(fonts().small, body)) / 2));
    text(fonts().small, p.muted, body);
  } else {
    const float gap = px(10), min_card = px(190);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float full = ImGui::GetWindowWidth();
    const float viewport = ImGui::GetContentRegionAvail().y;
    const float inset = px(2);
    // Build the grid from the width the cards really have. When the list overflows, the vertical
    // scrollbar takes width away; reserving it before the cards are placed keeps the column count from
    // flipping between frames at a threshold.
    const bool bar = avail < full - 0.5f;
    float layout_w = bar ? avail : full;
    GridSpec grid = card_grid(layout_w, gap, min_card);
    if (!bar) {
      const float thumb_h = std::round((grid.width - inset * 2) * 9.0f / 16.0f);
      const float card_h = inset + thumb_h + px(30);
      const int rows = (static_cast<int>(list.size()) + grid.columns - 1) / grid.columns;
      if (rows * (card_h + gap) + px(12) > viewport) {
        layout_w = full - ImGui::GetStyle().ScrollbarSize;
        grid = card_grid(layout_w, gap, min_card);
      }
    }
    const int columns = grid.columns;
    panel.rail_columns = columns;
    const float card_w = grid.width;
    const float thumb_w = card_w - inset * 2;
    const float thumb_h = std::round(thumb_w * 9.0f / 16.0f);
    const float card_h = inset + thumb_h + px(30);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const std::string highlight = store.newest;
    const int64_t now = now_unix();
    // C4: the keyboard's card has one ring; it glides from card to card on the hover token and fades out when no card has it.
    ImVec2 cursor_min(0, 0), cursor_max(0, 0);
    bool cursor_here = false;
    for (size_t i = 0; i < list.size(); ++i) {
      const UiMedia& item = list[i];
      const int column = static_cast<int>(i) % columns, row = static_cast<int>(i) / columns;
      const ImVec2 min(origin.x + column * (card_w + gap), origin.y + row * (card_h + gap));
      const ImVec2 max(min.x + card_w, min.y + card_h);
      if (static_cast<int>(i) == panel.rail_cursor) {
        cursor_min = min;
        cursor_max = max;
        cursor_here = true;
      }
      ImGui::SetCursorScreenPos(min);
      ImGui::PushID(item.file.c_str());
      if (static_cast<int>(i) == scroll_to) {
        // The card the arrows moved to comes into view; the scroll applies on the next frame.
        const float top = ImGui::GetWindowPos().y, bottom = top + ImGui::GetWindowHeight();
        if (min.y < top) scroll_to_glide(ImGui::GetScrollY() - (top - min.y) - px(8));
        else if (max.y > bottom) scroll_to_glide(ImGui::GetScrollY() + (max.y - bottom) + px(8));
      }
      if (!ImGui::IsRectVisible(min, max)) {
        ImGui::Dummy(ImVec2(card_w, card_h));
        ImGui::PopID();
        continue;
      }
      const bool newest = item.file == highlight;
      const ImVec2 thumb_min(min.x + inset, min.y + inset);
      ImGui::SetCursorScreenPos(thumb_min);
      const bool clicked = ImGui::InvisibleButton("thumb", ImVec2(thumb_w, thumb_h));
      const bool hovered = ImGui::IsItemHovered();
      if (hovered) tooltip(item.folder.empty() ? item.name : item.name + "\n" + item.folder);
      const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
      const float press = press_t();  // C3: the card sinks a pixel and shades while it is held
      const bool menu = ImGui::IsItemClicked(ImGuiMouseButton_Right);
      // C2: the picture grows 3% under the pointer (the same texture, a tighter window).
      const float sink = px(1) * press;
      thumbnail(env, item, ImVec2(thumb_min.x + sink, thumb_min.y + sink), ImVec2(thumb_w - 2 * sink, thumb_h - 2 * sink), 0,
                1.0f);
      if (press > 0.0f) draw->AddRectFilled(thumb_min, ImVec2(thumb_min.x + thumb_w, thumb_min.y + thumb_h), with_alpha(p.canvas, 0.2f * press));
      if (item.clip) {
        // The length on the frame's corner, in the card's own ink: the accent belongs to the pointer.
        const std::string length = format_duration(item.duration);
        const float lw = text_width(fonts().strong, length) + px(12), lh = fonts().strong->LegacySize + px(4);
        const ImVec2 badge(thumb_min.x, thumb_min.y + thumb_h - lh);
        draw->AddRectFilled(badge, ImVec2(badge.x + lw, badge.y + lh), with_alpha(p.canvas, 0.92f));
        draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(badge.x + px(6), badge.y + px(2)), p.ink, length.c_str());
      }
      if (hover > 0.0f) {
        // The play mark grows out of the picture as the pointer arrives.
        draw->AddRectFilled(thumb_min, ImVec2(thumb_min.x + thumb_w, thumb_min.y + thumb_h), with_alpha(p.canvas, 0.35f * hover));
        const ImVec2 c(thumb_min.x + thumb_w / 2, thumb_min.y + thumb_h / 2);
        const float half = px(16) * (0.6f + 0.4f * hover);
        draw->AddRectFilled(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), with_alpha(p.accent, hover));
        draw_icon(draw, item.clip ? "play" : "expand", ImVec2(c.x - px(7.5f), c.y - px(7.5f)), px(15), with_alpha(p.on_accent, hover), 1.7f, item.clip);
      }
      const bool lit = newest || hovered;
      const float lit_t = anim_to(ImGui::GetID("lit"), lit ? 1.0f : 0.0f, kRateState);
      const float border = 1.0f + (px(2) - 1.0f) * lit_t;
      draw->AddRect(ImVec2(min.x + border / 2, min.y + border / 2), ImVec2(max.x - border / 2, max.y - border / 2),
                    mix(p.line, p.accent, lit_t), 0, 0, border);
      // The caption is one row of cells: the file's own name (a screenshot carries its icon), when it was saved, its
      // size and the menu, divided by the same 1 px seams the panes use. Nothing stands in for a separator, and the row
      // stays one row - the name gives way first, the size never does.
      const std::string size = format_bytes(static_cast<double>(item.size));
      const std::string when = format_when(item.mtime, now);
      const float foot_h = card_h - inset - thumb_h;
      const float foot_top = thumb_min.y + thumb_h;
      const float pad = px(7);
      // A screenshot is marked as one; a clip is already a clip, so it carries no mark.
      const float icon_w = item.clip ? 0.0f : px(14), icon_gap = item.clip ? 0.0f : px(6);
      const float size_cell = text_width(fonts().small, size) + pad * 2;
      const float menu_cell = px(28);
      const float when_cell = when.empty() ? 0.0f : text_width(fonts().small, when) + pad * 2;
      const bool show_when = when_cell > 0.0f && thumb_w - size_cell - menu_cell - when_cell >= px(56);
      const float menu_x0 = thumb_min.x + thumb_w - menu_cell;
      const float size_x0 = menu_x0 - size_cell;
      const float when_x0 = size_x0 - when_cell;
      const float name_x0 = thumb_min.x + pad;
      const float name_w = std::max(0.0f, (show_when ? when_x0 : size_x0) - name_x0 - pad - icon_w - icon_gap);
      const float small_y = foot_top + (foot_h - fonts().small->LegacySize) * 0.5f;
      draw->AddLine(ImVec2(min.x + 1, foot_top), ImVec2(max.x - 1, foot_top), p.line, 1.0f);
      draw->AddLine(ImVec2(menu_x0, foot_top), ImVec2(menu_x0, max.y - 1), p.line, 1.0f);
      draw->AddLine(ImVec2(size_x0, foot_top), ImVec2(size_x0, max.y - 1), p.line, 1.0f);
      if (show_when) draw->AddLine(ImVec2(when_x0, foot_top), ImVec2(when_x0, max.y - 1), p.line, 1.0f);
      if (!item.clip) draw_icon(draw, "image", ImVec2(name_x0, foot_top + (foot_h - icon_w) * 0.5f), icon_w, p.muted, 1.4f);
      ImGui::SetCursorScreenPos(ImVec2(name_x0 + icon_w + icon_gap, small_y));
      text_ellipsis(fonts().small, p.ink, item.name, name_w);
      if (show_when)
        draw->AddText(fonts().small, fonts().small->LegacySize,
                      ImVec2(when_x0 + (when_cell - text_width(fonts().small, when)) * 0.5f, small_y), p.muted, when.c_str());
      draw->AddText(fonts().small, fonts().small->LegacySize,
                    ImVec2(size_x0 + (size_cell - text_width(fonts().small, size)) * 0.5f, small_y), p.muted, size.c_str());
      ImGui::SetCursorScreenPos(ImVec2(menu_x0 + (menu_cell - px(24)) * 0.5f, foot_top + (foot_h - px(24)) * 0.5f));
      if (icon_button("##menu", "dots", "More", false, 24, 14) || menu) ImGui::OpenPopup("##cardmenu");
      media_menu(env, "##cardmenu", item, list);
      ImGui::SetCursorScreenPos(min);
      ImGui::Dummy(ImVec2(card_w, card_h));
      ImGui::PopID();
      if (clicked) {
        open_preview(env, item, list);
        g_preview_origin = PreviewOrigin{true, min, max, anim_detail::wall_seconds()};
        g_opening = OpeningState{true, item.file, anim_detail::wall_seconds()};
      }
      if (g_opening.set && g_opening.file == item.file) {
        const double age = anim_detail::wall_seconds() - g_opening.at;
        if (age < 5.0) {
          // 30 frames a second while it runs, the one loop the card allows.
          anim_touch_slow();
          const float bar = card_w * 0.35f;
          const float sweep = static_cast<float>(std::fmod(age * 1.2, 1.0));
          const float x = min.x + (card_w + bar) * sweep - bar;
          draw->AddRectFilled(ImVec2(std::max(min.x, x), max.y - px(2)), ImVec2(std::min(max.x, x + bar), max.y), p.accent);
        } else {
          g_opening.set = false;
        }
      }
    }
    // C4: the ring follows the card it was last on while it is off, so a return to the grid glides from there.
    const float ring_t = anim_to(ImGui::GetID("##ringon"), cursor_here ? 1.0f : 0.0f, kRateHover);
    auto ring_axis = [&](const char* name, float target) {
      const ImGuiID id = ImGui::GetID(name);
      return cursor_here ? anim_to(id, target, kRateHover) : ImGui::GetStateStorage()->GetFloat(id, target);
    };
    const ImVec2 ring_min(ring_axis("##ringx0", cursor_min.x), ring_axis("##ringy0", cursor_min.y));
    const ImVec2 ring_max(ring_axis("##ringx1", cursor_max.x), ring_axis("##ringy1", cursor_max.y));
    if (ring_t > 0.0f) {
      const float border = px(2);
      draw->AddRect(ImVec2(ring_min.x + border / 2, ring_min.y + border / 2), ImVec2(ring_max.x - border / 2, ring_max.y - border / 2),
                    with_alpha(p.accent, ring_t), 0, 0, border);
    }
    const int rows = (static_cast<int>(list.size()) + columns - 1) / columns;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (card_h + gap)));
    ImGui::Dummy(ImVec2(layout_w, px(4)));
  }
  fade_since(tab_draw, tab_vertex, tab_t);
  shift_since(tab_draw, tab_vertex, px(6) * (1.0f - tab_t) * tab_direction, 0.0f);
  ImGui::EndChild();
  end_pane();
  ImGui::EndChild();
}

// ---------------------------------------------------------------- status strip

void status_strip(ViewEnv& env) {
  // The bottom bar: a square dot and the state on the left, and the help key at the right.
  const Json& status = env.store.status;
  const StatusLine line = status_line(status, env.store.connected);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = px(26);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), pal().canvas);
  draw->AddLine(pos, ImVec2(pos.x + w, pos.y), pal().accent, px(1));
  const float cy = pos.y + h / 2;
  const ImU32 dot = tone_color(line.tone);
  draw->AddRectFilled(ImVec2(pos.x + px(12), cy - px(3.5f)), ImVec2(pos.x + px(19), cy + px(3.5f)), dot);
  ImFont* small = fonts().small;
  const float text_y = cy - small->LegacySize / 2;
  const float x = pos.x + px(28);
  float right = pos.x + w - px(12);
  {
    // The way to the guide: always at the right end of the strip, and a button.
    // One font for both words: the keycap and its label share a baseline instead of sitting at two heights.
    const float hint_w = text_width(small, "F1") + px(6) + text_width(small, "help");
    right -= hint_w;
    ImGui::SetCursorScreenPos(ImVec2(right - px(4), pos.y));
    if (ImGui::InvisibleButton("##keyshint", ImVec2(hint_w + px(8), h))) env.panel.keys_open = true;
    const bool over = ImGui::IsItemHovered();
    draw->AddText(small, small->LegacySize, ImVec2(right, text_y), pal().accent, "F1");
    draw->AddText(small, small->LegacySize, ImVec2(right + text_width(small, "F1") + px(6), text_y), over ? pal().ink : pal().muted, "help");
    ImGui::SetCursorScreenPos(pos);
    right -= px(12);
  }
  float limit = right;
  if (status["recording"].as_bool()) {
    const std::string recording = "rec " + format_duration(status["recording_seconds"].as_double());
    const float rw = text_width(fonts().strong, recording) + px(24);
    const float text_end = std::min(x + text_width(small, line.tone == Tone::ok ? std::string("buffering") : line.text), limit - rw - px(16));
    const bool lit = (env.now_ms / 500) % 2 == 0;  // the same beat animation_reason asks frames on
    const ImVec2 tag(text_end + px(16), pos.y + px(4));
    draw->AddRectFilled(tag, ImVec2(tag.x + rw, pos.y + h - px(4)), lit ? pal().rec : with_alpha(pal().rec, 0.55f));
    draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(tag.x + px(12), cy - fonts().strong->LegacySize / 2), pal().canvas,
                  recording.c_str());
    limit = text_end;
  }
  // The buffer pane carries the count; the strip says only the state, and the long line when it is news.
  // It is ellipsised rather than clipped, so a fault never ends mid-word with the cause cut off.
  const std::string state_text = line.tone == Tone::ok ? "buffering" : line.tone == Tone::idle ? "paused" : line.text;
  const float stripe_start = x + text_width(small, state_text) + px(20);
  if (!status["recording"].as_bool() && stripe_start + px(56) < limit) {
    for (int i = 0; i < 5; ++i) {
      const float sx = stripe_start + px(static_cast<float>(i) * 9);
      draw->AddLine(ImVec2(sx + px(10), pos.y + px(6)), ImVec2(sx, pos.y + h - px(6)), pal().line_strong, px(2));
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(x, text_y));  // the cursor was left on the help hint: the state sits beside the dot
  text_ellipsis(small, line.tone == Tone::bad ? pal().danger : pal().ink, state_text, std::max(0.0f, limit - x - px(2)));
  if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(x, pos.y), ImVec2(limit, pos.y + h))) {
    const std::string full = line.text.empty() ? state_text : line.text;
    if (text_width(small, full) > std::max(0.0f, limit - x - px(2))) ImGui::SetTooltip("%s", full.c_str());
  }
  ImGui::SetCursorScreenPos(pos);
  ImGui::Dummy(ImVec2(w, h));
}

// ---------------------------------------------------------------- screenshot preview

void lightbox(ViewEnv& env) {
  PanelState& panel = env.panel;
  hint_keepout(ImVec2(0, 0), ImVec2(0, 0));
  if (!panel.preview_open) {
    g_preview_origin.set = false;
    env.shell.clip_picture_close();  // a preview that is no longer open takes its clip's page with it
    return;
  }
  if (panel.preview_files.empty()) {
    panel.preview_open = false;
    return;
  }
  panel.preview_index = std::clamp(panel.preview_index, 0, static_cast<int>(panel.preview_files.size()) - 1);
  if (!panel.preview_zoom) {  // fitted: nothing is zoomed or moved (opening, stepping and closing all land here)
    panel.preview_z = 1.0f;
    panel.preview_pan_x = panel.preview_pan_y = 0.0f;
  }
  const UiMedia* found = find_media(env, panel.preview_files[static_cast<size_t>(panel.preview_index)]);
  if (!found) {
    // It was moved, renamed in Explorer or deleted: say so instead of the picture just vanishing.
    const bool others = panel.preview_files.size() > 1;
    panel.preview_files.erase(panel.preview_files.begin() + panel.preview_index);
    if (others) env.store.toast(UiToast::info, "That file is gone", "It was moved or deleted, so the preview moved on.");
    if (panel.preview_files.empty()) {
      panel.preview_open = false;
      env.store.toast(UiToast::info, "That file is gone", "It was moved or deleted, so the preview closed.");
    }
    return;
  }
  const UiMedia item = *found;
  std::vector<UiMedia> list;
  for (const std::string& file : panel.preview_files) {
    if (const UiMedia* entry = find_media(env, file)) list.push_back(*entry);
  }
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float top = px(36);
  ImGui::SetNextWindowPos(ImVec2(0, top));
  ImGui::SetNextWindowSize(ImVec2(display.x, display.y - top));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, pal().canvas);
  ImGui::Begin("##preview", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  const int count = static_cast<int>(panel.preview_files.size());
  // Back and Esc: a preview opened from the quick panel goes back to it (the main panel is replaced there), any other just closes.
  auto leave = [&] {
    panel.preview_open = false;
    if (panel.preview_from_compact) {
      panel.preview_from_compact = false;
      env.shell.show("compact");
    }
  };
  auto step = [&](int delta) {
    panel.preview_index = (panel.preview_index + delta + count) % count;
    panel.preview_zoom = false;
  };
  if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !panel.popup_was_open) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) leave();
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step(1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step(-1);
  }
  // The header is a pane of its own: what it is, where it is, and the card's actions.
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float gap = px(kGap);
  const bool received = shared_item(item);
  const ImVec2 head_min(gap, origin.y + gap), head_max(display.x - gap, origin.y + gap + px(40));
  draw_pane(draw, head_min, head_max, item.clip ? "clip" : "screenshot", true);
  // The clip's page is a window of its own over the picture area: a hint there would be drawn under it.
  if (item.clip) hint_keepout(ImVec2(0, head_max.y + gap), ImVec2(display.x, display.y));
  // Left to right: the file's own name with when it was made on one line (a rename shows here at once), then the way
  // through the others (previous, where this one is, next), then the card's actions, which end a margin short of the
  // right edge like every other bar.
  const float actions_w = px(304);
  const float actions_x = head_max.x - px(10) - actions_w;
  const std::string place = std::to_string(panel.preview_index + 1) + " / " + std::to_string(count);
  const float counter_w = std::max(px(48), text_width(fonts().strong, place) + px(12));
  const float nav_w = px(56) + counter_w;
  const float nav_x = actions_x - nav_w - px(12);
  const float info_w = std::max(0.0f, (count > 1 ? nav_x : actions_x) - head_min.x - px(26));
  const size_t dot = item.file.find_last_of('.');
  std::string kind = dot == std::string::npos ? std::string() : item.file.substr(dot + 1);
  for (char& c : kind) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const std::string when = format_when(item.mtime, now_unix());
  const std::string rest = " \xC2\xB7 " + when + " \xC2\xB7 " + format_bytes(static_cast<double>(item.size)) +
                           (kind.empty() ? std::string() : " \xC2\xB7 " + kind);
  const std::string shared_notice = "Shared by a friend";
  const float notice_width = received ? text_width(fonts().small, shared_notice) : 0.0f;
  const float info_at_x = head_min.x + px(14);
  const float notice = received ? notice_width + px(12) : 0.0f;
  const float rest_w = text_width(fonts().strong, rest);
  const float name_cap = std::max(0.0f, info_w - notice - rest_w - px(4));
  const ImVec2 info_at(info_at_x, head_min.y + px(12));
  ImGui::SetCursorScreenPos(info_at);
  text_ellipsis(fonts().strong, pal().ink, item.name, name_cap);
  const float name_w = std::min(text_width(fonts().strong, item.name), name_cap);
  ImGui::SetCursorScreenPos(ImVec2(info_at.x + name_w + px(4), info_at.y));
  text_ellipsis(fonts().strong, pal().muted, rest, std::max(0.0f, info_w - notice - name_w - px(4)));
  if (received) {
    draw->AddText(fonts().small, fonts().small->LegacySize,
                  ImVec2(info_at.x + info_w - notice_width, info_at.y), pal().accent, shared_notice.c_str());
  }
  tooltip_rect(ImVec2(info_at.x, head_min.y), ImVec2(info_at.x + info_w, head_max.y),
               item.name + (item.folder.empty() ? std::string() : "\n" + item.folder));
  if (count > 1) {
    ImGui::SetCursorScreenPos(ImVec2(nav_x, head_min.y + px(6)));
    if (icon_button("##prev", "chevronLeft", "Previous (Left arrow)", false, 28, 15)) step(-1);
    const ImVec2 cell(nav_x + px(28), head_min.y + px(6));
    ImGui::SetCursorScreenPos(ImVec2(cell.x + (counter_w - text_width(fonts().strong, place)) / 2, cell.y + px(6)));
    text(fonts().strong, pal().ink, place);
    ImGui::SetCursorScreenPos(ImVec2(cell.x + counter_w, cell.y));
    if (icon_button("##next", "chevronRight", "Next (Right arrow)", false, 28, 15)) step(1);
    // A seam between the way through and the actions.
    const float seam = actions_x - px(6);
    draw->AddLine(ImVec2(seam, head_min.y + px(7)), ImVec2(seam, head_max.y - px(7)), pal().line);
  }
  ImGui::SetCursorScreenPos(ImVec2(actions_x, head_min.y + px(6)));
  if (button("Edit##preview", Variant::normal, "scissors", false, 88, 28)) run_media_action(env, "edit", item, list);
  ImGui::SameLine(0, px(4));
  if (icon_button("##share", "share", "Share", false, 28, 15)) run_media_action(env, "share", item, list);
  ImGui::SameLine(0, px(2));
  if (icon_button("##copy", "copy", "Copy to clipboard", false, 28, 15)) run_media_action(env, "copy", item, list);
  ImGui::SameLine(0, px(2));
  if (icon_button("##reveal", "folder", "Show in folder", false, 28, 15)) run_media_action(env, "reveal", item, list);
  ImGui::SameLine(0, px(2));
  if (icon_button("##rename", "edit", "Rename", false, 28, 15)) run_media_action(env, "rename", item, list);
  ImGui::SameLine(0, px(2));
  if (icon_button("##delete", "trash", "Delete", false, 28, 15, true)) run_media_action(env, "delete", item, list);
  ImGui::SameLine(0, px(8));
  if (button("Back##preview", Variant::ghost, nullptr, false, 56, 28)) leave();
  tooltip("Back (Esc)");

  // The picture: a clip is the page's own, a screenshot is fitted here and zooms with the wheel (middle button moves it).
  const ImVec2 area_min(gap, head_max.y + gap);
  const ImVec2 area_size(display.x - gap * 2, std::max(0.0f, display.y - area_min.y - gap));
  draw->AddRect(area_min, ImVec2(area_min.x + area_size.x, area_min.y + area_size.y), pal().line);
  if (item.clip) {
    // A clip is the page's own picture: a WebView2 child of this window, placed on this rectangle, so nothing is drawn over it.
    // A sheet over the picture hides it; a file the page cannot play says so here, with the screenshot's own way out.
    const RECT place{static_cast<LONG>(area_min.x + 1), static_cast<LONG>(area_min.y + 1), static_cast<LONG>(area_min.x + area_size.x - 1),
                     static_cast<LONG>(area_min.y + area_size.y - 1)};
    // A sheet or a popup that the panel draws over the picture (keys_free counts the preview itself, so it is not used here).
    const bool sheet_up = panel.share_open || panel.rename_open || panel.remove_open || panel.buffer_stop_open || panel.recorder_open ||
                          panel.keys_open || !panel.recording.empty() || !panel.link_offer.empty() ||
                          ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    const ClipPicture picture = env.shell.clip_picture(item.file, place, sheet_up);
    if (picture == ClipPicture::playing) g_opening.set = false;  // 6.6: the card's sweep ends when the picture answers
    if (picture == ClipPicture::failed) {
      const std::string message = "This file could not be shown here.";
      ImGui::SetCursorScreenPos(ImVec2(area_min.x + (area_size.x - text_width(fonts().body, message)) / 2, area_min.y + area_size.y / 2 - px(30)));
      text(fonts().body, pal().muted, message);
      // The panel's page plays what WebView2 decodes. The preview window makes an H.264 copy for
      // anything else, so a format the page refuses still has a way to play inside BuffrX.
      ImGui::SetCursorScreenPos(ImVec2(area_min.x + area_size.x / 2 - px(170), area_min.y + area_size.y / 2));
      if (button("Preview in BuffrX##preview", Variant::normal, "play")) {
        Json files = Json::array();
        files.push(Json(item.file));
        Json args = Json::object();
        args.set("files", files);
        args.set("preview", Json(true));
        env.store.attempt("editor.open", args, "The preview did not open");
      }
      tooltip("Opens the preview window, which can make this format play");
      ImGui::SameLine(0, px(6));
      if (button("Open file##preview", Variant::normal, "external")) run_media_action(env, "open", item, list);
      tooltip("Open in the default app");
    } else if (picture != ClipPicture::playing) {
      spinner(ImGui::GetWindowDrawList(), ImVec2(area_min.x + area_size.x / 2, area_min.y + area_size.y / 2), px(12), pal().accent);
    }
  } else {
    // A screenshot fills the same frame a clip does: the area edge to edge, an accent line inside its border. It is fitted;
    // the wheel zooms around the pointer, the middle button drags the zoomed picture, a double click fits it again.
    ImGui::SetCursorScreenPos(ImVec2(area_min.x + 1, area_min.y + 1));
    const ImVec2 inner(area_size.x - 2, area_size.y - 2);
    ImGui::BeginChild("##image", inner, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 size;
    bool failed = false;
    // Zoomed, the picture is decoded no larger than the display it is shown on: a 4K shot on a 1080p panel gains
    // nothing from a 4K texture, and decoding every one of them at 8192 px is what held gigabytes (B17). 4096 when the
    // monitor is unknown.
    const int display_box = std::max(env.display_w, env.display_h);
    const int zoom_box = display_box > 0 ? std::min(display_box, 8192) : 4096;
    const ImTextureID texture = env.images.get(item.file, panel.preview_zoom ? zoom_box : 2560, &size, &failed);
    const ImVec2 child_min = ImGui::GetCursorScreenPos();
    const ImVec2 child_max(child_min.x + inner.x, child_min.y + inner.y);
    if (texture && size.x > 0) {
      g_opening.set = false;  // 6.6: the card's sweep ends when the picture is ready
      ImGuiIO& io = ImGui::GetIO();
      const float fit = std::min(1.0f, std::min(inner.x / size.x, inner.y / size.y));  // all of it in view, never past its own pixels
      const float zoom_max = std::clamp(4.0f / fit, 1.0f, 32.0f);                         // up to four screen pixels to a picture pixel
      float z = std::clamp(panel.preview_z, 1.0f, zoom_max);
      ImVec2 pan(panel.preview_pan_x, panel.preview_pan_y);
      const ImVec2 center(child_min.x + inner.x / 2, child_min.y + inner.y / 2);
      ImGui::InvisibleButton("##picture", inner);
      const bool hover = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
      if (hover && io.MouseWheel != 0.0f) {
        // The point under the pointer stays under it.
        const float next = std::clamp(z * std::pow(1.2f, io.MouseWheel), 1.0f, zoom_max);
        const ImVec2 at(io.MousePos.x - center.x, io.MousePos.y - center.y);
        const float ratio = next / z;
        pan = ImVec2(at.x - (at.x - pan.x) * ratio, at.y - (at.y - pan.y) * ratio);
        z = next;
      }
      if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panel.preview_panning = true;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panel.preview_panning = false;
      if (panel.preview_panning) {
        pan = ImVec2(pan.x + io.MouseDelta.x, pan.y + io.MouseDelta.y);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      }
      if (hover && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        z = 1.0f;
        pan = ImVec2(0, 0);
      }
      const ImVec2 shown(size.x * fit * z, size.y * fit * z);
      // The picture cannot be pulled off the area: a side that fits stays centred, a side that overflows keeps its edge in view.
      const float room_x = std::max(0.0f, (shown.x - inner.x) / 2), room_y = std::max(0.0f, (shown.y - inner.y) / 2);
      pan = ImVec2(std::clamp(pan.x, -room_x, room_x), std::clamp(pan.y, -room_y, room_y));
      panel.preview_z = z;
      panel.preview_pan_x = pan.x;
      panel.preview_pan_y = pan.y;
      panel.preview_zoom = z > 1.001f;
      const ImVec2 at_min(std::floor(center.x + pan.x - shown.x / 2), std::floor(center.y + pan.y - shown.y / 2));
      const ImVec2 at_max(at_min.x + std::round(shown.x), at_min.y + std::round(shown.y));
      const float grow = g_preview_origin.set && !panel.preview_zoom
                             ? ease_out(progress(anim_detail::wall_seconds() - g_preview_origin.at, kDurBase))
                             : 1.0f;
      if (grow < 1.0f) {
        // Still growing: the picture is drawn from the card's rectangle to its place (foreground, so the child does not clip it).
        anim_touch();
        const ImVec2 from_min = g_preview_origin.min, from_max = g_preview_origin.max;
        ImGui::GetForegroundDrawList()->AddImage(
            texture, ImVec2(from_min.x + (at_min.x - from_min.x) * grow, from_min.y + (at_min.y - from_min.y) * grow),
            ImVec2(from_max.x + (at_max.x - from_max.x) * grow, from_max.y + (at_max.y - from_max.y) * grow));
      } else {
        ImGui::GetWindowDrawList()->AddImage(texture, at_min, at_max);  // the child's clip keeps a zoomed picture inside the frame
      }
      if (panel.preview_zoom) {
        // What the wheel has done, and the way back; nothing is drawn over a picture that is simply fitted.
        char level[64];
        std::snprintf(level, sizeof(level), "%.1fx  \xC2\xB7  double click to fit", static_cast<double>(z));
        const float tag_w = text_width(fonts().small, level) + px(16), tag_h = px(24);
        const ImVec2 tag_min(child_min.x + px(10), child_max.y - tag_h - px(10));
        ImDrawList* over = ImGui::GetWindowDrawList();
        over->AddRectFilled(tag_min, ImVec2(tag_min.x + tag_w, tag_min.y + tag_h), with_alpha(pal().canvas, 0.82f));
        over->AddRect(tag_min, ImVec2(tag_min.x + tag_w, tag_min.y + tag_h), pal().line_strong);
        over->AddText(fonts().small, fonts().small->LegacySize, ImVec2(tag_min.x + px(8), tag_min.y + (tag_h - fonts().small->LegacySize) / 2),
                      pal().ink, level);
      } else {
        tooltip("Scroll to zoom, middle-drag to move");
      }
    } else if (failed) {
      const std::string message = "This file could not be shown here.";
      ImGui::SetCursorScreenPos(ImVec2(child_min.x + (area_size.x - text_width(fonts().body, message)) / 2, child_min.y + area_size.y / 2 - px(30)));
      text(fonts().body, pal().muted, message);
      ImGui::SetCursorScreenPos(ImVec2(child_min.x + area_size.x / 2 - px(110), child_min.y + area_size.y / 2));
      if (button("Open in the default app##preview", Variant::normal, "external")) run_media_action(env, "open", item, list);
    } else {
      spinner(ImGui::GetWindowDrawList(), ImVec2(child_min.x + area_size.x / 2, child_min.y + area_size.y / 2), px(12), pal().accent);
    }
    // The clip's page draws one accent line just inside the border; the screenshot's is drawn here, over the picture.
    ImGui::GetWindowDrawList()->AddRect(child_min, child_max, pal().accent);
    ImGui::EndChild();
  }
  // Previous and next again over the image, where the pointer already is: larger square keys at the sides, framed in
  // the accent like the focused pane so they are seen at once. A clip's picture is the page's: its keys are the header's.
  if (count > 1 && !item.clip) {
    for (int side = 0; side < 2; ++side) {
      const float half = px(24);
      const float cx = side == 0 ? area_min.x + px(40) : area_min.x + area_size.x - px(40);
      const ImVec2 c(cx, area_min.y + area_size.y / 2);
      ImGui::SetCursorScreenPos(ImVec2(c.x - half, c.y - half));
      ImGui::PushID(side);
      const bool pressed = ImGui::InvisibleButton("step", ImVec2(half * 2, half * 2));
      const bool hovered = ImGui::IsItemHovered();
      ImGui::PopID();
      ImDrawList* fg = ImGui::GetWindowDrawList();
      fg->AddRectFilled(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), hovered ? pal().accent : with_alpha(pal().canvas, 0.82f));
      fg->AddRect(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), pal().accent, 0.0f, 0, px(hovered ? 2.0f : 1.0f));
      draw_icon(fg, side == 0 ? "chevronLeft" : "chevronRight", ImVec2(c.x - px(11), c.y - px(11)), px(22),
                hovered ? pal().on_accent : pal().ink);
      if (pressed) step(side == 0 ? -1 : 1);
    }
  }
  ImGui::End();
}

// ---------------------------------------------------------------- the panel

}  // namespace

void draw_panel(ViewEnv& env) {
  PanelState& panel = env.panel;
  static bool settings_was_open = false;
  const bool settings_open = !panel.settings.empty();
  if (settings_open && !settings_was_open) ++panel.settings_open_epoch;
  settings_was_open = settings_open;
  timeline_notices(env.store);  // a writer state change is said once, by whichever window is open
  if (panel.settings.empty()) panel.settings_loaded.clear();  // reopening a section reads it again
  // After a rebind, say so when Windows refused the chord.
  if (!panel.check_hotkey.empty() && env.store.hotkeys_version > panel.check_hotkey_version) {
    const std::string problem = env.store.hotkeys["problems"][panel.check_hotkey].as_string();
    if (!problem.empty()) env.store.toast(UiToast::error, "Saved, but Windows refused it", problem);
    panel.check_hotkey.clear();
  }
  // The panel's keys first: while Settings is open they read nothing, so the Escape below only leaves Settings. Read the
  // other way round, one press left Settings and then hid the window.
  panel_keys(env);
  // Escape leaves Settings (Rewind's key); the digits jump between its sections.
  if (!panel.settings.empty() && !panel.preview_open && panel.recording.empty() && !panel.popup_was_open &&
      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    panel.settings.clear();
  }
  titlebar(env, panel.settings.empty() ? std::string() : std::string("settings"), [&] {
    // An update waiting is the one thing the bar must say without being asked: the state beside the
    // badge, whose popup offers Restart or Later.
    if (parts::update_badge(env, false, true)) ImGui::SameLine();
    if (bar_button(env, "##editor", "scissors", "Open the editor")) open_editor(env, {});
    ImGui::SameLine();
    if (bar_button(env, "##quick", "compact", "Compact mode")) env.shell.show("compact");
    ImGui::SameLine();
    if (bar_button(env, "##settings", "settings", panel.settings.empty() ? "Settings" : "Back to the panel (Esc)",
                   !panel.settings.empty())) {
      panel.settings = panel.settings.empty() ? "capture" : std::string();
    }
  });
  const float strip = px(26);
  const float gap = px(kGap);
  // The panes end a gap above the status strip, as they start a gap under the titlebar: every pane's bottom border shows.
  const float body = ImGui::GetContentRegionAvail().y - strip - gap;
  // Settings and the panel each come in when they open: the picture fades up from the ground, and Settings also rises a
  // few pixels into place.
  const ImVec2 view_at = ImGui::GetCursorScreenPos();
  const ImVec2 view_min(ImGui::GetWindowPos().x, view_at.y);
  const ImVec2 view_max(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), view_at.y + body);
  float view_arrived = 1.0f;
  if (!panel.settings.empty()) {
    view_arrived = appear(ImGui::GetID("##settings_view"), kDurBase);
    const float lift = (1.0f - view_arrived) * px(10);
    const float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(top + lift);
    draw_settings(env, body - lift);
    ImGui::SetCursorPosY(top + body + gap);
  } else {
    view_arrived = appear(ImGui::GetID("##panel_view"), kDurBase);
    const float total = ImGui::GetContentRegionAvail().x - gap * 2;
    // The columns are locked, not dragged: the deck keeps the 400 design units its panes need and the library takes
    // all the rest, which is the most clips the window can show, up to four columns of cards.
    const float rail_w = px(std::max(unpx(total) - 400.0f, 260.0f));
    const float row_top = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + gap, row_top));
    deck(env, total - rail_w - gap, body);
    ImGui::SameLine(0, gap);
    rail(env, rail_w, body);
    ImGui::SetCursorPosY(row_top + body + gap);
  }
  fade_in_from(ImGui::GetForegroundDrawList(), view_min, view_max, pal().canvas, view_arrived);
  status_strip(env);
  lightbox(env);
  media_dialogs(env);
  hotkey_recorder(env);
  panel.popup_was_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  draw_toasts(env.store, ImVec2(display.x - px(16), display.y - px(36)), 340);
}

Animation animation_reason(const ViewEnv& env) {
    constexpr uint32_t kSmooth = 16;   // a fade the eye follows (a save's flash, a row finding its place)
  const Json& status = env.store.status;
  const std::string state = status["buffer_state"].as_string();
  // A stopped buffer draws nothing that moves: its dial is still, so an idle panel does not redraw.
  if (layout_settling()) return {"layout", kSmooth};                                            // a row found its place
  if (!env.store.connected) return {"disconnected", kSpinnerMs};                                  // the spinner
  if (state == "starting" || state == "stopping") return {"buffer_changing", kSpinnerMs};         // the spinner
  // The blinking rec tag toggles every 500 ms on the wall clock; the ticker's seconds arrive with the status.
  if (status["recording"].as_bool()) return {"recording", 500, true};
  for (const Json& receive : env.store.links["receives"].items()) {                            // a clip arriving
    const std::string state = receive["state"].as_string();
    if (state == "connecting" || state == "receiving" || state == "retrying") return {"link_receiving", kSpinnerMs};
  }
  if (env.panel.share_open) {                                                                  // the share sheet's spinner
    const std::string share = env.store.share["state"].as_string();
    if (share == "preparing" || share == "fitting" || share == "posting") return {"share_working", kSpinnerMs};
    // The preview's dissolve between stills is drawn at the display's rate: it is only on screen while the sheet is open.
    if (env.panel.share_playing) return {"share_preview", 16};
  }
  // The strip's Save spinner, and its live edge while it is on screen: one frame each time the edge moves about a pixel
  // (100-500 ms, ui_live.cpp). Only for the window that drew the strip within the last second: the tray panel shares the state.
  const bool strip_shown = env.panel.settings.empty() && env.now_ms - env.panel.live.drawn_at < 1000;
  if (strip_shown && env.panel.live.saving) return {"timeline_saving", kSpinnerMs};
  if (strip_shown && env.panel.live.tick_ms) return {"timeline_live", env.panel.live.tick_ms};
  if (!env.store.toasts.empty()) return {};  // toasts are static; they expire through the store
  if (env.store.pulse_at && env.now_ms - env.store.pulse_at < 1000) return {"save_pulse", kSmooth};  // the tick flash a save leaves
  if (!env.store.library_loaded) return {"library_loading", kSpinnerMs};
  if (env.panel.saving && env.now_ms < env.panel.saving_until + 50) return {"saving", kSpinnerMs};
  if (env.panel.settings == "about" && !env.panel.update_checking && env.panel.update_checked_ms > 0) {
    const int64_t age = static_cast<int64_t>(std::time(nullptr)) - env.panel.update_checked_ms / 1000;
    return {"update_age", age < 60 ? 1000u : 60000u};
  }
  return {};
}


void maybe_show_whats_new(ViewEnv& env) { parts::whats_new(env); }
void maybe_show_update_restart(ViewEnv& env) { parts::update_restart_dialog(env); }
}  // namespace bx::ui
