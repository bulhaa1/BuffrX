// What to do when the video encoder itself dies (not slow: dead), decided without libobs so the steps are tested.
//
// An encoder that stops twice in ten minutes on the same id is not going to be fine the third time: its driver path is
// broken on this machine (AMD's AV1 texture encoder after a reset, say). BuffrX steps down one rung and remembers the
// rung for this GPU and driver, so the next start does not walk into the same wall. The rungs are vendor-neutral:
//   0  the encoder as chosen
//   1  the same codec's system-memory variant (h264_fallback_amf, obs_nvenc_h264_soft, obs_qsv11_soft), when registered
//   2  H.264 on the same vendor
//   3  software x264
// It is a layer over the person's settings (ObsHost::effective_for), never a rewrite of them.

#ifndef BUFFRX_ENCODER_LADDER_H
#define BUFFRX_ENCODER_LADDER_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "capacity_memory.h"

namespace buffrx {

constexpr int kMaxLadderRung = 3;
// Two deaths inside this window are a pattern. A reset kills the output and freezes the encoder within seconds of each
// other: those are one death, so a second one counts only this long after the first.
constexpr uint32_t kEncoderDeathWindowMs = 600000;
constexpr uint32_t kEncoderDeathGapMs = 20000;

inline bool encoder_is_system_memory(const std::string& id) {
  return id.find("_soft") != std::string::npos || id.find("fallback") != std::string::npos;
}

inline std::string encoder_vendor_of(const std::string& id) {
  if (id.find("nvenc") != std::string::npos) return "nvenc";
  if (id.find("amf") != std::string::npos) return "amf";
  if (id.find("qsv") != std::string::npos) return "qsv";
  return "software";
}

struct EncoderDeaths {
  std::string id;
  std::vector<uint32_t> at_ms;
};

// Notes one death of encoder `id`. True when it is the second one inside ten minutes: time to step down.
inline bool encoder_death_note(EncoderDeaths& deaths, const std::string& id, uint32_t now_ms) {
  if (deaths.id != id) {
    deaths.id = id;
    deaths.at_ms.clear();
  }
  deaths.at_ms.erase(std::remove_if(deaths.at_ms.begin(), deaths.at_ms.end(),
                                    [&](uint32_t at) { return now_ms - at >= kEncoderDeathWindowMs; }),
                     deaths.at_ms.end());
  if (!deaths.at_ms.empty() && now_ms - deaths.at_ms.back() < kEncoderDeathGapMs) return false;
  deaths.at_ms.push_back(now_ms);
  if (deaths.at_ms.size() < 2) return false;
  deaths.at_ms.clear();
  return true;
}

// The rung after `rung`. `has_system_variant`: the codec in use has a registered system-memory encoder on this vendor.
inline int ladder_next_rung(int rung, bool has_system_variant, bool codec_is_h264, bool vendor_is_software) {
  if (vendor_is_software || rung >= kMaxLadderRung) return rung;
  int next = rung + 1;
  if (next == 1 && !has_system_variant) next = 2;
  if (next == 2 && codec_is_h264) next = 3;  // H.264 on this vendor is what runs already
  return std::min(next, kMaxLadderRung);
}

// What a rung does to the encoder choice: the codec and vendor to ask for, and whether system-memory ids go first.
struct LadderLayer {
  std::string codec;
  std::string encoder;
  bool system_first = false;
};

inline LadderLayer ladder_layer(int rung, const std::string& vendor, const std::string& codec, const std::string& encoder) {
  LadderLayer layer{codec, encoder, false};
  if (rung == 1) layer.system_first = true;
  if (rung == 2) {
    layer.codec = "h264";
    layer.encoder = vendor.empty() || vendor == "software" ? encoder : vendor;
  }
  if (rung >= 3) {
    layer.codec = "h264";
    layer.encoder = "software";
  }
  return layer;
}

// The ids with the system-memory ones first (rung 1); the order inside each group is kept.
inline std::vector<std::string> ladder_order_ids(const std::vector<std::string>& ids, bool system_first) {
  if (!system_first) return ids;
  std::vector<std::string> ordered = ids;
  std::stable_partition(ordered.begin(), ordered.end(), [](const std::string& id) { return encoder_is_system_memory(id); });
  return ordered;
}

// One rung per GPU, driver and configured encoder: another card or a newer driver asks the question again.
inline std::string ladder_key(const std::string& gpu, const std::string& driver, const std::string& codec,
                              const std::string& encoder) {
  return gpu + "|" + driver + "|" + codec + "|" + encoder;
}

}  // namespace buffrx

#endif  // BUFFRX_ENCODER_LADDER_H
