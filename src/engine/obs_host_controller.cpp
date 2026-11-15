// The host's side of the dynamic encoder controller (plan 008, encoder_controller.h): the level table for the encoder that runs, the readings the
// controller is fed, the move itself (ObsHost::swap_encoder), what a move says to the person, and what the machine remembers between runs.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <obs.h>

#include "../shared/log.h"
#include "capacity_memory.h"
#include "encoder_controller.h"
#include "encoder_settings.h"
#include "gpu.h"
#include "obs_host.h"
#include "recording_watch.h"

namespace bx {

namespace {

constexpr uint32_t kControllerStartGraceMs = 30000;  // a buffer start, a capture swap, a hook attach, a wake: nothing moves for this long
constexpr uint32_t kControllerMoveGraceMs = 10000;   // after a move the new encoder settles (the 30 s between moves counts from the move)
constexpr uint32_t kRememberAfterMs = 60000;         // a level that held this long is the one remembered

bool same_settings(const std::vector<EncoderSetting>& a, const std::vector<EncoderSetting>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].name != b[i].name || a[i].value.dump() != b[i].value.dump()) return false;
  }
  return true;
}

}  // namespace

// What the memory is keyed by: this GPU and its driver, and what the encoder is asked to do (codec, the encoder asked for, picture, rate, preset).
// A new driver or another card starts from level 0 again.
std::string ObsHost::controller_memory_key() const {
  // What runs, not what is saved: Performance mode and the fallback ladder change the encoder, rate and picture without touching config_, and a
  // level is an index into the table built from what runs, so a key from config_ would apply one table's level to another.
  const Config running = effective_config();
  // The picture the encoder is given: the output size when one is set, else the canvas (a native output is the display's own size).
  const int width = running.output_width > 0 ? running.output_width : canvas_width_;
  const int height = running.output_height > 0 ? running.output_height : canvas_height_;
  return "ctl|" + gpu_identity_ + "|" +
         capacity_key(running.codec, encoder_id_, width, height, running.capture_fps, running.video_preset) + (performance_active() ? "|perf" : "");
}

// The levels this encoder has, relative to the settings it runs at level 0: an effort step that changes nothing for it is skipped.
void ObsHost::controller_build_levels() {
  const Config running = effective_config();
  const EncoderFamily family = encoder_family(encoder_id_);
  buffrx::LevelFacts facts;
  std::vector<EncoderSetting> before = configured_encoder_settings(family, running, 0, encoder_traits());
  for (int step = 1; step <= 3; ++step) {
    std::vector<EncoderSetting> now = configured_encoder_settings(family, running, step, encoder_traits());
    // The effort levels are offered only where a swap between them was proven joinable (tools/encoder-swap-probe.ps1: NVENC keeps its header
    // byte for byte across presets). x264 changes its stream layout with the preset (CABAC, reorder depth), and AMD and Intel are not proven on
    // this PC: they get the frame-rate levels only until a probe on that hardware says otherwise.
    facts.effort_changes[step] = family == EncoderFamily::Nvenc && !same_settings(before, now);
    before = std::move(now);
  }
  facts.fps = running.capture_fps;
  facts.divisor_ok = divisor_refused_.count(encoder_id_) == 0;
  facts.half_rate_allowed = family == EncoderFamily::X264 || performance_active();
  controller_levels_ = buffrx::controller_levels(facts);
  controller_fps_ = running.capture_fps;
}

// The table, the level this run starts at, and the encoder built at it. Called after the video encoder is created (configure_encoders), so every
// rebuild - a settings change, a wake, a fallback - starts the controller again for what the encoder now is.
void ObsHost::controller_prepare() {
  controller_build_levels();
  controller_key_ = controller_memory_key();
  int start = 0;
  if (!governor_disabled_) start = capacity_read_step(capacity_path_, controller_key_);
  start = std::clamp(start, 0, std::max(0, static_cast<int>(controller_levels_.size()) - 1));
  buffrx::controller_start(controller_, controller_levels_.size(), start, GetTickCount(), kControllerStartGraceMs);
  controller_remembered_ = start;
  controller_pending_move_ = -1;
  if (start > 0) BX_LOG_NOTE("this PC held level %d (%s) for %s last time: starting there", start,
                             buffrx::level_label(controller_levels_[static_cast<size_t>(start)], controller_fps_).c_str(), controller_key_.c_str());
  const buffrx::EncoderLevel& level = controller_levels_[static_cast<size_t>(start)];
  if (level.effort == governor_step_ && level.divisor == frame_divisor_) return;
  // The encoder was made at the previous level: make it again at this one. Creating an encoder opens no hardware session, so this is cheap.
  if (video_encoder_) {
    obs_encoder_release(video_encoder_);
    video_encoder_ = nullptr;
  }
  governor_step_ = level.effort;
  frame_divisor_ = level.divisor;
  std::string error;
  if (!build_video_encoder(encoder_id_, effective_config(), error)) {
    BX_LOG_WARN("the encoder would not start at the remembered level (%s); starting at full speed", error.c_str());
    governor_step_ = 0;
    frame_divisor_ = 1;
    if (build_video_encoder(encoder_id_, effective_config(), error)) {
      buffrx::controller_start(controller_, controller_levels_.size(), 0, GetTickCount(), kControllerStartGraceMs);
      controller_remembered_ = 0;
    }
  }
}

void ObsHost::controller_grace(uint32_t now_ms, uint32_t ms) {
  buffrx::controller_grace(controller_, now_ms, ms ? ms : kControllerStartGraceMs);
}

// The tick: the readings, the holds, and a move when the controller says so (queued for the action worker, never made here).
void ObsHost::controller_tick(uint32_t now_ms) {
  if (controller_levels_.size() < 2 || governor_disabled_) return;
  if (dedupe_wanted()) return;  // experimental chunk-backed saves cannot span encoder swaps yet
  const bool active = buffer_active_ && replay_ && obs_output_active(replay_);
  if (active && !controller_was_active_) buffrx::controller_grace(controller_, now_ms, kControllerStartGraceMs);
  controller_was_active_ = active;
  if (!active || !video_encoder_) return;
  // A source that is still arriving (a capture swap, a game hook that has not delivered, a display change) costs pictures that are not the encoder's.
  if (retiring_item_ || !capture_item_ready() || deep_idle_) buffrx::controller_grace(controller_, now_ms, kControllerStartGraceMs);
  // The action worker has not made the last one yet. One that never arrives (the queue was full) is forgotten after a while, so the controller
  // is not left waiting for ever.
  if (controller_pending_move_ >= 0) {
    if (now_ms - controller_pending_since_ < 15000) return;
    controller_pending_move_ = -1;
  }

  buffrx::ControllerHolds holds;
  holds.frozen = encoder_watch_.frozen;
  holds.manual_recording = recording_busy();
  holds.save_busy = save_in_flight_ || !save_queue_.empty();
  holds.device_hold = buffrx::governor_held(now_ms, last_fault_ms());
  holds.rebuilding = rebuild_gate_.busy() || encoder_swapping_.load();
  buffrx::ControllerWindow totals;
  totals.rendered = obs_get_total_frames() / static_cast<uint64_t>(std::max(1, frame_divisor_));
  totals.encoded = obs_encoder_get_encoded_frames(video_encoder_);
  totals.skipped = video_output_get_skipped_frames(obs_get_video());
  totals.lagged = obs_get_lagged_frames();
  // A probe, not a setting: BUFFRX_TEST_ENCODER_RATIO=@<file> holds the share of the pictures the encoder is read as having finished (a number, 0-1,
  // re-read every tick), so a test can make the encoder seem short and then healthy without needing a load that does it on this GPU.
  {
    static char path[512] = {0};
    static bool looked = false;
    if (!looked) {
      looked = true;
      char text[512] = {0};
      if (GetEnvironmentVariableA("BUFFRX_TEST_ENCODER_RATIO", text, sizeof(text)) > 0 && text[0] == '@') std::snprintf(path, sizeof(path), "%s", text + 1);
    }
    if (path[0]) {
      static double faked = 0.0;
      static uint64_t last_rendered = 0;
      double ratio = 1.0;
      if (std::FILE* file = std::fopen(path, "rb")) {
        char number[32] = {0};
        if (std::fread(number, 1, sizeof(number) - 1, file) > 0) ratio = std::clamp(std::atof(number), 0.0, 1.0);
        std::fclose(file);
      }
      if (totals.rendered < last_rendered) faked = 0.0;
      else faked += static_cast<double>(totals.rendered - last_rendered) * ratio;
      last_rendered = totals.rendered;
      totals.encoded = static_cast<uint64_t>(faked);
      totals.skipped = totals.rendered > totals.encoded ? totals.rendered - totals.encoded : 0;
    }
  }
  const buffrx::ControllerDecision decision = buffrx::controller_feed(controller_, controller_levels_, now_ms, totals, holds);
  controller_ratio_ = controller_.ratio;

  if (decision.move == buffrx::Move::floor_reached) {
    BX_LOG_NOTE("encoder controller: at the lowest level and still short (%s)", decision.reason.c_str());
    if (on_notice) {
      on_notice("Encoder at its lowest setting",
                "It still cannot encode every frame. Try a lower frame rate or quality in Settings.");
    }
    return;
  }
  if (decision.move == buffrx::Move::down || decision.move == buffrx::Move::up) {
    controller_pending_move_ = decision.to;
    controller_pending_since_ = now_ms;
    controller_pending_reason_ = decision.reason;
    controller_move_due_ = true;
    return;
  }
  // What held for a minute is what the machine remembers for this GPU, driver and picture.
  if (controller_.level != controller_remembered_ && controller_.moved_ever && now_ms - controller_.last_move_ms >= kRememberAfterMs &&
      controller_.ratio >= 0.95) {
    controller_remembered_ = controller_.level;
    if (controller_.level == 0) capacity_forget_key(capacity_path_, controller_key_);
    else capacity_write_step(capacity_path_, controller_key_, controller_.level);
    BX_LOG_NOTE("encoder controller: level %d is remembered for this GPU and driver", controller_.level);
  }
}

// The move the tick asked for, on the action worker: the swap, and then what the person is told (a card for a frame-rate change only).
bool ObsHost::apply_controller_move(std::string& error) {
  const int to = controller_pending_move_;
  const std::string reason = controller_pending_reason_;
  controller_pending_move_ = -1;
  if (to < 0 || to >= static_cast<int>(controller_levels_.size()) || to == controller_.level) return true;
  const int from = controller_.level;
  const buffrx::EncoderLevel& target = controller_levels_[static_cast<size_t>(to)];
  const buffrx::EncoderLevel& previous = controller_levels_[static_cast<size_t>(from)];
  const uint32_t now = GetTickCount();
  const bool ok = swap_encoder(target.effort, target.divisor, error);
  if (!ok) {
    // A vendor whose encoder will not take a divisor has no frame-rate levels: the table is made again without them, and the run goes on from here.
    if (error.find("frame-rate divisor") != std::string::npos) {
      divisor_refused_.insert(encoder_id_);
      const int effort = previous.effort;
      controller_build_levels();
      int keep = 0;
      for (size_t i = 0; i < controller_levels_.size(); ++i)
        if (controller_levels_[i].effort == effort && controller_levels_[i].divisor == 1) keep = static_cast<int>(i);
      if (!controller_key_.empty()) capacity_forget_key(capacity_path_, controller_key_);
      buffrx::controller_start(controller_, controller_levels_.size(), std::min(keep, static_cast<int>(controller_levels_.size()) - 1), now, kControllerMoveGraceMs);
    } else {
      buffrx::controller_move_failed(controller_, now);
    }
    BX_LOG_WARN("encoder controller: the move from level %d to %d did not happen: %s", from, to, error.c_str());
    return false;
  }
  buffrx::controller_moved(controller_, controller_levels_, to, now, kControllerMoveGraceMs);
  BX_LOG_NOTE("encoder controller: level %d -> %d (%s): %s; effort %d, divisor %d, %d fps in", from, to,
              target.effort > previous.effort || target.divisor > previous.divisor ? "down" : "up", reason.c_str(),
              target.effort, target.divisor, controller_fps_);
  // A card only when the frame rate changes, because that is what the person can see in the clip: effort is quiet.
  if (on_notice && target.divisor != previous.divisor) {
    const int fps = std::max(1, controller_fps_ / std::max(1, target.divisor));
    if (target.divisor > previous.divisor) {
      on_notice("Clips at " + std::to_string(fps) + " fps",
                "Your GPU cannot encode " + std::to_string(controller_fps_) + " fps in real time right now, so clips are made at " + std::to_string(fps) +
                    " fps. The buffer keeps what it had. It goes back by itself when the GPU can.");
    } else {
      on_notice("Clips back at " + std::to_string(fps) + " fps", "The GPU keeps up again.");
    }
  }
  return true;
}

// "Start from full speed again": the memory for this GPU and picture is dropped and the encoder goes back to the person's own settings.
void ObsHost::controller_reset() {
  if (!controller_key_.empty()) capacity_forget_key(capacity_path_, controller_key_);
  controller_remembered_ = 0;
  if (controller_.level != 0 && video_encoder_ && !controller_levels_.empty()) {
    controller_pending_move_ = 0;
    controller_pending_since_ = GetTickCount();
    controller_pending_reason_ = "full speed asked for";
    controller_move_due_ = true;
  }
}

ObsHost::ControllerStatus ObsHost::controller_status() const {
  ControllerStatus status;
  status.level = controller_.level;
  status.levels = static_cast<int>(controller_levels_.size()) - 1;
  status.effort = governor_step_;
  status.divisor = frame_divisor_;
  status.canvas_fps = controller_fps_;
  status.fps = std::max(1, controller_fps_ / std::max(1, frame_divisor_));
  if (controller_.level > 0 && controller_.level < static_cast<int>(controller_levels_.size())) {
    status.label = buffrx::level_label(controller_levels_[static_cast<size_t>(controller_.level)], controller_fps_);
  }
  status.enabled = !governor_disabled_ && controller_levels_.size() > 1;
  return status;
}

}  // namespace bx
