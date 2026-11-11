// Steady-state benchmark arithmetic (docs/measurement.md).
//
// The engine's own CPU cost is only comparable to another tool's published figure when both discard
// their initialisation window, so the first seconds of a run are thrown away before sampling starts.
// Everything here is pure arithmetic on counters: no libobs, no windows, no clock of its own, which
// is what makes it testable off the capture hardware.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bx {

struct BenchmarkOptions {
  int seconds = 20;        // total run length
  int settle_seconds = 5;  // discarded before sampling; at least five seconds are always measured
};

// Reads --benchmark-seconds and --benchmark-settle out of a full argument vector. A missing,
// non-numeric or out-of-range value is clamped or defaulted, never fatal: a benchmark that refuses to
// run is worse than one that runs with the documented defaults.
BenchmarkOptions parse_benchmark_options(const std::vector<std::string>& arguments);

// Process and pipeline counters sampled at the edges of the measurement window.
struct BenchmarkSample {
  double cpu_seconds = 0.0;  // process kernel + user time
  double wall_seconds = 0.0;
  uint64_t frames = 0;   // frames the capture pipeline produced
  uint64_t dropped = 0;  // frames it could not use
};

struct BenchmarkResult {
  double wall_seconds = 0.0;  // the measured window
  double cpu_seconds = 0.0;   // process CPU time consumed inside that window
  double fps = 0.0;
  uint64_t frames = 0;   // produced during the window
  uint64_t dropped = 0;  // dropped during the window
};

// End minus start over the wall time. A counter that went backwards means the output restarted inside
// the window; its delta reads as zero rather than as a negative, and a window of zero length reports
// zero instead of dividing by it.
BenchmarkResult summarize_benchmark(const BenchmarkSample& start, const BenchmarkSample& end);

// CPU time as a share of the whole machine, the way Windows' task manager reports a process. A figure
// per core is many times larger and comparable to no other tool's published cost, so the denominator
// is a parameter here and appears on the summary line.
double machine_cpu_percent(double cpu_seconds, double wall_seconds, int logical_cores);

// Counters from libobs's own video thread and encoder. The output's own bookkeeping says "dropped=0"
// while the video thread is missing deadlines, so the benchmark reports both and lets the difference
// speak.
struct VideoStats {
  uint64_t video_frames = 0;    // frames the video thread rendered
  uint64_t video_lagged = 0;    // frames it missed its deadline on
  uint64_t encoder_frames = 0;  // frames the video encoder actually encoded
  double render_time_ms = 0.0;  // average time to render a frame
};

// The machine-readable summary line documented in docs/measurement.md.
std::string format_benchmark_line(const BenchmarkResult& result, double working_set_mb, const std::string& encoder,
                                  int settle_seconds, int logical_cores, const VideoStats& video);

}  // namespace bx
