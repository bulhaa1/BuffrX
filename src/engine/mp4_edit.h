// The MP4 index, rewritten so a trimmed copy starts where the user cut and not where the keyframe
// fell. A stream copy has to begin on a keyframe; the frames between the keyframe and the cut are
// kept in the file (they are what makes the copy cheap) and an edit list tells the player to start
// presenting that many seconds in. The same arithmetic shortens the movie's stated length.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bx {

// Where a file's `moov` sits: `begin` is the first byte of its box, `size` how many bytes that is,
// and `last` says only free space follows it, which is what allows it to be rewritten in place.
struct MoovBox {
  bool found = false;
  int64_t begin = 0;
  int64_t size = 0;
  bool last = false;
};

MoovBox mp4_find_moov(const std::string& path);

// The same `moov` with every picture and sound track starting `head_seconds` into its own media, so
// a player presents the file from the cut rather than from the keyframe the copy began at. Empty
// when the header holds no track it can trim. Pure: the caller writes the result back.
std::vector<uint8_t> mp4_trim_moov(const std::vector<uint8_t>& moov, double head_seconds);

// Rewrites the file's own index so it presents its media `head_seconds` later, in place: only the
// index is read and written, so a save several gigabytes long costs a seek. True when the file now
// starts where the caller cut; a head of nothing is a no-op that succeeds.
bool mp4_head_trim(const std::string& path, double head_seconds, std::string& error);

}  // namespace bx
