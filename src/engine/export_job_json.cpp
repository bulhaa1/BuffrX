#include "export_job_json.h"

#include "../shared/json.h"

namespace bx {
namespace {

std::vector<std::pair<double, double>> read_segments(const Json& spec) {
  std::vector<std::pair<double, double>> segments;
  for (const Json& segment : spec["segments"].items()) {
    if (segment.items().size() != 2) continue;
    segments.emplace_back(segment.items()[0].as_double(-1.0), segment.items()[1].as_double(-1.0));
  }
  return segments;
}

std::vector<double> read_volumes(const Json& spec) {
  std::vector<double> volumes;
  for (const Json& volume : spec["volumes"].items()) volumes.push_back(volume.as_double(1.0));
  return volumes;
}

}  // namespace

bool export_job_from_json(const Json& spec, ExportJob& job, std::string& error) {
  if (!spec.is_object()) {
    error = "the export job is not an object";
    return false;
  }
  job = ExportJob{};
  job.output = spec["output"].as_string();
  job.input = spec["input"].as_string();
  if (job.output.empty()) {
    error = "the export job has no output";
    return false;
  }
  job.replace_original = spec["replace_original"].as_bool(false);
  job.start = spec["start"].as_double(0.0);
  job.end = spec["end"].as_double(0.0);
  job.crop_x = static_cast<int>(spec["crop_x"].as_int(0));
  job.crop_y = static_cast<int>(spec["crop_y"].as_int(0));
  job.crop_width = static_cast<int>(spec["crop_width"].as_int(0));
  job.crop_height = static_cast<int>(spec["crop_height"].as_int(0));
  job.out_width = static_cast<int>(spec["out_width"].as_int(0));
  job.out_height = static_cast<int>(spec["out_height"].as_int(0));
  job.square_pixels = spec["square_pixels"].as_bool(false);
  job.quality = spec["quality"].as_double(0.0);
  job.grade_brightness = spec["grade_brightness"].as_double(100.0);
  job.grade_contrast = spec["grade_contrast"].as_double(100.0);
  job.grade_saturation = spec["grade_saturation"].as_double(100.0);
  job.video_kbps = static_cast<int>(spec["video_kbps"].as_int(0));
  job.audio_kbps = static_cast<int>(spec["audio_kbps"].as_int(0));
  job.speed = spec["speed"].as_double(1.0);
  job.volumes = read_volumes(spec);
  job.segments = read_segments(spec);
  for (const Json& entry : spec["inputs"].items()) {
    if (!entry.is_object()) continue;
    ExportInput input;
    input.input = entry["input"].as_string();
    input.start = entry["start"].as_double(0.0);
    input.end = entry["end"].as_double(0.0);
    input.segments = read_segments(entry);
    input.crop_x = static_cast<int>(entry["crop_x"].as_int(0));
    input.crop_y = static_cast<int>(entry["crop_y"].as_int(0));
    input.crop_width = static_cast<int>(entry["crop_width"].as_int(0));
    input.crop_height = static_cast<int>(entry["crop_height"].as_int(0));
    input.grade_brightness = entry["grade_brightness"].as_double(100.0);
    input.grade_contrast = entry["grade_contrast"].as_double(100.0);
    input.grade_saturation = entry["grade_saturation"].as_double(100.0);
    input.speed = entry["speed"].as_double(1.0);
    input.volumes = read_volumes(entry);
    if (!input.input.empty()) job.inputs.push_back(std::move(input));
  }
  job.fps = spec["fps"].as_double(0.0);
  job.audio_rate = static_cast<int>(spec["audio_rate"].as_int(0));
  job.audio_channels = static_cast<int>(spec["audio_channels"].as_int(0));
  job.video_codec = spec["video_codec"].as_string();
  job.video_encoder = spec["video_encoder"].as_string();
  job.fit_pad = spec["fit_pad"].as_bool(false);
  if (job.input.empty() && job.inputs.empty()) {
    error = "the export job has no input";
    return false;
  }
  return true;
}

}  // namespace bx
