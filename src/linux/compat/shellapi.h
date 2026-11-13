#pragma once
#include "windows.h"
#define SEE_MASK_NOCLOSEPROCESS 0x00000040
struct SHELLEXECUTEINFOA {
  DWORD cbSize = sizeof(SHELLEXECUTEINFOA);
  DWORD fMask = 0;
  HWND hwnd = nullptr;
  LPCSTR lpVerb = nullptr, lpFile = nullptr, lpParameters = nullptr, lpDirectory = nullptr;
  int nShow = 0;
  HINSTANCE hInstApp = nullptr;
  void* lpIDList = nullptr;
  LPCSTR lpClass = nullptr;
  HKEY hkeyClass = nullptr;
  DWORD dwHotKey = 0;
  HANDLE hIcon = nullptr, hProcess = nullptr;
};
inline BOOL ShellExecuteExA(SHELLEXECUTEINFOA* info) {
  if (!info || !info->lpFile) return FALSE;
  ShellExecuteA(info->hwnd, info->lpVerb, info->lpFile, info->lpParameters, info->lpDirectory, info->nShow);
  return TRUE;
}
inline int SHFileOperationA(void*) { return 0; }
