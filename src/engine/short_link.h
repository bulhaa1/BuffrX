#pragma once

#include <cstdint>
#include <string>

namespace bx {

// Where a short link is made: BuffrX's own link service on buffrx.app. Every failure falls back to the long
// link, which is complete before the attempt begins, so a service that is down costs the share nothing.
// BUFFRX_SHORTEN_URL may point the attempt at a loopback service for a probe, and at nothing else.
extern const char kShortLinkEndpoint[];

// One WinHTTP phase waits this long. The sheet never waits more than kShortLinkAnswerMs for the answer,
// so a service that stalls costs the long link, never the click.
constexpr int kShortLinkTimeoutMs = 1500;
// How long the sheet waits before the long link is the answer. The attempt finishes on its own thread;
// a network that drops packets instead of refusing them can hold every WinHTTP phase to its timeout.
constexpr int kShortLinkAnswerMs = 2000;

struct ShortLink {
  std::string url;   // the short link, or `long_url` unchanged when the attempt failed
  std::string note;  // empty on success; on failure the line the share sheet shows
};

// The ticket a link page link carries: the text after its '#', exactly as the service stores it. "" when
// the link has none. Every call to the service is about this text, never about the whole address.
std::string link_fragment(const std::string& url);

// The address that drops a code: the shorten endpoint's own host with `revoke` in place of `shorten`.
// "" when the address is not the service's, so nothing is asked of a host that never made a code.
std::string revoke_endpoint(const std::string& shorten_endpoint);

// `long_url` must be a complete https link page link. `expires` is its unix expiry, so the service can
// drop the row on its own. Returns `long_url` unchanged when the answer is not the service's own `/s/`
// link, or does not arrive within kShortLinkAnswerMs.
ShortLink shorten_link(const std::string& long_url, int64_t expires);

// Asks the service to drop the code made for `fragment`, a link that is being stopped. Best effort and
// never waited on: a code that survives expires with the link it points at.
void revoke_short_link(const std::string& fragment);
}  // namespace bx
