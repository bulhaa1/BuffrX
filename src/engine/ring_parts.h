// A replay save in parts (Timeline dedupe, docs/review-timeline-mux-2026-11-02.md). Pure: it only reads the snapshot a save took.
//
// When part of the ring's footage lives in Timeline chunk files instead of memory or the ring file, a clip is made of two kinds of part, in time order:
// packet parts (the records the ring still has, written through the muxer exactly as a plain save does) and file parts (whole chunk files, joined by
// stream copy). Every seam is a video keyframe: a chunk starts on one and ends where the next group of pictures begins, and the packet part that follows
// starts at that keyframe, with the audio from that moment on.
//
// A chunk whose file is gone is not used; the records that were taken out of RAM for it are unreadable, and the existing rule applies: the packets up to
// the next keyframe are left out, the report says so. A snapshot with no chunk entries has no parts to plan: the save stays today's single pipe.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ring_index.h"

namespace bx {

// What the store knows about a linked chunk file.
struct ChunkRec {
  uint32_t id = 0;
  std::string file;
  std::string session_id;          // the Timeline session the file belongs to: a save guards its read (timeline_retention.h)
  uint64_t at = 0;                  // the ring seq of the chunk's first video packet
  uint32_t count = 0;               // its video packets
  std::vector<uint32_t> key_index;  // the video packet index of each keyframe in it
  std::vector<double> key_time_s;   // where each of those keyframes is in the file, in seconds
};

struct SavePart {
  bool file = false;
  std::vector<uint32_t> entries;  // a packet part: indices into the snapshot, in arrival order
  std::vector<uint32_t> chunks;   // a file part: chunk ids, oldest first
  double start_in_first = 0;      // a file part: where in the first chunk the clip begins (the keyframe the ring's front is on)
};

struct SavePlan {
  bool valid = false;  // false: the snapshot does not fit its chunks; save as a single pipe with chunk entries unreadable
  std::vector<SavePart> parts;
  size_t lost = 0;  // records taken out of RAM for a chunk whose file is gone: not in any part, so the clip has a gap there
  size_t file_parts() const {
    size_t n = 0;
    for (const SavePart& part : parts) n += part.file ? 1 : 0;
    return n;
  }
};

// The clip to save when the chunk files cannot be joined to the rest (a stream signature that differs): the newest packet part, which is one contiguous run
// of footage written exactly like a plain save. -1 when the plan has no packet part. Packet parts that are not adjacent are never glued together.
inline int newest_packet_part(const SavePlan& plan) {
  for (size_t i = plan.parts.size(); i > 0; --i) {
    if (!plan.parts[i - 1].file) return static_cast<int>(i - 1);
  }
  return -1;
}

inline bool snapshot_has_chunks(const std::vector<RingEntry>& entries) {
  for (const RingEntry& entry : entries) {
    if (entry.on_chunk) return true;
  }
  return false;
}

// `available(id)`: the chunk file can be used (it exists). An unavailable chunk's window is planned as ordinary records.
inline SavePlan plan_save_parts(const std::vector<RingEntry>& entries, const std::map<uint32_t, ChunkRec>& chunks,
                                const std::function<bool(const ChunkRec&)>& available) {
  SavePlan plan;
  struct Window {
    const ChunkRec* chunk = nullptr;
    uint64_t seq0 = 0, seq1 = 0;
    int64_t t0 = 0, t1 = 0;
    double start = 0;
  };
  // Time of every video seq in the snapshot.
  std::vector<int64_t> video_dts;
  uint64_t first_seq = 0;
  for (const RingEntry& entry : entries) {
    if (!entry.video) continue;
    if (video_dts.empty()) first_seq = entry.video_seq;
    video_dts.push_back(entry.dts_usec);
  }
  const uint64_t end_seq = first_seq + video_dts.size();
  const auto dts_at = [&](uint64_t seq) { return video_dts[static_cast<size_t>(seq - first_seq)]; };

  std::vector<const ChunkRec*> used;
  for (const RingEntry& entry : entries) {
    if (!entry.on_chunk) continue;
    const auto found = chunks.find(entry.chunk_id);
    if (found == chunks.end() || !available(found->second)) continue;
    if (std::find(used.begin(), used.end(), &found->second) == used.end()) used.push_back(&found->second);
  }
  std::sort(used.begin(), used.end(), [](const ChunkRec* a, const ChunkRec* b) { return a->at < b->at; });

  std::vector<Window> windows;
  for (const ChunkRec* chunk : used) {
    Window window;
    window.chunk = chunk;
    window.seq0 = std::max(chunk->at, first_seq);
    window.seq1 = chunk->at + chunk->count;
    if (video_dts.empty()) return plan;
    if (window.seq1 <= first_seq) continue;  // the ring has left the chunk's pictures; only sound that was taken out for it can remain, and that is dropped below
    if (window.seq0 >= window.seq1 || window.seq1 >= end_seq) return plan;
    if (!windows.empty() && window.seq0 < windows.back().seq1) return plan;
    window.t0 = dts_at(window.seq0);
    window.t1 = dts_at(window.seq1);
    if (window.seq0 > chunk->at) {
      const uint32_t index = static_cast<uint32_t>(window.seq0 - chunk->at);
      const auto at = std::find(chunk->key_index.begin(), chunk->key_index.end(), index);
      if (at == chunk->key_index.end()) return plan;
      window.start = chunk->key_time_s[static_cast<size_t>(at - chunk->key_index.begin())];
    }
    windows.push_back(window);
  }

  // Which window (index) holds an entry's footage, or -1; and, for an entry outside every window, which gap it falls in.
  const auto window_of = [&](const RingEntry& entry) -> int {
    for (size_t w = 0; w < windows.size(); ++w) {
      if (entry.video ? (entry.video_seq >= windows[w].seq0 && entry.video_seq < windows[w].seq1)
                      : (entry.dts_usec >= windows[w].t0 && entry.dts_usec < windows[w].t1)) {
        return static_cast<int>(w);
      }
    }
    return -1;
  };
  const auto gap_of = [&](const RingEntry& entry) {
    size_t gap = 0;
    for (const Window& window : windows) {
      if (entry.video ? entry.video_seq >= window.seq1 : entry.dts_usec >= window.t1) ++gap;
    }
    return gap;
  };

  std::vector<std::vector<uint32_t>> gaps(windows.size() + 1);
  std::vector<bool> gap_has_video(windows.size() + 1, false);
  for (uint32_t i = 0; i < entries.size(); ++i) {
    const RingEntry& entry = entries[i];
    const int window = window_of(entry);
    if (window >= 0) {
      // A record taken out of RAM for a chunk must lie in that chunk's window, or the plan would drop footage nobody holds.
      if (entry.on_chunk && entry.chunk_id != windows[static_cast<size_t>(window)].chunk->id) return plan;
      continue;
    }
    if (entry.on_chunk) {  // its chunk is not available: the record is gone, and the save says there is a gap
      if (!entry.video && !video_dts.empty() && entry.dts_usec < video_dts.front()) continue;  // sound from before the first picture: not a gap in the picture
      ++plan.lost;
      continue;
    }
    const size_t gap = gap_of(entry);
    gaps[gap].push_back(i);
    if (entry.video) gap_has_video[gap] = true;
  }
  // The seams: a packet part begins on a video keyframe.
  for (size_t g = 0; g < gaps.size(); ++g) {
    if (!gap_has_video[g] || g == 0) continue;
    for (uint32_t i : gaps[g]) {
      if (entries[i].video) {
        if (!entries[i].video_keyframe) return plan;
        break;
      }
    }
  }

  for (size_t g = 0; g <= windows.size(); ++g) {
    if (gap_has_video[g]) {
      SavePart part;
      part.entries = gaps[g];
      plan.parts.push_back(std::move(part));
    }
    if (g == windows.size()) break;
    const bool joins_previous = g > 0 && !plan.parts.empty() && plan.parts.back().file && !gap_has_video[g] && windows[g].seq0 == windows[g - 1].seq1;
    if (joins_previous) {
      plan.parts.back().chunks.push_back(windows[g].chunk->id);
    } else {
      SavePart part;
      part.file = true;
      part.chunks.push_back(windows[g].chunk->id);
      part.start_in_first = windows[g].start;
      plan.parts.push_back(std::move(part));
    }
  }
  plan.valid = true;
  return plan;
}

}  // namespace bx
