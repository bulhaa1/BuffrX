// The ring's file on a real disk (Windows). Where it may live (pure rules, tested), and the file itself: reserved once with SetEndOfFile (the
// clusters are owned, nothing is written), opened without the file cache so the footage takes no extra RAM, with one 8 MB block moved per call,
// and removed by Windows when the handle closes - on a clean stop and on a crash alike. See ring_spill.h for what is written and why.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "ring_spill.h"

namespace bx {

// ---------------------------------------------------------------- where the file may live (pure)

struct SpillFolderFacts {
  std::string filesystem;  // "NTFS", "ReFS", "FAT32", ...
  bool network = false;
  bool cloud_synced = false;
  bool compressed_or_encrypted = false;
  int64_t free_bytes = 0;
};

// Why a folder cannot hold the file, or "" when it can. FAT32 would write the whole reservation as zeros and stops at 4 GB; a network share or a
// compressed or encrypted folder cannot do unbuffered, sequential 8 MB writes; and a multi-gigabyte file that changes every second must never
// sit in a folder a sync client uploads. The free space has to cover the file and leave a gigabyte besides.
inline std::string spill_folder_problem(const SpillFolderFacts& facts, int64_t needed_bytes) {
  auto same = [](std::string a, const char* b) {
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return a == b;
  };
#ifdef _WIN32
  if (!same(facts.filesystem, "NTFS") && !same(facts.filesystem, "REFS")) return "the drive is " + (facts.filesystem.empty() ? std::string("not NTFS or ReFS") : facts.filesystem + ", not NTFS or ReFS");
#else
  // A local POSIX filesystem needs no NTFS feature for a sparse file. The network and cloud checks
  // below are the only reasons to refuse a Linux folder.
#endif
  if (facts.network) return "the folder is on a network drive";
  if (facts.cloud_synced) return "the folder is synced to the cloud";
  if (facts.compressed_or_encrypted) return "the folder is compressed or encrypted";
  const int64_t gib = 1024LL * 1024 * 1024;
  if (facts.free_bytes < needed_bytes + gib) {
    return "only " + std::to_string(facts.free_bytes / (1024 * 1024)) + " MB are free and " + std::to_string((needed_bytes + gib) / (1024 * 1024)) + " MB are needed";
  }
  return std::string();
}

// A path under a cloud-sync client's folder: under one of `roots` (the sync clients' own variables), or with a folder named like one of them.
inline bool path_in_cloud_sync(const std::string& path, const std::vector<std::string>& roots) {
  auto normal = [](std::string text) {
    for (char& c : text) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!text.empty() && text.back() == '\\') text.pop_back();
    return text;
  };
  const std::string where = normal(path);
  for (const std::string& root : roots) {
    const std::string base = normal(root);
    if (!base.empty() && (where == base || where.compare(0, base.size() + 1, base + "\\") == 0)) return true;
  }
  size_t start = 0;
  while (start <= where.size()) {
    size_t end = where.find('\\', start);
    if (end == std::string::npos) end = where.size();
    const std::string part = where.substr(start, end - start);
    if (part.compare(0, 8, "onedrive") == 0 || part == "dropbox" || part == "google drive" || part == "iclouddrive") return true;
    start = end + 1;
  }
  return false;
}

// ---------------------------------------------------------------- the file (Windows)

// A page-aligned block of memory for the unbuffered calls; null when it cannot be had.
uint8_t* alloc_aligned_block();
void free_aligned_block(uint8_t* block);

// The folder and file name to use, or an empty `path` and the reason none could be. Tries `<clips_dir>\.buffrx` first (the drive the person chose
// for big files; the library skips dot folders), then `<data_root>\ring`. `pid` goes into the name so two engines on one folder keep apart.
struct SpillPlacement {
  std::string path;     // UTF-8, empty when no folder would do
  std::string problem;  // why, for the log and the notice
};
SpillPlacement place_spill_file(const std::string& clips_dir, const std::string& data_root, int64_t needed_bytes, uint32_t pid);

// Deletes `replay-*.ring` files in `folder` that no engine holds open (left by a power cut). A live engine's file is open exclusively, so
// Windows refuses to delete it.
void remove_stale_ring_files(const std::string& folder);

// The process id in the name of a part of a save in parts, `part-<pid>-<n>.<ext>`; 0 for any other name. The same call removes the parts of engines that
// are no longer running.
inline uint32_t save_part_pid(const std::string& name) {
  if (name.compare(0, 5, "part-") != 0) return 0;
  size_t at = 5;
  uint64_t pid = 0;
  size_t digits = 0;
  while (at < name.size() && name[at] >= '0' && name[at] <= '9' && digits < 10) {
    pid = pid * 10 + static_cast<uint64_t>(name[at] - '0');
    ++at;
    ++digits;
  }
  return digits > 0 && at < name.size() && name[at] == '-' && pid <= 0xFFFFFFFFull ? static_cast<uint32_t>(pid) : 0;
}

class RingFile : public SpillBackend {
public:
  ~RingFile() override { close(); }

  // Creates the file `bytes` long (a multiple of the block size). Nothing is written. False, with `error` set, when it cannot be had.
  bool open(const std::string& path_utf8, int64_t bytes, std::string& error);
  bool is_open() const { return handle_ != nullptr; }

  bool write_block(int64_t logical_offset, const uint8_t* data) override;
  bool read_block(int64_t logical_offset, uint8_t* destination) override;

  // Ends every call in flight (and refuses new ones), so a disk that has stopped answering cannot hold up a rebuild or a quit.
  void cancel();
  // Closes the handle; Windows deletes the file. Nothing may be using it.
  void close();

  int64_t bytes_written() const { return written_.load(); }
  int64_t size() const { return size_; }

private:
  bool transfer(bool write, int64_t logical_offset, uint8_t* buffer);

  void* handle_ = nullptr;
  void* write_event_ = nullptr;
  void* read_event_ = nullptr;
  int64_t size_ = 0;
  std::atomic<bool> cancelled_{false};
  std::atomic<int64_t> written_{0};
};

}  // namespace bx
