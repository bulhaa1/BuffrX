// The update this PC has downloaded and will install: one small JSON file under <data root>\update, plus a
// marker the launcher reads before it starts the engine. The launcher has no JSON reader, so the marker is
// an empty file whose existence alone says "apply"; everything else is the engine's.
#pragma once

#include <cstdint>
#include <string>

#include "../shared/json.h"

namespace bx {

struct PendingUpdate {
  std::string version;   // the release tag, as published
  std::string setup;     // the verified installer, always inside update_dir()
  std::string sha256;    // lower-case hex, as downloaded
  std::string notes;     // the release body, kept for the sheet after the restart
  std::string error;     // why the last attempt failed, shown in Settings
  int download_attempts = 0;
  int install_attempts = 0;
  int64_t downloaded_ms = 0;

  bool ready() const { return !version.empty() && !setup.empty() && !sha256.empty(); }
};

std::string update_dir(const std::string& data_root);
std::string update_installer_path(const std::string& data_root);
std::string update_pending_path(const std::string& data_root);
std::string update_apply_marker(const std::string& data_root);
std::string update_install_log(const std::string& data_root);
std::string update_failure_path(const std::string& data_root);

// A path a launcher or an apply run is allowed to execute: inside <data root>\update after Windows has
// normalised it, so "./", ".." and a symlink-looking string cannot name something else.
bool update_path_is_inside(const std::string& data_root, const std::string& path);

Json update_pending_to_json(const PendingUpdate& pending);
bool update_pending_from_json(const Json& json, PendingUpdate& pending);
bool update_pending_read(const std::string& data_root, PendingUpdate& pending, std::string& error);
bool update_pending_verify(const PendingUpdate& pending, std::string& error);
bool update_pending_write(const std::string& data_root, const PendingUpdate& pending);
void update_pending_clear(const std::string& data_root);

bool update_marker_exists(const std::string& data_root);
bool update_make_apply_marker(const std::string& data_root);
void update_drop_apply_marker(const std::string& data_root);

// After a run on a version at or past the pending one, the installer, its state and its log are no longer
// needed. A failed update (running older than pending) keeps them, so the next start can try again.
void update_cleanup_applied(const std::string& data_root, const std::string& running_version);

}  // namespace bx
