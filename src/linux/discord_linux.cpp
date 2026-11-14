#include "../engine/discord_webhook.h"

namespace bx {
WebhookPost discord_webhook_post(const std::string&, const std::string&, const std::string&,
                                 const std::function<void(double)>&, const std::atomic<bool>*) {
  WebhookPost post;
  post.error = "posting to Discord is not available in the Linux build yet";
  return post;
}
}  // namespace bx
