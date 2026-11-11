// The look in ImGui: the theme's palette (themes.h, the same table the editor page and the notification
// card paint from), JetBrains Mono in four weights and the flat, square style, all scaled for the
// window's DPI. No view knows which theme is on; they paint with these.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "imgui.h"

namespace bx::ui {

// BuffrX's own record red: the same in every theme, for the dot that means "recording".
inline constexpr ImU32 kRecordRed = IM_COL32(224, 58, 52, 255);

// canvas is the ground behind the panes, surface a pane's face, raised a control on a pane.
struct Palette {
  ImU32 canvas, surface, raised, line, line_strong, ink, muted, faint, accent, accent_dim, danger, warn, rec, ok;
  ImU32 hover, accent_soft, accent_line, danger_soft, warn_soft, on_accent;
  bool light;
};

Palette palette_for(const std::string& theme);  // a themes.h id; anything else is the main theme
ImVec4 to_vec(ImU32 color);
ImU32 with_alpha(ImU32 color, float alpha);
ImU32 mix(ImU32 a, ImU32 b, float t);  // t = share of b

struct Fonts {
  ImFont* body = nullptr;      // Medium 14
  ImFont* strong = nullptr;    // Bold 14
  ImFont* small = nullptr;     // Medium 13.5
  ImFont* tiny = nullptr;      // Medium 12 (the quick panel's row detail)
  ImFont* label = nullptr;     // Medium 13 (pane titles and field labels, lower case)
  ImFont* title = nullptr;     // Bold 16
  ImFont* display = nullptr;   // ExtraBold 22 (the game being captured, a Settings heading)
  ImFont* numbers = nullptr;   // ExtraBold 40 (the dial's seconds)
  ImFont* big = nullptr;       // ExtraBold 32 (the quick panel's seconds)
};

// Fills `atlas` for one window at `scale`. `extra` are characters seen in the library (game folders,
// clip names in any language); they come from Windows' own fonts, so the atlas only carries what is
// shown instead of every CJK glyph.
// The font files build_fonts keeps in memory between rebuilds, given back. Only once no atlas uses
// them: the host calls it when it releases its device and every window with it.
void release_font_files();

bool build_fonts(ImFontAtlas* atlas, const std::string& font_dir, float scale, const std::set<ImWchar>& extra,
                 std::vector<ImWchar>& extra_ranges,
                 Fonts& fonts);

// After the atlas is built (the backend builds it when it creates its device objects): lower case draws as capitals.
void capitalise_glyphs(ImFontAtlas* atlas);

void apply_style(const Palette& palette, float scale);

}  // namespace bx::ui
