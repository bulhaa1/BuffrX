// The session watcher against a real audio session of our own: active, inactive, the grace window,
// a relaunch, and repeated start/stop without leaving threads behind. Test-only; it spawns
// buffrx-audio-fixture.exe (never packaged).
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../src/engine/audio_watch.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  std::printf("  FAIL %s\n", what);
  ++g_failures;
}

std::wstring fixture_path() {
  wchar_t path[MAX_PATH * 2]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
  std::wstring full(path, length);
  const size_t slash = full.find_last_of(L"\\/");
  return (slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1)) +
         L"buffrx-audio-fixture.exe";
}

struct Fixture {
  HANDLE process = nullptr;
  HANDLE input = nullptr;
  uint32_t pid = 0;

  bool start(const wchar_t* mode) {
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) return false;
    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);

    std::wstring exe = fixture_path();
    std::wstring command = L"\"" + exe + L"\" " + (mode ? mode : L"");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = read_end;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &startup, &info);
    CloseHandle(read_end);
    if (!ok) {
      std::printf("  fixture did not start (%lu)\n", static_cast<unsigned long>(GetLastError()));
      CloseHandle(write_end);
      return false;
    }
    CloseHandle(info.hThread);
    process = info.hProcess;
    pid = info.dwProcessId;
    input = write_end;
    return true;
  }

  bool send(const char* line) {
    if (!input) return false;
    const std::string text = std::string(line) + "\n";
    DWORD written = 0;
    return WriteFile(input, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0;
  }

  void kill() {
    if (input) {
      CloseHandle(input);
      input = nullptr;
    }
    if (process) {
      TerminateProcess(process, 0);
      WaitForSingleObject(process, 3000);
      CloseHandle(process);
      process = nullptr;
    }
  }

  ~Fixture() { kill(); }
};

uint32_t current_threads() {
  const DWORD pid = GetCurrentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  uint32_t count = 0;
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID == pid) ++count;
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return count;
}

bool wait_until(const std::function<bool()>& predicate, uint32_t timeout_ms) {
  const uint64_t deadline = GetTickCount64() + timeout_ms;
  while (GetTickCount64() < deadline) {
    if (predicate()) return true;
    Sleep(50);
  }
  return predicate();
}

struct Session {
  bool found = false;
  bx::WatchedSession value;
};

Session session_for(bx::AudioSessionWatcher& watcher, uint32_t pid, bool want_active, uint32_t grace_ms = 1500) {
  Session result;
  for (const bx::WatchedSession& session : watcher.sessions(GetTickCount64(), grace_ms)) {
    if (session.pid != pid) continue;
    if (session.active == want_active) {
      result.found = true;
      result.value = session;
      return result;
    }
  }
  return result;
}

}  // namespace

int main() {
  std::printf("audio session watcher (live)\n");

  Fixture fixture;
  expect(fixture.start(L"silence"), "the test fixture starts");
  if (!fixture.process) return 1;

  std::atomic<uint32_t> wakes{0};
  bx::AudioSessionWatcher watcher;
  const bool started = watcher.start([&wakes] { wakes.fetch_add(1); });
  expect(started, "the watcher starts");
  if (!started) {
    std::printf("  error: %s\n", watcher.error().c_str());
    return 1;
  }
  expect(watcher.running(), "the watcher reports it is running");
  expect(watcher.error().empty(), "a working watcher reports no error");

  // The fixture's own stream appears as an active session.
  expect(wait_until([&] { return session_for(watcher, fixture.pid, true).found; }, 6000),
         "the fixture's active session appears");
  expect(watcher.generation() > 0, "its appearance bumps the generation");
  expect(wakes.load() > 0, "the host is woken for it");

  const uint64_t before_stop = GetTickCount64();
  expect(fixture.send("stop"), "the fixture accepts stop");
  const bool went_inactive = wait_until([&] {
    const Session session = session_for(watcher, fixture.pid, false);
    return session.found && !session.value.active;
  }, 5000);
  expect(went_inactive, "stopping the stream reports the session inactive");
  if (went_inactive) {
    const Session session = session_for(watcher, fixture.pid, false);
    expect(session.value.changed_ms >= before_stop, "its change time is the stop, on the monotonic clock");
    // Still listed inside the grace window, so a brief pause does not tear a capture down.
    bool inside_grace = false;
    for (const bx::WatchedSession& value : watcher.sessions(GetTickCount64(), 1500)) {
      if (value.pid == fixture.pid) inside_grace = true;
    }
    expect(inside_grace, "an inactive session stays visible through the grace window");
    Sleep(2200);
    bool after_grace = false;
    for (const bx::WatchedSession& value : watcher.sessions(GetTickCount64(), 1500)) {
      if (value.pid == fixture.pid) after_grace = true;
    }
    expect(!after_grace, "and is gone from the snapshot once the grace window passes");
  }

  expect(fixture.send("start"), "the fixture accepts start");
  expect(wait_until([&] { return session_for(watcher, fixture.pid, true).found; }, 5000),
         "starting again reports the same session active");

  expect(fixture.send("quit"), "the fixture accepts quit");
  expect(wait_until([&] {
           for (const bx::WatchedSession& value : watcher.sessions(GetTickCount64(), 1500)) {
             if (value.pid == fixture.pid) return false;
           }
           return true;
         }, 6000),
         "the session leaves the snapshot when the process exits");
  fixture.kill();

  // A relaunch is a new process and a new session.
  Fixture second;
  expect(second.start(L"silence"), "a second fixture starts");
  expect(wait_until([&] { return session_for(watcher, second.pid, true).found; }, 6000),
         "the relaunched process appears as its own session");
  second.kill();
  expect(wait_until([&] {
           for (const bx::WatchedSession& value : watcher.sessions(GetTickCount64(), 1500)) {
             if (value.pid == second.pid) return false;
           }
           return true;
         }, 6000),
         "and leaves when it exits");

  // start/stop must not leak threads or hang. The first cycle warms COM and the device.
  watcher.stop();
  expect(!watcher.running(), "stopping the watcher reports it stopped");
  const uint32_t warm = current_threads();
  uint64_t slowest_stop = 0;
  for (int cycle = 0; cycle < 5; ++cycle) {
    const uint64_t begin = GetTickCount64();
    const bool again = watcher.start([] {});
    const uint64_t stop_begin = GetTickCount64();
    watcher.stop();
    slowest_stop = std::max<uint64_t>(slowest_stop, GetTickCount64() - stop_begin);
    expect(again, "the watcher restarts");
    (void)begin;
  }
  const uint32_t after = current_threads();
  expect(slowest_stop < 2000, "every stop returns within two seconds");
  expect(after <= warm + 2, "repeated start/stop leaves no thread behind");
  std::printf("  threads: %u warm, %u after five cycles; slowest stop %llu ms\n", warm, after,
              static_cast<unsigned long long>(slowest_stop));
  std::printf("  (the watcher was proven with a silent WASAPI stream: the fixture makes no sound)\n");

  if (g_failures == 0) std::printf("audio session watcher: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
