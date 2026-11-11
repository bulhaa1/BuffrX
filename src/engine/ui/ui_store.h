// What the native interface knows about the engine, kept in one place: the engine's own JSON (its
// snake_case is the contract, so nothing is renamed), the library, profiles, hotkeys and the
// interface's own toasts. Lives on the interface's thread; the bridge delivers replies and events.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../shared/json.h"
#include "ui_bridge.h"

namespace bx::ui {

struct UiMedia {
  std::string file, name, thumbnail, folder;  // thumbnail: a local file path, "" when none
  int64_t size = 0, mtime = 0;
  double duration = 0;
  bool clip = true;
};

// A file a link brought in: it sits in the library's own "Shared with me", and the interface shows it only
// under Shared. Everything else (preview, delete, rename, share) works on it as on any other save.
inline bool shared_item(const UiMedia& item) { return item.folder == "Shared with me"; }

// One Timeline session: the footage the engine kept for a stretch of play, not a file the person saved. It is
// temporary (the retention window deletes it), so a card for one is a card for something that can go away.
struct UiSession {
  std::string id, title, poster;  // poster: a local file path, "" until the engine has drawn one
  int64_t started_at = 0, duration_ms = 0, bytes = 0;
  bool live = false;  // the writer is on this session now
};

struct UiProfile {
  int index = 0;
  int buffer_seconds = 60;
  int capture_fps = 60;
  std::string quality = "high";  // the profile's quality level (a version 3 tier reads as its level)
  std::string preset = "high";   // low | standard | high | highest | custom, the Video preset it saves
};

struct UiToast {
  int id = 0;
  enum Kind { success, info, error } kind = info;
  std::string title, detail;
  uint32_t until_ms = 0;
};

class UiStore {
public:
  explicit UiStore(UiBridge& bridge) : bridge_(bridge) {}

  void initialise();
  void handle_event(const std::string& name, const Json& data);
  // Timers: toasts expiring, the debounced library refresh, the late hotkey read-back.
  // True when something visible changed.
  bool tick(uint32_t now_ms);
  // Milliseconds until the earliest timer tick() owns is due (a toast expiring, a read waiting for its delay), 60 s when none.
  uint32_t next_due_ms(uint32_t now_ms) const;

  void refresh_config();
  void refresh_library(uint32_t delay_ms = 150);
  // The Timeline's sessions (timeline.get with list), read while its tab is open: chunks come and go with the
  // retention window, so the list is read again rather than remembered.
  void refresh_sessions(uint32_t delay_ms = 0);
  // One session's poster (timeline.poster), asked for once per session for the whole session.
  void refresh_poster(const std::string& id);
  void refresh_hotkeys(uint32_t delay_ms = 0);
  void refresh_profiles();
  // Writes settings: shown at once, put back if the engine refuses (with a toast saying so).
  void set_config(const Json& patch);
  // Runs a command; a failure becomes an error toast titled `failure`.
  void attempt(const std::string& command, Json args, const std::string& failure,
               std::function<void(const Json&)> on_ok = {});
  // attempt() that runs at most once per key: for the requests a frame can repeat sixty times a
  // second (a card asking for its own thumbnail), where flooding the pipe is the thing being avoided.
  void attempt_once(const std::string& key, const std::function<Json()>& args,
                    std::function<void(const Json&)> on_ok = {});
  // attempt_once with the command named: a Timeline session's poster fills a cache of its own, from the same
  // once-per-key rule (a card may ask on every frame it is drawn).
  void attempt_once_as(const std::string& command, const std::string& key, const std::function<Json()>& args,
                       std::function<void(const Json&)> on_ok = {});
  // A command whose reply the caller handles itself (on the interface thread).
  void bridge_call(const std::string& command, Json args, UiBridge::Reply reply) {
    bridge_.call(command, std::move(args), [this, reply](bool ok, const Json& data, const std::string& error) {
      if (reply) reply(ok, data, error);
      dirty = true;
    });
  }
  void toast(UiToast::Kind kind, const std::string& title, const std::string& detail = std::string());
  void dismiss(int id);

  std::string theme() const;  // the stored theme id (themes.h); palette_for maps anything else to the main theme
  std::string chord(const std::string& action, bool controller = false) const {
    return hotkeys[controller ? action + "_pad" : action].as_string();
  }
  // The chord the pads are holding while a controller binding records (hotkeys.captured).
  std::string pad_capture;
  bool buffer_on() const { return status["buffer_state"].as_string() == "on"; }

  bool connected = false;
  Json hello, status, config, hotkeys;
  // Window-thread samples, updated faster than the engine's status event while a meter is visible.
  float desktop_level = 0.0f, mic_level = 0.0f;
  // What Settings -> Games lists as never a game (`game.exclusions`: the built-in names and the user's own). Not part of the
  // status, which is built twice a second: it is read when that section is drawn and read again when the user's list changes.
  Json exclusions;
  void ensure_exclusions(const Json& user_list);
  std::vector<UiMedia> clips, screenshots;
  std::vector<UiSession> sessions;  // the Timeline's footage, newest first; only read while its tab is open
  bool library_loaded = false;
  bool sessions_loaded = false;
  std::vector<UiProfile> profiles;
  // The share sheet's job as the engine reports it (share.changed), {} when no sheet is open, and the
  // webhook's state ({set, hint}; null until read).
  Json share = Json::object();
  Json webhook;
  // BuffrX links ({links, receives, page_set}), as link.changed last said; null until first read.
  Json links;
  void refresh_links();
  std::string copied_link;  // the last link this PC copied itself: never offered back to it
  // The update version whose "ready" toast was already shown in this run, so a status tick every 500 ms does
  // not repeat it.
  std::string update_ready_toasted;
  std::string update_failed_toasted;
  std::deque<UiToast> toasts;
  // A card's picture that could not be made is retried on a slow beat, not once per frame: 30 s while
  // the media lock is busy, up to ten minutes otherwise, so a picture that becomes possible later
  // arrives and a file that never opens costs little. The key is forgotten at `asked_retry`'s due
  // time, and `load()` forgets every key when the engine is new.
  std::set<std::string> asked_once;
  std::map<std::string, uint32_t> asked_retry;
  std::map<std::string, uint32_t> asked_failures;  // consecutive failures: the retry backs off, and how far says which beat
  std::string newest;           // the file saved last, highlighted for a few seconds
  uint32_t newest_at = 0;
  int pulse = 0;                // bumps on every save: the dial's ripple
  uint32_t pulse_at = 0;
  bool dirty = true;            // something changed since the last frame
  bool active = false;          // a window is open: the library is only read then
  int hotkeys_version = 0;      // bumps each time the hotkeys are read
  int library_version = 0;      // bumps each time a library list arrives

private:
  void load();
  // The engine's config as a reply carries it, with the fields this window has written and not yet had answered laid
  // back over it: a reply that left the engine before the last click would otherwise flip the page back for a moment.
  void adopt_config(const Json& data);
  std::vector<UiMedia> media(const Json& items, bool clip) const;

  UiBridge& bridge_;
  int toast_id_ = 0;
  uint32_t library_due_ = 0;
  uint32_t hotkeys_due_ = 0;
  bool sessions_pending_ = false;
  uint32_t sessions_due_ = 0;
  uint32_t retry_due_ = 0;
  bool library_pending_ = false;
  // The previous library listing's count of clips whose duration is still unknown: while the persistent
  // probe cache fills, a listing that learned something schedules the next one; one that learned nothing
  // stops, so a clip that cannot be probed costs one attempt instead of a loop.
  int library_unknown_ = -1;
  bool hotkeys_pending_ = false;
  std::string exclusions_key_;  // the user's list as `exclusions` was last read for it
  bool loading_ = false;
  std::map<std::string, std::pair<Json, int>> unsettled_;  // field -> (value written, writes not yet answered)
};

}  // namespace bx::ui
