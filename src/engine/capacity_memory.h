// What this machine encoded in real time, remembered between launches (capacity.json).
//
// The governor settles at a step, and that step is remembered so the next launch starts where the last
// one settled instead of spending a minute of dropped frames to learn the same thing. The record has to be
// forgotten when the person asks for full speed again: otherwise the next launch reads it back and the
// reset looks like it did nothing.
//
// Header-only, built on the shared JSON and file helpers, so the rules are held by the test harness that
// links no libobs.

#ifndef BUFFRX_CAPACITY_MEMORY_H
#define BUFFRX_CAPACITY_MEMORY_H

#include <cstdio>
#include <string>

#include "../shared/files.h"
#include "../shared/json.h"

namespace bx {

// One key per question: what changes how hard the encoder works. A different key is a different
// question, and the ladder is asked again.
inline std::string capacity_key(const std::string& codec, const std::string& encoder, int width, int height, int fps,
                                const std::string& preset) {
  const int w = width > 0 ? width : 1920;
  const int h = height > 0 ? height : 1080;
  return codec + "|" + encoder + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(fps) + "|" +
         preset;
}

inline bool capacity_parse_file(const std::string& path, Json& json) {
  if (path.empty()) return false;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  std::string text;
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
  std::fclose(file);
  std::string error;
  if (!Json::parse(text, json, error)) return false;
  return json.is_object();
}

inline int capacity_read_step(const std::string& path, const std::string& key) {
  Json json;
  if (!capacity_parse_file(path, json) || !json.has(key)) return 0;
  return static_cast<int>(json[key].as_int(0));
}

inline void capacity_write_step(const std::string& path, const std::string& key, int step) {
  if (path.empty()) return;
  Json json = Json::object();
  capacity_parse_file(path, json);
  json.set(key, Json(step));
  write_file_atomic(path, json.dump(2));
}

// Drop this key so the next launch cannot re-apply a step the person has reset. Other keys are kept:
// another codec, size or rate is still a question this machine has already answered.
inline void capacity_forget_key(const std::string& path, const std::string& key) {
  if (path.empty()) return;
  Json json = Json::object();
  if (!capacity_parse_file(path, json)) return;
  if (!json.has(key)) return;
  Json kept = Json::object();
  for (const auto& field : json.fields()) {
    if (field.first != key) kept.set(field.first, field.second);
  }
  write_file_atomic(path, kept.dump(2));
}

}  // namespace bx

#endif  // BUFFRX_CAPACITY_MEMORY_H
