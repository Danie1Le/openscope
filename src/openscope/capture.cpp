#include "openscope/capture.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/trigger.hpp"

namespace openscope {

void CaptureAssembler::push(const SampleBlock& block, const AcquisitionSettings& acquisition,
                            std::span<const ChannelSettings, kChannelCount> channels,
                            const TriggerSettings& trigger) {
  const std::size_t record_length = acquisition.record_length;
  if (record_length == 0) {
    throw std::logic_error("record length of zero");  // No sample would ever fill a capture.
  }
  const std::size_t trigger_channel = trigger.source - 1;
  const std::vector<float>& trigger_samples = block.channels.at(trigger_channel);
  if (trigger_samples.size() != block.count) {
    throw std::logic_error("sample block lacks the trigger channel");
  }
  if (!trigger_) {
    trigger_.emplace(trigger.level, trigger.slope,
                     TriggerSettings::kHysteresisDivisions * channels[trigger_channel].scale);
  }
  // The trigger sees every sample, whatever the captures do with its events.
  const std::vector<std::size_t> events = trigger_->find(trigger_samples);
  auto next_event = events.begin();
  const std::size_t pre_trigger = trigger.pre_trigger_samples(record_length);

  for (std::size_t used = 0; used < block.count;) {
    if (!building_) {
      start(block.first_index + used, acquisition, channels, pre_trigger);
    }
    if (!building_->untriggered) {
      const std::size_t to = std::min(block.count, used + (record_length - filled_));
      append(block, used, to);
      used = to;
      if (filled_ == record_length) {
        publish();
      }
    } else {
      // Collect samples until a trigger event places the capture. Events count once the
      // pre-trigger samples have filled, so the Auto timeout is one record length after that.
      const std::size_t limit = record_length + pre_trigger;
      std::size_t to = std::min(block.count, used + (limit - filled_));
      bool placed = false;
      for (; next_event != events.end() && *next_event < to; ++next_event) {
        const std::size_t event = *next_event;
        // An event is ignored until the pre-trigger samples have filled and the holdoff passed.
        if (event >= used && filled_ + (event - used) >= pre_trigger &&
            block.first_index + event >= holdoff_end_) {
          to = event;
          placed = true;
          break;
        }
      }
      append(block, used, to);
      used = to;
      if (placed) {
        drop_oldest(filled_ - pre_trigger);
        building_->untriggered = false;
        holdoff_end_ = block.first_index + used + trigger.holdoff_samples(acquisition.sample_rate);
      } else if (filled_ == limit && trigger.mode == TriggerMode::kAuto) {
        drop_oldest(pre_trigger);  // The latest record becomes an untriggered capture.
        publish();
      } else if (filled_ == limit) {
        drop_oldest(record_length);  // Normal keeps only the samples that may precede an event.
      }
    }
  }
}

void CaptureAssembler::publish() {
  latest_ = std::make_shared<const Capture>(std::move(*building_));
  building_.reset();
}

void CaptureAssembler::start(SampleIndex first, const AcquisitionSettings& acquisition,
                             std::span<const ChannelSettings, kChannelCount> channels,
                             std::size_t pre_trigger) {
  building_.emplace(Capture{.first_index = first,
                            .sample_rate = acquisition.sample_rate,
                            .record_length = acquisition.record_length,
                            .trigger_position = pre_trigger,
                            .untriggered = true,
                            .channels = {}});
  filled_ = 0;
  for (std::size_t n = 0; n < kChannelCount; ++n) {
    if (channels[n].enabled) {
      building_->channels.at(n)
          .emplace(CapturedChannel{.settings = channels[n], .samples = {}})
          .samples.reserve(acquisition.record_length + pre_trigger);  // Its most while waiting.
    }
  }
}

void CaptureAssembler::append(const SampleBlock& block, std::size_t from, std::size_t to) {
  for (std::size_t n = 0; n < kChannelCount; ++n) {
    if (auto& channel = building_->channels.at(n)) {
      if (block.channels.at(n).size() != block.count) {
        throw std::logic_error("sample block lacks a channel of the capture in progress");
      }
      const auto begin = block.channels.at(n).begin();
      channel->samples.insert(channel->samples.end(), begin + static_cast<std::ptrdiff_t>(from),
                              begin + static_cast<std::ptrdiff_t>(to));
    }
  }
  filled_ += to - from;
}

void CaptureAssembler::drop_oldest(std::size_t count) {
  for (auto& channel : building_->channels) {
    if (channel) {
      channel->samples.erase(channel->samples.begin(),
                             channel->samples.begin() + static_cast<std::ptrdiff_t>(count));
    }
  }
  building_->first_index += count;
  filled_ -= count;
}

}  // namespace openscope
