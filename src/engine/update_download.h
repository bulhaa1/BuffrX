// Fetching the installer and proving it is the file GitHub published. The hash is computed while the bytes
// land, so a tampered or truncated download never exists on disk under its final name.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bx {

// Lower-case hex of the SHA-256 of these bytes.
std::string sha256_hex_of(std::string_view bytes);

// Lower-case hex of a file's SHA-256, or "" with `error` when it cannot be read. The apply path re-checks
// the installer with this before anything is run.
std::string sha256_file(const std::string& path, std::string& error);

// "sha256:<hex>" from the API against a computed hex digest: the prefix is optional and the case is not
// significant. False whenever either side is empty.
bool digest_text_equals(const std::string& digest, const std::string& hex);

struct DownloadOutcome {
  bool ok = false;
  std::string path;
  std::string error;
};

// Bytes written so far and the size the server declared (0 when it declared none). Called from the download
// thread, never the window thread.
using DownloadProgress = void (*)(void* context, int64_t received, int64_t total);

// Worker only. Streams `url` (following redirects, never from https down to http) into `dest_path`, hashes
// what it wrote, and keeps the file only when the digest matches. A mismatch deletes it.
// A failed attempt can leave an older file at dest_path. Callers must use outcome.ok and outcome.path,
// never the existence of dest_path, and must revalidate the file when they are ready to execute it.
// `timeout_ms` is the whole attempt's budget (an installer needs minutes; a feed answer seconds). `cancel`
// is checked between reads, so a quit does not wait for a 24 MB body.
DownloadOutcome download_verified(const std::string& url, const std::string& digest, const std::string& dest_path,
                                  int timeout_ms = 15000, DownloadProgress progress = nullptr, void* context = nullptr,
                                  const std::atomic<bool>* cancel = nullptr);

}  // namespace bx
