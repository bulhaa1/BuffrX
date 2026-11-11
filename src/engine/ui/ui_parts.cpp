#include "ui_parts.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "../config.h"
#include "../encoder_settings.h"
#include "../gamepad.h"
#include "../platform.h"
#include "../public_links.h"
#include "../update_math.h"
#include "../update_notes.h"
#include "../update_policy.h"
#include "../../version.h"
#include "../../shared/log.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

namespace bx::ui::parts {

std::string hotkey_label(const std::string& action) {
  if (action == "save_replay") return "Save clip";
  if (action == "quick_clip") return "Save quick clip";
  if (action == "screenshot") return "Screenshot";
  if (action == "toggle_record") return "Start / stop recording";
  if (action == "toggle_ui") return "Show / hide BuffrX";
  if (action == "view_clip") return "Timeline";
  if (action == "add_marker") return "Add timeline marker";
  return action;
}

// ---------------------------------------------------------------- actions

void toggle_buffer(ViewEnv& env) {
  const std::string state = env.store.status["buffer_state"].as_string();
  const bool on = state == "on" || state == "starting";
  if (on && (env.store.status["timeline"]["enabled"].as_bool() || env.store.config["timeline_minutes"].as_int(0) > 0)) {
    env.panel.buffer_stop_open = true;
    return;
  }
  env.store.attempt(on ? "buffer.stop" : "buffer.start", Json::object(),
                    on ? "The buffer did not stop" : "The buffer did not start",
                    [&store = env.store](const Json& status) { store.status = status; });
}

void save_clip(ViewEnv& env) {
  if (env.panel.saving && env.now_ms < env.panel.saving_until) return;
  // A recording shares the ring's encoder, so clips keep working while one runs; if the engine had to give
  // the recording an encoder of its own it says so when the save is refused.
  if (!env.store.buffer_on()) {
    // The engine refuses a save in every state but "on" and says which one it is in. "Off" is only true of
    // off: a buffer still waiting for its first frame is starting, and being told it was off sent the
    // reader looking for a switch that was already on.
    const std::string state = env.store.status["buffer_state"].as_string("off");
    const char* why = state == "starting"    ? "The buffer is still starting: its first frame has not arrived yet."
                      : state == "stopping" ? "The buffer is still stopping. Try again in a moment."
                                            : "The buffer is off. Start it and the last moments are kept.";
    env.store.toast(UiToast::error, "Clip not saved", why);
    return;
  }
  env.panel.saving = true;
  env.panel.saving_until = env.now_ms + 800;
  env.store.attempt("buffer.save", Json::object(), "Clip not saved");
}

bool draw_save_feedback(ViewEnv& env, ImDrawList* draw, ImVec2 icon, float size, ImU32 color) {
  if (!env.panel.saving) return false;
  const int32_t elapsed = static_cast<int32_t>(env.now_ms - env.panel.saving_until);
  if (elapsed < 0) {
    spinner(draw, ImVec2(icon.x + size / 2, icon.y + size / 2), px(6), color);
    anim_touch_slow();
    return true;
  }
  if (elapsed >= 900) return false;
  const float fade = elapsed < 500 ? 1.0f : 1.0f - ease_in((elapsed - 500) / 400.0f);
  draw_icon(draw, "check", icon, size, with_alpha(color, fade), 1.7f);
  anim_touch_slow();
  return true;
}

void quick_clip(ViewEnv& env) {
  Json args = Json::object();
  args.set("seconds", Json(static_cast<int>(env.store.config["quick_clip_seconds"].as_int(20))));
  env.store.attempt("buffer.save", args, "Clip not saved");
}

void screenshot(ViewEnv& env) {
  Json args = Json::object();
  args.set("format", Json("png"));
  env.store.attempt("screenshot.take", args, "Screenshot not taken");
}

void toggle_recording(ViewEnv& env) {
  const bool recording = env.store.status["recording"].as_bool();
  env.store.attempt(recording ? "recording.stop" : "recording.start", Json::object(),
                    recording ? "The recording did not stop" : "The recording did not start",
                    [&store = env.store](const Json& status) { store.status = status; });
}

void open_editor(ViewEnv& env, const std::vector<std::string>& files, bool preview) {
  // Asked before the panel steps aside: without the WebView2 runtime the editor dies a moment after it starts,
  // and hiding the panel first left a click that did nothing at all, with the reason only in the log. A card
  // rather than a message box: this runs in the frame the panel is drawing, and a modal box here would race
  // that frame's own window messages.
  if (!webview2_available()) {
    env.store.toast(UiToast::error, "The editor needs WebView2",
                    "This PC does not have Microsoft's WebView2 runtime. Everything else still works; install it and "
                    "open the editor again.");
    return;
  }
  Json list = Json::array();
  for (const std::string& file : files) list.push(Json(file));
  Json args = Json::object();
  args.set("files", list);
  if (preview) args.set("preview", Json(true));
  env.store.attempt("editor.open", args, preview ? "The preview did not open" : "The editor did not open");
}

void set(ViewEnv& env, const char* key, Json value) {
  Json patch = Json::object();
  patch.set(key, std::move(value));
  env.store.set_config(patch);
}

void timeline_action(ViewEnv& env) {
  // The preview is a process of its own, not a view of this window: same press as Alt+4 (timeline_pressed), which
  // opens it on playable footage, closes one that is open, and falls back to the saved-clip viewer when there is none.
  Json args = Json::object();
  args.set("view", Json("timeline"));
  env.store.attempt("ui.show", args, "The Timeline preview did not open");
}

namespace {
// How long a window is, said in a sentence: "30 minutes", "1 hour".
std::string window_words(int minutes) {
  if (minutes == 60) return "1 hour";
  if (minutes == 120) return "2 hours";
  return std::to_string(minutes) + " minutes";
}
}  // namespace

void timeline_notices(UiStore& store) {
  // Each writer state change earns one toast, and only a change: the first frame a window draws knows the state it
  // finds and says nothing about it. `timeline.changed` is already unread by the store, and a status frame carries it.
  static std::string last;
  if (!store.connected) return;
  const Json& timeline = store.status["timeline"];
  const std::string state = timeline["state"].as_string("off");
  if (state == last) return;
  const std::string before = last;
  last = state;
  if (before.empty()) return;  // what this window opened onto is not news
  if (state == "writing" && before != "writing") {
    const int minutes = static_cast<int>(timeline["minutes"].as_int(0));
    store.toast(UiToast::success, "Timeline started",
                "It keeps the last " + window_words(minutes) + " on the disk while a game and the buffer run.");
  } else if (state == "paused_disk") {
    store.toast(UiToast::error, "Timeline stopped",
                "Free space on the clip drive is low. Erase Timeline footage in Settings, or free some space.");
  }
}

// ---------------------------------------------------------------- titlebar

bool bar_button(ViewEnv& env, const char* id, const char* icon, const std::string& tip, bool active) {
  const bool pressed = icon_button(id, icon, tip, active, 28, 15);
  env.shell.no_drag(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  return pressed;
}

std::string update_state_text(const ViewEnv& env) {
  const std::string running = env.store.hello.is_null() ? std::string(BUFFRX_VERSION) : env.store.hello["engine"].as_string();
  const Json& update = env.store.status["update"];
  const std::string state = kAutoUpdate ? update["state"].as_string() : std::string();
  const bool downloading = state == "downloading";
  const bool ready = state == "ready";
  const bool failed = state == "failed";
  if (!(update_should_badge(env.panel.update_latest, running) || downloading || ready || failed)) return {};
  if (ready) return "UPDATE READY";
  if (downloading) return "GETTING UPDATE";
  if (failed) return "UPDATE FAILED";
  return "UPDATE AVAILABLE";
}

bool update_badge(ViewEnv& env, bool compact, bool with_label) {
  const std::string running = env.store.hello.is_null() ? std::string(BUFFRX_VERSION) : env.store.hello["engine"].as_string();
  const Json& update = env.store.status["update"];
  const std::string state = kAutoUpdate ? update["state"].as_string() : std::string();
  const bool downloading = state == "downloading";
  const bool ready = state == "ready";
  const bool failed = state == "failed";
  const std::string label = update_state_text(env);
  if (label.empty()) return false;
  const char* tip = ready ? "Update ready: restart to install" : downloading ? "Getting the update" : "A new update is available";
  const ImU32 accent = pal().accent;
  // The titlebar's label sits in the same 28-pixel row as the badge icon, so the two centre together.
  if (with_label) {
    const float label_h = px(28);
    const ImVec2 label_at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(text_width(fonts().label, label), label_h));
    ImGui::GetWindowDrawList()->AddText(
        fonts().label, fonts().label->LegacySize,
        ImVec2(label_at.x, label_at.y + (label_h - fonts().label->LegacySize) / 2), accent, label.c_str());
    ImGui::SameLine(0, px(8));
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float side = px(compact ? 22 : 28), tile = px(compact ? 17 : 22), glyph = px(compact ? 12 : 16);
  const bool pressed = ImGui::InvisibleButton("##update", ImVec2(side, side), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered();
  const float hover = anim_item((hovered || ImGui::IsItemFocused()) ? 1.0f : 0.0f, kRateHover);
  const float sink = px(1) * press_t();
  env.shell.no_drag(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  tooltip(tip);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 min(at.x + (side - tile) / 2, at.y + (side - tile) / 2);
  const ImVec2 max(min.x + tile, min.y + tile);
  // An outline, not a filled block: the badge stays quiet in the titlebar and the accent stays readable on
  // every theme (test_themes holds accent against the ground at 3:1).
  if (hover > 0.0f) draw->AddRectFilled(min, max, with_alpha(accent, 0.12f * hover));
  draw->AddRect(min, max, accent, 0, 0, px(1));
  draw_icon(draw, "update", ImVec2(at.x + (side - glyph) / 2 + sink, at.y + (side - glyph) / 2 + sink),
            glyph, accent, compact ? 1.5f : 1.75f);
  if (pressed) ImGui::OpenPopup("##update_ready");
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(14)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(6)));
  ImGui::SetNextWindowSizeConstraints(ImVec2(px(312), 0), ImVec2(px(312), FLT_MAX));
  if (ImGui::BeginPopup("##update_ready")) {
    const float content = ImGui::GetContentRegionAvail().x;
    const float action_width = unpx(content - px(8) - px(96));
    const bool focus_ring = ImGui::GetIO().NavVisible;
    const auto centre = [&](float width) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (content - width) / 2));
    };
    const std::string to = ready || downloading ? update["version"].as_string(env.panel.update_latest) : env.panel.update_latest;
    if (ready) {
      centre(text_width(fonts().strong, "UPDATE READY"));
      text(fonts().strong, pal().accent, "UPDATE READY");
      ImGui::Dummy(ImVec2(0, px(4)));
      centre(text_width(fonts().body, "BuffrX " + to + " is ready to install."));
      text(fonts().body, pal().muted, "BuffrX " + to + " is ready to install.");
      ImGui::Dummy(ImVec2(0, px(8)));
      if (button("Restart to update##restart_now", Variant::primary, "refresh", false, action_width, 30, focus_ring)) {
        env.shell.install_update();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine(0, px(8));
      if (button("Later##later", Variant::normal, nullptr, false, 96, 30, focus_ring)) {
        env.panel.update_offered = env.panel.update_latest;
        env.panel.update_prompted = to;
        env.panel.update_restart_open = false;
        env.shell.save_layout();
        ImGui::CloseCurrentPopup();
      }
    } else if (downloading) {
      centre(text_width(fonts().strong, "GETTING UPDATE"));
      text(fonts().strong, pal().accent, "GETTING UPDATE");
      ImGui::Dummy(ImVec2(0, px(4)));
      centre(text_width(fonts().strong, to));
      text(fonts().strong, pal().ink, to);
      ImGui::Dummy(ImVec2(0, px(8)));
      const float fraction = static_cast<float>(update["percent"].as_int(0)) / 100.0f;
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pal().accent);
      ImGui::ProgressBar(fraction, ImVec2(content, px(6)), "");
      ImGui::PopStyleColor();
      ImGui::Dummy(ImVec2(0, px(8)));
      centre(px(96));
      if (button("Later##later", Variant::normal, nullptr, false, 96, 30, focus_ring)) {
        env.panel.update_offered = env.panel.update_latest;
        env.shell.save_layout();
        ImGui::CloseCurrentPopup();
      }
    } else if (failed) {
      centre(text_width(fonts().strong, "UPDATE DID NOT FINISH"));
      text(fonts().strong, pal().accent, "UPDATE DID NOT FINISH");
      ImGui::Dummy(ImVec2(0, px(4)));
      const std::string why = update["error"].as_string();
      text_wrapped(fonts().body, pal().muted, why.empty() ? "Try again in a moment." : why, content);
      ImGui::Dummy(ImVec2(0, px(8)));
      const float retry_width = unpx((content - px(8)) / 2);
      if (button("Try again##retry", Variant::primary, "refresh", false, retry_width, 30, focus_ring)) {
        env.shell.check_update(true);
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine(0, px(8));
      if (button("Release page##rel", Variant::normal, "external", false, retry_width, 30, focus_ring)) {
        Json args = Json::object();
        args.set("url", Json(kReleasesUrl));
        env.store.attempt("app.open_url", args, "The page did not open");
        ImGui::CloseCurrentPopup();
      }
    } else {
      centre(text_width(fonts().strong, "UPDATE AVAILABLE"));
      text(fonts().strong, pal().accent, "UPDATE AVAILABLE");
      ImGui::Dummy(ImVec2(0, px(4)));
      centre(text_width(fonts().strong, running) + px(8) + px(14) + px(8) + text_width(fonts().strong, to));
      text(fonts().strong, pal().muted, running);
      ImGui::SameLine(0, px(8));
      const ImVec2 arrow = ImGui::GetCursorScreenPos();
      draw_icon(ImGui::GetWindowDrawList(), "arrowRight", ImVec2(arrow.x, arrow.y + px(3)), px(14), pal().accent);
      ImGui::Dummy(ImVec2(px(14), px(14)));
      ImGui::SameLine(0, px(8));
      text(fonts().strong, pal().ink, to);
      ImGui::Dummy(ImVec2(0, px(8)));
      if (button("Update##now", Variant::primary, "external", false, action_width, 30, focus_ring)) {
        Json args = Json::object();
        args.set("url", Json(kReleasesUrl));
        env.store.attempt("app.open_url", args, "The page did not open");
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine(0, px(8));
      if (button("Later##later", Variant::normal, nullptr, false, 96, 30, focus_ring)) {
        env.panel.update_offered = env.panel.update_latest;
        env.shell.save_layout();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  return true;
}

void apply_profile(ViewEnv& env, int index) {
  Json args = Json::object();
  args.set("index", Json(index));
  env.store.attempt("profiles.apply", args, "Profile not applied", [&store = env.store](const Json&) {
    store.refresh_profiles();
    store.refresh_config();
  });
}

namespace {

// A bar module: a small framed box on the ground with an optional square dot, a bold value and muted words.
struct Module {
  ImU32 dot = 0;
  std::string strong, rest;
};

float module_width(const Module& m) {
  float w = px(18);
  if (m.dot) w += px(13);
  if (!m.strong.empty()) w += text_width(fonts().strong, m.strong);
  if (!m.strong.empty() && !m.rest.empty()) w += px(6);
  if (!m.rest.empty()) w += text_width(fonts().small, m.rest);
  return w;
}

void draw_module(ImDrawList* draw, ImVec2 min, float height, const Module& m) {
  const Palette& p = pal();
  const float w = module_width(m);
  draw->AddRectFilled(min, ImVec2(min.x + w, min.y + height), p.surface);
  draw->AddRect(min, ImVec2(min.x + w, min.y + height), p.line);
  float x = min.x + px(9);
  const float cy = min.y + height / 2;
  if (m.dot) {
    draw->AddRectFilled(ImVec2(x, cy - px(3.5f)), ImVec2(x + px(7), cy + px(3.5f)), m.dot);
    x += px(13);
  }
  if (!m.strong.empty()) {
    draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(x, cy - fonts().strong->LegacySize / 2), p.ink, m.strong.c_str());
    x += text_width(fonts().strong, m.strong) + px(6);
  }
  if (!m.rest.empty()) {
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(x, cy - fonts().small->LegacySize / 2), p.muted, m.rest.c_str());
  }
}

}  // namespace

void titlebar(ViewEnv& env, const std::string& title, const std::function<void()>& buttons) {
  // A status bar, the way a tiling desktop's bar is built: the logo's place, the four profiles as
  // workspaces, the profile's module, then the window's own buttons at the right. It lies on the
  // ground, not on a pane.
  const Palette& p = pal();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x, height = px(36);
  draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), p.canvas);
  draw->AddLine(ImVec2(origin.x, origin.y + height - 1), ImVec2(origin.x + width, origin.y + height - 1), p.line_strong);
  env.shell.drag_area(origin, ImVec2(origin.x + width, origin.y + height));
  const float box = px(22), top = origin.y + (height - box) / 2;
  draw_app_icon(draw, ImVec2(origin.x + px(19), origin.y + height / 2), px(15));
  float x = origin.x + px(42);

  // The profiles as workspaces: the applied one filled; a click applies one, as the deck's chips do.
  const int active = static_cast<int>(env.store.config["active_profile"].as_int(-1));
  for (int index = 0; index < 4; ++index) {
    const UiProfile* profile = index < static_cast<int>(env.store.profiles.size()) ? &env.store.profiles[index] : nullptr;
    const ImVec2 min(x + index * box, top), max(min.x + box, top + box);
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID(index);
    ImGui::BeginDisabled(!profile);
    const bool pressed = ImGui::InvisibleButton("##workspace", ImVec2(box, box));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    ImGui::PopID();
    env.shell.no_drag(min, max);
    const bool on = index == active;
    if (on) draw->AddRectFilled(min, max, p.accent);
    else if (hovered) draw->AddRectFilled(min, max, p.raised);
    // B3: applying a profile pulses its chip once (a flash of the ink that settles on the base token; the appear slot restarts each
    // time the chip stops being the applied one).
    if (on) {
      ImGui::PushID(index);
      const float settled = appear(ImGui::GetID("##pulse"), kDurBase);
      ImGui::PopID();
      if (settled < 1.0f) draw->AddRectFilled(min, max, with_alpha(p.ink, 0.4f * (1.0f - settled)));
    }
    const std::string digit = std::to_string(index + 1);
    const float digit_size = px(15);  // the same glyph size as the view buttons beside the window keys
    const float tw = text_width(fonts().strong, digit);
    draw->AddText(fonts().strong, digit_size, ImVec2(min.x + (box - tw) / 2, min.y + (box - digit_size) / 2),
                  on ? p.on_accent : (profile ? (hovered ? p.ink : p.muted) : with_alpha(p.muted, 0.4f)), digit.c_str());
    if (profile) tooltip("Profile " + digit + " \xC2\xB7 " + format_span(profile->buffer_seconds) + " \xC2\xB7 " + std::to_string(profile->capture_fps) + " fps");
    if (pressed && profile && !on) apply_profile(env, index);
  }
  draw->AddRect(ImVec2(x, top), ImVec2(x + box * 4, top + box), p.line);
  x += box * 4 + px(6);
  // Settings names itself; numbered profiles do not need a second label.
  Module where;
  where.strong = title;
  if (!where.strong.empty()) {
    draw_module(draw, ImVec2(x, top), box, where);
  }

  // The middle of the bar is the capture's own line, held by a slash each side: while the buffer runs, what is captured
  // and at what size and rate; while it is off, a Start button. Parts drop out, the name first, when the room is short.
  if (title.empty()) {
    const Json& capture = env.store.status["capture"];
    // While Performance mode runs, the bar says only that: the picture it encodes is not the one the capture source
    // reports, and the numbers behind it are not the person's own settings.
    const bool perf = env.store.status["performance"]["active"].as_bool(false);
    const std::string state = !env.store.connected ? "disconnected" : env.store.status["buffer_state"].as_string("off");
    const bool on = state == "on" && capture["width"].as_int() > 0;
    ImFont* small = fonts().small;
    const float size = small->LegacySize, pad = px(12), tail = px(18);
    const float y = origin.y + (height - size) / 2;
    const float extra = ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##bar_extra_width"), 0);
    const float region_l = x + px(8) + tail, region_r = origin.x + width - px(6) - 2 * (px(28) + px(2)) - px(13) - extra - px(10) - tail;
    const float room = region_r - region_l;
    float total = 0.0f;
    std::string target, resolution, rate;
    const std::string perf_label = "performance mode on";
    bool with_name = false, with_rate = false;
    if (on && perf) {
      total = text_width(fonts().strong, perf_label);
    } else if (on) {
      target = capture["target"].as_string();
      resolution = std::to_string(capture["width"].as_int()) + " x " + std::to_string(capture["height"].as_int());
      rate = std::to_string(capture["fps"].as_int()) + " FPS";
      const float res_w = text_width(small, resolution), fps_w = text_width(small, rate), name_w = target.empty() ? 0.0f : text_width(fonts().strong, target);
      with_rate = res_w + pad * 2 + 1 + fps_w <= room;
      with_name = !target.empty() && name_w + pad * 2 + 1 + res_w + (with_rate ? pad * 2 + 1 + fps_w : 0.0f) <= room;
      total = (with_name ? name_w + pad * 2 + 1 : 0.0f) + res_w + (with_rate ? pad * 2 + 1 + fps_w : 0.0f);
    } else {
      total = text_width(small, state == "off" ? "buffer off" : state == "starting" ? "starting" : "connecting");
    }
    if (total > 0.0f && total <= room) {
      float left = std::floor(origin.x + (width - total) / 2);
      left = std::clamp(left, region_l, region_r - total);
      auto divider = [&](float at) {
        draw->AddLine(ImVec2(at, origin.y + px(11)), ImVec2(at, origin.y + height - px(11)), p.line_strong);
      };
      float cursor = left;
      if (on && perf) {
        draw->AddText(fonts().strong, fonts().strong->LegacySize,
                      ImVec2(cursor, origin.y + (height - fonts().strong->LegacySize) / 2), p.accent, perf_label.c_str());
      } else if (on) {
        if (with_name) {
          draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(cursor, origin.y + (height - fonts().strong->LegacySize) / 2), p.ink, target.c_str());
          cursor += text_width(fonts().strong, target) + pad;
          divider(cursor);
          cursor += pad + 1;
        }
        draw->AddText(small, size, ImVec2(cursor, y), p.ink, resolution.c_str());
        cursor += text_width(small, resolution);
        if (with_rate) {
          cursor += pad;
          divider(cursor);
          draw->AddText(small, size, ImVec2(cursor + pad + 1, y), p.muted, rate.c_str());
        }
      } else if (state == "off") {
        draw->AddText(small, size, ImVec2(cursor, y), p.muted, "buffer off");
      } else {
        draw->AddText(small, size, ImVec2(cursor, y), p.muted, state == "starting" ? "starting" : "connecting");
      }
      for (const float edge : {left - tail, left + total + tail})
        draw->AddLine(ImVec2(edge - px(5), origin.y + height - px(10)), ImVec2(edge + px(5), origin.y + px(10)), p.accent, px(2));
    }
  }

  // The buttons sit at the right: measure by drawing them right-to-left into a group placed there.
  const float button = px(28), gap = px(2);
  const int fixed = 2;
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID extra_id = ImGui::GetID("##bar_extra_width");
  const float extra = storage->GetFloat(extra_id, 0);
  const float right = origin.x + width - px(6);
  const float start = right - fixed * (button + gap) - px(13) - extra;
  ImGui::SetCursorScreenPos(ImVec2(start, origin.y + (height - button) / 2));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
  ImGui::BeginGroup();
  const float group_x = ImGui::GetCursorScreenPos().x;
  if (buttons) buttons();
  const float measured = ImGui::GetItemRectMax().x - group_x;
  if (buttons) storage->SetFloat(extra_id, std::max(0.0f, measured + gap));
  if (buttons) ImGui::SameLine();
  const ImVec2 separator = ImGui::GetCursorScreenPos();
  draw->AddLine(ImVec2(separator.x + px(6), origin.y + px(10)), ImVec2(separator.x + px(6), origin.y + height - px(10)), p.line);
  ImGui::Dummy(ImVec2(px(13), button));
  ImGui::SameLine();
  if (bar_button(env, "##min", "minus", "Minimise")) env.shell.minimise();
  ImGui::SameLine();
  if (icon_button("##close", "close", "Close", false, 28, 15, true)) env.shell.close_window();
  env.shell.no_drag(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  ImGui::EndGroup();
  ImGui::PopStyleVar();
  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
}

ImU32 tone_color(Tone tone) {
  switch (tone) {
    case Tone::ok: return pal().ok;
    case Tone::bad: return pal().danger;
    case Tone::busy: return pal().warn;
    default: return pal().faint;
  }
}

// A sheet or a menu that is closed keeps its place for the exit token and fades out first (V7, C9). `close_sheet` replaces
// ImGui::CloseCurrentPopup in them: it only marks the popup, and the owner (begin_dialog, or a menu through sheet_leaving) closes it when the
// fade is over, so a caller keeps drawing its content for those 90 ms and nothing it pressed can be pressed twice (the content is disabled).
namespace {
std::unordered_map<ImGuiID, double>& sheet_closings() {
  static std::unordered_map<ImGuiID, double> closings;
  return closings;
}
ImGuiID current_popup_id() {
  ImGuiWindow* window = ImGui::GetCurrentWindowRead();
  while (window && !(window->Flags & ImGuiWindowFlags_Popup)) window = window->ParentWindow;
  return window ? window->PopupId : 0;
}
}  // namespace

void close_sheet() {
  if (!anim_enabled()) {
    ImGui::CloseCurrentPopup();
    return;
  }
  const ImGuiID id = current_popup_id();
  if (id == 0) {
    ImGui::CloseCurrentPopup();
    return;
  }
  sheet_closings().emplace(id, anim_detail::wall_seconds());
  anim_touch();
}

// 0 while the popup stays, up to 1 as it leaves; the popup is closed at 1. Call inside the popup.
float sheet_leaving_of(ImGuiID popup) {
  const auto it = sheet_closings().find(popup);
  if (it == sheet_closings().end()) return 0.0f;
  const float t = progress(anim_detail::wall_seconds() - it->second, kDurFast);
  anim_touch();
  if (t >= 1.0f) {
    sheet_closings().erase(it);
    ImGui::CloseCurrentPopup();
  }
  return std::max(t, 0.001f);
}
float sheet_leaving() { return sheet_leaving_of(current_popup_id()); }

// ---------------------------------------------------------------- media

void thumbnail(ViewEnv& env, const UiMedia& item, ImVec2 min, ImVec2 size, float rounding, float zoom) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 max(min.x + size.x, min.y + size.y);
  draw->AddRectFilled(min, max, pal().canvas, rounding);
  if (!ImGui::IsRectVisible(min, max)) return;  // only what is on screen is ever decoded
  // The listing no longer decodes the whole library (library_annotate takes a budget), so a card that
  // has been scrolled to asks for its own. One command, one clip, off the tick - which is the point:
  // the cost lands on the card being looked at, not on the list that happens to contain it.
  if (item.thumbnail.empty() && item.clip) {
    const std::string file = item.file;
    env.store.attempt_once("clips.thumbnail:" + file, [file] {
      Json args = Json::object();
      args.set("file", Json(file));
      return args;
    }, [&store = env.store, &panel = env.panel, file](const Json& data) {
      // The reply carries the thumbnail for this one clip; it goes into the item so the card stops
      // asking and the next frame has something to draw.
      //
      // The store, never `env`: the reply runs frames later, and the ViewEnv is the frame's own stack
      // object. Capturing it read a dead frame's memory and took the engine down with an access
      // violation whenever a thumbnail came back - the acceptance harness caught it.
      const std::string thumbnail = data["thumbnail"].as_string();
      if (thumbnail.empty()) return;
      // The share sheet keeps its own copy of the clip (it may not be in the library list at all).
      if (panel.sharing.file == file) panel.sharing.thumbnail = thumbnail;
      for (UiMedia& clip : store.clips) {
        if (clip.file == file) {
          clip.thumbnail = thumbnail;
          ++store.library_version;  // the rail's Shared tab keeps a copy: its card takes the picture too
          break;
        }
      }
    });
  }
  // An engine thumbnail; a screenshot without one shows the image itself.
  const std::string source = !item.thumbnail.empty() ? item.thumbnail : (item.clip ? std::string() : item.file);
  picture(env, source, item.clip ? "film" : "image", min, size, rounding, zoom);
}

void picture(ViewEnv& env, const std::string& path, const char* icon, ImVec2 min, ImVec2 size, float rounding, float zoom) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 max(min.x + size.x, min.y + size.y);
  draw->AddRectFilled(min, max, pal().canvas, rounding);
  ImVec2 image_size;
  bool failed = false;
  const ImTextureID texture = env.images.get(path, 480, &image_size, &failed);
  if (texture && image_size.x > 0 && image_size.y > 0) {
    // Cover the box, cropping the overflow (object-fit: cover).
    const float box_ratio = size.x / size.y, image_ratio = image_size.x / image_size.y;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (image_ratio > box_ratio) {
      const float keep = box_ratio / image_ratio;
      uv0.x = (1 - keep) / 2;
      uv1.x = 1 - uv0.x;
    } else {
      const float keep = image_ratio / box_ratio;
      uv0.y = (1 - keep) / 2;
      uv1.y = 1 - uv0.y;
    }
    // The zoom shows the centre 1/zoom of the cropped picture: the same texture, a tighter window (no new texture).
    const float cut = (1.0f - 1.0f / std::max(1.0f, zoom)) / 2.0f;
    const ImVec2 span(uv1.x - uv0.x, uv1.y - uv0.y);
    uv0 = ImVec2(uv0.x + span.x * cut, uv0.y + span.y * cut);
    uv1 = ImVec2(uv1.x - span.x * cut, uv1.y - span.y * cut);
    // A picture eases in from its surface the first frame it is drawn (C5): a texture that arrives no longer pops in.
    // No fade-in: a card scrolled into view shows its picture at once (a fade per card read as a flicker while the list scrolls).
    const ImU32 tint = IM_COL32_WHITE;
    draw->AddImageRounded(texture, min, max, uv0, uv1, tint, rounding);
  } else {
    const float glyph = std::min(px(26), size.y * 0.5f);
    draw_icon(draw, icon, ImVec2(min.x + (size.x - glyph) / 2, min.y + (size.y - glyph) / 2), glyph, pal().line_strong);
  }
}

const UiMedia* find_media(const ViewEnv& env, const std::string& file) {
  for (const UiMedia& item : env.store.clips) {
    if (item.file == file) return &item;
  }
  for (const UiMedia& item : env.store.screenshots) {
    if (item.file == file) return &item;
  }
  return nullptr;
}

std::vector<LinkView> link_views(const Json& state) {
  std::vector<LinkView> links;
  const Json& list = state["links"];
  if (!list.is_array()) return links;
  for (const Json& item : list.items()) {
    LinkView link;
    link.id = item["id"].as_string();
    link.file = item["file"].as_string();
    link.name = item["name"].as_string();
    link.url = item["url"].as_string();
    link.expires = item["expires"].as_int();
    link.created = item["created"].as_int(0);
    link.viewers = static_cast<int>(item["viewers"].as_int());
    link.watching = static_cast<int>(item["watching"].as_int());
    if (!link.id.empty()) links.push_back(std::move(link));
  }
  return links;
}

void copy_link(UiStore& store, const std::string& url) {
  store.copied_link = url;  // set first: the clipboard watcher must not offer this PC its own link
  if (url.empty() || !copy_text_to_clipboard(url)) {
    store.toast(UiToast::error, "Link not copied", "The clipboard could not be set");
    return;
  }
  const bool page_set = store.links["page_set"].as_bool();
  store.toast(UiToast::success, "Link copied",
              page_set ? "Paste it anywhere - it opens in your friend's BuffrX"
                       : "Your friend copies it and opens BuffrX, which offers to open the clip");
}

void link_stop_button(ViewEnv& env, const char* id, const LinkView& link, float size, float icon_size, bool framed) {
  const bool armed = link_stop_armed(env.panel.link_stop, link.id, env.now_ms);
  const float side = px(size);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  if (framed) {
    ImGui::GetWindowDrawList()->AddRect(pos, ImVec2(pos.x + side, pos.y + side), armed ? pal().danger : pal().line, 0, 0,
                                        px(1));
  }
  if (!icon_button(id, "stop", armed ? "Press again to stop the link" : "Stop link", armed, size, icon_size, true, armed)) {
    return;
  }
  if (!link_stop_confirm(env.panel.link_stop, link.id, env.now_ms)) return;
  Json args = Json::object();
  args.set("id", link.id);
  env.store.attempt("link.stop", args, "The link did not stop");
}

void open_preview(ViewEnv& env, const UiMedia& item, const std::vector<UiMedia>& list) {
  // A clip and a screenshot open the same way: in the panel's own preview, the picture where the library was.
  env.panel.preview_files.clear();
  int index = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    env.panel.preview_files.push_back(list[i].file);
    if (list[i].file == item.file) index = static_cast<int>(i);
  }
  env.panel.preview_index = index;
  env.panel.preview_zoom = false;
  env.panel.preview_from_compact = false;
  env.panel.preview_open = true;
}

void run_media_action(ViewEnv& env, const std::string& action, const UiMedia& item, const std::vector<UiMedia>& list) {
  Json args = Json::object();
  args.set("file", Json(item.file));
  if (action == "preview") {
    open_preview(env, item, list);
  } else if (action == "edit") {
    open_editor(env, {item.file});
  } else if (action == "copy") {
    Json files = Json::array();
    files.push(Json(item.file));
    Json copy = Json::object();
    copy.set("files", files);
    const bool clip = item.clip;
    env.store.attempt("clips.copy", copy, "Not copied", [&store = env.store, clip](const Json&) {
      store.toast(UiToast::success, clip ? "Clip copied" : "Image copied", "Paste it into Discord, a chat or a folder");
    });
  } else if (action == "share") {
    open_share(env, item);
  } else if (action == "reveal") {
    env.store.attempt(item.clip ? "clips.reveal" : "screenshots.reveal", args, "The folder did not open");
  } else if (action == "open") {
    env.store.attempt("clips.open", args, "The file did not open");
  } else if (action == "rename") {
    env.panel.renaming = item;
    env.panel.rename_text = file_stem(item.file);
    env.panel.rename_problem_text.clear();
    env.panel.rename_open = true;
  } else if (action == "delete") {
    env.panel.removing = item;
    env.panel.remove_open = true;
  }
}

void media_menu(ViewEnv& env, const char* popup_id, const UiMedia& item, const std::vector<UiMedia>& list) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(4), px(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4), px(2)));
  if (ImGui::BeginPopup(popup_id)) {
    const float leaving = sheet_leaving();  // C9: a chosen entry closes the menu on the exit token (the popup closes at 1)
    const float pop_t = appear(ImGui::GetID("##pop"), kDurBase) * (1.0f - ease_in(leaving));  // the menu fades in under the pointer
    ImGui::BeginDisabled(leaving > 0.0f);
    const int pop_v = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    struct Entry {
      const char* id;
      const char* label;
      const char* icon;
      bool danger;
      bool lead;  // the action a clip is made for: lit at rest, so it reads without a hover
    };
    const Entry entries[] = {
        {"share", "Share\xE2\x80\xA6", "share", false, true},
        {"edit", item.clip ? "Trim in editor" : "Edit in editor", "scissors", false, false},
        {"copy", item.clip ? "Copy clip" : "Copy image", "copy", false, false},
        {"reveal", "Show in folder", "folder", false, false},
        {"open", "Open file", "external", false, false},
        {"rename", "Rename", "edit", false, false},
        {"delete", "Delete", "trash", true, false},
    };
    for (const Entry& entry : entries) {
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      const float w = px(150), h = px(23);
      if (ImGui::InvisibleButton(entry.id, ImVec2(w, h))) {
        run_media_action(env, entry.id, item, list);
        close_sheet();
      }
      const bool hovered = ImGui::IsItemHovered();
      const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
      ImDrawList* draw = ImGui::GetWindowDrawList();
      if (hover > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), with_alpha(pal().raised, hover));
      // The lead row: an accent bar on its left edge and a seam under it, in the gap the rows leave between them.
      if (entry.lead) {
        draw->AddRectFilled(pos, ImVec2(pos.x + px(2), pos.y + h), pal().accent);
        draw->AddLine(ImVec2(pos.x, pos.y + h + px(1)), ImVec2(pos.x + w, pos.y + h + px(1)), pal().line);
      }
      const ImU32 color = entry.danger ? pal().danger : entry.lead ? pal().accent : pal().ink;
      const ImU32 glyph = entry.danger ? pal().danger : entry.lead ? pal().accent : pal().muted;
      draw_icon(draw, entry.icon, ImVec2(pos.x + px(8), pos.y + (h - px(13)) / 2), px(13), glyph);
      draw->AddText(fonts().body, fonts().body->LegacySize, ImVec2(pos.x + px(28), pos.y + (h - fonts().body->LegacySize) / 2), color,
                    entry.label);
    }
    ImGui::EndDisabled();
    shift_since(ImGui::GetWindowDrawList(), pop_v, 0.0f, px(6) * (1.0f - pop_t));  // C9: the menu rises 6 px as it fades in
    fade_since(ImGui::GetWindowDrawList(), pop_v, pop_t);
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
}

// A Timeline card's menu. Its entries are what makes sense for footage nobody saved: it still plays, a clip can be cut
// out of it, and it can be erased - which cannot be undone, so the same entry asks a second time instead of a dialog.
void session_menu(ViewEnv& env, const char* popup_id, bool armed, const std::function<void(const char* action)>& run) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(4), px(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4), px(2)));
  if (ImGui::BeginPopup(popup_id)) {
    const float leaving = sheet_leaving();  // C9: a chosen entry closes the menu on the exit token (the popup closes at 1)
    const float pop_t = appear(ImGui::GetID("##pop"), kDurBase) * (1.0f - ease_in(leaving));  // the menu fades in under the pointer
    ImGui::BeginDisabled(leaving > 0.0f);
    const int pop_v = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    struct Entry {
      const char* id;
      const char* label;
      const char* icon;
      bool danger;
    };
    const Entry entries[] = {
        {"preview", "Preview", "play", false},
        {"trim", "Trim in editor", "scissors", false},
        {armed ? "erase" : "arm", armed ? "Erase now - it cannot be undone" : "Erase the footage\xE2\x80\xA6", "trash", true},
        {"folder", "Show in folder", "folder", false},
    };
    for (const Entry& entry : entries) {
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      const float w = px(armed ? 215 : 150), h = px(23);
      if (ImGui::InvisibleButton(entry.id, ImVec2(w, h))) {
        run(entry.id);
        // Arming keeps the menu open, so the same row reads "Erase now" on the next frame. Closing on the first
        // press looked like nothing had happened.
        if (std::string(entry.id) != "arm") close_sheet();
      }
      const bool hovered = ImGui::IsItemHovered();
      const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
      ImDrawList* draw = ImGui::GetWindowDrawList();
      if (hover > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), with_alpha(pal().raised, hover));
      draw_icon(draw, entry.icon, ImVec2(pos.x + px(8), pos.y + (h - px(13)) / 2), px(13),
                entry.danger ? pal().danger : pal().muted);
      draw->AddText(fonts().body, fonts().body->LegacySize, ImVec2(pos.x + px(28), pos.y + (h - fonts().body->LegacySize) / 2),
                    entry.danger ? pal().danger : pal().ink, entry.label);
    }
    ImGui::EndDisabled();
    shift_since(ImGui::GetWindowDrawList(), pop_v, 0.0f, px(6) * (1.0f - pop_t));  // C9: the menu rises 6 px as it fades in
    fade_since(ImGui::GetWindowDrawList(), pop_v, pop_t);
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------- dialogs
namespace {

std::vector<std::string>& dialog_titles() {
  static std::vector<std::string> titles;
  return titles;
}

// The most a sheet may take of the work area, in design units. The title sits half outside the top
// border and a capped sheet keeps a margin at both ends, so its buttons never leave the screen.
float dialog_cap() { return std::max(180.0f, unpx(ImGui::GetIO().DisplaySize.y) - 48.0f); }

// The keyboard's place in a dialog is ImGui's own navigation: Tab moves it and Enter presses the button
// it is on, once. The ring is drawn from the item's focus, in the palette's accent.
float& dialog_arrival() {  // how far the open dialog has come in, 0..1 (one open at a time)
  static float t = 1.0f;
  return t;
}

bool& dialog_leaving() {  // the open dialog is on its exit fade and its content is disabled
  static bool on = false;
  return on;
}

bool& dialog_scroll_open() {
  static bool open = false;
  return open;
}

// A dialog's button: `button`, plus the ring and Enter.
bool dialog_button(const char* label, Variant variant = Variant::normal, const char* icon = nullptr, bool disabled = false,
                   float width = 0, float height = 32) {
  const bool pressed = button(label, variant, icon, disabled, width, height);
  if (!disabled && ImGui::IsItemFocused()) {
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - px(2), min.y - px(2)), ImVec2(max.x + px(2), max.y + px(2)), pal().accent, 0, 0,
                                        px(2));
  }
  return pressed;
}

// A centred modal with a title and the caller's body; true while it is open. `height` (design units) is
// the height a sheet asks for when its body can grow: the body then scrolls and the buttons stay at the
// foot. Nothing is ever taller than the work area.
bool begin_dialog(const char* id, const std::string& title, bool& open, float width = 420, float height = 0, float footer = 46,
                  float fixed_height = -1) {
  // V7: a sheet that was asked to close (close_sheet) stays open on the exit token: its owner still sees it open, it fades and sinks, and
  // it is closed when the fade is over.
  const ImGuiID popup_id = ImGui::GetID(id);
  if (!ImGui::IsPopupOpen(id)) sheet_closings().erase(popup_id);  // a mark left by a close that had already finished
  const bool leaving_now = sheet_closings().count(popup_id) != 0 && ImGui::IsPopupOpen(id);
  if (leaving_now) open = true;
  if (open && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  // A sheet rises 8 px into place as it fades in (V7).
  float arrived = ImGui::IsPopupOpen(id) ? appear(ImGui::GetID(id), kDurBase) : 1.0f;
  if (leaving_now) {
    const auto it = sheet_closings().find(popup_id);
    arrived = std::min(arrived, 1.0f - ease_in(progress(anim_detail::wall_seconds() - it->second, kDurFast)));
  }
  dialog_arrival() = arrived;
  const float cap = px(dialog_cap());
  // A 780-unit sheet is wider than a 1366-pixel work area at 200%: it takes the window's width instead.
  const float dialog_w = std::min(px(width), display.x - px(24));
  ImGui::SetNextWindowPos(ImVec2(display.x / 2, display.y / 2 + (1.0f - arrived) * px(8)), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(dialog_w, 0), ImVec2(dialog_w, cap));
  // `fixed_height` is measured on screen by its caller, so it is already in pixels: scaling it again made a folded
  // sheet taller than its content by the display scale.
  ImGui::SetNextWindowSize(ImVec2(dialog_w, fixed_height > 0 ? std::min(fixed_height, cap) : height > 0 ? std::min(px(height), cap) : 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(18)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, pal().surface);
  const bool shown = ImGui::BeginPopupModal(id, nullptr,
                                            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (shown) {
    dialog_leaving() = leaving_now;
    if (leaving_now) {
      ImGui::BeginDisabled(true);
      if (sheet_leaving_of(popup_id) >= 1.0f) open = false;  // done: the popup was closed inside, and the owner may stop drawing it
    }
    dialog_titles().push_back(lower_ascii(title));
    ImGui::Dummy(ImVec2(0, px(2)));
    if (height > 0) {
      // The body scrolls inside the sheet (wheel included); the button row is drawn outside it, at the foot.
      ImGui::BeginChild("##dialogbody", ImVec2(0, -px(footer)), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
      // J2: a sheet's body glides on the wheel like the library (only while the pointer is over it).
      smooth_scroll(px(96));
      dialog_scroll_open() = true;
    }
  }
  return shown;
}

// A dialog is the focused pane: a 2 px accent frame with its title set into the top border. Drawn on the dialog's own
// list, with a clip that reaches above the window for the title, so a popup opened from the dialog covers it.
void end_dialog() {
  if (dialog_scroll_open()) {  // a sheet that returned before its button row, or one with no row
    ImGui::EndChild();
    dialog_scroll_open() = false;
  }
  if (dialog_leaving()) {
    ImGui::EndDisabled();
    dialog_leaving() = false;
  }
  const ImVec2 min = ImGui::GetWindowPos();
  const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
  const std::string title = dialog_titles().empty() ? std::string() : dialog_titles().back();
  if (!dialog_titles().empty()) dialog_titles().pop_back();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float border = px(2);
  draw->PushClipRect(ImVec2(min.x - px(8), min.y - fonts().strong->LegacySize - px(6)),
                     ImVec2(max.x + px(8), max.y + px(8)), false);
  draw->AddRect(ImVec2(min.x + border / 2, min.y + border / 2), ImVec2(max.x - border / 2, max.y - border / 2), pal().accent, 0, 0, border);
  if (!title.empty()) {
    ImFont* font = fonts().strong;
    const float x = min.x + px(16), w = text_width(font, title);
    const bool share = title == "share clip" || title == "share image";
    const float mark = share ? text_width(font, " //") : 0;
    draw->AddRectFilled(ImVec2(x - px(6), min.y - font->LegacySize / 2), ImVec2(x + w + mark + px(6), min.y + border + 1), pal().surface);
    const float y = std::floor(min.y - font->LegacySize / 2 - px(1));
    draw->AddText(font, font->LegacySize, ImVec2(x, y), pal().accent, title.c_str());
    if (share) draw->AddText(font, font->LegacySize, ImVec2(x + w, y), pal().accent, " //");
  }
  draw->PopClipRect();
  fade_in_from(ImGui::GetForegroundDrawList(), min, max, pal().surface, dialog_arrival());  // the sheet's body and frame come in from its own ground
  ImGui::EndPopup();
}

// Buttons right-aligned at the foot of a dialog.
void align_right(float width) {
  if (dialog_scroll_open()) {  // a scrolling body ends here: the button row is never inside it
    ImGui::EndChild();
    dialog_scroll_open() = false;
  }
  ImGui::Dummy(ImVec2(0, px(6)));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
}

}  // namespace

void hotkey_recorder(ViewEnv& env) {
  PanelState& panel = env.panel;
  // The input the modal listens to can be swapped while it is open: the keyboard's own capture and the
  // pad's report are each turned on by one, and off by the other, so a row can be tried either way.
  auto begin_input = [&](bool controller) {
    panel.recorded_chord.clear();
    panel.recorded_problem.clear();
    panel.recorder_preview.clear();
    env.store.pad_capture.clear();
    if (controller) {
      Json args = Json::object();
      args.set("on", Json(true));
      env.store.attempt("hotkeys.capture", args, "Controller buttons stayed on their actions");
    } else {
      env.shell.capture_keys(true);
    }
  };
  auto end_input = [&](bool controller) {
    env.shell.capture_keys(false);
    env.store.pad_capture.clear();
    if (controller) {
      Json args = Json::object();
      args.set("on", Json(false));
      env.store.attempt("hotkeys.capture", args, "Controller buttons did not go back to their actions");
    }
  };
  bool open = !panel.recording.empty();
  if (open && !panel.recorder_open) {
    // The engine lets go of every hotkey while one is recorded, so pressing the old chord to rebind it
    // does not also save a clip. A pad binding is recorded the same way: hotkeys.capture makes the pads
    // report their buttons instead of running their action.
    begin_input(panel.recorder_controller);
    Json args = Json::object();
    args.set("suspended", Json(true));
    env.store.attempt("hotkeys.suspend", args, "Hotkeys stayed on while recording");
  }
  panel.recorder_open = open;
  if (!open) return;
  auto close = [&] {
    panel.recording.clear();
    panel.recorder_preview.clear();
    end_input(panel.recorder_controller);
    Json args = Json::object();
    args.set("suspended", Json(false));
    env.store.attempt("hotkeys.suspend", args, "Hotkeys did not come back on");
    panel.recorder_open = false;
    close_sheet();
  };
  const std::string label = hotkey_label(panel.recording);
  const bool controller = panel.recorder_controller;
  const std::string title = controller ? "Set controller hotkey: " + label : "Set hotkey: " + label;
  if (begin_dialog("##recorder", title, open)) {
    if (controller) {
      const std::string live = env.store.pad_capture;
      if (!live.empty() && live != panel.recorded_chord) {
        panel.recorded_chord = live;
        panel.recorded_problem.clear();
      }
    } else {
      bool ctrl, alt, shift, win, down;
      unsigned key;
      while (!panel.recorder_saving && env.shell.take_key(ctrl, alt, shift, win, key, down)) {
        if (!down) {
          // A modifier let go before any key arrived: the box drops it again.
          if (panel.recorded_chord.empty()) panel.recorder_preview = chord_preview(ctrl, alt, shift, win);
          continue;
        }
        if (key == VK_ESCAPE && !ctrl && !alt) {
          close();
          end_dialog();
          return;
        }
        const ChordCapture capture = capture_chord(ctrl, alt, shift, win, key);
        if (capture.waiting) {
          panel.recorder_preview = chord_preview(ctrl, alt, shift, win);
          continue;
        }
        panel.recorded_chord = capture.chord;
        panel.recorded_problem = capture.problem;
        panel.recorder_preview.clear();
      }
    }
    // Which input this row is being set for. Swapping mid-recording clears the half-typed chord and
    // hands the pads and the keyboard back and forth, so the value saved is always the one on screen.
    int chosen = panel.recorder_controller ? 1 : 0;
    if (segmented("##input", {"Keyboard", "Controller"}, chosen, px(104), 24, fonts().small)) {
      const bool next = chosen == 1;
      if (next != panel.recorder_controller) {
        end_input(panel.recorder_controller);
        panel.recorder_controller = next;
        begin_input(next);
      }
    }
    ImGui::Dummy(ImVec2(0, px(8)));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = px(80);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), pal().canvas);
    draw->AddRect(pos, ImVec2(pos.x + w, pos.y + h), pal().line_strong);
    // The modifiers held now, before a key completes the chord: the box answers at once instead of
    // standing still until a whole combination arrives.
    const std::string value = panel.recorded_chord.empty() && !controller ? panel.recorder_preview : panel.recorded_chord;
    if (value.empty()) {
      const std::string prompt = controller ? "Press controller buttons" : "Press the keys for " + label;
      const float tw = text_width(fonts().body, prompt);
      draw->AddText(fonts().body, fonts().body->LegacySize, ImVec2(pos.x + (w - tw) / 2, pos.y + (h - fonts().body->LegacySize) / 2),
                    pal().faint, prompt.c_str());
      ImGui::Dummy(ImVec2(w, h));
    } else {
      ImGui::Dummy(ImVec2(w, h));
      const ImVec2 after = ImGui::GetCursorScreenPos();
      // The box holds the four buttons a chord may take. More are still recorded (the save refuses
      // them and says why), but drawing them all ran them out of the box.
      std::string shown = controller && value.size() > 4 ? value.substr(4) : value;
      const std::vector<std::string> parts = keycaps(shown);
      if (controller && parts.size() > kPadMaxChordButtons) {
        shown.clear();
        for (size_t i = 0; i < kPadMaxChordButtons; ++i) shown += (i == 0 ? "" : "+") + parts[i];
      }
      float caps_w = 0;
      for (const std::string& cap : keycaps(shown)) caps_w += std::max(px(28), text_width(fonts().label, cap) + px(16)) + px(3);
      ImGui::SetCursorScreenPos(ImVec2(pos.x + (w - caps_w) / 2, pos.y + (h - px(28)) / 2));
      // V8: a cap pops from 0.96 to 1 as it joins the chord (the string is the key of the arrival), and a refused chord shakes 4 px, two
      // cycles, 160 ms.
      const int caps_v = draw->VtxBuffer.Size;
      chord_button("##recorded", shown, "", false, 28);
      const ImVec2 caps_center(pos.x + w / 2, pos.y + h / 2);
      scale_since(draw, caps_v, caps_center, 0.96f + 0.04f * appear(ImGui::GetID(("##cap" + shown).c_str()), kDurFast));
      if (!panel.recorded_problem.empty()) {
        const float shake = appear(ImGui::GetID(("##shake" + panel.recorded_problem).c_str()), 0.16f);
        shift_since(draw, caps_v, std::sin(shake * 4.0f * IM_PI) * px(4) * (1.0f - shake), 0.0f);
      }
      if (panel.recorded_chord.empty()) {
        // Still waiting for the key that completes it.
        draw->AddText(fonts().label, fonts().label->LegacySize,
                      ImVec2(pos.x + (w + caps_w) / 2 + px(6), pos.y + (h - fonts().label->LegacySize) / 2), pal().faint, "+");
      }
      ImGui::SetCursorScreenPos(after);
    }
    // What a chord may be, said in the place one is chosen, not on the page behind it.
    const std::string limits = controller ? "Max 4 buttons" : "Max 3 keys";
    const float limits_w = text_width(fonts().small, limits);
    draw->AddText(fonts().small, fonts().small->LegacySize,
                  ImVec2(pos.x + w - limits_w - px(10), pos.y + h - fonts().small->LegacySize - px(6)), pal().faint,
                  limits.c_str());
    const std::string current = env.store.chord(panel.recording, controller);
    if (!panel.recorded_problem.empty()) text(fonts().small, pal().warn, panel.recorded_problem);
    else text(fonts().small, pal().faint, "Now: " + (current.empty() ? std::string("not set") : current));
    align_right(px(268));
    // Default puts this action's shipped chord back on this input and stays here, the way Clear does.
    if (dialog_button("Default##recorder", Variant::ghost, nullptr, panel.recorder_saving)) {
      const std::string shipped = default_hotkey(panel.recording, controller);
      panel.recorder_saving = true;
      Json args = Json::object();
      args.set("action", Json(panel.recording));
      args.set("chord", Json(shipped));
      if (controller) args.set("input", Json("controller"));
      env.store.bridge_call("hotkeys.set", args, [&panel = env.panel, &store = env.store](bool ok, const Json& data, const std::string& error) {
        panel.recorder_saving = false;
        if (!ok) {
          panel.recorded_problem = error;
          return;
        }
        store.hotkeys = data;
        store.refresh_hotkeys(350);
      });
      panel.recorded_chord = shipped;
      panel.recorder_preview.clear();
      panel.recorded_problem.clear();
      env.store.pad_capture.clear();
    }
    ImGui::SameLine();
    // Clear never leaves the row: it empties the board, and turns the stored binding off when there is one.
    // Pressing it again on an empty board simply keeps it empty, so the button has no state in which it vanishes.
    if (dialog_button("Clear##recorder", Variant::ghost, nullptr, panel.recorder_saving)) {
      if (!current.empty()) {
        panel.recorder_saving = true;
        Json args = Json::object();
        args.set("action", Json(panel.recording));
        args.set("chord", Json(std::string()));
        if (controller) args.set("input", Json("controller"));
        env.store.bridge_call("hotkeys.set", args, [&panel = env.panel, &store = env.store](bool ok, const Json& data, const std::string& error) {
          panel.recorder_saving = false;
          if (!ok) {
            panel.recorded_problem = error;
            return;
          }
          store.hotkeys = data;
          store.refresh_hotkeys(350);
        });
      }
      panel.recorded_chord.clear();
      panel.recorder_preview.clear();
      panel.recorded_problem.clear();
      env.store.pad_capture.clear();
    }
    ImGui::SameLine();
    if (dialog_button("Cancel##recorder", Variant::ghost)) {
      close();
    } else {
      ImGui::SameLine();
      if (dialog_button("Save##recorder", Variant::primary, nullptr,
                        panel.recorded_chord.empty() || !panel.recorded_problem.empty() || panel.recorder_saving)) {
        panel.recorder_saving = true;
        Json args = Json::object();
        args.set("action", Json(panel.recording));
        args.set("chord", Json(panel.recorded_chord));
        if (controller) args.set("input", Json("controller"));
        const std::string action = panel.recording;
        env.store.bridge_call("hotkeys.set", args, [&panel = env.panel, &store = env.store, action](bool ok, const Json& data, const std::string& error) {
          panel.recorder_saving = false;
          if (!ok) {
            panel.recorded_problem = error;
            return;
          }
          store.hotkeys = data;
          // The engine registers hotkeys on its own window thread a moment after it answers; read them back
          // and say so if Windows refused the chord.
          panel.check_hotkey = action;
          panel.check_hotkey_version = store.hotkeys_version;
          store.refresh_hotkeys(350);
        });
        close();
      }
    }
    end_dialog();
  } else if (!open) {
    close();
  }
}

// ---------------------------------------------------------------- share sheet

namespace {

void share_request(UiStore& store, const char* command, const std::string& id) {
  Json args = Json::object();
  args.set("id", Json(id));
  store.bridge_call(command, args, {});
}

}  // namespace

// The sheet closes: the engine cancels the job, and keeps its file only if it was copied or posted
// (the clipboard may still point at it).
void close_share(UiStore& store, PanelState& panel) {
  const std::string id = store.share["id"].as_string();
  if (!id.empty()) share_request(store, "share.cancel", id);
  store.share = Json::object();
  panel.share_open = false;
  panel.share_playing = false;
  panel.share_message.clear();
  panel.share_webhook_id.clear();
}

namespace {

void share_sheet(ViewEnv& env) {
  PanelState& panel = env.panel;
  UiStore& store = env.store;
  // The sheet is as tall as its content; a short window shrinks the preview instead of scrolling.
  const bool hook = store.webhook["set"].as_bool();
  if (!begin_dialog("##share", panel.sharing.clip ? "Share clip" : "Share image", panel.share_open, 380, 0, 46)) return;
  if (!panel.share_open) {
    close_sheet();
    end_dialog();
    return;
  }
  const Json job = store.share;  // a copy: a reply can replace the store's between two lines of this frame
  const std::string state = job["state"].as_string();
  const bool working = state == "preparing" || state == "fitting";
  const bool posting = state == "posting";
  const bool ready = state == "ready" || state == "posted";
  const auto& hooks = store.webhook["webhooks"].items();
  int chosen_hook = 0;
  std::vector<std::string> hook_names;
  for (size_t i = 0; i < hooks.size(); ++i) {
    hook_names.push_back(hooks[i]["name"].as_string());
    if (hooks[i]["id"].as_string() == panel.share_webhook_id) chosen_hook = static_cast<int>(i);
  }
  if (!hooks.empty() && !posting) panel.share_webhook_id = hooks[chosen_hook]["id"].as_string();

  // One column that holds from the quick panel's 320 wide to the big window, with nothing to scroll: the clip playing
  // across the top (the sheet's one large thing), one line of facts, where it goes, that destination's one or two
  // settings, then the action.
  const float width = ImGui::GetContentRegionAvail().x;
  if (panel.share_frames_for != panel.sharing.file) {
    panel.share_frames_for = panel.sharing.file;
    panel.share_frames.clear();
    Json args = Json::object();
    args.set("file", Json(panel.sharing.file));
    store.bridge_call("clips.frames", args, [&store, &panel, file = panel.sharing.file](bool ok, const Json& data, const std::string&) {
      if (!ok || panel.share_frames_for != file) return;
      for (const Json& frame : data["frames"].items()) panel.share_frames.push_back(frame.as_string());
      store.dirty = true;
    });
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  // The preview plays on its own: the stills spread across the clip, three a second, round and round. Its height gives
  // way first in a short window, so the sheet never needs a scroll bar.
  const float rest = hook ? 370.0f : 270.0f;  // design units the sheet needs besides the picture
  const float preview_h = std::max(px(64), std::min(std::round(width * 9.0f / 16.0f), px(std::min(200.0f, dialog_cap() - rest))));
  const float preview_w = std::min(width, std::round(preview_h * 16.0f / 9.0f));
  const ImVec2 column = ImGui::GetCursorScreenPos();
  const ImVec2 top(column.x + (width - preview_w) / 2, column.y);
  const ImVec2 bottom(top.x + preview_w, top.y + preview_h);
  const size_t frames = panel.share_frames.size();
  // Every still is asked for at once and the loop starts only when all of them are textures, so it never stops on a
  // picture that is still loading; until then the thumbnail stands in.
  std::vector<ImTextureID> stills;
  std::vector<ImVec2> still_sizes;
  for (size_t i = 0; i < frames && panel.sharing.clip; ++i) {
    ImVec2 size;
    const ImTextureID texture = env.images.get(panel.share_frames[i], 480, &size);
    if (!texture || size.x <= 0 || size.y <= 0) break;
    stills.push_back(texture);
    still_sizes.push_back(size);
  }
  panel.share_playing = frames > 1 && stills.size() == frames;
  ImGui::SetCursorScreenPos(top);
  const bool can_play = panel.sharing.clip && (ready || posting);
  ImGui::InvisibleButton("##sharepreview", ImVec2(preview_w, preview_h));
  const bool over = can_play && ImGui::IsItemHovered();
  const bool play = can_play && ImGui::IsItemClicked();
  if (panel.share_playing) {
    // One still a second; the second half of each dissolves into the next, so the loop reads as motion, not as steps.
    const double t = ImGui::GetTime() / 1.0;
    const size_t index = static_cast<size_t>(t) % frames, next = (index + 1) % frames;
    const float blend = ease_out(std::clamp(static_cast<float>((t - std::floor(t) - 0.5) / 0.5), 0.0f, 1.0f));
    const auto cover = [&](size_t i, float alpha) {
      const float box_ratio = preview_w / preview_h, image_ratio = still_sizes[i].x / still_sizes[i].y;
      ImVec2 uv0(0, 0), uv1(1, 1);
      if (image_ratio > box_ratio) {
        uv0.x = (1 - box_ratio / image_ratio) / 2;
        uv1.x = 1 - uv0.x;
      } else {
        uv0.y = (1 - image_ratio / box_ratio) / 2;
        uv1.y = 1 - uv0.y;
      }
      draw->AddImage(stills[i], top, bottom, uv0, uv1, IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f)));
    };
    draw->AddRectFilled(top, bottom, pal().canvas);
    cover(index, 1.0f);
    if (blend > 0.0f) cover(next, blend);
  } else {
    UiMedia shown = panel.sharing;
    if (shown.thumbnail.empty() && frames > 0) shown.thumbnail = panel.share_frames[frames / 2];
    thumbnail(env, shown, top, ImVec2(preview_w, preview_h), 0);
  }
  if (panel.sharing.clip) {
    const std::string length = format_duration(panel.sharing.duration > 0 ? panel.sharing.duration : job["duration"].as_double());
    const float lw = text_width(fonts().strong, length) + px(12), lh = fonts().strong->LegacySize + px(4);
    draw->AddRectFilled(ImVec2(top.x, bottom.y - lh), ImVec2(top.x + lw, bottom.y), with_alpha(pal().canvas, 0.92f));
    draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(top.x + px(6), bottom.y - lh + px(2)), pal().ink, length.c_str());
  }
  if (working) {
    // What the engine is doing to it, across the picture's foot.
    const float progress = state == "preparing" ? 0.0f : static_cast<float>(job["progress"].as_double());
    // V9: the bar eases to the engine's figure rather than jumping between its reports.
    const float shown = anim_to(ImGui::GetID("##shareprogress"), progress, kRateState);
    draw->AddRectFilled(ImVec2(top.x, bottom.y - px(3)), ImVec2(top.x + preview_w * shown, bottom.y), pal().accent);
  }
  if (over) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 c((top.x + bottom.x) / 2, (top.y + bottom.y) / 2);
    draw->AddRectFilled(ImVec2(c.x - px(16), c.y - px(16)), ImVec2(c.x + px(16), c.y + px(16)), pal().accent);
    draw_icon(draw, "play", ImVec2(c.x - px(7.5f), c.y - px(7.5f)), px(15), pal().on_accent, 1.7f, true);
  }
  draw->AddRect(top, bottom, over ? pal().accent : pal().line_strong, 0, 0, over ? px(2) : 1.0f);
  if (play) open_editor(env, {job["path"].as_string()}, true);
  ImGui::SetCursorScreenPos(ImVec2(column.x, bottom.y + px(10)));

  // The facts on one line: the name, and its size at the right (or what the engine is doing, while it works).
  {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const double size = panel.sharing.size > 0 ? static_cast<double>(panel.sharing.size) : job["source_bytes"].as_double();
    std::string facts = format_bytes(size);
    ImU32 facts_color = pal().muted;
    if (state == "preparing") {
      facts = "Looking\xE2\x80\xA6";
    } else if (state == "fitting") {
      facts = "Fitting " + std::to_string(static_cast<int>(job["progress"].as_double() * 100)) + "%";
    } else if (state == "failed") {
      facts = "Not prepared";
      facts_color = pal().danger;
    } else if (!job["copy"].as_bool() && job["bytes"].as_double() > 0) {
      facts = format_bytes(size) + " to " + format_bytes(job["bytes"].as_double());
    }
    const float facts_w = text_width(fonts().small, facts);
    text_ellipsis(fonts().strong, pal().ink, file_stem(panel.sharing.file), std::max(px(40), width - facts_w - px(12)));
    draw->AddText(fonts().small, fonts().small->LegacySize,
                  ImVec2(at.x + width - facts_w, at.y + (fonts().strong->LegacySize - fonts().small->LegacySize) / 2), facts_color,
                  facts.c_str());
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + fonts().strong->LegacySize + px(12)));
  }
  if (state == "failed") {
    text_wrapped(fonts().small, pal().danger, job["error"].as_string("It could not be prepared"), width);
    ImGui::Dummy(ImVec2(0, px(4)));
  }

  // Where it goes is the one choice, made first; under it only that destination's settings, one row each.
  Json live;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  for (const Json& link : store.links["links"].items()) {
    if (link["file"].as_string() == panel.sharing.file && link["expires"].as_int(0) > now) live = link;
  }
  const bool is_live = !live["id"].as_string().empty();
  int& tab_choice = panel.share_tab;
  tab_choice = std::clamp(tab_choice, 0, 1);
  segmented("##sharetab", {"BuffrX link", "Copy / Discord"}, tab_choice, unpx(std::floor(width / 2)));
  ImGui::Dummy(ImVec2(0, px(4)));

  auto copied = [&store](const std::string& url) { copy_link(store, url); };
  const bool plan_possible = job["plan_possible"].as_bool(true);
  const std::string id = job["id"].as_string();
  const bool fits = job["fits_limit"].as_bool(true);
  // A setting's row: its words on the left, its control at the right edge, one 28-unit line.
  const float line_h = px(28);
  const auto row_words = [&](ImFont* font, const std::string& words, ImU32 color, float indent = 0.0f) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(font, font->LegacySize, ImVec2(at.x + indent, at.y + (line_h - font->LegacySize) / 2), color,
                                        words.c_str());
    return at;
  };

  // V9: the chosen destination's settings fade in when the tab changes, and when the sheet opens (the old ones leave at once).
  static int shown_tab = -1;
  static double tab_changed_at = -10.0;
  if (shown_tab != tab_choice) {
    shown_tab = tab_choice;
    tab_changed_at = anim_detail::wall_seconds();
  }
  const float tab_t = ease_out(progress(anim_detail::wall_seconds() - tab_changed_at, kDurBase));
  if (tab_t < 1.0f) anim_touch();
  const int tab_v = draw->VtxBuffer.Size;
  if (tab_choice == 0) {
    if (is_live) {
      const int64_t left = live["expires"].as_int() - static_cast<int64_t>(std::time(nullptr));
      const int64_t viewers = live["viewers"].as_int();
      const int64_t watching = live["watching"].as_int();
      std::string line = "Live, ";
      line += link_activity(static_cast<int>(watching), static_cast<int>(viewers));
      line += ", " + link_expiry(left);
      const ImVec2 at = row_words(fonts().strong, line, pal().accent);
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + line_h));
      // The one thing the sharer can act on: a cap they set, or a relay that is why the send is slow.
      const std::string advice = link_advice(watching > 0, live["via"].as_string(),
                                             store.config["share_upload_kbps"].as_int(0));
      if (!advice.empty()) {
        text_wrapped(fonts().small, live["via"].as_string() == "relay" ? pal().warn : pal().faint, advice, width);
      }
    } else {
      const ImVec2 at = row_words(fonts().strong, "Link lasts", pal().ink);
      // Four windows, each a quarter of the strip: 1, 4, 12 and 24 hours.
      const int windows[4] = {60, 240, 720, 1440};
      const float chips = std::floor(std::min(px(248), width * 0.64f));
      ImGui::SetCursorScreenPos(ImVec2(at.x + width - chips, at.y + px(2)));
      int choice = 0;
      for (int i = 0; i < 4; ++i) {
        if (panel.link_minutes == windows[i]) choice = i;
      }
      ImGui::BeginDisabled(panel.link_busy);
      if (segmented("##linkminutes", {"1 h", "4 h", "12 h", "24 h"}, choice, unpx(chips / 4), 24, fonts().small)) {
        panel.link_minutes = windows[choice];
      }
      ImGui::EndDisabled();
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + line_h));
    }
    ImGui::Dummy(ImVec2(0, px(2)));
    text_wrapped(fonts().small, pal().faint, "Your friend needs BuffrX installed to open it.", width);
  } else {
    // Copy puts the file on the clipboard for any chat; Post sends it to the channel set in Settings. Both send what is
    // prepared here, so the size choice applies to both.
    const bool plan_copy = job["plan_copy"].as_bool();
    const std::string limit_text = format_bytes(static_cast<double>(job["limit_bytes"].as_int(static_cast<int64_t>(panel.share_limit) * 1024 * 1024)));
    std::string hint;
    if (state == "preparing") hint = "Checking the size\xE2\x80\xA6";
    else if (!panel.sharing.clip) hint = fits ? "The image goes as it is." : "The image is over the upload limit.";
    else if (plan_copy) hint = panel.share_fit ? "Already under " + limit_text + ": it goes as it is." : "It goes as it is.";
    else if (panel.share_fit) hint = "Made to fit " + limit_text + ". " + share_quality(false, job["kept"].as_double(1.0)) + ".";
    else if (!plan_possible) hint = "Too long for " + limit_text + ". Trim it first.";
    else hint = "Turn it on to send about " + std::to_string(job["plan_height"].as_int()) + "p under " + limit_text + ".";
    const bool locked = posting || state == "preparing" || !plan_possible;
    const bool on = panel.share_fit && plan_possible;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (line_h - px(20)) / 2));
    if (checkbox("##sharefit", on, locked)) {
      panel.share_fit = !on;
      open_share(env, panel.sharing, panel.share_limit);
    }
    ImGui::SetCursorScreenPos(at);
    row_words(fonts().strong, "Shrink for Discord", locked && !on ? pal().muted : pal().ink, px(30));
    // The limit it fits: Discord's free one by default, a boosted server's or Nitro's. A limit set by hand in config.json
    // shows as a fifth choice, so the row never claims one it is not.
    const std::vector<int> choices = share_limit_choices();
    std::vector<std::string> limits;
    for (int limit : choices) limits.push_back(std::to_string(limit) + " MB");
    int selected = -1;
    for (size_t i = 0; i < choices.size(); ++i) {
      if (choices[i] == panel.share_limit) selected = static_cast<int>(i);
    }
    if (selected < 0) {
      limits.push_back(std::to_string(panel.share_limit) + " MB");
      selected = static_cast<int>(choices.size());
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x + width - px(104), at.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), (line_h - fonts().body->LegacySize) * 0.5f));
    if (select("##sharelimit", limits, selected, 104, posting || !panel.share_fit) &&
        selected < static_cast<int>(choices.size())) {
      open_share(env, panel.sharing, choices[selected]);
    }
    ImGui::PopStyleVar();
    tooltip("Discord's upload limit: free 20 MB, boosted servers 50 or 100 MB, Nitro 500 MB.");
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + line_h + px(2)));
    const bool not_posted = state == "ready" && job.has("error");
    std::string status = hint;
    if (not_posted) status = "Post: " + job["error"].as_string();
    else if (posting) status = job["progress"].as_double() >= 1.0 ? "Upload sent. Waiting for Discord to confirm."
                         : "Uploading to " + job["destination"].as_string() + ": " + std::to_string(static_cast<int>(job["progress"].as_double() * 100)) + "%";
    else if (state == "posted") status = "Posted to " + job["destination"].as_string() + ". Discord confirmed the post.";
    text_wrapped(fonts().small, not_posted ? pal().warn : state == "posted" ? pal().ok : pal().faint, status, width);
    if (hook) {
      ImGui::Dummy(ImVec2(0, px(6)));
      const ImVec2 destination_at = row_words(fonts().strong, "Server", pal().ink);
      const float picker_w = std::min(px(230), width * 0.7f);
      ImGui::SetCursorScreenPos(ImVec2(destination_at.x + width - picker_w, destination_at.y));
      if (select("##sharedestination", hook_names, chosen_hook, unpx(picker_w), posting) && chosen_hook < static_cast<int>(hooks.size())) {
        panel.share_webhook_id = hooks[chosen_hook]["id"].as_string();
      }
      ImGui::SetCursorScreenPos(ImVec2(destination_at.x, destination_at.y + line_h + px(6)));
      text_field("##sharemessage", panel.share_message, unpx(width), "Message (optional)", 300);
    } else {
      ImGui::Dummy(ImVec2(0, px(6)));
      text_wrapped(fonts().small, pal().faint, "Add a server in Settings > Sharing to post to Discord.", width);
    }
  }
  fade_since(draw, tab_v, tab_t);

  // The foot: Close, and the action for the tab chosen. Copy and Post share what Close leaves.
  const float gap = ImGui::GetStyle().ItemSpacing.x, close_w = px(62);
  const bool two_actions = tab_choice == 1 ? hook : is_live;
  const int actions = two_actions ? 2 : 1;
  const float action_w = std::floor((width - close_w - gap * actions) / actions);
  align_right(width);
  if (dialog_button("Close##share", Variant::ghost, nullptr, false, unpx(close_w), 34) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    close_share(store, panel);
    close_sheet();
    end_dialog();
    return;
  }
  ImGui::SameLine();
  if (tab_choice == 0) {
    if (is_live) {
      if (dialog_button("Stop##link", Variant::ghost, nullptr, false, unpx(action_w), 34)) {
        Json args = Json::object();
        args.set("id", live["id"]);
        store.attempt("link.stop", args, "The link did not stop");
      }
      ImGui::SameLine();
      if (dialog_button("Copy link##link", Variant::primary, "copy", false, unpx(action_w), 34)) {
        copied(live["url"].as_string());
        close_share(store, panel);  // the sheet's one job is done; the link stays live
        close_sheet();
        end_dialog();
        return;
      }
    } else if (dialog_button(panel.link_busy ? "Making\xE2\x80\xA6##link" : "Copy link##link", Variant::primary, "share", panel.link_busy,
                             unpx(action_w), 34)) {
      panel.link_busy = true;
      Json args = Json::object();
      args.set("file", Json(panel.sharing.file));
      args.set("minutes", Json(panel.link_minutes));
      store.bridge_call("link.share", args, [&store, &panel, copied](bool ok, const Json& data, const std::string& error) {
        panel.link_busy = false;
        if (!ok) {
          store.toast(UiToast::error, "No link made", error);
          return;
        }
        copied(data["url"].as_string());
        // A short link is an improvement on the long one, never a prerequisite: when the service did
        // not answer, the sheet says so instead of pretending the short link was used.
        const std::string note = data["note"].as_string();
        if (!note.empty()) store.toast(UiToast::info, "Long link used", note);
        store.refresh_links();
        close_share(store, panel);  // a copied link leaves the sheet; the link itself keeps serving
      });
    }
  } else {
    const char* copy_label = panel.sharing.clip ? "Copy clip##copy" : "Copy image##copy";
    if (dialog_button(copy_label, hook ? Variant::normal : Variant::primary, "copy", !ready, unpx(action_w), 34)) {
      Json args = Json::object();
      args.set("id", Json(id));
      store.bridge_call("share.copy", args, [&store, &panel, fits](bool ok, const Json&, const std::string& error) {
        if (!ok) {
          store.toast(UiToast::error, "Not copied", error);
          return;
        }
        store.toast(UiToast::success, "Ready to paste",
                    fits ? "Ctrl+V in a Discord chat - it fits the upload limit" : "Ctrl+V into a chat or a folder");
        close_share(store, panel);
      });
    }
    if (hook) {
      ImGui::SameLine();
      const std::string post_label = posting ? (job["progress"].as_double() >= 1.0 ? "Confirming...##post"
                                                : "Posting " + std::to_string(static_cast<int>(job["progress"].as_double() * 100)) + "%##post")
                                             : state == "posted" ? "Post again##post" : "Post##post";
      if (dialog_button(post_label.c_str(), Variant::primary, posting ? nullptr : "share", !ready || !fits, unpx(action_w), 34)) {
        Json args = Json::object();
        args.set("id", Json(id));
        args.set("message", Json(panel.share_message));
        args.set("webhook_id", Json(panel.share_webhook_id));
        Json pending = job;
        pending.set("state", Json("posting"));
        pending.set("progress", Json(0.0));
        pending.set("destination", Json(hook_names[chosen_hook]));
        store.share = pending;  // disable Post immediately, before the worker replies
        store.bridge_call("share.discord", args, [&store, id, job](bool ok, const Json& data, const std::string& error) {
          if (store.share["id"].as_string() != id || store.share["state"].as_string() != "posting") return;
          if (ok) {
            // An upload event can beat this reply; never replace newer progress with its initial zero.
            if (store.share["progress"].as_double() <= data["progress"].as_double()) store.share = data;
          } else {
            Json failed = job;
            failed.set("state", Json("ready"));
            failed.set("error", Json(error));
            store.share = failed;
          }
        });
      }
      if (!fits) tooltip("Too big for the upload limit - turn on Shrink for Discord");
      else tooltip("Post to the selected server's webhook channel");
    }
  }
  end_dialog();
}

// A BuffrX link a friend sent, found on the clipboard: one question, and nothing happens without a yes.
void link_offer_sheet(ViewEnv& env) {
  PanelState& panel = env.panel;
  bool open = !panel.link_offer.empty();
  if (!begin_dialog("##linkoffer", "Open the clip a friend sent?", open, 440)) return;
  if (!open) {
    close_sheet();
    end_dialog();
    return;
  }
  const int64_t left = panel.link_offer_expires - static_cast<int64_t>(std::time(nullptr));
  const float width = ImGui::GetContentRegionAvail().x;
  if (left <= 0) {
    text_wrapped(fonts().body, pal().muted, "You copied a BuffrX link, but it has expired. Ask your friend for a new one.", width);
    align_right(px(90));
    if (dialog_button("OK##linkoffer", Variant::primary, nullptr, false, 90) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      panel.link_offer.clear();
      close_sheet();
    }
    end_dialog();
    return;
  }
  text_wrapped(fonts().body, pal().muted,
               "You copied a BuffrX link. The clip comes straight from your friend's PC while it is on (" + link_expiry(left) + ").",
               width);
  align_right(px(90) + px(120) + ImGui::GetStyle().ItemSpacing.x);
  if (dialog_button("Not now##linkoffer", Variant::ghost, nullptr, false, 90) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    panel.link_offer.clear();
    close_sheet();
  } else {
    ImGui::SameLine();
    if (dialog_button("Open clip##linkoffer", Variant::primary, "play", false, 120)) {
      Json args = Json::object();
      args.set("url", Json(panel.link_offer));
      env.store.attempt("link.open", args, "The link did not open");
      panel.link_offer.clear();
      close_sheet();
    }
  }
  end_dialog();
}

// The failing reasons that are the engine's own (link.cpp), in words. Everything else the sheet knows
// is link_failure()'s.
std::string receive_failure_text(const std::string& reason) {
  if (reason == "unresponsive") return "Your friend's PC stopped responding, so the clip did not arrive. Try again.";
  if (reason == "incomplete") return "The clip arrived incomplete, so it was thrown away. Try again.";
  if (reason == "corrupt") return "The clip arrived damaged, so it was thrown away. Try again.";
  if (reason == "helper") return "Sharing stopped unexpectedly. Try again.";
  return link_failure(reason);
}

// A clip arriving from a friend's link: one under way first, else the last one that failed. A finished
// one leaves by itself (it plays in the preview); a failed one stays until it is read or tried again.
void receive_sheet(ViewEnv& env) {
  UiStore& store = env.store;
  Json shown;
  for (const Json& receive : store.links["receives"].items()) {
    const std::string state = receive["state"].as_string();
    if (state == "connecting" || state == "receiving" || state == "asking" || state == "retrying") {
      shown = receive;
      break;
    }
    if (state == "failed") shown = receive;
  }
  bool open = !shown["id"].as_string().empty();
  if (!begin_dialog("##receive", "Incoming clip", open, 440)) return;
  if (!open) {
    close_sheet();
    end_dialog();
    return;
  }
  const std::string id = shown["id"].as_string();
  const std::string state = shown["state"].as_string();
  const std::string name = shown["name"].as_string();
  const double size = shown["size"].as_double();
  const float width = ImGui::GetContentRegionAvail().x;
  auto command = [&store, id](const char* name, bool accept, bool with_answer) {
    Json args = Json::object();
    args.set("id", Json(id));
    if (with_answer) args.set("accept", Json(accept));
    store.attempt(name, args, std::string(name) == "link.cancel" ? "The clip could not be cancelled" : "The answer was not sent");
  };
  if (!name.empty()) text_ellipsis(fonts().strong, pal().ink, name, width);
  // What is coming, before it is here: the first frame of the file while it is still arriving.
  const std::string poster = shown["thumbnail"].as_string();
  if (!poster.empty()) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = std::floor(width * 9.0f / 16.0f);
    picture(env, poster, "film", at, ImVec2(width, height), 0.0f, 1.0f);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::Dummy(ImVec2(0, px(10)));
  }
  if (state == "connecting") {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    spinner(ImGui::GetWindowDrawList(), ImVec2(at.x + px(7), at.y + fonts().body->LegacySize / 2), px(6), pal().accent);
    ImGui::SetCursorScreenPos(ImVec2(at.x + px(20), at.y));
    text(fonts().body, pal().muted, "Finding the clip\xE2\x80\xA6");
    align_right(px(90));
    if (dialog_button("Cancel##receive", Variant::ghost, nullptr, false, 90)) command("link.cancel", false, false);
  } else if (state == "retrying") {
    const int attempt = static_cast<int>(shown["attempt"].as_int(2));
    const int tries = static_cast<int>(shown["tries"].as_int(3));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    spinner(ImGui::GetWindowDrawList(), ImVec2(at.x + px(7), at.y + fonts().body->LegacySize / 2), px(6), pal().accent);
    ImGui::SetCursorScreenPos(ImVec2(at.x + px(20), at.y));
    text(fonts().body, pal().muted,
         "Trying again\xE2\x80\xA6 attempt " + std::to_string(attempt) + " of " + std::to_string(tries));
    align_right(px(90));
    if (dialog_button("Cancel##receive", Variant::ghost, nullptr, false, 90)) command("link.cancel", false, false);
  } else if (state == "asking") {
    text_wrapped(fonts().body, pal().muted, "This clip is " + format_bytes(size) + ". Download it to your clips?", width);
    align_right(px(90) + px(110) + ImGui::GetStyle().ItemSpacing.x);
    if (dialog_button("Not now##receive", Variant::ghost, nullptr, false, 90)) command("link.answer", false, true);
    ImGui::SameLine();
    if (dialog_button("Download##receive", Variant::primary, nullptr, false, 110)) command("link.answer", true, true);
  } else if (state == "receiving") {
    const double bytes = shown["bytes"].as_double();
    level_bar(unpx(width), size > 0 ? static_cast<float>(bytes / size) : 0.0f);
    // The route is worth saying when it is the slow one: a relay carries a fraction of a direct path.
    const std::string via = shown["via"].as_string();
    const std::string route = via == "relay" ? " \xC2\xB7 through a relay, slower" : "";
    text(fonts().small, pal().faint,
         format_bytes(bytes) + " of " + format_bytes(size) + route + " \xC2\xB7 it plays as soon as it arrives");
    align_right(px(90));
    if (dialog_button("Cancel##receive", Variant::ghost, nullptr, false, 90)) command("link.cancel", false, false);
  } else {
    text_wrapped(fonts().body, pal().muted, receive_failure_text(shown["error"].as_string()), width);
    // The ticket the engine kept for this receive: the same link, dialled again, and the failed entry
    // goes first so the retry is the one card on screen.
    const std::string url = shown["url"].as_string();
    if (!url.empty()) {
      align_right(px(100) + px(90) + ImGui::GetStyle().ItemSpacing.x);
      if (dialog_button("Close##receive", Variant::ghost, nullptr, false, 90) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        command("link.cancel", false, false);
      }
      ImGui::SameLine();
      if (dialog_button("Try again##receive", Variant::primary, "share", false, 100)) {
        command("link.cancel", false, false);
        Json args = Json::object();
        args.set("url", Json(url));
        store.attempt("link.open", args, "The link did not open");
      }
    } else {
      align_right(px(90));
      if (dialog_button("OK##receive", Variant::primary, nullptr, false, 90) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        command("link.cancel", false, false);  // a finished entry leaves the list
      }
    }
  }
  end_dialog();
}

}  // namespace

void open_share(UiStore& store, PanelState& panel, const UiMedia& item, int limit_mib) {
  const std::string previous = store.share["id"].as_string();
  if (!previous.empty()) share_request(store, "share.cancel", previous);
  // A fresh sheet starts from the remembered choice; a switch or a limit change inside it keeps its own.
  // Shrink starts off on every new sheet (asked for 2026-11-02): the clip goes as it is unless this sheet asks otherwise.
  if (!panel.share_open || panel.sharing.file != item.file) panel.share_fit = false;
  panel.sharing = item;
  panel.share_open = true;
  panel.share_limit = limit_mib > 0 ? limit_mib : static_cast<int>(store.config["share_limit_mib"].as_int(20));
  // Each request has its own token: a reply for a sheet that has since moved to another limit is
  // recognised as stale even though it names the same file.
  static int64_t requests = 0;
  const int64_t request = ++requests;
  Json pending = Json::object();
  pending.set("state", Json("preparing"));
  pending.set("file", Json(item.file));
  pending.set("request", Json(request));
  store.share = pending;
  Json args = Json::object();
  args.set("file", Json(item.file));
  args.set("limit_mib", Json(panel.share_limit));
  args.set("fit", Json(panel.share_fit));
  const std::string file = item.file;
  store.bridge_call("share.prepare", args, [&store, file, request](bool ok, const Json& data, const std::string& error) {
    // The sheet closed, or moved on to another clip or limit, while the engine looked at the file: this
    // job is nobody's, so it goes.
    if (store.share["request"].as_int() != request || store.share["state"].as_string() != "preparing") {
      if (ok) share_request(store, "share.cancel", data["id"].as_string());
      return;
    }
    if (!ok) {
      Json failed = Json::object();
      failed.set("state", Json("failed"));
      failed.set("file", Json(file));
      failed.set("error", Json(error));
      store.share = failed;
      return;
    }
    store.share = data;
    // A short clip can finish fitting before this reply is read; ask once more so the sheet never
    // waits on a "fitting" that is already over.
    Json again = Json::object();
    again.set("id", data["id"]);
    store.bridge_call("share.get", again, [&store](bool ok, const Json& data, const std::string&) {
      const std::string current = store.share["state"].as_string();
      if (ok && data["id"].as_string() == store.share["id"].as_string() && (current == "ready" || current == "fitting")) store.share = data;
    });
  });
  store.bridge_call("share.webhook.get", Json::object(), [&store](bool ok, const Json& data, const std::string&) {
    if (ok) store.webhook = data;
  });
  store.refresh_links();
}

// The guide every new user needs. One section at a time: what the part does, numbered steps, then tips. F1 or ? opens
// it; Esc or the button closes it.
void keys_sheet(ViewEnv& env) {
  PanelState& panel = env.panel;
  if (!begin_dialog("##keys", "Help", panel.keys_open, 760, 0, 46, panel.help_height)) return;
  struct Step {
    const char* title;
    const char* body;
  };
  struct Guide {
    const char* title;
    const char* icon;
    const char* subtitle;
    const char* overview;
    const Step* steps;
    int step_count;
    const char* tips;
  };
  static const Step capture_steps[] = {
      {"Set the buffer length.",
       "Use the BUFFER LIMIT chips in OPTIONS, or open Settings > Capture for the exact length."},
      {"Turn the buffer on.",
       "Press START in the BUFFER pane. ON means it is buffering, and the pane says how much play it keeps."},
      {"Save a moment.", "Press Alt+1 (Save clip). The clip appears in the library and a notice says it was saved."},
      {"Record or take a screenshot.",
       "Alt+3 starts and stops a manual recording. Alt+2 takes a screenshot of the game."},
  };
  static const Step timeline_steps[] = {
      {"Set the length.",
       "In the TIMELINE pane choose OFF, 30M, 1H or 2H. Timeline writes only while a game is detected."},
      {"Open the timeline.",
       "Press Alt+4 or the TIMELINE button. The overlay opens on the newest footage with a clip already chosen."},
      {"Find the moment.",
       "Drag the bar, wheel to zoom, and click the bar to move. I and O set the clip's start and end; , and . step a "
       "frame."},
      {"Save the clip.",
       "Press Enter to save, E to save and edit, or Shift+Enter to save and go back to the game."},
  };
  static const Step library_steps[] = {
      {"Open the panel.",
       "Use Alt+X or the tray icon. The library is on the right, with CLIPS and SCREENSHOTS tabs."},
      {"Preview a card.",
       "Click a card or press Enter. Clips open in the preview window; screenshots open in the panel's preview."},
      {"Manage a card.",
       "E edits, S shares, C copies and F shows the file. F2 renames the selected card and Del deletes it."},
      {"Move around.",
       "Tab walks the deck and the library, the arrows move between cards, and Esc steps back."},
  };
  static const Step editor_steps[] = {
      {"Open the editor.",
       "Press E on a clip, click Trim in the preview, or use Edit in a card's menu."},
      {"Add clips.",
       "Drop clips from the library rail onto the sequence, or use Browse. The same rail opens screenshots in the image "
       "editor."},
      {"Trim and arrange.",
       "Drag a clip's edge to trim, split at the playhead, and drag clips along the sequence to reorder. Edit, Color, "
       "Speed and Export are the tabs."},
      {"Export.",
       "Open Export, choose the size and quality, then Save. The finished file is added to the library."},
  };
  static const Step share_steps[] = {
      {"Pick the clip.", "Use a library card's menu, the preview's Share button, or Share in the tray."},
      {"Make a BuffrX link.",
       "BuffrX link creates a private link that opens in BuffrX. Copy or send it, and use Stop to end it early."},
      {"Post to Discord.",
       "Choose Discord, paste a channel webhook, and tick Shrink for Discord if the file is too large. Post sends it."},
      {"Receive a link.",
       "Open a buffrx:// link and accept the clip. It is checked, then appears in Shared with me."},
  };
  static const Step settings_steps[] = {
      {"Open Settings.", "Press , or the gear in the panel. The sections are on the left, and Esc returns to the panel."},
      {"Capture and Audio.",
       "Choose the game or display source, buffer length, desktop sound and the microphone."},
      {"Video.",
       "Pick Auto, Fast, Balanced or Quality, then a codec, resolution and frame rate. PERFORMANCE MODE is in OPTIONS."},
      {"Storage.",
       "Set the clips folder, the Timeline length and how long footage is kept. A clip is filed under its game, a "
       "recording under Recordings; screenshots and edits have their own folders."},
  };
  static const Step keys_steps[] = {
      {"Change a binding.", "Open Settings > Hotkeys. Every action has a keyboard row and a controller chip."},
      {"Record a chord.",
       "Click the row and press the whole chord. Keyboard / Controller switches the input; Clear turns it off."},
      {"Use a controller.",
       "D-pad or left stick moves, A presses, B goes back, and View opens this help."},
      {"Defaults.",
       "Alt+1 saves, Ctrl+Alt+1 quick clip, Alt+2 screenshot, Alt+3 record, Alt+X panel, Alt+4 timeline, Alt+5 marker."},
  };
  static const Guide guides[] = {
      {"capture", "record", "Save moments or record.",
       "BuffrX buffers a rolling stretch of play in the background. Save it after something good happens, or record a "
       "normal clip from start to stop.",
       capture_steps, 4, "Quick clip (Ctrl+Alt+1) saves the shorter quick length. The buffer must be ON for Save clip."},
      {"timeline", "history", "Save moments from play.",
       "Timeline keeps your recent play ready in the background, so you can go back further than the buffer and save a "
       "moment after it passed. It runs only while a game is open, and only clips you save join the library.",
       timeline_steps, 4,
       "The default clip is the 30 seconds before the playhead. Alt+5 adds a marker while you play; select one and use "
       "Remove marker."},
      {"library", "film", "View and manage saves.",
       "The library is every clip and screenshot BuffrX saved. Clips and screenshots have their own tab.",
       library_steps, 4, "The quick panel lists the newest saves and has the same two tabs. Every card's menu has the "
                         "same actions."},
      {"editor", "scissors", "Trim, crop and export.",
       "The editor trims, splits, crops, stitches and exports clips. It opens as its own window and closes when you are "
       "done.",
       editor_steps, 4,
       "Color and crop apply to every clip in the sequence. Speed and volume are set per clip."},
      {"share", "share", "Share clips and links.",
       "Share sends a clip without cloud storage. A BuffrX link goes app to app, and Discord gets a webhook post.",
       share_steps, 4,
       "Links carry no IP address and expire on their own. The LIVE LINK pane shows who is watching or downloading."},
      {"settings", "settings", "Buffer, quality, controls.",
       "Settings holds the engine's real controls: capture, audio, video, profiles, storage, hotkeys, sharing, theme "
       "and system.",
       settings_steps, 4,
       "Hotkeys sets a keyboard chord and a controller binding for every action. Profiles save a whole setup, and "
       "Theme repaints every BuffrX window."},
      {"keybindings", "keyboard", "Shortcuts and controller.",
       "Every action has a keyboard shortcut and an optional controller binding. Change either in Settings > Hotkeys.",
       keys_steps, 4,
       "F1 or ? opens this help anywhere in the panel. , opens Settings, Esc steps back. Settings > Hotkeys > View all "
       "keybinds shows the full list in one place."},
  };
  constexpr int guide_count = static_cast<int>(sizeof(guides) / sizeof(guides[0]));
  // A real control for every step: the same icon the panel draws for that action, in the order of the steps above.
  static const char* step_icons[guide_count][4] = {
      {"settings", "power", "save", "record"},
      {"sliders", "history", "play", "save"},
      {"expand", "play", "dots", "keyboard"},
      {"scissors", "plus", "edit", "save"},
      {"share", "external", "external", "folder"},
      {"settings", "mic", "sliders", "hdd"},
      {"keyboard", "record", "game", "keyboard"},
  };

  const float body_w = ImGui::GetContentRegionAvail().x;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Palette& p = pal();

  auto put = [&](ImFont* font, ImU32 color, ImVec2 at, const char* value, float wrap) {
    draw->AddText(font, font->LegacySize, at, color, value, nullptr, wrap);
    return font->CalcTextSizeA(font->LegacySize, FLT_MAX, wrap, value).y;
  };

  const float nav_w = px(222);
  // The seam sits halfway across `gap`. Twice the dialog's 20-unit padding gives the list the same space on its
  // right as the sheet's left edge gives it.
  const float gap = px(40);
  const float detail_w = body_w - nav_w - gap;
  const float nav_gap = px(4);

  auto heading = [&](const char* value, ImVec2 at, float width, ImU32 color) {
    draw->AddText(fonts().label, fonts().label->LegacySize, at, color, value);
    const float text_w = text_width(fonts().label, value);
    const float line_y = at.y + fonts().label->LegacySize * 0.5f;
    draw->AddLine(ImVec2(at.x + text_w + px(10), line_y), ImVec2(at.x + width, line_y), p.line, 1.0f);
  };

  // The sheet's own name and promise, in the section column, as a guide's cover. The right side starts at the same
  // height so the two columns read across.
  float nav_y = start.y;
  draw->AddText(fonts().title, fonts().title->LegacySize, ImVec2(start.x, nav_y), p.ink, "buffrx help");
  nav_y += fonts().title->LegacySize + px(4);
  nav_y += put(fonts().small, p.muted, ImVec2(start.x, nav_y), "Quick guides for every part of BuffrX.", nav_w);
  nav_y += px(13);
  draw->AddLine(ImVec2(start.x, nav_y), ImVec2(start.x + nav_w, nav_y), p.line, 1.0f);
  const float underline_y = nav_y;  // the cover card ends on this line, not below it
  nav_y += px(14);
  heading("sections", ImVec2(start.x, nav_y), nav_w, p.muted);
  const float top = nav_y + px(20);

  panel.help_section = std::clamp(panel.help_section, 0, guide_count - 1);
  const Guide& g = guides[panel.help_section];
  const float dx = start.x + nav_w + gap;
  float y = start.y;

  // The guide's cover: a bordered card with its icon, name and promise. Its foot matches the section column's rule, so
  // the two columns start on one line.
  const float cover_h = underline_y - start.y;
  draw->AddRectFilled(ImVec2(dx, y), ImVec2(dx + detail_w, y + cover_h), p.canvas);
  draw->AddRect(ImVec2(dx, y), ImVec2(dx + detail_w, y + cover_h), p.line, 0, 0, 1.0f);
  // One inset on every side: the tile takes the same 14 units from the left, top and bottom, and the text block is
  // centred vertically beside it instead of hanging from two hand-set offsets.
  const float cover_pad = px(14);
  const float tile_sz = std::max(px(32), cover_h - cover_pad * 2);
  draw->AddRectFilled(ImVec2(dx + cover_pad, y + cover_pad), ImVec2(dx + cover_pad + tile_sz, y + cover_pad + tile_sz),
                      with_alpha(p.accent, 0.14f));
  draw_icon(draw, g.icon,
            ImVec2(dx + cover_pad + (tile_sz - px(22)) * 0.5f, y + cover_pad + (tile_sz - px(22)) * 0.5f), px(22),
            p.accent);
  const float text_x = dx + cover_pad + tile_sz + px(14);
  const float title_h = fonts().display->LegacySize;
  const float text_h = title_h + px(4) + fonts().small->LegacySize;
  const float text_y = y + (cover_h - text_h) * 0.5f;
  draw->AddText(fonts().display, title_h, ImVec2(text_x, text_y), p.ink, g.title);
  draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(text_x, text_y + title_h + px(4)), p.muted,
                g.subtitle, nullptr, detail_w - (text_x - dx) - cover_pad);
  y += cover_h + px(12);

  // A section header opens and closes its body. The chevron and the cursor are the only states it needs.
  auto section = [&](int index, const char* icon, const char* title) {
    const ImVec2 at(dx, y);
    ImGui::SetCursorScreenPos(at);
    ImGui::PushID(index + 100);
    if (ImGui::InvisibleButton("##help-section", ImVec2(detail_w, px(38)), ImGuiButtonFlags_EnableNav)) {
      panel.help_open[index] = !panel.help_open[index];
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const bool open = panel.help_open[index];
    draw->AddRectFilled(at, ImVec2(dx + detail_w, y + px(38)),
                        with_alpha(p.accent, hovered ? 0.12f : 0.07f));
    draw->AddRect(at, ImVec2(dx + detail_w, y + px(38)), p.line, 0, 0, 1.0f);
    draw_icon(draw, icon, ImVec2(dx + px(12), y + px(11)), px(16), p.accent);
    draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(dx + px(38), y + px(10)), p.ink, title);
    draw_icon(draw, open ? "chevronUp" : "chevronDown", ImVec2(dx + detail_w - px(22), y + px(12)), px(14),
              hovered ? p.ink : p.muted, 1.4f);
    y += px(38);
    return open;
  };

  if (section(0, "layers", "overview")) {
    const float inner_w = detail_w - px(24);
    const float text_h = fonts().small->CalcTextSizeA(fonts().small->LegacySize, FLT_MAX, inner_w, g.overview).y;
    const float box_h = text_h + px(24);
    draw->AddRectFilled(ImVec2(dx, y), ImVec2(dx + detail_w, y + box_h), p.canvas);
    draw->AddRect(ImVec2(dx, y), ImVec2(dx + detail_w, y + box_h), p.line, 0, 0, 1.0f);
    put(fonts().small, p.muted, ImVec2(dx + px(12), y + px(12)), g.overview, inner_w);
    y += box_h + px(10);
  } else {
    y += px(10);
  }

  if (section(1, "sliders", "steps")) {
    const float tile_sz = px(38);
    const float text_x = dx + px(12) + tile_sz + px(12);
    const float text_w = detail_w - (text_x - dx) - px(12);
    for (int i = 0; i < g.step_count; ++i) {
      const Step& step = g.steps[i];
      const float title_h = fonts().strong->LegacySize;
      const float body_h = fonts().small->CalcTextSizeA(fonts().small->LegacySize, FLT_MAX, text_w, step.body).y;
      const float card_h = std::max(tile_sz, title_h + px(6) + body_h) + px(24);
      draw->AddRectFilled(ImVec2(dx, y), ImVec2(dx + detail_w, y + card_h), p.canvas);
      draw->AddRect(ImVec2(dx, y), ImVec2(dx + detail_w, y + card_h), p.line, 0, 0, 1.0f);
      const ImVec2 tile(dx + px(12), y + (card_h - tile_sz) * 0.5f);
      draw->AddRectFilled(tile, ImVec2(tile.x + tile_sz, tile.y + tile_sz), with_alpha(p.accent, 0.10f));
      draw_icon(draw, step_icons[panel.help_section][i], ImVec2(tile.x + px(10), tile.y + px(10)), px(18), p.accent);
      draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(text_x, y + px(12)), p.ink, step.title);
      put(fonts().small, p.muted, ImVec2(text_x, y + px(12) + title_h + px(6)), step.body, text_w);
      y += card_h + px(8);
    }
    y += px(2);
  } else {
    y += px(10);
  }

  if (section(2, "info", "tips")) {
    const float text_x = dx + px(28);
    const float inner_w = detail_w - px(40);
    const float text_h = fonts().small->CalcTextSizeA(fonts().small->LegacySize, FLT_MAX, inner_w, g.tips).y;
    const float box_h = text_h + px(24);
    draw->AddRectFilled(ImVec2(dx, y), ImVec2(dx + detail_w, y + box_h), p.canvas);
    draw->AddRect(ImVec2(dx, y), ImVec2(dx + detail_w, y + box_h), p.line, 0, 0, 1.0f);
    draw->AddRectFilled(ImVec2(dx + px(14), y + px(17)), ImVec2(dx + px(19), y + px(22)), p.accent);
    put(fonts().small, p.muted, ImVec2(text_x, y + px(12)), g.tips, inner_w);
    y += box_h + px(10);
  }

  // The list follows the guide's measured height, so folding a section shortens both columns together rather than
  // leaving a hole under the list. 46 is the readable floor and 68 the tallest a row should read as a control.
  const float nav_row_h =
      std::clamp((y - top - (guide_count - 1) * nav_gap) / guide_count, px(46), px(68));
  for (int i = 0; i < guide_count; ++i) {
    const float row_y = top + i * (nav_row_h + nav_gap);
    ImGui::SetCursorScreenPos(ImVec2(start.x, row_y));
    ImGui::PushID(i);
    // EnableNav: without it the rows are invisible to the keyboard, and the first button in the sheet (Replay) takes
    // the focus ring on open.
    if (ImGui::InvisibleButton("##help-nav", ImVec2(nav_w, nav_row_h), ImGuiButtonFlags_EnableNav)) panel.help_section = i;
    const bool hovered = ImGui::IsItemHovered();
    const bool selected = panel.help_section == i;
    // Open on the chosen section rather than on the first control ImGui finds.
    if (selected && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    ImGui::PopID();
    const ImVec2 a(start.x, row_y), b(start.x + nav_w, row_y + nav_row_h);
    draw->AddRectFilled(a, b, selected ? with_alpha(p.accent, 0.10f) : hovered ? with_alpha(p.raised, 0.7f) : p.canvas);
    draw->AddRect(a, b, selected ? p.accent : p.line, 0, 0, selected ? 1.5f : 1.0f);
    const ImVec2 tile(start.x + px(10), row_y + (nav_row_h - px(34)) * 0.5f);
    draw->AddRectFilled(tile, ImVec2(tile.x + px(34), tile.y + px(34)),
                        selected ? with_alpha(p.accent, 0.16f) : p.raised);
    draw_icon(draw, guides[i].icon, ImVec2(tile.x + px(8), tile.y + px(8)), px(18),
              selected ? p.accent : p.muted);
    draw->AddText(fonts().strong, fonts().strong->LegacySize,
                  ImVec2(tile.x + px(46), row_y + (nav_row_h - fonts().strong->LegacySize) * 0.5f),
                  selected ? p.accent : p.ink, guides[i].title);
    if (selected || hovered) {
      draw_icon(draw, "chevronRight", ImVec2(start.x + nav_w - px(18), row_y + (nav_row_h - px(12)) * 0.5f), px(12),
                selected ? p.accent : p.muted, 1.4f);
    }
  }
  const float nav_bottom = top + guide_count * (nav_row_h + nav_gap) - nav_gap;

  // The foot is its own band: a rule across the sheet, then the secondary action at the left and the one primary
  // action at the right. In the column, Replay looked like an eighth section.
  const float footer_y = std::max(nav_bottom, y) + px(16);
  // A seam between the list and the guide: the two columns are a split, not one long card.
  const float seam_x = start.x + nav_w + gap * 0.5f;
  draw->AddLine(ImVec2(seam_x, top), ImVec2(seam_x, footer_y), p.line, 1.0f);
  draw->AddLine(ImVec2(start.x, footer_y), ImVec2(start.x + body_w, footer_y), p.line, 1.0f);
  // Both actions sit on one line: align_right adds its own top gap, which put Close lower than Replay.
  const float buttons_y = footer_y + px(12);
  ImGui::SetCursorScreenPos(ImVec2(start.x, buttons_y));
  if (dialog_button("Show first-launch guide##keys", Variant::normal, "refresh", false, 190)) {
    panel.welcomed = false;
    panel.guide_step = 0;
    panel.guide_replay = true;
    panel.deck_pad = 0;
    panel.deck_pad_seen = -1;
    panel.deck_fill_ready = false;
    panel.keys_open = false;
    env.shell.save_layout();
    close_sheet();
  }
  ImGui::SetCursorScreenPos(ImVec2(start.x + body_w - px(90), buttons_y));
  if (dialog_button("Close##keys", Variant::primary, nullptr, false, 90) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    panel.keys_open = false;
    close_sheet();
  }
  // An open guide sizes itself to its content. Once a section folds, the popup would keep its old height, so measure
  // the foot and let the next frame open at that size instead.
  const bool folded = !panel.help_open[0] || !panel.help_open[1] || !panel.help_open[2];
  panel.help_height = folded ? footer_y + px(12) + px(32) + px(12) - ImGui::GetWindowPos().y : 0;
  end_dialog();
}

// The whole control surface in one place: the settings rows are for changing a binding, this is for reading them.
void keybinds_sheet(ViewEnv& env) {
  struct Bind {
    const char* key;
    const char* what;
  };
  struct Group {
    const char* title;
    const Bind* binds;
    int count;
  };
  static const Bind panel_binds[] = {
      {"tab", "deck, clips, screenshots"}, {"shift+tab", "the other way"},
      {"up / down", "the deck's actions"}, {"enter / space", "press the action"},
      {"1-4", "apply a profile"},          {",", "settings"},
      {"f1 or ?", "this help"},            {"esc", "back, then hide"},
  };
  static const Bind library_binds[] = {
      {"arrows", "move over the cards"}, {"enter", "play or open"}, {"tab", "clips, screenshots"},
      {"e", "edit"},                     {"s", "share"},           {"c", "copy"},
      {"f", "show in folder"},           {"f2", "rename"},         {"del", "delete"},
  };
  static const Bind settings_binds[] = {
      {"up / down or 1-9", "another section"}, {"tab", "next field"}, {"esc", "back to the panel"},
  };
  static const Group groups[] = {
      {"panel", panel_binds, 8},
      {"library", library_binds, 9},
      {"settings", settings_binds, 3},
  };

  PanelState& panel = env.panel;
  if (!begin_dialog("##keybinds", "Keybinds", panel.keybinds_open, 900)) return;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Palette& p = pal();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float body_w = ImGui::GetContentRegionAvail().x;
  const float row_h = px(28);
  const float nav_h = px(22);
  const float key_w = px(132);
  const float col_gap = px(40);
  const float col_w = (body_w - col_gap) * 0.5f;
  const float left_x = start.x;
  const float right_x = start.x + col_w + col_gap;
  const float seam_x = start.x + col_w + col_gap * 0.5f;

  draw->AddText(fonts().small, fonts().small->LegacySize, start, p.muted,
                "Every way to control BuffrX from a keyboard, grouped by where it works.");
  float ly = start.y + fonts().small->LegacySize + px(12);
  float ry = ly;

  auto heading = [&](const char* title, float x, float& y) {
    y += px(10);
    draw->AddText(fonts().label, fonts().label->LegacySize, ImVec2(x, y), p.ink, title);
    const float text_w = text_width(fonts().label, title);
    draw->AddLine(ImVec2(x + text_w + px(10), y + fonts().label->LegacySize * 0.5f),
                  ImVec2(x + col_w, y + fonts().label->LegacySize * 0.5f), p.line, 1.0f);
    y += px(18);
  };
  auto nav_row = [&](const Bind& bind, float x, float& y) {
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(x + px(8), y + px(4)), p.accent, bind.key);
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(x + key_w + px(8), y + px(4)), p.muted, bind.what);
    y += nav_h;
    draw->AddLine(ImVec2(x, y), ImVec2(x + col_w, y), with_alpha(p.line, 0.35f), 1.0f);
  };

  heading("hotkeys", left_x, ly);
  draw->AddText(fonts().label, fonts().label->LegacySize, ImVec2(left_x + px(8), ly), p.muted, "action");
  draw->AddText(fonts().label, fonts().label->LegacySize, ImVec2(left_x + px(160), ly), p.muted, "keyboard");
  draw->AddText(fonts().label, fonts().label->LegacySize, ImVec2(left_x + px(270), ly), p.muted, "controller");
  ly += px(18);
  for (const HotkeyDefault& row : kHotkeyDefaults) {
    const char* action = row.action;
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(left_x + px(8), ly + px(6)), p.ink,
                  hotkey_label(action).c_str());
    const std::string key = env.store.chord(action);
    const std::string pad = env.store.chord(action, true);
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(left_x + px(160), ly + px(6)),
                  key.empty() ? p.faint : p.ink, key.empty() ? "unset" : key.c_str());
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(left_x + px(270), ly + px(6)),
                  pad.empty() ? p.faint : p.muted, pad.empty() ? "unset" : pad.c_str());
    ly += row_h;
    draw->AddLine(ImVec2(left_x, ly), ImVec2(left_x + col_w, ly), with_alpha(p.line, 0.45f), 1.0f);
  }
  heading("panel", left_x, ly);
  for (int i = 0; i < groups[0].count; ++i) nav_row(groups[0].binds[i], left_x, ly);

  heading("library", right_x, ry);
  for (int i = 0; i < groups[1].count; ++i) nav_row(groups[1].binds[i], right_x, ry);
  heading("settings", right_x, ry);
  for (int i = 0; i < groups[2].count; ++i) nav_row(groups[2].binds[i], right_x, ry);

  const float body_bottom = std::max(ly, ry);
  draw->AddLine(ImVec2(seam_x, start.y + fonts().small->LegacySize + px(12)), ImVec2(seam_x, body_bottom), p.line, 1.0f);
  const float footer_y = body_bottom + px(14);
  draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(start.x, footer_y), p.muted,
                "Click a row in Settings > Hotkeys to change a hotkey.");
  const float buttons_y = footer_y + px(24);
  ImGui::SetCursorScreenPos(ImVec2(start.x + body_w - px(90), buttons_y));
  if (dialog_button("Close##keybinds", Variant::primary, nullptr, false, 90) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    panel.keybinds_open = false;
    close_sheet();
  }
  end_dialog();
}

void media_dialogs(ViewEnv& env) {
  keys_sheet(env);
  keybinds_sheet(env);
  share_sheet(env);
  receive_sheet(env);
  link_offer_sheet(env);
  PanelState& panel = env.panel;
  if (begin_dialog("##stopbuffer", "Stop buffer?", panel.buffer_stop_open, 560)) {
    text_wrapped(fonts().body, pal().muted,
                 "Stopping the buffer also ends this Timeline session. Footage already recorded stays in Timeline and can still be saved.",
                 ImGui::GetContentRegionAvail().x);
    // The compact panel is narrower than the two buttons together: there they take the sheet's width and stack,
    // so neither label is cut off at the border.
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float avail = std::floor(ImGui::GetContentRegionAvail().x);
    const bool stacked = avail < px(150) + gap + px(270);
    const float keep_w = stacked ? avail : px(150);
    const float stop_w = stacked ? avail : px(270);
    align_right(stacked ? keep_w : keep_w + gap + stop_w);
    if (dialog_button("Keep buffer on##stopbuffer", Variant::ghost, nullptr, false, unpx(keep_w)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      panel.buffer_stop_open = false;
      close_sheet();
    } else {
      if (!stacked) ImGui::SameLine();
      if (dialog_button("Stop buffer and end Timeline##stopbuffer", Variant::danger, nullptr, false, unpx(stop_w))) {
        panel.buffer_stop_open = false;
        close_sheet();
        env.store.attempt("buffer.stop", Json::object(), "The buffer did not stop",
                          [&store = env.store](const Json& status) { store.status = status; });
      }
    }
    end_dialog();
  }
  if (begin_dialog("##rename", panel.renaming.clip ? "Rename clip" : "Rename screenshot", panel.rename_open)) {
    const std::string name = file_name(panel.renaming.file);
    const std::string extension = name.substr(file_stem(panel.renaming.file).size());
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const float field = ImGui::GetContentRegionAvail().x - text_width(fonts().body, extension) - px(10);
    ImGui::PushFont(fonts().body);
    ImGui::SetNextItemWidth(field);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, pal().canvas);
    const bool enter = ImGui::InputText("##name", &panel.rename_text, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (ImGui::IsItemEdited()) panel.rename_problem_text = rename_problem(panel.rename_text);
    ImGui::SameLine();
    text(fonts().body, pal().faint, extension);
    if (!panel.rename_problem_text.empty()) text(fonts().small, pal().danger, panel.rename_problem_text);
    align_right(px(180));
    if (dialog_button("Cancel##rename", Variant::ghost) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      panel.rename_open = false;
      close_sheet();
    } else {
      ImGui::SameLine();
      const std::string problem = rename_problem(panel.rename_text);
      if ((dialog_button(panel.rename_busy ? "Saving\xE2\x80\xA6##rename" : "Rename##rename", Variant::primary, nullptr,
                         panel.rename_busy || !problem.empty()) ||
           (enter && problem.empty())) && !panel.rename_busy) {
        panel.rename_busy = true;
        std::string trimmed = panel.rename_text;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
        Json args = Json::object();
        args.set("file", Json(panel.renaming.file));
        args.set("name", Json(trimmed));
        env.store.bridge_call(panel.renaming.clip ? "clips.rename" : "screenshots.rename", args,
                              [&panel = env.panel, &store = env.store](bool ok, const Json& reply, const std::string&) {
                                panel.rename_busy = false;
                                if (!ok) {
                                  panel.rename_problem_text = "That name is taken, or the file is in use";
                                  return;
                                }
                                // The same file under a new name: every place holding its path follows it, so an open preview
                                // keeps its picture instead of taking the old path for a file that is gone.
                                const std::string renamed = reply["file"].as_string();
                                // The lists are replaced only when the new listing lands; until then the entry moves with the file, or
                                // the open preview looks the new path up in a list that still holds the old one and closes.
                                const auto follow = [&](std::vector<UiMedia>& list) {
                                  for (UiMedia& media : list) {
                                    if (media.file != panel.renaming.file) continue;
                                    media.file = renamed;
                                    media.name = file_stem(renamed);
                                  }
                                };
                                follow(store.clips);
                                follow(store.screenshots);
                                for (std::string& file : panel.preview_files) {
                                  if (file == panel.renaming.file) file = renamed;
                                }
                                if (store.newest == panel.renaming.file) store.newest = renamed;
                                panel.renaming.file = renamed;
                                panel.rename_text = file_stem(renamed);
                                panel.rename_open = false;
                                store.refresh_library(0);
                              });
      }
    }
    // The popup closes when the state says so. `IsPopupOpen("##rename")` answered false here: a popup
  // name resolves against the current ID stack, so the check never matched and the dialog stayed on
  // screen (and kept every panel key to itself) after the rename had already been saved.
  if (!panel.rename_open) close_sheet();
    end_dialog();
  }

  if (begin_dialog("##delete", "Delete", panel.remove_open)) {
    text_wrapped(fonts().body, pal().muted, file_name(panel.removing.file) + " moves to the Recycle Bin.",
                 ImGui::GetContentRegionAvail().x);
    align_right(px(170));
    if (dialog_button("Keep it##delete", Variant::ghost) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      panel.remove_open = false;
      close_sheet();
    } else {
      ImGui::SameLine();
      // Enter answers the question the same as the button: every other action in this panel is reachable
      // from the keyboard, and Escape already means "keep it".
      if (dialog_button("Delete##delete", Variant::danger) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        Json args = Json::object();
        args.set("file", Json(panel.removing.file));
        const std::string file = panel.removing.file;
        env.store.attempt(panel.removing.clip ? "clips.trash" : "screenshots.trash", args, "Not deleted",
                          [&store = env.store](const Json&) { store.refresh_library(0); });
        // A deleted screenshot leaves the preview too.
        if (panel.preview_open) {
          auto& files = panel.preview_files;
          const auto at = std::find(files.begin(), files.end(), file);
          if (at != files.end()) {
            const int removed = static_cast<int>(at - files.begin());
            files.erase(at);
            if (removed < panel.preview_index) --panel.preview_index;  // keep the picture being looked at
          }
          if (files.empty()) panel.preview_open = false;
          else panel.preview_index = std::clamp(panel.preview_index, 0, static_cast<int>(files.size()) - 1);
        }
        panel.remove_open = false;
        close_sheet();
      }
    }
    end_dialog();
  }
}

// The height the notes need at `width`, in design units, so a short changelog does not open a mostly
// empty sheet and a long one still scrolls inside the cap. A bullet's text sits behind a marker, so it
// wraps in the room the marker leaves.
float notes_height(const std::vector<NoteRow>& rows, float width) {
  ImGui::PushFont(fonts().body);
  const float spacing = ImGui::GetStyle().ItemSpacing.y;
  float total = 0;
  for (const NoteRow& row : rows) {
    if (row.kind == NoteKind::heading) total += px(12);
    const float room = row.kind == NoteKind::bullet ? width - px(12) : width;
    total += ImGui::CalcTextSize(row.text.c_str(), nullptr, false, room).y + spacing;
    if (row.kind == NoteKind::bullet) total += px(3) + spacing;
  }
  ImGui::PopFont();
  return unpx(total);
}

void update_restart_dialog(ViewEnv& env) {
  if (!kAutoUpdate) return;
  PanelState& panel = env.panel;
  const Json& update = env.store.status["update"];
  if (update["state"].as_string() != "ready") {
    panel.update_restart_open = false;
    return;
  }
  const std::string version = update["version"].as_string();
  if (version.empty()) return;
  if (!panel.update_restart_open) {
    const std::string running = env.store.hello["engine"].as_string();
    if (!update_should_prompt(version, panel.update_prompted, running)) return;
    panel.update_prompted = version;
    panel.update_restart_open = true;
  }
  if (!begin_dialog("##update_restart", "Update ready", panel.update_restart_open, 400)) return;
  const float width = ImGui::GetContentRegionAvail().x;
  text_wrapped(fonts().body, pal().muted, "BuffrX " + version + " is ready to install.", width);
  ImGui::Dummy(ImVec2(0, px(8)));
  const bool recording = env.store.status["recording"].as_bool();
  if (recording) {
    text_wrapped(fonts().body, pal().accent,
                 "Stop the recording first: the update would close BuffrX in the middle of the file.", width);
    ImGui::Dummy(ImVec2(0, px(6)));
  }
  align_right(px(150) + px(8) + px(96));
  if (dialog_button("Restart now##update_restart", Variant::primary, "refresh", recording, 150)) {
    panel.update_restart_open = false;
    env.shell.install_update();
  }
  ImGui::SameLine(0, px(8));
  if (dialog_button("Later##update_later", Variant::normal, nullptr, false, 96) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    panel.update_restart_open = false;
    panel.update_offered = version;
    env.shell.save_layout();
  }
  end_dialog();
}

void whats_new(ViewEnv& env) {
  PanelState& panel = env.panel;
  const std::string running = env.store.hello.is_null() ? std::string() : env.store.hello["engine"].as_string();
  if (running.empty()) return;  // no hello yet: the next frame that has one decides
  if (!panel.notes_checked) {
    panel.notes_checked = true;
    // A version already announced (the sheet closed in an earlier run) leaves nothing pending. An
    // installer that wrote it before relaunching keeps version_seen behind, so the sheet still opens.
    if (panel.pending_version == running && panel.version_seen == running) {
      panel.pending_version.clear();
      panel.pending_notes.clear();
      env.shell.save_layout();
    }
    if (panel.version_seen.empty() && panel.pending_version.empty()) {
      // A fresh install has no earlier version to compare with, so there is nothing to announce.
      panel.version_seen = running;
      env.shell.save_layout();
    } else if (panel.pending_version.empty() && panel.version_seen != running) {
      // A silent install leaves the new version running before the app ever saw it.
      panel.pending_version = running;
      const Version latest = parse_version(panel.update_latest), current = parse_version(running);
      panel.notes_available = latest.valid && current.valid && compare_versions(latest, current) == 0;
      panel.pending_notes = panel.notes_available ? panel.update_notes : std::string();
      env.shell.save_layout();
    }
    panel.notes_open = update_should_open_notes(running, panel.version_seen, panel.pending_version);
  }
  if (!panel.notes_open) return;

  const std::string version = panel.pending_version;
  const std::vector<NoteRow> rows = note_rows(panel.notes_available ? panel.pending_notes : std::string(), 200);
  const float height = std::clamp(panel.notes_available ? notes_height(rows, px(380)) + 140.0f : 230.0f, 230.0f, 460.0f);
  const std::string title = version.empty() ? std::string("What's new") : "What's new in " + version;
  if (!begin_dialog("##whats_new", title, panel.notes_open, 460, height, 46)) return;
  if (!panel.notes_open) {  // the sheet is on its exit fade; its owner has stopped drawing it
    close_sheet();
    end_dialog();
    return;
  }

  const ImU32 muted = pal().muted, ink = pal().ink, accent = pal().accent;
  const float width = ImGui::GetContentRegionAvail().x;
  if (panel.notes_loading) {
    text(fonts().body, muted, "Loading release notes\xE2\x80\xA6");
  } else if (!panel.notes_available) {
    text_wrapped(fonts().body, muted,
                 "This build's release notes are not on this PC yet. Load them from GitHub, or read them on the "
                 "release page.",
                 width);
  } else if (rows.empty()) {
    text_wrapped(fonts().body, muted, "This release has no notes.", width);
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    const NoteRow& row = rows[i];
    if (row.kind == NoteKind::heading && i > 0) ImGui::Dummy(ImVec2(0, px(12)));
    if (row.kind == NoteKind::bullet) {
      // A square marker in the accent, and the text hung behind it so a wrapped line stays in its column.
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + px(7)), ImVec2(at.x + px(4), at.y + px(11)), accent);
      ImGui::Indent(px(12));
      text_wrapped(!row.bold.empty() ? fonts().strong : fonts().body, ink, row.text, width - px(12));
      ImGui::Unindent(px(12));
    } else if (row.kind == NoteKind::heading) {
      text_wrapped(fonts().strong, accent, row.text, width);
    } else {
      text_wrapped(!row.bold.empty() ? fonts().strong : fonts().body, muted, row.text, width);
    }
    if (row.kind == NoteKind::bullet) ImGui::Dummy(ImVec2(0, px(3)));
  }

  align_right(px(120) + px(96) + ImGui::GetStyle().ItemSpacing.x);
  if (panel.notes_available || panel.notes_loading) {
    if (dialog_button("Release page##notes", Variant::ghost, "external", false, 120)) {
      Json args = Json::object();
      args.set("url", Json(kReleasesUrl));
      env.store.attempt("app.open_url", args, "The page did not open");
    }
  } else if (dialog_button("Load notes##notes", Variant::ghost, "refresh", false, 120)) {
    panel.notes_loading = true;
    PanelState* state = &panel;
    UiShell* shell = &env.shell;
    env.store.bridge_call("update.notes", Json::object(),
                          [state, shell, version](bool ok, const Json& data, const std::string&) {
                            state->notes_loading = false;
                            if (state->pending_version != version) return;  // another version took the sheet
                            state->notes_available = ok && data["found"].as_bool();
                            if (state->notes_available) state->pending_notes = data["notes"].as_string();
                            shell->save_layout();
                          });
  }
  ImGui::SameLine();
  if (dialog_button("Close##notes", Variant::primary, "check", false, 96) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    panel.version_seen = running;  // announced once, never again for this version
    panel.notes_open = false;      // begin_dialog keeps the sheet up for its exit fade
    env.shell.save_layout();
    close_sheet();
    end_dialog();
    return;
  }
  end_dialog();
}

}  // namespace bx::ui::parts

