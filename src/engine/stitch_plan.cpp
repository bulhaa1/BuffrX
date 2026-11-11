#include "stitch_plan.h"

#include <algorithm>
#include <cmath>

namespace bx {
namespace {

int even_at_least(int value, int minimum) {
  const int even = (value / 2) * 2;
  return even < minimum ? minimum : even;
}


}  // namespace

bool make_stitch_plan(const std::vector<StitchSource>& sources, const ExportJob& job, StitchPlan& plan,
                      std::string& error) {
  plan = StitchPlan{};
  if (sources.size() < 2) {
    error = "a join needs at least two clips";
    return false;
  }
  int with_audio = 0;
  for (const StitchSource& source : sources) {
    if (source.duration <= 0.0 || source.width <= 0 || source.height <= 0 || source.fps <= 0.0) {
      error = "one of the clips could not be read";
      return false;
    }
    if (!(source.speed >= 0.25 && source.speed <= 4.0)) {
      error = "one of the clips has a speed that cannot be exported";
      return false;
    }
    ExportJob one;
    one.start = source.start;
    one.end = source.end;
    one.segments = source.segments;
    std::vector<std::pair<double, double>> parts = export_segments(one, source.duration);
    if (parts.empty()) {
      error = "one of the clips has nothing left to keep";
      return false;
    }
    plan.total_seconds += segments_length(parts) / source.speed;
    plan.parts.push_back(std::move(parts));
    if (source.audio_tracks > 0) ++with_audio;
  }
  if (plan.total_seconds < 0.1) {
    error = "the joined clip would be shorter than a frame";
    return false;
  }
  if (with_audio != 0 && with_audio != static_cast<int>(sources.size())) {
    error = "some clips have sound and some do not; a join needs them to match";
    return false;
  }
  // A join copies the stream layout of every part, so one clip with two tracks beside one with a
  // single track would produce a file the muxer cannot splice. Refuse it here, before any encode.
  for (size_t i = 1; i < sources.size(); ++i) {
    if (sources[i].audio_tracks != sources[0].audio_tracks) {
      error = "the clips have different audio tracks";
      return false;
    }
  }
  plan.has_audio = with_audio == static_cast<int>(sources.size());
  // The user's own output size, when one was chosen, is the join's target too: every part is encoded
  // to it, so a downscaled export never runs at the first clip's full size first.
  plan.width = even_at_least(job.out_width > 0 ? job.out_width : sources.front().width, 16);
  plan.height = even_at_least(job.out_height > 0 ? job.out_height : sources.front().height, 16);
  plan.fps = job.fps > 0.0 ? job.fps : sources.front().fps;
  if (!(plan.fps >= 1.0 && plan.fps <= 240.0)) {
    error = "the frame rate of the first clip is not usable";
    return false;
  }
  return true;
}

double stitch_tolerance(double expected_seconds) {
  const double half_percent = expected_seconds * 0.005;
  return half_percent > 0.5 ? half_percent : 0.5;
}

bool stitch_duration_ok(double expected_seconds, double actual_seconds) {
  if (expected_seconds <= 0.0 || actual_seconds <= 0.0) return false;
  return std::fabs(actual_seconds - expected_seconds) <= stitch_tolerance(expected_seconds);
}


}  // namespace bx
