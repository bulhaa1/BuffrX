// Automatic app-audio routing (audio_auto.h): grouping, own-process exclusion, the sticky lane memory,
// the Other audio overflow, preference merging and the recorded lane journal.
// Built into buffrx-tests: one line in tests/engine_tests.cpp's main() and this file in BUFFRX_TEST_SOURCES.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/engine/audio_auto.h"
#include "../src/engine/audio_layout.h"
#include "../src/engine/audio_routing.h"
#include "../src/engine/config.h"
#include "../src/shared/files.h"
#include "../src/shared/json.h"

namespace {

void expect(int& failures, bool ok, const char* what) {
  if (ok) return;
  std::printf("  FAIL %s\n", what);
  ++failures;
}

bx::AutoAudioSession session(uint32_t pid, const char* exe, bool active = true, uint64_t changed = 1000) {
  bx::AutoAudioSession one;
  one.key = std::string(exe) + ":" + std::to_string(pid);
  one.pid = pid;
  one.exe = exe;
  one.active = active;
  one.changed_ms = changed;
  return one;
}

bx::AutoProcessNode process(uint32_t pid, uint32_t parent, const char* exe) {
  bx::AutoProcessNode node;
  node.pid = pid;
  node.parent_pid = parent;
  node.exe = exe;
  return node;
}

const bx::AutoAudioLane* lane_at(const bx::AutoAudioPlan& plan, int track) {
  for (const bx::AutoAudioLane& lane : plan.lanes) {
    if (lane.track == track) return &lane;
  }
  return nullptr;
}

}  // namespace

int test_audio_auto() {
  std::printf("automatic audio\n");
  int failures = 0;

  // Membership reconciliation keeps the same capture unless the PID identity changes.
  expect(failures, bx::auto_root_retain({42, 100}, {42, 100}), "unchanged root retains its capture");
  expect(failures, bx::auto_root_retain({42, 0}, {42, 100}), "unknown creation time retains its capture");
  expect(failures, !bx::auto_root_retain({42, 100}, {42, 200}), "reused PID replaces its capture");
  expect(failures, !bx::auto_root_retain({42, 100}, {43, 100}), "different PID replaces its capture");

  expect(failures, !bx::auto_tree_refresh_due(5999, 1000), "the process tree refresh waits five seconds");
  expect(failures, bx::auto_tree_refresh_due(6000, 1000), "a stable session refreshes the process tree");

  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.sessions = {session(38, "parent.exe")};
    input.processes = {process(38, 0, "parent.exe")};
    input.prefs["excluded.exe"].excluded = true;
    bx::AutoLaneMemory memory;
    expect(failures, !bx::plan_auto_audio(input, memory).lanes.empty(), "parent starts safely captured");
    input.processes.push_back(process(39, 38, "excluded.exe"));
    const auto refreshed = bx::plan_auto_audio(input, memory);
    expect(failures, refreshed.lanes.empty() && refreshed.uncaptured.size() == 1,
           "refresh blocks a new excluded child even without a new session");
  }

  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(40, 0, "browser.exe"), process(41, 40, "browser.exe"),
                       process(42, 40, "excluded.exe")};
    input.processes[0].created_ms = 300;
    input.processes[1].created_ms = 100;
    input.processes[2].created_ms = 100;
    input.sessions = {session(40, "browser.exe"), session(41, "browser.exe")};
    input.prefs["excluded.exe"].excluded = true;
    bx::AutoLaneMemory memory;
    const auto plan = bx::plan_auto_audio(input, memory);
    expect(failures, plan.lanes.size() == 1 && plan.lanes[0].roots.size() == 2 && plan.uncaptured.empty(),
           "a reused parent PID neither combines old children nor blocks the new process");
  }

  // BuffrX's own players are never captured, including the WebView2 children below the editor. A game
  // session is captured; the engine and a webview child of the editor are not.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(10, 0, "buffrx-ui.exe"), process(11, 10, "buffrx-engine.exe"),
                       process(12, 10, "msedgewebview2.exe"), process(20, 0, "game.exe")};
    input.sessions = {session(11, "buffrx-engine.exe"), session(12, "msedgewebview2.exe"),
                      session(20, "game.exe")};
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    expect(failures, plan.lanes.size() == 1 && plan.lanes[0].exe == "game.exe",
           "BuffrX's own playback is excluded and a game is captured");
  }

  // Two sessions of one executable are one group and one capture: the same PCM is never recorded twice.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.sessions = {session(30, "browser.exe", true, 1000), session(31, "browser.exe", true, 1200)};
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    int browser = 0;
    for (const bx::AutoAudioLane& lane : plan.lanes) {
      if (bx::auto_pref_key(lane.exe) == "browser.exe") ++browser;
    }
    expect(failures, browser == 1, "two sessions of one executable make one lane");
  }

  // Two independent process trees of one executable (two browser profiles) are one lane, one preference and
  // one bus, and both trees are captured: keeping only the lowest pid lost one of the two sounds.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(801, 0, "browser.exe"), process(802, 0, "browser.exe")};
    input.sessions = {session(801, "browser.exe", true, 1000), session(802, "browser.exe", true, 1100)};
    input.prefs["browser.exe"].volume = 0.5;
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    const bx::AutoAudioLane* lane = lane_at(plan, 3);
    expect(failures, plan.lanes.size() == 1 && lane && lane->roots.size() == 2,
           "two disjoint trees of one executable are one lane with both trees");
    expect(failures, lane && lane->roots[0].pid == 801 && lane->roots[1].pid == 802,
           "both roots are kept, in a stable order");
    expect(failures, lane && lane->apps.size() == 1 && lane->apps[0].roots.size() == 2,
           "the lane's one row carries both trees, so both are captured");
    expect(failures, lane && lane->volume == 0.5 && lane->apps[0].volume == 0.5,
           "the executable's preference applies once to every tree");
  }

  // A tree that is already inside another tree of the same executable is the same PCM: one root, not two.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(810, 0, "browser.exe"), process(811, 810, "browser.exe")};
    input.sessions = {session(810, "browser.exe", true, 1000), session(811, "browser.exe", true, 1100)};
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    const bx::AutoAudioLane* lane = lane_at(plan, 3);
    expect(failures, lane && lane->roots.size() == 1 && lane->roots[0].pid == 810,
           "a session inside another tree of its own program is captured once");
  }

  // Coverage removes a covered root, not a program: a helper with one tree inside a captured parent and one
  // of its own keeps the tree that only it can capture.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(820, 0, "parent.exe"), process(821, 820, "helper.exe"),
                       process(822, 0, "helper.exe")};
    input.sessions = {session(820, "parent.exe", true, 1000), session(821, "helper.exe", true, 1100),
                      session(822, "helper.exe", true, 1200)};
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    const bx::AutoAudioLane* helper = nullptr;
    for (const bx::AutoAudioLane& lane : plan.lanes) {
      if (bx::auto_pref_key(lane.exe) == "helper.exe") helper = &lane;
    }
    expect(failures, helper && helper->roots.size() == 1 && helper->roots[0].pid == 822,
           "a program keeps the tree that is not inside another program's capture");
  }

  // Windows' process loopback takes the whole tree below its root and cannot subtract one descendant, so a
  // tree holding an explicitly excluded program is not captured at all, and says so.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(830, 0, "parent.exe"), process(831, 830, "discord.exe")};
    input.sessions = {session(830, "parent.exe", true, 1000), session(831, "discord.exe", true, 1100)};
    input.prefs["discord.exe"].excluded = true;
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    expect(failures, plan.lanes.empty(), "a tree holding an excluded program is never captured through it");
    expect(failures, plan.uncaptured.size() == 1 && plan.uncaptured[0].exe == "parent.exe" &&
                         plan.uncaptured[0].state == "blocked",
           "the refused parent is reported as blocked, not silently dropped");
  }

  // An unsafe parent cannot suppress an otherwise safe sibling capture.
  for (bool own : {false, true}) {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(850, 0, "parent.exe"), process(851, 850, own ? "buffrx-ui.exe" : "excluded.exe"),
                       process(852, 850, "helper.exe"), process(853, 0, "helper.exe")};
    input.sessions = {session(852, "helper.exe"), session(850, "parent.exe"), session(853, "helper.exe")};
    input.prefs["excluded.exe"].excluded = true;
    bx::AutoLaneMemory memory;
    const auto plan = bx::plan_auto_audio(input, memory);
    const auto* lane = lane_at(plan, 3);
    expect(failures, lane && lane->roots.size() == 2 && lane->roots[0].pid == 852 && lane->roots[1].pid == 853,
           "safe sibling and independent root survive an unsafe parent");
  }

  // The same rule protects BuffrX's own playback: a player below a captured tree would be recorded by it.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(840, 0, "parent.exe"), process(841, 840, "buffrx-ui.exe")};
    input.sessions = {session(840, "parent.exe", true, 1000), session(841, "buffrx-ui.exe", true, 1100)};
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    expect(failures, plan.lanes.empty() && plan.uncaptured.size() == 1 &&
                         plan.uncaptured[0].state == "blocked",
           "a tree holding BuffrX's own playback is not captured either");
  }

  // The process table is walked only for a relevant event: a new, gone or relaunched program, or a
  // just-started one missing from the table. A pause and a fader are answered from the snapshot held.
  {
    const std::vector<bx::AutoAudioSession> sessions = {session(901, "a.exe", true, 1000)};
    const std::vector<bx::AutoProcessNode> processes = {process(901, 0, "a.exe")};
    const std::vector<std::string> identities = bx::auto_session_identities(sessions);
    expect(failures, !bx::auto_snapshot_due(sessions, identities, true, processes),
           "an unchanged session set needs no process walk");
    std::vector<bx::AutoAudioSession> paused = sessions;
    paused[0].active = false;
    expect(failures, !bx::auto_snapshot_due(paused, identities, true, processes),
           "a pause is answered from the snapshot already held");
    std::vector<bx::AutoAudioSession> relaunched = sessions;
    relaunched[0].key = "a.exe:902";
    relaunched[0].pid = 902;
    expect(failures, bx::auto_snapshot_due(relaunched, identities, true, processes),
           "a relaunch asks for a fresh process snapshot");
    expect(failures, bx::auto_snapshot_due(sessions, identities, true, {}),
           "an active session missing from the table asks again");
    expect(failures, bx::auto_snapshot_due(sessions, identities, false, processes),
           "the first pass always takes a snapshot");
  }

  // A 32-bit tick from a session index or a recording is placed on the journal's 64-bit clock, across the
  // wrap that happens after 49 days of uptime.
  {
    const uint64_t early = 5000;  // an engine started minutes ago: no wrap has happened
    expect(failures, bx::auto_tick64(4000u, early) == 4000ull, "before the first wrap a 32-bit tick is itself");
    const uint64_t now = 0x100000100ull;  // just past a 32-bit wrap
    expect(failures, bx::auto_tick64(0x100u, now) == now, "a tick on this side of the wrap is placed as is");
    expect(failures, bx::auto_tick64(0xFFFFFF00u, now) == 0xFFFFFF00ull,
           "a tick from before the wrap is placed before it");
  }

  // A child process of another captured program is already inside its parent's process tree: one
  // source, never two captures of the same PCM.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes = {process(21, 0, "parent.exe"), process(22, 21, "helper.exe")};
    input.sessions = {session(21, "parent.exe", true, 1000), session(22, "helper.exe", true, 1100)};
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    expect(failures, plan.lanes.size() == 1 && plan.lanes[0].exe == "parent.exe",
           "a program inside another's process tree is captured once");
  }

  // Three isolated lanes at most; every further program shares track 6, and none is dropped. With nine
  // programs all nine are in the plan (three isolated, six in Other audio).
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.processes.push_back(process(100, 0, "explorer.exe"));
    for (uint32_t index = 0; index < 9; ++index) {
      const std::string exe = "app" + std::to_string(index) + ".exe";
      input.processes.push_back(process(100 + index, 0, exe.c_str()));
      input.sessions.push_back(session(100 + index, exe.c_str(), true, 1000 + index));
    }
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    const bx::AutoAudioLane* other = lane_at(plan, 6);
    int isolated = 0;
    for (const bx::AutoAudioLane& lane : plan.lanes) {
      if (!lane.other) ++isolated;
    }
    expect(failures, isolated == 3, "at most three programs are isolated");
    expect(failures, other && other->apps.size() == 6, "every remaining program is in Other audio, none dropped");
    expect(failures, other && other->name == "Other audio" && other->exe.empty(),
           "the overflow lane is named Other audio and carries one bus");
  }

  // Sticky memory: a program arriving does not move the ones already captured.
  {
    bx::AutoLaneMemory memory;
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    for (const char* exe : {"a.exe", "b.exe", "c.exe"}) {
      input.sessions.push_back(session(200 + static_cast<uint32_t>(memory.lane_of.size()), exe, true,
                                       static_cast<uint64_t>(1000 + memory.lane_of.size())));
    }
    input.sessions[0].pid = 201;
    input.sessions[1].pid = 202;
    input.sessions[2].pid = 203;
    const bx::AutoAudioPlan first = bx::plan_auto_audio(input, memory);
    const int a_track = memory.lane_of["a.exe"];
    const int b_track = memory.lane_of["b.exe"];
    const int c_track = memory.lane_of["c.exe"];
    expect(failures, a_track == 3 && b_track == 4 && c_track == 5, "three programs take tracks 3, 4 and 5");

    input.sessions.push_back(session(204, "d.exe", true, 4000));
    const bx::AutoAudioPlan second = bx::plan_auto_audio(input, memory);
    expect(failures, memory.lane_of["a.exe"] == a_track && memory.lane_of["b.exe"] == b_track &&
                         memory.lane_of["c.exe"] == c_track,
           "a fourth program does not move the captured ones");
    expect(failures, lane_at(second, 6) != nullptr && lane_at(second, 6)->apps.size() == 1,
           "the fourth program goes to Other audio");
  }

  // A freed lane goes to the captured game first.
  {
    bx::AutoLaneMemory memory;
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.sessions = {session(301, "a.exe", true, 1000), session(302, "b.exe", true, 1100),
                      session(303, "c.exe", true, 1200)};
    bx::plan_auto_audio(input, memory);
    input.sessions.erase(input.sessions.begin() + 2);  // c leaves, track 5 is free
    input.game_exe = "game.exe";
    input.sessions.push_back(session(304, "game.exe", true, 3000));
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    expect(failures, memory.lane_of["game.exe"] == 5, "the captured game takes a freed isolated lane");
    expect(failures, lane_at(plan, 5) && lane_at(plan, 5)->exe == "game.exe", "and the plan shows it there");
  }

  // Grace: a brief pause keeps the row; 1.5 s after it went inactive, it is gone. Excluded stays out and
  // never falls back to the desktop.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.sessions = {session(501, "quiet.exe", false, 4000)};
    bx::AutoLaneMemory memory;
    expect(failures, bx::plan_auto_audio(input, memory).lanes.size() == 1,
           "an inactive session is visible inside the grace");
    input.now_ms = 6000;
    expect(failures, bx::plan_auto_audio(input, memory).lanes.empty(),
           "an inactive session leaves after the grace");
    input.sessions = {session(502, "excluded.exe", true, 6000)};
    input.prefs["excluded.exe"].excluded = true;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    expect(failures, plan.lanes.empty(), "an excluded program is never captured and never falls back to desktop");
  }

  // A relaunch of the same program keeps its lane; the process id is new but the identity is the executable.
  {
    bx::AutoLaneMemory memory;
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.sessions = {session(601, "discord.exe", true, 1000), session(602, "game.exe", true, 1100)};
    bx::plan_auto_audio(input, memory);
    const int track = memory.lane_of["discord.exe"];
    input.sessions[0] = session(699, "discord.exe", true, 4000);
    bx::plan_auto_audio(input, memory);
    expect(failures, memory.lane_of["discord.exe"] == track, "a relaunched program keeps its lane");
  }

  // The Other group's own gain and mute are applied on top of each member's saved values.
  {
    bx::AutoAudioInputs input;
    input.now_ms = 5000;
    input.other_volume = 0.5;
    input.other_muted = true;
    input.prefs["d.exe"].volume = 0.8;
    input.sessions = {session(701, "a.exe"), session(702, "b.exe"), session(703, "c.exe"),
                      session(704, "d.exe")};
    bx::AutoLaneMemory memory;
    const bx::AutoAudioPlan plan = bx::plan_auto_audio(input, memory);
    const bx::AutoAudioLane* other = lane_at(plan, 6);
    expect(failures, other && other->volume == 0.5 && other->muted, "the group control is the lane's own level");
    expect(failures, other && !other->apps.empty() && other->apps[0].volume == 0.4 && other->apps[0].muted,
           "a member's effective level is its saved gain times the group's, and either mute silences it");
  }

  // Configuration: an existing desktop/apps file migrates to automatic, keeps the old mode and routes,
  // and seeds preferences from the routes' saved gain and mute.
  {
    bx::Config config;
    config.config_version = 12;
    config.audio_capture_mode = "apps";
    bx::AudioApp app;
    app.window = "Discord:Chrome_WidgetWin_1:Discord.exe";
    app.exe = "Discord.exe";
    app.title = "Discord";
    app.volume = 0.35;
    app.muted = true;
    config.audio_apps = {app};
    bx::migrate_config(config, true);
    expect(failures, config.audio_capture_mode == "auto", "an existing file moves to automatic capture");
    expect(failures, config.audio_capture_legacy_mode == "apps", "its old mode is kept for compatibility");
    expect(failures, config.audio_apps.size() == 1, "its routes are untouched");
    const auto pref = config.audio_app_prefs.find("discord.exe");
    expect(failures, pref != config.audio_app_prefs.end() && pref->second.volume == 0.35 && pref->second.muted,
           "a route's gain and mute seed its automatic preference");
  }

  // A preference patch merges fields and keys: changing one fader never writes over another program, or
  // over the same program's mute.
  {
    bx::Config config;
    config.audio_app_prefs["discord.exe"].volume = 0.5;
    config.audio_app_prefs["discord.exe"].muted = true;
    config.audio_app_prefs["game.exe"].excluded = true;
    bx::Json patch = bx::Json::object();
    bx::Json prefs = bx::Json::object();
    bx::Json one = bx::Json::object();
    one.set("volume", bx::Json(0.25));
    one.set("pinned", bx::Json(true));
    prefs.set("discord.exe", one);
    patch.set("audio_app_prefs", prefs);
    bx::config_apply_audio(patch, config);
    expect(failures, config.audio_app_prefs["discord.exe"].volume == 0.25 &&
                         config.audio_app_prefs["discord.exe"].muted &&
                         config.audio_app_prefs["game.exe"].excluded,
           "a partial preference patch keeps every other field and key");

    expect(failures, !bx::auto_audio_prefs_json(config.audio_app_prefs)["discord.exe"].has("pinned"),
           "legacy pin is ignored while other preferences survive");

    // A gain change is live: it must not rebuild the encoders or clear the ring.
    bx::Config before = config;
    config.audio_app_prefs["discord.exe"].volume = 0.9;
    expect(failures, bx::config_change_bits(before, config) == 0,
           "a gain change is not a pipeline rebuild");
  }

  // Switching from together restores the encoders needed for separate audio.
  {
    bx::Config config;
    config.audio_capture_mode = "desktop";
    config.audio_tracks = 1;
    config.separate_tracks = false;
    config.mic_enabled = true;
    config.audio_app_prefs["saved.exe"].muted = true;
    expect(failures, bx::audio_track_count(config) == 1, "all sound together builds one track");
    config.audio_capture_mode = "apps";
    config.audio_tracks = 3;
    config.separate_tracks = true;
    expect(failures, bx::audio_track_count(config) == 3, "manual restores separate tracks after together");
    config.audio_capture_mode = "auto";
    expect(failures, bx::audio_track_count(config) == 6 && config.audio_app_prefs["saved.exe"].muted,
           "automatic restores six tracks without losing app preferences");
  }

  // The routing: six fixed tracks, stable bus ids, Other audio on track 6, and the mix decomposes exactly
  // into the lanes after it - so the editor sees a complete, editable file even when a lane carried A then B.
  {
    bx::Config config;
    config.audio_capture_mode = "auto";
    config.mic_enabled = true;
    bx::AutoAudioLane discord;
    discord.track = 3;
    discord.exe = "Discord.exe";
    discord.name = "Discord";
    discord.state = "connected";
    discord.pid = 10;
    bx::AutoAudioLane other;
    other.track = 6;
    other.name = "Other audio";
    other.other = true;
    other.state = "connected";
    other.apps.push_back(bx::AutoAudioApp{"game.exe", "Game", "connected", 20, 1.0, false, {}});
    const bx::AudioRoutingPlan routing = bx::plan_auto_audio_routing(config, {discord, other});
    expect(failures, routing.track_count == 6 && routing.tracks.size() == 6 && !routing.desktop_captured,
           "automatic mode builds six tracks with no desktop loopback");
    expect(failures, routing.tracks[0].sources.size() == 3 &&
                         routing.tracks[0].sources[0] == "microphone" &&
                         routing.tracks[0].sources[1] == "auto:bus:3" &&
                         routing.tracks[0].sources[2] == "auto:bus:6",
           "the mix names the microphone and both buses");
    expect(failures, routing.tracks[2].role == "application" && routing.tracks[2].sources[0] == "auto:bus:3",
           "an isolated lane carries its stable bus id");
    expect(failures, routing.tracks[5].role == "combined" && routing.tracks[5].title == "BuffrX Other audio",
           "Other audio is one combined lane with one title");

    // A final file with these labels reads back complete: the editor may level any lane and the export
    // rebuilds the mix from the same buses.
    const std::string root = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\buffrx-audio-auto";
    CreateDirectoryA(root.c_str(), nullptr);
    const std::string clip = root + "\\auto.mp4";
    bx::write_file_atomic(clip, "automatic six-track bytes");
    expect(failures, bx::write_audio_sidecar(clip, routing), "an automatic clip writes its sidecar");
    const bx::AudioLayout layout = bx::read_audio_layout(clip, 6);
    expect(failures, layout.buffrx && layout.aggregate && layout.complete,
           "the automatic layout is a complete aggregate the editor can recompute");
    const bx::AudioGains gains = bx::audio_gains({1.0, 0.5}, {false, false}, {}, 6);
    std::string error;
    expect(failures, bx::audio_mix_plan(layout, gains, error) == bx::AudioMixPlan::recompute,
           "a changed lane level is accepted, not locked out");
  }

  // The journal names a lane by everyone who occupied it, in the window a clip covers, and a manual
  // recording keeps its own summary after the rolling window has been pruned.
  {
    bx::AutoJournal journal;
    bx::AutoAudioPlan first;
    first.on = true;
    bx::AutoAudioLane a;
    a.track = 3;
    a.exe = "a.exe";
    a.name = "A";
    a.state = "connected";
    first.lanes.push_back(a);
    bx::AutoAudioPlan second;
    second.on = true;
    bx::AutoAudioLane b;
    b.track = 3;
    b.exe = "b.exe";
    b.name = "B";
    b.state = "connected";
    second.lanes.push_back(b);
    journal.start_output(1000);
    journal.observe(first, 1000);
    journal.observe(second, 2000);
    const std::vector<std::string> span = journal.names_in_span(3, 500, 2500);
    expect(failures, span.size() == 2 && span[0] == "A" && span[1] == "B",
           "a lane that carried A then B names both");
    journal.end_output(3000);
    journal.prune(60000, 100);
    const std::vector<std::string> kept = journal.output_names(3);
    expect(failures, kept.size() == 2, "a manual recording keeps both names after the rolling journal is pruned");
    expect(failures, journal.names_in_span(3, 500, 2500).empty(),
           "the rolling journal itself was pruned, so a ring save does not claim old footage");
  }

  expect(failures, bx::auto_error_is_unsupported("process audio activation failed (0x8000000E)"),
         "an unsupported activation is recognised");
  expect(failures, !bx::auto_error_is_unsupported("the process exited"),
         "a program that exited is not called unsupported");

  return failures;
}
