#include "web_clip.h"

#include <WebView2.h>

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "../../shared/files.h"
#include "../../shared/json.h"
#include "../../shared/log.h"
#include "../platform.h"

#ifdef __MINGW32__
// The interfaces and handlers this file names, for MinGW's __uuidof (the SDK's own headers do not carry these GUIDs there).
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, 0x4e8a3389, 0xc9d8, 0x4bd2, 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d)
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, 0x6c4819f3, 0xc9b7, 0x4260, 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c)
__CRT_UUID_DECL(ICoreWebView2WebMessageReceivedEventHandler, 0x57213f19, 0x00e6, 0x49fa, 0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2)
__CRT_UUID_DECL(ICoreWebView2ProcessFailedEventHandler, 0x79e0aea4, 0x990b, 0x42d9, 0xaa, 0x1d, 0x0f, 0xcc, 0x2e, 0x5b, 0xc7, 0xf1)
__CRT_UUID_DECL(ICoreWebView2TrySuspendCompletedHandler, 0x00f206a7, 0x9d17, 0x4605, 0x91, 0xf6, 0x4e, 0x8e, 0x4d, 0xe1, 0x92, 0xe3)
__CRT_UUID_DECL(ICoreWebView2Controller2, 0xc979903e, 0xd4ca, 0x4228, 0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab)
__CRT_UUID_DECL(ICoreWebView2_3, 0xa0d6df20, 0x3b92, 0x416d, 0xaa, 0x0c, 0x43, 0x7a, 0x9c, 0x72, 0x78, 0x57)
__CRT_UUID_DECL(ICoreWebView2_19, 0x6921f954, 0x79b0, 0x437f, 0xa9, 0x97, 0xc8, 0x58, 0x11, 0x89, 0x7c, 0x68)
#endif

namespace bx::ui {
namespace {

// The page is the app's own files, served from one host; the library folders are mapped to three more, so the video
// element reads the clip straight from disk and the browser serves its byte ranges (a request made by the media element
// is not seen by a handler the app registers, so the mapping is what the page reads through).
constexpr wchar_t kPageHost[] = L"app.buffrx.local";
constexpr wchar_t kPageUrl[] = L"https://app.buffrx.local/index.html?view=panel";
constexpr wchar_t kClipsHost[] = L"clips.buffrx.local";
constexpr wchar_t kShotsHost[] = L"shots.buffrx.local";
constexpr wchar_t kRecordingsHost[] = L"recordings.buffrx.local";

// Holds one COM reference. A callback argument is owned by its caller: anything kept past the call takes its own
// reference first (copy), or the object is released as soon as the callback returns, and the browser goes with it.
template <typename T>
class ComPtr {
public:
  ComPtr() = default;
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  T* get() const { return pointer_; }
  T* operator->() const { return pointer_; }
  explicit operator bool() const { return pointer_ != nullptr; }
  // Takes over a reference the caller already owns (a result of a Create, Get or QueryInterface call).
  void attach(T* value) {
    reset();
    pointer_ = value;
  }
  // Shares a reference the caller keeps (a callback argument).
  void copy(T* value) {
    reset();
    if (value) {
      value->AddRef();
      pointer_ = value;
    }
  }
  void reset() {
    if (pointer_) {
      T* old = pointer_;
      pointer_ = nullptr;
      old->Release();
    }
  }

private:
  T* pointer_ = nullptr;
};

// A COM object WebView2 calls back: reference counted, with the interface it answers to.
template <typename Interface>
class ComHandler : public Interface {
public:
  virtual ~ComHandler() = default;
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --references_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    if (id == IID_IUnknown || id == __uuidof(Interface)) {
      *out = static_cast<Interface*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

private:
  std::atomic<ULONG> references_{1};
};

class EnvironmentDone final : public ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
  explicit EnvironmentDone(std::function<void(HRESULT, ICoreWebView2Environment*)> run) : run_(std::move(run)) {}
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* environment) override {
    run_(result, environment);
    return S_OK;
  }

private:
  std::function<void(HRESULT, ICoreWebView2Environment*)> run_;
};

class ControllerDone final : public ComHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
  explicit ControllerDone(std::function<void(HRESULT, ICoreWebView2Controller*)> run) : run_(std::move(run)) {}
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
    run_(result, controller);
    return S_OK;
  }

private:
  std::function<void(HRESULT, ICoreWebView2Controller*)> run_;
};

class MessageIn final : public ComHandler<ICoreWebView2WebMessageReceivedEventHandler> {
public:
  explicit MessageIn(std::function<void(ICoreWebView2WebMessageReceivedEventArgs*)> run) : run_(std::move(run)) {}
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
    (void)sender;
    run_(args);
    return S_OK;
  }

private:
  std::function<void(ICoreWebView2WebMessageReceivedEventArgs*)> run_;
};

class ProcessFailedIn final : public ComHandler<ICoreWebView2ProcessFailedEventHandler> {
public:
  explicit ProcessFailedIn(std::function<void(ICoreWebView2ProcessFailedEventArgs*)> run) : run_(std::move(run)) {}
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) override {
    (void)sender;
    run_(args);
    return S_OK;
  }

private:
  std::function<void(ICoreWebView2ProcessFailedEventArgs*)> run_;
};

// A suspend asked for: the picture is hidden either way, so the answer changes nothing here.
class SuspendDone final : public ComHandler<ICoreWebView2TrySuspendCompletedHandler> {
public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, BOOL suspended) override {
    (void)result;
    (void)suspended;
    return S_OK;
  }
};

using CreateEnvironmentFn = HRESULT (*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

// The runtime's loader beside the engine (build.ps1 and the package stage it there). Loaded on first use, so a PC
// without WebView2 starts the panel as before and only the clip picture says why it cannot play.
CreateEnvironmentFn environment_creator() {
  static CreateEnvironmentFn create = [] {
    HMODULE loader = LoadLibraryW(utf8_to_wide(executable_directory() + "\\WebView2Loader.dll").c_str());
    return loader ? reinterpret_cast<CreateEnvironmentFn>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")) : nullptr;
  }();
  return create;
}

bool same_rect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

// One clip's page: the browser and everything the page is wired to. Destroying it releases the browser.
struct WebClip::Session {
  Session(WebClip* owner, unsigned generation) : owner(owner), generation(generation) {}
  ~Session() {
    // A browser that has gone takes no more calls: its references are only let go.
    if (!dead && web) {
      if (message_on) web->remove_WebMessageReceived(message_token);
      if (process_on) web->remove_ProcessFailed(process_token);
    }
    if (!dead && controller) controller->Close();
    // Let the browser's interfaces go before their parent window: destroying the parent first is the
    // teardown order that once ended the engine inside a cancelled WebView2 call (RPC 1818).
    web19.reset();
    web3.reset();
    web.reset();
    controller.reset();
    if (picture_window) {
      DestroyWindow(picture_window);
      picture_window = nullptr;
    }
    environment.reset();
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // The session on the clip, if it is still the one a callback was made for. A callback made for a session that was
  // released (or replaced) finds nothing here, and never touches the freed session.
  static Session* live(WebClip* owner, unsigned gen) {
    return owner->session_ && owner->session_->generation == gen ? owner->session_.get() : nullptr;
  }

  // The engine-side state, and what the page is told.
  WebClip* owner;
  unsigned generation;
  HWND parent = nullptr;
  HWND picture_window = nullptr;  // bounded sibling of the panel's composition surface
  std::string file, told;
  RECT rect{};
  std::vector<std::string> roots;  // the clips folder, the screenshots folder, then the recordings base
  std::string theme;               // the panel's theme id, which the page wears
  bool covered = false, visible = false, window_visible = false, suspended = false, ready = false, page_ran = false, failed = false, dead = false;
  bool file_failed = false;  // the page refused this file, not the browser: the next file may play

  // The browser, released in the reverse order of these declarations.
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> web;
  ComPtr<ICoreWebView2_3> web3;
  ComPtr<ICoreWebView2_19> web19;
  EventRegistrationToken message_token{}, process_token{};
  bool message_on = false, process_on = false;

  void start();
  void on_environment(HRESULT result, ICoreWebView2Environment* created);
  void on_controller(HRESULT result, ICoreWebView2Controller* created);
  void on_message(ICoreWebView2WebMessageReceivedEventArgs* args);
  void on_process_failed(ICoreWebView2ProcessFailedEventArgs* args);
  void map_roots();
  void post(const std::string& text);
  void tell_file();
  void apply();
  void fail(const std::string& why);
};

void WebClip::Session::start() {
  const CreateEnvironmentFn create = environment_creator();
  if (!create) {
    fail("The WebView2 runtime is not installed, so the clip cannot play here.");
    return;
  }
  const std::string page = owner->web_root_ + "\\index.html";
  if (GetFileAttributesW(utf8_to_wide(page).c_str()) == INVALID_FILE_ATTRIBUTES) {
    fail("The clip picture is missing from this build.");
    return;
  }
  // WebView2 creates child windows before its controller callback. Give it the picture rectangle as
  // its parent from the start, so those children cannot briefly cover the panel's composition surface.
  picture_window = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_BLACKRECT,
                                   rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                                   parent, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!picture_window) {
    fail("The clip picture could not be placed in the panel.");
    return;
  }
  CreateDirectoryW(utf8_to_wide(owner->profile_dir_).c_str(), nullptr);
  // A clip plays when it is opened, as the preview window's did: the browser must not hold back a video's sound for want of a click.
  // The arguments travel through the variable the runtime reads when its browser starts, which is when the clip opens.
  const std::wstring flag = L"--autoplay-policy=no-user-gesture-required";
  std::wstring arguments(static_cast<size_t>(GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", nullptr, 0)), L'\0');
  if (!arguments.empty()) GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", arguments.data(), static_cast<DWORD>(arguments.size()));
  while (!arguments.empty() && arguments.back() == L'\0') arguments.pop_back();
  bool changed = false;
  if (arguments.find(L"autoplay-policy") == std::wstring::npos) {
    arguments = arguments.empty() ? flag : arguments + L" " + flag;
    changed = true;
  }
  // The clip page is inside a WebView2 child window, so `ui.capture` cannot photograph it. A probe can
  // open the DevTools port the editor already supports (nothing listens without the variable).
  wchar_t debug_port[16] = {0};
  if (GetEnvironmentVariableW(L"BUFFRX_WEBVIEW_DEBUG_PORT", debug_port, 16) > 0 && debug_port[0] != L'\0' &&
      arguments.find(L"remote-debugging-port") == std::wstring::npos) {
    arguments += L" --remote-debugging-port=";
    arguments += debug_port;
    changed = true;
  }
  if (changed) SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", arguments.c_str());
  WebClip* host = owner;
  const unsigned gen = generation;
  ComPtr<EnvironmentDone> done;
  done.attach(new EnvironmentDone([host, gen](HRESULT result, ICoreWebView2Environment* created) {
    if (Session* s = live(host, gen)) s->on_environment(result, created);
  }));
  const HRESULT hr = create(nullptr, utf8_to_wide(owner->profile_dir_).c_str(), nullptr, done.get());
  if (FAILED(hr)) fail("The WebView2 runtime did not start (" + std::to_string(static_cast<unsigned long>(hr)) + ").");
}

void WebClip::Session::on_environment(HRESULT result, ICoreWebView2Environment* created) {
  if (FAILED(result) || !created) {
    fail("The WebView2 runtime did not start (" + std::to_string(static_cast<unsigned long>(result)) + ").");
    return;
  }
  // The environment is what keeps the browser running: the session keeps its own reference for as long as the clip is open.
  environment.copy(created);
  WebClip* host = owner;
  const unsigned gen = generation;
  ComPtr<ControllerDone> done;
  done.attach(new ControllerDone([host, gen](HRESULT hr, ICoreWebView2Controller* made) {
    if (Session* s = live(host, gen)) s->on_controller(hr, made);
  }));
  const HRESULT hr = environment->CreateCoreWebView2Controller(picture_window, done.get());
  if (FAILED(hr)) fail("The picture could not be placed in the panel (" + std::to_string(static_cast<unsigned long>(hr)) + ").");
}

void WebClip::Session::on_controller(HRESULT result, ICoreWebView2Controller* made) {
  if (FAILED(result) || !made) {
    fail("The picture could not be placed in the panel (" + std::to_string(static_cast<unsigned long>(result)) + ").");
    return;
  }
  controller.copy(made);
  controller->put_IsVisible(FALSE);
  const RECT bounds{0, 0, rect.right - rect.left, rect.bottom - rect.top};
  controller->put_Bounds(bounds);
  ICoreWebView2Controller2* controller2 = nullptr;
  if (SUCCEEDED(controller->QueryInterface(__uuidof(ICoreWebView2Controller2), reinterpret_cast<void**>(&controller2))) && controller2) {
    const COREWEBVIEW2_COLOR background{255, 0, 0, 0};
    controller2->put_DefaultBackgroundColor(background);
    controller2->Release();
  }
  ICoreWebView2* page = nullptr;
  if (FAILED(controller->get_CoreWebView2(&page)) || !page) {
    fail("The page could not be opened in the panel.");
    return;
  }
  web.attach(page);
  if (ICoreWebView2Settings* settings = nullptr; SUCCEEDED(web->get_Settings(&settings)) && settings) {
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->Release();
  }
  ICoreWebView2_3* v3 = nullptr;
  if (SUCCEEDED(web->QueryInterface(__uuidof(ICoreWebView2_3), reinterpret_cast<void**>(&v3))) && v3) web3.attach(v3);
  if (!web3) {
    // Without the host mapping the page cannot be reached: say so instead of navigating to an error page
    // and leaving the panel on a spinner for ever.
    fail("This WebView2 runtime cannot show the clip picture.");
    return;
  }
  ICoreWebView2_19* v19 = nullptr;
  if (SUCCEEDED(web->QueryInterface(__uuidof(ICoreWebView2_19), reinterpret_cast<void**>(&v19))) && v19) web19.attach(v19);
  // One page with one clip: the browser keeps the small memory target for the whole time the clip is open.
  if (web19) web19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
  if (web3) {
    web3->SetVirtualHostNameToFolderMapping(kPageHost, utf8_to_wide(owner->web_root_).c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    // Each folder gets its own host, and get_launch names only the ones mapped here.
    map_roots();
  }
  WebClip* host = owner;
  const unsigned gen = generation;
  ComPtr<MessageIn> message;
  message.attach(new MessageIn([host, gen](ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (Session* s = live(host, gen)) s->on_message(args);
  }));
  if (SUCCEEDED(web->add_WebMessageReceived(message.get(), &message_token))) message_on = true;
  // A browser that fails (its page's process exits) is told to the engine: the picture says so, and the browser is let go.
  ComPtr<ProcessFailedIn> crashed;
  crashed.attach(new ProcessFailedIn([host, gen](ICoreWebView2ProcessFailedEventArgs* args) {
    if (Session* s = live(host, gen)) s->on_process_failed(args);
  }));
  if (SUCCEEDED(web->add_ProcessFailed(crashed.get(), &process_token))) process_on = true;
  web->Navigate(kPageUrl);
  apply();
}

void WebClip::Session::on_message(ICoreWebView2WebMessageReceivedEventArgs* args) {
  LPWSTR raw = nullptr;
  if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return;
  const std::string text = wide_to_utf8(raw);
  CoTaskMemFree(raw);
  Json message;
  std::string error;
  if (!Json::parse(text, message, error)) return;
  const std::string command = message["cmd"].as_string();
  const std::string id = message["id"].as_string();
  // Each command is answered the way the engine answers its own: `{id, ok, data}` back to the page that asked.
  auto reply = [&](bool ok, const Json& data, const std::string& why) {
    Json frame = Json::object();
    frame.set("id", Json(id));
    frame.set("ok", Json(ok));
    if (ok) {
      frame.set("data", data);
    } else {
      frame.set("error", Json(why));
    }
    post(frame.dump());
  };
  if (command == "get_launch") {
    page_ran = true;
    Json launch = Json::object();
    launch.set("view", Json("panel"));
    launch.set("theme", Json(theme));
    Json files = Json::array();
    files.push(Json(file));
    launch.set("files", files);
    // The folders the page reads from, each under the host the page knows them by.
    Json media = Json::array();
    const char* hosts[] = {"clips.buffrx.local", "shots.buffrx.local", "recordings.buffrx.local"};
    for (size_t i = 0; i < roots.size() && i < 3; ++i) {
      Json entry = Json::object();
      entry.set("host", Json(hosts[i]));
      entry.set("folder", Json(roots[i]));
      media.push(entry);
    }
    launch.set("media", media);
    told = file;
    reply(true, launch, "");
    apply();
  } else if (command == "clip_ready") {
    ready = true;
    apply();
    reply(true, Json::object(), "");
    if (owner->change_) owner->change_();
  } else if (command == "clip_failed") {
    failed = true;
    file_failed = true;
    apply();
    reply(true, Json::object(), "");
    if (owner->change_) owner->change_();
  } else if (command == "panel_close") {
    reply(true, Json::object(), "");
    if (owner->close_) owner->close_();
  } else {
    reply(false, Json(), "the panel does not play " + command);
  }
}

void WebClip::Session::on_process_failed(ICoreWebView2ProcessFailedEventArgs* args) {
  COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
  args->get_ProcessFailedKind(&kind);
  if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
    Log::instance().write(LogLevel::Warn, "clip picture: the page stopped answering");
    return;
  }
  // A browser that has gone takes no more calls: the picture says so, and closing the clip lets its references go.
  dead = true;
  fail("The clip's browser closed. Close the clip and open it again.");
}

void WebClip::Session::map_roots() {
  if (!web3) return;
  const wchar_t* hosts[] = {kClipsHost, kShotsHost, kRecordingsHost};
  for (size_t i = 0; i < roots.size() && i < 3; ++i) {
    web3->SetVirtualHostNameToFolderMapping(hosts[i], utf8_to_wide(roots[i]).c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
  }
}

void WebClip::Session::post(const std::string& text) {
  if (web && !dead) web->PostWebMessageAsString(utf8_to_wide(text).c_str());
}

void WebClip::Session::tell_file() {
  if (!web || dead || !page_ran || file == told) return;
  told = file;
  Json event = Json::object();
  event.set("shell", Json("preview-files"));
  Json files = Json::array();
  files.push(Json(file));
  event.set("data", files);
  post(event.dump());
}

void WebClip::Session::apply() {
  // The page is shown from the start: a hidden page does not load its media, so the clip could never say it is ready.
  // A picture that failed is hidden, so the panel's own message is what shows.
  const bool shown = !covered && !failed && !dead;
  if (picture_window && shown != window_visible) {
    ShowWindow(picture_window, shown ? SW_SHOWNOACTIVATE : SW_HIDE);
    window_visible = shown;
  }
  if (!controller || dead) return;
  if (shown != visible) {
    controller->put_IsVisible(shown ? TRUE : FALSE);
    visible = shown;
  }
  if (failed) return;
  // A covered clip that is playing is suspended; it resumes when it is shown again.
  if (!shown && ready && !suspended && web3) {
    ComPtr<SuspendDone> done;
    done.attach(new SuspendDone());
    web3->TrySuspend(done.get());
    suspended = true;
  } else if (shown && suspended && web3) {
    web3->Resume();
    suspended = false;
  }
  if (ready) tell_file();
}

void WebClip::Session::fail(const std::string& why) {
  failed = true;
  apply();
  Log::instance().write(LogLevel::Warn, "clip picture: %s", why.c_str());
  if (owner->change_) owner->change_();
}

WebClip::WebClip() = default;

WebClip::~WebClip() { release(); }

void WebClip::configure(std::string web_root, std::string profile_dir) {
  web_root_ = std::move(web_root);
  profile_dir_ = std::move(profile_dir);
}

ClipPicture WebClip::show(HWND parent, const std::string& file, const RECT& rect, bool covered, const std::vector<std::string>& roots,
                          const std::string& theme) {
  if (!session_) {
    session_ = std::make_unique<Session>(this, ++generations_);
    session_->parent = parent;
    session_->file = file;
    session_->rect = rect;
    session_->roots = roots;
    session_->theme = theme;
    session_->covered = covered;
    session_->start();
  }
  Session& s = *session_;
  // A picture that failed takes no more calls until the clip closes, unless the page refused the file and another
  // one is asked for: one bad clip must not block the next. A browser that died stays failed, as no file could play.
  if (s.failed) {
    if (s.dead || !s.file_failed || s.file == file) return ClipPicture::failed;
    s.failed = false;
    s.file_failed = false;
  }
  s.covered = covered;
  // A library folder can move while a clip is open (Settings). The page's media hosts follow it, and
  // the page is read again so its own root list names the folders it is allowed to build URLs from.
  if (s.roots != roots) {
    s.roots = roots;
    if (s.web3) {
      s.map_roots();
      if (s.page_ran && s.web && !s.dead) {
        s.failed = false;
        s.file_failed = false;
        s.page_ran = false;
        s.ready = false;
        s.told.clear();
        s.file = file;
        s.web->Navigate(kPageUrl);
      }
    }
  }
  s.theme = theme;
  if (s.file != file) {
    s.file = file;
    s.ready = false;
    s.tell_file();
  }
  if (!same_rect(s.rect, rect)) {
    s.rect = rect;
    if (s.picture_window) {
      SetWindowPos(s.picture_window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (s.controller && !s.dead) {
      const RECT bounds{0, 0, rect.right - rect.left, rect.bottom - rect.top};
      s.controller->put_Bounds(bounds);
      s.controller->NotifyParentWindowPositionChanged();
    }
  }
  s.apply();
  if (s.failed) return ClipPicture::failed;
  return s.ready ? ClipPicture::playing : ClipPicture::loading;
}

void WebClip::release() {
  // The session's destructor closes the page and lets go of the browser, which then exits.
  session_.reset();
}

}  // namespace bx::ui
