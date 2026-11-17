// Local IPC for the Linux port: one Unix-domain socket, length-prefixed JSON frames, several clients.
// The wire format is exactly the Windows named pipe's (a 4-byte little-endian length, then JSON), so
// the editor shell and BuffrX's protocol do not care which OS they run on.
#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../shared/log.h"

namespace bx {

template <typename Body>
void guarded(const char* what, Body body) {
  try {
    body();
  } catch (const std::exception& error) {
    BX_LOG_ERROR("%s failed: %s", what, error.what());
  } catch (...) {
    BX_LOG_ERROR("%s failed with an unknown error", what);
  }
}

constexpr uint32_t kMaxFrameBytes = 8u * 1024u * 1024u;

inline bool frame_length(const unsigned char* header, uint32_t& length) {
  length = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
           (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
  return length <= kMaxFrameBytes;
}

// The socket path a Windows pipe name maps to. A test can pass its own BUFFRX_PIPE name, and that name
// lands in the same per-user runtime directory as the default one.
inline std::string pipe_socket_path(const std::string& name) {
  std::string leaf = name;
  const std::string prefix = "\\\\.\\pipe\\";
  if (leaf.rfind(prefix, 0) == 0) leaf = leaf.substr(prefix.size());
  for (char& c : leaf) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) c = '_';
  }
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  const std::string base = runtime && *runtime ? runtime : "/tmp";
  return base + "/buffrx-" + std::to_string(static_cast<unsigned long>(getuid())) + "-" + leaf + ".sock";
}

inline bool pipe_write_exact(int fd, const void* buffer, size_t bytes) {
  const auto* cursor = static_cast<const unsigned char*>(buffer);
  size_t remaining = bytes;
  while (remaining > 0) {
    const ssize_t written = send(fd, cursor, remaining, MSG_NOSIGNAL);
    if (written <= 0) return false;
    cursor += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

inline bool pipe_read_exact(int fd, void* buffer, size_t bytes) {
  auto* cursor = static_cast<unsigned char*>(buffer);
  size_t remaining = bytes;
  while (remaining > 0) {
    const ssize_t read_bytes = recv(fd, cursor, remaining, 0);
    if (read_bytes <= 0) return false;
    cursor += read_bytes;
    remaining -= static_cast<size_t>(read_bytes);
  }
  return true;
}

inline bool pipe_write_frame(int fd, const std::string& payload) {
  if (payload.size() > kMaxFrameBytes) return false;
  const uint32_t length = static_cast<uint32_t>(payload.size());
  const unsigned char header[4] = {
      static_cast<unsigned char>(length & 0xFF), static_cast<unsigned char>((length >> 8) & 0xFF),
      static_cast<unsigned char>((length >> 16) & 0xFF), static_cast<unsigned char>((length >> 24) & 0xFF)};
  return pipe_write_exact(fd, header, sizeof(header)) && (payload.empty() || pipe_write_exact(fd, payload.data(), payload.size()));
}

inline bool pipe_read_frame(int fd, std::string& payload) {
  unsigned char header[4];
  if (!pipe_read_exact(fd, header, sizeof(header))) return false;
  uint32_t length = 0;
  if (!frame_length(header, length)) return false;
  payload.resize(length);
  return length == 0 || pipe_read_exact(fd, payload.data(), length);
}

class PipeServer {
public:
  using FrameHandler = std::function<void(int client, const std::string& payload)>;
  using CloseHandler = std::function<void(int client)>;
  using BusyHandler = std::function<void(int client, const std::string& payload)>;

  ~PipeServer() { stop(); }

  bool start(const std::string& name, FrameHandler on_frame, CloseHandler on_close, BusyHandler on_busy = {}) {
    if (running_) return false;
    path_ = pipe_socket_path(name);
    on_frame_ = std::move(on_frame);
    on_close_ = std::move(on_close);
    on_busy_ = std::move(on_busy);
    unlink(path_.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(address.sun_path)) return false;
    std::strcpy(address.sun_path, path_.c_str());
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    chmod(path_.c_str(), 0600);
    if (listen(listen_fd_, 16) != 0) {
      close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    running_ = true;
    accept_thread_ = std::thread([this] {
      while (running_) {
        const int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
          if (!running_) break;
          continue;
        }
        const int id = ++next_client_;
        auto client = std::make_shared<Client>();
        client->fd = fd;
        client->id = id;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          clients_[id] = client;
        }
        client->thread = std::thread([this, client] {
          std::string payload;
          while (running_ && pipe_read_frame(client->fd, payload)) {
            if (on_frame_) guarded("a pipe command", [&] { on_frame_(client->id, payload); });
            payload.clear();
          }
          {
            std::lock_guard<std::mutex> lock(client->write_mutex);
            client->closed = true;
            shutdown(client->fd, SHUT_RDWR);
            close(client->fd);
            client->fd = -1;
          }
          {
            std::lock_guard<std::mutex> lock(mutex_);
            clients_.erase(client->id);
          }
          if (on_close_) guarded("a pipe close", [&] { on_close_(client->id); });
        });
        // The reader owns a shared_ptr to its Client; destroying the last one is what frees it. A
        // joinable std::thread member would then call std::terminate from inside that thread, so the
        // thread is detached and stop() wakes it by closing the socket.
        client->thread.detach();
      }
    });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& entry : clients_) {
        auto& client = entry.second;
        std::lock_guard<std::mutex> write_lock(client->write_mutex);
        client->closed = true;
        if (client->fd >= 0) {
          shutdown(client->fd, SHUT_RDWR);
          close(client->fd);
          client->fd = -1;
        }
      }
      clients_.clear();
    }
    if (listen_fd_ >= 0) {
      shutdown(listen_fd_, SHUT_RDWR);
      close(listen_fd_);
      listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    unlink(path_.c_str());
  }

  void send(int client, const std::string& payload) {
    std::shared_ptr<Client> target;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = clients_.find(client);
      if (found != clients_.end()) target = found->second;
    }
    if (target) write_to(*target, payload);
  }

  void broadcast(const std::string& payload) {
    std::vector<std::shared_ptr<Client>> targets;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& entry : clients_) targets.push_back(entry.second);
    }
    for (auto& target : targets) write_to(*target, payload);
  }

  size_t client_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
  }
  uint64_t dropped_clients() const { return dropped_clients_; }
  bool running() const { return running_; }
  const std::string& path() const { return path_; }

private:
  struct Client {
    int fd = -1;
    int id = 0;
    std::mutex write_mutex;
    std::thread thread;
    bool closed = false;
  };

  void write_to(Client& client, const std::string& payload) {
    std::lock_guard<std::mutex> lock(client.write_mutex);
    if (client.closed || client.fd < 0) return;
    struct timeval timeout{1, 0};
    setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (!pipe_write_frame(client.fd, payload)) {
      client.closed = true;
      shutdown(client.fd, SHUT_RDWR);
      ++dropped_clients_;
    }
  }

  std::string path_;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> dropped_clients_{0};
  std::atomic<int> next_client_{0};
  FrameHandler on_frame_;
  CloseHandler on_close_;
  BusyHandler on_busy_;
  std::mutex mutex_;
  std::map<int, std::shared_ptr<Client>> clients_;
  std::thread accept_thread_;
};

class PipeClient {
public:
  ~PipeClient() { disconnect(); }

  bool connect(const std::string& name, unsigned timeout_ms = 5000) {
    const std::string path = pipe_socket_path(name);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd >= 0) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
          fd_ = fd;
          return true;
        }
        close(fd);
      }
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  bool connected() const { return fd_ >= 0; }
  int handle() const { return fd_; }
  bool send(const std::string& payload) { return connected() && pipe_write_frame(fd_, payload); }
  bool receive(std::string& payload) { return connected() && pipe_read_frame(fd_, payload); }

  void disconnect() {
    if (fd_ >= 0) {
      shutdown(fd_, SHUT_RDWR);
      close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_ = -1;
};

}  // namespace bx
