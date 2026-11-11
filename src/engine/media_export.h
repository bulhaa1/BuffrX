// The editor's media work: what a file holds, its audio waveforms, and an export.
//
// All of it runs inside a short-lived `buffrx-engine.exe --media-*` process that the editor window
// starts and that exits when the work is done, so an export never shares memory with the engine that
// is buffering, and closing the editor frees everything (AGENTS.md: the editor is decoupled). It uses
// the FFmpeg DLLs the pinned runtime already ships — avformat, avcodec, avfilter — and no second copy.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "media_math.h"

namespace bx {

struct MediaTrack {
  int index = 0;          // the stream index in the file
  int channels = 0;
  int sample_rate = 0;
  std::string title;      // the stream's own title metadata, "" when it has none
};

struct MediaDetails {
  bool ok = false;
  std::string reason;
  double duration = 0.0;
  int width = 0;
  int height = 0;
  double sample_aspect_ratio = 1.0;  // the stream's SAR; 4:3 here is a 16:9 picture at 1440x1080
  double fps = 0.0;
  std::string video_codec;
  int video_kbps = 0;     // the video stream's own rate, else the container's less audio; 0 unknown
  int64_t bytes = 0;
  std::vector<MediaTrack> audio;
};

MediaDetails media_details(const std::string& path);

// Per audio track, `buckets` peak levels (0..1, each track normalised to its own loudest point) across
// the whole file: the editor's waveform lanes. Video is not decoded.
bool media_peaks(const std::string& path, int buckets, std::vector<std::vector<float>>& tracks, std::string& error);

// One audio track of a clip (`track` counts audio streams from 0) copied into an .m4a of its own, so
// the editor's preview can play every track at its own volume: a browser plays only a file's first
// one. A stream copy, on the file's own clock (time zero is the clip's time zero). An `out` newer than
// `path` is kept as it is: the second open of a clip costs nothing.
bool media_audio_track(const std::string& path, int track, const std::string& out, std::string& error);

// How evenly a recording's frames are spread, measured from the file: the probe behind "the video is
// choppy, frames are missing". Pass one reads every video packet's timestamp (no decoding): gaps are
// intervals longer than one and a half frame times, frames_per_second_min is the thinnest full second.
// Pass two decodes the first `decode_seconds` and counts frames that repeat the one before (a capture
// that delivered nothing new): only meaningful on footage that moves.
struct FrameReport {
  bool ok = false;
  std::string reason;
  double duration = 0.0;
  double nominal_fps = 0.0;        // from the median packet interval
  int packets = 0;
  double average_fps = 0.0;
  int frames_per_second_min = 0;   // the thinnest complete second
  int gaps = 0;                    // intervals over 1.5 frame times
  double gap_seconds_total = 0.0;  // time lost to those gaps beyond one frame each
  std::vector<double> worst_gaps_ms;
  int decoded = 0;
  int repeats = 0;                 // decoded frames that repeat the previous one
  std::vector<int> repeat_runs;    // [n] = how many runs of n+1 identical frames in a row
};
FrameReport media_frame_report(const std::string& path, double decode_seconds);

// Which source frames a recording of the probe window (buffrx-present-loop --probe 1) holds. Every decoded
// frame's counter is read from the blocks in its corner; `delta` is how far the counter moved from the
// frame before: 1 is smooth, 0 a repeat (the capture had nothing new), 2 or more skipped source frames.
// Skips are not an error when the source runs faster than the recording - 164 fps into 120 must skip -
// but their spread is: an even recording alternates 1 and 2, a choppy one has runs of 0s and 3s.
struct SequenceReport {
  bool ok = false;
  std::string reason;
  int decoded = 0;
  int unreadable = 0;           // frames whose guard bits were wrong (no probe visible, or a torn frame)
  int repeats = 0;              // delta 0
  int backwards = 0;            // the counter went back: frames out of order
  std::vector<int> delta_histogram;  // [d] frames whose counter moved by d (the last bucket is "or more")
  double source_per_second = 0.0;    // counter progress over the recording's own clock
  double recorded_fps = 0.0;
  double wall_ratio = 0.0;           // recorded seconds per source second: 1 on time, under 1 sped up
  int worst_stall_frames = 0;        // the longest run of repeats
};
SequenceReport media_sequence_report(const std::string& path, double decode_seconds);

// Runs one export (export_plan decides copy / audio-only / re-encode). The file is written beside the
// target as "<output>.part" and renamed only when it is complete and verified, so a cancelled or
// failed export never leaves something that looks like a clip. `progress` gets 0..1. `encoder` names
// the video encoder used ("copy" for a stream copy).
bool media_export(const ExportJob& job, const std::function<void(double)>& progress, std::string& encoder,
                  std::string& error);

}  // namespace bx
