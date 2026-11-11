// The pure part of a multi-clip join: what the output looks like, how long it will be, and where the
// joins are planned. No files are opened here, so every rule is testable without a video.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "media_math.h"

namespace bx {

// One file of a join, as the media probe described it.
struct StitchSource {
  std::string path;
  double duration = 0.0;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  int audio_tracks = 0;
  double start = 0.0;
  double end = 0.0;
  std::vector<std::pair<double, double>> segments;
  // The clip's own playback speed: what it contributes to the join is its kept parts divided by it.
  double speed = 1.0;
};

struct StitchPlan {
  int width = 0;
  int height = 0;
  double fps = 0.0;
  bool has_audio = false;
  double total_seconds = 0.0;
  // The cleaned parts of each source, in the same order as the sources.
  std::vector<std::vector<std::pair<double, double>>> parts;
};

// The common target and the total length. Refuses fewer than two sources, an unreadable one, a mixed
// set (some with sound, some without) and anything with no length. Pure.
bool make_stitch_plan(const std::vector<StitchSource>& sources, const ExportJob& job, StitchPlan& plan,
                      std::string& error);

// A saved join may be a frame or two off the sum of its parts: the last frame of a part ends where the
// next begins. Half a second, or half a percent of a long clip, is the acceptance window.
double stitch_tolerance(double expected_seconds);
bool stitch_duration_ok(double expected_seconds, double actual_seconds);


}  // namespace bx
