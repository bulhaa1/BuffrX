#include "update_pending.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include "../shared/files.h"
#include "update_math.h"
#include "update_download.h"

namespace bx {
namespace {

std::string lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// Windows' own resolved path, lower-cased for a comparison that does not care about the casing a person typed.
// An empty string means it could not be resolved, which the containment check treats as refused.
std::string full_path(const std::string& path) {
  if (path.empty()) return {};
  const DWORD needed = GetFullPathNameA(path.c_str(), 0, nullptr, nullptr);
  if (needed == 0) return {};
  std::string buffer(needed, '\0');
  const DWORD length = GetFullPathNameA(path.c_str(), needed, buffer.data(), nullptr);
  if (length == 0 || length >= needed) return {};
  buffer.resize(length);
  return lower(buffer);
}

bool read_text(const std::string& path, std::string& text) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  char buffer[4096];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

}  // namespace

std::string update_dir(const std::string& data_root) { return data_root + "\\update"; }
std::string update_installer_path(const std::string& data_root) { return update_dir(data_root) + "\\BuffrX-Setup.exe"; }
std::string update_pending_path(const std::string& data_root) { return update_dir(data_root) + "\\pending.json"; }
std::string update_apply_marker(const std::string& data_root) { return update_dir(data_root) + "\\apply"; }
std::string update_install_log(const std::string& data_root) { return update_dir(data_root) + "\\install.log"; }
std::string update_failure_path(const std::string& data_root) { return update_dir(data_root) + "\\failed"; }

bool update_path_is_inside(const std::string& data_root, const std::string& path) {
  if (path.empty()) return false;
  const std::string root = full_path(update_dir(data_root));
  const std::string target = full_path(path);
  if (root.empty() || target.empty()) return false;
  if (target.size() <= root.size()) return false;
  if (target.compare(0, root.size(), root) != 0) return false;
  return target[root.size()] == '\\';
}

Json update_pending_to_json(const PendingUpdate& pending) {
  Json json = Json::object();
  json.set("version", Json(pending.version));
  json.set("setup", Json(pending.setup));
  json.set("sha256", Json(pending.sha256));
  json.set("notes", Json(pending.notes));
  json.set("error", Json(pending.error));
  json.set("download_attempts", Json(pending.download_attempts));
  json.set("install_attempts", Json(pending.install_attempts));
  json.set("downloaded_ms", Json(pending.downloaded_ms));
  return json;
}

bool update_pending_from_json(const Json& json, PendingUpdate& pending) {
  if (!json.has("version")) return false;
  pending = PendingUpdate{};
  pending.version = json["version"].as_string();
  pending.setup = json["setup"].as_string();
  pending.sha256 = json["sha256"].as_string();
  pending.notes = json["notes"].as_string();
  pending.error = json["error"].as_string();
  pending.download_attempts = static_cast<int>(json["download_attempts"].as_int(0));
  pending.install_attempts = static_cast<int>(json["install_attempts"].as_int(0));
  pending.downloaded_ms = json["downloaded_ms"].as_int(0);
  return pending.ready();
}

bool update_pending_read(const std::string& data_root, PendingUpdate& pending, std::string& error) {
  std::string text;
  if (!read_text(update_pending_path(data_root), text)) {
    error = "no update has been downloaded";
    return false;
  }
  Json json;
  std::string parse_error;
  if (!Json::parse(text, json, parse_error) || !update_pending_from_json(json, pending)) {
    error = "the downloaded update could not be read";
    return false;
  }
  if (!update_path_is_inside(data_root, pending.setup)) {
    error = "the downloaded update is not where it belongs";
    return false;
  }
  return true;
}

bool update_pending_verify(const PendingUpdate& pending, std::string& error) {
  error.clear();
  const std::string hex = sha256_file(pending.setup, error);
  if (!hex.empty() && digest_text_equals(pending.sha256, hex)) return true;
  if (error.empty()) error = "the downloaded update did not check out";
  return false;
}

bool update_pending_write(const std::string& data_root, const PendingUpdate& pending) {
  CreateDirectoryA(update_dir(data_root).c_str(), nullptr);
  return write_file_atomic(update_pending_path(data_root), update_pending_to_json(pending).dump(2));
}

void update_pending_clear(const std::string& data_root) {
  DeleteFileA(update_pending_path(data_root).c_str());
  DeleteFileA(update_failure_path(data_root).c_str());
  update_drop_apply_marker(data_root);
}

bool update_marker_exists(const std::string& data_root) {
  return GetFileAttributesA(update_apply_marker(data_root).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool update_make_apply_marker(const std::string& data_root) {
  CreateDirectoryA(update_dir(data_root).c_str(), nullptr);
  const std::string path = update_apply_marker(data_root);
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  return std::fclose(file) == 0;
}

void update_drop_apply_marker(const std::string& data_root) { DeleteFileA(update_apply_marker(data_root).c_str()); }

void update_cleanup_applied(const std::string& data_root, const std::string& running_version) {
  PendingUpdate pending;
  std::string error;
  if (!update_pending_read(data_root, pending, error)) return;
  const Version running = parse_version(running_version);
  const Version pending_version = parse_version(pending.version);
  if (!running.valid || !pending_version.valid) return;
  if (compare_versions(running, pending_version) < 0) return;
  DeleteFileA(pending.setup.c_str());
  DeleteFileA(update_install_log(data_root).c_str());
  update_pending_clear(data_root);
}

}  // namespace bx
