// Which protocol commands may run outside the engine lock, and what the engine may rebuild at once.
//
// Commands arrive on the pipe server's reader threads while the main thread ticks the same libobs
// objects (capture source, outputs, encoders) and builds the status broadcast. A command that touches
// them, or the protocol's config, runs under one engine-wide lock that the main thread also takes.
// The exceptions are the slow commands that only read and write files: holding the lock through a
// library scan or a range export would delay a hotkey save and a chunk rotation by seconds. The list is
// protocol_commands.h's table, beside the command names themselves.
//
// The lock is also what keeps two rebuilds from running at once. `ObsHost::apply` tears the pipeline
// down and builds it again — including obs_reset_video, which takes seconds — and it is reachable from
// a pipe thread, a hotkey, a profile switch and the tick. Two of those overlapping is the hang libobs
// cannot survive, so a rebuild that is already running makes the next one wait rather than start.
#pragma once

#include <atomic>
#include <mutex>

#include "protocol_commands.h"

namespace bx {

inline bool config_change_waits_for_recording(bool rebuilds, bool performance_changed, bool recording) {
  return (rebuilds || performance_changed) && recording;
}

// One rebuild at a time. Taken for the whole of ObsHost::apply, released by whoever took it, so a
// second rebuild arriving while the first is mid-teardown waits for it rather than interleaving with
// it: obs_reset_video against a half-destroyed pipeline is the hang this exists to prevent.
class RebuildGate {
public:
  void acquire() { mutex_.lock(); }
  void release() { mutex_.unlock(); }

  // True while a rebuild is running, for the status line and the diagnostics.
  bool busy() const { return flag_.load(std::memory_order_relaxed); }

  // The flag is only written while the mutex is held, so a reader that does not take it sees a
  // moment-stale answer - which is all a "is it rebuilding" indicator is for.
  void mark_busy(bool value) { flag_.store(value, std::memory_order_relaxed); }

private:
  std::mutex mutex_;
  std::atomic<bool> flag_{false};
};

// RAII wrapper that also publishes the flag a reader can see without taking the mutex.
class RebuildGuard {
public:
  explicit RebuildGuard(RebuildGate& gate) : gate_(gate) {
    gate_.acquire();
    gate_.mark_busy(true);
  }
  ~RebuildGuard() {
    gate_.mark_busy(false);
    gate_.release();
  }
  RebuildGuard(const RebuildGuard&) = delete;
  RebuildGuard& operator=(const RebuildGuard&) = delete;

private:
  RebuildGate& gate_;
};

}  // namespace bx
