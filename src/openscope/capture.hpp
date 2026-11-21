#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/trigger.hpp"

namespace openscope {

/// The settings that alone define a capture (ADR 0004). The defaults are the `*RST` values.
struct AcquisitionSettings {
  /// The longest record length `ACQuire:POINts` accepts (2^20 samples).
  static constexpr std::size_t kMaxRecordLength = std::size_t{1} << 20U;

  /// Samples per second on the sample clock (`ACQuire:SRATe`).
  double sample_rate = 1e6;
  /// Sample indexes per capture (`ACQuire:POINts`).
  std::size_t record_length = 10'000;
};

/// One enabled channel's part of a capture, with the vertical settings its samples were taken at.
struct CapturedChannel {
  /// The channel's settings when the capture started, so its input-range limits are the ones
  /// that clipped the samples. `enabled` is always true: only enabled channels are captured.
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
  /// capture the pre-trigger position. Either way it is the trigger's pre-trigger sample count.
  std::size_t trigger_position = 0;
  /// True when no trigger event placed time zero (an untriggered capture).
  bool untriggered = true;
  /// Each channel's samples, indexed by channel number - 1; empty for a channel that was not
  /// enabled.
  std::array<std::optional<CapturedChannel>, kChannelCount> channels;
};

/// Assembles sample blocks into captures placed by the edge trigger, and keeps the latest
/// capture. A capture starts collecting samples as soon as the one before completes. A trigger
/// event places its time zero once it holds the pre-trigger samples and the holdoff since the
/// last capture's trigger event has passed. In Auto trigger mode, one record length of samples
/// after the pre-trigger samples have filled without such an event makes the latest record length
/// of samples an untriggered capture.
class CaptureAssembler {
 public:
  /// Appends `block`, taken under `acquisition`, `channels` and `trigger`. A capture takes its
  /// settings from the block that starts it, and the trigger keeps its state from block to block,
  /// so the caller discards the capture in progress whenever it changes a setting. Each capture
  /// that completes becomes the latest capture; a block may complete several. Throws
  /// std::logic_error when `acquisition` has a record length of zero, when `block` lacks the
  /// trigger channel's samples, or when it lacks samples for a channel of the capture in
  /// progress, which means a channel was enabled or disabled without a discard.
  void push(const SampleBlock& block, const AcquisitionSettings& acquisition,
            std::span<const ChannelSettings, kChannelCount> channels,
            const TriggerSettings& trigger);

  /// Drops the capture in progress and restarts the trigger, unarmed and with no holdoff
  /// running; the next block starts a new capture. The latest capture stays.
  void discard() {
    building_.reset();
    trigger_.reset();
    holdoff_end_ = 0;
  }

  /// The most recently completed capture, or null before the first one completes. It never
  /// changes once published, so readers may keep it as long as they like.
  [[nodiscard]] std::shared_ptr<const Capture> latest() const { return latest_; }

 private:
  /// Offsets of trigger events within the block being pushed.
  using EventIterator = std::vector<std::size_t>::const_iterator;

  /// Starts collecting a capture at sample index `first` and returns it.
  Capture& start(SampleIndex first, const AcquisitionSettings& acquisition,
                 std::span<const ChannelSettings, kChannelCount> channels, std::size_t pre_trigger);
  /// Completes `capture`, already placed, with `block`'s samples from offset `used` on; returns the
  /// offset it stopped at.
  std::size_t fill(Capture& capture, const SampleBlock& block, std::size_t used);
  /// Collects `block`'s samples from offset `used` into the unplaced `capture` until the first of
  /// the events from `next_event` on that may place it, or until the Auto timeout or the Normal
  /// trim point; returns the offset it stopped at. `holdoff` is in samples.
  std::size_t collect(Capture& capture, const SampleBlock& block, std::size_t used,
                      EventIterator& next_event, EventIterator end, TriggerMode mode,
                      std::uint64_t holdoff);
  /// Appends `block`'s samples at offsets [from, to) to every channel of `capture`.
  void append(Capture& capture, const SampleBlock& block, std::size_t from, std::size_t to);
  /// Drops the `count` oldest samples of `capture`.
  void drop_oldest(Capture& capture, std::size_t count);
  /// Makes `capture`, the capture in progress, the latest capture.
  void publish(Capture& capture);

  /// The trigger since the last discard (created by the first block after it), and the first
  /// sample index its holdoff lets place a capture.
  std::optional<EdgeTrigger> trigger_;
  SampleIndex holdoff_end_ = 0;
  /// The capture being collected, and how many samples it holds. Its `trigger_position` and
  /// `untriggered` are set when it is published, from the placement state below.
  std::optional<Capture> building_;
  std::size_t filled_ = 0;
  /// The pre-trigger sample count of the capture being collected.
  std::size_t pre_trigger_ = 0;
  /// True once a trigger event has placed the capture being collected.
  bool placed_ = false;
  std::shared_ptr<const Capture> latest_;
};

}  // namespace openscope
