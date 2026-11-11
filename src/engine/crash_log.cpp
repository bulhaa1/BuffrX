#include "crash_log.h"

#include "../shared/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>

namespace bx {
namespace {

// What the handler saw.
std::atomic<DWORD> g_fault_code{0};
std::atomic<void*> g_fault_address{nullptr};
// The type of a C++ exception, as the runtime's own pointer to it, and its message. Read later,
// where the runtime is safe to use: the message is what says whether this is a deadlocked mutex
// or a bad allocation.
std::atomic<void*> g_fault_type{nullptr};
std::atomic<bool> g_has_message{false};
// The message of the exception, copied out with memcpy: the C runtime may not be called here.
char g_fault_message[256] = {0};

// The report file, opened once. The handler writes to it directly: the C runtime may not be used
// there, and the process is often gone before the tick can report anything.
HANDLE g_report = INVALID_HANDLE_VALUE;

// Whether a piece of memory is really there to be read. The handler must not fault, so every
// pointer it follows is checked first.
bool readable(const void* address, size_t size) {
  if (!address) return false;
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
  if (info.State != MEM_COMMIT) return false;
  if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  const uintptr_t start = reinterpret_cast<uintptr_t>(address);
  return start + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}

// The report is built by hand: no C runtime, no allocation, no lock. These three do the work.
void append(char* buffer, size_t size, size_t& used, const char* text) {
  while (*text && used + 1 < size) buffer[used++] = *text++;
  buffer[used] = '\0';
}

void append_hex(char* buffer, size_t size, size_t& used, uintptr_t value) {
  char digits[24] = {0};
  int count = 0;
  if (!value) {
    digits[count++] = '0';
  } else {
    while (value && count < 20) {
      digits[count++] = "0123456789abcdef"[value & 0xF];
      value >>= 4;
    }
  }
  char reversed[24] = {0};
  for (int i = 0; i < count; ++i) reversed[i] = digits[count - 1 - i];
  append(buffer, size, used, reversed);
}

void append_module(char* buffer, size_t size, size_t& used, void* module, uintptr_t offset) {
  if (!module) {
    append(buffer, size, used, "an unknown module+0x");
    append_hex(buffer, size, used, offset);
    return;
  }
  char name[MAX_PATH] = {0};
  GetModuleFileNameA(static_cast<HMODULE>(module), name, sizeof(name));
  const char* leaf = name;
  for (const char* c = name; *c; ++c) {
    if (*c == '\\' || *c == '/') leaf = c + 1;
  }
  append(buffer, size, used, leaf);
  append(buffer, size, used, "+0x");
  append_hex(buffer, size, used, offset);
}

// Writes the report where it cannot be lost: the log file, opened for append, written with nothing
// but the operating system. The tick repeats the same words in the ordinary log.
void write_report(DWORD code, void* address) {
  if (g_report == INVALID_HANDLE_VALUE) return;
  char buffer[512] = {0};
  size_t used = 0;
  append(buffer, sizeof(buffer), used, "[exception] raised in the engine (code 0x");
  append_hex(buffer, sizeof(buffer), used, code);
  append(buffer, sizeof(buffer), used, ") in ");
  HMODULE module = nullptr;
  if (address) {
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(address), &module);
  }
  append_module(buffer, sizeof(buffer), used, module,
                reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
  append(buffer, sizeof(buffer), used, "\r\n");
  DWORD written = 0;
  WriteFile(g_report, buffer, static_cast<DWORD>(used), &written, nullptr);
}

// The handler. Nothing here may touch the C runtime, and it must not walk the stack either: the
// graphics backend raises exceptions of its own while it works, and unwinding inside that handler
// is what brought the engine down - the fault ended up in ntdll's heap, with the log empty.
//
// What it does is the least: two stores, a report written with nothing but the operating system,
// and a return. The report is small on purpose - the code, the address and the module it is in.
LONG WINAPI on_fault(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  // Windows sends every C++ DLL a thread-detach notice with this code. It is not a fault.
  if (code == 0x406D1388) return EXCEPTION_CONTINUE_SEARCH;
  g_fault_code.store(code, std::memory_order_relaxed);
  g_fault_address.store(info->ExceptionRecord->ExceptionAddress, std::memory_order_relaxed);
  // A C++ exception carries its type and its message in the object it was thrown with: the type
  // descriptor first, then the exception itself, whose first word is the text. Only the pointers
  // are taken here, and only when the memory is really there; the name and the text are read later,
  // where the runtime is safe to use. Reading them here faulted inside the handler, which is the
  // worst place to fault.
  if (code == 0xe06d7363 && info->ExceptionRecord->NumberParameters > 1) {
    const auto object = reinterpret_cast<const char*>(info->ExceptionRecord->ExceptionInformation[1]);
    if (object && readable(object, 2 * sizeof(void*))) {
      const void* type = *reinterpret_cast<const void* const*>(object);
      if (type && readable(type, 24)) g_fault_type.store(const_cast<void*>(type), std::memory_order_relaxed);
      const void* message = *reinterpret_cast<const void* const*>(object + sizeof(void*));
      if (message && readable(message, sizeof(void*))) {
        const char* text = *reinterpret_cast<const char* const*>(message);
        if (text && readable(text, 2)) {
          size_t length = 0;
          while (length < sizeof(g_fault_message) - 1 && text[length]) ++length;
          memcpy(g_fault_message, text, length);
          g_fault_message[length] = '\0';
          g_has_message.store(true, std::memory_order_relaxed);
        }
      }
    }
  }
  write_report(code, info->ExceptionRecord->ExceptionAddress);
  // Keep looking: the exception may be one libobs handles itself, and saying so is its business.
  return EXCEPTION_CONTINUE_SEARCH;
}

// Called when an exception escapes every handler and the process is about to end. It is handed the
// exception itself, so the log says what it was. A bare rethrow with no active exception is itself an
// exception, which re-enters this handler until the stack is gone and nothing is written.
void on_terminate() {
  char line[512] = {0};
  const std::exception_ptr failure = std::current_exception();
  if (failure) {
    try {
      std::rethrow_exception(failure);
    } catch (const std::exception& error) {
      std::snprintf(line, sizeof(line), "the engine is ending: %s", error.what());
    } catch (...) {
      std::snprintf(line, sizeof(line), "the engine is ending with an unknown error");
    }
  } else {
    std::snprintf(line, sizeof(line), "the engine is ending: std::terminate was called with no active exception");
  }
  BX_LOG_ERROR("%s", line);
}

}  // namespace

void install_crash_handler() {
  // The report goes to the engine's own log, opened for append. The C runtime's file handle is not
  // used: the handler may not touch the runtime.
  const std::string& path = Log::instance().path();
  if (!path.empty()) {
    g_report = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  AddVectoredExceptionHandler(1, on_fault);
  std::set_terminate(on_terminate);
}

void report_fault() {
  const DWORD code = g_fault_code.exchange(0, std::memory_order_relaxed);
  if (code == 0) return;
  void* address = g_fault_address.exchange(nullptr, std::memory_order_relaxed);
  void* type = g_fault_type.exchange(nullptr, std::memory_order_relaxed);
  char where[160] = {0};
  HMODULE module = nullptr;
  if (address) {
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(address), &module);
  }
  if (module) {
    char name[MAX_PATH] = {0};
    GetModuleFileNameA(module, name, sizeof(name));
    const char* leaf = name;
    for (const char* c = name; *c; ++c) {
      if (*c == '\\' || *c == '/') leaf = c + 1;
    }
    std::snprintf(where, sizeof(where), "%s+0x%llx", leaf,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                reinterpret_cast<uintptr_t>(module)));
  }
  // The name of the type: the runtime's descriptor holds it after two pointers, and it is a mangled
  // name that starts with ".?AV". "system_error" is a deadlocked mutex, "bad_alloc" is memory, and
  // anything else is a bug of ours.
  char type_name[160] = {0};
  if (type && readable(type, 24)) {
    const char* name = *reinterpret_cast<const char* const*>(reinterpret_cast<const char*>(type) + 16);
    if (name && readable(name, 2)) {
      size_t length = 0;
      while (length < sizeof(type_name) - 1 && name[length]) ++length;
      memcpy(type_name, name, length);
      type_name[length] = '\0';
    }
  }
  // A warning, not an error: this is a first-chance exception, and the graphics backend raises and catches some of its own
  // on every start (one 0xe06d7363 in KERNELBASE at each launch). One that really ends the process is reported by
  // on_terminate (an error) and by Windows' own crash log, so an error line here only made a healthy start look broken.
  if (type_name[0]) {
    BX_LOG_WARN("an exception was raised (code 0x%08lx) in %s: %s", code, where, type_name);
  } else if (code == 0xe06d7363UL) {
    // A C++ throw with no type visible (it came from a module, not this binary) has been caught by
    // whoever threw it: libobs's graphics backend raises one at every launch. Debug, or every healthy
    // start reads as half broken.
    BX_LOG_DEBUG("a caught exception was raised (code 0x%08lx) in %s", code, where);
  } else {
    BX_LOG_WARN("an exception was raised (code 0x%08lx) in %s", code, where);
  }
  if (g_has_message.exchange(false, std::memory_order_relaxed) && g_fault_message[0]) {
    BX_LOG_WARN("  the exception says: %s", g_fault_message);
    g_fault_message[0] = '\0';
  }
}

}  // namespace bx
