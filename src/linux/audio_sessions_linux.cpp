#include "../engine/audio_sessions.h"
namespace bx {

std::vector<AudioSessionEntry> audio_session_snapshot(std::string& error) {
  error = "the Windows audio-session list is not available on Linux";
  return {};
}

std::vector<std::string> list_audio_sessions(std::string& error) {
  error = "the Windows audio-session list is not available on Linux";
  return {};
}

}  // namespace bx
