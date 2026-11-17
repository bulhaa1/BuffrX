// Which programs are playing sound, from Windows' own audio session events.
//
// The picker used to enumerate every session four times a second and read its meter. That says what is
// audible but costs a device walk and a meter read on every ask, and it cannot say when a session
// appeared: a program that starts playing between two samples is a quarter second late. This watcher is
// event driven instead. A worker in an MTA apartment registers for render-endpoint notifications and for
// each session's own events, and the host asks for a snapshot only when the engine wakes.
//
// Nothing here is a libobs source and nothing polls IAudioMeterInformation. A session's `active` is
// AudioSessionStateActive: a stream is running. It does not mean the program is audible.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bx {

// The rule a snapshot applies to one session. Kept here (not in the .cpp) so it can be checked without
// Windows audio: an active session is always visible; an inactive one stays visible through `grace_ms`
// after it changed, so a brief pause does not tear a capture down and up again.
inline bool watched_session_visible(bool active, uint64_t changed_ms, uint64_t now_ms, uint32_t grace_ms) {
  if (active) return true;
  if (now_ms <= changed_ms) return true;  // a clock that stepped back must not hide everything
  return now_ms - changed_ms <= static_cast<uint64_t>(grace_ms);
}

// Inactive sessions can start again without a new session-created event. Keep their subscriptions until
// Windows expires them or their process exits, even after they disappear from the visible snapshot.
inline bool watched_session_remove(bool active, uint64_t changed_ms, bool process_exited) {
  return process_exited || (!active && changed_ms == 0);
}

inline bool watched_repair_due(uint64_t now_ms, uint64_t deadline_ms) {
  return deadline_ms != 0 && now_ms >= deadline_ms;
}

struct WatchedSession {
  // Stable per session instance (the session's instance identifier, or a process fallback). Two
  // sessions of one process have different keys.
  std::string key;
  uint32_t pid = 0;
  std::string exe;  // the process image's file name, empty when it cannot be read
  bool system_sounds = false;
  bool active = false;
  // GetTickCount64 when this session was first seen or last changed state. Monotonic, 64-bit.
  uint64_t changed_ms = 0;
};

// One worker thread owns the device and session COM objects. Callbacks post an immutable event and
// Nothing else: no registration, no libobs call and no host work runs on a COM thread.
class AudioSessionWatcher {
 public:
  using Wake = std::function<void()>;

  AudioSessionWatcher();
  ~AudioSessionWatcher();
  AudioSessionWatcher(const AudioSessionWatcher&) = delete;
  AudioSessionWatcher& operator=(const AudioSessionWatcher&) = delete;

  // Starts the worker and returns once it has bound the devices, so the first snapshot is meaningful.
  // False with error() set when COM or the device enumerator cannot be opened. Calling it while it runs
  // does nothing and returns true. A caller that must not block (the engine tick) uses begin and reads
  // ready() once it is set; start is begin plus that bounded wait, for tests and one-shot callers.
  bool start(Wake wake);
  // Starts the worker without waiting for it to bind the devices. False only when the thread or its wake
  // event cannot be created; true while binding is still in progress.
  bool begin(Wake wake);
  // True once begin's worker has finished binding (successfully or not), so sessions() and error() are
  // meaningful. A watcher that failed to start reports false here and error() says why.
  bool ready() const;
  // Stops the worker and joins it. Bounded: every wait the worker does is interrupted by the stop flag.
  void stop();
  bool running() const;

  // Every session visible under the grace rule. `now_ms` is GetTickCount64 from the caller's clock.
  std::vector<WatchedSession> sessions(uint64_t now_ms, uint32_t grace_ms = 1500) const;

  // Bumped whenever a session appears, disappears or changes state, so a host can skip a snapshot.
  uint64_t generation() const;
  std::string error() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace bx
