// Local IPC for BuffrX: one named pipe, length-prefixed JSON frames, several clients.
//
// The engine owns the server; the UI shell and the editor shell are clients. Writes are per-client
// and guarded, reads happen on one thread per client, and a client that dies is dropped rather than
// waited on — a wedged UI must never stall capture. That last promise is why the server's I/O is
// overlapped: a synchronous write to a client that has stopped reading blocks forever once its pipe
// buffer fills, which stops the engine's tick, its tray, its hotkeys and every save. Writes carry a
// timeout instead, and a client that misses it is dropped.
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>

#include "log.h"
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <exception>
#include <stdexcept>

namespace bx {

// Windows' PIPE_REJECT_REMOTE (0x8). llvm-mingw's winbase.h, which is the toolchain here, does not
// define it; the SDK does, with this value.
constexpr DWORD kPipeRejectRemote = 0x00000008;

// Runs `body` and turns an exception into a log line instead of the end of the process. A command
// that throws - a thread that could not be created, a bad allocation - must not take the engine
// with it: the tick, the watchdog and the hotkeys all keep working, and the user is told.
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

// A frame is at most this big on either side; a longer length is a corrupt stream, not a message. A
// reply that lists a large library stays well under it, and the parser has its own budget for values.
constexpr uint32_t kMaxFrameBytes = 8u * 1024u * 1024u;

// The length a frame's four-byte header asks for, and whether the stream may carry it. One rule for both readers, so the
// mutation harness drives exactly what a lying length prefix meets.
inline bool frame_length(const unsigned char* header, uint32_t& length) {
  length = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
           (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
  return length <= kMaxFrameBytes;
}
// How long a write may wait for a client to drain its pipe before that client is dropped. It only
// ever elapses when the client has stopped reading for ~45 s of broadcasts, so a live client never
// reaches it.
constexpr DWORD kWriteTimeoutMs = 1000;
// The reader waits in slices so it can notice a dropped client even if closing the handle does not
// signal its completion event.
constexpr DWORD kReaderPollMs = 250;
// How many commands may wait for a worker, and how many bytes together. A client that sends faster
// than the workers answer waits for room instead of growing the engine's memory: frames may be 32 MiB
// each, so a count alone would still allow gigabytes.
constexpr size_t kMaxPendingCommands = 256;
constexpr size_t kMaxPendingBytes = 64u * 1024u * 1024u;

inline bool pipe_read_exact(HANDLE pipe, void* buffer, DWORD bytes) {
  auto* cursor = static_cast<unsigned char*>(buffer);
  DWORD remaining = bytes;
  while (remaining > 0) {
    DWORD read = 0;
    if (!ReadFile(pipe, cursor, remaining, &read, nullptr) || read == 0) return false;
    cursor += read;
    remaining -= read;
  }
  return true;
}

inline bool pipe_write_exact(HANDLE pipe, const void* buffer, DWORD bytes) {
  auto* cursor = static_cast<const unsigned char*>(buffer);
  DWORD remaining = bytes;
  while (remaining > 0) {
    DWORD written = 0;
    if (!WriteFile(pipe, cursor, remaining, &written, nullptr) || written == 0) return false;
    cursor += written;
    remaining -= written;
  }
  return true;
}

inline bool pipe_write_frame(HANDLE pipe, const std::string& payload) {
  if (payload.size() > kMaxFrameBytes) return false;
  uint32_t length = static_cast<uint32_t>(payload.size());
  unsigned char header[4] = {
      static_cast<unsigned char>(length & 0xFF),
      static_cast<unsigned char>((length >> 8) & 0xFF),
      static_cast<unsigned char>((length >> 16) & 0xFF),
      static_cast<unsigned char>((length >> 24) & 0xFF)};
  if (!pipe_write_exact(pipe, header, 4)) return false;
  return payload.empty() ? true : pipe_write_exact(pipe, payload.data(), length);
}

// Blocking read of one frame. Returns false on disconnect or a malformed length.
inline bool pipe_read_frame(HANDLE pipe, std::string& payload) {
  unsigned char header[4];
  if (!pipe_read_exact(pipe, header, 4)) return false;
  uint32_t length = 0;
  if (!frame_length(header, length)) return false;
  payload.resize(length);
  if (length == 0) return true;
  return pipe_read_exact(pipe, &payload[0], length);
}

// One overlapped transfer on a pipe opened with FILE_FLAG_OVERLAPPED. timeout_ms of INFINITE waits
// in kReaderPollMs slices and gives up when the client is marked closed; any other value is a
// deadline that drops the operation with CancelIoEx. The OVERLAPPED and its event belong to the
// caller and must stay valid for the whole operation.
inline bool pipe_overlapped_transfer(HANDLE pipe, void* buffer, DWORD bytes, DWORD* transferred, OVERLAPPED& overlapped,
                                     HANDLE event, DWORD timeout_ms, bool is_write,
                                     const std::atomic<bool>* abort) {
  ResetEvent(event);
  overlapped.Offset = 0;
  overlapped.OffsetHigh = 0;
  overlapped.hEvent = event;
  DWORD done = 0;
  const BOOL started = is_write ? WriteFile(pipe, buffer, bytes, &done, &overlapped)
                                : ReadFile(pipe, buffer, bytes, &done, &overlapped);
  if (!started) {
    if (GetLastError() != ERROR_IO_PENDING) return false;
    for (;;) {
      const DWORD wait = WaitForSingleObject(event, timeout_ms == INFINITE ? kReaderPollMs : timeout_ms);
      if (wait == WAIT_OBJECT_0) break;
      if (wait == WAIT_TIMEOUT) {
        if (timeout_ms != INFINITE || (abort && abort->load())) {
          CancelIoEx(pipe, &overlapped);
          WaitForSingleObject(event, 1000);
          return false;
        }
        continue;
      }
      return false;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &done, FALSE)) return false;
  }
  *transferred = done;
  return true;
}

inline bool pipe_read_frame_overlapped(HANDLE pipe, std::string& payload, OVERLAPPED& overlapped, HANDLE event,
                                       const std::atomic<bool>* abort) {
  unsigned char header[4];
  size_t have = 0;
  while (have < 4) {
    DWORD got = 0;
    if (!pipe_overlapped_transfer(pipe, header + have, 4 - static_cast<DWORD>(have), &got, overlapped, event, INFINITE,
                                  false, abort)) {
      return false;
    }
    if (got == 0) return false;
    have += got;
  }
  uint32_t length = 0;
  if (!frame_length(header, length)) return false;
  payload.resize(length);
  have = 0;
  while (have < length) {
    DWORD got = 0;
    if (!pipe_overlapped_transfer(pipe, &payload[have], length - static_cast<DWORD>(have), &got, overlapped, event,
                                  INFINITE, false, abort)) {
      return false;
    }
    if (got == 0) return false;
    have += got;
  }
  return true;
}

// The whole frame in one buffer: a header and a body sent as two transfers could be interleaved with
// a broadcast, and a reader would then see a length that belongs to the other message.
inline bool pipe_write_frame_overlapped(HANDLE pipe, const std::string& payload, OVERLAPPED& overlapped, HANDLE event,
                                        DWORD timeout_ms) {
  if (payload.size() > kMaxFrameBytes) return false;
  const uint32_t length = static_cast<uint32_t>(payload.size());
  std::string frame;
  frame.resize(4 + payload.size());
  frame[0] = static_cast<char>(length & 0xFF);
  frame[1] = static_cast<char>((length >> 8) & 0xFF);
  frame[2] = static_cast<char>((length >> 16) & 0xFF);
  frame[3] = static_cast<char>((length >> 24) & 0xFF);
  if (!payload.empty()) std::memcpy(&frame[4], payload.data(), payload.size());
  size_t have = 0;
  while (have < frame.size()) {
    DWORD got = 0;
    if (!pipe_overlapped_transfer(pipe, &frame[have], static_cast<DWORD>(frame.size() - have), &got, overlapped, event,
                                  timeout_ms, true, nullptr)) {
      return false;
    }
    if (got == 0) return false;
    have += got;
  }
  return true;
}

class PipeServer {
public:
  using FrameHandler = std::function<void(int client, const std::string& payload)>;
  using CloseHandler = std::function<void(int client)>;
  // Called with a command that could not be queued in time, so the client gets an answer instead of
  // its slot being held until the workers catch up.
  using BusyHandler = std::function<void(int client, const std::string& payload)>;

  ~PipeServer() { stop(); }

  bool start(const std::string& name, FrameHandler on_frame, CloseHandler on_close, BusyHandler on_busy = {}) {
    if (running_) return false;
    name_ = name;
    on_frame_ = std::move(on_frame);
    on_close_ = std::move(on_close);
    on_busy_ = std::move(on_busy);
    running_ = true;
    // A fixed pool of slots, each with its own instance and its own thread parked on it. The editor
    // holds a connection for its whole life, so there has to be room for more than one client.
    start_slots(kSlots);
    // The commands themselves run on workers, not on the slot's thread. A command that takes its
    // time - a thumbnail, an export, a list of a big library - must not take the slot with it: a
    // slot busy with a slow command is an instance no client can reach, and four slow commands
    // leave the server answering nobody while the engine is perfectly alive.
    start_workers(kWorkers);
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) return;

    // The workers are told to stop and are waited for: a command still running when the server
    // goes is a command writing into a pipe that is closing.
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      stopping_workers_ = true;
    }
    pending_ready_.notify_all();
    pending_room_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();

    std::vector<std::shared_ptr<Client>> clients;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& entry : clients_) clients.push_back(entry.second);
      clients_.clear();
    }
    for (auto& client : clients) {
      client->closed = true;
      // Cancel what the slot's thread is waiting on, so it can see that the server is going. The
      // handle itself belongs to that thread now: closing it here would pull the pipe out from under
      // a connect that is still in flight, which is an access violation, not a shutdown.
      CancelIoEx(client->pipe, &client->connect_ov);
      CancelIoEx(client->pipe, &client->read_ov);
      CancelIoEx(client->pipe, &client->write_ov);
    }
    // A reader thread detaches itself, so nobody joins one: stopping waits for them to retire
    // instead. The wait is bounded so a handler that refuses to return cannot hang shutdown.
    for (int i = 0; i < 200 && active_readers_.load() > 0; ++i) Sleep(25);
  }

  // `client` is a connection id, as the frame handler received it. A reply to a client that has gone
  // goes nowhere: the slot it was on may be serving somebody else by now, and that somebody must not
  // receive an answer to a question it never asked.
  void send(int client, const std::string& payload) {
    std::shared_ptr<Client> target;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& entry : clients_) {
        if (entry.second->connection.load() == client) {
          target = entry.second;
          break;
        }
      }
    }
    if (target) write_to(*target, client, payload);
  }

  void broadcast(const std::string& payload) {
    std::vector<std::pair<std::shared_ptr<Client>, int>> targets;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& entry : clients_) {
        if (entry.second->connected) targets.emplace_back(entry.second, entry.second->connection.load());
      }
    }
    for (auto& target : targets) write_to(*target.first, target.second, payload);
  }

  size_t client_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto& entry : clients_) {
      if (entry.second->connected) ++count;
    }
    return count;
  }

  // How many clients were dropped for not draining their pipe. Exposed for the tests.
  uint64_t dropped_clients() const { return dropped_clients_; }

  bool running() const { return running_; }

private:
  struct Client {
    int id = 0;  // the slot's own number; the clients on it are told apart by `connection`
    // The connection on this slot right now, numbered across the whole server: a slot is reused, and
    // the engine keys its view of a client (its role, the editor) by this, not by the slot.
    std::atomic<int> connection{0};
    // Whether the current connection's end has been reported. Exactly one report per connection,
    // whether the client left on its own, was dropped for not reading, or the server stopped.
    std::atomic<bool> close_reported{true};
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::mutex write_mutex;
    std::atomic<bool> closed{false};
    // Whether this client was dropped for not reading. The slot lives on; the client does not.
    std::atomic<bool> dropped{false};
    // Whether a client is on this slot right now. The slot itself exists for as long as the server
    // does; what changes is whether somebody is connected to it.
    std::atomic<bool> connected{false};
    OVERLAPPED read_ov{};
    OVERLAPPED write_ov{};
    // The pending connect, kept on the client so that stopping the server can cancel it: a thread
    // blocked in ConnectNamedPipe with a local OVERLAPPED cannot be woken at all.
    OVERLAPPED connect_ov{};
    HANDLE read_event = INVALID_HANDLE_VALUE;
    HANDLE write_event = INVALID_HANDLE_VALUE;
    HANDLE connect_event = INVALID_HANDLE_VALUE;
  };

  // Exactly one thread closes a client's handle: whoever wins the exchange. The reader thread, the
  // writer and stop() all call this. DisconnectNamedPipe is deliberately not used here: on a pipe
  // with a write still queued it waits for the client to read the data, which is the very stall this
  // class exists to avoid. Closing the handle ends the connection just as well.
  static void close_pipe(Client& client) {
    HANDLE handle = static_cast<HANDLE>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&client.pipe),
                                                                  INVALID_HANDLE_VALUE));
    if (handle == INVALID_HANDLE_VALUE) return;
    CloseHandle(handle);
  }

  // The events belong to the reader thread, which closes them as it exits. Closing them from the
  // writer while the reader waits on one would leave it waiting on a stale handle.
  static void close_events(Client& client) {
    HANDLE read_event = static_cast<HANDLE>(InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&client.read_event), INVALID_HANDLE_VALUE));
    if (read_event != INVALID_HANDLE_VALUE) CloseHandle(read_event);
    HANDLE write_event = static_cast<HANDLE>(InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&client.write_event), INVALID_HANDLE_VALUE));
    if (write_event != INVALID_HANDLE_VALUE) CloseHandle(write_event);
    HANDLE connect_event = static_cast<HANDLE>(InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(&client.connect_event), INVALID_HANDLE_VALUE));
    if (connect_event != INVALID_HANDLE_VALUE) CloseHandle(connect_event);
  }

  // A client that could not be written to is not a client any more. The handle stays with the slot
  // thread: closing it here would pull the pipe out from under a connect that is still in flight,
  // and the thread's next use of it is an access violation rather than a shutdown.
  //
  // Cancelling the pending read is what retires the client: the thread wakes, sees the client gone
  // and re-arms the instance for the next one. The pending CONNECT is left alone on purpose: a slot
  // that lost its connect would be gone for good, and the pool would shrink by one every time a
  // client went quiet - until no client could connect at all and the server looked dead.
  void drop(Client& client) {
    if (client.dropped.exchange(true)) return;
    ++dropped_clients_;
    BX_LOG_WARN("the pipe server dropped a client that would not read");
    client.connected = false;
    CancelIoEx(client.pipe, &client.read_ov);
    CancelIoEx(client.pipe, &client.write_ov);
    report_close(client);
  }

  // Tells the engine a connection has ended, once.
  void report_close(Client& client) {
    if (client.close_reported.exchange(true)) return;
    if (on_close_) guarded("the pipe's close handler", [&] { on_close_(client.connection.load()); });
  }

  // Writes one frame to the connection `connection`, if it is still the one on this slot.
  void write_to(Client& target, int connection, const std::string& payload) {
    // A slot with nobody on it has nothing to say. Writing to it would fail, and a failed write
    // drops the slot - so a broadcast would take the server's own spare capacity away from it.
    if (!target.connected || target.connection.load() != connection) return;
    std::lock_guard<std::mutex> write_lock(target.write_mutex);
    if (target.closed || target.connection.load() != connection) return;
    if (!pipe_write_frame_overlapped(target.pipe, payload, target.write_ov, target.write_event, kWriteTimeoutMs)) {
      drop(target);
    }
  }

  // A slot is one pipe instance and the thread that serves whoever connects to it.
  //
  // The server keeps a fixed number of slots, each parked in ConnectNamedPipe on its own instance.
  // A client that connects is served by the slot whose instance it landed on; when that client
  // goes, the slot re-arms the same instance and waits for the next one.
  //
  // The earlier shape - the accept loop made a new instance for every connection and handed it to a
  // new reader - had a window in which a client connected to an instance whose reader had already
  // exited, and nobody ever answered it. From the client's side that is an engine that has stopped
  // answering, and it showed up as the acceptance harness failing half its runs at the same command.
  // A fixed pool has no such window: every instance is always armed, and the thread that owns it
  // keeps serving.
  void start_slots(int count) {
    // Never a connection from another PC over \\<pc>\pipe\<name>: the protocol hands out live link
    // tokens, so a remote reader must not get in. The pipe keeps Windows' default DACL, which is
    // this account, SYSTEM and Administrators; an SDDL DACL of our own denied the local client,
    // because the owner SID the object gets is not always the one the ACE placeholder resolves to.
    for (int i = 0; i < count; ++i) {
      auto client = std::make_shared<Client>();
      client->pipe = CreateNamedPipeA(name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | kPipeRejectRemote,
                                      PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
      if (client->pipe == INVALID_HANDLE_VALUE) {
        BX_LOG_WARN("a pipe slot could not be created: %lu", GetLastError());
        continue;
      }
      client->read_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
      client->write_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
      if (client->read_event == INVALID_HANDLE_VALUE || client->write_event == INVALID_HANDLE_VALUE) {
        close_events(*client);
        close_pipe(*client);
        continue;
      }
      client->id = next_client_++;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_[client->id] = client;
      }
      ++active_readers_;
      // Detached at once, and nothing joins it: the thread owns its own handle and closes it when it
      // retires. Keeping the std::thread in the client meant the thread had to detach itself, and a
      // thread that detaches itself while the thread object is still being assigned is a race - and a
      // failed detach throws, which on a thread is the end of the process.
      std::thread([this, client] { slot_loop(client); }).detach();
    }
  }

  // Parks the slot on its instance until a client is there, then serves it, then repeats.
  void slot_loop(std::shared_ptr<Client> client) {
    std::string payload;
    bool first = true;
    while (!client->closed && running_) {
      if (!connect_instance(*client, first)) {
        // Stopping the server cancels the wait on every slot: that is a shutdown, said at info. A wait that failed while the
        // server runs is a fault.
        if (running_ && !client->closed) {
          BX_LOG_WARN("the pipe server's slot %d stopped waiting for a client", client->id);
        } else {
          BX_LOG_INFO("the pipe server's slot %d stopped waiting for a client", client->id);
        }
        break;
      }
      first = false;
      {
        // Under the write lock: a write that failed on the previous connection drops it while holding
        // this lock, and must never land on the connection that replaces it.
        std::lock_guard<std::mutex> write_lock(client->write_mutex);
        client->connection = next_connection_++;
        client->close_reported = false;
        client->dropped = false;
        client->connected = true;
      }
      while (!client->closed && running_) {
        if (!pipe_read_frame_overlapped(client->pipe, payload, client->read_ov, client->read_event, &client->closed)) {
          break;
        }
        if (payload.empty()) continue;
        // The command goes to a worker; the slot keeps reading. A command that throws must not take
        // the engine with it: the tick, the watchdog and the hotkeys all keep working, and the
        // client is told the command failed.
        hand_to_worker(client->connection.load(), payload);
      }
      // The client is gone. The instance goes straight back to waiting for the next one, so a
      // client that connects while this one was being cleaned up is picked up here rather than
      // being left with nobody to answer it.
      client->connected = false;
      // Said out loud: the slot is about to serve somebody else, and the engine still thinks of this
      // connection as an editor or an interface until it is told otherwise.
      report_close(*client);
      if (client->closed || !running_) break;
    }
    client->closed = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      clients_.erase(client->id);
    }
    {
      // Under the write lock: a worker may be inside WriteFile on this handle, and closing it there
      // is an access violation on a handle Windows may already have recycled.
      std::lock_guard<std::mutex> write_lock(client->write_mutex);
      close_pipe(*client);
      close_events(*client);
    }
    report_close(*client);
    --active_readers_;
  }

  // Queues a command for a worker. A client that sends faster than the workers answer waits here once
  // the queue is full: the memory a client can make the engine hold stays bounded, and the wait lands
  // on that client's own slot, never on the engine or on another client. The wait is bounded, or one
  // flooding client would hold all four slots and every other client would get no answer at all: a
  // command that still has no room is refused through the busy handler instead.
  void hand_to_worker(int client, const std::string& payload) {
    {
      std::unique_lock<std::mutex> lock(pending_mutex_);
      const bool room = pending_room_.wait_for(lock, std::chrono::seconds(2), [&] {
        return stopping_workers_ ||
               (pending_.size() < kMaxPendingCommands && pending_bytes_ + payload.size() <= kMaxPendingBytes);
      });
      // Shutting down: the pipe this answer would go to is closing.
      if (stopping_workers_) return;
      if (!room) {
        lock.unlock();
        if (on_busy_) guarded("the pipe's busy handler", [&] { on_busy_(client, payload); });
        return;
      }
      pending_bytes_ += payload.size();
      pending_.push_back({client, payload});
    }
    pending_ready_.notify_one();
  }

  // The worker's loop: take a command, run it, take the next.
  void worker_loop() {
    for (;;) {
      int client = 0;
      std::string payload;
      {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_ready_.wait(lock, [&] { return stopping_workers_ || !pending_.empty(); });
        if (stopping_workers_ && pending_.empty()) return;
        if (pending_.empty()) continue;
        client = pending_.front().first;
        payload = pending_.front().second;
        pending_bytes_ -= payload.size();
        pending_.pop_front();
      }
      pending_room_.notify_one();
      if (on_frame_) guarded("a command", [&] { on_frame_(client, payload); });
    }
  }

  void start_workers(int count) {
    for (int i = 0; i < count; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  // Waits for the next client on the slot's instance.
  //
  // After a client has been served, the instance is disconnected first, because that is what makes
  // it available again: until the server says so, Windows considers it still taken by the client
  // that has just gone, and a new client is told the pipe is busy. No client can land in the
  // window between the two calls, for exactly that reason - the instance is not free yet.
  //
  // The unread data is discarded before the disconnect, and that is the whole trick. DisconnectNamedPipe
  // waits until the client has read everything the server wrote - and the client has just gone, so
  // that wait would never end. A slot stuck in it keeps its instance to itself, and when enough slots
  // are stuck the server has no instance left that a client can reach: the engine is alive, and it
  // answers nobody. FlushFileBuffers throws the data away, which is worth nothing now, and the
  // disconnect returns at once.
  //
  // The very first wait does NOT disconnect: the instance was created a moment ago and a client
  // may have connected to it already, and disconnecting then would throw that client away.
  bool connect_instance(Client& client, bool first) {
    if (!first) {
      FlushFileBuffers(client.pipe);
      DisconnectNamedPipe(client.pipe);
    }
    if (client.connect_event == INVALID_HANDLE_VALUE) {
      client.connect_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
      if (client.connect_event == INVALID_HANDLE_VALUE) return false;
    }
    ResetEvent(client.connect_event);
    client.connect_ov.hEvent = client.connect_event;
    bool connected = false;
    if (ConnectNamedPipe(client.pipe, &client.connect_ov)) {
      connected = true;
    } else {
      const DWORD error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED) {
        connected = true;  // a client arrived while the old one was going
      } else if (error == ERROR_IO_PENDING) {
        WaitForSingleObject(client.connect_event, INFINITE);
        DWORD transferred = 0;
        connected = GetOverlappedResult(client.pipe, &client.connect_ov, &transferred, FALSE) ? TRUE : FALSE;
      }
    }
    return connected != FALSE;
  }

  std::string name_;
  FrameHandler on_frame_;
  CloseHandler on_close_;
  BusyHandler on_busy_;
  std::atomic<bool> running_{false};
  std::atomic<int> next_client_{1};
  std::atomic<int> next_connection_{1};
  std::atomic<uint64_t> dropped_clients_{0};
  std::atomic<int> active_readers_{0};
  std::mutex mutex_;
  std::map<int, std::shared_ptr<Client>> clients_;
  // The commands waiting for a worker.
  std::deque<std::pair<int, std::string>> pending_;
  size_t pending_bytes_ = 0;
  std::mutex pending_mutex_;
  std::condition_variable pending_ready_;
  std::condition_variable pending_room_;
  bool stopping_workers_ = false;
  std::vector<std::thread> workers_;
  // How many clients can be served at once. The editor holds one connection for its whole life, the
  // interface is in-process, and a probe may want one as well: four is room for all of them.
  static constexpr int kSlots = 4;
  // How many commands run at once. Two is enough that one slow command (a thumbnail, an export)
  // does not hold up the rest, and small enough that the engine is not doing the work twice.
  static constexpr int kWorkers = 2;
};

class PipeClient {
public:
  ~PipeClient() { disconnect(); }

  bool connect(const std::string& name, DWORD timeout_ms = 5000) {
    const DWORD deadline = GetTickCount() + timeout_ms;
    while (GetTickCount() < deadline) {
      HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (pipe != INVALID_HANDLE_VALUE) {
        pipe_ = pipe;
        return true;
      }
      // A busy pipe is waited on, which is the cheap path; a name with no server yet gets a short
      // sleep, so the loop does not spin while the engine is still starting.
      if (GetLastError() == ERROR_PIPE_BUSY) {
        WaitNamedPipeA(name.c_str(), 100);
      } else {
        Sleep(50);
      }
    }
    return false;
  }

  bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }
  HANDLE handle() const { return pipe_; }

  bool send(const std::string& payload) { return connected() && pipe_write_frame(pipe_, payload); }
  bool receive(std::string& payload) { return connected() && pipe_read_frame(pipe_, payload); }

  void disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
  }

private:
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}  // namespace bx
