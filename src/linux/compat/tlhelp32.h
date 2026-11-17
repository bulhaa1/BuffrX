#pragma once
#include "windows.h"
#include <dirent.h>
#define TH32CS_SNAPPROCESS 0x00000002
struct PROCESSENTRY32 {
  DWORD dwSize = sizeof(PROCESSENTRY32);
  DWORD cntUsage = 0, th32ProcessID = 0;
  ULONG_PTR th32DefaultHeapID = 0;
  DWORD th32ModuleID = 0, cntThreads = 0, th32ParentProcessID = 0;
  LONG pcPriClassBase = 0;
  DWORD dwFlags = 0;
  char szExeFile[MAX_PATH] = {};
};
struct BX_PROCESS_SNAPSHOT {
  std::vector<PROCESSENTRY32> entries;
  size_t cursor = 0;
};
inline HANDLE CreateToolhelp32Snapshot(DWORD, DWORD) {
  auto* snap = new BX_PROCESS_SNAPSHOT();
  DIR* dir = opendir("/proc");
  if (dir) {
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) continue;
      PROCESSENTRY32 process{};
      process.th32ProcessID = static_cast<DWORD>(std::atoi(entry->d_name));
      char path[64];
      std::snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
      FILE* file = std::fopen(path, "r");
      if (file) {
        if (std::fgets(process.szExeFile, sizeof(process.szExeFile), file)) {
          char* newline = std::strchr(process.szExeFile, '\n');
          if (newline) *newline = '\0';
        }
        std::fclose(file);
      }
      snap->entries.push_back(process);
    }
    closedir(dir);
  }
  return snap;
}
inline BOOL Process32First(HANDLE snapshot, PROCESSENTRY32* entry) {
  auto* snap = static_cast<BX_PROCESS_SNAPSHOT*>(snapshot);
  if (!snap || snap->entries.empty()) return FALSE;
  snap->cursor = 0;
  *entry = snap->entries[0];
  return TRUE;
}
inline BOOL Process32Next(HANDLE snapshot, PROCESSENTRY32* entry) {
  auto* snap = static_cast<BX_PROCESS_SNAPSHOT*>(snapshot);
  if (!snap || snap->cursor + 1 >= snap->entries.size()) return FALSE;
  *entry = snap->entries[++snap->cursor];
  return TRUE;
}
