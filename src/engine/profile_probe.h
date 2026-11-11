// libobs's own profiler, as a probe.
//
// The engine's idle cost is not in the engine: the loop itself is under a millisecond a pass. It is
// in libobs's threads, and Windows will not say which. libobs's profiler does: it names every
// thread and every function it spends time in. This starts it, and later writes what it saw.
//
//   buffrx-engine.exe --profile 20
#pragma once

#include <string>

namespace bx {

// Starts the profiler. The overhead is small, and it only runs when the probe asks for it.
void profile_begin();

// Stops the profiler and writes what it saw as a CSV: one row per thread and per function, with the
// time it took. False when the file could not be written.
bool profile_end(const std::string& csv_path, std::string& error);

}  // namespace bx
