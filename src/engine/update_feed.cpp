#include "update_feed.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include "../linux/compat/windows.h"
#endif

#include <cstdlib>
#include <algorithm>
#include <string>

#include "../shared/json.h"
#ifdef _WIN32
#include "update_http.h"
#endif
#include "update_math.h"

namespace bx {
namespace {

#ifdef _WIN32
std::wstring wide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}
#endif

// One GET into a string. False with `error` set when the request, the connection or the status fails.
// `status` is 0 when the request never reached a server, which the caller reads as "no network".
bool http_get(const std::string& url, int timeout_ms, ULONGLONG deadline, size_t max_bytes, std::string& body,
              DWORD& status, std::string& error, const std::string& user_agent) {
#ifdef _WIN32
  body.clear();
  status = 0;
  const std::wstring target = wide(url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {0};
  wchar_t path[2048] = {0};
  wchar_t extra[4096] = {0};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 255;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2047;
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 4095;
  const bool local = is_loopback_url(url);
  if (!WinHttpCrackUrl(target.c_str(), 0, 0, &parts) ||
      (parts.nScheme != INTERNET_SCHEME_HTTPS && !(local && parts.nScheme == INTERNET_SCHEME_HTTP))) {
    error = "not an https address";
    return false;
  }

  const std::wstring object = std::wstring(path) + extra;
  // The API answers directly; anything that points elsewhere is not our release and is not followed.
  UpdateHttp request;
  const std::wstring agent = wide(user_agent);
  if (!request.open(host, parts.nPort, object.c_str(), parts.nScheme == INTERNET_SCHEME_HTTPS,
                    agent.c_str(), WINHTTP_OPTION_REDIRECT_POLICY_NEVER, timeout_ms, deadline)) {
    error = "could not open the request";
    return false;
  }

  const std::wstring headers = L"Accept: application/vnd.github+json";
  if (!request.send(headers.c_str())) {
    error = "could not send the request";
    return false;
  }
  if (!request.receive(status)) {
    error = "no answer";
    return false;
  }
  if (status == 404) {
    error = "no release published yet";
    return false;
  }
  if (status != 200) {
    error = "GitHub answered " + std::to_string(status);
    return false;
  }

  char buffer[8192];
  DWORD read = 0;
  while (true) {
    if (!request.read(buffer, sizeof(buffer), read)) {
      error = "the answer was interrupted";
      return false;
    }
    if (read == 0) break;
    body.append(buffer, read);
    if (body.size() > max_bytes) {
      error = "the answer was too large";
      return false;
    }
  }
  return true;
}
#else
  (void)deadline;
  body.clear();
  status = 0;
  // The feed is HTTPS; a probe may ask a loopback HTTP service. Nothing else is accepted.
  if (url.rfind("https://", 0) != 0 && !is_loopback_url(url)) {
    error = "not an https address";
    return false;
  }
  CURL* curl = curl_easy_init();
  if (!curl) {
    error = "could not open the request";
    return false;
  }
  const struct curl_slist* headers = nullptr;
  struct curl_slist* list = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
  headers = list;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  size_t limit = max_bytes;
  bool too_large = false;
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   +[](char* data, size_t size, size_t count, void* user) -> size_t {
                     size_t* remaining = static_cast<size_t*>(user);
                     const size_t bytes = size * count;
                     if (bytes > *remaining) return 0;
                     *remaining -= bytes;
                     return bytes;
                   });
  // WRITEDATA is the body; the limit pointer travels through it as well.
  struct Sink {
    std::string* body;
    size_t* limit;
  } sink{&body, &limit};
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   +[](char* data, size_t size, size_t count, void* user) -> size_t {
                     auto* sink = static_cast<Sink*>(user);
                     const size_t bytes = size * count;
                     if (bytes > *sink->limit) {
                       *sink->limit = 0;
                       return 0;
                     }
                     *sink->limit -= bytes;
                     sink->body->append(data, bytes);
                     return bytes;
                   });
  const CURLcode code = curl_easy_perform(curl);
  if (code == CURLE_WRITE_ERROR) too_large = limit == 0;
  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  if (code != CURLE_OK || too_large) {
    error = too_large ? "the answer was too large" : "no answer";
    return false;
  }
  status = static_cast<DWORD>(http_status);
  if (status == 404) {
    error = "no release published yet";
    return false;
  }
  if (status != 200) {
    error = "GitHub answered " + std::to_string(status);
    return false;
  }
  return true;
#endif
}
}  // namespace

// Localhost only: a probe may point the attempt at a development service, and nothing else, so an
// environment variable can never be used to send a request somewhere real. The authority is parsed rather
// than prefix-matched: "127.0.0.1:80.evil.com" and "localhost:9@evil.com" are not local.
bool is_loopback_url(const std::string& endpoint) {
  const size_t scheme = endpoint.find("://");
  if (scheme == std::string::npos) return false;
  const std::string head = endpoint.substr(0, scheme);
  if (head != "http" && head != "https") return false;
  std::string authority = endpoint.substr(scheme + 3);
  const size_t slash = authority.find('/');
  if (slash != std::string::npos) authority.resize(slash);
  const size_t colon = authority.rfind(':');
  const std::string name = colon == std::string::npos ? authority : authority.substr(0, colon);
  const std::string port = colon == std::string::npos ? std::string() : authority.substr(colon + 1);
  if (name != "127.0.0.1" && name != "localhost") return false;
  return port.empty() || port.find_first_not_of("0123456789") == std::string::npos;
}

std::string update_feed_url() {
  const char* probe = std::getenv("BUFFRX_UPDATE_FEED");
  if (probe != nullptr && is_loopback_url(probe)) return probe;
  return "https://api.github.com/repos/bulhaa1/BuffrX/releases/latest";
}

UpdateFeed parse_update_feed(const std::string& payload, const std::string& asset_name) {
  UpdateFeed feed;
  Json json;
  std::string parse_error;
  if (!Json::parse(payload, json, parse_error)) {
    feed.error = "the answer was not readable";
    return feed;
  }
  const std::string tag = json["tag_name"].as_string();
  if (tag.empty()) {
    feed.error = "the answer was not a release";
    return feed;
  }
  feed.ok = true;
  feed.latest = tag;
  feed.notes = json["body"].as_string();

  for (const Json& asset : json["assets"].items()) {
    if (asset["name"].as_string() != asset_name) continue;
    feed.url = asset["browser_download_url"].as_string();
    feed.digest = asset["digest"].as_string();
    break;
  }
  if (feed.url.empty()) feed.error = std::string("the release has no ") + asset_name;
  return feed;
}

UpdateFeed fetch_update_feed(const std::string& url, int timeout_ms, const std::string& user_agent) {
  UpdateFeed feed;
  std::string body;
  DWORD status = 0;
  const ULONGLONG deadline = GetTickCount64() + 15000;
  const int phase_ms = std::max(1, std::min(timeout_ms, 5000));
  if (!http_get(url, phase_ms, deadline, 1024 * 1024, body, status, feed.error, user_agent)) {
    if ((status != 0 && status < 500) || GetTickCount64() >= deadline ||
        !http_get(url, phase_ms, deadline, 1024 * 1024, body, status, feed.error, user_agent)) return feed;
  }
  feed.error.clear();
  return parse_update_feed(body);
}

}  // namespace bx
