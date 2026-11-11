// Clip integrity: what the engine checks before it tells the user a clip is saved.
//
// The save path had a failure where a file appeared and was not a clip, so "the file exists" is not
// good enough to announce. This reads the container back and proves it holds a video track and at
// least one keyframe. That is not a decode — it is the strongest check possible without a decoder in
// the runtime (the pinned OBS release ships none) — and it is the check that would have caught the
// original defect.
#pragma once

#include <cstdint>
#include <string>

namespace bx {

struct ClipCheck {
  bool ok = false;
  std::string reason;  // why not, when ok is false
  int64_t bytes = 0;
  bool has_video_track = false;
  bool has_keyframe = false;
};

// Matroska (mkv) and MP4 (mp4, mov) are the containers the engine can write.
ClipCheck check_clip_file(const std::string& path);

}  // namespace bx
