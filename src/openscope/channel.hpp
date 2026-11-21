#pragma once

#include <algorithm>
#include <cstddef>

namespace openscope {

/// The number of channels (CH1 to CH4), and of the Generators that feed them.
inline constexpr std::size_t kChannelCount = 4;

/// One channel's vertical settings. The defaults are the `*RST` values of CH2 to CH4; CH1 starts
/// enabled.
struct ChannelSettings {
  /// The number of vertical divisions above and below the offset that the input range spans.
  static constexpr double kHalfRangeDivisions = 4.0;

  /// An enabled channel is kept in captures (`CHANnel<n>:DISPlay`).
  bool enabled = false;
  /// Volts per vertical division.
  double scale = 1.0;
  /// The voltage at the centre of the input range.
  double offset = 0.0;

  /// The lowest voltage in the input range.
  [[nodiscard]] float lower_limit() const {
    return static_cast<float>(offset - (kHalfRangeDivisions * scale));
  }
  /// The highest voltage in the input range.
  [[nodiscard]] float upper_limit() const {
    return static_cast<float>(offset + (kHalfRangeDivisions * scale));
  }
  /// `volts` held to the input range: a sample outside it is clipped to the nearer limit.
  [[nodiscard]] float clip(float volts) const {
    return std::clamp(volts, lower_limit(), upper_limit());
  }
};

}  // namespace openscope
