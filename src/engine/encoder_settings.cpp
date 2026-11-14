#include "encoder_settings.h"

#include <algorithm>
#include <cmath>

namespace bx {

EncoderFamily encoder_family(const std::string& encoder_id) {
  if (encoder_id.find("nvenc") != std::string::npos) return EncoderFamily::Nvenc;
  if (encoder_id.find("amf") != std::string::npos) return EncoderFamily::Amf;
  if (encoder_id.find("qsv") != std::string::npos) return EncoderFamily::Qsv;
  if (encoder_id.find("x264") != std::string::npos) return EncoderFamily::X264;
  return EncoderFamily::Other;
}

const char* encoder_family_name(EncoderFamily family) {
  switch (family) {
    case EncoderFamily::Nvenc: return "nvenc";
    case EncoderFamily::Amf: return "amf";
    case EncoderFamily::Qsv: return "qsv";
    case EncoderFamily::X264: return "x264";
    default: return "other";
  }
}

namespace {

void put(std::vector<EncoderSetting>& out, const std::string& name, Json value) {
  out.push_back({name, std::move(value)});
}

const char* tier_name(const std::string& tier) {
  if (tier == "performance") return "performance";
  if (tier == "quality") return "quality";
  if (tier == "standard") return "standard";
  return "balanced";
}

}  // namespace

std::vector<EncoderSetting> encoder_tier_defaults(EncoderFamily family, const std::string& tier, const Config& config,
                                                  const EncoderTraits& traits) {
  const std::string t = tier_name(tier);
  const int keyint = std::max(1, config.keyframe_seconds);
  // Frame-rate bands (plan 009): heavier settings drop a step per band, so Quality at 144 fps is never a card's heaviest setting.
  const int fps = config.capture_fps;
  const int band = fps <= 60 ? 0 : fps <= 90 ? 1 : 2;
  std::vector<EncoderSetting> out;
  switch (family) {
    case EncoderFamily::Nvenc: {
      const bool performance = t == "performance";
      // Standard is its own tier: p4 without multipass or adaptive quantization, the light end of Balanced.
      const bool standard = t == "standard";
      // Measured on an RTX 5080 at 2560x1440, 165 fps, moving content (docs/measurement.md):
      //  - the UHQ tuning encoded 30-62% of the frames on every codec, even at p5: no mode uses it;
      //  - AV1 and H.264 keep up all the way to p7 with full-resolution multipass, and at the same quality target
      //    need about a third of the bitrate p5 does (27 Mbps against 81-91);
      //  - HEVC encoded 59% of the frames at p6 and 45% at p7 in that measurement; Highest with Maximum effort
      //    chooses p7 anyway (decided with the user, 2026-11-05): the top tier is the user's own choice.
      //  - look-ahead bought no compression in any run and costs GPU, VRAM and delay: only the quality tier has it.
      const bool quality = t == "quality";
      if (traits.nvenc_pre_ada) {
        // A generation slower, never full-resolution multipass (plan 009; unmeasured on hardware).
        put(out, "preset", Json(performance ? "p2" : standard ? "p3" : quality ? (band == 0 ? "p6" : band == 1 ? "p5" : "p4") : "p4"));
        put(out, "tune", Json("hq"));
        put(out, "multipass", Json(quality && band < 2 ? "qres" : "disabled"));
        put(out, "profile", Json("high"));
        put(out, "bf", Json(0));
        put(out, "adaptive_quantization", Json(!performance && !standard));
        put(out, "lookahead", Json(false));
        put(out, "keyint_sec", Json(keyint));
        break;
      }
      put(out, "preset", Json(standard ? "p4" : performance ? "p3" : quality ? "p7" : "p5"));
      // The names below are NVENC's own (nvenc-properties.c). "tuning" and "psycho_aq" were written
      // here before and no encoder carries either, so both settings were refused and did nothing.
      put(out, "tune", Json("hq"));  // never uhq: see above
      put(out, "multipass", Json(standard || performance ? "disabled" : quality ? "fullres" : "qres"));
      // "high" is H.264's only useful profile here; NVENC refuses it for HEVC and AV1 (main only),
      // and apply_encoder_setting drops it rather than writing a value the encoder would reject.
      put(out, "profile", Json("high"));
      // No B-frames in any mode (measured 2026-10-25, RTX 5080, 1440p60 H.264 buffering): they hold about 120 MB of the
      // engine's working set (255-262 MB with two, 136-139 MB with none) and, on a 10 s Borderlands 4 clip at the same
      // quality target, buy only 1.7% (H.264) to 6.4% (AV1) fewer bits and 0.13-0.27 dB. A ring that size gains about 10
      // MB per minute of buffer at 20 Mbps, so B-frames only pay for themselves past ten minutes. docs/measurement.md.
      put(out, "bf", Json(0));
      put(out, "adaptive_quantization", Json(!performance && !standard));
      put(out, "lookahead", Json(quality));
      put(out, "keyint_sec", Json(keyint));
      break;
    }
    case EncoderFamily::X264: {
      const bool performance = t == "performance";
      const bool quality = t == "quality";
      put(out, "preset", Json(performance ? "veryfast" : quality ? "slow" : "medium"));
      put(out, "profile", Json("high"));
      // No B-frame key: x264's preset is what carries the reference frames (obs-x264.c has no "bf"),
      // so writing one here was refused and did nothing.
      put(out, "keyint_sec", Json(keyint));
      break;
    }
    case EncoderFamily::Amf: {
      const bool quality = t == "quality";
      const bool extras = quality && traits.amf_quality_extras;
      // Quality drops to balanced above 90 fps. highQuality (AV1 only, <= 60 fps), HQVBR and pre-analysis are the extras
      // behind kAmfQualityExtrasProven: nothing here has run on AMD hardware yet.
      const char* preset = t == "performance" ? "speed" : !quality ? "balanced" : band == 2 ? "balanced" : "quality";
      if (extras && band == 0 && config.codec == "av1") preset = "highQuality";
      put(out, "preset", Json(preset));
      put(out, "rate_control", Json(extras && band < 2 ? "HQVBR" : "VBR"));
      put(out, "pre_analysis", Json(extras && band == 0));
      // No B-frames in any tier, as on NVENC: one stream shape at every size and preset (AMF's own default is 2 at 1080p60).
      put(out, "bf", Json(0));
      put(out, "profile", Json("high"));
      put(out, "keyint_sec", Json(keyint));
      break;
    }
    case EncoderFamily::Qsv: {
      // QSV's target usage is numbered TU1 (best quality, slowest) to TU7 (fastest), not named
      // (QSV_Encoder.h:89). Writing words here meant the list rejected every tier and Intel ran on
      // its own defaults whatever the user picked.
      put(out, "target_usage", Json(t == "performance" ? "TU7" : t == "standard" ? "TU5" : t == "quality" ? (band == 0 ? "TU2" : band == 1 ? "TU3" : "TU4") : "TU4"));
      put(out, "rate_control", Json("VBR"));  // ICQ is the quality-target candidate, once measured
      // The property is "bframes" (obs-qsv11.c:409, default 3); there is no "bf" property, so a "bf" row would be skipped.
      put(out, "bframes", Json(0));
      put(out, "profile", Json("high"));
      put(out, "keyint_sec", Json(keyint));
      break;
    }
    case EncoderFamily::Other:
    default:
      put(out, "keyint_sec", Json(keyint));
      break;
  }
  return out;
}

const char* encoder_mode_tier(const std::string& mode, const std::string& quality) {
  if (mode == "fast") return "performance";
  if (mode == "balanced") return "balanced";
  if (mode == "quality") return "quality";
  return quality_speed_tier(quality);
}

bool encoder_mode_is_known(const std::string& mode) {
  return mode == "auto" || mode == "fast" || mode == "balanced" || mode == "quality" || mode == "custom";
}

std::vector<EncoderSetting> governor_overrides(EncoderFamily family, int step) {
  std::vector<EncoderSetting> out;
  if (step <= 0) return out;
  const int s = step > 3 ? 3 : step;
  switch (family) {
    case EncoderFamily::Nvenc:
      // Measured on an RTX 5080 at 2560x1440, 120 fps AV1: p7 + UHQ + full-resolution multipass encoded 70%
      // of the frames; p5 + HQ + quarter-resolution multipass kept up with all of them. B-frames are not touched at any level: the pieces of a
      // clip that crosses an encoder swap are joined by stream copy, and a changed reorder depth is a different stream (plan 008).
      if (s == 1) {
        put(out, "preset", Json("p5"));
        put(out, "tune", Json("hq"));
        put(out, "multipass", Json("qres"));
        put(out, "lookahead", Json(false));
      } else if (s == 2) {
        put(out, "preset", Json("p4"));
        put(out, "tune", Json("hq"));
        put(out, "multipass", Json("disabled"));
        put(out, "lookahead", Json(false));
      } else {
        put(out, "preset", Json("p2"));
        put(out, "tune", Json("hq"));
        put(out, "multipass", Json("disabled"));
        put(out, "lookahead", Json(false));
        put(out, "adaptive_quantization", Json(false));
      }
      break;
    case EncoderFamily::X264:
      put(out, "preset", Json(s == 1 ? "veryfast" : s == 2 ? "superfast" : "ultrafast"));
      break;
    case EncoderFamily::Amf:
      put(out, "preset", Json(s == 1 ? "balanced" : "speed"));
      // The pre-analysis pass is extra GPU work per frame; OBS lists it only on some AMD cards, so the setter skips it
      // elsewhere. Off is the light value at every step.
      put(out, "pre_analysis", Json(false));
      // HQVBR is a heavier rate control: every step runs the plain one.
      put(out, "rate_control", Json("VBR"));
      break;
    case EncoderFamily::Qsv:
      put(out, "target_usage", Json(s == 1 ? "TU5" : s == 2 ? "TU6" : "TU7"));
      break;
    case EncoderFamily::Other:
    default:
      break;
  }
  return out;
}

void apply_encoder_overrides(std::vector<EncoderSetting>& settings, const std::vector<EncoderSetting>& overrides) {
  for (const EncoderSetting& item : overrides) {
    auto found = std::find_if(settings.begin(), settings.end(), [&](const EncoderSetting& s) { return s.name == item.name; });
    if (found != settings.end()) found->value = item.value;
    else settings.push_back(item);
  }
}

int encoder_speed_rank(EncoderFamily family, const std::string& name, const std::string& value) {
  if (family == EncoderFamily::Nvenc && name == "preset") {
    return value.size() == 2 && value[0] == 'p' && value[1] >= '1' && value[1] <= '7' ? value[1] - '0' : -1;
  }
  if (family == EncoderFamily::Nvenc && name == "multipass") {
    return value == "disabled" ? 0 : value == "qres" ? 1 : value == "fullres" ? 2 : -1;
  }
  if (family == EncoderFamily::Amf && name == "preset") {
    return value == "speed" ? 0 : value == "balanced" ? 1 : value == "quality" ? 2 : value == "highQuality" ? 3 : -1;
  }
  if (family == EncoderFamily::Qsv && name == "target_usage") {
    // TU7 is the fastest and TU1 the best: the rank counts up from TU7 so a larger rank is heavier.
    return value.size() == 3 && value[0] == 'T' && value[1] == 'U' && value[2] >= '1' && value[2] <= '7' ? '8' - value[2] : -1;
  }
  if (family == EncoderFamily::X264 && name == "preset") {
    static const char* const presets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast",
                                          "medium",     "slow",      "slower",   "veryslow", "placebo"};
    for (int index = 0; index < 10; ++index) {
      if (value == presets[index]) return index;
    }
    return -1;
  }
  return -1;
}

void apply_governor_relief(std::vector<EncoderSetting>& settings, EncoderFamily family, int step) {
  for (const EncoderSetting& item : governor_overrides(family, step)) {
    auto found = std::find_if(settings.begin(), settings.end(), [&](const EncoderSetting& s) { return s.name == item.name; });
    if (found == settings.end()) {
      settings.push_back(item);  // not set: the encoder's own default is unknown, so the relief value is the one that runs
      continue;
    }
    const bool speed_setting = item.name == "preset" || item.name == "target_usage" || item.name == "multipass";
    if (!speed_setting) {
      found->value = item.value;
      continue;
    }
    // A speed replaced only by a lighter one. An unnamed current value is kept rather than guessed at.
    const int now = encoder_speed_rank(family, item.name, found->value.is_string() ? found->value.as_string() : std::string());
    const int next = encoder_speed_rank(family, item.name, item.value.as_string());
    if (now >= 0 && next >= 0 && next < now) found->value = item.value;
  }
}

std::vector<EncoderSetting> merge_encoder_settings(std::vector<EncoderSetting> defaults, const Json& options) {
  if (!options.is_object()) return defaults;
  for (const auto& entry : options.fields()) {
    const std::string& name = entry.first;
    const Json& value = entry.second;
    auto found = std::find_if(defaults.begin(), defaults.end(), [&](const EncoderSetting& s) { return s.name == name; });
    if (value.is_null()) {
      if (found != defaults.end()) defaults.erase(found);
      continue;
    }
    if (found != defaults.end()) {
      found->value = value;
    } else {
      defaults.push_back({name, value});
    }
  }
  return defaults;
}

Json encoder_setting_value(const std::vector<EncoderSetting>& settings, const std::string& name) {
  for (const auto& setting : settings) {
    if (setting.name == name) return setting.value;
  }
  return Json();
}

Json encoder_options_after_edit(const Json& stored, const Json& requested, const std::vector<std::string>& keys,
                                const std::string& name, const Json& value) {
  Json options = stored.is_object() ? stored : Json::object();
  for (const std::string& key : keys) {
    // Stored wins, null and unknown included. A row the requested object does not carry (the encoder does not list it)
    // is not written: writing it would be a knob the encoder refuses.
    if (options.has(key) || !requested.has(key)) continue;
    options.set(key, requested[key]);
  }
  options.set(name, value);
  return options;
}

std::vector<EncoderSetting> configured_encoder_settings(EncoderFamily family, const Config& config, int governor_step,
                                                        const EncoderTraits& traits) {
  const std::string quality = effective_quality(config);
  std::vector<EncoderSetting> settings = merge_encoder_settings(
      encoder_tier_defaults(family, encoder_mode_tier(config.encoder_mode, quality), config, traits),
      config.encoder_mode == "custom" ? config.encoder_options : Json::object());
  // Never CBR, CQP, QVBR or a latency mode on AMD, nor anything but VBR on Intel (an older config's options could carry one):
  // a replay wants the rate to follow the picture. The host's own VBR path sizes the bit rate for every allowed value.
  if (family == EncoderFamily::Amf || family == EncoderFamily::Qsv) {
    for (EncoderSetting& setting : settings) {
      if (setting.name != "rate_control") continue;
      const std::string value = setting.value.is_string() ? setting.value.as_string() : std::string();
      if (value != "VBR" && !(family == EncoderFamily::Amf && value == "HQVBR")) setting.value = Json("VBR");
    }
  }
  // Relief is the last layer in every mode: Fast, Balanced, Maximum and Custom get the same steps (decided 2026-11-14).
  apply_governor_relief(settings, family, governor_step);
  if (family == EncoderFamily::Nvenc) apply_encoder_overrides(settings, {EncoderSetting{"tune", Json("hq")}});
  return settings;
}

double codec_bpp(const std::string& codec) {
  if (codec == "av1") return 0.070;
  if (codec == "hevc") return 0.105;
  return 0.145;  // h264, and anything unknown: the most demanding of the three
}

double tier_multiplier(const std::string& tier) {
  if (tier == "performance") return 0.70;
  if (tier == "quality") return 1.40;
  return 1.00;  // balanced, and anything unknown
}

int bitrate_plan(const std::string& codec, int width, int height, int fps, const std::string& tier) {
  // The capture source has no size until it has produced a frame (a game until it hooks), so an
  // unknown geometry is answered at the canvas's usual size rather than refused.
  const int w = width > 0 ? width : 1920;
  const int h = height > 0 ? height : 1080;
  const int rate = fps > 0 ? fps : 60;
  const double pixels_per_second = static_cast<double>(w) * static_cast<double>(h) * static_cast<double>(rate);
  const double kbps = pixels_per_second * codec_bpp(codec) * tier_multiplier(tier) / 1000.0;
  // Rounded, not truncated: truncation biases every rate down by up to a whole kbps for no reason.
  return std::clamp(static_cast<int>(std::lround(kbps)), 1000, 200000);
}

namespace {

double level_share(const std::string& level) {
  if (level == "low") return 0.6;
  if (level == "standard") return 0.8;
  if (level == "highest") return 1.5;
  return 1.0;  // high, custom, and anything unknown
}

}  // namespace

bool quality_is_preset(const std::string& level) {
  return level == "low" || level == "standard" || level == "high" || level == "highest";
}

int quality_target(const std::string& level) {
  if (level == "low") return 28;
  if (level == "standard") return 23;
  if (level == "highest") return 16;
  return 20;
}

const char* quality_speed_tier(const std::string& level) {
  if (level == "low") return "performance";
  if (level == "standard") return "standard";
  if (level == "highest") return "quality";
  return "balanced";
}

int preset_kbps(const std::string& codec, int width, int height, int fps, const std::string& level) {
  const double base = bitrate_plan(codec, width, height, fps, "balanced");
  return std::clamp(static_cast<int>(std::lround(base * level_share(level))), 1000, 200000);
}

int rescale_for_codec(int kbps, const std::string& from_codec, const std::string& to_codec) {
  const double scaled = static_cast<double>(kbps) * codec_bpp(to_codec) / codec_bpp(from_codec);
  return std::clamp(static_cast<int>(std::lround(scaled)), 1000, 200000);
}

int quality_kbps(const Config& config, int encode_width, int encode_height) {
  // "custom" is only ever seen on a Config built by hand: config_from_json turns it into High + fixed.
  const bool fixed = config.peak_mode == "fixed" || config.quality == "custom";
  if (fixed && config.custom_kbps > 0) return std::clamp(config.custom_kbps, 1000, 200000);
  return preset_kbps(config.codec, encode_width, encode_height, config.capture_fps,
                     config.quality == "custom" ? "high" : config.quality);
}

std::string effective_quality(const Config& config) {
  if (config.peak_mode == "fixed" || config.quality == "custom") return "high";
  return config.quality;
}

const char* video_preset_name(const std::string& preset) {
  if (preset == "low" || preset == "standard" || preset == "highest" || preset == "custom") return preset.c_str();
  return "high";
}

Json video_preset_patch(const std::string& preset) {
  Json patch = Json::object();
  const std::string level = video_preset_name(preset);
  if (level == "custom") return patch;
  patch.set("quality", Json(level));
  patch.set("peak_mode", Json("auto"));
  // Auto effort keeps the governor alive; a preset that pinned Quality effort could not step down by itself.
  patch.set("encoder_mode", Json("auto"));
  patch.set("encoder_options", Json::object());
  return patch;
}

std::string video_preset_from_config(const Config& config) {
  // A fixed rate or a hand-built quality is Custom whatever a stored preset says: that is what the encoder does.
  if (config.peak_mode == "fixed" || config.quality == "custom") return "custom";
  if (quality_is_preset(config.video_preset) || config.video_preset == "custom") return config.video_preset;
  return quality_is_preset(config.quality) ? config.quality : "high";
}

int ring_safety_mb(uint64_t ram_bytes) {
  if (ram_bytes == 0) return 2048;
  return static_cast<int>(std::max<uint64_t>(64, ram_bytes / 8 / (1024 * 1024)));
}

int cq_for_codec(const std::string& codec, int quality) {
  // Every hardware encoder libobs ships runs the quantizer 0-51 for H.264 and HEVC and 0-63 for AV1
  // (nvenc-properties.c:137, texture-amf.cpp:1267, obs-qsv11.c:381). A single stored number therefore
  // means different quality per codec: 18 is near-lossless on H.264 and visibly soft on AV1. The user
  // picks one quality number; this puts it on the codec's own scale.
  const bool av1 = codec == "av1";
  const int ceiling = av1 ? 63 : 51;
  const int floor = 1;
  const double scaled = av1 ? static_cast<double>(std::clamp(quality, floor, 51)) * 63.0 / 51.0
                            : static_cast<double>(quality);
  return std::clamp(static_cast<int>(std::lround(scaled)), floor, ceiling);
}

bool encoder_option_valid(const Json& value, const Json& options, bool numeric, double min, double max) {
  if (numeric) {
    if (!value.is_number()) return false;
    const double number = value.as_double();
    return number >= min && number <= max;
  }
  if (options.is_array()) {
    for (const Json& allowed : options.items()) {
      if (allowed.is_string() && allowed.as_string() == value.as_string()) return true;
    }
    return false;
  }
  return true;
}

}  // namespace bx
