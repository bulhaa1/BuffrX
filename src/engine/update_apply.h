// Running the downloaded installer without a wizard, and the short-lived mode the launcher starts before the
// engine. Nothing here talks to the network: the file has already been verified once, and is verified again
// here before it is executed.
#pragma once

#include <string>

namespace bx {

struct InstallOutcome {
  bool ok = false;
  std::string error;
};

// Starts a copy of the launcher that waits for this process, runs Setup, and brings BuffrX back.
InstallOutcome spawn_update_installer(const std::string& data_root, const std::string& app_root, bool autostart);

// Before normal startup: read and verify the pending update, count the attempt, and start the supervisor.
// Return 0 when it started, or 1 when nothing was applied and the engine should start normally.
int run_update_apply(const std::string& data_root, const std::string& app_root,
                     const std::string& running_version, bool autostart);

}  // namespace bx
