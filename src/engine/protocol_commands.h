// Every command the engine answers: the list `hello` hands a client, the surface `docs/protocol.md` documents,
// and the one place that says whether a command may run outside the engine lock. A pure list, no host and no
// config in it, so a unit test holds the doc and the engine to each other.
#pragma once

#include <string>
#include <vector>
#include <iterator>

namespace bx {

struct CommandSpec {
  const char* name;
  // Slow file work that must not hold the engine lock (command_policy.h): the library, Timeline and sharing
  // families. Everything else runs under it.
  bool lock_free;
};

inline constexpr CommandSpec kProtocolCommands[] = {
    {"hello", false},
    {"status.get", false},
    {"config.get", false},
    {"config.set", false},
    {"config.reset", false},
    {"sources.displays", false},
    {"sources.windows", false},
    {"sources.games", false},
    {"capture.set", false},
    {"buffer.start", false},
    {"buffer.stop", false},
    {"buffer.save", false},
    {"recording.start", false},
    {"recording.stop", false},
    {"screenshot.take", false},
    {"clips.list", true},
    {"clips.rename", false},
    {"clips.delete", false},
    {"clips.trash", false},
    {"clips.reveal", true},
    {"clips.open", true},
    {"clips.thumbnail", true},
    {"clips.frames", true},
    {"screenshots.list", true},
    {"screenshots.reveal", true},
    {"screenshots.rename", false},
    {"screenshots.trash", false},
    {"hotkeys.get", false},
    {"hotkeys.set", false},
    {"devices.list", false},
    {"audio.get", false},
    {"audio.set", false},
    {"profiles.list", false},
    {"profiles.add", false},
    {"profiles.remove", false},
    {"profiles.apply", false},
    {"profiles.save", false},
    {"editor.open", true},
    {"app.open_folder", false},
    {"app.open_url", true},
    {"update.check", true},
    {"update.notes", true},
    {"update.install", true},
    {"engine.restart", false},
    {"storage.usage", false},
    {"sessions.list", false},
    {"session.get", false},
    {"markers.add", false},
    {"markers.remove", false},
    {"markers.list", false},
    {"clips.export_range", true},
    {"sessions.prune", false},
    {"game.state", false},
    {"game.exclusions", false},
    {"timeline.get", true},
    {"timeline.pin", true},
    {"timeline.unpin", true},
    {"timeline.save", true},
    {"timeline.erase", true},
    {"timeline.thumbs", true},
    {"timeline.loudness", true},
    {"timeline.poster", true},
    {"timeline.preview", true},
    {"timeline.open", true},
    {"timeline.edit", true},
    {"timeline.edit_source", true},
    {"encoder.properties", false},
    {"encoder.set", false},
    {"ui.show", false},
    {"ui.share", false},
    {"ui.capture", true},
    {"ui.input", true},
    {"ui.frame_stats", true},
    {"profiles.update", false},
    {"clips.copy", true},
    {"app.pick_folder", true},
    {"app.pick_file", true},
    {"library.notify", true},
    {"hotkeys.suspend", false},
    {"hotkeys.capture", false},
    {"pad.navigate", false},
    {"share.prepare", true},
    {"share.get", true},
    {"share.copy", true},
    {"share.discord", true},
    {"share.cancel", true},
    {"share.webhook.get", true},
    {"share.webhook.set", true},
    {"share.webhook.clear", true},
    {"link.share", true},
    {"link.copy", true},
    {"link.stop", true},
    {"link.list", true},
    {"link.open", true},
    {"link.answer", true},
    {"link.cancel", true},
};

inline std::vector<std::string> protocol_capabilities() {
  std::vector<std::string> names;
  names.reserve(std::size(kProtocolCommands));
  for (const CommandSpec& command : kProtocolCommands) names.emplace_back(command.name);
  return names;
}

// Whether a command runs under the engine lock. An unknown command is locked by default.
inline bool command_needs_engine_lock(const std::string& command) {
  for (const CommandSpec& spec : kProtocolCommands) {
    if (command == spec.name) return !spec.lock_free;
  }
  return true;
}

}  // namespace bx
