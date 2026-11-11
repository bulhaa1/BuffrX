#include "media.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "log_policy.h"
#include "packet_hash.h"
#include "screenshot.h"

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "../shared/files.h"
#include "../shared/log.h"

namespace bx {
namespace {

// FFmpeg is not thread-safe. The engine decodes on several threads at once - the library listing on
// a pipe reader, a clip's thumbnail on another, the trim worker on its own - and two decoders
// running at the same time crash the process, which is what "the engine stops responding and
// starts working again" turned out to be.
//
// One lock for all of it. The media work is the slow part anyway, and the lock is NOT the engine
// lock: the engine keeps ticking, the watchdog keeps watching and the hotkeys keep working while a
// trim holds it.
//
// The lock is not recursive, so nothing may call a public media function while holding it. A trim
// is a one-file join and wants the same copy as a concat, so the copy is a separate worker the two
// public entry points call - the first version took the lock in the trim and then called the
// concat, which took it again, and the engine hung on every clip it saved.
//
// The lock has a deadline. A decode that hangs - a damaged file, a file a writer still holds - would
// otherwise hold it for good, and every listing, thumbnail and save would queue behind it: the engine
// alive, its library answering nobody. Waiting callers give up with an answer instead. A save's trim
// waits much longer than a thumbnail, because the user was promised that clip.
std::timed_mutex& media_lock() {
  static std::timed_mutex lock;
  return lock;
}

constexpr std::chrono::seconds kInteractiveWait{8};  // a listing, a thumbnail, a preview
constexpr std::chrono::seconds kSaveWait{180};       // a clip's trim, a range export

const char* kMediaBusy = "the media work is busy; try again in a moment";

// Takes the media lock within `wait`, or reports that it could not.
class MediaLock {
public:
  explicit MediaLock(std::chrono::seconds wait) : owned_(media_lock().try_lock_for(wait)) {}
  ~MediaLock() {
    if (owned_) media_lock().unlock();
  }
  MediaLock(const MediaLock&) = delete;
  MediaLock& operator=(const MediaLock&) = delete;
  bool owned() const { return owned_; }

private:
  bool owned_;
};

// The copy itself, with no lock of its own: the two public entry points hold the media lock while
// they call it. A trim is a one-file join, so it needs the same worker.
bool concat_files(const std::vector<std::string>& files, const std::string& output_path, double start_in_first,
                  double end_in_last, std::string& error, double* first_keyframe_seconds = nullptr,
                  size_t* dropped_head = nullptr);

std::string av_error(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

const char* codec_name(AVCodecID id) {
  const AVCodecDescriptor* descriptor = avcodec_descriptor_get(id);
  return descriptor && descriptor->name ? descriptor->name : "unknown";
}

struct Input {
  AVFormatContext* format = nullptr;

  ~Input() {
    if (format) avformat_close_input(&format);
  }
};

// FFmpeg's own log callback writes to stderr, so a demuxer that complains about a file complains once
// per open: `[mov,mp4,m4a,3gp,3g2,mj2] UDTA parsing failed retrying raw` printed for every clip on
// every listing, and with no callback installed nothing could route it.
// This one routes what went wrong into the engine log with a stable prefix and says a warning once, so
// the noise of a listing cannot flood the log while a real failure still names itself.
//
// Decoding is not touched: the av_log level is left exactly as the runtime set it, and this only
// chooses where a message that was already produced goes.
std::once_flag g_ffmpeg_log_once;
std::mutex g_ffmpeg_log_mutex;
std::set<std::string> g_ffmpeg_log_said;

void ffmpeg_log_callback(void* avcl, int level, const char* format, va_list args) {
  (void)avcl;
  // Only what went wrong: INFO and below is per-frame chatter (a frame rate note per stream, a
  // "guessed channel layout" per open) and never explains anything the log needs.
  if (level > AV_LOG_WARNING || level == AV_LOG_QUIET) return;
  char body[1024];
  std::vsnprintf(body, sizeof(body), format, args);
  std::string text = body;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  if (text.empty()) return;
  // Errors are never dropped or folded: they are the lines that explain a file that will not read.
  // The one exception is the pair libaom prints once per packet while avformat_find_stream_info
  // probes an AV1 stream whose level it refuses (563 lines in one second for one thumbnail): said
  // once, then folded like a warning.
  const bool probe_chatter = text.find("Failed to decode frame: Corrupt frame detected") != std::string::npos ||
                             text.find("Additional information: No sequence header") != std::string::npos;
  if (level <= AV_LOG_ERROR && !probe_chatter) {
    BX_LOG_ERROR("ffmpeg: %s", text.c_str());
    return;
  }
  {
    // A bounded set of what has already been said. A demuxer repeats the same complaint for every
    // file it opens, and 90 clips meant 90 identical lines per listing. It is cleared rather than
    // grown so a long run cannot leak; the worst case is one repeated line per 128 messages.
    std::lock_guard<std::mutex> lock(g_ffmpeg_log_mutex);
    if (g_ffmpeg_log_said.size() >= 128) g_ffmpeg_log_said.clear();
    if (!g_ffmpeg_log_said.insert(text).second) return;
  }
  // A demuxer retrying a tag it could not parse is not a failure of the clip: it is debug, once.
  if (text.find("UDTA parsing failed") != std::string::npos) {
    BX_LOG_DEBUG("ffmpeg: %s", text.c_str());
  } else if (ffmpeg_warning_is_routine(text)) {
    BX_LOG_INFO("ffmpeg: %s", text.c_str());
  } else {
    BX_LOG_WARN("ffmpeg: %s", text.c_str());
  }
}

bool open_input(const std::string& path, Input& input, std::string& error) {
  // Every open goes through here, so this is also where the engine's log callback is guaranteed to be
  // in place, including in the short-lived --media-* processes.
  media_install_log_callback();
  const int result = avformat_open_input(&input.format, path.c_str(), nullptr, nullptr);
  if (result < 0) {
    error = "the file could not be opened: " + av_error(result);
    return false;
  }
  const int streams = avformat_find_stream_info(input.format, nullptr);
  if (streams < 0) {
    error = "the file has no readable streams: " + av_error(streams);
    return false;
  }
  return true;
}

// What a copy must know about one file before it opens the output (media_math.h's StreamSignature): the
// first video stream's codec, geometry and rate, and the first audio stream's rate and layout. False when
// the file has no video: a chunk of the ring always does.
bool read_signature(AVFormatContext* format, StreamSignature& out) {
  const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) return false;
  const AVStream* stream = format->streams[video];
  out.codec_id = static_cast<int>(stream->codecpar->codec_id);
  out.width = stream->codecpar->width;
  out.height = stream->codecpar->height;
  out.pix_fmt = stream->codecpar->format;
  const AVRational rate = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
                              ? stream->avg_frame_rate
                              : stream->r_frame_rate;
  out.frame_rate = Rational{rate.num, rate.den};
  out.time_base = Rational{stream->time_base.num, stream->time_base.den};
  const int audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (audio >= 0) {
    const AVCodecParameters* parameters = format->streams[audio]->codecpar;
    out.sample_rate = parameters->sample_rate;
    out.channel_layout = static_cast<uint64_t>(parameters->ch_layout.u.mask);
  }
  return true;
}

}  // namespace

void media_install_log_callback() {
  std::call_once(g_ffmpeg_log_once, [] { av_log_set_callback(ffmpeg_log_callback); });
}

bool media_keyframes(const std::string& path, std::vector<double>& times, std::string& error) {
  times.clear();
  Input input;
  if (!open_input(path, input, error)) return false;
  AVFormatContext* format = input.format;
  const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) {
    error = "the file has no video stream";
    return false;
  }
  const AVRational base = format->streams[video]->time_base;
  AVPacket* packet = av_packet_alloc();
  if (!packet) {
    error = "out of memory";
    return false;
  }
  while (av_read_frame(format, packet) >= 0) {
    if (packet->stream_index == video && (packet->flags & AV_PKT_FLAG_KEY)) {
      const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
      if (stamp != AV_NOPTS_VALUE) {
        times.push_back(stamp * av_q2d(base));
        if (times.size() >= 5000) {
          av_packet_unref(packet);
          break;
        }
      }
    }
    av_packet_unref(packet);
  }
  av_packet_free(&packet);
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());
  return true;
}

MediaInfo media_probe(const std::string& path) {
  MediaInfo info;
  const MediaLock lock(kInteractiveWait);
  if (!lock.owned()) {
    info.reason = kMediaBusy;
    return info;
  }
  Input input;
  if (!open_input(path, input, info.reason)) return info;

  if (input.format->duration != AV_NOPTS_VALUE) {
    info.duration_seconds = static_cast<double>(input.format->duration) / AV_TIME_BASE;
  }
  const int video = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video >= 0) {
    const AVCodecParameters* parameters = input.format->streams[video]->codecpar;
    info.width = parameters->width;
    info.height = parameters->height;
    info.video_codec = codec_name(parameters->codec_id);
    info.video_kbps = static_cast<int>(parameters->bit_rate / 1000);
  }
  int64_t audio_bits = 0;
  const int audio = av_find_best_stream(input.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (audio >= 0) {
    const AVCodecParameters* parameters = input.format->streams[audio]->codecpar;
    info.audio_codec = codec_name(parameters->codec_id);
    audio_bits += parameters->bit_rate > 0 ? parameters->bit_rate : 160000;
  }
  if (video < 0 && audio < 0) {
    info.reason = "the file has neither a video nor an audio stream";
    return info;
  }
  info.ok = true;
  // Matroska rarely records a per-stream rate; the container's average less the sound is close.
  if (info.video_kbps <= 0 && input.format->bit_rate > 0) {
    info.video_kbps = static_cast<int>(std::max<int64_t>(0, input.format->bit_rate - audio_bits) / 1000);
  }
  return info;
}

bool video_packets_need_fixing(const AVCodecParameters* parameters) {
  return parameters && parameters->codec_id == AV_CODEC_ID_AV1;
}

int send_video_packet(AVCodecContext* decoder, const AVPacket* packet, bool fix) {
  if (!fix || !packet || !packet->data) return avcodec_send_packet(decoder, packet);
  const int level = av1_level(packet->data, static_cast<size_t>(packet->size));
  if (level < 20 || level > 23) return avcodec_send_packet(decoder, packet);
  // Only packets that carry a sequence header (keyframes) are copied; the demuxer's own stays as read.
  AVPacket* copy = av_packet_clone(packet);
  if (!copy) return AVERROR(ENOMEM);
  int result = av_packet_make_writable(copy);
  if (result >= 0) {
    av1_unconstrain_level(copy->data, static_cast<size_t>(copy->size));
    result = avcodec_send_packet(decoder, copy);
  }
  av_packet_free(&copy);
  return result;
}

bool learn_video_format(AVFormatContext* format, int stream_index) {
  AVStream* stream = format->streams[stream_index];
  AVCodecParameters* parameters = stream->codecpar;
  if (parameters->format != AV_PIX_FMT_NONE) return true;
  const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
  AVCodecContext* decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool learned = false;
  if (decoder && packet && frame && avcodec_parameters_to_context(decoder, parameters) >= 0) {
    decoder->thread_count = 1;
    if (avcodec_open2(decoder, codec, nullptr) >= 0) {
      av_seek_frame(format, stream_index, 0, AVSEEK_FLAG_BACKWARD);
      const bool fix = video_packets_need_fixing(parameters);
      // A keyframe and a few frames are all a decoder needs; a stream that gives nothing in this many
      // packets will not give a frame later either.
      for (int read = 0; !learned && read < 512 && av_read_frame(format, packet) >= 0; ++read) {
        if (packet->stream_index == stream_index && send_video_packet(decoder, packet, fix) >= 0 &&
            avcodec_receive_frame(decoder, frame) >= 0) {
          parameters->format = frame->format;
          if (parameters->width <= 0) parameters->width = frame->width;
          if (parameters->height <= 0) parameters->height = frame->height;
          learned = true;
        }
        av_packet_unref(packet);
      }
      av_seek_frame(format, stream_index, 0, AVSEEK_FLAG_BACKWARD);
    }
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoder);
  return learned;
}

namespace {

// One frame of a file, decoded at `at_seconds` and scaled to fit a `box`-pixel square, as BGRA. The
// library's thumbnails and the interface's previews both come through here: one FFmpeg decode path.
bool decode_frame_bgra(const std::string& path, double at_seconds, int box, std::vector<uint8_t>& pixels,
                       int& out_width, int& out_height, std::string& error) {
  Input input;
  if (!open_input(path, input, error)) return false;

  const int video = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) {
    error = "the file has no video stream";
    return false;
  }
  AVStream* stream = input.format->streams[video];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    error = std::string("no decoder for ") + codec_name(stream->codecpar->codec_id);
    return false;
  }
  AVCodecContext* decoder = avcodec_alloc_context3(codec);
  if (!decoder) {
    error = "the decoder could not be allocated";
    return false;
  }
  // One thread. A thumbnail is a single frame: FFmpeg's default is one thread per core, so every
  // decode of every clip in a listing spawned two dozen threads, and the engine reached a hundred
  // and fifty before a thread could not be created any more - which is a C++ exception, and an
  // exception nobody catches at that point ends the process. The frame is wanted, not the speed.
  decoder->thread_count = 1;
  bool ok = false;
  AVFrame* frame = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  SwsContext* scaler = nullptr;
  do {
    if (!frame || !packet) {
      error = "out of memory";
      break;
    }
    if (avcodec_parameters_to_context(decoder, stream->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0) {
      error = "the decoder could not be opened";
      break;
    }
    // Seek to the keyframe at or before the requested moment; the first frame that decodes after that
    // is the one. A clip shorter than the request simply gives its last frames.
    const double duration = input.format->duration != AV_NOPTS_VALUE
                                ? static_cast<double>(input.format->duration) / AV_TIME_BASE
                                : 0.0;
    const double moment = duration > 0.0 ? std::min(at_seconds, duration * 0.9) : at_seconds;
    if (moment > 0.0) {
      const int64_t timestamp = static_cast<int64_t>(moment / av_q2d(stream->time_base));
      av_seek_frame(input.format, video, timestamp, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(decoder);
    }

    bool decoded = false;
    const bool fix = video_packets_need_fixing(stream->codecpar);
    while (!decoded && av_read_frame(input.format, packet) >= 0) {
      if (packet->stream_index == video && send_video_packet(decoder, packet, fix) >= 0) {
        decoded = avcodec_receive_frame(decoder, frame) >= 0;
      }
      av_packet_unref(packet);
    }
    // A single still image (PNG, JPEG) may only come out once the decoder is drained.
    if (!decoded && avcodec_send_packet(decoder, nullptr) >= 0) decoded = avcodec_receive_frame(decoder, frame) >= 0;
    if (!decoded) {
      error = "no frame could be decoded";
      break;
    }

    thumbnail_size(frame->width, frame->height, box, out_width, out_height);
    if (out_width <= 0 || out_height <= 0) {
      error = "the frame has no usable size";
      break;
    }
    scaler = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), out_width,
                            out_height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler) {
      error = "the frame could not be scaled";
      break;
    }
    pixels.assign(static_cast<size_t>(out_width) * out_height * 4, 0);
    uint8_t* planes[4] = {pixels.data(), nullptr, nullptr, nullptr};
    int strides[4] = {out_width * 4, 0, 0, 0};
    sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
    ok = true;
  } while (false);

  if (scaler) sws_freeContext(scaler);
  if (packet) av_packet_free(&packet);
  if (frame) av_frame_free(&frame);
  avcodec_free_context(&decoder);
  return ok;
}

}  // namespace

bool media_thumbnail(const std::string& path, const std::string& output_path, int width, double at_seconds,
                     std::string& error) {
  const MediaLock lock(kInteractiveWait);
  if (!lock.owned()) {
    error = kMediaBusy;
    return false;
  }
  std::vector<uint8_t> pixels;
  int out_width = 0, out_height = 0;
  if (!decode_frame_bgra(path, at_seconds, width, pixels, out_width, out_height, error)) return false;
  // WIC writes the JPEG: the same encoder the screenshot path uses, so there is one image writer.
  return encode_image_wic(pixels.data(), out_width, out_height, output_path, true, error);
}

bool media_loudness(const std::string& path, std::vector<float>& rms_per_second, std::string& error) {
  rms_per_second.clear();
  const MediaLock lock(kInteractiveWait);
  if (!lock.owned()) {
    error = kMediaBusy;
    return false;
  }
  Input input;
  if (!open_input(path, input, error)) return false;
  constexpr size_t kMaxSeconds = 600;
  struct Track {
    AVCodecContext* decoder = nullptr;
    std::vector<double> sum;           // sum of squares per second
    std::vector<uint32_t> count;       // samples per second
    double first = -1.0;               // the first sample's time, so the file's own offset is not a silence
  };
  std::vector<Track> tracks;
  std::vector<int> stream_track(input.format->nb_streams, -1);
  for (unsigned i = 0; i < input.format->nb_streams; ++i) {
    AVStream* stream = input.format->streams[i];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
      stream->discard = AVDISCARD_ALL;
      continue;
    }
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!decoder) continue;
    decoder->thread_count = 1;
    if (avcodec_parameters_to_context(decoder, stream->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0) {
      avcodec_free_context(&decoder);
      continue;
    }
    stream_track[i] = static_cast<int>(tracks.size());
    Track track;
    track.decoder = decoder;
    tracks.push_back(std::move(track));
  }
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  if (!packet || !frame) {
    // Nothing was read yet: fail the read rather than report a file with no sound.
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    for (Track& track : tracks) avcodec_free_context(&track.decoder);
    error = "out of memory";
    return false;
  }
  auto take = [&](Track& track, AVStream* stream) {
    while (avcodec_receive_frame(track.decoder, frame) >= 0) {
      const int64_t stamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
      const double start = stamp != AV_NOPTS_VALUE ? stamp * av_q2d(stream->time_base) : 0.0;
      if (track.first < 0.0) track.first = start;
      const int channels = std::max(1, frame->ch_layout.nb_channels);
      const AVSampleFormat format = static_cast<AVSampleFormat>(frame->format);
      const bool planar = av_sample_fmt_is_planar(format) != 0;
      const AVSampleFormat packed = av_get_packed_sample_fmt(format);
      for (int s = 0; s < frame->nb_samples; ++s) {
        const double t = start - track.first + static_cast<double>(s) / std::max(1, frame->sample_rate);
        const size_t second = t < 0.0 ? 0 : static_cast<size_t>(t);
        if (second >= kMaxSeconds) break;
        if (track.sum.size() <= second) {
          track.sum.resize(second + 1, 0.0);
          track.count.resize(second + 1, 0);
        }
        for (int c = 0; c < channels; ++c) {
          const int plane = planar ? c : 0;
          const int at = planar ? s : s * channels + c;
          double value = 0.0;
          switch (packed) {
            case AV_SAMPLE_FMT_FLT: value = reinterpret_cast<const float*>(frame->extended_data[plane])[at]; break;
            case AV_SAMPLE_FMT_S16: value = reinterpret_cast<const int16_t*>(frame->extended_data[plane])[at] / 32768.0; break;
            case AV_SAMPLE_FMT_S32: value = reinterpret_cast<const int32_t*>(frame->extended_data[plane])[at] / 2147483648.0; break;
            case AV_SAMPLE_FMT_DBL: value = reinterpret_cast<const double*>(frame->extended_data[plane])[at]; break;
            default: break;
          }
          track.sum[second] += value * value;
          track.count[second] += 1;
        }
      }
      av_frame_unref(frame);
    }
  };
  if (packet && frame) {
    while (av_read_frame(input.format, packet) >= 0) {
      const int index = stream_track[static_cast<size_t>(packet->stream_index)];
      if (index >= 0 && avcodec_send_packet(tracks[static_cast<size_t>(index)].decoder, packet) >= 0) {
        take(tracks[static_cast<size_t>(index)], input.format->streams[packet->stream_index]);
      }
      av_packet_unref(packet);
    }
  }
  for (Track& track : tracks) {
    for (size_t second = 0; second < track.sum.size(); ++second) {
      if (rms_per_second.size() <= second) rms_per_second.resize(second + 1, 0.0f);
      if (track.count[second] == 0) continue;
      const float rms = static_cast<float>(std::sqrt(track.sum[second] / track.count[second]));
      rms_per_second[second] = std::max(rms_per_second[second], rms);
    }
    avcodec_free_context(&track.decoder);
  }
  if (frame) av_frame_free(&frame);
  if (packet) av_packet_free(&packet);
  return true;
}

bool media_decode_image(const std::string& path, int box, std::vector<uint8_t>& bgra, int& width, int& height,
                        std::string& error) {
  const MediaLock lock(kInteractiveWait);
  if (!lock.owned()) {
    error = kMediaBusy;
    return false;
  }
  return decode_frame_bgra(path,0.0, box, bgra, width, height, error);
}

bool media_trim(const std::string& path, const std::string& output_path, double start_seconds, double length_seconds,
                std::string& error, double* first_keyframe_seconds) {
  const MediaLock lock(kSaveWait);
  if (!lock.owned()) {
    error = kMediaBusy;
    return false;
  }
  Input input;
  if (!open_input(path, input, error)) return false;

  const double duration = input.format->duration != AV_NOPTS_VALUE
                              ? static_cast<double>(input.format->duration) / AV_TIME_BASE
                              : 0.0;
  double start = 0.0;
  double length = 0.0;
  if (!trim_range(duration, start_seconds, length_seconds, start, length)) {
    error = "there is nothing to copy in that range";
    return false;
  }
  avformat_close_input(&input.format);  // the copy reopens the file
  // A trim is a one-file join: the same stream copy that starts on the keyframe at or before `start`,
  // shifts every timestamp so the clip begins at 0, maps the streams it keeps, and drops audio from
  // before that keyframe. The old copy kept the source's timestamps (a clip that "started" at 41 s).
  //
  // The copy is the unlocked worker, not media_concat: this function already holds the media lock,
  // and the lock is not recursive.
  return concat_files({path}, output_path, start, start + length, error, first_keyframe_seconds);
}

bool media_concat(const std::vector<std::string>& files, const std::string& output_path, double start_in_first,
                  double end_in_last, std::string& error, double* first_keyframe_seconds, size_t* dropped_head) {
  const MediaLock lock(kSaveWait);
  if (!lock.owned()) {
    error = kMediaBusy;
    return false;
  }
  return concat_files(files, output_path, start_in_first, end_in_last, error, first_keyframe_seconds, dropped_head);
}

namespace {

// The copy itself, with no lock of its own: the two public entry points above hold the media lock
// while they call it.
bool concat_files(const std::vector<std::string>& requested_files, const std::string& output_path, double start_in_first,
                  double end_in_last, std::string& error, double* first_keyframe_seconds, size_t* dropped_head) {
  if (first_keyframe_seconds) *first_keyframe_seconds = -1.0;
  if (dropped_head) *dropped_head = 0;
  if (requested_files.empty()) {
    error = "there are no chunks to join";
    return false;
  }
  // Every file must carry the same stream, and it is known before the output exists: a join across a
  // settings change used to copy the second chunk's packets into the first chunk's codec and write a clip
  // that plays broken, quietly. Codec, size, pixel format, frame rate, sample rate, channel layout and time
  // bases, not just the stream types the mapping below checks.
  std::vector<StreamSignature> signatures;
  for (const std::string& file : requested_files) {
    Input probe;
    if (!open_input(file, probe, error)) return false;
    StreamSignature signature;
    if (!read_signature(probe.format, signature)) {
      error = "a chunk has no video: " + file;
      return false;
    }
    signatures.push_back(signature);
  }
  // The run that shares the last chunk's stream: all of them when nothing changed. A join across a change is refused unless
  // the caller (a Timeline save) asked for the part that holds the end of its range.
  const size_t from = trailing_compatible_run(signatures);
  if (from > 0 && !dropped_head) {
    // The person sees one sentence; whatever asks "why did my clip not save" needs the number that
    // differed, and probing it again by hand is the whole afternoon.
    const StreamSignature& signature = signatures[from - 1];
    const StreamSignature& expected = signatures.back();
    BX_LOG_WARN("chunk layout mismatch in %s: codec %d/%d %dx%d/%dx%d pix %d/%d fps %d/%d/%d/%d "
                "tb %d/%d/%d/%d audio %d/%d layout %llu/%llu",
                requested_files[from - 1].c_str(), signature.codec_id, expected.codec_id, signature.width, signature.height,
                expected.width, expected.height, signature.pix_fmt, expected.pix_fmt,
                signature.frame_rate.num, signature.frame_rate.den, expected.frame_rate.num,
                expected.frame_rate.den, signature.time_base.num, signature.time_base.den,
                expected.time_base.num, expected.time_base.den, signature.sample_rate, expected.sample_rate,
                static_cast<unsigned long long>(signature.channel_layout),
                static_cast<unsigned long long>(expected.channel_layout));
    error = "the chunks do not share one stream layout";
    return false;
  }
  if (from > 0) {
    BX_LOG_INFO("a join crosses a change of stream layout: copying the last %zu of %zu chunks", requested_files.size() - from,
                requested_files.size());
    *dropped_head = from;
    start_in_first = 0.0;  // the run begins at a chunk's own start
  }
  const std::vector<std::string> files(requested_files.begin() + static_cast<std::ptrdiff_t>(from), requested_files.end());
  AVFormatContext* output = nullptr;
  if (avformat_alloc_output_context2(&output, nullptr, nullptr, output_path.c_str()) < 0 || !output) {
    error = "the output container is not supported";
    return false;
  }
  AVPacket* packet = av_packet_alloc();
  std::vector<int64_t> last_dts;  // per output stream, in the output time base
  double cursor = 0.0;            // seconds of footage already written
  bool header_written = false;

  for (size_t file_index = 0; file_index < files.size() && error.empty(); ++file_index) {
    if (!packet) {
      error = "out of memory";
      break;
    }
    Input input;
    if (!open_input(files[file_index], input, error)) break;

    // Video and audio only, mapped in order; the first file defines the layout the others must match.
    std::vector<int> map(input.format->nb_streams, -1);
    unsigned mapped = 0;
    for (unsigned index = 0; index < input.format->nb_streams && error.empty(); ++index) {
      AVStream* source = input.format->streams[index];
      const AVMediaType type = source->codecpar->codec_type;
      if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;
      if (file_index == 0) {
        AVStream* target = avformat_new_stream(output, nullptr);
        if (!target || avcodec_parameters_copy(target->codecpar, source->codecpar) < 0) {
          error = "a stream could not be copied";
          break;
        }
        target->codecpar->codec_tag = 0;
        target->time_base = source->time_base;
      } else if (mapped >= output->nb_streams || output->streams[mapped]->codecpar->codec_type != type) {
        error = "the chunks do not share one stream layout";
        break;
      }
      map[index] = static_cast<int>(mapped++);
    }
    if (!error.empty()) break;
    if (mapped == 0 || mapped != output->nb_streams) {
      error = "the chunks do not share one stream layout";
      break;
    }

    if (!header_written) {
      if (avio_open(&output->pb, output_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        error = "the output file could not be created";
        break;
      }
      if (avformat_write_header(output, nullptr) < 0) {
        error = "the output header could not be written";
        break;
      }
      header_written = true;
      last_dts.assign(output->nb_streams, INT64_MIN);
    }

    const bool last_file = file_index + 1 == files.size();
    const double cut = file_index == 0 ? std::max(start_in_first, 0.0) : 0.0;
    if (cut > 0.0) av_seek_frame(input.format, -1, static_cast<int64_t>(cut * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    const int video = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    // `origin` is the file time that lands at `cursor`: the first video keyframe read, so the joined
    // file starts, and continues at every seam, on a frame that decodes by itself.
    bool have_origin = false;
    double origin = 0.0;
    double file_end = 0.0;
    while (av_read_frame(input.format, packet) >= 0) {
      const int target_index = map[packet->stream_index];
      AVStream* source = input.format->streams[packet->stream_index];
      const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
      if (target_index < 0 || stamp == AV_NOPTS_VALUE) {
        av_packet_unref(packet);
        continue;
      }
      const double at = stamp * av_q2d(source->time_base);
      const bool is_video = packet->stream_index == video;
      // A truncated tail can hand over a packet with no bytes; writing it makes the MP4 muxer's mdat
      // size go negative and the file unfinalized. A negative duration poisons the cursor the same way.
      if (packet->flags & AV_PKT_FLAG_CORRUPT) {
        av_packet_unref(packet);
        continue;
      }
      if (packet->size <= 0 || packet->size > 64 * 1024 * 1024) {
        av_packet_unref(packet);
        continue;
      }
      const double packet_seconds = packet->duration > 0 ? packet->duration * av_q2d(source->time_base) : 0.0;
      if (last_file && at >= end_in_last) {
        av_packet_unref(packet);
        if (is_video || video < 0) break;
        continue;
      }
      if (!have_origin) {
        if (video >= 0 && (!is_video || !(packet->flags & AV_PKT_FLAG_KEY))) {
          av_packet_unref(packet);
          continue;
        }
        origin = at;
        have_origin = true;
        // Where the copy itself begins: the caller hides the distance from its own cut with an edit list.
        if (file_index == 0 && first_keyframe_seconds) *first_keyframe_seconds = origin;
      }
      if (at < origin) {  // audio from before the keyframe the copy starts on
        av_packet_unref(packet);
        continue;
      }
      file_end = std::max(file_end, at - origin + packet_seconds);
      const int64_t shift = static_cast<int64_t>(std::llround((cursor - origin) / av_q2d(source->time_base)));
      if (packet->pts != AV_NOPTS_VALUE) packet->pts += shift;
      if (packet->dts != AV_NOPTS_VALUE) packet->dts += shift;
      AVStream* target = output->streams[target_index];
      packet->stream_index = target_index;
      av_packet_rescale_ts(packet, source->time_base, target->time_base);
      // Matroska stores no decode times, so a chunk's first keyframe comes back without one; and rounding
      // at a seam must never make a stream step backwards. Either way the packet gets a decode time just
      // after the stream's previous one, and never after its own presentation time.
      int64_t& previous = last_dts[static_cast<size_t>(target_index)];
      if (packet->dts == AV_NOPTS_VALUE) {
        packet->dts = previous == INT64_MIN ? packet->pts : std::min(previous + 1, packet->pts);
      }
      if (previous != INT64_MIN && packet->dts <= previous) packet->dts = previous + 1;
      if (packet->pts != AV_NOPTS_VALUE && packet->pts < packet->dts) packet->pts = packet->dts;
      previous = packet->dts;
      if (packet->duration < 0) packet->duration = 0;
      packet->pos = -1;
      const int written = av_interleaved_write_frame(output, packet);
      av_packet_unref(packet);
      if (written < 0) {
        error = "the copy stopped early: " + av_error(written);
        break;
      }
    }
    cursor += file_end;
  }

  bool ok = false;
  if (error.empty() && header_written) {
    if (av_write_trailer(output) < 0) {
      error = "the output trailer could not be written";
    } else {
      ok = true;
    }
  }
  if (packet) av_packet_free(&packet);
  if (output->pb) avio_closep(&output->pb);
  avformat_free_context(output);
  if (!ok) std::remove(output_path.c_str());
  return ok;
}

}  // namespace

// Every packet of a file as packet_hash.h sees it: the picture hash of each video packet, the whole-payload hash of each audio packet, and the stream
// signature a join checks. A probe of the Timeline dedupe spike (docs/review-timeline-mux-2026-11-02.md), run in a process of its own.
PacketReport media_packet_report(const std::string& path) {
  PacketReport report;
  Input input;
  if (!open_input(path, input, report.reason)) return report;
  AVFormatContext* format = input.format;
  std::vector<int> ordinal(format->nb_streams, -1);  // audio ordinal; the first video stream is 'v'
  std::vector<int> length_size(format->nb_streams, 0);
  std::vector<HashCodec> codec(format->nb_streams, HashCodec::other);
  int video_stream = -1, audio_streams = 0;
  for (unsigned i = 0; i < format->nb_streams; ++i) {
    const AVCodecParameters* parameters = format->streams[i]->codecpar;
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0) {
      video_stream = static_cast<int>(i);
      const uint8_t* extra = parameters->extradata;
      const int extra_size = parameters->extradata_size;
      if (parameters->codec_id == AV_CODEC_ID_H264) {
        codec[i] = HashCodec::h264;
        length_size[i] = (extra && extra_size >= 5 && extra[0] == 1) ? (extra[4] & 3) + 1 : 0;
      } else if (parameters->codec_id == AV_CODEC_ID_HEVC) {
        codec[i] = HashCodec::hevc;
        length_size[i] = (extra && extra_size >= 23 && extra[0] == 1) ? (extra[21] & 3) + 1 : 0;
      } else if (parameters->codec_id == AV_CODEC_ID_AV1) {
        codec[i] = HashCodec::av1;
      }
    } else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
      ordinal[i] = audio_streams++;
    }
  }
  if (video_stream < 0) {
    report.reason = "the file has no video stream";
    return report;
  }
  StreamSignature signature;
  read_signature(format, signature);
  char line[320];
  std::snprintf(line, sizeof(line), "codec=%d width=%d height=%d pix_fmt=%d fps=%d/%d time_base=%d/%d sample_rate=%d channel_layout=%llu audio_streams=%d length_size=%d",
                signature.codec_id, signature.width, signature.height, signature.pix_fmt, signature.frame_rate.num, signature.frame_rate.den, signature.time_base.num,
                signature.time_base.den, signature.sample_rate, static_cast<unsigned long long>(signature.channel_layout), audio_streams, length_size[video_stream]);
  report.signature = line;
  AVPacket* packet = av_packet_alloc();
  if (!packet) {
    report.reason = "out of memory";
    return report;
  }
  while (av_read_frame(format, packet) >= 0) {
    PacketLine entry;
    const int stream = packet->stream_index;
    if (stream == video_stream) {
      const VclDigest digest = vcl_digest(codec[stream], packet->data, static_cast<size_t>(packet->size), length_size[stream]);
      entry.kind = 'v';
      entry.key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
      entry.size = static_cast<uint32_t>(packet->size);
      entry.vcl_bytes = digest.ok ? digest.bytes : 0;
      entry.hash = digest.ok ? digest.hash : 0;
      report.lines.push_back(entry);
    } else if (stream >= 0 && stream < static_cast<int>(ordinal.size()) && ordinal[stream] >= 0) {
      entry.kind = 'a';
      entry.stream = ordinal[stream];
      entry.size = static_cast<uint32_t>(packet->size);
      entry.hash = payload_hash(packet->data, static_cast<size_t>(packet->size));
      report.lines.push_back(entry);
    }
    av_packet_unref(packet);
  }
  av_packet_free(&packet);
  report.ok = true;
  return report;
}

ChunkReport media_chunk_probe(const std::string& path) {
  ChunkReport report;
  const MediaLock lock(kInteractiveWait);
  if (!lock.owned()) {
    report.busy = true;
    report.reason = kMediaBusy;
    return report;
  }
  Input input;
  if (!open_input(path, input, report.reason)) return report;
  AVFormatContext* format = input.format;
  const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video < 0) {
    report.reason = "the file has no video stream";
    return report;
  }
  const AVCodecParameters* parameters = format->streams[video]->codecpar;
  HashCodec codec = HashCodec::other;
  int length_size = 0;
  const uint8_t* extra = parameters->extradata;
  const int extra_size = parameters->extradata_size;
  if (parameters->codec_id == AV_CODEC_ID_H264) {
    codec = HashCodec::h264;
    length_size = (extra && extra_size >= 5 && extra[0] == 1) ? (extra[4] & 3) + 1 : 0;
  } else if (parameters->codec_id == AV_CODEC_ID_HEVC) {
    codec = HashCodec::hevc;
    length_size = (extra && extra_size >= 23 && extra[0] == 1) ? (extra[21] & 3) + 1 : 0;
  } else if (parameters->codec_id == AV_CODEC_ID_AV1) {
    codec = HashCodec::av1;
  }
  if (codec == HashCodec::other) {
    report.reason = "the codec is not one the ring can match";
    return report;
  }
  const AVRational base = format->streams[video]->time_base;
  AVPacket* packet = av_packet_alloc();
  if (!packet) {
    report.reason = "out of memory";
    return report;
  }
  int64_t first_dts = 0;
  double last_end = 0;
  while (av_read_frame(format, packet) >= 0) {
    if (packet->stream_index == video) {
      const int64_t dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
      if (report.video_count == 0) first_dts = dts;
      if (packet->flags & AV_PKT_FLAG_KEY) {
        const VclDigest digest = vcl_digest(codec, packet->data, static_cast<size_t>(packet->size), length_size);
        ChunkKeyFact key;
        key.index = report.video_count;
        key.dts_us = static_cast<int64_t>(std::llround((dts - first_dts) * av_q2d(base) * 1e6));
        const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : dts;
        key.file_seconds = stamp * av_q2d(base);
        key.hash = digest.ok ? digest.hash : 0;
        key.vcl_bytes = digest.ok ? digest.bytes : 0;
        report.keys.push_back(key);
      }
      const int64_t stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : dts;
      last_end = std::max(last_end, (stamp + packet->duration) * av_q2d(base));
      ++report.video_count;
    }
    av_packet_unref(packet);
  }
  av_packet_free(&packet);
  report.duration_seconds = last_end;
  report.ok = report.video_count > 0;
  if (!report.ok) report.reason = "the file holds no video packet";
  return report;
}

namespace {

// The spec and its result sit beside a clip, so they carry whatever name the user gave that folder.
std::string read_text_file(const std::string& path) {
  std::string text;
  if (FILE* file = _wfopen(wide_path(path).c_str(), L"rb")) {
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
  }
  if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // a UTF-8 BOM
  return text;
}

}  // namespace

int run_media_join(const std::string& spec_path) {
  Json result = Json::object();
  result.set("ok", Json(false));
  result.set("error", Json("the join spec could not be read"));
  result.set("first_keyframe", Json(-1.0));
  result.set("dropped_head", Json(static_cast<int64_t>(0)));

  // The engine reads this file, so it is written whatever the outcome, beside the video it describes: an
  // outcome the engine cannot read is a save that fails with no reason to show.
  const auto write_result = [](const std::string& output, const Json& body) {
    if (output.empty()) return;
    const std::string result_path = media_join_result_path(output);
    if (FILE* file = _wfopen(wide_path(result_path).c_str(), L"wb")) {
      const std::string dumped = body.dump() + "\n";
      std::fwrite(dumped.data(), 1, dumped.size(), file);
      std::fclose(file);
    } else {
      BX_LOG_WARN("the join result could not be written beside the clip: %s", result_path.c_str());
    }
  };

  Json spec;
  std::string error;
  const std::string text = read_text_file(spec_path);
  if (text.empty() || !Json::parse(text, spec, error)) {
    std::printf("%s\n", result.dump().c_str());
    return 3;
  }
  MediaJoinRequest request;
  if (!parse_media_join_request(spec, request, error)) {
    result.set("error", Json(error));
    // A spec that names its output can still carry the sentence home; one that cannot is all the parent has.
    write_result(spec["output"].as_string(), result);
    std::printf("%s\n", result.dump().c_str());
    return 3;
  }

  double first_keyframe = -1.0;
  size_t dropped_head = 0;
  const bool ok = media_concat(request.files, request.output, request.start_in_first_seconds,
                               request.end_in_last_seconds, error, &first_keyframe, &dropped_head);
  result.set("ok", Json(ok));
  result.set("error", Json(ok ? std::string() : error));
  result.set("first_keyframe", Json(first_keyframe));
  result.set("dropped_head", Json(static_cast<int64_t>(dropped_head)));

  // The engine reads this file, so it is written whatever the outcome, beside the video it describes. A
  // failed join leaves no video behind (concat_files removes its own output).
  write_result(request.output, result);
  std::printf("%s\n", result.dump().c_str());
  return ok ? 0 : 3;
}

}  // namespace bx
