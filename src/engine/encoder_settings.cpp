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

std::vector<EncoderSetting> encoder_tier_defaults(EncoderFamily family, const std::string& tier, const Config& config) {
  const std::string t = tier_name(tier);
  const int keyint = std::max(1, config.keyframe_seconds);
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
      put(out, "preset", Json(t == "performance" ? "speed" : t == "quality" ? "quality" : "balanced"));
      put(out, "profile", Json("high"));
      put(out, "keyint_sec", Json(keyint));
      break;
    }
    case EncoderFamily::Qsv: {
      // QSV's target usage is numbered TU1 (best quality, slowest) to TU7 (fastest), not named
      // (QSV_Encoder.h:89). Writing words here meant the list rejected every tier and Intel ran on
      // its own defaults whatever the user picked.
      put(out, "target_usage", Json(t == "performance" ? "TU7" : t == "quality" ? "TU2" : "TU4"));
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
      // of the frames; p5 + HQ + quarter-resolution multipass kept up with all of them.
      if (s == 1) {
        put(out, "preset", Json("p5"));
        put(out, "tune", Json("hq"));
        put(out, "multipass", Json("qres"));
        put(out, "lookahead", Json(false));
        put(out, "bf", Json(0));
      } else if (s == 2) {
        put(out, "preset", Json("p4"));
        put(out, "tune", Json("hq"));
        put(out, "multipass", Json("disabled"));
        put(out, "lookahead", Json(false));
        put(out, "bf", Json(0));
      } else {
        put(out, "preset", Json("p2"));
        put(out, "tune", Json("hq"));
        put(out, "multipass", Json("disabled"));
        put(out, "lookahead", Json(false));
        put(out, "adaptive_quantization", Json(false));
        put(out, "bf", Json(0));
      }
      break;
    case EncoderFamily::X264:
      put(out, "preset", Json(s == 1 ? "veryfast" : s == 2 ? "superfast" : "ultrafast"));
      break;
    case EncoderFamily::Amf:
      put(out, "preset", Json(s == 1 ? "balanced" : "speed"));
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

std::vector<EncoderSetting> configured_encoder_settings(EncoderFamily family, const Config& config, int governor_step) {
  const std::string quality = effective_quality(config);
  std::vector<EncoderSetting> settings = merge_encoder_settings(
      encoder_tier_defaults(family, encoder_mode_tier(config.encoder_mode, quality), config),
      config.encoder_mode == "custom" ? config.encoder_options : Json::object());
  apply_encoder_overrides(settings, governor_overrides(family, config.encoder_mode == "auto" ? governor_step : 0));
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
