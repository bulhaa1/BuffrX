#include "benchmark.h"

#include <cstdio>
#include <cstdlib>

namespace bx {
namespace {

// Reads "<flag> <value>" from a full argument vector. False when the flag is absent or the value after
// it is not a number, in which case the caller keeps its default.
bool flag_value(const std::vector<std::string>& arguments, const char* flag, int& out) {
  for (size_t index = 0; index + 1 < arguments.size(); ++index) {
    if (arguments[index] != flag) continue;
    const std::string& text = arguments[index + 1];
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return false;
    out = static_cast<int>(value);
    return true;
  }
  return false;
}

}  // namespace

BenchmarkOptions parse_benchmark_options(const std::vector<std::string>& arguments) {
  BenchmarkOptions options;
  int seconds = options.seconds;
  int settle = options.settle_seconds;
  if (flag_value(arguments, "--benchmark-seconds", seconds)) options.seconds = seconds;
  if (flag_value(arguments, "--benchmark-settle", settle)) options.settle_seconds = settle;
  if (options.seconds < 5) options.seconds = 5;
  if (options.settle_seconds < 0) options.settle_seconds = 0;
  // At least five seconds of the run must survive the discard, or the figure describes nothing.
  if (options.settle_seconds > options.seconds - 5) options.settle_seconds = options.seconds - 5;
  return options;
}

BenchmarkResult summarize_benchmark(const BenchmarkSample& start, const BenchmarkSample& end) {
  BenchmarkResult result;
  result.wall_seconds = end.wall_seconds - start.wall_seconds;
  if (result.wall_seconds <= 0.0) return result;

  const double cpu = end.cpu_seconds - start.cpu_seconds;
  result.cpu_seconds = cpu > 0.0 ? cpu : 0.0;
  result.frames = end.frames >= start.frames ? end.frames - start.frames : 0;
  result.dropped = end.dropped >= start.dropped ? end.dropped - start.dropped : 0;
  result.fps = static_cast<double>(result.frames) / result.wall_seconds;
  return result;
}

double machine_cpu_percent(double cpu_seconds, double wall_seconds, int logical_cores) {
  if (cpu_seconds <= 0.0 || wall_seconds <= 0.0 || logical_cores <= 0) return 0.0;
  return cpu_seconds / wall_seconds / static_cast<double>(logical_cores) * 100.0;
}

std::string format_benchmark_line(const BenchmarkResult& result, double working_set_mb, const std::string& encoder,
                                  int settle_seconds, int logical_cores, const VideoStats& video) {
  char buffer[640];
  std::snprintf(buffer, sizeof(buffer),
                "BUFFRX_BENCHMARK seconds=%.1f cpu_seconds=%.2f cores=%d cpu_percent=%.2f working_set_mb=%.1f "
                "frames=%llu dropped=%llu fps=%.1f encoder=%s video_frames=%llu video_lagged=%llu encoder_frames=%llu "
                "render_ms=%.2f settle=%d",
                result.wall_seconds, result.cpu_seconds, logical_cores,
                machine_cpu_percent(result.cpu_seconds, result.wall_seconds, logical_cores), working_set_mb,
                static_cast<unsigned long long>(result.frames), static_cast<unsigned long long>(result.dropped),
                result.fps, encoder.c_str(), static_cast<unsigned long long>(video.video_frames),
                static_cast<unsigned long long>(video.video_lagged),
                static_cast<unsigned long long>(video.encoder_frames), video.render_time_ms, settle_seconds);
  return buffer;
}

}  // namespace bx
