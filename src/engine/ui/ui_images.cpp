#include "ui_images.h"

#include <algorithm>

#include "../media.h"

namespace bx::ui {

ImageCache::~ImageCache() { stop(); }

void ImageCache::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (worker_.joinable()) return;
  stopping_ = false;
  worker_ = std::thread([this] { run(); });
}

void ImageCache::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    queue_.clear();
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

ImTextureID ImageCache::get(const std::string& path, int box, ImVec2* size, bool* failed) {
  if (failed) *failed = false;
  if (path.empty()) {
    if (failed) *failed = true;
    return 0;
  }
  const std::string key = path + "|" + std::to_string(box);
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = entries_.find(key);
  if (found == entries_.end()) {
    entries_[key].used = ++clock_;
    // Newest first: what was just scrolled into view matters more than what scrolled past.
    queue_.push_front(Request{key, path, box});
    wake_.notify_all();
    return 0;
  }
  Entry& entry = found->second;
  entry.used = ++clock_;
  if (entry.state == Entry::failed && failed) *failed = true;
  if (entry.state != Entry::ready) return 0;
  if (size) *size = ImVec2(static_cast<float>(entry.width), static_cast<float>(entry.height));
  return entry.texture;
}

void ImageCache::run() {
  while (true) {
    Request request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      request = queue_.front();
      queue_.pop_front();
      // Asked for and scrolled away before its turn: skip it rather than decode what nobody sees.
      auto found = entries_.find(request.key);
      if (found == entries_.end() || found->second.state != Entry::queued) continue;
    }
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    std::string error;
    // A decode that throws ends the process: this thread is the only thing between a malformed file
    // and the whole engine.
    bool ok = false;
    try {
      ok = media_decode_image(request.path, request.box, pixels, width, height, error);
    } catch (const std::exception& failure) {
      error = failure.what();
    } catch (...) {
      error = "the picture could not be decoded";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(request.key);
    if (found == entries_.end()) continue;
    if (ok) {
      found->second.pixels = std::move(pixels);
      found->second.width = width;
      found->second.height = height;
      found->second.state = Entry::decoded;
    } else {
      found->second.state = Entry::failed;
    }
  }
}

bool ImageCache::upload(int most, const Create& create) {
  bool any = false;
  std::lock_guard<std::mutex> lock(mutex_);
  int made = 0;
  for (auto& item : entries_) {
    Entry& entry = item.second;
    if (entry.state != Entry::decoded) continue;
    if (made++ >= most) break;  // the rest next frame; loading() keeps the frames coming
    entry.texture = create(entry.pixels.data(), entry.width, entry.height);
    entry.state = entry.texture ? Entry::ready : Entry::failed;
    std::vector<uint8_t>().swap(entry.pixels);  // the pixels live on the GPU now
    any = true;
  }
  return any;
}

void ImageCache::trim(const Release& release, size_t max_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t total = 0;
  for (const auto& item : entries_) total += item.second.bytes();
  if (total <= max_bytes) return;
  std::vector<std::pair<uint64_t, std::string>> order;
  order.reserve(entries_.size());
  for (const auto& item : entries_) order.emplace_back(item.second.used, item.first);
  std::sort(order.begin(), order.end());
  // Oldest first, and the newest entry is never dropped: it is what this frame asked for, even if it alone is over budget.
  for (size_t i = 0; i + 1 < order.size() && total > max_bytes; ++i) {
    auto found = entries_.find(order[i].second);
    if (found == entries_.end()) continue;
    const size_t bytes = found->second.bytes();
    total = bytes >= total ? 0 : total - bytes;
    if (found->second.texture) release(found->second.texture);
    entries_.erase(found);
  }
}

void ImageCache::clear(const Release& release) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : entries_) {
    if (item.second.texture) release(item.second.texture);
  }
  entries_.clear();
  queue_.clear();
}

bool ImageCache::loading() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!queue_.empty()) return true;
  for (const auto& item : entries_) {
    if (item.second.state == Entry::queued || item.second.state == Entry::decoded) return true;
  }
  return false;
}

}  // namespace bx::ui
