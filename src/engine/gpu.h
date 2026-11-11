// Which graphics vendor is doing the work. The vendor comes from DXGI's VendorId, never from parsing
// a device name: names change with every driver release and every OEM rebrand, ids do not.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bx {

// 0x10DE NVIDIA, 0x1002 AMD/ATI, 0x8086 Intel; anything else is "other".
std::string gpu_vendor_from_id(uint32_t vendor_id);

// The adapter the engine captures and encodes on: adapter 0, which is what the engine asks libobs
// for. A hybrid laptop that drives its screen from an integrated GPU and encodes on a discrete one
// is reported as the integrated adapter; that is the honest answer for this call and is documented
// rather than guessed.
bool gpu_adapter(uint32_t& vendor_id, std::string& name);

// One DXGI adapter: its index, its own memory, whether it is the software renderer, and the displays
// (\\.\DISPLAY1) attached to it.
struct GpuAdapterInfo {
  int index = 0;
  uint64_t dedicated_bytes = 0;
  bool software = false;
  std::vector<std::string> outputs;
};
std::vector<GpuAdapterInfo> gpu_adapters();

// The adapter libobs is given. A display capture needs the adapter its display hangs off; anything else
// (a game, a window) runs on the card with the most memory, which on a laptop is the discrete one and not
// the integrated GPU that adapter 0 usually is. The software renderer is only chosen when it is all there
// is, and equal cards keep the first, so a setup that worked does not move.
int pick_gpu_adapter(const std::vector<GpuAdapterInfo>& adapters, const std::string& monitor_device);

}  // namespace bx
