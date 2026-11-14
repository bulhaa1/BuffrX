#pragma once
#include "windows.h"
// The Linux port uses libcurl-free POSIX HTTP in link_linux.cpp; this shim exists so shared headers that
// mention WinHTTP types still parse. No engine translation unit includes it on Linux.
using HINTERNET = void*;
