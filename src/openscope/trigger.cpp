#include "openscope/trigger.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace openscope {

EdgeTrigger::EdgeTrigger(double level, TriggerSlope slope, double hysteresis)
    : direction_(slope == TriggerSlope::kPositive ? 1.0F : -1.0F),
      fire_at_(static_cast<float>(direction_ * level)),
      arm_at_(static_cast<float>(direction_ * level - hysteresis)) {}

std::vector<std::size_t> EdgeTrigger::find(std::span<const float> samples) {
  std::vector<std::size_t> events;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const float volts = direction_ * samples[i];
    if (volts <= arm_at_) {
      armed_ = true;
    } else if (armed_ && volts >= fire_at_) {
      events.push_back(i);
      armed_ = false;
    }
  }
  return events;
}

}  // namespace openscope
