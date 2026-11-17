// The session watcher's grace rule and snapshot contract, without Windows audio. The live test
// (tests/audio_watch_live_tests.cpp) is what proves the events themselves.
#include <cstdint>
#include <cstdio>

#include "../src/engine/audio_watch.h"

namespace {

void expect(int& failures, bool ok, const char* what) {
  if (ok) return;
  std::printf("  FAIL %s\n", what);
  ++failures;
}

}  // namespace

int test_audio_watch_model() {
  std::printf("audio session watcher model\n");
  int failures = 0;

  // An active session is visible no matter how old its last change is.
  expect(failures, bx::watched_session_visible(true, 0, 100000, 1500), "an active session is always visible");

  // An inactive one is visible inside the grace window and hidden after it.
  expect(failures, bx::watched_session_visible(false, 1000, 1000, 1500), "the moment it goes inactive it is visible");
  expect(failures, bx::watched_session_visible(false, 1000, 2000, 1500), "it is visible 1s into a 1.5s grace");
  expect(failures, bx::watched_session_visible(false, 1000, 2500, 1500), "the grace boundary is inclusive");
  expect(failures, !bx::watched_session_visible(false, 1000, 2501, 1500), "one millisecond past the grace it is hidden");
  expect(failures, !bx::watched_session_visible(false, 1000, 60000, 1500), "and stays hidden");

  // A zero grace keeps only active sessions.
  expect(failures, !bx::watched_session_visible(false, 1000, 1001, 0), "zero grace hides an inactive session at once");

  // GetTickCount64 is 64-bit but a clock that stepped back must not hide every session.
  expect(failures, bx::watched_session_visible(false, 5000, 4000, 1500), "a change time in the future stays visible");
  expect(failures, bx::watched_session_visible(false, 0, 0, 0), "an unchanged timestamp is visible");

  expect(failures, !bx::watched_session_remove(false, 1000, false),
         "a long inactive session keeps its event subscription");
  expect(failures, bx::watched_session_remove(false, 0, false), "an expired session is removed");
  expect(failures, bx::watched_session_remove(true, 1000, true), "an exited process is removed");
  expect(failures, bx::watched_repair_due(30000, 30000), "repair runs at its first deadline");
  expect(failures, !bx::watched_repair_due(29999, 30000), "repair waits for its deadline");

  return failures;
}
