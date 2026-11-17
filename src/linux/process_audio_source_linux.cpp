// Windows process-loopback capture has no Linux counterpart. Automatic app audio never reaches this
// definition here (audio_watch_linux.cpp reports the mode as unsupported), so registering nothing and
// answering with an empty state keeps the auto-audio code linkable without inventing a capture.
#include "../engine/process_audio_source.h"

namespace bx {

void register_process_audio_source() {}

ProcessAudioState process_audio_state(obs_source*) { return ProcessAudioState{}; }

}  // namespace bx
