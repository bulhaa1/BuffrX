// Video probe: reports exactly which graphics resource libobs cannot create on this machine.
//
// libobs returns a bare failure when one of its core effects, the shared sampler, or the transparent
// texture comes back null, and logs nothing. This runs the same steps in the open and names the
// culprit, which is the difference between "capture does not work" and a fixable report.
#include <obs.h>

#include <cstdio>
#include <string>

#include "../shared/log.h"

namespace bx {

int run_video_probe(const std::string& runtime_root, const std::string& data_root) {
  static const auto slashify = [](const std::string& path) {
    std::string out = path;
    for (char& c : out) {
      if (c == '\\') c = '/';
    }
    return out;
  };
  if (!obs_startup("en-US", data_root.c_str(), nullptr)) {
    std::printf("PROBE_FAIL obs_startup\n");
    return 1;
  }
  const std::string core_data = slashify(runtime_root + "\\data\\libobs\\");
  // libobs 32 marks obs_add_data_path deprecated but does not find its own effect files without it:
  // obs_find_data_file("default.effect") returns nothing before this call (probe output, 2026-10-17).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  obs_add_data_path(core_data.c_str());
#pragma clang diagnostic pop

  const std::string graphics = slashify(runtime_root + "\\bin\\64bit\\libobs-d3d11.dll");
  graphics_t* device = nullptr;
  const int code = gs_create(&device, graphics.c_str(), 0);
  std::printf("PROBE gs_create=%d\n", code);
  if (code != GS_SUCCESS || !device) {
    std::printf("PROBE_FAIL gs_create\n");
    return 1;
  }

  gs_enter_context(device);
  std::printf("PROBE device=%s type=%d\n", gs_get_device_name() ? gs_get_device_name() : "?",
              static_cast<int>(gs_get_device_type()));

  const char* effects[] = {"default.effect",   "opaque.effect",      "solid.effect",
                           "repeat.effect",    "format_conversion.effect", "bicubic_scale.effect",
                           "lanczos_scale.effect", "area.effect",   "bilinear_lowres_scale.effect",
                           "premultiplied_alpha.effect", "color.effect", "default_rect.effect"};
  int failures = 0;
  // default_rect.effect samples a GL texture rectangle, which D3D has no equivalent for; libobs's
  // own effect load says "texture_rect is not supported in D3D". That is a fact of the backend, not
  // a fault on this machine, so it is not counted as one.
  const bool rect_supported = gs_get_device_type() == GS_DEVICE_OPENGL;
  for (const char* name : effects) {
    if (!rect_supported && std::string(name) == "default_rect.effect") {
      std::printf("PROBE effect %-28s skipped (texture_rect is GL only)\n", name);
      continue;
    }
    char* path = obs_find_data_file(name);
    char* error = nullptr;
    gs_effect_t* effect = path ? gs_effect_create_from_file(path, &error) : nullptr;
    if (!effect) {
      ++failures;
      std::printf("PROBE effect %-28s FAIL %s\n", name, error ? error : (path ? "no error text" : "file not found"));
    } else {
      std::printf("PROBE effect %-28s ok\n", name);
    }
    if (error) bfree(error);
    if (path) bfree(path);
    if (effect) gs_effect_destroy(effect);
  }

  gs_sampler_info sampler_info{};
  sampler_info.max_anisotropy = 1;
  gs_samplerstate_t* sampler = gs_samplerstate_create(&sampler_info);
  std::printf("PROBE sampler=%s\n", sampler ? "ok" : "FAIL");
  if (!sampler) ++failures;
  if (sampler) gs_samplerstate_destroy(sampler);

  uint8_t transparent_data[2 * 2 * 4] = {0};
  const uint8_t* transparent = transparent_data;
  gs_texture_t* texture = gs_texture_create(2, 2, GS_RGBA, 1, &transparent, 0);
  std::printf("PROBE texture=%s\n", texture ? "ok" : "FAIL");
  if (!texture) ++failures;
  if (texture) gs_texture_destroy(texture);

  gs_leave_context();
  gs_destroy(device);

  std::printf("PROBE_RESULT failures=%d\n", failures);
  return failures == 0 ? 0 : 2;
}

}  // namespace bx
