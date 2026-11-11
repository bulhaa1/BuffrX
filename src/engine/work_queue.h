// A bounded hand-off for work that must not run on the tick: one worker, a queue with a ceiling.
//
// The engine trims and verifies a saved clip by remuxing the whole file. That takes seconds, and it
// used to be joined on the tick thread (obs_host.cpp: process_saved_replay), so a second clip saved
// while the first was still being trimmed froze the tick for the whole of the first trim — and with
// it the watchdog, the status broadcast and every hotkey. This is that hand-off: the tick enqueues
// and walks away, the worker takes one at a time, and the queue refuses rather than growing.
//
// Kept free of libobs and of the host so the policy — what is admitted, what is dropped, in what
// order — is unit-testable without an encoder, a capture source or a thread.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace bx {

// One item of work: whatever the worker needs, moved in and handed out once.
template <typename T>
class WorkQueue {
public:
  // `capacity` is the most items held at once. A trim is minutes of disk at worst, so a small queue
  // is deliberate: past it, the work is refused rather than buffered forever.
  explicit WorkQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  ~WorkQueue() { stop(); }

  WorkQueue(const WorkQueue&) = delete;
  WorkQueue& operator=(const WorkQueue&) = delete;

  // Hands `item` to the worker. False when the queue is full or stopping, in which case the caller
  // still owns `item` and is responsible for it (finishing a save the user asked for must not be
  // silently dropped, so callers report the refusal rather than ignoring it).
  bool push(T item) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || queue_.size() >= capacity_) return false;
      queue_.push_back(std::move(item));
    }
    ready_.notify_one();
    return true;
  }

  // Starts the worker. `run` is called once per item, on its own thread, until stop().
  template <typename Run>
  void start(Run run) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    // stop() joined the old worker and left the queue empty: a later start (engine.restart calls
    // shutdown then initialize) is a fresh worker, not a permanently stopped one.
    stopping_ = false;
    thread_ = std::thread([this, run] {
      for (;;) {
        T item;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
          if (stopping_ && queue_.empty()) return;
          if (queue_.empty()) continue;
          item = std::move(queue_.front());
          queue_.pop_front();
        }
        run(item);
      }
    });
  }

  // Stops accepting work, drains what is queued, then joins. This is the one place a wait belongs:
  // shutdown, where nothing else can be starved and the process is ending anyway.
  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;
      stopping_ = true;
    }
    ready_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
  }

  size_t pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  size_t capacity() const { return capacity_; }
  bool running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread_.joinable() && !stopping_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<T> queue_;
  std::thread thread_;
  size_t capacity_;
  bool stopping_ = false;
};

}  // namespace bx