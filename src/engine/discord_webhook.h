// Posting a clip to a Discord channel through the webhook the user pasted: one HTTPS request to
// discord.com, sent only when the user presses Share (AGENTS.md, Private). WinHTTP, so TLS is Windows'
// own (Schannel) and nothing new is linked.
//
// The address is checked again here (discord_webhook_valid) whatever the caller did, redirects are
// refused, so the clip can never be sent anywhere but Discord, and the address is never logged.
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace bx {

struct WebhookPost {
  bool ok = false;
  int status = 0;              // the HTTP status, 0 when no answer came
  double retry_after = 0.0;    // seconds, when Discord said 429
  std::string error;           // a sentence for the interface; never carries the address
};

// Streams `file` as the message's one attachment with `content` as its text. `progress` gets 0..1 of
// the bytes sent; `cancel`, when it turns true, stops the upload between two writes.
WebhookPost discord_webhook_post(const std::string& url, const std::string& file, const std::string& content,
                                 const std::function<void(double)>& progress, const std::atomic<bool>* cancel);

}  // namespace bx
