// libobs's own voice: its log lines reach engine.log, and its bcrash() no longer ends the engine silently.
//
// libobs's default crash handler prints to stderr and calls exit(0): when its D3D11 layer cannot rebuild the device after a
// driver reset (d3d11-rebuild.cpp bcrash "Failed to recreate D3D11"), the engine vanished with exit code 0 and an empty
// log. The handler here writes the reason, leaves a marker for the next start, brings BuffrX back when that is safe
// (driver_fault.h) and ends the process itself.
#pragma once

#include <string>

namespace bx {

// Before obs_startup. `data_root` holds driver_crash.json. Installs libobs's log and crash handlers.
void install_libobs_handlers(const std::string& data_root);

// Tick ms (GetTickCount, never 0) of the last "Device Remove/Reset!" libobs-d3d11 logged; 0 when none. Any thread.
unsigned long libobs_device_lost_at_ms();

// Whether the buffer, a recording or Timeline is wanted: read by the crash handler, which may not take the engine lock.
void libobs_set_capture_wanted(bool wanted);

// The latest "settings:" block an encoder logged (AMF, QSV and NVENC write their effective settings at init), for the encoder
// whose log header contains `encoder_name` (the video encoder's own name, "BuffrX video"). Empty when none was logged. Any thread.
std::string libobs_encoder_settings_block(const std::string& encoder_name);

// The relaunched copy's first job (--relaunch-after <pid>): wait for the dying engine to be gone, then for the driver.
void wait_for_dying_engine(unsigned long pid);

}  // namespace bx
