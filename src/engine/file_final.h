// Whether a file is finished, asked of Windows rather than guessed from its age.
//
// A chunk is indexed only once its writer has let go of it: an index entry for a file that is still
// being written is a chunk a cut or a playback would fail on. Asking for read access while denying
// write access fails with a sharing violation for exactly as long as a writer holds the file, which is
// the writer's own word for "not finished".
#pragma once

#include <string>

namespace bx {

// True once nothing holds the file open for writing any more. False while a writer still does, and
// false for a path that is not there at all: a file that cannot be opened has not been seen finishing.
bool file_writer_closed(const std::string& path);

}  // namespace bx
