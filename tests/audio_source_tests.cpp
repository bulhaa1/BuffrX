// Per-application live gain and mute (audio_routing.h, config.h): a selected program carries its own
// volume and mute, saved with the route and sent on the pipe, and changing either is a live control
// change - never a routing change, so it does not rebuild the encoders, empty the ring or replace the
// application's capture source.
// Built into buffrx-tests: one line in tests/engine_tests.cpp's main() and this file in BUFFRX_TEST_SOURCES.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "../src/engine/audio_routing.h"
#include "../src/engine/config.h"
#include "../src/shared/json.h"

namespace {

void expect(int& failures, bool ok, const char* what) {
  if (ok) return;
  std::printf("  FAIL %s\n", what);
  ++failures;
}

bx::Json stored_app(const char* exe, const char* window, const bx::Json& volume, const bx::Json& muted) {
  bx::Json entry = bx::Json::object();
  entry.set("window", bx::Json(window));
  entry.set("exe", bx::Json(exe));
  entry.set("title", bx::Json(exe));
  if (!volume.is_null()) entry.set("volume", volume);
  if (!muted.is_null()) entry.set("muted", muted);
  return entry;
}

bx::Config apps_config(std::vector<bx::AudioApp> apps) {
  bx::Config config;
  config.audio_capture_mode = "apps";
  config.audio_apps = std::move(apps);
  return config;
}

bx::AudioApp named_app(const char* exe, double volume, bool muted) {
  bx::AudioApp app;
  app.window = exe;
  app.exe = exe;
  app.title = exe;
  app.volume = volume;
  app.muted = muted;
  return app;
}

}  // namespace

int test_audio_source_controls() {
  std::printf("application source controls\n");
  int failures = 0;

  // A route with no controls set records the program at full volume, unmuted.
  const bx::AudioApp fresh;
  expect(failures, fresh.volume == 1.0 && !fresh.muted, "a new route starts at full volume and unmuted");

  // A named program's gain and mute survive the config file, and come back on the pipe.
  const bx::Config saved = apps_config({named_app("Discord.exe", 0.35, true)});
  bx::Config reloaded;
  bx::config_from_json(bx::config_to_json(saved), reloaded);
  expect(failures,
         reloaded.audio_apps.size() == 1 && reloaded.audio_apps[0].volume == 0.35 && reloaded.audio_apps[0].muted,
         "a named app's gain and mute survive a restart");
  const bx::Json wire = bx::audio_apps_json(reloaded.audio_apps);
  expect(failures,
         wire.items().size() == 1 && wire.items()[0]["volume"].as_double(0.0) == 0.35 &&
             wire.items()[0]["muted"].as_bool(false),
         "the pipe carries each route's gain and mute");

  // A file written before these keys existed records exactly as it did: full volume, unmuted.
  bx::Json legacy = bx::Json::object();
  bx::Json legacy_list = bx::Json::array();
  legacy_list.push(stored_app("Discord.exe", "Discord", bx::Json(), bx::Json()));
  legacy.set("audio_apps", legacy_list);
  bx::Config old;
  bx::config_from_json(legacy, old);
  expect(failures,
         old.audio_apps.size() == 1 && old.audio_apps[0].volume == 1.0 && !old.audio_apps[0].muted,
         "a stored route without gain keys still records at full volume, unmuted");

  // An out-of-range gain is clamped, and a value that is not a number is never stored as one.
  const auto stored_volume = [](const bx::Json& volume) {
    bx::Json patch = bx::Json::object();
    bx::Json list = bx::Json::array();
    list.push(stored_app("Discord.exe", "Discord", volume, bx::Json()));
    patch.set("apps", list);
    bx::Config config;
    bx::config_apply_audio(patch, config);
    return config.audio_apps.empty() ? -1.0 : config.audio_apps[0].volume;
  };
  expect(failures, stored_volume(bx::Json(-0.5)) == 0.0, "a gain below zero is clamped to silence");
  expect(failures, stored_volume(bx::Json(2.5)) == 1.0, "a gain above one is clamped to full volume");
  const double nan_gain = stored_volume(bx::Json(std::numeric_limits<double>::quiet_NaN()));
  expect(failures, std::isfinite(nan_gain) && nan_gain == 1.0, "a gain that is not a number falls back to full volume");

  // Changing only gain or mute keeps routing identity: the application stays on its track and its
  // source is never replaced, so the plan's encoders are not rebuilt and a buffer or a recording
  // already running is not cut.
  const std::vector<bx::AudioApp> quiet = {named_app("Discord.exe", 1.0, false), named_app("Game.exe", 1.0, false)};
  const std::vector<bx::AudioApp> changed = {named_app("Discord.exe", 0.5, true), named_app("Game.exe", 1.0, false)};
  expect(failures, bx::audio_apps_equal(quiet, changed), "a gain change is not a routing change");
  expect(failures, !bx::audio_app_controls_equal(quiet, changed), "a gain change is seen as a live control change");
  expect(failures, bx::audio_app_controls_equal(quiet, quiet), "the same routes have the same controls");
  const bx::Config quiet_config = apps_config(quiet);
  const bx::Config changed_config = apps_config(changed);
  expect(failures, bx::config_change_bits(quiet_config, changed_config) == 0,
         "a gain or mute change never rebuilds the outputs");
  const bx::AudioRoutingPlan plan = bx::plan_audio_routing(changed_config);
  expect(failures, plan.apps.size() == 2 && plan.apps[0].volume == 0.5 && plan.apps[0].muted &&
                       plan.apps[0].track == bx::kAudioAppFirstTrack,
         "the running plan carries the new gain on the slot's own track");

  // A different program is a routing change and does rebuild; the rules stay separate.
  const std::vector<bx::AudioApp> other = {named_app("Other.exe", 0.5, true), named_app("Game.exe", 1.0, false)};
  expect(failures, !bx::audio_apps_equal(quiet, other), "another program is a different route");
  expect(failures, (bx::config_change_bits(quiet_config, apps_config(other)) & bx::kConfigRebuilds) != 0,
         "another program still rebuilds the outputs");

  return failures;
}
