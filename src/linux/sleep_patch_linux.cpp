#include "../engine/sleep_patch.h"
namespace bx {
bool install_sleep_patch(std::string& note) {
  note = "the frame wait is libobs's own pthread sleep on Linux";
  return false;
}
}  // namespace bx
