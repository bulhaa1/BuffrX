// What the selected audio slots fail to deliver, and when it is worth a card (audio_routing.h).
//
// A slot that is waiting for its program is not a failure: the page says so on its row, and a person who
// closed Discord on purpose must not be told three times a day that Discord is silent. A card is written
// once per unresolved problem, after it has lasted a grace period (a buffer start, a settings rebuild and
// a program that is still launching all look unresolved for a moment), and the next one waits for recovery
// or for another game. All of it is pure policy, so the wording's timing is unit-tested without libobs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cctype>

namespace bx {

struct AudioHealthSlot {
  bool game = false;
  std::string exe;
  bool connected = false;
};

struct AudioHealthFacts {
  bool apps_mode = false;
  // Something is being recorded now. Nothing is said about audio while the buffer is off.
  bool capture_active = false;
  // The game whose sound the recording is missing: the one the capture covers (audio_warning_game), not the
  // Current game routing target. Empty when no game is in the picture.
  std::string captured_game_exe;
  std::vector<AudioHealthSlot> slots;  // the selected slots, in order
};

enum class AudioProblem {
  none,
  game_missing,    // a game is captured, no slot covers its program, the desktop is not what records
  game_waiting,    // the Current game slot is selected but its capture never connected
  all_unavailable  // every selected slot is silent while something is being recorded
};

// The most important problem right now. `game_missing` and `game_waiting` need a captured game;
// `all_unavailable` needs at least one selected slot.
inline std::string audio_health_lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

inline AudioProblem audio_health_problem(const AudioHealthFacts& facts) {
  if (!facts.apps_mode || !facts.capture_active) return AudioProblem::none;
  const std::string game = audio_health_lower(facts.captured_game_exe);
  bool game_slot = false, game_slot_connected = false;
  bool named_game = false, named_game_connected = false;
  bool any_connected = false;
  for (const AudioHealthSlot& slot : facts.slots) {
    if (slot.game) {
      game_slot = true;
      game_slot_connected = game_slot_connected || slot.connected;
    }
    any_connected = any_connected || slot.connected;
    if (!game.empty() && !slot.game && audio_health_lower(slot.exe) == game) {
      named_game = true;
      named_game_connected = named_game_connected || slot.connected;
    }
  }
  if (!game.empty()) {
    // The game is covered by the Current game slot or by a named slot that is its program. Connected is what
    // counts: a slot that exists but never hooked is not recording the game, even when another app is.
    const bool selected = game_slot || named_game;
    const bool connected = game_slot_connected || named_game_connected;
    if (selected && !connected) return AudioProblem::game_waiting;
    // An empty selection records no game either: no slot exists to cover it.
    if (!selected) return AudioProblem::game_missing;
  }
  // With no selection and no captured game there is nothing to say; all_unavailable needs a slot to call silent.
  if (facts.slots.empty()) return AudioProblem::none;
  if (!any_connected) return AudioProblem::all_unavailable;
  return AudioProblem::none;
}

// The wire name `status.audio.problem` carries, so the page can draw the persistent line from the same pure
// result the card uses. Empty when there is nothing to say.
inline const char* audio_problem_name(AudioProblem problem) {
  switch (problem) {
    case AudioProblem::game_missing: return "game_missing";
    case AudioProblem::game_waiting: return "game_waiting";
    case AudioProblem::all_unavailable: return "all_unavailable";
    case AudioProblem::none: break;
  }
  return "";
}

// What the picture is capturing, as the audio warning needs it. The warning names a game whose sound the person
// is missing, so it is the game in the recording, never the routing target: a display capture shows whatever is
// on that screen, a game capture shows its own game, and a window capture shows one window - a game elsewhere on
// the desktop is not part of what is being recorded. Every field comes from state the tick already maintains.
struct AudioCoverage {
  std::string capture_mode;       // display | window | game
  std::string captured_game_exe;  // the game the capture source is on, empty when none (already liveness-checked)
  std::string detected_game_exe;  // a game the engine's detectors see, empty when none is known
  std::string window_exe;         // a window capture's program, empty when unknown or not a window capture
};

// The game the recording covers: the only one a card may name. Empty when there is no detected game, or when
// the capture does not hold that game's picture (a window capture of another program).
inline std::string audio_warning_game(const AudioCoverage& coverage) {
  if (coverage.capture_mode == "game") return coverage.captured_game_exe;
  const std::string detected = audio_health_lower(coverage.detected_game_exe);
  if (detected.empty()) return std::string();
  if (coverage.capture_mode == "window")
    return audio_health_lower(coverage.window_exe) == detected ? coverage.detected_game_exe : std::string();
  return coverage.detected_game_exe;
}

// Whether the page should draw its line yet. A game starting or a route connecting looks unresolved for a
// moment; a line under the slots that appears and leaves within a frame is noise, not news.
inline bool audio_problem_steady(AudioProblem problem, uint32_t age_ms) {
  return problem != AudioProblem::none && age_ms >= 1500;
}

// The identity a problem is deduplicated by: a different game is a new problem, the same one is not.
inline std::string audio_health_key(const AudioHealthFacts& facts, AudioProblem problem) {
  if (problem == AudioProblem::none) return std::string();
  const std::string game = audio_health_lower(facts.captured_game_exe);
  if (problem == AudioProblem::game_missing) return "game_missing:" + game;
  if (problem == AudioProblem::game_waiting) return "game_waiting:" + game;
  std::string key = "all:";
  for (const AudioHealthSlot& slot : facts.slots) {
    key += slot.game ? std::string("game,") : audio_health_lower(slot.exe) + ",";
  }
  return key;
}

struct AudioNoticeState {
  std::string key;
  uint32_t first_seen_ms = 0;
  bool noticed = false;
};

// True exactly once per unresolved problem, after it has lasted `settle_ms`. A recovered problem (none) or
// a different key forgets the state, so the next occurrence is told once more.
inline bool audio_notice_due(AudioNoticeState& state, AudioProblem problem, const std::string& key,
                             uint32_t now_ms, uint32_t settle_ms = 10000) {
  if (problem == AudioProblem::none || key != state.key) {
    state.key = key;
    state.first_seen_ms = now_ms;
    state.noticed = false;
    return false;
  }
  if (!state.noticed && now_ms - state.first_seen_ms >= settle_ms) {
    state.noticed = true;
    return true;
  }
  return false;
}

}  // namespace bx
