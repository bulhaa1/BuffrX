// The excluded-process denylist and the focus-based capture policy. Pure policy: no Win32 and no libobs
// in here, so every decision is unit-tested; the tick feeds the foreground window in.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bx {

// A process name as the lists hold it: trimmed, lowercase, without a trailing ".exe". "" for a blank.
std::string normalized_exe_name(const std::string& value);

// True when `path` (a process image path, either slash, or a bare name) names the executable `exe`: the file
// name alone, compared without case. "" names nothing.
bool process_path_is(const std::string& path, const std::string& exe);


// Processes that are never games even when fullscreen: a browser, editor, terminal, chat client,
// media player, overlay, launcher or shell process sitting fullscreen must not switch capture. Names
// are the process image name lowercased with ".exe" removed, which is how the check compares them.
bool game_process_excluded(const std::string& exe);
// The same check plus the user's own exclusion list (from config.game_exclusions).
bool game_process_excluded(const std::string& exe, const std::vector<std::string>& extra);
// Name a fullscreen foreground game for a screenshot, independently of the selected capture source.
std::string screenshot_game_title(const std::string& exe, const std::string& title, bool fullscreen,
                                  const std::vector<std::string>& extra);
//
// Whether a name is one of the product's own entries rather than the user's. The panel shows both
// lists and must not offer to delete one of ours: it would come back at the next launch, because the
// denylist in game_watch.cpp is what decides.
bool exclusion_is_builtin(const std::string& name);

// The two lists as the panel renders them: the user's own first (the entries they just added), then
// the product's own in the order they are declared, with anything in both appearing once. Pure, so the
// page's ordering and its duplicate handling are testable without an interface.
std::vector<std::string> exclusion_entries(const std::vector<std::string>& user_names,
                                          const std::vector<std::string>& builtin_names);

// Rewind's denylist as it stands, in the order it is declared: the panel shows it so a person can see
// what the product already treats as "not a game" without opening a source file.
const std::vector<std::string>& builtin_exclusions();

// Focus-based game capture, decided on time rather than on poll counts. The caller samples the foreground
// every tick (250 ms) and hands it in with the tick clock: `exe`/`title` are the foreground window's process
// and title, `fullscreen` the caller's verdict, `excluded` the user's own extra exclusions, `active_alive`
// whether the game being captured still has a window.
//   - A fullscreen game that has held the foreground for kGameEnterSettleMs enters. It used to take two polls
//     five seconds apart - five to ten seconds before the buffer was on the game - and a splash screen or a
//     launcher that flickers fullscreen still does not count: any sample that is not that game resets the clock.
//   - Once capturing, an alt-tab to something that is not a game keeps the game (Rewind's rule: the hook keeps
//     delivering it, and a clip of the moment before the alt-tab should still be the game).
//   - A different fullscreen game takes over directly - one `enter`, no trip back to the display - after the
//     longer kGameHandoverSettleMs, because leaving a live game for another window is not done on a glance.
//   - The game exiting leaves on the very next sample.
constexpr uint32_t kGameEnterSettleMs = 500;
constexpr uint32_t kGameHandoverSettleMs = 1500;

struct GameCaptureState {
  std::string candidate_exe;
  std::string candidate_title;
  uint32_t candidate_since_ms = 0;  // when the candidate first held the foreground; meaningful while candidate_exe is set
  bool capturing = false;
  std::string active_exe;
  std::string active_title;
};

enum class GameCaptureDecision { none, enter, leave };

GameCaptureDecision game_capture_observe(GameCaptureState& state, uint32_t now_ms, const std::string& exe,
                                         const std::string& title, bool fullscreen, bool excluded, bool active_alive);

}  // namespace bx
