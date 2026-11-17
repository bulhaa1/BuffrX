// Screenshots: one frame of the capture source, read back with libobs and encoded with stb_image.
#include "../engine/screenshot.h"

#include <obs.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "../shared/log.h"

namespace bx {

bool encode_image_wic(const uint8_t* bgra, int width, int height, const std::string& path, bool jpeg,
                      std::string& error) {
  if (!bgra || width <= 0 || height <= 0) {
    error = "no image data";
    return false;
  }
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0, pixels = static_cast<size_t>(width) * height; i < pixels; ++i) {
    rgb[i * 3 + 0] = bgra[i * 4 + 2];
    rgb[i * 3 + 1] = bgra[i * 4 + 1];
    rgb[i * 3 + 2] = bgra[i * 4 + 0];
  }
  const bool ok = jpeg ? stbi_write_jpg(path.c_str(), width, height, 3, rgb.data(), 92) != 0
                       : stbi_write_png(path.c_str(), width, height, 3, rgb.data(), width * 3) != 0;
  if (!ok) {
    error = "the image could not be written";
    std::remove(path.c_str());
  }
  return ok;
}

bool write_screenshot(obs_source* source, const std::string& path, bool jpeg, std::string& error) {
  if (!source) {
    error = "no capture source";
    return false;
  }
  const uint32_t width = obs_source_get_width(source);
  const uint32_t height = obs_source_get_height(source);
  if (width == 0 || height == 0) {
    error = "the capture source has no frame yet";
    return false;
  }
  std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
  bool captured = false;
  obs_enter_graphics();
  gs_texrender_t* texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
  gs_stagesurf_t* stagesurface = nullptr;
  if (texrender) {
    gs_texrender_reset(texrender);
    if (gs_texrender_begin(texrender, width, height)) {
      struct vec4 clear{};
      vec4_zero(&clear);
      clear.w = 1.0f;
      gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
      gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
      obs_source_video_render(source);
      gs_texrender_end(texrender);
      stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
      if (stagesurface) {
        gs_stage_texture(stagesurface, gs_texrender_get_texture(texrender));
        uint8_t* data = nullptr;
        uint32_t linesize = 0;
        if (gs_stagesurface_map(stagesurface, &data, &linesize) && data) {
          for (uint32_t y = 0; y < height; ++y)
            std::memcpy(&bgra[static_cast<size_t>(y) * width * 4],
                        data + static_cast<size_t>(y) * linesize, static_cast<size_t>(width) * 4);
          gs_stagesurface_unmap(stagesurface);
          captured = true;
        }
      }
    }
  }
  if (stagesurface) gs_stagesurface_destroy(stagesurface);
  if (texrender) gs_texrender_destroy(texrender);
  obs_leave_graphics();
  if (!captured) {
    error = "the frame could not be read back";
    return false;
  }
  return encode_image_wic(bgra.data(), static_cast<int>(width), static_cast<int>(height), path, jpeg, error);
}

}  // namespace bx
