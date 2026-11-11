// BuffrX links, as text: what a link carries, how it is written and read back, and the name a received
// clip is saved under. No network, no file: everything here is checked by tests/engine_tests.cpp.
//
// A link is https://<link page>/#<payload> (the page hands it to BuffrX) or buffrx://l/<payload> (what
// Windows passes to BuffrX). The payload is base64url of:
//   version (1) | endpoint id (32) | share id (16) | token (16) | expires, unix seconds (4, big-endian) |
//   relay length (1) | relay (0-200)
// Version 2 writes a relay on one of iroh's own domains as "~" + code + subdomain, which is 8 bytes
// instead of 36; version 1 (still read) writes it out in full. Either way the relay field is https only,
// and a link with no relay at all is valid: DNS discovery finds the peer.
// The endpoint id is the sharer's public key; the share id and token are random; together they are
// 256 bits a guesser would have to find. The link carries no IP address, ever: peers learn each
// other's address only while connected, and only if a direct path works.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace bx {

// Whether a link read from the Windows clipboard is worth offering to the person: one this PC wrote
// itself (the panel, the Share sheet or the Timeline page) comes back on the next frame, and offering
// it would pop a window over the game for a link they just made.
inline bool link_offer_wanted(const std::string& text, const std::string& offered, const std::string& copied,
                              const std::string& just_copied) {
  return !text.empty() && text != offered && text != copied && text != just_copied;
}

struct LinkTicket {
  uint8_t endpoint[32] = {};
  uint8_t share[16] = {};
  uint8_t token[16] = {};
  uint32_t expires = 0;  // unix seconds
  std::string relay;     // https://… or empty
};

std::string link_payload(const LinkTicket& ticket);
// Only what a poster needs: base64url both ways, so the helper can carry the bytes without knowing what
// they are.
std::string base64url_encode(const std::vector<uint8_t>& data);
bool base64url_decode(const std::string& text, std::vector<uint8_t>& out);
// False for anything that is not exactly a version-1 payload with a sane relay URL.
bool parse_link_payload(const std::string& payload, LinkTicket& ticket);

// The link to copy: `page` + "#" + payload, or buffrx://l/<payload> when no link page is set. With a
// page, the sender's name rides after the payload as "?n=<name>" (display only: it is not part of the
// ticket, and a buffrx:// link never carries it).
std::string link_url(const std::string& page, const LinkTicket& ticket, const std::string& sender = std::string());
// Reads a link back from what a click or a paste hands over: buffrx://l/<payload>, buffrx://l/#<payload>,
// or any https:// address whose fragment is a payload (a "?n=" sender name after it is ignored).
// Surrounding spaces, and the punctuation a link copied out of a sentence brings along, are ignored;
// nothing else is.
bool parse_link_url(const std::string& url, LinkTicket& ticket);

// The sender's name as a link page may show it: trimmed, no control characters, at most 32 bytes
// without splitting a UTF-8 character. "" when nothing is left. The page displays it as plain text;
// it is self-declared, never proof of who sent the clip.
std::string link_sender_name(const std::string& name);

// Whether a link page address is acceptable: https, no fragment, no query, no credentials, 200 bytes at
// most, ending in "/". Empty is acceptable here (the caller decides what empty means).
bool link_page_valid(const std::string& page);

// What a link should actually point at: the configured page when it is valid, the default when the
// setting is empty or malformed. Never empty, so a share is always an https:// link.
std::string link_page_or_default(const std::string& page);

// The name a received file is saved under. The sender chose it, so it is treated as hostile: no path,
// no characters Windows forbids, no device names, no leading or trailing dots and spaces, at most 100
// bytes before the extension (never splitting a UTF-8 character), and only the extensions BuffrX makes
// (.mp4, .mkv, .png, .jpg, .jpeg). "" when the extension is anything else.
std::string received_file_name(const std::string& sent);

// The sharer's PC decides when a link has expired. This PC turns a link away by itself only when it is
// more than this far past its end, so a receiver whose clock runs ahead still gets to ask.
constexpr int64_t kLinkClockGrace = 2 * 60 * 60;
bool link_expired_here(int64_t expires, int64_t now);

// Windows' proxy setting (WinHTTP's IE configuration: "host:port", "http=h:p;https=h:p", ...) as one
// http://host:port for the relay connection, preferring the https entry. "" for none, SOCKS-only, or a
// value that is not a host:port.
std::string proxy_from_windows(const std::string& setting);

std::string hex_encode(const uint8_t* data, size_t size);
bool hex_decode(const std::string& text, uint8_t* out, size_t size);

}  // namespace bx
