#include "openscope/trigger.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace openscope {

EdgeTrigger::EdgeTrigger(double level, TriggerSlope slope, double hysteresis)
    : direction_(slope == TriggerSlope::kPositive ? 1.0 : -1.0),
      level_(direction_ * level),
      hysteresis_(hysteresis) {}

std::vector<std::size_t> EdgeTrigger::find(std::span<const float> samples) {
  std::vector<std::size_t> events;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double volts = direction_ * samples[i];
    if (volts <= level_ - hysteresis_) {
      armed_ = true;
    } else if (armed_ && volts >= level_) {
      events.push_back(i);
      armed_ = false;
    }
  }
  return events;
}

}  // namespace openscope
