#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "openscope/capture.hpp"
#include "openscope/channel.hpp"
#include "openscope/sample_block.hpp"
#include "openscope/sample_clock.hpp"
#include "openscope/trigger.hpp"

namespace openscope {

/// How many of the captures a sample block could complete `CaptureAssembler::push` completes.
enum class CaptureLimit : std::uint8_t {
  /// Every one (the Running run state).
  kEvery,
  /// The first, after which push stops (the Armed-single run state).
  kFirst,
};

/// Called by `CaptureAssembler::push` with each capture as it completes: the one way captures
/// leave the assembler, so each can be measured and published. It never changes, so the sink may
/// keep it.
using CaptureSink = std::function<void(const std::shared_ptr<const Capture>&)>;

/// Assembles sample blocks into captures placed by the edge trigger. A capture starts collecting
/// samples as soon as the one before completes. A trigger event places its time zero once it holds
/// the pre-trigger samples and the holdoff since the last capture's trigger event has passed. In
/// Auto trigger mode, one record length of samples after the pre-trigger samples have filled
/// without such an event makes the latest record length of samples an untriggered capture.
class CaptureAssembler {
 public:
  /// Appends `block`, taken under `acquisition`, `channels` and `trigger`, and hands each capture
  /// to `on_completed`, if given, as it completes. A capture takes its settings from the block
  /// that starts it, and the trigger keeps its state from block to block, so the caller discards
  /// the capture in progress whenever it changes a setting or skips samples. A block may complete
  /// several captures, of which `limit` says how many to make: under kFirst, once a capture
  /// completes, it discards and returns the offset in `block` just past that capture, so the
  /// caller can push the samples after it as a new block; otherwise it uses the whole block and
  /// returns nullopt. Throws std::logic_error when `acquisition` has a record length of zero, when
  /// `block` lacks the trigger channel's samples, or when it lacks samples for a channel of the
  /// capture in progress, which means a channel was enabled or disabled without a discard.
  std::optional<std::size_t> push(const SampleBlock& block, const AcquisitionSettings& acquisition,
                                  std::span<const ChannelSettings, kChannelCount> channels,
                                  const TriggerSettings& trigger,
                                  CaptureLimit limit = CaptureLimit::kEvery,
                                  const CaptureSink& on_completed = {});

  /// Drops the capture in progress and restarts the trigger, unarmed and with no holdoff
  /// running; the next block starts a new capture. Returns whether a capture was in progress.
  bool discard() {
    const bool in_progress = building_.has_value();
    building_.reset();
    trigger_.reset();
    holdoff_end_ = 0;
    return in_progress;
  }

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
  /// Completes `capture`, the capture in progress: it becomes completed_.
  void complete(Capture& capture);

  /// The trigger since the last discard (created by the first block after it), and the first
  /// sample index its holdoff lets place a capture.
  std::optional<EdgeTrigger> trigger_;
  SampleIndex holdoff_end_ = 0;
  /// The capture being collected, and how many samples it holds. Its `time_zero_sample` and
  /// `untriggered` are set when it completes, from the placement state below.
  std::optional<Capture> building_;
  std::size_t filled_ = 0;
  /// The pre-trigger sample count of the capture being collected.
  std::size_t pre_trigger_ = 0;
  /// True once a trigger event has placed the capture being collected.
  bool placed_ = false;
  /// The capture complete() has just made, until push hands it on.
  std::shared_ptr<const Capture> completed_;
};

}  // namespace openscope
