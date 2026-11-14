#include "../engine/audio_sessions.h"
namespace bx {
std::vector<std::string> list_audio_sessions(std::string& error) {
  error = "the Windows audio-session list is not available on Linux";
  return {};
}
}  // namespace bx
