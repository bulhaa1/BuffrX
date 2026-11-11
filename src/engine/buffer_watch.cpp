#include "buffer_watch.h"

namespace buffrx {

bool abandoned_outputs_exhausted(uint64_t abandoned_count) {
  // Past the ceiling the engine stops starting recordings rather than leaking another encoder and
  // another set of GPU surfaces onto a wedged encode thread. The buffer is untouched: only the
  // recording output is involved, and clipping keeps working.
  return abandoned_count >= kMaxAbandonedOutputs;
}

WatchdogAction watchdog_action(bool intent, bool active, bool stall_ticks_over) {
  // The user's decision comes first and is never second-guessed. A stop that has not finished
  // unwinding is still a stop: reporting the output as running here is what let the stall detector
  // restart a buffer the user had just stopped.
  if (!intent) return active ? WatchdogAction::stop : WatchdogAction::none;
  if (!active) return WatchdogAction::start;
  return stall_ticks_over ? WatchdogAction::restart : WatchdogAction::none;
}

const char* buffer_state_name(bool intent, bool active, bool stopping) {
  if (!intent) return (stopping || active) ? "stopping" : "off";
  return active ? "on" : "starting";
}

}  // namespace buffrx