// BuffrX configuration: one JSON file under the data root, atomic writes, per-key defaults.
//
// Deliberately one flat object rather than nested sections: the alpha's surface is small enough to
// read at a glance, and a flat map means an older build ignores keys it does not know instead of
// failing to load.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "../shared/json.h"
#include "hotkey_defaults.h"

namespace bx {

// The static link page that makes a BuffrX link clickable in a chat.
inline constexpr char kDefaultLinkPage[] = "https://buffrx.app/l/";

// When Performance mode is used: off = never, games = only while a listed game is captured,
// always = the user's manual switch. Replaces the old performance_mode boolean; older files are
// migrated in config_from_json. Not a profile key.
enum class PerformanceWhen { off, games, always };

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
  // How many steps the encoder governor (encoder_governor.h) has lowered the encoder's speed settings because
  // it could not keep up, 0..3. The engine writes it; changing the settings that cause it resets it to 0.
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

  int chunk_seconds = 30;
  int recordings_budget_gb = 0;          // 0 = unlimited
  int min_free_gb = 2;
  // Focus-based game capture: switch the capture target to the game the user has focused (fullscreen,
  // not in the exclusion list) and back when it leaves. On by default, as Rewind's detector is.
  bool auto_game_capture = true;
  std::vector<std::string> game_exclusions;  // extra process names never treated as games

  int active_profile = -1;  // the profile applied last (index into profiles.json), -1 none
  int config_version = kConfigVersion;
  static constexpr int kConfigVersion = 8;
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

// Mixer tracks actually built: 1..6, with the older `separate_tracks` alias meaning three when the
// explicit count does not ask for more. The spill reservation and the encoders must agree on this.
inline int audio_track_count(const Config& config) {
  int tracks = config.audio_tracks >= 1 && config.audio_tracks <= 6 ? config.audio_tracks : 1;
  if (config.separate_tracks && tracks < 2) tracks = 3;
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
void migrate_config(Config& config);

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

// The canvas follows the capture target's native size and the output is what the user asked to record
// at, so the frame is scaled once, by the video thread, and never inside the scene. `target_width`
// and `target_height` come from Win32 before the first obs_reset_video (0 when the target is not
// known yet); a configured size of 0 means "the same as the canvas".
CanvasPlan plan_canvas(const Config& config, int target_width, int target_height);

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

// A profile's patch with the machine-wide settings taken out: game switching and its bindings, the
// library folders and retention. A profile is a saved config, and applying one must only change how a
// game is captured and encoded.
Json profile_patch(const Json& patch);

}  // namespace bx
