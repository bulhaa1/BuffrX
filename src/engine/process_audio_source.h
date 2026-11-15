// A libobs audio source that captures one Windows process tree by process id.
//
// The OBS module that ships with the pinned release (`wasapi_process_output_capture`) finds its target
// through a top-level window and stops when that window disappears. This source takes the pid directly
// and asks Windows for a process-loopback client, so it needs no window and no hook. It is registered
// by the engine after libobs has started (register_process_audio_source) and never by a plugin DLL.
#pragma once

#include <cstdint>
#include <string>

struct obs_source;

namespace bx {

inline constexpr const char* kProcessAudioSourceId = "buffrx_process_audio";
inline constexpr const char* kProcessAudioPidSetting = "pid";
inline constexpr const char* kProcessAudioLabelSetting = "label";

// Whether the source's asynchronous activation actually produced a capture client. A non-null
// obs_source_create only means the definition exists; libobs cannot report here on its own.
struct ProcessAudioState {
  uint32_t pid = 0;
  bool ready = false;   // the capture client is started and delivering
  bool failed = false;  // terminal for this process (unsupported, refused, exited)
  std::string error;    // the HRESULT text or the reason, empty while working
};

// Idempotent. Call after obs_startup and before obs_source_create("buffrx_process_audio", ...).
void register_process_audio_source();

// The current state of a source made from this definition; empty when the pointer is not one of ours.
ProcessAudioState process_audio_state(obs_source* source);

}  // namespace bx
