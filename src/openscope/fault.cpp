#include "openscope/fault.hpp"

#include <algorithm>
#include <cstddef>

#include "openscope/phase.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {

FaultLimits fault_limits(FaultKind kind) {
  switch (kind) {
    case FaultKind::kGlitch:
      return {.default_magnitude = 2.0, .default_duration = 1, .max_magnitude = 100.0};
    case FaultKind::kDropout:
      return {.default_magnitude = 0.0, .default_duration = 100, .max_magnitude = 100.0};
    case FaultKind::kFrequencyStep:
      return {.default_magnitude = 1e3, .default_duration = 1000, .max_magnitude = 1e9};
  }
  return {};
}

Fault::Fault(FaultKind kind, double magnitude, std::size_t duration, SampleIndex start,
             double sample_rate)
    : kind_(kind), magnitude_(magnitude), start_(start), end_(start + duration) {
  if (kind == FaultKind::kFrequencyStep) {
    cycles_per_sample_ = cycles_per_sample(magnitude, sample_rate);
  }
}

double Fault::phase_shift(SampleIndex index) const {
  if (kind_ != FaultKind::kFrequencyStep || index <= start_) {
    return 0.0;
  }
  return cycles_per_sample_ * static_cast<double>(std::min(index, end_) - start_);
}

double Fault::apply(SampleIndex index, double volts) const {
  if (index < start_ || index >= end_) {
    return volts;
  }
  switch (kind_) {
    case FaultKind::kGlitch:
      return volts + magnitude_;
    case FaultKind::kDropout:
      return magnitude_;
    case FaultKind::kFrequencyStep:
      break;
  }
  return volts;
}

}  // namespace openscope

int tidy_violation_probe() {
    int uninitialised;
    uninitialised = 1;
    return uninitialised;
}
