// Linux platform services for the engine: X11 global hotkeys, the clipboard, notifications, the
// editor launcher, the XDG paths and the small OS questions the engine asks (memory, power, elevation).
#include "../engine/platform_linux.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

#include <gtk/gtk.h>
#include <clocale>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include "../shared/log.h"
#include "webview.h"

namespace bx {
namespace {

// ---------------------------------------------------------------- small helpers

std::string env(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

std::string home_dir() {
  const std::string home = env("HOME");
  return home.empty() ? "/tmp" : home;
}

std::string data_home() {
  const std::string xdg = env("XDG_DATA_HOME");
  if (!xdg.empty()) return xdg;
  return home_dir() + "/.local/share";
}

std::string config_home() {
  const std::string xdg = env("XDG_CONFIG_HOME");
  if (!xdg.empty()) return xdg;
  return home_dir() + "/.config";
}

bool make_dirs(const std::string& path) {
  std::string partial;
  if (!path.empty() && path[0] == '/') partial = "/";
  for (size_t i = 0; i < path.size(); ++i) {
    partial += path[i];
    if (path[i] == '/' || i + 1 == path.size()) {
      if (partial.size() > 1) mkdir(partial.c_str(), 0755);
    }
  }
  return access(path.c_str(), F_OK) == 0;
}

std::string shell_quote(const std::string& text) {
  std::string out = "'";
  for (char c : text) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

pid_t spawn(const std::string& program, const std::vector<std::string>& arguments, bool capture = false,
            int* out_fd = nullptr) {
  int pipe_fds[2] = {-1, -1};
  if (capture && pipe(pipe_fds) != 0) return -1;
  const pid_t pid = fork();
  if (pid < 0) {
    if (capture) {
      close(pipe_fds[0]);
      close(pipe_fds[1]);
    }
    return -1;
  }
  if (pid == 0) {
    if (capture) {
      dup2(pipe_fds[1], STDOUT_FILENO);
      close(pipe_fds[0]);
      close(pipe_fds[1]);
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(program.c_str(), argv.data());
    _exit(127);
  }
  if (capture) {
    close(pipe_fds[1]);
    if (out_fd) *out_fd = pipe_fds[0];
    else close(pipe_fds[0]);
  }
  return pid;
}

std::string run_capture(const std::string& program, const std::vector<std::string>& arguments) {
  int fd = -1;
  const pid_t pid = spawn(program, arguments, true, &fd);
  if (pid < 0 || fd < 0) return "";
  std::string out;
  char buffer[512];
  ssize_t n;
  while ((n = read(fd, buffer, sizeof(buffer))) > 0) out.append(buffer, static_cast<size_t>(n));
  close(fd);
  int status = 0;
  waitpid(pid, &status, 0);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

std::string native_os_path(std::string path) {
  for (char& c : path)
    if (c == '\\') c = '/';
  return path;
}

std::string parent_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// ---------------------------------------------------------------- single instance

int g_instance_fd = -1;
std::string instance_path() {
  const std::string runtime = env("XDG_RUNTIME_DIR");
  const std::string base = runtime.empty() ? "/tmp" : runtime;
  return base + "/buffrx-" + std::to_string(static_cast<unsigned long>(getuid())) + ".lock";
}

// ---------------------------------------------------------------- key names

unsigned virtual_key_from_name(const std::string& name, bool& ok) {
  ok = true;
  if (name.size() == 1) {
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<unsigned>(c);
    ok = false;
    return 0;
  }
  if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f')) {
    const int number = std::atoi(name.c_str() + 1);
    if (number >= 1 && number <= 24) return static_cast<unsigned>(VK_F1 + number - 1);
  }
  struct Entry {
    const char* name;
    unsigned key;
  };
  static const Entry entries[] = {
      {"SPACE", VK_SPACE},       {"TAB", VK_TAB},         {"ENTER", VK_RETURN},    {"ESC", VK_ESCAPE},
      {"ESCAPE", VK_ESCAPE},     {"BACKSPACE", VK_BACK},  {"INSERT", VK_INSERT},   {"DELETE", VK_DELETE},
      {"HOME", VK_HOME},         {"END", VK_END},         {"PAGEUP", VK_PRIOR},    {"PAGEDOWN", VK_NEXT},
      {"UP", VK_UP},             {"DOWN", VK_DOWN},       {"LEFT", VK_LEFT},       {"RIGHT", VK_RIGHT},
      {"PRINTSCREEN", VK_SNAPSHOT}, {"SCROLLLOCK", VK_SCROLL}, {"PAUSE", VK_PAUSE},
      {"NUM0", VK_NUMPAD0}, {"NUM1", VK_NUMPAD1}, {"NUM2", VK_NUMPAD2}, {"NUM3", VK_NUMPAD3}, {"NUM4", VK_NUMPAD4},
      {"NUM5", VK_NUMPAD5}, {"NUM6", VK_NUMPAD6}, {"NUM7", VK_NUMPAD7}, {"NUM8", VK_NUMPAD8}, {"NUM9", VK_NUMPAD9},
  };
  std::string upper;
  for (char c : name) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  for (const auto& entry : entries) {
    if (upper == entry.name) return entry.key;
  }
  ok = false;
  return 0;
}

std::string key_name_from_virtual(unsigned key) {
  if (key >= 'A' && key <= 'Z') return std::string(1, static_cast<char>(key));
  if (key >= '0' && key <= '9') return std::string(1, static_cast<char>(key));
  if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
  if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return "Num" + std::to_string(key - VK_NUMPAD0);
  switch (key) {
    case VK_SNAPSHOT: return "PrintScreen";
    case VK_SCROLL: return "ScrollLock";
    case VK_PAUSE: return "Pause";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Esc";
    case VK_BACK: return "Backspace";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    default: return "?";
  }
}

KeySym keysym_from_virtual(unsigned key) {
  if (key >= 'A' && key <= 'Z') return static_cast<KeySym>(key);
  if (key >= '0' && key <= '9') return static_cast<KeySym>(key);
  if (key >= VK_F1 && key <= VK_F24) return XK_F1 + (key - VK_F1);
  if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return XK_KP_0 + (key - VK_NUMPAD0);
  switch (key) {
    case VK_SPACE: return XK_space;
    case VK_TAB: return XK_Tab;
    case VK_RETURN: return XK_Return;
    case VK_ESCAPE: return XK_Escape;
    case VK_BACK: return XK_BackSpace;
    case VK_INSERT: return XK_Insert;
    case VK_DELETE: return XK_Delete;
    case VK_HOME: return XK_Home;
    case VK_END: return XK_End;
    case VK_PRIOR: return XK_Prior;
    case VK_NEXT: return XK_Next;
    case VK_UP: return XK_Up;
    case VK_DOWN: return XK_Down;
    case VK_LEFT: return XK_Left;
    case VK_RIGHT: return XK_Right;
    case VK_SNAPSHOT: return XK_Print;
    case VK_SCROLL: return XK_Scroll_Lock;
    case VK_PAUSE: return XK_Pause;
    default: return NoSymbol;
  }
}

unsigned x_modifiers(unsigned modifiers) {
  unsigned mask = 0;
  if (modifiers & MOD_CONTROL) mask |= ControlMask;
  if (modifiers & MOD_ALT) mask |= Mod1Mask;
  if (modifiers & MOD_SHIFT) mask |= ShiftMask;
  if (modifiers & MOD_WIN) mask |= Mod4Mask;
  return mask;
}

int g_grab_error = 0;
int grab_error_handler(Display*, XErrorEvent* event) {
  if (event->error_code == BadAccess) g_grab_error = 1;
  return 0;
}

// ---------------------------------------------------------------- clipboard

struct ClipboardOwner {
  std::mutex mutex;
  std::string text;
  bool files = false;
  bool started = false;
  Display* display = nullptr;
  Window window = 0;
  Atom clipboard = 0;
  Atom targets = 0;
  Atom utf8 = 0;
  Atom text_atom = 0;
  Atom uri_list = 0;
  Atom property = 0;

  void start() {
    if (started) return;
    started = true;
    std::thread([this] {
      Display* dpy = XOpenDisplay(nullptr);
      if (!dpy) return;
      display = dpy;
      clipboard = XInternAtom(dpy, "CLIPBOARD", False);
      targets = XInternAtom(dpy, "TARGETS", False);
      utf8 = XInternAtom(dpy, "UTF8_STRING", False);
      text_atom = XInternAtom(dpy, "TEXT", False);
      uri_list = XInternAtom(dpy, "text/uri-list", False);
      property = XInternAtom(dpy, "BUFFRX_SELECTION", False);
      window = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, 0);
      XSelectInput(dpy, window, PropertyChangeMask | SelectionRequest | SelectionClear);
      XSetSelectionOwner(dpy, clipboard, window, CurrentTime);
      for (;;) {
        XEvent event;
        XNextEvent(dpy, &event);
        if (event.type == SelectionRequest) {
          const XSelectionRequestEvent& request = event.xselectionrequest;
          XSelectionEvent answer{};
          answer.type = SelectionNotify;
          answer.display = request.display;
          answer.requestor = request.requestor;
          answer.selection = request.selection;
          answer.target = request.target;
          answer.property = None;
          answer.time = request.time;
          std::lock_guard<std::mutex> lock(mutex);
          const bool target_ok = request.target == targets || request.target == utf8 ||
                                 request.target == XA_STRING || request.target == text_atom ||
                                 request.target == uri_list;
          if (target_ok) {
            std::string payload;
            if (request.target == targets) {
              const Atom list[] = {targets, utf8, XA_STRING, text_atom, uri_list};
              XChangeProperty(dpy, request.requestor, request.property, XA_ATOM, 32, PropModeReplace,
                              reinterpret_cast<const unsigned char*>(list), 5);
            } else {
              payload = text;
              XChangeProperty(dpy, request.requestor, request.property, request.target, 8, PropModeReplace,
                              reinterpret_cast<const unsigned char*>(payload.data()), static_cast<int>(payload.size()));
            }
            answer.property = request.property;
          }
          XSendEvent(dpy, request.requestor, False, 0, reinterpret_cast<XEvent*>(&answer));
          XFlush(dpy);
        } else if (event.type == SelectionClear) {
          // Another application took the selection: ours is no longer needed.
        }
      }
    }).detach();
  }

  bool set(const std::string& value, bool is_files) {
    start();
    {
      std::lock_guard<std::mutex> lock(mutex);
      text = value;
      files = is_files;
    }
    if (display && window) {
      XLockDisplay(display);
      XSetSelectionOwner(display, clipboard, window, CurrentTime);
      XFlush(display);
      XUnlockDisplay(display);
    }
    return display != nullptr;
  }
};

ClipboardOwner& clipboard() {
  static ClipboardOwner owner;
  return owner;
}

std::string g_last_copied;

// ---------------------------------------------------------------- secrets

std::string base64_encode(const std::vector<unsigned char>& data) {
  std::string out(static_cast<size_t>((data.size() + 2) / 3 * 4) + 1, '\0');
  const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data.data(),
                                      static_cast<int>(data.size()));
  out.resize(static_cast<size_t>(written));
  return out;
}

std::vector<unsigned char> base64_decode(const std::string& text) {
  std::vector<unsigned char> out(text.size() * 3 / 4 + 4);
  const int written = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(text.data()),
                                      static_cast<int>(text.size()));
  if (written < 0) return {};
  out.resize(static_cast<size_t>(written));
  while (!out.empty() && out.back() == 0) out.pop_back();
  return out;
}

bool secret_key(std::vector<unsigned char>& key) {
  const std::string directory = data_home() + "/buffrx";
  make_dirs(directory);
  const std::string path = directory + "/secret.key";
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    std::vector<unsigned char> generated(32);
    if (RAND_bytes(generated.data(), static_cast<int>(generated.size())) != 1) return false;
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    const ssize_t written = write(fd, generated.data(), generated.size());
    close(fd);
    if (written != static_cast<ssize_t>(generated.size())) return false;
    key = std::move(generated);
    return true;
  }
  key.resize(32);
  const ssize_t got = read(fd, key.data(), key.size());
  close(fd);
  return got == static_cast<ssize_t>(key.size());
}

// ---------------------------------------------------------------- launch helpers

// The file a desktop entry should start: the AppImage itself when the engine runs from one, so the
// entry works after the mount is gone (the mounted path changes every launch).
std::string install_launcher_path(const std::string& fallback) {
  const std::string appimage = env("APPIMAGE");
  return appimage.empty() ? fallback : appimage;
}

std::string ui_binary() {
  const std::string beside = executable_directory() + "/buffrx-ui";
  if (access(beside.c_str(), X_OK) == 0) return beside;
  const std::string appdir = env("APPDIR");
  if (!appdir.empty()) {
    const std::string in_app = appdir + "/usr/bin/buffrx-ui";
    if (access(in_app.c_str(), X_OK) == 0) return in_app;
  }
  return "";
}

}  // namespace

// ---------------------------------------------------------------- parse / format

bool parse_chord(const std::string& chord, unsigned& modifiers, unsigned& virtual_key, std::string& problem) {
  modifiers = 0;
  virtual_key = 0;
  problem.clear();
  std::vector<std::string> tokens;
  std::string current;
  for (char c : chord) {
    if (c == '+' || c == '-') {
      tokens.push_back(current);
      current.clear();
    } else if (c != ' ') {
      current.push_back(c);
    }
  }
  if (!current.empty()) tokens.push_back(current);
  if (tokens.size() < 2) {
    problem = "a chord needs a modifier and a key";
    return false;
  }
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    std::string token;
    for (char c : tokens[i]) token.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (token == "CTRL" || token == "CONTROL") modifiers |= MOD_CONTROL;
    else if (token == "ALT") modifiers |= MOD_ALT;
    else if (token == "SHIFT") modifiers |= MOD_SHIFT;
    else if (token == "WIN" || token == "SUPER") modifiers |= MOD_WIN;
    else {
      problem = "unknown modifier: " + tokens[i];
      return false;
    }
  }
  bool ok = false;
  virtual_key = virtual_key_from_name(tokens.back(), ok);
  if (!ok) {
    problem = "unknown key: " + tokens.back();
    return false;
  }
  if (modifiers == 0 && (virtual_key < VK_F13 || virtual_key > VK_F24)) {
    problem = "a lone key would fire while typing";
    return false;
  }
  modifiers |= MOD_NOREPEAT;
  return true;
}

std::string format_chord(unsigned modifiers, unsigned virtual_key) {
  std::string out;
  if (modifiers & MOD_CONTROL) out += "Ctrl+";
  if (modifiers & MOD_ALT) out += "Alt+";
  if (modifiers & MOD_SHIFT) out += "Shift+";
  if (modifiers & MOD_WIN) out += "Win+";
  out += key_name_from_virtual(virtual_key);
  return out;
}

// ---------------------------------------------------------------- PlatformWindow

namespace {
void tray_command(GtkMenuItem* item, gpointer data) {
  auto* owner = static_cast<PlatformWindow*>(data);
  const int command = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "buffrx-command"));
  if (owner) owner->tray_command(command);
}

void tray_popup(GtkStatusIcon*, guint button, guint activate_time, gpointer data) {
  auto* owner = static_cast<PlatformWindow*>(data);
  if (!owner) return;
  GtkWidget* menu = gtk_menu_new();
  struct Item { const char* label; int command; };
  const Item items[] = {
      {"Open BuffrX", PlatformWindow::kMenuOpenUi},
      {"Save clip", PlatformWindow::kMenuSaveClip},
      {"Quick clip", PlatformWindow::kMenuQuickClip},
      {"Screenshot", PlatformWindow::kMenuScreenshot},
      {"Start or stop recording", PlatformWindow::kMenuToggleRecording},
      {"Turn the buffer on or off", PlatformWindow::kMenuToggleBuffer},
      {"Open the clips folder", PlatformWindow::kMenuOpenClipsFolder},
      {"Quit BuffrX", PlatformWindow::kMenuQuit},
  };
  for (const Item& entry : items) {
    if (entry.command == PlatformWindow::kMenuQuit) gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget* item = gtk_menu_item_new_with_label(entry.label);
    g_object_set_data(G_OBJECT(item), "buffrx-command", GINT_TO_POINTER(entry.command));
    g_signal_connect(item, "activate", G_CALLBACK(tray_command), owner);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  }
  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), nullptr);
}

void tray_activate(GtkStatusIcon*, gpointer data) {
  auto* owner = static_cast<PlatformWindow*>(data);
  if (owner) owner->tray_command(PlatformWindow::kMenuOpenUi);
}

std::string tray_icon_path() {
  const std::string dir = executable_directory();
  const char* appdir = std::getenv("APPDIR");
  std::vector<std::string> candidates = {
      dir + "/../share/icons/hicolor/256x256/apps/buffrx.png",
      dir + "/buffrx.png",
  };
  if (appdir && *appdir) candidates.push_back(std::string(appdir) + "/buffrx.png");
  candidates.push_back(dir + "/../linux/appimage/buffrx.png");
  for (const auto& path : candidates)
    if (access(path.c_str(), R_OK) == 0) return path;
  return {};
}
}  // namespace

struct PlatformWindow::Impl {
  Display* display = nullptr;
  Window window = 0;
  std::thread thread;
  std::thread tray_thread;
  std::atomic<bool> running{false};
  std::atomic<bool> tray_running{false};
  std::atomic<bool> tray_stop{false};
  std::atomic<bool> tray_loop_running{false};
  GtkStatusIcon* status_icon = nullptr;
  GtkWidget* tray_menu = nullptr;
  std::mutex grab_mutex;
  std::vector<HotkeyBinding> grabbed;
  PlatformWindow* owner = nullptr;

  void ungrab_all() {
    std::lock_guard<std::mutex> lock(grab_mutex);
    if (!display) return;
    for (const auto& binding : grabbed) {
      const KeyCode keycode = XKeysymToKeycode(display, keysym_from_virtual(binding.virtual_key));
      if (!keycode) continue;
      const unsigned mask = x_modifiers(binding.modifiers);
      for (unsigned extra : {0u, static_cast<unsigned>(LockMask), static_cast<unsigned>(Mod2Mask),
                             static_cast<unsigned>(LockMask | Mod2Mask)}) {
        XUngrabKey(display, keycode, mask | extra, DefaultRootWindow(display));
      }
    }
    grabbed.clear();
    XFlush(display);
  }

  void grab_all(const std::vector<HotkeyBinding>& bindings) {
    if (!display) return;
    ungrab_all();
    std::lock_guard<std::mutex> lock(grab_mutex);
    XSetErrorHandler(grab_error_handler);
    for (const HotkeyBinding& binding : bindings) {
      HotkeyBinding result = binding;
      result.registered = false;
      if (!binding.valid) {
        grabbed.push_back(result);
        continue;
      }
      const KeySym symbol = keysym_from_virtual(binding.virtual_key);
      const KeyCode keycode = XKeysymToKeycode(display, symbol);
      if (!keycode) {
        result.problem = "the key is not on this keyboard";
        grabbed.push_back(result);
        continue;
      }
      const unsigned mask = x_modifiers(binding.modifiers);
      bool failed = false;
      for (unsigned extra : {0u, static_cast<unsigned>(LockMask), static_cast<unsigned>(Mod2Mask),
                             static_cast<unsigned>(LockMask | Mod2Mask)}) {
        g_grab_error = 0;
        XGrabKey(display, keycode, mask | extra, DefaultRootWindow(display), False, GrabModeAsync, GrabModeAsync);
        XSync(display, False);
        if (g_grab_error) failed = true;
      }
      result.registered = !failed;
      if (failed) result.problem = "another application holds this shortcut";
      grabbed.push_back(result);
      if (failed) {
        BX_LOG_WARN("hotkey %s (%s) could not be registered", binding.action.c_str(), binding.chord.c_str());
      }
    }
    XFlush(display);
  }

  void event_loop() {
    Display* dpy = display;
    if (!dpy) return;
    while (running) {
      while (running && XPending(dpy) > 0) {
        XEvent event;
        XNextEvent(dpy, &event);
        if (event.type != KeyPress) continue;
      const KeyCode keycode = event.xkey.keycode;
      const unsigned state = event.xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
      std::string action;
      {
        std::lock_guard<std::mutex> lock(grab_mutex);
        for (const auto& binding : grabbed) {
          if (!binding.registered) continue;
          if (XKeysymToKeycode(dpy, keysym_from_virtual(binding.virtual_key)) != keycode) continue;
          if (x_modifiers(binding.modifiers) != state) continue;
          action = binding.action;
          break;
        }
      }
        if (!action.empty() && owner && owner->on_action_) owner->on_action_(action);
      }
      // A blocking XNextEvent would hold this thread through the engine's shutdown. Poll the connection
      // instead, with a short wait, so the running flag is seen.
      if (!running) break;
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(ConnectionNumber(dpy), &read_set);
      struct timeval timeout{0, 200000};
      select(ConnectionNumber(dpy) + 1, &read_set, nullptr, nullptr, &timeout);
    }
  }
};

PlatformWindow::PlatformWindow() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }

PlatformWindow::~PlatformWindow() {
  if (!impl_) return;
  impl_->running = false;
  remove_tray_icon();
  if (impl_->thread.joinable()) impl_->thread.join();
  if (impl_->display) {
    XLockDisplay(impl_->display);
    impl_->ungrab_all();
    XCloseDisplay(impl_->display);
    impl_->display = nullptr;
  }
}

bool PlatformWindow::create(const std::string& title, ActionHandler on_action, CommandHandler on_command) {
  (void)title;
  on_action_ = std::move(on_action);
  on_command_ = std::move(on_command);
  // The X server is opened before the UI's GLFW connection so XInitThreads applies to every connection
  // this process makes.
  static const bool initialised = [] {
    XInitThreads();
    return true;
  }();
  (void)initialised;
  impl_->display = XOpenDisplay(nullptr);
  if (!impl_->display) {
    BX_LOG_WARN("no X display: global hotkeys are unavailable");
    return false;
  }
  // The hotkey grabs need the root window only: a passive grab is global there, and KeyPress is
  // selected so the grabbing client receives the match. No helper window is created. A mapped 1x1
  // window was listed by the window manager as a blank taskbar entry, and closing that entry killed
  // this X connection, which ended the engine.
  XSelectInput(impl_->display, DefaultRootWindow(impl_->display), KeyPressMask);
  impl_->running = true;
  impl_->thread = std::thread([this] { impl_->event_loop(); });
  // The engine applies the bindings before this window exists (the protocol owns the table, this
  // window owns the grabs). Take the table now that there is an X display to grab on.
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    impl_->grab_all(bindings_);
  }
  return true;
}

void PlatformWindow::apply_hotkeys(const std::vector<HotkeyBinding>& bindings) {
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    bindings_ = bindings;
  }
  if (suspended_) return;
  impl_->grab_all(bindings);
}

void PlatformWindow::retry_pending() {
  if (suspended_) return;
  std::vector<HotkeyBinding> bindings;
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    bindings = bindings_;
  }
  bool any_missing = false;
  {
    std::lock_guard<std::mutex> lock(impl_->grab_mutex);
    for (const auto& binding : impl_->grabbed) {
      if (binding.valid && !binding.registered) any_missing = true;
    }
  }
  if (any_missing) impl_->grab_all(bindings);
}

void PlatformWindow::suspend_hotkeys(bool suspended) {
  if (suspended_ == suspended) return;
  suspended_ = suspended;
  if (suspended) impl_->ungrab_all();
  else {
    std::vector<HotkeyBinding> bindings;
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      bindings = bindings_;
    }
    impl_->grab_all(bindings);
  }
}

std::vector<HotkeyBinding> PlatformWindow::hotkeys() const {
  std::lock_guard<std::mutex> lock(impl_->grab_mutex);
  return impl_->grabbed;
}

void PlatformWindow::add_tray_icon(const std::string& tooltip) {
  tray_added_ = true;
  tooltip_ = tooltip;
  if (impl_->tray_running) {
    impl_->status_icon ? gtk_status_icon_set_tooltip_text(impl_->status_icon, tooltip.c_str()) : (void)0;
    return;
  }
  impl_->tray_running = true;
  impl_->tray_stop = false;
  impl_->tray_thread = std::thread([this, tooltip] {
    int argc = 0;
    char** argv = nullptr;
    if (!gtk_init_check(&argc, &argv)) {
      BX_LOG_WARN("the GTK tray could not start (no display); use the toggle hotkey");
      impl_->tray_running = false;
      return;
    }
    // GTK calls setlocale(LC_ALL, "") during init, which would bring a comma-decimal locale back into
    // the engine's log lines and the JSON serializer. Numeric formatting stays POSIX-C.
    std::setlocale(LC_NUMERIC, "C");
    if (impl_->tray_stop) {
      impl_->tray_running = false;
      return;
    }
    const std::string icon = tray_icon_path();
    impl_->status_icon = icon.empty() ? gtk_status_icon_new()
                                      : gtk_status_icon_new_from_file(icon.c_str());
    gtk_status_icon_set_tooltip_text(impl_->status_icon, tooltip.c_str());
    gtk_status_icon_set_visible(impl_->status_icon, TRUE);
    g_signal_connect(impl_->status_icon, "activate", G_CALLBACK(tray_activate), this);
    g_signal_connect(impl_->status_icon, "popup-menu", G_CALLBACK(tray_popup), this);
    if (impl_->tray_stop) {
      gtk_status_icon_set_visible(impl_->status_icon, FALSE);
      g_object_unref(impl_->status_icon);
      impl_->status_icon = nullptr;
      impl_->tray_running = false;
      return;
    }
    impl_->tray_loop_running = true;
    gtk_main();
    impl_->tray_loop_running = false;
    if (impl_->status_icon) {
      gtk_status_icon_set_visible(impl_->status_icon, FALSE);
      g_object_unref(impl_->status_icon);
      impl_->status_icon = nullptr;
    }
    impl_->tray_running = false;
  });
}
void PlatformWindow::update_tray_tooltip(const std::string& tooltip) {
  tooltip_ = tooltip;
  if (impl_->status_icon) gtk_status_icon_set_tooltip_text(impl_->status_icon, tooltip.c_str());
}
void PlatformWindow::remove_tray_icon() {
  tray_added_ = false;
  if (!impl_->tray_running && !impl_->tray_thread.joinable()) return;
  impl_->tray_stop = true;
  if (impl_->tray_loop_running) gtk_main_quit();
  if (impl_->tray_thread.joinable()) impl_->tray_thread.join();
  impl_->tray_running = false;
}
void PlatformWindow::show_tray_menu() {
  tray_command(kMenuTrayPanel);
}
void PlatformWindow::notify(const std::string& title, const std::string& text, bool with_sound) {
  (void)with_sound;
  if (env("DISPLAY").empty() && env("WAYLAND_DISPLAY").empty()) return;
  spawn("notify-send", {"-a", "BuffrX", "-t", "5000", title, text});
}
void PlatformWindow::set_tray_state(const TrayMenuState& state) { tray_state_ = state; }
unsigned long PlatformWindow::handle() const { return static_cast<unsigned long>(impl_->window); }

// ---------------------------------------------------------------- single instance

bool acquire_single_instance() {
  const std::string path = instance_path();
  g_instance_fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
  if (g_instance_fd < 0) return true;  // no runtime directory: do not refuse to start
  if (flock(g_instance_fd, LOCK_EX | LOCK_NB) != 0) {
    close(g_instance_fd);
    g_instance_fd = -1;
    return false;
  }
  ftruncate(g_instance_fd, 0);
  const std::string pid = std::to_string(static_cast<unsigned>(getpid()));
  if (write(g_instance_fd, pid.data(), pid.size()) < 0) {
    // The lock is what matters; the written pid is only a convenience.
  }
  return true;
}

void release_single_instance() {
  if (g_instance_fd >= 0) {
    flock(g_instance_fd, LOCK_UN);
    close(g_instance_fd);
    g_instance_fd = -1;
  }
}

// ---------------------------------------------------------------- shell actions

void open_folder(const std::string& path) {
  if (path.empty()) return;
  spawn("xdg-open", {native_os_path(path)});
}
void reveal_file(const std::string& path) {
  const std::string parent = parent_of(native_os_path(path));
  if (!parent.empty()) spawn("xdg-open", {parent});
}
void open_file(const std::string& path) { spawn("xdg-open", {native_os_path(path)}); }

void fatal_notice(const std::string& text, const std::string& url) {
  BX_LOG_ERROR("%s%s%s", text.c_str(), url.empty() ? "" : "\n", url.c_str());
  if (env("DISPLAY").empty() && env("WAYLAND_DISPLAY").empty()) return;
  std::string body = text;
  if (!url.empty()) body += "\n\n" + url;
  spawn("zenity", {"--error", "--title=BuffrX", "--no-wrap", "--text=" + body});
}

bool webview2_available() {
  if (!env("BUFFRX_TEST_NO_WEBVIEW2").empty()) return false;
  return !ui_binary().empty();
}

void notice_editor_needs_webview2() {
  fatal_notice("The editor is not installed beside BuffrX (buffrx-ui).\n\n"
               "Everything else still works: buffering, clips, screenshots and the panel.");
}

bool launch_editor(const std::vector<std::string>& files, const std::string& data_root, DWORD& pid,
                   bool preview, void* parent) {
  (void)data_root;
  (void)parent;
  const std::string binary = ui_binary();
  if (binary.empty()) {
    notice_editor_needs_webview2();
    return false;
  }
  std::vector<std::string> arguments = {"--editor"};
  if (preview) arguments.push_back("--preview");
  for (const auto& file : files) arguments.push_back(file);
  const pid_t child = spawn(binary, arguments);
  if (child < 0) return false;
  pid = static_cast<unsigned long>(child);
  return true;
}

bool launch_timeline(const std::string& session, void*& process, DWORD& pid) {
  const std::string binary = ui_binary();
  if (binary.empty()) return false;
  const pid_t child = spawn(binary, {"--timeline", session});
  if (child < 0) return false;
  process = reinterpret_cast<void*>(static_cast<intptr_t>(child));
  pid = static_cast<unsigned long>(child);
  return true;
}

// ---------------------------------------------------------------- clipboard

bool copy_text_to_clipboard(const std::string& text) {
  g_last_copied = text;
  return clipboard().set(text, false);
}

std::string last_copied_text() { return g_last_copied; }

bool copy_files_to_clipboard(const std::vector<std::string>& files) {
  std::string payload;
  for (const auto& file : files) {
    const std::string native = native_os_path(file);
    std::string uri = "file://";
    for (char c : native) {
      if (c == ' ') uri += "%20";
      else uri += c;
    }
    payload += uri + "\r\n";
  }
  return clipboard().set(payload, true);
}

// ---------------------------------------------------------------- secrets

std::string protect_secret(const std::string& secret) {
  std::vector<unsigned char> key;
  if (!secret_key(key)) return "";
  std::vector<unsigned char> nonce(12);
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) return "";
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  if (!context) return "";
  std::vector<unsigned char> cipher(secret.size() + 16);
  int length = 0;
  int total = 0;
  bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) == 1 &&
            EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
            EVP_EncryptUpdate(context, cipher.data(), &length, reinterpret_cast<const unsigned char*>(secret.data()),
                              static_cast<int>(secret.size())) == 1;
  total = length;
  if (ok && EVP_EncryptFinal_ex(context, cipher.data() + total, &length) == 1) total += length;
  else ok = false;
  std::vector<unsigned char> tag(16);
  if (ok) ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
  EVP_CIPHER_CTX_free(context);
  if (!ok) return "";
  std::vector<unsigned char> blob;
  blob.insert(blob.end(), nonce.begin(), nonce.end());
  blob.insert(blob.end(), cipher.begin(), cipher.begin() + total);
  blob.insert(blob.end(), tag.begin(), tag.end());
  return "v1:" + base64_encode(blob);
}

bool unprotect_secret(const std::string& sealed, std::string& secret) {
  if (sealed.rfind("v1:", 0) != 0) return false;
  std::vector<unsigned char> blob = base64_decode(sealed.substr(3));
  if (blob.size() < 12 + 16) return false;
  std::vector<unsigned char> key;
  if (!secret_key(key)) return false;
  std::vector<unsigned char> nonce(blob.begin(), blob.begin() + 12);
  std::vector<unsigned char> cipher(blob.begin() + 12, blob.end() - 16);
  std::vector<unsigned char> tag(blob.end() - 16, blob.end());
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  if (!context) return false;
  std::vector<unsigned char> plain(cipher.size() + 16);
  int length = 0;
  int total = 0;
  bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
            EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
            EVP_DecryptUpdate(context, plain.data(), &length, cipher.data(), static_cast<int>(cipher.size())) == 1;
  total = length;
  if (ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1 &&
      EVP_DecryptFinal_ex(context, plain.data() + total, &length) == 1) {
    total += length;
  } else {
    ok = false;
  }
  EVP_CIPHER_CTX_free(context);
  if (!ok) return false;
  secret.assign(reinterpret_cast<char*>(plain.data()), static_cast<size_t>(total));
  return true;
}

// ---------------------------------------------------------------- link scheme

bool register_link_scheme(const std::string& engine_exe) {
  const std::string directory = data_home() + "/applications";
  make_dirs(directory);
  const std::string path = directory + "/buffrx.desktop";
  const std::string launcher = install_launcher_path(engine_exe);
  std::ostringstream content;
  content << "[Desktop Entry]\nType=Application\nName=BuffrX\nExec=" << launcher
          << " --open-link %u\nNoDisplay=true\nMimeType=x-scheme-handler/buffrx;\n";
  std::ifstream existing(path);
  std::string current((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
  if (current == content.str()) return true;
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << content.str();
  out.close();
  spawn("xdg-mime", {"default", "buffrx.desktop", "x-scheme-handler/buffrx"});
  return true;
}

// ---------------------------------------------------------------- pickers

std::string pick_folder(const std::string& title, const std::string& start) {
  std::vector<std::string> args = {"--file-selection", "--directory", "--title=" + title};
  if (!start.empty()) args.push_back("--filename=" + native_os_path(start) + "/");
  return run_capture("zenity", args);
}

std::string pick_executable(const std::string& title, const std::string& start) {
  std::vector<std::string> args = {"--file-selection", "--title=" + title};
  if (!start.empty()) args.push_back("--filename=" + native_os_path(start) + "/");
  return run_capture("zenity", args);
}

// ---------------------------------------------------------------- startup, power, memory

bool set_launch_on_startup(bool enabled, const std::string& exe_path) {
  const std::string directory = config_home() + "/autostart";
  make_dirs(directory);
  const std::string path = directory + "/buffrx.desktop";
  if (!enabled) {
    unlink(path.c_str());
    return true;
  }
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << "[Desktop Entry]\nType=Application\nName=BuffrX\nExec=" << install_launcher_path(exe_path)
      << "\nX-GNOME-Autostart-enabled=true\nNoDisplay=true\n";
  return out.good();
}

bool on_battery_power() {
  DIR* dir = opendir("/sys/class/power_supply");
  if (!dir) return false;
  bool battery = false;
  bool known = false;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    const std::string name = entry->d_name;
    if (name.rfind("BAT", 0) == 0) {
      std::ifstream status("/sys/class/power_supply/" + name + "/status");
      std::string line;
      if (status && std::getline(status, line)) {
        known = true;
        if (line.find("Discharging") != std::string::npos) battery = true;
      }
    } else if (name.rfind("AC", 0) == 0 || name.rfind("ADP", 0) == 0) {
      std::ifstream online("/sys/class/power_supply/" + name + "/online");
      int value = 1;
      if (online >> value && value == 0) known = true;
    }
  }
  closedir(dir);
  return known && battery;
}

int64_t system_free_memory_mb() {
  struct sysinfo info{};
  if (sysinfo(&info) != 0) return -1;
  return static_cast<int64_t>(info.freeram) * info.mem_unit / (1024 * 1024);
}

bool process_is_elevated() { return geteuid() == 0; }
unsigned long relaunch_as_administrator() { return 1223; }  // ERROR_CANCELLED: no UAC on Linux

std::vector<std::string> window_process_paths() {
  std::vector<std::string> paths;
  Display* display = XOpenDisplay(nullptr);
  if (!display) return paths;
  const Atom clients = XInternAtom(display, "_NET_CLIENT_LIST", False);
  const Atom pid_atom = XInternAtom(display, "_NET_WM_PID", False);
  Atom type = None;
  int format = 0;
  unsigned long count = 0, after = 0;
  unsigned char* data = nullptr;
  if (XGetWindowProperty(display, DefaultRootWindow(display), clients, 0, 4096, False, XA_WINDOW, &type,
                         &format, &count, &after, &data) == Success && data) {
    const Window* windows = reinterpret_cast<const Window*>(data);
    for (unsigned long i = 0; i < count; ++i) {
      Atom actual_type = None;
      int actual_format = 0;
      unsigned long items = 0, remaining = 0;
      unsigned char* property = nullptr;
      if (XGetWindowProperty(display, windows[i], pid_atom, 0, 1, False, XA_CARDINAL, &actual_type,
                             &actual_format, &items, &remaining, &property) == Success && property && items) {
        const unsigned long pid = *reinterpret_cast<unsigned long*>(property);
        char link[64];
        std::snprintf(link, sizeof(link), "/proc/%lu/exe", pid);
        char target[PATH_MAX];
        const ssize_t length = readlink(link, target, sizeof(target) - 1);
        if (length > 0) {
          target[length] = '\0';
          paths.emplace_back(target);
        }
      }
      if (property) XFree(property);
    }
    XFree(data);
  }
  XCloseDisplay(display);
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

std::string executable_path() {
  char buffer[PATH_MAX];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return "";
  buffer[length] = '\0';
  return buffer;
}

std::string executable_directory() {
  const std::string path = executable_path();
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

}  // namespace bx
