// "Is this file still there", asked of many files in one folder: one directory listing answers all of them.
//
// A Timeline session's chunks all sit in one media folder, and the preview and the panel ask `timeline.get` several times a
// second. A GetFileAttributes per chunk cost about 50 microseconds each on the development machine (70 ms for the 1440 chunks
// of a two-hour session, on every call); one enumeration of the folder costs a few milliseconds whatever it holds. A name the
// listing does not hold is checked on its own before it is called missing, so a file made after the listing, or a name the
// listing spells differently, never reads as gone.
#pragma once

#include <windows.h>

#include <string>
#include <unordered_map>

namespace bx {

class DirListing {
 public:
  bool exists(const std::string& path) const {
    const size_t cut = path.find_last_of("/\\");
    if (cut == std::string::npos) return present(path);
    const Folder& folder = list(path.substr(0, cut));
    if (folder.files.count(lower(path.substr(cut + 1))) != 0) return true;
    return present(path);
  }

  // The file's size from the listing, which carries it for free, or -1 when the folder does not list the name; the caller
  // then asks the file itself.
  int64_t size(const std::string& path) const {
    const size_t cut = path.find_last_of("/\\");
    if (cut == std::string::npos) return -1;
    const Folder& folder = list(path.substr(0, cut));
    const auto found = folder.files.find(lower(path.substr(cut + 1)));
    return found == folder.files.end() ? -1 : found->second;
  }

  // The file test `timeline_playable_chunks` and `timeline_save_view` take: `DirListing()` is one listing per folder for one call.
  bool operator()(const std::string& path) const { return exists(path); }

  // How many directory listings were made: the tests hold it to one per folder.
  size_t listings() const { return folders_.size(); }

 private:
  struct Folder {
    std::unordered_map<std::string, int64_t> files;  // lower-case name -> size
  };

  static bool present(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

  static std::string lower(std::string text) {
    for (char& character : text) {
      if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return text;
  }

  const Folder& list(const std::string& directory) const {
    std::string key = lower(directory);
    for (char& character : key) {
      if (character == '/') character = '\\';
    }
    const auto found = folders_.find(key);
    if (found != folders_.end()) return found->second;
    Folder folder;
    WIN32_FIND_DATAA entry{};
    HANDLE handle = FindFirstFileExA((directory + "\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr,
                                     FIND_FIRST_EX_LARGE_FETCH);
    if (handle != INVALID_HANDLE_VALUE) {
      do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) folder.files.emplace(lower(entry.cFileName), (static_cast<int64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow);
      } while (FindNextFileA(handle, &entry));
      FindClose(handle);
    }
    return folders_.emplace(std::move(key), std::move(folder)).first->second;
  }

  mutable std::unordered_map<std::string, Folder> folders_;
};

}  // namespace bx
