#include <cstdio>

namespace bx {
int run_video_probe(const std::string&, const std::string&) {
  std::printf("PROBE_FAIL video probe is not implemented on Linux\n");
  return 1;
}
}  // namespace bx
