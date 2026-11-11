#include "link_math.h"

#include <cctype>
#include <cstring>
#include <vector>

namespace bx {
namespace {

const char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr size_t kFixed = 1 + 32 + 16 + 16 + 4 + 1;  // everything before the relay URL
constexpr size_t kMaxRelay = 200;
constexpr uint8_t kVersion = 2;  // 1 wrote every relay out in full; no link of that shape ever shipped

// A relay URL is one of iroh's own domains, so "~" + a code + the subdomain stands for
// "https://<subdomain>.<domain>./" and saves 28 of the 36 bytes it would otherwise take. Anything
// that does not match one of these is written out whole, so a relay from another network still works.
struct RelayDomain {
  char code;
  const char* domain;
};
constexpr RelayDomain kRelayDomains[] = {
    {'1', "relay.n0.iroh.link"},
    {'2', "relay.iroh.network"},
};

std::string compact_relay(const std::string& relay) {
  if (relay.size() <= 8 || relay.compare(0, 8, "https://") != 0 || relay.size() > kMaxRelay) return relay;
  std::string body = relay.substr(8);
  while (!body.empty() && (body.back() == '/' || body.back() == '.')) body.pop_back();
  for (const RelayDomain& known : kRelayDomains) {
    const std::string suffix = std::string(".") + known.domain;
    if (body.size() > suffix.size() &&
        body.compare(body.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return std::string("~") + known.code + body.substr(0, body.size() - suffix.size());
    }
  }
  return relay;
}

std::string expand_relay(const std::string& relay) {
  if (relay.size() < 3 || relay[0] != '~') return relay;
  for (const RelayDomain& known : kRelayDomains) {
    if (relay[1] == known.code) return "https://" + relay.substr(2) + "." + known.domain + "./";
  }
  return std::string();  // a code this build does not know: no relay is safer than a wrong one
}

std::string base64url(const std::vector<uint8_t>& data) {
  std::string out;
  uint32_t buffer = 0;
  int bits = 0;
  for (uint8_t byte : data) {
    buffer = (buffer << 8) | byte;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += kBase64Url[(buffer >> bits) & 63];
    }
  }
  if (bits > 0) out += kBase64Url[(buffer << (6 - bits)) & 63];
  return out;
}

bool from_base64url(const std::string& text, std::vector<uint8_t>& out) {
  out.clear();
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    const char* at = c ? std::strchr(kBase64Url, c) : nullptr;
    if (!at) return false;
    buffer = (buffer << 6) | static_cast<uint32_t>(at - kBase64Url);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>(buffer >> bits));
    }
  }
  return true;
}

bool url_chars(const std::string& text) {
  for (unsigned char c : text) {
    if (!std::isalnum(c) && std::strchr(".-_~:/%", c) == nullptr) return false;
  }
  return true;
}

bool relay_valid(const std::string& relay) {
  if (relay.empty()) return true;
  return relay.size() <= kMaxRelay && relay.compare(0, 8, "https://") == 0 && relay.size() > 8 && url_chars(relay);
}

std::string trim(const std::string& text) {
  const size_t from = text.find_first_not_of(" \t\r\n");
  if (from == std::string::npos) return std::string();
  const size_t to = text.find_last_not_of(" \t\r\n");
  return text.substr(from, to - from + 1);
}

bool is_payload_text(const std::string& text) {
  return !text.empty() && text.find_first_not_of(kBase64Url) == std::string::npos;
}

// A sender's name sits in the fragment, so a space must be %20 rather than + (URLSearchParams on the
// page would read + as a space, but decodeURIComponent would not).
std::string percent_encode(const std::string& text) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

}  // namespace

std::string base64url_encode(const std::vector<uint8_t>& data) { return base64url(data); }

bool base64url_decode(const std::string& text, std::vector<uint8_t>& out) { return from_base64url(text, out); }

std::string link_payload(const LinkTicket& ticket) {
  std::vector<uint8_t> bytes;
  bytes.reserve(kFixed + ticket.relay.size());
  bytes.push_back(kVersion);
  bytes.insert(bytes.end(), ticket.endpoint, ticket.endpoint + 32);
  bytes.insert(bytes.end(), ticket.share, ticket.share + 16);
  bytes.insert(bytes.end(), ticket.token, ticket.token + 16);
  for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(ticket.expires >> shift));
  const std::string relay =
      ticket.relay.size() <= kMaxRelay ? compact_relay(ticket.relay) : std::string();
  bytes.push_back(static_cast<uint8_t>(relay.size()));
  bytes.insert(bytes.end(), relay.begin(), relay.end());
  return base64url(bytes);
}

bool parse_link_payload(const std::string& payload, LinkTicket& ticket) {
  std::vector<uint8_t> bytes;
  if (payload.size() > 400 || !from_base64url(payload, bytes) || bytes.size() < kFixed ||
      bytes[0] != kVersion) {
    return false;
  }
  const size_t relay_length = bytes[kFixed - 1];
  if (relay_length > kMaxRelay || bytes.size() != kFixed + relay_length) return false;
  LinkTicket parsed;
  std::memcpy(parsed.endpoint, &bytes[1], 32);
  std::memcpy(parsed.share, &bytes[33], 16);
  std::memcpy(parsed.token, &bytes[49], 16);
  parsed.expires = (static_cast<uint32_t>(bytes[65]) << 24) | (static_cast<uint32_t>(bytes[66]) << 16) |
                   (static_cast<uint32_t>(bytes[67]) << 8) | bytes[68];
  const std::string written(bytes.begin() + static_cast<std::ptrdiff_t>(kFixed), bytes.end());
  parsed.relay = expand_relay(written);
  if (!relay_valid(parsed.relay)) return false;
  ticket = parsed;
  return true;
}

std::string link_url(const std::string& page, const LinkTicket& ticket, const std::string& sender) {
  const std::string payload = link_payload(ticket);
  if (page.empty()) return "buffrx://l/" + payload;
  const std::string name = link_sender_name(sender);
  return page + "#" + payload + (name.empty() ? std::string() : "?n=" + percent_encode(name));
}

bool parse_link_url(const std::string& url, LinkTicket& ticket) {
  if (url.size() > 1024) return false;
  std::string text = trim(url);
  // A link copied out of a sentence brings its punctuation along: "(see buffrx://l/…)." None of these
  // characters can be part of a payload, so dropping them never changes a link.
  while (!text.empty() && std::strchr(".,;:!?)]>'\"", text.back()) != nullptr && text.back() != '\0') text.pop_back();
  while (!text.empty() && std::strchr("([<'\"", text.front()) != nullptr && text.front() != '\0') text.erase(0, 1);
  std::string payload;
  if (lower(text.substr(0, 11)) == "buffrx://l/") {
    payload = text.substr(11);
    if (!payload.empty() && payload[0] == '#') payload.erase(0, 1);
  } else if (lower(text.substr(0, 8)) == "https://") {
    const size_t hash = text.find('#');
    if (hash == std::string::npos) return false;
    payload = text.substr(hash + 1);
  } else {
    return false;
  }
  // A link page link can carry the sender's name after the payload (?n=Alex): the page shows it, and
  // it is not part of the ticket, so it is dropped before the payload is read.
  const size_t query = payload.find('?');
  if (query != std::string::npos) payload.resize(query);
  if (!payload.empty() && payload.back() == '/') payload.pop_back();
  return is_payload_text(payload) && parse_link_payload(payload, ticket);
}

std::string link_sender_name(const std::string& name) {
  const size_t from = name.find_first_not_of(" \t\r\n");
  if (from == std::string::npos) return std::string();
  const size_t to = name.find_last_not_of(" \t\r\n");
  std::string out;
  out.reserve(to - from + 1);
  for (size_t i = from; i <= to; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c >= 0x20 && c != 0x7F) out += static_cast<char>(c);  // a pasted name brings no controls along
  }
  if (out.size() > 32) {
    size_t cut = 32;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
  return out;
}

std::string link_page_or_default(const std::string& page) {
  return (!page.empty() && link_page_valid(page)) ? page : std::string(kDefaultLinkPage);
}

bool link_page_valid(const std::string& page) {
  if (page.empty()) return true;
  return page.size() <= 200 && page.compare(0, 8, "https://") == 0 && page.size() > 9 && page.back() == '/' &&
         page.find_first_of("?#@") == std::string::npos && url_chars(page);
}

std::string received_file_name(const std::string& sent) {
  const size_t slash = sent.find_last_of("\\/");
  std::string name = slash == std::string::npos ? sent : sent.substr(slash + 1);
  for (char& c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F || std::strchr("<>:\"|?*", c) != nullptr) c = '_';
  }
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) return std::string();
  const std::string extension = name.substr(dot);
  const std::string kind = lower(extension);
  if (kind != ".mp4" && kind != ".mkv" && kind != ".png" && kind != ".jpg" && kind != ".jpeg") return std::string();
  std::string stem = name.substr(0, dot);
  const size_t from = stem.find_first_not_of(" .");
  stem = from == std::string::npos ? std::string() : stem.substr(from, stem.find_last_not_of(" .") - from + 1);
  if (stem.empty()) stem = "Shared clip";
  // CON, PRN, AUX, NUL, COM1-9 and LPT1-9 are devices whatever follows them.
  const std::string device = lower(stem.substr(0, stem.find('.')));
  const bool reserved = device == "con" || device == "prn" || device == "aux" || device == "nul" ||
                        (device.size() == 4 && (device.compare(0, 3, "com") == 0 || device.compare(0, 3, "lpt") == 0) &&
                         device[3] >= '1' && device[3] <= '9');
  if (reserved) stem = "_" + stem;
  if (stem.size() > 100) {
    size_t cut = 100;
    while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) --cut;
    stem.resize(cut);
  }
  return stem + extension;
}

bool link_expired_here(int64_t expires, int64_t now) {
  return now > expires + kLinkClockGrace;
}

std::string proxy_from_windows(const std::string& setting) {
  std::string plain, http, https;
  size_t at = 0;
  while (at <= setting.size()) {
    size_t end = setting.find_first_of("; ", at);
    if (end == std::string::npos) end = setting.size();
    const std::string entry = setting.substr(at, end - at);
    at = end + 1;
    if (entry.empty()) continue;
    const size_t equals = entry.find('=');
    if (equals == std::string::npos) {
      if (plain.empty()) plain = entry;
    } else {
      const std::string kind = entry.substr(0, equals);
      if (kind == "https") https = entry.substr(equals + 1);
      else if (kind == "http") http = entry.substr(equals + 1);
    }
  }
  std::string chosen = !https.empty() ? https : !http.empty() ? http : plain;
  if (chosen.rfind("http://", 0) == 0) chosen = chosen.substr(7);
  // host:port and nothing else: a name or address, a colon, digits.
  const size_t colon = chosen.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == chosen.size()) return std::string();
  for (size_t i = 0; i < chosen.size(); ++i) {
    const char c = chosen[i];
    const bool ok = i > colon ? (c >= '0' && c <= '9')
                              : (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' ||
                                 c == '[' || c == ']');
    if (!ok) return std::string();
  }
  return "http://" + chosen;
}

std::string hex_encode(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

bool hex_decode(const std::string& text, uint8_t* out, size_t size) {
  if (text.size() != size * 2) return false;
  auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < size; ++i) {
    const int high = value(text[i * 2]);
    const int low = value(text[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    out[i] = static_cast<uint8_t>(high << 4 | low);
  }
  return true;
}

}  // namespace bx
