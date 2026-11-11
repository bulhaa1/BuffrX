// What the replay output's `save_result` procedure said about the save its `saved` signal just ended, and what the
// host does with it. The signal says a save ended, not that it worked: a muxer that died leaves bytes behind, and
// those bytes are not a clip (ring_save.h has the verdict itself). Pure: no libobs, so the rules the host applies
// are checked without an engine (tests/engine_tests.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace bx {

// One save's verdict, as the output hands it over. `known` is false for an output that offers no `save_result`
// procedure at all (OBS's own replay buffer): it keeps the host on the older route, the path its `get_last_replay`
// procedure reports.
struct SavedReplayVerdict {
  bool known = false;
  bool finished = false;   // final before the signal
  bool ok = false;         // the writer ended cleanly and every packet it could read reached it
  bool partial = false;    // a clip, but it leaves footage out
  int64_t skipped = 0;     // records the clip lacks
  bool unreadable = false; // the gap came from the ring's disk part
  int muxer_exit = 0;      // 0 clean, else the process's exit code; -1 when nothing was written
  std::string path;        // where the output was written; empty when no name could be taken
};

enum class SavedReplayOutcome {
  file_clip,  // a clip was produced and named: file and announce it
  failed,     // a verdict says no clip was produced: keep any bytes, file and announce nothing
  unknown,    // no verdict: the caller uses get_last_replay, with the rules it had before
};

// A save is filed only when the output says it produced a clip and named the file. A failed muxer's partial file
// stays where it is, but it is never trimmed, moved, listed as a clip or announced as saved.
inline SavedReplayOutcome saved_replay_outcome(const SavedReplayVerdict& verdict) {
  if (!verdict.known) return SavedReplayOutcome::unknown;
  if (!verdict.finished || !verdict.ok || verdict.path.empty()) return SavedReplayOutcome::failed;
  return SavedReplayOutcome::file_clip;
}

// Whether a filed clip left footage out. The clip is still kept and filed: losing the part that was saved helps
// nobody, and the person is told about the gap.
inline bool saved_replay_left_footage_out(const SavedReplayVerdict& verdict) {
  return verdict.partial || verdict.skipped > 0 || verdict.unreadable;
}

// Why a filed clip is short. The verdict carries a count, not a reason: a missing record is a disk part that
// could not be read back, a chunk file that was gone, or a part the join refused, and only `unreadable` says the
// disk. Naming the disk for every count sent a reader to a disk that was fine.
enum class SavedReplayGap {
  none,
  unreadable_disk,  // the ring's own disk part could not be read: the one case a disk read is named
  skipped_records,  // records are missing and the verdict does not say why
  part_only,        // no count to explain: the clip is only part of the buffer
};

inline SavedReplayGap saved_replay_gap(const SavedReplayVerdict& verdict) {
  if (!saved_replay_left_footage_out(verdict)) return SavedReplayGap::none;
  if (verdict.unreadable) return SavedReplayGap::unreadable_disk;
  if (verdict.skipped > 0) return SavedReplayGap::skipped_records;
  return SavedReplayGap::part_only;
}

// What a gap is reported with. `log_reason` is the phrase the log adds (the count, when there is one, is written
// by the caller); the rest is the card for a person. Only `unreadable_disk` may say the buffer could not be read
// back from the disk; the others say what is known and no more, so a join the writer refused is not reported as a
// disk failing to read.
struct SavedReplayGapText {
  const char* log_reason = "";
  const char* title = "";
  const char* detail = "";
};

inline SavedReplayGapText saved_replay_gap_text(SavedReplayGap gap) {
  switch (gap) {
    case SavedReplayGap::unreadable_disk:
      return {"part of the replay ring's file could not be read",
              "Clip has a gap", "Part of the buffer could not be read back from the disk, so the clip skips a few seconds"};
    case SavedReplayGap::skipped_records:
      return {"part of the buffer is not in the clip",
              "Clip has a gap", "Part of the buffer is not in the clip, so it skips a few seconds"};
    case SavedReplayGap::part_only:
      return {"only part of the buffer was written", "Clip holds part of the buffer",
              "Only part of the buffer could be saved, so the clip is shorter than asked"};
    case SavedReplayGap::none:
      break;
  }
  return {};
}

// What a salvage may say once its own save has ended - never when it was only asked for. The save that a fault
// starts is announced from where it really finished: a muxer that died, a buffer restarted under it, or a file
// that did not verify says nothing here, because its failure was already reported and footage that is not a clip
// must never be called saved.
enum class SalvageResult {
  unverified,    // no clip the library can open: the failure is the whole report
  kept_unfiled,  // the bytes are in the clips folder but could not be filed under their game
  filed,         // verified and in its own folder under the library: the buffer is saved
};

struct SalvageNotice {
  const char* title = nullptr;
  const char* detail = nullptr;
};

inline SalvageNotice salvage_notice(bool salvage, SalvageResult result) {
  if (!salvage) return {};
  switch (result) {
    case SalvageResult::filed:
      return {"Buffer saved", "Capture failed, so your buffer was saved"};
    case SalvageResult::kept_unfiled:
      return {"Buffer kept, not filed", "Your buffer is in the clips folder, but it could not be filed under its game"};
    case SalvageResult::unverified:
      break;
  }
  return {};
}

}  // namespace bx
