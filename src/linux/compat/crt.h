// The small piece of MSVC's CRT vocabulary the engine uses (64-bit stdio positioning, wide fopen,
// the safe variants of localtime). Included by windows.h and force-included by the Linux build so
// translation units that never include windows.h still get it.
#pragma once

#ifndef _WIN32

#include <cstdio>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <string>
#include <cwchar>
#include <unistd.h>

namespace bx_crt {
inline std::string narrow(const wchar_t* text) {
  if (!text) return {};
  std::string out;
  out.reserve(std::wcslen(text));
  for (const wchar_t* p = text; *p; ++p) out.push_back(static_cast<char>(*p & 0x7f));
  return out;
}
}  // namespace bx_crt

inline int _fseeki64(FILE* file, long long offset, int origin) { return fseeko(file, static_cast<off_t>(offset), origin); }
inline long long _ftelli64(FILE* file) { return static_cast<long long>(ftello(file)); }
inline FILE* _wfopen(const wchar_t* path, const wchar_t* mode) {
  return std::fopen(bx_crt::narrow(path).c_str(), bx_crt::narrow(mode).c_str());
}
inline int _wremove(const wchar_t* path) { return std::remove(bx_crt::narrow(path).c_str()); }
inline int _wrename(const wchar_t* from, const wchar_t* to) {
  return std::rename(bx_crt::narrow(from).c_str(), bx_crt::narrow(to).c_str());
}
inline int _fileno(FILE* file) { return fileno(file); }
inline int _chsize_s(int fd, long long size) { return ftruncate(fd, static_cast<off_t>(size)) == 0 ? 0 : errno; }
inline int _waccess(const wchar_t* path, int mode) { return access(bx_crt::narrow(path).c_str(), mode); }
inline int localtime_s(struct tm* out, const time_t* value) { return localtime_r(value, out) ? 0 : EINVAL; }
inline int gmtime_s(struct tm* out, const time_t* value) { return gmtime_r(value, out) ? 0 : EINVAL; }
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
#define _countof(array) (sizeof(array) / sizeof((array)[0]))

#endif  // !_WIN32
