// The engine's protocol surface: one request in, one response out, plus event builders.
//
// Handlers never block the capture path, never touch a client directly (main owns the transport) and
// never invent state: everything comes from ObsHost, the config, or the library.
#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../shared/json.h"
#include "../version.h"
#include "capture_enum.h"
#include "config.h"
#include "game_watch.h"
#include "gamepad_poll.h"
#include "platform.h"
#include "recovery_policy.h"
#include "recording_watch.h"
#include "timeline_state.h"
#include "work_queue.h"

namespace bx {

// The build's version, written once: the panel reports it and `--package-smoke-test` prints it, so a
// release bump cannot leave one of them on the old number.
inline constexpr char kEngineVersion[] = BUFFRX_VERSION;

class ObsHost;
class ShareService;
class LinkService;

// One item on the action worker's queue (main.cpp), the queue the hotkeys, the tray and `engine.restart` share:
// the action's name, and the one thing a restart has to carry across the rebuild - whether the buffer was running.
struct QueuedAction {
  std::string name;
  bool was_buffering = false;
  bool hotkey = false;
};

// `engine.restart` used to rebuild the engine inside its own handler: ObsHost::shutdown, initialize and the buffer
// start ran under the engine lock, so the client's reply - and with it the tick, the hotkeys and every other client
// - waited for the whole rebuild. The command hands the rebuild to the action worker and answers `{accepted:true}`;
// the worker's tail broadcasts `status` once the rebuild ends. This is the hand-off, kept out of libobs so the order
// is unit-tested: `enqueue` puts the job on the worker and nothing has been torn down when it returns. Called with
// the engine lock held. False with `error` while a manual recording or a Timeline session is being written (the same
// refusal Quit asks about), or when the queue refused the job.
inline bool request_restart(bool recording_busy, bool timeline_writing, bool was_buffering,
                            const std::function<bool(QueuedAction)>& enqueue, std::string& error) {
  if (buffrx::quit_needs_confirm(recording_busy, timeline_writing)) {
    error = "stop the recording first: restarting the engine would end it";
    return false;
  }
  if (!enqueue(QueuedAction{"restart_engine", was_buffering})) {
    error = "the engine is busy: try the restart again in a moment";
    return false;
  }
  return true;
}

class Protocol {
public:
  Protocol(ObsHost& host, PlatformWindow& window, std::string data_root, bool portable, std::string config_path);
  ~Protocol();
  Protocol(const Protocol&) = delete;
  Protocol& operator=(const Protocol&) = delete;

  bool load_settings(std::string& error);
  bool persist(std::string& error);
  void set_runtime_root(const std::string& runtime_root) { runtime_root_ = runtime_root; }
  // The share jobs and the webhook (share.h); set by main, owned there. Null: share.* is refused.
  void set_share(ShareService* share) { share_ = share; }
  // BuffrX links (link.h); set by main, owned there. Null: link.* is refused. Setting it passes the
  // link settings on.
  void set_links(LinkService* links);
  // Whether a game has focus now, for the links' upload cap. Called from the tick, engine lock held.
  bool game_in_focus() const;
  // Take a screenshot using the game in front when one is detected. Engine lock held.
  bool take_screenshot(const std::string& format, std::string& path, std::string& error);
  // A buffer that starts while a game runs starts on the game, not on the display with a switch a moment later
  // (which reset the buffer and made the first start slow): the fullscreen foreground game, else the one seen a
  // minute ago that still runs, else any fullscreen game already on screen (BuffrX started after it). The
  // engine lock held. No-op unless auto_game_capture.
  void start_on_running_game();
  // The tray menu's two switches, with the engine lock held: the buffer on or off (as buffer.start / buffer.stop), and
  // Performance mode (as config.set). Performance waits while a recording runs, like the interface's switch.
  bool toggle_buffer(std::string& error);
  // Alt+4 (the action "view_clip"): opens the Timeline preview, on the newest session with a published
  // chunk or on its empty state, and closes it when it is open. Engine lock held.
  void timeline_pressed();
  bool timeline_preview_open() const { return timeline_process_ != nullptr; }
  // Starts the preview process (`buffrx-ui.exe --timeline <session>`) and remembers its id, which is what
  // keeps the writer running while its window is in front (timeline_preview_is_foreground). An empty
  // `session` takes the newest Timeline session with at least one published chunk.
  bool open_timeline_preview(const std::string& session, std::string& error);
  // From the tick: notices that the Timeline preview's process ended and clears its id, so the writer's
  // foreground exemption lapses with the window; ends one that was asked to close and did not.
  void poll_timeline_preview(uint32_t now_ms);
  // Asks the preview to close itself (the page clears its player and exits); poll_timeline_preview ends it
  // if it has not within two seconds.
  void ask_timeline_preview_to_close();

  Config& config() { return config_; }
  const std::string& data_root() const { return data_root_; }
  bool portable() const { return portable_; }

  // Handles one request frame and returns the response frame.
  std::string handle(const std::string& payload);

  // Event frame used by main for the status tick and for host callbacks.
  static std::string make_event(const std::string& name, const Json& data);

  Json status_json();
  // The automatic update's state, for `status.get` and the `update.check` reply: `{state, percent, version,
  // error}`. Cheap: a lock and four strings, no disk.
  Json update_json();
  // The action worker has prepared an update and stopped the outputs; the main loop takes this once and ends.
  bool take_update_quit() { return update_quit_.exchange(false); }
  void request_update_quit() { update_shutting_down_.store(true); update_quit_.store(true); }
  bool update_shutting_down() const { return update_shutting_down_.load(); }
  void report_update_error(const std::string& error) { set_update_state("failed", error); }
  // Called on the action worker: re-checks the downloaded file and starts the supervisor.
  bool spawn_pending_installer(std::string& error);
  // Stops a download in flight (quit): the worker leaves within one read.
  void stop_update_download();

  // The `timeline` object both `status.get` and the `timeline.changed` event carry: the setting, the
  // writer's state and the legacy clip fields. Cheap enough for every status tick; the tray reads it
  // when nobody listens for the full status.
  Json timeline_status_json() const;
  Json config_json();
  std::vector<std::string> capabilities();

  // Applies a config patch and reconciles the engine; returns the applied config.
  Json apply_patch(const Json& patch, std::string& error);
  // The governor lowered the encoder's speed settings to `step`: remembered in the config, so the next launch
  // starts there, and the interface is told. Called with the engine lock held, from the action worker.
  void note_governor_step(int step);

  // Registers every configured hotkey with the platform window. Called at startup and whenever a
  // binding changes.
  void apply_hotkeys();
  // Controller hotkeys: the engine's action queue and the recorder's live chord. Set by main, which owns
  // the queue every hotkey action goes through. Safe before the first apply_hotkeys.
  void set_pad_sinks(PadPoller::ActionSink action, PadPoller::CaptureSink capture);
  // Controller navigation: the buttons that went down and came up, for the interface to move on.
  void set_nav_sink(PadPoller::NavSink sink) { pad_poller_.set_nav_sink(std::move(sink)); }
  // The session is locked: reading the pads is pointless and a press behind the lock screen is not a save.
  void pause_controller_hotkeys(bool paused) { pad_poller_.set_paused(paused); }
  int controllers_connected() const { return pad_poller_.connected(); }

  // After a successful write, leaves its marker (marker_kind_for_command) in the open session. With no
  // session open the marker is dropped silently: there is nothing to index it against. The tray and
  // hotkey paths call this with the command their action stands for.
  void mark(const std::string& command, const std::string& label);
  // A marker at `session_ms` (the session's own clock) in a Timeline session, open or closed: through the
  // host while it writes that session, else straight into its marker file. `markers.changed` either way.
  // The new marker's id (`marker_id` on the wire), or "" on failure with `error` set.
  std::string mark_timeline(const std::string& session_id, const std::string& kind, const std::string& label,
                            int64_t session_ms, std::string& error);

  // The Record key, wherever it comes from: the global hotkey, the tray's record row and both recording.*
  // commands. One press stops or starts a manual recording, independent of Timeline. Engine lock held.
  buffrx::RecordAction record_pressed(std::string& error);

  // The engine lock (see command_policy.h). main holds it around the tick, the status broadcast and the
  // tray/hotkey actions, so libobs objects are never used from two threads at once.
  std::mutex& engine_mutex() { return engine_mutex_; }

  // One poll of the running game, from the tick with the engine lock held: the capture target check and,
  // unless auto_game_capture is off, the focus-based capture switch.
  void poll_game(uint32_t now_ms);

  // The Timeline writer's tick (timeline_state.h): builds the facts the engine cannot see - the setting,
  // the buffer, the captured game, the foreground, the preview - and asks the host to open, pause, resume
  // or close its session (obs_host.cpp). Engine lock held, from the tick.
  void poll_timeline(uint32_t now_ms);
  // The windowed game in front, when the opt-in is on and one is found: its program name, empty otherwise.
  std::string poll_windowed_game(uint32_t now_ms, bool other_game_running);
  // What the last tick decided.
  TimelineState timeline_state() const;
  // Whether the Timeline preview's window is in front: the one foreground that does not pause the writer.
  // False while there is no preview process (Task 10 launches it and sets the id).
  bool timeline_preview_is_foreground() const;
  // A range of a Timeline session saved as a normal library clip: the body of `timeline.save`, which
  // Task 9 dispatches. `args`: `session` (empty takes the newest timeline session), `start_ms`, `end_ms` on
  // the session's own clock - the numbers `timeline.get` reports - and an optional `name`. Runs outside the
  // engine lock, because it can wait for the open segment to close and then copy for seconds. `out` is
  // `{file, bytes, chunks}`, like `clips.export_range`. False with `error` when the newest moment is not
  // saved yet, the range crosses a settings change, or the copy failed.
  bool timeline_save(const Json& args, Json& out, std::string& error);

  // The pinned capture target: is it still on the screen? A game that closed or an app that quit
  // leaves the engine recording nothing, so it falls back to the display and says so.
  void check_capture_target();

  // Set by main to publish events to every connected client.
  std::function<void(const std::string&)> broadcaster;
  // The action worker's queue (main.cpp): hotkeys, the tray and `engine.restart` reach it here, one job at a
  // time. A queued restart's rebuild and its `status` broadcast happen on that worker, never in the command.
  std::function<bool(QueuedAction)> action_queue;
  // Shows an engine notification card (title, one-line detail); set by main, may be empty.
  std::function<void(const std::string&, const std::string&)> notifier;
  // Hands files to an editor that is already open (true), or reports there is none (false); set by main.
  // Hands files to an editor that is already open; `preview` asks it to play them first.
  std::function<bool(const std::vector<std::string>&, bool preview)> editor_router;
  // ui.capture: draws a view of the native interface and writes it as a PNG (main.cpp wires it to the
  // interface host). Runs outside the engine lock: it waits for the main loop, which takes that lock.
  std::function<bool(const std::string& view, const std::string& path, std::string& error)> ui_capturer;
  // ui.input: clicks, keys and text handed straight to the native interface (never the system cursor).
  std::function<bool(const Json& args, std::string& error)> ui_inputter;
  // ui.frame_stats: how steadily the interface has drawn since the last reset (gaps between frames, input to frame delay).
  std::function<Json(bool reset)> ui_frame_stats;

private:
  Json handle_command(const std::string& command, const Json& args, std::string& error);
  // Re-points the library and the session store when a config change moved the clips or screenshots
  // folder.
  void follow_library_paths(const Config& before);
  // Focus-based capture switch, run every tick (250 ms): samples the foreground itself, feeds the
  // time-based policy (game_watch.h) and applies or restores the capture target. No-op unless
  // config.auto_game_capture.
  void poll_game_capture(uint32_t now_ms);
  // Whether the captured game still runs: its window by handle (one call), else a visible window of its
  // executable, looked up at most once a second.
  bool active_game_alive(uint32_t now_ms);
  // Performance mode for the games on the user's list: while the captured game is on it the mode is on, and off when it
  // leaves. A change that would rebuild the pipeline waits while a manual recording runs (performance_decide).
  void update_performance_for_game();
  // The automatic update: starts one download when the feed names a newer release with a digest, on a thread
  // of its own so `update.check` stays a short command. The thread writes the pending file and the launcher's
  // apply marker; nothing on the window thread touches the network or the disk.
  void maybe_start_update_download(const Json& feed);
  void run_update_download(std::string version, std::string url, std::string digest, std::string notes);
  void set_update_state(const std::string& state, const std::string& error = std::string());

  // The newest Timeline session that has a published chunk (footage a player may read), or "".
  void end_timeline_preview();
  bool game_is_performance_listed(const std::string& exe) const;
  Json game_state_json();

  GameCaptureState game_capture_;             // focus-based game capture state
  GameCaptureState timeline_game_;            // detection without changing the selected capture source
  uintptr_t timeline_game_hwnd_ = 0;
  uint32_t timeline_alive_checked_ms_ = 0;
  uint32_t timeline_scan_ms_ = 0;
  // A windowed game adopted under the opt-in (windowed_game.h): the window followed, and the last verdict per program so
  // the module list is read once a minute at most, not at every look.
  uintptr_t timeline_windowed_hwnd_ = 0;
  std::string timeline_windowed_exe_;
  uint32_t timeline_windowed_scan_ms_ = 0;
  std::map<std::string, std::pair<bool, uint32_t>> timeline_windowed_verdicts_;
  bool timeline_alive_cached_ = false;
  std::string timeline_pinned_exe_;
  uint32_t timeline_pinned_checked_ms_ = 0;
  bool timeline_pinned_alive_ = false;
  std::string before_capture_mode_;           // the capture target to restore when the game leaves
  std::string before_capture_title_;
  // True while the display is captured because the Timeline is on and no captured game is in front: the
  // return to the game is a follow swap, not a detection, and it restores nothing of the user's.
  bool follow_display_ = false;
  // The swap is a layer in the host (ObsHost::set_capture_follow), never a config write: the user's capture target is not
  // changed, saved or snapshotted for it, so it needs nothing put back.
  void restore_follow_capture(const char* why, bool reconfigure);
  // The last fullscreen game seen in the foreground and when: a buffer started from the panel (which is in
  // front of the game by then) starts on that game instead of on the display (buffer.start).
  std::string last_game_id_, last_game_exe_, last_game_title_;
  uint32_t last_game_seen_ms_ = 0;
  uintptr_t active_game_hwnd_ = 0;  // the captured game's window, for the liveness check
  uint32_t idle_sample_ms_ = 0;     // the last foreground read while nothing wanted frames
  uint32_t alive_checked_ms_ = 0;
  bool alive_cached_ = false;
  uint32_t last_target_check_ = 0;
  std::string observed_exe_, observed_title_;
  ForegroundWindow foreground_;  // the foreground window as game capture needs it (id, title, exe)
  // The Timeline preview process, once it runs (Task 10): its window in front does not pause the writer,
  // because looking at the last minute is not leaving the game.
  uint32_t timeline_preview_pid_ = 0;
  void* timeline_process_ = nullptr;       // the preview's process handle while it runs
  uint32_t timeline_close_asked_ms_ = 0;   // when it was asked to close; it is ended if it has not by itself
  // Whether the game the preview was opened over could be covered cannot be asked of Windows, so its
  // window is looked at once after the preview opens (timeline_overlay.h). One that left the screen is put
  // back, said once, and left unprobed for the rest of the run.
  uintptr_t timeline_overlay_hwnd_ = 0;      // the game's window to look at, 0 when there is nothing to look at
  uint32_t timeline_overlay_due_ms_ = 0;
  std::string timeline_overlay_exe_, timeline_overlay_title_;  // the game being probed, for its notice
  uintptr_t timeline_restore_hwnd_ = 0;      // the game's window to put back when the preview ends
  std::string timeline_exclusive_exe_;        // the game that could not be covered, normalized
  // The Timeline's disk guard: sampled once a minute, never while Timeline is off. Low space stops the
  // writer; it resumes only a gigabyte above the floor.
  uint32_t low_disk_checked_ms_ = 0;
  bool low_disk_ = false;
  // The Timeline retention sweep's last run: 0 makes the first tick run it.
  int64_t timeline_prune_last_ms_ = 0;
  // What the last poll decided, so `timeline.changed` is a state change and never a per-chunk stream.
  TimelineState last_timeline_state_ = TimelineState::Off;
  bool last_timeline_enabled_ = false;
  // Names a new lease; only touched with the engine lock held, next to the lease it names.
  uint64_t timeline_lease_seq_ = 0;
  // Serializes an editor's lease handoff with an explicit erase of the same store. An erase releases it
  // while it waits, and claims the session here so an edit arriving then is told rather than left waiting.
  std::mutex timeline_editor_gate_;
  std::string timeline_erasing_;
  Json profiles_json();
  bool write_profiles(const Json& profiles, std::string& error);
  bool read_profiles(Json& profiles);

  ObsHost& host_;
  PlatformWindow& window_;
  std::string data_root_;
  std::string runtime_root_;
  std::string config_path_;
  std::string profiles_path_;
  bool portable_ = false;
  ShareService* share_ = nullptr;
  LinkService* links_ = nullptr;
  Config config_;
  std::mutex engine_mutex_;
  std::mutex update_mutex_;
  Json update_cache_;
  // The automatic update's worker and its state. `update_running_` is true from the start of a download to
  // the moment its thread has written the pending file (or given up); the thread joins in the destructor.
  std::thread update_thread_;
  std::mutex update_thread_mutex_;
  std::atomic<bool> update_stop_{false};
  std::atomic<bool> update_quit_{false};
  std::atomic<bool> update_shutting_down_{false};
  std::atomic<int> update_percent_{0};
  bool update_running_ = false;
  std::string update_state_ = "idle";   // idle | downloading | ready | installing | failed | manual
  std::string update_version_;
  std::string update_state_error_;
  // The pads, read only while a controller binding exists (gamepad_poll.h). Its action sink is the queue
  // the window's hotkeys use, so a pad press and Alt+1 are the same action.
  PadPoller pad_poller_;
  std::map<std::string, std::string> pad_problems_;
};

}  // namespace bx
