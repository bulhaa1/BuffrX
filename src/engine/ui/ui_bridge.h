// The native interface's link to the engine: the same JSON commands a pipe client sends, handed to
// Protocol::handle in this process, so every command, its lock and its errors behave exactly as over
// \\.\pipe\buffrx. Commands run on worker threads, never on the interface's thread: commands that take
// the engine lock queue on one worker, the slow lock-free ones (library lists, range export) on another,
// the thumbnails on a third, and a modal dialog (the folder or file picker, which holds until the person
// answers) on a fourth, so no one of them holds up a save, a list or the panel.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../shared/json.h"

namespace bx::ui {

class UiBridge {
public:
  using Handler = std::function<std::string(const std::string& payload)>;
  using Reply = std::function<void(bool ok, const Json& data, const std::string& error)>;

  ~UiBridge();
  void start(Handler handler);
  void stop();

  // Called from the worker (or event) thread each time something is waiting for pump(), so the interface's
  // loop wakes at once instead of at its next timeout. Set before start().
  void set_wake(std::function<void()> wake) { wake_callback_ = std::move(wake); }

  // Queues a command; `reply` runs later on the interface's thread, from pump().
  void call(const std::string& command, Json args = Json::object(), Reply reply = {});
  // An engine event frame ({"event":…,"data":…}), from any thread.
  void push_event(const std::string& frame);

  // Interface thread: runs the replies that came back and hands over the events that arrived.
  // True when anything happened (the window then draws a frame).
  bool pump(std::vector<std::pair<std::string, Json>>& events);
  bool busy() const;  // commands still out

private:
  struct Job {
    std::string command;
    std::string payload;
    Reply reply;
  };
  struct Done {
    Reply reply;
    bool ok = false;
    Json data;
    std::string error;
  };
  struct Worker {
    std::thread thread;
    std::deque<Job> jobs;
  };

  void run(Worker& worker);

  Handler handler_;
  std::function<void()> wake_callback_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  // Phase 2.2: a third lane for the media listings and the thumbnails of the cards, so a screenshot list or a card's thumbnail
  // never waits behind the clips list (which decodes what it can on the lock-free lane). A fourth lane carries the modal
  // dialogs: the folder and file pickers hold until the person answers, so they must not block any command behind them.
  Worker locked_, lock_free_, media_, dialog_;
  Worker update_;
  std::deque<Done> done_;
  std::vector<std::string> events_;
  int next_id_ = 1;
  int outstanding_ = 0;
};

}  // namespace bx::ui
