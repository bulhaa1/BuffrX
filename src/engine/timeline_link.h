// Which ring packets a Timeline chunk file holds (docs/review-timeline-mux-2026-11-02.md, "Timeline dedupe"). Pure: no libobs, no files.
//
// The replay ring keeps a hash of the picture data of every video keyframe it receives (packet_hash.h) and numbers its video packets from the
// moment it started (`seq`, which never shifts when the front is evicted). A closed chunk is probed once: its video packet count and, for every
// keyframe in it, the packet index, the hash and the time since the chunk's first packet. A chunk is linked to the ring only when ALL of that
// matches one place in the ring - every keyframe of the chunk at its offset with the same hash and the same spacing in time, no other keyframe
// of the ring inside the span, and a ring keyframe right where the chunk ends. A still picture makes identical keyframes, so a chunk can match
// several places; then its neighbours decide (a chunk that follows the one before starts where that one ended). A chunk that stays ambiguous,
// or that cannot be checked yet because the ring has not got that far, is not linked, and the ring spills its footage as it always did: a failed
// link costs wear, never footage.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace bx {

// A video keyframe the ring holds.
struct RingKey {
  uint64_t seq = 0;      // the video packet's ordinal since the ring started
  int64_t dts_usec = 0;  // on the encoder's clock
  uint64_t hash = 0;     // vcl_digest of the packet
  uint32_t vcl_bytes = 0;
};

// A video keyframe inside a probed chunk.
struct ChunkKey {
  uint32_t index = 0;    // which video packet of the chunk (0 is the first)
  int64_t dts_us = 0;    // microseconds after the chunk's first video packet
  uint64_t hash = 0;
  uint32_t vcl_bytes = 0;
};

struct ChunkFacts {
  uint32_t video_count = 0;
  std::vector<ChunkKey> keys;     // ascending; keys[0].index == 0 or the chunk does not start on a keyframe and is never linked
  bool follows_previous = false;  // the file the writer opened right after the previous one: no pause between them
};

struct ChunkLink {
  bool linked = false;
  uint64_t at = 0;  // the ring seq of the chunk's first video packet
};

// A chunk's timestamps and the ring's agree to this much (rounding of the container's time base, the ring's microseconds).
constexpr int64_t kLinkTimeToleranceUs = 5000;

namespace timeline_link_detail {

inline const RingKey* find_key(const std::vector<RingKey>& keys, uint64_t seq) {
  const auto at = std::lower_bound(keys.begin(), keys.end(), seq, [](const RingKey& key, uint64_t value) { return key.seq < value; });
  return at != keys.end() && at->seq == seq ? &*at : nullptr;
}

// Every place the chunk could be: the ring's keyframes whose neighbourhood matches the chunk's keyframes one for one.
inline std::vector<uint64_t> candidates(const std::vector<RingKey>& keys, uint64_t ring_end_seq, const ChunkFacts& chunk) {
  std::vector<uint64_t> found;
  if (chunk.video_count == 0 || chunk.keys.empty() || chunk.keys.front().index != 0) return found;
  for (const RingKey& start : keys) {
    const uint64_t end = start.seq + chunk.video_count;
    if (end >= ring_end_seq) break;  // the ring has not seen the packet after the chunk yet: cannot check the end
    bool match = true;
    for (const ChunkKey& expected : chunk.keys) {
      const RingKey* key = find_key(keys, start.seq + expected.index);
      if (!key || key->hash != expected.hash || key->vcl_bytes != expected.vcl_bytes ||
          std::llabs((key->dts_usec - start.dts_usec) - expected.dts_us) > kLinkTimeToleranceUs) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    if (!find_key(keys, end)) continue;  // the chunk must end where a group of pictures ends
    // No keyframe of the ring inside the chunk that the chunk does not have.
    const auto first = std::lower_bound(keys.begin(), keys.end(), start.seq, [](const RingKey& key, uint64_t value) { return key.seq < value; });
    const auto last = std::lower_bound(keys.begin(), keys.end(), end, [](const RingKey& key, uint64_t value) { return key.seq < value; });
    if (static_cast<size_t>(last - first) != chunk.keys.size()) continue;
    found.push_back(start.seq);
  }
  return found;
}

}  // namespace timeline_link_detail

// `keys`: the ring's video keyframes, ascending by seq. `ring_end_seq`: one past the newest video packet the ring has. `chunks`: the probed chunks, oldest
// first. Returns one link per chunk.
// `known`: links settled earlier (same size as `chunks`, or empty). They stand as they are - the ring's front moves on and a settled chunk's keyframes
// leave `keys` - and they anchor their neighbours.
inline std::vector<ChunkLink> link_chunks(const std::vector<RingKey>& keys, uint64_t ring_end_seq, const std::vector<ChunkFacts>& chunks,
                                          const std::vector<ChunkLink>& known = {}) {
  using namespace timeline_link_detail;
  std::vector<ChunkLink> links(chunks.size());
  std::vector<std::vector<uint64_t>> options(chunks.size());
  for (size_t i = 0; i < chunks.size(); ++i) {
    if (known.size() == chunks.size() && known[i].linked) {
      links[i] = known[i];
      continue;
    }
    options[i] = candidates(keys, ring_end_seq, chunks[i]);
    if (options[i].size() == 1) {
      links[i].linked = true;
      links[i].at = options[i][0];
    }
  }
  const auto allowed = [&](size_t i, uint64_t at) { return std::find(options[i].begin(), options[i].end(), at) != options[i].end(); };
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < chunks.size(); ++i) {
      if (links[i].linked || options[i].size() < 2) continue;
      bool have_forward = false, have_backward = false;
      uint64_t forward = 0, backward = 0;
      if (i > 0 && chunks[i].follows_previous && links[i - 1].linked) {
        forward = links[i - 1].at + chunks[i - 1].video_count;
        have_forward = allowed(i, forward);
      }
      if (i + 1 < chunks.size() && chunks[i + 1].follows_previous && links[i + 1].linked && links[i + 1].at >= chunks[i].video_count) {
        backward = links[i + 1].at - chunks[i].video_count;
        have_backward = allowed(i, backward);
      }
      // Two neighbours that disagree leave the chunk unlinked.
      if (have_forward && have_backward && forward != backward) continue;
      if (have_forward || have_backward) {
        links[i].linked = true;
        links[i].at = have_forward ? forward : backward;
        changed = true;
      }
    }
  }
  // A chunk that follows another must start where that one ended. Where two linked chunks that follow each other disagree, neither is trusted.
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 1; i < chunks.size(); ++i) {
      if (!chunks[i].follows_previous || !links[i].linked || !links[i - 1].linked) continue;
      if (links[i - 1].at + chunks[i - 1].video_count == links[i].at) continue;
      links[i].linked = links[i - 1].linked = false;
      changed = true;
    }
  }
  return links;
}

}  // namespace bx
