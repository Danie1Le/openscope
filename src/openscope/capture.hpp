#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"

namespace openscope {

/// The settings that alone define a capture (ADR 0004). The defaults are the `*RST` values.
struct AcquisitionSettings {
  /// Samples per second on the sample clock (`ACQuire:SRATe`).
  double sample_rate = 1e6;
  /// Sample indexes per capture (`ACQuire:POINts`).
  std::size_t record_length = 10'000;
};

/// One enabled channel's part of a capture, with the vertical settings its samples were taken at.
struct CapturedChannel {
  /// The channel's settings when the capture started, so its input-range limits are the ones
  /// that clipped the samples.
  ChannelSettings settings;
  /// One sample per sample index of the capture, in volts, already clipped.
  std::vector<float> samples;
};

/// The samples of every enabled channel over the same record-length span of sample indexes.
struct Capture {
  /// The sample index of the first sample.
  SampleIndex first_index = 0;
  /// Samples per second the capture was taken at.
  double sample_rate = 0.0;
  /// Sample indexes in the capture: the length of every channel's samples.
  std::size_t record_length = 0;
  /// The sample of the capture where time zero sits: the trigger event, or for an untriggered
  /// capture the pre-trigger position. Until the trigger and its pre-trigger setting exist (#6)
  /// every capture is untriggered with time zero at its first sample.
  std::size_t trigger_position = 0;
  /// True when no trigger event placed time zero (an untriggered capture).
  bool untriggered = true;
  /// Each channel's samples, indexed by channel number - 1; empty for a channel that was not
  /// enabled.
  std::array<std::optional<CapturedChannel>, kChannelCount> channels;
};

/// Assembles sample blocks into untriggered captures that follow each other back to back, and
/// keeps the latest capture.
class CaptureAssembler {
 public:
  /// Appends `block`, taken under `acquisition` and `channels`. A capture takes its settings from
  /// the block that starts it, so the caller discards the capture in progress whenever it changes
  /// a setting. Each capture that fills becomes the latest capture; a block may fill several.
  /// Throws std::logic_error when `acquisition` has a record length of zero, or when `block` lacks
  /// samples for a channel of the capture in progress, which means a channel was enabled or
  /// disabled without a discard.
  void push(const SampleBlock& block, const AcquisitionSettings& acquisition,
            std::span<const ChannelSettings, kChannelCount> channels);

  /// Drops the capture in progress; the next block starts a new one. The latest capture stays.
  void discard() { building_.reset(); }

  /// The most recently completed capture, or null before the first one completes. It never
  /// changes once published, so readers may keep it as long as they like.
  [[nodiscard]] std::shared_ptr<const Capture> latest() const { return latest_; }

 private:
  /// The capture being filled, and how many of its sample indexes are filled so far.
  std::optional<Capture> building_;
  std::size_t filled_ = 0;
  std::shared_ptr<const Capture> latest_;
};

}  // namespace openscope
