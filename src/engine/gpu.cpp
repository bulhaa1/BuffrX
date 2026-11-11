#include "gpu.h"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

namespace bx {

std::string gpu_vendor_from_id(uint32_t vendor_id) {
  switch (vendor_id) {
    case 0x10DE: return "nvidia";
    case 0x1002: return "amd";
    case 0x8086: return "intel";
    default: return "other";
  }
}

bool gpu_adapter(uint32_t& vendor_id, std::string& name) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
    return false;
  }
  IDXGIAdapter1* adapter = nullptr;
  if (FAILED(factory->EnumAdapters1(0, &adapter)) || !adapter) {
    factory->Release();
    return false;
  }
  DXGI_ADAPTER_DESC1 description{};
  const bool ok = SUCCEEDED(adapter->GetDesc1(&description));
  if (ok) {
    vendor_id = description.VendorId;
    // DXGI reports the adapter name as UTF-16; the protocol is UTF-8.
    const int needed = WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, nullptr, 0, nullptr, nullptr);
    if (needed > 1) {
      std::string converted(static_cast<size_t>(needed), '\0');
      WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, &converted[0], needed, nullptr, nullptr);
      if (!converted.empty() && converted.back() == '\0') converted.pop_back();
      name = converted;
    }
  }
  adapter->Release();
  factory->Release();
  return ok;
}

std::vector<GpuAdapterInfo> gpu_adapters() {
  std::vector<GpuAdapterInfo> list;
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return list;
  for (UINT index = 0;; ++index) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
    DXGI_ADAPTER_DESC1 description{};
    if (SUCCEEDED(adapter->GetDesc1(&description))) {
      GpuAdapterInfo info;
      info.index = static_cast<int>(index);
      info.dedicated_bytes = description.DedicatedVideoMemory;
      info.software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || description.VendorId == 0x1414;
      for (UINT output_index = 0;; ++output_index) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND || !output) break;
        DXGI_OUTPUT_DESC output_description{};
        if (SUCCEEDED(output->GetDesc(&output_description))) {
          const int needed = WideCharToMultiByte(CP_UTF8, 0, output_description.DeviceName, -1, nullptr, 0, nullptr, nullptr);
          if (needed > 1) {
            std::string name(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, output_description.DeviceName, -1, &name[0], needed, nullptr, nullptr);
            name.resize(static_cast<size_t>(needed - 1));
            info.outputs.push_back(name);
          }
        }
        output->Release();
      }
      list.push_back(info);
    }
    adapter->Release();
  }
  factory->Release();
  return list;
}

int pick_gpu_adapter(const std::vector<GpuAdapterInfo>& adapters, const std::string& monitor_device) {
  if (adapters.empty()) return 0;
  if (!monitor_device.empty()) {
    for (const GpuAdapterInfo& adapter : adapters) {
      for (const std::string& output : adapter.outputs) {
        if (output == monitor_device && !adapter.software) return adapter.index;
      }
    }
  }
  const GpuAdapterInfo* best = nullptr;
  for (const GpuAdapterInfo& adapter : adapters) {
    if (adapter.software) continue;
    if (!best || adapter.dedicated_bytes > best->dedicated_bytes) best = &adapter;
  }
  return best ? best->index : adapters.front().index;
}

}  // namespace bx
