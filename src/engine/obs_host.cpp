#include "obs_host.h"
#include "sleep_patch.h"
#include "ring_output.h"

#include "buffer_watch.h"
#include "capture_follow.h"
#include "recording_watch.h"
#include "capacity_memory.h"
#include "audio_meter.h"

#include <windows.h>
#include <tlhelp32.h>

#include <psapi.h>

#include <obs.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "../shared/files.h"
#include "../shared/log.h"
#include "process_tuning.h"
#include "clip_check.h"
#include "encoder_choice.h"
#include "save_filing.h"
#include "save_check.h"
#include "capture_follow.h"
#include "command_policy.h"
#include "encoder_settings.h"
#include "file_final.h"
#include "game_watch.h"
#include "gpu.h"
#include "work_queue.h"
#include "library.h"
#include "media.h"
#include "media_math.h"
#include "memory_policy.h"
#include "platform.h"
#include "recovery_policy.h"
#include "recording_space.h"
#include "ring_disk.h"
#include "ring_spill.h"
#include "saved_replay.h"
#include "screenshot.h"
#include "timeline_pins.h"

#ifndef _WIN32
#include "../linux/display_linux.h"
#endif

// Live level meters: volmeters report peak and RMS dB from the audio thread.
#include <obs-audio-controls.h>

#if LIBOBS_API_MAJOR_VER >= 33
#define BX_ENCODER_FRAMES(encoder) obs_encoder_get_encoded_frames(encoder)
#else
// libobs 30's public API does not expose the encoder's running frame count. The status keeps the
// output's own count instead; the picture and the buffer are unaffected.
#define BX_ENCODER_FRAMES(encoder) static_cast<uint64_t>(obs_get_total_frames())
#endif

namespace bx {
namespace {

// What changes how hard the encoder works, as one key for capacity.json: codec, requested encoder, picture, rate and the
// quality preset. A different key is a different question, and the ladder is asked again.
std::string capacity_key_for(const Config& config) {
  return capacity_key(config.codec, config.encoder, config.output_width, config.output_height, config.capture_fps,
                      config.video_preset);
}

// The canvas channel each audio source sits on. libobs mixes the audio output from the canvas channels
// (obs-audio.c sums `root_nodes`, filled only from a canvas's channels), not from the list of audio
// sources: a source on neither is captured and metered but never reaches the encoder, which is how every
// clip came out with a silent AAC track. Channel 0 is the capture scene.
constexpr uint32_t kDesktopViewChannel = 1;
constexpr uint32_t kMicViewChannel = 2;

// The capture plugin and its two source ids differ by platform; everything above this line is the
// same graph either way.
#ifdef _WIN32
constexpr const char* kDesktopCaptureSource = "wasapi_output_capture";
constexpr const char* kMicCaptureSource = "wasapi_input_capture";
#else
constexpr const char* kDesktopCaptureSource = "pulse_output_capture";
constexpr const char* kMicCaptureSource = "pulse_input_capture";
#endif

// Defined further down; a recording that wrote no byte at all leaves nothing to keep.
int64_t file_size_now(const std::string& file);

// Retain the loudest peak until the window thread consumes it, including peaks between two frames.
void on_volume_sample(void* into, const float magnitude[], const float peak[], const float input_peak[]) {
  (void)magnitude;
  (void)input_peak;
  audio_meter_accumulate(*static_cast<std::atomic<float>*>(into), peak, MAX_AUDIO_CHANNELS);
}

// RMS reduces the microphone display's response to brief clicks without filtering the recorded sound.
void on_microphone_sample(void* into, const float magnitude[], const float[], const float[]) {
  audio_meter_accumulate(*static_cast<std::atomic<float>*>(into), magnitude, MAX_AUDIO_CHANNELS);
}

// Sets a value on an encoder's settings only when that encoder actually exposes the key. Vendors
// disagree about their parameter names, and writing an unsupported key is how a working encoder
// silently starts failing.
void set_if_supported(obs_data_t* settings, obs_properties_t* properties, const char* key, obs_data_type type,
                      int int_value, const char* string_value) {
  if (!obs_properties_get(properties, key)) return;
  switch (type) {
    case OBS_DATA_NUMBER:
      obs_data_set_int(settings, key, int_value);
      break;
    case OBS_DATA_BOOLEAN:
      obs_data_set_bool(settings, key, int_value != 0);
      break;
    case OBS_DATA_STRING:
      obs_data_set_string(settings, key, string_value ? string_value : "");
      break;
    default:
      break;
  }
}

// Writes one resolved setting onto the encoder, but only when the encoder's own property list accepts
// the value. A wrong value must not be written: NVENC's "multipass" and x264's "preset" do not share a
// vocabulary, and an unlisted value makes the encoder log and silently keep its default.
bool apply_encoder_setting(obs_data_t* settings, obs_properties_t* properties, const EncoderSetting& setting) {
  obs_property_t* property = obs_properties_get(properties, setting.name.c_str());
  if (!property) return false;
  switch (obs_property_get_type(property)) {
    case OBS_PROPERTY_LIST: {
      Json allowed = Json::array();
      const size_t count = obs_property_list_item_count(property);
      for (size_t index = 0; index < count; ++index) {
        const char* value = obs_property_list_item_string(property, index);
        if (value) allowed.push(Json(value));
      }
      if (!encoder_option_valid(setting.value, allowed, false, 0, 0)) return false;
      obs_data_set_string(settings, setting.name.c_str(), setting.value.as_string().c_str());
      return true;
    }
    case OBS_PROPERTY_INT:
      if (!encoder_option_valid(setting.value, Json(), true, obs_property_int_min(property),
                                obs_property_int_max(property))) {
        return false;
      }
      obs_data_set_int(settings, setting.name.c_str(), setting.value.as_int());
      return true;
    case OBS_PROPERTY_FLOAT:
      if (!encoder_option_valid(setting.value, Json(), true, obs_property_float_min(property),
                                obs_property_float_max(property))) {
        return false;
      }
      obs_data_set_double(settings, setting.name.c_str(), setting.value.as_double());
      return true;
    case OBS_PROPERTY_BOOL:
      obs_data_set_bool(settings, setting.name.c_str(), setting.value.as_bool());
      return true;
    case OBS_PROPERTY_TEXT:
      obs_data_set_string(settings, setting.name.c_str(), setting.value.as_string().c_str());
      return true;
    default:
      return false;
  }
}

void apply_encoder_settings(obs_data_t* settings, obs_properties_t* properties,
                            const std::vector<EncoderSetting>& list) {
  for (const EncoderSetting& setting : list) apply_encoder_setting(settings, properties, setting);
}

// libobs resolves an effect file's "#include" by looking for the last forward slash in the including
// file's path. Windows-style backslashes make that lookup fail, so every path this engine hands to
// libobs is converted first — the same convention OBS itself uses on Windows.
std::string slashify(const std::string& path) {
  std::string out = path;
  std::replace(out.begin(), out.end(), '\\', '/');
  return out;
}



bool primary_monitor_size(int& width, int& height) {
#ifdef _WIN32
  struct Lookup {
    int width = 0;
    int height = 0;
    bool found = false;
  } lookup;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto* lookup = reinterpret_cast<Lookup*>(parameter);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoA(monitor, &info)) return TRUE;
        if ((info.dwFlags & MONITORINFOF_PRIMARY) == 0) return TRUE;
        lookup->width = info.rcMonitor.right - info.rcMonitor.left;
        lookup->height = info.rcMonitor.bottom - info.rcMonitor.top;
        lookup->found = true;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&lookup));
  width = lookup.width;
  height = lookup.height;
  return lookup.found;
#else
  return linux_primary_size(width, height);
#endif
}

// Every monitor the desktop has right now, in EnumDisplayMonitors order (the order libobs's monitor
// list uses).
std::vector<DisplayInfo> enumerate_displays() {
#ifdef _WIN32
  std::vector<DisplayInfo> list;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto* output = reinterpret_cast<std::vector<DisplayInfo>*>(parameter);
        MONITORINFOEXA info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoA(monitor, &info)) {
          DisplayInfo entry;
          entry.id = static_cast<int>(output->size());
          entry.device = info.szDevice;
          entry.width = info.rcMonitor.right - info.rcMonitor.left;
          entry.height = info.rcMonitor.bottom - info.rcMonitor.top;
          entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
          entry.name = entry.primary ? "Primary display" : ("Display " + std::to_string(entry.id + 1));
          entry.name += " (" + std::to_string(entry.width) + "x" + std::to_string(entry.height) + ")";
          output->push_back(entry);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&list));
  return list;
#else
  return linux_displays();
#endif
}

// The GDI device name (\\.\DISPLAY1) of the configured display: by name when the config carries one,
// else by its position in EnumDisplayMonitors order (the index libobs's monitor list uses). Empty
// when no such monitor exists. `warned` remembers which missing display was already reported; it is the
// host's, written under the engine lock, because a function-local static was written from several threads.
std::string monitor_device_name(const Config& config, std::string* warned = nullptr) {
  const DisplayPick pick = pick_display(enumerate_displays(), config.display_id, config.display_name);
  if (!pick.found) return {};
  // The display that was chosen is gone (unplugged, a docking station left, a remote session): the capture
  // goes to the primary display instead of to nothing, and the buffer keeps working.
  if (pick.primary_fallback && !config.display_name.empty() && warned) {
    if (*warned != config.display_name) {
      *warned = config.display_name;
      BX_LOG_WARN("the display \"%s\" is not there; capturing the primary display (%s)", config.display_name.c_str(), pick.device.c_str());
    }
  }
  return pick.device;
}

// The refresh rate the capture's display is running at right now, 0 when it cannot be read.
int display_refresh_hz(const Config& config) {
#ifdef _WIN32
  const std::string device = monitor_device_name(config);
  DEVMODEA mode{};
  mode.dmSize = sizeof(mode);
  if (device.empty() || !EnumDisplaySettingsA(device.c_str(), ENUM_CURRENT_SETTINGS, &mode)) return 0;
  return mode.dmDisplayFrequency > 1 ? static_cast<int>(mode.dmDisplayFrequency) : 0;
#else
  return linux_display_refresh_hz(config.display_id, monitor_device_name(config));
#endif
}

// The window_capture and game_capture sources identify a window as "title:class:exe" (see
// capture_enum.h for the escaping). Learning the target's size from Win32 before the capture source
// exists is what keeps the first (and only) obs_reset_video honest.
bool win32_target_size(const Config& config, int& width, int& height) {
  width = 0;
  height = 0;
  if (config.capture_mode == "game" && config.game_title.empty()) {
    // "any fullscreen game": the primary monitor is where a fullscreen game lives.
    return primary_monitor_size(width, height);
  }
  const std::string wanted = capture_window_target(config);
  if (!wanted.empty() && find_window_size(wanted, width, height)) return true;

  if (config.capture_mode == "display") {
#ifdef _WIN32
    const std::string device = monitor_device_name(config);
    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    if (device.empty() || !EnumDisplaySettingsA(device.c_str(), ENUM_CURRENT_SETTINGS, &mode)) return false;
    width = static_cast<int>(mode.dmPelsWidth);
    height = static_cast<int>(mode.dmPelsHeight);
    return true;
#else
    const DisplayPick pick = pick_display(enumerate_displays(), config.display_id, config.display_name);
    if (!pick.found) return false;
    for (const DisplayInfo& display : enumerate_displays()) {
      if (display.id != pick.id) continue;
      width = display.width;
      height = display.height;
      return width > 0 && height > 0;
    }
    return false;
#endif
  }
  // A window or game target that is not running yet: the primary monitor is the honest guess.
  return primary_monitor_size(width, height);
}


// The machine's memory, read once: the ring's ceiling is the user's cap, at most an eighth of this.
uint64_t system_memory_bytes() {
  static const uint64_t total = [] {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? static_cast<uint64_t>(status.ullTotalPhys) : 0ull;
  }();
  return total;
}

// The peak rate the encoder may reach. A manual rate control (Advanced) brings its own numbers; otherwise the quality level's peak
// decides, for the size actually encoded. The ring never lowers it: the length and the quality are both promises.
int peak_kbps_for(const Config& config, int canvas_width, int canvas_height) {
  int width = 0, height = 0;
  encoder_size(config, canvas_width, canvas_height, width, height);
  int wanted = quality_kbps(config, width, height);
  if (config.encoder_mode == "custom" && config.encoder_options.is_object() && config.encoder_options.has("rate_control")) {
    const int manual = static_cast<int>(std::max(config.encoder_options["bitrate"].as_int(0),
                                                 config.encoder_options["max_bitrate"].as_int(0)));
    if (manual > 0) wanted = manual;
  }
  return std::max(1000, wanted);
}

// The first of `wanted` the encoder's own rate-control list offers, or "" when it has none of them.
std::string listed_rate_control(obs_properties_t* properties, std::initializer_list<const char*> wanted) {
  obs_property_t* property = obs_properties_get(properties, "rate_control");
  if (!property) return std::string();
  const size_t count = obs_property_list_item_count(property);
  for (const char* candidate : wanted) {
    for (size_t index = 0; index < count; ++index) {
      const char* value = obs_property_list_item_string(property, index);
      if (value && std::string(value) == candidate) return candidate;
    }
  }
  return std::string();
}

}  // namespace

ObsHost::~ObsHost() { shutdown(); }

bool ObsHost::initialize(const std::string& runtime_root, const std::string& data_root, const Config& config,
                         std::string& error) {
  if (initialized_) return true;
  runtime_root_ = runtime_root;
  data_root_ = data_root;
  capacity_path_ = data_root + "\\capacity.json";
  config_ = config;
  // A probe, not a setting: BUFFRX_NO_GOVERNOR=1 keeps the encoder governor from stepping, which a benchmark of
  // encoder settings needs - it would otherwise lower the very settings it is measuring.
  governor_disabled_ = GetEnvironmentVariableA("BUFFRX_NO_GOVERNOR", nullptr, 0) > 0;
  governor_step_ = config.encoder_speed_step;  // where the last run settled: the encoder is built at it
  // A probe, not a setting: BUFFRX_TUNE=gpu:-7,eco,below,io switches the candidate optimizations on, one at a time,
  // for a measurement (process_tuning.h, docs/measurement.md).
  {
    char spec[256] = {0};
    GetEnvironmentVariableA("BUFFRX_TUNE", spec, sizeof(spec));
    buffrx::set_active_tuning(buffrx::parse_tuning(spec));
    BX_LOG_INFO("tuning: %s", buffrx::tuning_describe(buffrx::active_tuning()).c_str());
    buffrx::apply_process_tuning(buffrx::active_tuning());
  }

  if (!start_obs(error)) return false;
  // Video before modules, as OBS's own frontend does it: win-capture asks for the graphics device type
  // while it loads, and with no device yet it registers the GDI BitBlt monitor capture instead of DXGI
  // duplication and turns WGC off — which is what cost ~18 ms per frame on display capture.
  {
    // The relief ladder's frame-rate rung is part of what runs, so startup builds the canvas from it too.
    Config running = config;
    // Start where this PC landed last time for the same codec, picture, rate and preset: the ladder is not climbed twice.
    if (running.encoder_speed_step == 0) {
      const int remembered = capacity_remembered_step(capacity_key_for(running));
      if (remembered > 0 && remembered <= buffrx::kMaxReliefStep) {
        running.encoder_speed_step = remembered;
        governor_step_ = remembered;
        BX_LOG_INFO("this PC needed step %d at %d fps %s last time; starting there", remembered, running.capture_fps, running.codec.c_str());
      }
    }
    running.capture_fps = buffrx::relief_capture_fps(running.capture_fps, running.encoder_speed_step);
    // The outputs and the encoders are built from config_, so the step this start runs has to be in it too: the canvas
    // came from `running`, and a config_ still saying step 0 built the encoder at full effort while the canvas ran at 60.
    config_.encoder_speed_step = running.encoder_speed_step;
    if (!configure_video(running, error)) return false;
  }
  if (!load_modules(error)) return false;

  scene_ = obs_scene_create_private("BuffrX capture graph");
  if (!scene_) {
    error = "could not create the internal scene";
    return false;
  }
  obs_set_output_source(0, obs_scene_get_source(scene_));

  initialized_ = true;
  if (!configure_capture(config, error)) return false;
  // Audio opens on demand, with the capture sources (ensure_capture_sources): a WASAPI client holds the
  // sound card open and Windows lists it in the user's mixer, which an engine nobody is buffering should
  // not do. An audio setting changed while the engine sleeps is remembered and applied when it wakes.
  // Canvas-follows-source is NOT done by re-resetting the video after the source exists: a second
  // obs_reset_video at startup hangs inside libobs. The correct approach is to learn the source size
  // from Win32 (GetMonitorInfo) *before* the first reset, and to re-reset only on an explicit
  // resolution change while the outputs are stopped. Recorded in ISSUES.md.
  if (!configure_encoders(config, error)) return false;
  if (!create_outputs(error)) return false;

  // A probe, not a setting: the recording gets encoders of its own and pauses the buffer, as it did before
  // it could share the ring's. Keeps that path testable.
  if (GetEnvironmentVariableA("BUFFRX_NO_SHARED_ENCODER", nullptr, 0) > 0) shared_encoders_ok_ = false;

  reaper_.start([](ReapJob job) {
    obs_output_release(job.output);
    // Released after the output: until it lets go, its own references are what keeps them alive for its own use.
    if (job.video_encoder) obs_encoder_release(job.video_encoder);
    for (obs_encoder* encoder : job.audio_encoders) obs_encoder_release(encoder);
    if (!job.delete_path.empty()) {
      DeleteFileA(job.delete_path.c_str());
      library_release(job.delete_path);
    }
  });

  // The trim worker starts once, with the engine: a bounded queue and one thread, so a clip save
  // never blocks the tick. See work_queue.h for what it is for.
  save_worker_.start([this](PendingSave pending) {
    // A trim re-reads and rewrites a whole clip, at the moment the person is playing: it runs behind the
    // game's own threads, not beside them.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (buffrx::active_tuning().background_io) buffrx::begin_background_io();
    // The trim worker is a thread of its own: a C++ exception here would end the process, so it is
    // caught and named, where the runtime is safe to use.
    try {
      finish_save(pending.path, pending.request, pending.peak_kbps);
    } catch (const std::exception& failure) {
      BX_LOG_ERROR("the trim of %s failed: %s", pending.path.c_str(), failure.what());
    } catch (...) {
      BX_LOG_ERROR("the trim of %s failed with an unknown error", pending.path.c_str());
    }
    library_release(pending.held);
  });

  // Retention: the sessions directory walk and its deletions happen here, never on the tick or a command
  // (prune_sessions queues one of these).
  prune_worker_.start([this](PruneJob job) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (buffrx::active_tuning().background_io) buffrx::begin_background_io();
    // A worker thread: an exception here would end the process, so it is caught and named.
    try {
      run_prune(job);
    } catch (const std::exception& failure) {
      BX_LOG_ERROR("the retention sweep failed: %s", failure.what());
    } catch (...) {
      BX_LOG_ERROR("the retention sweep failed with an unknown error");
    }
  });

  // The log line reports what libobs actually built, not what the config asked for: "auto" is a
  // preference, and the canvas size comes from the capture source once the canvas-follows-source
  // sizing has run.
  obs_video_info video{};
  obs_get_video_info(&video);
  BX_LOG_INFO("engine ready: libobs %s, canvas %ux%u, output %ux%u @ %u fps, encoder %s", obs_version().c_str(),
              video.base_width, video.base_height, video.output_width, video.output_height, video.fps_num,
              encoder_id_.empty() ? "none" : encoder_id_.c_str());
  return true;}

bool ObsHost::start_obs(std::string& error) {
  // The package keeps the OBS binaries in bin/64bit, which the Windows loader does not search by
  // default: tell it once, before anything loads a DLL from there.
#ifdef _WIN32
  const std::string binaries = runtime_root_ + "\\bin\\64bit";
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
  const std::wstring wide = wide_path(binaries);
  AddDllDirectory(wide.c_str());
  // OBS always runs from bin/64bit, and win-capture relies on it: it finds graphics-hook64.dll and the
  // Vulkan layer as "..\..\data\obs-plugins\win-capture\" relative to the working directory. Launched
  // from anywhere else, game capture had no hook to inject and could never capture a game.
  SetCurrentDirectoryW(wide.c_str());
#else
  (void)runtime_root_;
#endif

  const std::string module_config = slashify(data_root_);
#ifndef _WIN32
  // The Ubuntu obs-ffmpeg build hardcodes its muxer helper's path (the package patches an absolute
  // path into the plugin). The shimmed copy points at /tmp, where the engine puts a fresh helper.
  const std::string helper = std::string(runtime_root_) + "/obs-ffmpeg-mux";
  if (CopyFileA(helper.c_str(), "/tmp/buffrx-obs-ffmpeg-mux", FALSE)) chmod("/tmp/buffrx-obs-ffmpeg-mux", 0755);
#endif
  if (!obs_startup("en-US", module_config.c_str(), nullptr)) {
    error = "libobs failed to start";
    return false;
  }
  register_ring_output();
  const std::string version = obs_get_version_string();
  if (version.rfind(std::to_string(LIBOBS_API_MAJOR_VER) + ".", 0) != 0) {
    // The package pins its engine; a DLL of another major version than the headers it was built with (runtime\pin.json)
    // means a broken install, and failing loudly here is cheaper than debugging a mismatched ABI later.
    error = "unexpected libobs version: " + version;
    return false;
  }
  {
    // libobs spins for the end of every frame; its wait is replaced before the video thread exists (the first obs_reset_video).
    std::string sleep_note;
    if (install_sleep_patch(sleep_note)) BX_LOG_NOTE("%s", sleep_note.c_str());
    else BX_LOG_NOTE("the frame wait of libobs is unchanged (%s)", sleep_note.c_str());
  }
  obs_hotkey_enable_background_press(false);  // hotkeys belong to the engine, not OBS
  // libobs concatenates a data path with a file name without inserting a separator, so the trailing
  // backslash is required for effects and locale files to be found. The call is deprecated in libobs
  // 32 but still the only way to point it at its own effect files (see video_probe.cpp).
  const std::string core_data = slashify(runtime_root_ + "\\data\\libobs\\");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  obs_add_data_path(core_data.c_str());
#pragma clang diagnostic pop
  return true;
}

bool ObsHost::load_modules(std::string& error) {
  struct Module {
    const char* name;
    bool required;
  };
#ifdef _WIN32
  const Module modules[] = {
      {"win-capture", true},  {"win-wasapi", true},   {"obs-ffmpeg", true},  {"obs-nvenc", false},
      {"obs-qsv11", false},    {"obs-x264", false},    {"obs-filters", false}, {"image-source", false},
  };
  for (const auto& module : modules) {
    const std::string binary = slashify(runtime_root_ + "\\obs-plugins\\64bit\\" + module.name + ".dll");
    const std::string data = slashify(runtime_root_ + "\\data\\obs-plugins\\" + module.name + "\\");
#else
  const Module modules[] = {
      {"linux-capture", true}, {"linux-pulseaudio", true}, {"obs-ffmpeg", true}, {"obs-outputs", true},
      {"obs-x264", false},     {"obs-filters", false},     {"image-source", false},
  };
  for (const auto& module : modules) {
    const std::string binary = slashify(runtime_root_ + "/obs-plugins/" + module.name + ".so");
    const std::string data = slashify(runtime_root_ + "/data/obs-plugins/" + module.name + "/");
#endif
    if (GetFileAttributesA(binary.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (module.required) {
        error = std::string("required module missing from the package: ") + module.name;
        return false;
      }
      continue;
    }
    obs_module_t* handle = nullptr;
    const bool opened = obs_open_module(&handle, binary.c_str(), data.c_str()) == MODULE_SUCCESS;
    const bool started = opened && obs_init_module(handle);
    if (!started) {
      if (module.required) {
        error = std::string("required module failed to load: ") + module.name;
        return false;
      }
      BX_LOG_WARN("optional module unavailable: %s", module.name);
    } else {
      BX_LOG_INFO("module loaded: %s", module.name);
    }
  }
  obs_post_load_modules();
  return true;
}

CanvasPlan ObsHost::plan_canvas_for(const Config& user) {
  int target_width = 0;
  int target_height = 0;
  const bool target_known = win32_target_size(user, target_width, target_height);
  if (!target_known) {
    BX_LOG_WARN("capture target size not known before video initialisation; using the configured size");
  }
  target_width_ = target_width;
  target_height_ = target_height;
  const Config config = layered(user);
  // Performance mode draws the capture straight into a canvas the size of the output. With the canvas at the capture's own size
  // it is drawn there and then scaled to the output, a second pass over every pixel; measured, the single pass is the better
  // 1% low (+0.9 points, docs/measurement.md) at no cost to the picture, since the mode scales bilinear either way. Only when
  // the user chose an output size: at Native the canvas is the capture's own, and there is nothing to scale to.
  const bool canvas_is_output = performance_active() && config.output_width > 0 && config.output_height > 0;
  const CanvasPlan plan = plan_canvas(config, canvas_is_output ? 0 : target_width, canvas_is_output ? 0 : target_height);
  canvas_width_ = plan.canvas_width;
  canvas_height_ = plan.canvas_height;
  BX_LOG_INFO("canvas plan: target %dx%d, canvas %dx%d, output %dx%d", target_width, target_height, plan.canvas_width,
              plan.canvas_height, plan.output_width, plan.output_height);
  return plan;
}

Config ObsHost::layered(const Config& user) const {
  if (user.performance_when != bx::PerformanceWhen::always && !performance_game_) return user;
  return bx::apply_performance_plan(user, bx::performance_plan());
}

bool ObsHost::configure_video(const Config& requested, std::string& error) {
  // Audio is set up once. The settings never change (48 kHz stereo, which RNNoise needs), and a second
  // obs_reset_audio frees libobs's audio output and builds it again: for that moment obs->audio.audio is
  // null, and a WASAPI capture thread delivering audio into it faults inside obs.dll (offset 0x5f6ff, a
  // null read of that pointer: the two crashes Windows logged on 2026-10-24, after turning Performance
  // mode on). Every video change used to come through here, so every one of them risked it.
  if (!audio_ready_) {
    obs_audio_info audio{};
    audio.samples_per_sec = 48000;
    audio.speakers = SPEAKERS_STEREO;
    if (!obs_reset_audio(&audio)) {
      error = "audio initialisation failed";
      return false;
    }
    audio_ready_ = true;
  }

  // Machines differ: a GPU-less or headless box, a driver that refuses GPU conversion, or a
  // resolution the capture backend rejects. Try the requested configuration first, then honest
  // fallbacks, and record which one was used so the UI can report it.
  struct Variant {
    uint32_t base_width;
    uint32_t base_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t fps;
    enum video_format format;
    bool gpu_conversion;
    const char* label;
  };
  // The plan first: it learns the capture target's size, which the Performance layer is planned from.
  const CanvasPlan plan = plan_canvas_for(requested);
  const Config config = layered(requested);
  const uint32_t fps = static_cast<uint32_t>(config.capture_fps > 0 ? config.capture_fps : 60);
  const Variant variants[] = {
      {static_cast<uint32_t>(plan.canvas_width), static_cast<uint32_t>(plan.canvas_height),
       static_cast<uint32_t>(plan.output_width), static_cast<uint32_t>(plan.output_height), fps, VIDEO_FORMAT_NV12, true,
       "requested"},
      {static_cast<uint32_t>(plan.canvas_width), static_cast<uint32_t>(plan.canvas_height),
       static_cast<uint32_t>(plan.output_width), static_cast<uint32_t>(plan.output_height), fps, VIDEO_FORMAT_NV12, false,
       "no GPU conversion"},
      {static_cast<uint32_t>(plan.output_width), static_cast<uint32_t>(plan.output_height),
       static_cast<uint32_t>(plan.output_width), static_cast<uint32_t>(plan.output_height), fps, VIDEO_FORMAT_NV12, true,
       "no canvas scale"},
      {1920, 1080, 1920, 1080, 60, VIDEO_FORMAT_NV12, true, "1080p60 default"},
      {1280, 720, 1280, 720, 30, VIDEO_FORMAT_NV12, false, "720p30 compatibility"},
  };

#ifdef _WIN32
  const std::string graphics = slashify(runtime_root_ + "\\bin\\64bit\\libobs-d3d11.dll");
#else
  const std::string graphics = slashify(runtime_root_ + "/libobs/libobs-opengl.so");
#endif

  // How much memory the machine has left. libobs allocates the graphics device and the frame pool
  // inside obs_reset_video, and when the machine cannot give it, it does not return an error - it
  // faults. So the check happens here, where an answer can still be given: a game holding twelve
  // gigabytes leaves the capture with nothing to work with, and the engine says so instead of
  // vanishing mid-startup.
  char probe[32] = {0};
  GetEnvironmentVariableA("BUFFRX_TEST_FREE_MB", probe, sizeof(probe));  // a probe, not a setting (memory_policy.h)
  const int64_t free_mb = free_memory_reading(system_free_memory_mb(), probe);
  if (!capture_memory_ok(free_mb)) {
    BX_LOG_WARN("only %lld MB of memory is free; the graphics device will not fit", static_cast<long long>(free_mb));
    memory_full_ = true;
    error = memory_full_message(!initialized_);
    return false;
  }
  memory_full_ = false;

  // libobs fails video initialisation silently when one of its core effects cannot be loaded, so ask
  // the same question it asks before trying: can every required effect be found on disk?
  for (const char* required : {"default.effect", "opaque.effect", "solid.effect", "repeat.effect",
                               "format_conversion.effect", "bicubic_scale.effect", "lanczos_scale.effect",
                               "area.effect", "bilinear_lowres_scale.effect", "premultiplied_alpha.effect"}) {
    char* found = obs_find_data_file(required);
    BX_LOG_INFO("data file %s: %s", required, found ? found : "MISSING");
    if (found) bfree(found);
  }

  // The adapter: the one a display capture's display hangs off, else the card with the most memory (gpu.h).
  // Adapter 0 is tried after it if libobs refuses the choice, so this can only help a machine that works.
#ifdef _WIN32
  const std::vector<GpuAdapterInfo> adapters = gpu_adapters();
  const int chosen_adapter = pick_gpu_adapter(adapters, config.capture_mode == "display" ? monitor_device_name(config, &display_warned_name_) : std::string());
  if (adapters.size() > 1) BX_LOG_INFO("%zu graphics adapters; using adapter %d", adapters.size(), chosen_adapter);
  std::vector<int> adapter_order = {chosen_adapter};
  if (chosen_adapter != 0) adapter_order.push_back(0);
#else
  // libobs's OpenGL backend has no adapter index to choose; the driver's default is the one OBS uses.
  std::vector<int> adapter_order = {0};
#endif

#ifndef _WIN32
  BX_LOG_INFO("graphics module: %s", graphics.c_str());
#endif
  int last_code = 0;
  for (const int adapter_index : adapter_order)
  for (const Variant& variant : variants) {
    obs_video_info video{};
    video.graphics_module = graphics.c_str();
    video.fps_num = variant.fps;
    video.fps_den = 1;
    video.base_width = variant.base_width;
    video.base_height = variant.base_height;
    video.output_width = variant.output_width;
    video.output_height = variant.output_height;
    video.output_format = variant.format;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_PARTIAL;
    video.gpu_conversion = variant.gpu_conversion;
    video.scale_type = static_cast<obs_scale_type>(scale_type_for_filter(config.scale_filter));
    video.adapter = static_cast<uint32_t>(adapter_index);
    last_code = obs_reset_video(&video);
    if (last_code == OBS_VIDEO_SUCCESS) {
      BX_LOG_INFO("video initialised: canvas %ux%u, output %ux%u @ %u fps (%s, adapter %d)", variant.base_width,
                  variant.base_height, variant.output_width, variant.output_height, variant.fps, variant.label, adapter_index);
      // Every reset makes a new D3D11 device, and the GPU queue priority belongs to the device.
      if (buffrx::active_tuning().gpu_priority != 0) {
        obs_enter_graphics();
        void* device = gs_get_device_obj();
        obs_leave_graphics();
        buffrx::apply_gpu_priority(device, buffrx::active_tuning().gpu_priority);
      }
      return true;
    }
    BX_LOG_WARN("video variant '%s' refused (code %d)", variant.label, last_code);
  }
  #ifdef _WIN32
  error = "video initialisation failed (D3D11), code " + std::to_string(last_code);
#else
  error = "video initialisation failed (OpenGL), code " + std::to_string(last_code);
#endif
  return false;
}

bool ObsHost::configure_capture(const Config& requested, std::string& error, bool window_fallback, bool layered) {
  // The follow layer: the display stands in for the game while the game is not in front. The user's config is not touched.
  Config followed;
  const bool follow = follow_display_ && requested.capture_mode == "game";
  if (follow) {
    followed = requested;
    followed.capture_mode = "display";
  }
  const Config& config = follow ? followed : requested;
  if (!layered) drop_retiring_capture();
  if (capture_item_) {
    if (layered && capture_item_ready()) {
      // It is delivering: keep it under the new source until the new one has a frame (an older one still
      // waiting to retire goes now - it is two swaps behind).
      drop_retiring_capture();
      retiring_item_ = capture_item_;
      retiring_since_ms_ = GetTickCount();
    } else {
      obs_sceneitem_remove(capture_item_);  // never delivered: nothing to keep
    }
    capture_item_ = nullptr;
  }
  obs_data_t* settings = obs_data_create();
  const char* source_id = "monitor_capture";
  std::string target = "display";

  game_fallback_ = window_fallback;
  capture_created_ms_ = GetTickCount();
#ifdef _WIN32
  if (config.capture_mode == "game" && window_fallback && !config.game_title.empty()) {
    // The game hook delivered nothing (anti-cheat, a renderer the hook does not know, a protected
    // process): capture the same window through Windows Graphics Capture, which needs no injection.
    source_id = "window_capture";
    obs_data_set_string(settings, "window", config.game_title.c_str());
    obs_data_set_int(settings, "method", 2);
    obs_data_set_int(settings, "priority", 2);
    obs_data_set_bool(settings, "capture_cursor", config.capture_cursor);
    obs_data_set_bool(settings, "client_area", true);
    target = decode_window_part(config.game_title.substr(0, config.game_title.find(':')));
  } else if (config.capture_mode == "game") {
    source_id = "game_capture";
    if (config.game_title.empty()) {
      obs_data_set_string(settings, "capture_mode", "any_fullscreen");
      target = "any fullscreen game";
    } else {
      obs_data_set_string(settings, "capture_mode", "window");
      obs_data_set_string(settings, "window", config.game_title.c_str());
      // The id is "title:class:exe"; the label the user sees is the title alone.
      const std::string title = decode_window_part(config.game_title.substr(0, config.game_title.find(':')));
      target = title.empty() ? config.game_title : title;
    }
    // An integer in game-capture: 0 class, 1 title, 2 exe. Matching by executable survives a game
    // that changes its window title between menus and matches.
    obs_data_set_int(settings, "priority", 2);
    obs_data_set_bool(settings, "capture_cursor", config.capture_cursor);
  } else if (config.capture_mode == "window") {
    source_id = "window_capture";
    obs_data_set_string(settings, "window", config.window_id.c_str());
    obs_data_set_int(settings, "method", 2);  // WGC: no injection, lowest trust cost
    obs_data_set_bool(settings, "capture_cursor", config.capture_cursor);
    obs_data_set_bool(settings, "client_area", true);
    target = config.window_name.empty() ? "window" : config.window_name;
  } else {
    obs_data_set_int(settings, "monitor", config.display_id);
    // The DXGI duplicator ignores "monitor" and resolves "monitor_id"; it accepts the GDI device name
    // (\\.\DISPLAY1) as a fallback match. Without it the source captures nothing.
    const std::string device = monitor_device_name(config, &display_warned_name_);
    if (!device.empty()) obs_data_set_string(settings, "monitor_id", device.c_str());
    const int method = capture_method_for(config.display_method);
    obs_data_set_int(settings, "method", method);  // 0 auto: duplication, WGC fallback
    obs_data_set_bool(settings, "capture_cursor", config.capture_cursor);
    target = config.display_name.empty() ? "display" : config.display_name;
  }
#else
  if (config.capture_mode == "display" || (config.capture_mode == "game" && config.game_title.empty())) {
    // The X11 SHM screen capture. Its "screen" is the Xinerama screen index, which is the same order
    // the display picker uses; the monitor's device name is kept for the log and the config only.
    source_id = "xshm_input";
    obs_data_set_int(settings, "screen", config.display_id);
    obs_data_set_bool(settings, "show_cursor", config.capture_cursor);
    target = config.display_name.empty() ? "display" : config.display_name;
  } else {
    // XComposite captures one window by its title (the first part of the picker's id). A game and a
    // window are the same thing on Linux: OBS has no injection-based game hook here.
    source_id = "xcomposite_input";
    std::string title = config.capture_mode == "window"
                            ? (config.window_name.empty() ? decode_window_part(config.window_id.substr(0, config.window_id.find(':')))
                                                          : config.window_name)
                            : decode_window_part(config.game_title.substr(0, config.game_title.find(':')));
    obs_data_set_string(settings, "capture_window", title.c_str());
    obs_data_set_bool(settings, "show_cursor", config.capture_cursor);
    target = title.empty() ? "window" : title;
  }
#endif

  capture_ = obs_source_create(source_id, "BuffrX capture", settings, nullptr);
  obs_data_release(settings);
  if (!capture_) {
    error = std::string("capture source could not be created: ") + source_id;
    return false;
  }
  obs_sceneitem_t* item = obs_scene_add(scene_, capture_);
  if (!item) {
    // Nothing would deliver and nothing would say why: the caller (and the user's panel) get the reason instead.
    obs_source_release(capture_);
    capture_ = nullptr;
    error = std::string("the capture source could not be added to the scene: ") + source_id;
    return false;
  }
  {
    obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
    obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
    // Fit the source to the canvas, not to the encoder's output size: the video thread scales once,
    // and the scene stays at 1:1 whenever canvas and source agree.
    struct vec2 bounds{};
    bounds.x = static_cast<float>(canvas_width_ > 0 ? canvas_width_ : (config.output_width > 0 ? config.output_width : 1920));
    bounds.y = static_cast<float>(canvas_height_ > 0 ? canvas_height_ : (config.output_height > 0 ? config.output_height : 1080));
    obs_sceneitem_set_bounds(item, &bounds);
  }
  // The scene owns the source from here; hold only the item. Releasing our own reference keeps the
  // refcount honest, and every later read goes through the item.
  capture_item_ = item;
  // Which display this source actually resolved to, so the tick can tell when the choice moved. Set only
  // once the source is in the scene: a failed configure leaves the old value, and the tick retries.
  capture_display_device_ = config.capture_mode == "display" ? monitor_device_name(config, &display_warned_name_) : std::string();
  obs_source_release(capture_);
  capture_ = nullptr;
  // A new source starts unlimited.
  capture_fit_applied_ = false;
  update_capture_fit();
  BX_LOG_INFO("capture source %s -> %s", source_id, target.c_str());
  return true;
}

void ObsHost::drop_retiring_capture() {
  if (!retiring_item_) return;
  obs_sceneitem_remove(retiring_item_);
  retiring_item_ = nullptr;
}

void ObsHost::settle_retiring_capture() {
  if (!retiring_item_) return;
  const bool timed_out = GetTickCount() - retiring_since_ms_ >= kRetireMaxMs;
  // No current source at all (it could not be created): nothing is left to take over, and the old frames
  // would be the wrong thing's.
  if (capture_item_ready() || !capture_item_ || timed_out) {
    if (timed_out && capture_item_ && !capture_item_ready()) {
      BX_LOG_WARN("the new capture source had no frame in %u ms; the one it replaced is let go", kRetireMaxMs);
    }
    drop_retiring_capture();
  }
}

void ObsHost::release_idle_sources() {
  drop_retiring_capture();
  // The capture source goes first: it is the one that pulls frames. The scene item holds it.
  if (capture_item_) {
    obs_sceneitem_remove(capture_item_);
    capture_item_ = nullptr;
  }
  if (capture_) {
    obs_source_release(capture_);
    capture_ = nullptr;
  }
  // Nothing is delivering, so the tick's display check has nothing to compare with until something does.
  capture_display_device_.clear();
  // The audio sources go with it: a WASAPI client holds the sound card open, and Windows lists that
  // client as a source in the user's mixer. The canvas channel holds its own reference, so the meter is
  // detached and the channel cleared first (as shutdown and update_audio_channel do); releasing the
  // source alone left every audio source alive, with its device and its audio thread, while idle.
  detach_meter(&desktop_volmeter_);
  detach_meter(&mic_volmeter_);
  obs_set_output_source(kDesktopViewChannel, nullptr);
  obs_set_output_source(kMicViewChannel, nullptr);
  if (desktop_) {
    obs_source_release(desktop_);
    desktop_ = nullptr;
  }
  if (mic_) {
    if (mic_noise_suppress_) { obs_source_filter_remove(mic_, mic_noise_suppress_); obs_source_release(mic_noise_suppress_); mic_noise_suppress_ = nullptr; }
    if (mic_noise_gate_) { obs_source_filter_remove(mic_, mic_noise_gate_); obs_source_release(mic_noise_gate_); mic_noise_gate_ = nullptr; }
    obs_source_release(mic_);
    mic_ = nullptr;
  }
  sources_released_ = true;
  BX_LOG_INFO("the capture and audio sources were released: the engine has nothing to do");
}

bool ObsHost::ensure_capture_sources(std::string& error) {
  if (!wake_from_deep_idle(error)) return false;
  // The scene item holds the source: that is what tells us whether it is there.
  if (!capture_item_) {
    std::string capture_error;
    // A fallback already taken for this game is taken again: the hook that delivered nothing does not
    // start delivering because the buffer was switched off and on. A new game target resets it (apply).
    if (!configure_capture(config_, capture_error, game_fallback_)) {
      error = capture_error;
      return false;
    }
  }
  configure_audio(config_);
  sources_released_ = false;
  return true;
}

bool ObsHost::enter_deep_idle() {
  if (!initialized_ || deep_idle_) return true;
  RebuildGuard rebuilding(rebuild_gate_);
  // Re-checked here, under the lock: the tick asked a moment ago, and a start may have come in since.
  if (buffer_intent_ || recording_active_ || manual_active_ || recording_pending_ || !sources_released_ || capture_item_ || desktop_ || mic_) return false;
  const uint32_t started = GetTickCount();
  // The outputs and encoders hold the video handler that the reset frees, so they go first, as in a video change.
  // The encoder's settings stay as a snapshot of what last ran; the Video page derives the next settings from the config.
  if (video_encoder_) {
    if (encoder_snapshot_) obs_data_release(encoder_snapshot_);
    encoder_snapshot_ = obs_encoder_get_settings(video_encoder_);
  }
  stop_buffer();
  destroy_outputs();
  if (video_encoder_) {
    obs_encoder_release(video_encoder_);
    video_encoder_ = nullptr;
  }
  for (auto* encoder : audio_encoders_) obs_encoder_release(encoder);
  audio_encoders_.clear();
  Config sleeping = config_;
  sleeping.capture_fps = buffrx::deep_idle_fps(effective_config().capture_fps);
  std::string error;
  deep_idle_ = true;  // the pipeline is down from here: set before the reset so a failure below wakes it
  if (!configure_video(sleeping, error)) {
    BX_LOG_WARN("could not drop to the idle rate: %s; building the pipeline again", error.c_str());
    deep_idle_failed_ms_ = GetTickCount() == 0 ? 1 : GetTickCount();
    std::string wake_error;
    if (!wake_from_deep_idle(wake_error)) {
      last_error_ = "the capture pipeline could not be rebuilt after the idle rate failed: " + wake_error;
      BX_LOG_ERROR("%s", last_error_.c_str());
      if (on_error) on_error("buffer", last_error_);
    }
    return false;
  }
  deep_idle_failed_ms_ = 0;
  BX_LOG_INFO("idle: the canvas now ticks at %d fps (was %d), the encoder and outputs are released (%u ms)",
              sleeping.capture_fps, effective_config().capture_fps, GetTickCount() - started);
  return true;
}

bool ObsHost::wake_from_deep_idle(std::string& error) {
  if (!deep_idle_) return true;
  RebuildGuard rebuilding(rebuild_gate_);
  const uint32_t started = GetTickCount();
  // No capture source exists (sleep requires them released), so the video may be reset: the same order as a
  // video change in apply().
  if (!configure_video(effective_config(), error)) return false;
  if (!configure_encoders(config_, error)) return false;
  if (!create_outputs(error)) return false;
  // What a wake builds is what runs. Without this the next apply compared against the pipeline from
  // before the sleep: a setting changed while asleep looked like a change again and emptied a ring the
  // person had just started.
  built_config_ = effective_config();
  built_config_valid_ = true;
  if (encoder_snapshot_) {
    obs_data_release(encoder_snapshot_);
    encoder_snapshot_ = nullptr;
  }
  deep_idle_ = false;
  idle_since_ms_ = 0;
  BX_LOG_INFO("woke from idle: the canvas is back at %d fps (%u ms)", effective_config().capture_fps, GetTickCount() - started);
  return true;
}

void ObsHost::check_game_fallback() {
  if (game_fallback_ || capture_mode_now() != "game" || config_.game_title.empty()) return;
  if (capture_item_ready() || GetTickCount() - capture_created_ms_ < kGameHookGraceMs) return;
  BX_LOG_WARN("game capture produced no frame in %u ms; capturing the game's window instead", kGameHookGraceMs);
  std::string error;
  if (!configure_capture(config_, error, true, true)) {
    BX_LOG_WARN("window fallback for the game failed: %s", error.c_str());
    // configure_capture let go of the game source before it failed, so nothing would be capturing at all (and the
    // check above never fires again): put the hook source back, and stop trying.
    std::string restore_error;
    if (!configure_capture(config_, restore_error, false, true)) {
      BX_LOG_WARN("the game source could not be restored either: %s", restore_error.c_str());
    }
    game_fallback_ = true;  // tried once; the next target change resets it
  }
}

void ObsHost::check_display_present() {
  const uint32_t now = GetTickCount();
  if (now - display_check_at_ < kDisplayCheckMs) return;
  display_check_at_ = now;
  if (capture_mode_now() != "display" || rebuild_gate_.busy()) return;
  const std::string wanted = monitor_device_name(config_, &display_warned_name_);
  if (!display_follow_needed(capture_display_device_, wanted)) return;
  // Idle sources are released, not lost: nothing is delivering and there is nothing to follow until the
  // buffer or a recording asks for a source again.
  if (!capture_item_ && capture_display_device_.empty()) return;
  // The monitor the capture resolved to is not the one the settings mean now. The source moves; the
  // config keeps the choice, so the screen coming back is followed back without the user fixing anything.
  std::string error;
  if (!configure_capture(config_, error, false, true)) {
    BX_LOG_WARN("display follow failed: %s", error.c_str());
    return;
  }
  if (on_notice) {
    const bool lost = pick_display(enumerate_displays(), config_.display_id, config_.display_name).primary_fallback;
    on_notice(lost ? "Display disconnected" : "Display reconnected",
              lost ? "Capturing the primary display until it comes back" : "Capturing the chosen display again");
  }
}

void ObsHost::update_capture_fit() {
  if (!capture_item_) return;
  obs_source_t* source = capture_source();
  const int width = source ? static_cast<int>(obs_source_get_width(source)) : 0;
  const int height = source ? static_cast<int>(obs_source_get_height(source)) : 0;
  const bool fill = capture_fills_canvas(config_.stretch_mode, width, height, canvas_width_, canvas_height_);
  if (capture_fit_applied_ && fill == capture_stretched_) return;
  obs_sceneitem_set_bounds_type(capture_item_, fill ? OBS_BOUNDS_STRETCH : OBS_BOUNDS_SCALE_INNER);
  capture_stretched_ = fill;
  capture_fit_applied_ = true;
  if (width > 0) {
    BX_LOG_INFO("capture %dx%d %s the %dx%d canvas", width, height, fill ? "stretched to fill" : "fitted inside",
                canvas_width_, canvas_height_);
  }
}

obs_source_t* ObsHost::capture_source() const {
  return capture_item_ ? obs_sceneitem_get_source(capture_item_) : nullptr;
}

void ObsHost::attach_meter(obs_source* source, obs_volmeter** meter, std::atomic<float>* into) {
  detach_meter(meter);
  *meter = obs_volmeter_create(OBS_FADER_LOG);
  if (!*meter) return;
  if (!obs_volmeter_attach_source(*meter, source)) {
    obs_volmeter_destroy(*meter);
    *meter = nullptr;
    return;
  }
  obs_volmeter_add_callback(*meter, into == &mic_level_db_ ? on_microphone_sample : on_volume_sample, into);
  audio_meters_active_.store(true, std::memory_order_relaxed);
}

void ObsHost::detach_meter(obs_volmeter** meter) {
  if (*meter) {
    obs_volmeter_destroy(*meter);
    *meter = nullptr;
  }
  if (meter == &desktop_volmeter_) {
    desktop_level_db_.store(kAudioMeterSilenceDb, std::memory_order_relaxed);
    level_desktop_.store(0.0f, std::memory_order_relaxed);
  } else if (meter == &mic_volmeter_) {
    mic_level_db_.store(kAudioMeterSilenceDb, std::memory_order_relaxed);
    level_mic_.store(0.0f, std::memory_order_relaxed);
  }
  audio_meters_active_.store(desktop_volmeter_ || mic_volmeter_, std::memory_order_relaxed);
}

void ObsHost::set_audio_metering(bool enabled) {
  if (audio_metering_ == enabled) return;
  audio_metering_ = enabled;
  if (enabled) {
    if (desktop_) attach_meter(desktop_, &desktop_volmeter_, &desktop_level_db_);
    if (mic_) attach_meter(mic_, &mic_volmeter_, &mic_level_db_);
  } else {
    detach_meter(&desktop_volmeter_);
    detach_meter(&mic_volmeter_);
  }
}

void ObsHost::sample_audio_levels(bool sample_mic) {
  level_desktop_.store(audio_meter_level(audio_meter_take_peak(desktop_level_db_)), std::memory_order_relaxed);
  if (sample_mic)
    level_mic_.store(microphone_meter_level(audio_meter_take_peak(mic_level_db_)), std::memory_order_relaxed);
}

// Live updates for one audio channel. Releasing an audio source under the audio-io thread is the
// race the crash hunt pinned (three 0xc0000005 in the fader path), so a channel is destroyed only
// when it is no longer wanted or its device actually changed — every other setting updates in
// place, which is also what keeps a dragged fader from tearing the pipeline down.
bool ObsHost::update_audio_channel(obs_source*& source, obs_volmeter** meter, std::atomic<float>* level_db,
                                   std::string& bound_device, const char* source_id, const char* source_name,
                                   const char* device_id, float volume, bool muted, uint32_t mixers, bool wanted,
                                   uint32_t view_channel) {
  if (!wanted) {
    if (source) {
      detach_meter(meter);
      // The canvas holds its own reference and keeps the device open: clear the channel, then let the
      // source go.
      obs_set_output_source(view_channel, nullptr);
      obs_source_release(source);
      source = nullptr;
      bound_device.clear();
    }
    return false;
  }
  if (source && bound_device == device_id) {
    obs_source_set_volume(source, volume);
    obs_source_set_muted(source, muted);
    obs_source_set_audio_mixers(source, mixers);
    return true;
  }
  if (source) {
    detach_meter(meter);
    obs_set_output_source(view_channel, nullptr);
    obs_source_release(source);
    source = nullptr;
    bound_device.clear();
  }
  obs_data_t* settings = obs_data_create();
  obs_data_set_string(settings, "device_id", device_id);
  obs_data_set_bool(settings, "use_device_timing", true);
  source = obs_source_create(source_id, source_name, settings, nullptr);
  obs_data_release(settings);
  if (source) {
    bound_device = device_id;
    obs_source_set_volume(source, volume);
    obs_source_set_muted(source, muted);
    if (audio_metering_) attach_meter(source, meter, level_db);
    obs_source_set_audio_mixers(source, mixers);
    // Last, once volume, mute and mixers are set: this is what puts the source on the canvas, and with it
    // in the mix the encoder reads.
    obs_set_output_source(view_channel, source);
  } else {
    BX_LOG_WARN("%s capture could not be created", source_name);
  }
  return source != nullptr;
}

bool ObsHost::configure_audio(const Config& config) {
  // Detach the mic filters only when the mic source is about to be replaced or removed, so they never
  // outlive the source they were attached to; otherwise apply_mic_filters keeps them (and their state).
  const std::string wanted_mic = config.mic_device.empty() ? "default" : config.mic_device;
  if (mic_ && (!config.mic_enabled || mic_device_ != wanted_mic)) {
    if (mic_noise_suppress_) { obs_source_filter_remove(mic_, mic_noise_suppress_); obs_source_release(mic_noise_suppress_); mic_noise_suppress_ = nullptr; }
    if (mic_noise_gate_) { obs_source_filter_remove(mic_, mic_noise_gate_); obs_source_release(mic_noise_gate_); mic_noise_gate_ = nullptr; }
  }
  uint32_t desktop_mixers = 1u << 0;
  if (config.separate_tracks) desktop_mixers |= 1u << 1;  // track 2: desktop audio alone
  uint32_t mic_mixers = 1u << 0;
  if (config.separate_tracks) mic_mixers |= 1u << 2;      // track 3: microphone alone

  const double mic_gain = std::pow(10.0, config.mic_gain_db / 20.0);
  update_audio_channel(desktop_, &desktop_volmeter_, &desktop_level_db_, desktop_device_, kDesktopCaptureSource,
                       "Desktop audio", config.desktop_device.empty() ? "default" : config.desktop_device.c_str(),
                       static_cast<float>(config.desktop_volume), config.desktop_muted, desktop_mixers,
                       config.desktop_enabled, kDesktopViewChannel);
  update_audio_channel(mic_, &mic_volmeter_, &mic_level_db_, mic_device_, kMicCaptureSource, "Microphone",
                       config.mic_device.empty() ? "default" : config.mic_device.c_str(),
                       static_cast<float>(config.mic_volume * mic_gain), config.mic_muted, mic_mixers,
                       config.mic_enabled, kMicViewChannel);
  apply_mic_filters(config);
  return desktop_ != nullptr || mic_ != nullptr;
}

void ObsHost::apply_mic_filters(const Config& config) {
  if (!mic_) {
    if (mic_noise_suppress_) { obs_source_release(mic_noise_suppress_); mic_noise_suppress_ = nullptr; }
    if (mic_noise_gate_) { obs_source_release(mic_noise_gate_); mic_noise_gate_ = nullptr; }
    return;
  }
  if (config.mic_noise_suppress) {
    if (!mic_noise_suppress_) {
      obs_data_t* s = obs_data_create();
      obs_data_set_default_string(s, "method", "rnnoise");
      mic_noise_suppress_ = obs_source_create("noise_suppress_filter", "BuffrX mic noise suppress", s, nullptr);
      obs_data_release(s);
      if (mic_noise_suppress_) obs_source_filter_add(mic_, mic_noise_suppress_);
      else BX_LOG_WARN("noise suppress filter could not be created");
    }
  } else if (mic_noise_suppress_) {
    obs_source_filter_remove(mic_, mic_noise_suppress_);
    obs_source_release(mic_noise_suppress_);
    mic_noise_suppress_ = nullptr;
  }
  if (config.mic_noise_gate) {
    if (!mic_noise_gate_) {
      obs_data_t* s = obs_data_create();
      obs_data_set_default_double(s, "open_threshold", -26.0);
      obs_data_set_default_double(s, "close_threshold", -32.0);
      obs_data_set_default_int(s, "attack_time", 25);
      obs_data_set_default_int(s, "hold_time", 200);
      obs_data_set_default_int(s, "release_time", 150);
      mic_noise_gate_ = obs_source_create("noise_gate_filter", "BuffrX mic noise gate", s, nullptr);
      obs_data_release(s);
      if (mic_noise_gate_) obs_source_filter_add(mic_, mic_noise_gate_);
      else BX_LOG_WARN("noise gate filter could not be created");
    }
  } else if (mic_noise_gate_) {
    obs_source_filter_remove(mic_, mic_noise_gate_);
    obs_source_release(mic_noise_gate_);
    mic_noise_gate_ = nullptr;
  }
}

std::vector<std::string> ObsHost::choose_encoder_ids(const Config& config) const {
  // Which encoder to prefer is a decision about settings and names, so it lives in encoder_choice.cpp where
  // it is checked without libobs. All this owns is the list libobs reports.
  std::vector<std::string> available;
  for (size_t index = 0;; ++index) {
    const char* id = nullptr;
    if (!obs_enum_encoder_types(index, &id) || !id) break;
    available.emplace_back(id);
  }
  return encoder_candidates(available, config.codec, config.encoder);
}

// Builds the video encoder for one candidate with the settings the level, the user and the governor chose.
// False, with `error`, when libobs or the driver refuses to create it - the caller tries the next candidate.
bool ObsHost::build_video_encoder(const std::string& encoder_id, const Config& config, std::string& error) {
  // A probe, not a setting: BUFFRX_TEST_REFUSE_ENCODER="av1,nvenc" makes encoders whose id contains one of those
  // words refuse to start, which is how a machine without them is imitated.
  {
    char refused[512] = {0};
    if (GetEnvironmentVariableA("BUFFRX_TEST_REFUSE_ENCODER", refused, sizeof(refused)) > 0) {
      std::string list = refused;
      // A value that starts with @ names a file read on every build, so a probe can make an encoder
      // refuse mid-session: rewrite the file, then ask for a rebuild.
      if (!list.empty() && list[0] == '@') {
        std::ifstream file(list.substr(1).c_str());
        std::stringstream contents;
        contents << file.rdbuf();
        list = contents.str();
      }
      size_t from = 0;
      while (from <= list.size()) {
        const size_t comma = list.find(',', from);
        const std::string word = list.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
        if (!word.empty() && encoder_id.find(word) != std::string::npos) {
          error = "refused by BUFFRX_TEST_REFUSE_ENCODER";
          return false;
        }
        if (comma == std::string::npos) break;
        from = comma + 1;
      }
    }
  }
  obs_properties_t* properties = obs_get_encoder_properties(encoder_id.c_str());
  obs_data_t* settings = obs_encoder_defaults(encoder_id.c_str());

  // One quality choice becomes two numbers: a constant-quality target the encoder aims at, and a peak rate it may not pass.
  const std::string quality = effective_quality(config);
  const int cap_kbps = peak_kbps_for(config, canvas_width_, canvas_height_);
  const int target = cq_for_codec(config.codec, quality_target(quality));
  const EncoderFamily family = encoder_family(encoder_id);
  BX_LOG_INFO("encoder %s: codec %s, quality %s -> target %d, peak %d kbps, ring %d s, speed step %d", encoder_id.c_str(),
              config.codec.c_str(), quality.c_str(), target, cap_kbps, config.buffer_seconds, governor_step_);
  if (family == EncoderFamily::X264) {
    // x264's names for the same idea: CRF is the target, and the VBV buffer at the peak is the cap.
    set_if_supported(settings, properties, "rate_control", OBS_DATA_STRING, 0, "CRF");
    set_if_supported(settings, properties, "crf", OBS_DATA_NUMBER, target, nullptr);
    set_if_supported(settings, properties, "use_bufsize", OBS_DATA_BOOLEAN, 1, nullptr);
    set_if_supported(settings, properties, "buffer_size", OBS_DATA_NUMBER, cap_kbps, nullptr);
    set_if_supported(settings, properties, "bitrate", OBS_DATA_NUMBER, cap_kbps, nullptr);
  } else {
    // NVENC: constant quality with a ceiling (CQVBR). An encoder without it - AMF, QSV - gets VBR
    // averaging two thirds of the peak, with the peak as its maximum.
    const std::string mode = listed_rate_control(properties, {"CQVBR", "VBR"});
    if (mode == "CQVBR") {
      set_if_supported(settings, properties, "rate_control", OBS_DATA_STRING, 0, "CQVBR");
      set_if_supported(settings, properties, "target_quality", OBS_DATA_NUMBER, target, nullptr);
      set_if_supported(settings, properties, "max_bitrate", OBS_DATA_NUMBER, cap_kbps, nullptr);
      set_if_supported(settings, properties, "bitrate", OBS_DATA_NUMBER, cap_kbps, nullptr);
    } else {
      if (!mode.empty()) set_if_supported(settings, properties, "rate_control", OBS_DATA_STRING, 0, mode.c_str());
      set_if_supported(settings, properties, "bitrate", OBS_DATA_NUMBER, cap_kbps * 2 / 3, nullptr);
      set_if_supported(settings, properties, "max_bitrate", OBS_DATA_NUMBER, cap_kbps, nullptr);
    }
  }

  // The level's encoder speed (NVENC p1..p7, x264 ultrafast..placebo), then the user's explicit
  // options from Advanced over it. Both go through apply_encoder_setting, so a value the encoder does
  // not offer is refused instead of silently ignored.
  // The mode picks the speed tier (Auto follows the quality level); the person's own rows only count in Custom.
  std::vector<EncoderSetting> encoder_settings = configured_encoder_settings(family, config, governor_step_);
  // The governor's steps (encoder_governor.h) go over what was chosen: an encoder that cannot keep up with the
  // speed settings it was given is not helped by being given them again.
  // One tuning for everyone, not a setting (decided with the user, 2026-10-25): NVENC's high-quality tune. Ultra high
  // quality encoded 30-62% of the frames, and the low-latency tunes needed 5-55% more bits for the same quality, which in a
  // RAM ring is memory (docs/measurement.md). A tune left in an older config's encoder_options is overridden here.
  apply_encoder_settings(settings, properties, encoder_settings);
  set_if_supported(settings, properties, "gpu", OBS_DATA_NUMBER, 0, nullptr);

  video_encoder_ = obs_video_encoder_create(encoder_id.c_str(), "BuffrX video", settings, nullptr);
  obs_data_release(settings);
  if (properties) obs_properties_destroy(properties);
  if (!video_encoder_) {
    error = "encoder could not be created: " + encoder_id;
    return false;
  }
  encoder_id_ = encoder_id;
  obs_encoder_set_video(video_encoder_, obs_get_video());
  // The bitrate the encoder was created with, and its peak: the ring's disk tier sizes itself from the peak.
  {
    obs_data_t* created = obs_encoder_get_settings(video_encoder_);
    const std::string rate_control = obs_data_get_string(created, "rate_control");
    const bool has_bitrate = rate_control != "CQP" && rate_control != "ICQ" && rate_control != "Lossless";
    created_kbps_ = has_bitrate ? static_cast<int>(obs_data_get_int(created, "bitrate")) : 0;
    created_max_kbps_ = has_bitrate ? static_cast<int>(obs_data_get_int(created, "max_bitrate")) : 0;
    obs_data_release(created);
  }
  return true;
}

bool ObsHost::configure_encoders(const Config& requested, std::string& error) {
  const Config config = layered(requested);
  if (video_encoder_) {
    obs_encoder_release(video_encoder_);
    video_encoder_ = nullptr;
  }
  encoder_id_.clear();
  for (auto* encoder : audio_encoders_) obs_encoder_release(encoder);
  audio_encoders_.clear();

  // What is tried, in order: the choice as made; the automatic choice if that vendor is not here; and, for a
  // codec this machine has no encoder for (AV1 on a card from before it) or whose encoders all refuse to
  // start, H.264 - which every machine can do, in software if it must. A machine that cannot be given what
  // was asked for is given something that works, and told so.
  struct Attempt {
    Config config;
    std::string id;
  };
  std::vector<Attempt> attempts;
  Config effective = config;
  if (!codec_override_.empty()) effective.codec = codec_override_;
  for (const std::string& id : choose_encoder_ids(effective)) attempts.push_back({effective, id});
  if (effective.encoder != "auto") {
    Config automatic = effective;
    automatic.encoder = "auto";
    for (const std::string& id : choose_encoder_ids(automatic)) {
      bool known = false;
      for (const Attempt& attempt : attempts) known = known || attempt.id == id;
      if (!known) attempts.push_back({automatic, id});
    }
  }
  if (effective.codec != "h264") {
    Config h264 = effective;
    h264.codec = "h264";
    h264.encoder = "auto";
    for (const std::string& id : choose_encoder_ids(h264)) attempts.push_back({h264, id});
  }
  if (attempts.empty()) {
    error = "no usable encoder found for codec " + config.codec;
    return false;
  }
  std::string failures;
  for (const Attempt& attempt : attempts) {
    std::string attempt_error;
    if (!build_video_encoder(attempt.id, attempt.config, attempt_error)) {
      BX_LOG_WARN("encoder %s could not be used: %s", attempt.id.c_str(), attempt_error.c_str());
      failures += (failures.empty() ? "" : "; ") + attempt.id + " (" + attempt_error + ")";
      continue;
    }
    if (attempt.config.codec != effective.codec) {
      codec_override_ = attempt.config.codec;
      pending_notice_title_ = "Using H.264";
      pending_notice_detail_ = "This PC could not start a " + std::string(effective.codec == "av1" ? "AV1" : "HEVC") +
                               " encoder, so BuffrX is recording H.264. Your setting is unchanged.";
      BX_LOG_WARN("no %s encoder could be started here (%s); recording H.264", effective.codec.c_str(), failures.c_str());
    } else if (attempt.config.encoder != effective.encoder) {
      last_error_ = "the " + effective.encoder + " encoder is not available here; using " + attempt.id;
      BX_LOG_WARN("%s", last_error_.c_str());
    } else if (!failures.empty()) {
      pending_notice_title_ = "Encoder changed";
      pending_notice_detail_ = "The first encoder would not start, so BuffrX is using " + attempt.id + ".";
    }
    break;
  }
  if (!video_encoder_) {
    error = "no encoder could be created: " + failures;
    return false;
  }

  // Tracks 1..6 as OBS counts them. `separate_tracks` is the older two-source alias: on means three
  // tracks unless the explicit count asks for more. audio_track_count is the spill's own answer too.
  const int track_count = audio_track_count(config);
  for (int track = 0; track < track_count; ++track) {
    obs_data_t* audio_settings = obs_data_create();
    obs_data_set_int(audio_settings, "bitrate", std::max(64, config.audio_bitrate_kbps));
    obs_encoder_t* encoder =
        obs_audio_encoder_create("ffmpeg_aac", ("BuffrX audio " + std::to_string(track + 1)).c_str(),
                                 audio_settings, static_cast<size_t>(track), nullptr);
    obs_data_release(audio_settings);
    if (!encoder) {
      error = "audio encoder could not be created";
      return false;
    }
    obs_encoder_set_audio(encoder, obs_get_audio());
    audio_encoders_.push_back(encoder);
  }
  return true;
}

bool ObsHost::create_outputs(std::string& error) {
  if (!video_encoder_ || audio_encoders_.empty()) {
    error = "outputs need an encoder first";
    return false;
  }
  const std::string clips_dir = config_.clips_dir;

  obs_data_t* settings = obs_data_create();
  // The ring keeps the length asked for, at the quality asked for: a clip is always that long and never lowered to fit memory. The
  // time is the only limit the user sets. The byte limit is a safety ceiling, an eighth of the machine's memory, so a runaway cannot
  // take the whole machine (a failed allocation ends the engine); a scene that needs more than the ceiling for the whole length is
  // not something a setting can fix, and the ring then holds what the ceiling allows rather than lowering the picture.
  obs_data_set_int(settings, "max_time_sec", std::max(1, config_.buffer_seconds) + 2);
  obs_data_set_int(settings, "max_size_mb", ring_safety_mb(system_memory_bytes()));
  // The disk part (ring_spill.h): footage beyond the RAM limit goes to one reserved file, so a clip stays the length chosen. Its size comes from the
  // peak rate the encoder may reach, so it cannot be outgrown. None when the footage fits in RAM, and none with BUFFRX_NO_SPILL=1 (today's
  // behaviour exactly). BUFFRX_RING_RAM_MB shrinks the RAM limit so a test can overflow it in seconds; it is a probe, not a setting.
  spill_wanted_ = false;
  spill_problem_.clear();
  {
    const int64_t ceiling_bytes = static_cast<int64_t>(ring_safety_mb(system_memory_bytes())) * 1024 * 1024;
    int64_t limit = ring_ram_limit_bytes(ceiling_bytes);
    char probe[32] = {0};
    if (GetEnvironmentVariableA("BUFFRX_RING_RAM_MB", probe, sizeof(probe)) > 0 && std::atoll(probe) > 0) {
      limit = std::min<int64_t>(ceiling_bytes, std::atoll(probe) * 1024 * 1024);
    }
    const bool disabled = GetEnvironmentVariableA("BUFFRX_NO_SPILL", nullptr, 0) > 0;
    const int64_t peak_kbps = std::max(created_max_kbps_, created_kbps_);  // 0 for a rate control with no peak (CQP)
    const int64_t audio_kbps = static_cast<int64_t>(audio_track_count(config_)) * std::max(64, config_.audio_bitrate_kbps);
    const int64_t file_bytes = disabled ? 0 : spill_reservation_bytes(peak_kbps, audio_kbps, std::max(1, config_.buffer_seconds), limit);
    if (file_bytes > 0) {
      const SpillPlacement placement = place_spill_file(clips_dir, data_root_, file_bytes, GetCurrentProcessId());
      if (placement.path.empty()) {
        spill_problem_ = placement.problem;
        BX_LOG_WARN("the replay ring needs a %lld MB file for a %d s buffer and has no place for it (%s)", static_cast<long long>(file_bytes / (1024 * 1024)),
                    config_.buffer_seconds, placement.problem.c_str());
      } else {
        obs_data_set_int(settings, "spill_bytes", file_bytes);
        obs_data_set_int(settings, "ram_limit_mb", limit / (1024 * 1024));
        obs_data_set_string(settings, "spill_path", placement.path.c_str());
        spill_wanted_ = true;
        if (dedupe_wanted()) obs_data_set_bool(settings, "dedupe", true);
      }
    }
  }
  // Forward slashes throughout: libobs splits these paths with strrchr('/'), and a Windows-style
  // backslash path makes that lookup fail (the same trap as effect includes).
  obs_data_set_string(settings, "path", slashify(clips_dir + "\\").c_str());
  obs_data_set_string(settings, "format", config_.filename_format.c_str());
  obs_data_set_string(settings, "extension", config_.container.c_str());
  // The custom muxer settings field: passed to the muxer as-is. Empty means the muxer's own defaults.
  if (!config_.muxer_settings.empty()) obs_data_set_string(settings, "muxer_settings", config_.muxer_settings.c_str());
  // BuffrX's own ring (ring_output.h): the same packets and the same muxer process as OBS's replay buffer, with its own readout.
  replay_ = obs_output_create("buffrx_ring", "BuffrX replay buffer", settings, nullptr);
  if (!replay_) {
    obs_data_release(settings);
    error = "replay buffer output could not be created";
    return false;
  }
  // The file destination is a hidden setting OBS's own frontend applies *after* creation. Applying it
  // the same way is what makes the save procedure know where to write.
  obs_data_t* destination = obs_data_create();
  const std::string destination_path = slashify(clips_dir + "\\");
  obs_data_set_string(destination, "path", destination_path.c_str());
  obs_data_set_string(destination, "directory", destination_path.c_str());
  obs_data_set_string(destination, "format", config_.filename_format.c_str());
  obs_data_set_string(destination, "filename_format", config_.filename_format.c_str());
  obs_data_set_string(destination, "extension", config_.container.c_str());
  obs_data_set_string(destination, "prefix", "");
  obs_data_set_string(destination, "suffix", "");
  obs_output_update(replay_, destination);
  obs_data_release(destination);
  obs_data_t* readback = obs_output_get_settings(replay_);
  if (readback) {
    BX_LOG_INFO("replay settings: path='%s' format='%s' extension='%s' max_time=%lld max_size=%lld",
                obs_data_get_string(readback, "path"), obs_data_get_string(readback, "format"),
                obs_data_get_string(readback, "extension"),
                static_cast<long long>(obs_data_get_int(readback, "max_time_sec")),
                static_cast<long long>(obs_data_get_int(readback, "max_size_mb")));
    obs_data_release(readback);
  }
  obs_data_release(settings);
  obs_output_set_video_encoder(replay_, video_encoder_);
  for (size_t index = 0; index < audio_encoders_.size(); ++index) {
    obs_output_set_audio_encoder(replay_, audio_encoders_[index], index);
  }
  signal_handler_t* signals = obs_output_get_signal_handler(replay_);
  signal_handler_connect(signals, "saved", &ObsHost::on_replay_saved, this);
  signal_handler_connect(signals, "stop", &ObsHost::on_replay_stopped, this);

  recording_ = nullptr;
  return true;
}

void ObsHost::destroy_outputs() {
  // A save belongs to the output that was asked. File one that has already finished before the
  // signal is cut, and fail what is still waiting: the new ring would otherwise save an empty clip,
  // or the request would sit in the queue until its timeout and the person would be told nothing.
  if (replay_ && replay_saved_ && save_worker_.running()) process_saved_replay();
  if (save_in_flight_ || !save_queue_.empty()) {
    save_in_flight_ = false;
    save_timeout_reported_ = false;
    save_queue_.clear();
    last_error_ = "the clip was not saved because the buffer restarted";
    BX_LOG_WARN("%s", last_error_.c_str());
    if (on_error) on_error("save", last_error_);
  }
  if (replay_) {
    signal_handler_t* signals = obs_output_get_signal_handler(replay_);
    signal_handler_disconnect(signals, "saved", &ObsHost::on_replay_saved, this);
    signal_handler_disconnect(signals, "stop", &ObsHost::on_replay_stopped, this);
    if (obs_output_active(replay_)) obs_output_stop(replay_);
    obs_output_release(replay_);
    replay_ = nullptr;
  }
  // Tearing down mid-recording still leaves a finished session: releasing the output stops it and waits
  // for its file, then the open chunk is indexed and the session closed, exactly as a stop would.
  std::lock_guard<std::mutex> lock(recording_mutex_);
  release_manual_recording();
  release_recording_output();
  // A file the muxer split before the release: its change is still queued, and that file is the one that was
  // open. Taken before the wait below, so the wait is on the last file rather than the one before it.
  poll_timeline_split(GetTickCount());
  // The file is normally closed the moment the output is released; a muxer handle that outlives that
  // by a moment is waited for here, on the one thread nothing else needs, so the last chunk of a
  // session still reaches the index.
  const uint32_t released = GetTickCount();
  while (!current_chunk_.empty() && !file_writer_closed(current_chunk_) &&
         GetTickCount() - released < kShutdownFinalWaitMs) {
    Sleep(10);
  }
  const uint32_t now = GetTickCount();
  index_open_chunk(now);
  timeline_chunks_ = TimelineFileIndex{};
  pending_chunk_publish_locked(now);
  if (!pending_chunks_.empty()) {
    const std::string left = std::to_string(pending_chunks_.size()) +
                             " chunk(s) were still held by their writer at shutdown and are left out of the index";
    BX_LOG_ERROR("%s", left.c_str());
  }
  close_session(now);
  chunk_closed_ = false;
  stop_requested_ms_ = 0;
}

void ObsHost::release_recording_output() {
  release_current_output();
}

void ObsHost::release_current_output() {
  // The open chunk's path is NOT cleared here. The chunk is indexed by the tick once libobs says it
  // closed, and the output can be released before that - by a pipeline rebuild, or by a stop - so
  // clearing it here loses a recording the user cannot find or export.
  if (recording_) {
    signal_handler_t* signals = obs_output_get_signal_handler(recording_);
    signal_handler_disconnect(signals, "stop", &ObsHost::on_recording_stopped, this);
    signal_handler_disconnect(signals, "file_changed", &ObsHost::on_recording_file_changed, this);
    if (obs_output_active(recording_)) obs_output_stop(recording_);
    // Asked before the output is released: afterwards there is no output to ask, and a chunk that wrote
    // no frame is a header with no footage that the library would list as a zero second clip.
    closed_chunk_frames_ = static_cast<uint64_t>(obs_output_get_total_frames(recording_));
    closed_chunk_path_ = current_chunk_;
    obs_output_release(recording_);
    recording_ = nullptr;
  }
  destroy_recording_encoders();
}

static bool source_has_frame(obs_source_t* source) {
  return source && obs_source_get_width(source) > 0 && obs_source_get_height(source) > 0;
}

bool ObsHost::capture_item_ready() const { return source_has_frame(capture_source()); }

bool ObsHost::capture_source_ready() const {
  if (capture_item_ready()) return true;
  return retiring_item_ && source_has_frame(obs_sceneitem_get_source(retiring_item_));
}

obs_source_t* ObsHost::delivering_source() const {
  if (capture_item_ready() || !retiring_item_) return capture_source();
  obs_source_t* old = obs_sceneitem_get_source(retiring_item_);
  return source_has_frame(old) ? old : capture_source();
}

bool ObsHost::start_buffer(std::string& error) {
  buffer_intent_ = true;
  // Asleep there are no outputs: they are built again here, before they are looked for.
  if (!wake_from_deep_idle(error)) return false;
  if (!replay_) {
    error = "replay buffer is not available";
    return false;
  }
  // The sources were released while the engine was idle: they are made again here, before the ring
  // waits for the first frame.
  if (!ensure_capture_sources(error)) return false;
  if (obs_output_active(replay_)) {
    if (!buffer_stopping_) {
      buffer_active_ = true;
      return true;
    }
    // The previous stop is still unwinding. Calling the output "running" here is what made stop-then-
    // start look like it worked and then flicker off: let the stop finish so this start is real.
    if (!force_replay_stopped()) {
      error = "the buffer is still stopping";
      return false;
    }
  }
  buffer_stopping_ = false;
  // A capture source with no frames yet would buffer black; waiting for the first frame is why the
  // engine does not start the output at initialisation.
  if (!capture_source_ready()) {
    error = "waiting for the capture source to produce frames";
    buffer_active_ = false;
    return false;
  }
  buffer_active_ = obs_output_start(replay_);
  if (!buffer_active_) {
    error = obs_output_get_last_error(replay_) ? obs_output_get_last_error(replay_) : "replay buffer failed to start";
    return false;
  }
  dedupe_ring_started();
  last_error_.clear();
  return true;
}

void ObsHost::stop_buffer() {
  buffer_intent_ = false;
  stall_reported_ = false;  // a stop is a new chapter: a start after it is not "restarted after a freeze"
  if (replay_ && obs_output_active(replay_)) {
    obs_output_stop(replay_);
    buffer_stopping_ = true;
    buffer_stop_issued_ms_ = GetTickCount();
  }
  buffer_active_ = false;
}

// True once the output is down. It never waits: every caller runs on the tick or on a pipe command,
// under the engine lock, where a sleep is a stalled window thread and a stalled command. A stop that is
// still unwinding is forced once and reported here; the buffer's watchdog comes round again and starts
// the ring when it really is down.
bool ObsHost::force_replay_stopped() {
  if (!replay_) return true;
  if (obs_output_active(replay_)) {
    BX_LOG_WARN("the replay buffer was still stopping; forcing it");
    obs_output_force_stop(replay_);
  }
  const bool down = !obs_output_active(replay_);
  if (down) buffer_stopping_ = false;
  return down;
}

int ObsHost::save_clip(int seconds, std::string& error) {
  if (!replay_ || !obs_output_active(replay_) || buffer_stopping_) {
    // A buffer waiting for its first frame is starting, not stopping: the save was refused either way,
    // but the person reading the message was told the opposite of what the engine was doing.
    error = buffer_stopping_ ? "the buffer is still stopping"
                             : (buffer_intent_ ? "the buffer is still starting" : "the buffer is not running");
    return 1;
  }
  if (!buffer_intent_) {
    error = "the buffer is paused while a recording runs";
    return 3;
  }
  if (save_queue_.size() >= 8) {
    error = "too many saves queued at once";
    return 2;
  }
  if (seconds <= 0) seconds = config_.buffer_seconds;
  seconds = std::min(seconds, config_.buffer_seconds);
  SaveRequest request;
  request.seconds = seconds;
  request.whole_buffer = seconds >= config_.buffer_seconds;
  request.folder = capture_folder();
  request.stem = timestamped_stem_now("Replay");
  request.queued_at = GetTickCount();
  save_queue_.push_back(request);
  return 0;
}

bool ObsHost::create_recording_encoders(std::string& error) {
  if (!video_encoder_) {
    error = "there is no video encoder to copy";
    return false;
  }
  // Copied from the ring's encoder so both outputs encode identically without sharing an instance.
  obs_data_t* settings = obs_encoder_get_settings(video_encoder_);
  recording_video_encoder_ =
      obs_video_encoder_create(encoder_id_.c_str(), "BuffrX recording video", settings, nullptr);
  obs_data_release(settings);
  if (!recording_video_encoder_) {
    error = "the recording video encoder could not be created";
    return false;
  }
  obs_encoder_set_video(recording_video_encoder_, obs_get_video());

  for (size_t index = 0; index < audio_encoders_.size(); ++index) {
    obs_data_t* audio_settings = obs_encoder_get_settings(audio_encoders_[index]);
    obs_encoder_t* encoder = obs_audio_encoder_create(
        "ffmpeg_aac", ("BuffrX recording audio " + std::to_string(index + 1)).c_str(), audio_settings,
        static_cast<size_t>(index), nullptr);
    obs_data_release(audio_settings);
    if (!encoder) {
      error = "a recording audio encoder could not be created";
      destroy_recording_encoders();
      return false;
    }
    obs_encoder_set_audio(encoder, obs_get_audio());
    recording_audio_encoders_.push_back(encoder);
  }
  return true;
}

void ObsHost::destroy_recording_encoders() {
  if (recording_video_encoder_) {
    obs_encoder_release(recording_video_encoder_);
    recording_video_encoder_ = nullptr;
  }
  for (obs_encoder* encoder : recording_audio_encoders_) obs_encoder_release(encoder);
  recording_audio_encoders_.clear();
}

obs_output_t* ObsHost::create_chunk_output(const std::string& path, obs_encoder_t* video,
                                           const std::vector<obs_encoder_t*>& audio, void (*on_stop)(void*, calldata_t*),
                                           bool timeline, std::string& error) {
  obs_data_t* settings = obs_data_create();
  obs_data_set_string(settings, "path", slashify(path).c_str());
  obs_data_set_string(settings, "format", config_.filename_format.c_str());
  const std::string container = timeline ? "mp4" : config_.container;
  obs_data_set_string(settings, "extension", container.c_str());
  // One file for the whole recording, so it must survive a crash: an MP4 normally gets its index at the very end, and a
  // recording the engine did not get to finish would be unreadable. Fragmented, the index is written as it goes.
  const bool mov_family = container == "mp4" || container == "mov";
  if (timeline) {
    // Every Timeline segment is joined to the next by a MediaSource in the preview, so it carries its own
    // init and offsets that are not relative to the file. Forced, never the user's own muxer settings:
    // those belong to a saved clip. Half-second fragments let Live read the same encoded stream without
    // waiting for the next keyframe; chunk starts remain keyframe-aligned for seeking and recovery.
    // obs-ffmpeg-mux writes through a 64 KB buffer of its own (flush_packets=1 was tried and changes
    // nothing): at game bitrates that is milliseconds, on a still desktop it is about two seconds of Live.
    obs_data_set_string(settings, "muxer_settings", "movflags=frag_keyframe+empty_moov+default_base_moof frag_duration=500000");
    // The splitter names every file it opens from these, so it needs the whole destination set: a missing one
    // faults inside libobs's own name helper (a null string handed to strrchr). One muxer process writes the
    // whole session and cuts it on a keyframe; the engine indexes what `file_changed` reports.
    const std::string directory = slashify(session_media_dir(session_.id));
    obs_data_set_string(settings, "directory", directory.c_str());
    obs_data_set_bool(settings, "allow_spaces", true);
    obs_data_set_bool(settings, "split_file", true);
    obs_data_set_int(settings, "max_time_sec", kTimelineSplitSeconds);
    obs_data_set_int(settings, "max_size_mb", 0);
    obs_data_set_bool(settings, "allow_overwrite", false);
  } else if (!config_.muxer_settings.empty()) obs_data_set_string(settings, "muxer_settings", config_.muxer_settings.c_str());
  else if (mov_family) obs_data_set_string(settings, "muxer_settings", "movflags=frag_keyframe+empty_moov");
  obs_output_t* output = obs_output_create("ffmpeg_muxer", timeline ? "BuffrX Timeline" : "BuffrX recording", settings, nullptr);
  obs_data_release(settings);
  if (!output) {
    error = "recording output could not be created";
    return nullptr;
  }
  obs_output_set_video_encoder(output, video);
  for (size_t index = 0; index < audio.size(); ++index) obs_output_set_audio_encoder(output, audio[index], index);
  signal_handler_t* signals = obs_output_get_signal_handler(output);
  signal_handler_connect(signals, "stop", on_stop, this);
  if (timeline) signal_handler_connect(signals, "file_changed", &ObsHost::on_recording_file_changed, this);
  return output;
}

// Creates the encoders and the output for one recording and starts it. Called once per recording (a manual
// one) and once per Timeline segment, which is the same call: the segment's own output splits itself.
//
// A Timeline session is one `ffmpeg_muxer` output with OBS's split writer on, so one muxer process writes every
// file of the session and cuts them on keyframes, instead of one output per chunk with an overlap handover. A
// manual recording is one file and never splits. Caller holds recording_mutex_.
bool ObsHost::start_recording_output(std::string& error) {
  release_recording_output();
  const bool timeline = timeline_writer_.load();
  const bool shared = shared_encoders_ok_ && video_encoder_ && !audio_encoders_.empty();
  recording_shared_ = shared;
  if (!shared) {
    // Timeline has no encoders of its own and never takes the RAM ring's place: without the ring's it
    // writes nothing and says so.
    if (timeline) {
      error = "Timeline records on the buffer's encoders and they are not running";
      return false;
    }
    if (!create_recording_encoders(error)) return false;
  }

  const bool chunked = !session_.id.empty();
  const std::string directory =
      chunked ? session_media_dir(session_.id) : recordings_folder(config_);
  if (!chunked) CreateDirectoryA(directory.c_str(), nullptr);
  const std::string path = reserve_output_path(directory, timestamped_stem_now("Recording"), std::string(".") + chunk_container());

  recording_ = create_chunk_output(path, shared ? video_encoder_ : recording_video_encoder_,
                                   shared ? audio_encoders_ : recording_audio_encoders_, &ObsHost::on_recording_stopped, timeline, error);
  if (!recording_) {
    destroy_recording_encoders();
    return false;
  }
  recording_active_ = obs_output_start(recording_);
  if (!recording_active_) {
    error = obs_output_get_last_error(recording_) ? obs_output_get_last_error(recording_) : "recording failed to start";
    release_recording_output();
    if (shared && !timeline) {
      // libobs refused the shared encoders: the recording gets its own, as it always did, and the buffer
      // pauses for it. Said once, and sharing stays off until the engine restarts.
      BX_LOG_WARN("the recording could not share the ring's encoders (%s); using its own", error.c_str());
      shared_encoders_ok_ = false;
      return start_recording_output(error);
    }
    return false;
  }
  current_chunk_ = path;
  current_chunk_start_ms_ = GetTickCount();
  chunk_start_unconfirmed_ = true;
  // The session index follows the files the splitter opens from here; the first one is the file just started.
  if (timeline && !timeline_chunks_.begin(path, current_chunk_start_ms_)) {
    BX_LOG_WARN("the timeline segment's first span was not opened: %s", path.c_str());
  }
  // The library neither lists nor opens the chunk while it is being written: a file that is still
  // growing has no index yet, and handing it to a decoder is how the engine used to die on a
  // half-written recording.
  library_hold(path);
  BX_LOG_INFO("recording chunk started: %s", path.c_str());
  return true;
}

// A manual file and Timeline have separate output lifetimes, but receive the same encoded packets.
bool ObsHost::start_shared_manual_recording(std::string& error) {
  if (buffrx::abandoned_outputs_exhausted(abandoned_outputs_)) {
    error = "the recording output stopped responding earlier; restart BuffrX to record again";
    return false;
  }
  std::string space;
  if (!recording_space_guard(config_, volume_free_bytes, space)) {
    error = last_error_ = space;
    if (on_error) on_error("recording", space);
    return false;
  }

  bool sharing_refused = false;
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    if (manual_recording_) {
      if (obs_output_active(manual_recording_)) {
        error = manual_active_ ? "a recording is already running" : "the last recording is still being finished; try again in a moment";
        return false;
      }
      release_manual_recording();
    }
    const std::string directory = recordings_folder(config_);
    CreateDirectoryA(directory.c_str(), nullptr);
    manual_file_ = reserve_output_path(directory, timestamped_stem_now("Recording"), std::string(".") + config_.container);
    manual_closed_ = false;
    manual_stop_requested_ms_ = 0;
    manual_stop_code_ = 0;
    manual_opened_ms_ = GetTickCount();
    manual_started_ms_ = 0;
    manual_start_unconfirmed_ = true;
    manual_recording_ = create_chunk_output(manual_file_, video_encoder_, audio_encoders_,
                                             &ObsHost::on_manual_recording_stopped, false, error);
    if (!manual_recording_) { manual_file_.clear(); return false; }
    library_hold(manual_file_);
    manual_active_ = obs_output_start(manual_recording_);
    if (!manual_active_) {
      error = obs_output_get_last_error(manual_recording_) ? obs_output_get_last_error(manual_recording_) : "recording failed to start";
      release_manual_recording();
      sharing_refused = true;
    } else {
      BX_LOG_INFO("manual recording started on shared encoders: %s", manual_file_.c_str());
      last_error_.clear();
    }
  }
  if (!sharing_refused) {
    prune_sessions("");
    return true;
  }

  // Keep the pre-existing separate-encoder fallback when libobs refuses sharing.
  BX_LOG_WARN("the recording could not share the ring's encoders (%s); using its own", error.c_str());
  shared_encoders_ok_ = false;
  if (defer_recording_start(error)) return true;
  return start_recording_inner(error);
}

// Called outside libobs's callback, with recording_mutex_ held.
void ObsHost::release_manual_recording() {
  if (!manual_recording_) return;
  signal_handler_disconnect(obs_output_get_signal_handler(manual_recording_), "stop", &ObsHost::on_manual_recording_stopped, this);
  if (obs_output_active(manual_recording_)) obs_output_stop(manual_recording_);
  const uint64_t frames = static_cast<uint64_t>(obs_output_get_total_frames(manual_recording_));
  obs_output_release(manual_recording_);
  manual_recording_ = nullptr;
  if (!manual_file_.empty()) {
    // A muxer that died before writing a byte still counted encoded frames, and its empty file would be
    // listed as a clip that cannot play.
    if (frames == 0 || file_size_now(manual_file_) == 0) DeleteFileA(manual_file_.c_str());
    library_release(manual_file_);
    if (frames != 0 && on_session_changed) on_session_changed(std::string(), false);
  }
  manual_file_.clear();
  manual_active_ = false;
  manual_closed_ = false;
  manual_stop_requested_ms_ = 0;
  manual_opened_ms_ = 0;
  manual_started_ms_ = 0;
  manual_start_unconfirmed_ = false;
}

bool ObsHost::defer_recording_start(std::string& error) {
  if (!timeline_writer_active() && !recording_) return false;
  // The writer is closing its final chunk; the output slot is free once it has.
  if (timeline_writer_active()) close_timeline(GetTickCount());
  recording_request_ms_ = GetTickCount();
  recording_pending_ = true;
  recording_start_due_ = false;
  error.clear();
  return true;
}

bool ObsHost::start_recording(std::string& error) {
  // The recording path runs on the tick, on libobs's output thread and on the trim worker, and a
  // C++ exception in any of them ends the process. They are caught here, where the runtime is safe
  // to use, and named.
  try {
    if (recording_pending_) { error.clear(); return true; }
    if (!ensure_capture_sources(error)) return false;
    if (!capture_source_ready()) {
      recording_request_ms_ = GetTickCount();
      recording_pending_ = true;
      recording_start_due_ = false;
      error.clear();
      return true;
    }
    if (!shared_encoders_ok_ || !video_encoder_ || audio_encoders_.empty()) {
      // The existing fallback needs the original output slot and pauses the buffer. Wait for a Timeline
      // writer to close instead of replacing it while its final chunk is still being published.
      if (defer_recording_start(error)) return true;
      return start_recording_inner(error);
    }
    return start_shared_manual_recording(error);
  } catch (const std::exception& failure) {
    error = std::string("the recording could not start: ") + failure.what();
    BX_LOG_ERROR("%s", error.c_str());
    return false;
  } catch (...) {
    error = "the recording could not start: an unknown error";
    BX_LOG_ERROR("%s", error.c_str());
    return false;
  }
}

bool ObsHost::start_timeline(std::string& error) {
  try {
    return start_recording_inner(error, true);
  } catch (const std::exception& failure) {
    error = std::string("the timeline could not start: ") + failure.what();
    BX_LOG_ERROR("%s", error.c_str());
    return false;
  } catch (...) {
    error = "the timeline could not start: an unknown error";
    BX_LOG_ERROR("%s", error.c_str());
    return false;
  }
}

void ObsHost::stop_timeline() {
  // The output is stopped the way a manual recording is: the tick that sees it closed indexes its last
  // segment and closes the session. Capture and the RAM buffer are not touched.
  stop_primary_recording();
  timeline_writer_ = false;
}

// One tick of the Timeline writer. The rule that decides whether it writes is timeline_state.h's; this is
// the lifecycle around the one recording output: a session opens on the first Writing tick, its segment is
// closed when the rule says anything else, and the session stays open for a pause so the footage either side
// of a look at the desktop is one session, one reader, one thing.
void ObsHost::poll_timeline(uint32_t now, const TimelineFacts& facts) {
  if (!facts.enabled || !facts.buffer_on) timeline_wake_gap_pending_ = false;
  const TimelineState state = timeline_next_state(facts);
  timeline_state_ = state;
  // A manual stop asked for the end of the session: it closes as soon as its writer has let go, and this
  // tick may neither keep it for a pause (the sleep, the lock, a rebuild) nor open a new one behind it.
  if (timeline_closing_) {
    finish_timeline_close(now);
    return;
  }
  const bool writing = timeline_writes(state);
  const bool session_open = timeline_writer_.load();
  bool segment_open = false;
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    // Timeline's own segment only: a separate-encoder fallback manual recording occupies this output with no
    // Timeline writer behind it, and a session it does not write must not wait for it (recording_blocks_timeline).
    segment_open = timeline_writer_.load() && (recording_ != nullptr || recording_active_);
  }
  // One session spans a game, a look at the desktop and the next game: only Timeline switched off ends it
  // here. A change of game is written as a marker, so the strip says where one game's footage ends.
  if (session_open && !facts.enabled) {
    close_timeline(now);
    return;
  }
  if (session_open && timeline_game_changed(timeline_game_exe_, facts.game_exe)) {
    std::string session_id, marker_error;
    SessionMarker marker;
    const std::string label = media_folder_name(facts.game_exe);
    if (add_marker("game", label, session_id, marker, marker_error)) {
      BX_LOG_INFO("timeline marker: game changed to %s", facts.game_exe.c_str());
    } else if (marker_error != "no session open") {
      BX_LOG_WARN("the timeline game marker was not written: %s", marker_error.c_str());
    }
    timeline_game_exe_ = facts.game_exe;
  }
  // A start or a resume that is refused is asked for again next tick, so it is said once, not four times a
  // second: "waiting for the capture source to produce frames" is the usual one while a game hooks.
  const auto refuse = [this](const std::string& reason) {
    if (reason.empty() || reason == timeline_last_error_) return;
    timeline_last_error_ = reason;
    BX_LOG_INFO("timeline not writing: %s", reason.c_str());
  };
  const auto mark_wake_gap = [this]() {
    if (!timeline_wake_gap_pending_) return;
    std::string session_id, marker_error;
    SessionMarker marker;
    if (add_marker("gap", "Recording paused while the PC slept", session_id, marker, marker_error)) {
      timeline_wake_gap_pending_ = false;
    } else if (!marker_error.empty()) {
      BX_LOG_WARN("the Timeline wake marker was not written: %s", marker_error.c_str());
    }
  };
  if (writing) {
    if (!session_open) {
      std::string error;
      if (!start_timeline(error)) {
        refuse(error);
        return;
      }
      timeline_last_error_.clear();
      timeline_game_exe_ = facts.game_exe;
      timeline_paused_ = false;
      timeline_pause_state_ = TimelineState::Off;
      timeline_last_write_ms_ = now;
      mark_wake_gap();
      return;
    }
    if (timeline_paused_) {
      // The pause's stop is still finishing: the next tick resumes into the session that is still open.
      if (segment_open) return;
      std::string error;
      if (!resume_timeline(error)) {
        refuse(error);
        return;
      }
      timeline_last_error_.clear();
      timeline_paused_ = false;
      // The footage either side of the pause is not continuous, and the preview reads the index: say where
      // the hole is. A glance at another window is under kTimelineGapMs and is not worth a marker, and the
      // marker is labelled so the strip says NO GAME rather than a bare hole.
      const uint32_t gap = timeline_gap_ms(timeline_last_write_ms_, now);
      const char* gap_label =
          timeline_gap_label(gap, kTimelineGapMs, timeline_wake_gap_pending_,
                             timeline_pause_state_ == TimelineState::PausedNoGame,
                             timeline_pause_state_ == TimelineState::PausedIdle);
      timeline_pause_state_ = TimelineState::Off;
      if (*gap_label != '\0') {
        std::string session_id, marker_error;
        SessionMarker marker;
        if (add_marker("gap", gap_label, session_id, marker, marker_error)) {
          BX_LOG_INFO("timeline resumed after a gap of %u ms", gap);
        } else {
          BX_LOG_WARN("the timeline gap marker was not written: %s", marker_error.c_str());
        }
      }
      timeline_last_write_ms_ = now;
      mark_wake_gap();
      return;
    }
    timeline_last_write_ms_ = now;
    mark_wake_gap();
    return;
  }
  if (!session_open) return;
  if (segment_open && !timeline_paused_) {
    // Left Writing: the segment is closed and indexed, the session stays for the pause window.
    timeline_paused_ = true;
    timeline_pause_state_ = state;
    timeline_paused_at_ms_ = now;
    stop_primary_recording();
    return;
  }
  if (segment_open) return;  // the pause's stop is still finishing
  const uint32_t paused_ms = timeline_gap_ms(timeline_paused_at_ms_, now);
  if (timeline_session_should_close(paused_ms)) close_timeline(now);
}

// A paused session writes again: a new segment in the session that is already open. Not start_timeline,
// which would open a second session, and not start_recording_inner, which would reset session_ and begin a
// manual recording.
bool ObsHost::resume_timeline(std::string& error) {
  try {
    if (buffrx::abandoned_outputs_exhausted(abandoned_outputs_)) {
      error = "the recording output stopped responding earlier; restart BuffrX to record again";
      return false;
    }
    if (!ensure_capture_sources(error)) return false;
    if (!capture_source_ready()) {
      error = "waiting for the capture source to produce frames";
      return false;
    }
    std::lock_guard<std::mutex> lock(recording_mutex_);
    if (session_.id.empty()) {
      error = "the timeline session closed before it could resume";
      return false;
    }
    if (recording_ || recording_active_) {
      error = "the timeline is already writing";
      return false;
    }
    if (!start_recording_output(error)) return false;
    // Zero until the first frame lands (stamp_chunk_start sets it): the file cannot begin earlier.
    recording_started_ms_ = 0;
    BX_LOG_INFO("timeline session resumed: %s", session_.id.c_str());
    return true;
  } catch (const std::exception& failure) {
    error = std::string("the timeline could not resume: ") + failure.what();
    BX_LOG_ERROR("%s", error.c_str());
    return false;
  } catch (...) {
    error = "the timeline could not resume: an unknown error";
    BX_LOG_ERROR("%s", error.c_str());
    return false;
  }
}

// Ends the session for good: the last segment is indexed and the session closed, either here (nothing is
// open) or by the tick that sees the stop (a segment was still writing). close_session resets the pause state.
void ObsHost::close_timeline(uint32_t now) {
  bool open = false;
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    open = recording_ != nullptr || recording_active_;
    if (!open) {
      timeline_writer_ = false;
      if (!session_.id.empty()) close_session(now);
      return;
    }
  }
  // Cleared first, so the tick that finishes the stop closes the session instead of keeping it for a pause.
  stop_timeline();
}

// A manual stop, from the panel, the tray, the quick panel or `buffer.stop`: the session ends now. Nothing
// here waits - stopping an output takes its time, and every caller runs under the engine lock, where a sleep
// is a stalled window thread. The segment is stopped here and the tick that sees it close indexes it and
// closes the session (finish_timeline_close from the tick that follows, or process_closed_chunk).
// Needs no lock of its own: recording_mutex_ is taken by finish_timeline_close in its own scopes, never held
// across a call that takes it again (stop_timeline and close_session are the only parts that want it).
void ObsHost::end_timeline() {
  timeline_closing_ = !session_.id.empty();
  if (!timeline_closing_) timeline_writer_ = false;  // a flag with no session behind it is a leftover
  finish_timeline_close(GetTickCount());
}

void ObsHost::finish_timeline_close(uint32_t now) {
  if (!timeline_closing_) return;
  bool segment_open = false;
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    // Only the segment this stop asked for is waited for. timeline_segment_stopping_ covers the tick after the
    // stop: the writer flag is already down while its output flushes its last chunk, and that chunk must reach
    // the index. Any other output in the slot is a manual recording the session does not own.
    segment_open = (timeline_writer_.load() || timeline_segment_stopping_) &&
                   (recording_ != nullptr || recording_active_);
  }
  const TimelineEndAction action = timeline_end_action(!session_.id.empty(), segment_open);
  if (action == TimelineEndAction::none) {
    timeline_closing_ = false;
    timeline_writer_ = false;
    timeline_segment_stopping_ = false;
    return;
  }
  if (action == TimelineEndAction::stop_writer) {
    // Asked once: asking again would restart the stop's grace period on an output that is slow to let go.
    if (timeline_writer_.load()) {
      timeline_segment_stopping_ = true;
      stop_timeline();
    }
    return;
  }
  // A save still reading the session is not waited for: its pin (kTimelineSaveOwner) is what keeps the footage
  // it copies, and the index closes under it. Holding this lock is this function's one long scope, and every
  // other close_session call site (close_timeline, process_closed_chunk, destroy_outputs) holds it too.
  std::lock_guard<std::mutex> lock(recording_mutex_);
  close_session(now);  // clears timeline_closing_ with the rest of the session's state
}

bool ObsHost::start_recording_inner(std::string& error, bool timeline) {
  // A request is already waiting for the capture source: this click is the same request, not a second
  // one. Starting now would be the double start this flag exists to prevent.
  if (!timeline && recording_pending_) {
    error.clear();
    return true;
  }
  // The sources were released while the engine was idle: a record needs them.
  if (!ensure_capture_sources(error)) return false;
  // Past the ceiling of abandoned outputs the encode thread is wedged, not unlucky. Starting another
  // recording here would leave its encoders and GPU surfaces behind too, one per attempt, for as long
  // as the engine runs. Clipping and screenshots are untouched: only the recording output is involved.
  if (buffrx::abandoned_outputs_exhausted(abandoned_outputs_)) {
    error = "the recording output stopped responding earlier; restart BuffrX to record again";
    BX_LOG_WARN("not starting a recording: %llu outputs have already been abandoned",
                static_cast<unsigned long long>(abandoned_outputs_));
    last_error_ = error;
    if (on_error) on_error("recording", error);
    return false;
  }
  // An idle engine released the source, and a fresh one has no frame until the graphics thread has
  // ticked it (a game until it hooks). The buffer waits for the same first frame; this request waits
  // too, and the tick starts it. The old instant failure is why the first record click did nothing.
  if (!capture_source_ready()) {
    if (timeline) {
      // Timeline's start comes from the tick, not a click: it says why and the next tick that wants to
      // write asks again. It must not arm the manual recording's pending start, which would start a
      // recording instead of a timeline session.
      error = "waiting for the capture source to produce frames";
      return false;
    }
    recording_request_ms_ = GetTickCount();
    recording_pending_ = true;
    recording_start_due_ = false;
    BX_LOG_INFO("recording will start when the capture source has a frame");
    error.clear();
    return true;
  }
  // Timeline writes on the ring's encoders, never on its own and never by pausing the buffer: a machine
  // that cannot share them records no timeline rather than taking the ring's place (Task 5 retries).
  if (timeline && !(shared_encoders_ok_ && video_encoder_ && !audio_encoders_.empty())) {
    error = "Timeline records on the buffer's encoders and they are not running";
    return false;
  }
  // Start the recording first; the replay buffer is paused right after, so the click does not wait
  // for the buffer to finish stopping.
  const bool was_buffer = buffer_intent_;
  std::lock_guard<std::mutex> lock(recording_mutex_);
  // The output object lives until the tick has indexed its last chunk, so a start during a stop's
  // flush cannot replace a session that is still closing.
  if (recording_ && !obs_output_active(recording_)) {
    // Finished, but its stop signal came after the engine let go: the tick would release it on the next
    // pass, and a click must not be refused with "still being finished" while it waits.
    release_current_output();
    chunk_closed_ = false;
    stop_requested_ms_ = 0;
    recording_active_ = false;
  } else if (!recording_ && recording_active_) {
    // A flag with no output behind it is the same leftover from another angle: nothing to release, and
    // nothing is recording, so it must not pause the Timeline or answer a click.
    recording_active_ = false;
  }
  if (buffrx::recording_start_blocked(recording_ != nullptr, recording_ && obs_output_active(recording_)) || recording_active_) {
    if (timeline) {
      error = "the timeline is already writing";
      return false;  // and it never started a manual recording, so there is no buffer to restore
    }
    error = recording_active_ ? "a recording is already running"
                              : "the last recording is still being finished; try again in a moment";
    if (restore_buffer_after_recording_) {
      restore_buffer_after_recording_ = false;
      buffer_intent_ = true;
      std::string restore_error;
      start_buffer(restore_error);
    }
    return false;
  }
  session_ = SessionEntry{};
  note_open_timeline("");
  current_chunk_.clear();
  current_chunk_start_ms_ = 0;
  // A manual recording is one file, however long it runs (decided with the user, 2026-10-25): it is no longer cut
  // into chunks. The guard runs before anything is created: a recording that cannot be finished is not started. It is
  // asked on the volume recordings go to (recordings_base_dir, recording_space.h), which is where this file lands.
  std::string space;
  if (!recording_space_guard(config_, volume_free_bytes, space)) {
    error = space;
    last_error_ = space;
    BX_LOG_WARN("recording refused: %s", space.c_str());
    if (on_error) on_error("recording", space);
    if (restore_buffer_after_recording_) {
      restore_buffer_after_recording_ = false;
      buffer_intent_ = true;
      std::string restore_error;
      start_buffer(restore_error);
    }
    return false;
  }
  prune_sessions("");
  if (timeline) {
    // The session is the directory every segment goes into and the index the preview reads. Opened before
    // the first segment is written, because the segment's path is inside it.
    if (!session_store_begin("timeline", session_, error)) return false;
    note_open_timeline(session_.id);
    timeline_writer_ = true;
    // A new session starts with no span open and nothing the last output's splitter may have left queued.
    timeline_chunks_ = TimelineFileIndex{};
    clear_split_queue();
  }
  if (!start_recording_output(error)) {
    if (timeline) {
      // A session that never wrote a segment must not load as interrupted: it is closed with nothing.
      timeline_writer_ = false;
      std::string close_error;
      if (!session_store_close(session_, GetTickCount(), close_error)) {
        BX_LOG_WARN("the empty timeline session was not closed: %s", close_error.c_str());
      }
    }
    session_ = SessionEntry{};
    note_open_timeline("");
    return false;
  }
  // On the ring's own encoders the buffer simply keeps running; only a recording with encoders of its
  // own takes the buffer's place.
  if (!timeline && was_buffer && !recording_shared_) {
    restore_buffer_after_recording_ = true;
    stop_buffer();
  }
  // A start that went through is the newest thing the engine did: a refusal from before it - "the disk is
  // short", "the encoder is missing" - would otherwise sit in the status while a recording runs fine.
  if (!timeline) last_error_.clear();
  // Zero until the first frame lands (stamp_chunk_start sets it), so the clock the panel shows
  // measures the file instead of the wait for its first keyframe.
  recording_started_ms_ = 0;
  if (timeline) BX_LOG_INFO("timeline session started: %s", session_.id.c_str());
  // The mark on the screen goes up with the recording (the engine's own window, not the scene, so it
  // is never burned into the clip). The engine's tick watches the recording state and shows it.
  return true;
}

bool ObsHost::start_pending_recording(std::string& error) {
  if (!recording_pending_) {
    error.clear();
    return true;  // stopped while the start waited in the action queue
  }
  if (!capture_source_ready()) {
    error.clear();
    return true;  // still waiting; the next tick asks again if a frame has arrived
  }
  if (!shared_encoders_ok_ && (timeline_writer_active() || recording_)) {
    if (timeline_writer_active()) close_timeline(GetTickCount());
    error.clear();
    return true;
  }
  recording_pending_ = false;
  recording_start_due_ = false;
  return start_recording(error);
}

bool ObsHost::take_recording_start_due() { return recording_start_due_.exchange(false); }

void ObsHost::stop_recording() {
  // It also cancels a click that is still waiting for the capture source.
  recording_pending_ = false;
  recording_start_due_ = false;
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    if (manual_recording_) {
      manual_stop_requested_ms_ = GetTickCount() == 0 ? 1 : GetTickCount();
      if (obs_output_active(manual_recording_)) obs_output_stop(manual_recording_);
      else manual_closed_ = true;
      manual_active_ = false;
      return;
    }
  }
  // The old, separate-encoder fallback is still a manual recording; Timeline owns this slot otherwise.
  if (!timeline_writer_active()) stop_primary_recording();
}

void ObsHost::stop_primary_recording() {
  std::lock_guard<std::mutex> lock(recording_mutex_);
  if (recording_) {
    // An output libobs no longer calls active will never send "stop": the tick finishes it now.
    if (obs_output_active(recording_)) {
      obs_output_stop(recording_);
    } else {
      chunk_closed_ = true;
    }
    stop_requested_ms_ = GetTickCount();
  }
  recording_active_ = false;
}

int ObsHost::chunk_seconds_now() const {
  // A Timeline segment is asked for in 5 s steps so the index and the preview see footage regularly; the
  // splitter cuts on a keyframe, which is why a segment lands nearer 8 s than 5. A manual recording is one
  // file and never rotates.
  return chunk_seconds_clamped(timeline_writer_.load() ? kTimelineSplitSeconds : config_.chunk_seconds,
                               config_.keyframe_seconds);
}

const char* ObsHost::chunk_container() const { return timeline_writer_.load() ? "mp4" : config_.container.c_str(); }

// A recording's clock and its chunk's start time both begin with the first frame the output writes. A shared
// encoder hands a new output one only at its next keyframe, so a clock started at the click ran up to
// `keyframe_seconds` ahead of the file for the whole recording. A Timeline segment's first file is the same,
// and its index entry is moved with the clock. A manual recording has no session and still needs its clock.
void ObsHost::stamp_chunk_start() {
  std::lock_guard<std::mutex> lock(recording_mutex_);
  if (!recording_active_) return;
  if (!chunk_start_unconfirmed_ || !recording_ || obs_output_get_total_frames(recording_) == 0) return;
  current_chunk_start_ms_ = GetTickCount();
  recording_started_ms_ = current_chunk_start_ms_;
  chunk_start_unconfirmed_ = false;
  if (timeline_writer_.load()) timeline_chunks_.retime_current(current_chunk_start_ms_);
}

Config ObsHost::effective_config() const { return effective_for(config_); }

Config ObsHost::effective_for(const Config& user) const {
  Config config = layered(user);
  // The relief ladder's last two rungs lower the capture rate, never the resolution (encoder_governor.h).
  config.capture_fps = buffrx::relief_capture_fps(config.capture_fps, config.encoder_speed_step);
  if (!codec_override_.empty()) config.codec = codec_override_;
  return config;
}

bool ObsHost::set_capture_follow(bool display, bool reconfigure, std::string& error) {
  if (follow_display_ == display) return true;
  // Only recording the state touches no source, so it needs no gate.
  if (!reconfigure || !initialized_ || deep_idle_) {
    follow_display_ = display;
    return true;
  }
  // A swap is refused, not waited for, while a rebuild holds the gate: the gate is not recursive, so a call from inside a rebuild
  // would deadlock, and the tick (the caller) must never wait. Nothing changes; the caller asks again next tick.
  if (rebuild_gate_.busy()) {
    error = "a rebuild is running; the next tick follows";
    return false;
  }
  RebuildGuard rebuilding(rebuild_gate_);  // the same one-rebuild-at-a-time rule apply() keeps
  follow_display_ = display;
  enum_cache_.windows_at = 0;
  enum_cache_.games_at = 0;
  return configure_capture(config_, error, false, true);
}

bool ObsHost::set_performance_game(bool listed, std::string& error) {
  if (performance_game_ == listed) return true;
  const bool was_active = performance_active();
  performance_game_ = listed;
  if (performance_active() == was_active) return true;  // the user's switch already had it on
  BX_LOG_INFO("Performance mode %s: a listed game %s", listed ? "on" : "off", listed ? "is running" : "left");
  const bool ok = apply(config_, true, error);
  return ok;
}

bool ObsHost::recover_after_sleep(std::string& error) {
  // Nothing inside libobs reports a lost device after a sleep: the outputs keep their old state and the next frame
  // never comes. The pipeline is therefore built from the settings that were running, through the same teardown a
  // video change uses (sources down before obs_reset_video). A sleeping engine has nothing to rebuild.
  if (!initialized_) {
    error = "engine is not initialized";
    return false;
  }
  if (deep_idle_) {
    BX_LOG_INFO("woke from sleep with the pipeline already asleep: nothing to rebuild");
    return true;
  }
  if (buffer_intent_ && config_.timeline_minutes > 0) timeline_wake_gap_pending_ = true;
  force_video_rebuild_ = true;
  const bool ok = apply(config_, true, error);
  force_video_rebuild_ = false;
  return ok;
}

bool ObsHost::apply_governor_step(std::string& error) {
  if (governor_step_ >= buffrx::kMaxReliefStep) return false;
  // The tick only queues a step while nothing records, but the action queue can run it after a Record
  // click in the same pass: rebuilding here would cut that recording in two. The ladder re-arms next tick.
  if (recording_busy()) return false;
  // One step per four seconds is a minute of choppy footage on a setting that is far too heavy, and every step empties the
  // ring. A ratio this bad says where it has to end up, so the ladder jumps: under half the frames goes straight to the
  // frame-rate rungs, under seven tenths takes two.
  const int before = governor_step_;
  const double ratio = encoder_watch_.ratio;
  const int bump = ratio < 0.5 ? 3 : ratio < 0.7 ? 2 : 1;
  // Frame rate is the last resort: the effort rungs (1-3) are used up before a single fps step is taken, and from there
  // the rate falls one step at a time. A very bad ratio may skip preset rungs, never the frame rate.
  governor_step_ = governor_step_ < 3 ? std::min(governor_step_ + bump, 3)
                                      : std::min(governor_step_ + 1, buffrx::kMaxReliefStep);
  force_encoder_rebuild_ = true;
  Config config = config_;
  config.encoder_speed_step = governor_step_;
  BX_LOG_WARN("the encoder could not keep up (%d%% of the frames at %d fps): lowering its speed settings to step %d of %d",
              static_cast<int>(std::lround(ratio * 100.0)), config_.capture_fps, governor_step_,
              buffrx::kMaxReliefStep);
  const bool ok = apply(config, true, error);
  force_encoder_rebuild_ = false;
  // The cooldown starts when the new encoder is running, not when the step was asked for.
  governor_last_step_ms_ = GetTickCount();
  // Said over the game, every time it changes: the person must know why the picture or the rate is not what they chose.
  if (ok && on_notice) {
    const int running_fps = buffrx::relief_capture_fps(config_.capture_fps, governor_step_);
    // Name the change, nothing else: one short line fits the card.
    std::string title;
    std::string detail;
    if (running_fps < config_.capture_fps) {
      title = "Clips at " + std::to_string(running_fps) + " fps";
      detail = "Your GPU cannot encode " + std::to_string(config_.capture_fps) + " fps in real time, so clips are made at " +
               std::to_string(running_fps) + " fps. Resolution is unchanged.";
    } else {
      title = "Encoder effort lowered";
      detail = "Your GPU cannot encode this setting at real time. Resolution unchanged.";
    }
    if (governor_step_ > before) BX_LOG_INFO("relief: step %d -> %d, running %d fps", before, governor_step_, running_fps);
    on_notice(title, detail);
  }
  return ok;
}


// The splitter's word that the session is writing a new file. It runs on the output's own thread, inside
// libobs, where no engine lock may be taken and nothing may block, so the signal only queues the path and the
// tick does the rest here: the span that closed goes into the index queue (pending_chunk_publish writes it
// once its writer has let go of the file) and the open span follows the new file. Caller holds recording_mutex_.
void ObsHost::poll_timeline_split(uint32_t now) {
  std::vector<SplitChange> changes;
  {
    std::lock_guard<std::mutex> lock(split_mutex_);
    changes.swap(split_changes_);
  }
  for (const SplitChange& change : changes) {
    const uint32_t boundary = change.at_ms != 0 ? change.at_ms : now;
    if (!timeline_chunks_.change(change.file, boundary)) {
      BX_LOG_WARN("a timeline file change was not taken: %s", change.file.c_str());
      continue;
    }
    current_chunk_ = change.file;
    current_chunk_start_ms_ = timeline_chunks_.current_start_ms();
    // The new file begins on the frame the splitter cut on, so no first-frame correction is owed for it.
    chunk_start_unconfirmed_ = false;
    library_hold(change.file);
    BX_LOG_INFO("timeline segment file: %s", change.file.c_str());
  }
  for (const TimelineSpan& span : timeline_chunks_.take_closed()) {
    queue_session_chunk(span.file, span.start_ms, span.end_ms);
    // Closed and filed: the library may list it again. Its index entry still waits for the writer.
    library_release(span.file);
  }
}

void ObsHost::clear_split_queue() {
  std::lock_guard<std::mutex> lock(split_mutex_);
  split_changes_.clear();
}

// One closed span into the index queue, with the session's wall clock. Filed, not yet indexed: the entry waits
// until nobody holds the file (pending_chunk_publish_locked), because an index that names a file a writer still
// holds is a cut or a playback that fails. Caller holds recording_mutex_.
void ObsHost::queue_session_chunk(const std::string& file, uint32_t start_ms, uint32_t end_ms) {
  if (file.empty() || session_.id.empty()) return;
  SessionChunk chunk;
  chunk.file = file;
  chunk.start_ms = start_ms;
  chunk.end_ms = end_ms;
  // The session's wall start plus the span's offset on the tick clock; a manual recording never changes
  // encoder, so it stays in epoch 0.
  const int64_t session_wall_ms = session_.started_at * 1000;
  chunk.wall_start_ms = session_wall_ms + static_cast<int64_t>(start_ms - session_.started_ms);
  chunk.wall_end_ms = chunk.wall_start_ms + static_cast<int64_t>(end_ms - start_ms);
  chunk.published = true;
  pending_chunks_.push_back(PendingChunk{session_.id, chunk, GetTickCount()});
}

void ObsHost::reap_output(obs_output* output, const std::string& delete_path, obs_encoder* video_encoder,
                          std::vector<obs_encoder*> audio_encoders) {
  if (!reaper_.push(ReapJob{output, delete_path, video_encoder, audio_encoders})) {
    // The reaper is shutting down: nothing else is running, so waiting here starves nobody.
    obs_output_release(output);
    if (video_encoder) obs_encoder_release(video_encoder);
    for (obs_encoder* encoder : audio_encoders) obs_encoder_release(encoder);
    if (!delete_path.empty()) {
      DeleteFileA(delete_path.c_str());
      library_release(delete_path);
    }
  }
}

void ObsHost::index_open_chunk(uint32_t now) {
  // An output that wrote no frame left a container with nothing in it. It is deleted, not indexed: the
  // library listed it as a zero second clip that no player opens (a double tap on the record key made one).
  // A file the splitter opened after another is never empty, so this only ever matches a segment's first.
  const bool empty = closed_chunk_frames_ == 0 && closed_chunk_path_ == current_chunk_ && !current_chunk_.empty() &&
                     !closed_chunk_path_.empty();
  if (!empty) queue_session_chunk(current_chunk_, current_chunk_start_ms_, now);
  // The chunk is closed and filed, so the library may show it again. An empty one is deleted first, and
  // its hold with it, so the library never lists a file that is gone.
  if (!current_chunk_.empty()) {
    if (empty && DeleteFileA(current_chunk_.c_str())) {
      BX_LOG_INFO("an empty recording chunk was dropped: %s", current_chunk_.c_str());
    }
    library_release(current_chunk_);
  }
  current_chunk_.clear();
}

// The other half of index_open_chunk. A closed chunk goes into the index only once its file is final:
// an entry written while a writer still holds the file names footage that cannot be read yet, and a
// cut or a playback made from it would fail. The file is never deleted on a timeout - what the encoder
// wrote is still the person's footage.
void ObsHost::pending_chunk_publish(uint32_t now) {
  std::lock_guard<std::mutex> lock(recording_mutex_);
  pending_chunk_publish_locked(now);
}

void ObsHost::pending_chunk_publish_locked(uint32_t now) {
  for (std::ptrdiff_t index = 0; index < static_cast<std::ptrdiff_t>(pending_chunks_.size());) {
    const PendingChunk& pending = pending_chunks_[static_cast<size_t>(index)];
    if (file_writer_closed(pending.chunk.file)) {
      std::string error;
      // The session may have closed in the pass that queued the chunk (the last chunk of a recording), and
      // then session_ is empty: the entry goes to the index on disk instead, under the session it belongs to.
      SessionEntry closed;
      SessionEntry* target = &session_;
      if (pending.session_id != session_.id) {
        if (!session_store_load_one(pending.session_id, closed, error)) {
          BX_LOG_WARN("a chunk was not indexed: %s", error.c_str());
          pending_chunks_.erase(pending_chunks_.begin() + index);
          continue;
        }
        target = &closed;
      }
      if (!session_store_append_chunk(*target, pending.chunk, error)) {
        BX_LOG_WARN("a chunk was not indexed: %s", error.c_str());
      } else if (target == &session_) {
        dedupe_note_chunk(session_.id, pending.chunk);
      }
      pending_chunks_.erase(pending_chunks_.begin() + index);
      continue;
    }
    if (now - pending.since_ms >= kPendingPublishTimeoutMs) {
      BX_LOG_ERROR("a chunk stayed open for %u ms and is left out of the index: %s", now - pending.since_ms,
                   pending.chunk.file.c_str());
      pending_chunks_.erase(pending_chunks_.begin() + index);
      continue;
    }
    ++index;
  }
}

void ObsHost::close_session(uint32_t now) {
  recording_active_ = false;
  if (session_.id.empty()) return;
  // A chunk whose file is already final goes into the index now, before the session is measured for
  // retention, so the budget counts the last chunk of the session it just closed.
  pending_chunk_publish_locked(now);
  const std::string session_id = session_.id;
  std::string error;
  if (!session_store_close(session_, now, error)) {
    BX_LOG_WARN("the session index was not closed: %s", error.c_str());
  }
  session_ = SessionEntry{};
  note_open_timeline("");
  // No session is open any more, so the Timeline writer is not active: a failed rotation, a stop the tick
  // finished and a plain stop all end here, and a manual recording must not be refused for ever by one.
  timeline_writer_ = false;
  // A manual stop that was waiting for this close is done with it too.
  timeline_closing_ = false;
  timeline_segment_stopping_ = false;
  // A closed session keeps nothing of the pause: the next one begins where it began.
  timeline_paused_ = false;
  timeline_pause_state_ = TimelineState::Off;
  timeline_paused_at_ms_ = 0;
  timeline_game_exe_.clear();
  BX_LOG_INFO("recording session closed: %s", session_id.c_str());
  // A clips folder moved during the session: the store follows it now that nothing is open.
  const std::string wanted_root = sessions_dir(config_);
  if (session_store_root() != wanted_root) session_store_init(wanted_root);
  prune_sessions("");
  if (on_session_changed) on_session_changed(session_id, true);
}

void ObsHost::process_closed_chunk() {
  // A stop libobs never answered (no "stop" signal within the grace period) is finished anyway, so a
  // session can never stay open behind the user's back.
  const uint32_t asked = stop_requested_ms_;
  const uint32_t grace_ms = buffrx::recording_stop_grace_ms();
  const bool overdue = !chunk_closed_ && asked != 0 && GetTickCount() - asked > grace_ms;
  if (!overdue && !chunk_closed_.exchange(false)) return;
  bool restore_after = false;
  // A fault that should cost the ring nothing: set inside the lock, acted on after it.
  bool salvage = false;
  {
  std::lock_guard<std::mutex> lock(recording_mutex_);
  // A signal from an output the engine has already let go of (a stop it forced, then the output's own
  // answer arriving late) is nothing: handling it would close a recording that is running now.
  if (!recording_ && !overdue) {
    stop_requested_ms_ = 0;
    return;
  }
  // Nobody asked for this stop - not the user, not a chunk boundary - so the output ended by itself: the
  // encoder or the muxer failed, or the disk refused a write. That used to close the session in silence.
  const bool unexpected =
      buffrx::recording_stop_unexpected(recording_ != nullptr, stop_requested_ms_ != 0, overdue);
  stop_requested_ms_ = 0;
  bool resume = false;
  if (unexpected) {
    // A shared encoder that was refused shows up as an output that stops within moments of starting: the
    // recording then gets encoders of its own (the way it always had) rather than failing again. Timeline
    // has none of its own, so one of its segments dying says nothing about sharing the encoders.
    if (recording_shared_ && !timeline_writer_.load() && GetTickCount() - current_chunk_start_ms_ < 3000) {
      BX_LOG_WARN("the recording stopped moments after starting on the ring's encoders; using its own from now on");
      shared_encoders_ok_ = false;
    }
    const long long code = recording_stop_code_.exchange(0);
    const char* reason = recording_ ? obs_output_get_last_error(recording_) : nullptr;
    const std::string detail = std::string("code ") + std::to_string(code) + (reason && *reason ? std::string(": ") + reason : std::string());
    BX_LOG_WARN("the recording stopped by itself (%s)", detail.c_str());
    salvage = true;
    // Whether a session is open or not: a manual recording is one file with no index, and it used to be
    // finished here, so one output failure took the rest of the recording with it. The budget is what
    // keeps an output that fails again and again from being restarted for ever.
    resume = buffrx::recording_resume_allowed(recording_resume_budget_, GetTickCount());
    last_error_ = resume ? "the recording stopped by itself and carries on in a new file (" + detail + ")"
                         : "the recording stopped by itself (" + detail + ")";
    if (on_error) on_error("recording", last_error_);
  }
  if (overdue && recording_) {
    // libobs never finished this stop, and releasing an output whose stop is still pending blocks
    // forever. Ask it to stop once more, then abandon only if that does not land either.
    BX_LOG_WARN("the recording output did not report its stop within %u ms; forcing it", grace_ms);
    const char* last_err = obs_output_get_last_error(recording_);
    if (last_err && *last_err) BX_LOG_WARN("recording output last error: %s", last_err);
    obs_output_force_stop(recording_);
    // Two seconds, not a token wait: this is the one place a wait protects footage. An output that lands
    // here has written its moov and can be indexed; one that does not is unreadable and must not be.
    const uint32_t forced = GetTickCount();
    while (obs_output_active(recording_) && GetTickCount() - forced < 2000) Sleep(20);
    if (obs_output_active(recording_)) {
      BX_LOG_WARN("the recording output did not report its stop after a forced stop; abandoning it");
      signal_handler_disconnect(obs_output_get_signal_handler(recording_), "stop", &ObsHost::on_recording_stopped, this);
      // The chunk writer is fragmented MP4, so the prefix it wrote is playable even without its final moov:
      // the file stays, and a restart's recovery indexes it into the session. It is not indexed now, while
      // its writer may still hold it. The release waits on a thread of its own; the window thread must not.
      const std::string abandoned = current_chunk_;
      obs_output* output = recording_;
      recording_ = nullptr;
      current_chunk_.clear();
      // The abandoned output still uses its own encoders until it lets go of them: the reaper releases ours after the
      // output. Dropping these here leaked the NVENC or x264 session for the rest of the process.
      obs_encoder* abandoned_video = recording_video_encoder_;
      std::vector<obs_encoder*> abandoned_audio;
      abandoned_audio.swap(recording_audio_encoders_);
      recording_video_encoder_ = nullptr;
      ++abandoned_outputs_;
      // Its splitter's queue goes with it: the prefix that was written is left for recovery, not indexed now.
      timeline_chunks_ = TimelineFileIndex{};
      clear_split_queue();
      reap_output(output, std::string(), abandoned_video, std::move(abandoned_audio));
      BX_LOG_WARN("the abandoned chunk is kept for recovery: %s", abandoned.c_str());
      last_error_ = "the recording output stopped responding; the recording was closed with what was written";
      if (on_error) on_error("recording", last_error_);
    } else {
      // Forced down: let go of it now, so its own late answer finds nothing to act on and a new recording
      // can start. The file is closed with what the encoder had delivered.
      release_current_output();
      chunk_closed_ = false;
    }
  } else {
    // Outside libobs's signal now, so the finished output and its encoders can be released safely.
    release_current_output();
  }

  const uint32_t now = GetTickCount();
  const bool has_session = !session_.id.empty();
  // A file the splitter opened between the last tick and this stop: that one is what was open, and the file
  // before it is a closed span. Taken before the last span is indexed, so the index ends at the real file.
  poll_timeline_split(now);
  index_open_chunk(now);
  // The segment's file index ends with its output: a resume begins a new first span.
  timeline_chunks_ = TimelineFileIndex{};

  std::string error;
  const bool continuing = resume;
  // chunk_closed_action's rule is written per session, and a manual recording has none: a resume there is
  // still "open the next file on the running encoder", never "finish".
  const ChunkClosedAction action =
      (!has_session && resume) ? ChunkClosedAction::rotate : chunk_closed_action(has_session, continuing);
  bool keep_session = false;
  switch (action) {
    case ChunkClosedAction::finish_single:
      recording_active_ = false;
      restore_after = restore_buffer_after_recording_;
      break;
    case ChunkClosedAction::rotate:
      if (start_recording_output(error)) {
        if (has_session && on_session_changed) on_session_changed(session_.id, false);
        return;
      }
      BX_LOG_WARN("the next chunk could not start: %s", error.c_str());
      last_error_ = error;
      if (on_error) on_error("recording", error);
      break;  // the session closes with the chunks it has
    case ChunkClosedAction::close_session:
      // A Timeline pause closes the segment, not the session: the writer may resume into it until
      // timeline_session_should_close says otherwise. A real stop has already cleared timeline_writer_.
      keep_session = timeline_paused_ && timeline_writer_.load();
      break;
  }
  if (keep_session) {
    recording_active_ = false;
  } else {
    close_session(now);
    // A manual recording has no session, so close_session returns before it tells anyone. The output is released
    // above and the file is closed: say so now, or the library waits for the refresh button.
    if (action == ChunkClosedAction::finish_single && !has_session && on_session_changed) {
      on_session_changed(std::string(), false);
    }
  }
  restore_after = restore_buffer_after_recording_;
  }
// Before the restore below: what the ring holds now is about to be replaced by a fresh start.
if (salvage) salvage_buffer("the recording stopped by itself");
if (restore_after) {
  restore_buffer_after_recording_ = false;
  buffer_intent_ = true;
  std::string restore_error;
  start_buffer(restore_error);
}
}

void ObsHost::process_manual_recording() {
  std::unique_lock<std::mutex> lock(recording_mutex_);
  if (!manual_recording_) return;
  if (manual_active_ && manual_start_unconfirmed_ && obs_output_get_total_frames(manual_recording_) > 0) {
    manual_started_ms_ = GetTickCount();
    manual_start_unconfirmed_ = false;
  }
  const uint32_t requested = manual_stop_requested_ms_;
  const bool overdue = requested != 0 && !manual_closed_ &&
                       GetTickCount() - requested > buffrx::recording_stop_grace_ms();
  if (!overdue && !manual_closed_.exchange(false)) return;

  const bool unexpected = requested == 0 && !overdue;
  if (unexpected) {
    if (GetTickCount() - manual_opened_ms_ < 3000) {
      BX_LOG_WARN("the recording stopped moments after starting on shared encoders; using its own from now on");
      shared_encoders_ok_ = false;
    }
    const long long code = manual_stop_code_.exchange(0);
    const char* reason = obs_output_get_last_error(manual_recording_);
    const std::string detail = std::string("code ") + std::to_string(code) +
                               (reason && *reason ? std::string(": ") + reason : std::string());
    const bool resume = buffrx::recording_resume_allowed(manual_resume_budget_, GetTickCount());
    last_error_ = resume ? "the recording stopped by itself and carries on in a new file (" + detail + ")"
                         : "the recording stopped by itself (" + detail + ")";
    BX_LOG_WARN("%s", last_error_.c_str());
    if (on_error) on_error("recording", last_error_);
    release_manual_recording();
    if (resume) {
      recording_request_ms_ = GetTickCount();
      recording_pending_ = true;
      recording_start_due_ = true;
    }
    lock.unlock();
    salvage_buffer("the recording stopped by itself");
    return;
  }

  if (overdue) {
    BX_LOG_WARN("the manual recording did not finish its stop; forcing it");
    obs_output_force_stop(manual_recording_);
    const uint32_t forced = GetTickCount();
    while (obs_output_active(manual_recording_) && GetTickCount() - forced < 2000) Sleep(20);
    if (obs_output_active(manual_recording_)) {
      signal_handler_disconnect(obs_output_get_signal_handler(manual_recording_), "stop", &ObsHost::on_manual_recording_stopped, this);
      obs_output* output = manual_recording_;
      manual_recording_ = nullptr;
      // Keep the unfinished file held until restart. Its fragmented prefix remains available for recovery.
      BX_LOG_WARN("the abandoned recording is kept for recovery: %s", manual_file_.c_str());
      manual_file_.clear();
      manual_active_ = false;
      manual_closed_ = false;
      manual_stop_requested_ms_ = 0;
      ++abandoned_outputs_;
      reap_output(output, std::string());
      last_error_ = "the recording output stopped responding; the recording was closed with what was written";
      if (on_error) on_error("recording", last_error_);
      return;
    }
  }
  release_manual_recording();
}

bool ObsHost::take_screenshot(const std::string& format, std::string& path_out, std::string& error,
                              const std::string& game_title) {
  // The sources may have been released while the engine was idle: a screenshot needs them back.
  if (!ensure_capture_sources(error)) return false;
  // The frame that exists now; mid-swap, the source still delivering is the one with it. Nothing waits
  // here: a screenshot is a command under the engine lock, and a first frame can be a second away.
  // write_screenshot reports "the capture source has no frame yet" when there is none.
  obs_source_t* source = delivering_source();
  if (!source) {
    error = "no capture source";
    return false;
  }
  const bool jpeg = format == "jpg" || format == "jpeg";
  const std::string extension = jpeg ? ".jpg" : ".png";
  // Prefer the detected foreground game, even when its picture is coming from display capture.
  // Keep the received-files folder distinct.
  std::string folder = game_title.empty() ? capture_folder() : media_folder_name(game_title);
  std::string lower_folder = folder;
  std::transform(lower_folder.begin(), lower_folder.end(), lower_folder.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower_folder == "shared with me") folder += " (game)";
  // A second screenshot inside the same second gets " (1)".
  path_out = reserve_output_path(config_.screenshots_dir + "\\" + folder, timestamped_stem_now("Screenshot"), extension);
  if (!write_screenshot(source, path_out, jpeg, error)) return false;
  if (on_screenshot_saved) on_screenshot_saved(path_out);
  return true;
}

void ObsHost::process_save_queue() {
  if (save_in_flight_) {
    // The buffer went down under a save and never answered: waiting out the full timeout would refuse every
    // clip for a minute, including the ones after the buffer is started again. A salvage that was started
    // without a packet is the exception: its buffer is stopped on purpose while the ring writes itself out,
    // and a long ring takes longer than five seconds to write.
    const uint32_t down_limit_ms = in_flight_.now ? kSaveTimeoutMs : 5000;
    if ((!replay_ || !obs_output_active(replay_)) && GetTickCount() - in_flight_since_ > down_limit_ms) {
      save_in_flight_ = false;
      save_timeout_reported_ = false;
      last_error_ = "the buffer stopped before the clip was saved";
      BX_LOG_WARN("%s", last_error_.c_str());
      if (on_error) on_error("save", last_error_);
      return;
    }
    // One save at a time: the file the output reports next must belong to this request's length. A save
    // that passes the timeout is NOT dropped: it stays outstanding, so a save slower than a minute still
    // reports its own file to its own request instead of the next one being trimmed to it. The timeout is
    // reported once, not every tick.
    if (!save_timeout_reported_ && GetTickCount() - in_flight_since_ > kSaveTimeoutMs) {
      save_timeout_reported_ = true;
      last_error_ = "the clip save did not finish";
      BX_LOG_ERROR("replay save did not report a file within %u ms; it is still waited for", kSaveTimeoutMs);
      if (on_error) on_error("save", last_error_);
    }
    return;
  }
  if (save_queue_.empty()) return;
  if (!replay_ || !obs_output_active(replay_)) {
    if (GetTickCount() - save_queue_.front().queued_at > 5000) {
      last_error_ = "the buffer stopped before the clip could be saved";
      if (on_error) on_error("buffer", last_error_);
      save_queue_.clear();
    }
    return;
  }
  // One save a second, so a burst of presses cannot stack muxers over one buffer. It is not a naming rule: a save
  // claims its own free name (ring_save.h), so saves inside the same second land side by side.
  if (GetTickCount() - last_save_tick_ < 1100) return;
  SaveRequest request = save_queue_.front();
  save_queue_.erase(save_queue_.begin());
  last_save_tick_ = GetTickCount();

  proc_handler_t* handler = obs_output_get_proc_handler(replay_);
  if (!handler) {
    last_error_ = "the replay buffer has no procedure handler";
    return;
  }
  // OBS's own frontend calls "save" with an empty calldata: the output builds the file name from its
  // configured path/format/extension. Passing a path here is what a third-party host is tempted to
  // do, and it is not part of the documented procedure.
  // The procedure takes no length: it writes the whole ring, and process_saved_replay trims the file
  // to the request afterwards.
  calldata_t data{};
  calldata_init(&data);
  // The length is part of the ask: the ring cuts the clip at a keyframe before it writes, so the file is already the
  // length chosen and does not have to be copied again afterwards. A ring without the procedure writes the whole ring,
  // and finish_save trims it as before.
  calldata_set_int(&data, "seconds", request.seconds);
  // A salvage from a capture that froze: no packet will come to start the save on, so the ring starts it at once.
  calldata_set_int(&data, "now", request.now ? 1 : 0);
  bool asked = proc_handler_call(handler, "save_seconds", &data);
  if (!asked) {
    calldata_free(&data);
    calldata_init(&data);
    asked = proc_handler_call(handler, "save", &data);
  }
  calldata_free(&data);
  if (!asked) {
    // The output refused the save outright. Treating it as one in flight is what left the next minute
    // of clips failing with "the clip save did not finish" and no file: the request is reported now.
    last_error_ = "the replay buffer would not save a clip";
    BX_LOG_ERROR("replay save was refused by the buffer's own save procedure");
    if (on_error) on_error("save", last_error_);
    return;
  }
  save_in_flight_ = true;
  save_timeout_reported_ = false;
  in_flight_ = request;
  in_flight_since_ = GetTickCount();
}

void ObsHost::process_saved_replay() {
  if (!replay_saved_.exchange(false)) return;
  if (!replay_) return;
  // "saved" carries no path and no verdict, and it is raised on the output's own thread: both questions are asked
  // here, from the tick, after the signal. The ring writes its verdict before it signals, so what is read is this
  // save's result and never an older one's (ring_output.cpp).
  SavedReplayVerdict verdict;
  if (proc_handler_t* handler = obs_output_get_proc_handler(replay_)) {
    calldata_t data{};
    calldata_init(&data);
    if (proc_handler_call(handler, "save_result", &data)) {
      verdict.known = true;
      bool flag = false;
      if (calldata_get_bool(&data, "finished", &flag)) verdict.finished = flag;
      flag = false;
      if (calldata_get_bool(&data, "ok", &flag)) verdict.ok = flag;
      flag = false;
      if (calldata_get_bool(&data, "partial", &flag)) verdict.partial = flag;
      flag = false;
      if (calldata_get_bool(&data, "unreadable", &flag)) verdict.unreadable = flag;
      long long number = 0;
      if (calldata_get_int(&data, "skipped", &number)) verdict.skipped = number;
      number = 0;
      if (calldata_get_int(&data, "muxer_exit", &number)) verdict.muxer_exit = static_cast<int>(number);
      const char* written = nullptr;
      if (calldata_get_string(&data, "path", &written) && written) verdict.path = written;
    }
    calldata_free(&data);
  }
  const SavedReplayOutcome outcome = saved_replay_outcome(verdict);
  // The verdict's path is the one this save wrote, including a fallback's own destination. The older route is used
  // only by an output that offers no verdict (OBS's own replay buffer), exactly as it was before.
  std::string path = verdict.path;
  if (outcome == SavedReplayOutcome::unknown) {
    if (proc_handler_t* handler = obs_output_get_proc_handler(replay_)) {
      calldata_t data{};
      calldata_init(&data);
      if (proc_handler_call(handler, "get_last_replay", &data)) {
        const char* last = calldata_string(&data, "path");
        if (last) path = last;
      }
      calldata_free(&data);
    }
  }
  SaveRequest request = save_in_flight_ ? in_flight_ : SaveRequest{};
  save_in_flight_ = false;
  save_timeout_reported_ = false;
  if (outcome == SavedReplayOutcome::failed) {
    // A muxer that failed leaves bytes behind, and those bytes are not a clip: nothing is held from the library,
    // trimmed, moved or announced as saved, and the partial file is left where it is rather than deleted. The
    // failure itself is reported, with its exit code, so the log names what went wrong.
    if (path.empty()) {
      BX_LOG_ERROR("the replay ring's save produced no file");
    } else {
      BX_LOG_ERROR("the replay ring's save of %s produced no clip (muxer exit %d); the partial file is kept",
                   path.c_str(), verdict.muxer_exit);
    }
    last_error_ = "the clip was not saved";
    if (on_error) on_error("save", last_error_);
    return;
  }
  if (path.empty()) {
    last_error_ = "the clip was saved but its file could not be found";
    BX_LOG_ERROR("replay saved but %s returned no path", verdict.known ? "save_result" : "get_last_replay");
    if (on_error) on_error("save", last_error_);
    return;
  }
  if (verdict.known) {
    const SavedReplayGap gap = saved_replay_gap(verdict);
    if (gap != SavedReplayGap::none) {
      // The verdict carries a count, not a cause (saved_replay.h): only an unreadable disk part is a disk read, a
      // skipped count without it came from a part the writer could not join or a chunk file that was gone, and a
      // clip that is only part of the buffer has no reason in the verdict at all. The clip is kept either way.
      const SavedReplayGapText text = saved_replay_gap_text(gap);
      if (gap == SavedReplayGap::part_only) {
        BX_LOG_ERROR("the clip %s: %s", path.c_str(), text.log_reason);
      } else {
        BX_LOG_ERROR("the clip %s skips %lld packets: %s", path.c_str(), static_cast<long long>(verdict.skipped),
                     text.log_reason);
      }
      if (on_notice) on_notice(text.title, text.detail);
    }
  } else {
    // An output with no verdict procedure: the ring's own reported skip count is all there is, and it was the rule.
    const RingInfo ring_now = ring_info();
    if (ring_now.ok && ring_now.skipped > 0) {
      BX_LOG_ERROR("the clip %s skips %lld packets: part of the replay ring's file could not be read", path.c_str(), static_cast<long long>(ring_now.skipped));
      if (on_notice) on_notice("Clip has a gap", "Part of the buffer could not be read back from the disk, so the clip skips a few seconds");
    }
  }
  // Until it is trimmed and filed, the library neither lists nor opens it (library_hold).
  std::string held = path;
  std::replace(held.begin(), held.end(), '/', '\\');
  library_hold(held);
  // Off the tick, and the tick never waits for it: a trim reads and writes the whole clip, and the
  // worker below takes one at a time. Joining here (as this used to) froze the tick — and so the
  // watchdog, the status broadcast and every hotkey — for the length of the previous trim.
  PendingSave pending{path, request, held};
  pending.peak_kbps = peak_kbps_for(config_, canvas_width_, canvas_height_);
  if (!save_worker_.push(std::move(pending))) {
    // The queue is full or shutting down. Trimming here would run a whole remux on the tick, under the
    // engine lock, freezing the panel, the tray, the hotkeys and the watchdog for its length. The clip
    // stays in the clips root, untrimmed and unfiled, and the library lists it: nothing is lost, and the
    // person is told why it is where it is.
    library_release(held);
    BX_LOG_WARN("the trim queue is full; the clip stays in the clips root, untrimmed and unfiled: %s", path.c_str());
    last_error_ = "the clip was saved but could not be filed because the save queue was full";
    if (on_error) on_error("save", last_error_);
    // A salvage's card is not the person's: its bytes are reachable in the clips folder, so say that much and no
    // more. "Buffer saved" waits for the clip to verify and filed (finish_save).
    const SalvageNotice notice = salvage_notice(request.salvage, SalvageResult::kept_unfiled);
    if (notice.title) {
      BX_LOG_WARN("the buffer saved after %s is in the clips folder, unfiled: %s", request.salvage_reason.c_str(),
                  path.c_str());
      if (on_notice) on_notice(notice.title, notice.detail);
    }
  }
}

std::string ObsHost::capture_folder() const {
  const std::string mode = capture_mode_now();
  if (mode == "game") {
    if (config_.game_title.empty()) return kGameFolder;
    return media_folder_name(decode_window_part(config_.game_title.substr(0, config_.game_title.find(':'))));
  }
  if (mode == "window") {
    const std::string title = config_.window_name.empty()
                                  ? decode_window_part(config_.window_id.substr(0, config_.window_id.find(':')))
                                  : config_.window_name;
    return media_folder_name(title);
  }
  return kDisplayFolder;
}

void ObsHost::finish_save(const std::string& saved_path, const SaveRequest& request, int peak_kbps) {
  // The trim, the verify and the move live in save_filing.cpp, where every branch is exercised without
  // libobs or a decoder. What stays here is what belongs to the host: the counters and who to tell.
  SaveFilingMedia media;
  media.probe = [](const std::string& path) { return media_probe(path); };
  media.trim = [](const std::string& path, const std::string& output_path, double start, double length,
                  std::string& error) { return media_trim(path, output_path, start, length, error); };
  media.verify = [](const std::string& path) { return check_clip_file(path); };
  const SaveFiling filed = file_saved_clip(saved_path, request.seconds, request.whole_buffer,
                                           config_.keyframe_seconds, request.folder, request.stem, media);
  if (!filed.error.empty()) {
    // A clip that did not verify is not footage the person can open, so a salvage says nothing more here: the
    // failure is reported below, and it is the whole report.
    if (request.salvage) {
      BX_LOG_ERROR("the buffer saved after %s is not a clip and is not announced: %s", request.salvage_reason.c_str(),
                   filed.error.c_str());
    }
    if (on_error) on_error("save", filed.error);
    return;
  }
  ++saved_clips_;
  last_clip_seconds_ = request.seconds;
  // A clip whose file carries a picture's worth of almost nothing is black: the display source said it was 2560x1440
  // and delivered no frames, and six minutes of it were saved without a word. This worker thread is off the tick and
  // outside the media lock (the probe took it and let it go), so the notice costs the engine nothing. The line is
  // written either way, so the next clip that looks wrong has a rate to read.
  const bool black = bx::clip_may_be_black(static_cast<uint64_t>(std::max(0, filed.video_kbps)), peak_kbps);
  if (black) {
    BX_LOG_WARN("the clip %s is %d kbps at a %d kbps peak: it is probably black", filed.path.c_str(), filed.video_kbps,
                peak_kbps);
  } else {
    BX_LOG_NOTE("the clip %s is %d kbps at a %d kbps peak", filed.path.c_str(), filed.video_kbps, peak_kbps);
  }
  if (black && on_notice) on_notice("That clip looks black", "The display capture may have stopped delivering frames");
  if (on_clip_saved) on_clip_saved(filed.path);
  // The salvage's own card, and only now: the clip has verified and reached the library, so "Buffer saved" is a
  // fact rather than a request that was merely queued. A clip that verified but could not be moved stays in the
  // clips folder, and the card says that instead.
  const SalvageNotice notice =
      salvage_notice(request.salvage, filed.filed ? SalvageResult::filed : SalvageResult::kept_unfiled);
  if (notice.title) {
    BX_LOG_WARN("saved the buffer after %s (%d s of ring)", request.salvage_reason.c_str(),
                request.salvage_ring_seconds);
    if (on_notice) on_notice(notice.title, notice.detail);
  }
}

namespace {

// Probe only (BUFFRX_TEST_FREEZE_VIDEO=<after ms>,<for ms>): libobs's graphics thread is suspended for a while, which
// is what a capture that froze looks like from here - no frame, so no packet, so a frame counter that stands still -
// and it cannot be produced any other way on demand. On a thread of its own that only sleeps between the suspend
// and the resume: the suspended thread may hold a lock everything else needs, and the resume must not wait for it.
void start_test_freeze() {
#ifdef _WIN32
  char text[64] = {0};
  if (GetEnvironmentVariableA("BUFFRX_TEST_FREEZE_VIDEO", text, sizeof(text)) == 0) return;
  char* end = nullptr;
  const DWORD after_ms = std::strtoul(text, &end, 10);
  const DWORD for_ms = end && *end == ',' ? std::strtoul(end + 1, nullptr, 10) : 0;
  if (after_ms == 0 || for_ms == 0) return;
  std::thread([after_ms, for_ms] {
    Sleep(after_ms);
    using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto describe = reinterpret_cast<Describe>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription")));
    HANDLE graphics = nullptr;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (describe && snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
      do {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        PWSTR name = nullptr;
        const bool match = SUCCEEDED(describe(thread, &name)) && name && wcsstr(name, L"graphics") != nullptr;
        if (name) LocalFree(name);
        if (match && !graphics) graphics = thread;
        else CloseHandle(thread);
      } while (Thread32Next(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    if (!graphics) {
      BX_LOG_WARN("test freeze: libobs's graphics thread was not found");
      return;
    }
    BX_LOG_WARN("test freeze: the graphics thread is suspended for %lu ms", for_ms);
    SuspendThread(graphics);
    Sleep(for_ms);
    ResumeThread(graphics);
    CloseHandle(graphics);
    BX_LOG_WARN("test freeze: the graphics thread runs again");
  }).detach();
#else
  // A probe of the Windows thread-suspension path; there is no equivalent Linux need for it.
#endif
}

}  // namespace

void ObsHost::watchdog() {
  if (!replay_) return;
  const bool active = obs_output_active(replay_) != 0;
  // Stall detection first: a live ring always advances its frame counter, and a ring that is on its
  // way down stops advancing too. Counting ticks while the user is stopping would restart the buffer
  // they just stopped, so the count only runs for a buffer that is meant to be running.
  if (replay_ && buffer_active_ && buffer_intent_) {
    const uint64_t frames = obs_output_get_total_frames(replay_);
    if (frames == last_tick_frames_ && frames > 0) {
      ++stall_ticks_;
    } else {
      stall_ticks_ = 0;
    }
  } else {
    stall_ticks_ = 0;
  }

  // A game capture is on a shorter leash: its hook can hold the game's own present while it waits for us to take a
  // frame, so a stall that lasts seconds there is the game freezing, not just the buffer. Two and a half seconds is
  // long enough to tell a stall from a loading screen and short enough that the game is not left hanging.
  const bool game_hook = capture_mode_now() == "game" && !follow_display_;
  const buffrx::WatchdogAction action = buffrx::watchdog_action(buffer_intent_, active, stall_ticks_ > (game_hook ? 6 : 12));
  if (action == buffrx::WatchdogAction::stop) {
    // The user asked for no buffer and the output has not finished unwinding. Finishing the stop is
    // the whole job; calling it running is what made the stall detector restart it a moment later.
    // The stop is issued once, and forced only when libobs has not finished it within the grace.
    const uint32_t now = GetTickCount();
    if (!buffer_stopping_) {
      obs_output_stop(replay_);
      buffer_stopping_ = true;
      buffer_stop_issued_ms_ = now;
    } else if (now - buffer_stop_issued_ms_ > kBufferStopGraceMs) {
      BX_LOG_WARN("replay buffer stop not finished after %u ms; forcing it", kBufferStopGraceMs);
      obs_output_force_stop(replay_);
      buffer_stop_issued_ms_ = now;
    }
    buffer_active_ = false;
    return;
  }
  if (action == buffrx::WatchdogAction::start) {
    // Either the capture source was not ready when the buffer was asked for, or the output died.
    // Recovering on its own is the difference between a tool a player trusts and one that quietly
    // stops working. A source still warming up is retried at the tick's pace, so the buffer follows
    // the fallback source by half a second instead of two; anything else keeps the slower retry.
    const uint32_t retry_ms = capture_source_ready() ? 2000 : 500;
    if (GetTickCount() - last_restart_tick_ > retry_ms) {
      last_restart_tick_ = GetTickCount();
      const bool was_running = buffer_active_;
      const bool after_stall = stall_reported_;
      buffer_active_ = false;
      std::string error;
      if (start_buffer(error)) {
        stall_reported_ = false;
        if (after_stall) {
          ++restarts_;
          BX_LOG_WARN("replay buffer restarted after the capture froze");
          if (on_notice) on_notice("Buffer restarted", "The capture froze; BuffrX restarted it");
        } else if (was_running) {
          ++restarts_;
          BX_LOG_WARN("replay buffer restarted automatically");
          if (on_notice) on_notice("Buffer restarted", "It stopped on its own and BuffrX started it again");
        } else {
          BX_LOG_INFO("replay buffer started once the capture source was ready");
        }
      } else {
        replay_faulted_ = true;
        last_error_ = error;
        if (was_running && on_error) on_error("buffer", error);
      }
    }
    return;
  }
  if (action == buffrx::WatchdogAction::restart) {
    // A restart empties the ring, so a frozen counter that keeps recurring is not allowed to wipe the
    // buffer every three seconds.
    if (GetTickCount() - last_restart_tick_ < kStallRestartCooldownMs) return;
    last_restart_tick_ = GetTickCount();
    stall_ticks_ = 0;
    last_tick_frames_ = 0;
    BX_LOG_WARN("replay buffer stalled at %llu frames; restarting", static_cast<unsigned long long>(obs_output_get_total_frames(replay_)));
    if (game_hook) {
      // Under a game hook the stall is the game waiting on us. Waiting for a salvage and restarting the encoder
      // would keep the hook attached and the game waiting; letting go is the one thing that matters, so the
      // buffer stops here and the person starts it again. The ring is still asked to write itself out, on its
      // own thread and without a packet to start on: nothing here waits for it, and the footage used to be lost.
      BX_LOG_ERROR("game capture stalled: stopping the buffer so the game is released");
      salvage_buffer("the game capture froze", false, true);
      if (on_notice) on_notice("Buffer stopped", "Game released so it stays responsive. Start the buffer again.");
      stop_buffer();
      return;
    }
    // Before the stop below: the restart throws the ring away, and this is the last chance to keep it.
    salvage_buffer("the capture froze", true);
    stop_buffer();
    // stop_buffer cleared the intent, and this stop is not the person's: it is put back. Without it a stop
    // that had not landed - and a frozen ring only lets go on its next packet, so it never has by now - left
    // the buffer off for good, with a log line that said "restarting" and no card.
    buffer_intent_ = true;
    // Without waiting, start_buffer found the output still unwinding, called it running, and the
    // "restart" restarted nothing. It is not waited on here either: waiting holds the engine lock the
    // whole window thread needs. A stop that has not landed leaves the intent set, and the start action
    // finishes the job on the tick that sees the output down.
    if (!force_replay_stopped()) {
      if (!stall_reported_) {
        stall_reported_ = true;
        BX_LOG_WARN("the frozen replay buffer has not let go yet; it starts again when the capture answers");
        if (on_notice) on_notice("Buffer paused", "The capture froze. The buffer starts again when it answers.");
      }
      return;
    }
    stall_reported_ = false;
    std::string restart_error;
    if (start_buffer(restart_error)) {
      ++restarts_;
      if (on_notice) on_notice("Buffer restarted", "The capture froze; BuffrX restarted it");
    } else {
      replay_faulted_ = true;
      last_error_ = restart_error;
      if (on_error) on_error("buffer", restart_error);
    }
    return;
  }
  if (active && buffer_stopping_) {
    // A stop of the engine's own has not landed yet (a stall restart, or a start asked for during a stop): the
    // ring lets go on its next packet. It is not running, so it is not called running - that re-armed the stall
    // detector on an output already being stopped - and the start action takes over the tick it is down.
    buffer_active_ = false;
    return;
  }
  buffer_active_ = active;
  if (!active) buffer_stopping_ = false;
  replay_faulted_ = false;
  if (buffer_active_ && !capture_source_ready()) {
    last_error_ = "the capture source is not producing frames";
  } else if (buffer_active_ && last_error_ == "the capture source is not producing frames") {
    last_error_.clear();
  }
}

void ObsHost::poll_recording() {
  const uint32_t waited = GetTickCount() - recording_request_ms_.load();
  const buffrx::RecordingAction action =
      buffrx::recording_action(recording_pending_.load(), capture_source_ready(), waited);
  if (action == buffrx::RecordingAction::none) {
    recording_start_due_ = false;
    return;
  }
  if (action == buffrx::RecordingAction::start) {
    // The tick cannot create an encoder: main.cpp takes this and queues the action worker.
    recording_start_due_ = true;
    return;
  }
  if (action == buffrx::RecordingAction::give_up) {
    recording_pending_ = false;
    recording_start_due_ = false;
    last_error_ = "the capture source produced no frames; the recording did not start";
    BX_LOG_WARN("%s", last_error_.c_str());
    if (on_error) on_error("recording", last_error_);
  }
}

// --- Salvage on a fault (recovery_policy.h) ------------------------------------------------------------------
//
// Nothing is written while the capture works. The ring is dumped once, at the moment something goes wrong, through
// the ordinary clip path: same trim, same container check, same folder, same library listing. It is therefore a
// normal clip and needs no recovery surface of its own - and no second encoder, no session, no disk traffic in the
// hours when nothing is wrong. Called before the restart that would throw the ring away, and only by a caller that
// has seen a fault.
void ObsHost::salvage_buffer(const char* reason, bool wait_for_file, bool at_once) {
  const uint32_t now = GetTickCount();
  // The ring only holds the past while it runs, and a save needs the output that holds it. A paused ring (a
  // recording took the encoders) has nothing to dump, and saying so would be a warning about nothing.
  if (!replay_ || !obs_output_active(replay_) || buffer_stopping_ || !buffer_intent_) return;
  const uint32_t buffer_ms = static_cast<uint32_t>(stats().ring_filled * 1000.0);
  if (!buffrx::salvage_wanted(config_.crash_recovery, true, buffer_ms, now - last_salvage_ms_)) return;
  last_salvage_ms_ = now;
  std::string error;
  const int code = save_clip(config_.buffer_seconds, error);
  if (code != 0) {
    BX_LOG_WARN("the buffer could not be saved after %s: %s", reason, error.c_str());
    return;
  }
  // Asked for, not saved: the request carries the fault, so its card is drawn where the save really ends - a clip
  // verified and filed (finish_save), or its bytes kept in the clips folder when filing was refused
  // (process_saved_replay). This line says only that the save was asked for.
  {
    SaveRequest& salvage = save_queue_.back();
    salvage.salvage = true;
    salvage.salvage_reason = reason;
    salvage.salvage_ring_seconds = static_cast<int>(buffer_ms / 1000);
    // The capture froze, so the ring receives no packet, and a save normally starts on the next packet: this one
    // would have waited for a frame that never comes and the restart below it would have emptied the ring unsaved.
    if (wait_for_file || at_once) salvage.now = true;
  }
  BX_LOG_WARN("asking the buffer to save itself after %s (%u s of ring)", reason, buffer_ms / 1000);
  if (!wait_for_file && !at_once) return;
  if (!wait_for_file) {
    // Asked for before the caller stops the buffer (a stopped output takes no ask), and not waited for.
    process_save_queue();
    return;
  }
  // The caller is about to stop the output that is writing this file, so the file has to exist first: waiting is
  // the whole point, and it is one moment in a run that has already gone wrong. The trim and the container check
  // run on the save worker afterwards, exactly as they do for a clip the person asked for. The queue is asked
  // every pass: a save from the second before holds the next one back for 1.1 s, and one call would give up on it.
  // Waiting ends when THIS request has left the pipeline - its save answered - and not when any save has answered,
  // because a clip the person asked for a moment earlier is ahead of it in the queue.
  const uint32_t began = GetTickCount();  // elapsed, not a deadline: the tick clock wraps every 49.7 days
  while (save_in_flight_ || !save_queue_.empty()) {
    if (replay_saved_ && replay_) process_saved_replay();
    if (GetTickCount() - began >= 5000) break;
    process_save_queue();
    Sleep(20);
  }
  if (replay_saved_) process_saved_replay();
}

void ObsHost::tick() {
  if (!initialized_) return;
  if (memory_watch_.observe(system_free_memory_mb(), buffer_active_, GetTickCount())) {
    BX_LOG_WARN("memory is nearly full while the buffer runs (%lld MB free)", static_cast<long long>(system_free_memory_mb()));
    // A machine that runs out while buffering does not fail politely: libobs faults inside the graphics device.
    // The start says so in a box; a buffer that meets it later says so on a card.
    if (on_notice) on_notice("Your memory is nearly full", "Close some programs. If memory runs out, buffering may stop.");
  }
  // A buffer that needs the disk and cannot have it keeps what memory allows: said once, in words, and in the log with the reason.
  if (!spill_noticed_ && (!spill_problem_.empty() || (spill_wanted_ && buffer_active_))) {
    std::string why = spill_problem_;
    if (why.empty() && replay_ && obs_output_active(replay_)) {
      const RingInfo held = ring_info();
      if (held.ok && held.state == 2) {
        calldata_t call{};
        if (proc_handler_call(obs_output_get_proc_handler(replay_), "spill_problem", &call)) {
          const char* text = calldata_string(&call, "text");
          why = text && *text ? text : "the disk part failed";
        }
        calldata_free(&call);
      }
    }
    if (!why.empty()) {
      spill_noticed_ = true;
      BX_LOG_WARN("the replay ring has no disk part: %s", why.c_str());
      if (on_notice) on_notice("Buffer length", "Disk too slow for this length; only memory is used");
    }
  }
  process_saved_replay();
  process_save_queue();
  process_closed_chunk();
  process_manual_recording();
  stamp_chunk_start();
  poll_timeline_split(GetTickCount());
  pending_chunk_publish(GetTickCount());
  dedupe_tick(GetTickCount());
  poll_recording();
  watchdog();
  // A probe, not a setting: the two real triggers (a capture that froze, an output that stopped by itself) cannot be
  // produced on demand, so BUFFRX_TEST_SALVAGE_MS fires the fault path once that many milliseconds into the run.
  {
    static const uint32_t salvage_after_ms = [] {
      char text[32];
      if (GetEnvironmentVariableA("BUFFRX_TEST_SALVAGE_MS", text, sizeof(text)) == 0) return 0u;
      return static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
    }();
    static const uint32_t started_ms = GetTickCount();
    static bool fired = false;
    static const bool freeze_armed = (start_test_freeze(), true);
    (void)freeze_armed;
    if (salvage_after_ms && !fired && GetTickCount() - started_ms > salvage_after_ms) {
      fired = true;
      // BUFFRX_TEST_SALVAGE_WAIT=1 takes the path a frozen capture takes: the save starts without a packet and is waited for.
      // =2 takes the game-hook path instead: saved without a packet, not waited for, and the buffer stopped behind it.
      char wait[8] = {0};
      GetEnvironmentVariableA("BUFFRX_TEST_SALVAGE_WAIT", wait, sizeof(wait));
      salvage_buffer("a test fault", wait[0] == '1', wait[0] == '2');
      if (wait[0] == '2') stop_buffer();
    }
  }
  settle_retiring_capture();
  check_game_fallback();
  check_display_present();
  update_capture_fit();

  // Idle: the buffer off and no manual record means nobody is watching, so the capture and audio
  // sources go away and the engine stops pulling frames. The next buffer start makes them again.
  if (!buffer_intent_ && !recording_active_ && !manual_active_ && !recording_pending_ && (capture_item_ || desktop_ || mic_ || !sources_released_)) {
    const uint32_t idle_now = GetTickCount();
    if (idle_since_ms_ == 0) {
      idle_since_ms_ = idle_now;
    } else if (idle_now - idle_since_ms_ >= kIdleReleaseGraceMs) {
      idle_since_ms_ = 0;
      release_idle_sources();
    }
  } else {
    idle_since_ms_ = 0;
  }
  // Past the grace and released: ask the action worker to drop the canvas to the idle rate.
  {
    const buffrx::IdleView view{.buffer_wanted = buffer_intent_, .recording = recording_active_ || manual_active_ || recording_pending_,
                        .sources_released = sources_released_ && !capture_item_ && !desktop_ && !mic_, .asleep = deep_idle_};
    if (buffrx::deep_idle_due(view, effective_config().capture_fps, GetTickCount(), deep_idle_failed_ms_)) deep_idle_due_ = true;
  }

  const uint32_t now = GetTickCount();
  if (replay_) {
    const uint64_t frames = obs_output_get_total_frames(replay_);
    if (last_tick_time_ != 0) {
      const uint32_t elapsed = now - last_tick_time_;
      if (elapsed > 0 && frames >= last_tick_frames_) {
        capture_fps_ = static_cast<double>(frames - last_tick_frames_) * 1000.0 / static_cast<double>(elapsed);
      }
    }
    last_tick_frames_ = frames;
    last_tick_time_ = now;
  }
  if (!deep_idle_) {  // asleep there is no encoder, no tap and no ring to look at
    watch_encoder(now);
  }
  if (now - display_hz_at_ >= 5000 || display_hz_at_ == 0) {
    display_hz_at_ = now == 0 ? 1 : now;
    display_hz_ = display_refresh_hz(config_);
  }
  if (!pending_notice_title_.empty() && on_notice) {
    on_notice(pending_notice_title_, pending_notice_detail_);
    pending_notice_title_.clear();
    pending_notice_detail_.clear();
  }
}

void ObsHost::watch_encoder(uint32_t now_ms) {
  if (!video_encoder_) return;
  const bool changed = buffrx::encoder_watch_feed(encoder_watch_, now_ms, obs_get_total_frames(),
                                                  BX_ENCODER_FRAMES(video_encoder_),
                                                  obs_encoder_active(video_encoder_));
  // The relief ladder runs in every effort, Maximum included (decided with the user, 2026-11-05): a person must never
  // be left with a clip missing frames. It is a layer over the stored settings and never rewrites them.
  if (encoder_watch_.behind && !governor_disabled_ &&
      buffrx::governor_should_step(governor_step_, true, now_ms, governor_last_step_ms_, recording_busy())) {
    governor_due_ = true;
  }
  // What encoded in real time for ten seconds is what this machine can do here: remembered so the next start begins at it.
  if (!encoder_watch_.behind) {
    if (capacity_ok_since_ms_ == 0) capacity_ok_since_ms_ = now_ms;
    if (now_ms - capacity_ok_since_ms_ >= 10000 && capacity_recorded_step_ != governor_step_) {
      capacity_recorded_step_ = governor_step_;
      capacity_remember(capacity_key_for(config_), governor_step_);
    }
  } else {
    capacity_ok_since_ms_ = 0;
  }
  if (!changed || !encoder_watch_.behind) return;
  const int percent = static_cast<int>(std::lround(encoder_watch_.ratio * 100.0));
  BX_LOG_WARN("the video encoder is not keeping up: %d%% of the frames at %d fps (%s)", percent, effective_config().capture_fps,
              encoder_id_.c_str());
  // A step still to come fixes this by itself (its own "Encoder stepped down" notice follows), so an alarm here would be
  // noise. Only what BuffrX cannot fix on its own is said: the ladder out of steps, or a frame-rate rung saved but not
  // yet applied because a source is live.
  if (!governor_disabled_ && governor_step_ < buffrx::kMaxReliefStep) {
    // The first word is said while there is still something to fix: the person sees the problem and what BuffrX is doing
    // about it, not only the outcome. Once per two minutes, so a long session is not a stream of cards.
    if (on_notice && (encoder_behind_told_ms_ == 0 || now_ms - encoder_behind_told_ms_ >= 120000)) {
      encoder_behind_told_ms_ = now_ms;
      on_notice("Encoder cannot keep up",
                std::to_string(percent) + " of 100 frames encoded. Lowering the effort.");
    }
    return;
  }
  // Said once, and again only after two minutes: the interface keeps showing it while it is true.
  if (encoder_behind_told_ms_ != 0 && now_ms - encoder_behind_told_ms_ < 120000) return;
  encoder_behind_told_ms_ = now_ms;
  if (on_notice) {
    // The person's own symptom first (why the clip looks wrong), then where the choice is.
    const std::string encoded = "Only " + std::to_string(percent) + " of every 100 frames reached the last clip. ";
    on_notice("Encoder effort lowered", encoded + "Lighter settings keep every frame. Try full quality again in Settings.");
  }
}

EngineStats ObsHost::stats() const {
  EngineStats stats;
  stats.buffer_active = buffer_active_;
  // `recording` is the manual output, never a Timeline segment.
  const bool manual = recording_ != nullptr && recording_active_ && !timeline_writer_.load();
  stats.recording = recording_busy();
  stats.recording_pending = recording_pending_;
  stats.capture_ready = capture_source_ready();
  stats.capture_fps = capture_fps_;
  stats.encoder_fps = capture_fps_;
  stats.capture_mode = capture_mode_now();
  // The name a person reads ("Counter-Strike 2", "Display Capture"): the same one the clip's folder gets.
  stats.capture_target = capture_folder();
  stats.buffer_state = buffrx::buffer_state_name(buffer_intent_, buffer_active_, buffer_stopping_);
  if (manual_active_ && manual_started_ms_ != 0) stats.recording_seconds = (GetTickCount() - manual_started_ms_) / 1000.0;
  else if (manual && recording_started_ms_ != 0) stats.recording_seconds = (GetTickCount() - recording_started_ms_) / 1000.0;
  {
    const Config running = effective_config();
    stats.width = running.output_width;
    stats.height = running.output_height;
    stats.fps = running.capture_fps;
  }
  stats.encoder_id = encoder_id_;
  stats.asleep = deep_idle_;
  stats.last_error = last_error_;
  stats.restarts = restarts_;
  stats.saved_clips = saved_clips_;
  // libobs's own video-thread counters: these are the ones that show render lag, where the output's
  // counters show nothing.
  stats.video_frames = obs_get_total_frames();
  stats.video_lagged = obs_get_lagged_frames();
  stats.render_time_ms = static_cast<double>(obs_get_average_frame_time_ns()) / 1000000.0;
  stats.active_fps = obs_get_active_fps();
  if (video_encoder_) stats.encoder_frames = BX_ENCODER_FRAMES(video_encoder_);
  if (replay_) {
    stats.total_frames = obs_output_get_total_frames(replay_);
    stats.total_bytes = static_cast<uint64_t>(obs_output_get_total_bytes(replay_));
    stats.dropped_frames = obs_output_get_frames_dropped(replay_);
  }
  if (obs_source_t* source = delivering_source()) {
    stats.width = static_cast<int>(obs_source_get_width(source));
    stats.height = static_cast<int>(obs_source_get_height(source));
  }
  stats.bitrate_kbps = peak_kbps_for(effective_config(), canvas_width_, canvas_height_);
  stats.display_hz = display_hz_.load();
  {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
      stats.process_mb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
  }
  stats.encoder_behind = encoder_watch_.behind;
  stats.encoder_ratio = encoder_watch_.ratio;
  stats.ring_seconds = static_cast<double>(std::max(1, config_.buffer_seconds));
  // What the ring holds right now, read from the ring itself: the bytes, the footage they span, and the rate that makes.
  const RingInfo held = ring_info();
  stats.ring_measured = buffer_active_ && held.ok && held.span_ms >= 3000;
  if (held.ok) {
    stats.ring_used_bytes = static_cast<uint64_t>(held.bytes);
    stats.ring_ram_bytes = static_cast<uint64_t>(held.ram_bytes);
    stats.ring_disk_bytes = static_cast<uint64_t>(held.disk_bytes);
    stats.ring_spilled_bytes = static_cast<uint64_t>(held.spilled_bytes);
    stats.ring_received_bytes = static_cast<uint64_t>(held.total_bytes);
    stats.ring_chunk_bytes = static_cast<uint64_t>(held.chunk_bytes);
    stats.ring_deduped_bytes = static_cast<uint64_t>(held.deduped_bytes);
    stats.ring_chunks_linked = static_cast<int>(held.chunks_linked);
    stats.ring_chunks_waiting = static_cast<int>(held.chunks_waiting);
    stats.ring_disk_state = static_cast<int>(held.state);
    stats.ring_stalled = held.stalled != 0;
    stats.ring_held_seconds = static_cast<double>(held.span_ms) / 1000.0;
    if (held.span_ms >= 3000) stats.ring_avg_kbps = static_cast<double>(held.bytes) * 8.0 / static_cast<double>(held.span_ms);
    // The footage held, up to the length asked for (the ring keeps a little more than that).
    stats.ring_filled = std::min(stats.ring_seconds, static_cast<double>(held.span_ms) / 1000.0);
  }
  stats.level_desktop = level_desktop_.load(std::memory_order_relaxed);
  stats.level_mic = level_mic_.load(std::memory_order_relaxed);
  stats.audio_meters_active = audio_meters_active();
  return stats;
}

ObsHost::RingInfo ObsHost::ring_info() const {
  RingInfo info;
  if (!replay_ || !obs_output_active(replay_)) return info;
  calldata_t call{};
  if (proc_handler_call(obs_output_get_proc_handler(replay_), "ring_info", &call)) {
    long long value = 0;
    info.ok = true;
    if (calldata_get_int(&call, "bytes", &value)) info.bytes = value;
    if (calldata_get_int(&call, "packets", &value)) info.packets = value;
    if (calldata_get_int(&call, "span_ms", &value)) info.span_ms = value;
    if (calldata_get_int(&call, "total_bytes", &value)) info.total_bytes = value;
    if (calldata_get_int(&call, "ram_bytes", &value)) info.ram_bytes = value;
    if (calldata_get_int(&call, "disk_bytes", &value)) info.disk_bytes = value;
    if (calldata_get_int(&call, "spilled_bytes", &value)) info.spilled_bytes = value;
    if (calldata_get_int(&call, "state", &value)) info.state = value;
    if (calldata_get_int(&call, "stalled", &value)) info.stalled = value;
    if (calldata_get_int(&call, "backstop", &value)) info.backstop = value;
    if (calldata_get_int(&call, "skipped", &value)) info.skipped = value;
    if (calldata_get_int(&call, "chunk_bytes", &value)) info.chunk_bytes = value;
    if (calldata_get_int(&call, "deduped_bytes", &value)) info.deduped_bytes = value;
    if (calldata_get_int(&call, "chunks_linked", &value)) info.chunks_linked = value;
    if (calldata_get_int(&call, "chunks_waiting", &value)) info.chunks_waiting = value;
  }
  calldata_free(&call);
  return info;
}

std::string ObsHost::obs_version() const { return obs_get_version_string() ? obs_get_version_string() : "unknown"; }

// The record of what encoded in real time on this machine, keyed by what changes the encoder work: codec, size, rate and
// the quality preset. One small JSON file in the data root; nothing else reads it.
int ObsHost::capacity_remembered_step(const std::string& key) const {
  return capacity_read_step(capacity_path_, key);
}

void ObsHost::capacity_remember(const std::string& key, int step) const {
  capacity_write_step(capacity_path_, key, step);
}

std::string ObsHost::preferred_codec() const {
  // Hardware AV1 only: FFmpeg's own AV1 encoder is software and would not keep up with a game at any setting.
  for (size_t index = 0;; ++index) {
    const char* id = nullptr;
    if (!obs_enum_encoder_types(index, &id) || !id) break;
    const std::string name = id;
    if (name.find("av1") != std::string::npos && name.rfind("ffmpeg", 0) != 0) return "av1";
  }
  return "h264";
}

std::string ObsHost::replay_diagnostics() const {
  if (!replay_) return "no replay output";
  const char* last_error = obs_output_get_last_error(replay_);
  const int active = obs_output_active(replay_) ? 1 : 0;
  const uint64_t frames = obs_output_get_total_frames(replay_);
  const uint64_t dropped = obs_output_get_frames_dropped(replay_);
  char buffer[400];
  std::snprintf(buffer, sizeof(buffer), "active=%d frames=%llu dropped=%llu last_error='%s'", active,
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(dropped),
                last_error ? last_error : "");
  return buffer;
}

Json ObsHost::encoder_properties(bool configured) const {
  // An idle encoder is gone, and its snapshot predates any settings changed since sleep. Build the
  // values from the current plan without creating an encoder or waking the capture pipeline.
  configured = configured || deep_idle_;
  const Config planned = configured ? effective_for(config_) : config_;
  std::string property_id = encoder_id_;
  if (configured) {
    std::vector<std::string> candidates = choose_encoder_ids(planned);
    if (candidates.empty() && planned.encoder != "auto") {
      Config automatic = planned;
      automatic.encoder = "auto";
      candidates = choose_encoder_ids(automatic);
    }
    if (candidates.empty() && planned.codec != "h264") {
      Config fallback = planned;
      fallback.codec = "h264";
      fallback.encoder = "auto";
      candidates = choose_encoder_ids(fallback);
    }
    // What actually runs wins over the first candidate: a vendor that refused to start left the engine on
    // another encoder (x264), and the Advanced page must show that one's knobs, not the absent vendor's.
    const auto codec_of = [](const std::string& id) {
      if (id.find("av1") != std::string::npos) return std::string("av1");
      if (id.find("hevc") != std::string::npos) return std::string("hevc");
      return std::string("h264");
    };
    const bool running_matches = !encoder_id_.empty() && codec_of(encoder_id_) == planned.codec;
    property_id = running_matches ? encoder_id_ : candidates.empty() ? std::string() : candidates.front();
  }
  Json out = Json::object();
  out.set("id", Json(property_id));
  out.set("family", Json(encoder_family_name(encoder_family(property_id))));
  Json properties_out = Json::array();
  if (property_id.empty()) {
    out.set("properties", properties_out);
    return out;
  }
  obs_properties_t* properties = obs_get_encoder_properties(property_id.c_str());
  if (!properties) {
    out.set("properties", properties_out);
    return out;
  }
  obs_data_t* live = configured ? obs_encoder_defaults(property_id.c_str()) :
                     video_encoder_ ? obs_encoder_get_settings(video_encoder_) : nullptr;
  if (configured && live) {
    const auto settings = configured_encoder_settings(encoder_family(property_id), planned, governor_step_);
    apply_encoder_settings(live, properties, settings);
  }
  for (obs_property_t* property = obs_properties_first(properties); property; obs_property_next(&property)) {
    const char* name = obs_property_name(property);
    if (!name) continue;
    const obs_property_type type = obs_property_get_type(property);
    Json field = Json::object();
    field.set("name", Json(name));
    const char* label = obs_property_description(property);
    field.set("label", Json(label && *label ? label : name));
    const char* long_description = obs_property_long_description(property);
    if (long_description && *long_description) field.set("description", Json(long_description));
    switch (type) {
      case OBS_PROPERTY_BOOL:
        field.set("kind", Json("bool"));
        break;
      case OBS_PROPERTY_INT:
        field.set("kind", Json("int"));
        field.set("min", Json(obs_property_int_min(property)));
        field.set("max", Json(obs_property_int_max(property)));
        field.set("step", Json(std::max(1, obs_property_int_step(property))));
        break;
      case OBS_PROPERTY_FLOAT:
        field.set("kind", Json("float"));
        field.set("min", Json(obs_property_float_min(property)));
        field.set("max", Json(obs_property_float_max(property)));
        break;
      case OBS_PROPERTY_LIST: {
        field.set("kind", Json("list"));
        Json options = Json::array();
        Json labels = Json::array();
        const size_t count = obs_property_list_item_count(property);
        for (size_t index = 0; index < count; ++index) {
          if (obs_property_list_item_disabled(property, index)) continue;
          const char* value = obs_property_list_item_string(property, index);
          const char* text = obs_property_list_item_name(property, index);
          options.push(Json(value ? value : ""));
          labels.push(Json(text && *text ? text : (value ? value : "")));
        }
        field.set("options", options);
        field.set("option_labels", labels);
        break;
      }
      case OBS_PROPERTY_TEXT:
        field.set("kind", Json("text"));
        break;
      default:
        continue;  // group/colour/font/button/editable-list: not a simple knob
    }
    if (live) {
      switch (type) {
        case OBS_PROPERTY_BOOL: field.set("value", Json(obs_data_get_bool(live, name))); break;
        case OBS_PROPERTY_INT: field.set("value", Json(static_cast<int64_t>(obs_data_get_int(live, name)))); break;
        case OBS_PROPERTY_FLOAT: field.set("value", Json(obs_data_get_double(live, name))); break;
        default: field.set("value", Json(obs_data_get_string(live, name))); break;
      }
    }
    properties_out.push(std::move(field));
  }
  if (live) obs_data_release(live);
  obs_properties_destroy(properties);
  out.set("properties", properties_out);
  return out;
}

std::vector<WindowInfo> ObsHost::windows() {
  const uint32_t now = GetTickCount();
  if (!enum_cache_.windows.empty() && (now - enum_cache_.windows_at) < kEnumCacheTtlMs) return enum_cache_.windows;
  // Direct Win32 enumeration: no temporary capture source, which is where win-capture.dll crashed.
  const std::vector<WindowInfo> list = enumerate_windows();
  enum_cache_.windows = list;
  enum_cache_.windows_at = now;
  return list;
}

std::vector<WindowInfo> ObsHost::game_windows() {
  const uint32_t now = GetTickCount();
  if (!enum_cache_.games.empty() && (now - enum_cache_.games_at) < kEnumCacheTtlMs) return enum_cache_.games;
  const std::vector<WindowInfo> list = enumerate_game_windows();
  enum_cache_.games = list;
  enum_cache_.games_at = now;
  return list;
}

std::vector<DisplayInfo> ObsHost::displays() { return enumerate_displays(); }

std::vector<AudioDeviceInfo> ObsHost::audio_devices(bool inputs) {
  const uint32_t now = GetTickCount();
  auto& cache = inputs ? enum_cache_.audio_inputs : enum_cache_.audio_outputs;
  if (!cache.empty() && (now - enum_cache_.audio_at) < kEnumCacheTtlMs) return cache;
  std::vector<AudioDeviceInfo> devices;
  // The type's properties, never a throwaway source: a "default" output capture matches the monitoring
  // device, libobs queues it as the audio thread's deduplication source, and a source released before
  // that task runs is installed after it was freed. The audio thread then reads it every tick and
  // faults in obs.dll (+0x443e, should_silence_monitored_source) once the heap reuses the memory.
  obs_properties_t* properties =
      obs_get_source_properties(inputs ? kMicCaptureSource : kDesktopCaptureSource);
  obs_property_t* property = properties ? obs_properties_get(properties, "device_id") : nullptr;
  if (property) {
    const size_t count = obs_property_list_item_count(property);
    for (size_t index = 0; index < count; ++index) {
      AudioDeviceInfo info;
      const char* value = obs_property_list_item_string(property, index);
      const char* name = obs_property_list_item_name(property, index);
      info.id = value ? value : "";
      info.name = name ? name : info.id;
      info.is_default = info.id == "default" || info.id == "Default";
      if (!info.id.empty()) devices.push_back(info);
    }
  }
  if (properties) obs_properties_destroy(properties);
  if (inputs) enum_cache_.audio_inputs = devices;
  else enum_cache_.audio_outputs = devices;
  enum_cache_.audio_at = GetTickCount();
  return devices;
}

bool ObsHost::apply(const Config& config, bool restart_outputs, std::string& error) {
  if (!initialized_) {
    error = "engine is not initialized";
    return false;
  }
  // One rebuild at a time. This is reached from a pipe thread, a hotkey, a profile switch and the
  // tick, and the work below tears the pipeline down and builds it again - obs_reset_video included,
  // which takes seconds. Two of those overlapping is the hang libobs does not survive, so the second
  // waits here for the first rather than entering a pipeline that is halfway destroyed.
  RebuildGuard rebuilding(rebuild_gate_);
  // Any setting may change what the enumeration helpers should report; drop the caches.
  enum_cache_.windows_at = 0;
  enum_cache_.games_at = 0;
  enum_cache_.audio_at = 0;
  // What the pipeline runs now, and what it would run after this change. The mode is a layer over the user's
  // settings, never a video setting of its own: Auto -> On with the listed game already running leaves both
  // identical. A rebuild for that reset the video under a live game hook, which is how the interface hung.
  const Config running = built_config_valid_ ? built_config_ : effective_for(config_);
  if (config.codec != config_.codec || config.encoder != config_.encoder) codec_override_.clear();
  const Config next = effective_for(config);
  bool video_changed = next.scale_filter != running.scale_filter || next.capture_fps != running.capture_fps ||
                       next.output_width != running.output_width ||
                       next.output_height != running.output_height || force_video_rebuild_;
  const bool capture_changed = config.capture_mode != config_.capture_mode ||
                               config.display_id != config_.display_id ||
                               config.display_name != config_.display_name ||
                               config.window_id != config_.window_id ||
                               config.game_title != config_.game_title ||
                               config.display_method != config_.display_method ||
                               config.capture_cursor != config_.capture_cursor;
  // A different display is very often a different native size. Swapping the source under the old canvas
  // scales the picture to a canvas it does not belong to; a display switch that changes the size re-plans
  // the canvas once, at the new display's own resolution.
  int display_target_width = 0;
  int display_target_height = 0;
  const bool display_target_known =
      config.capture_mode == "display" && win32_target_size(config, display_target_width, display_target_height);
  const bool display_replan =
      display_target_known &&
      display_switch_needs_replan(config_, config, display_target_width, display_target_height, canvas_width_,
                                  canvas_height_,
                                  performance_active() && next.output_width > 0 && next.output_height > 0);
  if (display_replan) video_changed = true;
  const bool encoder_changed = next.codec != running.codec || next.encoder != running.encoder ||
                               next.quality != running.quality || next.custom_kbps != running.custom_kbps ||
                               next.peak_mode != running.peak_mode ||
                               next.encoder_options.dump() != running.encoder_options.dump() ||
                               next.encoder_mode != running.encoder_mode ||
                               next.buffer_seconds != running.buffer_seconds ||
                               next.keyframe_seconds != running.keyframe_seconds ||
                               next.container != running.container ||
                               next.muxer_settings != running.muxer_settings;
  // Only the audio encoders need the outputs rebuilt. Sources reach the outputs through libobs's global
  // mixer, so a device change, a channel switched on or off and the mic filters apply live and the
  // ring is kept (a noise-gate toggle used to empty the buffer).
  const bool audio_changed = next.separate_tracks != running.separate_tracks ||
                             next.audio_tracks != running.audio_tracks ||
                             next.audio_bitrate_kbps != running.audio_bitrate_kbps;
  const bool audio_sources_changed = config.desktop_device != config_.desktop_device ||
                                     config.mic_device != config_.mic_device ||
                                     config.desktop_enabled != config_.desktop_enabled ||
                                     config.mic_enabled != config_.mic_enabled ||
                                     config.mic_noise_suppress != config_.mic_noise_suppress ||
                                     config.mic_noise_gate != config_.mic_noise_gate;

  const bool speed_step_changed = config.encoder_speed_step != config_.encoder_speed_step;
  if (speed_step_changed) governor_step_ = config.encoder_speed_step;
  const bool clips_moved = config.clips_dir != config_.clips_dir;
  if (config.stretch_mode != config_.stretch_mode) capture_fit_applied_ = false;  // the tick re-fits, live
  // The settings that are running now. A rebuild that does not finish puts them back, so the engine is
  // never left running settings that do not match the encoders it has.
  const Config previous = config_;
  // The relief ladder's frame-rate rung: a restart is the only place it can land, because libobs forbids a
  // video reset under live sources. If the step changed the capture rate, the pipeline is rebuilt for it — but never
  // while a game hook is attached: resetting the video under one detaches the hook, and a hook the game is waiting on
  // is exactly how a game freezes. There the rung waits for the next buffer start, which builds at the stored rate.
  const bool relief_fps_changed =
      buffrx::relief_capture_fps(config.capture_fps, config.encoder_speed_step) != config.capture_fps &&
      !(capture_mode_now() == "game" && !follow_display_);
  config_ = config;
  // What the user asked for, not what happens to be running this instant: a buffer that was still
  // waiting for its first frame, or one between a stop and a restart, is still wanted.
  const bool buffer_wanted = buffer_intent_;
  // The person's own recording only: Timeline's segment shares the output and the flag, and the tick brings it back by
  // itself (paused_rebuild, then writing). Resuming it here started a manual recording out of every rebuild.
  const bool was_recording = recording_busy();
  if (deep_idle_) {
    // Nothing is built while the engine sleeps (no outputs, no encoders, no sources, a canvas at the idle rate),
    // and the wake builds all of it from config_: a change only has to be remembered. The canvas is re-planned so
    // the readouts quote the size the next start will use. Anything that wants frames wakes the engine first
    // (start_buffer, start_recording, take_screenshot), so a rebuild is never needed from here.
    if (video_changed || capture_changed || encoder_changed) plan_canvas_for(config);
    return true;
  }
  const bool rebuild_outputs = video_changed || encoder_changed || audio_changed || speed_step_changed || relief_fps_changed ||
                               force_encoder_rebuild_;
  // The tear-down and the build as one step, so a build that fails can run again with the settings that
  // were working. create_outputs reads config_, so it is only ever called with config_ already the
  // config being built.
  const auto build_pipeline = [&](const Config& wanted, std::string& build_error) {
    // A second obs_reset_video while the capture source exists hangs libobs (recorded in ISSUES.md),
    // so a video change tears the whole pipeline down first: outputs, the capture source (its canvas
    // just changed) and the encoders, which hold the old video handler.
    stop_buffer();
    stop_recording();
    destroy_outputs();
    if (video_changed) drop_retiring_capture();
    if ((video_changed || relief_fps_changed) && capture_item_) {
      obs_sceneitem_remove(capture_item_);
      capture_item_ = nullptr;
    }
    if ((video_changed || relief_fps_changed) && !configure_video(wanted, build_error)) return false;
    if ((video_changed || capture_changed || relief_fps_changed) && !configure_capture(wanted, build_error)) return false;
    if (audio_changed) configure_audio(wanted);
    if (!configure_encoders(wanted, build_error)) return false;
    if (!create_outputs(build_error)) return false;
    // What the pipeline runs from here, so the next change is compared with it rather than with the last ask.
    built_config_ = effective_config();
    built_config_valid_ = true;
    return true;
  };
  // The old session closed with the old settings; a recording that was running carries on in a new one.
  const auto resume_recording = [&]() {
    if (!was_recording) return;
    std::string recording_error;
    if (!start_recording(recording_error)) {
      BX_LOG_WARN("recording did not resume after the change: %s", recording_error.c_str());
      last_error_ = "the recording stopped when the settings changed: " + recording_error;
      if (on_error) on_error("recording", last_error_);
    }
  };
  if (rebuild_outputs) {
    if (!build_pipeline(effective_config(), error)) {
      // The change did not build. The outputs are gone and config_ still names the settings that could
      // not be built, so the settings that were running go back and the pipeline is built again. The
      // failure goes on the status line (last_error) and to the interface: a buffer that cannot be built
      // is off with a reason, not "starting" for ever.
      const std::string failure = error;
      config_ = previous;
      governor_step_ = previous.encoder_speed_step;
      std::string restore_error;
      const bool restored = build_pipeline(previous, restore_error);
      last_error_ = "the new settings did not start: " + failure;
      if (!restored) {
        last_error_ += " (the settings that were running did not come back either: " + restore_error + ")";
        // Nothing can start on a pipeline that is not there, and the watchdog's retry would fail the
        // same way every tick: it is built again when the settings change or the person starts it.
        buffer_intent_ = false;
      }
      BX_LOG_ERROR("%s", last_error_.c_str());
      if (on_error) on_error("settings", last_error_);
      error = last_error_;
      if (restored) {
        resume_recording();
        if (buffer_wanted) {
          std::string buffer_error;
          if (!start_buffer(buffer_error)) BX_LOG_INFO("buffer will start when ready: %s", buffer_error.c_str());
        }
      }
      return false;
    }
    resume_recording();
  } else if (capture_changed) {
    // The outputs encode the canvas, not the source, so the source is swapped under a running ring:
    // the footage before the swap stays in the buffer (an alt-tab out of a game used to empty it), and the
    // source being replaced stays under the new one until the new one has a frame, so the ring has no gap.
    if (!configure_capture(config, error, false, true)) return false;
  }
  if (audio_sources_changed && !audio_changed) configure_audio(config);
  // A moved clips folder applies live: the save procedure reads "directory" when it writes, so the ring
  // is kept (verified: frames kept counting, no restart, the next clip landed in the new folder). A
  // rebuild above already created the output with the new path.
  if (clips_moved && replay_ && !rebuild_outputs) {
    obs_data_t* moved = obs_data_create();
    const std::string moved_path = slashify(config_.clips_dir + "\\");
    obs_data_set_string(moved, "path", moved_path.c_str());
    obs_data_set_string(moved, "directory", moved_path.c_str());  // what the save procedure reads
    obs_output_update(replay_, moved);
    obs_data_release(moved);
  }
  if (!rebuild_outputs && restart_outputs) {
    stop_buffer();
    force_replay_stopped();
  }
  if ((rebuild_outputs || restart_outputs) && buffer_wanted) {
    // A new source has no size until the graphics thread has ticked it (game capture: until it hooks),
    // so this start may only record the intent; the watchdog starts the ring on the first frame.
    std::string buffer_error;
    if (!start_buffer(buffer_error)) BX_LOG_INFO("buffer will start when ready: %s", buffer_error.c_str());
  }
  if (desktop_) {
    const float volume = static_cast<float>(config.desktop_volume);
    obs_source_set_volume(desktop_, volume);
    obs_source_set_muted(desktop_, config.desktop_muted);
  }
  if (mic_) {
    const double gain = std::pow(10.0, config.mic_gain_db / 20.0);
    obs_source_set_volume(mic_, static_cast<float>(config.mic_volume * gain));
    obs_source_set_muted(mic_, config.mic_muted);
  }
  if (display_replan && (buffer_wanted || was_recording) && on_notice) {
    on_notice("Display changed", "The buffer restarts at " + std::to_string(canvas_width_) + " × " +
                                     std::to_string(canvas_height_));
  }
  return true;
}

void ObsHost::shutdown() {
  // A save that has already finished is filed before its worker stops; the queue then drains, so a
  // save the user was told about is still trimmed and filed.
  if (replay_ && replay_saved_) process_saved_replay();
  // The one place a wait belongs: nothing else can be starved, and the process is ending.
  save_worker_.stop();
  prune_worker_.stop();
  reaper_.stop();
  // The dedupe worker may be inside a chunk probe on the media lock, which outlives nothing here: joined while a wait
  // is still allowed, before libobs and the runtime's FFmpeg are unloaded under it.
  dedupe_shutdown();
  if (!initialized_) return;
  stop_buffer();
  stop_recording();
  destroy_outputs();
  for (auto* encoder : audio_encoders_) obs_encoder_release(encoder);
  audio_encoders_.clear();
  if (video_encoder_) {
    obs_encoder_release(video_encoder_);
    video_encoder_ = nullptr;
  }
  encoder_id_.clear();
  if (encoder_snapshot_) {
    obs_data_release(encoder_snapshot_);
    encoder_snapshot_ = nullptr;
  }
  drop_retiring_capture();
  if (capture_item_) {
    obs_sceneitem_remove(capture_item_);
    capture_item_ = nullptr;
  }
  detach_meter(&desktop_volmeter_);
  detach_meter(&mic_volmeter_);
  // Same rule as the scene on channel 0: the canvas keeps its own reference to each audio source, so the
  // channel is cleared before the source is released or the device stays open past shutdown.
  obs_set_output_source(kDesktopViewChannel, nullptr);
  obs_set_output_source(kMicViewChannel, nullptr);
  if (desktop_) {
    obs_source_release(desktop_);
    desktop_ = nullptr;
  }
  if (mic_noise_suppress_) { obs_source_release(mic_noise_suppress_); mic_noise_suppress_ = nullptr; }
  if (mic_noise_gate_) { obs_source_release(mic_noise_gate_); mic_noise_gate_ = nullptr; }
  if (mic_) {
    obs_source_release(mic_);
    mic_ = nullptr;
  }
  if (scene_) {
    // The output source is a weak reference: clear it before the scene goes away.
    obs_set_output_source(0, nullptr);
    obs_scene_release(scene_);
    scene_ = nullptr;
  }
  {
    const std::string core_data = slashify(runtime_root_ + "\\data\\libobs\\");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    obs_remove_data_path(core_data.c_str());
#pragma clang diagnostic pop
  }
  obs_shutdown();
  audio_ready_ = false;
  initialized_ = false;
  BX_LOG_INFO("engine shut down");
}

// Runs on the output's thread. OBS sends "saved" with empty calldata, so the path is asked for on the
// tick (process_saved_replay); this only raises the flag.
void ObsHost::on_replay_saved(void* data, calldata* call) {
  (void)call;
  auto* self = static_cast<ObsHost*>(data);
  if (self) self->replay_saved_ = true;
}

void ObsHost::on_replay_stopped(void* data, calldata* call) {
  auto* self = static_cast<ObsHost*>(data);
  if (!self) return;
  // This runs on libobs's own thread. An exception here has nowhere to go and ends the process, so
  // it is caught and named.
  try {
    const char* reason = calldata_string(call, "reason");
    // Only a stop nobody asked for is a warning: this used to say "the watchdog will restart it" on every stop the user
    // (or an idle sleep, a settings change, a profile switch) asked for - 28 of them in a 25-cycle stress run, which
    // buried the one that mattered. Read from libobs's thread; a stale flag at worst words one line the other way.
    if (self->buffer_intent_) {
      BX_LOG_WARN("replay buffer stopped on its own (%s) - the watchdog will restart it", reason ? reason : "no reason given");
    } else {
      BX_LOG_INFO("replay buffer stopped (%s)", reason ? reason : "as asked");
    }
  } catch (const std::exception& failure) {
    BX_LOG_ERROR("the replay buffer's stop handler failed: %s", failure.what());
  } catch (...) {
    BX_LOG_ERROR("the replay buffer's stop handler failed with an unknown error");
  }
  (void)call;
}

// Runs on the output's own thread, inside libobs's "file_changed" signal: the splitter has just opened a new
// file, before its headers are written and before the old one is closed. It must not take an engine lock, touch
// session state or block, so it only copies the path (Windows form) with the tick it fired on, and the tick
// takes it (poll_timeline_split). The old file is indexed only after its writer lets go of it.
void ObsHost::on_recording_file_changed(void* data, calldata* call) {
  auto* self = static_cast<ObsHost*>(data);
  if (!self) return;
  try {
    const char* next = calldata_string(call, "next_file");
    if (!next || !*next) return;
    std::string file = next;
    std::replace(file.begin(), file.end(), '/', '\\');
    const uint32_t at_ms = GetTickCount();
    std::lock_guard<std::mutex> lock(self->split_mutex_);
    // A session writes one file every few seconds, so a queue this long means the tick has stopped: dropping
    // the change loses index entries but never the footage, which stays in the session's own media folder.
    if (self->split_changes_.size() < 64) self->split_changes_.push_back(SplitChange{std::move(file), at_ms});
  } catch (...) {
    // Inside libobs's thread, where an exception has nowhere to go. A change that cannot be copied is a
    // segment the index misses, never a file that is lost.
  }
}

void ObsHost::on_recording_stopped(void* data, calldata* call) {
  auto* self = static_cast<ObsHost*>(data);
  if (!self) return;
  // The flag comes first, before anything that can fail. It is what tells the tick that this chunk
  // is finished and has to be indexed - and a chunk that is not indexed is a recording the user
  // cannot export, find or recover. Logging is a nicety; this is the job.
  self->recording_active_ = false;
  self->chunk_closed_ = true;
  // This runs on libobs's own thread, where an exception has nowhere to go.
  try {
    const char* path = calldata_string(call, "path");
    if (path && *path) BX_LOG_INFO("recording chunk closed: %s", path);
    const long long code = calldata_int(call, "code");
    self->recording_stop_code_ = code;
    if (code != 0) BX_LOG_WARN("the recording output stopped with code %lld", code);
    const char* last_error = calldata_string(call, "last_error");
    if (last_error && *last_error) BX_LOG_WARN("recording output last error: %s", last_error);
  } catch (const std::exception& failure) {
    BX_LOG_ERROR("the recording output's stop handler failed: %s", failure.what());
  } catch (...) {
    BX_LOG_ERROR("the recording output's stop handler failed with an unknown error");
  }
}

void ObsHost::on_manual_recording_stopped(void* data, calldata* call) {
  auto* self = static_cast<ObsHost*>(data);
  if (!self) return;
  self->manual_stop_code_ = calldata_int(call, "code");
  self->manual_active_ = false;
  self->manual_closed_ = true;
}

std::string ObsHost::current_session_id() const {
  std::lock_guard<std::mutex> lock(recording_mutex_);
  return session_.id;
}

namespace {
// A fresh handle sees the true size of a file another process is still writing; the directory entry
// keeps saying zero until the writer closes it, which is why the listing must ask the file itself.
int64_t file_size_now(const std::string& file) {
#ifdef _WIN32
  HANDLE handle = CreateFileA(file.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return 0;
  LARGE_INTEGER size{};
  const bool ok = GetFileSizeEx(handle, &size) != 0;
  CloseHandle(handle);
  return ok ? size.QuadPart : 0;
#else
  struct stat st{};
  return stat(file.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : 0;
#endif
}
}  // namespace

ObsHost::LiveChunk ObsHost::live_chunk() const {
  std::lock_guard<std::mutex> lock(recording_mutex_);
  LiveChunk live;
  live.chunk_seconds = chunk_seconds_now();
  if (session_.id.empty() || current_chunk_.empty()) return live;
  live.file = current_chunk_;
  live.start_ms = current_chunk_start_ms_;
  live.bytes = file_size_now(current_chunk_);
  return live;
}

// Queued, never walked here: this is called from the tick and from a command, both under the engine lock,
// and the sweep lists the sessions directory and deletes whole sessions from disk. The worker takes it;
// what the sweep may not touch (an open session) is copied with the job, and the leases are read under the
// retention gate when it decides, never from the list as it stood when this was queued.
void ObsHost::prune_sessions(const std::string& open_id) {
  PruneJob job;
  job.budget_bytes = config_.recordings_budget_gb > 0
                         ? static_cast<int64_t>(config_.recordings_budget_gb) * 1024 * 1024 * 1024
                         : 0;
  job.open_id = open_id;
  // Footage left by a stopped session keeps the Timeline window while it records; otherwise the keep-hours decide.
  job.timeline_minutes = config_.timeline_minutes > 0 ? config_.timeline_minutes : config_.timeline_keep_hours * 60;
  job.now_wall_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
  // The leases are not copied with the job: the sweep reserves what it deletes under the retention gate at the moment
  // it decides (timeline_retention.h), so a save that pins footage while this waits in the queue still keeps it.
  if (!prune_worker_.push(std::move(job))) {
    BX_LOG_INFO("a retention sweep is already waiting; the next session boundary sweeps again");
  }
}

void ObsHost::note_open_timeline(const std::string& id) {
  std::lock_guard<std::mutex> lock(open_timeline_mutex_);
  open_timeline_id_ = id;
}

void ObsHost::run_prune(const PruneJob& job) {
  // Footage the open Timeline session let go of: its files leave here, off the tick. A file Windows still holds waits for the
  // next sweep (prune_timeline_now hands it back).
  if (!job.delete_chunks.empty()) {
    std::vector<DroppedChunk> still_held;
    session_store_delete_chunk_files(job.delete_chunks, still_held);
    if (!still_held.empty()) {
      std::lock_guard<std::mutex> lock(held_mutex_);
      held_chunks_.insert(held_chunks_.end(), still_held.begin(), still_held.end());
    }
  }
  if (job.budget_bytes > 0) {
    std::vector<std::string> removed;
    int64_t freed = 0;
    int64_t total = 0;
    std::string error;
    if (!session_prune(job.budget_bytes, job.open_id, removed, freed, total, error)) {
      BX_LOG_WARN("pruning to the recordings budget was incomplete: %s", error.c_str());
    }
  }
  // The Timeline's footage is temporary and leaves on its own window, never through the recordings budget
  // above (session_prune skips kind == "timeline"). Its wall clock decides, so the window survives a reboot.
  // The tick sweeps once a minute as well; this is the boundary's extra sweep.
  if (job.timeline_minutes <= 0) return;
  std::string error;
  std::string open_timeline;
  {
    std::lock_guard<std::mutex> lock(open_timeline_mutex_);
    open_timeline = open_timeline_id_;
  }
  // The store reserves what it deletes under the retention gate, at the moment it decides each session's chunks
  // (timeline_retention.h): the leases in force when this job was queued are not what decides, and no disk work runs
  // under the gate.
  if (!session_store_prune_timeline(job.timeline_minutes, job.now_wall_ms, error, open_timeline)) {
    BX_LOG_WARN("the timeline retention was incomplete: %s", error.c_str());
  }
}

std::string ObsHost::take_timeline_lease(const TimelinePin& pin, int64_t now_wall_ms, uint64_t seen_generation) {
  return timeline_lease_take(pin, now_wall_ms, seen_generation);
}

bool ObsHost::release_timeline_lease(const std::string& id, int64_t now_wall_ms) {
  return timeline_lease_revoke(id, now_wall_ms);
}

size_t ObsHost::release_timeline_leases(const std::string& owner, const std::string& session_id, int64_t now_wall_ms) {
  return timeline_lease_revoke_owner(owner, session_id, now_wall_ms);
}

bool ObsHost::request_timeline_rotation() {
  if (!timeline_writer_.load()) return false;
  bool enabled = false;
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    obs_output_t* output = recording_;
    if (!output || !obs_output_active(output)) {
      BX_LOG_WARN("a timeline save asked for a new segment, but nothing is being written");
      return false;
    }
    // OBS's own split procedure: it asks the muxer for a boundary on the next keyframe. The output is not
    // stopped - one process keeps writing the session, and the ring keeps its encoders - and the new file
    // reaches the index through `file_changed` like every other one.
    proc_handler_t* handler = obs_output_get_proc_handler(output);
    calldata_t data{};
    calldata_init(&data);
    const bool called = handler && proc_handler_call(handler, "split_file", &data);
    enabled = called && calldata_bool(&data, "split_file_enabled");
    calldata_free(&data);
  }
  if (!enabled) {
    // Said as the reason a save waits rather than fails: the next file the splitter opens on its own (5 s ask,
    // a keyframe away) still closes this segment and reaches the index.
    BX_LOG_WARN("the timeline output could not split its file; the save waits for the next segment");
    return false;
  }
  BX_LOG_INFO("timeline save asked for the open segment to close now");
  return true;
}

// Retention follows the lock: the tick trims the open session's list under recording_mutex_; the sweep runs on
// prune_worker_ and skips that session by open_id.
void ObsHost::prune_timeline_now(int64_t now_wall_ms) {
  if (config_.timeline_minutes <= 0) return;
  // The walk and the deletions run on the prune worker: the tick calls this with the engine lock held.
  PruneJob job;
  job.timeline_minutes = config_.timeline_minutes;
  job.now_wall_ms = now_wall_ms;
  // The session being written trims its own list here, under the lock that guards it; the store decides with the leases
  // in force and reserves what it drops in one short step under the retention gate (timeline_retention.h), then renames
  // and rewrites the index with the gate free, so a save that pins footage as this runs is refused those chunks rather
  // than waiting on the tick. Its expired files go with the job; the sweep leaves this session alone (run_prune). Slow
  // deletion is not here: the worker deletes the files.
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    if (!session_.id.empty() && session_.kind == "timeline") {
      std::string drop_error;
      if (!session_store_drop_expired(session_, config_.timeline_minutes, now_wall_ms, job.delete_chunks, drop_error)) {
        BX_LOG_WARN("the open timeline session's expired footage was not dropped: %s", drop_error.c_str());
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(held_mutex_);
    job.delete_chunks.insert(job.delete_chunks.end(), held_chunks_.begin(), held_chunks_.end());
    held_chunks_.clear();
  }
  PruneJob queued = job;
  if (!prune_worker_.push(std::move(queued))) {
    // Nothing is lost: the dropped chunks wait for the next sweep with the ones Windows was holding.
    std::lock_guard<std::mutex> lock(held_mutex_);
    held_chunks_.insert(held_chunks_.end(), job.delete_chunks.begin(), job.delete_chunks.end());
    BX_LOG_INFO("a retention sweep is already waiting; the next one sweeps the timeline again");
  }
}

bool ObsHost::add_marker(const std::string& kind, const std::string& label, std::string& session_id,
                         SessionMarker& marker, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    if (session_.id.empty()) {
      error = "no session open";
      return false;
    }
    if (!session_store_add_marker(session_, kind, label, GetTickCount(), error)) return false;
    session_id = session_.id;
    marker = session_.markers.back();
  }
  if (on_marker_added) on_marker_added(session_id, marker);
  return true;
}

bool ObsHost::add_marker_at(const std::string& session_id, const std::string& kind, const std::string& label,
                            uint32_t at_tick, SessionMarker& marker, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    if (session_.id.empty() || session_.id != session_id) {
      error = "not the open session";
      return false;
    }
    if (!session_store_add_marker(session_, kind, label, at_tick, error)) return false;
    marker = session_.markers.back();
  }
  if (on_marker_added) on_marker_added(session_id, marker);
  return true;
}

bool ObsHost::remove_marker_at(const std::string& session_id, uint32_t at_tick, const std::string& kind,
                               const std::string& label, std::string& error) {
  std::lock_guard<std::mutex> lock(recording_mutex_);
  if (session_.id.empty() || session_.id != session_id) {
    error = "not the open session";
    return false;
  }
  return session_store_remove_marker(session_, at_tick, kind, label, error);
}

bool ObsHost::remove_marker_by_id(const std::string& session_id, const std::string& marker_id,
                                  std::string& error) {
  std::lock_guard<std::mutex> lock(recording_mutex_);
  if (session_.id.empty() || session_.id != session_id) {
    error = "not the open session";
    return false;
  }
  return session_store_remove_marker(session_, marker_id, error);
}

}  // namespace bx
