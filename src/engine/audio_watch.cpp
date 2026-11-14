// Windows audio session events -> a session list. See audio_watch.h for the contract.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "audio_watch.h"

#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "../shared/log.h"

namespace bx {
namespace {

// A missed session-created notification is repaired this slowly. Nothing here is a polling loop: the
// wait below is interrupted by every event.
constexpr uint32_t kRepairMs = 30000;
// The longest the worker sleeps with no events. It only re-checks process handles and prunes; it does
// not enumerate sessions.
constexpr DWORD kIdleWaitMs = 1000;

template <class T>
void release(T*& pointer) {
  if (pointer) {
    pointer->Release();
    pointer = nullptr;
  }
}

std::string narrow(const wchar_t* text) {
  if (!text || !*text) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return {};
  std::string out(static_cast<size_t>(bytes - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
  return out;
}

std::string process_image_name(DWORD pid) {
  if (pid == 0) return {};
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};
  wchar_t path[MAX_PATH]{};
  DWORD size = MAX_PATH;
  std::string name;
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    const std::wstring wide(path, size);
    const size_t slash = wide.find_last_of(L"\\/");
    name = narrow((slash == std::wstring::npos ? wide : wide.substr(slash + 1)).c_str());
  }
  CloseHandle(process);
  return name;
}

struct Core;
class SessionEvents;
class DeviceNotifications;
class SessionNotifications;

struct Entry {
  WatchedSession view;
  HANDLE process = nullptr;  // SYNCHRONIZE, so the worker's wait can name an exit
  IAudioSessionControl* control = nullptr;
  SessionEvents* events = nullptr;
};

struct Pending {
  enum class Kind { Created, State, Disconnected, DeviceChanged } kind = Kind::DeviceChanged;
  std::string key;
  AudioSessionState state = AudioSessionStateInactive;
  IAudioSessionControl* control = nullptr;  // AddRef'd by the callback until the worker takes it
};

// The COM objects hold this alive; the watcher holds it too. That is what makes a callback that races
// stop() harmless: it finds running_ false, drops its event and releases its reference.
struct Core : std::enable_shared_from_this<Core> {
  mutable std::mutex mutex;
  std::condition_variable ready;
  bool starting = false;
  std::atomic<bool> start_done{false};
  std::atomic<bool> start_ok{false};
  std::string error;                       // guarded by mutex
  std::vector<Pending> pending;            // guarded by mutex
  std::map<std::string, Entry> entries;    // guarded by mutex; COM members belong to the worker
  std::atomic<bool> running{false};
  std::atomic<uint64_t> generation{0};
  HANDLE wake = nullptr;
  std::thread worker;
  AudioSessionWatcher::Wake wake_fn;

  ~Core() {
    if (wake) CloseHandle(wake);
  }

  // Worker only.
  IMMDeviceEnumerator* enumerator = nullptr;
  DeviceNotifications* device_notify = nullptr;
  SessionNotifications* session_notify = nullptr;
  std::vector<std::pair<IMMDevice*, IAudioSessionManager2*>> devices;
  uint64_t next_repair_ms = 0;

  void set_error(std::string text) {
    std::lock_guard<std::mutex> lock(mutex);
    error = std::move(text);
  }
  void clear_error() {
    std::lock_guard<std::mutex> lock(mutex);
    error.clear();
  }
  void post(Pending item) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!running.load()) {
        if (item.control) item.control->Release();
        return;
      }
      pending.push_back(std::move(item));
    }
    if (wake) SetEvent(wake);
  }

  bool bind_devices();
  void unbind_devices();
  void register_session(IAudioSessionControl* control);
  void apply_state(const std::string& key, AudioSessionState state);
  void apply_disconnected(const std::string& key);
  void prune();
  void drain();
  void run();
};

class SessionEvents final : public IAudioSessionEvents {
 public:
  SessionEvents(std::shared_ptr<Core> core, std::string key) : core_(std::move(core)), key_(std::move(key)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    if (riid == __uuidof(IAudioSessionEvents) || riid == __uuidof(IUnknown)) {
      *object = static_cast<IAudioSessionEvents*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
  STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
  STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
  STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
  STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
  STDMETHODIMP OnStateChanged(AudioSessionState state) override {
    Pending item;
    item.kind = Pending::Kind::State;
    item.key = key_;
    item.state = state;
    core_->post(std::move(item));
    return S_OK;
  }
  STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
    Pending item;
    item.kind = Pending::Kind::Disconnected;
    item.key = key_;
    core_->post(std::move(item));
    return S_OK;
  }

 private:
  ~SessionEvents() = default;
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<Core> core_;
  std::string key_;
};

// Endpoint arrivals, removals and default changes are one "rebind" event; the worker coalesces them.
class DeviceNotifications final : public IMMNotificationClient {
 public:
  explicit DeviceNotifications(std::shared_ptr<Core> core) : core_(std::move(core)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    if (riid == __uuidof(IMMNotificationClient) || riid == __uuidof(IUnknown)) {
      *object = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return changed(); }
  STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return changed(); }
  STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return changed(); }
  STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return changed(); }
  STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  ~DeviceNotifications() = default;
  STDMETHODIMP changed() {
    Pending item;
    item.kind = Pending::Kind::DeviceChanged;
    core_->post(std::move(item));
    return S_OK;
  }
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<Core> core_;
};

// A session's creation notification can arrive before the enumerator lists it, and a session created
// between enumeration and registration would otherwise be invisible forever. The callback AddRefs the
// control and the worker registers it on its own thread.
class SessionNotifications final : public IAudioSessionNotification {
 public:
  explicit SessionNotifications(std::shared_ptr<Core> core) : core_(std::move(core)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    if (riid == __uuidof(IAudioSessionNotification) || riid == __uuidof(IUnknown)) {
      *object = static_cast<IAudioSessionNotification*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  STDMETHODIMP OnSessionCreated(IAudioSessionControl* session) override {
    if (!session) return S_OK;
    session->AddRef();
    Pending item;
    item.kind = Pending::Kind::Created;
    item.control = session;
    core_->post(std::move(item));
    return S_OK;
  }

 private:
  ~SessionNotifications() = default;
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<Core> core_;
};

void Core::register_session(IAudioSessionControl* control) {
  if (!control) return;
  IAudioSessionControl2* control2 = nullptr;
  if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) ||
      !control2) {
    return;
  }

  DWORD pid = 0;
  control2->GetProcessId(&pid);
  const bool system_sounds = control2->IsSystemSoundsSession() == S_OK;

  LPWSTR instance = nullptr;
  std::string key;
  if (SUCCEEDED(control2->GetSessionInstanceIdentifier(&instance)) && instance) {
    key = narrow(instance);
    CoTaskMemFree(instance);
  }
  control2->Release();
  if (key.empty()) {
    wchar_t fallback[64];
    std::swprintf(fallback, 64, L"pid:%lu:%p", static_cast<unsigned long>(pid), static_cast<void*>(control));
    key = narrow(fallback);
  }

  AudioSessionState state = AudioSessionStateInactive;
  control->GetState(&state);
  if (state == AudioSessionStateExpired) {
    apply_state(key, state);
    return;
  }
  const bool active = state == AudioSessionStateActive;
  const uint64_t now = GetTickCount64();

  {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = entries.find(key);
    if (found != entries.end()) {
      Entry& entry = found->second;
      if (entry.view.active != active) {
        entry.view.active = active;
        entry.view.changed_ms = now;
        generation.fetch_add(1);
      }
      return;
    }
  }

  Entry entry;
  entry.view.key = key;
  entry.view.pid = pid;
  entry.view.exe = system_sounds ? std::string() : process_image_name(pid);
  entry.view.system_sounds = system_sounds;
  entry.view.active = active;
  entry.view.changed_ms = now;
  entry.control = control;
  control->AddRef();
  entry.events = new SessionEvents(shared_from_this(), key);
  if (FAILED(control->RegisterAudioSessionNotification(entry.events))) {
    // Without events the session still appears; it just cannot report a state change, and the slow
    // repair re-reads it. Keep it rather than hiding a program that is plainly playing.
  }
  if (pid != 0) {
    entry.process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    // Two threads cannot register the same key: only the worker inserts, but a duplicate notification
    // between our lookup and here is still possible because the lookup released the lock.
    if (entries.find(key) != entries.end()) {
      control->UnregisterAudioSessionNotification(entry.events);
      if (entry.control) entry.control->Release();
      if (entry.events) entry.events->Release();
      if (entry.process) CloseHandle(entry.process);
      return;
    }
    entries.emplace(key, std::move(entry));
    generation.fetch_add(1);
  }
}

void Core::apply_state(const std::string& key, AudioSessionState state) {
  const uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> lock(mutex);
  auto found = entries.find(key);
  if (found == entries.end()) return;
  Entry& entry = found->second;
  if (state == AudioSessionStateExpired) {
    // Drop it on the worker: the COM objects cannot be released while the caller may read a snapshot.
    entry.view.active = false;
    entry.view.changed_ms = 0;  // zero means "expired"; prune() takes it now
    generation.fetch_add(1);
    return;
  }
  const bool active = state == AudioSessionStateActive;
  if (entry.view.active == active) return;
  entry.view.active = active;
  entry.view.changed_ms = now;
  generation.fetch_add(1);
}

void Core::apply_disconnected(const std::string& key) {
  const uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> lock(mutex);
  auto found = entries.find(key);
  if (found == entries.end()) return;
  Entry& entry = found->second;
  if (!entry.view.active) return;
  entry.view.active = false;
  entry.view.changed_ms = now;
  generation.fetch_add(1);
}

void Core::prune() {
  std::vector<Entry> dead;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = entries.begin(); it != entries.end();) {
      Entry& entry = it->second;
      const bool exited = entry.process && WaitForSingleObject(entry.process, 0) == WAIT_OBJECT_0;
      const bool gone = watched_session_remove(entry.view.active, entry.view.changed_ms, exited);
      if (gone) {
        dead.push_back(std::move(entry));
        it = entries.erase(it);
        generation.fetch_add(1);
      } else {
        ++it;
      }
    }
  }
  for (Entry& entry : dead) {
    if (entry.control && entry.events) entry.control->UnregisterAudioSessionNotification(entry.events);
    if (entry.events) entry.events->Release();
    if (entry.control) entry.control->Release();
    if (entry.process) CloseHandle(entry.process);
  }
}

bool Core::bind_devices() {
  if (!enumerator) {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))) ||
        !enumerator) {
      set_error("the audio device enumerator could not be opened");
      return false;
    }
    device_notify = new DeviceNotifications(shared_from_this());
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(device_notify))) {
      // The watcher still works for devices already present; it just misses a new one until repaired.
    }
  }

  std::vector<IMMDevice*> found;
  IMMDeviceCollection* collection = nullptr;
  if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) || !collection) {
    set_error("the audio endpoints could not be listed");
    return false;
  }
  UINT count = 0;
  collection->GetCount(&count);
  for (UINT index = 0; index < count; ++index) {
    IMMDevice* device = nullptr;
    if (SUCCEEDED(collection->Item(index, &device)) && device) found.push_back(device);
  }
  collection->Release();

  if (!session_notify) session_notify = new SessionNotifications(shared_from_this());
  for (IMMDevice* device : found) {
    IAudioSessionManager2* manager = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr,
                                reinterpret_cast<void**>(&manager))) ||
        !manager) {
      device->Release();
      continue;
    }
    manager->RegisterSessionNotification(session_notify);

    IAudioSessionEnumerator* sessions = nullptr;
    if (SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && sessions) {
      int session_count = 0;
      sessions->GetCount(&session_count);
      for (int session = 0; session < session_count; ++session) {
        IAudioSessionControl* control = nullptr;
        if (SUCCEEDED(sessions->GetSession(session, &control)) && control) {
          register_session(control);
          control->Release();
        }
      }
      sessions->Release();
    }
    devices.emplace_back(device, manager);
  }
  clear_error();
  return true;
}

void Core::unbind_devices() {
  for (auto& binding : devices) {
    IAudioSessionManager2* manager = binding.second;
    IMMDevice* device = binding.first;
    if (manager) {
      if (session_notify) manager->UnregisterSessionNotification(session_notify);
      release(manager);
    }
    if (device) release(device);
  }
  devices.clear();
  release(session_notify);
  if (enumerator && device_notify) enumerator->UnregisterEndpointNotificationCallback(device_notify);
  release(device_notify);
  release(enumerator);
}

void Core::drain() {
  std::vector<Pending> items;
  {
    std::lock_guard<std::mutex> lock(mutex);
    items.swap(pending);
  }
  bool touched = false;
  for (Pending& item : items) {
    switch (item.kind) {
      case Pending::Kind::Created:
        if (item.control) {
          register_session(item.control);
          item.control->Release();
          item.control = nullptr;
        }
        touched = true;
        break;
      case Pending::Kind::State:
        apply_state(item.key, item.state);
        touched = true;
        break;
      case Pending::Kind::Disconnected:
        apply_disconnected(item.key);
        touched = true;
        break;
      case Pending::Kind::DeviceChanged:
        unbind_devices();
        bind_devices();
        touched = true;
        break;
    }
    if (item.control) item.control->Release();
  }
  if (touched && wake_fn && running.load()) wake_fn();
}

void Core::run() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_ok = SUCCEEDED(com);
  if (!com_ok) set_error("COM could not be started on the audio session thread");
  if (com_ok) bind_devices();
  next_repair_ms = GetTickCount64() + kRepairMs;
  {
    std::lock_guard<std::mutex> lock(mutex);
    start_ok = com_ok && error.empty();
    start_done = true;
  }
  ready.notify_all();
  if (!start_ok) running.store(false);

  while (running.load()) {
    const uint64_t now = GetTickCount64();
    if (watched_repair_due(now, next_repair_ms)) {
      // Event-loss repair: re-read the devices without tearing registrations down. Slow on purpose.
      unbind_devices();
      bind_devices();
      prune();
      next_repair_ms = now + kRepairMs;
    }
    uint64_t wait_ms = kIdleWaitMs;
    if (next_repair_ms > now) wait_ms = std::min<uint64_t>(wait_ms, next_repair_ms - now);
    if (wake) WaitForSingleObject(wake, static_cast<DWORD>(wait_ms));
    drain();
    prune();
  }

  // Teardown on the worker, which owns every COM object here.
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (Pending& item : pending) {
      if (item.control) item.control->Release();
    }
    pending.clear();
  }
  std::vector<Entry> dead;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& item : entries) dead.push_back(std::move(item.second));
    entries.clear();
  }
  for (Entry& entry : dead) {
    if (entry.control && entry.events) entry.control->UnregisterAudioSessionNotification(entry.events);
    if (entry.events) entry.events->Release();
    if (entry.control) entry.control->Release();
    if (entry.process) CloseHandle(entry.process);
  }
  unbind_devices();
  if (com_ok) CoUninitialize();
}

}  // namespace

struct AudioSessionWatcher::Impl {
  std::shared_ptr<Core> core;
};

AudioSessionWatcher::AudioSessionWatcher() = default;

AudioSessionWatcher::~AudioSessionWatcher() { stop(); }

bool AudioSessionWatcher::begin(Wake wake) {
  if (running()) return true;
  if (impl_) stop();

  auto impl = std::make_shared<Impl>();
  impl->core = std::make_shared<Core>();
  Core& core = *impl->core;
  core.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!core.wake) {
    core.set_error("the audio session wake event could not be created");
    impl_ = impl;
    return false;
  }
  core.wake_fn = std::move(wake);
  core.running.store(true);
  core.starting = true;
  try {
    core.worker = std::thread([shared = impl->core] { shared->run(); });
  } catch (...) {
    core.running.store(false);
    core.set_error("the audio session thread could not be started");
    impl_ = impl;
    return false;
  }
  impl_ = impl;
  return true;
}

bool AudioSessionWatcher::start(Wake wake) {
  if (!begin(std::move(wake))) return false;
  const std::shared_ptr<Core> core = impl_->core;
  {
    std::unique_lock<std::mutex> lock(core->mutex);
    core->ready.wait_for(lock, std::chrono::seconds(3), [&core] { return core->start_done.load(); });
  }
  if (!core->start_done) core->set_error("the audio session watcher did not start in time");
  if (!core->start_ok) {
    core->running.store(false);
    if (core->wake) SetEvent(core->wake);
    if (core->worker.joinable()) core->worker.join();
    return false;
  }
  return true;
}

bool AudioSessionWatcher::ready() const {
  return impl_ && impl_->core && impl_->core->running.load() && impl_->core->start_done && impl_->core->start_ok;
}

void AudioSessionWatcher::stop() {
  if (!impl_) return;
  const std::shared_ptr<Core> core = impl_->core;
  impl_.reset();
  if (!core) return;
  core->running.store(false);
  if (core->wake) SetEvent(core->wake);
  if (core->worker.joinable()) core->worker.join();
  core->wake_fn = nullptr;
}

bool AudioSessionWatcher::running() const {
  return impl_ && impl_->core && impl_->core->running.load();
}

std::vector<WatchedSession> AudioSessionWatcher::sessions(uint64_t now_ms, uint32_t grace_ms) const {
  std::vector<WatchedSession> out;
  if (!impl_ || !impl_->core) return out;
  const std::shared_ptr<Core> core = impl_->core;
  std::lock_guard<std::mutex> lock(core->mutex);
  out.reserve(core->entries.size());
  for (const auto& item : core->entries) {
    const WatchedSession& view = item.second.view;
    if (watched_session_visible(view.active, view.changed_ms, now_ms, grace_ms)) out.push_back(view);
  }
  return out;
}

uint64_t AudioSessionWatcher::generation() const {
  return impl_ && impl_->core ? impl_->core->generation.load() : 0;
}

std::string AudioSessionWatcher::error() const {
  if (!impl_ || !impl_->core) return "the audio session watcher is not running";
  const std::shared_ptr<Core> core = impl_->core;
  std::lock_guard<std::mutex> lock(core->mutex);
  return core->error;
}

}  // namespace bx
