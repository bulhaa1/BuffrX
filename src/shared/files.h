// One atomic file write, shared by every place in the engine that replaces a file by writing a copy
// beside it and moving that copy into place.
#pragma once

#include <windows.h>

#include <cstdio>
#include <string>

namespace bx {

// A path with Windows' own separator everywhere. The engine mixes the two: libobs builds `/` paths and the
// library joins its own with `\`, so a clip's path reaches a shell API as `C:/…/clips\Display Capture\x.mp4`.
// Explorer and ShellExecute accept that, but SHCreateItemFromParsingName - the Recycle Bin's way in - refuses
// it with E_INVALIDARG, which is what made "delete" fail and "copy" put an odd path on the clipboard.
// A `\\server\share` (UNC) path keeps its leading pair: only single separators are rewritten.
inline std::string native_path(std::string path) {
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] != '/') continue;
    const bool unc_head = i == 0 && path.size() > 1 && path[1] == '/';
    path[i] = '\\';
    if (unc_head) {
      path[1] = '\\';
      i = 1;
    }
  }
  return path;
}

// The engine's text is UTF-8, so a byte-by-byte widening would mangle any name outside ASCII. One rule for
// the whole boundary: a path handed to Win32, a Win32 string read back, a page's message.
inline std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return std::wstring();
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

inline std::string wide_to_utf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) return std::string();
  std::string out(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
  return out;
}

// The same path as Windows' own UTF-16, for the APIs that take a wide string (WIC, the DLL directory).
inline std::wstring wide_path(const std::string& path) { return utf8_to_wide(path); }

// Writes `text` to `path` through a unique temporary beside it, then moves it into place. Returns false
// when the temporary could not be made, the write came up short (a full disk) or the move failed; the
// temporary is deleted and whatever was at `path` is left untouched. Callers say why in their own words.
inline bool write_file_atomic(const std::string& path, const std::string& text) {
  // One temporary name per write: two writers of the same file (the tick publishing a session's index,
  // a retention sweep rewriting it after a delete) each opened `<path>.tmp`, so one truncated the
  // other's half-written file and the last move put that half in place.
  static volatile LONG sequence = 0;
  const std::string temporary = path + "." + std::to_string(GetCurrentProcessId()) + "." +
                                std::to_string(static_cast<unsigned long>(InterlockedIncrement(&sequence))) +
                                ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return false;
  const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  const bool flushed = std::fflush(file) == 0;
  if (std::fclose(file) != 0 || !written || !flushed) {
    DeleteFileA(temporary.c_str());
    return false;
  }
  // The move is retried for a moment, because a reader holding the file (a listing, a preview) refuses
  // it while it is open.
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (attempt > 0) Sleep(20);
    if (MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      return true;
    }
  }
  DeleteFileA(temporary.c_str());
  return false;
}

}  // namespace bx
