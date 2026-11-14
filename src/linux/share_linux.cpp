// Sharing on Linux: the sheet's jobs are not ported yet. The class says so in the interface's own
// words instead of failing silently; capture, editing and Discord's file path still work.
#include "../engine/share.h"

namespace bx {
namespace {
constexpr const char* kUnavailable = "sharing from the share sheet is not available in the Linux build yet";
}

ShareService::ShareService(std::string data_root, std::string engine_exe)
    : data_root_(std::move(data_root)), cache_root_(data_root_ + "/share-cache"), engine_exe_(std::move(engine_exe)) {}
ShareService::~ShareService() = default;

bool ShareService::prepare(const std::string&, int, bool, Json&, std::string& error) {
  error = kUnavailable;
  return false;
}
bool ShareService::get(const std::string&, Json&, std::string& error) {
  error = kUnavailable;
  return false;
}
bool ShareService::copy(const std::string&, std::string& error) {
  error = kUnavailable;
  return false;
}
bool ShareService::post_discord(const std::string&, const std::string&, const std::string&, Json&, std::string& error) {
  error = kUnavailable;
  return false;
}
bool ShareService::cancel(const std::string&, std::string& error) {
  error = kUnavailable;
  return false;
}
Json ShareService::webhook_json() { return Json::object(); }
bool ShareService::set_webhook(const std::string&, const std::string&, bool, std::string& error) {
  error = kUnavailable;
  return false;
}
bool ShareService::clear_webhook(const std::string&, std::string& error) {
  error = kUnavailable;
  return false;
}
void ShareService::shutdown() {}

}  // namespace bx
