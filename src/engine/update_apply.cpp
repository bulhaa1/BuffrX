#include "update_apply.h"

#include <windows.h>

#include <string>

#include "update_download.h"
#include "update_math.h"
#include "update_pending.h"

namespace bx {
InstallOutcome spawn_update_installer(const std::string& data_root, const std::string& app_root, bool autostart) {
  InstallOutcome outcome;
  if (GetFileAttributesA(update_installer_path(data_root).c_str()) == INVALID_FILE_ATTRIBUTES) {
    outcome.error = "the downloaded update is gone";
    return outcome;
  }
  const std::string launcher = app_root + "\\BuffrX.exe";
  const std::string supervisor = update_dir(data_root) + "\\BuffrX-Update.exe";
  if (!CopyFileA(launcher.c_str(), supervisor.c_str(), FALSE)) {
    outcome.error = "the update could not prepare its restart";
    return outcome;
  }
  // The launcher is statically linked and can wait outside the installation directory while Setup replaces it.
  const std::string command = "\"" + supervisor + "\" --update-supervisor \"" + app_root + "\" \"" + data_root +
                              "\" " + std::to_string(GetCurrentProcessId()) + (autostart ? " 1" : " 0");
  const std::string ready_name = "Local\\BuffrX.Update.Ready." + std::to_string(GetCurrentProcessId());
  HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, ready_name.c_str());
  if (!ready) {
    outcome.error = "the update could not prepare its restart";
    return outcome;
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::string mutable_command = command;
  if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      app_root.c_str(), &startup, &process)) {
    CloseHandle(ready);
    outcome.error = "the update could not be started";
    return outcome;
  }
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(ready, 5000);
  CloseHandle(ready);
  if (wait != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    outcome.error = "the update could not prepare its restart";
    return outcome;
  }
  CloseHandle(process.hProcess);
  outcome.ok = true;
  return outcome;
}

int run_update_apply(const std::string& data_root, const std::string& app_root,
                     const std::string& running_version, bool autostart) {
  PendingUpdate pending;
  std::string error;
  if (!update_pending_read(data_root, pending, error)) {
    update_drop_apply_marker(data_root);
    return 1;
  }
  // The relaunch after a successful install runs this mode once more, on the new version: there is nothing
  // left to apply, and the marker and installer belong to the version that was just replaced.
  const Version running = parse_version(running_version);
  const Version ready = parse_version(pending.version);
  if (running.valid && ready.valid && compare_versions(running, ready) >= 0) {
    update_cleanup_applied(data_root, running_version);
    return 1;
  }
  if (update_install_exhausted(pending.install_attempts)) {
    pending.error = "the update did not install after three tries";
    update_pending_write(data_root, pending);
    update_drop_apply_marker(data_root);
    return 1;
  }
  if (!update_pending_verify(pending, error)) {
    // A file that no longer matches the release is deleted, never run.
    DeleteFileA(pending.setup.c_str());
    update_pending_clear(data_root);
    return 1;
  }
  // Counted before the run, because this process is gone by the time the installer finishes: a failed
  // install is seen by the next launch, and three of them end the attempts.
  pending.install_attempts += 1;
  if (!update_pending_write(data_root, pending)) return 1;
  const InstallOutcome started = spawn_update_installer(data_root, app_root, autostart);
  if (!started.ok) {
    pending.error = started.error;
    update_pending_write(data_root, pending);
    update_drop_apply_marker(data_root);
    return 1;
  }
  return 0;
}

}  // namespace bx
