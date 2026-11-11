#include "share_math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

#include "../shared/json.h"

namespace bx {
namespace {

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool all_digits(const std::string& text, size_t from, size_t to) {
  if (to <= from) return false;
  for (size_t i = from; i < to; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
  }
  return true;
}

// The parts of a valid webhook address: where the id starts and ends. False when it is not one.
bool parse_webhook(const std::string& url, size_t& id_from, size_t& id_to) {
  static const std::string scheme = "https://";
  if (url.size() > 512 || url.compare(0, scheme.size(), scheme) != 0) return false;
  const size_t host_end = url.find('/', scheme.size());
  if (host_end == std::string::npos) return false;
  // The host as typed, compared whole: "discord.com.evil.io", "user@discord.com" and "discord.com:8443"
  // are all something other than Discord.
  const std::string host = lower(url.substr(scheme.size(), host_end - scheme.size()));
  if (host != "discord.com" && host != "ptb.discord.com" && host != "canary.discord.com" && host != "discordapp.com") {
    return false;
  }
  size_t at = host_end;
  static const std::string api = "/api/";
  if (url.compare(at, api.size(), api) != 0) return false;
  at += api.size();
  // An optional API version: /api/v10/webhooks/...
  if (at < url.size() && url[at] == 'v') {
    const size_t slash = url.find('/', at);
    if (slash == std::string::npos || !all_digits(url, at + 1, slash) || slash - at > 4) return false;
    at = slash + 1;
  }
  static const std::string webhooks = "webhooks/";
  if (url.compare(at, webhooks.size(), webhooks) != 0) return false;
  at += webhooks.size();
  const size_t id_end = url.find('/', at);
  if (id_end == std::string::npos || id_end - at > 25 || !all_digits(url, at, id_end)) return false;
  const size_t token_from = id_end + 1;
  if (token_from >= url.size() || url.size() - token_from > 128) return false;
  // A token is letters, digits, '-' and '_': no '.', '?', '%', '/' or space can follow, so there is no
  // query, no path walk and no second address hiding in it.
  for (size_t i = token_from; i < url.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(url[i]);
    if (!std::isalnum(c) && c != '-' && c != '_') return false;
  }
  id_from = at;
  id_to = id_end;
  return true;
}

std::string content_type_for(const std::string& filename) {
  const size_t dot = filename.find_last_of('.');
  const std::string extension = dot == std::string::npos ? std::string() : lower(filename.substr(dot));
  if (extension == ".mp4") return "video/mp4";
  if (extension == ".mkv") return "video/x-matroska";
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  return "application/octet-stream";
}

}  // namespace

int64_t share_limit_bytes(int limit_mib) {
  const int64_t mib = limit_mib <= 0 ? 20 : std::min(limit_mib, 500);
  return mib * 1024 * 1024;
}

ShareFit share_fit(double duration_seconds, int64_t source_bytes, int source_width, int source_height,
                   int64_t limit_bytes) {
  ShareFit fit;
  if (!(duration_seconds > 0.05) || source_width <= 0 || source_height <= 0 || limit_bytes <= 0) return fit;
  if (source_bytes > 0 && source_bytes <= limit_bytes) {
    fit.fits = true;
    fit.copy = true;
    fit.width = source_width;
    fit.height = source_height;
    return fit;
  }
  // One AAC track at 128 kbit/s: the mix, which is the track Discord's player plays.
  fit.audio_kbps = 128;
  const double budget = static_cast<double>(limit_bytes) * 8.0 / 1000.0 * 0.92 / duration_seconds;
  const double video = budget - fit.audio_kbps;
  // The steps on the short side, from the clip's own size down. The floor is the rate below which a
  // game at that size turns to blocks in motion (about 0.0018 kbit/s per pixel: 3.7 Mbps at 1080p,
  // 1.7 at 720p, 0.9 at 540p); a smaller picture at a decent rate looks better than a large one that
  // is starved.
  const int short_side = std::min(source_width, source_height);
  std::vector<int> steps = {short_side};
  for (int step : {1440, 1080, 720, 540, 480, 360}) {
    if (step < short_side) steps.push_back(step);
  }
  int chosen_width = 0, chosen_height = 0;
  int smallest_width = 0, smallest_height = 0;
  for (int step : steps) {
    const double scale = static_cast<double>(step) / short_side;
    const int width = std::max(2, static_cast<int>(std::lround(source_width * scale)) & ~1);
    const int height = std::max(2, static_cast<int>(std::lround(source_height * scale)) & ~1);
    smallest_width = width;
    smallest_height = height;
    if (video >= static_cast<double>(width) * height * 0.0018) {
      chosen_width = width;
      chosen_height = height;
      break;
    }
  }
  if (chosen_width == 0) {
    // Nothing reaches its floor. Below 250 kbit/s even 360p is a slideshow: say it does not fit.
    if (video < 250.0) return fit;
    chosen_width = smallest_width;
    chosen_height = smallest_height;
  }
  fit.fits = true;
  fit.width = chosen_width;
  fit.height = chosen_height;
  fit.video_kbps = static_cast<int>(std::min(video, 150000.0));
  return fit;
}

ShareFit share_fit_smaller(const ShareFit& fit) {
  ShareFit smaller;
  if (fit.width <= 0 || fit.height <= 0) return smaller;
  const int short_side = std::min(fit.width, fit.height);
  int step = 0;
  for (int candidate : {1440, 1080, 720, 540, 480, 360}) {
    if (candidate < short_side) {
      step = candidate;
      break;
    }
  }
  if (step == 0) return smaller;
  const double scale = static_cast<double>(step) / short_side;
  smaller.width = std::max(2, static_cast<int>(std::lround(fit.width * scale)) & ~1);
  smaller.height = std::max(2, static_cast<int>(std::lround(fit.height * scale)) & ~1);
  smaller.video_kbps = fit.video_kbps;
  smaller.audio_kbps = fit.audio_kbps;
  smaller.fits = true;
  return smaller;
}

bool discord_webhook_valid(const std::string& url) {  size_t from = 0, to = 0;
  return parse_webhook(url, from, to);
}

std::string discord_webhook_id(const std::string& url) {
  size_t from = 0, to = 0;
  return parse_webhook(url, from, to) ? url.substr(from, to - from) : std::string();
}

std::string discord_webhook_hint(const std::string& url) {
  size_t from = 0, to = 0;
  if (!parse_webhook(url, from, to)) return std::string();
  const size_t keep = std::min<size_t>(4, to - from);
  return "webhook \xE2\x80\xA6" + url.substr(to - keep, keep);
}

std::string discord_payload_json(const std::string& content) {
  Json mentions = Json::object();
  mentions.set("parse", Json::array());
  Json payload = Json::object();
  payload.set("allowed_mentions", mentions);
  payload.set("content", Json(content.substr(0, 1900)));  // Discord's own cap is 2000
  return payload.dump();
}

std::string multipart_head(const std::string& boundary, const std::string& payload_json, const std::string& filename) {
  std::string safe = filename;
  for (char& c : safe) {
    if (c == '"' || c == '\r' || c == '\n' || c == '\\') c = '_';
  }
  std::string head;
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"payload_json\"\r\n";
  head += "Content-Type: application/json\r\n\r\n";
  head += payload_json + "\r\n";
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"files[0]\"; filename=\"" + safe + "\"\r\n";
  head += "Content-Type: " + content_type_for(safe) + "\r\n\r\n";
  return head;
}

std::string multipart_tail(const std::string& boundary) { return "\r\n--" + boundary + "--\r\n"; }

}  // namespace bx
