// The controls in ImGui: icons stroked from the shared path set, the logo's place, panes with their
// titles set into the border, buttons, a square switch, segmented choices, selects, key caps, cell
// meters, the dial, sliders that commit on release, settings sections and rows.
// Everything is flat and square. Views compose these; none of them knows which theme is on.
#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "ui_anim.h"
#include "ui_store.h"
#include "ui_theme.h"

namespace bx::ui {

// The theme and scale every widget reads, set once per frame by the window that draws.
void set_theme(const Palette* palette, const Fonts* fonts, float scale);
const Palette& pal();
const Fonts& fonts();
float px(float value);    // design pixels → window pixels
bool layout_settling();   // a settings row moved this frame and wants one more; reading it clears it
float unpx(float value);  // window pixels → design pixels

void draw_icon(ImDrawList* draw, const std::string& name, ImVec2 min, float size, ImU32 color, float stroke = 1.7f,
               bool fill = false);
void draw_mark(ImDrawList* draw, ImVec2 center, float size, ImU32 accent);  // the BuffrX mark, filled
// The mark's sampled bitmap, made once with the interface's device (UiHost::ensure_device). Until it exists the mark is
// drawn from its triangles.
void set_mark_texture(ImTextureID texture);
// The mark in the text colour, `size` units square, exactly as the editor's titlebar draws it.
void draw_app_icon(ImDrawList* draw, ImVec2 center, float size);
void spinner(ImDrawList* draw, ImVec2 center, float radius, ImU32 color);

void text(ImFont* font, ImU32 color, const std::string& value);
void text_ellipsis(ImFont* font, ImU32 color, const std::string& value, float max_width);
void text_wrapped(ImFont* font, ImU32 color, const std::string& value, float width);
// A field label: small, lower case. `color` 0 keeps it muted; the width drawn is returned so a control can sit beside it.
float label(const std::string& value, ImU32 color = 0);
float text_width(ImFont* font, const std::string& value);
void tooltip(const std::string& value, float wrap_width = 0);  // for the item just drawn, on hover
void tooltip_rect(ImVec2 min, ImVec2 max, const std::string& value, float wrap_width = 0);  // for an area with no item
// A window of its own sits over this rectangle (the clip page's WebView2 child): a hint is never drawn under it, so one that would
// land there is set above the pointer instead. Cleared (min == max) each frame the page is not shown.
void hint_keepout(ImVec2 min, ImVec2 max);

// A pane: a flat face on the ground with a 1 px seam, its title set into the top border and an optional
// aside at the border's right end. `focused` draws the 2 px accent border; `hover` draws the same border while the
// pointer is anywhere on the pane, for a pane that is itself one target. size.y == 0 fits the content. `padding_y`
// is the pane's inner padding top and bottom; a shorter one trims a pane whose last line needs a tighter foot.
void begin_pane(const char* id, const std::string& title, ImVec2 size, bool focused = false,
                const std::string& aside = std::string(), ImGuiWindowFlags flags = 0, bool hover = false,
                float padding_y = 13.0f, bool highlight = false);
void end_pane(float trim_bottom = 0.0f);
// The same frame drawn around a box the caller laid out itself.
void draw_pane(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::string& title, bool focused = false,
               const std::string& aside = std::string());
// Cell meters: `count` cells lit from the start up to `value` (0..1). The vertical one is a level meter
// (ok, then warn, then danger in its top eighth).
void cells(float width, float value, int count, float height = 8, ImU32 color = 0);
void vcells(float height, float value, int count, float width = 10);

bool icon_button(const char* id, const char* icon, const std::string& tip, bool active = false, float size = 30,
                 float icon_size = 16, bool danger_hover = false, bool danger = false);
enum class Variant { normal, primary, ghost, danger };
bool button(const char* label, Variant variant = Variant::normal, const char* icon = nullptr, bool disabled = false,
            float width = 0, float height = 32, bool focus_ring = true);
// A switch: true when it was flipped (the caller writes the new value). `scale` shrinks the frame, the knob and its slot together.
bool toggle(const char* id, bool on, bool disabled = false, float scale = 1.0f);
// A square box that ticks, for one choice beside its label in a row.
bool checkbox(const char* id, bool on, bool disabled = false);
// `hints`: one line per cell; the row's description follows the cell under the pointer, and the chosen one's otherwise.
bool segmented(const char* id, const std::vector<std::string>& labels, int& selected, float cell = 0.0f, float height = 28.0f,
               ImFont* font = nullptr, float padding = 12.0f, const std::vector<std::string>& hints = {});
// `right`: a right-aligned second column per row, for a count beside a name.
bool select(const char* id, const std::vector<std::string>& labels, int& selected, float width = 220,
            bool disabled = false, const std::vector<std::string>& hints = {}, bool strong = false,
            const std::vector<std::string>& right = {}, bool compact_rows = false);
// Keycaps for a chord; clickable ones rebind it.
bool chord_button(const char* id, const std::string& chord, const std::string& tip, bool clickable = true,
                  float height = 20, bool on_accent = false);
float chord_width(const std::string& chord, float height = 20);  // what chord_button will take, in window pixels
// An integer slider that reports only when the user lets go (no engine write per drag step).
bool slider_commit(const char* id, int& value, int minimum, int maximum, const char* format, float width = 220);
bool slider_vertical_commit(const char* id, float& value, float height, bool disabled);
void level_bar(float width, float value, float height = 4);
// A text field for `value`; true when it was committed (Enter or leaving the field).
// `flags`: ImGuiInputTextFlags (Password for a secret).
bool text_field(const char* id, std::string& value, float width, const char* hint = nullptr, int max_length = 256,
                int flags = 0);

// The buffer as a tape: the seconds held, large, over a row of cells from the buffer's length ago to now,
// with the quarters labelled. The whole strip starts and stops the buffer: true when clicked.
bool timeline(const char* id, const std::string& state, double filled, double seconds, uint32_t pulse_age_ms, float width,
              float height_px = 0.0f);

// The natural height of the tape, in window pixels: the count, one row of cells and the legend.
float timeline_height();

// A settings card with a title (and optional right-aligned aside), holding rows. width < 0 fills the page; a card whose
// rows carry no labels ends at its own last control instead of at a stretch of empty face.
void begin_section(const char* id, const std::string& title, const std::function<void()>& aside = {}, float width = -1.0f);
void end_section(float trim_bottom = 0.0f);
// One settings row: the label on the left, the controls the caller draws after it at the right seam. A hint
// with no colour is a description and shows as a tooltip while the row is hovered; a hint with a colour is
// state (a path, a version, a problem) and stays on the row. control_h is the height of a control taller than
// the usual 28 px row, so the label is centred on it rather than on the row. value is a chosen value shown beside
// the label in the muted colour, so it reads as the answer to the label instead of another setting.
void begin_row(const std::string& label, const std::string& hint = std::string(), ImU32 hint_color = 0,
               float control_h = 0, const std::string& value = std::string());
void end_row();
bool advanced(const char* id, const std::string& title, bool& open);  // the one disclosure per section
void note(const std::string& value, bool warn = false);

void draw_toasts(UiStore& store, ImVec2 anchor_bottom_right, float width);

}  // namespace bx::ui
