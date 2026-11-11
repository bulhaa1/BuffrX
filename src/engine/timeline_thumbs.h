// The Timeline preview's strip of thumbnails (docs/protocol.md, `timeline.thumbs`).
//
// Which requested moment becomes a JPEG, from which published chunk and at what offset, is decided here
// without a decoder and without the disk, so every rule - clamped, deduplicated, one image per second of
// footage, only published and present chunks - can be checked without a game, a file or FFmpeg. The file's
// own existence is the cache: a moment asked for twice costs one stat, and no picture is ever stale,
// because a chunk's footage never changes after its writer let go.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bx {

// One chunk a thumbnail may be cut from: published, and its file still on disk. All times are the
// session's own clock, the numbers `timeline.get` reports.
struct TimelineThumbSpan {
  size_t chunk_index = 0;  // where the chunk is in the session's own list
  std::string key;         // the chunk file's name, made safe to use in a file name
  int64_t start_ms = 0;
  int64_t end_ms = 0;
};

// One thumbnail the caller may make and name. `name` is the file inside the session's thumbs directory.
struct TimelineThumb {
  int64_t at_ms = 0;  // the clamped moment the request named, echoed back in the reply
  size_t chunk_index = 0;
  int64_t offset_ms = 0;  // inside that chunk, where the decoder is sent
  std::string name;       // the cache file's name; equal names are the same picture
};

// A call may ask for 24 moments and may make 6 new pictures: the first refresh of a strip is fast, and
// the rest arrives on the next poll. FFmpeg decodes one thread per picture, and a thumbnail is wanted,
// not the speed.
constexpr size_t kTimelineThumbsPerCall = 24;
constexpr size_t kTimelineThumbsDecoded = 12;
// Two moments inside one second of one chunk are one picture: a player's scrubbing asks for a dense
// strip, and a duplicate decode of the same second buys nothing the first did not.
constexpr int64_t kTimelineThumbBucketMs = 1000;

// A chunk's file name as the prefix of its cache files: letters, digits, '-', '_' and '.' are kept and
// everything else becomes '_', so a path from the index can never name a file outside the thumbs folder.
inline std::string timeline_thumb_key(const std::string& file) {
  const size_t slash = file.find_last_of("\\/");
  const std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
  std::string key;
  key.reserve(name.size());
  for (char character : name) {
    const unsigned char code = static_cast<unsigned char>(character);
    if (std::isalnum(code) || character == '-' || character == '_' || character == '.') {
      key.push_back(character);
    } else {
      key.push_back('_');
    }
  }
  // A file name that sanitised to nothing still names a picture: the bucket inside it keeps two chunks of
  // an odd index apart when their names were both unusable.
  return key.empty() ? std::string("chunk") : key;
}

inline std::string timeline_thumb_name(const std::string& key, int64_t bucket_ms) {
  return key + "-" + std::to_string(bucket_ms) + ".jpg";
}

// The list a caller decodes from, in request order: every moment clamped into the session's available span,
// mapped to the published chunk that holds it, one entry per second of one chunk, at most
// kTimelineThumbsPerCall asked and any number returned. A moment in a gap between two chunks, or on a
// session with no published chunk, names nothing: it is not footage, and no decoder is handed a file for it.
inline std::vector<TimelineThumb> timeline_thumb_plan(const std::vector<TimelineThumbSpan>& spans,
                                                      const std::vector<int64_t>& times_ms,
                                                      int64_t available_start_ms, int64_t available_end_ms) {
  std::vector<TimelineThumb> plan;
  if (spans.empty() || times_ms.empty()) return plan;
  if (available_end_ms <= available_start_ms) return plan;
  for (size_t asked = 0; asked < times_ms.size() && asked < kTimelineThumbsPerCall; ++asked) {
    const int64_t at_ms = std::max(available_start_ms, std::min(available_end_ms, times_ms[asked]));
    // The chunk that holds it: the newest one whose start is at or before the moment and whose end it does
    // not pass. A gap between two chunks answers nothing.
    const TimelineThumbSpan* chunk = nullptr;
    for (const TimelineThumbSpan& span : spans) {
      if (span.start_ms > at_ms || at_ms > span.end_ms) continue;
      if (!chunk || span.start_ms >= chunk->start_ms) chunk = &span;
    }
    if (!chunk || chunk->end_ms <= chunk->start_ms) continue;
    const int64_t offset_ms = at_ms - chunk->start_ms;
    const int64_t bucket_ms = offset_ms / kTimelineThumbBucketMs * kTimelineThumbBucketMs;
    TimelineThumb thumb;
    thumb.at_ms = at_ms;
    thumb.chunk_index = chunk->chunk_index;
    thumb.offset_ms = offset_ms;
    thumb.name = timeline_thumb_name(chunk->key, bucket_ms);
    const bool seen = std::any_of(plan.begin(), plan.end(), [&thumb](const TimelineThumb& made) {
      return made.name == thumb.name;
    });
    if (seen) continue;
    plan.push_back(std::move(thumb));
  }
  return plan;
}

}  // namespace bx
