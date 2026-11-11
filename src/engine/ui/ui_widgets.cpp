#include "ui_widgets.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include "ui_anim.h"
#include "ui_format.h"
#include "ui_icons.h"

namespace bx::ui {
namespace {

// The palette by value: set_theme is handed the frame's own copy, so pal() reads a live palette even outside the frame.
Palette g_palette{};
ImGuiID g_row_width_id = 0;      // the row begin_row opened, whose controls end_row measures
std::string g_row_hint;          // its description, shown as a tooltip while the row is hovered
ImVec2 g_row_min;                // where the row starts, for that hover test
bool g_layout_settling = false;  // a row moved this frame: draw one more
const Fonts* g_fonts = nullptr;
float g_scale = 1.0f;


const std::vector<PathSegment>& icon_segments(const std::string& name) {
  static std::map<std::string, std::vector<PathSegment>> cache;
  auto found = cache.find(name);
  if (found != cache.end()) return found->second;
  return cache.emplace(name, parse_svg_path(icon_path(name))).first->second;
}

// A centred string in a box.
void centred_text(ImDrawList* draw, ImFont* font, ImU32 color, ImVec2 min, ImVec2 max, const std::string& value) {
  const ImVec2 size = font->CalcTextSizeA(font->LegacySize, FLT_MAX, 0, value.c_str());
  draw->AddText(font, font->LegacySize, ImVec2(std::floor((min.x + max.x - size.x) / 2), std::floor((min.y + max.y - size.y) / 2)),
                color, value.c_str());
}

// Pane titles, field labels and key legends are set in lower case, as a terminal sets them.
std::string lower(const std::string& value) { return lower_ascii(value); }

// A colour part of the way from one to another; a transparent end fades in or out instead of passing through black.
ImU32 blend(ImU32 from, ImU32 to, float t) {
  if (t <= 0.0f) return from;
  if (t >= 1.0f) return to;
  if (!from) return with_alpha(to, t * to_vec(to).w);
  if (!to) return with_alpha(from, (1.0f - t) * to_vec(from).w);
  return mix(from, to, t);
}

// A pane's border, with its title (and aside) set into the top edge: the seam stops short of the words.
void pane_frame(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::string& title, const std::string& aside, bool focused,
                ImGuiID key, bool highlight = false) {
  const Palette& p = pal();
  // Focus is a state, not a decoration: while a dialog, a menu or a list is open, it has the focus, so no
  // pane under it keeps the accent border and only one is ever on screen.
  focused = focused && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  // The accent frame slides from one pane to the next: each pane fades its own in and out.
  // Keyed by the pane's own item (the child's id, stable while a list scrolls): two panes with no title must not share a value,
  // or they would pull it back and forth for ever and the window would never go idle.
  const float lit = anim_to(key ^ 0x7a11u, focused ? 1.0f : 0.0f, kRateHover);
  const float border = 1.0f + (px(2) - 1.0f) * lit;
  const ImU32 edge = blend(p.line, p.accent, lit);
  draw->AddRectFilled(min, max, p.surface);
  draw->AddRect(ImVec2(min.x + border / 2, min.y + border / 2), ImVec2(max.x - border / 2, max.y - border / 2), edge, 0, 0, border);
  ImFont* font = fonts().label;
  const float gap = px(5);
  auto inset = [&](float x, const std::string& value, ImU32 color) {
    const float w = text_width(font, value);
    draw->AddRectFilled(ImVec2(x - gap, min.y), ImVec2(x + w + gap, min.y + border + 1), p.surface);
    draw->AddText(font, font->LegacySize, ImVec2(x, std::floor(min.y - font->LegacySize / 2 - px(1))), color, value.c_str());
  };
  if (!title.empty()) {
    const float x = min.x + px(12);
    inset(x, title, highlight ? p.accent : blend(p.muted, p.accent, lit));
  }
  if (!aside.empty()) inset(max.x - px(12) - text_width(font, aside), aside, p.muted);
}

struct OpenPane {
  std::string title, aside;
  bool focused = false;
  bool hover = false;
  bool highlight = false;
};
std::vector<OpenPane>& open_panes() {
  static std::vector<OpenPane> panes;
  return panes;
}

}  // namespace

void set_theme(const Palette* palette, const Fonts* fonts_in, float scale) {
  if (palette) g_palette = *palette;
  g_fonts = fonts_in;
  g_scale = scale;
}
const Palette& pal() { return g_palette; }
bool layout_settling() {
  const bool settling = g_layout_settling;
  g_layout_settling = false;
  return settling;
}
const Fonts& fonts() { return *g_fonts; }
float px(float value) { return std::round(value * g_scale); }
float unpx(float value) { return value / g_scale; }

// ---------------------------------------------------------------- drawing

void draw_icon(ImDrawList* draw, const std::string& name, ImVec2 min, float size, ImU32 color, float stroke, bool fill) {
  const std::vector<PathSegment>& segments = icon_segments(name);
  if (segments.empty()) return;
  const float k = size / 24.0f;
  const float thickness = std::max(1.0f, stroke * k * 1.05f);
  auto at = [&](float x, float y) { return ImVec2(min.x + x * k, min.y + y * k); };
  ImVec2 start{}, last{};
  float length = 0;
  bool open = false;
  auto finish = [&](bool closed) {
    if (!open) return;
    if (fill) {
      draw->PathFillConvex(color);
    } else if (length < 0.2f) {
      // "M12 12h.01": SVG draws a round dot with a round cap; so does this.
      draw->PathClear();
      draw->AddCircleFilled(start, thickness * 0.75f, color, 12);
    } else {
      draw->PathStroke(color, closed ? ImDrawFlags_Closed : 0, thickness);
      if (!closed) {
        // Round caps, as the SVG has them.
        draw->AddCircleFilled(start, thickness * 0.5f, color, 8);
        draw->AddCircleFilled(last, thickness * 0.5f, color, 8);
      }
    }
    open = false;
    length = 0;
  };
  for (const PathSegment& s : segments) {
    switch (s.kind) {
      case PathSegment::move:
        finish(false);
        start = last = at(s.x, s.y);
        draw->PathClear();
        draw->PathLineTo(start);
        open = true;
        break;
      case PathSegment::line: {
        const ImVec2 point = at(s.x, s.y);
        length += std::hypot(point.x - last.x, point.y - last.y) / k;
        draw->PathLineTo(point);
        last = point;
        break;
      }
      case PathSegment::cubic: {
        const ImVec2 point = at(s.x, s.y);
        length += std::hypot(point.x - last.x, point.y - last.y) / k + 1.0f;
        // Explicit segments: at icon sizes the automatic tessellation turns a dot into a square.
        draw->PathBezierCubicCurveTo(at(s.c1x, s.c1y), at(s.c2x, s.c2y), point, 10);
        last = point;
        break;
      }
      case PathSegment::close:
        finish(true);
        last = start;
        break;
    }
  }
  finish(false);
}

namespace {

// The mark's sampled edges, once the interface's device has them (set_mark_texture).
ImTextureID mark_texture = 0;

// The mark's triangles (cut once, ui_icons.h) filled with `color` at `at(point)`, then its outline stroked with the same colour:
// the triangles give the shape and its notches, the stroke softens every edge, so the titlebars look as smooth as the page.
template <typename Place>
void fill_mark(ImDrawList* draw, ImU32 color, const Place& at) {
  static const std::vector<std::array<int, 3>> triangles = triangulate(mark_outline());
  static std::vector<ImVec2> points;
  const std::vector<MarkPoint>& outline = mark_outline();
  points.clear();
  points.reserve(outline.size());
  for (const MarkPoint& p : outline) points.push_back(at(p));
  for (const std::array<int, 3>& t : triangles) {
    ImVec2 a = points[static_cast<size_t>(t[0])], b = points[static_cast<size_t>(t[1])], c = points[static_cast<size_t>(t[2])];
    if ((c.x - b.x) * (b.y - a.y) - (b.x - a.x) * (c.y - b.y) <= 0) std::swap(b, c);
    draw->AddTriangleFilled(a, b, c, color);
  }
  draw->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_Closed, 1.0f);
}

}  // namespace

void set_mark_texture(ImTextureID texture) { mark_texture = texture; }

void draw_mark(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
  // `size` is the slot the titlebars reserve, drawn as the editor draws the same path: its SVG box (-18 35 360 360) scaled to
  // `size` square, so the mark is 0.88 of it wide. The path's box is x 3.2-320.5, y 93-336.4.
  const float scale = size / 360.0f;
  fill_mark(draw, color, [&](const MarkPoint& p) {
    return ImVec2(center.x + (p.x - 161.85f) * scale, center.y + (p.y - 214.7f) * scale);
  });
}

void draw_app_icon(ImDrawList* draw, ImVec2 center, float size) {
  // The mark in the text colour, as the editor's titlebar draws it: no tile, no shading. The panel, the quick
  // panel and the tray menu wear the same brand as the pages, and every theme paints it from `ink`.
  if (mark_texture) {
    // A sampled bitmap, so the edges are the browser's: the triangles alone stair-step at 18 units.
    const float half = size / 2;
    draw->AddImage(mark_texture, ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
                   ImVec2(0, 0), ImVec2(1, 1), pal().ink);
    return;
  }
  draw_mark(draw, center, size, pal().ink);
}

void spinner(ImDrawList* draw, ImVec2 center, float radius, ImU32 color) {
  // Eight cells around a square with a bright head and a tail fading behind it. The head moves continuously, so each
  // cell brightens and dims smoothly instead of stepping.
  const float head = static_cast<float>(std::fmod(ImGui::GetTime() / 0.72, 1.0)) * 8.0f;
  static const float kx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
  static const float ky[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
  const float cell = std::max(2.0f, std::round(radius * 0.55f));
  for (int i = 0; i < 8; ++i) {
    float behind = head - static_cast<float>(i);  // how far the head is past this cell, in cells
    if (behind < 0) behind += 8.0f;
    const float alpha = std::max(0.12f, 1.0f - behind / 3.2f);
    const ImVec2 at(center.x + kx[i] * cell * 1.3f - cell / 2, center.y + ky[i] * cell * 1.3f - cell / 2);
    draw->AddRectFilled(at, ImVec2(at.x + cell, at.y + cell), with_alpha(color, alpha));
  }
  anim_touch_slow();  // it moves: the window keeps drawing (about 30 a second) while it is on screen
}

// ---------------------------------------------------------------- text

void text(ImFont* font, ImU32 color, const std::string& value) {
  ImGui::PushFont(font);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(value.c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

float text_width(ImFont* font, const std::string& value) {
  return font->CalcTextSizeA(font->LegacySize, FLT_MAX, 0, value.c_str()).x;
}

void text_ellipsis(ImFont* font, ImU32 color, const std::string& value, float max_width) {
  ImGui::PushFont(font);
  const ImVec2 size = ImGui::CalcTextSize(value.c_str());
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float width = std::max(1.0f, std::min(size.x, max_width));
  ImGui::Dummy(ImVec2(width, size.y));
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + width, pos.y + size.y), pos.x + width,
                            value.c_str(), nullptr, &size);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (size.x > max_width && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", value.c_str());
}

void text_wrapped(ImFont* font, ImU32 color, const std::string& value, float width) {
  ImGui::PushFont(font);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::TextUnformatted(value.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

float label(const std::string& value, ImU32 color) {
  const std::string shown = lower(value);
  text(fonts().label, color ? color : pal().muted, shown);
  return text_width(fonts().label, shown);
}

namespace {
ImVec2 g_keepout_min(0, 0), g_keepout_max(0, 0);
// The card a hint is drawn in, shared by the item-hover and rectangle-hover forms.
void tooltip_body(const std::string& value, float wrap_width) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(5)));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, pal().raised);
  ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
  ImGui::PushFont(fonts().small);
  // The card keeps a margin from the window's edges and flips to the pointer's left when the right has no
  // room: ImGui clamps to the screen, which here is the window's own edge, so a hint beside a right-hand
  // control used to sit flush against it.
  const ImVec2 win = ImGui::GetMainViewport()->WorkPos;
  const ImVec2 win_size = ImGui::GetMainViewport()->WorkSize;
  const float wrap = wrap_width > 0 ? px(wrap_width) : 0.0f;
  const ImVec2 text_size = ImGui::CalcTextSize(value.c_str(), nullptr, false, wrap);
  const ImVec2 size(text_size.x + px(18), text_size.y + px(12));
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float edge = px(6);
  float x = mouse.x + px(14);
  if (x + size.x > win.x + win_size.x - edge) x = mouse.x - size.x - px(14);
  x = std::max(win.x + edge, x);
  float y = mouse.y + px(18);
  y = std::max(win.y + edge, std::min(y, win.y + win_size.y - size.y - edge));
  // Under a child window (the clip page) the card would be hidden: put it above the pointer, where the header is.
  if (g_keepout_max.x > g_keepout_min.x && x < g_keepout_max.x && x + size.x > g_keepout_min.x && y < g_keepout_max.y &&
      y + size.y > g_keepout_min.y) {
    y = std::max(win.y + edge, mouse.y - size.y - px(10));
  }
  ImGui::SetNextWindowPos(ImVec2(x, y));
  // A5: the tooltip fades in (no travel) on the fast token; it starts over whenever it was not shown on the frame before.
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear(ImGui::GetID("##tooltip"), kDurFast));
  // A disabled row can also have its own hint. Replace that tooltip instead of appending a second
  // positioned card to the same ImGui window, which clips both messages.
  if (ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None)) {
    // A long hint wraps into a readable column instead of running across the screen as one line.
    if (wrap_width > 0) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(value.c_str());
    if (wrap_width > 0) ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  ImGui::PopStyleVar();
  ImGui::PopFont();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
}
}  // namespace

void hint_keepout(ImVec2 min, ImVec2 max) {
  g_keepout_min = min;
  g_keepout_max = max;
}

void tooltip(const std::string& value, float wrap_width) {
  if (value.empty()) return;
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
    tooltip_body(value, wrap_width);
  }
}

void tooltip_rect(ImVec2 min, ImVec2 max, const std::string& value, float wrap_width) {
  if (value.empty()) return;
  // A group of disabled controls has no hovered item of its own, so the hint follows the area instead.
  if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || !ImGui::IsMouseHoveringRect(min, max)) return;
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
  tooltip_body(value, wrap_width);
}

// ---------------------------------------------------------------- panes

void begin_pane(const char* id, const std::string& title, ImVec2 size, bool focused, const std::string& aside,
                ImGuiWindowFlags flags, bool hover, float padding_y, bool highlight) {
  OpenPane pane{lower(title), aside, focused, hover, highlight};
  open_panes().push_back(pane);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(padding_y)));
  ImGuiChildFlags child = ImGuiChildFlags_AlwaysUseWindowPadding;
  if (size.y == 0) child |= ImGuiChildFlags_AutoResizeY;
  ImGui::BeginChild(id, size, child, flags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void end_pane(float trim_bottom) {
  const OpenPane pane = open_panes().back();
  open_panes().pop_back();
  // Rows place themselves with SetCursorScreenPos, and ImGui 1.92 no longer lets a placed cursor grow the pane by itself (it
  // reports an error on screen). This is the growth ImGui 1.91 applied (its own "old behavior", imgui.cpp
  // ErrorCheckUsingSetCursorPosToExtendParentBoundaries), so every pane keeps the size it had.
  if (ImGuiWindow* window = ImGui::GetCurrentWindow(); window->DC.IsSetPos) {
    window->DC.CursorMaxPos = ImMax(window->DC.CursorMaxPos, window->DC.CursorPos);
    window->DC.IsSetPos = false;
  }
  // A card whose last row is one short line ends closer to it than the pane's padding would leave it.
  if (trim_bottom > 0) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    window->DC.CursorMaxPos.y -= px(trim_bottom);
    window->ContentSize.y = std::max(0.0f, window->ContentSize.y - px(trim_bottom));
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  // A pane that is one target lights its border from anywhere on it, so the pointer says the whole face is pressable.
  const bool hovered = pane.hover && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  // The parent's draw list renders before the child's, so the face and the border lie under the content.
  pane_frame(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), pane.title, pane.aside,
             pane.focused || hovered, ImGui::GetItemID(), pane.highlight);
}

void draw_pane(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::string& title, bool focused, const std::string& aside) {
  // No item of its own: the cursor (window-local, so it holds still while the list scrolls) and the title make the key.
  const ImVec2 at = ImGui::GetCursorPos();
  const int place[2] = {static_cast<int>(at.x), static_cast<int>(at.y)};
  pane_frame(draw, min, max, lower(title), aside, focused, ImHashData(place, sizeof(place), ImGui::GetID(title.c_str())));
}

// ---------------------------------------------------------------- meters

void cells(float width, float value, int count, float height, ImU32 color) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = px(width), h = px(height), gap = std::max(1.0f, px(2));
  ImGui::Dummy(ImVec2(w, h));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float cell = (w - gap * (count - 1)) / count;
  const int lit = static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * count));
  for (int i = 0; i < count; ++i) {
    const float x = std::floor(pos.x + i * (cell + gap));
    draw->AddRectFilled(ImVec2(x, pos.y), ImVec2(std::floor(x + cell), pos.y + h), i < lit ? (color ? color : pal().accent) : pal().raised);
  }
}

void vcells(float height, float value, int count, float width) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = px(width), h = px(height), gap = std::max(1.0f, px(2));
  ImGui::Dummy(ImVec2(w, h));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float cell = (h - gap * (count - 1)) / count;
  const int lit = static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * count));
  for (int i = 0; i < count; ++i) {
    const float y = std::floor(pos.y + h - (i + 1) * cell - i * gap);
    const ImU32 on = i >= count * 7 / 8 ? pal().danger : i >= count * 3 / 4 ? pal().warn : pal().ok;
    draw->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + w, std::floor(y + cell)), i < lit ? on : pal().raised);
  }
}


// ---------------------------------------------------------------- controls

bool icon_button(const char* id, const char* icon, const std::string& tip, bool active, float size, float icon_size,
                 bool danger_hover, bool danger) {
  const float side = px(size);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(side, side), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered();
  const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  const float press = press_t();  // A2: the face darkens and the glyph sinks a pixel while the button is held
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImU32 rest = danger ? pal().danger : active ? pal().accent : pal().muted;
  const ImU32 lit = danger ? pal().danger : danger_hover ? pal().on_accent : active ? pal().accent : pal().ink;
  ImU32 fg = blend(rest, lit, hover);
  if (hover > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + side, pos.y + side), blend(0, danger_hover ? pal().danger : pal().raised, hover));
  if (press > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + side, pos.y + side), blend(0, pal().canvas, 0.25f * press));
  const float glyph = px(icon_size);
  const float sink = px(1) * press;
  draw_icon(draw, icon, ImVec2(pos.x + (side - glyph) / 2 + sink, pos.y + (side - glyph) / 2 + sink), glyph, fg);
  tooltip(tip);
  return pressed;
}

bool button(const char* label_text, Variant variant, const char* icon, bool disabled, float width, float height,
            bool focus_ring) {
  ImFont* font = fonts().strong;
  const float h = px(height);
  const float glyph = px(15);
  std::string visible = label_text;
  const size_t hash = visible.find("##");
  if (hash != std::string::npos) visible = visible.substr(0, hash);
  const float label_w = visible.empty() ? 0 : text_width(font, visible);
  const float content = label_w + (icon ? glyph + (visible.empty() ? 0 : px(8)) : 0);
  const float w = width > 0 ? px(width) : content + px(visible.empty() ? 17 : 24);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(disabled);
  const bool pressed = ImGui::InvisibleButton(label_text, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered() && !disabled;
  const bool focused = ImGui::IsItemFocused() && !disabled;
  const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  const float press = press_t();  // A2: the face darkens and sinks a pixel while it is held, and rises again as it is let go
  const float disabled_t = anim_item(disabled ? 1.0f : 0.0f, kRateState, 0x5d15u);  // A4: a disabled control fades to 45% opacity
  ImGui::EndDisabled();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Palette& p = pal();
  // Each variant is a resting look and a hovered look; the button moves between them.
  ImU32 bg0 = p.surface, bg1 = p.raised, border0 = p.line_strong, border1 = p.muted, fg0 = p.ink, fg1 = p.ink;
  switch (variant) {
    case Variant::primary:
      bg0 = p.accent;
      bg1 = mix(p.accent, p.ink, 0.12f);
      border0 = border1 = 0;
      fg0 = fg1 = p.on_accent;
      break;
    case Variant::ghost:
      bg0 = 0;
      bg1 = p.raised;
      border0 = border1 = 0;
      fg0 = p.muted;
      fg1 = p.ink;
      break;
    case Variant::danger:
      bg0 = 0;
      bg1 = p.danger;
      border0 = border1 = p.danger;
      fg0 = p.danger;
      fg1 = p.on_accent;
      break;
    default:
      break;
  }
  ImU32 bg = blend(bg0, bg1, hover), border = blend(border0, border1, hover), fg = blend(fg0, fg1, hover);
  if (press > 0.0f && variant != Variant::ghost) bg = blend(bg ? bg : p.raised, mix(bg ? bg : p.raised, p.canvas, 0.2f), press);
  if (disabled && variant == Variant::primary) {
    // A primary that cannot be pressed yet keeps its place but not its colour: a faded accent read as a
    // different, muddy button. It is a quiet raised cell with muted text until it can be pressed.
    bg = p.raised;
    border = p.line;
    fg = p.muted;
  }
  const float alpha = variant == Variant::primary ? 1.0f : 1.0f - 0.55f * disabled_t;
  const float inset = px(1) * press;  // the face sinks a pixel while it is held (A2)
  const ImVec2 min(pos.x + inset, pos.y + inset), max(pos.x + w - inset, pos.y + h - inset);
  if (bg) draw->AddRectFilled(min, max, with_alpha(bg, alpha * to_vec(bg).w));
  if (border) draw->AddRect(min, max, with_alpha(border, alpha * to_vec(border).w));
  // A3: the keyboard's place, the same accent frame a focused pane wears: it closes in on the control and fades up on the hover token
  // instead of switching on.
  const float ring = anim_item(focused && focus_ring ? 1.0f : 0.0f, kRateHover, 0x4f0cu);
  if (ring > 0.01f) {
    const float off = px(2) + px(5) * (1.0f - ring);
    draw->AddRect(ImVec2(min.x - off, min.y - off), ImVec2(max.x + off, max.y + off), with_alpha(p.accent, ring), 0, 0, px(2));
  }
  float x = min.x + (w - content) / 2;
  const ImU32 ink = with_alpha(fg, alpha);
  if (icon) {
    draw_icon(draw, icon, ImVec2(x, min.y + (h - glyph) / 2), glyph, ink);
    x += glyph + (visible.empty() ? 0 : px(8));
  }
  if (!visible.empty()) draw->AddText(font, font->LegacySize, ImVec2(x, min.y + (h - font->LegacySize) / 2), ink, visible.c_str());
  return pressed && !disabled;
}

bool toggle(const char* id, bool on, bool disabled, float scale) {
  // A square switch: a 1 px frame and a square knob that sits right and takes the accent when on.
  const float w = px(32) * scale, h = px(18) * scale, slot = px(28) * scale;
  const ImVec2 cell = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(disabled);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, slot), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered();
  const float slide = anim_item(on ? 1.0f : 0.0f, kRateState, 0x7091u);  // the knob travels, and the colours follow it
  const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  const float press = press_t();
  ImGui::EndDisabled();
  const ImVec2 pos(cell.x, cell.y + std::floor((slot - h) / 2));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Palette& p = pal();
  const float alpha = disabled ? 0.45f : 1.0f;
  draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), with_alpha(p.canvas, alpha));
  const ImU32 rest_edge = blend(p.line_strong, p.muted, hover);
  draw->AddRect(pos, ImVec2(pos.x + w, pos.y + h), with_alpha(blend(rest_edge, p.accent, slide), alpha));
  const float inset = px(3) * scale, knob = h - inset * 2;
  const float x = pos.x + inset + (w - inset * 2 - knob) * slide;
  const ImU32 rest_knob = blend(p.muted, p.ink, hover);
  draw->AddRectFilled(ImVec2(x, pos.y + inset), ImVec2(x + knob, pos.y + inset + knob),
                      with_alpha(blend(rest_knob, p.accent, slide), alpha));
  if (press > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), with_alpha(p.canvas, 0.3f * press));  // A2
  return pressed && !disabled;
}

bool checkbox(const char* id, bool on, bool disabled) {
  // A square box with a square inside it when it is ticked: the same frame and fill language as the switch.
  const float side = px(20), inset = px(5);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(disabled);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(side, side), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered();
  const float fill = anim_item(on ? 1.0f : 0.0f, kRateState, 0x3a11u);
  const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  const float press = press_t();  // A2
  ImGui::EndDisabled();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Palette& p = pal();
  const float alpha = disabled ? 0.45f : 1.0f;
  draw->AddRectFilled(pos, ImVec2(pos.x + side, pos.y + side), with_alpha(p.canvas, alpha));
  draw->AddRect(pos, ImVec2(pos.x + side, pos.y + side), with_alpha(blend(blend(p.line_strong, p.muted, hover), p.ink, fill), alpha));
  if (fill > 0.0f) {
    draw->AddRectFilled(ImVec2(pos.x + inset, pos.y + inset), ImVec2(pos.x + side - inset, pos.y + side - inset),
                        with_alpha(blend(p.muted, p.ink, fill), alpha * fill));
  }
  if (press > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + side, pos.y + side), with_alpha(p.canvas, 0.3f * press));
  return pressed && !disabled;
}

bool segmented(const char* id, const std::vector<std::string>& labels, int& selected, float cell, float height, ImFont* face,
               float padding, const std::vector<std::string>& hints) {
  // Cells side by side in one frame, split by seams; the chosen one is filled with the text colour. A cell width
  // makes every cell the same size, so two groups of chips read as one column of rows.
  ImGui::PushID(id);
  ImFont* font = face ? face : fonts().strong;
  const float h = px(height), inner = px(padding);
  const float fixed = cell > 0.0f ? px(cell) : 0.0f;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  float total = 0;
  for (const std::string& item : labels) total += fixed > 0.0f ? fixed : text_width(font, item) + inner * 2;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Palette& p = pal();
  // BeginDisabled only fades ImGui's own drawing; these cells are painted by hand, so they take the style's alpha
  // themselves or a locked row would still look live.
  const float fade = ImGui::GetStyle().Alpha;
  const auto dim = [fade](ImU32 color) { return with_alpha(color, to_vec(color).w * fade); };
  draw->AddRectFilled(start, ImVec2(start.x + total, start.y + h), dim(p.canvas));
  bool changed = false;
  float x = start.x;
  int hovered_cell = -1;
  ImVec2 hovered_min, hovered_max;
  for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
    const float w = fixed > 0.0f ? fixed : text_width(font, labels[i]) + inner * 2;
    ImGui::SetCursorScreenPos(ImVec2(x, start.y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("seg", ImVec2(w, h), ImGuiButtonFlags_EnableNav) && selected != i) {
      selected = i;
      changed = true;
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
      hovered_cell = i;
      hovered_min = ImVec2(x, start.y);
      hovered_max = ImVec2(x + w, start.y + h);
    }
    const bool on = selected == i;
    const float chosen = anim_item(on ? 1.0f : 0.0f, kRateState, 0x5e91u);  // the chosen cell fills in, the old one empties
    const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
    const float press = press_t();  // A2: a pressed cell shades on the frame of the press
    ImGui::PopID();
    if (chosen > 0.0f) draw->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + h), dim(blend(0, p.ink, chosen)));
    if (press > 0.0f) draw->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + h), dim(with_alpha(p.canvas, 0.25f * press)));
    if (hover > 0.0f && chosen < 1.0f)
      draw->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + h), dim(blend(0, p.raised, hover * (1.0f - chosen))));
    if (i > 0 && !on && selected != i - 1) draw->AddLine(ImVec2(x, start.y), ImVec2(x, start.y + h), dim(p.line));
    draw->AddText(font, font->LegacySize, ImVec2(x + inner, start.y + (h - font->LegacySize) / 2),
                  dim(blend(blend(p.muted, p.ink, hover), p.canvas, chosen)), labels[i].c_str());
    x += w;
  }
  draw->AddRect(start, ImVec2(start.x + total, start.y + h), dim(p.line));
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(total, h));
  // The row's description follows the cell under the pointer, and falls back to the chosen one's when there is none.
  if (hints.size() == labels.size()) {
    const int shown = hovered_cell >= 0 ? hovered_cell : selected;
    if (shown >= 0 && shown < static_cast<int>(hints.size())) g_row_hint = hints[shown];
    // The cell under the pointer says what it does in its own box: a strip that no row wraps (Performance
    // mode's When to use) has no row tooltip to carry it.
    if (hovered_cell >= 0) {
      tooltip_rect(hovered_min, hovered_max, hints[hovered_cell], px(320));
      g_row_hint.clear();
    }
  }
  ImGui::PopID();
  return changed;
}

bool select(const char* id, const std::vector<std::string>& labels, int& selected, float width, bool disabled,
            const std::vector<std::string>& hints, bool strong, const std::vector<std::string>& right, bool compact_rows) {
  bool changed = false;
  const std::string preview = selected >= 0 && selected < static_cast<int>(labels.size()) ? labels[selected] : std::string();
  const bool two_columns = right.size() == labels.size() && !right.empty();
  // Hints for every row are a right-hand column too, so a menu whose labels differ in width does not stagger them.
  bool hint_column = false;
  if (!two_columns && hints.size() == labels.size()) {
    for (const std::string& hint : hints) hint_column = hint_column || !hint.empty();
  }
  const std::vector<std::string>& aside = two_columns ? right : hints;
  float pop_width = px(width);
  if (two_columns || hint_column) {
    float widest = 0.0f;
    for (size_t i = 0; i < labels.size(); ++i) {
      widest = std::max(widest, text_width(fonts().body, labels[i]) + text_width(fonts().small, aside[i]));
    }
    pop_width = std::min(px(compact_rows ? 220 : 320), std::max(pop_width, widest + px(36)));
  }
  ImGui::PushStyleColor(ImGuiCol_FrameBg, pal().canvas);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, pal().raised);
  ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
  ImGui::PushStyleColor(ImGuiCol_Button, pal().canvas);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal().raised);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, pal().surface);
  ImGui::PushStyleColor(ImGuiCol_Header, pal().raised);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, pal().raised);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(4), px(4)));
  ImGui::PushFont(strong ? fonts().strong : fonts().body);
  ImGui::SetNextItemWidth(px(width));
  ImGui::BeginDisabled(disabled);
  // No ImGui arrow: the stroked chevron of the icon set is drawn over the field's right end instead, on the
  // field's own window (an open combo makes its popup the current window).
  ImDrawList* field_list = ImGui::GetWindowDrawList();
  const ImVec2 field_min = ImGui::GetCursorScreenPos();
  const ImVec2 field_max(field_min.x + px(width), field_min.y + ImGui::GetFrameHeight());
  // The popup never runs past the window's right edge: it is only as wide as the room the field has from its left end.
  const float space = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - field_min.x - px(8);
  const float pop_max = std::clamp(space, px(160), px(320));
  pop_width = std::min(pop_width, pop_max);
  ImGui::SetNextWindowSizeConstraints(ImVec2(two_columns ? pop_width : 0, 0), ImVec2(pop_max, FLT_MAX));
  const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
  {
    // A6: the arrow turns with the list. The icon set has no rotated draw, so the down arrow gives way to the up arrow on the
    // state token (a cross-fade, not a turn).
    const float glyph = px(14);
    const float turn = anim_to(ImGui::GetID("##turn"), open ? 1.0f : 0.0f, kRateState);
    const ImU32 ink = disabled ? with_alpha(pal().muted, 0.45f) : pal().muted;
    const ImVec2 at(field_max.x - glyph - px(8), field_min.y + (field_max.y - field_min.y - glyph) / 2);
    // A6: one chevron that turns half a revolution as the list opens (the vertices of the stroked icon rotate about its centre).
    const int turn_from = field_list->VtxBuffer.Size;
    draw_icon(field_list, "chevronDown", at, glyph, ink);
    rotate_since(field_list, turn_from, ImVec2(at.x + glyph / 2, at.y + glyph / 2), turn * IM_PI);
  }
  if (open) {
    const float pop_t = appear(ImGui::GetID("##pop"), kDurBase);  // the list fades in as it drops
    const int pop_v = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    if (compact_rows) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    const float row_height = px(compact_rows ? 22 : 26);
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
      const bool on = i == selected;
      ImGui::PushID(i);
      if (two_columns || hint_column) {
        // Name on the left, its second column right-aligned, a hairline between rows: a dot between them read as part
        // of the name, and a hint that followed each label staggered down the list.
        const ImVec2 row_min = ImGui::GetCursorScreenPos();
        const float row_w = std::max(ImGui::GetContentRegionAvail().x, pop_width - px(8));
        if (ImGui::Selectable("##row", on, 0, ImVec2(row_w, row_height))) {
          if (selected != i) changed = true;
          selected = i;
        }
        const ImVec2 row_max(row_min.x + row_w, row_min.y + row_height);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float name_h = fonts().body->LegacySize;
        const float right_h = fonts().small->LegacySize;
        const float count_width = text_width(fonts().small, aside[i]);
        const float name_end = two_columns ? row_max.x - px(18) - count_width : row_max.x - px(10);
        dl->PushClipRect(ImVec2(row_min.x, row_min.y), ImVec2(std::max(row_min.x, name_end), row_max.y), true);
        dl->AddText(fonts().body, name_h, ImVec2(row_min.x + px(10), row_min.y + (row_height - name_h) / 2),
                    on ? pal().accent : pal().ink, labels[i].c_str());
        dl->PopClipRect();
        if (compact_rows && ImGui::IsItemHovered() && text_width(fonts().body, labels[i]) > name_end - row_min.x - px(10)) {
          ImGui::SetTooltip("%s", labels[i].c_str());
        }
        if (!aside[i].empty()) {
          // The second column starts after the label, so a hint too long for the popup is cut there instead of
          // running back over the name.
          const float text_x = two_columns ? row_max.x - px(10) - count_width
                                           : row_min.x + px(10) + text_width(fonts().body, labels[i]) + px(12);
          dl->PushClipRect(ImVec2(text_x, row_min.y), ImVec2(row_max.x - px(10), row_max.y), true);
          dl->AddText(fonts().small, right_h,
                      ImVec2(row_max.x - px(10) - text_width(fonts().small, aside[i]),
                             row_min.y + (row_height - right_h) / 2),
                      on && two_columns ? pal().accent : pal().muted, aside[i].c_str());
          dl->PopClipRect();
        }
        if (i > 0 && two_columns) dl->AddLine(ImVec2(row_min.x, row_min.y), ImVec2(row_max.x, row_min.y), pal().line);
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, on ? pal().accent : pal().ink);
        if (ImGui::Selectable(labels[i].c_str(), on, 0, ImVec2(0, px(26)))) {
          if (selected != i) changed = true;
          selected = i;
        }
        ImGui::PopStyleColor();
        if (i < static_cast<int>(hints.size()) && !hints[i].empty()) {
          ImGui::SameLine();
          text(fonts().small, pal().muted, hints[i]);
        }
      }
      if (on) ImGui::SetItemDefaultFocus();
      ImGui::PopID();
    }
    if (compact_rows) ImGui::PopStyleVar();
    shift_since(ImGui::GetWindowDrawList(), pop_v, 0.0f, px(6) * (1.0f - pop_t));  // C9: the menu rises 6 px as it fades in
    fade_since(ImGui::GetWindowDrawList(), pop_v, pop_t);
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  ImGui::PopFont();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(8);
  return changed;
}

// Width of a run of caps, with a small `+` between each pair.
static float caps_run_width(ImFont* font, const std::vector<std::string>& caps, float h, float gap, float join,
                            float pad) {
  if (caps.empty()) return text_width(font, "set") + pad * 2;
  float total = 0;
  for (size_t i = 0; i < caps.size(); ++i) {
    total += std::max(h, text_width(font, lower(caps[i])) + pad * 2);
    if (i + 1 < caps.size()) total += gap * 2 + join;
  }
  return total;
}

bool chord_button(const char* id, const std::string& chord, const std::string& tip, bool clickable, float height,
                  bool on_accent) {
  // Each key a small square cap with its legend in lower case: alt, ctrl, 1. A `+` between them says the keys are
  // pressed together, which bare boxes did not.
  ImFont* font = fonts().label;
  const std::vector<std::string> caps = keycaps(chord);
  const float h = px(height), gap = px(6), pad = px(6), join = text_width(font, "+");
  const float w = caps_run_width(font, caps, h, gap, join, pad);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  bool pressed = false;
  bool hovered = false;
  if (clickable) {
    pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    hovered = ImGui::IsItemHovered();
  } else {
    ImGui::Dummy(ImVec2(w, h));
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Palette& p = pal();
  const ImU32 edge = on_accent ? with_alpha(p.on_accent, 0.6f) : hovered ? p.accent : p.line_strong;
  const ImU32 legend = on_accent ? p.on_accent : hovered ? p.ink : p.muted;
  const ImU32 join_color = with_alpha(legend, 0.55f);
  auto cap_box = [&](float x, float width, const std::string& value) {
    const ImVec2 min(x, pos.y), max(x + width, pos.y + h);
    draw->AddRect(min, max, edge);
    centred_text(draw, font, legend, min, max, value);
  };
  if (caps.empty()) {
    cap_box(pos.x, w, "set");
  } else {
    float x = pos.x;
    for (size_t i = 0; i < caps.size(); ++i) {
      const std::string legend_text = lower(caps[i]);
      const float width = std::max(h, text_width(font, legend_text) + pad * 2);
      cap_box(x, width, legend_text);
      x += width;
      if (i + 1 < caps.size()) {
        draw->AddText(font, font->LegacySize, ImVec2(x + gap, pos.y + (h - font->LegacySize) / 2), join_color, "+");
        x += gap * 2 + join;
      }
    }
  }
  tooltip(tip);
  return pressed;
}

float chord_width(const std::string& chord, float height) {
  return caps_run_width(fonts().label, keycaps(chord), px(height), px(6), text_width(fonts().label, "+"), px(6));
}

bool slider_commit(const char* id, int& value, int minimum, int maximum, const char* format, float width) {
  ImGui::PushID(id);
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID live_id = ImGui::GetID("live");
  const ImGuiID dragging_id = ImGui::GetID("dragging");
  int live = storage->GetBool(dragging_id) ? storage->GetInt(live_id, value) : value;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, pal().canvas);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, pal().raised);
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, pal().raised);
  ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(6), px(3)));
  // V6: the thumb grows while it is under the pointer or held, on the hover token (last frame's hover: one frame behind).
  const ImGuiID hover_id = ImGui::GetID("hover");
  const float grow = anim_to(ImGui::GetID("grow"), storage->GetBool(hover_id) || storage->GetBool(dragging_id) ? 1.0f : 0.0f, kRateHover);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, px(8) + px(2) * grow);
  ImGui::SetNextItemWidth(px(width));
  ImGui::SliderInt("##slider", &live, minimum, maximum, format, ImGuiSliderFlags_AlwaysClamp);
  const bool active = ImGui::IsItemActive();
  const bool released = ImGui::IsItemDeactivatedAfterEdit();
  storage->SetBool(hover_id, ImGui::IsItemHovered());
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(4);
  storage->SetBool(dragging_id, active);
  storage->SetInt(live_id, live);
  ImGui::PopID();
  if (released && live != value) {
    value = live;
    return true;
  }
  return false;
}

bool slider_vertical_commit(const char* id, float& value, float height, bool disabled) {
  ImGui::PushID(id);
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID live_id = ImGui::GetID("live");
  const ImGuiID dragging_id = ImGui::GetID("dragging");
  float live = storage->GetBool(dragging_id) ? storage->GetFloat(live_id, value) : value;
  const float w = px(20), h = px(height);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(disabled);
  ImGui::InvisibleButton("track", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
  const bool active = ImGui::IsItemActive() && !disabled;
  const bool focused = ImGui::IsItemFocused() && !disabled;
  const bool hovered_fader = ImGui::IsItemHovered() && !disabled;
  const float knob = px(5);
  if (active) {
    const float t = 1.0f - (ImGui::GetIO().MousePos.y - pos.y - knob) / std::max(1.0f, h - knob * 2);
    live = std::round(std::clamp(t, 0.0f, 1.0f) * 100.0f) / 100.0f;
  }
  const bool released = ImGui::IsItemDeactivated();
  ImGui::EndDisabled();
  bool keyed = false;
  if (focused) {
    // A fader the keyboard can reach: the arrows move it in steps of a twentieth and commit at once.
    int step = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
    if (step != 0) {
      live = std::clamp(live + step * 0.05f, 0.0f, 1.0f);
      keyed = true;
    }
  }
  // The wheel over the fader moves it a notch at a time and commits at once, as the arrows do: a twentieth, a hundredth
  // with Shift. A fader that is not recording at all ignores it.
  if (!disabled && ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
    const float step = ImGui::GetIO().KeyShift ? 0.01f : 0.05f;
    live = std::clamp(live + (ImGui::GetIO().MouseWheel > 0.0f ? step : -step), 0.0f, 1.0f);
    keyed = true;
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float y = pos.y + knob + (1.0f - live) * (h - knob * 2);
  // The fader is a thin rail with a marker across it at the volume: it reads as a level, not as a block.
  draw->AddLine(ImVec2(pos.x + w / 2, pos.y + knob), ImVec2(pos.x + w / 2, pos.y + h - knob), pal().line, 1.0f);
  const ImU32 marker = disabled ? pal().muted : pal().accent;
  draw->AddRectFilled(ImVec2(pos.x + px(2), y - px(1.5f)), ImVec2(pos.x + w - px(2), y + px(1.5f)), marker);
  // B11/V6: the marker grows 1.0 -> 1.15 under the pointer and while it is dragged (a tween, not a jump).
  const float grow = 1.0f + 0.15f * anim_item(active || hovered_fader ? 1.0f : 0.0f, kRateHover, 0x6a0du);
  draw->AddRectFilled(ImVec2(pos.x + w / 2 - px(2) * grow, y - px(4) * grow), ImVec2(pos.x + w / 2 + px(2) * grow, y + px(4) * grow), marker);
  if (focused) {
    draw->AddRect(ImVec2(pos.x - px(2), pos.y + knob - px(2)), ImVec2(pos.x + w + px(2), pos.y + h - knob + px(2)), pal().accent, 0, 0, px(2));
  }
  storage->SetBool(dragging_id, active);
  storage->SetFloat(live_id, live);
  ImGui::PopID();
  if (keyed) {
    value = live;
    return true;
  }
  if (released && std::fabs(live - value) > 0.001f) {
    value = live;
    return true;
  }
  return false;
}

void level_bar(float width, float value, float height) {
  // A row of cells, lit from the left; the last eighth warns.
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = px(width), h = px(height);
  ImGui::Dummy(ImVec2(w, h));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const int count = std::max(8, static_cast<int>(width / 5));
  const float gap = 1.0f;
  const float cell = (w - gap * (count - 1)) / count;
  const int lit = static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * count));
  for (int i = 0; i < count; ++i) {
    const float x = std::floor(pos.x + i * (cell + gap));
    const ImU32 on = i >= count * 7 / 8 ? pal().warn : pal().ok;
    draw->AddRectFilled(ImVec2(x, pos.y), ImVec2(std::floor(x + cell), pos.y + h), i < lit ? on : pal().raised);
  }
}

bool text_field(const char* id, std::string& value, float width, const char* hint, int max_length, int flags) {
  ImGui::PushStyleColor(ImGuiCol_FrameBg, pal().canvas);
  ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
  ImGui::PushFont(fonts().body);
  ImGui::SetNextItemWidth(px(width));
  if (static_cast<int>(value.size()) > max_length) value.resize(static_cast<size_t>(max_length));
  if (hint) ImGui::InputTextWithHint(id, hint, &value, flags);
  else ImGui::InputText(id, &value, flags);
  const bool committed = ImGui::IsItemDeactivatedAfterEdit();
  if (ImGui::IsItemActive()) {
    // The field being typed in takes the accent's frame.
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), pal().accent);
  }
  ImGui::PopFont();
  ImGui::PopStyleColor(2);
  return committed;
}

// ---------------------------------------------------------------- the timeline

bool timeline(const char* id, const std::string& state, double filled, double seconds, uint32_t pulse_age_ms, float width,
              float height_px) {
  // The buffer as a tape from the buffer's length ago to now: what a clip saved this moment would hold.
  // The seconds held are set large over it; the cells fill left to right as the ring fills, and the
  // labels under them count back from now. The whole strip is the start/stop switch.
  const Palette& p = pal();
  const float w = px(width);
  ImFont* big = fonts().numbers;
  const float number_h = big->LegacySize;
  const float cells_h = px(14), gap_y = px(8), legend_h = fonts().small->LegacySize;
  const float natural = number_h + gap_y + cells_h + px(6) + legend_h;
  // Given room, the cells take it: the tape is what the deck's leftover height goes into, so the column
  // ends on the library's line and no pane holds a blank.
  const float h = height_px > 0.0f ? std::max(natural, height_px) : natural;
  const float cells_fill = std::max(cells_h, h - number_h - gap_y - px(6) - legend_h);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool busy = state == "starting" || state == "stopping" || state == "disconnected";
  const bool on = state == "on";
  ImGui::BeginDisabled(busy);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::EndDisabled();
  tooltip(state == "disconnected" ? "Connecting to the engine"
          : on                    ? "Stop the buffer"
          : busy                  ? "Working\xE2\x80\xA6"
                                  : "Start the buffer");
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // The count: the seconds held, and the length after it.
  const std::string held = on ? format_duration(std::min(filled, seconds)) : busy ? std::string() : std::string("off");
  float x = pos.x;
  if (busy) {
    spinner(draw, ImVec2(x + px(12), pos.y + number_h / 2), px(8), p.accent);
    const char* word = state == "starting" ? "starting" : state == "stopping" ? "stopping" : "connecting";
    draw->AddText(fonts().title, fonts().title->LegacySize, ImVec2(x + px(32), pos.y + (number_h - fonts().title->LegacySize) / 2), p.muted, word);
  } else {
    draw->AddText(big, big->LegacySize, ImVec2(x, pos.y), on ? p.ink : p.muted, held.c_str());
    x += text_width(big, held) + px(10);
    const std::string of = on ? "/ " + format_duration(seconds) : "keeps the last moments";
    // Off, the line gives way to the start button rather than run under it.
    const float chip_left = pos.x + w;
    if (on || x + text_width(fonts().body, of) <= chip_left)
      draw->AddText(fonts().body, fonts().body->LegacySize, ImVec2(x, pos.y + number_h - fonts().body->LegacySize - px(6)),
                    hovered && !on ? p.ink : p.muted, of.c_str());
  }

  // The tape: one cell a second up to a minute, then one cell for each sixtieth of the length.
  const int count = std::clamp(static_cast<int>(std::round(seconds)), 10, 60);
  const float cell_gap = std::max(1.0f, px(2));
  const float cell_w = (w - cell_gap * (count - 1)) / count;
  const float share = on && seconds > 0 ? static_cast<float>(std::min(1.0, filled / seconds)) : 0.0f;
  const int lit = static_cast<int>(std::round(share * count));
  const float flash = pulse_age_ms < 700 ? 1.0f - pulse_age_ms / 700.0f : 0.0f;  // a save lights the tape toward white
  const float cy = pos.y + number_h + gap_y;
  for (int i = 0; i < count; ++i) {
    // The newest seconds sit at the right, against now: the tape fills from the right edge back.
    const int from_right = count - 1 - i;
    const bool is_lit = from_right < lit;
    ImU32 color = is_lit ? p.accent : (hovered && !busy ? p.line_strong : p.raised);
    if (is_lit && flash > 0) color = mix(p.accent, p.ink, flash * 0.7f);
    const float cx = std::floor(pos.x + i * (cell_w + cell_gap));
    draw->AddRectFilled(ImVec2(cx, cy), ImVec2(std::floor(cx + cell_w), cy + cells_fill), color);
  }

  // The legend: how long ago each quarter of the tape is, ending at now.
  const float ly = pos.y + h - legend_h;
  for (int q = 0; q <= 4; ++q) {
    const double ago = seconds * (4 - q) / 4.0;
    const std::string mark = q == 4 ? std::string("now") : "-" + format_span(ago);
    const float mw = text_width(fonts().small, mark);
    const float mx = q == 0 ? pos.x : q == 4 ? pos.x + w - mw : std::floor(pos.x + w * q / 4.0f - mw / 2);
    draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(mx, ly), q == 4 ? p.ink : p.muted, mark.c_str());
  }
  return pressed && !busy;
}

float timeline_height() {
  return fonts().numbers->LegacySize + px(8) + px(14) + px(6) + fonts().small->LegacySize;
}

// ---------------------------------------------------------------- settings layout

void begin_section(const char* id, const std::string& title, const std::function<void()>& aside, float width) {
  // A Settings card is a pane: its title set into the top border. The aside (a level meter, a button)
  // sits at the right of the first line inside.
  ImGui::Dummy(ImVec2(0, px(6)));
  begin_pane(id, title, ImVec2(width, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(3)));  // rows are tight: the seam is their gap
  if (aside) {
    const float right = ImGui::GetContentRegionMax().x;
    const float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(right - px(200), top - px(4)));
    ImGui::BeginGroup();
    aside();
    ImGui::EndGroup();
    ImGui::SetCursorPosY(top);
  }
}

void end_section(float trim_bottom) {
  ImGui::PopStyleVar();
  end_pane(trim_bottom);
}

void begin_row(const std::string& label_text, const std::string& hint, ImU32 hint_color, float control_h,
               const std::string& value) {
  const float full = ImGui::GetContentRegionAvail().x;
  // The controls sit flush with the pane's right seam: their width is the one the row measured last frame
  // (a row seen for the first time starts at the old 340 px column and settles on the next frame).
  const ImGuiID width_id = ImGui::GetID(("##row_controls" + label_text).c_str());
  g_row_width_id = width_id;
  const float measured = ImGui::GetStateStorage()->GetFloat(width_id, px(340));
  const float left = std::max(px(180), full - measured);
  const float label_w = std::min(left - px(24), px(560));
  // Rows are split by a seam, never above the first one in a pane.
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID first_id = ImGui::GetID("##rows_started");
  if (storage->GetInt(first_id, -1) == ImGui::GetFrameCount()) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y), ImVec2(at.x + full, at.y), pal().line);
    ImGui::Dummy(ImVec2(0, px(3)));
  }
  storage->SetInt(first_id, ImGui::GetFrameCount());
  ImGui::BeginGroup();
  const float row_top = ImGui::GetCursorPosY();
  g_row_min = ImVec2(ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x, ImGui::GetCursorScreenPos().y - px(4));
  // A one-line label is centred on the controls beside it (28 px tall unless a row brings its own control height,
  // and then on that); one with a line of state under it starts at the top of the row.
  const bool state_line = !hint.empty() && hint_color;
  const float centre_h = control_h > 0 ? control_h : px(26);
  if (!state_line) ImGui::SetCursorPosY(row_top + std::max(0.0f, (centre_h - fonts().strong->LegacySize) / 2));
  // A row with no label leaves the left column to its description, level with the control beside it.
  if (!label_text.empty()) text_wrapped(fonts().strong, pal().ink, label_text, label_w);
  if (!value.empty()) {
    // A value beside its label reads as the answer to it, not as another setting: it is drawn in the muted colour on
    // the label's own line, which is the only place that centres the two.
    ImGui::SameLine(0, px(8));
    text_wrapped(fonts().strong, pal().muted, value,
                 std::max(px(60), label_w - text_width(fonts().strong, label_text) - px(8)));
  }
  // A description waits behind the row and shows while it is hovered; a line in a state colour (a
  // hotkey Windows refused, say) is news, not a description, so it stays on the row.
  g_row_hint.clear();
  if (!hint.empty() && hint_color) text_wrapped(fonts().small, hint_color, hint, label_w);
  else if (!hint.empty()) g_row_hint = hint;
  // A row with neither a label nor a hint has nothing in its left column. An item there gives the row its
  // height, so the controls' own placement does not read as an attempt to grow the window (ImGui 1.92).
  if (label_text.empty() && hint.empty()) ImGui::Dummy(ImVec2(0, centre_h));
  ImGui::EndGroup();
  const float label_bottom = ImGui::GetCursorPosY();
  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + left, row_top));
  ImGui::BeginGroup();
  ImGui::GetStateStorage()->SetFloat(ImGui::GetID("##row_bottom"), label_bottom);
}

void end_row() {
  ImGui::EndGroup();
  const float width = ImGui::GetItemRectSize().x;
  ImGuiStorage* storage = ImGui::GetStateStorage();
  if (!g_row_hint.empty()) {
    const ImVec2 max(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, ImGui::GetItemRectMax().y + px(4));
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(g_row_min, max) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(10), px(7)));
      ImGui::PushStyleColor(ImGuiCol_PopupBg, pal().raised);
      ImGui::PushStyleColor(ImGuiCol_Border, pal().line_strong);
      if (ImGui::BeginTooltip()) {
        ImGui::PushFont(fonts().small);
        ImGui::PushTextWrapPos(px(380));
        ImGui::PushStyleColor(ImGuiCol_Text, pal().ink);
        ImGui::TextUnformatted(g_row_hint.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::EndTooltip();
      }
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar();
    }
    g_row_hint.clear();
  }
  if (g_row_width_id && std::fabs(storage->GetFloat(g_row_width_id, px(340)) - width) > 0.5f) {
    storage->SetFloat(g_row_width_id, width);
    g_layout_settling = true;  // one more frame, so the row is drawn where it now belongs
  }
  g_row_width_id = 0;
  const float label_bottom = ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##row_bottom"), 0);
  const float controls_bottom = ImGui::GetCursorPosY();
  ImGui::SetCursorPosY(std::max(label_bottom, controls_bottom));
  ImGui::Dummy(ImVec2(0, px(1)));
}

bool advanced(const char* id, const std::string& title, bool& open) {
  ImGui::PushID(id);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = px(32);
  if (ImGui::InvisibleButton("toggle", ImVec2(w, h), ImGuiButtonFlags_EnableNav)) open = !open;
  const bool hovered = ImGui::IsItemHovered();
  const float hover = anim_item(hovered ? 1.0f : 0.0f, kRateHover);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (hover > 0.0f) draw->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), blend(0, pal().raised, hover));
  draw->AddRect(pos, ImVec2(pos.x + w, pos.y + h), open ? pal().line_strong : pal().line);
  const float glyph = px(14);
  const float opened = anim_item(open ? 1.0f : 0.0f, kRateState, 0x7ad1u);  // A6: right turns to down
  const ImVec2 chevron_at(pos.x + px(10), pos.y + (h - glyph) / 2);
  const int chevron_from = draw->VtxBuffer.Size;
  draw_icon(draw, "chevronRight", chevron_at, glyph, blend(pal().muted, pal().accent, opened));
  rotate_since(draw, chevron_from, ImVec2(chevron_at.x + glyph / 2, chevron_at.y + glyph / 2), opened * IM_PI * 0.5f);
  draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(pos.x + px(32), pos.y + (h - fonts().strong->LegacySize) / 2),
                hovered || open ? pal().ink : pal().muted, title.c_str());
  ImGui::PopID();
  return open;
}

void note(const std::string& value, bool warn) {
  text_wrapped(fonts().small, warn ? pal().warn : pal().muted, value, ImGui::GetContentRegionAvail().x - px(4));
}

// ---------------------------------------------------------------- toasts

void draw_toasts(UiStore& store, ImVec2 anchor, float width) {
  if (store.toasts.empty()) return;
  const Palette& p = pal();
  const float w = px(width), pad = px(12);
  // A window of its own, on top: a click on a toast dismisses it and never reaches the control under it.
  ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, px(8)));
  ImGui::Begin("##toasts", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoMove);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  int dismiss = 0;
  for (const UiToast& toast : store.toasts) {  // oldest first: the newest sits nearest the corner
    // A notice as a notification daemon draws one: a square card, a 2 px frame in the notice's tone and
    // a filled tag that says what kind of notice it is.
    const char* tag = toast.kind == UiToast::error ? "alert" : toast.kind == UiToast::success ? "done" : "info";
    const ImU32 tone = toast.kind == UiToast::error ? p.warn : p.accent;
    const float tag_w = text_width(fonts().label, tag) + px(12);
    const float text_w = w - pad * 3 - tag_w;
    const ImVec2 title_size = fonts().strong->CalcTextSizeA(fonts().strong->LegacySize, FLT_MAX, text_w, toast.title.c_str());
    const ImVec2 detail_size = toast.detail.empty()
                                   ? ImVec2(0, 0)
                                   : fonts().small->CalcTextSizeA(fonts().small->LegacySize, FLT_MAX, text_w, toast.detail.c_str());
    const float h = pad * 2 + title_size.y + (toast.detail.empty() ? 0 : detail_size.y + px(3));
    // A notice slides in from the right and fades in; in its last quarter second it fades out instead of vanishing.
    ImGui::PushID(toast.id);
    const float arrived = appear(ImGui::GetID("in"), kDurBase);
    ImGui::PopID();
    const float leaving = std::clamp(static_cast<float>(static_cast<int32_t>(toast.until_ms - GetTickCount())) / 250.0f, 0.0f, 1.0f);
    if (leaving < 1.0f) anim_touch();
    const int toast_vertices = draw->VtxBuffer.Size;
    ImVec2 min = ImGui::GetCursorScreenPos();
    min.x += (1.0f - arrived) * px(28);
    const ImVec2 max(min.x + w, min.y + h);
    ImGui::PushID(toast.id);
    if (ImGui::InvisibleButton("toast", ImVec2(w, h), ImGuiButtonFlags_EnableNav)) dismiss = toast.id;
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    draw->AddRectFilled(min, max, hovered ? p.raised : p.surface);
    const float border = px(2);
    draw->AddRect(ImVec2(min.x + border / 2, min.y + border / 2), ImVec2(max.x - border / 2, max.y - border / 2), tone, 0, 0, border);
    const ImVec2 tag_min(min.x + pad, min.y + pad);
    const ImVec2 tag_max(tag_min.x + tag_w, tag_min.y + fonts().label->LegacySize + px(4));
    draw->AddRectFilled(tag_min, tag_max, tone);
    centred_text(draw, fonts().label, toast.kind == UiToast::error ? p.canvas : p.on_accent, tag_min, tag_max, tag);
    const float x = tag_max.x + pad;
    draw->AddText(fonts().strong, fonts().strong->LegacySize, ImVec2(x, min.y + pad), p.ink, toast.title.c_str(), nullptr, text_w);
    if (!toast.detail.empty()) {
      draw->AddText(fonts().small, fonts().small->LegacySize, ImVec2(x, min.y + pad + title_size.y + px(3)), p.muted,
                    toast.detail.c_str(), nullptr, text_w);
    }
    fade_since(draw, toast_vertices, arrived * leaving);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  if (dismiss) store.dismiss(dismiss);
}

}  // namespace bx::ui
