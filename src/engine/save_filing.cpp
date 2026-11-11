#include "save_filing.h"

#include <algorithm>

#include "../shared/log.h"
#include "library.h"
#include "media_math.h"
#include "platform.h"

namespace bx {

SaveFiling file_saved_clip(const std::string& saved_path, int seconds, bool whole_buffer, int keyframe_seconds,
                           const std::string& folder, const std::string& stem, const SaveFilingMedia& media) {
  SaveFiling result;
  std::string path = saved_path;
  // Windows paths from here on: the file is ours now, not libobs's.
  std::replace(path.begin(), path.end(), '/', '\\');
  const MediaInfo info = media.probe(path);
  result.video_kbps = info.video_kbps;
  double start = 0.0;
  // The ring cuts the clip at a keyframe before it writes, so the file already starts on one and the streams already
  // start at zero: the room the cut can leave is one keyframe interval, and a copy to remove it would write the whole
  // clip again for nothing. A trim is kept for the fallback (a ring that wrote its whole length) and for the slack the
  // ask allows.
  const double slack = std::max(save_trim_slack(whole_buffer), static_cast<double>(std::max(1, keyframe_seconds)));
  if (info.ok && seconds > 0 && save_tail_start(info.duration_seconds, seconds, slack, start)) {
    // The ring is longer than the request: keep its last `seconds`, from the keyframe at or before.
    const std::string trimmed = path + ".trim" + path.substr(path.find_last_of('.'));
    std::string trim_error;
    if (media.trim(path, trimmed, start, static_cast<double>(seconds) + 1.0, trim_error) &&
        MoveFileExA(trimmed.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      result.trimmed = true;
      BX_LOG_INFO("clip trimmed to its last %d s (ring held %.1f s)", seconds, info.duration_seconds);
    } else {
      DeleteFileA(trimmed.c_str());
      BX_LOG_WARN("clip kept at its full %.1f s: the trim failed (%s)", info.duration_seconds, trim_error.c_str());
    }
  }
  // A file appearing is not a clip: verify the container before the user is told it was saved.
  const ClipCheck check = media.verify(path);
  if (!check.ok) {
    BX_LOG_ERROR("clip saved but did not verify (%s): %s", check.reason.c_str(), path.c_str());
    // A file with nothing in it (the muxer died before its first byte) is not footage to keep: left where it
    // was, the library listed it as a clip that cannot play. Anything with bytes stays, it is the person's.
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data) && data.nFileSizeHigh == 0 &&
        data.nFileSizeLow == 0) {
      DeleteFileA(path.c_str());
    }
    result.path = path;
    result.error = "the saved clip did not verify: " + check.reason;
    return result;
  }
  // Filed under the game it came from: <clips>\<folder>\Replay <time>.<ext>. A failed move keeps the
  // clip where OBS wrote it (still in the library, at the root) rather than losing it.
  if (!folder.empty() && !stem.empty()) {
    const size_t slash = path.find_last_of('\\');
    const std::string root = slash == std::string::npos ? std::string() : path.substr(0, slash);
    const size_t dot = path.find_last_of('.');
    const std::string extension = dot == std::string::npos ? std::string() : path.substr(dot);
    const std::string target = reserve_output_path(root + "\\" + folder, stem, extension);
    if (MoveFileExA(path.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) {
      path = target;
      result.filed = true;
    } else {
      BX_LOG_WARN("clip left at %s: could not move it to %s (%lu)", path.c_str(), target.c_str(), GetLastError());
    }
  }
  result.path = path;
  return result;
}

}  // namespace bx
