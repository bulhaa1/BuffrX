// Encoder settings: the tier presets and the user's explicit overrides, kept as pure data so the
// choice is testable without libobs.
//
// The tier (performance | balanced | quality) is the simple default. Everything the encoder exposes on
// top of it is a name/value pair the user set, and an explicit pair always wins. Family-correct
// presets are the point: NVENC takes p1..p7, x264 takes ultrafast..placebo, and handing x264 a "p7"
// makes it log "Invalid preset" and silently keep its own - the bug this module closes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../shared/json.h"
#include "config.h"

namespace bx {

enum class EncoderFamily { Nvenc, Amf, Qsv, X264, Other };

// Which family a libobs encoder id belongs to. "obs_nvenc_h264_tex" -> Nvenc, "obs_x264" -> X264.
EncoderFamily encoder_family(const std::string& encoder_id);
const char* encoder_family_name(EncoderFamily family);

struct EncoderSetting {
  std::string name;  // libobs settings key, e.g. "preset", "multipass"
  Json value;
};

// The tier's family-correct defaults. Only quality-shaping keys live here; rate control and the
// keyframe interval are applied by the host because they depend on the buffer budget.
std::vector<EncoderSetting> encoder_tier_defaults(EncoderFamily family, const std::string& tier, const Config& config);

// The speed tier ("performance", "balanced", "quality") an encoder mode runs: Fast, Balanced and Quality are the
// tiers themselves whatever the quality level; Auto and Custom follow the level (quality_speed_tier).
const char* encoder_mode_tier(const std::string& mode, const std::string& quality);
bool encoder_mode_is_known(const std::string& mode);

// What the governor (encoder_governor.h) asks of an encoder at `step` (0 asks for nothing, past the last
// step is the last), as settings that replace whatever the level and the user chose for them: the speed
// knobs only - preset, tuning, multipass, look-ahead, B-frames - never the rate or the quality.
std::vector<EncoderSetting> governor_overrides(EncoderFamily family, int step);
// Puts `overrides` over `settings`: a name in both takes the override, the rest is left alone.
void apply_encoder_overrides(std::vector<EncoderSetting>& settings, const std::vector<EncoderSetting>& overrides);

// Explicit options win over the defaults, in the order they arrive. A null value removes the default
// so "unset" is expressible; any other value replaces it in place, or appends when new.
std::vector<EncoderSetting> merge_encoder_settings(std::vector<EncoderSetting> defaults, const Json& options);

// The same quality, explicit options and governor layers used to build an encoder, without building one.
std::vector<EncoderSetting> configured_encoder_settings(EncoderFamily family, const Config& config, int governor_step);

// The value a key holds, or Null when the list does not carry it.
Json encoder_setting_value(const std::vector<EncoderSetting>& settings, const std::string& name);

// How much video a configuration should be encoded at, in kbps.
//
// A rate per codec and resolution, not one number for everything: the engine used to derive the
// bitrate from the memory budget alone, so AV1 got an H.264 rate, 4K got a 720p rate, and a long
// buffer computed under 1000 kbps and was refused. The model is bits per pixel per second:
//   kbps = width * height * fps * bpp * tier
// with a per-codec bpp (h264 0.145, hevc 0.105, av1 0.070 - AV1 reaches roughly 50% less than
// H.264 and HEVC 40% less at equal quality, which is what the constants encode) and a tier
// multiplier (fast 0.70, balanced 1.00, quality 1.40). A zero or negative width, height or frame
// rate falls back to 1920x1080 at 60 fps, which is what the canvas is when the capture source has
// not reported its size yet. An unknown codec is treated as h264 (the most demanding) and an
// unknown tier as balanced.
int bitrate_plan(const std::string& codec, int width, int height, int fps, const std::string& tier);

// Quality levels: low | standard | high | highest. A rate the user set on the slider is `peak_mode` "fixed".
//
// A level is two things. A constant-quality target the encoder aims at (quality_target, on the H.264
// scale; cq_for_codec puts it on the codec's own), and a peak rate it may not pass (preset_kbps). The
// peak is what the ring is sized from, so it is also what the buffer costs in RAM. Quiet scenes come
// in under it; it only bites on the scenes that would otherwise blow the ring.
bool quality_is_preset(const std::string& level);
int quality_target(const std::string& level);
// The encoder speed a level runs at, as the tier encoder_tier_defaults understands.
const char* quality_speed_tier(const std::string& level);
// The peak rate a preset puts on the slider for what is being encoded: the ladder's balanced rate
// (bitrate_plan) times the level's share (Low 0.6, Standard 0.8, High 1.0, Highest 1.5).
int preset_kbps(const std::string& codec, int width, int height, int fps, const std::string& level);
// A rate carried from one codec to another at the same picture: scaled by their bits per pixel, so 60
// Mbps of H.264 becomes about 29 Mbps of AV1.
int rescale_for_codec(int kbps, const std::string& from_codec, const std::string& to_codec);
// The peak rate a configuration encodes at: the level's while the peak is automatic, or the fixed rate
// the user set (the level's when none is set yet).
int quality_kbps(const Config& config, int encode_width, int encode_height);
// The level the encoder actually runs. A fixed rate chooses the picture's size, so the level's own target
// and speed do not apply: it runs High (and Custom, a hand-built config only, does too).
std::string effective_quality(const Config& config);

// The five Video presets. A preset is one choice for the whole encoder: the four levels set the quality, an
// automatic bitrate and Auto effort together; Custom leaves the rate and effort to the user.
const char* video_preset_name(const std::string& preset);
// The config patch a preset writes: {quality, peak_mode:"auto", encoder_mode:"auto", encoder_options:{}} for
// the four levels, empty for Custom. Auto effort is deliberate: the governor only steps down in Auto.
Json video_preset_patch(const std::string& preset);
// The preset a configuration is showing: its stored one while it is valid, Custom when it runs a fixed rate or
// a hand-built quality, else the quality level it carries.
std::string video_preset_from_config(const Config& config);

// The most memory the replay ring may take: an eighth of the machine's memory, never under 64 MB (2 GB when the memory is not known).
// Not a quality limit and not a setting: the ring keeps the length asked for at the quality chosen, and this only stops a runaway - a
// failed allocation ends the engine - from taking the machine's memory.
int ring_safety_mb(uint64_t ram_bytes);

// One quality number on the codec's own quantizer scale. NVENC, AMF and QSV all run 0-51 for H.264 and
// HEVC but 0-63 for AV1, so a stored 18 meant "near-lossless" on one codec and "visibly soft" on
// another. Clamped to each codec's own range, so the number the user chose is never out of bounds.
int cq_for_codec(const std::string& codec, int quality);

// A value is only safe to write when the encoder's own property list allows it. `options` is the list
// of allowed strings (Null to skip the check); `numeric` checks against min/max instead.
bool encoder_option_valid(const Json& value, const Json& options, bool numeric, double min, double max);

}  // namespace bx
