// Settings: nine pages on a numbered spine. Each page shows the few choices a player
// makes and keeps everything else behind one Advanced disclosure (AGENTS.md: depth never in the way).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>

#include "imgui_internal.h"
#include "../update_math.h"
#include "../update_policy.h"
#include "../../version.h"
#include "../public_links.h"
#include "misc/cpp/imgui_stdlib.h"
#include "../config.h"
#include "../encoder_settings.h"
#include "../game_watch.h"
#include "../themes.h"
#include "ui_parts.h"

namespace bx::ui {
namespace {

using namespace parts;

struct Section {
  const char* id;
  const char* label;
  const char* icon;
  void (*draw)(ViewEnv&);
};
void capture_section(ViewEnv& env);
void encoder_section(ViewEnv& env);
void audio_section(ViewEnv& env);
void profiles_section(ViewEnv& env);
void storage_section(ViewEnv& env);
void hotkeys_section(ViewEnv& env);
void sharing_section(ViewEnv& env);
void theme_section(ViewEnv& env);
void about_section(ViewEnv& env);
void system_section(ViewEnv& env);
// One row per page: the id it is opened by (`settings:<id>`), its label and icon in the spine, and the
// function that draws it. The page index and the switch that used to live in draw_settings cannot drift.
const Section kSections[] = {{"capture", "Capture", "monitor", capture_section},   {"encoder", "Video", "cpu", encoder_section},
                             {"audio", "Audio", "speaker", audio_section},         {"profiles", "Profiles", "layers", profiles_section},
                             {"storage", "Storage", "hdd", storage_section},       {"hotkeys", "Hotkeys", "keyboard", hotkeys_section},
                             {"sharing", "Sharing", "share", sharing_section},     {"theme", "Theme", "palette", theme_section},
                             {"system", "System", "bell", system_section},         {"about", "About", "info", about_section}};
constexpr int kSectionCount = static_cast<int>(sizeof(kSections) / sizeof(kSections[0]));

// Loads a list a section needs once, when the section opens. A caller that names a `failure` says so on a card when the
// list cannot be read; the rest stay quiet, because a list re-read on a slow beat would repeat the same card forever.
void load(ViewEnv& env, const char* command, Json& into, bool& loading, Json args = Json::object(), const char* failure = nullptr) {
  if (loading) return;
  loading = true;
  UiStore& store = env.store;
  store.bridge_call(command, std::move(args), [&into, &loading, &store, failure](bool ok, const Json& data, const std::string&) {
    into = ok ? data : Json::array();
    loading = false;
    // `failure` points at a string literal, so it outlives the reply; the list is empty because the read failed, not because there is none.
    if (!ok && failure) store.toast(UiToast::error, failure, "Close BuffrX and open it again, then try once more.");
  });
}

// A select over integer presets that always shows the current value.
bool int_select(const char* id, int& value, const std::vector<int>& presets, const std::function<std::string(int)>& name,
                float width = 160) {
  const std::vector<int> values = with_current(presets, value);
  std::vector<std::string> labels;
  int selected = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    labels.push_back(name(values[i]));
    if (values[i] == value) selected = static_cast<int>(i);
  }
  if (select(id, labels, selected, width)) {
    value = values[static_cast<size_t>(selected)];
    return true;
  }
  return false;
}

// `hint_color` 0: the hint is a description, shown on hover. A colour: the hint is state, shown on the row.
void switch_row(ViewEnv& env, const char* key, const std::string& title, const std::string& hint = std::string(),
                bool disabled = false, ImU32 hint_color = 0) {
  begin_row(title, hint, hint_color);
  const bool on = env.store.config[key].as_bool();
  if (toggle(key, on, disabled)) set(env, key, Json(!on));
  end_row();
}

// ---------------------------------------------------------------- capture

// ---------------------------------------------------------------- timeline (on the Capture page)

// What Timeline is doing, as a card at the top of its settings: a dot, a headline, one line under it, and the way out when
// it took the wrong program for a game.
void timeline_status_card(ViewEnv& env, const TimelineStatus& status, const std::string& recorded) {
  const float w = ImGui::GetContentRegionAvail().x;
  const float h = px(52);
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(min, ImVec2(min.x + w, min.y + h), pal().canvas);
  draw->AddRect(min, ImVec2(min.x + w, min.y + h), status.tone == 1 ? pal().accent_line : pal().line);
  const ImU32 dot = status.tone == 1 ? pal().accent : status.tone == 2 ? pal().warn : pal().faint;
  draw->AddCircleFilled(ImVec2(min.x + px(18), min.y + h / 2), px(5), dot);
  const float text_x = min.x + px(34);
  const bool button_here = !recorded.empty() && status.tone == 1;
  const float button_w = button_here ? px(112) : 0.0f;
  const float text_w = w - px(34) - px(12) - (button_here ? button_w + px(10) : 0.0f);
  ImGui::SetCursorScreenPos(ImVec2(text_x, min.y + px(9)));
  text_ellipsis(fonts().strong, pal().ink, status.headline, text_w);
  ImGui::SetCursorScreenPos(ImVec2(text_x, min.y + px(28)));
  text_ellipsis(fonts().small, pal().muted, status.detail, text_w);
  if (button_here) {
    ImGui::SetCursorScreenPos(ImVec2(min.x + w - px(12) - button_w, min.y + (h - px(28)) / 2));
    // The button takes its width in design units, like its height, and scales it itself: handed pixels it came out wider
    // than the card on a scaled monitor and ran over its edge.
    if (button("Not a game", Variant::normal, nullptr, false, 112, 28)) {
      Json list = Json::array();
      for (const Json& kept : env.store.config["game_exclusions"].items()) list.push(Json(kept.as_string()));
      list.push(Json(recorded));
      set(env, "game_exclusions", list);
    }
    tooltip("Timeline took this for a game. If it is not one, it stops, and is never taken for one again.");
  }
  ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + h));
  ImGui::Dummy(ImVec2(0, px(8)));
}

// Timeline: footage kept on the disk beside the RAM ring (timeline_state.h). The ring is untouched by it. It sits with the
// other capture choices because it is one: what is recorded, and when.
void timeline_settings(ViewEnv& env) {
  const Json& config = env.store.config;
  const int minutes = static_cast<int>(config["timeline_minutes"].as_int(0));
  const bool on = minutes > 0;
  const bool everywhere = config["timeline_everywhere"].as_bool(false);
  const std::string state = env.store.status["timeline"]["state"].as_string("off");
  const std::string recorded = env.store.status["timeline"]["game"].as_string("");

  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##timeline_capture", "Timeline");
  timeline_status_card(env, timeline_status(state, recorded, everywhere, minutes), recorded);

  begin_row("Keep the last", timeline_disclosure());
  {
    int chosen = timeline_length_index(minutes);
    if (segmented("##timelinelength", timeline_length_choices(), chosen)) {
      set(env, "timeline_minutes", Json(timeline_length_minutes(chosen)));
    }
  }
  end_row();

  // Everything below is about what the window records, so it waits for a window: dimmed, never hidden, so the page does
  // not change shape under a click.
  if (!on) ImGui::BeginDisabled();
  begin_row("Records", everywhere ? "Games and everything else on your screen: the desktop, browsers, any program. Only a long spell with no keyboard or mouse pauses it (below)."
                                  : "Only while a game is running. The rest of the time nothing is written.");
  {
    int where = everywhere ? 1 : 0;
    if (segmented("##timelinewhere", {"Games only", "Everywhere"}, where)) set(env, "timeline_everywhere", Json(where == 1));
  }
  end_row();
  if (!on || everywhere) ImGui::BeginDisabled();
  switch_row(env, "timeline_windowed_games", "Windowed games",
             everywhere ? "Not needed: everywhere records windowed games too."
                        : "Also record when a windowed game is in front, not only a fullscreen one. A game is recognised by the graphics and controller libraries it loads, so a protected game may not be: pick that one under Source.");
  if (!on || everywhere) ImGui::EndDisabled();
  if (!on || !everywhere) ImGui::BeginDisabled();
  begin_row("Pause when idle", "Stops writing the desktop after this long without keyboard or mouse, and writes again when you are back. A game is never paused for it: a controller does not count as input to Windows.");
  {
    int idle = static_cast<int>(config["timeline_idle_minutes"].as_int(0));
    if (int_select("##timelineidle", idle, timeline_idle_choices(), [](int v) { return timeline_idle_label(v); }, 160)) {
      set(env, "timeline_idle_minutes", Json(idle));
    }
  }
  end_row();
  if (!on || !everywhere) ImGui::EndDisabled();
  if (!on) ImGui::EndDisabled();
  note("What is on the disk, Erase, and when old footage is deleted are under Storage.");
  end_section();
}

void capture_section(ViewEnv& env) {
  PanelState& panel = env.panel;
  const Json& config = env.store.config;
  // A monitor plugged in while this page is open has to appear without leaving it: the list is re-read
  // on a slow beat, and the cards keep the last answer until the new one lands.
  const bool displays_stale =
      panel.displays.is_null() || (env.now_ms - panel.displays_at) >= 2000;
  if (displays_stale && !panel.displays_loading) {
    panel.displays_at = env.now_ms;
    load(env, "sources.displays", panel.displays, panel.displays_loading);
  }
  const bool window_mode = config["capture_mode"].as_string() == "window";
  const int source = window_mode ? 2 : config["auto_game_capture"].as_bool() ? 0 : 1;
  if ((window_mode || panel.picking_window) && panel.windows.is_null() && !panel.windows_loading) load(env, "sources.windows", panel.windows, panel.windows_loading);

  begin_section("##source", "Source");
  begin_row("What to capture", source == 0   ? "Your display, switching to a game on its own when one is fullscreen, and back when it closes."
                               : source == 1 ? "Always the whole display. Games are not detected."
                                             : "One window, wherever it is. Nothing else is captured.");
  int choice = source;
  if (segmented("##source", {"Games + display", "Display", "Window"}, choice)) {
    panel.picking_window = choice == 2;
    Json patch = Json::object();
    if (choice < 2) {
      patch.set("auto_game_capture", Json(choice == 0));
      patch.set("capture_mode", Json("display"));
      env.store.set_config(patch);
    } else {
      // The choice is not made until a window is: nothing is applied here.
      panel.windows = Json();
    }
  }
  end_row();
  if (!panel.picking_window && source != 2 && panel.displays.is_array() && !panel.displays.items().empty()) {
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, px(4)));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = px(8);
    const int columns = std::max(1, static_cast<int>((avail + gap) / (px(180) + gap)));
    const float card_w = (avail - gap * (columns - 1)) / columns, card_h = px(52);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    int index = 0;
    for (const Json& display : panel.displays.items()) {
      const int id = static_cast<int>(display["id"].as_int());
      const ImVec2 min(origin.x + (index % columns) * (card_w + gap), origin.y + (index / columns) * (card_h + gap));
      ImGui::SetCursorScreenPos(min);
      ImGui::PushID(id);
      const bool chosen = config["display_id"].as_int(-1) == id && config["capture_mode"].as_string() != "window";
      if (ImGui::InvisibleButton("display", ImVec2(card_w, card_h))) {
        Json args = Json::object();
        args.set("mode", Json("display"));
        args.set("display_id", Json(id));
        env.store.attempt("capture.set", args, "Display not selected", [&store = env.store](const Json&) { store.refresh_config(); });
      }
      const bool hovered = ImGui::IsItemHovered();
      ImDrawList* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(min, ImVec2(min.x + card_w, min.y + card_h), chosen ? pal().raised : pal().canvas);
      draw->AddRect(min, ImVec2(min.x + card_w, min.y + card_h), chosen ? pal().accent : (hovered ? pal().muted : pal().line), 0, 0, chosen ? px(2) : 1.0f);
      draw_icon(draw, "monitor", ImVec2(min.x + px(12), min.y + (card_h - px(20)) / 2), px(20), chosen ? pal().accent : pal().faint);
      std::string name = display["name"].as_string();
      if (name.empty()) name = "Display " + std::to_string(id + 1);
      if (display["primary"].as_bool() && name.find("rimary") == std::string::npos) name += " (Primary)";
      // The name ends in an ellipsis at the card's edge, never under it.
      ImGui::SetCursorScreenPos(ImVec2(min.x + px(42), min.y + px(9)));
      text_ellipsis(fonts().strong, pal().ink, name, card_w - px(42) - px(10));
      const std::string size = std::to_string(display["width"].as_int()) + " \xC3\x97 " + std::to_string(display["height"].as_int());
      draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(min.x + px(42), min.y + px(28)), pal().faint, size.c_str());
      ImGui::PopID();
      ++index;
    }
    const int rows = (index + columns - 1) / columns;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (card_h + gap)));
    ImGui::Dummy(ImVec2(0, px(2)));
  }
  // The picker closes on the pick and this line says what is captured: standing open after a choice, the only answer to a
  // click was a thin border that was already there, so a pick looked like nothing happened. Change brings the list back.
  if (window_mode && !panel.picking_window) {
    std::string capturing = config["window_name"].as_string();
    if (capturing.empty()) capturing = env.store.status["capture"]["target"].as_string();
    // The line is a row like every other one, so its label and its Change box share one centre and one pair of
    // seams. Placed by hand it drifted off both, because the label and the box were measured separately. The box is
    // taller than a usual row, so the row is told its height (in window pixels) and the label centres on it.
    const float change_h = 34;  // design units, as every control height here
    begin_row("Capturing", "The window BuffrX records. Change picks another one.", 0, px(change_h),
              capturing.empty() ? std::string("one window") : capturing);
    if (button("Change##window", Variant::normal, "window", false, px(124), change_h)) {
      // The list is read again on the way in: a window opened since it was last read had to be waited for and refreshed by
      // hand, which is why the list felt stale.
      panel.windows = Json();
      panel.picking_window = true;
    }
    end_row();
  }
  if (panel.picking_window) {
    // The picker's own line is a row too, so its label, its two buttons and the seams above and below line up with every
    // other row in the pane instead of being placed beside them by hand.
    begin_row("Pick the window to capture", "One window is recorded. Cancel keeps the one already chosen.", 0, px(34));
    if (button("Cancel##windows", Variant::ghost, "close", false, 0, 34)) panel.picking_window = false;
    ImGui::SameLine();
    if (button("Refresh##windows", Variant::ghost, "refresh", panel.windows_loading, 0, 34)) {
      panel.windows = Json();
      load(env, "sources.windows", panel.windows, panel.windows_loading);
    }
    end_row();
    if (panel.windows_loading || panel.windows.is_null()) {
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      spinner(ImGui::GetWindowDrawList(), ImVec2(pos.x + ImGui::GetContentRegionAvail().x / 2, pos.y + px(24)), px(8), pal().accent);
      ImGui::Dummy(ImVec2(0, px(48)));
    } else if (panel.windows.items().empty()) {
      text(fonts().small, pal().faint, "No windows found. Open the app, then refresh.");
    } else {
      // One row a window, its title on the left and its executable on the right. The rows take the list's own gap, and
      // the box is told the height they really need: it used to be told 38 px a row while the rows took 44, so a
      // six-window list always carried a scrollbar and cut its last window in half.
      const float row_h = px(40), row_gap = px(6), list_pad = px(6);
      const int count = static_cast<int>(panel.windows.items().size());
      // Whole rows only: a box that stops between two rows cuts the last window in half, and that half row is what made the
      // list look cramped. It grows to eight rows and scrolls for the rest. The padding is set here, not taken from the
      // style, so the height the box is given is the height its rows take.
      const int fit = std::max(3, static_cast<int>((px(360) - list_pad * 2) / (row_h + row_gap)));
      const int shown = std::min(count, fit);
      const float list_h = shown * row_h + (shown - 1) * row_gap + list_pad * 2;
      // The executable never takes more than half the row: a long name keeps room to read its start.
      const float exe_room = std::max(px(60), ImGui::GetContentRegionAvail().x * 0.5f);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, row_gap));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), list_pad));
      ImGui::BeginChild("##windows", ImVec2(0, list_h));
      for (const Json& window : panel.windows.items()) {
        const std::string id = window["id"].as_string();
        ImGui::PushID(id.c_str());
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = row_h;
        const bool chosen = config["capture_mode"].as_string() == "window" && config["window_id"].as_string() == id;
        if (ImGui::InvisibleButton("window", ImVec2(w, h))) {
          // The picker closes now, not when the engine answers: the list is one long wait otherwise, and the page says
          // what was chosen the moment the row is pressed.
          panel.picking_window = false;
          // One window, nothing else: game detection off so it does not switch away.
          Json off = Json::object();
          off.set("auto_game_capture", Json(false));
          env.store.set_config(off);
          Json args = Json::object();
          args.set("mode", Json("window"));
          args.set("window_id", Json(id));
          args.set("window_name", Json(window["name"].as_string()));
          env.store.attempt("capture.set", args, "Window not selected", [&store = env.store, &panel](const Json&) {
            panel.picking_window = false;
            store.refresh_config();
          });
        }
        const bool hovered = ImGui::IsItemHovered();
        // A minimised window is listed like any other, but it hands over no picture until it is restored, so
        // the row says so instead of recording black frames. Its name is dimmed for the same reason.
        const bool minimized = window["minimized"].as_bool();
        // The row says the whole title and its program on hover: the row itself has one line to give.
        tooltip(window["name"].as_string() + "  -  " + window["exe"].as_string() +
                (minimized ? "\nMinimized: restore it so the picture can be recorded." : std::string()));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, ImVec2(min.x + w, min.y + h), chosen ? pal().raised : pal().canvas);
        draw->AddRect(min, ImVec2(min.x + w, min.y + h), chosen ? pal().accent : (hovered ? pal().muted : pal().line), 0, 0, chosen ? px(2) : 1.0f);
        draw_icon(draw, "window", ImVec2(min.x + px(10), min.y + (h - px(16)) / 2), px(16), pal().faint);
        const std::string exe = window["exe"].as_string();
        const float exe_w = std::min(text_width(fonts().small, exe), exe_room);
        const float name_max = std::max(px(40), w - px(36) - exe_w - px(20));
        draw->PushClipRect(min, ImVec2(min.x + px(36) + name_max, min.y + h), true);
        draw->AddText(fonts().body, fonts().body->LegacySize, ImVec2(min.x + px(36), min.y + (h - fonts().body->LegacySize) / 2),
                      minimized ? pal().muted : pal().ink, window["name"].as_string().c_str());
        draw->PopClipRect();
        draw->PushClipRect(ImVec2(min.x + w - exe_w - px(10), min.y), ImVec2(min.x + w - px(10), min.y + h), true);
        draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(min.x + w - px(10) - text_width(fonts().small, exe), min.y + (h - fonts().small->LegacySize) / 2),
                      pal().faint, exe.c_str());
        draw->PopClipRect();
        ImGui::PopID();
      }
      ImGui::EndChild();
      ImGui::PopStyleVar(2);
    }
  }
  // How the display is read. It sits under where the window is picked because both answer the same question - what is
  // recorded - and it used to stand above that pick, so the page asked how before it asked what.
  if (source != 2) {
    const std::string method = config["display_method"].as_string("auto");
    const int current = method == "dxgi" ? 1 : method == "wgc" ? 2 : 0;
    // The list names each API beside plain words, because "desktop duplication" alone named nothing a person could choose
    // between. What the one chosen costs is a hover hint, not a line under the label: the line pushed every row below it down
    // for words the dropdown already tags.
    static const char* const what[3] = {
        "Fast copy, falling back to Windows' own capture.",
        "Fastest. Reads the picture from the graphics card.",
        "Asks Windows. Works where the fast copy fails."};
    begin_row("Screen capture method", what[current]);
    {
      int selected = current;
      if (select("##capturemethod", {"Automatic", "Desktop duplication (DXGI)", "Windows capture (WGC)"}, selected, 280, false,
                 {"recommended", "fastest", "most compatible"})) {
        set(env, "display_method", Json(selected == 1 ? "dxgi" : selected == 2 ? "wgc" : "auto"));
      }
    }
    end_row();
  }
  begin_row("Stretched resolutions",
            "How a picture whose shape differs from the canvas is fitted. Stretch to fill records a 4:3 stretched-resolution game the way the monitor shows it, without black bars.");
  {
    const std::string modes[2] = {"fit", "stretch"};
    int selected = config["stretch_mode"].as_string("auto") == "stretch" ? 1 : 0;
    if (select("##stretch", {"Keep their shape", "Stretch to fill"}, selected, 240)) {
      set(env, "stretch_mode", Json(modes[selected]));
    }
  }
  end_row();
  switch_row(env, "capture_cursor", "Capture the cursor");
  if (env.store.status["game"]["capturing_game"].as_bool()) {
    note("Capturing " + env.store.status["capture"]["target"].as_string() + " now. It goes back to the display when the game closes.");
  }
  end_section();

  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##buffer", "Buffer");
  begin_row("Buffer length", "How far back a saved clip reaches. The buffer always keeps all of it; its memory is shown under Video.");
  {
    int seconds = static_cast<int>(config["buffer_seconds"].as_int(60));
    if (int_select("##bufferlength", seconds, {15, 30, 60, 90, 120, 180, 300, 600, 900, 1800},
                   [](int v) { return format_span(v); })) {
      set(env, "buffer_seconds", Json(seconds));
    }
  }
  end_row();
  begin_row("Quick clip", "A shorter save from the same buffer.");
  {
    int seconds = static_cast<int>(config["quick_clip_seconds"].as_int(20));
    // Never longer than the buffer: a quick clip is a shorter save from the same ring.
    const int buffer = static_cast<int>(config["buffer_seconds"].as_int(60));
    const std::vector<int> options = quick_clip_choices();
    std::vector<int> choices;
    for (int option : options) {
      if (option <= buffer) choices.push_back(option);
    }
    if (int_select("##quickclip", seconds, choices, [](int v) { return std::to_string(v) + " seconds"; })) {
      set(env, "quick_clip_seconds", Json(seconds));
    }
  }
  end_row();
  switch_row(env, "pause_on_lock", "Pause when the PC is locked");
  switch_row(env, "pause_on_battery", "Pause on battery");
  end_section();

  timeline_settings(env);

  ImGui::Dummy(ImVec2(0, px(6)));
  // What counts as a game. Rewind's denylist is carried over and already decides; what was missing was
  // that a person could not see it. Ours are shown read-only because deleting one only brings it back
  // at the next launch - the engine's list is the authority, not this page.
  begin_section("##games", "Games");
  {
    env.store.ensure_exclusions(config["game_exclusions"]);
    const Json& all = env.store.exclusions;
    const std::vector<Json>& entries = all.items();
    int builtin = 0, mine = 0;
    for (const Json& entry : entries) (entry["builtin"].as_bool() ? builtin : mine) += 1;
    char summary[128];
    std::snprintf(summary, sizeof(summary), "%d already excluded \xC2\xB7 %d of yours", builtin, mine);
    begin_row("Never treat as a game", summary, pal().muted);
    end_row();
    if (!entries.empty()) {
      // The built-in list is over a hundred names, so the box scrolls and carries a scrollbar: the
      // panel does not scroll with the mouse wheel by design, and a list with no way to reach the
      // bottom of it is a list that does not exist for the person reading it.
      // Whole rows only, the same rule the window list follows: the box used to be told 180 px while its rows took a
      // hundred and thirty of them, so it always cut the last name in half. A row is one line of text, and the remove
      // button on a name of ours is the same height, so every row has the same pitch and the last one lands on the edge.
      const float row_gap = px(7), list_pad = px(6);
      const float row_h = ImGui::GetTextLineHeight();
      const int rows = std::max(3, static_cast<int>((px(220) - list_pad * 2 + row_gap) / (row_h + row_gap)));
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, row_gap));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), list_pad));
      ImGui::BeginChild("##exclusions", ImVec2(0, rows * (row_h + row_gap) - row_gap + list_pad * 2), ImGuiChildFlags_Borders);
      for (const Json& entry : entries) {
        const std::string name = entry["name"].as_string();
        const bool builtin = entry["builtin"].as_bool();
        ImGui::PushID(name.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, builtin ? pal().faint : pal().ink);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopStyleColor();
        // Ours cannot be removed; theirs can, one click. The button is one line tall, so a row of ours is the same
        // height as a row of the built-in list and the box can end between two rows.
        if (!builtin) {
          ImGui::SameLine();
          if (icon_button("##remove", "trash", "Never treat " + name + " as a game", false, row_h, row_h)) {
            Json list = Json::array();
            for (const Json& kept : config["game_exclusions"].items()) {
              if (kept.as_string() != name) list.push(Json(kept.as_string()));
            }
            set(env, "game_exclusions", list);
          }
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
      ImGui::PopStyleVar(2);
    }
    // The one actually in front of you, which is what a person usually wants to exclude.
    const std::string detected = env.store.status["game"]["detected_exe"].as_string();
    if (!detected.empty()) {
      Json list = Json::array();
      bool already = false;
      for (const Json& item : config["game_exclusions"].items()) {
        already = already || item.as_string() == detected;
        list.push(Json(item.as_string()));
      }
      if (!already) {
        if (button(("Never treat " + detected + " as a game##exclude_detected").c_str(), Variant::normal, nullptr, false,
                   0, 28)) {
          list.push(Json(detected));
          set(env, "game_exclusions", list);
        }
      }
    }
    // And a name typed by hand, for the app that is not in front of you now.
    begin_row("Add by name", "An executable, with or without .exe");
    if (text_field("##exclusion_add", panel.exclusions_text, 200, "myapp.exe")) {
      std::string name = panel.exclusions_text;
      name.erase(0, name.find_first_not_of(" \t"));
      name.erase(name.find_last_not_of(" \t") + 1);
      if (!name.empty()) {
        Json list = Json::array();
        bool already = false;
        for (const Json& item : config["game_exclusions"].items()) {
          already = already || item.as_string() == name;
          list.push(Json(item.as_string()));
        }
        if (!already) {
          list.push(Json(name));
          set(env, "game_exclusions", list);
        }
        panel.exclusions_text.clear();
      }
    }
    end_row();
  }
  end_section();
  // "Capture games automatically" lived here as well, a second switch for the Games + display choice
  // at the top of the page. One control for one setting.
}

// ---------------------------------------------------------------- audio

void device_row(ViewEnv& env, const char* key, const char* list_key, const std::string& title, const std::string& hint,
                bool disabled) {
  begin_row(title, hint);
  const std::string current = env.store.config[key].as_string();
  std::vector<std::string> ids{"default"}, labels{"Windows default"};
  for (const Json& device : env.panel.devices[list_key].items()) {
    const std::string id = device["id"].as_string();
    if (id == "default" || id.empty()) continue;
    ids.push_back(id);
    labels.push_back(device["name"].as_string(id));
  }
  if (!current.empty() && std::find(ids.begin(), ids.end(), current) == ids.end()) {
    ids.push_back(current);
    labels.push_back("Selected device (not connected)");
  }
  int selected = 0;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == (current.empty() ? "default" : current)) selected = static_cast<int>(i);
  }
  if (select((std::string("##") + key).c_str(), labels, selected, 260, disabled)) set(env, key, Json(ids[static_cast<size_t>(selected)]));
  end_row();
}

void volume_row(ViewEnv& env, const char* key, const char* mute_key, const char* icon_on, const char* icon_off,
                const std::string& noun) {
  begin_row("Volume");
  int volume = static_cast<int>(std::lround(env.store.config[key].as_double(1.0) * 100));
  if (slider_commit(key, volume, 0, 100, "%d%%", 182)) set(env, key, Json(volume / 100.0));
  ImGui::SameLine(0, px(10));
  const bool muted = env.store.config[mute_key].as_bool();
  if (icon_button(mute_key, muted ? icon_off : icon_on, (muted ? "Unmute " : "Mute ") + noun, !muted, 28, 15)) {
    set(env, mute_key, Json(!muted));
  }
  end_row();
}

void audio_section(ViewEnv& env) {
  PanelState& panel = env.panel;
  const Json& config = env.store.config;
  if (panel.devices.is_null() && !panel.devices_loading)
    load(env, "devices.list", panel.devices, panel.devices_loading, Json::object(), "The device list did not load");

  const bool desktop = config["desktop_enabled"].as_bool(), desktop_muted = config["desktop_muted"].as_bool();
  begin_section("##desktop", "Desktop audio", [&] {
    // Level on the switch's own line, a hand's width left of the switch, centred on it.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(62));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(18));
    level_bar(90, desktop && !desktop_muted ? env.store.desktop_level : 0.0f);
  });
  switch_row(env, "desktop_enabled", "Record desktop audio", "Game sound, voice chat, music: everything you hear.");
  device_row(env, "desktop_device", "outputs", "Output device", "Pick the speakers or headset you actually listen on.", !desktop);
  volume_row(env, "desktop_volume", "desktop_muted", "speaker", "speakerOff", "desktop audio");
  end_section();

  ImGui::Dummy(ImVec2(0, px(6)));
  const bool mic = config["mic_enabled"].as_bool(), mic_muted = config["mic_muted"].as_bool();
  begin_section("##mic", "Microphone", [&] {
    // Level on the switch's own line, a hand's width left of the switch, centred on it.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(62));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(18));
    level_bar(90, mic && !mic_muted ? env.store.mic_level : 0.0f);
  });
  switch_row(env, "mic_enabled", "Record the microphone");
  device_row(env, "mic_device", "inputs", "Input device", "", !mic);
  volume_row(env, "mic_volume", "mic_muted", "mic", "micOff", "the microphone");
  begin_row("Boost", "Raise a quiet microphone.");
  {
    int gain = static_cast<int>(std::lround(config["mic_gain_db"].as_double()));
    if (slider_commit("##gain", gain, -12, 20, gain > 0 ? "+%d dB" : "%d dB", 182)) set(env, "mic_gain_db", Json(gain));
    // The bare slider keeps the mute column's width, so Boost ends where the volume bars end.
    ImGui::SameLine(0, px(10));
    ImGui::Dummy(ImVec2(px(28), 0));
  }
  end_row();
  switch_row(env, "mic_noise_suppress", "Noise suppression", "Removes fans, keyboards and hum (RNNoise).", !mic);
  switch_row(env, "mic_noise_gate", "Noise gate", "Silences the mic between words.", !mic);
  end_section();

  ImGui::Dummy(ImVec2(0, px(6)));
  if (advanced("##audioadvanced", "Advanced", panel.advanced.audio)) {
    begin_section("##audioadv", "");
    begin_row("Audio tracks", "Separate tracks let an editor turn the mic or the game up or down afterwards. Track 1 is always the mix.");
    {
      int selected = config["audio_tracks"].as_int(1) > 1 ? 1 : 0;
      if (select("##tracks", {"One mixed track", "Mix + desktop + mic"}, selected, 220)) {
        Json patch = Json::object();
        patch.set("audio_tracks", Json(selected ? 3 : 1));
        patch.set("separate_tracks", Json(selected == 1));
        env.store.set_config(patch);
      }
    }
    end_row();
    begin_row("Audio quality");
    {
      int bitrate = static_cast<int>(config["audio_bitrate_kbps"].as_int(160));
      if (int_select("##audiobitrate", bitrate, {96, 128, 160, 192, 256, 320}, [](int v) { return std::to_string(v) + " kbps"; })) {
        set(env, "audio_bitrate_kbps", Json(bitrate));
      }
    }
    end_row();
    note("Changing tracks or audio quality restarts the buffer.");
    end_section();
  }
}

// ---------------------------------------------------------------- encoder

// Settings, Storage: the Timeline's own store (timeline.get) - what it holds on the disk now, and the one destructive
// action the interface offers. Read when the page opens and after an erase; nothing polls it.
void timeline_storage(ViewEnv& env) {
  static Json store_data;
  static bool loading = false;
  static std::string loaded_for;
  static std::string erase_asked;  // the session whose Erase was pressed once: a second press within 4 s erases
  static uint32_t erase_asked_until = 0;

  auto read = [&store = env.store] {
    if (loading) return;
    loading = true;
    store.bridge_call("timeline.get", Json::object(), [](bool ok, const Json& reply, const std::string&) {
      loading = false;
      store_data = ok ? reply : Json::object();
    });
  };
  if (loaded_for != env.panel.settings + "#" + std::to_string(env.panel.settings_open_epoch)) {  // entering the page reads it again
    loaded_for = env.panel.settings + "#" + std::to_string(env.panel.settings_open_epoch);
    store_data = Json();
    loading = false;
    erase_asked.clear();
  }
  if (store_data.is_null()) read();

  begin_row("On disk", "The Timeline's own store, listed under timeline in the panel. It has its own budget, separate from "
                       "clips and recordings, and a clip you save from it is a copy.");
  if (button("Refresh##timelinestore", Variant::ghost, "refresh", loading, 0, 26)) {
    store_data = Json();
    read();
  }
  end_row();
  if (store_data.is_null()) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    spinner(ImGui::GetWindowDrawList(), ImVec2(pos.x + px(12), pos.y + px(16)), px(7), pal().accent);
    ImGui::Dummy(ImVec2(0, px(32)));
    return;
  }
  const std::vector<Json>& sessions = store_data["sessions"].items();
  if (sessions.empty()) {
    text(fonts().small, pal().muted, "Nothing on the disk yet. Turn Timeline on under Capture and it starts recording.");
    return;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  for (const Json& session : sessions) {
    const std::string id = session["id"].as_string();
    ImGui::PushID(id.c_str());
    begin_row(format_when(session["started_at"].as_int(), now),
              format_bytes(session["bytes"].as_double()) + " \xC2\xB7 " +
                  std::to_string(session["published_chunks"].as_int()) + " chunks",
              pal().muted);
    const bool armed = erase_asked == id && static_cast<int32_t>(env.now_ms - erase_asked_until) < 0;
    if (button(armed ? "Erase?" : "Erase", armed ? Variant::danger : Variant::normal, "trash", false, 0, 26)) {
      if (!armed) {
        erase_asked = id;
        erase_asked_until = env.now_ms + 4000;
      } else {
        erase_asked.clear();
        Json args = Json::object();
        args.set("session", Json(id));
        // The engine stops that session's writer and deletes outside the lock; a file it still cannot let go of comes
        // back in `error`, named. Nothing is claimed removed here.
        env.store.attempt("timeline.erase", args, "The footage was not erased", [](const Json&) { store_data = Json(); });
      }
    }
    tooltip(armed ? "Press again to erase this session from the disk. It cannot be undone."
                  : "Erase this session from the disk.");
    end_row();
    ImGui::PopID();
  }
}

// The encoder's own property list and the values the current settings will build.
void ensure_encoder_properties(ViewEnv& env) {
  PanelState& panel = env.panel;
  Json key = Json::object();
  for (const char* name : {"codec", "encoder", "quality", "peak_mode", "custom_kbps", "encoder_mode", "encoder_options",
                           "encoder_speed_step", "performance_when"})
    key.set(name, env.store.config[name]);
  key.set("performance_active", env.store.status["performance"]["active"]);
  const std::string requested = key.dump();
  if (panel.encoder_key != requested) {
    panel.encoder_key = requested;
    panel.encoder = Json();
    panel.encoder_failed = false;
  }
  if (!panel.encoder.is_null() || panel.encoder_loading || panel.encoder_failed) return;
  panel.encoder_loading = true;
  Json args = Json::object();
  args.set("configured", Json(true));
  env.store.bridge_call("encoder.properties", args, [&panel, requested](bool ok, const Json& data, const std::string&) {
    panel.encoder_loading = false;
    if (panel.encoder_key != requested) return;
    if (ok) panel.encoder = data;
    else panel.encoder_failed = true;
  });
}

// The property `name` of the running encoder, or null when it has none.
const Json* encoder_property(const PanelState& panel, const char* name) {
  for (const Json& property : panel.encoder["properties"].items()) {
    if (property["name"].as_string() == name) return &property;
  }
  return nullptr;
}

// The speed rows the page shows. Their live values are what Custom keeps: editing any one of them copies all of
// them (as the encoder is running them now) into `encoder_options`, makes the mode Custom, and changes the one.
const char* const kCuratedKeys[] = {"preset", "multipass", "lookahead", "adaptive_quantization", "target_usage"};

void curated_apply(ViewEnv& env, const std::string& name, Json value) {
  Json options = Json::object();
  for (const char* key : kCuratedKeys) {
    if (const Json* property = encoder_property(env.panel, key)) options.set(key, (*property)["value"]);
  }
  // Anything else already in the options (a hand-edited file) is kept as it was.
  const Json& stored = env.store.config["encoder_options"];
  if (stored.is_object()) {
    for (const auto& [key, existing] : stored.fields()) {
      if (!options.has(key)) options.set(key, existing);
    }
  }
  options.set(name, std::move(value));
  Json patch = Json::object();
  patch.set("encoder_options", options);
  patch.set("encoder_mode", Json("custom"));
  patch.set("video_preset", Json("custom"));  // editing an effort row is Custom, not a preset
  env.store.set_config(patch);
  env.panel.encoder = Json();  // read the running encoder again
}

// A list property as a select. `names` gives a plainer word for a value; `keep` leaves values out of the list
// (the low-latency tunings are for streaming and have no place here).
void curated_list_row(ViewEnv& env, const Json& property, const std::function<std::string(const std::string&)>& names,
                      const std::function<bool(const std::string&)>& keep = nullptr) {
  std::vector<std::string> labels;
  std::vector<Json> values;
  int selected = 0;
  const Json& current = property["value"];
  const std::string current_text = current.is_string() ? current.as_string() : current.dump();
  for (const Json& option : property["options"].items()) {
    const std::string value = option.is_string() ? option.as_string() : option.dump();
    if (keep && !keep(value) && value != current_text) continue;
    const std::string plain = names ? names(value) : std::string();
    if (value == current_text) selected = static_cast<int>(labels.size());
    labels.push_back(plain.empty() ? value : plain);
    values.push_back(option);
  }
  ImGui::PushID(property["name"].as_string().c_str());
  if (select("##value", labels, selected, 240)) curated_apply(env, property["name"].as_string(), values[static_cast<size_t>(selected)]);
  ImGui::PopID();
}

// NVENC's presets in words: p1 is the fastest, p7 the slowest and best.
std::string preset_words(const std::string& value) {
  static const char* const words[] = {"Fastest", "Faster", "Fast", "Medium", "Slow", "Slower", "Slowest"};
  if (value.size() == 2 && value[0] == 'p' && value[1] >= '1' && value[1] <= '7') {
    return std::string(words[value[1] - '1']) + " (" + value + ")";
  }
  return std::string();
}

void encoder_section(ViewEnv& env) {
  PanelState& panel = env.panel;
  const Json& config = env.store.config;
  const Json& status = env.store.status;
  const std::string codec = config["codec"].as_string("h264");
  const std::string quality = config["quality"].as_string("high");
  const int fps = static_cast<int>(config["capture_fps"].as_int(60));

  // The size the encoder is really fed: the configured output, else the capture's own size. Until the capture
  // has reported one (a game that has not hooked) it is the 1080p the ladder answers with. It used to be 1080p
  // always, so a 1440p screen was estimated at 1.8x too small a rate.
  int width = 0, height = 0;
  const int capture_width = static_cast<int>(status["capture"]["width"].as_int(0));
  const int capture_height = static_cast<int>(status["capture"]["height"].as_int(0));
  encoder_size(config["output_width"].as_int(0), config["output_height"].as_int(0), capture_width > 0 ? capture_width : 1920,
               capture_height > 0 ? capture_height : 1080, width, height);

  // Locked while Performance mode is on: the rows show the user's own values, which are not what is running.
  const bool locked_by_performance = status["performance"]["active"].as_bool(false);
  const std::string locked_hint = "Performance mode is on, so this is locked. Turn it off to change it.";
  // The Advanced rows belong to Custom. While a preset is chosen they are disabled, and each hint says why.
  bool advanced_rows = false;
  bool advanced_locked = false;
  // A disabled row still shows its hint (the row itself is hovered), so the reason is appended there instead of in a
  // second card over the same rows.
  const auto lock_hint = [&](std::string hint) {
    if (advanced_rows && advanced_locked) hint += " Pick Custom in Preset to change this.";
    if (locked_by_performance) hint += " " + locked_hint;
    return hint;
  };
  // The collapsed Advanced header is the one locked control with no row of its own to hover.
  auto lock_tooltip = [&](ImVec2 start) {
    if (!locked_by_performance) return;
    const ImVec2 end = ImGui::GetCursorScreenPos();
    tooltip_rect(ImVec2(start.x, start.y - px(4)), ImVec2(start.x + ImGui::GetContentRegionAvail().x, end.y), locked_hint, 240);
  };

  // Order of the page (decided with the user, 2026-11-04): the quality first, then the picture it applies to.
  note("Changing the picture, the codec or the quality restarts the encoder and empties the buffer.");
  ImGui::Dummy(ImVec2(0, px(6)));
  // ---- Quality: one choice for the whole encoder. The four levels set the quality, an automatic bitrate and Auto
  // effort together (encoder_settings.h video_preset_patch); Custom is the only place the rate is the user's own.
  if (locked_by_performance) ImGui::BeginDisabled();
  const bool fixed_peak = config["peak_mode"].as_string("auto") == "fixed" && config["custom_kbps"].as_int(0) > 0;
  std::string preset_shown = config["video_preset"].as_string("high");
  if (fixed_peak || quality == "custom") {
    preset_shown = "custom";
  } else if (preset_shown != "low" && preset_shown != "standard" && preset_shown != "high" && preset_shown != "highest" && preset_shown != "custom") {
    preset_shown = quality == "low" || quality == "standard" || quality == "highest" ? quality : "high";
  }
  begin_section("##preset", "Quality");
  {
    const char* presets[5] = {"low", "standard", "high", "highest", "custom"};
    const char* levels[4] = {"low", "standard", "high", "highest"};
    int chosen = 4;
    for (int i = 0; i < 5; ++i) {
      if (preset_shown == presets[i]) chosen = i;
    }
    int level = 2;
    for (int i = 0; i < 4; ++i) {
      if (quality == levels[i]) level = i;
    }
    begin_row("Preset", lock_hint(chosen == 4 ? "Your own bitrate and effort. Pick a preset to go back."
                                     : "Quality, bitrate and encoder effort in one choice."));
    if (segmented("##videopreset", {"Low", "Standard", "High", "Highest", "Custom"}, chosen, 0.0f, 28.0f, nullptr, 12.0f,
                  {"Smallest clips and the least memory.",
                   "Clean in most games, a little soft in fast motion.",
                   "The default: sharp in fast motion, at a sensible size.",
                   "For editing later: the finest detail and the largest clips.",
                   "Your own bitrate and effort."})) {
      Json patch = Json::object();
      if (chosen == 4) {
        // Custom starts from the rate the current level was using, so choosing it changes nothing until it is moved.
        patch.set("video_preset", Json("custom"));
        patch.set("peak_mode", Json("fixed"));
        patch.set("custom_kbps", Json(preset_kbps(codec, width, height, fps, levels[level])));
      } else {
        patch = video_preset_patch(presets[chosen]);
        patch.set("video_preset", Json(presets[chosen]));
      }
      env.store.set_config(patch);
      panel.encoder = Json();
    }
    end_row();
    if (chosen == 4) {
      const int auto_kbps = preset_kbps(codec, width, height, fps, levels[level]);
      const int current_kbps = fixed_peak ? static_cast<int>(config["custom_kbps"].as_int(0)) : auto_kbps;
      begin_row("Bitrate", lock_hint("The most the encoder may spend on a busy scene. The picture runs at High."));
      int mbps = std::max(1, static_cast<int>(std::lround(current_kbps / 1000.0)));
      if (slider_commit("##peakrate", mbps, 2, 200, "%d Mbps", 140)) {
        Json patch = Json::object();
        patch.set("peak_mode", Json("fixed"));
        patch.set("custom_kbps", Json(mbps * 1000));
        patch.set("video_preset", Json("custom"));
        env.store.set_config(patch);
        panel.encoder = Json();
      }
      end_row();
    }
    // Plain words only: what happened, what it means, and one way back. No step counts.
    const int speed_step = static_cast<int>(config["encoder_speed_step"].as_int(0));
    if (speed_step > 0) {
      // What is running, not what is stored: the capture rate the relief layer settled on, so a lowered frame rate is visible
      // instead of the page still promising 120.
      const int running_fps = static_cast<int>(status["capture"]["fps"].as_int(fps));
      note("BuffrX lowered quality to keep every frame. Your clips keep their resolution and length. Running at " +
           std::to_string(running_fps) + " fps now.");
      if (button("Try full quality again##resetstep", Variant::normal, nullptr, false, 0, 32)) {
        Json patch = Json::object();
        patch.set("encoder_speed_step", Json(0));
        env.store.set_config(patch);
        panel.encoder = Json();
      }
    }
    // The encoder falling behind is the one thing here that makes clips stutter while the game is smooth.
    if (status["stats"]["encoder_behind"].as_bool(false)) {
      const int percent = static_cast<int>(std::lround(status["stats"]["encoder_ratio"].as_double(1.0) * 100.0));
      const std::string behind = "Only " + std::to_string(percent) + " of every 100 frames are reaching your clips, so they play too fast. ";
      const int lower_fps = fps > 60 ? 60 : 30;
      auto use_lower_rate = [&] {
        if (button(("Use " + std::to_string(lower_fps) + " fps##lowerrate").c_str(), Variant::primary, nullptr, false, 0, 32)) {
          set(env, "capture_fps", Json(lower_fps));
          panel.encoder = Json();
        }
      };
      if (speed_step < 5) {
        note(behind + "BuffrX is lowering quality by itself to fix it.", true);
      } else if (locked_by_performance) {
        note(behind + "Performance mode already uses the lightest settings, so the frame rate is what is left.", true);
        if (fps > 30) use_lower_rate();
      } else {
        note(behind + "Quality is already as low as it goes, so the frame rate is what is left.", true);
        if (fps > 30) use_lower_rate();
      }
    }
  }
  end_section();
  if (locked_by_performance) ImGui::EndDisabled();

  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##picture", "Picture");
  if (locked_by_performance) ImGui::BeginDisabled();
  begin_row("Resolution", lock_hint("Native keeps every pixel; lower sizes make smaller clips. Performance mode runs 1080p."));
  {
    // Performance mode owns the picture: the row shows what it runs, not the stored choice it is not using.
    const int out_height =
        locked_by_performance ? 1080 : config["output_width"].as_int() > 0 ? static_cast<int>(config["output_height"].as_int()) : 0;
    const int heights[4] = {0, 1440, 1080, 720};
    int selected = 0;
    for (int i = 0; i < 4; ++i) {
      if (heights[i] == out_height) selected = i;
    }
    if (select("##resolution", {"Native", "1440p", "1080p", "720p"}, selected, 160)) {
      Json patch = Json::object();
      int w = 0, h = 0;
      if (heights[selected] > 0) resolution_for_height(heights[selected], w, h);
      patch.set("output_width", Json(w));
      patch.set("output_height", Json(h));
      env.store.set_config(patch);
      panel.encoder = Json();
    }
  }
  end_row();
  begin_row("Frame rate", lock_hint("Higher is smoother and costs more of the encoder. Performance mode runs 60 fps."));
  {
    // 30, 60 and 120: the rates a clip is made at. A rate set some other way (an older config, a profile) shows as a fourth choice.
    // Performance mode runs 60 fps; the chips say so while they are locked.
    const int shown_fps = locked_by_performance ? 60 : fps;
    std::vector<int> rates = {30, 60, 120};
    if (std::find(rates.begin(), rates.end(), shown_fps) == rates.end()) rates.push_back(shown_fps);
    std::vector<std::string> names;
    int selected = 0;
    for (size_t i = 0; i < rates.size(); ++i) {
      names.push_back(std::to_string(rates[i]));
      if (shown_fps == rates[i]) selected = static_cast<int>(i);
    }
    if (segmented("##fps", names, selected)) {
      set(env, "capture_fps", Json(rates[static_cast<size_t>(selected)]));
      panel.encoder = Json();
    }
  }
  end_row();
  if (locked_by_performance) ImGui::EndDisabled();
  // The codec and encoder are what Performance mode sets; the picture above is greyed too, because the plan owns it.
  if (locked_by_performance) ImGui::BeginDisabled();
  begin_row("Codec", lock_hint("A preset's rate follows the codec. A custom rate is carried across at the same picture."));
  {
    const char* codecs[3] = {"h264", "hevc", "av1"};
    int selected = 0;
    for (int i = 0; i < 3; ++i) {
      if (codec == codecs[i]) selected = i;
    }
    if (select("##codec", {"H.264", "HEVC", "AV1"}, selected, 200, false,
               {"Most compatible", "Smaller files", "Smallest, RTX 40+"})) {
      set(env, "codec", Json(codecs[selected]));
      panel.encoder = Json();
    }
  }
  end_row();
  begin_row("Encoder", lock_hint("Automatic picks your GPU's encoder and falls back to the CPU if it cannot start."));
  {
    const char* encoders[5] = {"auto", "nvenc", "amf", "qsv", "software"};  // the engine's vendor names
    int selected = 0;
    for (int i = 0; i < 5; ++i) {
      if (config["encoder"].as_string("auto") == encoders[i]) selected = i;
    }
    if (select("##encoderchoice", {"Automatic", "NVIDIA NVENC", "AMD AMF", "Intel Quick Sync", "CPU (x264)"}, selected, 200)) {
      set(env, "encoder", Json(encoders[selected]));
      panel.encoder = Json();
    }
    // Read through a const view: the non-const Json::operator[] turns a null into an empty object, which made
    // ensure_encoder_properties think the reply had arrived and never ask again (the Advanced rows vanished).
    const std::string shown_encoder = std::as_const(panel.encoder)["id"].as_string(status["encoder"].as_string());
    text(fonts().small, pal().faint, std::string(status["asleep"].as_bool() ? "Next encoder: " : "Running now: ") + encoder_name(shown_encoder));
  }
  end_row();
  if (locked_by_performance) ImGui::EndDisabled();
  end_section();

  // ---- Advanced: the rows that change how the video looks or how hard the GPU works, and nothing else. A mode sets
  // them; editing one makes the mode Custom. (Profile, B-frames, split encode and the rest are fixed to sensible
  // values, not rows: they cannot make a clip better and several could only break it.)
  ImGui::Dummy(ImVec2(0, px(6)));
  // The rows below the presets belong to Custom: a preset owns them, and editing one is what Custom is for.
  advanced_rows = true;
  const ImVec2 locked_advanced_at = ImGui::GetCursorScreenPos();
  advanced_locked = preset_shown != "custom";
  if (locked_by_performance || advanced_locked) ImGui::BeginDisabled();
  begin_section("##encoderadv", "Advanced");
  ensure_encoder_properties(env);
  const std::string mode = config["encoder_mode"].as_string("auto");
  {
      const char* modes[5] = {"auto", "fast", "balanced", "quality", "custom"};
      int selected = 0;
      for (int i = 0; i < 5; ++i) {
        if (mode == modes[i]) selected = i;
      }
      begin_row("Encoder effort", lock_hint("How hard the encoder works. The pointer picks a row's own line."));
      if (segmented("##encodermode", {"Auto", "Fast", "Balanced", "Maximum", "Custom"}, selected, 0.0f, 28.0f, nullptr, 12.0f,
                    {"Follows your quality level, and steps down by itself if your GPU cannot keep up.",
                     "Lightest on your GPU: a little softer, the most room for the game.",
                     "The usual balance of quality and GPU use.",
                     "The most effort, for GPUs with room to spare. A busy game can make it drop frames.",
                     "Your own settings below. Pick another effort to go back."})) {
        if (selected == 4) {
          // Custom starts from what is running now, so choosing it changes nothing until a row is edited.
          const Json* preset = encoder_property(panel, "preset");
          const Json* speed = preset ? preset : encoder_property(panel, "target_usage");
          if (speed) curated_apply(env, (*speed)["name"].as_string(), (*speed)["value"]);
        } else {
          Json patch = Json::object();
          patch.set("encoder_mode", Json(modes[selected]));
          patch.set("encoder_options", Json::object());
          env.store.set_config(patch);
          panel.encoder = Json();
        }
      }
      end_row();
    }
    if (panel.encoder_failed) {
      note("The encoder's own settings could not be read.", true);
    } else if (panel.encoder.is_null()) {
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      spinner(ImGui::GetWindowDrawList(), ImVec2(pos.x + px(12), pos.y + px(12)), px(7), pal().accent);
      ImGui::Dummy(ImVec2(0, px(24)));
    } else {
      const std::string family = panel.encoder["family"].as_string();
      const Json* preset = encoder_property(panel, "preset");
      const Json* usage = encoder_property(panel, "target_usage");
      if (const Json* speed = preset ? preset : usage; speed && (*speed)["options"].is_array()) {
        begin_row("Speed", lock_hint("How hard the encoder works on each frame. Slower is a little sharper and uses more of the GPU."));
        curated_list_row(env, *speed, [&](const std::string& value) {
          if (family == "nvenc") return preset_words(value);
          if (value == "speed") return std::string("Fast");
          if (value == "balanced") return std::string("Balanced");
          if (value == "quality") return std::string("Sharper");
          if (value == "highQuality") return std::string("Sharpest");
          return std::string();
        });
        end_row();
      }
      if (const Json* multipass = encoder_property(panel, "multipass"); multipass && (*multipass)["options"].is_array()) {
        begin_row("Multipass", lock_hint("A second look at each frame: a little sharper for a little more GPU."));
        curated_list_row(env, *multipass, [](const std::string& value) {
          return value == "disabled" ? std::string("Off") : value == "qres" ? std::string("Quarter resolution")
                                     : value == "fullres" ? std::string("Full resolution") : std::string();
        });
        end_row();
      }
      if (const Json* lookahead = encoder_property(panel, "lookahead"); lookahead && (*lookahead)["kind"].as_string() == "bool") {
        begin_row("Look-ahead", lock_hint("Looks ahead to spend bits where the motion is. Costs GPU and adds a little delay."));
        const bool on = (*lookahead)["value"].as_bool();
        ImGui::PushID("lookahead");
        if (toggle("##value", on)) curated_apply(env, "lookahead", Json(!on));
        ImGui::PopID();
        end_row();
      }
      if (const Json* aq = encoder_property(panel, "adaptive_quantization"); aq && (*aq)["kind"].as_string() == "bool") {
        begin_row("Adaptive quantization", lock_hint("Gives the hard parts of the picture more bits. Costs a little GPU."));
        const bool on = (*aq)["value"].as_bool();
        ImGui::PushID("adaptive_quantization");
        if (toggle("##value", on)) curated_apply(env, "adaptive_quantization", Json(!on));
        ImGui::PopID();
        end_row();
      }
      // No Tuning row: NVENC always runs its high-quality tune (obs_host.cpp). Ultra high quality is far heavier and the
      // low-latency tunes cost 5-55% more bits for the same picture, so there is nothing to choose.
    }
    begin_row("File format", lock_hint("MP4 plays in Discord, browsers and phones. MKV survives a crash mid-recording."));
    {
      int selected = config["container"].as_string("mp4") == "mkv" ? 1 : 0;
      if (segmented("##container", {"MP4", "MKV"}, selected)) set(env, "container", Json(selected ? "mkv" : "mp4"));
    }
    end_row();
    if (config["output_width"].as_int(0) > 0) {
      begin_row("Scaling", lock_hint("How the picture is shrunk to a smaller size. Sharper costs a little more GPU."));
      {
        const std::string filter = config["scale_filter"].as_string("bicubic");
        int selected = filter == "lanczos" ? 0 : filter == "bilinear" ? 2 : 1;
        if (select("##scalefilter", {"Sharp (Lanczos)", "Balanced (Bicubic)", "Fast (Bilinear)"}, selected, 240)) {
          set(env, "scale_filter", Json(selected == 0 ? "lanczos" : selected == 2 ? "bilinear" : "bicubic"));
        }
      }
      end_row();
    }
    if (mode == "custom") {
      ImGui::Dummy(ImVec2(0, px(4)));
      if (button("Back to Auto##resetencoder", Variant::ghost, "undo", false, 0, 28)) {
        Json patch = Json::object();
        patch.set("encoder_mode", Json("auto"));
        patch.set("encoder_options", Json::object());
        env.store.set_config(patch);
        panel.encoder = Json();
      }
    }
    end_section();
  if (locked_by_performance || advanced_locked) ImGui::EndDisabled();
  advanced_rows = false;
  lock_tooltip(locked_advanced_at);

  // The page's one highlighted card: its title is boxed in the accent. begin_section has no flag for that (its header
  // line is in use elsewhere), so the card opens the same pane itself: the two lines begin_section draws, plus the box.
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_pane("##performance", "Performance mode", ImVec2(-1, 0), false, std::string(), 0, false, 13.0f, true);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(3)));
  // The reference's blueprint, row for row: a label with its description stacked on the left, the control on
  // the right at the row's top, one hairline between the columns and one rule under the row. The pane's own
  // item spacing is off, so every gap below is exactly the Dummy that draws it.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
  const auto rule = [] {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y),
                                       ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y), pal().line);
  };
  const auto seam = [](float x, float top, float bottom) {
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, top), ImVec2(x, bottom), pal().line);
  };
  // The words of a row: the label, the description under it, and any state line under that.
  const auto words = [](const std::string& label, const std::string& description, float width,
                        const std::vector<std::string>& state) {
    ImGui::BeginGroup();
    text(fonts().strong, pal().ink, label);
    ImGui::Dummy(ImVec2(0, px(4)));
    text_wrapped(fonts().small, pal().muted, description, width);
    for (const std::string& line : state) {
      ImGui::Dummy(ImVec2(0, px(3)));
      text_wrapped(fonts().small, pal().muted, line, width);
    }
    ImGui::EndGroup();
    return ImGui::GetCursorScreenPos().y;
  };
  const std::string perf_when = config["performance_when"].as_string("off");
  const bool active = status["performance"]["active"].as_bool(false);
  const std::string reason = status["performance"]["reason"].as_string("off");
  const bool recording = status["recording"].as_bool(false);
  const bool rebuilding = status["rebuilding"].as_bool(false);
  const bool auto_capture = status["game"]["auto_capture"].as_bool(true);
  const std::string detected_raw = status["game"]["detected_exe"].as_string();
  const std::string detected_name = normalized_exe_name(detected_raw);
  const std::string detected_display = detected_name + ".exe";
  const bool mode_games = perf_when == "games";
  const bool deferred = recording && mode_games && active && detected_name.empty();

  std::vector<std::string> state;
  if (mode_games && !active) state.push_back("Waiting for a listed game.");
  else if (active && reason == "always") state.push_back("On in every game.");
  else if (active && !detected_name.empty()) state.push_back("On now for " + detected_display + ".");
  else if (active) state.push_back("On now.");
  if (deferred) state.push_back("Turns off when the recording ends.");
  if (mode_games && !auto_capture) state.push_back("Automatic mode needs Games + display in Capture.");

  // Row 1 â€” When to use. The strip is the row's control; its seam hugs its own left edge. The pane's own
  // padding leaves less over the row than the gap under it, so one Dummy levels the two.
  ImGui::Dummy(ImVec2(0, px(9)));
  const float strip_pad = px(20), strip_h = px(30);
  float strip_w = 0.0f;
  for (const char* item : {"OFF", "AUTO", "ON"}) strip_w += text_width(fonts().strong, item) + strip_pad * 2;
  const float when_x = ImGui::GetCursorScreenPos().x;
  const float when_top = ImGui::GetCursorScreenPos().y;
  const float when_full = ImGui::GetContentRegionAvail().x;
  const float when_left = when_full - strip_w - px(24) - px(18);
  const float when_left_bottom =
      words("When to use", "Choose when Performance mode turns on.", when_left, state);
  // Centred on the words beside it, so a shorter strip does not hang from the row's top.
  const float strip_y = when_top + std::max(0.0f, (when_left_bottom - when_top - strip_h) * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(when_x + when_full - strip_w, strip_y));
  int choice = perf_when == "games" ? 1 : perf_when == "always" ? 2 : 0;
  const bool mode_locked = !env.store.connected || rebuilding || recording;
  if (mode_locked) ImGui::BeginDisabled();
  if (segmented("##performance_when", {"OFF", "AUTO", "ON"}, choice, 0, 30, nullptr, 20,
                {"Never turns on by itself.", "On while a game in your list runs.", "On for every game."})) {
    set(env, "performance_when", Json(choice == 0 ? "off" : choice == 1 ? "games" : "always"));
  }
  if (mode_locked) ImGui::EndDisabled();
  const float when_right_bottom = ImGui::GetCursorScreenPos().y;
  seam(when_x + when_full - strip_w - px(24), strip_y, when_right_bottom);
  ImGui::SetCursorScreenPos(ImVec2(when_x, std::max(when_left_bottom, when_right_bottom)));
  ImGui::Dummy(ImVec2(0, px(18)));
  rule();
  ImGui::Dummy(ImVec2(0, px(12)));

  const std::vector<Json>& listed = config["performance_games"].items();
  std::vector<std::string> listed_names;
  listed_names.reserve(listed.size());
  for (const Json& item : listed) listed_names.push_back(normalized_exe_name(item.as_string()));

  auto add_game = [&](const std::string& raw) -> bool {
    const std::string normal = normalized_exe_name(raw);
    if (normal.empty()) return false;
    if (std::find(listed_names.begin(), listed_names.end(), normal) != listed_names.end()) return false;
    Json list = Json::array();
    for (const std::string& name : listed_names) list.push(Json(name));
    list.push(Json(normal));
    set(env, "performance_games", list);
    listed_names.push_back(normal);
    return true;
  };

  // The detected game is one press away even when the list already has games.
  const bool detected_already = detected_name.empty() ||
                                std::find(listed_names.begin(), listed_names.end(), detected_name) != listed_names.end();
  auto add_detected_button = [&]() {
    if (detected_already) return;
    if (button(("ADD " + detected_display + "##performance_detected").c_str(), Variant::normal, nullptr, false, 0, 28)) {
      add_game(detected_raw);
    }
  };

  // Explorer's own picker: the game's .exe, kept by its file name.
  auto add_by_path = [&store = env.store](const std::string& raw) {
    const std::string normal = normalized_exe_name(raw);
    if (normal.empty()) return;
    Json list = Json::array();
    bool exists = false;
    for (const Json& item : store.config["performance_games"].items()) {
      const std::string name = normalized_exe_name(item.as_string());
      if (name.empty()) continue;
      if (name == normal) exists = true;
      list.push(Json(name));
    }
    if (exists) return;
    list.push(Json(normal));
    Json patch = Json::object();
    patch.set("performance_games", list);
    store.set_config(patch);
  };
  auto pick_game = [&store = env.store, add_by_path]() {
    Json args = Json::object();
    args.set("title", Json("Choose the game's executable"));
    store.bridge_call("app.pick_file", args,
                      [&store, add_by_path](bool ok, const Json& data, const std::string& error) {
                        if (!ok) {
                          store.toast(UiToast::error, "The file picker did not open", error);
                          return;
                        }
                        const std::string path = data["path"].as_string();
                        if (path.empty()) return;  // cancelled
                        add_by_path(path);
                      });
  };

  // Row 2 â€” the games. The count is the row's label and its description reads under it, as in row 1.
  const float games_full = ImGui::GetContentRegionAvail().x;
  words("Games (" + std::to_string(listed.size()) + ")",
        "Performance mode turns on automatically for these games.", games_full, {});
  ImGui::Dummy(ImVec2(0, px(19)));

  if (listed.empty()) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14), px(14)));
    ImGui::BeginChild("##performance_empty", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    const float inner = ImGui::GetContentRegionAvail().x;
    const char* empty_title = "No games yet";
    const char* empty_hint = "Choose the game's .exe file in Explorer. It turns Performance mode on while that game runs.";
    // Centred like the reference: the title, the one line under it and the button share the middle of the box.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (inner - text_width(fonts().strong, empty_title)) * 0.5f));
    text(fonts().strong, pal().ink, empty_title);
    ImGui::Dummy(ImVec2(0, px(6)));
    const float hint_w = std::min(inner, text_width(fonts().small, empty_hint));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (inner - hint_w) * 0.5f));
    text_wrapped(fonts().small, pal().muted, empty_hint, hint_w);
    ImGui::Dummy(ImVec2(0, px(12)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (inner - px(178)) * 0.5f));
    // A person picks the game's own .exe in Explorer, so the name is never typed by hand here.
    if (button("ADD GAME##performance_empty_add", Variant::primary, nullptr, !env.store.connected, 178, 34)) {
      pick_game();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
  } else {
    const float row_gap = px(8), list_pad = px(10);
    const float row_h = ImGui::GetTextLineHeight();
    const size_t shown_rows = std::min<size_t>(4, listed.size());
    // The child grows to fit a short list, so no scrollbar appears until a fifth game pushes it past four rows.
    const bool scrolls = listed.size() > shown_rows;
    const float height = static_cast<float>(shown_rows) * (row_h + row_gap) - row_gap + list_pad * 2;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, row_gap));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), list_pad));
    ImGui::BeginChild("##performance_list", ImVec2(0, scrolls ? height + px(3) : 0),
                      ImGuiChildFlags_Borders | (scrolls ? 0 : ImGuiChildFlags_AutoResizeY));
    for (size_t i = 0; i < listed.size(); ++i) {
      const std::string& name = listed_names[i];
      const std::string display = name + ".exe";
      const bool running = !detected_name.empty() && detected_name == name;
      ImGui::PushID(name.c_str());
      const float right = row_h + ImGui::GetStyle().ItemSpacing.x * 2 +
                          (running ? text_width(fonts().small, "RUNNING") + ImGui::GetStyle().ItemSpacing.x : 0.0f);
      text_ellipsis(fonts().body, pal().ink, display,
                    std::max(px(48), ImGui::GetContentRegionAvail().x - right));
      if (running) {
        ImGui::SameLine();
        text(fonts().small, pal().accent, "RUNNING");
      }
      ImGui::SameLine();
      if (icon_button("##removeperf", "trash", "Stop using Performance mode for " + display, false, row_h, row_h)) {
        Json list = Json::array();
        for (size_t j = 0; j < listed.size(); ++j) {
          if (j != i) list.push(Json(listed_names[j]));
        }
        set(env, "performance_games", list);
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    if (!detected_already) add_detected_button();
  }

  ImGui::Dummy(ImVec2(0, px(16)));
  rule();
  ImGui::Dummy(ImVec2(0, px(17)));

  // Row 3 â€” Add by name. The field and its button are the row's control; the seam sits before the field. Their
  // widths follow the pane (a third of it for the field), so the words beside them keep one line at any window size.
  const float add_x = ImGui::GetCursorScreenPos().x;
  const float add_top = ImGui::GetCursorScreenPos().y;
  const float add_full = ImGui::GetContentRegionAvail().x;
  const float add_field = std::max(px(160), add_full * 0.34f);
  const float add_button = std::max(px(44), add_full * 0.075f);
  const float add_right = add_field + px(10) + add_button;
  const float add_left = add_full - add_right - px(24) - px(8);
  const float add_left_bottom =
      words("Add by name", "Type the game's name. You do not need to add .exe.", add_left, {});
  ImGui::SetCursorScreenPos(ImVec2(add_x + add_full - add_right, add_top));
  const std::string typed = normalized_exe_name(panel.performance_text);
  const bool duplicate = !typed.empty() && std::find(listed_names.begin(), listed_names.end(), typed) != listed_names.end();
  bool added_this_frame = false;
  // The field takes the button's own height, so the two share a top and a foot on the row.
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, px(8)));
  const bool committed = text_field("##performance_add", panel.performance_text, unpx(add_field), "game.exe");
  ImGui::PopStyleVar();
  if (committed && !added_this_frame && !typed.empty() && !duplicate) {
    if (add_game(panel.performance_text)) {
      panel.performance_text.clear();
      added_this_frame = true;
    }
  }
  ImGui::SameLine(0, px(10));
  const bool can_add = !typed.empty() && !duplicate;
  if (button("ADD##performance_add_button", Variant::normal, nullptr, !can_add, unpx(add_button), 30) &&
      !added_this_frame) {
    if (add_game(panel.performance_text)) {
      panel.performance_text.clear();
      added_this_frame = true;
    }
  }
  const float add_right_bottom = ImGui::GetCursorScreenPos().y;
  seam(add_x + add_full - add_right - px(24), add_top, add_right_bottom);
  ImGui::SetCursorScreenPos(ImVec2(add_x, std::max(add_left_bottom, add_right_bottom)));
  if (duplicate) {
    ImGui::Dummy(ImVec2(0, px(6)));
    note("Already in the list.", true);
  }
  if (active) {
    ImGui::Dummy(ImVec2(0, px(6)));
    // In the words column, never across the seam: the line is short enough to stay under the description.
    ImGui::SetCursorScreenPos(ImVec2(add_x, ImGui::GetCursorScreenPos().y));
    text_wrapped(fonts().small, pal().muted, "Your own settings come back when it stops.", add_left);
  }
  ImGui::PopStyleVar();  // this pane's rows
  ImGui::PopStyleVar();  // begin_section's own, which this card replaced
  end_pane();

}

// ---------------------------------------------------------------- profiles

void profiles_section(ViewEnv& env) {
  UiStore& store = env.store;
  note("Up to four profiles: buffer length, frame rate and quality together. Switch with the 1-4 chips on the panel.");
  ImGui::Dummy(ImVec2(0, px(4)));
  auto reload = [&store](const Json&) {
    store.refresh_profiles();
    store.refresh_config();
  };
  const int active = static_cast<int>(store.config["active_profile"].as_int(-1));
  // The rows are left-packed (nothing on them is labelled), so a full-width card carried a hand's width of empty face on
  // its right. The card is as wide as the rows measured last frame, which is the same trick a row uses for its controls.
  static float pane_w = -1.0f;
  begin_section("##profiles", "", {}, pane_w);
  // One height for everything on a profile's line, and the frame padding is set to reach it: a select is as tall as
  // the font plus its padding, so it came out two pixels taller than the buttons beside it and the row sat crooked.
  const float control_h = 30;  // design units
  float row_left = 0.0f, row_w = 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, (px(control_h) - ImGui::GetFontSize()) / 2));
  for (const UiProfile& profile : store.profiles) {
    ImGui::PushID(profile.index);
    // The gap a separator sits in is the same above and below it, so every line is a row of one rhythm instead of
    // two lines that each carry their own slack. Nothing is added above the first row: the card's own padding is the
    // space over it, the same as the space under the last one.
    if (profile.index > 0) {
      ImGui::Dummy(ImVec2(0, px(8)));
      ImGui::Separator();
      ImGui::Dummy(ImVec2(0, px(8)));
    }
    const bool on = profile.index == active;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float side = px(control_h);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + side, pos.y + side), on ? pal().accent : pal().raised);
    const std::string digit = std::to_string(profile.index + 1);
    draw->AddText(fonts().strong, fonts().strong->LegacySize,
                  ImVec2(pos.x + (side - text_width(fonts().strong, digit)) / 2, pos.y + (side - fonts().strong->LegacySize) / 2),
                  on ? pal().on_accent : pal().muted, digit.c_str());
    ImGui::SetCursorScreenPos(ImVec2(pos.x + side + px(10), pos.y));
    auto update = [&](const char* key, Json value) {
      Json patch = Json::object();
      patch.set(key, std::move(value));
      Json args = Json::object();
      args.set("index", Json(profile.index));
      args.set("patch", patch);
      store.attempt("profiles.update", args, "Not changed", reload);
    };
    int buffer = profile.buffer_seconds;
    if (int_select("##buffer", buffer, {15, 30, 60, 90, 120, 180, 300, 600, 900, 1800}, [](int v) { return format_span(v); }, 90)) {
      update("buffer_seconds", Json(buffer));
    }
    ImGui::SameLine(0, px(8));
    int fps = profile.capture_fps;
    if (int_select("##fps", fps, {30, 60, 120, 144}, [](int v) { return std::to_string(v) + " fps"; }, 90)) update("capture_fps", Json(fps));
    ImGui::SameLine(0, px(8));
    // The profile's Video preset. A custom rate stays the page's own: a profile picks a preset, and the
    // automatic rate follows whatever game, size and codec it is applied to.
    const char* presets[5] = {"low", "standard", "high", "highest", "custom"};
    int preset = 2;
    for (int i = 0; i < 5; ++i) {
      if (profile.preset == presets[i]) preset = i;
    }
    if (select("##preset", {"Low", "Standard", "High", "Highest", "Custom"}, preset, 130)) {
      Json patch = Json::object();
      patch.set("video_preset", Json(presets[preset]));
      if (preset != 4) {
        patch.set("quality", Json(presets[preset]));
        patch.set("peak_mode", Json("auto"));
      }
      Json args = Json::object();
      args.set("index", Json(profile.index));
      args.set("patch", patch);
      store.attempt("profiles.update", args, "Not changed", reload);
    }
    ImGui::SameLine(0, px(8));
    Json index_args = Json::object();
    index_args.set("index", Json(profile.index));
    if (on) {
      if (button("Save current##profile", Variant::normal, nullptr, false, 116, control_h)) store.attempt("profiles.save", index_args, "Not saved", reload);
    } else {
      if (button("Apply##profile", Variant::normal, nullptr, false, 116, control_h)) store.attempt("profiles.apply", index_args, "Not applied", reload);
    }
    ImGui::SameLine(0, px(8));
    if (button("##remove", Variant::ghost, "trash", on || store.profiles.size() <= 1, 0, control_h)) {
      store.attempt("profiles.remove", index_args, "Not removed", reload);
    }
    tooltip("Remove");
    if (profile.index == 0) row_left = pos.x;
    row_w = std::max(row_w, ImGui::GetItemRectMax().x - row_left);
    ImGui::PopID();
  }
  ImGui::PopStyleVar();
  // The card's width for the next frame: the rows' own width plus its padding on both sides and its seam.
  if (row_w > 0.0f) pane_w = row_w + px(12) * 2 + px(2);
  end_section();
  ImGui::Dummy(ImVec2(0, px(8)));
  if (button("Add profile from current settings##add", Variant::normal, "plus", store.profiles.size() >= 4)) {
    store.attempt("profiles.add", Json::object(), "Not added", reload);
  }
}

// ---------------------------------------------------------------- storage

void storage_section(ViewEnv& env) {
  PanelState& panel = env.panel;
  const Json& config = env.store.config;
  if (panel.storage.is_null() && !panel.storage_loading) load(env, "storage.usage", panel.storage, panel.storage_loading);
  auto open_folder = [&](const char* kind) {
    Json args = Json::object();
    args.set("kind", Json(kind));
    env.store.attempt("app.open_folder", args, "The folder did not open");
  };
  auto browse = [&](const std::string& title, const std::string& start, const char* key) {
    Json args = Json::object();
    args.set("title", Json(title));
    args.set("start", Json(start));
    env.store.bridge_call("app.pick_folder", args, [&store = env.store, &panel, key = std::string(key)](bool ok, const Json& data, const std::string& error) {
      if (!ok) {
        store.toast(UiToast::error, "The folder picker did not open", error);
        return;
      }
      const std::string path = data["path"].as_string();
      if (path.empty()) return;  // cancelled
      Json patch = Json::object();
      patch.set(key, Json(path));
      if (key == "clips_dir") patch.set("screenshots_dir", Json(path + "\\Screenshots"));
      store.set_config(patch);
      panel.storage = Json();
    });
  };
  begin_section("##where", "Where things are saved");
  begin_row("Clips", short_path(config["clips_dir"].as_string(), 56), pal().muted);
  if (button("Open##clipsfolder", Variant::ghost, "folder", false, 0, 30)) open_folder("clips");
  ImGui::SameLine(0, px(4));
  if (button("Browse\xE2\x80\xA6##clips", Variant::normal, nullptr, false, 0, 30)) browse("Save clips to", config["clips_dir"].as_string(), "clips_dir");
  end_row();
  // An unset folder is stored as an empty string, not as a missing key.
  std::string recordings_dir = config["recordings_dir"].as_string();
  if (recordings_dir.empty()) recordings_dir = config["clips_dir"].as_string();
  begin_row("Recordings", short_path(recordings_dir, 56), pal().muted);
  if (button("Open##recfolder", Variant::ghost, "folder", false, 0, 30)) open_folder("recordings");
  ImGui::SameLine(0, px(4));
  if (button("Browse\xE2\x80\xA6##rec", Variant::normal, nullptr, false, 0, 30)) {
    browse("Save recordings to", recordings_dir, "recordings_dir");
  }
  end_row();
  begin_row("Screenshots", short_path(config["screenshots_dir"].as_string(), 56), pal().muted);
  if (button("Open##shotsfolder", Variant::ghost, "folder", false, 0, 30)) open_folder("screenshots");
  ImGui::SameLine(0, px(4));
  if (button("Browse\xE2\x80\xA6##shots", Variant::normal, nullptr, false, 0, 30)) {
    browse("Save screenshots to", config["screenshots_dir"].as_string(), "screenshots_dir");
  }
  end_row();
  begin_row("Screenshot format", "JPEG is smaller; PNG is exact.");
  {
    int chosen_format = config["screenshot_format"].as_string("png") == "jpg" ? 1 : 0;
    if (segmented("##shotformat", {"PNG", "JPEG"}, chosen_format)) {
      set(env, "screenshot_format", Json(chosen_format == 1 ? "jpg" : "png"));
    }
  }
  end_row();
  end_section();

  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##usage", "Usage");
  // Its own row: as the pane's aside the button lay over the first figure.
  begin_row("Disk usage", "Counted when this page opens.");
  if (button("Refresh##usage", Variant::ghost, "refresh", panel.storage_loading, 0, 26)) {
    panel.storage = Json();
    load(env, "storage.usage", panel.storage, panel.storage_loading);
  }
  end_row();
  if (panel.storage.is_null() || panel.storage_loading) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    spinner(ImGui::GetWindowDrawList(), ImVec2(pos.x + px(12), pos.y + px(16)), px(7), pal().accent);
    ImGui::Dummy(ImVec2(0, px(32)));
  } else {
    const Json& usage = panel.storage;
    const std::pair<const char*, const char*> rows[4] = {{"Clips", "clips"}, {"Recordings", "recordings"}, {"Screenshots", "screenshots"}, {"Timeline", "sessions"}};
    double used = 0;
    for (const auto& row : rows) {
      const double bytes = usage[std::string(row.second) + "_bytes"].as_double();
      used += bytes;
      begin_row(row.first);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (px(26) - fonts().body->LegacySize) / 2);  // centred on the label beside it
      text(fonts().body, pal().muted, std::to_string(usage[std::string(row.second) + "_count"].as_int()) +
                                       (std::string(row.second) == "sessions" ? " sessions \xC2\xB7 " : " files \xC2\xB7 ") +
                                       format_bytes(bytes));
      end_row();
    }
    begin_row("Free space", "On the drive holding your clips.");
    const double free_bytes = usage["free_bytes"].as_double();
    const float line_top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(line_top + (px(26) - px(6)) / 2);
    level_bar(160, static_cast<float>(used / std::max(1.0, used + free_bytes)), 6);
    ImGui::SameLine(0, px(10));
    ImGui::SetCursorPosY(line_top + (px(26) - fonts().body->LegacySize) / 2);
    text(fonts().body, pal().muted, format_bytes(free_bytes) + " free");
    end_row();
  }
  end_section();

  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##cleanup", "Clean-up");
  begin_row("Old recordings", "The oldest recordings are deleted beyond this. 0 keeps everything.");
  {
    int budget = static_cast<int>(config["recordings_budget_gb"].as_int());
    if (int_select("##budget", budget, {0, 20, 50, 100, 200, 500}, [](int v) { return v ? std::to_string(v) + " GB" : std::string("Unlimited"); })) {
      set(env, "recordings_budget_gb", Json(budget));
    }
  }
  end_row();
  note("Clips and screenshots are never deleted automatically. Timeline footage expires on its own window (Capture > Timeline).");
  end_section();

  // ---- Timeline footage on the disk. Its settings (how long, where it records, idle, old footage) are on the Capture page
  // with the rest of what is recorded; this is the store itself: what it holds, and Erase.
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##timeline", "Timeline footage");
  begin_row("Delete old footage", "Footage left after Timeline is turned off deletes itself after this. Never keeps it until you erase it. While Timeline is on, its length under Capture decides.");
  {
    int keep = static_cast<int>(config["timeline_keep_hours"].as_int(24));
    if (int_select("##keepfootage", keep, {0, 1, 6, 24, 168},
                   [](int v) {
                     if (v == 0) return std::string("Never");
                     if (v == 24) return std::string("1 day");
                     if (v == 168) return std::string("7 days");
                     return std::to_string(v) + " h";
                   }, 160)) {
      set(env, "timeline_keep_hours", Json(keep));
    }
  }
  end_row();
  timeline_storage(env);
  note("How long it keeps footage, and where it records, are set under Capture > Timeline.");
  end_section();

  ImGui::Dummy(ImVec2(0, px(6)));
  if (advanced("##storageadvanced", "Advanced", panel.advanced.storage)) {
    begin_section("##storageadv", "");
    begin_row("Keep free on the drive", "Buffering and recording wait until this much space is free, so a full disk cannot break a capture.");
    {
      int minimum = static_cast<int>(config["min_free_gb"].as_int(2));
      if (int_select("##minfree", minimum, {1, 2, 5, 10, 20}, [](int v) { return std::to_string(v) + " GB"; })) {
        set(env, "min_free_gb", Json(minimum));
      }
    }
    end_row();
    begin_row("Clip file names", "How a new clip, recording or screenshot is named. Tokens: %CCYY %YY %MM %DD %hh %mm %ss %#m %#s.");
    if (text_field("##filename", panel.filename_format, 260, "%CCYY-%MM-%DD %hh-%mm-%ss", 120)) {
      if (panel.filename_format.empty()) {
        panel.filename_format = config["filename_format"].as_string("%CCYY-%MM-%DD %hh-%mm-%ss");
      } else {
        set(env, "filename_format", Json(panel.filename_format));
      }
    }
    end_row();
    end_section();
  }
}

// ---------------------------------------------------------------- hotkeys

namespace {
// The buttons after `Pad:`, each in a cap of its own.
std::vector<std::string> pad_button_names(const std::string& chord) {
  std::vector<std::string> buttons;
  const std::string rest = chord.size() > 4 ? chord.substr(4) : chord;
  size_t start = 0;
  while (start <= rest.size()) {
    size_t plus = rest.find('+', start);
    if (plus == std::string::npos) plus = rest.size();
    if (plus > start) buttons.push_back(rest.substr(start, plus - start));
    if (plus == rest.size()) break;
    start = plus + 1;
  }
  return buttons;
}

// A row's controller binding: one bordered chip holding the pad icon and its buttons, a clear step away
// from the keyboard caps beside it. The chip is exactly as wide as its buttons, so a two-button binding
// does not carry the dead space of a four-button one, and it is centred in the column the rows share so
// chips of unlike width sit on one axis.
bool pad_chord_control(const char* id, const std::string& chord, const std::string& tip, bool problem, float column) {
  const float h = px(24), edge_pad = px(6), gap = px(4), icon_size = px(14);
  const bool bound = !chord.empty();
  ImFont* font = fonts().small;
  const float join = text_width(font, "+");
  const auto cap_width = [&](const std::string& text) {
    return std::max(px(18), text_width(font, text) + px(8));
  };
  const std::vector<std::string> buttons = bound ? pad_button_names(chord) : std::vector<std::string>();
  const char* plus = "+";
  float caps_total = 0;
  for (size_t i = 0; i < buttons.size(); ++i) {
    caps_total += cap_width(buttons[i]);
    if (i + 1 < buttons.size()) caps_total += gap * 2 + join;
  }
  const float w = edge_pad * 2 + icon_size + gap + (bound ? caps_total : text_width(font, plus));

  // The slot is the whole column, so every row's control group is the same width; the chip is drawn
  // centred inside it.
  const ImVec2 slot = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(std::max(column, w), h), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered();
  const bool focused = ImGui::IsItemFocused();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 pos(slot.x + (std::max(column, w) - w) * 0.5f, slot.y);
  const ImVec2 max(pos.x + w, pos.y + h);

  const ImU32 edge = problem ? pal().warn
                     : bound ? (hovered || focused ? pal().accent : with_alpha(pal().accent, 0.45f))
                             : (hovered || focused ? pal().accent : pal().line_strong);
  const ImU32 legend = problem ? pal().warn : bound ? pal().ink : hovered ? pal().ink : pal().faint;
  if (bound) draw->AddRectFilled(pos, max, with_alpha(pal().accent, hovered || focused ? 0.14f : 0.08f));
  else if (hovered || focused) draw->AddRectFilled(pos, max, with_alpha(pal().accent, 0.06f));
  if (bound || hovered || focused || problem) {
    draw->AddRect(pos, max, edge);
  } else {
    // Dashed: an empty binding reads as a slot, not a key.
    const float dash = px(4);
    for (float x = pos.x; x < max.x; x += dash * 2) {
      draw->AddLine(ImVec2(x, pos.y), ImVec2(std::min(x + dash, max.x), pos.y), edge);
      draw->AddLine(ImVec2(x, max.y), ImVec2(std::min(x + dash, max.x), max.y), edge);
    }
    for (float y = pos.y; y < max.y; y += dash * 2) {
      draw->AddLine(ImVec2(pos.x, y), ImVec2(pos.x, std::min(y + dash, max.y)), edge);
      draw->AddLine(ImVec2(max.x, y), ImVec2(max.x, std::min(y + dash, max.y)), edge);
    }
  }
  if (focused) draw->AddRect(ImVec2(pos.x + 1, pos.y + 1), ImVec2(max.x - 1, max.y - 1), pal().accent, 0, 0, px(2));

  const ImU32 glyph = problem ? pal().warn : bound ? pal().accent : legend;
  const float content = icon_size + gap + (bound ? caps_total : text_width(font, plus));
  float x = pos.x + std::max(edge_pad, (w - content) / 2);
  draw_icon(draw, "game", ImVec2(x, pos.y + (h - icon_size) / 2), icon_size, glyph);
  x += icon_size + gap;
  if (bound) {
    for (size_t i = 0; i < buttons.size(); ++i) {
      const float cap_w = cap_width(buttons[i]);
      const ImVec2 cap_min(x, pos.y + (h - px(16)) / 2), cap_max(x + cap_w, pos.y + (h + px(16)) / 2);
      draw->AddRect(cap_min, cap_max, pal().line_strong);
      const float tw = text_width(font, buttons[i]);
      draw->AddText(font, font->LegacySize, ImVec2(cap_min.x + (cap_w - tw) / 2, cap_min.y + (px(16) - font->LegacySize) / 2),
                    legend, buttons[i].c_str());
      x += cap_w;
      if (i + 1 < buttons.size()) {
        draw->AddText(font, font->LegacySize, ImVec2(x + gap, pos.y + (h - font->LegacySize) / 2),
                      with_alpha(legend, 0.6f), "+");
        x += gap * 2 + join;
      }
    }
  } else {
    draw->AddText(font, font->LegacySize, ImVec2(x, pos.y + (h - font->LegacySize) / 2), legend, plus);
  }
  tooltip(tip);
  return pressed;
}

// What pad_chord_control will take, so the row can give the chip its own width and still line the
// column up: the icon, the buttons and the seams between them.
float pad_chord_width(const std::string& chord) {
  const float edge_pad = px(6), gap = px(4), icon_size = px(14);
  ImFont* font = fonts().small;
  const float join = text_width(font, "+");
  if (chord.empty()) return edge_pad * 2 + icon_size + gap + text_width(font, "+");
  float total = edge_pad * 2 + icon_size + gap;
  const std::vector<std::string> buttons = pad_button_names(chord);
  for (size_t i = 0; i < buttons.size(); ++i) {
    total += std::max(px(18), text_width(font, buttons[i]) + px(8));
    if (i + 1 < buttons.size()) total += gap * 2 + join;
  }
  return total;
}

}  // namespace

void hotkeys_section(ViewEnv& env) {
  UiStore& store = env.store;
  begin_section("##hotkeys", "Hotkeys");
  // One column each, measured from every row: the widest keyboard chord and the widest pad chip. The caps
  // are right-aligned in theirs and the chip is drawn at its own width from the second column's left edge.
  float key_column = 0, widest_chip = 0;
  for (const HotkeyDefault& row : kHotkeyDefaults) {
    key_column = std::max(key_column, chord_width(store.chord(row.action), 24));
    widest_chip = std::max(widest_chip, pad_chord_width(store.chord(row.action, true)));
  }
  for (const HotkeyDefault& row : kHotkeyDefaults) {
    const char* action = row.action;
    const std::string problem = store.hotkeys["problems"][action].as_string();
    const std::string pad_problem = store.hotkeys["pad_problems"][action].as_string();
    const std::string pad_chord = store.chord(action, true);
    ImGui::PushID(action);
    // What each key does, so the page explains the difference between Save clip and Quick clip.
    const std::string what = std::string(action) == "save_replay" ? "Keeps the whole buffer, the last " + format_span(store.config["buffer_seconds"].as_double(60))
                           : std::string(action) == "quick_clip" ? "Keeps only the last " + format_span(store.config["quick_clip_seconds"].as_double(20))
                           : std::string(action) == "screenshot" ? "A still of what is being captured"
                           : std::string(action) == "toggle_record" ? "A full recording, beside the buffer"
                           : std::string(action) == "toggle_ui" ? "Opens or hides this window"
                           : std::string(action) == "view_clip" ? "Opens the Timeline viewer over your game"
                           : std::string(action) == "add_marker" ? "Marks this moment on the Timeline"
                                                                 : "Bound to this action";
    const std::string hint = !problem.empty() ? "Not active: " + problem
                            : !pad_problem.empty() ? "Not active: " + pad_problem
                                                   : what;
    begin_row(hotkey_label(action), hint, (problem.empty() && pad_problem.empty()) ? 0 : pal().warn);
    // Two columns pinned to the pane's right seam, so every row lines up whether a pad chord is set or not:
    // the caps end a fixed gap before the pad column, and the widest chip ends at the seam. The chip is only
    // as wide as its buttons and sits centred on the column's axis, so a short binding reads as a chip of its
    // own size rather than one pushed to the left edge of a cell.
    const float column_gap = px(16);
    const float chord_w = chord_width(store.chord(action), 24);
    const float row_y = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, ImGui::GetContentRegionAvail().x - (key_column + column_gap + widest_chip)));
    const float group_x = ImGui::GetCursorScreenPos().x;
    ImGui::SetCursorScreenPos(ImVec2(group_x + std::max(0.0f, key_column - chord_w), row_y));
    if (chord_button("##chord", store.chord(action), "Change the keyboard hotkey: " + hotkey_label(action), true, 24)) {
      env.panel.recording = action;
      env.panel.recorder_controller = false;
    }
    // SameLine starts from the previous item's left edge, so the pad slot is placed by hand: one column,
    // the same on every row, so the chips centre on one axis.
    ImGui::SameLine(0.0f, column_gap);
    ImGui::SetCursorScreenPos(ImVec2(group_x + key_column + column_gap, row_y));
    if (pad_chord_control("##pad", pad_chord, "Change the controller hotkey: " + hotkey_label(action),
                          !pad_problem.empty(), widest_chip)) {
      env.panel.recording = action;
      env.panel.recorder_controller = true;
    }
    end_row();
    ImGui::PopID();
  }
  end_section();
  const int controllers = static_cast<int>(store.hotkeys["controllers"].as_int(0));
  note("Hotkeys work everywhere, in games too. Click one to change it. " +
       (controllers > 0 ? std::to_string(controllers) + (controllers == 1 ? " controller connected." : " controllers connected.")
                        : std::string("Connect a controller to set its buttons.")));
  ImGui::Dummy(ImVec2(0, px(4)));
  // A mouse action only: keeping it out of keyboard navigation stops the focus ring from sitting on it after a click.
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  const bool view_binds = button("View all keybinds##hotkeys", Variant::normal, "keyboard", false, 0, 32, false);
  ImGui::PopItemFlag();
  if (view_binds) env.panel.keybinds_open = true;
  ImGui::SameLine();
  if (button("Reset to defaults##hotkeys", Variant::ghost, "undo")) {
    // One table, shared with the recorder's Default button, so a reset and a per-row default cannot drift.
    for (const HotkeyDefault& row : kHotkeyDefaults) {
      for (const bool controller : {false, true}) {
        Json args = Json::object();
        args.set("action", Json(row.action));
        args.set("chord", Json(default_hotkey(row.action, controller)));
        if (controller) args.set("input", Json("controller"));
        store.attempt("hotkeys.set", args, "A hotkey was not reset");
      }
    }
    store.refresh_hotkeys(350);
    store.toast(UiToast::success, "Hotkeys reset", "Keyboard chords and pad buttons are back to their defaults");
  }
}

// ---------------------------------------------------------------- sharing

void sharing_section(ViewEnv& env) {
  PanelState& panel = env.panel;
  UiStore& store = env.store;
  if (store.webhook.is_null() && !panel.webhook_busy) {
    panel.webhook_busy = true;
    store.bridge_call("share.webhook.get", Json::object(), [&store, &panel](bool ok, const Json& data, const std::string&) {
      panel.webhook_busy = false;
      store.webhook = ok ? data : Json::object();
    });
  }
  begin_section("##sharefit", "Fit for Discord");
  begin_row("Upload limit", "20 MB fits Discord's free limit. A boosted server takes 50 or 100 MB, Nitro 500.");
  {
    const std::vector<int> limits = share_limit_choices();
    const int current = static_cast<int>(store.config["share_limit_mib"].as_int(20));
    std::vector<std::string> labels;
    for (int limit : limits) labels.push_back(std::to_string(limit) + " MB");
    int selected = -1;
    for (size_t i = 0; i < limits.size(); ++i) {
      if (limits[i] == current) selected = static_cast<int>(i);
    }
    if (selected < 0) {  // a limit set by hand in config.json
      labels.push_back(std::to_string(current) + " MB");
      selected = static_cast<int>(limits.size());
    }
    if (segmented("##sharelimit", labels, selected) && selected < static_cast<int>(limits.size())) {
      set(env, "share_limit_mib", Json(limits[selected]));
    }
  }
  end_row();
  end_section();
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##webhook", "Discord servers");
  for (const Json& hook : store.webhook["webhooks"].items()) {
    const std::string id = hook["id"].as_string();
    ImGui::PushID(id.c_str());
    begin_row(hook["name"].as_string(), hook["hint"].as_string() + ". Posts go to the channel configured in Discord.", pal().muted);
    if (button("Remove##webhook", Variant::ghost, "trash", panel.webhook_busy)) {
      panel.webhook_busy = true;
      panel.webhook_problem.clear();
      Json args = Json::object();
      args.set("webhook_id", Json(id));
      store.bridge_call("share.webhook.clear", args, [&store, &panel](bool ok, const Json& data, const std::string& error) {
        panel.webhook_busy = false;
        if (ok) store.webhook = data;
        else panel.webhook_problem = error;
      });
    }
    end_row();
    ImGui::PopID();
  }
  constexpr float webhook_width = 230.0f, webhook_add_width = 44.0f;
  begin_row("Server name", "Enter a name to recognize this destination. It is only stored on this PC.");
  text_field("##webhookname", panel.webhook_name, webhook_width, "Server name", 120);
  const float webhook_field_height = unpx(ImGui::GetItemRectSize().y);
  if (panel.webhook_name.find_first_not_of(" \t\r\n") != std::string::npos &&
      panel.webhook_problem == "Enter a server name first.") {
    panel.webhook_problem.clear();
  }
  ImGui::SameLine();
  ImGui::Dummy(ImVec2(px(webhook_add_width), px(webhook_field_height)));
  end_row();
  begin_row("Webhook URL", "In Discord: Server Settings, Integrations, Webhooks. Choose the channel, then copy its webhook URL.");
  text_field("##webhookurl", panel.webhook_text, webhook_width, "Paste the webhook URL", 400, ImGuiInputTextFlags_Password);
  ImGui::SameLine();
  if (button("Add##webhook", Variant::primary, nullptr, panel.webhook_busy || panel.webhook_text.empty(),
             webhook_add_width, webhook_field_height)) {
    std::string url = panel.webhook_text, name = panel.webhook_name;
    url.erase(0, url.find_first_not_of(" \t\r\n"));
    url.erase(url.find_last_not_of(" \t\r\n") + 1);
    name.erase(0, name.find_first_not_of(" \t\r\n"));
    name.erase(name.find_last_not_of(" \t\r\n") + 1);
    if (name.empty()) {
      panel.webhook_problem = "Enter a server name first.";
    } else {
      panel.webhook_busy = true;
      panel.webhook_problem.clear();
      Json args = Json::object();
      args.set("url", Json(url));
      args.set("name", Json(name));
      args.set("add", Json(true));
      store.bridge_call("share.webhook.set", args, [&store, &panel](bool ok, const Json& data, const std::string& error) {
        panel.webhook_busy = false;
        if (ok) {
          store.webhook = data;
          panel.webhook_text.clear();
          panel.webhook_name.clear();
          store.toast(UiToast::success, "Server saved", "Choose it when posting a clip to Discord");
        } else {
          panel.webhook_problem = error;
        }
      });
    }
  }
  end_row();
  if (!store.webhook["error"].as_string().empty()) text(fonts().small, pal().danger, store.webhook["error"].as_string());
  if (!panel.webhook_problem.empty()) text(fonts().small, pal().danger, panel.webhook_problem);
  end_section();
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##links", "BuffrX links");
  if (store.links.is_null()) {  // read once; link.changed keeps it current
    store.links = Json::object();
    store.refresh_links();
  }
  {
    const std::string name = store.config["share_sender_name"].as_string();
    begin_row("Your name on links",
              name.empty() ? "Left empty, a link says only that a friend sent it. Set it and the page shows \"Shared by\" and the name."
                           : "A link says \"Shared by " + name + "\". Anyone who has the link sees it.",
              panel.link_sender_problem.empty() ? pal().muted : pal().danger);
    if (panel.link_sender_text.empty() && !name.empty() && !ImGui::IsAnyItemActive()) panel.link_sender_text = name;
    text_field("##linksender", panel.link_sender_text, 230, "e.g. your name", 32);
    ImGui::SameLine();
    if (button("Save##linksender", Variant::normal, nullptr, panel.link_sender_text == name)) {
      std::string typed = panel.link_sender_text;
      typed.erase(0, typed.find_first_not_of(" \t"));
      typed.erase(typed.find_last_not_of(" \t") + 1);
      if (typed.size() > 32) {
        panel.link_sender_problem = "Keep it to 32 characters";
      } else {
        panel.link_sender_problem.clear();
        set(env, "share_sender_name", Json(typed));
      }
    }
    end_row();
    if (!panel.link_sender_problem.empty()) text(fonts().small, pal().danger, panel.link_sender_problem);
  }
  begin_row("Link upload speed", "Unlimited sends a clip as fast as your connection allows. A cap keeps a share from using all of your upload.");
  {
    const int caps[5] = {0, 25000, 50000, 100000, 200000};
    const int current = static_cast<int>(store.config["share_upload_kbps"].as_int(0));
    std::vector<std::string> labels = {"Unlimited", "25 Mbit/s", "50 Mbit/s", "100 Mbit/s", "200 Mbit/s"};
    int selected = -1;
    for (int i = 0; i < 5; ++i) {
      if (caps[i] == current) selected = i;
    }
    if (selected < 0) {  // a cap set by hand in config.json
      labels.push_back(std::to_string(current / 1000) + " Mbit/s");
      selected = 5;
    }
    if (segmented("##uploadcap", labels, selected) && selected < 5) set(env, "share_upload_kbps", Json(caps[selected]));
  }
  end_row();
  {
    const auto& links = store.links["links"].items();
    if (links.empty()) {
      begin_row("Live links", "None. Make one from a clip's Share sheet.");
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (px(26) - fonts().body->LegacySize) / 2);
      text(fonts().body, pal().muted, "None");
      end_row();
    }
    for (const Json& link : links) {
      ImGui::PushID(link["id"].as_string().c_str());
      const int64_t left = link["expires"].as_int() - static_cast<int64_t>(std::time(nullptr));
      begin_row(link["name"].as_string(), std::to_string(link["viewers"].as_int()) + " downloads \xC2\xB7 " + link_expiry(left), pal().muted);
      if (button("Stop##live", Variant::ghost, "close", false)) {
        Json args = Json::object();
        args.set("id", link["id"]);
        store.attempt("link.stop", args, "The link did not stop");
      }
      end_row();
      ImGui::PopID();
    }
  }
  end_section();
  note("Nothing leaves this PC unless you share. The network is used only when you press it.");
}

// ---------------------------------------------------------------- system

// Every theme as a tile drawn in its own colours: its ground, a pane with a seam, its text and its
// accent. The chosen one has the 2 px border of the theme that is on now.
void theme_grid(ViewEnv& env) {
  const std::string chosen = theme_colors(env.store.theme()).id;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float gap = px(8);
  const int columns = avail > px(600) ? 4 : avail > px(420) ? 3 : 2;
  const float w = std::floor((avail - gap * (columns - 1)) / columns), h = px(84);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  auto color = [](uint32_t c) { return IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 0xff); };
  const std::vector<ThemeColors>& all = themes();
  for (size_t i = 0; i < all.size(); ++i) {
    const ThemeColors& t = all[i];
    const ImVec2 min(origin.x + (i % columns) * (w + gap), origin.y + (i / columns) * (h + gap));
    const ImVec2 max(min.x + w, min.y + h);
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID(t.id);
    const bool pressed = ImGui::InvisibleButton("theme", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const bool on = chosen == t.id;
    draw->AddRectFilled(min, max, color(t.ground));
    // A small pane inside the tile, with the theme's accent as its focus border and a filled row.
    const ImVec2 pane_min(min.x + px(8), min.y + px(8)), pane_max(max.x - px(8), max.y - px(8));
    draw->AddRectFilled(pane_min, pane_max, color(t.tile));
    draw->AddRect(pane_min, pane_max, color(t.line));
    draw->AddRectFilled(ImVec2(pane_min.x + px(6), pane_max.y - px(12)), ImVec2(pane_min.x + px(34), pane_max.y - px(6)), color(t.acc));
    draw->AddRectFilled(ImVec2(pane_min.x + px(38), pane_max.y - px(12)), ImVec2(pane_min.x + px(52), pane_max.y - px(6)), color(t.raised));
    // The name, and the hint that tells the themes apart ("ember orange", "arcade yellow"), each in the
    // theme's own text colours.
    draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(pane_min.x + px(6), pane_min.y + px(4)), color(t.fg), t.name);
    // A hint too long for the tile breaks after its comma instead of running under the edge.
    std::string first = t.hint, second;
    if (text_width(fonts().label, first) > pane_max.x - pane_min.x - px(14)) {
      const size_t comma = first.find(", ");
      if (comma != std::string::npos) {
        second = first.substr(comma + 2);
        first.resize(comma + 1);
      }
    }
    draw->PushClipRect(pane_min, ImVec2(pane_max.x - px(4), pane_max.y), true);
    const float hint_y = pane_min.y + px(6) + fonts().strong->LegacySize;
    draw->AddText(fonts().label, fonts().label->LegacySize, ImVec2(pane_min.x + px(6), hint_y), color(t.muted), first.c_str());
    if (!second.empty()) {
      draw->AddText(fonts().label, fonts().label->LegacySize, ImVec2(pane_min.x + px(6), hint_y + fonts().label->LegacySize + px(1)),
                    color(t.muted), second.c_str());
    }
    draw->PopClipRect();
    const float border = on ? px(2) : 1.0f;
    draw->AddRect(ImVec2(min.x + border / 2, min.y + border / 2), ImVec2(max.x - border / 2, max.y - border / 2),
                  on ? pal().accent : hovered ? pal().muted : pal().line, 0, 0, border);
    // No tooltip: the tile already says its name and hint, and a tooltip follows the pointer across the next tile.
    if (pressed && !on) set(env, "theme", Json(t.id));
  }
  const int rows = static_cast<int>((all.size() + columns - 1) / columns);
  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (h + gap)));
  ImGui::Dummy(ImVec2(avail, 0));
  note("Clutch is the main theme. The editor and the notification card follow the one chosen here.");
}

void about_section(ViewEnv& env) {
  PanelState& panel = env.panel;
  const Json& hello = env.store.hello;
  const std::string version = hello.is_null() ? std::string(BUFFRX_VERSION) : hello["engine"].as_string();

  begin_section("##about_now", "BuffrX");
  begin_row("Version", hello.is_null() ? std::string("Connecting\xE2\x80\xA6") : version, pal().muted);
  if (button(panel.restarting ? "Restarting\xE2\x80\xA6##restart" : "Restart engine##restart", Variant::ghost, "refresh",
             panel.restarting, 150, 30)) {
    panel.restarting = true;
    env.store.bridge_call("engine.restart", Json::object(), [&panel, &store = env.store](bool ok, const Json&, const std::string& error) {
      panel.restarting = false;
      if (ok) store.toast(UiToast::success, "Engine restarted");
      else store.toast(UiToast::error, "The engine did not restart", error);
    });
  }
  end_row();
  // The check, its result, and the release page belong to the same row.
  {
    const Json& update = env.store.status["update"];
    const std::string state = kAutoUpdate ? update["state"].as_string() : std::string();
    const std::string ready_version = update["version"].as_string();
    const bool newer = !hello.is_null() && update_is_newer(panel.update_latest, version);
    const bool ready = state == "ready" && !ready_version.empty();
    std::string line;
    if (state == "downloading") {
      line = "Downloading " + ready_version + " \xC2\xB7 " + std::to_string(update["percent"].as_int(0)) + "%";
    } else if (ready) {
      line = ready_version + " is ready to install";
    } else if (state == "installing") {
      line = "Installing " + ready_version + "\xE2\x80\xA6";
    } else if (state == "failed") {
      line = update["error"].as_string("The update did not download");
    } else if (panel.update_checking) {
      line = "Checking for updates\xE2\x80\xA6";
    } else if (!panel.update_error.empty()) {
      line = panel.update_error;
    } else if (newer) {
      line = "Update " + panel.update_latest + " available";
    } else if (!panel.update_latest.empty()) {
      line = "You have the newest release";
    } else {
      line = "No check has run yet";
    }
    if (!panel.update_checking && state.empty() && line != "No check has run yet" && panel.update_checked_ms > 0) {
      line += std::string(" \xC2\xB7 checked ") +
              format_ago(panel.update_checked_ms / 1000, static_cast<int64_t>(std::time(nullptr)));
    }
    begin_row("Updates", line,
              (ready || (newer && panel.update_error.empty() && !panel.update_checking)) ? pal().accent : pal().muted);
    if (ready) {
      if (button("Restart to update##update_restart_row", Variant::primary, "refresh", false, 170, 30)) {
        env.shell.install_update();
      }
      ImGui::SameLine(0, px(8));
      if (button("Later##update_later_row", Variant::ghost, nullptr, false, 80, 30)) {
        env.panel.update_offered = ready_version;
        env.panel.update_prompted = ready_version;
        env.shell.save_layout();
      }
    } else {
      if (button(panel.update_checking ? "Checking\xE2\x80\xA6##check" : "Check updates##check", Variant::normal, "refresh",
                 panel.update_checking, 160, 30)) {
        env.shell.check_update(true);
      }
      ImGui::SameLine(0, px(8));
      if (button("View releases##updates", Variant::ghost, "external", false, 150, 30)) {
        Json args = Json::object();
        args.set("url", Json(kReleasesUrl));
        env.store.attempt("app.open_url", args, "The page did not open");
      }
    }
    end_row();
  }
  end_section(3);  // the last row is a 30-unit button line: trim so the foot matches the top

  // The sign-off on the left, the two links as marks on the right: one row under the pane, no second box.
  ImGui::Dummy(ImVec2(0, px(6)));
  const float marks = px(30) * 3 + px(8) * 2;
  const ImVec2 row = ImGui::GetCursorScreenPos();
  const float row_w = ImGui::GetContentRegionAvail().x;
  const float text_w = std::max(px(120), row_w - marks - px(24));
  // Two quiet lines, each sized to always fit the room (never wrapped), centred on the icons.
  const char* lines[2] = {
      "BuffrX is a little project built with a lot of care. Made by one developer, free forever, and always improving.",
      kAutoUpdate ? "No accounts or telemetry. Checks for updates once every 24 hours."
                  : "No accounts or telemetry. Checks for updates once every 24 hours; a new release opens in your browser."};
  ImFont* sign_font = fonts().small;
  for (int i = 0; i < 2; ++i) {
    float size = sign_font->LegacySize * 0.88f;
    const float natural = sign_font->CalcTextSizeA(size, FLT_MAX, 0.0f, lines[i]).x;
    if (natural > text_w) size *= text_w / natural;
    ImGui::GetWindowDrawList()->AddText(sign_font, size, ImVec2(row.x + px(2), row.y + px(i == 0 ? 8 : 22) - size * 0.5f),
                                        i == 0 ? pal().ink : pal().muted, lines[i]);
  }
  const float text_end = row.y;
  ImGui::SetCursorScreenPos(ImVec2(row.x + row_w - marks, row.y));
  if (icon_button("##github", "github", "GitHub", false, 30, 18)) {
    Json args = Json::object();
    args.set("url", Json(kGithubUrl));
    env.store.attempt("app.open_url", args, "The link did not open");
  }
  ImGui::SameLine(0, px(8));
  if (icon_button("##discord", "discord", "Discord", false, 30, 18)) {
    Json args = Json::object();
    args.set("url", Json(kDiscordUrl));
    env.store.attempt("app.open_url", args, "The link did not open");
  }
  ImGui::SameLine(0, px(8));
  if (icon_button("##website", "globe", "buffrx.app", false, 30, 15)) {
    Json args = Json::object();
    args.set("url", Json(kSiteUrl));
    env.store.attempt("app.open_url", args, "The link did not open");
  }
  ImGui::SetCursorScreenPos(ImVec2(row.x, std::max(text_end, row.y + px(30))));
  ImGui::Dummy(ImVec2(row_w, 0));
}

void theme_section(ViewEnv& env) {
  begin_section("##uiscale", "Interface size");
  begin_row("UI scale", "Changes the panel and quick panel. The panel scales down if needed to fit the screen.");
  int scale = static_cast<int>(env.store.config["ui_scale_percent"].as_int(100));
  if (slider_commit("##uiscale", scale, 75, 150, "%d%%", 220)) set(env, "ui_scale_percent", Json(scale));
  end_row();
  end_section();
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##themes", "themes");
  theme_grid(env);
  end_section();
}

void system_section(ViewEnv& env) {
  PanelState& panel = env.panel;
  const Json& config = env.store.config;
  begin_section("##notifications", "Notifications");
  switch_row(env, "notifications", "Show notifications", "A small card when a clip, screenshot or recording is saved or something needs you.");
  switch_row(env, "notify_over_fullscreen", "Show over fullscreen games",
             "Off: no card over a fullscreen game, because a window on top of it can cost the game a frame. Warnings still show.",
             !config["notifications"].as_bool());
  switch_row(env, "notification_sound", "Notification sound",
             "A different chime for a clip saved, a recording started, a recording stopped and a problem.",
             !config["notifications"].as_bool());
  switch_row(env, "notify_sound_while_recording", "Sound while recording",
             "On: the chime is heard even while the buffer or a recording writes, and it ends up in the clip's audio. Off: the chime is silent while capturing.",
             !config["notifications"].as_bool());
  end_section();
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##startup", "Startup");
  switch_row(env, "launch_on_startup", "Start with Windows", "BuffrX starts buffering when you sign in.");
  switch_row(env, "start_minimized", "Start in the tray", "No window at sign-in; the tray icon opens it.");
  // Buffering at startup is separate from starting at all: someone who only wants the hotkeys and
  // the editor still wants BuffrX open, just not a ring filling the disk behind it.
  switch_row(env, "start_buffering", "Start buffering", "Off means the ring waits until you ask, from the tray or the panel.");
  // Nothing is written while the capture works (recovery_policy.h): this row only decides whether the RAM ring is
  // saved when the engine notices a fault that would otherwise lose it.
  switch_row(env, "crash_recovery", "Save the buffer if capture fails",
             "When the capture freezes or the recording stops by itself, save what the buffer still holds as a clip. "
             "Writes nothing while everything runs; a kill or a power cut loses the buffer either way.");
  // A running engine cannot change its own token. Report its actual token and any pending change.
  {
    const bool wants = config["run_as_admin"].as_bool();
    const bool now = env.store.status["is_elevated"].as_bool();
    const std::string hint = wants && now ? "BuffrX is running as administrator."
                             : wants ? "Quit BuffrX from the tray, then launch it again. Windows will ask for permission."
                             : now ? "BuffrX is running as administrator. Quit and launch it again to turn this off."
                                   : "Off \xE2\x80\x94 needed for anti-cheat games.";
    switch_row(env, "run_as_admin", "Run as administrator", hint, false, pal().muted);
  }
  end_section();
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##files", "Files");
  begin_row("Data folder", "Settings, profiles, the library and the logs live here.", pal().muted);
  if (button("Open##datadir", Variant::ghost, "folder", false, 0, 30)) {
    Json args = Json::object();
    args.set("kind", Json("data"));
    env.store.attempt("app.open_folder", args, "The folder did not open");
  }
  end_row();
  begin_row("Logs", "What the engine did, written for a bug report.", pal().muted);
  if (button("Open##logsdir", Variant::ghost, "folder", false, 0, 30)) {
    Json args = Json::object();
    args.set("kind", Json("logs"));
    env.store.attempt("app.open_folder", args, "The folder did not open");
  }
  end_row();
  end_section();
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_section("##reset", "Reset");
  begin_row("Reset all settings", "Picture, audio, hotkeys, storage and sharing go back to their defaults. Files are untouched.");
  {
    // 0 is "not armed": the signed compare alone reads a machine up more than 24.8 days (now past
    // 2^31) as always armed, which would show the destructive button armed on first sight.
    const bool reset_armed = panel.reset_armed_until != 0 && static_cast<int32_t>(env.now_ms - panel.reset_armed_until) < 0;
    if (button(reset_armed ? "Reset everything?" : "Reset all", reset_armed ? Variant::danger : Variant::ghost, "undo", false, 0, 26)) {
      if (!reset_armed) {
        panel.reset_armed_until = env.now_ms + 4000;
      } else {
        panel.reset_armed_until = 0;
        env.store.attempt("config.reset", Json::object(), "The settings were not reset", [&store = env.store](const Json& data) {
          store.config = data;
          store.toast(UiToast::success, "Settings reset", "Clips, recordings, screenshots and profiles are untouched.");
        });
      }
    }
    tooltip(reset_armed ? "Press again to reset every setting. It cannot be undone."
                        : "Put every setting back to its default.");
  }
  end_row();
  end_section(4);
}

}  // namespace

void draw_settings(ViewEnv& env, float height) {
  PanelState& panel = env.panel;
  const Json& config = env.store.config;
  int at = 0;
  for (int i = 0; i < kSectionCount; ++i) {
    if (panel.settings == kSections[i].id) at = i;
  }
  // A view id this build does not know (a probe typo, a newer build's page) draws the first section and
  // says so in the state too, so the heading and the arrow keys cannot disagree.
  panel.settings = kSections[at].id;
  // Entering a section reads its lists again: a headset plugged in, a window opened, a folder that grew.
  if (panel.settings != panel.settings_loaded) {
    panel.settings_loaded = panel.settings;
    if (!panel.displays_loading) panel.displays = Json();
    if (!panel.windows_loading) panel.windows = Json();
    if (!panel.devices_loading) panel.devices = Json();
    if (!panel.storage_loading) panel.storage = Json();
    if (!panel.encoder_loading) panel.encoder = Json();
    panel.encoder_failed = false;
    panel.picking_window = false;  // reopening Settings never shows a stale picker
    panel.filename_format = env.store.config["filename_format"].as_string("%CCYY-%MM-%DD %hh-%mm-%ss");
  }
  // Up and down move through the sections when no field has focus (Rewind's keys).
  if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !panel.popup_was_open) {
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && at < kSectionCount - 1) panel.settings = kSections[at + 1].id;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && at > 0) panel.settings = kSections[at - 1].id;
    for (int i = 0; i < kSectionCount && i < 9; ++i) {
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt) {
        panel.settings = kSections[i].id;
      }
    }
  }
  // The spine: the sections as a launcher list in a pane, the chosen one filled with the accent, and what
  // is running in a pane of its own at the foot.
  const float gap = px(8);
  const float spine_w = px(216);
  const float row_top = ImGui::GetCursorPosY();
  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + gap, row_top));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
  // The spine scrolls when the work area cannot show every section at once (150% on a 1366x768 laptop
  // leaves about 390 design units for 11 rows). Without the bar, Sharing, Theme and System were simply
  // cut off, and there was no way to reach them: a paned list has no line of its own to scroll.
  ImGui::BeginChild("##spine", ImVec2(spine_w, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::Dummy(ImVec2(0, px(2)));
  {
    // What is running, measured first so the list above it knows its height. Performance mode is a layer over the
    // user's own settings, so while it is on the pane says only that: the encoder and the numbers behind it are not
    // the ones the person chose.
    const bool performance_on = env.store.status["performance"]["active"].as_bool(false);
    const std::string summary_title =
        performance_on ? "Performance mode on" : lower_ascii(encoder_name(env.store.status["encoder"].as_string()));
    std::string summary;
    if (!performance_on) {
      summary = "Reading settings\xE2\x80\xA6";
      if (config.is_object()) {
        // The encoder line above already names the codec.
        summary = lower_ascii(quality_label(config["quality"].as_string("high"))) + " quality \xC2\xB7 " +
                  std::to_string(config["capture_fps"].as_int()) + " fps \xC2\xB7 " + format_span(config["buffer_seconds"].as_double()) +
                  " buffer";
      }
    }
    const float summary_h = px(15) * 2 + fonts().strong->LegacySize + px(18) +
                            (summary.empty() ? 0.0f
                                             : fonts().small->CalcTextSizeA(fonts().small->LegacySize, FLT_MAX,
                                                                            spine_w - px(28), summary.c_str()).y);
    // The list never shrinks below the height its rows need, so a short window scrolls the spine rather
    // than squeezing the last sections out of the pane underneath it.
    const float rows_h = px(33) * (kSectionCount + 1) + px(9) + px(26);
    const float room = ImGui::GetContentRegionAvail().y - summary_h - gap - px(1);
    begin_pane("##sections", "settings", ImVec2(-1, std::max(rows_h, room)));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x, h = px(32);
    auto row = [&](const char* id, const char* icon, const std::string& name, const char* key, bool on) {
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
      const bool hovered = ImGui::IsItemHovered();
      // The accent moves between sections with a fade; a hovered one warms up.
      const float chosen = anim_item(on ? 1.0f : 0.0f, kRateState, 0x5ec7u);
      const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
      if (hover > 0.0f && chosen < 1.0f) draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), with_alpha(pal().raised, hover * (1.0f - chosen)));
      if (chosen > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), with_alpha(pal().accent, chosen));
      const ImU32 fg = mix(mix(pal().muted, pal().ink, hover), pal().on_accent, chosen);
      const float glyph = px(15);
      draw_icon(draw, icon, ImVec2(pos.x + px(8), pos.y + (h - glyph) / 2), glyph, fg);
      draw->AddText(on ? fonts().strong : fonts().body, fonts().body->LegacySize, ImVec2(pos.x + px(32), pos.y + (h - fonts().body->LegacySize) / 2),
                    on ? pal().on_accent : hovered ? pal().ink : pal().ink, name.c_str());
      if (key) {
        draw->AddText(fonts().label, fonts().label->LegacySize,
                      ImVec2(pos.x + w - px(8) - text_width(fonts().label, key), pos.y + (h - fonts().label->LegacySize) / 2), fg, key);
      }
      ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h + px(1)));
      return pressed;
    };
    if (row("##back", "chevronLeft", "Panel", "esc", false)) panel.settings.clear();
    const ImVec2 rule = ImGui::GetCursorScreenPos();
    draw->AddLine(ImVec2(rule.x, rule.y + px(4)), ImVec2(rule.x + w, rule.y + px(4)), pal().line);
    ImGui::SetCursorScreenPos(ImVec2(rule.x, rule.y + px(9)));
    for (int i = 0; i < kSectionCount; ++i) {
      ImGui::PushID(i);
      // Each section shows the digit that jumps to it; Up and Down also move through them. About has no
      // digit: the keys reach the first nine, and the list ends on the page that is not a setting.
      char digit[3] = {0, 0, 0};
      if (i < 9) digit[0] = static_cast<char>('1' + i);
      if (row("##section", kSections[i].icon, kSections[i].label, i < 9 ? digit : nullptr, i == at)) panel.settings = kSections[i].id;
      ImGui::PopID();
    }
    end_pane();
    begin_pane("##running", "running", ImVec2(-1, 0));
    text_ellipsis(fonts().strong, pal().ink, summary_title, spine_w - px(28));
    if (!summary.empty()) {
      ImGui::Dummy(ImVec2(0, px(4) - ImGui::GetStyle().ItemSpacing.y));
      text_wrapped(fonts().small, pal().muted, summary, spine_w - px(28));
    }
    end_pane();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::SameLine(0, 0);
  // The section, on the ground: its panes carry their own borders.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(28), px(18)));
  ImGui::BeginChild("##section", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollWithMouse);
  smooth_scroll(px(96));  // J2: a Settings page glides on the wheel like the library
  // V5: the section rises 8 px into place as it fades in: the heading by its draw list, the panes (child windows with draw lists of
  // their own) by the room above them. The list on the left keeps its place.
  const float section_arrived = appear(ImGui::GetID(kSections[at].id), kDurBase);
  const float section_lift = px(8) * (1.0f - section_arrived);
  // The page is as wide as its rows can use: a label and its control a few hundred pixels apart read as two
  // separate things. On a wide window the column is centred in what the section list leaves, so the
  // ground on its right is not a dead third (2026-11-02, from the UI review).
  const float max_width = px(700);
  const float avail = ImGui::GetContentRegionAvail().x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - max_width) / 2));
  ImGui::BeginGroup();
  ImGui::PushItemWidth(std::min(avail, max_width));
  ImGui::Dummy(ImVec2(std::min(avail, max_width), 0));
  if (!config.is_object()) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    spinner(ImGui::GetWindowDrawList(), ImVec2(pos.x + std::min(avail, max_width) / 2, pos.y + px(100)), px(10), pal().accent);
  } else {
    text(fonts().display, pal().ink, kSections[at].label);
    ImGui::Dummy(ImVec2(0, px(4) + section_lift));
    ImGui::BeginChild("##sectionbody", ImVec2(std::min(avail, max_width), 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    kSections[at].draw(env);
    // A focused control draws its ring 2 px outside its box, and the page's last row sits flush with this
    // child's clip, which cut the ring's bottom edge off. A few pixels of room keep it whole.
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::EndChild();
  }
  ImGui::PopItemWidth();
  ImGui::EndGroup();
  // Another section comes in from the ground. The page gliding in already carries the first one.
  shift_since(ImGui::GetWindowDrawList(), 0, 0.0f, section_lift);  // the heading
  {
    const ImVec2 at_min = ImGui::GetWindowPos();
    fade_in_from(ImGui::GetForegroundDrawList(), at_min, ImVec2(at_min.x + ImGui::GetWindowWidth(), at_min.y + ImGui::GetWindowHeight()),
                 pal().canvas, section_arrived);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

}  // namespace bx::ui
