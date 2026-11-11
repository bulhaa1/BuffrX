// What deserves a warn line in the engine log. The file keeps warn and above, and AGENTS.md's first next move is to read
// `slow command` and `window thread stalled` lines, so the routine ones must not bury them. Pure, so the rule is tested.
#pragma once

#include <cstdint>
#include <string>

namespace bx {

// FFmpeg notes that every thumbnail, export and share fit produces and that explain nothing about a failure: they go to the
// console at info, not into the file. A line that is not one of these keeps its warn.
inline bool ffmpeg_warning_is_routine(const std::string& text) {
  static const char* const kRoutine[] = {"deprecated pixel format", "frames left in the queue", "Changing video frame properties on the fly",
                                         "filter context - "};
  for (const char* needle : kRoutine) {
    if (text.find(needle) != std::string::npos) return true;
  }
  return false;
}

// A window message that starts Windows' own modal loop (WM_NCLBUTTONDOWN: a caption drag; WM_SYSCOMMAND: move or size): the
// time spent inside it is the person's hand on the window, not the engine stuck.
inline bool is_modal_loop_message(uint32_t message) { return message == 0x00A1 || message == 0x0112; }

// A main-loop pass minus the time the window spent in a modal loop, during which the heartbeat kept the tick running.
inline uint32_t stall_without_modal(uint32_t loop_ms, uint32_t modal_ms) { return loop_ms > modal_ms ? loop_ms - modal_ms : 0; }

}  // namespace bx
