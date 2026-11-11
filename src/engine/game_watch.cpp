#include "game_watch.h"

#include <algorithm>
#include <cctype>

namespace bx {

namespace {

std::string lowered(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char character : value) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  return out;
}

// A process name normalised the way the lists hold it: lowercased with a trailing ".exe" removed.
std::string normalize_exe(const std::string& value) {
  std::string name = lowered(value);
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.erase(name.size() - 4);
  return name;
}

// Rewind's denylist, carried over: a denylist not an allowlist, so an unknown fullscreen app is still a
// candidate. Load-bearing entries are the overlays (RTSS/Afterburner draw on top of a game and would
// win detection outright) and the launchers (Steam sits fullscreen while a game runs).
const char* const kExcluded[] = {
    // Browsers and web apps
    "chrome", "firefox", "msedge", "brave", "opera", "vivaldi", "arc", "chromium", "opera_gx", "waterfox",
    "librewolf", "thorium",
    // Editors and IDEs
    "code", "devenv", "idea64", "rider64", "webstorm64", "pycharm64", "clion64", "sublime_text", "notepad++",
    "atom", "cursor", "zed", "positrond", "vscodium",
    // AI assistants and agents
    "claude", "codex", "opencode", "openchamber", "windsurf", "kiro", "trae", "chatgpt", "mscopilot",
    "copilotapp", "perplexity", "lmstudio", "ollama",
    // Terminals and shells
    "windowsterminal", "powershell", "pwsh", "cmd", "conhost",
    // Communication
    "discord", "goofcord", "spotify", "slack", "teams", "zoom", "telegram", "signal", "whatsapp", "skype",
    // Media players
    "vlc", "mpc-hc", "mpc-hc64", "mpv", "wmplayer", "potplayer", "potplayermini64", "foobar2000",
    // Performance overlays: they draw on top of the game and are often the frontmost window
    "rtss", "msiafterburner",
    // Capture tools and this product line
    "electron", "rewind", "buffrx", "buffrx-engine", "buffrx-ui", "buffrx-launcher",
    "buffrx-editor", "obs64", "obs32", "streamlabs",
    // Shell and system
    "explorer", "dwm", "shellexperiencehost", "searchhost", "systemsettings", "applicationframehost",
    "textinputhost", "lockapp", "startmenuexperiencehost", "searchui", "cortana", "taskmgr", "snippingtool",
    "widgets", "lightshot",
    // Office and documents
    "powerpnt", "excel", "winword", "acrobat", "acrord32", "mstsc", "onenote", "outlook", "notepad",
    // Game launchers, which sit fullscreen while a game runs
    "steam", "steamwebhelper", "epicgameslauncher", "battle.net", "galaxyclient", "riotclientservices",
    "eadesktop", "origin", "gogalaxy", "ubisoftconnect", "bethesda.net_launcher",
};

}  // namespace

bool game_process_excluded(const std::string& exe) {
  const std::string name = normalize_exe(exe);
  if (name.empty()) return true;  // nothing to hook: never a candidate
  for (const char* exclude : kExcluded) {
    if (name == exclude) return true;
  }
  return false;
}

const std::vector<std::string>& builtin_exclusions() {
  static const std::vector<std::string> names(std::begin(kExcluded), std::end(kExcluded));
  return names;
}

bool exclusion_is_builtin(const std::string& name) {
  // An empty entry is nobody's: it is a list entry with nothing in it, not something we ship.
  const std::string normalized = normalize_exe(name);
  if (normalized.empty()) return false;
  return game_process_excluded(name);
}

std::vector<std::string> exclusion_entries(const std::vector<std::string>& user_names,
                                          const std::vector<std::string>& builtin_names) {
  // The user's own entries first: those are the ones they added and the ones they will look for,
  // and burying them under the hundred-odd names the product already excludes makes an entry they
  // just typed look like it did not save. Then the built-ins, in the order they are declared.
  // Anything in both lists appears once, and an empty entry is dropped rather than rendered blank.
  std::vector<std::string> combined;
  combined.reserve(builtin_names.size() + user_names.size());
  const auto seen_before = [&combined](const std::string& name) {
    return std::any_of(combined.begin(), combined.end(), [&](const std::string& earlier) {
      return normalize_exe(earlier) == normalize_exe(name);
    });
  };
  for (const std::string& name : user_names) {
    if (normalize_exe(name).empty() || seen_before(name)) continue;
    combined.push_back(name);
  }
  for (const std::string& name : builtin_names) {
    if (normalize_exe(name).empty() || seen_before(name)) continue;
    combined.push_back(name);
  }
  return combined;
}

std::string normalized_exe_name(const std::string& value) {
  size_t first = 0, last = value.size();
  while (first < last && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
  return normalize_exe(value.substr(first, last - first));
}

bool process_path_is(const std::string& path, const std::string& exe) {
  if (exe.empty()) return false;
  const size_t slash = path.find_last_of("\\/");
  return lowered(slash == std::string::npos ? path : path.substr(slash + 1)) == lowered(exe);
}

bool game_process_excluded(const std::string& exe, const std::vector<std::string>& extra) {
  if (game_process_excluded(exe)) return true;
  const std::string name = normalize_exe(exe);
  for (const std::string& entry : extra) {
    if (!entry.empty() && normalize_exe(entry) == name) return true;
  }
  return false;
}

std::string screenshot_game_title(const std::string& exe, const std::string& title, bool fullscreen,
                                  const std::vector<std::string>& extra) {
  if (!fullscreen || game_process_excluded(exe, extra)) return {};
  return title.empty() ? exe : title;
}

GameCaptureDecision game_capture_observe(GameCaptureState& state, uint32_t now_ms, const std::string& exe,
                                         const std::string& title, bool fullscreen, bool excluded, bool active_alive) {
  const bool candidate = !exe.empty() && fullscreen && !excluded && !game_process_excluded(exe);
  auto clear_candidate = [&state] {
    state.candidate_exe.clear();
    state.candidate_title.clear();
    state.candidate_since_ms = 0;
  };
  if (state.capturing && !active_alive) {
    // The game exited: nothing is left to capture. Any other game in front starts its own clock next sample.
    clear_candidate();
    state.capturing = false;
    state.active_exe.clear();
    state.active_title.clear();
    return GameCaptureDecision::leave;
  }
  if (!candidate) {
    // Alt-tabbed to something that is not a game (the game keeps its capture), or a flicker that was never one.
    clear_candidate();
    return GameCaptureDecision::none;
  }
  if (state.capturing && lowered(exe) == lowered(state.active_exe)) {
    clear_candidate();  // the game itself is in front: nothing to decide
    return GameCaptureDecision::none;
  }
  if (lowered(exe) != lowered(state.candidate_exe)) {
    state.candidate_exe = exe;
    state.candidate_title = title;
    state.candidate_since_ms = now_ms;
    return GameCaptureDecision::none;
  }
  const uint32_t settle = state.capturing ? kGameHandoverSettleMs : kGameEnterSettleMs;
  if (now_ms - state.candidate_since_ms < settle) return GameCaptureDecision::none;
  state.capturing = true;
  state.active_exe = exe;
  state.active_title = title;
  clear_candidate();
  return GameCaptureDecision::enter;
}

}  // namespace bx
