// See file_final.h: Windows is the only one who knows whether a writer still holds the file. This is
// the same question the library asked before it ever handed a file to the decoder (it was a private
// helper in library_media.cpp); it is here now because the index has the same question to ask.
#include "file_final.h"

#include <windows.h>

namespace bx {

bool file_writer_closed(const std::string& path) {
  HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;  // held for writing, or not there: not seen finishing
  CloseHandle(handle);
  return true;
}

}  // namespace bx
