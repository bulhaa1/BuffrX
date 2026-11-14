// X11/XRandR display facts for the Linux engine: the monitor list the picker shows, the primary
// monitor's size, and the refresh rate of the display a capture uses.
#pragma once

#include <string>
#include <vector>

#include "../engine/display_target.h"

namespace bx {
std::vector<DisplayInfo> linux_displays();
bool linux_primary_size(int& width, int& height);
int linux_display_refresh_hz(int display_id, const std::string& device);
}  // namespace bx
