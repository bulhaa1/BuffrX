// The spans of footage a Timeline session writes, one per file OBS's own splitter opens.
//
// A session is one `ffmpeg_muxer` output with `split_file` set: the muxer cuts a new file itself, on a
// keyframe, and says so through its `file_changed` signal. The signal fires on the output's thread inside
// libobs, before the new file's headers are written and before the old file is closed, so the engine cannot
// index anything from it. This is the small piece of bookkeeping in between: the tick begins a span when the
// session starts, changes it on each file the splitter reports (closing the previous one on the tick clock),
// and stops it when the output stops. Closed spans go to the session index once their writer has let go of
// the file (`file_writer_closed`, pending_chunk_publish).
//
// Kept free of libobs and of clocks so the rules are checked by tests: an empty or duplicate path is not a
// change, a span never ends before it started, and a change after a stop is nothing (the segment it belonged
// to is over).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bx {

struct TimelineSpan {
  std::string file;
  uint32_t start_ms = 0;  // the tick the span's first frame was written (or the tick it was asked for)
  uint32_t end_ms = 0;    // the tick the next span began, or the tick the session stopped
};

class TimelineFileIndex {
 public:
  // Starts the first span of a segment. False for an empty path, or while a span is already open.
  bool begin(const std::string& file, uint32_t now_ms) {
    if (file.empty() || active_) return false;
    current_ = TimelineSpan{file, now_ms, 0};
    active_ = true;
    return true;
  }

  // The splitter opened `next_file`: the open span closes at `now_ms` and the new one begins there. False for
  // an empty path, the open span's own path (two names for one file), or when nothing is open.
  bool change(const std::string& next_file, uint32_t now_ms) {
    if (next_file.empty() || !active_ || next_file == current_.file) return false;
    const uint32_t boundary = at_least(now_ms, current_.start_ms);
    current_.end_ms = boundary;
    closed_.push_back(current_);
    current_ = TimelineSpan{next_file, boundary, 0};
    return true;
  }

  // The output stopped: the open span closes at `now_ms`. False when nothing was open (a stop already taken,
  // or a change arriving after it).
  bool stop(uint32_t now_ms) {
    if (!active_) return false;
    current_.end_ms = at_least(now_ms, current_.start_ms);
    closed_.push_back(current_);
    current_ = TimelineSpan{};
    active_ = false;
    return true;
  }

  // The spans that have closed since the last call, oldest first; the caller now owns them.
  std::vector<TimelineSpan> take_closed() {
    std::vector<TimelineSpan> taken;
    taken.swap(closed_);
    return taken;
  }

  // The first frame an output writes is when its file really begins: a segment that joined an encoder already
  // running starts on the next keyframe, up to a keyframe interval after the tick that asked for it. Only ever
  // moves the open span's start later, so a closed span before it stays behind it.
  bool retime_current(uint32_t now_ms) {
    if (!active_ || now_ms <= current_.start_ms) return false;
    current_.start_ms = now_ms;
    return true;
  }

  const std::string& current() const { return current_.file; }
  uint32_t current_start_ms() const { return current_.start_ms; }
  bool active() const { return active_; }

 private:
  // GetTickCount wraps after 49 days; a boundary is never allowed to fall behind the span it closes.
  static uint32_t at_least(uint32_t now_ms, uint32_t floor_ms) {
    return static_cast<int32_t>(now_ms - floor_ms) < 0 ? floor_ms : now_ms;
  }

  bool active_ = false;
  TimelineSpan current_;
  std::vector<TimelineSpan> closed_;
};

}  // namespace bx
