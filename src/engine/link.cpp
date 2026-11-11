#include "link.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "../shared/files.h"
#include "../shared/log.h"
#include "clip_check.h"
#include "library.h"
#include "link_math.h"
#include "link_secret.h"
#include "short_link.h"
#include "platform.h"

namespace bx {
namespace {

constexpr int64_t kMaxBytes = 2LL * 1024 * 1024 * 1024;       // the largest clip a link carries, as the site says
constexpr int64_t kAskAbove = 300LL * 1024 * 1024;            // larger than this, the receiver says yes first
constexpr int64_t kSpareSpace = 512LL * 1024 * 1024;          // never fill the disk to the last byte
constexpr int kMaxWatching = 4;                                // downloads at once, per link
constexpr int kMaxViewers = 25;                                // downloads in total, per link
constexpr int kMaxReceives = 3;
// Finished entries stay so a client can see how a download ended; the oldest leaves past this many.
constexpr int kMaxFinishedReceives = 8;
constexpr int kGamingKbps = 25000;   // while a game has focus, a capped link drops to this
// The helper says ready after at most 10 s through a proxy that does not work plus 20 s without it: a wait
// shorter than that failed the first share behind a stale proxy setting while the helper was still starting.
constexpr int kHelperReadySeconds = 35;

int64_t now_unix() { return static_cast<int64_t>(std::time(nullptr)); }

// The proxy Windows is set to use for this account (Settings > Network > Proxy, or a school's or an
// office's policy), as one http:// address, or "". The helper falls back to HTTP_PROXY when it is "".
// shortcut: a PAC script setting (lpszAutoConfigUrl) is not evaluated; use WinHttpGetProxyForUrl if a
// network that only offers a PAC script and blocks direct HTTPS turns up.
std::string windows_proxy() {
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
  if (!WinHttpGetIEProxyConfigForCurrentUser(&config)) return std::string();
  std::string setting;
  if (config.lpszProxy) {
    for (const wchar_t* c = config.lpszProxy; *c; ++c) setting += *c < 128 ? static_cast<char>(*c) : '?';
  }
  if (config.lpszProxy) GlobalFree(config.lpszProxy);
  if (config.lpszProxyBypass) GlobalFree(config.lpszProxyBypass);
  if (config.lpszAutoConfigUrl) GlobalFree(config.lpszAutoConfigUrl);
  return proxy_from_windows(setting);
}

// "" when Windows' random source failed: a key or a token made of zeros is guessable, so a caller
// refuses to make or serve a link rather than use one.
std::string random_hex(size_t bytes) {
  std::vector<uint8_t> data(bytes);
  if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    BX_LOG_WARN("the system random source failed");
    return std::string();
  }
  return hex_encode(data.data(), data.size());
}

int64_t file_size(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return -1;
  return (static_cast<int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string read_text(const std::string& path) {
  std::string text;
  if (FILE* file = std::fopen(path.c_str(), "rb")) {
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
  }
  return text;
}

bool is_video(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  std::string extension = dot == std::string::npos ? std::string() : name.substr(dot);
  for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return extension == ".mp4" || extension == ".mkv";
}

bool is_image(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  std::string extension = dot == std::string::npos ? std::string() : name.substr(dot);
  for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return extension == ".png" || extension == ".jpg" || extension == ".jpeg";
}

}  // namespace

// ---------------------------------------------------------------- a receive's own rules

bool receive_retryable(const std::string& reason) {
  // The helper could not get there, the wire stopped carrying bytes, or the file arrived short: the
  // same link can still deliver it. Anything else is a decision (the user's, the sender's, the link's)
  // or a disk that will say the same thing again.
  return reason == "unreachable" || reason == "stalled" || reason == "unresponsive" || reason == "busy" ||
         reason == "helper" || reason == "incomplete";
}

int receive_tries(bool reached_sharer) {
  return reached_sharer ? kReceiveTries : kConnectTries;
}

uint64_t receive_backoff_ms(int attempt) {
  // Long enough that a friend's PC coming back from sleep or a relay hiccup is worth another try, and,
  // since a retry resumes where the last attempt stopped, cheap enough to try five times in a minute.
  if (attempt <= 1) return 1000;
  if (attempt == 2) return 3000;
  if (attempt == 3) return 8000;
  if (attempt == 4) return 15000;
  return 30000;
}

std::string receive_watchdog(const std::string& state, uint64_t last_progress, uint64_t now) {
  uint64_t limit = 0;
  if (state == "connecting") limit = kConnectLimitMs;
  else if (state == "receiving") limit = kStallLimitMs;
  if (limit == 0 || last_progress == 0 || now <= last_progress) return std::string();
  return now - last_progress > limit ? std::string("unresponsive") : std::string();
}

// The stall clock runs on bytes, not on events: a progress line that carries no new bytes says nothing
// about the wire, so it must not keep a dead transfer alive. The count never goes back either, or a
// resumed attempt reporting the older offset would move the clock and the bar backwards.
bool receive_progress_advanced(int64_t& held, int64_t incoming) {
  if (incoming <= held) return false;
  held = incoming;
  return true;
}

bool receive_clock_moves(int64_t& held, int64_t incoming, bool checking) {
  // The helper hashes the finished file before it says "done", and counts those bytes from zero again:
  // they are proof the helper is alive, though none of them is new.
  return receive_progress_advanced(held, incoming) || checking;
}

bool received_image_ok(const std::string& name, const uint8_t* head, size_t size) {
  static const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  static const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF};
  const size_t dot = name.find_last_of('.');
  std::string extension = dot == std::string::npos ? std::string() : name.substr(dot);
  for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (extension == ".png") return size >= sizeof(png) && std::equal(png, png + sizeof(png), head);
  if (extension == ".jpg" || extension == ".jpeg") return size >= sizeof(jpeg) && std::equal(jpeg, jpeg + sizeof(jpeg), head);
  return false;
}

bool receive_name_is_safe(const std::string& name) {
  if (name.empty() || name.size() > 160) return false;
  if (name.find_first_of("\\/:*?\"<>|") != std::string::npos) return false;
  if (name.find("..") != std::string::npos) return false;
  for (char c : name) {
    if (static_cast<unsigned char>(c) < 0x20) return false;
  }
  if (name.front() == ' ' || name.front() == '.') return false;
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return false;
  // Windows ignores a trailing space or dot in a name and refuses the DOS device names, so neither is a
  // name this can file: it would fail in MoveFileExA with an error the user cannot act on.
  std::string stem = name.substr(0, dot);
  while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
  if (stem.empty()) return false;
  std::string upper = stem;
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return false;
  if (upper.size() == 4 && (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) &&
      upper[3] >= '1' && upper[3] <= '9') {
    return false;
  }
  std::string extension = name.substr(dot);
  for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // The same set received_file_name allows: the containers BuffrX writes, and its screenshots.
  return extension == ".mp4" || extension == ".mkv" || extension == ".png" || extension == ".jpg" || extension == ".jpeg";
}

bool link_serve_live(int64_t expires, int64_t now) { return expires > now; }

bool link_reuse_allowed(bool ready, bool running, const std::string& endpoint, bool online) {
  return ready && running && online && !endpoint.empty();
}

// A link is only reused for the lifetime it was made with: a person who asks for 24 hours must never be
// handed the 1-hour link they made before, and one who asks for 1 hour must not leave a 24-hour link open.
bool link_window_matches(int64_t created, int64_t expires, int64_t requested_seconds) {
  return expires - created == requested_seconds;
}

void link_receive_self_check() {
  assert(receive_retryable("unreachable") && receive_retryable("stalled") && receive_retryable("helper"));
  assert(!receive_retryable("declined") && !receive_retryable("expired") && !receive_retryable("cancelled"));
  assert(!receive_retryable("no_space") && !receive_retryable("not_a_clip") && !receive_retryable("too_large"));
  assert(!receive_retryable("refused") && !receive_retryable("io"));
  assert(receive_backoff_ms(1) < receive_backoff_ms(2) && receive_backoff_ms(2) < receive_backoff_ms(3) &&
         receive_backoff_ms(3) < receive_backoff_ms(4) && receive_backoff_ms(4) < receive_backoff_ms(5));
  assert(receive_watchdog("connecting", 1000, 1000 + kConnectLimitMs).empty());
  assert(!receive_watchdog("connecting", 1000, 1000 + kConnectLimitMs + 1).empty());
  assert(receive_watchdog("receiving", 1000, 1000 + kStallLimitMs).empty());
  assert(!receive_watchdog("receiving", 1000, 1000 + kStallLimitMs + 1).empty());
  assert(receive_watchdog("asking", 1000, 1000 + 10 * kStallLimitMs).empty());
  {
    // A progress line with no new bytes must not push the stall clock, and the count never goes back.
    int64_t held = 1000;
    assert(!receive_progress_advanced(held, 1000) && held == 1000);
    assert(!receive_progress_advanced(held, 999) && held == 1000);
    assert(receive_progress_advanced(held, 1001) && held == 1001);
    // With the clock unmoved, the watchdog ends the receive exactly one stall limit later.
    const uint64_t clock = 5000;
    assert(receive_watchdog("receiving", clock, clock + kStallLimitMs).empty());
    assert(!receive_watchdog("receiving", clock, clock + kStallLimitMs + 1).empty());
  }
  assert(receive_name_is_safe("Clip 2026-10-29 12-00-00.mp4"));
  assert(receive_name_is_safe("Shared clip.png"));
  assert(!receive_name_is_safe("..\\evil.mp4") && !receive_name_is_safe("clip.mp4:ads") && !receive_name_is_safe("clip.exe"));
  assert(!receive_name_is_safe("") && !receive_name_is_safe(".mp4") && !receive_name_is_safe("clip"));
  assert(link_serve_live(1000, 999) && !link_serve_live(1000, 1000) && !link_serve_live(1000, 1001));
  assert(link_reuse_allowed(true, true, "ab", true) && !link_reuse_allowed(false, true, "ab", true));
  assert(!link_reuse_allowed(true, false, "ab", true) && !link_reuse_allowed(true, true, "", true));
  assert(!link_reuse_allowed(true, true, "ab", false));
  // A link is reused only for the lifetime it was made with: 1 h and 24 h never stand in for each other.
  assert(link_window_matches(1000, 1000 + 3600, 3600));
  assert(!link_window_matches(1000, 1000 + 3600, 24 * 3600));
  assert(!link_window_matches(1000, 1000 + 24 * 3600, 3600));
}

LinkService::LinkService(std::string data_root, std::string helper_exe)
    : data_root_(std::move(data_root)), helper_exe_(std::move(helper_exe)) {
  link_receive_self_check();
  load();
  ticker_ = std::thread([this] {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
      ticker_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; });
      if (stopping_) break;
      lock.unlock();
      tick();
      lock.lock();
    }
  });
}

LinkService::~LinkService() { shutdown(); }

// ---------------------------------------------------------------- persistence

std::string seal_short_secret(const std::string& value) {
  if (value.empty()) return std::string();
  const std::string sealed = protect_secret(value);
  if (sealed.empty()) return std::string();
  return std::string(kShortSecretPrefix) + sealed;
}

ShortSecret open_short_secret(const std::string& stored, bool fragment, std::string& value) {
  const ShortSecret kind = classify_short_secret(stored, fragment);
  if (kind == ShortSecret::Legacy) {
    // Sealed by the next write of links.json; the value works until then.
    value = stored;
    return kind;
  }
  if (kind != ShortSecret::Sealed) return kind;
  std::string opened;
  // A sealed text that does not open is not a credential: it is what a copied links.json holds, and it
  // must never be read back as the plaintext it resembles.
  if (!unprotect_secret(sealed_short_payload(stored), opened) || !legacy_short_secret_ok(opened, fragment)) {
    return ShortSecret::Broken;
  }
  value = opened;
  return ShortSecret::Sealed;
}

namespace {

// One of the two short-link fields of an entry links.json already held. The plaintext an older build wrote
// is sealed here, like every other entry's; anything else - a value sealed to an account this PC is not, or
// a value that was never one of these credentials - is left exactly as it was read, because it is not this
// PC's to rewrite and overwriting it would erase someone else's link.
bool seal_carried_field(Json& item, const char* field, bool fragment) {
  const std::string stored = item[field].as_string();
  if (classify_short_secret(stored, fragment) != ShortSecret::Legacy) return true;
  const std::string sealed = seal_short_secret(stored);
  if (sealed.empty()) return false;
  item.set(field, Json(sealed));
  return true;
}

}  // namespace

void LinkService::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  // A part file left by an engine that was killed: a receive lives in memory only, so nothing in
  // incoming can be resumed after a restart, and a dot-part can be gigabytes. The posters go with them
  // (a frame of a friend's clip, or of this PC's own, that a killed engine never got to delete). Only the
  // *.part and *.jpg files directly in that one folder are removed; nothing else there is touched.
  const std::string incoming = data_root_ + "\\incoming";
  WIN32_FIND_DATAA found{};
  for (const char* pattern : {"\\*.part", "\\*.jpg"}) {
  HANDLE search = FindFirstFileA((incoming + pattern).c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      const std::string stale = incoming + "\\" + found.cFileName;
      if (DeleteFileA(stale.c_str())) {
        BX_LOG_NOTE("removed a part file an earlier run left behind: %s", found.cFileName);
      } else {
        BX_LOG_WARN("a part file an earlier run left behind could not be removed: %s", found.cFileName);
      }
    } while (FindNextFileA(search, &found));
    FindClose(search);
  }
  }
  Json stored;
  std::string error;
  const std::string text = read_text(data_root_ + "\\links.json");
  // A file an older build wrote holds the short link and its ticket in the clear. Opening one is not a
  // failure: it works as it is, and one write below seals every entry that is kept.
  bool migrated = false;
  if (!text.empty() && Json::parse(text, stored, error)) {
    std::string secret;
    if (unprotect_secret(stored["key"].as_string(), secret) && secret.size() == 64) secret_ = secret;
    endpoint_ = stored["endpoint"].as_string();
    identity_ = endpoint_;
    relay_ = stored["relay"].as_string();
    const int64_t now = now_unix();
    carried_.clear();
    for (const Json& item : stored["links"].items()) {
      Link link;
      link.id = item["id"].as_string();
      link.file = item["file"].as_string();
      link.name = item["name"].as_string();
      link.size = item["size"].as_int(0);
      link.created = item["created"].as_int(0);
      link.expires = item["expires"].as_int(0);
      link.viewers = static_cast<int>(item["viewers"].as_int(0));
      std::string token;
      const bool usable =
          unprotect_secret(item["token"].as_string(), token) && token.size() == 32 && link.id.size() == 32;
      if (usable) link.token = token;
      // Both fields are bearer credentials, sealed like the token. A plaintext one an older build wrote
      // still opens and is sealed by the write below. One this PC cannot open - a copied links.json, a
      // profile that changed - is not a credential here, and it is carried back into the file as it was
      // instead of being overwritten with nothing.
      const ShortSecret url_kind = open_short_secret(item["short_url"].as_string(), false, link.short_url);
      const ShortSecret fragment_kind = open_short_secret(item["short_fragment"].as_string(), true, link.short_fragment);
      if (url_kind == ShortSecret::Legacy || fragment_kind == ShortSecret::Legacy) migrated = true;
      if (url_kind == ShortSecret::Broken) link.kept_short_url = item["short_url"].as_string();
      if (fragment_kind == ShortSecret::Broken) link.kept_short_fragment = item["short_fragment"].as_string();
      // An expired link is never served again, so it is dropped: nothing can bring it back.
      if (link.expires <= now) continue;
      // A link that is still live but cannot be served right now - its file sits on a drive that is not
      // plugged in, or its token will not open on this account - is not active in memory and is not
      // erased either: it goes back into links.json unchanged, ready for the next start.
      if (!usable || file_size(link.file) != link.size || !library_owns_path(link.file)) {
        carried_.push_back(item);
        continue;
      }
      links_[link.id] = link;
    }
  }
  if (secret_.empty()) {
    if (!stored["key"].as_string().empty()) {
      // The file holds an identity that would not open. Making a new one here would seal over it and
      // rewrite every link's token, so the file is left as it is and sharing stays off this run.
      BX_LOG_WARN("the sharing identity in links.json could not be opened; the file is left as it was");
    } else {
      // The helper's identity, made once. Links carry its public half; the secret never leaves this PC.
      secret_ = random_hex(32);
      if (secret_.empty()) {
        BX_LOG_WARN("no sharing identity could be made; links are off this run");
      } else {
        endpoint_.clear();
        identity_.clear();
        save_locked();
      }
    }
  }
  // A short link and its ticket written in the clear are replaced once, so the credentials stop sitting
  // in the file. An identity that did not open (above) is never rewritten, so a copied links.json keeps
  // what it has rather than being sealed over on another account; a seal that fails leaves the file as
  // it was, exactly as it does for a token.
  if (migrated && !secret_.empty()) save_locked();
}

// An id that is gone is gone from both lists. A links.json entry this run could not serve is carried, so a
// duplicate with the same id sits there while the link is live (every write leaves it out then) and must
// leave with it: written back, it would put the stopped, expired or helper-ended link in the file again and
// the next start would serve it. One id is one capability, so every carried entry with it goes and nothing
// else does.
void LinkService::drop_link_locked(const std::string& id) {
  links_.erase(id);
  carried_.erase(std::remove_if(carried_.begin(), carried_.end(),
                                [&id](const Json& item) { return item["id"].as_string() == id; }),
                 carried_.end());
}

void LinkService::save_locked() {
  const std::string sealed_key = protect_secret(secret_);
  // An empty seal would write a blank key and blank tokens: every link would be lost and this PC's
  // identity would change. The previous file stands until the seal works again.
  if (sealed_key.empty()) {
    BX_LOG_WARN("the sharing identity could not be sealed; links.json is left as it was");
    return;
  }
  Json stored = Json::object();
  stored.set("key", Json(sealed_key));
  stored.set("endpoint", Json(identity_));
  stored.set("relay", Json(relay_));
  Json links = Json::array();
  for (const auto& [id, link] : links_) {
    const std::string sealed_token = protect_secret(link.token);
    if (sealed_token.empty()) {
      BX_LOG_WARN("a link token could not be sealed; links.json is left as it was");
      return;
    }
    Json item = Json::object();
    item.set("id", Json(link.id));
    item.set("token", Json(sealed_token));
    item.set("file", Json(link.file));
    item.set("name", Json(link.name));
    item.set("size", Json(link.size));
    item.set("created", Json(link.created));
    item.set("expires", Json(link.expires));
    item.set("viewers", Json(link.viewers));
    // Kept across a restart: a resolved short link is the one the sheet, the clipboard and the rail show,
    // and losing it would hand out the long link again for a link that is still live. A short link is a
    // bearer capability - whoever holds it can fetch the clip - and the ticket beside it is the proof the
    // service drops the code by, so both are sealed like the token. A field this PC could not open is
    // written back exactly as it was read. A seal that fails writes nothing: the previous file stands.
    if (!link.kept_short_url.empty()) {
      item.set("short_url", Json(link.kept_short_url));
    } else if (!link.short_url.empty()) {
      const std::string sealed_url = seal_short_secret(link.short_url);
      if (sealed_url.empty()) {
        BX_LOG_WARN("a short link could not be sealed; links.json is left as it was");
        return;
      }
      item.set("short_url", Json(sealed_url));
    }
    if (!link.kept_short_fragment.empty()) {
      item.set("short_fragment", Json(link.kept_short_fragment));
    } else if (!link.short_fragment.empty()) {
      const std::string sealed_fragment = seal_short_secret(link.short_fragment);
      if (sealed_fragment.empty()) {
        BX_LOG_WARN("a short link's ticket could not be sealed; links.json is left as it was");
        return;
      }
      item.set("short_fragment", Json(sealed_fragment));
    }
    links.push(item);
  }
  // The entries this run did not bring back are written as they were: sealing one entry must never erase
  // another. One whose id an active link already has is left out, so a link the user stopped cannot come
  // back under the same id on the next start.
  for (Json& item : carried_) {
    if (links_.count(item["id"].as_string()) != 0) continue;
    if (!seal_carried_field(item, "short_url", false) || !seal_carried_field(item, "short_fragment", true)) {
      BX_LOG_WARN("a short link could not be sealed; links.json is left as it was");
      return;
    }
    links.push(item);
  }
  stored.set("links", links);
  if (!write_file_atomic(data_root_ + "\\links.json", stored.dump(2))) BX_LOG_WARN("links.json could not be written");
}

// ---------------------------------------------------------------- the helper process

int LinkService::upload_kbps_locked() const {
  // 0 is Unlimited: nothing to pace, and nothing to lower while a game runs. Any other value is a
  // ceiling the person chose, and a game in focus lowers it further.
  if (upload_kbps_ <= 0) return 0;
  return gaming_ ? std::min(upload_kbps_, kGamingKbps) : upload_kbps_;
}

bool LinkService::send_locked(const Json& command) {
  if (!stdin_) return false;
  const std::string line = command.dump() + "\n";
  DWORD written = 0;
  return WriteFile(static_cast<HANDLE>(stdin_), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
         written == line.size();
}

bool LinkService::serve_locked(const Link& link) {
  Json command = Json::object();
  command.set("op", Json("serve"));
  command.set("share", Json(link.id));
  command.set("token", Json(link.token));
  command.set("path", Json(link.file));
  command.set("name", Json(link.name));
  command.set("size", Json(link.size));
  command.set("expires", Json(link.expires));
  if (!link.poster.empty()) command.set("poster", Json(link.poster));
  command.set("max_active", Json(kMaxWatching));
  command.set("max_total", Json(std::max(0, kMaxViewers - link.viewers)));
  return send_locked(command);
}

bool LinkService::helper_dead() {
  std::lock_guard<std::mutex> lock(mutex_);
  return process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0;
}

bool LinkService::ensure_helper(std::string& error) {
  // A helper that ended by itself (it crashed, or Windows ended it) is cleaned up before a new one.
  if (helper_dead()) stop_helper();
  std::unique_lock<std::mutex> lock(mutex_);
  // A stop in progress is joined outside this lock; wait for it instead of racing its reader thread.
  if (helper_stopping_ && !ready_cv_.wait_for(lock, std::chrono::seconds(6), [this] { return !helper_stopping_ || stopping_; })) {
    error = "the sharing helper is restarting - try again";
    return false;
  }
  if (stopping_) {
    error = "the engine is shutting down";
    return false;
  }
  if (!process_ && !start_helper_locked(error)) return false;
  if (!ready_cv_.wait_for(lock, std::chrono::seconds(kHelperReadySeconds), [this] { return ready_ || !process_ || stopping_; }) || !ready_) {
    if (stopping_) {
      error = "the engine is shutting down";
      return false;
    }
    error = "BuffrX could not reach the sharing network - check the internet connection";
    return false;
  }
  return true;
}

bool LinkService::start_helper_locked(std::string& error) {
  if (helper_stopping_ || reader_.joinable()) {
    error = "the sharing helper is restarting - try again";
    return false;
  }
  last_start_ = GetTickCount64();
  idle_.reset();  // a new helper is idle for its first seconds; the old one's clock must not stop it
  {
    if (GetFileAttributesA(helper_exe_.c_str()) == INVALID_FILE_ATTRIBUTES) {
      error = "the sharing helper is missing from this install";
      return false;
    }
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &inherit, 0)) {
      error = "the sharing helper could not be started";
      return false;
    }
    if (!CreatePipe(&out_read, &out_write, &inherit, 0)) {
      // The first pair was opened and nothing will use it: both handles go back.
      CloseHandle(in_read);
      CloseHandle(in_write);
      error = "the sharing helper could not be started";
      return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in_read;
    startup.hStdOutput = out_write;
    startup.hStdError = nul;
    std::string command = "\"" + helper_exe_ + "\"";
    PROCESS_INFORMATION process{};
    // Suspended until it is in the job: killed with the engine, whatever way the engine ends.
    const BOOL started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                       nullptr, &startup, &process);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
      CloseHandle(in_write);
      CloseHandle(out_read);
      error = "the sharing helper could not be started";
      return false;
    }
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
      AssignProcessToJobObject(job, process.hProcess);
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    job_ = job;
    stdin_ = in_write;
    ready_ = false;
    reader_done_ = false;
    out_read_ = out_read;
    reader_ = std::thread([this, out_read] { reader_loop(out_read); });
    Json init = Json::object();
    init.set("op", Json("init"));
    init.set("secret", Json(secret_));
    init.set("rate_kbps", Json(upload_kbps_locked()));
    const std::string proxy = windows_proxy();
    init.set("proxy", Json(proxy));
    send_locked(init);
    online_ = false;
    BX_LOG_NOTE("sharing helper started%s", proxy.empty() ? "" : " (through the system proxy)");
  }
  return true;
}

void LinkService::stop_helper() {
  HANDLE process = nullptr, job = nullptr, out_read = nullptr;
  std::thread reader;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Every download was this helper's: no count it left behind is still true, and a stuck one would
    // keep an expired link in the list and in the LIVE LINK pane.
    for (auto& [id, link] : links_) link.watching = 0;
    if (helper_stopping_ || (!process_ && !reader_.joinable())) return;
    helper_stopping_ = true;
    // Closing its stdin is the helper's signal to close its connections and end.
    if (stdin_) CloseHandle(static_cast<HANDLE>(stdin_));
    stdin_ = nullptr;
    process = static_cast<HANDLE>(process_);
    job = static_cast<HANDLE>(job_);
    process_ = nullptr;
    job_ = nullptr;
    ready_ = false;
    // The endpoint was this helper's to carry: a link handed out from it would lead nowhere.
    endpoint_.clear();
    online_ = false;
    idle_.reset();
    // The reader is joined after the lock is let go: joining under it deadlocks against on_event.
    reader = std::move(reader_);
    out_read = static_cast<HANDLE>(out_read_);
    out_read_ = nullptr;
  }
  if (process) {
    if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) TerminateProcess(process, 1);
    CloseHandle(process);
  }
  if (job) CloseHandle(job);
  if (reader.joinable()) {
    // The pipe only ends when every copy of its write end is closed, and a child libobs started in the same
    // instant (a muxer, which runs for a whole Timeline session) can have inherited one. The helper is gone by
    // now, so after a second the read on this one handle is cancelled instead of waited out behind that other
    // process. Only the pipe's read: the reader may be filing a clip that just arrived, and its file work is
    // never cancelled, only waited for.
    for (int waited = 0; !reader_done_; waited += 50) {
      if (waited >= 1000 && out_read) CancelIoEx(out_read, nullptr);
      Sleep(50);
    }
    reader.join();
  }
  if (out_read) CloseHandle(out_read);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    helper_stopping_ = false;
  }
  ready_cv_.notify_all();
  BX_LOG_NOTE("sharing helper stopped");
}

void LinkService::reader_loop(void* read_pipe) {
  HANDLE pipe = static_cast<HANDLE>(read_pipe);  // closed by stop_helper, after this thread is joined
  std::string pending;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    pending.append(buffer, read);
    if (pending.size() > 1024 * 1024) pending.clear();  // a line this long is not one the helper writes
    size_t newline = 0;
    while ((newline = pending.find('\n')) != std::string::npos) {
      const std::string line = pending.substr(0, newline);
      pending.erase(0, newline + 1);
      Json event;
      std::string error;
      if (Json::parse(line, event, error)) on_event(event);
    }
  }
  // The helper is gone: whatever was arriving stops with it.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = false;
    // Its downloads ended with it: a count left behind would keep an expired link listed for good.
    for (auto& [id, link] : links_) link.watching = 0;
    for (auto& [id, receive] : receives_) {
      if (receive.state == "connecting" || receive.state == "receiving" || receive.state == "asking") {
        receive_failed_locked(receive, "helper");
      }
    }
  }
  ready_cv_.notify_all();
  publish();
  reader_done_ = true;
}

void LinkService::on_event(const Json& event) {
  const std::string kind = event["ev"].as_string();
  if (kind == "log") {
    BX_LOG_NOTE("sharing: %s", event["msg"].as_string().c_str());
    return;
  }
  if (kind == "error") {
    BX_LOG_WARN("sharing helper: %s", event["msg"].as_string().c_str());
    return;
  }
  std::string done_id, done_via;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (kind == "ready") {
      const std::string endpoint = event["endpoint"].as_string();
      if (!endpoint_.empty() && endpoint != endpoint_) BX_LOG_WARN("the sharing identity changed; older links stop working");
      endpoint_ = endpoint;
      identity_ = endpoint;
      if (!event["relay"].as_string().empty()) relay_ = event["relay"].as_string();
      // An older helper said nothing about it; it would not have said ready without a relay.
      online_ = event["online"].is_bool() ? event["online"].as_bool() : true;
      ready_ = true;
      // Every live link is offered again: after a restart the same links keep working. An expired one
      // is not: the helper would carry it past the expiry the sharer chose.
      const int64_t now = now_unix();
      for (const auto& [id, link] : links_) {
        if (!link_serve_live(link.expires, now)) continue;
        if (!serve_locked(link)) {
          // on_event holds the lock and runs on the reader thread: the stop belongs to the tick, which
          // also starts the helper again for the links still live.
          BX_LOG_WARN("link %s..: the helper did not take it; the helper is restarted on the next tick",
                      link.id.substr(0, 6).c_str());
          restart_pending_ = true;
          break;
        }
      }
      save_locked();
      BX_LOG_NOTE("sharing helper ready (relay %s%s)", relay_.c_str(), online_ ? "" : ", not reached yet");
    } else if (kind == "online") {
      if (!event["relay"].as_string().empty()) relay_ = event["relay"].as_string();
      online_ = true;
      save_locked();
      BX_LOG_NOTE("sharing helper reached its relay (%s)", relay_.c_str());
    } else if (kind == "viewer") {
      const auto found = links_.find(event["share"].as_string());
      if (found != links_.end()) {
        const std::string state = event["state"].as_string();
        // The route the transfer took: the share sheet says a relay is why a send is slow.
        const std::string via = event["via"].as_string();
        if (via == "direct" || via == "relay") found->second.via = via;
        if (state == "start") {
          ++found->second.watching;
        } else {
          found->second.watching = std::max(0, found->second.watching - 1);
          if (state == "done") ++found->second.viewers;
          BX_LOG_NOTE("link %s..: a download %s at byte %lld", found->first.substr(0, 6).c_str(),
                      state == "done" ? "finished" : "stopped", static_cast<long long>(event["bytes"].as_int(0)));
          save_locked();
        }
      }
    } else if (kind == "expired") {
      // The helper ends it, and this PC's own expiry ends it the same way: one removal, so a carried
      // duplicate with this id cannot be written back and revived on the next start.
      drop_link_locked(event["share"].as_string());
      save_locked();
    } else {
      const std::string helper_id = event["id"].as_string();
      const auto found = std::find_if(receives_.begin(), receives_.end(), [&](const auto& entry) {
        return entry.second.helper_id == helper_id;
      });
      if (found == receives_.end()) return;
      Receive& receive = found->second;
      if (receive.state == "retrying" || receive.state == "cancelled" || receive.state == "failed" || receive.state == "done") return;
      if (kind == "meta") {
        receive.name = received_file_name(event["name"].as_string());
        receive.size = event["size"].as_int(0);
        receive.tag = event["tag"].as_string();
        // The route is known as soon as the stream is open, so a slow transfer can say why at once.
        const std::string via = event["via"].as_string();
        if (via == "direct" || via == "relay") receive.via = via;
        receive.bytes = event["offset"].as_int(0);
        if (receive.bytes > 0) {
          BX_LOG_NOTE("link receive %s: resuming at byte %lld", receive.id.c_str(), static_cast<long long>(receive.bytes));
        }
        receive.progress = GetTickCount64();
        // The poster rode in the header, so it is here before the first video byte: the sheet shows what
        // is coming while the bytes are still on their way.
        const std::string poster = event["poster"].as_string();
        if (!poster.empty() && receive.thumbnail.empty()) {
          std::vector<uint8_t> bytes;
          if (base64url_decode(poster, bytes) && bytes.size() > 128 && bytes.size() <= 40000) {
            const std::string shot = data_root_ + "\\incoming\\" + receive.id + ".jpg";
            HANDLE handle = CreateFileA(shot.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
              DWORD written = 0;
              if (WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size()) {
                receive.thumbnail = shot;
              }
              CloseHandle(handle);
            }
          }
        }
        ULARGE_INTEGER free{};
        // The space this needs is the disk the part file lands on, which is not the clips folder's: clips
        // may sit on D: while the data root (and so incoming) is on C:.
        const size_t slash = receive.part.find_last_of("\\/");
        const std::string part_dir = slash == std::string::npos ? receive.part : receive.part.substr(0, slash);
        if (receive.name.empty()) {
          receive_failed_locked(receive, "not_a_clip");
        } else if (GetDiskFreeSpaceExA(part_dir.c_str(), &free, nullptr, nullptr) &&
                   static_cast<int64_t>(free.QuadPart) < receive.size + kSpareSpace) {
          receive_failed_locked(receive, "no_space");
        } else if (GetDiskFreeSpaceExA(receive.clips_dir.c_str(), &free, nullptr, nullptr) &&
                   static_cast<int64_t>(free.QuadPart) < receive.size + kSpareSpace) {
          // Where it is filed needs the room too, when that is another disk.
          receive_failed_locked(receive, "no_space");
        } else {
          receive.state = receive.size > kAskAbove && !receive.accepted ? "asking" : "receiving";
        }
      } else if (kind == "progress") {
        const int64_t count = event["bytes"].as_int(0);
        // Only new bytes move the stall clock, so a progress line cannot keep a dead wire alive.
        if (receive_clock_moves(receive.bytes, count, event["checking"].as_bool())) receive.progress = GetTickCount64();
      } else if (kind == "done") {
        receive.bytes = event["bytes"].as_int(0);
        // The frame came from the sender's clip and has done its job: the clip itself is here now, so
        // nothing of the preview stays on this disk.
        if (!receive.thumbnail.empty()) {
          DeleteFileA(receive.thumbnail.c_str());
          receive.thumbnail.clear();
        }
        done_id = receive.id;
        done_via = event["path"].as_string();
      } else if (kind == "failed") {
        const std::string reason = event["reason"].as_string();
        if (reason == "cancelled" || reason == "declined") {
          if (receive.state != "failed" && receive.state != "done") {
            receive.state = "cancelled";
            receive.retry_at = 0;
          }
          DeleteFileA(receive.part.c_str());
        } else {
          receive_failed_locked(receive, reason);
        }
      }
    }
  }
  if (kind == "ready") ready_cv_.notify_all();
  if (!done_id.empty()) finish_receive(done_id, done_via);
  publish();
}

void LinkService::receive_failed_locked(Receive& receive, const std::string& reason) {
  if (receive.state == "failed" || receive.state == "cancelled" || receive.state == "done") return;
  // The poster came from the sender's clip: a receive that ends without it ends without that too.
  if (!receive.thumbnail.empty()) {
    DeleteFileA(receive.thumbnail.c_str());
    receive.thumbnail.clear();
  }
  // Whatever the helper was doing with this one stops here, and its half file goes with it: nothing
  // partial is ever left in incoming, and the original (a file already in the library) is never touched.
  if (receive.state != "retrying") {
    Json command = Json::object();
    command.set("op", Json("cancel"));
    command.set("id", Json(receive.helper_id));
    send_locked(command);
  }
  ++receive.tries;
  // The tag comes with the sharer's first answer: a receive without one never got through.
  const int tries = receive_tries(!receive.tag.empty());
  const bool retry = receive_retryable(reason) && receive.tries < tries && !link_expired_here(receive.expires, now_unix());
  if (retry) {
    // The link may still deliver it: another fetch in a moment, with the sheet saying so. The part
    // file stays, and the next fetch asks only for what is missing.
    receive.state = "retrying";
    receive.error.clear();
    receive.retry_at = GetTickCount64() + receive_backoff_ms(receive.tries);
    BX_LOG_WARN("link receive %s: %s - trying again (attempt %d of %d)", receive.id.c_str(), reason.c_str(),
                receive.tries + 1, tries);
    return;
  }
  DeleteFileA(receive.part.c_str());
  receive.bytes = 0;
  receive.state = "failed";
  receive.error = reason;
  BX_LOG_WARN("link receive %s failed: %s", receive.id.c_str(), reason.c_str());
}

// The list of finished downloads is a short history, not a log: the oldest leaves when it is long. Called
// wherever an entry finishes, so links that keep failing (an expired one clicked again and again) cannot
// grow it without bound.
void LinkService::trim_finished_locked() {
  int finished = 0;
  for (const auto& [id, receive] : receives_) {
    const std::string& state = receive.state;
    if (state == "done" || state == "failed" || state == "cancelled") ++finished;
  }
  for (auto it = receives_.begin(); it != receives_.end() && finished > kMaxFinishedReceives;) {
    const std::string& state = it->second.state;
    if (state == "done" || state == "failed" || state == "cancelled") {
      it = receives_.erase(it);
      --finished;
    } else {
      ++it;
    }
  }
}

// A clip has arrived: its size and its container are checked first, and only then is it moved into the
// library under its sanitised name. A file that fails either check is deleted, never filed.
void LinkService::finish_receive(const std::string& id, const std::string& via) {
  std::string part, name, clips_dir;
  int64_t announced = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = receives_.find(id);
    if (found == receives_.end()) return;
    if (found->second.state == "cancelled") {
      DeleteFileA(found->second.part.c_str());
      return;
    }
    // A "done" that arrives after the user cancelled, or while a retry is fetching a new part, is
    // stale: filing it would take a file the receive no longer owns. A download nobody asked to keep
    // (still asking or connecting) leaves its part behind unless it is removed here.
    if (found->second.state == "asking" || found->second.state == "connecting") {
      DeleteFileA(found->second.part.c_str());
      return;
    }
    if (found->second.state != "receiving") return;
    part = found->second.part;
    name = found->second.name;
    clips_dir = found->second.clips_dir;
    announced = found->second.size;
  }
  // "Exactly what was announced": a stream that ended early is not a clip, however well it opened.
  const int64_t arrived = file_size(part);
  std::string failure;
  if (arrived < 0 || (announced > 0 && arrived != announced)) {
    failure = "incomplete";
  } else if (!receive_name_is_safe(name)) {
    failure = "not_a_clip";
  } else if (is_video(name) && !check_clip_file(part).ok) {
    failure = "not_a_clip";
  } else if (is_image(name)) {
    // A picture must begin as the picture its name says it is: nothing else is filed under that name.
    uint8_t head[8] = {0};
    DWORD read = 0;
    HANDLE handle = CreateFileA(part.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      ReadFile(handle, head, sizeof(head), &read, nullptr);
      CloseHandle(handle);
    }
    if (!received_image_ok(name, head, read)) failure = "not_a_clip";
  }
  std::string target;
  std::string base;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = receives_.find(id);
    if (found == receives_.end() || found->second.state != "receiving" || found->second.part != part) {
      DeleteFileA(part.c_str());
      return;
    }
    base = is_image(name) ? found->second.screenshots_dir : clips_dir;
    // The move below can be a copy to another disk, seconds long for a large clip: it runs without the lock
    // (every link.list waits on it), and the stall watchdog leaves this receive alone meanwhile.
    found->second.progress = 0;
  }
  if (failure.empty()) {
    // Keep the sender provenance in the folder, including after a rename or restart.
    const std::string folder = base + "\\Shared with me";
    CreateDirectoryA(base.c_str(), nullptr);
    CreateDirectoryA(folder.c_str(), nullptr);
    const size_t dot = name.find_last_of('.');
    // reserve_output_path never returns a name that is taken, and nothing here replaces a file. The
    // check and the move are two steps though: two friends sending the same name at once can reserve
    // the same free name, and the second move then finds it taken. One fresh name settles that; a
    // second failure is a real one and keeps the plain "io".
    const std::string stem = name.substr(0, dot);
    const std::string suffix = name.substr(dot);
    for (int attempt = 0; attempt < 2 && target.empty(); ++attempt) {
      target = reserve_output_path(folder, stem, suffix);
      if (target.empty()) break;
      if (!MoveFileExA(part.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) target.clear();
    }
    if (target.empty()) failure = "io";
  }
  if (failure.empty() || !receive_retryable(failure)) DeleteFileA(part.c_str());  // a retryable part is kept for the resume
  std::unique_lock<std::mutex> lock(mutex_);
  const auto found = receives_.find(id);
  if (found == receives_.end() || found->second.state != "receiving" || found->second.part != part) {
    // Cancelled while the file was being moved: it is not kept.
    if (!target.empty()) DeleteFileA(target.c_str());
    DeleteFileA(part.c_str());
    return;
  }
  {
    if (!failure.empty()) {
      receive_failed_locked(found->second, failure);
    } else {
      found->second.state = "done";
      found->second.error.clear();
      found->second.path = target;
      found->second.via = via;
      found->second.url.clear();  // no longer needed: nothing left to retry
    }
  }
  trim_finished_locked();
  lock.unlock();
  if (!failure.empty()) {
    BX_LOG_WARN("link receive %s could not be filed: %s", id.c_str(), failure.c_str());
    publish();
    return;
  }
  BX_LOG_NOTE("link receive %s arrived %s (%s)", id.c_str(), via.c_str(), base_name(target).c_str());
  Json data = Json::object();
  data.set("id", Json(id));
  data.set("file", Json(target));
  if (emit) {
    emit("library.changed", Json::object());
    emit("link.received", data);
  }
}

// ---------------------------------------------------------------- state for the interface

std::string LinkService::url_for_locked(const Link& link) {
  LinkTicket ticket;
  if (!hex_decode(identity_, ticket.endpoint, 32) || !hex_decode(link.id, ticket.share, 16) ||
      !hex_decode(link.token, ticket.token, 16)) {
    return std::string();
  }
  ticket.expires = static_cast<uint32_t>(link.expires);
  // The relay's address goes in too (never an IP). iroh can find this PC's relay by its key through
  // n0's DNS lookup, but that is one more service that must be up and reachable from the friend's
  // network; with the address in the link, the friend's BuffrX dials the relay straight away and the
  // lookup runs beside it, so either one is enough. It makes a link about a third longer.
  if (relay_.size() <= 200 && relay_.rfind("https://", 0) == 0) ticket.relay = relay_;
  return link_url(page_, ticket, sender_name_);
}

Json LinkService::state_json_locked() {
  Json links = Json::array();
  for (const auto& [id, link] : links_) {
    Json item = Json::object();
    item.set("id", Json(link.id));
    item.set("file", Json(link.file));
    item.set("name", Json(link.name));
    item.set("created", Json(link.created));
    item.set("expires", Json(link.expires));
    item.set("viewers", Json(link.viewers));
    item.set("watching", Json(link.watching));
    if (!link.via.empty()) item.set("via", Json(link.via));
    item.set("url", Json(link.short_url.empty() ? url_for_locked(link) : link.short_url));
    links.push(item);
  }
  Json receives = Json::array();
  for (const auto& [id, receive] : receives_) {
    Json item = Json::object();
    item.set("id", Json(receive.id));
    item.set("state", Json(receive.state));
    item.set("name", Json(receive.name));
    item.set("size", Json(receive.size));
    item.set("bytes", Json(receive.bytes));
    if (!receive.error.empty()) item.set("error", Json(receive.error));
    if (!receive.path.empty()) item.set("file", Json(receive.path));
    if (!receive.via.empty()) item.set("via", Json(receive.via));
    if (!receive.thumbnail.empty()) item.set("thumbnail", Json(receive.thumbnail));
    if (receive.state == "retrying") {
      const int tries = receive_tries(!receive.tag.empty());
      item.set("attempt", Json(std::min(receive.tries + 1, tries)));
      item.set("tries", Json(tries));
    }
    // The ticket comes back only while a retry is the user's to ask for, and never in a log line.
    if (receive.state == "failed" && !receive.url.empty()) item.set("url", Json(receive.url));
    receives.push(item);
  }
  Json state = Json::object();
  state.set("links", links);
  state.set("receives", receives);
  // How long ago the last pass of the tick ran. A stalled receive with a stale number here means the
  // timer stopped, not the wire; a fresh one means the transfer itself is what stopped.
  if (last_tick_ != 0) state.set("tick_age_ms", Json(static_cast<int64_t>(GetTickCount64() - last_tick_)));
  state.set("page_set", Json(!page_.empty()));
  return state;
}

void LinkService::publish() {
  Json state;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state = state_json_locked();
  }
  if (emit) emit("link.changed", state);
}

Json LinkService::list() {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_json_locked();
}

// ---------------------------------------------------------------- commands

void LinkService::configure(const std::string& page, const std::string& sender_name, int upload_kbps) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    page_ = link_page_or_default(page);
    sender_name_ = link_sender_name(sender_name);
    const int before = upload_kbps_locked();
    upload_kbps_ = std::max(0, upload_kbps);
    if (!restart_pending_ && before != upload_kbps_locked()) {
      Json command = Json::object();
      command.set("op", Json("rate"));
      command.set("kbps", Json(upload_kbps_locked()));
      send_locked(command);
    }
  }

}

void LinkService::set_gaming(bool gaming) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (gaming == gaming_) return;
  gaming_ = gaming;
  Json command = Json::object();
  command.set("op", Json("rate"));
  command.set("kbps", Json(upload_kbps_locked()));
  send_locked(command);
}

void LinkService::set_short_url(const std::string& id, const std::string& url, const std::string& fragment) {
  if (url.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = links_.find(id);
  if (found == links_.end()) return;
  found->second.short_url = url;
  // The service just answered with a short link for this one: what links.json held before is replaced,
  // readable or not. Writing an unreadable kept value instead would lose the new link on the next start.
  found->second.kept_short_url.clear();
  if (!fragment.empty()) {
    found->second.short_fragment = fragment;
    found->second.kept_short_fragment.clear();
  }
  // Written now, not at the next change: a restart before then would show the long link again.
  save_locked();
}

bool LinkService::share(const std::string& file, int minutes, Json& reply, std::string& error) {
  if (!library_owns_path(file)) {
    error = "that file is not in the BuffrX library";
    return false;
  }
  const int64_t size = file_size(file);
  if (size <= 0 || size > kMaxBytes) {
    error = size <= 0 ? "the file is empty or gone" : "links carry files up to 2 GB - trim it in the editor first";
    return false;
  }
  minutes = std::clamp(minutes, 10, 7 * 24 * 60);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, link] : links_) {
      // A link is only reusable while the helper that serves it is ready, running and reachable: a
      // stopped or crashed helper leaves its endpoint behind and the link would go nowhere.
      if (link.file == file && link.expires > now_unix() &&
          link_window_matches(link.created, link.expires, static_cast<int64_t>(minutes) * 60) &&
          link_reuse_allowed(ready_, process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT, endpoint_,
                       online_)) {
        reply = Json::object();
        reply.set("id", Json(link.id));
        reply.set("url", Json(link.short_url.empty() ? url_for_locked(link) : link.short_url));
        reply.set("expires", Json(link.expires));
        reply.set("existing", Json(true));
        return true;
      }
    }
  }
  bool served = false;
  Link link;
  link.id = random_hex(16);
  link.token = random_hex(16);
  if (link.id.empty() || link.token.empty()) {
    // Zeros would be a guessable share id and token: no link is made from them.
    error = "the sharing key could not be made - try again";
    return false;
  }
  link.file = file;
  link.name = base_name(file);
  link.size = size;
  {
    // The receiver's sheet shows the first frame before any video byte arrives. Made once, here, and
    // carried in the header: a saved clip is a plain MP4, so its own bytes cannot be decoded until the
    // last one lands. Made before the lock is taken (the decode can wait seconds for the media lock, and
    // every link.list, the panel's own, would wait with it) and before the helper is asked for, so nothing
    // slow sits between "the helper is ready" and "the link is served".
    if (make_poster) {
      CreateDirectoryA((data_root_ + "\\incoming").c_str(), nullptr);
      const std::string shot = data_root_ + "\\incoming\\link-" + link.id + ".jpg";
      std::string ignored;
      if (make_poster(file, shot, ignored)) {
        HANDLE handle = CreateFileA(shot.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
          LARGE_INTEGER length{};
          std::vector<uint8_t> bytes;
          // 30000 bytes is 40000 base64 characters, which is where the helper cuts a poster: a larger one
          // reached the receiver as the first three quarters of a JPEG.
          if (GetFileSizeEx(handle, &length) && length.QuadPart > 0 && length.QuadPart <= 30000) {
            bytes.resize(static_cast<size_t>(length.QuadPart));
            DWORD read = 0;
            if (ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size()) {
              link.poster = base64url_encode(bytes);
            }
          }
          CloseHandle(handle);
        }
        DeleteFileA(shot.c_str());
      }
    }
    if (!ensure_helper(error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // With no relay a friend's BuffrX has nowhere to find this PC: the link would be dead on arrival.
    if (!online_) {
      error = "BuffrX could not reach the sharing network. Check your internet connection and try again.";
      return false;
    }
    const int64_t now = now_unix();
    link.created = now;
    link.expires = now + static_cast<int64_t>(minutes) * 60;
    links_[link.id] = link;
    if (!serve_locked(link)) {
      // The helper never received it: the URL would lead nowhere, so it is not handed out. This id was made
      // a few lines up (16 random bytes) and the entry was never written to links.json, so there is no
      // carried entry it could share an id with and nothing to drop but the temporary one.
      links_.erase(link.id);
      save_locked();
      error = "the sharing helper stopped answering - try again";
    } else {
      served = true;
      save_locked();
      reply = Json::object();
      reply.set("id", Json(link.id));
      reply.set("url", Json(url_for_locked(link)));
      reply.set("expires", Json(link.expires));
      BX_LOG_NOTE("link %s.. made for %s, %d min", link.id.substr(0, 6).c_str(), link.name.c_str(), minutes);
    }
  }
  publish();
  return served;
}

bool LinkService::stop(const std::string& id, std::string& error) {
  bool sent = false;
  std::string revoked;  // the ticket whose code the service should drop
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = links_.find(id);
    if (found == links_.end()) {
      error = "no such link";
      return false;
    }
    // A link that was shortened leaves a code pointing at a ticket nobody can use any more; the service is
    // told, with the ticket as proof, once the lock is gone. The ticket that was shortened is the one the
    // service stored: the link would carry a different relay after a restarted helper, and a revoke naming
    // today's ticket would drop nothing.
    if (!found->second.short_url.empty()) {
      revoked = found->second.short_fragment;
      if (revoked.empty()) revoked = link_fragment(url_for_locked(found->second));
    }
    // The entry this run could not serve is carried; left there, the next write would put it back and the
    // stopped link would return on the next start. drop_link_locked takes both.
    drop_link_locked(id);
    Json command = Json::object();
    command.set("op", Json("unserve"));
    command.set("share", Json(id));
    // With no helper running nothing is being served, and the next one is only offered the links left.
    const bool running = stdin_ != nullptr;
    sent = !running || send_locked(command);
    save_locked();
  }
  // A helper that did not take the command cannot be trusted to have stopped serving: it is ended, which
  // ends every transfer, and the tick starts a new one for the links that are still live. The other links
  // used to be deleted here, so stopping one link while the helper restarted lost all of them.
  // stop_helper takes the lock itself, so it is called with the lock released; it also joins the
  // reader thread, which is safe here because this runs on a pipe worker.
  if (!sent) stop_helper();
  publish();
  if (!revoked.empty()) revoke_short_link(revoked);
  return true;
}

bool LinkService::open(const std::string& url, const std::string& clips_dir, const std::string& screenshots_dir, Json& reply,
                       std::string& error) {
  const bool ok = open_link(url, clips_dir, screenshots_dir, reply, error);
  if (!ok) {
    // A click on a link has no reply to read: the panel shows the reason instead.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Receive receive;
      receive.id = "r" + std::to_string(GetTickCount64()) + "-" + std::to_string(++next_id_);
      receive.state = "failed";
      receive.error = error;
      receives_[receive.id] = receive;
      trim_finished_locked();
    }
    publish();
  } else if (static_cast<const Json&>(reply)["own"].as_bool() && emit) {  // const: a lookup must not add the key
    Json data = Json::object();
    data.set("id", Json(""));
    data.set("file", static_cast<const Json&>(reply)["file"]);
    emit("link.received", data);
  }
  return ok;
}

bool LinkService::open_link(const std::string& url, const std::string& clips_dir, const std::string& screenshots_dir,
                            Json& reply, std::string& error) {
  LinkTicket ticket;
  if (!parse_link_url(url, ticket)) {
    error = "that is not a BuffrX link";
    return false;
  }
  if (link_expired_here(static_cast<int64_t>(ticket.expires), now_unix())) {
    error = "This link has expired. Ask for a new one.";
    return false;
  }
  const std::string share = hex_encode(ticket.share, 16);
  const std::string peer = hex_encode(ticket.endpoint, 32);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A link of this PC's own: nothing to fetch, the clip is already here.
    if (peer == identity_) {
      const auto found = links_.find(share);
      if (found == links_.end()) {
        error = "that link was stopped";
        return false;
      }
      reply = Json::object();
      reply.set("own", Json(true));
      reply.set("file", Json(found->second.file));
      return true;
    }
    int active = 0;
    for (const auto& [id, receive] : receives_) {
      // The same link clicked or retried twice is one download, not two: it gets the id it already has.
      if (receive.peer == peer && receive.share == share && (receive.state == "connecting" ||
          receive.state == "receiving" || receive.state == "asking" || receive.state == "retrying")) {
        reply = Json::object();
        reply.set("id", Json(receive.id));
        reply.set("existing", Json(true));
        return true;
      }
      if (receive.state == "connecting" || receive.state == "receiving" || receive.state == "asking" ||
          receive.state == "retrying") {
        ++active;
      }
    }
    if (active >= kMaxReceives) {
      error = "three clips are already arriving - wait for one to finish";
      return false;
    }
  }
  if (!ensure_helper(error)) return false;
  std::string id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    id = "r" + std::to_string(GetTickCount64()) + "-" + std::to_string(++next_id_);
    const std::string incoming = data_root_ + "\\incoming";
    CreateDirectoryA(incoming.c_str(), nullptr);
    Receive receive;
    receive.id = id;
    receive.state = "connecting";
    receive.part = incoming + "\\" + id + ".part";
    receive.clips_dir = clips_dir;
    receive.screenshots_dir = screenshots_dir;
    receive.url = url;
    receive.peer = peer;
    receive.relay = ticket.relay;
    receive.share = share;
    receive.token = hex_encode(ticket.token, 16);
    receive.expires = ticket.expires;
    receives_[id] = receive;
    send_fetch_locked(receives_[id]);
  }
  reply = Json::object();
  reply.set("id", Json(id));
  publish();
  return true;
}

// Sends one fetch for this receive: the first, or a retry of the same ticket.
void LinkService::send_fetch_locked(Receive& receive) {
  if (receive.share.empty() || receive.peer.empty() || receive.part.empty()) return;
  // One part file for every attempt: a retry continues it when the sharer's file is unchanged (the tag),
  // and the helper starts it again when not. Events from an earlier attempt carry its helper id, which
  // no longer matches, so a late "done" from it is never filed.
  if (receive.tag.empty()) DeleteFileA(receive.part.c_str());
  ++receive.attempt;
  receive.helper_id = receive.id + "-" + std::to_string(receive.attempt);
  Json command = Json::object();
  command.set("op", Json("fetch"));
  command.set("id", Json(receive.helper_id));
  command.set("endpoint", Json(receive.peer));
  command.set("relay", Json(receive.relay));
  command.set("share", Json(receive.share));
  command.set("token", Json(receive.token));
  command.set("dest", Json(receive.part));
  command.set("max_bytes", Json(kMaxBytes));
  command.set("auto_bytes", Json(receive.accepted ? kMaxBytes : kAskAbove));
  command.set("resume_tag", Json(receive.tag));
  const uint64_t now = GetTickCount64();
  receive.progress = now;
  receive.retry_at = 0;
  receive.error.clear();
  receive.state = "connecting";
  send_locked(command);
}

bool LinkService::answer(const std::string& id, bool accept, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = receives_.find(id);
    if (found == receives_.end()) {
      error = "no such download";
      return false;
    }
    Receive& receive = found->second;
    // Answered already (a second press, or a repeat from two windows): nothing more to say.
    if (receive.state == "receiving" || receive.state == "done") return true;
    if (receive.state != "asking") {
      error = receive.state == "cancelled" ? "that download was cancelled" : "that download is over";
      return false;
    }
    receive.state = accept ? "receiving" : "cancelled";
    receive.accepted = accept;
    receive.progress = GetTickCount64();
    if (!accept) DeleteFileA(receive.part.c_str());
    Json command = Json::object();
    command.set("op", Json("answer"));
    command.set("id", Json(receive.helper_id));
    command.set("accept", Json(accept));
    send_locked(command);
  }
  publish();
  return true;
}

bool LinkService::cancel(const std::string& id, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    const auto found = receives_.find(id);
    // Cancelling something that is already gone is the no-op the second press deserves.
    if (found == receives_.end()) return true;
    Receive& receive = found->second;
    if (receive.state == "done" || receive.state == "failed" || receive.state == "cancelled") {
      receives_.erase(found);  // dismissing a finished one
    } else {
      receive.state = "cancelled";
      receive.retry_at = 0;
      receive.url.clear();
      DeleteFileA(receive.part.c_str());
      Json command = Json::object();
      command.set("op", Json("cancel"));
      command.set("id", Json(receive.helper_id));
      send_locked(command);
    }
  }
  publish();
  return true;
}

void LinkService::tick() {
  const uint64_t began = GetTickCount64();
  bool changed = false;
  bool stop = false;
  bool restart = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    restart = restart_pending_;
    restart_pending_ = false;
  }
  if (restart) {
    // Live links are offered again by the new helper as soon as it is ready.
    stop_helper();
    std::lock_guard<std::mutex> lock(mutex_);
    std::string ignored;
    if (!links_.empty()) start_helper_locked(ignored);
  }
  {
    // The PC woke from sleep: the tick clock counts the sleep, the unbiased clock does not, so the two
    // drift apart by exactly that long. (A tick held up by a slow wait moves both and is not a sleep.)
    // The helper is told at once, so it looks for paths again instead of on its own next timer.
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t clock = GetTickCount64();
    ULONGLONG unbiased = 0;
    QueryUnbiasedInterruptTime(&unbiased);
    const uint64_t awake = unbiased / 10000;  // 100 ns units to ms
    if (last_tick_ != 0 && process_ != nullptr) {
      const uint64_t slept = (clock - last_tick_) - std::min(clock - last_tick_, awake - last_awake_);
      if (slept > 10000) {
        BX_LOG_NOTE("sharing: the PC was asleep for %llu s; looking for the network again",
                    static_cast<unsigned long long>(slept / 1000));
        Json command = Json::object();
        command.set("op", Json("network"));
        send_locked(command);
      }
    }
    last_tick_ = clock;
    last_awake_ = awake;
  }
  if (helper_dead()) {
    BX_LOG_WARN("the sharing helper ended by itself");
    stop_helper();
  }
  std::vector<std::string> retries;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = now_unix();
    // A link that runs out here is removed like one the helper ended: with every carried entry of its id,
    // or the next write would put it back and the next start would serve it again.
    std::vector<std::string> expired;
    for (const auto& [id, link] : links_) {
      if (link.expires <= now && link.watching == 0) expired.push_back(id);
    }
    for (const std::string& id : expired) {
      Json command = Json::object();
      command.set("op", Json("unserve"));
      command.set("share", Json(id));
      send_locked(command);
      drop_link_locked(id);
      changed = true;
    }
    if (changed) save_locked();
    // The watchdog: a receive that never connects, or one that stops arriving, is not left hanging.
    const uint64_t clock = GetTickCount64();
    for (auto& [id, receive] : receives_) {
      if (receive.state == "retrying") {
        if (receive.retry_at != 0 && clock >= receive.retry_at) retries.push_back(id);
        continue;
      }
      const std::string verdict = receive_watchdog(receive.state, receive.progress, clock);
      if (verdict.empty()) continue;
      receive_failed_locked(receive, verdict);
      changed = true;
    }
    if (changed) trim_finished_locked();
    bool busy = !links_.empty();
    for (const auto& [id, receive] : receives_) {
      if (receive.state == "connecting" || receive.state == "receiving" || receive.state == "asking" ||
          receive.state == "retrying") {
        busy = true;
      }
    }
    stop = idle_.should_stop(busy, process_ != nullptr, clock);
    if (busy) {
      // Live links from before a restart (or after a crash): the helper comes back by itself to keep
      // serving them, at most once every ten seconds so a missing helper is not retried every tick,
      // and a receiver's retries still find it inside their own window.
      if (!process_ && !links_.empty() && (last_start_ == 0 || clock - last_start_ > 10000)) {
        std::string ignored;
        start_helper_locked(ignored);
      }
    }
  }
  // A retry needs the helper on the network before it can send anything, and that wait is a network
  // wait: it happens here, on the tick's own thread, never on the engine's window thread.
  for (const std::string& id : retries) {
    {
      // The engine is going: no retry is worth holding shutdown for.
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) break;
    }
    std::string error;
    const bool ready = ensure_helper(error);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = receives_.find(id);
      if (found == receives_.end() || found->second.state != "retrying") continue;
      Receive& receive = found->second;
      if (!ready) {
        receive_failed_locked(receive, "helper");
      } else if (link_expired_here(receive.expires, now_unix())) {
        receive_failed_locked(receive, "expired");
      } else {
        BX_LOG_INFO("link receive %s: attempt %d of %d", id.c_str(), receive.attempt + 1, receive_tries(!receive.tag.empty()));
        send_fetch_locked(receive);
      }
    }
    changed = true;
  }
  if (stop) stop_helper();
  if (changed) publish();
  // A pass that takes seconds is doing something no pass should: it is the only way a receive can sit
  // past its stall limit unnoticed, so it names itself here instead of being guessed at later.
  const uint64_t took = GetTickCount64() - began;
  if (took > 5000) BX_LOG_WARN("sharing: one pass of the link tick took %llu ms", static_cast<unsigned long long>(took));
}

void LinkService::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  ticker_cv_.notify_all();
  ready_cv_.notify_all();  // a share or retry waiting for a helper must not sit out its 25 s at quit
  if (ticker_.joinable()) ticker_.join();
  stop_helper();
  if (reader_.joinable()) reader_.join();
}

}  // namespace bx
