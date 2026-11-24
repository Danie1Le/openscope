#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/trigger.hpp"

namespace openscope::test {

/// CH1 enabled, triggering on itself.
inline constexpr std::array<ChannelSettings, kChannelCount> kChannelOne{
    {{.enabled = true}, {}, {}, {}}};

/// Auto with time zero on the first sample: a signal that never triggers gives an untriggered
/// capture every record length from where capturing starts.
inline constexpr TriggerSettings kUntriggeredEveryRecord{.position = 0.0};

/// A block of CH1 samples from `first` on, each its own sample index in volts (rising from 0 V by
/// 1 V a sample), so a capture's samples show where it came from, and the signal never moves the
/// hysteresis band below the 0 V trigger level and never triggers.
inline SampleBlock rising_block(SampleIndex first, std::size_t count) {
  SampleBlock block{.first_index = first, .count = count, .channels = {}};
  for (std::size_t n = 0; n < count; ++n) {
    block.channels[0].push_back(static_cast<float>(first + n));
  }
  return block;
}

/// The captures an assembler hands on, oldest first.
struct HandedOn {
  std::vector<std::shared_ptr<const Capture>> captures;

  /// The sink to pass to the assembler.
  CaptureSink sink() {
    return [this](const std::shared_ptr<const Capture>& capture) { captures.push_back(capture); };
  }

  /// The first sample index of each capture, oldest first.
  [[nodiscard]] std::vector<SampleIndex> first_indexes() const {
    std::vector<SampleIndex> firsts;
    std::ranges::transform(captures, std::back_inserter(firsts),
                           [](const auto& capture) { return capture->first_index; });
    return firsts;
  }
};

}  // namespace openscope::test
