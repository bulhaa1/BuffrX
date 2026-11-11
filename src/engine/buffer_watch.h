// What the replay-buffer watchdog should do, decided without libobs, so the rule can be tested.
//
// The bug this exists for: the user asked for the buffer to stop, and it came back. `stop_buffer()`
// clears the intent, but the output can still report itself active for a moment while it winds down.
// The old code read that as "it is running" and set the running flag, which re-armed the stall
// detector; three seconds later the detector saw a frozen frame counter, declared a stall and
// restarted the ring. Every restart empties the buffer, so a 120 s save produced seconds, and a stop
// did not stick. The rule here is that an explicit stop is authoritative: while the user wants no
// buffer, the watchdog must keep stopping it and must never call it running.

#ifndef BUFFRX_BUFFER_WATCH_H
#define BUFFRX_BUFFER_WATCH_H

#include <cstdint>

namespace buffrx {

// How many recording outputs the engine will abandon before it stops starting new ones. An output
// libobs never stops cannot be released - releasing it blocks forever - so what leaks is its encoders
// and its GPU surfaces. One stall is a bad moment; a few mean a wedged encode thread, and starting
// another recording on top of that only leaks another one. Small and finite on purpose.
constexpr uint64_t kMaxAbandonedOutputs = 3;

bool abandoned_outputs_exhausted(uint64_t abandoned_count);

enum class WatchdogAction {
  none,   // nothing to do this tick
  stop,   // the output is still up but the user asked for no buffer: finish stopping it
  start,  // the user wants the buffer and it is not running: start it
  restart,// the buffer is running but its frame counter froze: restart it
};

// One tick of the watchdog. `stall_ticks_over` is the stall counter already past its limit (the
// caller owns the counting and the restart cooldown, both of which are time, not policy).
WatchdogAction watchdog_action(bool intent, bool active, bool stall_ticks_over);

// What the interface shows: "on" while the ring runs, "starting" while it is wanted but not running
// yet (the source has no frame, a game has not hooked), "stopping" while a stop is unwinding, "off".
// The user's intent wins over a lingering active flag, so a stop reads as "stopping" at once.
const char* buffer_state_name(bool intent, bool active, bool stopping);

}  // namespace buffrx

#endif  // BUFFRX_BUFFER_WATCH_H