// The recording store: one directory per session, holding the chunk files and a JSON index.
//
// A session is the unit of everything that follows — crash recovery, retention, range export — so it is
// one directory plus one small index file, never a database. The index is rewritten whole on every
// change: the files are small, and a partial-write protocol would cost more in recovery code than it
// saves in bytes.
//
// A session's markers are kept in `markers.json` beside the index, because a marker is added while the
// session writes and the index is rewritten with every chunk: a marker must not rewrite (or re-time) the
// file the footage's own bookkeeping lives in. A valid sidecar is the newer word; the index always carries
// the merged markers too, so a reader of either file sees a whole session.
//
// Every time in here is GetTickCount() milliseconds on the one clock the engine already uses for ticks.
// A surface that wants "12s into the session" subtracts `started_ms`; nothing else converts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bx {

class Json;

struct SessionChunk {
  std::string file;       // absolute path of a chunk that has been closed and its size measured
  int64_t bytes = 0;
  uint32_t start_ms = 0;  // first frame of the chunk
  uint32_t end_ms = 0;    // the moment the next chunk began, or the session stopped
  int64_t wall_start_ms = 0;  // start_ms on the wall clock, so retention survives a reboot
  int64_t wall_end_ms = 0;    // end_ms on the wall clock
  uint32_t epoch = 0;         // bumped when the encoders are rebuilt; a clip never crosses one
  bool published = true;      // false while its writer still holds the file: never playable
};

struct SessionMarker {
  uint32_t at_ms = 0;     // when the marker was written, same clock as the chunks
  std::string kind;       // clip | screenshot | recording | profile | gap | marker
  std::string label;      // what the user was told, if anything
  // The marker's own identity, so two markers with the same moment, kind and label are told apart. Written
  // by session_store_add_marker and kept across a close; "" for a marker written before ids existed, which
  // is removed by its old identity only.
  std::string id;
};

struct SessionEntry {
  std::string id;         // "BuffrX 2026-10-18 14-03-22"; also the directory name
  std::string kind;       // "recording"
  std::string app_id;     // the Steam app id the session belongs to; empty when unknown
  std::string profile;    // profile active when the session began
  uint32_t started_ms = 0;
  int64_t started_at = 0;  // wall clock, seconds since 1970: the only age that survives a reboot
  uint32_t ended_ms = 0;
  bool closed = false;      // written true by session_store_close
  bool interrupted = false; // derived on load: an index that exists with closed == false
  std::vector<SessionChunk> chunks;
  std::vector<SessionMarker> markers;

  int64_t bytes() const;    // the finished chunks only
};

// One root per process, set once after the clips directory is resolved.
void session_store_init(const std::string& sessions_root);
std::string session_store_root();

// Where a session's chunk files live: <root>\<id>\media
std::string session_media_dir(const std::string& id);

// Where a session's preview thumbnails live: <root>\<id>\thumbs. They are derived: nothing reads one as
// footage, and losing all of them costs a decode, never a clip.
std::string session_thumbs_dir(const std::string& id);

// Deletes the thumbnails a session no longer needs: every file in its thumbs folder whose name begins with
// `prefix` (one chunk's pictures, named by its key), or - with an empty prefix - the whole folder. The
// folder is removed too when it ends up empty. Never touches a chunk, an index or anything outside the
// session's own directory, and never fails a caller: a picture that will not go is retried by the next
// sweep, so deletion of footage is not blocked by a stale thumbnail.
void session_thumbs_remove(const std::string& id, const std::string& prefix);

// Begins a session: makes the directory, writes an index with closed == false, hands back the entry.
bool session_store_begin(const std::string& kind, SessionEntry& out, std::string& error);

// Records a chunk that has just been closed, then rewrites the index.
bool session_store_append_chunk(SessionEntry& entry, const SessionChunk& chunk, std::string& error);

// What a preview may play: the published chunks, in index order. An unpublished chunk is still being
// written, so its file is never handed to a player.
std::vector<const SessionChunk*> session_visible_chunks(const SessionEntry& entry);

// Records a marker in the session's own `markers.json` and leaves the chunk index alone. A missing
// sidecar (a session from before it existed) is migrated from the index's markers; duplicates are kept.
// The marker gets a new id, which is `entry.markers.back().id` afterwards.
bool session_store_add_marker(SessionEntry& entry, const std::string& kind, const std::string& label,
                              uint32_t now_ms, std::string& error);
// Remove one exact marker (at_ms + kind + label) and commit the sidecar before changing the caller's in-memory session.
bool session_store_remove_marker(SessionEntry& entry, uint32_t at_ms, const std::string& kind,
                                 const std::string& label, std::string& error);
// Remove the one marker carrying `marker_id`, which is how two identical markers are told apart. An empty
// or unknown id removes nothing: "a marker id is needed" or "marker not found".
bool session_store_remove_marker(SessionEntry& entry, const std::string& marker_id, std::string& error);

// Stamps ended_ms and closed, then rewrites the index with every marker in it and removes the sidecar.
bool session_store_close(SessionEntry& entry, uint32_t now_ms, std::string& error);

// Every session under the root, newest first. An index left with closed == false comes back interrupted.
// Markers come from the session's `markers.json` when it is readable, and from the index otherwise.
//
// A read never writes a session a writer still owns. A session kept open by a dead engine is the one
// exception: its chunks on disk are recovered into the index once, on its first read.
std::vector<SessionEntry> session_store_load(std::string& error);
bool session_store_load_one(const std::string& id, SessionEntry& out, std::string& error);

// The state file a live writer leaves in its session's own directory. It holds one token per process run,
// so a reader can tell a session being written now from one a dead engine left behind; that is the whole
// test behind "do not recover a live session". Exposed for the tests that stand in for a crashed engine.
std::string session_writer_state_path(const std::string& id);

// Removes a session's files and its index, refusing a session id that would leave the sessions root.
bool session_store_delete(const SessionEntry& entry, std::string& error);

struct TimelinePin;

// Deletes the timeline footage the `minutes` window expires (timeline_pins.h decides which chunks), then
// rewrites each affected index and drops the timeline sessions left empty. A file Windows still holds is
// kept in the index and named in `error` instead of being claimed as deleted. A normal `recording`
// session is never touched: Timeline's store is its own.
// The sweep reserves the chunks it means to delete in one short step under the retention gate
// (timeline_retention.h), then deletes and rewrites with no lock held: a lease asked for meanwhile is refused at once
// rather than waiting for a stalled disk, and a lease that read the index before a removal is refused after it.
// `open_id` is the session being written now: its writer owns its list and trims it itself (session_store_drop_chunks), so
// a sweep that also rewrote its index would have the writer's next append bring the deleted chunks back.
bool session_store_prune_timeline(int minutes, int64_t now_wall_ms, std::string& error,
                                  const std::string& open_id = std::string());

// A chunk the open session's writer let go of: its file is deleted by the retention worker, not by the tick.
struct DroppedChunk {
  std::string session_id;
  SessionChunk chunk;
};

// Removes the chunks at `expired` (indexes into entry.chunks) from the open session's own list and rewrites its index once.
// The dropped ones are returned in `dropped`; nothing changes when the index cannot be written.
bool session_store_drop_chunks(SessionEntry& entry, const std::vector<size_t>& expired, std::vector<DroppedChunk>& dropped,
                               std::string& error);

// The open session's own trim: the chunks its window expires, decided and reserved in one short step under the
// retention gate (timeline_retention.h) and then dropped from its list. A save that asks for a lease while the rename
// and the index write run is answered at once and refused for those chunks, never granted over footage that is going.
// The caller holds the recording mutex; drop_chunks takes the store's own lock.
bool session_store_drop_expired(SessionEntry& entry, int minutes, int64_t now_wall_ms,
                                std::vector<DroppedChunk>& dropped, std::string& error);

// Deletes dropped chunks' files (only inside their session's own directory) and the pictures made for them. A file Windows
// still holds is kept and returned in `still_held`, to be tried again at the next sweep.
void session_store_delete_chunk_files(const std::vector<DroppedChunk>& dropped, std::vector<DroppedChunk>& still_held);

// Erases one temporary timeline session, collecting in `locked` the files Windows still holds. Never a
// normal `recording` session and never a saved library clip: only files inside the session's own
// directory are removed, and success is claimed only when nothing was left behind. The session is
// claimed against live leases for the whole erase; a caller that must hold that claim across its own
// retries (protocol's timeline.erase) passes the ticket it took with TimelineRetentionTicket::claim_session.
class TimelineRetentionTicket;
bool session_store_erase_timeline(const std::string& id, std::vector<std::string>& locked, std::string& error,
                                  const TimelineRetentionTicket* claim = nullptr);

// True when an id is safe to use as a directory name under the root.
bool session_id_is_safe(const std::string& id);

// True when `a` began before `b`. The wall clock decides; an index written before started_at existed
// falls back to its id, a local "YYYY-MM-DD HH-MM-SS" stamp that sorts. Never the tick clock, which
// restarts at every boot and would make yesterday look newer than today.
bool session_older(const SessionEntry& a, const SessionEntry& b);

// Deletes the oldest sessions until the store fits `budget_bytes` (0 = unlimited, nothing removed).
// `open_id` is the session being recorded right now and is never touched; a crashed session (an index
// left unclosed by a dead engine) counts as finished. The newest session always stays. Reports what
// went, how much it freed and what remains.
bool session_prune(int64_t budget_bytes, const std::string& open_id, std::vector<std::string>& removed,
                   int64_t& freed_bytes, int64_t& total_bytes, std::string& error);

// The pre-start free-space guard: false with a reason when the clips volume has less than
// `min_free_gb` free. A floor of 0 never refuses, and an unreadable volume (negative) does not block.
bool recording_space_ok(const std::string& clips_dir, int64_t free_bytes, int min_free_gb, std::string& reason);

// The marker a successful write leaves in the open session: clip, screenshot, recording or profile.
// Empty for anything that is not one of those writes, so reads never touch the index.
std::string marker_kind_for_command(const std::string& command);

// The index's JSON half, with no files in it, so the mutation harness can drive it: reads one session entry out of a
// parsed index object, or writes one back. The file walk around them stays private to session_store.cpp.
bool session_entry_from_json(const Json& json, SessionEntry& out);
Json session_entry_json(const SessionEntry& entry);

}  // namespace bx
