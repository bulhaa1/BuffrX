#pragma once
#include "windows.h"
#include "shlobj.h"
struct IFileOperation;
struct IShellItem;
struct IShellItemArray;
using REFIID = const void*;
#define CLSID_FileOperation nullptr
#define IID_IFileOperation nullptr
#define IID_IShellItem nullptr
#define CLSCTX_ALL 0
#define COINIT_APARTMENTTHREADED 2
#define FOF_ALLOWUNDO 0x40
#define FOF_NOCONFIRMATION 0x10
#define FOF_SILENT 0x4
#define FOF_NOERRORUI 0x400
#define FOFX_RECYCLEONDELETE 0x00080000
inline HRESULT CoInitializeEx(void*, DWORD) { return 0; }
inline void CoUninitialize() {}
inline HRESULT CoCreateInstance(const void*, void*, DWORD, const void*, void** out) { if (out) *out = nullptr; return -1; }
inline HRESULT SHCreateItemFromParsingName(const wchar_t*, void*, const void*, void** out) { if (out) *out = nullptr; return -1; }
