#include "openscope/capture.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"

namespace openscope {

void CaptureAssembler::push(const SampleBlock& block, const AcquisitionSettings& acquisition,
                            std::span<const ChannelSettings, kChannelCount> channels) {
  for (std::size_t used = 0; used < block.count;) {
    if (!building_) {
      building_.emplace(Capture{.first_index = block.first + used,
                                .sample_rate = acquisition.sample_rate,
                                .record_length = acquisition.record_length,
                                .trigger_position = 0,
                                .untriggered = true,
                                .channels = {}});
      filled_ = 0;
      for (std::size_t n = 0; n < kChannelCount; ++n) {
        if (channels[n].enabled) {
          CapturedChannel& captured = building_->channels.at(n).emplace(CapturedChannel{
              .scale = channels[n].scale, .offset = channels[n].offset, .samples = {}});
          captured.samples.reserve(acquisition.record_length);
        }
      }
    }
    const std::size_t take = std::min(building_->record_length - filled_, block.count - used);
    for (std::size_t n = 0; n < kChannelCount; ++n) {
      if (auto& channel = building_->channels.at(n)) {
        if (block.channels.at(n).size() != block.count) {
          throw std::logic_error("sample block lacks a channel of the capture in progress");
        }
        const auto from = block.channels.at(n).begin() + static_cast<std::ptrdiff_t>(used);
        std::copy_n(from, take, std::back_inserter(channel->samples));
      }
    }
    used += take;
    filled_ += take;
    if (filled_ == building_->record_length) {
      latest_ = std::make_shared<const Capture>(std::move(*building_));
      building_.reset();
    }
  }
}

}  // namespace openscope
