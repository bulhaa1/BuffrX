// The update check's own rules: how two version tags compare, when a check is due, and whether a version
// is worth showing. No window, no network, no clock of its own - the caller hands in both times.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace bx {

// A day between automatic checks. A manual "check now" ignores it.
inline constexpr int64_t kUpdateIntervalMs = 24LL * 60 * 60 * 1000;

// Automatic download and install (update_policy.h). Three download tries two minutes apart, three install
// tries before the product stops trying and explains itself, and room on the disk before a byte is fetched.
inline constexpr int kUpdateDownloadAttempts = 3;
inline constexpr int kUpdateInstallAttempts = 3;
inline constexpr int64_t kUpdateRetryMs = 2LL * 60 * 1000;
inline constexpr int64_t kUpdateMinFreeBytes = 200LL * 1024 * 1024;

// "0.6.0", "v0.6.0", "0.6.0-alpha.1". A tag this does not understand has valid == false, and nothing may
// start an update from one.
struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::vector<std::string> pre;
  bool valid = false;
};

namespace detail {

inline bool version_digits(const std::string& text) {
  if (text.empty()) return false;
  for (const char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
  }
  return true;
}

inline std::vector<std::string> version_parts(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string current;
  for (const char c : text) {
    if (c == separator) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(current);
  return parts;
}

}  // namespace detail

inline Version parse_version(const std::string& tag) {
  Version version;
  std::string text = tag;
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.erase(text.begin());
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.pop_back();
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.erase(text.begin());

  const size_t dash = text.find('-');
  const std::string numbers = dash == std::string::npos ? text : text.substr(0, dash);
  const std::string pre = dash == std::string::npos ? std::string() : text.substr(dash + 1);

  const std::vector<std::string> parts = detail::version_parts(numbers, '.');
  if (parts.empty() || parts.size() > 3) return version;
  for (const std::string& part : parts) {
    if (!detail::version_digits(part)) return version;
  }
  // A number wider than an int is a tag nobody can mean; refuse it instead of wrapping.
  auto to_int = [](const std::string& part) -> long long {
    long long value = 0;
    for (const char c : part) {
      value = value * 10 + (c - '0');
      if (value > 1000000) return -1;
    }
    return value;
  };
  version.major = static_cast<int>(to_int(parts[0]));
  version.minor = parts.size() > 1 ? static_cast<int>(to_int(parts[1])) : 0;
  version.patch = parts.size() > 2 ? static_cast<int>(to_int(parts[2])) : 0;
  if (version.major < 0 || version.minor < 0 || version.patch < 0) return version;

  if (dash != std::string::npos) {
    if (pre.empty()) return version;
    version.pre = detail::version_parts(pre, '.');
    for (const std::string& part : version.pre) {
      if (part.empty()) return Version{};
      for (const char c : part) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return Version{};
      }
    }
  }
  version.valid = true;
  return version;
}

// Negative when a is older, zero when they are the same release, positive when a is newer.
inline int compare_versions(const Version& a, const Version& b) {
  if (!a.valid || !b.valid) return 0;
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  // A release outranks any prerelease of the same numbers; "alpha" is older than "alpha.1".
  if (a.pre.empty() && b.pre.empty()) return 0;
  if (a.pre.empty()) return 1;
  if (b.pre.empty()) return -1;
  const size_t shared = a.pre.size() < b.pre.size() ? a.pre.size() : b.pre.size();
  for (size_t i = 0; i < shared; ++i) {
    const std::string& left = a.pre[i];
    const std::string& right = b.pre[i];
    const bool left_digits = detail::version_digits(left);
    const bool right_digits = detail::version_digits(right);
    if (left_digits && right_digits) {
      const size_t li = left.find_first_not_of('0');
      const size_t ri = right.find_first_not_of('0');
      const std::string l = li == std::string::npos ? "0" : left.substr(li);
      const std::string r = ri == std::string::npos ? "0" : right.substr(ri);
      if (l.size() != r.size()) return l.size() < r.size() ? -1 : 1;
      if (l != r) return l < r ? -1 : 1;
    } else if (left_digits != right_digits) {
      return left_digits ? -1 : 1;
    } else if (left != right) {
      return left < right ? -1 : 1;
    }
  }
  if (a.pre.size() == b.pre.size()) return 0;
  return a.pre.size() < b.pre.size() ? -1 : 1;
}

inline bool update_is_newer(const std::string& tag, const std::string& current) {
  const Version latest = parse_version(tag);
  const Version running = parse_version(current);
  if (!latest.valid || !running.valid) return false;
  return compare_versions(latest, running) > 0;
}

// True when a check has never run, when the stored time is in the future (a clock that moved), or when a
// day has passed. Both times are wall clock, in milliseconds.
inline bool update_check_due(int64_t last_ms, int64_t now_ms) {
  if (last_ms <= 0) return true;
  if (last_ms > now_ms) return true;
  return now_ms - last_ms >= kUpdateIntervalMs;
}

inline bool update_should_check(bool visible, bool buffering, bool checking, int64_t last_ms, int64_t now_ms) {
  return (visible || buffering) && !checking && update_check_due(last_ms, now_ms);
}

// The panel's icon: a newer version than the one running, which the user has not already been shown.
inline bool update_should_offer(const std::string& latest, const std::string& offered, const std::string& current) {
  if (!update_is_newer(latest, current)) return false;
  return latest != offered;
}

// Automatic install: a newer release this build can verify. A release without a digest is still announced,
// but it opens in the browser, because nothing is ever run that was not checked.
inline bool update_is_installable(const std::string& latest, const std::string& current, const std::string& digest) {
  return update_is_newer(latest, current) && !digest.empty();
}

// The badge itself: while a newer release is known it stays, even after the person pressed Later. Later
// dismisses the prompt, not the update.
inline bool update_should_badge(const std::string& latest, const std::string& current) {
  return update_is_newer(latest, current);
}

// Whether the prompt is the user's to see: once per version.
inline bool update_should_prompt(const std::string& latest, const std::string& offered, const std::string& current) {
  return update_should_badge(latest, current) && latest != offered;
}

// A restart to install may stop the buffer and end a Timeline session, but never cut a manual recording: the
// installer would close the engine mid-file (recovery_policy.h's rule for a restart).
inline bool update_apply_allowed(bool manual_recording) { return !manual_recording; }

// A silent install that did not put the new version in place this many times is not tried again.
inline bool update_install_exhausted(int attempts) { return attempts >= kUpdateInstallAttempts; }

// The sheet that opens once after a silent install: only when this run is the version that was just put in
// place, and this run has not shown it yet. A fresh install (no version seen, no pending) never opens it.
inline bool update_should_open_notes(const std::string& version_now, const std::string& version_seen,
                                     const std::string& pending_version) {
  if (version_now.empty() || pending_version.empty()) return false;
  if (pending_version != version_now) return false;
  return version_seen != version_now;
}

}  // namespace bx
