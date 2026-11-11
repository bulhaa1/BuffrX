#include "update_download.h"

#include "update_feed.h"
#include "update_http.h"

#include <windows.h>
#include <winhttp.h>

#include <bcrypt.h>

#include <cstdio>
#include <algorithm>
#include <vector>

namespace bx {
namespace {

std::wstring wide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

std::string lowered(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

std::string hex_of(const unsigned char* bytes, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0F]);
  }
  return out;
}

// One SHA-256 over a stream of buffers. Kept as a tiny type so the download, the file check and the tests
// all hash the same way.
struct Sha256 {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<unsigned char> object;
  bool ready = false;

  Sha256() {
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
    DWORD length = 0, produced = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length),
                          &produced, 0) != 0) {
      return;
    }
    object.resize(length);
    if (BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) != 0) return;
    ready = true;
  }

  ~Sha256() {
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  }

  bool add(const void* data, size_t size) {
    if (!ready) return false;
    if (size > 0 && BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                                   static_cast<ULONG>(size), 0) != 0) ready = false;
    return ready;
  }

  bool finish(std::string& hex) {
    if (!ready) return false;
    unsigned char digest[32] = {0};
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) return false;
    hex = hex_of(digest, sizeof(digest));
    return true;
  }
};

}  // namespace

std::string sha256_hex_of(std::string_view bytes) {
  Sha256 hasher;
  std::string hex;
  if (!hasher.ready) return hex;
  if (!hasher.add(bytes.data(), bytes.size())) return {};
  hasher.finish(hex);
  return hex;
}

std::string sha256_file(const std::string& path, std::string& error) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    error = "the update could not be opened";
    return {};
  }
  Sha256 hasher;
  std::vector<char> buffer(64 * 1024);
  size_t got = 0;
  while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
    if (!hasher.add(buffer.data(), got)) {
      std::fclose(file);
      error = "the update could not be checked";
      return {};
    }
  }
  if (std::ferror(file)) {
    std::fclose(file);
    error = "the update could not be read";
    return {};
  }
  std::fclose(file);
  std::string hex;
  if (!hasher.finish(hex)) {
    error = "the update could not be checked";
    return {};
  }
  return hex;
}

bool digest_text_equals(const std::string& digest, const std::string& hex) {
  if (digest.empty() || hex.empty()) return false;
  std::string left = lowered(digest);
  if (left.rfind("sha256:", 0) == 0) left.erase(0, 7);
  else if (left.find(':') != std::string::npos) return false;  // another algorithm is not ours
  return !left.empty() && left == lowered(hex);
}

static DownloadOutcome download_attempt(const std::string& url, const std::string& digest, const std::string& dest_path,
                                        int timeout_ms, ULONGLONG deadline, DownloadProgress progress, void* context,
                                        const std::atomic<bool>* cancel) {
  DownloadOutcome outcome;
  if (digest.empty()) {
    outcome.error = "the release published no digest";
    return outcome;
  }

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
    outcome.error = "the download address is not https";
    return outcome;
  }

  const std::wstring object = std::wstring(path) + extra;
  // GitHub serves assets from another host: follow that, but never step down from https. A local
  // HTTP probe cannot redirect to another host.
  DWORD redirects = parts.nScheme == INTERNET_SCHEME_HTTP ? WINHTTP_OPTION_REDIRECT_POLICY_NEVER
                                                          : WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  UpdateHttp request;
  if (!request.open(host, parts.nPort, object.c_str(), parts.nScheme == INTERNET_SCHEME_HTTPS,
                    L"BuffrX", redirects, timeout_ms, deadline)) {
    outcome.error = "could not open the download";
    return outcome;
  }

  DWORD status = 0;
  if (!request.send(nullptr) || !request.receive(status)) {
    outcome.error = "the download did not answer";
    return outcome;
  }
  if (status != 200) {
    outcome.error = "the download answered " + std::to_string(status);
    return outcome;
  }
  int64_t declared = 0;
  request.content_length(declared);

  const std::string partial = dest_path + ".partial";
  std::FILE* file = std::fopen(partial.c_str(), "wb");
  if (file == nullptr) {
    outcome.error = "the update could not be written";
    return outcome;
  }

  Sha256 hasher;
  std::vector<char> buffer(64 * 1024);
  DWORD read = 0;
  uint64_t total = 0;
  bool received = true;
  while (true) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      outcome.error = "the update was cancelled";
      received = false;
      break;
    }
    if (!request.read(buffer.data(), static_cast<DWORD>(buffer.size()), read)) {
      outcome.error = "the download was interrupted";
      received = false;
      break;
    }
    if (read == 0) break;
    total += read;
    if (total > 512ULL * 1024 * 1024 || std::fwrite(buffer.data(), 1, read, file) != read ||
        !hasher.add(buffer.data(), read)) {
      outcome.error = total > 512ULL * 1024 * 1024 ? "the update was too large" : "the update could not be written or checked";
      received = false;
      break;
    }
    if (progress != nullptr) progress(context, static_cast<int64_t>(total), declared);
  }
  if (std::fclose(file) != 0) {
    outcome.error = "the update could not be written";
    received = false;
  }
  if (!received) {
    DeleteFileA(partial.c_str());
    return outcome;
  }

  std::string hex;
  if (!hasher.finish(hex) || !digest_text_equals(digest, hex)) {
    DeleteFileA(partial.c_str());
    outcome.error = "the file does not match the release";
    return outcome;
  }
  // Hash the closed file too: a buffered write can fail after fwrite accepted the input.
  file = std::fopen(partial.c_str(), "rb");
  Sha256 disk_hasher;
  bool disk_ok = file != nullptr;
  if (file) {
    size_t got = 0;
    while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
      if (GetTickCount64() >= deadline || !disk_hasher.add(buffer.data(), got)) { disk_ok = false; break; }
    }
    if (std::ferror(file)) disk_ok = false;
    if (std::fclose(file) != 0) disk_ok = false;
  }
  std::string disk_hex;
  if (!disk_ok || GetTickCount64() >= deadline || !disk_hasher.finish(disk_hex) ||
      !digest_text_equals(digest, disk_hex)) {
    DeleteFileA(partial.c_str());
    outcome.error = "the written update did not check out";
    return outcome;
  }
  if (!MoveFileExA(partial.c_str(), dest_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileA(partial.c_str());
    outcome.error = "the verified update could not be put in place";
    return outcome;
  }
  outcome.ok = true;
  outcome.path = dest_path;
  return outcome;
}

DownloadOutcome download_verified(const std::string& url, const std::string& digest, const std::string& dest_path,
                                  int timeout_ms, DownloadProgress progress, void* context,
                                  const std::atomic<bool>* cancel) {
  // The budget covers the whole attempt, not each read: a 24 MB installer over a slow line still finishes,
  // and a server that trickles is still cut off.
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(1000, timeout_ms));
  DownloadOutcome outcome = download_attempt(url, digest, dest_path, timeout_ms, deadline, progress, context, cancel);
  if (!outcome.ok && GetTickCount64() < deadline &&
      (outcome.error == "the download did not answer" || outcome.error == "the download was interrupted")) {
    outcome = download_attempt(url, digest, dest_path, timeout_ms, deadline, progress, context, cancel);
  }
  return outcome;
}

}  // namespace bx
