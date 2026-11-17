// The compatibility report (plan 009, phase 1): what this PC's graphics card and encoders can do, as one local file a
// person attaches to a bug report. Nothing here is sent anywhere.
//
// The file is built from a plain struct so the rule that matters most - what it must never contain - is tested without
// libobs. It carries no file path (the data folder holds the user's name), no game name, no window title and no user name.
// The gathering (DXGI, the registry, libobs) lives in compat_report.cpp and obs_host.cpp and only fills the struct.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../shared/json.h"

namespace bx {

struct CompatAdapter {
  int index = 0;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  std::string name;
  std::string driver;
  uint64_t dedicated_bytes = 0;
  bool software = false;
  bool used_by_libobs = false;
  std::vector<std::string> outputs;  // \\.\DISPLAY1, never a monitor's title
};

struct CompatEncoder {
  std::string id;
  std::string codec;
  bool hardware = false;
  // What obs_get_encoder_properties lists; an encoder is never created to find this out.
  std::vector<std::string> rate_control;
  std::vector<std::string> preset;
  bool has_pre_analysis = false;
  bool has_bf = false;
  bool has_multipass = false;
  bool has_lookahead = false;
  bool has_target_usage = false;
};

struct CompatFacts {
  std::string buffrx_version;
  std::string obs_version;
  int windows_build = 0;
  std::string hags;  // "on", "off" or "unknown"
  std::vector<CompatAdapter> adapters;
  std::string libobs_adapter_reason;
  std::vector<CompatEncoder> encoders;
  std::string running_encoder;  // empty when none runs
  std::vector<std::pair<std::string, Json>> running_settings;
  std::map<std::string, std::string> reported_settings;  // the encoder's own "settings:" block, parsed
  int controller_level = 0;
  std::string controller_label;
  // The watcher's latest completed sample while the buffer runs. `speed_note` says why it is missing.
  bool speed_measured = false;
  uint64_t speed_rendered = 0;
  uint64_t speed_encoded = 0;
  std::string speed_note;
};

// A message the AMF, QSV and NVENC plugins log at info level: their effective settings in one block, e.g.
// "[texture-amf-h264: 'BuffrX video'] settings:\n\trate_control: VBR\n\tpreset: balanced ...".
inline bool encoder_settings_block_is(const std::string& message) { return message.find("] settings:") != std::string::npos; }

inline std::string compat_trim(const std::string& text) {
  size_t from = 0;
  size_t to = text.size();
  while (from < to && (text[from] == ' ' || text[from] == '\t' || text[from] == '\r' || text[from] == '\n')) ++from;
  while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t' || text[to - 1] == '\r' || text[to - 1] == '\n')) --to;
  return text.substr(from, to - from);
}

// The "\tkey: value" lines of a settings block. A key keeps its own spelling ("pre-analysis", "b-frames").
inline std::map<std::string, std::string> encoder_settings_block_parse(const std::string& text) {
  std::map<std::string, std::string> out;
  size_t from = text.find("settings:");
  if (from == std::string::npos) return out;
  from = text.find('\n', from);
  while (from != std::string::npos && from < text.size()) {
    const size_t start = from + 1;
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    from = end < text.size() ? end : std::string::npos;
    if (line.empty() || line[0] != '\t') continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = compat_trim(line.substr(0, colon));
    if (!key.empty()) out[key] = compat_trim(line.substr(colon + 1));
  }
  return out;
}

// A value that looks like a path (a drive, a UNC share, a folder under the user's profile) or carries the machine's name
// is removed: the report is attached to public issues.
inline bool compat_looks_private(const std::string& text) {
  if (text.find(":\\") != std::string::npos || text.find(":/") != std::string::npos) return true;
  if (text.find("\\\\") != std::string::npos) return true;
  if (text.find("/Users/") != std::string::npos || text.find("/home/") != std::string::npos) return true;
  return false;
}

inline std::string compat_text(const std::string& text) { return compat_looks_private(text) ? std::string("(removed)") : text; }

inline Json compat_strings(const std::vector<std::string>& list) {
  Json out = Json::array();
  for (const std::string& item : list) out.push(Json(compat_text(item)));
  return out;
}

inline std::string compat_hex(uint32_t value, int digits) {
  static const char* const symbols = "0123456789abcdef";
  std::string out = "0x";
  for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) out += symbols[(value >> shift) & 0xF];
  return out;
}

inline std::string compat_vendor(uint32_t id) {
  switch (id) {
    case 0x10DE: return "nvidia";
    case 0x1002: return "amd";
    case 0x8086: return "intel";
    default: return "other";
  }
}

// Two adapters, at least one of them not a software renderer and one of them integrated is not knowable from DXGI alone;
// the honest cheap test is "two real adapters from different vendors, or an Intel/AMD one with little memory beside another".
inline bool compat_is_hybrid(const std::vector<CompatAdapter>& adapters) {
  int real = 0;
  bool small = false;
  bool big = false;
  for (const CompatAdapter& adapter : adapters) {
    if (adapter.software) continue;
    ++real;
    if (adapter.dedicated_bytes < (1ull << 30)) small = true;  // an integrated GPU shares system memory
    else big = true;
  }
  return real >= 2 && small && big;
}

inline Json compat_report_json(const CompatFacts& facts) {
  Json out = Json::object();
  out.set("report_version", Json(1));
  out.set("buffrx", Json(compat_text(facts.buffrx_version)));
  out.set("libobs", Json(compat_text(facts.obs_version)));
  Json system = Json::object();
  system.set("windows_build", Json(facts.windows_build));
  system.set("hags", Json(facts.hags.empty() ? std::string("unknown") : compat_text(facts.hags)));
  system.set("hybrid_gpu_laptop_likely", Json(compat_is_hybrid(facts.adapters)));
  out.set("system", system);

  Json adapters = Json::array();
  for (const CompatAdapter& adapter : facts.adapters) {
    Json item = Json::object();
    item.set("index", Json(adapter.index));
    item.set("vendor", Json(compat_vendor(adapter.vendor_id)));
    item.set("vendor_id", Json(compat_hex(adapter.vendor_id, 4)));
    item.set("device_id", Json(compat_hex(adapter.device_id, 4)));
    item.set("name", Json(compat_text(adapter.name)));
    item.set("driver", Json(compat_text(adapter.driver)));
    item.set("dedicated_mb", Json(static_cast<int64_t>(adapter.dedicated_bytes / (1024 * 1024))));
    item.set("software", Json(adapter.software));
    item.set("used_by_libobs", Json(adapter.used_by_libobs));
    Json outputs = Json::array();
    for (const std::string& output : adapter.outputs) {
      // A monitor is named by its device (\\.\DISPLAY1), nothing else.
      if (output.rfind("\\\\.\\DISPLAY", 0) == 0 && output.size() < 24) outputs.push(Json(output));
    }
    item.set("outputs", outputs);
    adapters.push(item);
  }
  out.set("adapters", adapters);
  out.set("libobs_adapter_reason", Json(compat_text(facts.libobs_adapter_reason)));

  Json encoders = Json::array();
  for (const CompatEncoder& encoder : facts.encoders) {
    Json item = Json::object();
    item.set("id", Json(compat_text(encoder.id)));
    item.set("codec", Json(compat_text(encoder.codec)));
    item.set("hardware", Json(encoder.hardware));
    item.set("rate_control", compat_strings(encoder.rate_control));
    item.set("preset", compat_strings(encoder.preset));
    item.set("has_pre_analysis", Json(encoder.has_pre_analysis));
    item.set("has_bf", Json(encoder.has_bf));
    item.set("has_multipass", Json(encoder.has_multipass));
    item.set("has_lookahead", Json(encoder.has_lookahead));
    item.set("has_target_usage", Json(encoder.has_target_usage));
    encoders.push(item);
  }
  out.set("encoders", encoders);

  Json running = Json::object();
  running.set("id", Json(compat_text(facts.running_encoder)));
  Json settings = Json::object();
  for (const auto& entry : facts.running_settings) {
    if (compat_looks_private(entry.first)) continue;
    if (entry.second.is_string()) settings.set(entry.first, Json(compat_text(entry.second.as_string())));
    else if (entry.second.is_bool() || entry.second.is_number()) settings.set(entry.first, entry.second);
    // anything else (a nested object or list) is not an encoder setting and is left out
  }
  running.set("settings", settings);
  Json reported = Json::object();
  for (const auto& entry : facts.reported_settings) reported.set(compat_text(entry.first), Json(compat_text(entry.second)));
  running.set("reported_by_encoder", reported);
  Json control = Json::object();
  control.set("level", Json(facts.controller_level));
  control.set("label", Json(compat_text(facts.controller_label)));
  running.set("controller", control);
  out.set("running_encoder", running);

  Json speed = Json::object();
  if (facts.speed_measured && facts.speed_rendered > 0) {
    const double ratio = static_cast<double>(facts.speed_encoded) / static_cast<double>(facts.speed_rendered);
    speed.set("measured", Json(true));
    speed.set("seconds", Json(10));
    speed.set("frames_rendered", Json(static_cast<int64_t>(facts.speed_rendered)));
    speed.set("frames_encoded", Json(static_cast<int64_t>(facts.speed_encoded)));
    speed.set("encoded_ratio", Json(ratio));
  } else {
    speed.set("measured", Json(false));
    speed.set("note", Json(compat_text(facts.speed_note.empty() ? std::string("not measured: start the buffer first") : facts.speed_note)));
  }
  out.set("speed", speed);
  return out;
}

// The short human summary: the lines a maintainer reads first.
inline std::string compat_report_text(const CompatFacts& facts) {
  std::string text = "BuffrX compatibility report\n";
  text += "BuffrX " + compat_text(facts.buffrx_version) + ", libobs " + compat_text(facts.obs_version) + ", Windows build " +
          std::to_string(facts.windows_build) + ", HAGS " + (facts.hags.empty() ? "unknown" : compat_text(facts.hags)) + "\n";
  for (const CompatAdapter& adapter : facts.adapters) {
    text += std::string(adapter.used_by_libobs ? "* " : "  ") + compat_text(adapter.name) + " (" + compat_vendor(adapter.vendor_id) + " " +
            compat_hex(adapter.vendor_id, 4) + ":" + compat_hex(adapter.device_id, 4) + "), driver " + compat_text(adapter.driver) + ", " +
            std::to_string(adapter.dedicated_bytes / (1024 * 1024)) + " MB\n";
  }
  if (!facts.libobs_adapter_reason.empty()) text += "libobs uses the starred adapter: " + compat_text(facts.libobs_adapter_reason) + "\n";
  if (compat_is_hybrid(facts.adapters)) text += "Two graphics adapters: likely a laptop with an integrated and a discrete GPU.\n";
  text += "Encoders:";
  for (const CompatEncoder& encoder : facts.encoders) text += " " + compat_text(encoder.id);
  text += "\n";
  text += "Running encoder: " + (facts.running_encoder.empty() ? std::string("none") : compat_text(facts.running_encoder)) + "\n";
  const auto reported_preset = facts.reported_settings.find("preset");
  if (reported_preset != facts.reported_settings.end()) text += "Preset the encoder reports: " + compat_text(reported_preset->second) + "\n";
  if (facts.speed_measured && facts.speed_rendered > 0) {
    text += "Encoded " + std::to_string(facts.speed_encoded) + " of " + std::to_string(facts.speed_rendered) +
            " rendered frames since the encoder started\n";
  } else {
    text += "Speed: " + compat_text(facts.speed_note.empty() ? std::string("not measured: start the buffer first") : facts.speed_note) + "\n";
  }
  text += "Nothing was sent anywhere: this file goes only where you paste it.\n";
  return text;
}

}  // namespace bx
