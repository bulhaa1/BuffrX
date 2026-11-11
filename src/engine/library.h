// Clip and screenshot library: listing, thumbnails, duration probing and safe file operations.
//
// Every path this module touches is validated against the configured media directories first, so a
// protocol client can never ask the engine to delete or rename something outside the library.
#pragma once

#include <string>
#include <vector>

#include "config.h"

namespace bx {

struct MediaItem {
  std::string file;       // absolute path
  std::string name;       // file name without extension
  int64_t size = 0;
  int64_t modified = 0;   // unix seconds
  double duration = 0.0;  // seconds, 0 when unknown
  std::string thumbnail;  // absolute path to a cached thumbnail, empty when not generated yet
  std::string folder;     // the library folder it lives in ("Counter-Strike 2", "Recordings"), "" at the root
};

struct LibraryPaths {
  std::string clips_dir;
  std::string screenshots_dir;
  std::string thumbnails_dir;
  // Where recordings and session chunks live: the configured recordings folder, or clips_dir when none is
  // set (recordings_base_dir). Writers and list_recordings must read the same one.
  std::string recordings_dir;
};

void library_init(const LibraryPaths& paths);
// The four roots the library reads and guards, built from one config: a new root cannot be wired into
// the writers and forgotten here.
LibraryPaths library_paths_from(const Config& config, const std::string& data_root);

// The paths the library was initialised with; the media side needs the thumbnails directory.
void library_paths(LibraryPaths& out);

// Stable cache key for a file path, used to name its thumbnail.
uint64_t library_file_key(const std::string& file);

// Newest first. Video extensions only; screenshots use list_screenshots.
std::vector<MediaItem> list_clips();

// Files the save pipeline is still trimming and moving: never listed, never opened. Opening one for a
// thumbnail held it open, and the trim and the move into its game folder failed (ERROR_SHARING_VIOLATION).
void library_hold(const std::string& file);
void library_release(const std::string& file);
bool library_is_held(const std::string& file);
std::vector<MediaItem> list_screenshots();

// Every recording under the clips directory, newest first: single-file recordings in
// <clips>\Recordings and every chunk of every session. Never returns a session index file, and never
// a Timeline session's footage: that is temporary and has its own store.
std::vector<MediaItem> list_recordings();

// Bytes and counts for the storage screen. Recordings come from the files themselves; sessions come
// from the session index, which is the same number retention prunes against.
void library_recordings_usage(int64_t& bytes, int& count);
void library_sessions_usage(int64_t& bytes, int& count);

// Generates a thumbnail on first request and returns its path (empty on failure). `reason`, when
// given, takes the decoder's own line: "the media work is busy" is the one answer that changes by
// itself, so a caller can tell it from a file that will not open.
std::string ensure_thumbnail(const std::string& file, std::string* reason = nullptr);
// The thumbnail already on disk for a file, or empty. Decodes nothing.
std::string cached_thumbnail(const std::string& file);
// Stills spread across a clip, for the share sheet's looping preview: `kPreviewFrames` JPEGs, cached beside the
// thumbnail, returned in order (empty when the clip cannot be decoded). Frames a decode could not make are left out.
constexpr int kPreviewFrames = 8;
std::vector<std::string> ensure_preview_frames(const std::string& file);

// Fills each item's duration and thumbnail path. The listing itself stays free of the media runtime;
// this is the one call that touches it.
//
// Durations are cached by path, size and modification time, in memory and in
// <data root>\library-cache.json, so a fresh engine start does not re-open a whole library; an entry
// is trusted only while the file is unchanged, and a file that was probed and has no length keeps its
// 0 rather than being probed again.
//
// `thumbnail_limit` caps how many thumbnails are generated here: each one opens the file through
// FFmpeg and decodes a frame, and this runs on the lock-free worker that also serves the folder picker
// and ui.capture. 0 (or fewer) means "no thumbnails" and is what a caller that does not draw the cards
// should pass; the panel asks for the rest as each card is drawn.
//
// A listing opens at most a few uncached files (kProbeBudget in library_media.cpp): a cold library is
// answered from what is known, and the next listing continues where this one stopped.
void library_annotate(std::vector<MediaItem>& items, int thumbnail_limit = 0);

// How many thumbnails a listing of `listed_count` clips should generate up front for a view showing
// `visible_count` of them, and never more than the listing has. Pure, so the policy is testable: the
// answer is the visible cards plus a screen's worth of lookahead, capped at what exists.
int thumbnail_budget(int visible_count, int listed_count);

// Whether a listing must leave this file alone for now: a save is trimming and moving it (`held`), a
// writer still holds it open (`writer_open`), or it is too fresh to be finished - a raw replay lands in
// the clips root and is still being written before the save knows its path (`in_clips_root`). Times are
// unix milliseconds. Pure, so the tests can hold the highest-consequence file rule in the library to
// what library_media.cpp does with it.
bool annotate_should_skip(const std::string& path, uint64_t now_ms, bool held, bool writer_open, bool in_clips_root,
                          uint64_t modified_ms);

// Whether a listing has to ask the writer that may hold this clip. The ask opens the file, and doing it for
// every clip is what a 5,000-clip listing spent 16 s on (overnight N10): a writer moves the mtime on every
// write, so a file whose mtime has not moved for a minute cannot be held and is listed without the open.
// Pure, for the tests.
bool annotate_writer_probe_needed(uint64_t now_ms, uint64_t modified_ms);

// How long the file is, read through the runtime's own FFmpeg; 0.0 when it cannot be determined.
double probe_duration(const std::string& file);

// Guarded operations: both return false for a path outside the configured directories.
bool library_delete(const std::string& file);
// Whether a file may be removed at all: inside the library, not one chunk of a session, and a file.
bool library_can_remove(const std::string& file);
// Moves a file to the Recycle Bin. Refuses what library_can_remove refuses, and never falls back to a
// permanent delete when the bin is unavailable.
bool library_trash(const std::string& file);
// Drops the cached thumbnail and preview frames of a path, for a file replaced under the same name.
void library_forget_cached_images(const std::string& file);
bool library_rename(const std::string& file, const std::string& new_name, std::string& result_path);

// The folder a clip is filed under, made from the game or window title the way Rewind made it: the
// trailing " - Something" suffix dropped (renderer or class tags), control characters and the
// characters Windows forbids removed, whitespace collapsed, at most 100 characters, no trailing dot
// or space, and "Unknown" for an empty result or a reserved device name (CON, NUL, COM1...).
std::string media_folder_name(const std::string& title);

// A file stem with the capture time: "Replay 2026-10-19 00-19-04". Local time, the format Rewind
// used, so clips sort by name the way they sort by date.
std::string timestamped_stem(const std::string& prefix, int year, int month, int day, int hour, int minute,
                             int second);
// The same, at the current local time.
std::string timestamped_stem_now(const std::string& prefix);

// The folders the library files clips into, under the clips root. Exposed for the listing rules:
// these are not game folders.
constexpr const char* kScreenshotsFolder = "Screenshots";
constexpr const char* kEditsFolder = "Edits";
constexpr const char* kDisplayFolder = "Display Capture";
constexpr const char* kGameFolder = "Game Capture";

// Reserves a unique target path for a new clip or screenshot, never overwriting an existing file.
std::string reserve_output_path(const std::string& directory, const std::string& stem, const std::string& extension);

// True when the path lives inside one of the configured media directories.
bool library_owns_path(const std::string& file);

// Storage totals for the UI's usage line.
void library_storage_usage(int64_t& clips_bytes, int64_t& screenshots_bytes, int& clip_count, int& screenshot_count);

// Free bytes on the volume holding `path`, 0 when the path has no volume (Rewind's storage.ts
// reports 0 the same way).
int64_t volume_free_bytes(const std::string& path);

}  // namespace bx
