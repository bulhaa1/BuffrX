// POSIX compatibility layer for the BuffrX engine.
//
// The engine is written against Win32 for files, processes, threads, events and paths. On Linux those
// calls are implemented here on top of POSIX APIs, so the portable majority of the engine keeps its
// source shape. Anything that is genuinely Windows-only (D3D11, WebView2, WASAPI, DXGI, WIC, the
// registry, the tray) lives in a Linux replacement file instead.
#pragma once

#ifndef _WIN32

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <limits.h>
#include <strings.h>
#include <glob.h>
#include "crt.h"
#include <fnmatch.h>
#include <wchar.h>
#include <cwchar>

// ---------------------------------------------------------------- scalar types

using BOOL = int;
using BYTE = unsigned char;
using WORD = unsigned short;
using DWORD = uint32_t;
using LONG = int32_t;
using ULONG = uint32_t;
using UINT = unsigned int;
using INT = int;
using UINT64 = uint64_t;
using INT64 = int64_t;
using ULONGLONG = unsigned long long;
using LONGLONG = long long;
using LONG64 = int64_t;
using DWORD_PTR = uintptr_t;
using WCHAR = wchar_t;
using LPSTR = char*;
using LPCSTR = const char*;
using LPWSTR = wchar_t*;
using PWSTR = wchar_t*;
using PCWSTR = const wchar_t*;
using LPCWSTR = const wchar_t*;
using LPVOID = void*;
using HRESULT = int32_t;
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
using LPTHREAD_START_ROUTINE = DWORD (*)(LPVOID);
using PVOID = void*;
using LPCVOID = const void*;
using SIZE_T = size_t;
using VOID = void;
#define WINAPI
#define CALLBACK
using HANDLE = void*;
using HMODULE = void*;
using HINSTANCE = void*;
using HWND = void*;
using HICON = void*;
using HFONT = void*;
using HMONITOR = void*;
using HBRUSH = void*;
using HCURSOR = void*;
using HDC = void*;
using HKEY = void*;
using HLOCAL = void*;
using HGLOBAL = void*;
using FARPROC = void*;
using COLORREF = unsigned int;
using ATOM = unsigned short;
using UINT_PTR = uintptr_t;
using LONG_PTR = intptr_t;
using ULONG_PTR = uintptr_t;
using LPARAM = intptr_t;
using INT_PTR = intptr_t;
using WPARAM = uintptr_t;
using LRESULT = intptr_t;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1
#define UNICODE 1
#define _UNICODE 1
#define MAX_PATH 260
#define MAX_PATHW 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFFu
#define CP_UTF8 65001
#define CP_ACP 0
#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_CANCELLED 1223L
#define ERROR_PIPE_BUSY 231L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_DISK_FULL 112L
#define ERROR_IO_PENDING 997L
#define ERROR_NO_MORE_FILES 18L
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_READ_ATTRIBUTES 0x80
#define FILE_ATTRIBUTE_READONLY 0x01
#define FILE_ATTRIBUTE_HIDDEN 0x02
#define FILE_ATTRIBUTE_SYSTEM 0x04
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_UNKNOWN 0
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x2000
#define FILE_ATTRIBUTE_COMPRESSED 0x800
#define FILE_ATTRIBUTE_ENCRYPTED 0x4000
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002
#define DRIVE_UNKNOWN 0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE 2
#define DRIVE_FIXED 3
#define DRIVE_REMOTE 4
#define DRIVE_CDROM 5
#define DRIVE_RAMDISK 6
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define DELETE 0x00010000L
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define CREATE_NEW 1
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 0x1
#define MOVEFILE_COPY_ALLOWED 0x2
#define MOVEFILE_WRITE_THROUGH 0x8
#define CREATE_NO_WINDOW 0x08000000
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#define STARTF_USESTDHANDLES 0x00000100
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define CREATE_UNICODE_ENVIRONMENT 0x00000400
#define STARTF_USESHOWWINDOW 0x00000001
#define SW_HIDE 0
#define SW_SHOW 5
#define SW_SHOWNORMAL 1
#define SW_RESTORE 9
#define SW_MINIMIZE 6
#define SW_MAXIMIZE 3
#define STILL_ACTIVE 259
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_BACKGROUND 0x0010
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#define SYNCHRONIZE 0x00100000L
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_VM_READ 0x0010
#define LIST_MODULES_ALL 0x03
#define MB_OK 0
#define MB_YESNO 4
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define MB_DEFBUTTON2 0x100
#define MB_TOPMOST 0x40000
#define IDYES 6
#define IDNO 7
#define IDOK 1
#define IDCANCEL 2
#define ALL_PROCESSOR_GROUPS 0xFFFF
#define ASFW_ANY ((DWORD)-1)
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define WM_QUIT 0x0012
#define WM_CLOSE 0x0010
#define WM_DESTROY 0x0002
#define WM_ENDSESSION 0x0016
#define WM_APP 0x8000
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((void*)-4)
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define FILE_MAP_READ 4
#define PAGE_READONLY 2
#define PAGE_READWRITE 4
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_DECOMMIT 0x4000
#define DUPLICATE_SAME_ACCESS 2
#define MOVEFILE_FAIL_IF_NOT_TRACKABLE 0x20
#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SE_ERR_ACCESSDENIED 5
#define SE_ERR_NOASSOC 31

// Windows uses backslash; the engine produces both. native_path() in shared/files.h normalises where
// it matters. On Linux a backslash is a legal file-name character, so translation happens at the
// filesystem boundary in this layer only.
inline std::string bx_slashify(const std::string& value) {
  std::string out = value;
  for (char& c : out)
    if (c == '\\') c = '/';
  return out;
}

// ---------------------------------------------------------------- structs

union ULARGE_INTEGER {
  struct {
    DWORD LowPart;
    DWORD HighPart;
  };
  ULONGLONG QuadPart;
};

union LARGE_INTEGER {
  struct {
    DWORD LowPart;
    LONG HighPart;
  };
  LONGLONG QuadPart;
};

struct RECT {
  LONG left = 0, top = 0, right = 0, bottom = 0;
};
using LPRECT = RECT*;
struct MONITORINFO {
  DWORD cbSize = sizeof(MONITORINFO);
  RECT rcMonitor{};
  RECT rcWork{};
  DWORD dwFlags = 0;
};
using LPMONITORINFO = MONITORINFO*;
struct MONITORINFOEXA : MONITORINFO {
  char szDevice[32] = {};
};
struct MONITORINFOEXW : MONITORINFO {
  wchar_t szDevice[32] = {};
};
#define MONITOR_DEFAULTTONULL 0
#define MONITOR_DEFAULTTOPRIMARY 1
#define MONITOR_DEFAULTTONEAREST 2
struct POINT {
  LONG x = 0, y = 0;
};
struct SIZE {
  LONG cx = 0, cy = 0;
};
struct FILETIME {
  DWORD dwLowDateTime = 0;
  DWORD dwHighDateTime = 0;
};
struct SYSTEMTIME {
  WORD wYear = 0, wMonth = 0, wDayOfWeek = 0, wDay = 0, wHour = 0, wMinute = 0, wSecond = 0, wMilliseconds = 0;
};
struct SECURITY_ATTRIBUTES {
  DWORD nLength = 0;
  LPVOID lpSecurityDescriptor = nullptr;
  BOOL bInheritHandle = FALSE;
};
struct STARTUPINFOA {
  DWORD cb = 0;
  LPSTR lpReserved = nullptr, lpDesktop = nullptr, lpTitle = nullptr;
  DWORD dwX = 0, dwY = 0, dwXSize = 0, dwYSize = 0, dwXCountChars = 0, dwYCountChars = 0, dwFillAttribute = 0, dwFlags = 0;
  WORD wShowWindow = 0, cbReserved2 = 0;
  BYTE* lpReserved2 = nullptr;
  HANDLE hStdInput = nullptr, hStdOutput = nullptr, hStdError = nullptr;
};
struct STARTUPINFOW : STARTUPINFOA {};
struct PROCESS_INFORMATION {
  HANDLE hProcess = nullptr;
  HANDLE hThread = nullptr;
  DWORD dwProcessId = 0;
  DWORD dwThreadId = 0;
};
struct WIN32_FILE_ATTRIBUTE_DATA {
  DWORD dwFileAttributes = 0;
  FILETIME ftCreationTime{}, ftLastAccessTime{}, ftLastWriteTime{};
  DWORD nFileSizeHigh = 0, nFileSizeLow = 0;
};
struct WIN32_FIND_DATAA {
  DWORD dwFileAttributes = 0;
  FILETIME ftCreationTime{}, ftLastAccessTime{}, ftLastWriteTime{};
  DWORD nFileSizeHigh = 0, nFileSizeLow = 0;
  DWORD dwReserved0 = 0, dwReserved1 = 0;
  char cFileName[MAX_PATH] = {};
  char cAlternateFileName[14] = {};
};
using WIN32_FIND_DATA = WIN32_FIND_DATAA;
struct WIN32_FIND_DATAW {
  DWORD dwFileAttributes = 0;
  FILETIME ftCreationTime{}, ftLastAccessTime{}, ftLastWriteTime{};
  DWORD nFileSizeHigh = 0, nFileSizeLow = 0;
  DWORD dwReserved0 = 0, dwReserved1 = 0;
  WCHAR cFileName[MAX_PATH] = {};
  WCHAR cAlternateFileName[14] = {};
};
struct MEMORYSTATUSEX {
  DWORD dwLength = sizeof(MEMORYSTATUSEX);
  DWORD dwMemoryLoad = 0;
  ULONGLONG ullTotalPhys = 0, ullAvailPhys = 0, ullTotalPageFile = 0, ullAvailPageFile = 0;
  ULONGLONG ullTotalVirtual = 0, ullAvailVirtual = 0, ullAvailExtendedVirtual = 0;
};
struct PROCESS_MEMORY_COUNTERS {
  DWORD cb = sizeof(PROCESS_MEMORY_COUNTERS);
  DWORD PageFaultCount = 0;
  SIZE_T PeakWorkingSetSize = 0, WorkingSetSize = 0, QuotaPeakPagedPoolUsage = 0, QuotaPagedPoolUsage = 0;
  SIZE_T QuotaPeakNonPagedPoolUsage = 0, QuotaNonPagedPoolUsage = 0, PagefileUsage = 0, PeakPagefileUsage = 0;
};
struct DEVMODEA {
  char dmDeviceName[32] = {};
  WORD dmSpecVersion = 0, dmDriverVersion = 0;
  WORD dmSize = sizeof(DEVMODEA);
  WORD dmDriverExtra = 0;
  DWORD dmFields = 0;
  int dmPositionX = 0, dmPositionY = 0;
  DWORD dmDisplayOrientation = 0, dmDisplayFixedOutput = 0;
  short dmColor = 0, dmDuplex = 0, dmYResolution = 0, dmTTOption = 0, dmCollate = 0;
  char dmFormName[32] = {};
  WORD dmLogPixels = 0;
  DWORD dmBitsPerPel = 0, dmPelsWidth = 0, dmPelsHeight = 0, dmDisplayFlags = 0, dmDisplayFrequency = 0;
};
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
struct SYSTEM_INFO {
  WORD wProcessorArchitecture = 0, wReserved = 0;
  DWORD dwPageSize = 4096;
  LPVOID lpMinimumApplicationAddress = nullptr, lpMaximumApplicationAddress = nullptr;
  DWORD_PTR dwActiveProcessorMask = 0;
  DWORD dwNumberOfProcessors = 0, dwProcessorType = 0, dwAllocationGranularity = 0;
  WORD wProcessorLevel = 0, wProcessorRevision = 0;
};
inline DWORD GetTickCount();

struct LASTINPUTINFO {
  UINT cbSize = sizeof(LASTINPUTINFO);
  DWORD dwTime = 0;
};
inline BOOL GetLastInputInfo(LASTINPUTINFO* info) {
  if (!info) return FALSE;
  info->dwTime = GetTickCount();
  return TRUE;
}

struct MSG {
  HWND hwnd = nullptr;
  UINT message = 0;
  WPARAM wParam = 0;
  LPARAM lParam = 0;
  DWORD time = 0;
  POINT pt{};
  DWORD lPrivate = 0;
};
struct MEASUREITEMSTRUCT { UINT CtlType = 0, CtlID = 0, itemID = 0, itemWidth = 0, itemHeight = 0; ULONG_PTR itemData = 0; };
struct DRAWITEMSTRUCT { UINT CtlType = 0, CtlID = 0, itemID = 0, itemAction = 0, itemState = 0; HWND hwndItem = nullptr; HDC hDC = nullptr; RECT rcItem{}; ULONG_PTR itemData = 0; };
struct OVERLAPPED {
  ULONG_PTR Internal = 0, InternalHigh = 0;
  DWORD Offset = 0, OffsetHigh = 0;
  HANDLE hEvent = nullptr;
};

struct CRITICAL_SECTION {
  pthread_mutex_t mutex;
};
using LPCRITICAL_SECTION = CRITICAL_SECTION*;
struct SRWLOCK {
  pthread_mutex_t mutex;
};
using PSRWLOCK = SRWLOCK*;
struct CONDITION_VARIABLE {
  pthread_cond_t cond;
};
struct INIT_ONCE {
  pthread_once_t once = PTHREAD_ONCE_INIT;
};

// ---------------------------------------------------------------- handle objects

namespace bxposix {
enum class HandleKind { File, Event, Mutex, Thread, Process, Map, Other };
struct Handle {
  HandleKind kind = HandleKind::Other;
  int fd = -1;
  bool delete_on_close = false;
  std::string delete_path;
  pthread_t thread{};
  bool valid_thread = false;
  bool manual_reset = false;
  bool signalled = false;
  pid_t pid = -1;
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
  void* map = nullptr;
  size_t map_size = 0;
  ~Handle() {
    if (kind == HandleKind::File && fd >= 0) close(fd);
    if (delete_on_close && !delete_path.empty()) unlink(delete_path.c_str());
    if (kind == HandleKind::Map && map) munmap(map, map_size);
    if (kind == HandleKind::Mutex) pthread_mutex_destroy(&mutex);
    if (kind == HandleKind::Event) pthread_cond_destroy(&cond);
  }
};
inline Handle* as(HANDLE h) { return static_cast<Handle*>(h); }
inline Handle* make(HandleKind kind) { auto* h = new Handle(); h->kind = kind; return h; }
inline DWORD& last_error() {
  static thread_local DWORD error = 0;
  return error;
}
}  // namespace bxposix

using bxposix::last_error;

inline DWORD GetLastError() { return last_error(); }
inline void SetLastError(DWORD value) { last_error() = value; }

// ---------------------------------------------------------------- strings

inline int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR text, int length, LPWSTR out, int out_length) {
  (void)cp;
  (void)flags;
  if (!text) return 0;
  const int n = length < 0 ? static_cast<int>(std::strlen(text)) + 1 : length;
  std::string source(text, text + n);
  std::mbstate_t state{};
  const char* begin = source.data();
  size_t need = std::mbsrtowcs(nullptr, &begin, 0, &state);
  if (need == static_cast<size_t>(-1)) return 0;
  // The query form reserves room for the terminator; callers allocate exactly this and then convert.
  if (!out || out_length <= 0) return static_cast<int>(need + 1);
  state = {};
  begin = source.data();
  const size_t written = std::mbsrtowcs(out, &begin, static_cast<size_t>(out_length - 1), &state);
  if (written == static_cast<size_t>(-1)) return 0;
  out[written] = L'\0';
  return static_cast<int>(written);
}

inline int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR text, int length, LPSTR out, int out_length,
                               LPCSTR default_char, BOOL* used_default) {
  (void)cp;
  (void)flags;
  (void)default_char;
  if (used_default) *used_default = FALSE;
  if (!text) return 0;
  const int n = length < 0 ? static_cast<int>(std::wcslen(text)) + 1 : length;
  std::wstring source(text, text + n);
  std::mbstate_t state{};
  const wchar_t* begin = source.data();
  size_t need = std::wcsrtombs(nullptr, &begin, 0, &state);
  if (need == static_cast<size_t>(-1)) return 0;
  if (!out || out_length <= 0) return static_cast<int>(need + 1);
  state = {};
  begin = source.data();
  const size_t written = std::wcsrtombs(out, &begin, static_cast<size_t>(out_length - 1), &state);
  if (written == static_cast<size_t>(-1)) return 0;
  out[written] = '\0';
  return static_cast<int>(written);
}

inline std::wstring bx_utf8_to_wide(const std::string& text) {
  if (text.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (n <= 0) return L"";
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
  return out;
}

inline std::string bx_wide_to_utf8(const std::wstring& text) {
  if (text.empty()) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return "";
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// ---------------------------------------------------------------- environment

inline DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size) {
  const char* value = std::getenv(name);
  if (!value) return 0;
  const size_t length = std::strlen(value);
  if (buffer && size > length) {
    std::memcpy(buffer, value, length + 1);
  }
  // Windows returns the length without the terminator; callers build std::string(buffer, length).
  return static_cast<DWORD>(length);
}

inline DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size) {
  const std::string narrow = bx_wide_to_utf8(name);
  const char* value = std::getenv(narrow.c_str());
  if (!value) return 0;
  const std::wstring wide = bx_utf8_to_wide(value);
  if (buffer && size > wide.size()) std::wmemcpy(buffer, wide.c_str(), wide.size() + 1);
  return static_cast<DWORD>(wide.size());
}

inline BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
  return (value ? setenv(name, value, 1) : unsetenv(name)) == 0 ? TRUE : FALSE;
}

// ---------------------------------------------------------------- time

inline ULONGLONG GetTickCount64() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<ULONGLONG>(ts.tv_sec) * 1000ull + static_cast<ULONGLONG>(ts.tv_nsec) / 1000000ull;
}
inline DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }
inline void Sleep(DWORD ms) { usleep(static_cast<useconds_t>(ms) * 1000u); }
inline BOOL SwitchToThread() { sched_yield(); return TRUE; }
inline DWORD GetCurrentThreadId() { return static_cast<DWORD>(reinterpret_cast<uintptr_t>(pthread_self())); }
inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
inline HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2)); }
inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-3)); }

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* value) {
  if (value) value->QuadPart = 1000000000ll;
  return TRUE;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* value) {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  if (value) value->QuadPart = static_cast<LONGLONG>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
  return TRUE;
}

inline void GetSystemTimeAsFileTime(FILETIME* ft) {
  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  const uint64_t value = (static_cast<uint64_t>(ts.tv_sec) + 11644473600ull) * 10000000ull + ts.tv_nsec / 100;
  ft->dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFu);
  ft->dwHighDateTime = static_cast<DWORD>(value >> 32);
}
inline void GetSystemTime(SYSTEMTIME* st) {
  time_t now = time(nullptr);
  struct tm tm{};
  gmtime_r(&now, &tm);
  st->wYear = tm.tm_year + 1900;
  st->wMonth = tm.tm_mon + 1;
  st->wDay = tm.tm_mday;
  st->wDayOfWeek = tm.tm_wday;
  st->wHour = tm.tm_hour;
  st->wMinute = tm.tm_min;
  st->wSecond = tm.tm_sec;
  st->wMilliseconds = 0;
}
inline void GetLocalTime(SYSTEMTIME* st) {
  time_t now = time(nullptr);
  struct tm tm{};
  localtime_r(&now, &tm);
  st->wYear = tm.tm_year + 1900;
  st->wMonth = tm.tm_mon + 1;
  st->wDay = tm.tm_mday;
  st->wDayOfWeek = tm.tm_wday;
  st->wHour = tm.tm_hour;
  st->wMinute = tm.tm_min;
  st->wSecond = tm.tm_sec;
  st->wMilliseconds = 0;
}

// ---------------------------------------------------------------- files

inline HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES*, DWORD disposition,
                          DWORD attributes, HANDLE) {
  (void)access;
  (void)share;
  (void)disposition;
  (void)attributes;
  if (!path) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  std::string native = bx_slashify(path);
  if (native == "NUL" || native == "/dev/null") native = "/dev/null";
  int flags = (access == GENERIC_READ) ? O_RDONLY : ((access == GENERIC_WRITE && !(access & GENERIC_READ)) ? O_WRONLY : O_RDWR);
  if (disposition == CREATE_ALWAYS || disposition == CREATE_NEW) flags |= O_CREAT;
  if (disposition == CREATE_ALWAYS) flags |= O_TRUNC;
  if (disposition == CREATE_NEW) flags |= O_EXCL;
  if (disposition == OPEN_ALWAYS) flags |= O_CREAT;
  const int fd = open(native.c_str(), flags, 0644);
  if (fd < 0) {
    SetLastError(errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
  }
  auto* handle = bxposix::make(bxposix::HandleKind::File);
  handle->fd = fd;
  if (attributes & FILE_FLAG_DELETE_ON_CLOSE) {
    handle->delete_on_close = true;
    handle->delete_path = native;
  }
  return handle;
}

inline HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa, DWORD disposition,
                          DWORD attributes, HANDLE template_handle) {
  const std::string narrow = bx_wide_to_utf8(path);
  return CreateFileA(narrow.c_str(), access, share, sa, disposition, attributes, template_handle);
}

inline BOOL SetEvent(HANDLE event);

inline BOOL ReadFile(HANDLE handle, LPVOID buffer, DWORD bytes, DWORD* read, OVERLAPPED* overlapped) {
  auto* h = bxposix::as(handle);
  if (!h || h->kind != bxposix::HandleKind::File) return FALSE;
  if (overlapped) {
    const off_t offset = static_cast<off_t>(static_cast<uint64_t>(overlapped->Offset) |
                                            (static_cast<uint64_t>(overlapped->OffsetHigh) << 32));
    if (lseek(h->fd, offset, SEEK_SET) < 0) return FALSE;
  }
  const ssize_t got = ::read(h->fd, buffer, bytes);
  if (got < 0) return FALSE;
  if (read) *read = static_cast<DWORD>(got);
  if (overlapped) {
    overlapped->InternalHigh = static_cast<ULONG_PTR>(got);
    if (overlapped->hEvent) SetEvent(overlapped->hEvent);
  }
  return TRUE;
}

inline BOOL WriteFile(HANDLE handle, LPCVOID buffer, DWORD bytes, DWORD* written, OVERLAPPED* overlapped) {
  auto* h = bxposix::as(handle);
  if (!h || h->kind != bxposix::HandleKind::File) return FALSE;
  if (overlapped) {
    const off_t offset = static_cast<off_t>(static_cast<uint64_t>(overlapped->Offset) |
                                            (static_cast<uint64_t>(overlapped->OffsetHigh) << 32));
    if (lseek(h->fd, offset, SEEK_SET) < 0) return FALSE;
  }
  const ssize_t put = ::write(h->fd, buffer, bytes);
  if (put < 0) return FALSE;
  if (written) *written = static_cast<DWORD>(put);
  if (overlapped) {
    overlapped->InternalHigh = static_cast<ULONG_PTR>(put);
    if (overlapped->hEvent) SetEvent(overlapped->hEvent);
  }
  return TRUE;
}

inline BOOL GetOverlappedResult(HANDLE, OVERLAPPED* overlapped, DWORD* transferred, BOOL) {
  if (!overlapped || !transferred) return FALSE;
  *transferred = static_cast<DWORD>(overlapped->InternalHigh);
  return TRUE;
}
inline BOOL CancelIoEx(HANDLE, OVERLAPPED*) { return TRUE; }

inline BOOL SetEndOfFile(HANDLE handle) {
  auto* h = bxposix::as(handle);
  if (!h || h->kind != bxposix::HandleKind::File) return FALSE;
  const off_t position = lseek(h->fd, 0, SEEK_CUR);
  return position >= 0 && ftruncate(h->fd, position) == 0 ? TRUE : FALSE;
}

inline BOOL FlushFileBuffers(HANDLE handle) {
  auto* h = bxposix::as(handle);
  return h && h->kind == bxposix::HandleKind::File ? (fsync(h->fd) == 0 ? TRUE : FALSE) : FALSE;
}

inline BOOL CloseHandle(HANDLE handle) {
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return FALSE;
  if (handle == GetCurrentProcess() || handle == GetCurrentThread()) return TRUE;
  auto* h = bxposix::as(handle);
  if (!h) return FALSE;
  delete h;
  return TRUE;
}

inline BOOL ReadFileEx(HANDLE, LPVOID, DWORD, OVERLAPPED*, void (*)(DWORD, DWORD, OVERLAPPED*)) { return FALSE; }

inline DWORD GetFileSize(HANDLE handle, DWORD* high) {
  auto* h = bxposix::as(handle);
  if (!h || h->kind != bxposix::HandleKind::File) return INVALID_FILE_SIZE;
  struct stat st{};
  if (fstat(h->fd, &st) != 0) return INVALID_FILE_SIZE;
  if (high) *high = static_cast<DWORD>(st.st_size >> 32);
  return static_cast<DWORD>(st.st_size & 0xFFFFFFFFu);
}

inline BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER* size) {
  auto* h = bxposix::as(handle);
  if (!h || h->kind != bxposix::HandleKind::File || !size) return FALSE;
  struct stat st{};
  if (fstat(h->fd, &st) != 0) return FALSE;
  size->QuadPart = st.st_size;
  return TRUE;
}

inline BOOL SetFilePointerEx(HANDLE handle, LARGE_INTEGER distance, LARGE_INTEGER* new_position, DWORD method) {
  auto* h = bxposix::as(handle);
  if (!h || h->kind != bxposix::HandleKind::File) return FALSE;
  const off_t result = lseek(h->fd, static_cast<off_t>(distance.QuadPart),
                             method == FILE_END ? SEEK_END : (method == FILE_CURRENT ? SEEK_CUR : SEEK_SET));
  if (result < 0) return FALSE;
  if (new_position) new_position->QuadPart = result;
  return TRUE;
}

inline DWORD SetFilePointer(HANDLE handle, LONG distance, LONG* high, DWORD method) {
  LARGE_INTEGER d{};
  d.QuadPart = distance;
  LARGE_INTEGER out{};
  if (!SetFilePointerEx(handle, d, &out, method)) return INVALID_FILE_SIZE;
  if (high) *high = static_cast<LONG>(out.QuadPart >> 32);
  return static_cast<DWORD>(out.QuadPart & 0xFFFFFFFFu);
}

inline BOOL DeleteFileA(LPCSTR path) {
  const std::string native = bx_slashify(path ? path : "");
  return unlink(native.c_str()) == 0 ? TRUE : FALSE;
}
inline BOOL DeleteFileW(LPCWSTR path) { return DeleteFileA(bx_wide_to_utf8(path).c_str()); }

inline BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES*) {
  const std::string native = bx_slashify(path ? path : "");
  if (native.empty()) return FALSE;
  // Windows callers create one level at a time; the Linux port lets the same call make a missing parent
  // chain, which is what a fresh XDG data root needs.
  std::string partial;
  if (native[0] == '/') partial = "/";
  for (size_t i = 0; i < native.size(); ++i) {
    partial += native[i];
    if (native[i] == '/' || i + 1 == native.size()) {
      if (partial.size() > 1) mkdir(partial.c_str(), 0755);
    }
  }
  struct stat st{};
  return stat(native.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? TRUE : FALSE;
}
inline BOOL CreateDirectoryW(LPCWSTR path, SECURITY_ATTRIBUTES* sa) {
  return CreateDirectoryA(bx_wide_to_utf8(path).c_str(), sa);
}
inline BOOL RemoveDirectoryA(LPCSTR path) { return rmdir(bx_slashify(path ? path : "").c_str()) == 0 ? TRUE : FALSE; }

inline void bx_unix_to_filetime(time_t seconds, FILETIME& ft) {
  const uint64_t value = (static_cast<uint64_t>(seconds) + 11644473600ull) * 10000000ull;
  ft.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFu);
  ft.dwHighDateTime = static_cast<DWORD>(value >> 32);
}

inline BOOL GetFileAttributesExA(LPCSTR path, int, WIN32_FILE_ATTRIBUTE_DATA* data) {
  struct stat st{};
  if (!data || stat(bx_slashify(path ? path : "").c_str(), &st) != 0) return FALSE;
  data->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
  data->nFileSizeLow = static_cast<DWORD>(st.st_size & 0xFFFFFFFFu);
  data->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
  bx_unix_to_filetime(st.st_ctime, data->ftCreationTime);
  bx_unix_to_filetime(st.st_atime, data->ftLastAccessTime);
  bx_unix_to_filetime(st.st_mtime, data->ftLastWriteTime);
  return TRUE;
}
inline BOOL GetFileAttributesExW(LPCWSTR path, int level, WIN32_FILE_ATTRIBUTE_DATA* data) {
  return GetFileAttributesExA(bx_wide_to_utf8(path).c_str(), level, data);
}
enum { GetFileExInfoStandard = 0 };
inline DWORD GetFileAttributesA(LPCSTR path) {
  struct stat st{};
  const std::string native = bx_slashify(path ? path : "");
  if (stat(native.c_str(), &st) != 0) return INVALID_FILE_ATTRIBUTES;
  DWORD result = 0;
  if (S_ISDIR(st.st_mode)) result |= FILE_ATTRIBUTE_DIRECTORY;
  if (!(st.st_mode & S_IWUSR)) result |= FILE_ATTRIBUTE_READONLY;
  return result;
}
inline DWORD GetFileAttributesW(LPCWSTR path) { return GetFileAttributesA(bx_wide_to_utf8(path).c_str()); }

inline BOOL SetFileAttributesW(LPCWSTR path, DWORD attributes) {
  (void)path;
  (void)attributes;
  return TRUE;
}
inline BOOL SetFileAttributesA(LPCSTR path, DWORD attributes) {
  (void)path;
  (void)attributes;
  return TRUE;
}

inline BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
  const std::string src = bx_slashify(from ? from : "");
  const std::string dst = bx_slashify(to ? to : "");
  if (flags & MOVEFILE_REPLACE_EXISTING) {
    if (rename(src.c_str(), dst.c_str()) == 0) return TRUE;
    if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED)) {
      FILE* in = std::fopen(src.c_str(), "rb");
      if (!in) return FALSE;
      FILE* out = std::fopen(dst.c_str(), "wb");
      if (!out) {
        std::fclose(in);
        return FALSE;
      }
      char buffer[64 * 1024];
      size_t n;
      bool ok = true;
      while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (std::fwrite(buffer, 1, n, out) != n) {
          ok = false;
          break;
        }
      }
      std::fclose(in);
      if (std::fclose(out) != 0) ok = false;
      if (ok) unlink(src.c_str());
      return ok ? TRUE : FALSE;
    }
    return FALSE;
  }
  return rename(src.c_str(), dst.c_str()) == 0 ? TRUE : FALSE;
}
inline BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  return MoveFileExA(bx_wide_to_utf8(from).c_str(), bx_wide_to_utf8(to).c_str(), flags);
}
inline BOOL MoveFileA(LPCSTR from, LPCSTR to) { return MoveFileExA(from, to, 0); }

inline BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
  const std::string src = bx_slashify(from ? from : "");
  const std::string dst = bx_slashify(to ? to : "");
  if (fail_if_exists && access(dst.c_str(), F_OK) == 0) return FALSE;
  FILE* in = std::fopen(src.c_str(), "rb");
  if (!in) return FALSE;
  FILE* out = std::fopen(dst.c_str(), "wb");
  if (!out) {
    std::fclose(in);
    return FALSE;
  }
  char buffer[64 * 1024];
  size_t n;
  bool ok = true;
  while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0) {
    if (std::fwrite(buffer, 1, n, out) != n) {
      ok = false;
      break;
    }
  }
  std::fclose(in);
  if (std::fclose(out) != 0) ok = false;
  return ok ? TRUE : FALSE;
}

inline DWORD GetFullPathNameA(LPCSTR path, DWORD size, LPSTR out, LPSTR* file_part) {
  char resolved[PATH_MAX];
  if (!realpath(bx_slashify(path ? path : "").c_str(), resolved)) {
    if (!path) return 0;
    std::strncpy(resolved, path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';
  }
  if (file_part) *file_part = std::strrchr(resolved, '/');
  const size_t length = std::strlen(resolved);
  if (!out || size == 0) return static_cast<DWORD>(length + 1);  // the query form
  if (size > length) std::memcpy(out, resolved, length + 1);
  return static_cast<DWORD>(length);
}
inline DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR out, LPWSTR* file_part) {
  const std::string narrow = bx_wide_to_utf8(path);
  char resolved[PATH_MAX];
  char* part = nullptr;
  const DWORD length = GetFullPathNameA(narrow.c_str(), sizeof(resolved), resolved, &part);
  if (out && size > 0) {
    const std::wstring wide = bx_utf8_to_wide(resolved);
    std::wmemcpy(out, wide.c_str(), std::min<size_t>(size, wide.size() + 1));
  }
  if (file_part) *file_part = nullptr;
  return length;
}

inline BOOL GetVolumePathNameW(LPCWSTR, LPWSTR out, DWORD size) {
  if (out && size >= 2) { out[0] = L'/'; out[1] = L'\0'; return TRUE; }
  return FALSE;
}
inline UINT GetDriveTypeW(LPCWSTR) { return DRIVE_FIXED; }
inline BOOL GetDiskFreeSpaceExA(LPCSTR, ULARGE_INTEGER* free_bytes, ULARGE_INTEGER* total, ULARGE_INTEGER* total_free) {
  struct statvfs info{};
  if (statvfs("/", &info) != 0) return FALSE;
  const uint64_t block = info.f_frsize ? info.f_frsize : info.f_bsize;
  const uint64_t available = static_cast<uint64_t>(info.f_bavail) * block;
  const uint64_t capacity = static_cast<uint64_t>(info.f_blocks) * block;
  if (free_bytes) free_bytes->QuadPart = available;
  if (total) total->QuadPart = capacity;
  if (total_free) total_free->QuadPart = static_cast<uint64_t>(info.f_bfree) * block;
  return TRUE;
}
inline BOOL GetDiskFreeSpaceExW(LPCWSTR path, ULARGE_INTEGER* a, ULARGE_INTEGER* b, ULARGE_INTEGER* c) {
  return GetDiskFreeSpaceExA(bx_wide_to_utf8(path).c_str(), a, b, c);
}
inline BOOL GetVolumeInformationW(LPCWSTR, LPWSTR volume, DWORD volume_size, DWORD* serial, DWORD* max_component,
                                  DWORD* flags, LPWSTR filesystem, DWORD filesystem_size) {
  if (volume && volume_size) volume[0] = L'\0';
  if (serial) *serial = 0;
  if (max_component) *max_component = 255;
  if (flags) *flags = 0;
  if (filesystem && filesystem_size) {
    const wchar_t* name = L"ext4";
    std::wmemcpy(filesystem, name, 5);
  }
  return TRUE;
}
inline DWORD GetCurrentDirectoryA(DWORD size, LPSTR out) {
  char buffer[PATH_MAX];
  if (!getcwd(buffer, sizeof(buffer))) return 0;
  const size_t length = std::strlen(buffer);
  if (out && size > length) std::memcpy(out, buffer, length + 1);
  return static_cast<DWORD>(length + 1);
}
inline BOOL SetCurrentDirectoryA(LPCSTR path) { return chdir(bx_slashify(path ? path : "").c_str()) == 0 ? TRUE : FALSE; }
inline BOOL SetCurrentDirectoryW(LPCWSTR path) { return SetCurrentDirectoryA(bx_wide_to_utf8(path).c_str()); }

inline DWORD GetTempPathA(DWORD size, LPSTR out) {
  const char* temp = std::getenv("TMPDIR");
  if (!temp || !*temp) temp = "/tmp/";
  const size_t length = std::strlen(temp);
  if (out && size > length) std::memcpy(out, temp, length + 1);
  return static_cast<DWORD>(length);
}

inline HANDLE GetStdHandle(DWORD which) {
  (void)which;
  return INVALID_HANDLE_VALUE;
}
struct BX_JOB {};
inline HANDLE CreateJobObjectW(SECURITY_ATTRIBUTES*, LPCWSTR) { return new BX_JOB(); }
inline BOOL SetInformationJobObject(HANDLE job, int, LPVOID, DWORD) { return job ? TRUE : FALSE; }
inline BOOL AssignProcessToJobObject(HANDLE job, HANDLE) { return job ? TRUE : FALSE; }
enum { JobObjectExtendedLimitInformation = 9 };
struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
  LARGE_INTEGER PerProcessUserTimeLimit{};
  LARGE_INTEGER PerJobUserTimeLimit{};
  DWORD LimitFlags = 0;
  SIZE_T MinimumWorkingSetSize = 0;
  SIZE_T MaximumWorkingSetSize = 0;
  DWORD ActiveProcessLimit = 0;
  ULONG_PTR Affinity = 0;
  DWORD PriorityClass = 0;
  DWORD SchedulingClass = 0;
};
struct IO_COUNTERS {
  ULONGLONG ReadOperationCount = 0, WriteOperationCount = 0, OtherOperationCount = 0;
  ULONGLONG ReadTransferCount = 0, WriteTransferCount = 0, OtherTransferCount = 0;
};
struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
  JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation{};
  IO_COUNTERS IoInfo{};
  SIZE_T ProcessMemoryLimit = 0;
  SIZE_T JobMemoryLimit = 0;
  SIZE_T PeakProcessMemoryUsed = 0;
  SIZE_T PeakJobMemoryUsed = 0;
};
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000

inline HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
inline FARPROC GetProcAddress(HMODULE, LPCSTR) { return nullptr; }
inline HMODULE LoadLibraryA(LPCSTR) { return nullptr; }
inline BOOL FreeLibrary(HMODULE) { return TRUE; }

inline DWORD GetModuleFileNameA(HMODULE, LPSTR out, DWORD size) {
  char path[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0) return 0;
  path[n] = '\0';
  const size_t length = std::strlen(path);
  if (out && size > length) std::memcpy(out, path, length + 1);
  return static_cast<DWORD>(length);
}
inline DWORD GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size) {
  char path[PATH_MAX];
  const DWORD n = GetModuleFileNameA(module, path, sizeof(path));
  if (n == 0) return 0;
  const std::wstring wide = bx_utf8_to_wide(path);
  if (out && size > wide.size()) std::wmemcpy(out, wide.c_str(), wide.size() + 1);
  return static_cast<DWORD>(wide.size());
}

// ---------------------------------------------------------------- find

struct FindState {
  DIR* dir = nullptr;
  std::string pattern;
  std::string directory;
};

inline HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* data);
enum { FindExInfoBasic = 0, FindExInfoStandard = 1 };
enum { FindExSearchNameMatch = 0, FindExSearchLimitToDirectories = 1 };
inline HANDLE FindFirstFileExA(LPCSTR pattern, int, WIN32_FIND_DATAA* data, int, void*, DWORD) {
  return FindFirstFileA(pattern, data);
}
inline HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* data) {
  const std::string native = bx_slashify(pattern ? pattern : "");
  const size_t slash = native.find_last_of('/');
  const std::string directory = slash == std::string::npos ? "." : native.substr(0, slash);
  const std::string leaf = slash == std::string::npos ? native : native.substr(slash + 1);
  DIR* dir = opendir(directory.c_str());
  if (!dir) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  auto* state = new FindState();
  state->dir = dir;
  state->pattern = leaf;
  state->directory = directory;
  const auto match = [&](const char* name) {
    if (fnmatch(state->pattern.c_str(), name, FNM_CASEFOLD) == 0) return true;
    return false;
  };
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (!match(entry->d_name)) continue;
    std::memset(data, 0, sizeof(*data));
    std::strncpy(data->cFileName, entry->d_name, sizeof(data->cFileName) - 1);
    struct stat st{};
    const std::string full = directory + "/" + entry->d_name;
    if (stat(full.c_str(), &st) == 0) {
      data->nFileSizeLow = static_cast<DWORD>(st.st_size & 0xFFFFFFFFu);
      data->nFileSizeHigh = static_cast<DWORD>(st.st_size >> 32);
      if (S_ISDIR(st.st_mode)) data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    return reinterpret_cast<HANDLE>(state);
  }
  closedir(dir);
  delete state;
  SetLastError(ERROR_NO_MORE_FILES);
  return INVALID_HANDLE_VALUE;
}

inline HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) {
  WIN32_FIND_DATAA narrow{};
  HANDLE handle = FindFirstFileA(bx_wide_to_utf8(pattern).c_str(), &narrow);
  if (handle == INVALID_HANDLE_VALUE) return handle;
  if (data) {
    data->dwFileAttributes = narrow.dwFileAttributes;
    data->nFileSizeHigh = narrow.nFileSizeHigh;
    data->nFileSizeLow = narrow.nFileSizeLow;
    const std::wstring name = bx_utf8_to_wide(narrow.cFileName);
    std::wcsncpy(data->cFileName, name.c_str(), MAX_PATH - 1);
  }
  return handle;
}
inline BOOL FindNextFileA(HANDLE handle, WIN32_FIND_DATAA* data) {
  auto* state = reinterpret_cast<FindState*>(handle);
  if (!state || !state->dir) return FALSE;
  struct dirent* entry;
  while ((entry = readdir(state->dir)) != nullptr) {
    if (fnmatch(state->pattern.c_str(), entry->d_name, FNM_CASEFOLD) != 0) continue;
    std::memset(data, 0, sizeof(*data));
    std::strncpy(data->cFileName, entry->d_name, sizeof(data->cFileName) - 1);
    struct stat st{};
    const std::string full = state->directory + "/" + entry->d_name;
    if (stat(full.c_str(), &st) == 0) {
      data->nFileSizeLow = static_cast<DWORD>(st.st_size & 0xFFFFFFFFu);
      data->nFileSizeHigh = static_cast<DWORD>(st.st_size >> 32);
      if (S_ISDIR(st.st_mode)) data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    return TRUE;
  }
  SetLastError(ERROR_NO_MORE_FILES);
  return FALSE;
}

inline BOOL FindNextFileW(HANDLE handle, WIN32_FIND_DATAW* data) {
  WIN32_FIND_DATAA narrow{};
  if (!FindNextFileA(handle, &narrow)) return FALSE;
  if (data) {
    data->dwFileAttributes = narrow.dwFileAttributes;
    data->nFileSizeHigh = narrow.nFileSizeHigh;
    data->nFileSizeLow = narrow.nFileSizeLow;
    const std::wstring name = bx_utf8_to_wide(narrow.cFileName);
    std::wcsncpy(data->cFileName, name.c_str(), MAX_PATH - 1);
  }
  return TRUE;
}
inline BOOL FindClose(HANDLE handle) {
  auto* state = reinterpret_cast<FindState*>(handle);
  if (!state) return FALSE;
  if (state->dir) closedir(state->dir);
  delete state;
  return TRUE;
}

#define LOCALE_NAME_USER_DEFAULT ((const wchar_t*)0)
#define TIME_NOSECONDS 0x00000002
inline BOOL EnumProcessModulesEx(HANDLE, void**, DWORD, DWORD* needed, DWORD) {
  if (needed) *needed = 0;
  return FALSE;
}
inline BOOL GetModuleInformation(HANDLE, HMODULE, void*, DWORD) { return FALSE; }
inline DWORD GetModuleBaseNameA(HANDLE, HMODULE, LPSTR name, DWORD size) {
  if (name && size) name[0] = '\0';
  return 0;
}
inline BOOL QueryFullProcessImageNameA(HANDLE, DWORD, LPSTR name, DWORD* size) {
  if (name && size && *size) name[0] = '\0';
  return FALSE;
}

// ---------------------------------------------------------------- threads, events, mutexes

inline BOOL InitializeCriticalSectionEx(CRITICAL_SECTION* section, DWORD, DWORD) {
  return pthread_mutex_init(&section->mutex, nullptr) == 0 ? TRUE : FALSE;
}
inline void InitializeCriticalSection(CRITICAL_SECTION* section) { pthread_mutex_init(&section->mutex, nullptr); }
inline void EnterCriticalSection(CRITICAL_SECTION* section) { pthread_mutex_lock(&section->mutex); }
inline void LeaveCriticalSection(CRITICAL_SECTION* section) { pthread_mutex_unlock(&section->mutex); }
inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* section) {
  return pthread_mutex_trylock(&section->mutex) == 0 ? TRUE : FALSE;
}
inline void DeleteCriticalSection(CRITICAL_SECTION* section) { pthread_mutex_destroy(&section->mutex); }
inline void InitializeSRWLock(SRWLOCK* lock) { pthread_mutex_init(&lock->mutex, nullptr); }
inline void AcquireSRWLockExclusive(SRWLOCK* lock) { pthread_mutex_lock(&lock->mutex); }
inline void ReleaseSRWLockExclusive(SRWLOCK* lock) { pthread_mutex_unlock(&lock->mutex); }
inline void InitializeConditionVariable(CONDITION_VARIABLE* condition) { pthread_cond_init(&condition->cond, nullptr); }
inline void WakeConditionVariable(CONDITION_VARIABLE* condition) { pthread_cond_signal(&condition->cond); }
inline void WakeAllConditionVariable(CONDITION_VARIABLE* condition) { pthread_cond_broadcast(&condition->cond); }
inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* condition, SRWLOCK* lock, DWORD ms, ULONG) {
  struct timespec ts{};
  if (ms != INFINITE) {
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
      ++ts.tv_sec;
      ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&condition->cond, &lock->mutex, &ts) == 0 ? TRUE : FALSE;
  }
  return pthread_cond_wait(&condition->cond, &lock->mutex) == 0 ? TRUE : FALSE;
}

struct ThreadStart {
  LPTHREAD_START_ROUTINE function = nullptr;
  LPVOID parameter = nullptr;
};
inline void* bx_thread_trampoline(void* arg) {
  auto* start = static_cast<ThreadStart*>(arg);
  start->function(start->parameter);
  delete start;
  return nullptr;
}

inline HANDLE CreateThread(SECURITY_ATTRIBUTES*, SIZE_T, LPTHREAD_START_ROUTINE function, LPVOID parameter, DWORD, DWORD* id) {
  auto* handle = bxposix::make(bxposix::HandleKind::Thread);
  auto* start = new ThreadStart{function, parameter};
  if (pthread_create(&handle->thread, nullptr, bx_thread_trampoline, start) != 0) {
    delete start;
    delete handle;
    return nullptr;
  }
  handle->valid_thread = true;
  if (id) *id = static_cast<DWORD>(reinterpret_cast<uintptr_t>(handle->thread));
  return handle;
}

inline HANDLE CreateEventA(SECURITY_ATTRIBUTES*, BOOL manual_reset, BOOL initial_state, LPCSTR) {
  auto* handle = bxposix::make(bxposix::HandleKind::Event);
  handle->manual_reset = manual_reset != FALSE;
  handle->signalled = initial_state != FALSE;
  return handle;
}
inline HANDLE CreateEventW(SECURITY_ATTRIBUTES* sa, BOOL manual_reset, BOOL initial_state, LPCWSTR name) {
  return CreateEventA(sa, manual_reset, initial_state, name ? bx_wide_to_utf8(name).c_str() : nullptr);
}
inline BOOL SetEvent(HANDLE event) {
  auto* h = bxposix::as(event);
  if (!h || h->kind != bxposix::HandleKind::Event) return FALSE;
  pthread_mutex_lock(&h->mutex);
  h->signalled = true;
  pthread_cond_broadcast(&h->cond);
  pthread_mutex_unlock(&h->mutex);
  return TRUE;
}
inline BOOL ResetEvent(HANDLE event) {
  auto* h = bxposix::as(event);
  if (!h || h->kind != bxposix::HandleKind::Event) return FALSE;
  pthread_mutex_lock(&h->mutex);
  h->signalled = false;
  pthread_mutex_unlock(&h->mutex);
  return TRUE;
}
inline BOOL PulseEvent(HANDLE event) { return SetEvent(event); }

inline DWORD WaitForSingleObject(HANDLE handle, DWORD ms) {
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return WAIT_FAILED;
  if (handle == GetCurrentProcess()) {
    if (ms) Sleep(ms);
    return WAIT_OBJECT_0;
  }
  auto* h = bxposix::as(handle);
  if (!h) return WAIT_FAILED;
  if (h->kind == bxposix::HandleKind::Thread) {
    if (h->valid_thread) {
      if (ms == INFINITE) pthread_join(h->thread, nullptr);
      else {
        // pthread_timedjoin_np is glibc-only and is fine on Linux.
        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
          ++ts.tv_sec;
          ts.tv_nsec -= 1000000000L;
        }
        if (pthread_timedjoin_np(h->thread, nullptr, &ts) != 0) return WAIT_TIMEOUT;
      }
      h->valid_thread = false;
    }
    return WAIT_OBJECT_0;
  }
  if (h->kind == bxposix::HandleKind::Process) {
    int status = 0;
    if (h->pid <= 0) return WAIT_OBJECT_0;
    if (ms == INFINITE) {
      waitpid(h->pid, &status, 0);
    } else {
      const uint64_t deadline = GetTickCount64() + ms;
      for (;;) {
        const pid_t result = waitpid(h->pid, &status, WNOHANG);
        if (result == h->pid) break;
        if (result < 0) break;
        if (GetTickCount64() >= deadline) return WAIT_TIMEOUT;
        usleep(2000);
      }
    }
    h->pid = -1;
    return WAIT_OBJECT_0;
  }
  if (h->kind == bxposix::HandleKind::Event) {
    pthread_mutex_lock(&h->mutex);
    if (ms == INFINITE) {
      while (!h->signalled) pthread_cond_wait(&h->cond, &h->mutex);
    } else {
      struct timespec ts{};
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += ms / 1000;
      ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) {
        ++ts.tv_sec;
        ts.tv_nsec -= 1000000000L;
      }
      while (!h->signalled) {
        if (pthread_cond_timedwait(&h->cond, &h->mutex, &ts) != 0) {
          pthread_mutex_unlock(&h->mutex);
          return WAIT_TIMEOUT;
        }
      }
    }
    if (!h->manual_reset) h->signalled = false;
    pthread_mutex_unlock(&h->mutex);
    return WAIT_OBJECT_0;
  }
  return WAIT_OBJECT_0;
}

inline DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms) {
  const uint64_t deadline = GetTickCount64() + (ms == INFINITE ? 0 : ms);
  for (;;) {
    DWORD ready = 0;
    for (DWORD i = 0; i < count; ++i) {
      auto* h = bxposix::as(handles[i]);
      if (h && h->kind == bxposix::HandleKind::Event) {
        pthread_mutex_lock(&h->mutex);
        const bool signalled = h->signalled;
        pthread_mutex_unlock(&h->mutex);
        if (signalled) {
          ++ready;
          if (!wait_all) return i;
        }
      }
    }
    if (wait_all && ready == count) return WAIT_OBJECT_0;
    if (ms != INFINITE && GetTickCount64() >= deadline) return WAIT_TIMEOUT;
    usleep(1000);
  }
}

inline HANDLE CreateMutexA(SECURITY_ATTRIBUTES*, BOOL, LPCSTR) {
  return bxposix::make(bxposix::HandleKind::Mutex);
}
inline HANDLE CreateMutexW(SECURITY_ATTRIBUTES* sa, BOOL initial, LPCWSTR name) {
  return CreateMutexA(sa, initial, name ? bx_wide_to_utf8(name).c_str() : nullptr);
}
inline BOOL ReleaseMutex(HANDLE mutex) {
  auto* h = bxposix::as(mutex);
  if (!h || h->kind != bxposix::HandleKind::Mutex) return FALSE;
  pthread_mutex_unlock(&h->mutex);
  return TRUE;
}
inline BOOL WaitForSingleObjectEx(HANDLE handle, DWORD ms, BOOL) { return WaitForSingleObject(handle, ms); }

// ---------------------------------------------------------------- interlocked

inline LONG InterlockedIncrement(volatile LONG* value) { return __sync_add_and_fetch(value, 1); }
inline LONG InterlockedDecrement(volatile LONG* value) { return __sync_sub_and_fetch(value, 1); }
inline LONG InterlockedExchange(volatile LONG* value, LONG exchange) { return __sync_lock_test_and_set(value, exchange); }
inline LONG64 InterlockedIncrement64(volatile LONG64* value) { return __sync_add_and_fetch(value, 1); }
inline LONG64 InterlockedDecrement64(volatile LONG64* value) { return __sync_sub_and_fetch(value, 1); }
inline LONG64 InterlockedExchange64(volatile LONG64* value, LONG64 exchange) { return __sync_lock_test_and_set(value, exchange); }
inline LONG64 InterlockedCompareExchange64(volatile LONG64* value, LONG64 exchange, LONG64 comparand) {
  return __sync_val_compare_and_swap(value, comparand, exchange);
}
inline PVOID InterlockedExchangePointer(PVOID volatile* target, PVOID value) {
  return __sync_lock_test_and_set(target, value);
}
template <typename T>
inline T* InterlockedCompareExchangePointer(T* volatile* target, T* exchange, T* comparand) {
  return __sync_val_compare_and_swap(target, comparand, exchange);
}

// ---------------------------------------------------------------- processes

inline BOOL CreateProcessW(LPCWSTR application, LPWSTR command_line, SECURITY_ATTRIBUTES*, SECURITY_ATTRIBUTES*,
                           BOOL inherit, DWORD flags, LPVOID, LPCWSTR, STARTUPINFOW*, PROCESS_INFORMATION* info) {
  (void)inherit;
  (void)flags;
  std::string program = application ? bx_wide_to_utf8(application) : "";
  std::string args = command_line ? bx_wide_to_utf8(command_line) : "";
  if (program.empty() && !args.empty()) {
    if (args.front() == '"') {
      const size_t close = args.find('"', 1);
      program = args.substr(1, close == std::string::npos ? std::string::npos : close - 1);
      args = close == std::string::npos ? "" : args.substr(close + 1);
    } else {
      const size_t space = args.find(' ');
      program = args.substr(0, space);
      args = space == std::string::npos ? "" : args.substr(space + 1);
    }
  }
  if (program.empty()) return FALSE;

  std::vector<std::string> argv_storage;
  argv_storage.push_back(program);
  size_t index = 0;
  while (index < args.size()) {
    while (index < args.size() && (args[index] == ' ' || args[index] == '\t')) ++index;
    if (index >= args.size()) break;
    std::string word;
    bool quoted = false;
    while (index < args.size()) {
      const char c = args[index];
      if (c == '"') {
        quoted = !quoted;
        ++index;
        continue;
      }
      if (!quoted && (c == ' ' || c == '\t')) break;
      word.push_back(c);
      ++index;
    }
    argv_storage.push_back(word);
  }
  std::vector<char*> argv;
  for (auto& arg : argv_storage) argv.push_back(arg.data());
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) return FALSE;
  if (pid == 0) {
    execvp(program.c_str(), argv.data());
    _exit(127);
  }
  info->hProcess = bxposix::make(bxposix::HandleKind::Process);
  info->hProcess && (bxposix::as(info->hProcess)->pid = pid);
  info->hThread = nullptr;
  info->dwProcessId = static_cast<DWORD>(pid);
  info->dwThreadId = 0;
  return TRUE;
}

inline BOOL CreateProcessA(LPCSTR application, LPSTR command_line, SECURITY_ATTRIBUTES* process_security,
                           SECURITY_ATTRIBUTES* thread_security, BOOL inherit, DWORD flags, LPVOID environment,
                           LPCSTR directory, STARTUPINFOA* startup, PROCESS_INFORMATION* info) {
  (void)process_security;
  (void)thread_security;
  (void)inherit;
  (void)flags;
  (void)environment;
  (void)startup;
  (void)directory;
  return CreateProcessW(bx_utf8_to_wide(application ? application : "").c_str(),
                        bx_utf8_to_wide(command_line ? command_line : "").data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, nullptr, info);
}

inline HANDLE OpenProcess(DWORD, BOOL, DWORD pid) {
  auto* handle = bxposix::make(bxposix::HandleKind::Process);
  handle->pid = static_cast<pid_t>(pid);
  return handle;
}
inline BOOL TerminateProcess(HANDLE process, UINT code) {
  auto* h = bxposix::as(process);
  if (!h || h->pid <= 0) return FALSE;
  kill(h->pid, SIGTERM);
  (void)code;
  return TRUE;
}
inline BOOL GetExitCodeProcess(HANDLE process, DWORD* code) {
  auto* h = bxposix::as(process);
  if (!h) return FALSE;
  if (h->pid <= 0) {
    if (code) *code = 0;
    return TRUE;
  }
  int status = 0;
  const pid_t result = waitpid(h->pid, &status, WNOHANG);
  if (result == h->pid) {
    h->pid = -1;
    if (code) *code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  } else if (code) {
    *code = STILL_ACTIVE;
  }
  return TRUE;
}
inline BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
inline BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
inline DWORD GetPriorityClass(HANDLE) { return 0; }
inline BOOL SetProcessPriorityBoost(HANDLE, BOOL) { return TRUE; }
inline BOOL SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }
inline BOOL GetProcessTimes(HANDLE, FILETIME* creation, FILETIME* exit, FILETIME* kernel, FILETIME* user) {
  if (creation) *creation = {};
  if (exit) *exit = {};
  if (kernel) *kernel = {};
  if (user) *user = {};
  return TRUE;
}
inline BOOL GetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS* counters, DWORD size) {
  if (!counters || size < sizeof(*counters)) return FALSE;
  std::memset(counters, 0, sizeof(*counters));
  FILE* file = std::fopen("/proc/self/status", "r");
  if (file) {
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
      unsigned long kb = 0;
      if (std::sscanf(line, "VmRSS: %lu kB", &kb) == 1) counters->WorkingSetSize = kb * 1024;
      if (std::sscanf(line, "VmHWM: %lu kB", &kb) == 1) counters->PeakWorkingSetSize = kb * 1024;
    }
    std::fclose(file);
  }
  counters->PagefileUsage = counters->WorkingSetSize;
  counters->PeakPagefileUsage = counters->PeakWorkingSetSize;
  counters->cb = sizeof(*counters);
  return TRUE;
}
inline BOOL GetProcessWorkingSetSize(HANDLE, SIZE_T* min, SIZE_T* max) {
  if (min) *min = 0;
  if (max) *max = 0;
  return TRUE;
}
inline BOOL SetProcessWorkingSetSize(HANDLE, SIZE_T, SIZE_T) { return TRUE; }
inline BOOL EmptyWorkingSet(HANDLE) { return TRUE; }
inline DWORD GetActiveProcessorCount(WORD) { return static_cast<DWORD>(sysconf(_SC_NPROCESSORS_ONLN)); }
inline void GetSystemInfo(SYSTEM_INFO* info) {
  if (!info) return;
  std::memset(info, 0, sizeof(*info));
  info->dwPageSize = static_cast<DWORD>(sysconf(_SC_PAGESIZE));
  info->dwNumberOfProcessors = static_cast<DWORD>(sysconf(_SC_NPROCESSORS_ONLN));
}

inline LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD, DWORD protect) {
  (void)address;
  const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  const int prot = (protect == PAGE_READONLY) ? PROT_READ : (PROT_READ | PROT_WRITE);
  void* memory = mmap(nullptr, size, prot, flags, -1, 0);
  return memory == MAP_FAILED ? nullptr : memory;
}
inline BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD) {
  return munmap(address, size) == 0 ? TRUE : FALSE;
}
inline BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* status) {
  if (!status) return FALSE;
  struct sysinfo info{};
  if (sysinfo(&info) != 0) return FALSE;
  status->dwLength = sizeof(*status);
  status->ullTotalPhys = static_cast<ULONGLONG>(info.totalram) * info.mem_unit;
  status->ullAvailPhys = static_cast<ULONGLONG>(info.freeram) * info.mem_unit;
  status->ullTotalPageFile = status->ullTotalPhys;
  status->ullAvailPageFile = status->ullAvailPhys;
  status->ullTotalVirtual = status->ullTotalPhys;
  status->ullAvailVirtual = status->ullAvailPhys;
  return TRUE;
}

// ---------------------------------------------------------------- messages

inline std::atomic<bool>& bx_quit_requested() {
  static std::atomic<bool> requested{false};
  return requested;
}

inline BOOL PeekMessageW(MSG* message, HWND, UINT, UINT, UINT) {
  if (message) std::memset(message, 0, sizeof(*message));
  if (bx_quit_requested().exchange(false)) {
    if (message) message->message = WM_QUIT;
    return TRUE;
  }
  return FALSE;
}
inline BOOL PeekMessageA(MSG* message, HWND, UINT, UINT, UINT) { return PeekMessageW(message, nullptr, 0, 0, 0); }
inline BOOL TranslateMessage(const MSG*) { return FALSE; }
inline LRESULT DispatchMessageW(const MSG*) { return 0; }
inline LRESULT DispatchMessageA(const MSG*) { return 0; }
inline void PostQuitMessage(int) { bx_quit_requested().store(true); }
inline BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM) { return FALSE; }
inline BOOL PostMessageA(HWND, UINT, WPARAM, LPARAM) { return FALSE; }
inline LRESULT SendMessageW(HWND, UINT, WPARAM, LPARAM) { return 0; }
inline BOOL ShowWindow(HWND, int) { return FALSE; }
inline BOOL UpdateWindow(HWND) { return FALSE; }
inline BOOL DestroyWindow(HWND) { return FALSE; }
inline HWND GetForegroundWindow() { return nullptr; }
inline BOOL AllowSetForegroundWindow(DWORD) { return TRUE; }
inline BOOL SetForegroundWindow(HWND) { return TRUE; }
inline int MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT) {
  std::fprintf(stderr, "%s: %s\n", title ? title : "BuffrX", text ? text : "");
  return IDNO;
}
inline int MessageBoxW(HWND, LPCWSTR text, LPCWSTR title, UINT flags) {
  const std::string narrow_text = bx_wide_to_utf8(text ? text : L"");
  const std::string narrow_title = bx_wide_to_utf8(title ? title : L"");
  return MessageBoxA(nullptr, narrow_text.c_str(), narrow_title.c_str(), flags);
}
inline BOOL RegisterApplicationRestart(LPCWSTR, DWORD) { return TRUE; }
inline void SetProcessDPIAware() {}
inline HANDLE GetConsoleWindow() { return nullptr; }
inline BOOL AttachConsole(DWORD) { return FALSE; }
inline BOOL FreeConsole() { return TRUE; }
inline BOOL AllocConsole() { return FALSE; }
inline BOOL SetConsoleOutputCP(UINT) { return TRUE; }
inline BOOL SetConsoleTitleW(LPCWSTR) { return TRUE; }

// A shell's "open with the default app": xdg-open is the Linux equivalent of ShellExecute.
inline HINSTANCE ShellExecuteA(HWND, LPCSTR operation, LPCSTR file, LPCSTR parameters, LPCSTR directory, int show) {
  (void)operation;
  (void)parameters;
  (void)show;
  std::string command = "xdg-open ";
  if (directory && *directory) {
    command += " -- ";
  }
  command += file ? file : "";
  return reinterpret_cast<HINSTANCE>(std::system(command.c_str()));
}
inline HINSTANCE ShellExecuteW(HWND window, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, int show) {
  const std::string op = bx_wide_to_utf8(operation ? operation : L"");
  const std::string f = bx_wide_to_utf8(file ? file : L"");
  const std::string p = bx_wide_to_utf8(parameters ? parameters : L"");
  const std::string d = bx_wide_to_utf8(directory ? directory : L"");
  return ShellExecuteA(window, op.c_str(), f.c_str(), p.c_str(), d.c_str(), show);
}

// ---------------------------------------------------------------- misc

inline DWORD GetLastError();
inline void SetLastError(DWORD);
inline int GetSystemMetrics(int) { return 0; }
inline HMONITOR MonitorFromWindow(HWND, DWORD) { return nullptr; }
inline HMONITOR MonitorFromPoint(POINT, DWORD) { return nullptr; }
inline BOOL GetMonitorInfoW(HMONITOR, MONITORINFO*) { return FALSE; }
inline BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO* info) { return GetMonitorInfoW(monitor, info); }
#define MONITORINFOF_PRIMARY 1
inline BOOL EnumDisplayMonitors(HDC, LPRECT, BOOL (*)(HMONITOR, HDC, LPRECT, LPARAM), LPARAM) { return TRUE; }
inline BOOL EnumDisplaySettingsA(LPCSTR, DWORD, DEVMODEA*) { return FALSE; }
inline int GetTimeFormatEx(const wchar_t*, DWORD, const SYSTEMTIME* time, const wchar_t*, LPWSTR out, int count) {
  if (!time || !out || count < 6) return 0;
  char text[8];
  std::snprintf(text, sizeof(text), "%02u:%02u", time->wHour, time->wMinute);
  const std::wstring wide = bx_utf8_to_wide(text);
  std::wmemcpy(out, wide.c_str(), std::min<size_t>(wide.size() + 1, static_cast<size_t>(count)));
  return static_cast<int>(wide.size());
}
inline BOOL IsWindow(HWND hwnd) { return hwnd != nullptr; }
inline DWORD GetWindowThreadProcessId(HWND, DWORD* pid) {
  if (pid) *pid = 0;
  return 0;
}
inline BOOL GetWindowRect(HWND, RECT*) { return FALSE; }
inline BOOL GetClientRect(HWND, RECT*) { return FALSE; }
inline BOOL ClientToScreen(HWND, POINT*) { return FALSE; }
inline BOOL ScreenToClient(HWND, POINT*) { return FALSE; }
inline int GetWindowTextW(HWND, LPWSTR buffer, int count) {
  if (buffer && count) buffer[0] = L'\0';
  return 0;
}
inline int GetWindowTextA(HWND, LPSTR buffer, int count) {
  if (buffer && count) buffer[0] = '\0';
  return 0;
}
inline BOOL EnumWindows(BOOL (*)(HWND, LPARAM), LPARAM) { return TRUE; }
inline BOOL EnumChildWindows(HWND, BOOL (*)(HWND, LPARAM), LPARAM) { return TRUE; }
inline BOOL IsWindowVisible(HWND hwnd) { return hwnd != nullptr; }
inline BOOL IsIconic(HWND) { return FALSE; }
inline BOOL IsZoomed(HWND) { return FALSE; }
inline HWND GetShellWindow() { return nullptr; }
inline HWND GetDesktopWindow() { return nullptr; }
inline BOOL OpenClipboard(HWND) { return FALSE; }
inline BOOL CloseClipboard() { return TRUE; }
inline BOOL EmptyClipboard() { return TRUE; }
inline HANDLE GetClipboardData(UINT) { return nullptr; }
inline HANDLE SetClipboardData(UINT, HANDLE) { return nullptr; }
inline HANDLE GlobalAlloc(UINT, SIZE_T size) { return std::malloc(size); }
inline HANDLE GlobalFree(HANDLE memory) {
  std::free(memory);
  return nullptr;
}
inline LPVOID GlobalLock(HANDLE memory) { return memory; }
inline BOOL GlobalUnlock(HANDLE) { return TRUE; }
inline UINT RegisterClipboardFormatW(LPCWSTR) { return 0; }
inline std::wstring& bx_command_line() {
  static std::wstring line = [] {
    std::string text;
    FILE* file = std::fopen("/proc/self/cmdline", "rb");
    if (file) {
      char buffer[4096];
      size_t n;
      while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, n);
      std::fclose(file);
    }
    std::wstring wide;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\0') wide.push_back(L' ');
      else wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(text[i])));
    }
    while (!wide.empty() && wide.back() == L' ') wide.pop_back();
    return wide;
  }();
  return line;
}
inline LPWSTR GetCommandLineW() { return const_cast<LPWSTR>(bx_command_line().c_str()); }
inline LPWSTR* CommandLineToArgvW(LPCWSTR, int* count) {
  // The process arguments are exactly the NUL-separated bytes of /proc/self/cmdline. Parsing the
  // reconstructed command line with spaces would split every path that contains one.
  std::string text;
  if (FILE* file = std::fopen("/proc/self/cmdline", "rb")) {
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, n);
    std::fclose(file);
  }
  std::vector<std::wstring> arguments;
  size_t begin = 0;
  while (begin < text.size()) {
    const size_t end = text.find('\0', begin);
    const size_t stop = end == std::string::npos ? text.size() : end;
    if (stop > begin) arguments.push_back(bx_utf8_to_wide(text.substr(begin, stop - begin)));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  wchar_t** argv = static_cast<wchar_t**>(std::malloc(sizeof(wchar_t*) * (arguments.size() + 1)));
  for (size_t i = 0; i < arguments.size(); ++i) {
    argv[i] = static_cast<wchar_t*>(std::malloc((arguments[i].size() + 1) * sizeof(wchar_t)));
    std::wmemcpy(argv[i], arguments[i].c_str(), arguments[i].size() + 1);
  }
  argv[arguments.size()] = nullptr;
  if (count) *count = static_cast<int>(arguments.size());
  return argv;
}
inline HLOCAL LocalFree(HLOCAL memory) { std::free(memory); return nullptr; }
inline void OutputDebugStringA(LPCSTR text) { if (text) std::fputs(text, stderr); }
inline void OutputDebugStringW(LPCWSTR text) { OutputDebugStringA(bx_wide_to_utf8(text).c_str()); }
inline BOOL IsDebuggerPresent() { return FALSE; }
inline void DebugBreak() { __builtin_trap(); }

// FILE_FLAG_* used by a few calls; values are ignored by this layer.
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000


#endif  // !_WIN32

// Virtual-key and modifier values (Windows numeric values, so saved chords keep their meaning).
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_SCROLL 0x91
#define MOD_ALT 0x0001
#define MOD_CONTROL 0x0002
#define MOD_SHIFT 0x0004
#define MOD_WIN 0x0008
#define MOD_NOREPEAT 0x4000
