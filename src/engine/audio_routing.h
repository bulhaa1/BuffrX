// Source-to-track routing for the mixer, and the recorded-time track labels a file carries.
//
// Track 1 is always the audible mix, and the layout is fixed: track 1 mix, track 2 microphone, tracks
// 3..6 the four selected application slots. A slot's track belongs to its position in the selection, so
// adding or removing a slot moves only the slots after it, and a slot never lands on another's track.
//
// The application sources are Windows process-loopback captures, so they contain that app and its
// children only. Windows has no "the desktop except these processes" capture, so an app track cannot be
// combined with the device loopback without recording that app twice: while apps are recorded, the
// desktop loopback is off. That is a hard platform limit, said in the settings page, never hidden.
//
// Four slots, and one of them may be the Current game: a slot that follows the game BuffrX is actually
// capturing (not any fullscreen candidate) and keeps one stable track while the game changes, so a
// recording never needs its encoders rebuilt for a new game and a file's labels stay generic ("Game"),
// truthful even when a replay spans two games.
//
// Whether the saved routes record is `audio_capture_mode`: "apps" records them, "desktop" (the default)
// records the output loopback and keeps the routes saved for later. Performance mode's effective config
// forces "desktop" without touching the stored mode or the routes.
//
// The first switch to "apps" starts with an empty list: the person adds the programs to record. `audio_apps_chosen`
// is still written and read for compatibility, but it seeds nothing.
//
// Legacy files: routes are stored as written and never truncated by loading. A file with more than four
// entries (a version 9/10 build allowed eight) or with several apps on one track still records its first
// four slots, each on its own track; the rest is reported as not recorded (status.audio.legacy_over) and
// the page offers to remove it. Nothing is silently dropped, and nothing is rewritten behind the person.
//
// The container does not carry stream titles through OBS's muxer, so a multitrack file gets one
// sidecar beside it (`<file>.buffrx-audio.json`) naming each track and its role. It describes the
// routing at recording time, not the live config.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../shared/json.h"
#include "config.h"

namespace bx {

inline constexpr int kAudioAppMax = 4;          // the four selected slots, tracks 3..6
inline constexpr int kAudioAppStoredMax = 16;   // a hand-edited file is read whole, never truncated on load
inline constexpr int kAudioAppFirstTrack = 3;   // slot 0's track; slot i is 3 + i
inline constexpr const char* kAudioSidecarSuffix = ".buffrx-audio.json";
inline constexpr const char* kAudioGameKind = "game";
inline constexpr const char* kAudioGameSource = "app:game";  // generic: the track is the game BuffrX captured

// The game BuffrX is actually capturing, resolved by the protocol from its own capture target (never from
// the detector's list of candidates). Empty/inactive while no game is captured or while the display is
// what the capture source is on with no game behind it.
struct GameAudioTarget {
  bool active = false;
  std::string exe;
  std::string window;  // win-capture's "title:class:exe" id at this moment
  std::string title;
};

// Whether an entry is the Current game slot rather than a named program. A saved file may still carry one: the
// route keeps following the captured game so an older selection never silently loses its audio. New selections
// are named programs only. `audio_apps_chosen` is kept in the file for compatibility and no longer seeds a list.
bool audio_app_is_game(const AudioApp& app);
// The identity a selection is compared by: the executable (case-insensitive), or "game".
std::string audio_app_key(const AudioApp& app);
// A stored gain, kept finite and inside 0..1: a hand-edited file or another client cannot hand libobs a NaN.
double audio_app_volume(double value);

// The Current game slot as a stored route: kind "game", with no executable or window of its own, because
// it follows whatever game BuffrX is capturing.
AudioApp current_game_app();

// Wins over duplicates and malformed entries in a hand-edited or legacy file. Never truncates below
// kAudioAppStoredMax, so loading a file with more slots than this build records keeps every entry.
std::vector<AudioApp> normalize_audio_apps(const std::vector<AudioApp>& apps);
// Routing identity only: the same program on the same track. A gain or mute change is not a route
// change, so this keeps a fader from rebuilding the encoders (config_change_bits).
bool audio_apps_equal(const std::vector<AudioApp>& left, const std::vector<AudioApp>& right);
// Whether two selections need no live control update either: identity, gain and mute all agree. Used to
// decide whether a saved gain change has to be applied to the running sources in place.
bool audio_app_controls_equal(const std::vector<AudioApp>& left, const std::vector<AudioApp>& right);

// The saved routes that actually record: the first kAudioAppMax normalized entries while the mode is
// "apps", none while it is "desktop". A route kept for later never creates a source, a track or a rebuild
// in desktop mode.
std::vector<AudioApp> effective_audio_apps(const Config& config);
// The entries a legacy file holds beyond the four slots: reported, never recorded.
std::vector<AudioApp> over_limit_audio_apps(const Config& config);

struct AudioTrackInfo {
  int track = 1;                     // 1..6, as the product and OBS count them
  std::string role;                  // mix | desktop | microphone | application | combined | empty
  std::string title;                 // "BuffrX Mix", "BuffrX Microphone", "BuffrX Game", ...
  std::vector<std::string> sources;  // desktop | microphone | app:<exe> | app:game
};

struct AudioRoutingPlan {
  int track_count = 1;
  bool desktop_captured = true;  // false while an application track exists
  uint32_t desktop_mixers = 1u << 0;
  uint32_t mic_mixers = 1u << 0;
  std::vector<AudioApp> apps;     // effective, in slot order; a game slot carries the resolved target or empty exe
  std::vector<uint32_t> app_mixers;  // one per apps entry
  std::vector<AudioTrackInfo> tracks;  // track_count entries, index 0 = track 1
  int game_slot = -1;            // index into apps, or -1
  int game_track = 0;            // its track while game_slot >= 0, 0 otherwise
  // The named slot whose program is the captured game, or -1. A game selected by name already captures it,
  // so the Current game slot then has no source: the same sound is never recorded twice.
  int game_covered_slot = -1;
};

AudioRoutingPlan plan_audio_routing(const Config& config, const GameAudioTarget& game = GameAudioTarget{});
// The automatic mode's six fixed tracks: 1 mix, 2 microphone, 3..5 the isolated programs the plan holds,
// 6 Other audio. The source ids are the stable bus ids (audio_auto.h: auto:bus:3..6), so the mix decomposes
// exactly into the lanes after it and the editor can level any lane without the file becoming "incomplete".
// Track 1's sources are the microphone and the buses that are on, never the desktop loopback: automatic
// mode has no fallback that could record an excluded program.
AudioRoutingPlan plan_auto_audio_routing(const Config& config, const std::vector<AutoAudioLane>& lanes);

// The routes as they are stored and sent on the pipe.
Json audio_apps_json(const std::vector<AudioApp>& apps);

// The labels and roles of the routing a file was recorded with, with no media identity: a sidecar needs
// `media` (audio_layout.h) before it is believed, and write_audio_sidecar supplies it.
Json audio_track_metadata(const AudioRoutingPlan& plan);

// `<file>.buffrx-audio.json`, beside the media file.
std::string audio_sidecar_path(const std::string& media_path);

// Writes the sidecar for a multitrack plan; a single-track file gets none. False only when the file
// could not be written, which never fails a clip: the footage is the product, the labels are not. The
// media's final identity is captured with it, so this is for a file whose writer has closed.
bool write_audio_sidecar(const std::string& media_path, const AudioRoutingPlan& plan);

// The same labels beside a file a writer still owns, with no identity yet: it reads as an import until
// bind_audio_layout (audio_layout.h) captures the final bytes at the close.
bool write_audio_sidecar_pending(const std::string& media_path, const AudioRoutingPlan& plan);

}  // namespace bx
