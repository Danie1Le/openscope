#pragma once

#include <array>
#include <cstdint>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/trigger.hpp"

namespace openscope {

/// A sample block with the instrument state it was acquired under. In the live pipeline a block
/// is processed after later commands may have changed the settings, so it carries its own
/// snapshot of everything processing needs and the processing thread never reads the instrument's
/// settings (#8).
struct AcquiredBlock {
  SampleBlock samples;
  AcquisitionSettings acquisition;
  std::array<ChannelSettings, kChannelCount> channels;
  TriggerSettings trigger;
  /// kFirst when the block was acquired while a Single was armed.
  CaptureLimit limit = CaptureLimit::kEvery;
  /// Which run of unchanged settings the block belongs to: every command that discards the
  /// capture in progress starts a new one, so a capture is only ever assembled from one.
  std::uint64_t generation = 0;
  /// Counts the blocks acquired, from 0. A sequence number skipped on the way to processing
  /// means the blocks between were dropped.
  std::uint64_t sequence = 0;
};

}  // namespace openscope
