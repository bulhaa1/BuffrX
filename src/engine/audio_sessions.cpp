// The audio sessions on the render endpoints, who owns them, and whether they are making sound.
//
// EarTrumpet (and Windows' own mixer) show the sessions that are on a device. Our own desktop audio
// capture opens a loopback client on the output device, and Windows shows that client as a source
// the user can mute or turn down - which is how "BuffrX appears in EarTrumpet" happens. This probe
// names every session on every active render output, with the session's own peak meter, so the app
// picker can offer what is making sound first without claiming that a running stream is audible.
//
// Nothing here creates a libobs source: reading a source's properties to enumerate is the crash trap
// this file exists to avoid.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // the session manager with process information is Windows 8 and later
#endif

#include "audio_sessions.h"

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../shared/log.h"

namespace bx {
namespace {

// MinGW's endpointvolume.h carries only a forward declaration of this interface, so its vtable is
// declared here; the IID is the one Windows documents for a session's own meter.
struct IAudioMeterInformation : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT count, float* peaks) = 0;
  virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};
const GUID kIidAudioMeter = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

std::string process_name(DWORD pid) {
  if (pid == 0) return "system";
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return "unknown";
  wchar_t path[MAX_PATH]{};
  DWORD size = MAX_PATH;
  std::string name = "unknown";
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    const std::wstring wide(path, size);
    const size_t slash = wide.find_last_of(L"\\/");
    const std::wstring leaf = slash == std::wstring::npos ? wide : wide.substr(slash + 1);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, leaf.data(), static_cast<int>(leaf.size()), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
      name.resize(static_cast<size_t>(bytes));
      WideCharToMultiByte(CP_UTF8, 0, leaf.data(), static_cast<int>(leaf.size()), name.data(), bytes, nullptr, nullptr);
    }
  }
  CloseHandle(process);
  return name;
}

std::string lower_ascii(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// One session, with the meter read while its objects are still alive.
void read_device_sessions(IMMDevice* device, std::vector<AudioSessionEntry>& into) {
  if (!device) return;
  IAudioSessionManager2* manager = nullptr;
  if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr,
                              reinterpret_cast<void**>(&manager))) ||
      !manager) {
    return;
  }
  IAudioSessionEnumerator* sessions = nullptr;
  if (SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && sessions) {
    int count = 0;
    sessions->GetCount(&count);
    for (int index = 0; index < count; ++index) {
      IAudioSessionControl* control = nullptr;
      if (FAILED(sessions->GetSession(index, &control)) || !control) continue;
      AudioSessionState state = AudioSessionStateInactive;
      control->GetState(&state);
      DWORD pid = 0;
      bool system_sounds = false;
      IAudioSessionControl2* control2 = nullptr;
      if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) && control2) {
        control2->GetProcessId(&pid);
        // S_OK means this is the system-sounds session; S_FALSE means it is not. Both are SUCCEEDED, so
        // testing against FALSE marks every ordinary session and hides the one that has no process.
        system_sounds = control2->IsSystemSoundsSession() == S_OK;
        control2->Release();
      }
      AudioSessionEntry entry;
      entry.pid = pid;
      entry.exe = process_name(pid);
      entry.active = state == AudioSessionStateActive;
      entry.system_sounds = system_sounds;
      // The session's own meter. Windows documents the endpoint's meter; the session object implements the
      // same interface for its own client on current Windows, which is what a per-app peak needs. When a
      // build does not, the peak stays unknown (-1) and the page never claims the app is silent.
      IAudioMeterInformation* meter = nullptr;
      if (SUCCEEDED(control->QueryInterface(kIidAudioMeter, reinterpret_cast<void**>(&meter))) && meter) {
        float peak = 0.0f;
        if (SUCCEEDED(meter->GetPeakValue(&peak))) entry.peak = peak;
        meter->Release();
      }
      control->Release();
      into.push_back(std::move(entry));
    }
    sessions->Release();
  }
  manager->Release();
}

// The snapshot is shared by every caller and at most 250 ms old: the picker asks four times a second and
// two pages asking at once must not enumerate twice.
std::mutex g_cache_mutex;
std::vector<AudioSessionEntry> g_cache;
uint32_t g_cache_at = 0;

bool snapshot_parts(std::vector<AudioSessionEntry>& out, std::string& error) {
  out.clear();
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // S_FALSE (already initialised on this apartment) must still be balanced by CoUninitialize.
  const bool initialised = SUCCEEDED(com);
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator))) ||
      !enumerator) {
    error = "the audio device enumerator could not be created";
    if (initialised) CoUninitialize();
    return false;
  }
  IMMDeviceCollection* devices = nullptr;
  if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) || !devices) {
    enumerator->Release();
    error = "the output devices could not be listed";
    if (initialised) CoUninitialize();
    return false;
  }
  UINT count = 0;
  devices->GetCount(&count);
  std::vector<AudioSessionEntry> raw;
  for (UINT index = 0; index < count; ++index) {
    IMMDevice* device = nullptr;
    if (FAILED(devices->Item(index, &device)) || !device) continue;
    read_device_sessions(device, raw);
    device->Release();
  }
  devices->Release();
  enumerator->Release();
  if (initialised) CoUninitialize();

  // One line per process: a program that plays to two outputs is one picker entry, and its loudest session
  // is what the page says about it.
  std::sort(raw.begin(), raw.end(), [](const AudioSessionEntry& a, const AudioSessionEntry& b) {
    return lower_ascii(a.exe) < lower_ascii(b.exe);
  });
  for (const AudioSessionEntry& entry : raw) {
    auto found = std::find_if(out.begin(), out.end(), [&](const AudioSessionEntry& other) {
      return !entry.exe.empty() && lower_ascii(other.exe) == lower_ascii(entry.exe);
    });
    if (found == out.end()) {
      out.push_back(entry);
      continue;
    }
    found->active = found->active || entry.active;
    if (entry.peak > found->peak) found->peak = entry.peak;
  }
  return true;
}

}  // namespace

std::vector<AudioSessionEntry> audio_session_snapshot(std::string& error) {
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    const uint32_t now = GetTickCount();
    if (g_cache_at != 0 && now - g_cache_at < 250) {
      error.clear();
      return g_cache;
    }
    std::string fresh_error;
    std::vector<AudioSessionEntry> fresh;
    const bool ok = snapshot_parts(fresh, fresh_error);
    if (!ok) {
      // A transient failure reports the last good list: the picker keeps its rows instead of blinking empty.
      error = fresh_error;
      std::vector<AudioSessionEntry> unavailable = g_cache;
      // The list can survive a failed read; its old peaks cannot prove that an app is making sound now.
      for (AudioSessionEntry& entry : unavailable) {
        entry.peak = -1.0f;
        entry.active = false;
      }
      return unavailable;
    }
    g_cache = fresh;
    g_cache_at = now;
    error.clear();
    return g_cache;
  }
}

std::vector<std::string> list_audio_sessions(std::string& error) {
  std::vector<std::string> lines;
  const std::vector<AudioSessionEntry> sessions = audio_session_snapshot(error);
  for (size_t index = 0; index < sessions.size(); ++index) {
    char line[256];
    char peak[32];
    if (sessions[index].peak >= 0.0f) std::snprintf(peak, sizeof(peak), "peak %.3f", sessions[index].peak);
    else std::snprintf(peak, sizeof(peak), "no meter");
    std::snprintf(line, sizeof(line), "session %zu: pid %lu %s, %s%s, %s", index,
                  static_cast<unsigned long>(sessions[index].pid), sessions[index].exe.c_str(),
                  sessions[index].active ? "active" : "inactive",
                  sessions[index].system_sounds ? ", system sounds" : "", peak);
    lines.push_back(line);
  }
  return lines;
}

}  // namespace bx
