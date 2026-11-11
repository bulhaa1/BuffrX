// The disk tier of BuffrX's replay ring, apart from libobs and Win32: how much RAM the ring may hold before it spills, how big the reserved
// file is, the record a packet becomes on disk, the block writer and the windowed reader, and the order a save writes packets in. Pure, so
// every rule is tested with an in-memory disk that can stall or fail (ring_store.h puts the pieces together).
//
// The idea (docs/replay-memory-options.md, Option C): footage stays in RAM up to a limit; older footage goes, once, to one file reserved when
// the buffer starts, written in 8 MB unbuffered blocks, strictly in order. A save reads it back by position. Nothing is written while the
// footage fits in RAM, and nothing is ever dropped to fit memory: a disk that stalls leaves the footage in RAM.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ring_index.h"

namespace bx {

constexpr int64_t kRingRamBytes = 2048LL * 1024 * 1024;       // the most footage RAM holds before it spills
constexpr int64_t kRingRamFloorBytes = 128LL * 1024 * 1024;   // a small machine still gets this much, if its ceiling allows
constexpr int64_t kSpillMarginBytes = 64LL * 1024 * 1024;     // RAM beyond the spill start that absorbs a disk that stalls
constexpr int kMaxSpillTracks = 6;

// The RAM the ring may hold: 2 GiB, but never more than three quarters of the safety ceiling (an eighth of the machine's memory), so a small
// machine keeps room for a stalled disk, and never under 128 MiB unless the ceiling itself is lower.
inline int64_t ring_ram_limit_bytes(int64_t ceiling_bytes) {
  const int64_t limit = std::min(kRingRamBytes, ceiling_bytes / 4 * 3);
  return std::max(limit, std::min(kRingRamFloorBytes, ceiling_bytes));
}

// Spilling starts this far under the limit: 64 MiB, or half of a small limit. RAM can reach the start plus the block being filled plus one
// record before a block goes out, and past that the margin is where a stalled disk's footage waits.
inline int64_t spill_start_bytes(int64_t limit_bytes) { return limit_bytes - std::min(kSpillMarginBytes, limit_bytes / 2); }

// How big the reserved file is: the footage the buffer can hold at its peak rate (video plus every audio track, the length plus the ring's 2 s
// margin) minus what stays in RAM, plus slack for a save that reads while the writer goes on. 0 when the footage fits under the spill start.
// A rate control with no peak (CQP) has no bound, so it reserves for the slider's maximum, 200 Mbps. A multiple of the block size.
inline int64_t spill_reservation_bytes(int64_t peak_kbps, int64_t audio_kbps, int length_seconds, int64_t limit_bytes) {
  const int64_t video = peak_kbps > 0 ? peak_kbps : 200000;
  const int64_t bytes_per_second = (video + std::max<int64_t>(0, audio_kbps)) * 1000 / 8;
  const int64_t footage = bytes_per_second * (std::max(1, length_seconds) + 2);
  const int64_t start = spill_start_bytes(limit_bytes);
  if (footage <= start) return 0;
  const int64_t needed = footage - start + kSpillBlockBytes;
  const int64_t slack = std::max(needed / 100 * 15, bytes_per_second * 30) + 16LL * 1024 * 1024;
  const int64_t total = needed + slack;
  return (total + kSpillBlockBytes - 1) / kSpillBlockBytes * kSpillBlockBytes;
}

// ---------------------------------------------------------------- a packet, apart from libobs

struct PacketFields {
  int64_t pts = 0, dts = 0, dts_usec = 0;
  int32_t timebase_num = 1, timebase_den = 1;
  uint32_t track = 0;
  bool video = false, keyframe = false;
  const uint8_t* data = nullptr;
  uint32_t size = 0;
};

// The record a packet becomes on disk: kSpillRecordHeaderBytes of header, then the payload. Little-endian, as the machine is.
constexpr uint32_t kSpillMagic = 0x31525842;  // "BXR1"

inline void encode_record_header(const PacketFields& packet, uint8_t* out) {
  std::memset(out, 0, static_cast<size_t>(kSpillRecordHeaderBytes));
  const uint8_t video = packet.video ? 1 : 0, track = static_cast<uint8_t>(packet.track), key = packet.keyframe ? 1 : 0;
  std::memcpy(out + 0, &packet.size, 4);
  out[4] = video;
  out[5] = track;
  out[6] = key;
  std::memcpy(out + 8, &packet.pts, 8);
  std::memcpy(out + 16, &packet.dts, 8);
  std::memcpy(out + 24, &packet.dts_usec, 8);
  std::memcpy(out + 32, &packet.timebase_num, 4);
  std::memcpy(out + 36, &packet.timebase_den, 4);
  std::memcpy(out + 40, &kSpillMagic, 4);
}

// False when the bytes are not a record (wrong magic): the file was overwritten or never written there.
inline bool decode_record_header(const uint8_t* in, PacketFields& packet) {
  uint32_t magic = 0;
  std::memcpy(&magic, in + 40, 4);
  if (magic != kSpillMagic) return false;
  packet = PacketFields{};
  std::memcpy(&packet.size, in + 0, 4);
  packet.video = in[4] != 0;
  packet.track = in[5];
  packet.keyframe = in[6] != 0;
  std::memcpy(&packet.pts, in + 8, 8);
  std::memcpy(&packet.dts, in + 16, 8);
  std::memcpy(&packet.dts_usec, in + 24, 8);
  std::memcpy(&packet.timebase_num, in + 32, 4);
  std::memcpy(&packet.timebase_den, in + 36, 4);
  return true;
}

// ---------------------------------------------------------------- the file

// One reserved file seen as a circle of blocks. `logical` offsets count up forever; the file position is `logical % size`, and the size is a
// multiple of the block, so a block never straddles the end. Both calls move exactly one aligned block.
class SpillBackend {
public:
  virtual ~SpillBackend() = default;
  virtual bool write_block(int64_t logical_offset, const uint8_t* data) = 0;
  virtual bool read_block(int64_t logical_offset, uint8_t* destination) = 0;
};

// Appends the record stream and writes each block the moment it is full. The writes go in strict order from offset 0: NTFS zero-fills any gap
// between the end of what was written and a write that skips ahead, which would be silent extra wear.
class BlockWriter {
public:
  // `block` is one aligned block of memory the writer owns the use of.
  BlockWriter(SpillBackend& io, uint8_t* block) : io_(io), block_(block) {}

  bool append(const uint8_t* data, size_t count) {
    if (failed_) return false;
    while (count > 0) {
      const size_t take = std::min(count, static_cast<size_t>(kSpillBlockBytes) - fill_);
      std::memcpy(block_ + fill_, data, take);
      fill_ += take;
      data += take;
      count -= take;
      pos_ += static_cast<int64_t>(take);
      if (fill_ == static_cast<size_t>(kSpillBlockBytes)) {
        if (!io_.write_block(flushed_, block_)) {
          failed_ = true;
          return false;
        }
        flushed_ += kSpillBlockBytes;
        fill_ = 0;
      }
    }
    return true;
  }
  // Fills the rest of a part-written block with zeros and writes it, so the next record starts on a block edge. The Timeline dedupe needs it where
  // a run of records that goes to the file ends and a run that does not begins: the records of the last block must be on disk before the ones
  // after them can leave RAM. At most one block of extra writing, once per switch. Nothing to do on an edge.
  bool pad() {
    if (failed_) return false;
    if (fill_ == 0) return true;
    std::memset(block_ + fill_, 0, static_cast<size_t>(kSpillBlockBytes) - fill_);
    pos_ += static_cast<int64_t>(static_cast<size_t>(kSpillBlockBytes) - fill_);
    fill_ = static_cast<size_t>(kSpillBlockBytes);
    if (!io_.write_block(flushed_, block_)) {
      failed_ = true;
      return false;
    }
    flushed_ += kSpillBlockBytes;
    fill_ = 0;
    return true;
  }
  int64_t pos() const { return pos_; }          // the end of what was appended
  int64_t flushed() const { return flushed_; }  // the end of what is written
  bool failed() const { return failed_; }

private:
  SpillBackend& io_;
  uint8_t* block_;
  size_t fill_ = 0;
  int64_t pos_ = 0, flushed_ = 0;
  bool failed_ = false;
};

// Reads bytes by logical position through two cached blocks (a save asks for the audio and the video of the same moment in turn, which sit
// close together, often either side of a block edge).
class DiskReader {
public:
  DiskReader(SpillBackend& io, uint8_t* window_a, uint8_t* window_b) : io_(io) {
    window_[0] = window_a;
    window_[1] = window_b;
  }

  bool read(int64_t offset, uint8_t* destination, size_t count) {
    while (count > 0) {
      const int64_t base = offset - offset % kSpillBlockBytes;
      const uint8_t* window = fetch(base);
      if (!window) return false;
      const size_t inside = static_cast<size_t>(offset - base);
      const size_t take = std::min(count, static_cast<size_t>(kSpillBlockBytes) - inside);
      std::memcpy(destination, window + inside, take);
      destination += take;
      offset += static_cast<int64_t>(take);
      count -= take;
    }
    return true;
  }

private:
  const uint8_t* fetch(int64_t base) {
    for (int i = 0; i < 2; ++i) {
      if (base_[i] == base) {
        newest_ = i;
        return window_[i];
      }
    }
    const int slot = 1 - newest_;
    base_[slot] = -1;
    if (!io_.read_block(base, window_[slot])) return nullptr;
    base_[slot] = base;
    newest_ = slot;
    return window_[slot];
  }

  SpillBackend& io_;
  uint8_t* window_[2];
  int64_t base_[2] = {-1, -1};
  int newest_ = 0;
};

// ---------------------------------------------------------------- the order of a save

// Each stream of a clip starts at zero: the video at its first picture's time, each audio track at its own first packet's.
struct StreamOffsets {
  bool has_video = false;
  int64_t video_pts = 0;  // in the video packets' time base
  int64_t video_us = 0;
  bool has_audio[kMaxSpillTracks] = {};
  int64_t audio_dts[kMaxSpillTracks] = {};
  int64_t audio_us[kMaxSpillTracks] = {};

  void note(const PacketFields& packet) {
    if (packet.video) {
      if (has_video) return;
      has_video = true;
      video_pts = packet.pts;
      video_us = packet.timebase_den ? packet.pts * 1000000 / packet.timebase_den : 0;
    } else {
      const size_t track = std::min<size_t>(packet.track, kMaxSpillTracks - 1);
      if (has_audio[track]) return;
      has_audio[track] = true;
      audio_dts[track] = packet.dts;
      audio_us[track] = packet.dts_usec;
    }
  }
  int64_t microseconds_for(bool video, uint32_t track) const { return video ? video_us : audio_us[std::min<size_t>(track, kMaxSpillTracks - 1)]; }
  void apply(PacketFields& packet) const {
    if (packet.video) {
      packet.dts_usec -= video_us;
      packet.dts -= video_pts;
      packet.pts -= video_pts;
    } else {
      const size_t track = std::min<size_t>(packet.track, kMaxSpillTracks - 1);
      packet.dts_usec -= audio_us[track];
      packet.dts -= audio_dts[track];
      packet.pts -= audio_dts[track];
    }
  }
};

// The order a save writes the entries in: ascending time on the clip's own clock, and entries of equal time in reverse arrival order. That is
// what the replay buffer's insertion sort did (`while (out[at - 1].dts_usec >= packet.dts_usec) --at`), and the clip bytes must not change.
inline std::vector<uint32_t> save_order(const std::vector<RingEntry>& entries, const StreamOffsets& offsets) {
  std::vector<int64_t> key(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) key[i] = entries[i].dts_usec - offsets.microseconds_for(entries[i].video, entries[i].track);
  std::vector<uint32_t> order(entries.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<uint32_t>(i);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key[a] != key[b] ? key[a] < key[b] : a > b; });
  return order;
}

}  // namespace bx
