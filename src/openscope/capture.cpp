#include "openscope/capture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/trigger.hpp"

namespace openscope {

std::optional<std::size_t> CaptureAssembler::push(
    const SampleBlock& block, const AcquisitionSettings& acquisition,
    std::span<const ChannelSettings, kChannelCount> channels, const TriggerSettings& trigger,
    CaptureLimit limit, const CaptureSink& on_completed) {
  if (acquisition.record_length == 0) {
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
  const std::uint64_t holdoff = trigger.holdoff_samples(acquisition.sample_rate);
  const std::size_t pre_trigger = trigger.pre_trigger_samples(acquisition.record_length);

  for (std::size_t used = 0; used < block.count;) {
    const SampleIndex index = block.first_index + used;
    Capture& capture = building_ ? *building_ : start(index, acquisition, channels, pre_trigger);
    used = placed_ ? fill(capture, block, used)
                   : collect(capture, block, used, next_event, events.end(), trigger.mode, holdoff);
    if (building_) {
      continue;  // Still collecting: only complete() ends the capture in progress here.
    }
    // Each step completes at most one capture, and it is handed on before the next one exists, so
    // a block's captures never pile up however many it completes.
    if (on_completed) {
      on_completed(completed_);
    }
    completed_.reset();
    if (limit == CaptureLimit::kFirst) {
      discard();  // The trigger has seen the samples after `used`, which the caller pushes again.
      return used;
    }
  }
  return std::nullopt;
}

std::size_t CaptureAssembler::fill(Capture& capture, const SampleBlock& block, std::size_t used) {
  const std::size_t to = std::min(block.count, used + (capture.record_length - filled_));
  append(capture, block, used, to);
  if (filled_ == capture.record_length) {
    complete(capture);
  }
  return to;
}

std::size_t CaptureAssembler::collect(Capture& capture, const SampleBlock& block, std::size_t used,
                                      EventIterator& next_event, EventIterator end,
                                      TriggerMode mode, std::uint64_t holdoff) {
  // Events count once the pre-trigger samples have filled, so the Auto timeout is one record
  // length after that.
  const std::size_t limit = capture.record_length + pre_trigger_;
  std::size_t to = std::min(block.count, used + (limit - filled_));
  // An event is ignored until the pre-trigger samples have filled and the holdoff has passed.
  const auto places = [&](std::size_t event) {
    return event >= used && filled_ + (event - used) >= pre_trigger_ &&
           block.first_index + event >= holdoff_end_;
  };
  while (next_event != end && *next_event < to && !places(*next_event)) {
    ++next_event;
  }
  const bool placed = next_event != end && *next_event < to;
  if (placed) {
    to = *next_event;
  }
  append(capture, block, used, to);
  if (placed) {
    drop_oldest(capture, filled_ - pre_trigger_);
    placed_ = true;
    holdoff_end_ = block.first_index + to + holdoff;
  } else if (filled_ == limit && mode == TriggerMode::kAuto) {
    drop_oldest(capture, pre_trigger_);  // The latest record becomes an untriggered capture.
    complete(capture);
  } else if (filled_ == limit) {
    drop_oldest(capture, capture.record_length);  // Normal keeps what may precede an event.
  }
  return to;
}

void CaptureAssembler::complete(Capture& capture) {
  capture.trigger_position = pre_trigger_;
  capture.untriggered = !placed_;
  completed_ = std::make_shared<const Capture>(std::move(capture));
  building_.reset();
}

Capture& CaptureAssembler::start(SampleIndex first, const AcquisitionSettings& acquisition,
                                 std::span<const ChannelSettings, kChannelCount> channels,
                                 std::size_t pre_trigger) {
  Capture& capture = building_.emplace(Capture{.first_index = first,
                                               .sample_rate = acquisition.sample_rate,
                                               .record_length = acquisition.record_length,
                                               .channels = {}});
  pre_trigger_ = pre_trigger;
  placed_ = false;
  filled_ = 0;
  for (std::size_t n = 0; n < kChannelCount; ++n) {
    if (channels[n].enabled) {
      capture.channels.at(n)
          .emplace(CapturedChannel{.settings = channels[n], .samples = {}})
          // Normal mode holds at most a record length plus the pre-trigger samples while it waits.
          .samples.reserve(acquisition.record_length + pre_trigger);
    }
  }
  return capture;
}

void CaptureAssembler::append(Capture& capture, const SampleBlock& block, std::size_t from,
                              std::size_t to) {
  for (std::size_t n = 0; n < kChannelCount; ++n) {
    if (auto& channel = capture.channels.at(n)) {
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

void CaptureAssembler::drop_oldest(Capture& capture, std::size_t count) {
  for (auto& channel : capture.channels) {
    if (channel) {
      channel->samples.erase(channel->samples.begin(),
                             channel->samples.begin() + static_cast<std::ptrdiff_t>(count));
    }
  }
  capture.first_index += count;
  filled_ -= count;
}

}  // namespace openscope
