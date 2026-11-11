// BuffrX package entry point.
//
// One job: make a broken package obvious before a single DLL is loaded. It verifies the pieces that
// must be present, checks the MSVC runtime that the pinned OBS binaries link against, then starts the
// engine, which draws its own panel. Keeping it tiny means it can still explain a problem when everything
// else fails.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string own_directory() {
  char buffer[MAX_PATH * 2] = {0};
  DWORD length = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
  std::string path(buffer, length);
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool msvc_runtime_present() {
  // The OBS binaries are MSVC-built: without the VC++ runtime, obs.dll cannot load and the package
  // looks broken for reasons that have nothing to do with BuffrX.
  char system32[MAX_PATH] = {0};
  if (GetSystemDirectoryA(system32, MAX_PATH) == 0) return true;
  const std::string root(system32);
  return exists(root + "\\vcruntime140.dll") && exists(root + "\\msvcp140.dll");
}

std::string quote(const std::string& value) { return "\"" + value + "\""; }

std::string narrow(const wchar_t* value) {
  // The launcher's manifest makes every ANSI call UTF-8 (launcher.manifest), so paths stay whole on a
  // non-English Windows where the system code page would turn them into question marks.
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
  out.pop_back();
  return out;
}

bool start_process(const std::string& command, DWORD flags) {
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::string mutable_command = command;
  if (!CreateProcessA(nullptr, &mutable_command[0], nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup,
                      &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

// Starts a process and waits for its exit code, or returns -1 when it could not be started.
int run_process(const std::string& command, DWORD flags) {
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::string mutable_command = command;
  if (!CreateProcessA(nullptr, &mutable_command[0], nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup,
                      &process)) {
    return -1;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}

// The copied launcher owns this mutex until Setup finishes, so another launch cannot load files being replaced.
int supervise_update(const std::string& directory, const std::string& data_root, DWORD parent, bool autostart) {
  HANDLE mutex = CreateMutexA(nullptr, TRUE, "Local\\BuffrX.Update.InProgress");
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (mutex) CloseHandle(mutex);
    return 1;
  }
  HANDLE parent_process = parent ? OpenProcess(SYNCHRONIZE, FALSE, parent) : nullptr;
  if (parent && !parent_process) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 1;
  }
  const std::string ready_name = "Local\\BuffrX.Update.Ready." + std::to_string(parent);
  HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, ready_name.c_str());
  if (ready) { SetEvent(ready); CloseHandle(ready); }
  if (parent_process) {
    WaitForSingleObject(parent_process, INFINITE);
    CloseHandle(parent_process);
  }
  const std::string update = data_root + "\\update";
  const std::string setup = update + "\\BuffrX-Setup.exe";
  const std::string command = quote(setup) + " /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /DIR=" +
                              quote(directory) + " /LOG=" + quote(update + "\\install.log");
  const int result = run_process(command, CREATE_NO_WINDOW);
  if (result != 0) {
    FILE* failure = std::fopen((update + "\\failed").c_str(), "wb");
    const bool recorded = failure && std::fclose(failure) == 0;
    DeleteFileA((update + "\\apply").c_str());
    if (!recorded)
      MessageBoxA(nullptr, "The update could not be installed. Open BuffrX to try again.", "BuffrX update", MB_ICONWARNING);
  }
  const std::string restart = quote(directory + "\\BuffrX.exe") + " --update-resume" +
                              (autostart ? " --autostart" : "");
  const bool started = start_process(restart, CREATE_NO_WINDOW);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  if (!started)
    MessageBoxA(nullptr, "BuffrX could not reopen after the update. Open it again from its installation folder.",
                "BuffrX update", MB_ICONERROR);
  return started ? 0 : 1;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR command_line, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && argc == 6 && std::wstring(argv[1]) == L"--update-supervisor") {
    const std::string directory = narrow(argv[2]);
    const std::string data_root = narrow(argv[3]);
    const DWORD parent = static_cast<DWORD>(std::strtoul(narrow(argv[4]).c_str(), nullptr, 10));
    const bool autostart = std::wstring(argv[5]) == L"1";
    LocalFree(argv);
    return supervise_update(directory, data_root, parent, autostart);
  }
  if (argv) LocalFree(argv);
  HANDLE updating = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, "Local\\BuffrX.Update.InProgress");
  if (updating) {
    const DWORD wait = WaitForSingleObject(updating, INFINITE);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) ReleaseMutex(updating);
    CloseHandle(updating);
  }
  // BuffrX.exe sits at the package root; the engine and the pinned runtime are in bin\64bit.
  const std::string directory = own_directory();
  const std::string engine = directory + "\\bin\\64bit\\buffrx-engine.exe";

  for (const std::string& required : {engine, directory + "\\bin\\64bit\\obs.dll",
                                      directory + "\\obs-plugins\\64bit\\win-capture.dll",
                                      directory + "\\data\\libobs"}) {
    if (!exists(required)) {
      MessageBoxA(nullptr,
                  ("Some of the BuffrX files did not arrive.\n\nUnzip the whole download again, then open BuffrX.\n\nMissing:\n" +
                   required)
                      .c_str(),
                  "BuffrX", MB_ICONERROR);
      return 1;
    }
  }

  if (!msvc_runtime_present()) {
    if (MessageBoxA(nullptr,
                    "BuffrX needs a free Microsoft part called Visual C++ 2015-2022, and this PC does not have it.\n\n"
                    "Open the download page?",
                    "BuffrX", MB_ICONWARNING | MB_YESNO) == IDYES) {
      ShellExecuteA(nullptr, "open", "https://aka.ms/vs/17/release/vc_redist.x64.exe", nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 2;
  }

  const std::string arguments = command_line ? command_line : "";
  const bool minimized = arguments.find("--minimized") != std::string::npos;
  const bool autostart = arguments.find("--autostart") != std::string::npos;
  const bool update_resume = arguments.find("--update-resume") != std::string::npos;

  // The engine is the whole app: it draws its own panel. A second start finds the first engine running
  // and asks it to show the panel instead.
  std::string engine_arguments = autostart ? " --autostart" : minimized ? " --minimized" : " --show";
  if (update_resume) engine_arguments += " --update-resume";

  // The engine reads its own config and asks for elevation if needed, whichever entry point starts it.
  if (!start_process(quote(engine) + engine_arguments, CREATE_NO_WINDOW)) {
    MessageBoxA(nullptr, "BuffrX could not start its engine.", "BuffrX", MB_ICONERROR);
    return 3;
  }
  return 0;
}
