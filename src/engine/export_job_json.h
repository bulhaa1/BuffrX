// The JSON a client hands `--media-export`, parsed in one place so the engine and its tests read the
// same fields. No file I/O: the caller reads the text.
#pragma once

#include <string>

#include "media_math.h"

namespace bx {

class Json;

// Fills `job` from a parsed job object. False with a sentence when the object cannot describe an
// export at all (no output, and no input or inputs). Every other field has a usable default.
bool export_job_from_json(const Json& spec, ExportJob& job, std::string& error);

}  // namespace bx
