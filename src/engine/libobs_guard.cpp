#include "libobs_guard.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <util/base.h>

#include "../shared/files.h"
#include "../shared/log.h"
#include "compat_report.h"
#include "driver_fault.h"

namespace bx {
namespace {

std::atomic<unsigned long> g_device_lost_at{0};
std::atomic<bool> g_capture_wanted{false};
// Set once before the handlers exist and read by them afterwards.
std::string g_marker_path;
std::mutex g_limiter_mutex;
buffrx::LogLimiter g_limiter;
// The latest "settings:" block per encoder (keyed by its log header), bounded: a handful of encoders exist at once.
std::mutex g_blocks_mutex;
std::vector<std::pair<std::string, std::string>> g_blocks;  // oldest first
constexpr size_t kBlockKeep = 8;

// Runs on whichever thread libobs logs from: no engine lock, no call into libobs, a bounded buffer.
void log_handler(int level, const char* format, va_list args, void*) {
  char message[2048];
  std::vsnprintf(message, sizeof(message), format ? format : "", args);
  size_t length = std::strlen(message);
  while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r')) message[--length] = '\0';
  if (level == LOG_INFO && encoder_settings_block_is(message)) {
    // The one info message let into engine.log: what an encoder really runs (AMF lowers the preset at init and says so only here).
    const size_t close = std::string(message).find("] settings:");
    const std::string header = std::string(message).substr(0, close + 1);
    {
      std::lock_guard<std::mutex> lock(g_blocks_mutex);
      if (g_blocks.size() >= kBlockKeep) g_blocks.erase(g_blocks.begin());
      g_blocks.emplace_back(header, message);
    }
    BX_LOG_NOTE("libobs: %s", message);
    return;
  }
  if (level > LOG_WARNING) {
    // Info and debug follow the engine's own policy: stderr only.
    if (level == LOG_INFO) BX_LOG_INFO("libobs: %s", message);
    else BX_LOG_DEBUG("libobs: %s", message);
    return;
  }
  if (buffrx::is_device_lost_line(message)) {
    unsigned long now = GetTickCount();
    g_device_lost_at = now == 0 ? 1 : now;
  }
  int suppressed = 0;
  bool admit = false;
  {
    std::lock_guard<std::mutex> lock(g_limiter_mutex);
    admit = g_limiter.admit(format, GetTickCount(), suppressed);
  }
  if (suppressed > 0) BX_LOG_WARN("libobs: %d similar lines suppressed", suppressed);
  if (!admit) return;
  if (level == LOG_ERROR) BX_LOG_ERROR("libobs: %s", message);
  else BX_LOG_WARN("libobs: %s", message);
}

bool relaunch_allowed_here() {
  // A test engine runs on a data root of its own and its harness stops only the pids it started: a copy it did not start
  // would be left behind. BUFFRX_CRASH_RELAUNCH=1 asks for the real behaviour there too.
  char probe[8] = {0};
  if (GetEnvironmentVariableA("BUFFRX_CRASH_RELAUNCH", probe, sizeof(probe)) > 0) return probe[0] == '1';
  return GetEnvironmentVariableA("BUFFRX_DATA_ROOT", nullptr, 0) == 0;
}

void spawn_relaunch() {
  wchar_t path[MAX_PATH * 2] = {0};
  if (GetModuleFileNameW(nullptr, path, MAX_PATH * 2) == 0) return;
  std::wstring directory = path;
  const size_t slash = directory.find_last_of(L"\\/");
  if (slash != std::wstring::npos) directory.resize(slash);
  std::wstring command = L"\"" + std::wstring(path) + L"\" --relaunch-after " + std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                     directory.c_str(), &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}

// Never returns. Not exit(): static destructors would run on a process libobs has just declared broken.
void crash_handler(const char* format, va_list args, void*) {
  static std::atomic<bool> entered{false};
  if (entered.exchange(true)) TerminateProcess(GetCurrentProcess(), 5);
  char message[1024];
  std::vsnprintf(message, sizeof(message), format ? format : "", args);
  size_t length = std::strlen(message);
  while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r')) message[--length] = '\0';
  BX_LOG_ERROR("libobs crashed: %s", message);  // the log write flushes the file
  const bool wanted = g_capture_wanted.load();
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  buffrx::CrashMarker before;
  buffrx::crash_marker_read(g_marker_path, before);
  buffrx::CrashDecision decision = buffrx::crash_decide(before.history, wanted, now);
  if (decision.relaunch && !relaunch_allowed_here()) decision.relaunch = false;
  const buffrx::CrashMarker marker = buffrx::crash_marker_after(before, message, wanted, now, decision);
  write_file_atomic(g_marker_path, buffrx::crash_marker_to_json(marker).dump(2));
  BX_LOG_ERROR("libobs crashed: %s", decision.relaunch ? "BuffrX starts again" : "BuffrX does not start again by itself");
  if (decision.relaunch) spawn_relaunch();
  TerminateProcess(GetCurrentProcess(), 5);
}

}  // namespace

void install_libobs_handlers(const std::string& data_root) {
  g_marker_path = data_root + "\\driver_crash.json";
  base_set_log_handler(log_handler, nullptr);
  base_set_crash_handler(crash_handler, nullptr);
}

std::string libobs_encoder_settings_block(const std::string& encoder_name) {
  std::lock_guard<std::mutex> lock(g_blocks_mutex);
  for (auto entry = g_blocks.rbegin(); entry != g_blocks.rend(); ++entry) {
    if (entry->first.find(encoder_name) != std::string::npos) return entry->second;
  }
  return std::string();
}

unsigned long libobs_device_lost_at_ms() { return g_device_lost_at.load(); }

void libobs_set_capture_wanted(bool wanted) { g_capture_wanted = wanted; }

void wait_for_dying_engine(unsigned long pid) {
  if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
    WaitForSingleObject(process, 30000);
    CloseHandle(process);
  }
  // The driver has just reset: starting the graphics device on it at once is how the first start failed too.
  Sleep(5000);
}

}  // namespace bx
