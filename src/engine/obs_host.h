// The BuffrX engine: libobs linked in-process, owned by one class.
//
// This is the whole product's core. It knows about capture sources, encoders, the replay buffer and
// the recording output, and nothing about HTTP, Electron, Node or any web view. Everything the UI
// shows comes out of `stats()` or an event callback.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture_enum.h"
#include "command_policy.h"
#include "config.h"
#include "display_target.h"
#include "encoder_governor.h"
#include "encoder_watch.h"
#include "idle_policy.h"
#include "memory_policy.h"
#include "performance.h"
#include "recording_watch.h"
#include "session_store.h"
#include "timeline_chunks.h"
#include "timeline_pins.h"
#include "timeline_retention.h"
#include "timeline_state.h"
#include "work_queue.h"

struct obs_source;
struct obs_output;
struct obs_encoder;
struct obs_scene;
struct obs_scene_item;
struct obs_data;
struct obs_volmeter;
struct calldata;

namespace bx {

// DisplayInfo lives in display_target.h with the rules that choose one.

// WindowInfo lives in capture_enum.h (included above): the enumeration and its wire shape belong
// together now that no libobs source is involved in producing it.

struct AudioDeviceInfo {
  std::string id;
  std::string name;
  bool is_default = false;
};

struct EngineStats {
  bool buffer_active = false;
  std::string buffer_state = "off";  // off | starting | on | stopping (buffer_watch.h)
  bool recording = false;
  bool recording_pending = false;  // a record click is waiting for the capture source's first frame
  bool capture_ready = false;
  double capture_fps = 0.0;
  double encoder_fps = 0.0;
  uint64_t total_frames = 0;
  uint64_t total_bytes = 0;  // everything the replay output has taken since it started (video and audio)
  uint64_t dropped_frames = 0;
  // libobs's own view of the video thread and the encoder — what OBS's stats panel reports. The
  // output's counters say dropped=0 while the video thread is missing deadlines, so both are kept.
  uint64_t video_frames = 0;    // frames the video thread rendered
  uint64_t video_lagged = 0;    // frames it missed its deadline on
  uint64_t encoder_frames = 0;  // frames the video encoder actually encoded
  double render_time_ms = 0.0;  // average time to render a frame
  double active_fps = 0.0;      // the video thread's current rate
  uint64_t saved_clips = 0;
  double ring_seconds = 0.0;       // the length asked for, which the ring always keeps
  double ring_filled = 0.0;        // seconds of footage it holds now (0..ring_seconds)
  double recording_seconds = 0.0;  // how long the recording in progress has run
  double bitrate_kbps = 0.0;       // the peak rate the encoder may reach
  // What the replay ring holds right now, read from the ring itself (ring_output.cpp): its bytes, the footage they span and the
  // average rate. `ring_measured` is false until the ring has been filling for a few seconds.
  bool ring_measured = false;
  uint64_t ring_used_bytes = 0;
  double ring_held_seconds = 0.0;
  double ring_avg_kbps = 0.0;
  // The ring's two tiers (ring_spill.h): footage in RAM, footage in the reserved file, bytes written to it since the buffer started,
  // "off" / "on" / "unavailable", and a disk that could not take what was due (the footage then waits in RAM).
  uint64_t ring_ram_bytes = 0;
  uint64_t ring_disk_bytes = 0;
  uint64_t ring_spilled_bytes = 0;
  uint64_t ring_received_bytes = 0;  // everything the encoder has handed the ring since the buffer started
  // The Timeline dedupe: footage that lives in chunk files instead of RAM or the ring file, bytes that left RAM that way since the buffer started, and the chunks
  // matched to the ring / probed and still waiting.
  uint64_t ring_chunk_bytes = 0;
  uint64_t ring_deduped_bytes = 0;
  int ring_chunks_linked = 0;
  int ring_chunks_waiting = 0;
  int ring_disk_state = 0;
  bool ring_stalled = false;
  double process_mb = 0.0;               // this process's working set: what Task Manager shows
  bool asleep = false;                   // idle long enough that the canvas ticks at the idle rate (idle_policy.h)
  // The video encoder is encoding fewer frames than the video thread renders (encoder_watch.h): the clips
  // stutter however smooth the game is. `encoder_ratio` is the share it manages.
  bool encoder_behind = false;
  double encoder_ratio = 1.0;
  int display_hz = 0;  // the capture's display refresh rate, 0 unknown
  float level_desktop = 0.0;       // peak level on a -60..0 dBFS scale, 0..1
  float level_mic = 0.0;           // RMS level on a -42..0 dBFS scale, 0..1
  bool audio_meters_active = false;
  std::string capture_mode;
  std::string capture_target;
  int width = 0;
  int height = 0;
  int fps = 0;
  std::string encoder_id;
  std::string last_error;
  uint64_t restarts = 0;
};

// A manual buffer stop ends the Timeline session; the internal pauses (a sleep, the lock, a rebuild, a
// settings change) keep it for kTimelineSessionPauseMs. The decision is here so the one manual-stop path
// and its checks agree, and so it can be checked without libobs.
enum class TimelineEndAction {
  none,        // nothing is open: a second stop is harmless
  stop_writer, // a segment is still writing: stop it, and close once it has let go
  close_now
};

// The lease a save holds on the session it is reading (commands.cpp: timeline_save). It is what keeps the
// footage it copies from being pruned; the stop does not wait for it, because the pin already protects the files.
constexpr const char* kTimelineSaveOwner = "timeline-save";

inline TimelineEndAction timeline_end_action(bool session_open, bool segment_open) {
  if (!session_open) return TimelineEndAction::none;
  if (segment_open) return TimelineEndAction::stop_writer;
  return TimelineEndAction::close_now;
}

class ObsHost {
public:
  using PathCallback = std::function<void(const std::string& path)>;
  using ErrorCallback = std::function<void(const std::string& component, const std::string& message)>;

  PathCallback on_clip_saved;  // called with the finished clip; last_clip_seconds() says how much was asked for
  PathCallback on_screenshot_saved;
  ErrorCallback on_error;
  // Something worth a calm notification that is not an error: the buffer recovered on its own.
  std::function<void(const std::string& title, const std::string& detail)> on_notice;
  // Fires when a chunk of a session is indexed and again when the session closes, so the protocol can
  // tell the interface what exists without it asking.
  std::function<void(const std::string& session_id, bool closed)> on_session_changed;
  // Fires after a marker has been written into the open session's index (never under the host's lock).
  std::function<void(const std::string& session_id, const SessionMarker& marker)> on_marker_added;

  ~ObsHost();

  // Starts libobs, loads the modules the product needs and creates the scene. `runtime_root` is the
  // package root that holds bin/64bit, obs-plugins/64bit and data/.
  bool initialize(const std::string& runtime_root, const std::string& data_root, const Config& config,
                  std::string& error);
  void shutdown();
  bool initialized() const { return initialized_; }
  // The last video start was refused because the machine's memory is nearly full (memory_policy.h); `error` then holds the words for the person.
  bool memory_full() const { return memory_full_; }
  // The length the clip just filed was asked for: the panel tells a quick clip from a buffer save by it.
  int last_clip_seconds() const { return last_clip_seconds_.load(); }

  // What this PC proved it can encode (capacity.json): a step it settled on for a codec, picture, rate and quality, so the
  // next start begins there. Advisory only; the stored settings are never rewritten.
  int capacity_remembered_step(const std::string& key) const;
  void capacity_remember(const std::string& key, int step) const;

  // Applies capture, encoder and audio settings. Set `restart_outputs` when the change requires the
  // replay buffer to be recreated (encoder, resolution, frame rate, codec, capture source).
  bool apply(const Config& config, bool restart_outputs, std::string& error);

  bool start_buffer(std::string& error);
  void stop_buffer();
  // A manual stop ends the Timeline session now, not after the pause grace: the segment is stopped here
  // and the tick closes the index once the writer has let go. It never waits. Called before stop_buffer by
  // the one manual-stop path; with nothing open a second call is harmless.
  void end_timeline();
  int save_clip(int seconds, std::string& error);  // returns 0 on success, else a Win32-ish code
  bool start_recording(std::string& error);
  // The recording body, wrapped by start_recording so an exception is named rather than fatal. `timeline`
  // is the Timeline writer's start: its own session, fragmented MP4, and never encoders of its own.
  bool start_recording_inner(std::string& error, bool timeline = false);
  // The Timeline writer (timeline_state.h): segments on the ring's shared encoders into a "timeline"
  // session, written only while the tick's state says so (Task 5). Starting opens the session and its
  // first segment; stopping lets the tick index the last one and close the session. Never pauses the
  // buffer, and a machine that cannot share the ring's encoders records nothing rather than taking them.
  bool start_timeline(std::string& error);
  void stop_timeline();
  // The tick's Timeline management: the protocol builds the facts it can see (the config, the buffer,
  // the captured game, the foreground, the preview) and asks here once per tick. Opening, pausing,
  // resuming and closing the session live together because they share the one recording output. A pause
  // closes the segment and keeps the session open for up to kTimelineSessionPauseMs; a stop closes both.
  // Engine lock held.
  void poll_timeline(uint32_t now, const TimelineFacts& facts);
  // The state the last poll decided; status.get reads it from the pipe threads.
  TimelineState timeline_state() const { return timeline_state_.load(); }
  // The buffer is asked to run: what Timeline writes beside.
  bool buffer_wanted() const { return buffer_intent_.load(); }
  // A Timeline session is open, including a pause between segments.
  bool timeline_writer_active() const { return timeline_writer_.load(); }
  // Ends an open Timeline session now, paused or writing. A segment still writing is stopped here and the
  // session is closed by the tick that sees the stop.
  void close_timeline(uint32_t now);
  // The leases a save, a preview or the ring's dedupe holds while it reads Timeline footage (timeline_pins.h). A lease
  // is granted only here, under the retention gate, and never over footage a sweep has reserved for deletion: empty
  // means the footage is going and the caller must not claim it (timeline_retention.h). `seen_generation` is the number
  // the caller read before it read the session's index (`timeline_removal_seen`), so a removal that ran in between
  // refuses a lease decided on a stale index.
  std::string take_timeline_lease(const TimelinePin& pin, int64_t now_wall_ms, uint64_t seen_generation);
  // Releases one lease by id, or every lease an owner holds on one session. Both under the same gate, and both a
  // read-modify-write: a sweep can never decide between the read and the write.
  bool release_timeline_lease(const std::string& id, int64_t now_wall_ms);
  size_t release_timeline_leases(const std::string& owner, const std::string& session_id, int64_t now_wall_ms);
  // The leases in force, as a copy, for a reader that only reports or decides. The caller holds the engine lock;
  // take_timeline_lease is how they are granted.
  std::vector<TimelinePin> timeline_pins() const { return bx::timeline_leases_snapshot(); }
  // Asks the open Timeline segment to end now: a save that needs the newest footage cannot read a file its
  // writer still holds. OBS's own split procedure asks the muxer for a boundary on its next keyframe; the
  // output is not stopped, so the ring keeps its encoders and the next file is written by the same process.
  // False when nothing is being written, or when the output cannot split (a clear line is logged).
  bool request_timeline_rotation();
  // One retention sweep over the Timeline's own store, on the wall clock the caller passes: the window
  // from the config, the pins above. The tick asks once a minute; prune_sessions asks at a session
  // boundary too.
  void prune_timeline_now(int64_t now_wall_ms);
  // The sweep with Timeline off: stopped footage by the keep-hours setting, recordings by their budget.
  void prune_idle_now() { prune_sessions(""); }
  // Starts a request that was waiting for the capture source's first frame. Called on the action
  // worker, never on the tick: creating an encoder is not a tick-sized job.
  bool start_pending_recording(std::string& error);
  // The tick asks for one start; main.cpp takes it and queues the action worker. A start queued from
  // the tick itself would create the encoder on the thread that draws the panel.
  bool take_recording_start_due();
  void stop_recording();  bool take_screenshot(const std::string& format, std::string& path_out, std::string& error,
                                              const std::string& game_title = {});

  void tick();  // every 250 ms: stats, output signals, watchdog, save queue
  // A closed chunk is indexed only once its file is final (file_final.h): the output has let go of it,
  // but the muxer's own handle can outlive the stop by a moment, and an index entry for a file that is
  // still being written is a chunk a cut or a playback would fail on. Called from the tick. A file still
  // held past kPendingPublishTimeoutMs is logged and left out of the index; it is never deleted.
  void pending_chunk_publish(uint32_t now);

  // The encoder governor (encoder_governor.h). The tick raises the flag when the encoder cannot keep up and the
  // next step is due; main.cpp takes it and queues the action worker, because a step rebuilds the encoder and
  // the outputs and must not run on the window thread. `apply_governor_step` takes the step.
  bool take_governor_step_due() { return governor_due_.exchange(false); }
  bool apply_governor_step(std::string& error);
  int governor_step() const { return governor_step_; }

  EngineStats stats() const;

  // The tick raises this once the engine has been idle past the grace and the sources are released; main.cpp takes
  // it and queues the action worker, because sleeping is a video reset and does not belong on the window thread.
  bool take_deep_idle_due() { return deep_idle_due_.exchange(false); }
  // Takes the pipeline down and drops the canvas to the idle rate (idle_policy.h). Refuses, harmlessly, if the
  // engine is no longer idle by the time it runs. The next start, recording or screenshot wakes it.
  bool enter_deep_idle();
  bool asleep() const { return deep_idle_; }

  // Performance mode (performance.h): always is on for the user's switch, games while a listed game is captured.
  // It is a layer over the user's settings (effective_config), never a rewrite of them.
  bool performance_active() const { return config_.performance_when == PerformanceWhen::always || performance_game_; }
  const char* performance_reason() const { return config_.performance_when == PerformanceWhen::always ? "always" : (performance_game_ ? "game" : "off"); }
  // The game being captured is on the user's list (or left it). A change of the mode rebuilds the pipeline, like any video
  // change. Returns false and fills `error` only if the rebuild failed.
  bool set_performance_game(bool listed, std::string& error);
  // A sleep takes the graphics device and every encoder session with it, and libobs never reports that as a fault.
  // Builds the running settings' pipeline again (the same full rebuild a Performance mode change makes live).
  // Caller holds the engine lock; it takes seconds, so never from the window thread.
  bool recover_after_sleep(std::string& error);
  // Capture follows focus while the Timeline records: a captured game that is not in front hands the picture to the display.
  // A layer, like Performance mode: it lives in memory over the user's config (which is never rewritten or saved for it),
  // and every build of the capture source honours it. `reconfigure` swaps the source now (under the ring, as a detection
  // does); false only records the state, for the moments when no source is wanted or the caller is about to swap itself.
  bool set_capture_follow(bool display, bool reconfigure, std::string& error);
  // The capture mode in effect: the user's, except while the follow layer shows the display in place of a game.
  std::string capture_mode_now() const { return follow_display_ && config_.capture_mode == "game" ? std::string("display") : config_.capture_mode; }
  // What the mode records on this machine's capture target, at the user's own size and rate (what it would, when it is
  // off): for the page's one-line summary.
  bx::PerformancePlan performance_plan_now() const { return bx::performance_plan(); }
  // A manual recording runs or is about to: a game-caused change of the mode waits for it to end. Timeline's own segment is
  // not one (manual_recording_flag): with Timeline writing, a game on the list never switched the mode on.
  bool recording_busy() const {
    return manual_active_ || buffrx::manual_recording_flag(recording_ != nullptr, recording_active_, recording_pending_, timeline_writer_.load());
  }

  // Only the fallback manual writer occupies Timeline's output; the shared-encoder writer does not.
  bool recording_blocks_timeline() const {
    return (!timeline_writer_ && recording_ != nullptr) || (recording_pending_ && !shared_encoders_ok_);
  }

  // Something wants frames: the buffer (running or waiting for its first frame) or a recording. The game
  // detection asks this every tick, so it is a flag read and not a stats() build.
  bool capture_wanted() const { return buffer_intent_ || recording_active_ || manual_active_ || recording_pending_; }

  // True while a pipeline rebuild (ObsHost::apply) is running. The buffer is empty for that moment by
  // construction; the interface reports it instead of showing a ring that is about to be discarded.
  bool rebuilding() const { return rebuild_gate_.busy(); }

  // The session the recording output is writing into, empty when no session is open. A single-file
  // recording (chunking turned off) has no session.
  std::string current_session_id() const;
  // The chunk a Timeline session is writing right now: its file, the tick it began on (the same absolute
  // clock a chunk record keeps, so the caller turns it into session time), how many bytes its writer has
  // put on disk, and the nominal chunk length. Empty `file` when nothing is open. The size is read with a
  // fresh handle: while the writer holds the file, directory metadata still says zero.
  struct LiveChunk {
    std::string file;
    uint32_t start_ms = 0;
    int64_t bytes = 0;
    int chunk_seconds = 0;
  };
  LiveChunk live_chunk() const;
  // Appends a marker to the open session. The host is the index's only writer while a session is open:
  // a marker written to disk behind its back would be lost at the next chunk. False with
  // "no session open" when nothing is recording into a session.
  bool add_marker(const std::string& kind, const std::string& label, std::string& session_id,
                  SessionMarker& marker, std::string& error);
  // The same, at `at_tick` (the index's tick clock) and only when `session_id` is the open session: the
  // Timeline marks the moment under its playhead or a saved range, not the moment the button was pressed.
  // False with "not the open session" otherwise, so the caller writes a closed session's index itself.
  bool add_marker_at(const std::string& session_id, const std::string& kind, const std::string& label,
                     uint32_t at_tick, SessionMarker& marker, std::string& error);
  bool remove_marker_at(const std::string& session_id, uint32_t at_tick, const std::string& kind,
                        const std::string& label, std::string& error);
  // The same by the marker's own id, so an identical duplicate in the open session is told apart. False
  // with "not the open session" otherwise, as remove_marker_at is.
  bool remove_marker_by_id(const std::string& session_id, const std::string& marker_id, std::string& error);

  // Meter attachment changes under the engine lock; sampling and reads use atomics on the window thread.
  void set_audio_metering(bool enabled);
  void sample_audio_levels(bool sample_mic);
  bool audio_meters_active() const { return audio_meters_active_.load(std::memory_order_relaxed); }
  float level_desktop() const { return level_desktop_.load(std::memory_order_relaxed); }
  float level_mic() const { return level_mic_.load(std::memory_order_relaxed); }

  std::vector<DisplayInfo> displays();
  std::vector<WindowInfo> windows();
  std::vector<WindowInfo> game_windows();
  std::vector<AudioDeviceInfo> audio_devices(bool inputs);

  // Diagnostics shown in the UI's About/diagnostics panel.
  std::string obs_version() const;
  // The codec this GPU can encode best: AV1 when a hardware AV1 encoder is registered (FFmpeg's software one does not
  // count), H.264 otherwise. Read once on the first start, then the stored choice is the person's.
  std::string preferred_codec() const;
  // The chosen encoder's own property list, as data: one entry per simple control (name, kind, range
  // or options, and the value in force). The advanced encoder page renders this, so a knob a vendor
  // adds appears with no engine change.
  Json encoder_properties(bool configured = false) const;
  // One line describing the replay output's own view of itself: last error, last file, frame counts.
  std::string replay_diagnostics() const;

private:
  bool start_obs(std::string& error);
  bool load_modules(std::string& error);
  bool configure_video(const Config& config, std::string& error);
  // window_fallback: a game target captured as a WGC window instead of through the game hook.
  // layered: the source being replaced is delivering frames, so it stays under the new one until the new one has
  // a frame of its own (settle_retiring_capture). A game capture takes a second or more to hook and a display
  // capture a few hundred ms; removing the old source first left the ring on nothing for that long, which was
  // the stutter when a game was detected.
  bool configure_capture(const Config& config, std::string& error, bool window_fallback = false, bool layered = false);
  // The tick's check: a game capture with no frame after kGameHookGraceMs falls back to WGC. The hook
  // injects when the capture starts, so a game it can reach has a frame in well under a second; six
  // seconds of waiting was the whole of a slow "buffer on" on a machine whose hook never takes.
  void check_game_fallback();
  // The replaced capture source, kept under the new one until the new one delivers (or kRetireMaxMs passes).
  void settle_retiring_capture();
  void drop_retiring_capture();
  // The tick's check: the display the capture resolved to is not the one the settings mean now (unplugged,
  // rearranged, a docking station left), so the source follows without the config being rewritten.
  void check_display_present();
  struct obs_scene_item* retiring_item_ = nullptr;
  uint32_t retiring_since_ms_ = 0;
  static constexpr uint32_t kRetireMaxMs = 4000;
  // The capture and audio sources are torn down while the engine has nothing to do (the buffer off
  // and no manual record) and made again when it has: a live source costs CPU and GPU for frames
  // nobody watches, and the audio one holds the sound card open.
  void release_idle_sources();
  bool ensure_capture_sources(std::string& error);
  // Builds the video, encoders and outputs again at the configured rate after enter_deep_idle. A no-op awake.
  bool wake_from_deep_idle(std::string& error);
  // The canvas the config asks for: sized from the capture target, stored in canvas_width_/height_.
  CanvasPlan plan_canvas_for(const Config& config);
  // `user` with the layers over it that decide what is really built: Performance mode's plan. (The codec fallback is
  // effective_config's.) The target size is the capture target's, from plan_canvas_for.
  Config layered(const Config& user) const;
  bool performance_game_ = false;
  bool force_video_rebuild_ = false;
  int target_width_ = 0;
  int target_height_ = 0;
  bool deep_idle_ = false;
  std::atomic<bool> deep_idle_due_{false};
  uint32_t deep_idle_failed_ms_ = 0;
  // The settings of the encoder that was running when the engine went to sleep: the Video page reads them while
  // there is no encoder (encoder_properties).
  struct obs_data* encoder_snapshot_ = nullptr;
  bool sources_released_ = false;
  // The sources are kept this long after the engine goes idle: a buffer stopped and started again (a settings
  // change, a pause, a restart from the panel) finds them where it left them instead of hooking the game
  // and opening the sound card all over again, which is most of why a restart felt slow.
  static constexpr uint32_t kIdleReleaseGraceMs = 15000;
  uint32_t idle_since_ms_ = 0;
  bool game_fallback_ = false;
  uint32_t capture_created_ms_ = 0;
  static constexpr uint32_t kGameHookGraceMs = 1500;
  // The GDI device name the live display source resolved to, and when it was last compared with the settings.
  std::string capture_display_device_;
  uint32_t display_check_at_ = 0;
  // The missing display already reported as fallen back to the primary, so the 2 s check says it once.
  std::string display_warned_name_;
  static constexpr uint32_t kDisplayCheckMs = 2000;
  bool configure_audio(const Config& config);
  void apply_mic_filters(const Config& config);
  void attach_meter(obs_source* source, obs_volmeter** meter, std::atomic<float>* into);
  void detach_meter(obs_volmeter** meter);
  bool update_audio_channel(obs_source*& source, obs_volmeter** meter, std::atomic<float>* level_db,
                            std::string& bound_device, const char* source_id, const char* source_name,
                            const char* device_id, float volume, bool muted, uint32_t mixers, bool wanted,
                            uint32_t view_channel);
  bool configure_encoders(const Config& config, std::string& error);
  // The recording output gets its own encoder instances, copied from the ring's settings: libobs keeps
  // per-packet timing per encoder, and sharing one between two outputs corrupts it.
  bool create_recording_encoders(std::string& error);
  void destroy_recording_encoders();
  // One chunk: encoders + output + file. Called once per recording and again at every boundary.
  bool start_recording_output(std::string& error);
  // Disconnects, stops and releases the recording output and its encoders. Never called from inside a
  // libobs signal: the output's own "stop" runs on the output's thread, and releasing it there hangs.
  void release_recording_output();
  // The same for the open chunk's output alone, leaving a chunk that is already waiting to take over.
  void release_current_output();
  // The recording shares the ring's encoders (one NVENC session, so the buffer keeps running and clips keep
  // working). The old way - its own encoders, the buffer paused - is the fallback if libobs refuses the shared one.
  obs_output* create_chunk_output(const std::string& path, obs_encoder* video, const std::vector<obs_encoder*>& audio,
                                  void (*on_stop)(void*, calldata*), bool timeline, std::string& error);
  bool start_shared_manual_recording(std::string& error);
  // A manual recording cannot take the output slot while a Timeline writer or another recording holds
  // it: the start waits for the tick instead. True when it deferred, and `error` is cleared.
  bool defer_recording_start(std::string& error);
  void stop_primary_recording();
  void process_manual_recording();
  void release_manual_recording();  // recording_mutex_ held
  // Releasing an output that never took a frame can wait on the encoder for seconds when the encoder is
  // behind, and the caller is the window thread. The release is handed to a thread of its own; the file it
  // leaves behind is deleted once the output has let go of it.
  struct ReapJob {
    obs_output* output = nullptr;
    std::string delete_path;
    // The abandoned output's own encoders (a recording that did not share the ring's): the output holds its own
    // references, so these are let go once it has, never before.
    obs_encoder* video_encoder = nullptr;
    std::vector<obs_encoder*> audio_encoders;
  };
  WorkQueue<ReapJob> reaper_{8};
  void reap_output(obs_output* output, const std::string& delete_path, obs_encoder* video_encoder = nullptr,
                   std::vector<obs_encoder*> audio_encoders = {});
  // The first frame an output writes is when its file really begins, and a file whose first frame joined an
  // encoder already running begins on the next keyframe instead of at the click.
  void stamp_chunk_start();
  // The nominal file length and the container a chunk is written in: a manual recording is one file in
  // the user's container; a Timeline segment is asked for in 5 s steps and is always fragmented MP4.
  int chunk_seconds_now() const;
  const char* chunk_container() const;
  // The tick's half of a file the muxer split: the path only queued by the signal is taken here, the span
  // that closed goes to the index queue and the open one follows the new file. Never blocks: an index entry
  // still waits for its writer (pending_chunk_publish).
  void poll_timeline_split(uint32_t now);
  // Drops what the splitter reported but the tick has not taken (a new session, an abandoned output).
  void clear_split_queue();
  // One closed span into the index queue, with the session's wall clock. Caller holds recording_mutex_.
  void queue_session_chunk(const std::string& file, uint32_t start_ms, uint32_t end_ms);
  // The tick's half of a closed chunk: indexes it, releases the output, then opens the next chunk or
  // closes the session. The "stop" signal only raises chunk_closed_.
  void process_closed_chunk();
  // Both with recording_mutex_ held and the output already released: index the chunk that was being
  // written, and close the open session (prune, then on_session_changed).
  void index_open_chunk(uint32_t now);
  // pending_chunk_publish's body, for the shutdown path, which already holds recording_mutex_.
  void pending_chunk_publish_locked(uint32_t now);
  void close_session(uint32_t now);
  // end_timeline's worker: one step of a manual stop, from the stop itself and from every tick that follows
  // (a segment has to be stopped and its last chunk indexed before the session can close). Never waits.
  void finish_timeline_close(uint32_t now);
  // Deletes the oldest sessions down to recordings_budget_gb (no-op when unlimited), sparing `open_id`.
  // The sweep walks the sessions directory and deletes from it, so it is queued to prune_worker_ instead
  // of running under the engine lock every command and the window thread share.
  void prune_sessions(const std::string& open_id);
  struct PruneJob {
    int64_t budget_bytes = 0;
    std::string open_id;
    int timeline_minutes = 0;
    int64_t now_wall_ms = 0;
    std::vector<DroppedChunk> delete_chunks;  // chunks the open Timeline session dropped from its own list: their files go here
  };
  // The Timeline session being written. Read by the retention worker when it runs (not when it was queued): the sweep leaves
  // that session to its writer, which trims its own list on the tick.
  std::mutex open_timeline_mutex_;
  std::string open_timeline_id_;
  void note_open_timeline(const std::string& id);
  // Dropped chunks Windows still held at the last sweep: tried again at the next.
  std::mutex held_mutex_;
  std::vector<DroppedChunk> held_chunks_;
  WorkQueue<PruneJob> prune_worker_{2};
  void run_prune(const PruneJob& job);
  bool create_outputs(std::string& error);
  void destroy_outputs();
  void process_save_queue();
  // The tick's half of a finished save: asks the output what its "saved" signal came to - the ring's
  // `save_result` verdict, or the file `get_last_replay` reports for an output without one - and hands
  // a produced clip to the post-save worker, which trims it to the requested length, verifies it and
  // reports it. A save the output says failed is kept on disk but never filed or announced.
  void process_saved_replay();
  struct SaveRequest;
  void finish_save(const std::string& path, const SaveRequest& request, int peak_kbps);
  // Forces a replay stop that is still unwinding, never waits for it. True once it is down.
  // A start issued while the old stop is still unwinding would otherwise find the output "active" and
  // start nothing.
  bool force_replay_stopped();
  void watchdog();
  // One tick of the pending record request: waits, times out, or asks for a start.
  void poll_recording();

  // --- Salvage on a fault (recovery_policy.h) ---
  // Nothing is written while the capture works. When a fault is detected, the ring is saved once through the
  // ordinary clip path, before the restart that would empty it: the footage is a normal clip in the library, so it
  // needs no recovery surface, no session of its own and no second encoder. A fault nobody saw coming leaves
  // nothing, which is the price of writing nothing in normal use.
  // `wait_for_file`: the caller is about to restart the ring, so the file is waited for (5 s at most).
  // `at_once`: the ring starts the save at once instead of on its next packet, and nothing here waits - for a
  // capture that froze under a game hook, where the buffer is stopped and the ring writes itself out behind it.
  // Nothing is announced here: the save is only asked for, and its card is drawn when it has really ended - a
  // clip verified and filed (finish_save), or the bytes kept in the clips folder when filing was refused.
  void salvage_buffer(const char* reason, bool wait_for_file = false, bool at_once = false);
  // Every encoder that fits the config's codec and vendor, best first (NVENC, AMF, QSV, then software).
  std::vector<std::string> choose_encoder_ids(const Config& config) const;
  bool build_video_encoder(const std::string& encoder_id, const Config& config, std::string& error);
  // The config with the codec the engine is really encoding: H.264 when this machine could not start the one asked for.
  Config effective_config() const;
  // The same layer over any config, without the pipeline's own state: what `user` would run as.
  Config effective_for(const Config& user) const;
  // What the pipeline was last built with. A setting only rebuilds when it changes this, so a switch that leaves
  // the effective settings alone (Auto to On while the listed game already runs) does not reset the video.
  Config built_config_;
  bool built_config_valid_ = false;
  std::string codec_override_;
  std::string pending_notice_title_, pending_notice_detail_;
  // Some source is delivering frames: the current one, or the one it is replacing.
  bool capture_source_ready() const;
  // The current capture source alone has a frame (what the hook fallback and the retiring swap ask).
  bool capture_item_ready() const;
  // The source whose frames the ring is getting right now: the current one once it has a frame, else the one
  // it is replacing. Sizes and screenshots read this one.
  obs_source* delivering_source() const;
  // Stretch or fit the capture item to the canvas (capture_fills_canvas), whenever the decision
  // changes: a game's size is only known once it hooks, and it can change mid-session.
  void update_capture_fit();
  bool capture_stretched_ = false;
  bool capture_fit_applied_ = false;
  // The capture source lives in the scene; this is the only accessor for it.
  obs_source* capture_source() const;

  // Static trampolines so libobs can call back into this instance.
  static void on_replay_saved(void* data, calldata* call);
  static void on_replay_stopped(void* data, calldata* call);
  static void on_recording_stopped(void* data, calldata* call);
  static void on_manual_recording_stopped(void* data, calldata* call);
  static void on_recording_file_changed(void* data, calldata* call);

  bool initialized_ = false;
  bool memory_full_ = false;
  std::atomic<int> last_clip_seconds_{0};
  std::string capacity_path_;
  uint32_t capacity_ok_since_ms_ = 0;
  int capacity_recorded_step_ = -1;
  MemoryWatch memory_watch_;
  bool spill_wanted_ = false;    // the ring was given a disk part to make; false when the footage fits in RAM or the kill switch is set
  bool spill_noticed_ = false;   // the person was told once that it could not be made
  // Timeline dedupe (obs_host_dedupe.cpp): chunk files stand in for the ring's spill. Off unless BUFFRX_DEDUPE=1 while it is being proved.
  struct DedupeState;
  std::shared_ptr<DedupeState> dedupe_;
  // States dropped at a buffer start, kept alive so their workers can be joined at shutdown: one may still be in
  // media_chunk_probe, which waits on the media lock for up to 8 s.
  std::vector<std::shared_ptr<DedupeState>> retiring_dedupe_;
  bool dedupe_wanted() const;
  void dedupe_ring_started();
  void dedupe_note_chunk(const std::string& session_id, const SessionChunk& chunk);
  void dedupe_tick(uint32_t now);
  // Joins the dedupe workers and lets the state go. Shutdown only: a buffer start retires its state without waiting.
  void dedupe_shutdown();
  std::string spill_problem_;    // why no place was found for the file (create_outputs); empty when one was
  bool audio_ready_ = false;  // obs_reset_audio ran: it runs once, never under live audio sources
  std::string runtime_root_;
  std::string data_root_;
  Config config_;

  obs_source* capture_ = nullptr;
  struct obs_scene_item* capture_item_ = nullptr;
  obs_source* desktop_ = nullptr;
  obs_source* mic_ = nullptr;
  obs_source* mic_noise_suppress_ = nullptr;
  obs_source* mic_noise_gate_ = nullptr;
  obs_scene* scene_ = nullptr;
  // Canvas size chosen before the first obs_reset_video; the scene fits the source to it.
  int canvas_width_ = 0;
  int canvas_height_ = 0;

  obs_encoder* video_encoder_ = nullptr;
  // The encoder libobs actually created, not the configured preference: "auto" is not an encoder.
  std::string encoder_id_;
  std::vector<obs_encoder*> audio_encoders_;  // one per track
  obs_output* replay_ = nullptr;
  obs_output* recording_ = nullptr;
  // A manual recording uses a separate output fed by the same encoders as the ring and Timeline.
  obs_output* manual_recording_ = nullptr;
  std::atomic<bool> manual_active_{false};
  std::atomic<bool> manual_closed_{false};
  std::atomic<uint32_t> manual_stop_requested_ms_{0};
  std::atomic<long long> manual_stop_code_{0};
  std::string manual_file_;
  uint32_t manual_opened_ms_ = 0;
  uint32_t manual_started_ms_ = 0;
  bool manual_start_unconfirmed_ = false;
  buffrx::ResumeBudget manual_resume_budget_;
  // The recording's own encoders: created when a recording starts, released when it stops.
  obs_encoder* recording_video_encoder_ = nullptr;
  std::vector<obs_encoder*> recording_audio_encoders_;
  // True while the open recording runs on the ring's own encoders (no copies above), and whether sharing is
  // still allowed: one refusal from libobs turns it off for the rest of the process.
  bool recording_shared_ = false;
  bool shared_encoders_ok_ = true;
  // A Timeline session is open (start_timeline .. close_session). Read by the output's own settings, by the
  // length of a segment and by start_recording, so it is an atomic: the tick, the action worker and the
  // pipe threads all look at it.
  std::atomic<bool> timeline_writer_{false};
  // What the last tick decided (status.get reads it) and what a pause has to remember. The rest is the tick's,
  // under the engine lock: whether the open session is paused rather than stopped, when it was paused, the last
  // tick a segment was written (the gap marker) and the game the session belongs to (a change closes it).
  std::atomic<TimelineState> timeline_state_{TimelineState::Off};
  bool timeline_paused_ = false;
  // Why the writer paused, kept only to label the hole it leaves when it resumes.
  TimelineState timeline_pause_state_ = TimelineState::Off;
  uint32_t timeline_paused_at_ms_ = 0;
  uint32_t timeline_last_write_ms_ = 0;
  bool timeline_wake_gap_pending_ = false;
  std::string timeline_game_exe_;
  std::string timeline_last_error_;  // the last refusal, so a start retried every tick is logged once
  // A manual stop asked for the session to end: the tick closes it as soon as its own segment has let go, and
  // never keeps it for a pause or opens a new one meanwhile. Cleared by close_session.
  bool timeline_closing_ = false;
  // The stop this close asked of Timeline's own segment, whose output is still flushing: timeline_writer_ is
  // already false by then, and any other output in the slot belongs to a manual recording that the close does
  // not wait for. Cleared by close_session.
  bool timeline_segment_stopping_ = false;
  // The spans of the segment being written, one per file the muxer split itself (timeline_chunks.h). The
  // signal below only queues a path; the tick takes it, so every session-time decision stays on the tick.
  TimelineFileIndex timeline_chunks_;
  struct SplitChange {
    std::string file;   // the file the splitter just opened, in Windows form
    uint32_t at_ms = 0;  // the tick the signal fired on: where one span ends and the next begins
  };
  std::mutex split_mutex_;
  std::vector<SplitChange> split_changes_;
  bool resume_timeline(std::string& error);
  static constexpr uint32_t kTimelineGapMs = 2000;  // a pause longer than this leaves a gap marker
  // The length the splitter is asked for. It cuts on a keyframe, so with 4 s keyframes the files land near 8 s.
  static constexpr int kTimelineSplitSeconds = 5;
  // The open chunk started on an encoder that was already running, so its first frame is a keyframe away
  // (up to keyframe_seconds): its start time in the index is taken when that frame arrives, not at the
  // call, or a range cut from it would be that much early.
  bool chunk_start_unconfirmed_ = false;
  // Frames the recording output wrote before it was released, and the chunk that output was writing. Zero
  // means it wrote none, and the file it left is a container header with no footage in it: that chunk is
  // dropped instead of indexed. The path is what pairs the count with its file - the tick may index a
  // different chunk than the one that was just released. UINT64_MAX is "not known", which keeps the file.
  uint64_t closed_chunk_frames_ = UINT64_MAX;
  std::string closed_chunk_path_;

  // The session the recording output writes into, and the chunk it is writing right now. The index is
  // rewritten by the tick each time a chunk closes. recording_mutex_ guards the output, its encoders and
  // the session: commands arrive on pipe threads while the tick rotates. libobs's signal threads never
  // take it; they only touch the atomics below.
  mutable std::mutex recording_mutex_;
  SessionEntry session_;
  std::string current_chunk_;
  uint32_t current_chunk_start_ms_ = 0;
  // Chunks that are closed but whose files are not final yet, with the session each belongs to and when
  // it was queued (for the deadline). The tick publishes a chunk after its session may already have
  // closed, so the record names the session rather than trusting session_. recording_mutex_ guards them.
  struct PendingChunk {
    std::string session_id;
    SessionChunk chunk;
    uint32_t since_ms = 0;
  };
  std::vector<PendingChunk> pending_chunks_;
  static constexpr uint32_t kPendingPublishTimeoutMs = 5000;
  // How long shutdown waits for the last chunk's file before indexing without it (see destroy_outputs).
  static constexpr uint32_t kShutdownFinalWaitMs = 1000;

  // When the ring was last dumped on a fault, so one fault episode cannot fill the library (kSalvageCooldownMs).
  uint32_t last_salvage_ms_ = 0;

  bool buffer_active_ = false;
  // A stop was issued and the output has not reported itself down yet, and when it was issued: the
  // watchdog forces a stop libobs has not finished after kBufferStopGraceMs.
  bool buffer_stopping_ = false;
  // A stall restart whose stop has not landed (the frozen ring lets go on its next packet): said once, and the
  // start that follows it is reported as the restart it is.
  bool stall_reported_ = false;
  uint32_t buffer_stop_issued_ms_ = 0;
  static constexpr uint32_t kBufferStopGraceMs = 5000;
  // A stall restart throws the ring away, so they are spaced at least this far apart.
  static constexpr uint32_t kStallRestartCooldownMs = 10000;
  // What the user (or the engine's own startup) asked for, as opposed to what is running: the watchdog
  // retries the start while this is true, and a user-requested stop clears it.
  std::atomic<bool> buffer_intent_{false};
  std::atomic<bool> recording_active_{false};
  // A manual recording paused the replay buffer so it could use the encoder; restore it when the
  // recording ends. Atomic because the tick and the action worker both touch it.
  std::atomic<bool> restore_buffer_after_recording_{false};
  // A record request that found no frame yet: kept alive so the idle release does not tear the source
  // down again, and retried by the tick until a frame arrives or the wait times out.
  std::atomic<bool> recording_pending_{false};
  std::atomic<uint32_t> recording_request_ms_{0};
  std::atomic<bool> recording_start_due_{false};
  uint32_t recording_started_ms_ = 0;  // for the elapsed time the interface shows
  // Raised by the recording output's "stop" signal, consumed by the tick.
  std::atomic<bool> chunk_closed_{false};
  // When a stop or a rotation was asked for; a stop libobs has not answered after kStopGraceMs is
  // finished by the tick anyway.
  std::atomic<uint32_t> stop_requested_ms_{0};
  // What libobs said when a recording output stopped (0 = success), and how many unexpected stops the
  // recording has already been resumed after (recording_watch.h).
  std::atomic<long long> recording_stop_code_{0};
  buffrx::ResumeBudget recording_resume_budget_;
  // (recording_watch.h: recording_stop_grace_ms - a stop the person asked for waits far less than a boundary.)
  // Recording outputs whose stop libobs never finished, left alive rather than waited on.
  uint64_t abandoned_outputs_ = 0;

  // The encoder as it was created: the bitrate and the peak (0 for a rate control with no peak, i.e.
  // CQP/CRF). The ring's disk tier sizes itself from the peak.
  int created_kbps_ = 0;
  int created_max_kbps_ = 0;
  // One pipeline rebuild at a time (see command_policy.h). Taken for the whole of apply().
  RebuildGate rebuild_gate_;
  bool replay_faulted_ = false;
  // Whether the encoder keeps up, sampled by the tick, and when the person was last told it does not.
  buffrx::EncoderWatch encoder_watch_;
  std::atomic<int> display_hz_{0};
  uint32_t display_hz_at_ = 0;
  bool governor_disabled_ = false;
  int governor_step_ = 0;
  uint32_t governor_last_step_ms_ = 0;
  std::atomic<bool> governor_due_{false};
  bool force_encoder_rebuild_ = false;  // a governor step rebuilds the encoder with nothing else changed
  uint32_t encoder_behind_told_ms_ = 0;
  void watch_encoder(uint32_t now_ms);
  // What the ring holds, asked of the ring itself (the `ring_info` proc of the buffrx_ring output); `ok` is false when no ring runs.
  struct RingInfo {
    bool ok = false;
    int64_t bytes = 0, packets = 0, span_ms = 0, total_bytes = 0;
    // The disk part (ring_spill.h): what RAM holds, what the file holds, what was written, 0 off / 1 on / 2 unavailable, a disk that could not
    // take what was due, times the disk part was given up at the ceiling, and packets the last save left out (an unreadable disk part).
    int64_t ram_bytes = 0, disk_bytes = 0, spilled_bytes = 0, state = 0, stalled = 0, backstop = 0, skipped = 0;
    int64_t chunk_bytes = 0, deduped_bytes = 0, chunks_linked = 0, chunks_waiting = 0;
  };
  RingInfo ring_info() const;
  uint32_t last_restart_tick_ = 0;
  uint64_t restarts_ = 0;

  // Save queue: bounded, reserved filenames, drained on the tick.
  struct SaveRequest {
    int seconds = 0;
    bool whole_buffer = false;  // the request is the whole ring: the file is never cut (save_trim_slack)
    uint32_t queued_at = 0;
    bool now = false;  // start at once, without waiting for the ring's next packet (a capture that froze sends none)
    // Decided when the save is asked for, not when OBS finishes: the game being played at the press.
    std::string folder;  // "Counter-Strike 2", "Display Capture", ...
    std::string stem;    // "Replay 2026-10-19 00-19-04"
    // Asked for by salvage_buffer after a fault, not by the person. The fault travels with the request so the
    // card is drawn where the save really ended (saved_replay.h, salvage_notice), never when it was only queued.
    bool salvage = false;
    std::string salvage_reason;       // "the capture froze", for the log and the record
    int salvage_ring_seconds = 0;     // what the ring held when the fault was seen
  };
  // The library folder the current capture files into (see library.h for the layout).
  std::string capture_folder() const;
  bool follow_display_ = false;  // the follow layer (set_capture_follow)
  std::vector<SaveRequest> save_queue_;
  uint32_t last_save_tick_ = 0;
  // The save OBS is writing now. One at a time, so the file OBS reports belongs to this request.
  bool save_in_flight_ = false;
  SaveRequest in_flight_;
  uint32_t in_flight_since_ = 0;
  // The timeout is a report, not a give-up: the request stays in flight (so a slow save still reports
  // its own file to its own length) and this keeps the tick from saying so every pass.
  bool save_timeout_reported_ = false;
  static constexpr uint32_t kSaveTimeoutMs = 60000;
  // Raised by the output's "saved" signal (its thread), consumed by the tick.
  std::atomic<bool> replay_saved_{false};
  // One clip waiting for the trim worker: the path libobs reported, the request that asked for it,
  // and the held path the library must not list until the trim and the move are done.
  struct PendingSave {
    std::string path;
    SaveRequest request;
    std::string held;
    // The peak the encoder was allowed, read on the tick: the black-clip guard compares the file's rate with it.
    int peak_kbps = 0;
  };
  // Trims, verifies and files saved clips off the tick, one at a time. The tick enqueues and walks
  // away; it never joins. Two clips deep is the ceiling; past it the clip stays in the clips root,
  // untrimmed and listed, and the person is told — a remux never runs on the tick.
  WorkQueue<PendingSave> save_worker_{2};

  // Stats sampling.
  uint64_t last_tick_frames_ = 0;
  uint32_t last_tick_time_ = 0;
  int stall_ticks_ = 0;
  double capture_fps_ = 0.0;
  std::atomic<uint64_t> saved_clips_{0};
  std::string last_error_;

  // Live audio levels: desktop peaks and microphone RMS from the audio thread, consumed by the visible interface.
  obs_volmeter* desktop_volmeter_ = nullptr;
  obs_volmeter* mic_volmeter_ = nullptr;
  bool audio_metering_ = false;  // desired by the window thread, changed under the engine lock
  std::atomic<bool> audio_meters_active_{false};
  std::atomic<float> desktop_level_db_{-100.0f};
  std::atomic<float> mic_level_db_{-100.0f};
  // Written by the window thread, read by pipe threads: atomics like their raw inputs above.
  std::atomic<float> level_desktop_{0.0f};
  std::atomic<float> level_mic_{0.0f};
  // The device each live source is bound to: a fader move must not recreate it.
  std::string desktop_device_;
  std::string mic_device_;

  // Cached enumeration results: the Win32 walk over every top-level window is not free, and the
  // pickers poll while open, so results are cached and refreshed on demand instead.
  struct EnumCache {
    std::vector<WindowInfo> windows;
    std::vector<WindowInfo> games;
    std::vector<AudioDeviceInfo> audio_inputs;
    std::vector<AudioDeviceInfo> audio_outputs;
    uint32_t windows_at = 0;
    uint32_t games_at = 0;
    uint32_t audio_at = 0;
  } enum_cache_;
  static constexpr uint32_t kEnumCacheTtlMs = 1000;
};

// Utility used by the engine and the editor: converts the capture source's current NV12 frame into a
// PNG or JPEG on disk. Returns false with a reason when it cannot.
bool write_screenshot(obs_source* source, const std::string& path, bool jpeg, std::string& error);

}  // namespace bx
