// The Linux graphics adapter: one DRM card, its PCI vendor, and the monitors XRandR reports.
#include "../engine/gpu.h"

#include <X11/extensions/Xrandr.h>
#include <X11/Xlib.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace bx {

std::string gpu_vendor_from_id(uint32_t vendor_id) {
  switch (vendor_id) {
    case 0x10DE: return "nvidia";
    case 0x1002: return "amd";
    case 0x8086: return "intel";
    default: return "other";
  }
}

namespace {
uint32_t pci_vendor() {
  for (int card = 0; card < 8; ++card) {
    std::ifstream vendor("/sys/class/drm/card" + std::to_string(card) + "/device/vendor");
    std::string text;
    if (!(vendor >> text)) continue;
    unsigned value = 0;
    if (std::sscanf(text.c_str(), "0x%x", &value) == 1) return value;
  }
  return 0;
}
std::vector<std::string> outputs() {
  std::vector<std::string> list;
  Display* display = XOpenDisplay(nullptr);
  if (!display) return list;
  int count = 0;
  XRRMonitorInfo* monitors = XRRGetMonitors(display, DefaultRootWindow(display), True, &count);
  if (monitors) {
    for (int i = 0; i < count; ++i) {
      char* name = XGetAtomName(display, monitors[i].name);
      if (name) {
        list.emplace_back(name);
        XFree(name);
      }
    }
    XRRFreeMonitors(monitors);
  }
  XCloseDisplay(display);
  return list;
}
}  // namespace

bool gpu_adapter(uint32_t& vendor_id, std::string& name) {
  vendor_id = pci_vendor();
  if (vendor_id == 0) return false;
  name = gpu_vendor_from_id(vendor_id) + " graphics";
  return true;
}

std::vector<GpuAdapterInfo> gpu_adapters() {
  std::vector<GpuAdapterInfo> list;
  GpuAdapterInfo info;
  info.index = 0;
  info.dedicated_bytes = 0;  // unified or shared memory; the engine only uses this to rank adapters
  info.software = false;
  info.outputs = outputs();
  if (pci_vendor() != 0) list.push_back(info);
  return list;
}

int pick_gpu_adapter(const std::vector<GpuAdapterInfo>& adapters, const std::string& monitor_device) {
  if (adapters.empty()) return 0;
  for (const GpuAdapterInfo& adapter : adapters) {
    for (const std::string& output : adapter.outputs) {
      if (output == monitor_device && !adapter.software) return adapter.index;
    }
  }
  return adapters.front().index;
}

}  // namespace bx
