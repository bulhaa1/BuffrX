// The arithmetic behind media access, kept away from FFmpeg so it can be tested anywhere: the
// thumbnail box and the part of a file a trim will copy. No libobs, no FFmpeg, no file.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "session_store.h"

namespace bx {

// The thumbnail box: the source fitted inside a square box of the requested side, never upscaled,
// always even. A portrait source is limited by its height, not by its width.
void thumbnail_size(int source_width, int source_height, int target_width, int& out_width, int& out_height);

// The recording chunk length: clamped to 5..600 seconds, and rounded up to a whole number of the
// configured keyframe interval so a chunk boundary can land on a keyframe. The 5 second floor wins
// over alignment: a 3 second request with a 2 second interval gives 5, not 4.
int chunk_seconds_clamped(int requested, int keyframe_seconds);

// The chunks a requested window touches, as the range [first, last] of stored chunk indices. False
// when it touches none. Times are on the session's one clock (see session_store.h).
bool chunk_range_for_window(const std::vector<SessionChunk>& chunks, uint32_t start_ms, uint32_t end_ms,
                            size_t& first, size_t& last);

// A session time window as a stream copy: the chunk files in order, the cut inside the first and the
// end inside the last, in seconds local to those files. False when the window is empty or touches no
// chunk. A chunk's file time 0 is its indexed start_ms, so the cuts are good to about a keyframe.
struct ConcatPlan {
  std::vector<std::string> files;
  double start_in_first = 0.0;
  double end_in_last = 0.0;
};
bool concat_plan_for_range(const std::vector<SessionChunk>& chunks, uint32_t start_ms, uint32_t end_ms,
                           ConcatPlan& out);

// A Timeline range as a clip save: the published chunks that cover it, and where the copy starts inside
// the first and ends inside the last, in milliseconds from those files' own beginnings (a chunk's file
// time 0 is its indexed start_ms, so the cuts are good to about a keyframe).
struct TimelineSavePlan {
  std::vector<std::string> files;
  int64_t start_in_first_ms = 0;
  int64_t end_in_last_ms = 0;
  // Footage at the start of the asked range that the clip leaves out because a settings change (a rebuild of the encoders)
  // came after it: a stream copy cannot join two layouts, so the clip is the part in the layout that holds the range's end.
  int64_t cut_head_ms = 0;
  // Per file: the footage missing between it and the file before it (a pause: the game left, the disk guard, a
  // rebuild). 0 for the first file and for a join with no hole. The copy stitches across it, so the clip is shorter
  // than the range by the sum of these, and the save says so.
  std::vector<int64_t> gap_before_ms;
};
// Footage missing between two chunks for longer than this is a hole the clip skips over.
constexpr int64_t kSaveGapMs = 2000;

// Plans a save from a Timeline session. `available_end_ms` is where the newest published chunk ends, on
// the session's own clock: a range past it names "the newest moment is not saved yet" instead of being
// shortened, because those frames are still being written. A range that spans two epochs (the encoders were
// rebuilt) cannot be stream-copied whole: it plans the chunks of the last epoch in the range and reports the
// start it left out in `cut_head_ms`. An empty `files` carries `error`.
TimelineSavePlan timeline_save_plan(const SessionEntry& entry, int64_t start_ms, int64_t end_ms,
                                    int64_t available_end_ms, std::string& error);

// A frame rate or a time base, numerator over denominator. FFmpeg's AVRational has this shape; it is
// spelled here because this header is compiled by the tests, which link no FFmpeg at all.
struct Rational {
  int num = 0;
  int den = 0;
};

// What every chunk of one join must share for a stream copy: the video's codec, size, pixel format,
// frame rate and time base, and the audio's sample rate and channel layout. A settings change, a governor
// step or Performance mode rebuilding the encoders changes these, and two files that differ cannot be
// joined without re-encoding.
struct StreamSignature {
  int codec_id = 0;
  int width = 0;
  int height = 0;
  int pix_fmt = -1;
  Rational frame_rate;
  int sample_rate = 0;
  uint64_t channel_layout = 0;
  Rational time_base;
};
bool streams_compatible(const StreamSignature& a, const StreamSignature& b);
// Where the run of chunks that share the last chunk's stream begins, as an index (0 when they all do, or there are fewer than
// two). A save across a settings change copies from here.
size_t trailing_compatible_run(const std::vector<StreamSignature>& signatures);

// What the engine does once a recording chunk has closed. Decided on the engine's own thread, never
// inside libobs's "stop" signal: that signal runs on the output's own thread, and releasing the output
// or its encoders from there deadlocks the video pipeline or frees what the encode thread still uses.
enum class ChunkClosedAction { finish_single, rotate, close_session };
ChunkClosedAction chunk_closed_action(bool has_session, bool rotating);

// The sessions a byte budget must delete, oldest first, to bring the total to or under budget_bytes.
// A budget of zero means unlimited. Never returns the newest session or an unclosed one.
std::vector<size_t> retention_prune_order(const std::vector<SessionEntry>& entries, int64_t budget_bytes);

// The part of the file a trim will copy, clamped to what exists. False when nothing would be copied.
bool trim_range(double duration_seconds, double requested_start, double requested_length, double& start_seconds,
                double& length_seconds);

// The tail a clip save keeps. OBS's replay save always writes the whole ring; when that file runs
// longer than the request by more than `slack` seconds, the copy keeps the last `requested` seconds
// and starts at `start` (duration - requested). False when the file is already short enough to keep.
bool save_tail_start(double duration_seconds, double requested_seconds, double slack_seconds, double& start);
// How much longer than the request a saved file may be before it is cut. The ring always holds a little
// more than the length asked for (two seconds and a keyframe), so a request for the whole buffer is never
// cut: re-reading and rewriting every byte of a large file at the moment a game is running, to take two
// seconds off its front, was most of what a save cost. A shorter request is cut as before.
double save_trim_slack(bool whole_buffer);

// One input of a join: a file, the parts of it to keep, and that clip's own edit. A job with more than
// one of these is a stitch; one is the same as the single-input fields above. Each input is re-encoded
// to the common target on its own, so its crop, colour, speed and volumes belong to it alone.
struct ExportInput {
  std::string input;
  double start = 0.0;
  double end = 0.0;
  std::vector<std::pair<double, double>> segments;
  int crop_x = 0, crop_y = 0, crop_width = 0, crop_height = 0;  // source pixels; width 0 means no crop
  double grade_brightness = 100.0, grade_contrast = 100.0, grade_saturation = 100.0;
  double speed = 1.0;
  std::vector<double> volumes;  // per audio track, 1.0 unchanged
};

// One editor export: a cut of one file, optionally cropped, scaled, compressed and re-mixed.
struct ExportJob {
  std::string input;
  std::string output;
  // Replace is ask-first in the editor: the new file is verified, the original is renamed aside, and
  // only then does the new file take its path. The aside file goes to the Recycle Bin.
  bool replace_original = false;
  double start = 0.0;       // seconds into the input
  double end = 0.0;         // seconds; 0 or past the end means "to the end"
  int crop_x = 0, crop_y = 0, crop_width = 0, crop_height = 0;  // source pixels; width 0 means no crop
  int out_width = 0, out_height = 0;  // 0 keeps the (cropped) size
  // Write square pixels: a preview copy of an anamorphic source (1440x1080 with SAR 4:3) must not
  // hand the browser a second stretch.
  bool square_pixels = false;
  // 0 keeps the source quality (a stream copy when nothing else changes); otherwise the share of the
  // source bitrate to aim for: Rewind's compress levels were 0.4, 0.65 and 0.9.
  double quality = 0.0;
  // Colour, as a percentage of the file's own picture: 100 is the file untouched. A grade cannot be a
  // stream copy, so any value other than 100 re-encodes the video.
  double grade_brightness = 100.0;
  double grade_contrast = 100.0;
  double grade_saturation = 100.0;
  // A share fit (share_math.h): the exact video rate to land under an upload limit, held to it rather
  // than allowed to peak above it. 0 leaves the rate to `quality`.
  int video_kbps = 0;
  int audio_kbps = 0;           // the AAC rate per track; 0 is 160 kbit/s
  // Playback speed: 2 plays twice as fast, so the clip is half as long. The bitrate is kept, so the
  // file is smaller by about the same factor. 1 leaves the timing alone.
  double speed = 1.0;
  std::vector<double> volumes;  // per audio track, 1.0 unchanged, 0 drops the track; missing = 1.0
  // The parts of the input to keep, in seconds, joined in order. Empty means the one range
  // start..end. A cut in the editor is a split and a deleted part: what is left is this list.
  std::vector<std::pair<double, double>> segments;
  // A join. More than one entry means every part is re-encoded to a common target and then joined;
  // empty or one entry uses the single-input fields above.
  std::vector<ExportInput> inputs;
  // The common target of a join. 0 leaves it to the first input (size) or the source (rate).
  double fps = 0.0;
  int audio_rate = 0;
  int audio_channels = 0;
  // Force a codec ("h264"), and pin the encoder the first part chose, so every part carries the same
  // sequence headers and the join is a stream copy.
  std::string video_codec;
  std::string video_encoder;
  // Fit the picture inside the target and pad black, instead of stretching it.
  bool fit_pad = false;
};

// The parts an export keeps, made usable: clamped to the file, in order, overlapping or touching parts
// merged, and parts too short to hold a frame dropped. An empty list in the job means start..end.
// `duration` 0 (unknown) leaves the ends unclamped. Empty when nothing is left to export.
std::vector<std::pair<double, double>> export_segments(const ExportJob& job, double duration);

// Whether the job changes the picture's colour at all. Half a percent is the wobble a slider leaves
// behind, not a grade.
bool export_graded(const ExportJob& job);

// How long the export will be: the sum of its parts.
double segments_length(const std::vector<std::pair<double, double>>& segments);

// What an export has to do. A plain cut is a stream copy (no quality loss, seconds); a volume change
// keeps the video stream-copied and re-encodes only the audio; a crop, a scale or a compress
// re-encodes the video, and so does a cut that keeps more than one part: a copy can only start on a
// keyframe, and a join would show the frames before every cut. A single part whose start is not on a
// keyframe is re-encoded too, but that needs the file: media_export decides it.
enum class ExportPlan { copy, audio_only, reencode };
ExportPlan export_plan(const ExportJob& job);

// Whether the job plays the clip at another speed. A stream copy cannot retime, so it re-encodes the
// video; the audio's own rate is the only other thing that has to change.
bool export_retimed(const ExportJob& job);

// The audio half of a retime: `atempo` stages whose product is `speed`, each inside the 0.5..2 range
// every FFmpeg version accepts. Ends with a comma, and is empty at 1x. Clamped to 0.25..4.
std::string atempo_chain(double speed);

// The crop made legal for 4:2:0 video: inside the frame, even sizes and offsets, at least 16x16.
// False (and the full frame) when the requested rectangle is empty or outside the frame.
bool export_crop(int source_width, int source_height, int x, int y, int width, int height, int& out_x, int& out_y,
                 int& out_width, int& out_height);

// The video bitrate an export aims for: the source's bits per pixel kept for the output's pixels,
// times `quality` (1.0 when 0), clamped to 500 kbit/s .. 150 Mbit/s.
int export_video_kbps(int source_kbps, int source_pixels, int output_pixels, double quality);

// True when the export's index goes first in the file. A share fit is played by Discord and by a browser
// before it has all arrived; a local save is read from the folder, so it writes the index once, at the end.
bool export_wants_faststart(const ExportJob& job);

// The longest head an edit list may hide. Past this the keyframe is so far back that the copy is
// thrown away and the range re-encoded: a player that ignores edit lists would otherwise show it.
inline constexpr double kMaxEditHeadSeconds = 10.0;

// How much of a stream copy is footage the user cut away: the cut minus the keyframe the copy has to
// start on. 0 when the cut is within half a frame of that keyframe, NaN when it is further back than
// an edit list may hide.
double edit_head_seconds(double start, double keyframe_seconds, double frame_seconds);

// The video encoders to try for an export of a clip that arrived as `source_codec` ("av1", "hevc",
// "h264", or anything else). An Original save keeps that codec; a share fit (`held`) is H.264 because
// that is what Discord takes. The H.264 chain is always the floor, so a machine with no encoder for
// the clip's own codec still saves.
std::vector<std::string> video_encoder_candidates(const std::string& source_codec, bool held);

// AV1 levels in a packet's sequence header OBUs. OBS's NVENC AV1 declares level 7.3, which libaom (the
// runtime FFmpeg's one software AV1 decoder) treats as undefined: it refuses the whole stream.
// `av1_level` is the first operating point's seq_level_idx (23 is 7.3, 31 unconstrained), -1 when the
// packet has no sequence header or it is malformed. `av1_unconstrain_level` rewrites every 7.x level
// (20-23) to 31 in place - the same number of bits, so nothing else moves - and says whether it did.
// Every OBU and every bit is bounds checked; a malformed packet is left as it is.
int av1_level(const uint8_t* data, size_t size);
bool av1_unconstrain_level(uint8_t* data, size_t size);

// A live level meter's next displayed value: rises instantly to the fresh sample, falls smoothly
// toward it, and never leaves 0..1. `previous` is what is on screen, `next` the fresh sample.
float smooth_level(float previous, float next, double dt_seconds);

// The GPU's own decoder for a codec as `MediaDetails::video_codec` spells it, or "" when the runtime
// has none: a name it has no decoder for keeps the software path rather than a guess.
std::string hardware_decoder_name(const std::string& codec_name);

}  // namespace bx
