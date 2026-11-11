// The rail's LIVE LINK pane, decided without a window so tests/engine_tests.cpp can hold it: which links are live, how
// much of each one's life is left, and the two-press Stop. Link.list carries each link's creation time, so the pane sorts
// newest made first; a link from a links.json written before that time was kept (`created` 0) falls back to the order the
// panel first saw links in, where the panel remembers the first `expires - now` it sees for each id and treats first sight
// as the start of its life.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace bx::ui {

struct LinkView {
  std::string id, file, name, url;
  int64_t expires = 0;
  int viewers = 0, watching = 0;
  int64_t created = 0;  // unix seconds, 0 when the engine did not say
};

struct LinkBook {
  std::vector<std::string> ids;       // first seen, oldest first
  std::vector<int64_t> full_seconds;  // parallel: expires - now at first sight
};

struct LinkStopArm {
  std::string id;
  uint32_t until = 0;
};

// Keeps the first-seen order and the full life of every live link. A link that is gone or expired is forgotten; a new
// one is appended, and several first seen in one read are ordered oldest expiry first, so the latest expiry is newest.
inline bool link_book_update(LinkBook& book, const std::vector<LinkView>& links, int64_t now) {
  std::vector<std::string> ids;
  std::vector<int64_t> full_seconds;
  for (size_t i = 0; i < book.ids.size(); ++i) {
    const auto it = std::find_if(links.begin(), links.end(), [&](const LinkView& link) { return link.id == book.ids[i]; });
    if (it == links.end() || it->expires <= now) continue;
    ids.push_back(book.ids[i]);
    full_seconds.push_back(book.full_seconds[i]);
  }
  book.ids = std::move(ids);
  book.full_seconds = std::move(full_seconds);

  std::vector<const LinkView*> added;
  for (const LinkView& link : links) {
    if (link.expires <= now || std::find(book.ids.begin(), book.ids.end(), link.id) != book.ids.end()) continue;
    added.push_back(&link);
  }
  std::sort(added.begin(), added.end(), [](const LinkView* a, const LinkView* b) {
    if (a->expires != b->expires) return a->expires < b->expires;
    return a->id < b->id;
  });
  for (const LinkView* link : added) {
    book.ids.push_back(link->id);
    book.full_seconds.push_back(std::max<int64_t>(1, link->expires - now));
  }
  return !added.empty();
}

// Indices into `links`, newest first, with expired links already gone. A live link with no creation time (any of them)
// makes the whole order fall back to first sight, because the two cannot be mixed honestly.
inline std::vector<size_t> live_link_order(const std::vector<LinkView>& links, const LinkBook& book, int64_t now) {
  std::vector<size_t> out;
  bool any_unknown = false;
  for (size_t i = 0; i < links.size(); ++i) {
    const LinkView& link = links[i];
    if (link.expires <= now) continue;
    const auto at = std::find(book.ids.begin(), book.ids.end(), link.id);
    if (at == book.ids.end()) continue;
    if (link.created == 0) any_unknown = true;
    out.push_back(i);
  }
  if (!any_unknown) {
    std::sort(out.begin(), out.end(), [&](size_t a, size_t b) {
      if (links[a].created != links[b].created) return links[a].created > links[b].created;
      return links[a].id < links[b].id;  // equal times: one fixed order, so a redraw never swaps them
    });
    return out;
  }
  std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
    const auto pa = std::find(book.ids.begin(), book.ids.end(), links[a].id);
    const auto pb = std::find(book.ids.begin(), book.ids.end(), links[b].id);
    return pa > pb;  // first seen later is newer
  });
  return out;
}

inline int64_t link_full_seconds(const LinkBook& book, const std::string& id) {
  const auto at = std::find(book.ids.begin(), book.ids.end(), id);
  return at == book.ids.end() ? 0 : book.full_seconds[static_cast<size_t>(at - book.ids.begin())];
}

inline float link_life_share(int64_t full, int64_t expires, int64_t now) {
  if (full <= 0 || expires <= now) return 0.0f;
  const double share = static_cast<double>(expires - now) / static_cast<double>(full);
  return static_cast<float>(std::clamp(share, 0.0, 1.0));
}

inline std::string link_activity(int watching, int viewers) {
  if (watching > 0) return std::to_string(watching) + " watching";
  return std::to_string(viewers) + (viewers == 1 ? " download" : " downloads");
}

inline size_t link_step(size_t index, size_t count, int delta) {
  if (count == 0) return 0;
  const int64_t n = static_cast<int64_t>(count);
  int64_t at = static_cast<int64_t>(index % count) + delta;
  at %= n;
  if (at < 0) at += n;
  return static_cast<size_t>(at);
}

inline bool link_stop_armed(const LinkStopArm& arm, const std::string& id, uint32_t now_ms) {
  return !arm.id.empty() && arm.id == id && now_ms <= arm.until;
}

// False arms the link; a second press on the same link inside `window_ms` returns true and clears the arm.
inline bool link_stop_confirm(LinkStopArm& arm, const std::string& id, uint32_t now_ms, uint32_t window_ms = 4000) {
  if (id.empty()) {
    arm = {};
    return false;
  }
  if (link_stop_armed(arm, id, now_ms)) {
    arm = {};
    return true;
  }
  arm.id = id;
  arm.until = now_ms + window_ms;
  return false;
}

}  // namespace bx::ui
