// The audio sessions on a device, and who owns them.
//
// EarTrumpet (and Windows' own mixer) show the sessions that are on a device. Our own desktop audio
// capture opens a loopback client on the output device, and Windows shows that client as a source
// the user can mute or turn down - which is how "BuffrX appears in EarTrumpet" happens. This probe
// names every session on the default device so the question "who is that" has an answer.
//
//   buffrx-engine.exe --audio-sessions
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // the session manager with process information is Windows 8 and later
#endif

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../shared/log.h"

namespace bx {
namespace {

std::string process_name(DWORD pid) {
  if (pid == 0) return "system";
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return "unknown";
  wchar_t path[MAX_PATH]{};
  DWORD size = MAX_PATH;
  std::string name = "unknown";
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    std::string wide(path, path + size);
    const size_t slash = wide.find_last_of("\\/");
    name = slash == std::string::npos ? wide : wide.substr(slash + 1);
  }
  CloseHandle(process);
  return name;
}

const char* state_name(AudioSessionState state) {
  switch (state) {
    case AudioSessionStateInactive: return "inactive";
    case AudioSessionStateActive: return "active";
    case AudioSessionStateExpired: return "expired";
    default: return "unknown";
  }
}

}  // namespace

// One line per session on the default output device: the process, the state and whether the session
// is a capture (a loopback client is one).
std::vector<std::string> list_audio_sessions(std::string& error) {
  std::vector<std::string> lines;
  // The device enumerator is a COM object; the engine does not otherwise need COM.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialized = SUCCEEDED(com);
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator))) ||
      !enumerator) {
    error = "the audio device enumerator could not be created";
    if (uninitialized) CoUninitialize();
    return lines;
  }
  IMMDevice* device = nullptr;
  HRESULT found = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (FAILED(found) || !device) {
    enumerator->Release();
    error = "the default output device could not be found";
    return lines;
  }
  IAudioSessionManager2* manager = nullptr;
  if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr,
                              reinterpret_cast<void**>(&manager))) ||
      !manager) {
    device->Release();
    enumerator->Release();
    error = "the session manager could not be reached";
    return lines;
  }
  IAudioSessionEnumerator* sessions = nullptr;
  if (FAILED(manager->GetSessionEnumerator(&sessions)) || !sessions) {
    manager->Release();
    device->Release();
    enumerator->Release();
    error = "the sessions could not be listed";
    return lines;
  }
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
      system_sounds = control2->IsSystemSoundsSession() != FALSE;
      control2->Release();
    }
    char line[256];
    std::snprintf(line, sizeof(line), "session %d: pid %lu %s, %s%s", index, static_cast<unsigned long>(pid), process_name(pid).c_str(),
                  state_name(state), system_sounds ? ", system sounds" : "");
    lines.push_back(line);
    control->Release();
  }
  sessions->Release();
  manager->Release();
  device->Release();
  enumerator->Release();
  if (uninitialized) CoUninitialize();
  return lines;
}

}  // namespace bx
