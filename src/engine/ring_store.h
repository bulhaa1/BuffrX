// The replay ring's contents, apart from libobs: the packets in RAM, the index that decides what leaves, the disk tier (ring_spill.h), and the
// save. `Ops` is how a packet is held: `Packet`, `add_ref`, `release`, and `fields` (the plain numbers and the payload). libobs's
// encoder_packet is one (ring_output.cpp); the tests' is another, so the whole ring, disk included, runs without libobs or a disk.
//
// Threads: the encoder's packet thread calls push(); the spill thread calls spill_step(); a save calls save() from the muxer's thread. One
// mutex guards the packets and the index; the writer belongs to the spill thread and the reader to the save, and neither is touched under
// the mutex, so a slow disk never holds up the encoder.
//
// The chunk tier (Timeline dedupe, timeline_link.h, ring_parts.h): when asked to (`configure_dedupe`), the store hashes every video keyframe it
// receives, and a Timeline chunk that has been probed and matched to the ring (`add_chunk`) holds footage the spill need not write: those records
// leave RAM without being written, and a save takes that part of the clip from the chunk files. Off by default; with it off nothing here runs.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "packet_hash.h"
#include "ring_index.h"
#include "ring_parts.h"
#include "ring_spill.h"
#include "timeline_link.h"

namespace bx {

struct StoreInfo {
  int64_t bytes = 0;         // payload held, in RAM or on disk
  int64_t ram_bytes = 0;
  int64_t disk_bytes = 0;    // what the disk part takes in the file
  int64_t spilled_total = 0; // written to the file since the ring started
  uint64_t total_bytes = 0;  // received since the ring started
  size_t packets = 0;
  int64_t span_us = 0;
  bool spill = false;        // a disk tier is configured and has not failed
  bool stalled = false;      // the disk could not take what was due: the footage waits in RAM
  bool spill_failed = false;
  int backstop_events = 0;   // times the disk part was given up at the RAM ceiling
  int64_t chunk_bytes = 0;   // payload held by chunk files instead of RAM or the ring file
  int64_t deduped_total = 0; // bytes that left RAM through a chunk since the ring started
  int chunks_linked = 0;     // chunks matched to the ring and in use
  int chunks_waiting = 0;    // probed chunks not matched (yet)
};

struct SaveReport {
  bool ok = false;         // every packet that could be read reached the sink
  size_t written = 0;
  size_t skipped = 0;      // packets left out because the disk could not be read (up to the next keyframe)
  bool unreadable = false; // some disk record could not be read: the clip has a gap
};

// A closed Timeline chunk as the probe read it (media.cpp), handed to the store to be matched against the ring.
struct ChunkProbe {
  std::string file;
  std::string session_id;           // the Timeline session the file belongs to (a save guards its read: timeline_retention.h)
  ChunkFacts facts;                 // video packet count, keyframes (index, hash, time), whether it follows the one before
  std::vector<double> key_time_s;   // where each of facts.keys is in the file, in seconds
};

template <class Ops>
class ReplayStore {
public:
  using Packet = typename Ops::Packet;

  struct DiskSetup {
    SpillBackend* io = nullptr;
    int64_t file_bytes = 0;      // a multiple of the block size
    int64_t ram_limit_bytes = 0;
    uint8_t* block = nullptr;    // aligned, kSpillBlockBytes: the writer's
    uint8_t* window_a = nullptr; // aligned, kSpillBlockBytes each: the reader's
    uint8_t* window_b = nullptr;
  };

  ~ReplayStore() { reset(); }

  void configure(int64_t max_time_us, int64_t ceiling_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_time_us_ = max_time_us;
    ceiling_bytes_ = ceiling_bytes;
  }

  // Turns the Timeline dedupe on: the video keyframes are hashed as they arrive. `other` leaves it off. Before the first packet.
  void configure_dedupe(HashCodec codec) { hash_codec_ = static_cast<int>(codec); }
  bool dedupe_enabled() const { return hash_codec_.load() != static_cast<int>(HashCodec::other); }

  void enable_disk(const DiskSetup& setup) {
    std::lock_guard<std::mutex> lock(mutex_);
    index_.enable_disk(setup.file_bytes);
    writer_ = std::make_unique<BlockWriter>(*setup.io, setup.block);
    reader_ = std::make_unique<DiskReader>(*setup.io, setup.window_a, setup.window_b);
    start_bytes_ = spill_start_bytes(setup.ram_limit_bytes);
    // A ceiling under two blocks is not one the disk part can work under: RAM passes it before a block is full enough to go out, and the ring would purge
    // footage without ever using the disk. The product's ceiling is never under 64 MiB; this keeps a smaller one from silently shortening the buffer.
    if (ceiling_bytes_ > 0) ceiling_bytes_ = std::max<int64_t>(ceiling_bytes_, 2 * kSpillBlockBytes);
  }

  // Everything released and the disk tier off: the ring starts again from nothing.
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Packet& packet : packets_) Ops::release(packet);
    packets_.clear();
    index_.clear();
    writer_.reset();
    reader_.reset();
    keys_.clear();
    probes_.clear();
    next_chunk_id_ = 1;
    linked_at_seq_ = 0;
    total_bytes_ = 0;
    due_ = false;
  }

  // Takes one reference the caller made (Ops::add_ref).
  void push(const Packet& packet) {
    const PacketFields fields = Ops::fields(packet);
    RingEntry entry;
    entry.dts_usec = fields.dts_usec;
    entry.size = fields.size;
    entry.video_keyframe = fields.video && fields.keyframe;
    entry.video = fields.video;
    entry.track = static_cast<uint8_t>(fields.track);
    // The hash of a keyframe is the only thing added to this thread, and it is computed before the lock: a keyframe is megabytes.
    VclDigest digest;
    const HashCodec codec = static_cast<HashCodec>(hash_codec_.load());
    if (entry.video_keyframe && codec != HashCodec::other) digest = vcl_digest(codec, fields.data, fields.size, 0);
    std::lock_guard<std::mutex> lock(mutex_);
    index_.room_for(entry, max_time_us_, ceiling_bytes_);
    for (size_t i = 0, drop = index_.removed_ram(); i < drop && !packets_.empty(); ++i) {
      Ops::release(packets_.front());
      packets_.pop_front();
    }
    const uint64_t seq = index_.end_video_seq();
    index_.push(entry);
    packets_.push_back(packet);
    total_bytes_ += fields.size;
    if (entry.video_keyframe && codec != HashCodec::other) {
      // A keyframe whose hash could not be taken still counts as a place in the ring; it matches nothing.
      keys_.push_back(RingKey{seq, fields.dts_usec, digest.ok ? digest.hash : 0, digest.ok ? digest.bytes : 0});
    }
    while (!keys_.empty() && keys_.front().seq < index_.first_video_seq()) keys_.pop_front();
    due_ = index_.spill_due(start_bytes_);
  }

  // The spill thread has something to do (or may: a disk that stalled is asked again).
  bool spill_due() const { return due_.load(); }

  // Writes what is due, a batch at a time. True when it did work, so the caller goes round again; false when nothing is due, the disk has no
  // room, or a write failed.
  bool spill_step() {
    constexpr size_t kBatch = 64;
    constexpr size_t kFlips = 256;
    std::vector<Packet> held;
    bool flush = false, flipped_any = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!writer_ || !index_.disk_enabled()) return false;
      relink_locked();
      size_t position = 0, flips = 0;
      int64_t offset = 0;
      while (held.size() < kBatch && flips < kFlips) {
        const SpillStep step = index_.step_spill(start_bytes_, position, offset);
        if (step == SpillStep::staged) {
          held.push_back(Ops::add_ref(packets_[position]));
        } else if (step == SpillStep::flipped) {
          Ops::release(packets_.front());
          packets_.pop_front();
          ++flips;
          flipped_any = true;
        } else {
          flush = step == SpillStep::flush;
          break;
        }
      }
      if (flipped_any) due_ = index_.spill_due(start_bytes_);
    }
    if (held.empty() && !flush) return flipped_any;
    bool ok = true;
    uint8_t header[kSpillRecordHeaderBytes];
    for (const Packet& packet : held) {
      if (!ok) break;
      const PacketFields fields = Ops::fields(packet);
      encode_record_header(fields, header);
      ok = writer_->append(header, sizeof(header)) && writer_->append(fields.data, fields.size);
    }
    for (Packet& packet : held) Ops::release(packet);
    // Records that go to the file ended and records that do not begin: the last block is padded and written so that what is staged can leave RAM.
    if (ok && flush) ok = writer_->pad();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok) {
      index_.fail_spill();
      due_ = false;
      return false;
    }
    if (flush) index_.note_padded(writer_->pos());
    const size_t flipped = index_.block_written(writer_->flushed());
    for (size_t i = 0; i < flipped && !packets_.empty(); ++i) {
      Ops::release(packets_.front());
      packets_.pop_front();
    }
    due_ = index_.spill_due(start_bytes_);
    return true;
  }

  StoreInfo info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    StoreInfo info;
    info.bytes = index_.bytes();
    info.ram_bytes = index_.ram_bytes();
    info.disk_bytes = index_.disk_bytes();
    info.spilled_total = index_.spilled_total();
    info.total_bytes = total_bytes_;
    info.packets = index_.packets();
    info.span_us = index_.span_us();
    info.spill = index_.disk_enabled();
    info.stalled = index_.stalled();
    info.spill_failed = index_.spill_failed();
    info.backstop_events = index_.backstop_events();
    info.chunk_bytes = index_.chunk_bytes();
    info.deduped_total = index_.deduped_total();
    for (const Probe& probe : probes_) {
      if (probe.link.linked) ++info.chunks_linked;
      else ++info.chunks_waiting;
    }
    return info;
  }

  // ---------------------------------------------------------------- the chunk tier

  // A probed chunk, oldest first. Returns its id. It is matched to the ring now or at the next spill step; it is not used until `set_chunk_usable`.
  uint32_t add_chunk(const ChunkProbe& probed) {
    std::lock_guard<std::mutex> lock(mutex_);
    Probe probe;
    probe.id = next_chunk_id_++;
    probe.file = probed.file;
    probe.session_id = probed.session_id;
    probe.facts = probed.facts;
    probe.key_time_s = probed.key_time_s;
    probe.born_seq = index_.end_video_seq();
    probes_.push_back(std::move(probe));
    linked_at_seq_ = 0;
    relink_locked();
    return probes_.back().id;
  }

  // The host has pinned the chunk against retention and found its file: the spill may now rely on it. False takes it back (no new records leave
  // RAM for it; the ones that did stay with it, and a save without the file skips them).
  void set_chunk_usable(uint32_t id, bool usable) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Probe& probe : probes_) {
      if (probe.id == id) probe.usable = usable;
    }
    rebuild_cover_locked();
  }

  struct ChunkState {
    uint32_t id = 0;
    std::string file;
    bool linked = false;
    bool usable = false;
    uint64_t at = 0;
    uint32_t count = 0;
  };
  // Every chunk still worth knowing about (not wholly behind the ring's front), for the host's pins and file checks.
  std::vector<ChunkState> chunk_states() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChunkState> out;
    for (const Probe& probe : probes_) out.push_back(ChunkState{probe.id, probe.file, probe.link.linked, probe.usable, probe.link.at, probe.facts.video_count});
    return out;
  }

  // Hands every packet to `sink(const PacketFields&)`, in the order a clip needs, each stream starting at zero. The sink returns false to
  // stop. The disk part is read by position; a record that cannot be read costs the packets up to the next video keyframe (a gap, said in
  // the report) rather than the whole clip.
  template <class Sink>
  SaveReport save(Sink&& sink) {
    std::unique_ptr<SaveJob> job = begin_save();
    if (snapshot_has_chunks(job->entries)) {
      std::vector<uint32_t> all(job->entries.size());
      for (uint32_t i = 0; i < all.size(); ++i) all[i] = i;
      return run_part(*job, all, sink);
    }
    return run_save(*job, sink);
  }

  // A save in two parts: the view of the ring is taken at once and the job is run later on the muxer's thread. The job holds its
  // own references to the RAM packets and a pin on the disk part until it is destroyed. `seconds` > 0 asks for only the last that
  // many seconds, cut at the video keyframe at or before them: the muxer then writes the clip once, at the length asked for, and
  // the file never has to be copied again to shorten it.
  struct SaveJob {
    std::vector<RingEntry> entries;
    std::vector<Packet> refs;
    std::map<uint32_t, ChunkRec> chunks;  // the chunk files the snapshot's chunk entries may be in
    ReplayStore* store = nullptr;
    SaveJob() = default;
    SaveJob(const SaveJob&) = delete;
    SaveJob& operator=(const SaveJob&) = delete;
    ~SaveJob() {
      for (Packet& packet : refs) Ops::release(packet);
      if (!store) return;
      std::lock_guard<std::mutex> lock(store->mutex_);
      store->index_.release_pin();
    }
  };

  std::unique_ptr<SaveJob> begin_save(int seconds = 0) {
    auto job = std::make_unique<SaveJob>();
    std::lock_guard<std::mutex> lock(mutex_);
    job->store = this;
    job->entries = index_.snapshot();
    // The part out of RAM (disk and chunk records, one prefix) is the oldest footage, so the cut below is usually inside it; what is left
    // of the RAM packets starts after it.
    const size_t out_of_ram = out_of_ram_prefix(job->entries);
    size_t first_entry = 0;
    if (seconds > 0 && !job->entries.empty()) {
      const int64_t want = job->entries.back().dts_usec - static_cast<int64_t>(seconds) * 1000000ll;
      for (size_t i = 0; i < job->entries.size(); ++i) {
        const RingEntry& entry = job->entries[i];
        if (entry.video && entry.video_keyframe && entry.dts_usec <= want) first_entry = i;
      }
      if (first_entry > 0) job->entries.erase(job->entries.begin(), job->entries.begin() + static_cast<std::ptrdiff_t>(first_entry));
    }
    const size_t ram_skip = first_entry > out_of_ram ? first_entry - out_of_ram : 0;
    job->refs.reserve(packets_.size() > ram_skip ? packets_.size() - ram_skip : 0);
    for (size_t i = ram_skip; i < packets_.size(); ++i) job->refs.push_back(Ops::add_ref(packets_[i]));
    for (const Probe& probe : probes_) {
      if (!probe.link.linked) continue;
      ChunkRec rec;
      rec.id = probe.id;
      rec.file = probe.file;
      rec.session_id = probe.session_id;
      rec.at = probe.link.at;
      rec.count = probe.facts.video_count;
      for (const ChunkKey& key : probe.facts.keys) rec.key_index.push_back(key.index);
      rec.key_time_s = probe.key_time_s;
      job->chunks[rec.id] = std::move(rec);
    }
    return job;
  }

  template <class Sink>
  SaveReport run_save(SaveJob& job, Sink&& sink) {
    SaveReport report;
    const std::vector<RingEntry>& entries = job.entries;
    std::vector<Packet>& refs = job.refs;

    const size_t out_of_ram = out_of_ram_prefix(entries);
    std::vector<uint8_t> payload;
    auto load = [&](size_t i, PacketFields& out, bool with_payload) {
      const RingEntry& entry = entries[i];
      if (!entry.on_disk) {
        out = Ops::fields(refs[i - out_of_ram]);
        return true;
      }
      uint8_t header[kSpillRecordHeaderBytes];
      if (!reader_ || !reader_->read(entry.disk_off, header, sizeof(header)) || !decode_record_header(header, out) || out.size != entry.size) return false;
      if (!with_payload) return true;
      payload.resize(out.size);
      if (out.size && !reader_->read(entry.disk_off + kSpillRecordHeaderBytes, payload.data(), out.size)) return false;
      out.data = payload.data();
      return true;
    };

    StreamOffsets offsets;
    for (size_t i = 0; i < entries.size(); ++i) {
      const bool known = entries[i].video ? offsets.has_video : offsets.has_audio[std::min<size_t>(entries[i].track, kMaxSpillTracks - 1)];
      PacketFields fields;
      if (!known && load(i, fields, false)) offsets.note(fields);
    }
    bool skipping = false;
    for (const uint32_t i : save_order(entries, offsets)) {
      const bool keyframe = entries[i].video && entries[i].video_keyframe;
      if (skipping && !keyframe) {
        ++report.skipped;
        continue;
      }
      PacketFields fields;
      if (!load(i, fields, true)) {
        skipping = true;
        report.unreadable = true;
        ++report.skipped;
        continue;
      }
      skipping = false;
      offsets.apply(fields);
      if (!sink(static_cast<const PacketFields&>(fields))) return report;
      ++report.written;
    }
    report.ok = true;
    return report;
  }

  // The plan of a save when the snapshot has chunk entries (ring_parts.h). `available(chunk)`: the file can be used.
  SavePlan plan_save(const SaveJob& job, const std::function<bool(const ChunkRec&)>& available) const {
    return plan_save_parts(job.entries, job.chunks, available);
  }

  // One packet part: the given entries (indices into the snapshot), each stream starting at zero, in the order a clip needs. A record that left RAM
  // for a chunk, or cannot be read from the disk, is unreadable: the packets up to the next keyframe are left out, as in run_save.
  template <class Sink>
  SaveReport run_part(SaveJob& job, const std::vector<uint32_t>& indices, Sink&& sink) {
    SaveReport report;
    const std::vector<RingEntry>& entries = job.entries;
    const size_t ram_start = out_of_ram_prefix(entries);
    std::vector<RingEntry> subset;
    subset.reserve(indices.size());
    for (const uint32_t i : indices) subset.push_back(entries[i]);
    std::vector<uint8_t> payload;
    // Sound that began before the first picture and was taken out for a chunk the ring has since left is not a gap in the picture: left out quietly.
    int64_t first_video_us = INT64_MAX;
    for (const RingEntry& entry : subset) {
      if (entry.video) first_video_us = std::min(first_video_us, entry.dts_usec);
    }
    auto load = [&](size_t k, PacketFields& out, bool with_payload) {
      const size_t i = indices[k];
      const RingEntry& entry = entries[i];
      if (entry.on_chunk) return false;
      if (!entry.on_disk) {
        out = Ops::fields(job.refs[i - ram_start]);
        return true;
      }
      uint8_t header[kSpillRecordHeaderBytes];
      if (!reader_ || !reader_->read(entry.disk_off, header, sizeof(header)) || !decode_record_header(header, out) || out.size != entry.size) return false;
      if (!with_payload) return true;
      payload.resize(out.size);
      if (out.size && !reader_->read(entry.disk_off + kSpillRecordHeaderBytes, payload.data(), out.size)) return false;
      out.data = payload.data();
      return true;
    };

    StreamOffsets offsets;
    for (size_t k = 0; k < subset.size(); ++k) {
      const bool known = subset[k].video ? offsets.has_video : offsets.has_audio[std::min<size_t>(subset[k].track, kMaxSpillTracks - 1)];
      PacketFields fields;
      if (!known && load(k, fields, false)) offsets.note(fields);
    }
    bool skipping = false;
    for (const uint32_t k : save_order(subset, offsets)) {
      const bool keyframe = subset[k].video && subset[k].video_keyframe;
      if (subset[k].on_chunk && !subset[k].video && subset[k].dts_usec < first_video_us) continue;
      if (skipping && !keyframe) {
        ++report.skipped;
        continue;
      }
      PacketFields fields;
      if (!load(k, fields, true)) {
        skipping = true;
        report.unreadable = true;
        ++report.skipped;
        continue;
      }
      skipping = false;
      offsets.apply(fields);
      if (!sink(static_cast<const PacketFields&>(fields))) return report;
      ++report.written;
    }
    report.ok = true;
    return report;
  }

private:
  // The entries at the front whose payload is out of RAM: on the disk tier or in a Timeline chunk. The index keeps them as one
  // prefix and `packets_` holds only what follows it, so this is how a snapshot's index maps to `packets_`/`refs`.
  static size_t out_of_ram_prefix(const std::vector<RingEntry>& entries) {
    size_t count = 0;
    while (count < entries.size() && (entries[count].on_disk || entries[count].on_chunk)) ++count;
    return count;
  }

  struct Probe {
    uint32_t id = 0;
    std::string file;
    std::string session_id;
    ChunkFacts facts;
    std::vector<double> key_time_s;
    ChunkLink link;
    bool usable = false;
    uint64_t born_seq = 0;  // the ring's end when the chunk was probed: its footage is older than this
  };

  // Matches the unmatched chunks against the ring's keyframes (timeline_link.h) and drops chunks the ring has moved wholly past. Under the lock.
  void relink_locked() {
    if (probes_.empty()) return;
    const uint64_t first = index_.first_video_seq(), end = index_.end_video_seq();
    // A matched chunk is done when the ring has left it; an unmatched one cannot be matched once the ring's front is past where the chunk was probed.
    probes_.erase(std::remove_if(probes_.begin(), probes_.end(),
                                 [&](const Probe& probe) {
                                   return probe.link.linked ? probe.link.at + probe.facts.video_count <= first : probe.born_seq < first;
                                 }),
                  probes_.end());
    if (end != linked_at_seq_) {
      std::vector<RingKey> keys(keys_.begin(), keys_.end());
      std::vector<ChunkFacts> facts;
      std::vector<ChunkLink> known;
      for (const Probe& probe : probes_) {
        facts.push_back(probe.facts);
        known.push_back(probe.link);
      }
      const std::vector<ChunkLink> found = link_chunks(keys, end, facts, known);
      for (size_t i = 0; i < probes_.size(); ++i) {
        if (!probes_[i].link.linked && found[i].linked) probes_[i].link = found[i];
      }
      linked_at_seq_ = end;
    }
    rebuild_cover_locked();
  }

  // The cover the index flips records by: linked, usable chunks, clipped to the ring's front.
  void rebuild_cover_locked() {
    std::vector<CoverRange> ranges;
    for (const Probe& probe : probes_) {
      if (!probe.link.linked || !probe.usable) continue;
      CoverRange range;
      range.id = probe.id;
      range.seq1 = probe.link.at + probe.facts.video_count;
      const RingKey* end_key = find_key_locked(range.seq1);
      if (!end_key) continue;
      range.t1 = end_key->dts_usec;
      const uint64_t first = keys_.empty() ? 0 : keys_.front().seq;
      if (probe.link.at >= first && !keys_.empty()) {
        const RingKey* start_key = find_key_locked(probe.link.at);
        if (!start_key) continue;
        range.seq0 = probe.link.at;
        range.t0 = start_key->dts_usec;
      } else if (!keys_.empty() && first < range.seq1) {
        range.seq0 = first;
        range.t0 = keys_.front().dts_usec;
      } else {
        continue;
      }
      ranges.push_back(range);
    }
    std::sort(ranges.begin(), ranges.end(), [](const CoverRange& a, const CoverRange& b) { return a.seq0 < b.seq0; });
    for (size_t i = 1; i < ranges.size();) {  // ranges that overlap are not trusted
      if (ranges[i].seq0 < ranges[i - 1].seq1) ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(i));
      else ++i;
    }
    index_.set_cover(std::move(ranges));
  }

  const RingKey* find_key_locked(uint64_t seq) const {
    for (const RingKey& key : keys_) {
      if (key.seq == seq) return &key;
      if (key.seq > seq) break;
    }
    return nullptr;
  }

  mutable std::mutex mutex_;
  std::deque<Packet> packets_;  // the RAM entries of the index, in the same order
  RingIndex index_;
  int64_t max_time_us_ = 0;
  int64_t ceiling_bytes_ = 0;
  int64_t start_bytes_ = 0;
  uint64_t total_bytes_ = 0;
  std::atomic<bool> due_{false};
  std::unique_ptr<BlockWriter> writer_;
  std::unique_ptr<DiskReader> reader_;
  std::atomic<int> hash_codec_{static_cast<int>(HashCodec::other)};
  std::deque<RingKey> keys_;      // the video keyframes held, with their hashes (dedupe only)
  std::vector<Probe> probes_;     // the probed chunks, oldest first
  uint32_t next_chunk_id_ = 1;
  uint64_t linked_at_seq_ = 0;    // the ring's end the chunks were last matched at
};

}  // namespace bx
