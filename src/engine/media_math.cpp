#include "media_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace bx {

void thumbnail_size(int source_width, int source_height, int target_width, int& out_width, int& out_height) {
  if (source_width <= 0 || source_height <= 0 || target_width <= 0) {
    out_width = 0;
    out_height = 0;
    return;
  }
  // Fit the source inside a square box whose side is the target width: a portrait clip is limited by
  // its height, not by its width, or a 1080x1920 source would come back 320x568 and overflow the box.
  // Never upscale: a small clip gets its own size back rather than a blurry enlargement.
  const int longest = std::max(source_width, source_height);
  const double scale = std::min(1.0, static_cast<double>(target_width) / static_cast<double>(longest));
  const int width = static_cast<int>(std::lround(static_cast<double>(source_width) * scale));
  const int height = static_cast<int>(std::lround(static_cast<double>(source_height) * scale));
  // Encoders want even dimensions; the thumbnail is small, so one pixel of rounding is invisible.
  out_width = std::max(2, width - (width % 2));
  out_height = std::max(2, height - (height % 2));
}

bool trim_range(double duration_seconds, double requested_start, double requested_length, double& start_seconds,
                double& length_seconds) {
  start_seconds = 0.0;
  length_seconds = 0.0;
  if (duration_seconds <= 0.0 || requested_length <= 0.0) return false;
  const double start = std::max(0.0, std::min(requested_start, duration_seconds));
  const double remaining = duration_seconds - start;
  const double length = std::min(requested_length, remaining);
  // A sliver shorter than a frame is not a clip.
  if (length < 0.05) return false;
  start_seconds = start;
  length_seconds = length;
  return true;
}

std::vector<std::pair<double, double>> export_segments(const ExportJob& job, double duration) {
  // A part shorter than this holds no frame at 60 fps worth keeping, and a gap shorter than it is a
  // slip of the mouse, not a cut.
  constexpr double kShortest = 0.05;
  std::vector<std::pair<double, double>> parts = job.segments;
  if (parts.empty()) parts.emplace_back(job.start, job.end > 0.0 ? job.end : (duration > 0.0 ? duration : 0.0));
  std::vector<std::pair<double, double>> clean;
  for (auto [from, to] : parts) {
    if (!std::isfinite(from) || !std::isfinite(to)) continue;
    from = std::max(0.0, from);
    if (duration > 0.0) to = std::min(to, duration);
    if (to - from >= kShortest) clean.emplace_back(from, to);
  }
  std::sort(clean.begin(), clean.end());
  std::vector<std::pair<double, double>> merged;
  for (const auto& part : clean) {
    if (!merged.empty() && part.first <= merged.back().second + kShortest) {
      merged.back().second = std::max(merged.back().second, part.second);
    } else {
      merged.push_back(part);
    }
  }
  return merged;
}

double segments_length(const std::vector<std::pair<double, double>>& segments) {
  double total = 0.0;
  for (const auto& part : segments) total += std::max(0.0, part.second - part.first);
  return total;
}

bool export_graded(const ExportJob& job) {
  return std::abs(job.grade_brightness - 100.0) > 0.5 || std::abs(job.grade_contrast - 100.0) > 0.5 ||
         std::abs(job.grade_saturation - 100.0) > 0.5;
}

bool export_retimed(const ExportJob& job) {
  return std::abs(job.speed - 1.0) > 0.001;
}

std::string atempo_chain(double speed) {
  if (std::abs(speed - 1.0) <= 0.001) return std::string();
  double rate = std::min(4.0, std::max(0.25, speed));
  std::string chain;
  char stage[32];
  // One stage per factor of two, because a single atempo only reaches 2 (0.5 the other way) on the
  // oldest FFmpeg the runtime might hand over.
  while (rate > 2.0) {
    chain += "atempo=2.000,";
    rate /= 2.0;
  }
  while (rate < 0.5) {
    chain += "atempo=0.500,";
    rate /= 0.5;
  }
  std::snprintf(stage, sizeof(stage), "atempo=%.3f,", rate);
  chain += stage;
  return chain;
}

ExportPlan export_plan(const ExportJob& job) {
  const bool cropped = job.crop_width > 0 && job.crop_height > 0;
  const bool scaled = job.out_width > 0 && job.out_height > 0;
  if (cropped || scaled || job.quality > 0.0 || job.video_kbps > 0) return ExportPlan::reencode;
  if (export_graded(job)) return ExportPlan::reencode;
  if (export_retimed(job)) return ExportPlan::reencode;
  if (job.segments.size() > 1) return ExportPlan::reencode;
  for (double volume : job.volumes) {
    if (std::abs(volume - 1.0) > 0.005) return ExportPlan::audio_only;
  }
  return ExportPlan::copy;
}

bool export_crop(int source_width, int source_height, int x, int y, int width, int height, int& out_x, int& out_y,
                 int& out_width, int& out_height) {
  out_x = 0;
  out_y = 0;
  out_width = source_width;
  out_height = source_height;
  if (width <= 0 || height <= 0 || source_width < 16 || source_height < 16) return false;
  if (x >= source_width || y >= source_height || x + width <= 0 || y + height <= 0) return false;
  int left = std::max(0, x) & ~1;
  int top = std::max(0, y) & ~1;
  int w = std::max(16, std::min(width, source_width - left)) & ~1;
  int h = std::max(16, std::min(height, source_height - top)) & ~1;
  if (left + w > source_width) left = (source_width - w) & ~1;
  if (top + h > source_height) top = (source_height - h) & ~1;
  out_x = left;
  out_y = top;
  out_width = w;
  out_height = h;
  return true;
}

std::string hardware_decoder_name(const std::string& codec_name) {
  // The CUVID decoders the runtime FFmpeg carries, keyed by the codec names `media_details` reports.
  if (codec_name == "av1") return "av1_cuvid";
  if (codec_name == "hevc") return "hevc_cuvid";
  if (codec_name == "h264") return "h264_cuvid";
  return {};
}

bool export_wants_faststart(const ExportJob& job) {
  // `video_kbps` is only set by a share fit, which sizes the file for Discord's cap: that file is played
  // before it has all arrived, so its index must be first.
  return job.video_kbps > 0;
}

double edit_head_seconds(double start, double keyframe_seconds, double frame_seconds) {
  const double head = start - keyframe_seconds;
  if (!(head > frame_seconds * 0.5 + 0.001)) return 0.0;  // the cut is on the frame the copy starts at
  if (head > kMaxEditHeadSeconds) return std::numeric_limits<double>::quiet_NaN();
  return head;
}

std::vector<std::string> video_encoder_candidates(const std::string& source_codec, bool held) {
  std::vector<std::string> candidates;
  // A share fit is an H.264 file: that is what a Discord player takes without a thought.
  if (!held) {
    if (source_codec == "av1") candidates = {"av1_nvenc", "av1_amf"};
    else if (source_codec == "hevc") candidates = {"hevc_nvenc", "hevc_amf", "hevc_qsv"};
    else if (source_codec == "h264") candidates = {"h264_nvenc", "h264_amf", "h264_qsv"};
  }
  // The floor every machine can reach, in vendor order, so a codec with no encoder of its own still saves.
  for (const char* h264 : {"h264_nvenc", "h264_amf", "h264_qsv", "libx264", "h264_mf"}) {
    if (std::find(candidates.begin(), candidates.end(), h264) == candidates.end()) candidates.emplace_back(h264);
  }
  return candidates;
}

int export_video_kbps(int source_kbps, int source_pixels, int output_pixels, double quality) {  // An unknown source rate is read as a generous 1080p60-class rate for its pixels.
  const double base = source_kbps > 0 ? source_kbps : 0.0055 * std::max(1, source_pixels);
  const double pixels = source_pixels > 0 ? static_cast<double>(output_pixels) / source_pixels : 1.0;
  const double kbps = base * pixels * (quality > 0.0 ? quality : 1.0);
  return static_cast<int>(std::clamp(kbps, 500.0, 150000.0));
}

bool save_tail_start(double duration_seconds, double requested_seconds, double slack_seconds, double& start) {
  start = 0.0;
  if (duration_seconds <= 0.0 || requested_seconds <= 0.0) return false;
  if (duration_seconds <= requested_seconds + slack_seconds) return false;
  start = duration_seconds - requested_seconds;
  return true;
}

double save_trim_slack(bool whole_buffer) { return whole_buffer ? 1.0e6 : 1.0; }

int chunk_seconds_clamped(int requested, int keyframe_seconds) {
  // Align up to the keyframe interval first, then clamp: at the bottom edge the floor wins, so a
  // 3-second request with a 2-second interval is 5 seconds, not 4.
  const int interval = std::max(1, keyframe_seconds);
  const int aligned = ((std::max(requested, 0) + interval - 1) / interval) * interval;
  return std::min(std::max(aligned, 5), 600);
}

bool concat_plan_for_range(const std::vector<SessionChunk>& chunks, uint32_t start_ms, uint32_t end_ms,
                           ConcatPlan& out) {
  out = ConcatPlan{};
  if (end_ms <= start_ms) return false;
  size_t first = 0;
  size_t last = 0;
  if (!chunk_range_for_window(chunks, start_ms, end_ms, first, last)) return false;
  for (size_t index = first; index <= last; ++index) out.files.push_back(chunks[index].file);
  const SessionChunk& head = chunks[first];
  const SessionChunk& tail = chunks[last];
  out.start_in_first = start_ms > head.start_ms ? (start_ms - head.start_ms) / 1000.0 : 0.0;
  const uint32_t tail_end = std::min(end_ms, tail.end_ms);
  out.end_in_last = tail_end > tail.start_ms ? (tail_end - tail.start_ms) / 1000.0 : 0.0;
  return true;
}

TimelineSavePlan timeline_save_plan(const SessionEntry& entry, int64_t start_ms, int64_t end_ms,
                                    int64_t available_end_ms, std::string& error) {
  TimelineSavePlan plan;
  error.clear();
  if (entry.chunks.empty()) {
    error = "the session has no saved footage";
    return plan;
  }
  if (start_ms < 0 || end_ms <= start_ms) {
    error = "the range is empty";
    return plan;
  }
  if (end_ms > available_end_ms) {
    error = "the newest moment is not saved yet";
    return plan;
  }
  if (end_ms > UINT32_MAX) {
    error = "the range is outside the session";
    return plan;
  }
  // Only a published chunk is footage: the writer still holds the newest one, and its file is not readable.
  std::vector<SessionChunk> published;
  published.reserve(entry.chunks.size());
  for (const SessionChunk& chunk : entry.chunks) {
    if (chunk.published) published.push_back(chunk);
  }
  size_t first = 0;
  size_t last = 0;
  if (!chunk_range_for_window(published, static_cast<uint32_t>(start_ms), static_cast<uint32_t>(end_ms), first,
                              last)) {
    error = "the range covers no recorded footage";
    return plan;
  }
  // A copy takes the first chunk's stream for the whole file, so every chunk must still be that stream: a
  // rebuild between them (a settings change, a governor step) made a second epoch, and joining across it
  // would write a clip that plays broken. Decided with the user (2026-11-01): the clip is the part in the layout that holds the
  // range's end, stream-copied (no re-encode, no loss), and it says how much of the start it left out.
  for (size_t index = last; index > first; --index) {
    if (published[index - 1].epoch != published[index].epoch) {
      first = index;
      break;
    }
  }
  plan.files.reserve(last - first + 1);
  for (size_t index = first; index <= last; ++index) {
    plan.files.push_back(published[index].file);
    const int64_t hole = index == first ? 0 : static_cast<int64_t>(published[index].start_ms) -
                                                  static_cast<int64_t>(published[index - 1].end_ms);
    plan.gap_before_ms.push_back(hole > kSaveGapMs ? hole : 0);
  }
  const SessionChunk& head = published[first];
  const SessionChunk& tail = published[last];
  plan.start_in_first_ms = start_ms > static_cast<int64_t>(head.start_ms) ? start_ms - head.start_ms : 0;
  plan.cut_head_ms = start_ms < static_cast<int64_t>(head.start_ms) ? static_cast<int64_t>(head.start_ms) - start_ms : 0;
  const int64_t tail_end = std::min(end_ms, static_cast<int64_t>(tail.end_ms));
  plan.end_in_last_ms = tail_end > static_cast<int64_t>(tail.start_ms) ? tail_end - tail.start_ms : 0;
  return plan;
}

namespace {

// Two rationals are the same rate when their values are: 60/1 and 120/2 are one frame rate. A zero
// denominator is "unknown", and only an unknown matches an unknown.
bool rationals_equal(const Rational& a, const Rational& b) {
  if (a.den == 0 || b.den == 0) return a.den == 0 && b.den == 0;
  return static_cast<int64_t>(a.num) * b.den == static_cast<int64_t>(b.num) * a.den;
}

// The same rate to within one percent, for a rate FFmpeg derived rather than was told. A chunk's average
// rate is its own frame count over its own duration, so two chunks one encoder wrote in one session report
// 60/1 and 933120/15593: the same stream, rounded differently, and an exact compare refused to join them -
// every Timeline save that spanned two chunks failed with "the chunks do not share one stream layout". A
// real settings change is 30 against 60, far outside the tolerance.
bool rationals_close(const Rational& a, const Rational& b) {
  if (a.num <= 0 || a.den <= 0 || b.num <= 0 || b.den <= 0) return rationals_equal(a, b);
  const double left = static_cast<double>(a.num) / a.den;
  const double right = static_cast<double>(b.num) / b.den;
  return std::fabs(left - right) <= std::max(left, right) * 0.01;
}

}  // namespace

size_t trailing_compatible_run(const std::vector<StreamSignature>& signatures) {
  if (signatures.size() < 2) return 0;
  size_t from = signatures.size() - 1;
  while (from > 0 && streams_compatible(signatures[from - 1], signatures[signatures.size() - 1])) --from;
  return from;
}

bool streams_compatible(const StreamSignature& a, const StreamSignature& b) {
  return a.codec_id == b.codec_id && a.width == b.width && a.height == b.height && a.pix_fmt == b.pix_fmt &&
         rationals_close(a.frame_rate, b.frame_rate) && a.sample_rate == b.sample_rate &&
         a.channel_layout == b.channel_layout && rationals_equal(a.time_base, b.time_base);
}

ChunkClosedAction chunk_closed_action(bool has_session, bool rotating) {
  if (!has_session) return ChunkClosedAction::finish_single;
  return rotating ? ChunkClosedAction::rotate : ChunkClosedAction::close_session;
}

bool chunk_range_for_window(const std::vector<SessionChunk>& chunks, uint32_t start_ms, uint32_t end_ms,
                            size_t& first, size_t& last) {
  first = 0;
  last = 0;
  bool found = false;
  for (size_t index = 0; index < chunks.size(); ++index) {
    // Half-open overlap, so a chunk that ends exactly when the window starts is not part of it.
    if (chunks[index].end_ms <= start_ms || chunks[index].start_ms >= end_ms) continue;
    if (!found) {
      first = index;
      found = true;
    }
    last = index;
  }
  return found;
}

std::vector<size_t> retention_prune_order(const std::vector<SessionEntry>& entries, int64_t budget_bytes) {
  std::vector<size_t> order;
  if (budget_bytes <= 0) return order;  // zero means unlimited

  int64_t total = 0;
  for (const SessionEntry& entry : entries) total += entry.bytes();
  if (total <= budget_bytes) return order;

  // The newest closed session is the one a user is most likely to want, and a session still being
  // written is off limits. Both are excluded before anything is chosen to go.
  size_t newest = entries.size();
  for (size_t index = 0; index < entries.size(); ++index) {
    if (!entries[index].closed) continue;
    if (newest == entries.size() || session_older(entries[newest], entries[index])) newest = index;
  }
  std::vector<size_t> candidates;
  for (size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].closed && index != newest) candidates.push_back(index);
  }
  std::sort(candidates.begin(), candidates.end(),
            [&entries](size_t a, size_t b) { return session_older(entries[a], entries[b]); });

  for (size_t index : candidates) {
    if (total <= budget_bytes) break;
    order.push_back(index);
    total -= entries[index].bytes();
  }
  return order;
}

namespace {

// One OBU of a packet: its type and where it ends. False when the bytes do not hold a whole OBU.
bool next_obu(const uint8_t* data, size_t size, size_t at, int& type, size_t& end) {
  if (at >= size) return false;
  const uint8_t header = data[at];
  if (header & 0x80) return false;  // the forbidden bit
  type = (header >> 3) & 15;
  const bool extension = (header >> 2) & 1;
  const bool has_size = (header >> 1) & 1;
  size_t pos = at + 1 + (extension ? 1 : 0);
  if (pos > size) return false;
  if (!has_size) {
    end = size;  // without a size field the OBU runs to the end of the packet
    return true;
  }
  uint64_t length = 0;
  for (int i = 0;; ++i) {
    if (i >= 8 || pos >= size) return false;  // leb128 is at most 8 bytes
    const uint8_t byte = data[pos++];
    length |= static_cast<uint64_t>(byte & 0x7F) << (7 * i);
    if (!(byte & 0x80)) break;
  }
  if (length > size - pos) return false;
  end = pos + static_cast<size_t>(length);
  return true;
}

constexpr int kObuSequenceHeader = 1;

// A big-endian bit reader over one OBU's payload that never reads past it.
struct Bits {
  const uint8_t* data;
  size_t size_bits;
  size_t at = 0;
  bool ok = true;
  uint64_t read(int count) {
    uint64_t value = 0;
    for (int i = 0; i < count; ++i) {
      if (at >= size_bits) {
        ok = false;
        return 0;
      }
      value = (value << 1) | ((data[at / 8] >> (7 - at % 8)) & 1);
      ++at;
    }
    return value;
  }
  void uvlc() {
    int zeros = 0;
    while (ok && read(1) == 0) {
      if (++zeros >= 32) {
        ok = false;
        return;
      }
    }
    read(zeros);
  }
};

// The bit offset (in the payload) and the value of every operating point's seq_level_idx. False when
// the header is malformed or ends early.
bool sequence_levels(const uint8_t* payload, size_t size, std::vector<std::pair<size_t, int>>& levels) {
  levels.clear();
  Bits bits{payload, size * 8};
  bits.read(3);  // seq_profile
  bits.read(1);  // still_picture
  if (bits.read(1)) {  // reduced_still_picture_header: one level and nothing else
    const size_t at = bits.at;  // before the read: argument order would be unspecified
    const int level = static_cast<int>(bits.read(5));
    levels.emplace_back(at, level);
    return bits.ok;
  }
  bool decoder_model = false;
  int delay_length = 0;
  if (bits.read(1)) {  // timing_info_present_flag
    bits.read(32);     // num_units_in_display_tick
    bits.read(32);     // time_scale
    if (bits.read(1)) bits.uvlc();  // equal_picture_interval, num_ticks_per_picture_minus_1
    decoder_model = bits.read(1) != 0;
    if (decoder_model) {
      delay_length = static_cast<int>(bits.read(5)) + 1;
      bits.read(32);  // num_units_in_decoding_tick
      bits.read(5);   // buffer_removal_time_length_minus_1
      bits.read(5);   // frame_presentation_time_length_minus_1
    }
  }
  const bool display_delay = bits.read(1) != 0;
  const int points = static_cast<int>(bits.read(5)) + 1;
  for (int i = 0; i < points && bits.ok; ++i) {
    bits.read(12);  // operating_point_idc
    const size_t at = bits.at;
    const int level = static_cast<int>(bits.read(5));
    levels.emplace_back(at, level);
    if (level > 7) bits.read(1);  // seq_tier
    if (decoder_model && bits.read(1)) {
      bits.read(delay_length);  // decoder_buffer_delay
      bits.read(delay_length);  // encoder_buffer_delay
      bits.read(1);             // low_delay_mode_flag
    }
    if (display_delay && bits.read(1)) bits.read(4);
  }
  return bits.ok;
}

// Calls `visit` with the payload of every sequence header OBU in the packet. False when the packet is
// malformed.
template <typename Visit>
bool each_sequence_header(const uint8_t* data, size_t size, Visit visit) {
  size_t at = 0;
  int type = 0;
  size_t end = 0;
  while (at < size) {
    if (!next_obu(data, size, at, type, end)) return false;
    if (type == kObuSequenceHeader) {
      // The payload starts after the header byte, the extension byte and the size field.
      const bool extension = (data[at] >> 2) & 1;
      size_t payload = at + 1 + (extension ? 1 : 0);
      if ((data[at] >> 1) & 1) {
        while (payload < end && (data[payload] & 0x80)) ++payload;
        ++payload;
      }
      if (payload > end || !visit(payload, end - payload)) return false;
    }
    at = end;
  }
  return true;
}

}  // namespace

int av1_level(const uint8_t* data, size_t size) {
  int found = -1;
  each_sequence_header(data, size, [&](size_t payload, size_t length) {
    std::vector<std::pair<size_t, int>> levels;
    if (found < 0 && sequence_levels(data + payload, length, levels) && !levels.empty()) found = levels.front().second;
    return true;
  });
  return found;
}

bool av1_unconstrain_level(uint8_t* data, size_t size) {
  // Every header is read before anything is written, so a malformed packet is never half rewritten.
  std::vector<size_t> writes;
  const bool parsed = each_sequence_header(data, size, [&](size_t payload, size_t length) {
    std::vector<std::pair<size_t, int>> levels;
    if (!sequence_levels(data + payload, length, levels)) return false;
    for (const auto& [bit, level] : levels) {
      if (level >= 20 && level <= 23) writes.push_back(payload * 8 + bit);
    }
    return true;
  });
  if (!parsed || writes.empty()) return false;
  for (size_t bit : writes) {
    for (size_t i = bit; i < bit + 5; ++i) data[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
  }
  return true;
}

float smooth_level(float previous, float next, double dt_seconds) {
  const float clamped = std::min(std::max(next, 0.0f), 1.0f);
  if (dt_seconds <= 0.0 || clamped >= previous) return clamped;
  // Decay: the display falls toward the fresh sample at 2.0/s of full scale, so a speech pause
  // glides instead of snapping shut.
  const float decayed = previous - static_cast<float>(2.0 * dt_seconds);
  return std::max(decayed, clamped);
}

}  // namespace bx

