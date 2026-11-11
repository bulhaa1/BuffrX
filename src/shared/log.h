// Logging for BuffrX: one file beside the data root (notes and failures, note and above, rotated to
// .1 on open past 1 MiB), plus stderr for everything when a console is attached.
//
// Levels exist because the engine's diagnostic value depends on being able to answer "why did that
// clip fail" without a debugger. The file is opened append-only and never rotated away within a run.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace bx {

// Note: an info line worth keeping in the file (a link made, a clip received and by which path).
enum class LogLevel { Debug, Info, Note, Warn, Error };

class Log {
public:
  // Never destroyed: the console writer thread may still be inside it at exit.
  static Log& instance() {
    static Log* log = new Log;
    return *log;
  }

  void open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) std::fclose(file_);
    file_ = nullptr;
    // Notes and failures, and bounded: the file keeps what explains a failure (note and above);
    // info and debug lines go to stderr only. On open, a file past 1 MiB is rotated to <path>.1
    // instead of deleted: what a crash left in it is exactly what the next run is being read for.
    if (FILE* probe = std::fopen(path.c_str(), "rb")) {
      std::fseek(probe, 0, SEEK_END);
      const long size = std::ftell(probe);
      std::fclose(probe);
      if (size > 1024 * 1024) {
        const std::string previous = path + ".1";
        std::remove(previous.c_str());
        std::rename(path.c_str(), previous.c_str());
      }
    }
    file_ = std::fopen(path.c_str(), "ab");
    path_ = path;
  }

  void write(LogLevel level, const char* format, ...) {
    char body[2048] = {};
    va_list args;
    va_start(args, format);
    const int needed = std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    // A line longer than the buffer is cut. The marker says so in the line itself, because a silent cut
    // made a stack or a path look complete when the rest of it was never written.
    const bool truncated = needed < 0 || needed >= static_cast<int>(sizeof(body));
    std::lock_guard<std::mutex> lock(mutex_);
    const char* tag = "info";
    switch (level) {
      case LogLevel::Debug: tag = "debug"; break;
      case LogLevel::Info: tag = "info"; break;
      case LogLevel::Note: tag = "note"; break;
      case LogLevel::Warn: tag = "warn"; break;
      case LogLevel::Error: tag = "error"; break;
    }
    std::string line;
    line.reserve(256);
    line += "[";
    line += timestamp();
    line += "] [";
    line += tag;
    line += "] ";
    line += body;
    if (truncated) line += " [truncated]";
    line += "\n";
    // The file is the failure record, and the notes that explain one; info and debug lines only ever
    // reach the console.
    if (file_ && level >= LogLevel::Note) {
      std::fwrite(line.data(), 1, line.size(), file_);
      std::fflush(file_);
    }
    // stderr is written by a thread of its own: a pipe nobody reads, or a console paused by a click,
    // blocks the write for good, and a write made here would hold this mutex and stop every thread
    // that logs - the window thread and the stall detector included (an AppHangB1 with a silent log).
    // The queue is bounded; a full one drops the new line.
    if (console_.size() < kConsoleLines) {
      console_.push_back(std::move(line));
      if (!console_thread_started_) start_console_thread();
      console_ready_.notify_one();
    } else {
      ++console_dropped_;
    }
  }

  const std::string& path() const { return path_; }

private:
  Log() = default;
  Log(const Log&) = delete;
  Log& operator=(const Log&) = delete;

  static constexpr size_t kConsoleLines = 512;

  // Called with mutex_ held.
  void start_console_thread() {
    console_thread_started_ = true;
    std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        console_ready_.wait(lock, [this] { return !console_.empty() || console_dropped_ > 0; });
        std::string text;
        if (console_dropped_ > 0) {
          text = "[log] " + std::to_string(console_dropped_) + " console lines dropped\n";
          console_dropped_ = 0;
        }
        while (!console_.empty()) {
          text += console_.front();
          console_.pop_front();
        }
        console_writing_ = true;
        lock.unlock();
        std::fputs(text.c_str(), stderr);
        lock.lock();
        console_writing_ = false;
        console_idle_.notify_all();
      }
    }).detach();
    // A probe reads the last lines a run prints: give the writer a moment at exit, never longer.
    std::atexit([] { instance().drain_console(std::chrono::milliseconds(300)); });
  }

  void drain_console(std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    console_idle_.wait_for(lock, wait, [this] { return console_.empty() && !console_writing_; });
  }

  static std::string timestamp() {
    // Local time, second resolution: enough to line up with Windows event timing, cheap enough
    // to call per line.
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
  }

  std::mutex mutex_;
  std::condition_variable console_ready_;
  std::condition_variable console_idle_;
  std::deque<std::string> console_;
  size_t console_dropped_ = 0;
  bool console_thread_started_ = false;
  bool console_writing_ = false;
  std::FILE* file_ = nullptr;
  std::string path_;
};

}  // namespace bx

#define BX_LOG_DEBUG(...) ::bx::Log::instance().write(::bx::LogLevel::Debug, __VA_ARGS__)
#define BX_LOG_INFO(...) ::bx::Log::instance().write(::bx::LogLevel::Info, __VA_ARGS__)
#define BX_LOG_NOTE(...) ::bx::Log::instance().write(::bx::LogLevel::Note, __VA_ARGS__)
#define BX_LOG_WARN(...) ::bx::Log::instance().write(::bx::LogLevel::Warn, __VA_ARGS__)
#define BX_LOG_ERROR(...) ::bx::Log::instance().write(::bx::LogLevel::Error, __VA_ARGS__)
