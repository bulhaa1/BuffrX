// Which encoder the engine tries first, and in what order it falls back.
//
// The rule lives here, apart from ObsHost, because it is a decision about settings and a list of names:
// no graphics device, no libobs, nothing to start. `available` is what the machine reports; the result
// is every name that fits the codec and the chosen vendor, best first.
#pragma once

#include <string>
#include <vector>

namespace bx {

std::vector<std::string> encoder_candidates(const std::vector<std::string>& available, const std::string& codec,
                                            const std::string& encoder);

}  // namespace bx
