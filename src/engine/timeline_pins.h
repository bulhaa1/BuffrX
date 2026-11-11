// The Timeline's retention rules (docs/timeline-blueprint.md): footage older than the window goes,
// except what an active pin still covers, and never a chunk whose writer has not let go.
//
// Every time in here is the wall clock in milliseconds, the same clock SessionChunk::wall_start_ms and
// wall_end_ms carry, so the rules keep their meaning after a reboot. Nothing touches the disk: the
// session store runs these and deletes what they name.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "session_store.h"

namespace bx {

struct TimelinePin {
  std::string id;
  std::string owner;      // who asked for it: a clip being exported, a link being viewed
  std::string session_id; // the session the pin was made against
  int64_t start_ms = 0;   // wall clock, first moment it protects
  int64_t end_ms = 0;     // wall clock, last moment it protects
  int64_t expires_ms = 0; // the pin protects nothing at or after this moment
};

inline bool timeline_pin_active(const TimelinePin& pin, int64_t now_ms) { return pin.expires_ms > now_ms; }

// The replay ring's dedupe lease (obs_host_dedupe.cpp). It renews every second while the ring relies on the
// Timeline's chunks, so an explicit erase ignores it: it must never keep the person's own erase out for as long
// as the buffer runs.
inline constexpr const char* kTimelineRingLeaseOwner = "replay-ring";

// A replay save reading the Timeline's chunks (timeline_retention.h's read guard). Unlike the dedupe lease it reads
// the files, so an erase waits for it - and it is released the moment the read finishes, so the wait is bounded.
inline constexpr const char* kTimelineReplaySaveOwner = "replay-save";

inline bool timeline_editor_holds(const std::vector<TimelinePin>& pins, const std::string& session, int64_t now_ms) {
  for (const TimelinePin& pin : pins) {
    if (pin.session_id == session && pin.owner == "editor:" + session && timeline_pin_active(pin, now_ms)) return true;
  }
  return false;
}

// The owner of an active lease on `session` that is not `ignore_owner`, or empty when none. A removal that would
// take the whole session asks this under the one gate: a lease granted first is a reader still using the footage,
// and one asked for after the removal is refused by the reservation instead.
inline std::string timeline_session_lease_owner(const std::vector<TimelinePin>& pins, const std::string& session,
                                                int64_t now_ms, const std::string& ignore_owner) {
  for (const TimelinePin& pin : pins) {
    if (pin.session_id != session) continue;
    if (!ignore_owner.empty() && pin.owner == ignore_owner) continue;
    if (!timeline_pin_active(pin, now_ms)) continue;
    return pin.owner;
  }
  return std::string();
}

// A pin covers every chunk it overlaps by more than a boundary touch: a clip taken across a chunk
// boundary keeps both sides, and the chunk that only ends where the pin starts is not it.
inline bool timeline_pin_covers(const TimelinePin& pin, int64_t chunk_start_ms, int64_t chunk_end_ms) {
  return pin.start_ms < chunk_end_ms && chunk_start_ms < pin.end_ms;
}

// Whether a sweep is due: once a minute, measured on the wall clock. Not wrap-safe arithmetic, because
// the wall clock does not wrap on this scale; a first sweep (last_ms 0) is always due.
inline bool timeline_prune_due(int64_t last_ms, int64_t now_ms) { return now_ms - last_ms >= 60000; }

// An explicit session stays explicit. Otherwise the active writer wins, including before its first
// chunk is published; while a writer is starting, an older session must not impersonate the current one.
inline const SessionEntry* timeline_choose_session(const std::vector<SessionEntry>& sessions,
                                                   const std::string& requested,
                                                   const std::string& current, bool waiting) {
  const std::string& wanted = requested.empty() ? current : requested;
  if (!wanted.empty()) {
    for (const SessionEntry& entry : sessions) if (entry.id == wanted) return &entry;
    return nullptr;
  }
  if (waiting || sessions.empty()) return nullptr;
  return &sessions.front();
}

// The chunks the window expires, as indexes into `chunks`: one whose end is older than the window by
// more than its length, published, and not covered by a pin that is still active. A chunk ending exactly
// on the edge is still inside. An unpublished chunk is being written: its file is never touched,
// however old its index entry looks.
inline std::vector<size_t> timeline_expired_chunks(const std::vector<SessionChunk>& chunks, int minutes,
                                                   int64_t now_wall_ms, const std::vector<TimelinePin>& pins,
                                                   int64_t now_ms) {
  std::vector<size_t> expired;
  if (minutes <= 0) return expired;
  const int64_t window_ms = static_cast<int64_t>(minutes) * 60000;
  for (size_t index = 0; index < chunks.size(); ++index) {
    const SessionChunk& chunk = chunks[index];
    if (!chunk.published) continue;
    if (now_wall_ms - chunk.wall_end_ms <= window_ms) continue;
    bool covered = false;
    for (const TimelinePin& pin : pins) {
      if (!timeline_pin_active(pin, now_ms)) continue;
      if (timeline_pin_covers(pin, chunk.wall_start_ms, chunk.wall_end_ms)) {
        covered = true;
        break;
      }
    }
    if (!covered) expired.push_back(index);
  }
  return expired;
}

// A chunk's tick times as the protocol reports them: milliseconds since the session began. `start_ms` and
// `end_ms` in the index are absolute GetTickCount ticks, so they mean nothing to a client; the unsigned
// subtraction is also what keeps the tick clock's wrap from turning a short session into a negative one.
inline int64_t timeline_session_ms(uint32_t tick_ms, uint32_t started_ms) {
  return static_cast<int64_t>(static_cast<uint32_t>(tick_ms - started_ms));
}

// The chunks a reader may actually open, as indexes into `entry.chunks`: published - the writer has let go -
// and still on disk. A chunk that was deleted, pruned or moved is not footage however honestly it was
// indexed: naming it would give the page a segment that never decodes, and a save a file FFmpeg cannot open.
// `exists` is the caller's file test, so the rule itself needs neither a disk nor a Windows call.
template <class Exists>
inline std::vector<size_t> timeline_playable_chunks(const SessionEntry& entry, const Exists& exists) {
  std::vector<size_t> playable;
  for (size_t index = 0; index < entry.chunks.size(); ++index) {
    const SessionChunk& chunk = entry.chunks[index];
    if (!chunk.published) continue;
    if (!exists(chunk.file)) continue;
    playable.push_back(index);
  }
  return playable;
}

// The playable span of a session, in the numbers `timeline.get` reports and `timeline.save` takes:
// session milliseconds, and the bytes of the playable chunks only. An unpublished chunk is not footage -
// its writer still holds the file - so it never moves the edge, and neither does a published chunk whose
// file has gone: the edge is where footage exists. A hole in the middle leaves the edge where the footage
// either side of it ends, and the chunk list carries the hole.
inline void timeline_published_span(const SessionEntry& entry, const std::vector<size_t>& playable,
                                    int64_t& start_ms, int64_t& end_ms, int64_t& bytes) {
  start_ms = 0;
  end_ms = 0;
  bytes = 0;
  bool any = false;
  for (size_t index : playable) {
    if (index >= entry.chunks.size()) continue;
    const SessionChunk& chunk = entry.chunks[index];
    const int64_t chunk_start = timeline_session_ms(chunk.start_ms, entry.started_ms);
    const int64_t chunk_end = timeline_session_ms(chunk.end_ms, entry.started_ms);
    if (!any) {
      start_ms = chunk_start;
      end_ms = chunk_end;
      any = true;
    } else {
      start_ms = std::min(start_ms, chunk_start);
      end_ms = std::max(end_ms, chunk_end);
    }
    bytes += chunk.bytes;
  }
}

// The same span over every published chunk, for a caller that already knows they are all on disk (a save
// that just read them, the retention sweep). A session with none has no span: 0, 0, 0.
inline void timeline_published_span(const SessionEntry& entry, int64_t& start_ms, int64_t& end_ms,
                                    int64_t& bytes) {
  std::vector<size_t> published;
  for (size_t index = 0; index < entry.chunks.size(); ++index) {
    if (entry.chunks[index].published) published.push_back(index);
  }
  timeline_published_span(entry, published, start_ms, end_ms, bytes);
}

// The session with its chunks' times as session milliseconds (the clock `timeline.get` reports) instead of
// absolute ticks. The unsigned subtraction is wrap-safe, so a session that spans the 49.7-day tick wrap saves
// like any other; a save plans on this and never adds a tick to a range.
inline SessionEntry timeline_on_session_clock(const SessionEntry& entry) {
  SessionEntry out = entry;
  for (SessionChunk& chunk : out.chunks) {
    chunk.start_ms = static_cast<uint32_t>(timeline_session_ms(chunk.start_ms, entry.started_ms));
    chunk.end_ms = static_cast<uint32_t>(timeline_session_ms(chunk.end_ms, entry.started_ms));
  }
  out.started_ms = 0;
  return out;
}

// The session a save plans from: every chunk the index names, except a published one whose file has gone -
// planning from that one named a file the copy could not open halfway through the range. Returns where the
// footage ends, on the tick clock, which is what `timeline_save_plan` takes as `available_end_ms`. An
// unpublished chunk is the writer's and never moves the edge; a file can disappear between the listing and
// the copy, so the save asks this again right before it exports.
template <class Exists>
inline int64_t timeline_save_view(const SessionEntry& entry, SessionEntry& usable, const Exists& exists) {
  usable = entry;
  usable.chunks.clear();
  int64_t available_end = 0;
  for (const SessionChunk& chunk : entry.chunks) {
    if (chunk.published) {
      if (!exists(chunk.file)) continue;
      available_end = std::max(available_end, static_cast<int64_t>(chunk.end_ms));
    }
    usable.chunks.push_back(chunk);
  }
  return available_end;
}

// The leases still in force. An expired one protects nothing, so it is dropped rather than kept for a
// sweep to skip over: a crashed preview must not leave a pin in the list for ever.
inline std::vector<TimelinePin> timeline_pins_live(std::vector<TimelinePin> pins, int64_t now_ms) {
  pins.erase(std::remove_if(pins.begin(), pins.end(),
                            [now_ms](const TimelinePin& pin) { return !timeline_pin_active(pin, now_ms); }),
             pins.end());
  return pins;
}

// The lease an owner already holds on a session, or pins.size() when it holds none. An unnamed owner is
// never matched: anonymous leases are released by id alone.
inline size_t timeline_pin_owned(const std::vector<TimelinePin>& pins, const std::string& owner,
                                 const std::string& session_id) {
  if (owner.empty()) return pins.size();
  for (size_t index = 0; index < pins.size(); ++index) {
    if (pins[index].owner == owner && pins[index].session_id == session_id) return index;
  }
  return pins.size();
}

// Drops the expired leases, then takes `pin`: an owner's lease on the same session is renewed - same id,
// new span and expiry - rather than stacked, so a preview that renews every few minutes cannot fill the
// list. Returns the id in force, empty when the lease asks for nothing.
inline std::string timeline_pin_upsert(std::vector<TimelinePin>& pins, const TimelinePin& pin, int64_t now_ms) {
  pins = timeline_pins_live(std::move(pins), now_ms);
  if (pin.id.empty() || pin.end_ms <= pin.start_ms) return std::string();
  const size_t held = timeline_pin_owned(pins, pin.owner, pin.session_id);
  if (held < pins.size()) {
    pins[held].start_ms = pin.start_ms;
    pins[held].end_ms = pin.end_ms;
    pins[held].expires_ms = pin.expires_ms;
    return pins[held].id;
  }
  pins.push_back(pin);
  return pin.id;
}

// Releases one lease. False when it was not held, so a client can tell a release from a typo.
inline bool timeline_pin_release(std::vector<TimelinePin>& pins, const std::string& id) {
  if (id.empty()) return false;
  const size_t before = pins.size();
  pins.erase(std::remove_if(pins.begin(), pins.end(),
                            [&id](const TimelinePin& pin) { return pin.id == id; }),
             pins.end());
  return pins.size() != before;
}

// Releases every lease an owner holds, on one session when `session_id` is given. Returns how many went.
inline size_t timeline_pin_release_owner(std::vector<TimelinePin>& pins, const std::string& owner,
                                         const std::string& session_id) {
  if (owner.empty()) return 0;
  const size_t before = pins.size();
  pins.erase(std::remove_if(pins.begin(), pins.end(),
                            [&owner, &session_id](const TimelinePin& pin) {
                              return pin.owner == owner &&
                                     (session_id.empty() || pin.session_id == session_id);
                            }),
             pins.end());
  return before - pins.size();
}

}  // namespace bx
