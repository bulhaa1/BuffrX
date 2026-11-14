// Updates on Linux: the release feed works (it is the same JSON), but there is no installer to run, so
// the download and apply paths answer with the honest reason instead of pretending.
#include "../engine/update_download.h"
#include "../engine/update_apply.h"

#include <openssl/evp.h>

#include <cstdio>
#include <string>
#include <vector>

namespace bx {

std::string sha256_hex_of(std::string_view bytes) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (!context) return {};
  std::string hex;
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
      EVP_DigestFinal_ex(context, digest, &length) == 1) {
    static const char* digits = "0123456789abcdef";
    for (unsigned int i = 0; i < length; ++i) {
      hex.push_back(digits[digest[i] >> 4]);
      hex.push_back(digits[digest[i] & 0xF]);
    }
  }
  EVP_MD_CTX_free(context);
  return hex;
}

std::string sha256_file(const std::string& path, std::string& error) {
  error.clear();
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    error = "the file could not be opened";
    return {};
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  char buffer[64 * 1024];
  size_t got = 0;
  while (ok && (got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    ok = EVP_DigestUpdate(context, buffer, got) == 1;
  if (ok && std::ferror(file)) ok = false;
  std::fclose(file);
  if (ok) ok = EVP_DigestFinal_ex(context, digest, &length) == 1;
  if (context) EVP_MD_CTX_free(context);
  if (!ok) {
    error = "the file could not be hashed";
    return {};
  }
  static const char* digits = "0123456789abcdef";
  std::string hex;
  for (unsigned int i = 0; i < length; ++i) {
    hex.push_back(digits[digest[i] >> 4]);
    hex.push_back(digits[digest[i] & 0xF]);
  }
  return hex;
}

bool digest_text_equals(const std::string& digest, const std::string& hex) {
  if (digest.empty() || hex.empty()) return false;
  std::string expected = digest;
  if (expected.rfind("sha256:", 0) == 0) expected = expected.substr(7);
  if (expected.size() != hex.size()) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(expected[i])) !=
        std::tolower(static_cast<unsigned char>(hex[i])))
      return false;
  }
  return true;
}

DownloadOutcome download_verified(const std::string&, const std::string&, const std::string&, int, DownloadProgress,
                                  void*, const std::atomic<bool>*) {
  DownloadOutcome outcome;
  outcome.error = "automatic updates are not available in the Linux build; download the AppImage from the releases page";
  return outcome;
}

InstallOutcome spawn_update_installer(const std::string&, const std::string&, bool) {
  InstallOutcome outcome;
  outcome.error = "installing an update automatically is not available in the Linux build";
  return outcome;
}

int run_update_apply(const std::string&, const std::string&, const std::string&, bool) { return 1; }

}  // namespace bx
