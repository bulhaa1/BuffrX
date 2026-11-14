#include "short_link.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#include <cstdlib>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "../shared/json.h"

namespace bx {
namespace {

#ifdef _WIN32
// Irohs and Windows both hand back UTF-16; the endpoint is ASCII but the path is not ours to assume.
std::wstring wide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

struct Handle {
  HINTERNET handle = nullptr;
  ~Handle() {
    if (handle) WinHttpCloseHandle(handle);
  }
};
#endif

std::string quoted(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

// Localhost only: a probe may point the attempt at a development service, and nothing else, so an
// environment variable can never be used to send a ticket somewhere real. The authority is parsed
// rather than prefix-matched: "127.0.0.1:80.evil.com" and "localhost:9@evil.com" are not local.
bool loopback(const std::string& endpoint) {
  const size_t scheme = endpoint.find("://");
  if (scheme == std::string::npos) return false;
  const std::string head = endpoint.substr(0, scheme);
  if (head != "http" && head != "https") return false;
  std::string authority = endpoint.substr(scheme + 3);
  const size_t slash = authority.find('/');
  if (slash != std::string::npos) authority.resize(slash);
  const size_t colon = authority.rfind(':');
  const std::string host = colon == std::string::npos ? authority : authority.substr(0, colon);
  const std::string port = colon == std::string::npos ? std::string() : authority.substr(colon + 1);
  if (host != "127.0.0.1" && host != "localhost") return false;
  return port.empty() || port.find_first_not_of("0123456789") == std::string::npos;
}

const char* kNoConnection = "No connection for a short link, using the long one.";
const char* kTimedOut = "Short link timed out, falling back to the long link.";
const char* kUnavailable = "Short link unavailable, falling back to the long link.";

// The service every run talks to: the product's own, or a loopback one a probe pointed at.
std::string service_endpoint() {
  const char* probe = std::getenv("BUFFRX_SHORTEN_URL");
  if (probe != nullptr && loopback(probe)) return probe;
  return kShortLinkEndpoint;
}

// One attempt. `failure` is the line to show when this returns false.
#ifdef _WIN32
// One attempt. `failure` is the line to show when this returns false.
bool attempt(const std::string& endpoint, const std::string& body, std::string& url, const char*& failure,
             bool& retry) {
  failure = kUnavailable;
  retry = true;
  const std::wstring target = wide(endpoint);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {0};
  wchar_t path[1024] = {0};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 255;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 1023;
  wchar_t extra[512] = {0};
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 511;
  if (!WinHttpCrackUrl(target.c_str(), 0, 0, &parts) ||
      (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP)) {
    retry = false;
    return false;
  }
  const std::wstring object = std::wstring(path) + extra;

  Handle session;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
  session.handle = WinHttpOpen(L"BuffrX", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
#endif
  if (!session.handle) {
    session.handle = WinHttpOpen(L"BuffrX", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session.handle) return false;
  // Every wait is the same short one: nothing here may hold a share.
  WinHttpSetTimeouts(session.handle, kShortLinkTimeoutMs, kShortLinkTimeoutMs, kShortLinkTimeoutMs, kShortLinkTimeoutMs);

  Handle connection;
  connection.handle = WinHttpConnect(session.handle, host, parts.nPort, 0);
  if (!connection.handle) return false;

  const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  Handle request;
  request.handle = WinHttpOpenRequest(connection.handle, L"POST", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!request.handle) return false;
  // No redirect: an answer that points elsewhere is an error, never a second place a ticket goes.
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));

  const std::wstring headers = L"Content-Type: application/json";
  if (!WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA,
                          0, static_cast<DWORD>(body.size()), 0)) {
    const DWORD code = GetLastError();
    failure = (code == ERROR_WINHTTP_NAME_NOT_RESOLVED || code == ERROR_WINHTTP_CANNOT_CONNECT)
                  ? kNoConnection
                  : (code == ERROR_WINHTTP_TIMEOUT ? kTimedOut : kUnavailable);
    return false;
  }
  DWORD written = 0;
  if (!WinHttpWriteData(request.handle, body.data(), static_cast<DWORD>(body.size()), &written) ||
      written != body.size()) {
    failure = kNoConnection;
    return false;
  }
  if (!WinHttpReceiveResponse(request.handle, nullptr)) {
    failure = GetLastError() == ERROR_WINHTTP_TIMEOUT ? kTimedOut : kUnavailable;
    return false;
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    // A 400 says our own request was wrong: asking again cannot help, and it would double the wait.
    retry = status >= 500;
    failure = kUnavailable;
    return false;
  }
  std::string text;
  char buffer[1024];
  DWORD read = 0;
  while (WinHttpReadData(request.handle, buffer, sizeof(buffer), &read) && read > 0) {
    text.append(buffer, read);
    if (text.size() > 8192) {  // an answer this long is not ours
      retry = false;
      failure = kUnavailable;
      return false;
    }
  }
  Json reply;
  std::string error;
  const std::string candidate =
      Json::parse(text, reply, error) && reply.is_object() ? reply["url"].as_string() : std::string();
  // The shape is checked here rather than trusted: whatever comes back is put in the clipboard. It must
  // stay on the service this request went to, so a wrong answer cannot hand over another host.
  const size_t origin_end = endpoint.find('/', endpoint.find("://") + 3);
  const std::string origin = endpoint.substr(0, origin_end);
  const size_t code = candidate.rfind(origin + "/s/", 0);
  if (code != 0 || candidate.size() < origin.size() + 6 || candidate.size() > 200) {
    retry = false;
    failure = kUnavailable;
    return false;
  }
  url = candidate;
  return true;
}

#else
bool attempt(const std::string& endpoint, const std::string& body, std::string& url, const char*& failure,
             bool& retry) {
  failure = kUnavailable;
  retry = true;
  if (endpoint.rfind("https://", 0) != 0 && !loopback(endpoint)) {
    retry = false;
    return false;
  }
  CURL* curl = curl_easy_init();
  if (!curl) return false;
  struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
  std::string answer;
  curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "BuffrX");
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(kShortLinkTimeoutMs));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   +[](char* data, size_t size, size_t count, void* user) -> size_t {
                     auto* text = static_cast<std::string*>(user);
                     const size_t bytes = size * count;
                     if (text->size() + bytes > 8192) return 0;
                     text->append(data, bytes);
                     return bytes;
                   });
  const CURLcode code = curl_easy_perform(curl);
  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (code != CURLE_OK) {
    failure = (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT) ? kNoConnection
              : (code == CURLE_OPERATION_TIMEDOUT ? kTimedOut : kUnavailable);
    return false;
  }
  if (http_status != 200) {
    retry = http_status >= 500;
    failure = kUnavailable;
    return false;
  }
  Json reply;
  std::string error;
  const std::string candidate =
      Json::parse(answer, reply, error) && reply.is_object() ? reply["url"].as_string() : std::string();
  const size_t origin_end = endpoint.find('/', endpoint.find("://") + 3);
  const std::string origin = endpoint.substr(0, origin_end);
  const size_t slash = candidate.rfind(origin + "/s/", 0);
  if (slash != 0 || candidate.size() < origin.size() + 6 || candidate.size() > 200) {
    retry = false;
    failure = kUnavailable;
    return false;
  }
  url = candidate;
  return true;
}
#endif
}  // namespace

const char kShortLinkEndpoint[] = "https://buffrx.app/api/shorten";

std::string link_fragment(const std::string& url) {
  const size_t hash = url.find('#');
  if (hash == std::string::npos || hash + 1 >= url.size()) return std::string();
  return url.substr(hash + 1);
}

std::string revoke_endpoint(const std::string& shorten_endpoint) {
  const size_t path = shorten_endpoint.rfind("/api/shorten");
  if (path == std::string::npos) return std::string();
  std::string out = shorten_endpoint;
  out.replace(path, std::string::npos, "/api/revoke");
  return out;
}

ShortLink shorten_link(const std::string& long_url, int64_t expires) {
  ShortLink result;
  result.url = long_url;
  // A link page link is https and carries its ticket after the '#'. Anything else is handed back
  // untouched: it is not something the service could resolve.
  const std::string fragment = link_fragment(long_url);
  if (fragment.empty() || fragment.size() > 400 || long_url.rfind("https://", 0) != 0) return result;
  // A probe may point the attempt at a loopback service and at nothing else; every other run uses the
  // product's own endpoint.
  const std::string endpoint = service_endpoint();
  const std::string body =
      "{\"fragment\":" + quoted(fragment) + ",\"expires\":" + std::to_string(expires) + "}";

  // The long link was complete before this began: the sheet must not wait on the service. The attempt
  // runs on a thread of its own and this answers after kShortLinkAnswerMs whatever WinHTTP is doing (a
  // network that drops packets instead of refusing them can hold every phase to its own timeout). One
  // retry covers a blip; its result is dropped when the sheet has already been answered.
  struct Outcome {
    std::string url;
    const char* failure = kUnavailable;
  };
  auto outcome = std::make_shared<std::promise<Outcome>>();
  std::future<Outcome> answered = outcome->get_future();
  std::thread worker;
  try {
    worker = std::thread([outcome, endpoint, body] {
      Outcome made;
      for (int tries = 0; tries < 2; ++tries) {
        bool retry = true;
        if (attempt(endpoint, body, made.url, made.failure, retry) || !retry) break;
        // A timeout has already spent the sheet's whole budget; a second 1.5 s wait only piles up work.
        if (made.failure == kTimedOut) break;
      }
      outcome->set_value(made);
    });
  } catch (...) {
    result.note = kUnavailable;  // no thread to spare: the long link is the answer
    return result;
  }
  worker.detach();
  if (answered.wait_for(std::chrono::milliseconds(kShortLinkAnswerMs)) != std::future_status::ready) {
    result.note = kTimedOut;
    return result;
  }
  const Outcome made = answered.get();
  if (made.url.empty()) {
    result.note = made.failure;
  } else {
    result.url = made.url;
  }
  return result;
}

// Best effort: the code behind a stopped link is deleted by the service, with the ticket as the only proof
// of ownership. Nothing waits for the answer and nothing is reported; a code that survives expires with the
// link it points at. The answer is not a URL, which is why `attempt` is only used for its request.
void revoke_short_link(const std::string& fragment) {
  if (fragment.empty() || fragment.size() > 400) return;
  const std::string endpoint = revoke_endpoint(service_endpoint());
  if (endpoint.empty()) return;  // not this service's address: nothing to ask
  const std::string body = "{\"fragment\":" + quoted(fragment) + "}";
  try {
    std::thread([endpoint, body] {
      std::string ignored;
      const char* failure = kUnavailable;
      bool retry = false;
      attempt(endpoint, body, ignored, failure, retry);
    }).detach();
  } catch (...) {
    // No thread to spare: the row expires on its own.
  }
}

}  // namespace bx
