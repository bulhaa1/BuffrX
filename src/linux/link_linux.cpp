// BuffrX links on Linux: the iroh helper is built for Windows next to the engine; the service says
// so plainly until the helper is packaged for Linux too.
#include "../engine/link.h"

namespace bx {
namespace {
constexpr const char* kUnavailable = "BuffrX links are not available in the Linux build yet";
}

LinkService::LinkService(std::string, std::string) {}
LinkService::~LinkService() = default;
void LinkService::configure(const std::string&, const std::string&, int) {}
bool LinkService::share(const std::string&, int, Json&, std::string& error) {
  error = kUnavailable;
  return false;
}
void LinkService::set_short_url(const std::string&, const std::string&, const std::string&) {}
bool LinkService::stop(const std::string&, std::string& error) {
  error = kUnavailable;
  return false;
}
Json LinkService::list() {
  Json data = Json::object();
  data.set("links", Json::array());
  data.set("receives", Json::array());
  return data;
}
bool LinkService::open(const std::string&, const std::string&, const std::string&, Json&, std::string& error) {
  error = kUnavailable;
  return false;
}
bool LinkService::answer(const std::string&, bool, std::string& error) {
  error = kUnavailable;
  return false;
}
bool LinkService::cancel(const std::string&, std::string& error) {
  error = kUnavailable;
  return false;
}
void LinkService::set_gaming(bool) {}
void LinkService::tick() {}
void LinkService::on_event(const Json&) {}
void LinkService::shutdown() {}

}  // namespace bx
