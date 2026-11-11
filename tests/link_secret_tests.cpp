// links.json keeps a short link and the ticket it was made from. Both are bearer credentials, so both are
// sealed with the same account-bound protection as a link's token (link_secret.h), and a plaintext one an
// older build wrote still opens and is sealed by the next write of the file.
//
// Checked here: the storage rule on its own, one real DPAPI round trip on this account, and the writes of
// links.json that migration makes - what it seals, what it must carry through untouched, and that a fresh
// short link or a stop replaces what an unreadable field or a duplicate entry held. It needs nothing beyond
// link.cpp, which tests/engine_tests.cpp already links, so registering it is four calls in that file's
// main():
//     g_failures += test_link_short_secret();
//     g_failures += test_link_short_secret_migration();
//     g_failures += test_link_short_secret_replaced();
//     g_failures += test_link_short_secret_duplicate();
//     g_failures += test_link_short_secret_helper_expired();
//     g_failures += test_link_short_secret_local_expired();
// beside test_links(), plus this file added to BUFFRX_TEST_SOURCES in CMakeLists.txt. Built on its own
// with -DBUFFRX_LINK_SECRET_STANDALONE it is the storage rule alone: the other tests drive a whole
// LinkService and need the engine sources beside it.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <windows.h>

#include "../src/engine/library.h"
#include "../src/engine/link.h"
#include "../src/engine/link_secret.h"
#include "../src/engine/platform.h"

namespace {

int g_short_secret_failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  std::printf("  FAIL %s\n", what);
  ++g_short_secret_failures;
}

std::string with_prefix(const std::string& sealed) { return std::string(bx::kShortSecretPrefix) + sealed; }

}  // namespace

// Returns how many checks failed, so the test harness can add it to its own count.
int test_link_short_secret() {
  using bx::ShortSecret;
  const std::string url = "https://buffrx.app/s/AbC123#ticket-ticket-ticket";
  const std::string fragment = "ticket-ticket-ticket";

  // --- the storage rule, without Windows in the way -----------------------------------------------
  expect(bx::classify_short_secret("", false) == ShortSecret::Missing &&
             bx::classify_short_secret("", true) == ShortSecret::Missing,
         "an absent field is missing, not a credential");
  expect(bx::classify_short_secret(url, false) == ShortSecret::Legacy &&
             bx::classify_short_secret(fragment, true) == ShortSecret::Legacy,
         "a plaintext short link and ticket from an older build still open");
  expect(bx::classify_short_secret(with_prefix("AQAAANCMnd8BFdERjGo="), false) == ShortSecret::Sealed &&
             bx::classify_short_secret(with_prefix("AQAAANCMnd8BFdERjGo="), true) == ShortSecret::Sealed,
         "a value carrying the seal prefix is sealed, whatever it resembles");
  expect(bx::legacy_short_secret_ok("http://127.0.0.1:8123/s/x#y", false),
         "a loopback short link a probe made is still one");
  expect(!bx::legacy_short_secret_ok("ticket-ticket", false) && !bx::legacy_short_secret_ok(url, true),
         "neither field's plaintext is accepted as the other's");
  expect(!bx::legacy_short_secret_ok("", true) && !bx::legacy_short_secret_ok("has space", true) &&
             !bx::legacy_short_secret_ok(std::string(401, 'x'), true),
         "an empty, spaced or oversized ticket was never one");
  expect(!bx::legacy_short_secret_ok("AQAAANCMnd8BFdERjGo=", false),
         "a base64 blob that is not a short link is not read as one");
  expect(bx::sealed_short_payload(with_prefix("abc")) == "abc" && bx::sealed_short_payload(url).empty(),
         "the payload is the seal without its prefix, and nothing for a plaintext value");

  // --- the seal itself, on this Windows account ----------------------------------------------------
  expect(bx::seal_short_secret("").empty(), "nothing is sealed from an empty value");
  const std::string sealed_url = bx::seal_short_secret(url);
  expect(!sealed_url.empty() && sealed_url.rfind(bx::kShortSecretPrefix, 0) == 0,
         "a short link seals, marked as sealed");
  expect(sealed_url.find(url) == std::string::npos && sealed_url.find(fragment) == std::string::npos,
         "the file never holds the link or its ticket in the clear");
  std::string opened;
  expect(bx::open_short_secret(sealed_url, false, opened) == ShortSecret::Sealed && opened == url,
         "a sealed short link opens exactly as it was");
  const std::string sealed_fragment = bx::seal_short_secret(fragment);
  opened.clear();
  expect(bx::open_short_secret(sealed_fragment, true, opened) == ShortSecret::Sealed && opened == fragment,
         "a sealed ticket opens exactly as it was");
  // The seal is bound to the field's own shape, so a cut or changed value is not handed back.
  opened.clear();
  const std::string truncated = sealed_url.substr(0, sealed_url.size() - 4);
  expect(bx::open_short_secret(truncated, false, opened) == ShortSecret::Broken && opened.empty(),
         "a seal that was cut or changed does not open");
  opened.clear();
  expect(bx::open_short_secret("dpapi:not-a-seal", true, opened) == ShortSecret::Broken && opened.empty(),
         "sealed text that does not open is not read back as the plaintext it resembles");
  opened.clear();
  expect(bx::open_short_secret("AQAAANCMnd8BFdERjGo=", true, opened) == ShortSecret::Legacy &&
             opened == "AQAAANCMnd8BFdERjGo=",
         "unprefixed text is the plaintext an older build wrote and opens as it is");

  // A legacy value opens unchanged, and sealing it is what the next write of links.json does.
  opened.clear();
  expect(bx::open_short_secret(url, false, opened) == ShortSecret::Legacy && opened == url,
         "a plaintext short link survives the upgrade as it was");

  return g_short_secret_failures;
}

#ifndef BUFFRX_LINK_SECRET_STANDALONE

namespace {

std::string read_all(const std::string& path) {
  std::string text;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return text;
  char buffer[8192];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
  std::fclose(file);
  return text;
}

bool write_all(const std::string& path, const std::string& text) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const size_t wrote = std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
  return wrote == text.size();
}

int occurrences(const std::string& text, const std::string& needle) {
  int count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
  return count;
}

bx::Json entry_for(const bx::Json& stored, const std::string& id) {
  for (const bx::Json& item : stored["links"].items()) {
    if (item["id"].as_string() == id) return item;
  }
  return bx::Json();
}

// An environment variable a test changes: whatever it held before is put back when the test ends, so a
// probe's setting never outlives it and the next test or the developer's own session keeps its value.
class ScopedEnv {
 public:
  explicit ScopedEnv(const char* name) : name_(name) {
    const DWORD size = GetEnvironmentVariableA(name_.c_str(), nullptr, 0);
    if (size == 0) return;
    std::string value(size, '\0');
    const DWORD read = GetEnvironmentVariableA(name_.c_str(), value.data(), size);
    if (read == 0 || read >= size) return;
    value.resize(read);
    value_ = value;
    had_ = true;
  }
  ~ScopedEnv() { SetEnvironmentVariableA(name_.c_str(), had_ ? value_.c_str() : nullptr); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

  void set(const char* value) { SetEnvironmentVariableA(name_.c_str(), value); }

 private:
  std::string name_, value_;
  bool had_ = false;
};

// A throwaway data root (and its clips folder), named after this process so two runs never share one.
std::string make_root(const char* tag, std::string& clips) {
  char temp[MAX_PATH] = {0};
  GetTempPathA(MAX_PATH, temp);
  char name[96] = {0};
  std::snprintf(name, sizeof(name), "buffrx-link-%s-%lu", tag, static_cast<unsigned long>(GetCurrentProcessId()));
  const std::string root = std::string(temp) + name;
  clips = root + "\\clips";
  CreateDirectoryA(root.c_str(), nullptr);
  CreateDirectoryA(clips.c_str(), nullptr);
  return root;
}

void remove_root(const std::string& root, const std::string& clips) {
  DeleteFileA((root + "\\links.json").c_str());
  WIN32_FIND_DATAA found{};
  HANDLE search = FindFirstFileA((clips + "\\*").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      DeleteFileA((clips + "\\" + found.cFileName).c_str());
    } while (FindNextFileA(search, &found));
    FindClose(search);
  }
  RemoveDirectoryA(clips.c_str());
  RemoveDirectoryA(root.c_str());
}

// What the short-link fields of one entry in links.json open to, after a write.
bool opens_to(const bx::Json& written, const std::string& id, const char* field, bool fragment,
              const std::string& expected) {
  std::string value;
  const std::string stored = entry_for(written, id)[field].as_string();
  return bx::open_short_secret(stored, fragment, value) == bx::ShortSecret::Sealed && value == expected;
}

// One links.json entry, with a token this account's protection opens and nothing else.
bx::Json fixture_entry(const std::string& id, const std::string& sealed_token, const std::string& file,
                       int64_t created, int64_t expires) {
  bx::Json item = bx::Json::object();
  item.set("id", bx::Json(id));
  item.set("token", bx::Json(sealed_token));
  item.set("file", bx::Json(file));
  item.set("name", bx::Json("clip.mp4"));
  item.set("size", bx::Json(static_cast<int64_t>(16)));
  item.set("created", bx::Json(created));
  item.set("expires", bx::Json(expires));
  item.set("viewers", bx::Json(0));
  return item;
}

bool write_links_file(const std::string& root, const std::vector<bx::Json>& entries) {
  bx::Json stored = bx::Json::object();
  stored.set("key", bx::Json(bx::protect_secret(std::string(64, 'e'))));
  stored.set("endpoint", bx::Json(std::string(64, '0')));
  bx::Json links = bx::Json::array();
  for (const bx::Json& item : entries) links.push(item);
  stored.set("links", links);
  return write_all(root + "\\links.json", stored.dump(2));
}

}  // namespace

// One write of links.json is a migration, and it must not take another link with it. Five entries are
// written: a live one whose short link and ticket are the plaintext an older build wrote (the migration's
// trigger), a live one whose file is on a drive that is not here, one whose short-link fields will not open
// on this account, one whose token will not, and one already expired. After the write every live one is
// still there - the plaintext sealed, the unopenable fields exactly as they were - none of them is served
// from memory except the two that can be, and the expired one is gone. A restart sees the same short link,
// and stopping it uses the ticket the code was made from, not today's link.
//
// Returns how many checks failed.
int test_link_short_secret_migration() {
  const int before = g_short_secret_failures;
  char temp[MAX_PATH] = {0};
  GetTempPathA(MAX_PATH, temp);
  char name[64] = {0};
  std::snprintf(name, sizeof(name), "buffrx-link-secret-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
  const std::string root = std::string(temp) + name;
  const std::string clips = root + "\\clips";
  const std::string links_file = root + "\\links.json";
  const std::string live_file = clips + "\\live.mp4";
  const std::string away_file = clips + "\\away.mp4";  // the drive it sits on is not plugged in
  CreateDirectoryA(root.c_str(), nullptr);
  CreateDirectoryA(clips.c_str(), nullptr);
  expect(write_all(live_file, std::string(16, 'x')), "the migration fixture can make its clip");

  // A link counts only while BuffrX's library owns its file. The folders are put back afterwards so the
  // rest of the suite reads the ones it set.
  bx::LibraryPaths saved_paths;
  bx::library_paths(saved_paths);
  bx::LibraryPaths paths;
  paths.clips_dir = clips;
  paths.screenshots_dir = clips;
  paths.thumbnails_dir = root + "\\thumbnails";
  paths.recordings_dir = root + "\\recordings";
  bx::library_init(paths);

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string url = "https://buffrx.app/s/abcdefghjk";
  const std::string ticket = "payload-one?n=Test";
  const std::string away_url = "https://buffrx.app/s/awayawayaway";
  const std::string away_ticket = "payload-two?n=Test";
  const std::string kept_url = with_prefix("not-a-seal");
  const std::string kept_ticket = with_prefix("not-a-ticket");
  const std::string foreign_token = "not-a-sealed-token";
  const std::string foreign_url = "https://buffrx.app/s/plaintoken#payload-three";

  const std::string live_id(32, 'a');
  const std::string away_id(32, 'b');
  const std::string kept_id(32, 'c');
  const std::string expired_id(32, 'd');
  const std::string foreign_id(32, 'e');
  const std::string sealed_token = bx::protect_secret(std::string(32, 'f'));

  auto make_entry = [&](const std::string& id, const std::string& token, const std::string& file, int64_t expires) {
    bx::Json item = bx::Json::object();
    item.set("id", bx::Json(id));
    item.set("token", bx::Json(token));
    item.set("file", bx::Json(file));
    item.set("name", bx::Json("clip.mp4"));
    item.set("size", bx::Json(static_cast<int64_t>(16)));
    item.set("created", bx::Json(now - 60));
    item.set("expires", bx::Json(expires));
    item.set("viewers", bx::Json(0));
    return item;
  };

  bx::Json live = make_entry(live_id, sealed_token, live_file, now + 3600);
  live.set("short_url", bx::Json(url));
  live.set("short_fragment", bx::Json(ticket));
  bx::Json away = make_entry(away_id, sealed_token, away_file, now + 3600);
  away.set("short_url", bx::Json(away_url));
  away.set("short_fragment", bx::Json(away_ticket));
  bx::Json kept = make_entry(kept_id, sealed_token, live_file, now + 3600);
  kept.set("short_url", bx::Json(kept_url));
  kept.set("short_fragment", bx::Json(kept_ticket));
  bx::Json expired = make_entry(expired_id, sealed_token, live_file, now - 60);
  expired.set("short_url", bx::Json(url));
  bx::Json foreign = make_entry(foreign_id, foreign_token, live_file, now + 3600);
  foreign.set("short_url", bx::Json(foreign_url));  // plaintext an older build wrote, beside a token that will not open

  bx::Json stored = bx::Json::object();
  stored.set("key", bx::Json(bx::protect_secret(std::string(64, 'e'))));
  stored.set("endpoint", bx::Json(std::string(64, '1')));
  bx::Json entries = bx::Json::array();
  entries.push(live);
  entries.push(away);
  entries.push(kept);
  entries.push(expired);
  entries.push(foreign);
  stored.set("links", entries);
  expect(write_all(links_file, stored.dump(2)), "the migration fixture writes links.json");

  {
    // The first start: the plaintext fields make this write a migration, and it carries the rest through.
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json state = service.list();
    bool live_active = false, away_active = false, kept_active = false, foreign_active = false;
    std::string handed_out;
    for (const bx::Json& link : state["links"].items()) {
      const std::string id = link["id"].as_string();
      if (id == live_id) {
        live_active = true;
        handed_out = link["url"].as_string();
      }
      if (id == away_id) away_active = true;
      if (id == kept_id) kept_active = true;
      if (id == foreign_id) foreign_active = true;
    }
    expect(live_active && kept_active && !away_active && !foreign_active,
           "only the links that can be served are active after a migration");
    expect(handed_out == url, "the short link an older build left in the clear is still the one handed out");
  }

  const std::string text = read_all(links_file);
  expect(!text.empty(), "the migration wrote links.json back");
  // Nothing that was a credential is in the clear any more, in any entry - the carried ones included.
  bool no_plaintext = !text.empty();
  for (const std::string& secret : {url, ticket, away_url, away_ticket, foreign_url}) {
    if (text.find(secret) != std::string::npos) no_plaintext = false;
  }
  expect(no_plaintext, "no short link or ticket the migration touched is left in the clear");
  expect(text.find(kept_url) != std::string::npos && text.find(kept_ticket) != std::string::npos,
         "a short-link field that will not open here is written back exactly as it was read");
  expect(text.find(foreign_token) != std::string::npos,
         "a token sealed to another account is written back unchanged");
  expect(occurrences(text, away_id) == 1 && occurrences(text, foreign_id) == 1,
         "a link that cannot be served right now is still in the file once");
  expect(text.find(expired_id) == std::string::npos, "an expired link is not carried forward");

  bx::Json written;
  std::string error;
  expect(bx::Json::parse(text, written, error), "the migrated links.json is still JSON");
  const std::string kept_url_sealed = entry_for(written, live_id)["short_url"].as_string();
  const std::string kept_fragment_sealed = entry_for(written, live_id)["short_fragment"].as_string();
  expect(!kept_url_sealed.empty() && !kept_fragment_sealed.empty(),
         "the migrated link keeps a short link and the ticket beside it");
  std::string opened_url, opened_fragment, opened_away;
  expect(bx::open_short_secret(kept_url_sealed, false, opened_url) == bx::ShortSecret::Sealed && opened_url == url,
         "the sealed short link opens to the one the service handed back");
  expect(bx::open_short_secret(kept_fragment_sealed, true, opened_fragment) == bx::ShortSecret::Sealed &&
             opened_fragment == ticket,
         "the sealed ticket opens to the one the service was handed");
  expect(bx::open_short_secret(entry_for(written, away_id)["short_url"].as_string(), false, opened_away) ==
                 bx::ShortSecret::Sealed &&
             opened_away == away_url,
         "a plaintext field of a link that is away is sealed too, not dropped");
  expect(entry_for(written, foreign_id)["token"].as_string() == foreign_token,
         "the token that will not open is never replaced");

  {
    // A restart: the short link is the one handed out again, and stopping it revokes the ticket the code
    // was made from. The revoke goes to a closed loopback port, never to the product's service.
    ScopedEnv shorten_url("BUFFRX_SHORTEN_URL");
    shorten_url.set("http://127.0.0.1:9/api/shorten");
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json restarted = service.list();
    std::string handed_out;
    for (const bx::Json& link : restarted["links"].items()) {
      if (link["id"].as_string() == live_id) handed_out = link["url"].as_string();
    }
    expect(handed_out == url, "a restarted engine hands out the short link it kept, not the long one");
    std::string stop_error;
    expect(service.stop(live_id, stop_error), "the link kept across the restart can be stopped");
    const bx::Json after = service.list();
    bool still_offered = false;
    for (const bx::Json& link : after["links"].items()) {
      if (link["id"].as_string() == live_id) still_offered = true;
    }
    expect(!still_offered, "a stopped link is not offered again");
  }

  const std::string after_stop = read_all(links_file);
  expect(after_stop.find(live_id) == std::string::npos, "a stopped link leaves links.json");
  expect(occurrences(after_stop, away_id) == 1,
         "stopping a link leaves the entry that could not be served where it was");

  DeleteFileA(links_file.c_str());
  DeleteFileA(live_file.c_str());
  RemoveDirectoryA(clips.c_str());
  RemoveDirectoryA(root.c_str());
  bx::library_init(saved_paths);
  return g_short_secret_failures - before;
}

// A short link the service hands back replaces whatever that link's entry held, including a field this
// account cannot open. Saving the unreadable value instead lost the new link on the next start, and with it
// the ticket a stop revokes by.
int test_link_short_secret_replaced() {
  const int before = g_short_secret_failures;
  std::string clips;
  const std::string root = make_root("replaced", clips);

  bx::LibraryPaths saved_paths;
  bx::library_paths(saved_paths);
  bx::LibraryPaths paths;
  paths.clips_dir = clips;
  paths.screenshots_dir = clips;
  paths.thumbnails_dir = root + "\\thumbnails";
  paths.recordings_dir = root + "\\recordings";
  bx::library_init(paths);

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string live_file = clips + "\\live.mp4";
  expect(write_all(live_file, std::string(16, 'x')), "the replaced-seal fixture can make its clip");
  const std::string id(32, '1');
  bx::Json item = fixture_entry(id, bx::protect_secret(std::string(32, 'f')), live_file, now - 60, now + 3600);
  item.set("short_url", bx::Json(with_prefix("not-a-seal")));
  item.set("short_fragment", bx::Json(with_prefix("not-a-ticket")));
  expect(write_links_file(root, {item}), "the replaced-seal fixture writes links.json");

  const std::string url = "https://buffrx.app/s/replacedseal";
  const std::string ticket = "payload-replaced?n=Test";
  {
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json state = service.list();
    bool active = false;
    for (const bx::Json& link : state["links"].items()) {
      if (link["id"].as_string() == id) active = true;
    }
    expect(active, "a link whose short-link fields will not open is still active on its long link");
    service.set_short_url(id, url, ticket);
  }

  bx::Json written;
  std::string error;
  const std::string text = read_all(root + "\\links.json");
  expect(bx::Json::parse(text, written, error), "the replaced links.json is still JSON");
  expect(opens_to(written, id, "short_url", false, url),
         "the short link the service just handed back is what links.json keeps");
  expect(opens_to(written, id, "short_fragment", true, ticket),
         "the ticket the new short link was made from is what links.json keeps");

  {
    ScopedEnv shorten_url("BUFFRX_SHORTEN_URL");
    shorten_url.set("http://127.0.0.1:9/api/shorten");
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json restarted = service.list();
    std::string handed_out;
    for (const bx::Json& link : restarted["links"].items()) {
      if (link["id"].as_string() == id) handed_out = link["url"].as_string();
    }
    expect(handed_out == url, "a restarted engine copies the replacing short link, not the long one");
    std::string stop_error;
    expect(service.stop(id, stop_error), "the link whose seal was replaced can be stopped");
    const bx::Json after = service.list();
    bool still_offered = false;
    for (const bx::Json& link : after["links"].items()) {
      if (link["id"].as_string() == id) still_offered = true;
    }
    expect(!still_offered, "the replaced link is not offered once it is stopped");
  }
  const std::string stopped_text = read_all(root + "\\links.json");
  expect(stopped_text.find(id) == std::string::npos,
         "the replaced link leaves links.json when it is stopped");

  remove_root(root, clips);
  bx::library_init(saved_paths);
  return g_short_secret_failures - before;
}

// A duplicate entry for an id this run could serve is carried so a write never erases it - but stopping
// that id must drop it, or the next write puts the stopped link back and the next start serves it again.
int test_link_short_secret_duplicate() {
  const int before = g_short_secret_failures;
  std::string clips;
  const std::string root = make_root("duplicate", clips);

  bx::LibraryPaths saved_paths;
  bx::library_paths(saved_paths);
  bx::LibraryPaths paths;
  paths.clips_dir = clips;
  paths.screenshots_dir = clips;
  paths.thumbnails_dir = root + "\\thumbnails";
  paths.recordings_dir = root + "\\recordings";
  bx::library_init(paths);

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string live_file = clips + "\\live.mp4";
  const std::string away_file = clips + "\\away.mp4";  // the drive it sits on is not plugged in
  expect(write_all(live_file, std::string(16, 'x')), "the duplicate fixture can make its clip");
  const std::string id(32, '2');
  const std::string away_id(32, '3');
  bx::Json live = fixture_entry(id, bx::protect_secret(std::string(32, 'f')), live_file, now - 60, now + 3600);
  // The same id again, with a token this account cannot open: it is carried, not served.
  bx::Json duplicate = fixture_entry(id, "not-a-sealed-token", live_file, now - 60, now + 3600);
  duplicate.set("short_url", bx::Json(with_prefix("not-a-seal")));
  bx::Json away = fixture_entry(away_id, bx::protect_secret(std::string(32, 'a')), away_file, now - 60, now + 3600);
  expect(write_links_file(root, {live, duplicate, away}), "the duplicate fixture writes links.json");

  {
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json state = service.list();
    int offered = 0;
    for (const bx::Json& link : state["links"].items()) {
      if (link["id"].as_string() == id) ++offered;
    }
    expect(offered == 1, "only the entry that can be served is offered for a duplicated id");
    std::string stop_error;
    expect(service.stop(id, stop_error), "the duplicated id can be stopped");
  }

  const std::string text = read_all(root + "\\links.json");
  expect(text.find(id) == std::string::npos, "stopping an id drops the duplicate entry carried for it");
  expect(occurrences(text, away_id) == 1, "stopping a link leaves an unavailable link with another id alone");

  remove_root(root, clips);
  bx::library_init(saved_paths);
  return g_short_secret_failures - before;
}

// The helper ends a link with its own "expired" event, and that removal used to take only the active
// entry: a duplicate carried for the same id was written back by the next save and served again on the
// next start. The removal now takes every entry with that id, and leaves an unavailable link with
// another id alone.
int test_link_short_secret_helper_expired() {
  const int before = g_short_secret_failures;
  std::string clips;
  const std::string root = make_root("helper-expired", clips);

  bx::LibraryPaths saved_paths;
  bx::library_paths(saved_paths);
  bx::LibraryPaths paths;
  paths.clips_dir = clips;
  paths.screenshots_dir = clips;
  paths.thumbnails_dir = root + "\\thumbnails";
  paths.recordings_dir = root + "\\recordings";
  bx::library_init(paths);

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string live_file = clips + "\\live.mp4";
  const std::string away_file = clips + "\\away.mp4";  // the drive it sits on is not plugged in
  expect(write_all(live_file, std::string(16, 'x')), "the helper-expiry fixture can make its clip");
  const std::string id(32, '6');
  const std::string away_id(32, '7');
  bx::Json live = fixture_entry(id, bx::protect_secret(std::string(32, 'f')), live_file, now - 60, now + 3600);
  // The same id again, with a token this account cannot open: carried, never served, still in the file.
  bx::Json duplicate = fixture_entry(id, "not-a-sealed-token", live_file, now - 60, now + 3600);
  duplicate.set("short_url", bx::Json(with_prefix("not-a-seal")));
  bx::Json away = fixture_entry(away_id, bx::protect_secret(std::string(32, 'a')), away_file, now - 60, now + 3600);
  expect(write_links_file(root, {live, duplicate, away}), "the helper-expiry fixture writes links.json");

  {
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json state = service.list();
    int offered = 0;
    for (const bx::Json& link : state["links"].items()) {
      if (link["id"].as_string() == id) ++offered;
    }
    expect(offered == 1, "the link the helper is about to end is offered once");
    bx::Json event = bx::Json::object();
    event.set("ev", bx::Json("expired"));
    event.set("share", bx::Json(id));
    service.on_event(event);
  }

  const std::string text = read_all(root + "\\links.json");
  expect(text.find(id) == std::string::npos, "a link the helper ended takes its carried duplicate with it");
  expect(occurrences(text, away_id) == 1, "ending one link leaves an unavailable link with another id alone");

  {
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json state = service.list();
    bool served = false;
    for (const bx::Json& link : state["links"].items()) {
      if (link["id"].as_string() == id) served = true;
    }
    expect(!served, "a link the helper ended is not served again after a restart");
  }

  remove_root(root, clips);
  bx::library_init(saved_paths);
  return g_short_secret_failures - before;
}

// This PC's own expiry (tick) used to erase the active entry only, so a duplicate carried for the same id
// came back on the next write and the expired link was served again after a restart.
int test_link_short_secret_local_expired() {
  const int before = g_short_secret_failures;
  std::string clips;
  const std::string root = make_root("local-expired", clips);

  bx::LibraryPaths saved_paths;
  bx::library_paths(saved_paths);
  bx::LibraryPaths paths;
  paths.clips_dir = clips;
  paths.screenshots_dir = clips;
  paths.thumbnails_dir = root + "\\thumbnails";
  paths.recordings_dir = root + "\\recordings";
  bx::library_init(paths);

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const std::string live_file = clips + "\\live.mp4";
  const std::string away_file = clips + "\\away.mp4";  // the drive it sits on is not plugged in
  expect(write_all(live_file, std::string(16, 'x')), "the local-expiry fixture can make its clip");
  const std::string id(32, '8');
  const std::string away_id(32, '9');
  // Live when it is loaded, so it is active; it runs out while the test watches.
  const int64_t expires = now + 2;
  bx::Json live = fixture_entry(id, bx::protect_secret(std::string(32, 'f')), live_file, now - 60, expires);
  bx::Json duplicate = fixture_entry(id, "not-a-sealed-token", live_file, now - 60, expires);
  bx::Json away = fixture_entry(away_id, bx::protect_secret(std::string(32, 'a')), away_file, now - 60, now + 3600);
  expect(write_links_file(root, {live, duplicate, away}), "the local-expiry fixture writes links.json");

  {
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json state = service.list();
    int offered = 0;
    for (const bx::Json& link : state["links"].items()) {
      if (link["id"].as_string() == id) ++offered;
    }
    expect(offered == 1, "the link that is about to run out is offered once");
    // The service's own tick does this once a second; calling it here keeps the test on the rule and not
    // on the ticker's beat.
    const uint64_t deadline = GetTickCount64() + 8000;
    while (static_cast<int64_t>(std::time(nullptr)) <= expires && GetTickCount64() < deadline) Sleep(50);
    service.tick();
  }

  const std::string text = read_all(root + "\\links.json");
  expect(text.find(id) == std::string::npos, "a link that ran out takes its carried duplicate with it");
  expect(occurrences(text, away_id) == 1, "expiry leaves an unavailable link with another id alone");

  {
    bx::LinkService service(root, root + "\\no-helper.exe");
    const bx::Json state = service.list();
    bool served = false;
    for (const bx::Json& link : state["links"].items()) {
      if (link["id"].as_string() == id) served = true;
    }
    expect(!served, "an expired link is not served again after a restart");
  }

  remove_root(root, clips);
  bx::library_init(saved_paths);
  return g_short_secret_failures - before;
}

#endif  // BUFFRX_LINK_SECRET_STANDALONE

#ifdef BUFFRX_LINK_SECRET_STANDALONE
int main() {
  std::printf("link secret\n");
  const int failures = test_link_short_secret();
  if (failures == 0) std::printf("link secret: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
#endif
