// Polls XInput for the controller hotkeys. One thread, only while a pad binding exists or the recorder
// is capturing: 16 ms while a pad is connected, 2 s while none is. It never takes the engine lock and
// never touches the window thread; an action goes to the same queue the window's hotkeys use.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gamepad.h"

namespace bx {

struct PadBinding {
  std::string action;
  uint32_t combo = 0;
};

class PadPoller {
 public:
  using ActionSink = std::function<void(const std::string& action)>;
  using CaptureSink = std::function<void(const std::string& chord)>;
  // Button edges for the interface's own navigation: the buttons that went down and the ones that came up.
  using NavSink = std::function<void(uint32_t pressed, uint32_t released)>;

  ~PadPoller();

  void set_action_sink(ActionSink sink);
  void set_capture_sink(CaptureSink sink);
  void set_nav_sink(NavSink sink);
  // The interface is on screen and one of its windows has the focus: the pads move it, whether or not a
  // hotkey is bound.
  void set_navigating(bool navigating);
  // Every configured pad binding, unbound ones left out. The thread starts on the first one and is
  // joined when the last one goes: an engine with no controller hotkey costs nothing.
  void set_bindings(std::vector<PadBinding> bindings);
  // While capturing the poller fires no action and reports the buttons held instead.
  void set_capturing(bool capturing);
  // Session locked or otherwise not wanted: the pads are not read at all.
  void set_paused(bool paused);

  int connected() const { return connected_.load(); }
  bool available() const { return available_.load(); }

  void shutdown();

 private:
  void ensure_started();
  void stop_if_idle();
  void run();

  mutable std::mutex mutex_;  // bindings_, capturing_, paused_, sinks
  std::vector<PadBinding> bindings_;
  ActionSink action_sink_;
  CaptureSink capture_sink_;
  NavSink nav_sink_;
  bool capturing_ = false;
  bool paused_ = false;
  bool navigating_ = false;
  // The buttons each pad held at its last read, so navigation sees edges and not a held state.
  uint32_t nav_last_[4] = {};

  std::mutex lifecycle_mutex_;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  void* wake_ = nullptr;  // HANDLE: ends a wait at once
  std::atomic<int> connected_{0};
  std::atomic<bool> available_{false};
};

}  // namespace bx
