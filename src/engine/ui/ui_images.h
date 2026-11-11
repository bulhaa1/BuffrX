// Thumbnails and screenshot previews as textures. Files are decoded on one worker thread through the
// runtime's FFmpeg (media_decode_image), turned into textures on the interface's thread, and kept in a
// small most-recently-used set: only what is on screen is ever asked for, and everything is let go when
// the windows close.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"

namespace bx::ui {

class ImageCache {
public:
  using Create = std::function<ImTextureID(const uint8_t* bgra, int width, int height)>;
  using Release = std::function<void(ImTextureID)>;

  ~ImageCache();
  void start();
  void stop();

  // The texture for a file fitted in a `box`-pixel square: 0 while it loads, and `failed` once it
  // cannot be read. `size` is the decoded size in pixels.
  ImTextureID get(const std::string& path, int box, ImVec2* size = nullptr, bool* failed = nullptr);
  // Interface thread, once a frame: uploads what the worker decoded. True when a new image is ready.
  bool upload(int most, const Create& create);  // at most `most` textures per call
  // Keeps the most recently drawn textures within `max_bytes` of BGRA (four bytes a pixel); frees the oldest until
  // the total fits, never the newest one. A count alone let a few 8K screenshots hold gigabytes of textures.
  void trim(const Release& release, size_t max_bytes);
  void clear(const Release& release);
  bool loading() const;

private:
  struct Entry {
    enum State { queued, decoded, ready, failed } state = queued;
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    ImTextureID texture = 0;
    uint64_t used = 0;
    // What this entry costs: the pixels still waiting to become a texture, else the texture itself (BGRA, 4 bytes a pixel).
    size_t bytes() const {
      return pixels.empty() ? static_cast<size_t>(width) * static_cast<size_t>(height) * 4 : pixels.size();
    }
  };
  struct Request {
    std::string key, path;
    int box = 0;
  };
  void run();

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool stopping_ = false;
  std::map<std::string, Entry> entries_;
  std::deque<Request> queue_;
  uint64_t clock_ = 0;
};

}  // namespace bx::ui
