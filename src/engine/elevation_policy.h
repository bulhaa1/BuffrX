// A normal engine launch can come from the launcher, Windows startup, or a link.
#pragma once

#include <string>

namespace bx {

inline bool elevation_needed(bool requested, bool elevated, bool utility) {
  return requested && !elevated && !utility;
}

// Keep the original argument text, including quoting, when Windows starts the elevated copy.
inline std::wstring elevation_arguments(const std::wstring& command) {
  size_t at = 0;
  if (at < command.size() && command[at] == L'"') {
    ++at;
    while (at < command.size() && command[at] != L'"') ++at;
    if (at < command.size()) ++at;
  } else {
    while (at < command.size() && command[at] != L' ' && command[at] != L'\t') ++at;
  }
  while (at < command.size() && (command[at] == L' ' || command[at] == L'\t')) ++at;
  return command.substr(at);
}

}  // namespace bx
