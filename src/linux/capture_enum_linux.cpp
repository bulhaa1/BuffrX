// Linux window enumeration for the capture pickers and game detection: EWMH properties on X11.
//
// The id format is the same "title:class:exe" the Windows build writes, so a saved config and the
// editor's protocol keep their meaning. libobs's xcomposite_input identifies a window by its title
// (the title alone), which is what configure_capture() hands it on Linux.
#include "../engine/capture_enum.h"

#include "../engine/game_watch.h"
#include "../engine/process_cache.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bx {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
bool equal_ci(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

struct DisplayHolder {
  Display* display = nullptr;
  std::mutex mutex;
  DisplayHolder() { XInitThreads(); display = XOpenDisplay(nullptr); }
  ~DisplayHolder() {
    if (display) XCloseDisplay(display);
  }
};
DisplayHolder& display_holder() {
  static DisplayHolder holder;
  return holder;
}

int ignore_x_error(Display*, XErrorEvent*) { return 0; }

bool window_alive(Display* display, Window window) {
  if (!display || !window) return false;
  int previous = 0;
  XErrorHandler handler = XSetErrorHandler(ignore_x_error);
  XWindowAttributes attributes{};
  const bool ok = XGetWindowAttributes(display, window, &attributes) != 0;
  XSync(display, False);
  XSetErrorHandler(handler);
  return ok;
}

std::string utf8_property(Display* display, Window window, Atom atom) {
  Atom type = None;
  int format = 0;
  unsigned long count = 0, remaining = 0;
  unsigned char* data = nullptr;
  if (XGetWindowProperty(display, window, atom, 0, 4096, False, AnyPropertyType, &type, &format, &count,
                         &remaining, &data) != Success || !data)
    return {};
  std::string text(reinterpret_cast<char*>(data), count);
  XFree(data);
  return text;
}

Atom atom(Display* display, const char* name) { return XInternAtom(display, name, True); }

std::string window_title(Display* display, Window window) {
  const Atom net_name = atom(display, "_NET_WM_NAME");
  const Atom utf8 = atom(display, "UTF8_STRING");
  if (net_name != None) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, net_name, 0, 4096, False, utf8, &type, &format, &count,
                           &remaining, &data) == Success && data && type == utf8) {
      std::string text(reinterpret_cast<char*>(data), count);
      XFree(data);
      return text;
    }
    if (data) XFree(data);
  }
  char* name = nullptr;
  if (XFetchName(display, window, &name) && name) {
    std::string text = name;
    XFree(name);
    return text;
  }
  return {};
}

std::string window_class(Display* display, Window window) {
  XClassHint hint{};
  if (!XGetClassHint(display, window, &hint)) return {};
  std::string result = hint.res_class ? hint.res_class : (hint.res_name ? hint.res_name : "");
  if (hint.res_name) XFree(hint.res_name);
  if (hint.res_class) XFree(hint.res_class);
  return result;
}

uint32_t window_pid(Display* display, Window window) {
  const Atom pid_atom = atom(display, "_NET_WM_PID");
  if (pid_atom == None) return 0;
  Atom type = None;
  int format = 0;
  unsigned long count = 0, remaining = 0;
  unsigned char* data = nullptr;
  uint32_t pid = 0;
  if (XGetWindowProperty(display, window, pid_atom, 0, 1, False, XA_CARDINAL, &type, &format, &count,
                         &remaining, &data) == Success && data && count)
    pid = *reinterpret_cast<uint32_t*>(data);
  if (data) XFree(data);
  return pid;
}

std::string process_image_name(uint32_t pid) {
  if (pid == 0) return {};
  char link[64];
  std::snprintf(link, sizeof(link), "/proc/%u/exe", pid);
  char target[PATH_MAX];
  const ssize_t length = readlink(link, target, sizeof(target) - 1);
  if (length <= 0) return {};
  target[length] = '\0';
  const char* slash = std::strrchr(target, '/');
  return slash ? slash + 1 : target;
}

struct WindowIdentity {
  std::string title;
  std::string window_class;
  std::string exe;
};

std::string make_identity(Display* display, Window window, WindowIdentity& out) {
  out.title = window_title(display, window);
  out.window_class = window_class(display, window);
  if (out.title.empty() && out.window_class.empty()) return {};
  const uint32_t pid = window_pid(display, window);
  out.exe = process_image_name(pid);
  if (out.exe.empty()) out.exe = "unknown";
  return make_window_id(out.title, out.window_class, out.exe);
}

bool split_window_key(const std::string& key, WindowIdentity& out) {
  const size_t first = key.find(':');
  if (first == std::string::npos) {
    out.title = decode_window_part(key);
    return !key.empty();
  }
  const size_t second = key.find(':', first + 1);
  if (second == std::string::npos) {
    out.title = decode_window_part(key.substr(0, first));
    out.window_class = decode_window_part(key.substr(first + 1));
    return !out.title.empty() || !out.window_class.empty();
  }
  out.title = decode_window_part(key.substr(0, first));
  out.window_class = decode_window_part(key.substr(first + 1, second - first - 1));
  out.exe = decode_window_part(key.substr(second + 1));
  return true;
}

bool window_state_hidden(Display* display, Window window) {
  const Atom state = atom(display, "_NET_WM_STATE");
  const Atom hidden = atom(display, "_NET_WM_STATE_HIDDEN");
  if (state == None || hidden == None) return false;
  Atom type = None;
  int format = 0;
  unsigned long count = 0, remaining = 0;
  unsigned char* data = nullptr;
  bool found = false;
  if (XGetWindowProperty(display, window, state, 0, 64, False, XA_ATOM, &type, &format, &count, &remaining,
                         &data) == Success && data) {
    const Atom* values = reinterpret_cast<const Atom*>(data);
    for (unsigned long i = 0; i < count; ++i) found = found || values[i] == hidden;
    XFree(data);
  }
  return found;
}

bool window_capturable(Display* display, Window window, bool allow_minimized) {
  if (!window_alive(display, window)) return false;
  XWindowAttributes attributes{};
  if (!XGetWindowAttributes(display, window, &attributes)) return false;
  if (attributes.map_state != IsViewable) return false;
  if (!allow_minimized && window_state_hidden(display, window)) return false;
  if (attributes.depth == 0) return false;
  return true;
}

struct Geometry {
  int x = 0, y = 0, width = 0, height = 0;
};

Geometry client_geometry(Display* display, Window window) {
  Geometry result;
  XWindowAttributes attributes{};
  if (XGetWindowAttributes(display, window, &attributes)) {
    result.width = attributes.width;
    result.height = attributes.height;
    Window child = 0;
    XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &result.x, &result.y, &child);
  }
  return result;
}

bool window_fullscreen(Display* display, Window window) {
  const Geometry geometry = client_geometry(display, window);
  const int screen_width = DisplayWidth(display, DefaultScreen(display));
  const int screen_height = DisplayHeight(display, DefaultScreen(display));
  if (screen_width <= 0 || screen_height <= 0) return false;
  const bool fullscreen_state = [&] {
    const Atom state = atom(display, "_NET_WM_STATE");
    const Atom full = atom(display, "_NET_WM_STATE_FULLSCREEN");
    if (state == None || full == None) return false;
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    bool found = false;
    if (XGetWindowProperty(display, window, state, 0, 64, False, XA_ATOM, &type, &format, &count,
                           &remaining, &data) == Success && data) {
      const Atom* values = reinterpret_cast<const Atom*>(data);
      for (unsigned long i = 0; i < count; ++i) found = found || values[i] == full;
      XFree(data);
    }
    return found;
  }();
  return fullscreen_state ||
         game_fullscreen_rule(static_cast<double>(geometry.width) / screen_width,
                              static_cast<double>(geometry.height) / screen_height, false);
}

std::vector<Window> top_level_windows(Display* display) {
  std::vector<Window> windows;
  const Atom clients = atom(display, "_NET_CLIENT_LIST");
  if (clients != None) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, DefaultRootWindow(display), clients, 0, 4096, False, XA_WINDOW, &type,
                           &format, &count, &remaining, &data) == Success && data) {
      const Window* values = reinterpret_cast<Window*>(data);
      windows.assign(values, values + count);
      XFree(data);
    }
  }
  if (windows.empty()) {
    Window root = 0, parent = 0, *children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(display, DefaultRootWindow(display), &root, &parent, &children, &count) && children) {
      windows.assign(children, children + count);
      XFree(children);
    }
  }
  return windows;
}

uintptr_t best_window_match(Display* display, const WindowIdentity& wanted) {
  uintptr_t best = 0;
  int best_rating = 0x7FFFFFFF;
  for (Window window : top_level_windows(display)) {
    if (!window_capturable(display, window, true)) continue;
    WindowIdentity current;
    make_identity(display, window, current);
    int rating = -1;
    if (!wanted.window_class.empty() && equal_ci(wanted.window_class, current.window_class) &&
        equal_ci(wanted.exe, current.exe)) {
      rating = equal_ci(wanted.title, current.title) ? 0 : 1;
    } else if (!wanted.title.empty() && equal_ci(wanted.title, current.title)) {
      rating = 2;
    }
    if (rating < 0 || rating >= best_rating) continue;
    best_rating = rating;
    best = window;
    if (rating == 0) break;
  }
  return best;
}

std::vector<WindowInfo> enumerate(bool games_only) {
  std::vector<WindowInfo> out;
  Display* display = display_holder().display;
  if (!display) return out;
  const uint32_t own_pid = static_cast<uint32_t>(getpid());
  for (Window window : top_level_windows(display)) {
    if (!window_capturable(display, window, !games_only)) continue;
    WindowIdentity identity;
    const std::string id = make_identity(display, window, identity);
    if (id.empty() || identity.title.empty()) continue;
    const uint32_t pid = window_pid(display, window);
    if (games_only) {
      if (pid == 0 || pid == own_pid) continue;
      if (game_process_excluded(identity.exe)) continue;
      if (!window_fullscreen(display, window)) continue;
    }
    WindowInfo info;
    info.title = id;
    info.label = identity.title;
    info.exe = identity.exe;
    info.minimized = window_state_hidden(display, window);
    info.hwnd = static_cast<uintptr_t>(window);
    out.push_back(info);
  }
  return out;
}

}  // namespace

std::string encode_window_part(const std::string& part) {
  std::string out = part;
  for (size_t at = out.find('#'); at != std::string::npos; at = out.find('#', at + 3)) out.replace(at, 1, "#22");
  for (size_t at = out.find(':'); at != std::string::npos; at = out.find(':', at + 3)) out.replace(at, 1, "#3A");
  return out;
}
std::string decode_window_part(const std::string& value) {
  std::string out = value;
  for (size_t at = out.find("#3A"); at != std::string::npos; at = out.find("#3A", at)) out.replace(at, 3, ":");
  for (size_t at = out.find("#22"); at != std::string::npos; at = out.find("#22", at)) out.replace(at, 3, "#");
  return out;
}
std::string make_window_id(const std::string& title, const std::string& window_class, const std::string& exe) {
  return encode_window_part(title) + ":" + encode_window_part(window_class) + ":" + encode_window_part(exe);
}

bool game_fullscreen_rule(double ratio_x, double ratio_y, bool maximized_with_frame, bool exclusive_foreground) {
  return !maximized_with_frame &&
         ((ratio_x >= 0.9 && ratio_y >= 0.9) || (exclusive_foreground && ratio_x >= 0.5 && ratio_y >= 0.5));
}

bool window_still_exists(uintptr_t hwnd) { return window_alive(display_holder().display, static_cast<Window>(hwnd)); }

bool foreground_window(ForegroundWindow& out) {
  out = ForegroundWindow{};
  Display* display = display_holder().display;
  if (!display) return false;
  const Atom active = atom(display, "_NET_ACTIVE_WINDOW");
  if (active == None) return false;
  Atom type = None;
  int format = 0;
  unsigned long count = 0, remaining = 0;
  unsigned char* data = nullptr;
  Window window = 0;
  if (XGetWindowProperty(display, DefaultRootWindow(display), active, 0, 1, False, XA_WINDOW, &type, &format,
                         &count, &remaining, &data) == Success && data && count)
    window = *reinterpret_cast<Window*>(data);
  if (data) XFree(data);
  if (!window) return false;
  WindowIdentity identity;
  const std::string id = make_identity(display, window, identity);
  if (id.empty()) return false;
  out.hwnd = static_cast<uintptr_t>(window);
  out.id = id;
  out.title = identity.title;
  out.exe = identity.exe == "unknown" ? "" : identity.exe;
  out.fullscreen = window_fullscreen(display, window);
  return true;
}

std::vector<WindowInfo> enumerate_windows() { return enumerate(false); }
std::vector<WindowInfo> enumerate_game_windows() { return enumerate(true); }

bool window_id_exists(const std::string& id) {
  WindowIdentity wanted;
  if (!split_window_key(id, wanted)) return false;
  return best_window_match(display_holder().display, wanted) != 0;
}

bool capture_target_alive(const std::string& id, bool process_matters) {
  if (id.empty()) return true;
  WindowIdentity wanted;
  if (!split_window_key(id, wanted)) return true;
  Display* display = display_holder().display;
  if (best_window_match(display, wanted)) return true;
  if (!process_matters) return false;
  if (wanted.exe.empty()) return false;
  for (const WindowInfo& window : enumerate_windows()) {
    if (equal_ci(window.exe, wanted.exe)) return true;
  }
  return false;
}

bool window_id_is_own_process(const std::string& id) {
  WindowIdentity wanted;
  if (!split_window_key(id, wanted)) return false;
  Display* display = display_holder().display;
  const uintptr_t window = best_window_match(display, wanted);
  if (!window) return false;
  return window_pid(display, static_cast<Window>(window)) == static_cast<uint32_t>(getpid());
}

bool find_window_size(const std::string& wanted_id, int& width, int& height) {
  WindowIdentity wanted;
  if (!split_window_key(wanted_id, wanted)) return false;
  Display* display = display_holder().display;
  const uintptr_t window = best_window_match(display, wanted);
  if (!window) return false;
  const Geometry geometry = client_geometry(display, static_cast<Window>(window));
  width = geometry.width;
  height = geometry.height;
  return width > 0 && height > 0;
}

bool validate_capture_target(Config& config, std::string& reason) {
  if (config.capture_mode != "window" && config.capture_mode != "game") return true;
  if (config.capture_mode == "game" && config.game_title.empty()) return true;
  bool exists = false;
  bool own = false;
  if (config.capture_mode == "game") {
    for (const auto& game : enumerate_game_windows()) {
      if (game.title == config.game_title) {
        exists = true;
        break;
      }
    }
    own = window_id_is_own_process(config.game_title);
  } else {
    exists = window_id_exists(config.window_id);
    own = window_id_is_own_process(config.window_id);
  }
  if (exists && !own) return true;
  reason = own ? "the saved capture target resolves to BuffrX itself"
               : "the saved capture target is not on screen right now";
  config.capture_mode = "display";
  return false;
}

}  // namespace bx
