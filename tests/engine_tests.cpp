// BuffrX engine tests: the pure logic that has no GPU, no libobs and no window in it.
//
// These are regression guards for behaviour that already exists and has been verified on hardware
// (see ISSUES.md). They are deliberately not described as TDD: the code came first, so a passing run
// proves only that the behaviour has not moved since. Every new engine change from here is written
// test-first, and these guards exist so a refactor of that change has something to break.
//
// Run: build the buffrx-tests target and execute it. Exit code 0 means every check passed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <future>
#include <io.h>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

// The tree is not always where the build was configured: resolve the source root at run time so a
// copied test binary still finds docs/protocol.md and the theme files.
static std::string source_root() {
  const char* env = std::getenv("BUFFRX_SOURCE_DIR");
  if (env && *env) return env;
  char exe[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0) {
    std::string dir(exe);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir.resize(slash);
    const std::string parent = dir + "\\..";
    if (GetFileAttributesA((parent + "\\docs\\protocol.md").c_str()) != INVALID_FILE_ATTRIBUTES) return parent;
  }
#ifdef BUFFRX_SOURCE_DIR
  return BUFFRX_SOURCE_DIR;
#else
  return ".";
#endif
}

#include "../src/shared/files.h"
#include "../src/engine/benchmark.h"
#include "../src/engine/audio_meter.h"
#include "../src/engine/buffer_watch.h"
#include "../src/engine/encoder_watch.h"
#include "../src/engine/encoder_governor.h"
#include "../src/engine/capacity_memory.h"
#include "../src/engine/dir_listing.h"
#include "../src/engine/file_final.h"
#include "../src/engine/gamepad.h"
#include "../src/engine/idle_policy.h"
#include "../src/engine/link_idle.h"
#include "../src/engine/link.h"
#include "../src/engine/link_secret.h"
#include "../src/engine/lock_watch.h"
#include "../src/engine/log_policy.h"
#include "../src/engine/loop_wait.h"
#include "../src/engine/process_cache.h"
#include "../src/engine/sleep_wait.h"
#include "../src/engine/ring_index.h"
#include "../src/engine/memory_policy.h"
#include "../src/engine/webview.h"
#include "../src/engine/packet_hash.h"
#include "../src/engine/ring_disk.h"
#include "../src/engine/ring_spill.h"
#include "../src/engine/ring_store.h"
#include "../src/engine/performance.h"
#include "../src/engine/process_tuning.h"
#include "../src/engine/recording_watch.h"
#include "../src/engine/recovery_policy.h"
#include "../src/engine/recording_space.h"
#include "../src/engine/capture_enum.h"
#include "../src/engine/clip_check.h"
#include "../src/engine/mp4_edit.h"
#include "../src/engine/command_policy.h"
#include "../src/engine/commands.h"
#include "../src/engine/game_watch.h"
#include "../src/engine/hotkey_enable.h"
#include "../src/engine/windowed_game.h"
#include "../src/engine/capture_follow.h"
#include "../src/engine/config.h"
#include "../src/engine/display_target.h"
#include "../src/engine/encoder_choice.h"
#include "../src/engine/encoder_settings.h"
#include "../src/engine/gpu.h"
#include "../src/engine/work_queue.h"
#include "../src/engine/library.h"
#include "../src/engine/link_math.h"
#include "../src/engine/media.h"
#include "../src/engine/obs_host.h"
#include "../src/engine/saved_replay.h"
#include "../src/engine/save_filing.h"
#include "../src/engine/save_check.h"
#include "../src/engine/media_math.h"
#include "../src/engine/export_job_json.h"
#include "../src/engine/stitch_plan.h"
#include "../src/engine/platform.h"
#include "../src/engine/protocol_commands.h"
#include "../src/engine/rec_mark.h"
#include "../src/engine/session_store.h"
#include "../src/engine/share_math.h"
#include "../src/engine/webhook_list.h"
#include "../src/engine/elevation_policy.h"
#include "../src/engine/short_link.h"
#include "../src/engine/themes.h"
#include "../src/engine/timeline_chunks.h"
#include "../src/engine/update_math.h"
#include "../src/engine/public_links.h"
#include "../src/engine/update_notes.h"
#include "../src/engine/update_feed.h"
#include "../src/engine/update_download.h"
#include "../src/engine/update_pending.h"
#include "../src/engine/timeline_pins.h"
#include "../src/engine/timeline_retention.h"
#include "../src/engine/timeline_overlay.h"
#include "../src/engine/timeline_state.h"
#include "../src/engine/timeline_loudness.h"
#include "../src/engine/timeline_thumbs.h"
#include "../src/engine/toast.h"
#include "../src/engine/ui/ui_format.h"
#include "../src/engine/ui/ui_anim_math.h"
#include "../src/engine/ui/ui_frame_stats.h"
#include "../src/engine/ui/ui_icons.h"
#include "../src/engine/ui/ui_link_math.h"
#include "../src/engine/ui/ui_live_math.h"
#include "../src/engine/ui/ui_window_math.h"
#include "../src/engine/ui/ui_pad_nav.h"
#include "../src/shared/json.h"
#include "../src/shared/log.h"
#include "../src/shared/pipe.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* name) {
  ++g_checks;
  if (condition) {
    std::printf("  ok   %s\n", name);
  } else {
    ++g_failures;
    std::printf("  FAIL %s\n", name);
  }
}

std::string temp_directory() {
  char buffer[512] = {0};
  DWORD length = GetEnvironmentVariableA("TEMP", buffer, sizeof(buffer));
  std::string root = length > 0 ? std::string(buffer, length) : std::string(".");
  return root + "\\buffrx-tests";
}

// The leases a sweep honours are one list per process, read under the retention gate (timeline_retention.h): point it at
// `pins` for one sweep. Cleared after a test, so one test cannot pin footage for the next.
void set_test_leases(std::vector<bx::TimelinePin> pins) {
  bx::timeline_leases_update([&pins](std::vector<bx::TimelinePin>& list) { list = pins; });
}

// Every run starts from an empty directory: a leftover file from the previous run made the
// reservation check depend on the order the tests were last run in.
void remove_tree(const std::string& root) {
  WIN32_FIND_DATAA data{};
  HANDLE find = FindFirstFileA((root + "\\*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    const std::string name = data.cFileName;
    if (name == "." || name == "..") continue;
    const std::string path = root + "\\" + name;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      remove_tree(path);
    } else {
      DeleteFileA(path.c_str());
    }
  } while (FindNextFileA(find, &data));
  FindClose(find);
  RemoveDirectoryA(root.c_str());
}

void test_json() {
  std::printf("json\n");
  const std::string text = R"({"a":1,"b":"two\n","c":[true,null,3.5],"d":{"e":-4}})";
  bx::Json value;
  std::string error;
  check(bx::Json::parse(text, value, error), "parses an object");
  check(value["a"].as_int(-1) == 1, "integer survives as an integer");
  check(value["b"].as_string() == "two\n", "escape sequences decode");
  check(value["c"].items().size() == 3, "array length");
  check(value["c"].items()[0].as_bool() == true, "bool in an array");
  check(value["c"].items()[1].is_null(), "null in an array");
  check(value["d"]["e"].as_int(0) == -4, "nested negative integer");
  check(value["missing"].is_null(), "missing key reads as null, never crashes");
  // A number that is not finite has no JSON spelling. Written as "nan" it made the whole config file
  // unreadable, and the next start fell back to defaults: the user's settings were gone.
  {
    bx::Json odd = bx::Json::object();
    odd.set("volume", bx::Json(std::nan("")));
    odd.set("gain", bx::Json(HUGE_VAL));
    odd.set("fine", bx::Json(0.5));
    bx::Json back;
    std::string odd_error;
    check(bx::Json::parse(odd.dump(), back, odd_error), "a value that is not finite still writes readable JSON");
    check(back["volume"].is_null() && back["gain"].is_null(), "and reads back as missing, so the default applies");
    check(back["fine"].as_double() == 0.5, "the finite values beside it survive");
  }

  bx::Json again;
  std::string second_error;
  check(bx::Json::parse(value.dump(), again, second_error), "round trips through dump and parse");
  check(again["b"].as_string() == "two\n", "round trip preserves escapes");

  bx::Json malformed;
  std::string malformed_error;
  check(!bx::Json::parse("{\"a\":", malformed, malformed_error), "rejects a truncated object");
  check(!bx::Json::parse("{\"a\":1} trailing", malformed, malformed_error), "rejects trailing text");
  check(!bx::Json::parse("[1,2", malformed, malformed_error), "rejects an unterminated array");
  // A number is read the way JSON writes one: a sign, digits, at most one dot and one exponent.
  // The scan used to take any of those characters anywhere, so `1-2` became 1 and `+1` became 1:
  // a broken command or a hand-edited config applied a value nobody wrote instead of being refused.
  check(!bx::Json::parse("{\"a\":1-2}", malformed, malformed_error), "rejects two numbers in one");
  check(!bx::Json::parse("{\"a\":+1}", malformed, malformed_error), "rejects a leading plus");
  check(!bx::Json::parse("{\"a\":1.2.3}", malformed, malformed_error), "rejects two dots");
  check(!bx::Json::parse("{\"a\":1e}", malformed, malformed_error), "rejects an exponent with no digits");
  check(!bx::Json::parse("{\"a\":-}", malformed, malformed_error), "rejects a sign with no digits");
  // 1e999 reads as +inf, which JSON has no spelling for: written back it became null and the value
  // silently disappeared. It is a parse error now, and so is anything that overflows the range.
  check(!bx::Json::parse("{\"a\":1e999}", malformed, malformed_error), "rejects a number past the double range");
  check(!bx::Json::parse("{\"a\":1e-999}", malformed, malformed_error), "rejects a number under the double range");
  // Notepad's "UTF-8 with BOM" and PowerShell's own UTF8 encoder both put EF BB BF in front of the file,
  // and a settings file saved that way used to fall back to every default instead of being read.
  bx::Json bom;
  check(bx::Json::parse("\xef\xbb\xbf{\"a\":1}", bom, malformed_error), "reads a file saved with a byte order mark");
  check(bom["a"].as_int() == 1, "the mark does not change the value");
  bx::Json negative;
  check(bx::Json::parse("{\"a\":-1.5e3}", negative, malformed_error) && negative["a"].as_double() == -1500.0,
        "reads a negative fractional exponent");
  // Escaped emoji arrive as a surrogate pair. Each half alone is not a character, and writing the two
  // halves out as they came made six bytes that Windows refuses as a file name.
  bx::Json emoji;
  check(bx::Json::parse("{\"s\":\"\\ud83d\\ude00\"}", emoji, malformed_error), "reads a surrogate pair");
  check(emoji["s"].as_string() == "\xf0\x9f\x98\x80", "as one four byte character, not two halves");
  check(!bx::Json::parse("{\"s\":\"\\ud83d\"}", emoji, malformed_error), "rejects a lone high surrogate");

  // A frame is only bounded in bytes, and every value in it becomes its own heap object: a frame full
  // of tiny numbers would cost the engine far more memory than the bytes it arrived in.
  std::string flood = "[";
  for (size_t i = 0; i < bx::kMaxJsonItems; ++i) flood += "0,";
  flood += "0]";
  check(!bx::Json::parse(flood, malformed, malformed_error), "rejects a frame with more values than the budget");
  std::string modest = "[";
  for (int i = 0; i < 1000; ++i) modest += (i == 0 ? "0" : ",0");
  modest += "]";
  check(bx::Json::parse(modest, malformed, malformed_error) && malformed.items().size() == 1000,
        "reads a thousand values");

  bx::Json built = bx::Json::object();
  built.set("s", std::string("quote\"and\\slash"));
  check(built.dump().find("\\\"") != std::string::npos, "escapes quotes on output");
  // A refused setting restores the fields it wrote. A field that did not exist before is erased, because a JSON
  // null is a value: the config would keep a key nobody had.
  bx::Json erased = bx::Json::object();
  erased.set("a", bx::Json(1));
  erased.set("b", bx::Json(2));
  check(erased.erase("a") && !erased.has("a") && erased["b"].as_int() == 2, "erasing a key leaves the rest");
  check(!erased.erase("missing"), "erasing a key that is not there says so");
  check(!bx::Json(1).erase("a"), "a value that is not an object erases nothing");
}

void test_config() {
  std::printf("config\n");
  {
    // A config that cannot be read is kept beside itself before defaults take over: the next save
    // writes the defaults, and without the copy the user's own settings would be gone for good.
    CreateDirectoryA(temp_directory().c_str(), nullptr);
    const std::string path = temp_directory() + "\\broken-config.json";
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
      std::fputs("{\"buffer_seconds\": 90, \"desktop_volume\": nan}", file);
      std::fclose(file);
    }
    bx::Config broken;
    std::string error;
    check(!bx::load_config(path, broken, error), "an unreadable config is reported");
    check(GetFileAttributesA((path + ".unreadable").c_str()) != INVALID_FILE_ATTRIBUTES,
          "and kept as .unreadable before anything can overwrite it");
    check(error.find(".unreadable") != std::string::npos, "the error says where it was kept");
    // A second unreadable config keeps its own copy: the first one is the settings the person had
    // before, and overwriting it with a later, equally broken write loses the only record of them.
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
      std::fputs("{\"buffer_seconds\": 30, \"mic_volume\": nan}", file);
      std::fclose(file);
    }
    bx::Config broken_again;
    std::string second_error;
    check(!bx::load_config(path, broken_again, second_error), "a second unreadable config is reported");
    check(GetFileAttributesA((path + ".unreadable").c_str()) != INVALID_FILE_ATTRIBUTES,
          "the first kept copy stays");
    check(GetFileAttributesA((path + ".unreadable-2").c_str()) != INVALID_FILE_ATTRIBUTES,
          "and the second write is kept beside it, not on top of it");
    std::string first_kept;
    if (FILE* file = std::fopen((path + ".unreadable").c_str(), "rb")) {
      char buffer[256] = {0};
      const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
      first_kept.assign(buffer, read);
      std::fclose(file);
    }
    check(first_kept.find("buffer_seconds\": 90") != std::string::npos,
          "the first copy still holds the text it was made from");
    check(second_error.find(".unreadable-2") != std::string::npos, "the error names the copy just kept");
  }
  {
    // Version 1 -> 2: old defaults move to MP4 and Rewind's hotkeys; a value the user chose stays.
    bx::Config old;
    old.config_version = 1;
    old.container = "mkv";
    old.hotkey_save = "Alt+C";
    old.hotkey_quick = "Alt+Z";
    old.hotkey_screenshot = "Ctrl+F12";
    old.hotkey_record = "Alt+R";
    bx::migrate_config(old);
    check(old.container == "mp4", "an old mkv default becomes mp4");
    check(old.hotkey_save == "Alt+1" && old.hotkey_quick == "Ctrl+Alt+1" && old.hotkey_record == "Alt+3",
          "old default hotkeys move to Rewind's");
    check(old.hotkey_screenshot == "Ctrl+F12", "a hotkey the user chose is kept");
    check(old.config_version == bx::Config::kConfigVersion, "the config is marked current");
    bx::Config current;
    current.container = "mkv";
    bx::migrate_config(current);
    check(current.container == "mkv", "a current config's mkv is the user's choice and stays");
  }
  {
    // Version 7 -> 8: the two old shipped pad defaults move to the pair the guide names; a chosen chord stays.
    bx::Config old_pads;
    old_pads.config_version = 7;
    old_pads.hotkey_record_pad.clear();
    old_pads.hotkey_toggle_ui_pad = "Pad:VIEW+MENU";
    bx::migrate_config(old_pads);
    check(old_pads.hotkey_record_pad == "Pad:VIEW+MENU" && old_pads.hotkey_toggle_ui_pad == "Pad:Y+VIEW",
          "the old shipped pad pair moves to Record on View+Menu and the panel on View+Y");
    bx::Config chosen_pads;
    chosen_pads.config_version = 7;
    chosen_pads.hotkey_record_pad.clear();
    chosen_pads.hotkey_toggle_ui_pad = "Pad:LB+VIEW";
    bx::migrate_config(chosen_pads);
    check(chosen_pads.hotkey_toggle_ui_pad == "Pad:LB+VIEW" && chosen_pads.hotkey_record_pad.empty(),
          "a pad chord the person chose is kept");
  }
  // Stretched games: what the clip looks like when the source's shape is not the canvas's.
  check(!bx::capture_fills_canvas("auto", 1280, 960, 2560, 1440), "a 4:3 game on a 16:9 canvas is fitted, not stretched");
  check(!bx::capture_fills_canvas("auto", 2560, 1440, 2560, 1440), "a native game has nothing to stretch");
  check(!bx::capture_fills_canvas("auto", 1920, 1080, 2560, 1440), "a 16:9 game at a lower resolution keeps its shape");
  check(!bx::capture_fills_canvas("auto", 1280, 960, 2560, 1440), "a 4:3 window is fitted, not stretched");
  check(!bx::capture_fills_canvas("auto", 1280, 960, 2560, 1440), "auto never stretches a display");
  check(bx::capture_fills_canvas("stretch", 1280, 960, 2560, 1440), "stretch fills whatever the source");
  check(!bx::capture_fills_canvas("fit", 1280, 960, 2560, 1440), "fit keeps the game's aspect");
  check(!bx::capture_fills_canvas("auto", 0, 0, 2560, 1440), "a source with no size yet is fitted");
  bx::Config config;
  config.capture_fps = 120;
  config.codec = "av1";
  config.buffer_seconds = 45;
  config.quality = "standard";
  config.peak_mode = "fixed";
  config.custom_kbps = 42000;
  config.hotkey_save = "Ctrl+Shift+F9";
  config.desktop_volume = 0.25;
  config.mic_gain_db = -6;
  config.separate_tracks = true;
  config.theme = "respawn";
  config.audio_tracks = 4;
  config.recordings_dir = "D:\\Recordings";
  config.encoder_options.set("preset", bx::Json("p1"));
  config.muxer_settings = "movflags=+faststart";

  bx::Config round_tripped;
  bx::config_from_json(bx::config_to_json(config), round_tripped);
  check(round_tripped.capture_fps == 120, "capture fps survives a round trip");
  check(round_tripped.codec == "av1", "codec survives");
  check(round_tripped.buffer_seconds == 45, "buffer length survives");
  check(round_tripped.quality == "standard" && round_tripped.peak_mode == "fixed" && round_tripped.custom_kbps == 42000,
        "the quality, a fixed peak and its rate survive");
  check(round_tripped.hotkey_save == "Ctrl+Shift+F9", "hotkey survives");
  check(round_tripped.desktop_volume == 0.25, "fractional volume survives");
  check(round_tripped.mic_gain_db == -6, "negative gain survives");
  check(round_tripped.separate_tracks, "boolean survives");
  check(round_tripped.theme == "respawn", "the chosen theme survives");
  check(round_tripped.audio_tracks == 4, "the audio track count survives");
  check(round_tripped.recordings_dir == "D:\\Recordings", "the recordings folder survives");
  check(round_tripped.encoder_options["preset"].as_string() == "p1", "an explicit encoder option survives");
  check(round_tripped.muxer_settings == "movflags=+faststart", "custom muxer settings survive");

  // The pipe's config.set is the second writer of the audio fields. It parses the same names as a stored
  // file and holds them to the same floor and ceiling (config_apply_audio), so neither writer can drift.
  bx::Config patched;
  bx::Json audio = bx::Json::object();
  audio.set("desktop_volume", bx::Json(3.5));
  audio.set("mic_gain_db", bx::Json(99));
  audio.set("audio_tracks", bx::Json(9));
  audio.set("mic_muted", bx::Json(true));
  bx::config_apply_audio(audio, patched);
  check(patched.desktop_volume == 1.0 && patched.mic_gain_db == 20 && patched.audio_tracks == 6,
        "a patched audio value is held to the same range as a stored one");
  check(patched.mic_muted, "a patched audio switch arrives");
  check(patched.mic_volume == 1.0, "an audio field a patch leaves out keeps its value");

  // Version 3: the three keys that were saved, loaded and never read are gone from the file. A
  // dead setting on disk is worse than a dead setting in memory - it is a promise the product cannot
  // keep, and every future reader has to work out whether it means anything.
  const std::string stored = bx::config_to_json(config).dump();
  check(stored.find("\"rate_control\"") == std::string::npos, "the unread rate_control key is not written");
  check(stored.find("\"save_accuracy\"") == std::string::npos, "the unread save_accuracy key is not written");
  check(stored.find("\"audio_codec\"") == std::string::npos, "the unread audio_codec key is not written");
  // Version 4: one quality choice replaces the tier, the size control, the bitrate, the quality number
  // and the memory budget, so none of those is written any more.
  for (const char* gone : {"\"quality_tier\"", "\"bitrate_kbps\"", "\"cqp\"", "\"duration_first\"", "\"buffer_memory_mb\""}) {
    check(stored.find(gone) == std::string::npos, (std::string("the replaced key ") + gone + " is not written").c_str());
  }
  // And an older file carrying them still loads, dropping what it carried.
  bx::Config older;
  bx::Json old_json = bx::Json::object();
  old_json.set("rate_control", bx::Json("cbr"));
  old_json.set("save_accuracy", bx::Json("exact"));
  old_json.set("audio_codec", bx::Json("opus"));
  old_json.set("buffer_seconds", bx::Json(90));
  bx::config_from_json(old_json, older);
  check(older.buffer_seconds == 90, "an older file's real settings still load");
  bx::migrate_config(older);
  check(older.config_version == bx::Config::kConfigVersion, "an older file is marked current");

  // Version 3 -> 4: the tier becomes a quality level, and an explicit rate becomes Custom, so nothing
  // the user chose is lost. A rate under Constant quality was only a cap the ladder set, not a choice.
  {
    bx::Json v3 = bx::Json::object();
    v3.set("quality_tier", bx::Json("performance"));
    bx::Config from_tier;
    bx::config_from_json(v3, from_tier);
    check(from_tier.quality == "standard", "the Fast tier becomes Standard");
    v3.set("quality_tier", bx::Json("quality"));
    bx::config_from_json(v3, from_tier);
    check(from_tier.quality == "highest", "the Quality tier becomes Highest");
    v3.set("quality_tier", bx::Json("balanced"));
    v3.set("bitrate_kbps", bx::Json(60000));
    v3.set("duration_first", bx::Json(true));
    bx::Config from_rate;
    bx::config_from_json(v3, from_rate);
    check(from_rate.quality == "high" && from_rate.peak_mode == "fixed" && from_rate.custom_kbps == 60000,
          "an explicit 60 Mbps becomes a fixed 60 Mbps peak under High");
    v3.set("duration_first", bx::Json(false));
    bx::Config from_cq;
    bx::config_from_json(v3, from_cq);
    check(from_cq.quality == "high", "a rate under Constant quality was not a choice, so the tier stands");
    bx::Json v4 = bx::Json::object();
    v4.set("quality", bx::Json("low"));
    v4.set("quality_tier", bx::Json("quality"));
    bx::Config new_wins;
    bx::config_from_json(v4, new_wins);
    check(new_wins.quality == "low", "a file with the new key ignores the old one");
    v4.set("quality", bx::Json("ultra"));
    bx::Config unknown;
    bx::config_from_json(v4, unknown);
    check(unknown.quality == "standard", "an unknown level is not stored");
    // Version 4's Custom level was High with a rate of the user's own: it is now High with a fixed peak,
    // so a file, a profile or an older client that still says "custom" keeps what it chose.
    bx::Json custom = bx::Json::object();
    custom.set("quality", bx::Json("custom"));
    custom.set("custom_kbps", bx::Json(100000));
    bx::Config legacy;
    bx::config_from_json(custom, legacy);
    check(legacy.quality == "high" && legacy.peak_mode == "fixed" && legacy.custom_kbps == 100000,
          "the old Custom level becomes High with a fixed peak");
    custom.set("custom_kbps", bx::Json(0));
    bx::Config legacy_unset;
    bx::config_from_json(custom, legacy_unset);
    check(legacy_unset.quality == "high" && legacy_unset.peak_mode == "auto",
          "the old Custom level with no rate yet is High with the automatic peak");
    bx::Json mode = bx::Json::object();
    mode.set("peak_mode", bx::Json("constant"));
    bx::Config bad_mode;
    bx::config_from_json(mode, bad_mode);
    check(bad_mode.peak_mode == "auto", "an unknown peak mode is not stored");
    check(bx::Config{}.peak_mode == "auto", "the default peak is automatic");
    bx::Json wild = bx::Json::object();
    wild.set("buffer_seconds", bx::Json(99999));
    wild.set("capture_fps", bx::Json(0));
    wild.set("custom_kbps", bx::Json(5));
    bx::Config clamped;
    bx::config_from_json(wild, clamped);
    check(clamped.buffer_seconds == 1800 && clamped.capture_fps == 1 && clamped.custom_kbps == 1000,
          "lengths, frame rates and rates are kept inside what the engine can do");
    // Hand-edited values reach the same enums and ranges as the pages: an unknown name is not stored,
    // a short chunk is lifted to the writer's floor and a negative reserve would refuse every recording.
    bx::Json bad_names = bx::Json::object();
    bad_names.set("capture_mode", bx::Json("telepathy"));
    bad_names.set("codec", bx::Json("vp9"));
    bad_names.set("theme", bx::Json("neon"));
    bad_names.set("container", bx::Json("avi"));
    bad_names.set("chunk_seconds", bx::Json(0));
    bad_names.set("min_free_gb", bx::Json(-5));
    bx::Config bad;
    bx::config_from_json(bad_names, bad);
    check(bad.capture_mode == "display" && bad.codec == "h264" && bad.theme == "clutch" && bad.container == "mp4",
          "an unknown capture mode, codec, theme or container is not stored");
    check(bad.chunk_seconds == 5 && bad.min_free_gb == 0,
          "a chunk shorter than the writer's floor is lifted, and a negative free-space reserve is zero");
    // `sessions.prune` hands whatever is stored straight to the pruner, and the pruner reads zero as
    // unlimited: a negative budget would otherwise delete every recording but the newest.
    bx::Json budget = bx::Json::object();
    budget.set("recordings_budget_gb", bx::Json(-5));
    bx::Config starved;
    bx::config_from_json(budget, starved);
    check(starved.recordings_budget_gb == 0, "a negative recordings budget is zero, which means unlimited");
    budget.set("recordings_budget_gb", bx::Json(99999));
    bx::Config huge;
    bx::config_from_json(budget, huge);
    check(huge.recordings_budget_gb == 10000, "a recordings budget past the ceiling is clamped");
    // A volume is a linear gain the pages write as 0-100%, and the gain is their slider's range: a
    // hand-edited 1e9 would reach obs_source_set_volume as inf.
    bx::Json loud = bx::Json::object();
    loud.set("desktop_volume", bx::Json(1e9));
    loud.set("mic_volume", bx::Json(-3.0));
    loud.set("mic_gain_db", bx::Json(400));
    bx::Config volumes;
    bx::config_from_json(loud, volumes);
    check(volumes.desktop_volume == 1.0 && volumes.mic_volume == 0.0 && volumes.mic_gain_db == 20,
          "a volume and a gain are kept inside the range the pages offer");
    // A size no device can make fails every video variant in turn, and the engine then cannot start -
    // which is also the moment there is no interface left to fix the file with.
    bx::Json absurd_size = bx::Json::object();
    absurd_size.set("output_width", bx::Json(100000));
    absurd_size.set("output_height", bx::Json(1079));
    bx::Config sized;
    bx::config_from_json(absurd_size, sized);
    check(sized.output_width == 7680 && sized.output_height == 1078,
          "a canvas size past what a device can make is clamped, and an odd one is made even");
    bx::Json native_size = bx::Json::object();
    native_size.set("output_width", bx::Json(0));
    native_size.set("output_height", bx::Json(-4));
    bx::Config native;
    bx::config_from_json(native_size, native);
    check(native.output_width == 0 && native.output_height == 0, "zero still means the native size");
    bx::Json good_names = bx::Json::object();
    good_names.set("capture_mode", bx::Json("game"));
    good_names.set("codec", bx::Json("hevc"));
    good_names.set("theme", bx::Json("auto"));
    good_names.set("container", bx::Json("mkv"));
    good_names.set("chunk_seconds", bx::Json(30));
    good_names.set("min_free_gb", bx::Json(5));
    bx::Config good;
    bx::config_from_json(good_names, good);
    check(good.capture_mode == "game" && good.codec == "hevc" && good.theme == "auto" &&
              good.container == "mkv" && good.chunk_seconds == 30 && good.min_free_gb == 5,
          "known names and the older cockpit themes are kept as written");
    // The vendor a machine's encoder comes from. "x264" is an encoder name an older file carried; a name
    // no vendor answers used to be stored as is and silently fell back to the automatic encoder.
    bx::Json vendor;
    vendor.set("encoder", bx::Json("x264"));
    bx::Config legacy_vendor;
    bx::config_from_json(vendor, legacy_vendor);
    check(legacy_vendor.encoder == "software", "the encoder name x264 is read as the software vendor");
    vendor.set("encoder", bx::Json("intel"));
    bx::Config unknown_vendor;
    bx::config_from_json(vendor, unknown_vendor);
    check(unknown_vendor.encoder == "auto", "an encoder vendor the engine has no candidates for is not stored");
    vendor.set("encoder", bx::Json("qsv"));
    bx::Config known_vendor;
    bx::config_from_json(vendor, known_vendor);
    check(known_vendor.encoder == "qsv", "a vendor the engine knows is kept");
  }

  // The two switches Phase C adds, with the defaults the plan settled on.
  check(bx::Config{}.start_buffering, "the buffer starts unless the user turned it off");
  check(!bx::Config{}.run_as_admin, "run as admin is off until the user turns it on");

  // Recordings follow the clips folder unless a folder of their own is set.
  bx::Config paths;
  paths.clips_dir = "C:\\Clips";
  check(bx::recordings_base_dir(paths) == "C:\\Clips", "recordings fall back to the clips folder");
  paths.recordings_dir = "E:\\Rec";
  check(bx::recordings_base_dir(paths) == "E:\\Rec", "a recordings folder overrides the fallback");

  bx::Config defaults;
  check(defaults.chunk_seconds == 30, "chunk seconds default to thirty");
  check(defaults.recordings_budget_gb == 0, "recordings budget defaults to unlimited");
  check(defaults.min_free_gb == 2, "minimum free space defaults to two gigabytes");

  bx::Config profiled;
  profiled.chunk_seconds = 60;
  profiled.recordings_budget_gb = 40;
  profiled.min_free_gb = 5;
  bx::Config profiled_back;
  bx::config_from_json(bx::config_to_json(profiled), profiled_back);
  check(profiled_back.chunk_seconds == 60 && profiled_back.recordings_budget_gb == 40 && profiled_back.min_free_gb == 5,
        "chunk and retention numbers survive a round trip");
  profiled.video_preset = "low";
  bx::Config preset_back;
  bx::config_from_json(bx::config_to_json(profiled), preset_back);
  check(preset_back.video_preset == "low", "the Video preset survives a round trip");
  {
    bx::Json no_preset = bx::Json::object();
    no_preset.set("peak_mode", bx::Json("fixed"));
    no_preset.set("custom_kbps", bx::Json(30000));
    bx::Config derived;
    bx::config_from_json(no_preset, derived);
    check(derived.video_preset == "custom", "a file from before the presets with a fixed rate opens as Custom");
    no_preset.set("peak_mode", bx::Json("auto"));
    no_preset.set("quality", bx::Json("highest"));
    bx::config_from_json(no_preset, derived);
    check(derived.video_preset == "highest", "and one on an automatic rate keeps its level as the preset");
  }

  // A file written before the bindings and the adaptive ladder were removed still loads, and the keys
  // it carries are gone the next time the engine writes it.
  bx::Json legacy = bx::Json::object();
  legacy.set("auto_game_switch", bx::Json(true));
  legacy.set("game_poll_seconds", bx::Json(3));
  legacy.set("game_switch_min_seconds", bx::Json(20));
  legacy.set("share_auto_fit", bx::Json(true));
  legacy.set("adaptive_quality", bx::Json(true));
  legacy.set("adaptive_min_bitrate_percent", bx::Json(70));
  legacy.set("adaptive_rate_limit_capture", bx::Json(false));
  legacy.set("app_profiles", bx::Json::object());
  legacy.set("exe_profiles", bx::Json::object());
  bx::Config from_old;
  bx::config_from_json(legacy, from_old);
  bx::Json rewritten = bx::config_to_json(from_old);
  check(!rewritten.has("auto_game_switch") && !rewritten.has("game_poll_seconds") && !rewritten.has("share_auto_fit") &&
            !rewritten.has("adaptive_quality") && !rewritten.has("app_profiles") && !rewritten.has("exe_profiles"),
        "settings a later build stopped reading are dropped when the file is written");

  // The screenshot format is a choice: JPEG is kept, and anything else reads as PNG.
  bx::Json screenshot = bx::Json::object();
  screenshot.set("screenshot_format", bx::Json("jpg"));
  bx::Config jpeg;
  bx::config_from_json(screenshot, jpeg);
  check(jpeg.screenshot_format == "jpg" && bx::config_to_json(jpeg)["screenshot_format"].as_string() == "jpg",
        "the screenshot format round-trips");
  bx::Json odd = bx::Json::object();
  odd.set("screenshot_format", bx::Json("gif"));
  bx::Config odd_read;
  bx::config_from_json(odd, odd_read);
  check(odd_read.screenshot_format == "png", "an unknown screenshot format reads as PNG");

  const std::string profiles_path = temp_directory() + "\\config-profiles.json";
  std::string profiles_error;
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  check(bx::save_config(profiles_path, profiled, profiles_error), "profiled config saves to disk");
  bx::Config profiles_from_disk;
  check(bx::load_config(profiles_path, profiles_from_disk, profiles_error), "profiled config reloads from disk");
  check(profiles_from_disk.chunk_seconds == 60 && profiles_from_disk.recordings_budget_gb == 40,
        "chunk and retention numbers survive a save and reload");

  const std::string missing = temp_directory() + "\\does-not-exist.json";
  bx::Config loaded;
  std::string error;
  check(bx::load_config(missing, loaded, error), "a missing file is not an error");
  check(loaded.capture_fps == 60, "defaults stand when no file exists");

  const std::string path = temp_directory() + "\\config-round-trip.json";
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  check(bx::save_config(path, config, error), "config saves");
  bx::Config reloaded;
  check(bx::load_config(path, reloaded, error), "config reloads");
  check(reloaded.buffer_seconds == 45 && reloaded.codec == "av1", "saved values come back");
  check(GetFileAttributesA((path + ".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "the temporary file is gone");

  bx::Config invalid;
  std::string invalid_error;
  check(!bx::load_config(path, invalid, invalid_error) == false, "a valid file loads without error");
}

// The one table that says what a settings change rebuilds. Its omissions are what let a settings
// change cut a manual recording in two: ObsHost::apply rebuilt for a field the refusal never saw.
void test_config_change_bits() {
  std::printf("config change bits\n");
  bx::Config before;
  check(bx::config_change_bits(before, before) == 0, "the same config changes nothing");

  bx::Config after = before;
  after.keyframe_seconds = before.keyframe_seconds == 2 ? 4 : 2;
  check((bx::config_change_bits(before, after) & bx::kConfigRebuilds) != 0,
        "a keyframe change rebuilds the encoders");
  check((bx::config_change_bits(before, after) & bx::kConfigSpeedInput) != 0,
        "and forgets what the governor settled on");

  after = before;
  after.container = before.container == "mp4" ? "mkv" : "mp4";
  check((bx::config_change_bits(before, after) & bx::kConfigRebuilds) != 0,
        "a container change rebuilds the outputs");

  after = before;
  after.audio_tracks = before.audio_tracks == 1 ? 2 : 1;
  check((bx::config_change_bits(before, after) & bx::kConfigRebuilds) != 0,
        "an audio track count change rebuilds the audio encoders");

  after = before;
  after.audio_bitrate_kbps = before.audio_bitrate_kbps + 32;
  check((bx::config_change_bits(before, after) & bx::kConfigRebuilds) != 0,
        "an audio bitrate change rebuilds the audio encoders");

  after = before;
  after.notifications = !before.notifications;
  check(bx::config_change_bits(before, after) == 0, "a notification switch rebuilds nothing");
}

// The Timeline's retention window: off until the user picks one of the four lengths, and anything else -
// a hand-edited file, a version 5 file that predates the field - is off. Four windows, so the engine
// never has to guess what 45 minutes would mean.
void test_config_timeline() {
  std::printf("config timeline\n");
  check(bx::Config{}.timeline_minutes == 0, "the Timeline is off by default");
  check(bx::timeline_minutes_allowed(0) && bx::timeline_minutes_allowed(30) && bx::timeline_minutes_allowed(60) &&
            bx::timeline_minutes_allowed(120),
        "0, 30, 60 and 120 minutes are the windows");
  check(!bx::timeline_minutes_allowed(45) && !bx::timeline_minutes_allowed(-30) && !bx::timeline_minutes_allowed(240),
        "any other length is not a window");
  bx::Json v5 = bx::Json::object();
  v5.set("buffer_seconds", bx::Json(60));
  v5.set("config_version", bx::Json(5));
  bx::Config older;
  bx::config_from_json(v5, older);
  bx::migrate_config(older);
  check(older.timeline_minutes == 0, "a version 5 file predates the field and loads as off");
  check(bx::config_to_json(older)["config_version"].as_int(0) == bx::Config::kConfigVersion,
        "and it is saved as the current version");
  bx::Json edited = bx::Json::object();
  edited.set("timeline_minutes", bx::Json(45));
  bx::Config odd;
  bx::config_from_json(edited, odd);
  check(odd.timeline_minutes == 0, "a length that is not one of the four reads as off, not as the nearest window");
  bx::Config chosen;
  chosen.timeline_minutes = 60;
  bx::Config round;
  bx::config_from_json(bx::config_to_json(chosen), round);
  check(round.timeline_minutes == 60, "a chosen window survives a round trip");
  check(!bx::Config{}.timeline_everywhere && bx::Config{}.timeline_idle_minutes == 0, "the desktop is not recorded unless asked for");
  bx::Config everywhere;
  everywhere.timeline_everywhere = true;
  everywhere.timeline_idle_minutes = 15;
  bx::Config everywhere_round;
  bx::config_from_json(bx::config_to_json(everywhere), everywhere_round);
  check(everywhere_round.timeline_everywhere && everywhere_round.timeline_idle_minutes == 15, "everywhere and its idle time survive a round trip");
  bx::Json odd_idle = bx::Json::object();
  odd_idle.set("timeline_idle_minutes", bx::Json(7));
  bx::Config odd_idle_config;
  bx::config_from_json(odd_idle, odd_idle_config);
  check(odd_idle_config.timeline_idle_minutes == 0, "an idle time Settings does not offer leaves it off");
  check(bx::timeline_idle_minutes_allowed(0) && bx::timeline_idle_minutes_allowed(5) && bx::timeline_idle_minutes_allowed(15) &&
            bx::timeline_idle_minutes_allowed(30) && bx::timeline_idle_minutes_allowed(60),
        "0, 5, 15, 30 and 60 minutes are the idle windows");
  check(!bx::timeline_idle_minutes_allowed(1) && !bx::timeline_idle_minutes_allowed(7) && !bx::timeline_idle_minutes_allowed(61),
        "any other idle time is not a window");

  // "Delete old footage" was written to config.json and never read back: the choice was forgotten on the next start.
  check(bx::Config{}.timeline_keep_hours == 24, "stopped footage is kept a day by default");
  bx::Config kept;
  kept.timeline_keep_hours = 0;
  bx::Config kept_round;
  bx::config_from_json(bx::config_to_json(kept), kept_round);
  check(kept_round.timeline_keep_hours == 0, "Never survives a round trip");
  bx::Json odd_hours = bx::Json::object();
  odd_hours.set("timeline_keep_hours", bx::Json(5));
  bx::Config odd_kept;
  bx::config_from_json(odd_hours, odd_kept);
  check(odd_kept.timeline_keep_hours == 24, "a keep time Settings does not offer leaves the value as it was");
  check(!bx::Config{}.timeline_windowed_games, "windowed games are not detected unless asked for");
  bx::Config windowed;
  windowed.timeline_windowed_games = true;
  bx::Config windowed_round;
  bx::config_from_json(bx::config_to_json(windowed), windowed_round);
  check(windowed_round.timeline_windowed_games, "the windowed-games choice survives a round trip");

  bx::Config paths;
  bx::apply_path_defaults(paths, "C:\\tmp\\root");
  check(!paths.clips_dir.empty() && paths.screenshots_dir == paths.clips_dir + "\\Screenshots",
        "the default folders are filled and screenshots follow the clips folder");
  bx::Config preset_paths;
  preset_paths.clips_dir = "C:\\tmp\\clips";
  bx::apply_path_defaults(preset_paths, "C:\\tmp\\root");
  check(preset_paths.screenshots_dir == "C:\\tmp\\clips\\Screenshots", "a chosen clips folder keeps its screenshots beside it");

  char exe[MAX_PATH] = {0};
  check(GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0, "the test executable has a path");
  std::string exe_dir(exe);
  const size_t exe_slash = exe_dir.find_last_of("\\/");
  if (exe_slash != std::string::npos) exe_dir.resize(exe_slash);
  const std::string marker = exe_dir + "\\portable.flag";
  const std::string portable_root = exe_dir + "\\data";
  const bool data_existed = GetFileAttributesA(portable_root.c_str()) != INVALID_FILE_ATTRIBUTES;
  FILE* flag = std::fopen(marker.c_str(), "wb");
  check(flag != nullptr, "a portable marker can be written beside the test executable");
  if (flag) std::fclose(flag);
  std::string root;
  bool portable = false;
  check(bx::resolve_data_root(root, portable), "the data root resolves with a portable marker");
  check(portable && root.size() >= 5 && root.compare(root.size() - 5, 5, "\\data") == 0, "the marker selects <exe>\\data");
  std::remove(marker.c_str());
  if (!data_existed) RemoveDirectoryA(portable_root.c_str());
  root.clear();
  portable = true;
  check(bx::resolve_data_root(root, portable), "the data root resolves without a portable marker");
  check(!portable && root.size() >= 7 && root.compare(root.size() - 7, 7, "\\BuffrX") == 0, "without the marker it lives under LOCALAPPDATA");
}

void test_windowed_game() {
  std::printf("windowed game\n");
  check(bx::modules_look_like_game({"game.exe", "D3D11.dll", "XInput1_4.dll"}), "a graphics library with a controller library is a game");
  check(bx::modules_look_like_game({"vulkan-1.dll", "steam_api64.dll"}), "Vulkan with a platform client is a game");
  check(!bx::modules_look_like_game({"d3d11.dll", "dxgi.dll", "presentationcore.dll"}), "a program that only draws with Direct3D is not");
  check(!bx::modules_look_like_game({"xinput1_4.dll", "kernel32.dll"}), "a controller library without graphics is not");
  check(!bx::modules_look_like_game({}), "no module list is not a game");
}

void test_chords() {
  std::printf("hotkeys\n");
  UINT modifiers = 0;
  UINT key = 0;
  std::string problem;
  check(bx::parse_chord("Alt+C", modifiers, key, problem) && key == 'C' && (modifiers & MOD_ALT),
        "Alt+letter parses");
  check(bx::parse_chord("Ctrl+Shift+F10", modifiers, key, problem) && key == VK_F10 &&
            (modifiers & MOD_CONTROL) && (modifiers & MOD_SHIFT),
        "multi-modifier function key parses");
  check(!bx::parse_chord("C", modifiers, key, problem), "a lone letter is rejected");
  check(!bx::parse_chord("Ctrl+NotAKey", modifiers, key, problem), "an unknown key is rejected");
  check(!bx::parse_chord("Ctrl+", modifiers, key, problem), "a dangling modifier is rejected");
  check(bx::format_chord(MOD_CONTROL | MOD_ALT, 'Z') == "Ctrl+Alt+Z", "chords format back");
  check(bx::ui::chord_preview(true, false, false, false) == "Ctrl", "a held modifier shows itself");
  check(bx::ui::chord_preview(true, true, false, false) == "Ctrl+Alt", "two modifiers show in order");
  check(bx::ui::chord_preview(false, false, false, false).empty(), "nothing held shows nothing");
}

void test_gamepad_chords() {
  std::printf("controller hotkeys\n");
  std::string problem;
  check(bx::parse_pad_chord("Pad:A", problem) == bx::pad_a, "one controller button parses");
  check(bx::parse_pad_chord("pad:a+b", problem) == (bx::pad_a | bx::pad_b),
        "lower case and the prefix's case do not matter");
  check(bx::parse_pad_chord(" Pad: LT + rt ", problem) == (bx::pad_lt | bx::pad_rt),
        "spaces around the parts are ignored");
  check(bx::parse_pad_chord("Pad:B+A", problem) == (bx::pad_a | bx::pad_b),
        "the order of the buttons does not change the combination");
  check(bx::parse_pad_chord("", problem) == std::nullopt && problem.empty(),
        "an empty chord is an unbound binding, not an error");
  check(bx::parse_pad_chord("Pad:", problem) == std::nullopt && !problem.empty(),
        "a prefix with no button is refused");
  check(bx::parse_pad_chord("Pad:A+", problem) == std::nullopt && !problem.empty(),
        "a dangling plus is refused");
  check(bx::parse_pad_chord("Pad:A+A", problem) == std::nullopt && !problem.empty(),
        "the same button twice is refused");
  check(bx::parse_pad_chord("Pad:Nope", problem) == std::nullopt && !problem.empty(),
        "an unknown button is refused");
  check(bx::parse_pad_chord("A", problem) == std::nullopt && !problem.empty(),
        "a chord without the prefix is refused");

  check(bx::format_pad_chord(bx::pad_a | bx::pad_lt) == "Pad:A+LT",
        "a combination formats in the canonical order");
  const std::optional<uint32_t> parsed = bx::parse_pad_chord("Pad:Menu+Down+Y", problem);
  check(parsed && bx::format_pad_chord(*parsed) == "Pad:Y+MENU+DOWN", "formatting round-trips");
  check(bx::parse_pad_chord("Pad:LB+RB+VIEW+MENU", problem) == (bx::pad_lb | bx::pad_rb | bx::pad_view | bx::pad_menu),
        "four buttons is the longest combination");
  check(bx::parse_pad_chord("Pad:A+B+X+Y+LB", problem) == std::nullopt && !problem.empty(),
        "a fifth button is refused with a reason");

  check(bx::pad_chord_held(bx::pad_a | bx::pad_b, bx::pad_a | bx::pad_b | bx::pad_x),
        "a combination is held when every button is held");
  check(!bx::pad_chord_held(bx::pad_a | bx::pad_b, bx::pad_a), "a part of a combination is not it");
  check(bx::pad_chords_conflict(bx::pad_a, bx::pad_a | bx::pad_b), "a contained combination clashes");
  check(bx::pad_chords_conflict(bx::pad_a, bx::pad_a), "the same combination clashes");
  check(!bx::pad_chords_conflict(bx::pad_a | bx::pad_b, bx::pad_a | bx::pad_x),
        "combinations that only share a button are allowed");

  bx::Config pad_config;
  pad_config.hotkey_save_pad = "Pad:A+B";
  bx::Config pad_round;
  bx::config_from_json(bx::config_to_json(pad_config), pad_round);
  check(pad_round.hotkey_save_pad == "Pad:A+B", "a controller binding survives a config round trip");
  check(pad_round.hotkey_quick_pad.empty(), "a config without a controller chord loads it unbound");

  bx::Config shipped;
  const std::optional<uint32_t> save = bx::parse_pad_chord(shipped.hotkey_save_pad, problem);
  const std::optional<uint32_t> shot = bx::parse_pad_chord(shipped.hotkey_screenshot_pad, problem);
  const std::optional<uint32_t> record = bx::parse_pad_chord(shipped.hotkey_record_pad, problem);
  const std::optional<uint32_t> panel = bx::parse_pad_chord(shipped.hotkey_toggle_ui_pad, problem);
  check(save && *save == (bx::pad_lb | bx::pad_rb), "the shipped save chord is both bumpers");
  check(shot && *shot == (bx::pad_ls | bx::pad_rs), "the shipped screenshot chord is both stick clicks");
  check(record && *record == (bx::pad_view | bx::pad_menu), "the shipped record chord is view plus menu");
  check(panel && *panel == (bx::pad_y | bx::pad_view), "the shipped panel chord is Y plus view");
  check(save && shot && record && panel && !bx::pad_chords_conflict(*save, *shot) && !bx::pad_chords_conflict(*save, *panel) &&
            !bx::pad_chords_conflict(*shot, *panel) && !bx::pad_chords_conflict(*record, *save) &&
            !bx::pad_chords_conflict(*record, *shot) && !bx::pad_chords_conflict(*record, *panel),
        "no shipped controller chord overlaps another");
}

void test_pad_nav() {
  std::printf("controller navigation\n");
  using bx::ui::PadNav;
  check(bx::ui::pad_nav_for(bx::pad_up) == PadNav::up, "the up button moves up");
  check(bx::ui::pad_nav_for(bx::pad_down) == PadNav::down, "the down button moves down");
  check(bx::ui::pad_nav_for(bx::pad_left) == PadNav::left, "the left button moves left");
  check(bx::ui::pad_nav_for(bx::pad_right) == PadNav::right, "the right button moves right");
  check(bx::ui::pad_nav_for(bx::pad_a) == PadNav::accept, "A accepts, the way Enter does");
  check(bx::ui::pad_nav_for(bx::pad_b) == PadNav::back, "B goes back, the way Escape does");
  check(bx::ui::pad_nav_for(bx::pad_x) == PadNav::space, "X is space");
  check(bx::ui::pad_nav_for(bx::pad_lb) == PadNav::previous_pane, "LB walks back a pane");
  check(bx::ui::pad_nav_for(bx::pad_rb) == PadNav::next_pane, "RB walks on a pane");
  check(bx::ui::pad_nav_for(bx::pad_view) == PadNav::help, "View opens the keys sheet");
  check(bx::ui::pad_nav_for(bx::pad_y) == PadNav::none && bx::ui::pad_nav_for(bx::pad_menu) == PadNav::none &&
            bx::ui::pad_nav_for(bx::pad_lt) == PadNav::none,
        "buttons with no place in the interface do nothing");

  check(!bx::ui::pad_nav_repeat_due(1119, 1000, 1000), "a held direction waits before it repeats");
  check(bx::ui::pad_nav_repeat_due(1120, 1000, 1000), "the first repeat comes after 120 ms");
  check(!bx::ui::pad_nav_repeat_due(1159, 1000, 1120), "then it holds off for 40 ms");
  check(bx::ui::pad_nav_repeat_due(1160, 1000, 1120), "then it repeats");
}

void test_gamepad_fire() {
  std::printf("controller fire policy\n");
  bx::PadRouter router;
  const std::vector<uint32_t> combos = {bx::pad_a, bx::pad_a | bx::pad_b};

  check(router.update(0, bx::pad_a, combos, false, 1000).size() == 1,
        "a single button fires on the press");
  check(router.update(0, bx::pad_a, combos, false, 1032).empty(), "holding it does not fire again");
  check(router.update(0, 0, combos, false, 1100).empty(), "a release fires nothing");
  check(router.update(0, bx::pad_a, combos, false, 1200).size() == 1, "the next press fires again");

  bx::PadRouter growing;
  check(growing.update(0, bx::pad_b, combos, false, 1000).empty(), "B alone does not complete A+B");
  const std::vector<bx::PadFire> completed = growing.update(0, bx::pad_a | bx::pad_b, combos, false, 1040);
  check(completed.size() == 2 && completed[1].combo == (bx::pad_a | bx::pad_b),
        "a button added to a hold completes a longer combination");

  bx::PadRouter captured;
  captured.update(0, bx::pad_a, combos, true, 1000);
  captured.update(0, bx::pad_a | bx::pad_b, combos, true, 1040);
  check(captured.captured() == (bx::pad_a | bx::pad_b), "capture accumulates the buttons held");
  captured.update(0, 0, combos, true, 1080);
  check(captured.captured() == 0, "letting go of everything starts the capture over");

  bx::PadRouter pads;
  check(pads.update(0, bx::pad_a, combos, false, 1000).size() == 1, "the first pad fires");
  check(pads.update(1, bx::pad_a, combos, false, 1100).empty(),
        "the second pad holding the same buttons does not fire again");
}

void test_gamepad_triggers() {
  std::printf("controller triggers\n");
  check(!(bx::pad_mask_from_xinput(0, 29, 0, 0) & bx::pad_lt), "a trigger under 30 is not pressed");
  check((bx::pad_mask_from_xinput(0, 30, 0, 0) & bx::pad_lt), "a trigger at 30 is pressed");
  check((bx::pad_mask_from_xinput(0, 20, 0, bx::pad_lt) & bx::pad_lt),
        "a held trigger stays held at 20");
  check(!(bx::pad_mask_from_xinput(0, 20, 0, 0) & bx::pad_lt), "a trigger at 20 is not pressed yet");
  check((bx::pad_mask_from_xinput(0x1000, 0, 0, 0) & bx::pad_a), "the A button maps to A");
  check((bx::pad_mask_from_xinput(0x0010, 0, 0, 0) & bx::pad_menu), "START maps to MENU");
  check((bx::pad_mask_from_xinput(0x0020, 0, 0, 0) & bx::pad_view), "BACK maps to VIEW");
}

void test_screenshot_game_title() {
  check(bx::screenshot_game_title("Marvel.exe", "Marvel Rivals", true, {}) == "Marvel Rivals",
        "a screenshot names the fullscreen game even with display capture");
  check(bx::screenshot_game_title("Marvel.exe", "", true, {}) == "Marvel.exe", "a titleless game uses its executable");
  check(bx::screenshot_game_title("Marvel.exe", "Marvel Rivals", false, {}).empty(), "a windowed app is not guessed to be a game");
  check(bx::screenshot_game_title("chrome.exe", "Browser", true, {}).empty(), "an excluded app is not named as a game");
  check(bx::screenshot_game_title("Marvel.exe", "Marvel Rivals", true, {"Marvel.exe"}).empty(),
        "the person's game exclusions apply to screenshots");
}

void test_library_guards() {
  std::printf("library\n");
  const std::string root = temp_directory();
  bx::LibraryPaths paths;
  paths.clips_dir = root + "\\clips";
  paths.screenshots_dir = root + "\\shots";
  paths.thumbnails_dir = root + "\\thumbs";
  bx::library_init(paths);

  check(bx::library_owns_path(paths.clips_dir + "\\clip.mkv"), "a clip inside the library is owned");
  check(bx::library_owns_path(paths.screenshots_dir + "\\shot.png"), "a screenshot is owned");
  check(!bx::library_owns_path("C:\\Windows\\System32\\drivers\\etc\\hosts"), "a system path is refused");
  check(!bx::library_owns_path(root + "\\..\\outside.mkv"), "a path outside the roots is refused");
  check(!bx::library_owns_path(paths.clips_dir + "\\..\\..\\outside.mkv"), "a traversal out of the clips folder is refused");
  check(!bx::library_owns_path(paths.clips_dir + "-old\\old.mkv"), "a sibling folder with the root as its prefix is refused");
  check(bx::library_owns_path(paths.clips_dir + "\\folder\\..\\clip.mkv"), "a traversal that stays inside the library is owned");
  check(!bx::library_owns_path(""), "an empty path is refused");

  const std::string first = bx::reserve_output_path(paths.clips_dir, "same name", ".mkv");
  FILE* file = std::fopen(first.c_str(), "wb");
  if (file) std::fclose(file);
  const std::string second = bx::reserve_output_path(paths.clips_dir, "same name", ".mkv");
  check(first != second, "a reserved name is never handed out twice");
  check(second.find("(1)") != std::string::npos, "the second reservation is numbered");
  // The stem may come from a client: a path in it would put the file outside the folder asked for.
  const std::string sneaky = bx::reserve_output_path(paths.clips_dir, "..\\..\\x", ".mkv");
  check(sneaky.find("..") == std::string::npos && bx::library_owns_path(sneaky),
        "a traversal in a reserved stem stays inside the folder");
  {
    // A name Windows would refuse, or one stripped to nothing, still gives a file a person can find.
    const auto leaf = [](const std::string& path) { return path.substr(path.find_last_of('\\') + 1); };
    check(leaf(bx::reserve_output_path(paths.clips_dir, "??", ".mp4")) == "Clip.mp4", "a name stripped to nothing becomes Clip");
    check(leaf(bx::reserve_output_path(paths.clips_dir, "  my clip. ", ".mp4")) == "my clip.mp4",
          "spaces and dots at the ends are dropped, as Windows would drop them");
    check(leaf(bx::reserve_output_path(paths.clips_dir, "con", ".mp4")) == "_con.mp4" &&
              leaf(bx::reserve_output_path(paths.clips_dir, "LPT1.final", ".mp4")) == "_LPT1.final.mp4",
          "a DOS device name is not a file name");
    check(leaf(bx::reserve_output_path(paths.clips_dir, "a\tb\nc", ".mp4")) == "abc.mp4", "control characters are dropped");
    check(leaf(bx::reserve_output_path(paths.clips_dir, "Console", ".mp4")) == "Console.mp4", "a name that only starts like a device is left alone");
  }

  check(!bx::library_delete("C:\\Windows\\notepad.exe"), "delete refuses a path outside the library");

  // The Recycle Bin path shares the delete guards; the unit harness checks the policy, never the bin itself.
  CreateDirectoryA(paths.clips_dir.c_str(), nullptr);
  const std::string removable = paths.clips_dir + "\\removable.mkv";
  if (FILE* clip = std::fopen(removable.c_str(), "wb")) std::fclose(clip);
  check(bx::library_can_remove(removable), "a file inside the library can be removed");
  check(!bx::library_can_remove(paths.clips_dir + "\\missing.mkv"), "a missing file cannot be removed");
  check(!bx::library_can_remove("C:\\Windows\\notepad.exe"), "removal refuses a path outside the library");
  check(!bx::library_can_remove(paths.clips_dir), "removal refuses a directory");

  // A reader that holds the clip for a moment (the engine reading its length after a save, a thumbnail
  // decode) must not make a rename fail: the acceptance harness hit "could not be renamed" this way.
  {
    CreateDirectoryA(paths.clips_dir.c_str(), nullptr);
    const std::string held = paths.clips_dir + "\\held clip.mkv";
    if (FILE* clip = std::fopen(held.c_str(), "wb")) std::fclose(clip);
    HANDLE reader = CreateFileA(held.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    check(reader != INVALID_HANDLE_VALUE, "a reader holds the clip without sharing delete");
    std::thread release([reader] {
      Sleep(300);
      CloseHandle(reader);
    });
    std::string renamed;
    check(bx::library_rename(held, "renamed while held", renamed), "a rename waits out a reader that lets go");
    release.join();
    check(bx::library_delete(renamed), "and the renamed clip deletes");
  }

  // The folder layout: <clips>\<Game>\, Display Capture\, Recordings\, Screenshots\, Edits\.
  check(bx::media_folder_name("Counter-Strike 2") == "Counter-Strike 2", "a plain game title is kept");
  check(bx::media_folder_name("Counter-Strike 2 - Direct3D 11") == "Counter-Strike 2", "a renderer suffix is dropped");
  check(bx::media_folder_name("Rewind | OpenChamber") == "Rewind OpenChamber",
        "characters Windows forbids are removed and the gap collapses");
  check(bx::media_folder_name("  VALORANT  ") == "VALORANT", "surrounding whitespace goes");
  check(bx::media_folder_name("Game.") == "Game", "a trailing dot is dropped");
  check(bx::media_folder_name("") == "Unknown", "an empty title files under Unknown");
  check(bx::media_folder_name("CON") == "Unknown", "a reserved device name files under Unknown");
  check(bx::media_folder_name("a:b*c?") == "abc", "colons and wildcards go");
  check(bx::media_folder_name(std::string(150, 'x')).size() == 100, "a long title is cut at 100");
  check(bx::timestamped_stem("Replay", 2026, 10, 9, 7, 5, 3) == "Replay 2026-10-09 07-05-03",
        "the stem is the prefix and a zero-padded local time");

  const std::string game_dir = paths.clips_dir + "\\Counter-Strike 2";
  CreateDirectoryA(game_dir.c_str(), nullptr);
  for (const std::string& path : {game_dir + "\\Replay 2026-10-19 00-00-01.mp4",
                                  paths.clips_dir + "\\legacy.mkv"}) {
    if (FILE* made = std::fopen(path.c_str(), "wb")) std::fclose(made);
  }
  const std::string recordings_dir = paths.clips_dir + "\\Recordings";
  CreateDirectoryA(recordings_dir.c_str(), nullptr);
  if (FILE* made = std::fopen((recordings_dir + "\\Recording 2026-10-19 00-00-02.mp4").c_str(), "wb")) std::fclose(made);
  const std::vector<bx::MediaItem> listed = bx::list_clips();
  bool game_clip = false, legacy_clip = false, recording_listed = false;
  for (const bx::MediaItem& item : listed) {
    if (item.name == "Replay 2026-10-19 00-00-01") game_clip = item.folder == "Counter-Strike 2";
    if (item.name == "legacy") legacy_clip = item.folder.empty();
    if (item.folder == "Recordings") recording_listed = true;
  }
  check(game_clip, "a clip in a game folder is listed with that folder");
  check(legacy_clip, "a clip at the root (older builds) is still listed");
  check(!recording_listed, "recordings are not listed as clips (list_recordings owns them)");
  CreateDirectoryA(paths.screenshots_dir.c_str(), nullptr);
  const std::string game_shots = paths.screenshots_dir + "\\Counter-Strike 2";
  CreateDirectoryA(game_shots.c_str(), nullptr);
  const std::string game_shot = game_shots + "\\game.png";
  const std::string legacy_shot = paths.screenshots_dir + "\\legacy.png";
  for (const std::string& path : {game_shot, legacy_shot}) {
    if (FILE* made = std::fopen(path.c_str(), "wb")) std::fclose(made);
  }
  bool game_shot_found = false, legacy_shot_found = false;
  for (const bx::MediaItem& item : bx::list_screenshots()) {
    if (item.file == game_shot) game_shot_found = item.folder == "Counter-Strike 2";
    if (item.file == legacy_shot) legacy_shot_found = item.folder.empty();
  }
  check(game_shot_found && legacy_shot_found, "screenshots list their game folder and keep older root files");
  const std::string shared_shots = paths.screenshots_dir + "\\Shared with me";
  CreateDirectoryA(shared_shots.c_str(), nullptr);
  const std::string shared_shot = shared_shots + "\\from a friend.png";
  DeleteFileA((shared_shots + "\\renamed friend.png").c_str());
  if (FILE* made = std::fopen(shared_shot.c_str(), "wb")) std::fclose(made);
  bool shared_shot_found = false;
  for (const bx::MediaItem& item : bx::list_screenshots()) {
    if (item.file == shared_shot) shared_shot_found = item.folder == "Shared with me";
  }
  check(shared_shot_found, "a received screenshot is listed with its shared folder");
  std::string renamed_shot;
  check(bx::library_rename(shared_shot, "renamed friend", renamed_shot), "a received screenshot can be renamed");
  shared_shot_found = false;
  for (const bx::MediaItem& item : bx::list_screenshots()) {
    if (item.file == renamed_shot) shared_shot_found = item.folder == "Shared with me";
  }
  check(shared_shot_found, "a renamed received screenshot keeps its shared provenance");
  // A name the file already has must answer the file, not call it taken: the Rename sheet reopens with the stem in its
  // field, so its second press sends that same name. A change of case alone is a rename and reaches the disk.
  std::string same_shot;
  check(bx::library_rename(renamed_shot, "renamed friend", same_shot) && same_shot == renamed_shot,
        "renaming to the name it already has answers the file instead of refusing it");
  std::string cased_shot;
  check(bx::library_rename(renamed_shot, "Renamed Friend", cased_shot), "a change of case alone is a rename");
  bool cased_listed = false;
  for (const bx::MediaItem& item : bx::list_screenshots()) {
    if (item.file == shared_shots + "\\Renamed Friend.png") cased_listed = true;
  }
  check(cased_listed, "the change of case reaches the disk");
  check(bx::library_owns_path(game_dir + "\\Replay 2026-10-19 00-00-01.mp4"), "a game folder is inside the library");
}

// The library's highest-consequence file rule: nothing that may still be written is opened. A refactor
// that dropped the writer check passes every other check here and brings "could not be renamed" back on
// every save, because FFmpeg holds the file for the thumbnail.
void test_annotate_skip() {
  std::printf("library skip rule\n");
  const std::string file = "C:\\clips\\Replay 2026-10-19 00-00-01.mkv";
  const uint64_t now = 1000000;
  check(bx::annotate_should_skip(file, now, true, false, false, now - 60000), "a held save is skipped");
  check(bx::annotate_should_skip(file, now, false, true, false, now - 60000), "a file a writer still holds is skipped");
  check(bx::annotate_should_skip(file, now, false, false, true, now - 1000), "a fresh clip at the root is skipped");
  check(!bx::annotate_should_skip(file, now, false, false, true, now - 60000), "an older clip at the root is listed");
  check(!bx::annotate_writer_probe_needed(now, now - 60000), "an old file is not opened to ask its writer");
  check(bx::annotate_writer_probe_needed(now, now - 59000), "a file written less than a minute ago is still asked");
  check(bx::annotate_writer_probe_needed(now, now - 1000), "a fresh file is still asked of its writer");
}

// Steady-state benchmark arithmetic (docs/measurement.md). The engine's CPU figure is only comparable
// to other tools when the initialisation window is discarded first, so the settle window and the
// window arithmetic are tested here rather than trusted to the loop in main().
void test_benchmark() {
  std::printf("benchmark\n");

  const bx::BenchmarkOptions defaults = bx::parse_benchmark_options({"buffrx-engine.exe"});
  check(defaults.seconds == 20 && defaults.settle_seconds == 5, "no flags keeps the documented defaults");

  const bx::BenchmarkOptions flagged =
      bx::parse_benchmark_options({"buffrx-engine.exe", "--benchmark-seconds", "300", "--benchmark-settle", "12"});
  check(flagged.seconds == 300 && flagged.settle_seconds == 12, "both flags are read");

  const bx::BenchmarkOptions too_short = bx::parse_benchmark_options({"--benchmark-seconds", "2"});
  check(too_short.seconds == 5 && too_short.settle_seconds == 0,
        "a run under five seconds is raised, and its settle window shrinks to zero");

  const bx::BenchmarkOptions greedy = bx::parse_benchmark_options({"--benchmark-seconds", "10", "--benchmark-settle", "60"});
  check(greedy.settle_seconds == 5, "the settle window never eats the whole run: at least five seconds are measured");

  const bx::BenchmarkOptions negative = bx::parse_benchmark_options({"--benchmark-settle", "-4"});
  check(negative.settle_seconds == 0, "a negative settle is clamped to zero");

  const bx::BenchmarkOptions garbage = bx::parse_benchmark_options({"--benchmark-seconds", "abc"});
  check(garbage.seconds == 20, "a non-numeric duration keeps the default");

  const bx::BenchmarkOptions dangling = bx::parse_benchmark_options({"--benchmark-seconds"});
  check(dangling.seconds == 20, "a flag with no value keeps the default");

  bx::BenchmarkSample start;
  start.cpu_seconds = 10.0;
  start.wall_seconds = 100.0;
  start.frames = 1000;
  start.dropped = 4;
  bx::BenchmarkSample end;
  end.cpu_seconds = 11.0;
  end.wall_seconds = 120.0;
  end.frames = 2200;
  end.dropped = 6;

  bx::VideoStats video;
  video.video_frames = 1210;
  video.video_lagged = 9;
  video.encoder_frames = 1205;
  video.render_time_ms = 1.42;

  const bx::BenchmarkResult result = bx::summarize_benchmark(start, end);
  check(result.wall_seconds == 20.0, "the measured window is end minus start");
  check(std::fabs(result.cpu_seconds - 1.0) < 1e-9, "the raw CPU seconds survive so anyone can recompute the ratio");
  check(result.frames == 1200 && result.dropped == 2, "frames and drops are window deltas, not totals");
  check(std::fabs(result.fps - 60.0) < 1e-9, "fps is frames over the measured window");

  const bx::BenchmarkResult empty = bx::summarize_benchmark(start, start);
  check(empty.cpu_seconds == 0.0 && empty.fps == 0.0, "a zero-length window reports zero, never divides by zero");

  bx::BenchmarkSample restarted = start;
  restarted.wall_seconds = 110.0;
  restarted.frames = 10;
  restarted.dropped = 0;
  const bx::BenchmarkResult after_restart = bx::summarize_benchmark(start, restarted);
  check(after_restart.frames == 0 && after_restart.dropped == 0,
        "a counter that went backwards reads as zero, never as a negative");

  // The published cost is a share of the whole machine, the way Windows' own task manager shows a
  // process: a figure per core is four times larger on this machine and comparable to nothing.
  check(std::fabs(bx::machine_cpu_percent(1.0, 10.0, 24) - 0.4166667) < 1e-6,
        "one CPU second over ten wall seconds on 24 logical cores is 0.42 percent of the machine");
  check(bx::machine_cpu_percent(1.0, 0.0, 24) == 0.0, "a zero-length window has no share of anything");
  check(bx::machine_cpu_percent(1.0, 10.0, 0) == 0.0, "an unknown core count reports zero, never a division by zero");
  check(bx::machine_cpu_percent(-1.0, 10.0, 24) == 0.0, "a negative CPU delta reads as zero");

  const std::string line = bx::format_benchmark_line(result, 512.5, "obs_nvenc_h264_tex", 5, 24, video);
  check(line == "BUFFRX_BENCHMARK seconds=20.0 cpu_seconds=1.00 cores=24 cpu_percent=0.21 working_set_mb=512.5 "
                 "frames=1200 dropped=2 fps=60.0 encoder=obs_nvenc_h264_tex video_frames=1210 video_lagged=9 "
                 "encoder_frames=1205 render_ms=1.42 settle=5",
        "the summary line keeps the shape docs/measurement.md documents");
}

// The canvas must be sized from the capture target before the first obs_reset_video: a second reset
// after the capture source exists hangs libobs, so the size has to be right the first time.
void test_canvas_plan() {
  std::printf("canvas plan\n");
  bx::Config config;

  const bx::CanvasPlan native = bx::plan_canvas(config, 2560, 1440);
  check(native.canvas_width == 2560 && native.canvas_height == 1440, "with no configured size the canvas takes the target's");
  check(native.output_width == 2560 && native.output_height == 1440, "and the output follows the canvas, so nothing is scaled");

  config.output_width = 1920;
  config.output_height = 1080;
  const bx::CanvasPlan scaled = bx::plan_canvas(config, 2560, 1440);
  check(scaled.canvas_width == 2560 && scaled.canvas_height == 1440, "a configured size leaves the canvas at the target");
  check(scaled.output_width == 1920 && scaled.output_height == 1080, "the configured size becomes the output, scaled once");

  config.output_width = 0;
  config.output_height = 0;
  const bx::CanvasPlan unknown = bx::plan_canvas(config, 0, 0);
  check(unknown.canvas_width == 1920 && unknown.canvas_height == 1080, "an unknown target falls back to 1080p");

  config.output_width = 1280;
  config.output_height = 720;
  const bx::CanvasPlan unknown_scaled = bx::plan_canvas(config, 0, 0);
  check(unknown_scaled.canvas_width == 1280 && unknown_scaled.canvas_height == 720,
        "an unknown target falls back to the configured size");

  config.output_width = 1920;
  config.output_height = 0;
  const bx::CanvasPlan half = bx::plan_canvas(config, 2560, 1440);
  check(half.output_width == 2560 && half.output_height == 1440, "half a size is not a size: the output follows the target");

  config.output_width = 0;
  config.output_height = 0;
  const bx::CanvasPlan broken = bx::plan_canvas(config, 2560, 0);
  check(broken.canvas_width == 1920 && broken.canvas_height == 1080, "half a target is not a target either");
}

// Which display a saved choice means, whether the live source has followed it, and when a switch costs a
// video rebuild. Getting any of these wrong records the wrong screen without saying so.
void test_display_target() {
  std::printf("display target\n");
  const std::vector<bx::DisplayInfo> list = {
      {0, "\\\\.\\DISPLAY1", "Primary display (2560x1440)", 2560, 1440, true},
      {1, "\\\\.\\DISPLAY2", "Display 2 (1920x1080)", 1920, 1080, false},
      {2, "\\\\.\\DISPLAY3", "Display 3 (1920x1080)", 1920, 1080, false},
  };
  // The name survives a rearrange; a stale index would pick a different screen.
  check(bx::pick_display(list, 0, "\\\\.\\DISPLAY3").device == "\\\\.\\DISPLAY3", "display: name wins over the index");
  check(bx::pick_display(list, 1, "").id == 1, "display: index picks when no name is saved");
  check(bx::pick_display(list, 2, "\\\\.\\DISPLAY9").id == 2, "display: a name that is gone falls back to the index");
  // Out of range: the primary keeps the buffer working, and the caller is told it was not the choice.
  const bx::DisplayPick missing = bx::pick_display(list, 7, "");
  check(missing.found && missing.primary_fallback && missing.id == 0, "display: an absent index captures the primary");
  check(!bx::pick_display({}, 0, "").found, "display: an empty list is not a pick");

  // What is saved: an explicit name is never rewritten, an empty one adopts the card's device, an
  // absent index stays empty rather than guessing another monitor.
  check(bx::resolved_display_name(list, 1, "\\\\.\\DISPLAY1") == "\\\\.\\DISPLAY1", "display: keep an explicit name");
  check(bx::resolved_display_name(list, 1, "") == "\\\\.\\DISPLAY2", "display: an empty name adopts the device");
  check(bx::resolved_display_name(list, 9, "").empty(), "display: an absent index stays empty");

  check(!bx::display_follow_needed("\\\\.\\DISPLAY1", "\\\\.\\DISPLAY1"), "display: a settled capture needs no follow");
  check(bx::display_follow_needed("\\\\.\\DISPLAY1", "\\\\.\\DISPLAY2"), "display: a moved display is followed");
  check(bx::display_follow_needed("", "\\\\.\\DISPLAY2"), "display: a first pick is followed");
  check(!bx::display_follow_needed("\\\\.\\DISPLAY1", ""), "display: no known display is not a follow");

  bx::Config before;
  before.capture_mode = "display";
  before.display_id = 1;
  before.display_name = "\\\\.\\DISPLAY2";
  bx::Config after = before;
  check(!bx::display_switch_needs_replan(before, after, 1920, 1080, 2560, 1440, false), "display: no switch, no rebuild");
  after.display_id = 0;
  after.display_name = "\\\\.\\DISPLAY1";
  check(bx::display_switch_needs_replan(before, after, 2560, 1440, 1920, 1080, false),
        "display: a switch to another size rebuilds the canvas");
  check(!bx::display_switch_needs_replan(before, after, 1920, 1080, 1920, 1080, false),
        "display: a switch to the same size keeps the buffer");
  check(!bx::display_switch_needs_replan(before, after, 2560, 1440, 1920, 1080, true),
        "display: performance mode owns the canvas");
  bx::Config game = before;
  game.capture_mode = "game";
  check(!bx::display_switch_needs_replan(game, after, 2560, 1440, 1920, 1080, false),
        "display: leaving a game does not replan");
}

// The integrity check decides whether the engine may tell the user a clip is saved, so it is tested
// against files built byte by byte here: a real Matroska header, a real MP4 box layout, and the
// half-written shapes that must not pass.
namespace {

std::string file_bytes(const std::vector<uint8_t>& bytes) {
  const std::string path = temp_directory() + "\\integrity.bin";
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file) {
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
  }
  return path;
}

void append(std::vector<uint8_t>& out, std::initializer_list<uint8_t> bytes) {
  out.insert(out.end(), bytes);
}

// Matroska sizes are variable-length integers with a length marker: one byte up to 127, two up to
// 16383, and so on.
std::vector<uint8_t> ebml_size(size_t value) {
  if (value < 0x7F) return {static_cast<uint8_t>(0x80 | value)};
  if (value < 0x3FFF) return {static_cast<uint8_t>(0x40 | (value >> 8)), static_cast<uint8_t>(value & 0xFF)};
  return {static_cast<uint8_t>(0x20 | (value >> 16)), static_cast<uint8_t>((value >> 8) & 0xFF),
          static_cast<uint8_t>(value & 0xFF)};
}

std::vector<uint8_t> ebml_element(std::initializer_list<uint8_t> id, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out;
  append(out, id);
  const std::vector<uint8_t> size = ebml_size(payload.size());
  out.insert(out.end(), size.begin(), size.end());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<uint8_t> concat(const std::vector<std::vector<uint8_t>>& parts) {
  std::vector<uint8_t> out;
  for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

// A minimal Matroska file: header, segment, one track of the given type, and optionally a cluster
// holding one block (keyframe when requested).
std::vector<uint8_t> matroska(int track_type, bool with_cluster, bool keyframe) {
  std::vector<uint8_t> header = ebml_element({0x1A, 0x45, 0xDF, 0xA3}, {0x42, 0x86, 0x81, 0x01});
  std::vector<uint8_t> track_entry = ebml_element({0xAE}, ebml_element({0x83}, {static_cast<uint8_t>(track_type)}));
  std::vector<uint8_t> tracks = ebml_element({0x16, 0x54, 0xAE, 0x6B}, track_entry);
  std::vector<uint8_t> segment_payload = tracks;
  if (with_cluster) {
    std::vector<uint8_t> block = {0x81, 0x00, 0x00, static_cast<uint8_t>(keyframe ? 0x80 : 0x00), 0xDE, 0xAD};
    std::vector<uint8_t> cluster = ebml_element({0x1F, 0x43, 0xB6, 0x75}, ebml_element({0xA3}, block));
    segment_payload = concat({segment_payload, cluster});
  }
  std::vector<uint8_t> segment = ebml_element({0x18, 0x53, 0x80, 0x67}, segment_payload);
  return concat({header, segment});
}

std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out;
  const uint32_t size = static_cast<uint32_t>(payload.size() + 8);
  out.push_back(static_cast<uint8_t>(size >> 24));
  out.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(size & 0xFF));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(type[i]));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// A minimal MP4: ftyp, a moov with one video track, and optionally media data.
std::vector<uint8_t> mp4(bool video, bool with_media) {
  std::vector<uint8_t> ftyp = box("ftyp", {0x69, 0x73, 0x6F, 0x6D, 0x00, 0x00, 0x02, 0x00});
  std::vector<uint8_t> hdlr = box("hdlr", {0, 0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(video ? 'v' : 's'), 'i', 'd', 'e'});
  std::vector<uint8_t> trak = box("trak", hdlr);
  std::vector<uint8_t> moov = box("moov", trak);
  std::vector<uint8_t> parts = concat({ftyp, moov});
  if (with_media) {
    std::vector<uint8_t> mdat = box("mdat", {0x00, 0x00, 0x00, 0x02, 0x65, 0x88});
    parts = concat({parts, mdat});
  }
  return parts;
}

// ---- what an MP4 index says, read back the way a player reads it ----

uint32_t read_be32(const std::vector<uint8_t>& bytes, size_t at) {
  return (uint32_t(bytes[at]) << 24) | (uint32_t(bytes[at + 1]) << 16) | (uint32_t(bytes[at + 2]) << 8) |
         uint32_t(bytes[at + 3]);
}

uint32_t box_type(const char* name) {
  return (uint32_t(static_cast<uint8_t>(name[0])) << 24) | (uint32_t(static_cast<uint8_t>(name[1])) << 16) |
         (uint32_t(static_cast<uint8_t>(name[2])) << 8) | uint32_t(static_cast<uint8_t>(name[3]));
}

// A box the tests read: where its payload starts and where its own bytes end.
struct TestBox {
  size_t payload = 0;
  size_t end = 0;
};

TestBox find_test_box(const std::vector<uint8_t>& bytes, size_t from, size_t to, const char* type) {
  const uint32_t wanted = box_type(type);
  size_t at = from;
  while (at + 8 <= to) {
    const uint32_t size = read_be32(bytes, at);
    if (size < 8 || at + size > to) break;
    if (read_be32(bytes, at + 4) == wanted) return {at + 8, at + size};
    at += size;
  }
  return {};
}

// The first track carrying a `vide` or `soun` handler, found the way a player finds it.
TestBox test_track(const std::vector<uint8_t>& bytes, const char* handler) {
  const size_t end = bytes.size();
  size_t at = 8;
  while (at + 8 <= end) {
    const uint32_t size = read_be32(bytes, at);
    if (size < 8 || at + size > end) break;
    if (read_be32(bytes, at + 4) == box_type("trak")) {
      const TestBox mdia = find_test_box(bytes, at + 8, at + size, "mdia");
      const TestBox hdlr = mdia.payload ? find_test_box(bytes, mdia.payload, mdia.end, "hdlr") : TestBox{};
      if (hdlr.payload && read_be32(bytes, hdlr.payload + 8) == box_type(handler))
        return {at + 8, at + size};
    }
    at += size;
  }
  return {};
}

TestBox test_edit_list(const std::vector<uint8_t>& bytes, bool video) {
  const TestBox trak = test_track(bytes, video ? "vide" : "soun");
  const TestBox edts = trak.payload ? find_test_box(bytes, trak.payload, trak.end, "edts") : TestBox{};
  return edts.payload ? find_test_box(bytes, edts.payload, edts.end, "elst") : TestBox{};
}

int64_t moov_movie_duration(const std::vector<uint8_t>& moov) {
  const TestBox mvhd = find_test_box(moov, 8, moov.size(), "mvhd");
  return mvhd.payload ? int64_t(read_be32(moov, mvhd.payload + 16)) : 0;
}

int64_t moov_track_duration(const std::vector<uint8_t>& moov, bool video) {
  const TestBox trak = test_track(moov, video ? "vide" : "soun");
  const TestBox tkhd = trak.payload ? find_test_box(moov, trak.payload, trak.end, "tkhd") : TestBox{};
  return tkhd.payload ? int64_t(read_be32(moov, tkhd.payload + 20)) : 0;
}

int64_t moov_edit_count(const std::vector<uint8_t>& moov, bool video) {
  const TestBox elst = test_edit_list(moov, video);
  return elst.payload ? int64_t(read_be32(moov, elst.payload + 4)) : 0;
}

int64_t moov_edit_media_time(const std::vector<uint8_t>& moov, bool video) {
  const TestBox elst = test_edit_list(moov, video);
  return elst.payload ? int64_t(read_be32(moov, elst.payload + 12)) : 0;
}

int64_t file_size(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return -1;
  std::fseek(file, 0, SEEK_END);
  const int64_t size = std::ftell(file);
  std::fclose(file);
  return size;
}

// A moov the way our muxer writes one: a movie header at 1/1000 s, a 90 kHz picture track of 61 s
// and a 48 kHz sound track, with no edit list yet.
std::vector<uint8_t> trim_fixture_moov(bool sound_edit_list) {
  const std::vector<uint8_t> mvhd =
      box("mvhd", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x03, 0xE8, 0x00, 0x00, 0xEE, 0x48});
  const std::vector<uint8_t> tkhd = box("tkhd", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
                                                 0, 0, 0, 0, 0x00, 0x00, 0xEE, 0x48});
  const auto mdhd = [](uint8_t a, uint8_t b, uint8_t c, uint8_t d, std::vector<uint8_t> duration) {
    std::vector<uint8_t> payload = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, a, b, c, d};
    payload.insert(payload.end(), duration.begin(), duration.end());
    append(payload, {0x55, 0xC4, 0x00, 0x00});
    return box("mdhd", payload);
  };
  const auto trak = [&](const char* handler, const std::vector<uint8_t>& media, bool edit_list) {
    const std::vector<uint8_t> hdlr = box("hdlr", {0, 0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(handler[0]),
                                                  static_cast<uint8_t>(handler[1]), static_cast<uint8_t>(handler[2]),
                                                  static_cast<uint8_t>(handler[3])});
    std::vector<uint8_t> body = concat({tkhd, box("mdia", concat({media, hdlr}))});
    if (edit_list) {
      const std::vector<uint8_t> elst =
          box("elst", {0, 0, 0, 0, 0, 0, 0, 1, 0x00, 0x00, 0xEE, 0x48, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00});
      body = concat({body, box("edts", elst)});
    }
    return box("trak", body);
  };
  return box("moov", concat({mvhd, trak("vide", mdhd(0x00, 0x01, 0x5F, 0x90, {0x00, 0x53, 0xC5, 0x50}), false),
                             trak("soun", mdhd(0x00, 0x00, 0xBB, 0x80, {0x00, 0x2C, 0xAD, 0x80}), sound_edit_list)}));
}

}  // namespace

void test_mp4_edit() {
  std::printf("mp4 edit lists\n");

  const std::vector<uint8_t> fresh = bx::mp4_trim_moov(trim_fixture_moov(false), 1.5);
  check(!fresh.empty(), "a header with tracks can be trimmed");
  // The picture keeps 59.5 s of its media, starting 1.5 s in: 1.5 * 90000 ticks.
  check(moov_edit_media_time(fresh, /*video=*/true) == 135000, "the picture starts 1.5 s into the copy");
  check(moov_track_duration(fresh, /*video=*/true) == 59500, "the picture's own length loses the head");
  check(moov_movie_duration(fresh) == 59500, "the movie's length loses the head");

  check(bx::mp4_trim_moov(trim_fixture_moov(false), 0.0) == trim_fixture_moov(false), "no head, no change");
  check(bx::mp4_trim_moov(box("moov", {}), 1.5).empty(), "a header with no track is left alone");

  // A sound track already carries an edit list when the encoder primed the AAC: 1024 samples the
  // player must skip. The head adds to it, it does not replace it, or audio moves against the picture.
  const std::vector<uint8_t> primed = bx::mp4_trim_moov(trim_fixture_moov(true), 1.5);
  check(moov_edit_media_time(primed, /*video=*/false) == 1024 + 72000, "the head adds to the priming");
  check(moov_edit_count(primed, /*video=*/false) == 1, "the sound keeps its one edit");

  const std::string index_last =
      file_bytes(concat({box("ftyp", {0x69, 0x73, 0x6F, 0x6D}), box("free", {}), trim_fixture_moov(false)}));
  const bx::MoovBox last = bx::mp4_find_moov(index_last);
  check(last.found && last.last, "a file whose moov ends it can be patched in place");
  const std::string index_first =
      file_bytes(concat({box("ftyp", {0x69, 0x73, 0x6F, 0x6D}), trim_fixture_moov(false), box("mdat", {1, 2, 3})}));
  check(!bx::mp4_find_moov(index_first).last, "a file with media after its moov cannot be patched");

  const std::string patchable =
      file_bytes(concat({box("ftyp", {0x69, 0x73, 0x6F, 0x6D}), box("mdat", {1, 2, 3}), trim_fixture_moov(false)}));
  const int64_t before = file_size(patchable);
  std::string error;
  check(bx::mp4_head_trim(patchable, 1.5, error), "a file whose moov ends it is trimmed in place");
  const int64_t trimmed = file_size(patchable);
  check(trimmed > before, "the file grew by the edit list");
  check(bx::mp4_find_moov(patchable).last, "the rewritten file still ends with its index");
  check(bx::mp4_head_trim(patchable, 0.0, error) && file_size(patchable) == trimmed, "a zero head writes nothing");

  const std::string locked =
      file_bytes(concat({box("ftyp", {0x69, 0x73, 0x6F, 0x6D}), trim_fixture_moov(false), box("mdat", {1, 2, 3})}));
  check(!bx::mp4_head_trim(locked, 1.5, error) && error == "the index is not at the end of the file",
        "a file with media after its index is refused, not corrupted");
}

void test_clip_check() {
  std::printf("clip integrity\n");

  const bx::ClipCheck missing = bx::check_clip_file(temp_directory() + "\\not-there.mkv");
  check(!missing.ok, "a file that does not exist is not a clip");
  check(!missing.reason.empty(), "and the check says why");

  const bx::ClipCheck empty = bx::check_clip_file(file_bytes({}));
  check(!empty.ok, "an empty file is not a clip");

  const bx::ClipCheck garbage = bx::check_clip_file(file_bytes({0x00, 0x01, 0x02, 0x03, 0x04, 0x05}));
  check(!garbage.ok, "bytes that are not a container are not a clip");

  const bx::ClipCheck good = bx::check_clip_file(file_bytes(matroska(1, true, true)));
  check(good.ok, "a Matroska file with a video track and a keyframe is a clip");
  check(good.has_video_track && good.has_keyframe, "and the check names what it found");
  check(good.bytes == static_cast<int64_t>(matroska(1, true, true).size()), "and reports its exact size");

  const bx::ClipCheck header_only = bx::check_clip_file(file_bytes(matroska(1, false, false)));
  check(!header_only.ok, "a Matroska file with no frames in it is not a clip");

  const bx::ClipCheck audio_only = bx::check_clip_file(file_bytes(matroska(2, true, true)));
  check(!audio_only.ok, "a Matroska file with only an audio track is not a clip");

  const bx::ClipCheck not_keyframe = bx::check_clip_file(file_bytes(matroska(1, true, false)));
  check(!not_keyframe.ok, "a Matroska file whose first block is not a keyframe is not a clip");

  const bx::ClipCheck mp4_good = bx::check_clip_file(file_bytes(mp4(true, true)));
  check(mp4_good.ok && mp4_good.has_video_track && mp4_good.has_keyframe, "an MP4 with a video track and media data is a clip");

  const bx::ClipCheck mp4_no_media = bx::check_clip_file(file_bytes(mp4(true, false)));
  check(!mp4_no_media.ok, "an MP4 with no media data is not a clip");

  const bx::ClipCheck mp4_audio = bx::check_clip_file(file_bytes(mp4(false, true)));
  check(!mp4_audio.ok, "an MP4 with only an audio track is not a clip");
}

// A clip over 2 GiB must be sized and walked with 64-bit offsets: `long` is 32 bits on Windows, and a size that came back -1 made the
// engine call a good file empty (which deleted received link clips and Timeline saves).
void test_clip_check_large() {
  const std::string path = temp_directory() + "\\large-clip.mp4";
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::vector<uint8_t> head = concat({box("ftyp", {0x69, 0x73, 0x6F, 0x6D, 0x00, 0x00, 0x02, 0x00}),
                                            box("moov", box("trak", box("hdlr", {0, 0, 0, 0, 0, 0, 0, 0, 'v', 'i', 'd', 'e'})))});
  const uint64_t media = 3ull * 1024 * 1024 * 1024;  // 3 GiB of media the file is only given the length of, never written
  std::vector<uint8_t> mdat = {0, 0, 0, 1, 'm', 'd', 'a', 't'};  // size 1: a 64-bit size follows
  for (int shift = 56; shift >= 0; shift -= 8) mdat.push_back(static_cast<uint8_t>((media >> shift) & 0xFF));
  FILE* file = std::fopen(path.c_str(), "wb");
  bool made = file != nullptr;
  if (made) {
    const int64_t total = static_cast<int64_t>(head.size() + mdat.size() + media);
    made = std::fwrite(head.data(), 1, head.size(), file) == head.size() && std::fwrite(mdat.data(), 1, mdat.size(), file) == mdat.size() &&
           _chsize_s(_fileno(file), static_cast<__int64>(total)) == 0;
    std::fclose(file);
  }
  if (made) {
    const bx::ClipCheck large = bx::check_clip_file(path);
    check(large.bytes == static_cast<int64_t>(head.size() + mdat.size() + media), "a clip over 2 GiB reports its real size, not -1");
    check(large.ok && large.has_video_track && large.has_keyframe, "and it is still read as a clip: boxes past 2 GiB are seeked to, not cast to 32 bits");
  } else {
    std::printf("  [skip] the file system would not give a file a length over 2 GiB\n");
  }
  DeleteFileA(path.c_str());
}

// The two pieces of media arithmetic: the thumbnail box and the part of a file a trim copies. Both are
// decided before FFmpeg is asked to do anything, so both are testable without a file.
void test_media_math() {
  std::printf("media arithmetic\n");

  int width = 0;
  int height = 0;
  bx::thumbnail_size(2560, 1440, 320, width, height);
  check(width == 320 && height == 180, "a 16:9 source gives a 16:9 thumbnail");

  bx::thumbnail_size(1080, 1920, 320, width, height);
  check(width == 180 && height == 320, "a portrait source keeps its shape");

  bx::thumbnail_size(2560, 1440, 4000, width, height);
  check(width == 2560 && height == 1440, "a thumbnail is never upscaled past the source");

  bx::thumbnail_size(1919, 1081, 320, width, height);
  check(width % 2 == 0 && height % 2 == 0, "odd sources still give even dimensions");

  bx::thumbnail_size(0, 0, 320, width, height);
  check(width == 0 && height == 0, "a source with no size gives no thumbnail");

  double start = 0.0;
  double length = 0.0;
  check(bx::trim_range(30.0, 5.0, 10.0, start, length), "a range inside the file is copied as asked");
  check(start == 5.0 && length == 10.0, "and its bounds are unchanged");

  check(bx::trim_range(30.0, 25.0, 20.0, start, length), "a range that runs past the end is clamped");
  check(start == 25.0 && length == 5.0, "to what is left of the file");

  check(bx::trim_range(30.0, -5.0, 10.0, start, length), "a negative start is clamped");
  check(start == 0.0 && length == 10.0, "to the beginning of the file");

  check(!bx::trim_range(30.0, 31.0, 5.0, start, length), "a start past the end copies nothing");
  check(!bx::trim_range(30.0, 5.0, 0.0, start, length), "a zero length copies nothing");
  check(!bx::trim_range(0.0, 0.0, 5.0, start, length), "a file with no duration copies nothing");
  check(!bx::trim_range(30.0, 5.0, -2.0, start, length), "a negative length copies nothing");

  // The save tail (ISSUES #21): a 20 s quick clip out of a 61 s ring keeps the last 20 s.
  double tail = -1.0;
  check(bx::save_tail_start(61.0, 20.0, 1.0, tail) && tail == 41.0, "a short request keeps the ring's last seconds");
  check(!bx::save_tail_start(20.5, 20.0, 1.0, tail), "a file within the slack of the request is kept whole");
  check(!bx::save_tail_start(12.0, 20.0, 1.0, tail), "a ring shorter than the request is kept whole");
  check(!bx::save_tail_start(61.0, 0.0, 1.0, tail), "no requested length keeps the whole file");
  check(!bx::save_tail_start(0.0, 20.0, 1.0, tail), "a file with no duration is kept whole");
  // The regression this guards: the ring holds 61.4 s for a 60 s buffer, so every full-length save was
  // re-read and rewritten to take 1.4 s off its front, while a game was running.
  check(bx::save_tail_start(61.4, 60.0, bx::save_trim_slack(false), tail), "a short request is still cut");
  check(!bx::save_tail_start(61.4, 60.0, bx::save_trim_slack(true), tail), "a save of the whole buffer is never rewritten");
  check(bx::save_trim_slack(true) > 600.0 && bx::save_trim_slack(false) == 1.0, "the slack is huge for the whole buffer, one second otherwise");

  // Editor exports: a cut is a copy, a volume change touches only the audio, anything visual re-encodes.
  bx::ExportJob job;
  job.start = 2.0;
  job.end = 8.0;
  check(bx::export_plan(job) == bx::ExportPlan::copy, "a plain cut is a stream copy");
  job.volumes = {1.0, 1.0};
  check(bx::export_plan(job) == bx::ExportPlan::copy, "unchanged volumes are still a copy");
  job.volumes = {1.0, 0.5};
  check(bx::export_plan(job) == bx::ExportPlan::audio_only, "a volume change re-encodes the audio only");
  job.crop_width = 1920;
  job.crop_height = 1080;
  check(bx::export_plan(job) == bx::ExportPlan::reencode, "a crop re-encodes the video");
  bx::ExportJob compress;
  compress.quality = 0.65;
  check(bx::export_plan(compress) == bx::ExportPlan::reencode, "a compress re-encodes the video");
  bx::ExportJob scaled;
  scaled.out_width = 1280;
  scaled.out_height = 720;
  check(bx::export_plan(scaled) == bx::ExportPlan::reencode, "a scale re-encodes the video");
  // Colour: the defaults are the file untouched, and any other value has to re-encode to change a pixel.
  check(!bx::export_graded(job), "an ungraded job is the file's own colour");
  bx::ExportJob graded;
  graded.grade_saturation = 120.0;
  check(bx::export_graded(graded) && bx::export_plan(graded) == bx::ExportPlan::reencode,
        "a grade re-encodes the video");
  graded.grade_saturation = 100.4;
  check(!bx::export_graded(graded), "a slider's own wobble is not a grade");

  // Playback speed: a stream copy cannot retime, so the video re-encodes and the audio follows with
  // atempo stages. The rate is kept, which is why a faster clip is the smaller file.
  check(!bx::export_retimed(job) && bx::atempo_chain(1.0).empty(), "1x leaves the timing alone");
  bx::ExportJob retimed;
  retimed.speed = 2.0;
  check(bx::export_retimed(retimed) && bx::export_plan(retimed) == bx::ExportPlan::reencode,
        "a speed change re-encodes the video");
  check(bx::atempo_chain(2.0) == "atempo=2.000,", "2x is one atempo stage");
  check(bx::atempo_chain(4.0) == "atempo=2.000,atempo=2.000,", "4x is two, because one stage stops at 2");
  check(bx::atempo_chain(0.5) == "atempo=0.500,", "half speed slows the audio to match");
  check(bx::atempo_chain(0.1) == "atempo=0.500,atempo=0.500,", "a speed past the floor is clamped to a quarter");
  check(bx::atempo_chain(9.0) == "atempo=2.000,atempo=2.000,", "and past the ceiling to four");

  // A cut keeps parts of the clip: a split and a deleted part leave two, and they are joined in order.
  {
    bx::ExportJob cut;
    cut.segments = {{20.0, 30.0}, {0.0, 8.0}};
    const auto parts = bx::export_segments(cut, 35.0);
    check(parts.size() == 2 && parts[0].first == 0.0 && parts[1].first == 20.0, "the parts are put in order");
    check(bx::segments_length(parts) == 18.0, "the export is as long as the parts it keeps");
    check(bx::export_plan(cut) == bx::ExportPlan::reencode, "joining two parts re-encodes, so every cut is exact");
    cut.segments = {{0.0, 10.0}, {9.0, 12.0}, {12.02, 15.0}};
    const auto joined = bx::export_segments(cut, 35.0);
    check(joined.size() == 1 && joined[0].first == 0.0 && joined[0].second == 15.0,
          "overlapping and touching parts become one");
    cut.segments = {{-3.0, 2.0}, {30.0, 99.0}, {5.0, 5.01}};
    const auto clamped = bx::export_segments(cut, 35.0);
    check(clamped.size() == 2 && clamped[0].first == 0.0 && clamped[1].second == 35.0,
          "parts are clamped to the file and a sliver is dropped");
    cut.segments = {{4.0, std::nan("")}};
    check(bx::export_segments(cut, 35.0).empty(), "a part that is not a number is dropped, not exported");
    bx::ExportJob range;
    range.start = 2.0;
    range.end = 8.0;
    const auto single = bx::export_segments(range, 35.0);
    check(single.size() == 1 && single[0].first == 2.0 && single[0].second == 8.0, "no parts means start to end");
    range.end = 0.0;
    check(bx::export_segments(range, 35.0)[0].second == 35.0, "an end of 0 means the end of the file");
  }

  // A join: each clip keeps its own parts, the first clip decides the picture, and the total is the sum.
  {
    std::vector<bx::StitchSource> sources(2);
    sources[0].path = "a.mp4";
    sources[0].duration = 60.0;
    sources[0].width = 1921;
    sources[0].height = 1081;
    sources[0].fps = 60.0;
    sources[0].audio_tracks = 1;
    sources[0].segments = {{0.0, 25.0}};
    sources[1].path = "b.mp4";
    sources[1].duration = 60.0;
    sources[1].width = 1280;
    sources[1].height = 720;
    sources[1].fps = 30.0;
    sources[1].audio_tracks = 1;
    sources[1].segments = {{10.0, 20.0}};
    bx::StitchPlan plan;
    std::string why;
    check(bx::make_stitch_plan(sources, bx::ExportJob{}, plan, why), "two readable clips make a join");
    check(plan.width == 1920 && plan.height == 1080, "the first clip sets the size, and it is even");
    check(plan.fps == 60.0, "the first clip sets the rate");
    check(plan.has_audio && plan.total_seconds == 35.0, "the join is as long as the parts it keeps, with sound");
    check(plan.parts.size() == 2 && plan.parts[1][0].first == 10.0, "each clip keeps its own parts");
    bx::ExportJob thirty;
    thirty.fps = 30.0;
    check(bx::make_stitch_plan(sources, thirty, plan, why) && plan.fps == 30.0, "the job can choose the rate");
    sources[1].audio_tracks = 0;
    check(!bx::make_stitch_plan(sources, bx::ExportJob{}, plan, why),
          "a mixed set with sound and silence is refused");
    sources[1].audio_tracks = 1;
    sources[1].audio_tracks = 2;
    check(!bx::make_stitch_plan(sources, bx::ExportJob{}, plan, why) &&
              why.find("different audio tracks") != std::string::npos,
          "a clip with two tracks beside one with a single track is refused");
    sources[1].audio_tracks = 1;
    sources[1].segments.clear();
    sources[1].start = 60.0;
    sources[1].end = 61.0;
    check(!bx::make_stitch_plan(sources, bx::ExportJob{}, plan, why), "a clip with nothing left is refused");
    sources.pop_back();
    check(!bx::make_stitch_plan(sources, bx::ExportJob{}, plan, why), "one clip is not a join");
  }


  check(bx::stitch_tolerance(55.0) == 0.5, "a short join is accepted within half a second");
  check(bx::stitch_tolerance(400.0) == 2.0, "a long join is accepted within half a percent");
  check(bx::stitch_duration_ok(55.0, 55.4) && !bx::stitch_duration_ok(55.0, 55.6), "the window is enforced");

  // The job file: several inputs through one export, with the common target a join needs.
  {
    bx::Json spec;
    std::string parse_error;
    check(bx::Json::parse(
              R"({"output":"out.mp4","inputs":[{"input":"a.mp4","segments":[[0,25]]},{"input":"b.mp4","start":10,"end":20}],"fps":60,"audio_rate":48000,"audio_channels":2,"video_codec":"h264","fit_pad":true,"replace_original":true})",
              spec, parse_error),
          "a join job parses");
    bx::ExportJob joined;
    check(bx::export_job_from_json(spec, joined, parse_error), "a join job is accepted");
    check(joined.inputs.size() == 2 && joined.inputs[0].segments.size() == 1 &&
              joined.inputs[0].segments[0].second == 25.0,
          "each input keeps its own parts");
    check(joined.fps == 60.0 && joined.audio_rate == 48000 && joined.audio_channels == 2 &&
              joined.video_codec == "h264" && joined.fit_pad && joined.replace_original,
          "the common target and the safe replace are read");
    bx::Json missing;
    check(bx::Json::parse(R"({"output":"out.mp4"})", missing, parse_error) &&
              !bx::export_job_from_json(missing, joined, parse_error),
          "a job with no input is refused");
  }

  // Each clip of a join carries its own edit, and a retimed clip is as long on the join as its speed says.
  {
    bx::Json spec;
    std::string parse_error;
    check(bx::Json::parse(R"({"output":"out.mp4","inputs":[{"input":"a.mp4","segments":[[0,20]],"crop_x":10,"crop_y":20,"crop_width":640,"crop_height":360,"grade_brightness":110,"grade_contrast":90,"grade_saturation":120,"speed":2,"volumes":[1,0.5]},{"input":"b.mp4","segments":[[0,10]],"speed":0.5}]})",
              spec, parse_error),
          "a job with per-clip edits parses");
    bx::ExportJob joined;
    check(bx::export_job_from_json(spec, joined, parse_error), "a job with per-clip edits is accepted");
    check(joined.inputs.size() == 2 && joined.inputs[0].crop_width == 640 && joined.inputs[0].crop_x == 10 &&
              joined.inputs[0].grade_saturation == 120.0 && joined.inputs[0].speed == 2.0 &&
              joined.inputs[0].volumes.size() == 2 && joined.inputs[0].volumes[1] == 0.5,
          "the first clip keeps its crop, grade, speed and volumes");
    check(joined.inputs[1].crop_width == 0 && joined.inputs[1].grade_brightness == 100.0 &&
              joined.inputs[1].speed == 0.5 && joined.inputs[1].volumes.empty(),
          "a clip without an edit keeps the file as it is");
    std::vector<bx::StitchSource> sources(2);
    sources[0].duration = 60.0;
    sources[0].width = 1920;
    sources[0].height = 1080;
    sources[0].fps = 60.0;
    sources[0].audio_tracks = 1;
    sources[0].segments = {{0.0, 20.0}};
    sources[0].speed = 2.0;
    sources[1].duration = 60.0;
    sources[1].width = 1920;
    sources[1].height = 1080;
    sources[1].fps = 60.0;
    sources[1].audio_tracks = 1;
    sources[1].segments = {{0.0, 10.0}};
    sources[1].speed = 0.5;
    bx::StitchPlan plan;
    std::string why;
    check(bx::make_stitch_plan(sources, bx::ExportJob{}, plan, why) && plan.total_seconds == 30.0,
          "a retimed clip is as long on the join as its speed says");
    sources[0].speed = 9.0;
    check(!bx::make_stitch_plan(sources, bx::ExportJob{}, plan, why), "a speed that cannot be exported is refused");
    // The size chosen on the export screen is the join's own target, not the first clip's shape.
    sources[0].speed = 2.0;
    bx::ExportJob scaled;
    scaled.out_width = 1281;
    scaled.out_height = 721;
    check(bx::make_stitch_plan(sources, scaled, plan, why) && plan.width == 1280 && plan.height == 720,
          "a chosen output size is the join's target, snapped to even");
    check(bx::make_stitch_plan(sources, bx::ExportJob{}, plan, why) && plan.width == 1920 && plan.height == 1080,
          "with no size chosen the first clip sets the target");
  }

  int cx = 0, cy = 0, cw = 0, ch = 0;
  check(bx::export_crop(2560, 1440, 101, 51, 1001, 563, cx, cy, cw, ch) && cx == 100 && cy == 50 && cw == 1000 &&
            ch == 562,
        "a crop is snapped to even offsets and sizes");
  check(bx::export_crop(2560, 1440, 2000, 1000, 1000, 1000, cx, cy, cw, ch) && cx + cw <= 2560 && cy + ch <= 1440,
        "a crop running off the frame is clamped inside it");
  check(!bx::export_crop(2560, 1440, 0, 0, 0, 0, cx, cy, cw, ch) && cw == 2560 && ch == 1440,
        "no crop keeps the full frame");
  check(!bx::export_crop(2560, 1440, 3000, 0, 100, 100, cx, cy, cw, ch), "a crop outside the frame is refused");
  check(bx::export_crop(2560, 1440, 0, 0, 4, 4, cx, cy, cw, ch) && cw >= 16 && ch >= 16, "a crop is at least 16x16");

  check(bx::export_video_kbps(40000, 2560 * 1440, 2560 * 1440, 0.0) == 40000, "same size, same quality, same bitrate");
  check(bx::export_video_kbps(40000, 2560 * 1440, 1280 * 720, 0.0) == 10000, "a quarter of the pixels, a quarter of the bits");
  check(bx::export_video_kbps(40000, 2560 * 1440, 2560 * 1440, 0.4) == 16000, "compress low keeps 40%");
  check(bx::export_video_kbps(2000, 2560 * 1440, 640 * 360, 0.4) == 500, "never below 500 kbit/s");
  check(bx::export_video_kbps(0, 2560 * 1440, 2560 * 1440, 0.0) > 0, "an unknown source bitrate still gives a usable rate");

  bx::thumbnail_size(1080, 1080, 320, width, height);
  check(width == 320 && height == 320, "a square source fits the box");

  bx::thumbnail_size(100, 4000, 320, width, height);
  check(width == 8 && height == 320, "a very tall source is limited by its height");

  // The GPU's own decoder for the codecs a clip arrives in. A name the runtime has no decoder for
  // keeps the software path, which is why this returns an empty string rather than a guess.
  check(bx::hardware_decoder_name("av1") == "av1_cuvid", "an AV1 clip decodes on the graphics card");
  check(bx::hardware_decoder_name("hevc") == "hevc_cuvid", "an HEVC clip decodes on the graphics card");
  check(bx::hardware_decoder_name("h264") == "h264_cuvid", "an H.264 clip decodes on the graphics card");
  check(bx::hardware_decoder_name("mpeg4").empty(), "a codec with no GPU decoder keeps the software one");
  check(bx::hardware_decoder_name("").empty(), "no codec name means no GPU decoder");

  // A local save plays from the folder, so its index goes last and the file is written once. A share
  // fit is played by Discord and browsers before it has all arrived, so its index goes first.
  bx::ExportJob fast;
  check(!bx::export_wants_faststart(fast), "a local save leaves the index at the end");
  fast.video_kbps = 8000;
  check(bx::export_wants_faststart(fast), "a share fit writes the index first");

  // The copy starts on the keyframe at or before the cut, so the picture holds up to one keyframe
  // interval of footage the user cut away. That head is hidden, not shown, and an untouched save has none.
  check(bx::edit_head_seconds(0.0, 0.0, 1.0 / 120) == 0.0, "an untouched save has no head");
  check(bx::edit_head_seconds(12.0, 12.0, 1.0 / 120) == 0.0, "a cut on a keyframe has no head");
  check(bx::edit_head_seconds(12.0, 11.9995, 1.0 / 120) == 0.0, "a cut within half a frame of one has no head");
  check(std::fabs(bx::edit_head_seconds(12.0, 10.0, 1.0 / 120) - 2.0) < 1e-9, "a cut between keyframes hides the gap");
  check(std::isnan(bx::edit_head_seconds(30.0, 10.0, 1.0 / 120)), "a head longer than 10 s is re-encoded, not hidden");

  // An "Original" save used to decode AV1 and write H.264. The codec the clip arrived in is kept when
  // the machine can encode it, and the H.264 chain stays the floor.
  check(bx::video_encoder_candidates("av1", false).front() == "av1_nvenc", "an AV1 clip stays AV1");
  check(bx::video_encoder_candidates("hevc", false).front() == "hevc_nvenc", "an HEVC clip stays HEVC");
  check(bx::video_encoder_candidates("h264", false).front() == "h264_nvenc", "an H.264 clip keeps the H.264 chain");
  check(bx::video_encoder_candidates("av1", true).front() == "h264_nvenc", "a share fit is H.264, which Discord takes");
  check(bx::video_encoder_candidates("av1", false).back() == "h264_mf", "the software encoders are still the floor");
  check(bx::video_encoder_candidates("mpeg4", false).front() == "h264_nvenc", "a codec we cannot encode falls back to H.264");
  check(bx::video_encoder_candidates("", false).front() == "h264_nvenc", "a size setting writes H.264, not the source codec");
}

// The spec a `--media-join` child reads: the files to join by stream copy and the cut inside the first
// and the end inside the last of them.
void test_media_join_spec() {
  std::printf("media join spec\n");
  bx::MediaJoinRequest request;
  std::string error;
  check(!bx::parse_media_join_request(bx::Json::object(), request, error), "a spec with nothing in it is refused");
  check(error.find("files") != std::string::npos, "and the refusal names the files");

  bx::Json spec;
  std::string parse_error;
  check(bx::Json::parse(
            R"({"files":["a.mp4","b.mp4"],"output":"out.mp4","start_in_first":1.5,"end_in_last":9.0})", spec, parse_error),
        "a join spec parses");
  check(bx::parse_media_join_request(spec, request, error), "a join spec is accepted");
  check(request.files.size() == 2 && request.output == "out.mp4" && request.start_in_first_seconds == 1.5 &&
            request.end_in_last_seconds == 9.0,
        "the files, the output and both cuts are read");

  bx::Json no_output;
  check(bx::Json::parse(R"({"files":["a.mp4"]})", no_output, parse_error) &&
            !bx::parse_media_join_request(no_output, request, error) && error.find("output") != std::string::npos,
        "a spec with no output is refused");

  bx::Json not_array;
  check(bx::Json::parse(R"({"files":"a.mp4","output":"out.mp4","end_in_last":1})", not_array, parse_error) &&
            !bx::parse_media_join_request(not_array, request, error) && error.find("files") != std::string::npos,
        "files that are not an array are refused");

  bx::Json backwards;
  check(bx::Json::parse(R"({"files":["a.mp4"],"output":"out.mp4","start_in_first":5,"end_in_last":5})", backwards,
                        parse_error) &&
            !bx::parse_media_join_request(backwards, request, error) &&
            error.find("end_in_last") != std::string::npos,
        "an end that does not follow the start is refused in one file");

  // Each cut is an offset inside its own file: a range whose first chunk gives up more than its last one does
  // is an ordinary save, and comparing the two refused it (a 20 s Timeline save ending just inside its last
  // chunk reported "the join did not report a result").
  bx::Json across;
  check(bx::Json::parse(R"({"files":["a.mp4","b.mp4"],"output":"out.mp4","start_in_first":4.5,"end_in_last":0.5})",
                        across, parse_error) &&
            bx::parse_media_join_request(across, request, error) && request.files.size() == 2 &&
            request.start_in_first_seconds == 4.5 && request.end_in_last_seconds == 0.5,
        "a range across files keeps both cuts, even when the last is shorter than the first");

  bx::Json negative;
  check(bx::Json::parse(R"({"files":["a.mp4","b.mp4"],"output":"out.mp4","start_in_first":-1,"end_in_last":0.5})",
                        negative, parse_error) &&
            !bx::parse_media_join_request(negative, request, error),
        "a cut before the start of its file is refused");
}

// The parent half of a join (A2): the copy runs in a child process, so the engine's media lock stays free.
// A child that never finishes is stopped at its deadline, and a child that writes no result is a failed
// join; both leave no output behind. The child stands in here - a real join needs FFmpeg and a clip - and
// the deadline the test shortens is the same one the product uses.
void test_media_join_process() {
  std::printf("media join process\n");
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::string folder = temp_directory() + "\\join-process";
  CreateDirectoryA(folder.c_str(), nullptr);
  const std::string output = folder + "\\joined.mp4";
  const std::string result_path = bx::media_join_result_path(output);
  check(result_path == output + ".join.json", "a join reports beside the video it made");
  check(bx::media_join_result_path("C:/a/b/clip.mkv") == "C:/a/b/clip.mkv.join.json",
        "and the file ends with .join.json whatever the container");
  // The command line a real child gets. A wide string that still carried its terminating NUL ended the
  // child's command line at the first quote, and the child started as an ordinary engine instead of a join.
  check(bx::media_join_wide("C:/a b/exe") == L"C:/a b/exe" && bx::media_join_wide("C:/a b/exe").size() == 10,
        "a path becomes a wide string with no terminating null in it");
  const std::wstring command = bx::media_join_command(L"C:\\a b\\buffrx-engine.exe", L"C:\\c d\\join.json");
  check(command == L"\"C:\\a b\\buffrx-engine.exe\" --media-join \"C:\\c d\\join.json\"",
        "the join child's command line quotes the engine and the spec");
  check(command.find(L'\0') == std::wstring::npos, "and carries no null inside it, which would cut it short");
  DeleteFileA(output.c_str());

  bx::MediaJoinRequest request;
  request.files = {folder + "\\a.mp4", folder + "\\b.mp4"};
  request.output = output;
  request.end_in_last_seconds = 5.0;

  // A child that outlives its deadline: it waits on itself for a minute, so the deadline is what ends it.
  bx::media_join_child_override() = "cmd.exe /c ping -n 60 127.0.0.1 > nul";
  std::string error;
  bx::MediaJoinResult result;
  const auto started = std::chrono::steady_clock::now();
  check(!bx::media_join_process(request, result, error, 250), "a join that outlives its deadline fails");
  const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  check(!error.empty() && error.find("longer") != std::string::npos, "and says the join was stopped");
  check(!result.ok, "and reports no join");
  check(spent.count() < 30000, "and does not wait for the child's own minute");
  check(GetFileAttributesA(output.c_str()) == INVALID_FILE_ATTRIBUTES, "and leaves no output file");
  check(GetFileAttributesA((output + ".part").c_str()) == INVALID_FILE_ATTRIBUTES, "and no part file");
  check(GetFileAttributesA(result_path.c_str()) == INVALID_FILE_ATTRIBUTES, "and no result file");
  check(GetFileAttributesA((output + ".join-spec.json").c_str()) == INVALID_FILE_ATTRIBUTES, "and no spec file");

  // A child that exits without writing one: a spec it could not read leaves no result at all, which is a
  // failed join and never a quiet success.
  bx::media_join_child_override() = "cmd.exe /c exit 0";
  check(!bx::media_join_process(request, result, error, 30000), "a child that reports nothing fails the join");
  check(error.find("report") != std::string::npos, "and says the join reported no result");
  check(GetFileAttributesA((output + ".join-spec.json").c_str()) == INVALID_FILE_ATTRIBUTES,
        "and that attempt leaves no spec file either");

  // A stand-in child that does what a real one does: makes the video it was asked for and writes the
  // outcome beside it, with the keys the child itself writes (media.cpp) and the parent reads here.
  const std::string good = folder + "\\joined-ok.mp4";
  const std::string script = folder + "\\join-stand-in.cmd";
  DeleteFileA(good.c_str());
  if (FILE* file = std::fopen(script.c_str(), "wb")) {
    std::fprintf(file,
                 "@echo off\n"
                 "echo.>\"%s\"\n"
                 "echo {\"ok\":true,\"error\":\"\",\"first_keyframe\":1.25,\"dropped_head\":2}>\"%s\"\n",
                 good.c_str(), bx::media_join_result_path(good).c_str());
    std::fclose(file);
  }
  bx::MediaJoinRequest joined;
  joined.files = {folder + "\\a.mp4", folder + "\\b.mp4"};
  joined.output = good;
  joined.end_in_last_seconds = 5.0;
  bx::media_join_child_override() = "cmd.exe /c \"" + script + "\"";
  check(bx::media_join_process(joined, result, error, 30000), "a join that reports success is a success");
  check(result.ok && result.error.empty(), "with the outcome the child wrote");
  check(result.dropped_head == 2, "and the chunks a layout change left out");
  check(std::fabs(result.first_keyframe_seconds - 1.25) < 1e-9, "and the first keyframe the copy began on");
  check(GetFileAttributesA(good.c_str()) != INVALID_FILE_ATTRIBUTES, "and the video it made is kept");
  check(GetFileAttributesA(bx::media_join_result_path(good).c_str()) == INVALID_FILE_ATTRIBUTES,
        "while the result file is cleaned away");
  check(GetFileAttributesA((good + ".join-spec.json").c_str()) == INVALID_FILE_ATTRIBUTES, "and the spec with it");
  bx::media_join_child_override().clear();
  remove_tree(folder);
}

// The sharing helper is stopped after 30 s of nothing to do. The bug (2026-10-26): the clock kept its old value across a stop, so a
// helper started later was stopped by its first tick, and the next share or receive failed.
void test_link_idle() {
  std::printf("link idle\n");
  bx::LinkIdleClock clock;
  uint64_t now = 100000;
  check(!clock.should_stop(false, true, now), "an idle helper is not stopped at once");
  check(!clock.should_stop(false, true, now + 30000), "it is not stopped at exactly 30 s");
  check(clock.should_stop(false, true, now + 30001), "it is stopped after 30 s of nothing to do");
  clock.reset();  // the stop
  now += 45000;   // a share or receive 45 s later starts a new helper: idle for its first second, nothing registered yet
  clock.reset();  // the start
  check(!clock.should_stop(false, true, now + 1000), "a new helper is not stopped by the clock of the old one");
  check(!clock.should_stop(true, true, now + 2000) && clock.since == 0, "a busy helper clears the clock");
  check(!clock.should_stop(false, false, now + 60000), "no helper, nothing to stop");

  bx::LinkIdleClock stale;  // what the engine did: no reset on stop or start
  stale.should_stop(false, true, 100000);
  check(stale.should_stop(false, true, 145000), "without the reset the old clock stops the new helper (the bug)");
}

// Sharing: a clip made to fit a chat's upload limit, and the webhook a clip is posted to. The fit is
// a promise to the person sharing ("it will upload"), so it is checked at the edges: a file that already
// fits, a long clip that only fits smaller, and one that cannot fit at all.
void test_share() {
  std::printf("share\n");
  const int64_t mib20 = bx::share_limit_bytes(20);
  check(mib20 == 20LL * 1024 * 1024, "the limit is in MiB, as Discord counts it");
  check(bx::share_limit_bytes(0) == mib20 && bx::share_limit_bytes(-5) == mib20, "no limit set means Discord's free 20 MiB");
  check(bx::share_limit_bytes(100000) == 500LL * 1024 * 1024, "a limit is capped at 500 MiB");

  bx::ShareFit fit = bx::share_fit(30.0, 12LL * 1024 * 1024, 1920, 1080, mib20);
  check(fit.fits && fit.copy && fit.width == 1920 && fit.height == 1080, "a clip that already fits is sent as it is");

  fit = bx::share_fit(30.0, 150LL * 1024 * 1024, 1920, 1080, mib20);
  check(fit.fits && !fit.copy, "a 30 s 1080p clip over the limit is re-encoded");
  const double bytes = (fit.video_kbps + fit.audio_kbps) * 1000.0 / 8.0 * 30.0;
  check(bytes < static_cast<double>(mib20) * 0.95, "the fitted rate lands under the limit with headroom");
  check(fit.height == 1080 && fit.width == 1920, "30 s at 20 MiB keeps 1080p");

  fit = bx::share_fit(90.0, 400LL * 1024 * 1024, 1920, 1080, mib20);
  check(fit.fits && fit.height < 1080 && fit.height >= 360, "90 s at 20 MiB steps the picture down");
  check(fit.width % 2 == 0 && fit.height % 2 == 0, "the fitted size is even");
  check(std::abs(static_cast<double>(fit.width) / fit.height - 16.0 / 9.0) < 0.02, "the step keeps the aspect ratio");

  fit = bx::share_fit(1800.0, 4000LL * 1024 * 1024, 1920, 1080, mib20);
  check(!fit.fits, "half an hour cannot fit in 20 MiB");

  fit = bx::share_fit(60.0, 300LL * 1024 * 1024, 1080, 1920, mib20);
  check(fit.fits && fit.width < fit.height, "a portrait clip stays portrait");
  check(fit.width <= 1080, "a portrait clip steps on its short side");

  fit = bx::share_fit(20.0, 90LL * 1024 * 1024, 1280, 720, mib20);
  check(fit.fits && fit.height <= 720, "a clip is never scaled up");

  check(!bx::command_needs_engine_lock("share.prepare") && !bx::command_needs_engine_lock("share.discord"),
        "sharing never waits for the engine lock");
  check(bx::command_needs_engine_lock("sharex"), "only the share. family is let through");

  bx::ExportJob held;
  held.video_kbps = 4000;
  check(bx::export_plan(held) == bx::ExportPlan::reencode, "a share fit re-encodes");

  bx::Config config;
  check(config.share_limit_mib == 20, "the share limit starts at Discord's free 20 MiB");
  bx::Json patch = bx::Json::object();
  patch.set("share_limit_mib", bx::Json(9999));
  bx::config_from_json(patch, config);
  check(config.share_limit_mib == 500, "a share limit is capped at 500 MiB");
  check(!bx::profile_patch(patch).has("share_limit_mib"), "the share limit is not part of a game profile");
  check(!bx::config_to_json(config).has("share_webhook") && !bx::config_to_json(config).has("webhook"),
        "the webhook is never in the config every client reads");

  check(!bx::share_fit(0.0, 1000, 1920, 1080, mib20).fits, "a clip with no length does not fit");
  check(!bx::share_fit(30.0, 0, 0, 0, mib20).fits, "a clip with no picture does not fit");

  // An encoder that ignores a low rate is answered by a smaller picture: the same ladder, one step down.
  bx::ShareFit big = bx::share_fit(20.0, 80LL * 1024 * 1024, 2560, 1440, mib20);
  check(big.fits && big.width == 2560, "a 1440p clip is first tried at its own size");
  bx::ShareFit next = bx::share_fit_smaller(big);
  check(next.width == 1920 && next.height == 1080, "the second attempt steps 1440p down to 1080p");
  next = bx::share_fit_smaller(next);
  check(next.width == 1280 && next.height == 720, "and 1080p down to 720p");
  check(next.video_kbps == big.video_kbps, "a smaller picture keeps the rate it was planned at");
  bx::ShareFit last;
  last.width = 640;
  last.height = 360;
  check(bx::share_fit_smaller(last).width == 0, "there is no step below 360p");
  bx::ShareFit portrait;
  portrait.width = 1080;
  portrait.height = 1920;
  next = bx::share_fit_smaller(portrait);
  check(next.width == 720 && next.height == 1280, "a portrait clip steps down on its short side and stays portrait");

  // The webhook is the one secret sharing keeps. Only Discord's own webhook address is accepted, so a
  // pasted lookalike can never become the place a clip is sent.
  check(bx::discord_webhook_valid("https://discord.com/api/webhooks/123456789012345678/AbC-d_e"), "a webhook is accepted");
  check(bx::discord_webhook_valid("https://ptb.discord.com/api/webhooks/1/x"), "ptb is accepted");
  check(bx::discord_webhook_valid("https://canary.discord.com/api/webhooks/1/x"), "canary is accepted");
  check(bx::discord_webhook_valid("https://discordapp.com/api/webhooks/1/x"), "the old host is accepted");
  check(bx::discord_webhook_valid("https://discord.com/api/v10/webhooks/1/x"), "a versioned path is accepted");
  check(!bx::discord_webhook_valid("http://discord.com/api/webhooks/1/x"), "plain http is refused");
  check(!bx::discord_webhook_valid("https://discord.com.evil.io/api/webhooks/1/x"), "a lookalike host is refused");
  check(!bx::discord_webhook_valid("https://evil.io/discord.com/api/webhooks/1/x"), "discord in the path is refused");
  check(!bx::discord_webhook_valid("https://user@discord.com/api/webhooks/1/x"), "credentials in the address are refused");
  check(!bx::discord_webhook_valid("https://discord.com:8443/api/webhooks/1/x"), "another port is refused");
  check(!bx::discord_webhook_valid("https://discord.com/api/webhooks/abc/x"), "a webhook id is digits");
  check(!bx::discord_webhook_valid("https://discord.com/api/webhooks/1/"), "a webhook needs its token");
  check(!bx::discord_webhook_valid("https://discord.com/api/webhooks/1/x?wait=true"), "a query is refused");
  check(!bx::discord_webhook_valid("https://discord.com/api/webhooks/1/x/../../y"), "a path walk is refused");
  check(!bx::discord_webhook_valid(" https://discord.com/api/webhooks/1/x"), "surrounding space is refused");
  check(bx::discord_webhook_hint("https://discord.com/api/webhooks/123456789012345678/secret-token") == "webhook \xE2\x80\xA6" "5678",
        "the hint shows the webhook's id ending, never the token");
  check(bx::discord_webhook_hint("nonsense").empty(), "an invalid address has no hint");
  check(bx::discord_webhook_id("https://discord.com/api/webhooks/123456789012345678/secret-token") == "123456789012345678",
        "destination identity uses the full webhook id and never its token");
  check(bx::discord_webhook_id("nonsense").empty(), "an invalid address has no destination id");

  // A clip name goes into a JSON message; a name full of quotes stays one string, and no mention fires.
  const std::string payload = bx::discord_payload_json("say \"hi\" @everyone");
  check(payload.find("\"allowed_mentions\":{\"parse\":[]}") != std::string::npos, "a shared clip can ping nobody");
  check(payload.find("say \\\"hi\\\" @everyone") != std::string::npos, "the message is escaped");

  // The multipart body is built around the file, which is streamed: the head and the tail are all that
  // live in memory, and the boundary may not appear in what it wraps.
  const std::string head = bx::multipart_head("BOUNDARY", "{}", "clip \"1\".mp4");
  check(head.find("--BOUNDARY\r\nContent-Disposition: form-data; name=\"payload_json\"") == 0, "the payload part comes first");
  check(head.find("name=\"files[0]\"; filename=\"clip _1_.mp4\"") != std::string::npos, "a quote never ends the file name");
  check(head.find("Content-Type: video/mp4\r\n\r\n") != std::string::npos, "the file part says it is a video");
  check(bx::multipart_tail("BOUNDARY") == "\r\n--BOUNDARY--\r\n", "the body ends with the closing boundary");
}

// The webhook is kept on disk sealed to this Windows account (DPAPI): readable back here, and nothing a
// copied file or a log line could reuse.
void test_share_secret() {
  std::printf("share secret\n");
  const std::string secret = "https://discord.com/api/webhooks/1/token";
  const std::string sealed = bx::protect_secret(secret);
  check(!sealed.empty(), "a secret seals");
  check(sealed.find("token") == std::string::npos, "the sealed form does not carry the secret");
  std::string opened;
  check(bx::unprotect_secret(sealed, opened) && opened == secret, "a sealed secret opens again on this account");
  check(!bx::unprotect_secret("not base64 at all!", opened), "garbage does not open");
  std::string tampered = sealed;
  tampered[tampered.size() / 2] = tampered[tampered.size() / 2] == 'A' ? 'B' : 'A';
  check(!bx::unprotect_secret(tampered, opened), "a changed seal does not open");
}

void test_webhook_list() {
  std::printf("webhook destinations\n");
  std::vector<bx::WebhookEntry> hooks;
  bx::webhook_upsert(hooks, {"1234", "Friends", "sealed-one"});
  bx::webhook_upsert(hooks, {"9876", "Team", "sealed-two"});
  check(hooks.size() == 2, "multiple destinations are kept");
  check(bx::webhook_find(hooks, "")->id == "1234", "old clients select the first destination");
  check(bx::webhook_find(hooks, "9876")->sealed == "sealed-two", "selection uses the full webhook id");
  check(bx::webhook_find(hooks, "missing") == nullptr, "an unknown destination never falls back");
  bx::webhook_upsert(hooks, {"1234", "Friends renamed", "sealed-new"});
  check(hooks.size() == 2 && hooks[0].name == "Friends renamed" && hooks[0].sealed == "sealed-new",
        "saving the same webhook updates its destination");
  check(bx::webhook_remove(hooks, "1234") && hooks.size() == 1 && hooks[0].id == "9876",
        "removing one destination keeps the others");
  check(!bx::webhook_remove(hooks, "missing") && hooks.size() == 1, "an unknown removal keeps all destinations");
}

void test_elevation_policy() {
  std::printf("administrator launch\n");
  check(bx::elevation_needed(true, false, false), "a direct normal engine launch requests elevation when configured");
  check(!bx::elevation_needed(true, true, false), "an elevated engine does not relaunch again");
  check(!bx::elevation_needed(false, false, false), "an ordinary engine does not request elevation when disabled");
  check(!bx::elevation_needed(true, false, true), "diagnostic and export processes do not request elevation");
  check(bx::elevation_arguments(L"\"C:\\Program Files\\BuffrX\\engine.exe\" --open-link \"buffrx://l/example\"") ==
            L"--open-link \"buffrx://l/example\"", "elevation keeps a link and its original quoting");
  check(bx::elevation_arguments(L"C:\\BuffrX\\engine.exe --autostart") == L"--autostart",
        "elevation keeps the autostart flag");
  check(bx::elevation_arguments(L"\"C:\\BuffrX\\engine.exe\"").empty(), "elevation permits a launch without arguments");
}

// AV1 from OBS's NVENC (an RTX 40) declares level 7.3, high tier, in its sequence header. libaom, the
// runtime FFmpeg's one software AV1 decoder, treats the 7.x levels as undefined and refuses the whole
// stream ("Bitstream not supported", then "No sequence header" on every frame), so no thumbnail,
// export or share fit worked on an AV1 clip. A 7.x level is rewritten to 31 ("unconstrained") in the
// packets: same bit length, and a level only states decoder limits, never how a frame decodes.
std::vector<uint8_t> av1_bits(const std::string& pattern) {
  std::vector<uint8_t> out;
  int count = 0;
  for (char c : pattern) {
    if (c != '0' && c != '1') continue;
    if (count % 8 == 0) out.push_back(0);
    if (c == '1') out.back() |= static_cast<uint8_t>(0x80 >> (count % 8));
    ++count;
  }
  return out;
}

std::vector<uint8_t> av1_obu(uint8_t type, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> obu = {static_cast<uint8_t>(type << 3 | 0x02), static_cast<uint8_t>(payload.size())};
  obu.insert(obu.end(), payload.begin(), payload.end());
  return obu;
}

void test_av1_level() {
  std::printf("av1 level\n");
  const std::vector<uint8_t> frame = {0x32, 0x02, 0x10, 0x20};  // a frame OBU (type 6) of two bytes
  // The sequence header of a real clip from this machine (2560x1440, 120 fps, NVENC AV1).
  std::vector<uint8_t> packet = {0x0A, 0x0F, 0x00, 0x00, 0x00, 0xBE, 0xEA, 0x7F, 0xEC, 0xF8, 0x04, 0x33, 0x20, 0x20, 0x20, 0x20, 0x80};
  packet.insert(packet.end(), frame.begin(), frame.end());
  std::vector<uint8_t> expected = packet;
  expected[5] = 0xFE;  // seq_level_idx 23 (7.3) -> 31; the tier bit after it stays
  check(bx::av1_level(packet.data(), packet.size()) == 23, "the clip declares level 7.3 (index 23)");
  check(bx::av1_unconstrain_level(packet.data(), packet.size()) && packet == expected,
        "level 7.3 becomes unconstrained, every other bit kept");
  check(bx::av1_level(packet.data(), packet.size()) == 31, "and reads back as 31");
  check(!bx::av1_unconstrain_level(packet.data(), packet.size()), "a second pass changes nothing");

  // Timing info and a decoder model sit before the level; the parser has to step over them.
  const std::string timed_head = "000 0 0 1" "00000000000000000000000000000001" "00000000000000000000000000111100"
                                 "0" "1" "00011" "00000000000000000000000000000001" "00100" "00100" "0" "00000"
                                 "000000000000";
  const std::vector<uint8_t> timed = av1_obu(1, av1_bits(timed_head + "10101" "1" "0" "00000"));
  std::vector<uint8_t> timed_packet = timed;
  check(bx::av1_level(timed_packet.data(), timed_packet.size()) == 21, "the level is found past timing and decoder model info");
  check(bx::av1_unconstrain_level(timed_packet.data(), timed_packet.size()) &&
            timed_packet == av1_obu(1, av1_bits(timed_head + "11111" "1" "0" "00000")),
        "and rewritten in place");

  // A reduced still-picture header has the level straight after the first five bits.
  std::vector<uint8_t> still = av1_obu(1, av1_bits("000 1 1 10110 000000"));
  check(bx::av1_unconstrain_level(still.data(), still.size()) && still == av1_obu(1, av1_bits("000 1 1 11111 000000")),
        "a still picture's level 7.2 is rewritten too");

  std::vector<uint8_t> level51 = av1_obu(1, av1_bits("000 0 0 0 0 00000 000000000000 01101 0 00"));
  const std::vector<uint8_t> level51_before = level51;
  check(bx::av1_level(level51.data(), level51.size()) == 13, "level 5.1 reads as 13");
  check(!bx::av1_unconstrain_level(level51.data(), level51.size()) && level51 == level51_before, "level 5.1 is left alone");

  std::vector<uint8_t> frame_only = {0x12, 0x00, 0x32, 0x02, 0x10, 0x20};  // a delimiter and a frame
  check(bx::av1_level(frame_only.data(), frame_only.size()) == -1, "a packet with no sequence header has no level");
  std::vector<uint8_t> truncated = {0x0A, 0x0F, 0x00, 0x00};  // says 15 bytes, has 2
  check(!bx::av1_unconstrain_level(truncated.data(), truncated.size()), "a truncated header is not read past its end");
  std::vector<uint8_t> runaway = {0x12, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
  check(bx::av1_level(runaway.data(), runaway.size()) == -1, "an overlong size field is refused");
}

// BuffrX links: whatever a click or a paste hands over is read strictly, and a received file's name
// (chosen by the sender) can never become a path, a device or a program.
void test_links() {
  std::printf("links\n");
  {
    // The helper hashes a finished file and counts those bytes from zero: the stall clock must move on them
    // (a slow hash is not a dead wire) while the count shown never goes back.
    int64_t held = 1000;
    check(bx::receive_clock_moves(held, 10, true) && held == 1000, "a hash pass moves the stall clock without moving the count");
    check(!bx::receive_clock_moves(held, 10, false) && held == 1000, "a progress line with no new bytes does not move the clock");
    check(bx::receive_clock_moves(held, 1001, false) && held == 1001, "new bytes move the clock and the count");
    // A received picture is filed only when its first bytes are the kind its name says.
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0};
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    const uint8_t exe[] = {'M', 'Z', 0x90, 0, 3, 0, 0, 0};
    check(bx::received_image_ok("a.png", png, sizeof(png)) && bx::received_image_ok("a.JPG", jpeg, sizeof(jpeg)) &&
              bx::received_image_ok("a.jpeg", jpeg, sizeof(jpeg)),
          "a PNG and a JPEG are taken as the pictures they are");
    check(!bx::received_image_ok("a.png", exe, sizeof(exe)), "a program named .png is not a picture");
    check(!bx::received_image_ok("a.jpg", png, sizeof(png)), "a PNG named .jpg is refused: the name and the bytes must agree");
    check(!bx::received_image_ok("a.png", png, 4) && !bx::received_image_ok("a.mp4", png, sizeof(png)),
          "a file too short to tell, or one that is not a picture by name, is not passed as one");
    check(bx::link_window_matches(1000, 1000 + 3600, 3600), "a link is reused for the lifetime it was made with");
    check(!bx::link_window_matches(1000, 1000 + 3600, 24 * 3600) && !bx::link_window_matches(1000, 1000 + 24 * 3600, 3600),
          "a 1 hour link is never handed out as a 24 hour one, nor the other way round");
  }
  bx::LinkTicket ticket;
  for (int i = 0; i < 32; ++i) ticket.endpoint[i] = static_cast<uint8_t>(i);
  for (int i = 0; i < 16; ++i) ticket.share[i] = static_cast<uint8_t>(0xA0 + i);
  for (int i = 0; i < 16; ++i) ticket.token[i] = static_cast<uint8_t>(0x50 + i);
  ticket.expires = 1792509473u;
  ticket.relay = "https://aps1-1.relay.n0.iroh.link./";
  const std::string payload = bx::link_payload(ticket);
  bx::LinkTicket back;
  check(bx::parse_link_payload(payload, back), "a payload reads back");
  check(std::memcmp(back.endpoint, ticket.endpoint, 32) == 0 && std::memcmp(back.share, ticket.share, 16) == 0 &&
            std::memcmp(back.token, ticket.token, 16) == 0 && back.expires == ticket.expires && back.relay == ticket.relay,
        "every field survives the round trip");
  check(payload.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos,
        "a payload is URL-safe as it is");
  check(payload.size() == 104, "an iroh relay is compacted: 104 characters, where writing it out gave 142");

  const std::string page_link = bx::link_url("https://example.github.io/buffrx/l/", ticket);
  check(page_link == "https://example.github.io/buffrx/l/#" + payload, "a link page link puts the payload in the fragment");
  const std::string named_link = bx::link_url("https://example.github.io/buffrx/l/", ticket, "Jude R");
  check(named_link == page_link + "?n=Jude%20R", "a sender name rides after the payload, percent-encoded");
  check(bx::parse_link_url(named_link, back) && back.expires == ticket.expires, "a link with a sender name reads back");
  check(bx::parse_link_url(named_link + ").", back), "punctuation after a sender name is ignored");
  check(bx::link_url("", ticket, "Jude") == "buffrx://l/" + payload, "a buffrx:// link carries no sender name");
  check(bx::link_sender_name("  Jude\n") == "Jude" && bx::link_sender_name(" \t\r\n") == std::string(),
        "a sender name is trimmed and control characters are dropped");
  check(bx::link_sender_name(std::string(40, 'a')).size() == 32, "a sender name is at most 32 bytes");
  std::string accented;
  for (int i = 0; i < 17; ++i) accented += "\xC3\xA9";  // 34 bytes: the cut must not split one
  check(bx::link_sender_name(accented) == accented.substr(0, 32), "a sender name never splits a UTF-8 character");
  check(bx::link_url("", ticket) == "buffrx://l/" + payload, "without a link page the link is buffrx://");
  check(bx::parse_link_url(page_link, back) && back.expires == ticket.expires, "a link page link reads back");
  check(bx::parse_link_url("buffrx://l/" + payload, back), "a buffrx:// link reads back");
  check(bx::parse_link_url("buffrx://l/#" + payload, back), "a buffrx:// link with a # reads back");
  check(bx::parse_link_url("  buffrx://l/" + payload + "/\r\n", back), "spaces and a trailing slash from a paste are ignored");
  check(bx::parse_link_url("buffrx://l/" + payload + ".", back) && bx::parse_link_url("(buffrx://l/" + payload + "),", back),
        "a link copied out of a sentence, with its punctuation, still reads");
  check(!bx::parse_link_url("buffrx://l/" + payload + "\"&calc", back), "anything after the payload is refused");
  check(!bx::parse_link_url("http://example.com/#" + payload, back), "plain http is refused");
  check(!bx::parse_link_url("file:///C:/x#" + payload, back), "other schemes are refused");
  check(!bx::parse_link_url("buffrx://l/AAAA", back), "a short payload is refused");
  check(!bx::parse_link_url(std::string(5000, 'A'), back), "an oversized paste is refused");

  bx::LinkTicket bad = ticket;
  bad.relay = "http://relay.example/";
  check(!bx::parse_link_payload(bx::link_payload(bad), back), "a relay that is not https is refused");
  bad.relay = "https://relay.example/\"onerror";
  check(!bx::parse_link_payload(bx::link_payload(bad), back), "a relay with odd characters is refused");
  bad.relay.clear();
  check(bx::parse_link_payload(bx::link_payload(bad), back) && back.relay.empty(), "no relay is fine (DNS finds the peer)");

  bx::LinkTicket other = ticket;
  other.relay = "https://euw1-1.relay.iroh.network./";
  check(bx::parse_link_payload(bx::link_payload(other), back) && back.relay == other.relay,
        "the other iroh relay domain round-trips compacted");
  other.relay = "https://relay.example:8443/";
  check(bx::parse_link_payload(bx::link_payload(other), back) && back.relay == other.relay,
        "a relay that is not on an iroh domain is written out whole");
  std::string tampered = payload;
  tampered[0] = tampered[0] == 'A' ? 'B' : 'A';  // changes the version byte
  check(!bx::parse_link_payload(tampered, back), "an unknown version is refused");
  check(!bx::parse_link_payload(payload + "AA", back), "trailing bytes are refused");

  // The panel reads the clipboard a frame after the engine wrote it, so a link this PC copied must be
  // recognised from the copy itself, not from the event that announced it.
  const std::string link = bx::link_url(std::string(), ticket);
  check(bx::link_offer_wanted(link, std::string(), std::string(), std::string()), "a link from someone else is offered");
  check(!bx::link_offer_wanted(link, link, std::string(), std::string()), "the link already offered is not offered again");
  check(!bx::link_offer_wanted(link, std::string(), link, std::string()), "the panel's own copy is not offered back");
  check(!bx::link_offer_wanted(link, std::string(), std::string(), link), "a copy the engine made for the page is not offered back");
  check(!bx::link_offer_wanted(std::string(), std::string(), std::string(), std::string()), "an empty clipboard offers nothing");

  check(bx::link_page_valid(bx::kDefaultLinkPage), "the default link page is a valid page address");
  check(bx::link_page_or_default("") == bx::kDefaultLinkPage, "an empty setting resolves to the default page");
  check(bx::link_page_or_default("https://example.github.io/buffrx/l/") == "https://example.github.io/buffrx/l/",
        "a configured page is kept");
  check(bx::link_page_or_default("http://example.com/") == bx::kDefaultLinkPage,
        "a malformed page falls back to the default");
  check(bx::Config().share_link_page == bx::kDefaultLinkPage, "a new config starts on the default page");
  check(bx::link_url(bx::kDefaultLinkPage, ticket).rfind("https://", 0) == 0,
        "a default-page link is always an https link");
  check(bx::link_page_valid("") && bx::link_page_valid("https://example.github.io/buffrx/l/"), "a link page address is accepted");
  check(!bx::link_page_valid("https://example.github.io/l") && !bx::link_page_valid("http://example.com/") &&
            !bx::link_page_valid("https://example.com/?a=1/") && !bx::link_page_valid("https://u@example.com/") &&
            !bx::link_page_valid("https://example.com/#x/"),
        "a link page address must be plain https ending in /");

  // The short-link service is asked about the ticket, never the whole address, and a code is dropped by the
  // same ticket the service stored: today's link can carry a different relay after a restarted helper.
  check(bx::link_fragment("https://buffrx.app/l/#abc?n=Jude") == "abc?n=Jude",
        "a link's ticket is the text after its #");
  check(bx::link_fragment("https://buffrx.app/l/").empty() && bx::link_fragment("https://buffrx.app/l/#").empty() &&
            bx::link_fragment("").empty(),
        "a link without a ticket has none to send");
  check(bx::revoke_endpoint("https://buffrx.app/api/shorten") == "https://buffrx.app/api/revoke",
        "a code is revoked at the same host that made it");
  check(bx::revoke_endpoint("http://127.0.0.1:8787/api/shorten") == "http://127.0.0.1:8787/api/revoke",
        "a probe service is revoked at the probe's own host");
  check(bx::revoke_endpoint("https://elsewhere.example/shorten").empty(),
        "an address that is not the service's never has a code dropped");

  check(bx::received_file_name("Clutch round.mp4") == "Clutch round.mp4", "a plain name is kept");
  check(bx::received_file_name("..\\..\\Windows\\evil.mp4") == "evil.mp4", "a path is reduced to its name");
  check(bx::received_file_name("a/b/c.MKV") == "c.MKV", "forward slashes too");
  check(bx::received_file_name("bad<>:\"|?*name.png") == "bad_______name.png", "forbidden characters are replaced");
  check(bx::received_file_name("CON.mp4") == "_CON.mp4" && bx::received_file_name("lpt1.jpg") == "_lpt1.jpg", "device names are defused");
  check(bx::received_file_name(" .. clip .. .mp4") == "clip.mp4", "leading and trailing dots and spaces go");
  check(bx::received_file_name("run.exe").empty() && bx::received_file_name("clip.mp4.exe").empty() &&
            bx::received_file_name("noext").empty(),
        "only BuffrX's own file types are accepted");
  check(bx::received_file_name(".mp4") == "Shared clip.mp4", "a nameless file gets a name");
  const std::string long_name = bx::received_file_name(std::string(300, 'x') + ".mp4");
  check(long_name.size() == 104 && long_name.substr(100) == ".mp4", "a long name is cut to 100 bytes plus the extension");
  std::string utf8;
  for (int i = 0; i < 60; ++i) utf8 += "\xC3\xA9";  // ÃƒÂ©, two bytes each
  const std::string cut = bx::received_file_name(utf8 + ".mp4");
  check(cut.size() == 104 && static_cast<unsigned char>(cut[99]) == 0xA9, "a cut never splits a UTF-8 character");
  check(bx::received_file_name(std::string("a\x01" "b.mp4")) == "a_b.mp4", "control characters are replaced");

  // What the engine will file and what it will try again: the rules that keep a bad name off disk and a
  // hopeless fetch from looping.
  check(bx::receive_name_is_safe("Clutch round.mp4") && bx::receive_name_is_safe("shot.PNG"),
        "a plain received name is safe");
  check(!bx::receive_name_is_safe("") && !bx::receive_name_is_safe("..") && !bx::receive_name_is_safe("a/b.mp4") &&
            !bx::receive_name_is_safe("a\\b.mp4") && !bx::receive_name_is_safe("a.mp4:ads") &&
            !bx::receive_name_is_safe("clip.mp4.exe") && !bx::receive_name_is_safe("noext") &&
            !bx::receive_name_is_safe(".mp4") && !bx::receive_name_is_safe("a.mp4 "),
        "a path, a stream, a foreign type or a trailing space is not a received name");
  check(!bx::receive_name_is_safe("CON.mp4") && !bx::receive_name_is_safe("nul.mkv") &&
            !bx::receive_name_is_safe("COM1.mp4") && !bx::receive_name_is_safe("lpt9.jpg"),
        "a DOS device name is not a received name");
  check(bx::receive_retryable("unreachable") && bx::receive_retryable("stalled") &&
            bx::receive_retryable("helper") && bx::receive_retryable("incomplete"),
        "a network failure is worth another try");
  check(!bx::receive_retryable("declined") && !bx::receive_retryable("expired") &&
            !bx::receive_retryable("refused") && !bx::receive_retryable("no_space") &&
            !bx::receive_retryable("not_a_clip") && !bx::receive_retryable("too_large") &&
            !bx::receive_retryable("cancelled"),
        "a settled refusal is not retried");
  check(bx::receive_backoff_ms(1) == 1000 && bx::receive_backoff_ms(2) == 3000 && bx::receive_backoff_ms(3) == 8000 &&
            bx::receive_backoff_ms(4) == 15000 && bx::receive_backoff_ms(5) == 30000,
        "retries wait 1, 3, 8, 15 and 30 seconds");
  check(bx::receive_retryable("gone") == false, "a clip deleted on the sharer's PC is not retried");
  check(!bx::receive_retryable("corrupt"), "a file that does not match the sender's hash is not retried");
  check(bx::receive_tries(false) == 3 && bx::receive_tries(true) == 5,
        "a sharer that never answered gets 3 tries; one that did gets 5, because a retry resumes");

  // The sharer's PC decides when a link has expired; this PC's clock only turns away what is long gone.
  check(!bx::link_expired_here(1000, 1000 + 60), "a link a minute past its end is still tried (clocks differ)");
  check(!bx::link_expired_here(1000, 1000 + bx::kLinkClockGrace), "within the grace it is tried");
  check(bx::link_expired_here(1000, 1000 + bx::kLinkClockGrace + 1), "past the grace it is expired here");

  // A live link is offered to the helper again; one at or past its expiry is not, or the helper would
  // serve past the time the sharer chose.
  check(bx::link_serve_live(2000, 1000), "a live link is served again");
  check(!bx::link_serve_live(1000, 1000), "a link expiring now is not served again");
  check(!bx::link_serve_live(999, 1000), "an expired link is not served again");
  check(bx::link_reuse_allowed(true, true, "https://relay.example/abc", true), "a ready, online helper's endpoint is reused");
  check(!bx::link_reuse_allowed(false, true, "https://relay.example/abc", true), "a helper that is not ready is not reused");
  check(!bx::link_reuse_allowed(true, false, "https://relay.example/abc", true), "a stopped helper is not reused");
  check(!bx::link_reuse_allowed(true, true, "", true), "an empty endpoint is not reused");
  check(!bx::link_reuse_allowed(true, true, "https://relay.example/abc", false), "a helper off the network is not reused");

  // Windows' own proxy setting, as WinHTTP hands it over, becomes one http:// address for the relay.
  check(bx::proxy_from_windows("") == "", "no proxy, no address");
  check(bx::proxy_from_windows("proxy.school.net:8080") == "http://proxy.school.net:8080", "a plain host:port");
  check(bx::proxy_from_windows("http=a.example:80;https=b.example:443") == "http://b.example:443",
        "the https entry wins: the relay is https");
  check(bx::proxy_from_windows("http=a.example:80") == "http://a.example:80", "an http entry alone is used");
  check(bx::proxy_from_windows("socks=s.example:1080") == "", "a SOCKS-only setting is not an http proxy");
  check(bx::proxy_from_windows("http://c.example:3128") == "http://c.example:3128", "a scheme already there is kept");
  check(bx::proxy_from_windows(" ftp=f:21 ; https=d.example:8443 ") == "http://d.example:8443", "spaces and other entries");
  check(bx::proxy_from_windows("bad host:80") == "", "a value with a space in the host is refused");
  check(bx::receive_watchdog("connecting", 1000, 1000 + bx::kConnectLimitMs).empty() &&
            !bx::receive_watchdog("connecting", 1000, 1000 + bx::kConnectLimitMs + 1).empty() &&
            bx::receive_watchdog("receiving", 5000, 5000 + 60000).empty() &&
            !bx::receive_watchdog("receiving", 5000, 5000 + 60001).empty(),
        "a connect times out at 180 s and a stalled transfer at 60 s");

  uint8_t raw[4] = {0x00, 0x7F, 0xA0, 0xFF};
  uint8_t decoded[4] = {};
  check(bx::hex_encode(raw, 4) == "007fa0ff" && bx::hex_decode("007fa0ff", decoded, 4) && std::memcmp(raw, decoded, 4) == 0,
        "hex round trips");
  check(!bx::hex_decode("007fa0f", decoded, 4) && !bx::hex_decode("zz7fa0ff", decoded, 4), "bad hex is refused");
}

// The vendor decides which cockpit the interface paints, so the mapping is a contract: DXGI ids in,
// protocol names out. A device name is never parsed.
void test_gpu_vendor() {
  std::printf("gpu vendor\n");
  check(bx::gpu_vendor_from_id(0x10DE) == "nvidia", "NVIDIA's vendor id maps to nvidia");
  check(bx::gpu_vendor_from_id(0x1002) == "amd", "AMD's vendor id maps to amd");
  check(bx::gpu_vendor_from_id(0x8086) == "intel", "Intel's vendor id maps to intel");
  check(bx::gpu_vendor_from_id(0x1234) == "other", "an unknown vendor id maps to other");
  check(bx::gpu_vendor_from_id(0x106B) == "other", "Apple's id is not guessed at");
}

// The mic strip's live level: attack is instant, decay is smooth, and the value never leaves 0..1.
void test_smooth_level() {
  std::printf("level smoothing\n");
  const float up = bx::smooth_level(0.2f, 0.8f, 0.25);
  check(up == 0.8f, "a rising sample is shown immediately");

  const float down = bx::smooth_level(0.8f, 0.2f, 0.25);
  check(down > 0.2f && down < 0.8f, "a falling sample decays smoothly toward it");

  const float settled = bx::smooth_level(0.5f, 0.2f, 10.0);
  check(settled == 0.2f, "a long quiet spell settles on the fresh sample");

  check(bx::smooth_level(0.5f, 1.5f, 0.1) <= 1.0f, "the level never leaves the top");
  check(bx::smooth_level(0.5f, -0.5f, 0.1) >= 0.0f, "the level never leaves the bottom");
  check(bx::smooth_level(0.4f, 0.6f, 0.0) == 0.6f, "a zero interval still moves with the sample");
}

void test_audio_meter() {
  std::printf("audio meter\n");
  check(bx::audio_meter_level(-60.0f) == 0.0f, "the meter floor is silence");
  check(std::abs(bx::audio_meter_level(-30.0f) - 0.5f) < 0.001f, "-30 dBFS fills half the meter");
  check(std::abs(bx::audio_meter_level(-12.0f) - 0.8f) < 0.001f, "a loud signal approaches the top");
  check(bx::audio_meter_level(3.0f) == 1.0f, "signals above full scale do not overflow");
  check(bx::audio_meter_level(-std::numeric_limits<float>::infinity()) == 0.0f, "negative infinity is silence");
  check(bx::audio_meter_level(std::numeric_limits<float>::infinity()) == 1.0f, "positive infinity reaches the top");
  check(bx::audio_meter_level(std::numeric_limits<float>::quiet_NaN()) == 0.0f, "invalid readings are silence");

  check(bx::microphone_meter_level(-60.0f) == 0.0f && bx::microphone_meter_level(-42.0f) == 0.0f,
        "the microphone meter ignores quiet noise at and below its threshold");
  check(bx::microphone_meter_level(-41.0f) > 0.0f && bx::microphone_meter_level(-41.0f) < 0.03f,
        "the microphone meter comes in gently above its threshold");
  check(std::abs(bx::microphone_meter_level(-30.0f) - 2.0f / 7.0f) < 0.001f &&
            bx::microphone_meter_level(-30.0f) < bx::audio_meter_level(-30.0f),
        "normal microphone levels remain visible but less sensitive than speaker peaks");
  check(bx::microphone_meter_level(0.0f) == 1.0f && bx::microphone_meter_level(3.0f) == 1.0f,
        "a loud microphone still reaches the top without overflowing");
  check(bx::microphone_meter_level(std::numeric_limits<float>::quiet_NaN()) == 0.0f &&
            bx::microphone_meter_level(-std::numeric_limits<float>::infinity()) == 0.0f &&
            bx::microphone_meter_level(std::numeric_limits<float>::infinity()) == 1.0f,
        "invalid and infinite microphone readings stay within bounds");
  check(bx::kAudioMeterSampleMs == 100 && bx::kMicrophoneMeterSampleMs == 200,
        "the microphone updates more slowly without slowing the speaker meter");

  std::atomic<float> peak{bx::kAudioMeterSilenceDb};
  const float louder[] = {-48.0f, -20.0f};
  const float quieter[] = {-40.0f, -80.0f};
  bx::audio_meter_accumulate(peak, louder, 2);
  bx::audio_meter_accumulate(peak, quieter, 2);
  check(bx::audio_meter_take_peak(peak) == -20.0f, "the loudest channel and callback survive until the next frame");
  check(bx::audio_meter_take_peak(peak) == bx::kAudioMeterSilenceDb, "taking a peak clears it for the next frame");
  const float special[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  bx::audio_meter_accumulate(peak, special, 2);
  check(bx::audio_meter_level(bx::audio_meter_take_peak(peak)) == 1.0f, "clipping is retained even beside an invalid sample");

  check(bx::audio_meter_wanted(true, false, true, false), "meters run on the expanded deck");
  check(!bx::audio_meter_wanted(false, false, true, false), "meters stop when the expanded window is absent or hidden");
  check(!bx::audio_meter_wanted(true, true, true, false), "meters stop while minimized");
  check(!bx::audio_meter_wanted(true, false, false, false), "meters stop when another view covers the deck");
  check(!bx::audio_meter_wanted(true, false, true, true), "meters stop as the window closes");
}

// Free bytes on the volume a folder sits on: the Usage section's denominator.
void test_volume_free_bytes() {
  std::printf("volume free space\n");
  char temp[512] = {0};
  GetEnvironmentVariableA("TEMP", temp, sizeof(temp));
  check(bx::volume_free_bytes(temp) > 0, "a real directory reports free bytes");
  check(bx::volume_free_bytes("C:\\no-such-drive-xyz\\deeper") == 0, "a path with no volume reports 0");
}

// A client that stops reading must not be able to stop the server. This is the promise that a wedged
// interface never stalls capture: the tick, the tray, the hotkeys and the save queue all broadcast
// from threads that must never block on a pipe.
// The filing half of a save, out of ObsHost: every branch the probe, the trim, the verify and the
// move can take, with no libobs, no decoder and no real footage.
// Which encoders this machine may use, best first: the rule ObsHost used to hold, now checkable without
// a graphics device.
void test_encoder_candidates() {
  std::printf("encoder choice\n");
  const std::vector<std::string> available{"obs_x264", "h264_nvenc", "h264_amf", "h264_qsv", "av1_nvenc",
                                           "av1_amf", "hevc_nvenc", "ffmpeg_av1_svt", "com.apple.videotoolbox"};
  check(bx::encoder_candidates(available, "h264", "auto") ==
            std::vector<std::string>{"h264_nvenc", "h264_amf", "h264_qsv", "obs_x264"},
        "H.264 in auto mode: NVENC, then AMF, then QSV, then software");
  check(bx::encoder_candidates(available, "av1", "auto") == std::vector<std::string>{"av1_nvenc", "av1_amf"},
        "AV1 keeps only the AV1 encoders");
  check(bx::encoder_candidates(available, "hevc", "auto") == std::vector<std::string>{"hevc_nvenc"},
        "HEVC keeps only the HEVC encoders");
  check(bx::encoder_candidates(available, "h264", "amf") == std::vector<std::string>{"h264_amf"},
        "a chosen vendor narrows the list to itself");
  check(bx::encoder_candidates(available, "h264", "software") == std::vector<std::string>{"obs_x264"},
        "software means the software encoder");
  check(bx::encoder_candidates(available, "av1", "qsv").empty(), "a vendor with no encoder for the codec leaves nothing");
  check(bx::encoder_candidates({"obs_nvenc_h264", "obs_nvenc_h264_tex"}, "h264", "auto") ==
            std::vector<std::string>{"obs_nvenc_h264_tex", "obs_nvenc_h264"},
        "a texture variant comes before its plain twin");
  const std::vector<std::string> unknown{"ffmpeg_av1_svt", "com.apple.videotoolbox", "something_else"};
  check(bx::encoder_candidates(unknown, "h264", "auto").empty(),
        "FFmpeg-built and foreign encoders are never candidates");
}

void test_save_check() {
  std::printf("save check\n");
  // The 2026-11-06 regression saved six minutes of black at 37-44 kbps on a 24,773 kbps peak; healthy motion on the
  // same setting measured about 1,400 kbps. The floor there is 123 kbps.
  check(bx::clip_may_be_black(37, 24773), "a black clip is named");
  check(bx::clip_may_be_black(44, 24773), "44 kbps black is named");
  check(bx::clip_may_be_black(1, 24773), "1 kbps is named");
  check(bx::clip_may_be_black(122, 24773), "one below the floor is named");
  check(!bx::clip_may_be_black(123, 24773), "the floor itself is not named");
  check(!bx::clip_may_be_black(1400, 24773), "healthy motion is not named");
  check(!bx::clip_may_be_black(1234, 0), "an unknown peak is never an alarm");
  check(!bx::clip_may_be_black(0, 24773), "an unknown rate is never an alarm");
  // The floor never drops below 60, however small the peak.
  check(!bx::clip_may_be_black(60, 1000), "the floor is never below 60 kbps");
  check(bx::clip_may_be_black(59, 1000), "one below 60 at a small peak is named");
}

void test_save_filing() {
  std::printf("save filing\n");
  const std::string root = temp_directory() + "\\save-filing";
  remove_tree(root);
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  CreateDirectoryA(root.c_str(), nullptr);

  auto make_file = [](const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    std::fwrite("x", 1, 1, file);
    std::fclose(file);
    return true;
  };

  int probes = 0;
  std::string probe_path;
  int trims = 0;
  std::string trim_to;
  double trim_start = 0.0;
  double trim_length = 0.0;
  bool trim_succeeds = true;
  int verifies = 0;
  bool verify_succeeds = true;

  bx::SaveFilingMedia media;
  media.probe = [&](const std::string& path) {
    ++probes;
    probe_path = path;
    bx::MediaInfo info;
    info.ok = true;
    info.duration_seconds = 600.0;
    info.video_kbps = 1400;
    return info;
  };
  media.trim = [&](const std::string&, const std::string& output_path, double start, double length, std::string&) {
    ++trims;
    trim_to = output_path;
    trim_start = start;
    trim_length = length;
    // A real trim writes its file; the failed one leaves a part behind for the caller to clear.
    make_file(output_path);
    return trim_succeeds;
  };
  media.verify = [&](const std::string&) {
    ++verifies;
    bx::ClipCheck check_result;
    check_result.ok = verify_succeeds;
    check_result.reason = verify_succeeds ? std::string() : std::string("no video track");
    return check_result;
  };

  // A ring longer than the length asked for is cut to its tail before it is filed.
  const std::string clip = root + "\\clip.mp4";
  make_file(clip);
  bx::SaveFiling trimmed = bx::file_saved_clip(clip, 30, false, 2, "", "", media);
  check(probes == 1 && probe_path == clip, "the clip is probed once");
  check(trims == 1 && trim_start > 0.0 && trim_length == 31.0, "a ring longer than the ask is trimmed to its tail");
  check(trimmed.trimmed && trimmed.error.empty(), "a trimmed clip is still the clip, with no error");
  check(trimmed.path == clip && !trimmed.filed, "with nowhere to file it, it stays where the ring wrote it");
  check(trimmed.video_kbps == 1400, "the filing carries the probe's rate for the black-clip guard");
  check(GetFileAttributesA((clip + ".trim.mp4").c_str()) == INVALID_FILE_ATTRIBUTES,
        "the trimmed file took the clip's place, so no temporary is left beside it");

  // The whole ring is never cut, however long it is.
  trims = 0;
  bx::file_saved_clip(clip, 30, true, 2, "", "", media);
  check(trims == 0, "a whole-ring save is never trimmed");

  // A trim that fails leaves the clip at its full length and says so in the log, not in an error.
  trims = 0;
  trim_succeeds = false;
  bx::SaveFiling untrimmed = bx::file_saved_clip(clip, 30, false, 2, "", "", media);
  check(trims == 1 && !untrimmed.trimmed && untrimmed.error.empty(), "a failed trim keeps the whole clip, without failing the save");
  check(GetFileAttributesA((clip + ".trim.mp4").c_str()) == INVALID_FILE_ATTRIBUTES, "a failed trim leaves no part behind");

  // A file appearing is not a clip: a container that does not verify is the one real failure.
  verify_succeeds = false;
  bx::SaveFiling unverified = bx::file_saved_clip(clip, 30, false, 2, "Game", "Replay 12", media);
  check(verifies > 0 && unverified.error == "the saved clip did not verify: no video track",
        "a clip that does not verify says exactly why");
  check(unverified.path == clip && !unverified.filed, "and is left where it is, never filed under a game");
  check(GetFileAttributesA(clip.c_str()) != INVALID_FILE_ATTRIBUTES, "a file with bytes in it is kept: it is the person's footage");
  {
    // The muxer died before its first byte: nothing to keep, and the library must not list it.
    const std::string empty = root + "\\empty.mp4";
    FILE* file = std::fopen(empty.c_str(), "wb");
    check(file != nullptr, "the empty file is there to begin with");
    if (file) std::fclose(file);
    const bx::SaveFiling nothing = bx::file_saved_clip(empty, 30, false, 2, "", "", media);
    check(!nothing.error.empty(), "an empty file is not a saved clip");
    check(GetFileAttributesA(empty.c_str()) == INVALID_FILE_ATTRIBUTES, "and an empty file is removed instead of listed as a clip");
  }

  // A verified clip is filed under the game it came from.
  verify_succeeds = true;
  CreateDirectoryA((root + "\\Game").c_str(), nullptr);
  bx::SaveFiling filed = bx::file_saved_clip(clip, 30, false, 2, "Game", "Replay 12", media);
  check(filed.filed && filed.error.empty(), "a verified clip is filed");
  check(filed.path == root + "\\Game\\Replay 12.mp4", "under the game, under the name the save asked for");
  check(GetFileAttributesA(filed.path.c_str()) != INVALID_FILE_ATTRIBUTES, "with the file really there");

  remove_tree(root);
}

void test_pipe_server() {
  std::printf("pipe server\n");
  const std::string name = "\\\\.\\pipe\\buffrx-tests-" + std::to_string(GetCurrentProcessId());
  bx::PipeServer server;
  check(server.start(name, [](int, const std::string&) {}, [](int) {}), "the pipe server starts");

  bx::PipeClient wedged;
  check(wedged.connect(name, 2000), "a client connects");
  for (int i = 0; i < 100 && server.client_count() < 1; ++i) Sleep(20);
  check(server.client_count() == 1, "the server registers the client");

  bx::PipeClient reader;
  check(reader.connect(name, 2000), "a second client connects");
  for (int i = 0; i < 100 && server.client_count() < 2; ++i) Sleep(20);
  check(server.client_count() == 2, "the server registers both clients");

  std::atomic<int> frames_read{0};
  std::thread draining([&] {
    std::string frame;
    while (bx::pipe_read_frame(reader.handle(), frame)) ++frames_read;
  });

  const std::string frame(4096, 'x');
  const DWORD started = GetTickCount();
  for (int i = 0; i < 96; ++i) server.broadcast(frame);  // 384 KB, more than the 64 KB pipe buffer
  const DWORD elapsed = GetTickCount() - started;
  check(elapsed < 4000, "a client that stopped reading cannot stall a broadcast");
  check(server.dropped_clients() == 1, "the client that stopped reading is dropped");
  check(server.client_count() == 1, "and only the client that reads is left");

  for (int i = 0; i < 200 && frames_read.load() < 96; ++i) Sleep(20);
  check(frames_read.load() == 96, "the client that keeps reading receives every frame");

  server.stop();
  draining.join();
  check(server.client_count() == 0, "stopping the server retires every client");
}

// A client that sends commands faster than the workers answer them must not be able to grow the
// engine's memory without a limit. The slot stops reading once the queue is full, so the sender
// waits - and the wait lands on that client's own connection, not on the engine or another client.
void test_pipe_queue_bounded() {
  std::printf("pipe command queue\n");
  const std::string name = "\\\\.\\pipe\\buffrx-tests-queue-" + std::to_string(GetCurrentProcessId());
  std::atomic<bool> open{false};
  std::atomic<int> handled{0};
  bx::PipeServer server;
  check(server.start(name,
                     [&](int, const std::string&) {
                       while (!open.load()) Sleep(1);
                       ++handled;
                     },
                     [](int) {}),
        "the pipe server starts");

  bx::PipeClient sender;
  check(sender.connect(name, 2000), "a client connects");
  const int total = 2000;
  std::atomic<int> sent{0};
  const std::string frame(8 * 1024, 'x');
  std::thread flooding([&] {
    for (int i = 0; i < total; ++i) {
      if (!sender.send(frame)) return;
      ++sent;
    }
  });
  // Two workers hold the first two commands; the queue takes the rest and no more. The pipe's own
  // 64 KB buffer and the calls in flight add a handful of frames to the cap.
  for (int i = 0; i < 200 && sent.load() < static_cast<int>(bx::kMaxPendingCommands); ++i) Sleep(20);
  Sleep(500);
  check(sent.load() <= static_cast<int>(bx::kMaxPendingCommands) + 16,
        "a client that floods the queue waits at its cap");
  open = true;
  for (int i = 0; i < 500 && handled.load() < total; ++i) Sleep(20);
  check(handled.load() == total, "every command the client sent is answered once the queue drains");
  flooding.join();
  check(sent.load() == total, "and every send completed");
  server.stop();
}

// A client that disconnects on its own is reported closed, and the next client on the same slot is a
// different client. The regression this guards: the slots are reused, nothing said the editor had
// gone, and the engine kept forwarding files to an editor that was closed - so the editor never opened
// a second time.
void test_pipe_client_leaves() {
  std::printf("pipe client leaves\n");
  const std::string name = "\\\\.\\pipe\\buffrx-tests-leave-" + std::to_string(GetCurrentProcessId());
  std::mutex mutex;
  std::vector<int> seen;
  std::vector<int> closed;
  bx::PipeServer server;
  check(server.start(name,
                     [&](int client, const std::string&) {
                       std::lock_guard<std::mutex> lock(mutex);
                       seen.push_back(client);
                     },
                     [&](int client) {
                       std::lock_guard<std::mutex> lock(mutex);
                       closed.push_back(client);
                     }),
        "the pipe server starts");
  auto count = [&](std::vector<int>& list) {
    std::lock_guard<std::mutex> lock(mutex);
    return list.size();
  };
  int first_id = 0;
  {
    bx::PipeClient editor;
    check(editor.connect(name, 2000), "the first client connects");
    check(editor.send("{\"cmd\":\"hello\"}"), "and sends a frame");
    for (int i = 0; i < 100 && count(seen) < 1; ++i) Sleep(20);
    check(count(seen) == 1, "the frame reaches the handler");
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!seen.empty()) first_id = seen[0];
    }
  }  // the client goes away
  for (int i = 0; i < 100 && count(closed) < 1; ++i) Sleep(20);
  check(count(closed) == 1, "a client that leaves is reported closed");
  {
    std::lock_guard<std::mutex> lock(mutex);
    check(!closed.empty() && closed[0] == first_id, "under the id its frames carried");
  }
  for (int round = 0; round < 6; ++round) {  // more rounds than there are slots: every slot is reused
    bx::PipeClient again;
    check(again.connect(name, 2000), "a later client connects");
    check(again.send("{\"cmd\":\"hello\"}"), "and sends a frame");
    for (int i = 0; i < 100 && count(seen) < static_cast<size_t>(round + 2); ++i) Sleep(20);
  }
  for (int i = 0; i < 100 && count(closed) < 7; ++i) Sleep(20);
  {
    std::lock_guard<std::mutex> lock(mutex);
    check(seen.size() == 7 && closed.size() == 7, "every client that came and went is reported closed");
    const std::set<int> distinct(seen.begin(), seen.end());
    check(distinct.size() == seen.size(), "every connection has an id of its own, even on a reused slot");
  }
  check(server.client_count() == 0, "nobody is counted once they have all gone");
  server.stop();
}

// Window enumeration now runs on Win32 alone Ã¢â‚¬â€ no temporary libobs capture source, which is the
// win-capture.dll crash this replaces. The ids must stay byte-compatible with what libobs's property
// lists used to hand out: "title:class:exe" with ':' as "#3A" and '#' as "#22".
void test_capture_enum() {
  std::printf("capture enumeration\n");

  check(bx::encode_window_part("a:b") == "a#3Ab", "encode escapes a colon");
  check(bx::encode_window_part("a#b") == "a#22b", "encode escapes a hash");
  check(bx::encode_window_part("#3A") == "#223A", "an existing escape sequence is escaped first");
  check(bx::decode_window_part(bx::encode_window_part("a:b#c:d")) == "a:b#c:d", "encode then decode round-trips");
  check(bx::decode_window_part(bx::encode_window_part("plain")) == "plain", "text without escapes survives");

  // A real window: a colon and a hash in the title, a private window class, our own process.
  const char* kTitle = "BuffrX: enum #1 test";
  WNDCLASSEXA wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcA;
  wc.lpszClassName = "BuffrXTestCls";
  check(RegisterClassExA(&wc) != 0, "the test window class registers");
  HWND window = CreateWindowExA(0, "BuffrXTestCls", kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 480, 320,
                                nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  check(window != nullptr, "the test window is created");
  if (window) {
    char exe_path[MAX_PATH * 2] = {0};
    GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    const char* base = strrchr(exe_path, '\\');
    const std::string exe_name = base ? base + 1 : exe_path;
    const std::string id = bx::make_window_id(kTitle, "BuffrXTestCls", exe_name);

    const std::vector<bx::WindowInfo> windows = bx::enumerate_windows();
    bool found = false;
    for (const auto& info : windows) {
      if (info.label == kTitle) {
        found = true;
        check(info.title == id, "the enumerated id is title:class:exe with libobs escaping");
        check(info.exe == exe_name, "the enumerated exe is the owning process image name");
      }
    }
    if (windows.empty()) std::printf("  [skip] no desktop windows in this session\n");
    else check(found, "a real top-level window appears in the enumeration");

    check(bx::window_id_exists(id), "the created window's id resolves to a live window");
    check(bx::window_id_is_own_process(id), "the created window belongs to this process");
    check(!bx::window_id_exists("Missing:Nope:gone.exe"), "an id nobody owns does not resolve");
    check(!bx::window_id_is_own_process("Missing:Nope:gone.exe"), "a missing id has no process of ours");

    int width = 0;
    int height = 0;
    check(bx::find_window_size(id, width, height), "the created window reports its client size");
    RECT client{};
    check(GetClientRect(window, &client) && width == client.right - client.left && height == client.bottom - client.top,
          "and the size is the window's client area");

    bool in_games = false;
    for (const auto& game : bx::enumerate_game_windows()) {
      if (game.title == id) in_games = true;
    }
    check(!in_games, "a small plain window is not a game candidate");

    DestroyWindow(window);
    UnregisterClassA("BuffrXTestCls", GetModuleHandleA(nullptr));
  }
}

// A persisted capture target must survive contact with reality at load: a saved window or game that
// no longer exists Ã¢â‚¬â€ or that resolves to the engine's own process Ã¢â‚¬â€ falls back to display and says
// why. The machine this is written on had game capture pointed at SearchHost.exe.
void test_validate_capture_target() {
  std::printf("capture target validation\n");

  const char* kTitle = "BuffrX: enum #2 test";
  WNDCLASSEXA wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcA;
  wc.lpszClassName = "BuffrXTestCls2";
  check(RegisterClassExA(&wc) != 0, "the second test window class registers");
  HWND window = CreateWindowExA(0, "BuffrXTestCls2", kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 420, 300,
                                nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  check(window != nullptr, "the second test window is created");
  if (!window) return;

  char exe_path[MAX_PATH * 2] = {0};
  GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
  const char* base = strrchr(exe_path, '\\');
  const std::string exe_name = base ? base + 1 : exe_path;
  const std::string id = bx::make_window_id(kTitle, "BuffrXTestCls2", exe_name);

  // No "kept window" case is constructible here: any window this process creates resolves to this
  // process, so the kept path for window mode is exercised on hardware and via display/any-fullscreen
  // below. What the load check must never do is capture ourselves or a vanished target.

  bx::Config gone;
  gone.capture_mode = "window";
  gone.window_id = "Missing:Nope:gone.exe";
  std::string reason;
  check(!bx::validate_capture_target(gone, reason), "a window that does not exist flips to display");
  check(gone.capture_mode == "display", "and the mode is display");
  check(!reason.empty(), "and the reason says why");

  bx::Config own;
  own.capture_mode = "window";
  own.window_id = id;
  check(!bx::validate_capture_target(own, reason), "a target inside our own process flips to display");
  check(own.capture_mode == "display", "and the mode is display");

  bx::Config not_a_game;
  not_a_game.capture_mode = "game";
  not_a_game.game_title = id;
  check(!bx::validate_capture_target(not_a_game, reason), "a plain window saved as the game target flips to display");
  check(not_a_game.capture_mode == "display", "and the mode is display");

  bx::Config game_gone;
  game_gone.capture_mode = "game";
  game_gone.game_title = "Missing:Nope:gone.exe";
  check(!bx::validate_capture_target(game_gone, reason), "a game that does not exist flips to display");
  check(game_gone.capture_mode == "display", "and the mode is display");

  bx::Config display_kept;
  display_kept.capture_mode = "display";
  check(bx::validate_capture_target(display_kept, reason), "a display target is always kept");

  bx::Config any_fullscreen;
  any_fullscreen.capture_mode = "game";
  any_fullscreen.game_title.clear();
  check(bx::validate_capture_target(any_fullscreen, reason), "'any fullscreen game' is always kept");

  DestroyWindow(window);
  UnregisterClassA("BuffrXTestCls2", GetModuleHandleA(nullptr));
}

// A file of a known size, so the store's byte accounting has something real to measure.
void write_bytes(const std::string& path, size_t count) {
  const size_t slash = path.find_last_of("\\/");
  if (slash != std::string::npos) CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return;
  const std::vector<char> buffer(count, 'x');
  std::fwrite(buffer.data(), 1, buffer.size(), file);
  std::fclose(file);
}

// A file's bytes and last write time, so a test can prove a write did not touch it.
struct FileStamp {
  std::string bytes;
  int64_t mtime = 0;
};

FileStamp file_stamp(const std::string& path) {
  FileStamp stamp;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file) {
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) stamp.bytes.append(buffer, read);
    std::fclose(file);
  }
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
    ULARGE_INTEGER written{};
    written.HighPart = data.ftLastWriteTime.dwHighDateTime;
    written.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.mtime = static_cast<int64_t>(written.QuadPart);
  }
  return stamp;
}

// A helper with pipes but no network, so the link service's failure paths can be exercised deterministically.
int link_test_helper(const std::string& mode) {
  if (mode == "hold-output") {
    char parent_id[32] = {}, ready_name[128] = {}, dead_name[128] = {};
    GetEnvironmentVariableA("BUFFRX_LINK_TEST_PARENT", parent_id, sizeof(parent_id));
    GetEnvironmentVariableA("BUFFRX_LINK_TEST_HOLDER_READY", ready_name, sizeof(ready_name));
    GetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER_DEAD", dead_name, sizeof(dead_name));
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::strtoul(parent_id, nullptr, 10)));
    HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, ready_name);
    HANDLE dead = OpenEventA(EVENT_MODIFY_STATE, FALSE, dead_name);
    if (ready) { SetEvent(ready); CloseHandle(ready); }
    if (parent) { WaitForSingleObject(parent, 5000); CloseHandle(parent); }
    if (dead) { SetEvent(dead); CloseHandle(dead); }
    Sleep(20000);  // keep the helper's stdout open until the service stops its job
    return 0;
  }
  std::string behavior = mode;
  if (mode == "dead-ready") {
    char marker[32768] = {};
    GetEnvironmentVariableA("BUFFRX_LINK_TEST_MARKER", marker, sizeof(marker));
    HANDLE first = CreateFileA(marker, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (first == INVALID_HANDLE_VALUE) behavior = "healthy";
    else CloseHandle(first);
  }
  char line[8192];
  const auto viewer = [](const bx::Json& id) {
    bx::Json event = bx::Json::object();
    event.set("ev", bx::Json("viewer"));
    event.set("share", id);
    event.set("state", bx::Json("start"));
    std::printf("%s\n", event.dump().c_str());
    std::fflush(stdout);
  };
  while (std::fgets(line, sizeof(line), stdin)) {
    bx::Json command;
    std::string error;
    if (!bx::Json::parse(line, command, error)) continue;
    const std::string op = command["op"].as_string();
    if (op == "init") {
      if (behavior == "expiry") {
        char id[64] = {}, expiry[32] = {};
        GetEnvironmentVariableA("BUFFRX_LINK_TEST_EXPIRED_ID", id, sizeof(id));
        GetEnvironmentVariableA("BUFFRX_LINK_TEST_EXPIRY", expiry, sizeof(expiry));
        viewer(bx::Json(id));
        while (static_cast<int64_t>(std::time(nullptr)) <= std::strtoll(expiry, nullptr, 10)) Sleep(20);
      }
      std::printf("{\"ev\":\"ready\",\"endpoint\":\"%s\",\"online\":true}\n", std::string(64, 'a').c_str());
      std::fflush(stdout);
    } else if (op == "serve") {
      if (behavior == "closed-input") CloseHandle(GetStdHandle(STD_INPUT_HANDLE));
      viewer(command["share"]);
      if (behavior == "dead-ready") {
        char executable[32768] = {}, ready_name[128] = {};
        GetModuleFileNameA(nullptr, executable, sizeof(executable));
        GetEnvironmentVariableA("BUFFRX_LINK_TEST_HOLDER_READY", ready_name, sizeof(ready_name));
        const std::string parent_id = std::to_string(GetCurrentProcessId());
        SetEnvironmentVariableA("BUFFRX_LINK_TEST_PARENT", parent_id.c_str());
        SetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER", "hold-output");
        std::string command_line = "\"" + std::string(executable) + "\"";
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION child{};
        if (CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &child)) {
          CloseHandle(child.hThread);
          CloseHandle(child.hProcess);
          HANDLE ready = OpenEventA(SYNCHRONIZE, FALSE, ready_name);
          if (ready) { WaitForSingleObject(ready, 5000); CloseHandle(ready); }
        }
        return 1;
      }
      if (behavior == "crash") {
        char event_name[128] = {};
        GetEnvironmentVariableA("BUFFRX_LINK_TEST_CRASH_EVENT", event_name, sizeof(event_name));
        HANDLE event = OpenEventA(SYNCHRONIZE, FALSE, event_name);
        if (event) {
          WaitForSingleObject(event, 5000);
          CloseHandle(event);
        }
        return 1;
      }
      if (behavior == "closed-input") {
        Sleep(20000);
        return 0;
      }
    }
  }
  return 0;
}

void test_link_helper_failures() {
  std::printf("link helper failures\n");
  const std::string root = temp_directory() + "\\link-helper";
  CreateDirectoryA(root.c_str(), nullptr);
  bx::LibraryPaths paths;
  paths.clips_dir = root + "\\clips";
  paths.screenshots_dir = root + "\\screenshots";
  paths.thumbnails_dir = root + "\\thumbs";
  bx::library_init(paths);
  const std::string file = paths.clips_dir + "\\clip.mp4";
  write_bytes(file, 16);
  char executable[32768] = {};
  const DWORD length = GetModuleFileNameA(nullptr, executable, sizeof(executable));
  check(length > 0 && length < sizeof(executable), "the test helper executable is available");
  if (length == 0 || length >= sizeof(executable)) return;

  const auto watching = [](bx::LinkService& service, const std::string& id) {
    const bx::Json state = service.list();
    for (const bx::Json& link : state["links"].items()) {
      if (link["id"].as_string() == id) return static_cast<int>(link["watching"].as_int());
    }
    return -1;
  };
  const auto wait_for_watching = [&](bx::LinkService& service, const std::string& id, int expected) {
    const uint64_t deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
      if (watching(service, id) == expected) return true;
      Sleep(10);
    }
    return false;
  };

  SetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER", "closed-input");
  const std::string stopped_root = root + "\\stop";
  CreateDirectoryA(stopped_root.c_str(), nullptr);
  {
    bx::LinkService service(stopped_root, executable);
    bx::Json reply;
    std::string error;
    const bool shared = service.share(file, 10, reply, error);
    check(shared, "the test helper serves a link before its input closes");
    if (shared) {
      check(wait_for_watching(service, reply["id"].as_string(), 1), "the helper starts a download before stop");
      // The helper that would not take the stop is ended, which ends the link with it: the stop worked, and
      // links to other files are no longer deleted with it (the new helper serves them again).
      check(service.stop(reply["id"].as_string(), error) && error.empty(), "a link whose helper refused the stop is ended with the helper");
      check(service.list()["links"].items().empty(), "the stopped link is gone from the list");
      check(!service.stop(reply["id"].as_string(), error), "a link that is already stopped is not stopped twice");
    }
  }

  SetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER", "crash");
  const std::string event_name = "Local\\BuffrXLinkTestCrash-" + std::to_string(GetCurrentProcessId());
  HANDLE crash_event = CreateEventA(nullptr, TRUE, FALSE, event_name.c_str());
  check(crash_event != nullptr, "the helper crash can be signalled");
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_CRASH_EVENT", event_name.c_str());
  const std::string crashed_root = root + "\\crash";
  CreateDirectoryA(crashed_root.c_str(), nullptr);
  {
    bx::LinkService service(crashed_root, executable);
    bx::Json reply;
    std::string error;
    const bool shared = service.share(file, 10, reply, error);
    check(shared, "the test helper serves a link before it exits");
    if (shared) {
      const std::string id = reply["id"].as_string();
      check(wait_for_watching(service, id, 1), "the helper reports a download before it exits");
      if (crash_event) SetEvent(crash_event);
      check(wait_for_watching(service, id, 0), "helper exit clears the active download count");
    }
  }
  if (crash_event) CloseHandle(crash_event);
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_CRASH_EVENT", nullptr);

  // Leave stdout open after the helper exits: the reader cannot observe EOF before the next share.
  const std::string holder_ready_name = "Local\\BuffrXLinkHolderReady-" + std::to_string(GetCurrentProcessId());
  const std::string helper_dead_name = "Local\\BuffrXLinkHelperDead-" + std::to_string(GetCurrentProcessId());
  HANDLE holder_ready = CreateEventA(nullptr, TRUE, FALSE, holder_ready_name.c_str());
  HANDLE helper_dead = CreateEventA(nullptr, TRUE, FALSE, helper_dead_name.c_str());
  check(holder_ready && helper_dead, "the dead-helper test events can be created");
  const std::string dead_root = root + "\\dead-ready";
  CreateDirectoryA(dead_root.c_str(), nullptr);
  const std::string marker = dead_root + "\\started";
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER", "dead-ready");
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_MARKER", marker.c_str());
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_HOLDER_READY", holder_ready_name.c_str());
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER_DEAD", helper_dead_name.c_str());
  if (holder_ready && helper_dead) {
    bx::LinkService service(dead_root, executable);
    bx::Json first;
    std::string error;
    const bool shared = service.share(file, 10, first, error);
    check(shared, "the helper serves a link before exiting with stdout held open");
    if (shared) {
      const bool exited = WaitForSingleObject(helper_dead, 5000) == WAIT_OBJECT_0;
      check(exited, "the helper process has exited while its stdout is still open");
      if (exited) {
        bx::Json again;
        check(service.share(file, 10, again, error) && !again["existing"].as_bool(),
              "sharing the same file restarts a dead helper instead of reusing its link");
      }
    }
  }
  if (holder_ready) CloseHandle(holder_ready);
  if (helper_dead) CloseHandle(helper_dead);
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_MARKER", nullptr);
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_HOLDER_READY", nullptr);
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER_DEAD", nullptr);

  // Keep an expiring link visible with an active viewer until the replacement helper says ready.
  const std::string expiry_root = root + "\\expiry";
  CreateDirectoryA(expiry_root.c_str(), nullptr);
  const std::string expired_id(32, 'a');
  const int64_t expires = static_cast<int64_t>(std::time(nullptr)) + 3;
  bx::Json stored = bx::Json::object();
  stored.set("key", bx::Json(bx::protect_secret(std::string(64, 'b'))));
  bx::Json old = bx::Json::object();
  old.set("id", bx::Json(expired_id));
  old.set("token", bx::Json(bx::protect_secret(std::string(32, 'c'))));
  old.set("file", bx::Json(file));
  old.set("name", bx::Json("clip.mp4"));
  old.set("size", bx::Json(static_cast<int64_t>(16)));
  old.set("expires", bx::Json(expires));
  bx::Json links = bx::Json::array();
  links.push(old);
  stored.set("links", links);
  FILE* saved = std::fopen((expiry_root + "\\links.json").c_str(), "wb");
  check(saved != nullptr, "the expiring link fixture can be written");
  if (saved) {
    const std::string json = stored.dump();
    std::fwrite(json.data(), 1, json.size(), saved);
    std::fclose(saved);
    SetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER", "expiry");
    SetEnvironmentVariableA("BUFFRX_LINK_TEST_EXPIRED_ID", expired_id.c_str());
    const std::string expiry_text = std::to_string(expires);
    SetEnvironmentVariableA("BUFFRX_LINK_TEST_EXPIRY", expiry_text.c_str());
    const std::string fresh_file = paths.clips_dir + "\\fresh.mp4";
    write_bytes(fresh_file, 16);
    bx::LinkService service(expiry_root, executable);
    bx::Json reply;
    std::string error;
    const bool shared = service.share(fresh_file, 10, reply, error);
    check(shared, "the replacement helper starts after the older link expires");
    if (shared) {
      check(wait_for_watching(service, reply["id"].as_string(), 1), "the helper serves the new link");
      check(watching(service, expired_id) == 1, "the expired link is not served again when the helper becomes ready");
    }
  }
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_EXPIRED_ID", nullptr);
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_EXPIRY", nullptr);
  SetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER", nullptr);

  // A resolved short link is written the moment the service answers. A restart before the next change used
  // to hand out the long link again, which is the symptom this keeps fixed.
  const std::string short_root = root + "\\short";
  CreateDirectoryA(short_root.c_str(), nullptr);
  const std::string short_id(32, 'd');
  const std::string short_file = paths.clips_dir + "\\short.mp4";
  write_bytes(short_file, 16);
  const std::string short_url = "https://buffrx.app/s/abcdefghjk";
  const std::string short_ticket = "payload?n=Test";
  bx::Json keep = bx::Json::object();
  keep.set("key", bx::Json(bx::protect_secret(std::string(64, 'e'))));
  bx::Json one = bx::Json::object();
  one.set("id", bx::Json(short_id));
  one.set("token", bx::Json(bx::protect_secret(std::string(32, 'f'))));
  one.set("file", bx::Json(short_file));
  one.set("name", bx::Json("short.mp4"));
  one.set("size", bx::Json(static_cast<int64_t>(16)));
  one.set("created", bx::Json(static_cast<int64_t>(std::time(nullptr))));
  one.set("expires", bx::Json(static_cast<int64_t>(std::time(nullptr)) + 3600));
  bx::Json kept_links = bx::Json::array();
  kept_links.push(one);
  keep.set("links", kept_links);
  FILE* kept = std::fopen((short_root + "\\links.json").c_str(), "wb");
  check(kept != nullptr, "the short-link fixture can be written");
  if (kept) {
    const std::string json = keep.dump();
    std::fwrite(json.data(), 1, json.size(), kept);
    std::fclose(kept);
    {
      bx::LinkService service(short_root, executable);
      service.set_short_url(short_id, short_url, short_ticket);
    }
    {
      bx::LinkService service(short_root, executable);
      const bx::Json state = service.list();
      std::string url;
      for (const bx::Json& link : state["links"].items()) {
        if (link["id"].as_string() == short_id) url = link["url"].as_string();
      }
      check(url == short_url, "a short link written once is the link a restarted engine hands out");
    }
    std::string stored_text;
    if (FILE* read_back = std::fopen((short_root + "\\links.json").c_str(), "rb")) {
      char buffer[4096] = {0};
      const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, read_back);
      stored_text.assign(buffer, read);
      std::fclose(read_back);
    }
    bx::Json stored_json;
    std::string stored_error;
    check(bx::Json::parse(stored_text, stored_json, stored_error), "links.json reads back");
    std::string stored_fragment;
    for (const bx::Json& link : stored_json["links"].items()) {
      if (link["id"].as_string() == short_id) stored_fragment = link["short_fragment"].as_string();
    }
    std::string opened_ticket;
    check(stored_text.find(short_ticket) == std::string::npos,
          "the ticket the code was made from is not kept in plaintext");
    check(bx::open_short_secret(stored_fragment, true, opened_ticket) == bx::ShortSecret::Sealed &&
              opened_ticket == short_ticket,
          "the sealed ticket opens to the same ticket, so a stop after a restart revokes the same code");
  }
}

// A plain chunk for the store tests: the fields a manual recording fills in.
bool index_test_chunk(bx::SessionEntry& entry, const std::string& file, uint32_t start_ms, uint32_t end_ms,
                      std::string& error) {
  bx::SessionChunk chunk;
  chunk.file = file;
  chunk.start_ms = start_ms;
  chunk.end_ms = end_ms;
  return bx::session_store_append_chunk(entry, chunk, error);
}

void test_chunk_math() {
  check(bx::chunk_seconds_clamped(30, 2) == 30, "chunk seconds pass through when already aligned");
  check(bx::chunk_seconds_clamped(3, 2) == 5, "chunk seconds clamp up to five");
  check(bx::chunk_seconds_clamped(900, 2) == 600, "chunk seconds clamp down to six hundred");
  check(bx::chunk_seconds_clamped(31, 2) == 32, "chunk seconds align up to the keyframe interval");
  check(bx::chunk_seconds_clamped(45, 1) == 45, "a one second keyframe interval accepts any length");

  std::vector<bx::SessionChunk> chunks;
  chunks.push_back({"a.mkv", 100, 0, 30000});
  chunks.push_back({"b.mkv", 100, 30000, 60000});
  chunks.push_back({"c.mkv", 100, 60000, 90000});
  size_t first = 0;
  size_t last = 0;
  check(bx::chunk_range_for_window(chunks, 1000, 2000, first, last) && first == 0 && last == 0,
        "a window inside one chunk resolves to that chunk");
  check(bx::chunk_range_for_window(chunks, 1000, 70000, first, last) && first == 0 && last == 2,
        "a window spanning chunks resolves to first and last");
  check(!bx::chunk_range_for_window(chunks, 900000, 901000, first, last),
        "a window past the end resolves to nothing");

  // Task 7: a window of session time becomes the files to stream-copy and where to cut them.
  bx::ConcatPlan plan;
  check(bx::concat_plan_for_range(chunks, 1000, 59000, plan) && plan.files.size() == 2 &&
            plan.files.front() == "a.mkv" && plan.files.back() == "b.mkv",
        "a range across a boundary concatenates the chunks it touches");
  check(plan.start_in_first == 1.0 && plan.end_in_last == 29.0, "the cut points are local to their chunk");
  check(bx::concat_plan_for_range(chunks, 1000, 70000, plan) && plan.files.size() == 3,
        "a three chunk range concatenates three files");
  check(bx::concat_plan_for_range(chunks, 0, 999999, plan) && plan.start_in_first == 0.0 && plan.end_in_last == 30.0,
        "a range past both ends is clamped to the footage");
  check(!bx::concat_plan_for_range(chunks, 5000, 5000, plan), "an empty range exports nothing");
  check(!bx::concat_plan_for_range(chunks, 900000, 901000, plan), "a range outside the session exports nothing");

  // What the tick does once libobs reports a closed chunk. The signal itself only raises a flag.
  using bx::ChunkClosedAction;
  check(bx::chunk_closed_action(false, false) == ChunkClosedAction::finish_single,
        "a recording with no session just finishes");
  check(bx::chunk_closed_action(false, true) == ChunkClosedAction::finish_single,
        "a rotation without a session still just finishes");
  check(bx::chunk_closed_action(true, true) == ChunkClosedAction::rotate, "a boundary opens the next chunk");
  check(bx::chunk_closed_action(true, false) == ChunkClosedAction::close_session,
        "a stop closes the session");
}

// Commands arrive on pipe threads while the main thread ticks the same libobs objects; only commands
// that never touch them may run outside the engine lock.
// A profile is a saved config, but some settings belong to the machine, not to a game: applying a
// profile must never move the library or change retention.
void test_profile_patch() {
  bx::Config saved;
  saved.buffer_seconds = 30;
  saved.clips_dir = "D:\\Old";
  saved.performance_when = bx::PerformanceWhen::games;
  saved.performance_games.push_back("cs2");
  const bx::Json patch = bx::profile_patch(bx::config_to_json(saved));
  check(patch.has("buffer_seconds"), "a profile keeps its capture and encoding settings");
  check(!patch.has("clips_dir") && !patch.has("screenshots_dir"), "a profile never moves the library");
  check(!patch.has("recordings_budget_gb") && !patch.has("min_free_gb"), "a profile never changes retention");
  check(patch.has("capture_fps") && patch.has("quality") && patch.has("custom_kbps") && patch.has("codec"),
        "a profile carries frame rate, quality and codec");
  check(patch.has("video_preset"), "a profile carries the Video preset");
  check(!patch.has("performance_when") && !patch.has("performance_games"),
        "a profile never carries Performance mode");
  {
    // A profile saved before quality levels still carries its tier, and applying it still works.
    bx::Json old_profile = bx::Json::object();
    old_profile.set("quality_tier", bx::Json("quality"));
    bx::Config applied;
    bx::config_from_json(bx::profile_patch(old_profile), applied);
    check(applied.quality == "highest", "an old profile's tier is applied as its quality level");
  }
  check(!patch.has("hotkey_save") && !patch.has("mic_device") && !patch.has("theme") && !patch.has("notifications") &&
            !patch.has("active_profile"),
        "a profile never changes hotkeys, devices, the cockpit, notifications or which profile is active");

  bx::Config live;
  live.buffer_seconds = 45;
  live.clips_dir = "E:\\Clips";
  bx::config_from_json(patch, live);
  check(live.buffer_seconds == 30 && live.clips_dir == "E:\\Clips",
        "applying the filtered profile changes the profile's settings and nothing else");
}

void test_command_policy() {
  check(bx::command_needs_engine_lock("capture.set"), "swapping the capture source holds the engine lock");
  check(bx::command_needs_engine_lock("config.set"), "a settings change holds the engine lock");
  check(bx::command_needs_engine_lock("status.get"), "reading live stats holds the engine lock");
  check(bx::command_needs_engine_lock("recording.start"), "starting a recording holds the engine lock");
  check(bx::command_needs_engine_lock("not.a.command"), "an unknown command is locked by default");
  check(!bx::command_needs_engine_lock("clips.list"), "listing the library does not stall the tick");
  check(!bx::command_needs_engine_lock("clips.export_range"), "a range export does not stall the tick");
  check(!bx::command_needs_engine_lock("ui.frame_stats"), "reading the frame numbers does not wait for the tick");
  check(!bx::command_needs_engine_lock("timeline.edit_source"), "reading editor footage never holds the engine lock");
  check(!bx::config_change_waits_for_recording(false, false, true), "a change that rebuilds nothing runs during a recording");
  check(bx::config_change_waits_for_recording(true, false, true), "a rebuild waits for a manual recording to end");
  check(bx::config_change_waits_for_recording(false, true, true), "switching Performance mode waits for a manual recording to end");
  check(!bx::config_change_waits_for_recording(true, false, false), "a rebuild is free when nothing records");
}

// Task 14: the adaptive quality ladder, as policy. Ticks are 250 ms apart.

void test_session_store() {
  using bx::SessionEntry;
  const std::string root = temp_directory() + "\\sessions";
  bx::session_store_init(root);
  std::string error;

  SessionEntry session;
  check(bx::session_store_begin("recording", session, error), "a session begins");
  check(!session.id.empty() && !session.closed, "a new session is open and named");

  const std::string chunk_a = bx::session_media_dir(session.id) + "\\chunk-a.mkv";
  write_bytes(chunk_a, 1024);
  check(index_test_chunk(session, chunk_a, 1000, 31000, error), "the first chunk is recorded");
  check(bx::session_store_add_marker(session, "clip", "BuffrX 20s", 21000, error), "a marker is recorded");
  check(bx::session_store_close(session, 60000, error), "the session closes");

  SessionEntry loaded;
  check(bx::session_store_load_one(session.id, loaded, error), "the session loads again");
  check(loaded.closed && !loaded.interrupted, "a closed session is not interrupted");
  check(loaded.chunks.size() == 1 && loaded.chunks[0].bytes == 1024, "the chunk and its measured size survive");
  check(loaded.markers.size() == 1 && loaded.markers[0].kind == "clip" && loaded.markers[0].at_ms == 21000,
        "the marker survives");
  check(!bx::session_store_remove_marker(loaded, 21000, "marker", "BuffrX 20s", error),
        "removing a different marker leaves the original");
  check(bx::session_store_remove_marker(loaded, 21000, "clip", "BuffrX 20s", error),
        "the exact marker is removed");
  SessionEntry without_marker;
  check(bx::session_store_load_one(session.id, without_marker, error) && without_marker.markers.empty() &&
            without_marker.chunks.size() == 1,
        "the removal persists without changing the footage");
  check(loaded.bytes() == 1024, "the session total is the sum of its chunks");

  SessionEntry crashed;
  check(bx::session_store_begin("recording", crashed, error), "a second session begins");
  check(crashed.id != session.id, "a second session in the same second gets its own directory");
  const std::string chunk_b = bx::session_media_dir(crashed.id) + "\\chunk-b.mkv";
  write_bytes(chunk_b, 2048);
  check(index_test_chunk(crashed, chunk_b, 1000, 31000, error),
        "its chunk is recorded while it stays open");

  const std::vector<SessionEntry> all = bx::session_store_load(error);
  check(all.size() == 2, "both sessions are listed");
  bool ordered = true;
  for (size_t index = 1; index < all.size(); ++index) {
    if (all[index - 1].started_ms < all[index].started_ms) ordered = false;
  }
  check(ordered, "sessions come back newest first");
  bool kept_chunks = false;
  for (const SessionEntry& entry : all) {
    if (entry.id == crashed.id) {
      kept_chunks = entry.interrupted && entry.chunks.size() == 1 && entry.bytes() == 2048;
    }
  }
  check(kept_chunks, "an unclosed session comes back interrupted with its chunks kept");

  SessionEntry escaped;
  escaped.id = "..\\outside";
  check(!bx::session_store_delete(escaped, error), "a session id that would leave the root is refused");
  check(!bx::session_id_is_safe("..\\outside") && bx::session_id_is_safe("BuffrX 2026-10-18 14-03-22"),
        "session ids are checked before they become directory names");

  // A tampered index must not turn a delete into a file shredder.
  const std::string keep_file = temp_directory() + "\\outside-session.txt";
  write_bytes(keep_file, 16);
  SessionEntry tampered;
  tampered.id = "BuffrX 2026-10-18 14-03-22";
  tampered.chunks.push_back({keep_file, 16, 0, 1000});
  check(bx::session_store_delete(tampered, error), "a session with only an outside chunk still deletes");
  check(GetFileAttributesA(keep_file.c_str()) != INVALID_FILE_ATTRIBUTES,
        "a chunk path outside the session is left alone");

  // A text prefix is not containment: a chunk path inside the session by spelling but holding `..`
  // resolved outside it, so a tampered index could delete any file the account can reach.
  const std::string traversal_id = "BuffrX 2026-01-01 00-00-00-traversal";
  const std::string traversal_media = bx::session_media_dir(traversal_id);
  CreateDirectoryA(traversal_media.c_str(), nullptr);
  const std::string traversal_victim = temp_directory() + "\\outside-traversal.txt";
  write_bytes(traversal_victim, 16);
  SessionEntry traversal;
  traversal.id = traversal_id;
  traversal.chunks.push_back({traversal_media + "\\..\\..\\..\\outside-traversal.txt", 16, 0, 1000});
  check(bx::session_store_delete(traversal, error), "a session with a parent-component chunk still deletes");
  check(GetFileAttributesA(traversal_victim.c_str()) != INVALID_FILE_ATTRIBUTES,
        "a chunk path with .. cannot reach outside the sessions root");

  // The id inside the index is what builds this session's thumbs and media paths, so an index that
  // names another folder is refused by the loader every reader goes through.
  const std::string tampered_index = bx::session_store_root() + "\\" + session.id + "\\index.json";
  const FileStamp index_text = file_stamp(tampered_index);
  bx::Json tampered_json = bx::session_entry_json(session);
  tampered_json.set("id", bx::Json("..\\..\\outside"));
  if (FILE* file = std::fopen(tampered_index.c_str(), "wb")) {
    const std::string text = tampered_json.dump();
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
  }
  SessionEntry tampered_id_entry;
  check(!bx::session_store_load_one(session.id, tampered_id_entry, error),
        "an index whose own id would leave its folder is refused");
  if (FILE* file = std::fopen(tampered_index.c_str(), "wb")) {
    std::fwrite(index_text.bytes.data(), 1, index_text.bytes.size(), file);
    std::fclose(file);
  }

  // Markers live in their own file beside the index: a marker operation must not rewrite (or re-mtime)
  // the index, which is the file every chunk append already rewrites.
  SessionEntry marker_session;
  check(bx::session_store_begin("recording", marker_session, error), "a session begins for the marker file");
  const std::string marker_chunk = bx::session_media_dir(marker_session.id) + "\\chunk-marker.mkv";
  write_bytes(marker_chunk, 777);
  check(index_test_chunk(marker_session, marker_chunk, 0, 5000, error), "its chunk is indexed");
  const std::string marker_index_path = bx::session_store_root() + "\\" + marker_session.id + "\\index.json";
  const std::string marker_sidecar_path = bx::session_store_root() + "\\" + marker_session.id + "\\markers.json";
  const FileStamp index_before = file_stamp(marker_index_path);
  check(bx::session_store_add_marker(marker_session, "clip", "one", 1000, error), "a marker is added");
  const FileStamp index_after_add = file_stamp(marker_index_path);
  check(!index_before.bytes.empty() && index_before.bytes == index_after_add.bytes &&
            index_before.mtime == index_after_add.mtime,
        "adding a marker leaves the chunk index byte for byte, and untimed");
  check(GetFileAttributesA(marker_sidecar_path.c_str()) != INVALID_FILE_ATTRIBUTES,
        "the marker goes into its own file");

  // Duplicates are real: two markers at the same millisecond with the same kind and label stay two.
  check(bx::session_store_add_marker(marker_session, "clip", "one", 1000, error), "the same marker is added again");
  SessionEntry marker_loaded;
  check(bx::session_store_load_one(marker_session.id, marker_loaded, error) && marker_loaded.markers.size() == 2,
        "duplicate markers are both kept");
  check(bx::session_store_remove_marker(marker_loaded, 1000, "clip", "one", error), "one duplicate is removed");
  SessionEntry marker_after_remove;
  check(bx::session_store_load_one(marker_session.id, marker_after_remove, error) &&
            marker_after_remove.markers.size() == 1 && marker_after_remove.chunks.size() == 1,
        "removing one duplicate keeps the other and the footage");
  const FileStamp index_after_remove = file_stamp(marker_index_path);
  check(index_after_remove.bytes == index_before.bytes && index_after_remove.mtime == index_before.mtime,
        "removing a marker leaves the chunk index alone too");

  // A session from before the sidecar keeps its markers inline; the first marker operation migrates them.
  SessionEntry legacy;
  check(bx::session_store_begin("recording", legacy, error), "a session begins for the migration");
  const std::string legacy_chunk = bx::session_media_dir(legacy.id) + "\\chunk-legacy.mkv";
  write_bytes(legacy_chunk, 64);
  legacy.markers.push_back({100, "clip", "inline", ""});
  check(index_test_chunk(legacy, legacy_chunk, 0, 1000, error), "an inline marker is written by the index");
  SessionEntry legacy_loaded;
  check(bx::session_store_load_one(legacy.id, legacy_loaded, error) && legacy_loaded.markers.size() == 1 &&
            legacy_loaded.markers[0].label == "inline",
        "a session with no sidecar still reads its index's markers");
  check(bx::session_store_add_marker(legacy_loaded, "clip", "newer", 2000, error), "a new marker is added");
  SessionEntry legacy_migrated;
  check(bx::session_store_load_one(legacy.id, legacy_migrated, error) && legacy_migrated.markers.size() == 2,
        "the inline marker is carried into the sidecar with the new one");

  // A sidecar cut short by a crash is ignored: only markers come from it, never a chunk.
  const std::string legacy_sidecar = bx::session_store_root() + "\\" + legacy.id + "\\markers.json";
  FILE* cut = std::fopen(legacy_sidecar.c_str(), "wb");
  if (cut) {
    std::fputs("{\"markers\":[{\"at_ms\":2000,\"kind\":\"cl", cut);
    std::fclose(cut);
  }
  SessionEntry truncated;
  check(bx::session_store_load_one(legacy.id, truncated, error) && truncated.chunks.size() == 1,
        "a truncated sidecar never loses a chunk");
  check(truncated.markers.size() == 1 && truncated.markers[0].label == "inline",
        "and the index's own markers are used instead");

  // Always verify a long session's markers. The disk-speed budget is an explicit benchmark,
  // because a shared CI runner's storage cannot establish a real-hardware performance claim.
  SessionEntry many;
  check(bx::session_store_begin("timeline", many, error), "a session begins for many markers");
  const DWORD marker_clock = GetTickCount();
  bool every_marker = true;
  for (int i = 0; i < 2000; ++i) {
    if (!bx::session_store_add_marker(many, "marker", "", static_cast<uint32_t>(i), error)) every_marker = false;
  }
  const DWORD marker_elapsed = GetTickCount() - marker_clock;
  check(every_marker, "two thousand markers are recorded");
  const char* performance = std::getenv("BUFFRX_TEST_PERFORMANCE");
  const bool enforce_marker_budget = performance && std::strcmp(performance, "1") == 0;
  std::printf("  marker benchmark: 2000 writes in %lu ms (five-second budget %s)\n",
              static_cast<unsigned long>(marker_elapsed), enforce_marker_budget ? "enforced" : "not requested");
  if (enforce_marker_budget)
    check(marker_elapsed < 5000, "two thousand markers are recorded in under five seconds");
  SessionEntry many_loaded;
  check(bx::session_store_load_one(many.id, many_loaded, error) && many_loaded.markers.size() == 2000,
        "every one of them comes back");
  check(bx::session_store_close(many, 3000, error), "the marked session closes");
  const std::string many_sidecar = bx::session_store_root() + "\\" + many.id + "\\markers.json";
  check(GetFileAttributesA(many_sidecar.c_str()) == INVALID_FILE_ATTRIBUTES,
        "closing folds the markers into the index and removes the sidecar");
  SessionEntry many_closed;
  check(bx::session_store_load_one(many.id, many_closed, error) && many_closed.markers.size() == 2000,
        "the closed session still has every marker");

  // Erasing a timeline session takes its marker file with it.
  SessionEntry erased_markers;
  check(bx::session_store_begin("timeline", erased_markers, error), "a timeline session begins for the erase");
  check(bx::session_store_add_marker(erased_markers, "clip", "x", 1, error), "it has a marker");
  const std::string erased_sidecar = bx::session_store_root() + "\\" + erased_markers.id + "\\markers.json";
  std::vector<std::string> marker_locked;
  check(bx::session_store_erase_timeline(erased_markers.id, marker_locked, error) && marker_locked.empty(),
        "the marked timeline session erases");
  check(GetFileAttributesA(erased_sidecar.c_str()) == INVALID_FILE_ATTRIBUTES,
        "the erase removes the marker file with the session");

  // A marker the sidecar refuses must not stay in memory: written into the index by the next chunk
  // append it would be invisible to a sidecar-preferring reader, and a retry would duplicate it. The
  // sidecar is held open with no sharing, which is exactly how an antivirus scan or a read-only folder
  // makes the atomic replace fail.
  SessionEntry refused;
  check(bx::session_store_begin("recording", refused, error), "a session begins for a failed marker write");
  check(bx::session_store_add_marker(refused, "clip", "first", 500, error), "its first marker is written");
  const std::string refused_sidecar = bx::session_store_root() + "\\" + refused.id + "\\markers.json";
  HANDLE held_sidecar =
      CreateFileA(refused_sidecar.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(held_sidecar != INVALID_HANDLE_VALUE, "the marker file can be held open for the test");
  const size_t refused_before = refused.markers.size();
  const bool refused_add = bx::session_store_add_marker(refused, "clip", "second", 1500, error);
  check(!refused_add && !error.empty(), "a marker the sidecar refuses is reported as not written");
  check(refused.markers.size() == refused_before, "and it is not left in the caller's session");
  if (held_sidecar != INVALID_HANDLE_VALUE) CloseHandle(held_sidecar);
  error.clear();
  SessionEntry refused_loaded;
  check(bx::session_store_load_one(refused.id, refused_loaded, error) && refused_loaded.markers.size() == 1 &&
            refused_loaded.markers[0].label == "first",
        "the session on disk still has only the marker that was written");

  // A marker carries a stable id of its own, so two identical markers are told apart. It is written only
  // when there is one: a marker from before ids keeps an empty one, and no file is rewritten to give it one.
  SessionEntry ids;
  check(bx::session_store_begin("timeline", ids, error), "a session begins for marker ids");
  check(bx::session_store_add_marker(ids, "marker", "", 7000, error), "the first of two identical markers is added");
  const std::string first_marker_id = ids.markers.back().id;
  check(bx::session_store_add_marker(ids, "marker", "", 7000, error), "the second of two identical markers is added");
  const std::string second_marker_id = ids.markers.back().id;
  check(!first_marker_id.empty() && !second_marker_id.empty() && first_marker_id != second_marker_id,
        "two identical markers get different ids");
  check(!bx::session_store_remove_marker(ids, std::string(), error), "an empty marker id removes nothing");
  error.clear();
  check(!bx::session_store_remove_marker(ids, "no such marker", error) && !error.empty(),
        "an unknown marker id removes nothing and says so");
  error.clear();
  check(ids.markers.size() == 2, "the failed removals leave both markers");
  check(bx::session_store_remove_marker(ids, first_marker_id, error), "the first identical marker is removed by its id");
  check(ids.markers.size() == 1 && ids.markers[0].id == second_marker_id,
        "exactly the other identical marker is left");
  SessionEntry ids_loaded;
  check(bx::session_store_load_one(ids.id, ids_loaded, error) && ids_loaded.markers.size() == 1 &&
            ids_loaded.markers[0].id == second_marker_id && ids_loaded.markers[0].at_ms == 7000,
        "the id survives a reload, and the removal with it");
  // The old identity still removes: an old client never sees an id.
  check(bx::session_store_remove_marker(ids_loaded, 7000, "marker", "", error),
        "the old identity still removes a marker");
  check(bx::session_store_load_one(ids.id, ids_loaded, error) && ids_loaded.markers.empty(),
        "and that removal persists");
  // A marker written by the index before ids existed has none, and loading never invents one.
  SessionEntry no_ids;
  no_ids.markers.push_back({100, "clip", "inline", ""});
  check(no_ids.markers[0].id.empty(), "a marker from before ids keeps an empty id");

  // Retention: three closed sessions of 1 GiB plus one that is still open.
  auto make_session = [](const std::string& id, uint32_t started, bool closed) {
    SessionEntry entry;
    entry.id = id;
    entry.started_ms = started;
    entry.started_at = started;
    entry.closed = closed;
    entry.chunks.push_back({id + ".mkv", 1024LL * 1024 * 1024, started, started + 1000});
    return entry;
  };
  std::vector<SessionEntry> budget_sessions;
  budget_sessions.push_back(make_session("oldest", 1000, true));
  budget_sessions.push_back(make_session("middle", 2000, true));
  budget_sessions.push_back(make_session("newest", 3000, true));
  budget_sessions.push_back(make_session("open", 4000, false));

  check(bx::retention_prune_order(budget_sessions, 0).empty(), "a zero budget deletes nothing");
  check(bx::retention_prune_order(budget_sessions, 4LL * 1024 * 1024 * 1024).empty(),
        "a budget above the total deletes nothing");
  const std::vector<size_t> order = bx::retention_prune_order(budget_sessions, 2LL * 1024 * 1024 * 1024);
  check(order.size() == 2 && order[0] == 0 && order[1] == 1,
        "a small budget removes the oldest closed sessions first");
  bool newest_kept = true;
  bool open_kept = true;
  for (size_t index : order) {
    if (index == 2) newest_kept = false;
    if (index == 3) open_kept = false;
  }
  check(newest_kept, "the newest session is never pruned");
  check(open_kept, "an open session is never pruned");

  // Task 8: the tick clock restarts at every boot, so age is the wall clock (or the id, which is a
  // local timestamp, for indexes written before started_at existed).
  std::vector<SessionEntry> rebooted;
  rebooted.push_back(make_session("BuffrX 2026-10-17 20-00-00", 90000000, true));  // yesterday, high tick
  rebooted.push_back(make_session("BuffrX 2026-10-18 09-00-00", 1000, true));      // today, after a reboot
  rebooted.push_back(make_session("BuffrX 2026-10-18 10-00-00", 2000, true));
  rebooted[0].started_at = 1792267200;
  rebooted[1].started_at = 1792310400;
  rebooted[2].started_at = 1792314000;
  const std::vector<size_t> after_reboot = bx::retention_prune_order(rebooted, 2LL * 1024 * 1024 * 1024);
  check(after_reboot.size() == 1 && after_reboot[0] == 0, "after a reboot the oldest by wall clock goes first");
  check(bx::session_older(rebooted[0], rebooted[1]) && !bx::session_older(rebooted[2], rebooted[1]),
        "session age follows the wall clock, not the tick");
  rebooted[0].started_at = 0;
  rebooted[1].started_at = 0;
  check(bx::session_older(rebooted[0], rebooted[1]), "an index without a wall clock is aged by its id");

  // The guard refuses a session that could not be finished.
  std::string reason;
  check(!bx::recording_space_ok("D:\\clips", 1LL * 1024 * 1024 * 1024, 2, reason) && !reason.empty(),
        "under the floor the guard refuses, with a reason");
  check(bx::recording_space_ok("D:\\clips", 3LL * 1024 * 1024 * 1024, 2, reason), "above the floor it allows");
  check(bx::recording_space_ok("D:\\clips", 1, 0, reason), "a zero floor never refuses");
  check(bx::recording_space_ok("D:\\clips", -1, 2, reason), "an unreadable volume does not block recording");

  // The floor is asked on the volume recordings go to, not on the clips drive: a recording whose own folder is on
  // another drive was refused because the *clips* drive was short, and that other drive could fill up with nothing said.
  // Every start path asks this one guard (recording_space.h), so it is the choice that is checked, not one call site.
  {
    bx::Config starts;
    starts.clips_dir = "C:\\clips";
    starts.recordings_dir = "R:\\footage";
    starts.min_free_gb = 5;
    std::string asked;
    const auto free_bytes = [&asked, &starts](const std::string& path) {
      asked = path;
      return path == starts.recordings_dir ? 1LL * 1024 * 1024 * 1024 : 100LL * 1024 * 1024 * 1024;
    };
    check(!bx::recording_space_guard(starts, free_bytes, reason) && asked == starts.recordings_dir,
          "the guard asks the recordings folder's volume, never the clips drive");
    check(reason.find("R:\\footage") != std::string::npos,
          "and names the volume the recording would have gone to");
    starts.min_free_gb = 0;
    check(bx::recording_space_guard(starts, free_bytes, reason) && asked == starts.recordings_dir,
          "a recordings folder on that drive is allowed when it has room");
    // With no recordings folder of its own, recordings live in the clips folder: that is the volume asked.
    starts.recordings_dir.clear();
    starts.min_free_gb = 5;
    check(bx::recording_space_guard(starts, free_bytes, reason) && asked == starts.clips_dir,
          "with no separate folder the clips volume is the one asked");
    check(starts.min_free_gb == 5 && starts.recordings_dir.empty(), "and asking changes nothing in the config");
  }

  // A chunk's size is only final once the muxer's child process has closed the file, so the index is
  // refreshed from disk when the session closes and again when it is loaded.
  bx::SessionEntry sized;
  check(bx::session_store_begin("recording", sized, error), "a session begins for the size check");
  const std::string grown = bx::session_media_dir(sized.id) + "\\grows.mkv";
  write_bytes(grown, 100);
  check(index_test_chunk(sized, grown, 0, 1000, error), "a chunk is indexed while it is small");
  write_bytes(grown, 4096);
  check(bx::session_store_close(sized, 2000, error), "the session closes");
  bx::SessionEntry refreshed;
  check(bx::session_store_load_one(sized.id, refreshed, error), "the closed session loads");
  check(refreshed.chunks.size() == 1 && refreshed.chunks[0].bytes == 4096,
        "closing a session refreshes a chunk's size from disk");
  write_bytes(grown, 8192);
  bx::SessionEntry reloaded;
  check(bx::session_store_load_one(sized.id, reloaded, error) && reloaded.chunks[0].bytes == 8192,
        "loading a session refreshes a chunk's size from disk");

  // Task 8: pruning on disk. Three closed sessions, one crashed, one still being recorded; the budget
  // fits one session. Order on disk is by started_at.
  const std::string prune_root = temp_directory() + "\\prune";
  bx::session_store_init(prune_root);
  auto disk_session = [&error](int64_t started_at, int64_t size, bool close) {
    SessionEntry entry;
    bx::session_store_begin("recording", entry, error);
    entry.started_at = started_at;
    const std::string file = bx::session_media_dir(entry.id) + "\\chunk.mkv";
    write_bytes(file, static_cast<size_t>(size));
    index_test_chunk(entry, file, 0, 1000, error);
    if (close) bx::session_store_close(entry, 2000, error);
    return entry;
  };
  const SessionEntry p_old = disk_session(100, 1000, true);
  const SessionEntry p_crashed = disk_session(200, 1000, false);
  const SessionEntry p_mid = disk_session(300, 1000, true);
  const SessionEntry p_new = disk_session(400, 1000, true);
  const SessionEntry p_live = disk_session(500, 1000, false);
  std::vector<std::string> removed;
  int64_t freed = 0;
  int64_t total = 0;
  check(bx::session_prune(0, p_live.id, removed, freed, total, error) && removed.empty() && total == 5000,
        "a zero budget prunes nothing and reports the total");
  check(bx::session_prune(2500, p_live.id, removed, freed, total, error), "pruning to a budget succeeds");
  auto removed_has = [&removed](const std::string& id) {
    return std::find(removed.begin(), removed.end(), id) != removed.end();
  };
  check(removed.size() == 3 && removed_has(p_old.id) && removed_has(p_crashed.id) && removed_has(p_mid.id),
        "the oldest sessions go first, a crashed one included");
  check(!removed_has(p_new.id) && !removed_has(p_live.id), "the newest and the live session stay");
  check(freed == 3000 && total == 2000, "freed and remaining bytes are reported");
  SessionEntry gone;
  check(!bx::session_store_load_one(p_old.id, gone, error), "a pruned session is gone from disk");
  check(bx::session_store_load_one(p_live.id, gone, error), "the live session is still on disk");

  // A crash between a chunk's rename to "<file>.expired" and its delete leaves a file the index no
  // longer names; the retention pass sweeps it and the emptied session goes with it.
  SessionEntry p_leftover;
  check(bx::session_store_begin("timeline", p_leftover, error), "a timeline session begins for the leftover check");
  const std::string leftover_file = bx::session_media_dir(p_leftover.id) + "\\leftover.mp4";
  write_bytes(leftover_file, 512);
  check(index_test_chunk(p_leftover, leftover_file, 0, 1000, error), "the leftover chunk is indexed");
  check(bx::session_store_close(p_leftover, 2000, error), "the leftover session closes");
  const int64_t leftover_end = p_leftover.chunks.front().wall_end_ms;
  std::vector<bx::DroppedChunk> leftover_jobs;
  check(bx::session_store_drop_chunks(p_leftover, {0}, leftover_jobs, error), "the closed chunk drops from the index");
  check(GetFileAttributesA((leftover_file + ".expired").c_str()) != INVALID_FILE_ATTRIBUTES,
        "the drop renames the footage before it is deleted");
  check(bx::session_store_prune_timeline(30, leftover_end + 30 * 60000 + 1, error, ""),
        "the retention pass succeeds");
  check(GetFileAttributesA((leftover_file + ".expired").c_str()) == INVALID_FILE_ATTRIBUTES,
        "retention sweeps footage an interrupted prune renamed");
  check(!bx::session_store_load_one(p_leftover.id, gone, error), "the emptied session is gone");

  // A clips folder spelled with forward slashes is normal (the setting has been typed and normalised that way), and the
  // splitter reports the footage file with backslashes. The sweep must still recognise its own files: when it did not,
  // it dropped chunks from the index without renaming or deleting their files, and the footage stayed on disk for ever.
  bx::session_store_init(temp_directory() + "/prune-forward");
  bx::SessionEntry p_forward;
  check(bx::session_store_begin("timeline", p_forward, error), "a session begins under a forward-slash root");
  const std::string forward_file = [&] {
    // The splitter answers with one spelling of the path, backslash everywhere, whatever the output folder was set to.
    std::string file = bx::session_media_dir(p_forward.id) + "\\chunk.mp4";
    std::replace(file.begin(), file.end(), '/', '\\');
    return file;
  }();
  write_bytes(forward_file, 512);
  check(index_test_chunk(p_forward, forward_file, 0, 1000, error), "the forward-root chunk is indexed");
  check(bx::session_store_close(p_forward, 2000, error), "the forward-root session closes");
  std::vector<bx::DroppedChunk> forward_jobs;
  check(bx::session_store_drop_chunks(p_forward, {0}, forward_jobs, error), "the forward-root chunk drops");
  check(GetFileAttributesA((forward_file + ".expired").c_str()) != INVALID_FILE_ATTRIBUTES,
        "a forward-slash root still renames the footage it drops");
  check(bx::session_store_prune_timeline(30, 3000, error, ""), "retention sweeps under a forward-slash root");
  check(GetFileAttributesA((forward_file + ".expired").c_str()) == INVALID_FILE_ATTRIBUTES,
        "the footage renamed under a forward-slash root is deleted");
  check(!bx::session_store_load_one(p_forward.id, gone, error), "the emptied forward-root session is gone");

  // A store with no usable root must refuse rather than invent one: an empty path, or a path that is
  // only a drive root, is exactly how a session ends up outside the library.
  bx::session_store_init("");
  bx::SessionEntry rootless;
  check(!bx::session_store_begin("recording", rootless, error), "a store with no root refuses to begin");
  check(rootless.id.empty(), "a refused begin leaves no session behind");
  bx::session_store_init("\\sessions");
  check(!bx::session_store_begin("recording", rootless, error), "a root at the drive root is refused");

  // Task 6: which writes leave a marker in the open session.
  check(bx::marker_kind_for_command("buffer.save") == "clip", "saving a clip is a clip marker");
  check(bx::marker_kind_for_command("screenshot.take") == "screenshot", "a screenshot is a screenshot marker");
  check(bx::marker_kind_for_command("recording.start") == "recording", "starting a recording is a recording marker");
  check(bx::marker_kind_for_command("profiles.apply") == "profile", "applying a profile is a profile marker");
  check(bx::marker_kind_for_command("game.capture") == "game", "a captured game is a game marker");
  check(bx::marker_kind_for_command("status.get").empty(), "reads do not create markers");
  check(bx::marker_kind_for_command("recording.stop").empty(), "a stop closes the session instead of marking it");
}

// One Timeline session, one muxer: the splitter's own `file_changed` decides where a segment begins and ends,
// so these are the rules that keep the session index's spans ordered and honest.
void test_timeline_file_index() {
  std::printf("timeline file index\n");
  bx::TimelineFileIndex index;
  check(!index.active() && index.current().empty(), "a new index has no open file");
  check(!index.begin("", 1000), "an empty file does not begin a span");
  check(!index.change("a.mp4", 1000), "a change with nothing open is nothing");
  check(!index.stop(1000), "a stop with nothing open is nothing");
  check(index.take_closed().empty(), "nothing closed yet");

  check(index.begin("a.mp4", 1000), "the first file begins a span");
  check(index.active() && index.current() == "a.mp4" && index.current_start_ms() == 1000,
        "the open span is the file just begun");
  check(!index.begin("b.mp4", 1200), "a second begin is refused while a span is open");

  check(index.change("b.mp4", 3000), "the splitter's next file closes the first span");
  check(index.current() == "b.mp4" && index.current_start_ms() == 3000, "the new span begins where the old one ended");
  std::vector<bx::TimelineSpan> closed = index.take_closed();
  check(closed.size() == 1 && closed[0].file == "a.mp4" && closed[0].start_ms == 1000 && closed[0].end_ms == 3000,
        "the closed span carries its file and both ticks");
  check(index.take_closed().empty(), "taking the closed spans clears them");

  check(!index.change("b.mp4", 4000), "the same file twice is not a change");
  check(!index.change("", 4000), "an empty file is not a change");
  check(index.current() == "b.mp4" && index.current_start_ms() == 3000, "a refused change leaves the span alone");

  // A splitter tick that arrives behind the span's start (a tick taken in the signal, before this pass's) must
  // not make a span that ends before it began.
  check(index.change("c.mp4", 2000), "a change behind the open start is clamped, not refused");
  closed = index.take_closed();
  check(closed.size() == 1 && closed[0].start_ms == 3000 && closed[0].end_ms == 3000,
        "a clamped boundary keeps the span from ending before it began");
  check(index.current() == "c.mp4" && index.current_start_ms() == 3000, "and the next span begins at that boundary");

  // The first frame's correction: later only, never earlier than the tick the file was asked for.
  check(!index.retime_current(2500), "a retime behind the start is refused");
  check(index.retime_current(3500) && index.current_start_ms() == 3500, "the first frame moves the start forward");
  check(index.stop(5000), "the stop closes the open span");
  check(!index.active() && index.current().empty(), "nothing is open after the stop");
  closed = index.take_closed();
  check(closed.size() == 1 && closed[0].file == "c.mp4" && closed[0].start_ms == 3500 && closed[0].end_ms == 5000,
        "the last span reaches the stop");
  check(!index.stop(6000), "a second stop is nothing");
  check(!index.change("d.mp4", 7000), "a change after the stop is nothing");
  check(!index.retime_current(8000), "a retime after the stop is nothing");
  check(index.take_closed().empty(), "and nothing was closed by any of it");

  // A whole segment: four files, each span starting exactly where the one before it ended.
  check(index.begin("e.mp4", 10000), "a new segment begins after the stop");
  check(index.change("f.mp4", 18000) && index.change("g.mp4", 26000) && index.change("h.mp4", 34000),
        "three more files, each closing the span before it");
  check(index.current() == "h.mp4" && index.current_start_ms() == 34000, "the last file is the open one");
  check(index.stop(42000), "the segment stops");
  closed = index.take_closed();
  check(closed.size() == 4, "every file of the segment is a span");
  check(closed[0].file == "e.mp4" && closed[0].start_ms == 10000 && closed[0].end_ms == 18000 &&
            closed[1].file == "f.mp4" && closed[1].start_ms == 18000 && closed[1].end_ms == 26000 &&
            closed[2].file == "g.mp4" && closed[2].start_ms == 26000 && closed[2].end_ms == 34000 &&
            closed[3].file == "h.mp4" && closed[3].start_ms == 34000 && closed[3].end_ms == 42000,
        "the spans are contiguous and in order");
  bool monotonic = true;
  for (size_t i = 0; i < closed.size(); ++i) {
    if (closed[i].end_ms < closed[i].start_ms) monotonic = false;
    if (i > 0 && closed[i].start_ms < closed[i - 1].end_ms) monotonic = false;
  }
  check(monotonic, "no span ends before it began, and no span begins before the one before it ended");
}

// Task 2 of the Timeline: a chunk carries the wall clock and the epoch it belongs to, and only a
// published chunk is playable. An index written before those fields existed still loads: its chunks
// were finished when they were written, so they are published, and their wall time is derived from
// the session's own start.
void test_timeline_chunks() {
  std::printf("timeline chunks\n");
  using bx::SessionChunk;
  using bx::SessionEntry;
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::string root = temp_directory() + "\\timeline-chunks";
  bx::session_store_init(root);
  std::string error;

  SessionEntry session;
  check(bx::session_store_begin("timeline", session, error), "a timeline session begins");
  session.started_at = 1792310400;  // a whole second: the wall clock is seconds since 1970
  const std::string first = bx::session_media_dir(session.id) + "\\chunk-1.mp4";
  const std::string second = bx::session_media_dir(session.id) + "\\chunk-2.mp4";
  write_bytes(first, 4096);
  write_bytes(second, 2048);

  SessionChunk finished;
  finished.file = first;
  finished.bytes = 4096;
  finished.start_ms = 12000;
  finished.end_ms = 20000;
  finished.wall_start_ms = 1792310412000LL;
  finished.wall_end_ms = 1792310420000LL;
  finished.epoch = 2;
  finished.published = true;
  check(bx::session_store_append_chunk(session, finished, error), "a finished chunk is indexed");

  SessionChunk unfinished = finished;
  unfinished.file = second;
  unfinished.bytes = 2048;
  unfinished.start_ms = 20000;
  unfinished.end_ms = 24000;
  unfinished.wall_start_ms = 1792310420000LL;
  unfinished.wall_end_ms = 1792310424000LL;
  unfinished.published = false;
  check(bx::session_store_append_chunk(session, unfinished, error), "an unfinished chunk is indexed too");
  check(bx::session_store_close(session, 30000, error), "the session closes");

  SessionEntry loaded;
  check(bx::session_store_load_one(session.id, loaded, error), "the session loads");
  check(loaded.chunks.size() == 2, "both chunks are in the index");
  const SessionChunk& kept = loaded.chunks[0];
  check(kept.start_ms == 12000 && kept.end_ms == 20000 && kept.bytes == 4096 &&
            kept.wall_start_ms == 1792310412000LL && kept.wall_end_ms == 1792310420000LL &&
            kept.epoch == 2 && kept.published,
        "every field of a new chunk survives the index");
  check(!loaded.chunks[1].published, "an unfinished chunk loads as unpublished");

  const std::vector<const SessionChunk*> visible = bx::session_visible_chunks(loaded);
  check(visible.size() == 1 && visible[0]->file == first,
        "an unpublished chunk is not in the preview's visible list");

  // An index written before the wall clock, the epoch and publication existed: only the four old
  // fields, and the session's own start. `start_ms` there is an absolute GetTickCount() tick, so the
  // wall time is the session's start plus the chunk's offset from `started_ms` - never the tick
  // added to a wall clock, which would land years in the future.
  auto write_legacy_index = [](const std::string& path, const std::string& body) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    std::fclose(file);
    return written;
  };
  SessionEntry legacy;
  check(bx::session_store_begin("recording", legacy, error), "a session begins for an old index");
  const std::string legacy_index =
      "{\"id\":\"" + legacy.id +
      "\",\"kind\":\"recording\",\"started_ms\":9000,\"started_at\":1792310400,\"ended_ms\":96000,"
      "\"closed\":true,\"chunks\":[{\"file\":\"legacy.mkv\",\"bytes\":2048,\"start_ms\":72000,"
      "\"end_ms\":92000}],\"markers\":[]}";
  check(write_legacy_index(root + "\\" + legacy.id + "\\index.json", legacy_index),
        "the old index is written");

  SessionEntry old;
  check(bx::session_store_load_one(legacy.id, old, error), "an index without the new fields loads");
  check(old.chunks.size() == 1 &&
            old.chunks[0].wall_start_ms == 1792310400LL * 1000 + 63000 &&
            old.chunks[0].wall_end_ms == 1792310400LL * 1000 + 83000 &&
            old.chunks[0].epoch == 0 && old.chunks[0].published,
        "an old chunk takes the session's wall clock plus its tick offset and stays playable");
  check(bx::session_visible_chunks(old).size() == 1, "an old chunk is in the visible list");

  // An index older still wrote no `started_ms`, so no tick offset can be known: the chunk takes the
  // session's start rather than a subtraction that wraps.
  SessionEntry older;
  check(bx::session_store_begin("recording", older, error), "a session begins for an older index");
  const std::string older_index =
      "{\"id\":\"" + older.id +
      "\",\"kind\":\"recording\",\"started_at\":1792310400,\"closed\":true,"
      "\"chunks\":[{\"file\":\"legacy-old.mkv\",\"bytes\":1024,\"start_ms\":5000,\"end_ms\":25000}],"
      "\"markers\":[]}";
  check(write_legacy_index(root + "\\" + older.id + "\\index.json", older_index),
        "the older index is written");
  SessionEntry without_start;
  check(bx::session_store_load_one(older.id, without_start, error), "an index without started_ms loads");
  check(without_start.chunks.size() == 1 &&
            without_start.chunks[0].wall_start_ms == 1792310400LL * 1000 &&
            without_start.chunks[0].wall_end_ms == 1792310400LL * 1000,
        "a missing session tick start keeps the session's wall start, without wrapping");
}

void test_dir_listing() {
  std::printf("directory listing\n");
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::string dir = temp_directory() + "/listing";
  CreateDirectoryA(dir.c_str(), nullptr);
  write_bytes(dir + "\\Chunk-A.mp4", 16);
  write_bytes(dir + "\\chunk-b.mp4", 16);
  bx::DirListing listing;
  check(listing.exists(dir + "\\Chunk-A.mp4"), "a listed file is there");
  check(listing.exists(dir + "/chunk-a.MP4"), "a name matches without case and with either slash");
  check(listing.exists(dir + "\\chunk-b.mp4"), "a second file in the same folder is there");
  check(!listing.exists(dir + "\\chunk-c.mp4"), "a file that is not there is not");
  check(listing.listings() == 1, "one folder is listed once for every name asked");
  write_bytes(dir + "\\late.mp4", 16);
  check(listing.exists(dir + "\\late.mp4"), "a file made after the listing is found by its own check");
  check(listing.size(dir + "\\Chunk-A.mp4") == 16 && listing.size(dir + "\\nothing.mp4") == -1, "a listed file brings its size, an unlisted one says so");
  check(!listing.exists(temp_directory() + "/no-such-folder/x.mp4"), "a folder that is not there holds nothing");
}

// The splitter reports a file with backslashes, while recovery builds the paths it finds by joining onto whatever the sessions root was
// configured with, so one file can be spelled two ways. Recovery must still know it: a root that mixes `/` and `\` made it treat every
// indexed file as unknown, so it indexed nothing (more than one) or filed the same footage a second time (exactly one).
void test_recovery_path_forms() {
  std::printf("recovery path forms\n");
  using bx::SessionChunk;
  using bx::SessionEntry;
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::string root = temp_directory() + "/timeline-mixed";  // a forward slash in the root, as a configured folder can have
  bx::session_store_init(root);
  std::string error;
  const auto backslashes = [](std::string path) {
    for (char& character : path) {
      if (character == '/') character = '\\';
    }
    return path;
  };
  const auto kill_writer = [](const std::string& id) {
    FILE* file = std::fopen(bx::session_writer_state_path(id).c_str(), "wb");
    if (!file) return false;
    std::fputs("1 1\n", file);
    std::fclose(file);
    return true;
  };

  SessionEntry two;
  check(bx::session_store_begin("timeline", two, error), "a session begins under a root that mixes separators");
  const std::string first = bx::session_media_dir(two.id) + "\\chunk-1.mp4";
  const std::string tail = bx::session_media_dir(two.id) + "\\chunk-2.mp4";
  write_bytes(first, 1024);
  write_bytes(tail, 512);
  SessionChunk indexed;
  indexed.file = backslashes(first);  // how the splitter's file_changed names it
  indexed.start_ms = two.started_ms;
  indexed.end_ms = two.started_ms + 5000;
  indexed.wall_start_ms = two.started_at * 1000;
  indexed.wall_end_ms = indexed.wall_start_ms + 5000;
  check(bx::session_store_append_chunk(two, indexed, error), "the writer indexes a chunk under the backslash spelling");
  check(kill_writer(two.id), "the writer dies");
  SessionEntry recovered;
  check(bx::session_store_load_one(two.id, recovered, error), "the session loads");
  check(recovered.chunks.size() == 2, "the indexed chunk is known under either spelling, so only the tail is filed");

  SessionEntry one;
  check(bx::session_store_begin("timeline", one, error), "a second session begins");
  const std::string only = bx::session_media_dir(one.id) + "\\chunk-1.mp4";
  write_bytes(only, 1024);
  SessionChunk sole;
  sole.file = backslashes(only);
  sole.start_ms = one.started_ms;
  sole.end_ms = one.started_ms + 5000;
  sole.wall_start_ms = one.started_at * 1000;
  sole.wall_end_ms = sole.wall_start_ms + 5000;
  check(bx::session_store_append_chunk(one, sole, error), "the writer indexes its only chunk");
  check(kill_writer(one.id), "and dies with nothing else on disk");
  SessionEntry reread;
  check(bx::session_store_load_one(one.id, reread, error) && reread.chunks.size() == 1,
        "the same footage is not filed a second time under its other spelling");
}

// A read must never write a session its writer still owns. The writer publishes its own chunks; a reader's
// recovery used to file the chunk that writer was still holding a second time, and the two then raced on
// index.json. A live session's index therefore reads back byte for byte, and recovery happens only for a
// session a dead engine left behind - once, because the files are then known to the index.
void test_timeline_store_reads() {
  std::printf("timeline store reads\n");
  using bx::SessionEntry;
  using bx::SessionChunk;
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::string root = temp_directory() + "\\timeline-live";
  bx::session_store_init(root);
  std::string error;

  const auto read_file = [](const std::string& path) {
    std::string text;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return text;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
    return text;
  };
  const auto replace_file = [](const std::string& path, const std::string& text) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    std::fclose(file);
    return written;
  };

  SessionEntry live;
  check(bx::session_store_begin("timeline", live, error), "a timeline session begins");
  const std::string indexed = bx::session_media_dir(live.id) + "\\chunk-1.mp4";
  const std::string in_flight = bx::session_media_dir(live.id) + "\\chunk-2.mp4";
  write_bytes(indexed, 1024);
  write_bytes(in_flight, 512);
  SessionChunk chunk;
  chunk.file = indexed;
  chunk.start_ms = live.started_ms;
  chunk.end_ms = live.started_ms + 5000;
  chunk.wall_start_ms = live.started_at * 1000;
  chunk.wall_end_ms = chunk.wall_start_ms + 5000;
  check(bx::session_store_append_chunk(live, chunk, error), "the writer indexes its first chunk");

  const std::string index_path = root + "\\" + live.id + "\\index.json";
  const std::string before = read_file(index_path);
  check(!before.empty(), "the live session's index is readable");
  SessionEntry read_back;
  check(bx::session_store_load_one(live.id, read_back, error), "a live session loads");
  check(read_file(index_path) == before, "reading a live session leaves its index byte for byte");
  check(read_back.chunks.size() == 1, "and does not file the chunk its writer is still holding");
  check(read_back.interrupted, "a live session still reads back as an open one");

  // The writer is gone: its state file carries an earlier run's token, which is what a crashed engine
  // leaves. The chunk it was writing is footage now, and it is published exactly once.
  check(replace_file(bx::session_writer_state_path(live.id), "1 1\n"),
        "the state file of the dead writer is left behind");
  SessionEntry recovered;
  check(bx::session_store_load_one(live.id, recovered, error), "a session whose writer died loads");
  check(recovered.chunks.size() == 2 && recovered.chunks[1].file == in_flight,
        "the tail the dead writer left is filed");
  check(recovered.chunks[1].published && recovered.chunks[1].wall_end_ms > recovered.chunks[1].wall_start_ms,
        "and it is published footage with a wall time");
  // The recovered chunk began when the last published one closed, not at the session's start: a tail
  // that claimed the whole session made every range over the last minute plan against a lie.
  check(recovered.chunks[1].start_ms == chunk.end_ms,
        "and it begins where the published footage stops, not at the session's start");
  const std::string settled = read_file(index_path);
  SessionEntry again;
  check(bx::session_store_load_one(live.id, again, error) && again.chunks.size() == 2,
        "a second read files nothing more");
  check(read_file(index_path) == settled, "and writes nothing either");

  SessionEntry overlap;
  check(bx::session_store_begin("timeline", overlap, error), "an overlapping session begins");
  const std::string overlap_a = bx::session_media_dir(overlap.id) + "\\chunk-a.mp4";
  const std::string overlap_b = bx::session_media_dir(overlap.id) + "\\chunk-b.mp4";
  write_bytes(overlap_a, 512);
  write_bytes(overlap_b, 512);
  check(replace_file(bx::session_writer_state_path(overlap.id), "1 1\n"), "the overlap writer dies");
  SessionEntry uncertain;
  check(bx::session_store_load_one(overlap.id, uncertain, error) && uncertain.chunks.empty(),
        "two unindexed overlapping chunks are not assigned invented times");
  check(!read_file(overlap_a).empty() && !read_file(overlap_b).empty(), "both uncertain files are kept");
}

// Task 8 of the Timeline: a range of session time becomes the files to copy and the cuts inside them.
// Only published chunks are footage; a range that reaches past the newest published chunk is refused
// with the one message the preview shows, never quietly shortened, and a range that spans two epochs
// (a settings change rebuilt the encoders) cannot be copied.
void test_timeline_save_plan() {
  std::printf("timeline save plan\n");
  auto chunk = [](const char* file, uint32_t start_ms, uint32_t end_ms, uint32_t epoch, bool published) {
    bx::SessionChunk c;
    c.file = file;
    c.start_ms = start_ms;
    c.end_ms = end_ms;
    c.wall_start_ms = 1700000000000LL + start_ms;
    c.wall_end_ms = 1700000000000LL + end_ms;
    c.epoch = epoch;
    c.published = published;
    return c;
  };
  bx::SessionEntry entry;
  entry.id = "timeline 2026-10-29 10-00-00";
  entry.kind = "timeline";
  entry.chunks.push_back(chunk("a.mp4", 10000, 20000, 0, true));
  entry.chunks.push_back(chunk("b.mp4", 20000, 28000, 0, true));
  entry.chunks.push_back(chunk("c.mp4", 30000, 38000, 1, true));
  entry.chunks.push_back(chunk("d.mp4", 38000, 42000, 1, false));
  std::string error;

  bx::TimelineSavePlan plan = bx::timeline_save_plan(entry, 12000, 26000, 38000, error);
  check(error.empty() && plan.files.size() == 2 && plan.files[0] == "a.mp4" && plan.files[1] == "b.mp4",
        "a published range plans the chunks that cover it");
  check(plan.start_in_first_ms == 2000 && plan.end_in_last_ms == 6000,
        "the cuts are local to the first and last chunk, in milliseconds");

  plan = bx::timeline_save_plan(entry, 22000, 25000, 38000, error);
  check(error.empty() && plan.files.size() == 1 && plan.files[0] == "b.mp4" &&
            plan.start_in_first_ms == 2000 && plan.end_in_last_ms == 5000,
        "a range inside one chunk plans one file");

  plan = bx::timeline_save_plan(entry, 32000, 41000, 38000, error);
  check(plan.files.empty() && error == "the newest moment is not saved yet",
        "a live range past the published edge is refused, not shortened");
  error.clear();
  plan = bx::timeline_save_plan(entry, 32000, 41000, 38000, error);
  check(plan.files.empty() && error == "the newest moment is not saved yet",
        "a finished session refuses the same missing tail");

  error.clear();
  plan = bx::timeline_save_plan(entry, 25000, 33000, 38000, error);
  check(error.empty() && plan.files.size() == 1 && plan.files[0] == "c.mp4" && plan.start_in_first_ms == 0 &&
            plan.end_in_last_ms == 3000 && plan.cut_head_ms == 5000,
        "a range across a settings change saves the part in the layout that holds its end, and says how much it left out");
  error.clear();
  plan = bx::timeline_save_plan(entry, 12000, 26000, 38000, error);
  check(error.empty() && plan.files.size() == 2 && plan.cut_head_ms == 0,
        "a range wholly before the change saves that layout's part as it is");
  entry.chunks.push_back(chunk("e.mp4", 42000, 50000, 2, true));
  error.clear();
  plan = bx::timeline_save_plan(entry, 14000, 46000, 50000, error);
  check(error.empty() && plan.files.size() == 1 && plan.files[0] == "e.mp4" && plan.start_in_first_ms == 0 && plan.end_in_last_ms == 4000 &&
            plan.cut_head_ms == 28000,
        "two changes inside the range keep only the last layout");
  entry.chunks.pop_back();

  error.clear();
  plan = bx::timeline_save_plan(entry, 36000, 38000, 38000, error);
  check(error.empty() && plan.files.size() == 1 && plan.files[0] == "c.mp4" && plan.start_in_first_ms == 6000 &&
            plan.end_in_last_ms == 8000,
        "a range ending exactly on the visible edge is saved as it is");

  error.clear();
  plan = bx::timeline_save_plan(entry, 15000, 15000, 38000, error);
  check(plan.files.empty() && !error.empty(), "an empty range plans nothing");

  // A published chunk whose file has gone is not footage: planning from it named a file the copy could
  // not open halfway through the range. The save view drops it before the plan sees it, and the edge falls
  // back to the newest chunk that is really there.
  const auto exists = [](const std::string& file) { return file != "b.mp4"; };
  bx::SessionEntry usable;
  const int64_t available_end = bx::timeline_save_view(entry, usable, exists);
  check(usable.chunks.size() == 3 && available_end == 38000,
        "a save view drops a published chunk that is no longer on disk, and its edge with it");
  error.clear();
  plan = bx::timeline_save_plan(usable, 22000, 25000, available_end, error);
  check(plan.files.empty() && error == "the range covers no recorded footage",
        "a range that only the missing chunk covered plans nothing");
  error.clear();
  plan = bx::timeline_save_plan(usable, 33000, 37000, available_end, error);
  check(error.empty() && plan.files.size() == 1 && plan.files[0] == "c.mp4",
        "the footage either side of the hole still plans");

  // A session that spans the 49.7-day tick wrap: its chunks hold raw ticks, and a range is on the session's clock.
  bx::SessionEntry wrapped;
  wrapped.id = "timeline wrap";
  wrapped.kind = "timeline";
  wrapped.started_ms = 4294960000u;  // 7.3 s before the wrap
  wrapped.chunks.push_back(chunk("w1.mp4", 4294960000u, 4294965000u, 0, true));
  wrapped.chunks.push_back(chunk("w2.mp4", 4294965000u, 4000u, 0, true));  // ends after the wrap
  bx::SessionEntry wrapped_usable;
  const int64_t wrapped_end = bx::timeline_save_view(bx::timeline_on_session_clock(wrapped), wrapped_usable,
                                                     [](const std::string&) { return true; });
  check(wrapped_end == 11296, "the edge of a session across the tick wrap is its length");
  error.clear();
  plan = bx::timeline_save_plan(wrapped_usable, 3000, 9000, wrapped_end, error);
  check(error.empty() && plan.files.size() == 2 && plan.start_in_first_ms == 3000 && plan.end_in_last_ms == 4000,
        "a range across the tick wrap plans both chunks, cut where it was asked");

  // A pause inside the range: the copy stitches across it, so the plan names how much footage is missing.
  bx::SessionEntry paused;
  paused.id = "timeline paused";
  paused.kind = "timeline";
  paused.chunks.push_back(chunk("p1.mp4", 0, 5000, 0, true));
  paused.chunks.push_back(chunk("p2.mp4", 5000, 10000, 0, true));
  paused.chunks.push_back(chunk("p3.mp4", 70000, 75000, 0, true));  // 60 s later: the game left and came back
  error.clear();
  plan = bx::timeline_save_plan(paused, 2000, 72000, 75000, error);
  check(error.empty() && plan.files.size() == 3 && plan.gap_before_ms.size() == 3 && plan.gap_before_ms[0] == 0 &&
            plan.gap_before_ms[1] == 0 && plan.gap_before_ms[2] == 60000,
        "a pause inside the range is named, and touching chunks have no hole");
  error.clear();
  plan = bx::timeline_save_plan(paused, 2000, 8000, 75000, error);
  check(error.empty() && plan.files.size() == 2 && plan.gap_before_ms[1] == 0, "a range before the pause has no hole");
}

// The preview's strip (`timeline.thumbs`): which moment becomes a JPEG, from which published chunk and at
// what offset, and what that picture is called. The rules are the cheap ones a strip refresh depends on -
// clamped into the published span, one picture per second of one chunk, 24 asked and 6 made - and the
// footage rules that keep a decoder off a file nobody may open: only a published chunk, only one on disk.
void test_timeline_loudness() {
  check(bx::timeline_loudness_byte(0.0f) == 0 && bx::timeline_loudness_byte(0.0005f) == 0, "silence and under -60 dBFS is level 0");
  check(bx::timeline_loudness_byte(1.0f) == 255 && bx::timeline_loudness_byte(2.0f) == 255, "full scale and beyond is 255");
  const int half = bx::timeline_loudness_byte(0.0316f);  // -30 dBFS
  check(half >= 126 && half <= 129, "-30 dBFS sits in the middle of the scale");
  check(bx::timeline_loudness_byte(0.5f) > bx::timeline_loudness_byte(0.1f), "louder is a higher level");
  std::vector<float> many(bx::kTimelineLoudnessMaxBuckets + 50, 0.5f);
  check(bx::timeline_loudness_bytes(many).size() == bx::kTimelineLoudnessMaxBuckets, "a runaway file is capped");
  const std::vector<int64_t> starts{0, 8000, 16000, 24000};
  check(bx::timeline_loudness_order(starts, 9000) == std::vector<size_t>({1, 2, 0, 3}),
        "loudness reads the rewind position and nearby chunks before the newest");
  check(bx::timeline_loudness_order(starts, 7999).front() == 0,
        "loudness reads the chunk containing the playhead before the next chunk");
  check(bx::timeline_loudness_order(starts, -1) == std::vector<size_t>({3, 2, 1, 0}),
        "loudness without a position still reads newest first");
}

void test_timeline_thumbs() {
  std::printf("timeline thumbnails\n");
  using bx::SessionChunk;
  using bx::SessionEntry;
  using bx::TimelineThumb;
  using bx::TimelineThumbSpan;

  check(bx::kTimelineThumbsPerCall == 24 && bx::kTimelineThumbsDecoded == 12,
        "a strip asks for 24 moments and six pictures are made per call");

  // A chunk's own file name is the prefix of its pictures: a path from the index can never name a file
  // outside the thumbs folder, and nothing that was safe in it is changed.
  check(bx::timeline_thumb_key("D:\\sessions\\s1\\media\\Recording 1.mp4") == "Recording_1.mp4",
        "a chunk path becomes a safe file name");
  check(bx::timeline_thumb_key("a-b_c.9.mp4") == "a-b_c.9.mp4", "safe characters are kept as they are");
  check(bx::timeline_thumb_key("../../etc/passwd") == "passwd", "only the file's own name is used");
  check(bx::timeline_thumb_key("") == "chunk", "an unusable name still names a picture");
  check(bx::timeline_thumb_name("Recording_1.mp4", 4000) == "Recording_1.mp4-4000.jpg",
        "a picture is named after its chunk and the second inside it");

  // Two chunks with a hole between them: the hole is not footage and names no picture.
  std::vector<TimelineThumbSpan> spans(2);
  spans[0].chunk_index = 3;
  spans[0].key = "Recording_1.mp4";
  spans[0].start_ms = 0;
  spans[0].end_ms = 10000;
  spans[1].chunk_index = 5;
  spans[1].key = "B.mp4";
  spans[1].start_ms = 12000;
  spans[1].end_ms = 20000;
  // 5.0 s and 5.5 s of the first chunk are one picture; the hole at 11 s has none; the far end is clamped
  // into the published span; 12.0 s and 12.5 s are one picture of the second chunk.
  std::vector<TimelineThumb> plan =
      bx::timeline_thumb_plan(spans, {5000, 5500, 11000, 97000, 12500, 12000}, 0, 20000);
  check(plan.size() == 3, "one picture per second of footage, and none for the hole");
  if (plan.size() == 3) {
    check(plan[0].at_ms == 5000 && plan[0].chunk_index == 3 && plan[0].offset_ms == 5000 &&
              plan[0].name == "Recording_1.mp4-5000.jpg",
          "the first moment is cut from the chunk that holds it, at its offset inside that chunk");
    check(plan[1].at_ms == 20000 && plan[1].chunk_index == 5 && plan[1].offset_ms == 8000,
          "a moment past the span is clamped to its end, and decoded inside the chunk");
    check(plan[2].at_ms == 12500 && plan[2].chunk_index == 5 && plan[2].offset_ms == 500,
          "a moment just inside the second chunk is kept, in request order");
  }

  // Below the span, on a boundary and with nothing published at all.
  plan = bx::timeline_thumb_plan(spans, {-4000}, 0, 20000);
  check(plan.size() == 1 && plan[0].at_ms == 0 && plan[0].offset_ms == 0,
        "a moment before the span is clamped to its start");
  plan = bx::timeline_thumb_plan(spans, {10000}, 0, 20000);
  check(plan.size() == 1 && plan[0].chunk_index == 3 && plan[0].offset_ms == 10000,
        "the last millisecond of a chunk still belongs to it");
  plan = bx::timeline_thumb_plan({}, {5000, 9000}, 0, 20000);
  check(plan.empty(), "a session with no published chunk asks no decoder for anything");
  plan = bx::timeline_thumb_plan(spans, {}, 0, 20000);
  check(plan.empty(), "no moments asked, no pictures");
  plan = bx::timeline_thumb_plan(spans, {5000}, 8000, 8000);
  check(plan.empty(), "an empty available span names no picture");

  // A caller that sends more than the strip needs gets the first 24 considered, not 300 decodes.
  std::vector<TimelineThumbSpan> long_span(1);
  long_span[0].chunk_index = 0;
  long_span[0].key = "L.mp4";
  long_span[0].start_ms = 0;
  long_span[0].end_ms = 60000;
  std::vector<int64_t> many;
  for (int index = 0; index < 300; ++index) many.push_back(index * 1000);
  plan = bx::timeline_thumb_plan(long_span, many, 0, 60000);
  check(plan.size() == bx::kTimelineThumbsPerCall, "at most 24 moments of one request are used");

  // Only a published chunk that is still on disk is footage. A published chunk whose file has gone must not
  // move the span: the page would draw a segment that never decodes, and a save would name a file FFmpeg
  // cannot open. The hole in the middle stays visible as a hole in the chunk list.
  SessionEntry entry;
  entry.started_ms = 1000;
  entry.chunks.resize(3);
  entry.chunks[0].file = "a.mp4";
  entry.chunks[0].start_ms = 1000;
  entry.chunks[0].end_ms = 6000;
  entry.chunks[0].bytes = 100;
  entry.chunks[1].file = "b.mp4";
  entry.chunks[1].start_ms = 6000;
  entry.chunks[1].end_ms = 10000;
  entry.chunks[1].bytes = 100;
  entry.chunks[2].file = "c.mp4";
  entry.chunks[2].start_ms = 10000;
  entry.chunks[2].end_ms = 12000;
  entry.chunks[2].bytes = 100;
  entry.chunks[2].published = false;
  const auto exists = [](const std::string& file) { return file != "b.mp4"; };
  const std::vector<size_t> playable = bx::timeline_playable_chunks(entry, exists);
  check(playable.size() == 1 && playable[0] == 0,
        "an unpublished chunk and one whose file has gone are not playable");
  int64_t span_start = -1, span_end = -1, span_bytes = -1;
  bx::timeline_published_span(entry, playable, span_start, span_end, span_bytes);
  check(span_start == 0 && span_end == 5000 && span_bytes == 100,
        "the span covers the footage that is really there");
  bx::timeline_published_span(entry, span_start, span_end, span_bytes);
  check(span_start == 0 && span_end == 9000 && span_bytes == 200,
        "the index-only span still counts a chunk whose file has gone");

  // On disk: the preview's pictures are derived, and they must not keep a session's folder from being erased
  // or pruned. A thumbs folder nobody removed made `timeline.erase` fail after it had deleted the footage.
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::string root = temp_directory() + "\\timeline-thumbs";
  bx::session_store_init(root);
  std::string error;
  SessionEntry session;
  check(bx::session_store_begin("timeline", session, error), "a timeline session begins for the thumbnails");
  const std::string chunk_file = bx::session_media_dir(session.id) + "\\chunk-1.mp4";
  write_bytes(chunk_file, 1024);
  SessionChunk chunk;
  chunk.file = chunk_file;
  chunk.bytes = 1024;
  chunk.start_ms = session.started_ms;
  chunk.end_ms = session.started_ms + 5000;
  chunk.wall_start_ms = session.started_at * 1000;
  chunk.wall_end_ms = chunk.wall_start_ms + 5000;
  check(bx::session_store_append_chunk(session, chunk, error), "its chunk is indexed");
  check(bx::session_store_close(session, session.started_ms + 6000, error), "the session closes");
  const std::string thumbs = bx::session_thumbs_dir(session.id);
  check(thumbs.find("\\thumbs") != std::string::npos, "a session's pictures live in its own thumbs folder");
  CreateDirectoryA(thumbs.c_str(), nullptr);
  write_bytes(thumbs + "\\chunk-1.mp4-0.jpg", 64);
  write_bytes(thumbs + "\\other.mp4-0.jpg", 64);
  bx::session_thumbs_remove(session.id, "chunk-1.mp4");
  check(GetFileAttributesA((thumbs + "\\chunk-1.mp4-0.jpg").c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA((thumbs + "\\other.mp4-0.jpg").c_str()) != INVALID_FILE_ATTRIBUTES,
        "one chunk's pictures go without touching another chunk's");
  std::vector<std::string> locked;
  check(bx::session_store_erase_timeline(session.id, locked, error) && locked.empty(),
        "a session with a thumbs folder is erased");
  check(GetFileAttributesA(thumbs.c_str()) == INVALID_FILE_ATTRIBUTES,
        "the thumbs folder goes with the session");
  check(GetFileAttributesA((root + "\\" + session.id).c_str()) == INVALID_FILE_ATTRIBUTES,
        "and the session's own folder is gone with it");
  bx::session_store_init("");
}

// Task 8: the chunks of one join must carry the same stream. A settings change between the chunks
// produces two readable files that cannot be concatenated, and the copy has to refuse it before it
// creates the output: a broken clip on disk is worse than an error.
void test_stream_compatibility() {
  std::printf("stream compatibility\n");
  {
    // A save across a settings change keeps the run of chunks that share the last one's stream.
    bx::StreamSignature h264;
    h264.codec_id = 27;
    h264.width = 1920;
    h264.height = 1080;
    h264.pix_fmt = 190;
    h264.frame_rate = {60, 1};
    h264.sample_rate = 48000;
    h264.channel_layout = 0x3;
    h264.time_base = {1, 60000};
    bx::StreamSignature av1 = h264;
    av1.codec_id = 226;
    check(bx::trailing_compatible_run({h264, h264, h264}) == 0, "one stream throughout starts at the first chunk");
    check(bx::trailing_compatible_run({h264, h264, av1, av1}) == 2, "the run that holds the end starts after the change");
    check(bx::trailing_compatible_run({h264, av1, h264}) == 2, "a change back is a change: only the last chunk is the end's layout");
    check(bx::trailing_compatible_run({}) == 0 && bx::trailing_compatible_run({av1}) == 0, "nothing, or one chunk, has no change");
  }
  bx::StreamSignature base;
  base.codec_id = 27;  // AV_CODEC_ID_H264, as an int: the test links no FFmpeg
  base.width = 1920;
  base.height = 1080;
  base.pix_fmt = 190;  // NV12
  base.frame_rate = {60, 1};
  base.sample_rate = 48000;
  base.channel_layout = 0x3;
  base.time_base = {1, 60000};
  check(bx::streams_compatible(base, base), "the same stream twice is compatible");
  bx::StreamSignature other = base;
  check(bx::streams_compatible(base, other), "an unmodified copy is compatible");

  other = base;
  other.codec_id = 28;
  check(!bx::streams_compatible(base, other), "a different codec is refused");
  other = base;
  other.width = 1280;
  check(!bx::streams_compatible(base, other), "a different width is refused");
  other = base;
  other.height = 720;
  check(!bx::streams_compatible(base, other), "a different height is refused");
  other = base;
  other.pix_fmt = 0;
  check(!bx::streams_compatible(base, other), "a different pixel format is refused");
  other = base;
  other.frame_rate = {30, 1};
  check(!bx::streams_compatible(base, other), "a different frame rate is refused");
  other = base;
  other.sample_rate = 44100;
  check(!bx::streams_compatible(base, other), "a different sample rate is refused");
  // 60/1 and 120/2 are the same rate written two ways.
  other = base;
  other.frame_rate = {120, 2};
  check(bx::streams_compatible(base, other), "the same rate written two ways is compatible");
  // A chunk's average rate is derived from its own frame count and duration, so FFmpeg reports the same
  // 59.99 fps stream as 60/1 in one chunk and 933120/15593 in the next. An exact compare refused every
  // Timeline save that spanned two chunks of one session.
  other = base;
  other.frame_rate = {933120, 15593};
  check(bx::streams_compatible(base, other), "a rate rounded differently for one chunk is compatible");
  other.frame_rate = {599, 10};
  check(bx::streams_compatible(base, other), "59.9 against 60 is the same stream");
}

// Task 3 of the Timeline: a chunk is indexed only once its writer has let go of the file. Windows
// answers the question directly: a writer that holds the file with no sharing makes a read-only open
// fail with a sharing violation.
void test_file_finality() {
  std::printf("file finality\n");
  const std::string path = temp_directory() + "\\finality\\open.bin";
  write_bytes(path, 16);
  HANDLE writer = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(writer != INVALID_HANDLE_VALUE, "the test can hold a file the way a writer does");
  check(!bx::file_writer_closed(path), "a file some writer still holds is not final");
  CloseHandle(writer);
  check(bx::file_writer_closed(path), "a file nobody holds any more is final");
  check(!bx::file_writer_closed(temp_directory() + "\\finality\\missing.bin"), "a missing file is not final");
}

// Task 4 of the Timeline: one rule decides whether the writer records. Precedence, highest first: off,
// buffer off, a manual recording, a pipeline rebuild, low disk, no captured game, the game not in front,
// and only then writing.
void test_timeline_state() {
  using bx::TimelineFacts;
  using bx::TimelineState;
  std::printf("timeline state\n");

  const auto next = [](const TimelineFacts& facts) { return bx::timeline_next_state(facts); };
  const auto on = []() {
    TimelineFacts facts{};
    facts.enabled = true;
    facts.buffer_on = true;
    return facts;
  };

  check(next(TimelineFacts{}) == TimelineState::Off, "Timeline off is Off");
  TimelineFacts facts = on();
  facts.buffer_on = false;
  check(next(facts) == TimelineState::Stopped, "with the buffer off it is Stopped");

  facts = on();
  facts.manual_owns_output = true;
  check(next(facts) == TimelineState::PausedManual, "a fallback recording occupying the Timeline output pauses it");
  facts.rebuilding = true;
  facts.low_disk = true;
  check(next(facts) == TimelineState::PausedManual, "a manual recording wins over a rebuild and the disk");

  facts = on();
  facts.rebuilding = true;
  check(next(facts) == TimelineState::PausedRebuild, "a settings rebuild pauses it");
  facts.low_disk = true;
  check(next(facts) == TimelineState::PausedRebuild, "a rebuild wins over low disk");

  facts = on();
  facts.low_disk = true;
  check(next(facts) == TimelineState::PausedDisk, "low disk pauses it");

  facts = on();
  check(next(facts) == TimelineState::PausedNoGame, "without a running game Timeline waits");
  // Everywhere: the desktop is footage, so no game is no reason to wait.
  facts.everywhere = true;
  check(next(facts) == TimelineState::Writing, "an everywhere Timeline writes the desktop with no game running");
  facts.idle = true;
  check(next(facts) == TimelineState::PausedIdle, "and pauses after a long spell without keyboard or mouse");
  facts.game_running = true;
  check(next(facts) == TimelineState::Writing, "idle never pauses a game: a controller is not input to Windows");
  facts.game_running = false;
  facts.low_disk = true;
  check(next(facts) == TimelineState::PausedDisk, "low disk still wins over the desktop");
  facts.low_disk = false;
  facts.buffer_on = false;
  check(next(facts) == TimelineState::Stopped, "no buffer, no Timeline, everywhere or not");
  facts.buffer_on = true;
  facts.everywhere = false;
  facts.idle = false;
  facts.game_running = true;
  facts.game_exe = "game.exe";
  check(next(facts) == TimelineState::Writing, "a running game enables Timeline");
  facts.manual_owns_output = true;
  check(next(facts) == TimelineState::PausedManual, "a fallback recording still pauses it, game or no game");

  const TimelineState states[] = {TimelineState::Off,     TimelineState::Stopped,  TimelineState::Writing,
                                  TimelineState::PausedManual, TimelineState::PausedRebuild, TimelineState::PausedDisk,
                                  TimelineState::PausedNoGame, TimelineState::PausedIdle};
  std::set<std::string> ids;
  for (TimelineState state : states) {
    const std::string id = bx::timeline_state_id(state);
    ids.insert(id);
    check(bx::timeline_writes(state) == (state == TimelineState::Writing),
          (std::string("only writing writes: ") + id).c_str());
  }
  check(ids.size() == 8, "every state has its own id");
  check(bx::timeline_state_id(TimelineState::PausedIdle) == std::string("paused_idle"), "idle has its id");
  check(std::string(bx::timeline_gap_label(3500, 2000, false, false, true)) == "Idle", "a gap after an idle pause is labelled Idle");
  check(std::string(bx::timeline_gap_label(3500, 2000, true, false, true)).empty(), "the sleep marker still wins over it");
  check(bx::timeline_idle_due(1000, 1000 + 5 * 60000, 5), "five minutes without input is idle at five");
  check(!bx::timeline_idle_due(1000, 1000 + 5 * 60000 - 1, 5), "a moment short of it is not");
  check(!bx::timeline_idle_due(0, 4000000000u, 0), "0 minutes never idles");
  check(bx::timeline_idle_due(4294967000u, 300000u - 296u, 5), "the tick clock wrapping gives the time that passed");
  check(bx::timeline_state_id(TimelineState::Writing) == std::string("writing"), "the state the status reports");

  check(bx::timeline_gap_ms(1000, 4500) == 3500, "a gap is the wall time between two ticks");
  check(bx::timeline_gap_ms(4500, 4500) == 0, "no time between them, no gap");
  check(bx::timeline_gap_ms(4294967295u, 0) == 1, "a gap across the tick clock's wrap is one millisecond");
  check(bx::timeline_gap_ms(4294966296u, 1000) == 2000, "a gap across the wrap is still the real interval");

  check(!bx::timeline_session_should_close(59999), "a session paused for under a minute stays open");
  check(bx::timeline_session_should_close(60000), "a session paused for a minute closes");
  check(bx::timeline_session_should_close(60001), "and one paused longer still closes");
  check(!bx::timeline_session_should_close(0), "a fresh pause keeps it");

  // The hole a resumed-after-game-pause leaves is labelled, so the preview shows NO GAME rather than a bare
  // gap; a pause for another reason is not a missing game, and a pending sleep/wake marker keeps its own
  // wording. A glance at another window writes nothing.
  check(std::string(bx::timeline_gap_label(3500, 2000, false, true)) == "Game paused",
        "a resumed-after-game-pause gap marker says Game paused");
  check(std::string(bx::timeline_gap_label(3500, 2000, false, false)).empty(),
        "a rebuild pause writes no game-paused marker");
  check(std::string(bx::timeline_gap_label(2000, 2000, false, true)).empty(), "a pause at the threshold writes no marker");
  check(std::string(bx::timeline_gap_label(3500, 2000, true, true)).empty(), "the sleep/wake marker is written instead");

  // Test-only knob for the pause/resume handle cost: the writer drops out for the first half of each period.
  check(!bx::timeline_test_pause_due(5000, 0), "the test pause is off with no period");
  check(bx::timeline_test_pause_due(0, 1000) && bx::timeline_test_pause_due(999, 1000), "the first half of the period pauses");
  check(!bx::timeline_test_pause_due(1000, 1000) && !bx::timeline_test_pause_due(1999, 1000), "the second half writes");
  check(bx::timeline_test_pause_due(2000, 1000), "and the next period pauses again");

  // A change of game is a marker inside one session, never a second session.
  check(bx::timeline_game_changed("cs2.exe", "dota2.exe"), "another game is a change");
  check(!bx::timeline_game_changed("CS2.EXE", "cs2.exe"), "the same game in another case is not");
  check(!bx::timeline_game_changed("", "cs2.exe"), "the first game of a session is not a change");
  check(!bx::timeline_game_changed("cs2.exe", ""), "footage with no game (the desktop) is not a change");
}
// A manual buffer stop ends the Timeline session at once; the internal pauses (a sleep, the lock, a
// rebuild, a settings change) keep it for the pause grace. A save still reading the session holds the
// stop until it lets go, so the footage it copies is never pulled away.
void test_timeline_manual_stop() {
  using bx::SessionChunk;
  using bx::SessionEntry;
  using bx::TimelineEndAction;
  using bx::TimelinePin;
  std::printf("timeline manual stop\n");
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  bx::session_store_init(temp_directory() + "\\timeline-stop");
  std::string error;

  SessionEntry session;
  check(bx::session_store_begin("timeline", session, error), "a timeline session begins");
  session.started_at = 1792310400;
  SessionChunk chunk;
  chunk.file = bx::session_media_dir(session.id) + "\\chunk-1.mp4";
  write_bytes(chunk.file, 4096);
  chunk.bytes = 4096;
  chunk.start_ms = 12000;
  chunk.end_ms = 20000;
  chunk.wall_start_ms = 1792310412000LL;
  chunk.wall_end_ms = 1792310420000LL;
  chunk.published = true;
  check(bx::session_store_append_chunk(session, chunk, error), "a published chunk is in the open index");

  // The writer has let go: the manual stop closes the index now, not after the pause grace.
  check(bx::timeline_end_action(true, false) == TimelineEndAction::close_now,
        "the manual stop closes a session whose writer has let go");
  check(bx::session_store_close(session, 20000, error), "the manual stop closes the session index");
  SessionEntry closed;
  check(bx::session_store_load_one(session.id, closed, error) && closed.closed,
        "the closed index says the session ended");
  check(!closed.interrupted, "a stopped session is not an interrupted one");
  check(closed.chunks.size() == 1 && closed.chunks[0].published,
        "the footage already recorded is still in the closed index");
  check(bx::timeline_end_action(false, false) == TimelineEndAction::none,
        "a second stop finds nothing open and is harmless");

  // An internal pause: the buffer is on and only the game went away. The session stays open until the
  // pause grace has passed, which is what a sleep, the lock, a rebuild and a settings change all rely on.
  bx::TimelineFacts facts;
  facts.enabled = true;
  facts.buffer_on = true;
  check(bx::timeline_next_state(facts) == bx::TimelineState::PausedNoGame,
        "the game leaving pauses the Timeline");
  check(!bx::timeline_writes(bx::TimelineState::PausedNoGame), "a paused Timeline writes no segment");
  check(!bx::timeline_session_should_close(30000), "a pause of thirty seconds keeps the session open");
  check(bx::timeline_session_should_close(bx::kTimelineSessionPauseMs),
        "only the pause grace closes it, and only when no manual stop asked");

  // A save in flight holds a lease on the session it is copying. The stop does not wait for that lease: the pin
  // is what keeps the footage out of the prune, and the index closes under the save, which carries on reading.
  const int64_t now = 1000000000000LL;
  TimelinePin save;
  save.id = "save " + session.id;
  save.owner = bx::kTimelineSaveOwner;
  save.session_id = session.id;
  save.start_ms = now - 60000;
  save.end_ms = now;
  save.expires_ms = now + 15 * 60000;
  check(bx::timeline_pin_active(save, now), "a save in flight holds a live lease on the session");
  check(bx::timeline_end_action(true, false) == TimelineEndAction::close_now,
        "a save reading the session does not hold the stop");
  save.expires_ms = now;
  check(!bx::timeline_pin_active(save, now), "a save that let go holds nothing");
  check(GetFileAttributesA(chunk.file.c_str()) != INVALID_FILE_ATTRIBUTES,
        "the footage the save read is still on disk after the stop");
  check(bx::timeline_end_action(true, true) == TimelineEndAction::stop_writer,
        "a segment still writing is stopped first, whatever a save holds");
  check(bx::timeline_end_action(true, false) == TimelineEndAction::close_now,
        "and the close is the next answer once it has let go");
}

// The manual stop must route through the Timeline: `buffer.stop` and the buffer toggle both call
// stop_buffer_with_timeline, and that helper ends the session before it stops the buffer. A call site that goes
// back to a plain stop_buffer still stops the buffer and looks right, and leaves the session open for the pause
// grace. No unit test can reach Protocol or ObsHost without libobs, so the route is read where it is written -
// the same way test_themes holds the page to the engine's colour table.
void test_buffer_stop_route() {
  std::printf("buffer stop route\n");
  std::string source;
  if (FILE* file = std::fopen((source_root() + "/src/engine/commands.cpp").c_str(), "rb")) {
    char buffer[8192];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) source.append(buffer, read);
    std::fclose(file);
  }
  check(!source.empty(), "the manual stop is checked against commands.cpp");

  const size_t helper = source.find("void stop_buffer_with_timeline(ObsHost& host)");
  check(helper != std::string::npos, "the one manual-stop helper exists");
  const size_t helper_end = helper == std::string::npos ? std::string::npos : source.find("\n}", helper);
  const std::string body =
      helper == std::string::npos ? std::string() : source.substr(helper, helper_end - helper);
  const size_t ended = body.find("end_timeline()");
  const size_t stopped = body.find("stop_buffer()");
  check(ended != std::string::npos && stopped != std::string::npos && ended < stopped,
        "the manual stop ends the Timeline session before it stops the buffer");

  const size_t branch = source.find("if (command == \"buffer.stop\")");
  check(branch != std::string::npos, "buffer.stop is routed");
  const size_t branch_end = branch == std::string::npos ? std::string::npos : source.find("\n  if (command ==", branch + 1);
  const std::string route =
      branch == std::string::npos ? std::string() : source.substr(branch, branch_end - branch);
  check(route.find("stop_buffer_with_timeline(host_)") != std::string::npos,
        "buffer.stop goes through the helper that ends the session");
  check(route.find("host_.stop_buffer()") == std::string::npos,
        "buffer.stop does not take the internal path that keeps the session");
  const size_t toggle = source.find("if (host_.buffer_wanted()) {");
  check(toggle != std::string::npos && source.find("stop_buffer_with_timeline(host_);", toggle) != std::string::npos,
        "the buffer toggle (hotkey, tray, panel) is the same manual stop");
}

// Only a lock and an unlock are a lock state. Every other WTS notification used to be read as an unlock, which
// started the buffer on a machine nobody was sitting at (a logoff, a disconnect, a logon).
void test_session_lock_commands() {
  std::printf("session lock commands\n");
  check(bx::session_lock_command(bx::kWtsSessionLock) == 100, "the session locking pauses the buffer");
  check(bx::session_lock_command(bx::kWtsSessionUnlock) == 101, "the session unlocking brings it back");
  // WTS_SESSION_LOGON 0x1, WTS_SESSION_LOGOFF 0x6, WTS_SESSION_DISCONNECT 0x2, WTS_CONSOLE_CONNECT 0x4.
  check(bx::session_lock_command(0x1) == 0, "a logon is not a lock state");
  check(bx::session_lock_command(0x6) == 0, "a logoff is not a lock state");
  check(bx::session_lock_command(0x2) == 0, "a disconnect is not a lock state");
  check(bx::session_lock_command(0x4) == 0, "a console connect is not a lock state");
}

#ifdef BUFFRX_SOURCE_DIR
std::string read_engine_source(const char* relative) {
  std::string text;
  if (FILE* file = std::fopen((source_root() + relative).c_str(), "rb")) {
    char buffer[8192];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, read);
    std::fclose(file);
  }
  return text;
}
#endif

// What the window thread does when it hears a notification or the person quits. It used to call the host itself,
// with no engine lock, while the pipe server was already answering clients; and the modal heartbeat never marked
// the window thread alive, so a 4 s drag was reported as a hang. Read where it is written, for the same reason as
// the manual stop above: no unit test can reach ObsHost or a message loop without libobs and a session.
void test_window_thread_routes() {
  std::printf("window thread routes\n");
#ifdef BUFFRX_SOURCE_DIR
  const std::string main_source = read_engine_source("/src/engine/main.cpp");
  const std::string platform_source = read_engine_source("/src/engine/platform.cpp");
  const std::string platform_header = read_engine_source("/src/engine/platform.h");
  check(!main_source.empty() && !platform_source.empty() && !platform_header.empty(),
        "the window thread's routes are checked against main.cpp and platform.cpp");

  // The lock and the unlock queue the host work; the window thread keeps only the controller poller, which has
  // its own lock.
  const size_t locked = main_source.find("case 100:  // session locked");
  const size_t locked_end = locked == std::string::npos ? std::string::npos : main_source.find("break;", locked);
  const std::string locked_route =
      locked == std::string::npos ? std::string() : main_source.substr(locked, locked_end - locked);
  check(locked_route.find("queue_action(\"pause_for_lock\")") != std::string::npos,
        "the session locking queues the pause");
  check(locked_route.find("host.stop_buffer") == std::string::npos,
        "the session locking does not stop the buffer on the window thread");
  const size_t unlocked = main_source.find("case 101:  // session unlocked");
  const size_t unlocked_end = unlocked == std::string::npos ? std::string::npos : main_source.find("break;", unlocked);
  const std::string unlocked_route =
      unlocked == std::string::npos ? std::string() : main_source.substr(unlocked, unlocked_end - unlocked);
  check(unlocked_route.find("queue_action(\"resume_after_lock\")") != std::string::npos,
        "the session unlocking queues the start");
  check(unlocked_route.find("host.start_buffer") == std::string::npos,
        "the session unlocking does not start the buffer on the window thread");

  // The action worker does the host work, under the engine lock run_action holds.
  const size_t pause = main_source.find("action == \"pause_for_lock\"");
  const size_t pause_end = pause == std::string::npos ? std::string::npos : main_source.find("} else if (action ==", pause);
  check(pause != std::string::npos && main_source.substr(pause, pause_end - pause).find("host.stop_buffer()") != std::string::npos,
        "the queued pause stops the buffer on the action worker");
  const size_t resume = main_source.find("action == \"resume_after_lock\"");
  const size_t resume_end = resume == std::string::npos ? std::string::npos : main_source.find("} else if (action ==", resume);
  check(resume != std::string::npos && main_source.substr(resume, resume_end - resume).find("host.start_buffer") != std::string::npos,
        "the queued resume starts it there too");

  // Quit: the box stays on the window thread, the stops are queued, and app.quit still goes out.
  const size_t quit = main_source.find("case PlatformWindow::kMenuQuit:");
  const size_t quit_end = quit == std::string::npos ? std::string::npos : main_source.find("case PlatformWindow::kMenuQuitNow:", quit);
  const std::string quit_route = quit == std::string::npos ? std::string() : main_source.substr(quit, quit_end - quit);
  const size_t asked = quit_route.find("MessageBoxA");
  const size_t queued = quit_route.find("queue_action(\"quit_now\")");
  check(asked != std::string::npos && queued != std::string::npos && asked < queued,
        "Quit asks on the window thread, then queues the stop");
  check(quit_route.find("host.stop_recording") == std::string::npos && quit_route.find("host.stop_timeline") == std::string::npos,
        "Quit does not stop an output on the window thread");
  check(quit_route.find("app.quit") != std::string::npos, "Quit still tells the interface and the editor");
  const size_t quit_action = main_source.find("action == \"quit_now\"");
  const size_t quit_action_end =
      quit_action == std::string::npos ? std::string::npos : main_source.find("} else if (action ==", quit_action);
  const std::string quit_action_body =
      quit_action == std::string::npos ? std::string() : main_source.substr(quit_action, quit_action_end - quit_action);
  check(quit_action_body.find("host.stop_timeline()") != std::string::npos &&
            quit_action_body.find("host.stop_recording()") != std::string::npos,
        "the queued quit stops the Timeline and the recording under the engine lock");

  // B3: the startup start keeps the engine lock open across host.start_buffer.
  const size_t startup = main_source.find("if (config.start_buffering && !restarted)");
  const size_t startup_end = startup == std::string::npos ? std::string::npos : main_source.find("} else {", startup);
  const std::string startup_block =
      startup == std::string::npos ? std::string() : main_source.substr(startup, startup_end - startup);
  check(startup_block.find("engine_lock(protocol.engine_mutex())") != std::string::npos,
        "the startup start takes the engine lock");
  check(startup_block.find("host.start_buffer(buffer_error)") != std::string::npos,
        "the startup start runs inside that lock");

  // B5/B6: the modal heartbeat marks the window thread alive, and the tray menu runs it.
  const size_t beat = main_source.find("auto beat_tick = [&](uint32_t now,");
  const size_t beat_end = beat == std::string::npos ? std::string::npos : main_source.find("if (now - last_tick >= 250)", beat);
  check(beat != std::string::npos && beat_end != std::string::npos &&
            main_source.substr(beat, beat_end - beat).find("heartbeat = now;") != std::string::npos,
        "beat_tick marks the window thread alive");
  check(main_source.find("heartbeat = loop_started") == std::string::npos,
        "the loop no longer marks it twice");
  check(main_source.find("ui_host.set_modal_heartbeat(modal_beat);") != std::string::npos,
        "the panel's modal loop runs the same beat");
  check(main_source.find("window.set_modal_heartbeat(modal_beat);") != std::string::npos,
        "the tray menu runs it too");

  // B2/B7/B6 in platform.cpp: only a lock and an unlock move the buffer, a session end asks for a quit with no
  // box, and the menu's timer keeps the tick going.
  check(platform_source.find("session_lock_command(static_cast<unsigned long>(wparam))") != std::string::npos,
        "the session notification is read through the one mapping");
  check(platform_source.find("? 100 : 101") == std::string::npos,
        "no other notification is read as an unlock");
  const size_t endsession = platform_source.find("if (message == WM_ENDSESSION)");
  const size_t endsession_end =
      endsession == std::string::npos ? std::string::npos : platform_source.find("if (message == WM_WTSSESSION_CHANGE)", endsession);
  const std::string endsession_route =
      endsession == std::string::npos ? std::string() : platform_source.substr(endsession, endsession_end - endsession);
  check(endsession_route.find("kMenuQuitNow") != std::string::npos, "a session end takes the quit with no confirmation");
  check(endsession_route.find("PostQuitMessage(0)") != std::string::npos, "and ends the loop instead of blocking the logoff");
  check(platform_source.find("SetTimer(window_, kMenuHeartbeatTimer, 100, nullptr)") != std::string::npos &&
            platform_source.find("KillTimer(window_, kMenuHeartbeatTimer)") != std::string::npos,
        "the tray menu keeps the tick beating while it is open");
  check(platform_header.find("session_locked") == std::string::npos, "the dead lock state is gone");
  check(platform_header.find("bool recording_ = false") == std::string::npos, "the dead tray recording flag is gone");
#else
  check(false, "BUFFRX_SOURCE_DIR is defined, so the window thread's routes can be read");
#endif
}

// The native interface's own lifetimes: the hotkey recorder let go of a window that hides or is destroyed, an editor
// that never shows its window, a lost graphics device, a DPI change's font rebuild, the clipboard poll and an input
// for a window that is not up yet. Read where they are written, like the window thread's routes: no unit test can
// reach a window, a swap chain or a process without libobs and a session.
void test_ui_host_lifetimes() {
  std::printf("ui host lifetimes\n");
#ifdef BUFFRX_SOURCE_DIR
  const std::string host = read_engine_source("/src/engine/ui/ui_host.cpp");
  const std::string header = read_engine_source("/src/engine/ui/ui_host.h");
  check(!host.empty() && !header.empty(), "the native interface is checked where it is written");
  // A function's body: from its signature to the first close brace in column 0.
  auto body = [&](const char* signature) {
    const size_t start = host.find(signature);
    if (start == std::string::npos) return std::string();
    const size_t end = host.find("\n}\n", start);
    return host.substr(start, end == std::string::npos ? std::string::npos : end - start);
  };

  // B14: hiding or destroying the panel while the recorder listens gives every global hotkey back.
  check(body("void UiHost::begin_hide").find("cancel_recorder();") != std::string::npos,
        "hiding the panel cancels the hotkey recorder");
  check(body("void UiHost::destroy_window").find("cancel_recorder();") != std::string::npos,
        "destroying the panel cancels it too");
  check(body("void UiHost::destroy_window").find("window.get() == main_.get()") != std::string::npos,
        "the cancel belongs to the panel's window");

  // B15: an editor that never puts its window inside the panel is given up on, and ended after a grace period.
  check(host.find("kEditorLoadTimeoutMs") != std::string::npos && host.find("kEditorKillGraceMs") != std::string::npos,
        "the editor's loading has a deadline and a kill grace");
  check(body("void UiHost::start_editor").find("editor_loading_since_ = GetTickCount();") != std::string::npos,
        "opening the editor starts its deadline");
  check(body("void UiHost::frame").find("editor_loading_since_") != std::string::npos &&
            body("void UiHost::frame").find("TerminateProcess") != std::string::npos,
        "the frame ends an editor that never showed its window");
  check(body("void UiHost::frame").find("finish_editor();") != std::string::npos,
        "the panel comes back from a loading editor");
  check(host.find("PROCESS_TERMINATE") != std::string::npos, "the editor is opened with the right to end it");

  // B16: a removed or reset device is noticed, and the next pass rebuilds it instead of drawing into it.
  check(host.find("DXGI_ERROR_DEVICE_REMOVED") != std::string::npos && host.find("DXGI_ERROR_DEVICE_RESET") != std::string::npos,
        "a removed or reset device is noticed");
  check(body("void UiHost::frame").find("device_lost_") != std::string::npos &&
            body("void UiHost::frame").find("release_device();") != std::string::npos,
        "the next pass rebuilds the device");
  check(body("void UiHost::render").find("!window.target") != std::string::npos, "a frame with no render target is not drawn");
  check(body("LRESULT UiHost::handle").find("FAILED(resized)") != std::string::npos &&
            body("LRESULT UiHost::handle").find("device_lost_ = true") != std::string::npos,
        "a failed resize is not drawn into");

  // B18: a DPI change defers the atlas rebuild out of the message and into the frame that presents it.
  check(body("LRESULT UiHost::handle").find("window.fonts_due = true") != std::string::npos,
        "a DPI change marks the fonts due");
  check(body("LRESULT UiHost::handle").find("!window.fonts_due") != std::string::npos,
        "the synchronous resize frame waits for the fonts");
  check(body("void UiHost::render").find("window.fonts_due = false") != std::string::npos,
        "the frame rebuilds the atlas before it presents");

  // B21: the clipboard is read with no window open too (the quick panel opens for a copied link), and a refused
  // clipboard backs off instead of being opened every pass.
  check(body("void UiHost::check_clipboard_for_link").find("if (!any_visible()) return;") == std::string::npos &&
            body("void UiHost::check_clipboard_for_link").find("clipboard_retry_at_") != std::string::npos,
        "the clipboard poll backs off without hiding the no-window offer");

  // B23: an input for a window that is not up yet waits a moment instead of being dropped.
  check(body("void UiHost::apply_inputs").find("kInputQueueMs") != std::string::npos &&
            body("void UiHost::apply_inputs").find("queued_ms") != std::string::npos &&
            body("void UiHost::apply_inputs").find("inputs_.insert") != std::string::npos,
        "input waits a moment for its window");
  check(header.find("struct InputRequest") != std::string::npos, "the queue carries the time it was asked for");
#else
  check(false, "BUFFRX_SOURCE_DIR is defined, so the native interface can be read");
#endif
}

// B17/B19/B20/B22: the interface's caches and motion maps are bounded and keyed to their window. The trim and prune
// run over ImGui textures and the strip's state, which need a window and an engine, so they are read where they are
// written, like the host's own lifetimes; the key rule itself is unit-tested in test_anim_math.
void test_ui_cache_and_motion_budgets() {
  std::printf("ui cache and motion budgets\n");
#ifdef BUFFRX_SOURCE_DIR
  const std::string images_cpp = read_engine_source("/src/engine/ui/ui_images.cpp");
  const std::string images_h = read_engine_source("/src/engine/ui/ui_images.h");
  const std::string host = read_engine_source("/src/engine/ui/ui_host.cpp");
  const std::string panel = read_engine_source("/src/engine/ui/ui_panel.cpp");
  const std::string live = read_engine_source("/src/engine/ui/ui_live.cpp");
  const std::string views = read_engine_source("/src/engine/ui/ui_views.h");
  const std::string widgets = read_engine_source("/src/engine/ui/ui_widgets.cpp");
  const std::string anim = read_engine_source("/src/engine/ui/ui_anim.h");
  check(!images_cpp.empty() && !images_h.empty() && !host.empty() && !panel.empty() && !live.empty() && !views.empty() &&
            !widgets.empty() && !anim.empty(),
        "the caches and maps are checked where they are written");

  // B17: the texture cache keeps a byte budget, not a count, and the preview at 100% is decoded no larger than the display.
  check(images_h.find("size_t bytes() const") != std::string::npos, "a cache entry knows what it costs in bytes");
  check(images_cpp.find("void ImageCache::trim(const Release& release, size_t max_bytes)") != std::string::npos,
        "the cache trims by bytes");
  check(images_cpp.find("total += item.second.bytes();") != std::string::npos &&
            images_cpp.find("i + 1 < order.size() && total > max_bytes") != std::string::npos,
        "the oldest entries go until the total fits, and the newest is never dropped");
  check(images_cpp.find("entries_.size() - keep") == std::string::npos, "no count-based trim is left");
  check(host.find("256u * 1024u * 1024u") != std::string::npos, "the cache is held to 256 MiB of textures");
  check(panel.find("panel.preview_zoom ? 8192 : 2560") == std::string::npos, "the preview no longer decodes at a flat 8192");
  check(panel.find("std::min(display_box, 8192)") != std::string::npos && views.find("display_w = 0, display_h = 0") != std::string::npos,
        "the 100% decode is capped at the display's pixels");

  // B19: the strip forgets pictures far from the view and the range, and a read no longer inserts into `asked`.
  check(live.find("void prune_thumbs(LiveStrip& strip, const LiveRange& view)") != std::string::npos,
        "the strip prunes what is off screen");
  check(live.find("prune_thumbs(strip, view);") != std::string::npos && views.find("pruned_start") != std::string::npos,
        "the prune runs when the view moves");
  check(live.find("int asked_count(const LiveStrip& strip, int64_t moment)") != std::string::npos &&
            live.find("asked_count(strip, moment) < kThumbTries") != std::string::npos && live.find("strip.asked[moment]") == std::string::npos,
        "a read asks the count with find instead of inserting");
  check(live.find("strip.thumbs.erase(it)") != std::string::npos && live.find("strip.asked.erase(it)") != std::string::npos &&
            live.find("strip.missed.erase(it)") != std::string::npos,
        "every map is pruned");
  check(live.find("whole_reply_empty || ++strip.missed[at] <= kThumbTries") != std::string::npos,
        "a busy lock is always retried; a picture that never comes stops being asked for");

  // B20: the palette is held by value, so pal() never reads a frame-local object.
  check(widgets.find("Palette g_palette{};") != std::string::npos && widgets.find("g_palette = *palette;") != std::string::npos,
        "the palette is copied into the widget module");
  check(widgets.find("g_palette = palette;") == std::string::npos && widgets.find("return *g_palette;") == std::string::npos,
        "no pointer to the frame's palette is kept");

  // B22: destroying a window purges every motion map entry that belongs to its context.
  const size_t start = host.find("void UiHost::destroy_window");
  const size_t end = start == std::string::npos ? std::string::npos : host.find("\n}\n", start);
  const std::string destroy = start == std::string::npos ? std::string() : host.substr(start, end - start);
  check(destroy.find("anim_purge_context(context);") != std::string::npos, "a destroyed window purges its motion entries");
  const size_t purge = anim.find("inline void anim_purge_context(ImGuiContext* context)");
  const size_t purge_end = purge == std::string::npos ? std::string::npos : anim.find("\n}\n", purge);
  const std::string purge_body = purge == std::string::npos ? std::string() : anim.substr(purge, purge_end - purge);
  check(anim.find("inline uint64_t context_key(const void* context)") != std::string::npos &&
            anim.find("(key & ~0xffffffffull) == owner") != std::string::npos,
        "a motion key packs its context in the high 32 bits, so one window's entries are told from another's");
  check(purge_body.find("if (!context) return;") != std::string::npos, "the purge is safe with no context");
  check(anim.find("a.context != context || frame < a.frame") != std::string::npos,
        "a list starts over in another context, or one made again at the old address");
  check(purge_body.find("appearing()") != std::string::npos && purge_body.find("tweens()") != std::string::npos &&
            purge_body.find("arrival_scopes()") != std::string::npos,
        "the purge covers the appearance, tween and arrival maps");
#else
  check(false, "BUFFRX_SOURCE_DIR is defined, so the caches can be read");
#endif
}


void test_crash_and_mark_lifetimes() {
  std::printf("crash and mark lifetimes\n");
#ifdef BUFFRX_SOURCE_DIR
  const std::string crash_source = read_engine_source("/src/engine/crash_log.cpp");
  const std::string mark_source = read_engine_source("/src/engine/rec_mark.cpp");
  check(!crash_source.empty() && !mark_source.empty(), "the handlers are checked where they are written");
  check(crash_source.find("std::current_exception()") != std::string::npos,
        "the terminate handler asks for the exception it was given");
  check(crash_source.find("throw;") == std::string::npos,
        "a bare throw with no active exception is not there any more");
  const size_t updated = mark_source.find("UpdateLayeredWindow(window_, screen");
  const size_t released = mark_source.find("ReleaseDC(nullptr, screen)", updated == std::string::npos ? 0 : updated);
  check(updated != std::string::npos && released != std::string::npos,
        "the mark releases its screen DC after the layered update");
#else
  check(false, "BUFFRX_SOURCE_DIR is defined, so the handlers can be read");
#endif
}

// A refused patch and a range command are read where they are written, for the same reason as the
// manual stop above: no unit test reaches Protocol without libobs.
void test_command_guards() {
  std::printf("command guards\n");
  std::string source;
  if (FILE* file = std::fopen((source_root() + "/src/engine/commands.cpp").c_str(), "rb")) {
    char buffer[8192];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) source.append(buffer, read);
    std::fclose(file);
  }
  check(!source.empty(), "the guards are checked against commands.cpp");

  // A patch the host refuses changed nothing: leaving its values in the config showed a setting the
  // engine never took, and the next successful patch wrote it to disk.
  const std::string apply_head = "if (!host_.apply(config_, restart, error)) {";
  const size_t apply = source.find(apply_head);
  const size_t apply_end =
      apply == std::string::npos ? std::string::npos : source.find("return Json::object();", apply);
  check(apply != std::string::npos && apply_end != std::string::npos &&
            source.find("config_ = before;", apply) < apply_end,
        "a refused patch puts the config back before it answers");

  // A range is clamped before the session's start is added to it, or INT64_MAX overflowed the sum.
  const auto branch = [&source](const std::string& head) {
    const size_t at = source.find(head);
    const size_t end = at == std::string::npos ? std::string::npos : source.find("\n  if (command ==", at + 1);
    return at == std::string::npos || end == std::string::npos ? std::string() : source.substr(at, end - at);
  };
  const std::string clamp = "std::clamp<int64_t>(args[\"start_ms\"].as_int(-1), 0, UINT32_MAX)";
  const std::string pin = branch("if (command == \"timeline.pin\")");
  const std::string export_range = branch("if (command == \"clips.export_range\")");
  check(!pin.empty() && !export_range.empty(), "the two range commands are found");
  check(pin.find(clamp) != std::string::npos && export_range.find(clamp) != std::string::npos,
        "timeline.pin and clips.export_range both clamp the start of a range");
  const std::string raw = "const int64_t start_ms = args[\"start_ms\"].as_int(-1);";
  check(pin.find(raw) == std::string::npos && export_range.find(raw) == std::string::npos,
        "and neither adds the unclamped count to the session's start");

  const size_t get = source.find("if (command == \"session.get\")");
  const size_t get_end =
      get == std::string::npos ? std::string::npos : source.find("if (command == \"timeline.get\")", get);
  check(get != std::string::npos && get_end != std::string::npos &&
            source.substr(get, get_end - get).find("throw std::runtime_error") == std::string::npos,
        "session.get reports a missing session without building a dead object");
}

// A reply the wire cannot carry, a queue wait that must end, the slot's close, the interface's dialog lane and the
// removed permanent screenshot delete: each is read where it is written, because a slot, the pipe's workers, the
// interface's lanes and the protocol's commands need a client, a window or libobs to run.
void test_pipe_and_interface_guards() {
  std::printf("pipe and interface guards\n");
#ifdef BUFFRX_SOURCE_DIR
  const std::string pipe_source = read_engine_source("/src/shared/pipe.h");
  const std::string main_source = read_engine_source("/src/engine/main.cpp");
  const std::string bridge_header = read_engine_source("/src/engine/ui/ui_bridge.h");
  const std::string bridge_source = read_engine_source("/src/engine/ui/ui_bridge.cpp");
  const std::string commands = read_engine_source("/src/engine/commands.cpp");
  const std::string capabilities = read_engine_source("/src/engine/protocol_commands.h");
  const std::string page = read_engine_source("/ui/src/lib/api.ts");
  check(!pipe_source.empty() && !main_source.empty() && !bridge_header.empty() && !bridge_source.empty() && !commands.empty() &&
            !capabilities.empty() && !page.empty(),
        "the guards are checked where they are written");

  // C1: the slot thread closes the handle under the write lock, so no worker can be inside WriteFile on it.
  const size_t slot = pipe_source.find("void slot_loop(std::shared_ptr<Client> client) {");
  const size_t slot_end = slot == std::string::npos ? std::string::npos : pipe_source.find("\n  }", slot);
  const std::string slot_body =
      slot == std::string::npos || slot_end == std::string::npos ? std::string() : pipe_source.substr(slot, slot_end - slot);
  const size_t close_at = slot_body.find("close_pipe(*client);");
  const size_t close_lock =
      slot_body.rfind("std::lock_guard<std::mutex> write_lock(client->write_mutex);", close_at);
  check(close_at != std::string::npos && close_lock != std::string::npos && close_lock < close_at,
        "the slot closes its handle under the write lock");
  check(slot_body.find("close_events(*client);", close_at) != std::string::npos, "and its events with it");

  // C2: a reply over the frame limit becomes a small refusal under the request's own id, never a dropped client.
  check(main_source.find("if (response.size() > kMaxFrameBytes)") != std::string::npos &&
            main_source.find("\"the reply is too large; ask for less\"") != std::string::npos,
        "a reply over the frame limit is refused in a frame the client can read");

  // C4: the wait for queue room is bounded, and a command that finds none is refused through the busy handler.
  check(pipe_source.find("#include <chrono>") != std::string::npos &&
            pipe_source.find("using BusyHandler = std::function<void(int client, const std::string& payload)>;") !=
                std::string::npos &&
            pipe_source.find("BusyHandler on_busy = {}") != std::string::npos &&
            pipe_source.find("pending_room_.wait_for(lock, std::chrono::seconds(2)") != std::string::npos &&
            pipe_source.find("on_busy_(client, payload)") != std::string::npos,
        "the queue's wait is bounded and a command that finds no room is refused");
  check(main_source.find("\"the engine is busy; try again\"") != std::string::npos,
        "the engine answers a command it had no room for");

  // C3: a picker holds until the person answers, so it has a lane of its own, ahead of every other rule.
  check(bridge_header.find("Worker locked_, lock_free_, media_, dialog_;") != std::string::npos,
        "the interface has a lane of its own for a dialog");
  check(bridge_source.find("dialog_.thread = std::thread([this] { run(dialog_); });") != std::string::npos &&
            bridge_source.find("if (dialog_.thread.joinable()) dialog_.thread.join();") != std::string::npos &&
            bridge_source.find("dialog_.jobs.clear();") != std::string::npos,
        "the dialog lane starts, joins and clears with the others");
  check(bridge_source.find("command == \"app.pick_folder\" || command == \"app.pick_file\"") != std::string::npos &&
            bridge_source.find("Worker& worker = dialog ? dialog_ :") != std::string::npos,
        "both pickers are routed to the dialog lane");

  // C15: a screenshot has no permanent delete, and nothing still offers one.
  check(commands.find("command == \"clips.delete\" || command == \"screenshots.delete\"") == std::string::npos &&
            commands.find("if (command == \"clips.delete\") {") != std::string::npos,
        "only clips.delete is handled permanently");
  check(capabilities.find("\"screenshots.delete\"") == std::string::npos, "the capability list no longer offers it");
  check(page.find("screenshots.delete") == std::string::npos && page.find("remove: (item: MediaItem)") == std::string::npos,
        "the page has no permanent delete left");

  // C14: the probe's output is a PNG and nothing else, in any folder.
  const size_t capture = commands.find("if (command == \"ui.capture\")");
  const size_t capture_end = capture == std::string::npos ? std::string::npos : commands.find("\n  if (command ==", capture + 1);
  const std::string capture_body =
      capture == std::string::npos || capture_end == std::string::npos ? std::string() : commands.substr(capture, capture_end - capture);
  check(capture_body.find("extension != \".png\"") != std::string::npos &&
            capture_body.find("\"the capture file must end in .png\"") != std::string::npos,
        "ui.capture refuses a file that does not end in .png");
#else
  check(false, "BUFFRX_SOURCE_DIR is defined, so the pipe, the interface and the commands can be read");
#endif
}

// Lifetimes that cost a stalled engine or a leaked encoder, read where they are written: no unit test reaches
// Protocol or ObsHost without libobs.
void test_lifetime_guards() {
  std::printf("lifetime guards\n");
  const auto read = [](const std::string& path) {
    std::string text;
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
      char buffer[8192];
      size_t read = 0;
      while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, read);
      std::fclose(file);
    }
    return text;
  };
  const std::string commands = read(source_root() + "/src/engine/commands.cpp");
  const std::string host = read(source_root() + "/src/engine/obs_host.cpp");
  const std::string header = read(source_root() + "/src/engine/obs_host.h");
  const std::string dedupe = read(source_root() + "/src/engine/obs_host_dedupe.cpp");
  check(!commands.empty() && !host.empty() && !header.empty() && !dedupe.empty(), "the lifetimes are checked against their sources");

  // B10: starting on a running game switches Performance mode only when no manual recording is being written;
  // otherwise the switch split the recording, which is the deferral update_performance_for_game already has.
  const size_t start = commands.find("void Protocol::start_on_running_game()");
  const size_t start_end = start == std::string::npos ? std::string::npos : commands.find("\n}", start);
  const std::string start_body = start == std::string::npos ? std::string() : commands.substr(start, start_end - start);
  const size_t deferred = start_body.find("host_.recording_busy()");
  const size_t switched = start_body.find("set_performance_game");
  check(deferred != std::string::npos && switched != std::string::npos && deferred < switched,
        "starting on a game defers the Performance switch while a recording runs");

  // B11: dropping the dedupe state must not join a worker that may sit in media_chunk_probe on the engine lock.
  check(dedupe.find("struct Shared {") != std::string::npos, "the dedupe worker reads a shared state block");
  check(dedupe.find("if (worker.joinable()) worker.detach();") != std::string::npos,
        "the dedupe state is let go without joining under the engine lock");
  check(host.find("dedupe_shutdown();") != std::string::npos, "shutdown joins the dedupe worker before libobs goes");

  // B12: the abandoned output's own encoders are released by the reaper after the output, never dropped.
  check(host.find("reap_output(output, std::string(), abandoned_video, std::move(abandoned_audio))") != std::string::npos,
        "the abandoned output's encoders are handed to the reaper");
  check(host.find("obs_encoder_release(job.video_encoder)") != std::string::npos,
        "the reaper releases the abandoned video encoder");
  check(header.find("obs_encoder* video_encoder = nullptr;") != std::string::npos,
        "the reap job carries the abandoned encoders");
  const size_t abandoning = host.find("abandoning it");
  const std::string after_abandon = abandoning == std::string::npos ? std::string() : host.substr(abandoning, 1200);
  check(abandoning != std::string::npos && after_abandon.find("recording_audio_encoders_.clear()") == std::string::npos,
        "the abandoned audio encoders are not dropped without a release");

  // B13: the display fallback warning is remembered by the host, not by a static written from several threads.
  check(host.find("static std::string warned") == std::string::npos, "no function-local static remembers the missing display");
  check(header.find("std::string display_warned_name_;") != std::string::npos, "the host owns that memory");
  check(host.find("monitor_device_name(config_, &display_warned_name_)") != std::string::npos,
        "the 2 s display check passes the host's memory");
}

// Alt+4 closes the preview that is open, else opens it, on the newest footage or on its empty state.
void test_timeline_preview_press() {
  using bx::TimelinePress;
  std::printf("timeline preview press\n");
  check(bx::timeline_press_action(true) == TimelinePress::close_preview, "Alt+4 closes the preview that is open");
  check(bx::timeline_press_action(false) == TimelinePress::open_preview,
        "with nothing recorded Alt+4 still opens the Timeline preview");
}

// The preview cannot be drawn over a game in exclusive fullscreen, and a game says which kind it is only
// by leaving the screen for it: that game is put back and explained once, and the next press opens over
// the desktop. A game the person minimized themselves is not touched.
void test_timeline_overlay() {
  std::printf("timeline overlay\n");
  check(bx::timeline_overlay_probe_wanted(true, false), "a game in front is looked at after the preview opens");
  check(!bx::timeline_overlay_probe_wanted(true, true), "a game that already said so is not probed again");
  check(!bx::timeline_overlay_probe_wanted(false, false), "no game in front means nothing to look at");

  check(bx::timeline_overlay_restore_wanted(true, true), "a game the preview took off the screen is put back");
  check(!bx::timeline_overlay_restore_wanted(true, false), "a game still on screen is left alone");
  check(!bx::timeline_overlay_restore_wanted(false, true), "nothing armed means the desktop is not touched");
}

// Task 6 of the Timeline: footage older than the window goes, an active pin keeps what it covers, the
// chunk a writer still holds is never touched, and a normal recording is not the Timeline's to delete.
void test_timeline_retention() {
  using bx::SessionChunk;
  using bx::SessionEntry;
  using bx::TimelinePin;
  std::printf("timeline retention\n");
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const int64_t now = 1000000000000LL;  // the wall clock, milliseconds since 1970
  const int64_t minute = 60000;
  std::string error;

  SessionEntry previous_session;
  previous_session.id = "previous";
  SessionEntry current_session;
  current_session.id = "current";  // no published chunks yet
  const std::vector<SessionEntry> choices{previous_session, current_session};
  check(bx::timeline_choose_session(choices, "", "current", false)->id == "current",
        "the active session wins even before its first chunk is published");
  check(bx::timeline_choose_session(choices, "", "", true) == nullptr,
        "an older session is not selected while the writer starts");
  check(bx::timeline_choose_session(choices, "previous", "current", false)->id == "previous",
        "an explicitly requested older session remains accessible");

  check(!bx::timeline_pin_active(TimelinePin{}, now), "a pin with no lease is never active");
  TimelinePin pin;
  pin.id = "pin-1";
  pin.expires_ms = now + minute;
  check(bx::timeline_pin_active(pin, now), "a pin whose lease has not run out is active");
  check(!bx::timeline_pin_active(pin, now + 2 * minute), "an expired lease protects nothing");

  pin.start_ms = now - 2 * minute;
  pin.end_ms = now - minute;
  check(bx::timeline_pin_covers(pin, pin.start_ms - minute, pin.start_ms + 1),
        "a pin covers a chunk that starts inside it");
  check(bx::timeline_pin_covers(pin, pin.end_ms - 1, pin.end_ms + minute),
        "and one that ends inside it");
  check(!bx::timeline_pin_covers(pin, pin.end_ms + 1, pin.end_ms + minute), "a chunk past it is not covered");

  // The sweep is due once a minute; the first one always is.
  check(!bx::timeline_prune_due(now - 59999, now), "a sweep 59.999 s after the last one is not due");
  check(bx::timeline_prune_due(now - 60000, now), "a sweep 60 s after the last one is due");
  check(bx::timeline_prune_due(0, now), "the first sweep is due");

  // Chunk 0 ends inside the window, chunk 1 ends outside it, chunk 2 is outside it too but is pinned.
  // Chunk 3 ends exactly on the window's edge, which is still inside.
  std::vector<SessionChunk> chunks(4);
  chunks[0].file = "fresh";
  chunks[0].wall_start_ms = now - 10 * minute;
  chunks[0].wall_end_ms = now - 5 * minute;
  chunks[1].file = "old";
  chunks[1].wall_start_ms = now - 40 * minute;
  chunks[1].wall_end_ms = now - 35 * minute;
  chunks[2].file = "pinned";
  chunks[2].wall_start_ms = now - 90 * minute;
  chunks[2].wall_end_ms = now - 80 * minute;
  chunks[3].file = "edge";
  chunks[3].wall_start_ms = now - 31 * minute;
  chunks[3].wall_end_ms = now - 30 * minute;

  TimelinePin middle;
  middle.id = "pin-middle";
  middle.start_ms = chunks[2].wall_start_ms;
  middle.end_ms = chunks[2].wall_end_ms;
  middle.expires_ms = now + minute;
  std::vector<size_t> expired = bx::timeline_expired_chunks(chunks, 30, now, {middle}, now);
  check(expired.size() == 1 && expired[0] == 1,
        "a chunk inside the window is kept, one outside expires, an active pin keeps its own, and the edge is inside");

  middle.expires_ms = now - minute;  // the lease ran out
  expired = bx::timeline_expired_chunks(chunks, 30, now, {middle}, now);
  check(expired.size() == 2 && expired[0] == 1 && expired[1] == 2,
        "an expired lease no longer protects the chunk it covered");

  // The live session's open chunk is already in the index with a wall time; it is still being written.
  std::vector<SessionChunk> live(1);
  live[0].file = "writing";
  live[0].wall_start_ms = now - 3 * 60 * minute;
  live[0].wall_end_ms = now - 3 * 60 * minute + minute;
  live[0].published = false;
  check(bx::timeline_expired_chunks(live, 30, now, {}, now).empty(),
        "a 30-minute window never expires the live session's open chunk");

  // On disk: one normal recording and one timeline session of three chunks, all older than the window.
  const std::string root = temp_directory() + "\\timeline-retention";
  bx::session_store_init(root);
  SessionEntry empty_closed;
  check(bx::session_store_begin("timeline", empty_closed, error) && bx::session_store_close(empty_closed, 1, error),
        "an empty timeline can close");
  SessionEntry empty_live;
  check(bx::session_store_begin("timeline", empty_live, error), "an empty active timeline begins");
  SessionEntry empty_abandoned;
  check(bx::session_store_begin("timeline", empty_abandoned, error), "an abandoned timeline begins");
  empty_abandoned.started_at = (now - 60 * minute) / 1000;
  // Markers live outside the index now, so the old start time is written the way the store writes it.
  auto store_start = [](const SessionEntry& entry) {
    FILE* file = std::fopen((bx::session_store_root() + "\\" + entry.id + "\\index.json").c_str(), "wb");
    if (!file) return false;
    const std::string text = bx::session_entry_json(entry).dump();
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    std::fclose(file);
    return written;
  };
  check(store_start(empty_abandoned), "its old start time is stored");
  DeleteFileA(bx::session_writer_state_path(empty_abandoned.id).c_str());
  SessionEntry unknown_media;
  check(bx::session_store_begin("timeline", unknown_media, error), "an abandoned session with unknown media begins");
  unknown_media.started_at = (now - 60 * minute) / 1000;
  check(store_start(unknown_media), "its old start time is stored");
  const std::string unknown_file = bx::session_media_dir(unknown_media.id) + "\\unknown.dat";
  write_bytes(unknown_file, 16);
  DeleteFileA(bx::session_writer_state_path(unknown_media.id).c_str());
  set_test_leases({});
  check(bx::session_store_prune_timeline(30, now, error), "retention sweeps empty sessions");
  SessionEntry checked_empty;
  check(!bx::session_store_load_one(empty_closed.id, checked_empty, error), "a closed empty timeline is cleaned up");
  check(bx::session_store_load_one(empty_live.id, checked_empty, error), "an active empty timeline is kept");
  check(!bx::session_store_load_one(empty_abandoned.id, checked_empty, error), "an expired empty abandoned timeline is cleaned up");
  check(bx::session_store_load_one(unknown_media.id, checked_empty, error) &&
            GetFileAttributesA(unknown_file.c_str()) != INVALID_FILE_ATTRIBUTES,
        "unknown media in an abandoned session is preserved");
  SessionEntry recording;
  check(bx::session_store_begin("recording", recording, error), "a normal recording begins");
  recording.started_at = 1792310400;
  const std::string recording_file = bx::session_media_dir(recording.id) + "\\recording.mkv";
  write_bytes(recording_file, 1024);
  SessionChunk recording_chunk;
  recording_chunk.file = recording_file;
  recording_chunk.start_ms = 1000;
  recording_chunk.end_ms = 2000;
  recording_chunk.wall_start_ms = now - 3 * 60 * minute;
  recording_chunk.wall_end_ms = now - 3 * 60 * minute + minute;
  check(bx::session_store_append_chunk(recording, recording_chunk, error), "its chunk is indexed");
  check(bx::session_store_close(recording, 2000, error), "it closes");

  SessionEntry timeline;
  check(bx::session_store_begin("timeline", timeline, error), "a timeline session begins");
  timeline.started_at = 1792310401;
  std::vector<std::string> files;
  for (int index = 0; index < 3; ++index) {
    const std::string file = bx::session_media_dir(timeline.id) + "\\chunk-" + std::to_string(index) + ".mp4";
    write_bytes(file, 1024);
    SessionChunk chunk;
    chunk.file = file;
    chunk.start_ms = static_cast<uint32_t>(index) * 1000;
    chunk.end_ms = static_cast<uint32_t>(index + 1) * 1000;
    chunk.wall_start_ms = now - 3 * 60 * minute + index * minute;
    chunk.wall_end_ms = now - 3 * 60 * minute + (index + 1) * minute;
    check(bx::session_store_append_chunk(timeline, chunk, error), "a timeline chunk is indexed");
    files.push_back(file);
  }
  check(bx::session_store_close(timeline, 2000, error), "the timeline session closes");

  TimelinePin keep_middle;
  keep_middle.id = "pin-middle";
  keep_middle.session_id = timeline.id;
  keep_middle.start_ms = timeline.chunks[1].wall_start_ms;
  keep_middle.end_ms = timeline.chunks[1].wall_end_ms;
  keep_middle.expires_ms = now + minute;
  const std::vector<TimelinePin> pins{keep_middle};
  set_test_leases(pins);
  check(bx::session_store_prune_timeline(30, now, error) && error.empty(), "the timeline prune succeeds");
  check(GetFileAttributesA(files[0].c_str()) == INVALID_FILE_ATTRIBUTES, "the unpinned chunk before the pin is gone");
  check(GetFileAttributesA(files[2].c_str()) == INVALID_FILE_ATTRIBUTES, "the unpinned chunk after it is gone");
  check(GetFileAttributesA(files[1].c_str()) != INVALID_FILE_ATTRIBUTES, "the pinned chunk's file stays");
  SessionEntry pruned;
  check(bx::session_store_load_one(timeline.id, pruned, error) && pruned.chunks.size() == 1 &&
            pruned.chunks[0].file == files[1],
        "the index loads and names only the kept chunk");
  SessionEntry untouched;
  check(bx::session_store_load_one(recording.id, untouched, error) && untouched.chunks.size() == 1,
        "a normal recording is never returned by the timeline prune");

  // Erasing one temporary session. A normal recording is refused.
  std::vector<std::string> locked;
  check(bx::session_store_erase_timeline(timeline.id, locked, error) && locked.empty(),
        "erasing a closed timeline session succeeds");
  check(GetFileAttributesA(bx::session_media_dir(timeline.id).c_str()) == INVALID_FILE_ATTRIBUTES,
        "its directory is gone");
  check(!bx::session_store_erase_timeline(recording.id, locked, error) && !error.empty(),
        "a normal recording cannot be erased as a timeline session");

  // A directory left behind means the session still exists: the erase is not reported as done.
  SessionEntry stuck;
  check(bx::session_store_begin("timeline", stuck, error), "a timeline session begins for the leftover check");
  const std::string leftover = bx::session_media_dir(stuck.id) + "\\leftover";
  CreateDirectoryA(leftover.c_str(), nullptr);
  check(!bx::session_store_erase_timeline(stuck.id, locked, error) && !error.empty(),
        "a leftover directory is reported instead of claiming the erase");
  RemoveDirectoryA(leftover.c_str());
  check(bx::session_store_erase_timeline(stuck.id, locked, error) && locked.empty(),
        "the erase succeeds once the leftover directory is gone");

  // P1-5: the open session's own list is trimmed by the tick, the sweep leaves an open session alone, and an append after
  // the trim never brings an expired chunk back.
  {
    SessionEntry live;
    check(bx::session_store_begin("timeline", live, error), "an open timeline session begins for the index trim");
    std::vector<std::string> live_files;
    const int64_t ends[6] = {now - 90 * minute, now - 80 * minute, now - 70 * minute, now - 20 * minute, now - 10 * minute, now - 1 * minute};
    for (int index = 0; index < 6; ++index) {
      const std::string file = bx::session_media_dir(live.id) + "\\live-" + std::to_string(index) + ".mp4";
      write_bytes(file, 512);
      SessionChunk chunk;
      chunk.file = file;
      chunk.start_ms = static_cast<uint32_t>(index) * 1000;
      chunk.end_ms = static_cast<uint32_t>(index + 1) * 1000;
      chunk.wall_start_ms = ends[index] - minute;
      chunk.wall_end_ms = ends[index];
      check(bx::session_store_append_chunk(live, chunk, error), "an open session's chunk is indexed");
      live_files.push_back(file);
    }
    TimelinePin keep;
    keep.id = "pin-live";
    keep.session_id = live.id;
    keep.start_ms = live.chunks[1].wall_start_ms;
    keep.end_ms = live.chunks[1].wall_end_ms;
    keep.expires_ms = now + minute;
    const std::vector<TimelinePin> live_pins{keep};
    set_test_leases(live_pins);
    check(bx::session_store_prune_timeline(30, now, error, live.id) && error.empty(), "a sweep that spares the open session succeeds");
    SessionEntry sweep_read;
    check(bx::session_store_load_one(live.id, sweep_read, error) && sweep_read.chunks.size() == 6 &&
              GetFileAttributesA(live_files[0].c_str()) != INVALID_FILE_ATTRIBUTES,
          "a sweep leaves the open session's index and files to its writer");

    const std::vector<size_t> past = bx::timeline_expired_chunks(live.chunks, 30, now, live_pins, now);
    check(past.size() == 2 && past[0] == 0 && past[1] == 2, "two chunks are past the window and the pinned one is kept");
    std::vector<bx::DroppedChunk> dropped;
    check(bx::session_store_drop_chunks(live, past, dropped, error) && live.chunks.size() == 4 && dropped.size() == 2,
          "the tick drops them from the open session's list");
    check(GetFileAttributesA(live_files[0].c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesA((live_files[0] + ".expired").c_str()) != INVALID_FILE_ATTRIBUTES,
          "a dropped file is renamed, so a crash before the delete leaves nothing recovery would index as footage");
    SessionEntry trimmed;
    check(bx::session_store_load_one(live.id, trimmed, error) && trimmed.chunks.size() == 4 && trimmed.chunks[0].file == live_files[1],
          "the index is rewritten once without them");
    SessionChunk next;
    next.file = bx::session_media_dir(live.id) + "\\live-6.mp4";
    write_bytes(next.file, 512);
    next.start_ms = 6000;
    next.end_ms = 7000;
    next.wall_start_ms = now;
    next.wall_end_ms = now + minute;
    check(bx::session_store_append_chunk(live, next, error), "the writer appends the next chunk");
    SessionEntry appended;
    check(bx::session_store_load_one(live.id, appended, error) && appended.chunks.size() == 5,
          "an append after the trim does not bring the dropped chunks back: N - (K - 1) + 1 entries");
    std::string compact;
    if (FILE* index_file = std::fopen((root + "\\" + live.id + "\\index.json").c_str(), "rb")) {
      char buffer[4096];
      size_t got;
      while ((got = std::fread(buffer, 1, sizeof(buffer), index_file)) > 0) compact.append(buffer, got);
      std::fclose(index_file);
    }
    check(!compact.empty() && compact.find("\n  ") == std::string::npos, "the index is written compact, not pretty-printed");

    // The deletion is the worker's: a file Windows still holds is handed back to be retried, never lost.
    HANDLE held_open = CreateFileA((live_files[2] + ".expired").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::vector<bx::DroppedChunk> still_held;
    bx::session_store_delete_chunk_files(dropped, still_held);
    check(GetFileAttributesA(live_files[0].c_str()) == INVALID_FILE_ATTRIBUTES, "a dropped chunk's file is deleted");
    check(still_held.size() == 1 && still_held[0].chunk.file == live_files[2] && GetFileAttributesA((live_files[2] + ".expired").c_str()) != INVALID_FILE_ATTRIBUTES,
          "a file that is still open is kept and returned for the next sweep");
    check(GetFileAttributesA(live_files[1].c_str()) != INVALID_FILE_ATTRIBUTES, "the pinned chunk's file stays");
    if (held_open != INVALID_HANDLE_VALUE) CloseHandle(held_open);
    std::vector<bx::DroppedChunk> retried;
    bx::session_store_delete_chunk_files(still_held, retried);
    check(retried.empty() && GetFileAttributesA((live_files[2] + ".expired").c_str()) == INVALID_FILE_ATTRIBUTES, "and is deleted once nothing holds it");
    // A file Windows holds when the tick wants to drop it cannot be renamed: its chunk stays in the list for the next sweep.
    SessionChunk busy;
    busy.file = bx::session_media_dir(live.id) + "\\live-busy.mp4";
    write_bytes(busy.file, 128);
    busy.start_ms = 9000;
    busy.end_ms = 10000;
    busy.wall_start_ms = now - 100 * minute;
    busy.wall_end_ms = now - 99 * minute;
    check(bx::session_store_append_chunk(live, busy, error), "a chunk that will be busy is indexed");
    HANDLE busy_handle = CreateFileA(busy.file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::vector<bx::DroppedChunk> none;
    const size_t before_busy = live.chunks.size();
    check(bx::session_store_drop_chunks(live, std::vector<size_t>{live.chunks.size() - 1}, none, error) && none.empty() && live.chunks.size() == before_busy,
          "a file that is open cannot be dropped: the chunk stays listed");
    if (busy_handle != INVALID_HANDLE_VALUE) CloseHandle(busy_handle);
  }

  // A file Windows still holds comes back in `locked`; the erase is not claimed.
  SessionEntry open;
  check(bx::session_store_begin("timeline", open, error), "a timeline session begins for the open file");
  const std::string open_file = bx::session_media_dir(open.id) + "\\chunk-0.mp4";
  write_bytes(open_file, 4096);
  SessionChunk open_chunk;
  open_chunk.file = open_file;
  open_chunk.start_ms = 1000;
  open_chunk.end_ms = 2000;
  check(bx::session_store_append_chunk(open, open_chunk, error), "its chunk is indexed");
  HANDLE writer = CreateFileA(open_file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(writer != INVALID_HANDLE_VALUE, "the test holds the chunk the way a writer does");
  check(!bx::session_store_erase_timeline(open.id, locked, error) && locked.size() == 1 &&
            locked[0] == open_file && !error.empty(),
        "an open file is reported in locked and the erase does not claim success");
  check(GetFileAttributesA(open_file.c_str()) != INVALID_FILE_ATTRIBUTES, "the held file is still on disk");
  CloseHandle(writer);
  locked.clear();
  check(bx::session_store_erase_timeline(open.id, locked, error) && locked.empty(),
        "the erase succeeds once the writer has let go");
  check(GetFileAttributesA(bx::session_media_dir(open.id).c_str()) == INVALID_FILE_ATTRIBUTES,
        "and the directory is gone");

  // The recordings budget counts and prunes real sessions only, so Timeline footage can never be taken
  // by it: a timeline session and two recordings, each 1 KB, against a budget that fits one.
  const std::string budget_root = temp_directory() + "\\timeline-prune-exclusion";
  bx::session_store_init(budget_root);
  const auto budget_session = [&error](const char* kind, int64_t started_at) {
    SessionEntry entry;
    bx::session_store_begin(kind, entry, error);
    entry.started_at = started_at;
    const std::string file = bx::session_media_dir(entry.id) + "\\chunk.mkv";
    write_bytes(file, 1024);
    index_test_chunk(entry, file, 0, 1000, error);
    bx::session_store_close(entry, 2000, error);
    return entry;
  };
  const SessionEntry timeline_old = budget_session("timeline", 100);
  const SessionEntry recording_old = budget_session("recording", 200);
  const SessionEntry recording_new = budget_session("recording", 300);
  std::vector<std::string> removed;
  int64_t freed = 0;
  int64_t total = 0;
  check(bx::session_prune(1, "", removed, freed, total, error), "pruning to a tiny budget succeeds");
  check(removed.size() == 1 && removed[0] == recording_old.id, "only the older real recording is pruned");
  check(total == 1024, "the budget counts real recordings only");
  SessionEntry still_there;
  check(bx::session_store_load_one(timeline_old.id, still_there, error), "the timeline session is left alone");
  check(bx::session_store_load_one(recording_new.id, still_there, error), "the newest recording stays");

  // The recordings list is the library's, not the Timeline's.
  const std::string listing_root = temp_directory() + "\\timeline-listing";
  CreateDirectoryA(listing_root.c_str(), nullptr);
  bx::library_init({});
  bx::session_store_init(listing_root + "\\sessions");
  bx::LibraryPaths paths;
  paths.clips_dir = listing_root;
  paths.screenshots_dir = listing_root + "\\Screenshots";
  paths.thumbnails_dir = listing_root + "\\thumbnails";
  bx::library_init(paths);
  const auto listing_session = [&error](const char* kind) {
    SessionEntry entry;
    bx::session_store_begin(kind, entry, error);
    const std::string file = bx::session_media_dir(entry.id) + "\\chunk.mp4";
    write_bytes(file, 1024);
    index_test_chunk(entry, file, 0, 1000, error);
    return file;
  };
  const std::string timeline_file = listing_session("timeline");
  const std::string recording_listed = listing_session("recording");
  bool timeline_listed = false;
  bool recording_was_listed = false;
  for (const bx::MediaItem& item : bx::list_recordings()) {
    if (item.file == timeline_file) timeline_listed = true;
    if (item.file == recording_listed) recording_was_listed = true;
  }
  check(!timeline_listed, "a timeline chunk is not in the recordings list");
  check(recording_was_listed, "a normal session chunk still is");
  bx::library_init({});

  set_test_leases({});
  bx::session_store_init("");
}

// The bug the gate exists for: a sweep was queued while no lease covered a chunk, a save then pinned that footage, and
// the sweep - carrying the leases it saw when it was queued - deleted it anyway. A sweep now decides and reserves under
// one gate (timeline_retention.h) and does its file work with the gate free: a lease covering a reserved chunk is
// refused at once, one that read the index before the removal is refused after it through the session's removal
// number, and a lease a chunk already had keeps it out of the reservation.
void test_timeline_retention_gate() {
  using bx::SessionChunk;
  using bx::SessionEntry;
  using bx::TimelinePin;
  std::printf("timeline retention gate\n");
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const int64_t now = 1000000000000LL;
  const int64_t minute = 60000;
  std::string error;
  set_test_leases({});

  std::vector<SessionChunk> chunks(3);
  for (int index = 0; index < 3; ++index) {
    chunks[index].file = "chunk-" + std::to_string(index);
    chunks[index].wall_start_ms = now - (90 - index * 10) * minute;
    chunks[index].wall_end_ms = chunks[index].wall_start_ms + minute;
    chunks[index].published = true;
  }
  TimelinePin save;
  save.id = "save-late";
  save.owner = "timeline-save 1";
  save.session_id = "session-a";
  save.start_ms = chunks[1].wall_start_ms;
  save.end_ms = chunks[1].wall_end_ms;
  save.expires_ms = now + minute;

  // The lease a save already holds keeps its chunk out of the reservation; the rest are reserved in one short step.
  set_test_leases({save});
  const uint64_t read_before = bx::timeline_removal_seen("session-a");
  bx::TimelineRetentionTicket reserved = bx::TimelineRetentionTicket::reserve_chunks(
      chunks, "session-a", 30, now, [](const SessionChunk&) { return true; });
  check(reserved.active() && reserved.reserved().size() == 2 && reserved.reserved()[0] == 0 &&
            reserved.reserved()[1] == 2,
        "a sweep reserves what no lease covers and leaves the leased chunk alone");
  check(bx::timeline_reservation_count() == 2, "the removals in flight are named under the gate");

  // A lease asked for reserved footage is refused, and answered at once: the gate is free even while the disk is not.
  TimelinePin wanted = save;
  wanted.id = "lease-reserved";
  wanted.owner = "preview";
  wanted.start_ms = chunks[0].wall_start_ms;
  wanted.end_ms = chunks[0].wall_end_ms;
  const auto asked_at = std::chrono::steady_clock::now();
  const std::string refused = bx::timeline_lease_take(wanted, now, bx::timeline_removal_seen("session-a"));
  const auto answered_in = std::chrono::steady_clock::now() - asked_at;
  check(refused.empty(), "a lease over footage a sweep has reserved is refused");
  check(answered_in < std::chrono::milliseconds(250), "and answered at once, not when the disk answers");
  // The footage its own lease covers is still granted, and another session is untouched by this removal.
  check(!bx::timeline_lease_take(save, now, bx::timeline_removal_seen("session-a")).empty(),
        "a lease over footage nobody reserved is still granted");
  TimelinePin elsewhere = save;
  elsewhere.id = "save-elsewhere";
  elsewhere.owner = "timeline-save 2";
  elsewhere.session_id = "session-b";
  check(!bx::timeline_lease_take(elsewhere, now, bx::timeline_removal_seen("session-b")).empty(),
        "another session's lease is not blocked by a removal here");

  reserved.release();
  check(bx::timeline_reservation_count() == 0, "releasing the ticket leaves no removal in flight");
  check(bx::timeline_removal_seen("session-a") != read_before,
        "the removal moves the number a late lease is checked against");
  check(bx::timeline_lease_take(save, now, read_before).empty(),
        "a lease that read the index before the removal is refused after it");
  TimelinePin fresh = save;
  fresh.id = "save-fresh";
  fresh.owner = "timeline-save 3";
  fresh.start_ms = chunks[0].wall_start_ms;
  fresh.end_ms = chunks[0].wall_end_ms;
  const std::string granted = bx::timeline_lease_take(fresh, now, bx::timeline_removal_seen("session-a"));
  check(!granted.empty(), "a lease asked for after the removal is granted on what the index still names");
  check(bx::timeline_lease_revoke(granted, now), "and released by id");

  // The same on disk: a lease the sweep honours keeps its chunk and its index entry, and the rest go.
  const std::string root = temp_directory() + "\\timeline-retention-gate";
  remove_tree(root);
  bx::session_store_init(root);
  SessionEntry session;
  check(bx::session_store_begin("timeline", session, error), "a timeline session begins for the gate check");
  std::vector<std::string> files;
  for (int index = 0; index < 3; ++index) {
    const std::string file = bx::session_media_dir(session.id) + "\\gate-" + std::to_string(index) + ".mp4";
    write_bytes(file, 512);
    SessionChunk chunk;
    chunk.file = file;
    chunk.start_ms = static_cast<uint32_t>(index) * 1000;
    chunk.end_ms = static_cast<uint32_t>(index + 1) * 1000;
    chunk.wall_start_ms = chunks[index].wall_start_ms;
    chunk.wall_end_ms = chunks[index].wall_end_ms;
    check(bx::session_store_append_chunk(session, chunk, error), "a gate chunk is indexed");
    files.push_back(file);
  }
  check(bx::session_store_close(session, 2000, error), "the gate session closes");
  TimelinePin late = save;
  late.session_id = session.id;
  late.start_ms = session.chunks[1].wall_start_ms;
  late.end_ms = session.chunks[1].wall_end_ms;
  set_test_leases({late});
  check(bx::session_store_prune_timeline(30, now, error) && error.empty(), "the gated sweep succeeds");
  check(GetFileAttributesA(files[0].c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(files[2].c_str()) == INVALID_FILE_ATTRIBUTES,
        "footage no lease covers is still pruned");
  check(GetFileAttributesA(files[1].c_str()) != INVALID_FILE_ATTRIBUTES,
        "the chunk a lease was granted over survives the sweep that was queued before the grant");
  SessionEntry kept;
  check(bx::session_store_load_one(session.id, kept, error) && kept.chunks.size() == 1 &&
            kept.chunks[0].file == files[1],
        "and its index entry is kept with it");
  check(bx::timeline_reservation_count() == 0, "the sweep leaves no removal behind");

  // The open session's trim (the tick's own path) uses the same reservation: the leased chunk stays, the rest drop.
  SessionEntry live;
  check(bx::session_store_begin("timeline", live, error), "an open session begins for the trim check");
  std::vector<std::string> live_files;
  for (int index = 0; index < 2; ++index) {
    const std::string file = bx::session_media_dir(live.id) + "\\trim-" + std::to_string(index) + ".mp4";
    write_bytes(file, 512);
    SessionChunk chunk;
    chunk.file = file;
    chunk.start_ms = static_cast<uint32_t>(index) * 1000;
    chunk.end_ms = static_cast<uint32_t>(index + 1) * 1000;
    chunk.wall_start_ms = now - (90 - index) * minute;
    chunk.wall_end_ms = chunk.wall_start_ms + minute;
    check(bx::session_store_append_chunk(live, chunk, error), "an open chunk is indexed for the trim");
    live_files.push_back(file);
  }
  TimelinePin keep_open;
  keep_open.id = "open-lease";
  keep_open.owner = "preview";
  keep_open.session_id = live.id;
  keep_open.start_ms = live.chunks[0].wall_start_ms;
  keep_open.end_ms = live.chunks[0].wall_end_ms;
  keep_open.expires_ms = now + minute;
  set_test_leases({keep_open});
  std::vector<bx::DroppedChunk> dropped;
  check(bx::session_store_drop_expired(live, 30, now, dropped, error) && dropped.size() == 1 &&
            live.chunks.size() == 1 && live.chunks[0].file == live_files[0],
        "the trim drops what no lease covers and keeps the leased chunk");
  check(GetFileAttributesA((live_files[1] + ".expired").c_str()) != INVALID_FILE_ATTRIBUTES,
        "and renames the dropped footage before the worker deletes it");
  check(bx::timeline_reservation_count() == 0, "the trim leaves no removal behind");

  set_test_leases({});
  bx::session_store_init("");
  remove_tree(root);
}

// A delete that stalls on the file system must not stall a lease: a save takes one under the engine lock, and so does the
// ring from the tick, so a gate held across a delete stopped the tick, the panel and every hotkey for as long as the disk
// took. The held-open delete is the test hook in timeline_retention.h.
void test_timeline_retention_no_stall() {
  using bx::SessionChunk;
  using bx::SessionEntry;
  using bx::TimelinePin;
  std::printf("timeline retention holds no lock across the disk\n");
  const std::string root = temp_directory() + "\\timeline-no-stall";
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  remove_tree(root);
  bx::session_store_init(root);
  const int64_t now = 1000000000000LL;
  const int64_t minute = 60000;
  std::string error;
  set_test_leases({});

  SessionEntry session;
  check(bx::session_store_begin("timeline", session, error), "a session begins for the blocked delete");
  std::vector<std::string> files;
  for (int index = 0; index < 2; ++index) {
    const std::string file = bx::session_media_dir(session.id) + "\\stall-" + std::to_string(index) + ".mp4";
    write_bytes(file, 512);
    SessionChunk chunk;
    chunk.file = file;
    chunk.start_ms = static_cast<uint32_t>(index) * 1000;
    chunk.end_ms = static_cast<uint32_t>(index + 1) * 1000;
    chunk.wall_start_ms = now - (90 - index) * minute;
    chunk.wall_end_ms = chunk.wall_start_ms + minute;
    check(bx::session_store_append_chunk(session, chunk, error), "a chunk is indexed for the blocked delete");
    files.push_back(file);
  }
  check(bx::session_store_close(session, 2000, error), "the session closes");

  std::promise<void> entered;
  std::future<void> entered_future = entered.get_future();
  std::promise<void> release;
  std::shared_future<void> release_future = release.get_future().share();
  std::atomic<bool> announced{false};
  bx::timeline_store_pause_hook() = [&](const char* stage, const std::string&) {
    if (std::strcmp(stage, "delete") != 0) return;
    if (!announced.exchange(true)) entered.set_value();
    release_future.wait();
  };
  std::string sweep_error;
  std::thread sweeper([&] { bx::session_store_prune_timeline(30, now, sweep_error, ""); });
  check(entered_future.wait_for(std::chrono::milliseconds(5000)) == std::future_status::ready,
        "the sweep reaches the blocked delete");

  // Exactly what a save and the ring's dedupe ask for, and what the panel reads.
  TimelinePin pin;
  pin.id = "lease-stall";
  pin.owner = "preview";
  pin.session_id = session.id;
  pin.start_ms = session.chunks[0].wall_start_ms;
  pin.end_ms = session.chunks[0].wall_end_ms;
  pin.expires_ms = now + 10 * minute;
  const auto asked_at = std::chrono::steady_clock::now();
  const std::string lease = bx::timeline_lease_take(pin, now, bx::timeline_removal_seen(session.id));
  const auto answered_in = std::chrono::steady_clock::now() - asked_at;
  check(lease.empty(), "a lease over footage the stalled sweep has reserved is refused");
  check(answered_in < std::chrono::milliseconds(250), "and the engine-side lease is answered while the disk is blocked");
  const auto read_at = std::chrono::steady_clock::now();
  const std::vector<bx::TimelinePin> held = bx::timeline_leases_snapshot();
  check(held.empty() && std::chrono::steady_clock::now() - read_at < std::chrono::milliseconds(250),
        "the leases stay readable while the disk is blocked");
  const auto seen_at = std::chrono::steady_clock::now();
  bx::timeline_removal_seen(session.id);
  check(std::chrono::steady_clock::now() - seen_at < std::chrono::milliseconds(250),
        "and so does the number a lease is checked against");

  release.set_value();
  sweeper.join();
  bx::timeline_store_pause_hook() = nullptr;
  check(sweep_error.empty(), "the sweep finishes once the disk answers");
  check(GetFileAttributesA(files[0].c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(files[1].c_str()) == INVALID_FILE_ATTRIBUTES,
        "the held delete finishes");
  check(bx::timeline_reservation_count() == 0, "no removal is left in flight");
  SessionEntry pruned;
  check(!bx::session_store_load_one(session.id, pruned, error),
        "and the emptied session is gone from the index");

  set_test_leases({});
  bx::session_store_init("");
  remove_tree(root);
}

// The ring's dedupe renews its lease from the tick while the sweep runs on its worker. Both go through the one
// gate-protected path (the race this replaces mutated the list from the tick with no gate at all), and footage a live
// lease covers is never deleted.
void test_timeline_dedupe_sweep_concurrent() {
  using bx::SessionChunk;
  using bx::SessionEntry;
  using bx::TimelinePin;
  std::printf("dedupe and the sweep hold the same leases\n");
  const std::string root = temp_directory() + "\\timeline-dedupe-sweep";
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  remove_tree(root);
  bx::session_store_init(root);
  const int64_t now = 1000000000000LL;
  const int64_t minute = 60000;
  std::string error;
  set_test_leases({});

  SessionEntry session;
  check(bx::session_store_begin("timeline", session, error), "a session begins for the concurrent check");
  std::vector<std::string> files;
  for (int index = 0; index < 3; ++index) {
    const std::string file = bx::session_media_dir(session.id) + "\\ring-" + std::to_string(index) + ".mp4";
    write_bytes(file, 512);
    SessionChunk chunk;
    chunk.file = file;
    chunk.start_ms = static_cast<uint32_t>(index) * 1000;
    chunk.end_ms = static_cast<uint32_t>(index + 1) * 1000;
    chunk.wall_start_ms = now - (90 - index * 10) * minute;
    chunk.wall_end_ms = chunk.wall_start_ms + minute;
    check(bx::session_store_append_chunk(session, chunk, error), "a chunk is indexed for the concurrent check");
    files.push_back(file);
  }
  check(bx::session_store_close(session, 2000, error), "the session closes");

  TimelinePin ring;
  ring.id = "replay-ring";
  ring.owner = "replay-ring";
  ring.session_id = session.id;
  ring.start_ms = session.chunks[0].wall_start_ms;
  ring.end_ms = session.chunks[1].wall_end_ms + 1;
  ring.expires_ms = now + 2 * minute;
  const std::string first = bx::timeline_lease_take(ring, now, bx::timeline_removal_seen(session.id));
  check(first == std::string("replay-ring"), "the ring's lease is taken through the gate");

  // The tick's renewal, as dedupe_tick makes it, while the sweep deletes the footage nobody pinned.
  std::atomic<bool> stop{false};
  std::atomic<int> renewals{0};
  std::thread renter([&] {
    while (!stop.load()) {
      const uint64_t seen = bx::timeline_removal_seen(session.id);
      if (!bx::timeline_lease_take(ring, now, seen).empty()) renewals.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  for (int wait = 0; wait < 500 && renewals.load() == 0; ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  std::string sweep_error;
  const bool swept = bx::session_store_prune_timeline(30, now, sweep_error, "");
  stop.store(true);
  renter.join();
  check(swept && sweep_error.empty(), "the sweep runs beside the renewals");
  check(renewals.load() > 0, "the ring renewed its lease while the sweep ran");
  check(GetFileAttributesA(files[0].c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(files[1].c_str()) != INVALID_FILE_ATTRIBUTES,
        "footage the ring's lease covers is never deleted");
  check(GetFileAttributesA(files[2].c_str()) == INVALID_FILE_ATTRIBUTES, "and the rest is pruned");
  SessionEntry kept;
  check(bx::session_store_load_one(session.id, kept, error) && kept.chunks.size() == 2,
        "the index keeps exactly the footage the lease covered");
  check(bx::timeline_reservation_count() == 0, "no removal is left in flight");

  // The ring lets go: the same footage is now the sweep's.
  check(bx::timeline_lease_revoke_owner("replay-ring", std::string(), now) == 1, "the ring releases its lease");
  check(bx::session_store_prune_timeline(30, now, sweep_error, "") && sweep_error.empty(),
        "the next sweep runs on the released footage");
  check(GetFileAttributesA(files[0].c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(files[1].c_str()) == INVALID_FILE_ATTRIBUTES,
        "the footage goes once nothing holds it");
  check(!bx::session_store_load_one(session.id, kept, error), "and the emptied session is gone");

  set_test_leases({});
  bx::session_store_init("");
  remove_tree(root);
}

// An explicit erase and a lease on the same session are decided under one gate: a save, a preview or an editor lease
// granted before the erase keeps its footage (the erase is refused), and a lease asked for while the erase runs is
// refused at once instead of being granted over files that are going. The ring's own dedupe lease renews every second,
// so it must never keep the person's erase out.
void test_timeline_erase_lease_claim() {
  using bx::SessionChunk;
  using bx::SessionEntry;
  using bx::TimelinePin;
  std::printf("timeline erase claims against live leases\n");
  const std::string root = temp_directory() + "\\timeline-erase-claim";
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  remove_tree(root);
  bx::session_store_init(root);
  set_test_leases({});
  std::string error;
  // The erase's claim is checked against the real wall clock (a lease's expiry is absolute), so the leases here are
  // made on the same clock, not on the sweep tests' made-up one.
  const int64_t real_now = static_cast<int64_t>(std::time(nullptr)) * 1000;

  const auto append = [&](SessionEntry& entry, const std::string& name) {
    const std::string file = bx::session_media_dir(entry.id) + "\\" + name;
    write_bytes(file, 512);
    SessionChunk chunk;
    chunk.file = file;
    chunk.start_ms = static_cast<uint32_t>(entry.chunks.size()) * 1000;
    chunk.end_ms = chunk.start_ms + 1000;
    chunk.wall_start_ms = real_now - 3600000;
    chunk.wall_end_ms = chunk.wall_start_ms + 1000;
    check(bx::session_store_append_chunk(entry, chunk, error), "a chunk is indexed for the erase claim");
    return file;
  };

  // A save holds its lease when the person asks for the erase: the erase is refused and deletes nothing.
  SessionEntry leased;
  check(bx::session_store_begin("timeline", leased, error), "a timeline session begins for the erase claim");
  const std::string leased_file = append(leased, "leased.mp4");
  check(bx::session_store_close(leased, 2000, error), "the leased session closes");
  TimelinePin save;
  save.id = "claim-save";
  save.owner = "timeline-save 1";
  save.session_id = leased.id;
  save.start_ms = real_now - 3600000;
  save.end_ms = real_now;
  save.expires_ms = real_now + 10 * 60000;
  check(!bx::timeline_lease_take(save, real_now, bx::timeline_removal_seen(leased.id)).empty(), "a save lease is granted");
  std::vector<std::string> locked;
  check(!bx::session_store_erase_timeline(leased.id, locked, error) && !error.empty(),
        "an erase asked for while a save reads the footage is refused");
  check(GetFileAttributesA(leased_file.c_str()) != INVALID_FILE_ATTRIBUTES && locked.empty(),
        "and it deletes nothing");
  SessionEntry still_there;
  check(bx::session_store_load_one(leased.id, still_there, error) && still_there.chunks.size() == 1,
        "the refused erase leaves the index and its chunk");
  check(bx::timeline_lease_revoke(save.id, real_now), "the save lets go");

  // The ring's dedupe lease is renewed every second while the buffer runs; an explicit erase ignores it.
  TimelinePin ring;
  ring.id = "replay-ring";
  ring.owner = bx::kTimelineRingLeaseOwner;
  ring.session_id = leased.id;
  ring.start_ms = save.start_ms;
  ring.end_ms = save.end_ms;
  ring.expires_ms = real_now + 2 * 60000;
  check(!bx::timeline_lease_take(ring, real_now, bx::timeline_removal_seen(leased.id)).empty(),
        "the ring's lease is granted");
  locked.clear();
  check(bx::session_store_erase_timeline(leased.id, locked, error) && locked.empty() &&
            GetFileAttributesA(leased_file.c_str()) == INVALID_FILE_ATTRIBUTES,
        "the ring's renewed lease never blocks the person's erase");

  // A lease racing the erase: the erase has claimed the session and is waiting on the disk. The lease is answered at
  // once (the gate is free) and refused - the two can never both be granted.
  SessionEntry racing;
  check(bx::session_store_begin("timeline", racing, error), "a timeline session begins for the erase race");
  const std::string racing_file = append(racing, "racing.mp4");
  check(bx::session_store_close(racing, 3000, error), "the racing session closes");
  std::promise<void> entered;
  std::future<void> entered_future = entered.get_future();
  std::promise<void> release;
  std::shared_future<void> release_future = release.get_future().share();
  std::atomic<bool> announced{false};
  bx::timeline_store_pause_hook() = [&](const char* stage, const std::string&) {
    if (std::strcmp(stage, "session") != 0) return;
    if (!announced.exchange(true)) entered.set_value();
    release_future.wait();
  };
  std::string erase_error;
  std::atomic<bool> erased{false};
  std::thread eraser([&] { erased.store(bx::session_store_erase_timeline(racing.id, locked, erase_error)); });
  check(entered_future.wait_for(std::chrono::milliseconds(5000)) == std::future_status::ready,
        "the erase claims the session and waits on the disk");
  TimelinePin preview;
  preview.id = "claim-preview";
  preview.owner = "preview";
  preview.session_id = racing.id;
  preview.start_ms = racing.chunks[0].wall_start_ms;
  preview.end_ms = racing.chunks[0].wall_end_ms;
  preview.expires_ms = real_now + 10 * 60000;
  const auto asked_at = std::chrono::steady_clock::now();
  const std::string race_lease = bx::timeline_lease_take(preview, real_now, bx::timeline_removal_seen(racing.id));
  const auto answered_in = std::chrono::steady_clock::now() - asked_at;
  check(race_lease.empty(), "a lease asked for while the erase runs is refused");
  check(answered_in < std::chrono::milliseconds(250), "and answered at once, not when the disk answers");
  release.set_value();
  eraser.join();
  bx::timeline_store_pause_hook() = nullptr;
  check(erased.load() && erase_error.empty(), "the erase finishes once the disk answers");
  check(GetFileAttributesA(racing_file.c_str()) == INVALID_FILE_ATTRIBUTES, "and its footage is gone");
  check(bx::timeline_reservation_count() == 0, "no removal is left in flight");

  set_test_leases({});
  bx::session_store_init("");
  remove_tree(root);
}

// A replay save reads Timeline chunk files while a sweep or the person's erase may want them. The read guard
// (timeline_retention.h) is what holds them: taken under the one gate before the first file is opened, it refuses a
// claim on its session and keeps the sweep off it, and it has no clock - a join slower than the ring's own 120 s dedupe
// lease stays protected. It is released the moment the read ends, on every way out, so an erase is delayed, never
// blocked for good.
void test_timeline_read_guard() {
  using bx::SessionChunk;
  using bx::TimelineReadGuard;
  using bx::TimelineRetentionTicket;
  std::printf("timeline read guards\n");
  set_test_leases({});
  const int64_t now = static_cast<int64_t>(std::time(nullptr)) * 1000;
  const int64_t long_after = now + 10 * 3600000;

  std::vector<SessionChunk> chunks(2);
  for (int index = 0; index < 2; ++index) {
    chunks[index].file = "guard-chunk-" + std::to_string(index);
    chunks[index].wall_start_ms = now - 3600000 - index * 60000;
    chunks[index].wall_end_ms = chunks[index].wall_start_ms + 60000;
    chunks[index].published = true;
  }
  const auto inside = [](const SessionChunk&) { return true; };

  // The save wins: its read is in force before the erase asks, so the erase is refused and names the read.
  TimelineReadGuard read = TimelineReadGuard::take("session-a");
  check(read.active() && bx::timeline_read_guard_count() == 1, "a replay save guards the session it reads");
  std::string leased_by;
  TimelineRetentionTicket erase = TimelineRetentionTicket::claim_session("session-a", now, bx::kTimelineRingLeaseOwner, leased_by);
  check(!erase.active() && leased_by == bx::kTimelineReplaySaveOwner,
        "an erase asked for while a replay save reads the footage is refused, and names the save");
  leased_by.clear();
  TimelineRetentionTicket elsewhere = TimelineRetentionTicket::claim_session("session-b", now, bx::kTimelineRingLeaseOwner, leased_by);
  check(elsewhere.active() && leased_by.empty(), "a read on one session never holds another session's erase");
  elsewhere.release();

  // Retention waits for the read even with the ring's own lease long expired and every ordinary TTL passed: the guard
  // has no clock, so a join slower than any lease is still protected.
  bx::TimelinePin expired;
  expired.id = "replay-ring";
  expired.owner = bx::kTimelineRingLeaseOwner;
  expired.session_id = "session-a";
  expired.start_ms = chunks[0].wall_start_ms;
  expired.end_ms = chunks[1].wall_end_ms;
  expired.expires_ms = now;
  set_test_leases({expired});
  TimelineRetentionTicket reserved = TimelineRetentionTicket::reserve_chunks(chunks, "session-a", 30, long_after, inside);
  check(!reserved.active() && bx::timeline_reservation_count() == 0,
        "a sweep past every lease's expiry leaves a session a replay save is reading alone");
  TimelineRetentionTicket whole = TimelineRetentionTicket::reserve_session("session-a", long_after);
  check(!whole.active() && bx::timeline_reservation_count() == 0, "nor does a whole-session sweep take it");
  // Another session's expired footage is still swept while this read stands.
  set_test_leases({});
  reserved = TimelineRetentionTicket::reserve_chunks(chunks, "session-c", 30, long_after, inside);
  check(reserved.active() && reserved.reserved().size() == 2, "an unrelated session is swept as usual");
  reserved.release();
  check(bx::timeline_reservation_count() == 0, "no removal is left in flight");

  // The erase wins: claimed before the save starts reading, so the save's guard cannot be taken and its chunks stay
  // out of its plan (ring_output.cpp) instead of being opened while the erase deletes them.
  read.release();
  check(bx::timeline_read_guard_count() == 0, "the read guard is released when the read finishes");
  leased_by.clear();
  erase = TimelineRetentionTicket::claim_session("session-a", now, bx::kTimelineRingLeaseOwner, leased_by);
  check(erase.active() && leased_by.empty(), "once the read ends, the erase claims the session");
  TimelineReadGuard late = TimelineReadGuard::take("session-a");
  check(!late.active() && bx::timeline_read_guard_count() == 0,
        "a replay save that starts after the erase claimed does not read that session's chunks");
  erase.release();
  TimelineReadGuard again = TimelineReadGuard::take("session-a");
  check(again.active(), "when the erase lets go, the next save's read guard is taken");

  // Two reads, one released: the other still holds its own session's erase, and the release is exactly one guard.
  TimelineReadGuard second = TimelineReadGuard::take("session-d");
  check(second.active() && bx::timeline_read_guard_count() == 2, "a save reading two sessions holds two guards");
  again.release();
  check(bx::timeline_read_guard_count() == 1, "releasing one read leaves the other in force");
  leased_by.clear();
  TimelineRetentionTicket erase_d = TimelineRetentionTicket::claim_session("session-d", now, bx::kTimelineRingLeaseOwner, leased_by);
  check(!erase_d.active() && leased_by == bx::kTimelineReplaySaveOwner, "and that read still keeps its session's erase out");
  erase_d.release();
  {
    // The destructor is the release on every way out of the save, including a failed one.
    TimelineReadGuard scoped = TimelineReadGuard::take("session-a");
    check(scoped.active() && bx::timeline_read_guard_count() == 2, "a save's guard is in force while it runs");
  }
  second.release();
  check(bx::timeline_read_guard_count() == 0, "every way out of a save releases its guard");
  check(!TimelineReadGuard::take("").active(), "there is no guard without a session");
  check(bx::timeline_reservation_count() == 0, "and no removal is left in flight");
  set_test_leases({});
}

// A Timeline save's copy outlives its lease. The lease (15 minutes) only proves the index the save planned from is
// current; the join it waits on may take 30 (media.h's deadline). The read guard commands.cpp takes beside the lease is
// what protects the sources for the copy's real lifetime - this shows the lease alone would let a sweep take them, and
// that the guard refuses the sweep, a whole-session sweep and the person's erase well past both clocks, then lets them
// go on release, on success and on failure alike.
void test_timeline_save_read_guard() {
  using bx::SessionChunk;
  using bx::TimelinePin;
  using bx::TimelineReadGuard;
  using bx::TimelineRetentionTicket;
  std::printf("timeline save holds its sources past the lease\n");
  set_test_leases({});
  const int64_t now = static_cast<int64_t>(std::time(nullptr)) * 1000;
  const int64_t minute = 60000;
  const int64_t after_join = now + 30 * minute;       // media_join_process' own deadline
  const int64_t long_after = now + 10 * 60 * minute;  // ten hours later: every clock has run out

  std::vector<SessionChunk> chunks(2);
  for (int index = 0; index < 2; ++index) {
    chunks[index].file = "save-chunk-" + std::to_string(index);
    chunks[index].wall_start_ms = now - (40 - index) * minute;
    chunks[index].wall_end_ms = chunks[index].wall_start_ms + 10 * minute;
    chunks[index].published = true;
  }
  const auto inside = [](const SessionChunk&) { return true; };

  // The save's lease, exactly as commands.cpp takes it: fifteen minutes over the chunks the plan copies.
  TimelinePin save;
  save.id = "save session-a";
  save.owner = std::string(bx::kTimelineSaveOwner) + " 1";
  save.session_id = "session-a";
  save.start_ms = chunks[0].wall_start_ms;
  save.end_ms = chunks[1].wall_end_ms;
  save.expires_ms = now + 15 * minute;

  // The lease alone, with a join that returned after its 30 minutes: both chunks are sweepable while the save still
  // reads them. This is the loss the read guard exists to stop, and it is why one is not enough.
  check(bx::timeline_lease_take(save, now, bx::timeline_removal_seen("session-a")) == save.id,
        "a timeline save's lease is granted when the copy starts");
  TimelineRetentionTicket expired_lease = TimelineRetentionTicket::reserve_chunks(chunks, "session-a", 30, after_join, inside);
  check(expired_lease.active() && expired_lease.reserved().size() == 2,
        "a lease alone leaves the sources sweepable once its fifteen minutes run out during a longer join");
  expired_lease.release();
  set_test_leases({});
  check(bx::timeline_reservation_count() == 0, "no removal is left in flight");

  // With the guard commands.cpp holds for the whole copy, the same sweep - and an erase - are refused at the join's
  // deadline and ten hours later, with the lease long expired.
  TimelineReadGuard read = TimelineReadGuard::take("session-a");
  check(read.active() && bx::timeline_read_guard_count() == 1, "the save guards the session its copy reads");
  set_test_leases({save});
  for (const int64_t at : {after_join, long_after}) {
    TimelineRetentionTicket sweep = TimelineRetentionTicket::reserve_chunks(chunks, "session-a", 30, at, inside);
    check(!sweep.active() && bx::timeline_reservation_count() == 0,
          "a sweep is refused for as long as the copy reads, expired lease or not");
    TimelineRetentionTicket whole = TimelineRetentionTicket::reserve_session("session-a", at);
    check(!whole.active() && bx::timeline_reservation_count() == 0,
          "nor does a whole-session sweep take it while the copy reads");
    std::string leased_by;
    TimelineRetentionTicket erase = TimelineRetentionTicket::claim_session("session-a", at, bx::kTimelineRingLeaseOwner, leased_by);
    check(!erase.active() && leased_by == bx::kTimelineReplaySaveOwner,
          "the person's erase is refused while the copy reads, and names the read");
  }
  // Another session's expired footage is still swept while this copy runs.
  TimelineRetentionTicket elsewhere = TimelineRetentionTicket::reserve_chunks(chunks, "session-c", 30, long_after, inside);
  check(elsewhere.active() && elsewhere.reserved().size() == 2, "an unrelated session is swept as usual");
  elsewhere.release();

  // Released on the way out: the sweep that was refused a moment ago now goes through, so an erase is delayed and
  // never kept out for good.
  read.release();
  check(bx::timeline_read_guard_count() == 0, "finishing the copy releases its guard");
  TimelineRetentionTicket after = TimelineRetentionTicket::reserve_chunks(chunks, "session-a", 30, after_join, inside);
  check(after.active() && after.reserved().size() == 2, "once the copy ends, the expired chunks are swept");
  after.release();
  check(bx::timeline_reservation_count() == 0, "no removal is left in flight");

  // Every way out of the function, including an early failure, drops the guard: a save that refused before copying
  // never keeps a session out of a sweep.
  const auto run_save = [](const char* session, bool succeed) {
    TimelineReadGuard scoped = TimelineReadGuard::take(session);
    if (!scoped.active()) return false;
    return succeed;  // returns through the guard's destructor on both answers
  };
  check(run_save("session-a", true) && bx::timeline_read_guard_count() == 0, "a successful save releases its guard");
  check(!run_save("session-a", false) && bx::timeline_read_guard_count() == 0, "a failed save releases its guard too");
  set_test_leases({});
}


// The sweep deletes files with the store free and writes the index afterwards. That write re-reads the index: a chunk
// published while the sweep was paused is kept, and a session another route removed is never written back (its old
// snapshot would recreate the index in a directory a failed delete left behind).
void test_timeline_prune_index_races() {
  using bx::SessionChunk;
  using bx::SessionEntry;
  std::printf("timeline prune keeps concurrent index changes\n");
  const std::string root = temp_directory() + "\\timeline-prune-index";
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  remove_tree(root);
  bx::session_store_init(root);
  set_test_leases({});
  std::string error;
  const int64_t now = 1000000000000LL;
  const int64_t minute = 60000;

  const auto append_expired = [&](SessionEntry& entry, const std::string& name, int index, std::vector<std::string>& files) {
    const std::string file = bx::session_media_dir(entry.id) + "\\" + name;
    write_bytes(file, 512);
    SessionChunk chunk;
    chunk.file = file;
    chunk.start_ms = static_cast<uint32_t>(index) * 1000;
    chunk.end_ms = chunk.start_ms + 1000;
    chunk.wall_start_ms = now - (90 - index * 10) * minute;
    chunk.wall_end_ms = chunk.wall_start_ms + minute;
    check(bx::session_store_append_chunk(entry, chunk, error), "an expired chunk is indexed for the index race");
    files.push_back(file);
  };

  // Two expired chunks; the sweep pauses just before it writes the index.
  SessionEntry swept;
  check(bx::session_store_begin("timeline", swept, error), "a session begins for the index race");
  std::vector<std::string> old_files;
  append_expired(swept, "race-0.mp4", 0, old_files);
  append_expired(swept, "race-1.mp4", 1, old_files);
  check(bx::session_store_close(swept, 2000, error), "the raced session closes");

  std::promise<void> entered;
  std::future<void> entered_future = entered.get_future();
  std::promise<void> release;
  std::shared_future<void> release_future = release.get_future().share();
  std::atomic<bool> announced{false};
  bx::timeline_store_pause_hook() = [&](const char* stage, const std::string&) {
    if (std::strcmp(stage, "index") != 0) return;
    if (!announced.exchange(true)) entered.set_value();
    release_future.wait();
  };
  std::string sweep_error;
  std::thread sweeper([&] { bx::session_store_prune_timeline(30, now, sweep_error, ""); });
  check(entered_future.wait_for(std::chrono::milliseconds(5000)) == std::future_status::ready,
        "the sweep reaches the index write with its files already deleted");
  // The writer publishes a chunk while the sweep waits: the same session's index changed under the sweep.
  SessionEntry during;
  check(bx::session_store_load_one(swept.id, during, error), "the index is read while the sweep is paused");
  const std::string fresh_file = bx::session_media_dir(swept.id) + "\\race-fresh.mp4";
  write_bytes(fresh_file, 512);
  SessionChunk fresh;
  fresh.file = fresh_file;
  fresh.start_ms = 5000;
  fresh.end_ms = 6000;
  fresh.wall_start_ms = now - 10 * minute;
  fresh.wall_end_ms = now - 9 * minute;
  check(bx::session_store_append_chunk(during, fresh, error), "a chunk is published while the sweep waits");
  release.set_value();
  sweeper.join();
  bx::timeline_store_pause_hook() = nullptr;
  check(sweep_error.empty(), "the paused sweep finishes");
  SessionEntry after;
  check(bx::session_store_load_one(swept.id, after, error) && after.chunks.size() == 1 &&
            after.chunks[0].file == fresh_file,
        "the index keeps the chunk published while the sweep waited, and drops only what it deleted");
  check(GetFileAttributesA(fresh_file.c_str()) != INVALID_FILE_ATTRIBUTES, "the published chunk's file was never deleted");
  check(GetFileAttributesA(old_files[0].c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesA(old_files[1].c_str()) == INVALID_FILE_ATTRIBUTES,
        "the expired chunks are still deleted");

  // A session another route removes while the sweep waits is not written back. The held file keeps its directory
  // alive, which is exactly the case where the stale write would recreate the index.
  SessionEntry removed;
  check(bx::session_store_begin("timeline", removed, error), "a session begins for the removal race");
  std::vector<std::string> gone_files;
  append_expired(removed, "gone-0.mp4", 0, gone_files);
  append_expired(removed, "gone-1.mp4", 1, gone_files);
  check(bx::session_store_close(removed, 2000, error), "the removed session closes");
  HANDLE held = CreateFileA(gone_files[1].c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(held != INVALID_HANDLE_VALUE, "the test holds a chunk the way a writer does");

  std::promise<void> entered_again;
  std::future<void> entered_again_future = entered_again.get_future();
  std::promise<void> release_again;
  std::shared_future<void> release_again_future = release_again.get_future().share();
  std::atomic<bool> announced_again{false};
  bx::timeline_store_pause_hook() = [&](const char* stage, const std::string&) {
    if (std::strcmp(stage, "index") != 0) return;
    if (!announced_again.exchange(true)) entered_again.set_value();
    release_again_future.wait();
  };
  std::string second_error;
  std::thread second([&] { bx::session_store_prune_timeline(30, now, second_error, ""); });
  check(entered_again_future.wait_for(std::chrono::milliseconds(5000)) == std::future_status::ready,
        "the second sweep reaches the index write of the session it will keep finding");
  const std::string removed_index = root + "\\" + removed.id + "\\index.json";
  std::string delete_error;
  check(bx::session_store_delete(removed, delete_error), "another route removes the session while the sweep waits");
  check(GetFileAttributesA(removed_index.c_str()) == INVALID_FILE_ATTRIBUTES, "the other route took the index");
  release_again.set_value();
  second.join();
  bx::timeline_store_pause_hook() = nullptr;
  check(GetFileAttributesA(removed_index.c_str()) == INVALID_FILE_ATTRIBUTES,
        "the sweep does not write the removed session's index back");
  SessionEntry resurrected;
  check(!bx::session_store_load_one(removed.id, resurrected, error), "and the session is not resurrected");
  if (held != INVALID_HANDLE_VALUE) CloseHandle(held);

  set_test_leases({});
  bx::session_store_init("");
  remove_tree(root);
}

// Task 9 checks: the numbers timeline.get reports and the owner-scoped leases behind timeline.pin.
void test_timeline_leases() {
  using bx::SessionChunk;
  using bx::SessionEntry;
  using bx::TimelinePin;
  std::printf("timeline leases and published span\n");
  const int64_t now = 1000000000000LL;
  const int64_t minute = 60000;

  // The index keeps absolute ticks; the protocol reports milliseconds since the session began, and the
  // tick clock's wrap must not turn a short session into a negative one.
  check(bx::timeline_session_ms(1005000, 1000000) == 5000, "a chunk's tick time becomes session milliseconds");
  check(bx::timeline_session_ms(500, 4294967290u) == 506, "the tick clock's wrap gives the interval that passed");

  // Only published chunks are footage: the newest one's writer still holds its file.
  SessionEntry entry;
  entry.started_ms = 1000000;
  entry.chunks.resize(2);
  entry.chunks[0].file = "published";
  entry.chunks[0].start_ms = 1000000;
  entry.chunks[0].end_ms = 1005000;
  entry.chunks[0].bytes = 100;
  entry.chunks[1].file = "writing";
  entry.chunks[1].start_ms = 1005000;
  entry.chunks[1].end_ms = 1009000;
  entry.chunks[1].bytes = 999;
  entry.chunks[1].published = false;
  int64_t span_start = -1, span_end = -1, span_bytes = -1;
  bx::timeline_published_span(entry, span_start, span_end, span_bytes);
  check(span_start == 0 && span_end == 5000, "the available span stops at the newest published chunk");
  check(span_bytes == 100, "and counts published bytes only");
  SessionEntry none_published;
  none_published.started_ms = 1000000;
  none_published.chunks.resize(1);
  none_published.chunks[0].start_ms = 1000000;
  none_published.chunks[0].end_ms = 1002000;
  none_published.chunks[0].published = false;
  bx::timeline_published_span(none_published, span_start, span_end, span_bytes);
  check(span_start == 0 && span_end == 0 && span_bytes == 0, "a session with no published chunk has no span");

  // An owner's lease renews rather than stacking.
  std::vector<TimelinePin> pins;
  TimelinePin preview;
  preview.id = "lease-1";
  preview.owner = "preview";
  preview.session_id = "session-a";
  preview.start_ms = now - 2 * minute;
  preview.end_ms = now - minute;
  preview.expires_ms = now + 10 * minute;
  check(bx::timeline_pin_upsert(pins, preview, now) == std::string("lease-1"), "a lease is taken");
  check(pins.size() == 1, "the lease list holds it once");
  TimelinePin renewed = preview;
  renewed.id = "lease-2";
  renewed.start_ms = now - 3 * minute;
  renewed.end_ms = now;
  renewed.expires_ms = now + 20 * minute;
  check(bx::timeline_pin_upsert(pins, renewed, now) == std::string("lease-1"),
        "the same owner on the same session renews the lease it holds");
  check(pins.size() == 1 && pins[0].start_ms == renewed.start_ms && pins[0].expires_ms == renewed.expires_ms,
        "the renewal takes the new span and expiry");
  TimelinePin save_pin = preview;
  save_pin.id = "lease-save";
  save_pin.owner = "timeline-save";
  check(bx::timeline_pin_upsert(pins, save_pin, now) == std::string("lease-save"), "another owner gets its own");
  check(pins.size() == 2, "owners do not share a lease");
  TimelinePin editor_pin = preview;
  editor_pin.id = "editor-lease";
  editor_pin.owner = "editor:session-a";
  pins.push_back(editor_pin);
  check(bx::timeline_editor_holds(pins, "session-a", now), "an editor lease prevents erasing its session");
  check(!bx::timeline_editor_holds(pins, "session-b", now), "an editor lease does not protect another session");
  check(!bx::timeline_editor_holds(pins, "session-a", editor_pin.expires_ms), "an expired editor lease does not prevent erasing");
  pins.pop_back();

  // An expired lease protects nothing and is not kept.
  TimelinePin stale = preview;
  stale.id = "lease-stale";
  stale.owner = "gone";
  stale.expires_ms = now - minute;
  pins.push_back(stale);
  pins = bx::timeline_pins_live(std::move(pins), now);
  check(pins.size() == 2 && bx::timeline_pin_owned(pins, "gone", "session-a") == pins.size(),
        "an expired lease is dropped");
  check(bx::timeline_pin_owned(pins, "", "session-a") == pins.size(), "an unnamed owner never matches a lease");

  // A lease that asks for nothing is refused, not silently dropped from the list.
  TimelinePin empty = preview;
  empty.id = "lease-empty";
  empty.owner = "preview";
  empty.end_ms = empty.start_ms;
  check(bx::timeline_pin_upsert(pins, empty, now).empty(), "an empty span is not a lease");
  check(pins.size() == 2, "and the owner keeps the lease it already had");

  check(!bx::timeline_pin_release(pins, "lease-unknown"), "releasing an unknown lease reports nothing went");
  check(!bx::timeline_pin_release(pins, ""), "an empty id releases nothing");
  check(bx::timeline_pin_release(pins, "lease-save") && pins.size() == 1, "a lease is released by id");
  check(bx::timeline_pin_release_owner(pins, "", "") == 0, "an unnamed owner releases nothing");
  check(bx::timeline_pin_release_owner(pins, "preview", "session-a") == 1 && pins.empty(),
        "a closing preview releases its own leases");
}

// Task 4 checks: recordings and sessions are visible to the interface.
void test_recordings_listing() {
  const std::string root = temp_directory() + "\\listing";
  CreateDirectoryA(root.c_str(), nullptr);
  bx::library_init({});
  // Point the store at a root that does not exist yet, so a leftover session directory on this
  // machine cannot make this check pass or fail for the wrong reason.
  bx::session_store_init(root + "\\sessions");
  check(bx::list_recordings().empty(), "an empty clips directory has no recordings");

  bx::LibraryPaths paths;
  paths.clips_dir = root;
  paths.screenshots_dir = root + "\\Screenshots";
  paths.thumbnails_dir = root + "\\thumbnails";
  bx::library_init(paths);

  // One single-file recording and one session chunk.
  write_bytes(root + "\\Recordings\\BuffrX recording.mkv", 2048);
  bx::SessionEntry session;
  std::string error;
  check(bx::session_store_begin("recording", session, error), "a session exists for the listing check");
  const std::string chunk = bx::session_media_dir(session.id) + "\\BuffrX recording.mkv";
  write_bytes(chunk, 4096);
  check(index_test_chunk(session, chunk, 0, 30000, error), "the chunk is indexed");

  const std::vector<bx::MediaItem> recordings = bx::list_recordings();
  check(recordings.size() == 2, "a single-file recording and a session chunk are both listed");
  bool saw_index = false;
  for (const bx::MediaItem& item : recordings) {
    if (item.file.find("index.json") != std::string::npos) saw_index = true;
  }
  check(!saw_index, "no session index is listed as a recording");

  int64_t bytes = 0;
  int count = 0;
  bx::library_recordings_usage(bytes, count);
  check(count == 2 && bytes == 6144, "recordings usage counts what it lists");
  bx::library_sessions_usage(bytes, count);
  check(count == 1 && bytes == 4096, "sessions usage reports the index total");

  // A chunk is not a file to delete on its own: the index would point at nothing.
  check(!bx::library_delete(chunk), "deleting one chunk of a session is refused");
  // Nor to rename on its own: the index keeps the old name, so the file it names is gone.
  std::string renamed;
  check(!bx::library_rename(chunk, "renamed chunk", renamed), "renaming one chunk of a session is refused");
  check(GetFileAttributesA(chunk.c_str()) != INVALID_FILE_ATTRIBUTES, "and the chunk keeps its name");
  // A new name that is only an extension leaves no base name: ".mkv" is not a clip name.
  check(!bx::library_rename(root + "\\Recordings\\BuffrX recording.mkv", ".mp4", renamed),
        "a new name that is only an extension is refused");
  check(bx::library_delete(root + "\\Recordings\\BuffrX recording.mkv"), "a single-file recording still deletes");

  // A NUL in the new name used to survive cleaning: MoveFileA stopped at it, so the file became
  // "gone" with no extension while the reply promised "gone\0.mp4" - a clip the library never lists.
  const std::string nul_file = root + "\\Recordings\\Nul base.mkv";
  write_bytes(nul_file, 512);
  std::string nul_name = "gone";
  nul_name.push_back('\0');
  nul_name += ".mp4";
  check(!bx::library_rename(nul_file, nul_name, renamed), "a new name holding a NUL is refused");
  check(GetFileAttributesA(nul_file.c_str()) != INVALID_FILE_ATTRIBUTES, "and the file keeps its own name");
  check(bx::library_delete(nul_file), "the NUL test's file deletes");

  // The same chunk written the other way: a data root configured with forward slashes makes the
  // sessions root forward-slash while a chunk path comes back with backslashes, so a literal compare
  // let a rename through on a running engine. A second file, so a failing run cannot eat the first.
  const std::string chunk2 = bx::session_media_dir(session.id) + "\\BuffrX recording 2.mkv";
  write_bytes(chunk2, 1024);
  std::string slashed_chunk = chunk2;
  std::replace(slashed_chunk.begin(), slashed_chunk.end(), '\\', '/');
  check(!bx::library_rename(slashed_chunk, "renamed chunk", renamed),
        "renaming a chunk written with forward slashes is refused");
  check(GetFileAttributesA(chunk2.c_str()) != INVALID_FILE_ATTRIBUTES, "and the slashed chunk keeps its name");
  check(!bx::library_delete(slashed_chunk), "deleting a chunk written with forward slashes is refused");
  check(GetFileAttributesA(chunk2.c_str()) != INVALID_FILE_ATTRIBUTES, "and the refused delete left it there");

  // A folder whose name merely starts with the sessions root is not the sessions root: it is an
  // ordinary clip folder, and refusing files there would break rename and delete for them.
  const std::string sibling_dir = root + "\\sessions-old";
  CreateDirectoryA(sibling_dir.c_str(), nullptr);
  const std::string sibling = sibling_dir + "\\BuffrX recording.mkv";
  write_bytes(sibling, 512);
  check(bx::library_rename(sibling, "kept", renamed), "a file in a folder named like the sessions root plus a suffix renames");
  check(bx::library_delete(sibling_dir + "\\kept.mkv"), "and that file still deletes");

  // A saved replay the save pipeline is still trimming and moving is never listed: a listing that
  // thumbnailed it held the file open, and the trim and the move into its game folder failed.
  const std::string in_flight = root + "\\2026-10-19 12-58-27.mp4";
  write_bytes(in_flight, 1024);
  bx::library_hold(in_flight);
  bool listed = false;
  for (const bx::MediaItem& item : bx::list_clips()) listed = listed || item.file == in_flight;
  check(!listed && bx::library_is_held(in_flight), "a held replay is not listed");
  bx::library_release(in_flight);
  listed = false;
  for (const bx::MediaItem& item : bx::list_clips()) listed = listed || item.file == in_flight;
  check(listed && !bx::library_is_held(in_flight), "a released replay is listed again");
  bx::library_hold(root + "/2026-10-19 12-58-27.MP4");
  listed = false;
  for (const bx::MediaItem& item : bx::list_clips()) listed = listed || item.file == in_flight;
  check(!listed, "a hold matches whatever the slashes and case");
  bx::library_release(in_flight);
  check(bx::library_delete(in_flight), "the test replay deletes");

  // A Timeline save joins into <clip>.part.<container> and renames it over the clip only once the whole
  // file has verified: the half-made copy must never appear in a listing that runs while the join writes.
  const std::string joining = root + "\\2026-10-19 12-58-29.mp4";
  const std::string staged_copy = joining + ".part.mp4";
  write_bytes(joining, 4096);
  write_bytes(staged_copy, 1024);
  bool saw_joining = false, saw_staged = false;
  for (const bx::MediaItem& item : bx::list_clips()) {
    saw_joining = saw_joining || item.file == joining;
    saw_staged = saw_staged || item.file == staged_copy;
  }
  check(saw_joining, "a clip is listed");
  check(!saw_staged, "while the copy a save is still joining is not");
  check(bx::library_delete(joining), "and the clip the join finished deletes");
  DeleteFileA(staged_copy.c_str());

  bx::library_init({});
}

}  // namespace

// Encoder settings: the tier presets must be family-correct, and an explicit option must always win.
// The x264 case is the one that shipped broken (it was handed NVENC's "p7" and logged Invalid preset).
void test_encoder_settings() {
  std::printf("encoder settings\n");
  using bx::EncoderFamily;
  check(bx::encoder_family("obs_nvenc_h264_tex") == EncoderFamily::Nvenc, "nvenc id is the nvenc family");
  check(bx::encoder_family("obs_nvenc_av1_tex") == EncoderFamily::Nvenc, "nvenc av1 is the nvenc family");
  check(bx::encoder_family("obs_x264") == EncoderFamily::X264, "x264 id is the x264 family");
  check(bx::encoder_family("h264_texture_amf") == EncoderFamily::Amf, "amf id is the amf family");
  check(bx::encoder_family("obs_qsv11_av1") == EncoderFamily::Qsv, "qsv id is the qsv family");
  check(bx::encoder_family("some_future_encoder") == EncoderFamily::Other, "an unknown id is Other");

  bx::Config config;
  config.keyframe_seconds = 3;
  // Standard is its own NVENC tier: p4, no multipass, no adaptive quantization. It is the light end of Balanced.
  const auto nvenc_standard = bx::encoder_tier_defaults(EncoderFamily::Nvenc, "standard", config);
  check(bx::encoder_setting_value(nvenc_standard, "preset").as_string() == "p4", "nvenc standard preset is p4");
  check(bx::encoder_setting_value(nvenc_standard, "multipass").as_string() == "disabled", "nvenc standard has no multipass");
  check(bx::encoder_setting_value(nvenc_standard, "adaptive_quantization").as_bool() == false,
        "nvenc standard has no adaptive quantization");
  const auto nvenc = bx::encoder_tier_defaults(EncoderFamily::Nvenc, "balanced", config);
  check(bx::encoder_setting_value(nvenc, "preset").as_string() == "p5", "nvenc balanced preset is p5");
  check(bx::encoder_setting_value(nvenc, "multipass").as_string() == "qres", "nvenc balanced multipass is qres");
  check(bx::encoder_setting_value(nvenc, "keyint_sec").as_int() == 3, "keyint_sec follows the config");
  check(bx::encoder_setting_value(nvenc, "lookahead").as_bool() == false, "nvenc balanced has no look-ahead: it bought no compression and costs GPU");
  check(bx::encoder_setting_value(nvenc, "tune").as_string() == "hq", "and never the UHQ tuning, which encoded 30-62% of the frames");

  // The keys the encoders actually carry. These three were written under names no encoder uses
  // (nvenc-properties.c:156,199; obs-x264.c:179-219), so apply_encoder_setting refused them and
  // they were silently inert on every card: the tuning and AQ knobs had never done anything.
  check(bx::encoder_setting_value(nvenc, "tune").as_string() == "hq", "nvenc tuning uses the key nvenc calls tune");
  check(bx::encoder_setting_value(nvenc, "tuning").is_null(), "the unknown 'tuning' key is not written at all");
  check(bx::encoder_setting_value(nvenc, "adaptive_quantization").as_bool() == true,
        "nvenc writes the quantizer control under its own name");
  check(bx::encoder_setting_value(nvenc, "psycho_aq").is_null(), "the unknown 'psycho_aq' key is not written at all");

  bx::Config quality_config = config;
  quality_config.codec = "h264";
  const auto nvenc_quality = bx::encoder_tier_defaults(EncoderFamily::Nvenc, "quality", quality_config);
  check(bx::encoder_setting_value(nvenc_quality, "preset").as_string() == "p7", "nvenc quality preset is p7");
  check(bx::encoder_setting_value(nvenc_quality, "lookahead").as_bool() == true, "nvenc quality turns lookahead on");
  check(bx::encoder_setting_value(nvenc_quality, "multipass").as_string() == "fullres" && bx::encoder_setting_value(nvenc_quality, "tune").as_string() == "hq",
        "with full-resolution multipass and the HQ tuning, never UHQ");
  // Highest plus Maximum is the user's own choice of the slowest settings (decided 2026-11-05): p7 and
  // full-resolution multipass at every frame rate. The governor steps back when the encoder cannot keep up.
  bx::Config fast = config;
  fast.capture_fps = 144;
  const auto nvenc_quality_fast = bx::encoder_tier_defaults(EncoderFamily::Nvenc, "quality", fast);
  check(bx::encoder_setting_value(nvenc_quality_fast, "preset").as_string() == "p7", "the quality tier stays p7 above 120 fps");
  check(bx::encoder_setting_value(nvenc_quality_fast, "multipass").as_string() == "fullres", "with full-resolution multipass");
  check(bx::encoder_setting_value(nvenc_quality_fast, "bf").as_int(9) == 0, "and no B-frames: they cost 120 MB of working set for 2-6% of the bits");
  check(bx::encoder_setting_value(bx::encoder_tier_defaults(EncoderFamily::Nvenc, "balanced", config), "bf").as_int(9) == 0 &&
            bx::encoder_setting_value(bx::encoder_tier_defaults(EncoderFamily::Nvenc, "performance", config), "bf").as_int(9) == 0,
        "in every tier");
  check(bx::encoder_setting_value(nvenc_quality_fast, "lookahead").as_bool(false), "and look-ahead on");
  fast.capture_fps = 120;
  fast.codec = "h264";
  check(bx::encoder_setting_value(bx::encoder_tier_defaults(EncoderFamily::Nvenc, "quality", fast), "preset").as_string() == "p7",
        "120 fps is still p7");
  // HEVC takes the same top tier now: the user's setting decides, not a per-codec cap. The old measurement
  // (p7 encoded 45% of the frames on an RTX 5080) is why the governor exists.
  bx::Config hevc = config;
  hevc.codec = "hevc";
  const auto hevc_quality = bx::encoder_tier_defaults(EncoderFamily::Nvenc, "quality", hevc);
  check(bx::encoder_setting_value(hevc_quality, "preset").as_string() == "p7" && bx::encoder_setting_value(hevc_quality, "multipass").as_string() == "fullres",
        "the HEVC quality tier is p7 with full multipass too");

  const auto x264 = bx::encoder_tier_defaults(EncoderFamily::X264, "balanced", config);
  check(bx::encoder_setting_value(x264, "preset").as_string() == "medium", "x264 balanced preset is medium");
  check(bx::encoder_setting_value(x264, "preset").as_string().rfind("p", 0) != 0, "x264 is never handed an NVENC p-name");
  check(bx::encoder_setting_value(x264, "multipass").is_null(), "x264 has no NVENC multipass key");
  check(bx::encoder_setting_value(x264, "bf").is_null(), "x264 has no B-frame key; its preset carries that");
  const auto x264_quality = bx::encoder_tier_defaults(EncoderFamily::X264, "quality", config);
  check(bx::encoder_setting_value(x264_quality, "preset").as_string() == "slow", "x264 quality preset is slow");

  const auto amf = bx::encoder_tier_defaults(EncoderFamily::Amf, "performance", config);
  check(bx::encoder_setting_value(amf, "preset").as_string() == "speed", "amf performance uses the speed preset");

  // QSV's target usage is numbered TU1..TU7 (QSV_Encoder.h:89), not named, so the words written here
  // before were refused and the Intel tier never applied either.
  const auto qsv_fast = bx::encoder_tier_defaults(EncoderFamily::Qsv, "performance", config);
  const auto qsv_mid = bx::encoder_tier_defaults(EncoderFamily::Qsv, "balanced", config);
  const auto qsv_slow = bx::encoder_tier_defaults(EncoderFamily::Qsv, "quality", config);
  check(bx::encoder_setting_value(qsv_fast, "target_usage").as_string() == "TU7", "qsv performance asks for TU7");
  check(bx::encoder_setting_value(qsv_mid, "target_usage").as_string() == "TU4", "qsv balanced asks for TU4");
  check(bx::encoder_setting_value(qsv_slow, "target_usage").as_string() == "TU2", "qsv quality asks for TU2");
  bx::Json tu_values = bx::Json::array();
  for (const char* tu : {"TU1", "TU2", "TU3", "TU4", "TU5", "TU6", "TU7"}) tu_values.push(bx::Json(tu));
  check(bx::encoder_option_valid(bx::Json("TU4"), tu_values, false, 0, 0),
        "the number qsv writes is a value qsv offers");

  // An explicit option wins; a null option removes the default; a new key is appended.
  bx::Json overrides = bx::Json::object();
  overrides.set("preset", bx::Json("p1"));
  overrides.set("multipass", bx::Json());
  overrides.set("split_encode", bx::Json(true));
  const auto merged = bx::merge_encoder_settings(nvenc, overrides);
  check(bx::encoder_setting_value(merged, "preset").as_string() == "p1", "an explicit preset wins over the tier");
  check(bx::encoder_setting_value(merged, "multipass").is_null(), "a null override removes the default");
  check(bx::encoder_setting_value(merged, "split_encode").as_bool() == true, "a new override is appended");
  check(bx::encoder_setting_value(merged, "tune").as_string() == "hq", "an untouched default survives");

  // A value the encoder does not offer must be rejected rather than written (x264 "Invalid preset: p7").
  bx::Json allowed = bx::Json::array();
  allowed.push(bx::Json("medium"));
  allowed.push(bx::Json("slow"));
  check(bx::encoder_option_valid(bx::Json("medium"), allowed, false, 0, 0), "a listed option is valid");
  check(!bx::encoder_option_valid(bx::Json("p7"), allowed, false, 0, 0), "an unlisted option is refused");
  check(bx::encoder_option_valid(bx::Json(2), bx::Json(), true, 0, 4), "a number within range is valid");
  check(!bx::encoder_option_valid(bx::Json(7), bx::Json(), true, 0, 4), "a number out of range is refused");
  check(bx::encoder_option_valid(bx::Json("anything"), bx::Json(), false, 0, 0), "no list means no constraint");
}

// The bitrate ladder: a rate per codec and resolution, not one number for everything. The regression
// this guards: the engine computed the bitrate from the memory budget alone, so AV1 was encoded at an
// H.264 rate, 4K at a 720p rate, and a 600 s buffer was refused outright.
void test_bitrate_plan() {
  std::printf("bitrate plan\n");
  // 1080p60 balanced, the reference point every constant below was calibrated against.
  check(bx::bitrate_plan("h264", 1920, 1080, 60, "balanced") == 18040, "1080p60 h264 balanced is 18040 kbps");
  check(bx::bitrate_plan("hevc", 1920, 1080, 60, "balanced") == 13064, "1080p60 hevc is about 72% of h264");
  check(bx::bitrate_plan("av1", 1920, 1080, 60, "balanced") == 8709, "1080p60 av1 is about 48% of h264");

  // The codecs are ordered at every resolution, and so are the tiers.
  check(bx::bitrate_plan("h264", 2560, 1440, 60, "balanced") >
            bx::bitrate_plan("hevc", 2560, 1440, 60, "balanced") &&
            bx::bitrate_plan("hevc", 2560, 1440, 60, "balanced") >
                bx::bitrate_plan("av1", 2560, 1440, 60, "balanced"),
        "h264 > hevc > av1 at 1440p");
  check(bx::bitrate_plan("h264", 1920, 1080, 60, "performance") <
            bx::bitrate_plan("h264", 1920, 1080, 60, "balanced"),
        "the fast tier is under the balanced tier");
  check(bx::bitrate_plan("h264", 1920, 1080, 60, "balanced") <
            bx::bitrate_plan("h264", 1920, 1080, 60, "quality"),
        "the quality tier is over the balanced tier");
  check(bx::bitrate_plan("h264", 1920, 1080, 30, "balanced") <
            bx::bitrate_plan("h264", 1920, 1080, 60, "balanced"),
        "30 fps asks for less than 60");
  check(bx::bitrate_plan("h264", 3840, 2160, 60, "balanced") >
            bx::bitrate_plan("h264", 1920, 1080, 60, "balanced"),
        "4K asks for more than 1080p");

  // The clamps, and the degenerate inputs a capture source can hand over.
  check(bx::bitrate_plan("h264", 8, 8, 60, "balanced") >= 1000, "a tiny source still gets 1 Mbps");
  check(bx::bitrate_plan("h264", 7680, 4320, 240, "quality") <= 200000, "8K240 is clamped");
  check(bx::bitrate_plan("h264", 0, 0, 0, "balanced") == bx::bitrate_plan("h264", 1920, 1080, 60, "balanced"),
        "an unknown size is answered at 1080p60");
  check(bx::bitrate_plan("h264", 0, 0, 0, "balanced") < bx::bitrate_plan("h264", 0, 0, 0, "quality"),
        "an unknown size still respects the tier");
  check(bx::bitrate_plan("h264", 1920, 1080, 60, "nonsense") == bx::bitrate_plan("h264", 1920, 1080, 60, "balanced"),
        "an unknown tier is the balanced tier");
  check(bx::bitrate_plan("vp9", 1920, 1080, 60, "balanced") == bx::bitrate_plan("h264", 1920, 1080, 60, "balanced"),
        "an unknown codec is the most demanding one");

  // A 600 s buffer used to compute under 1000 kbps and be refused outright. It must hold now.
  check(bx::bitrate_plan("h264", 1280, 720, 60, "balanced") > 1000, "720p60 has a usable rate to hold 600 s of");

  // Quality levels: each preset puts a rate on the slider that fits its name, for the codec, size and
  // frame rate being encoded. AV1 1440p60 High is the ladder's balanced answer.
  const int high = bx::preset_kbps("av1", 2560, 1440, 60, "high");
  check(high == bx::bitrate_plan("av1", 2560, 1440, 60, "balanced"), "High is the ladder's balanced rate");
  check(bx::preset_kbps("av1", 2560, 1440, 60, "low") < bx::preset_kbps("av1", 2560, 1440, 60, "standard") &&
            bx::preset_kbps("av1", 2560, 1440, 60, "standard") < high &&
            high < bx::preset_kbps("av1", 2560, 1440, 60, "highest"),
        "Low < Standard < High < Highest");
  check(bx::preset_kbps("h264", 2560, 1440, 60, "high") > bx::preset_kbps("hevc", 2560, 1440, 60, "high") &&
            bx::preset_kbps("hevc", 2560, 1440, 60, "high") > high,
        "the same preset asks less of HEVC and less again of AV1");
  check(bx::preset_kbps("av1", 2560, 1440, 60, "nonsense") == high, "an unknown level is High");
  check(bx::quality_target("low") == 28 && bx::quality_target("standard") == 23 && bx::quality_target("high") == 20 &&
            bx::quality_target("highest") == 16 && bx::quality_target("custom") == 20,
        "each level has its constant-quality target");

  // A custom rate keeps its picture across codecs: 60 Mbps of H.264 is about 29 Mbps of AV1.
  const int av1 = bx::rescale_for_codec(60000, "h264", "av1");
  check(av1 > 28000 && av1 < 30000, "a custom rate is scaled by the codecs' bits per pixel");
  check(bx::rescale_for_codec(bx::rescale_for_codec(30000, "hevc", "h264"), "h264", "hevc") == 30000,
        "and scaling back gives the same rate");
  check(bx::rescale_for_codec(30000, "av1", "av1") == 30000, "the same codec leaves it alone");

  bx::Config preset;
  preset.codec = "av1";
  preset.quality = "standard";
  check(bx::quality_kbps(preset, 2560, 1440) == bx::preset_kbps("av1", 2560, 1440, 60, "standard"),
        "a preset's rate follows the size and codec being encoded");
  preset.custom_kbps = 40000;
  check(bx::quality_kbps(preset, 2560, 1440) == bx::preset_kbps("av1", 2560, 1440, 60, "standard"),
        "an automatic peak ignores a stored rate");
  preset.peak_mode = "fixed";
  check(bx::quality_kbps(preset, 2560, 1440) == 40000, "a fixed peak is the rate on the slider, under any level");
  preset.custom_kbps = 0;
  check(bx::quality_kbps(preset, 2560, 1440) == bx::preset_kbps("av1", 2560, 1440, 60, "standard"),
        "a fixed peak with no rate yet falls back to the level's");

  // A fixed rate chooses the picture, so the level's target and speed do not apply: the encoder runs High.
  preset.quality = "highest";
  check(bx::effective_quality(preset) == "high", "a fixed rate encodes at High, whatever level is stored");
  check(bx::quality_target(bx::effective_quality(preset)) == bx::quality_target("high"), "its target is High's");
  preset.peak_mode = "auto";
  check(bx::effective_quality(preset) == "highest", "an automatic rate keeps the level chosen");
  preset.quality = "custom";
  check(bx::effective_quality(preset) == "high", "Custom runs High");

  // Video presets: one choice writes the level, an automatic rate and Auto effort together.
  check(std::string(bx::video_preset_name("low")) == "low" && std::string(bx::video_preset_name("custom")) == "custom" &&
            std::string(bx::video_preset_name("nonsense")) == "high",
        "the five presets, and anything else reads as High");
  const bx::Json high_patch = bx::video_preset_patch("highest");
  check(high_patch["quality"].as_string() == "highest" && high_patch["peak_mode"].as_string() == "auto" &&
            high_patch["encoder_mode"].as_string() == "auto" && high_patch["encoder_options"].is_object(),
        "a preset sets the level, an automatic rate and Auto effort");
  check(bx::video_preset_patch("custom").items().empty(), "Custom writes nothing");
  bx::Config shown;
  shown.video_preset.clear();
  shown.quality = "highest";
  check(bx::video_preset_from_config(shown) == "highest", "a level with an automatic rate shows as its preset");
  shown.video_preset = "low";
  check(bx::video_preset_from_config(shown) == "low", "a stored preset is the one shown");
  shown.peak_mode = "fixed";
  shown.video_preset = "high";
  check(bx::video_preset_from_config(shown) == "custom", "a fixed rate outranks a stored preset");
  shown.video_preset.clear();
  check(bx::video_preset_from_config(shown) == "custom", "a fixed rate with no preset shows as Custom");
  shown.peak_mode = "auto";
  shown.quality = "custom";
  check(bx::video_preset_from_config(shown) == "custom", "a hand-built quality shows as Custom");

  // The ring's safety ceiling: an eighth of the machine's memory, never under 64 MB, 2 GB when the memory is not known. It is not a
  // quality limit and not a setting: the ring keeps the length asked for at the quality chosen.
  const uint64_t gib = 1024ull * 1024 * 1024;
  check(bx::ring_safety_mb(32 * gib) == 4096, "an eighth of 32 GB is 4 GB");
  check(bx::ring_safety_mb(8 * gib) == 1024 && bx::ring_safety_mb(16 * gib) == 2048, "an eighth of the machine, whatever its size");
  check(bx::ring_safety_mb(256 * 1024 * 1024) == 64, "never under the 64 MB floor");
  check(bx::ring_safety_mb(0) == 2048, "an unknown amount of memory is 2 GB");
}

// How much of a library listing is decoded up front. The regression this guards: every clip in the
// library had a thumbnail generated on the lock-free worker, so a 200-clip library opened 200 files
// through FFmpeg and stalled the folder picker and ui.capture behind it.
void test_thumbnail_budget() {
  std::printf("thumbnail budget\n");
  // Nothing on screen means nothing is decoded.
  check(bx::thumbnail_budget(0, 200) == 0, "a closed view decodes nothing");
  check(bx::thumbnail_budget(-4, 200) == 0, "a nonsense view decodes nothing");
  check(bx::thumbnail_budget(12, 0) == 0, "an empty library decodes nothing");
  // What is visible is covered, with a screen of lookahead so a scroll does not stall.
  check(bx::thumbnail_budget(12, 200) > 12, "the visible cards plus a lookahead");
  check(bx::thumbnail_budget(12, 200) <= 24, "the lookahead is at most one more screen");
  check(bx::thumbnail_budget(40, 200) == 80, "a big view gets a screen again");
  // Never more than there is to decode.
  check(bx::thumbnail_budget(12, 5) == 5, "a short library is not over-drawn");
  check(bx::thumbnail_budget(12, 1) == 1, "one clip decodes one");
  check(bx::thumbnail_budget(1, 1) == 1, "a single visible clip decodes");
  check(bx::thumbnail_budget(1, 200) == 9, "one visible card still gets a lookahead");
}

// The bounded hand-off the trim worker runs on.
// previous clip's whole remux, so a second save froze the engine Ã¢â‚¬â€ and with it the watchdog, the
// status broadcast and every hotkey Ã¢â‚¬â€ for as long as the first trim took.
void test_work_queue() {
  std::printf("work queue\n");
  using bx::WorkQueue;

  // One item, one worker, and the producer never waits for the consumer.
  {
    WorkQueue<int> queue(4);
    std::atomic<int> total{0};
    queue.start([&](int value) {
      Sleep(20);
      total += value;
    });
    check(queue.push(1), "the first item is accepted");
    check(queue.push(2), "the second item is accepted");
    Sleep(300);
    check(total.load() == 3, "both items ran");
    check(queue.running(), "the worker is running");
    queue.stop();
    check(!queue.running(), "stop joins the worker");
  }

  // Order is preserved: clips are trimmed in the order they were saved.
  {
    WorkQueue<int> queue(8);
    std::mutex done_mutex;
    std::vector<int> done;
    queue.start([&](int value) {
      std::lock_guard<std::mutex> lock(done_mutex);
      done.push_back(value);
    });
    for (int i = 1; i <= 5; ++i) check(queue.push(i), "queued in order");
    queue.stop();
    std::lock_guard<std::mutex> lock(done_mutex);
    check(done.size() == 5, "every queued item ran");
    bool ordered = true;
    for (size_t i = 0; i < done.size(); ++i) ordered = ordered && done[i] == static_cast<int>(i) + 1;
    check(ordered, "the worker took them in the order they were queued");
  }

  // The ceiling: work is refused, not buffered forever. This is the unbounded-growth guard.
  {
    WorkQueue<int> queue(2);
    std::atomic<bool> release{false};
    std::atomic<int> taken{0};
    queue.start([&](int) {
      ++taken;
      while (!release.load()) Sleep(5);
    });
    check(queue.push(1), "one is accepted");
    Sleep(60);  // let the worker pick it up
    check(queue.push(2), "two is accepted");
    check(queue.push(3), "three is accepted");
    check(!queue.push(4), "the fourth is refused at the ceiling");
    check(queue.pending() <= 2, "the queue never holds more than its capacity");
    check(queue.capacity() == 2, "the capacity is what it was given");
    release = true;
    queue.stop();
    check(taken.load() >= 1, "the worker drained what it was given");
  }

  // The ceiling counts what is waiting, not what is running: a capacity of one still admits a second
  // item while the worker is busy with the first, and refuses the third. That is the point of the
  // number Ã¢â‚¬â€ it bounds the backlog, and one extra file in hand is cheaper than refusing a save.
  {
    WorkQueue<int> queue(1);
    std::atomic<bool> release{false};
    queue.start([&](int) {
      while (!release.load()) Sleep(5);
    });
    check(queue.push(1), "the first is accepted");
    Sleep(60);  // the worker takes it and blocks
    check(queue.push(2), "a second is accepted while the first is still running");
    check(!queue.push(3), "the third is refused: one is running, one is waiting");
    release = true;
    queue.stop();
    check(!queue.push(4), "a stopped queue accepts nothing");
  }

  // Stop drains rather than discards: a clip already queued when the engine shuts down is still
  // trimmed, because it is a file the user was told about.
  {
    WorkQueue<int> queue(4);
    std::atomic<int> ran{0};
    queue.start([&](int) { ++ran; });
    for (int i = 0; i < 3; ++i) queue.push(i);
    queue.stop();
    check(ran.load() == 3, "stop drained the queue instead of dropping it");
    check(queue.pending() == 0, "nothing is left behind");
  }

  // A stopped queue can start again: engine.restart calls shutdown (which stops every worker) and
  // initialize, and the new worker must run. Before this, stopping_ stayed true and every trim,
  // filing, retention sweep and reaper was dead for the rest of the process.
  {
    WorkQueue<int> queue(4);
    std::atomic<int> ran{0};
    queue.start([&](int) { ++ran; });
    check(queue.push(1), "the first worker takes an item");
    queue.stop();
    const int before_restart = ran.load();
    queue.start([&](int) { ++ran; });
    check(queue.running(), "a queue can start again after stop");
    check(queue.push(2), "the restarted worker accepts work");
    queue.stop();
    check(ran.load() == before_restart + 1, "the restarted worker ran the item");
  }

  // A zero capacity is a mistake, not a queue that refuses everything.
  {
    WorkQueue<int> queue(0);
    check(queue.capacity() == 1, "a zero capacity still admits one item");
    check(queue.push(1), "so the first push succeeds");
    queue.stop();
  }
}

  // One rebuild at a time. The regression this guards: two settings changes arriving together both
// entered ObsHost::apply and one of them ran obs_reset_video against a pipeline the other was
// already tearing down.
void test_rebuild_gate() {
  std::printf("rebuild gate\n");
  bx::RebuildGate gate;
  check(!gate.busy(), "nothing is rebuilding to begin with");

  // The flag is published while the gate is held, so a reader can ask without blocking.
  {
    bx::RebuildGuard guard(gate);
    check(gate.busy(), "a rebuild in progress says so");
  }
  check(!gate.busy(), "and stops saying so when it is done");

  // The real guarantee: a second rebuild waits rather than overlapping.
  std::atomic<bool> first_entered{false};
  std::atomic<bool> release_first{false};
  std::atomic<bool> second_entered_while_first_held{true};
  std::atomic<bool> second_started{false};
  std::thread first([&] {
    bx::RebuildGuard guard(gate);
    first_entered = true;
    while (!release_first.load()) Sleep(5);
  });
  while (!first_entered.load()) Sleep(5);

  std::thread second([&] {
    second_started = true;
    bx::RebuildGuard guard(gate);
    // If this can be true, the gate did not hold.
    if (first_entered.load() && !release_first.load()) second_entered_while_first_held = false;
  });
  for (int i = 0; i < 200 && !second_started.load(); ++i) Sleep(5);
  check(second_started.load(), "the second rebuild reached the gate");
  check(second_entered_while_first_held.load(), "a second rebuild waits for the first to finish");
  check(gate.busy(), "the gate is still held while the first rebuild runs");
  release_first = true;
  first.join();
  second.join();
  check(!gate.busy(), "both finished and the gate is free");

  // And it is usable again afterwards: a gate that stays shut would hang the engine forever.
  {
    bx::RebuildGuard guard(gate);
    check(gate.busy(), "a third rebuild can take it");
  }
  check(!gate.busy(), "and gives it back");
}

// The exclusion list the interface shows. Rewind's denylist is already carried over and already
// decides what counts as a game; what was missing was that a person could not see it or add to it
// without knowing the file name. The rule this guards: the built-in entries are shown as the
// product's own list and cannot be edited from the panel, and only the user's own entries can be.
void test_exclusion_view() {
  std::printf("exclusion view\n");
  // A name the product already excludes is a built-in, whatever the user typed.
  check(bx::exclusion_is_builtin("chrome.exe"), "a browser is one of ours");
  check(bx::exclusion_is_builtin("Code.exe"), "case does not matter");
  check(bx::exclusion_is_builtin("steam"), "a bare name is ours too");
  // Something the user added is theirs, and the panel may remove it.
  check(!bx::exclusion_is_builtin("myapp.exe"), "an unknown name is the user's own");
  check(!bx::exclusion_is_builtin(""), "an empty entry is not a built-in");
  // A duplicate of ours is still ours: the panel must not offer to delete an entry that would come
  // back the moment the engine restarted.
  check(bx::exclusion_is_builtin("vlc"), "a duplicate of a built-in stays built-in");
  // And the list the panel renders puts the user's own entries first - buried under the hundred-odd
  // names the product already excludes, an entry someone just typed looks like it did not save - then
  // the built-ins in the order the product declares them.
  const std::vector<std::string> combined = bx::exclusion_entries({"myapp.exe"}, {"chrome.exe", "Code.exe"});
  check(combined.size() == 3, "both lists are shown");
  check(combined[0] == "myapp.exe", "the user's own entry comes first");
  check(combined[1] == "chrome.exe" && combined[2] == "Code.exe", "then the built-ins, in order");
  // A name in both lists appears once, and it is the built-in that survives - it is the one that
  // would come back anyway, so it must not be offered for deletion.
  check(bx::exclusion_entries({"chrome.exe"}, {"chrome.exe"}).size() == 1, "a duplicate appears once");
  check(bx::exclusion_is_builtin(bx::exclusion_entries({"chrome.exe"}, {"chrome.exe"})[0]),
        "a name the product also excludes is marked as ours, not the user's");
  // An empty entry in either list is dropped rather than shown as a blank row.
  check(bx::exclusion_entries({""}, {""}).empty(), "empty entries are dropped");
}

// The two startup switches. The regression this guards: neither existed, so "start with the buffer
// off" and "run as administrator" were things the user could not ask for at all.
void test_startup_switches() {
  std::printf("startup switches\n");
  // Both survive a round trip, because a setting the engine forgets on restart is not a setting.
  bx::Config config;
  config.start_buffering = false;
  config.run_as_admin = true;
  bx::Config back;
  bx::config_from_json(bx::config_to_json(config), back);
  check(!back.start_buffering, "start with the buffer off survives");
  check(back.run_as_admin, "run as admin survives");
  // And the defaults are the safe ones: neither is on until asked for.
  const bx::Config defaults;
  check(defaults.start_buffering, "the buffer starts by default");
  check(!defaults.run_as_admin, "admin is off by default");
  // A stored value of the wrong type is ignored rather than trusted: 1 is not a bool here.
  bx::Config odd;
  bx::Json json = bx::Json::object();
  json.set("start_buffering", bx::Json(1));
  json.set("run_as_admin", bx::Json("yes"));
  bx::config_from_json(json, odd);
  check(odd.start_buffering, "a number where a bool belongs leaves the default alone");
  check(!odd.run_as_admin, "a string where a bool belongs leaves the default alone");
}

// The session store's root. The regression this guards: the store created its folder once, at
// startup, and then refused every recording with "the session store has no usable root" forever if
// that folder went missing - a user deleting or moving their recordings folder could not record
// again until the engine was restarted.
void test_session_store_root() {
  std::printf("session store root\n");
  const std::string base = temp_directory() + "\\sessionroot";
  remove_tree(base);
  CreateDirectoryA(base.c_str(), nullptr);
  const std::string root = base + "\\sessions";
  bx::session_store_init(root);
  check(GetFileAttributesA(root.c_str()) != INVALID_FILE_ATTRIBUTES, "the store creates its root at init");

  // Now the folder goes away, the way a person deleting a folder would.
  remove_tree(root);
  check(GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES, "the root is really gone");
  bx::SessionEntry entry;
  std::string error;
  check(bx::session_store_begin("recording", entry, error), "a missing root is created again, not refused");
  check(error.empty(), "and no error is reported");
  check(GetFileAttributesA(root.c_str()) != INVALID_FILE_ATTRIBUTES, "the root is there again");

  // A root whose parent does not exist is still refused: nothing may be invented where the user
  // did not ask for it. The refusal has to say which folder is missing.
  const std::string orphan = temp_directory() + "\\no-such-parent\\sessions";
  bx::session_store_init(orphan);
  bx::SessionEntry refused;
  std::string why;
  check(!bx::session_store_begin("recording", refused, why), "a root with no parent is refused");
  check(why.find("no-such-parent") != std::string::npos, "and the refusal names the missing folder");
  remove_tree(base);
}

// The recording mark's dot. The regression this guards: the mark is a window of the engine's own and
// never in the recording, so the shape of it is what the player sees - a mistake here is a red blob
// in the corner of the screen.
void test_rec_mark() {
  std::printf("recording mark\n");
  const double radius = 11.0;
  const double core = radius * 0.42;
  check(bx::dot_falloff(0.0, core, radius) == 1.0, "the middle of the dot is solid");
  check(bx::dot_falloff(core, core, radius) == 1.0, "the core is solid to its edge");
  check(bx::dot_falloff(radius, core, radius) == 0.0, "the rim of the dot is gone");
  check(bx::dot_falloff(radius * 2.0, core, radius) == 0.0, "past the rim there is nothing");
  // The edge is soft rather than a staircase, so the dot does not look jagged.
  const double edge = bx::dot_falloff(core + (radius - core) * 0.5, core, radius);
  check(edge > 0.0 && edge < 1.0, "the edge of the dot fades");
  check(edge * edge < edge, "the fade is not linear");
  // A core that reaches the rim would divide by zero; the shape stays finite.
  check(bx::dot_falloff(radius, radius, radius) == 1.0, "a core at the rim stays solid");
}

// The encoder's own geometry and quantizer. The rate is only right if it is computed from the size
// the encoder actually receives, and the same CQ number means different quality on different codecs.
void test_encoder_geometry() {
  std::printf("encoder geometry\n");
  bx::Config config;
  int width = 0, height = 0;

  // No configured output: the encoder gets the canvas, which is the capture target's own size.
  bx::encoder_size(config, 2560, 1440, width, height);
  check(width == 2560 && height == 1440, "with no output size the encoder gets the canvas");

  // A configured output is what the encoder is scaled to, and the rate must follow it.
  config.output_width = 1920;
  config.output_height = 1080;
  bx::encoder_size(config, 2560, 1440, width, height);
  check(width == 1920 && height == 1080, "a configured output size wins over the canvas");

  // A half-configured output is not a size: it falls back rather than asking for a 1920-pixel-wide
  // stream of height 1.
  config.output_height = 0;
  bx::encoder_size(config, 2560, 1440, width, height);
  check(width == 2560 && height == 1440, "half a configured size is not a size");

  // And the rate follows that geometry: the same tier at 1080p asks for less than at 1440p.
  const int at_canvas = bx::bitrate_plan("h264", 2560, 1440, 60, "balanced");
  const int at_output = bx::bitrate_plan("h264", 1920, 1080, 60, "balanced");
  check(at_output < at_canvas, "the configured output lowers the rate");

  // The quantizer is not portable: NVENC, AMF and QSV all run 0-51 for H.264/HEVC and 0-63 for AV1,
  // so one slider spanning 10-36 meant "near-lossless" on H.264 and "visibly soft" on AV1.
  check(bx::cq_for_codec("h264", 18) == 18, "h264 passes the quality number through");
  check(bx::cq_for_codec("av1", 18) == 22, "av1 scales the same number to its wider range");
  check(bx::cq_for_codec("hevc", 18) == 18, "hevc uses the h264 scale");
  check(bx::cq_for_codec("h264", 0) == 1, "a quantizer below the floor is lifted");
  check(bx::cq_for_codec("av1", 63) == 63, "the av1 ceiling is reachable");
  check(bx::cq_for_codec("h264", 63) <= 51, "h264 never exceeds its own ceiling");
  // The tiers land where they should on every codec, and the tiers keep their order on AV1 too.
  check(bx::cq_for_codec("av1", 10) < bx::cq_for_codec("av1", 18) && bx::cq_for_codec("av1", 18) < bx::cq_for_codec("av1", 26),
        "the av1 tiers keep their order");
}

// What the engine does about a recording output libobs will not stop. It cannot be released - doing
// so blocks forever - so the leak it leaves behind is bounded instead of ignored: past a few, no new
// recording is started until the engine is restarted, rather than leaking another encoder and another
// set of GPU surfaces on every chunk boundary.
void test_abandoned_outputs() {
  std::printf("abandoned outputs\n");
  // Under the limit, recording carries on: one stall is a bad moment, not a broken engine.
  check(!buffrx::abandoned_outputs_exhausted(0), "nothing abandoned yet");
  check(!buffrx::abandoned_outputs_exhausted(1), "one is not a reason to stop");
  check(!buffrx::abandoned_outputs_exhausted(buffrx::kMaxAbandonedOutputs - 1), "one short of the limit is fine");
  // At the limit, refuse: the next chunk would leak another encoder and another set of surfaces.
  check(buffrx::abandoned_outputs_exhausted(buffrx::kMaxAbandonedOutputs), "the limit stops new recordings");
  check(buffrx::abandoned_outputs_exhausted(buffrx::kMaxAbandonedOutputs + 3), "and it stays stopped");
  // The ceiling is small and finite on purpose.
  check(buffrx::kMaxAbandonedOutputs > 0 && buffrx::kMaxAbandonedOutputs <= 8, "the ceiling is a small number");
}

// The watchdog's verdict. The regression this guards: a user stop was undone, and because a restart
// empties the ring, a 120 s save then produced seconds.
// The notification card's colours follow the cockpit, and the chime is a well-formed WAV.
void test_clip_saved_detail() {
  using bx::ui::clip_saved_detail;
  check(clip_saved_detail("Replays", "0:20") == "Replays  \xC2\xB7  0:20", "folder and length: the card has one line");
  check(clip_saved_detail("", "0:20") == "0:20", "a clip at the root has no folder part");
  check(clip_saved_detail("Counter-Strike 2", "1:00") == "Counter-Strike 2  \xC2\xB7  1:00", "the game folder names it");
}

void test_view_clip_hotkey() {
  check(bx::Config{}.hotkey_view_clip == "Alt+4", "the viewer's key is Alt+4: the family of Alt+1, Alt+2 and Alt+3, never Alt+F4 (Windows' close)");
  check(bx::Config{}.hotkey_marker == "Alt+5", "the marker has its own default hotkey");
  bx::Json marker_patch = bx::Json::object();
  marker_patch.set("hotkey_marker", bx::Json(std::string("Ctrl+Alt+M")));
  bx::Config marker_config;
  bx::config_from_json(marker_patch, marker_config);
  check(marker_config.hotkey_marker == "Ctrl+Alt+M" && bx::config_to_json(marker_config)["hotkey_marker"].as_string() == "Ctrl+Alt+M", "marker hotkey round-trips");
  bx::Config read;
  bx::Json patch = bx::Json::object();
  patch.set("hotkey_view_clip", bx::Json(std::string("Ctrl+Alt+V")));
  bx::config_from_json(patch, read);
  check(read.hotkey_view_clip == "Ctrl+Alt+V" && bx::config_to_json(read)["hotkey_view_clip"].as_string() == "Ctrl+Alt+V", "a bound key is kept and round-trips");
  bx::Config old;
  bx::config_from_json(bx::Json::object(), old);
  check(old.hotkey_view_clip == "Alt+4", "a config file without it gets Alt+4");
  unsigned modifiers = 0, key = 0;
  std::string problem;
  check(bx::parse_chord("Alt+4", modifiers, key, problem), "Alt+4 parses as a chord");
  const std::vector<std::string> others = {bx::Config{}.hotkey_save, bx::Config{}.hotkey_quick, bx::Config{}.hotkey_screenshot,
                                           bx::Config{}.hotkey_record, bx::Config{}.hotkey_toggle_ui};
  check(std::find(others.begin(), others.end(), bx::Config{}.hotkey_view_clip) == others.end(), "it collides with none of the other defaults");
}

void test_hotkey_enable() {
  bx::HotkeyEnable confirm;
  check(!confirm.press("save_replay", "buffer", 100), "the first press only warns");
  check(confirm.press("save_replay", "buffer", 200), "the same hotkey enables the same feature");
  check(!confirm.press("save_replay", "buffer", 300), "confirmation is consumed");
  check(!confirm.press("quick_clip", "buffer", 400), "another hotkey cannot confirm the first");
  check(!confirm.press("quick_clip", "timeline", 500), "another feature cannot confirm the first");
  check(!confirm.press("quick_clip", "timeline", 500 + bx::HotkeyEnable::kWindowMs + 1), "an expired warning does not enable");
  check(confirm.press("quick_clip", "timeline", 500 + bx::HotkeyEnable::kWindowMs + 2), "a fresh warning can be confirmed");
  check(!confirm.press("view_clip", "timeline", UINT32_MAX - 50), "the clock can arm near wraparound");
  check(confirm.press("view_clip", "timeline", 30), "confirmation works across clock wraparound");
}

void test_performance_wording() {
  using bx::ui::performance_summary;
  check(performance_summary(1080, 60, 13000) == "1080p \xC2\xB7 60 fps \xC2\xB7 H.264 \xC2\xB7 about 95 MB a minute",
        "the one line that says what Performance mode records");
  check(performance_summary(720, 30, 4000) == "720p \xC2\xB7 30 fps \xC2\xB7 H.264 \xC2\xB7 about 29 MB a minute", "other sizes read the same way");
  check(performance_summary(1080, 60, 0) == "1080p \xC2\xB7 60 fps \xC2\xB7 H.264", "an unknown rate leaves the memory out");
}

void test_performance() {
  // Performance mode as a pure plan. It owns the picture: 1080p at 60 fps whatever the user's own size and rate are
  // (decided with the user, 2026-11-05), and it sizes the fixed bit rate for that picture.
  auto plan = [] { return bx::performance_plan(); };
  const bx::PerformancePlan sized = plan();
  check(sized.width == 1920 && sized.height == 1080 && sized.fps == 60, "any source is planned as 1080p60");
  check(plan().width == 1920 && plan().fps == 60, "a second call is the same plan");
  const bx::PerformancePlan p60 = plan();
  check(p60.codec == "h264" && p60.scale_filter == "bilinear" && p60.encoder_mode == "fast" && p60.audio_tracks == 1 &&
            p60.audio_bitrate_kbps == 128,
        "H.264, bilinear, the fast tier, one 128 kbps track");
  check(p60.peak_kbps >= 6000 && p60.peak_kbps <= 20000, "a 1080p60 bit rate that keeps the ring near 100 MB a minute");
  check(plan().peak_kbps == p60.peak_kbps, "the rate is planned for the plan's own picture");

  // The layer over the user's settings.
  bx::Config user;
  user.capture_fps = 120;
  user.codec = "av1";
  user.quality = "highest";
  user.encoder_mode = "quality";
  user.output_width = 2560;
  user.output_height = 1440;
  user.scale_filter = "lanczos";
  user.audio_tracks = 3;
  user.separate_tracks = true;
  user.audio_bitrate_kbps = 320;
  user.mic_enabled = true;
  user.mic_noise_suppress = true;
  user.buffer_seconds = 90;
  user.capture_mode = "game";
  user.hotkey_save = "Alt+7";
  const bx::Json before = bx::config_to_json(user);
  const bx::Config layered = bx::apply_performance_plan(user, p60);
  check(layered.codec == "h264" && layered.encoder == "auto" && layered.encoder_mode == "fast" && layered.scale_filter == "bilinear" &&
            layered.audio_tracks == 1 && !layered.separate_tracks && layered.audio_bitrate_kbps == 128 &&
            layered.peak_mode == "fixed" && layered.custom_kbps == p60.peak_kbps,
        "the plan's values replace the user's");
  check(layered.capture_fps == 60 && layered.output_width == 1920 && layered.output_height == 1080,
        "the plan owns the picture: 1080p60 whatever the user chose");
  check(layered.buffer_seconds == 90 && layered.mic_enabled && layered.mic_noise_suppress && layered.capture_mode == "game" &&
            layered.hotkey_save == "Alt+7",
        "everything else is the user's: the buffer length, the mic and its filters, the capture, the hotkeys");
  check(bx::config_to_json(user).dump() == before.dump(), "applying the layer never changes the user's config");
  check(bx::config_to_json(bx::apply_performance_plan(layered, p60)).dump() == bx::config_to_json(layered).dump(),
        "applying it twice is applying it once");

  // Ignore override metadata briefly written by an intermediate build: the Performance plan must still win.
  bx::Json intermediate = bx::config_to_json(user);
  bx::Json overrides = bx::Json::array();
  overrides.push(bx::Json("encoder_mode"));
  overrides.push(bx::Json("codec"));
  intermediate.set("performance_overrides", overrides);
  bx::Config reloaded;
  bx::config_from_json(intermediate, reloaded);
  const bx::Config restored = bx::apply_performance_plan(reloaded, p60);
  check(restored.codec == "h264" && restored.encoder_mode == "fast" && restored.capture_fps == 60,
        "Performance mode ignores overrides saved by an intermediate build");
  check(reloaded.codec == "av1" && reloaded.encoder_mode == "quality" && !bx::config_to_json(reloaded).has("performance_overrides"),
        "saved video choices survive and obsolete Performance overrides are dropped");

  // When it is on.
  using bx::PerformanceDecision;
  auto decide = [](bx::PerformanceWhen when, bool game, bool recording, bool active) {
    return bx::performance_decide(when, game, recording, active);
  };
  PerformanceDecision d = decide(bx::PerformanceWhen::always, false, true, false);
  check(d.active && !d.deferred, "ALWAYS applies at once, even during a recording: the user just chose it");
  d = decide(bx::PerformanceWhen::games, true, false, false);
  check(d.active && !d.deferred, "a listed game turns LISTED GAMES on");
  d = decide(bx::PerformanceWhen::games, true, true, false);
  check(!d.active && d.deferred, "a listed game that appears during a recording waits for the recording to end");
  d = decide(bx::PerformanceWhen::games, false, true, true);
  check(d.active && d.deferred, "a game leaving during a recording keeps it on until the recording ends");
  d = decide(bx::PerformanceWhen::games, false, false, true);
  check(!d.active && !d.deferred, "no listed game: LISTED GAMES turns off");
  d = decide(bx::PerformanceWhen::off, true, false, true);
  check(!d.active && !d.deferred, "OFF releases a stale layer");
  d = decide(bx::PerformanceWhen::off, false, false, false);
  check(!d.active && !d.deferred, "OFF stays off");

  // The config fields.
  bx::Config fresh;
  check(fresh.performance_when == bx::PerformanceWhen::off && fresh.performance_games.empty(), "defaults: off, no games");
  bx::Json patch = bx::Json::object();
  bx::Json games = bx::Json::array();
  for (const char* name : {"CS2.EXE", " cs2 ", "cs2", "", "Valorant-Win64-Shipping.exe"}) games.push(bx::Json(std::string(name)));
  patch.set("performance_games", games);
  patch.set("performance_when", bx::Json("always"));
  bx::Config read;
  bx::config_from_json(patch, read);
  check(read.performance_when == bx::PerformanceWhen::always, "the mode is read");
  check(read.performance_games == std::vector<std::string>({"cs2", "valorant-win64-shipping"}),
        "the games list is lowercase, without .exe, trimmed, without blanks or duplicates");
  bx::Json back = bx::config_to_json(read);
  check(back["performance_when"].as_string() == "always" && back["performance_games"].items().size() == 2 &&
            !back.has("performance_mode") && !back.has("performance_fps"),
        "the fields round-trip, with no old switch or separate frame rate any more");
  bx::Json profile = bx::Json::object();
  profile.set("performance_when", bx::Json("games"));
  profile.set("performance_games", games);
  profile.set("capture_fps", bx::Json(90));
  const bx::Json filtered = bx::profile_patch(profile);
  check(!filtered.has("performance_when") && !filtered.has("performance_games") && filtered.has("capture_fps"),
        "a profile never carries Performance mode");
}

void test_performance_modes() {
  check(std::string(bx::performance_when_name(bx::PerformanceWhen::off)) == "off", "off name");
  check(std::string(bx::performance_when_name(bx::PerformanceWhen::games)) == "games", "games name");
  check(std::string(bx::performance_when_name(bx::PerformanceWhen::always)) == "always", "always name");
  check(bx::parse_performance_when("off") == bx::PerformanceWhen::off, "parse off");
  check(bx::parse_performance_when("games") == bx::PerformanceWhen::games, "parse games");
  check(bx::parse_performance_when("always") == bx::PerformanceWhen::always, "parse always");
  check(bx::parse_performance_when("bogus") == bx::PerformanceWhen::off, "unknown mode is off");

  std::string error;
  bx::Json old_true;
  check(bx::Json::parse(R"json({"performance_mode":true,"performance_games":["CS2.EXE"," cs2 "]})json", old_true, error),
        "old config parses");
  bx::Config migrated;
  bx::config_from_json(old_true, migrated);
  check(migrated.performance_when == bx::PerformanceWhen::always, "old true migrates to always");
  check(migrated.performance_games.size() == 1 && migrated.performance_games[0] == "cs2", "games normalise and dedupe");

  bx::Json old_false_games;
  check(bx::Json::parse(R"json({"performance_mode":false,"performance_games":["valorant.exe"]})json", old_false_games, error),
        "old false with games parses");
  migrated = bx::Config();
  bx::config_from_json(old_false_games, migrated);
  check(migrated.performance_when == bx::PerformanceWhen::games, "old false with games migrates to games");

  bx::Json old_empty;
  check(bx::Json::parse(R"json({"performance_mode":false})json", old_empty, error), "old empty config parses");
  migrated = bx::Config();
  bx::config_from_json(old_empty, migrated);
  check(migrated.performance_when == bx::PerformanceWhen::off, "old empty config migrates to off");

  bx::Json explicit_off;
  check(bx::Json::parse(R"json({"performance_when":"off","performance_games":["cs2"]})json", explicit_off, error),
        "explicit off with games parses");
  migrated = bx::Config();
  bx::config_from_json(explicit_off, migrated);
  check(migrated.performance_when == bx::PerformanceWhen::off && migrated.performance_games.size() == 1, "OFF keeps the list");

  bx::Json back = bx::config_to_json(migrated);
  check(back["performance_when"].as_string() == "off", "serializes performance_when");
  check(!back.has("performance_mode"), "does not write performance_mode");

  check(bx::performance_decide(bx::PerformanceWhen::always, false, false, false).active, "always is active");
  check(bx::performance_decide(bx::PerformanceWhen::always, false, false, true).active, "always stays on when game closes");
  check(!bx::performance_decide(bx::PerformanceWhen::always, false, false, true).deferred, "always never defers");
  check(!bx::performance_decide(bx::PerformanceWhen::off, true, false, true).active, "off releases a stale layer");
  check(bx::performance_decide(bx::PerformanceWhen::games, true, false, false).active, "listed game turns games on");
  check(!bx::performance_decide(bx::PerformanceWhen::games, false, false, true).active, "game close turns games off");
  bx::PerformanceDecision deferred = bx::performance_decide(bx::PerformanceWhen::games, false, true, true);
  check(deferred.active && deferred.deferred, "recording defers release");
  bx::PerformanceDecision appearing = bx::performance_decide(bx::PerformanceWhen::games, true, true, false);
  check(!appearing.active && appearing.deferred, "a game appearing during a recording waits too");
  check(!bx::performance_decide(bx::PerformanceWhen::games, true, false, true).deferred, "switching listed games does not defer");

  bx::Json profile_fields = bx::Json::object();
  bx::Json listed = bx::Json::array();
  listed.push(bx::Json("cs2"));
  profile_fields.set("performance_when", bx::Json("games"));
  profile_fields.set("performance_games", listed);
  profile_fields.set("capture_fps", bx::Json(60));
  bx::Json filtered = bx::profile_patch(profile_fields);
  check(!filtered.has("performance_when") && !filtered.has("performance_games") && filtered.has("capture_fps"),
        "profile excludes performance fields");
}

void test_process_tuning() {
  using namespace buffrx;
  // BUFFRX_TUNE names the candidate optimizations a measurement turns on one at a time (docs/measurement.md).
  const TuningSet none = parse_tuning("");
  check(none.gpu_priority == 0 && !none.efficiency_cores && !none.below_normal && !none.background_io, "nothing asked, nothing set");
  const TuningSet all = parse_tuning("gpu:-7,eco,below,io");
  check(all.gpu_priority == -7 && all.efficiency_cores && all.below_normal && all.background_io, "every word sets its candidate");
  check(parse_tuning("gpu:5").gpu_priority == 0, "a GPU priority above zero needs admin and is not offered: it clamps to 0");
  check(parse_tuning("gpu:-99").gpu_priority == -7, "and one below the lowest clamps to -7");
  check(parse_tuning("gpu:-3").gpu_priority == -3, "a value in range is kept");
  check(parse_tuning("gpu:abc").gpu_priority == 0, "an unreadable value leaves the default");
  const TuningSet odd = parse_tuning("nonsense, ECO ,,Below");
  check(odd.efficiency_cores && odd.below_normal && !odd.background_io && odd.gpu_priority == 0,
        "unknown words are ignored, case and spaces do not matter");
  check(tuning_describe(all) == "gpu:-7,eco,below,io" && tuning_describe(none) == "none", "a set describes itself in the words that made it");
}

void test_toast_decision() {
  using bx::ToastCardDecision;
  using bx::ToastChime;
  using bx::ToastKind;
  // The regression this guards: a panel that was open but minimized, or behind a fullscreen game, counted as "the
  // user is looking at it", so the desktop card and the chime were skipped and the panel's own card (silent, and
  // out of sight) was all there was. Only a panel that is really in front suppresses the card.
  check(bx::toast_card_decision(ToastKind::success, false, false, true) == ToastCardDecision::skip_panel &&
            bx::toast_card_decision(ToastKind::info, true, true, true) == ToastCardDecision::skip_panel,
        "a success or info card is left to the panel when the panel is in front");
  check(bx::toast_card_decision(ToastKind::warning, false, false, true) == ToastCardDecision::draw &&
            bx::toast_card_decision(ToastKind::warning, true, false, true) == ToastCardDecision::draw,
        "a warning draws even with the panel in front: the panel has no card for a hotkey failure");
  check(bx::toast_card_decision(ToastKind::success, false, false, false) == ToastCardDecision::draw,
        "no panel in front, no game: the card draws");
  check(bx::toast_card_decision(ToastKind::success, true, false, false) == ToastCardDecision::skip_fullscreen,
        "over a fullscreen game the card is skipped by default");
  check(bx::toast_card_decision(ToastKind::success, true, true, false) == ToastCardDecision::draw,
        "and drawn when the person asked for cards over games");
  check(bx::toast_chime_plays(ToastCardDecision::skip_panel, ToastChime::saved, true, false) &&
            !bx::toast_chime_plays(ToastCardDecision::skip_panel, ToastChime::saved, false, false),
        "with the panel in front the chime follows the sound setting and nothing is forced");
  check(bx::toast_chime_plays(ToastCardDecision::skip_fullscreen, ToastChime::saved, false, false),
        "a card skipped over a game still chimes: it is the only sign the clip was saved");
  check(!bx::toast_chime_plays(ToastCardDecision::draw, ToastChime::saved, false, false) &&
            bx::toast_chime_plays(ToastCardDecision::draw, ToastChime::saved, true, false),
        "a drawn card chimes only when the sound is on");
  check(!bx::toast_chime_plays(ToastCardDecision::draw, ToastChime::none, true, false) &&
            !bx::toast_chime_plays(ToastCardDecision::skip_fullscreen, ToastChime::none, true, false) &&
            !bx::toast_chime_plays(ToastCardDecision::skip_panel, ToastChime::none, true, false),
        "a notice that asked for no sound makes none, whatever was decided");
  // A notice that lands while the buffer, a recording or Timeline is capturing is heard in the clip: the card
  // still says what happened, the sound is held until nothing is recording.
  for (const ToastChime chime : {ToastChime::saved, ToastChime::started, ToastChime::stopped, ToastChime::alert}) {
    check(!bx::toast_chime_plays(ToastCardDecision::draw, chime, true, true) &&
              !bx::toast_chime_plays(ToastCardDecision::skip_fullscreen, chime, true, true),
          "every chime is held while capture is active, drawn or skipped");
  }
  check(bx::toast_card_decision(ToastKind::success, false, false, false) == ToastCardDecision::draw,
        "the card still draws while capture is active: only the chime is held");
}

// The fourteen themes are one table the interface, the notification card and the editor page all paint from.
// Contrast is checked here, not by eye: every text pair a view uses must read on every theme.
double luminance(uint32_t rgb) {
  auto channel = [](uint32_t v) {
    const double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel((rgb >> 16) & 0xff) + 0.7152 * channel((rgb >> 8) & 0xff) + 0.0722 * channel(rgb & 0xff);
}
double contrast(uint32_t a, uint32_t b) {
  const double x = luminance(a), y = luminance(b);
  return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

void test_upper_codepoint() {
  check(bx::ui::upper_codepoint('a') == 'A' && bx::ui::upper_codepoint('z') == 'Z', "ASCII letters draw as capitals");
  check(bx::ui::upper_codepoint('A') == 'A' && bx::ui::upper_codepoint('7') == '7' && bx::ui::upper_codepoint('+') == '+',
        "capitals, digits and punctuation are left alone");
  check(bx::ui::upper_codepoint(0xE9) == 0xC9 && bx::ui::upper_codepoint(0xFF) == 0x178, "Latin-1 accents have capitals");
  check(bx::ui::upper_codepoint(0xF7) == 0xF7 && bx::ui::upper_codepoint(0xDF) == 0xDF, "the division sign and the sharp s stay");
  check(bx::ui::upper_codepoint(0x3B1) == 0x391 && bx::ui::upper_codepoint(0x3C2) == 0x3A3, "Greek, the final sigma included");
  check(bx::ui::upper_codepoint(0x430) == 0x410 && bx::ui::upper_codepoint(0x451) == 0x401, "Cyrillic, the yo included");
  check(bx::ui::upper_codepoint(0x3042) == 0x3042, "scripts without case are untouched");
}

void test_card_title() {
  check(bx::ui::card_title("Replay 2026-10-20 10-00-00") == "2026-10-20 10-00-00", "a replay's name starts at its date");
  check(bx::ui::card_title("Screenshot 2026-10-24 01-18-42") == "2026-10-24 01-18-42", "so does a screenshot's");
  check(bx::ui::card_title("Replay of the final") == "Replay of the final", "a name the user chose is kept whole");
  check(bx::ui::card_title("Replay ") == "Replay ", "a bare prefix is not emptied");
}

void test_lower_ascii() {
  check(bx::ui::lower_ascii("Save Clip ALT") == "save clip alt", "pane titles and key legends are set in lower case");
  check(bx::ui::lower_ascii("Ctrl+Alt+F9") == "ctrl+alt+f9", "a chord keeps its punctuation and digits");
  check(bx::ui::lower_ascii("\xC3\x89lan") == "\xC3\x89lan", "bytes beyond ASCII are left as they are");
}

void test_themes() {
  const std::vector<bx::ThemeColors>& all = bx::themes();
  check(all.size() == 14, "fourteen themes");
  check(!all.empty() && std::string(all.front().id) == "clutch", "Clutch is the main theme and comes first");
  check(std::string(bx::default_theme_id()) == "clutch" && bx::Config().theme == "clutch",
        "a new config paints the main theme");
  std::set<std::string> ids;
  for (const bx::ThemeColors& t : all) ids.insert(t.id);
  check(ids.size() == all.size(), "every theme has its own id");
  check(bx::theme_colors("overtime").acc == 0x4fd6be, "a theme is found by its id");
  check(std::string(bx::theme_colors("auto").id) == "clutch" && std::string(bx::theme_colors("green").id) == "clutch" &&
            std::string(bx::theme_colors("").id) == "clutch" && std::string(bx::theme_colors("nonsense").id) == "clutch",
        "a cockpit from an older config, an empty or an unknown id paints the main theme");
  check(bx::is_theme_id("arcade") && !bx::is_theme_id("blue"), "only the fourteen ids are themes");
  bool text = true, secondary = true, on_accent = true, marks = true, update_badge = true;
  for (const bx::ThemeColors& t : all) {
    text = text && contrast(t.fg, t.tile) >= 7.0 && contrast(t.fg, t.ground) >= 7.0;
    secondary = secondary && contrast(t.muted, t.tile) >= 4.5 && contrast(t.muted, t.ground) >= 4.5 &&
                contrast(t.muted, t.raised) >= 4.5;
    on_accent = on_accent && contrast(t.on, t.acc) >= 4.5;
    update_badge = update_badge && contrast(t.acc, t.ground) >= 3.0 && contrast(t.on, t.acc) >= 4.5;
    marks = marks && contrast(t.acc, t.tile) >= 3.0 && contrast(t.rec, t.tile) >= 3.0 && contrast(t.warn, t.tile) >= 3.0 &&
            contrast(t.danger, t.tile) >= 3.0 && contrast(t.ok, t.tile) >= 3.0;
    if (!(contrast(t.muted, t.raised) >= 4.5)) std::printf("  %s: muted on raised %.2f\n", t.id, contrast(t.muted, t.raised));
  }
  check(text, "body text reads at 7:1 on every theme");
  check(secondary, "secondary text reads at 4.5:1 on panes, raised controls and the ground");
  check(on_accent, "text on the accent reads at 4.5:1");
  check(update_badge, "the update badge and its icon stand out on every theme");
  check(marks, "the accent, recording, warning, problem and ok marks stand out at 3:1");
#ifdef BUFFRX_SOURCE_DIR
  // The editor page paints from ui/src/themes.css: the same ids and colours, or the two drift apart.
  std::string css;
  if (FILE* file = std::fopen((source_root() + "/ui/src/themes.css").c_str(), "rb")) {
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, file)) > 0) css.append(chunk, n);
    std::fclose(file);
  }
  bool synced = !css.empty();
  for (const bx::ThemeColors& t : all) {
    const size_t at = css.find(std::string("[data-theme='") + t.id + "']");
    // Every role, not a sample of them: a pane or a text colour changed in one file only is drift too.
    const std::pair<const char*, uint32_t> roles[12] = {{"ground", t.ground}, {"tile", t.tile},   {"raised", t.raised},
                                                        {"line", t.line},     {"fg", t.fg},       {"muted", t.muted},
                                                        {"acc", t.acc},       {"on", t.on},       {"ok", t.ok},
                                                        {"warn", t.warn},     {"danger", t.danger}, {"rec", t.rec}};
    const size_t end = at == std::string::npos ? at : css.find('}', at);
    synced = synced && at != std::string::npos;
    for (const auto& role : roles) {
      char wanted[40];
      std::snprintf(wanted, sizeof wanted, "--bx-%s: #%06x;", role.first, role.second);
      synced = synced && css.find(wanted, at) < end;
    }
  }
  check(synced, "the editor page's themes.css carries every theme with the same colours");
#endif
}

void test_toast() {
  check(bx::toast_colors("clutch").accent == 0xff6b2c, "the card paints the theme's accent");
  check(bx::toast_colors("respawn").accent == 0xf5a3c7 && bx::toast_colors("respawn").surface == 0x19151f,
        "another theme paints its own accent and pane");
  check(bx::toast_colors("green").accent == bx::toast_colors("clutch").accent,
        "a cockpit from an older config paints the main theme");
  const std::vector<uint8_t>& wav = bx::toast_chime_wav(bx::ToastChime::saved);
  check(wav.size() > 44 && std::memcmp(wav.data(), "RIFF", 4) == 0 && std::memcmp(wav.data() + 8, "WAVE", 4) == 0,
        "the chime is a RIFF WAVE");
  const uint32_t riff_size = wav[4] | (wav[5] << 8) | (wav[6] << 16) | (static_cast<uint32_t>(wav[7]) << 24);
  check(riff_size == wav.size() - 8, "its RIFF size matches the bytes");
  check(wav.size() > 44 + 44100 / 5 && wav.size() < 44 + 44100 * 2, "it lasts a fraction of a second");
  // The fullscreen rule: a saved-clip card must not cost a fullscreen game its frame pacing, but a
  // warning still has to be seen.
  check(!bx::toast_visible_over_fullscreen(bx::ToastKind::success), "a saved clip does not draw over a game");
  check(!bx::toast_visible_over_fullscreen(bx::ToastKind::info), "an informational card does not either");
  check(bx::toast_visible_over_fullscreen(bx::ToastKind::warning), "a warning still draws");

  // The regression this guards: over a fullscreen game a skipped card still played the chime - for every notice, even
  // ones that never asked for a sound and with the sound setting off - so background notices were unexplained beeps.
  check(!bx::toast_plays_sound(bx::ToastChime::none), "a notice that asked for no sound makes none, skipped or not");
  check(bx::toast_plays_sound(bx::ToastChime::saved) && bx::toast_plays_sound(bx::ToastChime::started) &&
            bx::toast_plays_sound(bx::ToastChime::stopped) && bx::toast_plays_sound(bx::ToastChime::alert),
        "the four events that ask for one do");
  // Whether the sound plays: only a notice that asked for one, and then when the person has the sound on - or when
  // its card was skipped over a fullscreen game, where the chime is the only way to know a clip was saved.
  check(!bx::toast_should_play(bx::ToastChime::none, true, true, false), "a background notice never beeps, whatever the settings");
  check(bx::toast_should_play(bx::ToastChime::saved, true, false, false), "sound on: a saved clip chimes");
  check(!bx::toast_should_play(bx::ToastChime::saved, false, false, false), "sound off and the card drawn: silent");
  check(bx::toast_should_play(bx::ToastChime::saved, false, true, false),
        "sound off but the card was skipped over a game: the clip still says it was saved");
  for (const bx::ToastChime chime :
       {bx::ToastChime::saved, bx::ToastChime::started, bx::ToastChime::stopped, bx::ToastChime::alert}) {
    check(!bx::toast_should_play(chime, true, false, true) && !bx::toast_should_play(chime, true, true, true),
        "capturing holds the chime out, unless Sound while recording is on");
  check(bx::toast_should_play(chime, true, false, true, true), "Sound while recording: the chime plays and lands in the clip");
  }
  check(bx::toast_card_visible(bx::ToastKind::success, false, false), "off a game every card draws");
  check(!bx::toast_card_visible(bx::ToastKind::success, true, false) && !bx::toast_card_visible(bx::ToastKind::info, true, false),
        "over a game the success and info cards are skipped by default");
  check(bx::toast_card_visible(bx::ToastKind::warning, true, false), "a warning still draws over a game");
  check(bx::toast_card_visible(bx::ToastKind::success, true, true), "and the person can ask for every card over a game");
  // One sound for everything meant nothing: each event is told apart by ear.
  const auto& saved = bx::toast_chime_wav(bx::ToastChime::saved);
  const auto& started = bx::toast_chime_wav(bx::ToastChime::started);
  const auto& stopped = bx::toast_chime_wav(bx::ToastChime::stopped);
  const auto& alert = bx::toast_chime_wav(bx::ToastChime::alert);
  check(saved != started && saved != stopped && saved != alert && started != stopped && started != alert && stopped != alert,
        "saved, recording started, recording stopped and an alert each sound different");
  for (const auto* chime : {&saved, &started, &stopped, &alert}) {
    check(chime->size() > 44 && std::memcmp(chime->data(), "RIFF", 4) == 0 && chime->size() < 44 + 44100 * 2, "every chime is a short complete WAV");
  }
  check(bx::Config{}.notify_over_fullscreen, "cards over fullscreen games are on by default: a notice must arrive"); 
}

// Game detection's fullscreen rule: 0.9 of the monitor on both axes, and never an ordinary maximized
// window (a browser filling a screen whose taskbar auto-hides is not a game).
void test_game_fullscreen_rule() {
  check(bx::game_fullscreen_rule(1.0, 1.0, false), "a borderless window covering the monitor is fullscreen");
  check(bx::game_fullscreen_rule(0.92, 0.95, false), "a window a little inside the monitor still counts");
  check(!bx::game_fullscreen_rule(0.75, 0.75, false), "a 1080p window on a 1440p monitor does not");
  check(!bx::game_fullscreen_rule(1.0, 1.0, true), "a maximized framed window is not a game");
  check(!bx::game_fullscreen_rule(1.0, 0.5, false), "both axes have to fill");
  check(bx::game_fullscreen_rule(0.5, 0.5, false, true), "foreground exclusive Direct3D can use a lower resolution");
  check(!bx::game_fullscreen_rule(0.4, 0.5, false, true), "exclusive state does not promote an ordinary small window");
  check(!bx::game_fullscreen_rule(0.5, 0.5, false, false), "a smaller window without exclusive Direct3D is not fullscreen");
  check(!bx::game_fullscreen_rule(0.5, 0.5, true, true), "exclusive state does not promote a maximized framed window");
  check(!bx::game_fullscreen_rule(0.0, 0.5, false, true), "exclusive state does not promote an empty window");
}

// One Record press, one decision (recording_watch.h). The regression this guards: a paused Timeline
// session made the key a dead end ("Timeline is recording; save a clip instead"), so a person outside
// the game could neither record nor keep a clip.
void test_record_action() {
  using buffrx::RecordAction;
  check(buffrx::record_action(true) == RecordAction::ManualStop, "a manual recording: the key stops it");
  check(buffrx::record_action(false) == RecordAction::ManualStart, "nothing running: the key records");
  check(buffrx::record_action(false) == RecordAction::ManualStart,
        "Timeline writing and a clip selection do not change the Record key: it starts a recording");
  // The dot marks what the person asked to keep, never Timeline writing by itself.
  check(!bx::recording_dot_visible(false), "no recording: no dot");
  check(bx::recording_dot_visible(true), "a manual recording is marked");
}

// A record click that finds no frame waits instead of failing. The regression this guards: an idle
// engine releases the capture source, and the first record click then failed while the buffer's start
// already waited for the same first frame.
void test_recording_watch() {
  using buffrx::RecordingAction;
  check(buffrx::recording_action(false, false, 0) == RecordingAction::none, "no request: nothing to do");
  check(buffrx::recording_action(false, true, 0) == RecordingAction::none, "no request: a ready source starts nothing");
  check(buffrx::recording_action(true, false, 0) == RecordingAction::wait, "a request waits for the first frame");
  check(buffrx::recording_action(true, false, buffrx::kRecordingStartTimeoutMs - 1) == RecordingAction::wait,
        "it waits right up to the limit");
  check(buffrx::recording_action(true, false, buffrx::kRecordingStartTimeoutMs) == RecordingAction::give_up,
        "no frame by the limit: give up and say so");
  check(buffrx::recording_action(true, true, 0) == RecordingAction::start, "a frame arrived: start");
  check(buffrx::recording_action(true, true, buffrx::kRecordingStartTimeoutMs) == RecordingAction::start,
        "a late frame still starts rather than being thrown away");
  check(buffrx::kRecordingStartTimeoutMs > 6000, "the wait outlives the game hook's six-second fallback");

  // The regression this guards: with a heavy encoder the output only reports its stop once the encoder's
  // backlog drains - 16 s measured, over 30 s in play. For all that time the panel said "stopped", a new
  // recording was refused as "already running", and the late stop was then reported as the recording
  // stopping by itself. A stop the person asked for waits a few seconds, not half a minute.
  check(buffrx::recording_stop_grace_ms() < 10000, "a stop is forced within ten seconds");
  check(buffrx::recording_stop_unexpected(true, false, false), "an open output that stops unasked stopped by itself");
  check(!buffrx::recording_stop_unexpected(true, true, false), "a stop the person asked for is not a surprise");
  check(!buffrx::recording_stop_unexpected(true, false, true), "nor a stop that had to be forced");
  check(!buffrx::recording_stop_unexpected(false, false, false), "a late signal from an output already let go is nothing");
  check(!buffrx::recording_start_blocked(false, false), "no output: a click records");
  check(buffrx::recording_start_blocked(true, true), "an active output refuses the next start");
  check(!buffrx::recording_start_blocked(true, false), "a finished output only owes its release, not a refusal");
  check(!buffrx::recording_busy_flag(false, true, false), "a flag with no output behind it is not a recording");
  check(buffrx::recording_busy_flag(true, true, false), "an output and its flag are a recording");
  check(buffrx::recording_busy_flag(false, false, true), "a start waiting for its first frame is a recording");
  // A rebuild brings back only what the person started. Timeline's segment shares the output and the active flag; resuming
  // it as a manual recording is how the Performance switch started a recording nobody asked for.
  check(buffrx::manual_recording_flag(true, true, false, false), "a manual recording runs: a rebuild resumes it");
  check(!buffrx::manual_recording_flag(true, true, false, true), "Timeline's segment is not a manual recording");
  check(buffrx::manual_recording_flag(false, false, true, false), "a recording still waiting for its first frame is resumed");
  check(buffrx::manual_recording_flag(false, false, true, true), "a waiting request is the person's even beside a Timeline session");
  check(!buffrx::manual_recording_flag(false, true, false, false), "a flag with no output behind it is not a recording");
  check(!buffrx::manual_recording_flag(true, false, false, false), "an output that is not active is not a recording");

  // A recording that stops by itself is resumed, a few times, then reported.
  buffrx::ResumeBudget budget;
  check(buffrx::recording_resume_allowed(budget, 1000), "the first unexpected stop is resumed");
  check(buffrx::recording_resume_allowed(budget, 2000), "and the second");
  check(buffrx::recording_resume_allowed(budget, 3000), "and the third");
  check(!buffrx::recording_resume_allowed(budget, 4000), "the fourth inside ten minutes is not: something is wrong");
  check(buffrx::recording_resume_allowed(budget, 1000 + buffrx::kRecordingResumeWindowMs), "the budget comes back after ten minutes");
}

// Encoder modes: Fast, Balanced and Quality are the speed tiers themselves; Auto follows the quality level;
// Custom is the person's own rows on top of Auto. Any row the person edits makes the mode Custom.
void test_encoder_modes() {
  std::printf("encoder modes\n");
  check(std::string(bx::encoder_mode_tier("fast", "highest")) == "performance", "Fast is the performance tier whatever the level");
  check(std::string(bx::encoder_mode_tier("balanced", "low")) == "balanced", "Balanced is the balanced tier whatever the level");
  check(std::string(bx::encoder_mode_tier("quality", "low")) == "quality", "Quality is the quality tier whatever the level");
  check(std::string(bx::encoder_mode_tier("auto", "low")) == "performance" && std::string(bx::encoder_mode_tier("auto", "highest")) == "quality" &&
            std::string(bx::encoder_mode_tier("auto", "high")) == "balanced",
        "Auto follows the quality level");
  check(std::string(bx::encoder_mode_tier("custom", "highest")) == "quality", "Custom starts from what Auto would have chosen");
  check(bx::encoder_mode_is_known("auto") && bx::encoder_mode_is_known("custom") && !bx::encoder_mode_is_known("ultra"), "only the five modes exist");

  bx::Config pending;
  pending.quality = "highest";
  auto planned = bx::configured_encoder_settings(bx::EncoderFamily::Nvenc, pending, 0);
  check(bx::encoder_setting_value(planned, "preset").as_string() == "p7", "Highest plans NVENC p7 without a live encoder");
  pending.quality = "low";
  planned = bx::configured_encoder_settings(bx::EncoderFamily::Nvenc, pending, 0);
  check(bx::encoder_setting_value(planned, "preset").as_string() == "p3" &&
            bx::encoder_setting_value(planned, "multipass").as_string() == "disabled" &&
            !bx::encoder_setting_value(planned, "lookahead").as_bool(),
        "Low replaces the old quality settings even without an encoder");
  pending.encoder_mode = "custom";
  pending.encoder_options.set("preset", bx::Json("p6"));
  pending.encoder_options.set("lookahead", bx::Json(true));
  planned = bx::configured_encoder_settings(bx::EncoderFamily::Nvenc, pending, 3);
  check(bx::encoder_setting_value(planned, "preset").as_string() == "p6" &&
            bx::encoder_setting_value(planned, "lookahead").as_bool(),
        "custom settings survive and are not overwritten by the Auto governor");

  // Stored settings: a new file starts Auto; an older file's own speed rows are kept as Custom; keys that never
  // worked (blank values written for integer lists) are dropped.
  check(bx::Config{}.encoder_mode == "auto" && bx::Config{}.display_method == "auto" &&
            bx::Config{}.scale_filter == "bicubic",
        "defaults: Auto, automatic capture method, bicubic scaling");
  bx::Config old_file;
  bx::Json old_json = bx::Json::object();
  bx::Json options = bx::Json::object();
  options.set("preset", bx::Json("p7"));
  options.set("tune", bx::Json("uhq"));
  options.set("split_encode", bx::Json(""));
  old_json.set("encoder_options", options);
  old_json.set("config_version", bx::Json(4));
  bx::config_from_json(old_json, old_file);
  bx::migrate_config(old_file);
  check(old_file.encoder_mode == "custom", "an older file's own speed rows become Custom");
  check(!old_file.encoder_options.has("split_encode") && old_file.encoder_options.has("preset"), "the blank key is dropped, the real ones kept");
  bx::Config clean;
  clean.config_version = 4;
  bx::migrate_config(clean);
  check(clean.encoder_mode == "auto", "an older file with no speed rows stays Auto");
  bx::Json bad = bx::Json::object();
  bad.set("encoder_mode", bx::Json("turbo"));
  bad.set("display_method", bx::Json("magic"));
  bad.set("scale_filter", bx::Json("sinc"));
  bx::Config rejected;
  bx::config_from_json(bad, rejected);
  check(rejected.encoder_mode == "auto" && rejected.display_method == "auto" && rejected.scale_filter == "bicubic", "unknown values are not stored");
  bx::Config chosen;
  chosen.encoder_mode = "quality";
  chosen.display_method = "wgc";
  chosen.scale_filter = "lanczos";
  bx::Config round;
  bx::config_from_json(bx::config_to_json(chosen), round);
  check(round.encoder_mode == "quality" && round.display_method == "wgc" && round.scale_filter == "lanczos",
        "the choices survive a round trip");
  check(bx::scale_type_for_filter("lanczos") == 4 && bx::scale_type_for_filter("bilinear") == 3 && bx::scale_type_for_filter("bicubic") == 2 &&
            bx::scale_type_for_filter("whatever") == 2,
        "the scaling filters map to libobs's own values");
  check(bx::capture_method_for("dxgi") == 1 && bx::capture_method_for("wgc") == 2 && bx::capture_method_for("auto") == 0 && bx::capture_method_for("x") == 0,
        "and the capture methods to win-capture's");
}

// Which graphics adapter libobs is given. Adapter 0 is right on a desktop with one card and wrong on a laptop
// that draws its screen with the integrated GPU and renders games on the discrete one.
void test_gpu_adapter_choice() {
  std::printf("gpu adapter choice\n");
  using bx::GpuAdapterInfo;
  const uint64_t gib = 1024ull * 1024 * 1024;
  check(bx::pick_gpu_adapter({}, "") == 0, "no adapters listed: adapter 0, as before");
  check(bx::pick_gpu_adapter({{0, 16 * gib, false, {"\\\\.\\DISPLAY1"}}}, "") == 0, "one card: that card");
  const std::vector<GpuAdapterInfo> laptop = {{0, 128ull * 1024 * 1024, false, {"\\\\.\\DISPLAY1"}}, {1, 8 * gib, false, {}}};
  check(bx::pick_gpu_adapter(laptop, "\\\\.\\DISPLAY1") == 0, "a display capture uses the adapter that owns the display");
  check(bx::pick_gpu_adapter(laptop, "") == 1, "a game or window capture uses the card with the memory");
  check(bx::pick_gpu_adapter(laptop, "\\\\.\\DISPLAY9") == 1, "a display no adapter owns falls to the biggest card");
  const std::vector<GpuAdapterInfo> with_software = {{0, 0, true, {}}, {1, 4 * gib, false, {}}};
  check(bx::pick_gpu_adapter(with_software, "") == 1, "the software renderer is never chosen over a real card");
  check(bx::pick_gpu_adapter({{0, 0, true, {}}}, "") == 0, "and is the answer only when there is nothing else");
  const std::vector<GpuAdapterInfo> equal = {{0, 8 * gib, false, {}}, {1, 8 * gib, false, {}}};
  check(bx::pick_gpu_adapter(equal, "") == 0, "equal cards: the first, so a working setup does not move");

  // A frame rate the display cannot give an even picture at.
  check(bx::capture_rate_is_even(60, 60) && bx::capture_rate_is_even(60, 120) && bx::capture_rate_is_even(72, 144),
        "a rate that divides the refresh repeats nothing unevenly");
  check(bx::capture_rate_is_even(144, 144) && bx::capture_rate_is_even(164, 165) && bx::capture_rate_is_even(82, 164),
        "and so does the refresh itself, within a hertz of how it is reported");
  check(!bx::capture_rate_is_even(120, 144) && !bx::capture_rate_is_even(120, 165) && !bx::capture_rate_is_even(100, 144),
        "120 on a 144 or 165 Hz display is the uneven case");
  check(bx::capture_rate_is_even(120, 0), "an unknown refresh is not a complaint");
  check(bx::capture_rate_is_even(240, 165), "a rate above the refresh has nothing to be uneven against");
}

// The governor: when the encoder cannot keep up, BuffrX lowers its own cost a step at a time instead of
// recording a clip with a third of its frames missing. The rules here are the whole decision.
void test_encoder_governor() {
  std::printf("encoder governor\n");
  using buffrx::governor_should_step;
  const uint32_t cool = buffrx::kGovernorCooldownMs;
  check(!governor_should_step(0, false, 100000, 0), "an encoder that keeps up is left alone");
  check(governor_should_step(0, true, 100000, 0), "behind, never stepped: step");
  check(!governor_should_step(1, true, 100000 + cool - 1, 100000), "a step's encoder gets its cooldown to settle");
  check(governor_should_step(1, true, 100000 + cool, 100000), "behind again after the cooldown: the next step");
  check(governor_should_step(buffrx::kMaxGovernorStep, true, 900000, 100000), "the ladder continues past the encoder rungs");
  check(!governor_should_step(buffrx::kMaxReliefStep, true, 900000, 100000), "the relief ladder's last rung is the floor");
  check(!governor_should_step(0, true, 900000, 0, true), "a manual recording in progress holds the ladder");
  check(governor_should_step(0, true, 900000, 0, false), "the ladder resumes once the recording has ended");
  check(buffrx::relief_capture_fps(120, 3) == 120, "encoder rungs leave the frame rate alone");
  check(buffrx::relief_capture_fps(120, 4) == 60 && buffrx::relief_capture_fps(60, 4) == 60, "the fourth rung is 60 fps");
  check(buffrx::relief_capture_fps(60, 5) == 30 && buffrx::relief_capture_fps(120, 5) == 30, "the fifth is 30 fps");
  check(governor_should_step(2, true, 0u - 5u, 0u - 5u - cool), "the tick clock wrapping does not stall the governor");

  // What each step asks of the encoder, per family. Step 0 asks for nothing.
  using bx::EncoderFamily;
  check(bx::governor_overrides(EncoderFamily::Nvenc, 0).empty(), "step 0 changes nothing");
  const auto one = bx::governor_overrides(EncoderFamily::Nvenc, 1);
  check(bx::encoder_setting_value(one, "preset").as_string() == "p5" && bx::encoder_setting_value(one, "multipass").as_string() == "qres" &&
            !bx::encoder_setting_value(one, "lookahead").as_bool(true),
        "NVENC step 1 is p5 with quarter-resolution multipass and no look-ahead");
  check(bx::encoder_setting_value(one, "tune").as_string() == "hq", "and the high-quality tune, not UHQ");
  const auto two = bx::governor_overrides(EncoderFamily::Nvenc, 2);
  check(bx::encoder_setting_value(two, "preset").as_string() == "p4" && bx::encoder_setting_value(two, "multipass").as_string() == "disabled" &&
            !bx::encoder_setting_value(two, "lookahead").as_bool(true),
        "NVENC step 2 is p4 with no multipass and no look-ahead");
  const auto three = bx::governor_overrides(EncoderFamily::Nvenc, 3);
  check(bx::encoder_setting_value(three, "preset").as_string() == "p2" && bx::encoder_setting_value(three, "bf").as_int(9) == 0,
        "NVENC step 3 is p2 without B-frames");
  check(bx::encoder_setting_value(bx::governor_overrides(EncoderFamily::Nvenc, 9), "preset").as_string() == "p2", "past the last step is the last step");
  check(bx::encoder_setting_value(bx::governor_overrides(EncoderFamily::X264, 1), "preset").as_string() == "veryfast" &&
            bx::encoder_setting_value(bx::governor_overrides(EncoderFamily::X264, 3), "preset").as_string() == "ultrafast",
        "x264 walks veryfast to ultrafast");
  check(bx::encoder_setting_value(bx::governor_overrides(EncoderFamily::Qsv, 3), "target_usage").as_string() == "TU7", "QSV ends at TU7");
  // AMF's speed knob is named "preset" (texture-amf.cpp). It was written as "quality", which no AMF encoder has,
  // so the setting was refused and AMD cards never got their tier.
  check(bx::encoder_setting_value(bx::governor_overrides(EncoderFamily::Amf, 1), "preset").as_string() == "balanced" &&
            bx::encoder_setting_value(bx::governor_overrides(EncoderFamily::Amf, 2), "preset").as_string() == "speed",
        "AMF goes to balanced, then to its speed preset");
  check(bx::encoder_setting_value(bx::governor_overrides(EncoderFamily::Amf, 2), "quality").is_null(), "and never writes the key AMF does not have");
  check(bx::governor_overrides(EncoderFamily::Other, 2).empty(), "an encoder BuffrX does not know is not guessed at");

  // The overrides win over what the user chose, and leave the rest of the choice alone.
  std::vector<bx::EncoderSetting> chosen = {{"preset", bx::Json("p7")}, {"tune", bx::Json("uhq")}, {"adaptive_quantization", bx::Json(true)}};
  bx::apply_encoder_overrides(chosen, bx::governor_overrides(EncoderFamily::Nvenc, 1));
  check(bx::encoder_setting_value(chosen, "preset").as_string() == "p5" && bx::encoder_setting_value(chosen, "tune").as_string() == "hq",
        "the step replaces the slow settings");
  check(bx::encoder_setting_value(chosen, "adaptive_quantization").as_bool(false), "and keeps the ones it has no opinion on");
  bx::Config remembered;
  remembered.encoder_speed_step = 2;
  bx::Config reread;
  bx::config_from_json(bx::config_to_json(remembered), reread);
  check(reread.encoder_speed_step == 2, "the step BuffrX settled on is remembered across runs");
  bx::Json silly = bx::Json::object();
  silly.set("encoder_speed_step", bx::Json(77));
  bx::config_from_json(silly, reread);
  check(reread.encoder_speed_step == buffrx::kMaxReliefStep, "and kept inside the steps that exist");
  remembered.encoder_speed_step = 4;
  bx::config_from_json(bx::config_to_json(remembered), reread);
  check(reread.encoder_speed_step == 4, "a machine that settled at 60 fps still reads back as 60 fps");
  remembered.encoder_speed_step = 5;
  bx::config_from_json(bx::config_to_json(remembered), reread);
  check(reread.encoder_speed_step == 5, "and a machine at 30 fps as 30 fps");
}

// capacity.json outlived the old reset: "Start from full speed again" zeroed the config step, the
// next launch read the remembered one and started stepped anyway.
void test_capacity_memory() {
  std::printf("capacity memory\n");
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::string path = temp_directory() + "\\capacity.json";
  DeleteFileA(path.c_str());
  const std::string h264 = bx::capacity_key("h264", "nvenc", 2560, 1440, 165, "p7");
  const std::string av1 = bx::capacity_key("av1", "nvenc", 2560, 1440, 165, "p7");
  check(h264 == "h264|nvenc|2560x1440|165|p7", "the key names every input that changes what the encoder must do");
  check(bx::capacity_key("h264", "nvenc", 0, 0, 60, "p5") == "h264|nvenc|1920x1080|60|p5",
        "a config with no size is the canvas BuffrX starts on");
  check(bx::capacity_read_step(path, h264) == 0, "no file means no remembered step");

  bx::capacity_write_step(path, h264, 4);
  bx::capacity_write_step(path, av1, 2);
  check(bx::capacity_read_step(path, h264) == 4, "the step is read back");
  check(bx::capacity_read_step(path, av1) == 2, "and one codec's step does not become another's");

  bx::capacity_forget_key(path, h264);
  check(bx::capacity_read_step(path, h264) == 0, "forgetting a key clears it, so a reset sticks");
  check(bx::capacity_read_step(path, av1) == 2, "and leaves every other entry alone");

  // A file torn by a crash or edited by hand is no memory, not a wrong step.
  const std::string broken = temp_directory() + "\\capacity-broken.json";
  FILE* f = std::fopen(broken.c_str(), "wb");
  if (f) {
    std::fputs("{ not json", f);
    std::fclose(f);
  }
  check(bx::capacity_read_step(broken, h264) == 0, "an unreadable file is no remembered step");
  bx::capacity_write_step(broken, h264, 5);
  check(bx::capacity_read_step(broken, h264) == 5, "and writing after it repairs the file");
  DeleteFileA(broken.c_str());
}

// Whether the encoder keeps up with the frames the video thread renders. The numbers are the ones measured
// on the development machine: AV1 p7 uhq at 120 fps encoded 3423 of 4879 frames (70%), p5 4310 of 4355.
void test_encoder_watch() {
  std::printf("encoder watch\n");
  buffrx::EncoderWatch watch;
  // Steady and keeping up: no verdict of "behind" however long it runs.
  bool changed = false;
  for (uint32_t t = 0; t <= 30000; t += 250) {
    changed |= buffrx::encoder_watch_feed(watch, t, static_cast<uint64_t>(t) * 120 / 1000, static_cast<uint64_t>(t) * 119 / 1000, true);
  }
  check(!watch.behind && !changed, "an encoder that keeps up is never reported behind");
  check(watch.ratio > 0.97, "and its ratio is near one");

  // Falls behind: 70% of the frames, sustained past the window.
  buffrx::EncoderWatch slow;
  int flips = 0;
  for (uint32_t t = 0; t <= 30000; t += 250) {
    if (buffrx::encoder_watch_feed(slow, t, static_cast<uint64_t>(t) * 120 / 1000, static_cast<uint64_t>(t) * 84 / 1000, true)) ++flips;
  }
  check(slow.behind && flips == 1, "an encoder at 70% is reported behind, once");
  check(slow.ratio > 0.65 && slow.ratio < 0.75, "with the ratio it managed");

  // It must be sustained: a stall shorter than the window is not a verdict.
  buffrx::EncoderWatch blip;
  uint64_t video = 0, encoded = 0;
  for (uint32_t t = 0; t <= 20000; t += 250) {
    video += 30;
    encoded += (t >= 2000 && t < 3000) ? 5 : 30;
    buffrx::encoder_watch_feed(blip, t, video, encoded, true);
  }
  check(!blip.behind, "a one second hitch is not an encoder that cannot keep up");

  // Recovery clears it, and an idle encoder is never behind.
  for (uint32_t t = 30250; t <= 50000; t += 250) {
    buffrx::encoder_watch_feed(slow, t, static_cast<uint64_t>(t) * 120 / 1000, 3500 + static_cast<uint64_t>(t - 30000) * 120 / 1000, true);
  }
  check(!slow.behind, "an encoder that catches up is cleared");
  buffrx::encoder_watch_feed(slow, 51000, 6000, 10, true);
  buffrx::encoder_watch_feed(slow, 52000, 6100, 10, false);
  check(!slow.behind && slow.ratio == 1.0, "an encoder that is not running is not behind");

  // Counters that move backwards (a rebuilt pipeline) start over instead of reading as a huge deficit.
  buffrx::EncoderWatch rebuilt;
  buffrx::encoder_watch_feed(rebuilt, 0, 0, 0, true);
  buffrx::encoder_watch_feed(rebuilt, 4000, 480, 470, true);
  buffrx::encoder_watch_feed(rebuilt, 8000, 10, 10, true);
  buffrx::encoder_watch_feed(rebuilt, 16000, 970, 960, true);
  check(!rebuilt.behind, "a counter reset is not a behind encoder");
}

void test_buffer_watch() {
  using buffrx::WatchdogAction;
  check(buffrx::watchdog_action(true, true, false) == WatchdogAction::none, "a healthy buffer is left alone");
  check(buffrx::watchdog_action(true, false, false) == WatchdogAction::start, "wanted but down: start it");
  check(buffrx::watchdog_action(true, true, true) == WatchdogAction::restart, "frozen frame counter: restart");
  // The three cases that made the bug: with no intent, an output that still reports active is a stop
  // that has not finished unwinding. It must never be read as running, and a frozen counter behind it
  // must never be read as a stall worth restarting.
  check(buffrx::watchdog_action(false, true, false) == WatchdogAction::stop, "stop asked for, still active: stop it");
  check(buffrx::watchdog_action(false, true, true) == WatchdogAction::stop, "a stopping buffer is not a stalled one");
  check(buffrx::watchdog_action(false, false, false) == WatchdogAction::none, "stopped and staying stopped");
  check(buffrx::watchdog_action(false, false, true) == WatchdogAction::none, "a stale stall count cannot restart a stopped buffer");

  check(std::string(buffrx::buffer_state_name(true, true, false)) == "on", "wanted and running reads on");
  check(std::string(buffrx::buffer_state_name(true, false, false)) == "starting", "wanted but no frame yet reads starting");
  check(std::string(buffrx::buffer_state_name(false, true, true)) == "stopping", "a stop unwinding reads stopping");
  check(std::string(buffrx::buffer_state_name(false, false, true)) == "stopping", "until the output reports itself down");
  check(std::string(buffrx::buffer_state_name(false, false, false)) == "off", "not wanted, not running reads off");
  check(std::string(buffrx::buffer_state_name(false, true, false)) == "stopping", "never 'on' once the user stopped it");
}

// Game detection: the denylist must be a denylist (an unknown fullscreen app is a game), and the
// capture switch must need two consecutive polls so a brief alt-tab does not swap sources.
void test_idle_policy() {
  using namespace buffrx;
  // The regression this guards: an engine with nothing to do still ran libobs's video thread at the capture
  // frame rate, and libobs ends every frame by spinning for the last millisecond or two: 0.9% of a core at 10 fps,
  // about 7% at 60 and 26-35% at 144, with the buffer off and nothing captured. Idle now drops the canvas to a few
  // frames a second, whatever the capture rate is.
  check(deep_idle_fps(144) == kDeepIdleFps && deep_idle_fps(60) == kDeepIdleFps, "a high capture rate idles at the idle rate");
  check(deep_idle_fps(3) == 3 && deep_idle_fps(kDeepIdleFps) == kDeepIdleFps, "a rate already that low is left alone");
  check(deep_idle_fps(0) == kDeepIdleFps && deep_idle_fps(-5) == kDeepIdleFps, "and a missing rate is the idle rate");
  check(deep_idle_worthwhile(60) && deep_idle_worthwhile(144) && deep_idle_worthwhile(30),
        "a normal capture rate is worth dropping to sleep");
  check(!deep_idle_worthwhile(kDeepIdleFps) && !deep_idle_worthwhile(10) && !deep_idle_worthwhile(1),
        "a capture rate that is already cheap is not rebuilt for nothing");

  const IdleView idle{.buffer_wanted = false, .recording = false, .sources_released = true, .asleep = false};
  check(deep_idle_due(idle, 144, 100000, 0), "released, nothing wanted and a high rate: due");
  IdleView busy = idle;
  busy.buffer_wanted = true;
  check(!deep_idle_due(busy, 144, 100000, 0), "a buffer wanted: not due");
  busy = idle;
  busy.recording = true;
  check(!deep_idle_due(busy, 144, 100000, 0), "a recording: not due");
  busy = idle;
  busy.sources_released = false;
  check(!deep_idle_due(busy, 144, 100000, 0), "the sources still held (inside the grace): not due");
  busy = idle;
  busy.asleep = true;
  check(!deep_idle_due(busy, 144, 100000, 0), "already asleep: not due");
  check(!deep_idle_due(idle, 8, 100000, 0), "a capture rate that is already cheap: not due");
  // A failed attempt is not repeated every tick.
  check(!deep_idle_due(idle, 144, 100000, 90000), "a failed attempt waits before the next");
  check(deep_idle_due(idle, 144, 100000 + kDeepIdleRetryMs, 100000), "and tries again after the wait");
  check(deep_idle_due(idle, 144, 5, 0), "no failed attempt on record: due at once, even right after the engine started");
}

void test_webview_present() {
  using buffrx::webview2_present;
  // The regression this guards: without the WebView2 runtime the editor process started and died a moment later, the
  // panel had already stepped aside, and nothing on screen said why. This rule is what the engine asks first now.
  check(!webview2_present(-2147024894L, nullptr), "a failed query with no answer: absent");
  check(!webview2_present(0, nullptr), "a query that names no version: absent");
  check(!webview2_present(0, L""), "an empty version string: absent, not unknown");
  check(webview2_present(0, L"120.0.2210.61"), "a version string: present");
  check(webview2_present(1, L"120.0.2210.61"), "any non-negative result that names a version: present");
}

void test_process_path_is() {
  // The regression this guards: the liveness check searched for "\/" (an invalid escape that means "/"), never split
  // a Windows path, compared the whole path to the file name and called every game dead - so an alt-tab out of a
  // game always dropped its capture.
  check(bx::process_path_is("C:\\Games\\Foo\\foo.exe", "foo.exe"), "a backslash path names its executable");
  check(bx::process_path_is("C:\\Games\\Foo\\FOO.EXE", "foo.exe") && bx::process_path_is("C:\\Games\\Foo\\foo.exe", "Foo.exe"),
        "compared without case");
  check(bx::process_path_is("D:/games/foo/foo.exe", "foo.exe"), "a forward-slash path too");
  check(bx::process_path_is("foo.exe", "foo.exe"), "and a bare name");
  check(!bx::process_path_is("C:\\Games\\Foo\\food.exe", "foo.exe"), "a longer name is another program");
  check(!bx::process_path_is("C:\\Games\\foo.exe\\bar.exe", "foo.exe"), "a folder named like it is not the program");
  check(!bx::process_path_is("C:\\Games\\Foo\\foo.exe", ""), "no executable matches nothing");
}

void test_capture_follows_focus() {
  std::printf("capture follows focus\n");
  using bx::CaptureTarget;
  check(bx::capture_follow_target(false, true, true, false) == CaptureTarget::leave, "Timeline off never moves capture");
  check(bx::capture_follow_target(true, false, true, false) == CaptureTarget::leave,
        "a stopped buffer leaves capture where the user put it");
  check(bx::capture_follow_target(true, true, true, true) == CaptureTarget::game, "the captured game in front is the picture");
  check(bx::capture_follow_target(true, true, true, false) == CaptureTarget::display, "alt-tab hands capture to the desktop");
  check(bx::capture_follow_target(true, true, false, false) == CaptureTarget::display, "the desktop with no game is the picture");
  check(bx::capture_follow_target(true, true, false, true) == CaptureTarget::display,
        "a foreground flag without a captured game means nothing");
  // The preview is excluded from capture, so a game behind it is still the picture the user opened it for.
  check(bx::capture_follow_write(CaptureTarget::game).capture_mode != nullptr, "the swap back sets the mode");
  check(bx::capture_follow_write(CaptureTarget::game).game_title == nullptr,
        "a swap back keeps the entered game id: a bare window title never hooks");
  check(bx::capture_follow_write(CaptureTarget::leave).capture_mode == nullptr, "leaving writes nothing");
  // A display is sized from its monitor. A window id left in the config by an earlier window capture must
  // not size it (a stale BuffrX panel id recorded a 2560x1440 display at the panel's 1440x1008).
  bx::Config target;
  target.capture_mode = "display";
  target.window_id = "BuffrX:BuffrXPanel:buffrx-engine.exe";
  check(bx::capture_window_target(target).empty(), "a display is never sized from a stale window id");
  target.capture_mode = "window";
  check(bx::capture_window_target(target) == target.window_id, "a window target is sized from its window");
  target.capture_mode = "game";
  target.game_title = "Game:Class:game.exe";
  check(bx::capture_window_target(target) == target.game_title, "a game target is sized from its game");
}
void test_game_detection() {
  std::printf("game detection\n");
  check(bx::game_process_excluded("chrome.exe"), "a browser is excluded");
  check(bx::game_process_excluded("Code.exe"), "an editor is excluded, case-insensitively");
  check(bx::game_process_excluded("explorer"), "the shell is excluded");
  check(bx::game_process_excluded("steam.exe"), "a launcher is excluded");
  check(bx::game_process_excluded("msiafterburner.exe"), "a performance overlay is excluded");
  check(bx::game_process_excluded("buffrx-engine.exe"), "this product is never a game");
  check(bx::game_process_excluded(""), "an empty name is never a game");
  check(!bx::game_process_excluded("eldenring.exe"), "an unknown executable is a game candidate");
  check(!bx::game_process_excluded("cs2"), "a bare game name is a candidate");
  check(bx::game_process_excluded("mytool.exe", {"mytool"}), "the user's own exclusion is honoured");
  check(!bx::game_process_excluded("mytool.exe", {"other"}), "an unrelated exclusion does not match");

  bx::GameCaptureState state;
  using D = bx::GameCaptureDecision;
  const uint32_t settle = bx::kGameEnterSettleMs;
  // Time, not poll counts: the foreground is sampled every tick (250 ms), and a game enters once it has been the
  // fullscreen foreground for the settle time - not after two samples five seconds apart, which was ten seconds.
  uint32_t t = 1000;
  auto observe = [&](bx::GameCaptureState& st, const char* exe, const char* title, bool fullscreen, bool excluded, bool alive) {
    t += 250;
    return bx::game_capture_observe(st, t, exe, title, fullscreen, excluded, alive);
  };
  // A windowed app is never a candidate, however long it stays.
  for (int i = 0; i < 12; ++i) check(observe(state, "game.exe", "Game", false, false, true) == D::none, "windowed is not a candidate");
  for (int i = 0; i < 12; ++i) check(observe(state, "chrome.exe", "Tab", true, false, true) == D::none, "excluded fullscreen is not a candidate");

  // It enters after the settle time, not before, and not after a fixed number of samples.
  D decision = D::none;
  uint32_t first_seen = t + 250;
  int ticks = 0;
  while (decision == D::none && ticks < 40) {
    decision = observe(state, "game.exe", "Game", true, false, true);
    ++ticks;
  }
  check(decision == D::enter, "a fullscreen game enters game capture");
  check(t - first_seen >= settle && t - first_seen < settle + 600, "after the settle time, within a tick or two of it");
  check(state.capturing && state.active_exe == "game.exe", "the active game is remembered");
  check(observe(state, "game.exe", "Game", true, false, true) == D::none, "the active game polls as none");

  // A flicker is not a game: a fullscreen window that goes away before the settle time resets the clock.
  bx::GameCaptureState flicker;
  observe(flicker, "splash.exe", "Splash", true, false, true);
  observe(flicker, "splash.exe", "Splash", true, false, true);
  check(observe(flicker, "explorer.exe", "Desktop", false, false, true) == D::none, "the splash gives way to the desktop");
  D after = D::none;
  for (int i = 0; i < 2; ++i) after = observe(flicker, "splash.exe", "Splash", true, false, true);
  check(after == D::none && !flicker.capturing, "and when it returns the clock starts over");

  // A different game takes over directly: no trip back to the display, but only after a longer settle - leaving a
  // live game for something else fullscreen is not done on a glance.
  D handover = D::none;
  uint32_t other_seen = t + 250;
  ticks = 0;
  while (handover == D::none && ticks < 80) {
    handover = observe(state, "other.exe", "Other", true, false, true);
    ++ticks;
  }
  check(handover == D::enter, "another game takes over with one decision, not leave and enter");
  check(t - other_seen >= bx::kGameHandoverSettleMs, "after the longer hand-over settle");
  check(state.capturing && state.active_exe == "other.exe", "and is now the active game");

  // The user's exclusion list also blocks a candidate.
  bx::GameCaptureState blocked;
  for (int i = 0; i < 12; ++i) check(observe(blocked, "mytool.exe", "Tool", true, true, true) == D::none, "a user-excluded fullscreen app is not a candidate");

  // A candidate that changes process starts the clock again.
  bx::GameCaptureState swap;
  for (int i = 0; i < 2; ++i) observe(swap, "a.exe", "A", true, false, true);
  D swapped = D::none;
  int swap_ticks = 0;
  while (swapped == D::none && swap_ticks < 40) {
    swapped = observe(swap, "b.exe", "B", true, false, true);
    ++swap_ticks;
  }
  check(swapped == D::enter && swap.active_exe == "b.exe" && swap_ticks >= 3, "a different process is its own candidate with its own settle");

  // Alt-tab (Rewind's rule): while the game runs, focusing a browser or the desktop keeps the game's capture - the
  // hook keeps delivering the game - and only the game exiting leaves, at once.
  bx::GameCaptureState tabbed;
  D entered = D::none;
  for (int i = 0; i < 40 && entered == D::none; ++i) entered = observe(tabbed, "game.exe", "Game", true, false, true);
  check(entered == D::enter, "the game is captured");
  check(observe(tabbed, "explorer.exe", "Desktop", false, false, true) == D::none, "alt-tab to the desktop keeps the game");
  for (int i = 0; i < 12; ++i) check(observe(tabbed, "chrome.exe", "Tab", true, false, true) == D::none, "a fullscreen browser is not a game and does not take over");
  check(tabbed.capturing, "still capturing the game");
  check(observe(tabbed, "explorer.exe", "Desktop", false, false, false) == D::leave, "the game exiting leaves on the very next sample");
}

// The native interface's wording (ui_format.h): the same conventions the React panel used.
// P2-4: the ring's rule for what leaves the front, ported from OBS's replay buffer: whole groups of pictures, never below two keyframes.
void test_ring_index() {
  using bx::RingEntry;
  const int64_t second = 1000000;
  auto feed = [&](bx::RingIndex& ring, int64_t seconds, bool video, bool key, uint32_t size, int64_t max_time_s, int64_t max_bytes) {
    RingEntry entry;
    entry.dts_usec = seconds * second;
    entry.size = size;
    entry.video_keyframe = video && key;
    const size_t removed = ring.room_for(entry, max_time_s * second, max_bytes);
    ring.push(entry);
    return removed;
  };
  bx::RingIndex ring;
  size_t total_removed = 0;
  for (int t = 0; t <= 10; ++t) total_removed += feed(ring, t, true, t % 2 == 0, 100, 5, 0);
  check(ring.keyframes() == 3 && ring.packets() == 5 && total_removed == 6,
        "a ring that holds 5 s of 2 s groups drops whole groups from the front: frames 0-5 are gone, 6-10 stay");
  check(ring.span_us() == 4 * second && ring.bytes() == 500, "the span and the bytes are those of what is left");

  bx::RingIndex tight;
  size_t tight_removed = 0;
  for (int t = 0; t <= 6; ++t) tight_removed += feed(tight, t, true, t % 2 == 0, 100, 1, 0);
  check(tight_removed == 0 || tight.keyframes() >= 2, "a ring never goes below two keyframes, whatever the time limit says");

  bx::RingIndex bytes;
  size_t bytes_removed = 0;
  for (int t = 0; t <= 9; ++t) bytes_removed += feed(bytes, t, true, t % 2 == 0, 100, 1000, 450);
  check(bytes.bytes() <= 450 && bytes.keyframes() >= 2 && bytes_removed > 0, "the byte limit drops groups too, and keeps two keyframes");

  bx::RingIndex lead;
  feed(lead, 0, false, false, 10, 100, 0);  // an audio packet before the first keyframe
  feed(lead, 0, true, true, 50, 100, 0);
  feed(lead, 1, true, false, 50, 100, 0);
  feed(lead, 2, true, true, 50, 100, 0);
  feed(lead, 4, true, true, 50, 100, 0);
  RingEntry late;
  late.dts_usec = 200 * second;
  late.size = 10;
  check(lead.room_for(late, 3 * second, 0) >= 1, "an old front is dropped when the span is past the limit");
  bx::RingIndex front_audio;
  feed(front_audio, -1, false, false, 10, 100, 0);  // audio that began a second before the first keyframe
  feed(front_audio, 0, true, true, 50, 100, 0);
  feed(front_audio, 2, true, true, 50, 100, 0);
  feed(front_audio, 4, true, true, 50, 100, 0);
  RingEntry next;
  next.dts_usec = 5 * second + second / 2;
  next.size = 10;
  check(front_audio.room_for(next, 6 * second, 0) == 1, "a front that is not a keyframe leaves alone, not with a group"); 
  front_audio.clear();
  check(front_audio.packets() == 0 && front_audio.bytes() == 0 && front_audio.keyframes() == 0 && front_audio.span_us() == 0, "clear empties it");
}

// RAM nearly full: the threshold, the two messages, and the probe that fakes the reading.
void test_memory_policy() {
  check(!bx::capture_memory_ok(300) && !bx::capture_memory_ok(767) && bx::capture_memory_ok(768) && bx::capture_memory_ok(16000),
        "capture needs 768 MB free: under it the start is refused");
  check(bx::capture_memory_ok(-1), "a reading Windows would not give is not a reason to refuse");
  const std::string fresh = bx::memory_full_message(true);
  const std::string running = bx::memory_full_message(false);
  check(fresh == "Your RAM looks full.\n\nClose some apps, then try starting BuffrX again.", "a fresh start is told the RAM looks full and to try starting BuffrX again");
  check(running == "Your RAM looks full. Close some apps, then start the buffer again.", "a running engine is told to start the buffer again");
  check(fresh.find("MB") == std::string::npos && running.find("MB") == std::string::npos && fresh.find("capture engine") == std::string::npos,
        "neither message carries a figure or the old capture-engine wording");
  check(bx::free_memory_reading(5000, nullptr) == 5000 && bx::free_memory_reading(5000, "") == 5000, "no probe variable leaves the real reading");
  check(bx::free_memory_reading(5000, "300") == 300 && bx::free_memory_reading(5000, "0") == 0, "the probe variable replaces the reading");
  check(bx::free_memory_reading(5000, "lots") == 5000 && bx::free_memory_reading(5000, "300x") == 5000, "a probe value that is not a number is ignored");

  bx::MemoryWatch watch;
  check(!watch.observe(300, false, 1000), "no warning while the buffer is not running");
  check(watch.observe(300, true, 6000), "a buffer running with memory under the need is said once");
  check(!watch.observe(300, true, 7000) && !watch.observe(300, true, 12000), "the same episode is not said again, and not sooner than 5 s");
  check(!watch.observe(4000, true, 17000), "memory coming back says nothing");
  check(watch.observe(200, true, 22000), "a second episode is said again");
}

// ---------------------------------------------------------------- the ring's disk tier (ring_spill.h, ring_store.h)

struct SpillTestPacket {
  std::shared_ptr<std::vector<uint8_t>> bytes;
  bx::PacketFields fields;
};
struct SpillTestOps {
  using Packet = SpillTestPacket;
  static Packet add_ref(const Packet& packet) { return packet; }
  static void release(Packet& packet) { packet.bytes.reset(); }
  static bx::PacketFields fields(const Packet& packet) {
    bx::PacketFields fields = packet.fields;
    fields.data = packet.bytes ? packet.bytes->data() : nullptr;
    fields.size = packet.bytes ? static_cast<uint32_t>(packet.bytes->size()) : 0;
    return fields;
  }
};
using SpillStore = bx::ReplayStore<SpillTestOps>;

// A file in memory that can refuse writes and reads, and remembers every block written.
class SpillFakeDisk : public bx::SpillBackend {
public:
  explicit SpillFakeDisk(int64_t size) : size_(size), file_(static_cast<size_t>(size), 0) {}
  bool write_block(int64_t offset, const uint8_t* data) override {
    if (fail_writes) return false;
    writes.push_back(offset);
    std::memcpy(&file_[static_cast<size_t>(offset % size_)], data, static_cast<size_t>(bx::kSpillBlockBytes));
    return true;
  }
  bool read_block(int64_t offset, uint8_t* destination) override {
    if (offset >= fail_read_from && offset < fail_read_to) return false;
    std::memcpy(destination, &file_[static_cast<size_t>(offset % size_)], static_cast<size_t>(bx::kSpillBlockBytes));
    return true;
  }
  bool fail_writes = false;
  int64_t fail_read_from = 1LL << 60, fail_read_to = 0;
  std::vector<int64_t> writes;

private:
  int64_t size_;
  std::vector<uint8_t> file_;
};

// A ring with a disk tier, wired to a fake disk. `ram_limit` is the limit the spill starts under (a margin below it).
struct SpillRig {
  SpillFakeDisk disk;
  std::vector<uint8_t> block, window_a, window_b;
  SpillStore store;
  int64_t ram_limit;
  int64_t file_bytes_;
  int64_t max_ram_seen = 0;
  SpillRig(int64_t file_bytes, int64_t limit, int64_t max_time_us, int64_t ceiling)
      : disk(file_bytes), block(static_cast<size_t>(bx::kSpillBlockBytes)), window_a(block.size()), window_b(block.size()), ram_limit(limit),
        file_bytes_(file_bytes) {
    store.configure(max_time_us, ceiling);
    enable();
  }
  // The disk settings again after a reset: clearing the store forgets the file size too.
  void enable() {
    SpillStore::DiskSetup setup;
    setup.io = &disk;
    setup.file_bytes = file_bytes_;
    setup.ram_limit_bytes = ram_limit;
    setup.block = block.data();
    setup.window_a = window_a.data();
    setup.window_b = window_b.data();
    store.enable_disk(setup);
  }
  void reset() {
    store.reset();
    enable();
  }
  void push(const SpillTestPacket& packet) {
    store.push(packet);
    while (store.spill_due() && store.spill_step()) {
    }
    max_ram_seen = std::max(max_ram_seen, store.info().ram_bytes);
  }
};

constexpr int64_t kMiB = 1024 * 1024;
int g_spill_seq = 0;

SpillTestPacket spill_packet(bool video, bool key, int64_t dts_usec, uint32_t size) {
  SpillTestPacket packet;
  packet.bytes = std::make_shared<std::vector<uint8_t>>(size, static_cast<uint8_t>(g_spill_seq & 0xff));
  if (size >= 4) std::memcpy(packet.bytes->data(), &g_spill_seq, 4);
  ++g_spill_seq;
  packet.fields.video = video;
  packet.fields.keyframe = video && key;
  packet.fields.track = 0;
  packet.fields.dts_usec = dts_usec;
  packet.fields.timebase_num = 1;
  packet.fields.timebase_den = video ? 1000000 : 48000;
  packet.fields.dts = video ? dts_usec : dts_usec * 48 / 1000;
  packet.fields.pts = packet.fields.dts;
  return packet;
}

// `seconds` of 20 fps video (a 4x keyframe every second) with an audio packet at every frame - the same time as the picture, so ties are common.
std::vector<SpillTestPacket> spill_stream(int seconds, int64_t base_us, uint32_t video_bytes = 100 * 1024) {
  std::vector<SpillTestPacket> out;
  for (int frame = 0; frame < seconds * 20; ++frame) {
    const int64_t at = base_us + frame * 50000LL;
    const bool key = frame % 20 == 0;
    out.push_back(spill_packet(true, key, at, key ? video_bytes * 4 : video_bytes));
    out.push_back(spill_packet(false, false, at, 3 * 1024));
  }
  return out;
}

struct SpillOut {
  bx::PacketFields fields;
  std::vector<uint8_t> bytes;
};
bool operator==(const SpillOut& a, const SpillOut& b) {
  return a.fields.pts == b.fields.pts && a.fields.dts == b.fields.dts && a.fields.dts_usec == b.fields.dts_usec && a.fields.video == b.fields.video &&
         a.fields.keyframe == b.fields.keyframe && a.fields.track == b.fields.track && a.bytes == b.bytes;
}

std::vector<SpillOut> spill_collect(SpillStore& store, bx::SaveReport* report = nullptr) {
  std::vector<SpillOut> out;
  const bx::SaveReport done = store.save([&](const bx::PacketFields& fields) {
    SpillOut item;
    item.fields = fields;
    item.bytes.assign(fields.data, fields.data + fields.size);
    out.push_back(std::move(item));
    return true;
  });
  if (report) *report = done;
  return out;
}

#include "dedupe_tests.h"

// A save cut short of the whole ring while part of the ring is in Timeline chunks: the job's references must be exactly the RAM
// packets of the snapshot it was cut to, and every picture it writes must be the one the ring received at that place. The cut used
// to be counted against the disk records alone while the RAM packets skip chunk records too, so the references were shifted and the
// save read past their end.
void test_dedupe_partial_save() {
  using namespace dedupe_test;
  const int64_t file = 12 * bx::kSpillBlockBytes;
  const int64_t limit = 16 * kMiB;
  const int64_t window_us = 45 * 1000000LL;
  const int64_t no_ceiling = 1LL << 40;
  const std::vector<SpillTestPacket> all = stream(80, 20);  // 80 s, about 1 MB a second

  SpillRig rig(file, limit, window_us, no_ceiling);
  rig.store.configure_dedupe(bx::HashCodec::h264);
  // Every third chunk is not Timeline's: that part of the ring spills to the file, so the records out of RAM alternate.
  feed(rig, all, [](int chunk) { return chunk % 3 != 1; }, [](int) { return true; });
  const bx::StoreInfo info = rig.store.info();
  check(info.chunk_bytes > 0 && info.disk_bytes > 0, "setup: the ring holds chunk records and disk records");

  std::unique_ptr<SpillStore::SaveJob> whole = rig.store.begin_save();
  const std::vector<bx::RingEntry>& full = whole->entries;
  size_t disk_front = 0;
  while (disk_front < full.size() && full[disk_front].on_disk) ++disk_front;
  size_t out_of_ram = 0;
  while (out_of_ram < full.size() && (full[out_of_ram].on_disk || full[out_of_ram].on_chunk)) ++out_of_ram;
  const int64_t want = full.back().dts_usec - 30 * 1000000LL;
  size_t cut = 0;
  for (size_t i = 0; i < full.size(); ++i) {
    if (full[i].video && full[i].video_keyframe && full[i].dts_usec <= want) cut = i;
  }
  whole.reset();  // its pin goes before the save under test takes its own
  check(disk_front < cut && cut < out_of_ram, "setup: a 30 s clip is cut inside the part that left RAM, past its disk-only front");

  std::unique_ptr<SpillStore::SaveJob> job = rig.store.begin_save(30);
  size_t ram = 0;
  for (const bx::RingEntry& entry : job->entries) ram += entry.on_disk || entry.on_chunk ? 0 : 1;
  check(bx::snapshot_has_chunks(job->entries), "the cut still holds chunk records, so the save goes in parts");
  check(job->refs.size() == ram, "the job's references are exactly the RAM packets of the snapshot it was cut to");

  // The packet part of a save in parts is a run of the snapshot's RAM records: what it writes must be those packets, in order. A picture
  // is named by its ordinal among the stream's video packets, which is the ring entry's own `video_seq`.
  std::vector<uint32_t> ram_indices;
  std::vector<int> expected;
  for (uint32_t i = 0; i < job->entries.size(); ++i) {
    const bx::RingEntry& entry = job->entries[i];
    if (entry.on_disk || entry.on_chunk) continue;
    ram_indices.push_back(i);
    if (entry.video) expected.push_back(static_cast<int>(entry.video_seq));
  }
  std::vector<std::pair<uint64_t, int>> by_hash;
  int ordinal = 0;
  for (const SpillTestPacket& p : all) {
    if (!p.fields.video) continue;
    by_hash.push_back({bx::payload_hash(p.bytes->data(), p.bytes->size()), ordinal++});
  }
  std::vector<int> written;
  bool named = true;
  const bx::SaveReport report = rig.store.run_part(*job, ram_indices, [&](const bx::PacketFields& fields) {
    if (!fields.video) return true;
    const uint64_t hash = bx::payload_hash(fields.data, fields.size);
    int which = -1;
    for (const auto& item : by_hash) {
      if (item.first == hash) which = item.second;
    }
    named = named && which >= 0;
    written.push_back(which);
    return true;
  });
  check(report.ok && named && !written.empty(), "the packet part of the save reads every RAM record it was given");
  check(written == expected, "and writes each picture at its own place: the stream is not shifted along");
}

// The store keeps the Timeline session each chunk file came from (obs_host_dedupe.cpp hands it over with the probe), so
// a save knows exactly which sessions to guard before it opens any of those files (ring_output.cpp).
void test_dedupe_chunk_sessions() {
  using namespace dedupe_test;
  SpillRig rig(12 * bx::kSpillBlockBytes, 16 * kMiB, 45 * 1000000LL, 1LL << 40);
  rig.store.configure_dedupe(bx::HashCodec::h264);
  feed(rig, stream(20, 20), [](int) { return true; }, [](int) { return true; }, "session-r");
  std::unique_ptr<SpillStore::SaveJob> job = rig.store.begin_save();
  check(!job->chunks.empty(), "setup: the save's snapshot names chunk records");
  bool named = !job->chunks.empty();
  for (const auto& item : job->chunks) named = named && item.second.session_id == "session-r";
  check(named, "a save's chunk records name the Timeline session they came from");
}

void test_ring_spill_policy() {
  const int64_t gib = 1024 * kMiB;
  check(bx::ring_ram_limit_bytes(4 * gib) == 2 * gib, "a roomy machine (4 GiB ceiling) holds 2 GiB of footage in RAM");
  check(bx::ring_ram_limit_bytes(2 * gib) == 1536 * kMiB, "a 16 GB PC (2 GiB ceiling) holds three quarters of its ceiling");
  check(bx::ring_ram_limit_bytes(1 * gib) == 768 * kMiB, "an 8 GB PC (1 GiB ceiling) holds 768 MiB");
  check(bx::ring_ram_limit_bytes(64 * kMiB) == 64 * kMiB && bx::ring_ram_limit_bytes(400 * kMiB) == 300 * kMiB,
        "a tiny ceiling is the limit itself, and the 128 MiB floor never passes the ceiling");
  check(bx::spill_start_bytes(2 * gib) == 2 * gib - 64 * kMiB && bx::spill_start_bytes(32 * kMiB) == 16 * kMiB,
        "spilling starts a margin under the limit: 64 MiB, or half of a small limit");

  const int64_t limit = 2 * gib;
  check(bx::spill_reservation_bytes(50000, 192, 300, limit) == 0, "the user's own buffer (300 s at a 50 Mbps peak, about 1.9 GB) needs no file");
  const int64_t ten_minutes = bx::spill_reservation_bytes(50000, 192, 600, limit);
  const int64_t footage = static_cast<int64_t>(50000 + 192) * 1000 / 8 * 602;
  check(ten_minutes > footage - bx::spill_start_bytes(limit) && ten_minutes % bx::kSpillBlockBytes == 0 && ten_minutes < footage,
        "ten minutes at the same peak reserves the footage past the RAM limit plus slack, in whole blocks");
  check(bx::spill_reservation_bytes(0, 192, 600, limit) > ten_minutes, "a rate control with no peak (CQP) reserves for 200 Mbps");
  check(bx::spill_reservation_bytes(50000, 192, 1800, limit) > ten_minutes && bx::spill_reservation_bytes(50000, 192, 60, limit) == 0,
        "a longer buffer reserves more, a short one nothing");
  check(bx::spill_reservation_bytes(50000, 192, 600, 32 * kMiB) > bx::spill_reservation_bytes(50000, 192, 600, limit),
        "a smaller RAM limit moves more footage to the file");

  uint8_t header[bx::kSpillRecordHeaderBytes];
  bx::PacketFields original;
  original.pts = 123456789012LL;
  original.dts = -42;
  original.dts_usec = 987654321;
  original.timebase_num = 1;
  original.timebase_den = 90000;
  original.track = 3;
  original.video = false;
  original.keyframe = false;
  original.size = 4321;
  bx::encode_record_header(original, header);
  bx::PacketFields decoded;
  check(bx::decode_record_header(header, decoded) && decoded.pts == original.pts && decoded.dts == original.dts && decoded.dts_usec == original.dts_usec &&
            decoded.timebase_den == 90000 && decoded.track == 3 && !decoded.video && !decoded.keyframe && decoded.size == 4321,
        "a record header carries every number a clip needs");
  header[40] ^= 0xff;
  check(!bx::decode_record_header(header, decoded), "bytes that are not a record (wrong magic) are refused");

  SpillFakeDisk disk(4 * bx::kSpillBlockBytes);
  std::vector<uint8_t> block(static_cast<size_t>(bx::kSpillBlockBytes)), wa(block.size()), wb(block.size());
  bx::BlockWriter writer(disk, block.data());
  std::vector<uint8_t> chunk(5 * 1024 * 1024);
  for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<uint8_t>(i * 13 + 7);
  check(writer.append(chunk.data(), chunk.size()) && disk.writes.empty() && writer.flushed() == 0 && writer.pos() == 5 * kMiB,
        "5 MB appended writes nothing: a block goes out only when it is full");
  check(writer.append(chunk.data(), chunk.size()) && disk.writes.size() == 1 && disk.writes[0] == 0 && writer.flushed() == bx::kSpillBlockBytes && writer.pos() == 10 * kMiB,
        "the second 5 MB fills the first block, and it is written at offset 0");
  std::vector<uint8_t> big(20 * 1024 * 1024, 0x5a);
  check(writer.append(big.data(), big.size()) && disk.writes.size() == 3 && disk.writes[1] == bx::kSpillBlockBytes && disk.writes[2] == 2 * bx::kSpillBlockBytes,
        "a record larger than a block is written as consecutive blocks, in order");
  bx::DiskReader reader(disk, wa.data(), wb.data());
  std::vector<uint8_t> back(chunk.size());
  check(reader.read(0, back.data(), back.size()) && back == chunk, "the first 5 MB read back as written");
  std::vector<uint8_t> across(3 * 1024 * 1024);
  check(reader.read(bx::kSpillBlockBytes - 1024 * 1024, across.data(), across.size()), "a read across a block edge succeeds");
  bool across_ok = true;
  for (size_t i = 0; i < across.size(); ++i) {
    const int64_t at = bx::kSpillBlockBytes - 1024 * 1024 + static_cast<int64_t>(i);
    const uint8_t expected = at < 10 * kMiB ? chunk[static_cast<size_t>(at % (5 * kMiB))] : 0x5a;
    if (across[i] != expected) {
      across_ok = false;
      break;
    }
  }
  check(across_ok, "and returns the bytes of both blocks");
  disk.fail_writes = true;
  bx::BlockWriter failing(disk, block.data());
  check(!failing.append(big.data(), big.size()) && failing.failed() && !failing.append(big.data(), 1), "a refused block fails the writer, and it stays failed");
}

// Today's replay save sorted by insertion: a packet goes after the last one that is strictly earlier, so equal times end up in reverse arrival order.
void test_save_order() {
  uint32_t state = 12345;
  auto next = [&]() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  };
  for (int round = 0; round < 20; ++round) {
    std::vector<bx::RingEntry> entries;
    bx::StreamOffsets offsets;
    offsets.has_video = true;
    offsets.video_us = 1000 + round;
    offsets.has_audio[0] = true;
    offsets.audio_us[0] = 5000;
    offsets.has_audio[1] = true;
    offsets.audio_us[1] = 7000;
    int64_t clock = 100000;
    for (int i = 0; i < 200; ++i) {
      bx::RingEntry entry;
      clock += next() % 3 == 0 ? 0 : static_cast<int64_t>(next() % 40000);
      entry.video = next() % 2 == 0;
      entry.track = static_cast<uint8_t>(next() % 2);
      entry.dts_usec = clock + static_cast<int64_t>(next() % 3) * 1000 + (entry.video ? 0 : 11000);
      entries.push_back(entry);
    }
    std::vector<uint32_t> reference;
    std::vector<int64_t> keys;
    for (const bx::RingEntry& entry : entries) keys.push_back(entry.dts_usec - offsets.microseconds_for(entry.video, entry.track));
    for (uint32_t i = 0; i < entries.size(); ++i) {
      size_t at = reference.size();
      while (at > 0 && keys[reference[at - 1]] >= keys[i]) --at;
      reference.insert(reference.begin() + static_cast<std::ptrdiff_t>(at), i);
    }
    if (bx::save_order(entries, offsets) != reference) {
      check(false, "the save order equals the insertion sort of the replay buffer on a random interleaving");
      return;
    }
  }
  check(true, "the save order equals the insertion sort of the replay buffer on 20 random interleavings (ties included)");
}

void test_ring_index_disk() {
  using bx::RingEntry;
  bx::RingIndex ring;
  ring.enable_disk(4 * bx::kSpillBlockBytes);
  auto entry = [](int64_t seconds, bool key, uint32_t size) {
    RingEntry e;
    e.dts_usec = seconds * 1000000;
    e.size = size;
    e.video = true;
    e.video_keyframe = key;
    return e;
  };
  for (int t = 0; t < 8; ++t) {
    ring.room_for(entry(t, t % 2 == 0, 1000000), 1000 * 1000000LL, 0);
    ring.push(entry(t, t % 2 == 0, 1000000));
  }
  check(ring.ram_bytes() == 8000000 && ring.disk_bytes() == 0 && ring.tail() == 0 && !ring.spill_due(9000000), "nothing is due while RAM is under the spill start");
  check(ring.spill_due(3000000), "RAM over the spill start makes a spill due");
  size_t position = 0;
  int64_t offset = 0;
  check(ring.next_to_stage(3000000, position, offset) && position == 0 && offset == 0, "the oldest entry is staged first, at offset 0");
  check(ring.next_to_stage(3000000, position, offset) && position == 1 && offset == 1000000 + bx::kSpillRecordHeaderBytes,
        "the next one follows it in the block stream, header included");
  check(ring.staged_count() == 2 && ring.ram_bytes() == 8000000 && ring.tail() == 0, "staged entries still count as RAM until their block is written");
  check(ring.block_written(1000000) == 0 && ring.disk_count() == 0, "a block that ends inside an entry frees nothing");
  const int64_t footprint = 1000000 + bx::kSpillRecordHeaderBytes;
  check(ring.block_written(footprint) == 1 && ring.disk_count() == 1 && ring.ram_bytes() == 7000000 && ring.disk_bytes() == footprint,
        "an entry leaves RAM when the block holding its last byte is written");
  check(ring.block_written(2 * footprint) == 1 && ring.disk_count() == 2 && ring.staged_count() == 0 && ring.spilled_total() == 2 * footprint, "and the next with its block");
  check(ring.bytes() == 8000000, "payload held counts RAM and disk together");

  bx::RingIndex tight;
  tight.enable_disk(bx::kSpillBlockBytes);
  for (int t = 0; t < 3; ++t) tight.push(entry(t, true, 3 * 1024 * 1024));
  check(tight.next_to_stage(0, position, offset) && tight.next_to_stage(0, position, offset), "two 3 MiB records fit an 8 MiB file");
  check(!tight.next_to_stage(0, position, offset) && tight.stalled(), "a third has no room: the file is full, the footage stays in RAM and the ring says so");
  tight.block_written(2 * (3 * kMiB + bx::kSpillRecordHeaderBytes));
  check(tight.disk_count() == 2, "what was staged still flips when its block is written");

  bx::RingIndex pinned;
  pinned.enable_disk(2 * bx::kSpillBlockBytes);
  for (int t = 0; t < 4; ++t) pinned.push(entry(t, true, 4 * 1024 * 1024));
  while (pinned.next_to_stage(0, position, offset)) {
  }
  pinned.block_written(pinned.head());
  check(pinned.disk_count() == 3 && pinned.stalled(), "a 16 MiB file takes three 4 MiB records and stalls on the fourth");
  const std::vector<RingEntry> view = pinned.snapshot();
  check(view.size() == 4 && view[0].on_disk && !view[3].on_disk, "a snapshot copies every entry, with where each one is");
  pinned.room_for(entry(4, true, 4 * 1024 * 1024), 2500000, 0);  // a 2.5 s limit: the two oldest leave by time
  check(pinned.packets() == 2 && pinned.removed_ram() == 0, "time eviction takes the disk front, which has no payload to release");
  check(pinned.tail() == 2 * (4 * kMiB + bx::kSpillRecordHeaderBytes) && pinned.floor() == 0,
        "the tail moved on, but while a save is reading the writer may not go past the oldest record it pinned");
  pinned.release_pin();
  check(pinned.floor() == pinned.tail(), "and once the save is over the space is free again");

  bx::RingIndex limit;
  limit.enable_disk(4 * bx::kSpillBlockBytes);
  for (int t = 0; t < 10; ++t) limit.push(entry(t, t % 2 == 0, 1000000));
  size_t pos = 0;
  int64_t off = 0;
  while (limit.next_to_stage(6000000, pos, off)) {
  }
  limit.block_written(limit.head());
  check(limit.disk_count() == 4 && limit.ram_bytes() == 6000000, "setup: four entries are on disk and six in RAM");
  limit.room_for(entry(10, false, 1000000), 1000 * 1000000LL, 6500000);  // RAM would pass its ceiling
  check(limit.disk_count() == 0 && limit.backstop_events() == 1, "at the RAM ceiling the disk part is given up as a whole, once");
  check(limit.packets() == 4 && limit.ram_bytes() == 4000000, "then RAM is trimmed from the front by whole groups, as before");
}

void test_replay_store() {
  const int64_t file = 7 * bx::kSpillBlockBytes;
  const int64_t limit = 32 * kMiB;
  const int64_t window_us = 20 * 1000000LL;
  const int64_t no_ceiling = 1LL << 40;

  // The same packets into a RAM-only ring and into one with a disk tier, 60 s through a 20 s window and a file that wraps.
  SpillStore reference;
  reference.configure(window_us, no_ceiling);
  SpillRig rig(file, limit, window_us, no_ceiling);
  for (const SpillTestPacket& packet : spill_stream(60, 0)) {
    reference.push(packet);
    rig.push(packet);
  }
  bx::SaveReport report_ram, report_disk;
  const std::vector<SpillOut> expected = spill_collect(reference, &report_ram);
  const std::vector<SpillOut> actual = spill_collect(rig.store, &report_disk);
  const bx::StoreInfo info = rig.store.info();
  check(!expected.empty() && report_ram.ok && report_disk.ok && report_disk.skipped == 0, "both rings save");
  check(actual.size() == expected.size() && actual == expected,
        "a ring with a disk tier saves exactly the packets, in exactly the order, with exactly the numbers of a RAM-only ring");
  check(info.spilled_total > 0 && info.disk_bytes > 0 && rig.max_ram_seen <= limit, "footage went to the file and RAM never passed its limit");
  check(rig.disk.writes.size() > 7, "the file wrapped: more blocks were written than it holds");
  bool sequential = true;
  for (size_t i = 0; i < rig.disk.writes.size(); ++i) sequential = sequential && rig.disk.writes[i] == static_cast<int64_t>(i) * bx::kSpillBlockBytes;
  check(sequential, "blocks are written strictly in order from offset 0");
  check(reference.info().bytes == info.bytes && reference.info().packets == info.packets && reference.info().span_us == info.span_us,
        "the disk tier holds the same footage as RAM would (same bytes, same packets, same span)");

  // A save in flight, and the writer trying to go on: the disk part it reads must not be overwritten.
  SpillStore reference2;
  reference2.configure(window_us, no_ceiling);
  SpillRig pin_rig(file, limit, window_us, no_ceiling);
  for (const SpillTestPacket& packet : spill_stream(30, 0)) {
    reference2.push(packet);
    pin_rig.push(packet);
  }
  const std::vector<SpillOut> expected2 = spill_collect(reference2);
  std::vector<SpillOut> during;
  bool stalled_while_reading = false;
  size_t seen = 0;
  const bx::SaveReport pinned_report = pin_rig.store.save([&](const bx::PacketFields& fields) {
    SpillOut item;
    item.fields = fields;
    item.bytes.assign(fields.data, fields.data + fields.size);
    during.push_back(std::move(item));
    if (++seen == 10) {
      for (const SpillTestPacket& packet : spill_stream(40, 40 * 1000000LL)) pin_rig.push(packet);
      stalled_while_reading = pin_rig.store.info().stalled;
    }
    return true;
  });
  check(pinned_report.ok && during == expected2, "a save that reads the disk while the ring goes on still writes the footage as it was when it began");
  check(stalled_while_reading, "the writer waited for the save rather than overwrite what it reads: the footage stayed in RAM");
  while (pin_rig.store.spill_due() && pin_rig.store.spill_step()) {
  }
  check(!pin_rig.store.info().stalled, "once the save is over the writer goes on");

  // A record bigger than a block.
  const int64_t long_window_us = 60 * 1000000LL;
  SpillStore reference3;
  reference3.configure(long_window_us, no_ceiling);
  SpillRig big_rig(9 * bx::kSpillBlockBytes, limit, long_window_us, no_ceiling);
  std::vector<SpillTestPacket> stream = spill_stream(5, 0);
  stream.push_back(spill_packet(true, true, 5 * 1000000LL, 20 * 1024 * 1024));
  for (const SpillTestPacket& packet : spill_stream(15, 5 * 1000000LL + 50000)) stream.push_back(packet);
  for (const SpillTestPacket& packet : stream) {
    reference3.push(packet);
    big_rig.push(packet);
  }
  check(big_rig.store.info().disk_bytes > 20 * kMiB, "setup: the 20 MB keyframe is on disk");
  check(spill_collect(big_rig.store) == spill_collect(reference3), "a 20 MB keyframe crosses three blocks and comes back whole");

  // A file too small for the footage: it fills, then the footage waits in RAM.
  SpillStore reference4;
  reference4.configure(long_window_us, no_ceiling);
  SpillRig small_rig(2 * bx::kSpillBlockBytes, limit, long_window_us, no_ceiling);
  for (const SpillTestPacket& packet : spill_stream(40, 0)) {
    reference4.push(packet);
    small_rig.push(packet);
  }
  check(small_rig.store.info().stalled && small_rig.store.info().ram_bytes > limit, "a full file stalls the spill and RAM grows past the limit instead of dropping footage");
  check(spill_collect(small_rig.store) == spill_collect(reference4), "and nothing was lost");

  // A disk that fails: the ring carries on in RAM, and at the RAM ceiling the disk part is given up.
  SpillRig dying(file, limit, long_window_us, 40 * kMiB);
  for (const SpillTestPacket& packet : spill_stream(12, 0)) dying.push(packet);
  const int64_t on_disk_before = dying.store.info().disk_bytes;
  dying.disk.fail_writes = true;
  for (const SpillTestPacket& packet : spill_stream(18, 12 * 1000000LL)) dying.push(packet);
  const bx::StoreInfo dead = dying.store.info();
  check(on_disk_before > 0 && dead.spill_failed && !dead.spill, "a refused block turns the disk tier off");
  check(dead.backstop_events == 1 && dead.disk_bytes == 0 && dead.ram_bytes <= 40 * kMiB, "at the ceiling the disk part is given up once and RAM stays under it");
  bx::SaveReport dying_report;
  const std::vector<SpillOut> rest = spill_collect(dying.store, &dying_report);
  size_t first_video = 0;
  while (first_video < rest.size() && !rest[first_video].fields.video) ++first_video;
  check(dying_report.ok && !rest.empty() && first_video < rest.size() && rest[first_video].fields.keyframe,
        "the clip that is left still begins on a keyframe");

  // A disk part that cannot be read: a gap up to the next keyframe, not a lost clip.
  SpillRig flaky(file, limit, window_us, no_ceiling);
  size_t total = 0;
  for (const SpillTestPacket& packet : spill_stream(25, 0)) {
    flaky.push(packet);
    ++total;
  }
  flaky.disk.fail_read_from = bx::kSpillBlockBytes;
  flaky.disk.fail_read_to = 2 * bx::kSpillBlockBytes;
  bx::SaveReport flaky_report;
  const std::vector<SpillOut> partial = spill_collect(flaky.store, &flaky_report);
  check(flaky_report.ok && flaky_report.unreadable && flaky_report.skipped > 0 && partial.size() + flaky_report.skipped == flaky.store.info().packets,
        "an unreadable block costs the packets up to the next keyframe, and the report says so");
  (void)total;
}

// The hash that tells a ring record from a chunk's packet whatever the container did to its framing (packet_hash.h).
void test_packet_hash() {
  using bx::HashCodec;
  using Bytes = std::vector<uint8_t>;
  const auto cat = [](std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
  };
  const auto sized = [](uint8_t first, uint8_t fill, size_t count) {  // a NAL unit: its header byte and `count` bytes of payload (never zero)
    Bytes nal(count + 1, fill);
    nal[0] = first;
    return nal;
  };
  const auto annexb4 = [](const Bytes& nal) { Bytes out = {0, 0, 0, 1}; out.insert(out.end(), nal.begin(), nal.end()); return out; };
  const auto annexb3 = [](const Bytes& nal) { Bytes out = {0, 0, 1}; out.insert(out.end(), nal.begin(), nal.end()); return out; };
  const auto prefixed = [](const Bytes& nal) {
    Bytes out = {static_cast<uint8_t>(nal.size() >> 24), static_cast<uint8_t>(nal.size() >> 16), static_cast<uint8_t>(nal.size() >> 8), static_cast<uint8_t>(nal.size())};
    out.insert(out.end(), nal.begin(), nal.end());
    return out;
  };

  check(bx::hash_codec_from_name("h264") == HashCodec::h264 && bx::hash_codec_from_name("hevc") == HashCodec::hevc && bx::hash_codec_from_name("av1") == HashCodec::av1 &&
            bx::hash_codec_from_name("vp9") == HashCodec::other && bx::hash_codec_from_name(nullptr) == HashCodec::other,
        "the codec names the encoder and the container use are known");

  // H.264: an IDR as the encoder hands it (an access unit delimiter, SPS, PPS, SEI, then two slices; mixed start code lengths) and as the mp4 muxer stores it
  // (length prefixes, the parameter sets in the extradata, the delimiter gone).
  const Bytes aud = {0x09, 0xf0};
  const Bytes sps = sized(0x67, 0x42, 20), pps = sized(0x68, 0xce, 4), sei = sized(0x06, 0x55, 30);
  const Bytes slice_a = sized(0x65, 0x11, 4000), slice_b = sized(0x65, 0x22, 3000);
  const Bytes encoder = cat({annexb4(aud), annexb4(sps), annexb4(pps), annexb3(sei), annexb4(slice_a), annexb3(slice_b)});
  const Bytes sample = cat({prefixed(sei), prefixed(slice_a), prefixed(slice_b)});
  const bx::VclDigest from_ring = bx::vcl_digest(HashCodec::h264, encoder.data(), encoder.size(), 0);
  const bx::VclDigest from_chunk = bx::vcl_digest(HashCodec::h264, sample.data(), sample.size(), 4);
  check(from_ring.ok && from_chunk.ok && from_ring.hash == from_chunk.hash && from_ring.bytes == from_chunk.bytes && from_ring.bytes == slice_a.size() + slice_b.size(),
        "H.264: the encoder's packet and the muxed sample hash alike: only the slices count, not the framing, the delimiter, the parameter sets or the SEI");
  Bytes other = sample;
  other[other.size() - 10] ^= 0x01;
  check(bx::vcl_digest(HashCodec::h264, other.data(), other.size(), 4).hash != from_chunk.hash, "H.264: one changed slice byte changes the hash");
  const Bytes swapped = cat({prefixed(slice_b), prefixed(slice_a)});
  check(bx::vcl_digest(HashCodec::h264, swapped.data(), swapped.size(), 4).hash != from_chunk.hash, "H.264: the order of the slices counts");
  const Bytes two_byte = cat({Bytes{0, static_cast<uint8_t>(slice_a.size())}, Bytes{}});
  check(!bx::vcl_digest(HashCodec::h264, two_byte.data(), two_byte.size(), 2).ok, "a length that runs past the packet is not a packet");
  const Bytes empty;
  check(!bx::vcl_digest(HashCodec::h264, empty.data(), 0, 0).ok && !bx::vcl_digest(HashCodec::h264, encoder.data(), 0, 4).ok, "an empty packet is nothing");
  const Bytes only_headers = cat({annexb4(sps), annexb4(pps)});
  check(!bx::vcl_digest(HashCodec::h264, only_headers.data(), only_headers.size(), 0).ok, "a packet with no slice has no picture data");
  const Bytes four_byte_prefix = cat({prefixed(slice_a), prefixed(slice_b)});
  const Bytes encoder_slices = cat({annexb4(slice_a), annexb4(slice_b)});
  check(bx::vcl_digest(HashCodec::h264, four_byte_prefix.data(), four_byte_prefix.size(), 4).hash == bx::vcl_digest(HashCodec::h264, encoder_slices.data(), encoder_slices.size(), 0).hash,
        "and the same slices alone agree as well");

  // HEVC: IDR_W_RADL is type 19 (header byte 0x26), VPS/SPS/PPS 32-34 and SEI 39 are not picture data.
  const Bytes vps = sized(0x40, 0x01, 12), idr = sized(0x26, 0x33, 5000), prefix_sei = sized(0x4e, 0x44, 16);
  const Bytes hevc_encoder = cat({annexb4(vps), annexb4(prefix_sei), annexb4(idr)});
  const Bytes hevc_sample = cat({prefixed(idr)});
  check(bx::vcl_digest(HashCodec::hevc, hevc_encoder.data(), hevc_encoder.size(), 0).hash == bx::vcl_digest(HashCodec::hevc, hevc_sample.data(), hevc_sample.size(), 4).hash &&
            bx::vcl_digest(HashCodec::hevc, hevc_sample.data(), hevc_sample.size(), 4).ok,
        "HEVC: the same slice reads alike framed either way, with the parameter sets and SEI left out");
  const Bytes trailing = cat({annexb4(idr), Bytes{0, 0, 0}});
  check(bx::vcl_digest(HashCodec::hevc, trailing.data(), trailing.size(), 0).hash == bx::vcl_digest(HashCodec::hevc, hevc_sample.data(), hevc_sample.size(), 4).hash,
        "and zero bytes after the last slice are not part of it");

  // AV1: OBUs. The temporal delimiter (type 2) and sequence header (type 1) are not picture data; a frame OBU (type 6) is, and whether its header carries a
  // size field must not matter (the muxer may rewrite that).
  const auto obu = [](int type, const Bytes& payload, bool with_size) {
    Bytes out;
    out.push_back(static_cast<uint8_t>((type << 3) | (with_size ? 2 : 0)));
    if (with_size) {
      size_t value = payload.size();
      do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        out.push_back(value ? static_cast<uint8_t>(byte | 0x80) : byte);
      } while (value);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
  };
  const Bytes frame = sized(0x5a, 0x77, 9000), seq = sized(0x0a, 0x01, 10);
  const Bytes av1_encoder = cat({obu(2, Bytes{}, true), obu(1, seq, true), obu(6, frame, true)});
  const Bytes av1_sample = cat({obu(6, frame, false)});
  check(bx::vcl_digest(HashCodec::av1, av1_encoder.data(), av1_encoder.size(), 0).ok &&
            bx::vcl_digest(HashCodec::av1, av1_encoder.data(), av1_encoder.size(), 0).hash == bx::vcl_digest(HashCodec::av1, av1_sample.data(), av1_sample.size(), 0).hash &&
            bx::vcl_digest(HashCodec::av1, av1_encoder.data(), av1_encoder.size(), 0).bytes == frame.size(),
        "AV1: the frame OBU hashes alike with or without a size field, the delimiter and sequence header left out");
  const Bytes cut = Bytes(av1_encoder.begin(), av1_encoder.end() - 100);
  check(!bx::vcl_digest(HashCodec::av1, cut.data(), cut.size(), 0).ok, "AV1: a frame cut short is not a packet");
  check(!bx::vcl_digest(HashCodec::other, encoder.data(), encoder.size(), 0).ok, "a codec the hash does not know is refused");
  const Bytes aac = {1, 2, 3, 4, 5};
  check(bx::payload_hash(aac.data(), aac.size()) == bx::payload_hash(aac.data(), aac.size()) && bx::payload_hash(aac.data(), aac.size()) != bx::payload_hash(aac.data(), aac.size() - 1),
        "an audio frame is hashed whole");
}

// Where the file may live (pure rules), and the real file on this machine's disk.
void test_ring_disk() {
  bx::SpillFolderFacts facts;
  facts.filesystem = "NTFS";
  facts.free_bytes = 50LL * 1024 * kMiB;
  check(bx::spill_folder_problem(facts, 10LL * 1024 * kMiB).empty(), "an NTFS folder with room is fine");
  facts.filesystem = "ReFS";
  check(bx::spill_folder_problem(facts, kMiB).empty(), "ReFS is fine");
  facts.filesystem = "FAT32";
  check(bx::spill_folder_problem(facts, kMiB).find("FAT32") != std::string::npos, "FAT32 is refused (it zero-fills a reservation and stops at 4 GB)");
  facts.filesystem = "exFAT";
  check(!bx::spill_folder_problem(facts, kMiB).empty(), "so is exFAT, which is untested");
  facts.filesystem = "NTFS";
  facts.network = true;
  check(!bx::spill_folder_problem(facts, kMiB).empty(), "a network drive is refused");
  facts.network = false;
  facts.cloud_synced = true;
  check(bx::spill_folder_problem(facts, kMiB).find("cloud") != std::string::npos, "a cloud-synced folder is refused");
  facts.cloud_synced = false;
  facts.compressed_or_encrypted = true;
  check(!bx::spill_folder_problem(facts, kMiB).empty(), "a compressed or encrypted folder is refused");
  facts.compressed_or_encrypted = false;
  facts.free_bytes = 2LL * 1024 * kMiB;
  check(!bx::spill_folder_problem(facts, 1500 * kMiB).empty() && bx::spill_folder_problem(facts, 1000 * kMiB).empty(),
        "the free space has to cover the file and leave a gigabyte besides");

  const std::vector<std::string> roots = {"C:\\Users\\someone\\OneDrive"};
  check(bx::path_in_cloud_sync("C:\\Users\\someone\\OneDrive\\Videos\\BuffrX", roots) && bx::path_in_cloud_sync("c:/users/someone/onedrive", roots),
        "a path under a sync client's own folder is cloud-synced, whatever its case or slashes");
  check(bx::path_in_cloud_sync("D:\\Backup\\OneDrive - Work\\clips", {}) && bx::path_in_cloud_sync("D:\\Dropbox\\BuffrX", {}) &&
            bx::path_in_cloud_sync("E:\\Google Drive\\x", {}),
        "and so is a folder named like one, even when the variable is unset");
  check(!bx::path_in_cloud_sync("C:\\Users\\someone\\Videos\\BuffrX", roots) && !bx::path_in_cloud_sync("D:\\work\\clips", roots), "an ordinary folder is not");

  char temp[MAX_PATH] = {0};
  GetTempPathA(MAX_PATH, temp);
  const std::string root = std::string(temp) + "buffrx-ringdisk-" + std::to_string(GetCurrentProcessId());
  const std::string folder = root + "\\clips\\.buffrx";
  CreateDirectoryA(root.c_str(), nullptr);
  const bx::SpillPlacement placed = bx::place_spill_file(root + "\\clips", root + "\\data", 24 * kMiB, 4242);
  check(!placed.path.empty() && placed.path == folder + "\\replay-4242.ring", "the file goes in a dot folder beside the clips");
  check((GetFileAttributesA(folder.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0, "and the folder is hidden");

  IO_COUNTERS before{}, after{};
  GetProcessIoCounters(GetCurrentProcess(), &before);
  bx::RingFile file;
  std::string error;
  const bool opened = file.open(placed.path, 3 * bx::kSpillBlockBytes, error);
  GetProcessIoCounters(GetCurrentProcess(), &after);
  check(opened && file.is_open() && file.size() == 3 * bx::kSpillBlockBytes, "a 24 MB file is reserved");
  check(after.WriteTransferCount - before.WriteTransferCount < static_cast<ULONGLONG>(kMiB), "and reserving it wrote nothing");
  check(GetFileAttributesA(placed.path.c_str()) != INVALID_FILE_ATTRIBUTES, "it exists while open");
  bx::RingFile second;
  check(!second.open(placed.path, 3 * bx::kSpillBlockBytes, error), "a second open of the same file is refused: it is held exclusively");
  check(bx::alloc_aligned_block() != nullptr, "an aligned block can be had");

  uint8_t* out = bx::alloc_aligned_block();
  uint8_t* in = bx::alloc_aligned_block();
  check(out && in && reinterpret_cast<uintptr_t>(out) % 4096 == 0, "and it is page aligned");
  bool round_trip = out && in;
  for (int64_t block = 0; round_trip && block < 5; ++block) {  // five blocks into a three-block file: it wraps
    std::memset(out, static_cast<int>(0x30 + block), static_cast<size_t>(bx::kSpillBlockBytes));
    round_trip = file.write_block(block * bx::kSpillBlockBytes, out);
  }
  check(round_trip && file.bytes_written() == 5 * bx::kSpillBlockBytes, "five blocks are written, in order, into a file that holds three");
  bool content = true;
  for (int64_t block = 2; content && block < 5; ++block) {  // the last three are what the file holds now
    content = file.read_block(block * bx::kSpillBlockBytes, in) && in[0] == 0x30 + block && in[bx::kSpillBlockBytes - 1] == 0x30 + block;
  }
  check(content, "the last three read back as written, the first two were overwritten by the wrap");
  file.cancel();
  check(!file.write_block(0, out), "after a cancel no call goes through");
  file.close();
  check(GetFileAttributesA(placed.path.c_str()) == INVALID_FILE_ATTRIBUTES, "closing the handle removes the file: nothing is left behind");
  bx::free_aligned_block(out);
  bx::free_aligned_block(in);

  const std::string stale = folder + "\\replay-1.ring";
  HANDLE leftover = CreateFileA(stale.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (leftover != INVALID_HANDLE_VALUE) CloseHandle(leftover);
  HANDLE live = CreateFileA((folder + "\\replay-2.ring").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bx::remove_stale_ring_files(folder);
  check(GetFileAttributesA(stale.c_str()) == INVALID_FILE_ATTRIBUTES, "a ring file nobody holds open (a power cut left it) is deleted at start");
  check(GetFileAttributesA((folder + "\\replay-2.ring").c_str()) != INVALID_FILE_ATTRIBUTES, "one a live engine holds open is left alone");
  if (live != INVALID_HANDLE_VALUE) CloseHandle(live);
  DeleteFileA((folder + "\\replay-2.ring").c_str());
  RemoveDirectoryA(folder.c_str());
  RemoveDirectoryA((root + "\\clips").c_str());
  RemoveDirectoryA(root.c_str());
}

void test_replay_store_ram_only() {
  // With no disk tier the store is the old ring: same eviction, same save.
  SpillStore store;
  store.configure(10 * 1000000LL, 1LL << 40);
  for (const SpillTestPacket& packet : spill_stream(30, 0)) store.push(packet);
  const bx::StoreInfo info = store.info();
  check(!info.spill && info.disk_bytes == 0 && info.ram_bytes == info.bytes && info.spilled_total == 0 && !info.stalled,
        "without a disk tier nothing is spilled and RAM is all there is");
  check(!store.spill_due() && !store.spill_step(), "and the spill thread has nothing to do");
  bx::SaveReport report;
  const std::vector<SpillOut> clip = spill_collect(store, &report);
  bool in_order = report.ok && !clip.empty();
  for (size_t i = 1; i < clip.size(); ++i) in_order = in_order && clip[i - 1].fields.dts_usec <= clip[i].fields.dts_usec;
  check(in_order && clip.front().fields.dts_usec == 0 && clip.back().fields.dts_usec > 0, "a clip is in time order and starts at zero");
}

// P2-1: the video thread's frame wait sleeps to a margin before the deadline and spins only the margin.
void test_sleep_wait() {
  const int64_t freq = 10000000;  // the usual QueryPerformanceFrequency: one tick is 100 ns
  check(bx::coarse_wait_100ns(69000, freq, 400) == -65000, "6.9 ms to wait with a 0.4 ms margin sleeps 6.5 ms");
  check(bx::coarse_wait_100ns(4000, freq, 400) == 0 && bx::coarse_wait_100ns(3000, freq, 400) == 0, "a wait no longer than the margin is all spin");
  check(bx::coarse_wait_100ns(0, freq, 400) == 0 && bx::coarse_wait_100ns(-5, freq, 400) == 0 && bx::coarse_wait_100ns(10, 0, 400) == 0,
        "no time left, or no clock, is not a sleep");
  check(bx::coarse_wait_100ns(30000000, 3000000, 400) == -((30000000 - 1200) * 10000000LL) / 3000000,
        "another counter frequency converts to 100 ns units");
  check(bx::ticks_from_ns(1000000000ull, 10000000) == 10000000 && bx::ticks_from_ns(6944444, 10000000) == 69444,
        "a nanosecond timestamp becomes counter ticks");
  const uint64_t uptime_ns = 90ull * 24 * 3600 * 1000000000ull;  // 90 days: ns x freq passes 64 bits
  check(bx::ticks_from_ns(uptime_ns, 10000000) == static_cast<int64_t>(90ll * 24 * 3600 * 10000000), "ninety days of uptime does not overflow");

  // The bounded loop: a wait longer than the margin sleeps the rest and spins one margin, a wait inside it spins only itself.
  const auto step = [](int64_t remaining_us) { return bx::plan_sleep_step(remaining_us, 400); };
  check(step(6900).wait_us == 6500 && step(6900).spin_us == 400, "6.9 ms to wait sleeps 6.5 ms and spins 0.4 ms");
  check(step(400).wait_us == 0 && step(400).spin_us == 400, "exactly the margin is all spin, no timer");
  check(step(100).wait_us == 0 && step(100).spin_us == 100, "below the margin spins only what is left");
  check(step(0).wait_us == 0 && step(0).spin_us == 0, "no time left asks for no wait and no spin");
  check(step(-5).wait_us == 0 && step(-5).spin_us == 0, "a passed deadline asks for nothing");
  check(step(1000000).wait_us == 999600 && step(1000000).spin_us == 400, "a long wait is bounded to one margin of spin");
  check(bx::plan_sleep_step(6900, -1).wait_us == 6900 && bx::plan_sleep_step(6900, -1).spin_us == 0,
        "a negative margin never makes a step ask for negative spin");
  check(bx::plan_sleep_step(1500, 0).wait_us == 1500 && bx::plan_sleep_step(1500, 0).spin_us == 0, "no margin is all sleep");
}

// P1-7: a window's executable is read once per (window, process), not once per look.
void test_process_cache() {
  bx::ProcessNameCache cache;
  int reads = 0;
  const auto read = [&](uint32_t pid) { ++reads; return std::string("game") + std::to_string(pid) + ".exe"; };
  check(cache.get(10, 100, read) == "game100.exe" && cache.get(10, 100, read) == "game100.exe" && reads == 1,
        "the same window and process are read once");
  check(cache.get(11, 100, read) == "game100.exe" && reads == 2, "another window of the same process is its own entry");
  check(cache.get(10, 200, read) == "game200.exe" && reads == 3,
        "a window handle that a later window reuses carries its own process and reads again");
  check(cache.get(10, 0, read).empty() && reads == 3, "no process id, no read");
  int failed = 0;
  const auto fails = [&](uint32_t) { ++failed; return std::string(); };
  check(cache.get(20, 300, fails).empty() && cache.get(20, 300, fails).empty() && failed == 2,
        "a process that could not be opened is asked again, never remembered as nothing");
  bx::ProcessNameCache full;
  int fill_reads = 0;
  const auto fill = [&](uint32_t pid) { ++fill_reads; return std::to_string(pid); };
  for (uint32_t i = 1; i <= bx::ProcessNameCache::kLimit + 1; ++i) full.get(i, i, fill);
  const int before = fill_reads;
  full.get(bx::ProcessNameCache::kLimit + 1, bx::ProcessNameCache::kLimit + 1, fill);
  check(fill_reads == before, "the cache is bounded but the newest entry survives the clear");
}

// P1-4: the main loop sleeps until the earliest deadline it owns.
void test_loop_wait() {
  check(bx::ms_until(1000, 800, 250) == 50, "a tick 200 ms into its 250 is due in 50");
  check(bx::ms_until(1100, 800, 250) == 0, "a tick that is past due is due now");
  check(bx::ms_until(100, 0xFFFFFF00u, 250) == 0 && bx::ms_until(10, 0xFFFFFFF0u, 250) == 250 - 26, "the 32-bit tick count wraps");
  check(bx::loop_sleep_ms(1000, 50) == 50 && bx::loop_sleep_ms(20, 50) == 20, "the sleep is the earliest deadline, never past the ceiling");
  check(bx::loop_sleep_ms(1000, 0) == 1, "a due deadline sleeps 1 ms, not in a spin");
}

// P1-8: the warn lines of the log are real faults. Routine FFmpeg notes, a modal drag and its time are not.
void test_log_policy() {
  check(bx::ffmpeg_warning_is_routine("deprecated pixel format used, make sure you did set range correctly"),
        "the deprecated-pixel-format note is routine");
  check(bx::ffmpeg_warning_is_routine("3 frames left in the queue on closing"), "frames left in the queue is routine");
  check(bx::ffmpeg_warning_is_routine("Changing video frame properties on the fly is not supported by all filters."),
        "changing frame properties on the fly is routine");
  check(bx::ffmpeg_warning_is_routine("filter context - w: 2560 h: 1440 fmt: 0 csp: unknown range: unknown"),
        "a filter context line is routine");
  check(!bx::ffmpeg_warning_is_routine("moov atom not found"), "a file that will not read is not routine");
  check(!bx::ffmpeg_warning_is_routine("Could not find codec parameters for stream 0"), "a codec failure is not routine");
  check(bx::is_modal_loop_message(0x00A1) && bx::is_modal_loop_message(0x0112), "a caption click and a system command start a modal loop");
  check(!bx::is_modal_loop_message(0x0100) && !bx::is_modal_loop_message(0x0312), "a key and a hotkey do not");
  check(bx::stall_without_modal(5000, 4800) == 200, "a drag's time is taken off a loop pass");
  check(bx::stall_without_modal(300, 900) == 0, "the modal time can never make a pass negative");
  check(bx::stall_without_modal(1200, 0) == 1200, "a real stall keeps its whole time");
}

// The log keeps what a crash left: a file past 1 MiB is rotated to .1 rather than deleted, and a line
// cut at the buffer says so. Both are read back from the file itself.
void test_log_file() {
  std::printf("log file\n");
  bx::Log& log = bx::Log::instance();
  const std::string previous_path = log.path();
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  const std::string path = temp_directory() + "\\engine.log";
  const std::string rotated = path + ".1";
  DeleteFileA(rotated.c_str());
  {
    FILE* file = std::fopen(path.c_str(), "wb");
    check(file != nullptr, "a full log can be written for the rotation");
    if (file) {
      const std::vector<char> big(1024 * 1024 + 1024, 'x');
      std::fwrite(big.data(), 1, big.size(), file);
      std::fclose(file);
    }
  }
  log.open(path);
  const FileStamp kept = file_stamp(rotated);
  check(kept.bytes.size() == 1024 * 1024 + 1024, "a log past 1 MiB is kept as .1 with every byte, not deleted");
  check(GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES, "and the new run's file starts beside it");

  const std::string long_line(2100, 'y');
  log.write(bx::LogLevel::Note, "%s", long_line.c_str());
  log.open(path);  // closes the handle so the file can be read
  const FileStamp written = file_stamp(path);
  check(written.bytes.find("[truncated]") != std::string::npos, "a line past the buffer says it was cut");
  check(written.bytes.find(long_line) == std::string::npos, "and only the part that fits is written");
  if (!previous_path.empty()) log.open(previous_path);
}

void test_ui_format() {
  using namespace bx::ui;
  using bx::Json;
  // BuffrX links: why a clip did not arrive, in words a person can act on, and how long a link lasts.
  check(link_failure("unreachable").find("may be off") != std::string::npos, "an unreachable sharer reads as their PC being off");
  check(link_failure("expired") == "This link has expired. Ask for a new one.", "an expired link says so");
  check(link_failure("full").find("Too many people") != std::string::npos, "a full link says to try again");
  check(link_failure("refused").find("stopped") != std::string::npos, "a refused link reads as stopped");
  check(link_failure("not_a_clip").find("not a clip") != std::string::npos, "a file that is not a clip says so");
  check(link_failure("no_space").find("space") != std::string::npos, "a full disk says so");
  check(link_failure("gone").find("no longer") != std::string::npos, "a clip deleted on the sharer's PC says so");
  check(link_failure("A sentence the engine wrote.") == "A sentence the engine wrote.", "a sentence passes through");
  check(link_failure("").find("did not arrive") != std::string::npos, "no reason still reads as a sentence");
  check(link_expiry(52 * 60 + 10) == "expires in 52 min" && link_expiry(30) == "expires in 1 min",
        "minutes never overstate the time left, and never read 0");
  check(link_expiry(23 * 3600 + 120) == "expires in 23 h" && link_expiry(2 * 3600) == "expires in 2 h" &&
            link_expiry(60 * 60) == "expires in 1 h",
        "from an hour on, the nearest hour");
  check(link_expiry(0) == "expired" && link_expiry(-5) == "expired", "a past time reads expired");
  // The one line a person can act on while a link is live.
  check(link_advice(false, "", 0) == "nobody has opened it yet", "an untouched link says nobody has opened it");
  check(link_advice(true, "direct", 0) == "direct", "a direct send says direct and nothing else");
  check(link_advice(true, "relay", 0) == "through a relay - slower", "a relayed send says why it is slow");
  check(link_advice(false, "relay", 0) == "last send went through a relay", "a past relay is not called current");
  check(link_advice(true, "direct", 20000).find("capped at 20 Mbit/s") != std::string::npos &&
            link_advice(true, "direct", 20000).find("Settings") != std::string::npos &&
            link_advice(true, "direct", 20000).find("direct") != std::string::npos,
        "a cap the person set is named, with where to change it, beside the route");
  check(link_advice(false, "", 20000).find("nobody has opened it yet") == std::string::npos,
        "a cap alone does not also claim nobody has opened it");
  // The share sheet: what the friend receives, said before it is sent.
  check(share_size_line(150e6, 19.5e6, 720) == format_bytes(150e6) + " \xE2\x86\x92 " + format_bytes(19.5e6) + " \xC2\xB7 720p",
        "the share size line reads before -> after, with the picture");
  check(share_size_line(12e6, 12e6, 1080) == format_bytes(12e6) + " \xC2\xB7 1080p \xC2\xB7 sent as it is",
        "a clip that already fits says it is sent as it is");
  check(share_quality(true, 1.0) == "Original quality", "a copy keeps the original quality");
  check(share_quality(false, 0.9) == "Close to the original", "most of the bits kept reads close");
  check(share_quality(false, 0.4) == "Softer in fast motion", "less than half reads softer");
  check(share_quality(false, 0.1) == "Visibly compressed", "a tenth reads visibly compressed");
  check(format_duration(0) == "0:00" && format_duration(-3) == "0:00", "duration of nothing is 0:00");
  check(format_duration(42.4) == "0:42" && format_duration(62) == "1:02", "duration under an hour");
  check(format_duration(3723) == "1:02:03", "duration over an hour");
  check(format_span(45) == "45s" && format_span(120) == "2m" && format_span(90) == "1m 30s", "span reads as said");
  check(format_ago(0, 100) == "" && format_ago(101, 100) == "0 seconds ago", "missing and future update times are safe");
  check(format_ago(100, 101) == "1 second ago" && format_ago(100, 159) == "59 seconds ago", "update age shows seconds");
  check(format_ago(100, 160) == "1 minute ago" && format_ago(100, 220) == "2 minutes ago", "update age shows minutes");
  check(format_ago(100, 3700) == "1 hour ago" && format_ago(100, 86500) == "1 day ago", "update age shows hours and days");
  check(format_ago(100, 100 + 30 * 86400LL) == "1 month ago" &&
            format_ago(100, 100 + 365 * 86400LL) == "1 year ago", "update age shows months and years");
  check(format_bytes(0) == "0 KB" && format_bytes(10) == "1 KB" && format_bytes(12 * 1024) == "12 KB", "bytes in KB");
  check(format_bytes(34.0 * 1024 * 1024) == "34 MB", "bytes in MB: never mb, which reads as megabits next to Mbps");
  check(format_bytes(1.25 * 1024 * 1024 * 1024) == "1.25 GB" && format_bytes(12.34 * 1024 * 1024 * 1024) == "12.3 GB",
        "bytes in gb: two decimals under 10, one above");

  // Local noon today, an hour before, yesterday and ten days back.
  std::time_t raw = std::time(nullptr);
  std::tm today{};
  localtime_s(&today, &raw);
  today.tm_hour = 12;
  today.tm_min = 0;
  today.tm_sec = 0;
  const int64_t noon = static_cast<int64_t>(std::mktime(&today));
  std::tm yesterday = today;
  yesterday.tm_mday -= 1;
  yesterday.tm_hour = 11;
  std::tm earlier = today;
  earlier.tm_mday -= 10;
  const std::string same_day = format_when(noon - 3600, noon);
  check(!same_day.empty() && same_day.find(':') != std::string::npos, "a time today shows the time of day");
  check(format_when(static_cast<int64_t>(std::mktime(&yesterday)), noon) == "Yesterday", "yesterday says so");
  const std::string older = format_when(static_cast<int64_t>(std::mktime(&earlier)), noon);
  check(older.size() >= 5 && older.find(':') == std::string::npos && older[0] != '0', "older reads as a date (Oct 9)");
  check(format_when(0, noon).empty(), "no time is empty");

  check(short_path("C:\\short") == "C:\\short", "a short path is kept");
  const std::string trimmed = short_path("C:\\Users\\someone\\Videos\\BuffrX\\Very Long Game Name\\Clips", 20);
  check(trimmed.size() <= 22 && trimmed.find("\xE2\x80\xA6") != std::string::npos, "a long path keeps both ends");
  check(short_path("C:\\\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\x83\xE3\x83\x97\\\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\\folder", 12)
                .find("\xEF\xBF\xBD") == std::string::npos,
        "a cut never splits a UTF-8 character");
  check(file_name("C:\\a\\b\\Replay 1.mp4") == "Replay 1.mp4" && file_name("a/b.png") == "b.png", "file name");
  check(file_stem("C:\\a\\Replay 1.mp4") == "Replay 1" && file_stem(".hidden") == ".hidden", "file stem");
  check(quality_label("low") == "Low" && quality_label("highest") == "Highest" && quality_label("custom") == "Custom",
        "quality labels");
  check(quality_label("performance") == "Standard" && quality_label("balanced") == "High",
        "a profile saved with a tier reads as the level it became");
  check(encoder_name("jim_nvenc") == "NVIDIA NVENC \xC2\xB7 H.264" && encoder_name("obs_x264") == "x264 (CPU) \xC2\xB7 H.264",
        "encoder names");
  check(encoder_name("jim_av1_nvenc").find("AV1") != std::string::npos && encoder_name("") == "Encoder starting",
        "encoder codec and empty");

  Json status = Json::object();
  check(status_line(status, false).tone == Tone::busy, "not connected is busy");
  status.set("buffer_state", Json("on"));
  Json ring = Json::object();
  ring.set("filled", Json(42));
  ring.set("seconds", Json(120));
  Json stats = Json::object();
  stats.set("ring", ring);
  status.set("stats", stats);
  const StatusLine on = status_line(status, true);
  check(on.tone == Tone::ok && on.text == "42s of 2m buffered", "buffering line");
  status.set("buffer_state", Json("off"));
  status.set("last_error", Json("the encoder failed"));
  check(status_line(status, true).tone == Tone::bad && status_line(status, true).text == "the encoder failed",
        "an error shows while the buffer is off");
  status.set("last_error", Json(""));
  check(status_line(status, true).text == "Buffer paused", "off is paused");
  status.set("buffer_state", Json("starting"));
  Json capture = Json::object();
  capture.set("mode", Json("game"));
  status.set("capture", capture);
  check(status_line(status, true).text == "Waiting for the game to draw a frame\xE2\x80\xA6", "a game waits for a frame");

  check(rename_problem("Clutch ace").empty(), "a plain name is fine");
  check(!rename_problem("  ").empty() && !rename_problem("a/b").empty() && !rename_problem("end.").empty(),
        "empty, slash and trailing dot are refused");
  check(!rename_problem("CON").empty() && !rename_problem("lpt1").empty() && !rename_problem(std::string(101, 'a')).empty(),
        "reserved names and long names are refused");

  check(keycaps("Ctrl+Alt+1") == std::vector<std::string>({"Ctrl", "Alt", "1"}) && keycaps("").empty(), "keycaps");
  check(default_hotkey("save_replay", false) == "Alt+1" && default_hotkey("toggle_ui", true) == "Pad:Y+VIEW",
        "the default table answers both inputs");
  check(default_hotkey("quick_clip", true).empty() && default_hotkey("nowhere", false).empty(),
        "a pad with no shipped binding, and an unknown action, reset to unbound");
  const bx::Config shipped;
  check(shipped.hotkey_save == default_hotkey("save_replay", false) &&
            shipped.hotkey_quick == default_hotkey("quick_clip", false) &&
            shipped.hotkey_record == default_hotkey("toggle_record", false) &&
            shipped.hotkey_toggle_ui == default_hotkey("toggle_ui", false) &&
            shipped.hotkey_view_clip == default_hotkey("view_clip", false) &&
            shipped.hotkey_marker == default_hotkey("add_marker", false),
        "a fresh config ships the keyboard bindings Settings' Default writes");
  check(shipped.hotkey_save_pad == "Pad:LB+RB" && shipped.hotkey_screenshot_pad == "Pad:LS+RS" &&
            shipped.hotkey_record_pad == "Pad:VIEW+MENU" && shipped.hotkey_toggle_ui_pad == "Pad:Y+VIEW",
        "a fresh config ships the pad bindings the guide names");
  bx::Config tracks;
  check(bx::audio_track_count(tracks) == 1, "one track by default");
  tracks.separate_tracks = true;
  check(bx::audio_track_count(tracks) == 3, "the old separate-tracks alias means three");
  tracks.audio_tracks = 4;
  check(bx::audio_track_count(tracks) == 4, "an explicit count wins over the alias");
  tracks.separate_tracks = false;
  tracks.audio_tracks = 9;
  check(bx::audio_track_count(tracks) == 1, "a count outside 1..6 falls back to one");
  const std::wstring wide = bx::wide_path("C:/clips/\xC3\xA9.mp4");
  check(wide.size() == 14 && wide[9] == L'\u00e9', "a UTF-8 path becomes Windows' own text");
  const ChordCapture modifier_only = capture_chord(true, false, false, false, VK_CONTROL);
  check(modifier_only.waiting, "a modifier alone keeps waiting");
  const ChordCapture alt_one = capture_chord(false, true, false, false, '1');
  check(!alt_one.waiting && alt_one.chord == "Alt+1" && alt_one.problem.empty(), "Alt+1 is captured");
  const ChordCapture bare = capture_chord(false, false, false, false, 'K');
  check(bare.chord == "K" && !bare.problem.empty(), "a bare key is refused with a reason");
  check(capture_chord(true, true, false, false, '1').problem.empty(), "Ctrl+Alt+1 is three parts and allowed");
  const ChordCapture too_many = capture_chord(true, true, true, false, '1');
  check(too_many.chord == "Ctrl+Alt+Shift+1" && !too_many.problem.empty(),
        "a fourth part is refused with a reason");
  check(capture_chord(false, false, false, false, VK_F13).problem.empty(), "F13 alone is allowed");
  const ChordCapture numpad = capture_chord(true, false, true, false, VK_NUMPAD3);
  UINT modifiers = 0, key = 0;
  std::string parse_problem;
  check(numpad.chord == "Ctrl+Shift+Num3" && bx::parse_chord(numpad.chord, modifiers, key, parse_problem) &&
            key == VK_NUMPAD3,
        "a numpad chord round-trips through the engine's parser");

  check(limit_chips(60) == std::vector<int>({30, 60, 120, 300}) && limit_chips(90) == std::vector<int>({30, 60, 90, 120, 300}),
        "buffer chips add the current value");
  check(clamp_buffer_seconds(1) == 5 && clamp_buffer_seconds(9999) == 1800 && clamp_buffer_seconds(900) == 900 &&
            clamp_buffer_seconds(75) == 75,
        "buffer clamp agrees with the config's own 5..1800");
  check(with_current({10, 20}, 15) == std::vector<int>({10, 15, 20}) && with_current({10, 20}, 20) == std::vector<int>({10, 20}),
        "a select keeps the current value");
  int width = 0, height = 0;
  resolution_for_height(1080, width, height);
  check(width == 1920 && height == 1080, "1080p is 1920x1080");
  resolution_for_height(1440, width, height);
  check(width == 2560 && height == 1440, "1440p is 2560x1440");

  // What the encoder page says about a rate. The regression this guards: the page showed the user's
  // own bitrate slider value and nothing else, so choosing AV1 looked like it changed nothing.
  check(bx::ui::format_bitrate(8018) == "8.0 Mbps", "a rate reads in Mbps above 1000");
  check(bx::ui::format_bitrate(500) == "500 kbps", "a sub-megabit rate reads in kbps, not 0.5 Mbps");
  check(bx::ui::format_bitrate(0) == "0 kbps", "no rate is not a negative number");
  check(bx::ui::format_bitrate(-5) == "0 kbps", "a nonsense rate is clamped rather than printed");

  // The note under the row is the reason to pick the codec at all.
  check(bx::ui::codec_size_note("h264", 1920, 1080, 60, "balanced").empty(),
        "H.264 is the baseline and has nothing to compare against");
  const std::string av1_note = bx::ui::codec_size_note("av1", 1920, 1080, 60, "balanced");
  check(av1_note.find("52% smaller") != std::string::npos, "AV1 is quoted against H.264 at the same quality");
  check(av1_note.find("8.7 Mbps") != std::string::npos, "the note carries the rate that will be used");
  check(av1_note.find("18.0 Mbps") != std::string::npos, "and the one H.264 would have needed");
  const std::string hevc_note = bx::ui::codec_size_note("hevc", 1920, 1080, 60, "balanced");
  check(hevc_note.find("28% smaller") != std::string::npos, "HEVC saves less than AV1 and says so");
  // The saving is a share of H.264, so it narrows as the tier rises: the same codec is a smaller
  // share of a bigger rate.
  check(bx::ui::codec_size_note("av1", 1920, 1080, 60, "quality").find("52%") != std::string::npos,
        "the saving is a share of the baseline, so it holds across tiers");

  // The Timeline in the native interface (Task 16): its own row, its four lengths, what it keeps, and the
  // Record key's label while a session writes.
  check(std::string(bx::ui::timeline_title()) == "TIMELINE", "the settings row is TIMELINE");
  check(bx::ui::timeline_length_choices() == std::vector<std::string>({"OFF", "30 MIN", "1 H", "2 H"}),
        "the four lengths read OFF, 30 MIN, 1 H, 2 H");
  check(bx::ui::timeline_length_label(0) == "OFF" && bx::ui::timeline_length_label(30) == "30 MIN" &&
            bx::ui::timeline_length_label(60) == "1 H" && bx::ui::timeline_length_label(120) == "2 H",
        "a length names itself the way the engine allows it");
  check(bx::ui::timeline_length_minutes(0) == 0 && bx::ui::timeline_length_minutes(1) == 30 &&
            bx::ui::timeline_length_minutes(2) == 60 && bx::ui::timeline_length_minutes(3) == 120,
        "the chips are the minutes the engine offers");
  check(bx::ui::timeline_length_index(60) == 2 && bx::ui::timeline_length_index(0) == 0,
        "a setting picks its chip");
  check(bx::ui::timeline_length_index(45) == 0, "a length the engine would refuse reads as OFF, not as the nearest chip");
  const std::string timeline_note = bx::ui::timeline_disclosure();
  check(timeline_note.find("on disk") != std::string::npos, "the disclosure says the footage is on disk");
  check(timeline_note.find("microphone") != std::string::npos && timeline_note.find("muted") != std::string::npos,
        "and that it includes the microphone while it is not muted");
  check(timeline_note.find("stays") != std::string::npos,
        "and never promises exact deletion while the engine is closed");
  {
    using bx::ui::timeline_status;
    check(timeline_status("writing", "", true, 60).headline == "Recording the desktop", "no game: the desktop is what is recording");
    check(timeline_status("writing", "cs2.exe", false, 60).headline == "Recording cs2.exe", "a game names itself");
    check(timeline_status("writing", "", true, 60).tone == 1 && timeline_status("paused_idle", "", true, 60).tone == 2, "recording and paused read differently");
    check(timeline_status("paused_no_game", "", false, 30).detail.find("Everywhere") != std::string::npos, "games-only waiting says how to record the desktop");
    check(timeline_status("paused_no_game", "", true, 30).detail.find("Everywhere") == std::string::npos, "everywhere does not tell you to choose it");
    check(timeline_status("off", "", false, 0).headline == "Timeline is off" && timeline_status("stopped", "", false, 30).headline == "Stopped", "off and stopped are named");
    check(!timeline_status("paused_disk", "", false, 30).detail.empty(), "any other pause says why");
    check(timeline_status("paused_disk", "", false, 30).detail.rfind("Paused", 0) != 0 && timeline_status("stopped", "", false, 30).detail.rfind("Stopped", 0) != 0, "the detail does not repeat the headline");
    check(bx::ui::timeline_idle_label(0) == "Never" && bx::ui::timeline_idle_label(15) == "15 min", "idle choices are worded");
    check(bx::ui::timeline_idle_choices().size() == 5, "idle has five choices");
    check(!bx::ui::timeline_state_note("paused_idle").empty(), "idle has a note");
  }
  check(!bx::ui::timeline_state_note("writing").empty() && !bx::ui::timeline_state_note("paused_disk").empty(),
        "every state the engine reports has a line");
  check(bx::ui::record_label(false) == "RECORD", "with no recording the Record key records");
  check(bx::ui::record_label(true) == "STOP RECORDING", "a manual recording keeps its Stop label");
  // The glyph and the hint follow the label's rule: the red dot is only for a recording that is about to start.
  check(bx::ui::record_icon(false) == "record", "a recording to come has the red dot");
  check(bx::ui::record_icon(true) == "stop", "the stop icon appears only for an active recording");
  check(bx::ui::record_hint(false).find("recording") != std::string::npos, "the record row says it records");
  check(bx::ui::record_hint(true).find("Stops") == 0, "the running row says it stops");
}

// How steadily the interface draws (ui_frame_stats.h): the gaps between frames, the delay from an input to the frame that
// shows it, and the rule that lets a paced window draw.
// The hang detector's only source of truth about who holds the engine lock: a deadlock must name its
// command while it is still happening, so the reading has to be live, not what was held last.
void test_lock_watch() {
  std::printf("engine lock watch\n");
  bx::LockWatch watch;
  check(watch.describe() == "free", "an unheld engine lock reads free");
  {
    bx::LockWatch::Scope scope(watch, "game.state");
    check(watch.describe().rfind("game.state for ", 0) == 0, "a held engine lock names its command");
  }
  check(watch.describe() == "free", "the scope releasing the lock reads free again");
  watch.hold("buffer.save");
  check(watch.describe().rfind("buffer.save for ", 0) == 0, "a held lock names the newest command");
  watch.hold("clips.list");
  check(watch.describe().rfind("clips.list for ", 0) == 0, "a second hold replaces the command");
  watch.release();
  check(watch.describe() == "free", "release clears the command");
}

// 4A: the motion tokens are one table, held in the native interface (ui_anim_math.h) and in the editor's stylesheet
// (ui/src/motion.css): the same durations, and curves whose cubic-bezier samples match the interface's ease_out and ease_in.
// A drift in either fails here, as test_themes does for the colours.
void test_motion() {
  using namespace bx::ui;
#ifdef BUFFRX_SOURCE_DIR
  std::string css;
  if (FILE* file = std::fopen((source_root() + "/ui/src/motion.css").c_str(), "rb")) {
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, file)) > 0) css.append(chunk, n);
    std::fclose(file);
  }
  check(!css.empty(), "the editor page carries its motion tokens in ui/src/motion.css");
  // The first declaration of a name: the reduced-motion block repeats some names, and it must not stand in for the tokens.
  const auto value_of = [&](const std::string& name) {
    const size_t at = css.find(name + ":");
    if (at == std::string::npos) return std::string();
    const size_t end = css.find(';', at);
    return css.substr(at + name.size() + 1, end == std::string::npos ? std::string::npos : end - at - name.size() - 1);
  };
  const auto ms_of = [&](const char* name) { return std::strtod(value_of(name).c_str(), nullptr); };
  check(std::fabs(ms_of("--bx-dur-fast") - kDurFast * 1000.0) < 0.5, "the page's fast token is the interface's 90 ms");
  check(std::fabs(ms_of("--bx-dur-base") - kDurBase * 1000.0) < 0.5, "the page's base token is the interface's 140 ms");
  check(std::fabs(ms_of("--bx-dur-slow") - kDurSlow * 1000.0) < 0.5, "the page's slow token is the interface's 220 ms");
  check(css.find("prefers-reduced-motion: reduce") != std::string::npos, "the page drops every motion to 0.01 ms under reduced motion");

  // A cubic-bezier's four numbers, read from the declaration, then y at x by bisection on the curve's parameter.
  const auto bezier_of = [&](const char* name, double out[4]) {
    const std::string v = value_of(name);
    const size_t open = v.find('(');
    if (open == std::string::npos) return false;
    const char* p = v.c_str() + open + 1;
    for (int i = 0; i < 4; ++i) {
      char* end = nullptr;
      out[i] = std::strtod(p, &end);
      if (end == p) return false;
      p = end;
      while (*p == ',' || *p == ' ') ++p;
    }
    return true;
  };
  const auto curve_y = [](const double c[4], double x) {
    double lo = 0, hi = 1, s = 0.5;
    for (int i = 0; i < 60; ++i) {
      s = (lo + hi) / 2;
      const double bx = 3 * (1 - s) * (1 - s) * s * c[0] + 3 * (1 - s) * s * s * c[2] + s * s * s;
      (bx < x ? lo : hi) = s;
    }
    return 3 * (1 - s) * (1 - s) * s * c[1] + 3 * (1 - s) * s * s * c[3] + s * s * s;
  };
  double out_curve[4] = {}, in_curve[4] = {};
  check(bezier_of("--bx-ease-out", out_curve) && bezier_of("--bx-ease-in", in_curve), "the page declares its enter and exit curves as cubic-bezier");
  bool out_matches = true, in_matches = true;
  for (int i = 1; i <= 9; ++i) {
    const float t = static_cast<float>(i) / 10.0f;
    // The CSS curves are the usual easeOutCubic and easeInCubic approximations: they differ from the cubic math by up to
    // about 0.03 of the progress, which is under 5 ms of a 140 ms move (a third of a frame at 60 Hz).
    out_matches = out_matches && std::fabs(curve_y(out_curve, t) - ease_out(t)) < 0.03;
    in_matches = in_matches && std::fabs(curve_y(in_curve, t) - ease_in(t)) < 0.03;
  }
  check(out_matches, "the page's enter curve samples to the interface's ease_out (1 - (1 - t)^3) within 0.03 of progress");
  check(in_matches, "the page's exit curve samples to the interface's ease_in (t^3) within 0.03 of progress");
#endif
}

void test_frame_stats() {
  using namespace bx::ui;
  FrameStats empty;
  check(empty.summary().frames == 0 && empty.summary().inputs == 0 && empty.summary().max_ms == 0, "no frames, no numbers");

  FrameStats steady;
  for (int i = 0; i < 11; ++i) steady.present(i * 8000);  // 125 fps
  FrameSummary summary = steady.summary();
  check(summary.frames == 10 && std::fabs(summary.avg_ms - 8.0) < 1e-9 && summary.max_ms == 8.0, "ten 8 ms gaps read 8 ms");

  FrameStats uneven;
  uneven.present(0);
  for (int i = 1; i <= 19; ++i) uneven.present(i * 10000);
  uneven.present(19 * 10000 + 40000);  // one slow frame in twenty
  summary = uneven.summary();
  check(summary.frames == 20 && summary.max_ms == 40.0, "the slow frame is the maximum");
  check(summary.p95_ms == 10.0, "one slow frame in twenty does not move the 95th percentile");

  FrameStats paused;
  paused.present(0);
  paused.present(10000);
  paused.present(10000 + FrameStats::kIdleGapUs + 1);  // the window went idle
  paused.present(10000 + FrameStats::kIdleGapUs + 1 + 10000);
  summary = paused.summary();
  check(summary.frames == 2 && summary.max_ms == 10.0, "a silence is idle time, not a slow frame");

  FrameStats latency;
  latency.present(0);
  latency.input(2000, InputKind::pointer);
  latency.input(5000, InputKind::pointer);  // the second input waited less: the first one counts
  latency.present(9000);
  latency.present(18000);  // no input waiting: no delay recorded
  summary = latency.summary();
  check(summary.inputs == 1 && summary.latency_max_ms == 7.0, "the delay runs from the first input waiting to the frame that shows it");

  latency.reset();
  check(latency.summary().frames == 0 && latency.summary().inputs == 0, "reset forgets everything");

  FrameStats long_run;
  for (int i = 0; i < 2000; ++i) long_run.present(i * 5000);
  check(long_run.summary().frames == static_cast<int>(FrameStats::kKeep), "only the latest frames are kept");

  check(frame_allowed(false, false, 0), "a window that is not paced always draws");
  check(frame_allowed(true, true, 0), "a paced window draws when the display is ready");
  check(!frame_allowed(true, false, 4000), "a paced window waits while the display is not ready");
  check(frame_allowed(true, false, kPacingGiveUpUs + 1), "a display that never answers does not freeze the picture");
  check(asked_frame_due(false, 0), "with no game running an asked-for frame is never held back");
  check(!asked_frame_due(true, 6000), "while a fullscreen game runs an asked-for frame waits like a slow one");
  check(asked_frame_due(true, kSlowFrameGapUs), "and comes about 60 times a second");
  check(!slow_frame_due(kSlowFrameGapUs - 1) && slow_frame_due(kSlowFrameGapUs), "a slow frame is due every 16 ms");

  // A skipped frame still answers the input that asked for it: the delay is counted, not left to grow until the next presented frame.
  FrameStats answered;
  answered.present(0);
  answered.input(1000000, InputKind::pointer);
  answered.skipped(1004000);
  answered.present(1500000);
  check(answered.summary().inputs == 1 && answered.summary().latency_max_ms == 4.0, "an input answered by a skipped frame has its own 4 ms delay");

  // N0-5: the window-open time runs from the show request to the frame that first appears, and only an open measures one.
  FrameStats opening;
  opening.opened(1000000);
  opening.present(1004250);
  check(opening.open_ms() == 4.25, "a window open reads from its show request to its first frame");

  // 0.2: every input is also kept under its kind (pointer, click, key, wheel, pad), so a click is not hidden behind the pointer.
  FrameStats kinds;
  kinds.present(0);
  kinds.input(1000000, InputKind::pointer);
  kinds.input(1002000, InputKind::click);  // another kind waiting for the same frame keeps its own delay
  kinds.present(1005000);
  check(kinds.summary(InputKind::pointer).inputs == 1 && kinds.summary(InputKind::pointer).latency_max_ms == 5.0,
        "a pointer input's delay is kept under the pointer");
  check(kinds.summary(InputKind::click).inputs == 1 && kinds.summary(InputKind::click).latency_max_ms == 3.0,
        "a click that waited for the same frame keeps its own delay");
  check(kinds.summary(InputKind::key).inputs == 0, "a kind that waited for nothing counts nothing");
  check(kinds.summary().inputs == 1 && kinds.summary().latency_max_ms == 5.0, "the mixed figure still runs from the first input waiting");
  check(std::string(input_kind_name(InputKind::pad)) == "pad" && std::string(input_kind_name(InputKind::wheel)) == "wheel",
        "each kind has the name ui.frame_stats reports");
  check(ui_input_kind(true, false, false) == InputKind::click && ui_input_kind(false, true, false) == InputKind::key &&
            ui_input_kind(false, false, true) == InputKind::wheel && ui_input_kind(false, false, false) == InputKind::pointer,
        "an injected ui.input is classed by what it carries: a click, a key, a wheel notch, or a bare move");
  opening.present(2000000);
  check(opening.open_ms() == 4.25, "a later frame does not overwrite the open time");
  opening.opened(3000000);
  check(opening.open_ms() == 4.25, "a request whose frame has not come keeps the last measured open");
  opening.present(3015000);
  check(opening.open_ms() == 15.0, "the newest open replaces it");
  opening.reset();
  check(opening.open_ms() == 0, "reset forgets the open time");

  // P1-2: a frame whose draw data equals the last presented one is not presented again. The digest folds bytes in order.
  {
    const unsigned char a[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    unsigned char b[13];
    std::memcpy(b, a, sizeof(a));
    const uint64_t da = digest_bytes(kDigestSeed, a, sizeof(a));
    check(da == digest_bytes(kDigestSeed, b, sizeof(b)), "the same bytes give the same digest");
    b[12] ^= 1;
    check(da != digest_bytes(kDigestSeed, b, sizeof(b)), "a changed last byte (past the last whole word) changes it");
    b[12] ^= 1;
    b[0] ^= 0x80;
    check(da != digest_bytes(kDigestSeed, b, sizeof(b)), "a changed first byte changes it");
    check(digest_bytes(kDigestSeed, a, 12) != digest_bytes(kDigestSeed, a, 13), "a shorter run is not the same run");
    check(digest_bytes(digest_bytes(kDigestSeed, a, 8), a + 8, 5) == da, "folding a run in two parts gives the digest of the whole");
    check(digest_bytes(kDigestSeed, a, 0) == kDigestSeed, "nothing folded leaves the digest alone");
  }
  // The rule that skips a frame: only a valid digest equal to the one on screen, never while a window opens or is dragged, and an
  // animation that is not the pointer's is always presented (its pacing comes from the present).
  check(frame_is_repeat(true, 7, 7, false, false, false), "the same picture is not presented again");
  check(!frame_is_repeat(true, 7, 8, false, false, false), "a changed picture is presented");
  check(!frame_is_repeat(false, 7, 7, false, false, false),
        "a digest made invalid (a show, a resize, a thumbnail texture made or freed) always presents the next frame");
  check(!frame_is_repeat(true, 7, 7, true, false, false), "a window's first frame after it opens is presented");
  check(!frame_is_repeat(true, 7, 7, false, true, false), "a frame during a drag or resize is presented");
  check(!frame_is_repeat(true, 7, 7, false, false, true), "a moving animation is presented even when it looks the same");
  // 1.1: a moving value is presented even when a pointer asked for it. A skipped frame does not take the display's slot, so the
  // loop would come round at once with the value still moving: a spin for as long as the tween runs.
  check(!frame_is_repeat(true, 7, 7, false, false, true), "a pointer's moving hover is presented, not skipped, so it keeps the display's pace");
  check(frame_is_repeat(true, 7, 7, false, false, false), "a pointer move that changed nothing and moves nothing is skipped");

  // 1.1/1.3/1.7: the focused window follows the display; an unfocused one draws at most 30 a second and a pointer move alone does not wake it.
  check(window_frame_due(true, false, false, 0) && !window_frame_due(false, false, false, 16000), "focused draws at once, unfocused waits 33 ms");
  check(window_frame_due(false, true, false, 0) && !window_frame_due(true, false, true, 6000), "a forced capture always draws; a game holds only the unfocused window");
  check(!pointer_move_wakes(true, false) && pointer_move_wakes(true, true) && pointer_move_wakes(false, false), "a bare pointer move does not wake an unfocused window");

  // 1.2: one settle frame replaces the tail. ImGui shows a tooltip after the pointer is still for HoverStationaryDelay (0.15 s)
  // plus HoverDelayNormal (0.40 s), so the settle frame must come after that; 0 means none is waiting.
  check(kSettleUs >= 550000, "the settle frame comes after the longest tooltip delay (0.15 s still plus 0.40 s)");
  check(!settle_due(0, 5000) && !settle_due(1000, 999), "no settle frame is due with none waiting or before its time");
  check(settle_due(1000, 1000) && settle_due(1000, 2000), "a settle frame is due at its time and after it, until it is drawn");

  // P1-1: an animation names how often it needs a frame instead of drawing at 62 a second for as long as it holds.
  // 1.6: the clock is microseconds (QPC), so a 33 ms spinner and a 500 ms blink are exact.
  check(!animation_frame_due(33, false, 1020000, 1000000) && animation_frame_due(33, false, 1033000, 1000000), "a spinner is due 33 ms after its last frame");
  check(!animation_frame_due(500, true, 1200000, 1000000) && animation_frame_due(500, true, 1500000, 1499000),
        "an aligned animation is due when a multiple of its period has passed since the last frame, not before");
  check(!animation_frame_due(500, true, 1999000, 1500000) && animation_frame_due(500, true, 2000000, 1999000), "the blink edge is the 500 ms beat");
  check(!animation_frame_due(33, false, 1032999, 1000000), "a frame 32.999 ms after the last is not yet due: the clock is not 16 ms coarse");

  // P1-1/P1-2 instrumentation: every presented frame is counted (a silence over 100 ms is no gap, so a window drawing twice a
  // second shows no gaps at all), with what the frame took to build and why it was drawn.
  FrameStats counted;
  counted.present(0);
  counted.present(500000);  // 2 frames a second: no gap is counted
  counted.present(1000000);
  check(counted.summary().frames == 0 && counted.presented() == 3, "slow frames are counted as presented even though they have no gap");
  counted.built(800);
  counted.built(1200);
  counted.built(400);
  check(counted.builds() == 3 && counted.build_avg_us() == 800 && counted.build_max_us() == 1200, "the build time is averaged and its worst kept");
  counted.skipped(1100000);
  counted.skipped(1200000);
  check(counted.skipped_frames() == 2 && counted.presented() == 3, "a skipped frame is counted apart from the presented ones");
  counted.reason("store");
  counted.reason("store");
  counted.reason("anim:recording");
  check(counted.reasons().at("store") == 2 && counted.reasons().at("anim:recording") == 1, "each reason a frame was drawn for is counted");
  counted.reset();
  check(counted.presented() == 0 && counted.skipped_frames() == 0 && counted.builds() == 0 && counted.reasons().empty() && counted.build_avg_us() == 0, "reset forgets the counters too");
}

// The panel's live timeline and recent actions (ui_live_math.h).
void test_live_math() {
  using namespace bx::ui;
  check(nice_step_ms(1) == 1000 && nice_step_ms(1000) == 1000 && nice_step_ms(1001) == 2000 && nice_step_ms(40000) == 60000 &&
            nice_step_ms(99999999) == 3600000,
        "a ruler step is the first readable step at least as long as asked");
  check(tick_step_ms(30000, 400, 64) == 5000, "30 s on 400 px with labels 64 px apart: one label every 5 s");
  check(tick_step_ms(30000, 0, 64) == 30000, "a strip with no width asks for one label");
  const std::vector<int64_t> slots = strip_slots(12500, 30000, 5000);
  check(slots.size() == 4 && slots.front() == 10000 && slots.back() == 25000, "slots cover the view on a grid from the session's start");
  check(strip_slots(12900, 30400, 5000) == std::vector<int64_t>({10000, 15000, 20000, 25000, 30000}),
        "a view that slid on keeps every slot it had and gains only the new one");
  check(strip_slots(-4000, 3000, 5000) == std::vector<int64_t>({0}), "no slot before the session began");
  check(strip_slots(0, 0, 5000).empty() && strip_slots(0, 10, 0).empty(), "an empty view or grid has no slots");
  check(zoom_span(30000, 1, 600000) == 60000 && zoom_span(30000, -1, 600000) == 15000, "the wheel steps one span out or in");
  check(zoom_span(60000, 1, 40000) == 60000, "never wider than the first span that shows all the footage");
  check(zoom_span(15000, -1, 600000) == 15000, "the narrowest span stays");
  check(zoom_span(7200000, 1, 99999999) == 7200000, "the widest span stays");
  LiveRange view = view_window(100000, 30000, 0, 100000);
  check(view.start_ms == 70000 && view.end_ms == 100000, "the view ends at the live edge");
  view = view_window(10000, 30000, 0, 100000);
  check(view.start_ms == 0 && view.end_ms == 30000, "a view panned back stops at the first footage");
  view = view_window(100000, 30000, 0, 12000);
  check(view.start_ms == -18000 && view.end_ms == 12000, "footage shorter than the view sits at its right, with the live edge");
  LiveRange range = clamp_range(50, 10, 0, 100000, 1000);
  check(range.start_ms == 10 && range.end_ms == 1010, "a reversed or short range is turned round and grown forward");
  range = clamp_range(99900, 99950, 0, 100000, 1000);
  check(range.start_ms == 99000 && range.end_ms == 100000, "a short range at the end grows back");
  range = clamp_range(-5, 200000, 0, 100000, 1000);
  check(range.start_ms == 0 && range.end_ms == 100000, "a range is kept inside the footage");
  range = clamp_range(10, 20, 0, 500, 1000);
  check(range.start_ms == 0 && range.end_ms == 500, "footage shorter than the shortest range is all of it");
  range = move_edge({10000, 20000}, true, 19800, 0, 100000, 1000);
  check(range.start_ms == 19000 && range.end_ms == 20000, "a start dragged past the end stops a second short");
  range = move_edge({10000, 20000}, false, 300000, 0, 100000, 1000);
  check(range.start_ms == 10000 && range.end_ms == 100000, "an end dragged past the footage stops at its end");
  range = shift_range({10000, 20000}, 95000, 0, 100000);
  check(range.start_ms == 90000 && range.end_ms == 100000, "a moved range keeps its length at the footage's end");
  range = shift_range({10000, 20000}, -95000, 0, 100000);
  check(range.start_ms == 0 && range.end_ms == 10000, "and at its start");
  check(live_edge_ms(1000000 * 1000LL + 30000, 1000000, 10000) == 28500, "the live edge is the wall clock, held 1.5 s back");
  check(live_edge_ms(1000000 * 1000LL + 1000, 1000000, 10000) == 10000, "never before the footage already published");

  auto media = [](const char* name, const char* folder, int64_t mtime, double duration, bool clip = true) {
    UiMedia item;
    item.name = name;
    item.folder = folder;
    item.file = std::string(folder) + "\\" + name;
    item.mtime = mtime;
    item.duration = duration;
    item.clip = clip;
    return item;
  };
  check(clip_action_kind(media("Replay 2026-10-20 10-00-00.mp4", "Display Capture", 0, 0)) == ActionKind::clip, "a replay is a clip save");
  check(clip_action_kind(media("BuffrX 2026-10-20 10-00-00.mp4", "", 0, 0)) == ActionKind::timeline_clip, "timeline.save's name is a Timeline save");
  check(clip_action_kind(media("Recording 2026-10-20.mp4", "Recordings", 0, 0)) == ActionKind::record_stop, "a recording is its stop");
  check(clip_action_kind(media("Replay x.mp4", "Edits", 0, 0)) == ActionKind::edit, "the Edits folder is an edit");
  check(clip_action_kind(media("Clip x.mp4", "Shared with me", 0, 0)) == ActionKind::received, "Shared with me is a clip received");
  check(shared_item(media("Clip x.mp4", "Shared with me", 0, 0)) && !shared_item(media("Clip y.mp4", "Game", 0, 0)) &&
            !shared_item(media("clip z.mp4", "", 0, 0, false)),
        "only the Shared with me folder is a received file, clip or screenshot");
  const std::vector<UiMedia> clips = {media("Replay a.mp4", "Game", 500, 20), media("Recording b.mp4", "Recordings", 400, 90),
                                      media("BuffrX c.mp4", "", 100, 25)};
  const std::vector<UiMedia> shots = {media("Screenshot d.png", "Game", 450, 0, false)};
  std::vector<RecentAction> recent = recent_actions(clips, shots, 0, 10);
  check(recent.size() == 5, "every save is a row, and a recording is two");
  check(recent[0].when == 500 && recent[0].kind == ActionKind::clip && recent[1].kind == ActionKind::screenshot &&
            recent[2].kind == ActionKind::record_stop && recent[3].kind == ActionKind::record_start && recent[3].when == 310 &&
            recent[4].kind == ActionKind::timeline_clip,
        "newest first, a recording's start its end less its length");
  check(!recent[1].clip && recent[1].file == "Game\\Screenshot d.png", "a screenshot row opens the screenshot");
  recent = recent_actions(clips, shots, 600, 3);
  check(recent.size() == 3 && recent[0].kind == ActionKind::record_start && recent[0].file.empty() && recent[0].when == 600,
        "a recording still running is the newest row, with no file yet, and the list stops at its limit");
  recent = recent_actions({media("Recording e.mp4", "Recordings", 300, 0)}, {}, 0, 10);
  check(recent.size() == 1 && recent[0].kind == ActionKind::record_stop, "a recording of unknown length is only its stop");
  recent = recent_actions({media("Recording f.mp4", "Recordings", 300, 0.4)}, {}, 0, 10);
  check(recent.size() == 2 && recent[0].kind == ActionKind::record_stop, "a start and its stop in one second: the stop is newer");
}

// The rail's LIVE LINK pane (ui_link_math.h): which link is newest, how much of its life is left, and the two-press stop.
void test_link_math() {
  using namespace bx::ui;
  const auto link = [](const std::string& id, int64_t expires, int viewers = 0, int watching = 0, int64_t created = 0) {
    return LinkView{id, "file-" + id, "name-" + id, "url-" + id, expires, viewers, watching, created};
  };
  LinkBook book;
  std::vector<LinkView> links = {link("old", 100), link("new", 200)};
  check(link_book_update(book, links, 0), "the first list of links is all new");
  check(book.ids == std::vector<std::string>({"old", "new"}) && book.full_seconds == std::vector<int64_t>({100, 200}),
        "same-frame links are first seen oldest expiry first, and their full life is remembered");
  std::vector<size_t> order = live_link_order(links, book, 0);
  check(order.size() == 2 && links[order[0]].id == "new" && links[order[1]].id == "old",
        "the newest link is the one first seen last, and the pane shows it first");
  check(!link_book_update(book, links, 50), "a known list adds nothing");
  check(link_full_seconds(book, "new") == 200 && link_life_share(200, 200, 50) == 0.75f,
        "the full life stays the first one seen, and the share falls with the clock");
  check(link_life_share(100, 100, 0) == 1.0f && link_life_share(100, 100, 50) == 0.5f &&
            link_life_share(100, 100, 100) == 0.0f && link_life_share(100, 100, 101) == 0.0f,
        "a link starts full, falls by half, and stops at nothing");
  links = {link("new", 200)};
  link_book_update(book, links, 50);
  check(book.ids == std::vector<std::string>({"new"}), "a link that is gone is forgotten");
  links = {link("new", 200), link("newer", 300)};
  check(link_book_update(book, links, 60) && book.ids == std::vector<std::string>({"new", "newer"}),
        "a link that appears later is appended as the newest");
  order = live_link_order(links, book, 60);
  check(order.size() == 2 && links[order[0]].id == "newer", "the newest link is shown first");
  order = live_link_order(links, book, 200);
  check(order.size() == 1 && links[order[0]].id == "newer", "an expired link is hidden before the engine reports it gone");
  check(live_link_order(links, book, 300).empty(), "an expired list has no pane");
  check(link_activity(2, 7) == "2 watching" && link_activity(1, 0) == "1 watching",
        "watching wins over finished downloads");
  check(link_activity(0, 3) == "3 downloads" && link_activity(0, 1) == "1 download" && link_activity(0, 0) == "0 downloads",
        "with nobody watching, downloads are counted and singular");
  check(link_step(0, 3, 1) == 1 && link_step(0, 3, -1) == 2 && link_step(2, 3, 1) == 0 &&
            link_step(0, 0, 1) == 0 && link_step(3, 1, -1) == 0,
        "the chevrons step and wrap, and an empty or single list stays put");
  LinkStopArm arm;
  check(!link_stop_confirm(arm, "a", 1000) && link_stop_armed(arm, "a", 1000) && !link_stop_armed(arm, "b", 1000),
        "the first press arms one link and turns the button");
  check(link_stop_confirm(arm, "a", 4999) && arm.id.empty(), "a second press inside four seconds stops it and clears the arm");
  check(!link_stop_confirm(arm, "b", 5000) && link_stop_armed(arm, "b", 5000), "another link starts its own arm");
  check(!link_stop_confirm(arm, "b", 9001) && link_stop_armed(arm, "b", 9001),
        "an arm older than four seconds only arms again");
  check(!link_stop_confirm(arm, "", 9002) && arm.id.empty(), "no link, no arm");
  // A link's creation time comes from the engine now, so the pane is newest made first even when the newer one lives less
  // time: a 10-minute link made after a 60-minute one is the newer of the two.
  LinkBook made;
  std::vector<LinkView> timed = {link("short", 600, 0, 0, 500), link("long", 5000, 0, 0, 100)};
  link_book_update(made, timed, 0);
  order = live_link_order(timed, made, 0);
  check(order.size() == 2 && timed[order[0]].id == "short" && timed[order[1]].id == "long",
        "the later-created link is first, though its expiry is earlier");
  order = live_link_order(timed, made, 601);
  check(order.size() == 1 && timed[order[0]].id == "long", "an expired link with a creation time is still hidden");
  std::vector<LinkView> same = {link("b", 600, 0, 0, 500), link("a", 600, 0, 0, 500)};
  LinkBook tied;
  link_book_update(tied, same, 0);
  order = live_link_order(same, tied, 0);
  check(order.size() == 2 && same[order[0]].id == "a" && same[order[1]].id == "b",
        "two links made in the same second keep one order, by id");
  // A link from a links.json written before the time was kept has none, so the whole pane falls back to first sight.
  LinkBook unknown;
  std::vector<LinkView> mixed = {link("first", 100), link("second", 9000)};
  link_book_update(unknown, mixed, 0);
  order = live_link_order(mixed, unknown, 0);
  check(order.size() == 2 && mixed[order[0]].id == "second" && mixed[order[1]].id == "first",
        "with no creation time the first-seen order still decides");
}

// The interface's motion (ui_anim_math.h): smoothing and easing are time-based and land exactly.
void test_anim_math() {
  using namespace bx::ui;
  float v = 0;
  for (int i = 0; i < 200; ++i) v = anim_step(v, 1.0f, 16.0f, 1.0f / 165.0f);  // 1.2 s at 165 fps
  check(v == 1.0f, "a smoothed value lands exactly on its target, so it stops asking for frames");
  float fast = 0, slow = 0;
  for (int i = 0; i < 165; ++i) fast = anim_step(fast, 1.0f, 16.0f, 1.0f / 165.0f);
  for (int i = 0; i < 30; ++i) slow = anim_step(slow, 1.0f, 16.0f, 1.0f / 30.0f);
  check(std::fabs(fast - slow) < 0.01f, "one second gets to the same place at 165 and at 30 frames a second");
  check(anim_step(0.5f, 0.5f, 16.0f, 0.016f) == 0.5f, "a value at its target stays");
  check(anim_step(0.0f, 1.0f, 16.0f, 0.0f) == 0.0f, "no time passed, no movement");
  check(anim_step(0.0f, 1.0f, 16.0f, -1.0f) == 0.0f, "time never runs backwards");
  const float half = anim_step(0.0f, 1.0f, 16.0f, 0.0433f);  // ln 2 / 16
  check(std::fabs(half - 0.5f) < 0.01f, "the rate is the speed: half the way in ln2/rate seconds");
  check(ease_out(0.0f) == 0.0f && ease_out(1.0f) == 1.0f && ease_out(-1.0f) == 0.0f && ease_out(2.0f) == 1.0f,
        "easing starts at 0, ends at 1 and stays inside");
  check(ease_out(0.5f) > 0.5f && ease_out(0.25f) < ease_out(0.5f), "easing is fast first, and rises");
  check(progress(0.08, 0.16f) == 0.5f && progress(1.0, 0.16f) == 1.0f && progress(-1.0, 0.16f) == 0.0f, "progress is elapsed over the length");
  check(progress(0.0, 0.0f) == 1.0f, "a transition of no length has arrived");
  // A view's appearance is kept against the frame sequence that made it. A window whose context was made again at the
  // old address counts frames from 1 again, and reading that against the stored count held the panel's body behind its
  // ground for ever; the same frame continues, the one before it restarts, and so does a frame beyond the stored one.
  check(appear_restart(-2, 1) && appear_restart(4, 1) && appear_restart(900, 5),
        "a slot from another frame sequence starts over");
  check(!appear_restart(4, 5) && !appear_restart(1, 2), "the frame after the stored one carries on");
  // 4A: the motion tokens (docs/ui-fluidity-plan-2026-11-07.md, 6.2). Arrivals take a base, fast or slow token; a slow token is the
  // ceiling; an exit eases in, so it leaves sooner than it arrived.
  check(kDurFast < kDurBase && kDurBase < kDurSlow && kDurSlow <= 0.22f, "the tokens step from fast to base to slow, and slow is the ceiling");
  check(ease_in(0.0f) == 0.0f && ease_in(1.0f) == 1.0f && ease_in(0.5f) < 0.5f && ease_in(2.0f) == 1.0f,
        "the exit curve starts slow, ends at 1 and stays inside");
  check(tween_value(0.0f, 1.0f, 0.0, kDurBase) == 0.0f && tween_value(0.0f, 1.0f, kDurBase, kDurBase) == 1.0f,
        "a fixed tween starts at its first value and lands exactly on its last");
  check(tween_value(0.0f, 1.0f, kDurBase / 2, kDurBase) > 0.5f && tween_value(2.0f, 4.0f, 9.0, 0.1f) == 4.0f,
        "a fixed tween eases out, and one that has finished holds its target");
  check(tween_value(0.0f, 10.0f, 0.0, 0.0f) == 10.0f, "a tween of no length is already at its target");
  // A scroll glide against a list that keeps its offset in whole pixels (ImGui rounds what it is given): it must arrive,
  // from near and far, at both frame rates. A step that rounds back to the start made the window draw every vsync forever.
  bool arrived = true;
  for (const float start : {2.0f, 4.0f, 1.0f, 37.0f, 900.0f}) {
    for (const float target : {0.0f, 3.0f, 500.0f}) {
      for (const float dt : {1.0f / 165.0f, 1.0f / 30.0f}) {
        float offset = start;
        int frames = 0;
        while (offset != target && frames < 2000) {
          const float next = glide_step(offset, target, 20.0f, dt);
          offset = std::round(next);  // what the list keeps
          ++frames;
        }
        arrived = arrived && offset == target;
      }
    }
  }
  check(arrived, "a scroll glide arrives on its target with the list keeping whole pixels");
  check(glide_step(5.0f, 5.0f, 20.0f, 0.006f) == 5.0f, "a list at its target stays");
  check(glide_step(2.0f, 0.0f, 20.0f, 0.006f) == 1.0f, "a step that would round back moves one pixel anyway");
}

// The quick panel's size (ui_window_math.h): a tenth either side of the default, never past the work area.
void test_quick_panel_size() {
  using namespace bx::ui;
  check(resize_frame_interval_us(0) == 8000 && resize_frame_interval_us(1) == 8000,
        "a missing monitor refresh keeps the existing resize limit");
  check(resize_frame_interval_us(165) < 1000000 / 165 && resize_frame_interval_us(165) > 5000,
        "resize redraws can follow a 165 Hz display with room for event jitter");
  check(resize_frame_interval_us(60) > resize_frame_interval_us(165) &&
            resize_frame_interval_us(240) < resize_frame_interval_us(165),
        "resize redraw limits follow the window monitor's refresh rate");
  auto resize_frames = [](int interval_us, int event_gap_us) {
    int frames = 0;
    int last_us = -interval_us;
    for (int at = 0; at < 1000000; at += event_gap_us) {
      if (at - last_us >= interval_us) { last_us = at; ++frames; }
    }
    return frames;
  };
  check(resize_frames(resize_frame_interval_us(165), 6060) >= 160 &&
            resize_frames(8000, 6060) < 90,
        "165 Hz resize events no longer lose every other redraw to the 8 ms limit");
  const float scales[] = {1.2f, 1.5f, 1.8f, 2.4f};
  const long works[][2] = {{1366, 728}, {1920, 1040}, {2560, 1400}, {3840, 2100}};
  bool inside = true, ordered = true;
  for (const float scale : scales) {
    for (const auto& work : works) {
      const QuickLimits limits = quick_limits(scale, work[0], work[1]);
      ordered = ordered && limits.min_width <= limits.max_width && limits.min_height <= limits.max_height;
      inside = inside && limits.max_width <= work[0] && limits.max_height <= work[1];
      const QuickSize opened = quick_size_for(kQuickWidth, kQuickHeight, scale, work[0], work[1]);
      inside = inside && opened.width <= work[0] && opened.height <= work[1] && opened.width >= limits.min_width &&
               opened.height >= limits.min_height;
    }
  }
  check(ordered, "the minimum is never over the maximum, on any scale or work area");
  check(inside, "the panel is never taller or wider than the work area it is on");
  const QuickLimits roomy = quick_limits(1.2f, 2560, 1400);
  check(roomy.max_width == std::lround(kQuickMaxWidth * 1.2f) && roomy.max_height == std::lround(kQuickMaxHeight * 1.2f),
        "on a roomy screen the maximum is its own, about a tenth over the default");
  check(kQuickMaxWidth / kQuickWidth > 1.08f && kQuickMaxWidth / kQuickWidth < 1.10f, "the widest is about a tenth over the default");
  check(kQuickMinWidth == kQuickWidth, "the narrowest is the default width: the key caps still fit");
  check(quick_size_for(kQuickWidth, kQuickHeight, 1.2f, 1920, 1040).width == std::lround(kQuickWidth * 1.2f) &&
            quick_size_for(kQuickWidth, kQuickHeight, 1.2f, 1920, 1040).height == std::lround(kQuickHeight * 1.2f),
        "the default opens at its design size");
  const QuickSize tiny = quick_size_for(10, 10, 1.2f, 1920, 1040), huge = quick_size_for(5000, 5000, 1.2f, 1920, 1040);
  check(tiny.width == roomy.min_width && tiny.height == roomy.min_height, "a stored size under the floor opens at the floor");
  check(huge.width == roomy.max_width && huge.height == roomy.max_height, "a stored size over the ceiling opens at the ceiling");
  const QuickLimits laptop = quick_limits(2.4f, 1366, 728);
  check(laptop.max_height == 728 && laptop.min_height == 728 && laptop.max_width <= 1366,
        "a work area shorter than the floor is the floor and the ceiling: the panel scrolls instead of leaving the screen");
  check(quick_design(384, 1.2f) == 320.0f && quick_design(1000, 0) == 0.0f, "pixels go back to whole design units");
}

// The expanded panel can grow in either direction within its bounds and the work area.
void test_panel_size() {
  using namespace bx::ui;
  const PanelLimits roomy = panel_limits(1.2f, 2560, 2000);
  check(roomy.min_width == 1242 && roomy.min_height == 1008,
        "the measured expanded window is the default-scale minimum");
  check(std::abs(panel_scale_to_fit(1.2f, 1242, 1008) - 1.2f) < 0.001f,
        "the measured window keeps its full scale");
  check(panel_scale_to_fit(1.8f, 1366, 768) <= 768.0f / kPanelMinHeight + 0.001f,
        "a small work area scales the whole panel down so its layout fits");
  check(std::abs(panel_scale_to_fit(0.9f, 2560, 1400) - 0.9f) < 0.001f,
        "a requested smaller UI scale is respected");
  check(kMixerTrackHeight >= 96, "the audio faders keep a useful minimum length");
  check(roomy.min_width == std::lround(kPanelMinWidth * 1.2f) &&
            roomy.min_height == std::lround(kPanelMinHeight * 1.2f),
        "the smallest panel keeps its layout floor");
  check(roomy.max_width == std::lround(kPanelMaxWidth * 1.2f) &&
            roomy.max_height == std::lround(kPanelMaxHeight * 1.2f),
        "the largest panel is its own ceiling");
  check(roomy.min_height == 1008 && roomy.max_height == 1008 && kPanelMaxHeight == 840,
        "the expanded panel's height is locked");
  check(roomy.max_width == 1579, "the expanded panel's width ceiling grows by 10 percent");
  const PanelLimits editor = panel_limits(1.2f, 2560, 2000, true);
  check(editor.min_height == 1008 && editor.max_height == 1440,
        "the embedded editor keeps its existing vertical resize range");
  const PanelSize opened = panel_size_for(kPanelWidth, kPanelHeight, 1.2f, 2560, 1400);
  check(opened.width == std::lround(kPanelWidth * 1.2f) && opened.height == std::lround(kPanelHeight * 1.2f),
        "the default opens at its design size");
  const PanelSize tiny = panel_size_for(10, 10, 1.2f, 2560, 2000), huge = panel_size_for(5000, 5000, 1.2f, 2560, 2000);
  check(tiny.width == roomy.min_width && tiny.height == roomy.min_height, "a stored size under the floor opens at the floor");
  check(huge.width == roomy.max_width && huge.height == roomy.max_height, "a stored size over the ceiling opens at the ceiling");
  const PanelLimits laptop = panel_limits(2.4f, 1366, 728);
  check(laptop.min_width == 1366 && laptop.max_width == 1366 && laptop.min_height == 728 && laptop.max_height == 728,
        "a work area smaller than the floor is both limits: the panel fills it and scrolls, never past the screen");
  check(editor_resize_inset(1.2f, 1152, 816) >= static_cast<long>(6 * 1.2f),
        "the editor leaves the panel's resize hit border exposed");
  check(editor_resize_inset(2.4f, 1366, 728) >= static_cast<long>(6 * 2.4f),
        "the editor leaves the resize border exposed at high DPI");
  check(editor_resize_inset(1.2f, 3, 2) == 1 && editor_resize_inset(1.2f, 0, 0) == 0,
        "the editor inset cannot exceed half its client area");
  bool ordered = true, inside = true, height_bounded = true;
  const float scales[] = {1.2f, 1.5f, 1.8f, 2.4f};
  const long works[][2] = {{1366, 728}, {1920, 1040}, {2560, 1400}, {3840, 2100}};
  for (const float scale : scales) {
    for (const auto& work : works) {
      const PanelLimits limits = panel_limits(scale, work[0], work[1]);
      ordered = ordered && limits.min_width <= limits.max_width && limits.min_height <= limits.max_height;
      inside = inside && limits.max_width <= work[0] && limits.max_height <= work[1];
      height_bounded = height_bounded && limits.max_height <= std::lround(kPanelMaxHeight * scale);
    }
  }
  check(ordered, "the minimum is never over the maximum, on any scale or work area");
  check(inside, "the panel is never taller or wider than the work area it is on");
  check(height_bounded, "the panel's height stays bounded at every scale and work-area size");
}

void test_interface_scale() {
  bx::Config config;
  check(config.ui_scale_percent == 100, "the interface keeps its current size by default");
  bx::Json patch = bx::Json::object();
  patch.set("ui_scale_percent", bx::Json(125));
  bx::config_from_json(patch, config);
  check(bx::config_to_json(config)["ui_scale_percent"].as_int() == 125, "the interface scale survives config serialization");
  patch.set("ui_scale_percent", bx::Json(999));
  bx::config_from_json(patch, config);
  check(config.ui_scale_percent == 150, "the interface scale is capped");
  patch.set("ui_scale_percent", bx::Json(1));
  bx::config_from_json(patch, config);
  check(config.ui_scale_percent == 75, "the interface scale has a readable floor");
  check(!bx::profile_patch(patch).has("ui_scale_percent"), "a game profile never changes the interface scale");
}

// The clip grid (ui_window_math.h): as many columns as the width holds, and never a card past its right edge.
void test_card_grid() {
  using namespace bx::ui;
  const float gap = 10, min_card = 190;
  check(card_grid(236, gap, min_card).columns == 1, "a narrow library holds one card");
  check(card_grid(436, gap, min_card).columns == 2, "a middle library holds two");
  check(card_grid(596, gap, min_card).columns == 3, "a wide library holds three");
  check(card_grid(816, gap, min_card).columns == 4, "the widest library holds four");
  check(card_grid(1090, gap, min_card).columns == 4, "the wider panel still keeps at most four card columns");
  bool fits = true;
  for (const float width : {50.0f, 236.0f, 436.0f, 596.0f, 816.0f, 1200.0f}) {
    const GridSpec grid = card_grid(width, gap, min_card);
    fits = fits && grid.columns >= 1 && grid.width > 0 &&
           grid.columns * grid.width + gap * (grid.columns - 1) <= width + 0.01f;
  }
  check(fits, "no card is ever past the grid's right edge");
  check(card_grid(50, gap, min_card).columns == 1 && card_grid(50, gap, min_card).width == 50,
        "a library narrower than one card keeps the whole width");
}

// The icon paths (ui_icons.h): parsed once into segments the interface strokes at any size.
// The path a shell API is handed (shared/files.h native_path): the engine mixes libobs's `/` with the
// library's `\`, and the Recycle Bin's IShellItem refuses the mixture with E_INVALIDARG.
void test_native_path() {
  check(bx::native_path("C:/Users/x/AppData/Local/Temp/b/clips\\Display Capture/a.mp4") ==
            "C:\\Users\\x\\AppData\\Local\\Temp\\b\\clips\\Display Capture\\a.mp4",
        "a path mixing both separators becomes the shell's own");
  check(bx::native_path("C:\\a\\b.mp4") == "C:\\a\\b.mp4", "an already native path is unchanged");
  check(bx::native_path("//server/share/a.mp4") == "\\\\server\\share\\a.mp4", "a UNC path keeps its leading pair");
  check(bx::native_path("\\\\server/share/a.mp4") == "\\\\server\\share\\a.mp4", "a half-native UNC path too");
  check(bx::native_path("clips/x.mp4") == "clips\\x.mp4", "a relative path is rewritten the same way");
  check(bx::native_path("") == "", "an empty path stays empty");
}

// The UTF-8 <-> UTF-16 boundary (shared/files.h): one rule for every Win32 call that takes or returns a
// wide string, so a name outside ASCII survives whichever file makes the call.
void test_wide_text() {
  const std::string path = "C:/clips/\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.mp4";  // æ—¥æœ¬èªž.mp4
  const std::wstring wide = bx::utf8_to_wide(path);
  check(wide.size() == 16, "three-byte UTF-8 becomes one UTF-16 unit per code point");
  check(bx::wide_to_utf8(wide) == path, "a path outside ASCII survives the round trip");
  check(bx::utf8_to_wide("").empty() && bx::wide_to_utf8(L"").empty(), "an empty string stays empty");
  check(bx::wide_path("C:/clips/a.mp4") == L"C:/clips/a.mp4", "an ASCII path widens unchanged");
}

void test_ui_icons() {
  using namespace bx::ui;
  using bx::Json;
  std::vector<PathSegment> line = parse_svg_path("M12 5v14M5 12h14");
  check(line.size() == 4 && line[0].kind == PathSegment::move && line[1].kind == PathSegment::line &&
            line[1].x == 12 && line[1].y == 19 && line[3].x == 19 && line[3].y == 12,
        "moves and relative lines");
  std::vector<PathSegment> square = parse_svg_path("M7 7h10v10H7z");
  check(square.size() == 5 && square.back().kind == PathSegment::close && square[3].x == 7 && square[3].y == 17,
        "absolute H and a close");
  std::vector<PathSegment> implicit = parse_svg_path("M5 12.5 10 17 19 7");
  check(implicit.size() == 3 && implicit[1].kind == PathSegment::line && implicit[2].x == 19, "pairs after M are lines");
  std::vector<PathSegment> arc = parse_svg_path("M12 12m-6 0a6 6 0 1 0 12 0a6 6 0 1 0-12 0");
  bool all_on_circle = true;
  int curves = 0;
  for (const PathSegment& s : arc) {
    if (s.kind != PathSegment::cubic) continue;
    ++curves;
    all_on_circle = all_on_circle && std::fabs(std::hypot(s.x - 12, s.y - 12) - 6) < 0.01;
  }
  check(curves >= 4 && all_on_circle, "arcs become cubics that end on the circle");
  std::vector<PathSegment> compact_numbers = parse_svg_path("M5 17v2a1 1 0 0 0 1 1h12");
  check(compact_numbers.size() >= 4 && std::fabs(compact_numbers.back().x - 18) < 0.001 &&
            std::fabs(compact_numbers.back().y - 20) < 0.001,
        "packed numbers and a small arc");
  std::vector<PathSegment> negative = parse_svg_path("M15 5l-7 7 7 7");
  check(negative.size() == 3 && negative[1].x == 8 && negative[2].x == 15 && negative[2].y == 19, "negative relative numbers");
  check(parse_svg_path("").empty() && parse_svg_path("Q1 2").empty(), "an unknown command parses to nothing");
  check(!icon_path("save").empty() && icon_path("no such icon").empty(), "the icon table");
  check(!parse_svg_path(icon_path("update")).empty() && icon_path("update") != icon_path("save") &&
            icon_path("update") != icon_path("refresh"), "the update icon is distinct and drawable");
  // A path the parser cannot read draws nothing at all, so a glyph with a typo is invisible rather than wrong.
  check(!parse_svg_path(icon_path("discord")).empty() && !parse_svg_path(icon_path("globe")).empty() &&
            !parse_svg_path(icon_path("arrowRight")).empty(),
        "the social and update glyphs parse");

  // The mark is cut into triangles once. ImGui's own concave fill lost its notches and drew a blob, so the cut is checked by
  // area: the triangles must cover exactly the polygon, whichever way it is wound.
  const auto polygon_area = [](const std::vector<MarkPoint>& points) {
    double sum = 0;
    for (size_t i = 0; i < points.size(); ++i) {
      const MarkPoint &a = points[i], &b = points[(i + 1) % points.size()];
      sum += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    return std::abs(sum) / 2;
  };
  const auto triangles_area = [](const std::vector<MarkPoint>& points, const std::vector<std::array<int, 3>>& triangles) {
    double sum = 0;
    for (const auto& t : triangles) {
      const MarkPoint &a = points[static_cast<size_t>(t[0])], &b = points[static_cast<size_t>(t[1])], &c = points[static_cast<size_t>(t[2])];
      sum += ((static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) - (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x)) / 2;
    }
    return sum;
  };
  const std::vector<MarkPoint> arrow = {{0, 0}, {4, 0}, {4, 1}, {1, 1}, {1, 3}, {0, 3}};  // an L: concave, area 6
  const std::vector<MarkPoint> arrow_back(arrow.rbegin(), arrow.rend());
  check(std::abs(triangles_area(arrow, triangulate(arrow)) - 6.0) < 1e-9 && triangulate(arrow).size() == 4, "an L is cut into four triangles that cover it");
  check(std::abs(std::abs(triangles_area(arrow_back, triangulate(arrow_back))) - 6.0) < 1e-9, "a polygon wound the other way is cut the same");
  check(triangulate({{0, 0}, {1, 1}}).empty(), "fewer than three points have no triangles");
  const std::vector<MarkPoint>& mark = mark_outline();
  const std::vector<std::array<int, 3>> mark_triangles = triangulate(mark);
  check(mark.size() > 100 && !(mark.front().x == mark.back().x && mark.front().y == mark.back().y), "the mark is one open contour, its closing repeat dropped");
  check(mark_triangles.size() == mark.size() - 2, "every corner of the mark is used: n - 2 triangles");
  check(std::abs(std::abs(triangles_area(mark, mark_triangles)) - polygon_area(mark)) < 1e-2 * polygon_area(mark) / 100,
        "the mark's triangles cover its area exactly: no notch is lost, none is filled");
  // The sampled bitmap the titlebars draw: same area as the contour, and edges between the two extremes.
  const int mark_size = 64;
  const std::vector<uint8_t> coverage = mark_coverage(mark_size);
  check(coverage.size() == static_cast<size_t>(mark_size) * mark_size && coverage[0] == 0, "the bitmap is empty outside the glyph");
  double covered = 0;
  bool soft = false;
  for (uint8_t alpha : coverage) {
    covered += alpha / 255.0;
    soft = soft || (alpha > 0 && alpha < 255);
  }
  const double step = 360.0 / mark_size;
  check(std::abs(covered - polygon_area(mark) / (step * step)) < 0.01 * polygon_area(mark) / (step * step),
        "the bitmap covers the mark's area: 4x4 samples per pixel");
  check(soft, "the bitmap's edge pixels are part-covered, so the mark has no steps to see");
}

// Crash recovery (recovery_policy.h): the ring is saved once when the capture faults, and what the first start after
// an end nobody executed says about the library. The regression this guards: the RAM ring was the whole buffer, so a
// crash took all of it and nothing said anything about it.
void test_recovery_policy() {
  std::printf("crash recovery\n");
  // The ring is dumped once, at the moment a fault is seen, and never in normal use.
  check(buffrx::salvage_wanted(true, true, 20000, buffrx::kSalvageCooldownMs),
        "a fault with a full ring and no recent salvage dumps the buffer");
  check(!buffrx::salvage_wanted(false, true, 20000, buffrx::kSalvageCooldownMs),
        "the person's switch turns it off");
  check(!buffrx::salvage_wanted(true, false, 20000, buffrx::kSalvageCooldownMs),
        "a capture that is working writes nothing");
  check(!buffrx::salvage_wanted(true, true, 3000, buffrx::kSalvageCooldownMs),
        "a ring under the minimum is not footage");
  check(!buffrx::salvage_wanted(true, true, 20000, buffrx::kSalvageCooldownMs - 1),
        "one salvage per fault episode: no library full of the same minute");
  check(buffrx::salvage_wanted(true, true, buffrx::kSalvageMinBufferMs, buffrx::kSalvageCooldownMs),
        "the minimum ring is enough to be worth a file");

  // The card: what survived, and a file that will not read is never called safe.
  buffrx::RecoveryFacts facts;
  facts.clips_ok = 3;
  facts.screenshots_ok = 2;
  check(!buffrx::recovery_notice_wanted(facts), "an intact library is not news: a kill leaves no card");
  check(buffrx::recovery_notice_text(facts) == "Your 3 clips and 2 screenshots are safe. BuffrX closed unexpectedly.",
        "the card says what survived, and what happened");
  facts.clips_ok = 1;
  facts.screenshots_ok = 0;
  check(buffrx::recovery_notice_text(facts) == "Your 1 clip is safe. BuffrX closed unexpectedly.",
        "one clip is singular");
  facts.clips_ok = 0;
  facts.screenshots_ok = 1;
  check(buffrx::recovery_notice_text(facts) == "Your 1 screenshot is safe. BuffrX closed unexpectedly.",
        "a screenshot alone is still an answer");
  facts.screenshots_ok = 0;
  facts.damaged = 1;
  check(buffrx::recovery_notice_wanted(facts), "a damaged file is the one thing a start has to say");
  check(buffrx::recovery_notice_text(facts) == "1 file looks damaged. BuffrX closed unexpectedly.",
        "damage is named even with nothing to count as safe");
  facts.clips_ok = 2;
  check(buffrx::recovery_notice_text(facts).rfind("Your 2 clips are safe, but 1 file looks damaged.", 0) == 0,
        "a file that will not read is named in the same breath");
  buffrx::RecoveryFacts nothing;
  check(!buffrx::recovery_notice_wanted(nothing), "a clean start says nothing at all");

  // Quitting: a hide to the tray is never a question, a running write is.
  check(!buffrx::quit_needs_confirm(false, false), "nothing is being written: Quit just quits");
  check(buffrx::quit_needs_confirm(true, false), "a manual recording is being written: confirm before cutting it");
  check(buffrx::quit_needs_confirm(false, true), "a Timeline session is being written: confirm too");
}

// `engine.restart` was the one command that rebuilt the engine inside its own handler: shutdown, initialize and
// the buffer start ran under the engine lock, so the client's reply - and the tick, the hotkeys and every other
// client - waited for the whole rebuild. It is handed to the action worker now (main.cpp's queue) and the command
// answers `{accepted:true}`; the worker's own tail broadcasts `status` once the rebuild ends. This holds the
// hand-off to that order with the real queue: the rebuild cannot run before the job is queued, the command
// returns while it has not begun, and the job carries the buffer state it must restore.
void test_engine_restart() {
  std::printf("engine restart\n");
  {
    bool enqueued = false;
    std::string error;
    auto take = [&](bx::QueuedAction) { enqueued = true; return true; };
    check(!bx::request_restart(true, false, true, take, error), "a restart is refused while a recording is written");
    check(error == "stop the recording first: restarting the engine would end it",
          "the refusal is the sentence Quit asks about");
    check(!enqueued, "and nothing is queued for the worker");
    error.clear();
    check(!bx::request_restart(false, true, false, take, error), "a restart is refused while Timeline writes");
    check(!enqueued, "and nothing is queued there either");
  }
  {
    // A save the worker is already running: a restart queued behind it still returns at once.
    bx::WorkQueue<bx::QueuedAction> actions(4);
    std::mutex order_mutex;
    std::vector<std::string> order;
    std::atomic<bool> release{false};
    std::atomic<bool> buffering_seen{false};
    actions.start([&](const bx::QueuedAction& action) {
      if (action.name == "save_replay") {
        while (!release.load()) Sleep(1);  // the save ahead of the restart
        return;
      }
      std::lock_guard<std::mutex> lock(order_mutex);
      order.push_back(action.name);
      buffering_seen = action.was_buffering;
    });
    check(actions.push(bx::QueuedAction{"save_replay", false}), "a save is on the worker");
    std::string error;
    auto enqueue = [&](bx::QueuedAction action) { return actions.push(action); };
    check(bx::request_restart(false, false, true, enqueue, error), "the command hands the restart over and returns");
    {
      std::lock_guard<std::mutex> lock(order_mutex);
      check(order.empty(), "the rebuild has not begun when the command returns");
    }
    release = true;
    actions.stop();  // drains in order, then joins: the rebuild happens after the reply
    std::lock_guard<std::mutex> lock(order_mutex);
    check(order.size() == 1 && order[0] == "restart_engine", "the worker took the rebuild after the reply");
    check(buffering_seen.load(), "and it was told the buffer was running, so it comes back");
  }
  {
    // A queue that cannot take the job says so: the client is answered with a reason, not a silent no-op.
    bx::WorkQueue<bx::QueuedAction> full(1);
    std::atomic<bool> release{false};
    full.start([&](const bx::QueuedAction&) { while (!release.load()) Sleep(1); });
    check(full.push(bx::QueuedAction{"save_replay", false}), "the worker holds a job");
    Sleep(60);  // it has taken it; the queue now holds its one waiting slot
    check(full.push(bx::QueuedAction{"quick_clip", false}), "the queue is full");
    std::string error;
    check(!bx::request_restart(false, false, false, [&](bx::QueuedAction action) { return full.push(action); }, error),
          "a restart the worker cannot take is refused");
    check(error.find("busy") != std::string::npos, "with a reason the client can read");
    release = true;
    full.stop();
  }
}

// The doc and the engine answer the same commands, in both directions: a command in one and not the other is drift.
void test_protocol_sync() {
#ifdef BUFFRX_SOURCE_DIR
  std::vector<std::string> documented;
  if (FILE* file = std::fopen((source_root() + "/docs/protocol.md").c_str(), "rb")) {
    std::string text;
    char buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
    std::fclose(file);
    bool in_commands = false;
    size_t pos = 0;
    while (pos <= text.size()) {
      const size_t end = text.find('\n', pos);
      std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("## ", 0) == 0) {
        in_commands = line.rfind("## Commands", 0) == 0;
      } else if (in_commands && line.rfind("| `", 0) == 0) {
        const std::string cell = line.substr(0, line.find('|', 1));  // the name cell, never an argument
        for (size_t open = cell.find('`'); open != std::string::npos;) {
          const size_t close = cell.find('`', open + 1);
          if (close == std::string::npos) break;
          const std::string name = cell.substr(open + 1, close - open - 1);
          if (name != "-" && name.find(' ') == std::string::npos) documented.push_back(name);
          open = cell.find('`', close + 1);
        }
      }
      if (end == std::string::npos) break;
      pos = end + 1;
    }
  }
  check(!documented.empty(), "docs/protocol.md lists the commands");

  std::set<std::string> unique_names;
  bool unique = true;
  for (const bx::CommandSpec& spec : bx::kProtocolCommands) {
    unique = unique && spec.name && spec.name[0] && unique_names.insert(spec.name).second;
  }
  check(unique, "the command table names each command once");
  check(bx::command_needs_engine_lock("status.get") && !bx::command_needs_engine_lock("timeline.save") &&
            !bx::command_needs_engine_lock("share.prepare") && !bx::command_needs_engine_lock("update.check") &&
            !bx::command_needs_engine_lock("app.open_url"),
        "the command table is also the lock policy");

  std::vector<std::string> answered = bx::protocol_capabilities();
  std::sort(documented.begin(), documented.end());
  documented.erase(std::unique(documented.begin(), documented.end()), documented.end());
  std::sort(answered.begin(), answered.end());
  answered.erase(std::unique(answered.begin(), answered.end()), answered.end());
  std::vector<std::string> doc_only, code_only;
  std::set_difference(documented.begin(), documented.end(), answered.begin(), answered.end(), std::back_inserter(doc_only));
  std::set_difference(answered.begin(), answered.end(), documented.begin(), documented.end(), std::back_inserter(code_only));
  for (const std::string& name : doc_only) std::printf("  doc command not answered: %s\n", name.c_str());
  for (const std::string& name : code_only) std::printf("  answered command not documented: %s\n", name.c_str());
  check(doc_only.empty(), "every command in docs/protocol.md is answered by Protocol::capabilities()");
  check(code_only.empty(), "every command in Protocol::capabilities() is in docs/protocol.md");
#else
  check(false, "BUFFRX_SOURCE_DIR is defined, so docs/protocol.md can be read");
#endif
}

// ---------------------------------------------------------------- the disk tier against a model (N3)

// A packet of random shape, small enough that the RAM ceiling never evicts it: the only eviction is then the time rule
// every ring shares, which is what makes a disk ring and a RAM-only ring save the same footage.
SpillTestPacket spill_model_packet(std::mt19937_64& random, bool video, bool key, int64_t dts_usec) {
  const uint64_t roll = random() % 1000;
  uint32_t size = 0;
  if (roll < 900) size = static_cast<uint32_t>(1 + random() % 4096);
  else if (roll < 995) size = static_cast<uint32_t>(1 + random() % (256 * 1024));
  else size = static_cast<uint32_t>(1 + random() % static_cast<uint64_t>(kMiB));
  return spill_packet(video, key, dts_usec, size);
}

// 200,000 seeded operations: a ring with a disk tier against a RAM-only ring fed the same packets. At every segment's
// quiet point the two saves must be identical, down to the bytes. A backstop or a failed spill is its own documented
// regime ("the disk part is given up"), so it is counted and the segment is dropped instead of compared.
void test_ring_store_model() {
  // The ceiling is above one 8 MiB spill block on purpose: the product's margin over the spill start is at least 64 MiB (or
  // half a small limit), so a ring never meets its ceiling while the disk keeps up. A ceiling under one block is outside
  // the ring's contract: entries are only released once a block flushes, so RAM would pass the ceiling and the ring would
  // purge footage instead of reporting the backstop.
  const int64_t file_bytes = 64 * kMiB, limit = 2 * kMiB, ceiling = 32 * kMiB, max_time_us = 3 * 1000000;
  constexpr int kSeeds = 40, kSegments = 10, kOpsPerSegment = 500;
  int64_t ops = 0, resets = 0;
  for (int seed = 1; seed <= kSeeds; ++seed) {
    std::mt19937_64 random(static_cast<uint64_t>(seed) * 7919);
    SpillRig rig(file_bytes, limit, max_time_us, ceiling);
    SpillStore oracle;
    oracle.configure(max_time_us, 0);
    int64_t dts_usec = 0;
    int video_since_key = 0;
    bool ok = true;
    for (int segment = 0; segment < kSegments && ok; ++segment) {
      for (int op = 0; op < kOpsPerSegment; ++op) {
        if (op % 2 == 0) dts_usec += 50000;  // 20 fps
        const bool video = op % 3 != 0;
        bool key = false;
        if (video && ++video_since_key >= 10) {
          key = true;
          video_since_key = 0;
        }
        const SpillTestPacket packet = spill_model_packet(random, video, key, dts_usec);
        rig.push(packet);
        oracle.push(packet);
        ++ops;
      }
      const bx::StoreInfo info = rig.store.info();
      if (info.backstop_events > 0 || info.spill_failed) {
        ++resets;
        rig.reset();
        oracle.reset();
        continue;
      }
      bx::SaveReport report;
      const std::vector<SpillOut> got = spill_collect(rig.store, &report);
      const std::vector<SpillOut> want = spill_collect(oracle);
      bool same = report.ok && !report.unreadable && report.skipped == 0 && got.size() == want.size();
      size_t at = 0;
      if (same) {
        for (; at < got.size(); ++at) {
          if (!(got[at] == want[at])) {
            same = false;
            break;
          }
        }
      }
      if (!same) {
        std::printf("  seed %d segment %d: disk %zu packets (ok %d, skipped %zu, unreadable %d) vs model %zu, first difference %zu\n",
                    seed, segment, got.size(), report.ok ? 1 : 0, report.skipped, report.unreadable ? 1 : 0, want.size(), at);
        const bx::StoreInfo other = oracle.info();
        std::printf("    disk info: packets %zu ram %lld disk %lld span %lld; model info: packets %zu ram %lld disk %lld span %lld\n",
                    info.packets, static_cast<long long>(info.ram_bytes), static_cast<long long>(info.disk_bytes),
                    static_cast<long long>(info.span_us), other.packets, static_cast<long long>(other.ram_bytes),
                    static_cast<long long>(other.disk_bytes), static_cast<long long>(other.span_us));
        if (!got.empty() && !want.empty()) {
          std::printf("    disk dts %lld..%lld, model dts %lld..%lld\n", static_cast<long long>(got.front().fields.dts_usec),
                      static_cast<long long>(got.back().fields.dts_usec), static_cast<long long>(want.front().fields.dts_usec),
                      static_cast<long long>(want.back().fields.dts_usec));
        }
        ok = false;
      }
      rig.reset();
      oracle.reset();
    }
    const std::string name = "seed " + std::to_string(seed) + ": a ring with a disk tier and a RAM-only ring save the same footage";
    check(ok, name.c_str());
  }
  {
    // A disk that refuses writes must be reported and the ring must fall back, not silently lose footage.
    SpillRig failing(file_bytes, limit, max_time_us, ceiling);
    failing.disk.fail_writes = true;
    std::mt19937_64 forced(12345);
    int64_t dts = 0;
    for (int op = 0; op < 4000; ++op) {
      if (op % 2 == 0) dts += 50000;
      const bool video = op % 3 != 0;
      const bool key = video && op % 30 == 0;
      failing.push(spill_model_packet(forced, video, key, dts));
    }
    const bx::StoreInfo info = failing.store.info();
    check(info.spill_failed || info.backstop_events > 0, "a disk that refuses writes is reported, not ignored");
    if (info.spill_failed || info.backstop_events > 0) ++resets;
  }
  check(ops >= 200000, "the ring model ran at least 200,000 operations");
  check(resets > 0, "segments the disk could not take were counted, not compared");
}

// A fake disk the two threads share. It remembers the logical offset last written to each physical block, so a save that
// reads a block the writer has already lapped reports it: that is the pin's whole job, and a failure here is a real bug.
class SpillSafeDisk : public bx::SpillBackend {
public:
  explicit SpillSafeDisk(int64_t size)
      : size_(size), file_(static_cast<size_t>(size), 0), last_(static_cast<size_t>(size / bx::kSpillBlockBytes), -1) {}
  bool write_block(int64_t offset, const uint8_t* data) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t at = static_cast<size_t>(offset % size_);
    last_[at / static_cast<size_t>(bx::kSpillBlockBytes)] = offset;
    std::memcpy(&file_[at], data, static_cast<size_t>(bx::kSpillBlockBytes));
    return true;
  }
  bool read_block(int64_t offset, uint8_t* destination) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t at = static_cast<size_t>(offset % size_);
    if (last_[at / static_cast<size_t>(bx::kSpillBlockBytes)] != offset) return false;
    std::memcpy(destination, &file_[at], static_cast<size_t>(bx::kSpillBlockBytes));
    return true;
  }

private:
  int64_t size_;
  std::mutex mutex_;
  std::vector<uint8_t> file_;
  std::vector<int64_t> last_;
};

// The same store driven by three threads: packets arriving, the spill writing, saves running. The accounting and the pin
// must hold whatever order they land in.
void test_ring_store_threads() {
  const int64_t file_bytes = 4 * kMiB, limit = 1 * kMiB, ceiling = 32 * kMiB, max_time_us = 6 * 1000000;
  SpillSafeDisk disk(file_bytes);
  std::vector<uint8_t> block(static_cast<size_t>(bx::kSpillBlockBytes)), window_a(block.size()), window_b(block.size());
  SpillStore store;
  store.configure(max_time_us, ceiling);
  SpillStore::DiskSetup setup;
  setup.io = &disk;
  setup.file_bytes = file_bytes;
  setup.ram_limit_bytes = limit;
  setup.block = block.data();
  setup.window_a = window_a.data();
  setup.window_b = window_b.data();
  store.enable_disk(setup);

  std::atomic<bool> running{true};
  std::atomic<int> bad_saves{0};
  std::atomic<int64_t> pushed{0};
  std::thread pusher([&] {
    int64_t dts_usec = 0;
    int video_since_key = 0;
    uint64_t n = 0;
    while (running.load()) {
      dts_usec += 20000;  // 50 fps
      const bool video = n % 5 != 0;
      bool key = false;
      if (video && ++video_since_key >= 20) {
        key = true;
        video_since_key = 0;
      }
      const uint32_t size = 1 + static_cast<uint32_t>(n % 32768);
      const SpillTestPacket packet = spill_packet(video, key, dts_usec, size);
      store.push(packet);
      ++pushed;
      ++n;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  std::thread spiller([&] {
    while (running.load()) {
      if (store.spill_due()) {
        store.spill_step();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  });
  std::thread saver([&] {
    while (running.load()) {
      bx::SaveReport report;
      spill_collect(store, &report);
      if (!report.ok || report.unreadable || report.skipped != 0) ++bad_saves;
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(3000));
  running.store(false);
  pusher.join();
  spiller.join();
  saver.join();
  bx::SaveReport last;
  const std::vector<SpillOut> out = spill_collect(store, &last);
  const bx::StoreInfo info = store.info();
  check(bad_saves.load() == 0, "no save read a block the writer had lapped, and none reported a gap");
  check(!out.empty() && last.ok && !last.unreadable, "a save after the three threads stop is whole");
  check(info.ram_bytes <= limit / 2 + bx::kSpillBlockBytes + 64 * 1024,
        "RAM stayed under the spill start plus one block and one packet");
  check(pushed.load() > 100, "the push thread ran");
}

// ---------------------------------------------------------------- the update check (update_math.h)
void test_update_math() {
  using bx::compare_versions;
  using bx::parse_version;
  using bx::update_check_due;
  using bx::update_is_newer;
  using bx::update_should_offer;
  using bx::update_should_open_notes;

  check(compare_versions(parse_version("0.6.0"), parse_version("0.5.0")) > 0, "0.6.0 is newer than 0.5.0");
  check(compare_versions(parse_version("0.5.0"), parse_version("0.6.0")) < 0, "0.5.0 is older than 0.6.0");
  check(compare_versions(parse_version("0.10.0"), parse_version("0.9.0")) > 0, "10 sorts after 9, not before");
  check(compare_versions(parse_version("v0.6.0"), parse_version("0.6.0")) == 0, "a v prefix is ignored");
  check(compare_versions(parse_version("0.6"), parse_version("0.6.0")) == 0, "a missing patch reads as zero");
  check(compare_versions(parse_version("0.6.0"), parse_version("0.6.0-alpha.1")) > 0,
        "a release outranks its own prerelease");
  check(compare_versions(parse_version("0.6.0-beta.2"), parse_version("0.6.0-beta.10")) < 0,
        "prerelease numbers compare numerically");
  check(compare_versions(parse_version("1.0.0-alpha.10"), parse_version("1.0.0-alpha.beta")) < 0 &&
            compare_versions(parse_version("1.0.0-alpha.beta"), parse_version("1.0.0-alpha.10")) > 0,
        "numeric prerelease identifiers precede nonnumeric identifiers");
  check(compare_versions(parse_version("0.6.0-alpha"), parse_version("0.6.0-alpha.1")) < 0,
        "a bare prerelease is older than the same one numbered");
  check(compare_versions(parse_version("0.6.1-alpha"), parse_version("0.6.0")) > 0,
        "the next patch's prerelease is newer than the release before it");
  check(!parse_version("nightly").valid, "a junk tag does not parse");
  check(!parse_version("0.6.0-").valid && !parse_version("0.6.0-alpha..1").valid &&
            !parse_version("0.6.0-alpha/1").valid, "malformed prerelease tags are refused");
  check(compare_versions(parse_version("0.6.0-alpha.999999999999999999999999"),
                         parse_version("0.6.0-alpha.1000000000000000000000000")) < 0,
        "large prerelease numbers compare without overflow");
  check(!parse_version("").valid, "an empty tag does not parse");
  check(!update_is_newer("nightly", "0.5.0"), "a junk tag is never newer");
  check(!update_is_newer("0.5.0", "0.5.0"), "the same version is not newer");
  check(update_is_newer("0.5.1", "0.5.0"), "one patch up is newer");

  check(update_check_due(0, 3000), "a first run checks");
  check(!update_check_due(1000, 1000 + 23 * 3600 * 1000), "23 hours is not due");
  check(update_check_due(1000, 1000 + 25 * 3600 * 1000), "25 hours is due");
  check(update_check_due(9000000000000LL, 1000), "a timestamp in the future must not lock checking out");
  check(!bx::update_should_check(false, false, false, 0, 1000), "an asleep engine does not check for updates");
  check(bx::update_should_check(true, false, false, 0, 1000), "an open panel checks when due");
  check(bx::update_should_check(false, true, false, 0, 1000), "a hidden buffering engine checks when due");
  check(!bx::update_should_check(true, true, true, 0, 1000), "an in-flight check is not duplicated");
  check(!bx::update_should_check(true, false, false, 1000, 1000 + 23LL * 3600 * 1000) &&
            bx::update_should_check(true, false, false, 1000, 1000 + 25LL * 3600 * 1000),
        "a long-running panel checks when the daily deadline arrives");

  check(update_should_offer("0.6.0", "", "0.5.0"), "a first offer shows");
  check(!update_should_offer("0.6.0", "0.6.0", "0.5.0"), "an offered version stays quiet");
  check(update_should_offer("0.7.0", "0.6.0", "0.5.0"), "a newer offer comes back");
  check(!update_should_offer("0.5.0", "", "0.5.0"), "the running version is never offered");

  check(update_should_open_notes("0.6.0", "0.5.0", "0.6.0"), "the version just installed opens its notes once");
  check(!update_should_open_notes("0.6.0", "0.6.0", "0.6.0"), "the same version does not open them again");
  check(!update_should_open_notes("0.6.0", "", ""), "a fresh install opens no sheet");
  check(!update_should_open_notes("0.6.0", "0.5.0", ""), "nothing pending, no sheet");
  check(!update_should_open_notes("0.7.0", "0.5.0", "0.6.0"), "a pending version that is not running stays closed");

  // The automatic path: a digest is what makes a release installable, the badge outlives Later, and a
  // manual recording is the one thing a restart may not cut.
  check(bx::update_is_installable("0.6.0", "0.5.0", "sha256:00"), "a newer release with a digest is installable");
  check(!bx::update_is_installable("0.6.0", "0.5.0", ""), "a release without a digest is never installed by itself");
  check(!bx::update_is_installable("0.5.0", "0.5.0", "sha256:00"), "the running version is not installable");
  check(bx::update_should_badge("0.6.0", "0.5.0") && !bx::update_should_badge("0.5.0", "0.5.0"),
        "the badge stays while a newer release is known");
  check(bx::update_should_prompt("0.6.0", "", "0.5.0"), "the prompt shows once");
  check(!bx::update_should_prompt("0.6.0", "0.6.0", "0.5.0"), "Later dismisses the prompt, not the badge");
  check(bx::update_apply_allowed(false) && !bx::update_apply_allowed(true),
        "a restart to update is refused while a manual recording runs");
  check(bx::update_install_exhausted(3) && !bx::update_install_exhausted(2), "three failed installs stop trying");
}

// ---------------------------------------------------------------- release notes to rows (update_notes.h)
void test_update_notes() {
  using bx::NoteKind;
  using bx::NoteRow;
  using bx::note_rows;

  const std::string body =
      "## Fixed\n"
      "- The ring no longer drops frames\n"
      "- **Timeline** saves faster\n"
      "\n"
      "Plain closing line.";
  const std::vector<NoteRow> rows = note_rows(body);
  check(rows.size() == 4, "a heading, two bullets and one paragraph");
  check(rows[0].kind == NoteKind::heading && rows[0].text == "Fixed", "## is a heading without its hashes");
  check(rows[1].kind == NoteKind::bullet && rows[1].text == "The ring no longer drops frames",
        "- is a bullet without its dash");
  check(rows[2].kind == NoteKind::bullet && rows[2].bold.size() == 1, "** marks one bold span");
  check(rows[2].text.substr(rows[2].bold[0].first, rows[2].bold[0].second) == "Timeline",
        "the bold span covers the word and not the markers");
  check(rows[3].kind == NoteKind::text && rows[3].text == "Plain closing line.", "a bare line is text");

  check(note_rows("").empty(), "an empty body has no rows");
  check(note_rows("# only\n# x").size() == 2, "a single # is a heading too");

  std::string many;
  for (int i = 0; i < 500; ++i) many += "- row\n";
  check(note_rows(many).size() == 200, "the rows stop at the cap");

  const std::vector<NoteRow> html = note_rows("<p>hi</p>");
  check(html.size() == 1 && html[0].text == "<p>hi</p>", "html is left as text, never interpreted");

  const std::vector<NoteRow> link = note_rows("- see [the notes](https://example.com/x)");
  check(link.size() == 1 && link[0].text == "see the notes", "a link keeps its text and drops its address");

  const std::vector<NoteRow> crlf = note_rows("## A\r\n- b\r\n");
  check(crlf.size() == 2 && crlf[0].text == "A" && crlf[1].text == "b", "a carriage return does not ride along");
}

// ---------------------------------------------------------------- the release feed (update_feed.h)
void test_update_feed() {
  using bx::parse_update_feed;
  check(bx::is_public_link(bx::kReleasesUrl) && bx::is_public_link(bx::kDiscordUrl),
        "the interface's public links may open");
  check(!bx::is_public_link("https://github.com/bulhaa1/BuffrX.evil.invalid") &&
            !bx::is_public_link("http://127.0.0.1/") && !bx::is_public_link("https://buffrx.app/@evil"),
        "caller supplied web addresses outside the public links are refused");
  const std::string payload =
      R"({"tag_name":"0.6.0","body":"## Fixed\n- ring","assets":[)"
      R"({"name":"BuffrX-Windows.zip","browser_download_url":"https://example.com/x.zip","digest":"sha256:1111"},)"
      R"({"name":"BuffrX-Setup.exe","browser_download_url":"https://example.com/x.exe","digest":"sha256:abcd"}]})";
  const bx::UpdateFeed feed = parse_update_feed(payload);
  check(feed.ok, "a release payload reads");
  check(feed.latest == "0.6.0", "the tag is the version");
  check(feed.notes == "## Fixed\n- ring", "the body is kept as it arrived");
  check(feed.url == "https://example.com/x.exe", "the installer asset is picked, not the zip");
  check(feed.digest == "sha256:abcd", "the digest keeps its sha256: prefix");
  check(feed.error.empty(), "a good feed says nothing");

  const bx::UpdateFeed no_asset =
      parse_update_feed(R"({"tag_name":"0.6.0","assets":[{"name":"BuffrX-Windows.zip"}]})");
  check(no_asset.ok && no_asset.url.empty(), "a release without the installer is still a release");
  check(no_asset.error.find("BuffrX-Setup.exe") != std::string::npos, "and the error names the asset");

  check(!parse_update_feed("{}").ok, "an empty object is not a release");
  check(!parse_update_feed("not json").ok, "junk is not a release");
}

// ---------------------------------------------------------------- a verified download (update_download.h)
void test_update_download() {
  using bx::digest_text_equals;
  using bx::sha256_hex_of;

  // The SHA-256 of the empty string, and of "abc": values any implementation must reproduce.
  check(sha256_hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "the empty digest is the known one");
  check(sha256_hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "abc hashes to the known digest");

  check(digest_text_equals("sha256:abc", "abc"), "the prefix is optional");
  check(digest_text_equals("sha256:ABC", "abc"), "case does not matter");
  check(digest_text_equals("abc", "abc"), "a bare digest still compares");
  check(!digest_text_equals("sha256:abc", "abd"), "a different digest does not match");
  check(!digest_text_equals("", ""), "two empty digests are not a match");
  check(!digest_text_equals("sha256:", "abc"), "an empty hex side is not a match");
  check(!digest_text_equals("md5:abc", "abc"), "another algorithm is not accepted");
}

// ---------------------------------------------------------------- the downloaded update on disk (update_pending.h)
void test_update_pending() {
  std::printf("update pending\n");
  const std::string root = temp_directory() + "\\update-pending";
  CreateDirectoryA(temp_directory().c_str(), nullptr);
  CreateDirectoryA(root.c_str(), nullptr);
  CreateDirectoryA(bx::update_dir(root).c_str(), nullptr);

  bx::PendingUpdate pending;
  pending.version = "0.6.0";
  pending.setup = bx::update_installer_path(root);
  pending.sha256 = "sha256:" + std::string(64, 'a');
  pending.notes = "notes";
  pending.download_attempts = 1;
  check(bx::update_pending_write(root, pending), "a downloaded update is written");
  bx::PendingUpdate read;
  std::string error;
  check(bx::update_pending_read(root, read, error) && read.version == "0.6.0" && read.setup == pending.setup,
        "and read back with its installer");
  check(bx::update_path_is_inside(root, pending.setup), "the installer inside the update folder is allowed");
  check(!bx::update_path_is_inside(root, root + "\\..\\elsewhere.exe"), "a path that escapes the folder is refused");
  check(!bx::update_path_is_inside(root, root + "\\update-evil\\x.exe"), "a sibling with the prefix is refused");

  DeleteFileA(pending.setup.c_str());
  check(!bx::update_pending_verify(read, error), "a missing installer is not ready to install");
  {
    FILE* file = std::fopen(pending.setup.c_str(), "wb");
    check(file != nullptr, "an installer fixture can be created");
    if (file) { std::fwrite("abc", 1, 3, file); std::fclose(file); }
  }
  check(!bx::update_pending_verify(read, error), "a corrupted installer is not ready to install");
  read.sha256 = "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  check(bx::update_pending_verify(read, error), "an installer matching its digest is ready");

  check(bx::update_make_apply_marker(root) && bx::update_marker_exists(root), "the launcher's marker is made");
  bx::update_pending_clear(root);
  check(!bx::update_marker_exists(root), "clearing removes the marker and the state");

  // A running version at or past the pending one removes the leftovers; an older one keeps them for a retry.
  check(bx::update_pending_write(root, pending), "a pending update is written again");
  bx::update_cleanup_applied(root, "0.5.0");
  check(bx::update_pending_read(root, read, error), "an older running version keeps the update");
  bx::update_cleanup_applied(root, "0.6.0");
  check(!bx::update_pending_read(root, read, error), "the version that installed it clears it");
}

// ---------------------------------------------------------------- a fresh install is quiet (config.h)
void test_config_defaults() {
  const bx::Config config;
  // The product starts with Windows by default (decided with the user 2026-11-10): a buffer that is not
  // running protects nothing. The cost is a persistence signal antivirus watches, accepted knowingly.
  check(config.launch_on_startup, "a fresh install starts with Windows, as the product needs");
}

// ---------------------------------------------------------------- what a finished save came to (saved_replay.h)
void test_saved_replay_verdict() {
  using bx::SavedReplayOutcome;

  // An output without the verdict procedure (OBS's own replay buffer) keeps the route it had.
  bx::SavedReplayVerdict none;
  check(bx::saved_replay_outcome(none) == SavedReplayOutcome::unknown,
        "an output with no save_result leaves the host on get_last_replay");

  bx::SavedReplayVerdict clip;
  clip.known = true;
  clip.finished = true;
  clip.ok = true;
  clip.path = "C:/clips/Replay.mp4";
  check(bx::saved_replay_outcome(clip) == SavedReplayOutcome::file_clip, "a finished, clean save is filed");
  check(!bx::saved_replay_left_footage_out(clip), "a clean clip has no gap");

  // A muxer that failed leaves a partial file: it is a failure however many bytes are on disk.
  bx::SavedReplayVerdict failed = clip;
  failed.ok = false;
  failed.muxer_exit = 3;
  check(bx::saved_replay_outcome(failed) == SavedReplayOutcome::failed,
        "a failed muxer is not a clip even though its part file exists");
  check(failed.path == "C:/clips/Replay.mp4", "the partial file's path is kept for the log, not filed");
  failed.partial = true;
  failed.skipped = 4;
  check(bx::saved_replay_outcome(failed) == SavedReplayOutcome::failed,
        "a failed save stays a failure whatever it managed to write");

  // A verdict that never finished, or a clean save with no name, is a failure the host must not announce.
  bx::SavedReplayVerdict unfinished = clip;
  unfinished.finished = false;
  check(bx::saved_replay_outcome(unfinished) == SavedReplayOutcome::failed, "an unfinished verdict is not filed");
  bx::SavedReplayVerdict unnamed = clip;
  unnamed.path.clear();
  check(bx::saved_replay_outcome(unnamed) == SavedReplayOutcome::failed, "a save with no path cannot be filed");

  // A clip that leaves footage out is still a clip, and it is reported.
  bx::SavedReplayVerdict gap = clip;
  gap.partial = true;
  check(bx::saved_replay_outcome(gap) == SavedReplayOutcome::file_clip, "a partial clip is filed");
  check(bx::saved_replay_left_footage_out(gap), "a partial clip is reported");
  bx::SavedReplayVerdict skipped = clip;
  skipped.skipped = 12;
  check(bx::saved_replay_left_footage_out(skipped), "skipped records are reported");
  bx::SavedReplayVerdict unreadable = clip;
  unreadable.unreadable = true;
  check(bx::saved_replay_left_footage_out(unreadable), "an unreadable disk part is reported");

  // The verdict carries a count, not a cause. Only an unreadable disk part may be reported as a disk that could
  // not be read back: a count on its own also comes from a part the writer could not join or a chunk file that was
  // gone, and naming the disk sent the reader to a disk that was fine.
  check(bx::saved_replay_gap(clip) == bx::SavedReplayGap::none, "a clean clip has no gap to explain");
  check(bx::saved_replay_gap(unreadable) == bx::SavedReplayGap::unreadable_disk,
        "an unreadable disk part is the one disk read");
  check(bx::saved_replay_gap(skipped) == bx::SavedReplayGap::skipped_records,
        "a skipped count on its own names no cause");
  check(bx::saved_replay_gap(gap) == bx::SavedReplayGap::part_only,
        "a clip that is only part of the buffer has no count to explain it");
  bx::SavedReplayVerdict disk_and_count = unreadable;
  disk_and_count.skipped = 7;
  check(bx::saved_replay_gap(disk_and_count) == bx::SavedReplayGap::unreadable_disk,
        "a disk read with a count is still a disk read");

  const auto mentions = [](const char* text, const char* word) {
    return text && std::string(text).find(word) != std::string::npos;
  };
  const bx::SavedReplayGapText disk_text = bx::saved_replay_gap_text(bx::SavedReplayGap::unreadable_disk);
  const bx::SavedReplayGapText skip_text = bx::saved_replay_gap_text(bx::SavedReplayGap::skipped_records);
  const bx::SavedReplayGapText part_text = bx::saved_replay_gap_text(bx::SavedReplayGap::part_only);
  check(mentions(disk_text.log_reason, "could not be read") && mentions(disk_text.detail, "read back from the disk"),
        "an unreadable disk part keeps the sentence that names the disk");
  check(!mentions(skip_text.log_reason, "disk") && !mentions(skip_text.detail, "disk"),
        "a count that is not the disk does not blame the disk");
  check(!mentions(part_text.log_reason, "disk") && !mentions(part_text.detail, "disk"),
        "a clip that is only part of the buffer does not blame the disk");
  // Nor may the rest of the buffer be declared gone when the verdict never said what happened to it.
  check(!mentions(skip_text.log_reason, "gone") && !mentions(skip_text.detail, "gone"),
        "a skipped count does not claim the rest of the buffer was gone");
  check(!mentions(part_text.log_reason, "gone") && !mentions(part_text.detail, "gone"),
        "a short clip does not claim the rest of the buffer was gone");
  check(disk_text.title[0] && skip_text.title[0] && part_text.title[0] && disk_text.detail[0] &&
            skip_text.detail[0] && part_text.detail[0],
        "every reported gap is said to the person, not only written to the log");

  // A salvage is announced where its save really ended, never where it was asked for. Its own save may fail after
  // the request was queued - a muxer that died, a buffer restarted under it, a timeout, a file that did not verify -
  // and none of those may be called saved.
  check(bx::salvage_notice(false, bx::SalvageResult::filed).title == nullptr,
        "a clip the person asked for never wears the salvage card");
  check(bx::salvage_notice(true, bx::SalvageResult::unverified).title == nullptr &&
            bx::salvage_notice(true, bx::SalvageResult::unverified).detail == nullptr,
        "a saved buffer that did not verify is not announced at all");
  const bx::SalvageNotice salvage_saved = bx::salvage_notice(true, bx::SalvageResult::filed);
  check(salvage_saved.title && std::string(salvage_saved.title) == "Buffer saved",
        "a verified, filed salvage keeps the words the product already used");
  check(salvage_saved.detail && std::string(salvage_saved.detail) == "Capture failed, so your buffer was saved",
        "the salvage's success text is unchanged");
  const bx::SalvageNotice salvage_kept = bx::salvage_notice(true, bx::SalvageResult::kept_unfiled);
  check(salvage_kept.title && std::string(salvage_kept.title) != "Buffer saved",
        "bytes that could not be filed are not called saved");
  check(!mentions(salvage_kept.detail, "saved") && mentions(salvage_kept.detail, "clips folder"),
        "a kept buffer says where its bytes are and claims no more");
}

// Compiled from tests/link_secret_tests.cpp, tests/ring_save_tests.cpp and tests/save_publish_tests.cpp
// (BUFFRX_TEST_SOURCES): they drive a whole LinkService and real files, so each keeps its own counter and returns how
// many checks failed.
int test_link_short_secret();
int test_link_short_secret_migration();
int test_link_short_secret_replaced();
int test_link_short_secret_duplicate();
int test_link_short_secret_helper_expired();
int test_link_short_secret_local_expired();
int test_ring_save_names();
int test_ring_save_claims();
int test_ring_save_result();
int test_ring_save_parts();
int test_save_publish();

int main() {
  char helper_mode[32] = {};
  const DWORD helper_length = GetEnvironmentVariableA("BUFFRX_LINK_TEST_HELPER", helper_mode, sizeof(helper_mode));
  if (helper_length > 0 && helper_length < sizeof(helper_mode)) return link_test_helper(helper_mode);
  std::printf("BuffrX engine tests\n");
  remove_tree(temp_directory());
  test_json();
  test_config();
  test_config_change_bits();
  test_config_timeline();
  test_chords();
  test_gamepad_chords();
  test_gamepad_fire();
  test_gamepad_triggers();
  test_screenshot_game_title();
  test_pad_nav();
  test_library_guards();
  test_annotate_skip();
  test_benchmark();
  test_canvas_plan();
  test_display_target();
  test_clip_check();
  test_clip_check_large();
  test_mp4_edit();
  test_media_math();
  test_media_join_spec();
  test_media_join_process();
  test_share();
  test_link_idle();
  test_share_secret();
  test_webhook_list();
  test_elevation_policy();
  test_av1_level();
test_themes();
test_motion();
  test_capacity_memory();
test_lower_ascii();
test_upper_codepoint();
test_card_title();
  test_links();
  g_failures += test_link_short_secret();
  g_failures += test_link_short_secret_migration();
  g_failures += test_link_short_secret_replaced();
  g_failures += test_link_short_secret_duplicate();
  g_failures += test_link_short_secret_helper_expired();
  g_failures += test_link_short_secret_local_expired();
  test_link_helper_failures();
  test_chunk_math();
  test_profile_patch();
  test_encoder_settings();
  test_bitrate_plan();
  test_thumbnail_budget();
  test_rebuild_gate();
  test_abandoned_outputs();
  test_exclusion_view();
  test_startup_switches();
  test_session_store_root();
  test_rec_mark();
  test_record_action();
  test_quick_panel_size();
  test_panel_size();
  test_interface_scale();
  test_card_grid();
  test_lock_watch();
  test_frame_stats();
  test_protocol_sync();
  test_anim_math();
  test_live_math();
  test_link_math();
  test_work_queue();
  test_engine_restart();
  test_encoder_geometry();
  test_command_policy();
  test_idle_policy();
  test_webview_present();
  test_process_path_is();
  test_capture_follows_focus();
  test_game_detection();
  test_buffer_watch();
  test_encoder_watch();
  test_encoder_governor();  test_gpu_adapter_choice();
  test_encoder_modes();
  test_recording_watch();
  test_recovery_policy();
  test_game_fullscreen_rule();
  test_clip_saved_detail();
  test_view_clip_hotkey();
  test_hotkey_enable();
  test_performance_wording();
  test_performance();
  test_performance_modes();
  test_process_tuning();
  test_toast_decision();
  test_toast();
  test_session_store();
  test_timeline_file_index();
  test_timeline_chunks();
  test_timeline_store_reads();
  test_recovery_path_forms();
  test_dir_listing();
  test_file_finality();
  test_timeline_state();
  test_timeline_manual_stop();
  test_buffer_stop_route();
  test_session_lock_commands();
  test_window_thread_routes();
  test_ui_host_lifetimes();
  test_ui_cache_and_motion_budgets();
  test_crash_and_mark_lifetimes();
  test_command_guards();
  test_pipe_and_interface_guards();
  test_lifetime_guards();
  test_timeline_preview_press();
  test_timeline_overlay();
  test_recordings_listing();
  test_timeline_retention();
  test_timeline_retention_gate();
  test_timeline_retention_no_stall();
  test_timeline_dedupe_sweep_concurrent();
  test_timeline_erase_lease_claim();
  test_timeline_read_guard();
  test_timeline_save_read_guard();
  test_timeline_prune_index_races();
  test_timeline_leases();
  test_timeline_save_plan();
  test_windowed_game();
  test_timeline_thumbs();
  test_timeline_loudness();
  test_stream_compatibility();
  test_gpu_vendor();
  test_smooth_level();
  test_audio_meter();
  test_encoder_candidates();
  test_volume_free_bytes();
  test_save_check();
  test_save_filing();
  test_saved_replay_verdict();
  g_failures += test_ring_save_names();
  g_failures += test_ring_save_claims();
  g_failures += test_ring_save_result();
  g_failures += test_ring_save_parts();
  g_failures += test_save_publish();
  test_pipe_server();
  test_pipe_queue_bounded();
  test_pipe_client_leaves();
  test_capture_enum();
  test_validate_capture_target();
  test_log_policy();
  test_log_file();
  test_loop_wait();
  test_process_cache();
  test_sleep_wait();
  test_ring_index();
  test_memory_policy();
  test_ring_spill_policy();
  test_save_order();
  test_ring_index_disk();
  test_replay_store();
  test_ring_disk();
  test_ring_store_model();
  test_ring_store_threads();
  test_packet_hash();
  test_replay_store_ram_only();
  test_timeline_link();
  test_ring_index_chunks();
  test_dedupe_store();
  test_dedupe_partial_save();
  test_dedupe_chunk_sessions();
  test_dedupe_policy();
  test_save_part_names();
  test_plan_evicted_chunk();
  test_ceiling_floor();
  test_key_hash_cost();
  test_ui_format();
  test_ui_icons();
  test_update_math();
  test_update_notes();
  test_update_feed();
  test_update_download();
  test_update_pending();
  test_config_defaults();
  test_native_path();
  test_wide_text();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
