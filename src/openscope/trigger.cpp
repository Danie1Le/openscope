#include "openscope/trigger.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace openscope {

std::size_t TriggerSettings::pre_trigger_samples(std::size_t record_length) const {
  return static_cast<std::size_t>(
      std::llround(position / 100.0 * static_cast<double>(record_length - 1)));
}

std::uint64_t TriggerSettings::holdoff_samples(double sample_rate) const {
  return static_cast<std::uint64_t>(std::llround(holdoff * sample_rate));
}

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
