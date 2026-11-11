// Who holds the engine lock, and since when: what the hang detector in main.cpp reports when the
// window thread stops coming round.
//
// A slow command is logged when it finishes; a command that never finishes is not. The detector runs on
// its own thread and asks this instead, so a deadlock writes its own cause into the log while it is
// still happening ("the engine lock is held by game.state for 41 s").
#pragma once

#include <windows.h>

#include <mutex>
#include <string>

namespace bx {

class LockWatch {
public:
  void hold(const std::string& what) {
    std::lock_guard<std::mutex> lock(mutex_);
    holder_ = what;
    since_ = GetTickCount();
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    holder_.clear();
    since_ = 0;
  }
  // "game.state for 4100 ms", or "free".
  std::string describe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (holder_.empty()) return "free";
    return holder_ + " for " + std::to_string(GetTickCount() - since_) + " ms";
  }

  // Marks the lock held by `what` for as long as the scope lasts.
  struct Scope {
    Scope(LockWatch& watch, const std::string& what) : watch_(watch) { watch_.hold(what); }
    ~Scope() { watch_.release(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    LockWatch& watch_;
  };

private:
  mutable std::mutex mutex_;
  std::string holder_;
  uint32_t since_ = 0;
};

// The one watch for the engine lock.
inline LockWatch& engine_lock_watch() {
  static LockWatch watch;
  return watch;
}

}  // namespace bx
