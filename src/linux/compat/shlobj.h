#pragma once
#include "windows.h"

// The engine only asks for the Videos known folder (the default clips root). On Linux the XDG user
// directories are the equivalent; the shim answers for the folders a desktop session normally has.
using REFKNOWNFOLDERID = const char*;
#define FOLDERID_Videos "Videos"
#define FOLDERID_Pictures "Pictures"
#define FOLDERID_Documents "Documents"
#define FOLDERID_Downloads "Downloads"
#define FOLDERID_Desktop "Desktop"
#define FOLDERID_Music "Music"

inline bool bx_xdg_folder(const char* name, std::string& out) {
  const char* home = std::getenv("HOME");
  if (!home || !*home) return false;
  out = std::string(home) + "/" + name;
  return true;
}

#include <cstdlib>
inline HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD, HANDLE, wchar_t** path) {
  if (!path) return -1;
  std::string folder;
  if (!bx_xdg_folder(id ? id : "", folder)) return -1;
  const std::wstring wide = bx_utf8_to_wide(folder);
  wchar_t* result = static_cast<wchar_t*>(std::malloc((wide.size() + 1) * sizeof(wchar_t)));
  if (!result) return -1;
  std::wmemcpy(result, wide.c_str(), wide.size() + 1);
  *path = result;
  return 0;
}
inline HRESULT SHGetFolderPathA(HWND, int, HANDLE, DWORD, LPSTR out) {
  std::string folder;
  if (!bx_xdg_folder("Videos", folder)) return -1;
  if (out) std::strcpy(out, folder.c_str());
  return 0;
}
inline void CoTaskMemFree(void* memory) { std::free(memory); }
