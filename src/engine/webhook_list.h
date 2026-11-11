// Ordered, named Discord destinations. The address stays sealed; the id is not the token.
#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace bx {

struct WebhookEntry {
  std::string id;
  std::string name;
  std::string sealed;
};

inline const WebhookEntry* webhook_find(const std::vector<WebhookEntry>& hooks, const std::string& id) {
  if (id.empty()) return hooks.empty() ? nullptr : &hooks.front();
  for (const auto& hook : hooks) if (hook.id == id) return &hook;
  return nullptr;
}

inline void webhook_upsert(std::vector<WebhookEntry>& hooks, WebhookEntry entry) {
  for (auto& hook : hooks) {
    if (hook.id == entry.id) { hook = std::move(entry); return; }
  }
  hooks.push_back(std::move(entry));
}

inline bool webhook_remove(std::vector<WebhookEntry>& hooks, const std::string& id) {
  const auto found = std::find_if(hooks.begin(), hooks.end(), [&](const WebhookEntry& hook) { return hook.id == id; });
  if (found == hooks.end()) return false;
  hooks.erase(found);
  return true;
}

}  // namespace bx
