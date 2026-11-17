// Which encoder the engine tries first, and in what order it falls back.
//
// The rule lives here, apart from ObsHost, because it is a decision about settings and a list of names:
// no graphics device, no libobs, nothing to start. `available` is what the machine reports; the result
// is every name that fits the codec and the chosen vendor, best first.
#pragma once

#include <string>
#include <vector>

namespace bx {

// The codec an OBS encoder id produces: "av1", "hevc" or "h264". Covers the real ids of OBS 33.0.1: obs_nvenc_hevc_tex,
// obs_qsv11_hevc, h265_texture_amf and its h265_fallback_amf (AMD's HEVC is "h265", never H.264). Anything else is H.264.
std::string encoder_codec(const std::string& identifier);

std::vector<std::string> encoder_candidates(const std::vector<std::string>& available, const std::string& codec,
                                            const std::string& encoder);

// The one-time move to H.264 (owner decision 2026-11-14): a first start used to pick AV1 on any card with an AV1 encoder, which
// put AMD users on av1_texture_amf. A config that says AV1 with the codec auto-picked, on a machine whose AV1 encoder is not
// NVENC, moves to H.264 once. `migrated` is the config flag that keeps it from running twice.
inline bool codec_default_migration_due(const std::string& codec, bool auto_picked, bool migrated, bool nvenc_av1) {
  return !migrated && auto_picked && codec == "av1" && !nvenc_av1;
}

}  // namespace bx
