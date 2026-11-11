// The release feed: what GitHub says the newest BuffrX is, read out of the release payload. The parse is
// pure and tested; the fetch is one WinHTTP GET on a worker, never on the window thread.
#pragma once

#include <string>

namespace bx {

// The one asset name every release must carry, so releases/latest/download keeps working.
inline constexpr char kUpdateAssetName[] = "BuffrX-Setup.exe";

struct UpdateFeed {
  bool ok = false;
  std::string latest;  // tag_name, as published ("0.6.0")
  std::string notes;   // the release body, markdown as GitHub stores it
  std::string url;     // the asset's browser_download_url, empty when the release carries no installer
  std::string digest;  // "sha256:<hex>" exactly as GitHub publishes it, empty when absent
  std::string error;   // the line Settings shows; empty on success
};

// Pure: no network. A payload that is not a release object gives ok == false. A release without the
// installer asset is a real release (ok == true) with an empty url and an error naming the miss.
UpdateFeed parse_update_feed(const std::string& payload, const std::string& asset_name = kUpdateAssetName);

// The endpoint. Overridable only for a probe, and only to a loopback address.
std::string update_feed_url();

// True for http/https on 127.0.0.1 or localhost with a numeric or empty port. The one place a probe may
// point the product at something other than the real host.
bool is_loopback_url(const std::string& url);

// Worker only. Follows no redirects for the API; caps the body and the wait. The user agent is "BuffrX"
// unless the caller names a version, which GitHub prefers.
UpdateFeed fetch_update_feed(const std::string& url, int timeout_ms = 15000, const std::string& user_agent = "BuffrX");

}  // namespace bx
