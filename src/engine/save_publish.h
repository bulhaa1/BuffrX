// How a finished copy reaches the name the library lists without either side losing a clip (commands.cpp's
// Timeline save). The copy is joined into a staging file of the save's own, and only a rename that never
// replaces publishes it: a name another writer took in the meantime refuses the rename, that writer keeps
// every byte, and the save tries the next configured name.
//
// A save used to claim a final name, stage beside it under `<name>.part.<ext>`, delete whatever was at that
// staging name, and move with no replacement in one try. A replay save or any other program that took the
// final name between the claim and the move made the move fail, and the verified staging file was then
// deleted: the footage was gone for a name that was never this save's. Nothing here deletes a file it did
// not create, and a staging name that is still taken is never opened over.
//
// The staging name is claimed by creating it, with the sidecars a join child writes beside it (claim_staging_path),
// because a name only checked for existence can be taken by another writer before the child truncates it. The final
// name is claimed at the moment of the move and only a name whose claim succeeded is ever released (claim_save_name);
// running out of names names no file, and the staged bytes stay named for the person.
//
// No Windows header: the file system calls are the caller's, so the rules are checked against a real folder
// (tests/save_publish_tests.cpp) and against a refusal a real disk cannot produce on demand.
#pragma once

#include <string>
#include <vector>

namespace bx {

// How many names one publication tries. A refused name belongs to another writer's file and is never
// written; running out is a failure the caller hears about with the footage still on disk.
constexpr int kSavePublishAttempts = 8;

// How many final names one save may try to hold. More than a publication tries, because the counter steps past
// every name this process's other saves hold; running out is a failure that names no file.
constexpr int kSaveClaimAttempts = 1000;

struct SavePublishResult {
  std::string path;         // the name it was published under; empty when none was free
  unsigned long error = 0;  // GetLastError of the last refusal, for the log line
};

// Publishes `staged` under the first name `next_name()` hands out that `move` accepts. `next_name()` is the
// next configured name free on disk, one counter step further each call (reserve_output_path's answer, held
// against other saves in flight); `move(from, to, error)` renames without replacing and returns false when
// a file is already at `to`. A refused name leaves that writer's file and `staged` exactly as they were.
template <class NextName, class Move>
SavePublishResult publish_staged_clip(const std::string& staged, NextName&& next_name, Move&& move,
                                      int attempts = kSavePublishAttempts) {
  SavePublishResult result;
  if (staged.empty()) return result;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const std::string target = next_name();
    if (target.empty()) break;
    unsigned long error = 0;
    if (move(staged, target, &error)) {
      result.path = target;
      return result;
    }
    result.error = error;
  }
  return result;
}

// The stem one save stages under: the configured stem, the marker every listing of the library drops (".part",
// so a copy being written is never listed), and this process plus this save's own number, so a part an earlier
// save left behind is never taken for this one's. The caller keeps the folder and the container's real
// extension: the join and the muxer pick the format from the path.
inline std::string save_staging_stem(const std::string& stem, unsigned long process_id, unsigned long long sequence) {
  return stem + ".part.timeline-" + std::to_string(process_id) + "-" + std::to_string(sequence);
}

// Claims the whole set of files one staged copy writes: the copy itself, then the sidecars a join child writes
// beside it. `create_new(path)` creates a file only when nothing is there (CreateFileA with CREATE_NEW) and
// answers false when something is; `remove(path)` deletes what this attempt made. A file of the set someone else
// has already made refuses the attempt, and only the files this attempt made are removed again: a foreign copy or
// sidecar is never opened over, and the cleanup that follows never deletes one.
//
// Reserving a name by asking whether it exists is not a claim: another writer can create that exact name before
// the join truncates it, and the cleanup would then take the other writer's file. The create is the claim.
template <class CreateNew, class Remove>
bool claim_staging_files(const std::vector<std::string>& paths, CreateNew&& create_new, Remove&& remove) {
  size_t made = 0;
  while (made < paths.size() && !paths[made].empty() && create_new(paths[made])) ++made;
  if (made == paths.size()) return true;
  for (size_t index = 0; index < made; ++index) remove(paths[index]);
  return false;
}

// The staging path this save owns: `next_paths()` hands out the next attempt's whole set (the copy first, then the
// sidecars a join child writes beside it), one counter step further each call, and the first set this save made
// every file of is its own. Empty means no attempt could be claimed: nothing was written, nothing is left behind,
// and the caller fails with its own sentence.
template <class NextPaths, class CreateNew, class Remove>
std::string claim_staging_path(NextPaths&& next_paths, CreateNew&& create_new, Remove&& remove,
                               int attempts = kSavePublishAttempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const std::vector<std::string> paths = next_paths();
    if (paths.empty() || paths.front().empty()) return std::string();
    if (claim_staging_files(paths, create_new, remove)) return paths.front();
  }
  return std::string();
}

// The stem one attempt at a final name asks for: the configured stem, then " (1)", " (2)" beside it, the same
// story reserve_output_path tells a name already on disk.
inline std::string save_name_attempt(const std::string& stem, int attempt) {
  if (attempt <= 0) return stem;
  return stem + " (" + std::to_string(attempt) + ")";
}

// The final name this save takes at the moment of its move, or empty when no name could be held. `free_name(stem)`
// is the next configured name free on disk; `hold(path)` records the name as this save's own and answers false
// when another save in flight already holds it. Only a name whose hold succeeded is ever released, so a claim's
// destructor cannot free another save's name. Running out of attempts is a failure that names no file: the loop it
// replaces handed the last refused name back anyway, and that save's cleanup then erased a name it never held.
template <class FreeName, class Hold>
std::string claim_save_name(const std::string& stem, FreeName&& free_name, Hold&& hold,
                            int attempts = kSaveClaimAttempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const std::string path = free_name(save_name_attempt(stem, attempt));
    if (path.empty()) return std::string();
    if (hold(path)) return path;
  }
  return std::string();
}

}  // namespace bx
