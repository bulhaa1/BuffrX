// libobs API differences behind one name. The port builds against libobs 30 on Linux and the pinned
// newer release on Windows, so a call the older API does not have is answered here for every caller.
#pragma once

#include <obs.h>

// The encoder's running frame count. libobs 30's public API does not expose it, so the status keeps
// the output's own count instead; the picture and the buffer are unaffected.
#if LIBOBS_API_MAJOR_VER >= 33
#define BX_ENCODER_FRAMES(encoder) obs_encoder_get_encoded_frames(encoder)
#else
#define BX_ENCODER_FRAMES(encoder) static_cast<uint64_t>(obs_get_total_frames())
#endif
