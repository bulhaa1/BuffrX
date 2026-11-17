// The Windows audio-session watcher has no Linux counterpart. Automatic app audio is built on WASAPI
// session events and Windows process loopback, and neither exists here: the port records the desktop
// and the microphone. This watcher stays stopped and says why, so the automatic page reports the mode
// as unsupported instead of showing an empty list that looks like silence.
#include "../engine/audio_watch.h"

namespace bx {

struct AudioSessionWatcher::Impl {};

AudioSessionWatcher::AudioSessionWatcher() : impl_(std::make_shared<Impl>()) {}

AudioSessionWatcher::~AudioSessionWatcher() = default;

bool AudioSessionWatcher::start(Wake) { return false; }

bool AudioSessionWatcher::begin(Wake) { return false; }

bool AudioSessionWatcher::ready() const { return false; }

void AudioSessionWatcher::stop() {}

bool AudioSessionWatcher::running() const { return false; }

std::vector<WatchedSession> AudioSessionWatcher::sessions(uint64_t, uint32_t) const { return {}; }

uint64_t AudioSessionWatcher::generation() const { return 0; }

std::string AudioSessionWatcher::error() const {
  return "automatic app audio reads Windows audio sessions; on Linux BuffrX records the desktop";
}

}  // namespace bx
