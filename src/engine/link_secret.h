// The two credentials links.json keeps beside a link's token: the short link the service handed back, and
// the ticket that link was shortened from. The short link is a bearer capability of its own - the service
// resolves it to the clip, so whoever holds it can fetch - and it carries a code, not that ticket. The
// ticket is kept because it is the proof the service drops the code by when the link is stopped. The file
// must hand over neither, so both are sealed with the same account-bound protection as the token
// (protect_secret).
//
// The seal sits behind a prefix. A value without it is the plaintext an older build wrote: it still
// opens, so a live link survives the upgrade, and it is sealed by the next write of the file. A value
// with the prefix is never read as plaintext, even when it does not open on this account (a copied
// links.json), because the sealed text is not the credential.
#pragma once

#include <string>

namespace bx {

// Written before every sealed value. Neither plaintext an older build wrote can begin with it: a short
// link begins with its scheme and a ticket holds no whitespace and, in practice, no colon.
constexpr const char* kShortSecretPrefix = "dpapi:";

// What links.json holds for one of those two fields.
enum class ShortSecret {
  Missing,  // absent or empty
  Legacy,   // plaintext from an older build: usable, and sealed on the next write
  Sealed,   // sealed to this Windows account, and it opened
  Broken,   // neither a sealed value that opens nor the plaintext shape a build wrote
};

// The shape of the plaintext an older build wrote. `fragment` picks the ticket's rules over the short
// link's (an http(s) address). Anything else is not one of those two credentials, so it is dropped
// rather than handed to the service or the sheet as if it were.
inline bool legacy_short_secret_ok(const std::string& text, bool fragment) {
  if (text.empty()) return false;
  if (fragment) {
    if (text.size() > 400) return false;  // revoke_short_link refuses more, so this was never a ticket
    if (text.find("://") != std::string::npos) return false;  // an address is not the ticket
  } else {
    if (text.size() < 8 || text.size() > 1024) return false;
    if (text.rfind("https://", 0) != 0 && text.rfind("http://", 0) != 0) return false;
  }
  // No whitespace and no control or non-ASCII byte: a ticket or an address is printable ASCII.
  for (unsigned char c : text) {
    if (c < 0x21 || c > 0x7e) return false;
  }
  return true;
}

// What a stored value claims to be, without opening it.
inline ShortSecret classify_short_secret(const std::string& stored, bool fragment) {
  if (stored.empty()) return ShortSecret::Missing;
  if (stored.rfind(kShortSecretPrefix, 0) == 0) return ShortSecret::Sealed;
  return legacy_short_secret_ok(stored, fragment) ? ShortSecret::Legacy : ShortSecret::Broken;
}

// A sealed value without its prefix, which is what unprotect_secret takes. "" when it is not sealed.
inline std::string sealed_short_payload(const std::string& stored) {
  const size_t prefix = std::char_traits<char>::length(kShortSecretPrefix);
  if (stored.rfind(kShortSecretPrefix, 0) != 0) return std::string();
  return stored.substr(prefix);
}

// `value` sealed for links.json. "" when protecting it failed: the caller then leaves the file as it is,
// exactly as it does for a token that would not seal. Implemented with the rest of links.json in link.cpp.
std::string seal_short_secret(const std::string& value);

// What to keep from `stored`: a sealed value that opens on this account, or the plaintext an older build
// wrote. `value` is written only when the result is Sealed or Legacy.
ShortSecret open_short_secret(const std::string& stored, bool fragment, std::string& value);

}  // namespace bx
