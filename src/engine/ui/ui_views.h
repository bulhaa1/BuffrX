// The native interface's surfaces: the panel (with Settings) and the tray panel, which is the one
// small interface: the tray icon, the quick panel and the panel's own quick button all open it.
// Each draws into the ImGui window its host opened and reaches the engine only through the
// store, so every action is the same protocol command the pipe takes.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../../shared/json.h"
#include "imgui.h"
#include "ui_images.h"
#include "ui_link_math.h"
#include "ui_live_math.h"
#include "ui_store.h"

#include "web_clip.h"

namespace bx::ui {

// What a view may ask of the window it is drawn in.
class UiShell {
public:
  virtual ~UiShell() = default;
  virtual void minimise() = 0;
  virtual void close_window() = 0;                 // the panel hides to the tray; the tray panel closes
  virtual void show(const std::string& view) = 0;  // "main" | "tray" | "compact" (the quick panel)
  virtual void no_drag(ImVec2 min, ImVec2 max) = 0;  // a titlebar control the window must not drag on
  virtual void drag_area(ImVec2 min, ImVec2 max) = 0;  // where the window moves when grabbed
  // The hotkey recorder: while on, key presses go to take_key() instead of the controls.
  virtual void capture_keys(bool on) = 0;
  virtual bool take_key(bool& ctrl, bool& alt, bool& shift, bool& win, unsigned& virtual_key, bool& down) = 0;
  virtual void save_layout() = 0;                  // writes ui.json now
  virtual void check_update(bool force) = 0;
  // The updated installer is verified and waiting: close cleanly, let it run silently, come back on the new
  // version. Refused by the engine while a manual recording runs; a refusal arrives as an error toast.
  virtual void install_update() = 0;
  // The quick panel stays above other windows only while pinned (off by default): without it, clicking another window covers it.
  virtual bool pinned() const = 0;
  virtual void set_pinned(bool pinned) = 0;
  // The clip the panel plays (web_clip.h): `file` plays in `rect` (client pixels of the panel) while `covered` is false.
  virtual ClipPicture clip_picture(const std::string& file, const RECT& rect, bool covered) = 0;
  virtual void clip_picture_close() = 0;  // the page and its browser go away
};

struct Advanced {
  bool capture = false, audio = false, encoder = false, storage = false;
};

// The deck's live timeline (ui_live.cpp): one Timeline session read with timeline.get while the panel shows, its filmstrip
// from timeline.thumbs, and the range Save sends to timeline.save. Times are the session's clock.
struct LiveStrip {
  std::string session;      // the session shown, "" until the first read finds one
  int64_t started_at = 0;   // unix seconds, from the listed read
  std::string state;        // the writer's state at the last read
  std::string seen;         // the writer's state the status last said: a change reads the list again at once
  bool live = false;        // the writer is on this session
  int64_t lo = 0, hi = 0;   // the published footage
  std::vector<LiveRange> spans;  // the published chunks, joined where they touch: a gap shows between two
  bool reading = false, list_next = true;
  uint32_t read_due = 0;
  int reads = 0;
  std::map<int64_t, std::string> thumbs;  // a moment's picture, a local JPEG
  std::map<int64_t, int> asked;           // how often a moment was asked for: three times, then it is left empty
  std::map<int64_t, int> missed;          // replies that did not make a moment: three refunds, then it is left empty
  int64_t pruned_start = 0, pruned_end = 0;  // the view the strip last pruned to: entries outside it and its margin are gone
  bool thumbs_reading = false;
  uint32_t thumbs_retry = 0;   // after a reply with no pictures (the media lock busy): ask again then, not every frame
  LiveRange range;          // what Save keeps
  int64_t want = 0;         // the length the person chose (the quick clip's at first): a range at the live edge grows back to it
  bool range_set = false;
  bool follow = true;       // the range's end rides the live edge
  int64_t span = 60000;     // the view's width (the goal: the wheel and the buttons set it)
  double shown_span = 0;    // the width on screen: it eases to `span` on the fast token (B8)
  int64_t zoom_t = 0;       // a wheel zoom's anchor: the moment under the pointer ...
  double zoom_frac = -1.0;  // ... and its distance from the view's right edge as a fraction of the width (-1: no glide anchored)
  int64_t view_end = 0;
  bool view_follow = true;  // the view's end rides the live edge
  int drag = 0;             // 0 none, 1 start edge, 2 end edge, 3 the whole range, 4 a new range
  int64_t drag_at = 0;      // where the drag began, on the session clock
  LiveRange drag_from;
  bool dragged = false;
  bool saving = false;
  uint32_t tick_ms = 0;     // while the live edge moves on screen: how often it moves a pixel (animation_reason); 0 still
  uint32_t drawn_at = 0;    // when the strip was last drawn: the tray panel shares this state and must not draw for it
  bool wheel = false;       // the strip or the actions list took the wheel last frame: the deck does not scroll under it
  bool audio_hover = false;  // the pointer is on a fader: the deck does not scroll under it either
  int64_t recording_since = 0;  // the running recording's start, kept from its first status so its row does not jitter
  std::vector<RecentAction> recent;
  int recent_version = -1;
  int64_t recent_since = -1;
};

// The panel's own state: kept outside ImGui so it survives the windows being torn down while hidden.
struct PanelState {
  bool welcomed = false;
  int guide_step = 0;              // the first-run guide's card
  bool guide_replay = false;       // reopened from the help sheet: stays even with clips in the library
  int rail_tab = 0;                // 0 clips, 1 screenshots, 2 Timeline, 3 shared
  // The update check's last answer, kept for the Settings page.
  bool update_checking = false;
  std::string update_latest;
  std::string update_notes;
  std::string update_error;
  std::string update_offered;  // the version the user dismissed with Later
  // The automatic update: the version whose ready sheet is open (or was dismissed), and the version whose
  // "ready" toast was shown. The state itself comes from `status.update`, so another window cannot disagree.
  std::string update_prompted;  // the version whose restart sheet was opened
  bool update_restart_open = false;
  // The changelog sheet: the version this run last showed notes for, and the version an install left
  // waiting for its first run.
  std::string version_seen;
  std::string pending_version;
  std::string pending_notes;
  bool notes_open = false;
  bool notes_loading = false;
  bool notes_available = false;
  bool notes_checked = false;
  int64_t update_checked_ms = 0;
  // The keyboard's place in the library: the card the arrows are on (-1 none, so E/S/Enter mean the newest), and
  // the columns the grid had last frame, for Up and Down.
  int rail_cursor = -1;
  int rail_columns = 1;
  // The deck's leftover height, in pixels: spread through the panes so the column ends on the same line
  // as the library's pane instead of leaving a strip of ground under it. Measured once from the natural
  // layout (`deck_fill_ready`); measuring it from an already padded frame fed back into itself and moved
  // the panes a pixel or two every frame at some window sizes.
  float deck_pad = 0;
  // The measure taken on the last frame while the deck was still settling. A pane that sizes to its
  // content reports no height on the frame after a window opens, so the first slack of a fresh layout
  // is short; the pad is taken only when two frames agree.
  float deck_pad_seen = -1;
  bool deck_fill_ready = false;
  // The run list's row the keyboard is on (0 save, 1 record, 2 shot, 3 timeline; -1 none), and the card each
  // library tab was left on, so Tab comes back to where it was instead of the newest.
  int deck_cursor = -1;
  int rail_memory[4] = {0, 0, 0, 0};
  // The Timeline session whose Erase was pressed once (Settings' own rule: a second press within 4 s erases).
  std::string timeline_erase;
  uint32_t timeline_erase_until = 0;

  // The live-link pane: first-seen order, the selected link, and the Stop that waits for a second press.
  LinkBook link_book;
  std::string link_chosen;
  LinkStopArm link_stop;
  LinkView link_last;
  bool link_last_valid = false;
  bool keys_open = false;          // the help sheet (F1 or ?)
  int help_section = 0;            // the guide shown in the help sheet's right panel
  bool help_open[3] = {true, true, true};  // overview, steps and tips expanded in the help sheet
  bool keybinds_open = false;      // the read-only list of every hotkey
  bool buffer_stop_open = false;   // stopping the buffer also stops Timeline from recording
  float help_height = 0;                   // the help sheet's measured height, fitted again when a section folds
  // A popup was open when the last frame ended. ImGui closes one on Escape before the view runs, so the same key press
  // would otherwise also reach whatever listens for Escape underneath (leaving Settings, hiding the window).
  bool popup_was_open = false;
  std::string game = "";           // "" all games
  std::string screenshot_game = "";  // screenshots keep their own filter
  std::string settings;            // "" the panel, else the section shown
  std::string settings_loaded;     // the section whose lists were last read
  // Preview (screenshots draw here; clips open in the editor's player).
  bool preview_open = false;
  int preview_index = 0;
  std::vector<std::string> preview_files;  // the list the preview steps through, as it was opened
  bool preview_zoom = false;        // the screenshot is zoomed past its fit
  float preview_z = 1.0f;           // how far (1 = fitted), and where the picture's centre is from the area's
  float preview_pan_x = 0.0f, preview_pan_y = 0.0f;
  bool preview_panning = false;     // the middle button is dragging it
  bool preview_from_compact = false;  // opened from the quick panel: Back (or Esc) goes back to it
  int settings_open_epoch = 0;        // bumps when Settings is opened: a page's cached read is not the last visit's
  // Dialogs.
  std::string recording;           // the hotkey action being rebound, "" none
  std::string recorded_chord, recorded_problem;
  std::string check_hotkey;        // after a rebind: the action whose registration is read back
  int check_hotkey_version = 0;
  bool recorder_saving = false;
  bool recorder_open = false;      // the recorder holds the keys and has the hotkeys suspended
  bool recorder_controller = false;  // the recorder is listening to a pad, not the keyboard
  std::string recorder_preview;      // the modifiers held right now, drawn before a key completes the chord
  UiMedia renaming, removing;
  // The share sheet (share.h). The clip being shared and what the sheet asks for; the engine's job
  // itself is UiStore::share.
  bool share_open = false;
  UiMedia sharing;
  std::vector<std::string> share_frames;  // stills across the clip being shared: the sheet loops through them
  std::string share_frames_for;           // the clip they belong to
  bool share_playing = false;             // every still is a texture: the preview loops (animation_reason draws it)
  int share_limit = 0;             // MiB this sheet fits to; starts at the setting
  bool share_fit = false;          // "Make it ready for Discord" is on for this sheet
  int share_tab = 0;               // the share sheet's destination: 0 a BuffrX link, 1 copy or Discord
  int link_minutes = 60;           // how long a new BuffrX link lasts: 60, 240, 720 or 1440
  bool link_busy = false;          // a link is being made (the first one waits for the network)
  std::string link_sender_text, link_sender_problem;  // Settings, Sharing: the name shown on links
  // A BuffrX link found on the clipboard while the panel is open: offered once ("Open the clip?").
  std::string link_offer;
  int64_t link_offer_expires = 0;
  std::string share_message;
  std::string share_webhook_id;  // selected destination, kept while the sheet is open
  // Settings, Sharing: the webhook being pasted (never shown back once saved).
  std::string webhook_text, webhook_name, webhook_problem;
  bool webhook_busy = false;
  bool rename_open = false, remove_open = false;
  std::string rename_text, rename_problem_text;
  bool rename_busy = false;
  Advanced advanced;
  LiveStrip live;
  bool saving = false;
  uint32_t saving_until = 0;
  // Lists Settings reads when a section opens.
  Json displays, windows, devices, storage, encoder;
  std::string encoder_key;  // the settings the pending property request describes
  uint32_t displays_at = 0;  // when the display list was last asked for
  uint32_t devices_at = 0;   // when the audio device list was last read
  bool displays_loading = false, windows_loading = false, devices_loading = false, storage_loading = false,
       encoder_loading = false, encoder_failed = false;
  bool picking_window = false;
  std::string exclusions_text;
  std::string performance_text;
  std::string filename_format;     // Settings, Storage: the name pattern being typed
  uint32_t reset_armed_until = 0;  // Settings, System: Reset all is armed until this moment
  bool restarting = false;
};

struct ViewEnv {
  UiStore& store;
  ImageCache& images;
  UiShell& shell;
  PanelState& panel;
  uint32_t now_ms;
  // The monitor the window is on, in pixels: a screenshot at 100% is decoded no larger than this (0 when unknown).
  int display_w = 0, display_h = 0;
};

void draw_panel(ViewEnv& env);     // the full panel and Settings
// The changelog sheet, drawn over whatever the panel shows. Opens once, on the first run after an install
// put a new version in place; the frame it opens on is the one where the engine's hello has arrived.
void maybe_show_whats_new(ViewEnv& env);
void maybe_show_update_restart(ViewEnv& env);
void draw_tray(ViewEnv& env);      // the tray panel
// Keeps drawing while something moves: a spinner, the dial's ripple, a toast, the recording dot. `name` is what moves (null when
// nothing does: `ui.frame_stats` counts frames by it, so a window that draws with nothing changing names its cause); the frame
// it needs comes every `period_ms` (see animation_frame_due; `aligned` puts it on the wall clock's beat).
struct Animation {
  const char* name = nullptr;
  uint32_t period_ms = 0;
  bool aligned = false;
};
Animation animation_reason(const ViewEnv& env);

}  // namespace bx::ui
