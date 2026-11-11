// Media access through the FFmpeg that ships inside the runtime.
//
// The pinned OBS release carries FFmpeg as DLLs (avformat-62, avcodec-62, avutil-60, swscale-9,
// swresample-6) and no `ffmpeg.exe`, so the library's thumbnails and durations and the editor's trim
// link those DLLs directly. Nothing here re-encodes: trim is a stream copy, and a thumbnail decodes
// exactly one frame and hands it to WIC (the encoder the screenshot path already uses).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "media_math.h"
#include "platform.h"
#include "../shared/json.h"

struct AVCodecContext;
struct AVCodecParameters;
struct AVFormatContext;
struct AVPacket;

namespace bx {

// Every video decode in the engine goes through these two, so a stream a decoder cannot read as it is
// stored is fixed in one place. Today that is AV1 from OBS's NVENC: it declares level 7.3, which libaom
// (the runtime's one software AV1 decoder) refuses outright, so the level is made "unconstrained" in
// each packet that carries a sequence header (av1_unconstrain_level). `video_packets_need_fixing` says
// whether a stream is one of those; `send_video_packet` is avcodec_send_packet with the fix applied.
bool video_packets_need_fixing(const AVCodecParameters* parameters);
int send_video_packet(AVCodecContext* decoder, const AVPacket* packet, bool fix);
// FFmpeg's stream probe decodes without that fix, so for such a stream it leaves the pixel format
// unknown, and anything built from the parameters (an export's filter graph) is refused. This decodes
// the first frame the fixed way, fills the format in, and seeks back to the start. True when the
// format is known afterwards.
bool learn_video_format(AVFormatContext* format, int stream_index);

struct MediaInfo {
  bool ok = false;
  std::string reason;       // why not, when ok is false
  double duration_seconds = 0.0;
  int width = 0;
  int height = 0;
  std::string video_codec;  // "h264", "av1", â€¦
  std::string audio_codec;
  // The video stream's rate, from the stream or the container's average less the sound; 0 when neither says.
  int video_kbps = 0;
};

// Installs the engine's own FFmpeg log callback, once. FFmpeg's default writes every message to
// stderr, and a demuxer warning then appears once per file the library opens. Warnings and errors
// reach the engine log with an "ffmpeg:" prefix; a warning already said is dropped, so the noise of a
// listing cannot flood the log. Decoding is untouched: the av_log level is left as it is.
void media_install_log_callback();

// Reads the container: duration, the video stream's size and both codec names.
MediaInfo media_probe(const std::string& path);
// Every video keyframe in the file, in seconds: the editor draws them as ticks and Alt snaps cuts to them.
bool media_keyframes(const std::string& path, std::vector<double>& times, std::string& error);

// The thumbnail box: the requested width, the source's aspect ratio, never upscaled, always even.
void thumbnail_size(int source_width, int source_height, int target_width, int& out_width, int& out_height);

// The part of the file a trim will copy, clamped to what exists. False when nothing would be copied.
bool trim_range(double duration_seconds, double requested_start, double requested_length, double& start_seconds,
                double& length_seconds);

// Writes a small JPEG for the library grid: one frame decoded at `at_seconds`, scaled, WIC-encoded.
bool media_thumbnail(const std::string& path, const std::string& output_path, int width, double at_seconds,
                     std::string& error);

// The loudness of a file's sound, one linear RMS value (0..1) per second from its start: the loudest audio track
// wins each second. Empty with no error when the file has no audio. Decodes audio only, on one thread.
bool media_loudness(const std::string& path, std::vector<float>& rms_per_second, std::string& error);

// An image (or a video's first frame) as BGRA, fitted inside a `box`-pixel square and never upscaled:
// what the interface draws for thumbnails and the screenshot preview.
bool media_decode_image(const std::string& path, int box, std::vector<uint8_t>& bgra, int& width, int& height,
                        std::string& error);

// Stream copy of [start, start + length) into `output_path`. The copy begins at the keyframe at or
// before `start`, which is the same "nearest keyframe" boundary the save path uses;
// `first_keyframe_seconds` reports that keyframe's own time in the source, which is where the copy
// begins reading and what an edit list then hides.
bool media_trim(const std::string& path, const std::string& output_path, double start_seconds, double length_seconds,
                std::string& error, double* first_keyframe_seconds = nullptr);

// Stream copy of several chunks into one file, end to end on one timeline: from the keyframe at or
// before `start_in_first` in the first file to `end_in_last` in the last. Every file must carry the same
// stream - codec, size, pixel format, frame rate, sample rate, channel layout and time bases - and that is
// checked before the output is created: a chunk from before a settings change is refused, not joined, unless
// `dropped_head` is given: the join then copies the run of files that share the last file's layout, from the beginning of
// the first of them, and reports in `*dropped_head` how many files before that run it left out (0: all of them match).
bool media_concat(const std::vector<std::string>& files, const std::string& output_path, double start_in_first,
                  double end_in_last, std::string& error, double* first_keyframe_seconds = nullptr,
                  size_t* dropped_head = nullptr);

// What a Timeline save asks a child process to join, so the engine's own media lock stays free while the
// copy runs. `start_in_first_seconds` and `end_in_last_seconds` are local to the first and last file.
struct MediaJoinRequest {
  std::vector<std::string> files;
  std::string output;
  double start_in_first_seconds = 0.0;
  double end_in_last_seconds = 0.0;
};

// The file a `--media-join` child writes its outcome to, beside the video it made: `<output>.join.json`.
// One helper, so the child that writes it and the parent that reads it cannot disagree.
inline std::string media_join_result_path(const std::string& output) { return output + ".join.json"; }

// The spec the join child is started with, beside the output it names: `<output>.join-spec.json`. Named here for the
// same reason as the result: the save that claims the staging name claims these sidecars with it.
inline std::string media_join_spec_path(const std::string& output) { return output + ".join-spec.json"; }

// Fills `request` from a parsed `--media-join` spec. False with a sentence when the spec cannot describe a
// join: no files, no output, or an end that does not follow the start. Inline here because media.cpp links
// FFmpeg and the test target does not.
inline bool parse_media_join_request(const Json& spec, MediaJoinRequest& request, std::string& error) {
  request = MediaJoinRequest();
  const Json& files = spec["files"];
  if (!files.is_array()) {
    error = "files must be an array of paths";
    return false;
  }
  for (const Json& entry : files.items()) {
    if (entry.as_string().empty()) {
      error = "files must hold paths";
      return false;
    }
    request.files.push_back(entry.as_string());
  }
  if (request.files.empty()) {
    error = "files is empty";
    return false;
  }
  request.output = spec["output"].as_string();
  if (request.output.empty()) {
    error = "output is required";
    return false;
  }
  request.start_in_first_seconds = spec["start_in_first"].as_double(0.0);
  request.end_in_last_seconds = spec["end_in_last"].as_double(request.start_in_first_seconds);
  // Both cuts are offsets inside their own file, so they are only comparable when one file holds the whole
  // range. Comparing them across a range of files refused every Timeline save whose first chunk gave up more
  // than the last one did - a 20 s range ending a few hundred ms into its last chunk, for one, with the
  // engine reporting that the join "did not report a result".
  const bool one_file = request.files.size() == 1;
  if ((one_file && !(request.end_in_last_seconds > request.start_in_first_seconds)) ||
      request.start_in_first_seconds < 0.0 || request.end_in_last_seconds < 0.0) {
    error = "end_in_last must be after start_in_first";
    return false;
  }
  return true;
}

// The child behind `--media-join <spec.json>`: reads the spec, joins the files with media_concat, and
// writes the result beside the output as `<output>.join.json`. 0 when the join was written, 3 when it was
// not; a failed join writes no video.
int run_media_join(const std::string& spec_path);

// What a join child reported, read back from `media_join_result_path`.
struct MediaJoinResult {
  bool ok = false;
  std::string error;
  double first_keyframe_seconds = -1.0;
  size_t dropped_head = 0;
};

// The command line a join child is started as, by a test that must exercise the deadline without a real
// copy. Empty means this engine: `"<this exe>" --media-join "<spec>"`. Test builds only.
#ifdef BUFFRX_TEST_HOOKS
inline std::string& media_join_child_override() {
  static std::string command;
  return command;
}
#endif

// A UTF-8 path as the wide string a Windows call takes, with no terminating NUL of its own: one left in
// ended the child's command line at the first quote, so the child started as an ordinary engine.
inline std::wstring media_join_wide(const std::string& text) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(length > 0 ? length : 0), L'\0');
  if (length > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), length);
  if (!out.empty()) out.pop_back();  // the NUL MultiByteToWideChar wrote is the terminator, not a character
  return out;
}

// The command line a join child is started as: the exe quoted, the flag, the spec path quoted. Kept here
// so it can be checked without starting a process: a wide string that still carried its terminating NUL
// ended the child's command line at the first quote, and the child then started as an ordinary engine.
inline std::wstring media_join_command(const std::wstring& executable, const std::wstring& spec_path) {
  return L"\"" + executable + L"\" --media-join \"" + spec_path + L"\"";
}

// Runs a join in a child process (`buffrx-engine.exe --media-join <spec>`), so the engine's own media
// lock stays free while the copy runs and listings, thumbnails and other saves keep answering. The spec
// and the result are written beside the output and deleted here whatever the outcome. `timeout_ms` is a
// copy deadline for the whole child - engine start plus the join - not the old 180 s wait for the media
// lock: it is long enough for a two-hour range on a slow disk, and a child that outlives it is stopped
// with nothing it wrote left behind.
// Inline here because media.cpp links FFmpeg and the test target does not; the child does the media work.
inline bool media_join_process(const MediaJoinRequest& request, MediaJoinResult& result, std::string& error,
                               unsigned timeout_ms = 1800000u) {
  result = MediaJoinResult();
  const std::string result_path = media_join_result_path(request.output);
  const std::string spec_path = media_join_spec_path(request.output);
  DeleteFileA(result_path.c_str());  // an outcome from an earlier attempt is not this one's

  Json spec = Json::object();
  Json files = Json::array();
  for (const std::string& file : request.files) files.push(Json(file));
  spec.set("files", std::move(files));
  spec.set("output", Json(request.output));
  spec.set("start_in_first", Json(request.start_in_first_seconds));
  spec.set("end_in_last", Json(request.end_in_last_seconds));
  const std::string dumped = spec.dump();
  FILE* spec_file = _wfopen(media_join_wide(spec_path).c_str(), L"wb");
  if (!spec_file) {
    error = "the join spec could not be written";
    return false;
  }
  std::fwrite(dumped.data(), 1, dumped.size(), spec_file);
  std::fclose(spec_file);

#ifdef BUFFRX_TEST_HOOKS
  const std::string& override = media_join_child_override();
  std::wstring command = override.empty()
                             ? media_join_command(media_join_wide(executable_path()), media_join_wide(spec_path))
                             : media_join_wide(override);
#else
  std::wstring command = media_join_command(media_join_wide(executable_path()), media_join_wide(spec_path));
#endif
  // The child's own JSON line is not the answer (the result file is), and it must not land on a console
  // the person is reading.
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE sink = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                            0, nullptr);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = sink;
  startup.hStdError = sink;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION info{};
  const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup,
                                      &info);
  if (sink != INVALID_HANDLE_VALUE) CloseHandle(sink);
  if (!started) {
    DeleteFileW(media_join_wide(spec_path).c_str());
    error = "the join could not be started";
    return false;
  }
  CloseHandle(info.hThread);

  // A join outliving its engine is a copy nobody is waiting for: the job ends the child with this process.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, info.hProcess)) {
      CloseHandle(job);
      job = nullptr;
    }
  }

  const DWORD waited = WaitForSingleObject(info.hProcess, timeout_ms);
  const bool stopped = waited != WAIT_OBJECT_0;
  if (stopped) {
    TerminateProcess(info.hProcess, 1);
    WaitForSingleObject(info.hProcess, 5000);
  }
  // What the child said when it left without a result: 2 is a command line it could not read, 3 a join it
  // refused, anything else is a crash. Without it "no result" is the whole answer and a save that fails has
  // no cause.
  DWORD child_exit = 0;
  GetExitCodeProcess(info.hProcess, &child_exit);
  CloseHandle(info.hProcess);
  if (job) CloseHandle(job);

  // Whatever the child left: a spec, and an output a stopped child did not clean up itself.
  DeleteFileW(media_join_wide(spec_path).c_str());
  if (stopped || GetFileAttributesA(result_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    DeleteFileA(request.output.c_str());
    DeleteFileA((request.output + ".part").c_str());
    DeleteFileA(result_path.c_str());
    if (stopped) {
      error = waited == WAIT_TIMEOUT ? "the join took longer than it may and was stopped" : "the join stopped early";
    } else {
      // A spec the child could not read writes no result at all, so a missing file is a failed join. The
      // child's exit code names which: 2 the command line, 3 a join it refused, anything else a crash.
      error = "the join did not report a result (the join process exited with " + std::to_string(child_exit) + ")";
    }
    return false;
  }

  std::string text;
  if (FILE* file = _wfopen(media_join_wide(result_path).c_str(), L"rb")) {
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
  }
  DeleteFileA(result_path.c_str());
  Json reply;
  std::string parse_error;
  if (text.empty() || !Json::parse(text, reply, parse_error)) {
    DeleteFileA(request.output.c_str());
    DeleteFileA((request.output + ".part").c_str());
    error = "the join did not report a result";
    return false;
  }
  result.ok = reply["ok"].as_bool(false);
  result.error = reply["error"].as_string();
  result.first_keyframe_seconds = reply["first_keyframe"].as_double(-1.0);
  const int64_t dropped = reply["dropped_head"].as_int(0);
  result.dropped_head = dropped > 0 ? static_cast<size_t>(dropped) : 0;
  if (!result.ok) {
    DeleteFileA(request.output.c_str());
    DeleteFileA((request.output + ".part").c_str());
    error = result.error.empty() ? "the join failed" : result.error;
    return false;
  }
  return true;
}

// One packet as packet_hash.h hashes it: 'v' (the first video stream; `vcl_bytes` and `hash` cover its picture data) or 'a' (an audio stream, its ordinal in
// `stream`; `hash` covers the whole payload).
struct PacketLine {
  char kind = 'v';
  int stream = 0;
  bool key = false;
  uint32_t size = 0;
  uint32_t vcl_bytes = 0;
  uint64_t hash = 0;
};
struct PacketReport {
  bool ok = false;
  std::string reason;
  std::string signature;  // the stream signature a join compares, as text
  std::vector<PacketLine> lines;
};
// Every packet of a file, in file order. The probe of the Timeline dedupe spike; not used by the product.
PacketReport media_packet_report(const std::string& path);

// What the Timeline dedupe needs to know about a closed chunk (docs/review-timeline-mux-2026-11-02.md): its video packets counted, and for every keyframe the
// packet index, the time since the first video packet (decode time, microseconds, the ring's clock), the picture hash (packet_hash.h) and where it is in the file
// in seconds (what media_concat's `start_in_first` takes). Takes the media lock with the interactive deadline, so it is never run on the tick or under the engine
// lock; "the media work is busy" in `reason` means try later.
struct ChunkKeyFact {
  uint32_t index = 0;
  int64_t dts_us = 0;
  double file_seconds = 0;
  uint64_t hash = 0;
  uint32_t vcl_bytes = 0;
};
struct ChunkReport {
  bool ok = false;
  bool busy = false;
  std::string reason;
  uint32_t video_count = 0;
  double duration_seconds = 0;
  std::vector<ChunkKeyFact> keys;
};
ChunkReport media_chunk_probe(const std::string& path);

}  // namespace bx

