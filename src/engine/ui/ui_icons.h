// The interface's icons: hand-drawn 24-unit stroke paths (the same set the editor page uses), parsed
// once into segments the native interface strokes at any size and DPI. Pure: no ImGui here.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bx::ui {

struct PathSegment {
  enum Kind { move, line, cubic, close } kind = move;
  float x = 0, y = 0;                    // end point
  float c1x = 0, c1y = 0, c2x = 0, c2y = 0;  // control points of a cubic
};

// SVG path data: M L H V C A Z, absolute and relative, packed numbers ("1-2.5.5"). Arcs come back as
// cubics. An unknown command ends the parse with nothing, so a typo shows no icon rather than a wrong one.
std::vector<PathSegment> parse_svg_path(const std::string& data);

// The BuffrX mark (brand/buffrx-mark.svg) as one closed contour in the SVG's own units (x 3.2-320.5, y 93-336.4): its path
// flattened, without the repeat of the first point that closes it.
struct MarkPoint {
  float x = 0, y = 0;
};
const std::vector<MarkPoint>& mark_outline();

// Ear clipping of a simple polygon (no crossings, no holes), wound either way: the triangles as indexes into `points`, all
// wound the same way. ImGui's own concave fill lost the mark's notches and filled it as a blob, so the mark is cut once here.
// Empty for fewer than three points; a polygon that is not simple yields what could be cut, never loops.
std::vector<std::array<int, 3>> triangulate(const std::vector<MarkPoint>& points);

// The mark's coverage over its SVG box, `size` square, one byte per pixel: 255 inside, 0 outside, and the edge pixels
// between. Sampled 4x4 per pixel, so the titlebar's 18-unit mark keeps the browser's smooth outline at any DPI — the
// triangles alone stair-step, and stroking them only moved the steps.
std::vector<uint8_t> mark_coverage(int size);

// The path data for an icon name ("save", "camera", …), or "" for a name that is not in the set.
std::string icon_path(const std::string& name);

}  // namespace bx::ui
