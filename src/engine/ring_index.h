// The bookkeeping of BuffrX's replay ring, apart from libobs: which packets leave the front before a new one is added, and which are on the
// disk tier. A port of the replay buffer's rule (OBS 33.0.1, obs-ffmpeg-mux.c): packets leave a whole group of pictures at a time, never so
// many that fewer than two keyframes are left, and for two reasons - the time the ring holds (the length the user chose, plus a margin) and the
// bytes RAM may use (a safety ceiling, an eighth of the machine's memory, never a quality limit). Pure, so the rule is tested without libobs.
//
// The disk tier (ring_spill.h, ring_store.h): footage that RAM cannot hold is written, oldest first, to one reserved file in 8 MB blocks. The
// index keeps every record in arrival order. The first `disk_count_` are on disk (their payload is out of RAM), the next `staged_count_` have a
// place in the block stream but their block is not written yet (still in RAM), the rest are RAM only. With no disk the index is exactly the old
// one: `ram_bytes() == bytes()` and nothing is ever staged.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace bx {

constexpr int64_t kSpillBlockBytes = 8LL * 1024 * 1024;
// The fixed part of a record on disk (ring_spill.h encodes it); a record's footprint is this plus its payload.
constexpr int64_t kSpillRecordHeaderBytes = 48;

struct RingEntry {
  int64_t dts_usec = 0;
  uint32_t size = 0;
  bool video_keyframe = false;
  bool video = false;   // a video packet (else audio)
  uint8_t track = 0;    // the audio track
  bool staged = false;  // has a place in the block stream (disk_off); its block may not be written yet
  bool on_disk = false; // its last byte is in a written block and its payload is out of RAM
  int64_t disk_off = 0; // logical offset of the record's header in the ring file, valid when staged
  // Timeline dedupe (timeline_link.h): a record whose footage a Timeline chunk file already holds leaves RAM without being written.
  bool on_chunk = false;     // its payload is out of RAM and is in chunk `chunk_id`
  uint32_t chunk_id = 0;
  uint64_t video_seq = 0;    // a video packet's ordinal since the ring started; set by push()
};

// Footage a linked chunk holds: its video packets [seq0, seq1) and, for audio, the time [t0, t1) between the keyframes at both ends.
struct CoverRange {
  uint32_t id = 0;
  uint64_t seq0 = 0, seq1 = 0;
  int64_t t0 = 0, t1 = 0;
};

enum class SpillStep { none, staged, flipped, flush };

class RingIndex {
public:
  // How many entries leave the front before `incoming` is added. `max_time_us` is the longest span the ring holds, `max_bytes` the most RAM
  // it may use (0: no byte limit). The caller drops that many entries from its own store, in order, then calls push(). `removed_ram()` says how
  // many of them were RAM entries: only those have a payload to release.
  size_t room_for(const RingEntry& incoming, int64_t max_time_us, int64_t max_bytes) {
    removed_ram_ = 0;
    size_t removed = 0;
    if (max_bytes > 0) {
      if (entries_.empty() || keyframes_ <= 2) return 0;
      while (!entries_.empty() && ram_bytes_ + static_cast<int64_t>(incoming.size) > max_bytes) {
        // RAM is at its ceiling and the disk could not keep it down. Dropping the oldest RAM group would leave a hole between the disk part and
        // the rest, so the disk part goes as a whole (the last resort: the one path where the length chosen is not kept). Chunk entries are
        // part of it: they are the same prefix.
        if (nonram_count_ > 0) {
          removed += abandon_disk();
          continue;
        }
        removed += purge();
      }
    }
    if (entries_.empty() || keyframes_ <= 2) return removed;
    while (!entries_.empty() && incoming.dts_usec - first_dts_ > max_time_us) removed += purge();
    return removed;
  }

  void push(const RingEntry& entry) {
    if (entries_.empty()) first_dts_ = entry.dts_usec;
    RingEntry added = entry;
    added.staged = false;
    added.on_disk = false;
    added.on_chunk = false;
    added.chunk_id = 0;
    added.video_seq = entry.video ? next_video_seq_++ : 0;
    if (entry.video) ++video_count_;
    entries_.push_back(added);
    bytes_ += entry.size;
    ram_bytes_ += entry.size;
    if (entry.video_keyframe) ++keyframes_;
    last_dts_ = entry.dts_usec;
  }

  // The ordinals of the video packets held: [first_video_seq(), end_video_seq()).
  uint64_t first_video_seq() const { return next_video_seq_ - video_count_; }
  uint64_t end_video_seq() const { return next_video_seq_; }

  void clear() { *this = RingIndex(); }

  size_t packets() const { return entries_.size(); }
  // Payload bytes of everything held, on disk or not.
  int64_t bytes() const { return bytes_; }
  int keyframes() const { return keyframes_; }
  // The span the ring holds, in microseconds on the encoder's clock.
  int64_t span_us() const { return entries_.empty() ? 0 : last_dts_ - first_dts_; }
  size_t removed_ram() const { return removed_ram_; }

  // ---------------------------------------------------------------- the disk tier

  // The file is `file_bytes` long (a multiple of the block size); 0 leaves the ring RAM only.
  void enable_disk(int64_t file_bytes) { file_bytes_ = file_bytes; }
  bool disk_enabled() const { return file_bytes_ > 0 && !spill_failed_; }
  int64_t ram_bytes() const { return ram_bytes_; }
  // What the disk part takes in the file (payload plus headers).
  int64_t disk_bytes() const { return disk_footprint_; }
  size_t disk_count() const { return disk_count_; }
  size_t staged_count() const { return staged_count_; }
  int64_t spilled_total() const { return spilled_total_; }
  int backstop_events() const { return backstop_events_; }
  bool stalled() const { return stalled_; }
  bool spill_failed() const { return spill_failed_; }
  int64_t head() const { return head_; }
  int64_t flushed() const { return flushed_; }
  int64_t file_bytes() const { return file_bytes_; }

  // True when RAM holds more than `start_bytes` that has no place in the block stream yet.
  bool spill_due(int64_t start_bytes) const {
    return disk_enabled() && ram_bytes_ - staged_ram_bytes_ > start_bytes && nonram_count_ + staged_count_ < entries_.size();
  }

  // Gives the oldest RAM entry that has no place yet a place at the end of the block stream. `ram_position` is its position among the RAM
  // entries (the caller's packet store). False when nothing is due, or when the file has no room (`stalled()`): then the footage stays in RAM.
  bool next_to_stage(int64_t start_bytes, size_t& ram_position, int64_t& offset) {
    if (!spill_due(start_bytes)) return false;
    RingEntry& entry = entries_[nonram_count_ + staged_count_];
    const int64_t footprint = kSpillRecordHeaderBytes + static_cast<int64_t>(entry.size);
    if (head_ + footprint - floor() > file_bytes_) {
      stalled_ = true;
      return false;
    }
    stalled_ = false;
    entry.staged = true;
    entry.disk_off = head_;
    disk_offs_.push_back(head_);
    ram_position = staged_count_;
    offset = head_;
    head_ += footprint;
    ++staged_count_;
    staged_ram_bytes_ += entry.size;
    return true;
  }

  // ---------------------------------------------------------------- the chunk tier (Timeline dedupe)

  // The footage linked chunks hold, sorted by seq0 (and so by t0), ranges apart. Replaces the last list.
  void set_cover(std::vector<CoverRange> ranges) { cover_ = std::move(ranges); }
  const std::vector<CoverRange>& cover() const { return cover_; }

  // What spilling does with the oldest RAM entry that has no place yet: if a linked chunk holds its footage it leaves RAM without being written
  // (`flipped`: the caller releases one packet from the front of its RAM store); a covered entry behind staged ones cannot, because the part
  // that left RAM must stay a prefix, so the block is padded and written first (`flush`, then block_written); anything else is staged as
  // next_to_stage does. With no cover this is next_to_stage.
  SpillStep step_spill(int64_t start_bytes, size_t& ram_position, int64_t& offset) {
    if (!spill_due(start_bytes)) return SpillStep::none;
    if (!cover_.empty()) {
      RingEntry& entry = entries_[nonram_count_ + staged_count_];
      uint32_t id = 0;
      if (covered_by(entry, id)) {
        if (staged_count_ > 0) {
          // The padding writes up to a block edge: it must not reach what the file still holds (the tail, or what a save is reading).
          const int64_t edge = (head_ + kSpillBlockBytes - 1) / kSpillBlockBytes * kSpillBlockBytes;
          if (edge - floor() > file_bytes_) {
            stalled_ = true;
            return SpillStep::none;
          }
          return SpillStep::flush;
        }
        entry.on_chunk = true;
        entry.chunk_id = id;
        ram_bytes_ -= entry.size;
        chunk_bytes_ += entry.size;
        deduped_total_ += entry.size;
        ++chunk_count_;
        ++nonram_count_;
        return SpillStep::flipped;
      }
    }
    return next_to_stage(start_bytes, ram_position, offset) ? SpillStep::staged : SpillStep::none;
  }

  // The block writer padded its block: the next record starts at `head` (a block edge).
  void note_padded(int64_t head) { head_ = std::max(head_, head); }

  size_t nonram_count() const { return nonram_count_; }
  size_t chunk_count() const { return chunk_count_; }
  // Payload bytes whose footage is held by a chunk file instead of RAM or the ring file.
  int64_t chunk_bytes() const { return chunk_bytes_; }
  // Bytes that left RAM through a chunk since the ring started: what the spill file did not have to write.
  int64_t deduped_total() const { return deduped_total_; }

  // Blocks up to logical offset `upto` are written. The staged entries whose last byte is inside leave RAM; returns how many (the caller
  // releases that many packets from the front of its RAM store).
  size_t block_written(int64_t upto) {
    flushed_ = upto;
    size_t flipped = 0;
    while (staged_count_ > 0) {
      RingEntry& entry = entries_[nonram_count_];
      const int64_t footprint = kSpillRecordHeaderBytes + static_cast<int64_t>(entry.size);
      if (entry.disk_off + footprint > upto) break;
      entry.on_disk = true;
      ram_bytes_ -= entry.size;
      staged_ram_bytes_ -= entry.size;
      disk_footprint_ += footprint;
      spilled_total_ += footprint;
      ++disk_count_;
      ++nonram_count_;
      --staged_count_;
      ++flipped;
    }
    return flipped;
  }

  // A write failed: nothing more goes to disk. What was staged stays in RAM, unstaged; what is on disk stays readable until the ceiling.
  void fail_spill() {
    spill_failed_ = true;
    stalled_ = true;
    for (size_t i = 0; i < staged_count_; ++i) {
      entries_[nonram_count_ + i].staged = false;
      disk_offs_.pop_back();
    }
    staged_count_ = 0;
    staged_ram_bytes_ = 0;
  }

  // The oldest byte of the file that still matters: the first disk or staged entry, else the end of what was written. Chunk entries have none.
  int64_t tail() const { return disk_offs_.empty() ? head_ : disk_offs_.front(); }
  // Where the writer may not go: the tail, or earlier while a save reads the disk part.
  int64_t floor() const { return pin_ >= 0 ? std::min(pin_, tail()) : tail(); }

  // A save's view: a copy of every entry, and a pin on the oldest disk record so the writer cannot lap it while the save reads.
  std::vector<RingEntry> snapshot() {
    pin_ = disk_count_ > 0 ? disk_offs_.front() : -1;
    return std::vector<RingEntry>(entries_.begin(), entries_.end());
  }
  void release_pin() { pin_ = -1; }

private:
  // Drops the front entry and, when it was a video keyframe (the head of a group of pictures), everything up to the next keyframe.
  size_t purge() {
    size_t removed = 0;
    const bool was_keyframe = pop_front(removed);
    if (!was_keyframe) return removed;
    while (!entries_.empty() && !entries_.front().video_keyframe) pop_front(removed);
    return removed;
  }

  bool pop_front(size_t& removed) {
    const RingEntry gone = entries_.front();
    entries_.pop_front();
    ++removed;
    bytes_ -= gone.size;
    if (gone.on_disk) {
      --disk_count_;
      --nonram_count_;
      disk_offs_.pop_front();
      disk_footprint_ -= kSpillRecordHeaderBytes + static_cast<int64_t>(gone.size);
    } else if (gone.on_chunk) {
      --chunk_count_;
      --nonram_count_;
      chunk_bytes_ -= gone.size;
    } else {
      ram_bytes_ -= gone.size;
      ++removed_ram_;
      if (gone.staged) {
        --staged_count_;
        staged_ram_bytes_ -= gone.size;
        disk_offs_.pop_front();
      }
    }
    if (gone.video) --video_count_;
    if (gone.video_keyframe) --keyframes_;
    first_dts_ = entries_.empty() ? 0 : entries_.front().dts_usec;
    return gone.video_keyframe;
  }

  // The part that is out of RAM (disk and chunk entries) leaves the index (the file's bytes stay until a pin is released, so a save in flight keeps
  // valid data). What is left must begin at a keyframe, or the clip would start in the middle of a group.
  size_t abandon_disk() {
    size_t removed = 0;
    while (nonram_count_ > 0) pop_front(removed);
    ++backstop_events_;
    while (!entries_.empty() && !entries_.front().video_keyframe && keyframes_ > 0) pop_front(removed);
    return removed;
  }

  // The cover range that holds this entry's footage, by video seq or, for audio, by time.
  bool covered_by(const RingEntry& entry, uint32_t& id) const {
    if (entry.video) {
      auto next = std::upper_bound(cover_.begin(), cover_.end(), entry.video_seq, [](uint64_t seq, const CoverRange& range) { return seq < range.seq0; });
      if (next == cover_.begin()) return false;
      const CoverRange& range = *(next - 1);
      if (entry.video_seq >= range.seq1) return false;
      id = range.id;
      return true;
    }
    auto next = std::upper_bound(cover_.begin(), cover_.end(), entry.dts_usec, [](int64_t t, const CoverRange& range) { return t < range.t0; });
    if (next == cover_.begin()) return false;
    const CoverRange& range = *(next - 1);
    if (entry.dts_usec >= range.t1) return false;
    id = range.id;
    return true;
  }

  std::deque<RingEntry> entries_;
  std::deque<int64_t> disk_offs_;  // the offsets of the disk and staged entries, in order: the front is the tail of the file
  std::vector<CoverRange> cover_;
  uint64_t next_video_seq_ = 0;
  uint64_t video_count_ = 0;
  size_t nonram_count_ = 0;   // the entries at the front whose payload is out of RAM: on disk or in a chunk
  size_t chunk_count_ = 0;
  int64_t chunk_bytes_ = 0;
  int64_t deduped_total_ = 0;
  int64_t bytes_ = 0;
  int64_t ram_bytes_ = 0;
  int keyframes_ = 0;
  int64_t first_dts_ = 0, last_dts_ = 0;
  size_t removed_ram_ = 0;

  int64_t file_bytes_ = 0;
  size_t disk_count_ = 0;
  size_t staged_count_ = 0;
  int64_t staged_ram_bytes_ = 0;
  int64_t disk_footprint_ = 0;
  int64_t spilled_total_ = 0;
  int64_t head_ = 0, flushed_ = 0, pin_ = -1;
  int backstop_events_ = 0;
  bool stalled_ = false;
  bool spill_failed_ = false;
};

}  // namespace bx
