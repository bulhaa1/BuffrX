#include "ui_theme.h"

#include <windows.h>

#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "../themes.h"
#include "ui_format.h"

namespace bx::ui {
namespace {

constexpr ImU32 rgb(unsigned value) {
  return IM_COL32((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF, 0xFF);
}

bool file_exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string windows_fonts() {
#ifdef _WIN32
  char buffer[MAX_PATH] = {0};
  const UINT length = GetWindowsDirectoryA(buffer, MAX_PATH);
  return std::string(buffer, length) + "\\Fonts\\";
#else
  return "/usr/share/fonts/truetype/";
#endif
}

// A font file's bytes, read once for the life of the process. The atlas used to read every file again
// for each of its seven sizes on every rebuild - and the CJK fallback alone (msyh.ttc) is about 20 MB,
// so a single new character in a clip name read some 300 MB from disk on the window thread.
std::map<std::string, std::vector<char>>& font_cache() {
  static std::map<std::string, std::vector<char>> cache;
  return cache;
}

const std::vector<char>* font_bytes(const std::string& path) {
  std::map<std::string, std::vector<char>>& cache = font_cache();
  auto found = cache.find(path);
  if (found != cache.end()) return found->second.empty() ? nullptr : &found->second;
  std::vector<char>& bytes = cache[path];
  if (FILE* file = std::fopen(path.c_str(), "rb")) {
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0) {
      bytes.resize(static_cast<size_t>(size));
      if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
    }
    std::fclose(file);
  }
  return bytes.empty() ? nullptr : &bytes;
}

// Which fallback faces the extra characters need: a Windows font is merged only for a script that is
// actually there, so a Cyrillic clip name never pays for loading Chinese, Japanese and Korean fonts.
std::vector<const char*> fallback_faces(const std::set<ImWchar>& extra) {
  bool cjk = false, kana = false, hangul = false, symbols = false, other = false;
  for (ImWchar c : extra) {
    if (c >= 0x3040 && c <= 0x30FF) kana = true;
    else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x3000 && c <= 0x303F) ||
             (c >= 0xFF00 && c <= 0xFFEF)) cjk = true;
    else if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F)) hangul = true;
    else if (c >= 0x2400 && c <= 0x2BFF) symbols = true;
    else other = true;
  }
  std::vector<const char*> faces;
#ifdef _WIN32
  if (other) faces.push_back("segoeui.ttf");
  if (kana) faces.push_back("YuGothM.ttc");
  if (hangul) faces.push_back("malgun.ttf");
  if (cjk) faces.push_back("msyh.ttc");
  if (symbols || other) faces.push_back("seguisym.ttf");
#else
  if (other) faces.push_back("dejavu/DejaVuSans.ttf");
  if (kana || cjk) faces.push_back("noto/NotoSansCJK-Regular.ttc");
  if (hangul) faces.push_back("noto/NotoSansCJK-Regular.ttc");
  if (symbols || other) faces.push_back("dejavu/DejaVuSans.ttf");
#endif
  return faces;
}

}  // namespace

ImVec4 to_vec(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }

ImU32 with_alpha(ImU32 color, float alpha) {
  ImVec4 v = to_vec(color);
  v.w = alpha;
  return ImGui::ColorConvertFloat4ToU32(v);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
  const ImVec4 x = to_vec(a), y = to_vec(b);
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

Palette palette_for(const std::string& theme) {
  // One row of the theme table (themes.h), which the tests hold to its contrast floors. The older role
  // names stay so the views did not have to learn new ones: canvas is the ground, surface a pane.
  const ThemeColors& t = theme_colors(theme);
  Palette p{};
  p.light = t.light;
  p.canvas = rgb(t.ground);
  p.surface = rgb(t.tile);
  p.raised = rgb(t.raised);
  p.line = rgb(t.line);
  p.ink = rgb(t.fg);
  p.muted = rgb(t.muted);
  // Tertiary text is secondary text: a third grey would sit under the 4.5:1 floor on some themes.
  p.faint = rgb(t.muted);
  p.accent = rgb(t.acc);
  p.on_accent = rgb(t.on);
  p.ok = rgb(t.ok);
  p.warn = rgb(t.warn);
  p.danger = rgb(t.danger);
  p.rec = rgb(t.rec);
  p.line_strong = mix(p.line, p.muted, 0.32f);
  p.accent_dim = mix(p.accent, p.canvas, 0.22f);
  p.hover = p.raised;
  p.accent_soft = with_alpha(p.accent, 0.14f);
  p.accent_line = with_alpha(p.accent, 0.55f);
  p.danger_soft = with_alpha(p.danger, 0.14f);
  p.warn_soft = with_alpha(p.warn, 0.12f);
  return p;
}

void release_font_files() { std::map<std::string, std::vector<char>>().swap(font_cache()); }

bool build_fonts(ImFontAtlas* atlas, const std::string& font_dir, float scale, const std::set<ImWchar>& extra,
                 std::vector<ImWchar>& extra_ranges,
                 Fonts& fonts) {
  atlas->Clear();
  const std::string regular = font_dir + "\\JetBrainsMonoNL-Regular.ttf";
  const std::string medium = font_dir + "\\JetBrainsMonoNL-Medium.ttf";
  const std::string bold = font_dir + "\\JetBrainsMonoNL-Bold.ttf";
  const std::string heavy = font_dir + "\\JetBrainsMonoNL-ExtraBold.ttf";
  if (!file_exists(regular) || !file_exists(medium) || !file_exists(bold) || !file_exists(heavy)) return false;

  // Latin, Latin-1, Latin Extended-A, Greek, Cyrillic, general punctuation (… · – " "), arrows, the
  // replacement character: what Inter covers and the interface's own words use.
  static const ImWchar base_ranges[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2000, 0x206F,
                                        0x2190, 0x21FF, 0x2300, 0x23FF, 0xFFFD, 0xFFFD, 0};
  // The extra characters, as a range list ImGui reads (pairs, zero-terminated). The caller keeps it alive
  // as long as the atlas, which reads it again whenever it rebuilds.
  extra_ranges.clear();
  for (ImWchar c : extra) {
    extra_ranges.push_back(c);
    extra_ranges.push_back(c);
  }
  extra_ranges.push_back(0);
  std::vector<std::string> fallbacks;
  const std::string system = windows_fonts();
  if (!extra.empty()) {
    for (const char* face : fallback_faces(extra)) {
      if (file_exists(system + face)) fallbacks.push_back(system + face);
    }
  }

  // From memory the process already holds, never owned by the atlas: a rebuild re-reads nothing.
  auto from_memory = [&](const std::string& path, float size, ImFontConfig& config, const ImWchar* ranges) -> ImFont* {
    const std::vector<char>* bytes = font_bytes(path);
    if (!bytes) return nullptr;
    config.FontDataOwnedByAtlas = false;
    return atlas->AddFontFromMemoryTTF(const_cast<char*>(bytes->data()), static_cast<int>(bytes->size()), size * scale,
                                       &config, ranges);
  };
  auto add = [&](const std::string& path, float size) {
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    ImFont* font = from_memory(path, size, config, base_ranges);
    if (font && !extra.empty()) {
      for (const std::string& fallback : fallbacks) {
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.OversampleH = 1;
        merge.OversampleV = 1;
        from_memory(fallback, size, merge, extra_ranges.data());
      }
    }
    return font;
  };
  // One monospaced face in four weights. A mono glyph is wider than Inter's, so the sizes sit a step
  // under the old ones and the lines measure about the same. Medium for running text: Regular is thin
  // on the dark themes.
  fonts.body = add(medium, 14.0f);
  fonts.strong = add(bold, 14.0f);
  fonts.small = add(medium, 13.5f);
  fonts.tiny = add(medium, 12.0f);
  fonts.label = add(medium, 13.0f);
  fonts.title = add(bold, 16.0f);
  fonts.display = add(heavy, 22.0f);
  fonts.numbers = add(heavy, 40.0f);
  fonts.big = add(heavy, 32.0f);
  return fonts.body && fonts.strong && fonts.small && fonts.tiny && fonts.label && fonts.title && fonts.display && fonts.numbers &&
         fonts.big;
}

void capitalise_glyphs(ImFontAtlas* atlas) {
  // The whole interface is set in capitals: each lower-case character draws its capital's glyph, so no call site
  // changes and a measured width is the drawn width. Since ImGui 1.92 the remap is a lookup applied as glyphs load, so it
  // needs no built atlas.
  for (ImFont* font : atlas->Fonts) {
    for (unsigned c = 'a'; c <= 0x45F; ++c) {
      const unsigned upper = upper_codepoint(c);
      if (upper != c) font->AddRemapChar(static_cast<ImWchar>(c), static_cast<ImWchar>(upper));
    }
  }
}

void apply_style(const Palette& p, float scale) {
  ImGuiStyle style;
  ImGui::StyleColorsDark(&style);
  // Square everywhere: panes, controls, popups and the scrollbar are flat rectangles with 1 px seams.
  style.WindowPadding = ImVec2(0, 0);
  style.WindowRounding = 0;
  style.WindowBorderSize = 0;
  style.ChildRounding = 0;
  style.ChildBorderSize = 1;
  style.FramePadding = ImVec2(10, 6);
  style.FrameRounding = 0;
  style.FrameBorderSize = 1;
  style.ItemSpacing = ImVec2(8, 8);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.PopupRounding = 0;
  style.PopupBorderSize = 1;
  style.ScrollbarSize = 6;
  style.ScrollbarRounding = 0;
  style.GrabRounding = 0;
  style.GrabMinSize = 12;
  style.TabRounding = 0;
  style.SelectableTextAlign = ImVec2(0, 0.5f);
  style.DisabledAlpha = 0.45f;

  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = to_vec(p.ink);
  c[ImGuiCol_TextDisabled] = to_vec(p.faint);
  c[ImGuiCol_WindowBg] = to_vec(p.canvas);
  c[ImGuiCol_ChildBg] = to_vec(with_alpha(p.surface, 0.0f));
  c[ImGuiCol_PopupBg] = to_vec(p.surface);
  c[ImGuiCol_Border] = to_vec(p.line);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = to_vec(p.canvas);
  c[ImGuiCol_FrameBgHovered] = to_vec(p.raised);
  c[ImGuiCol_FrameBgActive] = to_vec(p.raised);
  c[ImGuiCol_TitleBg] = to_vec(p.surface);
  c[ImGuiCol_TitleBgActive] = to_vec(p.surface);
  c[ImGuiCol_MenuBarBg] = to_vec(p.surface);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = to_vec(p.line);
  c[ImGuiCol_ScrollbarGrabHovered] = to_vec(p.muted);
  c[ImGuiCol_ScrollbarGrabActive] = to_vec(p.accent);
  c[ImGuiCol_CheckMark] = to_vec(p.accent);
  c[ImGuiCol_SliderGrab] = to_vec(p.accent);
  c[ImGuiCol_SliderGrabActive] = to_vec(p.accent);
  c[ImGuiCol_Button] = to_vec(p.raised);
  c[ImGuiCol_ButtonHovered] = to_vec(p.hover);
  c[ImGuiCol_ButtonActive] = to_vec(p.hover);
  c[ImGuiCol_Header] = to_vec(p.raised);
  c[ImGuiCol_HeaderHovered] = to_vec(p.raised);
  c[ImGuiCol_HeaderActive] = to_vec(p.hover);
  c[ImGuiCol_Separator] = to_vec(p.line);
  c[ImGuiCol_SeparatorHovered] = to_vec(p.accent_line);
  c[ImGuiCol_SeparatorActive] = to_vec(p.accent);
  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_Tab] = to_vec(p.canvas);
  c[ImGuiCol_TabHovered] = to_vec(p.raised);
  c[ImGuiCol_TabSelected] = to_vec(p.raised);
  c[ImGuiCol_TextSelectedBg] = to_vec(with_alpha(p.accent, 0.35f));
  c[ImGuiCol_NavCursor] = to_vec(p.accent);
  c[ImGuiCol_ModalWindowDimBg] = to_vec(with_alpha(p.canvas, 0.72f));
  c[ImGuiCol_TableBorderLight] = to_vec(p.line);
  c[ImGuiCol_TableBorderStrong] = to_vec(p.line);
  style.ScaleAllSizes(scale);
  ImGui::GetStyle() = style;
}

}  // namespace bx::ui
