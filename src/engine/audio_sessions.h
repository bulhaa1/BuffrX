// The audio sessions on a device, and who owns them.
#pragma once

#include <string>
#include <vector>

namespace bx {

// One line per session on the default output device: the process, the state and whether the session
// is a capture (a loopback client is one). EarTrumpet shows the same list, which is how "BuffrX
// appears in EarTrumpet" happens: our own desktop audio capture opens a loopback client on the
// output device and Windows lists it like any other source.
std::vector<std::string> list_audio_sessions(std::string& error);

}  // namespace bx
