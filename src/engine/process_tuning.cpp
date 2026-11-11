#include "process_tuning.h"

#include <windows.h>

#include <dxgi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

#include "../shared/log.h"

namespace buffrx {

namespace {

TuningSet g_active;

std::string trimmed_lower(const std::string& text) {
  size_t first = 0, last = text.size();
  while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
  std::string out = text.substr(first, last - first);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

TuningSet parse_tuning(const std::string& spec) {
  TuningSet set;
  size_t at = 0;
  while (at <= spec.size()) {
    const size_t comma = spec.find(',', at);
    const std::string word = trimmed_lower(spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
    if (word == "eco") set.efficiency_cores = true;
    else if (word == "below") set.below_normal = true;
    else if (word == "io") set.background_io = true;
    else if (word.rfind("gpu:", 0) == 0) {
      char* end = nullptr;
      const std::string number = word.substr(4);
      const long value = std::strtol(number.c_str(), &end, 10);
      if (!number.empty() && end && *end == '\0') set.gpu_priority = static_cast<int>(std::max(-7L, std::min(0L, value)));
    }
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  return set;
}

std::string tuning_describe(const TuningSet& set) {
  std::string out;
  auto add = [&out](const std::string& word) { out += (out.empty() ? "" : ",") + word; };
  if (set.gpu_priority != 0) add("gpu:" + std::to_string(set.gpu_priority));
  if (set.efficiency_cores) add("eco");
  if (set.below_normal) add("below");
  if (set.background_io) add("io");
  return out.empty() ? "none" : out;
}

const TuningSet& active_tuning() { return g_active; }
void set_active_tuning(const TuningSet& set) { g_active = set; }

namespace {

// Windows 10 calls the MinGW headers do not declare: looked up in kernel32, which also keeps the engine starting on a
// Windows that lacks them.
FARPROC kernel32_proc(const char* name) {
  const HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  return kernel32 ? GetProcAddress(kernel32, name) : nullptr;
}

}  // namespace

bool cpu_has_efficiency_cores() {
  using GetCpuSetsFn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
  const auto get_cpu_sets = reinterpret_cast<GetCpuSetsFn>(kernel32_proc("GetSystemCpuSetInformation"));
  if (!get_cpu_sets) return false;
  ULONG bytes = 0;
  get_cpu_sets(nullptr, 0, &bytes, GetCurrentProcess(), 0);
  if (bytes == 0) return false;
  std::vector<unsigned char> buffer(bytes);
  if (!get_cpu_sets(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), bytes, &bytes, GetCurrentProcess(), 0)) {
    return false;
  }
  int lowest = 255, highest = 0;
  for (ULONG offset = 0; offset < bytes;) {
    const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
    if (entry->Size == 0) break;
    if (entry->Type == CpuSetInformation) {
      lowest = std::min<int>(lowest, entry->CpuSet.EfficiencyClass);
      highest = std::max<int>(highest, entry->CpuSet.EfficiencyClass);
    }
    offset += entry->Size;
  }
  return highest > lowest;
}

bool apply_process_tuning(const TuningSet& set) {
  bool ok = true;
  if (set.below_normal) {
    const bool done = SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) != 0;
    BX_LOG_INFO("tuning: process priority below normal %s", done ? "accepted" : "refused");
    ok = ok && done;
  }
  if (set.efficiency_cores) {
    if (!cpu_has_efficiency_cores()) {
      BX_LOG_INFO("tuning: this CPU has no efficiency cores; EcoQoS not applied");
    } else {
      PROCESS_POWER_THROTTLING_STATE state{};
      state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
      state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      state.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      using SetProcessInfoFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
      const auto set_info = reinterpret_cast<SetProcessInfoFn>(kernel32_proc("SetProcessInformation"));
      const bool done = set_info && set_info(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != 0;
      BX_LOG_INFO("tuning: EcoQoS (efficiency cores) %s", done ? "accepted" : "refused");
      ok = ok && done;
    }
  }
  return ok;
}

bool apply_gpu_priority(void* d3d11_device, int priority) {
  if (!d3d11_device || priority == 0) return true;
  IUnknown* unknown = static_cast<IUnknown*>(d3d11_device);
  IDXGIDevice* dxgi = nullptr;
  if (FAILED(unknown->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) || !dxgi) {
    BX_LOG_INFO("tuning: the graphics device has no DXGI interface; GPU priority not applied");
    return false;
  }
  const HRESULT result = dxgi->SetGPUThreadPriority(priority);
  dxgi->Release();
  BX_LOG_INFO("tuning: GPU thread priority %d %s (0x%08lX)", priority, SUCCEEDED(result) ? "accepted" : "refused",
              static_cast<unsigned long>(result));
  return SUCCEEDED(result);
}

bool begin_background_io() {
  const bool done = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
  BX_LOG_INFO("tuning: background I/O for the trim thread %s", done ? "accepted" : "refused");
  return done;
}

}  // namespace buffrx
