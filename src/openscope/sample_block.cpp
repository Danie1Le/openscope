#include "openscope/sample_block.hpp"

#include <cstddef>
#include <span>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/generator.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {

SampleBlock sample_channels(std::span<const Generator, kChannelCount> generators,
                            std::span<const ChannelSettings, kChannelCount> channels,
                            std::size_t trigger_source, SampleIndex first, std::size_t count) {
  SampleBlock block{.first_index = first, .count = count, .channels = {}};
  for (std::size_t n = 0; n < kChannelCount; ++n) {
    if (!channels[n].enabled && n != trigger_source - 1) {
      continue;
    }
    std::vector<float>& samples = block.channels.at(n);
    samples.resize(count);
    generators[n].generate(first, samples);
    for (float& sample : samples) {
      sample = channels[n].clip(sample);
    }
  }
  return block;
}

}  // namespace openscope
