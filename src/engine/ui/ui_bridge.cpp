#include "ui_bridge.h"

#include "../command_policy.h"
#include "../../shared/log.h"

namespace bx::ui {

UiBridge::~UiBridge() { stop(); }

void UiBridge::start(Handler handler) {
  stop();
  handler_ = std::move(handler);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = false;
  }
  locked_.thread = std::thread([this] { run(locked_); });
  lock_free_.thread = std::thread([this] { run(lock_free_); });
  media_.thread = std::thread([this] { run(media_); });
  dialog_.thread = std::thread([this] { run(dialog_); });
  update_.thread = std::thread([this] { run(update_); });
}

void UiBridge::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (locked_.thread.joinable()) locked_.thread.join();
  if (lock_free_.thread.joinable()) lock_free_.thread.join();
  if (media_.thread.joinable()) media_.thread.join();
  if (dialog_.thread.joinable()) dialog_.thread.join();
  if (update_.thread.joinable()) update_.thread.join();
  std::lock_guard<std::mutex> lock(mutex_);
  locked_.jobs.clear();
  lock_free_.jobs.clear();
  media_.jobs.clear();
  dialog_.jobs.clear();
  update_.jobs.clear();
  done_.clear();
  outstanding_ = 0;
}

void UiBridge::call(const std::string& command, Json args, Reply reply) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || !handler_) return;
  Json frame = Json::object();
  frame.set("id", Json(next_id_++));
  frame.set("cmd", Json(command));
  frame.set("args", std::move(args));
  const bool media = command == "screenshots.list" || command == "clips.thumbnail" || command == "clips.frames";
  // A picker holds until the person answers: it gets a lane of its own, ahead of every other rule.
  const bool dialog = command == "app.pick_folder" || command == "app.pick_file";
  Worker& worker = dialog ? dialog_ : command == "update.check" || command == "update.notes" ? update_ :
                   command_needs_engine_lock(command) ? locked_ : media ? media_ : lock_free_;
  worker.jobs.push_back(Job{command, frame.dump(), std::move(reply)});
  ++outstanding_;
  wake_.notify_all();
}

void UiBridge::push_event(const std::string& frame) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(frame);
  }
  if (wake_callback_) wake_callback_();
}

void UiBridge::run(Worker& worker) {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || !worker.jobs.empty(); });
      if (stopping_) return;
      job = std::move(worker.jobs.front());
      worker.jobs.pop_front();
    }
    Done done;
    done.reply = std::move(job.reply);
    // A command that throws must not take the engine with it: this thread has no other way to
    // report it, and an exception on a thread ends the process.
    std::string response;
    try {
      response = handler_(job.payload);
    } catch (const std::exception& failure) {
      done.ok = false;
      done.error = failure.what();
      response.clear();
      BX_LOG_ERROR("a command on the interface thread failed: %s", failure.what());
    } catch (...) {
      done.ok = false;
      done.error = "the command failed with an unknown error";
      response.clear();
      BX_LOG_ERROR("a command on the interface thread failed with an unknown error");
    }
    Json parsed;
    std::string parse_error;
    if (!response.empty() && Json::parse(response, parsed, parse_error)) {
      done.ok = parsed["ok"].as_bool();
      done.data = parsed["data"];
      done.error = parsed["error"].as_string();
    } else if (response.empty()) {
      // The handler threw; the reason is already in done.error.
    } else {
      done.error = "the engine answered something unreadable";
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_.push_back(std::move(done));
    }
    if (wake_callback_) wake_callback_();
  }
}

bool UiBridge::pump(std::vector<std::pair<std::string, Json>>& events) {
  std::deque<Done> done;
  std::vector<std::string> frames;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    done.swap(done_);
    frames.swap(events_);
    outstanding_ -= static_cast<int>(done.size());
  }
  for (Done& item : done) {
    if (item.reply) item.reply(item.ok, item.data, item.error);
  }
  for (const std::string& frame : frames) {
    Json parsed;
    std::string parse_error;
    if (Json::parse(frame, parsed, parse_error)) events.emplace_back(parsed["event"].as_string(), parsed["data"]);
  }
  return !done.empty() || !frames.empty();
}

bool UiBridge::busy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outstanding_ > 0;
}

}  // namespace bx::ui
