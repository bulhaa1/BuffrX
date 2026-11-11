#include "ui_store.h"

#include <windows.h>

#include <algorithm>

#include "ui_format.h"
#include "../update_policy.h"

namespace bx::ui {

void UiStore::initialise() { load(); }

void UiStore::load() {
  if (loading_) return;
  loading_ = true;
  Json args = Json::object();
  args.set("client", Json("ui"));
  bridge_.call("hello", args, [this](bool ok, const Json& data, const std::string&) {
    loading_ = false;
    if (!ok) {
      connected = false;
      retry_due_ = GetTickCount() + 2000;  // the engine may still be starting
      return;
    }
    hello = data;
    connected = true;
    // A new engine may answer for pictures the old one never made.
    asked_once.clear();
    asked_retry.clear();
    asked_failures.clear();
    dirty = true;
    refresh_config();
    bridge_.call("status.get", Json::object(), [this](bool ok, const Json& data, const std::string&) {
      if (ok) status = data;
      dirty = true;
    });
    refresh_library(0);
    refresh_hotkeys();
    refresh_profiles();
    // Links restored from links.json are not announced again: ask once when the interface connects,
    // or the rail's LIVE LINK pane and Settings stay empty until a link happens to change.
    refresh_links();
  });
}

void UiStore::ensure_exclusions(const Json& user_list) {
  const std::string key = user_list.dump();
  if (key == exclusions_key_) return;
  exclusions_key_ = key;
  bridge_.call("game.exclusions", Json::object(), [this](bool ok, const Json& data, const std::string&) {
    if (ok) exclusions = data["exclusions"];
    dirty = true;
  });
}

void UiStore::refresh_links() {
  bridge_.call("link.list", Json::object(), [this](bool ok, const Json& data, const std::string&) {
    if (ok) links = data;
    dirty = true;
  });
}

void UiStore::handle_event(const std::string& name, const Json& data) {
  dirty = true;
  if (name == "status") {
    // The automatic update speaks once when it becomes ready: a card the person sees even when the panel was
    // closed while it downloaded.
    const Json& update = data["update"];
    if (kAutoUpdate && update["state"].as_string() == "ready") {
      const std::string version = update["version"].as_string();
      if (!version.empty() && version != update_ready_toasted) {
        update_ready_toasted = version;
        toast(UiToast::success, "Update " + version + " is ready", "Restart BuffrX to install it");
      }
    } else if (kAutoUpdate && update["state"].as_string() == "failed") {
      const std::string version = update["version"].as_string();
      if (!version.empty() && version != update_failed_toasted) {
        update_failed_toasted = version;
        const std::string why = update["error"].as_string();
        toast(UiToast::error, "The update did not finish", why.empty() ? "Open Settings to try again" : why);
      }
    }
    status = data;
    connected = true;
  } else if (name == "clip.saved" || name == "screenshot.saved") {
    newest = data["file"].as_string();
    newest_at = GetTickCount();
    ++pulse;
    pulse_at = newest_at;
    // The panel says so itself: the engine's card is drawn over the desktop and is skipped while a
    // fullscreen app runs, which is exactly when this panel is the only thing the user can see.
    if (name == "clip.saved") {
      const std::string folder = data["folder"].as_string();
      const std::string length = format_duration(data["seconds"].as_double());
      toast(UiToast::success, "Clip saved", clip_saved_detail(folder, length));
    } else {
      toast(UiToast::success, "Screenshot saved", file_name(newest));
    }
    refresh_library();
  } else if (name == "hotkeys.captured") {
    pad_capture = data["chord"].as_string();
  } else if (name == "link.changed") {
    links = data;
  } else if (name == "link.received") {
    // A clip from a friend arrived (or a link of this PC's own was opened): it plays at once.
    const std::string file = data["file"].as_string();
    if (!file.empty()) {
      if (!data["id"].as_string().empty()) {
        toast(UiToast::success, "Clip from a friend", "Saved under Shared with me");
      }
    }
    // The finished download stays in the list until its sheet is dismissed: a client that asks for
    // the state later still sees that it finished.
    refresh_library(0);
  } else if (name == "share.changed") {
    // Only the sheet's own job: another client's share is none of this window's business.
    if (!data["id"].as_string().empty() && data["id"].as_string() == share["id"].as_string()) share = data;
  } else if (name == "library.changed") {
    refresh_library(400);
  } else if (name == "timeline.changed") {
    // A session opened, ended or was erased: the panel's Timeline tab reads its list again, if it is looking.
    refresh_sessions(0);
  } else if (name == "error") {
    const std::string message = data["message"].as_string();
    if (!message.empty()) toast(UiToast::error, data["component"].as_string() == "save" ? "Clip not saved" : "BuffrX", message);
  } else if (name == "config.changed") {
    // Somebody else changed the settings - a hotkey, a profile switch, another client on the pipe.
    // The page reads the config again, or it would go on showing the old values and a change would
    // look like it had done nothing.
    refresh_config();
  } else if (name == "game.changed" || name == "recording.changed") {
    // The next status frame carries the details; a config read picks up a profile switch.
    if (name == "game.changed") refresh_config();
  }
}

bool UiStore::tick(uint32_t now) {
  bool changed = false;
  while (!toasts.empty() && static_cast<int32_t>(now - toasts.front().until_ms) >= 0) {
    toasts.pop_front();
    changed = true;
  }
  // Closed windows read nothing: a save while the interface is hidden refreshes the library on open.
  if (library_pending_ && active && static_cast<int32_t>(now - library_due_) >= 0) {
    library_pending_ = false;
    bridge_.call("clips.list", Json::object(), [this](bool ok, const Json& data, const std::string& error) {
      if (ok) {
        clips = media(data, true);
        ++library_version;
        // A cold cache probes only a few files per listing, so the rest of a large library would keep
        // blank durations until something else refreshed it. Keep listing while each pass learns a clip
        // or two, and stop when a pass learns nothing (a file that cannot be probed is not retried).
        int unknown = 0;
        for (const Json& item : data.items()) {
          if (item["duration"].as_double() <= 0.0) ++unknown;
        }
        const bool learned = unknown > 0 && (library_unknown_ < 0 || unknown != library_unknown_);
        library_unknown_ = unknown;
        if (learned) refresh_library(2000);
      }
      else toast(UiToast::error, "The library could not be read", error);
      library_loaded = true;
      dirty = true;
    });
    bridge_.call("screenshots.list", Json::object(), [this](bool ok, const Json& data, const std::string&) {
      if (ok) {
        screenshots = media(data, false);
        ++library_version;
      }
      library_loaded = true;
      dirty = true;
    });
  }
  // The Timeline's sessions, read while its tab is open (the tab asks) and again whenever the engine says the
  // footage changed. A poster already drawn stays: the engine would make the same one again.
  if (sessions_pending_ && active && static_cast<int32_t>(now - sessions_due_) >= 0) {
    sessions_pending_ = false;
    Json args = Json::object();
    args.set("list", Json(true));
    bridge_.call("timeline.get", args, [this](bool ok, const Json& data, const std::string&) {
      if (ok) {
        const std::string current = data["current_session"].as_string();
        std::vector<UiSession> next;
        for (const Json& raw : data["sessions"].items()) {
          UiSession session;
          session.id = raw["id"].as_string();
          session.title = raw["title"].as_string();
          session.started_at = raw["started_at"].as_int();
          session.duration_ms = raw["duration_ms"].as_int();
          session.bytes = raw["bytes"].as_int();
          session.live = !current.empty() && session.id == current;
          for (const UiSession& old : sessions) {
            if (old.id == session.id) session.poster = old.poster;
          }
          // A session whose writer has published nothing has no picture, no length and no size: it is not a card
          // yet. The poll brings it back once a chunk has landed.
          if (session.duration_ms <= 0) continue;
          next.push_back(session);
        }
        std::sort(next.begin(), next.end(), [](const UiSession& a, const UiSession& b) { return a.started_at > b.started_at; });
        sessions = std::move(next);
        sessions_loaded = true;
      }
      dirty = true;
    });
  }
  if (hotkeys_pending_ && static_cast<int32_t>(now - hotkeys_due_) >= 0) {
    hotkeys_pending_ = false;
    bridge_.call("hotkeys.get", Json::object(), [this](bool ok, const Json& data, const std::string&) {
      if (ok) {
        hotkeys = data;
        ++hotkeys_version;
      }
      dirty = true;
    });
  }
  if (!connected && !loading_ && retry_due_ && static_cast<int32_t>(now - retry_due_) >= 0) {
    retry_due_ = 0;
    load();
  }
  // The newest card's highlight and the dial's ripple end on their own.
  if (!newest.empty() && now - newest_at > 4000) {
    newest.clear();
    changed = true;
  }
  return changed;
}

uint32_t UiStore::next_due_ms(uint32_t now) const {
  uint32_t ms = 60000;
  auto due = [&](uint32_t at) { ms = std::min(ms, static_cast<int32_t>(at - now) > 0 ? at - now : 0u); };
  if (!toasts.empty()) due(toasts.front().until_ms);
  if (library_pending_ && active) due(library_due_);
  if (sessions_pending_ && active) due(sessions_due_);
  if (hotkeys_pending_) due(hotkeys_due_);
  if (!connected && !loading_ && retry_due_) due(retry_due_);
  if (!newest.empty()) due(newest_at + 4000);
  return ms;
}

void UiStore::refresh_config() {
  bridge_.call("config.get", Json::object(), [this](bool ok, const Json& data, const std::string&) {
    if (ok) adopt_config(data);
    dirty = true;
  });
}

void UiStore::adopt_config(const Json& data) {
  config = data;
  for (const auto& entry : unsettled_) config.set(entry.first, entry.second.first);
}

void UiStore::refresh_library(uint32_t delay_ms) {
  library_pending_ = true;
  library_due_ = GetTickCount() + delay_ms;
}

void UiStore::refresh_sessions(uint32_t delay_ms) {
  if (sessions_pending_) return;  // one read in flight; the tab asks again on the next frame
  sessions_pending_ = true;
  sessions_due_ = GetTickCount() + delay_ms;
}

void UiStore::refresh_poster(const std::string& id) {
  Json args = Json::object();
  args.set("session", Json(id));
  attempt_once_as("timeline.poster", "timeline.poster:" + id, [args] { return args; }, [this, id](const Json& data) {
    const std::string file = data["file"].as_string();
    for (UiSession& session : sessions) {
      if (session.id == id) session.poster = file;
    }
  });
}

void UiStore::refresh_hotkeys(uint32_t delay_ms) {
  hotkeys_pending_ = true;
  hotkeys_due_ = GetTickCount() + delay_ms;
}

void UiStore::refresh_profiles() {
  bridge_.call("profiles.list", Json::object(), [this](bool ok, const Json& data, const std::string&) {
    if (!ok) return;
    profiles.clear();
    int index = 0;
    for (const Json& raw : data.items()) {
      UiProfile profile;
      profile.index = index;
      const Json& patch = raw["patch"];
      profile.buffer_seconds = static_cast<int>(patch["buffer_seconds"].as_int(60));
      profile.capture_fps = static_cast<int>(patch["capture_fps"].as_int(60));
      // A profile saved before quality levels carries a tier; quality_label reads it as its level.
      profile.quality = patch["quality"].as_string(patch["quality_tier"].as_string("high"));
      // A profile saved before the presets has none: a fixed rate means Custom, else its quality level.
      const std::string fixed = patch["peak_mode"].as_string("auto") == "fixed" ? "custom" : "";
      profile.preset = patch["video_preset"].as_string(fixed.empty() ? profile.quality : fixed);
      profiles.push_back(profile);
      ++index;
    }
    dirty = true;
  });
}

void UiStore::set_config(const Json& patch) {
  const Json before = config;
  for (const auto& field : patch.fields()) {
    config.set(field.first, field.second);
    auto& pending = unsettled_[field.first];
    pending.first = field.second;
    ++pending.second;
  }
  dirty = true;
  Json args = Json::object();
  args.set("patch", patch);
  bridge_.call("config.set", args, [this, before, patch](bool ok, const Json& data, const std::string& error) {
    for (const auto& field : patch.fields()) {
      const auto found = unsettled_.find(field.first);
      if (found != unsettled_.end() && --found->second.second <= 0) unsettled_.erase(found);
    }
    if (ok) {
      adopt_config(data);
    } else {
      // Only the fields this patch wrote go back. Restoring the whole pre-patch snapshot also undid a
      // later patch that had already been applied, which is how one refused setting reverted another.
      // A field a later write still covers keeps that write's value: config.set replies arrive in the
      // order the clicks were made, so nothing newer can be showing for a field whose count is gone.
      for (const auto& field : patch.fields()) {
        if (unsettled_.find(field.first) != unsettled_.end()) continue;
        // A field that was absent before the patch is removed again, not left as a JSON null.
        if (before.has(field.first)) config.set(field.first, before[field.first]);
        else config.erase(field.first);
      }
      toast(UiToast::error, "That setting was not applied", error);
    }
    dirty = true;
  });
}

void UiStore::attempt(const std::string& command, Json args, const std::string& failure,
                      std::function<void(const Json&)> on_ok) {
  bridge_.call(command, std::move(args), [this, failure, on_ok](bool ok, const Json& data, const std::string& error) {
    if (!ok) toast(UiToast::error, failure, error);
    else if (on_ok) on_ok(data);
    dirty = true;
  });
}

void UiStore::attempt_once(const std::string& key, const std::function<Json()>& args,
                           std::function<void(const Json&)> on_ok) {
  attempt_once_as("clips.thumbnail", key, args, std::move(on_ok));
}

void UiStore::attempt_once_as(const std::string& command, const std::string& key, const std::function<Json()>& args,
                              std::function<void(const Json&)> on_ok) {
  // Once per key, for the whole session. A card that is still asking sixty times a second gets one
  // command; when the reply arrives the item carries its thumbnail and the question stops.
  const uint32_t now = GetTickCount();
  const auto retry = asked_retry.find(key);
  if (retry != asked_retry.end()) {
    if (static_cast<int32_t>(now - retry->second) < 0) return;
    asked_retry.erase(retry);
  }
  if (!asked_once.insert(key).second) return;
  // No failure toast: a thumbnail that will not generate leaves the film icon, which is the answer.
  bridge_.call(command, args(), [this, on_ok, command, key](bool ok, const Json& data, const std::string& why) {
    const std::string picture = command == "timeline.poster" ? data["file"].as_string() : data["thumbnail"].as_string();
    if (ok && !picture.empty()) {
      if (on_ok) on_ok(data);
      asked_failures.erase(key);
      dirty = true;
      return;
    }
    // Nothing is final, because the engine's answer is not the file's last word: a clip still being
    // written when its card first asked, or the session the writer has open, becomes a picture on its
    // own. A busy media lock changes in seconds, so it keeps the 30 s beat; anything else backs off to
    // a ten-minute beat, where a file that will never open costs six commands an hour instead of
    // crowding the pipe's two workers, and a picture that became possible still arrives.
    asked_once.erase(key);
    const uint32_t failures = ++asked_failures[key];
    const bool busy = why.find("busy") != std::string::npos;
    const uint32_t cap = busy ? 30000u : 600000u;
    const uint32_t delay = std::min<uint32_t>(cap, 2000u << std::min<uint32_t>(failures - 1, busy ? 4 : 9));
    asked_retry[key] = GetTickCount() + delay;
    dirty = true;
  });
}

void UiStore::toast(UiToast::Kind kind, const std::string& title, const std::string& detail) {
  UiToast item;
  item.id = ++toast_id_;
  item.kind = kind;
  item.title = title;
  item.detail = detail;
  item.until_ms = GetTickCount() + (kind == UiToast::error ? 7000 : 4000);
  toasts.push_back(item);
  while (toasts.size() > 4) toasts.pop_front();
  dirty = true;
}

void UiStore::dismiss(int id) {
  for (auto it = toasts.begin(); it != toasts.end(); ++it) {
    if (it->id == id) {
      toasts.erase(it);
      break;
    }
  }
  dirty = true;
}

std::string UiStore::theme() const { return config["theme"].as_string("clutch"); }

std::vector<UiMedia> UiStore::media(const Json& items, bool clip) const {
  std::vector<UiMedia> out;
  const std::string thumbnails = hello["data_root"].as_string() + "\\thumbnails\\";
  for (const Json& item : items.items()) {
    UiMedia entry;
    entry.file = item["file"].as_string();
    entry.name = item["name"].as_string();
    entry.size = item["size"].as_int();
    entry.mtime = item["mtime"].as_int();
    entry.duration = item["duration"].as_double();
    entry.folder = item["folder"].as_string();
    entry.clip = clip;
    // The protocol names a thumbnail by URL for the web page; this process reads the file itself.
    const std::string thumbnail = item["thumbnail"].as_string();
    if (!thumbnail.empty()) entry.thumbnail = thumbnails + file_name(thumbnail);
    out.push_back(entry);
  }
  return out;
}

}  // namespace bx::ui
