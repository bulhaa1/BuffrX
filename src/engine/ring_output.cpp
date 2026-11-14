#include "ring_output.h"

#include <windows.h>

#include <obs.h>
#include <util/pipe.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../shared/log.h"
#include "library.h"
#include "media.h"
#include "packet_hash.h"
#include "platform.h"
#include "ring_disk.h"
#include "ring_publish.h"
#include "ring_save.h"
#include "ring_spill.h"
#include "ring_store.h"
#include "timeline_retention.h"

namespace bx {

namespace {

constexpr int kMaxTracks = 6;  // libobs's MAX_AUDIO_MIXES

// What obs-ffmpeg-mux.exe reads from its stdin ahead of every packet (plugins/obs-ffmpeg/ffmpeg-mux/ffmpeg-mux.h): the same layout,
// field for field, so the same muxer process writes the file and a clip is the one OBS's replay buffer would have written.
enum PacketType : int32_t { kPacketVideo = 0, kPacketAudio = 1 };
struct PacketInfo {
  int64_t pts;
  int64_t dts;
  uint32_t size;
  uint32_t index;
  PacketType type;
  bool keyframe;
};

// FFmpeg's enum values, spelled here so this file needs no FFmpeg header: they are what the muxer process takes on its command line.
enum : int { kPriBt709 = 1, kPriUnspec = 2, kPriSmpte170m = 6, kPriBt2020 = 9 };
enum : int { kTrcBt709 = 1, kTrcUnspec = 2, kTrcSmpte170m = 6, kTrcSrgb = 13, kTrcPq = 16, kTrcHlg = 18 };
enum : int { kSpcBt709 = 1, kSpcUnspec = 2, kSpcSmpte170m = 6, kSpcBt2020 = 9 };
enum : int { kRangeMpeg = 1, kRangeJpeg = 2 };
enum : int { kChromaUnspec = 0, kChromaLeft = 1, kChromaCenter = 2, kChromaTopLeft = 3 };

// libobs's encoder_packet as the ring's store holds it (ring_store.h): the encoder's buffer is shared, not copied.
struct ObsPacketOps {
  using Packet = encoder_packet;
  static Packet add_ref(const Packet& packet) {
    encoder_packet copy;
    obs_encoder_packet_ref(&copy, const_cast<encoder_packet*>(&packet));
    return copy;
  }
  static void release(Packet& packet) { obs_encoder_packet_release(&packet); }
  static PacketFields fields(const Packet& packet) {
    PacketFields fields;
    fields.pts = packet.pts;
    fields.dts = packet.dts;
    fields.dts_usec = packet.dts_usec;
    fields.timebase_num = packet.timebase_num;
    fields.timebase_den = packet.timebase_den;
    fields.track = static_cast<uint32_t>(packet.track_idx);
    fields.video = packet.type == OBS_ENCODER_VIDEO;
    fields.keyframe = packet.keyframe;
    fields.data = packet.data;
    fields.size = static_cast<uint32_t>(packet.size);
    return fields;
  }
};
using Store = ReplayStore<ObsPacketOps>;

struct Ring {
  obs_output_t* output = nullptr;
  Store store;
  std::atomic<int64_t> save_ts{0};
  // The length the save was asked for (0: the whole ring). Taken by the muxer's thread so the clip is written once, at length.
  std::atomic<int> save_seconds{0};
  std::atomic<bool> active{false};
  std::atomic<bool> capturing{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> muxing{false};
  int64_t stop_ts = 0;
  std::thread mux_thread;
  std::mutex path_mutex;
  std::mutex teardown_mutex;  // one teardown at a time: the save's thread and the packet thread can both reach it
  std::mutex save_mutex;      // one save start at a time: the packet thread and a "save now" ask can both reach it
  std::string path;  // the last save
  // The names this ring has handed out, lower case (Windows compares them that way), and when. A save publishes its
  // staged file by a rename that never replaces (ring_publish.h), so the name is the save's own from the first byte, and
  // once the save ends the host files the clip into its game's folder, which frees the root name while a trim can still
  // be reading the file it moved. A name therefore stays claimed for a while after it was used. Old claims are dropped,
  // so the set cannot grow for a session.
  std::map<std::string, int64_t> claimed_paths;
  // The last save's verdict, written before the `saved` signal: the signal says a save ended, not that it worked.
  std::mutex result_mutex;
  SaveOutcome last_result;
  std::atomic<uint64_t> save_seq{0};
  os_process_pipe_t* pipe = nullptr;
  std::atomic<int> last_skipped{0};  // packets the last save left out because the disk part could not be read

  // A probe of the Timeline dedupe spike (docs/review-timeline-mux-2026-11-02.md), off unless BUFFRX_RING_KEYLOG=<file> is set: one line a packet with the
  // hash packet_hash.h computes, so a Timeline chunk's packets can be matched to the ring's. Not used by the product.
  FILE* keylog = nullptr;
  HashCodec keylog_codec = HashCodec::other;

  // The disk tier (ring_spill.h). Asked for by the output's settings; made by the spill thread, so the buffer starts without waiting for the
  // reservation. `file` is made when the ring starts and opened by the spill thread; the three blocks are the writer's and the reader's.
  std::string spill_path;
  int64_t spill_bytes = 0;
  int64_t ram_limit = 0;
  std::unique_ptr<RingFile> file;
  uint8_t* blocks[3] = {nullptr, nullptr, nullptr};
  std::atomic<int> spill_state{0};  // 0 not (yet) on, 1 on, 2 asked for and unavailable
  std::mutex problem_mutex;
  std::string spill_problem;  // why, for the log and the notice
  std::thread spill_thread;
  std::mutex spill_mutex;
  std::condition_variable spill_cv;
  std::atomic<bool> spill_run{false};
};

void join_mux(Ring& ring) {
  if (ring.mux_thread.joinable()) ring.mux_thread.join();
}

// The spill thread: reserves the file, then writes what is due. Low priority in every sense (CPU, I/O, memory): a game's own loading comes
// first, and a write that waits only leaves the footage in RAM a little longer.
void spill_main(Ring* ring) {
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  std::string error;
  bool ready = ring->file != nullptr;
  for (uint8_t*& block : ring->blocks) {
    block = alloc_aligned_block();
    ready = ready && block != nullptr;
  }
  if (!ready) error = "the staging memory could not be had";
  if (ready && !ring->file->open(ring->spill_path, ring->spill_bytes, error)) ready = false;
  if (!ready) {
    {
      std::lock_guard<std::mutex> lock(ring->problem_mutex);
      ring->spill_problem = error;
    }
    ring->spill_state = 2;
    // A tier that never worked must not keep its reservation: the three 8 MB blocks (24 MiB) were
    // held for the whole buffer session, and a half-opened file held its handle.
    for (uint8_t*& block : ring->blocks) {
      free_aligned_block(block);
      block = nullptr;
    }
    ring->file.reset();
    BX_LOG_WARN("the replay ring has no disk part (%s); it holds the footage in memory only", error.c_str());
    return;
  }
  if (ring->spill_run) {
    Store::DiskSetup setup;
    setup.io = ring->file.get();
    setup.file_bytes = ring->file->size();
    setup.ram_limit_bytes = ring->ram_limit;
    setup.block = ring->blocks[0];
    setup.window_a = ring->blocks[1];
    setup.window_b = ring->blocks[2];
    ring->store.enable_disk(setup);
    ring->spill_state = 1;
    BX_LOG_INFO("the replay ring keeps %lld MB in memory and the rest in a %lld MB file: %s", static_cast<long long>(ring->ram_limit / (1024 * 1024)),
                static_cast<long long>(ring->file->size() / (1024 * 1024)), ring->spill_path.c_str());
  }
  std::unique_lock<std::mutex> lock(ring->spill_mutex);
  while (ring->spill_run) {
    lock.unlock();
    const bool worked = ring->store.spill_due() && ring->store.spill_step();
    lock.lock();
    if (!ring->spill_run) break;
    // Nothing due: sleep until the packet thread says so. Due but the disk has no room or refused: look again soon.
    if (!worked) ring->spill_cv.wait_for(lock, std::chrono::milliseconds(ring->store.spill_due() ? 100 : 500));
  }
}

void stop_spill(Ring& ring) {
  if (!ring.spill_thread.joinable()) return;
  ring.spill_run = false;
  if (ring.file) ring.file->cancel();  // a disk that stopped answering must not hold up a rebuild
  ring.spill_cv.notify_all();
  ring.spill_thread.join();
}

// The ring's contents and its disk part go; the file is removed by Windows when its handle closes. No save may be running. The save's
// thread and the packet thread can both reach this (the save finished as the ring was deactivated), so the second one finds nothing left
// to do rather than racing the first.
void teardown(Ring& ring) {
  std::lock_guard<std::mutex> lock(ring.teardown_mutex);
  if (ring.keylog) {
    std::fclose(ring.keylog);
    ring.keylog = nullptr;
  }
  stop_spill(ring);
  ring.store.reset();  // before the blocks and the file: the store's writer and reader point at them
  ring.file.reset();
  for (uint8_t*& block : ring.blocks) {
    free_aligned_block(block);
    block = nullptr;
  }
  ring.spill_state = 0;
  ring.save_ts = 0;
}

int chroma_location(video_format format, int spc) {
  switch (format) {
    case VIDEO_FORMAT_NV12:
    case VIDEO_FORMAT_I420:
    case VIDEO_FORMAT_I010:
    case VIDEO_FORMAT_P010:
      return spc == kSpcBt2020 ? kChromaTopLeft : kChromaLeft;  // 4:2:0
    case VIDEO_FORMAT_I422:
    case VIDEO_FORMAT_I210:
    case VIDEO_FORMAT_YUY2:
    case VIDEO_FORMAT_UYVY:
    case VIDEO_FORMAT_YVYU:
      return kChromaLeft;  // 4:2:2
    case VIDEO_FORMAT_I444:
    case VIDEO_FORMAT_I412:
    case VIDEO_FORMAT_YA2L:
      return kChromaCenter;  // 4:4:4
    default:
      return kChromaUnspec;
  }
}

// libobs 30 has no os_process_args_t: on POSIX a command line is built here with every argument
// shell-quoted, because os_process_pipe_create() runs it through popen(). Windows keeps OBS's own
// argument builder.
#ifndef _WIN32
struct MuxArgs {
  std::string command;
  void add(const std::string& value) {
    if (!command.empty()) command += ' ';
    command += '\'';
    for (char c : value) {
      if (c == '\'') command += "'\\''";
      else command += c;
    }
    command += '\'';
  }
  void addf(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    add(buffer);
  }
};
using MuxArgsType = MuxArgs;
#else
using MuxArgsType = os_process_args_t;
#endif

inline void mux_add_arg(MuxArgsType* args, const std::string& value) {
#ifndef _WIN32
  args->add(value);
#else
  os_process_args_add_arg(args, value.c_str());
#endif
}
inline void mux_add_argf(MuxArgsType* args, const char* format, ...) {
  char buffer[512];
  va_list values;
  va_start(values, format);
  std::vsnprintf(buffer, sizeof(buffer), format, values);
  va_end(values);
#ifndef _WIN32
  args->add(buffer);
#else
  os_process_args_add_arg(args, buffer);
#endif
}

void add_video_params(MuxArgsType* args, Ring& ring, obs_encoder_t* encoder) {
  obs_data_t* settings = obs_encoder_get_settings(encoder);
  const int bitrate = static_cast<int>(obs_data_get_int(settings, "bitrate"));
  int codec_tag = static_cast<int>(obs_data_get_int(settings, "codec_type"));
  obs_data_release(settings);
  // The muxer takes the four characters in the other byte order.
  codec_tag = ((codec_tag >> 24) & 0x000000FF) | ((codec_tag << 8) & 0x00FF0000) | ((codec_tag >> 8) & 0x0000FF00) |
              static_cast<int>((static_cast<uint32_t>(codec_tag) << 24) & 0xFF000000u);

  const video_output_info* info = video_output_get_info(obs_get_video());
  int pri = kPriUnspec, trc = kTrcUnspec, spc = kSpcUnspec;
  switch (info->colorspace) {
    case VIDEO_CS_601:
      pri = kPriSmpte170m;
      trc = kTrcSmpte170m;
      spc = kSpcSmpte170m;
      break;
    case VIDEO_CS_DEFAULT:
    case VIDEO_CS_709:
      pri = kPriBt709;
      trc = kTrcBt709;
      spc = kSpcBt709;
      break;
    case VIDEO_CS_SRGB:
      pri = kPriBt709;
      trc = kTrcSrgb;
      spc = kSpcBt709;
      break;
    case VIDEO_CS_2100_PQ:
      pri = kPriBt2020;
      trc = kTrcPq;
      spc = kSpcBt2020;
      break;
    case VIDEO_CS_2100_HLG:
      pri = kPriBt2020;
      trc = kTrcHlg;
      spc = kSpcBt2020;
      break;
  }
  const int range = info->range == VIDEO_RANGE_FULL ? kRangeJpeg : kRangeMpeg;
  const int max_luminance = trc == kTrcPq ? static_cast<int>(obs_get_video_hdr_nominal_peak_level()) : (trc == kTrcHlg ? 1000 : 0);

  mux_add_arg(args, obs_encoder_get_codec(encoder));
  mux_add_argf(args, "%d", bitrate);
  mux_add_argf(args, "%d", static_cast<int>(obs_output_get_width(ring.output)));
  mux_add_argf(args, "%d", static_cast<int>(obs_output_get_height(ring.output)));
  mux_add_argf(args, "%d", pri);
  mux_add_argf(args, "%d", trc);
  mux_add_argf(args, "%d", spc);
  mux_add_argf(args, "%d", range);
  mux_add_argf(args, "%d", chroma_location(info->format, spc));
  mux_add_argf(args, "%d", max_luminance);
  mux_add_argf(args, "%d", static_cast<int>(info->fps_num));
  mux_add_argf(args, "%d", static_cast<int>(info->fps_den));
  mux_add_argf(args, "%d", codec_tag);
}

void add_audio_params(MuxArgsType* args, obs_encoder_t* encoder) {
  obs_data_t* settings = obs_encoder_get_settings(encoder);
  const int bitrate = static_cast<int>(obs_data_get_int(settings, "bitrate"));
  obs_data_release(settings);
  mux_add_arg(args, obs_encoder_get_name(encoder));
  mux_add_argf(args, "%d", bitrate);
  mux_add_argf(args, "%d", static_cast<int>(obs_encoder_get_sample_rate(encoder)));
  mux_add_argf(args, "%d", static_cast<int>(obs_encoder_get_frame_size(encoder)));
#if LIBOBS_API_MAJOR_VER >= 31
  // OBS 31 added the encoder's priming samples to the muxer's audio arguments. 30 does not read it.
  mux_add_argf(args, "%d", static_cast<int>(obs_encoder_get_priming_samples(encoder)));
#endif
  mux_add_argf(args, "%d", static_cast<int>(audio_output_get_channels(obs_get_audio())));
}

// The muxer process for one save: its command line is what OBS's own muxer builds (path, which streams, the codec parameters, the
// muxer settings), and the packets follow on its stdin.
os_process_pipe_t* start_muxer(Ring& ring, const std::string& path) {
  obs_encoder_t* video = obs_output_get_video_encoder(ring.output);
  std::vector<obs_encoder_t*> audio;
  for (int track = 0; track < kMaxTracks; ++track) {
    obs_encoder_t* encoder = obs_output_get_audio_encoder(ring.output, static_cast<size_t>(track));
    if (!encoder) break;
    audio.push_back(encoder);
  }
#ifdef _WIN32
  char* exe = os_get_executable_path_ptr("obs-ffmpeg-mux.exe");
  MuxArgsType* args = os_process_args_create(exe ? exe : "obs-ffmpeg-mux.exe");
  if (exe) bfree(exe);
#else
  // The same helper the runtime staging script points obs-ffmpeg at: it runs from a private per-user
  // folder, never from the read-only AppImage mount or a fixed name in /tmp. The engine staged it
  // before libobs started, so an empty path means the engine cannot record at all.
  const std::string muxer = obs_muxer_path();
  if (muxer.empty()) return nullptr;
  MuxArgs built;
  built.add(muxer);
  MuxArgsType* args = &built;
#endif
  mux_add_arg(args, path.c_str());
  mux_add_argf(args, "%d", video ? 1 : 0);
  mux_add_argf(args, "%d", static_cast<int>(audio.size()));
  if (video) add_video_params(args, ring, video);
  if (!audio.empty()) {
    mux_add_arg(args, obs_encoder_get_codec(audio[0]));
    for (obs_encoder_t* encoder : audio) add_audio_params(args, encoder);
  }
  mux_add_arg(args, "");  // the stream key: none for a file
  obs_data_t* settings = obs_output_get_settings(ring.output);
  const char* muxer_settings = obs_data_get_string(settings, "muxer_settings");
  mux_add_arg(args, muxer_settings ? muxer_settings : "");
  obs_data_release(settings);
#ifdef _WIN32
  os_process_pipe_t* pipe = os_process_pipe_create2(args, "w");
  os_process_args_destroy(args);
#else
  os_process_pipe_t* pipe = os_process_pipe_create(args->command.c_str(), "w");
#endif
  return pipe;
}

bool write_packet(Ring& ring, const PacketFields& packet) {
  PacketInfo info{};
  info.pts = packet.pts;
  info.dts = packet.dts;
  info.size = packet.size;
  info.index = packet.track;
  info.type = packet.video ? kPacketVideo : kPacketAudio;
  info.keyframe = packet.keyframe;
  if (os_process_pipe_write(ring.pipe, reinterpret_cast<const uint8_t*>(&info), sizeof(info)) != sizeof(info)) return false;
  if (os_process_pipe_write(ring.pipe, packet.data, packet.size) != packet.size) return false;
  return true;
}

bool write_headers(Ring& ring) {
  obs_encoder_t* video = obs_output_get_video_encoder(ring.output);
  PacketFields header;
  header.video = true;
  uint8_t* data = nullptr;
  size_t size = 0;
  if (!obs_encoder_get_extra_data(video, &data, &size)) return false;
  header.data = data;
  header.size = static_cast<uint32_t>(size);
  if (!write_packet(ring, header)) return false;
  for (size_t track = 0;; ++track) {
    obs_encoder_t* encoder = obs_output_get_audio_encoder(ring.output, track);
    if (!encoder) break;
    PacketFields audio_header;
    audio_header.video = false;
    audio_header.track = static_cast<uint32_t>(track);
    if (!obs_encoder_get_extra_data(encoder, &data, &size)) return false;
    audio_header.data = data;
    audio_header.size = static_cast<uint32_t>(size);
    if (!write_packet(ring, audio_header)) return false;
  }
  return true;
}

// The details of one save's name, taken on the packet thread when the save is asked for. Strings only: naming the file
// is a file system question (the folder, the names already there), and it runs on the save's own thread.
struct SaveName {
  std::string directory;  // the folder the clip goes in, Windows separators
  std::string stem;       // the configured name without its extension
  std::string suffix;     // the generated extension, with its dot
  uint64_t id = 0;        // this save's number, so its parts and its verdict cannot be taken for another save's
};

// Creates a file exclusively (CREATE_NEW): a name another writer already made refuses, and what this returns owns is the
// save's own. A save only ever creates its staging name (this save's process and number, ring_publish.h), so nothing
// another writer made is opened and the empty file a save that wrote nothing leaves behind is its own to remove.
bool create_exclusive(const std::string& path) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  return true;
}

// A handed-out name stays claimed for this long after its save. The host files the clip into its game's folder as soon
// as the save ends, which frees the root name while a trim can still be reading the file it moved.
constexpr int64_t kClaimHoldMs = 120000;

// This save's tag in a staged name: its process and its own number, so no other save - in this process or another, before
// or after a restart - is writing the file this save writes.
std::string owner_tag(uint64_t save_id) {
  return std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) + "-" + std::to_string(save_id);
}

// The staged name for one attempt: the folder the person chose, cleaned and counted by the one function that hands out
// every clip's name (reserve_output_path), with the marker the library's listing drops (ring_publish.h) and this save's
// tag. libobs splits its paths on '/', so the separator is made one here.
std::string staged_name_for(const SaveName& name, int attempt) {
  std::string staged =
      reserve_output_path(name.directory, ring_staging_stem(name.stem, owner_tag(name.id), attempt), name.suffix);
  std::replace(staged.begin(), staged.end(), '\\', '/');
  return staged;
}

// The staging file this save writes and later publishes, created in the destination folder under a name of its own: the
// muxer or the join opens a file nothing else can have written, and a half-made clip is never listed. Empty when the
// folder cannot be written or every staged name was taken.
std::string stage_output(const SaveName& name) {
  for (int attempt = 0; attempt < kStageNameAttempts; ++attempt) {
    const std::string staged = staged_name_for(name, attempt);
    if (!staged.empty() && create_exclusive(staged)) return staged;
  }
  return std::string();
}

// Brings a finished packet part into the destination folder under this save's staging name (ring_publish.h). The move
// may cross a volume and never replaces, so a name that is somehow taken refuses and the next is tried; the part stays
// where it is on every refusal.
std::string stage_part(const std::string& part, const SaveName& name) {
  return ring_stage(
      part, [&name](int attempt) { return staged_name_for(name, attempt); },
      [](const std::string& from, const std::string& to) {
        return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED) != 0;
      });
}

// True when the file holds at least one byte: footage, whatever the writer's verdict. A save that wrote nothing leaves an
// empty file, and an empty file is not a clip.
bool file_has_bytes(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) return false;
  return info.nFileSizeHigh != 0 || info.nFileSizeLow != 0;
}

// Removes an empty file at a name this save created, when nothing wrote to it: a failed save leaves no 0-byte clip beside
// the clips. Only a name this save made is ever passed here, and a file with bytes in it is footage and stays.
void remove_empty_staged(const std::string& path) {
  if (!file_has_bytes(path)) DeleteFileA(path.c_str());
}

// Publishes this save's staged file under the configured name, at the first name no other file holds, and keeps that name
// as this ring's own for a while. Runs on the save's thread: every attempt asks the file system a question, and nothing
// about a save may ask the packet thread that. Empty when every name was refused; the staged file is untouched then.
std::string publish_staged_output(Ring& ring, const std::string& staged, const SaveName& name) {
  const int64_t now = static_cast<int64_t>(GetTickCount64());
  for (auto it = ring.claimed_paths.begin(); it != ring.claimed_paths.end();) {
    if (now - it->second > kClaimHoldMs) it = ring.claimed_paths.erase(it);
    else ++it;
  }
  const std::string published = ring_publish(
      staged, name.directory, name.stem, name.suffix,
      [](const std::string& directory, const std::string& stem, const std::string& suffix) {
        std::string path = reserve_output_path(directory, stem, suffix);
        std::replace(path.begin(), path.end(), '\\', '/');  // libobs splits its paths on '/'
        return path;
      },
      [](const std::string& from, const std::string& to) {
        // MOVEFILE_REPLACE_EXISTING is never given: a file another writer put at the name keeps every byte it has.
        return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
      },
      [&ring](const std::string& key) { return ring.claimed_paths.count(key) != 0; });
  if (!published.empty()) ring.claimed_paths[ring_name_key(published)] = now;
  return published;
}

// What a save wrote at its staging name becomes the clip when the writer finished, or the footage stays where it is.
// Publishing is the only step that names a file for the person: a writer that failed never publishes, because a truncated
// file under a clip's name would sit in the library as a clip while the verdict says the save failed. A failed writer's
// bytes stay at the staging name (hidden from the listing, owned by this save) and the verdict's path names them, so the
// footage is not lost. A clip no name could be freed for also stays at its staging name, named, and is a failed save.
// True when a name was published.
bool publish_or_keep(Ring& ring, const std::string& staged, const SaveName& name, bool writer_ok, std::string& path) {
  const SaveSettlement settled = ring_settle(
      staged, writer_ok, [](const std::string& candidate) { return file_has_bytes(candidate); }, remove_empty_staged,
      [&ring, &name](const std::string& source) { return publish_staged_output(ring, source, name); });
  path = settled.path;
  if (settled.kept) {
    BX_LOG_WARN("the save did not finish; its bytes stay at %s and are not published", path.c_str());
  } else if (!settled.published && !path.empty()) {
    BX_LOG_ERROR("the clip could not be published under a free name; it stays at %s", path.c_str());
  }
  return settled.published;
}

struct PartsOutcome {
  bool failed = false;
  bool wrote = false;       // an output was produced: a packet part's muxer ran, or the join wrote the clip
  int muxer_exit = 0;       // the exit code of a part's muxer that failed, 0 when every part finished cleanly
  size_t skipped = 0;       // records the clip lacks: chunk files that were gone, an unreadable disk part, parts that could not be joined
  bool unreadable = false;  // the gap came from the ring's disk part
  bool partial = false;     // the clip is the newest part only: footage before it was left out
  // Where the footage ended up: the published name; else the staging name (a writer that failed, or no name could be
  // freed); or a packet part left where it was. The verdict never loses sight of a clip that no name could be freed for,
  // and a failed writer's bytes stay at a name the listing drops. Empty when nothing was written.
  std::string path;
};

// One packet part of a save in parts, through the same muxer process a plain save uses. False when the muxer, a packet or
// the muxer's own exit code failed; `outcome.wrote` says a process was created and `outcome.muxer_exit` carries its
// verdict (0 clean).
bool write_packet_part(Ring& ring, Store::SaveJob& job, const std::vector<uint32_t>& entries, const std::string& path,
                       PartsOutcome& outcome) {
  ring.pipe = start_muxer(ring, path);
  if (!ring.pipe) {
    BX_LOG_WARN("the ring could not start its muxer process for %s", path.c_str());
    return false;
  }
  outcome.wrote = true;
  bool ok = write_headers(ring);
  if (ok) {
    const SaveReport report = ring.store.run_part(job, entries, [&](const PacketFields& packet) { return write_packet(ring, packet); });
    outcome.skipped += report.skipped;
    outcome.unreadable = outcome.unreadable || report.unreadable;
    ok = report.ok;
  }
  // Waits for the muxer to finish the file. Its exit code is its verdict on the file: a disk that filled as the
  // last fragment was written took every packet and still left a file that is not a clip.
  outcome.muxer_exit = os_process_pipe_destroy(ring.pipe);
  ring.pipe = nullptr;
  if (outcome.muxer_exit != 0) {
    BX_LOG_WARN("the muxer ended with code %d writing %s", outcome.muxer_exit, path.c_str());
    ok = false;
  }
  return ok;
}

// The newest packet part becomes the clip when the join is refused. The part is first brought into the destination folder
// under this save's staging name (across a volume when it must; the parts live beside the ring's own file), and published
// from there by a rename that never replaces: a file another writer put at the name keeps its own bytes and the save takes
// the next name. When no name can be freed the part is not deleted and `path` names where it really is, because the
// footage exists and the person must be able to reach it.
bool publish_newest_part(Ring& ring, std::string& source, const SaveName& name, std::string& path) {
  const std::string staged = stage_part(source, name);
  if (staged.empty()) {
    BX_LOG_ERROR("the newest part of the ring's clip could not be staged beside the clips; it stays at %s", source.c_str());
    path = source;
    return false;
  }
  source.clear();  // its bytes are at the staging name now; the old name holds nothing
  return publish_or_keep(ring, staged, name, true, path);  // the part's own writer finished: it is a valid clip
}

// A save when some of the ring's footage is in Timeline chunk files (ring_parts.h): the packets the ring still has go through the muxer in parts, each
// starting on a keyframe, and the parts and the chunk files are joined by stream copy. When the join is refused the clip is the newest packet part, said in the
// count of records left out; parts that are not adjacent are never glued together. Everything is written at `staged`,
// this save's own name in the destination folder, and published only once the writer is done, so the name the person
// asked for never holds a half-made clip.
PartsOutcome save_in_parts(Ring& ring, Store::SaveJob& job, const SaveName& name, const std::string& staged) {
  PartsOutcome outcome;
  outcome.path = staged;
  // What was written at the staging name becomes the clip when the writer finished. A writer that failed never publishes:
  // its bytes stay at the staging name for recovery, and the verdict's path names them. A name that could not be freed
  // is a failed save, whatever the writer did: the file never reached the name the person chose.
  bool settled = false;  // the staging file was decided: published, kept for a failed writer, or a refused name
  const auto settle = [&](bool writer_ok) {
    settled = true;
    if (!publish_or_keep(ring, staged, name, writer_ok, outcome.path)) outcome.failed = true;
  };
  // Every Timeline session this save may read a chunk file from is guarded for the whole read (timeline_retention.h):
  // retention skips it and an explicit erase is refused, until this save is finished with the files - however long a
  // slow join takes, where the ring's renewed dedupe lease could have expired. A guard that could not be taken means
  // a removal already owns that session: the save does not open its chunks at all, and saves what the ring still
  // holds, rather than reading a file another pass is deleting.
  std::vector<TimelineReadGuard> guards;
  for (const auto& entry : job.chunks) {
    const std::string& session = entry.second.session_id;
    if (session.empty()) continue;
    if (std::any_of(guards.begin(), guards.end(),
                    [&session](const TimelineReadGuard& guard) { return guard.session_id() == session; })) {
      continue;
    }
    TimelineReadGuard guard = TimelineReadGuard::take(session);
    if (!guard.active()) BX_LOG_WARN("a replay save leaves out the timeline chunks of %s: that session is being removed", session.c_str());
    guards.push_back(std::move(guard));
  }
  const auto guarded = [&guards](const std::string& session) {
    return std::any_of(guards.begin(), guards.end(),
                       [&session](const TimelineReadGuard& guard) { return guard.active() && guard.session_id() == session; });
  };
  const SavePlan plan = ring.store.plan_save(job, [&guarded](const ChunkRec& chunk) {
    // A chunk of a session a removal already owns is not read; a chunk with no session (a store used without the
    // host) is judged by its file alone, as it always was.
    if (!chunk.session_id.empty() && !guarded(chunk.session_id)) return false;
    return GetFileAttributesA(chunk.file.c_str()) != INVALID_FILE_ATTRIBUTES;
  });
  std::vector<uint32_t> all(job.entries.size());
  for (uint32_t i = 0; i < all.size(); ++i) all[i] = i;
  if (!plan.valid) {
    BX_LOG_WARN("the ring's chunk records do not fit its chunk files; saving what the ring holds");
    const bool wrote = write_packet_part(ring, job, all, staged, outcome);
    outcome.partial = outcome.skipped > 0 || outcome.unreadable;
    settle(wrote);
    return outcome;
  }
  outcome.skipped = plan.lost;
  if (plan.parts.empty()) {
    // A valid plan can hold no part: every picture the snapshot names was taken out of RAM for a chunk file that is gone
    // (erased, or being erased), and only sound is left. There is nothing to write and nothing to concatenate - the
    // empty plan has no front part - so no clip is made. The verdict is a failure with the records it lacked, and the
    // settlement removes this save's own empty staging file; no chunk file is opened and no part is left behind.
    outcome.skipped = job.entries.size();
    BX_LOG_ERROR("a replay save has nothing it can read: its %zu records were left out, so no clip was made", outcome.skipped);
    settle(false);
    return outcome;
  }
  if (plan.parts.size() == 1 && !plan.parts[0].file) {
    const bool wrote = write_packet_part(ring, job, plan.parts[0].entries, staged, outcome);
    outcome.partial = outcome.skipped > 0 || outcome.unreadable;
    settle(wrote);
    return outcome;
  }
  // The temporary parts live beside the ring file (the hidden .buffrx folder), else beside the clip. The save's own id
  // is in their names, so a part left behind by a save that failed cannot be taken for this one's.
  std::string folder = ring.spill_path;
  const size_t folder_end = folder.find_last_of("/\\");
  folder = folder_end == std::string::npos ? std::string() : folder.substr(0, folder_end);
  if (folder.empty()) folder = staged.substr(0, staged.find_last_of('/')) + "/.buffrx";
  os_mkdirs(folder.c_str());
  const size_t dot = staged.find_last_of('.');
  const std::string extension = dot == std::string::npos ? std::string(".mp4") : staged.substr(dot);
  std::vector<std::string> files, temps(plan.parts.size());
  for (size_t i = 0; i < plan.parts.size() && !outcome.failed; ++i) {
    const SavePart& part = plan.parts[i];
    if (part.file) {
      for (const uint32_t id : part.chunks) files.push_back(job.chunks.at(id).file);
      continue;
    }
    temps[i] = folder + "/part-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(name.id) + "-" + std::to_string(i) + extension;
    if (!write_packet_part(ring, job, part.entries, temps[i], outcome)) outcome.failed = true;
    files.push_back(temps[i]);
  }
  if (!outcome.failed) {
    // The cut is the keyframe at the ring's front; a little past it, so the seek lands on that keyframe and not the one before.
    const double start = plan.parts.front().file ? plan.parts.front().start_in_first + 0.01 : 0.0;
    std::string error;
    if (media_concat(files, staged, start, 1e9, error)) {
      // The join is the writer here: no muxer process wrote this clip, and the verdict below reads a produced file, not
      // a process having run.
      outcome.wrote = true;
      settle(true);
    } else {
      BX_LOG_ERROR("the replay ring could not join its chunk files with its packets (%s)", error.c_str());
      const int newest = newest_packet_part(plan);
      if (newest >= 0 && publish_newest_part(ring, temps[static_cast<size_t>(newest)], name, outcome.path)) {
        outcome.skipped = job.entries.size() - plan.parts[static_cast<size_t>(newest)].entries.size();
        outcome.partial = true;
        BX_LOG_ERROR("the clip %s holds only the newest %zu records of the ring", outcome.path.c_str(), plan.parts[static_cast<size_t>(newest)].entries.size());
      } else {
        outcome.failed = true;
        if (newest >= 0) {
          // The one path where a valid clip cannot be published: the part stays where it is (the verdict's path names
          // it) rather than being deleted. The part may already have been staged into the destination folder, which is
          // the path the verdict holds.
          BX_LOG_ERROR("the newest part of the ring's clip could not be published; it stays at %s", outcome.path.c_str());
          temps[static_cast<size_t>(newest)].clear();  // the verdict's path names it; this cleanup does not touch it
        }
      }
    }
  }
  // A save whose writer gave up keeps every part with bytes in it: those bytes are the only copy of that footage until
  // a save is asked again, and a part beside the ring's file (the hidden .buffrx folder, under this save's own name) is
  // recoverable where a deleted file is not. A save that reached its staging file deletes its parts, whose bytes the
  // staging file now holds, and an empty part is never a clip and is removed.
  std::string newest_kept;
  for (const std::string& temp : temps) {
    if (temp.empty()) continue;
    if (!outcome.failed || settled) DeleteFileA(temp.c_str());
    else if (file_has_bytes(temp)) newest_kept = temp;
    else remove_empty_staged(temp);
  }
  if (outcome.failed && !settled) {
    // No staging file was decided: the staging file holds whatever the writer left there - kept when it has bytes,
    // removed when it is empty - and a kept part is named only when nothing more recent holds footage.
    std::string kept_stage;
    publish_or_keep(ring, staged, name, false, kept_stage);
    if (outcome.path.empty() || !file_has_bytes(outcome.path)) {
      outcome.path = kept_stage.empty() ? newest_kept : kept_stage;
    }
    if (!newest_kept.empty()) {
      BX_LOG_ERROR("the save failed; its packet parts are kept beside the ring's file for recovery, such as %s",
                   newest_kept.c_str());
    }
  }
  if (outcome.skipped > 0 || outcome.unreadable || outcome.partial) outcome.partial = true;
  if (outcome.unreadable) BX_LOG_ERROR("the disk part of the replay ring could not be read: %zu records were left out of %s, up to the next keyframe, so the clip has a gap", outcome.skipped, outcome.path.c_str());
  if (plan.lost > 0) BX_LOG_ERROR("%zu records of the replay ring were in chunk files that are gone: %s has a gap", plan.lost, outcome.path.c_str());
  return outcome;
}

// The verdict of a save is kept before the signal, so the host reads this save's result and never an older one's.
void finish_save(Ring& ring, const SaveOutcome& result) {
  {
    std::lock_guard<std::mutex> lock(ring.result_mutex);
    ring.last_result = result;
  }
  if (!save_produced_clip(result)) {
    if (result.path.empty()) {
      BX_LOG_WARN("the ring's save wrote no file at all (muxer exit %d): there is no clip to announce", result.muxer_exit);
    } else {
      BX_LOG_WARN("the ring's save of %s failed (muxer exit %d): a clip cannot be announced", result.path.c_str(), result.muxer_exit);
    }
  } else if (save_left_footage_out(result)) {
    BX_LOG_WARN("the ring wrote %s, and it leaves %zu records out", result.path.c_str(), result.skipped);
  } else {
    BX_LOG_INFO("the ring wrote %s", result.path.c_str());
  }
  // Signalled whether the save worked or not: the host clears its "a save is in flight" flag on this signal, and a failed
  // save that stayed silent refused every later clip. What it worked out is in `save_result`, not in this signal.
  calldata_t data{};
  signal_handler_signal(obs_output_get_signal_handler(ring.output), "saved", &data);
}

void mux_thread_main(Ring* ring, const SaveName& name, int seconds) {
  // The view of the ring is taken here, on this thread, so the packet thread is free the moment a save is asked for.
  // Naming the file happens here too: it is a file system question, and the packet thread's only job is the encoders.
  std::unique_ptr<Store::SaveJob> job = ring->store.begin_save(seconds);
  const std::string staged = stage_output(name);
  {
    std::lock_guard<std::mutex> lock(ring->path_mutex);
    ring->path = staged;
  }
  if (staged.empty()) {
    // No staged name could be made: the folder refused the file. The save says so instead of writing over anything; the
    // signal below is the one every save sends, so the host clears its in-flight flag and its own path check reports it.
    BX_LOG_ERROR("the ring found no free name for a save in %s: no file was replaced", name.directory.c_str());
    job.reset();
    ring->muxing = false;
    finish_save(*ring, save_result(name.id, std::string(), false, false, 0, 0, false, false));
    if (!ring->active) teardown(*ring);  // the ring went down under this save: its memory goes now, and nothing may start another
    return;
  }
  ring->last_skipped = 0;
  if (snapshot_has_chunks(job->entries)) {
    PartsOutcome outcome = save_in_parts(*ring, *job, name, staged);
    {
      std::lock_guard<std::mutex> lock(ring->path_mutex);
      // The published name, or the staging name the footage stayed at when no name could be freed.
      ring->path = outcome.path;
    }
    ring->last_skipped = static_cast<int>(std::min<size_t>(outcome.skipped, 1000000));
    const SaveOutcome result = save_parts_result(name.id, outcome.path, outcome.wrote, outcome.failed, outcome.muxer_exit,
                                                 outcome.skipped, outcome.unreadable, outcome.partial);
    job.reset();
    ring->muxing = false;
    finish_save(*ring, result);
    if (!ring->active) teardown(*ring);  // the ring went down under this save: its memory goes now, and nothing may start another
    return;
  }
  bool wrote_every_packet = true;
  bool unreadable = false;
  size_t skipped = 0;
  int muxer_exit = 0;
  ring->pipe = start_muxer(*ring, staged);
  if (!ring->pipe) {
    BX_LOG_WARN("the ring could not start its muxer process for %s", staged.c_str());
    wrote_every_packet = false;
  } else {
    if (!write_headers(*ring)) {
      BX_LOG_WARN("the ring could not write the headers of %s", staged.c_str());
      wrote_every_packet = false;
    } else {
      const SaveReport report = ring->store.run_save(*job, [&](const PacketFields& packet) { return write_packet(*ring, packet); });
      skipped = report.skipped;
      unreadable = report.unreadable;
      if (!report.ok) {
        BX_LOG_WARN("the ring could not write a packet of %s", staged.c_str());
        wrote_every_packet = false;
      }
      if (report.unreadable) {
        BX_LOG_ERROR("the disk part of the replay ring could not be read: %zu packets were left out of %s, up to the next keyframe, so the clip has a gap",
                     report.skipped, staged.c_str());
      }
    }
    // Waits for the muxer to finish the file. Its exit code is its verdict on the file: a disk that filled as the
    // last fragment was written took every packet and still left a file that is not a clip.
    muxer_exit = os_process_pipe_destroy(ring->pipe);
  }
  ring->pipe = nullptr;
  // The writer's verdict: every write reached it and its process ended cleanly. Only then may the file take the name the
  // person sees. A failed writer's bytes stay at the staging name (hidden from the listing and owned by this save) and the
  // verdict's path names them, so a truncated clip is never published or announced while a failure is reported.
  const bool writer_ok = wrote_every_packet && muxer_exit == 0;
  std::string kept;
  const bool published = publish_or_keep(*ring, staged, name, writer_ok, kept);
  ring->last_skipped = static_cast<int>(std::min<size_t>(skipped, 1000000));
  job.reset();  // the references to the RAM packets and the pin on the disk part
  {
    std::lock_guard<std::mutex> lock(ring->path_mutex);
    ring->path = kept;
  }
  ring->muxing = false;
  finish_save(*ring, save_result(name.id, kept, !kept.empty(), published, muxer_exit, skipped, unreadable, false));

  if (!ring->active) teardown(*ring);  // the ring went down under this save: its memory goes now, and nothing may start another
}

// A save: a thread of its own takes the view of the ring and writes it out. The snapshot used to be taken on the
// packet thread, the one libobs feeds encoded frames to; a ring holding a minute at a high rate made that thread stop
// long enough for the capture to stall, and a stalled game-capture hook can hold the game's own present. Nothing about
// a save may run on the thread that feeds the encoders, and that includes choosing the file's name: the configured
// name is made here, at the second the save was asked for, and the folder and the names already in it are asked on the
// save's own thread.
void start_save(Ring& ring) {
  if (!ring.active) return;  // deactivated under the ask: a save must never start after the ring is down
  obs_data_t* settings = obs_output_get_settings(ring.output);
  const char* directory_setting = obs_data_get_string(settings, "directory");
  const char* format = obs_data_get_string(settings, "format");
  const char* extension = obs_data_get_string(settings, "extension");
  const bool spaces = obs_data_get_bool(settings, "allow_spaces");
  char* filename = os_generate_formatted_filename(extension, spaces, format);
  // The generated name carries its extension and may carry a folder of its own: the folder joined the directory, the
  // rest is the stem this save's staged name and then its published name are built from.
  std::string generated = filename ? filename : "";
  bfree(filename);
  std::string folder;
  const size_t name_slash = generated.find_last_of("/\\");
  if (name_slash != std::string::npos) {
    folder = generated.substr(0, name_slash);
    generated.erase(0, name_slash + 1);
  }
  const size_t dot = generated.find_last_of('.');
  const std::string stem = dot == std::string::npos ? generated : generated.substr(0, dot);
  const std::string suffix = dot == std::string::npos
                                 ? std::string(".") + (extension && *extension ? extension : "mp4")
                                 : generated.substr(dot);
  std::string directory = directory_setting ? directory_setting : "";
  std::replace(directory.begin(), directory.end(), '/', '\\');
  while (!directory.empty() && directory.back() == '\\') directory.pop_back();
  if (directory.empty()) directory = ".";
  if (!folder.empty()) {
    std::replace(folder.begin(), folder.end(), '/', '\\');
    directory += "\\" + folder;
  }
  obs_data_release(settings);
  SaveName name;
  name.directory = std::move(directory);
  name.stem = stem.empty() ? std::string("Replay") : stem;
  name.suffix = suffix;
  name.id = ring.save_seq.fetch_add(1) + 1;
  ring.muxing = true;
  const int seconds = ring.save_seconds.exchange(0);
  ring.mux_thread = std::thread([&ring, name, seconds]() { mux_thread_main(&ring, name, seconds); });
}

// Starts the save that was asked for, unless one is still being written (it then waits for the next packet). The
// packet thread comes here on the first packet after the ask, which is the usual way. The host's thread comes here
// for a "save now": a capture that froze delivers no packet to start the save on, and that is exactly when the
// ring is saved before a restart throws it away.
void begin_asked_save(Ring& ring) {
  std::lock_guard<std::mutex> lock(ring.save_mutex);
  if (!ring.save_ts || ring.muxing) return;
  join_mux(ring);
  ring.save_ts = 0;
  start_save(ring);
}

void deactivate(Ring& ring, int code) {
  if (code) obs_output_signal_stop(ring.output, code);
  else if (ring.stopping) obs_output_end_data_capture(ring.output);
  {
    // Under the save's lock: a "save now" from the host's thread either started before this (and is seen below as
    // muxing) or finds the ring inactive and starts nothing.
    std::lock_guard<std::mutex> lock(ring.save_mutex);
    ring.active = false;
  }
  ring.stopping = false;
  // A save in flight is reading the ring, so its end waits for the muxer's thread, which tears the ring down itself when it sees the
  // ring is no longer active; otherwise memory goes back at once.
  if (ring.muxing) ring.save_ts = 0;
  else teardown(ring);
}

// ---------------------------------------------------------------- libobs callbacks

const char* ring_name(void*) { return "BuffrX replay ring"; }

void save_proc(void* data, calldata_t*) {
  auto* ring = static_cast<Ring*>(data);
  if (!ring->active) return;
  obs_encoder_t* video = obs_output_get_video_encoder(ring->output);
  if (video && obs_encoder_paused(video)) {
    BX_LOG_INFO("the ring was not saved: the encoders are paused");
    return;
  }
  ring->save_seconds = 0;
  ring->save_ts = static_cast<int64_t>(os_gettime_ns() / 1000ull);
}

// The save the host asks for: the last `seconds` of the ring, cut at a keyframe before it is written, so the muxer's file is
// already the length the person chose. A trim afterwards is then the exception (a keyframe a little early), not the rule.
void save_seconds_proc(void* data, calldata_t* call) {
  auto* ring = static_cast<Ring*>(data);
  if (!ring->active) return;
  obs_encoder_t* video = obs_output_get_video_encoder(ring->output);
  if (video && obs_encoder_paused(video)) {
    BX_LOG_INFO("the ring was not saved: the encoders are paused");
    return;
  }
  long long seconds = 0, now = 0;
  calldata_get_int(call, "seconds", &seconds);
  calldata_get_int(call, "now", &now);
  ring->save_seconds = static_cast<int>(std::clamp<long long>(seconds, 0, 3600));
  ring->save_ts = static_cast<int64_t>(os_gettime_ns() / 1000ull);
  if (now) begin_asked_save(*ring);
}

void last_replay_proc(void* data, calldata_t* call) {
  auto* ring = static_cast<Ring*>(data);
  if (ring->muxing) return;
  std::lock_guard<std::mutex> lock(ring->path_mutex);
  calldata_set_string(call, "path", ring->path.c_str());
}

// What the last save came to: "saved" says a save ended, not that it worked. The host reads this on its own thread after
// the signal (never inside it), and a save whose muxer failed is a failure there even when a file of the right size is
// left behind.
void save_result_proc(void* data, calldata_t* call) {
  auto* ring = static_cast<Ring*>(data);
  SaveOutcome result;
  {
    std::lock_guard<std::mutex> lock(ring->result_mutex);
    result = ring->last_result;
  }
  calldata_set_int(call, "id", static_cast<long long>(result.id));
  calldata_set_bool(call, "finished", result.finished);
  calldata_set_bool(call, "ok", result.ok);
  calldata_set_bool(call, "partial", result.partial);
  calldata_set_int(call, "skipped", static_cast<long long>(result.skipped));
  calldata_set_bool(call, "unreadable", result.unreadable);
  calldata_set_int(call, "muxer_exit", result.muxer_exit);
  calldata_set_string(call, "path", result.path.c_str());
}

// What the ring holds right now: bytes, packets, the span in milliseconds, the bytes received since it started, and the disk part's state.
void info_proc(void* data, calldata_t* call) {
  auto* ring = static_cast<Ring*>(data);
  const StoreInfo info = ring->store.info();
  const int state = info.spill_failed ? 2 : (info.spill ? 1 : ring->spill_state.load());
  calldata_set_int(call, "bytes", info.bytes);
  calldata_set_int(call, "packets", static_cast<long long>(info.packets));
  calldata_set_int(call, "span_ms", info.span_us / 1000);
  calldata_set_int(call, "total_bytes", static_cast<long long>(info.total_bytes));
  calldata_set_int(call, "ram_bytes", info.ram_bytes);
  calldata_set_int(call, "disk_bytes", info.disk_bytes);
  calldata_set_int(call, "spilled_bytes", info.spilled_total);
  calldata_set_int(call, "state", state);
  calldata_set_int(call, "stalled", info.stalled ? 1 : 0);
  calldata_set_int(call, "backstop", info.backstop_events);
  calldata_set_int(call, "skipped", ring->last_skipped.load());
  calldata_set_int(call, "chunk_bytes", info.chunk_bytes);
  calldata_set_int(call, "deduped_bytes", info.deduped_total);
  calldata_set_int(call, "chunks_linked", info.chunks_linked);
  calldata_set_int(call, "chunks_waiting", info.chunks_waiting);
}

// The Timeline dedupe's three calls (obs_host_dedupe.cpp): hand over a probed chunk, say whether the spill may rely on one, and list what the ring knows.
void link_chunk_proc(void* data, calldata_t* call) {
  auto* ring = static_cast<Ring*>(data);
  void* probe = nullptr;
  if (!calldata_get_ptr(call, "probe", &probe) || !probe) return;
  calldata_set_int(call, "id", ring->store.add_chunk(*static_cast<ChunkProbe*>(probe)));
}

void chunk_usable_proc(void* data, calldata_t* call) {
  auto* ring = static_cast<Ring*>(data);
  long long id = 0, usable = 0;
  calldata_get_int(call, "id", &id);
  calldata_get_int(call, "usable", &usable);
  ring->store.set_chunk_usable(static_cast<uint32_t>(id), usable != 0);
}

void chunk_states_proc(void* data, calldata_t* call) {
  auto* ring = static_cast<Ring*>(data);
  void* out = nullptr;
  if (!calldata_get_ptr(call, "states", &out) || !out) return;
  std::vector<RingChunkState>& states = *static_cast<std::vector<RingChunkState>*>(out);
  states.clear();
  for (const Store::ChunkState& state : ring->store.chunk_states()) states.push_back(RingChunkState{state.id, state.file, state.linked, state.usable, state.at, state.count});
}

// Why the disk part is unavailable, once it is (empty otherwise).
void problem_proc(void* data, calldata_t* call) {
  auto* ring = static_cast<Ring*>(data);
  std::lock_guard<std::mutex> lock(ring->problem_mutex);
  calldata_set_string(call, "text", ring->spill_problem.c_str());
}

void* ring_create(obs_data_t*, obs_output_t* output) {
  auto* ring = new Ring;
  ring->output = output;
  proc_handler_t* procs = obs_output_get_proc_handler(output);
  proc_handler_add(procs, "void save()", save_proc, ring);
  proc_handler_add(procs, "void save_seconds(int seconds, int now)", save_seconds_proc, ring);
  proc_handler_add(procs, "void get_last_replay(out string path)", last_replay_proc, ring);
  proc_handler_add(procs,
                   "void save_result(out int id, out bool finished, out bool ok, out bool partial, out int skipped, "
                   "out bool unreadable, out int muxer_exit, out string path)",
                   save_result_proc, ring);
  proc_handler_add(procs,
                   "void ring_info(out int bytes, out int packets, out int span_ms, out int total_bytes, out int ram_bytes, out int disk_bytes, "
                   "out int spilled_bytes, out int state, out int stalled, out int backstop, out int skipped, "
                   "out int chunk_bytes, out int deduped_bytes, out int chunks_linked, out int chunks_waiting)",
                   info_proc, ring);
  proc_handler_add(procs, "void spill_problem(out string text)", problem_proc, ring);
  proc_handler_add(procs, "void link_chunk(ptr probe, out int id)", link_chunk_proc, ring);
  proc_handler_add(procs, "void chunk_usable(int id, int usable)", chunk_usable_proc, ring);
  proc_handler_add(procs, "void chunk_states(ptr states)", chunk_states_proc, ring);
  signal_handler_add(obs_output_get_signal_handler(output), "void saved()");
  return ring;
}

void ring_destroy(void* data) {
  auto* ring = static_cast<Ring*>(data);
  join_mux(*ring);
  teardown(*ring);
  delete ring;
}

bool ring_start(void* data) {
  auto* ring = static_cast<Ring*>(data);
  if (!obs_output_can_begin_data_capture(ring->output, 0)) return false;
  if (!obs_output_initialize_encoders(ring->output, 0)) return false;
  join_mux(*ring);  // a save from before a restart
  teardown(*ring);
  {
    // A fresh ring has no save verdict: an older one must not be read as this buffer's.
    std::lock_guard<std::mutex> lock(ring->result_mutex);
    ring->last_result = SaveOutcome{};
  }
  obs_data_t* settings = obs_output_get_settings(ring->output);
  ring->store.configure(obs_data_get_int(settings, "max_time_sec") * 1000000ll, obs_data_get_int(settings, "max_size_mb") * (1024ll * 1024ll));
  {
    // The Timeline dedupe (obs_host_dedupe.cpp decides whether to): the ring hashes the video keyframes of the encoder's codec, and only that codec.
    obs_encoder_t* video = obs_output_get_video_encoder(ring->output);
    const bool dedupe = obs_data_get_bool(settings, "dedupe");
    ring->store.configure_dedupe(dedupe && video ? hash_codec_from_name(obs_encoder_get_codec(video)) : HashCodec::other);
  }
  ring->spill_bytes = obs_data_get_int(settings, "spill_bytes");
  ring->ram_limit = obs_data_get_int(settings, "ram_limit_mb") * (1024ll * 1024ll);
  const char* spill_path = obs_data_get_string(settings, "spill_path");
  ring->spill_path = spill_path ? spill_path : "";
  obs_data_release(settings);
  {
    std::lock_guard<std::mutex> lock(ring->problem_mutex);
    ring->spill_problem.clear();
  }
  if (ring->spill_bytes > 0 && ring->ram_limit > 0 && !ring->spill_path.empty()) {
    ring->file = std::make_unique<RingFile>();
    ring->spill_run = true;
    ring->spill_thread = std::thread(spill_main, ring);
  }
  {
    char key_path[1024] = {0};
    if (GetEnvironmentVariableA("BUFFRX_RING_KEYLOG", key_path, sizeof(key_path)) > 0) {
      ring->keylog = std::fopen(key_path, "wb");
      obs_encoder_t* video = obs_output_get_video_encoder(ring->output);
      ring->keylog_codec = hash_codec_from_name(video ? obs_encoder_get_codec(video) : nullptr);
    }
  }
  ring->active = true;
  ring->capturing = true;
  obs_output_begin_data_capture(ring->output, 0);
  return true;
}

void ring_stop(void* data, uint64_t ts) {
  auto* ring = static_cast<Ring*>(data);
  if (ring->capturing || ts == 0) {
    ring->stop_ts = static_cast<int64_t>(ts) / 1000;
    ring->stopping = true;
    ring->capturing = false;
  }
}

void ring_packet(void* data, encoder_packet* packet) {
  auto* ring = static_cast<Ring*>(data);
  if (!ring->active) return;
  if (!packet) {  // the encoder failed
    deactivate(*ring, OBS_OUTPUT_ENCODE_ERROR);
    return;
  }
  if (ring->stopping && packet->sys_dts_usec >= ring->stop_ts) {
    deactivate(*ring, 0);
    return;
  }
  if (ring->keylog) {
    if (packet->type == OBS_ENCODER_VIDEO) {
      const VclDigest digest = vcl_digest(ring->keylog_codec, packet->data, packet->size, 0);
      std::fprintf(ring->keylog, "v %lld %d %zu %u %016llx\n", static_cast<long long>(packet->dts_usec), packet->keyframe ? 1 : 0, packet->size,
                   digest.ok ? digest.bytes : 0u, static_cast<unsigned long long>(digest.ok ? digest.hash : 0));
    } else {
      std::fprintf(ring->keylog, "a %d %lld %zu %016llx\n", static_cast<int>(packet->track_idx), static_cast<long long>(packet->dts_usec), packet->size,
                   static_cast<unsigned long long>(payload_hash(packet->data, packet->size)));
    }
    std::fflush(ring->keylog);  // a probe: the file must be whole when the engine is stopped in a hurry
  }
  encoder_packet kept;
  obs_encoder_packet_ref(&kept, packet);
  ring->store.push(kept);
  if (ring->store.spill_due()) ring->spill_cv.notify_one();
  // While the last save is still being written this one waits for it.
  if (ring->save_ts && packet->sys_dts_usec >= ring->save_ts && !ring->muxing) begin_asked_save(*ring);
}

uint64_t ring_total_bytes(void* data) { return static_cast<Ring*>(data)->store.info().total_bytes; }

void ring_defaults(obs_data_t* settings) {
  obs_data_set_default_int(settings, "max_time_sec", 15);
  obs_data_set_default_int(settings, "max_size_mb", 0);
  obs_data_set_default_int(settings, "spill_bytes", 0);
  obs_data_set_default_int(settings, "ram_limit_mb", 0);
  obs_data_set_default_string(settings, "spill_path", "");
  obs_data_set_default_bool(settings, "dedupe", false);
  obs_data_set_default_string(settings, "format", "%CCYY-%MM-%DD %hh-%mm-%ss");
  obs_data_set_default_string(settings, "extension", "mp4");
  obs_data_set_default_bool(settings, "allow_spaces", true);
}

}  // namespace

void register_ring_output() {
  obs_output_info info{};
  info.id = "buffrx_ring";
  info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK | OBS_OUTPUT_CAN_PAUSE;
  info.get_name = ring_name;
  info.create = ring_create;
  info.destroy = ring_destroy;
  info.start = ring_start;
  info.stop = ring_stop;
  info.encoded_packet = ring_packet;
  info.get_total_bytes = ring_total_bytes;
  info.get_defaults = ring_defaults;
  obs_register_output(&info);
}

}  // namespace bx
