// The Timeline dedupe's host side (docs/review-timeline-mux-2026-11-02.md): finds the chunks the writer closes, has them probed off every lock, hands the
// probes to the replay ring, and keeps the ring's reliance on a chunk file safe - a pin against retention first, then the file's existence, then the ring may
// take records out of RAM for it. Off unless BUFFRX_DEDUPE=1 (BUFFRX_NO_DEDUPE=1 wins) while it is being proved.
#include <windows.h>

#include <obs.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "../shared/log.h"
#include "dedupe_policy.h"
#include "media.h"
#include "obs_host.h"
#include "ring_output.h"
#include "timeline_pins.h"

namespace bx {

namespace {

// The shared name (timeline_pins.h): an explicit erase ignores this owner, so the two files must agree.
constexpr const char* kRingPinOwner = kTimelineRingLeaseOwner;
constexpr int64_t kPinSeconds = 120;       // renewed every second, so this is how long a stalled tick keeps the footage safe
constexpr uint32_t kPinRenewMs = 1000;

}  // namespace

struct ObsHost::DedupeState {
  struct Job {
    std::string file, session;
    bool follows = false;
    uint64_t generation = 0;
    int64_t wall_start_ms = 0, wall_end_ms = 0;
    int tries = 0;
  };
  struct Result {
    Job job;
    ChunkProbe probe;
    bool ok = false;
  };
  struct Known {
    std::string session;
    int64_t wall_start_ms = 0, wall_end_ms = 0;
  };

  // What the worker reads and writes, held by a reference of its own: a DedupeState destroyed while the worker is
  // inside media_chunk_probe (up to an 8 s wait on the media lock) must leave it with something alive to read.
  struct Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::vector<Result> results;
    bool stopping = false;
    std::atomic<uint64_t> generation{1};
  };

  std::shared_ptr<Shared> shared = std::make_shared<Shared>();
  std::thread worker;

  // Tick-side state (the tick thread only).
  uint32_t ring_tick = 0;
  bool have_previous = false;
  uint32_t previous_end_ms = 0;
  std::map<uint32_t, Known> known;
  uint32_t last_pin_tick = 0;
  bool pinned = false;

  DedupeState() {
    std::shared_ptr<Shared> held = shared;
    worker = std::thread([held] { run(held); });
  }
  // The buffer start that drops this holds the engine lock, and the worker may be waiting on the media lock for
  // seconds: it is not joined here. The thread keeps its own reference to the shared block, sees `stopping` when the
  // probe returns, and lets go of the block itself.
  ~DedupeState() {
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->stopping = true;
    }
    shared->wake.notify_all();
    if (worker.joinable()) worker.detach();
  }

  // Tells the worker to stop without waiting: the caller holds the engine lock and the worker may be inside a probe.
  // The owner keeps this object alive and joins it in dedupe_shutdown.
  void retire() {
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->stopping = true;
    }
    shared->wake.notify_all();
  }

  // The blocking form, for shutdown only: there nothing else can be starved and the process is ending.
  void stop_and_join() {
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->stopping = true;
    }
    shared->wake.notify_all();
    if (worker.joinable()) worker.join();
  }

  void push(Job job) {
    {
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->jobs.push_back(std::move(job));
    }
    shared->wake.notify_one();
  }

  // The worker: one chunk at a time, in the order the writer closed them, so the ring is handed them oldest first.
  static void run(std::shared_ptr<Shared> state) {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->wake.wait(lock, [&] { return state->stopping || !state->jobs.empty(); });
        if (state->stopping) return;
        job = std::move(state->jobs.front());
        state->jobs.pop_front();
      }
      if (job.generation != state->generation.load()) continue;
      const ChunkReport report = media_chunk_probe(job.file);
      if (report.busy && job.tries < 8) {
        // The media lock is held by a trim or a join: this goes again in a moment, first in line, so the order of chunks is kept.
        ++job.tries;
        Sleep(500);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->jobs.push_front(std::move(job));
        continue;
      }
      Result result;
      result.ok = dedupe_probe_from_report(job.file, report, job.follows, result.probe);
      // The session is the host's own knowledge: the store keeps it so a save can guard the read of its chunk files
      // (ring_output.cpp, timeline_retention.h).
      result.probe.session_id = job.session;
      if (!result.ok) BX_LOG_INFO("a timeline chunk is not offered to the replay ring (%s): %s", report.reason.empty() ? "no keyframe to match" : report.reason.c_str(), job.file.c_str());
      result.job = std::move(job);
      std::lock_guard<std::mutex> lock(state->mutex);
      state->results.push_back(std::move(result));
    }
  }

  std::vector<Result> take() {
    std::lock_guard<std::mutex> lock(shared->mutex);
    std::vector<Result> out;
    out.swap(shared->results);
    return out;
  }

  void forget_queued() {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->jobs.clear();
    shared->results.clear();
  }
};

// Asked once, when the ring is made: whether it hashes keyframes for chunks to be matched to. Needs a disk part (otherwise nothing spills) and Timeline on.
bool ObsHost::dedupe_wanted() const {
  if (GetEnvironmentVariableA("BUFFRX_NO_DEDUPE", nullptr, 0) > 0) return false;
  char text[8] = {0};
  if (GetEnvironmentVariableA("BUFFRX_DEDUPE", text, sizeof(text)) == 0 || text[0] != '1') return false;
  return spill_wanted_ && config_.timeline_minutes > 0;
}

// The ring started: chunks written before this moment are not in it, and anything probed for an older ring is dropped.
void ObsHost::dedupe_ring_started() {
  if (!dedupe_wanted()) {
    if (dedupe_) {
      dedupe_->retire();
      retiring_dedupe_.push_back(std::move(dedupe_));
    }
    return;
  }
  if (!dedupe_) dedupe_ = std::make_shared<DedupeState>();
  DedupeState& state = *dedupe_;
  state.shared->generation.fetch_add(1);
  state.forget_queued();
  state.known.clear();
  state.have_previous = false;
  state.pinned = false;
  state.ring_tick = GetTickCount();
  state.last_pin_tick = 0;
}

// Shutdown's blocking form: a worker may be in a chunk probe, and libobs is about to go away under it.
void ObsHost::dedupe_shutdown() {
  for (const auto& state : retiring_dedupe_) state->stop_and_join();
  retiring_dedupe_.clear();
  if (!dedupe_) return;
  dedupe_->stop_and_join();
  dedupe_.reset();
}

// A chunk reached the session index (recording_mutex_ held by the caller): queue it for a probe.
void ObsHost::dedupe_note_chunk(const std::string& session_id, const SessionChunk& chunk) {
  if (!dedupe_ || !buffer_active_) return;
  DedupeState& state = *dedupe_;
  const bool follows = dedupe_chunk_follows(state.have_previous, state.previous_end_ms, chunk.start_ms);
  state.have_previous = true;
  state.previous_end_ms = chunk.end_ms;
  // A chunk that began before the ring did is only partly in it, and one from before a rebuild is not in it at all: neither is matched.
  if (static_cast<int32_t>(chunk.start_ms - (state.ring_tick - 1500)) < 0) return;
  DedupeState::Job job;
  job.file = chunk.file;
  job.session = session_id;
  job.follows = follows;
  job.generation = state.shared->generation.load();
  job.wall_start_ms = chunk.wall_start_ms;
  job.wall_end_ms = chunk.wall_end_ms;
  state.push(std::move(job));
}

void ObsHost::dedupe_tick(uint32_t now) {
  if (!dedupe_ || !replay_ || !obs_output_active(replay_)) return;
  DedupeState& state = *dedupe_;
  proc_handler_t* procs = obs_output_get_proc_handler(replay_);
  for (DedupeState::Result& result : state.take()) {
    if (!result.ok || result.job.generation != state.shared->generation.load()) continue;
    calldata_t call{};
    calldata_init(&call);
    calldata_set_ptr(&call, "probe", &result.probe);
    long long id = 0;
    if (proc_handler_call(procs, "link_chunk", &call) && calldata_get_int(&call, "id", &id) && id > 0) {
      state.known[static_cast<uint32_t>(id)] = DedupeState::Known{result.job.session, result.job.wall_start_ms, result.job.wall_end_ms};
    }
    calldata_free(&call);
  }
  if (now - state.last_pin_tick < kPinRenewMs && state.last_pin_tick != 0) return;
  state.last_pin_tick = now;

  std::vector<RingChunkState> states;
  {
    calldata_t call{};
    calldata_init(&call);
    calldata_set_ptr(&call, "states", &states);
    proc_handler_call(procs, "chunk_states", &call);
    calldata_free(&call);
  }
  std::string session_id;
  {
    std::lock_guard<std::mutex> lock(recording_mutex_);
    session_id = session_.id;
  }
  const int64_t now_wall_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
  // Allowed only while a Timeline session is being written and keeps its footage longer than the ring needs it.
  const bool allowed = timeline_writer_.load() && !session_id.empty() && dedupe_window_ok(config_.timeline_minutes, config_.buffer_seconds);

  std::vector<const RingChunkState*> wanted;
  int64_t start_ms = 0, end_ms = 0;
  // The number a lease is checked against, read before the files are looked at: a sweep that begins or ends after this
  // moment moves it, and the lease is refused instead of protecting footage the ring can no longer read.
  const uint64_t removal_seen = timeline_removal_seen(session_id);
  for (const RingChunkState& chunk : states) {
    const auto found = state.known.find(chunk.id);
    const bool ours = found != state.known.end() && found->second.session == session_id;
    if (!allowed || !chunk.linked || !ours || GetFileAttributesA(chunk.file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    if (wanted.empty()) {
      start_ms = found->second.wall_start_ms;
      end_ms = found->second.wall_end_ms;
    } else {
      start_ms = std::min(start_ms, found->second.wall_start_ms);
      end_ms = std::max(end_ms, found->second.wall_end_ms);
    }
    wanted.push_back(&chunk);
  }
  // The pin goes in before any chunk is trusted, and is renewed every pass; with nothing to rely on it is let go. The
  // lease is taken through the host's one gate-protected path, so the ring can never mutate the list a sweep is reading.
  bool trusted = false;
  if (!wanted.empty()) {
    TimelinePin pin;
    pin.id = kRingPinOwner;
    pin.owner = kRingPinOwner;
    pin.session_id = session_id;
    pin.start_ms = start_ms;
    pin.end_ms = end_ms + 1;
    pin.expires_ms = now_wall_ms + kPinSeconds * 1000;
    trusted = !take_timeline_lease(pin, now_wall_ms, removal_seen).empty();
  }
  if (!trusted) {
    // Nothing to rely on, or a sweep has already reserved the footage this pass: the ring drops the chunks it cannot
    // promise and lets its lease go, rather than keeping usable footage that is being deleted underneath it.
    if (state.pinned) release_timeline_leases(kRingPinOwner, std::string(), now_wall_ms);
    state.pinned = false;
    wanted.clear();
  } else {
    state.pinned = true;
  }
  for (const RingChunkState& chunk : states) {
    const bool should = std::find(wanted.begin(), wanted.end(), &chunk) != wanted.end();
    if (should == chunk.usable) continue;
    calldata_t call{};
    calldata_init(&call);
    calldata_set_int(&call, "id", chunk.id);
    calldata_set_int(&call, "usable", should ? 1 : 0);
    proc_handler_call(procs, "chunk_usable", &call);
    calldata_free(&call);
  }
  // Chunks the ring has forgotten.
  for (auto it = state.known.begin(); it != state.known.end();) {
    const uint32_t id = it->first;
    if (std::none_of(states.begin(), states.end(), [id](const RingChunkState& s) { return s.id == id; })) it = state.known.erase(it);
    else ++it;
  }
}

}  // namespace bx
