// Process-tree audio capture as a libobs source. See process_audio_source.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "process_audio_source.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <obs.h>

#include "../shared/log.h"

namespace bx {
namespace {

// The values and layout of AUDIOCLIENT_ACTIVATION_PARAMS / PROCESS_LOOPBACK_MODE (Windows 10 2004+).
// The toolchain's MinGW headers do not carry audioclientactivationparams.h, and pulling the MSVC SDK's
// copy into a MinGW build mixes two header sets; the ABI is three integers either way.
constexpr wchar_t kProcessLoopbackDevice[] = L"VAD\\Process_Loopback";
constexpr int32_t kActivationProcessLoopback = 1;
constexpr int32_t kLoopbackIncludeTree = 0;

struct ProcessLoopbackParams {
  DWORD target_pid;
  int32_t mode;
};
struct ActivationParams {
  int32_t type;
  ProcessLoopbackParams loopback;
};
static_assert(sizeof(ActivationParams) == 12, "AUDIOCLIENT_ACTIVATION_PARAMS is two 32-bit words and a DWORD");

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled here so no ksmedia.h is needed.
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// IAgileObject: ActivateAudioInterfaceAsync refuses a completion handler that does not answer this
// marker interface with E_ILLEGAL_METHOD_CALL. It has no methods; answering is the whole promise.
constexpr GUID kIidAgileObject = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};

// The buffer WASAPI is asked for. Process loopback's documented sample asks for the engine's own
// default (0); OBS asks for five seconds. Zero is the documented path, so it is the one used here.
constexpr REFERENCE_TIME kBufferDuration = 0;
constexpr DWORD kActivationTimeoutMs = 5000;
constexpr DWORD kCaptureTickMs = 10;  // a loopback event does not always fire; 10 ms is OBS's own timer
constexpr int kTransientAttempts = 3;

std::string hresult_text(HRESULT hr) {
  char text[64];
  std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
  return text;
}

struct Status {
  std::atomic<uint32_t> pid{0};
  std::atomic<bool> ready{false};
  std::atomic<bool> failed{false};
  mutable std::mutex mutex;
  std::string error;  // guarded by mutex

  void set_error(std::string text) {
    std::lock_guard<std::mutex> lock(mutex);
    error = std::move(text);
  }
  std::string get_error() const {
    std::lock_guard<std::mutex> lock(mutex);
    return error;
  }
  void mark_ready() {
    std::lock_guard<std::mutex> lock(mutex);
    error.clear();
    failed.store(false);
    ready.store(true);
  }
  void mark_stopped() { ready.store(false); }
  void mark_failed(std::string text) {
    std::lock_guard<std::mutex> lock(mutex);
    error = std::move(text);
    ready.store(false);
    failed.store(true);
  }
};

struct ActivationState {
  HANDLE done = nullptr;
  std::mutex mutex;
  HRESULT result = E_PENDING;
  IAudioClient* client = nullptr;

  ~ActivationState() {
    if (client) client->Release();
    if (done) CloseHandle(done);
  }
};

// Completion runs on a Windows thread-pool thread. It only fills the shared state and signals; every
// COM release happens in the destructor or on the source's own worker.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
 public:
  explicit ActivationHandler(std::shared_ptr<ActivationState> activation) : activation_(std::move(activation)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    if (riid == __uuidof(IActivateAudioInterfaceCompletionHandler) || riid == __uuidof(IUnknown)) {
      *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == kIidAgileObject) {
      *object = static_cast<IUnknown*>(static_cast<IActivateAudioInterfaceCompletionHandler*>(this));
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

  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
    IUnknown* unknown = nullptr;
    HRESULT activated = E_FAIL;
    HRESULT hr = operation ? operation->GetActivateResult(&activated, &unknown) : E_POINTER;
    if (SUCCEEDED(hr)) hr = activated;
    if (SUCCEEDED(hr) && !unknown) hr = E_NOINTERFACE;
    {
      std::lock_guard<std::mutex> lock(activation_->mutex);
      if (activation_->client) activation_->client->Release();
      activation_->client = SUCCEEDED(hr) ? static_cast<IAudioClient*>(unknown) : nullptr;
      activation_->result = hr;
    }
    if (activation_->done) SetEvent(activation_->done);
    return S_OK;
  }

 private:
  ~ActivationHandler() = default;
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<ActivationState> activation_;
};

using ActivateAudioInterfaceAsyncFn = HRESULT(STDAPICALLTYPE*)(LPCWSTR, REFIID, PROPVARIANT*,
                                                               IActivateAudioInterfaceCompletionHandler*,
                                                               IActivateAudioInterfaceAsyncOperation**);

ActivateAudioInterfaceAsyncFn activate_audio_interface_async() {
  static ActivateAudioInterfaceAsyncFn fn = [] {
    HMODULE module = GetModuleHandleW(L"Mmdevapi.dll");
    if (!module) module = LoadLibraryW(L"Mmdevapi.dll");  // never freed: the process owns it
    return module ? reinterpret_cast<ActivateAudioInterfaceAsyncFn>(GetProcAddress(module, "ActivateAudioInterfaceAsync"))
                  : nullptr;
  }();
  return fn;
}

DWORD channel_mask_for(speaker_layout speakers, uint32_t channels) {
  if (speakers == SPEAKERS_STEREO) return 0x3;  // KSAUDIO_SPEAKER_STEREO
  if (channels >= 31) return 0;
  return (1u << channels) - 1u;
}

// Builds the format the source produces: libobs's own mix format, which is float (WASAPI process
// loopback is float only) at the audio output's rate and speaker layout.
WAVEFORMATEXTENSIBLE capture_format(speaker_layout& speakers, uint32_t& rate, uint32_t& channels) {
  obs_audio_info info{};
  if (obs_get_audio_info(&info) && info.samples_per_sec >= 8000) {
    speakers = info.speakers;
    rate = info.samples_per_sec;
  } else {
    speakers = SPEAKERS_STEREO;
    rate = 48000;
  }
  channels = get_audio_channels(speakers);
  if (channels == 0) {
    speakers = SPEAKERS_STEREO;
    channels = 2;
  }

  WAVEFORMATEXTENSIBLE format{};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = static_cast<WORD>(channels);
  format.Format.nSamplesPerSec = rate;
  format.Format.wBitsPerSample = 32;
  format.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
  format.Format.nAvgBytesPerSec = rate * format.Format.nBlockAlign;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = 32;
  format.dwChannelMask = channel_mask_for(speakers, channels);
  format.SubFormat = kSubtypeFloat;
  return format;
}

struct SourceData {
  obs_source* source = nullptr;
  std::shared_ptr<Status> status;
  std::thread worker;
  HANDLE stop = nullptr;
  uint32_t pid = 0;
  std::mutex retarget;  // one update at a time; a retarget waits for the old thread to finish
};

std::mutex g_registry_mutex;
std::unordered_map<obs_source*, std::shared_ptr<Status>> g_registry;

enum class CaptureResult { stopped, process_exited, transient, permanent };

struct CaptureContext {
  SourceData* data;
  HANDLE process;
  uint32_t pid;
};

// A retry is only for a device that may come back; an unsupported activation is reported at once so the
// host can say why and the user can choose desktop audio explicitly instead.
constexpr HRESULT kIllegalMethodCall = static_cast<HRESULT>(0x8000000Eu);  // E_ILLEGAL_METHOD_CALL

bool activation_is_permanent(HRESULT hr) {
  switch (hr) {
    case E_NOTIMPL:
    case E_NOINTERFACE:
    case E_ACCESSDENIED:
    case E_INVALIDARG:
    case E_POINTER:
    case kIllegalMethodCall:
      return true;
    default:
      return false;
  }
}

HRESULT activate_process_client(uint32_t pid, HANDLE stop, IAudioClient** client) {
  *client = nullptr;
  const ActivateAudioInterfaceAsyncFn activate = activate_audio_interface_async();
  if (!activate) return E_NOTIMPL;

  auto activation = std::make_shared<ActivationState>();
  activation->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!activation->done) return HRESULT_FROM_WIN32(GetLastError());

  ActivationParams params{};
  params.type = kActivationProcessLoopback;
  params.loopback.target_pid = pid;
  params.loopback.mode = kLoopbackIncludeTree;
  PROPVARIANT activate_params{};
  activate_params.vt = VT_BLOB;
  activate_params.blob.cbSize = sizeof(params);
  activate_params.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

  ActivationHandler* handler = new ActivationHandler(activation);
  IActivateAudioInterfaceAsyncOperation* operation = nullptr;
  const HRESULT started = activate(kProcessLoopbackDevice, __uuidof(IAudioClient), &activate_params, handler,
                                   &operation);
  handler->Release();
  if (FAILED(started)) {
    if (operation) operation->Release();
    return started;
  }

  // A destroy must not wait for an activation that never answers: the stop event ends this wait, the
  // handler keeps the shared state alive, and whatever it receives later is released by it. The
  // operation stays referenced until the wait ends, as its documented lifetime ties the handler to it.
  const HANDLE waits[] = {stop, activation->done};
  const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, kActivationTimeoutMs);
  if (operation) operation->Release();
  if (waited == WAIT_OBJECT_0) return E_ABORT;
  if (waited != WAIT_OBJECT_0 + 1) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);

  std::lock_guard<std::mutex> lock(activation->mutex);
  if (FAILED(activation->result) || !activation->client) {
    return FAILED(activation->result) ? activation->result : E_NOINTERFACE;
  }
  *client = activation->client;
  activation->client = nullptr;
  return S_OK;
}

CaptureResult capture_once(CaptureContext& context, HANDLE stop, bool& ran) {
  Status& status = *context.data->status;

  IAudioClient* client = nullptr;
  HRESULT hr = activate_process_client(context.pid, stop, &client);
  if (hr == E_ABORT) return CaptureResult::stopped;
  if (FAILED(hr)) {
    status.set_error("process audio activation failed (" + hresult_text(hr) + ")");
    return activation_is_permanent(hr) ? CaptureResult::permanent : CaptureResult::transient;
  }

  speaker_layout speakers = SPEAKERS_STEREO;
  uint32_t rate = 48000;
  uint32_t channels = 2;
  const WAVEFORMATEXTENSIBLE format = capture_format(speakers, rate, channels);

  const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &format.Format, nullptr);
  if (FAILED(hr)) {
    client->Release();
    status.set_error("the process audio client refused the format (" + hresult_text(hr) + ")");
    return CaptureResult::transient;
  }

  HANDLE capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!capture_event) {
    client->Release();
    status.set_error("the audio wake event could not be created");
    return CaptureResult::transient;
  }

  IAudioCaptureClient* capture = nullptr;
  hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
  if (SUCCEEDED(hr) && capture) hr = client->SetEventHandle(capture_event);
  if (SUCCEEDED(hr) && capture) hr = client->Start();
  if (FAILED(hr) || !capture) {
    if (capture) capture->Release();
    client->Release();
    CloseHandle(capture_event);
    status.set_error("the process audio capture could not start (" + hresult_text(hr) + ")");
    return CaptureResult::transient;
  }

  status.mark_ready();
  ran = true;
  std::vector<float> silence;
  CaptureResult result = CaptureResult::transient;

  const HANDLE waits[] = {stop, context.process, capture_event};
  for (;;) {
    const DWORD waited = WaitForMultipleObjects(3, waits, FALSE, kCaptureTickMs);
    if (waited == WAIT_OBJECT_0) {
      result = CaptureResult::stopped;
      break;
    }
    if (waited == WAIT_OBJECT_0 + 1) {
      status.set_error("the process exited");
      result = CaptureResult::process_exited;
      break;
    }
    if (waited == WAIT_FAILED) {
      status.set_error("waiting for process audio failed (" + hresult_text(HRESULT_FROM_WIN32(GetLastError())) + ")");
      result = CaptureResult::transient;
      break;
    }

    UINT32 next = 0;
    hr = capture->GetNextPacketSize(&next);
    if (FAILED(hr)) {
      status.set_error("reading process audio failed (" + hresult_text(hr) + ")");
      result = CaptureResult::transient;
      break;
    }

    bool broken = false;
    while (next > 0) {
      BYTE* buffer = nullptr;
      UINT32 frames = 0;
      DWORD packet_flags = 0;
      UINT64 device_position = 0;
      UINT64 timestamp = 0;
      hr = capture->GetBuffer(&buffer, &frames, &packet_flags, &device_position, &timestamp);
      if (FAILED(hr)) {
        status.set_error("reading a process audio packet failed (" + hresult_text(hr) + ")");
        broken = true;
        break;
      }

      if (packet_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        const size_t samples = static_cast<size_t>(frames) * channels;
        if (silence.size() < samples) silence.resize(samples);
        std::fill(silence.begin(), silence.begin() + static_cast<std::ptrdiff_t>(samples), 0.0f);
        buffer = reinterpret_cast<BYTE*>(silence.data());
      }

      obs_source_audio audio{};
      audio.data[0] = buffer;
      audio.frames = frames;
      audio.speakers = speakers;
      audio.samples_per_sec = rate;
      audio.format = AUDIO_FORMAT_FLOAT;
      // WASAPI reports 100-nanosecond units. A discontinuity is a gap the timestamp already names;
      // libobs places the packet by it, so no packet is dropped or repeated here.
      audio.timestamp = timestamp * 100;
      obs_source_output_audio(context.data->source, &audio);

      capture->ReleaseBuffer(frames);
      if (FAILED(capture->GetNextPacketSize(&next))) {
        status.set_error("reading process audio failed");
        broken = true;
        break;
      }
    }
    if (broken) {
      result = CaptureResult::transient;
      break;
    }
  }

  status.mark_stopped();
  client->Stop();
  capture->Release();
  client->Release();
  CloseHandle(capture_event);
  return result;
}

void capture_main(SourceData* data, uint32_t pid, HANDLE stop, HANDLE process) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(com)) {
    data->status->mark_failed("COM could not be started for process audio (" + hresult_text(com) + ")");
    CloseHandle(process);
    return;
  }

  DWORD code = 0;
  if (!GetExitCodeProcess(process, &code) || code != STILL_ACTIVE) {
    data->status->mark_failed("the process has exited");
    CloseHandle(process);
    CoUninitialize();
    return;
  }

  CaptureContext context{data, process, pid};
  int attempt = 0;
  for (;;) {
    bool ran = false;
    const CaptureResult result = capture_once(context, stop, ran);
    // A capture that ran and then failed is a fresh failure: three unplugs over hours must not add up to "failed".
    if (ran) attempt = 0;
    if (result == CaptureResult::stopped || result == CaptureResult::process_exited ||
        result == CaptureResult::permanent) {
      if (result == CaptureResult::process_exited) data->status->mark_failed("the process exited");
      if (result == CaptureResult::permanent) {
        data->status->mark_failed(data->status->get_error().empty() ? "process audio is not supported"
                                                                   : data->status->get_error());
      }
      break;
    }
    if (++attempt >= kTransientAttempts) {
      data->status->mark_failed(data->status->get_error().empty() ? "process audio kept failing"
                                                                 : data->status->get_error());
      break;
    }
    const HANDLE waits[] = {stop, process};
    const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 400u * static_cast<DWORD>(attempt));
    if (waited == WAIT_OBJECT_0) break;
    if (waited == WAIT_OBJECT_0 + 1) {
      data->status->mark_failed("the process exited");
      break;
    }
  }

  CloseHandle(process);
  CoUninitialize();
}

void start_worker(SourceData* data) {
  if (data->pid == 0 || !data->stop) return;
  ResetEvent(data->stop);
  data->status->pid.store(data->pid);
  const uint32_t pid = data->pid;
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    data->status->mark_failed("the process could not be opened (" +
                              hresult_text(HRESULT_FROM_WIN32(GetLastError())) + ")");
    return;
  }
  const HANDLE stop = data->stop;
  try {
    data->worker = std::thread([data, pid, stop, process] { capture_main(data, pid, stop, process); });
  } catch (...) {
    CloseHandle(process);
    data->status->mark_failed("the process audio thread could not be started");
  }
}

void stop_worker(SourceData* data) {
  if (data->worker.joinable()) {
    if (data->stop) SetEvent(data->stop);
    data->worker.join();
  }
  data->status->mark_stopped();
}

uint32_t read_pid(obs_data_t* settings) {
  const int64_t pid = settings ? obs_data_get_int(settings, kProcessAudioPidSetting) : 0;
  if (pid <= 0 || pid > 0xFFFFFFFFll) return 0;
  return static_cast<uint32_t>(pid);
}

const char* process_audio_name(void*) { return "Application audio"; }

void process_audio_defaults(obs_data_t* settings) { obs_data_set_default_int(settings, kProcessAudioPidSetting, 0); }

void* process_audio_create(obs_data_t* settings, obs_source_t* source) {
  SourceData* data = new SourceData;
  data->source = source;
  data->status = std::make_shared<Status>();
  data->pid = read_pid(settings);
  data->status->pid.store(data->pid);
  data->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!data->stop) {
    data->status->mark_failed("the process audio stop event could not be created");
    delete data;
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_registry[source] = data->status;
  }
  start_worker(data);
  return data;
}

void process_audio_destroy(void* object) {
  SourceData* data = static_cast<SourceData*>(object);
  if (!data) return;
  stop_worker(data);
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_registry.erase(data->source);
  }
  if (data->stop) CloseHandle(data->stop);
  delete data;
}

// A new pid replaces the capture only after the old worker is completely gone: no source is recreated,
// and volume, mute and other libobs controls stay untouched.
void process_audio_update(void* object, obs_data_t* settings) {
  SourceData* data = static_cast<SourceData*>(object);
  if (!data) return;
  const uint32_t pid = read_pid(settings);
  std::lock_guard<std::mutex> lock(data->retarget);
  if (pid == data->pid) return;
  stop_worker(data);
  data->pid = pid;
  data->status->pid.store(pid);
  data->status->failed.store(false);
  data->status->set_error("");
  start_worker(data);
}

obs_source_info process_audio_info() {
  obs_source_info info = {};
  info.id = kProcessAudioSourceId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR;
  info.get_name = process_audio_name;
  info.create = process_audio_create;
  info.destroy = process_audio_destroy;
  info.update = process_audio_update;
  info.get_defaults = process_audio_defaults;
  info.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT;
  return info;
}

}  // namespace

void register_process_audio_source() {
  // Registration lives in the obs startup's own tables, which obs_shutdown frees: a second obs_startup
  // in this process (a probe that starts and stops libobs twice) needs the source registered again. Ask
  // the tables first, so calling this twice in one lifetime is harmless.
  for (size_t index = 0;; ++index) {
    const char* id = nullptr;
    if (!obs_enum_source_types(index, &id) || !id) break;
    if (std::strcmp(id, kProcessAudioSourceId) == 0) return;
  }
  const obs_source_info info = process_audio_info();
  obs_register_source(&info);
}

ProcessAudioState process_audio_state(obs_source* source) {
  ProcessAudioState out;
  std::shared_ptr<Status> status;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const auto found = g_registry.find(source);
    if (found == g_registry.end()) return out;
    status = found->second;
  }
  out.pid = status->pid.load();
  out.ready = status->ready.load();
  out.failed = status->failed.load();
  out.error = status->get_error();
  return out;
}

}  // namespace bx
