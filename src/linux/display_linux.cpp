#include "display_linux.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cmath>

namespace bx {
namespace {
struct Dpy {
  Display* display = nullptr;
  Dpy() {
    XInitThreads();
    display = XOpenDisplay(nullptr);
  }
  ~Dpy() {
    if (display) XCloseDisplay(display);
  }
};
Dpy& dpy() {
  static Dpy holder;
  return holder;
}
}  // namespace

std::vector<DisplayInfo> linux_displays() {
  std::vector<DisplayInfo> list;
  Display* display = dpy().display;
  if (!display) return list;
  int count = 0;
  XRRMonitorInfo* monitors = XRRGetMonitors(display, DefaultRootWindow(display), True, &count);
  if (!monitors) return list;
  for (int i = 0; i < count; ++i) {
    DisplayInfo entry;
    entry.id = i;
    char* name = XGetAtomName(display, monitors[i].name);
    entry.device = name ? name : ("screen" + std::to_string(i));
    if (name) XFree(name);
    entry.width = monitors[i].width;
    entry.height = monitors[i].height;
    entry.primary = monitors[i].primary != 0;
    entry.name = entry.primary ? "Primary display" : ("Display " + std::to_string(i + 1));
    entry.name += " (" + std::to_string(entry.width) + "x" + std::to_string(entry.height) + ")";
    list.push_back(entry);
  }
  XRRFreeMonitors(monitors);
  return list;
}

bool linux_primary_size(int& width, int& height) {
  const std::vector<DisplayInfo> displays = linux_displays();
  for (const DisplayInfo& display : displays) {
    if (!display.primary) continue;
    width = display.width;
    height = display.height;
    return width > 0 && height > 0;
  }
  if (!displays.empty()) {
    width = displays.front().width;
    height = displays.front().height;
    return width > 0 && height > 0;
  }
  return false;
}

int linux_display_refresh_hz(int display_id, const std::string& device) {
  Display* display = dpy().display;
  if (!display) return 0;
  int monitor_count = 0;
  XRRMonitorInfo* monitors = XRRGetMonitors(display, DefaultRootWindow(display), True, &monitor_count);
  if (!monitors) return 0;
  int chosen = display_id >= 0 && display_id < monitor_count ? display_id : 0;
  if (!device.empty()) {
    for (int i = 0; i < monitor_count; ++i) {
      char* name = XGetAtomName(display, monitors[i].name);
      const bool match = name && device == name;
      if (name) XFree(name);
      if (match) {
        chosen = i;
        break;
      }
    }
  }
  int hz = 0;
  XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, DefaultRootWindow(display));
  if (resources && monitors[chosen].noutput > 0) {
    XRROutputInfo* output = XRRGetOutputInfo(display, resources, monitors[chosen].outputs[0]);
    if (output && output->crtc) {
      XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, resources, output->crtc);
      if (crtc && crtc->mode) {
        for (int i = 0; i < resources->nmode; ++i) {
          if (resources->modes[i].id != crtc->mode) continue;
          const XRRModeInfo& mode = resources->modes[i];
          const double total = static_cast<double>(mode.hTotal) * mode.vTotal;
          if (total > 0) hz = static_cast<int>(std::lround(mode.dotClock / total));
          break;
        }
      }
      if (crtc) XRRFreeCrtcInfo(crtc);
    }
    if (output) XRRFreeOutputInfo(output);
  }
  if (resources) XRRFreeScreenResources(resources);
  XRRFreeMonitors(monitors);
  return hz > 1 ? hz : 0;
}

}  // namespace bx
