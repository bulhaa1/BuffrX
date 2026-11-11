// Replaces libobs's frame wait (os_sleepto_ns) in the loaded obs.dll with one that sleeps instead of spinning. libobs calls it
// inside the DLL, so only an in-place patch of the function reaches it. On by default since 2026-11-02 (BUFFRX_NO_SLEEP_PATCH=1 turns it
// off; see sleep_patch.cpp). Guarded two more ways: the function must still start with the bytes
// recorded from the pinned OBS 33.0.1 obs.dll (a different build is left alone and logged), and the machine must give a
// high-resolution timer. Install it after obs_startup and before the first obs_reset_video. BUFFRX_SLEEP_TRACE=1 records what the
// wait did (one info line every 2 s) without changing it.
#pragma once

#include <string>

namespace bx {

// True when the replacement is in place. `note` says why not (logged once by the caller).
bool install_sleep_patch(std::string& note);

}  // namespace bx
