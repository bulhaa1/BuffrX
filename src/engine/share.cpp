#include "share.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "../shared/files.h"
#include "../shared/log.h"
#include "discord_webhook.h"
#include "library.h"
#include "media.h"
#include "platform.h"
#include "share_math.h"

namespace bx {
namespace {

int64_t file_size(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(utf8_to_wide(path).c_str(), GetFileExInfoStandard, &data)) return 0;
  return (static_cast<int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::string stem_of(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// A job folder holds only what the job wrote (the fitted file, its .part, the job file): every file in
// it goes, then the folder. Never recursive, never outside the cache.
void remove_job_folder(const std::string& folder) {
  WIN32_FIND_DATAW found{};
  HANDLE search = FindFirstFileW(utf8_to_wide(folder + "\\*").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      DeleteFileW(utf8_to_wide(folder + "\\" + wide_to_utf8(found.cFileName)).c_str());
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  RemoveDirectoryW(utf8_to_wide(folder).c_str());
}

std::string read_text(const std::string& path) {
  std::string text;
  if (FILE* file = _wfopen(utf8_to_wide(path).c_str(), L"rb")) {
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
  }
  return text;
}

}  // namespace

struct ShareService::Job {
  std::string id;
  std::string source;       // the library file
  std::string path;         // what is sent: the source itself for a copy, else the fitted file
  std::string folder;       // the job's cache folder, "" for a copy
  std::string state = "fitting";  // fitting | ready | posting | posted | failed
  std::string destination;       // the destination captured when Post was pressed
  std::string error;
  double progress = 0.0;
  double duration = 0.0;
  int64_t source_bytes = 0;
  int64_t bytes = 0;
  int64_t limit_bytes = 0;
  int source_width = 0, source_height = 0;
  ShareFit fit;       // how it is sent: as it is (copy) or re-encoded to these
  ShareFit plan;      // what "Make it ready for Discord" would do, shown before anything is encoded
  bool fitting = false;  // the user asked for the Discord fit
  bool used = false;        // copied or posted: the file must outlive the sheet
  std::atomic<bool> cancel{false};
  HANDLE process = nullptr; // the export while it runs, guarded by the service's mutex
};

ShareService::ShareService(std::string data_root, std::string engine_exe)
    : data_root_(std::move(data_root)), engine_exe_(std::move(engine_exe)) {
  cache_root_ = data_root_ + "\\share-cache";
  clean_cache();
}

ShareService::~ShareService() { shutdown(); }

void ShareService::clean_cache() {
  // Anything a day old is gone: a clip copied yesterday has been pasted by now.
  FILETIME now_file{};
  GetSystemTimeAsFileTime(&now_file);
  const uint64_t now = (static_cast<uint64_t>(now_file.dwHighDateTime) << 32) | now_file.dwLowDateTime;
  const uint64_t day = 24ull * 3600ull * 10000000ull;
  WIN32_FIND_DATAW found{};
  HANDLE search = FindFirstFileW(utf8_to_wide(cache_root_ + "\\*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    const std::string name = wide_to_utf8(found.cFileName);
    if (name == "." || name == "..") continue;
    const uint64_t written = (static_cast<uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
    if (now > written && now - written > day) remove_job_folder(cache_root_ + "\\" + name);
  } while (FindNextFileW(search, &found));
  FindClose(search);
}

Json ShareService::job_json_locked(const Job& job) const {
  Json json = Json::object();
  json.set("id", Json(job.id));
  json.set("file", Json(job.source));
  json.set("path", Json(job.state == "fitting" || job.state == "failed" ? std::string() : job.path));
  json.set("state", Json(job.state));
  json.set("progress", Json(job.progress));
  if (!job.destination.empty()) json.set("destination", Json(job.destination));
  if (!job.error.empty()) json.set("error", Json(job.error));
  json.set("copy", Json(job.fit.copy));
  json.set("duration", Json(job.duration));
  json.set("source_bytes", Json(job.source_bytes));
  json.set("bytes", Json(job.bytes));
  json.set("limit_bytes", Json(job.limit_bytes));
  json.set("width", Json(job.fit.width));
  json.set("height", Json(job.fit.height));
  json.set("video_kbps", Json(job.fit.video_kbps));
  // The share of the source's bits per pixel the sent file keeps: what the sheet's quality line reads.
  double kept = 1.0;
  if (!job.fit.copy && job.duration > 0.0 && job.source_bytes > 0 && job.fit.width > 0 && job.source_width > 0) {
    const double source_rate = static_cast<double>(job.source_bytes) * 8.0 / job.duration / (job.source_width * job.source_height);
    const double sent_rate = (job.fit.video_kbps + job.fit.audio_kbps) * 1000.0 / (job.fit.width * job.fit.height);
    kept = source_rate > 0.0 ? std::min(1.0, sent_rate / source_rate) : 1.0;
  }
  json.set("kept", Json(kept));
  json.set("used", Json(job.used));
  // Whether what is sent fits the upload limit, and what the Discord fit would make of it.
  json.set("fitting", Json(job.fitting));
  json.set("fits_limit", Json(job.fit.copy ? job.source_bytes <= job.limit_bytes : true));
  json.set("plan_possible", Json(job.plan.fits));
  json.set("plan_copy", Json(job.plan.copy));
  json.set("plan_width", Json(job.plan.width));
  json.set("plan_height", Json(job.plan.height));
  return json;
}

void ShareService::publish(const std::shared_ptr<Job>& job) {
  Json data;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    data = job_json_locked(*job);
  }
  if (emit) emit("share.changed", data);
}

std::shared_ptr<ShareService::Job> ShareService::find(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = jobs_.find(id);
  return found == jobs_.end() ? nullptr : found->second;
}

bool ShareService::prepare(const std::string& file, int limit_mib, bool fit, Json& reply, std::string& error) {
  if (!library_owns_path(file)) {
    error = "that file is not in the BuffrX library";
    return false;
  }
  auto job = std::make_shared<Job>();
  job->source = file;
  job->source_bytes = file_size(file);
  job->limit_bytes = share_limit_bytes(limit_mib);
  if (job->source_bytes <= 0) {
    error = "the file is empty or gone";
    return false;
  }
  const MediaInfo info = media_probe(file);
  if (info.ok && info.width > 0) {
    job->duration = info.duration_seconds;
    job->source_width = info.width;
    job->source_height = info.height;
    job->plan = share_fit(info.duration_seconds, job->source_bytes, info.width, info.height, job->limit_bytes);
  } else if (job->source_bytes <= job->limit_bytes) {
    // A screenshot, or anything that is not a video: it can only be sent as it is.
    job->plan.fits = true;
    job->plan.copy = true;
  } else if (fit) {
    error = info.reason.empty() ? "the file is too large to share and is not a clip" : info.reason;
    return false;
  }
  job->fitting = fit;
  if (!fit) {
    // The original, as it is: nothing is encoded until the user asks for the Discord fit.
    job->fit = ShareFit{};
    job->fit.fits = true;
    job->fit.copy = true;
    job->fit.width = job->source_width;
    job->fit.height = job->source_height;
  } else {
    job->fit = job->plan;
  }
  if (!job->fit.fits) {
    error = "this clip is too long to fit in " + std::to_string(job->limit_bytes / (1024 * 1024)) +
            " MB - trim it in the editor first";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      error = "BuffrX is closing";
      return false;
    }
    job->id = "s" + std::to_string(GetTickCount64()) + "-" + std::to_string(++next_id_);
    if (job->fit.copy) {
      job->path = file;
      job->bytes = job->source_bytes;
      job->state = "ready";
      job->progress = 1.0;
    } else {
      job->folder = cache_root_ + "\\" + job->id;
      job->path = job->folder + "\\" + stem_of(file) + ".mp4";
      ++running_;
    }
    jobs_[job->id] = job;
    reply = job_json_locked(*job);
  }
  if (!job->fit.copy) {
    // The thread holds the service, not the other way round: shutdown may give up waiting before this
    // one reaches its next checkpoint, and it must not then write into a destroyed service.
    std::shared_ptr<ShareService> self = shared_from_this();
    std::thread([self, job] {
      self->run_fit(job);
      std::lock_guard<std::mutex> lock(self->mutex_);
      if (--self->running_ == 0) self->idle_.notify_all();
    }).detach();
  }
  BX_LOG_INFO("share %s: %lld bytes, limit %lld, %s %dx%d at %d kbps", job->id.c_str(),
              static_cast<long long>(job->source_bytes), static_cast<long long>(job->limit_bytes),
              job->fit.copy ? "sent as it is" : "fitting to", job->fit.width, job->fit.height, job->fit.video_kbps);
  return true;
}

void ShareService::run_fit(std::shared_ptr<Job> job) {
  CreateDirectoryW(utf8_to_wide(cache_root_).c_str(), nullptr);
  CreateDirectoryW(utf8_to_wide(job->folder).c_str(), nullptr);
  int kbps = job->fit.video_kbps;
  for (int attempt = 0; attempt < 2; ++attempt) {
    // The job file: ExportJob's fields. Only track 1, the mix, goes - it is the one Discord's player plays.
    Json spec = Json::object();
    spec.set("input", Json(job->source));
    spec.set("output", Json(job->path));
    spec.set("video_kbps", Json(kbps));
    spec.set("audio_kbps", Json(job->fit.audio_kbps));
    if (job->fit.width != job->source_width || job->fit.height != job->source_height) {
      spec.set("out_width", Json(job->fit.width));
      spec.set("out_height", Json(job->fit.height));
    }
    Json volumes = Json::array();
    volumes.push(Json(1.0));
    for (int track = 1; track < 6; ++track) volumes.push(Json(0.0));
    spec.set("volumes", volumes);
    const std::string spec_path = job->folder + "\\job.json";
    if (!write_file_atomic(spec_path, spec.dump())) {
      std::lock_guard<std::mutex> lock(mutex_);
      job->state = "failed";
      job->error = "the share cache could not be written";
      break;
    }

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
      std::lock_guard<std::mutex> lock(mutex_);
      job->state = "failed";
      job->error = "the export could not be started";
      break;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring command = L"\"" + utf8_to_wide(engine_exe_) + L"\" --media-export \"" + utf8_to_wide(spec_path) + L"\"";
    PROCESS_INFORMATION process{};
    // Below normal: a fit runs while a game may still be running, and the game comes first.
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process);
    CloseHandle(write_end);
    if (!started) {
      CloseHandle(read_end);
      std::lock_guard<std::mutex> lock(mutex_);
      job->state = "failed";
      job->error = "the export could not be started";
      break;
    }
    CloseHandle(process.hThread);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job->process = process.hProcess;
      if (job->cancel) TerminateProcess(process.hProcess, 1);
    }
    std::string pending, failure;
    bool exported = false;
    double reported = -1.0;
    char buffer[512];
    DWORD read = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
      pending.append(buffer, read);
      size_t newline = 0;
      while ((newline = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("PROGRESS ", 0) == 0) {
          // The second pass (a file that came out over the limit) is the last stretch of the bar.
          const double share = std::atof(line.c_str() + 9);
          const double overall = attempt == 0 ? share * 0.9 : 0.9 + share * 0.1;
          if (overall - reported >= 0.02) {
            reported = overall;
            {
              std::lock_guard<std::mutex> lock(mutex_);
              job->progress = overall;
            }
            publish(job);
          }
        } else if (line.rfind("EXPORT_OK", 0) == 0) {
          exported = true;
        } else if (line.rfind("EXPORT_FAIL ", 0) == 0) {
          failure = line.substr(12);
        }
      }
    }
    CloseHandle(read_end);
    WaitForSingleObject(process.hProcess, 10000);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job->process = nullptr;
    }
    CloseHandle(process.hProcess);
    if (job->cancel) return;  // the sheet closed: cancel() cleans up
    if (!exported) {
      std::lock_guard<std::mutex> lock(mutex_);
      job->state = "failed";
      job->error = failure.empty() ? "the export stopped" : failure;
      break;
    }
    const int64_t bytes = file_size(job->path);
    if (bytes > 0 && bytes <= job->limit_bytes) {
      std::lock_guard<std::mutex> lock(mutex_);
      job->bytes = bytes;
      job->fit.video_kbps = kbps;
      job->state = "ready";
      job->progress = 1.0;
      break;
    }
    // Over the limit (an encoder that overshoots its rate): once more, smaller. An encoder that will not
    // go below the rate its picture costs ignores a lower number rather than honouring it - measured, the
    // same file came out at every rate under that floor - so the picture steps down the ladder and the
    // rate follows only when there is no step left.
    BX_LOG_INFO("share %s came out at %lld bytes over a %lld limit; fitting again", job->id.c_str(),
                static_cast<long long>(bytes), static_cast<long long>(job->limit_bytes));
    if (attempt == 1 || bytes <= 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      job->state = "failed";
      job->error = "the clip did not come out small enough - trim it in the editor first";
      break;
    }
    const ShareFit smaller = share_fit_smaller(job->fit);
    if (smaller.width > 0) {
      job->fit.width = smaller.width;
      job->fit.height = smaller.height;
    } else {
      const double scale = static_cast<double>(job->limit_bytes) / static_cast<double>(bytes) * 0.92;
      kbps = std::max(250, static_cast<int>(kbps * scale));
    }
  }
  DeleteFileW(utf8_to_wide(job->folder + "\\job.json").c_str());
  std::string state;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state = job->state;
  }
  BX_LOG_INFO("share %s: %s", job->id.c_str(), state.c_str());
  publish(job);
}

bool ShareService::get(const std::string& id, Json& reply, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = jobs_.find(id);
  if (found == jobs_.end()) {
    error = "no such share";
    return false;
  }
  reply = job_json_locked(*found->second);
  return true;
}

bool ShareService::copy(const std::string& id, std::string& error) {
  const std::shared_ptr<Job> job = find(id);
  std::string path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!job || (job->state != "ready" && job->state != "posted")) {
      error = job ? "the clip is not ready yet" : "no such share";
      return false;
    }
    path = job->path;
  }
  if (!copy_files_to_clipboard({path})) {
    error = "the clipboard could not be set";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job->used = true;
  }
  publish(job);
  return true;
}

bool ShareService::post_discord(const std::string& id, const std::string& message, const std::string& webhook_id,
                                Json& reply, std::string& error) {
  std::string url, destination;
  if (!load_webhook(webhook_id, url, destination, error)) return false;
  const std::shared_ptr<Job> job = find(id);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!job || (job->state != "ready" && job->state != "posted")) {
      error = job ? "the clip is not ready yet" : "no such share";
      return false;
    }
    if (job->fit.copy && job->source_bytes > job->limit_bytes) {
      error = "it is larger than the channel takes - turn on Make it ready for Discord";
      return false;
    }
    if (stopped_) {
      error = "BuffrX is closing";
      return false;
    }
    job->state = "posting";
    job->destination = destination;
    job->progress = 0.0;
    job->error.clear();
    ++running_;
    reply = job_json_locked(*job);
  }
  std::shared_ptr<ShareService> self = shared_from_this();
  std::thread([self, job, url, message] {
    self->run_post(job, url, message);
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (--self->running_ == 0) self->idle_.notify_all();
  }).detach();
  return true;
}

void ShareService::run_post(std::shared_ptr<Job> job, std::string url, std::string message) {
  double reported = -1.0;
  auto progress = [&](double share) {
    if (share < 1.0 && share - reported < 0.02) return;
    reported = share;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job->progress = share;
    }
    publish(job);
  };
  WebhookPost post = discord_webhook_post(url, job->path, message, progress, &job->cancel);
  if (!post.ok && post.status == 429 && !job->cancel) {
    // Discord's own rate limit: wait what it asked (ten seconds at most) and try once more.
    const double wait = std::clamp(post.retry_after, 0.5, 10.0);
    for (double waited = 0; waited < wait && !job->cancel; waited += 0.1) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!job->cancel) post = discord_webhook_post(url, job->path, message, progress, &job->cancel);
  }
  if (job->cancel) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (post.ok) {
      job->state = "posted";
      job->used = true;
      job->progress = 1.0;
    } else {
      // Back to ready: the file is still good, and the sheet can try again or copy it instead.
      job->state = "ready";
      job->error = post.error;
    }
  }
  BX_LOG_INFO("share %s: %s", job->id.c_str(), post.ok ? "posted to Discord" : post.error.c_str());
  publish(job);
}

bool ShareService::cancel(const std::string& id, std::string& error) {
  std::shared_ptr<Job> job;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(id);
    if (found == jobs_.end()) {
      error = "no such share";
      return false;
    }
    job = found->second;
    // Set before anything else: this is the flag the post's upload checks between chunks, and what the
    // fit's export process is stopped with.
    job->cancel = true;
    jobs_.erase(found);
    if (job->process) TerminateProcess(job->process, 1);
  }
  if (job->used || job->folder.empty()) return true;
  // A file nobody used goes now. A running fit or post is stopped first; its thread may still hold the
  // file for a moment (a post can sit in a stalled socket write for up to 30 s), so the folder is
  // removed from a thread that waits for it, and says so once when it never goes.
  const std::string folder = job->folder;
  const std::string job_id = job->id;
  std::thread([folder, job_id] {
    for (int tries = 0; tries < 300; ++tries) {
      remove_job_folder(folder);
      if (GetFileAttributesW(utf8_to_wide(folder).c_str()) == INVALID_FILE_ATTRIBUTES) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BX_LOG_WARN("share %s: its cached folder could not be removed - a file in it is still open", job_id.c_str());
  }).detach();
  return true;
}

// These helpers run under webhook_mutex_. The old single-webhook file is read in place and is only
// converted on the next successful write, so an upgrade does not lose the saved address.
bool ShareService::read_webhooks(std::vector<WebhookEntry>& hooks, std::string& error) {
  hooks.clear();
  Json stored;
  std::string parse_error;
  const std::string text = read_text(data_root_ + "\\share.json");
  if (text.empty() && GetFileAttributesW(utf8_to_wide(data_root_ + "\\share.json").c_str()) == INVALID_FILE_ATTRIBUTES) return true;
  if (!Json::parse(text, stored, parse_error) || !stored.is_object()) {
    error = "saved Discord destinations could not be read";
    return false;
  }
  if (stored.has("webhooks")) {
    if (!stored["webhooks"].is_array()) { error = "saved Discord destinations could not be read"; return false; }
    for (const Json& item : stored["webhooks"].items()) {
      WebhookEntry entry{item["id"].as_string(), item["name"].as_string(), item["webhook"].as_string()};
      if (entry.id.empty() || entry.name.empty() || entry.sealed.empty() || webhook_find(hooks, entry.id)) {
        error = "saved Discord destinations could not be read";
        return false;
      }
      hooks.push_back(std::move(entry));
    }
  } else if (!stored["webhook"].as_string().empty()) {
    std::string url;
    if (!unprotect_secret(stored["webhook"].as_string(), url) || !discord_webhook_valid(url)) {
      error = "the saved Discord webhook could not be opened";
      return false;
    }
    hooks.push_back({discord_webhook_id(url), discord_webhook_hint(url), stored["webhook"].as_string()});
  }
  return true;
}

bool ShareService::write_webhooks(const std::vector<WebhookEntry>& hooks, std::string& error) {
  Json stored = Json::object(), list = Json::array();
  for (const auto& hook : hooks) {
    Json item = Json::object();
    item.set("id", Json(hook.id));
    item.set("name", Json(hook.name));
    item.set("webhook", Json(hook.sealed));
    list.push(item);
  }
  stored.set("webhooks", list);
  // A rolled-back engine can still read the first destination in its original single-webhook field.
  if (!hooks.empty()) stored.set("webhook", Json(hooks.front().sealed));
  if (write_file_atomic(data_root_ + "\\share.json", stored.dump(2))) return true;
  error = "the Discord destinations could not be saved";
  return false;
}

bool ShareService::load_webhook(const std::string& id, std::string& url, std::string& name, std::string& error) {
  std::lock_guard<std::mutex> lock(webhook_mutex_);
  std::vector<WebhookEntry> hooks;
  if (!read_webhooks(hooks, error)) return false;
  const WebhookEntry* hook = webhook_find(hooks, id);
  if (!hook) {
    error = id.empty() ? "no Discord webhook is set - add one in Settings, Sharing"
                       : "that Discord destination is no longer saved - choose another";
    return false;
  }
  if (!unprotect_secret(hook->sealed, url) || discord_webhook_id(url) != hook->id) {
    error = "the saved Discord webhook could not be opened";
    return false;
  }
  name = hook->name;
  return true;
}

Json ShareService::webhook_json() {
  std::lock_guard<std::mutex> lock(webhook_mutex_);
  std::vector<WebhookEntry> hooks;
  std::string error;
  if (!read_webhooks(hooks, error)) hooks.clear();
  Json json = Json::object(), list = Json::array();
  for (const auto& hook : hooks) {
    Json entry = Json::object();
    entry.set("id", Json(hook.id));
    entry.set("name", Json(hook.name));
    entry.set("hint", Json("webhook \xE2\x80\xA6" + hook.id.substr(hook.id.size() > 4 ? hook.id.size() - 4 : 0)));
    list.push(entry);
  }
  json.set("set", Json(!hooks.empty()));
  json.set("hint", Json(hooks.empty() ? std::string() : list.items()[0]["hint"].as_string()));
  json.set("webhooks", list);
  if (!error.empty()) json.set("error", Json(error));
  return json;
}

bool ShareService::set_webhook(const std::string& url, const std::string& name, bool add, std::string& error) {
  if (!discord_webhook_valid(url)) {
    error = "that is not a Discord webhook address (it starts https://discord.com/api/webhooks/)";
    return false;
  }
  if (add && (name.empty() || name.size() > 120 || name.find_first_not_of(" \t\r\n") == std::string::npos)) {
    error = "enter a server name of up to 120 bytes";
    return false;
  }
  const std::string sealed = protect_secret(url);
  if (sealed.empty()) {
    error = "the webhook could not be saved";
    return false;
  }
  std::lock_guard<std::mutex> lock(webhook_mutex_);
  std::vector<WebhookEntry> hooks;
  if (!read_webhooks(hooks, error)) return false;
  if (!add) hooks.clear();  // the original protocol command replaced the one saved webhook
  webhook_upsert(hooks, {discord_webhook_id(url), name.empty() ? discord_webhook_hint(url) : name, sealed});
  if (!write_webhooks(hooks, error)) return false;
  BX_LOG_INFO("a Discord webhook was set (%s)", discord_webhook_hint(url).c_str());
  return true;
}

bool ShareService::clear_webhook(const std::string& id, std::string& error) {
  std::lock_guard<std::mutex> lock(webhook_mutex_);
  std::vector<WebhookEntry> hooks;
  if (!read_webhooks(hooks, error)) return false;
  if (id.empty()) hooks.clear();  // an older client removes its only known destination
  else if (!webhook_remove(hooks, id)) { error = "that Discord destination is no longer saved"; return false; }
  if (!write_webhooks(hooks, error)) return false;
  BX_LOG_INFO("the Discord webhook was removed");
  return true;
}

void ShareService::shutdown() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopped_ && running_ == 0) return;
  stopped_ = true;
  for (auto& [id, job] : jobs_) {
    job->cancel = true;
    if (job->process) TerminateProcess(job->process, 1);
  }
  if (!idle_.wait_for(lock, std::chrono::seconds(10), [this] { return running_ == 0; })) {
    BX_LOG_WARN("%d share jobs were still running at shutdown", running_);
  }
}

}  // namespace bx
