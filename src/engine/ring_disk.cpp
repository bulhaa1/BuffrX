#include "ring_disk.h"

#include <windows.h>

#include "../shared/files.h"
#include "../shared/log.h"

namespace bx {

namespace {

constexpr DWORD kPollMs = 250;  // how often a call in flight looks for a cancel

std::vector<std::string> cloud_roots() {
  std::vector<std::string> roots;
  for (const char* name : {"OneDrive", "OneDriveConsumer", "OneDriveCommercial"}) {
    char value[1024];
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length > 0 && length < sizeof(value)) roots.push_back(value);
  }
  return roots;
}

// A folder that may not exist yet is judged by its nearest parent that does.
std::wstring existing_ancestor(std::wstring folder) {
  while (!folder.empty() && GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const size_t slash = folder.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    folder.resize(slash);
  }
  return folder;
}

SpillFolderFacts facts_for(const std::wstring& folder) {
  SpillFolderFacts facts;
  const std::wstring probe = existing_ancestor(folder);
  if (probe.empty()) return facts;
  wchar_t root[MAX_PATH] = {0};
  if (GetVolumePathNameW(probe.c_str(), root, MAX_PATH)) {
    wchar_t filesystem[MAX_PATH] = {0};
    DWORD flags = 0;
    if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, &flags, filesystem, MAX_PATH)) facts.filesystem = wide_to_utf8(filesystem);
    facts.network = GetDriveTypeW(root) == DRIVE_REMOTE;
  }
  if (folder.compare(0, 2, L"\\\\") == 0 && folder.compare(0, 4, L"\\\\?\\") != 0) facts.network = true;
  facts.cloud_synced = path_in_cloud_sync(wide_to_utf8(folder), cloud_roots());
  const DWORD attributes = GetFileAttributesW(probe.c_str());
  facts.compressed_or_encrypted = attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED)) != 0;
  ULARGE_INTEGER free_to_caller{};
  if (GetDiskFreeSpaceExW(probe.c_str(), &free_to_caller, nullptr, nullptr)) facts.free_bytes = static_cast<int64_t>(free_to_caller.QuadPart);
  return facts;
}

bool make_folder(const std::wstring& folder) {
  if (GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
  const size_t slash = folder.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash > 2 && !make_folder(folder.substr(0, slash))) return false;
  return CreateDirectoryW(folder.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

uint8_t* alloc_aligned_block() {
  return static_cast<uint8_t*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(kSpillBlockBytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

void free_aligned_block(uint8_t* block) {
  if (block) VirtualFree(block, 0, MEM_RELEASE);
}

void remove_stale_ring_files(const std::string& folder) {
  const std::wstring base = wide_path(folder);
  WIN32_FIND_DATAW entry{};
  HANDLE find = FindFirstFileW((base + L"\\replay-*.ring").c_str(), &entry);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (DeleteFileW((base + L"\\" + entry.cFileName).c_str())) BX_LOG_INFO("removed a ring file left by an engine that did not close: %s", wide_to_utf8(entry.cFileName).c_str());
    } while (FindNextFileW(find, &entry));
    FindClose(find);
  }
  // The parts of a save in parts (Timeline dedupe), `part-<pid>-<n>.<ext>`, are deleted by the save that made them. One left behind belongs to an engine that
  // died mid-save; the part of an engine that is still running is not touched.
  find = FindFirstFileW((base + L"\\part-*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const uint32_t pid = save_part_pid(wide_to_utf8(entry.cFileName));
    if (pid == 0) continue;
    HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, pid);
    const bool running = owner != nullptr && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT;
    if (owner) CloseHandle(owner);
    if (running) continue;
    if (DeleteFileW((base + L"\\" + entry.cFileName).c_str())) BX_LOG_INFO("removed a part of a save left by an engine that did not close: %s", wide_to_utf8(entry.cFileName).c_str());
  } while (FindNextFileW(find, &entry));
  FindClose(find);
}

SpillPlacement place_spill_file(const std::string& clips_dir, const std::string& data_root, int64_t needed_bytes, uint32_t pid) {
  SpillPlacement placement;
  const std::string candidates[2] = {clips_dir.empty() ? std::string() : clips_dir + "\\.buffrx", data_root.empty() ? std::string() : data_root + "\\ring"};
  for (const std::string& folder : candidates) {
    if (folder.empty()) continue;
    const std::wstring wide = wide_path(folder);
    const std::string problem = spill_folder_problem(facts_for(wide), needed_bytes);
    if (!problem.empty()) {
      placement.problem += (placement.problem.empty() ? "" : "; ") + folder + ": " + problem;
      continue;
    }
    if (!make_folder(wide)) {
      placement.problem += (placement.problem.empty() ? "" : "; ") + folder + ": the folder could not be made";
      continue;
    }
    SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_HIDDEN);
    remove_stale_ring_files(folder);
    placement.path = folder + "\\replay-" + std::to_string(pid) + ".ring";
    placement.problem.clear();
    return placement;
  }
  return placement;
}

bool RingFile::open(const std::string& path_utf8, int64_t bytes, std::string& error) {
  close();
  written_ = 0;  // a RingFile is made for one ring start: a cancel that came before this open stays
  if (bytes <= 0 || bytes % kSpillBlockBytes != 0) {
    error = "the ring file is not a whole number of blocks long";
    return false;
  }
  // Exclusive (no sharing), unbuffered so the footage takes no room in the file cache, overlapped so a stuck disk can be cancelled, and deleted by
  // Windows when the handle closes, whether the engine closes it or dies. No write-through: the file is disposable, so nothing needs to be durable.
  HANDLE file = CreateFileW(wide_path(path_utf8).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = "the ring file could not be created (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  // The space is taken now and nothing is written: the clusters are allocated, the data length stays zero.
  LARGE_INTEGER end{};
  end.QuadPart = bytes;
  if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
    const DWORD code = GetLastError();
    CloseHandle(file);
    error = code == ERROR_DISK_FULL ? "the disk is full" : "the ring file could not be reserved (Windows error " + std::to_string(code) + ")";
    return false;
  }
  handle_ = file;
  write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  size_ = bytes;
  if (!write_event_ || !read_event_) {
    error = "the ring file could not make its events";
    close();
    return false;
  }
  return true;
}

bool RingFile::transfer(bool write, int64_t logical_offset, uint8_t* buffer) {
  if (!handle_ || cancelled_ || size_ <= 0) return false;
  HANDLE event = write ? write_event_ : read_event_;
  const int64_t physical = logical_offset % size_;
  OVERLAPPED overlapped{};
  overlapped.hEvent = event;
  overlapped.Offset = static_cast<DWORD>(physical & 0xffffffffLL);
  overlapped.OffsetHigh = static_cast<DWORD>(physical >> 32);
  ResetEvent(event);
  const BOOL started = write ? WriteFile(handle_, buffer, static_cast<DWORD>(kSpillBlockBytes), nullptr, &overlapped)
                             : ReadFile(handle_, buffer, static_cast<DWORD>(kSpillBlockBytes), nullptr, &overlapped);
  if (!started && GetLastError() != ERROR_IO_PENDING) return false;
  DWORD done = 0;
  while (WaitForSingleObject(event, kPollMs) == WAIT_TIMEOUT) {
    if (cancelled_) {
      CancelIoEx(handle_, &overlapped);
      GetOverlappedResult(handle_, &overlapped, &done, TRUE);
      return false;
    }
  }
  if (!GetOverlappedResult(handle_, &overlapped, &done, FALSE)) return false;
  if (write) written_ += done;
  return done == static_cast<DWORD>(kSpillBlockBytes);
}

bool RingFile::write_block(int64_t logical_offset, const uint8_t* data) { return transfer(true, logical_offset, const_cast<uint8_t*>(data)); }

bool RingFile::read_block(int64_t logical_offset, uint8_t* destination) { return transfer(false, logical_offset, destination); }

void RingFile::cancel() {
  cancelled_ = true;
  if (handle_) CancelIoEx(handle_, nullptr);
}

void RingFile::close() {
  if (handle_) CloseHandle(handle_);
  if (write_event_) CloseHandle(write_event_);
  if (read_event_) CloseHandle(read_event_);
  handle_ = write_event_ = read_event_ = nullptr;
  size_ = 0;
}

}  // namespace bx
