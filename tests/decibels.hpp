#pragma once

#include <cmath>
#include <numbers>

namespace openscope::test {

/// `volts` RMS in dBV, the unit of spectrum levels.
inline double dbv(double volts) { return 20.0 * std::log10(volts); }

/// The spectrum level of a sine of 1 V peak (the 2 Vpp `*RST` amplitude) centred on a bin:
/// 1 / sqrt 2 V RMS, -3.0103 dBV.
inline const double kOneVoltPeakSineDb = dbv(1.0 / std::numbers::sqrt2);

}  // namespace openscope::test
