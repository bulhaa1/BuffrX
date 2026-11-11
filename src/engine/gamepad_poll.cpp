#include "gamepad_poll.h"

#include <windows.h>

#include <chrono>

#include "../shared/log.h"

namespace bx {
namespace {

// XInput's own layouts. Declared here so the poller needs no xinput header and loads whichever
// version the machine has.
struct XInputGamepad {
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  int16_t thumb_lx;
  int16_t thumb_ly;
  int16_t thumb_rx;
  int16_t thumb_ry;
};

struct XInputState {
  uint32_t packet;
  XInputGamepad pad;
};

using XInputGetStateFn = DWORD(WINAPI*)(DWORD index, XInputState* state);

constexpr size_t kPads = 4;
constexpr DWORD kConnectedWaitMs = 16;
// While the pad drives the interface the poll doubles its rate: an edge reaches a frame about 8 ms sooner.
constexpr DWORD kNavWaitMs = 4;
constexpr DWORD kScanWaitMs = 2000;
constexpr DWORD kPausedWaitMs = 250;

XInputGetStateFn load_xinput() {
  const char* const names[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
  for (const char* name : names) {
    HMODULE module = LoadLibraryA(name);
    if (!module) continue;
    FARPROC address = GetProcAddress(module, "XInputGetState");
    if (address) return reinterpret_cast<XInputGetStateFn>(address);
    FreeLibrary(module);
  }
  return nullptr;
}

uint64_t now_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

PadPoller::~PadPoller() { shutdown(); }

void PadPoller::set_action_sink(ActionSink sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  action_sink_ = std::move(sink);
}

void PadPoller::set_capture_sink(CaptureSink sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  capture_sink_ = std::move(sink);
}

void PadPoller::set_nav_sink(NavSink sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  nav_sink_ = std::move(sink);
}

void PadPoller::set_navigating(bool navigating) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    navigating_ = navigating;
  }
  ensure_started();
  stop_if_idle();
}

void PadPoller::set_bindings(std::vector<PadBinding> bindings) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bindings_ = std::move(bindings);
  }
  ensure_started();
  stop_if_idle();
}

void PadPoller::set_capturing(bool capturing) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    capturing_ = capturing;
  }
  ensure_started();
  stop_if_idle();
}

void PadPoller::set_paused(bool paused) {
  std::lock_guard<std::mutex> lock(mutex_);
  paused_ = paused;
}

void PadPoller::shutdown() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (!thread_.joinable()) {
    if (wake_) {
      CloseHandle(static_cast<HANDLE>(wake_));
      wake_ = nullptr;
    }
    return;
  }
  quit_ = true;
  if (wake_) SetEvent(static_cast<HANDLE>(wake_));
  thread_.join();
  quit_ = false;
  if (wake_) {
    CloseHandle(static_cast<HANDLE>(wake_));
    wake_ = nullptr;
  }
}

void PadPoller::ensure_started() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bindings_.empty() && !capturing_ && !navigating_) return;
  }
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (thread_.joinable()) return;
  if (!wake_) wake_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!wake_) return;
  // The event stays set from the last stop; a new thread would spin on it.
  ResetEvent(static_cast<HANDLE>(wake_));
  quit_ = false;
  thread_ = std::thread([this] { run(); });
}

void PadPoller::stop_if_idle() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bindings_.empty() || capturing_ || navigating_) return;
  }
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (!thread_.joinable()) return;
  quit_ = true;
  if (wake_) SetEvent(static_cast<HANDLE>(wake_));
  thread_.join();
  quit_ = false;
}

void PadPoller::run() {
  XInputGetStateFn get_state = load_xinput();
  available_ = get_state != nullptr;
  if (!get_state) BX_LOG_WARN("controller hotkeys are unavailable: XInput could not be loaded");

  PadRouter router;
  bool pad_connected[kPads] = {};
  uint32_t packet[kPads] = {};
  uint64_t next_scan[kPads] = {};
  uint32_t last_capture = 0;
  int connected = 0;

  while (!quit_) {
    std::vector<PadBinding> bindings;
    bool capturing = false;
    bool paused = false;
    bool navigating = false;
    ActionSink action_sink;
    CaptureSink capture_sink;
    NavSink nav_sink;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bindings = bindings_;
      capturing = capturing_;
      paused = paused_;
      navigating = navigating_;
      action_sink = action_sink_;
      capture_sink = capture_sink_;
      nav_sink = nav_sink_;
    }

    if (paused || (bindings.empty() && !capturing && !navigating)) {
      if (connected || router.captured()) {
        router.reset();
        for (size_t i = 0; i < kPads; ++i) {
          pad_connected[i] = false;
          packet[i] = 0;
          next_scan[i] = 0;
        }
        connected = 0;
        connected_ = 0;
      }
      // No edges from a stale hold: the interface lets go of what it held when it turns navigation off.
      for (size_t i = 0; i < kPads; ++i) nav_last_[i] = 0;
      last_capture = 0;
      WaitForSingleObject(static_cast<HANDLE>(wake_), kPausedWaitMs);
      continue;
    }

    std::vector<uint32_t> combos;
    combos.reserve(bindings.size());
    for (const PadBinding& binding : bindings) combos.push_back(binding.combo);

    const uint64_t now = now_ms();
    bool any_connected = connected > 0;
    if (get_state) {
      for (DWORD index = 0; index < kPads; ++index) {
        if (!pad_connected[index] && any_connected && now < next_scan[index]) continue;

        XInputState state = {};
        const DWORD result = get_state(index, &state);
        if (result == ERROR_SUCCESS) {
          if (!pad_connected[index]) {
            pad_connected[index] = true;
            packet[index] = state.packet;
            ++connected;
            connected_ = connected;
            any_connected = true;
            BX_LOG_INFO("controller %u connected", static_cast<unsigned>(index + 1));
          }
          if (state.packet != packet[index]) {
            packet[index] = state.packet;
            const uint32_t mask = pad_mask_from_xinput(state.pad.buttons, state.pad.left_trigger,
                                                        state.pad.right_trigger, router.pads[index].held);
            const uint32_t pressed = mask & ~nav_last_[index];
            const uint32_t released = nav_last_[index] & ~mask;
            nav_last_[index] = mask;
            // Tracked even while navigation is off, so a button already held when the panel takes focus
            // is not read as a fresh press.
            if (navigating && nav_sink && (pressed || released)) nav_sink(pressed, released);
            const std::vector<PadFire> fired = router.update(index, mask, combos, capturing, now);
            if (!capturing) {
              for (const PadFire& fire : fired) {
                if (fire.index < bindings.size() && action_sink) {
                  const std::string action = bindings[fire.index].action;
                  BX_LOG_INFO("controller hotkey: %s", action.c_str());
                  action_sink(action);
                }
              }
            }
          }
        } else if (pad_connected[index]) {
          pad_connected[index] = false;
          packet[index] = 0;
          next_scan[index] = now + kScanWaitMs;
          if (connected > 0) --connected;
          connected_ = connected;
          BX_LOG_INFO("controller %u disconnected", static_cast<unsigned>(index + 1));
          if (nav_last_[index] && nav_sink) nav_sink(0, nav_last_[index]);
          nav_last_[index] = 0;
          router.update(index, 0, combos, capturing, now);
        } else {
          next_scan[index] = now + kScanWaitMs;
        }
      }
    }

    if (capturing) {
      const uint32_t held = router.captured();
      if (held != last_capture) {
        last_capture = held;
        if (held && capture_sink) capture_sink(format_pad_chord(held));
      }
    } else {
      last_capture = 0;
    }

    // With the panel in front, a pad is looked for often enough that plugging one in feels immediate.
    const DWORD wait_ms = any_connected ? (navigating ? kNavWaitMs : kConnectedWaitMs)
                                        : (navigating ? kPausedWaitMs : kScanWaitMs);
    WaitForSingleObject(static_cast<HANDLE>(wake_), wait_ms);
  }

  connected_ = 0;
  router.reset();
}

}  // namespace bx
