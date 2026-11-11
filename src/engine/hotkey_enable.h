#pragma once

#include <cstdint>
#include <string>

namespace bx {

// An off-feature warning arms only the same action and feature, for a short time.
class HotkeyEnable {
public:
  bool press(const std::string& action, const std::string& feature, uint32_t now) {
    if (armed_ && action_ == action && feature_ == feature && now - armed_at_ <= kWindowMs) {
      clear();
      return true;
    }
    action_ = action;
    feature_ = feature;
    armed_at_ = now;
    armed_ = true;
    return false;
  }

  void clear() { armed_ = false; }
  static constexpr uint32_t kWindowMs = 10000;

private:
  std::string action_;
  std::string feature_;
  uint32_t armed_at_ = 0;
  bool armed_ = false;
};

}  // namespace bx
