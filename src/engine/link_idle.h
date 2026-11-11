// When the sharing helper is stopped for being idle, decided without a process so it can be tested.
//
// The clock must be reset whenever a helper starts or stops. It was not: after the first idle stop `since` kept its old
// value, a new helper (idle for its first second, nothing registered yet) was stopped by the very next tick, and the
// next share or receive failed (docs/share-links.md, Known failures 1).

#ifndef BUFFRX_LINK_IDLE_H
#define BUFFRX_LINK_IDLE_H

#include <cstdint>

namespace bx {

constexpr uint64_t kLinkIdleStopMs = 30000;

struct LinkIdleClock {
  uint64_t since = 0;

  // A helper started or stopped: its idle time starts from nothing.
  void reset() { since = 0; }

  // One tick. True when a running helper has been idle long enough to stop.
  bool should_stop(bool busy, bool helper_running, uint64_t now) {
    if (busy) {
      since = 0;
      return false;
    }
    if (!helper_running) return false;
    if (since == 0) since = now;
    return now - since > kLinkIdleStopMs;
  }
};

}  // namespace bx

#endif
