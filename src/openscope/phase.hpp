#pragma once

#include <cmath>

namespace openscope {

/// The fraction of a cycle the phase advances per sample at `frequency` (hertz, negative for a
/// phase that runs backwards) and `sample_rate`, with the sign of `frequency` and below one cycle
/// in size. Whole cycles never change the phase, and dropping them with the exact fmod keeps them
/// from eating the precision of the phase far along the sample clock when the frequency is above
/// the sample rate.
inline double cycles_per_sample(double frequency, double sample_rate) {
  return std::fmod(frequency, sample_rate) / sample_rate;
}

/// `cycles` wrapped into a phase in [0, 1).
inline double wrap_phase(double cycles) {
  const double phase = cycles - std::floor(cycles);
  return phase < 1.0 ? phase : 0.0;  // A tiny negative `cycles` rounds up to exactly 1.
}

}  // namespace openscope
