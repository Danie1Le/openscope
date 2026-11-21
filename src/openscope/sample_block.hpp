#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {

/// A run of consecutive sample indexes carrying one sample per enabled channel at each index.
struct SampleBlock {
  /// The sample index of the first sample.
  SampleIndex first_index = 0;
  /// How many sample indexes the block spans.
  std::size_t count = 0;
  /// Each channel's samples in volts, indexed by channel number - 1: `count` samples for an
  /// enabled channel, none for a disabled one.
  std::array<std::vector<float>, kChannelCount> channels;
};

/// Samples every enabled channel over `count` indexes from `first` on the shared sample clock:
/// Generator n feeds Channel n, and each sample is clipped to its channel's input range.
SampleBlock sample_channels(std::span<const Generator, kChannelCount> generators,
                            std::span<const ChannelSettings, kChannelCount> channels,
                            SampleIndex first, std::size_t count);

}  // namespace openscope
