#include "../engine/rec_mark.h"
namespace bx {
double dot_falloff(double distance, double core, double radius) {
  if (distance <= core) return 1.0;
  const double edge = (distance - core) / (radius - core);
  if (edge >= 1.0) return 0.0;
  const double soft = 1.0 - edge;
  return soft * soft;
}
bool recording_dot_visible(bool manual_active) { return manual_active; }
RecMark::~RecMark() = default;
bool RecMark::create() { return true; }
void RecMark::destroy() {}
void RecMark::set_recording(bool) {}
}  // namespace bx
