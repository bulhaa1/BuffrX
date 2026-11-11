#pragma once

#include <functional>
#include <string>

#include "clip_check.h"
#include "media.h"

namespace bx {

// The filing half of a save, in one place and out of ObsHost: trim the clip to the length asked for,
// verify the container, then move it under the game it came from. The three media calls are handed in,
// so every branch can be checked without libobs or a decoder.
struct SaveFilingMedia {
  std::function<MediaInfo(const std::string& path)> probe;
  std::function<bool(const std::string& path, const std::string& output_path, double start, double length,
                     std::string& error)>
      trim;
  std::function<ClipCheck(const std::string& path)> verify;
};

struct SaveFiling {
  std::string path;   // where the clip is now: the original when it could not be moved
  std::string error;  // set only when the clip did not verify, the one case that is a failure
  int video_kbps = 0; // the probe's video rate, for the black-clip guard; 0 when unknown
  bool trimmed = false;
  bool filed = false;
};

// `seconds` is the length the save asked for; `whole_buffer` says the request was the whole ring, which
// is never cut; `keyframe_seconds` is the room an older ring's file may hold before its start. A clip
// that could not be trimmed or moved is still a clip: it stays where it is, and the log says why.
SaveFiling file_saved_clip(const std::string& saved_path, int seconds, bool whole_buffer, int keyframe_seconds,
                           const std::string& folder, const std::string& stem, const SaveFilingMedia& media);

}  // namespace bx
