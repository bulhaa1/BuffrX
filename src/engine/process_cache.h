// The executable behind a window, remembered. Reading it is OpenProcess + QueryFullProcessImageName, and the capture-target
// check used to do that for every capturable window every 2 s under the engine lock. A window's process never changes, so the
// name is read once per (window, process): the pair is the key, so a window handle that a later window reuses carries its own
// process id and reads again. Pure, with the reader passed in, so the rule is tested.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace bx {

class ProcessNameCache {
public:
  // Entries are dropped all at once past this many: the windows of a desktop number in the hundreds, and a stale entry for a
  // window that is gone only costs a few bytes until then.
  static constexpr size_t kLimit = 512;

  std::string get(uintptr_t window, uint32_t pid, const std::function<std::string(uint32_t)>& read) {
    if (pid == 0) return std::string();
    const auto key = std::make_pair(window, pid);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = names_.find(key);
      if (found != names_.end()) return found->second;
    }
    std::string name = read(pid);
    if (name.empty()) return name;  // a process that could not be opened is asked again next time
    std::lock_guard<std::mutex> lock(mutex_);
    if (names_.size() >= kLimit) names_.clear();
    names_[key] = name;
    return name;
  }

private:
  std::mutex mutex_;
  std::map<std::pair<uintptr_t, uint32_t>, std::string> names_;
};

}  // namespace bx
