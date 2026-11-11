// Performance mode: the plan it applies, and the rule for when it is on. Pure, so every size and frame rate is tested.
//
// The mode is a layer over the user's settings, never a rewrite of them: ObsHost::effective_config() applies the plan on top
// of the config it reads, the Video page shows the locked rows with the values that are running, and turning the mode off
// puts the user's own back exactly. What it sets is what costs a running game frames: the picture (1920x1080 at 60 fps,
// decided with the user on 2026-11-05), the codec and the encoder's effort, the scaling filter, one audio track and a fixed
// bit rate. docs/measurement.md has the measurements the values come from.
#ifndef BUFFRX_PERFORMANCE_H
#define BUFFRX_PERFORMANCE_H

#include <string>
#include <vector>

#include "config.h"

namespace bx {

struct PerformancePlan {
  // The picture the mode runs and the bit rate was planned for. A smaller game is scaled up, a larger one down.
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int peak_kbps = 0;  // the fixed bit rate, for that picture
  int audio_tracks = 1;
  int audio_bitrate_kbps = 128;
  std::string codec = "h264";
  std::string scale_filter = "bilinear";
  std::string encoder_mode = "fast";  // the fast tier of the machine's own encoder
};

// The plan for a clip. The picture is the plan's own 1080p60 now.
PerformancePlan performance_plan();

// `user` with the plan's values in place of its own: the picture, the codec, the effort, the scaling and the rate. The
// user's object is not touched.
Config apply_performance_plan(const Config& user, const PerformancePlan& plan);

struct PerformanceDecision {
  bool active = false;    // whether the mode should be on
  bool deferred = false;  // the change is waiting for a recording to end
};
// When the mode is on. `when`: off, games, or always. `listed_game_running`: the game being captured is on the user's
// list. The always choice applies at once; a change caused by a game coming or going waits while a manual recording
// runs, because applying it rebuilds the pipeline. `currently_active` is what is running now.
PerformanceDecision performance_decide(PerformanceWhen when, bool listed_game_running, bool recording, bool currently_active);

// The config string for a mode, and the mode for a config string. An unknown string is off.
const char* performance_when_name(PerformanceWhen when);
PerformanceWhen parse_performance_when(const std::string& value);

}  // namespace bx

#endif  // BUFFRX_PERFORMANCE_H
