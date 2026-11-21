#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {

/// A run of consecutive sample indexes carrying one sample per enabled channel at each index, plus
/// the trigger channel's samples when it is disabled.
struct SampleBlock {
  /// The sample index of the first sample.
  SampleIndex first_index = 0;
  /// How many sample indexes the block spans.
  std::size_t count = 0;
  /// Each channel's samples in volts, indexed by channel number - 1: `count` samples for an
  /// enabled channel and the trigger channel, none for another disabled one.
  std::array<std::vector<float>, kChannelCount> channels;
};

/// Samples every enabled channel, and the trigger channel `trigger_source` (1 to 4) whether
/// enabled or not, over `count` indexes from `first` on the shared sample clock: Generator n
/// feeds Channel n, and each sample is clipped to its channel's input range.
SampleBlock sample_channels(std::span<const Generator, kChannelCount> generators,
                            std::span<const ChannelSettings, kChannelCount> channels,
                            std::size_t trigger_source, SampleIndex first, std::size_t count);

}  // namespace openscope
