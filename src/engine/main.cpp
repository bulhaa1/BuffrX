// BuffrX engine entry point.
//
// Owns the process lifetime: data root, config, libobs, tray, hotkeys, the protocol server and the
// 250 ms tick. It deliberately knows nothing about windows except the hidden message window it needs
// for tray and hotkey messages â€” the product keeps running with the UI closed.
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "timeline_loudness.h"
#include "update_feed.h"
#include "update_download.h"
#include "update_apply.h"
#include "update_pending.h"
#include "update_math.h"
#include "update_policy.h"
#include "../shared/json.h"
#include "../shared/log.h"
#include "crash_log.h"
#include "../shared/pipe.h"
#include "benchmark.h"
#include "audio_meter.h"
#include "clip_check.h"
#include "elevation_policy.h"
#include "export_job_json.h"
#include "gpu.h"
#include "hotkey_enable.h"
#include "commands.h"
#include "library.h"
#include "lock_watch.h"
#include "log_policy.h"
#include "loop_wait.h"
#include "media.h"
#include "media_export.h"
#include "link.h"
#include "share.h"
#include "obs_host.h"
#include "platform.h"
#include "recovery_policy.h"
#include "screenshot.h"
#include "toast.h"
#include "ui/ui_format.h"
#include "rec_mark.h"
#include "audio_sessions.h"
#include "profile_probe.h"
#include "ui/ui_host.h"
#include "work_queue.h"

namespace bx {
// Reports which graphics resource libobs cannot create on this machine (see video_probe.cpp).
int run_video_probe(const std::string& runtime_root, const std::string& data_root);
}  // namespace bx

namespace {

const char* kPipeName = "\\\\.\\pipe\\buffrx";

bool has_argument(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) return true;
  }
  return false;
}

std::string environment(const char* name) {
  char buffer[1024] = {0};
  DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  return length > 0 && length < sizeof(buffer) ? std::string(buffer, length) : std::string();
}

// The pipe the engine listens on. A second engine can be given its own name, so that a test run
// never answers to - or writes into - the engine the person is actually using.
std::string pipe_name() {
  const std::string chosen = environment("BUFFRX_PIPE");
  if (chosen.empty()) return kPipeName;
  return chosen.rfind("\\\\.\\pipe\\", 0) == 0 ? chosen : "\\\\.\\pipe\\" + chosen;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace bx;
  // Per-monitor DPI awareness before anything touches D3D or a window. DXGI desktop duplication
  // (DuplicateOutput1) refuses a process that is not DPI aware with DXGI_ERROR_UNSUPPORTED, which left
  // display capture with no frames at all; OBS declares the same awareness in its manifest.
  // Resolved at run time: the SDK level this builds against does not declare it (Windows 10 1703+).
  using SetDpiContext = BOOL(WINAPI*)(HANDLE);
  if (auto set_dpi = reinterpret_cast<SetDpiContext>(
          reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext")))) {
    set_dpi(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  }

  // A click on a buffrx:// link starts the engine with exactly "--open-link <url>". The URL comes from a
  // web page, so nothing else on that command line is honoured: a link that smuggles in another flag
  // (a quote in the URL splitting it into two arguments, say) starts nothing at all. This runs before
  // every other flag, some of which write files.
  std::string open_link;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--open-link") != 0) continue;
    if (argc != 3 || i != 1 || std::strlen(argv[2]) > 1024) return 2;
    open_link = argv[2];
    // Windows starts a link handler in whatever folder the browser was in; the engine works from its own.
    SetCurrentDirectoryA(executable_directory().c_str());
  }
  const bool package_smoke = has_argument(argc, argv, "--package-smoke-test");
  const bool capture_smoke = has_argument(argc, argv, "--capture-smoke-test");
  const bool print_version = has_argument(argc, argv, "--version");
  const bool autostart = has_argument(argc, argv, "--autostart");
  // Windows relaunched us after a crash (RegisterApplicationRestart): show what survived and do not start capturing
  // on a machine nobody is sitting at.
  const bool restarted = has_argument(argc, argv, "--restarted");

  if (print_version) {
    std::printf("BuffrX engine 0.1.0-alpha.1\n");
    return 0;
  }

  std::string data_root;
  bool portable = false;
  if (!resolve_data_root(data_root, portable)) {
    // Before the log exists, and started by a launcher that opens no console: a message box is the only
    // channel there is, and a launch that shows nothing at all is the worst failure this app can have.
    std::fprintf(stderr, "BuffrX: no writable data directory\n");
    fatal_notice("BuffrX cannot save its settings here.\n\n" + data_root +
                 "\n\nMove BuffrX to a folder you can save in, such as your Documents folder, then start it again.");
    return 2;
  }
  // Tests and development runs keep their state out of the user's real profile.
  const std::string override_root = environment("BUFFRX_DATA_ROOT");
  if (!override_root.empty()) {
    data_root = override_root;
    CreateDirectoryA(data_root.c_str(), nullptr);
  }
  std::string runtime_root = executable_directory();
  // Packaged layout keeps the engine in bin/64bit next to the OBS DLLs, so the runtime root is two
  // levels up; a development build sits in the repository root instead.
  if (GetFileAttributesA((runtime_root + "\\..\\..\\data\\libobs").c_str()) != INVALID_FILE_ATTRIBUTES) {
    char resolved[MAX_PATH * 2] = {0};
    if (GetFullPathNameA((runtime_root + "\\..\\..").c_str(), sizeof(resolved), resolved, nullptr) > 0) {
      runtime_root = resolved;
    }
  }
  const std::string runtime_override = environment("BUFFRX_RUNTIME_ROOT");
  if (!runtime_override.empty()) runtime_root = runtime_override;

  CreateDirectoryA((data_root + "\\logs").c_str(), nullptr);
  Log::instance().open(data_root + "\\logs\\engine.log");
  install_crash_handler();
  BX_LOG_INFO("BuffrX engine starting (portable=%s, data=%s, runtime=%s)", portable ? "yes" : "no", data_root.c_str(),
              runtime_root.c_str());
  BX_LOG_INFO("the engine's main thread is %u", GetCurrentThreadId());

  if (has_argument(argc, argv, "--update-probe")) {
    // One fetch of the release feed, printed for a probe. Never part of the product's own path.
    const UpdateFeed feed = fetch_update_feed(update_feed_url(), 15000,
                                              std::string("BuffrX/") + kEngineVersion);
    std::printf("BUFFRX_UPDATE_PROBE ok=%d latest=%s error=%s url=%s digest=%s\n", feed.ok ? 1 : 0,
                feed.latest.c_str(), feed.error.c_str(), feed.url.c_str(), feed.digest.c_str());
    return feed.ok ? 0 : 4;
  }

  if (has_argument(argc, argv, "--update-download")) {
    // --update-download <url> <digest> <dest>: proves the verify path against a served file.
    std::string values[3];
    int found = 0;
    for (int i = 1; i + 1 < argc && found < 3; ++i) {
      if (std::string(argv[i]) == "--update-download") {
        for (int k = 1; k <= 3 && i + k < argc; ++k) values[found++] = argv[i + k];
        break;
      }
    }
    const DownloadOutcome out = download_verified(values[0], values[1], values[2], 15000);
    std::printf("BUFFRX_UPDATE_DOWNLOAD ok=%d path=%s error=%s\n", out.ok ? 1 : 0, out.path.c_str(),
                out.error.c_str());
    return out.ok ? 0 : 5;
  }

  if (has_argument(argc, argv, "--video-probe")) {
    return run_video_probe(runtime_root, data_root);
  }

  if (has_argument(argc, argv, "--audio-sessions")) {    // Who is on the sound card: the sessions EarTrumpet shows, and the process behind each.
    std::string session_error;
    const std::vector<std::string> sessions = list_audio_sessions(session_error);
    if (sessions.empty()) {
      std::printf("BUFFRX_AUDIO_SESSIONS_FAIL %s\n", session_error.c_str());
      return 3;
    }
    for (const std::string& line : sessions) std::printf("%s\n", line.c_str());
    std::printf("BUFFRX_AUDIO_SESSIONS_OK %zu sessions\n", sessions.size());
    return 0;
  }

  // The editor's media work (media_export.h). Each runs in its own short-lived process started by the
  // editor window and prints one machine-readable result; an export also prints "PROGRESS 0.42" lines.
  // Arguments are read from the wide command line so a clip named in any language opens.
  {
    std::vector<std::string> args;
    int wide_count = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &wide_count)) {
      for (int a = 0; a < wide_count; ++a) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[a], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<size_t>(std::max(0, bytes - 1)), '\0');
        if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, wide[a], -1, arg.data(), bytes, nullptr, nullptr);
        args.push_back(arg);
      }
      LocalFree(wide);
    }
    auto argument_after = [&](const char* flag, size_t offset) -> std::string {
      for (size_t a = 0; a + offset < args.size(); ++a) {
        if (args[a] == flag) return args[a + offset];
      }
      return std::string();
    };
    auto has_flag = [&](const char* flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };
    if (has_flag("--media-info")) {
      const MediaDetails details = media_details(argument_after("--media-info", 1));
      Json json = Json::object();
      json.set("ok", Json(details.ok));
      if (!details.ok) json.set("error", Json(details.reason));
      json.set("duration", Json(details.duration));
      json.set("width", Json(details.width));
      json.set("height", Json(details.height));
      json.set("sample_aspect_ratio", Json(details.sample_aspect_ratio));
      json.set("fps", Json(details.fps));
      json.set("video_codec", Json(details.video_codec));
      json.set("video_kbps", Json(details.video_kbps));
      json.set("bytes", Json(details.bytes));
      Json tracks = Json::array();
      for (const MediaTrack& track : details.audio) {
        Json entry = Json::object();
        entry.set("index", Json(track.index));
        entry.set("channels", Json(track.channels));
        entry.set("sample_rate", Json(track.sample_rate));
        entry.set("title", Json(track.title));
        tracks.push(entry);
      }
      json.set("audio", tracks);
      std::printf("%s\n", json.dump().c_str());
      return details.ok ? 0 : 3;
    }
    if (has_flag("--media-packets")) {
      // The Timeline dedupe spike's probe: every packet of a file as packet_hash.h hashes it, one line each, after one line of stream signature.
      const PacketReport report = media_packet_report(argument_after("--media-packets", 1));
      if (!report.ok) {
        std::printf("error %s\n", report.reason.c_str());
        return 3;
      }
      std::printf("sig %s\n", report.signature.c_str());
      for (const PacketLine& line : report.lines) {
        if (line.kind == 'v') std::printf("v %d %u %u %016llx\n", line.key ? 1 : 0, line.size, line.vcl_bytes, static_cast<unsigned long long>(line.hash));
        else std::printf("a %d %u %016llx\n", line.stream, line.size, static_cast<unsigned long long>(line.hash));
      }
      return 0;
    }
    if (has_flag("--media-concat")) {
      // The same probe: `--media-concat <out> <in> <in> ...` joins the files by stream copy exactly as a Timeline save joins its chunks.
      std::vector<std::string> inputs;
      std::string output;
      for (size_t a = 0; a < args.size(); ++a) {
        if (args[a] != "--media-concat") continue;
        if (a + 2 < args.size()) {
          output = args[a + 1];
          inputs.assign(args.begin() + static_cast<std::ptrdiff_t>(a) + 2, args.end());
        }
        break;
      }
      Json json = Json::object();
      std::string join_error;
      double first_keyframe = 0.0;
      bool joined = false;
      if (output.empty() || inputs.empty()) {
        join_error = "usage: --media-concat <out> <in> [<in> ...]";
      } else {
        const MediaDetails last = media_details(inputs.back());
        joined = last.ok && media_concat(inputs, output, 0.0, last.duration, join_error, &first_keyframe);
      }
      json.set("ok", Json(joined));
      if (!joined) json.set("error", Json(join_error));
      if (joined) json.set("duration", Json(media_details(output).duration));
      json.set("first_keyframe", Json(first_keyframe));
      std::printf("%s\n", json.dump().c_str());
      return joined ? 0 : 3;
    }
    if (has_flag("--media-join")) {
      // A Timeline save's join, run in a child of its own so the engine's media lock stays free: the spec
      // is a JSON file (MediaJoinRequest's fields), and run_media_join prints the result it writes beside
      // the clip. 0 when the join was written, 3 when it was not.
      const std::string join_spec = argument_after("--media-join", 1);
      if (join_spec.empty()) {
        std::printf("usage: --media-join <spec.json>\n");
        return 2;
      }
      return run_media_join(join_spec);
    }
    if (has_flag("--media-sequence")) {
      // Which frames of the probe window (present-loop --probe) a recording holds: exact skips and repeats.
      const std::string seconds_text = argument_after("--media-sequence", 2);
      const SequenceReport report = media_sequence_report(argument_after("--media-sequence", 1),
                                                          seconds_text.empty() ? 20.0 : std::atof(seconds_text.c_str()));
      Json json = Json::object();
      json.set("ok", Json(report.ok));
      if (!report.ok) json.set("error", Json(report.reason));
      json.set("decoded", Json(report.decoded));
      json.set("unreadable", Json(report.unreadable));
      json.set("recorded_fps", Json(std::round(report.recorded_fps * 10.0) / 10.0));
      json.set("source_per_second", Json(std::round(report.source_per_second * 10.0) / 10.0));
      json.set("repeats", Json(report.repeats));
      json.set("backwards", Json(report.backwards));
      json.set("worst_stall_frames", Json(report.worst_stall_frames));
      Json histogram = Json::array();
      for (int count : report.delta_histogram) histogram.push(Json(count));
      json.set("delta_histogram", histogram);
      std::printf("%s\n", json.dump().c_str());
      return report.ok ? 0 : 3;
    }
    if (has_flag("--media-frames")) {
      // The probe behind "the video is choppy": packet timing and repeated frames, measured from the file.
      const std::string seconds_text = argument_after("--media-frames", 2);
      const FrameReport report = media_frame_report(argument_after("--media-frames", 1),
                                                    seconds_text.empty() ? 8.0 : std::atof(seconds_text.c_str()));
      Json json = Json::object();
      json.set("ok", Json(report.ok));
      if (!report.ok) json.set("error", Json(report.reason));
      json.set("duration", Json(report.duration));
      json.set("nominal_fps", Json(std::round(report.nominal_fps * 10.0) / 10.0));
      json.set("average_fps", Json(std::round(report.average_fps * 10.0) / 10.0));
      json.set("packets", Json(report.packets));
      json.set("fps_min_second", Json(report.frames_per_second_min));
      json.set("gaps", Json(report.gaps));
      json.set("gap_seconds_total", Json(std::round(report.gap_seconds_total * 1000.0) / 1000.0));
      Json worst = Json::array();
      for (double gap : report.worst_gaps_ms) worst.push(Json(std::round(gap * 10.0) / 10.0));
      json.set("worst_gaps_ms", worst);
      json.set("decoded", Json(report.decoded));
      json.set("repeats", Json(report.repeats));
      json.set("repeat_percent", Json(report.decoded > 0 ? std::round(report.repeats * 1000.0 / report.decoded) / 10.0 : 0.0));
      Json runs = Json::array();
      for (int count : report.repeat_runs) runs.push(Json(count));
      json.set("repeat_runs", runs);
      std::printf("%s\n", json.dump().c_str());
      return report.ok ? 0 : 3;
    }
    if (has_flag("--media-peaks")) {
      std::vector<std::vector<float>> peaks;
      std::string peaks_error;
      const int buckets = std::atoi(argument_after("--media-peaks", 2).c_str());
      const bool ok = media_peaks(argument_after("--media-peaks", 1), buckets > 0 ? buckets : 600, peaks, peaks_error);
      Json json = Json::object();
      json.set("ok", Json(ok));
      if (!ok) json.set("error", Json(peaks_error));
      Json lanes = Json::array();
      for (const std::vector<float>& lane : peaks) {
        Json values = Json::array();
        // Two decimals are plenty for a waveform and keep the line short.
        for (float value : lane) values.push(Json(std::round(value * 100.0f) / 100.0));
        lanes.push(values);
      }
      json.set("tracks", lanes);
      std::printf("%s\n", json.dump().c_str());
      return ok ? 0 : 3;
    }
    if (has_flag("--media-keyframes")) {
      std::vector<double> keyframes;
      std::string keyframe_error;
      const bool ok = media_keyframes(argument_after("--media-keyframes", 1), keyframes, keyframe_error);
      Json json = Json::object();
      json.set("ok", Json(ok));
      if (!ok) json.set("error", Json(keyframe_error));
      Json times = Json::array();
      for (double time : keyframes) times.push(Json(time));
      json.set("keyframes", times);
      std::printf("%s\n", json.dump().c_str());
      return ok ? 0 : 3;
    }
    if (has_flag("--media-audio")) {
      std::string audio_error;
      const bool ok = media_audio_track(argument_after("--media-audio", 1), std::atoi(argument_after("--media-audio", 2).c_str()),
                                        argument_after("--media-audio", 3), audio_error);
      Json json = Json::object();
      json.set("ok", Json(ok));
      if (!ok) json.set("error", Json(audio_error));
      std::printf("%s\n", json.dump().c_str());
      return ok ? 0 : 3;
    }
    if (has_flag("--media-loudness")) {
      // The Timeline bar's levels for one file, as the engine would cache them: a byte per second (0..255).
      std::vector<float> rms;
      std::string loudness_error;
      const bool ok = media_loudness(argument_after("--media-loudness", 1), rms, loudness_error);
      Json json = Json::object();
      json.set("ok", Json(ok));
      if (!ok) json.set("error", Json(loudness_error));
      Json values = Json::array();
      for (uint8_t level : timeline_loudness_bytes(rms)) values.push(Json(static_cast<int>(level)));
      json.set("levels", values);
      std::printf("%s\n", json.dump().c_str());
      return ok ? 0 : 3;
    }
    if (has_flag("--media-export")) {
      // The job is a JSON file (ExportJob's fields), so no path or number is ever split by a shell.
      const std::string job_path = argument_after("--media-export", 1);
      std::string text;
      std::wstring wide_job(static_cast<size_t>(std::max(0, MultiByteToWideChar(CP_UTF8, 0, job_path.c_str(), -1, nullptr, 0))), L'\0');
      if (!wide_job.empty()) MultiByteToWideChar(CP_UTF8, 0, job_path.c_str(), -1, wide_job.data(), static_cast<int>(wide_job.size()));
      if (FILE* file = _wfopen(wide_job.c_str(), L"rb")) {
        char buffer[4096];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
        std::fclose(file);
      }
      if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // a UTF-8 BOM
      Json spec;
      std::string parse_error;
      if (text.empty() || !Json::parse(text, spec, parse_error)) {
        std::printf("EXPORT_FAIL the export job could not be read\n");
        return 2;
      }
      ExportJob job;
      if (!export_job_from_json(spec, job, parse_error)) {
        std::printf("EXPORT_FAIL %s\n", parse_error.c_str());
        return 2;
      }
      if (job.replace_original) {
        // The media process exits before the normal engine startup. Initialise the same library roots
        // here so the replacement can recycle the original while retaining the library's path checks.
        Config media_config;
        std::string config_error;
        if (load_config(data_root + "\\config.json", media_config, config_error)) {
          apply_path_defaults(media_config, data_root);
          library_init(library_paths_from(media_config, data_root));
          session_store_init(sessions_dir(media_config));
        } else {
          BX_LOG_WARN("replacement backup will stay beside the original: settings could not be read: %s", config_error.c_str());
        }
      }
      std::string used, export_error;
      const bool ok = media_export(job, [](double share) {
        std::printf("PROGRESS %.3f\n", share);
        std::fflush(stdout);
      }, used, export_error);
      if (!ok) {
        std::printf("EXPORT_FAIL %s\n", export_error.c_str());
        return 3;
      }
      std::printf("EXPORT_OK encoder=%s %s\n", used.c_str(), job.output.c_str());
      return 0;
    }
  }

  // Media probes: the same functions the library and the editor use, reachable without the interface.
  // They exist so clip handling can be verified on real files from a script.
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool is_probe = flag == "--media-probe";
    const bool is_thumb = flag == "--media-thumb";
    const bool is_trim = flag == "--media-trim";
    if (!is_probe && !is_thumb && !is_trim) continue;
    if (i + 1 >= argc) {
      std::fprintf(stderr, "usage: %s <file> [output] [number]\n", flag.c_str());
      return 2;
    }
    const std::string file = argv[i + 1];
    if (is_probe) {
      const MediaInfo info = media_probe(file);
      if (!info.ok) {
        std::printf("MEDIA_PROBE_FAIL %s\n", info.reason.c_str());
        return 3;
      }
      std::printf("MEDIA_PROBE_OK duration=%.3f width=%d height=%d video=%s audio=%s\n", info.duration_seconds,
                  info.width, info.height, info.video_codec.c_str(),
                  info.audio_codec.empty() ? "none" : info.audio_codec.c_str());
      return 0;
    }
    std::string error;
    if (is_thumb) {
      const std::string output = i + 2 < argc ? argv[i + 2] : file + ".thumb.jpg";
      const int width = i + 3 < argc ? std::atoi(argv[i + 3]) : 320;
      if (!media_thumbnail(file, output, width, 1.0, error)) {
        std::printf("MEDIA_THUMB_FAIL %s\n", error.c_str());
        return 3;
      }
      std::printf("MEDIA_THUMB_OK %s\n", output.c_str());
      return 0;
    }
    const double start = i + 2 < argc ? std::atof(argv[i + 2]) : 0.0;
    const double length = i + 3 < argc ? std::atof(argv[i + 3]) : 0.0;
    const std::string output = i + 4 < argc ? argv[i + 4] : file + ".trim.mkv";
    if (!media_trim(file, output, start, length, error)) {
      std::printf("MEDIA_TRIM_FAIL %s\n", error.c_str());
      return 3;
    }
    std::printf("MEDIA_TRIM_OK %s\n", output.c_str());
    return 0;
  }

  // An engine on its own data root (a test, a probe) is not the person's BuffrX: it must be able to run
  // beside the one they are using, as the acceptance harness and the benchmarks already assume.
  if (!package_smoke && !capture_smoke && environment("BUFFRX_DATA_ROOT").empty()) {
    if (!acquire_single_instance()) {
      BX_LOG_INFO("another engine instance is already running");
      // Started again from the launcher (BuffrX opened a second time): the running engine shows its
      // panel, the way any single-instance app comes to the front.
      if (has_argument(argc, argv, "--show")) {
        PipeClient client;
        std::string reply;
        if (client.connect(pipe_name(), 3000) &&
            client.send(R"({"id":1,"cmd":"ui.show","args":{"view":"compact"}})") && client.receive(reply)) {
          AllowSetForegroundWindow(ASFW_ANY);
        }
      }
      // A link clicked while BuffrX runs: the running engine fetches it and shows its panel.
      if (!open_link.empty()) {
        PipeClient client;
        std::string reply;
        Json args = Json::object();
        args.set("url", Json(open_link));
        Json request = Json::object();
        request.set("id", Json(1));
        request.set("cmd", Json("link.open"));
        request.set("args", args);
        if (client.connect(pipe_name(), 3000)) {
          AllowSetForegroundWindow(ASFW_ANY);
          client.send(R"({"id":2,"cmd":"ui.show","args":{"view":"main"}})");
          client.receive(reply);
          client.send(request.dump());
          client.receive(reply);
        }
      }
      return 0;
    }
  }

  // Apply only after single-instance arbitration: a second launch must show the running engine instead.
  const bool apply_probe = has_argument(argc, argv, "--apply-update");
  const bool normal_start = autostart || has_argument(argc, argv, "--show") || apply_probe;
  if (kAutoUpdate && normal_start && !has_argument(argc, argv, "--update-resume") &&
      update_marker_exists(data_root) && run_update_apply(data_root, runtime_root, kEngineVersion, autostart) == 0) {
    release_single_instance();
    return 0;
  }
  if (apply_probe) {
    release_single_instance();
    return 1;
  }

  ObsHost host;
  PlatformWindow window;
  Protocol protocol(host, window, data_root, portable, data_root + "\\config.json");
  protocol.set_runtime_root(runtime_root);

  std::string error;
  // Problems found before the notification card exists are held here and said once it does, so a settings file
  // that was set aside and a capture target that is no longer there are not log-only.
  std::vector<std::pair<std::string, std::string>> startup_notes;
  if (!protocol.load_settings(error)) {
    BX_LOG_WARN("settings could not be read (%s); using defaults", error.c_str());
    startup_notes.emplace_back("Your settings were reset",
                               "BuffrX could not read its settings, so it started with the defaults. The old file is kept as config.json.unreadable.");
  }
  Config& config = protocol.config();
  // Every entry point reaches this check, including Windows startup and buffrx:// links which start
  // the engine directly. A second launch was handed to the existing instance before this point.
  const bool utility = package_smoke || capture_smoke || has_argument(argc, argv, "--benchmark-seconds");
  if (elevation_needed(config.run_as_admin, process_is_elevated(), utility)) {
    // The elevated copy must be able to acquire the single-instance mutex as soon as UAC returns.
    release_single_instance();
    const DWORD result = relaunch_as_administrator();
    if (result != ERROR_SUCCESS && result != ERROR_CANCELLED) {
      BX_LOG_ERROR("administrator launch failed: Windows error %lu", static_cast<unsigned long>(result));
      fatal_notice("BuffrX could not start as administrator.\n\nWindows error " + std::to_string(result) +
                   ".\n\nTry launching BuffrX again.");
    }
    return result == ERROR_SUCCESS || result == ERROR_CANCELLED ? 0 : 2;
  }
  // Load-time reality check: a persisted window or game target that no longer exists â€” or that
  // resolves to this process â€” falls back to display for this session. The flip is deliberately NOT
  // falls back to display. The fallback is written to the config like the runtime one
  // (`check_capture_target`), so the user picks again rather than keeping a dead target that would fail
  // the same way on every launch.
  {
    std::string target_reason;
    if (!validate_capture_target(config, target_reason)) {
      BX_LOG_WARN("capture target falling back to display: %s", target_reason.c_str());
      // Recording the desktop when the user asked for a game is the wrong picture: say it, once, naming why.
      startup_notes.emplace_back("Recording the whole screen",
                                 "The window you picked is not available, so BuffrX records the whole screen. "
                                 "Pick a window or a game again on the Video page when it is running.");
    }
  }

  library_init(library_paths_from(config, data_root));
  // Sessions live under the recordings folder: one directory per recording, holding its chunks and the
  // index that recovery, retention and range export all read. The clips directory itself is created
  // here so the store can refuse a root whose parent does not exist instead of inventing one.
  CreateDirectoryA(config.clips_dir.c_str(), nullptr);
  session_store_init(sessions_dir(config));
  // Retention runs once at start, before the buffer exists: nothing is being recorded yet.
  if (config.recordings_budget_gb > 0) {
    std::vector<std::string> pruned;
    int64_t freed = 0, remaining = 0;
    std::string prune_error;
    session_prune(static_cast<int64_t>(config.recordings_budget_gb) * 1024 * 1024 * 1024, "", pruned, freed,
                  remaining, prune_error);
  }

  protocol.apply_hotkeys();

  if (!host.initialize(runtime_root, data_root, config, error)) {
    BX_LOG_ERROR("engine failed to start: %s", error.c_str());
    if (capture_smoke) {
      std::printf("BUFFRX_CAPTURE_SMOKE_FAIL %s\n", error.c_str());
      return 4;
    }
    // A full machine is not a broken package: the person is told the RAM looks full, in the words of memory_policy.h. The probe
    // variable that fakes the reading (BUFFRX_TEST_FREE_MB) skips the box so a test never waits on a click.
    if (host.memory_full()) {
      if (GetEnvironmentVariableA("BUFFRX_TEST_FREE_MB", nullptr, 0) == 0) MessageBoxA(nullptr, error.c_str(), "BuffrX", MB_ICONWARNING);
      return 4;
    }
    // A packaged build that cannot start libobs is a broken package; report it plainly.
    MessageBoxA(nullptr, ("BuffrX could not start its capture engine.\n\n" + error).c_str(), "BuffrX", MB_ICONERROR);
    return 4;
  }
  // FFmpeg's messages become the engine's, under one prefix, and its per-file chatter is said once.
  // Installed after libobs starts: the runtime sets its own callback while it loads its modules.
  media_install_log_callback();
  // A start that picked up the step this PC settled at says so in the settings too, or the page shows full speed while
  // the encoder runs lighter and the "Start from full speed again" button has nothing to clear.
  if (host.governor_step() > 0) protocol.note_governor_step(host.governor_step());

  // The first start picks the codec from the hardware (decided with the user, 2026-11-05): H.264 unless an AV1 encoder is
  // there, and the answer is remembered, so a codec the person chooses afterwards is never overridden again.
  if (!config.codec_auto_picked) {
    const std::string picked = host.preferred_codec();
    config.codec_auto_picked = true;
    if (picked != config.codec) {
      config.codec = picked;
      std::string pick_error;
      if (!host.apply(config, true, pick_error)) {
        BX_LOG_WARN("the codec chosen for this GPU did not start (%s); keeping it stored for the next attempt", pick_error.c_str());
      } else {
        BX_LOG_INFO("first start: %s is the codec this GPU encodes", picked.c_str());
      }
    }
    std::string pick_save_error;
    if (!save_config(data_root + "\\config.json", config, pick_save_error)) {
      BX_LOG_WARN("the codec choice was not written: %s", pick_save_error.c_str());
    }
  }

  PipeServer server;
  // The interface, drawn in this process (src/engine/ui): it sends the protocol's own commands through
  // Protocol::handle and receives every event the pipe clients do.
  ui::UiHost ui_host;
  auto emit = [&](const std::string& frame) {
    if (server.running()) server.broadcast(frame);
    ui_host.push_event(frame);
  };
  // The engine's own notification card (toast.h): created with the message window below, safe to call
  // from any thread. Its colours follow the stored theme (themes.h).
  Toast toast;
  // The mark that says a manual recording is running (rec_mark.h): a window of the engine's own, over
  // the game, never in the clip.
  RecMark rec_mark;
  // The settings the window, status and notification threads read. `apply_patch` rewrites protocol.config()
  // under the engine lock, so a reference to it is not safe off that lock: a std::string in it (clips_dir)
  // can be freed under a reader. This copy is refreshed by the tick under the engine lock and read under its
  // own mutex, so a reader always sees a value that stays put while it is used.
  struct UiConfig {
    std::mutex mutex;
    Config value;
    Config get() {
      std::lock_guard<std::mutex> lock(mutex);
      return value;
    }
    void set(const Config& next) {
      std::lock_guard<std::mutex> lock(mutex);
      value = next;
    }
  };
  auto ui_config = std::make_shared<UiConfig>();
  ui_config->value = config;
  auto ui_settings = [ui_config] { return ui_config->get(); };
  // A notice makes a sound only when it asks for one (`chime`): a background notice is a card and nothing else.
  // Whether the asked-for chime plays (the sound setting, or a card skipped over a game) is the toast's call.
  // `panel_in_front`: a BuffrX window is really in front of the user (ui_host.foreground_is_ours()) and draws its own
  // card for a saved clip or screenshot. A panel that is merely open does not count: it used to swallow the card.
  // `capture_active`: the buffer, a recording or Timeline is writing, so the chime is held out of the clip (the card
  // still draws).
  auto notify = [&, ui_config](ToastKind kind, const std::string& title, const std::string& detail,
                               ToastChime chime = ToastChime::none, bool panel_in_front = false) {
    const Config settings = ui_config->get();
    if (!settings.notifications) return;
    toast.show(kind, title, detail, chime, settings.notification_sound, toast_colors(settings.theme),
               settings.notify_over_fullscreen, panel_in_front, false,
               host.buffer_wanted() || host.recording_busy() || host.timeline_writer_active(),
               settings.notify_sound_while_recording);
  };
  auto folder_of = [](const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos || slash == 0) return std::string();
    const size_t parent = path.find_last_of("\\/", slash - 1);
    return path.substr(parent == std::string::npos ? 0 : parent + 1, slash - (parent == std::string::npos ? 0 : parent + 1));
  };
  host.on_clip_saved = [&, ui_config](const std::string& path) {
    const double seconds = probe_duration(path);
    Json data = Json::object();
    data.set("file", Json(path));
    data.set("name", Json(path.substr(path.find_last_of("\\/") + 1)));
    data.set("folder", Json(folder_of(path)));
    data.set("seconds", Json(seconds));
    emit(Protocol::make_event("clip.saved", data));
    char length[32];
    std::snprintf(length, sizeof(length), "%d:%02d", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
    // The panel draws its own card for the same save when it is the window in front; the desktop card is for every
    // other case, including a panel left open behind the game.
    const Config settings = ui_config->get();
    const bool quick = host.last_clip_seconds() == settings.quick_clip_seconds;
    notify(ToastKind::success, quick ? "Quick clip saved" : "Clip saved", ui::clip_saved_detail(folder_of(path), length),
           quick ? ToastChime::quick : ToastChime::saved,
           ui_host.foreground_is_ours());
    emit(Protocol::make_event("library.changed", Json::object()));
  };
  host.on_screenshot_saved = [&](const std::string& path) {
    Json data = Json::object();
    data.set("file", Json(path));
    emit(Protocol::make_event("screenshot.saved", data));
    notify(ToastKind::success, "Screenshot saved", folder_of(path), ToastChime::shot,
           ui_host.foreground_is_ours());
    emit(Protocol::make_event("library.changed", Json::object()));
  };
  host.on_error = [&](const std::string& component, const std::string& message) {
    Json data = Json::object();
    data.set("component", Json(component));
    data.set("message", Json(message));
    emit(Protocol::make_event("error", data));
    const char* title = component == "save" ? "Clip not saved"
                        : component == "recording" ? "Recording problem"
                        : component == "buffer" ? "Buffer problem"
                                                : "BuffrX";
    notify(ToastKind::warning, title, message, ToastChime::alert);
  };
  protocol.notifier = [&](const std::string& title, const std::string& detail) {
    // A Timeline clip saved or refused (the Record key, or a preview that went back to the game) asks for
    // its chime: over a fullscreen game the card is skipped and the sound is the only answer.
    if (title == "CLIP SAVED") notify(ToastKind::success, title, detail, ToastChime::saved);
    else if (title == "CLIP NOT SAVED") notify(ToastKind::warning, title, detail, ToastChime::alert);
    else if (title == "UPDATE READY") {
      const Config settings = ui_config->get();
      if (settings.notifications)
        toast.show(ToastKind::success, title, detail, ToastChime::none, false, toast_colors(settings.theme),
                   settings.notify_over_fullscreen, ui_host.foreground_is_ours(), false, false, false);
    }
    else notify(ToastKind::info, title, detail);
  };

  // Which connected pipe clients are an interface or an editor (named in their hello).
  std::map<int, std::string> client_roles;
  std::mutex roles_mutex;
  auto has_client = [&](const std::string& role) {
    std::lock_guard<std::mutex> lock(roles_mutex);
    for (const auto& entry : client_roles) {
      if (entry.second == role) return true;
    }
    return false;
  };
  // Brings a view forward: "main" (the panel), "tray" (the quick panel by the tray), "compact" (the quick
  // panel, opened without the tray). The windows are this process's own, so the tray click's right to take
  // the foreground is already ours.
  auto show_ui = [&](const std::string& view) { ui_host.request(view); };
  // An editor started a moment ago that has not said hello yet: requests wait for it rather than starting
  // another, whose second-instance hand-off would come straight back here and start yet another.
  struct PendingEditor {
    std::vector<std::string> files;
    bool preview = false;
  };
  std::mutex editor_mutex;
  uint32_t editor_launched_at = 0;
  uint32_t editor_hello_at = 0;
  std::vector<PendingEditor> editor_pending;
  protocol.editor_router = [&](const std::vector<std::string>& files, bool preview) {
    if (!server.running()) return false;
    if (!has_client("editor")) {
      std::lock_guard<std::mutex> lock(editor_mutex);
      if (editor_launched_at && GetTickCount() - editor_launched_at < 15000) {
        editor_pending.push_back(PendingEditor{files, preview});
        return true;
      }
      editor_pending.clear();               // an earlier start never said hello: its requests are void
      editor_launched_at = GetTickCount();  // the caller starts one now
      if (!preview) {
        ui_host.request_editor(files);
        return true;
      }
      return false;
    }
    AllowSetForegroundWindow(ASFW_ANY);
    Json list = Json::array();
    for (const std::string& file : files) list.push(Json(file));
    Json data = Json::object();
    data.set("files", list);
    data.set("preview", Json(preview));
    server.broadcast(Protocol::make_event("editor.files", data));
    return true;
  };

  // A new chunk or a closed session is new footage in the library (and retention may have removed some).
  host.on_session_changed = [&](const std::string&, bool) {
    emit(Protocol::make_event("library.changed", Json::object()));
  };
  host.on_marker_added =[&](const std::string& session_id, const SessionMarker& marker) {
    Json data = Json::object();
    data.set("id", Json(session_id));
    data.set("at_ms", Json(static_cast<int64_t>(marker.at_ms)));
    data.set("kind", Json(marker.kind));
    data.set("label", Json(marker.label));
    emit(Protocol::make_event("markers.changed", data));
  };

  protocol.broadcaster = [&](const std::string& frame) {
    emit(frame);
  };

  // Sharing (share.h): its jobs publish through the same emit, and it is stopped after the pipe and the
  // interface, which are the only callers. Held by a shared_ptr: a job thread takes one of its own, so a
  // fit or a post still winding down at quit outlives `main`'s owner instead of writing into freed memory.
  auto share = std::make_shared<ShareService>(data_root, executable_path());
  share->emit = [&](const std::string& name, const Json& data) { emit(Protocol::make_event(name, data)); };
  protocol.set_share(share.get());
  // BuffrX links (link.h): app-to-app sharing through buffrx-share.exe, started only while needed.
  LinkService links(data_root, executable_directory() + "\\buffrx-share.exe");
  // The poster of a clip still arriving is decoded here, not in link.cpp: that file is linked into the
  // fuzz target too, and pulling FFmpeg into it for one frame would be a poor trade.
  links.make_poster = [](const std::string& part, const std::string& out, std::string& error) {
    return media_thumbnail(part, out, 320, 0.0, error);
  };
  links.emit = [&](const std::string& name, const Json& data) {
    emit(Protocol::make_event(name, data));
    // A clip that arrived while the panel was hidden or another app was in front still says so.
    if (name == "link.received" && !data["file"].as_string().empty() && !data["id"].as_string().empty()) {
      notify(ToastKind::success, "Clip from a friend", "In your clips, under Shared with me", ToastChime::saved,
             ui_host.foreground_is_ours());
    }
    // A clip over 300 MB waits for a yes: if no window is on screen the question has nowhere to be asked.
    if (name == "link.changed" && !ui_host.any_visible()) {
      for (const Json& receive : data["receives"].items()) {
        if (receive["state"].as_string() == "asking") {
          ui_host.request("compact");
          break;
        }
      }
    }
  };
  protocol.set_links(&links);
  // buffrx:// opens this engine. Pointed at this exe on every start, so a moved install still works;
  // never from a test run, which must not take over the user's links.
  if (environment("BUFFRX_DATA_ROOT").empty() && !register_link_scheme(executable_path())) {
    BX_LOG_WARN("buffrx:// links could not be registered");
  }

  if (!server.start(pipe_name(),
                    [&](int client, const std::string& payload) {
                      // A client names its role in hello ("ui", "editor"), so the tray and editor.open
                      // can reach a window that is already open instead of starting a second one.
                      if (payload.find("\"hello\"") != std::string::npos) {
                        Json frame;
                        std::string parse_error;
                        if (Json::parse(payload, frame, parse_error) && frame["cmd"].as_string() == "hello") {
                          std::lock_guard<std::mutex> lock(roles_mutex);
                          client_roles[client] = frame["args"]["client"].as_string("");
                          if (client_roles[client] == "editor") {
                            std::lock_guard<std::mutex> editor_lock(editor_mutex);
                            editor_launched_at = 0;
                            editor_hello_at = GetTickCount();  // queued files follow once its page listens
                          }
                          ui_host.wake();  // the loop may be asleep to its next deadline: a new client gets its status now
                        }
                      }
                      std::string response = protocol.handle(payload);
                      if (response.size() > kMaxFrameBytes) {
                        // A reply the wire cannot carry would be refused by the writer and the client dropped
                        // with no answer at all: say so in a frame it can read, under its own id.
                        Json request;
                        std::string parse_error;
                        const int64_t id = Json::parse(payload, request, parse_error) ? request["id"].as_int(0) : 0;
                        Json refusal = Json::object();
                        refusal.set("id", Json(id));
                        refusal.set("ok", Json(false));
                        refusal.set("error", Json("the reply is too large; ask for less"));
                        response = refusal.dump();
                      }
                      server.send(client, response);
                    },
                    [&](int client) {
                      std::lock_guard<std::mutex> lock(roles_mutex);
                      client_roles.erase(client);
                    },
                    [&](int client, const std::string& payload) {
                      // The workers had no room for this command in time: answer it instead of leaving the
                      // client waiting on a slot it has taken.
                      Json request;
                      std::string parse_error;
                      const int64_t id = Json::parse(payload, request, parse_error) ? request["id"].as_int(0) : 0;
                      Json refusal = Json::object();
                      refusal.set("id", Json(id));
                      refusal.set("ok", Json(false));
                      refusal.set("error", Json("the engine is busy; try again"));
                      server.send(client, refusal.dump());
                    })) {
    BX_LOG_ERROR("protocol server could not start");
    fatal_notice("Another copy of BuffrX may already be running.\n\nLook for it in the tray, next to the clock. "
                 "To start this copy instead, right-click that icon, choose Quit, then open BuffrX again.\n\nDetails are in: " +
                 data_root + "\\logs\\engine.log");
    return 5;
  }
  BX_LOG_INFO("protocol listening on %s", pipe_name().c_str());

  // Tray and hotkey commands route through the same code paths the protocol uses.
  HotkeyEnable hotkey_enable;
  auto run_action = [&](const QueuedAction& job) {
    const std::string& action = job.name;
    // The same engine lock a protocol command takes: a hotkey and a pipe command never race on libobs.
    const uint32_t asked = GetTickCount();
    std::lock_guard<std::mutex> engine_lock(protocol.engine_mutex());
    if (GetTickCount() - asked > 750) {
      BX_LOG_WARN("the %s action waited %u ms for the engine", action.c_str(), GetTickCount() - asked);
    }
    const LockWatch::Scope held(engine_lock_watch(), "the " + action + " action");
    if (protocol.update_shutting_down()) return;
    std::string action_error;
    auto enable_off_feature = [&](bool timeline) {
      const bool start_buffer = !host.buffer_wanted();
      const char* feature = timeline ? "timeline" : "buffer";
      if (!hotkey_enable.press(action, feature, GetTickCount())) {
        const char* detail = timeline
            ? (start_buffer ? "Press the hotkey again to enable 30M Timeline and start Buffer."
                            : "Press the hotkey again to enable 30M Timeline.")
            : "Press the hotkey again to start the buffer.";
        notify(ToastKind::warning, timeline ? "Timeline is off" : "Buffer is off", detail, ToastChime::alert);
        return;
      }
      if (timeline) {
        Json patch = Json::object();
        patch.set("timeline_minutes", Json(30));
        protocol.apply_patch(patch, action_error);
      }
      if (action_error.empty() && start_buffer) protocol.toggle_buffer(action_error);
      if (!action_error.empty()) {
        notify(ToastKind::warning, "Could not start", action_error, ToastChime::alert);
      } else {
        notify(ToastKind::info, timeline ? "Timeline enabled" : "Buffer starting",
               timeline ? "30 minutes. Recording follows your Timeline settings." : "New footage will be buffered.");
      }
      action_error.clear();
    };
    // Each action leaves the same session marker its protocol command would.
    if (action == "save_replay" || action == "quick_clip") {
      const int seconds = action == "save_replay" ? config.buffer_seconds : config.quick_clip_seconds;
      if (host.save_clip(seconds, action_error) == 0) {
        protocol.mark("buffer.save", std::to_string(seconds) + " s");
      } else if (job.hotkey && action_error == "the buffer is not running") {
        action_error.clear();
        enable_off_feature(false);
      } else {
        // A hotkey press the engine could not honour was only logged: nothing on screen, nothing to hear.
        notify(ToastKind::warning, "Clip not saved", action_error, ToastChime::alert, ui_host.foreground_is_ours());
      }
    } else if (action == "screenshot") {
      std::string path;
      if (protocol.take_screenshot(config.screenshot_format, path, action_error)) {
        protocol.mark("screenshot.take", path.substr(path.find_last_of("\\/") + 1));
      } else {
        BX_LOG_WARN("screenshot failed: %s", action_error.c_str());
        notify(ToastKind::warning, "Screenshot not saved", action_error, ToastChime::alert, ui_host.foreground_is_ours());
      }
    } else if (action == "toggle_record") {
      // One press starts or stops a standalone recording, independent of Timeline.
      const bool was_pending = host.stats().recording_pending;
      const buffrx::RecordAction what = protocol.record_pressed(action_error);
      if (!action_error.empty()) {
        BX_LOG_WARN("the record key did nothing: %s", action_error.c_str());
        notify(ToastKind::warning, what == buffrx::RecordAction::ManualStart ? "Recording not started" : "Recording not stopped",
               action_error, ToastChime::alert);
        action_error.clear();  // already reported
      } else if (what == buffrx::RecordAction::ManualStop) {
        // The click had not produced a file yet: saying "saved" would promise a clip that never was.
        if (was_pending) notify(ToastKind::info, "Recording cancelled", "It had not started yet");
      } else if (host.stats().recording_pending) {
        notify(ToastKind::info, "Recording starting", "Waiting for the first frame");
      }
    } else if (action == "start_recording") {
      // The tick saw the first frame the click was waiting for. The encoder is created here, on the
      // action worker, never on the thread that draws the panel.
      if (!host.start_pending_recording(action_error)) {
        BX_LOG_WARN("recording did not start once the capture source was ready: %s", action_error.c_str());
        notify(ToastKind::warning, "Recording not started", action_error, ToastChime::alert);
        action_error.clear();
      } else if (host.stats().recording && !host.stats().recording_pending) {
        protocol.mark("recording.start", "start");
        emit(Protocol::make_event("recording.changed", Json(true)));
      }
    } else if (action == "governor_step") {
      // The encoder could not keep up: its speed settings go down a step (encoder_governor.h), and the person is
      // told, because their own settings are not what is running any more.
      if (host.apply_governor_step(action_error)) {
        protocol.note_governor_step(host.governor_step());
      } else if (!action_error.empty()) {
        notify(ToastKind::warning, "Encoder", action_error);
        action_error.clear();
      }
    } else if (action == "deep_idle") {
      host.enter_deep_idle();
    } else if (action == "resume") {
      // The machine slept and woke. A sleep takes the graphics device and the encoder sessions with it, and nothing
      // reports that as a fault: the ring would sit there looking alive with a frozen picture. Nothing capturing means
      // nothing to rebuild - the sources are released while idle and the next start reads them fresh.
      if (!host.buffer_wanted() && !host.recording_busy() && !host.timeline_writer_active()) {
        BX_LOG_INFO("woke from sleep with nothing capturing: no rebuild needed");
      } else if (!host.recover_after_sleep(action_error)) {
        BX_LOG_WARN("the capture did not come back after sleep: %s", action_error.c_str());
        notify(ToastKind::warning, "Capture did not come back", action_error, ToastChime::alert);
        action_error.clear();
      } else {
        BX_LOG_INFO("woke from sleep: the capture was rebuilt");
      }
    } else if (action == "pause_for_lock") {
      // The session locked. Stopping the buffer takes the engine lock and libobs, so the window thread that
      // heard the notification only queued this.
      if (config.pause_on_lock) host.stop_buffer();
    } else if (action == "resume_after_lock") {
      if (config.pause_on_lock) {
        std::string buffer_error;
        if (!host.start_buffer(buffer_error)) BX_LOG_WARN("buffer did not come back after the session unlocked: %s", buffer_error.c_str());
      }
    } else if (action == "quit_now") {
      // The person chose Quit, or Windows is ending the session: the outputs are stopped here, under the engine
      // lock. The main loop ends at once and its shutdown waits for this action before libobs is torn down.
      if (host.timeline_writer_active()) host.stop_timeline();
      if (host.recording_busy()) host.stop_recording();
    } else if (action == "install_update") {
      // The supervisor waits for this engine to exit before running the verified installer.
      if (!update_apply_allowed(host.recording_busy())) {
        protocol.report_update_error("Stop the recording first. The update will wait until it ends.");
        notify(ToastKind::warning, "Stop the recording first", "The update will wait until the recording ends.");
        emit(Protocol::make_event("status", protocol.status_json()));
        return;
      }
      std::string install_error;
      if (!protocol.spawn_pending_installer(install_error)) {
        protocol.report_update_error(install_error);
        BX_LOG_WARN("the update did not start: %s", install_error.c_str());
        notify(ToastKind::warning, "The update did not start", install_error, ToastChime::alert);
      } else {
        protocol.request_update_quit();
        if (host.timeline_writer_active()) host.stop_timeline();
        if (host.buffer_wanted()) host.stop_buffer();
        ui_host.wake();
      }
    } else if (action == "view_clip") {
      if (job.hotkey && config.timeline_minutes <= 0 && !protocol.timeline_preview_open()) enable_off_feature(true);
      else protocol.timeline_pressed();
    } else if (action == "toggle_buffer") {
      // The tray menu's Buffer switch.
      if (host.buffer_wanted() && config.timeline_minutes > 0) {
        ui_host.request("confirm_buffer_stop");
      } else if (!protocol.toggle_buffer(action_error)) {
        notify(ToastKind::warning, "Buffer not started", action_error, ToastChime::alert);
      }
      action_error.clear();
    } else if (action == "add_marker") {
      std::string session_id;
      SessionMarker marker;
      if (job.hotkey && config.timeline_minutes <= 0) {
        enable_off_feature(true);
      } else if (job.hotkey && !host.buffer_wanted()) {
        enable_off_feature(false);
      } else if (host.timeline_state() == TimelineState::Writing && host.add_marker("marker", "", session_id, marker, action_error)) {
        if (config.notifications) toast.show(ToastKind::info, "Marked", "", ToastChime::none, false,
                                              toast_colors(config.theme), config.notify_over_fullscreen, false, true);
      } else {
        if (action_error.empty()) action_error = "Timeline is not recording";
        notify(ToastKind::warning, "Marker not added", action_error);
      }
    } else if (action == "restart_engine") {
      // `engine.restart` handed this here so its reply is written before libobs is torn down: the same teardown
      // and start as before, on this worker instead of the command's pipe thread. The status broadcast at the end
      // of this action is the one the client waits for, and `job.was_buffering` is the buffer it had running.
      host.shutdown();
      if (!host.initialize(runtime_root, data_root, config, action_error)) {
        action_error = "the engine could not be restarted: " + action_error;
        notify(ToastKind::warning, "Restart failed", action_error, ToastChime::alert);
      } else if (job.was_buffering) {
        std::string buffer_error;
        if (!host.start_buffer(buffer_error)) BX_LOG_WARN("buffer did not restart: %s", buffer_error.c_str());
      }
    } else if (action == "stop_output_test") {
      // The probe's stop (BUFFRX_TEST_STOP_OUTPUT_MS): the same call the Stop key makes (Protocol::record_pressed),
      // on this worker and under the engine lock, so a stop libobs does not answer stalls exactly what it stalls
      // for someone pressing Stop.
      BX_LOG_INFO("BUFFRX_TEST_STOP_OUTPUT_MS: calling the stop path on the live output");
      host.stop_recording();
      BX_LOG_NOTE("BUFFRX_TEST_STOP_OUTPUT_MS: the stop path returned");
    } else if (action == "toggle_ui") {
      // The hotkey hides the panel when it is up and brings it back when it is not.
      ui_host.request("toggle");
    }
    if (!action_error.empty()) BX_LOG_WARN("action %s failed: %s", action.c_str(), action_error.c_str());
    emit(Protocol::make_event("status", protocol.status_json()));
  };

  // Hotkeys and tray actions run here, one at a time and in order, never on the window thread. They
  // take the engine lock, and a hotkey pressed while a settings change rebuilds the pipeline used to
  // hold the panel, the tray and every other hotkey for the whole rebuild. `engine.restart` is queued here
  // too, so its shutdown/initialize is not run inside the command that asked for it.
  WorkQueue<QueuedAction> actions(16);
  actions.start([&](const QueuedAction& action) { guarded("a hotkey action", [&] { run_action(action); }); });
  auto queue_action = [&](const std::string& action) {
    if (!actions.push(QueuedAction{action})) BX_LOG_WARN("the %s action was dropped: too many waiting", action.c_str());
  };
  auto queue_hotkey = [&](const std::string& action) {
    if (!actions.push(QueuedAction{action, false, true})) BX_LOG_WARN("the %s hotkey was dropped: too many waiting", action.c_str());
  };
  // The command side of the same queue (engine.restart): it answers the client as soon as the job is here.
  protocol.action_queue = [&](QueuedAction action) { return actions.push(std::move(action)); };
  // A controller press runs the same action the window's hotkey does, on the same queue. The recorder's
  // live chord rides the event stream the panel already listens to.
  protocol.set_pad_sinks(queue_hotkey, [&](const std::string& chord) {
    Json data = Json::object();
    data.set("chord", Json(chord));
    emit(Protocol::make_event("hotkeys.captured", data));
  });
  // The interface's own navigation: the panel turns a button edge into the key it stands for, so one
  // keyboard path moves both.
  protocol.set_nav_sink([&](uint32_t pressed, uint32_t released) { ui_host.pad_event(pressed, released); });

  auto run_command = [&](int command) {
    switch (command) {
      case PlatformWindow::kMenuOpenUi: show_ui("main"); break;
      case PlatformWindow::kMenuTrayPanel: show_ui("tray"); break;
      case PlatformWindow::kMenuOpenEditor: {
        DWORD editor_pid = 0;
        if (!protocol.editor_router || !protocol.editor_router({}, false)) launch_editor({}, data_root, editor_pid);
        break;
      }
      case PlatformWindow::kMenuSaveClip: queue_action("save_replay"); break;
      case PlatformWindow::kMenuQuickClip: queue_action("quick_clip"); break;
      case PlatformWindow::kMenuScreenshot: queue_action("screenshot"); break;
      case PlatformWindow::kMenuToggleRecording: queue_action("toggle_record"); break;
      case PlatformWindow::kMenuToggleBuffer: queue_action("toggle_buffer"); break;
      case PlatformWindow::kMenuTimeline: queue_action("view_clip"); break;
      case PlatformWindow::kMenuOpenClipsFolder: open_folder(ui_settings().clips_dir); break;
      case PlatformWindow::kMenuQuit:
        // A recording or a Timeline session is being written, and quitting ends it (recovery_policy.h). The default
        // button is Cancel and Esc cancels, so a stray Enter never cuts someone's recording. A Task Manager kill
        // cannot be intercepted: the fragmented containers and the recovery tail make that survivable, not prevented.
        // The box stays here on the window thread; the stops it agrees to take the engine lock and libobs, so they
        // are queued for the action worker, which the shutdown below waits for.
        if (buffrx::quit_needs_confirm(host.recording_busy(), host.timeline_writer_active())) {
          const int answer = MessageBoxA(nullptr, "BuffrX is still writing.\n\nStop it and quit?", "BuffrX",
                                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST);
          if (answer != IDYES) break;
        }
        queue_action("quit_now");
        // Quit means all of BuffrX: the interface and the editor are told first, so none of them
        // starts the engine again.
        emit(Protocol::make_event("app.quit", Json::object()));
        PostQuitMessage(0);
        break;
      case PlatformWindow::kMenuQuitNow:
        // Windows is ending the session (WM_ENDSESSION). No box and no wait: the stops are queued and the loop
        // ends at once, so a logoff is never held up by a recording.
        queue_action("quit_now");
        emit(Protocol::make_event("app.quit", Json::object()));
        PostQuitMessage(0);
        break;
      case 100:  // session locked
        // The controller poller has its own lock, so this is safe here; stopping the buffer is not, and is queued.
        protocol.pause_controller_hotkeys(true);
        queue_action("pause_for_lock");
        break;
      case 101:  // session unlocked
        protocol.pause_controller_hotkeys(false);
        queue_action("resume_after_lock");
        break;
      default: break;
    }
  };

  // "BuffrX Engine", not "BuffrX": the interface window carries that title, and a window search must
  // never find this hidden one.
  if (!window.create("BuffrX Engine", queue_hotkey, run_command)) {
    BX_LOG_ERROR("message window could not be created");
    // Without this window the tray icon and every hotkey are gone: say so instead of leaving a process
    // with no interface and no capture.
    fatal_notice("BuffrX could not finish starting.\n\nRestart BuffrX. If it happens again, restart your PC.\n\nDetails are in: " +
                 data_root + "\\logs\\engine.log");
    return 3;
  }
  window.set_menu_font_dir(runtime_root + "\\data\\buffrx\\fonts");
  // Sleep and wake reach the engine as a queued action, never on the window thread: the wake rebuild takes seconds
  // and the engine lock. A sleep itself needs nothing done - everything is thrown away by the time it wakes.
  window.set_power_handler([&](bool resumed) {
    if (resumed) queue_action("resume");
  });
  window.add_tray_icon("BuffrX - buffering");
  // A click on a notification brings BuffrX forward.
  if (!toast.create([&] { show_ui("main"); }, runtime_root + "\\data\\buffrx\\fonts")) {
    BX_LOG_WARN("notification window could not be created");
  }
  // What the start found wrong, now that a card can draw it.
  for (const auto& note : startup_notes) notify(ToastKind::warning, note.first, note.second);
  // The mark that says a manual recording is running. It is a window of the engine's own, so it is
  // never burned into the recording it marks.
  if (!rec_mark.create()) BX_LOG_WARN("the recording mark window could not be created");

  // Crash recovery (recovery_policy.h). Nothing is written to disk while the capture works, so an end nobody executed
  // - a crash, a Task Manager kill, a power cut - takes the ring with it. What it cannot take is the library, and the
  // first question a person has after a crash is whether their clips survived: one card answers it, and it is drawn by
  // the engine, so a start straight to the tray still says it.
  const std::string running_marker = data_root + "\\engine.running";
  if (GetFileAttributesA(running_marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
    buffrx::RecoveryFacts facts;
    // The container check is the engine's own (clip_check.h) and reads the file, so it is bounded to the newest
    // handful: the card is about what the person just lost, not an audit of the library.
    std::vector<MediaItem> clips = list_clips();
    std::sort(clips.begin(), clips.end(), [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
    for (size_t index = 0; index < clips.size() && index < 8; ++index) {
      if (check_clip_file(clips[index].file).ok) ++facts.clips_ok;
      else ++facts.damaged;
    }
    // A screenshot is a PNG, not a container: its check is that it is there and not empty.
    std::vector<MediaItem> shots = list_screenshots();
    std::sort(shots.begin(), shots.end(), [](const MediaItem& a, const MediaItem& b) { return a.modified > b.modified; });
    for (size_t index = 0; index < shots.size() && index < 8; ++index) {
      if (shots[index].size > 0) ++facts.screenshots_ok;
      else ++facts.damaged;
    }
    if (buffrx::recovery_notice_wanted(facts)) {
      notify(ToastKind::info, "Welcome back", buffrx::recovery_notice_text(facts), ToastChime::saved);
    }
    BX_LOG_WARN("the last run did not exit cleanly: %d clips and %d screenshots checked, %d damaged",
                facts.clips_ok + facts.damaged, facts.screenshots_ok + facts.damaged, facts.damaged);
  }
  // Windows brings BuffrX back after a crash (RegisterApplicationRestart). The relaunched copy shows the notice above
  // and does not start capturing by itself; Windows limits how often it tries, so a crash loop cannot be built here.
  RegisterApplicationRestart(L"--restarted", 0);

  if (package_smoke) {
    std::printf("BUFFRX_PACKAGE_SMOKE_OK version=%s obs=%s\n", kEngineVersion, host.obs_version().c_str());
    window.remove_tray_icon();
    server.stop();
    host.shutdown();
    return 0;
  }

  // The ring starts unless the user asked it not to. Starting it is not the same as wanting it: the
  // capture source may not have a frame yet, in which case the watchdog starts it when one arrives,
  // and that is what buffer_state "starting" means.
  if (config.start_buffering && !restarted) {
    std::string buffer_error;
    {
      // Already in a game when BuffrX starts (started at sign-in with the game running, or launched after it):
      // the buffer starts on the game, not on the display with a switch half a second later. The lock stays
      // open across the start: the pipe server is already answering clients, and one of them could change the
      // settings under the start.
      std::lock_guard<std::mutex> engine_lock(protocol.engine_mutex());
      protocol.start_on_running_game();
      if (!host.start_buffer(buffer_error)) {
        BX_LOG_WARN("buffer did not start yet: %s", buffer_error.c_str());
      }
    }
  } else {
    BX_LOG_INFO("started with the buffer off (start_buffering is false)");
  }

  if (has_argument(argc, argv, "--benchmark-seconds")) {
    // Measures what buffering costs on this machine: engine CPU, working set, capture and encode
    // rates, dropped frames. The "light on resources" claim has to answer to these numbers.
    //
    // The first seconds of a run contain libobs initialisation and the capture source warming up, so
    // they are discarded: the figure is only comparable to another tool's published cost when both
    // describe the settled pipeline. --benchmark-settle sets the discard, five seconds by default.
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) arguments.push_back(argv[i]);
    const BenchmarkOptions options = parse_benchmark_options(arguments);

    auto process_cpu_seconds = []() {
      FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
      GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time);
      auto filetime_seconds = [](const FILETIME& time) {
        ULARGE_INTEGER value;
        value.HighPart = time.dwHighDateTime;
        value.LowPart = time.dwLowDateTime;
        return static_cast<double>(value.QuadPart) / 10000000.0;
      };
      return filetime_seconds(kernel_time) + filetime_seconds(user_time);
    };
    auto sample = [&](BenchmarkSample& out) {
      out.cpu_seconds = process_cpu_seconds();
      out.wall_seconds = GetTickCount() / 1000.0;
      const EngineStats stats = host.stats();
      out.frames = stats.total_frames;
      out.dropped = stats.dropped_frames;
    };
    const int window = options.seconds - options.settle_seconds;
    BX_LOG_INFO("benchmark: discarding %d s, measuring %d s", options.settle_seconds, window);
    for (int i = 0; i < options.settle_seconds * 4; ++i) {
      Sleep(250);
      host.tick();
    }
    BenchmarkSample start;
    sample(start);
    // A long run is a soak: a line a minute is what shows a leak, a creeping lag or a restart that
    // happened while nobody was looking.
    for (int second = 0; second < window; ++second) {
      for (int slice = 0; slice < 4; ++slice) {
        Sleep(250);
        host.tick();
      }
      if ((second + 1) % 60 == 0) {
        const EngineStats progress = host.stats();
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
        BX_LOG_INFO("soak t=%ds working_set_mb=%.1f frames=%llu dropped=%llu video_lagged=%llu restarts=%llu",
                    second + 1, counters.WorkingSetSize / (1024.0 * 1024.0),
                    static_cast<unsigned long long>(progress.total_frames),
                    static_cast<unsigned long long>(progress.dropped_frames),
                    static_cast<unsigned long long>(progress.video_lagged),
                    static_cast<unsigned long long>(progress.restarts));
      }
    }
    BenchmarkSample end;
    sample(end);

    const BenchmarkResult result = summarize_benchmark(start, end);
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
    const EngineStats stats = host.stats();
    const int logical_cores = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    VideoStats video;
    video.video_frames = stats.video_frames;
    video.video_lagged = stats.video_lagged;
    video.encoder_frames = stats.encoder_frames;
    video.render_time_ms = stats.render_time_ms;
    std::printf("%s\n", format_benchmark_line(result, memory.WorkingSetSize / (1024.0 * 1024.0), stats.encoder_id,
                                              options.settle_seconds, logical_cores, video)
                            .c_str());
    BX_LOG_INFO("benchmark | %s", host.replay_diagnostics().c_str());
    server.stop();
    host.shutdown();
    return 0;
  }

  if (capture_smoke) {
    // Hardware acceptance: capture for six seconds, save one clip, confirm the file exists.
    // A copy: a pipe client may patch the settings while this runs.
    const std::string clips_dir = ui_settings().clips_dir;
    // Snapshot the library first: a clip left over from an earlier run must never satisfy the check,
    // or a broken save path passes forever.
    std::set<std::string> existing;
    for (const auto& clip : list_clips()) existing.insert(clip.file);
    for (int i = 0; i < 24; ++i) {
      Sleep(250);
      host.tick();
      if (i % 4 == 3) {
        const EngineStats stats = host.stats();
        BX_LOG_INFO("smoke t=%ds ready=%d %dx%d fps=%.1f frames=%llu dropped=%llu | %s", (i + 1) / 4,
                    stats.capture_ready ? 1 : 0, stats.width, stats.height, stats.capture_fps,
                    static_cast<unsigned long long>(stats.total_frames),
                    static_cast<unsigned long long>(stats.dropped_frames), host.replay_diagnostics().c_str());
      }
    }
    std::string save_error;
    host.save_clip(5, save_error);
    std::string saved_path;
    // Two conditions before the clip can be judged: the trim worker must have let it go, and the
    // muxer must have finished. libobs writes the file on its own thread and the "saved" signal
    // arrives as soon as the ring is written, so a file can still be growing when it first appears â€”
    // checking it then reports "no video track" for a clip that is fine. Waiting for the size to
    // hold steady across two polls is the honest signal that the writer is done.
    int64_t last_size = -1;
    int stable_polls = 0;
    for (int i = 0; i < 120 && saved_path.empty(); ++i) {
      Sleep(250);
      host.tick();
      for (const auto& clip : list_clips()) {
        if (clip.size <= 0 || existing.find(clip.file) != existing.end()) continue;
        std::string held = clip.file;
        std::replace(held.begin(), held.end(), '/', '\\');
        if (library_is_held(held)) continue;
        if (clip.size == last_size) {
          if (++stable_polls >= 2) {
            saved_path = clip.file;
            break;
          }
        } else {
          stable_polls = 0;
          last_size = clip.size;
        }
      }
      if (i % 8 == 7) BX_LOG_INFO("smoke waiting for the file | %s", host.replay_diagnostics().c_str());
    }
    std::string result = saved_path.empty() ? "BUFFRX_CAPTURE_SMOKE_FAIL no clip appeared" : "BUFFRX_CAPTURE_SMOKE_OK " + saved_path;
    if (!saved_path.empty()) {
      // The same check the save path runs before it announces a clip.
      const ClipCheck check = check_clip_file(saved_path);
      result += " verified=" + std::string(check.ok ? "yes" : "no") + " bytes=" + std::to_string(check.bytes);
      if (!check.ok) result += " reason=" + check.reason;
    }
    std::printf("%s\n", result.c_str());
    BX_LOG_INFO("final diagnostics | %s", host.replay_diagnostics().c_str());
    server.stop();
    host.shutdown();
    return saved_path.empty() ? 6 : 0;
  }

  BX_LOG_INFO("engine running");
  // The marker an unclean end leaves behind. It is written once the engine is really running (every probe and smoke
  // test has returned by now) and deleted by the clean exit at the end of this function, so its presence at the next
  // start means the last run was killed, crashed or lost power.
  {
    std::ofstream marker(running_marker, std::ios::binary | std::ios::trunc);
    marker << static_cast<unsigned long>(GetCurrentProcessId()) << "\n";
  }
  // Started by Windows at sign-in: the panel opens unless the user asked to start in the tray. The
  // Run entry is rewritten when it is on (a moved package fixes its own path); it is never removed
  // here, so a test engine with an isolated config cannot take away the real one.
  if (config.launch_on_startup && override_root.empty()) set_launch_on_startup(true, executable_path());
  // The interface: started now, drawn only when a window is asked for. The launcher passes --show; Windows
  // starting BuffrX at sign-in opens the panel unless the user chose to start in the tray.
  ui_host.start([&](const std::string& payload) { return protocol.handle(payload); }, data_root,
                runtime_root + "\\data\\buffrx\\fonts", config.ui_scale_percent);
  protocol.ui_capturer = [&](const std::string& view, const std::string& path, std::string& capture_error) {
    return ui_host.capture(view, path, capture_error);
  };
  protocol.ui_inputter = [&](const Json& args, std::string& input_error) { return ui_host.input(args, input_error); };
  protocol.ui_frame_stats = [&](bool reset) { return ui_host.frame_stats(reset); };
  // BuffrX starts as the compact mode; the expanded panel is one press away (decided with the user, 2026-10-31).
  if (has_argument(argc, argv, "--show") || (autostart && !config.start_minimized)) ui_host.request("compact");
  // Started by a click on a link: the panel shows the clip arriving. The fetch can wait for the
  // network for a while, so it runs off the window thread; it is joined at shutdown.
  std::thread open_link_thread;
  if (!open_link.empty()) {
    ui_host.request("main");
    open_link_thread = std::thread([&protocol, open_link] {
      Json args = Json::object();
      args.set("url", Json(open_link));
      Json request = Json::object();
      request.set("id", Json(0));
      request.set("cmd", Json("link.open"));
      request.set("args", args);
      protocol.handle(request.dump());
    });
  }
  host.on_notice = [&](const std::string& title, const std::string& detail) {
    // A buffer saved after a fault is a save, and over a fullscreen game the card is skipped: the chime is then
    // the only thing that says the footage is safe.
    notify(ToastKind::info, title, detail,
           title == "Buffer saved" ? ToastChime::saved : ToastChime::none);
  };
  bool paused_for_battery = false;
  bool low_disk_warned = false;
  // Low disk: one warning when the drive holding the clips drops below the minimum, again only after
  // it has recovered by a gigabyte. The check and the sleep deadline read the same interval.
  constexpr uint32_t kDiskCheckMs = 30000;
  uint32_t last_disk_check = 0;

  // The hang detector. The window thread is the engine's heartbeat: it draws the panel, pumps the tray
  // and the hotkeys, and ticks the ring. When it stops coming round, this thread says so while it is
  // still stuck, and names who holds the engine lock - a deadlock leaves no other trace, because
  // nothing that is stuck ever gets to write its own line.
  std::atomic<uint32_t> heartbeat{GetTickCount()};
  std::atomic<bool> detector_running{true};
  std::thread detector([&] {
    uint32_t reported_at = 0;
    while (detector_running) {
      Sleep(1000);
      const uint32_t beat = heartbeat.load();
      const uint32_t silent = GetTickCount() - beat;
      if (silent > 4000 && reported_at != beat) {
        reported_at = beat;
        BX_LOG_ERROR("the engine's window thread has not come round for %u ms; the engine lock is %s", silent,
                     engine_lock_watch().describe().c_str());
      } else if (silent <= 4000 && reported_at != 0 && reported_at != beat) {
        BX_LOG_WARN("the engine's window thread is back");
        reported_at = 0;
      }
    }
  });

  uint32_t last_tick = GetTickCount();
  // The heartbeat: the tick every 250 ms and the status every 500 ms. The main loop calls these, and so
  // does the interface while Windows runs its own loop for a dragged or resized panel - the engine's
  // loop does not come round then, and the watchdog, the levels and the status must not stop with it.
  auto beat_tick = [&](uint32_t now, uint32_t& lock_wait_ms, uint32_t& tick_ms) {
    // Any caller marks the window thread alive: the main loop, and the modal heartbeat a drag or the tray menu
    // runs while Windows holds the thread in its own loop. Without this a 4 s drag logged a false hang.
    heartbeat = now;
    if (now - last_tick >= 250) {
      last_tick = now;
      std::lock_guard<std::mutex> engine_lock(protocol.engine_mutex());
      const uint32_t tick_started = GetTickCount();
      lock_wait_ms = tick_started - now;
      const LockWatch::Scope held(engine_lock_watch(), "the tick");
      // The tick is the engine's heartbeat. If something in it throws, the engine says so and keeps
      // beating: a thrown exception here would otherwise end the process, and the capture with it.
      guarded("the engine tick", [&] {
        host.tick();
        // The settings the window and notification threads read, taken under the lock they are written under.
        ui_config->set(config);
        // The encoder cannot keep up and the next step down is due: it rebuilds the encoder, so it is not done here.
        if (host.take_governor_step_due()) queue_action("governor_step");
        // Idle long enough: the canvas drops to the idle rate (a video reset, so not on this thread either).
        if (host.take_deep_idle_due()) queue_action("deep_idle");
        // A probe, not a setting: a real sleep cannot be produced on demand, so BUFFRX_TEST_RESUME_MS queues the
        // action the power broadcast queues, once, that many milliseconds into the run.
        {
          static const uint32_t resume_after_ms = [] {
            char text[32];
            if (GetEnvironmentVariableA("BUFFRX_TEST_RESUME_MS", text, sizeof(text)) == 0) return 0u;
            return static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
          }();
          static const uint32_t run_started = GetTickCount();
          static bool resume_fired = false;
          if (resume_after_ms && !resume_fired && GetTickCount() - run_started > resume_after_ms) {
            resume_fired = true;
            BX_LOG_NOTE("BUFFRX_TEST_RESUME_MS: asking for the rebuild a wake runs");
            queue_action("resume");
          }
        }
        // A probe, not a setting: a stop libobs never answers cannot be produced on demand either, so
        // BUFFRX_TEST_STOP_OUTPUT_MS asks the real stop path to stop the live output that many milliseconds into
        // the run (tools/output-death-probe.ps1). It goes through the action worker, where the Stop key's own stop
        // runs, so what a client's stop would meet is met here.
        {
          static const uint32_t stop_after_ms = [] {
            char text[32];
            if (GetEnvironmentVariableA("BUFFRX_TEST_STOP_OUTPUT_MS", text, sizeof(text)) == 0) return 0u;
            return static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
          }();
          static const uint32_t run_started = GetTickCount();
          static bool stop_fired = false;
          if (stop_after_ms && !stop_fired && GetTickCount() - run_started > stop_after_ms) {
            stop_fired = true;
            BX_LOG_NOTE("BUFFRX_TEST_STOP_OUTPUT_MS: asking for the stop of the live output");
            queue_action("stop_output_test");
          }
        }
        // A record click that waited for its first frame starts here, on the action worker, so the
        // encoder creation never runs on the window thread.
        if (host.take_recording_start_due()) queue_action("start_recording");
        const uint32_t poll_started = GetTickCount();
        protocol.poll_game(now);
        // The Timeline preview is its own process: its id goes when its window does.
        protocol.poll_timeline_preview(now);
        // Timeline's own tick, beside the game's: it writes while the buffer runs, whatever is in front.
        protocol.poll_timeline(now);
        // A link being downloaded while a game has focus uploads slower, so the game keeps its connection.
        links.set_gaming(protocol.game_in_focus());
        const uint32_t poll_ms = GetTickCount() - poll_started;
        if (poll_ms > 100) {
          BX_LOG_WARN("the game detection took %u ms", poll_ms);
        }
        window.retry_pending();
        // The mark follows what was asked to be kept: a manual recording, or a Timeline clip that is still
        // open. Timeline writing by itself marks nothing. A request still waiting for its first frame is
        // not a recording yet: no dot until it runs.
        const EngineStats mark_stats = host.stats();
        rec_mark.set_recording(
            recording_dot_visible(mark_stats.recording && !mark_stats.recording_pending));
        // Anything the graphics backend raised while it worked is written down here, where the C
        // runtime is safe to use - the handler itself may not touch it.
        report_fault();
      });
      tick_ms = GetTickCount() - tick_started;
      // Pause on battery, and come back on mains power (it used to stay paused for good).
      const bool battery = config.pause_on_battery && on_battery_power();
      if (battery && !paused_for_battery && host.stats().buffer_active) {
        host.stop_buffer();
        paused_for_battery = true;
        notify(ToastKind::info, "Buffer paused", "Running on battery");
      } else if (!battery && paused_for_battery) {
        paused_for_battery = false;
        std::string resume_error;
        host.start_buffer(resume_error);
      }
    }
  };
  // The full status is for whoever listens: an interface window that is visible, or a pipe client (the editor, the Timeline
  // preview). With neither, nothing would read it, so it is not built, dumped or parsed; the tray needs only the engine's
  // numbers. The moment a listener appears the next pass sends a fresh status at once, so nothing opens on stale state.
  bool was_listening = false;
  bool last_recording = false;  // the record state last seen: the card and its sound follow the real thing, whoever asked
  // The status is built on a worker (phase 1.5): the build takes the engine lock, and a pipe command holding it
  // must not hold the thread that draws the panel, pumps the tray and runs the hotkeys. The window thread takes
  // the newest result and sends what changed; the worker wakes the window thread with the interface's wake().
  struct BeatShared {
    std::mutex mutex;
    std::condition_variable cv;
    bool wanted = false;   // a listener appeared: build now, not at the next beat
    bool stop = false;
    bool listening = false;
    uint64_t seq = 0;      // counts builds; the window thread takes each once
    bool emit = false;     // the status text changed since the window thread last sent one
    Json status;
    Json timeline;
    EngineStats stats;
  } beat;
  std::thread status_worker([&] {
    std::string last_text;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(beat.mutex);
        beat.cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return beat.stop || beat.wanted; });
        if (beat.stop) return;
        beat.wanted = false;
      }
      bool listening = false;
      {
        std::lock_guard<std::mutex> lock(beat.mutex);
        listening = beat.listening;
      }
      Json status;
      Json timeline;
      EngineStats stats;
      {
        std::lock_guard<std::mutex> engine_lock(protocol.engine_mutex());
        if (listening) status = protocol.status_json();
        timeline = protocol.timeline_status_json();
        stats = host.stats();
      }
      bool changed = false;
      if (listening) {
        const std::string text = status.dump();
        changed = text != last_text;
        last_text = text;
      } else {
        last_text.clear();
      }
      {
        std::lock_guard<std::mutex> lock(beat.mutex);
        beat.seq++;
        beat.emit = beat.emit || changed;  // kept until the window thread has sent it
        beat.status = std::move(status);
        beat.timeline = std::move(timeline);
        beat.stats = stats;
      }
      ui_host.wake();
    }
  });
  uint64_t beat_seen = 0;
  auto beat_status = [&](uint32_t) {
    const bool listening = ui_host.any_visible() || server.client_count() > 0;
    const bool newly_listening = listening && !was_listening;
    was_listening = listening;
    {
      std::lock_guard<std::mutex> lock(beat.mutex);
      beat.listening = listening;
      if (newly_listening) {
        beat.wanted = true;
        beat.cv.notify_one();
      }
    }
    // Everything below is on the window thread and uses only what the worker built.
    Json status;
    bool send = false;
    EngineStats stats;
    {
      std::lock_guard<std::mutex> lock(beat.mutex);
      if (beat.seq == beat_seen) return;
      beat_seen = beat.seq;
      stats = beat.stats;
      if (listening && beat.emit) {
        status = beat.status;
        send = true;
      }
      beat.emit = false;
    }
    ui_host.set_buffer_active(stats.buffer_active);
    if (send) emit(Protocol::make_event("status", status));
    std::string tooltip = stats.buffer_active ? "BuffrX - buffering" : "BuffrX - buffer stopped";
    if (stats.recording) tooltip += " + recording";
    tooltip += " | " + std::to_string(static_cast<int>(stats.capture_fps)) + " fps";
    if (stats.dropped_frames > 0) tooltip += " | " + std::to_string(stats.dropped_frames) + " dropped";
    window.update_tray_tooltip(tooltip);
    window.set_tray_activity(stats.recording ? 2 : stats.buffer_active ? 1 : 0);
    // What the tray menu shows next time it opens: it is drawn from this, never from the engine.
    const Config settings = ui_settings();
    TrayMenuState tray;
    tray.theme = settings.theme;
    tray.buffer_state = stats.buffer_state;
    tray.recording = stats.recording;
    tray.running_seconds = stats.recording_seconds;
    tray.quick_seconds = settings.quick_clip_seconds;
    window.set_tray_state(tray);
    // One place says it: the key, the panel button and a share receiver all end in the same state change.
    if (stats.recording && !last_recording) {
      notify(ToastKind::success, "Recording started", stats.capture_target, ToastChime::started);
    } else if (!stats.recording && last_recording) {
      notify(ToastKind::success, "Recording saved", stats.capture_target, ToastChime::stopped);
    }
    last_recording = stats.recording;
  };
  bool meter_requested = false;
  bool meters_were_active = false;
  uint32_t last_meter_sample = GetTickCount();
  uint32_t last_mic_meter_sample = last_meter_sample;
  auto audio_beat = [&](uint32_t now) {
    const bool wanted = ui_host.audio_meter_visible();
    if (wanted != meter_requested) {
      std::lock_guard<std::mutex> engine_lock(protocol.engine_mutex());
      host.set_audio_metering(wanted);
      meter_requested = wanted;
      meters_were_active = false;
      if (!wanted) ui_host.set_audio_levels(0.0f, 0.0f);
    }
    if (!wanted) return;
    if (!host.audio_meters_active()) {
      if (meters_were_active) ui_host.set_audio_levels(0.0f, 0.0f);
      meters_were_active = false;
      return;
    }
    if (!meters_were_active) {
      last_meter_sample = now - kAudioMeterSampleMs;
      last_mic_meter_sample = now - kMicrophoneMeterSampleMs;
      meters_were_active = true;
    }
    if (now - last_meter_sample < kAudioMeterSampleMs) return;
    last_meter_sample = now;
    const bool sample_mic = now - last_mic_meter_sample >= kMicrophoneMeterSampleMs;
    if (sample_mic) last_mic_meter_sample = now;
    host.sample_audio_levels(sample_mic);
    ui_host.set_audio_levels(host.level_desktop(), host.level_mic());
  };

  // The same beat while Windows holds the thread in its own loop: a panel drag or resize (the interface's own
  // timer) and the tray menu (the message window's). The tick and the status keep going, and beat_tick marks the
  // heartbeat, so a long drag or a menu left open is not reported as a hang.
  auto modal_beat = [&] {
    const uint32_t now = GetTickCount();
    uint32_t lock_wait_ms = 0, tick_ms = 0;
    beat_tick(now, lock_wait_ms, tick_ms);
    audio_beat(now);
    beat_status(now);
  };
  ui_host.set_modal_heartbeat(modal_beat);
  window.set_modal_heartbeat(modal_beat);
  bool quit = false;
  MSG message;
  // The profile probe: libobs's own profiler, so the idle cost can be named instead of guessed.
  const int profile_seconds = [&] {
    for (int a = 1; a + 1 < argc; ++a) {
      if (std::string(argv[a]) == "--profile") return std::atoi(argv[a + 1]);
    }
    return 0;
  }();
  const uint32_t profile_started = GetTickCount();
  if (profile_seconds > 0) profile_begin();
  while (!quit) {
    // An update was told to restart the app (`update.install`): the same clean stop and quit as the tray's
    // Quit, plus the installer behind it. Checked here, on the window thread, where PostQuitMessage belongs.
    if (protocol.take_update_quit()) {
      emit(Protocol::make_event("app.quit", Json::object()));
      PostQuitMessage(0);
    }

    // The whole loop is inside a guard. A C++ exception on the main thread ends the process, and
    // the main thread runs the interface, the tray, the tick and the message pump - so this is the
    // last place an exception can still be turned into a message instead of a dead engine.
    try {
    // Each part of the loop is timed: the window, the tray and every hotkey wait for this thread, so a
    // part that takes long is "the app stopped responding", and the log says which part it was.
    const uint32_t loop_started = GetTickCount();
    uint32_t window_ms = 0, lock_wait_ms = 0, tick_ms = 0, pump_ms = 0, frame_ms = 0;
    // Phase 0.3: how long this pass held the window thread, and the last message it dispatched (ui.frame_stats hitches).
    const auto pass_started = std::chrono::steady_clock::now();
    uint32_t last_message = 0;
    uint32_t messages = 0;  // how many this pass dispatched: a stall line says whether the pump or the frame held it
    // Wide messages: the interface's text fields take any language as typed.
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        quit = true;
        break;
      }
      ++messages;
      TranslateMessage(&message);
      const uint32_t dispatched = GetTickCount();
      DispatchMessageW(&message);
      last_message = static_cast<uint32_t>(message.message);
      // One message that holds the thread names itself: a drag or resize loop, the tray menu, a hotkey
      // action. The window part of a stall line is the sum of these and the frame.
      const uint32_t took = GetTickCount() - dispatched;
      if (took > 100) {
        // A caption drag or a size loop is Windows running its own loop for the person's hand; a stuck message is a fault.
        if (is_modal_loop_message(static_cast<uint32_t>(message.message))) {
          BX_LOG_INFO("the window thread spent %u ms in message 0x%04x", took, static_cast<unsigned>(message.message));
        } else {
          BX_LOG_WARN("the window thread spent %u ms in message 0x%04x", took, static_cast<unsigned>(message.message));
        }
      }
    }
    const uint32_t frame_started = GetTickCount();
    ui_host.frame();
    if (GetTickCount() - frame_started > 100) {
      BX_LOG_WARN("the interface took %u ms to draw: %s", GetTickCount() - frame_started, ui_host.last_frame_report().c_str());
    }
    pump_ms = frame_started - loop_started;
    frame_ms = GetTickCount() - frame_started;
    window_ms = GetTickCount() - loop_started;
    ui_host.note_pass(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pass_started).count(), last_message);
    // Files asked for while the editor was starting reach it a moment after its hello, when its page is
    // listening (a preview plays the last one asked for; files for editing are all added).
    {
      std::vector<PendingEditor> deliver;
      {
        std::lock_guard<std::mutex> lock(editor_mutex);
        if (editor_hello_at && GetTickCount() - editor_hello_at > 1500) {
          deliver.swap(editor_pending);
          editor_hello_at = 0;
        }
      }
      for (const PendingEditor& request : deliver) protocol.editor_router(request.files, request.preview);
    }
    const uint32_t now = GetTickCount();
    beat_tick(now, lock_wait_ms, tick_ms);
    audio_beat(now);
    // Low disk: one warning when the drive holding the clips drops below the minimum, again only after
    // it has recovered by a gigabyte.
    if (now - last_disk_check >= kDiskCheckMs) {
      last_disk_check = now;
      // Pipe threads change the config under the engine lock; copy what the check needs.
      std::string clips_dir;
      int min_free_gb = 1;
      {
        std::lock_guard<std::mutex> engine_lock(protocol.engine_mutex());
        clips_dir = config.clips_dir;
        min_free_gb = config.min_free_gb;
      }
      const int64_t free_bytes = volume_free_bytes(clips_dir);
      const int64_t floor = static_cast<int64_t>(std::max(1, min_free_gb)) * 1024 * 1024 * 1024;
      if (free_bytes > 0 && free_bytes < floor && !low_disk_warned) {
        low_disk_warned = true;
        char detail[96];
        std::snprintf(detail, sizeof(detail), "%.1f GB left on the drive with your clips", free_bytes / 1073741824.0);
        notify(ToastKind::warning, "Low disk space", detail, ToastChime::alert);
      } else if (free_bytes > floor + 1073741824LL) {
        low_disk_warned = false;
      }
    }
    beat_status(now);
    if (profile_seconds > 0 && now - profile_started >= static_cast<uint32_t>(profile_seconds) * 1000) {
      const std::string csv = data_root + "\\logs\\profile.csv";
      std::string profile_error;
      if (profile_end(csv, profile_error)) {
        std::printf("BUFFRX_PROFILE_OK %s\n", csv.c_str());
      } else {
        std::printf("BUFFRX_PROFILE_FAIL %s\n", profile_error.c_str());
      }
      quit = true;
    }
    // A drag or resize inside this pass ran its own loop with the heartbeat going: its time is not a stall.
    const uint32_t loop_ms = stall_without_modal(GetTickCount() - loop_started, ui_host.take_modal_ms());
    if (loop_ms > 750) {
      BX_LOG_WARN("the engine's window thread stalled for %u ms (window %u ms, waiting for the engine lock %u ms, "
                  "tick %u ms; %u messages in %u ms, the frame %u ms)",
                  loop_ms, window_ms, lock_wait_ms, tick_ms, messages, pump_ms, frame_ms);
    }
    // Not a fixed sleep: a window with frames to draw waits for the display's next frame, an idle one for the next
    // message or request (at most 20 ms), so a click does not wait for the end of a sleep.
    // Sleeps to the earliest deadline the loop owns; a message, a request, a reply or an event wakes it sooner.
    {
      const uint32_t after = GetTickCount();
      // The status beat wakes the loop itself (the worker calls wake()), so it needs no deadline here.
      uint32_t next_ms = ms_until(after, last_tick, 250);
      if (meter_requested && host.audio_meters_active())
        next_ms = std::min(next_ms, ms_until(after, last_meter_sample, kAudioMeterSampleMs));
      next_ms = std::min(next_ms, ms_until(after, last_disk_check, kDiskCheckMs));
      if (profile_seconds > 0) next_ms = std::min(next_ms, 250u);
      {
        std::lock_guard<std::mutex> lock(editor_mutex);
        if (editor_hello_at) next_ms = std::min(next_ms, ms_until(after, editor_hello_at, 1500));
      }
      ui_host.wait(loop_sleep_ms(1000, next_ms));
    }
    } catch (const std::exception& failure) {
      BX_LOG_ERROR("the engine's main loop failed: %s", failure.what());
    } catch (...) {
      BX_LOG_ERROR("the engine's main loop failed with an unknown error");
    }
  }

  BX_LOG_INFO("engine shutting down");
  protocol.stop_update_download();  // The worker's notifier refers to UI objects that are about to be stopped.
  // A clean exit says so: the next start has nothing to warn about and shows no card.
  DeleteFileA(running_marker.c_str());
  actions.stop();  // what was pressed is finished first: a save the user asked for is still filed
  {
    std::lock_guard<std::mutex> lock(beat.mutex);
    beat.stop = true;
  }
  beat.cv.notify_all();
  status_worker.join();
  detector_running = false;
  detector.join();
  ui_host.stop();
  server.stop();
  protocol.set_share(nullptr);
  share->shutdown();
  if (open_link_thread.joinable()) open_link_thread.join();
  protocol.set_links(nullptr);
  links.shutdown();
  window.remove_tray_icon();
  host.shutdown();
  release_single_instance();
  return 0;
}

