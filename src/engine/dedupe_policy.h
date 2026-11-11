// The Timeline dedupe's host-side rules (docs/review-timeline-mux-2026-11-02.md), apart from libobs and the disk. Pure, so they are tested.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

#include "media.h"
#include "ring_store.h"

namespace bx {

// A chunk is written about 5 s after the last keyframe boundary and the group of pictures is up to 4 s: ten seconds is the most a chunk holds.
constexpr int kDedupeChunkSeconds = 10;

// The ring may rely on chunk files only while Timeline keeps them longer than the ring needs them: the buffer, two chunks (one being written, one the
// ring has not left yet) and a minute of slack for the pin to be renewed in. Otherwise retention could delete footage the ring still points at.
inline bool dedupe_window_ok(int timeline_minutes, int buffer_seconds) {
  return timeline_minutes > 0 && static_cast<int64_t>(timeline_minutes) * 60 >= static_cast<int64_t>(buffer_seconds) + 2 * kDedupeChunkSeconds + 60;
}

// The writer opened this file right after the previous one (a split, no pause between): both are ticks of the same clock, and a split shares its boundary.
inline bool dedupe_chunk_follows(bool have_previous, uint32_t previous_end_ms, uint32_t start_ms) {
  if (!have_previous) return false;
  const int32_t gap = static_cast<int32_t>(start_ms - previous_end_ms);
  return std::abs(gap) <= 100;
}

// The probe's reading of a closed chunk as the store takes it. False when the chunk cannot be matched: nothing read, or it does not begin on a keyframe.
inline bool dedupe_probe_from_report(const std::string& file, const ChunkReport& report, bool follows, ChunkProbe& out) {
  if (!report.ok || report.keys.empty() || report.keys.front().index != 0 || report.video_count == 0) return false;
  out = ChunkProbe{};
  out.file = file;
  out.facts.video_count = report.video_count;
  out.facts.follows_previous = follows;
  for (const ChunkKeyFact& key : report.keys) {
    if (key.vcl_bytes == 0) return false;  // a keyframe whose picture could not be read matches nothing
    ChunkKey fact;
    fact.index = key.index;
    fact.dts_us = key.dts_us;
    fact.hash = key.hash;
    fact.vcl_bytes = key.vcl_bytes;
    out.facts.keys.push_back(fact);
    out.key_time_s.push_back(key.file_seconds);
  }
  return true;
}

}  // namespace bx
