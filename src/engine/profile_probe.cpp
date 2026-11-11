#include "profile_probe.h"

#include <util/profiler.h>

#include <cstdio>

namespace bx {

void profile_begin() { profiler_start(); }

bool profile_end(const std::string& csv_path, std::string& error) {
  profiler_stop();
  profiler_snapshot_t* snapshot = profile_snapshot_create();
  if (!snapshot) {
    error = "the profile could not be read";
    return false;
  }
  const bool written = profiler_snapshot_dump_csv(snapshot, csv_path.c_str());
  profile_snapshot_free(snapshot);
  if (!written) {
    error = "the profile could not be written to " + csv_path;
    return false;
  }
  return true;
}

}  // namespace bx
