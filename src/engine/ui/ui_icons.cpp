#include "ui_icons.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace bx::ui {
namespace {

const std::map<std::string, std::string>& icon_table() {
  static const std::map<std::string, std::string> table = {
      {"save", "M12 4v10m0 0-4-4m4 4 4-4M5 17v2a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2"},
      {"update", "M12 4v12m-5-5 5 5 5-5M6 20h12"},
      {"history", "M4 12a8 8 0 1 0 2.4-5.7L4 8.5M4 4v4.5h4.5M12 8v4.5l3 2"},
      {"record", "M12 12m-6 0a6 6 0 1 0 12 0a6 6 0 1 0-12 0"},
      {"stop", "M7 7h10v10H7z"},
      {"camera", "M4 8a2 2 0 0 1 2-2h2l1.5-2h5L16 6h2a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V8Zm8 1a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Z"},
      {"settings", "M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6Zm7.4 3a7.4 7.4 0 0 0-.1-1.2l2-1.6-2-3.4-2.4 1a7.6 7.6 0 0 0-2-1.2L14.5 3h-4l-.4 2.6a7.6 7.6 0 0 0-2 1.2l-2.4-1-2 3.4 2 1.6a7.4 7.4 0 0 0 0 2.4l-2 1.6 2 3.4 2.4-1a7.6 7.6 0 0 0 2 1.2l.4 2.6h4l.4-2.6a7.6 7.6 0 0 0 2-1.2l2.4 1 2-3.4-2-1.6c.1-.4.1-.8.1-1.2Z"},
      {"scissors", "M6 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6Zm0 12a3 3 0 1 0 0-6 3 3 0 0 0 0 6ZM8.2 7.8 20 18M8.2 16.2 20 6"},
      {"folder", "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Z"},
      {"minus", "M6 12h12"},
      {"feather", "M20 4C11 4 6 9 6 16v4M6 20l8-8M14 12h5M12 9h5"},
      {"pin", "M9 4h6M10 4v6l-3 4h10l-3-4V4M12 14v6"},
      {"square", "M6 6h12v12H6z"},
      {"close", "M6 6l12 12M18 6 6 18"},
      {"compact", "M4 14h6v6M20 10h-6V4M14 10l7-7M3 21l7-7"},
      {"expand", "M15 3h6v6M9 21H3v-6M21 3l-7 7M3 21l7-7"},
      {"play", "M8 5v14l11-7L8 5Z"},
      {"pause", "M8 5h3v14H8zM13 5h3v14h-3z"},
      {"dots", "M5 12h.01M12 12h.01M19 12h.01"},
      {"mic", "M12 3a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3Zm-6 9a6 6 0 0 0 12 0M12 18v3"},
      {"micOff", "M9 9v3a3 3 0 0 0 5.1 2.1M15 9.3V6a3 3 0 0 0-5.9-.8M6 12a6 6 0 0 0 10 4.5M18 12a6 6 0 0 1-.4 2.1M12 18v3M4 4l16 16"},
      {"speaker", "M4 9v6h4l5 4V5L8 9H4Zm12.5-.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12"},
      {"speakerOff", "M4 9v6h4l5 4V5L8 9H4Zm12 1 5 5m0-5-5 5"},
      {"check", "M5 12.5 10 17 19 7"},
      {"alert", "M12 8v5m0 3.5h.01M10.3 3.9 2.6 17.5A2 2 0 0 0 4.3 20.5h15.4a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z"},
      {"info", "M4 4h16v16H4zM12 11v5m0-8.5h.01"},
      {"copy", "M9 9h10v12H9zM5 15V3h10"},
      {"share", "M12 15V3m-5 5 5-5 5 5M5 13v7h14v-7"},
      {"trash", "M4 7h16M10 11v6m4-6v6M6 7l1 13h10l1-13M9 7V4h6v3"},
      {"edit", "M4 20h4L19 9l-4-4L4 16v4Zm9-13 4 4"},
      {"external", "M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"},
      {"chevronLeft", "M15 5l-7 7 7 7"},
      {"arrowRight", "M4 12h15M14 7l5 5-5 5"},
      {"chevronRight", "M9 5l7 7-7 7"},
      {"chevronDown", "M5 9l7 7 7-7"},
      {"chevronUp", "M5 15l7-7 7 7"},
      {"palette", "M12 3a9 9 0 1 0 0 18c1 0 1.6-.8 1.6-1.7 0-.4-.2-.8-.4-1.1-.3-.3-.4-.7-.4-1.1 0-.9.7-1.6 1.6-1.6H16a5 5 0 0 0 5-5c0-4.1-4-7.5-9-7.5ZM7.5 12h.01M10 7.5h.01M15 8h.01"},
      {"game", "M7 9v4m-2-2h4m6 1h.01M18 10h.01M7 5h10a5 5 0 0 1 5 5v2.5a4.5 4.5 0 0 1-8 2.8l-.8-1h-2.4l-.8 1a4.5 4.5 0 0 1-8-2.8V10a5 5 0 0 1 5-5Z"},
      {"monitor", "M3 5h18v11H3zM8 20h8M12 16v4"},
      {"window", "M3 5h18v14H3zM3 9h18"},
      {"film", "M4 4h16v16H4zM8 4v16M16 4v16M4 8h4M4 12h4M4 16h4M16 8h4M16 12h4M16 16h4"},
      {"image", "M4 5h16v14H4zM4 16l5-5 4 4 2-2 5 5M15 9h.01"},
      {"plus", "M12 5v14M5 12h14"},
      {"refresh", "M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7"},
      {"power", "M12 3v8M6.3 7.3a8 8 0 1 0 11.4 0"},
      {"keyboard", "M3 7h18v10H3zM7 11h.01M11 11h.01M15 11h.01M8 14h8"},
      {"sliders", "M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12M20 18h0M14 4v4M8 10v4M16 16v4"},
      {"layers", "M12 3 3 8l9 5 9-5-9-5Zm-9 9 9 5 9-5M3 16l9 5 9-5"},
      {"hdd", "M4 14h16v5H4zM4 14l2.5-9h11L20 14M8 17h.01"},
      {"bell", "M6 16V11a6 6 0 1 1 12 0v5l2 2H4l2-2Zm4 4h4"},
      {"cpu", "M8 8h8v8H8zM9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"},
      {"undo", "M9 14 4 9l5-5M4 9h11a5 5 0 0 1 0 10h-3"},
      // A simplified Discord mark: the face, its two bottom claws and the eyes, in the same 24-unit stroke
      // style as every other glyph. The filled brand silhouette turns to mush at the size a row draws.
      {"discord",
       "M7.3 6.3C9 5.8 10.5 5.6 12 5.6C13.5 5.6 15 5.8 16.7 6.3C18.7 9.1 19.6 12.1 19.4 15.2"
       "C18 16.4 16.6 17.2 15.2 17.5L14.4 16.3C12.8 16.6 11.2 16.6 9.6 16.3L8.8 17.5"
       "C7.4 17.2 6 16.4 4.6 15.2C4.4 12.1 5.3 9.1 7.3 6.3Z"
       "M9.7 12.2h.01M14.3 12.2h.01"},
      // A simplified GitHub cat in the same stroke style: the head with its two ears, and the legs.
      {"github",
       "M5 9.5C5 6.5 7.5 4.5 12 4.5C16.5 4.5 19 6.5 19 9.5C19 13 16.5 15.5 14.5 15.5H9.5C7.5 15.5 5 13 5 9.5Z"
       "M5.6 7.6 5.2 4.2l3.2 1.4M18.4 7.6l.4-3.4-3.2 1.4M9.5 15.5V20M14.5 15.5V20M9.5 18.2C7.2 18.8 6 17.8 5 16.6"},
      {"globe",
       "M12 3a9 9 0 1 0 0 18a9 9 0 0 0 0-18M3.4 9h17.2M3.4 15h17.2"
       "M12 3C14.6 5.5 16 8.5 16 12C16 15.5 14.6 18.5 12 21C9.4 18.5 8 15.5 8 12C8 8.5 9.4 5.5 12 3Z"},
  };
  return table;
}

struct Reader {
  const std::string& text;
  size_t at = 0;

  void skip() {
    while (at < text.size() && (std::isspace(static_cast<unsigned char>(text[at])) || text[at] == ',')) ++at;
  }
  bool number_next() {
    skip();
    if (at >= text.size()) return false;
    const char c = text[at];
    return c == '-' || c == '+' || c == '.' || std::isdigit(static_cast<unsigned char>(c));
  }
  // SVG packs numbers: "1-2.5.5" is 1, -2.5, .5. strtod alone would read ".5.5" as one bad token.
  bool number(float& out) {
    if (!number_next()) return false;
    size_t end = at;
    if (text[end] == '-' || text[end] == '+') ++end;
    bool dot = false;
    while (end < text.size()) {
      const char c = text[end];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        ++end;
      } else if (c == '.' && !dot) {
        dot = true;
        ++end;
      } else {
        break;
      }
    }
    if (end < text.size() && (text[end] == 'e' || text[end] == 'E')) {
      size_t exponent = end + 1;
      if (exponent < text.size() && (text[exponent] == '-' || text[exponent] == '+')) ++exponent;
      if (exponent < text.size() && std::isdigit(static_cast<unsigned char>(text[exponent]))) {
        end = exponent;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
      }
    }
    out = static_cast<float>(std::strtod(text.substr(at, end - at).c_str(), nullptr));
    at = end;
    return true;
  }
  // An arc's flags are single digits that may be packed with no separator ("0 0 1" or "001").
  bool flag(bool& out) {
    skip();
    if (at >= text.size() || (text[at] != '0' && text[at] != '1')) return false;
    out = text[at] == '1';
    ++at;
    return true;
  }
};

// The SVG arc (endpoint form) as cubic Béziers of at most 90 degrees each.
void arc_to_cubics(float x0, float y0, float rx, float ry, float rotation_degrees, bool large, bool sweep, float x,
                   float y, std::vector<PathSegment>& out) {
  if (x0 == x && y0 == y) return;
  rx = std::fabs(rx);
  ry = std::fabs(ry);
  if (rx == 0 || ry == 0) {
    PathSegment segment;
    segment.kind = PathSegment::line;
    segment.x = x;
    segment.y = y;
    out.push_back(segment);
    return;
  }
  const double pi = 3.14159265358979323846;
  const double phi = rotation_degrees * pi / 180.0;
  const double cos_phi = std::cos(phi), sin_phi = std::sin(phi);
  const double dx = (x0 - x) / 2.0, dy = (y0 - y) / 2.0;
  const double x1p = cos_phi * dx + sin_phi * dy;
  const double y1p = -sin_phi * dx + cos_phi * dy;
  double rxs = rx, rys = ry;
  const double lambda = (x1p * x1p) / (rxs * rxs) + (y1p * y1p) / (rys * rys);
  if (lambda > 1) {
    rxs *= std::sqrt(lambda);
    rys *= std::sqrt(lambda);
  }
  const double numerator = rxs * rxs * rys * rys - rxs * rxs * y1p * y1p - rys * rys * x1p * x1p;
  const double denominator = rxs * rxs * y1p * y1p + rys * rys * x1p * x1p;
  double factor = denominator == 0 ? 0 : std::sqrt(std::max(0.0, numerator / denominator));
  if (large == sweep) factor = -factor;
  const double cxp = factor * rxs * y1p / rys;
  const double cyp = -factor * rys * x1p / rxs;
  const double cx = cos_phi * cxp - sin_phi * cyp + (x0 + x) / 2.0;
  const double cy = sin_phi * cxp + cos_phi * cyp + (y0 + y) / 2.0;
  auto angle = [](double ux, double uy, double vx, double vy) {
    const double dot = ux * vx + uy * vy;
    const double length = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    double value = std::acos(std::max(-1.0, std::min(1.0, length == 0 ? 1.0 : dot / length)));
    if (ux * vy - uy * vx < 0) value = -value;
    return value;
  };
  const double theta = angle(1, 0, (x1p - cxp) / rxs, (y1p - cyp) / rys);
  double delta = angle((x1p - cxp) / rxs, (y1p - cyp) / rys, (-x1p - cxp) / rxs, (-y1p - cyp) / rys);
  if (!sweep && delta > 0) delta -= 2 * pi;
  if (sweep && delta < 0) delta += 2 * pi;
  const int pieces = std::max(1, static_cast<int>(std::ceil(std::fabs(delta) / (pi / 2) - 1e-9)));
  const double step = delta / pieces;
  const double k = 4.0 / 3.0 * std::tan(step / 4.0);
  auto point = [&](double t, double& px, double& py) {
    px = cx + rxs * std::cos(t) * cos_phi - rys * std::sin(t) * sin_phi;
    py = cy + rxs * std::cos(t) * sin_phi + rys * std::sin(t) * cos_phi;
  };
  auto tangent = [&](double t, double& tx, double& ty) {
    tx = -rxs * std::sin(t) * cos_phi - rys * std::cos(t) * sin_phi;
    ty = -rxs * std::sin(t) * sin_phi + rys * std::cos(t) * cos_phi;
  };
  double t = theta;
  for (int i = 0; i < pieces; ++i) {
    double ax, ay, bx, by, atx, aty, btx, bty;
    point(t, ax, ay);
    point(t + step, bx, by);
    tangent(t, atx, aty);
    tangent(t + step, btx, bty);
    PathSegment segment;
    segment.kind = PathSegment::cubic;
    segment.c1x = static_cast<float>(ax + k * atx);
    segment.c1y = static_cast<float>(ay + k * aty);
    segment.c2x = static_cast<float>(bx - k * btx);
    segment.c2y = static_cast<float>(by - k * bty);
    segment.x = static_cast<float>(bx);
    segment.y = static_cast<float>(by);
    out.push_back(segment);
    t += step;
  }
  // Land exactly on the requested end point.
  out.back().x = x;
  out.back().y = y;
}

}  // namespace

std::vector<PathSegment> parse_svg_path(const std::string& data) {
  std::vector<PathSegment> out;
  Reader reader{data};
  float cx = 0, cy = 0;          // current point
  float sx = 0, sy = 0;          // start of the current subpath
  char command = 0;
  while (true) {
    reader.skip();
    if (reader.at >= data.size()) break;
    const char c = data[reader.at];
    if (std::isalpha(static_cast<unsigned char>(c))) {
      command = c;
      ++reader.at;
    } else if (!command) {
      return {};
    }
    const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
    if (upper == 'Z') {
      PathSegment segment;
      segment.kind = PathSegment::close;
      segment.x = sx;
      segment.y = sy;
      out.push_back(segment);
      cx = sx;
      cy = sy;
      command = 0;
      continue;
    }
    bool first = true;
    do {
      PathSegment segment;
      if (upper == 'M' || upper == 'L') {
        float x, y;
        if (!reader.number(x) || !reader.number(y)) return {};
        if (relative) {
          x += cx;
          y += cy;
        }
        // Pairs after a move are lines (SVG's implicit lineto).
        segment.kind = upper == 'M' && first ? PathSegment::move : PathSegment::line;
        segment.x = x;
        segment.y = y;
        if (segment.kind == PathSegment::move) {
          sx = x;
          sy = y;
        }
        cx = x;
        cy = y;
        out.push_back(segment);
      } else if (upper == 'H' || upper == 'V') {
        float value;
        if (!reader.number(value)) return {};
        segment.kind = PathSegment::line;
        segment.x = upper == 'H' ? (relative ? cx + value : value) : cx;
        segment.y = upper == 'V' ? (relative ? cy + value : value) : cy;
        cx = segment.x;
        cy = segment.y;
        out.push_back(segment);
      } else if (upper == 'C') {
        float v[6];
        for (float& value : v) {
          if (!reader.number(value)) return {};
        }
        if (relative) {
          for (int i = 0; i < 6; i += 2) {
            v[i] += cx;
            v[i + 1] += cy;
          }
        }
        segment.kind = PathSegment::cubic;
        segment.c1x = v[0];
        segment.c1y = v[1];
        segment.c2x = v[2];
        segment.c2y = v[3];
        segment.x = v[4];
        segment.y = v[5];
        cx = v[4];
        cy = v[5];
        out.push_back(segment);
      } else if (upper == 'A') {
        float rx, ry, rotation, x, y;
        bool large = false, sweep = false;
        if (!reader.number(rx) || !reader.number(ry) || !reader.number(rotation) || !reader.flag(large) ||
            !reader.flag(sweep) || !reader.number(x) || !reader.number(y)) {
          return {};
        }
        if (relative) {
          x += cx;
          y += cy;
        }
        arc_to_cubics(cx, cy, rx, ry, rotation, large, sweep, x, y, out);
        cx = x;
        cy = y;
      } else {
        return {};
      }
      first = false;
    } while (reader.number_next());
  }
  return out;
}

std::string icon_path(const std::string& name) {
  const auto& table = icon_table();
  const auto found = table.find(name);
  return found == table.end() ? std::string() : found->second;
}

const std::vector<MarkPoint>& mark_outline() {
  static const std::vector<MarkPoint> outline = [] {
    static const char* kMark = "M3.2 93L42.2 143.4L230 143.4C254.3 148.8 262 168.5 240 184.4C233.3 189.3 225.7 190.8 218 192.8L118.3 192.8L152.2 235.4L234 235.4C243.6 237.4 251.8 238.8 260 244.6C265.3 248.3 266.8 254.1 267.6 260C269.4 274.1 259.7 282.4 247 285.3L96 285.3L95.5 168L42.8 201.5L42.8 336.4L257 336.4C300.7 328.5 320.5 313.6 320.5 266C320.5 250 318.2 235.3 308 222.6C301.7 214.8 292 211.9 284.6 206C289.1 196.6 298.7 189.6 302.3 179C309.6 157.2 304.8 115.4 281 103.7C270.7 98.6 260.6 96.2 250 93L3.2 93Z";
    std::vector<MarkPoint> points;
    float x = 0, y = 0;
    for (const PathSegment& seg : parse_svg_path(kMark)) {
      if (seg.kind == PathSegment::cubic) {
        const float sx = x, sy = y;
        for (int i = 1; i <= 24; ++i) {
          const float t = i / 24.0f, u = 1 - t;
          points.push_back({u * u * u * sx + 3 * u * u * t * seg.c1x + 3 * u * t * t * seg.c2x + t * t * t * seg.x,
                            u * u * u * sy + 3 * u * u * t * seg.c1y + 3 * u * t * t * seg.c2y + t * t * t * seg.y});
        }
      } else if (seg.kind != PathSegment::close) {
        points.push_back({seg.x, seg.y});
      }
      if (seg.kind != PathSegment::close) {
        x = seg.x;
        y = seg.y;
      }
    }
    // The path ends on its first point to close; the contour does not repeat it.
    if (points.size() > 1 && points.front().x == points.back().x && points.front().y == points.back().y) points.pop_back();
    return points;
  }();
  return outline;
}

std::vector<uint8_t> mark_coverage(int size) {
  std::vector<uint8_t> coverage(size > 0 ? static_cast<size_t>(size) * static_cast<size_t>(size) : 0, 0);
  const std::vector<MarkPoint>& outline = mark_outline();
  if (size <= 0 || outline.size() < 3) return coverage;
  // The viewBox the page draws the same path in (-18 35 360 360), and the path's own box: pixels outside the glyph's
  // box cost nothing, because nothing there is inside the contour.
  constexpr float kBoxX = -18.0f, kBoxY = 35.0f, kBox = 360.0f;
  constexpr float kLeft = 3.2f, kRight = 320.5f, kTop = 93.0f, kBottom = 336.4f;
  constexpr int kSamples = 4;
  // The same triangles the titlebar drew before the bitmap existed: they already carry the mark's notches, where a
  // ray cast over the flattened contour counted in and out again at the vertices the outline touches.
  const std::vector<std::array<int, 3>> triangles = triangulate(outline);
  const float step = kBox / static_cast<float>(size);
  const int first_x = std::max(0, static_cast<int>(std::floor((kLeft - kBoxX) / step)));
  const int last_x = std::min(size - 1, static_cast<int>(std::ceil((kRight - kBoxX) / step)));
  const int first_y = std::max(0, static_cast<int>(std::floor((kTop - kBoxY) / step)));
  const int last_y = std::min(size - 1, static_cast<int>(std::ceil((kBottom - kBoxY) / step)));
  for (int y = first_y; y <= last_y; ++y) {
    for (int x = first_x; x <= last_x; ++x) {
      int inside = 0;
      for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
          // Each subsample at the centre of its own cell: how many of the sixteen a triangle covers is the pixel's alpha.
          const float px = kBoxX + (static_cast<float>(x) + (sx + 0.5f) / kSamples) * step;
          const float py = kBoxY + (static_cast<float>(y) + (sy + 0.5f) / kSamples) * step;
          bool in = false;
          for (const std::array<int, 3>& triangle : triangles) {
            const MarkPoint& a = outline[static_cast<size_t>(triangle[0])];
            const MarkPoint& b = outline[static_cast<size_t>(triangle[1])];
            const MarkPoint& c = outline[static_cast<size_t>(triangle[2])];
            const float d1 = (px - b.x) * (a.y - b.y) - (a.x - b.x) * (py - b.y);
            const float d2 = (px - c.x) * (b.y - c.y) - (b.x - c.x) * (py - c.y);
            const float d3 = (px - a.x) * (c.y - a.y) - (c.x - a.x) * (py - a.y);
            if ((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0)) {
              in = true;
              break;
            }
          }
          if (in) ++inside;
        }
      }
      coverage[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] =
          static_cast<uint8_t>(inside * 255 / (kSamples * kSamples));
    }
  }
  return coverage;
}

std::vector<std::array<int, 3>> triangulate(const std::vector<MarkPoint>& points) {
  std::vector<std::array<int, 3>> triangles;
  const int count = static_cast<int>(points.size());
  if (count < 3) return triangles;
  // Wound one way for the test below, whichever way it came.
  double area = 0;
  for (int i = 0; i < count; ++i) {
    const MarkPoint& a = points[static_cast<size_t>(i)];
    const MarkPoint& b = points[static_cast<size_t>((i + 1) % count)];
    area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
  }
  std::vector<int> ring(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) ring[static_cast<size_t>(i)] = area >= 0 ? i : count - 1 - i;
  const auto cross = [&](int a, int b, int c) {
    const MarkPoint &p = points[static_cast<size_t>(a)], &q = points[static_cast<size_t>(b)], &r = points[static_cast<size_t>(c)];
    return (static_cast<double>(q.x) - p.x) * (static_cast<double>(r.y) - p.y) - (static_cast<double>(q.y) - p.y) * (static_cast<double>(r.x) - p.x);
  };
  while (ring.size() > 3) {
    bool clipped = false;
    const size_t size = ring.size();
    for (size_t i = 0; i < size && !clipped; ++i) {
      const int a = ring[(i + size - 1) % size], b = ring[i], c = ring[(i + 1) % size];
      if (cross(a, b, c) <= 1e-9) continue;  // a reflex corner, or a straight one: not an ear
      bool empty = true;
      for (const int other : ring) {
        if (other == a || other == b || other == c) continue;
        if (cross(a, b, other) >= 0 && cross(b, c, other) >= 0 && cross(c, a, other) >= 0) {
          empty = false;
          break;
        }
      }
      if (!empty) continue;
      triangles.push_back({a, b, c});
      ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
      clipped = true;
    }
    if (!clipped) {
      // Nothing can be cut: a straight run of points, or a polygon that crosses itself. A straight corner is dropped without a
      // triangle (it adds no area); anything else ends the cutting rather than looping.
      bool dropped = false;
      for (size_t i = 0; i < size && !dropped; ++i) {
        if (std::abs(cross(ring[(i + size - 1) % size], ring[i], ring[(i + 1) % size])) <= 1e-9) {
          ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
          dropped = true;
        }
      }
      if (!dropped) return triangles;
    }
  }
  if (ring.size() == 3 && cross(ring[0], ring[1], ring[2]) > 1e-9) triangles.push_back({ring[0], ring[1], ring[2]});
  return triangles;
}

}  // namespace bx::ui
