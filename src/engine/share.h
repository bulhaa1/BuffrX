// Sharing a clip: the jobs behind the share sheet, and the one Discord webhook the user has set.
//
// A job takes a library file and makes it fit an upload limit (share_math.h). A clip that already
// fits is the file itself; one that does not is re-encoded by a short-lived `buffrx-engine.exe
// --media-export` process at below-normal priority, so the encode never shares memory with the
// buffering engine and costs a running game as little as it can. From there the job's file is copied
// to the clipboard or posted to the webhook, both of which the user presses.
//
// The fitted file lives in <data root>/share-cache/<id>/ under the clip's own name, because that name is
// what a friend sees. A job nobody used is deleted when the sheet closes; one that was copied stays
// (the clipboard points at it) until the next start clears anything older than a day.
//
// Every state change is published as `share.changed` with the job's JSON. Nothing here takes the
// engine lock: a job reads files and starts processes, never libobs.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "../shared/json.h"
#include "webhook_list.h"

namespace bx {

class ShareService : public std::enable_shared_from_this<ShareService> {
public:
  ShareService(std::string data_root, std::string engine_exe);
  ~ShareService();

  // Publishes an event to every client and the interface; set by main.
  std::function<void(const std::string& name, const Json& data)> emit;

  // Starts a job for one library file. Without `fit` the job is the file as it is ("ready" at once),
  // with a preview of what the Discord fit would do; with it the file is made to fit `limit_mib`
  // ("fitting", or "ready" when it already fits).
  bool prepare(const std::string& file, int limit_mib, bool fit, Json& job, std::string& error);
  bool get(const std::string& id, Json& job, std::string& error);
  bool copy(const std::string& id, std::string& error);
  // Posts the job's file to the selected webhook, in the background ("posting", then "posted" or "ready" with an error).
  bool post_discord(const std::string& id, const std::string& message, const std::string& webhook_id, Json& job, std::string& error);
  // The sheet closed: a running fit or post stops, and a file nobody used is deleted.
  bool cancel(const std::string& id, std::string& error);

  // Webhooks are sealed on disk (protect_secret) in <data root>/share.json, never in config.json and
  // never sent back. Replies carry user-supplied names and non-secret webhook ids and hints.
  Json webhook_json();
  bool set_webhook(const std::string& url, const std::string& name, bool add, std::string& error);
  bool clear_webhook(const std::string& webhook_id, std::string& error);

  // Stops every job and waits for it. Called once the pipe and the interface are stopped.
  void shutdown();

private:
  struct Job;
  void run_fit(std::shared_ptr<Job> job);
  void run_post(std::shared_ptr<Job> job, std::string url, std::string message);
  void publish(const std::shared_ptr<Job>& job);
  Json job_json_locked(const Job& job) const;
  std::shared_ptr<Job> find(const std::string& id);
  void clean_cache();
  bool read_webhooks(std::vector<WebhookEntry>& hooks, std::string& error);
  bool write_webhooks(const std::vector<WebhookEntry>& hooks, std::string& error);
  bool load_webhook(const std::string& id, std::string& url, std::string& name, std::string& error);

  std::string data_root_;
  std::string cache_root_;
  std::string engine_exe_;
  std::mutex mutex_;
  std::mutex webhook_mutex_;  // share commands can run concurrently without the engine lock
  std::map<std::string, std::shared_ptr<Job>> jobs_;
  // Work runs on detached threads, counted, so shutdown can wait for the last one to let go of `this`.
  int running_ = 0;
  std::condition_variable idle_;
  uint64_t next_id_ = 0;
  bool stopped_ = false;
};

}  // namespace bx
