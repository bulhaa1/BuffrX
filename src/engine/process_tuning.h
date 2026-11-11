// Candidate optimizations for what the engine costs a running game, switched on one at a time by BUFFRX_TUNE so a
// measurement can say which of them helps (docs/measurement.md). Nothing here changes the picture: they move the
// engine's work to cheaper places - a lower GPU queue priority, the efficiency cores, a background I/O class.
//
//   BUFFRX_TUNE=gpu:-7,eco,below,io
//
// gpu:N   the priority of the engine's D3D11 device in the GPU scheduler, -7 (lowest) to 0 (the default). Lowering it
//         needs no admin; raising it does (libobs's own attempt to do so is the "not admin?" line in its log).
// eco     EcoQoS for the process (the scheduler prefers efficiency cores and lower clocks); only where the CPU has
//         efficiency cores.
// below   the process priority class BELOW_NORMAL.
// io      background I/O and memory priority for the clip trim thread.
//
// Measured on 2026-10-24 (docs/measurement.md): gpu:-7 buys 1% of average FPS and starves the engine (5-12% late frames, a clip
// that is mostly repeated frames), eco and below change nothing on a GPU-bound load. None of them is on by default; the probe is
// how the next candidate is measured.
#ifndef BUFFRX_PROCESS_TUNING_H
#define BUFFRX_PROCESS_TUNING_H

#include <string>

namespace buffrx {

struct TuningSet {
  int gpu_priority = 0;  // -7..0
  bool efficiency_cores = false;
  bool below_normal = false;
  bool background_io = false;
};

TuningSet parse_tuning(const std::string& spec);
std::string tuning_describe(const TuningSet& set);

// The set the engine runs with (parsed once from BUFFRX_TUNE at start); read by the pieces that apply it.
const TuningSet& active_tuning();
void set_active_tuning(const TuningSet& set);

// Win32 effects. Each returns whether the OS accepted it; none throws.
bool cpu_has_efficiency_cores();
bool apply_process_tuning(const TuningSet& set);         // eco and below, once at start
bool apply_gpu_priority(void* d3d11_device, int priority);  // after every video reset: it makes a new device
bool begin_background_io();                              // on the calling thread (the trim worker)

}  // namespace buffrx

#endif  // BUFFRX_PROCESS_TUNING_H
