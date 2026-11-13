#include "media_export.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include "clip_check.h"
#include "library.h"
#include "media.h"
#include "mp4_edit.h"
#include "stitch_plan.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

namespace bx {
namespace {

// Set when a hardware video decoder opened but could not deliver a frame: the same save then runs once
// more in software instead of failing on a stream this machine's decoder happens not to take.
bool g_hardware_decode_failed = false;

std::string av_text(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

struct InputFile {
  AVFormatContext* format = nullptr;
  ~InputFile() {
    if (format) avformat_close_input(&format);
  }
};

bool open_file(const std::string& path, InputFile& input, std::string& error) {
  // Without this the demuxer's own warnings ("UDTA parsing failed retrying raw" on every clip) go to
  // stderr with FFmpeg's default callback, where no log setting reaches them.
  media_install_log_callback();
  const int opened = avformat_open_input(&input.format, path.c_str(), nullptr, nullptr);
  if (opened < 0) {
    error = "the file could not be opened: " + av_text(opened);
    return false;
  }
  if (avformat_find_stream_info(input.format, nullptr) < 0) {
    error = "the file's streams could not be read";
    return false;
  }
  return true;
}

double file_duration(AVFormatContext* format) {
  return format->duration != AV_NOPTS_VALUE ? static_cast<double>(format->duration) / AV_TIME_BASE : 0.0;
}

int64_t file_bytes(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return (static_cast<int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// Owners for the FFmpeg objects one pipeline creates; released in reverse order by the destructor.
struct CodecContext {
  AVCodecContext* context = nullptr;
  ~CodecContext() { avcodec_free_context(&context); }
};
struct FilterGraph {
  AVFilterGraph* graph = nullptr;
  AVFilterContext* source = nullptr;
  AVFilterContext* sink = nullptr;
  ~FilterGraph() { avfilter_graph_free(&graph); }
};

bool build_graph(FilterGraph& graph, const char* source_name, const std::string& source_args, const char* sink_name,
                 const std::string& chain, std::string& error) {
  graph.graph = avfilter_graph_alloc();
  if (!graph.graph) {
    error = "out of memory";
    return false;
  }
  if (avfilter_graph_create_filter(&graph.source, avfilter_get_by_name(source_name), "in", source_args.c_str(), nullptr,
                                   graph.graph) < 0 ||
      avfilter_graph_create_filter(&graph.sink, avfilter_get_by_name(sink_name), "out", nullptr, nullptr, graph.graph) <
          0) {
    error = "the filter graph could not be created";
    return false;
  }
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  if (!outputs || !inputs) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    error = "out of memory";
    return false;
  }
  outputs->name = av_strdup("in");
  inputs->name = av_strdup("out");
  if (!outputs->name || !inputs->name) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    error = "out of memory";
    return false;
  }
  outputs->filter_ctx = graph.source;
  outputs->pad_idx = 0;
  outputs->next = nullptr;
  inputs->filter_ctx = graph.sink;
  inputs->pad_idx = 0;
  inputs->next = nullptr;
  const int parsed = avfilter_graph_parse_ptr(graph.graph, chain.c_str(), &inputs, &outputs, nullptr);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (parsed < 0 || avfilter_graph_config(graph.graph, nullptr) < 0) {
    error = "the filter chain was refused: " + chain;
    return false;
  }
  return true;
}

bool open_decoder(AVStream* stream, CodecContext& decoder, std::string& error, bool allow_hardware = false) {
  // The graphics card's own decoder first, when the caller wants it: software AV1 (libaom, the
  // runtime's only one) decodes 1440p120 at about half real time, which is the whole cost of a save.
  // Every step may fail on a machine without the card or the driver, and each one falls through to the
  // software decoder below - that is the floor, never a failed save.
  const char* no_hardware = std::getenv("BUFFRX_NO_HW_DECODE");
  if (allow_hardware && !(no_hardware && std::strcmp(no_hardware, "1") == 0)) {
    const std::string name = hardware_decoder_name(avcodec_get_name(stream->codecpar->codec_id));
    const AVCodec* codec = name.empty() ? nullptr : avcodec_find_decoder_by_name(name.c_str());
    CodecContext attempt;
    if (codec) attempt.context = avcodec_alloc_context3(codec);
    if (attempt.context && avcodec_parameters_to_context(attempt.context, stream->codecpar) >= 0) {
      attempt.context->pkt_timebase = stream->time_base;
      AVBufferRef* device = nullptr;
      if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) >= 0) {
        attempt.context->hw_device_ctx = device;  // released with the context
        if (avcodec_open2(attempt.context, codec, nullptr) >= 0) {
          decoder.context = attempt.context;
          attempt.context = nullptr;
          return true;
        }
      }
    }
  }
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  decoder.context = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!decoder.context || avcodec_parameters_to_context(decoder.context, stream->codecpar) < 0) {
    error = "no decoder for the stream";
    return false;
  }
  decoder.context->pkt_timebase = stream->time_base;
  decoder.context->thread_count = 0;  // as many threads as the CPU has
  if (avcodec_open2(decoder.context, codec, nullptr) < 0) {
    error = "the decoder would not open";
    return false;
  }
  return true;
}

// The software pixels of a decoded frame: a CUDA frame is copied out into `software`, which the caller
// allocates once and reuses. `software`'s format says whether it was filled; anything else was already
// software and the caller keeps its own frame.
bool frame_to_software(AVFrame* frame, AVFrame* software, std::string& error) {
  av_frame_unref(software);
  if (frame->format != AV_PIX_FMT_CUDA) return true;
  if (av_hwframe_transfer_data(software, frame, 0) < 0) {
    error = "the decoder's frame could not be copied out";
    return false;
  }
  software->pts = frame->pts;
  return true;
}

// The pixel format the decoder delivers as software pixels: a GPU decoder delivers CUDA frames and
// names the software format it copies them out to, which is what the filter chain is fed.
AVPixelFormat decoder_software_format(AVCodecContext* context) {
  if (!context->hw_device_ctx) return context->pix_fmt;
  return context->sw_pix_fmt != AV_PIX_FMT_NONE ? context->sw_pix_fmt : AV_PIX_FMT_NV12;
}

// The pixel format an encoder takes: NV12 (what GPU encoders want) or 4:2:0 planar (x264), whichever
// it lists first.
AVPixelFormat encoder_pixel_format(AVCodecContext* context, const AVCodec* codec) {
#if LIBAVCODEC_VERSION_MAJOR >= 61
  const void* formats = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(context, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &formats, &count) >= 0 && formats) {
    const auto* list = static_cast<const AVPixelFormat*>(formats);
    for (int i = 0; i < count; ++i) {
      if (list[i] == AV_PIX_FMT_NV12 || list[i] == AV_PIX_FMT_YUV420P) return list[i];
    }
  }
#else
  // FFmpeg 6 and older expose the encoder's list directly. Still present in 7 as a deprecated field.
  for (const AVPixelFormat* format = codec->pix_fmts; format && *format != AV_PIX_FMT_NONE; ++format) {
    if (*format == AV_PIX_FMT_NV12 || *format == AV_PIX_FMT_YUV420P) return *format;
  }
#endif
  return AV_PIX_FMT_YUV420P;
}

// The chain `video_encoder_candidates` names: the clip's own codec first when the machine can encode
// it, then the H.264 floor. The first encoder that opens is used.
bool open_video_encoder(int width, int height, AVRational time_base, AVRational frame_rate, int kbps, bool held,
                        const std::vector<std::string>& candidates, bool global_header, CodecContext& encoder,
                        std::string& name, std::string& error) {
  for (const std::string& candidate : candidates) {
    const AVCodec* codec = avcodec_find_encoder_by_name(candidate.c_str());
    if (!codec) continue;
    CodecContext attempt;
    attempt.context = avcodec_alloc_context3(codec);
    if (!attempt.context) continue;
    AVCodecContext* c = attempt.context;
    c->width = width;
    c->height = height;
    c->time_base = time_base;
    c->framerate = frame_rate;
    c->pix_fmt = encoder_pixel_format(c, codec);
    c->bit_rate = static_cast<int64_t>(kbps) * 1000;
    // A share fit is a promise about the file's size: the peak is held to the average so the file
    // lands where the arithmetic said. An editor export may peak at one and a half times it.
    c->rc_max_rate = held ? c->bit_rate : c->bit_rate * 3 / 2;
    c->rc_buffer_size = static_cast<int>(std::min<int64_t>(INT32_MAX, held ? c->bit_rate : c->bit_rate * 2));
    const double fps = frame_rate.num > 0 && frame_rate.den > 0 ? av_q2d(frame_rate) : 60.0;
    c->gop_size = std::max(1, static_cast<int>(std::lround(fps * 2.0)));
    c->max_b_frames = 0;  // monotonic decode times, and every player handles it
    c->color_range = AVCOL_RANGE_MPEG;
    if (global_header) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* options = nullptr;
    // Each vendor's own speed knob, matched by suffix so AV1 and HEVC get the same ones as H.264.
    const bool nvenc = candidate.size() > 6 && candidate.compare(candidate.size() - 6, 6, "_nvenc") == 0;
    const bool amf = candidate.size() > 4 && candidate.compare(candidate.size() - 4, 4, "_amf") == 0;
    if (nvenc) {
      av_dict_set(&options, "preset", "p5", 0);
      av_dict_set(&options, "rc", "vbr", 0);
    } else if (candidate == "libx264") {
      av_dict_set(&options, "preset", "veryfast", 0);
    } else if (amf) {
      av_dict_set(&options, "quality", "balanced", 0);
    }
    const int opened = avcodec_open2(c, codec, &options);
    av_dict_free(&options);
    if (opened < 0) continue;
    encoder.context = attempt.context;
    attempt.context = nullptr;
    name = candidate;
    return true;
  }
  error = "no encoder for this clip would open on this machine";
  return false;
}

// One audio track through decode -> volume -> AAC.
struct AudioPipe {
  int input_index = -1;
  double volume = 1.0;
  CodecContext decoder;
  CodecContext encoder;
  FilterGraph graph;
  AVStream* output = nullptr;
  bool done = false;
};

bool write_packet(AVFormatContext* output, AVPacket* packet, AVRational from, AVStream* stream, std::string& error) {
  av_packet_rescale_ts(packet, from, stream->time_base);
  packet->stream_index = stream->index;
  packet->pos = -1;
  const int written = av_interleaved_write_frame(output, packet);
  if (written < 0) {
    error = "writing stopped: " + av_text(written);
    return false;
  }
  return true;
}

// Sends one frame (or nullptr to flush) to an encoder and writes every packet it gives back.
bool encode_frame(AVCodecContext* encoder, AVFrame* frame, AVFormatContext* output, AVStream* stream, AVPacket* packet,
                  std::string& error) {
  if (avcodec_send_frame(encoder, frame) < 0 && frame) {
    error = "the encoder refused a frame";
    return false;
  }
  while (true) {
    const int received = avcodec_receive_packet(encoder, packet);
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
    if (received < 0) {
      error = "the encoder failed: " + av_text(received);
      return false;
    }
    if (!write_packet(output, packet, encoder->time_base, stream, error)) return false;
  }
}

// Pulls whatever the filter graph has ready and encodes it.
bool drain_filter(FilterGraph& graph, AVCodecContext* encoder, AVFormatContext* output, AVStream* stream,
                  AVFrame* filtered, AVPacket* packet, std::string& error) {
  while (true) {
    const int got = av_buffersink_get_frame(graph.sink, filtered);
    if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return true;
    if (got < 0) {
      error = "the filter graph failed";
      return false;
    }
    filtered->pict_type = AV_PICTURE_TYPE_NONE;
    const bool ok = encode_frame(encoder, filtered, output, stream, packet, error);
    av_frame_unref(filtered);
    if (!ok) return false;
  }
}

// How long one picture lasts, from the stream's own frame rate.
double frame_length(AVFormatContext* format, int video) {
  if (video < 0) return 0.0;
  const AVRational rate = av_guess_frame_rate(format, format->streams[video], nullptr);
  return rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : 0.0;
}

// The keyframe a stream copy would begin on: the last one at or before `start`, -1 when none is found.
double keyframe_before(AVFormatContext* format, int video, double start) {
  if (video < 0 || start <= 0.001) return 0.0;
  AVStream* stream = format->streams[video];
  const int64_t target = std::llround(start / av_q2d(stream->time_base));
  if (av_seek_frame(format, video, target, AVSEEK_FLAG_BACKWARD) < 0) return -1.0;
  AVPacket* packet = av_packet_alloc();
  double keyframe = -1.0;
  for (int read = 0; packet && read < 4096 && av_read_frame(format, packet) >= 0; ++read) {
    const bool is_video = packet->stream_index == video;
    const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (is_video && stamp != AV_NOPTS_VALUE && (packet->flags & AV_PKT_FLAG_KEY)) {
      keyframe = stamp * av_q2d(stream->time_base);
    }
    av_packet_unref(packet);
    if (is_video) break;
  }
  av_packet_free(&packet);
  return keyframe;
}

}  // namespace

MediaDetails media_details(const std::string& path) {
  MediaDetails details;
  InputFile input;
  if (!open_file(path, input, details.reason)) return details;
  details.duration = file_duration(input.format);
  details.bytes = file_bytes(path);
  int64_t audio_bits = 0;
  for (unsigned i = 0; i < input.format->nb_streams; ++i) {
    AVStream* stream = input.format->streams[i];
    const AVCodecParameters* parameters = stream->codecpar;
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO && details.width == 0) {
      details.width = parameters->width;
      details.height = parameters->height;
      const AVRational sar = parameters->sample_aspect_ratio;
      details.sample_aspect_ratio = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
      const AVRational rate = av_guess_frame_rate(input.format, stream, nullptr);
      details.fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
      const AVCodecDescriptor* descriptor = avcodec_descriptor_get(parameters->codec_id);
      details.video_codec = descriptor ? descriptor->name : "unknown";
      details.video_kbps = static_cast<int>(parameters->bit_rate / 1000);
    } else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
      MediaTrack track;
      track.index = static_cast<int>(i);
      track.channels = parameters->ch_layout.nb_channels;
      track.sample_rate = parameters->sample_rate;
      const AVDictionaryEntry* title = av_dict_get(stream->metadata, "title", nullptr, 0);
      if (title && title->value) track.title = title->value;
      details.audio.push_back(track);
      audio_bits += parameters->bit_rate > 0 ? parameters->bit_rate : 160000;
    }
  }
  if (details.width == 0) {
    details.reason = "the file has no video";
    return details;
  }
  // Matroska rarely records a per-stream rate; the container's average less the audio is close.
  if (details.video_kbps <= 0 && details.duration > 0.0) {
    const double total = static_cast<double>(details.bytes) * 8.0 / details.duration;
    details.video_kbps = static_cast<int>(std::max(0.0, total - static_cast<double>(audio_bits)) / 1000.0);
  }
  details.ok = true;
  return details;
}

SequenceReport media_sequence_report(const std::string& path, double decode_seconds) {
  SequenceReport report;
  InputFile input;
  if (!open_file(path, input, report.reason)) return report;
  const int video = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) {
    report.reason = "the file has no video";
    return report;
  }
  AVStream* stream = input.format->streams[video];
  for (unsigned i = 0; i < input.format->nb_streams; ++i) {
    if (static_cast<int>(i) != video) input.format->streams[i]->discard = AVDISCARD_ALL;
  }
  CodecContext decoder;
  if (!open_decoder(stream, decoder, report.reason)) return report;
  const bool fix = video_packets_need_fixing(stream->codecpar);
  report.delta_histogram.assign(9, 0);
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int64_t previous = -1, first = -1, last = -1;
  double first_time = -1.0, last_time = 0.0;
  int run = 0;
  bool done = false;
  auto take = [&]() {
    while (avcodec_receive_frame(decoder.context, frame) >= 0) {
      const int64_t stamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
      const double at = stamp != AV_NOPTS_VALUE ? static_cast<double>(stamp) * av_q2d(stream->time_base) : 0.0;
      if (first_time < 0.0) first_time = at;
      if (at - first_time > decode_seconds) {
        done = true;
        av_frame_unref(frame);
        return;
      }
      ++report.decoded;
      const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
      const bool wide = description && description->comp[0].depth > 8;
      const int height = frame->height;
      const int cell = std::max(4, height / 36), margin = std::max(2, height / 180), pitch = cell + std::max(1, height / 360);
      int64_t value = 0;
      int guard = 0;
      for (int bit = 0; bit < 32; ++bit) {
        const int px = margin + bit * pitch + cell / 2, py = margin + cell / 2;
        const uint8_t* row = frame->data[0] + static_cast<ptrdiff_t>(py) * frame->linesize[0];
        const int luma = wide ? (reinterpret_cast<const uint16_t*>(row)[px] >> (description->comp[0].depth - 8)) : row[px];
        const int set = luma > 126 ? 1 : 0;
        if (bit < 28) value |= static_cast<int64_t>(set) << bit;
        else guard |= set << (bit - 28);
      }
      if (getenv("BUFFRX_SEQUENCE_VERBOSE")) std::fprintf(stderr, "t=%.4f value=%lld guard=%x\n", at, static_cast<long long>(value), guard);
      av_frame_unref(frame);
      if (guard != 0xA) {
        ++report.unreadable;
        continue;
      }
      if (first < 0) first = value;
      last = value;
      last_time = at;
      if (previous >= 0) {
        const int64_t delta = value - previous;
        if (delta < 0) {
          ++report.backwards;
        } else {
          const size_t bucket = static_cast<size_t>(std::min<int64_t>(delta, 8));
          ++report.delta_histogram[bucket];
          if (delta == 0) {
            ++report.repeats;
            report.worst_stall_frames = std::max(report.worst_stall_frames, ++run);
          } else {
            run = 0;
          }
        }
      }
      previous = value;
    }
  };
  while (!done && av_read_frame(input.format, packet) >= 0) {
    if (packet->stream_index == video && send_video_packet(decoder.context, packet, fix) >= 0) take();
    av_packet_unref(packet);
  }
  if (!done) {
    avcodec_send_packet(decoder.context, nullptr);
    take();
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  const int readable = report.decoded - report.unreadable;
  if (readable < 3 || first < 0) {
    report.reason = "the probe window was not visible in the recording";
    return report;
  }
  const double span = last_time - first_time;
  if (span > 0.0) {
    report.source_per_second = static_cast<double>(last - first) / span;
    report.recorded_fps = static_cast<double>(readable - 1) / span;
  }
  report.ok = true;
  return report;
}

FrameReport media_frame_report(const std::string& path, double decode_seconds) {
  FrameReport report;
  InputFile input;
  if (!open_file(path, input, report.reason)) return report;
  const int video = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) {
    report.reason = "the file has no video";
    return report;
  }
  AVStream* stream = input.format->streams[video];
  report.duration = file_duration(input.format);
  for (unsigned i = 0; i < input.format->nb_streams; ++i) {
    if (static_cast<int>(i) != video) input.format->streams[i]->discard = AVDISCARD_ALL;
  }

  // Pass one: every video packet's presentation time.
  std::vector<double> times;
  AVPacket* packet = av_packet_alloc();
  while (av_read_frame(input.format, packet) >= 0) {
    if (packet->stream_index == video) {
      const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
      if (stamp != AV_NOPTS_VALUE) times.push_back(static_cast<double>(stamp) * av_q2d(stream->time_base));
    }
    av_packet_unref(packet);
  }
  std::sort(times.begin(), times.end());
  report.packets = static_cast<int>(times.size());
  if (times.size() < 3) {
    av_packet_free(&packet);
    report.reason = "too few frames to measure";
    return report;
  }
  std::vector<double> deltas;
  for (size_t i = 1; i < times.size(); ++i) deltas.push_back(times[i] - times[i - 1]);
  std::vector<double> sorted = deltas;
  std::sort(sorted.begin(), sorted.end());
  const double nominal = sorted[sorted.size() / 2];
  report.nominal_fps = nominal > 0.0 ? 1.0 / nominal : 0.0;
  const double span = times.back() - times.front();
  report.average_fps = span > 0.0 ? static_cast<double>(times.size() - 1) / span : 0.0;
  std::vector<double> gaps;
  for (double delta : deltas) {
    if (nominal > 0.0 && delta > nominal * 1.5) {
      ++report.gaps;
      report.gap_seconds_total += delta - nominal;
      gaps.push_back(delta * 1000.0);
    }
  }
  std::sort(gaps.rbegin(), gaps.rend());
  for (size_t i = 0; i < gaps.size() && i < 5; ++i) report.worst_gaps_ms.push_back(gaps[i]);
  const int seconds = static_cast<int>(span);
  if (seconds >= 2) {
    std::vector<int> per_second(static_cast<size_t>(seconds), 0);
    for (double t : times) {
      const int at = static_cast<int>(t - times.front());
      if (at >= 0 && at < seconds) ++per_second[static_cast<size_t>(at)];
    }
    report.frames_per_second_min = *std::min_element(per_second.begin(), per_second.end());
  }

  // Pass two: decode the start and compare each frame with the one before it.
  CodecContext decoder;
  std::string decoder_error;
  const bool fix = video_packets_need_fixing(stream->codecpar);
  if (decode_seconds > 0.0 && open_decoder(stream, decoder, decoder_error)) {
    av_seek_frame(input.format, video, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(decoder.context);
    AVFrame* frame = av_frame_alloc();
    constexpr int kGridX = 96, kGridY = 54;
    std::vector<uint8_t> previous, current(static_cast<size_t>(kGridX) * kGridY);
    bool have_previous = false;
    int run = 1;
    double first_time = -1.0;
    bool done = false;
    auto take = [&]() {
      while (avcodec_receive_frame(decoder.context, frame) >= 0) {
        const int64_t stamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        const double at = stamp != AV_NOPTS_VALUE ? static_cast<double>(stamp) * av_q2d(stream->time_base) : 0.0;
        if (first_time < 0.0) first_time = at;
        if (at - first_time > decode_seconds) {
          done = true;
          av_frame_unref(frame);
          return;
        }
        const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        const bool wide = description && description->comp[0].depth > 8;
        for (int y = 0; y < kGridY; ++y) {
          for (int x = 0; x < kGridX; ++x) {
            const int px = x * frame->width / kGridX, py = y * frame->height / kGridY;
            const uint8_t* row = frame->data[0] + static_cast<ptrdiff_t>(py) * frame->linesize[0];
            const int value = wide ? (reinterpret_cast<const uint16_t*>(row)[px] >> (description->comp[0].depth - 8)) : row[px];
            current[static_cast<size_t>(y) * kGridX + x] = static_cast<uint8_t>(value);
          }
        }
        ++report.decoded;
        if (have_previous) {
          long difference = 0;
          for (size_t i = 0; i < current.size(); ++i) difference += std::abs(static_cast<int>(current[i]) - static_cast<int>(previous[i]));
          const double mean = static_cast<double>(difference) / static_cast<double>(current.size());
          if (mean < 0.25) {
            ++report.repeats;
            ++run;
          } else {
            if (run > 1) {
              if (report.repeat_runs.size() < static_cast<size_t>(run)) report.repeat_runs.resize(static_cast<size_t>(run), 0);
              ++report.repeat_runs[static_cast<size_t>(run - 1)];
            }
            run = 1;
          }
        }
        previous = current;
        have_previous = true;
        av_frame_unref(frame);
      }
    };
    while (!done && av_read_frame(input.format, packet) >= 0) {
      if (packet->stream_index == video && send_video_packet(decoder.context, packet, fix) >= 0) take();
      av_packet_unref(packet);
    }
    if (!done) {
      avcodec_send_packet(decoder.context, nullptr);
      take();
    }
    av_frame_free(&frame);
  }
  av_packet_free(&packet);
  report.ok = true;
  return report;
}

bool media_audio_track(const std::string& path, int track, const std::string& out, std::string& error) {
  std::error_code fs_error;
  const auto source = std::filesystem::u8path(path);
  const auto target = std::filesystem::u8path(out);
  if (std::filesystem::exists(target, fs_error) &&
      std::filesystem::last_write_time(target, fs_error) >= std::filesystem::last_write_time(source, fs_error) && !fs_error) {
    return true;
  }
  InputFile input;
  if (!open_file(path, input, error)) return false;
  int stream_index = -1;
  for (unsigned i = 0, seen = 0; i < input.format->nb_streams; ++i) {
    if (input.format->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
    if (static_cast<int>(seen++) == track) stream_index = static_cast<int>(i);
  }
  if (stream_index < 0) {
    error = "the file has no audio track " + std::to_string(track + 1);
    return false;
  }
  AVStream* in = input.format->streams[stream_index];
  std::filesystem::create_directories(target.parent_path(), fs_error);
  const std::string part = out + ".part";
  AVFormatContext* output = nullptr;
  if (avformat_alloc_output_context2(&output, nullptr, "mp4", part.c_str()) < 0 || !output) {
    error = "the audio file could not be made";
    return false;
  }
  struct Owner {
    AVFormatContext* output;
    ~Owner() {
      if (output->pb) avio_closep(&output->pb);
      avformat_free_context(output);
    }
  } owner{output};
  AVStream* copy = avformat_new_stream(output, nullptr);
  if (!copy || avcodec_parameters_copy(copy->codecpar, in->codecpar) < 0) {
    error = "out of memory";
    return false;
  }
  copy->codecpar->codec_tag = 0;
  copy->time_base = in->time_base;
  if (avio_open(&output->pb, part.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(output, nullptr) < 0) {
    error = "the audio file could not be written";
    return false;
  }
  // The clip's time zero: the earliest start of any stream, which is what the video element calls 0.
  const int64_t origin = input.format->start_time != AV_NOPTS_VALUE
                             ? av_rescale_q(input.format->start_time, AV_TIME_BASE_Q, in->time_base)
                             : 0;
  AVPacket* packet = av_packet_alloc();
  bool ok = packet != nullptr;
  while (ok && av_read_frame(input.format, packet) >= 0) {
    if (packet->stream_index == stream_index) {
      if (packet->pts != AV_NOPTS_VALUE) packet->pts -= origin;
      if (packet->dts != AV_NOPTS_VALUE) packet->dts -= origin;
      av_packet_rescale_ts(packet, in->time_base, copy->time_base);
      packet->stream_index = 0;
      packet->pos = -1;
      ok = av_interleaved_write_frame(output, packet) >= 0;
    }
    av_packet_unref(packet);
  }
  av_packet_free(&packet);
  if (!ok || av_write_trailer(output) < 0) {
    error = "the audio file could not be written";
    avio_closep(&output->pb);
    DeleteFileA(part.c_str());
    return false;
  }
  avio_closep(&output->pb);
  if (!MoveFileExA(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileA(part.c_str());
    error = "the audio file could not be saved";
    return false;
  }
  // The folder is a cache: a copy nobody opened for a week goes, so it never grows without end.
  const auto week_ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 7);
  for (const auto& entry : std::filesystem::directory_iterator(target.parent_path(), fs_error)) {
    std::error_code entry_error;
    if (entry.path().extension() == ".m4a" && entry.last_write_time(entry_error) < week_ago && !entry_error) {
      std::filesystem::remove(entry.path(), entry_error);
    }
  }
  return true;
}

bool media_peaks(const std::string& path, int buckets, std::vector<std::vector<float>>& tracks, std::string& error) {
  tracks.clear();
  buckets = std::clamp(buckets, 16, 4000);
  InputFile input;
  if (!open_file(path, input, error)) return false;
  const double duration = file_duration(input.format);
  if (duration <= 0.0) {
    error = "the file has no duration";
    return false;
  }
  std::vector<int> stream_track(input.format->nb_streams, -1);
  std::vector<std::unique_ptr<CodecContext>> decoders;
  for (unsigned i = 0; i < input.format->nb_streams; ++i) {
    AVStream* stream = input.format->streams[i];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
      stream->discard = AVDISCARD_ALL;  // the waveform never needs a video frame
      continue;
    }
    auto decoder = std::make_unique<CodecContext>();
    std::string decoder_error;
    if (!open_decoder(stream, *decoder, decoder_error)) continue;
    stream_track[i] = static_cast<int>(decoders.size());
    decoders.push_back(std::move(decoder));
    tracks.emplace_back(static_cast<size_t>(buckets), 0.0f);
  }
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  auto take = [&](int track, AVStream* stream) {
    AVCodecContext* decoder = decoders[static_cast<size_t>(track)]->context;
    while (avcodec_receive_frame(decoder, frame) >= 0) {
      const int64_t stamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
      const double start = stamp != AV_NOPTS_VALUE ? stamp * av_q2d(stream->time_base) : 0.0;
      const int channels = std::max(1, frame->ch_layout.nb_channels);
      const AVSampleFormat format = static_cast<AVSampleFormat>(frame->format);
      const bool planar = av_sample_fmt_is_planar(format) != 0;
      for (int s = 0; s < frame->nb_samples; ++s) {
        float peak = 0.0f;
        for (int c = 0; c < channels; ++c) {
          const int plane = planar ? c : 0;
          const int at = planar ? s : s * channels + c;
          float value = 0.0f;
          switch (av_get_packed_sample_fmt(format)) {
            case AV_SAMPLE_FMT_FLT: value = reinterpret_cast<const float*>(frame->extended_data[plane])[at]; break;
            case AV_SAMPLE_FMT_S16: value = reinterpret_cast<const int16_t*>(frame->extended_data[plane])[at] / 32768.0f; break;
            case AV_SAMPLE_FMT_S32: value = static_cast<float>(reinterpret_cast<const int32_t*>(frame->extended_data[plane])[at] / 2147483648.0); break;
            case AV_SAMPLE_FMT_DBL: value = static_cast<float>(reinterpret_cast<const double*>(frame->extended_data[plane])[at]); break;
            default: break;
          }
          peak = std::max(peak, std::fabs(value));
        }
        const double t = start + static_cast<double>(s) / std::max(1, frame->sample_rate);
        const int bucket = std::clamp(static_cast<int>(t / duration * buckets), 0, buckets - 1);
        float& slot = tracks[static_cast<size_t>(track)][static_cast<size_t>(bucket)];
        slot = std::max(slot, peak);
      }
      av_frame_unref(frame);
    }
  };
  while (av_read_frame(input.format, packet) >= 0) {
    const int track = stream_track[static_cast<size_t>(packet->stream_index)];
    if (track >= 0 && avcodec_send_packet(decoders[static_cast<size_t>(track)]->context, packet) >= 0) {
      take(track, input.format->streams[packet->stream_index]);
    }
    av_packet_unref(packet);
  }
  for (unsigned i = 0; i < input.format->nb_streams; ++i) {
    const int track = stream_track[i];
    if (track < 0) continue;
    avcodec_send_packet(decoders[static_cast<size_t>(track)]->context, nullptr);
    take(track, input.format->streams[i]);
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  // Each lane normalised to its own loudest point, so a quiet microphone track still reads.
  for (std::vector<float>& lane : tracks) {
    const float loudest = *std::max_element(lane.begin(), lane.end());
    if (loudest > 0.0f) {
      for (float& value : lane) value /= loudest;
    }
  }
  return true;
}

// Puts the verified export in place. "Replace original" never overwrites in one step: the original is
// renamed aside first, so a failure at any point leaves one of the two files whole, and the aside file
// goes to the Recycle Bin rather than being deleted.
bool put_export_in_place(const ExportJob& job, const std::string& part, std::string& error) {
  const bool replacing = job.replace_original && GetFileAttributesA(job.output.c_str()) != INVALID_FILE_ATTRIBUTES;
  std::string aside = job.output + ".old-" + std::to_string(GetCurrentProcessId());
  for (int n = 1; GetFileAttributesA(aside.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n) {
    aside = job.output + ".old-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(n);
  }
  if (GetFileAttributesA(aside.c_str()) != INVALID_FILE_ATTRIBUTES) {
    DeleteFileA(part.c_str());
    error = "the original could not be set aside; it was left as it was";
    return false;
  }
  if (replacing && !MoveFileExA(job.output.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileA(part.c_str());
    error = "the original could not be set aside; it was left as it was";
    return false;
  }
  if (!MoveFileExA(part.c_str(), job.output.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    if (replacing) MoveFileExA(aside.c_str(), job.output.c_str(), MOVEFILE_REPLACE_EXISTING);
    DeleteFileA(part.c_str());
    error = "the export could not be put in place; the original was left as it was";
    return false;
  }
  library_forget_cached_images(job.output);
  if (replacing) library_trash(aside);  // a refusal leaves the old file beside the clip, never lost
  return true;
}

bool export_once(const ExportJob& job, const std::function<void(double)>& progress, std::string& encoder_name,
                 std::string& error, bool allow_hardware, bool force_reencode = false) {
  encoder_name.clear();
  InputFile input;
  if (!open_file(job.input, input, error)) return false;
  const double duration = file_duration(input.format);
  // The parts the cut keeps, in order. A plain trim is one part.
  const std::vector<std::pair<double, double>> segments = export_segments(job, duration);
  const double total = segments_length(segments);
  if (segments.empty() || total < 0.1) {
    error = "the selection is too short";
    return false;
  }
  const double start = segments.front().first;
  const double end = segments.back().second;
  const std::string part = job.output + ".part";
  const size_t extension = job.output.find_last_of('.');
  const std::string staged = job.output + ".part" + (extension == std::string::npos ? "" : job.output.substr(extension));
  std::error_code path_error;
  if (std::filesystem::equivalent(job.input, part, path_error) ||
      std::filesystem::equivalent(job.input, staged, path_error)) {
    error = "the export temporary file is the clip it reads";
    return false;
  }
  DeleteFileA(part.c_str());
  // The library's listing skips any name with ".part" in it, so a temporary left behind by a failed
  // export would sit in the clips folder, unseen, at the clip's own size. Both success paths move
  // their temporary into place first, so deleting a name that is no longer there costs nothing.
  struct TemporaryOwner {
    const std::string& part;
    const std::string& staged;
    ~TemporaryOwner() {
      DeleteFileA(part.c_str());
      DeleteFileA(staged.c_str());
    }
  } temporaries{part, staged};
  ExportJob planned = job;
  planned.segments = segments;
  ExportPlan plan = export_plan(planned);
  // The fallback from a cut that could not be hidden: the same export with the video re-encoded.
  if (force_reencode) plan = ExportPlan::reencode;
  const int video_index = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_index < 0) {
    error = "the file has no video";
    return false;
  }
  // A copy begins on the keyframe at or before the cut whatever it is, and the frames between that
  // keyframe and the cut are hidden by an edit list, so the picture starts where the user cut it. A
  // keyframe further back than an edit list may hide is re-encoded instead: never a wrong file.
  const double frame_seconds = frame_length(input.format, video_index);
  const double keyframe = keyframe_before(input.format, video_index, start);
  // A seek that found no keyframe is not "a cut on a keyframe": it goes to the re-encode, where the
  // range is decoded and the cut is exact.
  const double probed_head =
      edit_head_seconds(start, keyframe >= 0.0 ? keyframe : start, frame_seconds);
  if (std::isnan(probed_head) || (keyframe < 0.0 && start > 0.001)) plan = ExportPlan::reencode;

  if (plan == ExportPlan::copy) {
    avformat_close_input(&input.format);
    encoder_name = "copy";
    if (progress) progress(0.0);
    // The muxer is chosen from the real name, written to the .part file through a same-format copy.
    double copy_keyframe = -1.0;
    if (!media_concat({job.input}, staged, start, end, error, &copy_keyframe)) return false;
    // The head is measured against the keyframe the copy itself began on, not the probe's.
    const double head = edit_head_seconds(start, copy_keyframe < 0.0 ? start : copy_keyframe, frame_seconds);
    const bool small_head = !std::isnan(head);
    if (small_head && (head <= 0.0 || mp4_head_trim(staged, head, error))) {
      if (!put_export_in_place(job, staged, error)) return false;
      if (progress) progress(1.0);
      return true;
    }
    // The keyframe this copy landed on cannot be hidden, or the index would not take it: the copy is
    // thrown away and the same range re-encoded, so the cut is honoured however slow that is.
    DeleteFileA(staged.c_str());
    encoder_name.clear();
    error.clear();
    if (!open_file(job.input, input, error)) return false;
    plan = ExportPlan::reencode;
  }

  AVStream* video_in = input.format->streams[video_index];
  const bool reencode_video = plan == ExportPlan::reencode;
  const bool fix_video = video_packets_need_fixing(video_in->codecpar);

  // The format comes from the real name, the context's own url is the .part file: +faststart re-opens
  // the file by that url to move the index to the front.
  AVFormatContext* output = nullptr;
  const AVOutputFormat* format = av_guess_format(nullptr, job.output.c_str(), nullptr);
  if (!format || avformat_alloc_output_context2(&output, format, nullptr, part.c_str()) < 0 || !output) {
    error = "the output format is not supported";
    return false;
  }
  struct OutputOwner {
    AVFormatContext*& output;
    ~OutputOwner() {
      if (output) {
        if (output->pb) avio_closep(&output->pb);
        avformat_free_context(output);
      }
    }
  } output_owner{output};
  const bool global_header = (output->oformat->flags & AVFMT_GLOBALHEADER) != 0;

  // ---- video
  CodecContext video_decoder;
  CodecContext video_encoder;
  FilterGraph video_graph;
  AVStream* video_out = avformat_new_stream(output, nullptr);
  if (!video_out) {
    error = "out of memory";
    return false;
  }
  if (reencode_video) {
    if (!learn_video_format(input.format, video_index)) {
      error = "the video could not be decoded";
      return false;
    }
    if (!open_decoder(video_in, video_decoder, error, allow_hardware)) return false;
    const int source_width = video_in->codecpar->width;
    const int source_height = video_in->codecpar->height;
    int crop_x = 0, crop_y = 0, crop_w = source_width, crop_h = source_height;
    const bool cropped = export_crop(source_width, source_height, job.crop_x, job.crop_y, job.crop_width,
                                     job.crop_height, crop_x, crop_y, crop_w, crop_h);
    int out_w = crop_w, out_h = crop_h;
    if (job.out_width > 0 && job.out_height > 0) {
      out_w = job.out_width & ~1;
      out_h = job.out_height & ~1;
    }
    AVRational frame_rate = av_guess_frame_rate(input.format, video_in, nullptr);
    if (job.fps > 0.0) frame_rate = av_d2q(job.fps, 100000);
    const MediaDetails details = media_details(job.input);
    const bool held = job.video_kbps > 0;
    const int kbps = held ? job.video_kbps
                          : export_video_kbps(details.video_kbps, source_width * source_height, out_w * out_h, job.quality);
    // The clip's own codec is kept only for an Original save: a size setting asks for a smaller file,
    // and H.264 is what every player and Discord take. A share fit is H.264 by the same rule. A join
    // names its common codec, because every part must carry the same stream parameters.
    const std::string source_codec =
        !job.video_codec.empty() ? job.video_codec
                                 : (job.quality > 0.0 ? std::string() : avcodec_get_name(video_in->codecpar->codec_id));
    // A join pins the first part's encoder so later parts carry the same profile. Re-picking could land
    // on another vendor's encoder when a session limit is reached, which concat refuses.
    const std::vector<std::string> candidates = job.video_encoder.empty()
                                                    ? video_encoder_candidates(source_codec, held)
                                                    : std::vector<std::string>{job.video_encoder};
    // The encoder works in the requested output rate, not the source's: the MOV muxer takes its
    // track timescale from the encoder, so this is what keeps two join parts on one time base.
    const AVRational out_tb = job.fps > 0.0 ? av_inv_q(frame_rate) : video_in->time_base;
    if (!open_video_encoder(out_w, out_h, out_tb, frame_rate, kbps, held, candidates, global_header,
                            video_encoder, encoder_name, error)) {
      return false;
    }
    std::string chain;
    if (cropped) chain += "crop=" + std::to_string(crop_w) + ":" + std::to_string(crop_h) + ":" +
                          std::to_string(crop_x) + ":" + std::to_string(crop_y) + ",";
    // The editor's three numbers, as eq takes them: brightness is a -1..1 shift, the other two a factor.
    if (export_graded(job)) {
      char eq[160];
      std::snprintf(eq, sizeof(eq), "eq=brightness=%.4f:contrast=%.4f:saturation=%.4f,",
                    job.grade_brightness / 100.0 - 1.0, job.grade_contrast / 100.0, job.grade_saturation / 100.0);
      chain += eq;
    }
    // A join fits the whole picture inside the first clip's frame and pads the rest, never stretches it.
    if (job.fit_pad) {
      chain += "scale=" + std::to_string(out_w) + ":" + std::to_string(out_h) +
               ":force_original_aspect_ratio=decrease:flags=lanczos,pad=" + std::to_string(out_w) + ":" +
               std::to_string(out_h) + ":(ow-iw)/2:(oh-ih)/2:color=black,";
    } else if (out_w != crop_w || out_h != crop_h) {
      chain += "scale=" + std::to_string(out_w) + ":" + std::to_string(out_h) + ":flags=lanczos,";
    }
    // The retime is one timestamp scale; the frames themselves are untouched, so quality is too. It
    // runs first and on a fine time base: a half tick of a coarse one (1/60 divided by a speed of 2)
    // rounds two frames onto one stamp, and the muxer refuses a dts that does not increase.
    if (export_retimed(job)) {
      char setpts[80];
      std::snprintf(setpts, sizeof(setpts), "settb=1/90000,setpts=PTS/%.6f,",
                    std::min(4.0, std::max(0.25, job.speed)));
      chain += setpts;
    }
    if (job.fps > 0.0) {
      chain += "fps=" + std::to_string(frame_rate.num) + "/" + std::to_string(frame_rate.den) +
               ",settb=" + std::to_string(out_tb.num) + "/" + std::to_string(out_tb.den) + ",";
    }
    if (job.square_pixels) chain += "setsar=1,";
    chain += std::string("format=") + av_get_pix_fmt_name(video_encoder.context->pix_fmt);
    const AVRational sar = video_in->codecpar->sample_aspect_ratio.num > 0 ? video_in->codecpar->sample_aspect_ratio
                                                                           : AVRational{1, 1};
    char args[256];
    std::snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", source_width,
                  source_height, decoder_software_format(video_decoder.context), video_in->time_base.num,
                  video_in->time_base.den, sar.num, sar.den);
    if (!build_graph(video_graph, "buffer", args, "buffersink", chain, error)) return false;
    avcodec_parameters_from_context(video_out->codecpar, video_encoder.context);
    video_out->time_base = out_tb;
    video_out->avg_frame_rate = frame_rate;
    video_out->r_frame_rate = frame_rate;
  } else {
    // A volume change only: the picture is copied untouched.
    avcodec_parameters_copy(video_out->codecpar, video_in->codecpar);
    video_out->codecpar->codec_tag = 0;
    video_out->time_base = video_in->time_base;
    encoder_name = "copy";
  }

  // ---- audio: every track re-encoded through its own volume; a track at 0 is dropped
  std::vector<std::unique_ptr<AudioPipe>> audio;
  int track_number = 0;
  for (unsigned i = 0; i < input.format->nb_streams; ++i) {
    AVStream* stream = input.format->streams[i];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
    const double volume = static_cast<size_t>(track_number) < job.volumes.size()
                              ? job.volumes[static_cast<size_t>(track_number)]
                              : 1.0;
    ++track_number;
    if (volume <= 0.0001) continue;
    auto pipe = std::make_unique<AudioPipe>();
    pipe->input_index = static_cast<int>(i);
    pipe->volume = std::min(volume, 4.0);
    if (!open_decoder(stream, pipe->decoder, error)) return false;
    const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    pipe->encoder.context = aac ? avcodec_alloc_context3(aac) : nullptr;
    if (!pipe->encoder.context) {
      error = "no AAC encoder";
      return false;
    }
    AVCodecContext* e = pipe->encoder.context;
    int channels = pipe->decoder.context->ch_layout.nb_channels;
    if (job.audio_channels == 1) channels = 1;
    else if (job.audio_channels >= 2) channels = 2;
    channels = std::min(2, std::max(1, channels));
    const int rate = job.audio_rate > 0
                         ? job.audio_rate
                         : (pipe->decoder.context->sample_rate > 0 ? pipe->decoder.context->sample_rate : 48000);
    e->sample_rate = rate;
    e->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&e->ch_layout, channels);
    e->bit_rate = job.audio_kbps > 0 ? static_cast<int64_t>(job.audio_kbps) * 1000 : 160000;
    e->time_base = AVRational{1, rate};
    if (global_header) e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(e, aac, nullptr) < 0) {
      error = "the AAC encoder would not open";
      return false;
    }
    char layout_in[128] = {0};
    char layout_out[128] = {0};
    if (av_channel_layout_describe(&pipe->decoder.context->ch_layout, layout_in, sizeof(layout_in)) < 0) {
      // An unspecified order (AV_CHANNEL_ORDER_UNSPEC) has no name to describe: build a default layout of
      // the same width instead, because an empty channel_layout= is refused by abuffer.
      AVChannelLayout fallback = {};
      av_channel_layout_default(&fallback, std::max(1, pipe->decoder.context->ch_layout.nb_channels));
      av_channel_layout_describe(&fallback, layout_in, sizeof(layout_in));
      av_channel_layout_uninit(&fallback);
    }
    av_channel_layout_describe(&e->ch_layout, layout_out, sizeof(layout_out));
    char args[320];
    std::snprintf(args, sizeof(args), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s", rate, rate,
                  av_get_sample_fmt_name(pipe->decoder.context->sample_fmt), layout_in);
    char chain[512];
    std::snprintf(chain, sizeof(chain), "volume=%.4f,%saformat=sample_fmts=fltp:sample_rates=%d:channel_layouts=%s",
                  pipe->volume, atempo_chain(job.speed).c_str(), rate, layout_out);
    if (!build_graph(pipe->graph, "abuffer", args, "abuffersink", chain, error)) return false;
    av_buffersink_set_frame_size(pipe->graph.sink, static_cast<unsigned>(e->frame_size > 0 ? e->frame_size : 1024));
    pipe->output = avformat_new_stream(output, nullptr);
    if (!pipe->output) {
      error = "out of memory";
      return false;
    }
    avcodec_parameters_from_context(pipe->output->codecpar, e);
    pipe->output->time_base = e->time_base;
    if (const AVDictionaryEntry* title = av_dict_get(stream->metadata, "title", nullptr, 0)) {
      av_dict_set(&pipe->output->metadata, "title", title->value, 0);
    }
    audio.push_back(std::move(pipe));
  }

  if (avio_open(&output->pb, part.c_str(), AVIO_FLAG_WRITE) < 0) {
    error = "the export file could not be created";
    return false;
  }
  AVDictionary* mux_options = nullptr;
  // The index goes first only for a share fit, which is played while it downloads; a local save is read
  // from the folder, so it writes the index once, at the end (and the head trim in Task 8 needs that).
  if (export_wants_faststart(job)) av_dict_set(&mux_options, "movflags", "+faststart", 0);
  const int header = avformat_write_header(output, &mux_options);
  av_dict_free(&mux_options);
  if (header < 0) {
    error = "the export header could not be written: " + av_text(header);
    return false;
  }


  // The clip's time zero for the copied picture: the keyframe the copy begins on (a copy cannot start
  // mid-GOP; the plan above made sure that keyframe is the requested start). A re-encode maps every
  // part onto the output's own clock instead, so the parts follow each other with no gap.
  double origin = -1.0;
  AVPacket* packet = av_packet_alloc();
  AVPacket* out_packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  AVFrame* filtered = av_frame_alloc();
  AVFrame* software = av_frame_alloc();
  struct Frees {
    AVPacket*& a;
    AVPacket*& b;
    AVFrame*& c;
    AVFrame*& d;
    AVFrame*& e;
    ~Frees() {
      av_packet_free(&a);
      av_packet_free(&b);
      av_frame_free(&c);
      av_frame_free(&d);
      av_frame_free(&e);
    }
  } frees{packet, out_packet, frame, filtered, software};
  if (!packet || !out_packet || !frame || !filtered || !software) {
    error = "out of memory";
    return false;
  }

  double last_reported = -1.0;
  auto report = [&](double written) {
    if (!progress) return;
    const double share = std::clamp(written / std::max(0.1, total), 0.0, 1.0);
    if (share - last_reported >= 0.01) {
      last_reported = share;
      progress(share);
    }
  };

  // The part being read: its range in the file and where it lands in the output.
  double part_from = start;
  double part_to = end;
  double part_offset = 0.0;
  bool video_done = false;
  int64_t last_video_pts = INT64_MIN;
  std::vector<int64_t> next_audio_pts(audio.size(), 0);

  auto push_video_frames = [&]() -> bool {
    while (true) {
      const int received = avcodec_receive_frame(video_decoder.context, frame);
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
      if (received < 0) {
        // A hardware decoder that opened but cannot deliver this stream is retried in software once.
        if (video_decoder.context->hw_device_ctx) g_hardware_decode_failed = true;
        error = "the video could not be decoded";
        return false;
      }
      const int64_t stamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
      const double t = stamp != AV_NOPTS_VALUE ? stamp * av_q2d(video_in->time_base) : part_from;
      if (t < part_from - 0.0005 || video_done) {
        av_frame_unref(frame);
        continue;
      }
      if (t >= part_to) {
        video_done = true;
        av_frame_unref(frame);
        continue;
      }
      const double at = part_offset + (t - part_from);
      int64_t pts = std::llround(at / av_q2d(video_in->time_base));
      if (last_video_pts != INT64_MIN && pts <= last_video_pts) pts = last_video_pts + 1;  // never backwards at a join
      last_video_pts = pts;
      // A GPU decoder hands back a CUDA frame; the filter graph is fed software pixels.
      if (!frame_to_software(frame, software, error)) {
        av_frame_unref(frame);
        return false;
      }
      AVFrame* picture = software->format != AV_PIX_FMT_NONE ? software : frame;
      picture->pts = pts;
      report(at);
      const int added = av_buffersrc_add_frame_flags(video_graph.source, picture, AV_BUFFERSRC_FLAG_KEEP_REF);
      av_frame_unref(frame);
      if (added < 0) {
        error = "the picture could not be filtered";
        return false;
      }
      if (!drain_filter(video_graph, video_encoder.context, output, video_out, filtered, out_packet, error)) return false;
    }
  };

  auto push_audio_frames = [&](size_t which) -> bool {
    AudioPipe& pipe = *audio[which];
    AVStream* stream = input.format->streams[pipe.input_index];
    // The copied picture starts on its keyframe; the sound starts with it.
    const double base = reencode_video ? part_from : origin;
    while (true) {
      const int received = avcodec_receive_frame(pipe.decoder.context, frame);
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
      if (received < 0) return true;  // a damaged audio packet is skipped, not fatal
      const int64_t stamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
      const double t = stamp != AV_NOPTS_VALUE ? stamp * av_q2d(stream->time_base) : base;
      if (base < 0.0 || t < base || t >= part_to || pipe.done) {
        if (t >= part_to) pipe.done = true;
        av_frame_unref(frame);
        continue;
      }
      // On the output's clock, and never overlapping the frame before: at a join the sound of the next
      // part follows the last sample of the one before it.
      const int rate = pipe.encoder.context->sample_rate;
      int64_t pts = std::llround((part_offset + (t - base)) * rate);
      pts = std::max(pts, next_audio_pts[which]);
      next_audio_pts[which] = pts + frame->nb_samples;
      frame->pts = pts;
      const int added = av_buffersrc_add_frame_flags(pipe.graph.source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
      av_frame_unref(frame);
      if (added < 0) {
        error = "the audio could not be filtered";
        return false;
      }
      if (!drain_filter(pipe.graph, pipe.encoder.context, output, pipe.output, filtered, out_packet, error)) return false;
    }
  };

  auto part_finished = [&]() {
    if (!video_done) return false;
    for (const auto& pipe : audio) {
      if (!pipe->done) return false;
    }
    return true;
  };

  bool reached_end_of_file = false;
  for (size_t index = 0; index < segments.size() && !reached_end_of_file; ++index) {
    part_from = segments[index].first;
    part_to = segments[index].second;
    video_done = false;
    for (auto& pipe : audio) pipe->done = false;
    // Each part is read from the keyframe before it: the frames between that keyframe and the cut are
    // decoded (a frame needs the ones before it) and then dropped.
    if (av_seek_frame(input.format, -1, static_cast<int64_t>(part_from * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0) {
      av_seek_frame(input.format, -1, 0, AVSEEK_FLAG_BACKWARD);
    }
    if (reencode_video) avcodec_flush_buffers(video_decoder.context);
    for (auto& pipe : audio) avcodec_flush_buffers(pipe->decoder.context);

    while (!part_finished()) {
      if (av_read_frame(input.format, packet) < 0) {
        reached_end_of_file = true;
        break;
      }
      AVStream* stream = input.format->streams[packet->stream_index];
      if (packet->stream_index == video_index) {
        if (reencode_video) {
          if (send_video_packet(video_decoder.context, packet, fix_video) >= 0 && !push_video_frames()) {
            av_packet_unref(packet);
            return false;
          }
        } else if (!video_done) {
          const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
          const double t = stamp != AV_NOPTS_VALUE ? stamp * av_q2d(stream->time_base) : 0.0;
          if (origin < 0.0) {
            if (!(packet->flags & AV_PKT_FLAG_KEY)) {
              av_packet_unref(packet);
              continue;
            }
            origin = t;  // the copy starts on this keyframe
          }
          if (t >= part_to) {
            video_done = true;
          } else {
            const int64_t shift = std::llround(origin / av_q2d(stream->time_base));
            if (packet->pts != AV_NOPTS_VALUE) packet->pts -= shift;
            if (packet->dts != AV_NOPTS_VALUE) packet->dts -= shift;
            report(t - origin);
            if (!write_packet(output, packet, stream->time_base, video_out, error)) {
              av_packet_unref(packet);
              return false;
            }
          }
        }
      } else {
        for (size_t which = 0; which < audio.size(); ++which) {
          if (audio[which]->input_index != packet->stream_index) continue;
          if (avcodec_send_packet(audio[which]->decoder.context, packet) >= 0 && !push_audio_frames(which)) {
            av_packet_unref(packet);
            return false;
          }
        }
      }
      av_packet_unref(packet);
    }
    if (reached_end_of_file) {
      // The last part ran to the end of the file: what the decoders still hold belongs to it.
      if (reencode_video) {
        avcodec_send_packet(video_decoder.context, nullptr);
        if (!push_video_frames()) return false;
      }
      for (size_t which = 0; which < audio.size(); ++which) {
        avcodec_send_packet(audio[which]->decoder.context, nullptr);
        if (!push_audio_frames(which)) return false;
      }
    }
    part_offset += part_to - part_from;
  }

  // Flush: the filter graphs, then the encoders.
  if (reencode_video) {
    (void)av_buffersrc_add_frame_flags(video_graph.source, nullptr, 0);  // the flush: a failure shows in the drain below
    if (!drain_filter(video_graph, video_encoder.context, output, video_out, filtered, out_packet, error)) return false;
    if (!encode_frame(video_encoder.context, nullptr, output, video_out, out_packet, error)) return false;
  }
  for (auto& pipe : audio) {
    (void)av_buffersrc_add_frame_flags(pipe->graph.source, nullptr, 0);
    if (!drain_filter(pipe->graph, pipe->encoder.context, output, pipe->output, filtered, out_packet, error)) return false;
    if (!encode_frame(pipe->encoder.context, nullptr, output, pipe->output, out_packet, error)) return false;
  }
  if (av_write_trailer(output) < 0) {
    error = "the export could not be finished";
    return false;
  }
  avio_closep(&output->pb);
  // The input is closed before the rename: "Replace original" renames over the very file it read.
  avformat_close_input(&input.format);

  // A volume-only save copies the picture from the keyframe as well, so it carries the same head; it
  // is hidden here too, or the file would play the footage that was cut away. `origin` is the keyframe
  // this file actually starts on.
  if (!reencode_video && origin >= 0.0) {
    const double head = edit_head_seconds(start, origin, frame_seconds);
    if (std::isnan(head) || (head > 0.0 && !mp4_head_trim(part, head, error))) {
      // The keyframe is further back than an edit list may hide, or this container has no edit list to
      // write: the part is thrown away and the same range re-encoded, so the picture still starts where
      // the user cut it. Once only - the re-encode is the same export with the video re-encoded.
      DeleteFileA(part.c_str());
      encoder_name.clear();
      error.clear();
      if (export_once(job, progress, encoder_name, error, allow_hardware, /*force_reencode=*/true)) return true;
      if (error.empty()) error = "the cut could not be hidden; re-encoding instead";
      return false;
    }
  }

  // Verified before it is put in place, the same way a saved clip is.
  const ClipCheck check = check_clip_file(part);
  if (!check.ok) {
    DeleteFileA(part.c_str());
    error = "the export did not verify: " + check.reason;
    return false;
  }
  if (!put_export_in_place(job, part, error)) return false;
  if (progress) progress(1.0);
  return true;
}

namespace {

std::filesystem::path utf8_path(const std::string& text) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string utf8_text(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// One join: each input is trimmed and re-encoded to the same target, then the parts are copied end
// to end. The target is the size chosen for the export, else the first input's own, so a join never
// changes the first clip's shape; every part pins the first part's encoder, because concat refuses
// streams whose SPS/PPS differ.
bool export_stitch(const ExportJob& job, const std::function<void(double)>& progress, std::string& encoder_name,
                   std::string& error) {
  std::vector<StitchSource> sources;
  sources.reserve(job.inputs.size());
  for (const ExportInput& input : job.inputs) {
    const MediaDetails details = media_details(input.input);
    if (!details.ok) {
      error = "a clip in the join could not be read: " + input.input + " (" + details.reason + ")";
      return false;
    }
    StitchSource source;
    source.path = input.input;
    source.duration = details.duration;
    source.width = details.width;
    source.height = details.height;
    source.fps = details.fps;
    source.audio_tracks = static_cast<int>(details.audio.size());
    source.start = input.start;
    source.end = input.end;
    source.segments = input.segments;
    source.speed = input.speed;
    sources.push_back(std::move(source));
  }
  StitchPlan plan;
  if (!make_stitch_plan(sources, job, plan, error)) return false;

  const std::filesystem::path output = utf8_path(job.output);
  const std::filesystem::path work =
      output.parent_path() / ".buffrx" / "stitch" /
      (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
  std::error_code made;
  std::filesystem::create_directories(work, made);
  if (made) {
    error = "the join's work folder could not be made: " + made.message();
    return false;
  }
  struct Cleanup {
    std::filesystem::path work;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(work, ignored);
    }
  } cleanup{work};

  const size_t count = sources.size();
  std::vector<std::string> parts;
  parts.reserve(count);
  std::string first_encoder;
  for (size_t i = 0; i < count; ++i) {
    ExportJob one;
    one.input = sources[i].path;
    one.segments = plan.parts[i];
    one.output = utf8_text(work / ("part-" + std::to_string(i) + ".mp4"));
    one.out_width = plan.width;
    one.out_height = plan.height;
    one.square_pixels = true;
    one.fps = plan.fps;
    one.quality = job.quality;
    one.video_codec = job.video_codec.empty() ? "h264" : job.video_codec;
    one.video_encoder = i == 0 ? std::string() : first_encoder;
    one.fit_pad = true;
    one.audio_rate = plan.has_audio ? 48000 : 0;
    one.audio_channels = plan.has_audio ? 2 : 0;
    one.audio_kbps = job.audio_kbps;
    // The clip's own edit: it is applied in this part's own export, before the common target and the
    // join, so one clip's crop or grade never reaches another's picture.
    const ExportInput& edit = job.inputs[i];
    one.crop_x = edit.crop_x;
    one.crop_y = edit.crop_y;
    one.crop_width = edit.crop_width;
    one.crop_height = edit.crop_height;
    one.grade_brightness = edit.grade_brightness;
    one.grade_contrast = edit.grade_contrast;
    one.grade_saturation = edit.grade_saturation;
    one.speed = edit.speed;
    one.volumes = edit.volumes;
    const auto report = [&](double share) {
      if (progress) progress((static_cast<double>(i) + share) / static_cast<double>(count) * 0.9);
    };
    std::string part_encoder;
    bool ok = export_once(one, report, part_encoder, error, /*allow_hardware=*/true);
    if (!ok && g_hardware_decode_failed) {
      g_hardware_decode_failed = false;
      error.clear();
      part_encoder.clear();
      ok = export_once(one, report, part_encoder, error, /*allow_hardware=*/false);
    }
    if (!ok) return false;
    if (i == 0) {
      first_encoder = part_encoder;
      if (first_encoder.empty() || first_encoder == "copy") {
        error = "the join could not choose an encoder";
        return false;
      }
    }
    parts.push_back(one.output);
  }

  if (progress) progress(0.90);
  const std::string staged = job.output + ".part.mp4";
  DeleteFileA(staged.c_str());
  const double last_length = segments_length(plan.parts.back()) / sources.back().speed;
  if (!media_concat(parts, staged, 0.0, last_length, error)) {
    DeleteFileA(staged.c_str());
    return false;
  }
  if (progress) progress(0.97);
  const ClipCheck check = check_clip_file(staged);
  if (!check.ok) {
    DeleteFileA(staged.c_str());
    error = "the join did not verify: " + check.reason;
    return false;
  }
  const MediaInfo probe = media_probe(staged);
  if (!probe.ok || !stitch_duration_ok(plan.total_seconds, probe.duration_seconds)) {
    DeleteFileA(staged.c_str());
    error = "the join did not come out the right length";
    return false;
  }
  ExportJob final_job = job;
  final_job.replace_original = false;
  if (!put_export_in_place(final_job, staged, error)) {
    DeleteFileA(staged.c_str());
    return false;
  }
  encoder_name = first_encoder;
  if (progress) progress(1.0);
  return true;
}

}  // namespace

bool media_export(const ExportJob& job, const std::function<void(double)>& progress, std::string& encoder_name,
                  std::string& error) {
  if (job.inputs.size() > 1) {
    g_hardware_decode_failed = false;
    return export_stitch(job, progress, encoder_name, error);
  }
  if (job.inputs.size() == 1) {
    ExportJob one = job;
    const ExportInput& only = job.inputs[0];
    one.input = only.input;
    one.start = only.start;
    one.end = only.end;
    one.segments = only.segments;
    // One clip of a sequence carries its own edit too: the job's own fields are the global ones.
    one.crop_x = only.crop_x;
    one.crop_y = only.crop_y;
    one.crop_width = only.crop_width;
    one.crop_height = only.crop_height;
    one.grade_brightness = only.grade_brightness;
    one.grade_contrast = only.grade_contrast;
    one.grade_saturation = only.grade_saturation;
    one.speed = only.speed;
    one.volumes = only.volumes;
    one.inputs.clear();
    return media_export(one, progress, encoder_name, error);
  }
  g_hardware_decode_failed = false;
  if (export_once(job, progress, encoder_name, error, /*allow_hardware=*/true)) return true;
  // A decoder that opened but could not deliver a frame (an unusual chroma format, a driver reset) is
  // not worth failing a save over: the same save runs once more, in software.
  if (!g_hardware_decode_failed) return false;
  g_hardware_decode_failed = false;
  encoder_name.clear();
  error.clear();
  return export_once(job, progress, encoder_name, error, /*allow_hardware=*/false);
}

}  // namespace bx
