// The Timeline's leases and the removals that must never take what one covers.
//
// A save, a preview or the ring's dedupe holds a lease (timeline_pins.h); retention deletes footage older than the
// window. Both sides run under this one gate, and the gate is only ever held for a few vector operations. A sweep
// reserves the chunks it means to delete under it, then deletes the files and rewrites the index with no lock held: a
// delete under this gate used to freeze a hotkey, the tick and the panel, because a save takes the engine lock before
// it asks for its lease, and the gate held through a stalled disk held the engine lock behind it.
//
// The reservation is what keeps footage safe without holding the gate. A chunk reserved for removal is named in
// timeline_reservation_list(); a lease covering it is refused while the reservation stands (timeline_lease_take), and
// a chunk a live lease covers is never reserved. There are only two orders: the lease wins and the chunk is kept, or
// the reservation wins and the lease is told the footage is going.
//
// Telling a late lease the truth takes one more number. A client reads the index first and asks for the lease after, so
// a removal that finished in between would leave it holding a lease over a file that is gone. timeline_removal_seen()
// is that number, kept per session: it changes whenever a reservation for that session is taken or dropped, a client
// captures it before it reads the index, and a lease asked for under a changed number is refused. So any removal that
// overlaps its read is seen, and one that finished before it is already out of the index it reads.
//
// A lease covers a span and expires; a replay save reading chunk files needs neither (ring_output.cpp). Its read guard
// covers the whole session with no clock and stands until the read actually finishes, so a join slower than any lease
// stays protected. It is not the ring's background dedupe lease, which the person's erase ignores by design.
//
// One list and one gate per process, like the store's root: the host installs the leases the protocol hands over, the
// session store sweeps with the same list, and obs_host_dedupe.cpp renews the ring's lease through the same path.
//
// Lock order: the engine lock, then the session store's recording/store locks, then this gate - it is innermost, and
// nothing takes another lock under it (a sweep that held it across a delete was the bug). No disk call is made under it.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "session_store.h"
#include "timeline_pins.h"

#ifdef BUFFRX_TEST_HOOKS
#include <functional>
#endif

namespace bx {

// A removal in flight: one chunk, or the whole session when `file` is empty. Read and written under the gate.
struct TimelineReservation {
  uint64_t owner = 0;  // the ticket that holds it, so a release drops only its own
  std::string session_id;
  std::string file;
  int64_t start_ms = 0;  // the chunk's wall-clock span, for a lease that must be refused while it goes
  int64_t end_ms = 0;
};

// How many reservations of one session have been taken or dropped. Read under the gate, and only compared: a lease
// granted under a different number was decided on an index the removal changed.
struct TimelineRemovalMark {
  uint64_t generation = 0;
  int64_t touched_ms = 0;
};

// The leases in force. Read and written under timeline_retention_gate().
inline std::vector<TimelinePin>& timeline_lease_list() {
  static std::vector<TimelinePin> pins;
  return pins;
}

inline std::vector<TimelineReservation>& timeline_reservation_list() {
  static std::vector<TimelineReservation> reservations;
  return reservations;
}

inline std::map<std::string, TimelineRemovalMark>& timeline_removal_marks() {
  static std::map<std::string, TimelineRemovalMark> marks;
  return marks;
}

// The next ticket's number. One counter for the process: it only has to name one removal among the ones in flight.
inline uint64_t& timeline_reservation_sequence() {
  static uint64_t next = 0;
  return next;
}

inline std::mutex& timeline_retention_gate() {
  static std::mutex gate;
  return gate;
}

#ifdef BUFFRX_TEST_HOOKS
// Test builds only: called just before retention waits on the file system ("delete" a chunk, "index" a session's
// index, "session" a whole session's files), so a test can hold that wait open and prove a lease asked for meanwhile is
// answered at once instead of waiting for the disk.
inline std::function<void(const char* stage, const std::string& path)>& timeline_store_pause_hook() {
  static std::function<void(const char*, const std::string&)> hook;
  return hook;
}
#endif

inline void timeline_removal_mark_locked(const std::string& session_id, int64_t now_wall_ms) {
  TimelineRemovalMark& mark = timeline_removal_marks()[session_id];
  ++mark.generation;
  mark.touched_ms = now_wall_ms;
}

inline uint64_t timeline_removal_generation_locked(const std::string& session_id) {
  const auto found = timeline_removal_marks().find(session_id);
  return found == timeline_removal_marks().end() ? 0 : found->second.generation;
}

// Marks of sessions nobody has swept for a day are dropped: they only exist to cover a client's read of an index it
// has since been handed again. Called when a removal starts, never while one is held.
inline void timeline_removal_marks_prune_locked(int64_t now_wall_ms) {
  if (now_wall_ms <= 0) return;
  std::map<std::string, TimelineRemovalMark>& marks = timeline_removal_marks();
  for (auto it = marks.begin(); it != marks.end();) {
    if (it->second.touched_ms > 0 && now_wall_ms - it->second.touched_ms > 24 * 60 * 60 * 1000) {
      it = marks.erase(it);
    } else {
      ++it;
    }
  }
}

inline bool timeline_session_reserved_locked(const std::string& session_id) {
  for (const TimelineReservation& item : timeline_reservation_list()) {
    if (item.session_id == session_id && item.file.empty()) return true;
  }
  return false;
}

inline bool timeline_file_reserved_locked(const std::string& session_id, const std::string& file) {
  for (const TimelineReservation& item : timeline_reservation_list()) {
    if (item.session_id == session_id && !item.file.empty() && item.file == file) return true;
  }
  return false;
}

// One read-modify-write of the leases under the gate: a lease is released as one step, so a sweep can never decide
// between the read and the write. A lease is granted through timeline_lease_take, which also checks the removals in
// flight; this is for a change that only drops one.
template <class Mutate>
inline auto timeline_leases_update(Mutate&& mutate) -> decltype(mutate(std::declval<std::vector<TimelinePin>&>())) {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  return mutate(timeline_lease_list());
}

// The leases in force, as a copy: for a reader that only reports or decides, never for one that deletes.
inline std::vector<TimelinePin> timeline_leases_snapshot() {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  return timeline_lease_list();
}

// The removals in flight, as a copy: a report or a test, never a decision.
inline std::vector<TimelineReservation> timeline_reservations_snapshot() {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  return timeline_reservation_list();
}

// How many removals are in flight. One gate step, for a test or a log line.
inline size_t timeline_reservation_count() {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  return timeline_reservation_list().size();
}

// The number a client captures before it reads a session's index, and the number its lease is checked against.
inline uint64_t timeline_removal_seen(const std::string& session_id) {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  return timeline_removal_generation_locked(session_id);
}

// Grants (or renews) a lease under the gate. Empty when the footage is going: a removal has reserved a span the lease
// covers, or one began or ended after `seen_generation` was read - the index the client decided on is stale then, and
// a lease over a deleted file must not be reported as held.
inline std::string timeline_lease_take(const TimelinePin& pin, int64_t now_ms, uint64_t seen_generation) {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  if (timeline_removal_generation_locked(pin.session_id) != seen_generation) return std::string();
  for (const TimelineReservation& item : timeline_reservation_list()) {
    if (item.session_id != pin.session_id) continue;
    if (item.file.empty() || timeline_pin_covers(pin, item.start_ms, item.end_ms)) return std::string();
  }
  return timeline_pin_upsert(timeline_lease_list(), pin, now_ms);
}

// Releases one lease by id. False when it was not held, so a client can tell a release from a typo.
inline bool timeline_lease_revoke(const std::string& id, int64_t now_ms) {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  std::vector<TimelinePin>& pins = timeline_lease_list();
  pins = timeline_pins_live(std::move(pins), now_ms);
  return timeline_pin_release(pins, id);
}

// Releases every lease an owner holds, on one session when `session_id` is given.
inline size_t timeline_lease_revoke_owner(const std::string& owner, const std::string& session_id, int64_t now_ms) {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  std::vector<TimelinePin>& pins = timeline_lease_list();
  pins = timeline_pins_live(std::move(pins), now_ms);
  return timeline_pin_release_owner(pins, owner, session_id);
}

// A replay save reading a session's chunk files (ring_output.cpp). It is not a lease: there is no span to cover and
// no expiry, because what it protects is the read itself, which lasts exactly as long as the save that holds it -
// however long a slow join takes. Retention skips a guarded session and an explicit erase is refused while one
// stands. The ring's background dedupe lease is NOT one of these: it never opens a file and is renewed forever, so an
// erase ignores it; a read guard is released on every way out of the save, so it can never keep an erase out for good.
struct TimelineReadGuardEntry {
  uint64_t id = 0;
  std::string session_id;
};

inline std::vector<TimelineReadGuardEntry>& timeline_read_guard_list() {
  static std::vector<TimelineReadGuardEntry> guards;
  return guards;
}

inline uint64_t& timeline_read_guard_sequence() {
  static uint64_t next = 0;
  return next;
}

// A read guard in flight: taken before the first chunk file is opened, released when the read ends. The take and a
// removal are decided under the one gate, so a save and an erase can never both be granted: whichever asks second is
// refused - the erase by `claim_session`, the read by an inactive guard whose chunks the save then does not use.
class TimelineReadGuard {
 public:
  TimelineReadGuard() = default;
  TimelineReadGuard(const TimelineReadGuard&) = delete;
  TimelineReadGuard& operator=(const TimelineReadGuard&) = delete;
  TimelineReadGuard(TimelineReadGuard&& other) noexcept { adopt(other); }
  TimelineReadGuard& operator=(TimelineReadGuard&& other) noexcept {
    if (this != &other) {
      release();
      adopt(other);
    }
    return *this;
  }
  ~TimelineReadGuard() { release(); }

  bool active() const { return id_ != 0; }
  const std::string& session_id() const { return session_; }

  // Inactive when a removal already owns the session: the save must not read files that pass is deleting, so it
  // leaves those chunks out of its plan (ring_output.cpp) instead of opening them.
  static TimelineReadGuard take(const std::string& session_id) {
    TimelineReadGuard guard;
    if (session_id.empty()) return guard;
    std::lock_guard<std::mutex> gate(timeline_retention_gate());
    for (const TimelineReservation& item : timeline_reservation_list()) {
      if (item.session_id == session_id) return guard;
    }
    guard.id_ = ++timeline_read_guard_sequence();
    guard.session_ = session_id;
    timeline_read_guard_list().push_back(TimelineReadGuardEntry{guard.id_, session_id});
    return guard;
  }

  void release() {
    if (id_ == 0) return;
    {
      std::lock_guard<std::mutex> gate(timeline_retention_gate());
      std::vector<TimelineReadGuardEntry>& guards = timeline_read_guard_list();
      guards.erase(std::remove_if(guards.begin(), guards.end(),
                                  [owner = id_](const TimelineReadGuardEntry& item) { return item.id == owner; }),
                   guards.end());
    }
    id_ = 0;
    session_.clear();
  }

 private:
  void adopt(TimelineReadGuard& other) noexcept {
    id_ = other.id_;
    session_ = std::move(other.session_);
    other.id_ = 0;
    other.session_.clear();
  }

  uint64_t id_ = 0;
  std::string session_;
};

// Whether a replay save is reading this session now. Read under the gate.
inline bool timeline_read_guard_held_locked(const std::string& session_id) {
  for (const TimelineReadGuardEntry& guard : timeline_read_guard_list()) {
    if (guard.session_id == session_id) return true;
  }
  return false;
}

// How many reads are in flight. One gate step, for a test or a log line.
inline size_t timeline_read_guard_count() {
  std::lock_guard<std::mutex> gate(timeline_retention_gate());
  return timeline_read_guard_list().size();
}


// A removal in flight. The gate is taken only to install it and to drop it; the file work between the two runs with no
// lock held, so a delete that stalls a disk stalls nothing else. A lease asked for while it stands is refused, and one
// that read its index before it is refused through the removal's number.
class TimelineRetentionTicket {
 public:
  TimelineRetentionTicket() = default;
  TimelineRetentionTicket(const TimelineRetentionTicket&) = delete;
  TimelineRetentionTicket& operator=(const TimelineRetentionTicket&) = delete;
  TimelineRetentionTicket(TimelineRetentionTicket&& other) noexcept { adopt(other); }
  TimelineRetentionTicket& operator=(TimelineRetentionTicket&& other) noexcept {
    if (this != &other) {
      release();
      adopt(other);
    }
    return *this;
  }
  ~TimelineRetentionTicket() { release(); }

  bool active() const { return id_ != 0; }
  const std::vector<size_t>& reserved() const { return reserved_; }

  // Drops the reservation and moves the session's number on, so a lease that read the index before this removal is
  // refused after it. Called by the destructor, or by a caller that must take a new ticket on the same session (the
  // empty-session delete after its chunks went).
  void release() {
    if (id_ == 0) return;
    {
      std::lock_guard<std::mutex> gate(timeline_retention_gate());
      std::vector<TimelineReservation>& reservations = timeline_reservation_list();
      reservations.erase(std::remove_if(reservations.begin(), reservations.end(),
                                        [owner = id_](const TimelineReservation& item) { return item.owner == owner; }),
                         reservations.end());
      timeline_removal_mark_locked(session_, now_ms_);
    }
    id_ = 0;
    reserved_.clear();
  }

  // The chunks the window expires, decided and reserved in one short step under the gate. `inside` is the caller's
  // check that the file is the session's own. Returns the indexes it reserved, ready to be deleted with the gate free.
  template <class Inside>
  static TimelineRetentionTicket reserve_chunks(const std::vector<SessionChunk>& chunks, const std::string& session_id,
                                                int minutes, int64_t now_wall_ms, Inside&& inside) {
    TimelineRetentionTicket ticket;
    std::lock_guard<std::mutex> gate(timeline_retention_gate());
    timeline_removal_marks_prune_locked(now_wall_ms);
    // A session going away as a whole owns every file in it; one already going is left to its own pass.
    if (timeline_session_reserved_locked(session_id)) return ticket;
    // A replay save is reading this session's chunks: its records live in them and its read guard stands until the
    // save is done, however long the join takes (a lease would have expired by then). The sweep waits for it.
    if (timeline_read_guard_held_locked(session_id)) return ticket;
    const std::vector<size_t> expired =
        timeline_expired_chunks(chunks, minutes, now_wall_ms, timeline_lease_list(), now_wall_ms);
    std::vector<size_t> indexes;
    std::vector<TimelineReservation> fresh;
    for (size_t index : expired) {
      const SessionChunk& chunk = chunks[index];
      if (!inside(chunk)) continue;
      if (timeline_file_reserved_locked(session_id, chunk.file)) continue;
      fresh.push_back(TimelineReservation{0, session_id, chunk.file, chunk.wall_start_ms, chunk.wall_end_ms});
      indexes.push_back(index);
    }
    if (fresh.empty()) return ticket;
    const uint64_t id = ++timeline_reservation_sequence();
    for (TimelineReservation& item : fresh) item.owner = id;
    std::vector<TimelineReservation>& reservations = timeline_reservation_list();
    reservations.insert(reservations.end(), fresh.begin(), fresh.end());
    ticket.id_ = id;
    ticket.session_ = session_id;
    ticket.now_ms_ = now_wall_ms;
    ticket.reserved_ = std::move(indexes);
    // Taken before the file work: a lease that read the index before this moment is refused after it.
    timeline_removal_mark_locked(session_id, now_wall_ms);
    return ticket;
  }

  // The whole session is being removed - its index, its media directory, its markers. Inactive when any removal still
  // holds part of it: the next sweep finishes what this pass found going.
  static TimelineRetentionTicket reserve_session(const std::string& session_id, int64_t now_wall_ms) {
    TimelineRetentionTicket ticket;
    std::lock_guard<std::mutex> gate(timeline_retention_gate());
    for (const TimelineReservation& item : timeline_reservation_list()) {
      if (item.session_id == session_id) return ticket;
    }
    // A replay save is reading the footage: the whole-session removal waits for the read to end.
    if (timeline_read_guard_held_locked(session_id)) return ticket;
    const uint64_t id = ++timeline_reservation_sequence();
    timeline_reservation_list().push_back(TimelineReservation{id, session_id, std::string(), 0, 0});
    ticket.id_ = id;
    ticket.session_ = session_id;
    ticket.now_ms_ = now_wall_ms;
    timeline_removal_mark_locked(session_id, now_wall_ms);
    return ticket;
  }

  // The whole session is being removed by an explicit erase, so a lease that still reads it is a reader that must
  // finish first: the claim is refused and `leased_by` names the owner. The ring's own dedupe lease is ignored - it is
  // renewed every second while the ring relies on the footage, and it must never block the person's erase for good.
  // A replay save's read guard is not ignored: it is released when the read ends, so the erase waits for a real read
  // and no longer than that.
  // The lease and the claim are decided under the one gate, so a save, a preview or an editor lease and an erase can
  // never both be granted: whichever asks second is refused, and no check-then-reserve gap is left between them.
  static TimelineRetentionTicket claim_session(const std::string& session_id, int64_t now_wall_ms,
                                               const std::string& ignore_owner, std::string& leased_by) {
    leased_by.clear();
    TimelineRetentionTicket ticket;
    std::lock_guard<std::mutex> gate(timeline_retention_gate());
    // Another removal owns part of it; the next pass finishes what this one found going.
    for (const TimelineReservation& item : timeline_reservation_list()) {
      if (item.session_id == session_id) return ticket;
    }
    // A replay save is reading the session's chunk files now. It is a reader that must finish first, and its guard
    // goes the moment the read ends - so unlike the ring's dedupe lease it never keeps the erase out for good.
    if (timeline_read_guard_held_locked(session_id)) {
      leased_by = kTimelineReplaySaveOwner;
      return ticket;
    }
    leased_by = timeline_session_lease_owner(timeline_lease_list(), session_id, now_wall_ms, ignore_owner);
    if (!leased_by.empty()) return ticket;
    const uint64_t id = ++timeline_reservation_sequence();
    timeline_reservation_list().push_back(TimelineReservation{id, session_id, std::string(), 0, 0});
    ticket.id_ = id;
    ticket.session_ = session_id;
    ticket.now_ms_ = now_wall_ms;
    timeline_removal_mark_locked(session_id, now_wall_ms);
    return ticket;
  }

 private:
  void adopt(TimelineRetentionTicket& other) noexcept {
    id_ = other.id_;
    session_ = std::move(other.session_);
    now_ms_ = other.now_ms_;
    reserved_ = std::move(other.reserved_);
    other.id_ = 0;
    other.reserved_.clear();
  }

  uint64_t id_ = 0;
  std::string session_;
  int64_t now_ms_ = 0;
  std::vector<size_t> reserved_;
};

}  // namespace bx
