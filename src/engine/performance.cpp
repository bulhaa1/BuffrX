#include "performance.h"

#include "encoder_settings.h"

namespace bx {

PerformancePlan performance_plan() {
  // The picture is the plan's own: 1080p60 (decided with the user, 2026-11-05). A smaller game is scaled up by the
  // encoder, a larger one down; the rate below is planned for exactly this picture.
  PerformancePlan plan;
  plan.peak_kbps = preset_kbps(plan.codec, plan.width, plan.height, plan.fps, "standard");
  return plan;
}

Config apply_performance_plan(const Config& user, const PerformancePlan& plan) {
  Config out = user;
  out.output_width = plan.width;
  out.output_height = plan.height;
  out.capture_fps = plan.fps;
  out.codec = plan.codec;
  out.encoder = "auto";
  out.encoder_mode = plan.encoder_mode;
  out.encoder_options = Json::object();
  out.scale_filter = plan.scale_filter;
  out.audio_tracks = plan.audio_tracks;
  out.separate_tracks = false;
  out.audio_bitrate_kbps = plan.audio_bitrate_kbps;
  out.quality = "standard";
  out.peak_mode = "fixed";
  out.custom_kbps = plan.peak_kbps;
  return out;
}

const char* performance_when_name(PerformanceWhen when) {
  switch (when) {
    case PerformanceWhen::games: return "games";
    case PerformanceWhen::always: return "always";
    case PerformanceWhen::off: break;
  }
  return "off";
}

PerformanceWhen parse_performance_when(const std::string& value) {
  if (value == "games") return PerformanceWhen::games;
  if (value == "always") return PerformanceWhen::always;
  return PerformanceWhen::off;
}

PerformanceDecision performance_decide(PerformanceWhen when, bool listed_game_running, bool recording, bool currently_active) {
  PerformanceDecision decision;
  if (when == PerformanceWhen::always) {
    decision.active = true;
    return decision;
  }
  if (when == PerformanceWhen::off) {
    decision.active = false;
    return decision;
  }
  if (listed_game_running == currently_active) {
    decision.active = currently_active;
    return decision;
  }
  // A game came or went: that is a pipeline rebuild, so not in the middle of a recording.
  if (recording) {
    decision.active = currently_active;
    decision.deferred = true;
    return decision;
  }
  decision.active = listed_game_running;
  return decision;
}

}  // namespace bx
