// The native interface's present recovery, checked on a simulated clock: a deferred Present is retried without new input, a covered
// window keeps a bounded number of attempts and then re-arms once a second for a minute, a failing window holds its ordinary causes
// until its retry, a hidden or minimized window keeps none, a device rebuild is bounded, and the test faults parse and replace the
// right returns. Pure: no window, no GPU, no libobs. Built as buffrx-ui-present-tests (see CMakeLists.txt) and direct-compiled for a
// focused run. The frame and render paths that use these rules are exercised by tools/ui-idle-probe.ps1 on a private engine.
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "../src/engine/ui/ui_present_math.h"

namespace {

using namespace bx::ui;

int failures = 0;

void check(bool condition, const char* name) {
  std::printf("  %s %s\n", condition ? "ok  " : "FAIL", name);
  if (!condition) ++failures;
}

constexpr uint32_t kOk = 0;
constexpr uint32_t kStillDrawing = 0x887A000Au;      // DXGI_ERROR_WAS_STILL_DRAWING
constexpr uint32_t kOccluded = 0x087A0001u;          // DXGI_STATUS_OCCLUDED (a success code)
constexpr uint32_t kRemoved = 0x887A0005u;           // DXGI_ERROR_DEVICE_REMOVED
constexpr uint32_t kUnknownFailure = 0x80004005u;    // E_FAIL
constexpr int64_t kSpanUs = 300LL * 1000000LL;       // five minutes: far past the last re-arm

// Runs the retry state against a simulated clock, one millisecond at a time, the way frame() drives it: an attempt is made whenever one
// is due and the window is on screen. `script` gives the outcome of each attempt in order (the last one repeats).
int run_clock(PresentRetry& retry, int64_t& now_us, int64_t span_us, const std::vector<PresentOutcome>& script, int64_t& last_us,
              bool on_screen = true) {
  int attempts = 0;
  const int64_t end = now_us + span_us;
  for (; now_us <= end; now_us += 1000) {
    retry = retry_while_shown(retry, on_screen);
    if (!retry_due(retry, now_us, on_screen)) continue;
    const PresentOutcome outcome = script[static_cast<size_t>(attempts < static_cast<int>(script.size()) ? attempts : static_cast<int>(script.size()) - 1)];
    ++attempts;
    last_us = now_us;
    retry = next_retry(retry, outcome, now_us);
  }
  return attempts;
}

// One episode in which every attempt fails with `outcome`, the first made at `start_us`. The frame pass makes an attempt only when the
// retry is due. `ordinary` counts the times an ordinary cause (new data, a moving value) would have drawn while the window is held:
// a failing window must not draw for those, so the count stays 0.
struct Episode {
  int attempts = 0;          // every attempt made, the first included
  int spent = 0;             // how often the episode reported that its fast attempts were spent
  int64_t first_gap_us = 0;  // from the first attempt to the second
  int64_t min_gap_us = std::numeric_limits<int64_t>::max();
  int64_t max_gap_us = 0;
  bool gaps_grow = true;     // the gaps between attempts never shrink
  int ungated = 0;           // ordinary causes that would have drawn while a retry was pending or spent
  PresentRetry end_state;    // the state after the span
};

Episode run_episode(PresentOutcome outcome, int64_t start_us, int64_t span_us, bool ordinary) {
  Episode e;
  PresentRetry retry = next_retry(PresentRetry{}, outcome, start_us);
  e.attempts = 1;
  int64_t last = start_us;
  int64_t previous_gap = 0;
  for (int64_t now = start_us + 1000; now <= start_us + span_us; now += 1000) {
    if (!retry_due(retry, now, true)) {
      if (ordinary && !retry_holds(retry, false, false)) ++e.ungated;
      continue;
    }
    const int64_t gap = now - last;
    if (e.attempts == 1) e.first_gap_us = gap;
    if (gap < e.min_gap_us) e.min_gap_us = gap;
    if (gap > e.max_gap_us) e.max_gap_us = gap;
    if (previous_gap != 0 && gap < previous_gap) e.gaps_grow = false;
    previous_gap = gap;
    last = now;
    ++e.attempts;
    retry = next_retry(retry, outcome, now);
    if (retry_gave_up_now(retry)) ++e.spent;
  }
  e.end_state = retry;
  return e;
}

}  // namespace

int main() {
  std::printf("ui present recovery\n");

  // ---------------------------------------------------------------- classification of what Present returns
  check(classify_present(kOk) == PresentOutcome::shown, "S_OK is a shown frame");
  check(classify_present(kStillDrawing) == PresentOutcome::busy, "WAS_STILL_DRAWING is busy, not shown");
  check(classify_present(kOccluded) == PresentOutcome::occluded, "DXGI_STATUS_OCCLUDED is occluded, although it is a success code");
  check(classify_present(kRemoved) == PresentOutcome::lost, "DEVICE_REMOVED is a lost device");
  check(classify_present(kUnknownFailure) == PresentOutcome::failed, "an unknown failure is failed");

  // ---------------------------------------------------------------- deferred, then success, with no more input
  {
    int64_t now = 1000000, last = 0;
    PresentRetry retry = next_retry(PresentRetry{}, classify_present(kStillDrawing), now);
    check(retry_pending(retry) && retry.due_us == now + kBusyRetryUs, "one busy present schedules a single transient retry");
    check(!retry_due(retry, now, true), "the retry is not due before its deadline");
    const int attempts = run_clock(retry, now, 2 * kBusyRetryUs, {PresentOutcome::shown}, last);
    check(attempts == 1 && last == 1000000 + kBusyRetryUs, "the retry fires at its deadline with no input and then stops");
    check(!retry_pending(retry) && !retry.gave_up, "after the retry succeeds nothing is pending");
  }

  // ---------------------------------------------------------------- a busy display that keeps refusing: bounded fast, then one a second
  {
    const Episode e = run_episode(PresentOutcome::busy, 1000000, kSpanUs, false);
    check(e.attempts == kPresentMisses + kPresentRearms + 1, "a display that never takes the frame gets the fast attempts, then the slow re-arms, then no more");
    check(e.spent == 1, "the spend is reported once per episode (the transition that asks for a device rebuild)");
    check(e.first_gap_us == kBusyRetryUs && e.min_gap_us == kBusyRetryUs, "fast busy attempts are 60 ms apart");
    check(e.max_gap_us == kBackoffMaxUs && e.gaps_grow, "the slow re-arms are one second apart, never sooner than the fast ones end");
    check(!retry_pending(e.end_state) && e.end_state.gave_up, "after the re-arms nothing is pending, and a deliberate input is what draws again");
  }
  {
    // The first thirteen attempts are the fast burst; the thirteenth spends the episode and schedules the first re-arm.
    const Episode burst = run_episode(PresentOutcome::busy, 1000000, 12 * kBusyRetryUs, false);
    check(burst.attempts == 13 && burst.spent == 1 && retry_pending(burst.end_state), "the spend comes at the thirteenth attempt, with one re-arm pending");
  }

  // ---------------------------------------------------------------- ordinary causes never add attempts while a retry is owed
  {
    const Episode plain = run_episode(PresentOutcome::busy, 1000000, kSpanUs, false);
    const Episode moving = run_episode(PresentOutcome::busy, 1000000, kSpanUs, true);
    check(moving.ungated == 0, "a window with a moving value or new data draws for none of its pending retry's gap");
    check(moving.attempts == plain.attempts && moving.min_gap_us == plain.min_gap_us, "the same attempts with or without ordinary causes");
  }

  // ---------------------------------------------------------------- occlusion: backoff >= 250 ms, doubling, capped at 1 s, then one a second
  {
    const Episode e = run_episode(PresentOutcome::occluded, 5000000, kSpanUs, true);
    check(e.first_gap_us == kOccludedRetryUs, "occluded retries are never sooner than 250 ms (the first)");
    check(e.gaps_grow, "occluded retries back off, they do not repeat at the first gap");
    check(e.max_gap_us == kBackoffMaxUs, "occluded retries stop at the 1 s cap");
    check(e.attempts == kPresentMisses + kPresentRearms + 1, "a covered window draws the bounded number of passes, then waits");
    check(e.ungated == 0, "a covered window with moving content draws at its own deadlines only");
    check(!escalates(PresentOutcome::occluded), "occlusion never escalates to a device rebuild");
    check(e.spent == 1, "occlusion reports its spend once");
  }

  // ---------------------------------------------------------------- hidden or minimized: no attempts, no hint, no memory
  {
    int64_t now = 9000000, last = 0;
    PresentRetry retry = next_retry(PresentRetry{}, classify_present(kStillDrawing), now);
    const int attempts = run_clock(retry, now, 10 * 1000000LL, {PresentOutcome::shown}, last, /*on_screen=*/false);
    check(attempts == 0, "a hidden or minimized window makes no present attempts");
    check(!retry_pending(retry_while_shown(retry, false)), "leaving the screen clears a pending retry");
    PresentRetry cleared = retry_while_shown(PresentRetry{}, false);
    check(cleared.misses == 0 && !retry_pending(cleared), "a restored window does not inherit the old backoff");
  }

  // ---------------------------------------------------------------- a due retry the display did not take is rescheduled, not counted
  {
    const int64_t now = 2000000;
    PresentRetry blocked;
    blocked.due_us = now - 1000;
    blocked.misses = 1;
    check(retry_wait_ms(blocked, now) >= 4, "a due retry waits at least 4 ms, so a blocked display cannot spin the loop");
    const PresentRetry pending = next_retry(PresentRetry{}, classify_present(kStillDrawing), now);
    check(retry_wait_ms(pending, now) <= kBusyRetryUs / 1000 + 1, "a pending busy retry asks the loop for no more than its own deadline");
    const PresentRetry later = retry_later(blocked, now);
    check(later.misses == blocked.misses && !later.gave_up, "a retry held back for the display counts no attempt");
    check(later.due_us == now + kBusyRetryUs && retry_wait_ms(later, now) <= kBusyRetryUs / 1000 + 1, "and asks again on the busy deadline");
    check(retry_wait_ms(PresentRetry{}, now) == 0 && retry_wait_ms(retry_spent(), now) == 0, "nothing pending asks the loop for nothing");
  }

  // ---------------------------------------------------------------- a target that cannot be made: bounded, one escalation per episode
  {
    const Episode e = run_episode(PresentOutcome::no_target, 3000000, kSpanUs, false);
    check(e.attempts == kPresentMisses + kPresentRearms + 1, "a target that stays missing is retried exactly the bounded number of times");
    check(e.spent == 1 && escalates(PresentOutcome::no_target), "a spent missing-target episode asks for one device rebuild");
    check(!escalates(PresentOutcome::busy) && !escalates(PresentOutcome::occluded), "busy and occluded never ask for a rebuild");
    check(may_rebuild(0) && may_rebuild(kResourceRebuilds - 1), "device rebuilds are allowed up to the cap");
    check(!may_rebuild(kResourceRebuilds), "device rebuilds stop at the cap, so a broken device cannot loop forever");
  }

  // ---------------------------------------------------------------- a target that fails once, then comes back: no rebuild
  {
    int64_t now = 4000000, last = 0;
    PresentRetry retry = next_retry(PresentRetry{}, PresentOutcome::no_target, now);
    const int attempts = run_clock(retry, now, 2 * 1000000LL, {PresentOutcome::shown}, last);
    check(attempts == 1, "a missing target that returns on the retry is drawn once more and nothing else");
    check(!retry.gave_up && !retry_pending(retry), "and it settles with no escalation");
  }

  // ---------------------------------------------------------------- a lost device: the device rebuild replaces the retry
  {
    int64_t now = 6000000;
    PresentRetry retry = next_retry(PresentRetry{}, classify_present(kStillDrawing), now);
    retry = next_retry(retry, classify_present(kRemoved), now);
    check(!retry_pending(retry), "a lost device leaves no retry: the device rebuild is the recovery");
    check(!escalates(PresentOutcome::lost), "a lost device is not counted as a target escalation");
  }

  // ---------------------------------------------------------------- a device that stays lost: bounded rebuilds, then input only
  {
    int rebuilds = 0, device_rebuilds = 0, refusals = 0;
    PresentRetry retry;
    for (int present = 0; present < 50; ++present) {  // every present on the rebuilt device is lost again
      retry = next_retry(retry, PresentOutcome::lost, 0);
      if (may_rebuild(rebuilds)) {
        ++rebuilds;
        ++device_rebuilds;
        retry = PresentRetry{};  // the rebuild replaces the window, and its retry with it
      } else {
        ++refusals;
        retry = retry_spent();   // the window is held: no more rebuilds, no more attempts
      }
    }
    check(device_rebuilds == kResourceRebuilds, "a device that fails its first present every time is rebuilt exactly the cap, then no more");
    check(refusals == 50 - kResourceRebuilds && retry.gave_up && !retry_pending(retry), "the rest are refused, with the window held");
    rebuilds = grant_rebuild(rebuilds);
    check(may_rebuild(rebuilds) && !may_rebuild(rebuilds + 1), "a deliberate input is granted one rebuild, and no more than that");
    check(grant_rebuild(0) == 0 && grant_rebuild(kResourceRebuilds - 1) == kResourceRebuilds - 1, "a grant never lowers a budget that has room");
    const int64_t t = 100000000;
    check(may_grant_rebuild(kResourceRebuilds, t, 0), "a spent budget may be granted on the first deliberate input");
    check(!may_grant_rebuild(kResourceRebuilds, t, t - 1000000), "a second grant within 5 s is refused");
    check(may_grant_rebuild(kResourceRebuilds, t, t - kInputRebuildGapUs), "a grant is allowed again once 5 s have passed");
    check(!may_grant_rebuild(kResourceRebuilds - 1, t, 0), "no grant is needed while the budget has room");
  }
  {
    // A held key repeats its input some thirty times a second for twelve seconds on a device that never recovers: each grant is one
    // device rebuild, and the grants follow the 5 s gap, not the events (three in all, not 360).
    int rebuilds = kResourceRebuilds;
    int64_t last_grant = 0;
    int grants = 0;
    const int64_t start = 1000000000;
    for (int64_t t = start; t < start + 12 * 1000000LL; t += 33333) {
      if (may_grant_rebuild(rebuilds, t, last_grant)) {
        rebuilds = grant_rebuild(rebuilds);
        last_grant = t;
        ++grants;
        rebuilds = kResourceRebuilds;  // the granted rebuild fails again: the budget is spent once more
      }
    }
    check(grants == 3, "a held key's repeats grant at most one rebuild per 5 s");
  }
  {
    // Transient losses: each one is rebuilt, and the frame that shows afterwards starts the run over, so the next loss is rebuilt too.
    int rebuilds = 0;
    bool every_loss_rebuilt = true;
    for (int episode = 0; episode < 5; ++episode) {
      if (may_rebuild(rebuilds)) ++rebuilds;
      else every_loss_rebuilt = false;
      rebuilds = 0;  // the rebuilt device shows its frame
    }
    check(every_loss_rebuilt, "a rebuild that succeeds clears the failure state, so every later loss is rebuilt");
  }

  // ---------------------------------------------------------------- what holds ordinary causes, and what draws at once
  {
    const PresentRetry pending = next_retry(PresentRetry{}, PresentOutcome::busy, 1000000);
    check(retry_holds(pending, false, false), "a pending retry holds the ordinary causes until its deadline");
    check(!retry_holds(pending, true, false), "when the retry is due, it is the draw");
    check(!retry_holds(pending, false, true), "deliberate input draws at once, even while a retry is pending");
    check(!retry_holds(PresentRetry{}, false, false), "a window that did not fail is never held");
    check(retry_holds(retry_spent(), false, false), "a spent window holds the ordinary causes");
    check(!retry_holds(retry_spent(), false, true), "a deliberate input draws a spent window");
    const PresentRetry renewed = next_retry(PresentRetry{}, PresentOutcome::busy, 2000000);
    check(renewed.misses == 1 && renewed.due_us == 2000000 + kBusyRetryUs, "a deliberate input starts a fresh fast episode");
  }

  // ---------------------------------------------------------------- the test faults: parsing, which attempts, what they return
  {
    check(parse_present_fault(nullptr) == PresentFault::none, "no BUFFRX_UI_TEST_PRESENT means no fault");
    check(parse_present_fault("busy-once") == PresentFault::busy_once && parse_present_fault("busy") == PresentFault::busy, "the busy faults parse");
    check(parse_present_fault("occluded") == PresentFault::occluded, "the occluded fault parses");
    check(parse_present_fault("lost-once") == PresentFault::lost_once && parse_present_fault("lost") == PresentFault::lost, "the lost faults parse");
    check(parse_present_fault("bogus") == PresentFault::none && parse_present_fault("") == PresentFault::none, "an unknown value is no fault");
    check(parse_target_fault(nullptr) == TargetFault::none && parse_target_fault("once") == TargetFault::once, "the target faults parse");
    check(parse_target_fault("persistent") == TargetFault::persistent && parse_target_fault("always") == TargetFault::none, "and an unknown target is no fault");
    check(present_fault_applies(PresentFault::busy_once, 1) && !present_fault_applies(PresentFault::busy_once, 2), "busy-once replaces the first attempt only");
    check(present_fault_applies(PresentFault::lost, 1) && present_fault_applies(PresentFault::lost, 40), "lost replaces every attempt");
    check(!present_fault_applies(PresentFault::lost_once, 2) && !present_fault_applies(PresentFault::none, 1), "lost-once and none replace nothing after the first");
    check(classify_present(present_fault_hresult(PresentFault::busy_once)) == PresentOutcome::busy, "the busy fault returns the display's busy code");
    check(classify_present(present_fault_hresult(PresentFault::busy)) == PresentOutcome::busy, "every busy fault returns busy");
    check(classify_present(present_fault_hresult(PresentFault::occluded)) == PresentOutcome::occluded, "the occluded fault returns the occluded status");
    check(classify_present(present_fault_hresult(PresentFault::lost_once)) == PresentOutcome::lost, "the lost fault returns device removed");
    check(target_fault_applies(TargetFault::once, 1) && !target_fault_applies(TargetFault::once, 2), "target once fails the first target only");
    check(target_fault_applies(TargetFault::persistent, 9) && !target_fault_applies(TargetFault::none, 1), "target persistent fails every target; none fails none");
  }

  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
