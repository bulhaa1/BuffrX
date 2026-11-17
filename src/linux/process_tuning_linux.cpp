#include "../engine/process_tuning.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "../shared/log.h"

namespace buffrx {
namespace {
TuningSet g_active;
std::string trimmed_lower(const std::string& text) {
  size_t first = 0, last = text.size();
  while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
  std::string out = text.substr(first, last - first);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}
}  // namespace

TuningSet parse_tuning(const std::string& spec) {
  TuningSet set;
  size_t at = 0;
  while (at <= spec.size()) {
    const size_t comma = spec.find(',', at);
    const std::string word = trimmed_lower(spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
    if (word == "eco") set.efficiency_cores = true;
    else if (word == "below") set.below_normal = true;
    else if (word == "io") set.background_io = true;
    else if (word.rfind("gpu:", 0) == 0) {
      char* end = nullptr;
      const std::string number = word.substr(4);
      const long value = std::strtol(number.c_str(), &end, 10);
      if (!number.empty() && end && *end == '\0') set.gpu_priority = static_cast<int>(std::max(-7L, std::min(0L, value)));
    }
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  return set;
}
std::string tuning_describe(const TuningSet& set) {
  std::string out;
  auto add = [&out](const std::string& word) { out += (out.empty() ? "" : ",") + word; };
  if (set.gpu_priority != 0) add("gpu:" + std::to_string(set.gpu_priority));
  if (set.efficiency_cores) add("eco");
  if (set.below_normal) add("below");
  if (set.background_io) add("io");
  return out.empty() ? "none" : out;
}
const TuningSet& active_tuning() { return g_active; }
void set_active_tuning(const TuningSet& set) { g_active = set; }
bool cpu_has_efficiency_cores() { return false; }
bool apply_process_tuning(const TuningSet& set) {
  BX_LOG_INFO("tuning: Linux does not apply the Windows scheduling hints (%s)", tuning_describe(set).c_str());
  return true;
}
bool apply_gpu_priority(void*, int) { return true; }
bool begin_background_io() { return false; }
}  // namespace buffrx
