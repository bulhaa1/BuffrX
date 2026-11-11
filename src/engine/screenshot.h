// Screenshots: one frame of the capture source, converted and written as PNG or JPEG.
//
// The frame comes from libobs as NV12 on the CPU (a rare, explicit copy — screenshots are user
// initiated, not per-frame), and the encoder is WIC, which ships with Windows and needs no library.
#pragma once

#include <cstdint>
#include <string>

struct obs_source;

namespace bx {

// Captures the source's current frame and writes it to `path`. Returns false with a reason on
// failure; a missing file is never left behind.
bool write_screenshot(obs_source* source, const std::string& path, bool jpeg, std::string& error);

// Encodes a BGRA buffer. Exposed because the editor reuses it for frame previews.
bool encode_image_wic(const uint8_t* bgra, int width, int height, const std::string& path, bool jpeg,
                      std::string& error);

}  // namespace bx
