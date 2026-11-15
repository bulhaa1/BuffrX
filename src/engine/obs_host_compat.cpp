// The libobs and Windows side of the compatibility report (plan 009): read the machine into a CompatFacts and write the two
// files. What may be in them is decided by compat_report.h, which is tested; this only gathers.
#include <obs.h>
#include <windows.h>

#include <algorithm>

#include "../shared/files.h"
#include "../shared/log.h"
#include "compat_report.h"
#include "encoder_choice.h"
#include "gpu.h"
#include "libobs_guard.h"
#include "obs_host.h"

namespace bx {
namespace {

std::vector<std::string> list_values(obs_properties_t* properties, const char* key) {
  std::vector<std::string> values;
  obs_property_t* property = obs_properties_get(properties, key);
  if (!property || obs_property_get_type(property) != OBS_PROPERTY_LIST) return values;
  const size_t count = obs_property_list_item_count(property);
  for (size_t index = 0; index < count; ++index) {
    const char* value = obs_property_list_item_string(property, index);
    if (value) values.emplace_back(value);
  }
  return values;
}

int windows_build() {
  char text[32] = {0};
  DWORD size = sizeof(text);
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuildNumber", RRF_RT_REG_SZ, nullptr,
                   text, &size) != ERROR_SUCCESS) {
    return 0;
  }
  return std::atoi(text);
}

// Hardware-accelerated GPU scheduling: HwSchMode 2 is on, 1 is off. Unreadable is "unknown", never a guess.
std::string hags_state() {
  DWORD mode = 0;
  DWORD size = sizeof(mode);
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "HwSchMode", RRF_RT_REG_DWORD, nullptr, &mode,
                   &size) != ERROR_SUCCESS) {
    return "unknown";
  }
  return mode == 2 ? "on" : mode == 1 ? "off" : "unknown";
}

}  // namespace

void ObsHost::compat_fill(CompatFacts& facts) const {
  facts.obs_version = obs_version();
  facts.windows_build = windows_build();
  facts.hags = hags_state();
  facts.libobs_adapter_reason = video_adapter_reason_;
  for (const GpuAdapterInfo& adapter : gpu_adapters()) {
    CompatAdapter item;
    item.index = adapter.index;
    item.vendor_id = adapter.vendor_id;
    item.device_id = adapter.device_id;
    item.name = adapter.name;
    item.driver = adapter.driver;
    item.dedicated_bytes = adapter.dedicated_bytes;
    item.software = adapter.software;
    item.used_by_libobs = adapter.index == video_adapter_index_;
    item.outputs = adapter.outputs;
    facts.adapters.push_back(item);
  }
  // Every registered video encoder. An encoder is never created to find out what it takes: encoders only initialise on an output's
  // start, and the property list is what libobs itself builds the settings page from.
  for (size_t index = 0;; ++index) {
    const char* id = nullptr;
    if (!obs_enum_encoder_types(index, &id) || !id) break;
    if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) continue;
    CompatEncoder encoder;
    encoder.id = id;
    encoder.codec = encoder_codec(id);
    const EncoderFamily family = encoder_family(id);
    encoder.hardware = family == EncoderFamily::Nvenc || family == EncoderFamily::Amf || family == EncoderFamily::Qsv;
    if (obs_properties_t* properties = obs_get_encoder_properties(id)) {
      encoder.rate_control = list_values(properties, "rate_control");
      encoder.preset = list_values(properties, "preset");
      encoder.has_pre_analysis = obs_properties_get(properties, "pre_analysis") != nullptr;
      encoder.has_bf = obs_properties_get(properties, "bf") != nullptr || obs_properties_get(properties, "bframes") != nullptr;
      encoder.has_multipass = obs_properties_get(properties, "multipass") != nullptr;
      encoder.has_lookahead = obs_properties_get(properties, "lookahead") != nullptr;
      encoder.has_target_usage = obs_properties_get(properties, "target_usage") != nullptr;
      obs_properties_destroy(properties);
    }
    facts.encoders.push_back(std::move(encoder));
  }
  if (video_encoder_) {
    facts.running_encoder = encoder_id_;
    if (obs_data_t* settings = obs_encoder_get_settings(video_encoder_)) {
      for (obs_data_item_t* item = obs_data_first(settings); item; obs_data_item_next(&item)) {
        const char* name = obs_data_item_get_name(item);
        if (!name) continue;
        switch (obs_data_item_gettype(item)) {
          case OBS_DATA_STRING: facts.running_settings.emplace_back(name, Json(obs_data_item_get_string(item))); break;
          case OBS_DATA_NUMBER:
            if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT) facts.running_settings.emplace_back(name, Json(static_cast<int64_t>(obs_data_item_get_int(item))));
            else facts.running_settings.emplace_back(name, Json(obs_data_item_get_double(item)));
            break;
          case OBS_DATA_BOOLEAN: facts.running_settings.emplace_back(name, Json(obs_data_item_get_bool(item))); break;
          default: break;
        }
      }
      obs_data_release(settings);
    }
    facts.reported_settings = encoder_settings_block_parse(libobs_encoder_settings_block("BuffrX video"));
  }
  const ControllerStatus control = controller_status();
  facts.controller_level = control.level;
  facts.controller_label = control.label;
}

Json ObsHost::hardware_json() const {
  Json out = Json::object();
  std::string vendor = "other";
  std::string gpu;
  for (const GpuAdapterInfo& adapter : gpu_adapters()) {
    if (adapter.index != video_adapter_index_) continue;
    gpu = adapter.name;
    vendor = gpu_vendor_from_id(adapter.vendor_id);
  }
  const EncoderFamily wanted = vendor == "nvidia" ? EncoderFamily::Nvenc : vendor == "amd" ? EncoderFamily::Amf
                               : vendor == "intel" ? EncoderFamily::Qsv : EncoderFamily::Other;
  bool seen_card[3] = {false, false, false};  // h264, hevc, av1 on the libobs adapter's vendor
  bool seen_any[3] = {true, false, false};    // on any hardware encoder; H.264 is always there (x264)
  // The vendors whose encoders libobs registered: a plugin registers its encoders only when its driver and hardware are present
  // (obs-nvenc probes the driver, AMF needs AMD's runtime, QSV an Intel adapter), so the Encoder list can grey the rest.
  bool vendor_present[3] = {false, false, false};  // nvenc, amf, qsv
  for (size_t index = 0;; ++index) {
    const char* id = nullptr;
    if (!obs_enum_encoder_types(index, &id) || !id) break;
    if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) continue;
    const EncoderFamily family = encoder_family(id);
    if (family != EncoderFamily::Nvenc && family != EncoderFamily::Amf && family != EncoderFamily::Qsv) continue;
    const std::string codec = encoder_codec(id);
    const int slot = codec == "av1" ? 2 : codec == "hevc" ? 1 : 0;
    seen_any[slot] = true;
    if (family == wanted) seen_card[slot] = true;
    vendor_present[family == EncoderFamily::Nvenc ? 0 : family == EncoderFamily::Amf ? 1 : 2] = true;
  }
  Json vendors = Json::array();
  const char* const vendor_names[3] = {"nvenc", "amf", "qsv"};
  for (int i = 0; i < 3; ++i) {
    if (vendor_present[i]) vendors.push(Json(vendor_names[i]));
  }
  out.set("encoder_vendors", vendors);
  const char* const names[3] = {"h264", "hevc", "av1"};
  Json card = Json::array();
  Json any = Json::array();
  for (int slot = 0; slot < 3; ++slot) {
    if (seen_card[slot]) card.push(Json(names[slot]));
    if (seen_any[slot]) any.push(Json(names[slot]));
  }
  out.set("gpu", Json(gpu));
  out.set("vendor", Json(vendor));
  out.set("codecs", card);
  out.set("available_codecs", any);
  // What the driver did to the preset BuffrX asked for (AMF lowers it at init and says so only in its settings block).
  if (video_encoder_) {
    out.set("running_id", Json(encoder_id_));
    const auto reported = encoder_settings_block_parse(libobs_encoder_settings_block("BuffrX video"));
    const auto preset = reported.find("preset");
    if (preset != reported.end()) out.set("reported_preset", Json(preset->second));
    if (obs_data_t* settings = obs_encoder_get_settings(video_encoder_)) {
      const char* asked = obs_data_get_string(settings, "preset");
      if (asked && *asked) out.set("asked_preset", Json(asked));
      obs_data_release(settings);
    }
  }
  return out;
}

ObsHost::EncodeCounters ObsHost::encode_counters() const {
  EncodeCounters counters;
  counters.active = buffer_active_ && replay_ && obs_output_active(replay_) && video_encoder_;
  if (!counters.active) return counters;
  // Use one completed window. The canvas counter outlives encoder restarts and
  // swaps, so dividing its lifetime total by a fresh encoder's total reports a
  // healthy encoder as slow until it has caught up with the canvas's entire history.
  counters.rendered = encoder_watch_.measured_rendered;
  counters.encoded = encoder_watch_.measured_encoded;
  return counters;
}

bool write_compat_report(const std::string& data_root, const CompatFacts& facts, std::string& file, std::string& summary,
                         std::string& error) {
  // One file, the summary inside it first: the name is what the person sees when it is pasted into a chat or an issue.
  file = data_root + "\\BuffrX compatibility report.json";
  summary = compat_report_text(facts);
  Json report = Json::object();
  report.set("about", Json(summary));  // keys are written sorted: "about" comes before "report"
  report.set("report", compat_report_json(facts));
  if (!write_file_atomic(file, report.dump(2))) {
    error = "the report could not be written";
    return false;
  }
  // The two files an earlier build wrote: one report is the point now.
  DeleteFileA((data_root + "\\compat-report.json").c_str());
  DeleteFileA((data_root + "\\compat-report.txt").c_str());
  return true;
}

}  // namespace bx
