// The audio sessions on the render endpoints, who owns them, and whether they are making sound.
#pragma once

#include <string>
#include <vector>

namespace bx {

// One session as Windows reports it. `active` is AudioSessionStateActive, which only means a stream is
// running: a silent app is active too. `peak` is the session's own meter (0..1), the only honest answer to
// "is this program making sound now"; it is -1 when the session exposes no meter.
struct AudioSessionEntry {
  unsigned long pid = 0;
  std::string exe;  // the process image's file name, empty when it cannot be read
  bool active = false;
  bool system_sounds = false;
  float peak = -1.0f;
};

// Every session on every active render endpoint, merged per process (the loudest session wins). Process
// loopback is not bound to one device, so the default endpoint alone would report a program as silent
// while it plays to another output. Cached for 250 ms: the picker asks four times a second, and two panels
// asking at once cost one enumeration.
std::vector<AudioSessionEntry> audio_session_snapshot(std::string& error);

// The same list as one line each, for `--audio-sessions` (who is that in EarTrumpet).
std::vector<std::string> list_audio_sessions(std::string& error);

}  // namespace bx
