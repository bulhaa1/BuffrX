#include "encoder_choice.h"

#include <algorithm>

namespace bx {

std::vector<std::string> encoder_candidates(const std::vector<std::string>& available, const std::string& codec,
                                            const std::string& encoder) {
  struct Candidate {
    std::string id;
    int rank = 100;
  };
  std::vector<Candidate> candidates;
  for (const std::string& identifier : available) {
    // The FFmpeg-built encoders (software AV1 from SVT and libaom, the old ffmpeg NVENC wrapper) are not
    // candidates: a software AV1 encoder cannot keep up with a screen at 1440p120 on any CPU, and choosing one
    // for a card without AV1 hardware would make every clip stutter. Such a machine gets H.264 instead.
    if (identifier.rfind("ffmpeg_", 0) == 0) continue;
    if (identifier.find("nvenc") == std::string::npos && identifier.find("amf") == std::string::npos &&
        identifier.find("qsv") == std::string::npos && identifier != "obs_x264" &&
        identifier.find("av1") == std::string::npos) {
      continue;
    }
    const bool wants_av1 = codec == "av1";
    const bool wants_hevc = codec == "hevc";
    const bool is_av1 = identifier.find("av1") != std::string::npos;
    const bool is_hevc = identifier.find("hevc") != std::string::npos;
    if (wants_av1 && !is_av1) continue;
    if (wants_hevc && !is_hevc) continue;
    if (!wants_av1 && !wants_hevc && (is_av1 || is_hevc)) continue;
    std::string vendor = "software";
    if (identifier.find("nvenc") != std::string::npos) vendor = "nvenc";
    else if (identifier.find("amf") != std::string::npos) vendor = "amf";
    else if (identifier.find("qsv") != std::string::npos) vendor = "qsv";
    if (encoder != "auto" && encoder != vendor) continue;
    const int rank = vendor == "nvenc" ? 0 : vendor == "amf" ? 1 : vendor == "qsv" ? 2 : 3;
    candidates.push_back({identifier, rank * 10 + (identifier.find("_tex") != std::string::npos ? 0 : 1)});
  }
  // Best first: every one of them is tried in turn, so an encoder the driver refuses to create does not
  // leave the machine with none.
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });
  std::vector<std::string> ids;
  for (const Candidate& candidate : candidates) ids.push_back(candidate.id);
  return ids;
}

}  // namespace bx
