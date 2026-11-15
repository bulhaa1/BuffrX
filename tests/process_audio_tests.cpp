// The native process-audio source against a real capture of our own headless renderer:
// activation actually succeeds, PCM reaches libobs, a retarget switches programs, a killed process
// ends the capture, and destroying the source joins its worker. Test-only; the fixture is never
// packaged.
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <obs.h>

#include "../src/engine/process_audio_source.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  std::printf("  FAIL %s\n", what);
  ++g_failures;
}

constexpr uint32_t kRate = 48000;
constexpr size_t kRing = kRate;  // one second of mono samples

// libobs hands each source's own audio here, before any mix. That is the source's PCM: the right
// place to count frames and measure the tone, without depending on the program mix.
struct Sink {
  mutable std::mutex mutex;
  uint64_t frames = 0;
  uint64_t packets = 0;
  double sum_squares = 0.0;
  uint64_t samples = 0;
  std::vector<float> ring = std::vector<float>(kRing, 0.0f);
  size_t position = 0;
  size_t filled = 0;

  void add(const float* plane, uint32_t count) {
    std::lock_guard<std::mutex> lock(mutex);
    ++packets;
    frames += count;
    for (uint32_t frame = 0; frame < count; ++frame) {
      const float value = plane[frame];
      sum_squares += static_cast<double>(value) * value;
      ++samples;
      ring[position] = value;
      position = (position + 1) % kRing;
      if (filled < kRing) ++filled;
    }
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex);
    frames = 0;
    packets = 0;
    sum_squares = 0.0;
    samples = 0;
    filled = 0;
    position = 0;
  }

  double rms() const {
    std::lock_guard<std::mutex> lock(mutex);
    return samples ? std::sqrt(sum_squares / static_cast<double>(samples)) : 0.0;
  }

  // Zero crossings over the stored window: a clean tone gives 2f crossings a second.
  double frequency() const {
    std::lock_guard<std::mutex> lock(mutex);
    if (filled < kRate / 4) return 0.0;
    int crossings = 0;
    float previous = ring[(position + kRing - filled) % kRing];
    for (size_t index = 1; index < filled; ++index) {
      const float value = ring[(position + kRing - filled + index) % kRing];
      if ((previous < 0.0f && value >= 0.0f) || (previous > 0.0f && value <= 0.0f)) ++crossings;
      previous = value;
    }
    return static_cast<double>(crossings) / (2.0 * static_cast<double>(filled) / kRate);
  }

  // Amplitude of one frequency in the stored window: a single-bin correlation, so a mix of two tones can
  // be asked which of them it carries (a zero-crossing count cannot).
  double tone(double hertz) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (filled < kRate / 4) return 0.0;
    double real = 0.0;
    double imaginary = 0.0;
    for (size_t index = 0; index < filled; ++index) {
      const double value = ring[(position + kRing - filled + index) % kRing];
      const double phase = 2.0 * 3.14159265358979323846 * hertz * static_cast<double>(index) / kRate;
      real += value * std::cos(phase);
      imaginary += value * std::sin(phase);
    }
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) / static_cast<double>(filled);
  }
};

void on_capture(void* param, obs_source_t*, const audio_data* data, bool muted) {
  Sink& sink = *static_cast<Sink*>(param);
  if (!data || !data->data[0] || data->frames == 0 || muted) return;
  // libobs's own format is planar float: plane 0 is channel 0.
  sink.add(reinterpret_cast<const float*>(data->data[0]), data->frames);
}

// The mixed output of one audio mix, after every source (a scene's bus included) has been mixed into it.
// A scene never calls obs_source_output_audio, so its bus is only visible here.
void on_mix(void* param, size_t, audio_data* data) {
  Sink& sink = *static_cast<Sink*>(param);
  if (!data || !data->data[0] || data->frames == 0) return;
  sink.add(reinterpret_cast<const float*>(data->data[0]), data->frames);
}

// Test-only sanity gate: the fixture's tone is really reaching the default render endpoint. This is a
// plain system loopback client, not the product's process capture; it separates "the fixture is silent"
// from "process loopback delivered nothing".
double system_loopback_rms_on_this_thread(uint32_t milliseconds) {
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator))) ||
      !enumerator) {
    return -1.0;
  }
  IMMDevice* device = nullptr;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
    enumerator->Release();
    return -1.0;
  }
  IAudioClient* client = nullptr;
  HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  device->Release();
  enumerator->Release();
  if (FAILED(hr) || !client) return -1.0;
  WAVEFORMATEX* format = nullptr;
  hr = client->GetMixFormat(&format);
  if (FAILED(hr) || !format) {
    client->Release();
    return -1.0;
  }
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  IAudioCaptureClient* capture = nullptr;
  if (SUCCEEDED(hr))
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0,
                            0, format, nullptr);
  if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
  if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr) || !capture) {
    if (capture) capture->Release();
    client->Release();
    CloseHandle(event);
    CoTaskMemFree(format);
    return -1.0;
  }
  double sum_squares = 0.0;
  uint64_t samples = 0;
  const uint64_t deadline = GetTickCount64() + milliseconds;
  while (GetTickCount64() < deadline) {
    WaitForSingleObject(event, 10);
    UINT32 next = 0;
    while (SUCCEEDED(capture->GetNextPacketSize(&next)) && next > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 position = 0;
      UINT64 timestamp = 0;
      if (FAILED(capture->GetBuffer(&data, &frames, &flags, &position, &timestamp))) break;
      if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
        const float* values = reinterpret_cast<const float*>(data);
        const size_t count = static_cast<size_t>(frames) * format->nChannels;
        for (size_t index = 0; index < count; ++index) {
          sum_squares += static_cast<double>(values[index]) * values[index];
          ++samples;
        }
      }
      capture->ReleaseBuffer(frames);
    }
  }
  client->Stop();
  capture->Release();
  client->Release();
  CloseHandle(event);
  CoTaskMemFree(format);
  return samples ? std::sqrt(sum_squares / static_cast<double>(samples)) : -1.0;
}

// On a thread of its own: libobs wants the main thread's COM apartment unchanged.
double system_loopback_rms(uint32_t milliseconds) {
  double result = -1.0;
  std::thread worker([&] {
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
      result = system_loopback_rms_on_this_thread(milliseconds);
      CoUninitialize();
    }
  });
  worker.join();
  return result;
}

std::wstring fixture_path() {
  wchar_t path[MAX_PATH * 2]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
  std::wstring full(path, length);
  const size_t slash = full.find_last_of(L"\\/");
  return (slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1)) +
         L"buffrx-audio-fixture.exe";
}

struct Fixture {
  HANDLE process = nullptr;
  uint32_t pid = 0;

  bool start(double hertz) {
    const std::wstring exe = fixture_path();
    wchar_t frequency[32];
    std::swprintf(frequency, sizeof(frequency) / sizeof(frequency[0]), L"%.0f", hertz);
    std::wstring command = L"\"" + exe + L"\" tone " + frequency;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &info)) {
      std::printf("  fixture did not start (%lu)\n", static_cast<unsigned long>(GetLastError()));
      return false;
    }
    CloseHandle(info.hThread);
    process = info.hProcess;
    pid = info.dwProcessId;
    return true;
  }

  void kill() {
    if (!process) return;
    TerminateProcess(process, 0);
    WaitForSingleObject(process, 3000);
    CloseHandle(process);
    process = nullptr;
  }

  ~Fixture() { kill(); }
};

bool wait_until(const std::function<bool()>& predicate, uint32_t timeout_ms) {
  const uint64_t deadline = GetTickCount64() + timeout_ms;
  while (GetTickCount64() < deadline) {
    if (predicate()) return true;
    Sleep(50);
  }
  return predicate();
}

bx::ProcessAudioState wait_ready(obs_source* source, uint32_t timeout_ms) {
  bx::ProcessAudioState state;
  wait_until(
      [&] {
        state = bx::process_audio_state(source);
        return state.ready || state.failed;
      },
      timeout_ms);
  return state;
}

obs_source* make_source(uint32_t pid, const char* name) {
  obs_data_t* settings = obs_data_create();
  obs_data_set_int(settings, bx::kProcessAudioPidSetting, static_cast<long long>(pid));
  obs_source_t* source = obs_source_create(bx::kProcessAudioSourceId, name, settings, nullptr);
  obs_data_release(settings);
  return source;
}

bool make_obs() {
  // The engine runs from bin/64bit and libobs finds its own effects, shaders and default data tree relative
  // to that directory. This test binary does not live there, so it takes the same working directory.
  wchar_t wide[MAX_PATH * 2]{};
  const int length = MultiByteToWideChar(CP_UTF8, 0, BUFFRX_SOURCE_DIR "/runtime/bin/64bit", -1, wide,
                                         static_cast<int>(sizeof(wide) / sizeof(wide[0])));
  if (length > 1) SetCurrentDirectoryW(wide);
  if (!obs_startup("en-US", nullptr, nullptr)) {
    std::printf("  obs_startup failed\n");
    return false;
  }
  // The product always resets video; a scene's audio is mixed through the main canvas's video mix, which
  // exists only after this call (obs_canvas_reset_video_internal sets the canvas's MIX_AUDIO flag). Without
  // it a bus of two trees would have no PCM to prove.
  obs_video_info video{};
  video.graphics_module = BUFFRX_SOURCE_DIR "/runtime/bin/64bit/libobs-d3d11.dll";
  video.fps_num = 60;
  video.fps_den = 1;
  video.base_width = 1280;
  video.base_height = 720;
  video.output_width = 1280;
  video.output_height = 720;
  video.output_format = VIDEO_FORMAT_NV12;
  video.adapter = 0;
  video.gpu_conversion = true;
  video.colorspace = VIDEO_CS_709;
  video.range = VIDEO_RANGE_PARTIAL;
  video.scale_type = OBS_SCALE_BILINEAR;
  if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) {
    std::printf("  obs_reset_video failed\n");
    return false;
  }
  obs_audio_info audio{};
  audio.samples_per_sec = kRate;
  audio.speakers = SPEAKERS_STEREO;
  if (!obs_reset_audio(&audio)) {
    std::printf("  obs_reset_audio failed\n");
    return false;
  }
  return true;
}

}  // namespace

int main() {
  std::printf("process audio source (live)\n");
  std::printf("  (quiet 440 Hz and 880 Hz tones play for about nine seconds in total)\n");

  Fixture first;
  expect(first.start(440.0), "the first fixture starts");
  if (!first.process) return 1;
  Sleep(400);

  const double system_rms = system_loopback_rms(1000);
  std::printf("  system loopback rms %.4f (the fixture must be audible there)\n", system_rms);
  expect(system_rms > 0.002, "the fixture's tone reaches the default render endpoint");

  if (!make_obs()) return 1;
  bx::register_process_audio_source();

  obs_data_t* settings = obs_data_create();
  obs_data_set_int(settings, bx::kProcessAudioPidSetting, first.pid);
  obs_source_t* source = obs_source_create(bx::kProcessAudioSourceId, "process audio test", settings, nullptr);
  obs_data_release(settings);
  expect(source != nullptr, "the source definition creates a source");
  if (!source) return 1;

  Sink sink;
  obs_source_add_audio_capture_callback(source, on_capture, &sink);
  obs_source_inc_active(source);

  const bx::ProcessAudioState state = wait_ready(source, 6000);
  expect(state.ready, "process loopback activation succeeds");
  expect(state.pid == first.pid, "the source reports the process it captures");
  if (!state.ready) {
    std::printf("  activation state: ready=%d failed=%d error=%s\n", static_cast<int>(state.ready),
                static_cast<int>(state.failed), state.error.c_str());
  }

  Sleep(2500);
  const uint64_t first_frames = sink.frames;
  const double first_rms = sink.rms();
  const double first_hz = sink.frequency();
  expect(first_frames > kRate, "PCM reaches libobs from the captured process");
  expect(first_rms > 0.002, "the PCM is the tone, not silence");
  expect(first_hz > 380.0 && first_hz < 500.0, "the first program's 440 Hz tone is what was captured");
  expect(!obs_source_audio_pending(source), "libobs accepted the source's audio packets");
  std::printf("  first: %llu frames, rms %.4f, %.0f Hz\n", static_cast<unsigned long long>(first_frames), first_rms,
              first_hz);

  Fixture second;
  expect(second.start(880.0), "the second fixture starts");
  Sleep(400);
  sink.reset();
  obs_data_t* retarget = obs_data_create();
  obs_data_set_int(retarget, bx::kProcessAudioPidSetting, second.pid);
  const uint64_t update_begin = GetTickCount64();
  obs_source_update(source, retarget);
  const uint64_t update_ms = GetTickCount64() - update_begin;
  obs_data_release(retarget);
  expect(update_ms < 3000, "a retarget returns once the old capture is stopped");

  const bx::ProcessAudioState retargeted = wait_ready(source, 6000);
  expect(retargeted.ready, "the retargeted capture activates");
  Sleep(2000);
  const double second_hz = sink.frequency();
  const double second_rms = sink.rms();
  expect(sink.frames > kRate / 2, "PCM reaches libobs from the second process");
  expect(second_rms > 0.002, "the second process is audible in the capture");
  expect(second_hz > 760.0 && second_hz < 1000.0, "the capture switched to the second program's 880 Hz tone");
  std::printf("  second: %.0f Hz, rms %.4f, retarget took %llu ms\n", second_hz, second_rms,
              static_cast<unsigned long long>(update_ms));

  // A killed process ends the capture without a hang; the source says why.
  second.kill();
  bx::ProcessAudioState ended;
  const bool ended_ok = wait_until(
      [&] {
        ended = bx::process_audio_state(source);
        return ended.failed;
      },
      5000);
  expect(ended_ok, "the source notices its process exit");
  expect(!ended.error.empty(), "and reports the reason");
  const uint64_t settled = sink.frames;
  Sleep(600);
  expect(sink.frames == settled, "no more PCM reaches libobs after the process exits");
  std::printf("  after exit: failed=%d error=%s\n", static_cast<int>(ended.failed), ended.error.c_str());

  obs_source_dec_active(source);
  obs_source_remove_audio_capture_callback(source, on_capture, &sink);
  const uint64_t destroy_begin = GetTickCount64();
  obs_source_release(source);
  const uint64_t destroy_ms = GetTickCount64() - destroy_begin;
  expect(destroy_ms < 3000, "destroying the source joins its worker promptly");
  std::printf("  destroy took %llu ms\n", static_cast<unsigned long long>(destroy_ms));

  // One bus, two disjoint process trees of one executable. The policy gives two browser profiles one row
  // and both of their trees (audio_auto.h); the host captures each tree with its own source inside one
  // private scene. Two copies of the same fixture at different tones prove the bus carries both, and each
  // tree's capture carries only its own sound (no tree recorded twice).
  Fixture twin;
  expect(twin.start(880.0), "a second fixture of the same executable starts");
  Sleep(400);
  obs_source_t* first_tree = make_source(first.pid, "lane tree 440");
  obs_source_t* twin_tree = make_source(twin.pid, "lane tree 880");
  expect(first_tree && twin_tree, "each tree of the executable gets its own capture");
  if (first_tree && twin_tree) {
    Sink first_sink;
    Sink twin_sink;
    obs_source_add_audio_capture_callback(first_tree, on_capture, &first_sink);
    obs_source_add_audio_capture_callback(twin_tree, on_capture, &twin_sink);
    obs_source_inc_active(first_tree);
    obs_source_inc_active(twin_tree);
    const bx::ProcessAudioState first_ready = wait_ready(first_tree, 6000);
    const bx::ProcessAudioState twin_ready = wait_ready(twin_tree, 6000);
    expect(first_ready.ready && twin_ready.ready, "both trees of the one lane activate");

    obs_scene_t* lane = obs_scene_create_private("process audio lane test");
    expect(lane != nullptr, "the lane's mixer is created");
    if (lane) {
      obs_sceneitem_t* first_item = obs_scene_add(lane, first_tree);
      obs_sceneitem_t* twin_item = obs_scene_add(lane, twin_tree);
      obs_source_t* bus = obs_scene_get_source(lane);
      Sink bus_sink;
      // The host connects a bus to a view channel; that is what makes libobs render the scene's audio, and
      // the raw mix callback is where a composite source's mixed output can be read.
      obs_set_output_source(0, bus);
      obs_source_inc_active(bus);
      obs_add_raw_audio_callback(0, nullptr, on_mix, &bus_sink);
      Sleep(2000);
      const double bus_440 = bus_sink.tone(440.0);
      const double bus_880 = bus_sink.tone(880.0);
      const double first_880 = first_sink.tone(880.0);
      const double twin_440 = twin_sink.tone(440.0);
      expect(bus_sink.frames > kRate, "the lane's bus delivers PCM");
      expect(bus_440 > 0.02 && bus_880 > 0.02, "the bus carries both trees of the one executable");
      expect(first_sink.tone(440.0) > 0.02 && first_880 < 0.01,
             "the first tree's capture carries only its own sound");
      expect(twin_sink.tone(880.0) > 0.02 && twin_440 < 0.01,
             "the second tree's capture carries only its own sound");
      std::printf("  lane: bus 440 %.4f / 880 %.4f, cross 880 %.4f / 440 %.4f\n", bus_440, bus_880, first_880,
                  twin_440);
      obs_remove_raw_audio_callback(0, on_mix, &bus_sink);
      obs_source_dec_active(bus);
      obs_set_output_source(0, nullptr);
      // The items go before the scene and the sources: a scene item holds a reference, and a source left
      // alive into obs_shutdown would be destroyed on libobs's teardown thread while its worker still runs.
      obs_sceneitem_remove(first_item);
      obs_sceneitem_remove(twin_item);
      obs_scene_release(lane);
    }
    obs_source_dec_active(first_tree);
    obs_source_dec_active(twin_tree);
    obs_source_remove_audio_capture_callback(first_tree, on_capture, &first_sink);
    obs_source_remove_audio_capture_callback(twin_tree, on_capture, &twin_sink);
    obs_source_release(first_tree);
    obs_source_release(twin_tree);
  }
  twin.kill();

  first.kill();
  obs_shutdown();

  if (g_failures == 0) std::printf("process audio source: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
