// What the tick should do about a record request whose capture source has no frame yet, decided
// without libobs so the rule can be tested.
//
// The bug this exists for: an idle engine releases the capture source, so the first record click
// recreates it and then found no frame. The click failed with "waiting for the capture source to
// produce frames", and a second click worked only once a frame had arrived. The buffer already waits
// for its first frame; a record request now waits the same way instead of being thrown away.

#ifndef BUFFRX_RECORDING_WATCH_H
#define BUFFRX_RECORDING_WATCH_H

#include <cstdint>

namespace buffrx {

// How long a record request waits for the capture source's first frame. A game hook gets six seconds
// before the engine falls back to the game's window, and the fallback needs a moment to deliver.
constexpr uint32_t kRecordingStartTimeoutMs = 15000;

enum class RecordingAction {
  none,     // no request is waiting
  wait,     // the request waits for the capture source's first frame
  start,    // the source has a frame: start the output now
  give_up,  // no frame within the timeout: clear the request and tell the user
};

inline RecordingAction recording_action(bool pending, bool source_ready, uint32_t waited_ms) {
  if (!pending) return RecordingAction::none;
  if (source_ready) return RecordingAction::start;
  if (waited_ms >= kRecordingStartTimeoutMs) return RecordingAction::give_up;
  return RecordingAction::wait;
}

// Record starts or stops a standalone recording in every Timeline state.
enum class RecordAction {
  ManualStart,  // nothing is being recorded: start a manual recording
  ManualStop,   // a manual recording runs: stop it
};

// Recording has its own output; Timeline's state and selections do not change the key's meaning.
inline RecordAction record_action(bool manual_active) {
  return manual_active ? RecordAction::ManualStop : RecordAction::ManualStart;
}

// A recording is running only when an output behind the flag still exists. The stop path can leave
// `recording_active_` set after the output is gone (a chunk handover, a force stop); the flag alone then
// answers "recording" to everything - the Record key becomes a stop, the Timeline waits in paused_manual.
inline bool recording_busy_flag(bool have_output, bool active, bool pending) {
  return (have_output && active) || pending;
}

// The recording the person asked for: an output behind the flag that is not Timeline's segment, or a start still waiting
// for its first frame. Timeline shares the output and `active`, so `recording_busy_flag` alone cannot tell them apart; a
// rebuild that resumed it by that flag started a manual recording out of every settings change (the Performance switch).
inline bool manual_recording_flag(bool have_output, bool active, bool pending, bool timeline_writer) {
  return (have_output && active && !timeline_writer) || pending;
}

// A manual recording cannot start beside an output libobs still reports active. An output reported
// inactive is finished: its stop landed, or its signal came after the engine let go, and only its release
// is owed - so it must never answer the next click with "still being finished".
inline bool recording_start_blocked(bool have_output, bool output_active) { return have_output && output_active; }

// How long an output may take to report its stop before it is forced. The person pressing Stop should not
// wait for the encoder's backlog (16 s measured with a heavy NVENC setting, over 30 s under a game): the
// file is closed with what the encoder has delivered.
constexpr uint32_t kUserStopGraceMs = 8000;
inline uint32_t recording_stop_grace_ms() { return kUserStopGraceMs; }

// Whether an output's stop signal was a surprise: the output was still open (not already let go of), and
// nobody asked for it and it was not one the engine had to force.
inline bool recording_stop_unexpected(bool output_open, bool stop_requested, bool forced) {
  return output_open && !stop_requested && !forced;
}

// A recording whose output stopped by itself (the encoder or the muxer failed, the disk refused a write)
// used to end the session in silence: the person pressed record, played, and found a recording that had
// stopped minutes before with nothing to say so. The engine now says so and carries on in a new chunk -
// a few times, because an output that fails again and again is a problem to report, not to retry for ever.
constexpr int kMaxRecordingResumes = 3;
constexpr uint32_t kRecordingResumeWindowMs = 10 * 60 * 1000;

struct ResumeBudget {
  uint32_t window_start_ms = 0;
  int used = 0;
};

// Asks for one resume and spends it. False once kMaxRecordingResumes have been used inside the window.
inline bool recording_resume_allowed(ResumeBudget& budget, uint32_t now_ms) {
  if (budget.used == 0 || now_ms - budget.window_start_ms >= kRecordingResumeWindowMs) {
    budget.window_start_ms = now_ms;
    budget.used = 0;
  }
  if (budget.used >= kMaxRecordingResumes) return false;
  ++budget.used;
  return true;
}

}  // namespace buffrx

#endif  // BUFFRX_RECORDING_WATCH_H
