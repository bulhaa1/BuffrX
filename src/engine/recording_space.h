// The free-space guard every recording start runs, on the volume the footage really lands on.
//
// Recordings and Timeline chunks live under recordings_base_dir (config.h): the person's own recordings folder when one
// is set, the clips folder otherwise. Asking the guard about clips_dir refused a recording because the *clips* drive was
// short while the recordings drive had room, and let the recordings drive fill up with nothing said - four start paths
// (the shared-encoder one, the separate-encoder fallback, Timeline's first segment and a resume) each had to get it right.
// This is the one place that chooses, so a new path cannot choose differently, and the choice is checkable without disk.
#pragma once

#include <string>

#include "config.h"
#include "session_store.h"

namespace bx {

// True when the volume recordings go to has at least `config.min_free_gb` free, else false with the words for the
// person in `reason`. `free_bytes` is the volume's free space, asked with the directory it is for.
template <class FreeBytes>
inline bool recording_space_guard(const Config& config, const FreeBytes& free_bytes, std::string& reason) {
  const std::string directory = recordings_base_dir(config);
  return recording_space_ok(directory, free_bytes(directory), config.min_free_gb, reason);
}

}  // namespace bx
