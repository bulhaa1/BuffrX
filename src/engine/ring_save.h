// The name of one replay save and the verdict it leaves for the host (ring_output.cpp). Pure: no file system, no
// libobs, so the naming that keeps two saves apart and the answer the host is given - a clip, a clip with a gap,
// or a failure - are checked without an engine (tests/ring_save_tests.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace bx {

// How many names one save may try before it gives up. A save that cannot own a name reports failure; it never falls
// back to a name another file holds.
constexpr int kSaveNameAttempts = 1000;

// The stem of the nth attempt at one save: the configured name the first time, then " (1)", " (2)" beside it.
// Two saves can land in the same second - the host throttles its own requests, but a salvage and a save, or two
// rings across a rebuild, do not - and a second muxer on the same path truncates the first save's file.
inline std::string save_attempt_stem(const std::string& stem, int attempt) {
  if (attempt <= 0) return stem;
  return stem + " (" + std::to_string(attempt) + ")";
}

// The path this save owns, or an empty string when no name could be taken. `claim(stem)` builds the whole path for one
// attempt and takes the name - the publication itself: it publishes the file by a move that never replaces an existing
// file (ring_publish.h), so a clip already on disk, a name another save in this process holds and a file another process
// made all refuse in the same way, the other writer keeps every byte it has, and a race between a check and a write
// cannot exist. It returns the path it owns, or empty when the name was refused.
//
// A save never reuses a name that was refused: writing a clip over a file nobody asked it to touch is worse than not
// writing one, so every refused attempt moves to the next counter and running out of attempts is a failure the host
// hears about.
template <class Claim>
inline std::string save_path_for(const std::string& stem, Claim&& claim, int attempts = kSaveNameAttempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    std::string path = claim(save_attempt_stem(stem, attempt));
    if (!path.empty()) return path;
  }
  return std::string();
}

// What one save came to, durable after the `saved` signal: the signal says a save ended, not that it worked.
struct SaveOutcome {
  uint64_t id = 0;        // a new number every save, so the host cannot read an older save's verdict
  // The file that holds this save's footage: the name it published, or - when no name could be freed, or the writer did
  // not finish - the staging file it stayed at, so a failed save's bytes are still named and the host can file them. A
  // failed writer's staged name carries ".part", which the library's listing drops, so a truncated clip is never offered
  // as a clip. Empty when nothing was written (no staged file could be made, or the writer produced no bytes), which is the
  // only case with no file at all.
  std::string path;
  bool finished = false;  // final before the signal: everything below can be read
  bool ok = false;        // the writer ended cleanly and every packet it could read reached it
  bool partial = false;   // the file is a clip but leaves footage out (only the newest part was kept)
  size_t skipped = 0;     // records the clip lacks
  bool unreadable = false;  // the gap came from the ring's disk part, not from chunk files that were gone
  int muxer_exit = 0;       // 0 clean, else the process's exit code; -1 when nothing was written at all
};

// The verdict of one save, from what the writer and the store reported. `output_written` false: nothing produced the
// file (no muxer process and no join), so there is no exit code to report. `muxer_exit` is the muxer's own exit code,
// 0 when the join wrote the clip from chunk files without a process, which is still a clip. The muxer's own exit code
// is the last word on the file: a disk that filled as the final fragment was written took every packet and still left
// a file that is not a clip.
inline SaveOutcome save_result(uint64_t id, std::string path, bool output_written, bool wrote_every_packet,
                               int muxer_exit, size_t skipped, bool unreadable, bool partial) {
  SaveOutcome result;
  result.id = id;
  result.path = std::move(path);
  result.finished = true;
  result.muxer_exit = output_written ? muxer_exit : -1;
  result.ok = output_written && wrote_every_packet && muxer_exit == 0;
  result.skipped = skipped;
  result.unreadable = unreadable;
  result.partial = partial;
  return result;
}

// The verdict of a save in parts (ring_output.cpp). `wrote` says an output was produced - a packet part's muxer ran,
// or the join wrote the chunk files, which starts no process at all - and `failed` says the clip was not produced.
// Reading a chunk-file-only save from whether a muxer process ran announced a good clip as a failure; `wrote` is the
// fact that matters, and a save that failed is still a failure however much of it ran.
inline SaveOutcome save_parts_result(uint64_t id, std::string path, bool wrote, bool failed, int muxer_exit,
                                     size_t skipped, bool unreadable, bool partial) {
  return save_result(id, std::move(path), wrote, !failed, muxer_exit, skipped, unreadable, partial);
}

// A clip the host may announce as saved.
inline bool save_produced_clip(const SaveOutcome& outcome) { return outcome.finished && outcome.ok; }

// A clip that exists but leaves footage out: it is kept and the person is told about the gap.
inline bool save_left_footage_out(const SaveOutcome& outcome) {
  return save_produced_clip(outcome) && (outcome.partial || outcome.skipped > 0 || outcome.unreadable);
}

}  // namespace bx
