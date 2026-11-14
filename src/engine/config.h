// BuffrX configuration: one JSON file under the data root, atomic writes, per-key defaults.
//
// Deliberately one flat object rather than nested sections: the alpha's surface is small enough to
// read at a glance, and a flat map means an older build ignores keys it does not know instead of
// failing to load.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../shared/json.h"
#include "audio_auto.h"
#include "hotkey_defaults.h"

namespace bx {

// The static link page that makes a BuffrX link clickable in a chat.
inline constexpr char kDefaultLinkPage[] = "https://buffrx.app/l/";

// When Performance mode is used: off = never, games = only while a listed game is captured,
// always = the user's manual switch. Replaces the old performance_mode boolean; older files are
// migrated in config_from_json. Not a profile key.
enum class PerformanceWhen { off, games, always };

// One selected application slot (audio_routing.h). `window` is the win-capture id
// ("title:class:exe") at the moment it was picked; `exe` is the stable part a restart or a retitled
// window is matched on. `kind` is empty for a named program and "game" for the Current game slot, whose
// window and executable follow what BuffrX is capturing. `track` is written for older builds; the plan
// assigns tracks by slot position (3..6) and never reads it.
struct AudioApp {
  std::string window;
  std::string exe;
  std::string title;
  std::string kind;
  int track = 2;
  // The program's own live gain, 0..1, and whether its capture is muted. audio.set writes them beside
  // the route; they change the running source in place and are not part of the routing identity
  // (audio_routing.h), so a fader never rebuilds the encoders or clears the ring.
  double volume = 1.0;
  bool muted = false;
};

struct Config {
  std::string capture_mode = "display";  // display | window | game
  int display_id = 0;
  std::string display_name;              // device name from the capture source, e.g. \\.\DISPLAY1
  std::string window_id;                 // window handle as a decimal string
  std::string window_name;
  std::string game_title;                // empty = any fullscreen game
  int capture_fps = 60;
  int output_width = 0;                  // 0 = native
  int output_height = 0;
  bool capture_cursor = true;
  // How a source whose shape differs from the canvas is fitted: "stretch" fills the frame (a 4:3
  // stretched-resolution game recorded the way the player sees it), "fit" and the older "auto" keep its
  // aspect with bars. Applied live, never a restart.
  std::string stretch_mode = "auto";

  std::string codec = "h264";            // h264 | hevc | av1; the first start picks AV1 when the GPU has it (codec_auto_picked)
  std::string encoder = "auto";          // auto | nvenc | amf | qsv | software
  // low | standard | high | highest. A level is a constant-quality target; its peak rate follows the
  // codec, size and frame rate (encoder_settings.h) while `peak_mode` is "auto". "fixed" is the rate
  // the user set on the slider, in custom_kbps, under whichever level is chosen. Version 4's "custom"
  // level is read as High with a fixed peak (config_from_json).
  std::string quality = "standard";
  std::string peak_mode = "auto";  // auto | fixed
  int custom_kbps = 0;
  // low | standard | high | highest | custom: the one Video choice, which sets quality, rate and effort together
  // (encoder_settings.h video_preset_patch). "custom" means the rate and effort are the user's own.
  std::string video_preset = "standard";
  int keyframe_seconds = 2;
  // Read from an older config as the level the encoder controller (encoder_controller.h) starts at, once; never written. The Video page's
  // "Try full quality again" sends 0 as a trigger. The memory is capacity.json, keyed by GPU and driver.
  int encoder_speed_step = 0;
  // auto | fast | balanced | quality | custom. Fast, Balanced and Quality are the speed tiers whatever the quality
  // level; Auto follows the level; Custom is `encoder_options` on top of Auto. Editing a speed row on the page
  // makes the mode Custom. `encoder_options` is only applied in Custom.
  std::string encoder_mode = "auto";
  // Display capture: auto | dxgi (desktop duplication) | wgc (Windows graphics capture). Some PCs capture one
  // way and not the other; Auto lets win-capture choose.
  std::string display_method = "auto";
  // The filter that scales the canvas to a smaller output size: bilinear | bicubic | lanczos.
  std::string scale_filter = "bicubic";

  // Explicit encoder settings, keyed by the libobs setting name ("preset", "multipass", "bf", ...).
  // They override the tier's defaults and a null value removes one. The advanced encoder page writes
  // here from the encoder's own property list, so a vendor knob needs no engine change.
  Json encoder_options = Json::object();
  // Extra muxer options handed to the output, the "custom muxer settings" field.
  std::string muxer_settings;

  // The length the ring keeps, always, at the quality chosen: a saved clip is this long and never lowered to fit memory.
  int buffer_seconds = 60;
  int quick_clip_seconds = 20;
  // The gameplay Timeline: how many minutes of footage the disk keeps while a buffer runs, 0 (off) or one
  // of 30, 60, 120. Off by default; the ring is untouched by it. timeline_minutes_allowed is the one rule
  // for what a window is, so a hand-edited file cannot invent a fifth.
  int timeline_minutes = 0;
  // Stopped Timeline footage deletes itself after this many hours (0: kept until erased). With Timeline on, its own window
  // governs the footage; this is for what is left once recording stops or Timeline is turned off.
  int timeline_keep_hours = 24;
  // Opt-in: a framed window that is not fullscreen counts as a game for Timeline when its process loaded a graphics
  // library together with a controller, sound or platform library (windowed_game.h). Off by default.
  bool timeline_windowed_games = false;
  // Record the desktop too when no game runs, not only games. Off by default: it is every minute of the PC on the disk.
  bool timeline_everywhere = false;
  // With everywhere on: stop writing desktop footage after this many minutes without keyboard or mouse (0: never).
  int timeline_idle_minutes = 0;
  // Whether the ring starts with the engine. Off means the tray says "not buffering" until the user
  // asks for it, which is what someone who only wants the hotkeys and the editor wants.
  bool start_buffering = true;

  bool desktop_enabled = true;
  double desktop_volume = 1.0;
  bool desktop_muted = false;
  std::string desktop_device = "default";
  bool mic_enabled = false;
  double mic_volume = 1.0;
  int mic_gain_db = 0;
  bool mic_muted = false;
  std::string mic_device = "default";
  bool mic_noise_suppress = false;   // RNNoise/Speex noise suppression on the microphone
  bool mic_noise_gate = false;      // noise gate on the microphone
  bool separate_tracks = false;     // deprecated alias: on means "three tracks" unless audio_tracks says otherwise
  int audio_tracks = 1;             // 1..6 mixer tracks / audio encoders (OBS's track 1..6)
  int audio_bitrate_kbps = 192;
  // What the audio encoders capture: "auto" is the normal mode (audio_auto.h: the programs Windows says
  // are playing, three isolated lanes and Other audio), "desktop" is the output loopback and the
  // microphone, "apps" is only the selected programs and the microphone. The saved routes below are kept
  // in every mode and are only recorded in "apps", so switching modes never loses a selection
  // (audio_routing.h). A directly constructed Config still says "desktop"; the first run and every file
  // written before version 13 move to "auto".
  std::string audio_capture_mode = "desktop";
  // Applications on their own tracks (Discord, a game), recorded only while audio_capture_mode is
  // "apps". Four slots at most; each slot's track follows its position (3..6). While they record the
  // desktop loopback is off, because Windows cannot record the desktop without the app that is also
  // recorded on its own track (audio_routing.h). A file written by an older build may hold more than four:
  // they are kept as written and the extra ones are reported as not recorded, never silently dropped.
  std::vector<AudioApp> audio_apps;
  // Whether the application selection has ever been written, including an intentionally empty one. Kept for
  // compatibility with older files; it no longer seeds a list.
  bool audio_apps_chosen = false;
  // Automatic mode's per-program controls, keyed by the lower-case executable name (audio_auto.h). A
  // fader, mute or exclude change is applied live and is never part of the routing identity: it does
  // not rebuild the encoders or clear the ring. Unknown keys are kept, so a newer build's preferences
  // survive a save by this one.
  std::map<std::string, AutoAudioPref> audio_app_prefs;
  // The Other audio group's own controls, applied on top of each member's saved gain/mute.
  double audio_other_volume = 1.0;
  bool audio_other_muted = false;
  // The mode a file recorded with before automatic mode became the normal one ("desktop" or "apps"). It
  // is read for compatibility and never used to pick a route again; the saved routes stay in audio_apps.
  std::string audio_capture_legacy_mode;

  // Rewind's bindings, carried over: Alt+1 saves, Ctrl+Alt+1 saves a quick clip, Alt+2 screenshots,
  // Alt+3 records. Every default comes from hotkey_defaults.h, the table Settings' Default and Reset
  // write too.
  std::string hotkey_save = default_hotkey("save_replay", false);
  std::string hotkey_quick = default_hotkey("quick_clip", false);
  std::string hotkey_screenshot = default_hotkey("screenshot", false);
  std::string hotkey_record = default_hotkey("toggle_record", false);
  std::string hotkey_toggle_ui = default_hotkey("toggle_ui", false);
  // Opens the last saved clip in the small viewer (and closes it again); never Alt+F4, which is Windows' own.
  std::string hotkey_view_clip = default_hotkey("view_clip", false);
  std::string hotkey_marker = default_hotkey("add_marker", false);
  // The same actions on a controller, as `Pad:A+B`; empty is unbound.
  std::string hotkey_save_pad = default_hotkey("save_replay", true);
  std::string hotkey_quick_pad = default_hotkey("quick_clip", true);
  std::string hotkey_screenshot_pad = default_hotkey("screenshot", true);
  std::string hotkey_record_pad = default_hotkey("toggle_record", true);
  std::string hotkey_toggle_ui_pad = default_hotkey("toggle_ui", true);
  std::string hotkey_view_clip_pad = default_hotkey("view_clip", true);
  std::string hotkey_marker_pad = default_hotkey("add_marker", true);

  std::string clips_dir;
  std::string screenshots_dir;
  std::string recordings_dir;  // empty = <clips_dir>; recordings and session chunks live here
  std::string filename_format = "%CCYY-%MM-%DD %hh-%mm-%ss";
  std::string screenshot_format = "png";  // "png" or "jpg"; anything else reads as png
  // The theme the interface paints, one of the ids in themes.h. An older config's cockpit (auto, green,
  // red, blue) is kept as written and paints the main theme.
  std::string theme = "clutch";
  int ui_scale_percent = 100;  // native panel and quick panel, relative to their existing DPI-aware size
  // mp4 | mkv. MP4 by default: it is what Discord, browsers and phones play. A replay save is written
  // in one go from the ring, so MKV's crash safety buys nothing for clips.
  std::string container = "mp4";
  // The upload limit a shared clip is made to fit, in MiB as Discord counts it: 20 for a free account,
  // 50 or 100 for a boosted server, 500 with Nitro. The Discord webhook is not here: it is a secret,
  // sealed in share.json, and config.json is sent to every client on the pipe.
  int share_limit_mib = 20;
  // BuffrX links (link.h). The page a link points to: the project's page unless a fork sets another
  // (https, ending in "/"; a malformed value falls back to the default, so a link is always clickable),
  // the name the page shows as "Shared by" (empty: it says only that a friend sent it; self-declared,
  // never proof), and the upload cap while serving in kbit/s (0: Unlimited; any other value is a
  // ceiling, lowered to 25 Mbit/s while a game has the focus).
  std::string share_link_page = kDefaultLinkPage;
  std::string share_sender_name;
  int share_upload_kbps = 0;

  // Performance mode (performance.h): the fewest frames lost in a game. `performance_when` chooses off, listed games,
  // or always; `performance_games` are the executables the games choice turns on for while they run. The engine layers
  // the plan over these settings and never rewrites them. Not profile keys.
  PerformanceWhen performance_when = PerformanceWhen::off;
  std::vector<std::string> performance_games;  // lowercase, no ".exe" (normalized_exe_name)

  bool notifications = true;
  bool notify_sound_while_recording = true;  // the chime is heard while the buffer records it, so it is in the clip's audio
  bool notification_sound = true;   // a chime with every notice, unless it is turned off
  // Cards are skipped over a fullscreen game (a topmost window can take it out of independent flip and stutter
  // the game and the clip); true draws them anyway. Warnings always draw.
  bool notify_over_fullscreen = true;  // cards are topmost over a game: the person asked for notices that always arrive
  // On by default: the ring only protects a game while it runs, and a person who installs this wants it
  // waiting before they launch anything. Settings > System turns it off, and the installer's own entry is
  // the only thing written (HKCU Run, one value, removed the moment the switch is off).
  bool launch_on_startup = true;
  bool start_minimized = false;
  // Run the engine elevated. Off until the user turns it on, and then it stays on: an anti-cheat
  // title, an installer-launched game and Windows' own firewall prompt all need it, and a person who
  // has asked for it once does not want to be asked again.
  bool run_as_admin = false;
  bool pause_on_lock = true;
  bool pause_on_battery = false;
  // Keep the newest seconds of the buffer on disk while it runs, so a crash, a power cut or a kill leaves footage
  // behind (recovery_policy.h). On by default: it is about 20 MB and one flush every five seconds, and the RAM ring
  // itself is untouched.
  // A detected fault saves the ring once as a normal clip; nothing is written while capture works
  // (recovery_policy.h).
  bool crash_recovery = true;
  bool codec_auto_picked = false;  // the first start chose a codec from the hardware; a later choice is the person's
  // The one-time move of an auto-picked AV1 on a machine whose AV1 encoder is not NVENC to H.264 has been looked at (2026-11-14).
  bool codec_h264_default_migrated = false;

  int chunk_seconds = 30;
  int recordings_budget_gb = 0;          // 0 = unlimited
  int min_free_gb = 2;
  // Focus-based game capture: switch the capture target to the game the user has focused (fullscreen,
  // not in the exclusion list) and back when it leaves. On by default, as Rewind's detector is.
  bool auto_game_capture = true;
  // With detection on: capture only while a game is on screen. The buffer pauses when the game is gone and starts on
  // the next one (game_only.h). Off by default: the display stays the fallback, and a press outside a game still saves.
  bool game_only = false;
  std::vector<std::string> game_exclusions;  // extra process names never treated as games

  int active_profile = -1;  // the profile applied last (index into profiles.json), -1 none
  int config_version = kConfigVersion;
  static constexpr int kConfigVersion = 13;
};

// What a settings change does to the running pipeline. One table, so the refusal that protects a
// manual recording and ObsHost::apply cannot drift apart: a field that rebuilds the encoder but is
// missing here let a settings change cut a recording in two.
inline constexpr unsigned kConfigSpeedInput = 1u << 0;  // what the governor settled on no longer applies
inline constexpr unsigned kConfigRebuilds = 1u << 1;    // outputs and encoders are made again
unsigned config_change_bits(const Config& before, const Config& after);

// The buffer length the engine and the interface both accept, in seconds.
inline constexpr int kBufferSecondsMin = 5;
inline constexpr int kBufferSecondsMax = 1800;

// The lengths the Quick clip row offers, smallest first. One list, read by the engine's fallback and by the interface's
// row, so neither can offer a length the other does not know.
inline constexpr int kQuickClipChoices[] = {10, 15, 20, 30, 45, 60, 90};

// The quick clip length to keep with this buffer: the chosen one when it is a listed length the buffer can keep, else
// the closest listed length at or below the buffer. 0 only when the buffer is shorter than every listed length (5 s).
// A caller must never hand that 0 to ObsHost::save_clip: there 0 means the whole buffer, which is Save clip's job. A
// press refuses it and explains instead (ui::quick_clip_press).
int quick_clip_for_buffer(int chosen_seconds, int buffer_seconds);

// Mixer tracks actually built: 1..6, with the older `separate_tracks` alias meaning three when the
// explicit count does not ask for more. The spill reservation and the encoders must agree on this.
inline int audio_track_count(const Config& config) {
  // Automatic capture always builds six: the mix, the microphone, three isolated lanes and Other audio.
  // They are made once and never rebuilt because a program arrived or left, so a lane keeps its encoder
  // through a whole recording.
  if (config.audio_capture_mode == "auto") return 6;
  int tracks = config.audio_tracks >= 1 && config.audio_tracks <= 6 ? config.audio_tracks : 1;
  if (config.separate_tracks && tracks < 2) tracks = 3;
  if (config.separate_tracks && config.mic_enabled && tracks < 3) tracks = 3;
  // An app slot needs its own encoder even when the explicit count says one: the plan puts slot i on
  // track 3 + i (audio_routing.h), so the plan and the spill reservation must agree on every track the
  // mixer writes to. Saved routes count only while the app mode records them; a route kept for later must
  // not build encoders in desktop mode.
  if (config.audio_capture_mode == "apps") {
    const size_t slots = config.audio_apps.size() < 4 ? config.audio_apps.size() : 4;  // kAudioAppMax
    if (slots > 0 && tracks < static_cast<int>(slots) + 2) tracks = static_cast<int>(slots) + 2;
  }
  return tracks;
}

// Brings a config written by an older build up to kConfigVersion. Version 2: clips default to MP4,
// and the hotkeys move to Rewind's Alt+1 / Ctrl+Alt+1 / Alt+2 / Alt+3 — each only where the stored
// value is still the old default, so a choice the user made is kept. Version 3: rate_control,
// save_accuracy and audio_codec are gone; they were saved and loaded but never read, so a file
// carrying them is simply a version 3 file without them. Version 4: quality_tier, cqp, bitrate_kbps,
// duration_first and buffer_memory_mb become one quality choice; config_from_json translates them
// wherever they arrive (a file, a profile, an older client), so an explicit rate survives as custom.
// Version 6 adds the Timeline's retention window (`timeline_minutes`), off unless one of the four lengths
// is stored: nothing to carry over, and anything else reads as off.
// Version 7 adds the crash-recovery tail (`crash_recovery`), on unless a file says otherwise.
// Version 8 fixes the shipped pad bindings: the old defaults had Record unbound and the panel on
// View+Menu, while four actions are meant to ship bound (Record on View+Menu, panel on View+Y). A file
// still carrying that exact pair moves; a chord the person chose is kept.
// Version 9 adds application audio routes (`audio_apps`), empty in every file written before it: no
// desktop loopback is lost and no existing recording's layout changes.
// Version 10 adds `audio_capture_mode` ("desktop" or "apps"). A file that already has routes reads as
// "apps", because that is what it recorded until now; a file without them reads as "desktop". The routes
// are kept either way, so switching back to "apps" restores them. A file that already carries a valid mode
// chose it, whatever its version says, so `mode_stored` keeps that value (config_from_json has parsed it).
// Version 11 makes the application selection four slots whose tracks follow their position (3..6) and adds
// the optional Current game slot (`kind`). A version 10 file's stored `track` numbers are ignored from now
// on: its first four apps still record, each on its own track, and entries past the fourth are kept in the
// file and reported as not recorded (status.audio.legacy_over) instead of being silently dropped.
// Version 12 adds `audio_apps_chosen`: whether the application selection was ever written. It is kept for
// compatibility with files that carry it; a selection is never seeded from it, so a new switch to Selected
// apps starts empty and the person adds the programs to record.
// Version 13 makes automatic capture the normal mode. A file that recorded desktop or selected apps moves to
// "auto"; its old mode is kept in `audio_capture_legacy_mode`, its routes stay in `audio_apps` untouched, and
// each route's gain and mute seed an automatic preference for the same executable. Files already written by
// older builds keep their tracks: nothing reads the mode when a recorded file is opened.
void migrate_config(Config& config, bool mode_stored = false);

// Whether a Timeline window is one the engine offers: 0 (off), 30, 60 or 120 minutes.
bool timeline_minutes_allowed(int minutes);
bool timeline_idle_minutes_allowed(int minutes);  // 0, 5, 15, 30 or 60

// libobs's obs_scale_type for a filter name (bicubic when unknown), and win-capture's monitor `method` for a capture
// method name (0, automatic, when unknown).
int scale_type_for_filter(const std::string& filter);
int capture_method_for(const std::string& method);

// Data root: <exe>\data when a portable marker sits beside the executable, else %LOCALAPPDATA%\BuffrX.
// Returns false only when neither location can be created.
bool resolve_data_root(std::string& data_root, bool& portable);

// How the video canvas and the encoder output are sized.
struct CanvasPlan {
  int canvas_width = 0;
  int canvas_height = 0;
  int output_width = 0;
  int output_height = 0;
};

// Whether two picture sizes have the same shape, within a rounding tolerance: 1366x768 and 1280x720 are the same
// 16:9 (0.03% apart), 16:9 and 21:9 are not (31% apart). Integer math, so a caller (and a test) needs nothing else.
inline bool same_picture_shape(int width_a, int height_a, int width_b, int height_b) {
  if (width_a <= 0 || height_a <= 0 || width_b <= 0 || height_b <= 0) return false;
  const int64_t left = static_cast<int64_t>(width_a) * height_b;
  const int64_t right = static_cast<int64_t>(height_a) * width_b;
  const int64_t difference = left > right ? left - right : right - left;
  return difference * 200 <= (left < right ? left : right);  // 0.5%: a pixel of rounding is not a different shape
}

// Whether the canvas has to take the configured output's shape instead of the capture's.
// libobs scales the finished canvas straight to the output, so a canvas of another shape is squeezed there, and no
// scene bounds can undo it: a 21:9 display recorded at a configured 1920x1080 was stretched to 16:9 by the output
// scale. With the canvas at the output's shape the source is fitted inside it (bars) and the output scale is even.
// Only when the output is configured and the target's size is known. A Stretch choice still fills the canvas.
inline bool canvas_takes_output_shape(const Config& config, int target_width, int target_height) {
  if (config.output_width <= 0 || config.output_height <= 0) return false;
  if (target_width <= 0 || target_height <= 0) return false;
  return !same_picture_shape(target_width, target_height, config.output_width, config.output_height);
}

// The canvas follows the capture target's native size and the output is what the user asked to record
// at, so the frame is scaled once, by the video thread, and never inside the scene. `target_width`
// and `target_height` come from Win32 before the first obs_reset_video (0 when the target is not
// known yet); a configured size of 0 means "the same as the canvas".
// One exception: a configured output of another shape takes the canvas (canvas_takes_output_shape), because the
// output scale would squeeze the picture otherwise. A target whose size is not known cannot offer a shape to keep.
inline CanvasPlan plan_canvas(const Config& config, int target_width, int target_height) {
  // A size counts only as a complete pair of positive dimensions: half a size is a mistake, not a
  // request for the other half to be guessed.
  const bool target_known = target_width > 0 && target_height > 0;
  const bool configured = config.output_width > 0 && config.output_height > 0;
  const bool output_shape = canvas_takes_output_shape(config, target_width, target_height);

  CanvasPlan plan;
  if (target_known && !output_shape) {
    plan.canvas_width = target_width;
    plan.canvas_height = target_height;
  } else if (configured) {
    plan.canvas_width = config.output_width;
    plan.canvas_height = config.output_height;
  } else {
    plan.canvas_width = 1920;
    plan.canvas_height = 1080;
  }
  plan.output_width = configured ? config.output_width : plan.canvas_width;
  plan.output_height = configured ? config.output_height : plan.canvas_height;
  return plan;
}

// The size the video encoder actually receives: the configured output when there is one, else the
// canvas. Both configured halves must be present to count — half a size is not a size, and asking
// for a 1920-wide stream of height 1 is worse than falling back to the canvas. The bitrate ladder
// reads this, so a configured 1080p output asks for less than a native 1440p one.
void encoder_size(const Config& config, int canvas_width, int canvas_height, int& width, int& height);
// The same rule on four plain numbers, for the settings page, which reads the config as JSON and
// never holds a Config. One implementation of the rule, so the page cannot disagree with the engine
// about what the encoder is being fed.
void encoder_size(int output_width, int output_height, int canvas_width, int canvas_height, int& width, int& height);

// Whether the capture fills the canvas (true) or keeps its aspect inside it (false). Only the explicit
// "stretch" fills: a 21:9 game on a 16:9 canvas is letterboxed, never distorted. "auto" is kept as the
// older name for "fit", so a saved config reads the same way it always did.
bool capture_fills_canvas(const std::string& stretch_mode, int source_width, int source_height, int canvas_width,
                          int canvas_height);

// The window whose size the canvas follows before the first video reset. Only a window or game target has
// one: a display target is sized from its monitor, never from a window id an earlier mode left behind (a
// stale panel id sized a 2560x1440 display's canvas to the 1440x1008 panel and recorded black).
std::string capture_window_target(const Config& config);

// Whether a capture rate gives an even picture on a display at `refresh_hz`. The capture takes the newest
// frame the source has at each of its own ticks, so a rate that divides the refresh (60 on 120 Hz) steps
// evenly and a rate that does not (120 on 144 Hz, 165) skips or repeats a frame in a pattern that shows as
// judder however smooth the game is. Within a hertz counts: displays report 164 for 165. An unknown refresh
// (0) is never a complaint, and a rate at or above the refresh has nothing to be uneven against.
bool capture_rate_is_even(int capture_fps, int refresh_hz);

// Default clips/screenshots locations: the user's Videos known folder, or the data root as fallback.
void apply_path_defaults(Config& config, const std::string& data_root);

// Where recordings and session chunks live: the configured recordings folder, or the clips folder when
// none is set.
std::string recordings_base_dir(const Config& config);
// The one folder a manual recording is filed in, and the one holding the sessions: both under the base,
// so a moved recordings folder moves them together.
inline constexpr const char* kRecordingsFolder = "Recordings";
std::string recordings_folder(const Config& config);
std::string sessions_dir(const Config& config);

// Load and merge over defaults. A missing file is not an error: defaults are written on first save.
bool load_config(const std::string& path, Config& config, std::string& error);

// Atomic save: write beside the target, then replace. A half-written config must never be readable.
bool save_config(const std::string& path, const Config& config, std::string& error);

Json config_to_json(const Config& config);
void config_from_json(const Json& json, Config& config);

// The audio fields of a stored file or a patch. The config file and the pipe's `config.set` are two writers
// of these values, so the parse and the floor/ceiling on each live here once: a track count outside 1..6
// would build encoders libobs has no track for, and a hand-edited volume would reach obs_source_set_volume
// as inf and blast the track.
void config_apply_audio(const Json& json, Config& config);
// The automatic preferences as the pipe and the config file carry them: one object keyed by the lower-case
// executable, each with volume, muted and excluded. A patch merges fields; this writes them all.
Json auto_audio_prefs_json(const std::map<std::string, AutoAudioPref>& prefs);

// A profile's patch with the machine-wide settings taken out: game switching and its bindings, the
// library folders and retention. A profile is a saved config, and applying one must only change how a
// game is captured and encoded.
Json profile_patch(const Json& patch);

}  // namespace bx
