// Test-only WASAPI renderer. One process, one audio session, controllable from stdin:
//   start / stop / tone / silence / quit
// Used to prove the session watcher's events and the process-audio source's capture. It is never part
// of a package (tools/build.ps1 stages only the product binaries).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr uint32_t kRateFallback = 48000;
constexpr double kTwoPi = 6.283185307179586;

struct Renderer {
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  WAVEFORMATEX* format = nullptr;
  HANDLE render_event = nullptr;
  HANDLE quit_event = nullptr;
  std::atomic<bool> started{false};
  std::atomic<bool> tone{true};
  double frequency = 440.0;
  double phase = 0.0;

  bool open() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))) ||
        !enumerator) {
      std::fprintf(stderr, "fixture: no device enumerator\n");
      return false;
    }
    IMMDevice* device = nullptr;
    const HRESULT found = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(found) || !device) {
      std::fprintf(stderr, "fixture: no render endpoint\n");
      return false;
    }
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr) || !client) {
      std::fprintf(stderr, "fixture: audio client activation failed 0x%08lX\n", static_cast<unsigned long>(hr));
      return false;
    }
    hr = client->GetMixFormat(&format);
    if (FAILED(hr) || !format) return false;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
      std::fprintf(stderr, "fixture: the mix format is not extensible\n");
      return false;
    }
    render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 10000000, 0, format, nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(render_event);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
    if (FAILED(hr) || !render) {
      std::fprintf(stderr, "fixture: render client failed 0x%08lX\n", static_cast<unsigned long>(hr));
      return false;
    }
    return true;
  }

  void fill(UINT32 frames) {
    BYTE* buffer = nullptr;
    if (FAILED(render->GetBuffer(frames, &buffer)) || !buffer) return;
    const WORD channels = format->nChannels;
    const uint32_t rate = format->nSamplesPerSec ? format->nSamplesPerSec : kRateFallback;
    float* samples = reinterpret_cast<float*>(buffer);
    for (UINT32 frame = 0; frame < frames; ++frame) {
      float value = 0.0f;
      if (tone.load()) {
        value = static_cast<float>(0.05 * std::sin(phase));
        phase += kTwoPi * frequency / rate;
        if (phase > kTwoPi) phase -= kTwoPi;
      }
      for (WORD channel = 0; channel < channels; ++channel) samples[frame * channels + channel] = value;
    }
    render->ReleaseBuffer(frames, 0);
  }

  void run() {
    UINT32 buffer_frames = 0;
    if (FAILED(client->GetBufferSize(&buffer_frames)) || buffer_frames == 0) return;
    const HANDLE waits[] = {quit_event, render_event};
    while (true) {
      const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 50);
      if (waited == WAIT_OBJECT_0) break;
      if (!started.load()) continue;
      UINT32 padding = 0;
      if (FAILED(client->GetCurrentPadding(&padding))) continue;
      if (buffer_frames > padding) fill(buffer_frames - padding);
    }
  }

  void close() {
    if (client) {
      if (started.exchange(false)) client->Stop();
      client->Release();
    }
    if (render) render->Release();
    if (format) CoTaskMemFree(format);
    if (render_event) CloseHandle(render_event);
    if (quit_event) CloseHandle(quit_event);
  }
};

}  // namespace

int main(int argc, char** argv) {
  Renderer renderer;
  if (argc > 1 && std::strcmp(argv[1], "silence") == 0) renderer.tone = false;
  if (argc > 2) renderer.frequency = std::atof(argv[2]);
  if (renderer.frequency < 100.0 || renderer.frequency > 4000.0) renderer.frequency = 440.0;

  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
  if (!renderer.open()) return 2;
  renderer.quit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  renderer.started.store(true);
  if (FAILED(renderer.client->Start())) return 2;
  std::thread thread([&renderer] { renderer.run(); });
  std::printf("fixture ready pid %lu %s %.0f Hz\n", static_cast<unsigned long>(GetCurrentProcessId()),
              renderer.tone.load() ? "tone" : "silence", renderer.frequency);
  std::fflush(stdout);

  std::string line;
  while (true) {
    if (!std::getline(std::cin, line)) {
      // No stdin (a test starts it with no pipe): keep rendering until the test kills it.
      while (true) Sleep(1000);
    }
    if (line == "start") {
      if (!renderer.started.exchange(true)) renderer.client->Start();
    } else if (line == "stop") {
      if (renderer.started.exchange(false)) renderer.client->Stop();
    } else if (line == "tone") {
      renderer.tone.store(true);
    } else if (line == "silence") {
      renderer.tone.store(false);
    } else if (line.rfind("spawn ", 0) == 0) {
      // Test-only: a child process, so a probe can build a process tree whose descendant is excluded. The engine
      // refuses such a tree whole (it cannot subtract one descendant), and the interface has to say so.
      const std::string child = line.substr(6);
      STARTUPINFOA startup{};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION info{};
      if (CreateProcessA(child.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
        std::printf("spawned %lu\n", static_cast<unsigned long>(info.dwProcessId));
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
      } else {
        std::printf("spawn failed %lu\n", static_cast<unsigned long>(GetLastError()));
      }
      std::fflush(stdout);
    } else if (line == "quit") {
      break;
    }
  }

  renderer.started.store(false);
  if (renderer.quit_event) SetEvent(renderer.quit_event);
  thread.join();
  renderer.close();
  CoUninitialize();
  return 0;
}
