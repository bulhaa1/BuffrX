// The shape of the recorded picture: which size the video canvas takes and how the finished picture reaches the
// encoder's output.
//
// libobs renders the scene into the canvas (base) and then scales that canvas straight to the output, so the canvas
// and the output must share a shape or the final frame is squeezed. The scene's own fit (OBS_BOUNDS_SCALE_INNER)
// only decides what happens INSIDE the canvas; it cannot undo a non-uniform output scale. That is why the canvas,
// not the scene item, is the place this rule belongs (config.h: plan_canvas, canvas_takes_output_shape).
//
// It lives in a file of its own while another change owns tests/engine_tests.cpp: the same checks move into that
// harness with one include and one call when that change lands. Built as buffrx-canvas-tests, which needs nothing
// but the pure header (no libobs, no GPU, no window), so it runs anywhere.
#include <cstdio>
#include <string>

#include "../src/engine/config.h"

namespace {

int failures = 0;

void check(bool condition, const char* name) {
  std::printf("  %s %s\n", condition ? "ok  " : "FAIL", name);
  if (!condition) ++failures;
}

bx::Config config_with_output(const char* stretch_mode, int width, int height) {
  bx::Config config;
  config.stretch_mode = stretch_mode;
  config.output_width = width;
  config.output_height = height;
  return config;
}

// What the final picture's scale factors are: the canvas is scaled to the output on x and on y independently, and
// nothing preserves the shape there. Equal factors mean an undistorted picture; a difference is the squeeze.
struct OutputScale {
  double x;
  double y;
};

OutputScale output_scale(const bx::CanvasPlan& plan) {
  return {static_cast<double>(plan.output_width) / plan.canvas_width,
          static_cast<double>(plan.output_height) / plan.canvas_height};
}

bool scale_is_even(const bx::CanvasPlan& plan) {
  const OutputScale scale = output_scale(plan);
  const double biggest = scale.x > scale.y ? scale.x : scale.y;
  const double smallest = scale.x > scale.y ? scale.y : scale.x;
  return (biggest - smallest) <= biggest / 1000.0;  // one part in a thousand: below a pixel over any frame we record
}

// The picture inside the canvas after the scene's fit (OBS_BOUNDS_SCALE_INNER): the source scaled by one factor
// into the canvas, centred, with the remainder as bars.
struct PictureBox {
  int width = 0;
  int height = 0;
  int bar_x = 0;  // left and right bars, each
  int bar_y = 0;  // top and bottom bars, each
};

PictureBox fitted_box(int source_width, int source_height, int canvas_width, int canvas_height) {
  const double by_x = static_cast<double>(canvas_width) / source_width;
  const double by_y = static_cast<double>(canvas_height) / source_height;
  const double scale = by_x < by_y ? by_x : by_y;
  PictureBox box;
  box.width = static_cast<int>(source_width * scale + 0.5);
  box.height = static_cast<int>(source_height * scale + 0.5);
  box.bar_x = (canvas_width - box.width) / 2;
  box.bar_y = (canvas_height - box.height) / 2;
  return box;
}

bool box_keeps_shape(int source_width, int source_height, const PictureBox& box) {
  // The fitted picture's own shape against the source's, within the one pixel integer rounding can cost.
  const long long source = static_cast<long long>(source_width) * box.height;
  const long long fitted = static_cast<long long>(source_height) * box.width;
  const long long difference = source > fitted ? source - fitted : fitted - source;
  return difference <= static_cast<long long>(source_height) + source_width;
}

// ---------------------------------------------------------------- same shape

void test_same_shape() {
  std::printf("shape: which sizes share a shape\n");
  check(bx::same_picture_shape(1920, 1080, 1280, 720), "1920x1080 and 1280x720 are the same 16:9");
  check(bx::same_picture_shape(1920, 1080, 3840, 2160), "1920x1080 and 3840x2160 are the same 16:9");
  check(bx::same_picture_shape(1366, 768, 1920, 1080), "1366x768 is 16:9 within a pixel of rounding");
  check(bx::same_picture_shape(1921, 1081, 1920, 1080), "an odd capture size one pixel off is the same shape");
  check(bx::same_picture_shape(1920, 1080, 1080, 1920) == false, "16:9 and portrait are not the same shape");
  check(bx::same_picture_shape(3440, 1440, 1920, 1080) == false, "21:9 ultrawide and 16:9 are not the same shape");
  check(bx::same_picture_shape(1920, 1200, 1920, 1080) == false, "16:10 and 16:9 are not the same shape");
  check(bx::same_picture_shape(1920, 1080, 3440, 1440) == false, "the comparison is symmetric (16:9 against 21:9)");
  check(bx::same_picture_shape(1000, 1000, 1004, 999) == bx::same_picture_shape(1004, 999, 1000, 1000),
        "the rounding boundary gives the same answer in either order");
  check(bx::same_picture_shape(0, 1080, 1920, 1080) == false, "an unavailable size is no shape at all");
  check(bx::same_picture_shape(-1920, 1080, 1920, 1080) == false, "a negative size is no shape at all");
}

// ---------------------------------------------------------------- a native output

void test_native_output() {
  std::printf("shape: a native output keeps the capture's own size\n");
  const bx::Config native = config_with_output("auto", 0, 0);

  const bx::CanvasPlan wide = bx::plan_canvas(native, 3440, 1440);
  check(wide.canvas_width == 3440 && wide.canvas_height == 1440, "a 21:9 capture is drawn at its own size");
  check(wide.output_width == 3440 && wide.output_height == 1440, "and encoded at that size: nothing to squeeze");
  check(scale_is_even(wide), "canvas and output share a shape, so the output scale is even");

  const bx::CanvasPlan tall = bx::plan_canvas(native, 1200, 1920);
  check(tall.canvas_width == 1200 && tall.canvas_height == 1920, "a portrait capture keeps its own size");
  check(tall.output_width == 1200 && tall.output_height == 1920, "and its own output size");

  // No target and no output: the reported default, not a guess about a screen that is not there.
  const bx::CanvasPlan unknown = bx::plan_canvas(native, 0, 0);
  check(unknown.canvas_width == 1920 && unknown.canvas_height == 1080, "with nothing known the canvas is 1920x1080");
  check(unknown.output_width == 1920 && unknown.output_height == 1080, "and the output is the canvas");

  // Half a size is a mistake, never a request to guess the other half.
  const bx::CanvasPlan half = bx::plan_canvas(config_with_output("auto", 1920, 0), 3440, 1440);
  check(half.output_width == 3440 && half.output_height == 1440, "a half-configured output is ignored");
  check(half.canvas_width == 3440 && half.canvas_height == 1440, "so the native canvas stands");
}

// ---------------------------------------------------------------- a configured output of the same shape

void test_configured_same_shape() {
  std::printf("shape: a configured output of the same shape changes nothing\n");
  // The shipped behaviour, kept: the canvas is the capture's own size and the video thread scales it once.
  const bx::CanvasPlan plan = bx::plan_canvas(config_with_output("auto", 1920, 1080), 2560, 1440);
  check(plan.canvas_width == 2560 && plan.canvas_height == 1440, "the canvas stays at the capture's own size");
  check(plan.output_width == 1920 && plan.output_height == 1080, "the output is what the person configured");
  check(scale_is_even(plan), "both shapes are 16:9, so the single output scale is even");
}

// ---------------------------------------------------------------- a configured output of another shape

void test_configured_other_shape() {
  std::printf("shape: a configured output of another shape takes the canvas\n");
  // THE DEFECT: a 21:9 display, a 16:9 output. Before this rule the canvas was 3440x1440 and the output scale ran
  // 0.558 on x against 0.750 on y: every circle became an ellipse 34% taller than wide, and no scene fit could undo
  // it because the fit happens inside the canvas, one stage earlier.
  const bx::Config config = config_with_output("auto", 1920, 1080);
  const bx::CanvasPlan plan = bx::plan_canvas(config, 3440, 1440);
  check(plan.canvas_width == 1920 && plan.canvas_height == 1080,
        "the canvas takes the output's shape, so nothing is squeezed on the way to the encoder");
  check(plan.output_width == 1920 && plan.output_height == 1080, "the configured output dimensions are preserved");
  check(scale_is_even(plan), "the output scale is even (canvas and output are the same size here)");

  const OutputScale old_scale = {1920.0 / 3440.0, 1080.0 / 1440.0};
  check(old_scale.y / old_scale.x > 1.30,
        "the old plan really did distort: 3440x1440 pushed to 1920x1080 stretched y by 34% over x");

  // The picture inside that canvas: fitted, centred, bars only in the space left over.
  const PictureBox box = fitted_box(3440, 1440, plan.canvas_width, plan.canvas_height);
  check(box.width == 1920, "the fitted picture uses the output's full width");
  check(box.height == 804, "and 804 of its 1080 rows");
  check(box.bar_y == 138 && box.bar_x == 0, "the 276 missing rows are split evenly top and bottom, no side bars");
  check(box_keeps_shape(3440, 1440, box), "the picture inside the canvas keeps its 21:9 shape");

  // 16:10 and portrait, the same rule.
  const bx::CanvasPlan ten = bx::plan_canvas(config_with_output("auto", 1280, 720), 1920, 1200);
  check(ten.canvas_width == 1280 && ten.canvas_height == 720, "a 16:10 capture at a 16:9 output takes the canvas");
  check(scale_is_even(ten), "and is not squeezed");
  const bx::CanvasPlan portrait = bx::plan_canvas(config_with_output("auto", 1920, 1080), 1080, 1920);
  check(portrait.canvas_width == 1920 && portrait.canvas_height == 1080, "a portrait capture at a 16:9 output too");
  const PictureBox portrait_box = fitted_box(1080, 1920, portrait.canvas_width, portrait.canvas_height);
  check(portrait_box.height == 1080 && portrait_box.width == 608, "it is pillarboxed: 608 columns of 1920");
  check(portrait_box.bar_x == 656 && portrait_box.bar_y == 0, "with the bars on the sides, evenly");
  check(box_keeps_shape(1080, 1920, portrait_box), "and its shape kept");
}

// ---------------------------------------------------------------- Stretch stays the person's choice

void test_stretch_choice() {
  std::printf("shape: Stretch still fills the output\n");
  // The canvas is the output's shape for every choice: a fit then letterboxes inside it, and a stretch fills it,
  // which is exactly the configured output. Choosing Stretch must not bring the old squeezed canvas back.
  for (const char* mode : {"auto", "fit", "stretch"}) {
    const bx::CanvasPlan plan = bx::plan_canvas(config_with_output(mode, 1920, 1080), 3440, 1440);
    check(plan.canvas_width == 1920 && plan.canvas_height == 1080,
          mode == std::string("stretch") ? "Stretch keeps the output's canvas, so it fills the output exactly"
                                         : "a fit mode keeps the output's canvas too");
  }
  const bx::CanvasPlan stretched = bx::plan_canvas(config_with_output("stretch", 1920, 1080), 3440, 1440);
  // capture_fills_canvas ("stretch", ...) returns true, so the item is OBS_BOUNDS_STRETCH over the whole canvas:
  // the source fills the output's frame by the person's own choice, with no second shape change afterwards.
  check(stretched.canvas_width == stretched.output_width && stretched.canvas_height == stretched.output_height,
        "a stretched source reaches the encoder with no shape change after the stretch");
}

// ---------------------------------------------------------------- Performance mode plans the same picture

void test_performance_mode_agrees() {
  std::printf("shape: Performance mode plans the same picture\n");
  // os_host's plan_canvas_for hands plan_canvas no target size while Performance mode owns the canvas (the mode draws
  // straight into an output-sized canvas). With an output of another shape the two roads must agree, or switching the
  // mode on would change the picture's shape as well as its speed.
  const bx::Config config = config_with_output("auto", 1280, 1440);
  const bx::CanvasPlan performance = bx::plan_canvas(config, 0, 0);
  const bx::CanvasPlan normal = bx::plan_canvas(config, 2560, 1440);
  check(performance.canvas_width == normal.canvas_width && performance.canvas_height == normal.canvas_height,
        "Performance mode and the default agree on the canvas for a differently shaped output");
  check(performance.output_width == normal.output_width && performance.output_height == normal.output_height,
        "and on the output dimensions");
  check(scale_is_even(normal), "so the output scale is even with the mode on or off");
}

// ---------------------------------------------------------------- odd and unavailable sizes, screen moves

void test_odd_sizes_and_moves() {
  std::printf("shape: odd sizes, an unknown target and a screen move\n");
  // A target one pixel off a configured 1080p is the same shape: no need to change the canvas for it.
  const bx::CanvasPlan odd = bx::plan_canvas(config_with_output("auto", 1920, 1080), 1921, 1081);
  check(odd.canvas_width == 1921 && odd.canvas_height == 1081, "a capture one pixel off keeps its own canvas");
  check(scale_is_even(odd), "and the output scale stays even");

  // The target's size is not known yet (a window that is not running, a monitor enumeration that failed): the
  // configured output is the only honest plan, and it was already the fallback.
  const bx::CanvasPlan unknown = bx::plan_canvas(config_with_output("auto", 1920, 1080), 0, 0);
  check(unknown.canvas_width == 1920 && unknown.canvas_height == 1080, "an unknown target plans at the output size");

  // Moving between two screens feeds plan_canvas nothing but the target's size: negative desktop coordinates, a
  // window dragged to another monitor and a display reorder cannot change the picture's shape by themselves.
  const bx::Config wide_output = config_with_output("auto", 2560, 1080);
  const bx::CanvasPlan from_screen_one = bx::plan_canvas(wide_output, 3440, 1440);
  const bx::CanvasPlan from_screen_two = bx::plan_canvas(wide_output, 1680, 1050);
  check(from_screen_one.canvas_width == 2560 && from_screen_one.canvas_height == 1080, "a 21:9 capture at 2560x1080");
  check(from_screen_two.canvas_width == 2560 && from_screen_two.canvas_height == 1080,
        "a 16:10 capture at 2560x1080 takes the same canvas shape");
  check(scale_is_even(from_screen_one) && scale_is_even(from_screen_two),
        "neither screens' own sizes can squeeze the output scale");
}

}  // namespace

int main() {
  std::printf("canvas and output shape\n");
  test_same_shape();
  test_native_output();
  test_configured_same_shape();
  test_configured_other_shape();
  test_stretch_choice();
  test_performance_mode_agrees();
  test_odd_sizes_and_moves();
  if (failures == 0) {
    std::printf("\ncanvas shape: all checks passed\n");
    return 0;
  }
  std::printf("\ncanvas shape: %d FAILED\n", failures);
  return 1;
}
