#include "discord_webhook.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "../shared/files.h"
#include "../shared/json.h"
#include "../shared/log.h"
#include "share_math.h"

namespace bx {
namespace {

std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 32 hex characters from the system's random source: a boundary the file cannot plausibly contain. A
// generator that will not answer fails the post instead: a predictable boundary is not worth posting.
bool make_boundary(std::string& boundary) {
  unsigned char bytes[16] = {};
  const NTSTATUS status = BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) return false;
  static const char hex[] = "0123456789abcdef";
  boundary = "BuffrXShare";
  for (const unsigned char byte : bytes) {
    boundary += hex[byte >> 4];
    boundary += hex[byte & 15];
  }
  return true;
}

struct Handle {
  HINTERNET handle = nullptr;
  ~Handle() {
    if (handle) WinHttpCloseHandle(handle);
  }
};

}  // namespace

WebhookPost discord_webhook_post(const std::string& url, const std::string& file, const std::string& content,
                                 const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
  WebhookPost result;
  if (!discord_webhook_valid(url)) {
    result.error = "the webhook address is not a Discord webhook";
    return result;
  }
  const std::wstring wide_url = utf8_to_wide(url + "?wait=true");
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
  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    result.error = "the webhook address could not be read";
    return result;
  }
  const std::wstring object = std::wstring(path) + extra;

  HANDLE input = CreateFileW(utf8_to_wide(file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (input == INVALID_HANDLE_VALUE) {
    result.error = "the clip could not be opened";
    return result;
  }
  struct FileOwner {
    HANDLE handle;
    ~FileOwner() { CloseHandle(handle); }
  } file_owner{input};
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(input, &size) || size.QuadPart <= 0) {
    result.error = "the clip is empty";
    return result;
  }

  std::string boundary;
  if (!make_boundary(boundary)) {
    result.error = "the system's random source is unavailable";
    return result;
  }
  const std::string head = multipart_head(boundary, discord_payload_json(content), base_name(file));
  const std::string tail = multipart_tail(boundary);
  const uint64_t total = head.size() + static_cast<uint64_t>(size.QuadPart) + tail.size();
  if (total > 0xFFFFFFFFull) {
    result.error = "the clip is too large to post";
    return result;
  }

  Handle session;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
  session.handle = WinHttpOpen(L"BuffrX", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
  if (!session.handle) {
    session.handle = WinHttpOpen(L"BuffrX", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session.handle) {
    result.error = "Windows' HTTP service would not start";
    return result;
  }
  // TLS 1.2 or newer only, and no redirect: an answer that points elsewhere is an error, never a second
  // place the clip goes.
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  WinHttpSetOption(session.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  WinHttpSetTimeouts(session.handle, 15000, 15000, 60000, 60000);

  Handle connection;
  connection.handle = WinHttpConnect(session.handle, host, parts.nPort, 0);
  Handle request;
  if (connection.handle) {
    request.handle = WinHttpOpenRequest(connection.handle, L"POST", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  }
  if (!request.handle) {
    result.error = "Discord could not be reached";
    return result;
  }
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));

  const std::wstring headers = L"Content-Type: multipart/form-data; boundary=" + utf8_to_wide(boundary);
  if (!WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0,
                          static_cast<DWORD>(total), 0)) {
    result.error = GetLastError() == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "Discord could not be reached (no internet?)"
                                                                     : "the connection to Discord failed";
    return result;
  }
  auto write = [&](const char* data, DWORD length) {
    DWORD written = 0;
    return WinHttpWriteData(request.handle, data, length, &written) && written == length;
  };
  if (!write(head.data(), static_cast<DWORD>(head.size()))) {
    result.error = "the upload to Discord stopped";
    return result;
  }
  std::vector<char> buffer(256 * 1024);
  uint64_t sent = 0;
  double reported = -1.0;
  while (true) {
    if (cancel && cancel->load()) {
      result.error = "cancelled";
      return result;
    }
    DWORD read = 0;
    if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      result.error = "the clip could not be read";
      return result;
    }
    if (read == 0) break;
    if (!write(buffer.data(), read)) {
      result.error = "the upload to Discord stopped";
      return result;
    }
    sent += read;
    const double share = static_cast<double>(sent) / static_cast<double>(size.QuadPart);
    if (progress && (share >= 1.0 || share - reported >= 0.01)) {
      reported = share;
      progress(share);
    }
  }
  if (!write(tail.data(), static_cast<DWORD>(tail.size())) || !WinHttpReceiveResponse(request.handle, nullptr)) {
    result.error = "Discord did not confirm the post - check Discord before retrying";
    return result;
  }
  DWORD status = 0, status_size = sizeof(status);
  WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &status_size, WINHTTP_NO_HEADER_INDEX);
  result.status = static_cast<int>(status);
  std::string body;
  for (DWORD available = 0; WinHttpQueryDataAvailable(request.handle, &available) && available > 0 && body.size() < 65536;) {
    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request.handle, chunk.data(), available, &read) || read == 0) break;
    body.append(chunk.data(), read);
  }
  if (status >= 200 && status < 300) {
    result.ok = true;
    return result;
  }
  Json answer;
  std::string parse_error;
  const bool parsed = Json::parse(body, answer, parse_error);
  if (status == 429) {
    result.retry_after = parsed ? answer["retry_after"].as_double(1.0) : 1.0;
    result.error = "Discord asked to wait before posting again";
  } else if (status == 413) {
    result.error = "Discord said the clip is too large for this channel";
  } else if (status == 401 || status == 403 || status == 404) {
    result.error = "Discord no longer knows this webhook - it was deleted or the address is wrong";
  } else {
    result.error = "Discord refused the post (HTTP " + std::to_string(status) + ")";
  }
  BX_LOG_WARN("discord post failed: HTTP %lu, code %lld", static_cast<unsigned long>(status),
              static_cast<long long>(parsed ? answer["code"].as_int(0) : 0));
  return result;
}

}  // namespace bx
