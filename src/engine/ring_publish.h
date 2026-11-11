// How one replay save's file reaches the name a person sees (ring_output.cpp). Naming and one publishing loop, with the
// file system calls the caller supplies: no libobs and no Windows header, so the publication rules are checked against a
// real folder (tests/ring_save_tests.cpp).
//
// A save used to take its final name by creating an empty file there, close the handle, and let the muxer (or the join)
// open that same name for truncation. Another writer could replace or fill the name in between, and the muxer then
// truncated its bytes; the cleanup that removed the "empty claim" told the save's own reservation from a stranger's file
// by nothing but a path and a size. Instead the save writes a staging file its own name owns from the first byte, and
// publishes it with a rename that never replaces: a name another writer took refuses the rename, that writer keeps every
// byte, and the save tries the next name. Only the save's own staging file is ever deleted.
#pragma once

#include <cctype>
#include <string>

#include "ring_save.h"

namespace bx {

// How many staged names and published names one save tries. A refused name belongs to another writer's file and is never
// written; running out of names is a failure the host hears about, with the footage still on disk.
constexpr int kStageNameAttempts = 8;
constexpr int kPublishNameAttempts = kSaveNameAttempts;

// The key a name is compared by: Windows compares paths without case.
inline std::string ring_name_key(std::string path) {
  for (char& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return path;
}

// The stem of a staging name: the configured stem, the marker every listing of the library drops (".part", so a clip
// being written is never listed), and the save's owner tag - its process and its own number - so a part left behind by an
// earlier save is never taken for this one's. `attempt` only adds a counter for a staged name that is somehow already
// there. The caller keeps the folder and the real extension: the muxer and the join choose the container from it.
inline std::string ring_staging_stem(const std::string& stem, const std::string& owner, int attempt = 0) {
  std::string name = stem + ".part." + owner;
  if (attempt > 0) name += " (" + std::to_string(attempt) + ")";
  return name;
}

// Publishes `source` under the first name that is free on disk and no earlier save of this process holds, by a move that
// never replaces. `free_name(directory, stem, suffix)` is reserve_output_path's answer for one attempt (the name
// available now, cleaned); `rename(from, to)` performs the move and returns false when a file is already at `to`
// (MoveFileExA is never given MOVEFILE_REPLACE_EXISTING); `claimed(key)` says a name this process already handed out,
// which stays taken for a while because the host files the clip away and frees the name while a trim still reads it.
//
// A refused name leaves that writer's file and `source` exactly as they were, so neither loses a byte and the save
// settles on the next name. Returns the published path, or empty when every attempt was refused: the caller then keeps
// its staged file, because the footage in it is the person's whether a name was freed or not.
template <class FreeName, class Rename, class Claimed>
inline std::string ring_publish(const std::string& source, const std::string& directory, const std::string& stem,
                                const std::string& suffix, FreeName&& free_name, Rename&& rename, Claimed&& claimed,
                                int attempts = kPublishNameAttempts) {
  if (source.empty()) return std::string();
  return save_path_for(
      stem,
      [&](const std::string& candidate) {
        const std::string target = free_name(directory, candidate, suffix);
        if (target.empty()) return target;                        // no name free on disk for this attempt
        if (claimed(ring_name_key(target))) return std::string();  // a name this process already handed out
        // false: a file is there now, and it keeps its own bytes. The save moves to the next name.
        return rename(source, target) ? target : std::string();
      },
      attempts);
}

// What a save does with the file it wrote: give it the name the person sees, or leave it where the save wrote it. The
// decision is the writer's verdict, never the file's size. A writer that did not finish - a header it could not write, a
// packet the pipe refused, a muxer process that exited non-zero, a part whose write failed - can leave a file that looks
// like a clip and plays part of the footage. Publishing it would put a truncated clip in the library under a normal name
// while the verdict says the save failed, so a failed writer's file is never published: its bytes stay at the staging name
// the save owns, which the library's listing drops (".part" in the name), and the verdict's path names it, so the footage
// can be recovered and nothing is announced that did not finish.
struct SaveSettlement {
  std::string path;        // where the footage really is: the published name, the staging name, or empty with no bytes
  bool published = false;  // the file took the name the person sees
  bool kept = false;       // bytes stayed at the staging name because the writer failed: hidden, owned, recoverable
};

// Settles one save's staged file. `has_bytes(path)` says a file holds footage; `remove(path)` deletes an empty stage this
// save created; `publish(staged)` gives a finished file the first free name (ring_publish) and returns that name, or empty
// when no name could be freed. An empty stage is deleted, because an empty file is not a clip and no name should be
// announced for it. A failed writer's bytes are kept at `staged` and never published. A finished file is published, and
// when no name could be freed it stays at `staged` too - still the person's footage, and still named for the host.
template <class HasBytes, class Remove, class Publish>
inline SaveSettlement ring_settle(const std::string& staged, bool writer_ok, HasBytes&& has_bytes, Remove&& remove,
                                  Publish&& publish) {
  SaveSettlement settlement;
  if (staged.empty()) return settlement;
  if (!has_bytes(staged)) {  // nothing was written: the empty file this save made is not a clip
    remove(staged);
    return settlement;
  }
  if (!writer_ok) {  // a truncated file is kept for recovery, never given a clip's name
    settlement.path = staged;
    settlement.kept = true;
    return settlement;
  }
  settlement.path = publish(staged);
  settlement.published = !settlement.path.empty();
  if (!settlement.published) settlement.path = staged;  // no name free: the footage stays, named, and is never deleted
  return settlement;
}

// Brings a finished file that is not in the destination folder - a packet part beside the ring's own file, which may be
// on another volume - to a staging name in that folder, so publishing it is a rename inside one folder. `staged_name
// (attempt)` builds a staging path for one attempt (the caller cleans the stem and keeps the real extension); `move(from,
// to)` never replaces, and takes MOVEFILE_COPY_ALLOWED so the volume can be crossed. Returns the staged path, or empty
// when every staged name was refused: the source then stays exactly where it was.
template <class StagedName, class Move>
inline std::string ring_stage(const std::string& source, StagedName&& staged_name, Move&& move,
                              int attempts = kStageNameAttempts) {
  if (source.empty()) return std::string();
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const std::string staged = staged_name(attempt);
    if (staged.empty()) continue;
    if (move(source, staged)) return staged;
  }
  return std::string();
}

}  // namespace bx
